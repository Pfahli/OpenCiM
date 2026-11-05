// Citizens, pedestrians and passengers (docs/simulation/citizens.md, transport.md §5, pathfinding.md §5-6): the save's
// citizens (0x0707) and pedestrians (0x0705), the place pools, the daily cycle with mode choice, walking, driving, and
// riding the player's lines with boarding, fares (vehicle onGetPassenger) and alighting.
// Deviations: the pedestrian search is a single A* over quarter cells and walk layers with the phase-2 costs instead
// of the two-phase search (a small box first, then the original's ±100-cell range around the goal), pedestrians far
// from the camera walk by time without a path and board without walking to the doors, metro stations' doors are taken
// at street level directly (the original's re-snap after its lead-out search).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <queue>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/grid_edit.h"
#include "engine/grid_render.h"
#include "engine/interaction.h"
#include "engine/sim.h"
#include "engine/sim_state.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
using render::Mat4;
using render::Vec3;

namespace sim {
namespace {

constexpr int kVisitHome = 1, kVisitWork = 2;
constexpr uint32_t kStateMask = 0xf0, kIdle = 0, kWalk = 0x10, kDrive = 0x20, kTravel = 0x30, kReplan = 0x40;

SimState* stateOf(GridObj* g) { return g ? g->sim.get() : nullptr; }

// ---- configuration from the gsGridCitizens object ----

Object* module(SimState& s) { return s.citizensModule.get(); }

float moduleFloat(SimState& s, const char16_t* name, float def) {
  Object* m = module(s);
  return m && s.vm ? fieldFloat(*s.vm, m, name, def) : def;
}
int moduleInt(SimState& s, const char16_t* name, int def) {
  Object* m = module(s);
  return m && s.vm ? fieldInt(*s.vm, m, name, def) : def;
}
// values stored by setReasonProbability / setPriceSignificance / setUtilProbability
float tableValue(SimState& s, const char16_t* key, size_t i, float def) {
  Object* m = module(s);
  if (!m) return def;
  auto it = m->fieldValues.find(key);
  ArrayObj* a = it == m->fieldValues.end() ? nullptr : it->second.array();
  if (!a || i >= a->items.size() || a->items[i].isNone()) return def;
  return s.vm->toFloat(a->items[i]);
}
int reason(SimState& s, int cls, int k) { return int(tableValue(s, u"<reason>", size_t(cls * 4 + k), k == 0 ? 55.0f : k == 1 ? 20.0f : 35.0f)); }
float priceSignificance(SimState& s, int cls) { return tableValue(s, u"<priceSig>", size_t(cls), 1.0f); }
float utilProbability(SimState& s, int type) { return tableValue(s, u"<utilProb>", size_t(type), 0.0f); }

// addCharacter / addCar records → the pools (rebuilt when the scripts changed them)
void refreshPools(SimState& s) {
  Object* m = module(s);
  if (!m) return;
  int version = fieldInt(*s.vm, m, u"<poolsVersion>", 0);
  if (version == s.poolsVersion) return;
  s.poolsVersion = version;
  for (auto& p : s.charPools) p.clear();
  for (auto& p : s.carPools) p.clear();
  s.fleetPools.clear();
  auto list = [&](const char16_t* key) {
    auto it = m->fieldValues.find(key);
    return it == m->fieldValues.end() ? nullptr : it->second.array();
  };
  if (ArrayObj* a = list(u"<charPools>"))
    for (const Value& r : a->items) {
      ArrayObj* e = r.array();
      if (!e || e->items.size() < 4) continue;
      int cls = s.vm->toInt(e->items[0]);
      if (cls < 0 || cls > 6) continue;
      CharacterModel cm;
      cm.eraMask = uint32_t(s.vm->toInt(e->items[1]));
      cm.gender = s.vm->toInt(e->items[2]);
      cm.model = modelFor(*s.vm, s.grid, s, utf16ToUtf8(s.vm->toStr(e->items[3])));
      if (cm.model >= 0) s.charPools[cls].push_back(cm);
    }
  if (ArrayObj* a = list(u"<carPools>"))
    for (const Value& r : a->items) {
      ArrayObj* e = r.array();
      if (!e || e->items.size() < 4) continue;
      int type = s.vm->toInt(e->items[0]), cls = s.vm->toInt(e->items[1]);
      if (type < 0 || type > 12 || cls < 0 || cls > 6) continue;
      CarModel car;
      car.eraMask = uint32_t(s.vm->toInt(e->items[2]));
      car.weight = s.vm->toInt(e->items[3]);
      for (size_t k = 4; k < e->items.size(); ++k) {
        int model = modelFor(*s.vm, s.grid, s, utf16ToUtf8(s.vm->toStr(e->items[k])));
        if (model >= 0) car.parts.push_back(model);
      }
      if (type == 0) s.carPools[cls].push_back(car);
      else s.fleetPools[type * 7 + cls].push_back(car);
    }
}

// FUN_100ce250 for a utility trip: a weighted pick from the fleet of the visit type, no ownership roll; null if none
const CarModel* pickFleetCar(SimState& s, int type, int cls) {
  refreshPools(s);
  auto it = s.fleetPools.find(type * 7 + cls);
  if (it == s.fleetPools.end()) return nullptr;
  uint32_t mask = uint32_t(moduleInt(s, u"modelMask", -1));
  int total = 0;
  for (const auto& r : it->second) total += (r.eraMask & mask) ? r.weight : 0;
  if (total <= 0) return nullptr;
  int k = rnd(s) % total;
  for (const auto& r : it->second) {
    if (!(r.eraMask & mask)) continue;
    if (k < r.weight) return r.parts.empty() ? nullptr : &r;
    k -= r.weight;
  }
  return nullptr;
}

uint32_t modelMask(SimState& s) { return uint32_t(moduleInt(s, u"modelMask", -1)); }

// FUN_100ce180: uniform among the class's records of the era (and gender), −1 if none
int pickCharacter(SimState& s, int cls, int gender) {
  refreshPools(s);
  const auto& pool = s.charPools[cls];
  uint32_t mask = modelMask(s);
  int n = 0;
  for (const auto& r : pool) n += (r.eraMask & mask) && (gender < 0 || r.gender == gender);
  if (!n) return -1;
  int k = rnd(s) % n;
  for (size_t i = 0; i < pool.size(); ++i)
    if ((pool[i].eraMask & mask) && (gender < 0 || pool[i].gender == gender) && k-- == 0) return int(i);
  return -1;
}

// FUN_100ce250: no car with probability 1 − carMultiplier, else a weighted pick (an empty record = no car)
int pickCar(SimState& s, int cls) {
  refreshPools(s);
  // the port's traffic density below 1x scales the scripts' carMultiplier (above 1x, background trips add traffic)
  if (rndFloat(s, 0, 1) > moduleFloat(s, u"carMultiplier", 1.0f) * std::min(1.0f, trafficDensity(s))) return -1;
  const auto& pool = s.carPools[cls];
  uint32_t mask = modelMask(s);
  int total = 0;
  for (const auto& r : pool) total += (r.eraMask & mask) ? r.weight : 0;
  if (total <= 0) return -1;
  int k = rnd(s) % total;
  for (size_t i = 0; i < pool.size(); ++i) {
    if (!(pool[i].eraMask & mask)) continue;
    if (k < pool[i].weight) return pool[i].parts.empty() ? -1 : int(i);
    k -= pool[i].weight;
  }
  return -1;
}

const CharacterModel* charModelOf(SimState& s, const Citizen& c) {
  refreshPools(s);
  const auto& pool = s.charPools[c.flags & 0xf];
  return c.charModel >= 0 && size_t(c.charModel) < pool.size() ? &pool[size_t(c.charModel)] : nullptr;
}
const CarModel* carModelOf(SimState& s, const Citizen& c) {
  refreshPools(s);
  const auto& pool = s.carPools[c.flags & 0xf];
  return c.carModel >= 0 && size_t(c.carModel) < pool.size() && !pool[size_t(c.carModel)].parts.empty() ? &pool[size_t(c.carModel)] : nullptr;
}
float walkSpeed(SimState& s, const Citizen& c) {
  const CharacterModel* cm = charModelOf(s, c);
  return cm ? std::max(0.2f, s.models[size_t(cm->model)].speed) : 1.3f;
}

// ---- items ----

struct ItemInfo {
  int x = 0, z = 0, sx = 1, sz = 1, type = 0;
  uint32_t flags = 0;
  ObjectObj* tpl = nullptr;
};
ItemInfo itemInfo(SimState& s, int item) {
  ItemInfo r;
  const GridData& g = *s.grid->data;
  const auto& it = g.items[size_t(item)];
  r.x = it.x;
  r.z = it.z;
  r.flags = it.flags;
  if (it.type < g.types.size())
    if (auto t = s.grid->templates.find(g.types[it.type]); t != s.grid->templates.end()) {
      r.tpl = dynamic_cast<ObjectObj*>(t->second.get());
      int w = std::max(1, fieldInt(*s.vm, t->second.get(), u"width", 1)), l = std::max(1, fieldInt(*s.vm, t->second.get(), u"length", 1));
      r.sx = (it.orient & 1) ? l : w;
      r.sz = (it.orient & 1) ? w : l;
      r.type = fieldInt(*s.vm, t->second.get(), u"type", 0);
      r.flags |= uint32_t(fieldInt(*s.vm, t->second.get(), u"flags", 0));
    }
  return r;
}
// gsGridObject fields the place natives keep for the simulation: the utility visit type of a class's workplaces
// (addWorkPlaces' third argument, template +0x320) and the opening days (addVisitPlaces' open and period, +0x3c0)
int workVisitType(SimState& s, int item, int cls) {
  ItemInfo info = itemInfo(s, item);
  if (!info.tpl) return -1;
  auto it = info.tpl->fieldValues.find(u"<workVisit" + utf8ToUtf16(std::to_string(cls)) + u">");
  return it == info.tpl->fieldValues.end() ? -1 : s.vm->toInt(it->second);
}
uint32_t openDays(SimState& s, int item) {
  ItemInfo info = itemInfo(s, item);
  if (!info.tpl) return 0;
  auto it = info.tpl->fieldValues.find(u"<openDays>");
  return it == info.tpl->fieldValues.end() ? 0 : uint32_t(s.vm->toInt(it->second));
}
int clockDate(SimState& s) {
  Object* m = module(s);
  if (!m) return 0;
  auto it = m->fieldValues.find(u"clock");
  Object* clock = it != m->fieldValues.end() ? s.vm->resolve(it->second).object() : nullptr;
  if (!clock) return 0;
  return s.vm->toInt(s.vm->getMember(Value::heap(VT::Object, clock), u"date"));
}
// the centre in cells: (x + (w >> 1), z + (l >> 1)) (0x1011bf00.c:13-16)
void centreCells(SimState& s, int item, int& cx, int& cz) {
  ItemInfo i = itemInfo(s, item);
  cx = i.x + (i.sx >> 1);
  cz = i.z + (i.sz >> 1);
}
float cellDistance(SimState& s, int a, int b) {
  int ax, az, bx, bz;
  centreCells(s, a, ax, az);
  centreCells(s, b, bx, bz);
  return std::sqrt(float((ax - bx) * (ax - bx) + (az - bz) * (az - bz)));
}

// ---- place pools (§2.1) ----

// registerBuilding (FUN_100d2d90): an item's home, work and visit slots into the pools
void addPlaces(SimState& s, size_t i, float hm, float wm) {
  const GridData& g = *s.grid->data;
  const auto& it = g.items[i];
  if ((it.flags & 0x700000) || it.removed) return;
  ItemInfo info = itemInfo(s, int(i));
  if (!info.tpl || it.x < 0 || it.z < 0 || it.x + info.sx > g.width || it.z + info.sz > g.length) return;
  for (int cls = 0; cls < 7; ++cls) {
    if (int h = info.tpl->homePlaces[cls]) s.freeHomes[cls].insert(s.freeHomes[cls].end(), size_t(std::max(1, int(float(h) * hm + 0.5f))), int(i));
    if (int w = info.tpl->workPlaces[cls]) s.freeWork[cls].insert(s.freeWork[cls].end(), size_t(std::max(1, int(float(w) * wm + 0.5f))), int(i));
  }
  for (int t = 0; t < 13; ++t) s.visitPlaces[t].insert(s.visitPlaces[t].end(), size_t(std::max(0, info.tpl->visitPlaces[t])), int(i));
}

void buildPools(SimState& s) {
  const GridData& g = *s.grid->data;
  // home slots max(1, round(homePlaces · homeMultiplier · regsPopulationDensity)): the port's density knob scales the
  // population with the slots; it must have the value the save was made with before the map loads
  float hm = moduleFloat(s, u"homeMultiplier", 1.0f) * populationDensity(s), wm = moduleFloat(s, u"workplaceMultiplier", 1.0f);
  for (auto& p : s.freeHomes) p.clear();
  for (auto& p : s.freeWork) p.clear();
  for (auto& p : s.visitPlaces) p.clear();
  for (size_t i = 0; i < g.items.size(); ++i) addPlaces(s, i, hm, wm);
}

// FUN_100ce360: a random entry, replaced by the last
int popRandom(SimState& s, std::vector<int>& v) {
  if (v.empty()) return -1;
  int r1 = rnd(s), r2 = rnd(s);
  size_t idx = size_t(((r2 << 12) | (r1 & 0xfff))) % v.size();
  int item = v[idx];
  v[idx] = v.back();
  v.pop_back();
  return item;
}

// §3.1: (rand() & 0xfff) ≤ 2²⁹ / (dx² + dz² + 2¹⁷) from the citizen's home
bool acceptDistance(SimState& s, int home, int cand) {
  int hx, hz, cx, cz;
  centreCells(s, home, hx, hz);
  centreCells(s, cand, cx, cz);
  int64_t d2 = int64_t(cx - hx) * (cx - hx) + int64_t(cz - hz) * (cz - hz);
  return int64_t(rnd(s) & 0xfff) <= 0x20000000LL / (d2 + 0x20000);
}

int findWorkplace(SimState& s, const Citizen& c) {
  int cls = int(c.flags & 0xf);
  auto& pool = s.freeWork[cls];
  if (reason(s, cls, 0) == 0 || pool.empty() || c.home < 0) return -1;
  for (int t = 0; t < 32; ++t) {
    int r1 = rnd(s), r2 = rnd(s);
    size_t idx = size_t(((r2 << 12) | (r1 & 0xfff))) % pool.size();
    if (!acceptDistance(s, c.home, pool[idx])) continue;
    int item = pool[idx];
    pool[idx] = pool.back();
    pool.pop_back();
    return item;
  }
  return -1;
}

int findPlace(SimState& s, const Citizen& c, int type) {
  if (type < 0 || type >= 13 || s.visitPlaces[type].empty() || c.home < 0) return -1;
  const auto& pool = s.visitPlaces[type];
  int cls = int(c.flags & 0xf);
  for (int t = 0; t < 32; ++t) {
    int r1 = rnd(s), r2 = rnd(s);
    int cand = pool[size_t(((r2 << 12) | (r1 & 0xfff))) % pool.size()];
    ItemInfo info = itemInfo(s, cand);
    if (info.tpl && (info.tpl->barredVisitors & (1u << cls))) continue;
    if (type >= 3 && type <= 5)
      if (uint32_t od = openDays(s, cand); od && (od & 0xffff))
        if (uint32_t(clockDate(s) + info.z + info.x) % (od & 0xffff) >= (od >> 16)) continue;
    if (acceptDistance(s, c.home, cand)) return cand;
  }
  return -1;
}

// ---- walk surface and pedestrian paths (pathfinding.md §1.4, §5) ----

// <stem>.bmp of a template: per cell four 2-bit codes per q-cell row from R (pavement), G (open), B (crossing) = 255,
// in the four orientations (gsGridObject::loadFromFile L:0x0817f650.c:65-260)
std::map<const Object*, std::vector<uint32_t>> walkMaps;

const std::vector<uint32_t>* walkMap(SimState& s, ObjectObj* tpl, int w, int l) {
  auto known = walkMaps.find(tpl);
  if (known != walkMaps.end()) return known->second.empty() ? nullptr : &known->second;
  auto& out = walkMaps[tpl];
  std::string path = tpl->pendingPath;
  size_t dot = path.rfind('.');
  if (dot == std::string::npos) return nullptr;
  std::vector<uint8_t> file;
  int bw = 0, bh = 0;
  std::vector<uint32_t> px;
  if (!s.vm->vfs().read(path.substr(0, dot) + ".bmp", file) || !parseBmp(file, bw, bh, px) || bw < w * 4 || bh < l * 4) return nullptr;
  // orientation 0 as stored (rows top-down here); the item rotation is applied when stamping
  out.assign(size_t(w) * size_t(l), 0);
  for (int cz = 0; cz < l; ++cz)
    for (int cx = 0; cx < w; ++cx) {
      uint32_t word = 0;
      for (int qz = 0; qz < 4; ++qz)
        for (int qx = 0; qx < 4; ++qx) {
          uint32_t p = px[size_t(cz * 4 + qz) * size_t(bw) + size_t(cx * 4 + qx)];
          uint32_t code = ((p >> 16) & 0xff) == 0xff ? 2 : ((p >> 8) & 0xff) == 0xff ? 3 : (p & 0xff) == 0xff ? 1 : 0;
          word |= code << (2 * (qx + qz * 4));
        }
      out[size_t(cz) * size_t(w) + size_t(cx)] = word;
    }
  return &out;
}

// <stem>-y.bmp of a template: per cell the walk height offset R·2 − negHeight·256 (L:0x0817f650.c:262-300); the
// stamping puts such items on a layer of their own
std::map<const Object*, std::vector<int32_t>> walkHeights;

const std::vector<int32_t>* walkHeightMap(SimState& s, ObjectObj* tpl, int w, int l) {
  auto known = walkHeights.find(tpl);
  if (known != walkHeights.end()) return known->second.empty() ? nullptr : &known->second;
  auto& out = walkHeights[tpl];
  std::string path = tpl->pendingPath;
  size_t dot = path.rfind('.');
  if (dot == std::string::npos) return nullptr;
  std::vector<uint8_t> file;
  int bw = 0, bh = 0;
  std::vector<uint32_t> px;
  if (!s.vm->vfs().read(path.substr(0, dot) + "-y.bmp", file) || !parseBmp(file, bw, bh, px) || bw < w || bh < l) return nullptr;
  int neg = fieldInt(*s.vm, tpl, u"negHeight", 0);
  out.assign(size_t(w) * size_t(l), 0);
  for (int cz = 0; cz < l; ++cz)
    for (int cx = 0; cx < w; ++cx) out[size_t(cz) * size_t(w) + size_t(cx)] = int32_t((px[size_t(cz) * size_t(bw) + size_t(cx)] >> 16) & 0xff) * 2 - neg * 256;
  return &out;
}

// gsGridItem::getY (0x100e90e0) at the corner (px, pz) of a cell, footprint-relative in world orientation: ramps rise
// 16/32/64/128 per cell with the slope flags 0x10/0x20/0x40/0x80, a bridge ramp from its low end; diagonal footprints
// have their own planes at half the rise
int32_t rampY(int32_t y, int sx, int sz, int orient, uint32_t flags, uint32_t diag, int px, int pz) {
  int sh = (flags & 0x10) ? 4 : (flags & 0x20) ? 5 : (flags & 0x40) ? 6 : (flags & 0x80) ? 7 : 0;
  if (!sh) return y;
  if (flags & 0x4000) y -= ((orient & 1) ? sz : sx) * (1 << sh);
  if (!diag) {
    switch (orient & 3) {
      case 0: return y + px * (1 << sh);
      case 1: return y + (sz - pz) * (1 << sh);
      case 2: return y + (sx - px) * (1 << sh);
      default: return y + pz * (1 << sh);
    }
  }
  int a = int(diag & 0xf), b = int((diag >> 4) & 0xf), c = int((diag >> 8) & 0xf), k = 1 << (sh - 1);
  if (sx == 3 && sz == 3) {
    switch (orient & 3) {
      case 0: return y + (px - a - pz + 3) * k;
      case 1: return y + (6 - b - px - pz) * k;
      case 2: return y + (pz - c - px + 3) * k;
      default: return y + (c + px - 4 + pz) * k;
    }
  }
  if (sx == 12 && sz == 12) {
    switch (orient & 3) {
      case 0: return y + (px - a - pz + 12) * k;
      case 1: return y + (25 - b - px - pz) * k;
      case 2: return y + (pz - c - px + 12) * k;
      default: return y + (c + px - 13 + pz) * k;
    }
  }
  switch (orient & 3) {
    case 0: return y + (sz - a - pz + px) * k;
    case 1: return y + (sz - b - px - pz + sx) * k;
    case 2: return y + (sx - c - px + pz) * k;
    default: return y + (c - sx + px + pz) * k;
  }
}

// FUN_100e7d10 over a whole cell: blocked stays blocked, a road crossing stays a crossing unless the item blocks it,
// pavement and open ground take the item's code
uint32_t mergeWalk(uint32_t a, uint32_t b) {
  uint32_t x = ((~(b >> 1) | ~(a >> 1)) & (b >> 1 | b) & a) | ((a >> 1 | a) & b);
  return ((x ^ (a & b)) & 0x55555555u) ^ (a & b);
}

int32_t cornerHeight(const GridData& g, int x, int z) {
  x = std::clamp(x, 0, g.width - 1), z = std::clamp(z, 0, g.length - 1);
  return g.height[size_t(z) * size_t(g.width) + size_t(x)];
}

// the ground before any item: open on land, blocked on water (the original leaves water open ground)
void resetWalkCells(SimState& s, int x0, int z0, int x1, int z1) {
  const GridData& g = *s.grid->data;
  WalkGrid& wg = s.walk;
  for (int z = z0; z <= z1; ++z)
    for (int x = x0; x <= x1; ++x) {
      size_t ci = size_t(z) * size_t(g.width) + size_t(x);
      wg.words[ci] = (g.flags[ci] & 0x2000) ? 0 : 0xffffffffu;
      wg.height[ci] = -1;
      if (wg.deckList[ci] >= 0) wg.decks[size_t(wg.deckList[ci])].clear();  // the cell keeps its (empty) deck list
    }
}

// DAT_10166450 (not dumped): the q-cells of a cell's covered quadrants (cover bits 0x100 z−, 0x200 x+, 0x400 z+,
// 0x800 x−; a q-cell on a diagonal belongs to both of its quadrants), 2 bits each
uint32_t coverMask(uint32_t cover) {
  if ((cover & 0xF00) == 0xF00) return 0xffffffffu;
  uint32_t mask = 0;
  for (int qz = 0; qz < 4; ++qz)
    for (int qx = 0; qx < 4; ++qx) {
      float dx = float(qx) - 1.5f, dz = float(qz) - 1.5f;
      bool in = (dz < 0 && -dz >= std::fabs(dx) && (cover & 0x100)) || (dx > 0 && dx >= std::fabs(dz) && (cover & 0x200)) ||
                (dz > 0 && dz >= std::fabs(dx) && (cover & 0x400)) || (dx < 0 && -dx >= std::fabs(dz) && (cover & 0x800));
      if (in) mask |= 3u << (2 * (qx + qz * 4));
    }
  return mask;
}

// One item's walk map on the layers, written to its cells inside [rx0, rx1] × [rz0, rz1] in the order items stamp;
// its door candidates come from all its cells. Items with BRIDGE, TUNNEL or ENABLE_BASE (`& 0xc001`) or a -y.bmp
// walk at their own height, per cell: on the ground where the terrain corners are level with it, else on a deck of
// the cell at that height (gsGridItem::getExtraSlot); a covering one blocks the ground under it, and so does a -y.bmp
// one less than 1.25 cells above it (0x100e9730.c:356-431). false: the item stamps nothing
bool stampWalkItem(SimState& s, size_t i, int rx0, int rz0, int rx1, int rz1, bool& layered, std::map<std::string, int>* water) {
  const GridData& g = *s.grid->data;
  WalkGrid& wg = s.walk;
  const auto& it = g.items[i];
  if ((it.flags & 0x500000) || it.removed) return false;
  ItemInfo info = itemInfo(s, int(i));
  if (!info.tpl || info.type == 0x15 || info.type == 0x13) return false;  // markers, trees
  int w = (it.orient & 1) ? info.sz : info.sx, l = (it.orient & 1) ? info.sx : info.sz;  // template size
  const std::vector<int32_t>* ydata = walkHeightMap(s, info.tpl, w, l);
  bool deck = (info.flags & 0xc001) || ydata;
  if (!deck && !(info.flags & 0x4) && info.type != 0x14 && (info.type < 1 || info.type > 0x12)) return false;  // only covering items
  const std::vector<uint32_t>* map = walkMap(s, info.tpl, w, l);
  int32_t base = itemHeight(g, it, info.flags, info.type);
  uint32_t diagonals = uint32_t(fieldInt(*s.vm, info.tpl, u"diagonals", 0));
  ItemFootprint fp = itemFootprint(w, l, diagonals, it.orient);  // the item's cuts rotated with it (+0x54)
  uint32_t diag = fp.diag;
  bool ramp = (info.flags & 0xf0) && (!(info.flags & 0xc000) || !(info.flags & 0x4));
  std::vector<uint32_t> doors;
  for (int cz = 0; cz < info.sz; ++cz)
    for (int cx = 0; cx < info.sx; ++cx) {
      int gx = it.x + cx, gz = it.z + cz;
      if (gx < 0 || gz < 0 || gx >= g.width || gz >= g.length) continue;
      bool write = gx >= rx0 && gx <= rx1 && gz >= rz0 && gz <= rz1;
      // gsGridItem::cellCover: a diagonal item covers some quadrants of a cell, the rest keeps what lies there
      uint32_t cover = diagonals ? itemCellCover(fp, info.flags, cx, cz) : 0xF00;
      if (!cover) continue;
      uint32_t mask = coverMask(cover);
      // world q-cell (qx, qz) of grid cell (cx, cz) → template q-cell under the item's quarter turns. The engine keeps
      // four turned copies of the walk and height maps and picks the one of the item's orientation (+0x44); copy 0
      // is the bitmap as stored (gsGridObject::loadSlotData L:0x081865b0, loadYData L:0x08186c20), so the maps are
      // half a turn off the mesh, which the renderer turns by (orient + 2)·90°. W, L: the world footprint
      auto toTemplate = [&](int wx, int wz, int W, int L, int& tx, int& tz) {
        switch (it.orient & 3) {
          case 0: tx = wx, tz = wz; break;
          case 1: tx = L - 1 - wz, tz = wx; break;
          case 2: tx = W - 1 - wx, tz = L - 1 - wz; break;
          default: tx = wz, tz = W - 1 - wx; break;
        }
      };
      uint32_t out = 0;
      if (map)
        for (int qz = 0; qz < 4; ++qz)
          for (int qx = 0; qx < 4; ++qx) {
            int tx, tz;
            toTemplate(cx * 4 + qx, cz * 4 + qz, info.sx * 4, info.sz * 4, tx, tz);
            uint32_t word = (*map)[size_t(tz >> 2) * size_t(w) + size_t(tx >> 2)];
            uint32_t code = (word >> (2 * ((tx & 3) + (tz & 3) * 4))) & 3;
            out |= code << (2 * (qx + qz * 4));
          }
      size_t ci = size_t(gz) * size_t(g.width) + size_t(gx);
      int level = 0;
      if (!deck) {
        if (write) {
          if ((g.flags[ci] & 0x2000) && (out & mask) && water) (*water)[g.types[it.type]]++;
          wg.words[ci] = (wg.words[ci] & ~mask) | (out & mask);
          // the walking height: an item that does not lock the ground to itself has its own surface (plazas, quays,
          // footpaths on walls); a LOCK_GROUND item lies on the terrain
          if (mask == 0xffffffffu) wg.height[ci] = (info.flags & 0x8) ? -1 : base;
        }
      } else {
        int32_t y = ramp ? rampY(base, info.sx, info.sz, it.orient, info.flags, diag, cx, cz) : base;
        if (ydata) {
          int tx, tz;
          toTemplate(cx, cz, info.sx, info.sz, tx, tz);
          y += (*ydata)[size_t(tz) * size_t(w) + size_t(tx)];
        }
        if (cornerHeight(g, gx, gz) == y && cornerHeight(g, gx + 1, gz) == y && cornerHeight(g, gx, gz + 1) == y &&
            cornerHeight(g, gx + 1, gz + 1) == y) {
          if (write) {
            wg.words[ci] = (wg.words[ci] & ~mask) | (out & mask);  // level with the terrain: the ground itself
            if (mask == 0xffffffffu) wg.height[ci] = -1;
          }
        } else if (write) {
          if (wg.deckList[ci] < 0) wg.deckList[ci] = int32_t(wg.decks.size()), wg.decks.emplace_back();
          auto& list = wg.decks[size_t(wg.deckList[ci])];
          size_t k = 0;
          while (k < list.size() && list[k].height != y) ++k;
          if (k == list.size() && k < 7) list.push_back({y, 0xffffffffu});
          if (k < list.size()) {
            list[k].word = (list[k].word & ~mask) | (mergeWalk(list[k].word, out) & mask);
            level = int(k) + 1;
            wg.maxDecks = std::max(wg.maxDecks, int(list.size()));
          }
          if ((!(info.flags & 0x8000) || (info.flags & 0x4)) &&
              ((info.flags & 0x4) || (ydata && y - cornerHeight(g, gx, gz) <= 0x140)))
            wg.words[ci] &= ~mask;
        } else if (wg.deckList[ci] >= 0) {  // outside the rectangle: the deck it made before
          const auto& list = wg.decks[size_t(wg.deckList[ci])];
          for (size_t k = 0; k < list.size(); ++k)
            if (list[k].height == y) level = int(k) + 1;
        }
      }
      // a door candidate: the item's own pavement in this cell (FUN_100e26b0 insideItem)
      for (int q = 0; q < 16; ++q)
        if ((((out & mask) >> (2 * q)) & 3) == 2) {
          doors.push_back(uint32_t(gz * 4 + (q >> 2) | level << kLayerShift) << 16 | uint32_t(gx * 4 + (q & 3)));
          break;
        }
    }
  wg.doors.erase(int(i));
  if (!doors.empty()) wg.doors[int(i)].cells = std::move(doors);
  layered = deck;
  return true;
}

// the walk layers (pathfinding.md §1.4): every covering item stamps its walk map in file order
void buildWalkGrid(SimState& s) {
  const GridData& g = *s.grid->data;
  WalkGrid& wg = s.walk;
  wg.width = g.width;
  wg.length = g.length;
  wg.words.assign(size_t(g.width) * size_t(g.length), 0xffffffffu);
  wg.height.assign(size_t(g.width) * size_t(g.length), -1);
  wg.deckList.assign(size_t(g.width) * size_t(g.length), -1);
  wg.decks.clear();
  wg.doors.clear();
  wg.maxDecks = 0;
  resetWalkCells(s, 0, 0, g.width - 1, g.length - 1);
  size_t stamped = 0, layered = 0;
  std::map<std::string, int> waterByTemplate;
  for (size_t i = 0; i < g.items.size(); ++i) {
    bool deck = false;
    if (!stampWalkItem(s, i, 0, 0, g.width - 1, g.length - 1, deck, debugSim() ? &waterByTemplate : nullptr)) continue;
    ++stamped;
    layered += deck;
  }
  LOG_INFO("citizens: walk surface from %zu items (%zu on their own height), %zu cells with decks, up to %d per cell", stamped,
           layered, wg.decks.size(), wg.maxDecks);
  if (debugSim()) {
    std::vector<std::pair<int, std::string>> top;
    for (auto& [name, n] : waterByTemplate) top.push_back({n, name});
    std::sort(top.rbegin(), top.rend());
    std::string list;
    for (size_t k = 0; k < top.size() && k < 12; ++k) list += " " + top[k].second + ":" + std::to_string(top[k].first);
    LOG_INFO("citizens: water cells made walkable by items:%s", list.c_str());
  }
}

// the height (1/256 cell) of a q-cell's walk layer: a deck's own, the ground item's surface, else the terrain
// interpolated between the cell's corners
float layerHeight(SimState& s, int qx, int qz) {
  const GridData& g = *s.grid->data;
  int level = qz >> kLayerShift;
  qz &= kQzMask;
  int cx = std::clamp(qx >> 2, 0, g.width - 1), cz = std::clamp(qz >> 2, 0, g.length - 1);
  if (level > 0)
    if (const WalkDeck* d = s.walk.deck(cx, cz, level)) return float(d->height);
  int32_t item = s.walk.height.empty() ? -1 : s.walk.height[size_t(cz) * size_t(g.width) + size_t(cx)];
  if (item >= 0) return float(item);
  auto h = [&](int x, int z) { return float(cornerHeight(g, x, z)); };
  float fx = (float(qx & 3) + 0.5f) * 0.25f, fz = (float(qz & 3) + 0.5f) * 0.25f;
  return (h(cx, cz) * (1 - fx) + h(cx + 1, cz) * fx) * (1 - fz) + (h(cx, cz + 1) * (1 - fx) + h(cx + 1, cz + 1) * fx) * fz;
}

// the q-cell centre on its walk layer
Vec3 qWorld(SimState& s, int qx, int qz) {
  const GridData& g = *s.grid->data;
  float cs = g.cellSize, y = layerHeight(s, qx, qz);
  int z = qz & kQzMask;
  return {((float(qx) - 2.0f * float(g.width)) * 0.25f + 0.125f) * cs, y * cs / 256.0f, ((float(z) - 2.0f * float(g.length)) * 0.25f + 0.125f) * cs};
}

// FUN_100e1d40: the layer a pedestrian at height y steps onto at (qx, qz): the ground if walkable there within ±0xa0
// (0.625 cell), else the first deck of the cell that is; −1 if none. From the ground the ground stays usable at any
// height difference, because the port lays some item surfaces on the ground that the original keeps on layers
int stepLayer(SimState& s, int qx, int qz, float y, bool fromGround) {
  qz &= kQzMask;
  if (qx < 0 || qz < 0 || qx >= s.walk.width * 4 || qz >= s.walk.length * 4) return -1;
  int ground = s.walk.code(qx, qz);
  if (ground && std::abs(layerHeight(s, qx, qz) - y) <= 160.0f) return 0;
  int list = s.walk.deckList[size_t(qz >> 2) * size_t(s.walk.width) + size_t(qx >> 2)];
  if (list >= 0) {
    const auto& decks = s.walk.decks[size_t(list)];
    for (size_t k = 0; k < decks.size(); ++k)
      if (((decks[k].word >> (2 * ((qx & 3) + (qz & 3) * 4))) & 3) && std::abs(float(decks[k].height) - y) <= 160.0f) return int(k) + 1;
  }
  return ground && fromGround ? 0 : -1;
}

// the item's footprint pavement leads to pavement outside it (FUN_100e6610's cell search, on q-cells here)
bool doorLeadsOut(SimState& s, const ItemInfo& info, uint32_t door) {
  int x0 = info.x * 4 - 8, z0 = info.z * 4 - 8, W = info.sx * 4 + 16, L = info.sz * 4 + 16;
  auto inside = [&](int qx, int qz) { return qx >= info.x * 4 && qz >= info.z * 4 && qx < (info.x + info.sx) * 4 && qz < (info.z + info.sz) * 4; };
  std::vector<uint8_t> seen(size_t(W) * size_t(L), 0);
  std::vector<uint32_t> open{door};
  int dx = int(door & 0xffff) - x0, dz = int((door >> 16) & kQzMask) - z0;
  if (dx < 0 || dz < 0 || dx >= W || dz >= L) return false;
  seen[size_t(dz) * size_t(W) + size_t(dx)] = 1;
  static const int dxs[8] = {1, 0, -1, 0, 1, 1, -1, -1}, dzs[8] = {0, 1, 0, -1, 1, -1, 1, -1};
  while (!open.empty()) {
    uint32_t q = open.back();
    open.pop_back();
    int qx = int(q & 0xffff), qz = int(q >> 16);
    float y = layerHeight(s, qx, qz);
    for (int d = 0; d < 8; ++d) {
      int nx = qx + dxs[d], nz = (qz & kQzMask) + dzs[d];
      if (nx < x0 || nz < z0 || nx >= x0 + W || nz >= z0 + L) continue;
      uint8_t& m = seen[size_t(nz - z0) * size_t(W) + size_t(nx - x0)];
      if (m) continue;
      int lv = stepLayer(s, nx, nz, y, (qz >> kLayerShift) == 0);
      if (lv < 0) continue;
      m = 1;
      int nq = nz | lv << kLayerShift;
      if (!inside(nx, nz) && s.walk.code(nx, nq) == 2) return true;
      open.push_back(uint32_t(nq) << 16 | uint32_t(nx));
    }
  }
  return false;
}

// FUN_100e6610: the door of an item, a random one of its own pavement q-cells that leads out (from its walk map), else
// a pavement q-cell just outside it, found on an outward square spiral (on the ground or a deck)
bool doorOf(SimState& s, int item, int& qx, int& qz) {
  ItemInfo info = itemInfo(s, item);
  // metro stations (the station body 0x10, and the platform stop pieces on elevated 0xf or underground 0xe metro
  // track): their platform cells lead out onto the rest of the platform, so the door is on the platform at the
  // station's depth or height (waiters stand there; the stairs items connect it to the street). Only without such a
  // door the spiral below takes one at street level
  const bool station = info.type == 0x10 || ((info.type == 0xe || info.type == 0xf) && (info.flags & 0x2000));
  if (auto d = s.walk.doors.find(item); d != s.walk.doors.end()) {
    WalkDoors& doors = d->second;
    if (!doors.checked) {
      std::vector<uint32_t> good;
      for (uint32_t q : doors.cells)
        if (doorLeadsOut(s, info, q)) good.push_back(q);
      doors.cells = std::move(good);
      doors.checked = true;
    }
    if (!doors.cells.empty()) {
      uint32_t q = doors.cells[size_t(rnd(s)) % doors.cells.size()];
      qx = int(q & 0xffff);
      qz = int(q >> 16);
      return true;
    }
  }
  int cx = info.x + (info.sx >> 1), cz = info.z + (info.sz >> 1);
  auto inside = [&](int x, int z) { return x >= info.x && z >= info.z && x < info.x + info.sx && z < info.z + info.sz; };
  bool roadLike = info.type >= 1 && info.type <= 0x12 && !station;  // stops and roads: their own pavement counts
  for (int r = 0; r <= 25; ++r)
    for (int z = cz - r; z <= cz + r; ++z)
      for (int x = cx - r; x <= cx + r; ++x) {
        if (std::max(std::abs(x - cx), std::abs(z - cz)) != r || (!roadLike && inside(x, z))) continue;
        if (x < 0 || z < 0 || x >= s.walk.width || z >= s.walk.length) continue;
        size_t ci = size_t(z) * size_t(s.walk.width) + size_t(x);
        int list = s.walk.deckList[ci];
        int levels = station ? 1 : 1 + (list >= 0 ? int(s.walk.decks[size_t(list)].size()) : 0);
        for (int lv = 0; lv < levels; ++lv) {
          uint32_t w = lv == 0 ? s.walk.words[ci] : s.walk.decks[size_t(list)][size_t(lv - 1)].word;
          for (int q = 0; q < 16; ++q)
            if (((w >> (2 * q)) & 3) == 2) {
              qx = x * 4 + (q & 3);
              qz = (z * 4 + (q >> 2)) | lv << kLayerShift;
              return true;
            }
        }
      }
  qx = cx * 4 + 2;
  qz = cz * 4 + 2;
  return false;
}

// §5.3 costs on one A* over q-cells and their layers: 8 directions, ×3 road crossings, ×4 open ground, 0.05 per 1/256
// cell of climb, inside a box around both ends. The goal is its q-cell on any layer
bool walkPathIn(SimState& s, int qx0, int qz0, int qx1, int qz1, std::vector<uint32_t>& out, bool wide, bool last);

// The search box is the ends' bounding box plus 16 cells; when that holds no way (a block whose exits face away from
// the goal), the original's phase-1 range, every cell within ±100 cells of the goal (pathfinding.md §5.2)
bool walkPath(SimState& s, int qx0, int qz0, int qx1, int qz1, std::vector<uint32_t>& out) {
  return walkPathIn(s, qx0, qz0, qx1, qz1, out, false, false) || walkPathIn(s, qx0, qz0, qx1, qz1, out, true, true);
}

bool walkPathIn(SimState& s, int qx0, int qz0, int qx1, int qz1, std::vector<uint32_t>& out, bool wide, bool last) {
  out.clear();
  int l0 = qz0 >> kLayerShift;
  qz0 &= kQzMask, qz1 &= kQzMask;
  int pad = 64;
  int x0 = std::max(0, std::min(qx0, qx1) - pad), x1 = std::min(s.walk.width * 4 - 1, std::max(qx0, qx1) + pad);
  int z0 = std::max(0, std::min(qz0, qz1) - pad), z1 = std::min(s.walk.length * 4 - 1, std::max(qz0, qz1) + pad);
  if (wide) {  // ±100 cells around the goal; a start outside it has no way, as in the original
    x0 = std::max(0, qx1 - 400), x1 = std::min(s.walk.width * 4 - 1, qx1 + 403);
    z0 = std::max(0, qz1 - 400), z1 = std::min(s.walk.length * 4 - 1, qz1 + 403);
  }
  const int cap = wide ? 600000 : 60000;
  int W = x1 - x0 + 1, L = z1 - z0 + 1;
  if (W <= 0 || L <= 0 || qx0 < x0 || qx0 > x1 || qz0 < z0 || qz0 > z1 || qx1 < x0 || qx1 > x1 || qz1 < z0 || qz1 > z1) return false;
  // an end off the walk surface (a car door on the road, a car parked inside a multi-storey garage, the middle of a
  // station's tracks): the nearest walkable q-cell within 10 cells, as the original snaps both ends of a search
  // (FUN_100e26b0 without an item)
  auto snap = [&](int& qx, int& qz, int& lv, bool fromGround) {
    float y = layerHeight(s, qx, qz | lv << kLayerShift);
    for (int r = 1; r <= 40; ++r)
      for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx) {
          if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
          int x = qx + dx, z = qz + dz;
          if (x < x0 || x > x1 || z < z0 || z > z1) continue;
          int l = stepLayer(s, x, z, y, fromGround);
          if (l >= 0) {
            qx = x, qz = z, lv = l;
            return;
          }
        }
  };
  if (!s.walk.code(qx0, qz0 | l0 << kLayerShift)) snap(qx0, qz0, l0, l0 == 0);
  {
    int list = s.walk.deckList[size_t(qz1 >> 2) * size_t(s.walk.width) + size_t(qx1 >> 2)];
    bool open = s.walk.code(qx1, qz1) != 0;  // the goal counts on any layer
    if (list >= 0)
      for (const WalkDeck& d : s.walk.decks[size_t(list)]) open = open || ((d.word >> (2 * ((qx1 & 3) + (qz1 & 3) * 4))) & 3);
    if (!open) {
      int l1 = 0;
      snap(qx1, qz1, l1, true);
    }
  }
  int K = 1;  // layers in the box
  if (s.walk.maxDecks > 0)
    for (int cz = z0 >> 2; cz <= z1 >> 2; ++cz)
      for (int cx = x0 >> 2; cx <= x1 >> 2; ++cx)
        if (int list = s.walk.deckList[size_t(cz) * size_t(s.walk.width) + size_t(cx)]; list >= 0)
          K = std::max(K, 1 + int(s.walk.decks[size_t(list)].size()));
  if (l0 >= K) l0 = 0;
  static thread_local std::vector<float> cost;
  static thread_local std::vector<int> parent;
  cost.assign(size_t(W) * size_t(L) * size_t(K), -1.0f);
  parent.assign(cost.size(), -1);
  using E = std::pair<float, int>;
  std::priority_queue<E, std::vector<E>, std::greater<E>> open;
  auto idx = [&](int x, int z, int lv) { return ((z - z0) * W + (x - x0)) * K + lv; };
  auto h = [&](int x, int z) {
    float dx = float(std::abs(x - qx1)), dz = float(std::abs(z - qz1));
    return std::max(dx, dz) + 0.4142135f * std::min(dx, dz);
  };
  int start = idx(qx0, qz0, l0), goal = -1;
  cost[size_t(start)] = 0;
  open.push({h(qx0, qz0), start});
  static const int dxs[8] = {1, 0, -1, 0, 1, 1, -1, -1}, dzs[8] = {0, 1, 0, -1, 1, -1, 1, -1};
  int expansions = 0;
  while (!open.empty() && expansions < cap * K) {
    auto [f, p] = open.top();
    open.pop();
    int cell = p / K, pl = p % K, px = cell % W + x0, pz = cell / W + z0;
    if (f - h(px, pz) > cost[size_t(p)] + 1e-3f) continue;
    if (px == qx1 && pz == qz1) {
      goal = p;
      break;
    }
    ++expansions;
    float y = layerHeight(s, px, pz | pl << kLayerShift);
    for (int d = 0; d < 8; ++d) {
      int nx = px + dxs[d], nz = pz + dzs[d];
      if (nx < x0 || nx > x1 || nz < z0 || nz > z1) continue;
      int nl = stepLayer(s, nx, nz, y, pl == 0);
      bool end = nx == qx1 && nz == qz1;
      if (nl < 0 && !end) continue;
      nl = std::max(nl, 0);
      if (nl >= K) continue;
      int nzl = nz | nl << kLayerShift;
      int v = s.walk.code(nx, nzl);
      float c = (d < 4 ? 1.0f : 1.4142135f) + 0.05f * std::abs(layerHeight(s, nx, nzl) - y);
      if (v == 1) c *= 3;
      else if (v == 3) c *= 4;
      int q = idx(nx, nz, nl);
      float ng = cost[size_t(p)] + c;
      if (cost[size_t(q)] >= 0 && cost[size_t(q)] <= ng) continue;
      cost[size_t(q)] = ng;
      parent[size_t(q)] = p;
      open.push({ng + h(nx, nz), q});
    }
  }
  if (goal < 0) {
    if (last && debugSim()) {  // why: the expansion cap, or every reachable q-cell of the box searched (disconnected)
      ++s.walkFailKind[expansions >= cap * K ? 0 : 1];
      static int shown = 0;
      if (shown++ < 25) {
        auto itemsAt = [&](int qx, int qz) {  // the items covering a q-cell's cell, with their base height
          std::string names;
          const GridData& g = *s.grid->data;
          int cx = qx >> 2, cz = qz >> 2;
          for (size_t i = 0; i < g.items.size(); ++i) {
            if (g.items[i].removed) continue;
            ItemInfo in = itemInfo(s, int(i));
            if (cx < in.x || cz < in.z || cx >= in.x + in.sx || cz >= in.z + in.sz) continue;
            names += " " + g.types[g.items[i].type] + "@" + std::to_string(g.items[i].y);
          }
          return names;
        };
        LOG_INFO("sim: walk failed (%s): (%d,%d L%d code %d) -> (%d,%d code %d), box %dx%d, %d layers, %d expanded; "
                 "start:%s; goal:%s",
                 expansions >= cap * K ? "cap" : "no way", qx0, qz0, l0, s.walk.code(qx0, qz0 | l0 << kLayerShift), qx1,
                 qz1, s.walk.code(qx1, qz1), W, L, K, expansions, itemsAt(qx0, qz0).c_str(), itemsAt(qx1, qz1).c_str());
      }
    }
    return false;
  }
  for (int q = goal; q >= 0; q = parent[size_t(q)]) {
    int cell = q / K, lv = q % K;
    out.push_back(uint32_t((cell / W + z0) | lv << kLayerShift) << 16 | uint32_t(cell % W + x0));
  }
  std::reverse(out.begin(), out.end());
  return true;
}

// ---- pedestrians ----

int newCharacter(SimState& s) {
  if (!s.freeCharacters.empty()) {
    int i = s.freeCharacters.back();
    s.freeCharacters.pop_back();
    s.characters[size_t(i)] = Character();
    return i;
  }
  s.characters.emplace_back();
  return int(s.characters.size()) - 1;
}

void removeCharacter(SimState& s, int i) {
  if (i < 0) return;
  Character& ch = s.characters[size_t(i)];
  if (!ch.alive) return;
  ch.alive = false;
  if (Object* o = ch.controller.get()) {
    o->fieldValues[u"<instance>"] = Value::boolean(false);
    if (!o->destroyed) s.vm->destroy(o);
  }
  ch.controller = Ref<Object>();
  s.freeCharacters.push_back(i);
}

// a walk from q-cell to q-cell: a real path near the camera, else a timed walk along the straight line
void walkTo(SimState& s, Character& ch, int qx0, int qz0, int qx1, int qz1) {
  ch.next = 0;
  ch.virtualLeft = -1;
  Vec3 a = qWorld(s, qx0, qz0), b = qWorld(s, qx1, qz1);
  ch.goal = b;
  Vec3 e = a - s.eye;
  if (s.eyeKnown && e.x * e.x + e.z * e.z < 450.0f * 450.0f) {
    if (!walkPath(s, qx0, qz0, qx1, qz1, ch.path)) {
      // no way (onPathError): the original's pedestrian keeps an empty path and is done at once; it never walks
      // through blocked cells. It reappears at the goal.
      ch.path = {uint32_t(qz1) << 16 | uint32_t(qx1)};
      ch.next = 1;
      ch.pos = b;
      ++s.walkFailed;
      return;
    }
    for (uint32_t q : ch.path) ++s.walkCodes[s.walk.code(int(q & 0xffff), int(q >> 16))];
    ++s.walkPaths;
    return;
  }
  Vec3 d = b - a;
  ch.virtualLeft = std::sqrt(d.x * d.x + d.z * d.z) / std::max(0.2f, ch.speed);
  ch.path = {uint32_t(qz1) << 16 | uint32_t(qx1)};
  ch.next = 1;
}

int spawnCharacter(SimState& s, Citizen& c, int ci, int qx, int qz) {
  const CharacterModel* cm = charModelOf(s, c);
  if (!cm) return -1;
  int i = newCharacter(s);
  Character& ch = s.characters[size_t(i)];
  ch.alive = true;
  ch.model = cm->model;
  ch.citizen = ci;
  ch.speed = walkSpeed(s, c);
  ch.pos = qWorld(s, qx, qz);
  ch.path = {uint32_t(qz) << 16 | uint32_t(qx)};
  c.character = i;
  return i;
}

void stepCharacter(SimState& s, Character& ch, float dt) {
  // gsGridCharacter::move: the walk cycle at the walking speed, idle when standing
  if (ch.model >= 0 && ch.virtualLeft < 0)
    if (ObjectObj* o = s.models[size_t(ch.model)].object; o && o->data)
      animateCharacter(*s.vm, o, ch.anim, ch.done() ? "idle" : "walk", dt, ch.done() ? 1.0f : ch.speed);
  if (ch.virtualLeft >= 0) {
    ch.virtualLeft = std::max(0.0f, ch.virtualLeft - dt);
    if (ch.virtualLeft == 0) ch.pos = ch.goal;
    return;
  }
  float left = ch.speed * dt;
  while (left > 0 && ch.next < ch.path.size()) {
    uint32_t q = ch.path[ch.next];
    Vec3 t = qWorld(s, int(q & 0xffff), int(q >> 16));
    Vec3 d = t - ch.pos;
    float len = std::sqrt(d.x * d.x + d.z * d.z);
    if (len > 1e-4f) ch.yaw = std::atan2(d.x, d.z);
    if (len <= left) {
      ch.pos = t;
      left -= len;
      ++ch.next;
    } else {
      ch.pos = ch.pos + d * (left / len);
      left = 0;
    }
  }
}

// ---- animals (grid-traffic.md §7, terrain-characters-citizens.md §2.2) ----

// a spawn point item (GRID_ITEM_SPAWNPOINT 0x16) turns into one animal of its template and goes away: offsetx
// (+0x264) 0 a bird high above it, 1 a ground animal on its cell (0x100ca490.c:65-90)
int spawnAnimal(SimState& s, size_t item) {
  const GridData& g = *s.grid->data;
  const auto& it = g.items[item];
  ItemInfo info = itemInfo(s, int(item));
  if (!info.tpl) return -1;
  int kind = fieldInt(*s.vm, info.tpl, u"offsetx", 0) == 0 ? 1 : 2;
  auto t = g.types[it.type];
  int model = modelFor(*s.vm, s.grid, s, t);
  if (model < 0) return -1;
  int i = newCharacter(s);
  Character& ch = s.characters[size_t(i)];
  ch.alive = true;
  ch.model = model;
  ch.animal = uint8_t(kind);
  ch.speed = std::max(0.2f, s.models[size_t(model)].speed);
  int qx = 4 * it.x + (kind == 2), qz = 4 * it.z + (kind == 2);
  int y = itemHeight(g, it, info.flags, info.type);
  ch.anchor = uint16_t((qx >> 5 & 0xff) | (qz >> 5 & 0xff) << 8);
  ch.pos = qWorld(s, qx, qz);
  if (kind == 1) {
    ch.pos.y = float(y + 0x600 + (rnd(s) & 0xfff)) * g.cellSize / 256.0f;
    ch.target[0] = qx, ch.target[1] = int(ch.pos.y * 256.0f / g.cellSize), ch.target[2] = qz;
  }
  ch.path = {uint32_t(qz) << 16 | uint32_t(qx)};
  ch.next = 1;
  return i;
}

// birds: every 0.25 s one of them, when close to its target, picks the next one a few q-cells on, pulled towards
// its home block, at ground + 0x600..0xdff (0x100ca490.c:92-171); in between it flies straight at it
void stepBird(SimState& s, Character& ch, float dt) {
  const GridData& g = *s.grid->data;
  float cs = g.cellSize;
  Vec3 to{((float(ch.target[0]) - 2.0f * float(g.width)) * 0.25f + 0.125f) * cs, float(ch.target[1]) * cs / 256.0f,
          ((float(ch.target[2]) - 2.0f * float(g.length)) * 0.25f + 0.125f) * cs};
  Vec3 d = to - ch.pos;
  float len = std::sqrt(dot(d, d)), step = ch.speed * cs * dt;
  if (len > 1e-3f) {
    ch.pos = ch.pos + d * (std::min(step, len) / len);
    if (d.x * d.x + d.z * d.z > 1e-6f) ch.yaw = std::atan2(d.x, d.z);
  }
  if (ch.model >= 0)
    if (ObjectObj* o = s.models[size_t(ch.model)].object; o && o->data) animateCharacter(*s.vm, o, ch.anim, "walk", dt, ch.speed);
}

void retargetBird(SimState& s, Character& ch) {
  const GridData& g = *s.grid->data;
  float cs = g.cellSize;
  Vec3 to{((float(ch.target[0]) - 2.0f * float(g.width)) * 0.25f + 0.125f) * cs, ch.pos.y,
          ((float(ch.target[2]) - 2.0f * float(g.length)) * 0.25f + 0.125f) * cs};
  Vec3 d = to - ch.pos;
  if (d.x * d.x + d.z * d.z >= 64.0f) return;
  float l = std::sqrt(d.x * d.x + d.z * d.z);
  float dx = l > 1e-4f ? d.x / l : 0, dz = l > 1e-4f ? d.z / l : 0;
  for (int axis = 0; axis < 2; ++axis) {
    int& q = ch.target[axis ? 2 : 0];
    int home = axis ? ch.anchor >> 8 : ch.anchor & 0xff, block = q >> 5;
    int pull = home > block ? 1 : home < block ? -1 : 0;
    int limit = (axis ? g.length : g.width) * 4 - 1;
    q = std::clamp(q + rnd(s) % 5 - 2 + int(15.0f * (axis ? dz : dx)) + pull, 0, limit);
  }
  int qx = ch.target[0], qz = ch.target[2];
  ch.target[1] = int(layerHeight(s, qx, qz)) + 0x600 + (rnd(s) & 0x7ff);
}

// ground animals: every 10 s one of them, when its walk is over, gets 64 random steps over open ground
// (0x100ca490.c:172-183, 258-330): a blocked step turns by one, a second blocked one ends the walk
void wanderAnimal(SimState& s, Character& ch) {
  if (!ch.done()) return;
  static const int dxs[8] = {1, 0, -1, 0, 1, 1, -1, -1}, dzs[8] = {0, 1, 0, -1, 1, -1, 1, -1};
  uint32_t q = ch.path.empty() ? 0 : ch.path.back();
  int qx = int(q & 0xffff), qz = int(q >> 16);
  std::vector<uint32_t> path{q};
  for (int k = 0; k < 64; ++k) {
    int d = rnd(s) & 7, nx = qx + dxs[d], nz = (qz & kQzMask) + dzs[d];
    if (s.walk.code(nx, nz | (qz & ~kQzMask)) != 3) {
      d = (d + ((rnd(s) & 1) ? 1 : 7)) & 7;
      nx = qx + dxs[d], nz = (qz & kQzMask) + dzs[d];
      if (s.walk.code(nx, nz | (qz & ~kQzMask)) != 3) break;
    }
    qx = nx, qz = nz | (qz & ~kQzMask);
    path.push_back(uint32_t(qz) << 16 | uint32_t(qx));
  }
  ch.path = std::move(path);
  ch.next = 1;
  ch.virtualLeft = -1;
}

void stepAnimals(VM& vm, SimState& s, float dt) {
  // spawn points left on the map (a new game): each becomes its animal, the item is removed
  if (!s.spawnPoints.empty()) {
    gridEditBegin(vm, s.grid);
    for (uint32_t item : s.spawnPoints)
      if (item < s.grid->data->items.size() && !s.grid->data->items[item].removed && spawnAnimal(s, item) >= 0)
        gridEditRemoveItem(vm, s.grid, item);
    s.spawnPoints.clear();
    gridEditEnd(vm, s.grid);
  }
  std::vector<int> birds, ground;
  for (size_t i = 0; i < s.characters.size(); ++i)
    if (s.characters[i].alive && s.characters[i].animal) (s.characters[i].animal == 1 ? birds : ground).push_back(int(i));
  s.birdAcc += float(birds.size()) * dt;
  for (; s.birdAcc >= 0.25f && !birds.empty(); s.birdAcc -= 0.25f) retargetBird(s, s.characters[size_t(birds[size_t(s.birdCursor++) % birds.size()])]);
  s.animalAcc += float(ground.size()) * dt;
  for (; s.animalAcc >= 10.0f && !ground.empty(); s.animalAcc -= 10.0f) wanderAnimal(s, s.characters[size_t(ground[size_t(s.animalCursor++) % ground.size()])]);
  for (int i : birds) stepBird(s, s.characters[size_t(i)], dt);
}

// ---- controllers and events ----

Object* citizenController(SimState& s, int ci) {
  Citizen& c = s.citizens[size_t(ci)];
  if (!c.alive) return nullptr;
  if (!c.controller) {
    ClassInfo* cls = s.vm->findClass("gsGridCitizenController");
    if (!cls) return nullptr;
    c.controller = s.vm->newObject(cls, s.grid);
    if (auto* t = dynamic_cast<TransportObj*>(c.controller.get())) {
      t->sim = s.grid->sim;
      t->index = ci;
    }
  }
  return c.controller.get();
}

Object* characterController(SimState& s, int i) {
  Character& ch = s.characters[size_t(i)];
  if (!ch.alive) return nullptr;
  if (!ch.controller) {
    ClassInfo* cls = s.vm->findClass("gsGridCharacterController");
    if (!cls) return nullptr;
    ch.controller = s.vm->newObject(cls, s.grid);
    if (auto* t = dynamic_cast<TransportObj*>(ch.controller.get())) {
      t->sim = s.grid->sim;
      t->index = i;
    }
    Object* o = ch.controller.get();
    o->fieldValues[u"isCharacter"] = Value::boolean(true);
    o->fieldValues[u"<instance>"] = Value::boolean(true);
    if (ch.model >= 0) o->fieldValues[u"<model>"] = Value::heap(VT::Object, s.models[size_t(ch.model)].object);
    // the pose at once: a controller made while the game is paused gets no tick update until it runs again
    setPoseFields(o, {ch.pos, {std::sin(ch.yaw), 0, std::cos(ch.yaw)}});
  }
  return ch.controller.get();
}

// events fire only when someone listens (FUN_1001c330); the controller is created on demand
void fireCitizen(SimState& s, int ci, const char16_t* ev, std::vector<Value> args = {}) {
  Citizen& c = s.citizens[size_t(ci)];
  if (c.controller) s.vm->fireEvent(c.controller.get(), ev, args);
}
void fireCitizens(SimState& s, int ci, const char16_t* ev) {
  if (!s.citizensModule) return;
  auto it = s.citizensModule->events.find(ev);
  if (it == s.citizensModule->events.end() || it->second.empty()) return;
  if (Object* ctrl = citizenController(s, ci)) s.vm->fireEvent(s.citizensModule.get(), ev, {Value::heap(VT::Object, ctrl)});
}

// ---- stops ----

LineStopState& nodeState(SimState& s, int node) {
  auto [l, k] = s.lineNodes[size_t(node)];
  return s.lines[size_t(l)].stops[size_t(k)];
}
LineState& nodeLine(SimState& s, int node) { return s.lines[size_t(s.lineNodes[size_t(node)].first)]; }
int nextNode(SimState& s, int node) {
  auto [l, k] = s.lineNodes[size_t(node)];
  const LineState& line = s.lines[size_t(l)];
  int n = int(line.stops.size());
  if (k + 1 < n) return line.firstNode + k + 1;
  return line.closed ? line.firstNode : -1;
}
int stopPassengers(SimState& s, Object* stop, int delta) {
  if (!stop) return 0;
  int v = fieldInt(*s.vm, stop, u"passengerCount", 0) + delta;
  stop->fieldValues[u"passengerCount"] = Value::integer(std::max(0, v));
  return v;
}
// FUN_101110c0: 3 if two or more mood-3 waiters, 2 if any mood-2, 1 if any mood-1
void updateStopMood(SimState& s, int node) {
  LineStopState& st = nodeState(s, node);
  if (!st.stop) return;
  int m3 = 0, m2 = 0, m1 = 0;
  for (const LineState& l : s.lines)
    for (const LineStopState& o : l.stops)
      if (o.stop == st.stop) m1 += o.mood[0], m2 += o.mood[1], m3 += o.mood[2];
  int mood = m3 >= 2 ? 3 : m2 ? 2 : m1 ? 1 : 0;
  if (fieldInt(*s.vm, st.stop.get(), u"mood", 0) != mood) {
    st.stop->fieldValues[u"mood"] = Value::integer(mood);
    s.vm->fireEvent(st.stop.get(), u"onMoodChange", {Value::integer(mood)});
  }
}
int moodOf(const Citizen& c) { return (c.flags & 0x4000000) ? 3 : (c.flags & 0x2000000) ? 2 : (c.flags & 0x1000000) ? 1 : 0; }
void setMood(SimState& s, int ci, int mood) {
  Citizen& c = s.citizens[size_t(ci)];
  int old = moodOf(c);
  if (old == mood) return;
  if (c.lineNode >= 0) {
    LineStopState& st = nodeState(s, c.lineNode);
    if (old) --st.mood[old - 1];
    if (mood) ++st.mood[mood - 1];
  }
  c.flags = (c.flags & ~0x7000000u) | (mood ? (0x1000000u << (mood - 1)) : 0u);
  if (c.lineNode >= 0) updateStopMood(s, c.lineNode);
  if (c.character >= 0) fireCitizens(s, ci, u"onCitizenMood");  // the handler pins the mood on the pedestrian
}
void leaveStop(SimState& s, int ci) {
  Citizen& c = s.citizens[size_t(ci)];
  if (c.lineNode < 0) return;
  LineStopState& st = nodeState(s, c.lineNode);
  int mood = moodOf(c);
  if (mood) --st.mood[mood - 1];
  st.waiting = std::max(0, st.waiting - 1);
  stopPassengers(s, st.stop.get(), -1);
  int node = c.lineNode;
  c.lineNode = -1;
  updateStopMood(s, node);
}
void enterStop(SimState& s, int ci, int node) {
  Citizen& c = s.citizens[size_t(ci)];
  c.lineNode = node;
  LineStopState& st = nodeState(s, node);
  ++st.waiting;
  stopPassengers(s, st.stop.get(), 1);
  int mood = moodOf(c);
  if (mood) ++st.mood[mood - 1];
}

// ---- trips ----

// FUN_100cc6c0
void endTrip(SimState& s, int ci, bool keepCar) {
  Citizen& c = s.citizens[size_t(ci)];
  removeCharacter(s, c.character);
  c.character = -1;
  if (!keepCar && c.car >= 0) {
    Mover& m = s.movers[size_t(c.car)];
    if (m.live && m.citizen == ci) {
      m.citizen = -1;
      despawn(*s.vm, s, c.car);
    }
    c.car = -1;
    c.flags &= ~0x100000u;
  }
  c.hasRoute = false;
  c.route = Route();
  c.toCar = false;
  if ((c.flags & 0x100) && (c.flags & kStateMask) == kTravel && c.vehicle < 0 && c.lineNode >= 0) {
    // a door walk cut short: its seat is free again (the 30 s rule would clear it too)
    int vi = nodeState(s, c.lineNode).docked;
    if (vi >= 0) s.vehicles[size_t(vi)].boardingReserved = std::max(0, s.vehicles[size_t(vi)].boardingReserved - 1);
    c.flags &= ~0x100u;
  }
  leaveStop(s, ci);
  if (c.vehicle >= 0) {
    VehicleState& v = s.vehicles[size_t(c.vehicle)];
    v.passengerCount = std::max(0, v.passengerCount - 1);
    c.vehicle = -1;
  }
  uint32_t clear = 0xff0u | 0x7000000u;
  if (c.target >= 0 && c.target == c.work) clear |= 0xff000u;
  c.flags &= ~clear;
  c.start = c.target >= 0 ? c.target : c.home;
  c.target = -1;
  c.driveStart = c.driveEnd = -1;
  c.usedTransport = 0;
  c.waitTime = 0;
  fireCitizen(s, ci, u"onIdle");
}

// FUN_100cbd50 / FUN_100cbdf0
int rollReason(SimState& s, const Citizen& c) {
  int cls = int(c.flags & 0xf);
  int w = reason(s, cls, 0), sh = reason(s, cls, 1), le = reason(s, cls, 2);
  int r = c.work >= 0 ? rnd(s) % 100 : (100 - w > 0 ? rnd(s) % (100 - w) + w : 99);
  return r < w ? 2 : r < w + sh ? 3 : r < w + sh + le ? 4 : 5;
}
void planDay(SimState& s, Citizen& c) {
  uint32_t q = 0;
  int pos = 0;
  for (int i = 0; i < 3; ++i) {
    int r = rollReason(s, c);
    bool dup = false;
    for (int k = 0; k < pos; k += 6) dup = dup || int((q >> k) & 0x3f) == r;
    if (dup) continue;
    q |= uint32_t(r) << pos;
    pos += 6;
    if (r == kVisitWork && c.work >= 0) {
      int u = workVisitType(s, c.work, int(c.flags & 0xf));
      if (u > 5 && u < 13 && rndFloat(s, 0, 1) < utilProbability(s, u)) {
        q |= uint32_t(u) << pos;
        q |= uint32_t(kVisitWork) << (pos + 6);
        pos += 12;
      }
    }
  }
  q |= uint32_t(kVisitHome) << pos;
  c.tripQueue = q;
}

int resolveTarget(SimState& s, Citizen& c, int code) {
  if (code == kVisitHome) return c.home;
  if (code == kVisitWork) return c.work;
  return findPlace(s, c, code);
}

// FUN_1011a200: Dijkstra over line nodes with randomised waiting and riding terms, fares once per transport type
bool transitRoute(SimState& s, int fromItem, int toItem, float speed, float ps, Route& out, float& fareSum) {
  if (s.lineNodes.empty()) return false;
  int x0, z0, x1, z1;
  centreCells(s, fromItem, x0, z0);
  centreCells(s, toItem, x1, z1);
  float cs = s.graph.cellSize, w = cs / speed;
  size_t N = s.lineNodes.size();
  std::vector<float> best(N, -1.0f);
  std::vector<int> parent(N, -1);
  std::vector<uint32_t> paid(N, 0);
  std::vector<uint8_t> closed(N, 0);
  struct E {
    float g;
    uint32_t paid;
    int node;
    bool operator>(const E& o) const { return g > o.g || (g == o.g && paid > o.paid); }
  };
  std::priority_queue<E, std::vector<E>, std::greater<E>> open;
  auto stopCells = [&](int item, int& x, int& z) { centreCells(s, item, x, z); };
  auto attr = [&](const LineStopState& st) { return st.stop ? fieldFloat(*s.vm, st.stop.get(), u"attractiveness", 1.0f) : 1.0f; };
  auto quality = [&](const LineState& l) {
    float a = l.object ? fieldFloat(*s.vm, l.object.get(), u"attractiveness", 1.0f) : 1.0f, mean = 0;
    for (int vi : l.vehicles) mean += s.vehicles[size_t(vi)].attractiveness;
    if (!l.vehicles.empty()) mean /= float(l.vehicles.size());
    return a * mean / (1 + 0.01f * l.avgWait);
  };
  auto fare = [&](const LineState& l) {
    float price = l.object ? fieldFloat(*s.vm, l.object.get(), u"ticketPrice", 0) : 0;
    return std::max(price - 200.0f, 1.0f) * ps;
  };
  auto usable = [&](int node) {
    const LineState& l = nodeLine(s, node);
    const LineStopState& st = nodeState(s, node);
    if (!l.object || st.item < 0 || !st.reachable || (st.flags & 1)) return false;
    auto it = l.object->fieldValues.find(u"active");
    bool active = it == l.object->fieldValues.end() || it->second.isNone() || s.vm->resolve(it->second).toBool();
    return active && nextNode(s, node) >= 0 && !l.vehicles.empty();
  };
  // boarding candidates within the catchment of their departure stop
  for (size_t n = 0; n < N; ++n) {
    if (!usable(int(n))) continue;
    const LineStopState& st = nodeState(s, int(n));
    const LineState& l = nodeLine(s, int(n));
    int sx, sz;
    stopCells(st.item, sx, sz);
    float catchment = st.stop ? fieldFloat(*s.vm, st.stop.get(), u"catchmentArea", 100.0f) : 100.0f;
    float dx = float(sx - x0), dz = float(sz - z0);
    if ((dx * dx + dz * dz) * cs * cs > catchment * catchment) continue;
    float q = quality(l);
    float g = w * std::sqrt(dx * dx + dz * dz) + 6 * rndFloat(s, 0, 1 - 0.8f * attr(st)) + rndFloat(s, 1, 2 - 0.8f * q) * st.cost + fare(l);
    uint32_t mask = 1u << l.type;
    if (best[n] < 0 || g < best[n]) {
      best[n] = g;
      paid[n] = mask;
      parent[n] = -1;
      open.push({g, mask, int(n)});
    }
  }
  float bestTotal = -1;
  int bestNode = -1;
  while (!open.empty()) {
    E e = open.top();
    open.pop();
    if (closed[size_t(e.node)] || e.g > best[size_t(e.node)] + 1e-4f) continue;
    if (bestNode >= 0 && bestTotal <= e.g) break;
    closed[size_t(e.node)] = 1;
    int arr = nextNode(s, e.node);
    const LineStopState& as = nodeState(s, arr);
    int ax, az;
    stopCells(as.item, ax, az);
    if (std::abs(ax - x1) <= 50 && std::abs(az - z1) <= 50) {  // alight here and walk
      float dx = float(ax - x1), dz = float(az - z1), total = e.g + w * std::sqrt(dx * dx + dz * dz);
      if (bestNode < 0 || total < bestTotal) bestTotal = total, bestNode = e.node;
    }
    auto relax = [&](int n, float g, uint32_t mask) {
      if (closed[size_t(n)] || !usable(n)) return;
      if (best[size_t(n)] >= 0 && g >= best[size_t(n)]) return;
      best[size_t(n)] = g;
      paid[size_t(n)] = mask;
      parent[size_t(n)] = e.node;
      open.push({g, mask, n});
    };
    const LineState& l = nodeLine(s, e.node);
    relax(arr, e.g + rndFloat(s, 1, 2 - 0.8f * quality(l)) * as.cost + 5.0f, e.paid);  // ride on
    for (int t : nodeState(s, e.node).transfers) {
      const LineStopState& ts = nodeState(s, t);
      const LineState& tl = nodeLine(s, t);
      int tx, tz;
      stopCells(ts.item, tx, tz);
      float dx = float(tx - ax), dz = float(tz - az);
      uint32_t bit = 1u << tl.type;
      float g = e.g + w * std::sqrt(dx * dx + dz * dz) + 6 * rndFloat(s, 2, 3 - 0.8f * attr(ts)) +
                rndFloat(s, 1, 2 - 0.8f * quality(tl)) * ts.cost + ((e.paid & bit) ? 0.0f : fare(tl));
      relax(t, g, e.paid | bit);
    }
  }
  if (bestNode < 0) return false;
  out = Route();
  out.cost = bestTotal;
  for (int n = bestNode; n >= 0; n = parent[size_t(n)]) out.nodes.push_back(n);
  std::reverse(out.nodes.begin(), out.nodes.end());
  fareSum = 0;
  uint32_t types = 0;
  for (int n : out.nodes) {
    const LineState& l = nodeLine(s, n);
    if (!(types & (1u << l.type))) fareSum += fare(l), types |= 1u << l.type;
  }
  return true;
}

// FUN_100e24f0: a random node with all mask bits of the item's own paths, else the nearest one to its centre
int itemNode(SimState& s, int item, uint32_t mask) {
  std::vector<int> found;
  for (int n : s.graph.itemNodes[size_t(item)])
    if ((s.graph.nodes[size_t(n)].flags & mask) == mask) found.push_back(n);
  if (!found.empty()) return found[size_t(rnd(s)) % found.size()];
  return s.graph.nearest(itemCentre(*s.vm, s.grid, item), mask);
}

// the idle state (FUN_100d18f0): pick the next trip, price walking, driving and transit, go
void startTrip(SimState& s, int ci) {
  Citizen& c = s.citizens[size_t(ci)];
  int cls = int(c.flags & 0xf);
  if (c.tripQueue == 0) {
    int jobless = moduleInt(s, u"joblessRate", 0);
    if (rnd(s) % 100 < jobless) {
      if (c.work >= 0) s.freeWork[cls].push_back(c.work), c.work = -1, ++s.unemployed[cls];
    } else if (c.work < 0) {
      c.work = findWorkplace(s, c);
      if (c.work >= 0) --s.unemployed[cls];
    }
    planDay(s, c);
    c.start = c.home;
    c.target = -1;
  }
  if (!(c.flags & 0xff000) && (rnd(s) & 0x1f) == 0) {  // new looks now and then
    const CharacterModel* cm = charModelOf(s, c);
    int nc = pickCharacter(s, cls, cm ? cm->gender : -1);
    if (nc >= 0) c.charModel = nc;
    c.carModel = pickCar(s, cls);
  }
  int tripType = 0;
  while (c.target < 0 && c.tripQueue) {
    int code = int(c.tripQueue & 0x3f);
    c.tripQueue >>= 6;
    c.target = resolveTarget(s, c, code);
    tripType = code;
    if (c.target >= 0 && code > 5) c.flags |= uint32_t(code) << 12, ++s.utilityTrips;
  }
  if (c.target < 0 || c.target == c.start || c.start < 0) {
    endTrip(s, ci, true);
    return;
  }
  ++s.trips;
  removeCharacter(s, c.character);  // a pedestrian kept from the last trip starts over at the start's door
  c.character = -1;
  c.flags &= ~0x400u;
  float v = walkSpeed(s, c), cs = s.graph.cellSize, ps = priceSignificance(s, cls);
  bool utility = (c.flags & 0xff000) != 0;
  float W = utility ? -1.0f : cellDistance(s, c.start, c.target) * cs / v;
  // driving: a road START node at the start, a road END node at the target (citizens.md §5.3)
  // below 1x traffic a car owner takes it only on that share of trips, so a loaded city thins out at once
  float td = trafficDensity(s);
  const Mover* parked = c.car >= 0 && s.movers[size_t(c.car)].live && s.movers[size_t(c.car)].parked ? &s.movers[size_t(c.car)] : nullptr;
  bool hasCar = (parked || carModelOf(s, c) != nullptr) && (td >= 1.0f || rndFloat(s, 0, 1) < td);
  if (!hasCar) parked = nullptr;  // below 1x a parked car is left (and removed) on the trips it is not taken
  float D = -1, fuel = 0;
  std::vector<int> drive;
  if ((hasCar && W >= 100.0f) || utility || parked) {
    // from the parked car: the START road node nearest to it (FUN_100e21e0(graph, car, 0x10100)), else one of the start's
    c.driveStart = parked ? s.graph.nearest(s.graph.nodes[size_t(parked->track.nodes[0])].p0, 0x10100) : itemNode(s, c.start, 0x10100);
    c.driveEnd = itemNode(s, c.target, 0x20100);
    float cost = 0;
    if (c.driveStart >= 0 && c.driveEnd >= 0 && s.graph.search(c.driveStart, c.driveEnd, false, drive, cost) && !drive.empty()) {
      Vec3 a = itemCentre(*s.vm, s.grid, c.start) - s.graph.nodes[size_t(c.driveStart)].p0;
      Vec3 b = itemCentre(*s.vm, s.grid, c.target) - s.graph.nodes[size_t(c.driveEnd)].p1;
      fuel = ps * cost * 0.1f;
      D = cost + fuel + (std::sqrt(a.x * a.x + a.z * a.z) + std::sqrt(b.x * b.x + b.z * b.z)) / v;
    }
  }
  // transit
  float T = -1, F = 0;
  Route route;
  bool hasRoute = !utility && transitRoute(s, c.start, c.target, v, ps, route, F);
  if (hasRoute) T = route.cost;
  if (hasRoute && (D < 0 || T < D) && (W < 0 || T < W)) {
    c.flags = (c.flags & ~kStateMask) | kTravel;
    c.route = route;
    c.hasRoute = true;
    ++s.travelCount[cls], ++s.canTravelCount[cls];
  } else if (D < 0 || (W >= 0 && W <= D && !parked)) {
    ++s.walkCount[cls];
    if (hasRoute) {
      ++s.canTravelCount[cls];
      if (T - F < W) ++s.priceTooHighCount[cls];
    }
    if (W < 0 || W * v > 250.0f) {  // too far to walk: arrives without moving
      fireCitizens(s, ci, u"onCitizenArrive");
      endTrip(s, ci, false);
    } else {
      c.flags = (c.flags & ~kStateMask) | kWalk;
    }
  } else {
    c.flags = (c.flags & ~kStateMask) | kDrive;
    if (!utility) {
      ++s.driveCount[cls];
      if (hasRoute) {
        ++s.canTravelCount[cls];
        if (T - F < D - fuel) ++s.priceTooHighCount[cls];
      }
    }
  }
  if ((c.flags & kStateMask) != kDrive && c.car >= 0 && s.movers[size_t(c.car)].parked) {  // removed for walk/transit
    Mover& m = s.movers[size_t(c.car)];
    m.citizen = -1;
    despawn(*s.vm, s, c.car);
    c.car = -1;
    c.flags &= ~0x100000u;
  }
  // the cycle timer (§4.3): an event venue holds the citizen until it opens again
  int mn = moduleInt(s, u"minCycleDuration", 0), mx = moduleInt(s, u"maxCycleDuration", 600);
  uint32_t od = tripType >= 3 && tripType <= 5 && c.target >= 0 ? openDays(s, c.target) : 0;
  if (od && (od & 0xffff)) {
    ItemInfo ti = itemInfo(s, c.target);
    uint32_t period = od & 0xffff, open = od >> 16;
    uint32_t d = (period - uint32_t(clockDate(s) + ti.z + ti.x) % period) - open;  // unsigned, as the original
    c.cycleTimer = int(std::min<uint64_t>(std::max<uint64_t>(10, uint64_t(d) * 216 / 25), 0x7fffffff));
  } else {
    c.cycleTimer = mx > mn ? rnd(s) % (mx - mn + 1) + mn : mn;
  }
}

// ---- states ----

void stepWalk(SimState& s, int ci) {
  Citizen& c = s.citizens[size_t(ci)];
  if (c.character < 0) {
    int qx0, qz0, qx1, qz1;
    doorOf(s, c.start, qx0, qz0);
    doorOf(s, c.target, qx1, qz1);
    int i = spawnCharacter(s, c, ci, qx0, qz0);
    if (i < 0) {
      fireCitizens(s, ci, u"onCitizenArrive");
      endTrip(s, ci, false);
      return;
    }
    walkTo(s, s.characters[size_t(i)], qx0, qz0, qx1, qz1);
    if (Object* ctrl = characterController(s, i)) fireCitizen(s, ci, u"onSpawnCharacter", {Value::heap(VT::Object, ctrl)});
    return;
  }
  Character& ch = s.characters[size_t(c.character)];
  if (!ch.done()) return;
  if (c.flags & 0x600) {
    c.flags &= ~0x600u;
    int qx, qz;
    doorOf(s, c.target, qx, qz);
    uint32_t q = ch.path.empty() ? 0 : ch.path.back();
    walkTo(s, ch, int(q & 0xffff), int(q >> 16), qx, qz);
    return;
  }
  fireCitizens(s, ci, u"onCitizenArrive");
  endTrip(s, ci, false);
}

// DRIVE (FUN_100ceee0): the car starts at the start's road node; arrival comes through carArrived
void stepDrive(SimState& s, int ci) {
  Citizen& c = s.citizens[size_t(ci)];
  if (c.flags & 0x200) {  // walking from the car to the target
    if (c.character < 0 || s.characters[size_t(c.character)].done()) {
      fireCitizens(s, ci, u"onCitizenArrive");
      endTrip(s, ci, true);
    }
    return;
  }
  if (c.flags & 0x100) {  // driving; at speed now and then a driver turns reckless (1/131072 per processing)
    if (c.car >= 0) {
      Mover& m = s.movers[size_t(c.car)];
      if (m.live && m.speed > 1 && !m.reckless && !m.crashing && !fieldInt(*s.vm, s.grid, u"disableEvents", 0) &&
          (rnd(s) & 0x7fff) == 0 && (rnd(s) & 3) == 0)
        m.reckless = true;
      // test switch REGS_TEST_CRASH=<n>: drivers within 150 units of the camera turn reckless until n crashes happened,
      // so a crash can be filmed (the normal rate is far too low to catch one on screen)
      static const int testCrashes = std::getenv("REGS_TEST_CRASH") ? std::atoi(std::getenv("REGS_TEST_CRASH")) : 0;
      if (testCrashes > s.crashes && m.live && m.speed > 1 && !m.reckless && !m.crashing && s.eyeKnown &&
          !m.track.nodes.empty()) {
        Vec3 d = s.graph.nodes[size_t(m.track.nodes[m.track.at(m.dist)])].p0 - s.eye;
        if (d.x * d.x + d.z * d.z < 150.0f * 150.0f) m.reckless = true;
      }
    }
    return;
  }
  Mover* parked = c.car >= 0 && s.movers[size_t(c.car)].live && s.movers[size_t(c.car)].parked ? &s.movers[size_t(c.car)] : nullptr;
  if (parked && !c.toCar) {  // phase 0x100: walk from the start to the parked car
    const GridData& g = *s.grid->data;
    Vec3 at = s.graph.nodes[size_t(parked->track.nodes[0])].p0;
    int qx1 = int(std::lround((at.x / g.cellSize + float(g.width) * 0.5f) * 4)), qz1 = int(std::lround((at.z / g.cellSize + float(g.length) * 0.5f) * 4));
    int qx0, qz0;
    doorOf(s, c.start, qx0, qz0);
    if (c.character < 0) spawnCharacter(s, c, ci, qx0, qz0);
    if (c.character >= 0) {
      Character& ch = s.characters[size_t(c.character)];
      uint32_t q = ch.path.empty() ? uint32_t(qz0) << 16 | uint32_t(qx0) : ch.path.back();
      walkTo(s, ch, int(q & 0xffff), int(q >> 16), qx1, qz1);
    }
    c.toCar = true;
    return;
  }
  if (c.toCar && c.character >= 0 && !s.characters[size_t(c.character)].done()) return;
  std::vector<Unit> parkedUnits;
  if (parked) {  // at the car: it leaves its spot and drives off from the road
    parkedUnits = parked->units;
    parked->citizen = -1;
    despawn(*s.vm, s, c.car);
    c.car = -1;
    c.flags &= ~0x100000u;
    ++s.parkedReused;
  }
  c.toCar = false;
  removeCharacter(s, c.character);
  c.character = -1;
  int utility = int((c.flags >> 12) & 0xff);
  const CarModel* car = utility > 5 ? pickFleetCar(s, utility, int(c.flags & 0xf)) : carModelOf(s, c);
  if ((!car && parkedUnits.empty()) || c.driveStart < 0 || c.driveEnd < 0) {
    fireCitizens(s, ci, u"onCitizenArrive");
    endTrip(s, ci, false);
    return;
  }
  if (!canEnter(s, c.driveStart, -2, false)) return;  // wait until the spawn node is free
  std::vector<int> path;
  float cost = 0;
  if (!s.graph.search(c.driveStart, c.driveEnd, false, path, cost) || path.empty()) {
    fireCitizens(s, ci, u"onCitizenArrive");
    endTrip(s, ci, false);
    return;
  }
  std::vector<Unit> units = parkedUnits;
  if (units.empty())
    for (int m : car->parts) units.push_back({m, false, 0});
  int id = spawnTraffic(*s.vm, s, units, c.driveStart, path, false);
  Mover& m = s.movers[size_t(id)];
  m.citizen = ci;
  // paint from the colour map seeded with the citizen id (§7.2); the saved seed keeps its low half
  for (Unit& u : m.units)
    if (u.model >= 0 && s.models[size_t(u.model)].colorMap) u.tint = paint(s, u.model, c.id), u.seed = uint16_t(c.id);
  c.car = id;
  c.flags |= 0x100;
  ++s.carsSpawned;
  if (m.controller) fireCitizen(s, ci, u"onSpawnVehicle", {Value::heap(VT::Object, m.controller.get())});
}

// TRAVEL (FUN_100cc990)
void stepTravel(SimState& s, int ci) {
  Citizen& c = s.citizens[size_t(ci)];
  int n = int(c.route.nodes.size());
  if (!c.hasRoute || n == 0) {
    endTrip(s, ci, false);
    return;
  }
  // riding (§8.4)
  if (c.vehicle >= 0) {
    VehicleState& v = s.vehicles[size_t(c.vehicle)];
    LineState& line = s.lines[size_t(v.line)];
    const Mover* m = v.mover >= 0 ? &s.movers[size_t(v.mover)] : nullptr;
    if (!m || !m->live) {
      endTrip(s, ci, false);
      return;
    }
    if ((v.brokenHalted || m->crashing) && m->speed < 0.1f) {  // evacuation: out at the stop or the car, mood 3, plan anew
      v.passengerCount = std::max(0, v.passengerCount - 1);
      c.vehicle = -1;
      c.flags |= 0x200;
      int item = line.stops[size_t(v.stopB)].item;
      int qx, qz;
      doorOf(s, item, qx, qz);
      if (m->crashing && !m->track.nodes.empty()) {
        const GridData& g = *s.grid->data;
        Vec3 at = s.graph.nodes[size_t(m->track.nodes[m->track.at(m->dist)])].p0 + m->crashOffset;
        qx = int(std::lround((at.x / g.cellSize + float(g.width) * 0.5f) * 4));
        qz = int(std::lround((at.z / g.cellSize + float(g.length) * 0.5f) * 4));
      }
      int i = spawnCharacter(s, c, ci, qx, qz);
      if (i >= 0)
        if (Object* ctrl = characterController(s, i)) fireCitizen(s, ci, u"onSpawnCharacter", {Value::heap(VT::Object, ctrl)});
      setMood(s, ci, 3);
      c.hasRoute = false;
      c.route = Route();
      c.replanFrom = item;
      c.flags = (c.flags & ~kStateMask) | kReplan;
      ++s.evacuated;
      return;
    }
    bool docked = v.stopB >= 0 && line.stops[size_t(v.stopB)].docked == c.vehicle;
    int nodeA = line.firstNode + v.stopA, nodeB = line.firstNode + v.stopB;
    if (!docked) {
      if (c.route.leg < n && c.route.nodes[size_t(c.route.leg)] == nodeA) ++c.route.leg;
      return;
    }
    if (c.route.leg < n && c.route.nodes[size_t(c.route.leg)] == nodeB) return;  // rides on
    // alight at stopB
    v.passengerCount = std::max(0, v.passengerCount - 1);
    ++v.passengersTransported;
    if (line.object)
      line.object->fieldValues[u"passengersTransported"] = Value::integer(fieldInt(*s.vm, line.object.get(), u"passengersTransported", 0) + 1);
    c.vehicle = -1;
    c.flags |= 0x200;
    ++s.alighted;
    int qx, qz;
    doorOf(s, line.stops[size_t(v.stopB)].item, qx, qz);
    int i = spawnCharacter(s, c, ci, qx, qz);
    if (i >= 0)
      if (Object* ctrl = characterController(s, i)) fireCitizen(s, ci, u"onSpawnCharacter", {Value::heap(VT::Object, ctrl)});
    return;
  }
  // waiting (§8.2-8.3)
  if (c.lineNode >= 0) {
    LineStopState& st = nodeState(s, c.lineNode);
    LineState& line = nodeLine(s, c.lineNode);
    int vi = st.docked;
    Character* walker = c.character >= 0 ? &s.characters[size_t(c.character)] : nullptr;
    // in the vehicle: the pedestrian goes, the citizen rides (0x100cc990.c:405-449)
    auto board = [&](VehicleState& v) {
      removeCharacter(s, c.character);
      c.character = -1;
      c.vehicle = vi;
      ++v.passengerCount;
      ++st.passengersTransported;
      st.waitingTime += c.waitTime;
      line.avgWait = 0.95f * line.avgWait + 0.05f * float(c.waitTime);
      leaveStop(s, ci);
      bool alreadyPaid = (c.usedTransport >> line.type) & 1;
      if (Object* ctrl = citizenController(s, ci))
        s.vm->fireEvent(v.object.get(), u"onGetPassenger", {Value::heap(VT::Object, ctrl), Value::integer(c.waitTime), Value::boolean(alreadyPaid)});
      c.usedTransport |= 1u << line.type;
      c.waitTime = 0;
      ++s.boarded;
      fireCitizen(s, ci, u"onSpawnVehicle", {Value::heap(VT::Object, v.object.get())});
    };
    if (c.flags & 0x100) {  // walking to the doors (flag 0x100): board on arrival if the vehicle is still docked
      if (walker && !walker->done()) return;
      c.flags &= ~0x100u;
      if (vi < 0) {
        endTrip(s, ci, false);
        return;
      }
      VehicleState& v = s.vehicles[size_t(vi)];
      v.boardingReserved = std::max(0, v.boardingReserved - 1);
      board(v);
      return;
    }
    if (vi >= 0) {
      VehicleState& v = s.vehicles[size_t(vi)];
      // a waiter starts boarding when it stands at its place (within 3 units, FUN_100c7870), the doors are open
      // (stopTime < 10), and no waiter of a higher mood tier is still waiting: mood 2 first, then mood 1, then the rest
      int mood = moodOf(c);
      bool ahead = (st.mood[1] > 0 && mood < 2) || (st.mood[0] > 0 && mood < 1);
      bool standing = !walker || walker->done();
      if (v.capacity > v.passengerCount + v.boardingReserved && !v.broken && v.stopTime < 10.0f && !ahead && standing) {
        if (mood && mood < 3) setMood(s, ci, 0);  // cleared when boarding starts
        // the walk to the doors is shown for pedestrians on screen; off-screen ones (timed walks, a port shortcut) and
        // ones far from the doors (a platform away from the stop's entrance) board at once
        Vec3 door, gap;
        bool walk = walker && walker->virtualLeft < 0 && vehicleDoorPoint(s, v, walker->pos, door) &&
                    (gap = door - walker->pos, gap.x * gap.x + gap.z * gap.z < 40.0f * 40.0f);
        if (walk) {
          // from where the pedestrian stands (its position; the deck from the end of its last walk) to the doors on
          // the same deck
          const GridData& g = *s.grid->data;
          auto qOf = [&](float w, int n) { return int(std::lround((w / g.cellSize + float(n) * 0.5f) * 4)); };
          int level = walker->path.empty() ? 0 : int(walker->path.back() >> 16) & ~kQzMask;
          ++v.boardingReserved;  // vehicle +0x174: holds the doors open until the walker arrives (or 30 s)
          ++s.doorWalks;
          c.flags |= 0x100;
          walkTo(s, *walker, qOf(walker->pos.x, g.width), qOf(walker->pos.z, g.length) | level, qOf(door.x, g.width),
                 qOf(door.z, g.length) | level);
          return;
        }
        board(v);  // no pedestrian to walk (far from the camera or loaded without one)
        return;
      }
    }
    int p = int(c.id & 0xff) + 256;
    if ((c.waitTime & 7) == 0 && !((c.usedTransport >> line.type) & 1) && line.object) {  // the fare check
      float W = cellDistance(s, st.item, c.target) * s.graph.cellSize / walkSpeed(s, c);
      float price = fieldFloat(*s.vm, line.object.get(), u"ticketPrice", 0);
      if (W * 0.4f < (price - 300.0f) * priceSignificance(s, int(c.flags & 0xf))) c.waitTime = std::max(c.waitTime, p);
    }
    ++c.waitTime;
    if (c.waitTime == p >> 1) setMood(s, ci, 1);
    else if (c.waitTime == (3 * p) >> 2) setMood(s, ci, 2);
    else if (c.waitTime > p) {  // gives up: walks back, then is done
      setMood(s, ci, 3);
      c.route.leg = n;
      leaveStop(s, ci);
      if (c.character < 0) {
        endTrip(s, ci, false);
        return;
      }
      {
        Character& ch = s.characters[size_t(c.character)];
        int qx, qz;
        doorOf(s, c.start, qx, qz);
        uint32_t q = ch.path.empty() ? 0 : ch.path.back();
        walkTo(s, ch, int(q & 0xffff), int(q >> 16), qx, qz);
      }
    }
    return;
  }
  // walking (§8.1)
  if (c.character < 0) {
    if (c.flags & 0x200) {  // alighted without a pedestrian
      c.flags &= ~0x200u;
    } else {
      int qx0, qz0, qx1, qz1;
      doorOf(s, c.start, qx0, qz0);
      doorOf(s, nodeState(s, c.route.nodes[0]).item, qx1, qz1);
      int i = spawnCharacter(s, c, ci, qx0, qz0);
      if (i < 0) {
        endTrip(s, ci, false);
        return;
      }
      walkTo(s, s.characters[size_t(i)], qx0, qz0, qx1, qz1);
      if (Object* ctrl = characterController(s, i)) fireCitizen(s, ci, u"onSpawnCharacter", {Value::heap(VT::Object, ctrl)});
      return;
    }
  }
  Character* ch = c.character >= 0 ? &s.characters[size_t(c.character)] : nullptr;
  if (ch && !ch->done()) return;
  uint32_t here = ch && !ch->path.empty() ? ch->path.back() : 0;
  if (c.flags & 0x200) {  // just alighted
    c.flags &= ~0x200u;
    int qx, qz;
    if (c.route.leg < n) {
      doorOf(s, nodeState(s, c.route.nodes[size_t(c.route.leg)]).item, qx, qz);
    } else {
      fireCitizens(s, ci, u"onCitizenArrive");
      doorOf(s, c.target, qx, qz);
    }
    if (ch) walkTo(s, *ch, int(here & 0xffff), int(here >> 16), qx, qz);
    return;
  }
  if (c.route.leg < n) enterStop(s, ci, c.route.nodes[size_t(c.route.leg)]), c.waitTime = 0;
  else endTrip(s, ci, false);
}

// one processing (FUN_100d3ca0)
void process(SimState& s, int ci) {
  Citizen& c = s.citizens[size_t(ci)];
  if (c.cycleTimer) --c.cycleTimer;
  switch (c.flags & kStateMask) {
    case kIdle:
      if (c.cycleTimer || (c.character >= 0 && !s.characters[size_t(c.character)].done())) return;
      startTrip(s, ci);
      return;
    case kWalk: stepWalk(s, ci); return;
    case kDrive: stepDrive(s, ci); return;
    case kTravel: stepTravel(s, ci); return;
    default: {  // REPLAN (FUN_100cd5c0): when the pedestrian stands, transit again, walk, or give up
      if (c.character >= 0 && !s.characters[size_t(c.character)].done()) return;
      int from = c.replanFrom >= 0 ? c.replanFrom : c.start;
      c.replanFrom = -1;
      c.flags &= ~0x200u;
      float v = walkSpeed(s, c), F = 0;
      float W = c.target >= 0 && from >= 0 ? cellDistance(s, from, c.target) * s.graph.cellSize / v : -1;
      Route route;
      if (c.target >= 0 && from >= 0 && transitRoute(s, from, c.target, v, priceSignificance(s, int(c.flags & 0xf)), route, F) &&
          route.cost < W) {
        c.route = route;
        c.hasRoute = true;
        c.flags = (c.flags & ~kStateMask) | kTravel | 0x200;  // walks on from where it stands
        ++s.replanned;
      } else if (W >= 0 && W * v <= 250.0f) {
        c.flags = (c.flags & ~kStateMask) | kWalk | 0x400;
        ++s.replanned;
      } else {
        endTrip(s, ci, false);
      }
      return;
    }
  }
}

int newCitizen(SimState& s) {
  if (!s.freeCitizens.empty()) {
    int i = s.freeCitizens.back();
    s.freeCitizens.pop_back();
    s.citizens[size_t(i)] = Citizen();
    return i;
  }
  s.citizens.emplace_back();
  return int(s.citizens.size()) - 1;
}

// FUN_100d2ac0
void createCitizen(SimState& s, int cls) {
  int ci = newCitizen(s);
  Citizen& c = s.citizens[size_t(ci)];
  c.alive = true;
  int mx = std::max(1, moduleInt(s, u"maxCycleDuration", 600));
  c.cycleTimer = rnd(s) % mx;
  do c.id = uint32_t((rnd(s) & 0x7fff) << 15 | (rnd(s) & 0x7fff));
  while (s.citizenById.count(c.id));
  c.flags = uint32_t(cls);
  c.carModel = pickCar(s, cls);
  c.charModel = pickCharacter(s, cls, -1);
  c.home = popRandom(s, s.freeHomes[cls]);
  if (rnd(s) % 100 >= moduleInt(s, u"joblessRate", 0)) c.work = findWorkplace(s, c);
  s.citizenById[c.id] = ci;
  ++s.population[cls];
  if (c.work < 0) ++s.unemployed[cls];
  planDay(s, c);
  int codes = 0;
  for (uint32_t q = c.tripQueue; q; q >>= 6) ++codes;
  int k = codes > 1 ? rnd(s) % codes : 0;
  c.tripQueue >>= 6 * k;
  int code = int(c.tripQueue & 0x3f);
  c.start = code == kVisitHome ? c.home : code == kVisitWork ? c.work : findPlace(s, c, code);
  if (c.start >= 0 && code > 5) c.flags |= uint32_t(code) << 12;
  if (c.start < 0) c.tripQueue = 0, c.start = c.home;
}

void deleteCitizen(SimState& s, int ci) {
  Citizen& c = s.citizens[size_t(ci)];
  int cls = int(c.flags & 0xf);
  endTrip(s, ci, false);
  if (c.home >= 0) s.freeHomes[cls].push_back(c.home);
  if (c.work >= 0) s.freeWork[cls].push_back(c.work);
  else --s.unemployed[cls];
  --s.population[cls];
  s.citizenById.erase(c.id);
  if (c.controller && !c.controller->destroyed) s.vm->destroy(c.controller.get());
  c = Citizen();
  s.freeCitizens.push_back(ci);
}

// transfers between line nodes (FUN_10119290): after riding N to its arrival stop, every node departing within the
// larger catchment of the two stops
void linkTransfers(SimState& s) {
  float cs = s.graph.cellSize;
  for (size_t n = 0; n < s.lineNodes.size(); ++n) {
    LineStopState& st = nodeState(s, int(n));
    st.transfers.clear();
    int arr = nextNode(s, int(n));
    if (arr < 0 || st.item < 0) continue;
    const LineStopState& as = nodeState(s, arr);
    if (as.item < 0) continue;
    int ax, az;
    centreCells(s, as.item, ax, az);
    float ca = as.stop ? fieldFloat(*s.vm, as.stop.get(), u"catchmentArea", 100.0f) : 100.0f;
    for (size_t m = 0; m < s.lineNodes.size(); ++m) {
      if (int(m) == arr) continue;
      const LineStopState& ms = nodeState(s, int(m));
      if (ms.item < 0) continue;
      int mx, mz;
      centreCells(s, ms.item, mx, mz);
      float cm = ms.stop ? fieldFloat(*s.vm, ms.stop.get(), u"catchmentArea", 100.0f) : 100.0f, r = std::max(ca, cm);
      float dx = float(mx - ax), dz = float(mz - az);
      if ((dx * dx + dz * dz) * cs * cs <= r * r) st.transfers.push_back(int(m));
    }
  }
}

}  // namespace

