// Night lights of the port's day/night cycle: lit windows and street lamps (engine/night_lights.h,
// docs/renderer/night-lights.md).
#include "engine/night_lights.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <tuple>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/grid_render.h"
#include "engine/scene3d.h"
#include "engine/sky.h"
#include "render/renderer2d.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

bool debugNight() {
  static const bool on = std::getenv("REGS_DEBUG_NIGHT") != nullptr;
  return on;
}

float smooth(float a, float b, float x) {
  float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
float hash01(uint32_t x) { return float(hash32(x) >> 8) / float(1u << 24); }

// the level of a texture no wider than maxSize, decoded to RGBA8
bool decodeAtMost(const Image& img, int maxSize, std::vector<uint8_t>& rgba, int& w, int& h) {
  int level = 0;
  while (level + 1 < img.mipCount && std::max(img.width >> level, img.height >> level) > maxSize) ++level;
  return decodeRgba(img, level, rgba, w, h);
}

// box-filtered mip chain of BGRA8 level 0
void buildMips(Image& img) {
  img.mipCount = 1;
  int w = img.width, h = img.height;
  while (w > 1 || h > 1) {
    int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    const std::vector<uint8_t>& src = img.levels.back();
    std::vector<uint8_t> dst(size_t(nw) * size_t(nh) * 4);
    for (int y = 0; y < nh; ++y)
      for (int x = 0; x < nw; ++x)
        for (int c = 0; c < 4; ++c) {
          int sum = 0, n = 0;
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
              int sx = std::min(w - 1, 2 * x + dx), sy = std::min(h - 1, 2 * y + dy);
              sum += src[(size_t(sy) * size_t(w) + size_t(sx)) * 4 + size_t(c)];
              ++n;
            }
          dst[(size_t(y) * size_t(nw) + size_t(x)) * 4 + size_t(c)] = uint8_t((sum + n / 2) / n);
        }
    img.levels.push_back(std::move(dst));
    ++img.mipCount;
    w = nw, h = nh;
  }
}

// ---- the hour and the light levels ----

// share of the window groups lit over the day (hours â†’ share), linear between the keys
float windowShare(float hours) {
  // people go to bed between 22:00 and 1:00: most windows go dark, a few stay lit through the small hours; from
  // 5:30 the early risers switch theirs on until daylight
  static const float k[][2] = {{0, 0.15f},   {1, 0.08f},  {2.5f, 0.05f}, {4.5f, 0.04f}, {5.5f, 0.12f},
                               {6.5f, 0.36f}, {8, 0.3f},  {12, 0.2f},    {16, 0.28f},  {17.5f, 0.45f},
                               {19, 0.56f},  {21, 0.58f}, {22, 0.48f},   {23, 0.3f},   {24, 0.15f}};
  const int n = int(sizeof k / sizeof k[0]);
  hours = std::fmod(std::fmod(hours, 24.0f) + 24.0f, 24.0f);
  for (int i = 0; i + 1 < n; ++i)
    if (hours <= k[i + 1][0]) {
      float t = (hours - k[i][0]) / (k[i + 1][0] - k[i][0]);
      return k[i][1] + (k[i + 1][1] - k[i][1]) * t;
    }
  return k[n - 1][1];
}

constexpr float kLampGain = 1.9f;     // lamp light at the pool centre, in units of the surface colour
constexpr float kLampTexel = 1.5f;    // metres per lamp-map texel (capped by kLampMapMax)
constexpr int kLampMapMax = 2048;

// ---- lamps ----

struct LampCache {
  uint64_t generation = ~0ull, itemsVersion = ~0ull;
  uint32_t texture = 0;
  float xf[4] = {0, 0, 0, 0};
  float y0 = 0, yScale = 1;
  uint64_t builtFor = ~0ull;  // the grid generation of the last full build
  int w = 0, h = 0;
  float texel[2] = {1, 1};
  std::vector<uint8_t> px;  // BGRA, kept for area updates
  struct Head {
    Vec3 pos;
    float onAt = 0.4f;  // night01 at which this lamp switches on (a small stagger over the dusk)
  };
  std::vector<Head> heads;
};

std::map<const GridObj*, LampCache>& lampCaches() {
  static std::map<const GridObj*, LampCache> m;
  return m;
}

std::unordered_map<const ObjectData*, std::vector<Vec3>>& headCache() {
  static std::unordered_map<const ObjectData*, std::vector<Vec3>> m;
  return m;
}

