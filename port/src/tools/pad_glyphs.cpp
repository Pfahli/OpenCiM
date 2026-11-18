// regs-tool padglyphs <out dir>: the controller button glyphs of the controls page's legend (port/docs/controller.md),
// drawn here at build time so no artwork ships: Xbox, PlayStation, Steam Deck and Steam Controller face buttons,
// shoulders, triggers, sticks, system buttons, touchpads, back grips and the d-pad. Shapes are antialiased distance
// fields, letters come from GDI (Segoe UI Bold, antialiased); each glyph is an uncompressed 32-bit DDS, 48 pixels high
// (shown at 24 by the page, so UI scale 2x stays sharp), 48 or 96 wide.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kH = 48;

struct Canvas {
  int w;
  std::vector<float> px;  // premultiplied RGBA
  explicit Canvas(int width) : w(width), px(size_t(width) * kH * 4, 0.0f) {}
  // "over" with coverage a of colour c
  void blend(int x, int y, uint32_t c, float a) {
    if (a <= 0) return;
    a = std::min(1.0f, a) * float((c >> 24) & 0xFF) / 255.0f;
    float* p = &px[(size_t(y) * w + x) * 4];
    const float r = float((c >> 16) & 0xFF) / 255.0f, g = float((c >> 8) & 0xFF) / 255.0f, b = float(c & 0xFF) / 255.0f;
    p[0] = r * a + p[0] * (1 - a);
    p[1] = g * a + p[1] * (1 - a);
    p[2] = b * a + p[2] * (1 - a);
    p[3] = a + p[3] * (1 - a);
  }
  // fills where the signed distance d(x, y) (pixels, negative inside) is below zero, antialiased over a pixel
  template <class F>
  void fill(uint32_t c, F d) {
    for (int y = 0; y < kH; ++y)
      for (int x = 0; x < w; ++x) blend(x, y, c, std::clamp(0.5f - d(float(x) + 0.5f, float(y) + 0.5f), 0.0f, 1.0f));
  }
};

float roundRect(float x, float y, float x0, float y0, float x1, float y1, float r) {
  const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
  const float qx = std::fabs(x - cx) - hx, qy = std::fabs(y - cy) - hy;
  return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}