// port jam relief (engine/sim_jams.cpp): the riders of a vehicle taken off the map get off where it stood and plan
// again (REPLAN) from the nearer of its last and next stop, as the evacuation of a broken-down or crashed vehicle does
// (stepTravel), without that one's mood 3: the jam was not the vehicle's fault
int evacuateRiders(SimState& s, int vi, const Vec3& at) {
  VehicleState& v = s.vehicles[size_t(vi)];
  if (v.line < 0 || !s.grid || !s.grid->data) return 0;
  const LineState& line = s.lines[size_t(v.line)];
  int item = -1;
  float best = 1e30f;
  for (int k : {v.stopA, v.stopB}) {
    if (k < 0 || k >= int(line.stops.size()) || line.stops[size_t(k)].item < 0) continue;
    Vec3 d = itemCentre(*s.vm, s.grid, line.stops[size_t(k)].item) - at;
    if (d.x * d.x + d.z * d.z < best) best = d.x * d.x + d.z * d.z, item = line.stops[size_t(k)].item;
  }
  const GridData& g = *s.grid->data;
  int qx = int(std::lround((at.x / g.cellSize + float(g.width) * 0.5f) * 4));
  int qz = int(std::lround((at.z / g.cellSize + float(g.length) * 0.5f) * 4));
  int n = 0;
  for (size_t ci = 0; ci < s.citizens.size(); ++ci) {
    Citizen& c = s.citizens[ci];
    if (!c.alive || c.vehicle != vi) continue;
    c.vehicle = -1;
    c.flags |= 0x200;
    int i = spawnCharacter(s, c, int(ci), qx, qz);
    if (i >= 0)
      if (Object* ctrl = characterController(s, i)) fireCitizen(s, int(ci), u"onSpawnCharacter", {Value::heap(VT::Object, ctrl)});
    c.hasRoute = false;
    c.route = Route();
    c.replanFrom = item;
    c.flags = (c.flags & ~kStateMask) | kReplan;
    ++n;
  }
  v.passengerCount = std::max(0, v.passengerCount - n);
  s.evacuated += n;
  return n;
}

