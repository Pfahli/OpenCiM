// Terrain of a loaded map, after gsGridDivision::update (L:0x08166080 = W:0x100d6ab0), docs/renderer/terrain.md.
// The grid is cut into n×n-cell divisions (n = 1 << bucketShift); each is one mesh centred on the division, built in
// two LODs (lod 0: a 4-triangle fan per cell, lod 1: 2 triangles per cell) with one part per material group. Water
// quads, the map-edge walls, the water-edge curtain and the items' foundation walls are LAYER groups, so both LODs carry
// them. The underground view (docs/renderer/underground.md) builds tunnel divisions as an earth plane with inward walls.
// The data layer draws the stops' catchment circles over the layerable parts (dataMaterial, terrain.md §5.2).
#include "engine/terrain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/curve_roads.h"
#include "engine/paint_edges.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/grid_render.h"
#include "engine/gui.h"
#include "engine/interaction.h"
#include "engine/scene3d.h"
#include "engine/sim.h"
#include "engine/surface_map.h"
#include "render/glsl.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

// material groups, in submesh order (kEarth: earthMaterial, the terrain without bit 0x20 and the underground plane)
// kBase: baseMaterial, the foundation walls of items on uneven ground; kWaterEdge: waterEdgeMaterial, the curtain where
// the map's border meets water
constexpr int kTerrain = 0, kYard = 1, kField = 9, kWater = 17, kEdge = 18, kEarth = 19, kBase = 20, kWaterEdge = 21,
              kGroupCount = 22;
