#include "core/text.h"

namespace regs {

std::string utf16ToUtf8(std::u16string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    }
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | (c >> 18));
      out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

std::u16string utf8ToUtf16(std::string_view s) {
  std::u16string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    uint8_t b = uint8_t(s[i]);
    uint32_t c;
    int n;
    if (b < 0x80) { c = b; n = 1; }
    else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; n = 2; }
    else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; n = 3; }
    else if ((b & 0xF8) == 0xF0) { c = b & 0x07; n = 4; }
    else { out += char16_t(0xFFFD); ++i; continue; }
    if (i + n > s.size()) { out += char16_t(0xFFFD); break; }
    for (int k = 1; k < n; ++k) c = (c << 6) | (uint8_t(s[i + k]) & 0x3F);
    i += n;
    if (c >= 0x10000) {
      c -= 0x10000;
      out += char16_t(0xD800 + (c >> 10));
      out += char16_t(0xDC00 + (c & 0x3FF));
    } else {
      out += char16_t(c);
    }
  }
  return out;
}

char16_t cp1252ToUtf16(uint8_t b) {
  static const char16_t kC1[32] = {
      0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
      0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
  return (b >= 0x80 && b < 0xA0) ? kC1[b - 0x80] : char16_t(b);
}

std::string cp1252ToUtf8(std::string_view s) {
  std::u16string w;
  w.reserve(s.size());
  for (char c : s) w += cp1252ToUtf16(uint8_t(c));
  return utf16ToUtf8(w);
}

std::u16string decodeTextFile(std::span<const uint8_t> b) {
  std::u16string out;
  if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
    out.reserve((b.size() - 2) / 2);
    for (size_t i = 2; i + 1 < b.size(); i += 2) out += char16_t(b[i] | (b[i + 1] << 8));
    return out;
  }
  if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
    return utf8ToUtf16(std::string_view(reinterpret_cast<const char*>(b.data() + 3), b.size() - 3));
  }
  // 8-bit files: one byte per code unit, zero-extended (Latin-1), as gsParser::parseSource does; the fonts carry
  // glyphs for the cp1252 typographic characters at those code units (docs/ui/fonts.md §5.3)
  out.reserve(b.size());
  for (uint8_t c : b) out += char16_t(c);
  return out;
}

}  // namespace regs