// ---------------------------------------------------------------------------------------------------------------
// interface to engine/sim.cpp

void loadCitizens(VM& vm, GridObj* grid, SimState& s, const std::vector<uint8_t>& file, const std::vector<SavedAgentRef>& agents) {
  if (!s.citizensModule) return;
  buildWalkGrid(s);
  buildPools(s);
  linkTransfers(s);
  const GridData& g = *grid->data;
  auto u32 = [&](size_t at) {
    uint32_t v = 0;
    if (at + 4 <= file.size()) std::memcpy(&v, file.data() + at, 4);
    return v;
  };
  auto children = [&](size_t from, size_t end, auto fn) {
    end = std::min(end, file.size());
    for (size_t at = from; at + 8 <= end;) {
      uint32_t tag = u32(at), size = u32(at + 4);
      fn(tag, at + 8, std::min(end, at + 8 + size));
      at += 8 + size;
    }
  };
  // characters (0x0705): pedestrians of citizens; their saved walks restart from where they stand
  std::vector<int> charOf;  // record → character index
  size_t citizensBody = 0, citizensEnd = 0;
  children(8 + 24, std::min(file.size(), size_t(8) + u32(4)), [&](uint32_t tag, size_t body, size_t stop) {
    if (tag == 0x07070000) citizensBody = body, citizensEnd = stop;
    if (tag != 0x07050000) return;
    uint32_t typeCount = u32(body);
    std::vector<int> types;
    children(body + 8, stop, [&](uint32_t t2, size_t b2, size_t s2) {
      if (t2 == 0x07050100) {
        size_t p = b2;
        for (uint32_t i = 0; i < typeCount && p < s2; ++i) {
          std::string name;
          while (p < s2 && file[p]) name += char(file[p++]);
          ++p;
          types.push_back(modelFor(vm, grid, s, name));
        }
        return;
      }
      if (t2 != 0x07050200) return;
      uint16_t type = uint16_t(u32(b2) & 0xffff), anchor = uint16_t(u32(b2) >> 16);
      if (type >= types.size() || types[type] < 0) {
        charOf.push_back(-1);
        return;
      }
      int i = newCharacter(s);
      Character& ch = s.characters[size_t(i)];
      ch.alive = true;
      ch.model = types[type];
      if (anchor) {  // an animal: ground until the flying list says otherwise; birds fly to their saved target
        ch.animal = 2;
        ch.anchor = anchor;
        ch.target[0] = int(u32(b2 + 4)), ch.target[1] = int(u32(b2 + 8)), ch.target[2] = int(u32(b2 + 0x10));
      }
      ch.speed = std::max(0.2f, s.models[size_t(ch.model)].speed);
      float px, py, pz, heading;
      std::memcpy(&px, file.data() + b2 + 0x30, 4);
      std::memcpy(&py, file.data() + b2 + 0x34, 4);
      std::memcpy(&pz, file.data() + b2 + 0x38, 4);
      std::memcpy(&heading, file.data() + b2 + 0x48, 4);
      ch.pos = {px, py, pz};
      ch.yaw = 3.14159265f - heading;  // the file's heading is atan2(vx, −vz) (all moving samples); yaw: (sin, 0, cos)
      {
        int qx = int(u32(b2 + 4)) & 0xffff, qz = int(u32(b2 + 0x10)) & kQzMask;
        float y = py * 256.0f / g.cellSize;
        int lv = stepLayer(s, qx, qz, y, false);  // the layer the saved height is on
        ch.path = {uint32_t(qz | std::max(lv, 0) << kLayerShift) << 16 | uint32_t(qx)};
        // the walk ahead (0x07030100 after the 0x50-byte record, pathfinding.md §5.4): direction codes 0..7, run
        // codes 8..15 repeat the last direction 1..8 more times, from the code at cursor >> 3; each step's layer is
        // the one its height reaches from the previous step (FUN_100e1d40)
        if (!anchor && size_t(b2) + 0x50 + 28 <= file.size() && u32(b2 + 0x50) == 0x07030100) {
          size_t m = b2 + 0x58;
          int steps = int(u32(m)), nibbles = int(u32(m + 4));
          uint32_t cursor = u32(m + 12);
          static const int dxs[8] = {1, 0, -1, 0, 1, 1, -1, -1}, dzs[8] = {0, 1, 0, -1, 1, -1, 1, -1};
          int dir = -1, x = qx, z = qz, l = std::max(lv, 0);
          auto step = [&](int d) {
            x += dxs[d], z += dzs[d];
            int nl = stepLayer(s, x, z, y, l == 0);
            l = std::max(nl, 0);
            y = layerHeight(s, x, z | l << kLayerShift);
            ch.path.push_back(uint32_t(z | l << kLayerShift) << 16 | uint32_t(x));
          };
          for (int k = int(cursor >> 3); k < nibbles && int(ch.path.size()) <= steps && m + 20 + size_t(k / 8) * 4 + 4 <= file.size(); ++k) {
            int code = int((u32(m + 20 + size_t(k / 8) * 4) >> (4 * (k & 7))) & 0xf);
            if (code < 8) {
              dir = code;
              step(dir);
            } else if (dir >= 0) {
              for (int r = 0; r < code - 7; ++r) step(dir);
            }
          }
          if (ch.path.size() > 1) ch.goal = qWorld(s, int(ch.path.back() & 0xffff), int(ch.path.back() >> 16));
        }
      }
      ch.next = ch.path.size() > 1 ? 1 : 0;
      charOf.push_back(i);
    });
    children(body + 8, stop, [&](uint32_t t2, size_t b2, size_t) {  // the flying animals (0x07050300)
      if (t2 != 0x07050300) return;
      for (uint32_t k = 0, n = u32(b2); k < n; ++k) {
        uint32_t ref = u32(b2 + 4 + size_t(k) * 4);
        if (ref >= 1 && ref <= charOf.size() && charOf[ref - 1] >= 0) s.characters[size_t(charOf[ref - 1])].animal = 1;
      }
    });
  });
  s.loadedCharacter = charOf;  // for sim_save.cpp's pass-through of the records not loaded
  if (debugSim()) {
    size_t withWalk = 0, steps = 0;
    for (int i : charOf)
      if (i >= 0 && s.characters[size_t(i)].path.size() > 1) ++withWalk, steps += s.characters[size_t(i)].path.size() - 1;
    LOG_INFO("citizens: %zu loaded pedestrians continue a saved walk (%zu steps)", withWalk, steps);
  }
  for (size_t i = 0; i < g.items.size(); ++i)  // spawn points still on the map (new games) become animals
    if (!g.items[i].removed && !(g.items[i].flags & 0x700000) && itemInfo(s, int(i)).type == 0x16) s.spawnPoints.push_back(uint32_t(i));
  if (!citizensBody) return;
  // citizens (0x0707): records take their home and workplace out of the pools (load-time reconciliation)
  for (int k = 0; k < 7; ++k) {
    s.walkCount[k] = int(u32(citizensBody + 4 + size_t(k) * 4));
    s.driveCount[k] = int(u32(citizensBody + 0x20 + size_t(k) * 4));
    s.travelCount[k] = int(u32(citizensBody + 0x3c + size_t(k) * 4));
    s.canTravelCount[k] = int(u32(citizensBody + 0x58 + size_t(k) * 4));
    s.priceTooHighCount[k] = int(u32(citizensBody + 0x74 + size_t(k) * 4));
  }
  auto take = [](std::vector<int>& pool, int item) {
    auto it = std::find(pool.begin(), pool.end(), item);
    if (it == pool.end()) return false;
    *it = pool.back();
    pool.pop_back();
    return true;
  };
  int loaded = 0, droppedHomes = 0, waiting = 0, riding = 0;
  size_t transportVehicles = s.vehicles.size();
  children(citizensBody + 0x90, citizensEnd, [&](uint32_t tag, size_t b, size_t stop) {
    if (tag != 0x07070100) return;
    int ci = newCitizen(s);
    Citizen& c = s.citizens[size_t(ci)];
    c.alive = true;
    c.flags = u32(b);
    c.id = u32(b + 4);
    c.tripQueue = u32(b + 8);
    c.usedTransport = u32(b + 0xc);
    c.cycleTimer = int(u32(b + 0x10));
    c.waitTime = int(u32(b + 0x14));
    int cls = int(c.flags & 0xf) % 7;
    auto item = [&](uint32_t ref) { return ref >= 1 && ref <= g.items.size() ? int(ref) - 1 : -1; };
    c.home = item(u32(b + 0x28));
    c.work = item(u32(b + 0x2c));
    if (c.home >= 0 && !take(s.freeHomes[cls], c.home)) c.home = -1, ++droppedHomes;
    if (c.work >= 0 && !take(s.freeWork[cls], c.work)) c.work = -1;
    c.start = item(u32(b + 0x30));
    c.target = item(u32(b + 0x34));
    c.charModel = int(u32(b + 0x38));
    c.carModel = int32_t(u32(b + 0x3c)) < 0 ? -1 : int(u32(b + 0x3c));
    uint32_t chr = u32(b + 0x18), car = u32(b + 0x1c), node = u32(b + 0x20), veh = u32(b + 0x24);
    if (chr >= 1 && chr <= charOf.size() && charOf[chr - 1] >= 0) {
      c.character = charOf[chr - 1];
      s.characters[size_t(c.character)].citizen = ci;
    }
    if (car >= 1 && car <= agents.size() && agents[car - 1].mover >= 0) {
      c.car = agents[car - 1].mover;
      Mover& m = s.movers[size_t(c.car)];
      if (m.parked) {
        m.citizen = ci;  // the citizen's parked car (flag 0x100000), taken on the next drive
        c.flags |= 0x100000;
      } else {
        m.citizen = ci;
        c.flags |= 0x100;  // driving: the pedestrian left when the car spawned
        if (c.character >= 0) removeCharacter(s, c.character), c.character = -1;
      }
      for (Unit& u : m.units)  // the citizen's paint, which the saved 16-bit seed cannot give back
        if (uint32_t t = paint(s, u.model, c.id)) u.tint = t;
    }
    if (veh >= 1 && veh <= transportVehicles) c.vehicle = int(veh) - 1, ++riding;
    // the route child (0x07061000)
    children(b + 0x40, stop, [&](uint32_t t2, size_t b2, size_t s2) {
      if (t2 != 0x07061000 || c.hasRoute) return;
      Route r;
      std::memcpy(&r.cost, file.data() + b2, 4);
      uint32_t n = u32(b2 + 4);
      r.leg = int(u32(b2 + 8));
      bool ok = true;
      for (uint32_t k = 0; k < n && b2 + 12 + size_t(k) * 4 + 4 <= s2; ++k) {
        uint32_t ref = u32(b2 + 12 + size_t(k) * 4);
        if (ref < 1 || ref > s.lineNodes.size()) ok = false;
        else r.nodes.push_back(int(ref) - 1);
      }
      if (ok && !r.nodes.empty()) c.route = r, c.hasRoute = true;
    });
    if (node >= 1 && node <= s.lineNodes.size()) {
      c.flags &= 0xf0ffffffu;  // mood rebuilt from the wait (0x100d31a0.c:329-369)
      enterStop(s, ci, int(node) - 1);
      int p = int(c.id & 0xff) + 256;
      int mood = c.waitTime > p ? 3 : c.waitTime >= (3 * p) / 4 ? 2 : c.waitTime >= p / 2 ? 1 : 0;
      if (mood) setMood(s, ci, mood);
      ++waiting;
    }
    if ((c.flags & kStateMask) == kTravel && !c.hasRoute) c.flags &= ~kStateMask;
    if ((c.flags & kStateMask) == kDrive && c.car < 0 && !(c.flags & 0x200)) {
      c.driveStart = c.start >= 0 ? itemNode(s, c.start, 0x10100) : -1;  // drive nodes rebuilt (0x100d31a0.c:258-291)
      c.driveEnd = c.target >= 0 ? itemNode(s, c.target, 0x20100) : -1;
      c.flags &= ~0x100u;
    }
    s.citizenById[c.id] = ci;
    ++s.population[cls];
    if (c.work < 0) ++s.unemployed[cls];
    ++loaded;
  });
  // below 1x traffic a share of the cars driving at load ends its trip now, so the density shows at once
  if (float td = trafficDensity(s); td < 1.0f) {
    int thinned = 0;
    for (size_t ci = 0; ci < s.citizens.size(); ++ci) {
      Citizen& c = s.citizens[ci];
      if (!c.alive || (c.flags & kStateMask) != kDrive || c.car < 0 || s.movers[size_t(c.car)].parked) continue;
      if (rndFloat(s, 0, 1) >= td) endTrip(s, int(ci), false), ++thinned;
    }
    for (size_t id = 0; id < s.movers.size(); ++id) {
      const Mover& m = s.movers[id];
      if (m.live && !m.parked && !m.transit && !m.train && !m.boat && !m.aircraft && m.citizen < 0 && rndFloat(s, 0, 1) >= td)
        despawn(vm, s, int(id)), ++thinned;
    }
    LOG_INFO("citizens: %d cars taken off for the traffic density %.2f", thinned, td);
  }
  // pedestrians without a citizen (left over from removed trips) go away
  for (size_t i = 0; i < s.characters.size(); ++i)
    if (s.characters[i].alive && s.characters[i].citizen < 0 && !s.characters[i].animal) removeCharacter(s, int(i));
  int homes = 0, free = 0;
  for (int k = 0; k < 7; ++k) homes += s.population[k] + int(s.freeHomes[k].size()), free += int(s.freeHomes[k].size());
  // a map without saved citizens (a new game) fills its free home slots on the first tick (§2.2)
  s.citizensLoaded = loaded > 0 || homes > 0;
  LOG_INFO("citizens: %d loaded (%d waiting at stops, %d riding), %d home slots (%d free, %d saved homes not found), %zu pedestrians",
           loaded, waiting, riding, homes, free, droppedHomes, s.characters.size() - s.freeCharacters.size());
}

