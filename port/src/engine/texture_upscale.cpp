#include "engine/texture_upscale.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include "core/text.h"
#include "render/mesh.h"
#include "vfs/vfs.h"

namespace fs = std::filesystem;
using regs::render::Image;

namespace regs::engine::texup {

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Color: return "colour";
    case Kind::AlphaTest: return "alpha-tested";
    case Kind::Normal: return "normal";
    case Kind::Mask: return "mask";
    case Kind::Ui: return "ui";
    default: return "skip";
  }
}

uint64_t sourceStamp(const std::vector<uint8_t>& file) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint8_t b : file) h = (h ^ b) * 0x100000001b3ull;
  return h ^ (uint64_t(file.size()) * 0x9E3779B97F4A7C15ull);
}

namespace {

fs::path widen(const std::string& s) {
  auto w = utf8ToUtf16(s);
  return regs::pathFromUtf16(w);
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

bool endsWith(const std::string& s, const char* e) {
  size_t n = std::strlen(e);
  return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

// ---- float RGBA images -------------------------------------------------------------------------------------

struct Pix {
  int w = 0, h = 0;
  std::vector<float> d;  // RGBA 0..1, row 0 first
  float* at(int x, int y) { return d.data() + (size_t(y) * size_t(w) + size_t(x)) * 4; }
  const float* at(int x, int y) const { return d.data() + (size_t(y) * size_t(w) + size_t(x)) * 4; }
};

int edge(int i, int n, bool wrap) { return wrap ? ((i % n) + n) % n : std::clamp(i, 0, n - 1); }

// Colours for the texels alpha leaves out (alpha < cut): the mean of their already-filled neighbours, ring by ring,
// so filters that reach across an edge see the edge colour instead of the black that DXT1 stores there
void bleed(Pix& p, float cut, bool wrap) {
  size_t n = size_t(p.w) * size_t(p.h);
  std::vector<uint8_t> ok(n);
  size_t holes = 0;
  for (size_t i = 0; i < n; ++i) holes += !(ok[i] = p.d[i * 4 + 3] >= cut);
  if (!holes || holes == n) return;
  std::vector<uint8_t> next;
  for (int pass = 0; pass < 16 && holes; ++pass) {
    next = ok;
    for (int y = 0; y < p.h; ++y)
      for (int x = 0; x < p.w; ++x) {
        size_t i = size_t(y) * size_t(p.w) + size_t(x);
        if (ok[i]) continue;
        float s[3] = {0, 0, 0};
        int c = 0;
        const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
          int nx = edge(x + dx[k], p.w, wrap), ny = edge(y + dy[k], p.h, wrap);
          size_t j = size_t(ny) * size_t(p.w) + size_t(nx);
          if (!ok[j]) continue;
          for (int ch = 0; ch < 3; ++ch) s[ch] += p.d[j * 4 + size_t(ch)];
          ++c;
        }
        if (!c) continue;
        for (int ch = 0; ch < 3; ++ch) p.d[i * 4 + size_t(ch)] = s[ch] / float(c);
        next[i] = 1;
        --holes;
      }
    ok.swap(next);
  }
  if (holes) {  // far from any edge: the mean colour
    double s[3] = {0, 0, 0};
    size_t c = 0;
    for (size_t i = 0; i < n; ++i)
      if (ok[i]) {
        for (int ch = 0; ch < 3; ++ch) s[ch] += p.d[i * 4 + size_t(ch)];
        ++c;
      }
    for (size_t i = 0; i < n; ++i)
      if (!ok[i])
        for (int ch = 0; ch < 3; ++ch) p.d[i * 4 + size_t(ch)] = float(s[ch] / double(c));
  }
}

// Lanczos-3 weights for the two output phases of a 2× upscale: output 2i sits at source i - 0.25 (taps i-3..i+2),
// output 2i+1 at i + 0.25 (taps i-2..i+3)
struct Phases {
  float even[6], odd[6];
  Phases() {
    auto L = [](double x) {
      if (std::fabs(x) < 1e-9) return 1.0;
      if (std::fabs(x) >= 3) return 0.0;
      const double pi = 3.14159265358979323846;
      return 3 * std::sin(pi * x) * std::sin(pi * x / 3) / (pi * pi * x * x);
    };
    double se = 0, so = 0, e[6], o[6];
    for (int k = 0; k < 6; ++k) {
      e[k] = L(2.75 - k);  // distance from i - 0.25 to tap i-3+k
      o[k] = L(2.25 - k);  // from i + 0.25 to tap i-2+k
      se += e[k];
      so += o[k];
    }
    for (int k = 0; k < 6; ++k) even[k] = float(e[k] / se), odd[k] = float(o[k] / so);
  }
};

// 2× Lanczos-3, clamped per channel to the range of the 2×2 source texels around each output texel (no ringing
// halos; edges stay as steep as the filter makes them)
Pix lanczos2x(const Pix& s, bool wrap) {
  static const Phases ph;
  Pix t;
  t.w = s.w * 2;
  t.h = s.h;
  t.d.resize(size_t(t.w) * size_t(t.h) * 4);
  for (int y = 0; y < s.h; ++y)
    for (int X = 0; X < t.w; ++X) {
      int i = X >> 1;
      const float* wt = (X & 1) ? ph.odd : ph.even;
      int j0 = (X & 1) ? i - 2 : i - 3;
      float acc[4] = {0, 0, 0, 0};
      for (int k = 0; k < 6; ++k) {
        const float* p = s.at(edge(j0 + k, s.w, wrap), y);
        for (int c = 0; c < 4; ++c) acc[c] += wt[k] * p[c];
      }
      std::memcpy(t.at(X, y), acc, sizeof acc);
    }
  Pix o;
  o.w = t.w;
  o.h = s.h * 2;
  o.d.resize(size_t(o.w) * size_t(o.h) * 4);
  for (int Y = 0; Y < o.h; ++Y) {
    int i = Y >> 1;
    const float* wt = (Y & 1) ? ph.odd : ph.even;
    int j0 = (Y & 1) ? i - 2 : i - 3;
    int ya = edge((Y - 1) >> 1, s.h, wrap), yb = edge(((Y - 1) >> 1) + 1, s.h, wrap);
    for (int X = 0; X < o.w; ++X) {
      float acc[4] = {0, 0, 0, 0};
      for (int k = 0; k < 6; ++k) {
        const float* p = t.at(X, edge(j0 + k, t.h, wrap));
        for (int c = 0; c < 4; ++c) acc[c] += wt[k] * p[c];
      }
      int xa = edge((X - 1) >> 1, s.w, wrap), xb = edge(((X - 1) >> 1) + 1, s.w, wrap);
      const float* q[4] = {s.at(xa, ya), s.at(xb, ya), s.at(xa, yb), s.at(xb, yb)};
      float* out = o.at(X, Y);
      for (int c = 0; c < 4; ++c) {
        float lo = std::min({q[0][c], q[1][c], q[2][c], q[3][c]}), hi = std::max({q[0][c], q[1][c], q[2][c], q[3][c]});
        out[c] = std::clamp(acc[c], lo, hi);
      }
    }
  }
  return o;
}

// Contrast-adaptive sharpening (after AMD FidelityFX CAS, MIT): sharpens most where local contrast is low, so flat
// detail gains bite without haloing the strong edges. RGB only.
void cas(Pix& p, float sharpness, bool wrap) {
  if (sharpness <= 0) return;
  Pix s = p;
  float peak = -1.0f / (8.0f - 3.0f * std::clamp(sharpness, 0.0f, 1.0f));
  for (int y = 0; y < p.h; ++y) {
    int yu = edge(y - 1, p.h, wrap), yd = edge(y + 1, p.h, wrap);
    for (int x = 0; x < p.w; ++x) {
      int xl = edge(x - 1, p.w, wrap), xr = edge(x + 1, p.w, wrap);
      const float *a = s.at(x, yu), *b = s.at(xl, y), *c = s.at(x, y), *d = s.at(xr, y), *e = s.at(x, yd);
      const float *f = s.at(xl, yu), *g = s.at(xr, yu), *h = s.at(xl, yd), *i = s.at(xr, yd);
      float* out = p.at(x, y);
      for (int ch = 0; ch < 3; ++ch) {
        float mn = std::min({a[ch], b[ch], c[ch], d[ch], e[ch]});
        float mx = std::max({a[ch], b[ch], c[ch], d[ch], e[ch]});
        mn += std::min({mn, f[ch], g[ch], h[ch], i[ch]});
        mx += std::max({mx, f[ch], g[ch], h[ch], i[ch]});
        float amp = mx > 1e-5f ? std::clamp(std::min(mn, 2.0f - mx) / mx, 0.0f, 1.0f) : 0.0f;
        float w = std::sqrt(amp) * peak;
        float v = ((a[ch] + b[ch] + d[ch] + e[ch]) * w + c[ch]) / (1.0f + 4.0f * w);
        out[ch] = std::clamp(v, 0.0f, 1.0f);
      }
    }
  }
}

// ---- block compression ---------------------------------------------------------------------------------------

struct C3 {
  float r, g, b;
};

uint16_t to565(C3 c) {
  int r = std::clamp(int(std::lround(c.r * 31.0f / 255.0f)), 0, 31);
  int g = std::clamp(int(std::lround(c.g * 63.0f / 255.0f)), 0, 63);
  int b = std::clamp(int(std::lround(c.b * 31.0f / 255.0f)), 0, 31);
  return uint16_t(r << 11 | g << 5 | b);
}

C3 from565(uint16_t v) {  // as the decoder expands it
  return {float((v >> 11 & 31) * 255 / 31), float((v >> 5 & 63) * 255 / 63), float((v & 31) * 255 / 31)};
}

// palette of a colour block as the decoder (image.cpp, and the GPU within rounding) makes it
void palette(uint16_t c0, uint16_t c1, bool four, C3 pal[4]) {
  pal[0] = from565(c0);
  pal[1] = from565(c1);
  if (four) {
    pal[2] = {float(int(2 * pal[0].r + pal[1].r) / 3), float(int(2 * pal[0].g + pal[1].g) / 3), float(int(2 * pal[0].b + pal[1].b) / 3)};
    pal[3] = {float(int(pal[0].r + 2 * pal[1].r) / 3), float(int(pal[0].g + 2 * pal[1].g) / 3), float(int(pal[0].b + 2 * pal[1].b) / 3)};
  } else {
    pal[2] = {float(int(pal[0].r + pal[1].r) / 2), float(int(pal[0].g + pal[1].g) / 2), float(int(pal[0].b + pal[1].b) / 2)};
    pal[3] = {0, 0, 0};
  }
}

float dist2(C3 a, C3 b) {
  float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
  return dr * dr * 0.30f + dg * dg * 0.59f + db * db * 0.11f;
}

// indices for c0/c1 (four-colour mode, or three colours + transparent), returns the error
float assign(const C3 px[16], const bool use[16], const bool clear[16], uint16_t c0, uint16_t c1, bool four, uint8_t idx[16]) {
  C3 pal[4];
  palette(c0, c1, four, pal);
  float err = 0;
  for (int i = 0; i < 16; ++i) {
    if (clear[i]) {
      idx[i] = 3;
      continue;
    }
    int best = 0;
    float bd = 1e30f;
    for (int k = 0; k < (four ? 4 : 3); ++k) {
      float d = dist2(px[i], pal[k]);
      if (d < bd) bd = d, best = k;
    }
    idx[i] = uint8_t(best);
    if (use[i]) err += bd;
  }
  return err;
}

// least-squares endpoints for fixed indices (weights of c0 per index: 1, 0, 2/3, 1/3 or 1, 0, 1/2)
bool refit(const C3 px[16], const bool use[16], const uint8_t idx[16], bool four, C3& e0, C3& e1) {
  double aa = 0, bb = 0, ab = 0;
  double ax[3] = {0, 0, 0}, bx[3] = {0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    if (!use[i] || idx[i] == 3 && !four) continue;
    static const double w4[4] = {1.0, 0.0, 2.0 / 3.0, 1.0 / 3.0}, w3[3] = {1.0, 0.0, 0.5};
    double a = four ? w4[idx[i]] : w3[idx[i]], b = 1.0 - a;
    aa += a * a;
    bb += b * b;
    ab += a * b;
    const float v[3] = {px[i].r, px[i].g, px[i].b};
    for (int c = 0; c < 3; ++c) ax[c] += a * v[c], bx[c] += b * v[c];
  }
  double det = aa * bb - ab * ab;
  if (std::fabs(det) < 1e-8) return false;
  double r0[3], r1[3];
  for (int c = 0; c < 3; ++c) {
    r0[c] = std::clamp((ax[c] * bb - bx[c] * ab) / det, 0.0, 255.0);
    r1[c] = std::clamp((bx[c] * aa - ax[c] * ab) / det, 0.0, 255.0);
  }
  e0 = {float(r0[0]), float(r0[1]), float(r0[2])};
  e1 = {float(r1[0]), float(r1[1]), float(r1[2])};
  return true;
}

// One DXT colour block from 16 RGBA8 texels. allowClear: DXT1 punch-through (alpha < 128 becomes transparent black).
void encodeColor(const uint8_t rgba[16][4], bool allowClear, uint8_t out[8]) {
  C3 px[16];
  bool use[16], clear[16];
  int n = 0;
  for (int i = 0; i < 16; ++i) {
    px[i] = {float(rgba[i][0]), float(rgba[i][1]), float(rgba[i][2])};
    clear[i] = allowClear && rgba[i][3] < 128;
    use[i] = !clear[i];
    n += use[i];
  }
  bool anyClear = n < 16;
  bool four = !anyClear;
  uint16_t c0 = 0, c1 = 0;
  uint8_t idx[16];
  if (n == 0) {
    std::memset(idx, 3, sizeof idx);  // c0 == c1: three-colour mode, all transparent
  } else {
    // principal axis of the colours (power iteration on the covariance)
    double mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i)
      if (use[i]) mean[0] += px[i].r, mean[1] += px[i].g, mean[2] += px[i].b;
    for (double& m : mean) m /= n;
    double cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
      if (!use[i]) continue;
      double r = px[i].r - mean[0], g = px[i].g - mean[1], b = px[i].b - mean[2];
      cov[0] += r * r, cov[1] += r * g, cov[2] += r * b, cov[3] += g * g, cov[4] += g * b, cov[5] += b * b;
    }
    double ax[3] = {1, 1, 1};
    for (int it = 0; it < 8; ++it) {
      double x = cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2];
      double y = cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2];
      double z = cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2];
      double m = std::max({std::fabs(x), std::fabs(y), std::fabs(z)});
      if (m < 1e-9) break;
      ax[0] = x / m, ax[1] = y / m, ax[2] = z / m;
    }
    double len = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    for (double& a : ax) a /= len;
    double tmin = 1e30, tmax = -1e30;
    for (int i = 0; i < 16; ++i) {
      if (!use[i]) continue;
      double t = (px[i].r - mean[0]) * ax[0] + (px[i].g - mean[1]) * ax[1] + (px[i].b - mean[2]) * ax[2];
      tmin = std::min(tmin, t), tmax = std::max(tmax, t);
    }
    double inset = (tmax - tmin) / 16.0;  // as stb_dxt: the extremes sit slightly inside the line's ends
    tmin += inset, tmax -= inset;
    C3 e0{float(mean[0] + ax[0] * tmax), float(mean[1] + ax[1] * tmax), float(mean[2] + ax[2] * tmax)};
    C3 e1{float(mean[0] + ax[0] * tmin), float(mean[1] + ax[1] * tmin), float(mean[2] + ax[2] * tmin)};
    auto clamp3 = [](C3 c) { return C3{std::clamp(c.r, 0.f, 255.f), std::clamp(c.g, 0.f, 255.f), std::clamp(c.b, 0.f, 255.f)}; };
    c0 = to565(clamp3(e0));
    c1 = to565(clamp3(e1));
    float best = assign(px, use, clear, c0, c1, four, idx);
    // near-flat blocks: both ends round to the same 565 colour and the block takes its quantisation error (a hue cast
    // on dark panels); the two 565 colours around the mean let the indices dither between them instead
    {
      auto q = [](double v, int maxv, bool up) {
        double f = v * maxv / 255.0;
        return std::clamp(int(up ? std::ceil(f) : std::floor(f)), 0, maxv);
      };
      uint16_t hi = uint16_t(q(mean[0], 31, true) << 11 | q(mean[1], 63, true) << 5 | q(mean[2], 31, true));
      uint16_t lo = uint16_t(q(mean[0], 31, false) << 11 | q(mean[1], 63, false) << 5 | q(mean[2], 31, false));
      uint8_t ni[16];
      float e = assign(px, use, clear, hi, lo, four, ni);
      if (e < best) best = e, c0 = hi, c1 = lo, std::memcpy(idx, ni, sizeof idx);
    }
    for (int it = 0; it < 3; ++it) {
      C3 r0, r1;
      if (!refit(px, use, idx, four, r0, r1)) break;
      uint16_t n0 = to565(r0), n1 = to565(r1);
      if (n0 == c0 && n1 == c1) break;
      uint8_t ni[16];
      float e = assign(px, use, clear, n0, n1, four, ni);
      if (e >= best) break;
      best = e, c0 = n0, c1 = n1;
      std::memcpy(idx, ni, sizeof idx);
    }
    // the mode is chosen by the endpoint order: c0 > c1 four colours, c0 <= c1 three + transparent
    if (four) {
      if (c0 < c1) {
        std::swap(c0, c1);
        for (uint8_t& k : idx) k = uint8_t(k ^ 1);  // 0<->1, 2<->3
      } else if (c0 == c1) {
        std::memset(idx, 0, sizeof idx);
      }
    } else if (c0 > c1) {
      std::swap(c0, c1);
      for (uint8_t& k : idx)
        if (k < 2) k = uint8_t(k ^ 1);
    }
  }
  out[0] = uint8_t(c0), out[1] = uint8_t(c0 >> 8), out[2] = uint8_t(c1), out[3] = uint8_t(c1 >> 8);
  uint32_t bits = 0;
  for (int i = 0; i < 16; ++i) bits |= uint32_t(idx[i]) << (2 * i);
  std::memcpy(out + 4, &bits, 4);
}

