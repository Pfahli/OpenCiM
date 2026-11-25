// The in-game minimap after docs/ui/minimap.md: gsGrid::renderMap paints the map texture from a fixed 256×256 raster
// of per-pixel counters (§2-3), gsMapCtrl maps world ↔ control pixels and shifts the image through minimap.shader's
// custom0 (§4), gsMapViewportCtrl draws the main camera's ground footprint (§5) and gsMapItemCtrl centres pins on
// their targets (§6). The transport marks (raster bytes 0-5) come from the live stops and line legs (§2.4). Not yet:
// the selected-line ribbon (gsMapRouteCtrl, §7); the traffic and popularity data views read 0.
#include "engine/minimap.h"
#include "core/env.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/curve_roads.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/grid_render.h"
#include "engine/gui.h"
#include "engine/scene3d.h"
#include "engine/sim_state.h"
#include "render/image.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

constexpr int kTypeTree = 0x13, kTypeBuilding = 0x14;
constexpr uint32_t kFlagHidden = 0x400000;

int intField(VM& vm, const Object* o, const char16_t* name, int def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toInt(it->second);
}

float floatField(VM& vm, Object* o, const char16_t* name, float def) {
  try {
    Value v = vm.resolve(vm.getMember(Value::heap(VT::Object, o), name));
    return v.type() == VT::Float || v.type() == VT::Int ? vm.toFloat(v) : def;
  } catch (const ScriptError&) {
    return def;
  }
}

Vec3 vecField(VM& vm, Object* o, const char16_t* name) {
  try {
    Value v = vm.resolve(vm.getMember(Value::heap(VT::Object, o), name));
    if (v.type() == VT::Vector) return {v.vec()[0], v.vec()[1], v.vec()[2]};
  } catch (const ScriptError&) {
  }
  return {0, 0, 0};
}

Object* objField(VM& vm, Object* o, const char16_t* name) {
  try {
    return vm.resolve(vm.getMember(Value::heap(VT::Object, o), name)).object();
  } catch (const ScriptError&) {
    return nullptr;
  }
}

// gsInterpolate(gsColor, gsColor, uint): two-lane SWAR blend, per channel a + floor((b − a)·t/256) (§3.3)
uint32_t lerpColor(uint32_t a, uint32_t b, uint32_t t) {
  uint32_t lo = ((((b & 0x00FF00FFu) - (a & 0x00FF00FFu)) * t) >> 8) + (a & 0x00FF00FFu);
  uint32_t hi = ((((b & 0xFF00FF00u) >> 8) - ((a & 0xFF00FF00u) >> 8)) * t) + (a & 0xFF00FF00u);
  return (lo & 0x00FF00FFu) | (hi & 0xFF00FF00u);
}

// gsGetDataColor: red → yellow → green (§3.4)
uint32_t dataColor(uint32_t v) {
  return v <= 127 ? lerpColor(0xFFFF0000u, 0xFFFFFF00u, 2 * v) : lerpColor(0xFFFFFF00u, 0xFF00FF00u, 2 * v - 256);
}

struct MapTemplate {
  const ObjectObj* object = nullptr;
  int width = 1, length = 1, type = 0;
  uint32_t flags = 0, diagonals = 0;
};

std::vector<MapTemplate> mapTemplates(VM& vm, const GridObj& grid, const GridData& g) {
  std::vector<MapTemplate> out(g.types.size());
  for (size_t t = 0; t < g.types.size(); ++t) {
    auto it = grid.templates.find(g.types[t]);
    auto* o = it == grid.templates.end() ? nullptr : dynamic_cast<const ObjectObj*>(it->second.get());
    if (!o) continue;
    MapTemplate& m = out[t];
    m.object = o;
    m.width = std::max(1, intField(vm, o, u"width", 1));
    m.length = std::max(1, intField(vm, o, u"length", 1));
    m.type = intField(vm, o, u"type", 0);
    m.flags = uint32_t(intField(vm, o, u"flags", 0));
    m.diagonals = uint32_t(intField(vm, o, u"diagonals", 0));
  }
  return out;
}

