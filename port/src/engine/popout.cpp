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
#include <cstdlib>
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

// (never destroyed by the C runtime at exit, whose order is not the engine's: popoutsShutdown empties it in time)
std::vector<std::unique_ptr<Popout>>& g_pops = *new std::vector<std::unique_ptr<Popout>>();
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
  desc.borderless = true;  // like the setup program: no title bar, a close button of its own (drawChrome)
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
    case T::ToggleFullscreen: toggleFullscreen(p); break;  // the borderless window's own button
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

// the borderless window's buttons, drawn like the setup program's close button (a dark disc with a white symbol and a
// lighter ring): kind 0 close (a cross; red under the pointer), 1 fullscreen (four corners pointing out), 2 back to a
// window (four corners pointing in; blue under the pointer); state 0 plain, 1 under the pointer, 2 held. Made at the
// size they are drawn at; straight alpha, B G R A.
struct ButtonImages {
  int size = 0;
  uint32_t tex[9] = {};  // kind * 3 + state
};
ButtonImages g_buttons;

uint32_t buttonTexture(Display& d, int size, int kind, int state) {
  if (g_buttons.size != size) {
    for (uint32_t& t : g_buttons.tex)
      if (t) d.r2d().destroyTexture(t), t = 0;
    g_buttons.size = size;
  }
  uint32_t& slot = g_buttons.tex[kind * 3 + state];
  if (slot) return slot;
  struct Look {
    uint32_t fill;
    int alpha;
    uint32_t ring;
  };
  const Look plain{0x1e1a16, 200, 0x7a6656};
  const Look red[2] = {{0xc0392b, 235, 0xf08a6a}, {0x962d22, 245, 0xd0705a}};
  const Look blue[2] = {{0x3d6ea8, 235, 0x8fb8e8}, {0x2c5486, 245, 0x7aa4d4}};
  const Look& lk = state == 0 ? plain : kind == 0 ? red[state - 1] : blue[state - 1];
  const float k = float(size) / 32.0f;  // the setup program draws it 32 pixels wide
  const float c = float(size) * 0.5f, r = c - 0.5f, half = std::max(1.0f, 1.0f * k);
  // the symbol as strokes (from, to) in the button's units about its centre
  struct Seg {
    float x0, y0, x1, y1;
  };
  std::vector<Seg> segs;
  if (kind == 0) {
    const float a = 5.5f;
    segs = {{-a, -a, a, a}, {a, -a, -a, a}};
  } else {
    for (int sx = -1; sx <= 1; sx += 2)
      for (int sy = -1; sy <= 1; sy += 2) {
        const float fx = float(sx), fy = float(sy);
        if (kind == 1) {  // corners at the outside, arms inward
          const float a = 6.0f, l = 3.5f;
          segs.push_back({fx * a, fy * a, fx * (a - l), fy * a});
          segs.push_back({fx * a, fy * a, fx * a, fy * (a - l)});
        } else {  // corners near the middle, arms outward
          const float a = 2.0f, l = 3.5f;
          segs.push_back({fx * a, fy * a, fx * (a + l), fy * a});
          segs.push_back({fx * a, fy * a, fx * a, fy * (a + l)});
        }
      }
  }
  std::vector<uint8_t> px(size_t(size) * size_t(size) * 4);
  auto cover = [](float dist) { return std::clamp(0.5f - dist, 0.0f, 1.0f); };  // 1 inside, a pixel's ramp
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      const float fx = float(x) + 0.5f - c, fy = float(y) + 0.5f - c;
      const float dc = std::sqrt(fx * fx + fy * fy);
      const float disc = cover(dc - r);
      const float ring = cover(std::fabs(dc - (r - 0.5f * k)) - 0.5f * std::max(1.0f, k));
      float symbol = 0;  // round-capped strokes
      for (const Seg& s : segs) {
        const float ax = s.x0 * k, ay = s.y0 * k, bx = s.x1 * k, by = s.y1 * k;
        const float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
        const float t = len2 > 0 ? std::clamp(((fx - ax) * vx + (fy - ay) * vy) / len2, 0.0f, 1.0f) : 0.0f;
        const float qx = fx - (ax + t * vx), qy = fy - (ay + t * vy);
        symbol = std::max(symbol, cover(std::sqrt(qx * qx + qy * qy) - half));
      }
      float rr = float(lk.fill >> 16 & 0xFF), gg = float(lk.fill >> 8 & 0xFF), bb = float(lk.fill & 0xFF);
      float a = float(lk.alpha) / 255.0f;
      auto mix = [&](uint32_t col, float t, float alphaTo) {
        rr += (float(col >> 16 & 0xFF) - rr) * t, gg += (float(col >> 8 & 0xFF) - gg) * t, bb += (float(col & 0xFF) - bb) * t;
        a += (alphaTo - a) * t;
      };
      mix(lk.ring, ring, 1.0f);
      mix(0xFFFFFF, symbol, 1.0f);
      a *= disc;
      uint8_t* o = &px[(size_t(y) * size_t(size) + size_t(x)) * 4];
      o[0] = uint8_t(bb), o[1] = uint8_t(gg), o[2] = uint8_t(rr), o[3] = uint8_t(std::lround(a * 255.0f));
    }
  return slot = d.r2d().createTextureBGRA(px.data(), size, size);
}

