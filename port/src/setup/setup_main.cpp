// opencim-setup.exe: OpenCiM's own setup program (docs/installer.md). One window in the style of the Cities in Motion
// main menu: the dark panel on the left, a real screenshot of the port on the right; pages Welcome, Game not found,
// Options, Installing (the feature showcase), Done and, when OpenCiM is installed already, Maintenance (update,
// repair, game folder, uninstall).
//
//   opencim-setup                              the window
//   opencim-setup --uninstall                  the window on the maintenance page, uninstall selected (Apps & features)
//   opencim-setup --silent-install [--target <dir>] [--game <dir>] [--desktop] [--no-start-menu]
//   opencim-setup --silent-uninstall [--target <dir>] [--remove-documents]
//   opencim-setup --verify [--target <dir>]    lists installed files that are missing or changed (exit 1 when any)
//   opencim-setup --make <stub.exe> <appDir> <showDir> <out.exe>   the offline setup (make-release.ps1)
//   opencim-setup --render <page> <out.png> [--scale <s>] [--slide <n>] [--progress <0..1>] [--action <n>]
//                                              a page drawn without a window (tests): welcome, notfound, options,
//                                              installing, done, maintenance
// Test switches: OPENCIM_SETUP_SANDBOX=<dir> (install.h), OPENCIM_SETUP_GAME=<dir>|none (instead of the search).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "build_info.h"
#include "core/game_find.h"
#include "setup/install.h"
#include "setup/payload.h"

namespace fs = std::filesystem;
using namespace Gdiplus;
using namespace regs;
using namespace regs::setup;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// text and colours

std::wstring W(const std::string& s) { return pathFromUtf8(s).wstring(); }
std::string U(const std::wstring& s) { return pathToUtf8(fs::path(s)); }

Color rgb(uint32_t c, int a = 255) { return Color(BYTE(a), BYTE(c >> 16), BYTE(c >> 8), BYTE(c)); }

const uint32_t kPanel = 0x18120e, kPanelEdge = 0x5c5147, kBg = 0x15100c, kText = 0xf1ece4, kSoft = 0xd8cfc3,
               kDim = 0xb9ab9b, kNote = 0x999999, kOrange = 0xd9772b, kLink = 0xf3d3a6;

struct Feature {
  const wchar_t* shortName;
  const wchar_t* heading;
  const wchar_t* text;
};
const Feature kFeatures[8] = {
    {L"YOUR OWN CITY", L"Your own city",
     L"Build a map of any real town from OpenStreetMap: streets, rail, rivers, parks and buildings."},
    {L"FREE ROAD BUILDING", L"Free road building",
     L"Curves, roundabouts, one-way streets, avenues, tunnels, bridges and express ramps."},
    {L"RAIN, SNOW, AUTUMN", L"Rain, snow and autumn",
     L"Weather that changes the streets, seasons that colour the trees."},
    {L"CITIES AT NIGHT", L"Cities at night", L"Street lamps, lit windows, headlights and lit trams after dark."},
    {L"MORE WAYS TO RUN LINES", L"More ways to run lines",
     L"Trams on railway tracks, tram stops at stations, metro trains turning at end stations."},
    {L"ROOM TO BUILD", L"Room to build", L"Maps up to 2048 cells, map templates, autosave and new building tools."},
    {L"A SHARPER LOOK", L"A sharper look", L"Better shadows, HDR, lens blur and tilt-shift, UI scaling for big screens."},
    {L"MADE TO BE EXTENDED", L"Made to be extended",
     L"A mod manager for new content, loaded beside the game, never changing its files."},
};

// ---------------------------------------------------------------------------------------------------------------------
// state

enum class Page { Welcome, NotFound, Options, Installing, Done, Maintenance };
enum Act { ActUpdate, ActRepair, ActFolder, ActUninstall };

enum Hot {
  HotNone,
  HotInstall,
  HotOptions,
  HotCancel,
  HotChangeGame,
  HotChooseFolder,
  HotSteam,
  HotBack,
  HotTarget,
  HotStartMenu,
  HotDesktop,
  HotContinue,
  HotNext,
  HotPlay,
  HotWhatsNew,
  HotClose,
  HotPanelButton,
  HotKeepDocs,
  HotX,
  HotSlide0 = 100,   // + slide
  HotAction0 = 200,  // + action
};

struct HotRect {
  RectF r;
  int id;
};

struct Pictures {
  std::unique_ptr<Bitmap> title, welcome, notFound, done, maintenance;
  std::unique_ptr<Bitmap> slides[8];
  std::vector<std::unique_ptr<Bitmap>> welcomeShots;  // the welcome page's cross-fade: bg-welcome, then welcome-*.jpg
};

struct App {
  HWND hwnd = nullptr;
  float scale = 1.0f;
  fs::path self;
  Payload payload;
  Pictures pics;
  Page page = Page::Welcome;
  // the game
  fs::path game;
  GameCheck check;
  std::string gameSource;
  std::wstring gameMessage;  // after a folder was picked that has no game
  // install options
  InstallOptions opts;
  // the install
  fs::path installed;  // an existing install (maintenance)
  Manifest manifest;
  std::thread worker;
  Progress progress;
  std::atomic<bool> working{false}, failed{false}, finished{false};
  std::string error;
  bool updating = false;  // the showcase runs for an update
  bool whatsNew = false;  // the showcase opened from the Done page
  // the showcase
  int slide = 0, prevSlide = -1;
  double fadeStart = 0, slideStart = 0;
  // the welcome page's pictures (summer, snow, sunset, night, autumn), 6 s each with a 1 s cross-fade
  int shot = 0, prevShot = -1;
  double shotFade = 0, shotStart = 0;
  // maintenance
  int action = ActUpdate;
  bool keepDocs = true;
  std::wstring panelStatus;
  bool removed = false;
  // input
  std::vector<HotRect> hot;
  int hover = HotNone;
  float renderProgress = -1;  // --render
};

App g;

double nowSec() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------------------------------------------------
// pictures

std::unique_ptr<Bitmap> decode(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return nullptr;
  IStream* s = SHCreateMemStream(bytes.data(), UINT(bytes.size()));
  if (!s) return nullptr;
  std::unique_ptr<Bitmap> src(Bitmap::FromStream(s));
  std::unique_ptr<Bitmap> out;
  if (src && src->GetLastStatus() == Ok) {
    // a copy that no longer needs the stream, in the fastest format to draw
    out = std::make_unique<Bitmap>(src->GetWidth(), src->GetHeight(), PixelFormat32bppPARGB);
    Graphics gr(out.get());
    gr.DrawImage(src.get(), 0, 0, src->GetWidth(), src->GetHeight());
  }
  src.reset();
  s->Release();
  return out;
}

std::vector<uint8_t> payloadFile(const std::string& path) {
  std::vector<uint8_t> out;
  if (const PayloadEntry* e = g.payload.find(path)) g.payload.read(*e, out);
  return out;
}

std::vector<uint8_t> diskFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> resource(int id) {
  HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
  if (!r) return {};
  HGLOBAL h = LoadResource(nullptr, r);
  const uint8_t* d = static_cast<const uint8_t*>(LockResource(h));
  return d ? std::vector<uint8_t>(d, d + SizeofResource(nullptr, r)) : std::vector<uint8_t>{};
}

void loadPictures() {
  g.pics.title = decode(resource(1));
  g.pics.welcome = decode(payloadFile("show/bg-welcome.jpg"));
  g.pics.notFound = decode(payloadFile("show/bg-notfound.jpg"));
  g.pics.done = decode(payloadFile("show/bg-done.jpg"));
  g.pics.maintenance = decode(payloadFile("show/bg-maintenance.jpg"));
  if (!g.pics.maintenance && !g.installed.empty())  // the installed setup program has no payload: its own copy
    g.pics.maintenance = decode(diskFile(g.installed / "setup" / "maintenance.jpg"));
  for (const PayloadEntry& e : g.payload.entries) {
    if (e.path.rfind("show/", 0) != 0 || e.path.size() < 7) continue;
    char c = e.path[5];
    if (c >= '1' && c <= '8' && e.path[6] == '-') g.pics.slides[c - '1'] = decode(payloadFile(e.path));
  }
  // the welcome page fades through bg-welcome and every show/welcome-*.jpg (the payload's entries are sorted)
  if (g.pics.welcome) {
    std::vector<uint8_t> first = payloadFile("show/bg-welcome.jpg");
    g.pics.welcomeShots.push_back(decode(first));
  }
  for (const PayloadEntry& e : g.payload.entries)
    if (e.path.rfind("show/welcome-", 0) == 0)
      if (auto b = decode(payloadFile(e.path))) g.pics.welcomeShots.push_back(std::move(b));
}

