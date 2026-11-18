// Game controllers (owner's request 2026-11-18; port/docs/controller.md). The pad plays the game through the paths a
// mouse and a keyboard take, so every menu, panel and tool works unchanged and the mouse and keys keep working
// alongside:
//   * the left stick moves a virtual cursor: the OS-style MouseMove events of host().input, as the real mouse sends
//     them (the system cursor follows, so the mouse goes on from there); A presses the left mouse button, B the right
//     one (hold A to drag); pushed against the screen's edge the cursor pans the map; with LB held the left stick pans
//     the map and the cursor stays;
//   * the right stick turns (sideways) and tilts (up and down) the camera, the triggers zoom (LT out, RT in), R3 sets
//     the tilt back: amounts for the smooth camera (camera_input.cpp cameraPadInput → regsCameraInput → the gameUI
//     extension, regs/extend/metro/scripts/ui.script);
//   * Start is Esc (the pause menu, closing windows); the other buttons press the game's keys the scripts bind to them
//     (regsPadBind: the gameUI extension binds them from the player's controls.keys, so rebound keys follow), and the
//     scripts can ask for presses themselves (regsPadPressed: game speed on the d-pad).
// Settings (MAIN.$settings, the controls page's "Controller" category): $regsController (on), $regsControllerCursor
// and $regsControllerCamera (speed factors 0.25..2.5), $regsControllerInvertTilt.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <string>

#include "core/log.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/engine.h"
#include "engine/gui.h"
#include "engine/host.h"
#include "engine/input.h"
#include "engine/settings.h"
#include "platform/gamepad.h"
#include "platform/window.h"