void stepCitizens(VM& vm, SimState& s, float dt) {
  if (!s.citizensLoaded || !s.citizensModule || !s.citizensModule->enabled()) return;
  // every free home slot gets a citizen (§2.2)
  for (int cls = 0; cls < 7; ++cls) {
    refreshPools(s);
    if (s.charPools[cls].empty()) continue;
    while (!s.freeHomes[cls].empty()) createCitizen(s, cls);
  }
  // round robin, about once per grid-second each (§0)
  int count = 0;
  for (const Citizen& c : s.citizens) count += c.alive;
  float x = float(count) * dt + s.citizenAcc;
  int n = std::min(count, int(x));
  s.citizenAcc = x - float(n);
  for (int k = 0; k < n && !s.citizens.empty(); ++k) {
    if (s.citizenCursor >= s.citizens.size()) s.citizenCursor = 0;
    size_t ci = s.citizenCursor++;
    Citizen& c = s.citizens[ci];
    if (!c.alive) {
      --k;
      if (count == 0) break;
      continue;
    }
    if (c.home < 0) {
      int cls = int(c.flags & 0xf);
      c.home = popRandom(s, s.freeHomes[cls]);
      if (c.home < 0) {
        deleteCitizen(s, int(ci));
        continue;
      }
    }
    process(s, int(ci));
  }
  stepAnimals(vm, s, dt);
  for (Character& ch : s.characters)
    if (ch.alive && ch.animal != 1) stepCharacter(s, ch, dt);
}