void encodeAlpha5(const uint8_t rgba[16][4], uint8_t out[8]) {
  int lo = 255, hi = 0;
  for (int i = 0; i < 16; ++i) lo = std::min(lo, int(rgba[i][3])), hi = std::max(hi, int(rgba[i][3]));
  out[0] = uint8_t(hi);
  out[1] = uint8_t(lo);
  uint64_t bits = 0;
  if (hi > lo) {
    int pal[8] = {hi, lo};
    for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * hi + k * lo) / 7;
    for (int i = 0; i < 16; ++i) {
      int best = 0, bd = 1 << 30;
      for (int k = 0; k < 8; ++k) {
        int d = std::abs(pal[k] - int(rgba[i][3]));
        if (d < bd) bd = d, best = k;
      }
      bits |= uint64_t(best) << (3 * i);
    }
  }
  for (int k = 0; k < 6; ++k) out[2 + k] = uint8_t(bits >> (8 * k));
}

void encodeAlpha3(const uint8_t rgba[16][4], uint8_t out[8]) {
  std::memset(out, 0, 8);
  for (int i = 0; i < 16; ++i) out[i / 2] |= uint8_t(std::min(15, (rgba[i][3] + 8) / 17) << (4 * (i & 1)));
}

std::vector<uint8_t> compress(const std::vector<uint8_t>& rgba, int w, int h, Image::Format f) {
  int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
  size_t bb = f == Image::Format::DXT1 ? 8 : 16;
  std::vector<uint8_t> out(size_t(bw) * size_t(bh) * bb);
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      uint8_t blk[16][4];
      for (int i = 0; i < 16; ++i) {
        int x = std::min(w - 1, bx * 4 + (i & 3)), y = std::min(h - 1, by * 4 + (i >> 2));
        std::memcpy(blk[i], rgba.data() + (size_t(y) * size_t(w) + size_t(x)) * 4, 4);
      }
      uint8_t* o = out.data() + (size_t(by) * size_t(bw) + size_t(bx)) * bb;
      if (f == Image::Format::DXT1) {
        encodeColor(blk, true, o);
      } else {
        if (f == Image::Format::DXT3) encodeAlpha3(blk, o);
        else encodeAlpha5(blk, o);
        encodeColor(blk, false, o + 8);
      }
    }
  return out;
}

