// The building pass of gsGrid.tick (W:0x100c48c0.c:278-449; docs/simulation/grid-traffic.md §4.1, §4.3, §4.4): a
// round robin over the map's buckets (16×16 cells), trunc(buckets · dt) of them per tick (one sweep per scaled
// second), with the timer items' countdown and the fires on buildings.
//
// Per visited bucket a fresh 20-bit counter ((rand & 0x1f) << 15 | rand & 0x7fff; 0 while grid.startFire) is counted
// down by each qualifying building (template type GRID_ITEM_BUILDING, both sides > 3 cells); the building where it
// was 0 burns if events are on, it has homes or workplaces, the template "fire-medium" exists and the era has a fire
// engine (the first GRID_VISIT_EMERGENCY_FIRE car record whose era mask meets citizens.modelMask). The fire is an item
// of fire-medium at (x + rand % (sizeX − 6), z + rand % (sizeZ − 6)) on a large building, (x − 1, z − 1) otherwise,
// flags 0x40000000, its countdown 120; the fire engine stands at the road lane nearest the fire (graphNodeNear with no
// lanes of the fire's own: the nearest node with GRID_PATH_ROAD), cell (cellX − 1, cellZ − 1), turned rand & 3, also
// 120. The original tries three such picks and skips one within 3 cells of an earlier one, so with the nearest lane
// only one engine comes. grid.startFire is cleared when a fire starts.
//
// Timer items count down by one per visit of their bucket; the first one of a visit to reach 0 is removed (the timer
// items after it in that visit are not counted down).
//
// Port choices: the countdowns are kept here, not in the item (the port's items have no word field): the fire and
// engines placed here, and any GRID_ITEM_TIMER item met in a visit (from a save) starting at 120. The engines count
// down whatever their template's type is (the original counts only type 0x17 items, so an engine template of another
// type would stay). The traffic heat of §4.2 is not here (only the data views read it).
#include <algorithm>
#include <cstdlib>
#include <random>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/scene3d.h"
#include "engine/sim_graph.h"
#include "engine/sim_state.h"

