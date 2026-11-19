// gsWindow_OGL / gsVideo_OGL back end: the OS window, the per-frame update/render/present order of
// gsWindow_OGL::tick (0x10057160) and the GUI pass (docs/renderer/interfaces-2d-output.md §1.5).
#include "engine/display.h"
#include "core/env.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/effects.h"
#include "engine/font.h"
#include "engine/gui.h"
#include "engine/hdr.h"
#include "engine/temporal.h"
#include "engine/host.h"
#include "engine/input.h"
#include "engine/interaction.h"
#include "engine/popout.h"
#include "engine/scene3d.h"
#include "engine/minimap.h"
#include "engine/scene3d.h"
#include "engine/settings.h"
#include "engine/texture_upscale.h"
#include "engine/ui_graphs.h"
#include "engine/user_folders.h"
#include "render/gpu.h"
#include "render/glsl.h"
#include "render/image.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using render::GuiVertex;

namespace {

std::unique_ptr<Display> g_display;

Value field(VM& vm, Object* o, const char16_t* name) {
  try {
    Value v = vm.getMember(Value::heap(VT::Object, o), name);
    return v.ref() ? v.ref()->get() : v;
  } catch (const ScriptError&) {
    return Value();
  }
}

// Stored PNG (uncompressed deflate blocks): enough for screenshots without a compression library.
void writePng(std::ofstream& f, const uint8_t* rgba, int w, int h) {
  auto crcTable = [] {
    std::vector<uint32_t> t(256);
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  auto be32 = [](std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(uint8_t(v >> 24)); o.push_back(uint8_t(v >> 16)); o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v));
  };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> o;
    be32(o, uint32_t(data.size()));
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), data.begin(), data.end());
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 4; i < o.size(); ++i) c = crcTable[(c ^ o[i]) & 0xFF] ^ (c >> 8);
    be32(o, c ^ 0xFFFFFFFFu);
    f.write(reinterpret_cast<const char*>(o.data()), std::streamsize(o.size()));
  };
  f.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> ihdr;
  be32(ihdr, uint32_t(w));
  be32(ihdr, uint32_t(h));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  chunk("IHDR", ihdr);
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + size_t(y) * size_t(w) * 4, rgba + size_t(y + 1) * size_t(w) * 4);
  }
  std::vector<uint8_t> z{0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  for (size_t at = 0; at < raw.size(); at += 65535) {
    size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
  }
  be32(z, (b << 16) | a);
  chunk("IDAT", z);
  chunk("IEND", {});
}

// Glyph quads of one text run (gsGuiText_OGL::render's draw step) in the current projection.
void drawText(VM& vm, Display& d, Font& f, const std::u16string& s, const TextParams& p, float x, float y, uint32_t argb,
              bool inlineColours) {
  auto pageHeight = [&](int page) {
    auto* t = page >= 0 && size_t(page) < f.pages.size() ? dynamic_cast<TextureObj*>(f.pages[size_t(page)].get()) : nullptr;
    return t ? t->height : 1;
  };
  auto emit = [&](const TextQuad& q) {
    uint32_t tex = 0;
    if (q.page >= 0 && size_t(q.page) < f.pages.size())
      if (auto* t = dynamic_cast<TextureObj*>(f.pages[size_t(q.page)].get())) tex = d.texture(vm, t);
    uint32_t rgba = render::argbToRgba(q.argb);
    GuiVertex v[4];
    for (int k = 0; k < 4; ++k) v[k] = {q.x[k], q.y[k], q.u[k], q.v[k], rgba};
    d.r2d().draw(tex, render::Prim::Quads, v, 4);
  };
  layoutText(f, s, p, x, y, argb, 0, inlineColours, emit, pageHeight);
}

}  // namespace

Display* display() { return g_display.get(); }

void useProgramCache() {
  if (!render::gpu::hasDevice() || host().userFolder.empty() || regs::envVar("REGS_NO_PROGRAM_CACHE")) return;
  render::gpu::dev().setProgramCache(host().userFolder / L"cache" / L"gl");
}

void WindowObj::tick(VM& vm, float dt) {
  // timing (0x10057160.c:25-49): fps every >= 0.1 s, avgFps over the whole run, frameTime in ms
  ++frames;
  ++framesSinceFps;
  total += dt;
  sinceFps += dt;
  if (sinceFps >= 0.1) {
    fieldValues[u"fps"] = Value::real(float(double(framesSinceFps) / sinceFps));
    sinceFps = 0;
    framesSinceFps = 0;
  }
  fieldValues[u"avgFps"] = Value::real(total > 0 ? float(double(frames) / total) : 0.0f);
  fieldValues[u"frameTime"] = Value::real(dt * 1000.0f);
  Ref<Object> keep(this);
  pollMouse(vm, this);
  vm.tickChildren(this, dt);
  if (Display* d = display(); d && d->windowObject() == this) d->renderFrame(vm, this, fieldValues[u"video"].object());
}

bool Display::open(const platform::WindowDesc& desc, bool vulkan, std::string* error) {
  // Vulkan first when chosen; any failure there opens OpenGL in a new window (docs/renderer/vulkan.md §7)
  if (vulkan) {
    platform::WindowDesc vd = desc;
    vd.openGL = false;
    window_ = platform::createWindow();
    std::string why;
    std::unique_ptr<render::gpu::Device> device;
    // the shader and pipeline caches in the player's OpenCiM folder (follows --documents)
    std::filesystem::path cache;
    if (!host().userFolder.empty()) cache = host().userFolder / "cache";
    else if (!host().documents.empty()) cache = host().documents / kPortFolderName / "cache";
    if (window_->create(vd, &why)) device = render::gpu::createVulkanDevice(*window_, desc.vsync, &why, cache);
    if (device) {
      render::gpu::setDevice(std::move(device));
    } else {
      LOG_WARN("display: Vulkan is not available (%s), using OpenGL 3.3", why.c_str());
      window_.reset();
    }
  }
  if (!window_) {
    window_ = platform::createWindow();
    if (!window_->create(desc, error)) return false;
    auto device = render::gpu::createGlDevice(*window_, error);
    if (!device) return false;
    render::gpu::setDevice(std::move(device));
  }
  useProgramCache();
  if (!r2d_.init(error) || !r3d_.init(error)) return false;
  LOG_INFO("display: %dx%d, %s", window_->width(), window_->height(), r2d_.info.c_str());
  if (int dpi = window_->dpi()) LOG_INFO("display: %d dpi (%d %%), per-monitor DPI aware", dpi, dpi * 100 / 96);
  else LOG_WARN("display: not DPI aware, Windows may stretch the window");
  return true;
}

float Display::pickUiScale(VM& vm, int w, int h) const {
  // the graphics page's choices 1×, 1.5×, 2×; no setting: 2× on windows taller than 1800 px (4K-class screens)
  float want = settingNumber(vm, u"regsUiScale", h > 1800 ? 2.0f : 1.0f);
  float s = want >= 1.75f ? 2.0f : want >= 1.25f ? 1.5f : 1.0f;
  // the scripts' window stays at least 1024 × 720: the main menu's buttons run into its bottom links below about
  // 660 script pixels, and the graphics page is 1000 wide. Smaller scaled windows step down to the next scale
  while (s > 1.0f && (float(w) / s < 1024.0f || float(h) / s < 720.0f)) s -= 0.5f;
  return s;
}

