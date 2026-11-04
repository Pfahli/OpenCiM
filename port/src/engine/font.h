#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "script/vm.h"

namespace regs::engine {

class TextureObj;

// One glyph (gsCharData_OGL): all values in normalised texture units of its page (docs/ui/fonts.md §5.1).
struct Glyph {
  float u = 0, v = 0, w = 0, h = 0, xoff = 0, yoff = 0, adv = 0, lh = 0;
  int page = 0;
};

// gsFont_OGL: glyphs keyed by UTF-16 code unit, page textures and the font line height L.
class Font {
 public:
  std::map<char16_t, Glyph> glyphs;
  std::vector<script::Ref<script::Object>> pages;  // TextureObj
  std::set<std::u16string> extras;
  float L = -1.0f;

  // Exact key, else the glyph with the smallest key (getCharData, §5.2); null for a font without glyphs.
  const Glyph* glyph(char16_t c) const;
  bool load(script::VM& vm, const std::string& path);  // .fnt (BMFont 3) or .font (native chunk file)
};

// gsFonts_OGL::getFont(name, extra, 0): cached by name; an extra merges <name>/<extra>.fnt|.font once (§2).
Font* getFont(script::VM& vm, const std::u16string& name, const std::u16string& extra);

// gsGuiText_OGL parameters in screen units (§6).
struct TextParams {
  float sizeX = 0.015f, sizeY = 0.015f;
  float spacingX = 0, spacingY = 0;
  float boxX = 10000;  // wrap width
  float indent = 0;    // first-line pen offset
};

float measureWidth(const Font& f, const std::u16string& s, const TextParams& p);   // slot 14 (§8.2)
float measureHeight(const Font& f, const std::u16string& s, const TextParams& p);  // slot 15 (§8.3)
float charAdvance(const Font& f, char16_t c, const TextParams& p);                // slot 16 (§8.1)

// renderText (§7): emits glyph quads (texture page, 4 corners in screen units, ARGB colour) and untextured
// background quads (page -1). 'x0/y0' is the text position.
struct TextQuad {
  int page;  // -1 = untextured
  float x[4], y[4], u[4], v[4];
  uint32_t argb;
};
void layoutText(const Font& f, const std::u16string& s, const TextParams& p, float x0, float y0, uint32_t colour,
                uint32_t bgColour, bool inlineColours, const std::function<void(const TextQuad&)>& emit,
                const std::function<int(int page)>& pageHeight);

}  // namespace regs::engine
