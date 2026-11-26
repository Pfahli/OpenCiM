// Picking and selection in the 3D view (docs/ui/selection.md): gsGrid::intersectGrid, the gsGridPlayer select mode
// (hover, click, editMode) and the grid controllers of picked items.
#include "engine/interaction.h"
#include "engine/effects.h"
#include "core/env.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <chrono>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/grid_render.h"
#include "engine/gui.h"
#include "engine/headtrack.h"
#include "engine/scene3d.h"
#include "engine/settings.h"
#include "engine/sim.h"
#include "engine/sim_state.h"
#include "engine/sky.h"
#include "engine/stereo.h"
#include "engine/tools.h"
#include "render/math3d.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::render;

namespace {

constexpr int kModeSelectObject = 1;  // GRIDMODE_SELECT_OBJECT
constexpr uint32_t kFlagStop = 0x2000, kFlagTunnel = 0x8000, kFlagTemp = 0x100000, kFlagHidden = 0x400000;
constexpr int kTypeTree = 0x13, kTypeMarker = 0x15, kTypeSpawnPoint = 0x16;

Value obj(Object* o) { return o ? Value::heap(VT::Object, o) : Value::null(); }

Value member(VM& vm, Object* o, std::u16string_view name) {
  try {
    return vm.resolve(vm.getMember(Value::heap(VT::Object, o), name));
  } catch (const ScriptError&) {
    return Value();
  }
}

float numberOf(VM& vm, Object* o, std::u16string_view name, float def) {
  Value v = member(vm, o, name);
  return v.type() == VT::Float || v.type() == VT::Int ? vm.toFloat(v) : def;
}

int intField(VM& vm, const Object* o, std::u16string_view name, int def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toInt(it->second);
}

// truthiness of a possibly unset value (unset reads as false instead of a script error)
bool isSet(VM& vm, const Value& v) { return !v.isNone() && vm.truthy(v); }

bool flagField(VM& vm, const Object* o, std::u16string_view name) {
  auto it = o->fieldValues.find(name);
  return it != o->fieldValues.end() && isSet(vm, it->second);
}

float floatField(VM& vm, const Object* o, std::u16string_view name, float def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toFloat(it->second);
}

// ---------------------------------------------------------------------------------------------------------------
// Per-map picking data: the template fields intersectGrid reads, items bucketed by division, the controllers

struct PickTemplate {
  Object* object = nullptr;
  int width = 1, length = 1, type = 0;
  int height = 0;  // 1/256 cell (gsGridItem +0x3c)
  uint32_t flags = 0, diagonals = 0;
  bool named = false;  // non-empty displayName
};

struct PickState {
  std::weak_ptr<GridData> data;
  uint64_t itemsVersion = 0;  // GridObj::itemsVersion the buckets were made for (live map edits)
  std::vector<PickTemplate> templates;  // per map type index
  int n = 16, cols = 0, rows = 0;
  std::vector<std::vector<uint32_t>> divisions;          // item indices by the division of their origin cell
  std::unordered_map<uint32_t, Ref<Object>> controllers;  // item index → its controller, kept while the map lives
};

std::unordered_map<const GridObj*, PickState> g_pick;

void pickTemplates(VM& vm, GridObj* grid, PickState& st) {
  const GridData& g = *grid->data;
  st.templates.resize(g.types.size());
  for (size_t t = 0; t < g.types.size(); ++t) {
    if (st.templates[t].object) continue;  // (known; a type without its template is looked up again)
    auto it = grid->templates.find(g.types[t]);
    if (it == grid->templates.end() || !it->second) continue;
    Object* o = it->second.get();
    PickTemplate& p = st.templates[t];
    p.object = o;
    p.width = std::max(1, intField(vm, o, u"width", 1));
    p.length = std::max(1, intField(vm, o, u"length", 1));
    p.type = intField(vm, o, u"type", 0);
    p.flags = uint32_t(intField(vm, o, u"flags", 0));
    p.diagonals = uint32_t(intField(vm, o, u"diagonals", 0));
    p.height = int(numberOf(vm, o, u"height", 0) * 256.0f);
    Value name = member(vm, o, u"displayName");
    p.named = name.type() == VT::String && !vm.toStr(name).empty();
  }
}

// the division list an item belongs in, or null (off the map)
std::vector<uint32_t>* pickDivision(const GridData& g, PickState& st, const GridData::Item& it) {
  if (it.x < 0 || it.z < 0 || it.x >= g.width || it.z >= g.length) return nullptr;
  return &st.divisions[size_t(it.z / st.n) * size_t(st.cols) + size_t(it.x / st.n)];
}

PickState& pickState(VM& vm, GridObj* grid) {
  PickState& st = g_pick[grid];
  if (!grid->data || (st.data.lock() == grid->data && st.itemsVersion == grid->itemsVersion)) return st;
  const GridData& g = *grid->data;
  // live edits of the same map (a tool preview on every mouse move): only the items they touched, in item order as
  // the full build below (equal hits keep the first)
  static std::vector<uint32_t> touched;
  if (st.data.lock() == grid->data && gridItemsTouchedSince(grid, st.itemsVersion, touched)) {
    st.itemsVersion = grid->itemsVersion;
    pickTemplates(vm, grid, st);
    for (uint32_t i : touched) {
      if (i >= g.items.size()) continue;
      const GridData::Item& it = g.items[i];
      std::vector<uint32_t>* d = pickDivision(g, st, it);
      if (!d) continue;
      auto at = std::lower_bound(d->begin(), d->end(), i);
      bool listed = at != d->end() && *at == i;
      if (it.removed && listed) d->erase(at);
      else if (!it.removed && !listed) d->insert(at, i);
    }
    if (gridDebugItemCaches()) {
      PickState full;
      full.n = st.n, full.cols = st.cols, full.rows = st.rows;
      full.divisions.assign(st.divisions.size(), {});
      for (uint32_t i = 0; i < g.items.size(); ++i)
        if (!g.items[i].removed)
          if (std::vector<uint32_t>* d = pickDivision(g, full, g.items[i])) d->push_back(i);
      gridItemCacheChecked("picking", full.divisions == st.divisions && st.templates.size() == g.types.size());
    }
    return st;
  }
  // a new map starts over; edits of the same map keep the controllers (scripts hold them)
  auto controllers = st.data.lock() == grid->data ? std::move(st.controllers) : decltype(st.controllers){};
  st = PickState{};
  st.data = grid->data;
  st.itemsVersion = grid->itemsVersion;
  st.controllers = std::move(controllers);
  pickTemplates(vm, grid, st);
  st.n = 1 << std::min(g.bucketShift, 8u);
  st.cols = (g.width + st.n - 1) / st.n;
  st.rows = (g.length + st.n - 1) / st.n;
  st.divisions.assign(size_t(st.cols) * size_t(st.rows), {});
  for (uint32_t i = 0; i < g.items.size(); ++i) {
    const GridData::Item& it = g.items[i];
    if (it.removed) continue;
    if (std::vector<uint32_t>* d = pickDivision(g, st, it)) d->push_back(i);
  }
  return st;
}

uint32_t itemFlags(const GridData& g, const PickState& st, uint32_t i) {
  const GridData::Item& it = g.items[i];
  return it.flags | (it.type < st.templates.size() ? st.templates[it.type].flags : 0);
}

int itemType(const GridData& g, const PickState& st, uint32_t i) {
  const GridData::Item& it = g.items[i];
  return it.type < st.templates.size() ? st.templates[it.type].type : 0;
}

uint32_t rootItem(const GridData& g, uint32_t i) {
  for (int guard = 0; guard < 16 && g.items[i].parent && g.items[i].parent <= g.items.size(); ++guard) i = g.items[i].parent - 1;
  return i;
}

// gsGridItem::isVisible(viewMode) (L:0x08185c50, §2.5)
bool itemVisible(const GridData& g, const PickState& st, uint32_t i, uint32_t viewMode) {
  if (itemFlags(g, st, i) & kFlagTemp) return true;
  if (itemType(g, st, i) == kTypeTree) return viewMode & 0x08;
  uint32_t r = rootItem(g, i), rf = itemFlags(g, st, r);
  int rt = itemType(g, st, r);
  if ((viewMode & 0x100) && (rf & kFlagTunnel)) return true;
  if ((rt >= 2 && rt <= 0x12) || (rf & 0x8000000)) return viewMode & 0x10;
  return viewMode & 0x20;
}

// ---------------------------------------------------------------------------------------------------------------
// gsGrid::intersectGrid (§2)

// gsLineRectIntersect (L:0x0810ad20): t along D of the hit with the parallelogram C + u·e1 + v·e2, a = origin − C;
// two-sided, −1 outside or parallel
float lineRect(const Vec3& e1, const Vec3& e2, const Vec3& a, const Vec3& D) {
  Vec3 n = cross(e1, e2);
  float det = dot(D, n);
  if (det == 0) return -1;
  float inv = 1.0f / det;
  float u = dot(D, cross(a, e2)) * inv, v = dot(D, cross(e1, a)) * inv;
  return u >= 0 && u <= 1 && v >= 0 && v <= 1 ? -dot(a, n) * inv : -1;
}

// gsGrid::intersectCell (L:0x0813f280): the parallelogram through the corners (x,z), (x+1,z), (x,z+1)
// With a plane height (planeY != −0x8000, the subway level) the cell is the flat square at that height, covered or not.
constexpr int kNoPlane = -0x8000;
float intersectCell(const GridData& g, const Vec3& start, const Vec3& D, int x, int z, int planeY = kNoPlane) {
  if (x < 0 || z < 0 || x >= g.width || z >= g.length) return -1;
  size_t c = size_t(z) * size_t(g.width) + size_t(x);
  // fully covered by items (port: cells a curved road covers stay pickable ground)
  if (planeY == kNoPlane && (g.flags[c] & 0xF00) == 0xF00 && !(c < g.curveCover.size() && g.curveCover[c])) return -1;
  float cs = g.cellSize, k = cs / 256.0f;
  float h00 = g.height[c] * k;
  float h10 = (x < g.width - 1 ? g.height[c + 1] : g.height[c]) * k;
  float h01 = (z < g.length - 1 ? g.height[c + size_t(g.width)] : g.height[c]) * k;
  if (planeY != kNoPlane) h00 = h10 = h01 = float(planeY) * k;
  Vec3 C{float(x - (g.width >> 1)) * cs, h00, float(z - (g.length >> 1)) * cs};
  return lineRect({cs, h10 - h00, 0}, {0, h01 - h00, cs}, start - C, D);
}

struct Hit {
  float t = 1.0f;  // fraction of start → end
  int cellX = -1, cellZ = -1;
  int item = -1;                // item index (after the parent walk)
  Object* vehicle = nullptr;    // the vehicle's gsTransportVehicle or gsGridVehicleController
  Object* character = nullptr;  // the pedestrian's gsGridCharacterController
  int y = kNoPlane;             // the hit height in the engine's 1/256 cells (intersectGrid's int* out parameter)
  bool hit() const { return t < 1.0f; }
};

// planeY (the engine's int* in/out height): −0x8000 for the normal pick; else only tunnel items are hit and the
// terrain is the plane at that height (the subway level)
Hit intersectGrid(VM& vm, GridObj* grid, const Vec3& start, const Vec3& end, bool terrainOnly, bool vehicles,
                  int planeY = kNoPlane) {
  Hit h;
  if (!grid || !grid->data || grid->data->height.empty()) return h;
  const GridData& g = *grid->data;
  PickState& st = pickState(vm, grid);
  float cs = g.cellSize, hW = float(g.width) * cs * 0.5f, hL = float(g.length) * cs * 0.5f;
  int x0 = int((start.x + hW) / cs), z0 = int((start.z + hL) / cs);  // truncated toward zero
  int x1 = int((end.x + hW) / cs), z1 = int((end.z + hL) / cs);
  int steps = std::max(std::abs(x1 - x0), std::abs(z1 - z0)) + 1;
  Vec3 D = end - start;
  float stepX = D.x / float(steps), stepZ = D.z / float(steps);
  float len = std::sqrt(dot(D, D)), invLen = len < 0.1f ? 10.0f : 1.0f / len;
  int rx0 = std::min(x0, x1), rz0 = std::min(z0, z1), rx1 = std::max(x0, x1), rz1 = std::max(z0, z1);
  bool showMarkers = isSet(vm, member(vm, grid, u"showMarkers"));
  Value vmv = member(vm, grid, u"viewMode");
  uint32_t viewMode = vmv.type() == VT::Int ? uint32_t(vm.toInt(vmv)) : 0xFEu;

  // vehicles and characters, every pick (the original re-picks every tick, so the hover follows what moves under a
  // still cursor). The simulation skips what cannot be hit before working out its pose (SimPickCull: intersectGrid's
  // rectangle below and the hit reach around the segment). REGS_DEBUG_NO_PICK_CULL=1 tests everything as before,
  // =check does both and logs where they differ.
  auto movers = [&](Hit& h, const SimPickCull* cull) {
    // vehicles (§2.3): one flat rectangle per unit, 2.6 wide, 0.8 above the pivot, ranked 2 units nearer. Port: the
    // length is the model's bounds along the track (the original stretches front/rear offsets by 1 + 0.15·speed)
    forEachSimVehicle(grid, [&](const SimVehicleView& v) {
      if (!v.object) return;
      int cx = int((v.position.x + hW) / cs), cz = int((v.position.z + hL) / cs);
      if (cx < rx0 - 3 || cx > rx1 + 3 || cz < rz0 - 3 || cz > rz1 + 3) return;
      Vec3 fwd = normalize(v.forward), side = normalize(cross(Vec3{0, 1, 0}, fwd)), up = cross(fwd, side);
      float along = dot(v.center - v.position, fwd), half = std::max(v.halfExtents.z, 0.5f);
      Vec3 C = v.position + up * 0.8f - side * 1.3f + fwd * (along - half);
      float t = lineRect(side * 2.6f, fwd * (2.0f * half), start - C, D) - 2.0f * invLen;
      if (t >= 0 && t < h.t) {
        h.t = t;
        h.cellX = cx;
        h.cellZ = cz;
        h.vehicle = v.object;
        h.item = -1;
        h.y = int(std::lround(v.position.y / cs * 256.0f));
      }
    }, cull);

    // characters (§2.4): within 1 unit of the point 1 above the feet, ranked by the distance to that point minus 2
    forEachSimCharacter(grid, [&](const SimCharacterView& c) {
      int cx = int((c.position.x + hW) / cs), cz = int((c.position.z + hL) / cs);
      if (cx < rx0 - 3 || cx > rx1 + 3 || cz < rz0 - 3 || cz > rz1 + 3) return;
      Vec3 L = c.position + Vec3{0, 1, 0}, w = start - L, x = cross(D, w);
      float dd = dot(D, D), d2 = dd > 0 ? dot(x, x) / dd : 0;
      if (d2 >= 1.0f) return;
      float t = (std::sqrt(dot(w, w)) - 2.0f + std::sqrt(d2)) * invLen;
      if (t >= 0 && t < h.t) {
        // the controller only for a hit (getController in the original's pick, docs/ui/selection.md §5.2)
        Object* ctrl = c.object ? c.object : simCharacterController(grid, c.index);
        if (!ctrl) return;  // no controller: not pickable
        h.t = t;
        h.cellX = cx;
        h.cellZ = cz;
        h.vehicle = nullptr;
        h.character = ctrl;
        h.item = -1;
        h.y = int(std::lround(c.position.y / cs * 256.0f));
      }
    }, cull);
  };
  // The tools pick without vehicles and characters (intersectGrid(..., null, null), tools.h toolPick).
  if (vehicles) {
    static const char* noCull = regs::envVar("REGS_DEBUG_NO_PICK_CULL");
    SimPickCull cull;
    cull.start = start, cull.end = end;
    cull.halfWidth = hW, cull.halfLength = hL, cull.cellSize = cs;
    cull.x0 = rx0 - 3, cull.z0 = rz0 - 3, cull.x1 = rx1 + 3, cull.z1 = rz1 + 3;
    const bool check = noCull && std::string_view(noCull) == "check";
    Hit all = h;
    movers(h, noCull && !check ? nullptr : &cull);
    if (check) {
      movers(all, nullptr);
      if (all.t != h.t || all.vehicle != h.vehicle || all.character != h.character || all.cellX != h.cellX ||
          all.cellZ != h.cellZ)
        LOG_ERROR("pick cull: t %.5f vehicle %p character %p, without the cull t %.5f vehicle %p character %p", h.t,
                  static_cast<void*>(h.vehicle), static_cast<void*>(h.character), all.t, static_cast<void*>(all.vehicle),
                  static_cast<void*>(all.character));
    }
  }

  // items (§2.5): a flat rectangle over the footprint, at most a quarter cell above the base, then the cell cover
  if (!terrainOnly && st.cols > 0) {
    auto division = [&](int c, int count) { return std::clamp(c < 0 ? -1 : c / st.n, 0, count - 1); };
    int dx0 = std::max(0, division(rx0, st.cols) - 1), dx1 = std::min(st.cols - 1, division(rx1, st.cols) + 1);
    int dz0 = std::max(0, division(rz0, st.rows) - 1), dz1 = std::min(st.rows - 1, division(rz1, st.rows) + 1);
    for (int dz = dz0; dz <= dz1; ++dz)
      for (int dx = dx0; dx <= dx1; ++dx)
        for (uint32_t i : st.divisions[size_t(dz) * size_t(st.cols) + size_t(dx)]) {
          const GridData::Item& it = g.items[i];
          if (it.type >= st.templates.size() || !st.templates[it.type].object) continue;
          const PickTemplate& tp = st.templates[it.type];
          uint32_t f = it.flags | tp.flags;
          if (f & (kFlagTemp | kFlagHidden | kItemPortHidden)) continue;
          if (planeY != kNoPlane && !(f & kFlagTunnel)) continue;
          if (!itemVisible(g, st, i, viewMode)) continue;
          if (tp.type == kTypeMarker || tp.type == kTypeSpawnPoint) {
            if (!showMarkers) continue;
          } else if (tp.type == 0 || tp.type >= kTypeTree) {
            if (!tp.named && !showMarkers) continue;  // roads, tracks and stations are always pickable
          }
          ItemFootprint fp = itemFootprint(tp.width, tp.length, tp.diagonals, it.orient);
          int x = it.x, z = it.z, sx = fp.sx, sz = fp.sz;
          if (x > rx1 || x + sx <= rx0 || z > rz1 || z + sz <= rz0) continue;
          int hgt = tp.height;
          if (tp.type == kTypeMarker && sx >= 2 && hgt > 0x3FF) {  // markers: the 2×2 core
            x += (sx >> 1) - 1;
            z += (sz >> 1) - 1;
            sx = sz = 2;
          }
          int off = hgt > 0x3FF ? 0x40 : hgt >> 4;
          float top = float(it.y + off) * cs / 256.0f + (it.parent ? 1.0f : 0.0f);
          Vec3 C{float(x - (g.width >> 1)) * cs - 0.05f, top, float(z - (g.length >> 1)) * cs - 0.05f};
          float t = lineRect({float(sx) * cs + 0.1f, 0, 0}, {0, 0, float(sz) * cs + 0.1f}, start - C, D);
          if (t < 0 || t >= h.t) continue;
          int hx = std::clamp(int((start.x + D.x * t + hW) / cs), x, x + sx - 1);
          int hz = std::clamp(int((start.z + D.z * t + hL) / cs), z, z + sz - 1);
          if (!itemCellCover(fp, f, hx - it.x, hz - it.z)) continue;
          uint32_t r = i;  // parts of a composite item resolve to their parent
          while ((itemFlags(g, st, r) & 0x6000000) == 0x6000000 && g.items[r].parent && g.items[r].parent <= g.items.size())
            r = g.items[r].parent - 1;
          h.t = t;
          h.cellX = hx;
          h.cellZ = hz;
          h.item = int(r);
          h.vehicle = h.character = nullptr;
          h.y = it.y + (0x200 - int(g.heightBase));
        }
  }

  // terrain (§2.6): the cells along the segment, side cells first on a diagonal step; the minimum t wins
  if ((h.t >= 1.0f || (viewMode & 2)) && steps > 0) {
    int x = x0, z = z0;
    float fx = start.x - float(x0 - (g.width >> 1)) * cs, fz = start.z - float(z0 - (g.length >> 1)) * cs;
    auto testCell = [&](int cx, int cz) {
      float t = intersectCell(g, start, D, cx, cz, planeY);
      if (t >= 0 && t < h.t) {
        h.t = t;
        h.cellX = cx;
        h.cellZ = cz;
        h.y = planeY != kNoPlane ? planeY : (g.height[size_t(cz) * size_t(g.width) + size_t(cx)] + 0x80) & ~0xff;
        h.item = -1;
        h.vehicle = h.character = nullptr;
      }
    };
    for (int s = 0; s < steps; ++s) {
      fx += stepX;
      fz += stepZ;
      int nx = x, nz = z;
      if (fx >= cs) fx -= cs, nx = x + 1;
      else if (fx < 0) fx += cs, nx = x - 1;
      if (fz >= cs) fz -= cs, nz = z + 1;
      else if (fz < 0) fz += cs, nz = z - 1;
      if (nx != x && nz != z) {
        testCell(nx, z);
        testCell(x, nz);
      }
      testCell(nx, nz);
      x = nx;
      z = nz;
    }
  }
  return h;
}

// ---------------------------------------------------------------------------------------------------------------
// Controllers (§5)

// gsGridItem::getController: a gsTransportStopController for STOP items, else a gsGridItemController, child of the
// grid, at the footprint centre. Port: kept for the life of the map instead of freed when unreferenced.
Object* itemController(VM& vm, GridObj* grid, uint32_t i) {
  PickState& st = pickState(vm, grid);
  if (auto found = st.controllers.find(i); found != st.controllers.end()) return found->second.get();
  const GridData& g = *grid->data;
  if (i >= g.items.size()) return nullptr;
  const GridData::Item& it = g.items[i];
  if (it.type >= st.templates.size() || !st.templates[it.type].object) return nullptr;
  if (Object* stop = simStopController(grid, int(i))) {  // a line stop: the simulation's controller is the one object
    st.controllers[i] = Ref<Object>(stop);
    return stop;
  }
  const PickTemplate& tp = st.templates[it.type];
  const ClassInfo* cls = vm.findClass((it.flags | tp.flags) & kFlagStop ? "gsTransportStopController" : "gsGridItemController");
  if (!cls) return nullptr;
  Ref<Object> o = vm.newObject(cls, grid);
  ItemFootprint fp = itemFootprint(tp.width, tp.length, tp.diagonals, it.orient);
  float cs = g.cellSize, a = float(it.orient) * 1.57079633f;
  Value p = Value::vector(float(2 * it.x + fp.sx - g.width) * cs * 0.5f, float(it.y) * cs / 256.0f,
                          float(2 * it.z + fp.sz - g.length) * cs * 0.5f);
  o->fieldValues[u"position"] = p;
  o->fieldValues[u"globalPos"] = p;
  o->fieldValues[u"orientation"] = Value::quat(0, -std::sin(a * 0.5f), 0, std::cos(a * 0.5f));  // quat(orient·π/2, −Y)
  o->fieldValues[u"<item>"] = Value::integer(int32_t(i));
  o->fieldValues[u"<model>"] = obj(tp.object);
  st.controllers[i] = o;
  return o.get();
}

}  // namespace