constexpr int kWaterLevel = -0x140, kEdgeBottom = -0x2000, kEarthLevel = -0x800;
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
  // the division's view mode (divisionViewMode) and, for a tunnel division of the underground view, the sides that
  // face a division without tunnels (north z−, south z+, west x−, east x+): they get inward walls (§8.1 of terrain.md)
  uint32_t view = 0xFE;
  bool inner[4] = {false, false, false, false};
  // the division's items (TerrainCache::divisionItems) for the foundation walls
  const TerrainCache* tc = nullptr;
  int index = -1;

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
    if (group == kEarth) return 1;  // without bit 0x20 every cell is terrain, drawn in earthMaterial (§6, :1232)
    if (group == kTerrain) return (fc & 0x40) || !(fc & 0x28);
    if (group < kField) return (fc & 0x48) == 8 && (fc & 7) == group - kYard;
    return (fc & 0x60) == 0x20 && (fc & 7) == group - kField;
  }

  // port: a cell cut by a painted area's edge (engine/paint_edges.h): the fan's uncovered quadrants clipped along the
  // edge's lines, the part inside (inside = true) or outside of it, each piece with vertices of its own (attributes
  // interpolated along the triangle's edges, so the cut lies in the triangle's plane)
  template <class Scale>
  void edgeCell(int lx, int lz, uint16_t cover, const PaintEdge& e, bool inside, const Scale& scaled) {
    struct P {
      Corner c;
      float u, v;
    };
    const Corner& c0 = corner(lx, lz);
    auto mk = [&](const Corner& c) { return P{c, (c.pos.x - c0.pos.x) / cs, (c.pos.z - c0.pos.z) / cs}; };
    auto lerp = [](const P& a, const P& b, float t) {
      P p;
      p.c.pos = a.c.pos + (b.c.pos - a.c.pos) * t;
      p.c.normal = normalize(a.c.normal + (b.c.normal - a.c.normal) * t);
      p.c.u = a.c.u + (b.c.u - a.c.u) * t, p.c.v = a.c.v + (b.c.v - a.c.v) * t;
      p.c.s = a.c.s + (b.c.s - a.c.s) * t, p.c.t = a.c.t + (b.c.t - a.c.t) * t;
      uint32_t col = 0;
      for (int k = 0; k < 32; k += 8) {
        float ca = float(a.c.color >> k & 0xFF), cb = float(b.c.color >> k & 0xFF);
        col |= uint32_t(std::clamp(std::lrint(ca + (cb - ca) * t), 0L, 255L)) << k;
      }
      p.c.color = col;
      p.u = a.u + (b.u - a.u) * t, p.v = a.v + (b.v - a.v) * t;
      return p;
    };
    // Sutherland–Hodgman against one line: keeps the positive side (pos) or the negative one
    auto clip = [&](const std::vector<P>& in, const float* l, bool pos) {
      std::vector<P> res;
      const size_t m = in.size();
      for (size_t i = 0; i < m; ++i) {
        const P& a = in[i];
        const P& b = in[(i + 1) % m];
        float da = l[0] * a.u + l[1] * a.v + l[2], db = l[0] * b.u + l[1] * b.v + l[2];
        if (!pos) da = -da, db = -db;
        if (da >= 0) res.push_back(a);
        if ((da >= 0) != (db >= 0)) res.push_back(lerp(a, b, da / (da - db)));
      }
      return res;
    };
    const P tl = mk(corner(lx, lz)), tr = mk(corner(lx + 1, lz)), bl = mk(corner(lx, lz + 1)),
            br = mk(corner(lx + 1, lz + 1)), c = mk(centre(lx, lz));
    std::vector<std::vector<P>> tris;
    if (!(cover & 0x100)) tris.push_back({tl, c, tr});
    if (!(cover & 0x800)) tris.push_back({bl, c, tl});
    if (!(cover & 0x200)) tris.push_back({tr, c, br});
    if (!(cover & 0x400)) tris.push_back({br, c, bl});
    const float* l0 = e.l[0];
    const float* l1 = e.l[1];
    for (const std::vector<P>& t : tris) {
      std::vector<std::vector<P>> pieces;
      if (e.lines < 2) pieces.push_back(clip(t, l0, inside));
      else if (inside && !e.join) pieces.push_back(clip(clip(t, l0, true), l1, true));
      else if (inside) pieces = {clip(t, l0, true), clip(clip(t, l0, false), l1, true)};
      else if (!e.join) pieces = {clip(t, l0, false), clip(clip(t, l0, true), l1, false)};
      else pieces.push_back(clip(clip(t, l0, false), l1, false));
      for (const std::vector<P>& p : pieces) {
        if (p.size() < 3) continue;
        uint32_t first = push(scaled(p[0].c));
        uint32_t prev = push(scaled(p[1].c));
        for (size_t k = 2; k < p.size(); ++k) {
          uint32_t cur = push(scaled(p[k].c));
          out.indices.insert(out.indices.end(), {first, prev, cur});
          prev = cur;
        }
      }
    }
  }

  void surfaceGroup(int group, int lod) {
    size_t firstIndex = out.indices.size();
    std::vector<int32_t> map(size_t((n + 1) * (n + 1)), -1);
    // the earth terrain takes uv0 · 0.5 (L:0x08166080.c:2360-2370)
    const float uvScale = group == kEarth ? 0.5f : 1.0f;
    auto scaled = [&](Corner c) {
      c.u *= uvScale;
      c.v *= uvScale;
      return c;
    };
    auto at = [&](int lx, int lz) {
      int32_t& m = map[size_t(lz * (n + 1) + lx)];
      if (m < 0) m = int32_t(push(scaled(corner(lx, lz))));
      return uint32_t(m);
    };
    for (int lz = 0; lz < n && dz + lz < L(); ++lz)
      for (int lx = 0; lx < n && dx + lx < W(); ++lx) {
        uint16_t fc = f(dx + lx, dz + lz);
        uint16_t cover = fc & 0xF00;
        // port: a painted area's edge cuts the cell (engine/paint_edges.h): its paint inside, the cell's own outside
        const PaintEdge* edge = nullptr;
        if (!g.paintEdges.empty() && group != kEarth) {
          auto it = g.paintEdges.find(paintEdgeKey(dx + lx, dz + lz));
          if (it != g.paintEdges.end()) edge = &it->second;
        }
        const bool own = groupOf(fc, group), paint = edge && group == kYard + edge->type;
        if (cover == 0xF00 || (!own && !paint)) continue;  // fully under an item: no terrain
        if (edge && !(own && paint)) {
          bool absent[4];
          absentCorners(fc, absent);
          if (!absent[0] && !absent[1] && !absent[2] && !absent[3]) {
            edgeCell(lx, lz, cover, *edge, paint, scaled);
            continue;
          }
          if (!own) continue;  // corners missing under an item: the cell as its flags say
        }
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
        uint32_t c = push(scaled(centre(lx, lz)));
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

  // the underground earth plane (createEarthVertex, L:0x08166080.c:2375-2470, W: y −0x800): a flat square per cell at
  // y = −0x800 (−8 cells) over the whole division, normal +y, uv (gx, gz)·cs/64, colour 0xff808080, earthMaterial
  void earthPlane() {
    size_t firstIndex = out.indices.size();
    std::vector<int32_t> map(size_t((n + 1) * (n + 1)), -1);
    auto at = [&](int lx, int lz) {
      int32_t& m = map[size_t(lz * (n + 1) + lx)];
      if (m < 0) {
        Corner c;
        c.pos = {lx * cs - n * cs * 0.5f, kEarthLevel * cs / 256.0f, lz * cs - n * cs * 0.5f};
        c.normal = {0, 1, 0};
        c.u = float(dx + lx) * cs * 0.015625f;
        c.v = float(dz + lz) * cs * 0.015625f;
        c.color = 0xFF808080u;
        m = int32_t(push(c));
      }
      return uint32_t(m);
    };
    for (int lz = 0; lz < n && dz + lz < L(); ++lz)
      for (int lx = 0; lx < n && dx + lx < W(); ++lx) {
        uint32_t tl = at(lx, lz), tr = at(lx + 1, lz), bl = at(lx, lz + 1), br = at(lx + 1, lz + 1);
        out.indices.insert(out.indices.end(), {tl, bl, tr, tr, bl, br});
      }
    finishGroup(kEarth, firstIndex);
  }

  // outward-facing walls from the terrain down to −96 m on the map's border sides (§8.1). A tunnel division of the
  // underground view clamps their top to −0x800 and adds inward-facing walls from the terrain down on its sides
  // towards divisions without tunnels (L:0x08166080.c:309-345, 4250-4537)
  void edges() {
    size_t firstIndex = out.indices.size();
    struct Side { bool on; Vec3 normal; bool alongX; int fixed; };
    Side sides[4] = {{dz == 0, {0, 0, -1}, true, 0},
                     {dz + n >= L(), {0, 0, 1}, true, std::min(n, L() - dz)},
                     {dx == 0, {-1, 0, 0}, false, 0},
                     {dx + n >= W(), {1, 0, 0}, false, std::min(n, W() - dx)}};
    for (int si = 0; si < 4; ++si) {
      const Side& sd = sides[si];
      bool border = sd.on, wall = !border && inner[si];
      if (!border && !wall) continue;
      Vec3 normal = wall ? sd.normal * -1.0f : sd.normal;
      int count = sd.alongX ? std::min(n, W() - dx) : std::min(n, L() - dz);
      uint32_t base = uint32_t(out.vertexCount());
      for (int i = 0; i <= count; ++i) {
        int lx = sd.alongX ? i : sd.fixed, lz = sd.alongX ? sd.fixed : i;
        int hgt = h(dx + lx, dz + lz), top = border && view == 0x180 ? std::min(hgt, kEarthLevel) : hgt;
        for (int bottom = 0; bottom < 2; ++bottom) {
          Corner c;
          int y = bottom ? kEdgeBottom : top;
          c.pos = {lx * cs - n * cs * 0.5f, y * cs / 256.0f, lz * cs - n * cs * 0.5f};
          c.normal = normal;
          c.u = (lx + lz) * 0.0625f;
          c.v = 1.0f - float(y - kEdgeBottom) / float(std::max(hgt - kEdgeBottom, 1));
          c.color = 0xFF808080u;
          push(c);
        }
      }
      for (int i = 0; i < count; ++i) {
        uint32_t t0 = base + uint32_t(i) * 2, b0 = t0 + 1, t1 = t0 + 2, b1 = t0 + 3;
        // wind each triangle so that it faces outward
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
          auto P = [&](uint32_t k) { return Vec3{out.positions[k * 3], out.positions[k * 3 + 1], out.positions[k * 3 + 2]}; };
          if (dot(cross(P(b) - P(a), P(c) - P(a)), normal) < 0) std::swap(b, c);
          out.indices.insert(out.indices.end(), {a, b, c});
        };
        tri(t0, b0, t1);
        tri(t1, b0, b1);
      }
    }
    finishGroup(kEdge, firstIndex);
  }

  // the water-edge curtain (§7.3, L:0x08166080.c:4684-4979): on the map's border sides, a vertical face per water cell
  // along the side from the sea floor (min(h, −0x140) at each corner) up to the water level, normal outward, colour
  // 0xff808080, uv0.x = 1 on the top row only (waterEdge.vertex waves it); waterEdgeMaterial is blended and two-sided.
  // Normal view only (the underground view has no water bit).
  void waterEdges() {
    if (!(view & 4)) return;
    size_t firstIndex = out.indices.size();
    struct Side { bool on; Vec3 normal; bool alongX; int fixed, cell; };
    int cx = std::min(n, W() - dx), cz = std::min(n, L() - dz);
    Side sides[4] = {{dz == 0, {0, 0, -1}, true, 0, dz},
                     {dz + n >= L(), {0, 0, 1}, true, cz, dz + cz - 1},
                     {dx == 0, {-1, 0, 0}, false, 0, dx},
                     {dx + n >= W(), {1, 0, 0}, false, cx, dx + cx - 1}};
    for (const Side& sd : sides) {
      if (!sd.on) continue;
      int count = sd.alongX ? cx : cz;
      auto cellAt = [&](int i, int& x, int& z) {
        x = sd.alongX ? dx + i : sd.cell;
        z = sd.alongX ? sd.cell : dz + i;
      };
      bool any = false;
      for (int i = 0; i < count && !any; ++i) {
        int x, z;
        cellAt(i, x, z);
        any = (f(x, z) & 0x2000) != 0;
      }
      if (!any) continue;
      uint32_t base = uint32_t(out.vertexCount());
      for (int i = 0; i <= count; ++i) {
        int x, z;
        cellAt(i, x, z);  // the corner's height from the side's cells (clamped at the map's end)
        int lx = sd.alongX ? i : sd.fixed, lz = sd.alongX ? sd.fixed : i;
        int ys[2] = {std::min(h(x, z), kWaterLevel), kWaterLevel};
        for (int y : ys) {
          Corner c;
          c.pos = {lx * cs - n * cs * 0.5f, y * cs / 256.0f, lz * cs - n * cs * 0.5f};
          c.normal = sd.normal;
          c.u = y < kWaterLevel ? 0.0f : 1.0f;
          c.v = 0;
          c.color = 0xFF808080u;
          push(c);
        }
      }
      for (int i = 0; i < count; ++i) {
        int x, z;
        cellAt(i, x, z);
        if (!(f(x, z) & 0x2000)) continue;
        uint32_t b0 = base + uint32_t(i) * 2, t0 = b0 + 1, b1 = b0 + 2, t1 = b0 + 3;
        out.indices.insert(out.indices.end(), {b0, b1, t0, t0, b1, t1});
      }
    }
    finishGroup(kWaterEdge, firstIndex);
  }

  // foundation walls (§8.3, L:0x08166080.c:3737-4249; createBaseVertex L:0x08165b90): every visible ENABLE_BASE item of
  // the division whose ground is not flat at its height (gsGridItem::fixY's flag 0x800, L:0x08187760.c:37-109) gets
  // strips along its footprint, at each perimeter grid point from the ground (clamped cell height) up to the item's y:
  // the four diagonal cuts (item diagonals nibbles 0..3), then the straight sides between them. Normals face away from
  // the item when it stands above the ground and towards it when it is sunk; uv (u along the side, v = −y/1024)
  void baseWalls() {
    if (!tc || index < 0 || size_t(index) >= tc->divisionItems.size()) return;
    size_t firstIndex = out.indices.size();
    const int heightShift = 0x200 - int(g.heightBase);
    const float r = 0.70710677f;
    for (uint32_t i : tc->divisionItems[size_t(index)]) {
      const GridData::Item& it = g.items[i];
      int ti = it.type < tc->typeTemplate.size() ? tc->typeTemplate[it.type] : -1;
      if (it.removed || ti < 0) continue;
      const GridTemplate& tp = tc->templates[size_t(ti)];
      uint32_t flags = it.flags | tp.flags;
      if (!(flags & 1) || (flags & 0x400000)) continue;  // ENABLE_BASE, not hidden
      if (!gridItemVisible(g, *tc, i, view)) continue;
      ItemFootprint fp = itemFootprint(tp.width, tp.length, tp.diagonals, it.orient);
      const int x0 = it.x, z0 = it.z, sx = fp.sx, sz = fp.sz, y = it.y + heightShift;
      // fixY: inside the map, the min and max of the footprint's (sx+1)·(sz+1) points against y
      if (x0 < 0 || z0 < 0 || x0 + sx > W() || z0 + sz > L()) continue;
      int mx = std::min(sx, W() - x0 - 1), mz = std::min(sz, L() - z0 - 1), lo = 0x7fff, hi = -0x8000;
      for (int z = 0; z <= mz; ++z)
        for (int x = 0; x <= mx; ++x) {
          int hv = g.height[size_t((z0 + z) * W() + x0 + x)];
          lo = std::min(lo, hv), hi = std::max(hi, hv);
        }
      if (lo == hi && lo == y) continue;
      // debugging: REGS_DEBUG_BASE=1 lists the walled items (world x/z of the footprint centre for the cam:x,z test action)
      if (static const bool debug = std::getenv("REGS_DEBUG_BASE") != nullptr; debug)
        LOG_INFO("base walls: item %u %s at %d,%d size %dx%d y %d ground %d..%d world %.0f,%.0f", i, g.types[it.type].c_str(),
                 x0, z0, sx, sz, y, lo, hi, (x0 + sx * 0.5f - W() * 0.5f) * cs, (z0 + sz * 0.5f - L() * 0.5f) * cs);
      auto vertex = [&](int gx, int gz, int hy, const Vec3& normal, bool diagonal) {
        Corner c;
        int lx = gx - dx, lz = gz - dz;
        c.pos = {lx * cs - n * cs * 0.5f, hy * cs / 256.0f, lz * cs - n * cs * 0.5f};
        c.normal = normal;
        c.u = diagonal ? float(lz) * 0.35355338f : float(lx + lz) * 0.25f;
        c.v = float(hy) * -0.0009765625f;
        c.color = 0xFF808080u;
        push(c);
      };
      auto strip = [&](int count, auto point, Vec3 sunk, Vec3 raised, bool diagonal) {
        if (count <= 0) return;
        uint32_t base = uint32_t(out.vertexCount());
        for (int k = 0; k <= count; ++k) {
          int gx = 0, gz = 0;
          point(k, gx, gz);
          int ground = h(gx, gz);
          const Vec3& normal = y < ground ? sunk : raised;
          vertex(gx, gz, ground, normal, diagonal);
          vertex(gx, gz, y, normal, diagonal);
        }
        for (int k = 0; k < count; ++k) {
          uint32_t b = base + uint32_t(k) * 2;
          out.indices.insert(out.indices.end(), {b, b + 1, b + 2, b + 2, b + 1, b + 3});
        }
      };
      const int d0 = int(fp.diag & 0xF), d1 = int(fp.diag >> 4 & 0xF), d2 = int(fp.diag >> 8 & 0xF), d3 = int(fp.diag >> 12 & 0xF);
      strip(d0, [&](int k, int& x, int& z) { x = x0 + sx - d0 + k, z = z0 + k; }, {-r, 0, r}, {r, 0, -r}, true);
      strip(d1, [&](int k, int& x, int& z) { x = x0 + sx - k, z = z0 + sz - d1 + k; }, {-r, 0, -r}, {r, 0, r}, true);
      strip(d2, [&](int k, int& x, int& z) { x = x0 + d2 - k, z = z0 + sz - k; }, {r, 0, -r}, {-r, 0, r}, true);
      strip(d3, [&](int k, int& x, int& z) { x = x0 + k, z = z0 + d3 - k; }, {r, 0, r}, {-r, 0, -r}, true);
      strip(sx - d3 - d0, [&](int k, int& x, int& z) { x = x0 + d3 + k, z = z0; }, {0, 0, 1}, {0, 0, -1}, false);
      strip(sz - d1 - d0, [&](int k, int& x, int& z) { x = x0 + sx, z = z0 + d0 + k; }, {-1, 0, 0}, {1, 0, 0}, false);
      strip(sx - d2 - d1, [&](int k, int& x, int& z) { x = x0 + sx - d1 - k, z = z0 + sz; }, {0, 0, -1}, {0, 0, 1}, false);
      strip(sz - d3 - d2, [&](int k, int& x, int& z) { x = x0, z = z0 + sz - d2 - k; }, {1, 0, 0}, {-1, 0, 0}, false);
    }
    finishGroup(kBase, firstIndex);
  }

  void build(int lod) {
    corners.resize(size_t((n + 1) * (n + 1)));
    for (int lz = 0; lz <= n; ++lz)
      for (int lx = 0; lx <= n; ++lx) corners[size_t(lz * (n + 1) + lx)] = makeCorner(dx + lx, dz + lz);
    // view bits (§6): 2 = the terrain surface (yards and fields only with 0x20, else all earth), else the
    // underground earth plane; 4 = water. Port: the plane in both LODs (the original builds it in lod 0 only).
    if (view & 2) {
      if (view & 0x20)
        for (int group = kTerrain; group < kWater; ++group) surfaceGroup(group, lod);
      else
        surfaceGroup(kEarth, lod);
    } else {
      earthPlane();
    }
    if (view & 4) water();
    edges();
    waterEdges();
    baseWalls();
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
  else if (group == kEarth) key = u"earthMaterial";
  else if (group == kBase) key = u"baseMaterial";
  else if (group == kWaterEdge) key = u"waterEdgeMaterial";
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

// a division's terrain mesh in its view mode; a tunnel division of the underground view walls off the sides towards
// divisions without tunnels
void buildTerrain(const GridData& g, const TerrainCache& tc, int index, int lod, MeshData& out) {
  Builder b{g, tc.n, (index % tc.cols) * tc.n, (index / tc.cols) * tc.n, g.cellSize, {}, {}};
  b.view = divisionViewMode(tc, index);
  b.tc = &tc;
  b.index = index;
  if (b.view == 0x180) {
    int c = index % tc.cols, r = index / tc.cols;
    int nb[4] = {r > 0 ? index - tc.cols : -1, r + 1 < tc.rows ? index + tc.cols : -1, c > 0 ? index - 1 : -1,
                 c + 1 < tc.cols ? index + 1 : -1};
    for (int k = 0; k < 4; ++k) b.inner[k] = nb[k] >= 0 && !tc.tunnelDivision[size_t(nb[k])];
  }
  b.build(lod);
  out = std::move(b.out);
}

// gsGridDivision +0x30 (addObject L:0x08163f80.c:41-75, removeObject L:0x081643b0.c:190-215): the divisions a TUNNEL
// item's footprint grown by one cell overlaps, for every item in the map (hidden ones and tool previews too)
std::vector<uint8_t> tunnelDivisions(const GridData& g, const TerrainCache& tc) {
  std::vector<uint8_t> out(tc.divisions.size(), 0);
  const int shift = int(std::min(g.bucketShift, 8u));
  for (const GridData::Item& it : g.items) {
    int ti = it.type < tc.typeTemplate.size() ? tc.typeTemplate[it.type] : -1;
    if (it.removed || ti < 0) continue;
    const GridTemplate& tp = tc.templates[size_t(ti)];
    if (!((it.flags | tp.flags) & 0x8000)) continue;
    int sx = (it.orient & 1) ? tp.length : tp.width, sz = (it.orient & 1) ? tp.width : tp.length;
    int c0 = std::max(0, (it.x - 1) >> shift), c1 = std::min(tc.cols - 1, (it.x + 1 + sx) >> shift);
    int r0 = std::max(0, (it.z - 1) >> shift), r1 = std::min(tc.rows - 1, (it.z + 1 + sz) >> shift);
    for (int r = r0; r <= r1; ++r)
      for (int c = c0; c <= c1; ++c) out[size_t(r * tc.cols + c)] = 1;
  }
  return out;
}

// gsGridItem::setColor(colour, recurse) (L:0x08181de0.c:29-97) for the requests of gsGridItemController.setColor: the
// item's tint, then with recurse every other item in the divisions from one before to one after the root's footprint
// whose root is the same root (or which is the root itself) and whose template has no colour map (template +0x314).
// The tint is the colorize colour of the item's COLORIZE parts in the merged division mesh, so the division rebuilds.
void applyItemColors(GridObj& grid, TerrainCache& tc) {
  if (grid.itemColorRequests.empty()) return;
  const GridData& g = *grid.data;
  tc.itemColor.resize(g.items.size(), 0);
  auto paint = [&](uint32_t i, uint32_t argb) {
    if (i >= g.items.size() || g.items[i].removed || tc.itemColor[i] == argb) return;
    tc.itemColor[i] = argb;
    if (int d = itemDivision(g, tc, i); d >= 0) tc.dirtyItems.insert(d);
  };
  auto rootOf = [&](uint32_t i) {
    for (int guard = 0; guard < 64 && g.items[i].parent && g.items[i].parent <= g.items.size(); ++guard) i = g.items[i].parent - 1;
    return i;
  };
  const int shift = int(std::min(g.bucketShift, 8u));
  for (const GridObj::ItemColorRequest& r : grid.itemColorRequests) {
    if (r.item >= g.items.size()) continue;
    paint(r.item, r.argb);
    if (!r.recurse) continue;
    uint32_t root = rootOf(r.item);
    const GridData::Item& ri = g.items[root];
    int ti = ri.type < tc.typeTemplate.size() ? tc.typeTemplate[ri.type] : -1;
    int sx = 1, sz = 1;
    if (ti >= 0) {
      const GridTemplate& tp = tc.templates[size_t(ti)];
      sx = (ri.orient & 1) ? tp.length : tp.width, sz = (ri.orient & 1) ? tp.width : tp.length;
    }
    int c0 = std::max(0, (ri.x >> shift) - 1), c1 = std::min(tc.cols - 1, ((ri.x + sx - 1) >> shift) + 1);
    int r0 = std::max(0, (ri.z >> shift) - 1), r1 = std::min(tc.rows - 1, ((ri.z + sz - 1) >> shift) + 1);
    for (int dz = r0; dz <= r1; ++dz)
      for (int dx = c0; dx <= c1; ++dx)
        for (uint32_t i : tc.divisionItems[size_t(dz * tc.cols + dx)]) {
          if (i == r.item || g.items[i].removed || (i != root && rootOf(i) != root)) continue;
          int t = g.items[i].type < tc.typeTemplate.size() ? tc.typeTemplate[g.items[i].type] : -1;
          if (t < 0 || tc.templates[size_t(t)].colorMap) continue;
          paint(i, r.argb);
        }
  }
  grid.itemColorRequests.clear();
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

// the map's cache (divisions, materials, grid items, surface map), rebuilt when the map changed; collectGrid
// calls it, and regsPrewarmGrid() on the loading screen so the first frame after it does not wait for it
void ensureGridCache(Display& d, VM& vm, GridObj* grid) {
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
    for (int group = 0; group < kGroupCount; ++group) {
      tc.materials.push_back(scriptMaterial(d, vm, gridMaterialObject(vm, grid, group)));
      // the surface class for the mods' shader hooks (renderer3d.h)
      tc.materials.back().surfaceClass = group == kTerrain ? kSurfaceTerrain
                                         : group >= kYard && group < kField ? kSurfacePavement
                                         : group >= kField && group < kWater ? kSurfaceField
                                         : group == kWater ? kSurfaceWater : kSurfaceOther;
    }
    // the tool highlights (terrain.md §5: positiveMaterial / negativeMaterial, selection.shader decals)
    // and the data layer of the catchment circles (dataMaterial, selection/data.shader decal)
    for (const char16_t* key : {u"positiveMaterial", u"negativeMaterial", u"dataMaterial"}) {
      Object* m = nullptr;
      try {
        m = vm.resolve(vm.getMember(Value::heap(VT::Object, grid), key)).object();
      } catch (const ScriptError&) {
      }
      (key[0] == u'p' ? tc.positiveMaterial : key[0] == u'n' ? tc.negativeMaterial : tc.dataMaterial) = int(tc.materials.size());
      tc.materials.push_back(scriptMaterial(d, vm, m));
      tc.materials.back().surfaceClass = kSurfaceOverlay;  // highlights and the catchment layer: never hooked
    }
    initGridItems(d, vm, grid, tc);
    fixGridCover(vm, *grid->data, tc, grid->viewMode);
    if (!shaderHooks().empty()) {  // the surface map for the mods' shader hooks: the divisions' square
      tc.surfaceMap = std::make_shared<SurfaceMap>();
      const GridDivision& first = tc.divisions[0];
      surfaceMapInit(*tc.surfaceMap, first.center.x - tc.n * cs * 0.5f, first.center.z - tc.n * cs * 0.5f,
                     tc.cols * tc.n * cs, tc.rows * tc.n * cs);
    }
    LOG_INFO("terrain: %dx%d divisions of %d cells", tc.cols, tc.rows, tc.n);
    if (std::getenv("REGS_DEBUG_BASE")) {  // every division once, for the walled items' list
      for (int index = 0; index < int(tc.divisions.size()); ++index) {
        MeshData scratch;
        buildTerrain(g, tc, index, 1, scratch);
      }
    }
  }
}

// port curved roads (engine/curve_roads.h): every curve in one world-space mesh of the small road's own materials
// (road_small_basic: asphalt, markings, walkways), rebuilt when they change, and the curve tool's preview tinted green
// (fits) or red; the surface map learns their asphalt and pavements for the mods' shader hooks
void collectCurves(Display& d, VM& vm, GridObj* grid, TerrainCache& tc, std::vector<DrawItem>& items) {
  GridData& g = *grid->data;
  if (g.curves.empty() && !grid->hasCurvePreview && !tc.curvesBuilt && !tc.curvePreviewBuilt) return;
  if (tc.curvesVersion != grid->curvesVersion) {
    tc.curvesVersion = grid->curvesVersion;
    if (tc.curvesBuilt) d.r3d().destroy(tc.curves);
    if (tc.curvePreviewBuilt) d.r3d().destroy(tc.curvePreview);
    tc.curvesBuilt = tc.curvePreviewBuilt = false;
    // a template's first material in tc.materials and its object (its type joins the map when none of its items is
    // there yet: a map with curves but no small road or tram track)
    auto templateOf = [&](const std::string& name, ObjectObj** object) {
      int type = -1;
      for (size_t t = 0; t < g.types.size() && type < 0; ++t)
        if (g.types[t] == name) type = int(t);
      if (type < 0 && grid->templates.count(name)) {
        g.types.push_back(name);
        type = int(g.types.size()) - 1;
      }
      syncGridItems(d, vm, grid, tc);
      int ti = type >= 0 && size_t(type) < tc.typeTemplate.size() ? tc.typeTemplate[size_t(type)] : -1;
      if (object) *object = ti >= 0 ? tc.templates[size_t(ti)].object : nullptr;
      return ti >= 0 ? tc.templates[size_t(ti)].materialBase : -1;
    };
    // every template the curves draw joins the map's types before any mesh is built: a type added later rebuilds the
    // template table, and the material bases taken before it then point at other materials (two bridges, the second
    // with pillars: the first span drew with the wrong materials, no railings)
    {
      auto need = [&](const char* name) {
        if (name && name[0]) templateOf(name, nullptr);
      };
      auto needCurve = [&](const CurveRoad& c) {
        const CurveProfile& pf = curveProfile(c.kind);
        need(pf.straight), need(pf.deadend), need(pf.pillar);
        if (c.ends & 4) need(pf.tram);
        if (c.bridge) need(curveBridgePiece(c.kind)), need(curveBridgePillar(c.kind));
        for (const CurveRoad::Stop& st : c.stops) need(stopTemplateName(st).c_str());
      };
      need("road_small_basic"), need("tram_small_basic"), need(curveProfile(3).straight);
      for (const CurveRoad& c : g.curves) needCurve(c);
      if (grid->hasCurvePreview) needCurve(grid->curvePreview);
    }
    int base = templateOf("road_small_basic", nullptr);
    if (base < 0) {
      LOG_WARN("curved roads: no template road_small_basic, not drawn");
    } else {
      bool tram = false;
      for (const CurveRoad& c : g.curves) tram = tram || (c.ends & 4);
      if (grid->hasCurvePreview) tram = tram || (grid->curvePreview.ends & 4);
      int tramBase = tram ? templateOf("tram_small_basic", nullptr) : -1;
      auto upload = [&](MeshData& m, GpuMesh& out, bool& built) {
        if (!m.vertexCount()) return;
        render::computeTangents(m);
        m.radius = 1e6f;
        out = d.r3d().upload(m);
        built = true;
      };
      // a curve with its stops: their stop templates' meshes bent along the curve beside the road (the anchor item's
      // centre 3 units out, as on a straight block)
      // the other kinds: copies of their straight piece (cut into 1.5-unit slabs once) bent along the curve, the
      // street tram piece's likewise when it has tracks, and their dead-end piece at a free end
      static std::map<const render::ObjectData*, MeshData> sliced;
      auto slicedOf = [&](const render::ObjectData* o) -> const MeshData& {
        auto it = sliced.find(o);
        if (it == sliced.end()) it = sliced.emplace(o, sliceMeshAlongX(o->mesh, 1.5f)).first;
        return it->second;
      };
      auto bentRoad = [&](const CurveRoad& c, MeshData& m, float t0, float t1, const char* straight = nullptr) {
        const CurveProfile& pf = curveProfile(c.kind);
        const float kept = curveLength(c) - t0 - t1;
        if (kept <= 0.05f) return;
        CurvePieces cp = curvePieces(c, kept);
        std::vector<CurveSample> shape = curveShape(c);
        auto pieces = [&](const char* name) {
          ObjectObj* o = nullptr;
          int b = name[0] ? templateOf(name, &o) : -1;
          if (b < 0 || !o || !o->data) return;
          const MeshData& sm = slicedOf(o->data.get());
          for (int i = 0; i < cp.count; ++i)
            appendBentMesh(sm, b, shape, t0 + (float(i) + 0.5f) * pf.piece * cp.stretch, 1.0f, 0.0f, m, cp.stretch);
        };
        const size_t before = m.positions.size();
        pieces(straight ? straight : pf.straight);
        static const bool debugBridge = std::getenv("REGS_DEBUG_CURVES") != nullptr;
        if (debugBridge && straight) {  // the span's drawn extent and the live ground under the curve's middle
          float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
          for (size_t k = before; k + 2 < m.positions.size(); k += 3)
            for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], m.positions[k + a]), hi[a] = std::max(hi[a], m.positions[k + a]);
          CurveSample mid = curveAt(shape, t0 + kept * 0.5f);
          int cx = int(std::floor(mid.p.x / g.cellSize + float(g.width) * 0.5f)), cz = int(std::floor(mid.p.z / g.cellSize + float(g.length) * 0.5f));
          float ground = cx >= 0 && cz >= 0 && cx < g.width && cz < g.length
                             ? float(g.height[size_t(cz) * size_t(g.width) + size_t(cx)]) * g.cellSize / 256.0f : 0.0f;
          LOG_INFO("curves: span %s, %zu vertices, x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f", straight,
                   (m.positions.size() - before) / 3, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
          LOG_INFO("curves:   middle %.1f %.1f %.1f, ground %.1f", mid.p.x, mid.p.y, mid.p.z, ground);
          for (float s = 0; s <= shape.back().s + 0.01f; s += 2.0f) {  // the highest corner of each cell along it
            CurveSample k = curveAt(shape, s);
            int kx = int(std::floor(k.p.x / g.cellSize + float(g.width) * 0.5f)), kz = int(std::floor(k.p.z / g.cellSize + float(g.length) * 0.5f));
            int top = -0x8000;
            for (int dz = 0; dz <= 1; ++dz)
              for (int dx = 0; dx <= 1; ++dx)
                if (kx + dx >= 0 && kz + dz >= 0 && kx + dx < g.width && kz + dz < g.length)
                  top = std::max(top, int(g.height[size_t(kz + dz) * size_t(g.width) + size_t(kx + dx)]));
            uint16_t fl = kx >= 0 && kz >= 0 && kx < g.width && kz < g.length ? g.flags[size_t(kz) * size_t(g.width) + size_t(kx)] : 0;
            LOG_INFO("curves:   s %4.1f cell %d,%d road %.1f ground top %.1f flags %04x", s, kx, kz, k.p.y, float(top) * g.cellSize / 256.0f, fl);
          }
        }
        if (c.ends & 4) pieces(pf.tram);
        if (straight && curveBridgePillar(c.kind)[0]) {  // a bridge: its pillar under every 24 units (between the ends),
                                                         // its foot 8 cells under the deck as the span's own parts
          ObjectObj* po = nullptr;
          int pb = templateOf(curveBridgePillar(c.kind), &po);
          const float every = 24.0f;
          const int count = int(kept / every);
          if (pb >= 0 && po && po->data)
            for (int i = 1; i <= count; ++i) {
              float s = t0 + kept * float(i) / float(count + 1);
              CurveSample k = curveAt(shape, s);
              Vec3 ax = render::normalize(Vec3{k.dir.x, 0, k.dir.z});
              appendPlacedMesh(po->data->mesh, pb, k.p + Vec3{0, -24.0f, 0}, ax, k.right, m);
            }
        }
        if (pf.pillar[0]) {  // elevated tracks: the straight piece's pillar parts (metroe_basic onPlace: a half on each
                             // side, one cell under the track, at both ends of a piece) under every joint
          ObjectObj* po = nullptr;
          int pb = templateOf(pf.pillar, &po);
          if (pb >= 0 && po && po->data)
            for (int i = 0; i <= cp.count; ++i) {
              CurveSample k = curveAt(shape, t0 + float(i) * pf.piece * cp.stretch);
              Vec3 ax = render::normalize(Vec3{k.dir.x, 0, k.dir.z}), az = k.right, down{0, -3.0f, 0};
              appendPlacedMesh(po->data->mesh, pb, k.p + az * 3.0f + down, ax, az, m);
              appendPlacedMesh(po->data->mesh, pb, k.p - az * 3.0f + down, ax * -1.0f, az * -1.0f, m);
            }
        }
        CurveEnds caps = curveFreeEnds(g, c);
        ObjectObj* dob = nullptr;
        int db = templateOf(pf.deadend, &dob);
        if (db < 0 || !dob || !dob->data) return;
        auto cf = dob->fieldValues.find(u"connections");
        uint32_t conn = cf == dob->fieldValues.end() || cf->second.isNone() ? 0x40u << pf.shift : uint32_t(vm.toInt(cf->second));
        bool openPlusX = ((conn >> pf.shift) & 0x40) != 0;
        for (int end = 0; end < 2; ++end) {
          if (!(end ? caps.free1 : caps.free0)) continue;
          Vec3 o, ax, az;
          curveCapFrame(c, end, openPlusX, o, ax, az);
          appendPlacedMesh(dob->data->mesh, db, o, ax, az, m);
        }
      };
      auto curveMesh = [&](const CurveRoad& c, MeshData& m, float t0, float t1, bool noCaps = false) {
        if (c.bridge && curveBridgePiece(c.kind)[0]) {  // a bridge: the kind's bridge span bent along it
          // shorter than 3/4 of a span piece (after the junctions' trims): the plain road (a span squeezed that much
          // shows its girders instead of a deck)
          const float kept = curveLength(c) - t0 - t1, piece = curveProfile(c.kind).piece;
          static const bool debugCurves = std::getenv("REGS_DEBUG_CURVES") != nullptr;
          if (debugCurves)
            LOG_INFO("curves: bridge kind %d from %.1f %.1f %.1f to %.1f %.1f %.1f, length %.1f, trims %.1f / %.1f, %s",
                     c.kind, c.x0, c.y0, c.z0, c.x1, c.y1, c.z1, curveLength(c), t0, t1,
                     kept >= 0.75f * piece ? "span" : "plain road (short)");
          if (kept >= 0.75f * piece) {
            bentRoad(c, m, t0, t1, curveBridgePiece(c.kind));
            return;
          }
        }
        if (c.kind != 0) {
          bentRoad(c, m, t0, t1);
          return;
        }
        bool meet0 = false, meet1 = false;
        if (!noCaps) curveOneWayMeets(g, c, t0, t1, meet0, meet1);
        buildCurveMesh(c, noCaps ? CurveEnds{} : curveFreeEnds(g, c), base, base + 1, base + 2, m, tramBase, t0, t1, meet0, meet1);
        if (c.stops.empty()) return;
        std::vector<CurveSample> shape = curveShape(c);
        for (const CurveRoad::Stop& st : c.stops) {
          ObjectObj* o = nullptr;
          int sb = templateOf(stopTemplateName(st), &o);
          if (sb >= 0 && o && o->data) appendBentMesh(o->data->mesh, sb, shape, st.s, float(st.side), 3.0f, m);
        }
      };
      MeshData all;
      std::vector<CurveJunction> junctions = findCurveJunctions(g);
      for (size_t i = 0; i < g.curves.size(); ++i) {
        float t0, t1;
        curveTrims(junctions, int(i), t0, t1);
        curveMesh(g.curves[i], all, t0, t1);
      }
      // junctions: roads fill with the small road's asphalt (one texture for all road kinds) and its pavement, footpaths
      // with the footpath; the fill's texture plane is fitted to the template's own mesh once
      static std::map<std::string, PlanarUV> fills;
      auto fillOf = [&](const char* name, int material, int& base0) -> const PlanarUV& {
        ObjectObj* o = nullptr;
        base0 = templateOf(name, &o);
        auto it = fills.find(name);
        if (it == fills.end() && o && o->data) it = fills.emplace(name, fitPlanarUV(o->data->mesh, material)).first;
        static const PlanarUV none;
        return it == fills.end() ? none : it->second;
      };
      for (const CurveJunction& j : junctions) {
        int fb = -1;
        if (j.kind == 3) {
          const PlanarUV& uv = fillOf(curveProfile(3).straight, 0, fb);
          buildJunctionMesh(j, fb, uv, -1, all);
        } else {
          const PlanarUV& uv = fillOf("road_small_basic", 0, fb);
          buildJunctionMesh(j, fb, uv, base + 2, all, base + 1, tramBase);
        }
      }
      if (tc.surfaceMap && all.vertexCount()) surfaceMapPaint(*tc.surfaceMap, all, Mat4(), tc.materials, true);
      upload(all, tc.curves, tc.curvesBuilt);
      if (grid->hasCurvePreview) {
        MeshData p;
        bool ring = !grid->curvePreviewMore.empty();
        curveMesh(grid->curvePreview, p, 0.0f, 0.0f, ring);
        for (const CurveRoad& more : grid->curvePreviewMore) curveMesh(more, p, 0.0f, 0.0f, true);
        // a little above the map: a preview of a road already there (the bulldozer's, the remove mode's) shows over it
        for (size_t v = 1; v < p.positions.size(); v += 3) p.positions[v] += 0.04f;
        upload(p, tc.curvePreview, tc.curvePreviewBuilt);
      }
    }
  }
  // the underground view (viewMode 0x180) hides the surface: curves are all surface roads and tracks (no subway curves)
  if (tc.viewMode == 0x180) return;
  if (tc.curvesBuilt && !tc.curves.parts.empty()) {
    DrawItem it;
    it.mesh = &tc.curves;
    it.materials = &tc.materials;
    items.push_back(it);
  }
  if (grid->hasCurvePreview && tc.curvePreviewBuilt && !tc.curvePreview.parts.empty()) {
    DrawItem it;
    it.mesh = &tc.curvePreview;
    it.materials = &tc.materials;
    const float good[4] = {0.75f, 1.0f, 0.75f, 1.0f}, bad[4] = {1.0f, 0.35f, 0.3f, 1.0f};
    for (int k = 0; k < 4; ++k) it.colorize[k] = grid->curvePreviewBad ? bad[k] : good[k];
    items.push_back(it);
  }
}

void collectGrid(Display& d, VM& vm, GridObj* grid, const Vec3& eye, const Mat4& viewProj, float farZ, float viewScale,
                 float detailScale, std::vector<DrawItem>& items) {
  if (!grid->data || grid->data->height.empty()) return;
  ensureGridCache(d, vm, grid);
  const GridData& g = *grid->data;
  if (!grid->terrain) grid->terrain = std::make_shared<TerrainCache>();
  TerrainCache& tc = *grid->terrain;
  float cs = g.cellSize;
  // live map edits (engine/grid_edit.h): new types and items, then the touched divisions rebuilt at once where built
  syncGridItems(d, vm, grid, tc);
  applyItemColors(*grid, tc);
  // the view mode (docs/renderer/underground.md): a change in bits 0xfe rebuilds every division (write_viewMode); the
  // data-view bits do not change the division meshes in the port
  if (tc.viewVersion != grid->viewVersion) {
    tc.viewVersion = grid->viewVersion;
    tc.viewMode = grid->viewMode & ~0xF7000u;
    for (GridDivision& dv : tc.divisions)
      for (int k = 0; k < 2; ++k) {
        if (dv.terrainBuilt[k]) d.r3d().destroy(dv.terrain[k]);
        if (dv.itemsBuilt[k]) d.r3d().destroy(dv.items[k]);
        dv.terrainBuilt[k] = dv.itemsBuilt[k] = false;
      }
    tc.dirtyTerrain.clear();
    tc.dirtyItems.clear();
    tc.tunnelDivision.clear();
    tc.tunnelItemsVersion = ~0ull;
  }
  // underground: the tunnel divisions, recounted after edits (tool previews included, as addObject/removeObject do);
  // a division that gains or loses tunnels is rebuilt with its items, its neighbours for their inward walls
  if (tc.viewMode == 0x180 && tc.tunnelItemsVersion != grid->itemsVersion) {
    std::vector<uint8_t> now = tunnelDivisions(g, tc);
    if (tc.tunnelDivision.size() == now.size())
      for (int index = 0; index < int(now.size()); ++index) {
        if (now[size_t(index)] == tc.tunnelDivision[size_t(index)]) continue;
        tc.dirtyTerrain.insert(index);
        tc.dirtyItems.insert(index);
        int c = index % tc.cols, r = index / tc.cols;
        if (r > 0) tc.dirtyTerrain.insert(index - tc.cols);
        if (r + 1 < tc.rows) tc.dirtyTerrain.insert(index + tc.cols);
        if (c > 0) tc.dirtyTerrain.insert(index - 1);
        if (c + 1 < tc.cols) tc.dirtyTerrain.insert(index + 1);
      }
    tc.tunnelDivision = std::move(now);
    tc.tunnelItemsVersion = grid->itemsVersion;
  }
  if (!tc.dirtyTerrain.empty() || !tc.dirtyItems.empty()) {
    for (int index : tc.dirtyTerrain) {
      if (index < 0 || size_t(index) >= tc.divisions.size()) continue;
      GridDivision& dv = tc.divisions[size_t(index)];
      for (int lod = 0; lod < 2; ++lod) {
        if (!dv.terrainBuilt[lod]) continue;
        d.r3d().destroy(dv.terrain[lod]);
        MeshData out;
        buildTerrain(g, tc, index, lod, out);
        dv.terrain[lod] = d.r3d().upload(out);
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
      MeshData out;
      buildTerrain(g, tc, index, lod, out);
      dv.terrain[lod] = d.r3d().upload(out);
      dv.terrainBuilt[lod] = true;
      if (tc.surfaceMap) surfaceMapPaint(*tc.surfaceMap, out, dv.world, tc.materials, false);  // mods' shader hooks
    }
    if (!dv.itemsBuilt[lod]) {
      MeshData out;
      buildGridItems(g, tc, index, lod, out);
      dv.items[lod] = d.r3d().upload(out);
      dv.itemsBuilt[lod] = true;
      if (tc.surfaceMap) surfaceMapPaint(*tc.surfaceMap, out, dv.world, tc.materials, true);
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
  // the catchment circles of the visible stop controllers (gsGrid::addAreaHighlight L:0x08142940): a circle belongs to
  // the divisions its square grown by 4 cells overlaps; each such division draws a data layer over its layerable parts
  // with the 8 circles nearest to its centre (gsGridDivision::updateDataLayer L:0x081635d0)
  std::vector<StopCircle> circles;
  visibleStopCircles(vm, grid, circles);
  auto layerFor = [&](int index, GridDivision& dv) {
    if (circles.empty() || tc.dataMaterial < 0) return false;
    const int shift = int(std::min(g.bucketShift, 8u));
    const int c = index % tc.cols, r = index / tc.cols;
    std::vector<std::pair<float, const StopCircle*>> near;
    for (const StopCircle& k : circles) {
      float hx = g.width * cs * 0.5f, hz = g.length * cs * 0.5f;
      int c0 = (int(std::lround((k.x - k.radius + hx) / cs)) - 4) >> shift, c1 = (int(std::lround((k.x + k.radius + hx) / cs)) + 4) >> shift;
      int r0 = (int(std::lround((k.z - k.radius + hz) / cs)) - 4) >> shift, r1 = (int(std::lround((k.z + k.radius + hz) / cs)) + 4) >> shift;
      if (c < c0 || c > c1 || r < r0 || r > r1) continue;
      float ex = k.x - dv.center.x, ez = k.z - dv.center.z;
      near.push_back({ex * ex + ez * ez, &k});
    }
    if (near.empty()) return false;
    std::stable_sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (int i = 0; i < 8; ++i) {
      float* p = dv.layerCustom[i];
      if (size_t(i) < near.size()) p[0] = near[size_t(i)].second->x, p[1] = 0, p[2] = near[size_t(i)].second->z, p[3] = near[size_t(i)].second->radius;
      else p[0] = p[1] = p[2] = 0, p[3] = -1000.0f;
    }
    return true;
  };
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
      const bool layer = layerFor(index, dv);
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
        if (layer && !it.shadowOnly) {
          it.layerMaterial = tc.dataMaterial;
          it.layerCustom = dv.layerCustom;
          items.push_back(it);
        }
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
    it.surfaceClass = kSurfaceOverlay;  // tool highlights keep their colours under the mods' shader hooks
    items.push_back(it);
  }
  collectCurves(d, vm, grid, tc, items);  // port curved roads
  std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) { return a.dist < b.dist; });
  if (pending.size() > size_t(kItemBuildsPerFrame)) pending.resize(size_t(kItemBuildsPerFrame));
  for (const Pending& p : pending) build(tc.divisions[size_t(p.division)], p.division, p.lod);  // shown from the next frame
  if (tc.surfaceMap) surfaceMapUpload(*tc.surfaceMap);
}

bool gridSurfaceMap(GridObj* grid, uint32_t& texture, float rect[4], uint32_t* topTexture) {
  if (!grid || !grid->terrain || !grid->terrain->surfaceMap || !grid->terrain->surfaceMap->texture) return false;
  const SurfaceMap& m = *grid->terrain->surfaceMap;
  texture = m.texture;
  if (topTexture) *topTexture = m.topTexture;
  rect[0] = m.originX;
  rect[1] = m.originZ;
  rect[2] = 1.0f / (float(m.w) * m.texel);
  rect[3] = 1.0f / (float(m.h) * m.texel);
  return true;
}

void registerTerrainNatives(VM& vm) {
  // regsPrewarmGrid(): builds the main map's cache (materials, textures, window masks, grid items) now, on the loading
  // screen before its Spielen button (data/regs/extend/metro/ui/loading/loading.script), instead of in the first
  // frame after it, which stayed black for seconds
  implementGlobal(vm, u"regsPrewarmGrid", [](VM& v, Object*, int, const Value*) {
    Display* d = display();
    Value* main = v.findGlobal(u"MAIN");
    Object* m = main ? main->object() : nullptr;
    Value* g = m ? m->findVar(u"grid") : nullptr;
    auto* grid = g ? dynamic_cast<GridObj*>(v.resolve(*g).object()) : nullptr;
    if (d && grid && grid->data) {
      auto t0 = std::chrono::steady_clock::now();
      ensureGridCache(*d, v, grid);
      auto t1 = std::chrono::steady_clock::now();
      // the first view drawn once, unseen (the loading screen covers it): its shader programs compile now; with a
      // cold driver cache (a new driver, changed shaders) that took half a minute of black screen after Spielen
      const int linked = render::gProgramsLinked;
      Value* vpv = m->findVar(u"viewport");
      Value* scene = m->findVar(u"scene");
      Value* camera = m->findVar(u"camera");
      auto* vp = vpv ? dynamic_cast<ControlObj*>(v.resolve(*vpv).object()) : nullptr;
      if (vp && scene && camera) {  // the game state gives the viewport its scene and camera only after Spielen
        Value self = Value::heap(VT::Object, vp);
        Value oldScene = v.getMember(self, u"scene"), oldCamera = v.getMember(self, u"camera");
        try {
          v.setMember(self, u"scene", v.resolve(*scene));
          v.setMember(self, u"camera", v.resolve(*camera));
          renderViewportScene(*d, v, vp);
          v.setMember(self, u"scene", oldScene);
          v.setMember(self, u"camera", oldCamera);
        } catch (const ScriptError&) {
        }
      }
      auto t2 = std::chrono::steady_clock::now();
      LOG_INFO("grid cache prepared on the loading screen in %.0f ms, first view drawn ahead in %.0f ms (%d shader programs)",
               std::chrono::duration<double, std::milli>(t1 - t0).count(),
               std::chrono::duration<double, std::milli>(t2 - t1).count(), render::gProgramsLinked - linked);
    }
    return Value();
  });
}

}  // namespace regs::engine
