#pragma once
// Game controllers (owner's request 2026-11-18, port/docs/controller.md): the state of the first connected pad, as raw
// values (no dead zones; the engine's gamepad layer applies them). Windows: XInput (xinput1_4 / xinput1_3 /
// xinput9_1_0, loaded at run time, so the game starts without it) for Xbox-compatible pads, which includes
// PlayStation and other pads under Steam Input; without an XInput pad, a DualShock 4 or DualSense connected directly
// (USB or Bluetooth) is read from its raw HID reports (the window's WM_INPUT), and a Steam Deck or Steam Controller
// with Steam not running from Valve's HID protocol (steam_hid_win32.cpp; its touchpads too).
#include <cstdint>

namespace regs::platform {

enum PadButton : uint32_t {
  kPadA = 0x1, kPadB = 0x2, kPadX = 0x4, kPadY = 0x8,  // PlayStation: cross, circle, square, triangle
  kPadLB = 0x10, kPadRB = 0x20,                         // L1, R1
  kPadBack = 0x40, kPadStart = 0x80,                    // View / Share / Create, Menu / Options
  kPadL3 = 0x100, kPadR3 = 0x200,                       // the stick clicks
  kPadUp = 0x400, kPadDown = 0x800, kPadLeft = 0x1000, kPadRight = 0x2000,
  kPadGuide = 0x4000,                                   // PS button, Steam button (XInput: through GetStateEx only)
  kPadLPadClick = 0x8000, kPadRPadClick = 0x10000,       // touchpad clicks (Steam Deck, Steam Controller)
  kPadL4 = 0x20000, kPadR4 = 0x40000,                   // Deck back grips (upper), Steam Controller grips
  kPadL5 = 0x80000, kPadR5 = 0x100000,                  // Deck back grips (lower)
};

// the button names and glyphs a pad shows (the engine maps every pad onto the Xbox layout above)
enum PadLayout : int { kLayoutXbox = 0, kLayoutPlayStation = 1, kLayoutDeck = 2, kLayoutSteamController = 3 };

struct PadState {
  bool connected = false;
  int kind = 0;                  // 1 XInput, 2 PlayStation (raw HID), 3 simulated (tests), 4 Steam Deck, 5 Steam
                                 // Controller (both Valve HID, without Steam)
  int layout = kLayoutXbox;      // PadLayout
  float lx = 0, ly = 0;          // left stick, -1..1, up positive
  float rx = 0, ry = 0;          // right stick
  float lt = 0, rt = 0;          // triggers, 0..1
  uint32_t buttons = 0;          // PadButton bits
  bool lpad = false, rpad = false;   // a finger on the left / right touchpad
  float lpx = 0, lpy = 0, rpx = 0, rpy = 0;  // where, -1..1, up positive
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

// ---- Valve pads (steam_hid_win32.cpp) ----
// Steam Deck (28DE:1205) and Steam Controller (28DE:1102 wired, 1142 wireless receiver) read from their vendor HID
// interface on a background thread, only while allowed (setValveNative) and Steam is not running (Steam owns the pads
// then and gives the game an XInput pad and a mouse through Steam Input); REGS_VALVE_HID=0 never, =1 also with Steam.
// Opening a pad turns its "lizard mode" (the firmware's own mouse and keys) off, as long as the port reads it;
// closing turns it back on.
void setValveNative(bool allowed);
bool readValvePad(PadState& out);
// a Valve pad is plugged in (whether or not it is read): kLayoutDeck / kLayoutSteamController, -1 none
int valvePadPresent();
// one Valve input report (with its report-id byte) → state; prev carries the Steam Controller's shared left
// stick / left pad between reports
bool parseValveReport(uint32_t productId, const uint8_t* data, size_t size, const PadState& prev, PadState& out);

}  // namespace regs::platform