void carArrived(VM& vm, SimState& s, int mover, bool stuck) {
  Mover& m = s.movers[size_t(mover)];
  int ci = m.citizen;
  Vec3 at = m.track.nodes.empty() ? Vec3{} : s.graph.nodes[size_t(m.track.nodes.back())].p1;
  std::vector<Unit> units = m.units;
  m.citizen = -1;
  despawn(vm, s, mover);
  if (ci < 0 || !s.citizens[size_t(ci)].alive) return;
  Citizen& c = s.citizens[size_t(ci)];
  c.car = -1;
  c.flags &= ~0x100u;
  if (stuck || c.target < 0) {
    fireCitizens(s, ci, u"onCitizenArrive");
    endTrip(s, ci, false);
    return;
  }
  // park (§7 step 3): a one-car vehicle of a normal trip takes a random free PARK node of the target, else one within
  // 6x6 cells around where it stopped; it holds that node until the next trip
  ItemInfo ti = itemInfo(s, c.target);
  bool roadTarget = ti.type >= 1 && ti.type <= 7;
  if (!roadTarget && !(c.flags & 0xff000) && units.size() == 1) {
    std::vector<int> spots;
    for (int n : s.graph.itemNodes[size_t(c.target)])
      if ((s.graph.nodes[size_t(n)].flags & kPathPark) && s.rt[size_t(n)].occupant < 0) spots.push_back(n);
    if (spots.empty()) {
      const GridData& gg = *s.grid->data;
      int cx = int(at.x / gg.cellSize + float(gg.width) * 0.5f), cz = int(at.z / gg.cellSize + float(gg.length) * 0.5f);
      std::vector<int> near;
      s.graph.query(cx - 3, cz - 3, 6, 6, kPathPark, near);
      for (int n : near)
        if (s.rt[size_t(n)].occupant < 0) spots.push_back(n);
    }
    if (!spots.empty()) {
      int spot = spots[size_t(rnd(s)) % spots.size()];
      int id = parkCar(vm, s, units, spot);
      if (id >= 0) {
        s.movers[size_t(id)].citizen = ci;
        c.car = id;
        c.flags |= 0x100000;
        at = s.graph.nodes[size_t(spot)].p0;
        ++s.carsParked;
      }
    }
  }
  // the last metres on foot (§7 step 3)
  const GridData& g = *s.grid->data;
  int qx0 = int(std::lround((at.x / g.cellSize + float(g.width) * 0.5f) * 4)), qz0 = int(std::lround((at.z / g.cellSize + float(g.length) * 0.5f) * 4));
  int qx1, qz1;
  doorOf(s, c.target, qx1, qz1);
  int i = spawnCharacter(s, c, ci, qx0, qz0);
  if (i >= 0) walkTo(s, s.characters[size_t(i)], qx0, qz0, qx1, qz1);
  c.flags |= 0x200;
}

