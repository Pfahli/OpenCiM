// Working traffic lights: see traffic_lights.h.
#include "engine/traffic_lights.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <string>

#include "core/log.h"
#include "engine/display.h"
#include "vfs/vfs.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/grid_render.h"
#include "engine/night_lights.h"
#include "engine/scene3d.h"
#include "render/image.h"
#include "render/math3d.h"
#include "render/mesh.h"
#include "render/renderer2d.h"

using namespace regs::render;

namespace regs::engine {

namespace {

// the lamp sprite: a white disc with a soft edge and a faint halo, mipmapped; additive particle material (as the night
// lights' glow, engine/night_lights.cpp)
GpuMaterial* lampMaterial(Display& d) {
  static std::unique_ptr<GpuMaterial> m;
  if (m) return m.get();
  m = std::make_unique<GpuMaterial>();
  Image img;
  img.format = Image::Format::BGRA8;
  img.width = img.height = 64;
  std::vector<uint8_t> px(64 * 64 * 4);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      const float dx = (float(x) + 0.5f) / 32.0f - 1.0f, dy = (float(y) + 0.5f) / 32.0f - 1.0f, r2 = dx * dx + dy * dy;
      const float a = r2 >= 1 ? 0.0f : (0.95f * std::exp(-r2 * 9.0f) + 0.25f * std::exp(-r2 * 2.5f)) * (1 - r2) * (1 - r2);
      uint8_t* p = &px[(size_t(y) * 64 + size_t(x)) * 4];
      p[0] = p[1] = p[2] = 255;
      p[3] = uint8_t(std::lround(255.0f * std::min(1.0f, a)));
    }
  img.levels.push_back(std::move(px));
  for (int s = 32; s >= 1; s /= 2) {  // (mipmaps: 2 × 2 box)
    const std::vector<uint8_t>& src = img.levels.back();
    const int ss = s * 2;
    std::vector<uint8_t> dst(size_t(s) * size_t(s) * 4);
    for (int y = 0; y < s; ++y)
      for (int x = 0; x < s; ++x)
        for (int c = 0; c < 4; ++c) {
          int sum = 0;
          for (int k = 0; k < 4; ++k) sum += src[(size_t(y * 2 + k / 2) * size_t(ss) + size_t(x * 2 + k % 2)) * 4 + size_t(c)];
          dst[(size_t(y) * size_t(s) + size_t(x)) * 4 + size_t(c)] = uint8_t((sum + 2) / 4);
        }
    img.levels.push_back(std::move(dst));
  }
  m->flags = 0x4300u | kMatTwoSided;  // DIFFUSEMAP | POINT | ADDITIVE_A
  m->shader = "shaders/particles/particles.shader";
  for (int k = 0; k < 4; ++k) m->ambient[k] = m->diffuse[k] = 0, m->emission[k] = 1, m->specular[k] = 0;
  m->ambient[3] = m->diffuse[3] = m->specular[3] = 1;
  m->textures.assign(1, d.r2d().createTexture(img));
  m->cube.assign(1, 0);
  return m.get();
}

constexpr float kCycle = 40.0f, kGreen = 16.0f, kYellow = 3.0f, kRedYellow = 1.0f;
constexpr float kWaveSpeed = 12.0f;  // units per second
// the main head's three lamps in model space (red at the top), on its lenses (traffic-lights01: they look along the
// model's +x; placed by eye in the game)
const Vec3 kLamp[3] = {{0.43f, 4.19f, -0.16f}, {0.43f, 3.82f, -0.16f}, {0.43f, 3.45f, -0.16f}};

// the lamps of a traffic-light model from its texture (owner 2026-11-16: "check the textures where it's red, green
// and yellow and make that light up"): on the faces of the head that look along the model's +x (the lenses), the
// texels that are clearly red, amber or green, each colour's centre (their 3D points by the triangle's barycentric
// coordinates) and size (from their spread). Falls back to kLamp when a colour is not found.
struct Lenses {
  Vec3 at[3];
  float radius[3] = {0.14f, 0.14f, 0.14f};
  bool found = false;
};
int lensColour(const uint8_t* p) {  // 0 red, 1 amber, 2 green, -1 none
  const int r = p[0], g = p[1], b = p[2];
  if (r > 130 && g < r * 0.5f && b < r * 0.5f) return 0;
  if (r > 140 && g >= r * 0.55f && g < r * 0.95f && b < r * 0.45f) return 1;
  if (g > 100 && g > r * 1.3f && b < g * 1.1f) return 2;
  return -1;
}
const Lenses& lensesOf(script::VM& vm, const ObjectData& od) {
  static std::map<const ObjectData*, Lenses> cache;
  if (auto it = cache.find(&od); it != cache.end()) return it->second;
  Lenses L;
  for (int m = 0; m < 3; ++m) L.at[m] = kLamp[m];
  double sum[3][3] = {}, sq[3] = {}, n[3] = {};
  const MeshData& mesh = od.mesh;
  const size_t nv = mesh.vertexCount();
  for (const Submesh& sm : mesh.submeshes) {
    if (sm.material < 0 || size_t(sm.material) >= od.materials.size()) continue;
    const MaterialDef& mat = od.materials[size_t(sm.material)];
    if (mat.textures.empty() || nv == 0 || mesh.uv0.size() < nv * 2 || mesh.normals.size() < nv * 3) continue;
    std::vector<uint8_t> file, rgba;
    Image img;
    int w = 0, h = 0;
    if (!vm.vfs().read(mat.textures[0], file) || !parseDds(file, img, nullptr)) continue;
    int level = 0;
    while (level + 1 < img.mipCount && std::max(img.width >> level, img.height >> level) > 1024) ++level;
    if (!decodeRgba(img, level, rgba, w, h)) continue;
    for (uint32_t k = sm.firstIndex; k + 2 < sm.firstIndex + sm.indexCount && k + 2 < mesh.indices.size(); k += 3) {
      const uint32_t t[3] = {mesh.indices[k], mesh.indices[k + 1], mesh.indices[k + 2]};
      if (t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
      float nx = 0, py = 0;
      for (uint32_t q : t) nx += mesh.normals[q * 3], py += mesh.positions[q * 3 + 1];
      if (nx / 3 < 0.6f || py / 3 < 2.5f) continue;  // (the head's lens side)
      float u[3], v[3];
      const float ub = std::floor(std::min({mesh.uv0[t[0] * 2], mesh.uv0[t[1] * 2], mesh.uv0[t[2] * 2]}));
      const float vb = std::floor(std::min({mesh.uv0[t[0] * 2 + 1], mesh.uv0[t[1] * 2 + 1], mesh.uv0[t[2] * 2 + 1]}));
      for (int j = 0; j < 3; ++j) u[j] = (mesh.uv0[t[j] * 2] - ub) * float(w), v[j] = (mesh.uv0[t[j] * 2 + 1] - vb) * float(h);
      const float area = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
      if (std::fabs(area) < 1e-6f) continue;
      const int x0 = std::max(0, int(std::floor(std::min({u[0], u[1], u[2]})))), x1 = std::min(w - 1, int(std::ceil(std::max({u[0], u[1], u[2]}))));
      const int y0 = std::max(0, int(std::floor(std::min({v[0], v[1], v[2]})))), y1 = std::min(h - 1, int(std::ceil(std::max({v[0], v[1], v[2]}))));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const float qx = float(x) + 0.5f, qy = float(y) + 0.5f;
          const float b1 = ((qx - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (qy - v[0])) / area;
          const float b2 = ((u[1] - u[0]) * (qy - v[0]) - (qx - u[0]) * (v[1] - v[0])) / area;
          if (b1 < 0 || b2 < 0 || 1 - b1 - b2 < 0) continue;
          const int c = lensColour(&rgba[(size_t(y) * size_t(w) + size_t(x)) * 4]);
          if (c < 0) continue;
          const float b0 = 1 - b1 - b2;
          double p[3];
          for (int a = 0; a < 3; ++a)
            p[a] = b0 * mesh.positions[t[0] * 3 + a] + b1 * mesh.positions[t[1] * 3 + a] + b2 * mesh.positions[t[2] * 3 + a];
          for (int a = 0; a < 3; ++a) sum[c][a] += p[a];
          sq[c] += p[1] * p[1] + p[2] * p[2];
          n[c] += 1;
        }
    }
  }
  int got = 0;
  for (int c = 0; c < 3; ++c) {
    if (n[c] < 4) continue;
    const double cx = sum[c][0] / n[c], cy = sum[c][1] / n[c], cz = sum[c][2] / n[c];
    L.at[c] = Vec3{float(cx) + 0.03f, float(cy), float(cz)};  // (a hair in front of the lens)
    const double var = sq[c] / n[c] - (cy * cy + cz * cz);
    L.radius[c] = std::clamp(float(std::sqrt(std::max(0.0, 2.0 * var))), 0.08f, 0.3f);
    ++got;
  }
  L.found = got == 3;
  if (!L.found)
    for (int m = 0; m < 3; ++m) L.at[m] = kLamp[m], L.radius[m] = 0.14f;
  LOG_INFO("traffic lights: %s: lamps %s (red %.2f %.2f %.2f r %.2f, amber %.2f %.2f %.2f, green %.2f %.2f %.2f; %.0f/%.0f/%.0f texels)",
           od.name.c_str(), L.found ? "from the texture" : "by default", L.at[0].x, L.at[0].y, L.at[0].z, L.radius[0], L.at[1].x,
           L.at[1].y, L.at[1].z, L.at[2].x, L.at[2].y, L.at[2].z, n[0], n[1], n[2]);
  return cache.emplace(&od, L).first->second;
}

struct Light {
  Vec3 lamp[3];
  float radius[3] = {0.14f, 0.14f, 0.14f};
  Vec3 face;         // where the lenses look (flat, unit)
  Vec3 base;         // the pole's foot
  float offset = 0;  // seconds into the cycle at time 0
};
// a traffic light item's pole: placement and facing
struct Raw {
  Vec3 pos, face;
  Mat4 world;
  const ObjectData* od = nullptr;
  uint32_t item = 0;
};
struct Cache {
  uint64_t generation = ~0ull, itemsVersion = ~0ull;
  bool incomplete = false;  // a light's model not loaded yet (grid templates load on first use): again next frame
  std::vector<Raw> raw;     // the light items in item order (live edits update it)
  std::vector<Light> lights;
};
std::map<const GridObj*, Cache>& caches() {
  static std::map<const GridObj*, Cache> m;
  return m;
}

bool lightTemplate(const GridTemplate& tp) { return tp.object && tp.object->pendingPath.find("traffic-lights") != std::string::npos; }

// item i as a traffic light (isLight: per template; null: looked up)
void rawOf(const GridData& g, const TerrainCache& tc, uint32_t i, const std::vector<uint8_t>* isLight, std::vector<Raw>& out) {
  const GridData::Item& it = g.items[i];
  if (it.removed || it.type >= tc.typeTemplate.size()) return;
  const int ti = tc.typeTemplate[it.type];
  if (ti < 0 || !(isLight ? (*isLight)[size_t(ti)] != 0 : lightTemplate(tc.templates[size_t(ti)]))) return;
  const int div = itemDivision(g, tc, i);
  if (div < 0) return;
  Raw r;
  r.od = tc.templates[size_t(ti)].object->data.get();
  r.world = tc.divisions[size_t(div)].world * itemPlacement(g, tc, i, div);
  r.pos = transformPoint(r.world, Vec3{0, 0, 0});
  const Vec3 f = transformPoint(r.world, Vec3{1, 0, 0});  // (the lenses look along the model's +x)
  r.face = Vec3{f.x - r.pos.x, 0, f.z - r.pos.z};
  r.item = i;
  out.push_back(r);
}

void collect(script::VM& vm, GridObj* grid, Cache& c) {
  c.lights.clear();
  const GridData& g = *grid->data;
  const TerrainCache& tc = *grid->terrain;
  // live edits (a tool preview on every mouse move): only the touched items' poles; the junctions are worked out
  // again from all poles below (few)
  static std::vector<uint32_t> touched;
  if (!c.incomplete && c.generation == grid->generation && renderItemsTouchedSince(grid, c.itemsVersion, touched)) {
    std::vector<Raw> fresh;
    for (uint32_t i : touched)
      if (i < g.items.size()) rawOf(g, tc, i, nullptr, fresh);
    gridMergeTouched(c.raw, touched, fresh, [](Raw&, Raw*) {});
    if (gridDebugItemCaches()) {  // the full walk below, compared
      std::vector<Raw> all;
      for (uint32_t i = 0; i < g.items.size(); ++i) rawOf(g, tc, i, nullptr, all);
      bool same = all.size() == c.raw.size();
      for (size_t k = 0; same && k < all.size(); ++k)
        same = all[k].item == c.raw[k].item && all[k].pos.x == c.raw[k].pos.x && all[k].pos.z == c.raw[k].pos.z &&
               all[k].face.x == c.raw[k].face.x && all[k].face.z == c.raw[k].face.z;
      gridItemCacheChecked("traffic lights", same);
    }
  } else {
    std::vector<uint8_t> isLight(tc.templates.size(), 0);
    for (size_t t = 0; t < tc.templates.size(); ++t) isLight[t] = lightTemplate(tc.templates[t]);
    c.raw.clear();
    for (uint32_t i = 0; i < g.items.size(); ++i) rawOf(g, tc, i, &isLight, c.raw);
  }
  c.generation = grid->generation;
  c.itemsVersion = grid->itemsVersion;
  c.incomplete = false;
  const std::vector<Raw>& raw = c.raw;
  // junctions: the lights within 30 units of a light already in one (by 32-unit buckets)
  std::vector<int> junction(raw.size(), -1);
  std::map<std::pair<int, int>, std::vector<size_t>> bucket;
  for (size_t k = 0; k < raw.size(); ++k) bucket[{int(std::floor(raw[k].pos.x / 32)), int(std::floor(raw[k].pos.z / 32))}].push_back(k);
  int nj = 0;
  std::vector<Vec3> axis;
  std::vector<size_t> firstLight;
  for (size_t k = 0; k < raw.size(); ++k) {
    if (junction[k] >= 0) continue;
    const int id = nj++;
    axis.push_back(raw[k].face);
    firstLight.push_back(k);
    std::vector<size_t> stack{k};
    junction[k] = id;
    while (!stack.empty()) {
      const size_t q = stack.back();
      stack.pop_back();
      const int bx = int(std::floor(raw[q].pos.x / 32)), bz = int(std::floor(raw[q].pos.z / 32));
      for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
          auto it = bucket.find({bx + dx, bz + dz});
          if (it == bucket.end()) continue;
          for (size_t o : it->second)
            if (junction[o] < 0 && std::hypot(raw[o].pos.x - raw[q].pos.x, raw[o].pos.z - raw[q].pos.z) < 30.0f) {
              junction[o] = id;
              stack.push_back(o);
            }
        }
    }
  }
  for (size_t k = 0; k < raw.size(); ++k) {
    const Vec3 ax = normalize(axis[size_t(junction[k])]);
    const Vec3 f = normalize(raw[k].face);
    const bool along = std::fabs(f.x * ax.x + f.z * ax.z) > 0.7071f;
    Light l;
    static const Lenses kDefault = [] {
      Lenses d;
      for (int m = 0; m < 3; ++m) d.at[m] = kLamp[m];
      return d;
    }();
    const Lenses& lens = raw[k].od ? lensesOf(vm, *raw[k].od) : kDefault;
    for (int m = 0; m < 3; ++m) l.lamp[m] = transformPoint(raw[k].world, lens.at[m]), l.radius[m] = lens.radius[m];
    l.face = f;
    l.base = raw[k].pos;
    // the wave: the junction's place along its axis (the first light's position stands for it)
    const Raw& first = raw[firstLight[size_t(junction[k])]];
    const float wave = (first.pos.x * ax.x + first.pos.z * ax.z) / kWaveSpeed;
    l.offset = std::fmod(-wave + (along ? 0.0f : kCycle * 0.5f), kCycle);
    if (l.offset < 0) l.offset += kCycle;
    c.lights.push_back(l);
    if (!raw[k].od) c.incomplete = true;
  }
}

}  // namespace