// gsGridItem::getObjectDataValue (§3.5); −1 = no data colour. The traffic word and stops do not exist yet.
int dataValue(const MapTemplate& tp, int sx, int sz, uint32_t mode) {
  uint32_t g = mode & 0xF0000;
  int c = int((mode & 0x7000) >> 12), area = std::max(1, sx * sz);
  const ObjectObj* o = tp.object;
  auto clamp255 = [](int v) { return std::min(v, 255); };
  auto sum = [](const int* a, int n) {
    int s = 0;
    for (int i = 0; i < n; ++i) s += a[i];
    return s;
  };
  bool building = tp.type == kTypeBuilding;
  bool barred = c && (o->barredVisitors >> (c - 1) & 1);
  switch (g) {
    case 0x10000: return building ? clamp255((c ? o->homePlaces[c - 1] : sum(o->homePlaces, 7)) * 1024 / area) : -1;
    case 0x20000: return building ? clamp255((c ? o->workPlaces[c - 1] : sum(o->workPlaces, 7)) * 1024 / area) : -1;
    case 0x30000: return building && !barred ? clamp255(o->visitPlaces[3] * 2048 / area) : -1;
    case 0x40000: return building && !barred ? clamp255(o->visitPlaces[4] * 2048 / area) : -1;
    case 0x50000: return building && !barred ? clamp255(o->visitPlaces[5] * 1024 / area) : -1;
    case 0x60000: return tp.type >= 2 && tp.type <= 15 ? 255 : -1;  // traffic: no heat yet (u = 0)
    case 0x70000: return building ? 0 : -1;                           // catchment: no stops yet
    case 0x80000: return building ? 0 : -1;                           // popularity: no statistics yet
    default: return -1;
  }
}

// The transport kind of a stop template (0 bus, 1 tram, 2 metro, 3 waterbus, 4 special), −1 for other items
int stopKind(int type) {
  return (type == 2 || type == 5 || type == 6) ? 0
         : (type >= 10 && type <= 12)          ? 1
         : (type >= 13 && type <= 15)          ? 2
         : type == 0x11                        ? 3
         : type == 0x12                        ? 4
                                               : -1;
}

struct MapStop {
  int cx, cz, kind;
};

// The live stops by footprint centre (x + (size >> 1)) and kind, for the catchment view
std::vector<MapStop> mapStops(const GridData& g, const std::vector<MapTemplate>& tpl, const SimState& s) {
  std::vector<MapStop> out;
  for (const auto& [item, idx] : s.stopOfItem) {
    (void)idx;
    if (item < 0 || size_t(item) >= g.items.size()) continue;
    const GridData::Item& it = g.items[size_t(item)];
    if (it.removed || it.type >= tpl.size() || !tpl[it.type].object) continue;
    const MapTemplate& tp = tpl[it.type];
    int k = stopKind(tp.type);
    if (k < 0) continue;
    ItemFootprint f = itemFootprint(tp.width, tp.length, tp.diagonals, it.orient);
    out.push_back({it.x + (f.sx >> 1), it.z + (f.sz >> 1), k});
  }
  return out;
}

// §3.5 catchment (0x70000, gsGridItem::getObjectDataValue :108-168): the best reach r − d of the stops of kind c − 1
// (all stops for c = 0) whose catchment circle (r 100 for bus and tram, 150 otherwise) holds the building's centre,
// scaled by the building's size; 0 when no stop reaches it (owner's report 2026-11-25: buildings stayed red)
int catchmentValue(const std::vector<MapStop>& stops, int x, int z, int sx, int sz, int c, float cs) {
  const float cx = float(x + (sx >> 1)), cz = float(z + (sz >> 1));
  float best = -1;
  for (const MapStop& s : stops) {
    if (c && s.kind != c - 1) continue;
    const float r = s.kind <= 1 ? 100.0f : 150.0f;
    const float dx = (cx - float(s.cx)) * cs, dz = (cz - float(s.cz)) * cs, d2 = dx * dx + dz * dz;
    if (d2 < r * r) best = std::max(best, r - std::sqrt(d2));
  }
  if (best < 0) return 0;
  return std::clamp(int(best * 255.0f / (float(sx + sz) * cs * 0.25f)), 0, 255);
}

