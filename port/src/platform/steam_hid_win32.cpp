// Valve pads without Steam (platform/gamepad.h; port/docs/controller.md "Steam Deck and Steam Controller"): the Steam
// Deck's built-in controller and the Steam Controller speak Valve's own HID protocol on a vendor-defined interface
// (usage page 0xFF00). Reports start with the HID report-id byte (0: the interfaces have none), then the header
// { u16 version, u8 type, u8 length } and the payload: type 0x01 = Steam Controller state, 0x09 = Steam Deck state.
// Feature reports (65 bytes: report id, type, length, data) set the firmware up; 0x81 clears the "lizard mode" key
// mappings, 0x87 sets settings (u8 id + u16 value each; trackpad modes 7/8 = 7 "none" turns the pads' own mouse off),
// 0x85 + 0x8E put the defaults back. The Deck turns lizard mode on again by itself after a while without the host,
// so the settings are sent again every two seconds while the pad is read. All of this follows the protocol as SDL's
// HIDAPI drivers use it; nothing here was tried on a real pad yet.
#include <windows.h>
#include <setupapi.h>
#include <tlhelp32.h>
#include <winternl.h>  // NTSTATUS for hidpi.h
#include <hidsdi.h>
#include <hidpi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform/gamepad.h"

namespace regs::platform {

namespace {

constexpr uint16_t kValve = 0x28DE;
// feature report types (only these four are ever sent)
constexpr uint8_t kClearDigitalMappings = 0x81, kSetDefaultDigitalMappings = 0x85, kSetSettingsValues = 0x87,
                  kLoadDefaultSettings = 0x8E;
// settings
constexpr uint8_t kSettingLeftTrackpadMode = 7, kSettingRightTrackpadMode = 8, kSettingSmoothAbsoluteMouse = 24;
constexpr uint16_t kTrackpadNone = 7;

struct Known {
  uint16_t pid;
  int layout;
};
std::vector<Known> knownPads() {
  std::vector<Known> k = {{0x1205, kLayoutDeck}, {0x1102, kLayoutSteamController}, {0x1142, kLayoutSteamController}};
  // REGS_VALVE_PIDS=1302:deck,1303:sc adds product ids (newer pads that speak one of the two protocols)
  if (const char* e = std::getenv("REGS_VALVE_PIDS")) {
    std::string s = e;
    for (size_t at = 0; at < s.size();) {
      size_t end = s.find(',', at);
      std::string item = s.substr(at, end == std::string::npos ? std::string::npos : end - at);
      size_t c = item.find(':');
      uint16_t pid = uint16_t(std::strtoul(item.substr(0, c).c_str(), nullptr, 16));
      int layout = c != std::string::npos && item.substr(c + 1) == "sc" ? kLayoutSteamController : kLayoutDeck;
      if (pid) k.push_back({pid, layout});
      if (end == std::string::npos) break;
      at = end + 1;
    }
  }
  return k;
}

int16_t s16(const uint8_t* p) { return int16_t(uint16_t(p[0]) | uint16_t(p[1]) << 8); }
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
float axis16(int16_t v) { return std::max(-1.0f, float(v) / 32767.0f); }

int layoutOf(uint16_t pid) {
  for (const Known& k : knownPads())
    if (k.pid == pid) return k.layout;
  return -1;
}

bool steamRunning() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof pe;
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
    found = _wcsicmp(pe.szExeFile, L"steam.exe") == 0;
  CloseHandle(snap);
  return found;
}

struct Candidate {
  std::wstring path;
  uint16_t pid = 0;
  int layout = -1;
};

// the vendor interfaces of the Valve pads plugged in now
std::vector<Candidate> enumerate() {
  std::vector<Candidate> out;
  GUID hid;
  HidD_GetHidGuid(&hid);
  HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return out;
  SP_DEVICE_INTERFACE_DATA ifd{};
  ifd.cbSize = sizeof ifd;
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &ifd); ++i) {
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
    if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
    std::vector<uint8_t> buf(need);
    auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;
    std::wstring path = det->DevicePath;
    std::wstring lower = path;
    for (auto& c : lower) c = wchar_t(towlower(c));
    if (lower.find(L"vid_28de") == std::wstring::npos) continue;
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) continue;
    HIDD_ATTRIBUTES a{};
    a.Size = sizeof a;
    PHIDP_PREPARSED_DATA pp = nullptr;
    HIDP_CAPS caps{};
    bool vendor = false;
    if (HidD_GetAttributes(h, &a) && HidD_GetPreparsedData(h, &pp)) {
      vendor = HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage >= 0xFF00;
      HidD_FreePreparsedData(pp);
    }
    CloseHandle(h);
    const int layout = a.VendorID == kValve ? layoutOf(a.ProductID) : -1;
    if (vendor && layout >= 0) out.push_back({path, a.ProductID, layout});
  }
  SetupDiDestroyDeviceInfoList(set);
  return out;
}