namespace regs::engine {

using namespace regs::script;
using platform::PadState;

namespace {

struct ButtonName {
  const char* name;
  uint32_t bit;
};
const ButtonName kButtons[] = {
    {"a", platform::kPadA},         {"b", platform::kPadB},       {"x", platform::kPadX},
    {"y", platform::kPadY},         {"lb", platform::kPadLB},     {"rb", platform::kPadRB},
    {"back", platform::kPadBack},   {"start", platform::kPadStart}, {"l3", platform::kPadL3},
    {"r3", platform::kPadR3},       {"up", platform::kPadUp},     {"down", platform::kPadDown},
    {"left", platform::kPadLeft},   {"right", platform::kPadRight}, {"guide", platform::kPadGuide}};
constexpr int kButtonCount = 15;

int bitIndex(uint32_t bit) {
  for (int i = 0; i < 32; ++i)
    if (bit == (1u << i)) return i;
  return -1;
}

template <class S>
std::string ascii(const S& s) {
  std::string out;
  for (auto c : s) out += char(c < 0x80 ? c : u'?');
  return out;
}

uint32_t buttonBit(std::string n) {
  for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
  for (const auto& b : kButtons)
    if (n == b.name) return b.bit;
  return 0;
}

struct Gamepad {
  bool simOn = false;
  PadState sim;
  int kind = 0;               // the connected pad (platform::PadState::kind), 0 none
  uint32_t prev = 0;          // buttons held last frame
  uint32_t pressed = 0;       // pressed this frame (regsPadPressed)
  uint32_t held = 0;
  float cx = -1, cy = -1;     // the virtual cursor (client pixels, fractional)
  int outX = INT_MIN, outY = INT_MIN;  // the position last sent
  float fast = 0;             // seconds the cursor stick has been pushed fully (acceleration)
  bool mouse[2] = {};         // the left / right button is held by A / B
  int keyDown[kButtonCount] = {};      // the engine key a button holds down now
  int binding[kButtonCount] = {};      // the engine key a button presses (scripts' regsPadBind)
  int holdBinding[kButtonCount] = {};  // the engine key a button presses once held for kHoldTime ("<button>_hold")
  float holdTime[kButtonCount] = {};
} g_pad;

constexpr float kHoldTime = 0.5f;

using T = platform::InputEvent::Type;

void pushMouse(T type, int button, int x, int y) {
  platform::InputEvent e;
  e.type = type;
  e.button = button;
  e.x = x;
  e.y = y;
  host().input.push_back(e);
}

void pushKey(bool down, int key) {
  platform::InputEvent e;
  e.type = down ? T::KeyDown : T::KeyUp;
  e.engineKey = key;
  host().input.push_back(e);
}

// radial dead zone, rescaled so the stick starts from zero at its edge
void deadZone(float& x, float& y, float dz) {
  const float m = std::hypot(x, y);
  if (m <= dz) {
    x = y = 0;
    return;
  }
  const float s = std::min(1.0f, (m - dz) / (1.0f - dz)) / m;
  x *= s;
  y *= s;
}

// a response curve on the stick's length (fine control near the centre, full speed at the edge)
void curve(float& x, float& y, float power) {
  const float m = std::hypot(x, y);
  if (m <= 0) return;
  const float s = std::pow(m, power) / m;
  x *= s;
  y *= s;
}

float trigger(float t) { return t <= 0.12f ? 0.0f : std::min(1.0f, (t - 0.12f) / 0.88f); }

// everything the pad holds is let go (pad gone, switched off, window in the background)
void releaseAll(int x, int y) {
  for (int b = 0; b < 2; ++b)
    if (g_pad.mouse[b]) {
      pushMouse(T::MouseUp, b, x, y);
      g_pad.mouse[b] = false;
    }
  for (int& k : g_pad.keyDown)
    if (k) {
      pushKey(false, k);
      k = 0;
    }
  g_pad.prev = g_pad.held = g_pad.pressed = 0;
  g_pad.fast = 0;
  cameraPadInput({});
}

bool gameUiOpen(VM& vm) {
  Value* main = vm.findGlobal(u"MAIN");
  Object* m = main ? main->object() : nullptr;
  Value* g = m ? m->findVar(u"gameUI") : nullptr;
  return g && vm.resolve(*g).object() != nullptr;
}

}  // namespace

void gamepadSimulate(const std::string& arg) {
  if (arg == "off") {
    g_pad.simOn = false;
    return;
  }
  g_pad.simOn = true;
  g_pad.sim.connected = true;
  g_pad.sim.kind = 3;
  const size_t c = arg.find(',');
  std::string name = arg.substr(0, c);
  const float v = c == std::string::npos ? 1.0f : float(std::atof(arg.c_str() + c + 1));
  PadState& s = g_pad.sim;
  if (name == "lx") s.lx = v;
  else if (name == "ly") s.ly = v;
  else if (name == "rx") s.rx = v;
  else if (name == "ry") s.ry = v;
  else if (name == "lt") s.lt = v;
  else if (name == "rt") s.rt = v;
  else if (uint32_t bit = buttonBit(name)) s.buttons = v != 0 ? s.buttons | bit : s.buttons & ~bit;
}

void gamepadFrame(Engine& eng, float dt, bool warp) {
  VM& vm = eng.vm();
  Display* d = display();
  auto* w = d ? dynamic_cast<WindowObj*>(d->windowObject()) : nullptr;
  if (!d || !w) return;
  PadState s;
  if (g_pad.simOn) s = g_pad.sim;
  else platform::readGamepad(s);
  if (s.kind != g_pad.kind) {
    if (s.connected) LOG_INFO("controller: connected (%s)", s.kind == 1 ? "XInput" : s.kind == 2 ? "PlayStation" : "simulated");
    else LOG_INFO("controller: disconnected");
    g_pad.kind = s.connected ? s.kind : 0;
  }
  const int ww = std::max(1, d->window().width()), wh = std::max(1, d->window().height());
  // where the pointer is: the real mouse's latest event of this frame, else where it was left
  int mx = d->mouseKnown() ? d->mouseX() : ww / 2, my = d->mouseKnown() ? d->mouseY() : wh / 2;
  for (const auto& e : host().input)
    if (e.type == T::MouseMove || e.type == T::MouseDown || e.type == T::MouseUp) mx = e.x, my = e.y;
  const bool enabled = settingNumber(vm, u"regsController", 1.0f) != 0.0f;
  if (!s.connected || !enabled || !w->appActive) {
    if (g_pad.prev || g_pad.mouse[0] || g_pad.mouse[1]) releaseAll(mx, my);
    return;
  }
  if (!gameUiOpen(vm)) {  // the scripts' bindings are the game's keys: only while a game runs
    for (int& b : g_pad.binding) b = 0;
    for (int& b : g_pad.holdBinding) b = 0;
  }
  if (mx != g_pad.outX || my != g_pad.outY || g_pad.cx < 0) {  // the mouse moved (or first use): go on from there
    g_pad.cx = float(mx);
    g_pad.cy = float(my);
  }

  const float cursorSpeed = std::clamp(settingNumber(vm, u"regsControllerCursor", 1.0f), 0.25f, 2.5f);
  const float cameraSpeed = std::clamp(settingNumber(vm, u"regsControllerCamera", 1.0f), 0.25f, 2.5f);
  const bool invertTilt = settingNumber(vm, u"regsControllerInvertTilt", 0.0f) != 0.0f;
  float lx = s.lx, ly = s.ly, rx = s.rx, ry = s.ry;
  deadZone(lx, ly, 0.24f);  // XInput's own recommended dead zones (7849 / 8689 of 32767)
  deadZone(rx, ry, 0.27f);
  const float lt = trigger(s.lt), rt = trigger(s.rt);
  const uint32_t now = s.buttons;
  g_pad.pressed = now & ~g_pad.prev;
  const uint32_t released = g_pad.prev & ~now;
  g_pad.prev = g_pad.held = now;

  CameraPad cam;
  // left stick: the cursor, or with LB held the map
  float sx = lx, sy = -ly;  // screen right / down
  if (now & platform::kPadLB) {
    curve(sx, sy, 1.6f);
    cam.panX = sx * 1.1f * cameraSpeed * dt;
    cam.panY = sy * 1.1f * cameraSpeed * dt;
    g_pad.fast = 0;
  } else if (sx != 0 || sy != 0) {
    const float m = std::hypot(sx, sy);
    g_pad.fast = m > 0.9f ? g_pad.fast + dt : 0.0f;
    const float accel = 1.0f + 1.4f * std::clamp((g_pad.fast - 0.3f) / 0.6f, 0.0f, 1.0f);
    const float slow = overWorld(vm, w) ? 1.0f : 0.7f;  // finer over panels and menus
    float vx = sx, vy = sy;
    curve(vx, vy, 2.0f);
    const float speed = 0.8f * float(wh) * cursorSpeed * accel * slow * dt;
    // a margin off the edges (the game's own edge scrolling stays the mouse's); pushed further, the map pans
    const float margin = 12.0f;
    float nx = g_pad.cx + vx * speed, ny = g_pad.cy + vy * speed;
    const float cx = std::clamp(nx, margin, float(ww - 1) - margin), cy = std::clamp(ny, margin, float(wh - 1) - margin);
    float ex = 0, ey = 0;
    if ((nx < cx && sx < 0) || (nx > cx && sx > 0)) ex = sx;
    if ((ny < cy && sy < 0) || (ny > cy && sy > 0)) ey = sy;
    curve(ex, ey, 1.6f);
    cam.panX += ex * 1.1f * cameraSpeed * dt;
    cam.panY += ey * 1.1f * cameraSpeed * dt;
    g_pad.cx = cx;
    g_pad.cy = cy;
  } else {
    g_pad.fast = 0;
  }
  const int px = int(std::lround(g_pad.cx)), py = int(std::lround(g_pad.cy));
  if (px != g_pad.outX || py != g_pad.outY) {
    pushMouse(T::MouseMove, 0, px, py);
    if (warp && (px != mx || py != my)) d->window().warpCursor(px, py);
    g_pad.outX = px;
    g_pad.outY = py;
  }

  // A / B: the mouse buttons at the cursor
  const uint32_t mouseBits[2] = {platform::kPadA, platform::kPadB};
  for (int b = 0; b < 2; ++b) {
    if ((g_pad.pressed & mouseBits[b]) && !g_pad.mouse[b]) {
      pushMouse(T::MouseDown, b, px, py);
      g_pad.mouse[b] = true;
    } else if ((released & mouseBits[b]) && g_pad.mouse[b]) {
      pushMouse(T::MouseUp, b, px, py);
      g_pad.mouse[b] = false;
    }
  }
  // keys: Start is Esc, the others what the scripts bound
  for (int i = 0; i < kButtonCount; ++i) {
    const uint32_t bit = 1u << i;
    if (bit == platform::kPadA || bit == platform::kPadB) continue;
    if (g_pad.pressed & bit) {
      const int k = bit == platform::kPadStart ? kKeyEscape : g_pad.binding[i];
      if (k && !g_pad.keyDown[i]) {
        pushKey(true, k);
        g_pad.keyDown[i] = k;
      }
    } else if ((released & bit) && g_pad.keyDown[i]) {
      pushKey(false, g_pad.keyDown[i]);
      g_pad.keyDown[i] = 0;
    }
  }

  // held buttons: a key once (B held: leave the tool, while its press stays the right mouse button)
  for (int i = 0; i < kButtonCount; ++i) {
    if (!g_pad.holdBinding[i] || !(now & (1u << i))) {
      g_pad.holdTime[i] = 0;
      continue;
    }
    const float before = g_pad.holdTime[i];
    g_pad.holdTime[i] += dt;
    if (before < kHoldTime && g_pad.holdTime[i] >= kHoldTime) {
      pushKey(true, g_pad.holdBinding[i]);
      pushKey(false, g_pad.holdBinding[i]);
    }
  }

  // camera: right stick turns (sideways) and tilts (up / down), the triggers zoom, R3 sets the tilt back
  curve(rx, ry, 1.5f);
  cam.turn = -rx * 1.7f * cameraSpeed * dt;  // pushed right: as the rotate-right key
  cam.turning = rx != 0;
  cam.tilt = (invertTilt ? ry : -ry) * 0.9f * cameraSpeed * dt;  // pushed up: towards the horizon
  cam.zoom = (lt - rt) * 1.6f * cameraSpeed * dt;
  cam.tiltReset = (g_pad.pressed & platform::kPadR3) != 0;
  cameraPadInput(cam);
}

void registerGamepadNatives(VM& vm) {
  // regsPadBind(button, keyName): the button presses that key (a controls.keys name, "Undefined" / "" for none) while
  // a game runs; "<button>_hold" presses it once when the button has been held half a second (any button, also a and
  // b); regsPadBind() clears every binding. Buttons: a b x y lb rb back start l3 r3 up down left right guide (a, b and
  // start themselves are the mouse buttons and Esc and cannot be bound)
  implementGlobal(vm, u"regsPadBind", [](VM& v, Object*, int argc, const Value* a) {
    if (argc == 0) {
      for (int& b : g_pad.binding) b = 0;
      for (int& b : g_pad.holdBinding) b = 0;
      return Value();
    }
    Value n = v.resolve(a[0]);
    if (n.type() != VT::String) return Value();
    std::string name = ascii(n.str());
    const bool hold = name.size() > 5 && name.compare(name.size() - 5, 5, "_hold") == 0;
    if (hold) name.resize(name.size() - 5);
    const int i = bitIndex(buttonBit(name));
    if (i < 0 || i >= kButtonCount) return Value();
    int key = 0;
    if (argc > 1)
      if (Value k = v.resolve(a[1]); k.type() == VT::String)
        for (int c = 1; c < kKeyCount; ++c)
          if (k.str() == keyName(c)) key = c;
    (hold ? g_pad.holdBinding : g_pad.binding)[i] = key;
    return Value();
  }, 0, 2);
  // regsPadPressed(button): pressed in this frame; regsPadHeld(button): held now
  auto query = [](bool pressed) {
    return [pressed](VM& v, Object*, int, const Value* a) {
      Value n = v.resolve(a[0]);
      if (n.type() != VT::String) return Value::boolean(false);
      const uint32_t bit = buttonBit(ascii(n.str()));
      return Value::boolean(bit && ((pressed ? g_pad.pressed : g_pad.held) & bit));
    };
  };
  implementGlobal(vm, u"regsPadPressed", query(true), 1, 1);
  implementGlobal(vm, u"regsPadHeld", query(false), 1, 1);
  // regsPadStatus(): 0 no controller, 1 Xbox-compatible (XInput), 2 PlayStation, 3 simulated (tests)
  implementGlobal(vm, u"regsPadStatus", [](VM&, Object*, int, const Value*) { return Value::integer(g_pad.kind); }, 0, 0);
}

}  // namespace regs::engine