// The catchment circles (gsTransportStopController::statusUpdated L:0x081b6e70 = gsGrid::addAreaHighlight /
// removeAreaHighlight while the controller's visible bit 2 is set; radius +0x124 from create L:0x081c2120: 100 for bus
// and tram stops, 150 for metro stations, docks and heliports). Collected each frame from the live controllers: the
// simulation's stops and the controllers made for picking and tool previews. Centre = the item's footprint centre.
void visibleStopCircles(VM& vm, GridObj* grid, std::vector<StopCircle>& out) {
  out.clear();
  if (!grid || !grid->data) return;
  static const ClassInfo* cls = nullptr;
  if (!cls) cls = vm.findClass("gsTransportStopController");
  if (!cls) return;
  const GridData& g = *grid->data;
  PickState& st = pickState(vm, grid);
  std::vector<Object*> seen;
  auto consider = [&](Object* c) {
    if (!c || !(c->flags & 2) || !c->cls->isA(cls)) return;
    if (std::find(seen.begin(), seen.end(), c) != seen.end()) return;
    seen.push_back(c);
    auto f = c->fieldValues.find(u"<item>");
    if (f == c->fieldValues.end()) return;
    uint32_t i = uint32_t(vm.toInt(f->second));
    if (i >= g.items.size() || g.items[i].removed || g.items[i].type >= st.templates.size()) return;
    const GridData::Item& it = g.items[i];
    const PickTemplate& tp = st.templates[it.type];
    ItemFootprint fp = itemFootprint(tp.width, tp.length, tp.diagonals, it.orient);
    int type = intField(vm, c, u"type", 0);
    float cs = g.cellSize;
    out.push_back({float((fp.sx >> 1) + it.x - (g.width >> 1)) * cs, float((fp.sz >> 1) + it.z - (g.length >> 1)) * cs,
                   type > 1 ? 150.0f : 100.0f});
  };
  if (grid->sim)
    for (const Ref<Object>& o : grid->sim->stops) consider(o.get());
  for (auto& [item, c] : st.controllers) consider(c.get());
}