struct Reader {
  std::mutex m;
  PadState state;
  ULONGLONG last = 0;  // tick of the last state report
  std::atomic<bool> allowed{false};
  std::atomic<int> present{-1};
  std::atomic<bool> quit{false};
  std::thread thread;
  bool started = false;

  ~Reader() {
    if (!started) return;
    quit = true;
    if (thread.joinable()) thread.join();
  }

  void start() {
    if (started) return;
    started = true;
    thread = std::thread([this] { run(); });
  }

  static bool feature(HANDLE h, size_t len, uint8_t type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buf(std::max<size_t>(len, 65), 0);
    buf[1] = type;
    buf[2] = uint8_t(data.size());
    std::copy(data.begin(), data.end(), buf.begin() + 3);
    return HidD_SetFeature(h, buf.data(), ULONG(buf.size())) != FALSE;
  }

  static void lizardOff(HANDLE h, size_t len) {
    feature(h, len, kClearDigitalMappings, {});
    std::vector<uint8_t> s;
    auto set = [&](uint8_t id, uint16_t v) { s.insert(s.end(), {id, uint8_t(v & 0xFF), uint8_t(v >> 8)}); };
    set(kSettingSmoothAbsoluteMouse, 0);
    set(kSettingLeftTrackpadMode, kTrackpadNone);
    set(kSettingRightTrackpadMode, kTrackpadNone);
    feature(h, len, kSetSettingsValues, s);
  }

  static void lizardOn(HANDLE h, size_t len) {
    feature(h, len, kSetDefaultDigitalMappings, {});
    feature(h, len, kLoadDefaultSettings, {});
  }

  void run() {
    const char* env = std::getenv("REGS_VALVE_HID");
    const bool never = env && env[0] == '0', always = env && env[0] == '1';
    ULONGLONG nextScan = 0;
    while (!quit) {
      const ULONGLONG now = GetTickCount64();
      if (now < nextScan) {
        Sleep(100);
        continue;
      }
      nextScan = now + 2000;
      std::vector<Candidate> pads = enumerate();
      present = pads.empty() ? -1 : pads.front().layout;
      if (never || pads.empty() || !allowed || (!always && steamRunning())) continue;
      read(pads.front(), always);
    }
  }

  // reads one pad until it goes away, reading is no longer allowed or Steam starts
  void read(const Candidate& c, bool always) {
    HANDLE h = CreateFileW(c.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    size_t inLen = 65, featLen = 65;
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (HidD_GetPreparsedData(h, &pp)) {
      HIDP_CAPS caps{};
      if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
        inLen = std::max<size_t>(caps.InputReportByteLength, 2);
        featLen = std::max<size_t>(caps.FeatureReportByteLength, 65);
      }
      HidD_FreePreparsedData(pp);
    }
    lizardOff(h, featLen);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<uint8_t> buf(inLen);
    PadState prev;
    ULONGLONG nextLizard = GetTickCount64() + 2000, nextSteamCheck = GetTickCount64() + 3000;
    bool pending = false;
    while (!quit && allowed) {
      const ULONGLONG now = GetTickCount64();
      if (now >= nextLizard) {
        lizardOff(h, featLen);
        nextLizard = now + 2000;
      }
      if (now >= nextSteamCheck) {
        nextSteamCheck = now + 3000;
        if (!always && steamRunning()) break;
      }
      if (!pending) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        if (ReadFile(h, buf.data(), DWORD(buf.size()), &got, &ov)) {
          handle(c, buf.data(), got, prev);
          continue;
        }
        if (GetLastError() != ERROR_IO_PENDING) break;  // unplugged
        pending = true;
      }
      if (WaitForSingleObject(ov.hEvent, 100) != WAIT_OBJECT_0) continue;
      DWORD got = 0;
      pending = false;
      if (!GetOverlappedResult(h, &ov, &got, FALSE)) break;
      handle(c, buf.data(), got, prev);
    }
    if (pending) {
      CancelIo(h);
      DWORD got = 0;
      GetOverlappedResult(h, &ov, &got, TRUE);
    }
    lizardOn(h, featLen);
    CloseHandle(ov.hEvent);
    CloseHandle(h);
    std::lock_guard<std::mutex> lock(m);
    last = 0;
  }

  void handle(const Candidate& c, const uint8_t* d, size_t n, PadState& prev) {
    PadState s;
    if (!parseValveReport(c.pid, d, n, prev, s)) return;
    prev = s;
    std::lock_guard<std::mutex> lock(m);
    state = s;
    last = GetTickCount64();
  }
};

Reader& reader() {
  static Reader r;
  r.start();
  return r;
}

// Steam Controller / Steam Deck button bits (the first 32 bits of the state's button word; the Deck has more above)
constexpr uint32_t kVR2 = 0x1, kVL2 = 0x2, kVR1 = 0x4, kVL1 = 0x8, kVY = 0x10, kVB = 0x20, kVX = 0x40, kVA = 0x80,
                   kVUp = 0x100, kVRight = 0x200, kVLeft = 0x400, kVDown = 0x800, kVView = 0x1000, kVSteam = 0x2000,
                   kVMenu = 0x4000, kVL5 = 0x8000, kVR5 = 0x10000, kVLPadClick = 0x20000, kVRPadClick = 0x40000,
                   kVLPadTouch = 0x80000, kVRPadTouch = 0x100000, kVL3 = 0x400000, kVStickData = 0x800000,
                   kVR3 = 0x4000000;
