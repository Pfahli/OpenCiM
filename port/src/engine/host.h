#pragma once
#include <filesystem>
#include <functional>
#include <vector>

#include "platform/window.h"

namespace regs::engine {

// Process-wide settings the natives need from the host application.
struct Host {
  std::filesystem::path documents;  // returned by getMyDocumentsFolder(); a sandbox folder in test runs
  bool quit = false;                // set by quit()
  bool headless = true;             // no window or rendering (tools, tests)
  bool forceWindowed = false;       // ignore the settings' fullscreen flag
  std::filesystem::path dataDir;    // the port's overlay files (empty = none)
  bool communityAddons = false;     // mount community addons and loose install overrides (Vfs::setCommunityContent)
  std::vector<platform::InputEvent> input;  // OS events of this frame, pumped by the host before the tick
};

Host& host();

}  // namespace regs::engine