void addTrafficLights(Display& d, script::VM& vm, const std::vector<GridObj*>& grids, const SceneView& v, ParticleDrawList& particles) {
  std::vector<ExtraNightLight>& pools = extraNightLights();
  pools.clear();
  GridObj* grid = nullptr;
  for (GridObj* g : grids)
    if (g->data && g->terrain && !g->data->items.empty()) grid = g;
  if (!grid || grid->terrain->viewMode == 0x180) return;  // (the underground view: none)
  Cache& c = caches()[grid];
  if (c.generation != grid->generation || c.itemsVersion != grid->itemsVersion || c.incomplete)
    collect(vm, grid, c);  // (it takes the generation and version)
  if (c.lights.empty()) return;
  static const auto start = std::chrono::steady_clock::now();
  const float now = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
  const float night = vehicleNight01();
  const Mat4 vp = v.proj * v.view;
  auto inView = [&](const Vec3& p) {
    const float cw = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
    if (cw <= 0.1f || cw > 900.0f) return false;  // (too far: a dot under a pixel)
    const float cx = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
    const float cy = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
    return std::fabs(cx) <= 1.05f * cw && std::fabs(cy) <= 1.05f * cw;
  };
  const uint32_t batch = uint32_t(particles.batches.size());
  particles.batches.push_back({lampMaterial(d), 0x100u});
  static const uint32_t kColour[3] = {0xFFFF2A14u, 0xFFFFB414u, 0xFF28FF6Eu};  // red, yellow, green
  // the camera (the view's inverse translation): a lamp shows only to the side its lenses face
  const Mat4& vw = v.view;
  const Vec3 eye{-(vw.at(0, 0) * vw.at(0, 3) + vw.at(1, 0) * vw.at(1, 3) + vw.at(2, 0) * vw.at(2, 3)),
                 -(vw.at(0, 1) * vw.at(0, 3) + vw.at(1, 1) * vw.at(1, 3) + vw.at(2, 1) * vw.at(2, 3)),
                 -(vw.at(0, 2) * vw.at(0, 3) + vw.at(1, 2) * vw.at(1, 3) + vw.at(2, 2) * vw.at(2, 3))};
  for (const Light& l : c.lights) {
    const float t = std::fmod(now + l.offset, kCycle);
    bool on[3] = {false, false, false};
    if (t < kGreen) on[2] = true;
    else if (t < kGreen + kYellow) on[1] = true;
    else if (t < kCycle - kRedYellow) on[0] = true;
    else on[0] = on[1] = true;
    // at night an unseen light: a small pool on the street in front of the signal (the light map has no colour of its
    // own: neutral for green, warm for red and amber; owner 2026-11-16)
    if (night > 0.08f && std::hypot(l.base.x - eye.x, l.base.z - eye.z) < 420.0f) {
      ExtraNightLight e;
      e.pos = l.base + l.face * 1.6f;
      e.radius = 3.2f;
      e.gain = 0.32f;
      e.warm = on[2] ? 0.0f : 1.0f;
      pools.push_back(e);
    }
    if (!inView(l.lamp[1])) continue;
    if ((eye.x - l.lamp[1].x) * l.face.x + (eye.z - l.lamp[1].z) * l.face.z < 0) continue;
    for (int m = 0; m < 3; ++m)
      if (on[m]) {
        ParticleSprite s;
        s.pos = l.lamp[m];
        s.size = l.radius[m] * (1.6f + 1.6f * night);  // (by day the lens lit, at night with a halo)
        s.argb = kColour[m];
        s.batch = batch;
        particles.sprites.push_back(s);
      }
  }
}

}  // namespace regs::engine
