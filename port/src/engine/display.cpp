// gsWindow_OGL / gsVideo_OGL back end: the OS window, the per-frame update/render/present order of
// gsWindow_OGL::tick (0x10057160) and the GUI pass (docs/renderer/interfaces-2d-output.md §1.5).
#include "engine/display.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/font.h"
#include "engine/gui.h"
#include "engine/host.h"
#include "engine/input.h"
#include "engine/interaction.h"
#include "engine/minimap.h"
#include "engine/scene3d.h"
#include "engine/settings.h"
#include "render/gl.h"
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

bool Display::open(const platform::WindowDesc& desc, std::string* error) {
  window_ = platform::createWindow();
  if (!window_->create(desc, error)) return false;
  auto proc = [](const char* n, void* u) { return static_cast<platform::Window*>(u)->glProc(n); };
  if (const char* missing = gl::load(proc, window_.get())) {
    if (error) *error = std::string("OpenGL function missing: ") + missing;
    return false;
  }
  if (!r2d_.init(error) || !r3d_.init(error)) return false;
  LOG_INFO("display: %dx%d, %s", window_->width(), window_->height(), r2d_.info.c_str());
  if (int dpi = window_->dpi()) LOG_INFO("display: %d dpi (%d %%), per-monitor DPI aware", dpi, dpi * 100 / 96);
  else LOG_WARN("display: not DPI aware, Windows may stretch the window");
  return true;
}

float Display::chooseUiScale(VM& vm, int w, int h) {
  // the graphics page's choices 1×, 1.5×, 2×; no setting: 2× on windows taller than 1800 px (4K-class screens)
  float want = settingNumber(vm, u"regsUiScale", h > 1800 ? 2.0f : 1.0f);
  float s = want >= 1.75f ? 2.0f : want >= 1.25f ? 1.5f : 1.0f;
  // the scripts' window stays at least 1024 × 720: the main menu's buttons run into its bottom links below about
  // 660 script pixels, and the graphics page is 1000 wide. Smaller scaled windows step down to the next scale
  while (s > 1.0f && (float(w) / s < 1024.0f || float(h) / s < 720.0f)) s -= 0.5f;
  if (s != want && settingNumber(vm, u"regsUiScale", 0) != 0)
    LOG_WARN("ui scale %.1f does not fit a %dx%d window, using %.1f", double(want), w, h, double(s));
  if (const char* f = std::getenv("REGS_UI_FILTER")) uiFilterSharp_ = std::string(f) != "linear";
  return uiScale = s;
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
  std::vector<uint8_t> data;
  render::Image img;
  std::string err;
  if (!vm.vfs().read(utf16ToUtf8(t->path), data) || !render::parseDds(data, img, &err)) {
    // "Invalid texture file": the cache hands out textures/default.dds instead (FUN_100431c0)
    if (!t->loadFailed) LOG_WARN("Invalid texture file: '%s' (%s)", utf16ToUtf8(t->path).c_str(), err.c_str());
    t->loadFailed = true;
    auto* def = dynamic_cast<TextureObj*>(getTexture(vm, u"textures/default.dds").get());
    return def && def != t && !def->loadFailed ? texture(vm, def) : 0;
  }
  r2d_.anisotropy = settingNumber(vm, u"anisotropy", 0);  // window.script: video.anisotropy = $settings.$anisotropy
  t->gl = r2d_.createTexture(img);
  t->cube = img.cube;
  return t->gl;
}

namespace {

// The GUI pass of one viewport (gsViewport_OGL::renderGui, docs/ui/controls.md §1): per control, its image and border
// in the owner's client area, then its text and children in its own client area (scissor = client rect ∩ owner's).
struct GuiPass {
  VM& vm;
  Display& d;
  float vpX = 0, vpY = 0, unit = 1;  // viewport origin (px) and pixels per GUI unit
  int sx = 0, sy = 0, sw = 0, sh = 0;  // current scissor in window pixels

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
    if (std::getenv("REGS_DEBUG_VIEWPORT") && c->width * c->height > 500000) {
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

  void control(ControlObj* c) {
    if (!c->visible()) return;
    if (drawMapFootprint(d, vm, c)) return;  // a line primitive, no rect of its own
    placeMapPin(vm, c);
    placeTracer(vm, c);
    if (c->clipped()) return;
    if (c->cls->name == "gsViewPort") renderViewportScene(d, vm, c);  // fills its render target first
    if (c->hasImage) image(c);
    if (c->hasBorder) border(c);
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
      if (auto* kc = dynamic_cast<ControlObj*>(k.get())) control(kc);
    scissor(px, py, pw, ph);
  }
};

}  // namespace

void Display::renderFrame(VM& vm, Object* win, Object* video) {
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
      // the viewport is its own client area: its children are drawn in it
      for (auto& c : vp->children)
        if (auto* cc = dynamic_cast<ControlObj*>(c.get())) pass.control(cc);
    }
  }
  drawFpsReadout(vm);
  r2d_.endFrame();
  if (!shot_.empty()) {
    if (saveScreenshot(shot_)) LOG_INFO("screenshot: %s", shot_.string().c_str());
    else LOG_ERROR("screenshot failed: %s", shot_.string().c_str());
    shot_.clear();
  }
  window_->swap();
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
  gl::BindFramebuffer(gl::FRAMEBUFFER, rt.fbo);
  gl::PixelStorei(gl::PACK_ALIGNMENT, 1);
  gl::ReadPixels(0, 0, rt.width, rt.height, gl::RGBA, gl::UNSIGNED_BYTE, px.data());
  gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
  std::ofstream f(file, std::ios::binary);
  if (f) writePng(f, px.data(), rt.width, rt.height);
}

void registerDisplayNatives(VM& vm) {
  if (ClassInfo* c = vm.findClass("gsWindow_OGL")) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new WindowObj(ci)); };
  // video.getEffectTimer() (0x1004e620 → gsI3DVideo slot 0x11c): the shaders' effect timer, for shader parameters
  // that start an effect "now" (achievement.script:196)
  implement(vm, "gsVideo_OGL", u"getEffectTimer", [](VM&, Object*, int, const Value*) {
    Display* d = display();
    return Value::real(d ? d->effectTimer : 0.0f);
  });
  // window.init(): gsWindow_OGL::start (0x10059200) creates the OS window and context from the fields
  implement(vm, "gsWindow_OGL", u"init", [](VM& v, Object* s, int, const Value*) {
    if (host().headless || g_display) return Value();
    platform::WindowDesc desc;
    desc.width = std::max(320, v.toInt(field(v, s, u"width")));
    desc.height = std::max(240, v.toInt(field(v, s, u"height")));
    desc.title = v.toStr(field(v, s, u"title"));
    desc.fullscreen = !host().forceWindowed && v.truthy(field(v, s, u"fullscreen"));
    desc.vsync = v.truthy(field(v, s, u"vsync"));
    // graphics engine (settings regsRenderer): 0 = OpenGL 3.3, the only one so far; others are planned
    if (int engine = int(settingNumber(v, u"regsRenderer", 0)); engine != 0)
      LOG_WARN("graphics engine %d is not available yet, using OpenGL 3.3", engine);
    // MSAA on the window only without post-processing; with it the effect buffers are multisampled instead
    if (!v.truthy(field(v, s, u"postProcess"))) desc.samples = std::max(0, v.toInt(field(v, s, u"antialiasing")));
    auto d = std::make_unique<Display>();
    d->antialiasing = std::max(0, v.toInt(field(v, s, u"antialiasing")));
    std::string err;
    if (!d->open(desc, &err)) {
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
