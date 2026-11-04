// Terrain of a loaded map, after gsGridDivision::update (L:0x08166080 = W:0x100d6ab0), docs/renderer/terrain.md.
// The grid is cut into n×n-cell divisions (n = 1 << bucketShift); each is one mesh centred on the division, built in
// two LODs (lod 0: a 4-triangle fan per cell, lod 1: 2 triangles per cell) with one part per material group. Water
// quads and the map-edge walls are LAYER groups, so both LODs carry them. Not yet: cover quadrants and normal
// overrides from items, the water-edge curtain, highlights, the data overlay and the underground view.
#include "engine/terrain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>

#include "core/log.h"
#include "core/text.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/grid_render.h"
#include "engine/scene3d.h"
#include "engine/sim.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

// material groups, in submesh order
constexpr int kTerrain = 0, kYard = 1, kField = 9, kWater = 17, kEdge = 18, kGroupCount = 19;
constexpr int kWaterLevel = -0x140, kEdgeBottom = -0x2000;
// mesh LOD bands for D = 800 (interfaces-3d §1.3); meshes are freed beyond kFreeDistance
constexpr float kLod0Distance = 266.4f, kCullDistance = 880.0f, kFreeDistance = 1000.0f;
// division meshes are built on demand (the port's streaming; the original keeps divisions around each camera): those
// in view (or casting into it) at once, up to kVisibleBuildsPerFrame, else the nearest kItemBuildsPerFrame per frame,
// including a kPrefetchMargin ring just outside the view so panning finds them ready
constexpr int kItemBuildsPerFrame = 6, kVisibleBuildsPerFrame = 64;
constexpr float kPrefetchMargin = 120.0f;

struct Corner {
  Vec3 pos;     // division-local
  Vec3 normal;
  float u = 0, v = 0;
  uint32_t color = 0xFF000000;  // R = ruin / wild-ground variant
  float s = 0, t = 0;           // TEXCOORD1: field or sea-floor weight, cliff weight
};

struct Builder {
  const GridData& g;
  int n, dx, dz;  // division size and first cell
  float cs;
  std::vector<Corner> corners;  // (n+1)², row-major in z
  MeshData out;

  int W() const { return g.width; }
  int L() const { return g.length; }
  int cell(int x, int z) const { return std::clamp(z, 0, L() - 1) * W() + std::clamp(x, 0, W() - 1); }
  int h(int x, int z) const { return g.height[size_t(cell(x, z))]; }
  uint16_t f(int x, int z) const { return g.flags[size_t(cell(x, z))]; }

  uint32_t override(int x, int z) const { return g.normals.empty() ? 0 : g.normals[size_t(cell(x, z))]; }
  static Vec3 decode(uint32_t o) {  // packed 0x00XXYYZZ, component = byte/127 − 1
    return {float(o >> 16 & 0xFF) / 127.0f - 1.0f, float(o >> 8 & 0xFF) / 127.0f - 1.0f, float(o & 0xFF) / 127.0f - 1.0f};
  }

