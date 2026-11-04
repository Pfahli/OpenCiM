// regs: runs Cities in Motion from an installed copy with the ReGS engine.
//
//   regs <gameDir> [--documents <dir>] [--windowed] [--frames <n>] [--screenshot <file.png>]
//
// The main loop follows gsMainLoop (FUN_10082da0): frame time clamped to [0.0001, 0.05] s, pump window messages,
// then one gsMain tick (script threads, then the module tree, whose window renders and presents).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/text.h"
#include "engine/display.h"
#include "engine/engine.h"
#include "engine/host.h"
#include "engine/input.h"

#if defined(_WIN32)
#include <windows.h>
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
               "usage: regs <gameDir> [--documents <dir>] [--windowed] [--frames <n>] [--screenshot <file.png>]\n"
               "  --documents   stand-in for the Documents folder: settings and saves go to <dir>\\Cities In Motion\n"
               "                (default .\\documents, so the real game's saves and settings are never touched)\n"
               "  --windowed    ignore the fullscreen setting\n"
               "  --community-addons  also load community addons and loose overrides in the install (off: game and\n"
               "                DLC archives only)\n"
               "  --frames      quit after n frames (tests)\n"
               "  --screenshot  save the last frame before quitting\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // real pixels, no OS scaling
#endif
  if (argc < 2) return usage();
  fs::path game = widen(argv[1]), documents = fs::current_path() / "documents", screenshot;
  // the port's own data (menu pages, strings) next to the executable; --data overrides
  fs::path data = fs::absolute(fs::path(widen(argv[0]))).parent_path() / "data";
  long long maxFrames = -1;
  std::vector<Scripted> scripted;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--documents" && i + 1 < argc) documents = widen(argv[++i]);
    else if (a == "--data" && i + 1 < argc) data = widen(argv[++i]);
    else if (a == "--windowed") engine::host().forceWindowed = true;
    else if (a == "--community-addons") engine::host().communityAddons = true;
    else if (a == "--frames" && i + 1 < argc) maxFrames = std::atoll(argv[++i]);
    else if (a == "--screenshot" && i + 1 < argc) screenshot = widen(argv[++i]);
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
  fs::create_directories(documents);
  engine::host().headless = false;
  std::error_code ec;
  if (fs::is_directory(data, ec)) engine::host().dataDir = data;
  engine::Engine eng(game, documents);
  eng.echo = [](const std::string& s) { std::printf("[script] %s\n", s.c_str()); };
  eng.errors = [](const std::string& s) { std::printf("[error] %s\n", s.c_str()); };
  if (!eng.init(u"metro/scripts/init.script")) {
    std::fprintf(stderr, "init.script failed\n");
    return 1;
  }
  using clock = std::chrono::steady_clock;
  auto last = clock::now();
  long long frame = 0;
  int mx = 0, my = 0;
  while (!engine::host().quit && (maxFrames < 0 || frame < maxFrames)) {
    auto now = clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0001f, 0.05f);
    last = now;
    auto& input = engine::host().input;
    input.clear();
    engine::Display* d = engine::display();
    if (d) {
      d->window().pump(input);
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
        } else if (sc.action == "text") {  // typed characters (edit controls)
          for (unsigned char ch : sc.arg) {
            platform::InputEvent c;
            c.type = T::Char;
            c.ch = ch;
            input.push_back(c);
          }
          continue;
        } else if (sc.action == "shot") {
          d->requestScreenshot(widen(sc.arg.c_str()));
          continue;
        }
        e.x = mx;
        e.y = my;
        input.push_back(e);
      }
    }
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
  if (std::getenv("REGS_REPORT_MISSING")) {  // natives the scripts called that the port does not implement yet
    std::vector<std::pair<int, std::string>> miss;
    for (auto& [k, n] : eng.vm().missing) miss.emplace_back(n, k);
    std::sort(miss.begin(), miss.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::printf("%zu distinct unimplemented natives called:\n", miss.size());
    for (auto& [n, k] : miss) std::printf("  %6d  %s\n", n, k.c_str());
  }
  return 0;
}
