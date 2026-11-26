// Glasses-free 3D on Leia SR displays (engine/leiasr.h, docs/stereo-3d.md "Leia SR").
//
// The SR Platform runtime (installed with the monitor: C:\Program Files\LeiaSR\Platform) does the weaving: its weaver
// takes the two views side by side and writes the lenticular picture for the window, using the eye tracker's
// prediction of where the viewer's eyes will be when the frame is seen. OpenCiM loads the runtime's DLLs at run time and
// calls the few entry points it needs; nothing of Leia's is compiled in or shipped. The interfaces below are declared
// with the runtime's virtual method order (that order is the binary layout the runtime's objects have), and the
// factory functions are looked up by their exported names. Thanks to the owner's OdySee project (Samsung Odyssey 3D
// research, runtime 1.36.4), where this calling sequence was worked out and tried on the monitor.
//
//   OpenGL:  SR::CreateGLWeaver(SRContext&, HWND, IGLWeaver1**) (SimulatedRealityOpenGL.dll) weaves a GL texture into
//            the bound framebuffer, the window's.
//   Vulkan:  the runtime has no Vulkan weaver. A Direct3D 11 device on the same GPU (by LUID) holds two shared textures
//            that Vulkan imports (gpu::Device::importD3D11Texture): Vulkan composes the views into the first and waits,
//            SR::CreateDX11Weaver's weaver (SimulatedRealityDirectX.dll) weaves it into the second, D3D11 flushes and
//            waits, and Vulkan copies the second into its back buffer.
//   Both:    SR::SRContext::create / initialize / deleteSRContext (SimulatedRealityCore.dll); the lens request through
//            the runtime's C functions (newSRContext, createSwitchableLensHint, lensEnableHint, SimulatedRealityDisplays
//            .dll) so the panel switches to 3D while the mode runs and back when it ends.
#include "engine/leiasr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <typeindex>

#include "core/env.h"
#include "core/log.h"
#include "engine/display.h"
#include "engine/stereo.h"
#include "platform/window.h"
#include "render/gpu.h"
#include "render/renderer3d.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#endif

namespace regs::engine {

#ifdef _WIN32

namespace {

// ---- the runtime's interfaces (virtual bases and method order as the runtime has them) ----
namespace sr {
struct IDestroyable {
  virtual void destroy() = 0;
};
struct IQueryInterface {
 protected:
  virtual void* queryInterface(std::type_index type) = 0;
};
struct IWeaverBase1 : virtual IQueryInterface {
  virtual void setWindowHandle(HWND handle) = 0;
  virtual void weave() = 0;
  virtual void enableLateLatching(bool enable) = 0;
  virtual bool isLateLatchingEnabled() const = 0;
  virtual void setShaderSRGBConversion(bool read, bool write) = 0;
  virtual void setLatencyInFrames(uint64_t latencyInFrames) = 0;
  virtual void setLatency(uint64_t latency) = 0;
  virtual uint64_t getLatency() const = 0;
  virtual void getPredictedEyePositions(float left[3], float right[3]) = 0;

 protected:
  virtual ~IWeaverBase1() = default;
};
struct IGLWeaver1 : virtual IDestroyable, virtual IQueryInterface, virtual IWeaverBase1 {
  virtual void setInputViewTexture(unsigned texture, int width, int height, unsigned format) = 0;

 protected:
  virtual ~IGLWeaver1() = 0;
};
inline IGLWeaver1::~IGLWeaver1() {}
struct IDX11Weaver1 : virtual IDestroyable, virtual IQueryInterface, virtual IWeaverBase1 {
  virtual void setInputViewTexture(ID3D11ShaderResourceView* texture, int width, int height, DXGI_FORMAT format) = 0;
  virtual void setContext(ID3D11DeviceContext* context) = 0;