  // createVertex for corner (gx, gz) (§3); "covered" = a cell around it carries an item normal override
  Corner makeCorner(int gx, int gz) const {
    Corner c;
    int x = std::clamp(gx, 0, W() - 1), z = std::clamp(gz, 0, L() - 1);
    c.pos = {(gx - dx) * cs - n * cs * 0.5f, h(x, z) * cs / 256.0f, (gz - dz) * cs - n * cs * 0.5f};
    // the up to 4 cells around the corner
    int qx[4] = {x, x - 1, x, x - 1}, qz[4] = {z, z, z - 1, z - 1};
    bool qok[4] = {true, x > 0, z > 0, x > 0 && z > 0};
    Vec3 sum{0, 0, 0};
    bool covered = false;
    for (int k = 0; k < 4; ++k)
      if (qok[k])
        if (uint32_t o = override(qx[k], qz[k])) {
          covered = true;
          sum = sum + decode(o);
        }
    if (covered) {
      c.normal = normalize(sum);
    } else {  // height gradient; a neighbour outside the map is the cell itself
      int hx0 = x > 0 ? h(x - 1, z) : h(x, z), hx1 = x + 1 < W() ? h(x + 1, z) : h(x, z);
      int hz0 = z > 0 ? h(x, z - 1) : h(x, z), hz1 = z + 1 < L() ? h(x, z + 1) : h(x, z);
      c.normal = normalize(Vec3{float(hx0 - hx1) / 512.0f, 1.0f, float(hz0 - hz1) / 512.0f});
    }
    c.u = gx * 0.125f;
    c.v = gz * 0.125f;
    uint16_t fc = f(x, z);
    uint32_t r = (fc & 0x40) ? (covered ? 0 : 255) : ((fc & 0x28) || covered) ? 0 : uint32_t(fc & 7) << 5;
    c.color = 0xFF000000u | r;
    if ((fc & 0x20) && !covered) {  // field weight: all cells around are field of the same style
      bool all = true;
      for (int k = 0; k < 4; ++k)
        if (qok[k]) {
          uint16_t q = f(qx[k], qz[k]);
          if (!(q & 0x20) || (q & 7) != (fc & 7)) all = false;
        }
      c.s = all ? 1.0f : 0.0f;
    } else if (!covered) {  // sea-floor weight: any cell around is water
      for (int k = 0; k < 4; ++k)
        if (qok[k] && (f(qx[k], qz[k]) & 0x2000)) c.s = 1.0f;
    }
    c.t = covered ? 0.0f : std::clamp(4.0f - 4.2f * c.normal.y, 0.0f, 1.0f);
    return c;
  }

  const Corner& corner(int lx, int lz) const { return corners[size_t(lz * (n + 1) + lx)]; }

  uint32_t push(const Corner& c) {
    uint32_t i = uint32_t(out.vertexCount());
    out.positions.insert(out.positions.end(), {c.pos.x, c.pos.y, c.pos.z});
    out.normals.insert(out.normals.end(), {c.normal.x, c.normal.y, c.normal.z});
    out.uv0.insert(out.uv0.end(), {c.u, c.v});
    out.uv1.insert(out.uv1.end(), {c.s, c.t});
    out.colors.push_back(c.color);
    return i;
  }

  // corners absent from a cell's fan: both quadrants touching them are covered (§4); order TL, TR, BL, BR
  static void absentCorners(uint16_t fc, bool out[4]) {
    out[0] = (fc & 0x900) == 0x900;
    out[1] = (fc & 0x300) == 0x300;
    out[2] = (fc & 0xC00) == 0xC00;
    out[3] = (fc & 0x600) == 0x600;
  }

  static uint32_t meanColor(const Corner* const* q, int count) {
    uint32_t col = 0;
    for (int b = 0; b < 32; b += 8) {
      uint32_t sum = 0;
      for (int k = 0; k < count; ++k) sum += q[k]->color >> b & 0xFF;
      col |= (count == 4 ? sum >> 2 : sum >> 1) << b;
    }
    return col;
  }

  // centre vertex of cell (lx, lz) (createVertex(gsIndexedMesh const&, …), §3.1)
  Corner centre(int lx, int lz) const {
    int x = dx + lx, z = dz + lz;
    const Corner* q[4] = {&corner(lx, lz), &corner(lx + 1, lz), &corner(lx, lz + 1), &corner(lx + 1, lz + 1)};
    bool absent[4];
    absentCorners(f(x, z), absent);
    Corner c;
    c.pos = q[0]->pos + Vec3{cs * 0.5f, 0, cs * 0.5f};
    c.u = q[0]->u + 0.0625f;
    c.v = q[0]->v + 0.0625f;
    if (!absent[0] && !absent[1] && !absent[2] && !absent[3]) {
      c.pos.y = (q[0]->pos.y + q[1]->pos.y + q[2]->pos.y + q[3]->pos.y) * 0.25f;
      c.normal = normalize(q[0]->normal + q[1]->normal + q[2]->normal + q[3]->normal);
      c.color = meanColor(q, 4);
      c.s = (q[0]->s + q[1]->s + q[2]->s + q[3]->s) * 0.25f;
    } else {
      // the height of the (first) missing corner, read from the cells
      static const int ox[4] = {0, 1, 0, 1}, oz[4] = {0, 0, 1, 1};
      for (int k = 0; k < 4; ++k)
        if (absent[k]) {
          c.pos.y = h(std::min(x + ox[k], W() - 1), std::min(z + oz[k], L() - 1)) * cs / 256.0f;
          break;
        }
      const Corner* pair[2] = {nullptr, nullptr};
      if (!absent[0] && !absent[3]) pair[0] = q[0], pair[1] = q[3];
      else if (!absent[1] && !absent[2]) pair[0] = q[1], pair[1] = q[2];
      if (pair[0]) {
        c.normal = normalize(pair[0]->normal + pair[1]->normal);
        c.color = meanColor(pair, 2);
        c.s = (pair[0]->s + pair[1]->s) * 0.5f;
      } else {
        uint32_t o = override(x, z);
        c.normal = o ? normalize(decode(o)) : Vec3{0, 1, 0};
        c.color = !absent[0] ? q[0]->color : q[3]->color;
        c.s = 0;
      }
    }
    c.t = std::clamp(4.0f - 4.2f * c.normal.y, 0.0f, 1.0f);
    if (c.s == 0 && (f(x, z) & 0x20)) c.s = 1.0f;  // isolated field cells get a field centre
    return c;
  }

