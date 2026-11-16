// Working traffic lights: see traffic_lights.h.
#include "engine/traffic_lights.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <string>

#include "engine/display.h"
#include "engine/grid.h"
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

struct Light {
  Vec3 lamp[3];
  Vec3 face;         // where the lenses look (flat, unit)
  float offset = 0;  // seconds into the cycle at time 0
};
struct Cache {
  uint64_t generation = ~0ull, itemsVersion = ~0ull;
  std::vector<Light> lights;
};
std::map<const GridObj*, Cache>& caches() {
  static std::map<const GridObj*, Cache> m;
  return m;
}

void collect(GridObj* grid, Cache& c) {
  c.lights.clear();
  const GridData& g = *grid->data;
  const TerrainCache& tc = *grid->terrain;
  std::vector<uint8_t> isLight(tc.templates.size(), 0);
  for (size_t t = 0; t < tc.templates.size(); ++t) {
    const GridTemplate& tp = tc.templates[t];
    if (tp.object && tp.object->pendingPath.find("traffic-lights") != std::string::npos) isLight[t] = 1;
  }
  struct Raw {
    Vec3 pos, face;
    Mat4 world;
  };
  std::vector<Raw> raw;
  for (uint32_t i = 0; i < g.items.size(); ++i) {
    const GridData::Item& it = g.items[i];
    if (it.removed || it.type >= tc.typeTemplate.size()) continue;
    const int ti = tc.typeTemplate[it.type];
    if (ti < 0 || !isLight[size_t(ti)]) continue;
    const int div = itemDivision(g, tc, i);
    if (div < 0) continue;
    Raw r;
    r.world = tc.divisions[size_t(div)].world * itemPlacement(g, tc, i, div);
    r.pos = transformPoint(r.world, Vec3{0, 0, 0});
    const Vec3 f = transformPoint(r.world, Vec3{1, 0, 0});  // (the lenses look along the model's +x)
    r.face = Vec3{f.x - r.pos.x, 0, f.z - r.pos.z};
    raw.push_back(r);
  }
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
    for (int m = 0; m < 3; ++m) l.lamp[m] = transformPoint(raw[k].world, kLamp[m]);
    l.face = f;
    // the wave: the junction's place along its axis (the first light's position stands for it)
    const Raw& first = raw[firstLight[size_t(junction[k])]];
    const float wave = (first.pos.x * ax.x + first.pos.z * ax.z) / kWaveSpeed;
    l.offset = std::fmod(-wave + (along ? 0.0f : kCycle * 0.5f), kCycle);
    if (l.offset < 0) l.offset += kCycle;
    c.lights.push_back(l);
  }
}

}  // namespace

void addTrafficLights(Display& d, script::VM&, const std::vector<GridObj*>& grids, const SceneView& v, ParticleDrawList& particles) {
  GridObj* grid = nullptr;
  for (GridObj* g : grids)
    if (g->data && g->terrain && !g->data->items.empty()) grid = g;
  if (!grid || grid->terrain->viewMode == 0x180) return;  // (the underground view: none)
  Cache& c = caches()[grid];
  if (c.generation != grid->generation || c.itemsVersion != grid->itemsVersion) {
    c.generation = grid->generation;
    c.itemsVersion = grid->itemsVersion;
    collect(grid, c);
  }
  if (c.lights.empty()) return;
  static const auto start = std::chrono::steady_clock::now();
  const float now = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
  const float night = vehicleNight01();
  const float size = 0.28f + 0.25f * night;  // (by day a bright lamp the size of the lens, at night a halo)
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
    if (!inView(l.lamp[1])) continue;
    if ((eye.x - l.lamp[1].x) * l.face.x + (eye.z - l.lamp[1].z) * l.face.z < 0) continue;
    const float t = std::fmod(now + l.offset, kCycle);
    bool on[3] = {false, false, false};
    if (t < kGreen) on[2] = true;
    else if (t < kGreen + kYellow) on[1] = true;
    else if (t < kCycle - kRedYellow) on[0] = true;
    else on[0] = on[1] = true;
    for (int m = 0; m < 3; ++m)
      if (on[m]) {
        ParticleSprite s;
        s.pos = l.lamp[m];
        s.size = size;
        s.argb = kColour[m];
        s.batch = batch;
        particles.sprites.push_back(s);
      }
  }
}

}  // namespace regs::engine