// ---------------------------------------------------------------------------------------------------------------------
// drawing helpers (design pixels: the page is 1024 x 640, scaled by the DPI)

struct Fonts {
  std::unique_ptr<FontFamily> sans, caps;
  bool hasCaps = false;
  Fonts() {
    sans = std::make_unique<FontFamily>(L"Tahoma");
    if (sans->GetLastStatus() != Ok) sans = std::make_unique<FontFamily>(L"Segoe UI");
    caps = std::make_unique<FontFamily>(L"Copperplate Gothic Bold");
    hasCaps = caps->GetLastStatus() == Ok;
    if (!hasCaps) caps = std::make_unique<FontFamily>(L"Tahoma");
  }
};
Fonts* fonts = nullptr;

void text(Graphics& gr, const std::wstring& s, float size, uint32_t color, RectF r, bool bold = false,
          StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentNear, bool caps = false,
          int alpha = 255) {
  Font f(caps ? fonts->caps.get() : fonts->sans.get(), size, (bold && !(caps && fonts->hasCaps)) ? FontStyleBold : 0,
         UnitPixel);
  StringFormat fmt;
  fmt.SetAlignment(h);
  fmt.SetLineAlignment(v);
  fmt.SetTrimming(StringTrimmingEllipsisCharacter);
  SolidBrush b(rgb(color, alpha));
  gr.DrawString(s.c_str(), int(s.size()), &f, r, &fmt, &b);
}

// text with the menu's black outline (the game's bar labels)
void outlined(Graphics& gr, const std::wstring& s, float size, uint32_t color, RectF r, StringAlignment h,
              bool caps = false) {
  for (auto [dx, dy] : {std::pair{1.f, 0.f}, {-1.f, 0.f}, {0.f, 1.f}, {0.f, -1.f}, {1.f, 1.f}}) {
    RectF o = r;
    o.X += dx, o.Y += dy;
    text(gr, s, size, 0x000000, o, true, h, StringAlignmentCenter, caps);
  }
  text(gr, s, size, color, r, true, h, StringAlignmentCenter, caps);
}

void vgrad(Graphics& gr, RectF r, std::initializer_list<std::pair<float, uint32_t>> stops, int alpha = 255) {
  std::vector<Color> cols;
  std::vector<REAL> pos;
  for (auto& [p, c] : stops) pos.push_back(p), cols.push_back(rgb(c, alpha));
  LinearGradientBrush b(PointF(0, r.Y - 0.5f), PointF(0, r.GetBottom() + 0.5f), cols.front(), cols.back());
  b.SetInterpolationColors(cols.data(), pos.data(), int(cols.size()));
  gr.FillRectangle(&b, r);
}

void fill(Graphics& gr, RectF r, Color c) {
  SolidBrush b(c);
  gr.FillRectangle(&b, r);
}

void hline(Graphics& gr, float x0, float x1, float y, uint32_t c, float w = 1) {
  Pen p(rgb(c), w);
  gr.DrawLine(&p, x0, y, x1, y);
}

GraphicsPath* roundRect(RectF r, float rad) {
  auto* p = new GraphicsPath();
  float d = rad * 2;
  p->AddArc(r.X, r.Y, d, d, 180, 90);
  p->AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
  p->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
  p->AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
  p->CloseFigure();
  return p;
}

// the game's caption box: dark translucent fill, grey edge, round corners
void captionBox(Graphics& gr, RectF r) {
  std::unique_ptr<GraphicsPath> p(roundRect(r, 20));
  SolidBrush b(rgb(0x1e1a16, 230));
  gr.FillPath(&b, p.get());
  Pen outer(rgb(0x201a16), 4), edge(rgb(0x3a3a3a), 2);
  gr.DrawPath(&outer, p.get());
  gr.DrawPath(&edge, p.get());
}

// a picture filling r (cover: cropped, never stretched)
void cover(Graphics& gr, Bitmap* bmp, RectF r, float alpha = 1.0f) {
  if (!bmp) {
    vgrad(gr, r, {{0.f, 0x2b211b}, {1.f, kBg}});
    return;
  }
  float iw = float(bmp->GetWidth()), ih = float(bmp->GetHeight());
  float s = std::max(r.Width / iw, r.Height / ih);
  float sw = r.Width / s, sh = r.Height / s;
  RectF src((iw - sw) / 2, (ih - sh) / 2, sw, sh);
  if (alpha >= 0.999f) {
    gr.DrawImage(bmp, r, src.X, src.Y, src.Width, src.Height, UnitPixel);
    return;
  }
  ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha, 0}, {0, 0, 0, 0, 1}}};
  ImageAttributes ia;
  ia.SetColorMatrix(&cm);
  gr.DrawImage(bmp, r, src.X, src.Y, src.Width, src.Height, UnitPixel, &ia);
}

void addHot(RectF r, int id) { g.hot.push_back({r, id}); }

enum class Icon { None, Down, Gear, Cross, Play, Folder, Store, Back, Wrench, Trash, Refresh };

// a round icon of the menu bars
void iconDot(Graphics& gr, PointF c, float rad, uint32_t light, uint32_t mid, uint32_t dark, Icon icon) {
  GraphicsPath p;
  p.AddEllipse(c.X - rad, c.Y - rad, rad * 2, rad * 2);
  {
    SolidBrush shadow(rgb(0x000000, 160));
    gr.FillEllipse(&shadow, c.X - rad, c.Y - rad + 1, rad * 2, rad * 2);
  }
  PathGradientBrush b(&p);
  b.SetCenterPoint(PointF(c.X - rad * 0.3f, c.Y - rad * 0.4f));
  b.SetCenterColor(rgb(light));
  Color edge = rgb(dark);
  int n = 1;
  b.SetSurroundColors(&edge, &n);
  Color cols[3] = {rgb(dark), rgb(mid), rgb(light)};
  REAL pos[3] = {0.f, 0.4f, 1.f};
  b.SetInterpolationColors(cols, pos, 3);
  gr.FillPath(&b, &p);
  Pen pen(rgb(icon == Icon::Gear ? 0x2b2b2b : 0xffffff), 1.6f);
  pen.SetStartCap(LineCapRound);
  pen.SetEndCap(LineCapRound);
  float s = rad * 0.45f;
  switch (icon) {
    case Icon::Down:
      gr.DrawLine(&pen, c.X, c.Y - s, c.X, c.Y + s * 0.6f);
      gr.DrawLine(&pen, c.X - s * 0.7f, c.Y, c.X, c.Y + s * 0.7f);
      gr.DrawLine(&pen, c.X + s * 0.7f, c.Y, c.X, c.Y + s * 0.7f);
      gr.DrawLine(&pen, c.X - s, c.Y + s * 1.2f, c.X + s, c.Y + s * 1.2f);
      break;
    case Icon::Gear:
      gr.DrawEllipse(&pen, c.X - s * 0.6f, c.Y - s * 0.6f, s * 1.2f, s * 1.2f);
      for (int k = 0; k < 8; ++k) {
        float a = float(k) * 3.14159f / 4;
        gr.DrawLine(&pen, c.X + std::cos(a) * s, c.Y + std::sin(a) * s, c.X + std::cos(a) * s * 1.5f,
                    c.Y + std::sin(a) * s * 1.5f);
      }
      break;
    case Icon::Cross:
      gr.DrawLine(&pen, c.X - s, c.Y - s, c.X + s, c.Y + s);
      gr.DrawLine(&pen, c.X + s, c.Y - s, c.X - s, c.Y + s);
      break;
    case Icon::Play: {
      PointF tri[3] = {{c.X - s * 0.7f, c.Y - s * 1.1f}, {c.X + s * 1.2f, c.Y}, {c.X - s * 0.7f, c.Y + s * 1.1f}};
      SolidBrush w(rgb(0xffffff));
      gr.FillPolygon(&w, tri, 3);
      break;
    }
    case Icon::Folder:
      gr.DrawRectangle(&pen, c.X - s, c.Y - s * 0.6f, s * 2, s * 1.5f);
      gr.DrawLine(&pen, c.X - s, c.Y - s * 0.6f, c.X - s * 0.2f, c.Y - s * 1.0f);
      break;
    case Icon::Store:
      gr.DrawRectangle(&pen, c.X - s, c.Y - s * 0.3f, s * 2, s * 1.3f);
      gr.DrawArc(&pen, c.X - s * 0.6f, c.Y - s * 1.2f, s * 1.2f, s * 1.4f, 180, 180);
      break;
    case Icon::Back:
      gr.DrawLine(&pen, c.X + s * 0.5f, c.Y - s, c.X - s * 0.5f, c.Y);
      gr.DrawLine(&pen, c.X - s * 0.5f, c.Y, c.X + s * 0.5f, c.Y + s);
      break;
    case Icon::Wrench:
      gr.DrawLine(&pen, c.X - s, c.Y + s, c.X + s * 0.3f, c.Y - s * 0.3f);
      gr.DrawEllipse(&pen, c.X, c.Y - s * 1.1f, s * 1.1f, s * 1.1f);
      break;
    case Icon::Trash:
      gr.DrawRectangle(&pen, c.X - s * 0.7f, c.Y - s * 0.5f, s * 1.4f, s * 1.6f);
      gr.DrawLine(&pen, c.X - s, c.Y - s * 0.8f, c.X + s, c.Y - s * 0.8f);
      break;
    case Icon::Refresh:
      gr.DrawArc(&pen, c.X - s, c.Y - s, s * 2, s * 2, 40, 280);
      gr.DrawLine(&pen, c.X + s * 0.8f, c.Y - s * 0.9f, c.X + s * 0.8f, c.Y - s * 0.1f);
      break;
    case Icon::None:
      break;
  }
}