  int groupOf(uint16_t fc, int group) const {
    if (group == kTerrain) return (fc & 0x40) || !(fc & 0x28);
    if (group < kField) return (fc & 0x48) == 8 && (fc & 7) == group - kYard;
    return (fc & 0x60) == 0x20 && (fc & 7) == group - kField;
  }

  void surfaceGroup(int group, int lod) {
    size_t firstIndex = out.indices.size();
    std::vector<int32_t> map(size_t((n + 1) * (n + 1)), -1);
    auto at = [&](int lx, int lz) {
      int32_t& m = map[size_t(lz * (n + 1) + lx)];
      if (m < 0) m = int32_t(push(corner(lx, lz)));
      return uint32_t(m);
    };
    for (int lz = 0; lz < n && dz + lz < L(); ++lz)
      for (int lx = 0; lx < n && dx + lx < W(); ++lx) {
        uint16_t fc = f(dx + lx, dz + lz);
        uint16_t cover = fc & 0xF00;
        if (cover == 0xF00 || !groupOf(fc, group)) continue;  // fully under an item: no terrain
        if (lod == 1 && !cover) {
          uint32_t tl = at(lx, lz), tr = at(lx + 1, lz), bl = at(lx, lz + 1), br = at(lx + 1, lz + 1);
          out.indices.insert(out.indices.end(), {tl, bl, tr, tr, bl, br});
          continue;
        }
        // the fan around the centre, one triangle per uncovered quadrant: N, W, E, S (§4)
        bool absent[4];
        absentCorners(fc, absent);
        uint32_t k[4];
        static const int ox[4] = {0, 1, 0, 1}, oz[4] = {0, 0, 1, 1};
        for (int i = 0; i < 4; ++i) k[i] = absent[i] ? 0 : at(lx + ox[i], lz + oz[i]);
        uint32_t c = push(centre(lx, lz));
        const uint32_t tl = k[0], tr = k[1], bl = k[2], br = k[3];
        if (!(cover & 0x100)) out.indices.insert(out.indices.end(), {tl, c, tr});
        if (!(cover & 0x800)) out.indices.insert(out.indices.end(), {bl, c, tl});
        if (!(cover & 0x200)) out.indices.insert(out.indices.end(), {tr, c, br});
        if (!(cover & 0x400)) out.indices.insert(out.indices.end(), {br, c, bl});
      }
    finishGroup(group, firstIndex);
  }

  void finishGroup(int group, size_t firstIndex) {
    if (out.indices.size() == firstIndex) return;
    Submesh s;
    s.firstIndex = uint32_t(firstIndex);
    s.indexCount = uint32_t(out.indices.size() - firstIndex);
    s.material = group;
    out.submeshes.push_back(s);
  }