namespace {

// the map item behind an item controller, or null
const GridData::Item* controllerItem(VM& vm, Object* c, GridObj** gridOut = nullptr, uint32_t* indexOut = nullptr) {
  auto* grid = dynamic_cast<GridObj*>(c->parent);
  auto it = c->fieldValues.find(u"<item>");
  if (!grid || !grid->data || it == c->fieldValues.end()) return nullptr;
  uint32_t i = uint32_t(vm.toInt(it->second));
  if (i >= grid->data->items.size()) return nullptr;
  if (gridOut) *gridOut = grid;
  if (indexOut) *indexOut = i;
  return &grid->data->items[i];
}

// ---------------------------------------------------------------------------------------------------------------
// gsGridPlayer (§1, §3, §4)

bool isPlayer(VM& vm, const Object* o) {
  static const ClassInfo* cls = nullptr;
  if (!cls) cls = vm.findClass("gsGridPlayer");
  return o && cls && o->cls->isA(cls);
}

int editMode(VM& vm, Object* p) { return intField(vm, p, u"editMode", kModeSelectObject); }  // ctor default

// §1: the segment from the eye to the far-plane point under a desktop pixel of the player's viewport
bool pickSegment(VM& vm, Object* player, float x, float y, Vec3& start, Vec3& end) {
  Object* cam = member(vm, player, u"camera").object();
  auto* vp = dynamic_cast<ControlObj*>(member(vm, player, u"viewport").object());
  if (!cam || !vp || vp->width <= 0 || vp->height <= 0) return false;
  Vec3 eye, look, up;
  cameraFrame(vm, cam, eye, look, up);
  float nearZ, farZ;
  viewDepthRange(vm, vp, cam, nearZ, farZ);
  farZ *= std::clamp(settingNumber(vm, u"regsViewDistance", 2.0f), 0.5f, 4.0f);  // what the port's view shows (default 200 %)
  float w = vp->width, h = vp->height;
  float nx = 2.0f * (x - vp->desktopLeft()) / w - 1.0f, ny = 1.0f - 2.0f * (y - vp->desktopTop()) / h;
  float t = std::tan(numberOf(vm, cam, u"fov", 60.0f) * 3.14159265f / 360.0f);
  Vec3 fwd = normalize(look - eye), right = normalize(cross(fwd, up)), upv = cross(right, fwd);
  start = eye;
  end = eye + (fwd + right * (nx * t * w / h) + upv * (ny * t)) * farZ;
  return true;
}

void setHover(VM& vm, Object* player, Object* ctrl, bool event) {
  player->fieldValues[u"hoverObject"] = obj(ctrl);
  if (event) vm.fireEvent(player, u"onHoverObject", {obj(ctrl)});
}

// gsGridPlayer::mouseMoveSelectObject (L:0x0818d3a0, §3.2): event 17 whenever the controller under the cursor
// changes, and every tick while there is none
void mouseMoveSelectObject(VM& vm, ControlObj* player, float x, float y) {
  auto* grid = dynamic_cast<GridObj*>(member(vm, player, u"grid").object());
  Vec3 start, end;
  Object* ctrl = nullptr;
  if (grid && pickSegment(vm, player, x + player->desktopLeft(), y + player->desktopTop(), start, end)) {
    Hit h = intersectGrid(vm, grid, start, end, false, true);
    if (h.item >= 0) ctrl = itemController(vm, grid, uint32_t(h.item));
    else if (h.vehicle) ctrl = h.vehicle;
    else if (h.character) ctrl = h.character;
    static const bool debug = regs::envVar("REGS_DEBUG_PICK") != nullptr;  // logs each change of the hovered object
    if (debug && ctrl && ctrl != player->fieldValues[u"hoverObject"].object()) {
      const GridData& g = *grid->data;
      LOG_INFO("pick (%.0f,%.0f): t %.4f cell %d,%d item %d %s vehicle %p character %p", x, y, h.t, h.cellX, h.cellZ, h.item,
               h.item >= 0 ? g.types[g.items[size_t(h.item)].type].c_str() : "-", static_cast<void*>(h.vehicle),
               static_cast<void*>(h.character));
    }
  }
  Object* old = player->fieldValues[u"hoverObject"].object();
  if (ctrl && ctrl == old) return;
  setHover(vm, player, ctrl, true);
}

// the select mode's cases of cancelTool (L:0x0819bcc0: the hover is dropped without an event) and finishTool
// (L:0x0819a3e0: event 20 with the hovered controller, then the hover is dropped)
ToolMode selectMode() {
  ToolMode m;
  m.move = [](VM& vm, ControlObj* player, GridObj*, float x, float y) { mouseMoveSelectObject(vm, player, x, y); };
  m.cancel = [](VM&, ControlObj* player, GridObj*) { player->fieldValues[u"hoverObject"] = Value::null(); };
  m.finish = [](VM& vm, ControlObj* player, GridObj*) {
    Value hover = player->fieldValues[u"hoverObject"];
    if (!hover.object()) return true;
    vm.fireEvent(player, u"onSelectObject", {hover});
    player->fieldValues[u"hoverObject"] = Value::null();
    return true;
  };
  return m;
}

Value emptyArray() { return Value::heap(VT::Array, make<ArrayObj>().get()); }

// the hooks run from the desktop's input dispatch, outside any script call: script errors end there
template <typename F>
void guarded(F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    LOG_ERROR("gsGridPlayer: %s", e.what());
  }
}

}  // namespace

