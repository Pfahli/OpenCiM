// Fonts and text metrics, following docs/ui/fonts.md: native .font chunk files and BMFont 3 extras, the font cache,
// gsFont_OGL::renderText and the gsGuiText_OGL measuring functions. Measurements reproduce the x87 code: each
// expression is evaluated in double from float inputs and rounded to float where the engine stores a float.
#include "engine/font.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/gui.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

namespace {

// cp1252 cells also reachable by their Unicode code point (§3)
char16_t alias(uint32_t id) {
  switch (id) {
    case 0x85: return 0x2026;
    case 0x8C: return 0x0152;
    case 0x92: return 0x2019;
    case 0x93: return 0x201C;
    case 0x94: return 0x201D;
    case 0x96: return 0x2013;
    case 0x9C: return 0x0153;
    default: return 0;
  }
}

std::string withoutSlash(std::string p) {
  while (!p.empty() && (p[0] == '/' || p[0] == '\\')) p.erase(0, 1);
  return p;
}

bool exists(VM& vm, const std::string& path) {
  std::vector<uint8_t> d;
  return vm.vfs().read(path, d);
}

uint32_t u32(const std::vector<uint8_t>& d, size_t at) {
  uint32_t v = 0;
  if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
  return v;
}
int16_t s16(const std::vector<uint8_t>& d, size_t at) {
  int16_t v = 0;
  if (at + 2 <= d.size()) std::memcpy(&v, d.data() + at, 2);
  return v;
}
uint16_t u16(const std::vector<uint8_t>& d, size_t at) { return uint16_t(s16(d, at)); }

float f32(double v) { return float(v); }

int hexDigit(char16_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

}  // namespace

const Glyph* Font::glyph(char16_t c) const {
  auto it = glyphs.find(c);
  if (it != glyphs.end()) return &it->second;
  return glyphs.empty() ? nullptr : &glyphs.begin()->second;
}

bool Font::load(VM& vm, const std::string& path) {
  std::vector<uint8_t> d;
  if (!vm.vfs().read(path, d) || d.size() < 8) return false;
  if (u32(d, 0) == 0x03464D42) {  // "BMF\3" (readBMFdata, §4)
    std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
    size_t pagesBefore = pages.size();
    float sx = 1, sy = 1, yAdj = 0, advPad = 0;
    int pageCount = 0;
    for (size_t at = 4; at + 5 <= d.size();) {
      uint8_t type = d[at];
      int32_t size = int32_t(u32(d, at + 1));
      size_t body = at + 5;
      if (type == 2) {
        uint16_t lineHeight = u16(d, body), scaleW = u16(d, body + 4), scaleH = u16(d, body + 6);
        pageCount = u16(d, body + 8);
        sx = 1.0f / float(scaleW);
        sy = 1.0f / float(scaleH);
        if (L >= 0) {
          yAdj = f32(double(sy) * std::trunc((double(L) / double(sy) - double(lineHeight)) * 0.5));
        } else {
          L = f32(double(lineHeight) * double(sy));
          yAdj = 0;
        }
        advPad = f32(double(lineHeight / 20 + 1) * double(sx));
      } else if (type == 3) {
        size_t p = body;
        for (int k = 0; k < pageCount && p < body + size_t(size); ++k) {
          std::string name;
          while (p < d.size() && d[p]) name += char(d[p++]);
          ++p;
          pages.push_back(getTexture(vm, utf8ToUtf16(dir + name)));
        }
      } else if (type == 4) {
        for (size_t r = body; r + 20 <= body + size_t(size) && r + 20 <= d.size(); r += 20) {
          uint32_t id = u32(d, r);
          Glyph g;
          g.u = float(s16(d, r + 4)) * sx;
          g.v = float(s16(d, r + 6)) * sy;
          g.w = float(s16(d, r + 8)) * sx;
          g.h = float(s16(d, r + 10)) * sy;
          g.xoff = float(s16(d, r + 12)) * sx;
          g.yoff = f32(double(float(s16(d, r + 14)) * sy) + double(yAdj));
          g.adv = f32(double(float(s16(d, r + 16)) * sx) + double(advPad));
          g.lh = L;
          g.page = int(d[r + 18]) + int(pagesBefore);
          glyphs[char16_t(id)] = g;  // 16-bit wchar_t key: id -1 becomes U+FFFF (Windows)
          if (char16_t a = alias(id)) glyphs[a] = g;
        }
        break;  // reading stops after the chars block
      }
      at = body + size_t(std::max(0, size));
    }
    return true;
  }
  if (u32(d, 0) != 0x03000000) return false;  // native chunk file (§3)
  std::string dds = path.substr(0, path.find_last_of('.')) + ".dds";
  pages.push_back(getTexture(vm, utf8ToUtf16(dds)));
  int page = int(pages.size()) - 1;
  size_t end = std::min(d.size(), size_t(8) + u32(d, 4));
  for (size_t at = 8; at + 8 <= end;) {
    uint32_t tag = u32(d, at), size = u32(d, at + 4);
    size_t body = at + 8;
    if ((tag == 0x03010000 || tag == 0x03020000) && body + 256 * 16 <= d.size()) {
      bool fixed = tag == 0x03020000;
      auto val = [&](size_t o) {
        if (fixed) return float(int32_t(u32(d, o))) * (1.0f / 1024.0f);
        float f;
        std::memcpy(&f, d.data() + o, 4);
        return f;
      };
      if (L < 0) L = val(body + 12);
      for (uint32_t i = 0; i < 256; ++i) {
        size_t o = body + i * 16;
        Glyph g;
        g.u = val(o);
        g.v = val(o + 4);
        g.w = val(o + 8);
        g.h = val(o + 12);
        g.adv = g.w;
        g.lh = L;
        g.page = page;
        glyphs[char16_t(i)] = g;
        if (char16_t a = alias(i)) glyphs[a] = g;
      }
    }
    at = body + size;
  }
  return true;
}

Font* getFont(VM& vm, const std::u16string& name, const std::u16string& extra) {
  static std::unordered_map<std::u16string, std::unique_ptr<Font>> cache;
  auto& slot = cache[name];
  std::string base = withoutSlash(utf16ToUtf8(name));
  if (!slot) {
    slot = std::make_unique<Font>();
    std::string fnt = base + "/normal.fnt";
    if (!slot->load(vm, exists(vm, fnt) ? fnt : base + "/normal.font"))
      LOG_WARN("font not found: %s", utf16ToUtf8(name).c_str());
  }
  if (!extra.empty() && !slot->extras.count(extra)) {
    std::string e = base + "/" + utf16ToUtf8(extra);
    if (exists(vm, e + ".fnt") && slot->load(vm, e + ".fnt")) slot->extras.insert(extra);
    else if (exists(vm, e + ".font") && slot->load(vm, e + ".font")) slot->extras.insert(extra);
  }
  return slot.get();
}

float charAdvance(const Font& f, char16_t c, const TextParams& p) {
  if (c == 4 || c == 5 || c == 10 || c == 13) return 0;
  const Glyph* g = f.glyph(c);
  return g ? f32(double(g->adv) / double(g->lh) * double(p.sizeX) + double(p.spacingX)) : 0.0f;
}

float measureWidth(const Font& f, const std::u16string& s, const TextParams& p) {
  float best = 0, cur = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char16_t c = s[i];
    if (c == 4) { i += 8; continue; }
    if (c == 5 || c == 13) continue;
    if (c == 10) { best = std::max(best, cur); cur = 0; continue; }
    const Glyph* g = f.glyph(c);
    if (!g) continue;
    cur = f32(double(g->adv) / double(g->lh) * double(p.sizeX) + double(p.spacingX) + double(cur));
  }
  return std::max(best, cur);
}

