// Game controllers on Windows (platform/gamepad.h): XInput loaded at run time, and PlayStation pads from raw HID.
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "platform/gamepad.h"

namespace regs::platform {

namespace {

// XInputGetState; ordinal 100 (XInputGetStateEx, xinput1_3 / 1_4) has the same layout and reports the Guide button
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

struct XInput {
  bool tried = false;
  GetStateFn getState = nullptr;
  ULONGLONG nextTry[XUSER_MAX_COUNT] = {};  // empty slots: polled again at this tick count
  bool present[XUSER_MAX_COUNT] = {};
} g_x;

void loadXInput() {
  if (g_x.tried) return;
  g_x.tried = true;
  for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
    HMODULE m = LoadLibraryW(dll);
    if (!m) continue;
    auto ex = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, reinterpret_cast<LPCSTR>(100))));
    auto plain = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
    g_x.getState = ex ? ex : plain;
    if (g_x.getState) return;
    FreeLibrary(m);
  }
}

float stick(SHORT v) { return std::max(-1.0f, float(v) / 32767.0f); }

bool readXInput(PadState& out) {
  loadXInput();
  if (!g_x.getState) return false;
  const ULONGLONG now = GetTickCount64();
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    if (!g_x.present[i] && now < g_x.nextTry[i]) continue;
    XINPUT_STATE st{};
    if (g_x.getState(i, &st) != ERROR_SUCCESS) {
      g_x.present[i] = false;
      g_x.nextTry[i] = now + 1000;  // an absent pad costs a lot to poll: once a second
      continue;
    }
    g_x.present[i] = true;
    const XINPUT_GAMEPAD& g = st.Gamepad;
    out.connected = true;
    out.kind = 1;
    out.lx = stick(g.sThumbLX);
    out.ly = stick(g.sThumbLY);
    out.rx = stick(g.sThumbRX);
    out.ry = stick(g.sThumbRY);
    out.lt = float(g.bLeftTrigger) / 255.0f;
    out.rt = float(g.bRightTrigger) / 255.0f;
    const WORD w = g.wButtons;
    uint32_t b = 0;
    if (w & XINPUT_GAMEPAD_A) b |= kPadA;
    if (w & XINPUT_GAMEPAD_B) b |= kPadB;
    if (w & XINPUT_GAMEPAD_X) b |= kPadX;
    if (w & XINPUT_GAMEPAD_Y) b |= kPadY;
    if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= kPadLB;
    if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= kPadRB;
    if (w & XINPUT_GAMEPAD_BACK) b |= kPadBack;
    if (w & XINPUT_GAMEPAD_START) b |= kPadStart;
    if (w & XINPUT_GAMEPAD_LEFT_THUMB) b |= kPadL3;
    if (w & XINPUT_GAMEPAD_RIGHT_THUMB) b |= kPadR3;
    if (w & XINPUT_GAMEPAD_DPAD_UP) b |= kPadUp;
    if (w & XINPUT_GAMEPAD_DPAD_DOWN) b |= kPadDown;
    if (w & XINPUT_GAMEPAD_DPAD_LEFT) b |= kPadLeft;
    if (w & XINPUT_GAMEPAD_DPAD_RIGHT) b |= kPadRight;
    if (w & 0x0400) b |= kPadGuide;  // XInputGetStateEx only
    out.buttons = b;
    return true;  // the first connected pad
  }
  return false;
}

// ---- PlayStation pads (raw HID) ----

constexpr DWORD kSony = 0x054C;
bool isDualSense(uint32_t pid) { return pid == 0x0CE6 || pid == 0x0DF2; }
bool isDualShock4(uint32_t pid) { return pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0; }

struct Sony {
  std::unordered_map<HANDLE, uint32_t> product;  // raw device → product id (0: not a Sony pad)
  PadState state;
  ULONGLONG lastReport = 0;
} g_sony;

float axis8(uint8_t v) { return std::clamp((float(v) - 127.5f) / 127.5f, -1.0f, 1.0f); }

uint32_t hatButtons(uint8_t hat) {
  switch (hat & 0x0F) {
    case 0: return kPadUp;
    case 1: return kPadUp | kPadRight;
    case 2: return kPadRight;
    case 3: return kPadDown | kPadRight;
    case 4: return kPadDown;
    case 5: return kPadDown | kPadLeft;
    case 6: return kPadLeft;
    case 7: return kPadUp | kPadLeft;
    default: return 0;
  }
}