Pix toPix(const std::vector<uint8_t>& rgba, int w, int h) {
  Pix p;
  p.w = w, p.h = h;
  p.d.resize(rgba.size());
  for (size_t i = 0; i < rgba.size(); ++i) p.d[i] = rgba[i] / 255.0f;
  return p;
}

}  // namespace

// ---- classification ------------------------------------------------------------------------------------------

Roles scanRoles(const Vfs& vfs, const std::function<bool(const std::string&, std::vector<uint8_t>&)>& read,
                const std::atomic<bool>* stop) {
  Roles roles;
  auto set = [&](const std::string& path, Kind k) {
    if (path.empty()) return;
    auto [it, fresh] = roles.emplace(path, k);
    if (fresh || it->second == k) return;
    Kind o = it->second;
    if ((o == Kind::Color && k == Kind::AlphaTest) || (o == Kind::AlphaTest && k == Kind::Color)) it->second = Kind::AlphaTest;
    else it->second = Kind::Mask;  // used two ways (e.g. normal and colour): scaled without sharpening or renormalising
  };
  auto add = [&](const render::MaterialDef& m) {
    size_t k = 0;
    auto next = [&]() { return k < m.textures.size() ? m.textures[k++] : std::string(); };
    if (m.flags & render::kMatDiffuseMap) set(next(), (m.flags & render::kMatAlphaTest) ? Kind::AlphaTest : Kind::Color);
    if (m.flags & render::kMatSpecularMap) set(next(), Kind::Mask);
    if (m.flags & render::kMatNormalMap) set(next(), Kind::Normal);
    if (m.flags & render::kMatIlluminationMap) set(next(), Kind::Mask);
    if (m.flags & render::kMatAmbientOcclusionMap) set(next(), Kind::Mask);
  };
  for (const auto& [path, rec] : vfs.records()) {
    if (stop && *stop) break;
    std::string l = lower(path);
    if (endsWith(l, ".material")) {
      render::MaterialDef m;
      if (render::loadMaterialFile(path, read, m)) add(m);
    } else if (endsWith(l, ".object")) {
      render::ObjectData obj;
      if (render::loadObject(path, read, obj, nullptr))
        for (const auto& m : obj.materials) add(m);
    }
  }
  return roles;
}

