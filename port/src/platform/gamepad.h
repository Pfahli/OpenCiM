#pragma once
// Game controllers (owner's request 2026-11-18, port/docs/controller.md): the state of the first connected pad, as raw
// values (no dead zones; the engine's gamepad layer applies them). Windows: XInput (xinput1_4 / xinput1_3 /
// xinput9_1_0, loaded at run time, so the game starts without it) for Xbox-compatible pads, which includes
// PlayStation and other pads under Steam Input; without an XInput pad, a DualShock 4 or DualSense connected directly
// (USB or Bluetooth) is read from its raw HID reports (the window's WM_INPUT).
#include <cstdint>

namespace regs::platform {

enum PadButton : uint32_t {
  kPadA = 0x1, kPadB = 0x2, kPadX = 0x4, kPadY = 0x8,  // PlayStation: cross, circle, square, triangle
  kPadLB = 0x10, kPadRB = 0x20,                         // L1, R1
  kPadBack = 0x40, kPadStart = 0x80,                    // View / Share / Create, Menu / Options
  kPadL3 = 0x100, kPadR3 = 0x200,                       // the stick clicks
  kPadUp = 0x400, kPadDown = 0x800, kPadLeft = 0x1000, kPadRight = 0x2000,
  kPadGuide = 0x4000,                                   // PS button (HID pads only; XInput does not report it)
};

struct PadState {
  bool connected = false;
  int kind = 0;                  // 1 XInput, 2 PlayStation (raw HID), 3 simulated (tests)
  float lx = 0, ly = 0;          // left stick, -1..1, up positive
  float rx = 0, ry = 0;          // right stick
  float lt = 0, rt = 0;          // triggers, 0..1
  uint32_t buttons = 0;          // PadButton bits
};

// polls the pads (XInput slots that were empty are tried again once a second: hot-plug without the cost of polling
// absent pads every frame); false and a disconnected state when none is connected
bool readGamepad(PadState& out);

// raw input registration for PlayStation pads on the game window (no effect without one)
void gamepadAttach(void* hwnd);
// WM_INPUT: true when the report was a PlayStation pad's (its state is kept for readGamepad)
bool gamepadRawInput(void* lparam);
// a raw HID report of a Sony pad (vendor 0x054C, by product id) → state; false when the report is not one it knows
bool parseSonyReport(uint32_t productId, const uint8_t* data, uint32_t size, PadState& out);

}  // namespace regs::platform