 protected:
  virtual ~IDX11Weaver1() = 0;
};
inline IDX11Weaver1::~IDX11Weaver1() {}

using CreateContextFn = void* (*)(int networkMode);  // static SRContext* SRContext::create(NetworkMode)
using ContextMemberFn = void (*)(void* self);        // void SRContext::initialize() (x64: this in the first register)
using DeleteContextFn = void (*)(void* context);     // static void SRContext::deleteSRContext(SRContext*)
using CreateGLWeaverFn = int (*)(void* context, HWND, IGLWeaver1**);  // the SRContext& is passed as a pointer
using CreateDX11WeaverFn = int (*)(void* context, ID3D11DeviceContext*, HWND, IDX11Weaver1**);
// the C functions (lens request)
using NewCFn = void* (*)();
using PtrFn = void (*)(void*);
using PtrRetFn = void* (*)(void*);
constexpr int kNonBlockingClientMode = 5;  // NetworkMode: fails at once when SRService does not answer
constexpr unsigned kGlRgba8 = 0x8058;
}  // namespace sr

// ---- detection: SR panels by their EDID manufacturer and product id (the runtime's own product table, as read in
// OdySee's notes, docs/re/06-sr-runtime-and-weaver.md §2.2) ----
struct Panel {
  const char* mfg;
  uint16_t lo, hi;
  const char* name;
};
constexpr Panel kPanels[] = {
    {"SAM", 0xE067, 0xE072, "Samsung Odyssey 3D"}, {"SAM", 0x7859, 0x7866, "Samsung Odyssey 3D"},
    {"ACR", 0x0000, 0x0003, "Acer SpatialLabs"},   {"LEN", 0x62CB, 0x62CB, "Lenovo 3D"},
    {"LEN", 0xE499, 0xE499, "Lenovo 3D"},          {"LEN", 0x61E6, 0x61E6, "Lenovo 3D"},
    {"LEN", 0x4146, 0x4146, "Lenovo ThinkVision 3D"}, {"DEL", 0x4147, 0x4147, "Dell 3D"},
    {"BNQ", 0x7FA6, 0x7FA7, "BenQ 3D"},            {"VSC", 0x36C3, 0x36C3, "ViewSonic 3D"},
    {"GSM", 0x7706, 0x7707, "LG 3D"},              {"AUS", 0x28E0, 0x28E0, "ASUS 3D"},
    {"AUS", 0x2830, 0x2830, "ASUS 3D"},            {"AUO", 0xB194, 0xB194, "SR laptop panel"},
    {"AUO", 0x39A4, 0x39A4, "SR laptop panel"},    {"SDC", 0x415D, 0x415D, "SR laptop panel"},
    {"SDC", 0x4178, 0x4178, "SR laptop panel"},    {"RTK", 0x3840, 0x3840, "SR display"},
};

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[MAX_PATH] = {};
  DWORD size = sizeof buf;
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return buf;
}

bool regDword(HKEY root, const wchar_t* key, const wchar_t* value, DWORD& out) {
  DWORD size = sizeof out;
  return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}

std::string narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s += c < 128 ? char(c) : '?';
  return s;
}