Kind classify(const std::string& path, const Roles& roles, const Image& img, bool includeUi) {
  if (img.cube || img.levels.empty()) return Kind::Skip;
  int lo = std::min(img.width, img.height), hi = std::max(img.width, img.height);
  std::string l = lower(path);
  // tiny ones are gradients, ramps and lookups; 2048 and up are already sharp and would cost 4× their VRAM again,
  // except the full-screen art of the loading screens and the main menu (2048 × 1024 stretched to the screen height
  // × 2: 2.1× at 4K, one screen at a time)
  bool screenArt = l.find("location-data/illustrations/") != std::string::npos || l.find("/ui/loading/") != std::string::npos ||
                   l.find("/ui/menus/animation/") != std::string::npos;
  if (lo < 16 || hi > (screenArt ? 2048 : 1024)) return Kind::Skip;
  static const char* skipPrefix[] = {"effects/", "textures/", "fonts/", "metro/fonts/", "metro/maps/", "userdata/", "regs/", "memory/", "regs-cim/"};
  for (const char* p : skipPrefix)
    if (l.compare(0, std::strlen(p), p) == 0) return Kind::Skip;
  if (l.find("/fonts/") != std::string::npos || l.find("/sky/") != std::string::npos ||
      l.find("/particles/") != std::string::npos || endsWith(l, "/selection.dds"))
    return Kind::Skip;
  if (auto it = roles.find(path); it != roles.end()) return it->second;
  size_t slash = l.find_last_of('/');
  std::string name = l.substr(slash == std::string::npos ? 0 : slash + 1);
  std::string stem = name.substr(0, name.size() > 4 ? name.size() - 4 : name.size());
  if (l.find("/ui/") != std::string::npos || l.find("portraits") != std::string::npos ||
      l.find("company-logos") != std::string::npos || l.find("location-data/") != std::string::npos ||
      name == "icon.dds" || name == "render.dds" || endsWith(stem, "-render") || endsWith(stem, "_render") ||
      endsWith(stem, "-icon") || endsWith(stem, "_icon"))
    return includeUi ? Kind::Ui : Kind::Skip;
  auto suffix = [&](std::initializer_list<const char*> list) {
    for (const char* s : list)
      if (endsWith(stem, s)) return true;
    return false;
  };
  if (suffix({"_n", "-n", "_n1", "_na", "_normal", "-normal", "_nrm"}) || stem.find("water-n") == 0) return Kind::Normal;
  if (suffix({"_s", "-s", "_spec", "-specular", "_specular", "_ao", "-ao", "_i", "_illum", "_illumination", "_mask"}) ||
      l.find("/illumination/") != std::string::npos)
    return Kind::Mask;
  if (l.find("/graphics/") != std::string::npos || l.find("/objects/") != std::string::npos || l.compare(0, 7, "addons/") == 0)
    return Kind::Color;
  return Kind::Skip;
}

// ---- upscaling -----------------------------------------------------------------------------------------------