struct DotColors {
  uint32_t light, mid, dark;
};
const DotColors kBlue{0x6aa3e0, 0x2f6db5, 0x1d4677}, kGrey{0xd8d8d8, 0x8d8d8d, 0x555555},
    kRed{0xf08a6a, 0xc0392b, 0x7a2018}, kGreen{0x8fd09a, 0x3f8a4a, 0x1f4d27};

// one of the menu's bevelled bars, 28 px tall
void bar(Graphics& gr, RectF r, const std::wstring& label, int id, Icon icon = Icon::None, DotColors dot = kGrey,
         bool selected = false, bool enabled = true) {
  bool hov = enabled && g.hover == id;
  if (selected || hov)
    vgrad(gr, r, {{0.f, 0x4d4031}, {0.5f, 0x463828}, {1.f, 0x4d4031}});
  else
    vgrad(gr, r, {{0.f, 0x332921}, {0.5f, 0x2a1e16}, {1.f, 0x332921}});
  hline(gr, r.X, r.GetRight(), r.Y, selected || hov ? 0x504333 : 0x493934);
  hline(gr, r.X, r.GetRight(), r.GetBottom(), 0x493934);
  float left = 0;
  if (icon != Icon::None) {
    iconDot(gr, PointF(r.X + 14 + 12, r.Y + r.Height / 2), 12, dot.light, dot.mid, dot.dark, icon);
    left = 36;
  }
  RectF t(r.X + left, r.Y, r.Width - left * 2, r.Height);
  outlined(gr, label, 12.5f, enabled ? 0xe6e6e6 : 0x857a70, t, StringAlignmentCenter, true);
  if (enabled) addHot(r, id);
}

void checkbox(Graphics& gr, float x, float y, float w, const std::wstring& label, bool on, int id) {
  RectF box(x, y + 2, 14, 14);
  fill(gr, box, rgb(0x1a1714));
  Pen p(rgb(g.hover == id ? 0xffffff : 0xa0a0a0), 1);
  gr.DrawRectangle(&p, box);
  if (on) {
    Pen m(rgb(0xd6d6d6), 2);
    m.SetStartCap(LineCapRound);
    m.SetEndCap(LineCapRound);
    gr.DrawLine(&m, x + 3, y + 9, x + 6, y + 13);
    gr.DrawLine(&m, x + 6, y + 13, x + 11, y + 5);
  }
  text(gr, label, 12, 0xe9e1d6, RectF(x + 24, y, w - 24, 18), false, StringAlignmentNear, StringAlignmentCenter);
  addHot(RectF(x, y, w, 18), id);
}

// the dark panel's top: the OpenCiM title and a line in small capitals
void panelHeader(Graphics& gr, const std::wstring& sub) {
  RectF r(4, 4, 320, 124);
  vgrad(gr, r, {{0.f, 0x151311}, {1.f, 0x262422}});
  hline(gr, r.X, r.GetRight(), r.GetBottom() - 1, 0x3c3a39, 2);
  hline(gr, r.X, r.GetRight(), r.GetBottom() + 1, 0x666666, 2);
  if (g.pics.title)
    gr.DrawImage(g.pics.title.get(), RectF(36, 28, 256, 64));
  else
    outlined(gr, L"OPENCiM", 40, 0xeeeeee, RectF(4, 28, 320, 64), StringAlignmentCenter, true);
  text(gr, sub, 10, kDim, RectF(4, 98, 320, 16), false, StringAlignmentCenter, StringAlignmentCenter, true);
}

void panelBase(Graphics& gr) {
  fill(gr, RectF(0, 0, 328, 640), rgb(kPanel));
  fill(gr, RectF(326, 0, 2, 640), rgb(kPanelEdge));
}

void noteAtBottom(Graphics& gr, const std::wstring& s) {
  text(gr, s, 11, kNote, RectF(20, 640 - 64, 290, 52), false, StringAlignmentNear, StringAlignmentFar);
}

void corner(Graphics& gr, const std::wstring& a, const std::wstring& b) {
  RectF r(340, 640 - 50, 664, 18);
  text(gr, a, 11, 0x000000, RectF(r.X + 1, r.Y + 1, r.Width, r.Height), true, StringAlignmentFar);
  text(gr, a, 11, kText, r, true, StringAlignmentFar);
  RectF r2(340, 640 - 32, 664, 18);
  text(gr, b, 11, 0x000000, RectF(r2.X + 1, r2.Y + 1, r2.Width, r2.Height), false, StringAlignmentFar);
  text(gr, b, 11, kText, r2, false, StringAlignmentFar);
}

std::wstring versionLine() {
  std::wstring v = W(g.payload.present() ? g.payload.version : REGS_VERSION_STRING);
  int build = g.payload.present() ? g.payload.build : REGS_BUILD_NUMBER;
  return L"Version " + v + L" (build " + std::to_wstring(build) + L")";
}

std::wstring dlcLine() {
  std::wstring src = W(g.gameSource.empty() ? "your folder" : g.gameSource);
  if (g.check.dlcs.empty()) return L"found in " + src;
  return L"found in " + src + L" \u00b7 " + std::to_wstring(g.check.dlcs.size()) + L" DLC" +
         (g.check.dlcs.size() == 1 ? L"" : L"s");
}

const RectF kRight(328, 0, 696, 640);

// ---------------------------------------------------------------------------------------------------------------------
// pages