const std::vector<Vec3>& headsOf(const ObjectData& od, const std::string& path) {
  auto& c = headCache();
  auto it = c.find(&od);
  if (it != c.end()) return it->second;
  std::vector<Vec3> h = findLampHeads(od.mesh);
  if (debugNight()) {
    std::string s;
    for (const Vec3& p : h) s += " (" + std::to_string(p.x) + ", " + std::to_string(p.y) + ", " + std::to_string(p.z) + ")";
    LOG_INFO("night: lamp %s: %zu heads%s", path.c_str(), h.size(), s.c_str());
  }
  return c.emplace(&od, std::move(h)).first->second;
}

// lamp templates that bring their own light (the DLC street lights' additive "glow" pool, sky-and-night.md Â§4.2) keep it
bool ownGlow(const ObjectData& od) {
  for (const MaterialDef& m : od.materials) {
    std::string n = m.name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (n.find("glow") != std::string::npos) return true;
  }
  return false;
}

// the lamp heads of the map in world space, in item order
std::vector<LampCache::Head> collectHeads(GridObj* grid) {
  const GridData& g = *grid->data;
  const TerrainCache& tc = *grid->terrain;
  constexpr uint32_t kHidden = 0x400000;
  std::vector<uint8_t> isLamp(tc.templates.size(), 0);
  for (size_t t = 0; t < tc.templates.size(); ++t) {
    const GridTemplate& tp = tc.templates[t];
    if (tp.object && tp.object->data && isLampTemplate(tp.object->pendingPath) && !ownGlow(*tp.object->data) &&
        !headsOf(*tp.object->data, tp.object->pendingPath).empty())
      isLamp[t] = 1;
  }
  std::vector<LampCache::Head> heads;
  for (uint32_t i = 0; i < g.items.size(); ++i) {
    const GridData::Item& it = g.items[i];
    if (it.removed || it.type >= tc.typeTemplate.size()) continue;
    int ti = tc.typeTemplate[it.type];
    if (ti < 0 || !isLamp[size_t(ti)]) continue;
    const GridTemplate& tp = tc.templates[size_t(ti)];
    if ((it.flags | tp.flags) & kHidden) continue;
    int div = itemDivision(g, tc, i);
    if (div < 0) continue;
    Mat4 world = tc.divisions[size_t(div)].world * itemPlacement(g, tc, i, div);
    uint32_t k = 0;
    for (const Vec3& h : headsOf(*tp.object->data, tp.object->pendingPath)) {
      LampCache::Head head;
      head.pos = transformPoint(world, h);
      head.onAt = 0.36f + 0.14f * hash01(uint32_t(it.x) * 7919u ^ uint32_t(it.z) * 104729u ^ k++);
      heads.push_back(head);
    }
  }
  return heads;
}

constexpr float kPoolRadius = 14.0f;

// the pools of all heads over the texel rect [x0, x1] Ã— [z0, z1] into lc.px: warm light (1, 0.78, 0.5) with a bright
// core (â‰ˆ 4 m) and a long faint tail (â‰ˆ 8.5 m) that joins the pools along a street, faded to 0 at 14 m; alpha = the
// heads' height weighted by their light
void splatRect(LampCache& lc, int x0, int z0, int x1, int z1) {
  const int W = lc.w;
  const int rw = x1 - x0 + 1, rh = z1 - z0 + 1;
  std::vector<float> wsum(size_t(rw) * size_t(rh), 0.0f), ysum(size_t(rw) * size_t(rh), 0.0f);
  const float tx = lc.texel[0], tz = lc.texel[1], R = kPoolRadius;
  for (const auto& h : lc.heads) {
    float cx = (h.pos.x - lc.xf[0]) / tx - 0.5f, cz = (h.pos.z - lc.xf[1]) / tz - 0.5f;
    int ax = std::max(x0, int(std::floor(cx - R / tx))), bx = std::min(x1, int(std::ceil(cx + R / tx)));
    int az = std::max(z0, int(std::floor(cz - R / tz))), bz = std::min(z1, int(std::ceil(cz + R / tz)));
    for (int z = az; z <= bz; ++z)
      for (int x = ax; x <= bx; ++x) {
        float dx = (float(x) - cx) * tx, dz = (float(z) - cz) * tz, d2 = dx * dx + dz * dz;
        float q = 1.0f - d2 / (R * R);
        if (q <= 0) continue;
        float f = (0.7f * std::exp(-d2 / 16.0f) + 0.3f * std::exp(-d2 / 72.0f)) * q * q;
        size_t k = size_t(z - z0) * size_t(rw) + size_t(x - x0);
        wsum[k] += f;
        ysum[k] += f * h.pos.y;
      }
  }
  auto lim = [](float v) { return uint8_t(std::lround(255.0f * v / (1.0f + 0.35f * v))); };  // overlaps: soft limit
  for (int z = z0; z <= z1; ++z)
    for (int x = x0; x <= x1; ++x) {
      size_t k = size_t(z - z0) * size_t(rw) + size_t(x - x0);
      uint8_t* p = &lc.px[(size_t(z) * size_t(W) + size_t(x)) * 4];
      float f = wsum[k];
      p[0] = lim(f * 0.5f), p[1] = lim(f * 0.78f), p[2] = lim(f);
      p[3] = f > 0 ? uint8_t(std::lround(255.0f * std::clamp((ysum[k] / f - lc.y0) / lc.yScale, 0.0f, 1.0f))) : 0;
    }
}