bool upscaleLevel0(const Image& src, Kind kind, bool wrap, std::vector<uint8_t>& rgba2x, int& w2, int& h2) {
  if (kind == Kind::Skip) return false;
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  if (!render::decodeRgba(src, 0, rgba, w, h)) return false;
  Pix p = toPix(rgba, w, h);
  // DXT1 alpha is one bit: those textures get a crisp 1-bit edge again after scaling
  bool binaryAlpha = false, anyAlpha = false;
  for (size_t i = 3; i < rgba.size(); i += 4) {
    if (rgba[i] != 255) anyAlpha = true;
    if (rgba[i] == 0) binaryAlpha = true;
  }
  binaryAlpha = binaryAlpha && src.format == Image::Format::DXT1;
  // only where alpha is transparency: the alpha of plain diffuse maps is often a colourise mask (liveries, roofs) whose
  // texels keep their real colour under alpha 0
  if (anyAlpha && (kind == Kind::AlphaTest || kind == Kind::Ui)) bleed(p, binaryAlpha ? 0.5f : 1.0f / 255.0f, wrap);
  Pix o = lanczos2x(p, wrap);
  switch (kind) {
    case Kind::Color:
    case Kind::AlphaTest: cas(o, 0.6f, wrap); break;
    case Kind::Ui: cas(o, 0.35f, false); break;
    case Kind::Normal: {
      // renormalised only when the source's texels are unit vectors to begin with (a few "_n" maps hold something
      // else, e.g. harbourprops_n.dds; those are scaled like masks)
      double dev = 0;
      for (size_t i = 0; i < p.d.size(); i += 4) {
        float x = p.d[i] * 2 - 1, y = p.d[i + 1] * 2 - 1, z = p.d[i + 2] * 2 - 1;
        dev += std::fabs(std::sqrt(x * x + y * y + z * z) - 1.0f);
      }
      if (dev / double(p.d.size() / 4) > 0.1) break;
      for (size_t i = 0; i < o.d.size(); i += 4) {
        float x = o.d[i] * 2 - 1, y = o.d[i + 1] * 2 - 1, z = std::max(0.0f, o.d[i + 2] * 2 - 1);
        float len = std::sqrt(x * x + y * y + z * z);
        if (len < 1e-4f) continue;
        o.d[i] = (x / len) * 0.5f + 0.5f, o.d[i + 1] = (y / len) * 0.5f + 0.5f, o.d[i + 2] = (z / len) * 0.5f + 0.5f;
      }
      break;
    }
    default: break;
  }
  w2 = o.w, h2 = o.h;
  rgba2x.resize(o.d.size());
  for (size_t i = 0; i < o.d.size(); ++i) {
    float v = o.d[i];
    if ((i & 3) == 3 && binaryAlpha) v = v >= 0.5f ? 1.0f : 0.0f;
    rgba2x[i] = uint8_t(std::clamp(int(std::lround(v * 255.0f)), 0, 255));
  }
  return true;
}

bool buildUpscaled(const Image& src, const std::vector<uint8_t>& rgba2x, Image& out) {
  if (src.cube || src.levels.empty()) return false;
  int w2 = src.width * 2, h2 = src.height * 2;
  if (rgba2x.size() != size_t(w2) * size_t(h2) * 4) return false;
  Image img;
  img.format = src.format;
  img.width = w2;
  img.height = h2;
  std::vector<uint8_t> top;
  switch (src.format) {
    case Image::Format::DXT1:
    case Image::Format::DXT3:
    case Image::Format::DXT5: top = compress(rgba2x, w2, h2, src.format); break;
    case Image::Format::BGR8:
      top.resize(size_t(w2) * size_t(h2) * 3);
      for (size_t i = 0, n = size_t(w2) * size_t(h2); i < n; ++i)
        top[i * 3] = rgba2x[i * 4 + 2], top[i * 3 + 1] = rgba2x[i * 4 + 1], top[i * 3 + 2] = rgba2x[i * 4];
      break;
    default:
      top.resize(rgba2x.size());
      for (size_t i = 0; i < rgba2x.size(); i += 4)
        top[i] = rgba2x[i + 2], top[i + 1] = rgba2x[i + 1], top[i + 2] = rgba2x[i], top[i + 3] = rgba2x[i + 3];
      break;
  }
  img.levels.push_back(std::move(top));
  int keep = src.mipCount > 1 ? src.mipCount : 1;
  for (int m = 0; m < keep && size_t(m) < src.levels.size(); ++m) img.levels.push_back(src.levels[size_t(m)]);
  img.mipCount = int(img.levels.size());
  out = std::move(img);
  return true;
}

// ---- cache files ---------------------------------------------------------------------------------------------

namespace {
constexpr uint32_t kMagic = 0x55534752;  // "RGSU" in the DDS header's reserved words
void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); }
uint32_t get32(const std::vector<uint8_t>& d, size_t at) {
  uint32_t v;
  std::memcpy(&v, d.data() + at, 4);
  return v;
}
}  // namespace

std::vector<uint8_t> writeDds(const Image& img, uint64_t stamp, Kind kind, bool external) {
  std::vector<uint8_t> d(128, 0);
  std::memcpy(d.data(), "DDS ", 4);
  put32(d, 4, 124);
  put32(d, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (img.compressed() ? 0x80000 : 0x8));
  put32(d, 12, uint32_t(img.height));
  put32(d, 16, uint32_t(img.width));
  put32(d, 20, uint32_t(img.compressed() ? render::levelSize(img.format, img.width, img.height)
                                         : size_t(img.width) * (img.format == Image::Format::BGR8 ? 3 : 4)));
  put32(d, 28, uint32_t(img.mipCount));
  put32(d, 32, kMagic);
  put32(d, 36, kCacheVersion);
  put32(d, 40, uint32_t(stamp));
  put32(d, 44, uint32_t(stamp >> 32));
  put32(d, 48, uint32_t(kind) | (external ? 0x100u : 0u));
  put32(d, 76, 32);
  switch (img.format) {
    case Image::Format::DXT1: put32(d, 80, 0x4), put32(d, 84, 0x31545844); break;
    case Image::Format::DXT3: put32(d, 80, 0x4), put32(d, 84, 0x33545844); break;
    case Image::Format::DXT5: put32(d, 80, 0x4), put32(d, 84, 0x35545844); break;
    case Image::Format::BGR8:
      put32(d, 80, 0x40), put32(d, 88, 24), put32(d, 92, 0xFF0000), put32(d, 96, 0xFF00), put32(d, 100, 0xFF);
      break;
    default:
      put32(d, 80, 0x41), put32(d, 88, 32), put32(d, 92, 0xFF0000), put32(d, 96, 0xFF00), put32(d, 100, 0xFF);
      put32(d, 104, 0xFF000000u);
      break;
  }
  put32(d, 108, 0x1000 | (img.mipCount > 1 ? 0x400008 : 0));
  for (const auto& lv : img.levels) d.insert(d.end(), lv.begin(), lv.end());
  return d;
}

bool readStamp(const std::vector<uint8_t>& d, uint64_t& stamp, bool* external) {
  if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0 || get32(d, 32) != kMagic || get32(d, 36) != kCacheVersion)
    return false;
  stamp = uint64_t(get32(d, 40)) | uint64_t(get32(d, 44)) << 32;
  if (external) *external = (get32(d, 48) & 0x100) != 0;
  return true;
}

fs::path cacheRoot(const fs::path& userFolder) { return userFolder / L"texture-cache" / L"x2"; }

fs::path cacheFile(const fs::path& root, const std::string& vfsPath) { return root / widen(vfsPath); }