  // water quads at the fixed level, alpha = depth below the surface (§7.1)
  void water() {
    size_t firstIndex = out.indices.size();
    std::vector<int32_t> map(size_t((n + 1) * (n + 1)), -1);
    auto at = [&](int lx, int lz) {
      int32_t& m = map[size_t(lz * (n + 1) + lx)];
      if (m < 0) {
        int gx = dx + lx, gz = dz + lz;
        Corner c;
        c.pos = {lx * cs - n * cs * 0.5f, kWaterLevel * cs / 256.0f, lz * cs - n * cs * 0.5f};
        c.normal = {0, 1, 0};
        c.u = gx * cs * 0.5f;
        c.v = gz * cs * 0.5f;
        uint32_t a = uint32_t(std::clamp((kWaterLevel - h(gx, gz)) >> 2, 0, 255));
        c.color = a << 24 | 0x808080u;
        m = int32_t(push(c));
      }
      return uint32_t(m);
    };
    for (int lz = 0; lz < n && dz + lz < L(); ++lz)
      for (int lx = 0; lx < n && dx + lx < W(); ++lx) {
        if (!(f(dx + lx, dz + lz) & 0x2000)) continue;
        uint32_t tl = at(lx, lz), tr = at(lx + 1, lz), bl = at(lx, lz + 1), br = at(lx + 1, lz + 1);
        out.indices.insert(out.indices.end(), {tl, bl, tr, tr, bl, br});
      }
    finishGroup(kWater, firstIndex);
  }

  // outward-facing walls from the terrain down to −96 m on the map's border sides (§8.1)
  void edges() {
    size_t firstIndex = out.indices.size();
    struct Side { bool on; Vec3 normal; bool alongX; int fixed; };
    Side sides[4] = {{dz == 0, {0, 0, -1}, true, 0},
                     {dz + n >= L(), {0, 0, 1}, true, std::min(n, L() - dz)},
                     {dx == 0, {-1, 0, 0}, false, 0},
                     {dx + n >= W(), {1, 0, 0}, false, std::min(n, W() - dx)}};
    for (const Side& sd : sides) {
      if (!sd.on) continue;
      int count = sd.alongX ? std::min(n, W() - dx) : std::min(n, L() - dz);
      uint32_t base = uint32_t(out.vertexCount());
      for (int i = 0; i <= count; ++i) {
        int lx = sd.alongX ? i : sd.fixed, lz = sd.alongX ? sd.fixed : i;
        int top = h(dx + lx, dz + lz);
        for (int bottom = 0; bottom < 2; ++bottom) {
          Corner c;
          int y = bottom ? kEdgeBottom : top;
          c.pos = {lx * cs - n * cs * 0.5f, y * cs / 256.0f, lz * cs - n * cs * 0.5f};
          c.normal = sd.normal;
          c.u = (lx + lz) * 0.0625f;
          c.v = 1.0f - float(y - kEdgeBottom) / float(std::max(top - kEdgeBottom, 1));
          c.color = 0xFF808080u;
          push(c);
        }
      }
      for (int i = 0; i < count; ++i) {
        uint32_t t0 = base + uint32_t(i) * 2, b0 = t0 + 1, t1 = t0 + 2, b1 = t0 + 3;
        // wind each triangle so that it faces outward
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
          auto P = [&](uint32_t k) { return Vec3{out.positions[k * 3], out.positions[k * 3 + 1], out.positions[k * 3 + 2]}; };
          if (dot(cross(P(b) - P(a), P(c) - P(a)), sd.normal) < 0) std::swap(b, c);
          out.indices.insert(out.indices.end(), {a, b, c});
        };
        tri(t0, b0, t1);
        tri(t1, b0, b1);
      }
    }
    finishGroup(kEdge, firstIndex);
  }

  void build(int lod) {
    corners.resize(size_t((n + 1) * (n + 1)));
    for (int lz = 0; lz <= n; ++lz)
      for (int lx = 0; lx <= n; ++lx) corners[size_t(lz * (n + 1) + lx)] = makeCorner(dx + lx, dz + lz);
    for (int group = kTerrain; group < kWater; ++group) surfaceGroup(group, lod);
    water();
    edges();
    computeTangents(out);  // the terrain and yards 4-7 are normal-mapped
    float r2 = 0;
    out.boundsMin = {1e30f, 1e30f, 1e30f};
    out.boundsMax = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < out.vertexCount(); ++i) {
      float p[3] = {out.positions[i * 3], out.positions[i * 3 + 1], out.positions[i * 3 + 2]};
      for (int k = 0; k < 3; ++k) {
        out.boundsMin[size_t(k)] = std::min(out.boundsMin[size_t(k)], p[k]);
        out.boundsMax[size_t(k)] = std::max(out.boundsMax[size_t(k)], p[k]);
      }
      r2 = std::max(r2, p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    }
    out.radius = std::sqrt(r2);
  }
};