// §2.4: the transport marks (bytes 0..5). The original keeps them incrementally (stop create/destroy, segment
// updateMesh/clearRoute); the port rebuilds the raster on every render, so it counts the live stops and line legs here.
void addTransportMarks(std::vector<uint8_t>& R, const GridData& g, const std::vector<MapTemplate>& tpl, const SimState& s) {
  const int W = g.width, L = g.length;
  if (W <= 0 || L <= 0) return;
  auto mark = [&](long p, int t) {
    if (p < 0 || p >= 65536) return;  // the original has no bounds check; the port drops marks off the raster
    uint8_t* r = &R[size_t(p) * 16];
    ++r[0];
    ++r[1 + t];
  };
  // stops (gsTransportStopController::create L:0x081c2120.c:44-94): a plus shape around the footprint centre, record
  // from the exact division
  for (const auto& [item, idx] : s.stopOfItem) {
    (void)idx;
    if (item < 0 || size_t(item) >= g.items.size()) continue;
    const GridData::Item& it = g.items[size_t(item)];
    if (it.removed || it.type >= tpl.size() || !tpl[it.type].object) continue;
    const MapTemplate& tp = tpl[it.type];
    int type = tp.type;
    int t = stopKind(type);
    if (t < 0) continue;
    int sx = (it.orient & 1) ? tp.length : tp.width, sz = (it.orient & 1) ? tp.width : tp.length;
    long cx = it.x + (sx >> 1), cz = it.z + (sz >> 1);
    if (cx < 0 || cz < 0) continue;
    long p = (((cz * 65536) / L) & 0xff00) | ((cx * 256) / W);
    for (long d : {0L, -256L, -1L, 1L, 256L}) mark(p + d, t);
  }
  // line legs (gsTransportSegment::updateMesh L:0x081ba9f0.c:447-463,640-648,688-721): every path node's cell and the
  // midpoint of consecutive nodes, with the §2.1 reciprocal; bridge (0x4000) and tunnel (0x8000) pieces mark positions
  // sampled along their curve instead (port: the route strip's 4 steps per piece; the original's step count is not traced)
  const uint32_t sxr = 0x1000000u / uint32_t(W), szr = 0x1000000u / uint32_t(L);
  auto recOf = [&](int x, int z) -> long {
    if (x < 0 || z < 0) return -1;
    return long(((uint32_t(z) * szr) >> 16) * 256 + ((uint32_t(x) * sxr) >> 16));
  };
  const float cs = s.graph.cellSize > 0 ? s.graph.cellSize : 3.0f;
  for (const sim::LineState& line : s.lines) {
    if (line.deleted || line.type < 0 || line.type > 4) continue;
    for (const sim::LineStopState& st : line.stops) {
      long last = -1;
      int px = 0, pz = 0;
      bool havePrev = false;
      for (int nd : st.path) {
        if (nd < 0 || size_t(nd) >= s.graph.nodes.size()) continue;
        const LaneNode& n = s.graph.nodes[size_t(nd)];
        if (n.flags & 0xC000) {
          float Ln = n.length;
          Vec3 a = n.p0, b = n.p0 + n.f0 * (Ln / 3), c = n.p1 - n.f1 * (Ln / 3), e = n.p1;
          for (int k = 0; k <= 4; ++k) {
            float t = float(k) * 0.25f, u = 1 - t;
            Vec3 q = a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + e * (t * t * t);
            int qx = int((float(W) * cs * 0.5f + q.x) / cs), qz = int((float(L) * cs * 0.5f + q.z) / cs);
            long p = recOf(qx, qz);
            if (p != last) mark(p, line.type);
            last = p;
          }
          havePrev = false;
          continue;
        }
        if (havePrev) {
          long m = -1;  // (a + b)·s >> 17: the midpoint pixel without halving the cells first
          if (px + n.cellX >= 0 && pz + n.cellZ >= 0)
            m = long(((uint32_t(pz + n.cellZ) * szr) >> 17) * 256 + ((uint32_t(px + n.cellX) * sxr) >> 17));
          if (m != last) mark(m, line.type);
          last = m;
        }
        long p = recOf(n.cellX, n.cellZ);
        if (p != last) mark(p, line.type);
        last = p;
        px = n.cellX;
        pz = n.cellZ;
        havePrev = true;
      }
    }
  }
}

