// Night lights of the port's day/night cycle: lit windows and street lamps (engine/night_lights.h,
// docs/renderer/night-lights.md).
#include "engine/night_lights.h"
#include "engine/passenger_atlas.h"
#include "core/env.h"

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
#include "render/gpu.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/grid_render.h"
#include "engine/scene3d.h"
#include "engine/sim.h"
#include "engine/sky.h"
#include "render/renderer2d.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

bool debugNight() {
  static const bool on = regs::envVar("REGS_DEBUG_NIGHT") != nullptr;
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
// maxChannels: bit per byte of a BGRA texel whose smaller levels keep the brightest of the four instead of the mean
// (the vehicle lamps: a lamp of a texel or two keeps its full light at every distance, so the shader needs no boost
// that made it flicker as the car moved)
void buildMips(Image& img, unsigned maxChannels = 0) {
  img.mipCount = 1;
  int w = img.width, h = img.height;
  while (w > 1 || h > 1) {
    int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    const std::vector<uint8_t>& src = img.levels.back();
    std::vector<uint8_t> dst(size_t(nw) * size_t(nh) * 4);
    for (int y = 0; y < nh; ++y)
      for (int x = 0; x < nw; ++x)
        for (int c = 0; c < 4; ++c) {
          int sum = 0, n = 0, mx = 0;
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
              int sx = std::min(w - 1, 2 * x + dx), sy = std::min(h - 1, 2 * y + dy);
              int s = src[(size_t(sy) * size_t(w) + size_t(sx)) * 4 + size_t(c)];
              sum += s, mx = std::max(mx, s);
              ++n;
            }
          dst[(size_t(y) * size_t(nw) + size_t(x)) * 4 + size_t(c)] =
              uint8_t((maxChannels >> c & 1) ? mx : (sum + n / 2) / n);
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

// share of the shop windows lit over the day: displays stay lit long into the night
float shopShare(float hours) {
  static const float k[][2] = {{0, 0.45f}, {3, 0.32f}, {6, 0.45f}, {9, 0.6f}, {17, 0.9f}, {21, 0.9f}, {22.5f, 0.72f}, {24, 0.45f}};
  const int n = int(sizeof k / sizeof k[0]);
  hours = std::fmod(std::fmod(hours, 24.0f) + 24.0f, 24.0f);
  for (int i = 0; i + 1 < n; ++i)
    if (hours <= k[i + 1][0]) return k[i][1] + (k[i + 1][1] - k[i][1]) * (hours - k[i][0]) / (k[i + 1][0] - k[i][0]);
  return k[n - 1][1];
}

// the market's opening hours: share of the stands lit (evening and early morning, when it is dark at the stands)
float tentShare(float hours) {
  static const float k[][2] = {{0, 0}, {4.5f, 0}, {5.5f, 0.6f}, {6.5f, 0.9f}, {8, 0.85f}, {8.75f, 0}, {16, 0},
                               {17, 0.6f}, {18, 0.9f}, {20.5f, 0.8f}, {21.5f, 0.3f}, {22.25f, 0}, {24, 0}};
  const int n = int(sizeof k / sizeof k[0]);
  hours = std::fmod(std::fmod(hours, 24.0f) + 24.0f, 24.0f);
  for (int i = 0; i + 1 < n; ++i)
    if (hours <= k[i + 1][0]) return k[i][1] + (k[i + 1][1] - k[i][1]) * (hours - k[i][0]) / (k[i + 1][0] - k[i][0]);
  return 0;
}

constexpr float kCarGain = 3.2f;     // headlight light at the beam's brightest, in units of the surface colour
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
    uint32_t item = 0;
  };
  std::vector<Head> heads;
};

std::map<const GridObj*, LampCache>& lampCaches() {
  static std::map<const GridObj*, LampCache> m;
  return m;
}

// the light map: one RGBA16F texture for the lamp map and the headlight map (a texture unit fewer, render/renderer3d.h
// kNightMapUnit): the headlights in rows 0-2047 (drawn each frame), the lamp map from row 2048 (uploaded when it
// changes); 2048 x 4096
constexpr int kLightMapSide = 2048;
RenderTarget* lightMap(Display& d) {
  static RenderTarget t;
  static bool failed = false;
  if (!t.fbo && !failed && !d.r3d().makeTarget(t, kLightMapSide, 2 * kLightMapSide, TargetFormat::Rgba16f, false, true)) {
    LOG_WARN("night lights: no light map texture");
    failed = true;
  }
  return t.fbo ? &t : nullptr;
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

bool lampTemplate(const GridTemplate& tp) {
  return tp.object && tp.object->data && isLampTemplate(tp.object->pendingPath) && !ownGlow(*tp.object->data) &&
         !headsOf(*tp.object->data, tp.object->pendingPath).empty();
}

// the lamp heads of item i in world space (isLamp: per template, 1 = a lamp; null: looked up)
void itemHeads(const GridData& g, const TerrainCache& tc, uint32_t i, const std::vector<uint8_t>* isLamp,
               std::vector<LampCache::Head>& out) {
  constexpr uint32_t kHidden = 0x400000;
  const GridData::Item& it = g.items[i];
  if (it.removed || it.type >= tc.typeTemplate.size()) return;
  int ti = tc.typeTemplate[it.type];
  if (ti < 0 || !(isLamp ? (*isLamp)[size_t(ti)] != 0 : lampTemplate(tc.templates[size_t(ti)]))) return;
  const GridTemplate& tp = tc.templates[size_t(ti)];
  if ((it.flags | tp.flags) & kHidden) return;
  int div = itemDivision(g, tc, i);
  if (div < 0) return;
  Mat4 world = tc.divisions[size_t(div)].world * itemPlacement(g, tc, i, div);
  uint32_t k = 0;
  for (const Vec3& h : headsOf(*tp.object->data, tp.object->pendingPath)) {
    LampCache::Head head;
    head.pos = transformPoint(world, h);
    head.onAt = 0.36f + 0.14f * hash01(uint32_t(it.x) * 7919u ^ uint32_t(it.z) * 104729u ^ k++);
    head.item = i;
    out.push_back(head);
  }
}

// the lamp heads of the map in world space, in item order
std::vector<LampCache::Head> collectHeads(GridObj* grid) {
  const GridData& g = *grid->data;
  const TerrainCache& tc = *grid->terrain;
  std::vector<uint8_t> isLamp(tc.templates.size(), 0);
  for (size_t t = 0; t < tc.templates.size(); ++t) isLamp[t] = lampTemplate(tc.templates[t]);
  std::vector<LampCache::Head> heads;
  for (uint32_t i = 0; i < g.items.size(); ++i) itemHeads(g, tc, i, &isLamp, heads);
  return heads;
}

}  // namespace

// live edits of the items since `version` for a cache of the renderer's item data (night and traffic lights): false
// when the batches are not kept or the renderer has not registered every type yet (collectGrid does that earlier in
// the frame, syncGridItems); then the cache walks all items
bool renderItemsTouchedSince(const GridObj* grid, uint64_t version, std::vector<uint32_t>& touched) {
  return grid->data->types.size() <= grid->terrain->typeTemplate.size() && gridItemsTouchedSince(grid, version, touched);
}

namespace {

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
  bool full = !lc.texture || lc.builtFor != grid->generation;
  // live edits (a tool preview on every mouse move): the heads of the touched items only, and the heads that differ
  // straight from them (the full walk below finds them by comparing all heads)
  static std::vector<uint32_t> touched;
  std::vector<LampCache::Head> heads, changed;
  bool known = false;  // `changed` holds the differing heads
  if (!full && lc.generation == grid->generation && renderItemsTouchedSince(grid, lc.itemsVersion, touched)) {
    std::vector<LampCache::Head> fresh, dropped;
    for (uint32_t i : touched)
      if (i < g.items.size()) itemHeads(g, tc, i, nullptr, fresh);
    heads = lc.heads;
    gridMergeTouched(heads, touched, fresh, [&](LampCache::Head& old, LampCache::Head*) { dropped.push_back(old); });
    // (fresh was moved into heads: its heads are those of the touched items there)
    for (const auto& h : heads)
      if (std::binary_search(touched.begin(), touched.end(), h.item)) fresh.push_back(h);
    auto key = [](const LampCache::Head& a, const LampCache::Head& b) {
      return std::tie(a.pos.x, a.pos.y, a.pos.z) < std::tie(b.pos.x, b.pos.y, b.pos.z);
    };
    std::sort(dropped.begin(), dropped.end(), key);
    std::sort(fresh.begin(), fresh.end(), key);
    std::set_symmetric_difference(dropped.begin(), dropped.end(), fresh.begin(), fresh.end(), std::back_inserter(changed), key);
    known = true;
    if (gridDebugItemCaches()) {
      std::vector<LampCache::Head> all = collectHeads(grid);
      bool same = all.size() == heads.size();
      for (size_t k = 0; same && k < all.size(); ++k)
        same = all[k].item == heads[k].item && all[k].pos.x == heads[k].pos.x && all[k].pos.y == heads[k].pos.y &&
               all[k].pos.z == heads[k].pos.z && all[k].onAt == heads[k].onAt;
      gridItemCacheChecked("lamp heads", same);
    }
  } else {
    heads = collectHeads(grid);
  }
  lc.generation = grid->generation;
  lc.itemsVersion = grid->itemsVersion;
  auto same = [](const LampCache::Head& a, const LampCache::Head& b) {
    return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z;
  };
  float yMin = 1e30f, yMax = -1e30f;
  if (known) {
    if (changed.empty()) {
      lc.heads = std::move(heads);
      return;
    }
    for (const auto& h : changed) yMin = std::min(yMin, h.pos.y), yMax = std::max(yMax, h.pos.y);
    if (yMin < lc.y0 || yMax > lc.y0 + lc.yScale) {  // out of the height encoding: all again, its range from all heads
      full = true;
      yMin = 1e30f, yMax = -1e30f;
    }
  }
  if (!known || full)
    for (const auto& h : heads) yMin = std::min(yMin, h.pos.y), yMax = std::max(yMax, h.pos.y);
  if (!full && !known) {
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
    std::vector<LampCache::Head> diff;
    if (known) {
      diff = std::move(changed);
    } else {
      std::vector<LampCache::Head> a = lc.heads, b = heads;
      std::sort(a.begin(), a.end(), key);
      std::sort(b.begin(), b.end(), key);
      std::set_symmetric_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(diff), key);
    }
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
  if (known && !full && gridDebugItemCaches()) {  // the area against the whole map splatted again
    LampCache all = lc;
    std::fill(all.px.begin(), all.px.end(), uint8_t(0));
    splatRect(all, 0, 0, all.w - 1, all.h - 1);
    gridItemCacheChecked("lamp map", all.px == lc.px);
  }
  // one level, linear: from the highest camera a screen pixel covers about one texel, so no mipmaps are needed
  // into the light map's upper half (all of it cleared first on a full build: a smaller map leaves no old texels)
  RenderTarget* lm = lightMap(d);
  if (!lm) return;
  d.r2d().flushAll();
  using render::gpu::Pixels;
  if (full) {
    std::vector<uint8_t> zero(size_t(kLightMapSide) * kLightMapSide * 4, 0);
    render::gpu::dev().texSubImage(lm->color, 0, 0, kLightMapSide, kLightMapSide, kLightMapSide, Pixels::BGRA8, zero.data());
  }
  if (full || !lc.texture)
    render::gpu::dev().texSubImage(lm->color, 0, 0, kLightMapSide, lc.w, lc.h, Pixels::BGRA8, lc.px.data());
  else  // an area: its rows (whole rows are contiguous in px)
    render::gpu::dev().texSubImage(lm->color, 0, 0, kLightMapSide + z0, lc.w, z1 - z0 + 1, Pixels::BGRA8,
                                   lc.px.data() + size_t(z0) * size_t(lc.w) * 4);
  lc.texture = lm->color;  // not owned: marks the lamp map as uploaded
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

// a point in the view of the scene camera, with a margin (1 = the screen edge)
bool inViewOf(const SceneView& v, const Vec3& p, float margin) {
  Mat4 vp = v.proj * v.view;
  float cw = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
  if (cw <= 0.1f) return false;
  float cx = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
  float cy = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
  return std::fabs(cx) <= margin * cw && std::fabs(cy) <= margin * cw;
}

// ---- vehicles ----

// cells covered by buildings (templates under a buildings folder), for the high-beam choice
struct BuiltUp {
  uint64_t generation = ~0ull, itemsVersion = ~0ull;
  int w = 0, h = 0;
  float ox = 0, oz = 0, cs = 3;
  std::vector<uint16_t> cell;     // the buildings on each cell
  std::vector<uint8_t> building;  // per item: counted in cell (live edits take it out again)
  struct Tent {
    Vec3 pos;  // the stand's ground centre
    float radius = 3;
    uint32_t seed = 0;
    uint32_t item = 0;
  };
  std::vector<Tent> tents;  // market stands: a lamp under each canopy
  bool cellAt(float x, float z) const {  // a building on the cell of (x, z)
    int cx = int(std::floor((x - ox) / cs)), cz = int(std::floor((z - oz) / cs));
    return cx >= 0 && cz >= 0 && cx < w && cz < h && cell[size_t(cz) * size_t(w) + size_t(cx)];
  }
  bool at(float x, float z) const {  // a building within one cell of (x, z)
    int cx = int(std::floor((x - ox) / cs)), cz = int(std::floor((z - oz) / cs));
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx) {
        int a = cx + dx, b = cz + dz;
        if (a >= 0 && b >= 0 && a < w && b < h && cell[size_t(b) * size_t(w) + size_t(a)]) return true;
      }
    return false;
  }
};

// a building's cells in BuiltUp::cell, counted up (+1) or down (−1)
void stampBuilding(BuiltUp& bu, const GridTemplate& tp, const GridData::Item& it, int delta) {
  ItemFootprint f = itemFootprint(tp.width, tp.length, 0, it.orient);  // (buildings only from here)
  for (int z = std::max(0, int(it.z)); z < std::min(bu.h, it.z + f.sz); ++z)
    for (int x = std::max(0, int(it.x)); x < std::min(bu.w, it.x + f.sx); ++x)
      bu.cell[size_t(z) * size_t(bu.w) + size_t(x)] = uint16_t(bu.cell[size_t(z) * size_t(bu.w) + size_t(x)] + delta);
}

// a market stand's lamp, or false
bool makeTent(const GridData& g, const TerrainCache& tc, uint32_t i, BuiltUp::Tent& t) {
  const GridData::Item& it = g.items[i];
  if (it.removed || it.type >= tc.typeTemplate.size() || tc.typeTemplate[it.type] < 0) return false;
  const GridTemplate& tp = tc.templates[size_t(tc.typeTemplate[it.type])];
  if (!tp.object || !tp.object->data) return false;
  const std::string& p = tp.object->pendingPath;
  if (p.find("marketstand") == std::string::npos) return false;
  int div = itemDivision(g, tc, i);
  if (div < 0) return false;
  const MeshData& me = tp.object->data->mesh;
  Mat4 world = tc.divisions[size_t(div)].world * itemPlacement(g, tc, i, div);
  t.pos = transformPoint(world, Vec3{0.5f * (me.boundsMin[0] + me.boundsMax[0]), me.boundsMin[1], 0.5f * (me.boundsMin[2] + me.boundsMax[2])});
  t.radius = 1.3f * 0.5f * std::max(me.boundsMax[0] - me.boundsMin[0], me.boundsMax[2] - me.boundsMin[2]) + 1.0f;
  t.seed = uint32_t(it.x) * 73856093u ^ uint32_t(it.z) * 19349663u;
  t.item = i;
  return true;
}

void updateBuiltUp(GridObj* grid, BuiltUp& bu) {
  const GridData& g = *grid->data;
  const TerrainCache& tc = *grid->terrain;
  auto isBuilding = [&](const GridTemplate& tp) { return tp.object && tp.object->pendingPath.find("/buildings/") != std::string::npos; };
  // live edits (a tool preview on every mouse move): only the touched items
  static std::vector<uint32_t> touched;
  if (bu.generation == grid->generation && renderItemsTouchedSince(grid, bu.itemsVersion, touched)) {
    bu.itemsVersion = grid->itemsVersion;
    bu.building.resize(g.items.size(), 0);
    std::vector<BuiltUp::Tent> fresh;
    for (uint32_t i : touched) {
      if (i >= g.items.size()) continue;
      const GridData::Item& it = g.items[i];
      int ti = it.type < tc.typeTemplate.size() ? tc.typeTemplate[it.type] : -1;
      bool now = !it.removed && ti >= 0 && isBuilding(tc.templates[size_t(ti)]);
      if (bool(bu.building[i]) != now && ti >= 0) {  // (an item's place and template never change)
        stampBuilding(bu, tc.templates[size_t(ti)], it, now ? 1 : -1);
        bu.building[i] = now;
      }
      BuiltUp::Tent t;
      if (makeTent(g, tc, i, t)) fresh.push_back(t);
    }
    gridMergeTouched(bu.tents, touched, fresh, [](BuiltUp::Tent&, BuiltUp::Tent*) {});
    if (gridDebugItemCaches()) {  // the full walk below, compared
      BuiltUp full;
      full.generation = ~0ull;
      updateBuiltUp(grid, full);
      bool same = full.cell == bu.cell && full.tents.size() == bu.tents.size();
      for (size_t k = 0; same && k < full.tents.size(); ++k)
        same = full.tents[k].item == bu.tents[k].item && full.tents[k].pos.x == bu.tents[k].pos.x &&
               full.tents[k].pos.z == bu.tents[k].pos.z && full.tents[k].radius == bu.tents[k].radius;
      gridItemCacheChecked("built-up cells", same);
    }
    return;
  }
  bu.generation = grid->generation;
  bu.itemsVersion = grid->itemsVersion;
  bu.w = g.width, bu.h = g.length, bu.cs = g.cellSize;
  bu.ox = -(float(tc.n / 2) + float((g.width - tc.n) >> 1)) * g.cellSize;
  bu.oz = -(float(tc.n / 2) + float((g.length - tc.n) >> 1)) * g.cellSize;
  bu.cell.assign(size_t(bu.w) * size_t(bu.h), 0);
  bu.building.assign(g.items.size(), 0);
  std::vector<int8_t> building(tc.templates.size(), -1);
  for (uint32_t i = 0; i < g.items.size(); ++i) {
    const GridData::Item& it = g.items[i];
    if (it.removed || it.type >= tc.typeTemplate.size()) continue;
    int ti = tc.typeTemplate[it.type];
    if (ti < 0) continue;
    const GridTemplate& tp = tc.templates[size_t(ti)];
    int8_t& b = building[size_t(ti)];
    if (b < 0) b = isBuilding(tp);
    if (!b) continue;
    stampBuilding(bu, tp, it, 1);
    bu.building[i] = 1;
  }
  bu.tents.clear();
  for (uint32_t i = 0; i < g.items.size(); ++i) {
    BuiltUp::Tent t;
    if (makeTent(g, tc, i, t)) bu.tents.push_back(t);
  }
}

// the vehicles' light map: headlight beams and the market stands' lamps, drawn each frame on the GPU into a float
// target around the camera's view centre. Each light is a quad whose fragment shader evaluates its profile from
// metric local coordinates, so the edges are exact at any angle (a CPU raster at 0.94 m texels left them jagged).
// Channels: R = light, G = light from the stands' lamps (warm tint), B = light × (its ground height − y0).
constexpr int kCarMap = 2048;
constexpr float kCarMapSize = 960.0f;  // metres across: 0.47 m texels; beams fade out before its edge

const char* kSplatVs =
    "varying vec4 v_a;\n"
    "varying vec4 v_b;\n"
    "void main() {\n"
    "  gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0);\n"
    "  v_a = vec4(gl_Vertex.z, gl_Normal);\n"
    "  v_b = vec4(gl_MultiTexCoord0.xy, gl_MultiTexCoord1.xy);\n"
    "}\n";
// v_a = (along, across, length, start half width) in metres; v_b = (end half width, gain, height − y0, warm); a length
// of 0 is a round lamp of radius v_a.w
const char* kSplatFs =
    "varying vec4 v_a;\n"
    "varying vec4 v_b;\n"
    "void main() {\n"
    "  float along = v_a.x, across = v_a.y, len = v_a.z, w0 = v_a.w, l = 0.0;\n"
    "  if (len > 0.0) {\n"
    "    float al = along / len;\n"
    "    float hwid = w0 + (v_b.x - w0) * clamp(al, 0.0, 1.0);\n"
    "    float a = abs(across) / hwid;\n"
    "    if (al > 0.0 && al < 1.0 && a < 1.0)\n"
    "      l = smoothstep(1.0, 0.35, a) * (0.7 + 0.3 * (1.0 - a * a)) * smoothstep(0.0, 0.1, al) * pow(1.0 - al, 1.6);\n"
    "  } else {\n"
    "    float q = 1.0 - (along * along + across * across) / (w0 * w0);\n"
    "    if (q > 0.0) l = q * q;\n"
    "  }\n"
    "  l *= v_b.y;\n"
    "  gl_FragColor = vec4(l, l * v_b.w, l * v_b.z, 0.0);\n"
    "}\n";

struct VehicleLightState {
  std::map<const GridObj*, BuiltUp> builtUp;
  std::vector<float> beam;     // per mover: 0 low beam … 1 high beam, eased
  std::vector<float> beamLen;  // per mover: the beam's reach, eased (a building ahead shortens it smoothly)
  std::vector<VehicleLightSource> sources;

  uint32_t program = 0;
  bool failed = false;
  GpuMesh mesh;
};
VehicleLightState& vehicleState() {
  static VehicleLightState s;
  return s;
}

struct Splat {
  Vec3 o, f, s;          // origin, forward and side (horizontal, unit)
  float len, w0, w1;     // len 0: a round lamp of radius w0
  float gain, ground;
  float warm;
};

void addVehicleLights(Display& d, VM& vm, GridObj* grid, const LampCache& lc, SceneView& v, float night01, float hours,
                      uint32_t batch, std::vector<ParticleSprite>& glows) {
  VehicleLightState& st = vehicleState();
  simVehicleLights(grid, v.eye, 650.0f, st.sources);
  const auto& src = st.sources;
  BuiltUp& bu = st.builtUp[grid];
  if (bu.generation != grid->generation || bu.itemsVersion != grid->itemsVersion) updateBuiltUp(grid, bu);
  // other vehicles near a point: a hash of the fronts and backs on a 32 m grid
  std::unordered_map<int64_t, std::vector<int>> hash;
  auto key = [](float x, float z) { return (int64_t(std::floor(x / 32.0f)) << 32) ^ (int64_t(std::floor(z / 32.0f)) & 0xFFFFFFFF); };
  for (int i = 0; i < int(src.size()); ++i) {
    hash[key(src[size_t(i)].front.x, src[size_t(i)].front.z)].push_back(i);
    hash[key(src[size_t(i)].back.x, src[size_t(i)].back.z)].push_back(i);
  }
  auto lampLit = [&](const Vec3& p) {  // a street the lamps light (the lamp map's red channel)
    if (lc.px.empty()) return false;
    int x = int((p.x - lc.xf[0]) / lc.texel[0]), z = int((p.z - lc.xf[1]) / lc.texel[1]);
    return x >= 0 && z >= 0 && x < lc.w && z < lc.h && lc.px[(size_t(z) * size_t(lc.w) + size_t(x)) * 4 + 2] > 50;
  };
  float dt = std::clamp(vm.frameDt(), 0.0f, 0.1f);
  // the map window: centred where the view axis meets the ground, snapped to whole texels (no shimmer as it moves)
  const float texel = kCarMapSize / float(kCarMap);
  Vec3 fwd{-v.view.at(2, 0), -v.view.at(2, 1), -v.view.at(2, 2)};
  float t = fwd.y < -0.05f ? std::min(800.0f, (v.eye.y - 0.0f) / -fwd.y) : 200.0f;
  Vec3 c = v.eye + fwd * t;
  float ox = std::floor((c.x - kCarMapSize / 2) / texel) * texel, oz = std::floor((c.z - kCarMapSize / 2) / texel) * texel;
  std::vector<Splat> splats;
  Vec3 up{0, 1, 0};
  for (const VehicleLightSource& L : src) {
    if (L.mover >= int(st.beam.size())) st.beam.resize(size_t(L.mover) + 1, 0.0f);
    Vec3 f = L.forward;
    Vec3 side = normalize(cross(up, f));
    // high beam (road vehicles): no building, lamp-lit street or other vehicle in the 60 m ahead
    bool high = !L.rail;
    for (float a = 8.0f; high && a <= 56.0f; a += 6.0f) {
      Vec3 p = L.front + f * a;
      for (float s = -6.0f; high && s <= 6.0f; s += 6.0f) high = !bu.at(p.x + side.x * s, p.z + side.z * s);
      if (high && a <= 44.0f) high = !lampLit(p);
    }
    for (float a = 0.0f; high && a <= 96.0f; a += 32.0f) {
      Vec3 q0 = L.front + f * a;
      for (int dz = -1; dz <= 1 && high; ++dz)
        for (int dx = -1; dx <= 1 && high; ++dx) {
          auto it = hash.find(key(q0.x + float(dx) * 32.0f, q0.z + float(dz) * 32.0f));
          if (it == hash.end()) continue;
          for (int j : it->second) {
            const VehicleLightSource& o = src[size_t(j)];
            if (o.mover == L.mover) continue;
            for (const Vec3& q : {o.front, o.back}) {
              Vec3 e = q - L.front;
              float along = dot(e, f), lat = std::fabs(dot(e, side));
              if (along > 1.0f && along < 90.0f && lat < 4.0f + 0.3f * along && std::fabs(e.y) < 8.0f) high = false;
            }
          }
        }
    }
    float& b = st.beam[size_t(L.mover)];
    b += ((high ? 1.0f : 0.0f) - b) * std::min(1.0f, dt * 3.0f);
    // each driver switches on at an own point of the dusk (night01 0.12 … 0.55), over a moment
    float strength = vehicleLightsOn(L.seed, night01);
    if (strength <= 0) continue;
    float hw = std::max(0.3f, L.halfWidth * 0.68f);
    // head and tail light glows on the bumpers, only for ends whose model has no lamps found on its texture
    bool frontLamps = false, rearLamps = false;
    if (L.frontData) {
      VehicleLampInfo li = vehicleLamps(d, vm, *L.frontData);
      frontLamps = (L.frontIsModelRear ? li.rearLamps : li.frontLamps) > 0;
    }
    if (L.backData) {
      VehicleLampInfo li = vehicleLamps(d, vm, *L.backData);
      rearLamps = (L.backIsModelFront ? li.frontLamps : li.rearLamps) > 0;
    }
    if (!frontLamps && inViewOf(v, L.front, 1.3f))
      for (float sgn : {-1.0f, 1.0f}) {
        ParticleSprite s;
        s.pos = L.front + f * 0.04f + side * (sgn * hw) + up * L.frontY;
        s.size = 0.32f + 0.18f * b;
        s.argb = uint32_t(std::lround(255.0f * strength)) << 24 | 0xFFF4E0u;
        s.batch = batch;
        glows.push_back(s);
      }
    if (!rearLamps && inViewOf(v, L.back, 1.3f)) {
      Vec3 bf = L.backForward, bs = normalize(cross(up, bf));
      bool braking = L.speed < 0.5f;
      for (float sgn : {-1.0f, 1.0f}) {
        ParticleSprite s;
        s.pos = L.back - bf * 0.04f + bs * (sgn * hw) + up * L.backY;
        s.size = braking ? 0.28f : 0.2f;
        s.argb = uint32_t(std::lround(255.0f * strength * (braking ? 1.0f : 0.7f))) << 24 | 0xFF1E0Cu;
        s.batch = batch;
        glows.push_back(s);
      }
    }
    // the beam into the light map: low beam 15-17 m, high beam 42 m, stopped by the first building ahead
    // the reach stops at the first building on the beam's centre line (its own cell, so walls beside the road do
    // not cut it), eased per vehicle so it never jumps
    float reach = (L.bus ? 17.0f : 15.0f) + 27.0f * b;
    for (float a = 1.0f; a < reach; a += 0.75f)
      if (Vec3 p = L.front + f * a; bu.cellAt(p.x, p.z)) {
        reach = std::max(4.0f, a + 1.5f);
        break;
      }
    if (L.mover >= int(st.beamLen.size())) st.beamLen.resize(size_t(L.mover) + 1, -1.0f);
    float& len = st.beamLen[size_t(L.mover)];
    len = len < 0 ? reach : len + (reach - len) * std::min(1.0f, dt * 4.0f);
    float w0 = std::max(0.7f, L.halfWidth * 0.95f), w1 = 3.4f + 1.4f * b;
    // fading out towards the edge of the vehicles' range and of the map window instead of a cut
    Vec3 te = L.front - v.eye;
    float edge = std::min({L.front.x - ox, ox + kCarMapSize - L.front.x, L.front.z - oz, oz + kCarMapSize - L.front.z});
    float gain = (1.0f + 0.25f * b) * strength * smooth(650.0f, 520.0f, std::sqrt(dot(te, te))) * smooth(40.0f, 90.0f, edge);
    if (gain <= 0) continue;
    Vec3 f2 = normalize(Vec3{f.x, 0, f.z}), s2 = normalize(Vec3{side.x, 0, side.z});
    splats.push_back({L.front + f * 0.1f, f2, s2, len, w0, w1, gain, L.front.y, 0.0f});
  }
  // market stands: a warm lamp under the canopy, in the evening and early morning, each stand at its own time; it
  // lights the goods and the ground under and around the stand, not the canopy above it (its height lies 0.6 m
  // below the ground, so the map's height window ends ~2.6 m up)
  if (float share = tentShare(hours); share > 0)
    for (const BuiltUp::Tent& t : bu.tents) {
      float open = 0.1f + 0.85f * hash01(t.seed);
      float on = smooth(open - 0.05f, open + 0.05f, share) * smooth(0.1f, 0.4f, night01);
      float edge = std::min({t.pos.x - ox, ox + kCarMapSize - t.pos.x, t.pos.z - oz, oz + kCarMapSize - t.pos.z});
      if (on <= 0 || edge < t.radius) continue;
      splats.push_back({t.pos, {1, 0, 0}, {0, 0, 1}, 0.0f, t.radius, 0.0f, 0.75f * on, t.pos.y - 0.6f, 1.0f});
    }
  // the other parts' round lights (extraNightLights: the traffic lights' glow on the street)
  for (const ExtraNightLight& e : extraNightLights()) {
    float edge = std::min({e.pos.x - ox, ox + kCarMapSize - e.pos.x, e.pos.z - oz, oz + kCarMapSize - e.pos.z});
    float on = smooth(0.1f, 0.4f, night01);
    if (on <= 0 || edge < e.radius) continue;
    splats.push_back({e.pos, {1, 0, 0}, {0, 0, 1}, 0.0f, e.radius, 0.0f, e.gain * on, e.pos.y, e.warm});
  }
  if (splats.empty() || st.failed) return;
  if (!st.program) {
    std::string err;
    st.program = d.r3d().buildProgram(kSplatVs, kSplatFs, &err);
    if (!st.program || !lightMap(d)) {
      LOG_WARN("night lights: the vehicles' light map pass is not available (%s)", err.c_str());
      st.failed = true;
      return;
    }
  }
  // a height reference that moves in whole 16 m steps (B holds light × height above it)
  float yMin = 1e30f;
  for (const Splat& sp : splats) yMin = std::min(yMin, sp.ground);
  float y0 = std::floor((yMin - 4.0f) / 16.0f) * 16.0f;
  MeshData m;
  auto vert = [&](const Splat& sp, float along, float across) {
    Vec3 p = sp.o + sp.f * along + sp.s * across;
    m.positions.insert(m.positions.end(), {(p.x - ox) / kCarMapSize * 2 - 1, (p.z - oz) / kCarMapSize * 2 - 1, along});
    m.normals.insert(m.normals.end(), {across, sp.len, sp.w0});
    m.uv0.insert(m.uv0.end(), {sp.w1, sp.gain});
    m.uv1.insert(m.uv1.end(), {sp.ground - y0, sp.warm});
  };
  for (const Splat& sp : splats) {
    uint32_t base = uint32_t(m.vertexCount());
    if (sp.len > 0) {  // the beam's trapezoid, half a metre larger all round (the profile ends inside it)
      vert(sp, -0.5f, -(sp.w0 + 0.5f));
      vert(sp, -0.5f, sp.w0 + 0.5f);
      vert(sp, sp.len + 0.5f, sp.w1 + 0.5f);
      vert(sp, sp.len + 0.5f, -(sp.w1 + 0.5f));
    } else {
      float r = sp.w0 + 0.5f;
      vert(sp, -r, -r), vert(sp, -r, r), vert(sp, r, r), vert(sp, r, -r);
    }
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }
  m.submeshes.push_back({0, uint32_t(m.vertexCount()), 0, uint32_t(m.indices.size()), 0});
  if (st.mesh.vao) d.r3d().destroy(st.mesh);
  st.mesh = d.r3d().upload(m);
  d.r2d().flushAll();
  RenderTarget* lm = lightMap(d);
  if (!lm) return;
  d.r3d().splat(*lm, st.program, st.mesh, d.window().width(), d.window().height(), kCarMap);  // the lower half
  v.carMap = lm->color;
  v.carMapXf[0] = ox, v.carMapXf[1] = oz, v.carMapXf[2] = v.carMapXf[3] = 1.0f / kCarMapSize;
  v.carLight[0] = y0, v.carLight[1] = 0, v.carLight[2] = kCarGain;
}

}  // namespace