LeiaSrInfo detect() {
  LeiaSrInfo info;
  const wchar_t* kKey = L"SOFTWARE\\Dimenco\\Simulated Reality";
  std::wstring root = regString(HKEY_LOCAL_MACHINE, kKey, nullptr);
  if (root.empty()) root = L"C:\\Program Files\\LeiaSR\\Platform";
  info.bin = root + L"\\bin";
  auto exists = [&](const wchar_t* f) { return GetFileAttributesW((info.bin + L"\\" + f).c_str()) != INVALID_FILE_ATTRIBUTES; };
  info.runtime = exists(L"SimulatedRealityCore.dll") && exists(L"SimulatedRealityOpenGL.dll") &&
                 exists(L"SimulatedRealityDirectX.dll");
  // the monitors: MONITOR\<PNP id><product, 4 hex digits>\... (from the EDID)
  for (DWORD a = 0;; ++a) {
    DISPLAY_DEVICEW ad{sizeof ad};
    if (!EnumDisplayDevicesW(nullptr, a, &ad, 0)) break;
    for (DWORD m = 0;; ++m) {
      DISPLAY_DEVICEW md{sizeof md};
      if (!EnumDisplayDevicesW(ad.DeviceName, m, &md, 0)) break;
      if (!(md.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
      const std::string id = narrow(md.DeviceID);  // "MONITOR\SAM7860\{...}\0001"
      if (id.size() < 15 || id.compare(0, 8, "MONITOR\\") != 0) continue;
      const std::string mfg = id.substr(8, 3);
      const uint16_t product = uint16_t(std::strtoul(id.substr(11, 4).c_str(), nullptr, 16));
      for (const Panel& p : kPanels)
        if (mfg == p.mfg && product >= p.lo && product <= p.hi && !info.display) {
          info.display = true;
          info.monitor = std::string(p.name) + " (" + id.substr(8, 7) + ")";
        }
    }
  }
  // the runtime's own way to add a panel it does not list
  DWORD mid = 0, pid = 0;
  if (!info.display && regDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Dimenco\\Simulated Reality\\Manual", L"EDIDManufacturerId", mid) &&
      regDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Dimenco\\Simulated Reality\\Manual", L"EDIDProductId", pid)) {
    info.display = true;
    info.monitor = "SR display (added by hand in the SR Platform)";
  }
  if (const char* e = regs::envVar("REGS_LEIASR")) {
    if (std::atoi(e) == 0) info.runtime = false;
    else if (!info.display) info.display = true, info.monitor = "claimed by REGS_LEIASR=1";
  }
  info.usable = info.runtime && info.display;
  return info;
}

// ---- the running weaver ----
struct SrState {
  bool active = false, failed = false;
  std::string why;
  HMODULE core = nullptr, gl = nullptr, dx = nullptr, displays = nullptr;
  sr::CreateContextFn createContext = nullptr;
  sr::ContextMemberFn initialize = nullptr;
  sr::DeleteContextFn deleteContext = nullptr;
  sr::CreateGLWeaverFn createGLWeaver = nullptr;
  sr::CreateDX11WeaverFn createDX11Weaver = nullptr;
  void* context = nullptr;
  sr::IGLWeaver1* glWeaver = nullptr;
  sr::IDX11Weaver1* dxWeaver = nullptr;
  sr::IWeaverBase1* weaver = nullptr;  // whichever runs
  // the lens request (C functions, its own context)
  void* lensContext = nullptr;
  void* lensHint = nullptr;
  sr::PtrFn lensDisable = nullptr, deleteLensContext = nullptr;
  sr::PtrRetFn deleteLensHint = nullptr;
  // OpenGL: the views side by side
  render::RenderTarget glViews;
  // Vulkan: Direct3D 11 and the shared textures
  ID3D11Device* d3d = nullptr;
  ID3D11DeviceContext* d3dc = nullptr;
  ID3D11Query* done = nullptr;
  ID3D11Texture2D* inTex = nullptr;
  ID3D11Texture2D* outTex = nullptr;
  ID3D11ShaderResourceView* inView = nullptr;
  ID3D11RenderTargetView* outView = nullptr;
  HANDLE inHandle = nullptr, outHandle = nullptr;
  uint32_t vkIn = 0, vkOut = 0, fbIn = 0, fbOut = 0;
  int w = 0, h = 0;
  bool loggedFirst = false;
};

SrState& state() {
  static SrState s;
  return s;
}

template <class T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

bool fail(SrState& s, const std::string& why) {
  if (!s.failed) LOG_WARN("Leia SR: %s; glasses-free 3D stays off", why.c_str());
  s.failed = true;
  s.why = why;
  return false;
}

bool loadRuntime(SrState& s, const LeiaSrInfo& info) {
  if (s.core) return true;
  // the runtime's folder holds an older Visual C++ runtime: the system's is already loaded with OpenCiM itself and so
  // is the one the runtime's DLLs get; their other dependencies come from their own folder
  AddDllDirectory(info.bin.c_str());
  const DWORD flags = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
  auto load = [&](const wchar_t* f) { return LoadLibraryExW((info.bin + L"\\" + f).c_str(), nullptr, flags); };
  s.core = load(L"SimulatedRealityCore.dll");
  s.displays = load(L"SimulatedRealityDisplays.dll");
  if (!s.core) return fail(s, "SimulatedRealityCore.dll could not be loaded (error " + std::to_string(GetLastError()) + ")");
  s.createContext = reinterpret_cast<sr::CreateContextFn>(GetProcAddress(s.core, "?create@SRContext@SR@@SAPEAV12@W4NetworkMode@12@@Z"));
  s.initialize = reinterpret_cast<sr::ContextMemberFn>(GetProcAddress(s.core, "?initialize@SRContext@SR@@QEAAXXZ"));
  s.deleteContext = reinterpret_cast<sr::DeleteContextFn>(GetProcAddress(s.core, "?deleteSRContext@SRContext@SR@@SAXPEAV12@@Z"));
  if (!s.createContext || !s.initialize || !s.deleteContext) return fail(s, "the SR runtime's context functions are missing");
  return true;
}

// the panel to 3D while the mode runs (cooperative: the runtime collects every program's request)
void lensOn(SrState& s) {
  if (s.lensHint || !s.core || !s.displays) return;
  auto newCtx = reinterpret_cast<sr::NewCFn>(GetProcAddress(s.core, "newSRContext"));
  auto init = reinterpret_cast<sr::PtrFn>(GetProcAddress(s.core, "initializeSRContext"));
  s.deleteLensContext = reinterpret_cast<sr::PtrFn>(GetProcAddress(s.core, "deleteSRContext"));
  auto createHint = reinterpret_cast<sr::PtrRetFn>(GetProcAddress(s.displays, "createSwitchableLensHint"));
  s.deleteLensHint = reinterpret_cast<sr::PtrRetFn>(GetProcAddress(s.displays, "deleteSwitchableLensHint"));
  auto enable = reinterpret_cast<sr::PtrFn>(GetProcAddress(s.displays, "lensEnableHint"));
  s.lensDisable = reinterpret_cast<sr::PtrFn>(GetProcAddress(s.displays, "lensDisableHint"));
  if (!newCtx || !init || !s.deleteLensContext || !createHint || !s.deleteLensHint || !enable || !s.lensDisable) return;
  try {
    s.lensContext = newCtx();
    if (!s.lensContext) return;
    s.lensHint = createHint(s.lensContext);  // the hint before the context is initialised (as the SR Hub does)
    init(s.lensContext);
    if (s.lensHint) enable(s.lensHint);
  } catch (...) {
    s.lensHint = nullptr;
  }
}

void lensOff(SrState& s) {
  try {
    if (s.lensHint) {
      s.lensDisable(s.lensHint);
      s.deleteLensHint(s.lensHint);
    }
    if (s.lensContext) s.deleteLensContext(s.lensContext);
  } catch (...) {
  }
  s.lensHint = s.lensContext = nullptr;
}

bool makeContext(SrState& s) {
  try {
    s.context = s.createContext(sr::kNonBlockingClientMode);  // throws when SRService does not answer
  } catch (...) {
    s.context = nullptr;
  }
  if (!s.context) return fail(s, "the SR service does not answer (is \"SR Service\" running?)");
  return true;
}

// Direct3D 11 on the Vulkan device's GPU, for the DX11 weaver
bool makeD3D(SrState& s) {
  uint8_t luid[8];
  IDXGIAdapter1* adapter = nullptr;
  if (render::gpu::dev().adapterLuid(luid)) {
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) {
      for (UINT i = 0; !adapter; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d)) && std::memcmp(&d.AdapterLuid, luid, 8) == 0) adapter = a;
        else a->Release();
      }
      f->Release();
    }
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &s.d3d, nullptr, &s.d3dc);
  if (adapter) adapter->Release();
  if (FAILED(hr)) return fail(s, "no Direct3D 11 device for the weaver");
  D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
  s.d3d->CreateQuery(&qd, &s.done);
  return true;
}