// §2.2 + §3.2: the counter raster of the whole grid, then w×h BGRA pixels (0xAARRGGBB)
std::vector<uint32_t> renderMapPixels(const GridData& g, const std::vector<MapTemplate>& tpl, int w, int h, uint32_t mode,
                                      const SimState* sim = nullptr) {
  const int W = g.width, L = g.length;
  std::vector<uint8_t> R(size_t(256) * 256 * 16, 0);
  if (sim) addTransportMarks(R, g, tpl, *sim);
  const bool catchment = (mode & 0xF0000) == 0x70000;
  std::vector<MapStop> stops;
  if (catchment && sim) stops = mapStops(g, tpl, *sim);
  const uint32_t sxr = 0x1000000u / uint32_t(std::max(1, W)), szr = 0x1000000u / uint32_t(std::max(1, L));
  auto rec = [&](int x, int z) -> uint8_t* {
    size_t px = (uint32_t(x) * sxr) >> 16, pz = (uint32_t(z) * szr) >> 16;
    return &R[(std::min<size_t>(pz, 255) * 256 + std::min<size_t>(px, 255)) * 16];
  };
  for (int z = 0; z < L; ++z)
    for (int x = 0; x < W; ++x) {
      uint16_t f = g.flags[size_t(z) * size_t(W) + size_t(x)];
      uint8_t* r = rec(x, z);
      if (f & 0x2000) ++r[9];
      else if (f & 0x20) ++r[7];
      else if (f & 0x40) ++r[10];
      else if (f & 0x08) ++r[11];
    }
  // port: cells a painted area's edge cuts (engine/grid.h PaintEdge) count as paved when its paint holds their centre
  for (const auto& [key, e] : g.paintEdges) {
    int x = int(key & 0xFFFF), z = int(key >> 16);
    if (x >= W || z >= L) continue;
    uint16_t f = g.flags[size_t(z) * size_t(W) + size_t(x)];
    if (!(f & 0x2068) && paintEdgeInside(e, 0.5f, 0.5f)) ++rec(x, z)[11];
  }
  for (const GridData::Item& it : g.items) {
    if (it.removed) continue;
    if (it.type >= tpl.size() || !tpl[it.type].object) continue;
    const MapTemplate& tp = tpl[it.type];
    uint32_t flags = it.flags | tp.flags;
    if (flags & (kFlagHidden | kItemPortHidden)) continue;
    ItemFootprint f = itemFootprint(tp.width, tp.length, tp.diagonals, it.orient);
    if (it.x < 0 || it.z < 0 || it.x + f.sx > W || it.z + f.sz > L) continue;  // in no bucket
    if (tp.type == kTypeTree) {
      for (int z = std::max(0, it.z - 2); z <= std::min(L - 1, it.z + 2); ++z)
        for (int x = std::max(0, it.x - 2); x <= std::min(W - 1, it.x + 2); ++x) ++rec(x, z)[8];
      continue;
    }
    int v = catchment && tp.type == kTypeBuilding
                ? catchmentValue(stops, it.x, it.z, f.sx, f.sz, int((mode & 0x7000) >> 12), g.cellSize > 0 ? g.cellSize : 3.0f)
                : dataValue(tp, f.sx, f.sz, mode);
    if (!(tp.type < kTypeTree || tp.type == kTypeBuilding || v != -1)) continue;
    for (int z = 0; z < f.sz; ++z)
      for (int x = 0; x < f.sx; ++x) {
        if (!itemCellCover(f, flags, x, z)) continue;
        uint8_t* r = rec(it.x + x, it.z + z);
        if (tp.type == kTypeBuilding) ++r[11];
        else if (tp.type < kTypeTree) ++r[12];
        if (v != -1) {
          ++r[13];
          uint16_t s = uint16_t(r[14] | r[15] << 8);
          s = uint16_t(s + v);
          r[14] = uint8_t(s);
          r[15] = uint8_t(s >> 8);
        }
      }
  }
  // port: curved roads and their junctions count as road cells (byte 12) where a cell's centre lies on them (footpaths
  // at least 1.6 units to each side, so their single cell row shows)
  if (!g.curves.empty() && g.flags.size() == size_t(W) * size_t(L)) {
    const float cs = g.cellSize;
    // the minimap is drawn every second: the cells stay cached while the curves are the same
    static std::vector<uint8_t> road;
    static uint64_t roadKey = 0;
    uint64_t key = 1469598103934665603ull ^ uint64_t(W) << 32 ^ uint64_t(L);
    auto mix = [&key](const void* p, size_t n) {
      for (size_t i = 0; i < n; ++i) key = (key ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
    };
    for (const CurveRoad& c : g.curves) {
      mix(&c.x0, sizeof(float) * 12);  // x0 .. b
      mix(&c.kind, sizeof c.kind);
      mix(&c.ends, 1);
    }
    if (key != roadKey || road.size() != g.flags.size()) {
    roadKey = key;
    road.assign(g.flags.size(), 0);
    auto centre = [&](int x, int z, float& px, float& pz) {
      px = (float(x) + 0.5f - float(W) * 0.5f) * cs, pz = (float(z) + 0.5f - float(L) * 0.5f) * cs;
    };
    for (const CurveRoad& c : g.curves) {
      int x0, z0, x1, z1;
      curveCells(g, c, x0, z0, x1, z1);
      std::vector<CurveSample> d = curveShape(c, curveFreeEnds(g, c));
      float reach = std::max(curveProfile(c.kind).halfWidth, 1.6f);
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
          float px, pz, side, s;
          centre(x, z, px, pz);
          curveNearest(d, px, pz, side, s);
          if (curveAcross(c, side) <= reach) road[size_t(z) * size_t(W) + size_t(x)] = 1;
        }
    }
    for (const CurveJunction& j : findCurveJunctions(g)) {
      int x0, z0, x1, z1;
      junctionCells(g, j, x0, z0, x1, z1);
      std::vector<Vec3> outline = junctionOutline(j, 1, 0.0f, 6);
      for (int z = std::max(0, z0); z <= std::min(L - 1, z1); ++z)
        for (int x = std::max(0, x0); x <= std::min(W - 1, x1); ++x) {
          float px, pz;
          centre(x, z, px, pz);
          if (insideOutline(outline, px, pz)) road[size_t(z) * size_t(W) + size_t(x)] = 1;
        }
    }
    }
    for (int z = 0; z < L; ++z)
      for (int x = 0; x < W; ++x)
        if (road[size_t(z) * size_t(W) + size_t(x)]) ++rec(x, z)[12];
  }
  w = std::min(w, W);
  h = std::min(h, L);
  std::vector<uint32_t> out(size_t(std::max(0, w)) * size_t(std::max(0, h)), 0);
  if (w <= 0 || h <= 0) return out;
  const uint32_t K = 0x10000u / uint32_t((W / w) * (L / h));
  auto wt = [K](uint32_t c) { return std::min<uint32_t>(255, (c * K) >> 8); };
  auto wt2 = [K](uint32_t c) { return std::min<uint32_t>(255, (c * K) >> 9); };
  uint32_t seed = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t* r = &R[(size_t(y) * 256 + size_t(x)) * 16];
      seed = seed * 0x343FDu + 0x269EC3u;
      uint32_t n = (seed >> 16) & 0xFF;
      uint32_t col = lerpColor(0xD0495226u, 0xD0414C20u, n);
      if (r[7]) col = lerpColor(col, lerpColor(0xD070601Au, 0xD0605010u, n), wt(r[7]));
      if (r[8]) col = lerpColor(col, 0xD0202C0Au, wt2(r[8]));
      if (r[10]) col = lerpColor(col, 0xFF92491Cu, wt(r[10]));
      if (r[9]) col = lerpColor(col, 0xD0295383u, wt(r[9]));
      if (r[11]) col = lerpColor(col, 0xFF42391Cu, wt(r[11]));
      if (r[12]) col = lerpColor(col, 0xFF605D58u, wt(r[12]));
      if (r[13]) col = dataColor(uint32_t(r[14] | r[15] << 8) / r[13]);
      // §3.2: a one-pixel black edge next to any transport mark, then the mark's type colour (bus > … > heli); the
      // neighbour tests use the fixed 256-record rows of the raster
      if ((x > 0 && r[-16]) || (y > 0 && r[-0x1000]) || (x < 255 && r[16]) || (y < 255 && r[0x1000])) col = 0xFF000000u;
      if (r[0]) {
        static const uint32_t kLine[5] = {0xFF4040FFu, 0xFFFF0000u, 0xFF00FF00u, 0xFFFFFF00u, 0xFFFF00FFu};
        for (int t = 0; t < 5; ++t)
          if (r[1 + t]) {
            col = kLine[t];
            break;
          }
      }
      out[size_t(y) * size_t(w) + size_t(x)] = col;
    }
  return out;
}