// the player's mouse handlers: the tool framework (engine/tools.cpp) dispatches by editMode
void gridPlayerMouseMove(VM& vm, ControlObj* c, float x, float y, int buttons) {
  if (!isPlayer(vm, c)) return;
  guarded([&] { toolMouseMove(vm, c, x, y, buttons); });
}

void gridPlayerMouseDown(VM& vm, ControlObj* c, float x, float y, int key) {
  if (!isPlayer(vm, c)) return;
  guarded([&] { toolMouseDown(vm, c, x, y, key); });
}

void gridPlayerMouseUp(VM& vm, ControlObj* c, float x, float y, int key) {
  if (!isPlayer(vm, c)) return;
  guarded([&] { toolMouseUp(vm, c, x, y, key); });
}

// port (stop bubbles behind buildings, 2026-11-25): whether the segment passes through an item's box at its full
// height (the footprint from its cells, from its base y up by the template height). intersectGrid is the mouse pick:
// it leaves unnamed buildings out and takes items as flat slabs. Trees, markers and items under 4 m are no obstacle.
bool segmentBlockedByItems(VM& vm, GridObj* grid, const Vec3& start, const Vec3& end) {
  if (!grid || !grid->data) return false;
  const GridData& g = *grid->data;
  PickState& st = pickState(vm, grid);
  if (st.cols <= 0) return false;
  const float cs = g.cellSize, hW = float(g.width) * cs * 0.5f, hL = float(g.length) * cs * 0.5f;
  auto cellOf = [&](float v, float half) { return int(std::floor((v + half) / cs)); };
  const int rx0 = std::min(cellOf(start.x, hW), cellOf(end.x, hW)), rx1 = std::max(cellOf(start.x, hW), cellOf(end.x, hW));
  const int rz0 = std::min(cellOf(start.z, hL), cellOf(end.z, hL)), rz1 = std::max(cellOf(start.z, hL), cellOf(end.z, hL));
  auto division = [&](int c, int count) { return std::clamp(c < 0 ? -1 : c / st.n, 0, count - 1); };
  const int dx0 = std::max(0, division(rx0, st.cols) - 1), dx1 = std::min(st.cols - 1, division(rx1, st.cols) + 1);
  const int dz0 = std::max(0, division(rz0, st.rows) - 1), dz1 = std::min(st.rows - 1, division(rz1, st.rows) + 1);
  const Vec3 D = end - start;
  const uint32_t viewMode = uint32_t(vm.toInt(member(vm, grid, u"viewMode")));
  for (int dz = dz0; dz <= dz1; ++dz)
    for (int dx = dx0; dx <= dx1; ++dx)
      for (uint32_t i : st.divisions[size_t(dz) * size_t(st.cols) + size_t(dx)]) {
        const GridData::Item& it = g.items[i];
        if (it.removed || it.type >= st.templates.size() || !st.templates[it.type].object) continue;
        const PickTemplate& tp = st.templates[it.type];
        const uint32_t f = it.flags | tp.flags;
        if (f & (kFlagTemp | kFlagHidden | kItemPortHidden)) continue;
        if (tp.type == kTypeMarker || tp.type == kTypeSpawnPoint || tp.type == kTypeTree) continue;
        const float height = float(tp.height) * cs / 256.0f;
        if (height < 4.0f || !itemVisible(g, st, i, viewMode)) continue;
        const ItemFootprint fp = itemFootprint(tp.width, tp.length, tp.diagonals, it.orient);
        if (it.x > rx1 || it.x + fp.sx <= rx0 || it.z > rz1 || it.z + fp.sz <= rz0) continue;
        const float lo[3] = {float(it.x) * cs - hW, float(it.y) * cs / 256.0f, float(it.z) * cs - hL};
        const float hi[3] = {lo[0] + float(fp.sx) * cs, lo[1] + height, lo[2] + float(fp.sz) * cs};
        const float s[3] = {start.x, start.y, start.z}, d[3] = {D.x, D.y, D.z};
        float t0 = 0.0f, t1 = 1.0f;
        bool miss = false;
        for (int k = 0; k < 3 && !miss; ++k) {
          if (std::fabs(d[k]) < 1e-6f) {
            miss = s[k] < lo[k] || s[k] > hi[k];
            continue;
          }
          float a = (lo[k] - s[k]) / d[k], b = (hi[k] - s[k]) / d[k];
          if (a > b) std::swap(a, b);
          t0 = std::max(t0, a), t1 = std::min(t1, b);
          miss = t0 > t1;
        }
        if (!miss) return true;
      }
  return false;
}