void freeTextures(Display& d, SrState& s) {
  render::gpu::Device& g = render::gpu::dev();
  if (s.fbIn) g.destroyFramebuffer(s.fbIn);
  if (s.fbOut) g.destroyFramebuffer(s.fbOut);
  if (s.vkIn) g.destroyTexture(s.vkIn);
  if (s.vkOut) g.destroyTexture(s.vkOut);
  s.fbIn = s.fbOut = s.vkIn = s.vkOut = 0;
  release(s.inView);
  release(s.outView);
  release(s.inTex);
  release(s.outTex);
  if (s.inHandle) CloseHandle(s.inHandle);
  if (s.outHandle) CloseHandle(s.outHandle);
  s.inHandle = s.outHandle = nullptr;
  if (s.glViews.fbo) d.r3d().destroyTarget(s.glViews);
  s.w = s.h = 0;
}

// the shared pair at the window's size: D3D11 makes them, Vulkan imports them
bool makeShared(SrState& s, int w, int h) {
  D3D11_TEXTURE2D_DESC td{};
  td.Width = UINT(w);
  td.Height = UINT(h);
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  auto make = [&](ID3D11Texture2D*& tex, HANDLE& handle, uint32_t& vk, uint32_t& fb) {
    if (FAILED(s.d3d->CreateTexture2D(&td, nullptr, &tex))) return false;
    IDXGIResource1* r = nullptr;
    if (FAILED(tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&r)))) return false;
    HRESULT hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
    r->Release();
    if (FAILED(hr)) return false;
    render::gpu::Device& g = render::gpu::dev();
    vk = g.importD3D11Texture(handle, w, h);
    fb = vk ? g.createFramebuffer(vk, 0) : 0;
    return fb != 0;
  };
  if (!make(s.inTex, s.inHandle, s.vkIn, s.fbIn) || !make(s.outTex, s.outHandle, s.vkOut, s.fbOut))
    return fail(s, "the textures shared between Vulkan and Direct3D 11 could not be made");
  s.d3d->CreateShaderResourceView(s.inTex, nullptr, &s.inView);
  s.d3d->CreateRenderTargetView(s.outTex, nullptr, &s.outView);
  if (!s.inView || !s.outView) return fail(s, "Direct3D 11 views on the shared textures failed");
  return true;
}