namespace regs::engine {

using namespace regs::script;
using namespace regs::engine::sim;

constexpr int kGridItemBuilding = 20, kGridItemTimer = 23;

struct SimFires {
  uint64_t generation = ~0ull, version = ~0ull;
  int bucketsX = 0, bucketsZ = 0;
  std::vector<std::vector<uint32_t>> buckets;  // item indices per bucket
  struct TypeInfo {
    bool known = false;
    int type = 0, w = 1, l = 1;
    bool inhabited = false;
  };
  std::vector<TypeInfo> types;
  size_t cursor = 0;
  std::unordered_map<uint32_t, int> timers;  // item → visits left
  std::mt19937 rng{0x5eed1234u};
  int started = 0, removed = 0;
};

namespace {

int rand15(SimFires& f) { return int(f.rng() & 0x7fff); }

const SimFires::TypeInfo& typeOf(VM& vm, GridObj* grid, SimFires& f, uint16_t type) {
  const GridData& g = *grid->data;
  if (f.types.size() < g.types.size()) f.types.resize(g.types.size());
  SimFires::TypeInfo& t = f.types[type];
  if (t.known) return t;
  t.known = true;
  auto it = grid->templates.find(g.types[type]);
  if (it == grid->templates.end()) return t;
  Object* o = it->second.get();
  t.type = fieldInt(vm, o, u"type", 0);
  t.w = std::max(1, fieldInt(vm, o, u"width", 1));
  t.l = std::max(1, fieldInt(vm, o, u"length", 1));
  if (auto* oo = dynamic_cast<ObjectObj*>(o))  // template +0x2e8 homes, +0x304 workplaces (7 classes each)
    for (int k = 0; k < 7; ++k) t.inhabited = t.inhabited || oo->homePlaces[k] || oo->workPlaces[k];
  return t;
}

void rebuild(GridObj* grid, SimFires& f) {
  const GridData& g = *grid->data;
  f.generation = grid->generation;
  f.version = grid->itemsVersion;
  if (f.types.size() != g.types.size()) f.types.assign(g.types.size(), {});
  int shift = int(g.bucketShift);
  f.bucketsX = std::max(1, (g.width + (1 << shift) - 1) >> shift);
  f.bucketsZ = std::max(1, (g.length + (1 << shift) - 1) >> shift);
  f.buckets.assign(size_t(f.bucketsX) * size_t(f.bucketsZ), {});
  for (size_t i = 0; i < g.items.size(); ++i) {
    const auto& it = g.items[i];
    if (it.removed || it.type >= g.types.size()) continue;
    int bx = std::clamp(int(it.x) >> shift, 0, f.bucketsX - 1), bz = std::clamp(int(it.z) >> shift, 0, f.bucketsZ - 1);
    f.buckets[size_t(bz) * size_t(f.bucketsX) + size_t(bx)].push_back(uint32_t(i));
  }
  // countdowns of items that are gone
  for (auto it = f.timers.begin(); it != f.timers.end();)
    it = (it->first >= g.items.size() || g.items[it->first].removed) ? f.timers.erase(it) : std::next(it);
}

// the fire engine of the era: the first GRID_VISIT_EMERGENCY_FIRE (10) record of citizens.addCar whose era mask meets
// citizens.modelMask (citizens+0x5c8)
std::string fireEngine(VM& vm, GridObj* grid) {
  Object* citizens = nullptr;
  try {
    citizens = vm.resolve(vm.getMember(Value::heap(VT::Object, grid), u"citizens")).object();
  } catch (...) {
  }
  if (!citizens) return {};
  int mask = fieldInt(vm, citizens, u"modelMask", 0);
  auto list = citizens->fieldValues.find(u"<carPools>");
  if (list == citizens->fieldValues.end()) return {};
  if (ArrayObj* a = list->second.array())
    for (const Value& r : a->items) {
      ArrayObj* e = r.array();
      if (!e || e->items.size() < 5 || vm.toInt(e->items[0]) != 10) continue;
      if (!(vm.toInt(e->items[2]) & mask)) continue;
      return utf16ToUtf8(vm.toStr(e->items[4]));
    }
  return {};
}

bool gridFlag(VM& vm, GridObj* grid, const char16_t* name) { return fieldInt(vm, grid, name, 0) != 0; }

void startFire(VM& vm, GridObj* grid, SimState& s, SimFires& f, uint32_t item, int sx, int sz) {
  const GridData& g = *grid->data;
  if (gridTypeIndex(vm, grid, "fire-medium") < 0) return;
  std::string engine = fireEngine(vm, grid);
  if (engine.empty()) return;
  const auto it = g.items[item];  // a copy: adding items may grow the list
  int fx = sx > 6 ? it.x + rand15(f) % (sx - 6) : it.x - 1;
  int fz = sz > 6 ? it.z + rand15(f) % (sz - 6) : it.z - 1;
  gridEditBegin(vm, grid);
  int fire = gridEditAddItem(vm, grid, "fire-medium", fx, it.y, fz, 0, 0x40000000u, -1, false);
  if (fire >= 0) f.timers[uint32_t(fire)] = 120;
  std::vector<std::pair<int, int>> picks;
  int engines = 0;
  if (fire >= 0) {
    float cs = g.cellSize;
    render::Vec3 p{(float(fx) + 0.5f - float(g.width) * 0.5f) * cs, float(it.y) * cs / 256.0f,
                   (float(fz) + 0.5f - float(g.length) * 0.5f) * cs};
    for (int k = 0; k < 3; ++k) {
      int nd = s.graph.nearest(p, kPathRoad);
      if (nd < 0) break;
      const LaneNode& n = s.graph.nodes[size_t(nd)];
      bool near = false;
      for (auto [px, pz] : picks) near = near || (std::abs(px - n.cellX) < 3 && std::abs(pz - n.cellZ) < 3);
      if (near) continue;
      picks.push_back({n.cellX, n.cellZ});
      int y = int((n.p0.y + n.p1.y) * 128.0f / cs);
      int truck = gridEditAddItem(vm, grid, engine, n.cellX - 1, y, n.cellZ - 1, int(f.rng() & 3), 0x40000000u, -1, false);
      if (truck >= 0) f.timers[uint32_t(truck)] = 120, ++engines;
    }
  }
  gridEditEnd(vm, grid);
  if (fire < 0) return;
  vm.setMember(Value::heap(VT::Object, grid), u"startFire", Value::integer(0));
  ++f.started;
  LOG_INFO("fires: building %u (%d,%d) burns: fire item %d at (%d,%d), %d %s", item, it.x, it.z, fire, fx, fz, engines,
           engine.c_str());
}

}  // namespace

namespace sim {

void stepBuildingPass(VM& vm, GridObj* grid, SimState& s, float dt) {
  if (!grid->data || dt <= 0) return;
  if (!s.fires) s.fires = std::make_shared<SimFires>();
  SimFires& f = *s.fires;
  if (f.generation != grid->generation || f.version != grid->itemsVersion) rebuild(grid, f);
  const size_t n = f.buckets.size();
  if (!n) return;
  size_t count = std::min(n, size_t(float(n) * dt));  // :278-281, no remainder kept
  // test switch REGS_TEST_FIRES=1: grid.startFire stays on until a fire burns (a fire as soon as a bucket with a
  // qualifying building comes round)
  static const bool forceFire = std::getenv("REGS_TEST_FIRES") != nullptr;
  const bool showMarkers = gridFlag(vm, grid, u"showMarkers");
  for (size_t visit = 0; visit < count; ++visit) {
    f.cursor = (f.cursor + 1) % n;
    uint32_t counter = uint32_t((rand15(f) & 0x1f) << 15 | rand15(f));
    if (gridFlag(vm, grid, u"startFire") || (forceFire && f.started == 0)) counter = 0;
    const GridData& g = *grid->data;
    int expired = -1;
    std::vector<uint32_t> list = f.buckets[f.cursor];  // a copy: a fire edits the items
    for (uint32_t i : list) {
      if (i >= g.items.size() || g.items[i].removed || g.items[i].type >= g.types.size()) continue;
      const auto& it = g.items[i];
      const SimFires::TypeInfo& t = typeOf(vm, grid, f, it.type);
      auto timer = f.timers.find(i);
      if (timer == f.timers.end() && t.type == kGridItemTimer) timer = f.timers.emplace(i, 120).first;
      if (timer != f.timers.end()) {
        if (expired < 0 && --timer->second <= 0) expired = int(i);
        continue;
      }
      if (t.type != kGridItemBuilding || showMarkers) continue;
      int sx = (it.orient & 1) ? t.l : t.w, sz = (it.orient & 1) ? t.w : t.l;
      if (sx <= 3 || sz <= 3) continue;
      if (counter-- != 0) continue;                          // :324-328
      if (gridFlag(vm, grid, u"disableEvents") || !t.inhabited) continue;
      startFire(vm, grid, s, f, i, sx, sz);
    }
    if (expired >= 0) {
      f.timers.erase(uint32_t(expired));
      gridEditBegin(vm, grid);
      gridEditRemoveItem(vm, grid, uint32_t(expired));
      gridEditEnd(vm, grid);
      ++f.removed;
    }
    if (f.generation != grid->generation || f.version != grid->itemsVersion) rebuild(grid, f);
  }
}

}  // namespace sim

}  // namespace regs::engine