std::vector<ExtraNightLight>& extraNightLights() {
  static std::vector<ExtraNightLight> v;
  return v;
}

// ---- windows ----

bool makeWindowMask(const Image& specular, const Image* illumination, Image& out, WindowMaskInfo* info,
                    const MeshData* mesh, int material) {
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
  // shop windows: glass whose texels the mesh uses only within 4.2 m of the model's ground (its origin); they get
  // their own channel (A), the other windows three random groups (B, G, R)
  std::vector<uint8_t> floorOf(n, 0);  // bit 1: a ground-floor face uses the texel, bit 2: a face above it
  if (mesh && mesh->uv0.size() >= mesh->vertexCount() * 2) {
    const size_t nv = mesh->vertexCount();
    for (const Submesh& sm : mesh->submeshes) {
      if (sm.material != material) continue;
      for (uint32_t k = sm.firstIndex; k + 2 < sm.firstIndex + sm.indexCount && k + 2 < mesh->indices.size(); k += 3) {
        uint32_t t[3] = {mesh->indices[k], mesh->indices[k + 1], mesh->indices[k + 2]};
        if (t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
        float u[3], v[3];
        float ub = std::floor(std::min({mesh->uv0[t[0] * 2], mesh->uv0[t[1] * 2], mesh->uv0[t[2] * 2]}));
        float vb = std::floor(std::min({mesh->uv0[t[0] * 2 + 1], mesh->uv0[t[1] * 2 + 1], mesh->uv0[t[2] * 2 + 1]}));
        for (int j = 0; j < 3; ++j) u[j] = (mesh->uv0[t[j] * 2] - ub) * float(w), v[j] = (mesh->uv0[t[j] * 2 + 1] - vb) * float(h);
        float area = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
        if (std::fabs(area) < 1e-6f) continue;
        int x0 = std::max(0, int(std::floor(std::min({u[0], u[1], u[2]})))), x1 = std::min(w - 1, int(std::ceil(std::max({u[0], u[1], u[2]}))));
        int y0 = std::max(0, int(std::floor(std::min({v[0], v[1], v[2]})))), y1 = std::min(h - 1, int(std::ceil(std::max({v[0], v[1], v[2]}))));
        for (int y = y0; y <= y1; ++y)
          for (int x = x0; x <= x1; ++x) {
            float px = float(x) + 0.5f, py = float(y) + 0.5f;
            float b1 = ((px - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (py - v[0])) / area;
            float b2 = ((u[1] - u[0]) * (py - v[0]) - (px - u[0]) * (v[1] - v[0])) / area;
            float b0 = 1 - b1 - b2;
            if (b0 < -0.02f || b1 < -0.02f || b2 < -0.02f) continue;
            float py3 = b0 * mesh->positions[t[0] * 3 + 1] + b1 * mesh->positions[t[1] * 3 + 1] + b2 * mesh->positions[t[2] * 3 + 1];
            floorOf[size_t(y) * size_t(w) + size_t(x)] |= py3 < 4.2f ? 1 : 2;
          }
      }
    }
  }
  std::vector<size_t> groundGlass(comps.size(), 0);
  for (size_t i = 0; i < n; ++i)
    if (label[i] >= 0 && glass[i] && floorOf[i] == 1) ++groundGlass[size_t(label[i])];
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
    bool shop = groundGlass[c] * 10 >= comps[c].glass * 6;
    group[c] = shop ? 3 : int((hc >> 20) % 3);
    st.shops += shop;
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
  const Vfs* vfs = nullptr;
  const MeshData* mesh = nullptr;
  int material = -1;
  std::string key, spec, illum, objectPath;
  Image mask;
  WindowMaskInfo info;
  bool ok = false;
};

// the reading and inflating of the source maps is part of the job: prepareWindowMasks runs the jobs on worker threads
// (Vfs::read only reads; nothing rescans or writes the file system while they run)
void runMaskJob(MaskJob& j) {
  std::vector<uint8_t> specData, illumData;
  j.vfs->read(j.spec, specData);
  if (!j.illum.empty()) j.vfs->read(j.illum, illumData);
  Image si, ii;
  bool haveIllum = !illumData.empty() && parseDds(illumData, ii, nullptr);
  j.ok = !specData.empty() && parseDds(specData, si, nullptr) && makeWindowMask(si, haveIllum ? &ii : nullptr, j.mask, &j.info, j.mesh, j.material);
}

void finishMaskJob(Display& d, MaskJob& j) {
  uint32_t tex = j.ok ? d.r2d().createTexture(j.mask) : 0;
  if (debugNight())
    LOG_INFO("night: windows %s (%s): %d windows (%d shop), %d rejected, glass > %.2f, %.1f %% %s", j.spec.c_str(), j.objectPath.c_str(),
             j.info.windows, j.info.shops, j.info.rejected, j.info.threshold, j.info.coverage * 100, j.info.skipped ? j.info.skipped : "");
  maskCache().emplace(j.key, tex);
}

bool newJob(VM& vm, const MaterialDef& m, const std::string& objectPath, MaskJob& j, const MeshData* mesh, int material) {
  j.vfs = &vm.vfs();
  j.mesh = mesh, j.material = material;
  if (!maskSources(m, objectPath, j.spec, j.illum)) return false;
  j.key = j.spec + "|" + j.illum;
  if (maskCache().count(j.key)) return false;
  j.objectPath = objectPath;
  return true;
}

}  // namespace

void prepareWindowMasks(Display& d, VM& vm, const std::vector<std::pair<const ObjectData*, std::string>>& objects) {
  auto t0 = std::chrono::steady_clock::now();
  std::vector<MaskJob> jobs;
  std::set<std::string> queued;
  for (const auto& [od, path] : objects)
    for (size_t i = 0; i < od->materials.size(); ++i) {
      MaskJob j;
      if (newJob(vm, od->materials[i], path, j, &od->mesh, int(i)) && queued.insert(j.key).second) jobs.push_back(std::move(j));
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

uint32_t buildingWindowMask(Display& d, VM& vm, const MaterialDef& m, const std::string& objectPath, const MeshData* mesh,
                            int material) {
  std::string spec, illum;
  if (!maskSources(m, objectPath, spec, illum)) return 0;
  if (auto it = maskCache().find(spec + "|" + illum); it != maskCache().end()) return it->second;
  MaskJob j;  // not prepared (live edits bring a new template): made here
  if (!newJob(vm, m, objectPath, j, mesh, material)) return 0;
  runMaskJob(j);
  finishMaskJob(d, j);
  return maskCache()[j.key];
}

// ---- vehicle lamps ----

namespace {

// Vehicles whose head lamps the search below gets wrong, by their diffuse texture: the front lamps are these lens
// discs instead (texture fractions u, v, radius in widths), and nothing else on that texture
struct LampFix {
  const char* texture;
  int count;  // lamps on the model (VehicleLampInfo::frontLamps)
  struct Disc {
    float u, v, r;
  };
  Disc discs[3];
};
const LampFix kLampFixes[] = {
    // the 1920s Berlin tram (tram-01): the two roof-corner lamps (both on the hexagonal lens at texel (158, 127), above
    // the search's height band) and the lamp in the middle of the dash (the round lens at texel (350, 171)); the
    // search took the cream panel under the windows instead (one large glow, off centre and far too bright)
    {"metro/objects/vehicles/tram-01/tram01.dds", 3,
     {{158.5f / 512.0f, 127.5f / 256.0f, 4.5f / 512.0f}, {349.5f / 512.0f, 170.5f / 256.0f, 4.5f / 512.0f}, {0, 0, 0}}},
};

const LampFix* lampFixFor(const std::string& texture) {
  std::string t = texture;
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  for (const LampFix& f : kLampFixes)
    if (t == f.texture) return &f;
  return nullptr;
}

}  // namespace

bool makeVehicleLampMask(const MeshData& mesh, int material, const Image& diffuse, const Image* specular, Image& out,
                         VehicleLampInfo* info, bool rail, const std::string& texture) {
  VehicleLampInfo local;
  VehicleLampInfo& st = info ? *info : local;
  const LampFix* fix = lampFixFor(texture);
  std::vector<uint8_t> dif, spe;
  int w = 0, h = 0, sw = 0, sh = 0;
  if (!decodeAtMost(diffuse, 512, dif, w, h)) return false;
  bool haveSpec = specular && decodeAtMost(*specular, 512, spe, sw, sh);
  const size_t nv = mesh.vertexCount();
  if (!nv || mesh.uv0.size() < nv * 2 || mesh.normals.size() < nv * 3) return false;
  // the body at lamp height: front and rear surface (the front is −Z), from all of the mesh
  float minY = mesh.boundsMin[1], height = std::max(0.1f, mesh.boundsMax[1] - mesh.boundsMin[1]);
  float hw = 0.5f * (mesh.boundsMax[0] - mesh.boundsMin[0]), cx = 0.5f * (mesh.boundsMax[0] + mesh.boundsMin[0]);
  float zFront = 1e30f, zRear = -1e30f;
  for (size_t i = 0; i < nv; ++i) {
    float x = mesh.positions[i * 3] - cx, y = mesh.positions[i * 3 + 1], z = mesh.positions[i * 3 + 2];
    if (y < minY + 0.12f * height || y > minY + 0.6f * height || std::fabs(x) > 0.85f * hw) continue;
    zFront = std::min(zFront, z), zRear = std::max(zRear, z);
  }
  if (zFront > zRear) return false;
  const float depth = std::max(0.25f, 0.06f * (zRear - zFront));
  // every texel the material's triangles cover: which end it faces, at lamp height
  std::vector<uint8_t> side(size_t(w) * size_t(h), 0);  // 1 front, 2 rear
  // the cabin light fades towards the front: the driver's cab stays dark (0 up to 1.5 m from the front, full at 4.5 m)
  std::vector<float> texY(size_t(w) * size_t(h), -1e30f);  // model height of the face using the texel
  std::vector<float> fade(size_t(w) * size_t(h), 1.0f), fadeRear(size_t(w) * size_t(h), 1.0f);  // rail: a cab at each end
  for (const Submesh& sm : mesh.submeshes) {
    if (sm.material != material) continue;
    for (uint32_t k = sm.firstIndex; k + 2 < sm.firstIndex + sm.indexCount && k + 2 < mesh.indices.size(); k += 3) {
      uint32_t t[3] = {mesh.indices[k], mesh.indices[k + 1], mesh.indices[k + 2]};
      if (t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
      float u[3], v[3];
      float ub = std::floor(std::min({mesh.uv0[t[0] * 2], mesh.uv0[t[1] * 2], mesh.uv0[t[2] * 2]}));
      float vb = std::floor(std::min({mesh.uv0[t[0] * 2 + 1], mesh.uv0[t[1] * 2 + 1], mesh.uv0[t[2] * 2 + 1]}));
      for (int j = 0; j < 3; ++j) u[j] = (mesh.uv0[t[j] * 2] - ub) * float(w), v[j] = (mesh.uv0[t[j] * 2 + 1] - vb) * float(h);
      float area = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
      if (std::fabs(area) < 1e-6f) continue;
      int x0 = std::max(0, int(std::floor(std::min({u[0], u[1], u[2]})))), x1 = std::min(w - 1, int(std::ceil(std::max({u[0], u[1], u[2]}))));
      int y0 = std::max(0, int(std::floor(std::min({v[0], v[1], v[2]})))), y1 = std::min(h - 1, int(std::ceil(std::max({v[0], v[1], v[2]}))));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          float px = float(x) + 0.5f, py = float(y) + 0.5f;
          float b1 = ((px - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (py - v[0])) / area;
          float b2 = ((u[1] - u[0]) * (py - v[0]) - (px - u[0]) * (v[1] - v[0])) / area;
          float b0 = 1 - b1 - b2;
          if (b0 < -0.02f || b1 < -0.02f || b2 < -0.02f) continue;
          float bb[3] = {b0, b1, b2};
          float py3 = 0, pz = 0, nz = 0;
          for (int j = 0; j < 3; ++j) {
            py3 += bb[j] * mesh.positions[t[j] * 3 + 1];
            pz += bb[j] * mesh.positions[t[j] * 3 + 2];
            nz += bb[j] * mesh.normals[t[j] * 3 + 2];
          }
          texY[size_t(y) * size_t(w) + size_t(x)] = py3;
          float& fd = fade[size_t(y) * size_t(w) + size_t(x)];
          fd = std::min(fd, smooth(zFront + 1.5f, zFront + 4.5f, pz));
          float& fr = fadeRear[size_t(y) * size_t(w) + size_t(x)];
          fr = std::min(fr, smooth(zRear - 1.5f, zRear - 4.5f, pz));
          if (py3 < minY + 0.08f * height || py3 > minY + 0.58f * height) continue;
          uint8_t& sd = side[size_t(y) * size_t(w) + size_t(x)];
          if (nz < -0.45f && pz < zFront + depth) sd = 1;
          else if (nz > 0.45f && pz > zRear - depth) sd = 2;
        }
    }
  }
  // lamp glass by its look: front lamps bright, nearly white and clearly brighter than the rest of the front (or very
  // shiny), rear lamps clearly red
  std::vector<float> frontLum;
  for (size_t k = 0; k < side.size(); ++k)
    if (side[k] == 1) {
      const uint8_t* p = &dif[k * 4];
      frontLum.push_back((0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]) / 255.0f);
    }
  float median = 0;
  if (!frontLum.empty()) {
    std::nth_element(frontLum.begin(), frontLum.begin() + std::ptrdiff_t(frontLum.size() / 2), frontLum.end());
    median = frontLum[frontLum.size() / 2];
  }
  std::vector<uint8_t> lamp(size_t(w) * size_t(h), 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      size_t k = size_t(y) * size_t(w) + size_t(x);
      if (!side[k]) continue;
      const uint8_t* p = &dif[k * 4];
      float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
      float mx = std::max({r, g, b}), mn = std::min({r, g, b}), lum = 0.3f * r + 0.59f * g + 0.11f * b;
      float sat = mx > 0 ? (mx - mn) / mx : 0;
      float s = 0;
      if (haveSpec) {
        const uint8_t* q = &spe[(size_t(y * sh / h) * size_t(sw) + size_t(x * sw / w)) * 4];
        s = std::max({q[0], q[1], q[2]}) / 255.0f;
      }
      bool bright = lum > std::max(0.5f, median + 0.25f) || (s > 0.55f && lum > std::max(0.38f, median + 0.15f));
      if (side[k] == 1 && bright && sat < 0.35f) lamp[k] = 1;
      if (side[k] == 2 && r > 0.3f && r > 1.7f * g && r > 1.7f * b) lamp[k] = 2;
    }
  // lamps are compact spots: not thin lines (chrome trims), not large areas (over 3 % of the texture: a white bumper,
  // a red panel); of each kind the spots of at least a quarter of the largest one's size
  struct Spot {
    std::vector<size_t> texels;
    int kind;
  };
  std::vector<Spot> spots;
  std::vector<int> label(lamp.size(), -1);
  std::vector<size_t> stack;
  for (size_t i = 0; i < lamp.size(); ++i) {
    if (!lamp[i] || label[i] >= 0) continue;
    Spot sp;
    sp.kind = lamp[i];
    stack.assign(1, i);
    label[i] = 1;
    int bx0 = w, bx1 = -1, by0 = h, by1 = -1;
    while (!stack.empty()) {
      size_t p = stack.back();
      stack.pop_back();
      sp.texels.push_back(p);
      int x = int(p % size_t(w)), y = int(p / size_t(w));
      bx0 = std::min(bx0, x), bx1 = std::max(bx1, x), by0 = std::min(by0, y), by1 = std::max(by1, y);
      const int nb[8][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}, {x - 1, y - 1}, {x + 1, y - 1}, {x - 1, y + 1}, {x + 1, y + 1}};
      for (const auto& q : nb) {
        if (q[0] < 0 || q[1] < 0 || q[0] >= w || q[1] >= h) continue;
        size_t kk = size_t(q[1]) * size_t(w) + size_t(q[0]);
        if (lamp[kk] == lamp[i] && label[kk] < 0) label[kk] = 1, stack.push_back(kk);
      }
    }
    int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    float fill = float(sp.texels.size()) / float(bw * bh);
    if (sp.texels.size() > lamp.size() * 3 / 100 || std::max(bw, bh) > 3 * std::min(bw, bh) || fill < 0.45f) continue;
    spots.push_back(std::move(sp));
  }
  size_t largest[3] = {0, 0, 0};
  for (const Spot& sp : spots) largest[sp.kind] = std::max(largest[sp.kind], sp.texels.size());
  std::vector<uint8_t> keep(lamp.size(), 0);
  for (const Spot& sp : spots) {
    if (sp.texels.size() * 4 < largest[sp.kind]) continue;
    for (size_t p : sp.texels) keep[p] = 1;
    (sp.kind == 1 ? st.frontLamps : st.rearLamps)++;
  }
  {  // lamps are 1-3 texels: one texel more around them (on the same end's faces) so they survive minification
    std::vector<uint8_t> grow = keep;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        size_t k = size_t(y) * size_t(w) + size_t(x);
        if (!keep[k]) continue;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            size_t kk = size_t(yy) * size_t(w) + size_t(xx);
            if (!keep[kk] && side[kk] == lamp[k]) grow[kk] = 1, lamp[kk] = lamp[k];
          }
      }
    keep.swap(grow);
  }
  if (fix) {  // the known lamps of this texture instead of the front lamps found
    for (size_t k = 0; k < keep.size(); ++k)
      if (keep[k] && lamp[k] == 1) keep[k] = 0, lamp[k] = 0;
    for (const LampFix::Disc& dc : fix->discs) {
      if (dc.r <= 0) continue;
      float cx = dc.u * float(w), cy = dc.v * float(h), r = dc.r * float(w);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          if (std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy) <= r) {
            size_t k = size_t(y) * size_t(w) + size_t(x);
            keep[k] = 1, lamp[k] = 1;
          }
    }
    st.frontLamps = fix->count;
  }
  // window glass (interior light of buses, trams and trains): the specular map's bright areas, above 60 % of its
  // brightest level (the paint is shiny too, but well below the glass); window-sized areas only (no lamp lenses)
  std::vector<uint8_t> win(lamp.size(), 0);
  if (haveSpec) {
    std::vector<float> sv(lamp.size());
    std::vector<int> hist(256, 0);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const uint8_t* q = &spe[(size_t(y * sh / h) * size_t(sw) + size_t(x * sw / w)) * 4];
        uint8_t m = std::max({q[0], q[1], q[2]});
        sv[size_t(y) * size_t(w) + size_t(x)] = m / 255.0f;
        ++hist[m];
      }
    int top = 255;
    for (size_t acc = 0; top > 0 && (acc += size_t(hist[size_t(top)])) < lamp.size() / 200;) --top;
    float T = std::max(0.3f, 0.6f * float(top) / 255.0f);
    std::vector<int> lab(lamp.size(), -1);
    for (size_t i = 0; i < lamp.size(); ++i) {
      if (sv[i] <= T || keep[i] || lab[i] >= 0) continue;
      std::vector<size_t> comp;
      stack.assign(1, i);
      lab[i] = 1;
      while (!stack.empty()) {
        size_t p = stack.back();
        stack.pop_back();
        comp.push_back(p);
        int x = int(p % size_t(w)), y = int(p / size_t(w));
        const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& q : nb) {
          if (q[0] < 0 || q[1] < 0 || q[0] >= w || q[1] >= h) continue;
          size_t kk = size_t(q[1]) * size_t(w) + size_t(q[0]);
          if (sv[kk] > T && !keep[kk] && lab[kk] < 0) lab[kk] = 1, stack.push_back(kk);
        }
      }
      if (comp.size() * 1000 < lamp.size() * 15 || comp.size() * 100 > lamp.size() * 15) continue;
      // the fade only on a cab car (head lamps at its front); middle sections are lit evenly
      for (size_t p : comp) win[p] = uint8_t(std::lround(255.0f * smooth(T, T + 0.1f, sv[p]) * (st.frontLamps ? fade[p] : 1.0f) *
                                                         (rail && st.rearLamps ? fadeRear[p] : 1.0f)));
      ++st.windows;
    }
  }
  {  // the window band's height on the model (10th to 90th percentile of the glass): where the passengers show
    std::vector<float> ys;
    for (size_t i = 0; i < win.size(); ++i)
      if (win[i] > 128 && texY[i] > -1e29f) ys.push_back(texY[i]);
    if (ys.size() > 20) {
      std::sort(ys.begin(), ys.end());
      st.windowY0 = ys[ys.size() / 10], st.windowY1 = ys[ys.size() * 9 / 10];
    }
  }
  if (!st.frontLamps && !st.rearLamps && !st.windows) return false;
  std::vector<uint8_t> px(size_t(w) * size_t(h) * 4, 0);  // BGRA: R = front lamps, G = rear lamps, B = windows
  for (size_t i = 0; i < lamp.size(); ++i) {
    if (keep[i]) px[i * 4 + (lamp[i] == 1 ? 2 : 1)] = 255;
    px[i * 4] = win[i];
  }
  out = Image();
  out.format = Image::Format::BGRA8;
  out.width = w, out.height = h;
  out.levels.push_back(std::move(px));
  buildMips(out, 0b0110);  // the lamps (G rear, R front) by their brightest texel; the windows (B) averaged
  return true;
}