float measureHeight(const Font& f, const std::u16string& s, const TextParams& p) {
  float h = p.sizeY, x = 0, tab = 0;
  auto newline = [&] {
    h = f32(double(p.spacingY) + double(p.sizeY) + double(h));
    x = 0;
    tab = 0;
  };
  auto adv = [&](const Glyph* g) { return f32(double(g->adv) / double(g->lh) * double(p.sizeX) + double(p.spacingX)); };
  for (size_t i = 0; i < s.size(); ++i) {
    char16_t c = s[i];
    if (c == 4) { i += 8; continue; }
    if (c == 5 || c == 13) continue;
    if (c == 10) { newline(); continue; }
    if (c == 9) {
      tab = f32((double(p.spacingX) + double(p.sizeX)) * 3 + double(tab));
      x = tab;
      continue;
    }
    const Glyph* g = f.glyph(c);
    if (!g) continue;
    if (c == 32) {
      float w = adv(g), xs = f32(double(w) + double(x));
      double look = xs;
      for (size_t j = i + 1; j < s.size(); ++j) {
        char16_t d = s[j];
        if (d == 4) { j += 8; continue; }
        if (d == 5) continue;
        const Glyph* gd = f.glyph(d);
        if (d <= 0x20 || !gd || gd->lh == 0 || look > p.boxX) break;
        look = f32(double(gd->adv) / double(gd->lh) * double(p.sizeX) + double(p.spacingX) + look);
      }
      if (look <= p.boxX) x = xs;
      else newline();
      continue;
    }
    if (g->lh <= 0) continue;
    float w = adv(g);
    if (double(w) + double(x) > p.boxX) newline();
    x = f32(double(w) + double(x));
  }
  return h;
}