Object* gridMaterialObject(VM& vm, GridObj* grid, int group) {
  std::u16string key;
  if (group == kTerrain) key = u"material";
  else if (group < kField) key = u"<getYardMaterial>" + utf8ToUtf16(std::to_string(group - kYard));
  else if (group < kWater) key = u"<getFieldMaterial>" + utf8ToUtf16(std::to_string(group - kField));
  else if (group == kWater) key = u"waterMaterial";
  else key = u"edgeMaterial";
  if (key[0] != u'<') {  // a sub-object field: the getter creates it on first access
    try {
      return vm.resolve(vm.getMember(Value::heap(VT::Object, grid), key)).object();
    } catch (const ScriptError&) {
      return nullptr;
    }
  }
  auto it = grid->fieldValues.find(key);
  return it == grid->fieldValues.end() ? nullptr : it->second.object();
}

}  // namespace

void prepareTerrainCells(GridData& g) {
  int W = g.width, L = g.length;
  for (auto& fl : g.flags) fl &= 0xB0FF;  // selection highlight and cover quadrants are runtime state
  auto h = [&](int x, int z) { return g.height[size_t(std::min(z, L - 1) * W + std::min(x, W - 1))]; };
  for (int z = 0; z < L; ++z)
    for (int x = 0; x < W; ++x) {
      uint16_t& fl = g.flags[size_t(z * W + x)];
      if ((fl & 0x2000) && std::min({h(x, z), h(x + 1, z), h(x, z + 1), h(x + 1, z + 1)}) >= kWaterLevel) fl &= 0xDFFF;
    }
}

