#pragma once
#include <filesystem>
#include <functional>
#include <vector>

#include "platform/window.h"

namespace regs::engine {

// Process-wide settings the natives need from the host application.
struct Host {
  // returned by getMyDocumentsFolder(): the real Documents folder, or the --documents stand-in in test runs
  std::filesystem::path documents;
  // set by init_userdata when the scripts ask for the original's <documents>\Cities In Motion (engine/user_folders.h):
  // the port's own user folder <documents>\OpenCiM, and the original's folder, which is only read (empty otherwise)
  std::filesystem::path userFolder, originalFolder;
  bool quit = false;                // set by quit()
  bool headless = true;             // no window or rendering (tools, tests)
  bool forceWindowed = false;       // ignore the settings' fullscreen flag
  std::filesystem::path dataDir;    // the port's overlay files (empty = none)
  bool communityAddons = false;     // mount community addons and loose install overrides (Vfs::setCommunityContent)
  std::vector<platform::InputEvent> input;  // OS events of this frame, pumped by the host before the tick
};

Host& host();

}  // namespace regs::engine