// the lamp map of a grid: built in full on load, then only the area of lamps added or removed by live edits
void updateLampMap(Display& d, GridObj* grid, LampCache& lc) {
  auto t0 = std::chrono::steady_clock::now();
  const GridData& g = *grid->data;
  const TerrainCache& tc = *grid->terrain;
  std::vector<LampCache::Head> heads = collectHeads(grid);
  auto same = [](const LampCache::Head& a, const LampCache::Head& b) {
    return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z;
  };
  bool full = !lc.texture || lc.builtFor != grid->generation;
  float yMin = 1e30f, yMax = -1e30f;
  for (const auto& h : heads) yMin = std::min(yMin, h.pos.y), yMax = std::max(yMax, h.pos.y);
  if (!full) {
    if (heads.size() == lc.heads.size() && std::equal(heads.begin(), heads.end(), lc.heads.begin(), same)) return;
    if (!heads.empty() && (yMin < lc.y0 || yMax > lc.y0 + lc.yScale)) full = true;  // out of the height encoding
  }
  int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  if (full) {
    lc.builtFor = grid->generation;
    float cs = g.cellSize;
    // the world position of cell edge 0 (the division placement of terrain.cpp: (cÂ·n âˆ’ ((Wâˆ’n)>>1))Â·cs per division)
    float ox = -(float(tc.n / 2) + float((g.width - tc.n) >> 1)) * cs;
    float oz = -(float(tc.n / 2) + float((g.length - tc.n) >> 1)) * cs;
    float sizeX = float(g.width) * cs, sizeZ = float(g.length) * cs;
    int w = std::clamp(int(std::ceil(sizeX / kLampTexel)), 16, kLampMapMax);
    int h = std::clamp(int(std::ceil(sizeZ / kLampTexel)), 16, kLampMapMax);
    if (lc.texture && (w != lc.w || h != lc.h)) {
      d.r2d().destroyTexture(lc.texture);
      lc.texture = 0;
    }
    lc.w = w, lc.h = h;
    lc.xf[0] = ox, lc.xf[1] = oz, lc.xf[2] = 1.0f / sizeX, lc.xf[3] = 1.0f / sizeZ;
    lc.texel[0] = sizeX / float(lc.w), lc.texel[1] = sizeZ / float(lc.h);
    if (heads.empty()) yMin = 0, yMax = 0;
    lc.y0 = yMin - 30.0f;  // room for lamps added later on lower or higher ground
    lc.yScale = std::max(120.0f, yMax - yMin + 60.0f);
    lc.px.assign(size_t(lc.w) * size_t(lc.h) * 4, 0);
    x1 = lc.w - 1, z1 = lc.h - 1;
  } else {
    // the texel rect around the heads that differ (removed or added)
    auto key = [](const LampCache::Head& a, const LampCache::Head& b) {
      return std::tie(a.pos.x, a.pos.y, a.pos.z) < std::tie(b.pos.x, b.pos.y, b.pos.z);
    };
    std::vector<LampCache::Head> a = lc.heads, b = heads, diff;
    std::sort(a.begin(), a.end(), key);
    std::sort(b.begin(), b.end(), key);
    std::set_symmetric_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(diff), key);
    if (diff.empty()) {
      lc.heads = std::move(heads);  // the same heads in another order
      return;
    }
    float lx = 1e30f, lz = 1e30f, hx = -1e30f, hz = -1e30f;
    for (const auto& h : diff)
      lx = std::min(lx, h.pos.x), hx = std::max(hx, h.pos.x), lz = std::min(lz, h.pos.z), hz = std::max(hz, h.pos.z);
    x0 = std::max(0, int((lx - kPoolRadius - lc.xf[0]) / lc.texel[0]) - 1);
    x1 = std::min(lc.w - 1, int((hx + kPoolRadius - lc.xf[0]) / lc.texel[0]) + 1);
    z0 = std::max(0, int((lz - kPoolRadius - lc.xf[1]) / lc.texel[1]) - 1);
    z1 = std::min(lc.h - 1, int((hz + kPoolRadius - lc.xf[1]) / lc.texel[1]) + 1);
  }
  lc.heads = std::move(heads);
  splatRect(lc, x0, z0, x1, z1);
  // one level, linear: from the highest camera a screen pixel covers about one texel, so no mipmaps are needed
  if (!lc.texture) lc.texture = d.r2d().createTextureBGRA(lc.px.data(), lc.w, lc.h);
  else d.r2d().updateTextureBGRA(lc.texture, lc.px.data(), lc.w, lc.h);
  LOG_INFO("night lights: %zu lamp heads, lamp map %dx%d (%s %dx%d texels) in %lld ms", lc.heads.size(), lc.w, lc.h,
           full ? "all" : "area", x1 - x0 + 1, z1 - z0 + 1,
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

// the glow sprite texture (white, alpha a soft core and a wide faint halo) and its particle material
GpuMaterial* glowMaterial(Display& d) {
  static std::unique_ptr<GpuMaterial> m;
  if (m) return m.get();
  m = std::make_unique<GpuMaterial>();
  Image img;
  img.format = Image::Format::BGRA8;
  img.width = img.height = 64;
  std::vector<uint8_t> px(64 * 64 * 4);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      float dx = (float(x) + 0.5f) / 32.0f - 1.0f, dy = (float(y) + 0.5f) / 32.0f - 1.0f;
      float r2 = dx * dx + dy * dy;
      float a = r2 >= 1 ? 0.0f : (0.85f * std::exp(-r2 * 14.0f) + 0.22f * std::exp(-r2 * 3.5f)) * (1 - r2) * (1 - r2);
      uint8_t* p = &px[(size_t(y) * 64 + size_t(x)) * 4];
      p[0] = p[1] = p[2] = 255;
      p[3] = uint8_t(std::lround(255.0f * std::min(1.0f, a)));
    }
  img.levels.push_back(std::move(px));
  buildMips(img);
  m->flags = 0x4300u | kMatTwoSided;  // DIFFUSEMAP | POINT | ADDITIVE_A, as the particle batches (engine/particles.cpp)
  m->shader = "shaders/particles/particles.shader";
  for (int k = 0; k < 4; ++k) m->ambient[k] = m->diffuse[k] = 0, m->emission[k] = 1, m->specular[k] = 0;
  m->ambient[3] = m->diffuse[3] = m->specular[3] = 1;
  m->textures.assign(1, d.r2d().createTexture(img));
  m->cube.assign(1, 0);
  return m.get();
}

}  // namespace