void pageWelcome(Graphics& gr) {
  panelBase(gr);
  panelHeader(gr, L"FOR CITIES IN MOTION");
  text(gr, L"A new engine for Cities in Motion: curved roads, real cities from OpenStreetMap, weather and nights, big "
           L"maps and mods.",
       12, kSoft, RectF(20, 146, 290, 64));
  // the game line
  float y = 214;
  bool ok = g.check.ok;
  {
    SolidBrush b(rgb(ok ? 0x3f8a4a : 0xc0392b));
    gr.FillEllipse(&b, 22.f, y + 4, 16.f, 16.f);
    Pen p(rgb(0xffffff), 2);
    if (ok) {
      gr.DrawLine(&p, 26.5f, y + 12.2f, 29.f, y + 14.5f);
      gr.DrawLine(&p, 29.f, y + 14.5f, 33.5f, y + 9.5f);
    } else {
      gr.DrawLine(&p, 30.f, y + 7, 30.f, y + 13);
      gr.DrawLine(&p, 30.f, y + 15, 30.f, y + 16.5f);
    }
  }
  text(gr, ok ? L"Your Cities in Motion" : L"Cities in Motion not found", 11, kSoft, RectF(46, y, 200, 14));
  text(gr, ok ? dlcLine() : L"choose its folder", 11, kDim, RectF(46, y + 14, 200, 14));
  RectF change(250, y + 2, 60, 20);
  text(gr, L"CHANGE", 10, g.hover == HotChangeGame ? 0xffffff : kLink, change, true, StringAlignmentFar,
       StringAlignmentCenter);
  addHot(change, HotChangeGame);
  bool can = ok && g.payload.present();
  bar(gr, RectF(4, 270, 320, 28), L"INSTALL", HotInstall, Icon::Down, kBlue, false, can);
  bar(gr, RectF(4, 307, 320, 28), L"OPTIONS", HotOptions, Icon::Gear, kGrey);
  bar(gr, RectF(4, 344, 320, 28), L"CANCEL", HotCancel, Icon::Cross, kRed);
  if (!g.payload.present())
    text(gr, L"This setup program carries no OpenCiM files. Use the full setup from the OpenCiM release page.", 11,
         0xf3b06a, RectF(20, 386, 290, 48));
  noteAtBottom(gr, L"Unofficial port. Needs the original Cities in Motion; nothing of the game is in this download.");
  const auto& shots = g.pics.welcomeShots;
  if (shots.empty()) {
    cover(gr, g.pics.welcome.get(), kRight);
  } else {
    int cur = g.shot % int(shots.size());
    float fade = g.prevShot < 0 ? 1.f : float(std::clamp((nowSec() - g.shotFade) / 1.0, 0.0, 1.0));
    if (g.prevShot >= 0 && fade < 1) cover(gr, shots[size_t(g.prevShot)].get(), kRight);
    cover(gr, shots[size_t(cur)].get(), kRight, fade);
  }
  uint64_t mb = g.payload.appBytes() / 1048576;
  corner(gr, L"OpenCiM Setup", versionLine() + (mb ? L" \u00b7 " + std::to_wstring(mb) + L" MB" : L""));
}

void pageNotFound(Graphics& gr) {
  panelBase(gr);
  panelHeader(gr, L"FOR CITIES IN MOTION");
  float y = 146;
  {
    SolidBrush b(rgb(0xc0392b));
    gr.FillEllipse(&b, 22.f, y + 4, 16.f, 16.f);
    Pen p(rgb(0xffffff), 2);
    gr.DrawLine(&p, 30.f, y + 7, 30.f, y + 13);
    gr.DrawLine(&p, 30.f, y + 15, 30.f, y + 16.5f);
  }
  text(gr, L"Cities in Motion not found", 11, kSoft, RectF(46, y + 4, 260, 16));
  text(gr, L"OpenCiM runs on the original game's data. Point it to your Cities in Motion folder, or get the game "
           L"first.",
       12, kSoft, RectF(20, y + 32, 290, 60));
  if (!g.gameMessage.empty()) text(gr, g.gameMessage, 11, 0xf3b06a, RectF(20, y + 92, 290, 44));
  bar(gr, RectF(4, 300, 320, 28), L"CHOOSE GAME FOLDER", HotChooseFolder, Icon::Folder, kGreen);
  bar(gr, RectF(4, 337, 320, 28), L"GET IT ON STEAM", HotSteam, Icon::Store, kBlue);
  bar(gr, RectF(4, 374, 320, 28), L"CANCEL", HotCancel, Icon::Cross, kRed);
  noteAtBottom(gr, L"Searched: Steam libraries, GOG and retail installs.");
  cover(gr, g.pics.notFound.get(), kRight);
  fill(gr, kRight, rgb(kBg, 90));
}

void pageOptions(Graphics& gr) {
  panelBase(gr);
  panelHeader(gr, L"OPTIONS");
  text(gr, L"Where OpenCiM goes and how you start it. Your saves, maps and settings stay in Documents\\OpenCiM.", 12,
       kSoft, RectF(20, 146, 290, 60));
  bool can = g.check.ok && g.payload.present();
  bar(gr, RectF(4, 270, 320, 28), L"INSTALL", HotInstall, Icon::Down, kBlue, false, can);
  bar(gr, RectF(4, 307, 320, 28), L"BACK", HotBack, Icon::Back, kGrey);
  cover(gr, g.pics.welcome.get(), kRight);
  fill(gr, kRight, rgb(kBg, 90));
  RectF box(368, 60, 616, 400);
  captionBox(gr, box);
  RectF head(box.X + 2, box.Y + 2, box.Width - 4, 48);
  text(gr, L"OPTIONS", 15, 0xd6d6d6, head, false, StringAlignmentCenter, StringAlignmentCenter, true);
  hline(gr, box.X + 12, box.GetRight() - 12, box.Y + 50, 0x604b45);
  float x = box.X + 24, w = box.Width - 48, y = box.Y + 70;
  auto row = [&](const std::wstring& label, const std::wstring& value, int id) {
    text(gr, label, 11, kDim, RectF(x, y, w, 16), false, StringAlignmentNear, StringAlignmentNear, true);
    text(gr, value, 12, 0xe9e1d6, RectF(x, y + 18, w - 90, 18), false, StringAlignmentNear, StringAlignmentCenter);
    RectF b(x + w - 80, y + 16, 80, 22);
    text(gr, L"CHANGE", 10, g.hover == id ? 0xffffff : kLink, b, true, StringAlignmentFar, StringAlignmentCenter);
    addHot(b, id);
    y += 56;
  };
  row(L"INSTALL FOLDER", g.opts.target.wstring(), HotTarget);
  row(L"GAME FOLDER", g.check.ok ? g.game.wstring() + L"  (" + dlcLine() + L")" : L"not found", HotChangeGame);
  y += 6;
  checkbox(gr, x, y, w, L"Start menu shortcuts (OpenCiM and its setup)", g.opts.startMenu, HotStartMenu);
  checkbox(gr, x, y + 30, w, L"Desktop shortcut", g.opts.desktop, HotDesktop);
  text(gr, L"No admin rights needed: OpenCiM installs for your Windows account only and never changes the game's "
           L"folder.",
       11, kDim, RectF(x, y + 76, w, 40));
}