namespace {
bool readFile(const fs::path& p, std::vector<uint8_t>& out, size_t limit = 0) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  size_t n = size_t(f.tellg());
  if (limit) n = std::min(n, limit);
  f.seekg(0);
  out.resize(n);
  f.read(reinterpret_cast<char*>(out.data()), std::streamsize(n));
  return bool(f) || f.gcount() == std::streamsize(n);
}
bool writeFile(const fs::path& p, const std::vector<uint8_t>& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!f) return false;
  }
  fs::rename(tmp, p, ec);
  return !ec;
}
}  // namespace

bool loadCached(const fs::path& root, const std::string& vfsPath, const std::vector<uint8_t>& source, Image& out) {
  std::vector<uint8_t> d;
  uint64_t stamp = 0;
  if (!readFile(cacheFile(root, vfsPath), d) || !readStamp(d, stamp) || stamp != sourceStamp(source)) return false;
  return render::parseDds(d, out, nullptr);
}

std::string installSignature(const Vfs& vfs) {
  uint64_t h = (0xcbf29ce484222325ull ^ kCacheVersion) + kSelectionRules;
  auto mix = [&](const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001b3ull;
  };
  for (const auto& a : vfs.archives()) {
    std::string n = a.file.filename().string();
    mix(n.data(), n.size());
    std::error_code ec;
    uint64_t sz = fs::file_size(a.file, ec);
    auto t = fs::last_write_time(a.file, ec).time_since_epoch().count();
    mix(&sz, sizeof sz);
    mix(&t, sizeof t);
  }
  char buf[40];
  std::snprintf(buf, sizeof buf, "v%u-r%u-%016llx", kCacheVersion, kSelectionRules, (unsigned long long)h);
  return buf;
}

bool cacheComplete(const fs::path& root, const std::string& signature) {
  std::vector<uint8_t> d;
  if (!readFile(root / L"complete.txt", d)) return false;
  std::string s(d.begin(), d.end());
  return s.compare(0, signature.size(), signature) == 0;
}

// ---- generator -----------------------------------------------------------------------------------------------

namespace {

// resample an external upscaler's result to exactly w × h (box when it is an integer multiple, else bilinear)
std::vector<uint8_t> fitTo(const std::vector<uint8_t>& rgba, int sw, int sh, int w, int h) {
  if (sw == w && sh == h) return rgba;
  std::vector<uint8_t> out(size_t(w) * size_t(h) * 4);
  if (sw % w == 0 && sh % h == 0) {
    int fx = sw / w, fy = sh / h;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        for (int c = 0; c < 4; ++c) {
          int s = 0;
          for (int j = 0; j < fy; ++j)
            for (int i = 0; i < fx; ++i) s += rgba[(size_t(y * fy + j) * size_t(sw) + size_t(x * fx + i)) * 4 + size_t(c)];
          out[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)] = uint8_t((s + fx * fy / 2) / (fx * fy));
        }
    return out;
  }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float sx = (x + 0.5f) * sw / w - 0.5f, sy = (y + 0.5f) * sh / h - 0.5f;
      int x0 = std::clamp(int(std::floor(sx)), 0, sw - 1), y0 = std::clamp(int(std::floor(sy)), 0, sh - 1);
      int x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
      float fx = std::clamp(sx - x0, 0.f, 1.f), fy = std::clamp(sy - y0, 0.f, 1.f);
      for (int c = 0; c < 4; ++c) {
        auto px = [&](int xx, int yy) { return float(rgba[(size_t(yy) * size_t(sw) + size_t(xx)) * 4 + size_t(c)]); };
        float v = (px(x0, y0) * (1 - fx) + px(x1, y0) * fx) * (1 - fy) + (px(x0, y1) * (1 - fx) + px(x1, y1) * fx) * fy;
        out[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)] = uint8_t(std::clamp(int(v + 0.5f), 0, 255));
      }
    }
  return out;
}

size_t gpuBytes(const Image& img) {
  size_t n = 0;
  for (const auto& lv : img.levels) n += lv.size();
  return n;
}

}  // namespace