// ---- windows ----

bool makeWindowMask(const Image& specular, const Image* illumination, Image& out, WindowMaskInfo* info) {
  WindowMaskInfo local;
  WindowMaskInfo& st = info ? *info : local;
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  if (!decodeAtMost(specular, 512, rgba, w, h)) {
    st.skipped = "specular map not decodable";
    return false;
  }
  const size_t n = size_t(w) * size_t(h);
  std::vector<float> s(n);
  std::vector<int> hist(256, 0);
  for (size_t i = 0; i < n; ++i) {
    uint8_t m = std::max({rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]});
    s[i] = m / 255.0f;
    ++hist[m];
  }
  // the glass level: half the brightest specular (its 99.5th percentile), between 0.2 and 0.4
  int top = 255;
  for (size_t acc = 0; top > 0 && (acc += size_t(hist[size_t(top)])) < n / 200;) --top;
  float peak = top / 255.0f;
  if (peak < 0.22f) {
    st.skipped = "no glass in the specular map";
    return false;
  }
  float T = std::clamp(0.5f * peak, 0.2f, 0.4f);
  st.threshold = T;
  // texels an illumination map lights already (signs): no windows there; a map lighting more than a little (the DLC's
  // lit windows) means the texture has its own night look
  std::vector<uint8_t> lit(n, 0);
  if (illumination) {
    std::vector<uint8_t> il;
    int iw = 0, ih = 0;
    if (decodeAtMost(*illumination, 512, il, iw, ih)) {
      size_t count = 0;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const uint8_t* p = &il[(size_t(y * ih / h) * size_t(iw) + size_t(x * iw / w)) * 4];
          if (std::max({p[0], p[1], p[2]}) > 40) lit[size_t(y) * size_t(w) + size_t(x)] = 1, ++count;
        }
      if (count > n / 60) {
        st.skipped = "the illumination map lights it";
        return false;
      }
    }
  }
  std::vector<uint8_t> glass(n, 0), grown(n, 0);
  for (size_t i = 0; i < n; ++i) glass[i] = s[i] > T && !lit[i];
  // panes of one window (split by thin frames) belong together: grow by ~1 % of the width before labelling
  const int r = std::max(1, w / 128);
  std::vector<uint8_t> tmp(n, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      bool any = false;
      for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r) && !any; ++k) any = glass[size_t(y) * size_t(w) + size_t(k)];
      tmp[size_t(y) * size_t(w) + size_t(x)] = any;
    }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      bool any = false;
      for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r) && !any; ++k) any = tmp[size_t(k) * size_t(w) + size_t(x)];
      grown[size_t(y) * size_t(w) + size_t(x)] = any;
    }
  std::vector<int> label(n, -1);
  std::vector<int> stack;
  struct Comp {
    size_t glass = 0;
  };
  std::vector<Comp> comps;
  for (size_t i = 0; i < n; ++i) {
    if (!grown[i] || label[i] >= 0) continue;
    int id = int(comps.size());
    comps.push_back({});
    label[i] = id;
    stack.assign(1, int(i));
    while (!stack.empty()) {
      int p = stack.back();
      stack.pop_back();
      if (glass[size_t(p)]) ++comps[size_t(id)].glass;
      int x = p % w, y = p / w;
      const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
      for (const auto& q : nb) {
        if (q[0] < 0 || q[1] < 0 || q[0] >= w || q[1] >= h) continue;
        size_t k = size_t(q[1]) * size_t(w) + size_t(q[0]);
        if (grown[k] && label[k] < 0) label[k] = id, stack.push_back(int(k));
      }
    }
  }
  // a glass area over 6 % of the texture is no window (glass roofs, sheet metal); groups at random per window
  std::vector<int> group(comps.size(), -1);
  std::vector<float> bright(comps.size(), 1.0f);  // per window: lamps, curtains, room depth
  size_t covered = 0;
  for (size_t c = 0; c < comps.size(); ++c) {
    if (comps[c].glass == 0) continue;
    if (comps[c].glass > n * 6 / 100) {
      ++st.rejected;
      continue;
    }
    uint32_t hc = hash32(hash32(uint32_t(c) * 0x9E3779B9u + 0x632BE5ABu) ^ uint32_t(w));
    group[c] = int(hc >> 20 & 3);
    bright[c] = 0.5f + 0.5f * float(hc & 0xFFFF) / 65535.0f;
    ++st.windows;
  }
  if (st.windows == 0) {
    st.skipped = "no window-sized glass";
    return false;
  }
  std::vector<uint8_t> px(n * 4, 0);
  for (size_t i = 0; i < n; ++i) {
    if (label[i] < 0 || group[size_t(label[i])] < 0 || lit[i]) continue;
    float cov = smooth(T - 0.07f, T + 0.07f, s[i]);
    if (cov <= 0) continue;
    px[i * 4 + size_t(group[size_t(label[i])])] = uint8_t(std::lround(255.0f * cov * bright[size_t(label[i])]));
    covered += cov > 0.5f;
  }
  st.coverage = float(covered) / float(n);
  out = Image();
  out.format = Image::Format::BGRA8;
  out.width = w, out.height = h;
  out.levels.push_back(std::move(px));
  buildMips(out);
  return true;
}