// the Deck's upper button word
constexpr uint32_t kVHL4 = 0x200, kVHR4 = 0x400;

uint32_t commonButtons(uint32_t v) {
  uint32_t b = 0;
  if (v & kVA) b |= kPadA;
  if (v & kVB) b |= kPadB;
  if (v & kVX) b |= kPadX;
  if (v & kVY) b |= kPadY;
  if (v & kVL1) b |= kPadLB;
  if (v & kVR1) b |= kPadRB;
  if (v & kVView) b |= kPadBack;
  if (v & kVMenu) b |= kPadStart;
  if (v & kVSteam) b |= kPadGuide;
  if (v & kVUp) b |= kPadUp;
  if (v & kVDown) b |= kPadDown;
  if (v & kVLeft) b |= kPadLeft;
  if (v & kVRight) b |= kPadRight;
  if (v & kVL3) b |= kPadL3;
  if (v & kVLPadClick) b |= kPadLPadClick;
  if (v & kVRPadClick) b |= kPadRPadClick;
  return b;
}

}  // namespace

bool parseValveReport(uint32_t pid, const uint8_t* d, size_t n, const PadState& prev, PadState& out) {
  // report id, then the header
  if (n < 5) return false;
  const uint8_t type = d[3];
  const uint8_t* p = d + 5;  // payload
  const int layout = layoutOf(uint16_t(pid));
  if (type == 0x09 && n >= 5 + 56) {  // Steam Deck state
    const uint32_t lo = u32(p + 4), hi = u32(p + 8);
    out = PadState{};
    out.connected = true;
    out.kind = 4;
    out.layout = layout >= 0 ? layout : kLayoutDeck;
    uint32_t b = commonButtons(lo);
    if (lo & kVR3) b |= kPadR3;
    if (lo & kVL5) b |= kPadL5;
    if (lo & kVR5) b |= kPadR5;
    if (hi & kVHL4) b |= kPadL4;
    if (hi & kVHR4) b |= kPadR4;
    out.buttons = b;
    const uint16_t pressL = u16(p + 52), pressR = u16(p + 54);
    out.lpad = (lo & kVLPadTouch) || pressL > 0;
    out.rpad = (lo & kVRPadTouch) || pressR > 0;
    out.lpx = axis16(s16(p + 12));
    out.lpy = axis16(s16(p + 14));
    out.rpx = axis16(s16(p + 16));
    out.rpy = axis16(s16(p + 18));
    out.lt = std::clamp(float(u16(p + 40)) / 32767.0f, 0.0f, 1.0f);
    out.rt = std::clamp(float(u16(p + 42)) / 32767.0f, 0.0f, 1.0f);
    out.lx = axis16(s16(p + 44));
    out.ly = axis16(s16(p + 46));
    out.rx = axis16(s16(p + 48));
    out.ry = axis16(s16(p + 50));
    return true;
  }
  if (type == 0x01 && n >= 5 + 20) {  // Steam Controller state: one stick, which shares its fields with the left pad
    const uint32_t v = uint32_t(p[4]) | uint32_t(p[5]) << 8 | uint32_t(p[6]) << 16;
    out = prev;
    out.connected = true;
    out.kind = 5;
    out.layout = layout >= 0 ? layout : kLayoutSteamController;
    uint32_t b = commonButtons(v);
    if (v & kVL5) b |= kPadL4;  // the grips (back left / right)
    if (v & kVR5) b |= kPadR4;
    out.buttons = b;
    out.lt = float(p[7]) / 255.0f;
    out.rt = float(p[8]) / 255.0f;
    const float x = axis16(s16(p + 12)), y = axis16(s16(p + 14));
    out.lpad = (v & kVLPadTouch) != 0;
    if ((v & kVStickData) || !out.lpad) {  // these fields are the stick now
      out.lx = x;
      out.ly = y;
    } else {
      out.lpx = x;
      out.lpy = y;
    }
    if (!out.lpad) out.lpx = out.lpy = 0;
    out.rpad = (v & kVRPadTouch) != 0;
    out.rpx = out.rpad ? axis16(s16(p + 16)) : 0;
    out.rpy = out.rpad ? axis16(s16(p + 18)) : 0;
    out.rx = out.ry = 0;
    return true;
  }
  return false;
}

void setValveNative(bool allowed) { reader().allowed = allowed; }

bool readValvePad(PadState& out) {
  Reader& r = reader();
  std::lock_guard<std::mutex> lock(r.m);
  if (!r.last || GetTickCount64() - r.last > 1000) return false;
  out = r.state;
  return true;
}

int valvePadPresent() { return reader().present; }

}  // namespace regs::platform