uint32_t vehicleLampMask(Display& d, VM& vm, const ObjectData& od, int material, VehicleLampInfo* info) {
  static std::map<std::pair<const ObjectData*, int>, std::pair<uint32_t, VehicleLampInfo>> cache;
  auto keyv = std::make_pair(&od, material);
  if (auto it = cache.find(keyv); it != cache.end()) {
    if (info) *info = it->second.second;
    return it->second.first;
  }
  VehicleLampInfo li;
  uint32_t tex = 0;
  const MaterialDef& m = od.materials[size_t(material)];
  std::vector<uint8_t> data;
  Image di, si;
  if ((m.flags & kMatDiffuseMap) && !m.textures.empty() && vm.vfs().read(m.textures[0], data) && parseDds(data, di, nullptr)) {
    size_t at = 1 + ((m.flags & 0x400) ? 1 : 0) + ((m.flags & 0x800) ? 2 : 0);
    bool haveSpec = (m.flags & kMatSpecularMap) && at < m.textures.size() && vm.vfs().read(m.textures[at], data) &&
                    parseDds(data, si, nullptr);
    Image mask;
    std::string n = od.name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    bool rail = n.find("tram") != std::string::npos || n.find("metro") != std::string::npos || n.find("train") != std::string::npos;
    if (makeVehicleLampMask(od.mesh, material, di, haveSpec ? &si : nullptr, mask, &li, rail, m.textures[0]))
      tex = d.r2d().createTexture(mask);
  }
  if (debugNight())
    LOG_INFO("night: vehicle lamps %s / %s: %d front, %d rear, %d windows", od.name.c_str(), m.name.c_str(), li.frontLamps,
             li.rearLamps, li.windows);
  cache.emplace(keyv, std::make_pair(tex, li));
  if (info) *info = li;
  return tex;
}