void pageInstalling(Graphics& gr) {
  panelBase(gr);
  panelHeader(gr, L"WHAT'S NEW");
  for (int k = 0; k < 8; ++k) {
    RectF r(4, 140 + float(k) * 39, 320, 30);
    bool on = k == g.slide, hov = g.hover == HotSlide0 + k;
    if (on) {
      vgrad(gr, r, {{0.f, 0x332921}, {0.5f, 0x2a1e16}, {1.f, 0x332921}});
      hline(gr, r.X, r.GetRight(), r.Y, 0x493934);
      hline(gr, r.X, r.GetRight(), r.GetBottom(), 0x493934);
    }
    SolidBrush dot(rgb(on ? kOrange : 0x5a4a3d));
    gr.FillEllipse(&dot, r.X + 12, r.Y + 10, 10.f, 10.f);
    RectF t(r.X + 32, r.Y, r.Width - 40, r.Height);
    outlined(gr, kFeatures[k].shortName, 12, on || hov ? 0xffffff : 0xc9bcad, t, StringAlignmentNear);
    addHot(r, HotSlide0 + k);
  }
  // the progress
  float p = g.renderProgress >= 0 ? g.renderProgress : g.progress.fraction.load();
  bool done = g.whatsNew || (g.renderProgress >= 1) || g.finished;
  std::wstring step;
  if (g.whatsNew) step = L"Installed";
  else if (g.failed) step = L"Stopped";
  else if (done) step = L"Done";
  else {
    int s = g.progress.step;
    step = s == 0 ? (g.updating ? L"Updating the files" : L"Unpacking OpenCiM") : s == 1 ? L"Checking" : L"Creating shortcuts";
  }
  float by = 640 - 120;
  if (g.failed) {
    text(gr, W(g.error), 11, 0xf3b06a, RectF(12, by - 56, 304, 56), false, StringAlignmentNear, StringAlignmentFar);
  }
  text(gr, step, 11, kSoft, RectF(12, by, 200, 16));
  if (!g.whatsNew) text(gr, std::to_wstring(int(p * 100 + 0.5f)) + L" %", 11, kSoft, RectF(212, by, 104, 16), false, StringAlignmentFar);
  RectF track(12, by + 22, 304, 12);
  fill(gr, track, rgb(0x120d0a));
  Pen te(rgb(0x493934), 1);
  gr.DrawRectangle(&te, track);
  if (p > 0)
    vgrad(gr, RectF(track.X + 1, track.Y + 1, (track.Width - 2) * std::min(1.f, p), track.Height - 2),
          {{0.f, 0x8c7a64}, {0.5f, 0x6b5a46}, {1.f, 0x574836}});
  if (done || g.failed)
    bar(gr, RectF(4, by + 48, 320, 28), g.failed ? L"CLOSE" : L"CONTINUE", g.failed ? HotClose : HotContinue);
  // the slide
  double t = nowSec();
  float fade = g.prevSlide < 0 ? 1.f : float(std::clamp((t - g.fadeStart) / 0.6, 0.0, 1.0));
  if (g.prevSlide >= 0 && fade < 1) cover(gr, g.pics.slides[g.prevSlide].get(), kRight);
  cover(gr, g.pics.slides[g.slide].get(), kRight, fade);
  {
    LinearGradientBrush shade(PointF(0, 640 * 0.45f), PointF(0, 641), rgb(kBg, 0), rgb(kBg, 140));
    gr.FillRectangle(&shade, RectF(kRight.X, 640 * 0.45f, kRight.Width, 640 * 0.55f));
  }
  RectF next(kRight.GetRight() - 62, 298, 44, 44);
  {
    SolidBrush b(rgb(0x2b211b, g.hover == HotNext ? 240 : 215));
    gr.FillEllipse(&b, next);
    Pen e(rgb(0x7a6656), 1);
    gr.DrawEllipse(&e, next);
    Pen a(rgb(0xffffff), 2);
    a.SetStartCap(LineCapRound);
    a.SetEndCap(LineCapRound);
    gr.DrawLine(&a, next.X + 19, next.Y + 15, next.X + 25, next.Y + 22);
    gr.DrawLine(&a, next.X + 25, next.Y + 22, next.X + 19, next.Y + 29);
    addHot(next, HotNext);
  }
  RectF cap(kRight.X + 24, 640 - 26 - 110, kRight.Width - 48, 110);
  captionBox(gr, cap);
  std::wstring num = std::to_wstring(g.slide + 1) + L" / 8";
  text(gr, num, 13, kOrange, RectF(cap.X + 22, cap.Y + 18, 60, 28), false, StringAlignmentNear, StringAlignmentCenter);
  std::wstring head = kFeatures[g.slide].heading;
  std::transform(head.begin(), head.end(), head.begin(), ::towupper);
  text(gr, head, 24, 0xf4f1ec, RectF(cap.X + 70, cap.Y + 14, cap.Width - 90, 34), true, StringAlignmentNear,
       StringAlignmentCenter, true);
  text(gr, kFeatures[g.slide].text, 13, 0xe3dacd, RectF(cap.X + 22, cap.Y + 54, cap.Width - 44, 48));
}

void pageDone(Graphics& gr) {
  panelBase(gr);
  panelHeader(gr, L"READY TO PLAY");
  text(gr, L"OpenCiM is installed. Your saves and maps live in Documents\\OpenCiM; saves from Cities in Motion show "
           L"up there too and are copied when you open them.",
       12, kSoft, RectF(20, 146, 290, 80));
  bar(gr, RectF(4, 240, 320, 28), L"PLAY", HotPlay, Icon::Play, kBlue, true);
  bar(gr, RectF(4, 277, 320, 28), L"WHAT'S NEW", HotWhatsNew);
  bar(gr, RectF(4, 314, 320, 28), L"CLOSE", HotClose);
  noteAtBottom(gr, L"Installed in " + g.opts.target.wstring());
  cover(gr, g.pics.done.get(), kRight);
  corner(gr, L"OpenCiM " + W(g.payload.present() ? g.payload.version : REGS_VERSION_STRING),
         L"Unofficial port, needs Cities in Motion");
}

struct ActionInfo {
  std::wstring label, title, text, button;
  bool enabled;
  Icon icon;
  DotColors dot;
};

bool payloadNewer() {
  return g.payload.present() && g.payload.build > g.manifest.build;
}

ActionInfo actionInfo(int a) {
  std::wstring inst = W(g.manifest.version), mine = W(g.payload.version);
  switch (a) {
    case ActUpdate:
      if (payloadNewer())
        return {L"UPDATE TO " + mine, L"UPDATE",
                L"Installs OpenCiM " + mine + L" over " + inst + L": replaces only the files OpenCiM installed. Your "
                L"options, mods and the game folder stay as they are.",
                L"UPDATE NOW", true, Icon::Down, kBlue};
      return {L"UPDATE", L"UPDATE",
              g.payload.present()
                  ? L"This setup program carries " + mine + L" (build " + std::to_wstring(g.payload.build) +
                        L"), not newer than the installed " + inst + L". New versions come as a new setup program."
                  : L"New versions come as a new setup program. Downloading them from inside this page comes when "
                    L"OpenCiM's releases are published on GitHub.",
              L"UPDATE NOW", false, Icon::Down, kBlue};
    case ActRepair:
      return {L"REPAIR", L"REPAIR",
              L"Checks every installed file against the list from the install and puts back the ones that are missing "
              L"or changed" +
                  std::wstring(g.payload.present() && g.payload.build == g.manifest.build
                                   ? L"."
                                   : L" (that needs the setup program of the installed version, " + inst + L")."),
              L"CHECK AND REPAIR", true, Icon::Wrench, kGrey};
    case ActFolder:
      return {L"CHANGE GAME FOLDER", L"GAME FOLDER",
              L"Point OpenCiM at your Cities in Motion folder, for example after moving the game to another drive or "
              L"Steam library. In use now: " +
                  g.manifest.game.wstring(),
              L"CHOOSE FOLDER", true, Icon::Folder, kGreen};
    default:
      return {L"UNINSTALL", L"UNINSTALL",
              L"Removes OpenCiM's program files, shortcuts and its entry in Apps & features. Cities in Motion itself "
              L"is not touched.",
              g.removed ? L"CLOSE" : L"UNINSTALL", true, Icon::Trash, kRed};
  }
}