bool generate(const Vfs& vfs, const fs::path& root, const GenerateOptions& opt, Progress& p,
              const std::function<void(const std::string&)>& log) {
  std::mutex readMu, logMu;
  auto read = [&](const std::string& path, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(readMu);
    return vfs.read(path, out);
  };
  auto say = [&](const std::string& s) {
    std::lock_guard<std::mutex> lock(logMu);
    if (log) log(s);
  };
  std::string signature = installSignature(vfs);
  std::vector<std::string> paths;
  std::set<std::string> seen;
  for (const std::string& f : opt.first)
    if (vfs.exists(f) && seen.insert(f).second && f.compare(0, opt.only.size(), opt.only) == 0) paths.push_back(f);
  for (const auto& [path, rec] : vfs.records())
    if (endsWith(lower(path), ".dds") && path.compare(0, opt.only.size(), opt.only) == 0 && !seen.count(path)) paths.push_back(path);
  p.total = int(paths.size());
  say("texture upscale: " + std::to_string(paths.size()) + " textures, scanning materials");
  auto t0 = std::chrono::steady_clock::now();
  Roles roles = scanRoles(vfs, read, &p.stop);
  say("texture upscale: " + std::to_string(roles.size()) + " material textures (" +
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()) + " ms)");

  struct ExtJob {
    std::string path;
    uint64_t stamp;
    Kind kind;
  };
  std::vector<ExtJob> external;
  std::mutex extMu;
  std::atomic<size_t> next{0};
  auto work = [&]() {
    std::vector<uint8_t> data, head;
    while (!p.stop) {
      size_t i = next++;
      if (i >= paths.size()) break;
      const std::string& path = paths[i];
      Image src;
      if (!read(path, data) || !render::parseDds(data, src, nullptr)) {
        ++p.skipped, ++p.done;
        continue;
      }
      Kind kind = classify(path, roles, src, opt.ui);
      if (kind == Kind::Skip) {
        ++p.skipped, ++p.done;
        continue;
      }
      uint64_t stamp = sourceStamp(data);
      bool useExt = !opt.external.empty() &&
                    (kind == Kind::Ui || (!opt.externalUiOnly && (kind == Kind::Color || kind == Kind::AlphaTest)));
      fs::path out = cacheFile(root, path);
      uint64_t have = 0;
      bool haveExt = false;
      if (!opt.force && readFile(out, head, 128) && readStamp(head, have, &haveExt) && have == stamp && (haveExt || !useExt)) {
        ++p.upToDate, ++p.done;
        continue;
      }
      if (useExt) {
        std::lock_guard<std::mutex> lock(extMu);
        external.push_back({path, stamp, kind});
        continue;
      }
      std::vector<uint8_t> up;
      int w2 = 0, h2 = 0;
      Image res;
      if (!upscaleLevel0(src, kind, kind != Kind::Ui, up, w2, h2) || !buildUpscaled(src, up, res) ||
          !writeFile(out, writeDds(res, stamp, kind, false))) {
        ++p.failed, ++p.done;
        say("texture upscale: failed " + path);
        continue;
      }
      p.bytesSource += gpuBytes(src);
      p.bytesCache += gpuBytes(res);
      ++p.written, ++p.done;
      {
        std::lock_guard<std::mutex> lock(p.madeMutex);
        p.made.push_back(path);
      }
    }
  };
  int threads = opt.threads > 0 ? opt.threads : std::max(1, int(std::thread::hardware_concurrency()) - 1);
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) pool.emplace_back(work);
  for (auto& t : pool) t.join();

  // the external upscaler: colour and UI textures as PNG into one folder, one run per batch, results back from the
  // other folder; their alpha (and every normal map and mask) comes from the built-in filter
  if (!external.empty() && !p.stop) {
    fs::path tmp = root / L"_external";
    // one run of the upscaler on these jobs (fresh folders)
    auto runExternal = [&](const std::vector<size_t>& jobs) {
      std::error_code ec;
      fs::remove_all(tmp, ec);
      fs::create_directories(tmp / L"in", ec);
      fs::create_directories(tmp / L"out", ec);
      for (size_t i : jobs) {
        std::vector<uint8_t> data, rgba;
        Image src;
        int w = 0, h = 0;
        if (!read(external[i].path, data) || !render::parseDds(data, src, nullptr) || !render::decodeRgba(src, 0, rgba, w, h)) continue;
        Pix px = toPix(rgba, w, h);
        if (external[i].kind != Kind::Color) bleed(px, src.format == Image::Format::DXT1 ? 0.5f : 1.0f / 255.0f, external[i].kind != Kind::Ui);
        for (size_t k = 0; k < rgba.size(); ++k) rgba[k] = (k & 3) == 3 ? 255 : uint8_t(std::lround(px.d[k] * 255.0f));
        writeFile(tmp / L"in" / (std::to_wstring(i) + L".png"), render::encodePng(rgba.data(), w, h));
      }
      std::string cmd = opt.external;
      auto replace = [&](const std::string& key, const fs::path& v) {
        for (size_t at; (at = cmd.find(key)) != std::string::npos;) cmd.replace(at, key.size(), "\"" + v.string() + "\"");
      };
      replace("{in}", tmp / L"in");
      replace("{out}", tmp / L"out");
      say("texture upscale: external upscaler on " + std::to_string(jobs.size()) + " textures");
      int rc = std::system(("\"" + cmd + "\"").c_str());
      if (rc != 0) say("texture upscale: external upscaler returned " + std::to_string(rc));
    };
    // takes a job's result into the cache; false (nothing written) when it is rejected and may be tried again
    auto finish = [&](size_t i, bool lastTry) {
      const ExtJob& j = external[i];
      std::vector<uint8_t> data, png, ext, up;
      Image src, res;
      int ew = 0, eh = 0, w2 = 0, h2 = 0;
      bool ok = read(j.path, data) && render::parseDds(data, src, nullptr) &&
                upscaleLevel0(src, j.kind, j.kind != Kind::Ui, up, w2, h2);
      bool usedExt = false;
      if (ok && readFile(tmp / L"out" / (std::to_wstring(i) + L".png"), png) && decodePng(png, ext, ew, eh) && ew >= w2 / 2) {
        ext = fitTo(ext, ew, eh, w2, h2);
        // a broken run (a lost GPU device writes black images) must not end up in the cache: the result has to
        // stay close to the built-in one on average (the upscalers add detail, they do not move colours)
        double diff = 0;
        size_t n = 0;
        for (size_t k = 0; k < up.size(); k += 4)
          if (up[k + 3] >= 128) {
            for (int c = 0; c < 3; ++c) diff += std::abs(int(up[k + size_t(c)]) - int(ext[k + size_t(c)]));
            n += 3;
          }
        if (n && diff / double(n) > 24.0) {
          if (!lastTry) return false;
          say("texture upscale: external result for " + j.path + " differs too much (" + std::to_string(int(diff / double(n))) +
              "/255 on average), built-in used");
        } else {
          for (size_t k = 0; k < up.size(); k += 4) std::memcpy(&up[k], &ext[k], 3);  // RGB from the upscaler, alpha ours
          usedExt = true;
        }
      } else if (ok && !lastTry) {
        return false;  // no result at all: the run may have died
      }
      if (!ok || !buildUpscaled(src, up, res) || !writeFile(cacheFile(root, j.path), writeDds(res, j.stamp, j.kind, usedExt))) {
        ++p.failed, ++p.done;
        return true;
      }
      p.bytesSource += gpuBytes(src);
      p.bytesCache += gpuBytes(res);
      ++p.written, ++p.done;
      std::lock_guard<std::mutex> lock(p.madeMutex);
      p.made.push_back(j.path);
      return true;
    };
    for (size_t b = 0; b < external.size() && !p.stop; b += 256) {
      std::vector<size_t> jobs, again;
      for (size_t i = b; i < std::min(external.size(), b + 256); ++i) jobs.push_back(i);
      runExternal(jobs);
      for (size_t i : jobs)
        if (!finish(i, false)) again.push_back(i);
      // GPU upscalers fail now and then for part of a run (black or shifted images); a second run nearly always
      // gets those right
      if (!again.empty() && !p.stop) {
        say("texture upscale: " + std::to_string(again.size()) + " external results rejected, trying them once more");
        runExternal(again);
        for (size_t i : again) finish(i, true);
      }
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
  }
  bool complete = !p.stop && opt.only.empty() && opt.markComplete;
  if (complete) {
    std::string s = signature + "\n" + std::to_string(p.written + p.upToDate) + " textures\n";
    writeFile(root / L"complete.txt", std::vector<uint8_t>(s.begin(), s.end()));
  }
  p.finished = true;
  return p.failed == 0;
}

// ---- PNG reader ------------------------------------------------------------------------------------------------

