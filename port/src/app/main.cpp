// regs: runs Cities in Motion from an installed copy with the ReGS engine.
//
//   regs [<gameDir>] [--documents <dir>] [--windowed] [--frames <n>] [--screenshot <file.png>]
//   regs --download-game [<folder>]   (the player's own Cities in Motion with Valve's SteamCMD)
//
// Without <gameDir> the game folder comes from install.cfg next to regs.exe (written by the setup program), else from
// the search for the player's Cities in Motion (core/game_find.h: Steam libraries, GOG and retail entries). A folder
// without the game's archives, given or found, ends the start with a message naming what is missing.
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

#include "core/crash.h"
#include "core/env.h"
#include "core/game_find.h"
#include "core/steam_download.h"
#include "core/log.h"
#include "core/text.h"
#include "engine/display.h"
#include "engine/engine.h"
#include "engine/gui.h"
#include "engine/host.h"
#include "engine/input.h"
#include "engine/platform.h"
#include "engine/popout.h"
#include "engine/headtrack.h"
#include "engine/stereo.h"
#include "engine/tools.h"
#include "engine/user_folders.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif
#if defined(__ANDROID__)
// the game is SDL_main in libmain.so, which SDL's activity (GameActivity, docs/android.md) looks up and starts; not
// through SDL_main.h, whose #define main would also rename the engine's main()
namespace regs {
void androidLogToLogcat();  // platform/android_jni.cpp
void androidDebugEnv();
}
#define REGS_MAIN regsMain
#else
#define REGS_MAIN main
#endif

#if defined(_WIN32)
void profilerFrame(long long frame);  // tools/profiler_win32.cpp: REGS_PROFILE=<first frame>:<frames>:<report file>
#else
inline void profilerFrame(long long) {}  // (no sampling profiler outside Windows)
#endif
namespace regs {
void crashTest(const char* kind);  // tools/crash_win32.cpp, crash_posix.cpp: REGS_CRASH_TEST=<kind>[:<frame>]
void crashThreadHandlers();        // the same: std::terminate's handler for this thread
}
#if defined(__APPLE__)
#include <mach-o/dyld.h>
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
  return regs::pathFromUtf16(w);
}

// the folder of this executable (data/ and install.cfg sit next to it); on Linux the real file, also when started
// through a link (~/.local/bin/opencim, install.sh) or from the PATH, where argv[0] holds only the name
fs::path exeDir(const char* argv0) {
#if defined(__linux__)
  std::error_code ec;
  fs::path self = fs::read_symlink("/proc/self/exe", ec);
  if (!ec && !self.empty()) return self.parent_path();
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) == 0) {
    std::error_code ec;
    fs::path self = fs::canonical(fs::path(buf), ec);
    if (!ec) return self.parent_path();
  }
#endif
  return fs::absolute(widen(argv0)).parent_path();
}

// the port's own data: next to the executable; in the Mac app bundle in Contents/Resources (the signed bundle keeps
// code in Contents/MacOS and everything else beside it)
fs::path dataDir(const char* argv0) {
  const fs::path exe = exeDir(argv0);
#if defined(__APPLE__)
  std::error_code ec;
  if (fs::is_directory(exe.parent_path() / "Resources" / "data", ec)) return exe.parent_path() / "Resources" / "data";
#endif
  return exe / "data";
}

// install.cfg (the game folder the setup or --download-game chose): next to the executable; on the Mac in
// ~/Library/Application Support/OpenCiM, since writing into the signed app bundle would break its signature
fs::path installCfg(const char* argv0) {
#if defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home && *home)
    return fs::path(home) / "Library" / "Application Support" / "OpenCiM" / "install.cfg";
#endif
  return exeDir(argv0) / "install.cfg";
}