namespace {

// the specular and illumination map of a building material ("" when it gets no window mask)
bool maskSources(const MaterialDef& m, const std::string& objectPath, std::string& spec, std::string& illum) {
  if (objectPath.find("/buildings/") == std::string::npos || !(m.flags & kMatSpecularMap) || !(m.flags & kMatDiffuseMap))
    return false;
  // texture slots in unit order: diffuse, mix maps, specular, normal, illumination (maps.fragment)
  size_t at = 1 + ((m.flags & 0x400) ? 1 : 0) + ((m.flags & 0x800) ? 2 : 0);
  if (at >= m.textures.size()) return false;
  spec = m.textures[at];
  size_t il = at + 1 + ((m.flags & kMatNormalMap) ? 1 : 0);
  illum = (m.flags & kMatIlluminationMap) && il < m.textures.size() ? m.textures[il] : "";
  return true;
}

std::map<std::string, uint32_t>& maskCache() {
  static std::map<std::string, uint32_t> cache;
  return cache;
}

struct MaskJob {
  std::string key, spec, objectPath;
  std::vector<uint8_t> specData, illumData;
  Image mask;
  WindowMaskInfo info;
  bool ok = false;
};

void runMaskJob(MaskJob& j) {
  Image si, ii;
  bool haveIllum = !j.illumData.empty() && parseDds(j.illumData, ii, nullptr);
  j.ok = !j.specData.empty() && parseDds(j.specData, si, nullptr) && makeWindowMask(si, haveIllum ? &ii : nullptr, j.mask, &j.info);
}

void finishMaskJob(Display& d, MaskJob& j) {
  uint32_t tex = j.ok ? d.r2d().createTexture(j.mask) : 0;
  if (debugNight())
    LOG_INFO("night: windows %s (%s): %d groups, %d rejected, glass > %.2f, %.1f %% %s", j.spec.c_str(), j.objectPath.c_str(),
             j.info.windows, j.info.rejected, j.info.threshold, j.info.coverage * 100, j.info.skipped ? j.info.skipped : "");
  maskCache().emplace(j.key, tex);
}

bool newJob(VM& vm, const MaterialDef& m, const std::string& objectPath, MaskJob& j) {
  std::string illum;
  if (!maskSources(m, objectPath, j.spec, illum)) return false;
  j.key = j.spec + "|" + illum;
  if (maskCache().count(j.key)) return false;
  j.objectPath = objectPath;
  vm.vfs().read(j.spec, j.specData);
  if (!illum.empty()) vm.vfs().read(illum, j.illumData);
  return true;
}

}  // namespace