// the face, shoulder and system buttons of both pads: b0 = hat | square 0x10, cross 0x20, circle 0x40, triangle 0x80;
// b1 = L1 0x01, R1 0x02, (L2 0x04, R2 0x08), share/create 0x10, options 0x20, L3 0x40, R3 0x80; b2 = PS 0x01,
// touchpad click 0x02
uint32_t sonyButtons(uint8_t b0, uint8_t b1, uint8_t b2) {
  uint32_t b = hatButtons(b0);
  if (b0 & 0x10) b |= kPadX;
  if (b0 & 0x20) b |= kPadA;
  if (b0 & 0x40) b |= kPadB;
  if (b0 & 0x80) b |= kPadY;
  if (b1 & 0x01) b |= kPadLB;
  if (b1 & 0x02) b |= kPadRB;
  if (b1 & 0x10) b |= kPadBack;
  if (b1 & 0x20) b |= kPadStart;
  if (b1 & 0x40) b |= kPadL3;
  if (b1 & 0x80) b |= kPadR3;
  if (b2 & 0x01) b |= kPadGuide;
  if (b2 & 0x02) b |= kPadBack;  // the touchpad click: the second "back" button
  return b;
}

}  // namespace

bool parseSonyReport(uint32_t pid, const uint8_t* d, uint32_t n, PadState& out) {
  if (n < 10) return false;
  // where the left stick's byte is, and which layout follows: DualShock 4 (sticks, buttons x3, L2, R2) or DualSense
  // (sticks, L2, R2, counter, buttons x3)
  size_t b = 0;
  bool ds4Layout = true;
  if (isDualShock4(pid)) {
    if (d[0] == 0x01) b = 1;             // USB, or Bluetooth before the full reports are asked for
    else if (d[0] == 0x11) b = 3;        // Bluetooth full report
    else return false;
  } else if (isDualSense(pid)) {
    if (d[0] == 0x01 && n >= 64) b = 1, ds4Layout = false;   // USB
    else if (d[0] == 0x01) b = 1;                             // Bluetooth simple report (DualShock 4 layout)
    else if (d[0] == 0x31) b = 2, ds4Layout = false;          // Bluetooth full report
    else return false;
  } else {
    return false;
  }
  if (b + 10 > n) return false;
  out.connected = true;
  out.kind = 2;
  out.lx = axis8(d[b]);
  out.ly = -axis8(d[b + 1]);
  out.rx = axis8(d[b + 2]);
  out.ry = -axis8(d[b + 3]);
  if (ds4Layout) {
    out.buttons = sonyButtons(d[b + 4], d[b + 5], d[b + 6]);
    out.lt = float(d[b + 7]) / 255.0f;
    out.rt = float(d[b + 8]) / 255.0f;
  } else {
    out.lt = float(d[b + 4]) / 255.0f;
    out.rt = float(d[b + 5]) / 255.0f;
    out.buttons = sonyButtons(d[b + 7], d[b + 8], d[b + 9]);
  }
  return true;
}

void gamepadAttach(void* hwnd) {
  RAWINPUTDEVICE rid[2] = {{0x01, 0x05, 0, static_cast<HWND>(hwnd)},   // gamepads
                           {0x01, 0x04, 0, static_cast<HWND>(hwnd)}};  // joysticks
  RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
}

bool gamepadRawInput(void* lparam) {
  HRAWINPUT h = reinterpret_cast<HRAWINPUT>(lparam);
  UINT size = 0;
  if (GetRawInputData(h, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size < sizeof(RAWINPUTHEADER))
    return false;
  static thread_local std::vector<uint8_t> buf;
  buf.resize(size);
  if (GetRawInputData(h, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return false;
  const auto* ri = reinterpret_cast<const RAWINPUT*>(buf.data());
  if (ri->header.dwType != RIM_TYPEHID) return false;
  auto it = g_sony.product.find(ri->header.hDevice);
  if (it == g_sony.product.end()) {
    RID_DEVICE_INFO info{};
    info.cbSize = sizeof info;
    UINT sz = sizeof info;
    uint32_t pid = 0;
    if (GetRawInputDeviceInfoW(ri->header.hDevice, RIDI_DEVICEINFO, &info, &sz) != UINT(-1) &&
        info.dwType == RIM_TYPEHID && info.hid.dwVendorId == kSony &&
        (isDualShock4(info.hid.dwProductId) || isDualSense(info.hid.dwProductId)))
      pid = info.hid.dwProductId;
    it = g_sony.product.emplace(ri->header.hDevice, pid).first;
  }
  if (!it->second) return false;
  const RAWHID& hid = ri->data.hid;
  for (DWORD i = 0; i < hid.dwCount; ++i) {
    PadState s;
    if (parseSonyReport(it->second, hid.bRawData + size_t(i) * hid.dwSizeHid, hid.dwSizeHid, s)) {
      g_sony.state = s;
      g_sony.lastReport = GetTickCount64();
    }
  }
  return true;
}

bool readGamepad(PadState& out) {
  out = PadState{};
  if (readXInput(out)) return true;
  out = PadState{};
  // a PlayStation pad reports many times a second while the window has the focus; none for a second: gone
  if (g_sony.lastReport && GetTickCount64() - g_sony.lastReport < 1000) {
    out = g_sony.state;
    return true;
  }
  return false;
}

}  // namespace regs::platform