VehicleLampInfo vehicleLamps(Display& d, VM& vm, const ObjectData& od) {
  VehicleLampInfo all;
  for (size_t i = 0; i < od.materials.size(); ++i) {
    VehicleLampInfo li;
    vehicleLampMask(d, vm, od, int(i), &li);
    all.frontLamps += li.frontLamps, all.rearLamps += li.rearLamps;
  }
  return all;
}

float vehicleLightsOn(uint32_t seed, float night01) {
  float onAt = 0.12f + 0.43f * hash01(seed);
  return smooth(onAt, onAt + 0.03f, night01);
}

bool& vehicleCycleOn() {
  static bool on = false;
  return on;
}

float& vehicleNight01() {
  static float n = 0;
  return n;
}

// ---- street-lamp glass ----

bool makeLampGlassMask(const MeshData& mesh, int material, const std::vector<Vec3>& heads, const Image& diffuse,
                       const Image* specular, Image& out, int* texels) {
  std::vector<uint8_t> dif, spe;
  int w = 0, h = 0, sw = 0, sh = 0;
  if (heads.empty() || !decodeAtMost(diffuse, 512, dif, w, h)) return false;
  bool haveSpec = specular && decodeAtMost(*specular, 512, spe, sw, sh);
  const size_t nv = mesh.vertexCount();
  if (!nv || mesh.uv0.size() < nv * 2 || mesh.normals.size() < nv * 3) return false;
  // texels of the head: within 0.8 m (horizontally) of a light point, from 1.5 m below it (hanging lanterns) to 1 m
  // above; 1 = its underside (faces pointing down), 2 = the rest of the head
  std::vector<uint8_t> part(size_t(w) * size_t(h), 0);
  for (const Submesh& sm : mesh.submeshes) {
    if (sm.material != material) continue;
    for (uint32_t k = sm.firstIndex; k + 2 < sm.firstIndex + sm.indexCount && k + 2 < mesh.indices.size(); k += 3) {
      uint32_t t[3] = {mesh.indices[k], mesh.indices[k + 1], mesh.indices[k + 2]};
      if (t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
      float u[3], v[3];
      float ub = std::floor(std::min({mesh.uv0[t[0] * 2], mesh.uv0[t[1] * 2], mesh.uv0[t[2] * 2]}));
      float vb = std::floor(std::min({mesh.uv0[t[0] * 2 + 1], mesh.uv0[t[1] * 2 + 1], mesh.uv0[t[2] * 2 + 1]}));
      for (int j = 0; j < 3; ++j) u[j] = (mesh.uv0[t[j] * 2] - ub) * float(w), v[j] = (mesh.uv0[t[j] * 2 + 1] - vb) * float(h);
      float area = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
      if (std::fabs(area) < 1e-6f) continue;
      int x0 = std::max(0, int(std::floor(std::min({u[0], u[1], u[2]})))), x1 = std::min(w - 1, int(std::ceil(std::max({u[0], u[1], u[2]}))));
      int y0 = std::max(0, int(std::floor(std::min({v[0], v[1], v[2]})))), y1 = std::min(h - 1, int(std::ceil(std::max({v[0], v[1], v[2]}))));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          float px = float(x) + 0.5f, py = float(y) + 0.5f;
          float b1 = ((px - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (py - v[0])) / area;
          float b2 = ((u[1] - u[0]) * (py - v[0]) - (px - u[0]) * (v[1] - v[0])) / area;
          float b0 = 1 - b1 - b2;
          if (b0 < -0.02f || b1 < -0.02f || b2 < -0.02f) continue;
          float bb[3] = {b0, b1, b2};
          Vec3 p;
          float ny = 0;
          for (int j = 0; j < 3; ++j) {
            p = p + Vec3{mesh.positions[t[j] * 3], mesh.positions[t[j] * 3 + 1], mesh.positions[t[j] * 3 + 2]} * bb[j];
            ny += bb[j] * mesh.normals[t[j] * 3 + 1];
          }
          bool near = false;
          for (const Vec3& hd : heads) {
            float dx = p.x - hd.x, dz = p.z - hd.z;
            near = near || (dx * dx + dz * dz < 0.64f && p.y > hd.y - 1.5f && p.y < hd.y + 1.0f);
          }
          if (!near) continue;
          uint8_t& pt = part[size_t(y) * size_t(w) + size_t(x)];
          pt = std::max<uint8_t>(pt, ny < -0.6f ? 1 : 2);
        }
    }
  }
  // the glass: the underside, and on the rest of the head what looks like glass (light and unsaturated, clearly
  // lighter than the head's housing, or shiny in the specular map)
  std::vector<float> lums;
  for (size_t k = 0; k < part.size(); ++k)
    if (part[k] == 2) lums.push_back((0.3f * dif[k * 4] + 0.59f * dif[k * 4 + 1] + 0.11f * dif[k * 4 + 2]) / 255.0f);
  float median = 0;
  if (!lums.empty()) {
    std::nth_element(lums.begin(), lums.begin() + std::ptrdiff_t(lums.size() / 2), lums.end());
    median = lums[lums.size() / 2];
  }
  std::vector<uint8_t> px(size_t(w) * size_t(h) * 4, 0);
  int count = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      size_t k = size_t(y) * size_t(w) + size_t(x);
      if (!part[k]) continue;
      const uint8_t* p = &dif[k * 4];
      float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
      float mx = std::max({r, g, b}), mn = std::min({r, g, b}), lum = 0.3f * r + 0.59f * g + 0.11f * b;
      float sat = mx > 0 ? (mx - mn) / mx : 0;
      float s = 0;
      if (haveSpec) {
        const uint8_t* q = &spe[(size_t(y * sh / h) * size_t(sw) + size_t(x * sw / w)) * 4];
        s = std::max({q[0], q[1], q[2]}) / 255.0f;
      }
      bool glass = part[k] == 1 || (sat < 0.45f && (lum > std::max(0.4f, median + 0.15f) || s > 0.5f));
      if (!glass) continue;
      px[k * 4 + 2] = 255;
      ++count;
    }
  if (texels) *texels = count;
  if (!count) return false;
  out = Image();
  out.format = Image::Format::BGRA8;
  out.width = w, out.height = h;
  out.levels.push_back(std::move(px));
  buildMips(out);
  return true;
}