void collectCharacters(Display& d, VM& vm, SimState& s, const Vec3& eye, const Mat4& viewProj, float farZ,
                       std::vector<render::DrawItem>& items) {
  s.eye = eye;
  s.eyeKnown = true;
  static int drawCalls = 0;
  ++drawCalls;
  float reach = std::min(farZ, 350.0f);
  // gsGridCharacter::updateVisibility (L:0x08153ae0, docs/renderer/underground.md §5): in the underground view only
  // characters more than half a cell below the terrain of their cell show (y + 0x80 < cell height, 1/256 cells)
  const bool underground = s.grid && s.grid->underground() && s.grid->data;
  for (const Character& ch : s.characters) {
    if (!ch.alive || ch.model < 0 || ch.virtualLeft > 0) continue;
    Vec3 e = ch.pos - eye;
    if (dot(e, e) > reach * reach) continue;
    if (underground) {
      const GridData& g = *s.grid->data;
      float cs = g.cellSize;
      int cx = int(std::floor(ch.pos.x / cs + float(g.width) * 0.5f)), cz = int(std::floor(ch.pos.z / cs + float(g.length) * 0.5f));
      if (cx < 0 || cz < 0 || cx >= g.width || cz >= g.length) continue;
      if (!(ch.pos.y * 256.0f / cs + 128.0f < float(g.height[size_t(cz) * size_t(g.width) + size_t(cx)]))) continue;
    }
    float clip[4];
    for (int r = 0; r < 4; ++r) clip[r] = viewProj.at(r, 0) * ch.pos.x + viewProj.at(r, 1) * ch.pos.y + viewProj.at(r, 2) * ch.pos.z + viewProj.at(r, 3);
    float margin = std::fabs(clip[3]) * 0.1f + 5.0f;
    if (clip[3] < -5 || std::fabs(clip[0]) > clip[3] + margin || std::fabs(clip[1]) > clip[3] + margin) continue;
    // debugging (REGS_STRIDE_SHOTS=<prefix>): follow the first walker close to the camera with a screenshot every
    // third frame, logging where it is on screen, to check the walk cycle
    if (static const char* stride = std::getenv("REGS_STRIDE_SHOTS"); stride && *stride && !ch.done() && ch.virtualLeft < 0 && clip[3] > 0) {
      static const Character* followed = nullptr;
      static int frame = 0, shots = 0;
      float sx = (clip[0] / clip[3] * 0.5f + 0.5f) * 1600, sy = (0.5f - clip[1] / clip[3] * 0.5f) * 900;
      if (!followed && drawCalls > 200 && sx > 200 && sx < 1400 && sy > 100 && sy < 750) followed = &ch;
      if (followed == &ch && shots < 16 && frame++ % 2 == 0) {
        std::string file = std::string(stride) + "-" + std::to_string(shots++) + ".png";
        d.requestScreenshot(file);
        LOG_INFO("sim: stride shot %s: walker at (%.0f, %.0f), speed %.2f", file.c_str(), sx, sy, ch.speed);
      }
    }
    // the one place that picks a character's mesh and pose: skinned and animated within 300 units (beyond, the
    // original draws the bind pose), a plain mesh for models without a skeleton
    ObjectObj* o = s.models[size_t(ch.model)].object;
    if (!o || !o->data) continue;
    Mat4 world = poseMatrix({ch.pos, {std::sin(ch.yaw), 0, std::cos(ch.yaw)}});
    if (isSkinnedModel(vm, o)) {
      regs::engine::Pose pose = dot(e, e) < 300.0f * 300.0f ? poseCharacter(vm, o, ch.anim) : nullptr;
      collectSkinned(d, vm, o, world, pose, items);
      continue;
    }
    if (!o->gpu) {
      o->gpu = std::make_shared<render::GpuMesh>(d.r3d().upload(o->data->mesh));
      o->materials = std::make_shared<std::vector<render::GpuMaterial>>();
      for (const auto& mat : o->data->materials) o->materials->push_back(objectMaterial(d, vm, o, mat));
    }
    render::DrawItem it;
    it.mesh = o->gpu.get();
    it.materials = o->materials.get();
    it.world = world;
    items.push_back(it);
  }
}