// a pick for the editing tools: intersectGrid without vehicles and characters, under a player-local position
ToolPick toolPick(VM& vm, ControlObj* player, float x, float y, bool terrainOnly) {
  return toolPickLevel(vm, player, x, y, kNoPlane, terrainOnly);
}

// the same with intersectGrid's height parameter (the subway level: tunnels only, and the plane at that height)
ToolPick toolPickLevel(VM& vm, ControlObj* player, float x, float y, int planeY, bool terrainOnly) {
  ToolPick out;
  auto* grid = dynamic_cast<GridObj*>(member(vm, player, u"grid").object());
  Vec3 start, end;
  if (!grid || !pickSegment(vm, player, x + player->desktopLeft(), y + player->desktopTop(), start, end)) return out;
  Hit h = intersectGrid(vm, grid, start, end, terrainOnly, false, planeY);
  if (!h.hit()) return out;
  out.hit = true;
  out.cellX = h.cellX;
  out.cellZ = h.cellZ;
  out.item = h.item;
  out.y = h.y;
  out.point = start + (end - start) * h.t;
  return out;
}

namespace {

// A plain field read by a key made once: the FieldInfo is found once per class instead of building the name and
// walking the class chain on every read. The same as member(vm, o, name) (getMember's field path: the getter, then
// resolve); names that are no field of the class (methods, script functions, errors) take the general path.
struct FieldKey {
  std::u16string name;
  const ClassInfo* cls = nullptr;
  const FieldInfo* field = nullptr;
  explicit FieldKey(const char16_t* n) : name(n) {}
};

Value member(VM& vm, Object* o, FieldKey& k) {
  if (o->cls != k.cls) k.cls = o->cls, k.field = o->cls->findField(k.name);
  if (!k.field) return member(vm, o, std::u16string_view(k.name));
  try {
    return vm.resolve(k.field->get ? k.field->get(vm, o) : Value());
  } catch (const ScriptError&) {
    return Value();
  }
}

float numberOf(VM& vm, Object* o, FieldKey& k, float def) {
  Value v = member(vm, o, k);
  return v.type() == VT::Float || v.type() == VT::Int ? vm.toFloat(v) : def;
}

using TracerView = TracerViews::View;

void tracerView(VM& vm, ControlObj* vp, TracerView& v) {
  static FieldKey kCamera(u"camera"), kFov(u"fov");
  v.vp = vp;
  v.ok = false;
  Object* cam = member(vm, vp, kCamera).object();
  if (!cam) return;
  Vec3 eye, look, up;
  cameraFrame(vm, cam, eye, look, up);
  // the camera as the view is drawn: head tracking turns it about its look point (engine/headtrack.h), so the
  // bubbles stay on their stops while the head moves
  headTrackCamera(eye, look);
  v.w = vp->width, v.h = vp->height, v.t = std::tan(numberOf(vm, cam, kFov, 60.0f) * 3.14159265f / 360.0f);
  v.eye = eye;
  v.focus = std::sqrt(dot(look - eye, look - eye));
  v.fwd = normalize(look - eye), v.right = normalize(cross(v.fwd, up)), v.upv = cross(v.right, v.fwd);
  v.left = vp->desktopLeft(), v.top = vp->desktopTop();
  v.ok = true;
}

// stereo 3D: each tracer's sideways shift for the eye being drawn (control units), so it lies at its point's depth;
// an organizer adds it after laying its children out (both eyes then get the same layout)
std::unordered_map<const ControlObj*, float>& eyeShifts() {
  static std::unordered_map<const ControlObj*, float> m;
  return m;
}

// views: the viewports already worked out in this GUI pass (null: work the viewport out for this tracer alone);
// applyShift false: the stereo eye's shift is left to the caller (organizeTracers)
void placeTracerIn(VM& vm, ControlObj* c, TracerViews* views, bool applyShift = true) {
  static const ClassInfo* cls = vm.findClass("gsTracerCtrl");
  if (!cls || !c->cls->isA(cls)) return;
  static FieldKey kViewport(u"targetViewport"), kPosition(u"targetPosition"), kModule(u"targetModule"),
      kGlobalPos(u"globalPos"), kModulePos(u"position"), kAnchorX(u"anchorX"), kAnchorY(u"anchorY");
  guarded([&] {
    auto* vp = dynamic_cast<ControlObj*>(member(vm, c, kViewport).object());
    if (!vp || vp->width <= 0 || vp->height <= 0) return;
    TracerView own;
    const TracerView* view = nullptr;
    if (views)
      for (const TracerView& v : views->views)
        if (v.vp == vp) view = &v;
    if (!view) {
      tracerView(vm, vp, own);
      if (views) views->views.push_back(own);
      view = &own;
    }
    if (!view->ok) return;  // no camera
    // the target point: targetPosition in the module's frame (port: offsets are not rotated with the module; the
    // shipped tracers use vertical ones) or in world space without a module
    Value tp = member(vm, c, kPosition);
    Vec3 p = tp.type() == VT::Vector ? Vec3{tp.vec()[0], tp.vec()[1], tp.vec()[2]} : Vec3{};
    if (Object* m = member(vm, c, kModule).object()) {
      Value g = member(vm, m, kGlobalPos);
      if (g.type() != VT::Vector) g = member(vm, m, kModulePos);
      if (g.type() == VT::Vector) p = p + Vec3{g.vec()[0], g.vec()[1], g.vec()[2]};
    }
    const Vec3 &eye = view->eye, &fwd = view->fwd, &right = view->right, &upv = view->upv;
    const float w = view->w, h = view->h, t = view->t;
    Vec3 d = p - eye;
    float z = dot(d, fwd), nx = z > 0 ? dot(d, right) / (z * t * w / h) : 2, ny = z > 0 ? dot(d, upv) / (z * t) : 2;
    if (z <= 0 || std::abs(nx) > 1 || std::abs(ny) > 1) {  // behind the camera or outside the four side planes
      c->left = c->top = -10000;
      return;
    }
    // port (owner's request 2026-11-25): a stop's bubble is left out far off (beyond about twice the camera's distance
    // to its look point: only with the camera tilted towards the horizon) or behind a building or a hill (a ray from
    // the eye, checked every 0.15 s per bubble)
    // (2026-11-25, owner: the obstacle test only with the camera low and zoomed in; the bubbles fade in and out over
    // about half a second instead of popping)
    static const ClassInfo* stopCls = vm.findClass("gsTransportStopController");
    float fadeAlpha = 1.0f;
    Object* m = member(vm, c, kModule).object();
    const bool isStop = m && stopCls && m->cls->isA(stopCls);
    // the grid the target lives in (a stop's parent; a passenger's mood smiley follows a character further down)
    GridObj* targetGrid = nullptr;
    for (Object* o = m ? m->parent : nullptr; o && !targetGrid; o = o->parent) targetGrid = dynamic_cast<GridObj*>(o);
    // stereo 3D: every map tracer (the passengers' mood smileys too, owner 2026-11-26) hides behind what stands in
    // front of its point
    if (isStop || (stereoEye() != 0 && targetGrid)) {
      const float dist = std::sqrt(dot(d, d));
      struct Seen {
        std::chrono::steady_clock::time_point at, last;
        bool hidden = false;
        float alpha = 1.0f;
      };
      static std::unordered_map<const ControlObj*, Seen> seen;
      if (seen.size() > 4096) seen.clear();
      const bool fresh = !seen.count(c);
      Seen& s = seen[c];
      const auto now = std::chrono::steady_clock::now();
      const bool lowAndClose = -view->fwd.y < 0.64f && view->focus > 0 && view->focus < 260.0f;  // under ~40°, near
      // stereo 3D: always (a bubble over a building that stands in front of its stop looks wrong in depth; owner
      // 2026-11-26 on the Odyssey 3D)
      const bool testObstacles = lowAndClose || stereoEye() != 0;
      // (the many passengers' smileys less often than the stops' bubbles, each at its own moment)
      const auto every = std::chrono::milliseconds(isStop ? 150 : 250 + int(reinterpret_cast<uintptr_t>(c) >> 4 & 63));
      if (now - s.at > every) {
        s.at = now;
        s.hidden = false;
        if (GridObj* grid = targetGrid; testObstacles && grid && dist > 1.0f) {
          // to just short of the point (a stop's shelter and the road under it, a passenger's own body are no obstacle)
          const Vec3 end = eye + d * std::max(0.0f, 1.0f - (isStop ? 12.0f : 2.5f) / dist);
          s.hidden = intersectGrid(vm, grid, eye, end, true, false).hit() || segmentBlockedByItems(vm, grid, eye, end);
        }
      }
      const bool far = isStop && view->focus > 0 && dist > view->focus * 2.2f + 120.0f;
      const float target = (s.hidden || far) ? 0.0f : 1.0f;
      const float dt = fresh ? 1.0f : std::min(0.25f, std::chrono::duration<float>(now - s.last).count());
      s.last = now;
      s.alpha = target > s.alpha ? std::min(target, s.alpha + dt * 2.0f) : std::max(target, s.alpha - dt * 2.0f);
      if (fresh) s.alpha = target;
      if (s.alpha < 0.01f) {
        c->left = c->top = -10000;
        return;
      }
      fadeAlpha = s.alpha;
    }
    float sx = view->left + (nx + 1) * 0.5f * w, sy = view->top + (1 - ny) * 0.5f * h;
    // stereo 3D: moved sideways for the eye being drawn as the scene's point is, so the bubble lies at its stop's
    // depth instead of on the screen plane
    if (stereoEye() != 0) {
      const float shift = stereoEyeShift(eye, eye + fwd * view->focus, h / (w * t), 1.0f / t, z) * 0.5f * w;
      if (applyShift) sx += shift;
      else eyeShifts()[c] = shift;
    }
    float px = 0, py = 0;
    if (ControlObj* parent = c->parentControl()) px = parent->desktopLeft(), py = parent->desktopTop();
    c->left = sx - c->width * numberOf(vm, c, kAnchorX, 0.5f) - px;
    c->top = sy - c->height * numberOf(vm, c, kAnchorY, 0.5f) - py;
    tracerFade(vm, c, eye, p);  // enableFade: visibility by distance (engine/sky.cpp)
    // the lens blur's radius where the bubble's point is: the GUI draws it as soft (engine/display.cpp control)
    if (float blurPx = lensBlurRadiusAt(vm, z, view->focus, (1 - ny) * 0.5f); blurPx >= 0.75f)
      c->fieldValues[u"<regsBlur>"] = Value::real(blurPx);
    else
      c->fieldValues.erase(u"<regsBlur>");
    if (fadeAlpha < 1.0f || c->fieldValues.count(u"<regsFaded>")) {  // a stop bubble fading: it and its parts
      const bool ownFade = c->fieldValues.count(u"<fade>") != 0;
      c->visibility = (ownFade ? c->visibility : 1.0f) * fadeAlpha;
      for (auto& k : c->children)
        if (auto* ch = dynamic_cast<ControlObj*>(k.get())) ch->visibility = fadeAlpha;
      if (fadeAlpha < 1.0f) c->fieldValues[u"<regsFaded>"] = Value::boolean(true);
      else c->fieldValues.erase(u"<regsFaded>");
    }
  });
}

}  // namespace

