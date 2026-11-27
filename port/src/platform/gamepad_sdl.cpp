// Game controllers through SDL3 (Linux and other non-Windows systems, docs/linux.md): the first connected gamepad,
// whatever SDL knows it as (Xbox, PlayStation, Switch, Steam Deck, Steam Controller, Steam Input's virtual pad), mapped
// onto the Xbox layout the engine's gamepad layer reads (platform/gamepad.h). SDL reads Sony and Valve pads itself
// (HIDAPI), so the Windows raw-HID paths have no counterpart here.
#include <SDL3/SDL.h>

#include <algorithm>

#include "platform/gamepad.h"

namespace regs::platform {

namespace {

SDL_Gamepad* pad = nullptr;

bool gamepadUp() {
  static int state = 0;
  if (state == 0) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    state = SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
  }
  return state > 0;
}

float axis(SDL_GamepadAxis a) {
  const float v = float(SDL_GetGamepadAxis(pad, a));
  return std::clamp(v / 32767.0f, -1.0f, 1.0f);
}

int layoutOf(SDL_Gamepad* g, int& kind) {
  const Uint16 vendor = SDL_GetGamepadVendor(g), product = SDL_GetGamepadProduct(g);
  if (vendor == 0x28DE && product == 0x1205) return kind = 4, kLayoutDeck;
  if (vendor == 0x28DE && (product == 0x1102 || product == 0x1142)) return kind = 5, kLayoutSteamController;
  switch (SDL_GetGamepadType(g)) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return kind = 2, kLayoutPlayStation;
    default: return kind = 1, kLayoutXbox;
  }
}

}  // namespace

bool readGamepad(PadState& out) {
  out = PadState{};
  if (!gamepadUp()) return false;
  SDL_UpdateGamepads();
  if (pad && !SDL_GamepadConnected(pad)) {
    SDL_CloseGamepad(pad);
    pad = nullptr;
  }
  if (!pad) {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    // (a Deck or Steam Controller that Steam already reads shows up only as Steam Input's pad: SDL leaves the real one
    // to Steam)
    for (int i = 0; i < n && !pad; ++i) pad = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    if (!pad) return false;
  }
  out.connected = true;
  out.layout = layoutOf(pad, out.kind);
  out.lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
  out.ly = -axis(SDL_GAMEPAD_AXIS_LEFTY);
  out.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX);
  out.ry = -axis(SDL_GAMEPAD_AXIS_RIGHTY);
  out.lt = std::max(0.0f, axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
  out.rt = std::max(0.0f, axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
  struct Map {
    SDL_GamepadButton b;
    uint32_t bit;
  };
  static const Map map[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, kPadA},          {SDL_GAMEPAD_BUTTON_EAST, kPadB},
      {SDL_GAMEPAD_BUTTON_WEST, kPadX},           {SDL_GAMEPAD_BUTTON_NORTH, kPadY},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadLB}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadRB},
      {SDL_GAMEPAD_BUTTON_BACK, kPadBack},        {SDL_GAMEPAD_BUTTON_START, kPadStart},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, kPadL3},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kPadR3},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, kPadUp},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDown},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadLeft},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadRight},
      {SDL_GAMEPAD_BUTTON_GUIDE, kPadGuide},      {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, kPadL4},
      {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, kPadR4}, {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, kPadL5},
      {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, kPadR5},
  };
  for (const Map& m : map)
    if (SDL_GetGamepadButton(pad, m.b)) out.buttons |= m.bit;
  // touchpads: the Steam Deck's two (left, right), a PlayStation pad's one (its click is the left pad's)
  const int pads = SDL_GetNumGamepadTouchpads(pad);
  for (int t = 0; t < std::min(pads, 2); ++t) {
    bool down = false;
    float x = 0, y = 0, pressure = 0;
    if (!SDL_GetGamepadTouchpadFinger(pad, t, 0, &down, &x, &y, &pressure) || !down) continue;
    const float px = x * 2 - 1, py = 1 - y * 2;
    if (t == 0) out.lpad = true, out.lpx = px, out.lpy = py;
    else out.rpad = true, out.rpx = px, out.rpy = py;
  }
  if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_TOUCHPAD)) out.buttons |= kPadLPadClick;
  return true;
}

// the Windows raw-input and Valve HID paths: SDL covers these pads
void gamepadAttach(void*) {}
bool gamepadRawInput(void*) { return false; }
bool parseSonyReport(uint32_t, const uint8_t*, uint32_t, PadState&) { return false; }
void setValveNative(bool) {}
bool readValvePad(PadState&) { return false; }
int valvePadPresent() {
  if (!pad) return -1;
  int kind = 0;
  const int layout = layoutOf(pad, kind);
  return layout == kLayoutDeck || layout == kLayoutSteamController ? layout : -1;
}
bool parseValveReport(uint32_t, const uint8_t*, size_t, const PadState&, PadState&) { return false; }

}  // namespace regs::platform
