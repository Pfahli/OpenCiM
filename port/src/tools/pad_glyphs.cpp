// regs-tool padglyphs <out dir>: the controller button glyphs of the controls page's legend (port/docs/controller.md),
// drawn here at build time so no artwork ships: Xbox, PlayStation, Steam Deck and Steam Controller face buttons,
// shoulders, triggers, sticks, system buttons, touchpads, back grips and the d-pad. Shapes are antialiased distance
// fields, letters come from GDI (Segoe UI Bold, antialiased); each glyph is an uncompressed 32-bit DDS, 48 pixels high
// (shown at 24 by the page, so UI scale 2x stays sharp), 48 or 96 wide. Also the controller pictures of the page's
// diagram (body_<layout>.dds, 720 x 440) with their label layouts (<layout>.layout).
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kH = 48;

struct Canvas {
  int w, h;
  std::vector<float> px;  // premultiplied RGBA
  explicit Canvas(int width, int height = kH) : w(width), h(height), px(size_t(width) * size_t(height) * 4, 0.0f) {}
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
    for (int y = 0; y < h; ++y)
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

// a text label centred in the rect (x0, y0)-(x1, y1) (GDI coverage, then coloured)
void textAt(Canvas& cv, const std::wstring& s, uint32_t color, int height, int x0, int y0, int x1, int y1) {
  if (s.empty()) return;
  const int kH = cv.h;
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
  RECT r{x0, y0, x1, y1};
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

void text(Canvas& cv, const std::wstring& s, uint32_t color, int height) { textAt(cv, s, color, height, 0, 0, cv.w, cv.h); }

bool writeDds(const fs::path& out, const Canvas& cv) {
  const int kH = cv.h;
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


// ---- controller pictures for the controls page's diagram (owner 2026-11-23: "look like an Xbox/PS5/Steam
// Controller, silhouette-wise with lines going to the buttons") ----
// Each picture is drawn at 720 × 440 (shown at half size, so UI scale 2x stays sharp): the controller's outline (from
// Kenney's icons, see below) with its sticks, pads, buttons and shoulders (no logos). Beside it goes <name>.layout, a
// script the page loads: where each labelled button is and the slot of its label in the column left or right of the
// picture.

// the whole diagram (twice the shown size) and the controller's place in its middle
constexpr int kDiaW = 1760, kDiaH = 544;
constexpr float kBodyX0 = 500, kBodyX1 = 1260;
constexpr uint32_t kBody = 0xFF3A3D44, kRim = 0xFF6C717B, kDeep = 0xFF1C1E22, kCap = 0xFF2C2F34, kCapRim = 0xFF585C65;
constexpr uint32_t kPadFill = 0xFF454951, kShoulder = 0xFF30333A;

float capsule(float x, float y, float ax, float ay, float bx, float by, float r) { return segment(x, y, ax, ay, bx, by) - r; }
float circle(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) - r; }

struct Anchor {
  std::string tag;  // the label's text: strings regs_pad_tag_<tag>
  float x, y;       // picture pixels
  float r = 0;      // a round button's radius: the leader ends at its edge (so its letter stays clear)
};

struct BodyPainter {
  Canvas& cv;
  std::vector<Anchor>& anchors;
  template <class F>
  void part(uint32_t fill, uint32_t rim, float rimW, F d) {
    if (rimW > 0) cv.fill(rim, [&](float x, float y) { return d(x, y) - rimW; });
    cv.fill(fill, d);
  }
  template <class F>
  void stroke(uint32_t c, float w, F d) {
    cv.fill(c, [&](float x, float y) { return std::fabs(d(x, y)) - w; });
  }
  void mark(const char* tag, float x, float y, float r = 0) { anchors.push_back({tag, x, y, r}); }
  void letter(const wchar_t* s, uint32_t color, int height, float x, float y) {
    textAt(cv, s, color, height, int(x - 30), int(y - 30), int(x + 30), int(y + 30));
  }
  void stick(float x, float y, float r, const char* tag) {
    part(kDeep, kRim, 2, [&](float px, float py) { return circle(px, py, x, y, r + 9); });
    part(kCap, kCapRim, 3, [&](float px, float py) { return circle(px, py, x, y, r); });
    stroke(0xFF3C4047, 1.5f, [&](float px, float py) { return circle(px, py, x, y, r * 0.62f); });
    if (tag) mark(tag, x, y);
  }
  void dpad(float x, float y, float s, const char* tag) {
    const float a = s * 0.36f, r = s * 0.12f;
    auto d = [&](float px, float py) {
      return std::min(roundRect(px, py, x - a, y - s, x + a, y + s, r), roundRect(px, py, x - s, y - a, x + s, y + a, r));
    };
    part(kDeep, kRim, 2, [&](float px, float py) { return d(px, py) - 5; });
    part(kCap, kCapRim, 2, d);
    if (tag) mark(tag, x, y);
  }
  void face(float x, float y, float r, const wchar_t* s, uint32_t color, const char* tag) {
    part(kCap, kCapRim, 2.5f, [&](float px, float py) { return circle(px, py, x, y, r); });
    letter(s, color, int(r * 1.25f), x, y);
    if (tag) mark(tag, x, y, r);
  }
  // PlayStation symbols: 0 cross, 1 circle, 2 square, 3 triangle
  void psFace(float x, float y, float r, int sym, uint32_t color, const char* tag) {
    part(kCap, kCapRim, 2.5f, [&](float px, float py) { return circle(px, py, x, y, r); });
    const float k = r * 0.48f, w = r * 0.09f;
    switch (sym) {
      case 0: cv.fill(color, [&](float px, float py) { return std::min(segment(px, py, x - k, y - k, x + k, y + k), segment(px, py, x - k, y + k, x + k, y - k)) - w; }); break;
      case 1: cv.fill(color, [&](float px, float py) { return std::fabs(circle(px, py, x, y, k * 1.05f)) - w; }); break;
      case 2: cv.fill(color, [&](float px, float py) { return std::fabs(roundRect(px, py, x - k, y - k, x + k, y + k, 1)) - w; }); break;
      default: {
        const float ax = x, ay = y - k * 1.15f, bx = x + k * 1.1f, by = y + k * 0.75f, qx = x - k * 1.1f, qy = by;
        cv.fill(color, [&](float px, float py) { return std::min({segment(px, py, ax, ay, bx, by), segment(px, py, bx, by, qx, qy), segment(px, py, qx, qy, ax, ay)}) - w; });
      }
    }
    if (tag) mark(tag, x, y, r);
  }
  // small round system buttons: 0 plain, 1 view (two windows), 2 menu (three lines), 3 arrow left, 4 arrow right
  void small(float x, float y, float r, int icon, const char* tag) {
    part(kCap, kCapRim, 2, [&](float px, float py) { return circle(px, py, x, y, r); });
    const float k = r * 0.5f;
    switch (icon) {
      case 1:
        stroke(kWhite, 0.9f, [&](float px, float py) { return roundRect(px, py, x - k, y - k * 0.8f, x + k * 0.3f, y + k * 0.3f, 1); });
        cv.fill(kWhite, [&](float px, float py) { return roundRect(px, py, x - k * 0.3f, y - k * 0.3f, x + k, y + k * 0.8f, 1); });
        break;
      case 2:
        cv.fill(kWhite, [&](float px, float py) {
          return std::min({segment(px, py, x - k, y - k * 0.6f, x + k, y - k * 0.6f), segment(px, py, x - k, y, x + k, y),
                           segment(px, py, x - k, y + k * 0.6f, x + k, y + k * 0.6f)}) - 1.0f;
        });
        break;
      case 3:
      case 4: {
        const float s = icon == 3 ? -1.0f : 1.0f;
        cv.fill(kWhite, [&](float px, float py) {
          return std::min(segment(px, py, x + s * k, y, x - s * k * 0.6f, y - k), segment(px, py, x + s * k, y, x - s * k * 0.6f, y + k)) - 1.2f;
        });
        break;
      }
      default: break;
    }
    if (tag) mark(tag, x, y, r);
  }
  void pill(float x0, float y0, float x1, float y1, const char* tag) {  // PlayStation's Create / Options
    part(kCap, kCapRim, 2, [&](float px, float py) { return capsule(px, py, x0, y0, x1, y1, 6); });
    if (tag) mark(tag, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
  }
  void shoulder(float x0, float y0, float x1, float y1, float r, uint32_t fill, const char* tag, float ax, float ay) {
    part(fill, kRim, 2.5f, [&](float px, float py) { return roundRect(px, py, x0, y0, x1, y1, r); });
    if (tag) mark(tag, ax, ay);
  }
};

// The outlines are Kenney's whole-controller icons (Input Prompts 1.5A, CC0; third_party/kenney-input-prompts): one
// path each on a 64-unit square (commands M, L and Q only, nonzero fill), the body with its buttons cut out. Drawn
// here at full size as a distance field, so the rim and the soft shadow follow the outline; the buttons go into the
// cut-outs, and the few the icons leave out (Xbox View / Menu, the Steam Controller's middle buttons) are added where
// the real controllers have them. The Steam Deck's icon has no sticks: its outline is used, the inside drawn here.

struct Pt {
  float x, y;
};
using Poly = std::vector<Pt>;

// the path of an icon as closed polygons (quadratic curves in 12 steps), in icon units; false when unreadable
bool loadIcon(const fs::path& file, std::vector<Poly>& out) {
  FILE* f = _wfopen(file.wstring().c_str(), L"rb");
  if (!f) return false;
  std::string s;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
  std::fclose(f);
  const size_t at = s.find(" d=\"");
  if (at == std::string::npos) return false;
  const size_t end = s.find('"', at + 4);
  const std::string d = s.substr(at + 4, end - at - 4);
  size_t i = 0;
  char cmd = 0;
  Pt pos{0, 0};
  auto number = [&](float& v) {
    while (i < d.size() && (d[i] == ' ' || d[i] == ',')) ++i;
    char* e = nullptr;
    v = std::strtof(d.c_str() + i, &e);
    if (e == d.c_str() + i) return false;
    i = size_t(e - d.c_str());
    return true;
  };
  while (i < d.size()) {
    while (i < d.size() && (d[i] == ' ' || d[i] == ',')) ++i;
    if (i >= d.size()) break;
    if (std::isalpha(static_cast<unsigned char>(d[i]))) {
      cmd = d[i++];
      continue;
    }
    if (cmd == 'M') {
      if (!number(pos.x) || !number(pos.y)) return false;
      out.push_back({pos});
      cmd = 'L';
    } else if (cmd == 'L') {
      if (out.empty() || !number(pos.x) || !number(pos.y)) return false;
      out.back().push_back(pos);
    } else if (cmd == 'Q') {
      Pt c, e;
      if (out.empty() || !number(c.x) || !number(c.y) || !number(e.x) || !number(e.y)) return false;
      for (int k = 1; k <= 12; ++k) {
        const float t = float(k) / 12.0f, u = 1 - t;
        out.back().push_back({u * u * pos.x + 2 * u * t * c.x + t * t * e.x, u * u * pos.y + 2 * u * t * c.y + t * t * e.y});
      }
      pos = e;
    } else {
      return false;  // a command these icons do not use
    }
  }
  return !out.empty();
}

float signedArea(const Poly& p) {
  float a = 0;
  for (size_t j = 0; j < p.size(); ++j) {
    const Pt& u = p[j];
    const Pt& v = p[(j + 1) % p.size()];
    a += u.x * v.y - v.x * u.y;
  }
  return a * 0.5f;
}

// a signed distance field of polygons (picture pixels, negative inside; nonzero winding), one value per pixel
struct Field {
  int w = 0, h = 0;
  std::vector<float> d;
  float operator()(float x, float y) const {
    const int ix = std::clamp(int(x), 0, w - 1), iy = std::clamp(int(y), 0, h - 1);
    return d[size_t(iy) * size_t(w) + size_t(ix)];
  }
};

Field field(const std::vector<Poly>& polys, int w, int h) {
  Field f{w, h, std::vector<float>(size_t(w) * size_t(h), 1e9f)};
  std::vector<std::pair<float, int>> xs;
  // only near the polygons (farther out nothing reads more than "outside")
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  for (const Poly& p : polys)
    for (const Pt& q : p) bx0 = std::min(bx0, q.x), by0 = std::min(by0, q.y), bx1 = std::max(bx1, q.x), by1 = std::max(by1, q.y);
  const int xa = std::max(0, int(bx0) - 24), xb = std::min(w, int(bx1) + 25);
  for (int y = std::max(0, int(by0) - 24); y < std::min(h, int(by1) + 25); ++y) {
    const float yc = float(y) + 0.5f;
    xs.clear();
    for (const Poly& p : polys)
      for (size_t j = 0; j < p.size(); ++j) {
        const Pt& a = p[j];
        const Pt& b = p[(j + 1) % p.size()];
        if ((a.y <= yc) != (b.y <= yc)) xs.push_back({a.x + (yc - a.y) * (b.x - a.x) / (b.y - a.y), b.y > a.y ? 1 : -1});
      }
    std::sort(xs.begin(), xs.end());
    size_t k = 0;
    int wind = 0;
    for (int x = xa; x < xb; ++x) {
      const float xc = float(x) + 0.5f;
      while (k < xs.size() && xs[k].first < xc) wind += xs[k++].second;
      float best = 1e9f;
      for (const Poly& p : polys)
        for (size_t j = 0; j < p.size(); ++j) {
          const Pt& a = p[j];
          const Pt& b = p[(j + 1) % p.size()];
          if (std::fabs(a.y - yc) > best && std::fabs(b.y - yc) > best && (a.y - yc) * (b.y - yc) > 0) continue;
          best = std::min(best, segment(xc, yc, a.x, a.y, b.x, b.y));
        }
      f.d[size_t(y) * size_t(w) + size_t(x)] = wind != 0 ? -best : best;
    }
  }
  return f;
}

// an icon placed in the picture: scaled to fit the middle (at most 17 pixels a unit), centred, room above it for the
// shoulders drawn behind it
struct Outline {
  std::vector<Poly> polys;  // picture pixels
  std::vector<Poly> units;  // icon units, as loaded
  float s = 1, ox = 0, oy = 0;
  Pt T(float u, float v) const { return {ox + u * s, oy + v * s}; }
  float R(float r) const { return r * s; }
  bool load(const fs::path& file) {
    if (!loadIcon(file, units)) return false;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const Poly& p : units)
      for (const Pt& q : p) x0 = std::min(x0, q.x), y0 = std::min(y0, q.y), x1 = std::max(x1, q.x), y1 = std::max(y1, q.y);
    s = std::min({(kBodyX1 - kBodyX0) / (x1 - x0), 420.0f / (y1 - y0), 17.0f});
    ox = (kBodyX0 + kBodyX1 - (x1 + x0) * s) * 0.5f;
    oy = (kDiaH + 56 - (y1 - y0) * s) * 0.5f - y0 * s;  // a little lower: the triggers and bumpers stand above it
    for (const Poly& p : units) {
      Poly q;
      for (const Pt& u : p) q.push_back(T(u.x, u.y));
      polys.push_back(q);
    }
    return true;
  }
  // the outline alone (the polygons winding the outer way) and the whole path with its cut-outs
  std::vector<Poly> outer() const {
    std::vector<Poly> o;
    for (const Poly& p : polys)
      if (signedArea(p) > 0) o.push_back(p);
    return o;
  }
  // the cut-out whose middle is nearest a point (icon units)
  std::vector<Poly> hole(float u, float v) const {
    size_t best = 0;
    float bd = 1e9f;
    for (size_t k = 0; k < units.size(); ++k) {
      if (signedArea(units[k]) > 0) continue;
      float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
      for (const Pt& q : units[k]) x0 = std::min(x0, q.x), y0 = std::min(y0, q.y), x1 = std::max(x1, q.x), y1 = std::max(y1, q.y);
      const float dd = std::hypot((x0 + x1) * 0.5f - u, (y0 + y1) * 0.5f - v);
      if (dd < bd) bd = dd, best = k;
    }
    return {polys[best]};
  }
};

// the body: shadow, the outline filled dark with its rim (the cut-outs show it as recesses), the body over it
void iconBody(BodyPainter& p, const Outline& ic, bool withHoles) {
  const Field outer = field(ic.outer(), kDiaW, kDiaH);
  p.cv.fill(0x50000000, [&](float x, float y) { return outer(x - 4, y - 7) - 2; });
  p.part(withHoles ? kDeep : kBody, kRim, 3, outer);
  if (withHoles) {
    const Field body = field(ic.polys, kDiaW, kDiaH);
    p.cv.fill(kBody, body);
  }
}

// the shoulders, both sides (x0..x1 in icon units of the left one, mirrored about the icon's middle, 32): the
// outlines have the bumpers as the bumps of their top edge (top = its height), so the bumper's label points there;
// behind it only the trigger peeks out, `peek` units high and a little narrower, as seen from the front. The Steam
// Deck's top edge is straight: `bumper` draws its bumpers as a thin bar along it.
void shoulders(BodyPainter& p, const Outline& ic, float x0, float x1, float top, float peek, bool bumper = false) {
  for (int s = 0; s < 2; ++s) {
    const bool r = s == 1;
    auto X = [&](float u) { return r ? 64 - u : u; };
    const float a = std::min(X(x0), X(x1)), b = std::max(X(x0), X(x1)), m = (a + b) * 0.5f;
    const float lift = bumper ? 0.9f : 0.0f;  // the Deck's bar raises the trigger by its height
    const Pt t0 = ic.T(a + 0.9f, top - lift - peek), t1 = ic.T(b - 0.9f, top + 1.5f);
    p.part(kShoulder, kRim, 2.5f, [&](float x, float y) { return roundRect(x, y, t0.x, t0.y, t1.x, t1.y, ic.R(1.1f)); });
    const Pt ta = ic.T(m, top - lift - peek * 0.55f);
    p.mark(r ? "zoomIn" : "zoomOut", ta.x, ta.y);
    if (bumper) {
      const Pt b0 = ic.T(a, top - lift), b1 = ic.T(b, top + 1.0f);
      p.part(0xFF383B42, kRim, 2.5f, [&](float x, float y) { return roundRect(x, y, b0.x, b0.y, b1.x, b1.y, ic.R(0.6f)); });
    }
    const Pt ba = ic.T(m, top + (bumper ? -lift * 0.5f : 0.7f));
    p.mark(r ? "pause" : "panHold", ba.x, ba.y);
  }
}

// a stick cap in a round cut-out (radius of the cut-out in units)
void capIn(BodyPainter& p, const Outline& ic, float u, float v, float r, const char* tag) {
  const Pt c = ic.T(u, v);
  const float cr = ic.R(r) - 5;
  p.part(kCap, kCapRim, 3, [&](float x, float y) { return circle(x, y, c.x, c.y, cr); });
  p.stroke(0xFF3C4047, 1.5f, [&](float x, float y) { return circle(x, y, c.x, c.y, cr * 0.62f); });
  if (tag) p.mark(tag, c.x, c.y);
}

void drawXbox(BodyPainter& p, const Outline& ic) {
  shoulders(p, ic, 17.0f, 24.3f, 15.0f, 2.0f);
  iconBody(p, ic, true);
  capIn(p, ic, 20.5f, 23.5f, 3.5f, "cursor");
  { const Pt c = ic.T(25.5f, 32.5f); p.dpad(c.x, c.y, ic.R(2.6f), "dpad"); }
  capIn(p, ic, 36.0f, 32.0f, 3.0f, "camera");
  { const Pt c = ic.T(43, 21); p.face(c.x, c.y, ic.R(1.7f), L"Y", kXYellow, "construct"); }
  { const Pt c = ic.T(39, 25); p.face(c.x, c.y, ic.R(1.7f), L"X", kXBlue, "bulldoze"); }
  { const Pt c = ic.T(47, 25); p.face(c.x, c.y, ic.R(1.7f), L"B", kXRed, "rclick"); }
  { const Pt c = ic.T(43, 29); p.face(c.x, c.y, ic.R(1.7f), L"A", kXGreen, "click"); }
  { const Pt c = ic.T(32, 20); p.part(0xFF32353B, kCapRim, 2, [&](float x, float y) { return circle(x, y, c.x, c.y, ic.R(2) - 4); }); }
  { const Pt c = ic.T(28.6f, 24.6f); p.small(c.x, c.y, ic.R(0.85f), 1, "underground"); }
  { const Pt c = ic.T(35.4f, 24.6f); p.small(c.x, c.y, ic.R(0.85f), 2, "esc"); }
}

void drawPlayStation(BodyPainter& p, const Outline& ic) {
  shoulders(p, ic, 13.8f, 20.9f, 16.05f, 2.0f);
  iconBody(p, ic, true);
  {  // the touchpad fills its cut-out
    const Field pad = field(ic.hole(32, 22.5f), kDiaW, kDiaH);
    p.part(kPadFill, kRim, 1.5f, [&](float x, float y) { return pad(x, y) + 4; });
    const Pt c = ic.T(32, 22.5f);
    p.mark("touchpad", c.x, c.y);
  }
  {  // the d-pad's four arms fill theirs
    std::vector<Poly> arms;
    for (Pt a : {Pt{21, 26}, Pt{18, 23}, Pt{18, 29}, Pt{15, 26}}) arms.push_back(ic.hole(a.x, a.y)[0]);
    const Field d = field(arms, kDiaW, kDiaH);
    p.part(kCap, kCapRim, 1.5f, [&](float x, float y) { return d(x, y) + 3; });
    const Pt c = ic.T(18, 26);
    p.mark("dpad", c.x, c.y);
  }
  capIn(p, ic, 25, 32, 3, "cursor");
  capIn(p, ic, 39, 32, 3, "camera");
  { const Pt c = ic.T(46.5f, 23.5f); p.psFace(c.x, c.y, ic.R(1.3f), 3, 0xFF47D6A6, "construct"); }
  { const Pt c = ic.T(43.5f, 26.5f); p.psFace(c.x, c.y, ic.R(1.3f), 2, 0xFFF29AE0, "bulldoze"); }
  { const Pt c = ic.T(49.5f, 26.5f); p.psFace(c.x, c.y, ic.R(1.3f), 1, 0xFFFF7070, "rclick"); }
  { const Pt c = ic.T(46.5f, 29.5f); p.psFace(c.x, c.y, ic.R(1.3f), 0, 0xFF8CC4FF, "click"); }
  { const Pt a = ic.T(22.6f, 18.8f), b = ic.T(23.0f, 20.8f); p.pill(a.x, a.y, b.x, b.y, "underground"); }
  { const Pt a = ic.T(41.4f, 18.8f), b = ic.T(41.0f, 20.8f); p.pill(a.x, a.y, b.x, b.y, "esc"); }
  { const Pt c = ic.T(32, 31); p.part(kCap, kCapRim, 2, [&](float x, float y) { return circle(x, y, c.x, c.y, ic.R(0.9f)); }); }
}

void drawDeck(BodyPainter& p, const Outline& ic) {
  shoulders(p, ic, 9.0f, 17.5f, 20.0f, 1.6f, true);
  iconBody(p, ic, false);
  { const Pt a = ic.T(19.6f, 22.2f), b = ic.T(44.4f, 41.8f); p.part(0xFF0E0F12, 0xFF50545C, 2.5f, [&](float x, float y) { return roundRect(x, y, a.x, a.y, b.x, b.y, 10); }); }
  { const Pt c = ic.T(11.6f, 25.0f); p.dpad(c.x, c.y, ic.R(1.9f), "dpad"); }
  { const Pt c = ic.T(16.2f, 25.6f); p.stick(c.x, c.y, ic.R(1.25f), "cursor"); }
  { const Pt a = ic.T(11.0f, 30.6f), b = ic.T(17.4f, 37.0f); p.part(kPadFill, kRim, 2.5f, [&](float x, float y) { return roundRect(x, y, a.x, a.y, b.x, b.y, 12); }); p.mark("lpad", (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f); }
  { const Pt c = ic.T(47.8f, 25.6f); p.stick(c.x, c.y, ic.R(1.25f), "camera"); }
  const float fr = ic.R(0.62f), fs = 1.35f;
  { const Pt c = ic.T(52.4f, 25.0f - fs); p.face(c.x, c.y, fr, L"Y", kWhite, "construct"); }
  { const Pt c = ic.T(52.4f - fs, 25.0f); p.face(c.x, c.y, fr, L"X", kWhite, "bulldoze"); }
  { const Pt c = ic.T(52.4f + fs, 25.0f); p.face(c.x, c.y, fr, L"B", kWhite, "rclick"); }
  { const Pt c = ic.T(52.4f, 25.0f + fs); p.face(c.x, c.y, fr, L"A", kWhite, "click"); }
  { const Pt a = ic.T(46.6f, 30.6f), b = ic.T(53.0f, 37.0f); p.part(kPadFill, kRim, 2.5f, [&](float x, float y) { return roundRect(x, y, a.x, a.y, b.x, b.y, 12); }); p.mark("rpad", (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f); }
  { const Pt c = ic.T(18.1f, 21.4f); p.small(c.x, c.y, ic.R(0.5f), 1, "underground"); }
  { const Pt c = ic.T(45.9f, 21.4f); p.small(c.x, c.y, ic.R(0.5f), 2, "esc"); }
  // the back buttons (behind the grips): outlines along the bottom edge
  const Pt l4a = ic.T(10.2f, 40.0f), l4b = ic.T(14.2f, 41.4f), r4a = ic.T(49.8f, 40.0f), r4b = ic.T(53.8f, 41.4f);
  p.stroke(0xFF9AA0AA, 1.4f, [&](float x, float y) { return roundRect(x, y, l4a.x, l4a.y, l4b.x, l4b.y, 6); });
  p.stroke(0xFF9AA0AA, 1.4f, [&](float x, float y) { return roundRect(x, y, r4a.x, r4a.y, r4b.x, r4b.y, 6); });
  p.mark("camHold", (l4a.x + l4b.x) * 0.5f, (l4a.y + l4b.y) * 0.5f);
  p.mark("rclick2", (r4a.x + r4b.x) * 0.5f, (r4a.y + r4b.y) * 0.5f);
}

void drawSteamController(BodyPainter& p, const Outline& ic) {
  shoulders(p, ic, 13.6f, 23.3f, 16.0f, 2.0f);
  iconBody(p, ic, true);
  for (float u : {19.0f, 45.0f}) {
    const Pt c = ic.T(u, 25);
    p.part(kPadFill, kRim, 2, [&](float x, float y) { return circle(x, y, c.x, c.y, ic.R(5) - 5); });
    p.stroke(0xFF555962, 1.2f, [&](float x, float y) { return circle(x, y, c.x, c.y, ic.R(3.9f)); });
  }
  { const Pt c = ic.T(19, 25); p.mark("lpadSc", c.x, c.y); }
  { const Pt c = ic.T(45, 25); p.mark("rpad", c.x, c.y); }
  capIn(p, ic, 26.5f, 32.5f, 2.5f, "cursor");
  { const Pt c = ic.T(37.5f, 29.5f); p.face(c.x, c.y, ic.R(1.35f), L"Y", kXYellow, "construct"); }
  { const Pt c = ic.T(34.5f, 32.5f); p.face(c.x, c.y, ic.R(1.35f), L"X", kXBlue, "bulldoze"); }
  { const Pt c = ic.T(40.5f, 32.5f); p.face(c.x, c.y, ic.R(1.35f), L"B", kXRed, "rclick"); }
  { const Pt c = ic.T(37.5f, 35.5f); p.face(c.x, c.y, ic.R(1.35f), L"A", kXGreen, "click"); }
  { const Pt c = ic.T(32, 24.4f); p.part(kCap, kCapRim, 2, [&](float x, float y) { return circle(x, y, c.x, c.y, ic.R(1.5f)); }); }
  { const Pt c = ic.T(28.4f, 26.6f); p.small(c.x, c.y, ic.R(0.75f), 3, "underground"); }
  { const Pt c = ic.T(35.6f, 26.6f); p.small(c.x, c.y, ic.R(0.75f), 4, "esc"); }
  // the back grips: outlines on the handles
  const Pt la = ic.T(14.6f, 40.0f), lb = ic.T(13.6f, 44.6f), ra = ic.T(49.4f, 40.0f), rb = ic.T(50.4f, 44.6f);
  p.stroke(0xFF9AA0AA, 1.4f, [&](float x, float y) { return capsule(x, y, la.x, la.y, lb.x, lb.y, ic.R(0.9f)); });
  p.stroke(0xFF9AA0AA, 1.4f, [&](float x, float y) { return capsule(x, y, ra.x, ra.y, rb.x, rb.y, ic.R(0.9f)); });
  p.mark("camHold", (la.x + lb.x) * 0.5f, (la.y + lb.y) * 0.5f);
  p.mark("rclick2", (ra.x + rb.x) * 0.5f, (ra.y + rb.y) * 0.5f);
}

// The labels: the picture is the whole diagram (880 × 272 shown pixels, drawn at twice that), the controller in the
// middle and a column of labels left and right of it, whose texts the page lays over the picture. Each label has a
// slot (52 picture pixels apart, as many as the side has labels, centred); its leader runs level from the column for
// a short stub, then straight to its button, where a dot sits. The slots are given out so that no two leaders cross
// and none runs over another button, then the leaders are short.
struct Callout {
  Anchor a;
  int side;    // 0 left, 1 right
  float slot;  // the label's y, picture pixels
};

constexpr float kSlot = 52, kStub = 22;
constexpr float kLeftEdge = 492, kRightEdge = kDiaW - 492;  // where the columns end and the leaders begin

bool segmentsCross(Pt a, Pt b, Pt c, Pt d) {
  auto side = [](Pt p, Pt q, Pt r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
  const float d1 = side(c, d, a), d2 = side(c, d, b), d3 = side(a, b, c), d4 = side(a, b, d);
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

std::vector<Callout> placeLabels(const std::vector<Anchor>& anchors) {
  std::vector<Callout> out;
  for (int side = 0; side < 2; ++side) {
    std::vector<Anchor> mine;
    for (const Anchor& a : anchors)
      if ((a.x < kDiaW * 0.5f) == (side == 0)) mine.push_back(a);
    if (mine.empty()) continue;
    std::sort(mine.begin(), mine.end(), [](const Anchor& l, const Anchor& r) { return l.y < r.y; });
    const int n = int(mine.size());
    const float first = (kDiaH - float(n - 1) * kSlot) * 0.5f;
    const float edge = side == 0 ? kLeftEdge : kRightEdge, stub = side == 0 ? kLeftEdge + kStub : kRightEdge - kStub;
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    auto cost = [&]() {
      float c = 0;
      for (int i = 0; i < n; ++i) {
        const float si = first + float(order[i]) * kSlot;
        const Pt e{edge, si}, k{stub, si}, a{mine[i].x, mine[i].y};
        c += std::hypot(a.x - k.x, a.y - k.y);
        for (int j = 0; j < n; ++j) {
          if (j == i) continue;
          const float sj = first + float(order[j]) * kSlot;
          const Pt ej{edge, sj}, kj{stub, sj}, aj{mine[j].x, mine[j].y};
          if (j > i && (segmentsCross(k, a, kj, aj) || segmentsCross(k, a, ej, kj) || segmentsCross(e, k, kj, aj))) c += 10000;
          if (segment(aj.x, aj.y, k.x, k.y, a.x, a.y) < 16) c += 600;  // over another button
        }
      }
      return c;
    };
    float best = cost();
    for (bool improved = true; improved;) {
      improved = false;
      for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
          std::swap(order[i], order[j]);
          const float c = cost();
          if (c + 0.01f < best) best = c, improved = true;
          else std::swap(order[i], order[j]);
        }
    }
    for (int i = 0; i < n; ++i) out.push_back({mine[i], side, first + float(order[i]) * kSlot});
  }
  return out;
}

void drawLeaders(Canvas& cv, const std::vector<Callout>& callouts) {
  constexpr uint32_t kLine = 0xFFC8B48C;
  for (const Callout& c : callouts) {
    const float edge = c.side == 0 ? kLeftEdge : kRightEdge, stub = c.side == 0 ? kLeftEdge + kStub : kRightEdge - kStub;
    const Pt e{edge, c.slot}, k{stub, c.slot};
    Pt a{c.a.x, c.a.y};
    if (c.a.r > 0) {  // a round button: to its edge
      const float dx = k.x - a.x, dy = k.y - a.y, len = std::hypot(dx, dy);
      if (len > c.a.r) a = {a.x + dx / len * (c.a.r + 1), a.y + dy / len * (c.a.r + 1)};
    }
    cv.fill(0x90000000, [&](float x, float y) { return std::min(segment(x, y, e.x, e.y, k.x, k.y), segment(x, y, k.x, k.y, a.x, a.y)) - 2.6f; });
    cv.fill(kLine, [&](float x, float y) { return std::min(segment(x, y, e.x, e.y, k.x, k.y), segment(x, y, k.x, k.y, a.x, a.y)) - 1.1f; });
    cv.fill(0xFF000000, [&](float x, float y) { return circle(x, y, a.x, a.y, 6.5f); });
    cv.fill(kLine, [&](float x, float y) { return circle(x, y, a.x, a.y, 4.5f); });
  }
}

bool writeLayout(const fs::path& file, const std::vector<Callout>& callouts) {
  std::string s = "// written by regs-tool padglyphs: the label slots of the controller picture on the controls page\n";
  char buf[200];
  std::snprintf(buf, sizeof buf, "$width = %d;\n$height = %d;\n$leftEdge = %d;\n$rightEdge = %d;\n", kDiaW / 2, kDiaH / 2,
                int(kLeftEdge / 2), int(kRightEdge / 2));
  s += buf;
  s += "// [side (0 left: the text ends at $leftEdge, 1 right: it begins at $rightEdge), label y, text]\n$callouts = [\n";
  for (size_t i = 0; i < callouts.size(); ++i) {
    std::snprintf(buf, sizeof buf, "  [%d, %d, \"%s\"]%s\n", callouts[i].side, int(std::lround(callouts[i].slot / 2)),
                  callouts[i].a.tag.c_str(), i + 1 < callouts.size() ? "," : "");
    s += buf;
  }
  s += "];\n";
  FILE* f = _wfopen(file.wstring().c_str(), L"wb");
  const bool ok = f && std::fwrite(s.data(), 1, s.size(), f) == s.size();
  if (f) std::fclose(f);
  return ok;
}

int writeBodies(const fs::path& dir, const fs::path& icons) {
  struct B {
    const char* name;
    const char* icon;
    void (*draw)(BodyPainter&, const Outline&);
  };
  const B bodies[] = {{"xbox", "controller_xboxseries.svg", drawXbox},
                      {"playstation", "controller_playstation5.svg", drawPlayStation},
                      {"deck", "controller_steamdeck.svg", drawDeck},
                      {"steamcontroller", "controller_steam.svg", drawSteamController}};
  int ok = 0;
  for (const B& b : bodies) {
    Outline ic;
    if (!ic.load(icons / b.icon)) {
      std::fprintf(stderr, "padglyphs: cannot read %s\n", (icons / b.icon).string().c_str());
      continue;
    }
    Canvas cv(kDiaW, kDiaH);
    std::vector<Anchor> anchors;
    BodyPainter p{cv, anchors};
    b.draw(p, ic);
    const std::vector<Callout> callouts = placeLabels(anchors);
    drawLeaders(cv, callouts);
    if (writeDds(dir / (std::string("body_") + b.name + ".dds"), cv) && writeLayout(dir / (std::string(b.name) + ".layout"), callouts)) ++ok;
  }
  return ok;
}

}  // namespace

int cmdPadGlyphs(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: regs-tool padglyphs <out dir> [<controller icon dir>]\n");
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
  const int bodies = argc > 3 ? writeBodies(dir, argv[3]) : 0;  // the controller pictures and their label layouts
  std::printf("padglyphs: wrote %d of %d glyphs and %d of 4 controller pictures to %s\n", written, total, bodies,
              dir.string().c_str());
  return written == total && bodies == 4 ? 0 : 1;
}
