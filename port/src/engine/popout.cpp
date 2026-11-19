// Pop-out views (owner's request 2026-11-19: "Pop Out Windows ... can be placed anywhere (for example on other
// screens). Scrolling and moving the camera is independent and can be changed when the window has focus. It can
// either be fullscreen or a resizable window"). Spec and controls: docs/popout-views.md.
//
// Each pop-out is a second OS window (platform::Window, secondary + resizable) the graphics device can show pictures
// in (gpu::Device::addWindow: its own swapchain in Vulkan, the shared context in OpenGL). Once a frame, after the main
// view, the map viewport's scene is drawn again from the pop-out's own camera into a scene target of the window's size
// (renderViewportScene with a ViewOverride: sky, fog, weather, lights, night lights, shadows; not the screen-only
// passes) and shown in the window. The camera is an orbit about a point on the ground (yaw, tilt, distance), eased
// toward its goals like the game's smooth camera, and only listens to its own window's input.
#include "engine/popout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>

#include "core/log.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/gui.h"
#include "engine/host.h"
#include "engine/scene3d.h"
#include "render/gpu.h"

namespace regs::engine {

using namespace regs::script;
using render::Vec3;

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kMinDist = 15.0f, kMaxDist = 2500.0f;
constexpr float kMinPitch = 0.10f, kMaxPitch = 1.53f;  // radians below the horizon (about 6 to 88 degrees)

// Windows virtual-key codes (platform::InputEvent::key)
enum : int {
  kVkReturn = 0x0D, kVkShift = 0x10, kVkControl = 0x11, kVkEscape = 0x1B, kVkPrior = 0x21, kVkNext = 0x22,
  kVkHome = 0x24, kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28, kVkA = 0x41, kVkD = 0x44,
  kVkE = 0x45, kVkF = 0x46, kVkQ = 0x51, kVkR = 0x52, kVkS = 0x53, kVkW = 0x57, kVkAdd = 0x6B, kVkSubtract = 0x6D,
  kVkF7 = 0x76, kVkF11 = 0x7A, kVkOemPlus = 0xBB, kVkOemMinus = 0xBD
};

struct Orbit {
  Vec3 focus;          // the point on the ground it looks at and turns about
  float yaw = 0;       // the view's heading: looking along (sin yaw, ·, cos yaw)
  float pitch = 0.8f;  // below the horizon
  float dist = 300;
};

Vec3 forwardOf(float yaw, float pitch) {
  return {std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}

float wrapAngle(float a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}

struct Popout {
  int number = 2;  // "View 2": the main window is the first
  std::unique_ptr<platform::Window> win;
  uint32_t surface = 0;
  render::RenderTarget target;
  int targetSamples = -1;
  Orbit cur, goal;
  float fov = 60;
  bool camKnown = false;
  Ref<Object> scene;  // the scene its camera was placed in (a new map places it again)
  bool keys[256] = {};
  bool panning = false, turning = false;
  int lastX = 0, lastY = 0;
  bool closing = false;
};

std::vector<std::unique_ptr<Popout>> g_pops;
std::vector<platform::InputEvent> g_injected;  // tests: events for the first pop-out
std::string g_shot;
std::chrono::steady_clock::time_point g_last;
int g_autoOpen = -1;  // REGS_POPOUT=<n>: that many open by themselves once a map is shown

struct Place {
  int x = 0, y = 0, w = 0, h = 0;
  bool fullscreen = false, known = false;
};
std::vector<Place> g_places;  // by pop-out number − 2, from <user folder>\popouts.cfg
bool g_placesLoaded = false;

std::filesystem::path placesFile() {
  return host().userFolder.empty() ? std::filesystem::path() : host().userFolder / L"popouts.cfg";
}

void loadPlaces() {
  if (g_placesLoaded) return;
  g_placesLoaded = true;
  std::filesystem::path f = placesFile();
  if (f.empty()) return;
  std::ifstream in(f);
  Place p;
  int fs = 0;
  while (in >> p.x >> p.y >> p.w >> p.h >> fs) {
    p.fullscreen = fs != 0;
    p.known = p.w >= 160 && p.h >= 90;
    g_places.push_back(p);
  }
}

void savePlaces() {
  std::filesystem::path f = placesFile();
  if (f.empty() || g_places.empty()) return;
  std::ofstream out(f);
  out << "# OpenCiM pop-out views: outer left top, client width height, fullscreen (one line per view, View 2 first)\n";
  for (const Place& p : g_places) out << p.x << ' ' << p.y << ' ' << p.w << ' ' << p.h << ' ' << (p.fullscreen ? 1 : 0) << '\n';
}

void rememberPlace(const Popout& p) {
  if (!p.win) return;
  Place pl;
  p.win->placement(pl.x, pl.y, pl.w, pl.h);
  pl.fullscreen = p.win->fullscreen();
  pl.known = true;
  size_t i = size_t(p.number - 2);
  if (g_places.size() <= i) g_places.resize(i + 1);
  g_places[i] = pl;
}

std::u16string titleFor(VM& vm, int number) {
  std::u16string word = u"View";
  if (Value* main = vm.findGlobal(u"MAIN")) {
    Value lang = vm.getMember(*main, u"$language");
    if (lang.type() == VT::String && lang.str().rfind(u"de", 0) == 0) word = u"Ansicht";
  }
  std::u16string n;
  for (char c : std::to_string(number)) n += char16_t(c);
  return u"OpenCiM – " + word + u" " + n;
}

float fovOf(Object* cam) {
  if (cam)
    if (auto it = cam->fieldValues.find(u"fov"); it != cam->fieldValues.end()) {
      const Value& v = it->second;
      if (v.isNumber()) {
        float f = v.type() == VT::Float ? v.f() : float(v.i());
        if (f > 5 && f < 150) return f;
      }
    }
  return 60.0f;
}

// the camera placed where the game camera looks from, as it looks
void placeFromMain(VM& vm, Popout& p) {
  Object* cam = lastMapCamera();
  if (!cam) return;
  Vec3 eye, look, up;
  cameraFrame(vm, cam, eye, look, up);
  Vec3 pivot = cameraPivot(vm, cam);
  Vec3 dir = normalize(look - eye);
  Vec3 off = eye - pivot;
  Orbit o;
  o.focus = pivot;
  o.dist = std::clamp(std::sqrt(dot(off, off)), kMinDist, kMaxDist);
  o.pitch = std::clamp(std::asin(std::clamp(-dir.y, -1.0f, 1.0f)), kMinPitch, kMaxPitch);
  o.yaw = std::atan2(dir.x, dir.z);
  p.cur = p.goal = o;
  p.fov = fovOf(cam);
  p.camKnown = true;
}

bool popoutOpenAt(VM& vm, const Place* at) {
  Display* d = display();
  if (!d || !render::gpu::hasDevice()) return false;
  if (!lastMapViewport()) {
    LOG_INFO("popout: no map shown, nothing to open");
    return false;
  }
  loadPlaces();
  auto p = std::make_unique<Popout>();
  for (int n = 2;; ++n) {  // the lowest free number, so a closed view's place comes back with its number
    bool used = false;
    for (auto& o : g_pops) used |= o->number == n;
    if (!used) {
      p->number = n;
      break;
    }
  }
  const bool vulkan = std::string(render::gpu::dev().name()) == "Vulkan";
  platform::WindowDesc desc;
  desc.title = titleFor(vm, p->number);
  desc.resizable = true;
  desc.secondary = true;
  desc.vsync = false;
  desc.openGL = !vulkan;
  desc.shareGL = vulkan ? nullptr : &d->window();
  Place pl;
  size_t slot = size_t(p->number - 2);
  if (at) pl = *at;
  else if (slot < g_places.size() && g_places[slot].known &&
           platform::onSomeMonitor(g_places[slot].x, g_places[slot].y, g_places[slot].w, g_places[slot].h))
    pl = g_places[slot];
  if (pl.known || at) {
    desc.x = pl.x;
    desc.y = pl.y;
    desc.width = pl.w;
    desc.height = pl.h;
  } else {
    // first time: the middle of another monitor at 75 % of it when there is one, else beside the game's window
    int mx, my, mw, mh;
    const bool other = platform::otherMonitorWorkArea(&d->window(), mx, my, mw, mh);
    desc.width = other ? mw * 3 / 4 : std::min(960, mw / 2);
    desc.height = other ? mh * 3 / 4 : std::min(540, mh / 2);
    const int step = 32 * int(g_pops.size());
    desc.x = mx + (mw - desc.width) / 2 + step;
    desc.y = my + (mh - desc.height) / 2 + step;
    if (!other) desc.x = mx + mw - desc.width - 40 - step, desc.y = my + 40 + step;
  }
  p->win = platform::createWindow();
  std::string err;
  if (!p->win->create(desc, &err)) {
    LOG_ERROR("popout: the window could not be made: %s", err.c_str());
    return false;
  }
  p->surface = render::gpu::dev().addWindow(*p->win);
  if (!p->surface) {
    LOG_ERROR("popout: the graphics device cannot show pictures in a second window");
    return false;
  }
  if (pl.known && pl.fullscreen) p->win->setMode(true, pl.w, pl.h);
  placeFromMain(vm, *p);
  if (Object* s = viewportScene(vm, lastMapViewport())) p->scene = Ref<Object>(s);
  LOG_INFO("popout: view %d opened, %dx%d%s (%s)", p->number, p->win->width(), p->win->height(),
           p->win->fullscreen() ? " fullscreen" : "", vulkan ? "own swapchain" : "shared context");
  g_pops.push_back(std::move(p));
  return true;
}

void closePopout(Popout& p) {
  rememberPlace(p);
  savePlaces();
  if (render::gpu::hasDevice()) {
    if (p.surface) render::gpu::dev().removeWindow(p.surface);
    if (Display* d = display()) d->r3d().destroyTarget(p.target);
  }
  p.surface = 0;
  LOG_INFO("popout: view %d closed", p.number);
  p.win.reset();
}

void toggleFullscreen(Popout& p) {
  int x, y, w, h;
  p.win->placement(x, y, w, h);
  p.win->setMode(!p.win->fullscreen(), w, h);
  rememberPlace(p);
}

// one event of a pop-out's window
void handleEvent(VM& vm, Popout& p, const platform::InputEvent& e) {
  using T = platform::InputEvent::Type;
  const int h = std::max(1, p.win->height());
  // world units per pixel at the point it looks at (grabbing the ground: it follows the pointer)
  const float upp = 2.0f * p.cur.dist * std::tan(p.fov * kPi / 360.0f) / float(h);
  switch (e.type) {
    case T::Close: p.closing = true; break;
    case T::KeyDown:
      if (e.key == kVkReturn && e.system) {  // Alt + Enter
        if (!e.repeat) toggleFullscreen(p);
        break;
      }
      if (e.system) break;
      if (e.key >= 0 && e.key < 256) p.keys[e.key] = true;
      if (e.repeat) break;
      if (e.key == kVkF11) toggleFullscreen(p);
      else if (e.key == kVkEscape && p.win->fullscreen()) toggleFullscreen(p);
      else if (e.key == kVkHome) placeFromMain(vm, p);
      else if (e.key == kVkF7) popoutOpen(vm);
      break;
    case T::KeyUp:
      if (e.key >= 0 && e.key < 256) p.keys[e.key] = false;
      break;
    case T::Deactivate:
      std::fill(std::begin(p.keys), std::end(p.keys), false);
      p.panning = p.turning = false;
      break;
    case T::MouseDown:
      p.lastX = e.x, p.lastY = e.y;
      if (e.button == 0 || e.button == 2) p.panning = true;
      if (e.button == 1) p.turning = true;
      break;
    case T::MouseUp:
      if (e.button == 0 || e.button == 2) p.panning = false;
      if (e.button == 1) p.turning = false;
      break;
    case T::MouseMove: {
      const float dx = float(e.x - p.lastX), dy = float(e.y - p.lastY);
      p.lastX = e.x, p.lastY = e.y;
      if (p.turning) {  // right drag: turn about the point and tilt
        p.goal.yaw = p.cur.yaw = wrapAngle(p.cur.yaw - dx * 0.006f);
        p.goal.pitch = p.cur.pitch = std::clamp(p.cur.pitch + dy * 0.004f, kMinPitch, kMaxPitch);
      } else if (p.panning) {  // left or middle drag: the ground follows the pointer
        const Vec3 fwd{std::sin(p.cur.yaw), 0, std::cos(p.cur.yaw)};
        const Vec3 right{-fwd.z, 0, fwd.x};  // cross(forward, up): the screen's right
        const float along = upp / std::max(0.25f, std::sin(p.cur.pitch));
        const Vec3 move = right * (-dx * upp) + fwd * (dy * along);
        p.cur.focus = p.cur.focus + move;
        p.goal.focus = p.goal.focus + move;
      }
      break;
    }
    case T::MouseWheel:  // nearer away from the user, the game's 1.2 per notch, eased
      p.goal.dist = std::clamp(p.goal.dist * std::pow(1.2f, -e.wheel), kMinDist, kMaxDist);
      break;
    case T::MouseHWheel: p.goal.yaw = wrapAngle(p.goal.yaw - e.wheel * kPi / 16.0f); break;
    case T::Gesture:
      if (e.gesture == 1 && e.value > 0) p.goal.dist = std::clamp(p.goal.dist / e.value, kMinDist, kMaxDist);
      if (e.gesture == 2) p.goal.yaw = wrapAngle(p.goal.yaw + e.value);
      break;
    default: break;
  }
}

// held keys move the goals; the camera eases toward them
void stepCamera(Popout& p, float dt) {
  auto held = [&](int a, int b = -1) { return p.keys[a] || (b >= 0 && p.keys[b]); };
  const float fast = held(kVkShift) ? 2.5f : 1.0f;
  float mx = 0, mz = 0;
  if (held(kVkA, kVkLeft)) mx -= 1;
  if (held(kVkD, kVkRight)) mx += 1;
  if (held(kVkW, kVkUp)) mz += 1;
  if (held(kVkS, kVkDown)) mz -= 1;
  if (mx != 0 || mz != 0) {
    const Vec3 fwd{std::sin(p.goal.yaw), 0, std::cos(p.goal.yaw)};
    const Vec3 right{-fwd.z, 0, fwd.x};
    const float speed = p.goal.dist * 1.1f * fast * dt;  // about the view's width in two seconds
    p.goal.focus = p.goal.focus + right * (mx * speed) + fwd * (mz * speed);
  }
  if (held(kVkQ)) p.goal.yaw = wrapAngle(p.goal.yaw + 1.4f * fast * dt);
  if (held(kVkE)) p.goal.yaw = wrapAngle(p.goal.yaw - 1.4f * fast * dt);
  if (held(kVkR, kVkPrior)) p.goal.pitch = std::clamp(p.goal.pitch + 0.8f * dt, kMinPitch, kMaxPitch);
  if (held(kVkF, kVkNext)) p.goal.pitch = std::clamp(p.goal.pitch - 0.8f * dt, kMinPitch, kMaxPitch);
  if (held(kVkAdd, kVkOemPlus)) p.goal.dist = std::clamp(p.goal.dist * std::pow(0.35f, dt * fast), kMinDist, kMaxDist);
  if (held(kVkSubtract, kVkOemMinus)) p.goal.dist = std::clamp(p.goal.dist * std::pow(2.8f, dt * fast), kMinDist, kMaxDist);
  const float k = 1.0f - std::exp(-dt * 12.0f);
  p.cur.focus = p.cur.focus + (p.goal.focus - p.cur.focus) * k;
  p.cur.yaw = wrapAngle(p.cur.yaw + wrapAngle(p.goal.yaw - p.cur.yaw) * k);
  p.cur.pitch += (p.goal.pitch - p.cur.pitch) * k;
  p.cur.dist = std::exp(std::log(p.cur.dist) + (std::log(p.goal.dist) - std::log(p.cur.dist)) * k);
}

bool ensureTarget(Display& d, Popout& p, int w, int h) {
  const int samples = std::max(0, d.antialiasing);
  if (p.target.fbo && p.target.width == w && p.target.height == h && p.targetSamples == samples) return true;
  d.r3d().destroyTarget(p.target);
  p.target = {};
  for (int n = samples; n >= 0; n -= 2) {  // MSAA steps down by 2 like the engine's targets
    if (d.r3d().makeSceneTarget(p.target, w, h, n, false, render::TargetFormat::Rgba8)) {
      p.targetSamples = samples;
      return true;
    }
    d.r3d().destroyTarget(p.target);
    p.target = {};
    if (n == 0) break;
  }
  p.targetSamples = -1;
  return false;
}

}  // namespace

bool popoutOpen(VM& vm) { return popoutOpenAt(vm, nullptr); }

int popoutCount() { return int(g_pops.size()); }

void popoutInject(const platform::InputEvent& e) { g_injected.push_back(e); }
void popoutScreenshot(const std::string& file) { g_shot = file; }

void popoutsPump(VM& vm, const std::vector<platform::InputEvent>& mainInput, bool scripted) {
  using T = platform::InputEvent::Type;
  if (g_autoOpen < 0) {
    g_autoOpen = 0;
    if (const char* n = std::getenv("REGS_POPOUT")) g_autoOpen = std::clamp(std::atoi(n), 0, 8);
  }
  if (g_autoOpen > 0 && lastMapViewport()) {
    while (g_autoOpen > 0) {
      --g_autoOpen;
      Place at;
      const char* r = std::getenv("REGS_POPOUT_RECT");  // x,y,w,h for the first (tests)
      if (r && g_pops.empty() && std::sscanf(r, "%d,%d,%d,%d", &at.x, &at.y, &at.w, &at.h) == 4) {
        at.known = true;
        popoutOpenAt(vm, &at);
      } else {
        popoutOpen(vm);
      }
    }
  }
  if (!scripted)  // F7 in the game window: one more view (while a map is shown)
    for (const auto& e : mainInput)
      if (e.type == T::KeyDown && e.key == kVkF7 && !e.repeat && !e.system) popoutOpen(vm);
  std::vector<platform::InputEvent> events;
  for (size_t i = 0; i < g_pops.size(); ++i) {
    Popout& p = *g_pops[i];
    events.clear();
    if (p.win) p.win->pump(events);
    if (scripted)
      std::erase_if(events, [](const platform::InputEvent& e) { return e.type != T::Close && e.type != T::Resize; });
    if (i == 0) {
      events.insert(events.end(), g_injected.begin(), g_injected.end());
      g_injected.clear();
    }
    for (const auto& e : events) handleEvent(vm, p, e);
  }
  for (auto it = g_pops.begin(); it != g_pops.end();) {
    if ((*it)->closing) {
      closePopout(**it);
      it = g_pops.erase(it);
    } else {
      ++it;
    }
  }
}

void popoutsRender(Display& d, VM& vm) {
  const auto now = std::chrono::steady_clock::now();
  float dt = g_last.time_since_epoch().count() ? std::chrono::duration<float>(now - g_last).count() : 0.016f;
  g_last = now;
  dt = std::clamp(dt, 0.0f, 0.1f);
  if (g_pops.empty()) return;
  ControlObj* vp = lastMapViewport();
  Object* scene = viewportScene(vm, vp);
  const int bw = d.window().width(), bh = d.window().height();
  for (auto& pp : g_pops) {
    Popout& p = *pp;
    if (!p.win || !p.surface || p.win->minimized()) continue;
    const int w = p.win->width(), h = p.win->height();
    if (w <= 0 || h <= 0) continue;
    if (scene && (!p.camKnown || p.scene.get() != scene)) {  // a new map: start where the game camera is
      placeFromMain(vm, p);
      p.scene = Ref<Object>(scene);
    }
    stepCamera(p, dt);
    if (!ensureTarget(d, p, w, h)) continue;
    if (vp && p.camKnown) {
      ViewOverride ov;
      const Vec3 fwd = forwardOf(p.cur.yaw, p.cur.pitch);
      ov.look = p.cur.focus;
      ov.eye = p.cur.focus - fwd * p.cur.dist;
      ov.up = {0, 1, 0};
      ov.fov = p.fov;
      ov.distance = p.cur.dist;
      ov.target = &p.target;
      ov.w = w;
      ov.h = h;
      renderViewportScene(d, vm, vp, &ov);
    } else {  // the menus (no map): a dark picture until a game is shown again
      d.r3d().begin(&p.target, 0, 0, w, h, 0xFF141820u, true, bw, bh);
      d.r3d().end(bw, bh);
    }
    d.r3d().resolve(p.target);
    if (!g_shot.empty() && &p == g_pops.front().get()) {
      d.saveTargetPng(p.target, w, h, g_shot);
      LOG_INFO("popout screenshot: %s", g_shot.c_str());
      g_shot.clear();
    }
    render::gpu::dev().showInWindow(p.surface, p.target.fbo, w, h);
  }
  d.r2d().resume();
}

void popoutsShutdown() {
  for (auto& p : g_pops) closePopout(*p);
  g_pops.clear();
}

void registerPopoutNatives(VM& vm) {
  // regsPopoutOpen(): one more pop-out view of the map (F7, the button beside the minimap); false without a map
  implementGlobal(vm, u"regsPopoutOpen", [](VM& v, Object*, int, const Value*) { return Value::boolean(popoutOpen(v)); }, 0, 0);
  // regsPopoutCount(): the pop-out views open now
  implementGlobal(vm, u"regsPopoutCount", [](VM&, Object*, int, const Value*) { return Value::integer(popoutCount()); }, 0, 0);
}

}  // namespace regs::engine