void placeTracer(VM& vm, ControlObj* c, TracerViews* views) { placeTracerIn(vm, c, views); }

// gsTracerOrganizer::move (L:0x081d14c0 = W:0x100af3d0, docs/ui/lines.md §4.1). The organizer keeps no state: each
// frame its children move first (gsControl::move → each child's gsTracerCtrl::move), then the tracer children with
// organize set (+0x1cb, default true) and left > −width (on screen; off-view tracers are parked at −2·size) are taken
// in ascending left order (a multimap keyed by left, equal keys after earlier ones) and relaxed in 3 passes: for each
// A and each later B whose key is below A's right edge, if the two overlap vertically both move apart by a third of the
// smallest of the three overlaps (B's bottom − A's top: A down / B up; A's right − B's left: A left / B right; A's
// bottom − B's top: A up / B down), with the epsilons 0.001 / 0.002 of the original. No clamping, no smoothing.
void organizeTracers(VM& vm, ControlObj* organizer, TracerViews* views) {
  static const ClassInfo* tracer = vm.findClass("gsTracerCtrl");
  if (!tracer) return;
  struct Entry {
    float key;
    ControlObj* c;
  };
  std::vector<Entry> list;
  TracerViews own;  // without a pass's views: this organizer's target viewports
  if (!views) views = &own;
  static const std::u16string kOrganize = u"organize";
  eyeShifts().clear();
  for (auto& k : organizer->children) {
    auto* t = dynamic_cast<ControlObj*>(k.get());
    if (!t || t->destroyed || !t->cls->isA(tracer)) continue;
    placeTracerIn(vm, t, views, false);
    // organize: a plain field the scripts write; unset = the constructor's true (L:0x081d38a0.c:29)
    if (auto o = t->fieldValues.find(kOrganize); o != t->fieldValues.end() && !o->second.isNone() && !vm.toInt(o->second)) continue;
    if (!(t->left > -t->width)) continue;
    list.push_back({t->left, t});
  }
  std::stable_sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });
  for (int pass = 0; pass < 3; ++pass)
    for (size_t i = 0; i < list.size(); ++i) {
      ControlObj* a = list[i].c;
      for (size_t j = i + 1; j < list.size(); ++j) {
        if (a->left + a->width <= list[j].key) break;
        ControlObj* b = list[j].c;
        float ay = a->top;
        float d7 = ((b->top + b->height) - ay) * 0.33f + 0.001f;
        float d6 = ((ay + a->height) - b->top) * 0.33f - 0.001f;
        if (!(d7 > 0 && d6 > 0)) continue;
        float d5 = ((a->left + a->width) - b->left) * 0.33f - 0.002f;
        if (d7 < d6 && d7 < d5) {
          a->top = ay + d7;
          b->top -= d7;
        } else if (d5 <= d6) {
          a->left -= d5;
          b->left += d5;
        } else {
          a->top = ay - d6;
          b->top += d6;
        }
      }
    }
  // stereo 3D: the eye's shift after the layout, which so stays the same in both eyes' pictures
  for (const auto& [t, shift] : eyeShifts()) const_cast<ControlObj*>(t)->left += shift;
  eyeShifts().clear();
}