void collectGrid(Display& d, VM& vm, GridObj* grid, const Vec3& eye, const Mat4& viewProj, float farZ, float viewScale,
                 float detailScale, std::vector<DrawItem>& items) {
  if (!grid->data || grid->data->height.empty()) return;
  const GridData& g = *grid->data;
  if (!grid->terrain) grid->terrain = std::make_shared<TerrainCache>();
  TerrainCache& tc = *grid->terrain;
  float cs = g.cellSize;
  if (tc.generation != grid->generation) {
    for (GridDivision& dv : tc.divisions)
      for (int k = 0; k < 2; ++k) {
        d.r3d().destroy(dv.terrain[k]);
        d.r3d().destroy(dv.items[k]);
      }
    if (tc.highlightBuilt) d.r3d().destroy(tc.highlight);
    tc = TerrainCache();
    tc.generation = grid->generation;
    tc.n = 1 << std::min(g.bucketShift, 8u);
    tc.cols = (g.width + tc.n - 1) / tc.n;
    tc.rows = (g.length + tc.n - 1) / tc.n;
    tc.divisions.resize(size_t(tc.cols * tc.rows));
    for (int r = 0; r < tc.rows; ++r)
      for (int c = 0; c < tc.cols; ++c) {
        GridDivision& dv = tc.divisions[size_t(r * tc.cols + c)];
        // mesh position (§1): ((DX − ((W−n)>>1))·cs, 0, (DZ − ((L−n)>>1))·cs)
        Vec3 p{(c * tc.n - ((g.width - tc.n) >> 1)) * cs, 0, (r * tc.n - ((g.length - tc.n) >> 1)) * cs};
        dv.world = translation(p);
        dv.center = p;
        dv.radius = tc.n * cs * 0.7071f;
      }
    for (int group = 0; group < kGroupCount; ++group) tc.materials.push_back(scriptMaterial(d, vm, gridMaterialObject(vm, grid, group)));
    // the tool highlights (terrain.md §5: positiveMaterial / negativeMaterial, selection.shader decals)
    for (const char16_t* key : {u"positiveMaterial", u"negativeMaterial"}) {
      Object* m = nullptr;
      try {
        m = vm.resolve(vm.getMember(Value::heap(VT::Object, grid), key)).object();
      } catch (const ScriptError&) {
      }
      (key[0] == u'p' ? tc.positiveMaterial : tc.negativeMaterial) = int(tc.materials.size());
      tc.materials.push_back(scriptMaterial(d, vm, m));
    }
    initGridItems(d, vm, grid, tc);
    fixGridCover(vm, *grid->data, tc);
    LOG_INFO("terrain: %dx%d divisions of %d cells", tc.cols, tc.rows, tc.n);
  }
  // live map edits (engine/grid_edit.h): new types and items, then the touched divisions rebuilt at once where built
  syncGridItems(d, vm, grid, tc);
  if (!tc.dirtyTerrain.empty() || !tc.dirtyItems.empty()) {
    for (int index : tc.dirtyTerrain) {
      if (index < 0 || size_t(index) >= tc.divisions.size()) continue;
      GridDivision& dv = tc.divisions[size_t(index)];
      for (int lod = 0; lod < 2; ++lod) {
        if (!dv.terrainBuilt[lod]) continue;
        d.r3d().destroy(dv.terrain[lod]);
        Builder b{g, tc.n, (index % tc.cols) * tc.n, (index / tc.cols) * tc.n, cs, {}, {}};
        b.build(lod);
        dv.terrain[lod] = d.r3d().upload(b.out);
      }
    }
    for (int index : tc.dirtyItems) {
      if (index < 0 || size_t(index) >= tc.divisions.size()) continue;
      GridDivision& dv = tc.divisions[size_t(index)];
      for (int lod = 0; lod < 2; ++lod) {
        if (!dv.itemsBuilt[lod]) continue;
        d.r3d().destroy(dv.items[lod]);
        MeshData out;
        buildGridItems(g, tc, index, lod, out);
        dv.items[lod] = d.r3d().upload(out);
      }
    }
    tc.dirtyTerrain.clear();
    tc.dirtyItems.clear();
  }
  // view frustum planes (Gribb/Hartmann) for culling the divisions' bounding spheres
  float planes[6][4];
  for (int i = 0; i < 6; ++i) {
    int row = i / 2;
    float sign = (i & 1) ? -1.0f : 1.0f;
    for (int k = 0; k < 4; ++k) planes[i][k] = viewProj.m[k * 4 + 3] + sign * viewProj.m[k * 4 + row];
  }
  auto visible = [&](const Vec3& c, float r) {
    for (auto& p : planes) {
      float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
      if (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3] < -r * len) return false;
    }
    return true;
  };
  float reach = std::min(kCullDistance * viewScale, farZ), freeDistance = kFreeDistance * viewScale;
  // a division's terrain and items of one LOD are built together and shown only together: terrain without its items
  // has holes where the items cover it (fixGrid), which showed as glitches at the screen edges while panning
  auto build = [&](GridDivision& dv, int index, int lod) {
    if (!dv.terrainBuilt[lod]) {
      Builder b{g, tc.n, (index % tc.cols) * tc.n, (index / tc.cols) * tc.n, cs, {}, {}};
      b.build(lod);
      dv.terrain[lod] = d.r3d().upload(b.out);
      dv.terrainBuilt[lod] = true;
    }
    if (!dv.itemsBuilt[lod]) {
      MeshData out;
      buildGridItems(g, tc, index, lod, out);
      dv.items[lod] = d.r3d().upload(out);
      dv.itemsBuilt[lod] = true;
    }
  };
  struct Pending {
    float dist;
    int division, lod;
  };
  std::vector<Pending> pending;
  int buildsLeft = kVisibleBuildsPerFrame;
  // the grid clock of animated items: wall time scaled by grid.timeSpeed (0 while paused), as gsGrid.tick scales the
  // dt its items move by; one clock per grid, so several views in a frame do not advance it twice
  static std::map<const GridObj*, std::pair<std::chrono::steady_clock::time_point, double>> clocks;
  auto now = std::chrono::steady_clock::now();
  auto& clock = clocks.try_emplace(grid, now, 0.0).first->second;
  {
    auto speed = grid->fieldValues.find(u"timeSpeed");
    double ts = speed == grid->fieldValues.end() || speed->second.isNone() ? 1.0 : vm.toFloat(speed->second);
    double dt = std::min(std::chrono::duration<double>(now - clock.first).count(), 0.05);
    clock.second += dt * ts;
    clock.first = now;
  }
  std::map<const ObjectObj*, std::shared_ptr<const std::vector<Mat4>>> poses;  // one pose per template per frame
  const std::unordered_map<uint32_t, float>* doorTimes = simDoorTimes(grid);   // door items: their own clip time
  for (int r = 0; r < tc.rows; ++r)
    for (int c = 0; c < tc.cols; ++c) {
      int index = r * tc.cols + c;
      GridDivision& dv = tc.divisions[size_t(index)];
      Vec3 dvec = dv.center - eye;
      float dist = std::sqrt(dot(dvec, dvec));
      if (dist > freeDistance + dv.radius) {
        for (int k = 0; k < 2; ++k) {
          if (dv.terrainBuilt[k]) d.r3d().destroy(dv.terrain[k]);
          if (dv.itemsBuilt[k]) d.r3d().destroy(dv.items[k]);
          dv.terrainBuilt[k] = dv.itemsBuilt[k] = false;
        }
        continue;
      }
      if (dist - dv.radius > reach) continue;
      int lod = dist < kLod0Distance * detailScale ? 0 : 1;
      auto complete = [&](int k) { return dv.terrainBuilt[k] && dv.itemsBuilt[k]; };
      // bounds before the meshes exist: the division square from below the ground up to tall buildings (generous);
      // casters are taken with the radius doubled (shadows.md §4.3); slightly outside the view is built ahead
      Vec3 gc = dv.center + Vec3{0, 20.0f, 0};
      float gr = dv.radius + 60.0f;
      bool near = visible(gc, 2.0f * gr), ahead = visible(gc, gr + kPrefetchMargin * viewScale);
      if (!near && !ahead) continue;
      if (!complete(lod)) {
        if (near && buildsLeft > 0) {
          build(dv, index, lod);
          --buildsLeft;
        } else {
          pending.push_back({dist, index, lod});
        }
      }
      int show = complete(lod) ? lod : complete(1 - lod) ? 1 - lod : -1;  // during a LOD switch: the other one
      if (show < 0) continue;
      for (GpuMesh* m : {&dv.terrain[show], &dv.items[show]}) {
        if (m->parts.empty()) continue;
        // the built mesh's own bounds: bucketed items reach beyond the division square
        Vec3 mc = dv.center + m->center;
        if (!visible(mc, 2.0f * m->radius)) continue;
        DrawItem it;
        it.mesh = m;
        it.materials = &tc.materials;
        it.world = dv.world;
        it.shadowOnly = !visible(mc, m->radius);
        items.push_back(it);
      }
      collectSkinnedItems(d, vm, g, tc, index, dv.world, dist, show, clock.second, !visible(gc, gr), doorTimes, poses, items);
    }
  // the tool highlight overlay: rebuilt when a tool changed the highlighted cells or items, drawn in world space
  if (tc.highlightVersion != grid->highlightVersion) {
    if (tc.highlightBuilt) d.r3d().destroy(tc.highlight);
    tc.highlightBuilt = false;
    tc.highlightVersion = grid->highlightVersion;
    if (!grid->highlightCells.empty() || !grid->highlightItems.empty()) {
      MeshData out;
      buildHighlightOverlay(g, *grid, tc, out);
      if (out.vertexCount()) {
        tc.highlight = d.r3d().upload(out);
        tc.highlightBuilt = true;
      }
    }
  }
  if (tc.highlightBuilt && !tc.highlight.parts.empty()) {
    DrawItem it;
    it.mesh = &tc.highlight;
    it.materials = &tc.materials;
    items.push_back(it);
  }
  std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) { return a.dist < b.dist; });
  if (pending.size() > size_t(kItemBuildsPerFrame)) pending.resize(size_t(kItemBuildsPerFrame));
  for (const Pending& p : pending) build(tc.divisions[size_t(p.division)], p.division, p.lod);  // shown from the next frame
}

}  // namespace regs::engine