float Display::chooseUiScale(VM& vm, int w, int h) {
  float s = pickUiScale(vm, w, h);
  float want = settingNumber(vm, u"regsUiScale", 0);
  if (want != 0 && s != want) LOG_WARN("ui scale %.1f does not fit a %dx%d window, using %.1f", double(want), w, h, double(s));
  if (const char* f = regs::envVar("REGS_UI_FILTER")) uiFilterSharp_ = std::string(f) != "linear";
  return uiScale = s;
}

int Display::applyDisplay(VM& vm, bool fullscreen, int w, int h, bool relayout) {
  // the graphics page's fullscreen, resolution and UI scale while the game runs (docs/ui/scaling.md §6): the same
  // window gets a new style and size, the GL context stays. Fullscreen is borderless over the monitor (the desktop's
  // resolution and refresh rate), so the resolution sizes the window when it is not fullscreen
  w = std::max(320, w);
  h = std::max(240, h);
  if (host().forceWidth > 0 && host().forceHeight > 0) {  // --size keeps its window
    fullscreen = false;
    w = host().forceWidth;
    h = host().forceHeight;
  }
  int rw = w, rh = h;
  if (fullscreen) window_->monitorSize(rw, rh);
  bool sameWindow = fullscreen == window_->fullscreen() && rw == window_->width() && rh == window_->height();
  float scale = pickUiScale(vm, rw, rh);
  if (sameWindow && scale == uiScale) return 0;
  Object* win = windowObj_.get();
  int oldSW = vm.toInt(field(vm, win, u"width")), oldSH = vm.toInt(field(vm, win, u"height"));
  int sw = int(std::lround(float(rw) / scale)), sh = int(std::lround(float(rh) / scale));
  if ((sw != oldSW || sh != oldSH) && !relayout) return 2;
  int oldW = window_->width(), oldH = window_->height();
  float oldScale = uiScale;
  if (!sameWindow) window_->setMode(fullscreen, w, h);
  rw = window_->width();  // a window larger than the screen may come out smaller than asked
  rh = window_->height();
  chooseUiScale(vm, rw, rh);
  if (uiScale != oldScale) dropGuiTargets();  // they are sized texture × scale
  if (rw != oldW || rh != oldH) resetSizedEffects(*this);
  sw = int(std::lround(float(rw) / uiScale));
  sh = int(std::lround(float(rh) / uiScale));
  LOG_INFO("display: %s %dx%d, ui scale %.1f (scripts' window %dx%d)", window_->fullscreen() ? "fullscreen" : "window", rw,
           rh, double(uiScale), sw, sh);
  if (sw == oldSW && sh == oldSH) return 1;
  win->fieldValues[u"width"] = Value::integer(sw);
  win->fieldValues[u"height"] = Value::integer(sh);
  relayoutGui(vm, field(vm, win, u"video").object(), oldSW, oldSH, sw, sh);
  return 2;
}

void Display::dropGuiTargets() {
  for (auto& [t, rt] : targets_) {
    if (t->gl == rt.color) t->gl = 0;
    r3d_.destroyTarget(rt);
  }
  targets_.clear();
}

namespace {
size_t imageBytes(const render::Image& img) {
  size_t n = 0;
  for (const auto& lv : img.levels) n += lv.size();
  return n;
}
}  // namespace

struct Display::DecodedTexture {
  std::string path;
  std::vector<uint8_t> data;  // kept for the upscale cache's check only
  render::Image img;
  bool read = false, parsed = false;
  std::string err;
};

