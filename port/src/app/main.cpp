// regs: runs Cities in Motion from an installed copy with the ReGS engine.
//
//   regs [<gameDir>] [--documents <dir>] [--windowed] [--frames <n>] [--screenshot <file.png>]
//
// Without <gameDir> the game folder comes from install.cfg next to regs.exe (written by the setup program), else from
// the search for the player's Cities in Motion (core/game_find.h: Steam libraries, GOG and retail entries).
//
// The main loop follows gsMainLoop (FUN_10082da0): frame time clamped to [0.0001, 0.05] s, pump window messages,
// then one gsMain tick (script threads, then the module tree, whose window renders and presents).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/game_find.h"
#include "core/log.h"
#include "core/text.h"
#include "engine/display.h"
#include "engine/engine.h"
#include "engine/gui.h"
#include "engine/host.h"
#include "engine/input.h"
#include "engine/platform.h"
#include "engine/popout.h"
#include "engine/tools.h"
#include "engine/user_folders.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(_WIN32)
void profilerFrame(long long frame);  // tools/profiler_win32.cpp: REGS_PROFILE=<first frame>:<frames>:<report file>
#else
inline void profilerFrame(long long) {}
#endif

using namespace regs;
namespace fs = std::filesystem;

namespace {

// --at actions: synthetic OS events injected after the message pump of their frame
struct Scripted {
  long long frame = 0;
  std::string action, arg;
};

fs::path widen(const char* s) {
  auto w = utf8ToUtf16(s);
  return fs::path(std::wstring(w.begin(), w.end()));
}

int usage() {
  std::fprintf(stderr,
               "usage: regs [<gameDir>] [--documents <dir>] [--windowed] [--frames <n>] [--screenshot <file.png>]\n"
               "  --documents   stand-in for the Documents folder (tests): settings, saves and maps go to\n"
               "                <dir>\\OpenCiM, the original's saves and maps are read from <dir>\\Cities In Motion\n"
               "                (default: the real Documents folder, so Documents\\OpenCiM)\n"
               "  --print-paths print the resolved folders and quit (creates and reads nothing)\n"
               "  --windowed    ignore the fullscreen setting\n"
               "  --community-addons  also load community addons and loose overrides in the install (off: game and\n"
               "                DLC archives only)\n"
               "  --mods <dir>  user mods folder and its mods.json (tests; default <documents>\\OpenCiM\\mods)\n"
               "  --no-mods     mount no mod (safe mode)\n"
               "  --frames      quit after n frames (tests)\n"
               "  --screenshot  save the last frame before quitting\n"
               "  --osm <place> skip the menus: a new map in the editor with an OpenStreetMap import of the place (a\n"
               "                name to search, the first hit, or \"lat,lon\")\n"
               "  --osm-size <cells>  its size (default 512)\n"
               "  --osm-scale <f>     metres scale of the area (1 1.5 2 3 4 5 6 8 10 15; default 1)\n"
               "  --osm-name <name>   the map's file name (default osm-import)\n"
               "  --osm-options <list> what to import, of terrain,water,trees,streets,trams,buildings,satellite\n"
               "                (default all)\n"
               "  --osm-save    save the imported map under its name (userdata maps) when the import is done\n");
  return 2;
}

// a start flag as the environment variable the scripts read (main.script extension: REGS_START and friends)
void setStartEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // real pixels, no OS scaling
#endif
  // the game folder: the first argument, else install.cfg next to regs.exe, else the search (core/game_find.h)
  const bool gameGiven = argc >= 2 && argv[1][0] != '-';
  if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) return usage();
  fs::path game = gameGiven ? widen(argv[1]) : fs::path(), documents, screenshot;
  if (!gameGiven) {
    fs::path exeDir = fs::absolute(fs::path(widen(argv[0]))).parent_path();
    fs::path cfg = readInstallCfgGame(exeDir / "install.cfg");
    if (!cfg.empty() && checkGameFolder(cfg).ok) game = cfg;
    else game = findGame();
    if (game.empty()) {
#if defined(_WIN32)
      MessageBoxW(nullptr,
                  L"OpenCiM needs Cities in Motion, but it was not found on this PC.\n\nRun the OpenCiM setup "
                  L"again to choose the game folder, or start regs.exe with the folder as its first argument.",
                  L"OpenCiM", MB_OK | MB_ICONINFORMATION);
#endif
      return usage();
    }
  }
  bool printPaths = false;
  // the port's own data (menu pages, strings) next to the executable; --data overrides
  fs::path data = fs::absolute(fs::path(widen(argv[0]))).parent_path() / "data";
  long long maxFrames = -1;
  std::vector<Scripted> scripted;
  for (int i = gameGiven ? 2 : 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--documents" && i + 1 < argc) documents = widen(argv[++i]);
    else if (a == "--data" && i + 1 < argc) data = widen(argv[++i]);
    else if (a == "--print-paths") printPaths = true;
    else if (a == "--windowed") engine::host().forceWindowed = true;
    else if (a == "--size" && i + 1 < argc) {  // --size WxH: the window's client size (screenshots, e.g. 3840x2160)
      std::sscanf(argv[++i], "%dx%d", &engine::host().forceWidth, &engine::host().forceHeight);
      engine::host().forceWindowed = true;
    }
    else if (a == "--community-addons") engine::host().communityAddons = true;
    else if (a == "--mods" && i + 1 < argc) engine::host().modsDir = fs::absolute(widen(argv[++i]));
    else if (a == "--no-mods") engine::host().noMods = true;
    else if (a == "--frames" && i + 1 < argc) maxFrames = std::atoll(argv[++i]);
    else if (a == "--screenshot" && i + 1 < argc) screenshot = widen(argv[++i]);
    else if (a == "--osm" && i + 1 < argc) setStartEnv("REGS_START", std::string("osm:") + argv[++i]);
    else if (a == "--osm-size" && i + 1 < argc) setStartEnv("REGS_OSM_SIZE", argv[++i]);
    else if (a == "--osm-scale" && i + 1 < argc) setStartEnv("REGS_OSM_SCALE", argv[++i]);
    else if (a == "--osm-name" && i + 1 < argc) setStartEnv("REGS_OSM_NAME", argv[++i]);
    else if (a == "--osm-options" && i + 1 < argc) setStartEnv("REGS_OSM_OPTIONS", argv[++i]);
    else if (a == "--osm-save") setStartEnv("REGS_OSM_SAVE", "1");
    else if (a == "--at" && i + 1 < argc) {
      std::string v = argv[++i];
      size_t c1 = v.find(':'), c2 = v.find(':', c1 + 1);
      if (c1 == std::string::npos || c2 == std::string::npos) return usage();
      Scripted sc{std::atoll(v.substr(0, c1).c_str()), v.substr(c1 + 1, c2 - c1 - 1), v.substr(c2 + 1)};
      if (sc.action == "click") {
        scripted.push_back({sc.frame, "down", sc.arg});
        scripted.push_back({sc.frame + 1, "up", sc.arg});
      } else {
        scripted.push_back(sc);
      }
    } else return usage();
  }
  // the Documents folder: the real one (as the original resolves it), or the --documents stand-in; the port's user
  // folder is <documents>\OpenCiM, the original's <documents>\Cities In Motion is only read (engine/user_folders.h)
  bool standIn = !documents.empty();
  if (!standIn) documents = engine::platformDocumentsFolder();
  documents = fs::absolute(documents);
  if (printPaths) {
    std::error_code ec;
    auto line = [&](const char* what, const fs::path& p) {
      std::printf("%-28s %s%s\n", what, utf16ToUtf8(std::u16string(p.u16string())).c_str(),
                  fs::exists(p, ec) ? "" : "  (does not exist yet)");
    };
    line("game folder:", game);
    std::printf("documents root (%s)\n", standIn ? "--documents stand-in" : "real Documents folder");
    line("  documents:", documents);
    line("  OpenCiM user folder:", documents / engine::kPortFolderName);
    line("  original folder (read):", documents / engine::kOriginalFolderName);
    line("  user mods (+ mods.json):", engine::host().modsDir.empty() ? documents / engine::kPortFolderName / "mods"
                                                                     : engine::host().modsDir);
    line("  built-in mods:", data / "mods");
    return 0;
  }
  fs::create_directories(documents);
  engine::host().headless = false;
  std::error_code ec;
  if (fs::is_directory(data, ec)) engine::host().dataDir = data;
  engine::Engine eng(game, documents);
  eng.echo = [](const std::string& s) { std::printf("%s[script] %s\n", logStamp().c_str(), s.c_str()); };
  eng.errors = [](const std::string& s) { std::printf("[error] %s\n", s.c_str()); };
  if (!eng.init(u"metro/scripts/init.script")) {
    std::fprintf(stderr, "init.script failed\n");
    return 1;
  }
  using clock = std::chrono::steady_clock;
  auto last = clock::now();
  long long frame = 0;
  int mx = 0, my = 0;
  // REGS_FIXED_DT=<seconds> (tests): every frame advances by this step whatever it took, so two runs of a scripted
  // start show the same scene at the same frame (picture comparisons between builds)
  const char* fixedDtEnv = std::getenv("REGS_FIXED_DT");
  const float fixedDt = fixedDtEnv ? std::clamp(float(std::atof(fixedDtEnv)), 0.0f, 0.05f) : 0.0f;
  // REGS_TEST_MOUSE_SWEEP=<first frame>,<x0>,<y0>,<x1>,<y1>,<frames> (tests, with --at actions): from that frame on the
  // mouse moves every frame, back and forth between the two window points, <frames> frames each way (a tool preview
  // following the mouse, docs/performance-audit.md C2)
  struct {
    long long first = -1;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, frames = 1;
  } sweep;
  if (const char* sw = std::getenv("REGS_TEST_MOUSE_SWEEP"))
    if (std::sscanf(sw, "%lld,%d,%d,%d,%d,%d", &sweep.first, &sweep.x0, &sweep.y0, &sweep.x1, &sweep.y1, &sweep.frames) != 6)
      sweep.first = -1;
  sweep.frames = std::max(1, sweep.frames);
  while (!engine::host().quit && (maxFrames < 0 || frame < maxFrames)) {
    profilerFrame(frame);
    auto now = clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0001f, 0.05f);
    if (fixedDt > 0) dt = fixedDt;
    last = now;
    auto& input = engine::host().input;
    input.clear();
    engine::Display* d = engine::display();
    if (d) {
      d->window().pump(input);
      engine::popoutsPump(eng.vm(), input, !scripted.empty());  // the pop-out views' windows (engine/popout.cpp)
      if (!screenshot.empty() && frame + 1 == maxFrames) d->requestScreenshot(screenshot);
      using T = platform::InputEvent::Type;
      if (!scripted.empty()) {  // scripted runs: the window counts as focused and the real mouse and keys are ignored
        std::erase_if(input, [](const platform::InputEvent& e) { return e.type != T::Close && e.type != T::Resize; });
        if (frame == 0) {
          platform::InputEvent e;
          e.type = T::Activate;
          input.push_back(e);
        }
      }
      for (const auto& sc : scripted) {
        if (sc.frame != frame) continue;
        platform::InputEvent e;
        int button = sc.arg == "right" ? 1 : sc.arg == "middle" ? 2 : 0;
        if (sc.action == "move") {
          std::sscanf(sc.arg.c_str(), "%d,%d", &mx, &my);
          e.type = T::MouseMove;
        } else if (sc.action == "down" || sc.action == "up") {
          e.type = sc.action == "down" ? T::MouseDown : T::MouseUp;
          e.button = button;
        } else if (sc.action == "key") {
          e.type = T::KeyDown;
          e.key = std::atoi(sc.arg.c_str());
          input.push_back(e);
          e.type = T::KeyUp;
        } else if (sc.action == "keydown" || sc.action == "keyup") {  // hold a key over several frames
          e.type = sc.action == "keydown" ? T::KeyDown : T::KeyUp;
          e.key = std::atoi(sc.arg.c_str());
        } else if (sc.action == "wheel") {  // wheel:<notches> at the mouse position, positive away from the user
          e.type = T::MouseWheel;
          e.wheel = float(std::atof(sc.arg.c_str()));
        } else if (sc.action == "hwheel") {  // hwheel:<notches>, the sideways wheel (positive to the right)
          e.type = T::MouseHWheel;
          e.wheel = float(std::atof(sc.arg.c_str()));
        } else if (sc.action == "gesture") {
          // gesture:zoom,<factor> | turn,<radians> | pan,<dx>,<dy> | end: a two-finger gesture at the mouse position
          char kind[16] = {};
          float a = 0, b = 0;
          std::sscanf(sc.arg.c_str(), "%15[a-z],%f,%f", kind, &a, &b);
          e.type = T::Gesture;
          const std::string k = kind;
          e.gesture = k == "zoom" ? 1 : k == "turn" ? 2 : k == "pan" ? 3 : 4;
          e.value = a;
          e.dx = a;
          e.dy = b;
        } else if (sc.action == "text") {  // typed characters (edit controls)
          for (unsigned char ch : sc.arg) {
            platform::InputEvent c;
            c.type = T::Char;
            c.ch = ch;
            input.push_back(c);
          }
          continue;
        } else if (sc.action == "pad") {  // pad:<button|axis>,<value> | pad:off: the simulated controller (gamepad.cpp)
          engine::gamepadSimulate(sc.arg);
          continue;
        } else if (sc.action == "shot") {
          d->requestScreenshot(widen(sc.arg.c_str()));
          continue;
        } else if (sc.action == "popout") {  // popout: one more pop-out view (as F7)
          engine::popoutOpen(eng.vm());
          continue;
        } else if (sc.action == "popshot") {  // popshot:<file.png>: the first pop-out's next picture
          engine::popoutScreenshot(sc.arg);
          continue;
        } else if (sc.action == "popmsg") {  // popmsg:<msg>,<wparam>,<x>,<y>: a window message to the first pop-out
          engine::popoutTestMessage(sc.arg);
          continue;
        } else if (sc.action.rfind("pop", 0) == 0) {
          // popkey:<vk> | popkeydown:<vk> | popkeyup:<vk> | popwheel:<notches> | popmove:x,y | popdown:<button> |
          // popup:<button>: input of the first pop-out's window
          platform::InputEvent pe;
          const std::string a = sc.action.substr(3);
          static int px = 0, py = 0;
          pe.x = px, pe.y = py;
          if (a == "key" || a == "keydown") pe.type = T::KeyDown, pe.key = std::atoi(sc.arg.c_str());
          else if (a == "keyup") pe.type = T::KeyUp, pe.key = std::atoi(sc.arg.c_str());
          else if (a == "wheel") pe.type = T::MouseWheel, pe.wheel = float(std::atof(sc.arg.c_str()));
          else if (a == "move") std::sscanf(sc.arg.c_str(), "%d,%d", &px, &py), pe.type = T::MouseMove, pe.x = px, pe.y = py;
          else if (a == "down" || a == "up") pe.type = a == "down" ? T::MouseDown : T::MouseUp, pe.button = button;
          else continue;
          engine::popoutInject(pe);
          if (a == "key") pe.type = T::KeyUp, engine::popoutInject(pe);
          continue;
        } else if (sc.action == "cam") {  // cam:x,z puts the game camera over a world point (MAIN.$gameUI.setCameraPos)
          float cx = 0, cz = 0;
          std::sscanf(sc.arg.c_str(), "%f,%f", &cx, &cz);
          try {
            auto& vm = eng.vm();
            if (script::Value* main = vm.findGlobal(u"MAIN"))
              vm.call(vm.getMember(vm.getMember(*main, u"$gameUI"), u"setCameraPos"),
                      {script::Value::real(cx), script::Value::real(cz), script::Value::boolean(true)});
          } catch (...) {
          }
          continue;
        } else if (sc.action == "tilt" || sc.action == "zoom" || sc.action == "dir") {
          // tilt:<0..1.5>, zoom:<75..500>, dir:<n>: the game camera (MAIN.$gameUI.setCameraTilt / Zoom / Direction)
          try {
            auto& vm = eng.vm();
            if (script::Value* main = vm.findGlobal(u"MAIN")) {
              const char16_t* fn = sc.action == "tilt" ? u"setCameraTilt" : sc.action == "zoom" ? u"setCameraZoom" : u"setCameraDirection";
              script::Value arg = sc.action == "dir" ? script::Value::integer(std::atoi(sc.arg.c_str()))
                                                     : script::Value::real(float(std::atof(sc.arg.c_str())));
              script::Value ui = vm.getMember(*main, u"$gameUI");
              if (sc.action == "tilt") vm.setMember(ui, u"$screenShotMode", script::Value::boolean(true));  // ui.script: tilt only then
              vm.call(vm.getMember(ui, fn), {arg});
            }
          } catch (...) {
          }
          continue;
        } else if (sc.action == "road" || sc.action == "track" || sc.action == "tram") {
          // road:editMode,toolMode,x0,z0,x1,z1 lays a road drag between two blocks (cells) with the editor's road tool;
          // track:x0,z0,x1,z1 a straight double track, tram:x0,z0,x1,z1 a straight tram track (tests: on MAIN.$player,
          // the grid player); the log says whether it was laid
          int em = 0, tm = 0, x0 = 0, z0 = 0, x1 = 0, z1 = 0;
          if (sc.action == "road") std::sscanf(sc.arg.c_str(), "%d,%d,%d,%d,%d,%d", &em, &tm, &x0, &z0, &x1, &z1);
          else std::sscanf(sc.arg.c_str(), "%d,%d,%d,%d", &x0, &z0, &x1, &z1), em = sc.action == "track" ? 6 : 0x11, tm = sc.action == "track" ? 6 : 0;
          try {
            auto& vm = eng.vm();
            const char* result = "no MAIN";
            if (script::Value* main = vm.findGlobal(u"MAIN")) {
              result = "no grid player (MAIN.$player)";
              if (auto* p = dynamic_cast<engine::ControlObj*>(vm.resolve(vm.getMember(*main, u"$player")).object())) {
                result = "no grid";
                if (engine::GridObj* g = engine::toolGrid(vm, p))
                  result = (sc.action == "road" ? engine::toolLayRoad(vm, p, g, em, tm, x0, z0, x1, z1)
                                                : engine::toolLayLine(vm, p, g, em, tm, x0, z0, x1, z1))
                               ? "laid"
                               : "not laid (conflict or no piece)";
              }
            }
            LOG_INFO("%s %s: %s", sc.action.c_str(), sc.arg.c_str(), result);
          } catch (const std::exception& ex) {
            LOG_WARN("road %s: %s", sc.arg.c_str(), ex.what());
          }
          continue;
        } else if (sc.action == "ui") {  // ui:off draws the scene only (screenshots without the interface), ui:on back
          engine::host().hideGui = sc.arg == "off";
          continue;
        } else if (sc.action == "vsync") {  // vsync:off after a frame-timed load, for frame-time measurements
          d->window().setVsync(sc.arg == "on");
          continue;
        }
        e.x = mx;
        e.y = my;
        input.push_back(e);
      }
    }
    if (d && sweep.first >= 0 && frame >= sweep.first) {
      long long k = (frame - sweep.first) % (2 * sweep.frames);
      float f = float(k <= sweep.frames ? k : 2 * sweep.frames - k) / float(sweep.frames);
      mx = int(std::lround(float(sweep.x0) + float(sweep.x1 - sweep.x0) * f));
      my = int(std::lround(float(sweep.y0) + float(sweep.y1 - sweep.y0) * f));
      platform::InputEvent e;
      e.type = platform::InputEvent::Type::MouseMove;
      e.x = mx;
      e.y = my;
      engine::host().input.push_back(e);
    }
    if (d) engine::gamepadFrame(eng, dt, scripted.empty());  // the controller: cursor, buttons, camera
    engine::dispatchInput(eng);
    eng.frame(dt);
    ++frame;
    // MAIN.fpsLimit (init.script: $settings.$fpsLimit): frames per second, 0 = unlimited
    float limit = 0;
    try {
      limit = eng.vm().toFloat(eng.vm().getMember(script::Value::heap(script::VT::Object, eng.main()), u"fpsLimit"));
    } catch (...) {
    }
    if (limit > 0) std::this_thread::sleep_until(now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / limit)));
  }
  engine::popoutsShutdown();  // the pop-out views' windows go while the device, display and scripts are all still there
  if (std::getenv("REGS_REPORT_MISSING")) {  // natives the scripts called that the port does not implement yet
    std::vector<std::pair<int, std::string>> miss;
    for (auto& [k, n] : eng.vm().missing) miss.emplace_back(n, k);
    std::sort(miss.begin(), miss.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::printf("%zu distinct unimplemented natives called:\n", miss.size());
    for (auto& [n, k] : miss) std::printf("  %6d  %s\n", n, k.c_str());
  }
  regs::Vfs::waitForWrites();  // the editor's autosave writes in the background
  return 0;
}
