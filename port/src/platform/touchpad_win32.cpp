// Precision touchpad twist from raw HID input (touchpad_win32.h). A precision touchpad reports its contacts in link
// collections (one per finger slot): contact id (0x0D/0x51), tip switch (0x0D/0x42), X and Y (0x01/0x30, 0x31), and a
// contact count (0x0D/0x54) in the first report of a frame ("hybrid" devices spread a frame over several reports).
#include "platform/touchpad_win32.h"

#include <winternl.h>  // NTSTATUS for hidpi.h
#include <hidusage.h>
#include <hidpi.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>

namespace regs::platform {

namespace {

constexpr USAGE kDigitizer = 0x0D, kTouchPad = 0x05, kTip = 0x42, kContactId = 0x51, kContactCount = 0x54;
constexpr USAGE kDesktop = 0x01, kX = 0x30, kY = 0x31;
constexpr float kPi = 3.14159265f;
constexpr float kStart = 8.0f * kPi / 180.0f;  // a twist begins after 8 degrees (two-finger scrolls and pinches wobble)

struct Slot {
  USHORT link = 0;
  float sx = 1, sy = 1;  // logical units → physical (equal scale on both axes, so angles are true)
};

struct Device {
  std::vector<uint8_t> ppd;  // preparsed data
  std::vector<Slot> slots;
  USHORT countLink = 0;
  bool hasCount = false, ok = false;
};

struct Contact {
  float x = 0, y = 0;
  bool down = false;
};

}  // namespace

struct TouchpadTwist::Impl {
  std::map<HANDLE, Device> devices;
  std::map<ULONG, Contact> contacts;
  ULONG remaining = 0;
  ULONG idA = 0, idB = 0;
  bool pair = false, twisting = false;
  float lastAngle = 0, pending = 0;
  std::vector<uint8_t> buf;

  Device& device(HANDLE h) {
    auto it = devices.find(h);
    if (it != devices.end()) return it->second;
    Device& d = devices[h];
    UINT size = 0;
    if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, nullptr, &size) != 0 || size == 0) return d;
    d.ppd.resize(size);
    if (GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, d.ppd.data(), &size) == UINT(-1)) return d;
    auto ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(d.ppd.data());
    HIDP_CAPS caps{};
    if (HidP_GetCaps(ppd, &caps) != HIDP_STATUS_SUCCESS) return d;
    USHORT n = caps.NumberInputValueCaps;
    std::vector<HIDP_VALUE_CAPS> vc(n);
    if (n == 0 || HidP_GetValueCaps(HidP_Input, vc.data(), &n, ppd) != HIDP_STATUS_SUCCESS) return d;
    std::map<USHORT, Slot> slots;
    for (USHORT i = 0; i < n; ++i) {
      const HIDP_VALUE_CAPS& c = vc[i];
      USAGE u = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
      if (c.UsagePage == kDigitizer && u == kContactCount) {
        d.hasCount = true;
        d.countLink = c.LinkCollection;
      }
      if (c.UsagePage != kDesktop || (u != kX && u != kY)) continue;
      float logical = float(c.LogicalMax - c.LogicalMin), physical = float(c.PhysicalMax - c.PhysicalMin);
      float s = logical > 0 && physical > 0 ? physical / logical : 1.0f;
      Slot& sl = slots[c.LinkCollection];
      sl.link = c.LinkCollection;
      (u == kX ? sl.sx : sl.sy) = s;
    }
    for (auto& [link, sl] : slots) d.slots.push_back(sl);
    d.ok = !d.slots.empty();
    return d;
  }

  // the two fingers' angle (radians, counterclockwise positive: the pad's y points down)
  bool pairAngle(float& a) {
    const Contact* c[2] = {nullptr, nullptr};
    int k = 0;
    ULONG ids[2] = {0, 0};
    for (auto& [id, ct] : contacts)
      if (ct.down) {
        if (k < 2) {
          c[k] = &ct;
          ids[k] = id;
        }
        ++k;
      }
    if (k != 2) return false;
    if (!pair || ids[0] != idA || ids[1] != idB) {
      pair = false;
      idA = ids[0];
      idB = ids[1];
    }
    a = std::atan2(-(c[1]->y - c[0]->y), c[1]->x - c[0]->x);
    return true;
  }
};