void layoutText(const Font& f, const std::u16string& s, const TextParams& p, float x0, float y0, uint32_t colour,
                uint32_t bgColour, bool inlineColours, const std::function<void(const TextQuad&)>& emit,
                const std::function<int(int page)>& pageHeight) {
  // glyph space: one unit per line; all lengths divided by size.x (gsGuiText_OGL::render)
  const double k = 1.0 / p.sizeX, wrapW = p.boxX * k, spX = p.spacingX * k, spY = p.spacingY * k;
  double pen = p.indent * k, tabAcc = 0, line = 0;
  uint32_t cur = colour;
  auto newline = [&] {
    pen = 0;
    line += spY + 1;
    tabAcc = 0;
  };
  auto quad = [&](int page, double gx0, double gy0, double gx1, double gy1, float u0, float v0, float u1, float v1, uint32_t argb) {
    TextQuad q;
    q.page = page;
    float X0 = x0 + float((pen + gx0) * p.sizeX), X1 = x0 + float((pen + gx1) * p.sizeX);
    float Y0 = y0 + float((line + gy0) * p.sizeY), Y1 = y0 + float((line + gy1) * p.sizeY);
    q.x[0] = X0; q.y[0] = Y0; q.u[0] = u0; q.v[0] = v0;
    q.x[1] = X0; q.y[1] = Y1; q.u[1] = u0; q.v[1] = v1;
    q.x[2] = X1; q.y[2] = Y1; q.u[2] = u1; q.v[2] = v1;
    q.x[3] = X1; q.y[3] = Y0; q.u[3] = u1; q.v[3] = v0;
    q.argb = argb;
    emit(q);
  };
  for (size_t i = 0; i < s.size();) {
    char16_t c = s[i];
    switch (c) {
      case 4:
        if (inlineColours && i + 8 < s.size()) {
          // Windows parses only the first digit of each pair (byte = digit * 17, fonts.md §7.2)
          uint32_t kc = 0;
          for (int n = 0; n < 4; ++n) kc = kc << 8 | uint32_t(hexDigit(s[i + 1 + 2 * n]) * 17);
          cur = (kc & 0x00FFFFFFu) | ((kc >> 24) * (colour >> 24) >> 8) << 24;
        }
        i += 9;
        continue;
      case 5:
        if (inlineColours) cur = colour;
        ++i;
        continue;
      case 9:
        tabAcc = (spX + 1) * 3 + tabAcc;
        pen = tabAcc;
        ++i;
        continue;
      case 10:
        newline();
        ++i;
        continue;
      case 13:
        ++i;
        continue;
      case 32: {
        const Glyph* g = f.glyph(32);
        if (!g) { ++i; continue; }
        double a = double(g->adv) / g->lh + spX, look = pen + a, save = look;
        for (size_t j = i + 1; j < s.size(); ++j) {
          char16_t d = s[j];
          if (d == 4) { j += 8; continue; }
          if (d == 5) continue;
          const Glyph* h = f.glyph(d);
          if (d <= 0x20 || !h || h->lh == 0 || look > wrapW) break;
          look = double(h->adv) / h->lh + look + spX;
        }
        if (look <= wrapW) pen = save;
        else newline();
        ++i;
        continue;
      }
      default: break;
    }
    const Glyph* g = f.glyph(c);
    if (!g || g->lh <= 0) { ++i; continue; }
    double a = double(g->adv) / g->lh + spX;
    if (pen + a > wrapW) newline();
    if (bgColour >> 24) quad(-1, 0, 0, a, 1, 0, 0, 0, 0, bgColour);
    int T = std::max(1, pageHeight(g->page));
    double pd = 1.0 / T, q = 1.0 / (double(T) * g->lh);
    quad(g->page, g->xoff / g->lh - q, g->yoff / g->lh - q, (g->xoff + g->w) / g->lh + q, (g->yoff + g->h) / g->lh + q,
         float(g->u - pd), float(g->v - pd), float(g->u + g->w + pd), float(g->v + g->h + pd), cur);
    pen += a;
    ++i;
  }
}

}  // namespace regs::engine