void prepareWindowMasks(Display& d, VM& vm, const std::vector<std::pair<const MaterialDef*, std::string>>& materials) {
  auto t0 = std::chrono::steady_clock::now();
  std::vector<MaskJob> jobs;
  std::set<std::string> queued;
  for (const auto& [m, path] : materials) {
    MaskJob j;
    if (newJob(vm, *m, path, j) && queued.insert(j.key).second) jobs.push_back(std::move(j));
  }
  if (jobs.empty()) return;
  // decoding and labelling run on worker threads; the uploads stay on this (the GL) thread
  std::atomic<size_t> next{0};
  unsigned n = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  std::vector<std::thread> workers;
  for (unsigned t = 0; t < n; ++t)
    workers.emplace_back([&] {
      for (size_t i; (i = next++) < jobs.size();) runMaskJob(jobs[i]);
    });
  for (auto& w : workers) w.join();
  for (MaskJob& j : jobs) finishMaskJob(d, j);
  LOG_INFO("night lights: %zu window masks in %lld ms", jobs.size(),
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

uint32_t buildingWindowMask(Display& d, VM& vm, const MaterialDef& m, const std::string& objectPath) {
  std::string spec, illum;
  if (!maskSources(m, objectPath, spec, illum)) return 0;
  if (auto it = maskCache().find(spec + "|" + illum); it != maskCache().end()) return it->second;
  MaskJob j;  // not prepared (live edits bring a new template): made here
  if (!newJob(vm, m, objectPath, j)) return 0;
  runMaskJob(j);
  finishMaskJob(d, j);
  return maskCache()[j.key];
}

uint8_t buildingSeed(int x, int y, int z) {
  return uint8_t(1 + hash32(uint32_t(x) * 73856093u ^ uint32_t(z) * 19349663u ^ uint32_t(y) * 83492791u) % 255);
}

// ---- lamps ----

bool isLampTemplate(const std::string& objectPath) {
  std::string p = objectPath;
  std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  std::string stem = p.substr(p.find_last_of('/') + 1);
  if (stem.find("traffic") != std::string::npos || stem.find("lighthouse") != std::string::npos) return false;
  return stem.find("lamp") != std::string::npos || stem.find("light") != std::string::npos ||
         stem.find("lantern") != std::string::npos;
}

std::vector<Vec3> findLampHeads(const MeshData& m) {
  const size_t nv = m.vertexCount();
  std::vector<Vec3> out;
  if (!nv || m.normals.size() < nv * 3) return out;
  float minY = 1e30f, maxY = -1e30f;
  for (size_t i = 0; i < nv; ++i) minY = std::min(minY, m.positions[i * 3 + 1]), maxY = std::max(maxY, m.positions[i * 3 + 1]);
  const float height = maxY - minY;
  if (height < 1.5f) return out;  // no pole
  // the light leaves the head downwards: faces in the upper part of the model whose normals point down
  struct Tri {
    Vec3 c;
    float area, low;
    float x0, x1, z0, z1;  // horizontal footprint
  };
  std::vector<Tri> tris;
  auto P = [&](uint32_t i) { return Vec3{m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]}; };
  for (size_t k = 0; k + 2 < m.indices.size(); k += 3) {
    uint32_t a = m.indices[k], b = m.indices[k + 1], c = m.indices[k + 2];
    if (a >= nv || b >= nv || c >= nv) continue;
    Vec3 pa = P(a), pb = P(b), pc = P(c);
    Vec3 cr = cross(pb - pa, pc - pa);
    float area = 0.5f * std::sqrt(dot(cr, cr));
    float ny = (m.normals[a * 3 + 1] + m.normals[b * 3 + 1] + m.normals[c * 3 + 1]) / 3.0f;
    Vec3 ctr = (pa + pb + pc) * (1.0f / 3.0f);
    if (area < 1e-5f || ny > -0.6f || ctr.y < minY + 0.55f * height) continue;
    tris.push_back({ctr, area, std::min({pa.y, pb.y, pc.y}), std::min({pa.x, pb.x, pc.x}), std::max({pa.x, pb.x, pc.x}),
                    std::min({pa.z, pb.z, pc.z}), std::max({pa.z, pb.z, pc.z})});
  }
  if (tris.empty()) {  // no underside: the top of the model
    Vec3 s;
    int count = 0;
    for (size_t i = 0; i < nv; ++i)
      if (m.positions[i * 3 + 1] > maxY - 0.3f) s = s + P(uint32_t(i)), ++count;
    if (count) out.push_back({s.x / float(count), maxY - 0.3f, s.z / float(count)});
    return out;
  }
  // one head per connected underside: faces whose footprints touch (within 0.2 m) are joined, so a long housing on
  // an arm is one head; heads whose centres are closer than 1.2 m are one (floodlight clusters)
  std::vector<int> parent(tris.size());
  for (size_t i = 0; i < tris.size(); ++i) parent[i] = int(i);
  auto root = [&](int i) {
    while (parent[size_t(i)] != i) i = parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
    return i;
  };
  for (size_t i = 0; i < tris.size(); ++i)
    for (size_t j = i + 1; j < tris.size(); ++j) {
      const Tri &a = tris[i], &b = tris[j];
      if (a.x0 - 0.2f <= b.x1 && b.x0 - 0.2f <= a.x1 && a.z0 - 0.2f <= b.z1 && b.z0 - 0.2f <= a.z1 &&
          std::fabs(a.c.y - b.c.y) < 1.0f)
        parent[size_t(root(int(i)))] = root(int(j));
    }
  struct Cluster {
    Vec3 sum;
    float area = 0, low = 1e30f;
    Vec3 centre() const { return sum * (1.0f / area); }
  };
  // an underside reaching over the pole axis on both sides (V-shaped double arms) is two heads, one per side
  std::map<int, std::pair<float, float>> spanX, spanZ;
  for (size_t i = 0; i < tris.size(); ++i) {
    auto& sx = spanX.try_emplace(root(int(i)), 1e30f, -1e30f).first->second;
    auto& sz = spanZ.try_emplace(root(int(i)), 1e30f, -1e30f).first->second;
    sx = {std::min(sx.first, tris[i].x0), std::max(sx.second, tris[i].x1)};
    sz = {std::min(sz.first, tris[i].z0), std::max(sz.second, tris[i].z1)};
  }
  std::map<int, Cluster> byRoot;
  for (size_t i = 0; i < tris.size(); ++i) {
    int r = root(int(i));
    const auto &sx = spanX[r], &sz = spanZ[r];
    bool alongX = sx.second - sx.first >= sz.second - sz.first;
    const auto& s = alongX ? sx : sz;
    float along = alongX ? tris[i].c.x : tris[i].c.z;
    int side = s.first < -0.8f && s.second > 0.8f && along > 0 ? 1 : 0;
    Cluster& c = byRoot[r * 2 + side];
    c.sum = c.sum + tris[i].c * tris[i].area;
    c.area += tris[i].area;
    c.low = std::min(c.low, tris[i].low);
  }
  std::vector<Cluster> cl;
  for (auto& [r, c] : byRoot) cl.push_back(c);
  std::sort(cl.begin(), cl.end(), [](const Cluster& a, const Cluster& b) { return a.area > b.area; });
  std::vector<Cluster> merged;
  for (const Cluster& c : cl) {
    Cluster* near = nullptr;
    for (Cluster& k : merged) {
      Vec3 d = k.centre() - c.centre();
      if (d.x * d.x + d.z * d.z < 1.44f) near = &k;
    }
    if (!near) {
      merged.push_back(c);
      continue;
    }
    near->sum = near->sum + c.sum;
    near->area += c.area;
    near->low = std::min(near->low, c.low);
  }
  float maxArea = 0;
  for (const Cluster& c : merged) maxArea = std::max(maxArea, c.area);
  for (const Cluster& c : merged)
    if (c.area >= 0.25f * maxArea) out.push_back({c.centre().x, c.low - 0.05f, c.centre().z});
  return out;
}

// ---- per frame ----

void addNightLights(Display& d, VM& vm, const std::vector<GridObj*>& grids, Object* sky, SceneView& v,
                    ParticleDrawList& particles) {
  NightState ns = skyNightState(sky);
  if (!ns.on) return;
  float windows = smooth(0.18f, 0.72f, ns.night01);
  float lamps = smooth(0.32f, 0.62f, ns.night01);
  v.night[0] = windows;
  v.night[1] = windowShare(float(ns.hours));
  v.night[2] = 0;
  v.night[3] = float(ns.hours / 24.0);
  GridObj* grid = nullptr;
  for (GridObj* g : grids)
    if (g->data && g->terrain && !g->data->items.empty()) grid = g;
  if (!grid) return;
  LampCache& lc = lampCaches()[grid];  // built while the cycle is on, so dusk brings no hitch
  if (lc.generation != grid->generation || lc.itemsVersion != grid->itemsVersion || !lc.texture) {
    lc.generation = grid->generation;
    lc.itemsVersion = grid->itemsVersion;
    updateLampMap(d, grid, lc);
  }
  const TerrainCache& tc = *grid->terrain;
  if (lamps <= 0 || tc.viewMode == 0x180) return;  // the underground view: no lamps, no buildings
  v.night[2] = lamps * kLampGain;
  v.lampMap = lc.texture;
  std::copy(lc.xf, lc.xf + 4, v.lampMapXf);
  v.lampHeight[0] = lc.y0;
  v.lampHeight[1] = lc.yScale;
  // head glows: lamps in front of the camera within 700 m, each switching on at its own point of the dusk
  GpuMaterial* mat = glowMaterial(d);
  uint32_t batch = uint32_t(particles.batches.size());
  particles.batches.push_back({mat, 0x100u});
  Mat4 vp = v.proj * v.view;
  std::vector<ParticleSprite> glows;
  for (const auto& h : lc.heads) {
    Vec3 dv = h.pos - v.eye;
    float dist2 = dot(dv, dv);
    if (dist2 > 700.0f * 700.0f) continue;
    float cw = vp.at(3, 0) * h.pos.x + vp.at(3, 1) * h.pos.y + vp.at(3, 2) * h.pos.z + vp.at(3, 3);
    if (cw <= 0.1f) continue;
    float cx = vp.at(0, 0) * h.pos.x + vp.at(0, 1) * h.pos.y + vp.at(0, 2) * h.pos.z + vp.at(0, 3);
    float cy = vp.at(1, 0) * h.pos.x + vp.at(1, 1) * h.pos.y + vp.at(1, 2) * h.pos.z + vp.at(1, 3);
    if (std::fabs(cx) > 1.2f * cw || std::fabs(cy) > 1.2f * cw) continue;
    float on = smooth(h.onAt, h.onAt + 0.04f, ns.night01);
    if (on <= 0) continue;
    float dist = std::sqrt(dist2);
    ParticleSprite s;
    s.pos = h.pos + dv * (-0.9f / std::max(dist, 1.0f));  // toward the eye, over the lamp housing
    s.size = 1.5f;
    uint32_t a = uint32_t(std::lround(255.0f * on));
    s.argb = a << 24 | 0xFFE0B0u;
    s.batch = batch;
    glows.push_back(s);
  }
  // additive: order does not matter among them; drawn before the scene's own particles (smoke in front stays in front)
  particles.sprites.insert(particles.sprites.begin(), glows.begin(), glows.end());
}

}  // namespace regs::engine
