#pragma once
#include <vector>

namespace regs::engine {

// Platform services the natives need. Windows implementation in platform_win32.cpp; other platforms will provide
// their own (SDL or native) without touching the natives.
struct DisplayMode {
  int width = 0, height = 0, bits = 32, freq = 60;
};

std::vector<DisplayMode> platformDisplayModes();
DisplayMode platformCurrentDisplayMode();

}  // namespace regs::engine