// REGS_MINIMAP_CHECK=amsterdam,tutorialcity: render shipped maps with the loaded templates and compare them with the
// editor's saved minimaps (metro/maps/<map>.dds, renderMap output; docs/ui/minimap.md §3.7)
void referenceCheck(VM& vm, const GridObj& grid) {
  static bool done = false;
  const char* list = regs::envVar("REGS_MINIMAP_CHECK");
  if (done || !list) return;
  done = true;
  std::stringstream ss(list);
  std::string name;
  while (std::getline(ss, name, ',')) {
    std::vector<uint8_t> gridFile, ddsFile;
    GridData g;
    Image img;
    std::string err;
    if (!vm.vfs().read("metro/maps/" + name + ".grid", gridFile) || !parseGridFile(gridFile, g, &err) ||
        !vm.vfs().read("metro/maps/" + name + ".dds", ddsFile) || !parseDds(ddsFile, img, &err) || img.levels.empty()) {
      LOG_WARN("minimap check %s: cannot read the map or its .dds (%s)", name.c_str(), err.c_str());
      continue;
    }
    std::vector<MapTemplate> tpl = mapTemplates(vm, grid, g);
    size_t unknown = 0;
    for (const auto& t : tpl) unknown += t.object ? 0 : 1;
    std::vector<uint32_t> px = renderMapPixels(g, tpl, img.width, img.height, 0xFE);
    size_t same = 0, total = std::min(px.size(), img.levels[0].size() / 4);
    for (size_t i = 0; i < total; ++i) {
      uint32_t ref;
      std::memcpy(&ref, img.levels[0].data() + i * 4, 4);
      same += ref == px[i] ? 1 : 0;
    }
    LOG_INFO("minimap check %s: %zu of %zu pixels identical (%.2f %%), %zu of %zu template types not loaded", name.c_str(),
             same, total, total ? 100.0 * double(same) / double(total) : 0.0, unknown, tpl.size());
  }
}