void syncCharacterFields(SimState& s) {
  for (const Character& ch : s.characters)
    if (ch.alive && ch.controller) setPoseFields(ch.controller.get(), {ch.pos, {std::sin(ch.yaw), 0, std::cos(ch.yaw)}});
}

// ---------------------------------------------------------------------------------------------------------------
// natives

// ---- live map edits (simItemsChanged) ----

void restampWalk(SimState& s, int x0, int z0, int x1, int z1) {
  const GridData& g = *s.grid->data;
  if (s.walk.words.size() != size_t(g.width) * size_t(g.length)) return;
  x0 = std::max(0, x0), z0 = std::max(0, z0), x1 = std::min(g.width - 1, x1), z1 = std::min(g.length - 1, z1);
  if (x0 > x1 || z0 > z1) return;
  resetWalkCells(s, x0, z0, x1, z1);
  // the items over the rectangle stamp it again in their order; those within 2 cells find their doors again
  for (size_t i = 0; i < g.items.size(); ++i) {
    const auto& it = g.items[i];
    if (it.removed) {
      s.walk.doors.erase(int(i));
      continue;
    }
    if (it.x > x1 + 2 || it.z > z1 + 2 || it.x + 64 < x0 - 2 || it.z + 64 < z0 - 2) continue;
    ItemInfo info = itemInfo(s, int(i));
    if (it.x + info.sx - 1 < x0 - 2 || it.z + info.sz - 1 < z0 - 2) continue;
    bool deck = false;
    stampWalkItem(s, i, x0, z0, x1, z1, deck, nullptr);
  }
  // pedestrians whose way crosses the rectangle look for a new one from where they are
  int repathed = 0;
  for (Character& ch : s.characters) {
    if (!ch.alive || ch.virtualLeft >= 0 || ch.done()) continue;
    bool through = false;
    for (size_t k = ch.next; k < ch.path.size() && !through; ++k) {
      int cx = int(ch.path[k] & 0xffff) >> 2, cz = int((ch.path[k] >> 16) & kQzMask) >> 2;
      through = cx >= x0 && cx <= x1 && cz >= z0 && cz <= z1;
    }
    if (!through) continue;
    uint32_t from = ch.path[ch.next > 0 ? ch.next - 1 : 0], to = ch.path.back();
    walkTo(s, ch, int(from & 0xffff), int(from >> 16), int(to & 0xffff), int(to >> 16));
    ++repathed;
  }
  if (debugSim()) LOG_INFO("citizens: walk layers of cells (%d, %d)-(%d, %d) stamped again, %d pedestrians on a new path", x0, z0, x1, z1, repathed);
}