uint32_t lampGlassMask(Display& d, VM& vm, const ObjectData& od, int material, const std::string& objectPath) {
  if (!isLampTemplate(objectPath) || ownGlow(od)) return 0;
  static std::map<std::pair<const ObjectData*, int>, uint32_t> cache;
  auto keyv = std::make_pair(&od, material);
  if (auto it = cache.find(keyv); it != cache.end()) return it->second;
  uint32_t tex = 0;
  int count = 0;
  const MaterialDef& m = od.materials[size_t(material)];
  std::vector<uint8_t> data;
  Image di, si, mask;
  if ((m.flags & kMatDiffuseMap) && !m.textures.empty() && vm.vfs().read(m.textures[0], data) && parseDds(data, di, nullptr)) {
    size_t at = 1 + ((m.flags & 0x400) ? 1 : 0) + ((m.flags & 0x800) ? 2 : 0);
    bool haveSpec = (m.flags & kMatSpecularMap) && at < m.textures.size() && vm.vfs().read(m.textures[at], data) &&
                    parseDds(data, si, nullptr);
    if (makeLampGlassMask(od.mesh, material, headsOf(od, objectPath), di, haveSpec ? &si : nullptr, mask, &count))
      tex = d.r2d().createTexture(mask);
  }
  if (debugNight()) LOG_INFO("night: lamp glass %s / %s: %d texels", objectPath.c_str(), m.name.c_str(), count);
  cache.emplace(keyv, tex);
  return tex;
}