bool start(Display& d, SrState& s, int w, int h) {
  const LeiaSrInfo& info = leiaSrDetected();
  if (!info.runtime) return fail(s, "the SR Platform runtime is not installed");
  if (!loadRuntime(s, info)) return false;
  render::gpu::Device& g = render::gpu::dev();
  const bool vulkan = std::strcmp(g.name(), "Vulkan") == 0;
  HWND hwnd = static_cast<HWND>(d.window().nativeWindow());
  if (vulkan) {
    s.dx = LoadLibraryExW((info.bin + L"\\SimulatedRealityDirectX.dll").c_str(), nullptr,
                          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (s.dx)
      s.createDX11Weaver = reinterpret_cast<sr::CreateDX11WeaverFn>(GetProcAddress(s.dx,
          "?CreateDX11Weaver@SR@@YA?AW4WeaverErrorCode@@PEAVSRContext@1@PEAUID3D11DeviceContext@@PEAUHWND__@@PEAPEAVIDX11Weaver1@1@@Z"));
    if (!s.createDX11Weaver) return fail(s, "SimulatedRealityDirectX.dll has no CreateDX11Weaver");
    if (!makeD3D(s) || !makeShared(s, w, h)) return false;
  } else {
    s.gl = LoadLibraryExW((info.bin + L"\\SimulatedRealityOpenGL.dll").c_str(), nullptr,
                          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (s.gl)
      s.createGLWeaver = reinterpret_cast<sr::CreateGLWeaverFn>(GetProcAddress(s.gl,
          "?CreateGLWeaver@SR@@YA?AW4WeaverErrorCode@@AEAVSRContext@1@PEAUHWND__@@PEAPEAVIGLWeaver1@1@@Z"));
    if (!s.createGLWeaver) return fail(s, "SimulatedRealityOpenGL.dll has no CreateGLWeaver");
    if (!d.r3d().makeTarget(s.glViews, w, h, render::TargetFormat::Rgba8, false, true))
      return fail(s, "no target for the views");
  }
  s.w = w;
  s.h = h;
  if (!makeContext(s)) return false;
  lensOn(s);
  try {  // the weaver before the context is initialised (the order of the SDK's examples)
    int rc = vulkan ? s.createDX11Weaver(s.context, s.d3dc, hwnd, &s.dxWeaver) : s.createGLWeaver(s.context, hwnd, &s.glWeaver);
    if (rc != 0 || !(vulkan ? static_cast<void*>(s.dxWeaver) : static_cast<void*>(s.glWeaver)))
      return fail(s, "the SR runtime made no weaver (error " + std::to_string(rc) + "; is the SR display attached?)");
    s.weaver = vulkan ? static_cast<sr::IWeaverBase1*>(s.dxWeaver) : static_cast<sr::IWeaverBase1*>(s.glWeaver);
    s.initialize(s.context);
  } catch (...) {
    return fail(s, "the SR runtime failed while starting the weaver");
  }
  s.active = true;
  LOG_INFO("Leia SR: weaving with the %s weaver for %s, %dx%d", vulkan ? "Direct3D 11" : "OpenGL",
           info.monitor.c_str(), w, h);
  return true;
}

void stop(Display& d, SrState& s) {
  try {
    if (s.dxWeaver) s.dxWeaver->destroy();
    if (s.glWeaver) s.glWeaver->destroy();
  } catch (...) {
  }
  s.dxWeaver = nullptr;
  s.glWeaver = nullptr;
  s.weaver = nullptr;
  try {
    if (s.context) s.deleteContext(s.context);
  } catch (...) {
  }
  s.context = nullptr;
  lensOff(s);
  if (render::gpu::hasDevice()) freeTextures(d, s);
  release(s.done);
  release(s.d3dc);
  release(s.d3d);
  if (s.active) LOG_INFO("Leia SR: stopped");
  s.active = false;
}

}  // namespace

const LeiaSrInfo& leiaSrDetected() {
  static const LeiaSrInfo info = [] {
    LeiaSrInfo i = detect();
    if (i.display || i.runtime)
      LOG_INFO("Leia SR: runtime %s, display %s", i.runtime ? "found" : "missing", i.display ? i.monitor.c_str() : "none");
    return i;
  }();
  return info;
}

bool leiaSrPresent(Display& d, script::VM& vm, int w, int h) {
  SrState& s = state();
  if (s.failed) return false;
  if (s.active && (w != s.w || h != s.h)) {  // the window changed size: the targets anew (the weaver stays)
    freeTextures(d, s);
    const bool vulkan = s.dxWeaver != nullptr;
    if (vulkan ? !makeShared(s, w, h) : !d.r3d().makeTarget(s.glViews, w, h, render::TargetFormat::Rgba8, false, true)) {
      stop(d, s);
      return fail(s, "the views' targets could not be made at the new size");
    }
    s.w = w;
    s.h = h;
  }
  if (!s.active && !start(d, s, w, h)) {
    stop(d, s);
    s.failed = true;
    return false;
  }
  render::gpu::Device& g = render::gpu::dev();
  const int viewW = w / 2;
  try {
    if (s.dxWeaver) {
      // Vulkan: the views into the shared texture (rows as Direct3D has them), run, then the weave in D3D11
      if (!stereoCompose(d, vm, kStereoSideBySide, w, h, s.fbIn, true)) return false;
      g.finishGpu();
      s.d3dc->OMSetRenderTargets(1, &s.outView, nullptr);
      D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
      s.d3dc->RSSetViewports(1, &vp);
      s.dxWeaver->setInputViewTexture(s.inView, viewW, h, DXGI_FORMAT_B8G8R8A8_UNORM);
      s.dxWeaver->weave();
      s.d3dc->End(s.done);
      s.d3dc->Flush();
      const auto t0 = std::chrono::steady_clock::now();
      while (s.d3dc->GetData(s.done, nullptr, 0, 0) == S_FALSE) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(200)) break;
        std::this_thread::yield();
      }
      // the woven picture into the back buffer, rows turned back to the device's own order
      g.blit(s.fbOut, 0, 0, 0, w, h, 0, h, w, 0, true, false);
    } else {
      // OpenGL: the views into a texture, the weave straight into the window's framebuffer
      if (!stereoCompose(d, vm, kStereoSideBySide, w, h, s.glViews.fbo, false)) return false;
      g.bindFramebuffer(0);
      g.scissorTest(false);
      g.viewport(0, 0, w, h);
      s.glWeaver->setInputViewTexture(s.glViews.color, viewW, h, sr::kGlRgba8);
      s.glWeaver->weave();
      g.externalStateChanged();
    }
  } catch (...) {
    stop(d, s);
    return fail(s, "the SR runtime failed while weaving");
  }
  if (!s.loggedFirst) {
    float l[3] = {}, r[3] = {};
    s.weaver->getPredictedEyePositions(l, r);
    LOG_INFO("Leia SR: first frame woven (eyes at %.0f,%.0f,%.0f and %.0f,%.0f,%.0f mm)", l[0], l[1], l[2], r[0], r[1], r[2]);
    s.loggedFirst = true;
  }
  return true;
}