// §4.2 with the per-tick centre and zoom of §4.3
struct MapFrame {
  Vec3 centre;
  float scale = 0.001f, zoom = 0.15f, width = 0, height = 0;
};

MapFrame mapFrame(VM& vm, ControlObj* map) {
  MapFrame f;
  if (Object* t = objField(vm, map, u"targetModule")) f.centre = vecField(vm, t, u"position");
  f.scale = floatField(vm, map, u"scale", 0.001f);
  f.zoom = floatField(vm, map, u"zoom", 0.15f);  // zoomFactor 0 in the game: no speed zoom
  if (f.zoom == 0) f.zoom = 0.15f;
  f.width = map->width;
  f.height = map->height;
  return f;
}

Vec3 worldToMap(const MapFrame& f, const Vec3& p) {
  return {(0.5f - (p.x - f.centre.x) * f.scale / f.zoom) * f.width, (0.5f - (p.z - f.centre.z) * f.scale / f.zoom) * f.height, 0};
}

Vec3 mapToWorld(const MapFrame& f, float mx, float my) {
  return {(0.5f - mx / f.width) * f.zoom / f.scale + f.centre.x, 0, (0.5f - my / f.height) * f.zoom / f.scale + f.centre.z};
}

bool isA(VM& vm, ControlObj* c, const char* cls) {
  const ClassInfo* ci = vm.findClass(cls);
  return ci && c->cls->isA(ci);
}

}  // namespace

bool mapImageParams(VM& vm, ControlObj* c, float custom0[4]) {
  if (!isA(vm, c, "gsMapCtrl")) return false;
  MapFrame f = mapFrame(vm, c);
  Vec3 o = vecField(vm, c, u"offset");
  custom0[0] = (o.x - f.centre.x) * f.scale + 0.5f - f.zoom * 0.5f;
  custom0[1] = (o.z - f.centre.z) * f.scale + 0.5f - f.zoom * 0.5f;
  custom0[2] = f.zoom;
  custom0[3] = 1;
  return true;
}