// ---- lit signs and advertisements ----

bool isSignTemplate(const std::string& objectPath) {
  std::string p = objectPath;
  std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  std::string stem = p.substr(p.find_last_of('/') + 1);
  if (p.find("/ad-props/") != std::string::npos) return stem.find("graffiti") == std::string::npos;
  for (const char* k : {"wallsign", "casinosign", "beergardensign", "marketsign", "grocerysign"})
    if (stem.find(k) != std::string::npos) return true;
  return false;
}

bool makeSignMask(const MeshData& mesh, int material, int w, int h, Image& out, int* texels) {
  const size_t nv = mesh.vertexCount();
  if (!nv || mesh.uv0.size() < nv * 2) return false;
  // the sign's face: the direction (and its opposite) holding the most triangle area, over the whole model
  auto P = [&](uint32_t i) { return Vec3{mesh.positions[i * 3], mesh.positions[i * 3 + 1], mesh.positions[i * 3 + 2]}; };
  struct Tri {
    uint32_t t[3];
    Vec3 n;
    float area;
    int material;
  };
  std::vector<Tri> tris;
  for (const Submesh& sm : mesh.submeshes)
    for (uint32_t k = sm.firstIndex; k + 2 < sm.firstIndex + sm.indexCount && k + 2 < mesh.indices.size(); k += 3) {
      Tri tr{{mesh.indices[k], mesh.indices[k + 1], mesh.indices[k + 2]}, {}, 0, sm.material};
      if (tr.t[0] >= nv || tr.t[1] >= nv || tr.t[2] >= nv) continue;
      Vec3 c = cross(P(tr.t[1]) - P(tr.t[0]), P(tr.t[2]) - P(tr.t[0]));
      float l = std::sqrt(dot(c, c));
      if (l < 1e-8f) continue;
      tr.n = c * (1.0f / l), tr.area = 0.5f * l;
      tris.push_back(tr);
    }
  Vec3 best;
  float bestArea = 0;
  for (const Tri& a : tris) {
    float sum = 0;
    for (const Tri& b : tris) sum += std::fabs(dot(a.n, b.n)) > 0.9f ? b.area : 0.0f;
    if (sum > bestArea) bestArea = sum, best = a.n;
  }
  if (bestArea <= 0) return false;
  std::vector<uint8_t> px(size_t(w) * size_t(h) * 4, 0);
  int count = 0;
  for (const Tri& tr : tris) {
    // the panel: faces along the main direction and not tiny (frame edges, legs)
    if (tr.material != material || std::fabs(dot(tr.n, best)) < 0.9f || tr.area < 0.04f * bestArea) continue;
    float u[3], v[3];
    float ub = std::floor(std::min({mesh.uv0[tr.t[0] * 2], mesh.uv0[tr.t[1] * 2], mesh.uv0[tr.t[2] * 2]}));
    float vb = std::floor(std::min({mesh.uv0[tr.t[0] * 2 + 1], mesh.uv0[tr.t[1] * 2 + 1], mesh.uv0[tr.t[2] * 2 + 1]}));
    for (int j = 0; j < 3; ++j) u[j] = (mesh.uv0[tr.t[j] * 2] - ub) * float(w), v[j] = (mesh.uv0[tr.t[j] * 2 + 1] - vb) * float(h);
    float area = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
    if (std::fabs(area) < 1e-6f) continue;
    int x0 = std::max(0, int(std::floor(std::min({u[0], u[1], u[2]})))), x1 = std::min(w - 1, int(std::ceil(std::max({u[0], u[1], u[2]}))));
    int y0 = std::max(0, int(std::floor(std::min({v[0], v[1], v[2]})))), y1 = std::min(h - 1, int(std::ceil(std::max({v[0], v[1], v[2]}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        float px0 = float(x) + 0.5f, py0 = float(y) + 0.5f;
        float b1 = ((px0 - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (py0 - v[0])) / area;
        float b2 = ((u[1] - u[0]) * (py0 - v[0]) - (px0 - u[0]) * (v[1] - v[0])) / area;
        if (b1 < -0.01f || b2 < -0.01f || 1 - b1 - b2 < -0.01f) continue;
        uint8_t& o = px[(size_t(y) * size_t(w) + size_t(x)) * 4 + 2];
        if (!o) o = 255, ++count;
      }
  }
  if (texels) *texels = count;
  if (!count) return false;
  out = Image();
  out.format = Image::Format::BGRA8;
  out.width = w, out.height = h;
  out.levels.push_back(std::move(px));
  buildMips(out);
  return true;
}

uint32_t signMask(Display& d, VM& vm, const ObjectData& od, int material, const std::string& objectPath) {
  if (!isSignTemplate(objectPath)) return 0;
  const MaterialDef& m = od.materials[size_t(material)];
  if (!(m.flags & kMatDiffuseMap) || (m.flags & kMatIlluminationMap) || m.textures.empty()) return 0;  // lit already
  if (m.name.find("painted") != std::string::npos) return 0;  // ads painted on walls are not lit
  static std::map<std::pair<const ObjectData*, int>, uint32_t> cache;
  auto keyv = std::make_pair(&od, material);
  if (auto it = cache.find(keyv); it != cache.end()) return it->second;
  uint32_t tex = 0;
  int count = 0;
  std::vector<uint8_t> data;
  Image di, mask;
  if (vm.vfs().read(m.textures[0], data) && parseDds(data, di, nullptr)) {
    int w = di.width, h = di.height;
    for (int l = 0; l + 1 < di.mipCount && std::max(w, h) > 512; ++l) w = std::max(1, w / 2), h = std::max(1, h / 2);
    if (makeSignMask(od.mesh, material, w, h, mask, &count)) tex = d.r2d().createTexture(mask);
  }
  if (debugNight()) LOG_INFO("night: sign %s / %s: %d texels", objectPath.c_str(), m.name.c_str(), count);
  cache.emplace(keyv, tex);
  return tex;
}

// ---- market tents and clock faces ----

int propGlowKind(const std::string& objectPath) {
  std::string p = objectPath;
  std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  std::string stem = p.substr(p.find_last_of('/') + 1);
  if (stem.find("clock") != std::string::npos) return 5;
  return 0;
}

bool makePropGlowMask(const MeshData& mesh, int material, int kind, const Image& diffuse, Image& out, int* texels) {
  std::vector<uint8_t> dif;
  int w = 0, h = 0;
  if (!decodeAtMost(diffuse, 512, dif, w, h)) return false;
  const size_t nv = mesh.vertexCount();
  if (!nv || mesh.uv0.size() < nv * 2) return false;
  std::vector<uint8_t> px(size_t(w) * size_t(h) * 4, 0);
  int count = 0;
  for (const Submesh& sm : mesh.submeshes) {
    if (sm.material != material) continue;
    for (uint32_t k = sm.firstIndex; k + 2 < sm.firstIndex + sm.indexCount && k + 2 < mesh.indices.size(); k += 3) {
      uint32_t t[3] = {mesh.indices[k], mesh.indices[k + 1], mesh.indices[k + 2]};
      if (t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
      float u[3], v[3];
      float ub = std::floor(std::min({mesh.uv0[t[0] * 2], mesh.uv0[t[1] * 2], mesh.uv0[t[2] * 2]}));
      float vb = std::floor(std::min({mesh.uv0[t[0] * 2 + 1], mesh.uv0[t[1] * 2 + 1], mesh.uv0[t[2] * 2 + 1]}));
      for (int j = 0; j < 3; ++j) u[j] = (mesh.uv0[t[j] * 2] - ub) * float(w), v[j] = (mesh.uv0[t[j] * 2 + 1] - vb) * float(h);
      float area = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
      if (std::fabs(area) < 1e-6f) continue;
      int x0 = std::max(0, int(std::floor(std::min({u[0], u[1], u[2]})))), x1 = std::min(w - 1, int(std::ceil(std::max({u[0], u[1], u[2]}))));
      int y0 = std::max(0, int(std::floor(std::min({v[0], v[1], v[2]})))), y1 = std::min(h - 1, int(std::ceil(std::max({v[0], v[1], v[2]}))));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          float qx = float(x) + 0.5f, qy = float(y) + 0.5f;
          float b1 = ((qx - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (qy - v[0])) / area;
          float b2 = ((u[1] - u[0]) * (qy - v[0]) - (qx - u[0]) * (v[1] - v[0])) / area;
          if (b1 < -0.01f || b2 < -0.01f || 1 - b1 - b2 < -0.01f) continue;
          size_t i = size_t(y) * size_t(w) + size_t(x);
          if (kind == 5) {  // the clock: its dial, light and unsaturated
            const uint8_t* p = &dif[i * 4];
            float mx = std::max({p[0], p[1], p[2]}) / 255.0f, mn = std::min({p[0], p[1], p[2]}) / 255.0f;
            float lum = (0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]) / 255.0f;
            if (lum < 0.55f || (mx > 0 && (mx - mn) / mx > 0.25f)) continue;
          }
          if (!px[i * 4 + 2]) px[i * 4 + 2] = 255, ++count;
        }
    }
  }
  if (texels) *texels = count;
  if (!count) return false;
  out = Image();
  out.format = Image::Format::BGRA8;
  out.width = w, out.height = h;
  out.levels.push_back(std::move(px));
  buildMips(out);
  return true;
}

uint32_t propGlowMask(Display& d, VM& vm, const ObjectData& od, int material, const std::string& objectPath, int& kind) {
  kind = propGlowKind(objectPath);
  if (!kind) return 0;
  const MaterialDef& m = od.materials[size_t(material)];
  if (!(m.flags & kMatDiffuseMap) || m.textures.empty()) return 0;
  static std::map<std::pair<const ObjectData*, int>, uint32_t> cache;
  auto keyv = std::make_pair(&od, material);
  if (auto it = cache.find(keyv); it != cache.end()) return it->second;
  uint32_t tex = 0;
  int count = 0;
  std::vector<uint8_t> data;
  Image di, mask;
  if (vm.vfs().read(m.textures[0], data) && parseDds(data, di, nullptr) && makePropGlowMask(od.mesh, material, kind, di, mask, &count))
    tex = d.r2d().createTexture(mask);
  if (debugNight()) LOG_INFO("night: glow %s / %s (kind %d): %d texels", objectPath.c_str(), m.name.c_str(), kind, count);
  cache.emplace(keyv, tex);
  return tex;
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
                    ParticleDrawList& particles, std::vector<DrawItem>& items) {
  NightState ns = skyNightState(sky);
  vehicleNight01() = 0;
  vehicleCycleOn() = false;
  // the passengers in lit cabins (by day too, and without the cycle): the game's citizens as silhouettes
  v.passengerAtlas = passengerAtlasTexture(vm.vfs(), v.passengerInfo);
  if (!ns.on) return;
  float windows = smooth(0.18f, 0.72f, ns.night01);
  float lamps = smooth(0.32f, 0.62f, ns.night01);
  v.night[0] = windows;
  v.night[1] = windowShare(float(ns.hours));
  v.night[2] = 0;
  v.night[3] = ns.night01;
  v.shopShare = shopShare(float(ns.hours));
  GridObj* grid = nullptr;
  for (GridObj* g : grids)
    if (g->data && g->terrain && !g->data->items.empty()) grid = g;
  if (!grid) return;
  LampCache& lc = lampCaches()[grid];  // built while the cycle is on, so dusk brings no hitch
  if (lc.generation != grid->generation || lc.itemsVersion != grid->itemsVersion || !lc.texture)
    updateLampMap(d, grid, lc);  // (it takes the generation and version)
  const TerrainCache& tc = *grid->terrain;
  if (tc.viewMode == 0x180) return;  // the underground view: no lamps, no buildings, no vehicle lights
  Mat4 vp = v.proj * v.view;
  auto inView = [&](const Vec3& p, float margin) {
    float cw = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
    if (cw <= 0.1f) return false;
    float cx = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
    float cy = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
    return std::fabs(cx) <= margin * cw && std::fabs(cy) <= margin * cw;
  };
  GpuMaterial* mat = glowMaterial(d);
  uint32_t batch = uint32_t(particles.batches.size());
  particles.batches.push_back({mat, 0x100u});
  std::vector<ParticleSprite> glows;
  if (lamps > 0) {
    v.night[2] = lamps * kLampGain;
    v.lampMap = lc.texture;
    std::copy(lc.xf, lc.xf + 4, v.lampMapXf);
    v.lampHeight[0] = lc.y0;
    v.lampHeight[1] = lc.yScale;
    v.lampHeight[2] = float(lc.w) / float(kLightMapSide);  // the lamp map's share of the light map's upper half
    v.lampHeight[3] = float(lc.h) / float(2 * kLightMapSide);
    // no sprites on the heads: the lamps' own glass glows (lampGlassMask, the shader's window path)
  }
  vehicleNight01() = ns.night01;
  vehicleCycleOn() = true;
  if (ns.night01 > 0.08f) addVehicleLights(d, vm, grid, lc, v, ns.night01, float(ns.hours), batch, glows);
  // additive: order does not matter among them; drawn before the scene's own particles (smoke in front stays in front)
  particles.sprites.insert(particles.sprites.begin(), glows.begin(), glows.end());
}

}  // namespace regs::engine