namespace {

// inflate (RFC 1951), after zlib's puff.c
struct Inflate {
  const uint8_t* in;
  size_t n, pos = 0;
  uint32_t buf = 0;
  int cnt = 0;
  std::vector<uint8_t>& out;
  bool bad = false;
  Inflate(const uint8_t* d, size_t sz, std::vector<uint8_t>& o) : in(d), n(sz), out(o) {}
  int bits(int need) {
    uint32_t v = buf;
    while (cnt < need) {
      if (pos >= n) {
        bad = true;
        return 0;
      }
      v |= uint32_t(in[pos++]) << cnt;
      cnt += 8;
    }
    buf = v >> need;
    cnt -= need;
    return int(v & ((1u << need) - 1));
  }
  struct Huff {
    short count[16];
    short symbol[320];
  };
  static int build(Huff& h, const short* len, int num) {
    std::memset(h.count, 0, sizeof h.count);
    for (int s = 0; s < num; ++s) h.count[len[s]]++;
    if (h.count[0] == num) return 0;
    int left = 1;
    for (int l = 1; l < 16; ++l) {
      left <<= 1;
      left -= h.count[l];
      if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = short(offs[l] + h.count[l]);
    for (int s = 0; s < num; ++s)
      if (len[s]) h.symbol[offs[len[s]]++] = short(s);
    return left;
  }
  int decode(const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; ++l) {
      code |= bits(1);
      int count = h.count[l];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
      if (bad) return -1;
    }
    bad = true;
    return -1;
  }
  bool codes(const Huff& lc, const Huff& dc) {
    static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int sym = decode(lc);
      if (bad || sym < 0) return false;
      if (sym < 256) {
        out.push_back(uint8_t(sym));
      } else if (sym == 256) {
        return true;
      } else {
        sym -= 257;
        if (sym >= 29) return false;
        int len = lbase[sym] + bits(lext[sym]);
        int ds = decode(dc);
        if (bad || ds < 0 || ds >= 30) return false;
        size_t dist = size_t(dbase[ds] + bits(dext[ds]));
        if (dist > out.size()) return false;
        for (int k = 0; k < len; ++k) out.push_back(out[out.size() - dist]);
      }
    }
  }
  bool run() {
    int last;
    do {
      last = bits(1);
      int type = bits(2);
      if (bad) return false;
      if (type == 0) {
        buf = 0, cnt = 0;
        if (pos + 4 > n) return false;
        size_t len = size_t(in[pos] | in[pos + 1] << 8);
        pos += 4;
        if (pos + len > n) return false;
        out.insert(out.end(), in + pos, in + pos + len);
        pos += len;
      } else if (type == 1) {
        struct Fixed {
          Huff lc, dc;
          Fixed() {
            short l[288];
            for (int s = 0; s < 288; ++s) l[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
            build(lc, l, 288);
            for (int s = 0; s < 30; ++s) l[s] = 5;
            build(dc, l, 30);
          }
        };
        static const Fixed fixed;
        if (!codes(fixed.lc, fixed.dc)) return false;
      } else if (type == 2) {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (bad || nlen > 286 || ndist > 30) return false;
        short len[320] = {};
        for (int k = 0; k < ncode; ++k) len[order[k]] = short(bits(3));
        Huff lc, dc;
        if (build(lc, len, 19) != 0) return false;
        int k = 0;
        while (k < nlen + ndist) {
          int sym = decode(lc);
          if (bad || sym < 0) return false;
          if (sym < 16) {
            len[k++] = short(sym);
          } else {
            short v = 0;
            int rep;
            if (sym == 16) {
              if (k == 0) return false;
              v = len[k - 1];
              rep = 3 + bits(2);
            } else if (sym == 17) {
              rep = 3 + bits(3);
            } else {
              rep = 11 + bits(7);
            }
            if (k + rep > nlen + ndist) return false;
            while (rep--) len[k++] = v;
          }
        }
        int el = build(lc, len, nlen);
        if (el < 0 || (el > 0 && nlen - lc.count[0] != 1)) return false;
        int ed = build(dc, len + nlen, ndist);
        if (ed < 0 || (ed > 0 && ndist - dc.count[0] != 1)) return false;
        if (!codes(lc, dc)) return false;
      } else {
        return false;
      }
    } while (!last);
    return true;
  }
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

}  // namespace

bool decodePng(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgba, int& w, int& h) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (png.size() < 33 || std::memcmp(png.data(), sig, 8) != 0) return false;
  int depth = 0, type = 0, interlace = 0;
  std::vector<uint8_t> idat, plte, trns;
  for (size_t at = 8; at + 12 <= png.size();) {
    uint32_t len = be32(&png[at]);
    if (at + 12 + len > png.size()) return false;
    const uint8_t* body = &png[at + 8];
    std::string t(reinterpret_cast<const char*>(&png[at + 4]), 4);
    if (t == "IHDR") {
      w = int(be32(body)), h = int(be32(body + 4)), depth = body[8], type = body[9], interlace = body[12];
    } else if (t == "PLTE") {
      plte.assign(body, body + len);
    } else if (t == "tRNS") {
      trns.assign(body, body + len);
    } else if (t == "IDAT") {
      idat.insert(idat.end(), body, body + len);
    } else if (t == "IEND") {
      break;
    }
    at += 12 + len;
  }
  if (w <= 0 || h <= 0 || interlace || (depth != 8 && depth != 16) || idat.size() < 2) return false;
  int ch = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
  if (!ch || (type == 3 && depth != 8)) return false;
  std::vector<uint8_t> raw;
  Inflate inf(idat.data() + 2, idat.size() - 2, raw);
  if (!inf.run()) return false;
  size_t bpp = size_t(ch) * size_t(depth / 8), stride = bpp * size_t(w);
  if (raw.size() < (stride + 1) * size_t(h)) return false;
  std::vector<uint8_t> cur(stride), prev(stride, 0);
  rgba.assign(size_t(w) * size_t(h) * 4, 255);
  for (int y = 0; y < h; ++y) {
    const uint8_t* line = &raw[size_t(y) * (stride + 1)];
    uint8_t f = line[0];
    for (size_t i = 0; i < stride; ++i) {
      int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = line[1 + i];
      switch (f) {
        case 1: x += a; break;
        case 2: x += b; break;
        case 3: x += (a + b) / 2; break;
        case 4: {
          int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
          x += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
          break;
        }
        default: break;
      }
      cur[i] = uint8_t(x);
    }
    for (int x = 0; x < w; ++x) {
      uint8_t* o = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
      auto s = [&](int k) { return cur[size_t(x) * bpp + size_t(k) * size_t(depth / 8)]; };  // high byte of 16-bit
      switch (type) {
        case 0: o[0] = o[1] = o[2] = s(0); break;
        case 4: o[0] = o[1] = o[2] = s(0), o[3] = s(1); break;
        case 2: o[0] = s(0), o[1] = s(1), o[2] = s(2); break;
        case 6: o[0] = s(0), o[1] = s(1), o[2] = s(2), o[3] = s(3); break;
        case 3: {
          size_t k = s(0);
          if (k * 3 + 2 < plte.size()) o[0] = plte[k * 3], o[1] = plte[k * 3 + 1], o[2] = plte[k * 3 + 2];
          if (k < trns.size()) o[3] = trns[k];
          break;
        }
      }
    }
    std::swap(cur, prev);
  }
  return true;
}

}  // namespace regs::engine::texup