void placeMapPin(VM& vm, ControlObj* c) {
  if (!isA(vm, c, "gsMapItemCtrl")) return;
  auto* map = dynamic_cast<ControlObj*>(c->parent);
  if (!map || !isA(vm, map, "gsMapCtrl")) return;
  MapFrame f = mapFrame(vm, map);
  Object* target = objField(vm, c, u"targetModule");
  Vec3 m = worldToMap(f, target ? vecField(vm, target, u"position") : vecField(vm, c, u"targetPos"));
  float dx = m.x - f.width * 0.5f, dy = m.y - f.height * 0.5f, rho = std::sqrt(dx * dx + dy * dy);
  float maxDistance = floatField(vm, map, u"maxDistance", 0.4f);
  if (rho > maxDistance * f.width) {
    m.x = dx * f.width * maxDistance / rho + f.width * 0.5f;
    m.y = f.height * dy * maxDistance / rho + f.height * 0.5f;
  }
  float x = m.x - c->width * 0.5f, y = m.y - c->height * 0.5f;
  if (c->width >= 1) {
    x = std::trunc(x);
    y = std::trunc(y);
  }
  c->left = x;
  c->top = y;
}

bool drawMapFootprint(Display& d, VM& vm, ControlObj* c) {
  if (!isA(vm, c, "gsMapViewportCtrl")) return false;
  auto* map = dynamic_cast<ControlObj*>(c->parent);
  auto* vp = dynamic_cast<ControlObj*>(objField(vm, c, u"viewport"));
  Object* cam = vp ? objField(vm, vp, u"camera") : nullptr;
  if (!map || !vp || !cam || vp->height <= 0) return true;
  Vec3 eye, look, up;
  cameraFrame(vm, cam, eye, look, up);
  Vec3 dir = normalize(look - eye), right = normalize(cross(dir, up)), u = cross(dir, right);
  float t = std::tan(floatField(vm, cam, u"fov", 60.0f) * 0.017453292f * 0.5f);
  u = u * t;
  right = right * (t * vp->width / vp->height);
  const Vec3 rays[4] = {dir + right + u, dir + right - u, dir - right - u, dir - right + u};
  MapFrame f = mapFrame(vm, map);
  GuiVertex v[8];
  Vec3 corner[4];
  for (int i = 0; i < 4; ++i) {
    const Vec3& q = rays[i];
    Vec3 p = q.y == 0 ? eye + q : eye + q * ((0 - eye.y) / q.y);  // the ground plane y = 0
    Vec3 m = worldToMap(f, p);
    corner[i] = {map->desktopLeft() + m.x, map->desktopTop() + m.y, 0};
  }
  for (int i = 0; i < 4; ++i) {  // the closed 5-vertex line strip as 4 segments, white, 1 px
    const Vec3& a = corner[i];
    const Vec3& b = corner[(i + 1) % 4];
    v[i * 2] = {a.x, a.y, 0, 0, 0xFFFFFFFFu};
    v[i * 2 + 1] = {b.x, b.y, 0, 0, 0xFFFFFFFFu};
  }
  d.r2d().draw(0, Prim::Lines, v, 8);
  return true;
}

std::vector<uint32_t> renderGridThumbnail(VM& vm, const GridObj& grid, const GridData& g, int w, int h) {
  return renderMapPixels(g, mapTemplates(vm, grid, g), w, h, 0xFE, &g == grid.data.get() ? grid.sim.get() : nullptr);
}