void TouchpadTwist::attach(HWND hwnd) {
  if (!impl_) impl_ = new Impl;
  RAWINPUTDEVICE rid{kDigitizer, kTouchPad, 0, hwnd};
  RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

TouchpadTwist::~TouchpadTwist() { delete impl_; }

bool TouchpadTwist::input(LPARAM l, float& angle, bool& ended) {
  if (!impl_) return false;
  Impl& m = *impl_;
  UINT size = 0;
  if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || !size)
    return false;
  m.buf.resize(size);
  if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, m.buf.data(), &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
    return false;
  auto* ri = reinterpret_cast<RAWINPUT*>(m.buf.data());
  if (ri->header.dwType != RIM_TYPEHID) return false;
  Device& d = m.device(ri->header.hDevice);
  if (!d.ok) return false;
  auto ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(d.ppd.data());
  for (DWORD r = 0; r < ri->data.hid.dwCount; ++r) {
    auto rep = reinterpret_cast<PCHAR>(ri->data.hid.bRawData + size_t(r) * ri->data.hid.dwSizeHid);
    ULONG len = ri->data.hid.dwSizeHid;
    ULONG count = 0;
    if (d.hasCount && HidP_GetUsageValue(HidP_Input, kDigitizer, d.countLink, kContactCount, &count, ppd, rep, len) ==
                          HIDP_STATUS_SUCCESS &&
        count > 0)
      m.remaining = count;
    for (const Slot& s : d.slots) {
      if (d.hasCount && m.remaining == 0) break;
      ULONG id = 0, x = 0, y = 0;
      if (HidP_GetUsageValue(HidP_Input, kDigitizer, s.link, kContactId, &id, ppd, rep, len) != HIDP_STATUS_SUCCESS ||
          HidP_GetUsageValue(HidP_Input, kDesktop, s.link, kX, &x, ppd, rep, len) != HIDP_STATUS_SUCCESS ||
          HidP_GetUsageValue(HidP_Input, kDesktop, s.link, kY, &y, ppd, rep, len) != HIDP_STATUS_SUCCESS)
        continue;
      USAGE pressed[16];
      ULONG np = 16;
      bool tip = false;
      if (HidP_GetUsages(HidP_Input, kDigitizer, s.link, pressed, &np, ppd, rep, len) == HIDP_STATUS_SUCCESS)
        for (ULONG i = 0; i < np; ++i) tip = tip || pressed[i] == kTip;
      Contact& c = m.contacts[id];
      c.x = float(x) * s.sx;
      c.y = float(y) * s.sy;
      c.down = tip;
      if (!tip) m.contacts.erase(id);
      if (m.remaining) --m.remaining;
    }
  }
  float a = 0;
  if (!m.pairAngle(a)) {
    bool was = m.twisting;
    m.pair = m.twisting = false;
    m.pending = 0;
    if (!was) return false;
    angle = 0;
    ended = true;
    return true;
  }
  if (!m.pair) {  // a new pair of fingers: its starting angle
    m.pair = true;
    m.twisting = false;
    m.pending = 0;
    m.lastAngle = a;
    return false;
  }
  float da = a - m.lastAngle;
  while (da > kPi) da -= 2 * kPi;
  while (da < -kPi) da += 2 * kPi;
  m.lastAngle = a;
  if (!m.twisting) {
    m.pending += da;
    if (std::fabs(m.pending) < kStart) return false;
    m.twisting = true;
    da = m.pending;
  }
  angle = da;
  ended = false;
  return true;
}

}  // namespace regs::platform