void pageMaintenance(Graphics& gr) {
  panelBase(gr);
  panelHeader(gr, L"SETUP");
  float y = 142;
  auto info = [&](const std::wstring& k, const std::wstring& v, uint32_t c = kSoft) {
    text(gr, k, 11, kDim, RectF(20, y, 140, 16));
    text(gr, v, 11, c, RectF(130, y, 180, 16), c != kSoft, StringAlignmentFar);
    y += 20;
  };
  info(L"Installed", W(g.manifest.version) + L" (build " + std::to_wstring(g.manifest.build) + L")");
  if (g.payload.present())
    info(L"This setup", W(g.payload.version) + L" (build " + std::to_wstring(g.payload.build) + L")",
         payloadNewer() ? 0xf3b06a : kSoft);
  info(L"Channel", W(g.manifest.channel));
  text(gr, L"Game folder", 11, kDim, RectF(20, y, 140, 16));
  text(gr, g.manifest.game.wstring(), 10, kSoft, RectF(20, y + 16, 290, 28));
  y += 54;
  for (int a = 0; a < 4; ++a) {
    ActionInfo ai = actionInfo(a);
    bar(gr, RectF(4, y + float(a) * 41, 320, 32), ai.label, HotAction0 + a, ai.icon, ai.dot, a == g.action,
        !g.working && !g.removed);
  }
  noteAtBottom(gr, L"Also under Apps & features and in the Start menu. The game's own files are never changed.");
  cover(gr, g.pics.maintenance.get(), kRight);
  fill(gr, kRight, rgb(kBg, 90));
  ActionInfo ai = actionInfo(g.action);
  RectF box(368, 60, 616, 330);
  captionBox(gr, box);
  text(gr, ai.title, 15, 0xd6d6d6, RectF(box.X, box.Y + 2, box.Width, 48), false, StringAlignmentCenter,
       StringAlignmentCenter, true);
  hline(gr, box.X + 12, box.GetRight() - 12, box.Y + 50, 0x604b45);
  text(gr, ai.text, 13, 0xe3dacd, RectF(box.X + 24, box.Y + 66, box.Width - 48, 90));
  float cy = box.Y + 166;
  if (g.action == ActUninstall && !g.removed) {
    checkbox(gr, box.X + 24, cy, box.Width - 48, L"Keep my saves, maps and settings (Documents\\OpenCiM)", g.keepDocs,
             HotKeepDocs);
  }
  if (!g.panelStatus.empty())
    text(gr, g.panelStatus, 12, 0xf3d3a6, RectF(box.X + 24, cy + 30, box.Width - 48, 60));
  RectF btn(box.GetRight() - 24 - 240, box.GetBottom() - 52, 240, 32);
  bool en = ai.enabled && !g.working;
  vgrad(gr, btn, {{0.f, 0x413328}, {0.2f, 0x3d2f25}, {0.3f, 0x16110c}, {0.52f, 0x150f0b}, {0.58f, 0x3b2d22},
                  {1.f, 0x42342a}});
  hline(gr, btn.X, btn.GetRight(), btn.Y, g.hover == HotPanelButton ? 0xa08070 : 0x604b45);
  outlined(gr, ai.button, 13, en ? 0xe6e6e6 : 0x857a70, btn, StringAlignmentCenter, true);
  if (en) addHot(btn, HotPanelButton);
}

void paint(Graphics& gr) {
  g.hot.clear();
  gr.SetSmoothingMode(SmoothingModeAntiAlias);
  gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  gr.SetInterpolationMode(InterpolationModeHighQualityBilinear);
  gr.SetPixelOffsetMode(PixelOffsetModeHalf);
  fill(gr, RectF(0, 0, 1024, 640), rgb(kBg));
  switch (g.page) {
    case Page::Welcome: pageWelcome(gr); break;
    case Page::NotFound: pageNotFound(gr); break;
    case Page::Options: pageOptions(gr); break;
    case Page::Installing: pageInstalling(gr); break;
    case Page::Done: pageDone(gr); break;
    case Page::Maintenance: pageMaintenance(gr); break;
  }
  // no Windows title bar (owner's request 2026-11-19): the window's edge and its own close button, top right
  RectF x(1024 - 44, 10, 32, 32);
  bool hov = g.hover == HotX;
  {
    SolidBrush b(hov ? rgb(0xc0392b, 235) : rgb(0x1e1a16, 200));
    gr.FillEllipse(&b, x);
    Pen e(rgb(hov ? 0xf08a6a : 0x7a6656), 1);
    gr.DrawEllipse(&e, x);
    Pen p(rgb(0xffffff), 2);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    float c = x.X + 16, m = x.Y + 16, s = 5.5f;
    gr.DrawLine(&p, c - s, m - s, c + s, m + s);
    gr.DrawLine(&p, c + s, m - s, c - s, m + s);
  }
  addHot(x, HotX);
  Pen edge(rgb(kPanelEdge), 1);
  gr.DrawRectangle(&edge, RectF(0.5f, 0.5f, 1023, 639));
}

// ---------------------------------------------------------------------------------------------------------------------
// actions

void redraw() {
  if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}

void setGame(const fs::path& dir, const std::string& source) {
  g.game = dir;
  g.check = checkGameFolder(dir);
  g.gameSource = source;
  g.opts.game = dir;
}

void searchGame() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"OPENCIM_SETUP_GAME", buf, DWORD(std::size(buf)));
  if (n > 0 && n < std::size(buf)) {
    if (std::wstring(buf) == L"none") {
      g.check = checkGameFolder({});
      return;
    }
    setGame(fs::path(buf), "Steam");
    return;
  }
  std::string src;
  fs::path found = findGame(&src);
  if (!found.empty()) setGame(found, src);
  else g.check = checkGameFolder({});
}

fs::path pickFolder(const std::wstring& title, const fs::path& start) {
  fs::path out;
  IFileOpenDialog* d = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
  DWORD o = 0;
  d->GetOptions(&o);
  d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  d->SetTitle(title.c_str());
  IShellItem* si = nullptr;
  if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
    d->SetFolder(si);
    si->Release();
  }
  if (SUCCEEDED(d->Show(g.hwnd))) {
    IShellItem* r = nullptr;
    if (SUCCEEDED(d->GetResult(&r))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = p;
      CoTaskMemFree(p);
      r->Release();
    }
  }
  d->Release();
  return out;
}

// a picked game folder: the folder itself, or the game inside it (a Steam library or "steamapps\common")
bool acceptGameFolder(const fs::path& picked, fs::path& game) {
  for (const fs::path& c : {picked, picked / "Cities In Motion", picked / "common" / "Cities In Motion",
                            picked / "steamapps" / "common" / "Cities In Motion"})
    if (checkGameFolder(c).ok) {
      game = c;
      return true;
    }
  return false;
}

void startWorker(std::function<bool(std::string&)> job) {
  if (g.worker.joinable()) g.worker.join();
  g.working = true;
  g.failed = false;
  g.finished = false;
  g.progress.fraction = 0;
  g.progress.step = 0;
  g.worker = std::thread([job] {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string err;
    bool ok = job(err);
    CoUninitialize();
    g.error = err;
    g.failed = !ok;
    g.finished = ok;
    g.working = false;
  });
}

void beginInstall() {
  g.page = Page::Installing;
  g.slide = 0;
  g.prevSlide = -1;
  g.slideStart = nowSec();
  g.whatsNew = false;
  InstallOptions o = g.opts;
  startWorker([o](std::string& err) { return installFromPayload(g.payload, g.self, o, g.progress, err); });
}

void showSlide(int k) {
  if (k == g.slide) return;
  g.prevSlide = g.slide;
  g.slide = (k + 8) % 8;
  g.fadeStart = nowSec();
  g.slideStart = g.fadeStart;
}