void Display::uploadTextures(VM& vm, const std::vector<TextureObj*>& textures) {
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<TextureObj*> todo;
  std::set<TextureObj*> seen;
  for (TextureObj* t : textures)
    if (t && !t->gl && !t->renderTarget && !t->path.empty() && t->path[0] != u'<' && seen.insert(t).second)
      todo.push_back(t);
  const Vfs& vfs = vm.vfs();
  const bool keepData = upscaleOn_;
  const unsigned n = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  // in batches, so that only one batch of decoded images waits for its upload at a time
  const size_t kBatch = 64;
  std::vector<DecodedTexture> batch;
  for (size_t from = 0; from < todo.size(); from += kBatch) {
    const size_t count = std::min(kBatch, todo.size() - from);
    batch.assign(count, DecodedTexture());
    for (size_t i = 0; i < count; ++i) batch[i].path = utf16ToUtf8(todo[from + i]->path);
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    for (size_t k = 0; k < std::min<size_t>(n, count); ++k)
      workers.emplace_back([&] {  // Vfs::read only reads; nothing rescans or writes the file system meanwhile
        for (size_t i; (i = next++) < count;) {
          DecodedTexture& dt = batch[i];
          dt.read = vfs.read(dt.path, dt.data);
          dt.parsed = dt.read && render::parseDds(dt.data, dt.img, &dt.err);
          if (!keepData) std::vector<uint8_t>().swap(dt.data);
        }
      });
    for (auto& w : workers) w.join();
    for (size_t i = 0; i < count; ++i) {  // texture() as on first use, its image taken from the batch
      decoded_[todo[from + i]] = &batch[i];
      texture(vm, todo[from + i]);
      decoded_.erase(todo[from + i]);
    }
  }
  if (!todo.empty())
    LOG_INFO("textures: %zu decoded on worker threads and uploaded in %.0f ms", todo.size(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

uint32_t Display::texture(VM& vm, TextureObj* t) {
  if (t->gl) return t->gl;
  if (t->renderTarget) {
    render::RenderTarget* rt = renderTarget(t, 0, 0);
    return rt ? rt->color : 0;
  }
  if (t->path.empty() || t->path[0] == u'<') {
    // blank and render-target textures: transparent until render-to-texture exists
    static const uint8_t clear[4] = {0, 0, 0, 0};
    if (!blank_) blank_ = r2d_.createTextureBGRA(clear, 1, 1);
    return blank_;
  }
  render::Image img;
  std::string err;
  if (!loadTextureImage(vm, t, img, &err)) {
    // "Invalid texture file": the cache hands out textures/default.dds instead (FUN_100431c0)
    if (!t->loadFailed) LOG_WARN("Invalid texture file: '%s' (%s)", utf16ToUtf8(t->path).c_str(), err.c_str());
    t->loadFailed = true;
    auto* def = dynamic_cast<TextureObj*>(getTexture(vm, u"textures/default.dds").get());
    return def && def != t && !def->loadFailed ? texture(vm, def) : 0;
  }
  r2d_.setAnisotropy(settingNumber(vm, u"anisotropy", 0));  // window.script: video.anisotropy = $settings.$anisotropy
  t->gl = r2d_.createTexture(img);
  textureBytes_[t->gl] = imageBytes(img);
  t->cube = img.cube;
  return t->gl;
}

// ---- upscaled textures (docs/renderer/texture-upscaling.md) ----

struct Display::UpscaleJob {
  texup::Progress progress;
  std::thread thread;
  std::vector<std::string> pending;  // made by the job and loaded here: swapped in a few per frame
  ~UpscaleJob() {
    progress.stop = true;
    if (thread.joinable()) thread.join();
  }
};

Display::~Display() { popoutsShutdown(); }

int Display::textureUpscaleProgress() const {
  if (!upscaleJob_ || upscaleJob_->progress.finished) return -1;
  int total = upscaleJob_->progress.total;
  return total > 0 ? std::min(99, upscaleJob_->progress.done * 100 / total) : 0;
}

bool Display::loadTextureImage(VM& vm, TextureObj* t, render::Image& img, std::string* err) {
  std::vector<uint8_t> data;
  std::string path = utf16ToUtf8(t->path);
  t->upscaled = false;
  if (auto it = decoded_.find(t); it != decoded_.end()) {  // read and decoded ahead (uploadTextures)
    DecodedTexture& dt = *it->second;
    if (!dt.read) return false;
    if (upscaleOn_ && !host().userFolder.empty() && !dt.data.empty() &&
        texup::loadCached(texup::cacheRoot(host().userFolder), path, dt.data, img)) {
      t->upscaled = true;
      return true;
    }
    if (!dt.parsed) {
      if (err) *err = dt.err;
      return false;
    }
    img = std::move(dt.img);
    return true;
  }
  if (!vm.vfs().read(path, data)) return false;
  // the cached upscale only when it was made from exactly these bytes (a changed file shows its original)
  if (upscaleOn_ && !host().userFolder.empty() && texup::loadCached(texup::cacheRoot(host().userFolder), path, data, img)) {
    t->upscaled = true;
    return true;
  }
  return render::parseDds(data, img, err);
}

void Display::syncTextureUpscale(VM& vm) {
  // graphics page "Upscaled textures" (live); REGS_TEXTURE_UPSCALE=0|1 overrides it for test runs
  bool on = settingNumber(vm, u"regsTextureUpscale", 0) != 0;
  if (const char* e = regs::envVar("REGS_TEXTURE_UPSCALE")) on = std::atoi(e) != 0;
  if (host().userFolder.empty()) on = false;
  auto reloadable = [](TextureObj* t) {
    return t->gl && !t->renderTarget && !t->cube && !t->loadFailed && t->cpuBGRA.empty() && !t->path.empty() && t->path[0] != u'<';
  };
  if (on != upscaleOn_) {
    upscaleOn_ = on;
    auto t0 = std::chrono::steady_clock::now();
    int swapped = 0;
    // every loaded texture again, from the cache (on) or from its original file (off: exactly the original upload)
    forEachTexture([&](TextureObj* t) {
      if (!reloadable(t) || (!on && !t->upscaled)) return;
      render::Image img;
      if (!loadTextureImage(vm, t, img, nullptr)) return;
      if (on && !t->upscaled) return;  // not in the cache (yet)
      r2d_.createTexture(img, t->gl);
      textureBytes_[t->gl] = imageBytes(img);
      ++swapped;
    });
    LOG_INFO("upscaled textures %s: %d textures swapped in %.0f ms", on ? "on" : "off", swapped,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    upscaleJob_.reset();
    if (on) {
      // the missing ones are made in the background, the loaded textures first. The thread checks the cache's
      // complete.txt itself and ends at once when the cache is complete for this install
      auto job = std::make_unique<UpscaleJob>();
      texup::GenerateOptions opt;
      forEachTexture([&](TextureObj* t) {
        if (reloadable(t) && !t->upscaled) opt.first.push_back(utf16ToUtf8(t->path));
      });
      opt.threads = std::max(1, int(std::thread::hardware_concurrency()) / 2);  // the game keeps the other half
      std::filesystem::path root = texup::cacheRoot(host().userFolder), install = vm.vfs().installRoot();
      bool community = host().communityAddons;
      UpscaleJob* j = job.get();
      j->thread = std::thread([j, root, install, community, opt] {
        Vfs vfs;
        vfs.setInstallRoot(install);
        vfs.setCommunityContent(community);
        vfs.rescan();
        if (texup::cacheComplete(root, texup::installSignature(vfs))) {
          j->progress.finished = true;
          return;
        }
        LOG_INFO("upscaled textures: making the cache in %s", root.string().c_str());
        texup::generate(vfs, root, opt, j->progress, [](const std::string& s) { LOG_INFO("%s", s.c_str()); });
        LOG_INFO("upscaled textures: %d made, %d up to date, %d skipped, %d failed%s", j->progress.written.load(),
                 j->progress.upToDate.load(), j->progress.skipped.load(), j->progress.failed.load(),
                 j->progress.stop ? " (stopped)" : "");
      });
      upscaleJob_ = std::move(job);
    }
    return;
  }
  if (!upscaleJob_) return;
  // textures the job made since the last look that the game has loaded: a few per frame, so no frame stalls long
  auto now = std::chrono::steady_clock::now();
  bool finished = upscaleJob_->progress.finished;
  if (now - upscalePoll_ > std::chrono::milliseconds(500) || finished) {
    upscalePoll_ = now;
    std::vector<std::string> made;
    {
      std::lock_guard<std::mutex> lock(upscaleJob_->progress.madeMutex);
      made.swap(upscaleJob_->progress.made);
    }
    upscaleJob_->pending.insert(upscaleJob_->pending.end(), made.begin(), made.end());
  }
  auto& pending = upscaleJob_->pending;
  for (int k = 0; k < 6 && !pending.empty(); ++k) {
    auto* t = dynamic_cast<TextureObj*>(textureIfLoaded(utf8ToUtf16(pending.back())));
    pending.pop_back();
    if (!t || !reloadable(t) || t->upscaled) continue;
    render::Image img;
    if (loadTextureImage(vm, t, img, nullptr) && t->upscaled) {
      r2d_.createTexture(img, t->gl);
      textureBytes_[t->gl] = imageBytes(img);
    }
  }
  if (finished && pending.empty()) upscaleJob_.reset();
}

namespace {

// The GUI pass of one viewport (gsViewport_OGL::renderGui, docs/ui/controls.md §1): per control, its image and border
// in the owner's client area, then its text and children in its own client area (scissor = client rect ∩ owner's).
struct GuiPass {
  VM& vm;
  Display& d;
  float vpX = 0, vpY = 0, unit = 1;  // viewport origin (px) and pixels per GUI unit
  int sx = 0, sy = 0, sw = 0, sh = 0;  // current scissor in window pixels
  TracerViews tracerViews;             // the tracers' target viewports, worked out once in this pass

  void scissor(int x, int y, int w, int h) {
    sx = x;
    sy = y;
    sw = w;
    sh = h;
    d.r2d().setScissor(x, y, w, h);
  }

  void image(ControlObj* c) {
    // updateBackGround (§4): border-inset rect, crop uvs, hot colour while hovered or active, own visibility only
    uint32_t col = mulA((c->hot || c->active) ? c->hotBackground : c->background, c->visibility);
    if (!(col >> 24)) return;
    if (regs::envVar("REGS_DEBUG_VIEWPORT") && c->width * c->height > 500000) {
      static std::map<ControlObj*, int> seen;
      auto* t = dynamic_cast<TextureObj*>(c->texture.get());
      if (seen[c]++ == 0)
        LOG_INFO("large image: %s %.0fx%.0f colour %08x texture %s shader %s", c->cls->name.c_str(), c->width, c->height, col,
                 t ? utf16ToUtf8(t->path).c_str() : "-", utf16ToUtf8(c->shaderPath).c_str());
    }
    float bw = c->borderWidth;
    float wl = c->borderSides & 4 ? bw : 0, wt = c->borderSides & 1 ? bw : 0;
    float wr = c->borderSides & 8 ? bw : 0, wb = c->borderSides & 2 ? bw : 0;
    float L = c->desktopLeft(), T = c->desktopTop(), W = c->width, H = c->height;
    uint32_t rgba = render::argbToRgba(col);
    GuiVertex q[4] = {{L + wl, T + wt, c->cropL, c->cropT, rgba},
                      {L + W - wr, T + wt, c->cropL + c->cropW, c->cropT, rgba},
                      {L + W - wr, T + H - wb, c->cropL + c->cropW, c->cropT + c->cropH, rgba},
                      {L + wl, T + H - wb, c->cropL, c->cropT + c->cropH, rgba}};
    auto* tex = dynamic_cast<TextureObj*>(c->texture.get());
    uint32_t gltex = tex ? d.texture(vm, tex) : 0;
    if (uint32_t prog = c->shaderPath.empty() ? 0 : d.guiProgram(vm, c->shaderPath)) {
      // the material shader replaces modulation; it sees every texture slot (secondary view: picture + mask)
      uint32_t slots[8] = {gltex};
      int count = 1;
      for (int i = 0; i < 7; ++i)
        if (auto* t = dynamic_cast<TextureObj*>(c->slotTextures[i].get())) {
          slots[i + 1] = d.texture(vm, t);
          count = i + 2;
        }
      float custom0[4];
      bool params = mapImageParams(vm, c, custom0);  // the minimap's view of its texture
      d.r2d().drawWithProgram(prog, slots, count, render::Prim::Quads, q, 4, params ? custom0 : nullptr);
    }
    else
      d.r2d().draw(gltex, render::Prim::Quads, q, 4);
  }

  void border(ControlObj* c) {
    // updateBorder (§5): 4 mitred quads inside the rect; sides not in the mask keep alpha 0
    uint32_t col = mulA(c->borderColor, c->visibility);
    if (!(col >> 24)) return;
    float bw = c->borderWidth;
    float wl = c->borderSides & 4 ? bw : 0, wt = c->borderSides & 1 ? bw : 0;
    float wr = c->borderSides & 8 ? bw : 0, wb = c->borderSides & 2 ? bw : 0;
    float L = c->desktopLeft(), T = c->desktopTop(), W = c->width, H = c->height;
    struct P { float x, y; };
    P oTL{L, T}, oTR{L + W, T}, oBR{L + W, T + H}, oBL{L, T + H};
    P iTL{L + wl, T + wt}, iTR{L + W - wr, T + wt}, iBR{L + W - wr, T + H - wb}, iBL{L + wl, T + H - wb};
    const P sides[4][4] = {{iTL, oTL, oTR, iTR}, {iBR, oBR, oBL, iBL}, {iTL, oTL, oBL, iBL}, {oTR, iTR, iBR, oBR}};
    uint32_t rgba = render::argbToRgba(col);
    for (int s = 0; s < 4; ++s) {
      if (!(c->borderSides & (1u << s))) continue;  // drawn with alpha 0 by the engine: invisible
      GuiVertex q[4];
      for (int k = 0; k < 4; ++k) q[k] = {sides[s][k].x, sides[s][k].y, 0, 0, rgba};
      d.r2d().draw(0, render::Prim::Quads, q, 4);
    }
  }

  void text(ControlObj* c) {
    // gsGuiText_OGL::render (§6.4): shadow pass first (if its colour has alpha), then the text with inline colours
    if (c->display.empty()) return;
    Font* f = controlFont(vm, c);
    if (!f || f->glyphs.empty()) return;
    TextParams p;
    float x, y;
    controlTextPos(c, *f, p, x, y);
    uint32_t colour = mulA(c->textColor, c->visibility);
    uint32_t shadow = c->shadowPushed ? mulA(c->shadowColor, c->visibility) : 0;
    if (shadow >> 24) drawText(vm, d, *f, c->display, p, x + c->shadowDX, y + c->shadowDY, shadow, false);
    drawText(vm, d, *f, c->display, p, x, y, colour, true);
  }

  // gsEditCtrl caret (updateCaret, controls.md §11): a bar after the selStart-th character, 0.12 chars wide, one char
  // high, in caretColor with the blink as alpha
  void caret(ControlObj* c) {
    Font* f = controlFont(vm, c);
    if (!f || f->glyphs.empty()) return;
    TextParams p;
    float x, y;
    controlTextPos(c, *f, p, x, y);
    auto sel = c->fieldValues.find(u"selStart");
    int n = int(c->display.size());
    int start = sel == c->fieldValues.end() || sel->second.isNone() ? 0 : std::clamp(vm.toInt(sel->second), 0, n);
    for (int i = 0; i < start; ++i) x += charAdvance(*f, c->display[size_t(i)], p);
    auto col = c->fieldValues.find(u"caretColor");
    uint32_t argb = col == c->fieldValues.end() || col->second.isNone() ? 0xFFFFFFFFu : uint32_t(vm.toInt(col->second));
    uint32_t alpha = std::min(255u, uint32_t(std::abs(std::trunc(c->caretPhase * 255.0f))));
    uint32_t rgba = render::argbToRgba((argb & 0x00FFFFFFu) | alpha << 24);
    float x1 = x + 0.12f * p.sizeX, y1 = y + p.sizeY;
    GuiVertex q[4] = {{x, y, 0, 0, rgba}, {x1, y, 0, 0, rgba}, {x1, y1, 0, 0, rgba}, {x, y1, 0, 0, rgba}};
    d.r2d().draw(0, render::Prim::Quads, q, 4);
  }

  // gsGraphCtrl items (docs/ui/graphs.md §4): in the owner's client area like image and border, layered after them
  // (z·256+10.. series strips, +0x1e grid, +0x1f labels) and before the control's own client area (+0x80)
  void graph(GraphObj* g) {
    Font* f = controlFont(vm, g);
    bool font = f && !f->glyphs.empty();
    TextParams p;
    p.sizeX = g->sizeX;
    p.sizeY = g->sizeY;
    p.spacingX = g->spacingX;
    p.spacingY = g->spacingY;
    p.boxX = std::max(0.0f, g->width - g->marginL - g->marginR);  // the items are sized to the client area
    GraphGeometry geo;
    graphGeometry(vm, g, [&](const std::u16string& s) { return font ? measureWidth(*f, s, p) : 0.0f; }, geo);
    std::vector<GuiVertex> v;
    for (const auto& strip : geo.strips) {
      v.clear();
      for (const auto& q : strip) v.push_back({q.x, q.y, 0, 0, render::argbToRgba(q.argb)});
      d.r2d().draw(0, render::Prim::LineStrip, v.data(), v.size());
    }
    v.clear();
    for (const auto& q : geo.grid) v.push_back({q.x, q.y, 0, 0, render::argbToRgba(q.argb)});
    if (!v.empty()) d.r2d().draw(0, render::Prim::Lines, v.data(), v.size());
    if (!font) return;
    // the labels take the control's font, colour and shadow (gsGraphCtrl setFont / setTextShadow)
    uint32_t colour = mulA(g->textColor, g->visibility);
    uint32_t shadow = g->shadowPushed ? mulA(g->shadowColor, g->visibility) : 0;
    for (const auto& l : geo.labels) {
      if (shadow >> 24) drawText(vm, d, *f, l.text, p, l.x + g->shadowDX, l.y + g->shadowDY, shadow, false);
      drawText(vm, d, *f, l.text, p, l.x, l.y, colour, true);
    }
  }

  // placed: the control is a child of a gsTracerOrganizer, which placed and laid it out already this frame
  void control(ControlObj* c, bool placed = false) {
    if (!c->visible()) return;
    if (drawMapFootprint(d, vm, c)) return;  // a line primitive, no rect of its own
    placeMapPin(vm, c);
    if (!placed) placeTracer(vm, c, &tracerViews);
    static const ClassInfo* organizerClass = vm.findClass("gsTracerOrganizer");
    const bool organizer = organizerClass && c->cls->isA(organizerClass);
    if (organizer) organizeTracers(vm, c, &tracerViews);
    if (c->clipped()) return;
    if (c->cls->name == "gsViewPort") {
      renderViewportScene(d, vm, c);  // fills its render target first
      tracerViews.clear();            // it eased its camera: later tracers project with the new one
    }
    if (c->hasImage) image(c);
    if (c->hasBorder) border(c);
    if (auto* g = dynamic_cast<GraphObj*>(c)) graph(g);
    // own client area: rect = client rect (no translation), intersected with the owner's scissor
    int px = sx, py = sy, pw = sw, ph = sh;
    float cx = vpX + (c->desktopLeft() + c->marginL) * unit, cy = vpY + (c->desktopTop() + c->marginT) * unit;
    float cw = (c->width - c->marginL - c->marginR) * unit, ch = (c->height - c->marginT - c->marginB) * unit;
    int x0 = std::max(px, int(std::floor(cx))), y0 = std::max(py, int(std::floor(cy)));
    int x1 = std::min(px + pw, int(std::ceil(cx + cw))), y1 = std::min(py + ph, int(std::ceil(cy + ch)));
    if (x1 <= x0 || y1 <= y0) return;
    scissor(x0, y0, x1 - x0, y1 - y0);
    if (c->hasText) text(c);
    if (c->active && c->isEdit()) caret(c);
    for (auto& k : c->children)
      if (auto* kc = dynamic_cast<ControlObj*>(k.get())) control(kc, organizer);
    scissor(px, py, pw, ph);
  }
};

}  // namespace

void Display::renderFrame(VM& vm, Object* win, Object* video) {
  // REGS_LOG_TIME: frames over 250 ms are logged with the shader programs linked in them
  static const bool logTime = regs::envVar("REGS_LOG_TIME") != nullptr;
  const auto frameStart = std::chrono::steady_clock::now();
  // REGS_FRAME_STATS=<from>:<to> (measurements): frame times between those frames, the bytes of the uploaded file
  // textures and the driver's free video memory (GL_NVX_gpu_memory_info) at the end
  if (static const char* fs = regs::envVar("REGS_FRAME_STATS"); fs && std::strchr(fs, ':')) {
    static const int from = std::atoi(fs), to = std::atoi(std::strchr(fs, ':') + 1);
    static int frame = 0;
    static std::vector<double> ms;
    static std::chrono::steady_clock::time_point last;
    if (++frame > from && frame <= to) ms.push_back(std::chrono::duration<double, std::milli>(frameStart - last).count());
    last = frameStart;
    if (frame == to && !ms.empty()) {
      std::vector<double> s = ms;
      std::sort(s.begin(), s.end());
      double sum = 0;
      for (double v : s) sum += v;
      size_t bytes = 0, up = 0, count = 0;
      forEachTexture([&](TextureObj* t) {
        if (auto it = textureBytes_.find(t->gl); t->gl && it != textureBytes_.end()) bytes += it->second, up += t->upscaled, ++count;
      });
      LOG_INFO("frame stats %d-%d: mean %.2f ms, median %.2f, 95th %.2f, 99th %.2f; %zu file textures (%zu upscaled) %.1f MB; video memory in use %d MB",
               from, to, sum / double(s.size()), s[s.size() / 2], s[s.size() * 95 / 100], s[s.size() * 99 / 100], count, up,
               double(bytes) / 1048576.0, r2d_.videoMemoryInUseMB());
    }
  }
  const int linkedBefore = render::gProgramsLinked;
  const double linkBefore = render::gLinkSeconds;
  for (double& p : gPhaseSeconds) p = 0;
  struct SlowFrame {
    bool on;
    std::chrono::steady_clock::time_point t0;
    int linked;
    double link;
    int pipelines;
    double pipelineMs;
    ~SlowFrame() {
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (on && ms > 250)
        LOG_INFO("slow frame: %.0f ms (%d shader programs linked in %.0f ms, %d pipelines in %.0f ms; scene %.0f, grid %.0f, vehicles %.0f, particles %.0f, night %.0f, programs %.0f ms)",
                 ms, render::gProgramsLinked - linked, (render::gLinkSeconds - link) * 1000,
                 render::gpu::dev().pipelinesCreated() - pipelines, render::gpu::dev().pipelineMs() - pipelineMs,
                 gPhaseSeconds[1] * 1000, gPhaseSeconds[2] * 1000, gPhaseSeconds[3] * 1000, gPhaseSeconds[4] * 1000,
                 gPhaseSeconds[5] * 1000, gPhaseSeconds[6] * 1000);
    }
  } slowFrame{logTime, frameStart, linkedBefore, linkBefore, render::gpu::dev().pipelinesCreated(),
              render::gpu::dev().pipelineMs()};
  sceneFrameBegin();
  syncRenderSettings(vm);
  sweepEffects(*this);
  stylePreviewFrame(*this, vm);
  int w = window_->width(), h = window_->height();
  uint32_t background = uint32_t(vm.toInt(field(vm, win, u"background")));
  r2d_.timer = effectTimer;
  r2d_.sharpScale = uiFilterSharp_ && uiScale > 1.0f ? uiScale : 0.0f;
  r2d_.lineWidth = uiScale;
  r2d_.beginFrame(w, h, background);
  if (video) {
    for (auto& k : video->children) {
      auto* vp = dynamic_cast<ControlObj*>(k.get());
      if (!vp || !vp->visible()) continue;
      // pixel mode: script pixels, uiScale window pixels each (the scripts' window is the real one / uiScale);
      // otherwise one unit = the window height
      bool pixels = vm.truthy(field(vm, vp, u"pixelCoordinates"));
      float scale = pixels ? uiScale : float(h);
      float px = vp->left * scale, py = vp->top * scale, pw = vp->width * scale, ph = vp->height * scale;
      if (pw <= 0 || ph <= 0) continue;
      float uw = pixels ? vp->width : pw / ph, uh = pixels ? vp->height : 1.0f;
      r2d_.setProjection(px, py, pw, ph, uw, uh);
      renderViewportScene(*this, vm, vp);  // a root viewport with a scene draws it first, its GUI children on top
      r2d_.setProjection(px, py, pw, ph, uw, uh);
      GuiPass pass{vm, *this, px, py, pixels ? uiScale : ph};
      pass.scissor(int(px), int(py), int(pw), int(ph));
      // the viewport is its own client area: its children are drawn in it (not with --at ui:off)
      if (!host().hideGui)
        for (auto& c : vp->children)
          if (auto* cc = dynamic_cast<ControlObj*>(c.get())) pass.control(cc);
    }
  }
  if (!host().hideGui) drawFpsReadout(vm);
  r2d_.endFrame();
  if (captureNext) {
    auto fn = std::move(captureNext);
    captureNext = nullptr;
    std::vector<uint8_t> px = r2d_.readPixels();
    fn(px, r2d_.frameWidth(), r2d_.frameHeight());
  }
  popoutsRender(*this, vm);  // the pop-out views (engine/popout.cpp), each into its own window
  std::filesystem::path pqShot;
  if (!shot_.empty()) {
    if (saveScreenshot(shot_)) LOG_INFO("screenshot: %s", shot_.string().c_str());
    else LOG_ERROR("screenshot failed: %s", shot_.string().c_str());
    // REGS_HDR_DUMP=1 (a check): in HDR, the encoded frame too (PQ code values as an image, <shot>-pq.png)
    static const bool pqDump = regs::envVar("REGS_HDR_DUMP") != nullptr;
    if (pqDump && render::gpu::dev().hdrActive()) pqShot = shot_.parent_path() / (shot_.stem().string() + "-pq.png");
    shot_.clear();
  }
  hdrEncode(*this, vm, w, h);  // after the screenshot, which stays SDR
  temporalFrameGenSubmit(*this, vm, w, h);  // FSR 3 frame generation: this frame's HUD-less view to the device
  if (!pqShot.empty() && saveScreenshot(pqShot)) LOG_INFO("screenshot: %s", pqShot.string().c_str());
  const auto beforePresent = std::chrono::steady_clock::now();
  render::gpu::dev().present();
  benchFrame(std::chrono::duration<double, std::milli>(beforePresent - frameStart).count());
}

// REGS_BENCH=<first frame>:<frames> (docs/renderer/vulkan.md §10): from the first frame on, the interval between two
// presents (the whole game frame: simulation, scripts, rendering and waiting for the GPU) and the CPU time of the
// render call before the present, then one summary line in the log. Run with vsync and the frame limit off.
void Display::benchFrame(double renderCpuMs) {
  static const char* spec = regs::envVar("REGS_BENCH");
  if (!spec) return;
  static int first = 0, count = 0, frame = 0;
  static bool parsed = false, done = false;
  static std::vector<double> intervals, cpu, gpu;
  static std::chrono::steady_clock::time_point last;
  if (!parsed) {
    parsed = true;
    if (std::sscanf(spec, "%d:%d", &first, &count) != 2 || count <= 0) done = true;
  }
  auto now = std::chrono::steady_clock::now();
  ++frame;
  if (!done && frame > first) {
    intervals.push_back(std::chrono::duration<double, std::milli>(now - last).count());
    cpu.push_back(renderCpuMs);
    if (double g = render::gpu::dev().gpuFrameMs(); g >= 0) gpu.push_back(g);
    if (int(intervals.size()) >= count) {
      done = true;
      std::vector<double> s = intervals;
      std::sort(s.begin(), s.end());
      double sum = 0, cpuSum = 0;
      for (double v : intervals) sum += v;
      for (double v : cpu) cpuSum += v;
      auto pct = [&](double p) { return s[std::min(s.size() - 1, size_t(p * double(s.size())))]; };
      // the worst frame's number (as REGS_BENCH and --at count frames)
      const int worst = first + 1 + int(std::max_element(intervals.begin(), intervals.end()) - intervals.begin());
      std::sort(gpu.begin(), gpu.end());
      double gpuMedian = gpu.empty() ? -1 : gpu[gpu.size() / 2];
      double gpuP95 = gpu.empty() ? -1 : gpu[std::min(gpu.size() - 1, size_t(0.95 * double(gpu.size())))];
      LOG_INFO("bench: %s, %dx%d, %d frames: %.1f fps average, frame ms median %.2f, p95 %.2f, p99 %.2f, max %.2f (frame %d); render cpu ms %.2f; gpu ms median %.2f, p95 %.2f",
               render::gpu::dev().name(), r2d_.frameWidth(), r2d_.frameHeight(), count, 1000.0 * double(s.size()) / sum,
               pct(0.5), pct(0.95), pct(0.99), s.back(), worst, cpuSum / double(cpu.size()), gpuMedian, gpuP95);
    }
  }
  last = now;
}

void Display::syncRenderSettings(VM& vm) {
  // graphics page, applied live (the page writes the settings when the player presses OK): MSAA samples of the view
  // targets and anisotropic filtering of every loaded texture. window.script copies the same settings into
  // window.antialiasing and video.anisotropy at start
  int aa = std::clamp(int(settingNumber(vm, u"antialiasing", float(antialiasing))), 0, 16);
  if (aa != antialiasing) {
    LOG_INFO("antialiasing: %d -> %d samples", antialiasing, aa);
    antialiasing = aa;
  }
  // window.vsync: the page sets it on OK (the original applied it at once too)
  if (windowObj_) {
    bool vs = vm.truthy(field(vm, windowObj_.get(), u"vsync"));
    if (vs != vsync_) {
      if (vsyncKnown_) LOG_INFO("vsync: %s", vs ? "on" : "off");
      render::gpu::dev().setVsync(vs);
      vsync_ = vs;
      vsyncKnown_ = true;
    }
  }
  hdrSync(*this, vm);  // HDR output (engine/hdr.h): the device switches before the frame's back buffer begins
  float aniso = settingNumber(vm, u"anisotropy", r2d_.anisotropy);
  if (aniso != r2d_.anisotropy) {
    LOG_INFO("anisotropic filtering: %.0f -> %.0f", double(r2d_.anisotropy), double(aniso));
    r2d_.setAnisotropy(aniso);
  }
  syncTextureUpscale(vm);
}

void Display::drawFpsReadout(VM& vm) {
  if (settingNumber(vm, u"regsShowFps", 0) == 0) {
    if (fps_.on) fps_ = FpsReadout{};  // off: nothing measured, history dropped
    return;
  }
  auto now = std::chrono::steady_clock::now();
  if (!fps_.on) {
    fps_.on = true;
    fps_.last = now;
    fps_.text = u"-- FPS\n";
  } else {
    double dt = std::chrono::duration<double>(now - fps_.last).count();
    fps_.last = now;
    fps_.window.push_back(dt);
    fps_.windowSum += dt;
    while (fps_.window.size() > 1 && fps_.windowSum - fps_.window.front() >= 1.0) {
      fps_.windowSum -= fps_.window.front();
      fps_.window.pop_front();
    }
    ++fps_.frames;
    fps_.elapsed += dt;
    if (fps_.elapsed >= 0.5) {  // the shown values change twice a second
      auto [lo, hi] = std::minmax_element(fps_.window.begin(), fps_.window.end());
      char buf[96];
      std::snprintf(buf, sizeof buf, "%.0f FPS  %.1f ms\nmin %.0f  max %.0f", fps_.frames / fps_.elapsed,
                    fps_.elapsed * 1000.0 / fps_.frames, 1.0 / std::max(*hi, 1e-6), 1.0 / std::max(*lo, 1e-6));
      fps_.text = utf8ToUtf16(buf);
      fps_.frames = 0;
      fps_.elapsed = 0;
    }
  }
  Font* f = getFont(vm, u"metro/fonts/tahoma-8", u"");
  if (!f || f->glyphs.empty()) return;
  // the GUI's text font (ui.script setTextFont) in window pixels, at the top edge right of the centred toolbar
  // (query / build / bulldoze, ±105 px): the reputation bar and money are top left, the line and vehicle panel and
  // the menus' close buttons top right, the minimap and date bottom left, the info panel bottom right
  // in script pixels, scaled with the GUI (UI scale)
  int fw = r2d_.frameWidth(), fh = r2d_.frameHeight();
  float w = float(fw) / uiScale, h = float(fh) / uiScale;
  r2d_.setProjection(0, 0, float(fw), float(fh), w, h);
  r2d_.setScissor(0, 0, -1, -1);
  TextParams p;
  p.sizeX = p.sizeY = 15;
  p.spacingX = -2;
  const float pad = 5;
  float tw = std::max(measureWidth(*f, fps_.text, p), 96.0f), th = measureHeight(*f, fps_.text, p);
  float x = std::floor(float(w) * 0.5f + 116), y = 3;
  uint32_t bg = render::argbToRgba(0xA0000000u);
  GuiVertex q[4] = {{x, y, 0, 0, bg}, {x + tw + 2 * pad, y, 0, 0, bg}, {x + tw + 2 * pad, y + th + 2 * pad, 0, 0, bg},
                    {x, y + th + 2 * pad, 0, 0, bg}};
  r2d_.draw(0, render::Prim::Quads, q, 4);
  drawText(vm, *this, *f, fps_.text, p, x + pad + 1, y + pad + 1, 0xFF000000u, false);
  drawText(vm, *this, *f, fps_.text, p, x + pad, y + pad, 0xFFE8E8E8u, false);
}


uint32_t Display::guiProgram(VM& vm, const std::u16string& path) {
  // gsShaders_OGL::getShader for a GUI material: the .shader file is a script (gsShaderCombiner_OGL) whose onProgram
  // event names the vertex and fragment components (docs/renderer/interfaces-3d.md §2.12)
  auto it = guiPrograms_.find(path);
  if (it != guiPrograms_.end()) return it->second;
  uint32_t& prog = guiPrograms_[path];
  LoadOptions opt;
  opt.cls = vm.findClass("gsShaderCombiner_OGL");
  Value sv = vm.load(u"/" + path, opt);  // already resolved
  Object* shader = sv.object();
  if (!shader || !shader->code) return prog = 0;
  vm.fireEvent(shader, u"onProgram", {Value::integer(0), Value::integer(0), Value::integer(0)});
  std::string src[2], why;
  const char16_t* fields[2] = {u"vertexComponent", u"fragmentComponent"};
  for (int i = 0; i < 2; ++i) {
    Value c = field(vm, shader, fields[i]);
    std::vector<uint8_t> data;
    if (c.type() != VT::String || !vm.vfs().read(utf16ToUtf8(vm.resolvePath(c.str(), shader)), data)) {
      LOG_WARN("shader %s: component missing", utf16ToUtf8(path).c_str());
      return prog = 0;
    }
    std::string text = utf16ToUtf8(decodeTextFile(data));
    if (text.find("\n#") != std::string::npos || text.rfind("#", 0) == 0) {
      LOG_WARN("shader %s: template components need the shader combiner (not yet ported)", utf16ToUtf8(path).c_str());
      return prog = 0;
    }
    src[i] = render::translateGlsl110(text, i == 1, &why);
    if (src[i].empty()) {
      LOG_WARN("shader %s: uses %s", utf16ToUtf8(path).c_str(), why.c_str());
      return prog = 0;
    }
  }
  std::string err;
  prog = r2d_.buildProgram(src[0], src[1], &err);
  if (!prog) LOG_WARN("shader %s: %s", utf16ToUtf8(path).c_str(), err.c_str());
  return prog;
}

render::RenderTarget* Display::renderTarget(TextureObj* t, int minW, int minH) {
  // sizes only grow: max(requested, texture size) (gsPostEffect_OGL::initFrameBuffer, scene-basics.md §7.1). Port:
  // the texture size times the UI scale, so a view shown in the GUI keeps one texel per window pixel; the scripts'
  // crop and tiling rects are fractions of the texture and stay right (docs/ui/scaling.md §3)
  int sw = int(std::ceil(float(t->width) * uiScale)), sh = int(std::ceil(float(t->height) * uiScale));
  int w = std::min(8192, std::max({1, sw, minW})), h = std::min(8192, std::max({1, sh, minH}));
  auto it = targets_.find(t);
  if (it != targets_.end()) {
    if (!it->second.fbo) return nullptr;
    if (it->second.width >= w && it->second.height >= h) return &it->second;
    w = std::max(w, it->second.width);
    h = std::max(h, it->second.height);
    r3d_.destroyTarget(it->second);
  }
  render::RenderTarget& rt = targets_[t];
  if (!r3d_.makeTarget(rt, w, h)) {
    LOG_ERROR("render target %dx%d: framebuffer incomplete", w, h);
    rt.fbo = 0;
    return nullptr;
  }
  t->gl = rt.color;
  return &rt;
}

bool Display::saveScreenshot(const std::filesystem::path& file) {
  std::vector<uint8_t> px = r2d_.readPixels();
  for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // the window shows the colour only; 3D clears leave alpha 0
  std::ofstream f(file, std::ios::binary);
  if (!f) return false;
  writePng(f, px.data(), r2d_.frameWidth(), r2d_.frameHeight());
  return bool(f);
}

void Display::dumpTarget(const render::RenderTarget& rt, const std::filesystem::path& file) {
  // debugging aid (REGS_DUMP_TARGET=<file.png>): the target's pixels as stored (bottom row first)
  std::vector<uint8_t> px(size_t(rt.width) * size_t(rt.height) * 4);
  render::gpu::dev().readPixels(rt.fbo, 0, 0, rt.width, rt.height, px.data());
  std::ofstream f(file, std::ios::binary);
  if (f) writePng(f, px.data(), rt.width, rt.height);
}

void Display::saveTargetPng(const render::RenderTarget& rt, int w, int h, const std::filesystem::path& file) {
  w = std::min(w, rt.width);
  h = std::min(h, rt.height);
  if (w <= 0 || h <= 0) return;
  std::vector<uint8_t> px(size_t(w) * size_t(h) * 4), rows(px.size());
  render::gpu::dev().readPixels(rt.fbo, 0, 0, w, h, px.data());
  for (int y = 0; y < h; ++y)  // GL rows (bottom first) to image rows
    std::memcpy(&rows[size_t(y) * size_t(w) * 4], &px[size_t(h - 1 - y) * size_t(w) * 4], size_t(w) * 4);
  std::ofstream f(file, std::ios::binary);
  if (f) writePng(f, rows.data(), w, h);
}

void registerDisplayNatives(VM& vm) {
  if (ClassInfo* c = vm.findClass("gsWindow_OGL")) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new WindowObj(ci)); };
  // video.getEffectTimer() (0x1004e620 → gsI3DVideo slot 0x11c): the shaders' effect timer, for shader parameters
  // that start an effect "now" (achievement.script:196)
  implement(vm, "gsVideo_OGL", u"getEffectTimer", [](VM&, Object*, int, const Value*) {
    Display* d = display();
    return Value::real(d ? d->effectTimer : 0.0f);
  });
  // port: window.regsApplyDisplay(fullscreen, width, height [, relayout]) for the graphics page (Display::applyDisplay)
  if (ClassInfo* c = vm.findClass("gsWindow_OGL")) {
    addMethod(c, u"regsApplyDisplay", 3, 4);
    addMethod(c, u"regsUiScale", 0, 0);
    addMethod(c, u"regsTextureUpscaleProgress", 0, 0);
    addMethod(c, u"regsSetRect", 5, 5);
    addMethod(c, u"regsGraphicsCaps", 0, 0);
  }
  // window.regsGraphicsCaps(): what the running graphics engine offers, for the Image quality page (which greys out
  // what does not apply): 1 Vulkan, 2 FSR 3.1, 4 frame generation, 8 ray queries (shadows, occlusion), 16 raytraced
  // reflections, 32 HDR10 output offered by the display
  implement(vm, "gsWindow_OGL", u"regsGraphicsCaps", [](VM&, Object*, int, const Value*) {
    if (!render::gpu::hasDevice()) return Value::integer(0);
    render::gpu::Device& g = render::gpu::dev();
    int bits = g.info().rfind("Vulkan", 0) == 0 ? 1 : 0;
    if (g.fsrAvailable()) bits |= 2;
    if (g.frameGenAvailable()) bits |= 4;
    if (g.rayTracing()) bits |= 8;
    if (g.rayReflections()) bits |= 16;
    if (g.hdrAvailable()) bits |= 32;
    return Value::integer(bits);
  });
  // window.regsSetRect(control, left, top, width, height): placeControl, for HUD scripts that place a control again
  // when the desktop changes size (their extensions' onChangeRect handlers)
  implement(vm, "gsWindow_OGL", u"regsSetRect", [](VM& v, Object*, int, const Value* a) {
    Value cv = a[0].ref() ? a[0].ref()->get() : a[0];
    auto* c = dynamic_cast<ControlObj*>(cv.object());
    if (c && !c->destroyed) placeControl(v, c, v.toFloat(a[1]), v.toFloat(a[2]), v.toFloat(a[3]), v.toFloat(a[4]));
    return Value();
  });
  // window.regsTextureUpscaleProgress(): percent of the background run making upscaled textures, -1 when none runs
  implement(vm, "gsWindow_OGL", u"regsTextureUpscaleProgress", [](VM&, Object*, int, const Value*) {
    Display* d = display();
    return Value::integer(d ? d->textureUpscaleProgress() : -1);
  });
  // window.regsUiScale(): the UI scale in use (the automatic choice when the setting is unset)
  implement(vm, "gsWindow_OGL", u"regsUiScale", [](VM&, Object*, int, const Value*) {
    Display* d = display();
    return Value::real(d ? d->uiScale : 1.0f);
  });
  implement(vm, "gsWindow_OGL", u"regsApplyDisplay", [](VM& v, Object* s, int argc, const Value* a) {
    Display* d = display();
    if (!d || d->windowObject() != s) return Value::integer(0);
    bool relayout = argc > 3 && v.truthy(a[3]);
    return Value::integer(d->applyDisplay(v, v.truthy(a[0]), v.toInt(a[1]), v.toInt(a[2]), relayout));
  });
  // window.init(): gsWindow_OGL::start (0x10059200) creates the OS window and context from the fields
  implement(vm, "gsWindow_OGL", u"init", [](VM& v, Object* s, int, const Value*) {
    if (host().headless || g_display) return Value();
    platform::WindowDesc desc;
    desc.width = std::max(320, v.toInt(field(v, s, u"width")));
    desc.height = std::max(240, v.toInt(field(v, s, u"height")));
    desc.title = v.toStr(field(v, s, u"title"));
    desc.fullscreen = !host().forceWindowed && v.truthy(field(v, s, u"fullscreen"));
    if (host().forceWidth > 0 && host().forceHeight > 0) {  // --size: e.g. 3840x2160 for 4K screenshots
      desc.width = host().forceWidth;
      desc.height = host().forceHeight;
      desc.fullscreen = false;
    }
    desc.vsync = v.truthy(field(v, s, u"vsync"));
    // graphics engine (settings regsRenderer): 0 = OpenGL 3.3, 1 = Vulkan, the default (falls back to OpenGL when it cannot start);
    // REGS_RENDERER=gl|vulkan overrides the setting (test runs)
    int engine = int(settingNumber(v, u"regsRenderer", 1));
    if (const char* r = regs::envVar("REGS_RENDERER")) engine = std::string(r) == "vulkan" ? 1 : 0;
    // the engine put MSAA into the window's pixel format without post-processing (and into the effect buffers with
    // it). Port: never into the pixel format, which cannot change once set; views without effects draw into a
    // multisampled target (msaaTarget), so the antialiasing setting applies while the game runs
    desc.samples = 0;
    auto d = std::make_unique<Display>();
    d->antialiasing = std::max(0, v.toInt(field(v, s, u"antialiasing")));
    std::string err;
    if (!d->open(desc, engine == 1, &err)) {
      LOG_ERROR("cannot open the game window: %s", err.c_str());
      host().quit = true;
      return Value();
    }
    d->window().showCursor(v.truthy(field(v, s, u"systemCursor")));
    // the client area is the real size (fullscreen takes the monitor's); with a UI scale the scripts see it divided
    // by the scale and lay the GUI out in that space (docs/ui/scaling.md)
    int rw = d->window().width(), rh = d->window().height();
    float ui = d->chooseUiScale(v, rw, rh);
    s->fieldValues[u"width"] = Value::integer(int(std::lround(float(rw) / ui)));
    s->fieldValues[u"height"] = Value::integer(int(std::lround(float(rh) / ui)));
    if (ui != 1.0f)
      LOG_INFO("ui scale %.1f: the scripts' window is %dx%d", double(ui), v.toInt(s->fieldValues[u"width"]),
               v.toInt(s->fieldValues[u"height"]));
    s->fieldValues[u"mode"] = Value::string(utf8ToUtf16("Video: " + d->r2d().info));
    d->setWindowObject(s);
    g_display = std::move(d);
    return Value();
  });
}

}  // namespace regs::engine