float segment(float x, float y, float ax, float ay, float bx, float by) {
  const float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
  const float t = std::clamp((px * dx + py * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
  return std::hypot(px - dx * t, py - dy * t);
}

uint32_t lighter(uint32_t c, float k) {
  auto ch = [&](int s) { return uint32_t(std::min(255.0f, float((c >> s) & 0xFF) * k + 255.0f * (k - 1) * 0.3f)); };
  return (c & 0xFF000000) | ch(16) << 16 | ch(8) << 8 | ch(0);
}

// a text label centred (GDI coverage, then coloured)
void text(Canvas& cv, const std::wstring& s, uint32_t color, int height) {
  if (s.empty()) return;
  HDC dc = CreateCompatibleDC(nullptr);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = cv.w;
  bi.bmiHeader.biHeight = -kH;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);
  std::fill_n(static_cast<uint32_t*>(bits), size_t(cv.w) * kH, 0u);
  HFONT font = CreateFontW(-height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  HGDIOBJ oldFont = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  RECT r{0, 0, cv.w, kH};
  DrawTextW(dc, s.c_str(), int(s.size()), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  GdiFlush();
  const auto* p = static_cast<const uint32_t*>(bits);
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < cv.w; ++x) cv.blend(x, y, color, float((p[size_t(y) * cv.w + x] >> 8) & 0xFF) / 255.0f);
  SelectObject(dc, oldFont);
  DeleteObject(font);
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

bool writeDds(const fs::path& out, const Canvas& cv) {
  std::vector<uint8_t> file(128, 0);
  auto put = [&](size_t at, uint32_t v) {
    for (int k = 0; k < 4; ++k) file[at + size_t(k)] = uint8_t(v >> (8 * k));
  };
  file[0] = 'D', file[1] = 'D', file[2] = 'S', file[3] = ' ';
  put(4, 124);
  put(8, 0x1 | 0x2 | 0x4 | 0x1000);  // CAPS | HEIGHT | WIDTH | PIXELFORMAT
  put(12, uint32_t(kH));
  put(16, uint32_t(cv.w));
  put(76, 32);
  put(80, 0x40 | 0x1);  // RGB | ALPHAPIXELS
  put(88, 32);
  put(92, 0x00FF0000), put(96, 0x0000FF00), put(100, 0x000000FF), put(104, 0xFF000000);
  put(108, 0x1000);
  for (size_t i = 0; i < size_t(cv.w) * kH; ++i) {
    const float* p = &cv.px[i * 4];
    const float a = p[3], inv = a > 0 ? 1.0f / a : 0.0f;  // straight alpha in the file
    auto c = [&](float v) { return uint8_t(std::lround(std::clamp(v * inv, 0.0f, 1.0f) * 255)); };
    file.insert(file.end(), {c(p[2]), c(p[1]), c(p[0]), uint8_t(std::lround(a * 255))});
  }
  FILE* f = _wfopen(out.wstring().c_str(), L"wb");
  const bool ok = f && std::fwrite(file.data(), 1, file.size(), f) == file.size();
  if (f) std::fclose(f);
  return ok;
}

// base shapes
enum Shape { kCircle, kRound, kPad, kRoundPad, kNone };
// icons drawn over the shape
enum Icon { kNoIcon, kCross, kRing, kSquare, kTriangle, kView, kMenu, kBack, kStart, kDUp, kDDown, kDLeft, kDRight, kDLR, kDUD };

struct Glyph {
  const char* name;
  int w;
  Shape shape;
  uint32_t fill;
  const wchar_t* label;
  uint32_t textColor;
  Icon icon;
  uint32_t iconColor;
};

constexpr uint32_t kDark = 0xFF2A2C30, kWhite = 0xFFF2F2F2, kGrey = 0xFF8A8D93;
constexpr uint32_t kXGreen = 0xFF6DBE45, kXRed = 0xFFE0453B, kXBlue = 0xFF3B8EE0, kXYellow = 0xFFF2C230;

const Glyph kGlyphs[] = {
    // Xbox
    {"xb_a", 48, kCircle, kDark, L"A", kXGreen, kNoIcon, 0},
    {"xb_b", 48, kCircle, kDark, L"B", kXRed, kNoIcon, 0},
    {"xb_x", 48, kCircle, kDark, L"X", kXBlue, kNoIcon, 0},
    {"xb_y", 48, kCircle, kDark, L"Y", kXYellow, kNoIcon, 0},
    {"xb_lb", 48, kRound, kDark, L"LB", kWhite, kNoIcon, 0},
    {"xb_rb", 48, kRound, kDark, L"RB", kWhite, kNoIcon, 0},
    {"xb_lt", 48, kRound, kDark, L"LT", kWhite, kNoIcon, 0},
    {"xb_rt", 48, kRound, kDark, L"RT", kWhite, kNoIcon, 0},
    {"xb_ls", 48, kCircle, kDark, L"LS", kWhite, kNoIcon, 0},
    {"xb_rs", 48, kCircle, kDark, L"RS", kWhite, kNoIcon, 0},
    {"xb_view", 48, kCircle, kDark, L"", 0, kView, kWhite},
    {"xb_menu", 48, kCircle, kDark, L"", 0, kMenu, kWhite},
    // PlayStation
    {"ps_cross", 48, kCircle, kDark, L"", 0, kCross, 0xFF8CC4FF},
    {"ps_circle", 48, kCircle, kDark, L"", 0, kRing, 0xFFFF7070},
    {"ps_square", 48, kCircle, kDark, L"", 0, kSquare, 0xFFF29AE0},
    {"ps_triangle", 48, kCircle, kDark, L"", 0, kTriangle, 0xFF47D6A6},
    {"ps_l1", 48, kRound, kDark, L"L1", kWhite, kNoIcon, 0},
    {"ps_r1", 48, kRound, kDark, L"R1", kWhite, kNoIcon, 0},
    {"ps_l2", 48, kRound, kDark, L"L2", kWhite, kNoIcon, 0},
    {"ps_r2", 48, kRound, kDark, L"R2", kWhite, kNoIcon, 0},
    {"ps_ls", 48, kCircle, kDark, L"L", kWhite, kNoIcon, 0},
    {"ps_rs", 48, kCircle, kDark, L"R", kWhite, kNoIcon, 0},
    {"ps_l3", 48, kCircle, kDark, L"L3", kWhite, kNoIcon, 0},
    {"ps_r3", 48, kCircle, kDark, L"R3", kWhite, kNoIcon, 0},
    {"ps_create", 96, kRound, kDark, L"Create", kWhite, kNoIcon, 0},
    {"ps_options", 96, kRound, kDark, L"Options", kWhite, kNoIcon, 0},
    {"ps_touchpad", 96, kPad, kDark, L"Touchpad", kWhite, kNoIcon, 0},
    // Steam Deck (dark buttons, white letters)
    {"deck_a", 48, kCircle, kDark, L"A", kWhite, kNoIcon, 0},
    {"deck_b", 48, kCircle, kDark, L"B", kWhite, kNoIcon, 0},
    {"deck_x", 48, kCircle, kDark, L"X", kWhite, kNoIcon, 0},
    {"deck_y", 48, kCircle, kDark, L"Y", kWhite, kNoIcon, 0},
    {"deck_l1", 48, kRound, kDark, L"L1", kWhite, kNoIcon, 0},
    {"deck_r1", 48, kRound, kDark, L"R1", kWhite, kNoIcon, 0},
    {"deck_l2", 48, kRound, kDark, L"L2", kWhite, kNoIcon, 0},
    {"deck_r2", 48, kRound, kDark, L"R2", kWhite, kNoIcon, 0},
    {"deck_l4", 48, kRound, 0xFF3A3D44, L"L4", kWhite, kNoIcon, 0},
    {"deck_r4", 48, kRound, 0xFF3A3D44, L"R4", kWhite, kNoIcon, 0},
    {"deck_l5", 48, kRound, 0xFF3A3D44, L"L5", kWhite, kNoIcon, 0},
    {"deck_r5", 48, kRound, 0xFF3A3D44, L"R5", kWhite, kNoIcon, 0},
    {"deck_ls", 48, kCircle, kDark, L"L", kWhite, kNoIcon, 0},
    {"deck_rs", 48, kCircle, kDark, L"R", kWhite, kNoIcon, 0},
    {"deck_l3", 48, kCircle, kDark, L"L3", kWhite, kNoIcon, 0},
    {"deck_r3", 48, kCircle, kDark, L"R3", kWhite, kNoIcon, 0},
    {"deck_view", 48, kCircle, kDark, L"", 0, kView, kWhite},
    {"deck_menu", 48, kCircle, kDark, L"", 0, kMenu, kWhite},
    {"deck_lpad", 48, kPad, kDark, L"L", kWhite, kNoIcon, 0},
    {"deck_rpad", 48, kPad, kDark, L"R", kWhite, kNoIcon, 0},
    // Steam Controller (round pads, Xbox-coloured letters, arrow buttons beside the Steam button)
    {"sc_a", 48, kCircle, kDark, L"A", kXGreen, kNoIcon, 0},
    {"sc_b", 48, kCircle, kDark, L"B", kXRed, kNoIcon, 0},
    {"sc_x", 48, kCircle, kDark, L"X", kXBlue, kNoIcon, 0},
    {"sc_y", 48, kCircle, kDark, L"Y", kXYellow, kNoIcon, 0},
    {"sc_lb", 48, kRound, kDark, L"LB", kWhite, kNoIcon, 0},
    {"sc_rb", 48, kRound, kDark, L"RB", kWhite, kNoIcon, 0},
    {"sc_lt", 48, kRound, kDark, L"LT", kWhite, kNoIcon, 0},
    {"sc_rt", 48, kRound, kDark, L"RT", kWhite, kNoIcon, 0},
    {"sc_lg", 48, kRound, 0xFF3A3D44, L"LG", kWhite, kNoIcon, 0},
    {"sc_rg", 48, kRound, 0xFF3A3D44, L"RG", kWhite, kNoIcon, 0},
    {"sc_stick", 48, kCircle, kDark, L"S", kWhite, kNoIcon, 0},
    {"sc_back", 48, kCircle, kDark, L"", 0, kBack, kWhite},
    {"sc_start", 48, kCircle, kDark, L"", 0, kStart, kWhite},
    {"sc_lpad", 48, kRoundPad, kDark, L"L", kWhite, kNoIcon, 0},
    {"sc_rpad", 48, kRoundPad, kDark, L"R", kWhite, kNoIcon, 0},
    // d-pad (all layouts)
    {"dpad_up", 48, kCircle, kDark, L"", 0, kDUp, kWhite},
    {"dpad_down", 48, kCircle, kDark, L"", 0, kDDown, kWhite},
    {"dpad_left", 48, kCircle, kDark, L"", 0, kDLeft, kWhite},
    {"dpad_right", 48, kCircle, kDark, L"", 0, kDRight, kWhite},
    {"dpad_lr", 48, kCircle, kDark, L"", 0, kDLR, kWhite},
    {"dpad_ud", 48, kCircle, kDark, L"", 0, kDUD, kWhite},
};

void draw(Canvas& cv, const Glyph& g) {
  const float W = float(cv.w), cx = W * 0.5f, cy = kH * 0.5f;
  switch (g.shape) {
    case kCircle:
      cv.fill(lighter(g.fill, 1.6f), [&](float x, float y) { return std::hypot(x - cx, y - cy) - 21.5f; });
      cv.fill(g.fill, [&](float x, float y) { return std::hypot(x - cx, y - cy) - 19.5f; });
      break;
    case kRound:
      cv.fill(lighter(g.fill, 1.6f), [&](float x, float y) { return roundRect(x, y, 2, 7, W - 2, kH - 7, 10); });
      cv.fill(g.fill, [&](float x, float y) { return roundRect(x, y, 4, 9, W - 4, kH - 9, 8); });
      break;
    case kPad:
      cv.fill(lighter(g.fill, 1.6f), [&](float x, float y) { return roundRect(x, y, 2, 2, W - 2, kH - 2, 9); });
      cv.fill(g.fill, [&](float x, float y) { return roundRect(x, y, 4, 4, W - 4, kH - 4, 7); });
      cv.fill(lighter(g.fill, 1.25f), [&](float x, float y) { return roundRect(x, y, 9, 9, W - 9, kH - 9, 5); });
      break;
    case kRoundPad:
      cv.fill(lighter(g.fill, 1.6f), [&](float x, float y) { return std::hypot(x - cx, y - cy) - 21.5f; });
      cv.fill(g.fill, [&](float x, float y) { return std::hypot(x - cx, y - cy) - 19.5f; });
      cv.fill(lighter(g.fill, 1.25f), [&](float x, float y) { return std::hypot(x - cx, y - cy) - 14.0f; });
      break;
    case kNone: break;
  }
  const uint32_t ic = g.iconColor;
  const float sw = 1.9f;  // half the stroke width
  switch (g.icon) {
    case kCross:
      cv.fill(ic, [&](float x, float y) {
        return std::min(segment(x, y, cx - 9, cy - 9, cx + 9, cy + 9), segment(x, y, cx - 9, cy + 9, cx + 9, cy - 9)) - sw;
      });
      break;
    case kRing: cv.fill(ic, [&](float x, float y) { return std::fabs(std::hypot(x - cx, y - cy) - 10.0f) - sw; }); break;
    case kSquare:
      cv.fill(ic, [&](float x, float y) { return std::fabs(roundRect(x, y, cx - 9, cy - 9, cx + 9, cy + 9, 1)) - sw; });
      break;
    case kTriangle:
      cv.fill(ic, [&](float x, float y) {
        const float ax = cx, ay = cy - 11, bx = cx + 10.5f, by = cy + 7, qx = cx - 10.5f, qy = cy + 7;
        return std::min({segment(x, y, ax, ay, bx, by), segment(x, y, bx, by, qx, qy), segment(x, y, qx, qy, ax, ay)}) - sw;
      });
      break;
    case kView:  // two overlapping windows
      cv.fill(ic, [&](float x, float y) { return std::fabs(roundRect(x, y, cx - 10, cy - 8, cx + 3, cy + 3, 1.5f)) - 1.4f; });
      cv.fill(ic, [&](float x, float y) { return roundRect(x, y, cx - 3, cy - 3, cx + 10, cy + 8, 1.5f); });
      break;
    case kMenu:  // three lines
      cv.fill(ic, [&](float x, float y) {
        return std::min({segment(x, y, cx - 9, cy - 7, cx + 9, cy - 7), segment(x, y, cx - 9, cy, cx + 9, cy),
                         segment(x, y, cx - 9, cy + 7, cx + 9, cy + 7)}) - 1.6f;
      });
      break;
    case kBack:
    case kStart: {  // a filled triangle pointing left (back) or right (start)
      const float k = g.icon == kBack ? -1.0f : 1.0f;
      const float ax = cx + k * 9, ay = cy, bx = cx - k * 7, by = cy - 10, qx = cx - k * 7, qy = cy + 10;
      cv.fill(ic, [&](float x, float y) {
        auto side = [&](float x0, float y0, float x1, float y1) { return (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0); };
        const float s1 = side(ax, ay, bx, by), s2 = side(bx, by, qx, qy), s3 = side(qx, qy, ax, ay);
        const bool inside = (s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0);
        const float e = std::min({segment(x, y, ax, ay, bx, by), segment(x, y, bx, by, qx, qy), segment(x, y, qx, qy, ax, ay)});
        return inside ? -e : e;
      });
      break;
    }
    case kDUp: case kDDown: case kDLeft: case kDRight: case kDLR: case kDUD: {
      auto arm = [&](int dir) {  // 0 up, 1 down, 2 left, 3 right
        return [=](float x, float y) {
          switch (dir) {
            case 0: return roundRect(x, y, cx - 5, cy - 16, cx + 5, cy - 2, 2);
            case 1: return roundRect(x, y, cx - 5, cy + 2, cx + 5, cy + 16, 2);
            case 2: return roundRect(x, y, cx - 16, cy - 5, cx - 2, cy + 5, 2);
            default: return roundRect(x, y, cx + 2, cy - 5, cx + 16, cy + 5, 2);
          }
        };
      };
      for (int a = 0; a < 4; ++a) cv.fill(kGrey, arm(a));
      cv.fill(kGrey, [&](float x, float y) { return roundRect(x, y, cx - 5, cy - 5, cx + 5, cy + 5, 1); });
      const bool lit[4] = {g.icon == kDUp || g.icon == kDUD, g.icon == kDDown || g.icon == kDUD,
                           g.icon == kDLeft || g.icon == kDLR, g.icon == kDRight || g.icon == kDLR};
      for (int a = 0; a < 4; ++a)
        if (lit[a]) cv.fill(ic, arm(a));
      break;
    }
    case kNoIcon: break;
  }
  const size_t n = std::wcslen(g.label);
  text(cv, g.label, g.textColor, n <= 1 ? 24 : n == 2 ? 19 : 16);
}

}  // namespace

int cmdPadGlyphs(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: regs-tool padglyphs <out dir>\n");
    return 1;
  }
  const fs::path dir = argv[2];
  std::error_code ec;
  fs::create_directories(dir, ec);
  int written = 0;
  for (const Glyph& g : kGlyphs) {
    Canvas cv(g.w);
    draw(cv, g);
    if (writeDds(dir / (std::string(g.name) + ".dds"), cv)) ++written;
  }
  const int total = int(sizeof kGlyphs / sizeof kGlyphs[0]);
  std::printf("padglyphs: wrote %d of %d glyphs to %s\n", written, total, dir.string().c_str());
  return written == total ? 0 : 1;
}
