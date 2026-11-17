#pragma once
// Precision touchpad twist (owner's request 2026-11-17, smooth camera): Windows turns two-finger scrolling into wheel
// messages and a pinch into Ctrl + wheel, but has no message for turning two fingers on a touchpad. This reads the
// touchpad's contacts as raw HID input (usage page 0x0D, touch pad 0x05) and reports the twist of two fingers.
#include <windows.h>

namespace regs::platform {

class TouchpadTwist {
 public:
  // registers for the touchpads' raw input on this window (no effect without a precision touchpad)
  void attach(HWND hwnd);
  // WM_INPUT: true with the twist since the last report (radians, counterclockwise on the pad positive), or with
  // ended = true when a twist stopped (fingers lifted or not two any more)
  bool input(LPARAM l, float& angle, bool& ended);
  ~TouchpadTwist();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace regs::platform