Object* gridItemController(VM& vm, GridObj* grid, uint32_t item) {
  return grid && grid->data && item < grid->data->items.size() && !grid->data->items[item].removed
             ? itemController(vm, grid, item)
             : nullptr;
}

void gridItemControllerRemoved(VM& vm, GridObj* grid, uint32_t item) {
  // gsGridController::itemDeleted (L:0x08163450): the controller of a removed item is destroyed (onDestroy), so a
  // selection or the corner preview on it ends
  auto st = g_pick.find(grid);
  if (st == g_pick.end()) return;
  auto c = st->second.controllers.find(item);
  if (c == st->second.controllers.end()) return;
  Ref<Object> o = c->second;
  st->second.controllers.erase(c);
  vm.destroy(o.get());
}

void registerInteractionNatives(VM& vm) {
  // ---- gsGridPlayer ----
  // getWorldPosition(x, y) (L:0x0818bd80, §2.9): the point under a desktop pixel on the plane y = 0 (the terrain is
  // not consulted); the far point when the ray does not descend
  implement(vm, "gsGridPlayer", u"getWorldPosition", [](VM& v, Object* s, int, const Value* a) {
    Vec3 start, end;
    if (!pickSegment(v, s, v.toFloat(a[0]), v.toFloat(a[1]), start, end)) return Value::vector(0, 0, 0);
    if (start.y > end.y) {
      Vec3 p = start + (end - start) * (start.y / (start.y - end.y));
      return Value::vector(p.x, 0, p.z);
    }
    return Value::vector(end.x, end.y, end.z);
  });
  // canApplyTool, applyTool, resetTool and the new/replaced item lists: engine/tools.cpp
  registerToolMode(kGridModeSelectObject, selectMode());
  // editMode: the constructor starts in select mode. editMode = m (setEditMode L:0x0819c2c0, §4.5): cancel the old
  // tool, clear the tool objects, drop the hover with event 17, then event 21 onModeChange(old mode)
  if (ClassInfo* c = vm.findClass("gsGridPlayer"); c && c->fields.count(u"editMode")) {
    c->fields[u"editMode"].get = [](VM& v, Object* o) { return Value::integer(editMode(v, o)); };
    c->fields[u"editMode"].set = [](VM& v, Object* o, const Value& val) {
      int old = editMode(v, o);
      toolCancel(v, o);
      o->fieldValues[u"toolMode"] = Value::integer(0);
      o->fieldValues[u"toolType"] = Value::integer(0);
      o->fieldValues[u"editMode"] = Value::integer(v.toInt(val));
      for (const char16_t* f : {u"toolObject", u"toolObject2", u"toolLine"}) o->fieldValues[f] = Value::null();
      if (o->fieldValues[u"hoverObject"].object()) setHover(v, o, nullptr, true);
      v.fireEvent(o, u"onModeChange", {Value::integer(old)});
    };
  }

  // gsGrid.viewMode: the constructor's GRID_VIEW_DEFAULT 0xfe (L:0x0813d6a0.c:56) until a script sets it.
  // write_viewMode (L:0x0813ef00, docs/renderer/underground.md §2): a change in bits 0xfe clears every cell's cover
  // quadrants and runs fixGrid for all items again (cover only in views with 0x20), then rebuilds every division and
  // updates the vehicles' and characters' visibility (the renderer reads GridObj::viewMode each frame). A change in
  // the data bits 0xf7000 only refreshes the selection/data layers (not ported).
  if (ClassInfo* c = vm.findClass("gsGrid"); c && c->fields.count(u"viewMode")) {
    c->fields[u"viewMode"].get = [](VM&, Object* o) {
      auto* g = dynamic_cast<GridObj*>(o);
      return Value::integer(int32_t(g ? g->viewMode : 0xFEu));
    };
    c->fields[u"viewMode"].set = [](VM& v, Object* o, const Value& val) {
      auto* g = dynamic_cast<GridObj*>(o);
      if (!g) return;
      uint32_t mode = uint32_t(v.toInt(val)), changed = g->viewMode ^ mode;
      if (!changed) return;
      g->viewMode = mode;
      o->fieldValues[u"viewMode"] = Value::integer(int32_t(mode));
      if (!(changed & 0xFE)) return;
      if (g->data && !g->data->flags.empty()) fixGridCoverArea(v, g, 0, 0, g->data->width - 1, g->data->length - 1);
      ++g->viewVersion;
      LOG_INFO("grid: viewMode 0x%x", mode);
    };
  }

  // ---- controllers ----
  implement(vm, "gsGridController", u"getModel", [](VM&, Object* s, int, const Value*) {
    auto it = s->fieldValues.find(u"<model>");
    return it == s->fieldValues.end() ? Value::null() : it->second;
  });
  implement(vm, "gsGridController", u"hasInstance", [](VM& v, Object* s, int, const Value*) {
    auto it = s->fieldValues.find(u"<instance>");
    return Value::boolean(it == s->fieldValues.end() || isSet(v, it->second));
  });
  // name: the map's item name (gsGridItem +0x60); writes rename the item
  if (ClassInfo* c = vm.findClass("gsGridItemController"); c && c->fields.count(u"name")) {
    c->fields[u"name"].get = [](VM& v, Object* o) {
      // port: the rename tool's stand-in for curves (tool_curves.cpp): its pending name
      if (auto pn = o->fieldValues.find(u"<curveName>"); pn != o->fieldValues.end()) return pn->second;
      GridObj* grid = nullptr;
      uint32_t i = 0;
      if (!controllerItem(v, o, &grid, &i) || i >= grid->data->names.size()) return Value::string(u"");
      return Value::string(utf8ToUtf16(grid->data->names[i]));
    };
    c->fields[u"name"].set = [](VM& v, Object* o, const Value& val) {
      if (auto pn = o->fieldValues.find(u"<curveName>"); pn != o->fieldValues.end()) {
        pn->second = Value::string(v.toStr(val));
        return;
      }
      GridObj* grid = nullptr;
      uint32_t i = 0;
      if (!controllerItem(v, o, &grid, &i)) return;
      if (grid->data->names.size() <= i) grid->data->names.resize(i + 1);
      grid->data->names[i] = utf16ToUtf8(v.toStr(val));
    };
  }
  // setColor(colour) (method_setColor L:0x08181f90 = W:0x100e9430): gsGridItem::setColor(colour, true) (L:0x08181de0):
  // setColorize on every submesh of the item's own mesh, then the same for every item of the same root (or the root
  // itself) in the divisions around the root's footprint whose template has no colour map. The scripts paint the stops
  // in the company colour with it (ui.script transportStopCreated). The renderer applies the request (terrain.cpp).
  implement(vm, "gsGridItemController", u"setColor", [](VM& v, Object* s, int, const Value* a) {
    GridObj* grid = nullptr;
    uint32_t i = 0;
    if (controllerItem(v, s, &grid, &i)) grid->itemColorRequests.push_back({i, uint32_t(v.toInt(a[0])), true});    return Value();
  });
  implement(vm, "gsGridItemController", u"getPos", [](VM& v, Object* s, int, const Value*) {
    const GridData::Item* it = controllerItem(v, s);
    return Value::integer(it ? int32_t(uint32_t(uint16_t(it->z)) << 16 | uint16_t(it->x)) : 0);
  });
  implement(vm, "gsGridItemController", u"getSize", [](VM& v, Object* s, int, const Value*) {
    GridObj* grid = nullptr;
    const GridData::Item* it = controllerItem(v, s, &grid);
    if (!it) return Value::integer(0);
    const PickTemplate& tp = pickState(v, grid).templates[it->type];
    ItemFootprint fp = itemFootprint(tp.width, tp.length, tp.diagonals, it->orient);
    return Value::integer(int32_t(uint32_t(fp.sx) << 16 | uint32_t(fp.sz)));
  });
  implement(vm, "gsGridItemController", u"getFlags", [](VM& v, Object* s, int, const Value*) {
    GridObj* grid = nullptr;
    uint32_t i = 0;
    if (!controllerItem(v, s, &grid, &i)) return Value::integer(0);
    return Value::integer(int32_t(itemFlags(*grid->data, pickState(v, grid), i)));
  });
  // activate / deactivate (method_activate L:0x08187650, method_deactivate L:0x08187610 = W:0x100ecca0 / 0x100ecce0):
  // the item's own type (+0x58) becomes GRID_ITEM_NONE or GRID_ITEM_MARKER, then updateMesh and updateGraph. The
  // demonstrations and parades of default.scenario (startAnimationEvent) show and hide their crowd items with them.
  // Port: the renderer buckets an activated marker into its division and rebuilds it; fixGrid and the graphs read the
  // override when they are next built (the lane graph is not rebuilt here).
  auto setItemType = [](VM& v, Object* s, int type) {
    GridObj* grid = nullptr;
    uint32_t i = 0;
    if (!controllerItem(v, s, &grid, &i)) return;
    GridData::Item& it = grid->data->items[i];
    if (it.typeOverride == type) return;
    it.typeOverride = int8_t(type);
    LOG_INFO("grid item %u (%d,%d): %s", i, it.x, it.z, type ? "deactivated" : "activated");
    if (TerrainCache* tc = grid->terrain.get(); tc && tc->cols > 0) {
      int d = itemDivision(*grid->data, *tc, i);
      if (d >= 0) {
        auto& list = tc->divisionItems[size_t(d)];
        if (type != 0x15 && std::find(list.begin(), list.end(), i) == list.end()) list.push_back(i);
        tc->dirtyItems.insert(d);
      }
    }
    gridItemsChangedOutsideBatch(grid, {i});
  };
  implement(vm, "gsGridItemController", u"activate", [setItemType](VM& v, Object* s, int, const Value*) {
    setItemType(v, s, 0);
    return Value();
  });
  implement(vm, "gsGridItemController", u"deactivate", [setItemType](VM& v, Object* s, int, const Value*) {
    setItemType(v, s, 0x15);
    return Value();
  });
  // getStats: the item's traffic word (+0x5c), maintained by the simulation's building pass (not ported yet)
  implement(vm, "gsGridItemController", u"getStats", [](VM&, Object*, int, const Value*) { return Value::integer(0); });
  implement(vm, "gsGridItemController", u"getConnections", [](VM&, Object*, int, const Value*) { return emptyArray(); });

  // ---- gsGridObject place counts (L:0x0817f020 / 0x0817efa0 / 0x0817ef20) ----
  implement(vm, "gsGridObject", u"getHomePlaces", [](VM& v, Object* s, int, const Value* a) {
    auto* o = dynamic_cast<ObjectObj*>(s);
    int i = v.toInt(a[0]);
    return Value::integer(o && i >= 0 && i < 7 ? o->homePlaces[i] : 0);
  });
  implement(vm, "gsGridObject", u"getWorkPlaces", [](VM& v, Object* s, int, const Value* a) {
    auto* o = dynamic_cast<ObjectObj*>(s);
    int i = v.toInt(a[0]);
    return Value::integer(o && i >= 0 && i < 7 ? o->workPlaces[i] : 0);
  });
  implement(vm, "gsGridObject", u"getVisitPlaces", [](VM& v, Object* s, int, const Value* a) {
    auto* o = dynamic_cast<ObjectObj*>(s);
    int i = v.toInt(a[0]);
    return Value::integer(o && i >= 0 && i < 13 ? o->visitPlaces[i] : 0);
  });
}

}  // namespace regs::engine
