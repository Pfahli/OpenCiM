// Destination display texts (engine/sign_atlas.h).
#include "engine/sign_atlas.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "core/log.h"
#include "engine/font.h"
#include "render/gpu.h"
#include "render/image.h"
#include "script/vm.h"
#include "vfs/vfs.h"

namespace regs::engine {

namespace {

// a font's page as coverage (alpha, or brightness when the page has no alpha)
struct Page {
  std::vector<uint8_t> cov;
  int w = 0, h = 0;
};

bool loadPage(script::VM& vm, const std::string& dds, Page& out) {
  std::vector<uint8_t> file;
  render::Image img;
  if (!vm.vfs().read(dds, file) || !render::parseDds(file, img, nullptr)) return false;
  std::vector<uint8_t> rgba;
  if (!render::decodeRgba(img, 0, rgba, out.w, out.h)) return false;
  // the glyph's fill: brightness times alpha (Tahoma's glyphs are white with a dark outline for the UI; Blair is white
  // on black without alpha)
  out.cov.resize(size_t(out.w) * size_t(out.h));
  for (size_t i = 0; i < out.cov.size(); ++i) {
    int lum = (rgba[i * 4] * 30 + rgba[i * 4 + 1] * 59 + rgba[i * 4 + 2] * 11) / 100;
    out.cov[i] = uint8_t(lum * rgba[i * 4 + 3] / 255);
  }
  return true;
}

struct FontPx {
  Font* font = nullptr;
  Page page;
  bool ok = false;
};

FontPx& fontPx(script::VM& vm, const char16_t* name, const char* dds) {
  static std::map<std::u16string, FontPx> fonts;
  FontPx& f = fonts[name];
  if (!f.font) {
    f.font = getFont(vm, name, u"");
    f.ok = f.font && !f.font->glyphs.empty() && loadPage(vm, dds, f.page);
    if (!f.ok) LOG_WARN("signs: font %s not usable", dds);
  }
  return f;
}

// draws the text into rows [top, top + rows) of a strip, glyphs at one texel per page pixel, the line's height
// centred; returns the width in texels. threshold > 0: on/off (LED dots)
float drawText(const FontPx& f, const std::u16string& text, std::vector<uint8_t>& strip, int top, int rows, int threshold,
               float advAdjust = 0) {
  if (!f.ok) return 0;
  const float pw = float(f.page.w), ph = float(f.page.h);
  float lineH = f.font->L * ph;
  float y0 = float(top) + std::floor((float(rows) - lineH) * 0.5f);
  float pen = 1;
  for (char16_t c : text) {
    const Glyph* g = f.font->glyph(c);
    if (!g) continue;
    int gx = int(std::lround(g->u * pw)), gy = int(std::lround(g->v * ph));
    int gw = int(std::lround(g->w * pw)), gh = int(std::lround(g->h * ph));
    int ox = int(std::lround(g->xoff * pw)), oy = int(std::lround(g->yoff * ph));
    for (int y = 0; y < gh; ++y)
      for (int x = 0; x < gw; ++x) {
        int sx = gx + x, sy = gy + y;
        if (sx < 0 || sy < 0 || sx >= f.page.w || sy >= f.page.h) continue;
        int dx = int(pen) + ox + x, dy = int(y0) + oy + y;
        if (dx < 0 || dx >= kSignAtlasW || dy < top || dy >= top + rows) continue;
        uint8_t v = f.page.cov[size_t(sy) * size_t(f.page.w) + size_t(sx)];
        if (threshold > 0) v = v >= threshold ? 255 : 0;
        uint8_t& d = strip[size_t(dy) * kSignAtlasW + size_t(dx)];
        d = std::max(d, v);
      }
    pen += std::max(1.0f, std::round(g->adv * pw) + advAdjust);
  }
  return std::min(pen + 1, float(kSignAtlasW));
}

struct Atlas {
  uint32_t tex = 0;
  std::map<std::u16string, SignText> texts;
  int used = 0;
};

Atlas& atlas() {
  static Atlas a;
  return a;
}

}  // namespace

uint32_t signAtlasTexture() { return atlas().tex; }

SignText signText(script::VM& vm, const std::u16string& text) {
  Atlas& a = atlas();
  if (text.empty() || !render::gpu::hasDevice()) return {};
  if (auto it = a.texts.find(text); it != a.texts.end()) return it->second;
  if (a.used >= kSignSlots) return {};
  using namespace render::gpu;
  if (!a.tex) {
    a.tex = dev().createTexture(TexKind::Tex2D, 1);
    SamplerDesc sd;
    sd.minFilter = sd.magFilter = Filter::Linear;
    sd.wrapS = sd.wrapT = Wrap::ClampToEdge;
    sd.maxLevel = 0;
    dev().setSampler(a.tex, sd);
    std::vector<uint8_t> zero(size_t(kSignAtlasW) * size_t(kSignStripH) * kSignSlots, 0);
    dev().texImage(a.tex, 0, 0, Format::R8, kSignAtlasW, kSignStripH * kSignSlots, Pixels::R8, zero.data());
  }
  std::vector<uint8_t> strip(size_t(kSignAtlasW) * kSignStripH, 0);
  SignText t;
  t.slot = a.used++;
  t.ledWidth = drawText(fontPx(vm, u"/metro/fonts/tahoma-8", "metro/fonts/tahoma-8/normal.dds"), text, strip, 0, kSignLedRows, 110, -2.0f);  // (the UI outline's two pixels off the advance)
  t.printWidth = drawText(fontPx(vm, u"/metro/fonts/blair-18", "metro/fonts/blair-18/normal.dds"), text, strip, kSignPrintTop,
                          kSignPrintRows, 0);
  dev().texSubImage(a.tex, 0, 0, t.slot * kSignStripH, kSignAtlasW, kSignStripH, Pixels::R8, strip.data());
  a.texts[text] = t;
  return t;
}

}  // namespace regs::engine