// the borderless window's chrome over the picture: a shade along the top while the pointer is on the band that moves
// the window (drag it there), the fullscreen button and the close button (the window's round corners and thin edge
// are the system's, engine-side nothing)
void drawChrome(Display& d, Popout& p, int w, int h) {
  if (!p.win->borderless()) return;
  float cx, cy, cs, band;
  p.win->chrome(cx, cy, cs, band);
  const int hot = p.win->chromeHot();
  auto& r = d.r2d();
  r.beginTarget(w, h);
  render::gpu::dev().bindFramebuffer(p.target.fbo);
  r.setProjection(0, 0, float(w), float(h), float(w), float(h));
  auto quad = [&](uint32_t tex, float x, float y, float qw, float qh, uint32_t argb) {
    const uint32_t c = render::argbToRgba(argb);
    const render::GuiVertex v[4] = {{x, y, 0, 0, c}, {x + qw, y, 1, 0, c}, {x + qw, y + qh, 1, 1, c}, {x, y + qh, 0, 1, c}};
    r.draw(tex, render::Prim::Quads, v, 4);
  };
  if (hot == 3 && band > 0) quad(0, 0, 0, float(w), band, 0x50000000u);
  const int size = std::max(8, int(cs));
  const int fullState = hot == 5 ? 2 : hot == 4 ? 1 : 0, closeState = hot == 2 ? 2 : hot == 1 ? 1 : 0;
  quad(buttonTexture(d, size, p.win->fullscreen() ? 2 : 1, fullState), cx - p.win->chromeGap(), cy, float(size),
       float(size), 0xFFFFFFFFu);
  quad(buttonTexture(d, size, 0, closeState), cx, cy, float(size), float(size), 0xFFFFFFFFu);
  r.endTarget();
  render::gpu::dev().bindFramebuffer(0);
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

void popoutTestMessage(const std::string& spec) {
  if (g_pops.empty() || !g_pops.front()->win) return;
  platform::Window& w = *g_pops.front()->win;
  unsigned long msg = 0, wp = 0;
  long x = 0, y = 0;
  char* e = nullptr;
  const char* s = spec.c_str();
  msg = std::strtoul(s, &e, 0);
  if (*e == ',') wp = std::strtoul(e + 1, &e, 0);
  if (*e == ',') x = std::strtol(e + 1, &e, 0);
  if (*e == ',') y = std::strtol(e + 1, &e, 0);
  w.testMessage(unsigned(msg), uintptr_t(wp), intptr_t((uint32_t(y) << 16) | (uint32_t(x) & 0xFFFF)));
  int px, py, pw, ph;
  w.placement(px, py, pw, ph);
  LOG_INFO("popout test: message 0x%lx at %ld,%ld: window %dx%d at %d,%d, %s, pointer on %d", msg, x, y, w.width(), w.height(),
           px, py, w.fullscreen() ? "fullscreen" : w.maximized() ? "maximised" : "windowed", w.chromeHot());
}
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
      std::erase_if(events, [](const platform::InputEvent& e) {
        return e.type != T::Close && e.type != T::Resize && e.type != T::ToggleFullscreen;  // (the window's own buttons stay)
      });
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
    drawChrome(d, p, w, h);
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
  if (Display* d = display(); d && render::gpu::hasDevice())
    for (uint32_t& t : g_buttons.tex)
      if (t) d->r2d().destroyTexture(t), t = 0;
  g_buttons.size = 0;
}

void registerPopoutNatives(VM& vm) {
  // regsPopoutOpen(): one more pop-out view of the map (F7, the button beside the minimap); false without a map
  implementGlobal(vm, u"regsPopoutOpen", [](VM& v, Object*, int, const Value*) { return Value::boolean(popoutOpen(v)); }, 0, 0);
  // regsPopoutCount(): the pop-out views open now
  implementGlobal(vm, u"regsPopoutCount", [](VM&, Object*, int, const Value*) { return Value::integer(popoutCount()); }, 0, 0);
}

}  // namespace regs::engine