bool leiaSrFailed() { return state().failed; }

bool leiaSrHead(float& x, float& y, float& z) {
  SrState& s = state();
  if (!s.active || !s.weaver) return false;
  float l[3] = {}, r[3] = {};
  try {
    s.weaver->getPredictedEyePositions(l, r);
  } catch (...) {
    return false;
  }
  // no face: the runtime reports both eyes at 0 (or not a number)
  if (!(std::isfinite(l[2]) && std::isfinite(r[2])) || (l[2] == 0 && r[2] == 0)) return false;
  x = (l[0] + r[0]) * 0.05f;  // mm → cm, the middle
  y = (l[1] + r[1]) * 0.05f;
  z = (l[2] + r[2]) * 0.05f;
  return true;
}

void leiaSrStop(Display& d) {
  SrState& s = state();
  if (!s.active && !s.context && !s.lensHint && !s.d3d) return;
  stop(d, s);
  s.loggedFirst = false;
}

void leiaSrRetry() { state().failed = false; }

#else  // not Windows: the SR Platform runtime is Windows only

const LeiaSrInfo& leiaSrDetected() {
  static const LeiaSrInfo info;
  return info;
}
bool leiaSrPresent(Display&, script::VM&, int, int) { return false; }
bool leiaSrFailed() { return true; }
bool leiaSrHead(float&, float&, float&) { return false; }
void leiaSrStop(Display&) {}
void leiaSrRetry() {}

#endif

}  // namespace regs::engine