int usage() {
  std::fprintf(stderr,
               "usage: regs [<gameDir>] [--documents <dir>] [--windowed] [--frames <n>] [--screenshot <file.png>]\n"
               "       regs --download-game [<folder>]   your own Cities in Motion with Valve's SteamCMD (log in there)\n"
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

int REGS_MAIN(int argc, char** argv) {
#if defined(__ANDROID__)
  androidLogToLogcat();
  androidDebugEnv();
#endif
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  crashThreadHandlers();
#if defined(_WIN32)
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // real pixels, no OS scaling
#endif
  // the game folder: the first argument, else install.cfg next to regs.exe, else the search (core/game_find.h)
  // --download-game [<folder>]: the player's own Cities in Motion with Valve's SteamCMD, in this console
  // (core/steam_download.h); then started without a folder, OpenCiM finds it there
  if (argc >= 2 && std::string(argv[1]) == "--download-game") {
    const fs::path dir = argc >= 3 ? fs::absolute(widen(argv[2])) : steamDownloadDefaultFolder();
    const bool ok = runSteamDownloadHere(dir);
    std::printf("%s\n", ok ? "Cities in Motion is ready. Start OpenCiM without a folder: it finds the game there."
                           : "Cities in Motion was not downloaded.");
    if (ok && argc >= 3) {  // another folder than the default: remembered in install.cfg next to regs
      std::error_code mk;
      fs::create_directories(installCfg(argv[0]).parent_path(), mk);
      if (!writeInstallCfg(installCfg(argv[0]), dir)) std::printf("(install.cfg could not be written: start regs with the folder)\n");
    }
    return ok ? 0 : 1;
  }
  const bool gameGiven = argc >= 2 && argv[1][0] != '-';
  if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) return usage();
  fs::path game = gameGiven ? gameDataFolder(widen(argv[1])) : fs::path(), documents, screenshot;
  // no game: the reason on stderr and (Windows) in a message box; REGS_NO_DIALOGS=1 (test runs) leaves the box out
  auto noGame = [](const std::string& why) {
    std::fprintf(stderr, "%s\n", why.c_str());
#if defined(_WIN32)
    if (!regs::envVar("REGS_NO_DIALOGS")) {
      const std::u16string w = utf8ToUtf16(why);
      MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(w.c_str()), L"OpenCiM", MB_OK | MB_ICONINFORMATION);
    }
#else
    // started from the desktop there is no console: the same as a message box (no box without a display)
    if (!regs::envVar("REGS_NO_DIALOGS")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "OpenCiM", why.c_str(), nullptr);
#endif
    return 1;
  };
  std::string gameSource = "argument";
  if (!gameGiven) {
    fs::path cfg = readInstallCfgGame(installCfg(argv[0]));
    if (!cfg.empty()) cfg = gameDataFolder(cfg);
    if (!cfg.empty() && checkGameFolder(cfg).ok) game = cfg, gameSource = "install.cfg";
    else game = findGame(&gameSource);
    if (game.empty()) return noGame(gameNotFoundMessage());
  } else if (GameCheck check = checkGameFolder(game); !check.ok) {
    return noGame(gameFolderMessage(game, check));
  }
  std::printf("%s[info] game folder: %s (%s)\n", logStamp().c_str(), utf16ToUtf8(std::u16string(game.u16string())).c_str(),
              gameSource.c_str());
  crashContext("game folder", utf16ToUtf8(std::u16string(game.u16string())) + " (" + gameSource + ")");
  crashSetDialog(true);  // the game shows where its crash report went (tools/crash_win32.cpp)
  bool printPaths = false;
  // the port's own data (menu pages, strings) next to the executable; --data overrides
  fs::path data = dataDir(argv[0]);
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
#if defined(__ANDROID__)
  standIn = false;  // the app gives its own storage as --documents: the player's real folder (with the log)
#endif
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
  // crash reports next to the player's saves: Documents\OpenCiM\crashes (core/crash.h, docs/crash-reports.md)
  crashSetReportDir(documents / engine::kPortFolderName / "crashes");
  // the port's log of a player's run (not a test's stand-in, whose output the test captures): Documents\OpenCiM\
  // opencim.log, the run before as opencim.old.log (2026-11-24: a started game's log went nowhere)
  if (!standIn) {
    std::error_code lec;
    fs::path dir = documents / engine::kPortFolderName;
    fs::create_directories(dir, lec);
    fs::rename(dir / "opencim.log", dir / "opencim.old.log", lec);
    logSetFile((dir / "opencim.log").string());
  }
  engine::host().headless = false;
  std::error_code ec;
  if (fs::is_directory(data, ec)) engine::host().dataDir = data;
  engine::Engine eng(game, documents);
  eng.echo = [](const std::string& s) {
    std::printf("%s[script] %s\n", logStamp().c_str(), s.c_str());
    crashLogLine("[script] " + s);
  };
  eng.errors = [](const std::string& s) {
    std::printf("[error] %s\n", s.c_str());
    crashLogLine("[error] " + s);
  };
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
  // REGS_CRASH_TEST=<kind>[:<frame>] (tests): crashes on purpose at that frame (default 60) to check the report
  std::string crashKind;
  long long crashFrame = 60;
  if (const char* ct = std::getenv("REGS_CRASH_TEST")) {
    crashKind = ct;
    if (size_t c = crashKind.find(':'); c != std::string::npos) {
      crashFrame = std::atoll(crashKind.c_str() + c + 1);
      crashKind.resize(c);
    }
  }
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
        } else if (sc.action == "script") {
          // script:<vfs path>: runs a script file into MAIN (tests: a tool chosen as its button would, e.g.
          // /userdata/test.script from the documents stand-in's OpenCiM folder)
          try {
            auto& vm = eng.vm();
            script::LoadOptions opt;
            if (script::Value* main = vm.findGlobal(u"MAIN"); main && main->object()) opt.into = script::Ref<script::Object>(main->object());
            vm.load(utf8ToUtf16(sc.arg), opt);
            LOG_INFO("test: script %s run", sc.arg.c_str());
          } catch (const std::exception& ex) {
            LOG_ERROR("test: script %s: %s", sc.arg.c_str(), ex.what());
          }
          continue;
        } else if (sc.action == "setting") {
          // setting:<name>,<number>: MAIN.$settings.$<name> while the game runs, as the options pages set it (tests of
          // live setting changes, e.g. setting:regsRtAo,1)
          try {
            auto& vm = eng.vm();
            size_t comma = sc.arg.find(',');
            if (script::Value* main = vm.findGlobal(u"MAIN"); main && comma != std::string::npos) {
              script::Value st = vm.getMember(*main, u"$settings");
              std::u16string name = u"$" + utf8ToUtf16(sc.arg.substr(0, comma));
              vm.setMember(st, name, script::Value::integer(std::atoi(sc.arg.c_str() + comma + 1)));
              LOG_INFO("test: setting %s = %s", sc.arg.substr(0, comma).c_str(), sc.arg.c_str() + comma + 1);
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
        } else if (sc.action == "head") {  // head:x,y: a head offset in cm as if OpenTrack had sent it (headtrack.h)
          float hx = 0, hy = 0;
          std::sscanf(sc.arg.c_str(), "%f,%f", &hx, &hy);
          engine::headTrackTest(hx, hy);
          continue;
        } else if (sc.action == "stereo") {  // stereo:<mode>: stereo 3D in that mode from now on (engine/stereo.h)
          engine::stereoTestMode(std::atoi(sc.arg.c_str()));
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
    if (!crashKind.empty() && frame == crashFrame) crashTest(crashKind.c_str());
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

#if defined(__ANDROID__)
extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char* argv[]) { return regsMain(argc, argv); }
#endif