void launchGame() {
  fs::path exe = g.opts.target / "regs.exe";
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> line(cmd.begin(), cmd.end());
  line.push_back(0);
  if (CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, g.opts.target.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

void runAction() {
  g.panelStatus.clear();
  switch (g.action) {
    case ActUpdate: {
      if (!payloadNewer()) return;
      g.updating = true;
      g.opts.target = g.installed;
      g.opts.game = g.manifest.game;
      g.opts.startMenu = g.manifest.startMenu;
      g.opts.desktop = g.manifest.desktop;
      beginInstall();
      return;
    }
    case ActRepair: {
      std::vector<std::string> bad = changedFiles(g.installed, g.manifest);
      if (bad.empty()) {
        g.panelStatus = L"All " + std::to_wstring(g.manifest.files.size()) + L" files are as installed.";
        return;
      }
      if (!(g.payload.present() && g.payload.build == g.manifest.build)) {
        g.panelStatus = std::to_wstring(bad.size()) + L" files are missing or changed (e.g. " + W(bad[0]) +
                        L"). Run the setup program of version " + W(g.manifest.version) + L" to put them back.";
        return;
      }
      g.panelStatus = L"Repairing " + std::to_wstring(bad.size()) + L" files...";
      InstallOptions o;
      o.target = g.installed;
      o.game = g.manifest.game;
      o.startMenu = g.manifest.startMenu;
      o.desktop = g.manifest.desktop;
      size_t n = bad.size();
      startWorker([o, n](std::string& err) {
        bool ok = installFromPayload(g.payload, g.self, o, g.progress, err);
        if (ok) g.panelStatus = L"Repaired: " + std::to_wstring(n) + L" files put back.";
        return ok;
      });
      return;
    }
    case ActFolder: {
      fs::path picked = pickFolder(L"Your Cities in Motion folder", g.manifest.game);
      if (picked.empty()) return;
      fs::path game;
      if (!acceptGameFolder(picked, game)) {
        g.panelStatus = L"No Cities in Motion in " + picked.wstring() + L" (data.gs and the other archives are missing).";
        return;
      }
      std::string err;
      if (changeGameFolder(g.installed, game, err)) {
        g.manifest.game = game;
        GameCheck c = checkGameFolder(game);
        g.panelStatus = L"OpenCiM now uses " + game.wstring() + L" (" + std::to_wstring(c.dlcs.size()) + L" DLCs).";
      } else {
        g.panelStatus = W(err);
      }
      return;
    }
    case ActUninstall: {
      if (g.removed) {
        PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
        return;
      }
      if (!g.keepDocs &&
          MessageBoxW(g.hwnd,
                      (L"This also deletes your OpenCiM saves, maps and settings in\n" + places().documents.wstring() +
                       L"\n\nThey cannot be brought back. Continue?")
                          .c_str(),
                      L"OpenCiM Setup", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
      bool later = false;
      std::string err;
      bool ok = uninstall(g.installed, g.keepDocs, g.self, later, err);
      g.removed = ok;
      g.panelStatus = ok ? L"OpenCiM was removed." + std::wstring(g.keepDocs ? L" Your saves and maps are still in Documents\\OpenCiM." : L"")
                         : W(err);
      return;
    }
  }
}

void click(int id) {
  if (id >= HotSlide0 && id < HotSlide0 + 8) return showSlide(id - HotSlide0);
  if (id >= HotAction0 && id < HotAction0 + 4) {
    g.action = id - HotAction0;
    g.panelStatus.clear();
    return;
  }
  switch (id) {
    case HotInstall:
      if (g.check.ok && g.payload.present()) beginInstall();
      break;
    case HotOptions: g.page = Page::Options; break;
    case HotBack: g.page = Page::Welcome; break;
    case HotCancel:
    case HotClose: PostMessageW(g.hwnd, WM_CLOSE, 0, 0); break;
    case HotChangeGame:
    case HotChooseFolder: {
      fs::path picked = pickFolder(L"Your Cities in Motion folder", g.game);
      if (picked.empty()) break;
      fs::path game;
      if (acceptGameFolder(picked, game)) {
        setGame(game, "your folder");
        g.gameMessage.clear();
        if (g.page == Page::NotFound) g.page = Page::Welcome;
      } else {
        g.gameMessage = L"No Cities in Motion in " + picked.wstring() + L": data.gs and the other archives are missing.";
        if (g.page == Page::Welcome) g.page = Page::NotFound;
      }
      break;
    }
    case HotSteam: ShellExecuteW(g.hwnd, L"open", L"https://store.steampowered.com/app/73010/", nullptr, nullptr, SW_SHOWNORMAL); break;
    case HotTarget: {
      fs::path picked = pickFolder(L"Install OpenCiM in", g.opts.target.parent_path());
      if (!picked.empty()) g.opts.target = picked.filename() == L"OpenCiM" ? picked : picked / L"OpenCiM";
      break;
    }
    case HotStartMenu: g.opts.startMenu = !g.opts.startMenu; break;
    case HotDesktop: g.opts.desktop = !g.opts.desktop; break;
    case HotNext: showSlide(g.slide + 1); break;
    case HotContinue:
      if (g.updating) {
        g.updating = false;
        g.manifest.load(g.installed);
        g.page = Page::Maintenance;
        g.panelStatus = L"Updated to " + W(g.manifest.version) + L".";
      } else {
        g.page = g.whatsNew ? Page::Done : Page::Done;
      }
      g.whatsNew = false;
      break;
    case HotPlay:
      launchGame();
      PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
      break;
    case HotWhatsNew:
      g.whatsNew = true;
      g.page = Page::Installing;
      g.prevSlide = -1;
      g.slideStart = nowSec();
      break;
    case HotKeepDocs: g.keepDocs = !g.keepDocs; break;
    case HotX: PostMessageW(g.hwnd, WM_CLOSE, 0, 0); break;
    case HotPanelButton: runAction(); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// the window

int hitTest(float x, float y) {
  x /= g.scale, y /= g.scale;
  for (auto it = g.hot.rbegin(); it != g.hot.rend(); ++it)
    if (it->r.Contains(x, y)) return it->id;
  return HotNone;
}

// round corners (owner's request 2026-11-19): Windows 11 rounds the window itself, antialiased and with its thin
// border (DWMWA_WINDOW_CORNER_PREFERENCE); Windows 10 has no such attribute and gets a rounded window region instead.
// OPENCIM_SETUP_REGION=1 forces the region (tests).
void roundCorners(HWND h) {
  if (!GetEnvironmentVariableW(L"OPENCIM_SETUP_REGION", nullptr, 0)) {
    DWORD pref = 2;  // DWMWCP_ROUND
    if (SUCCEEDED(DwmSetWindowAttribute(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof(pref)))) return;
  }
  RECT r;
  GetWindowRect(h, &r);
  int d = int(16 * g.scale);
  SetWindowRgn(h, CreateRoundRectRgn(0, 0, r.right - r.left + 1, r.bottom - r.top + 1, d, d), TRUE);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      {
        Graphics gr(mem);
        gr.ScaleTransform(g.scale, g.scale);
        paint(gr);
      }
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: {
      // without a title bar: any spot that is not a button moves the window (the showcase picture is a button)
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(h, &pt);
      int id = hitTest(float(pt.x), float(pt.y));
      bool picture = g.page == Page::Installing && float(pt.x) / g.scale > 328;
      return id == HotNone && !picture ? HTCAPTION : HTCLIENT;
    }
    case WM_MOUSEMOVE: {
      int id = hitTest(float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)));
      if (id != g.hover) {
        g.hover = id;
        SetCursor(LoadCursorW(nullptr, id ? IDC_HAND : IDC_ARROW));
        redraw();
      }
      TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
      TrackMouseEvent(&t);
      return 0;
    }
    case WM_MOUSELEAVE:
      g.hover = HotNone;
      redraw();
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, g.hover ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_LBUTTONUP: {
      int id = hitTest(float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)));
      if (id == HotNone && g.page == Page::Installing && float(GET_X_LPARAM(lp)) / g.scale > 328) id = HotNext;
      if (id != HotNone) click(id);
      redraw();
      return 0;
    }
    case WM_TIMER: {
      double now = nowSec();
      if (g.page == Page::Installing && now - g.slideStart > 7.0) showSlide(g.slide + 1);
      bool fading = false;
      if (g.page == Page::Welcome && g.pics.welcomeShots.size() > 1) {
        if (g.shotStart == 0) g.shotStart = now;
        if (now - g.shotStart > 6.0) {
          g.prevShot = g.shot;
          g.shot = (g.shot + 1) % int(g.pics.welcomeShots.size());
          g.shotFade = g.shotStart = now;
        }
        fading = g.prevShot >= 0 && now - g.shotFade < 1.1;
      }
      if (g.page == Page::Installing || g.working || fading) redraw();
      return 0;
    }
    case WM_DPICHANGED: {
      g.scale = HIWORD(wp) / 96.0f;
      RECT* r = reinterpret_cast<RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      roundCorners(h);
      redraw();
      return 0;
    }
    case WM_CLOSE:
      if (g.working &&
          MessageBoxW(h, L"OpenCiM is being installed. Stop and close the setup?", L"OpenCiM Setup",
                      MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 0;
      DestroyWindow(h);
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

int runWindow(bool uninstallFirst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(2));
  wc.lpszClassName = L"OpenCiMSetup";
  wc.style = CS_DROPSHADOW;
  RegisterClassExW(&wc);
  UINT dpi = GetDpiForSystem();
  g.scale = dpi / 96.0f;
  // a popup without the Windows title bar (its own X, dragged by any free spot); still on the taskbar, Alt+F4 and
  // the taskbar's Close work through the system menu
  DWORD style = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX;
  int w = int(1024 * g.scale), hgt = int(640 * g.scale);
  int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2;
  g.hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"OpenCiM Setup", style, x, y, w, hgt, nullptr, nullptr,
                           wc.hInstance, nullptr);
  UINT real = GetDpiForWindow(g.hwnd);
  if (real != dpi) {
    g.scale = real / 96.0f;
    SetWindowPos(g.hwnd, nullptr, 0, 0, int(1024 * g.scale), int(640 * g.scale), SWP_NOMOVE | SWP_NOZORDER);
  }
  roundCorners(g.hwnd);
  if (uninstallFirst && g.page == Page::Maintenance) g.action = ActUninstall;
  ShowWindow(g.hwnd, SW_SHOW);
  SetTimer(g.hwnd, 1, 33, nullptr);
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  if (g.worker.joinable()) g.worker.join();
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// the command line

void attachConsole() {
  // output redirected to a file or pipe stays there; otherwise the console of the shell that started us
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD type = out && out != INVALID_HANDLE_VALUE ? GetFileType(out) : FILE_TYPE_UNKNOWN;
  if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) return;
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
  }
}

bool encoderClsid(const wchar_t* mime, CLSID& out) {
  UINT n = 0, size = 0;
  GetImageEncodersSize(&n, &size);
  std::vector<uint8_t> buf(size);
  auto* enc = reinterpret_cast<ImageCodecInfo*>(buf.data());
  GetImageEncoders(n, size, enc);
  for (UINT i = 0; i < n; ++i)
    if (std::wstring(enc[i].MimeType) == mime) {
      out = enc[i].Clsid;
      return true;
    }
  return false;
}

int renderPage(const std::wstring& page, const fs::path& out, float scale, int slide, float progress, int action) {
  if (page == L"welcome") g.page = Page::Welcome;
  else if (page == L"notfound") g.page = Page::NotFound;
  else if (page == L"options") g.page = Page::Options;
  else if (page == L"installing") g.page = Page::Installing;
  else if (page == L"done") g.page = Page::Done;
  else if (page == L"maintenance") g.page = Page::Maintenance;
  else return 2;
  g.slide = std::clamp(slide, 0, 7);
  g.renderProgress = progress;
  g.action = std::clamp(action, 0, 3);
  if (g.page == Page::Maintenance && g.installed.empty()) {
    g.manifest.version = "0.2.0";
    g.manifest.build = 193;
    g.manifest.game = g.game.empty() ? fs::path(L"D:\\SteamLibrary\\steamapps\\common\\Cities In Motion") : g.game;
  }
  Bitmap bmp(INT(1024 * scale), INT(640 * scale), PixelFormat32bppARGB);
  {
    Graphics gr(&bmp);
    gr.ScaleTransform(scale, scale);
    paint(gr);
  }
  CLSID png;
  if (!encoderClsid(L"image/png", png)) return 1;
  return bmp.Save(out.c_str(), &png, nullptr) == Ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  GdiplusStartupInput gsi;
  ULONG_PTR gtoken = 0;
  GdiplusStartup(&gtoken, &gsi, nullptr);
  fonts = new Fonts();
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  wchar_t selfBuf[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, selfBuf, DWORD(std::size(selfBuf)));
  g.self = fs::path(selfBuf);
  g.payload.open(g.self);
  std::vector<std::wstring> args(argv + 1, argv + argc);
  auto has = [&](const wchar_t* f) { return std::find(args.begin(), args.end(), f) != args.end(); };
  auto value = [&](const wchar_t* f) -> std::wstring {
    for (size_t i = 0; i + 1 < args.size(); ++i)
      if (args[i] == f) return args[i + 1];
    return {};
  };
  int rc = 0;
  const Places pl = places();
  g.opts.target = pl.defaultInstall;
  if (has(L"--make") && args.size() >= 5) {
    attachConsole();
    size_t i = std::find(args.begin(), args.end(), L"--make") - args.begin();
    bool ok = i + 4 < args.size() + 0 &&
              makeOfflineSetup(args[i + 1], args[i + 2], args[i + 3], args[i + 4], REGS_VERSION_STRING,
                               REGS_BUILD_NUMBER, [](const std::string& s) { std::printf("%s\n", s.c_str()); });
    rc = ok ? 0 : 1;
  } else if (has(L"--silent-install")) {
    attachConsole();
    if (!g.payload.present()) {
      std::printf("this setup program has no payload\n");
      rc = 1;
    } else {
      InstallOptions o;
      o.target = value(L"--target").empty() ? pl.defaultInstall : fs::absolute(value(L"--target"));
      fs::path game = value(L"--game");
      if (game.empty()) game = findGame();
      if (game.empty() || !checkGameFolder(game).ok) {
        std::printf("Cities in Motion not found (pass --game <folder>)\n");
        rc = 3;
      } else {
        o.game = game;
        o.desktop = has(L"--desktop");
        o.startMenu = !has(L"--no-start-menu");
        Progress p;
        std::string err;
        bool ok = installFromPayload(g.payload, g.self, o, p, err);
        std::printf(ok ? "installed %s (build %d) in %s\n" : "install failed: %s%s%s\n",
                    ok ? g.payload.version.c_str() : err.c_str(), ok ? g.payload.build : 0,
                    ok ? pathToUtf8(o.target).c_str() : "");
        rc = ok ? 0 : 1;
      }
    }
  } else if (has(L"--verify")) {
    attachConsole();
    fs::path dir = value(L"--target").empty() ? findInstall(g.self) : fs::absolute(value(L"--target"));
    Manifest m;
    if (dir.empty() || !m.load(dir)) {
      std::printf("no OpenCiM install found\n");
      rc = 2;
    } else {
      std::vector<std::string> bad = changedFiles(dir, m);
      for (const std::string& f : bad) std::printf("changed or missing: %s\n", f.c_str());
      std::printf("%s %s (build %d): %zu files, %zu changed or missing\n", pathToUtf8(dir).c_str(), m.version.c_str(),
                  m.build, m.files.size(), bad.size());
      rc = bad.empty() ? 0 : 1;
    }
  } else if (has(L"--silent-uninstall")) {
    attachConsole();
    fs::path dir = value(L"--target").empty() ? findInstall(g.self) : fs::absolute(value(L"--target"));
    bool later = false;
    std::string err;
    bool ok = !dir.empty() && uninstall(dir, !has(L"--remove-documents"), g.self, later, err);
    std::printf(ok ? "removed %s\n" : "uninstall failed: %s\n", ok ? pathToUtf8(dir).c_str() : err.c_str());
    rc = ok ? 0 : 1;
  } else {
    g.installed = findInstall(g.self);
    if (!g.installed.empty() && g.manifest.load(g.installed)) {
      g.page = Page::Maintenance;
      g.opts.target = g.installed;
      setGame(g.manifest.game, "install.cfg");
    } else {
      g.installed.clear();
      searchGame();
      g.page = g.check.ok ? Page::Welcome : Page::NotFound;
    }
    loadPictures();
    if (has(L"--render") && args.size() >= 3) {
      size_t i = std::find(args.begin(), args.end(), L"--render") - args.begin();
      std::wstring sc = value(L"--scale"), sl = value(L"--slide"), pr = value(L"--progress"), ac = value(L"--action");
      rc = i + 2 < args.size() + 0
               ? renderPage(args[i + 1], args[i + 2], sc.empty() ? 1.f : std::stof(sc), sl.empty() ? 0 : std::stoi(sl),
                            pr.empty() ? -1.f : std::stof(pr), ac.empty() ? 0 : std::stoi(ac))
               : 2;
    } else {
      rc = runWindow(has(L"--uninstall"));
    }
  }
  LocalFree(argv);
  delete fonts;
  g.pics = Pictures{};
  GdiplusShutdown(gtoken);
  CoUninitialize();
  return rc;
}