void registerMinimapNatives(VM& vm) {
  // renderMap(texture, width, height, mode) (L:0x08143f60): the minimap texture of the current map
  implement(vm, "gsGrid", u"renderMap", [](VM& v, Object* s, int, const Value* a) {
    auto* g = dynamic_cast<GridObj*>(s);
    auto* tex = dynamic_cast<TextureObj*>(v.resolve(a[0]).object());
    if (!g || !g->data || !tex) return Value();
    referenceCheck(v, *g);
    int w = v.toInt(a[1]), h = v.toInt(a[2]);
    std::vector<uint32_t> px = renderMapPixels(*g->data, mapTemplates(v, *g, *g->data), w, h, uint32_t(v.toInt(a[3])), g->sim.get());
    // REGS_MINIMAP_DUMP=<file>: every default-mode render also goes to a raw BGRA file (w × h × 4, rows top-down), to
    // compare the live map (e.g. after city growth) with the shipped per-year thumbnails <map>-timeline.dds
    if (const char* dump = regs::envVar("REGS_MINIMAP_DUMP"); dump && (uint32_t(v.toInt(a[3])) & 0xF0000) == 0)
      if (std::FILE* f = std::fopen(dump, "wb")) {
        std::fwrite(px.data(), 4, px.size(), f);
        std::fclose(f);
      }
    w = std::min(w, g->data->width);
    h = std::min(h, g->data->length);
    Display* d = display();
    if (!d || px.empty()) return Value();
    // BGRA bytes, RGBA8, linear, repeat, no mipmaps; row 0 = the top of the map (§3.6)
    const auto* bytes = reinterpret_cast<const uint8_t*>(px.data());
    if (tex->gl && tex->width == w && tex->height == h) d->r2d().updateTextureBGRA(tex->gl, bytes, w, h);
    else {
      if (tex->gl) d->r2d().destroyTexture(tex->gl);
      tex->gl = d->r2d().createTextureBGRA(bytes, w, h);
    }
    tex->width = w;
    tex->height = h;
    tex->cpuBGRA.assign(bytes, bytes + size_t(w) * size_t(h) * 4);  // for saveToFile (the editor's <map>.dds)
    return Value();
  });
  // worldToMapPosition(vector | x, z) / mapToWorldPosition(vector | x, y): control-local pixels ↔ world (§4.2)
  implement(vm, "gsMapCtrl", u"worldToMapPosition", [](VM& v, Object* s, int argc, const Value* a) {
    auto* c = dynamic_cast<ControlObj*>(s);
    if (!c) return Value::vector(0, 0, 0);
    Vec3 p;
    Value p0 = v.resolve(a[0]);
    if (argc == 1 && p0.type() == VT::Vector) p = {p0.vec()[0], p0.vec()[1], p0.vec()[2]};
    else p = {v.toFloat(a[0]), 0, argc > 1 ? v.toFloat(a[1]) : 0};
    Vec3 m = worldToMap(mapFrame(v, c), p);
    return Value::vector(m.x, m.y, 0);
  });
  implement(vm, "gsMapCtrl", u"mapToWorldPosition", [](VM& v, Object* s, int argc, const Value* a) {
    auto* c = dynamic_cast<ControlObj*>(s);
    if (!c || c->width <= 0 || c->height <= 0) return Value::vector(0, 0, 0);
    float mx, my;
    Value p0 = v.resolve(a[0]);
    if (argc == 1 && p0.type() == VT::Vector) {
      mx = p0.vec()[0];
      my = p0.vec()[1];
    } else {
      mx = v.toFloat(a[0]);
      my = argc > 1 ? v.toFloat(a[1]) : 0;
    }
    Vec3 w = mapToWorld(mapFrame(v, c), mx, my);
    return Value::vector(w.x, 0, w.z);
  });
  // template places (batch-A.md): counts per citizen class / visit type, barred visitor classes
  implement(vm, "gsGridObject", u"addHomePlaces", [](VM& v, Object* s, int, const Value* a) {
    if (auto* o = dynamic_cast<ObjectObj*>(s); o && v.toInt(a[0]) >= 0 && v.toInt(a[0]) < 7) o->homePlaces[v.toInt(a[0])] += v.toInt(a[1]);
    return Value();
  });
  implement(vm, "gsGridObject", u"addWorkPlaces", [](VM& v, Object* s, int, const Value* a) {
    if (auto* o = dynamic_cast<ObjectObj*>(s); o && v.toInt(a[0]) >= 0 && v.toInt(a[0]) < 7) o->workPlaces[v.toInt(a[0])] += v.toInt(a[1]);
    return Value();
  });
  implement(vm, "gsGridObject", u"addVisitPlaces", [](VM& v, Object* s, int, const Value* a) {
    auto* o = dynamic_cast<ObjectObj*>(s);
    int type = v.toInt(a[0]), n = v.toInt(a[1]);
    if (!o) return Value();
    if (type == 8) {  // GRID_VISIT_EMERGENCY counts for injury, fire and police
      for (int t : {9, 10, 11}) o->visitPlaces[t] += n;
    } else if (type >= 0 && type < 13) {
      o->visitPlaces[type] += n;
    }
    return Value();
  });
  implement(vm, "gsGridObject", u"preventVisitors", [](VM& v, Object* s, int argc, const Value* a) {
    if (auto* o = dynamic_cast<ObjectObj*>(s))
      for (int i = 0; i < argc; ++i)
        if (int c = v.toInt(a[i]); c >= 0 && c < 32) o->barredVisitors |= 1u << c;
    return Value();
  });
}

}  // namespace regs::engine