void citizensItemsChanged(SimState& s, const std::vector<uint32_t>& added, const std::vector<uint32_t>& removed) {
  const GridData& g = *s.grid->data;
  // FUN_100d2fb0: the pools lose every slot of the item; trips from or to it end, its workers lose the job, its
  // residents are deleted (their slots went with the item, so nobody moves in again)
  std::vector<uint8_t> gone(g.items.size(), 0);
  bool any = false;
  for (uint32_t i : removed)
    if (i < gone.size()) gone[i] = 1, any = true;
  if (any) {
    auto drop = [&](std::vector<int>& v) { v.erase(std::remove_if(v.begin(), v.end(), [&](int it) { return it >= 0 && gone[size_t(it)]; }), v.end()); };
    for (auto& p : s.freeHomes) drop(p);
    for (auto& p : s.freeWork) drop(p);
    for (auto& p : s.visitPlaces) drop(p);
    auto isGone = [&](int it) { return it >= 0 && size_t(it) < gone.size() && gone[size_t(it)]; };
    int ended = 0, jobless = 0, deleted = 0;
    for (size_t ci = 0; ci < s.citizens.size(); ++ci) {
      Citizen& c = s.citizens[ci];
      if (!c.alive) continue;
      if (isGone(c.start) || isGone(c.target)) {
        endTrip(s, int(ci), false);
        c.start = c.target = -1;
        c.tripQueue = 0;
        ++ended;
      }
      if (isGone(c.work)) c.work = -1, ++s.unemployed[c.flags & 0xf], ++jobless;
      if (isGone(c.home)) {
        c.home = -1;
        deleteCitizen(s, int(ci));
        ++deleted;
      }
    }
    LOG_INFO("citizens: %zu items removed: %d trips ended, %d jobs lost, %d residents gone", removed.size(), ended, jobless, deleted);
  }
  for (uint32_t i : added)  // addObject: a new spawn point is queued (characters.spawnPoints.insert)
    if (i < g.items.size() && !(g.items[i].flags & 0x700000) && itemInfo(s, int(i)).type == 0x16) s.spawnPoints.push_back(i);
  // registerBuilding (FUN_100d2d90): the new slots; the next citizens tick creates the residents
  float hm = moduleFloat(s, u"homeMultiplier", 1.0f) * populationDensity(s), wm = moduleFloat(s, u"workplaceMultiplier", 1.0f);
  int homes = 0;
  for (int k = 0; k < 7; ++k) homes -= int(s.freeHomes[k].size());
  for (uint32_t i : added)
    if (i < g.items.size()) addPlaces(s, i, hm, wm);
  for (int k = 0; k < 7; ++k) homes += int(s.freeHomes[k].size());
  if (homes) LOG_INFO("citizens: %d new home slots", homes);
  if (homes > 0) s.citizensLoaded = true;
}

void citizensLeaveLineNodes(SimState& s, const std::vector<uint8_t>& nodeGone, const std::vector<uint8_t>& lineChanged) {
  auto isGone = [&](int n) { return n >= 0 && size_t(n) < nodeGone.size() && nodeGone[size_t(n)]; };
  for (size_t ci = 0; ci < s.citizens.size(); ++ci) {
    Citizen& c = s.citizens[ci];
    if (!c.alive) continue;
    bool hit = isGone(c.lineNode);
    for (int n : c.route.nodes) hit = hit || isGone(n);
    if (c.vehicle >= 0) hit = hit || lineChanged[size_t(s.vehicles[size_t(c.vehicle)].line)];
    if (hit) endTrip(s, int(ci), false);
  }
}

void citizensRemapLineNodes(SimState& s, const std::vector<int>& newNode) {
  for (Citizen& c : s.citizens) {
    if (!c.alive) continue;
    if (c.lineNode >= 0) c.lineNode = newNode[size_t(c.lineNode)];
    for (int& n : c.route.nodes) n = newNode[size_t(n)];
  }
  linkTransfers(s);
}

void registerCitizenNatives(VM& vm) {
  for (const char* cls : {"gsGridCitizenController", "gsGridCharacterController"})
    if (ClassInfo* c = vm.findClass(cls)) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new TransportObj(ci)); };
  auto stateFor = [](Object* o) -> SimState* {
    for (Object* p = o; p; p = p->parent)
      if (auto* g = dynamic_cast<GridObj*>(p)) return g->sim.get();
    return nullptr;
  };
  auto citizenOf = [](Object* o, SimState** out) -> Citizen* {
    auto* t = dynamic_cast<TransportObj*>(o);
    auto s = t ? t->sim.lock() : nullptr;
    if (!s || t->index < 0 || size_t(t->index) >= s->citizens.size() || !s->citizens[size_t(t->index)].alive) return nullptr;
    if (out) *out = s.get();
    return &s->citizens[size_t(t->index)];
  };

  // ---- gsGridCitizens configuration ----
  auto bump = [](VM& v, Object* m) { m->fieldValues[u"<poolsVersion>"] = Value::integer(fieldInt(v, m, u"<poolsVersion>", 0) + 1); };
  auto append = [](Object* m, const char16_t* key, Value entry) {
    Value& slot = m->fieldValues[key];
    if (!slot.array()) slot = Value::heap(VT::Array, make<ArrayObj>().get());
    slot.array()->items.push_back(entry);
  };
  // addCharacter(class, eraMask, gender, name) or the old (class, name)
  implement(vm, "gsGridCitizens", u"addCharacter", [bump, append](VM& v, Object* s, int argc, const Value* a) {
    auto e = make<ArrayObj>();
    if (argc >= 4) e->items = {v.resolve(a[0]), v.resolve(a[1]), v.resolve(a[2]), v.resolve(a[3])};
    else if (argc >= 2) e->items = {v.resolve(a[0]), Value::integer(-1), Value::integer(-1), v.resolve(a[1])};
    else return Value();
    append(s, u"<charPools>", Value::heap(VT::Array, e.get()));
    bump(v, s);
    return Value();
  });
  // addCar(visitType, class, eraMask, weight, parts…)
  implement(vm, "gsGridCitizens", u"addCar", [bump, append](VM& v, Object* s, int argc, const Value* a) {
    auto e = make<ArrayObj>();
    for (int i = 0; i < argc; ++i) e->items.push_back(v.resolve(a[i]));
    append(s, u"<carPools>", Value::heap(VT::Array, e.get()));
    bump(v, s);
    return Value();
  });
  for (const char16_t* m : {u"resetCharacters", u"resetCars"}) {
    std::u16string key = m[5] == u'C' && m[6] == u'h' ? u"<charPools>" : u"<carPools>";
    implement(vm, "gsGridCitizens", m, [bump, key](VM& v, Object* s, int, const Value*) {
      s->fieldValues.erase(key);
      bump(v, s);
      return Value();
    });
  }
  auto setTable = [](VM& v, Object* m, const char16_t* key, size_t i, const Value& val) {
    Value& slot = m->fieldValues[key];
    if (!slot.array()) slot = Value::heap(VT::Array, make<ArrayObj>().get());
    auto& items = slot.array()->items;
    if (items.size() <= i) items.resize(i + 1);
    items[i] = v.resolve(val);
  };
  implement(vm, "gsGridCitizens", u"setReasonProbability", [setTable](VM& v, Object* s, int argc, const Value* a) {
    int cls = v.toInt(a[0]);
    if (cls >= 0 && cls < 7)
      for (int k = 0; k < 4 && k + 1 < argc; ++k) setTable(v, s, u"<reason>", size_t(cls * 4 + k), Value::integer(v.toInt(a[k + 1])));
    return Value();
  });
  implement(vm, "gsGridCitizens", u"setPriceSignificance", [setTable](VM& v, Object* s, int, const Value* a) {
    int cls = v.toInt(a[0]);
    if (cls >= 0 && cls < 7) setTable(v, s, u"<priceSig>", size_t(cls), Value::real(v.toFloat(a[1])));
    return Value();
  });
  implement(vm, "gsGridCitizens", u"setUtilProbability", [setTable](VM& v, Object* s, int, const Value* a) {
    int t = v.toInt(a[0]);
    if (t >= 0 && t < 13) setTable(v, s, u"<utilProb>", size_t(t), Value::real(v.toFloat(a[1])));
    return Value();
  });

  // ---- the simulation's view of addWorkPlaces / addVisitPlaces (the place counts stay with engine/minimap.cpp) ----
  if (ClassInfo* go = vm.findClass("gsGridObject")) {
    auto wrap = [&vm, go](const char16_t* name, std::function<void(VM&, Object*, int, const Value*)> extra) {
      auto it = go->methods.find(name);
      NativeFn base = it == go->methods.end() ? NativeFn() : it->second.fn;
      implement(vm, "gsGridObject", name, [base, extra](VM& v, Object* s, int argc, const Value* a) {
        extra(v, s, argc, a);
        return base ? base(v, s, argc, a) : Value();
      });
    };
    wrap(u"addWorkPlaces", [](VM& v, Object* s, int argc, const Value* a) {
      if (argc >= 3) s->fieldValues[u"<workVisit" + utf8ToUtf16(std::to_string(v.toInt(a[0]))) + u">"] = Value::integer(v.toInt(a[2]));
    });
    wrap(u"addVisitPlaces", [](VM& v, Object* s, int argc, const Value* a) {
      if (argc >= 4) s->fieldValues[u"<openDays>"] = Value::integer(int32_t(uint32_t(v.toInt(a[2])) << 16 | (uint32_t(v.toInt(a[3])) & 0xffff)));
    });
  }

  // ---- gsGridCitizens statistics ----
  if (ClassInfo* c = vm.findClass("gsGridCitizens")) {
    FieldInfo& f = c->fields[u"count"];
    f.name = u"count";
    f.readOnly = true;
    f.get = [stateFor](VM&, Object* o) {
      SimState* s = stateFor(o);
      int n = 0;
      if (s)
        for (int k = 0; k < 7; ++k) n += s->population[k];
      return Value::integer(n);
    };
  }
  auto perClass = [stateFor](VM& v, Object* o, int argc, const Value* a, auto get) {
    SimState* s = stateFor(o);
    if (!s) return Value::integer(0);
    if (argc == 0) {
      int n = 0;
      for (int k = 0; k < 7; ++k) n += get(*s, k);
      return Value::integer(n);
    }
    int n = 0;
    for (int i = 0; i < argc; ++i) {
      int k = v.toInt(a[i]);
      if (k >= 0 && k < 7) n += get(*s, k);
    }
    return Value::integer(n);
  };
  implement(vm, "gsGridCitizens", u"getHomeCount", [perClass](VM& v, Object* o, int argc, const Value* a) {
    return perClass(v, o, argc, a, [](SimState& s, int k) { return s.population[k] + int(s.freeHomes[k].size()); });
  });
  implement(vm, "gsGridCitizens", u"getWorkplaceCount", [perClass](VM& v, Object* o, int argc, const Value* a) {
    return perClass(v, o, argc, a, [](SimState& s, int k) { return s.population[k] - s.unemployed[k] + int(s.freeWork[k].size()); });
  });
  // 0x100cdb30: the unemployed share of the population of the given classes (0..1), 0 without citizens
  implement(vm, "gsGridCitizens", u"getUnemployment", [stateFor](VM& v, Object* o, int argc, const Value* a) {
    SimState* s = stateFor(o);
    int pop = 0, jobless = 0;
    for (int i = 0; s && i < argc; ++i) {
      int k = v.toInt(a[i]);
      if (k >= 0 && k < 7) pop += s->population[k], jobless += s->unemployed[k];
    }
    return Value::real(pop ? float(jobless) / float(pop) : 0.0f);
  });
  for (auto [name, member] : {std::pair{u"getWalkCount", &SimState::walkCount}, std::pair{u"getDriveCount", &SimState::driveCount},
                              std::pair{u"getTravelCount", &SimState::travelCount}, std::pair{u"getCanTravelCount", &SimState::canTravelCount},
                              std::pair{u"getPriceTooHighCount", &SimState::priceTooHighCount}}) {
    // class −1 = the sum over all seven classes (L:0x081589e0): the monthly coverage, usage and car-usage
    // statistics ask for −1 (player.script)
    implement(vm, "gsGridCitizens", name, [stateFor, member](VM& v, Object* o, int, const Value* a) {
      SimState* s = stateFor(o);
      int k = v.toInt(a[0]);
      if (s && k == -1) {
        int n = 0;
        for (int i = 0; i < 7; ++i) n += (s->*member)[i];
        return Value::integer(n);
      }
      return Value::integer(s && k >= 0 && k < 7 ? (s->*member)[k] : 0);
    });
  }
  implement(vm, "gsGridCitizens", u"resetTripCounts", [stateFor](VM&, Object* o, int, const Value*) {
    if (SimState* s = stateFor(o))
      for (int k = 0; k < 7; ++k) s->walkCount[k] = s->driveCount[k] = s->travelCount[k] = s->canTravelCount[k] = s->priceTooHighCount[k] = 0;
    return Value();
  });
  implement(vm, "gsGridCitizens", u"findCitizen", [stateFor](VM& v, Object* o, int, const Value* a) {
    SimState* s = stateFor(o);
    if (!s) return Value::null();
    auto it = s->citizenById.find(uint32_t(v.toInt(a[0])));
    Object* c = it == s->citizenById.end() ? nullptr : citizenController(*s, it->second);
    return c ? Value::heap(VT::Object, c) : Value::null();
  });
  // getRandomCitizen: the first id at or after a random 30-bit number, wrapping (0x100ce110)
  implement(vm, "gsGridCitizens", u"getRandomCitizen", [stateFor](VM&, Object* o, int, const Value*) {
    SimState* s = stateFor(o);
    if (!s || s->citizenById.empty()) return Value::null();
    uint32_t r = uint32_t((rnd(*s) & 0x7fff) << 15 | (rnd(*s) & 0x7fff));
    auto it = s->citizenById.lower_bound(r);
    if (it == s->citizenById.end()) it = s->citizenById.begin();
    Object* c = citizenController(*s, it->second);
    return c ? Value::heap(VT::Object, c) : Value::null();
  });

  // ---- gsGridCitizenController ----
  auto citizenMethod = [&vm, citizenOf](const char16_t* name, std::function<Value(VM&, SimState&, Citizen&)> fn) {
    implement(vm, "gsGridCitizenController", name, [citizenOf, fn](VM& v, Object* o, int, const Value*) {
      SimState* s = nullptr;
      Citizen* c = citizenOf(o, &s);
      return c ? fn(v, *s, *c) : Value::null();
    });
  };
  citizenMethod(u"getID", [](VM&, SimState&, Citizen& c) { return Value::integer(int32_t(c.id)); });
  citizenMethod(u"getClass", [](VM&, SimState&, Citizen& c) { return Value::integer(int(c.flags & 0xf)); });
  citizenMethod(u"getState", [](VM&, SimState&, Citizen& c) { return Value::integer(int(c.flags & kStateMask)); });
  citizenMethod(u"getMood", [](VM&, SimState&, Citizen& c) { return Value::integer(moodOf(c)); });
  auto itemValue = [](VM& v, SimState& s, int item) {
    Object* o = item >= 0 ? gridItemController(v, s.grid, uint32_t(item)) : nullptr;
    return o ? Value::heap(VT::Object, o) : Value::null();
  };
  citizenMethod(u"getHome", [itemValue](VM& v, SimState& s, Citizen& c) { return itemValue(v, s, c.home); });
  citizenMethod(u"getWorkplace", [itemValue](VM& v, SimState& s, Citizen& c) { return itemValue(v, s, c.work); });
  citizenMethod(u"getStart", [itemValue](VM& v, SimState& s, Citizen& c) { return itemValue(v, s, c.start); });
  citizenMethod(u"getTarget", [itemValue](VM& v, SimState& s, Citizen& c) { return itemValue(v, s, c.target); });
  citizenMethod(u"getCharModel", [](VM&, SimState& s, Citizen& c) {
    const CharacterModel* cm = charModelOf(s, c);
    return cm ? Value::heap(VT::Object, s.models[size_t(cm->model)].object) : Value::null();
  });
  citizenMethod(u"getCarModel", [](VM&, SimState& s, Citizen& c) {
    const CarModel* car = carModelOf(s, c);
    return car ? Value::heap(VT::Object, s.models[size_t(car->parts[0])].object) : Value::null();
  });
  citizenMethod(u"getCharacter", [](VM&, SimState& s, Citizen& c) {
    Object* o = c.character >= 0 ? characterController(s, c.character) : nullptr;
    return o ? Value::heap(VT::Object, o) : Value::null();
  });
  citizenMethod(u"getVehicle", [](VM& v, SimState& s, Citizen& c) {
    if (c.vehicle >= 0) return Value::heap(VT::Object, s.vehicles[size_t(c.vehicle)].object.get());
    if (c.car >= 0 && s.movers[size_t(c.car)].live && s.movers[size_t(c.car)].controller)
      return Value::heap(VT::Object, s.movers[size_t(c.car)].controller.get());
    (void)v;
    return Value::null();
  });
  // usedTransport([type]): the mask, or whether that line type was used
  implement(vm, "gsGridCitizenController", u"usedTransport", [citizenOf](VM& v, Object* o, int argc, const Value* a) {
    Citizen* c = citizenOf(o, nullptr);
    if (!c) return Value::integer(0);
    if (argc > 0) return Value::boolean((c->usedTransport >> v.toInt(a[0])) & 1);
    return Value::integer(int32_t(c->usedTransport));
  });

  // ---- gsGridCharacterController.getCitizen, gsGridVehicleController.getCitizen ----
  implement(vm, "gsGridCharacterController", u"getCitizen", [](VM&, Object* o, int, const Value*) {
    auto* t = dynamic_cast<TransportObj*>(o);
    auto s = t ? t->sim.lock() : nullptr;
    if (!s || t->index < 0 || size_t(t->index) >= s->characters.size()) return Value::null();
    int ci = s->characters[size_t(t->index)].citizen;
    Object* c = ci >= 0 ? citizenController(*s, ci) : nullptr;
    return c ? Value::heap(VT::Object, c) : Value::null();
  });
  implement(vm, "gsGridVehicleController", u"getCitizen", [stateFor](VM& v, Object* o, int, const Value*) {
    SimState* s = stateFor(o);
    int id = fieldInt(v, o, u"<mover>", -1);
    if (!s || id < 0 || size_t(id) >= s->movers.size() || s->movers[size_t(id)].citizen < 0) return Value::null();
    Object* c = citizenController(*s, s->movers[size_t(id)].citizen);
    return c ? Value::heap(VT::Object, c) : Value::null();
  });
}

}  // namespace sim

// ---------------------------------------------------------------------------------------------------------------
// sim.h

void forEachSimCharacter(GridObj* grid, const std::function<void(const SimCharacterView&)>& fn) {
  SimState* s = sim::stateOf(grid);
  if (!s) return;
  for (size_t i = 0; i < s->characters.size(); ++i) {
    const sim::Character& ch = s->characters[i];
    if (!ch.alive || ch.model < 0 || ch.virtualLeft > 0) continue;
    const sim::UnitModel& m = s->models[size_t(ch.model)];
    SimCharacterView view;
    view.object = sim::characterController(*s, int(i));
    view.citizen = ch.citizen;
    view.position = ch.pos;
    view.yaw = ch.yaw;
    Mat4 w = sim::poseMatrix({ch.pos, {std::sin(ch.yaw), 0, std::cos(ch.yaw)}});
    Vec3 mc = (m.boundsMin + m.boundsMax) * 0.5f, half = (m.boundsMax - m.boundsMin) * 0.5f;
    view.center = render::transformPoint(w, mc);
    view.halfExtents = half;
    view.radius = std::sqrt(dot(half, half));
    fn(view);
  }
}

script::Object* simCitizenController(GridObj* grid, int citizen) {
  SimState* s = sim::stateOf(grid);
  if (!s || citizen < 0 || size_t(citizen) >= s->citizens.size()) return nullptr;
  return sim::citizenController(*s, citizen);
}

}  // namespace regs::engine
