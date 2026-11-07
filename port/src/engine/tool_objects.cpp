// The grid player's object tools (tool_objects.h, docs/ui/construction.md §7-9): creating stops (GRIDMODE_CREATE_STOP,
// L:0x0818ff00 mouseMoveCreateStop), placing objects (GRIDMODE_PLACE_OBJECT, L:0x08198400 mouseMovePlaceObject,
// L:0x08196b00 placeObject), their commit (finishTool cases 7/10, L:0x0819a3e0.c:299-511) and cancel (cancelTool
// L:0x0819bcc0), and the bulldozer (GRIDMODE_REMOVE_OBJECTS: mouseMove case 8 L:0x0819f2a0.c:520-597, finishTool
// case 8, gsGridPlayer::removeObject L:0x08198b10). Heights are in the engine's units (1/256 cell, cell heights carry
// the same shift, see tools.h toolAddPreview).
#include "engine/tool_objects.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/grid_render.h"
#include "engine/gui.h"
#include "engine/interaction.h"
#include "engine/scene3d.h"
#include "engine/timeline.h"
#include "engine/tools.h"

namespace regs::engine {

using namespace regs::script;
using render::Vec3;

namespace {

// GRID_ITEM_* flags
constexpr uint32_t kFlagEnableBase = 0x1, kFlagJoin = 0x2, kFlagCover = 0x4, kFlagLockGround = 0x8,
                   kFlagStop = 0x2000, kFlagBridge = 0x4000, kFlagTunnel = 0x8000, kFlagBlock = 0x10000,
                   kFlagAlign4 = 0x8000000, kFlagFixedY = 0x40000000;
// GRID_ITEM_* types
constexpr int kTypeAlley = 1, kTypeMetroStation = 0x10, kTypeDock = 0x11, kTypeMarker = 0x15;
constexpr uint16_t kCellLocked = 0x1000, kCellHighlight = 0x4000, kCellNegative = 0x8000;
constexpr int kNoY = -0x8000;  // "on the ground": gsGridItem::fixY places the item

Value obj(Object* o) { return o ? Value::heap(VT::Object, o) : Value::null(); }

int intField(VM& vm, const Object* o, const char16_t* name, int def) {
  if (!o) return def;
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : int(vm.toInt(it->second));
}

Object* objField(Object* o, const char16_t* name) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() ? nullptr : it->second.object();
}

void setIntField(Object* o, const char16_t* name, int v) { o->fieldValues[name] = Value::integer(v); }

int editMode(VM& vm, Object* p) { return intField(vm, p, u"editMode", kGridModeSelectObject); }

// ---------------------------------------------------------------------------------------------------------------
// templates and items in the engine's terms

// a gsGridObject's placement facts (L: +0x1fc width, +0x200 height, +0x204 negHeight, +0x208 length, +0x20c flags,
// +0x210/+0x214 connections, +0x218 diagonals, +0x21c type, +0x220 variation, +0x224/+0x228 offsetx/z)
struct Tpl {
  Object* obj = nullptr;
  std::string name;
  int width = 1, length = 1, height = 0, negHeight = 0, type = 0, variation = 0, offx = 0, offz = 0;
  uint32_t flags = 0, conn = 0, conn2 = 0, diag = 0;
};

Tpl tplOf(VM& vm, GridObj* grid, const std::string& name) {
  Tpl t;
  auto it = grid->templates.find(name);
  if (it == grid->templates.end()) return t;
  Object* o = it->second.get();
  t.obj = o;
  t.name = name;
  t.width = std::max(1, intField(vm, o, u"width", 1));
  t.length = std::max(1, intField(vm, o, u"length", 1));
  auto f = o->fieldValues.find(u"height");
  t.height = f == o->fieldValues.end() || f->second.isNone() ? 0 : int(vm.toFloat(f->second) * 256.0f);
  t.negHeight = intField(vm, o, u"negHeight", 0);
  t.type = intField(vm, o, u"type", 0);
  t.variation = intField(vm, o, u"variation", 0);
  t.offx = intField(vm, o, u"offsetx", 0);
  t.offz = intField(vm, o, u"offsetz", 0);
  t.flags = uint32_t(intField(vm, o, u"flags", 0));
  t.conn = uint32_t(intField(vm, o, u"connections", 0));
  t.conn2 = uint32_t(intField(vm, o, u"connections2", 0));
  t.diag = uint32_t(intField(vm, o, u"diagonals", 0));
  return t;
}

std::string templateName(VM& vm, Object* o) {
  if (!o) return {};
  auto it = o->fieldValues.find(u"idName");
  return it == o->fieldValues.end() ? std::string() : utf16ToUtf8(vm.toStr(it->second));
}

// an item as gsGridItem holds it: +0x2c x, +0x30 y, +0x34 z, +0x38/+0x40 the turned size, +0x3c height, +0x44 orient,
// +0x48 flags (item | template), +0x4c/+0x50 the turned connections, +0x54 diagonals, +0x58 type, +0x1c parent
struct Info {
  bool ok = false;
  uint32_t index = 0;
  int x = 0, y = 0, z = 0, sx = 1, sz = 1, orient = 0, parent = -1;
  uint32_t flags = 0, conn = 0, conn2 = 0, diag = 0;
  Tpl tpl;
  int type() const { return tpl.type; }
};

int engineShift(const GridData& g) { return 0x200 - int(g.heightBase); }

Info infoOf(VM& vm, GridObj* grid, uint32_t i) {
  Info n;
  const GridData& g = *grid->data;
  if (i >= g.items.size() || g.items[i].removed) return n;
  const GridData::Item& it = g.items[i];
  n.tpl = tplOf(vm, grid, g.types[it.type]);
  if (!n.tpl.obj) return n;
  n.ok = true;
  n.index = i;
  n.x = it.x;
  n.z = it.z;
  n.y = it.y + engineShift(g);
  n.orient = it.orient & 3;
  ItemFootprint fp = itemFootprint(n.tpl.width, n.tpl.length, n.tpl.diag, n.orient);
  n.sx = fp.sx;
  n.sz = fp.sz;
  n.diag = fp.diag;
  n.parent = it.parent ? int(it.parent) - 1 : -1;
  n.flags = it.flags | n.tpl.flags;
  n.conn = rotateConnections(n.tpl.conn, n.orient * 2);
  n.conn2 = rotateConnections(n.tpl.conn2, n.orient * 2);
  return n;
}

GridData::Item& itemRef(GridObj* grid, uint32_t i) { return grid->data->items[i]; }

void addItemFlags(GridObj* grid, uint32_t i, uint32_t f) { itemRef(grid, i).flags |= f; }

// gsGridItem::getRoadXZ (L:0x08181510): the corner of the 4×4 road block an item belongs to; getAlleyXZ: the same on
// the alleys' 1×1 grid
std::pair<int, int> blockXZ(const Info& n, int block = 4) {
  const Tpl& t = n.tpl;
  switch (n.orient) {
    case 1: return {n.x - t.offz, n.z + t.offx - block + n.sz};
    case 2: return {n.x + t.offx - block + n.sx, n.z + t.offz - block + n.sz};
    case 3: return {n.x + t.offz - block + n.sx, n.z - t.offx};
    default: return {n.x - t.offx, n.z - t.offz};
  }
}
std::pair<int, int> roadXZ(const Info& n) { return blockXZ(n, n.type() == kTypeAlley ? 1 : 4); }

// ---------------------------------------------------------------------------------------------------------------
// gsGrid::getObject(type, variation, connections, connections2, flags, &orient) (L:0x0813f520): the template search
// map loadGridObject fills (L:0x08149f70.c:619-848): every template under its key (flags & 0xcf0f0), and turned by
// 90° steps (orientation 3, 2, 1) while its connections are not symmetric; a later template with the same key takes
// the slot but keeps the first orientation

struct SearchKey {
  uint32_t type, variation, conn, conn2, flags;
  bool operator<(const SearchKey& o) const {
    if (type != o.type) return type < o.type;
    if (variation != o.variation) return variation < o.variation;
    if (conn != o.conn) return conn < o.conn;
    if (conn2 != o.conn2) return conn2 < o.conn2;
    return flags < o.flags;
  }
};
struct SearchHit {
  int orient = 0;
  std::string name;
};
struct SearchMap {
  size_t templates = size_t(-1);
  std::map<SearchKey, SearchHit> map;
};
std::unordered_map<const GridObj*, SearchMap> g_search;

const SearchMap& searchMap(VM& vm, GridObj* grid) {
  SearchMap& m = g_search[grid];
  if (m.templates == grid->templates.size()) return m;
  m.templates = grid->templates.size();
  m.map.clear();
  std::vector<std::pair<uint64_t, std::string>> order;  // load order: the objects' creation numbers
  for (auto& [name, o] : grid->templates) order.emplace_back(o->serial, name);
  std::sort(order.begin(), order.end());
  for (auto& [serial, name] : order) {
    (void)serial;
    Tpl t = tplOf(vm, grid, name);
    uint32_t c = t.conn, c2 = t.conn2, f = t.flags & 0xcf0f0u;
    auto put = [&](uint32_t a, uint32_t b, int orient) {
      SearchKey k{uint32_t(t.type), uint32_t(t.variation), a, b, f};
      auto it = m.map.find(k);
      if (it == m.map.end()) m.map.emplace(k, SearchHit{orient, name});
      else it->second.name = name;
    };
    put(c, c2, 0);
    if (!c && !c2) continue;
    int orient = 0;
    for (int count = 1; count <= 3; ++count) {
      c = rotateConnections(c, 6);
      c2 = rotateConnections(c2, 6);
      orient = (orient + 3) & 3;
      put(c, c2, orient);
      if (count == 1 && rotateConnections(c, 4) == c && rotateConnections(c2, 4) == c2) break;
    }
  }
  return m;
}

const SearchHit* getObject(VM& vm, GridObj* grid, uint32_t type, uint32_t variation, uint32_t conn, uint32_t conn2,
                           uint32_t flags) {
  const SearchMap& m = searchMap(vm, grid);
  auto it = m.map.find(SearchKey{type, variation, conn, conn2, flags & 0xcf0f0u});
  return it == m.map.end() ? nullptr : &it->second;
}

// gsGridPlayer::createRoadItem (L:0x0818c3b0): the road, track or stop piece of a type and variation with the given
// connections, at the block corner (x, z) shifted by the template's offsets for its orientation; a preview (TEMP) not
// listed in newItems. Without an exact template the nearest one stands in, marked as an error (0x200). Flag 2 of
// `flags` is kept. Returns −1 when the type has no template at all.
int createRoadItem(VM& vm, GridObj* grid, int x, int y, int z, uint32_t conn, uint32_t conn2, int type, int variation,
                   uint32_t flags, int parent = -1) {
  const SearchHit* hit = getObject(vm, grid, uint32_t(type), uint32_t(variation), conn, conn2, flags);
  bool exact = hit != nullptr;
  if (!hit) hit = getObject(vm, grid, uint32_t(type), uint32_t(variation), 0, 0, flags & 0xc000);
  if (!hit) hit = getObject(vm, grid, uint32_t(type), uint32_t(variation), 0, 0, 0);
  if (!hit) hit = getObject(vm, grid, uint32_t(type), 0, 0, 0, 0);
  if (!hit) return -1;
  Tpl t = tplOf(vm, grid, hit->name);
  int block = type != kTypeAlley ? 4 : 1;
  switch (hit->orient) {
    case 1: x += t.offz, z += block - t.offx - t.width; break;
    case 2: z += block - t.offz - t.length, x += block - t.offx - t.width; break;
    case 3: z += t.offx, x += block - t.offz - t.length; break;
    default: x += t.offx, z += t.offz; break;
  }
  int i = gridEditAddItem(vm, grid, hit->name, x, y, z, hit->orient, kItemTemp, parent, false);
  if (i < 0) return -1;
  addItemFlags(grid, uint32_t(i), (flags & kFlagJoin) | (exact ? 0 : kItemNegative));
  return i;
}

// REGS_DEBUG_TOOLS=1: what a preview meets
void debugConflicts(VM& vm, GridObj* grid, uint32_t item, bool conflict) {
  static const bool on = std::getenv("REGS_DEBUG_TOOLS") != nullptr;
  if (!on) return;
  Info n = infoOf(vm, grid, item);
  LOG_INFO("tool preview %u %s at %d,%d y %d size %dx%d orient %d conn %08x flags %08x: %s", item, n.tpl.name.c_str(), n.x,
           n.z, n.y, n.sx, n.sz, n.orient, n.conn, n.flags, conflict ? "CONFLICT" : "ok");
  for (uint32_t o : gridItemsNear(vm, grid, n.x, n.z, n.x + n.sx - 1, n.z + n.sz - 1)) {
    if (o == item || !gridItemsConflict(vm, grid, o, item)) continue;
    Info m = infoOf(vm, grid, o);
    LOG_INFO("  conflicts with %u %s at %d,%d y %d size %dx%d conn %08x flags %08x", o, m.tpl.name.c_str(), m.x, m.z, m.y,
             m.sx, m.sz, m.conn, m.flags);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// highlights

// gsGridItem::gsGridItem (L:0x08187fc0.c:152-161): a STOP item gets its controller at once and the grid announces it
// with event 6 transportStopCreated, previews too (the simulation announces the mounted ones); ui.script gives a
// preview bus or tram stop its direction arrow there, which goes with the controller when the preview is removed
void announcePreviewStop(VM& vm, GridObj* grid, uint32_t item) {
  Info n = infoOf(vm, grid, item);
  if (!n.ok || !(n.flags & kFlagStop) || !(n.flags & kItemTemp)) return;
  int ty = n.type();
  int stopType = (ty == 2 || ty == 5 || ty == 6) ? 0 : (ty >= 10 && ty <= 12) ? 1 : (ty >= 13 && ty <= 15) ? 2
                 : ty == kTypeDock ? 3 : ty == 0x12 ? 4 : -1;
  if (stopType < 0) return;
  Object* c = gridItemController(vm, grid, item);
  if (!c) return;
  c->fieldValues[u"type"] = Value::integer(stopType);
  try {
    bool fired = vm.fireEvent(grid, u"transportStopCreated", {obj(c)});
    static const bool debug = std::getenv("REGS_DEBUG_TOOLS") != nullptr;
    if (debug) {
      Value tr;
      try {
        tr = vm.resolve(vm.getMember(obj(c), u"$tracer1"));
      } catch (const ScriptError&) {
      }
      LOG_INFO("preview stop %u announced (%d), tracer %s", item, fired, tr.object() ? "made" : "none");
    }
  } catch (const std::exception& e) {
    LOG_ERROR("transportStopCreated: %s", e.what());
  }
}

void unselectItemArea(VM& vm, GridObj* grid, uint32_t item) {
  Info n = infoOf(vm, grid, item);
  if (n.ok) gridUnselectArea(vm, grid, n.x, n.z, n.sx, n.sz);
  gridSetSelected(vm, grid, item, false, false);
}

// mouseMovePlaceObject / mouseMoveCreateStop / cancelTool 7, 10: the previews leave (last first) with their area's
// highlight; the replaced set is emptied
void dropPreviews(VM& vm, Object* player, GridObj* grid) {
  ToolState& t = toolState(player);
  for (auto i = t.newItems.rbegin(); i != t.newItems.rend(); ++i) {
    unselectItemArea(vm, grid, *i);
    gridEditRemoveItem(vm, grid, *i);
  }
  t.newItems.clear();
  t.replaced.clear();
}

// the template events of mounted items (event 12 onCreateInstance), after their batch
void fireInstanceEvents(VM& vm, GridObj* grid, const std::vector<uint32_t>& items) {
  for (uint32_t i : items) {
    if (i >= grid->data->items.size() || grid->data->items[i].removed) continue;
    if (grid->data->items[i].flags & (kItemTemp | kItemHidden)) continue;
    Tpl t = tplOf(vm, grid, grid->data->types[grid->data->items[i].type]);
    Object* c = gridItemController(vm, grid, i);
    if (!t.obj || !c) continue;
    try {
      vm.fireEvent(t.obj, u"onCreateInstance", {obj(c)});
    } catch (const std::exception& e) {
      LOG_ERROR("onCreateInstance: %s", e.what());
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// gsGridItem::fixY (L:0x08187760) for a new item at (x, z): its base height from the ground under its corners. y is
// the requested height (kNoY = on the ground); parentY the parent's height or kNoY.

int fixY(GridObj* grid, const Tpl& t, int x, int z, int orient, uint32_t flags, int y, int parentY) {
  const GridData& g = *grid->data;
  ItemFootprint fp = itemFootprint(t.width, t.length, t.diag, orient);
  int sx = fp.sx, sz = fp.sz;
  if (x < 0 || z < 0 || x + sx > g.width || z + sz > g.length) return y;
  int px1 = std::min(sx, g.width - x - 1), pz1 = std::min(sz, g.length - z - 1);
  auto h = [&](int px, int pz) { return int(g.height[size_t(z + pz) * size_t(g.width) + size_t(x + px)]); };
  auto locked = [&](int px, int pz) {
    return (g.flags[size_t(z + pz) * size_t(g.width) + size_t(x + px)] & kCellLocked) != 0;
  };
  // a corner belongs to the item when a covered cell touches it (getY is 0x7fffffff elsewhere)
  auto inside = [&](int px, int pz) {
    if (!fp.diag) return true;
    for (int dz = -1; dz <= 0; ++dz)
      for (int dx = -1; dx <= 0; ++dx)
        if (px + dx >= 0 && pz + dz >= 0 && px + dx < sx && pz + dz < sz && itemCellCover(fp, flags, px + dx, pz + dz))
          return true;
    return false;
  };
  int lo = 0x8000, hi = -0x8000;
  for (int pz = 0; pz <= pz1; ++pz)
    for (int px = 0; px <= px1; ++px) lo = std::min(lo, h(px, pz)), hi = std::max(hi, h(px, pz));
  bool align = t.type == kTypeMetroStation || (flags & kFlagAlign4);
  if (flags & kFlagEnableBase) {  // on a base: the highest corner
    if (y != kNoY) return y;
    if (align) return (hi + 0x7f) & ~0x7f;
    if (t.type == kTypeDock) return hi < 0x181 ? 0x100 : 0x200;
    return hi;
  }
  if ((flags & 0x40014004u) == kFlagBridge) return (hi + 0xc0) & ~0x7f;  // above the highest corner
  if ((flags & 0x40018004u) == kFlagTunnel) return (lo - 0x1c0) & ~0x7f;  // below the lowest one
  if ((flags & (kFlagFixedY | kFlagLockGround)) == kFlagLockGround) {   // the mean of its corners, or a locked one
    int sum = 0, count = 0;
    bool found = false;
    for (int pz = 0; pz <= pz1 && !found; ++pz)
      for (int px = 0; px <= px1 && !found; ++px) {
        if (!inside(px, pz)) continue;
        if (locked(px, pz)) {
          sum = h(px, pz), count = 1, found = true;
          break;
        }
        sum += h(px, pz), ++count;
      }
    int v = count ? sum / count : 0;
    if (t.type == kTypeAlley) return (v + 0x20) & ~0x3f;
    v = (v + 0x40) & ~0x7f;
    if (t.type == kTypeDock) v = hi < 0x181 ? 0x100 : 0x200;
    return v;
  }
  if (flags & kFlagFixedY) return y;
  if (parentY != kNoY) return parentY;
  int sum = 0;
  for (int pz = 0; pz <= pz1; ++pz)
    for (int px = 0; px <= px1; ++px) sum += h(px, pz);
  return sum / ((pz1 + 1) * (px1 + 1));
}

// ---------------------------------------------------------------------------------------------------------------
// placeObject (L:0x08196b00): the template's onPlace(parts, x, y, z, orient) (event 10) lists extra parts, five values
// each: a template name or a road / track type, x, y (cells), z, and the orientation (names) or connections (types),
// relative to the item turned with it. Named parts become previews below the item (flag 0x2000000); road and track
// parts become blocks through the laying tools (createRoadBlock / createTrackBlock, tools.h toolBlocks).

void placeObject(VM& vm, ControlObj* player, GridObj* grid, uint32_t item) {
  Info n = infoOf(vm, grid, item);
  if (!n.ok) return;
  Ref<ArrayObj> parts = make<ArrayObj>();
  try {
    vm.fireEvent(n.tpl.obj, u"onPlace",
                 {Value::heap(VT::Array, parts.get()), Value::integer(n.x), Value::real(float(n.y) * 0.00390625f),
                  Value::integer(n.z), Value::integer(n.orient)});
  } catch (const std::exception& e) {
    LOG_ERROR("onPlace %s: %s", n.tpl.name.c_str(), e.what());
    return;
  }
  Value savedMode = player->fieldValues[u"toolMode"], savedType = player->fieldValues[u"toolType"];
  std::vector<Value> a = parts->items;
  for (Value& v : a) v = vm.resolve(v);
  static bool warned = false;
  for (size_t k = 0; k + 4 < a.size(); k += 5) {
    int a1 = int(vm.toInt(a[k + 1])), a3 = int(vm.toInt(a[k + 3]));
    int y = int(std::nearbyint(vm.toFloat(a[k + 2]) * 256.0f)) + n.y;
    if (a[k].type() != VT::Int) {
      Tpl p = tplOf(vm, grid, utf16ToUtf8(vm.toStr(a[k])));
      if (!p.obj) continue;
      int dx, dz;
      switch (n.orient) {
        case 1: dx = a3, dz = n.sz - p.width - a1; break;
        case 2: dx = n.sx - p.width - a1, dz = n.sz - p.length - a3; break;
        case 3: dx = n.sx - p.length - a3, dz = a1; break;
        default: dx = a1, dz = a3; break;
      }
      int orient = (n.orient + int(vm.toInt(a[k + 4]))) & 3;
      toolAddPreview(vm, player, grid, p.name, n.x + dx, y, n.z + dz, orient, kItemAutoChild, int(item));
      continue;
    }
    uint32_t v = uint32_t(vm.toInt(a[k]));
    int code = int(v & 0xffff);
    setIntField(player, u"toolType", int(v >> 16));
    switch (code) {  // the laying tools read toolMode as the kind of road or track
      case 2: setIntField(player, u"toolMode", 0); break;
      case 5: setIntField(player, u"toolMode", 3); break;
      case 6: setIntField(player, u"toolMode", 4); break;
      case 7: setIntField(player, u"toolMode", 5); break;
      case 9: setIntField(player, u"toolMode", 6); break;
      case 0xd: setIntField(player, u"toolMode", 7); break;
      case 0xe: setIntField(player, u"toolMode", 8); break;
      case 0xf: setIntField(player, u"toolMode", 9); break;
      default: break;
    }
    bool road = code >= 2 && code <= 7, track = code == 8 || code == 9 || (code >= 0xd && code <= 0xf);
    if (!road && !track) continue;
    int dx, dz;
    switch (n.orient) {
      case 1: dx = a3, dz = n.sz - 4 - a1; break;
      case 2: dx = n.sx - 4 - a1, dz = n.sz - 4 - a3; break;
      case 3: dx = n.sx - 4 - a3, dz = a1; break;
      default: dx = a1, dz = a3; break;
    }
    uint32_t conn = rotateConnections(uint32_t(vm.toInt(a[k + 4])), n.orient * 2);
    auto& fn = road ? toolBlocks().road : toolBlocks().track;
    if (fn) {
      fn(vm, player, grid, n.x + dx, n.z + dz, conn, true, true, road ? -1 : code, y);
    } else if (!warned) {
      warned = true;
      LOG_WARN("placeObject: no %s block tool registered, the parts of %s are left out", road ? "road" : "track",
               n.tpl.name.c_str());
    }
  }
  player->fieldValues[u"toolMode"] = savedMode;
  player->fieldValues[u"toolType"] = savedType;
}

// ---------------------------------------------------------------------------------------------------------------
// GRIDMODE_CREATE_STOP (L:0x0818ff00): the road (toolMode GRIDSTOP_BUS: types 2-6) or tram track (GRIDSTOP_TRAM:
// 10-12) under the cursor gets a stop piece of variant toolType on the side of the cursor; the stop on the other side
// of the same block lends its name. Junctions, curves, bridges and tunnels take none (a red cell).

// the stop preview for the cursor cell (cx, cz): its item (TEMP, not in newItems, flag 2 set; red when it is an
// error) or −1 when the cell has no fitting road
int stopPreview(VM& vm, GridObj* grid, int cx, int cz, int toolMode, int toolType) {
  GridData& g = *grid->data;
  int road = -1;
  uint32_t mask = 0;
  bool unfit = false;
  for (uint32_t o : gridGetObjects(vm, grid, cx, cz, 1, 1)) {
    Info n = infoOf(vm, grid, o);
    if (!n.ok) continue;
    bool bus = n.type() >= 2 && n.type() <= 6, tram = n.type() >= 10 && n.type() <= 12;
    if (!((bus && toolMode == 0) || (tram && toolMode == 1))) continue;
    Info root = n.parent >= 0 ? infoOf(vm, grid, uint32_t(n.parent)) : n;
    if ((root.ok ? root.flags : n.flags) & 0xe000) continue;  // stops, bridges, tunnels
    auto [rx, rz] = roadXZ(n);
    int shift = n.type() == 5 || n.type() == 0xb ? 8 : n.type() == 6 || n.type() == 0xc ? 16 : 0;
    uint32_t c = n.conn;
    auto is = [&](uint32_t pattern) { return c == pattern || c == pattern << 8 || c == pattern << 16; };
    road = int(o);
    if (is(0x44)) mask |= (cz <= rz + 1 ? 0x01u : 0x10u) << shift;              // east-west: north or south side
    else if (is(0x11)) mask |= (cx <= rx + 1 ? 0x40u : 0x04u) << shift;         // north-south: west or east
    else if (is(0x22)) mask |= (rx + rz + 3 < cx + cz ? 0x08u : 0x80u) << shift;  // diagonals
    else if (is(0x88)) mask |= (rx - rz <= cx - cz ? 0x02u : 0x20u) << shift;
    else unfit = true;
  }
  int stop = -1;
  if (road >= 0 && !unfit) {
    Info r = infoOf(vm, grid, uint32_t(road));
    std::string name;  // the stop facing the other way on this block
    for (uint32_t o : gridGetObjects(vm, grid, r.x, r.z, r.sx, r.sz)) {
      Info s = infoOf(vm, grid, o);
      if (s.ok && (s.flags & kFlagStop) && !(s.conn & mask)) name = o < g.names.size() ? g.names[o] : std::string();
    }
    auto [rx, rz] = roadXZ(r);
    stop = createRoadItem(vm, grid, rx, r.y, rz, mask, 0, r.type(), toolType, r.flags | kFlagStop);
    if (stop >= 0) {
      if (!name.empty() && g.names[size_t(stop)].empty()) g.names[size_t(stop)] = name;
      addItemFlags(grid, uint32_t(stop), kFlagJoin);
    }
  }
  return stop;
}

// REGS_TEST_STOPS=1: every bus and tram stop of the map rebuilt by the stop tool from a cell it covers, compared with
// the saved one (template, position, orientation); a check of the placement rules against the original's own stops
void testStops(VM& vm, GridObj* grid) {
  const GridData& g = *grid->data;
  int checked = 0, same = 0;
  size_t count = g.items.size();
  gridEditBegin(vm, grid);
  for (uint32_t i = 0; i < count; ++i) {
    Info s = infoOf(vm, grid, i);
    if (!s.ok || !(s.flags & kFlagStop) || (s.flags & kItemTemp) || s.parent >= 0) continue;
    bool bus = s.type() >= 2 && s.type() <= 6, tram = s.type() >= 10 && s.type() <= 12;
    if (!bus && !tram) continue;
    ItemFootprint fp{s.sx, s.sz, s.diag};
    // any covered cell the stop tool would build this stop from (the user points at the road or track under it)
    int cx = -1, cz = -1;
    Info n;
    bool ok = false;
    for (int pz = 0; pz < s.sz && !ok; ++pz)
      for (int px = 0; px < s.sx && !ok; ++px) {
        if (!itemCellCover(fp, s.flags, px, pz)) continue;
        cx = s.x + px, cz = s.z + pz;
        int p = stopPreview(vm, grid, cx, cz, bus ? 0 : 1, s.tpl.variation);
        n = p >= 0 ? infoOf(vm, grid, uint32_t(p)) : Info{};
        ok = n.ok && n.tpl.obj == s.tpl.obj && n.x == s.x && n.z == s.z && n.orient == s.orient && n.y == s.y;
        if (p >= 0) gridEditRemoveItem(vm, grid, uint32_t(p));
      }
    if (cx < 0) continue;
    ++checked;
    if (ok) ++same;
    else if (checked - same <= 12) {
      LOG_INFO("stop test: %s at %d,%d orient %d y %d (cell %d,%d) -> %s at %d,%d orient %d y %d", s.tpl.name.c_str(), s.x,
               s.z, s.orient, s.y, cx, cz, n.ok ? n.tpl.name.c_str() : "none", n.x, n.z, n.orient, n.y);
      for (uint32_t o : gridGetObjects(vm, grid, s.x - 4, s.z - 4, s.sx + 8, s.sz + 8)) {
        Info m = infoOf(vm, grid, o);
        if (m.type() < 2 || m.type() > 12) continue;
        LOG_INFO("    cell item %s type %d at %d,%d size %dx%d orient %d conn %08x flags %08x parent %d", m.tpl.name.c_str(),
                 m.type(), m.x, m.z, m.sx, m.sz, m.orient, m.conn, m.flags, m.parent);
      }
    }
  }
  gridEditEnd(vm, grid);
  LOG_INFO("stop test: %d of %d stops rebuilt identically", same, checked);
}

int upgradedStop(VM& vm, GridObj* grid, uint32_t preview);

void moveCreateStop(VM& vm, ControlObj* player, GridObj* grid, float mx, float my) {
  ToolState& t = toolState(player);
  static bool tested = false;
  if (!tested && std::getenv("REGS_TEST_STOPS")) tested = true, testStops(vm, grid);
  ToolPick p = toolPick(vm, player, mx, my, false);
  if (!p.hit) return;
  int cx = p.cellX, cz = p.cellZ;
  if (cx == t.cellX && cz == t.cellZ) return;
  GridData& g = *grid->data;
  gridEditBegin(vm, grid);
  if (t.newItems.empty()) {
    if (t.cellX != -1) gridUnselectArea(vm, grid, t.cellX, t.cellZ, 1, 1);
  } else {
    dropPreviews(vm, player, grid);
  }
  t.newItems.clear();
  t.replaced.clear();
  t.upgraded.clear();
  int stop = stopPreview(vm, grid, cx, cz, intField(vm, player, u"toolMode", 0), intField(vm, player, u"toolType", 0));
  int announce = -1;
  bool announceError = false;
  if (stop >= 0) {
    if (!(g.items[size_t(stop)].flags & kItemNegative)) {
      t.newItems.push_back(uint32_t(stop));
      bool conflict = gridCheckConflict(vm, grid, uint32_t(stop), {});
      if (conflict) addItemFlags(grid, uint32_t(stop), kItemNegative);
      debugConflicts(vm, grid, uint32_t(stop), conflict);
      gridSelectObjectArea(vm, grid, uint32_t(stop), conflict);
      announce = stop, announceError = conflict;
      if (!conflict)
        if (int old = upgradedStop(vm, grid, uint32_t(stop)); old >= 0) t.upgraded.push_back(uint32_t(old));
    } else {
      gridEditRemoveItem(vm, grid, uint32_t(stop));  // no fitting template: nothing is shown (not even the red cell)
    }
  } else {
    gridSelectArea(vm, grid, cx, cz, 1, 1, true);
  }
  gridEditEnd(vm, grid);
  if (announce >= 0) {  // after the batch, when the preview's controller can be made
    announcePreviewStop(vm, grid, uint32_t(announce));
    if (announceError)  // the engine fires the controller's onError (event 7) for a conflicting stop: the arrow goes
      if (Object* c = gridItemController(vm, grid, uint32_t(announce))) vm.fireEvent(c, u"onError", {});
  }
  t.cellX = cx;
  t.cellZ = cz;
  vm.fireEvent(player, u"onHoverConstruct", {});
}

// ---------------------------------------------------------------------------------------------------------------
// GRIDMODE_PLACE_OBJECT (L:0x08198400): toolObject (or toolObject2, the diagonal variant, on odd toolMode) centred on
// the cursor, turned by toolMode, raised by toolType; stations and ALIGN_4 objects snap to the 4-cell grid, elevated
// ones (BRIDGE) stand 2 cells above the picked point, underground ones (TUNNEL) on the level toolSize·2 cells.

void movePlaceObject(VM& vm, ControlObj* player, GridObj* grid, float mx, float my) {
  ToolState& t = toolState(player);
  Object* tobj = objField(player, u"toolObject");
  Object* tobj2 = objField(player, u"toolObject2");
  if (!tobj) return;
  int orient = intField(vm, player, u"toolMode", 0);
  if (tobj2) {
    if (orient & 1) tobj = tobj2;
    orient >>= 1;
  }
  orient &= 3;
  Tpl tp = tplOf(vm, grid, templateName(vm, tobj));
  if (!tp.obj) return;
  const GridData& g = *grid->data;
  int y = (tp.flags & kFlagTunnel) ? intField(vm, player, u"toolSize", 0) << 9 : kNoY;
  int cx = -1, cz = -1, hovered = -1;
  if (y != kNoY) {
    ToolPick p = toolPickLevel(vm, player, mx, my, y);  // the subway level: the plane at y, tunnels only
    if (!p.hit) return;
    cx = p.cellX, cz = p.cellZ;
  } else {
    ToolPick p = toolPick(vm, player, mx, my, false);
    if (!p.hit) return;
    cx = p.cellX, cz = p.cellZ, hovered = p.item;
    y = p.y;  // the hit item's height, else the ground's rounded to a cell
  }
  if (hovered >= 0 && (gridItemShape(vm, grid, uint32_t(hovered)).flags & kItemOptional)) hovered = -1;
  if (tp.flags & kFlagBridge) {
    y += 0x200;
    hovered = -1;
  } else if (tp.flags & kFlagTunnel) {
    hovered = -1;
  } else {
    y = kNoY;
  }
  int ox = tp.offx, oz = tp.offz;
  if (orient == 1 || orient == 2) ox = (-ox - tp.width) & 3;
  if (orient == 2 || orient == 3) oz = (-oz - tp.length) & 3;
  int sizeX = tp.width, sizeZ = tp.length, offX = ox, offZ = oz;
  if (orient & 1) sizeX = tp.length, sizeZ = tp.width, offX = oz, offZ = ox;
  int x, z;
  if (tp.type == kTypeMetroStation || (tp.flags & kFlagAlign4)) {
    x = ((cx - (sizeX >> 1)) & ~3) + offX;
    z = ((cz - (sizeZ >> 1)) & ~3) + offZ;
  } else {
    x = cx - (sizeX >> 1);
    z = cz - (sizeZ >> 1);
  }
  if (x == t.cellX && z == t.cellZ) return;

  gridEditBegin(vm, grid);
  dropPreviews(vm, player, grid);
  // gsGrid::addObject(template, x, y, z, orient, TEMP, null, true): stations and ALIGN_4 items round their height up
  // to half a cell; on the ground (or on a base) fixY finds it
  bool align = tp.type == kTypeMetroStation || (tp.flags & kFlagAlign4);
  if (align && y != kNoY) y = (y + 0x7f) & ~0x7f;
  int toolType = intField(vm, player, u"toolType", 0);
  auto yFor = [&](int requested, uint32_t extra, int parentY) {
    if (requested == kNoY || (tp.flags & kFlagEnableBase)) return fixY(grid, tp, x, z, orient, tp.flags | extra, requested, parentY);
    return requested;
  };
  int baseY = yFor(y, 0, kNoY);
  if (baseY == kNoY) baseY = 0;  // off the map: checkConflict marks it
  int main = toolAddPreview(vm, player, grid, tp.name, x, baseY + toolType, z, orient, toolType ? kFlagFixedY : 0);
  if (main >= 0) {
    // standing on the hovered item: it becomes the parent when they do not conflict (not for stations and markers)
    if (hovered >= 0) {
      Info h = infoOf(vm, grid, uint32_t(hovered));
      if ((h.flags & kItemOptional) && h.parent >= 0) h = infoOf(vm, grid, uint32_t(h.parent));
      if (h.ok && !(h.flags & kItemOptional) && toolType == 0 && tp.type != kTypeMetroStation && tp.type != kTypeMarker &&
          !gridItemsConflict(vm, grid, h.index, uint32_t(main))) {
        gridEditRemoveItem(vm, grid, uint32_t(main));
        t.newItems.pop_back();
        main = toolAddPreview(vm, player, grid, tp.name, x, h.y + toolType, z, orient, 0, int(h.index));
      }
    }
  }
  if (main >= 0) {
    for (size_t k = 0; k < t.newItems.size(); ++k) placeObject(vm, player, grid, t.newItems[k]);
    bool conflict = false;
    for (size_t k = 0; k < t.newItems.size(); ++k) {
      if (gridCheckConflict(vm, grid, t.newItems[k], t.replaced)) conflict = true;
      if (k > 0 && gridItemsConflict(vm, grid, t.newItems[k - 1], t.newItems[k])) conflict = true;
    }
    if ((g.items[size_t(main)].flags & (kItemOptional | kItemHidden)) == (kItemOptional | kItemHidden)) {
      conflict = true;
      addItemFlags(grid, uint32_t(main), kItemNegative);
    }
    for (uint32_t i : t.newItems) {
      debugConflicts(vm, grid, i, conflict);
      gridSelectObjectArea(vm, grid, i, conflict);
    }
  }
  gridEditEnd(vm, grid);
  for (uint32_t i : t.newItems) announcePreviewStop(vm, grid, i);  // docks, heliports, station platforms
  t.cellX = x;
  t.cellZ = z;
  vm.fireEvent(player, u"onHoverConstruct", {});
}

// the live stop a stop preview would upgrade: same type, place, height and connections, another template; −1 for none
int upgradedStop(VM& vm, GridObj* grid, uint32_t preview) {
  Info n = infoOf(vm, grid, preview);
  if (!n.ok || !(n.flags & kFlagStop)) return -1;
  for (uint32_t o : gridGetObjects(vm, grid, n.x, n.z, n.sx, n.sz)) {
    Info s = infoOf(vm, grid, o);
    if (s.ok && (s.flags & kFlagStop) && s.conn == n.conn && s.type() == n.type() && s.x == n.x && s.z == n.z &&
        s.y == n.y && s.tpl.obj != n.tpl.obj)
      return int(o);
  }
  return -1;
}

// finishTool cases 7 and 10 (L:0x0819a3e0.c:299-511): when no preview conflicts (against the map without the replaced
// items, and each with the one before it) the replaced items go and the previews are mounted. A new stop on the place
// of a stop of the same type, facing the same way, upgrades it (the old stop's name, controller and line places move
// to the new one, gsTransportLines::removeStop); the port turns the old item into the new template in place, which
// keeps its index, controller and line memberships.
bool finishPlace(VM& vm, ControlObj* player, GridObj* grid) {
  ToolState& t = toolState(player);
  if (!grid) return true;
  GridData& g = *grid->data;
  int mode = editMode(vm, player);
  gridEditBegin(vm, grid);
  bool conflict = false;
  for (size_t k = 0; k < t.newItems.size(); ++k) {
    if (gridCheckConflict(vm, grid, t.newItems[k], t.replaced)) conflict = true;
    if (k > 0 && gridItemsConflict(vm, grid, t.newItems[k - 1], t.newItems[k])) conflict = true;
  }
  // the editor records the placing as one group (finishTool L:0x0819a3e0.c:299-508)
  Object* tl = conflict ? nullptr : playerTimeline(vm, player);
  if (tl) {  // placing: "%9 " and the object (L:0x0819a3e0.c:302-311), stops "%!10"
    std::string what = !t.newItems.empty() && t.newItems[0] < g.items.size() ? g.types[g.items[t.newItems[0]].type] : std::string();
    timelineBeginGroup(vm, tl, mode == kGridModeCreateStop ? std::string("%!10") : "%9 " + what);
  }
  if (!conflict)
    for (uint32_t r : t.replaced) {
      gridSetSelected(vm, grid, r, false, false);
      timelineRecordDelete(vm, tl, grid, r);
      gridEditRemoveItem(vm, grid, r);
    }
  grid->mountedCopies.clear();
  std::vector<uint32_t> mounted;
  std::vector<std::pair<uint32_t, Object*>> upgraded;  // old stop item, its new template
  for (uint32_t i : t.newItems) {
    unselectItemArea(vm, grid, i);
    if (conflict) continue;
    Info n = infoOf(vm, grid, i);
    if (mode == kGridModeCreateStop && n.ok && (n.flags & kFlagStop)) {
      int old = upgradedStop(vm, grid, i);
      if (old >= 0) {
        timelineRecordDelete(vm, tl, grid, uint32_t(old));  // replayed as the old stop going and the new one coming
        GridData::Item& it = g.items[size_t(old)];
        it.type = g.items[i].type;
        it.orient = g.items[i].orient;
        gridEditItemChanged(grid, uint32_t(old));
        timelineRecordCreate(vm, tl, grid, uint32_t(old));
        upgraded.emplace_back(uint32_t(old), n.tpl.obj);
        continue;
      }
    }
    int m = gridMountObject(vm, grid, i);
    if (m >= 0) mounted.push_back(uint32_t(m));
    if (m >= 0) timelineRecordCreate(vm, tl, grid, uint32_t(m));
  }
  for (auto i = t.newItems.rbegin(); i != t.newItems.rend(); ++i) gridEditRemoveItem(vm, grid, *i);
  grid->mountedCopies.clear();
  t.newItems.clear();
  t.replaced.clear();
  if (tl) timelineEndGroup(vm, tl);
  gridEditEnd(vm, grid);
  for (auto& [item, model] : upgraded)
    if (Object* c = gridItemController(vm, grid, item)) {
      c->fieldValues[u"<model>"] = obj(model);
      try {
        vm.fireEvent(c, u"itemUpdated", {});  // event 6
      } catch (const std::exception& e) {
        LOG_ERROR("itemUpdated: %s", e.what());
      }
    }
  fireInstanceEvents(vm, grid, mounted);
  return true;
}

// cancelTool cases 7 and 10 (L:0x0819bcc0.c:119-170)
void cancelPlace(VM& vm, ControlObj* player, GridObj* grid) {
  if (!grid) return;
  ToolState& t = toolState(player);
  gridEditBegin(vm, grid);
  dropPreviews(vm, player, grid);
  if (t.cellX != -1) gridUnselectArea(vm, grid, t.cellX, t.cellZ, 1, 1);
  gridEditEnd(vm, grid);
}

// ---------------------------------------------------------------------------------------------------------------
// GRIDMODE_REMOVE_OBJECTS: the hovered item (its root when it is a part) and its parts are the replaced set; roads,
// ramps and railway tracks are red in game (toolMode 1), so the script refuses them. onHoverDemolish(left button held)
// lets the script bulldoze while dragging.

std::unordered_map<const Object*, int> g_lastHover;  // +0x2a4: the item of the last pick

void moveRemove(VM& vm, ControlObj* player, GridObj* grid, float mx, float my) {
  ToolState& t = toolState(player);
  ToolPick p = toolPick(vm, player, mx, my, false);
  if (!p.hit) return;
  int& last = g_lastHover.try_emplace(player, -1).first->second;
  if (p.cellX == t.cellX && p.cellZ == t.cellZ && p.item == last) return;
  const GridData& g = *grid->data;
  gridEditBegin(vm, grid);
  if (t.cellX != -1) gridUnselectArea(vm, grid, t.cellX - 5, t.cellZ - 5, 10, 10);
  for (uint32_t r : t.replaced) gridSetSelected(vm, grid, r, false, false);  // port: parts far from the cursor too
  t.replaced.clear();
  int item = p.item;
  if (item >= 0) {
    while ((g.items[size_t(item)].flags & kItemAutoChild) && g.items[size_t(item)].parent)
      item = int(g.items[size_t(item)].parent) - 1;
    Info n = infoOf(vm, grid, uint32_t(item));
    t.replaced.insert(uint32_t(item));
    if (intField(vm, player, u"toolMode", 0) == 1 && n.type() > 1 && n.type() < 10 && !(n.flags & kFlagStop)) {
      gridSetSelected(vm, grid, uint32_t(item), true, true);
    } else {
      gridSetSelected(vm, grid, uint32_t(item), true, false);
      for (uint32_t o : gridGetObjects(vm, grid, n.x - 5, n.z - 5, n.sx + 10, n.sz + 10))
        if (g.items[o].parent == uint32_t(item) + 1 && (g.items[o].flags & kItemAutoChild)) t.replaced.insert(o);
    }
  }
  gridEditEnd(vm, grid);
  t.cellX = p.cellX;
  t.cellZ = p.cellZ;
  last = item;
  vm.fireEvent(player, u"onHoverDemolish", {Value::boolean((t.buttons & 1) != 0)});
}

// the direction bit of an item's own end (+0x44 orientation, +0x54 diagonals), for the slope correction
uint32_t endBit(const Info& n) {
  static const uint32_t straight[4] = {0x40, 0x10, 4, 1}, diagonal[4] = {0x20, 8, 2, 0x80};
  return n.diag ? diagonal[n.orient] : straight[n.orient];
}

// the height of a sloped piece at its far end (the joint is at its base height on its own end)
int slopedY(const Info& n, int y) {
  uint32_t f = n.flags;
  if (n.type() == kTypeAlley) return (f & 0x40) ? y + 0x40 : (f & 0x80) ? y + 0x80 : y;
  if ((n.type() >= 5 && n.type() <= 7) || (n.type() >= 0xb && n.type() <= 0xc && !n.diag))
    return (f & 0x10) ? y + 0x80 : (f & 0x20) ? y + 0x100 : y;
  return (f & 0x20) ? y + 0x80 : (f & 0x40) ? y + 0x100 : y;
}

// the first byte of a connection mask that holds the direction d, shifted there; 0 for none
uint32_t inMask(uint32_t d, uint32_t mask) {
  for (int b = 0; b < 32; b += 8)
    if ((d << b) & mask) return d << b;
  return 0;
}

bool removeObjectTool(VM& vm, ControlObj* player, GridObj* grid, uint32_t item, int depth = 0);

// gsGridPlayer::removeObject (L:0x08198b10). Roads and tracks next to the removed item that lead into it are replaced by
// pieces without that connection (dead ends); a neighbour that cannot be replaced is marked and removed afterwards. A
// road or track leading into a non-road item (a station) cannot be removed: the call fails. Returns false then.
bool removeObjectTool(VM& vm, ControlObj* player, GridObj* grid, uint32_t item, int depth) {
  ToolState& t = toolState(player);
  GridData& g = *grid->data;
  Info r = infoOf(vm, grid, item);
  if (!r.ok || depth > 32) return false;
  uint32_t rConn = r.conn;  // the removed item's connections, losing those that found their neighbour
  bool refused = false, cascade = false;
  bool rRoad = r.type() >= 1 && r.type() <= 15;
  auto [rbx, rbz] = roadXZ(r);
  t.newItems.clear();
  t.replaced.clear();
  gridEditBegin(vm, grid);
  // a replacement piece: kept when it is valid, else the neighbour is marked for removal
  auto keep = [&](int np, const Info& n) {
    if (np < 0) {
      addItemFlags(grid, n.index, kItemNegative);
      cascade = true;
      return;
    }
    GridData::Item& it = g.items[size_t(np)];
    it.flags |= kFlagJoin;
    if (!(it.flags & kItemNegative)) {
      t.replaced.insert(n.index);
      t.newItems.push_back(uint32_t(np));
    } else {
      addItemFlags(grid, n.index, kItemNegative);
      gridEditRemoveItem(vm, grid, uint32_t(np));
      cascade = true;
    }
  };
  auto replacement = [&](const Info& n, int bx, int bz, uint32_t conn, uint32_t flags) {
    int np = createRoadItem(vm, grid, bx, n.y, bz, conn, n.conn2, n.type(), n.tpl.variation, flags, n.parent);
    if (np >= 0) {
      g.items[size_t(np)].variation = g.items[n.index].variation;
      if (n.index < g.names.size() && !g.names[n.index].empty() && g.names[size_t(np)].empty())
        g.names[size_t(np)] = g.names[n.index];
    }
    return np;
  };
  for (uint32_t o : gridGetObjects(vm, grid, r.x - 5, r.z - 5, r.sx + 10, r.sz + 10)) {
    if (o == item || g.items[o].parent == item + 1) continue;
    Info n = infoOf(vm, grid, o);
    if (!n.ok) continue;
    bool nRoad = n.type() >= 1 && n.type() <= 15;
    if (!rRoad) {
      // a building, station or stop: the roads and tracks (2-15) that lead into it end there
      if (n.type() < 2 || n.type() > 15 || ((r.flags | n.flags) & kFlagStop)) continue;
      uint32_t d = 0;
      int nxe = n.x + n.sx, rxe = r.x + r.sx, nze = n.z + n.sz, rze = r.z + r.sz;
      bool sameY = n.y == r.y;
      if (nxe == r.x && n.z >= r.z && nze <= rze && sameY) d = 4;
      if (n.x == rxe && r.z <= n.z && nze <= rze && sameY) d = 0x40;
      if (r.z == nze && r.x <= n.x && nxe <= rxe && sameY) d = 0x10;
      if (n.z == rze && r.x <= n.x && nxe <= rxe && sameY) d = 1;
      uint32_t bit = inMask(d, n.conn);
      if (!bit || (n.flags & kItemNegative)) continue;
      auto [bx, bz] = roadXZ(n);
      int np = replacement(n, bx, bz, ~bit & n.conn, n.flags & 0xfff3ff0fu);
      if (np >= 0 && bit != endBit(n)) g.items[size_t(np)].y = int16_t(slopedY(n, g.items[size_t(np)].y));
      keep(np, n);
      continue;
    }
    if (!nRoad) {
      // a road or track leading into something else: not removable
      if (((r.flags | n.flags) & kFlagStop) || r.type() == kTypeAlley) continue;
      uint32_t d = 0;
      int nxe = n.x + n.sx, rxe = r.x + r.sx, nze = n.z + n.sz, rze = r.z + r.sz;
      bool sameY = n.y == r.y;
      if (rxe == n.x && n.z <= r.z && rze <= nze && sameY) d = 4;
      if (r.x == nxe && n.z <= r.z && rze <= nze && sameY) d = 0x40;
      if (n.z == rze && n.x <= r.x && rxe <= nxe && sameY) d = 0x10;
      if (nze == r.z && n.x <= r.x && rxe <= nxe && sameY) d = 1;
      if (!inMask(d, rConn) || (r.flags & kItemNegative)) continue;
      addItemFlags(grid, o, kItemNegative);
      refused = cascade = true;
      continue;
    }
    // two roads or tracks of one family
    auto tram = [](int ty) { return ty >= 10 && ty <= 12; };
    auto metro = [](int ty) { return ty >= 0xd && ty <= 0xf; };
    if (tram(r.type()) != tram(n.type()) || metro(r.type()) != metro(n.type()) ||
        (r.type() == kTypeAlley) != (n.type() == kTypeAlley) || ((r.flags | n.flags) & kFlagStop))
      continue;
    auto [nbx, nbz] = roadXZ(n);
    int block;
    if (tram(n.type())) {
      if (nbx == rbx && nbz == rbz && n.y == r.y) {
        addItemFlags(grid, o, kItemNegative);
        cascade = true;
      }
      block = 4;
    } else {
      block = n.type() != kTypeAlley ? 4 : 1;
    }
    // the direction from the removed piece to the neighbour
    uint32_t d = 0;
    if (n.x + n.sx == r.x && nbz == rbz) d = 0x40;
    else if (n.x == r.x + r.sx && nbz == rbz) d = 4;
    else if (nbx == rbx && n.z + n.sz == r.z) d = 1;
    else if (nbx == rbx && n.z == r.z + r.sz) d = 0x10;
    else if (rbx == nbx + block) d = rbz == nbz + block ? 0x80 : nbz == rbz + block ? 0x20 : 0;
    else if (nbx == rbx + block) d = nbz + block == rbz ? 2 : nbz == rbz + block ? 8 : 0;
    // large pieces (8 cells across) meet diagonals off their block grid
    if ((r.conn & 0xff00) == 0x4400 || (r.conn & 0xff000000u) == 0x44000000u) {
      if ((rConn & 8) && rbx + 4 == nbx && rbz + 8 == nbz) d = 8;
      if ((rConn & 0x20) && nbx + 4 == rbx && rbz + 8 == nbz) d = 0x20;
      if ((rConn & 0x80) && nbx + 4 == rbx && nbz + 8 == rbz) d = 0x80;
      if ((rConn & 2) && rbx + 4 == nbx && nbz + 8 == rbz) d = 2;
    }
    if ((r.conn & 0xff00) == 0x1100 || (r.conn & 0xff000000u) == 0x11000000u) {
      if ((rConn & 2) && rbx + 8 == nbx && nbz + 4 == rbz) d = 2;
      if ((rConn & 8) && rbx + 8 == nbx && rbz + 4 == nbz) d = 8;
      if ((rConn & 0x20) && nbx + 8 == rbx && rbz + 4 == nbz) d = 0x20;
      if ((rConn & 0x80) && nbx + 8 == rbx && nbz + 4 == rbz) d = 0x80;
    }
    uint32_t nc = n.conn;
    if ((nc & 0xff00) == 0x4400 || (nc & 0xff000000u) == 0x44000000u) {
      if ((nc & 0x80) && rbx + 4 == nbx && rbz + 8 == nbz) d = 8;
      if ((nc & 2) && nbx + 4 == rbx && rbz + 8 == nbz) d = 0x20;
      if ((nc & 8) && nbx + 4 == rbx && nbz + 8 == rbz) d = 0x80;
      if ((nc & 0x20) && rbx + 4 == nbx && nbz + 8 == rbz) d = 2;
    }
    if ((nc & 0xff00) == 0x1100 || (nc & 0xff000000u) == 0x11000000u) {
      if ((nc & 0x20) && rbx + 8 == nbx && nbz + 4 == rbz) d = 2;
      if ((nc & 0x80) && rbx + 8 == nbx && rbz + 4 == nbz) d = 8;
      if ((nc & 2) && nbx + 8 == rbx && rbz + 4 == nbz) d = 0x20;
      if ((nc & 8) && nbx + 8 == rbx && nbz + 4 == rbz) d = 0x80;
    }
    // the joint heights must match (a sloped piece rises towards its far end)
    int rJoint = d != endBit(r) ? slopedY(r, r.y) : r.y;
    int nJoint = rotateConnections(d, 4) != endBit(n) ? slopedY(n, n.y) : n.y;
    if (rJoint != nJoint) continue;
    uint32_t bit = inMask(d, rConn);
    if (!bit) continue;
    uint32_t back = rotateConnections(bit, 4);
    if ((n.flags & kItemNegative) || !(n.conn & back)) continue;
    rConn &= ~bit;
    uint32_t flags = n.flags & 0xfff3ffffu;
    if (!(n.flags & (kFlagBridge | kFlagTunnel))) flags = n.flags & 0xfff3ff0fu;
    int np = replacement(n, nbx, nbz, ~back & n.conn, flags);
    if (np >= 0) {
      GridData::Item& it = g.items[size_t(np)];
      auto bridgeOrTunnel = [&](int p) {
        if (p < 0) return false;
        uint32_t pf = gridItemShape(vm, grid, uint32_t(p)).flags;
        return (pf & (kFlagBridge | kFlagTunnel)) && !(pf & kFlagCover);
      };
      if (bridgeOrTunnel(n.parent) || bridgeOrTunnel(r.parent)) it.flags |= kItemNegative;
      if (n.parent >= 0) {
        uint32_t pf = gridItemShape(vm, grid, uint32_t(n.parent)).flags;
        uint32_t f = gridItemShape(vm, grid, uint32_t(np)).flags;
        if (!(pf & (kFlagBridge | kFlagTunnel)) && ((pf ^ f) & 0xf0)) it.flags |= kItemNegative;
      }
      if (d != endBit(n)) it.y = int16_t(slopedY(n, it.y));
    }
    keep(np, n);
  }
  if (refused) {
    for (uint32_t o : gridGetObjects(vm, grid, r.x - 5, r.z - 5, r.sx + 10, r.sz + 10))
      g.items[o].flags &= ~kItemNegative;
    for (auto i = t.newItems.rbegin(); i != t.newItems.rend(); ++i) gridEditRemoveItem(vm, grid, *i);
    t.newItems.clear();
    t.replaced.clear();
    gridEditEnd(vm, grid);
    return false;
  }
  // the editor records the removal: the item, the neighbours cut back and their new ends (removeObject L:0x08198b10)
  Object* tl = playerTimeline(vm, player);
  if (tl && depth == 0) timelineBeginGroup(vm, tl, "%0");
  gridSetSelected(vm, grid, item, false, false);
  timelineRecordDelete(vm, tl, grid, item);
  gridEditRemoveItem(vm, grid, item);  // with its parts (gsGrid::removeObject)
  for (uint32_t o : t.replaced) {
    gridSetSelected(vm, grid, o, false, false);
    timelineRecordDelete(vm, tl, grid, o);
    gridEditRemoveItem(vm, grid, o);
  }
  for (size_t k = 0; k < t.newItems.size(); ++k) placeObject(vm, player, grid, t.newItems[k]);
  grid->mountedCopies.clear();
  std::vector<uint32_t> mounted;
  for (uint32_t i : t.newItems) {
    int m = gridMountObject(vm, grid, i);
    if (m >= 0) mounted.push_back(uint32_t(m));
    if (m >= 0) timelineRecordCreate(vm, tl, grid, uint32_t(m));
  }
  for (auto i = t.newItems.rbegin(); i != t.newItems.rend(); ++i) gridEditRemoveItem(vm, grid, *i);
  grid->mountedCopies.clear();
  t.newItems.clear();
  t.replaced.clear();
  // the neighbours that could not be cut go too
  while (cascade) {
    int next = -1;
    for (uint32_t o : gridGetObjects(vm, grid, r.x - 5, r.z - 5, r.sx + 10, r.sz + 10))
      if (g.items[o].flags & kItemNegative) {
        next = int(o);
        break;
      }
    if (next < 0) break;
    if (!removeObjectTool(vm, player, grid, uint32_t(next), depth + 1)) g.items[size_t(next)].flags &= ~kItemNegative;
  }
  if (tl && depth == 0) timelineEndGroup(vm, tl);
  gridEditEnd(vm, grid);
  fireInstanceEvents(vm, grid, mounted);
  return true;
}

// finishTool case 8 (L:0x0819a3e0.c:512-552): the first replaced item that is not a part and not red is removed
bool finishRemove(VM& vm, ControlObj* player, GridObj* grid) {
  ToolState& t = toolState(player);
  g_lastHover[player] = -1;
  if (!grid) return true;
  const GridData& g = *grid->data;
  bool ok = true;
  gridEditBegin(vm, grid);
  std::set<uint32_t> list = t.replaced;
  for (uint32_t r : list) {
    if (r >= g.items.size() || g.items[r].removed) continue;
    uint32_t f = gridItemShape(vm, grid, r).flags;
    if ((!g.items[r].parent || !(f & kItemAutoChild)) && !(f & kItemNegative)) {
      t.replaced.clear();
      ok = removeObjectTool(vm, player, grid, r);
      break;
    }
    gridSetSelected(vm, grid, r, false, false);
  }
  for (uint32_t r : list)
    if (r < g.items.size() && !g.items[r].removed) gridSetSelected(vm, grid, r, false, false);
  t.replaced.clear();
  if (t.cellX != -1) gridUnselectArea(vm, grid, t.cellX - 5, t.cellZ - 5, 10, 10);
  gridEditEnd(vm, grid);
  return ok;
}

// cancelTool case 8: only the replaced set is emptied (finishTool then clears the highlight around the cursor)
void cancelRemove(VM& vm, ControlObj* player, GridObj* grid) {
  ToolState& t = toolState(player);
  if (grid)
    for (uint32_t r : t.replaced)
      if (r < grid->data->items.size()) gridSetSelected(vm, grid, r, false, false);
  t.replaced.clear();
}

// ---------------------------------------------------------------------------------------------------------------
// natives

// whether a template names its stops' neighbourhood (gsGridObject +0x370: set by addHomePlaces with more than 9
// tourist homes, addWorkPlaces with more than 19 work places, addVisitPlaces with more than 19 shopping, leisure and
// government places)
bool landmark(Object* t) {
  auto* o = dynamic_cast<ObjectObj*>(t);
  if (!o) return false;
  int work = 0;
  for (int w : o->workPlaces) work += w;
  return o->homePlaces[4] > 9 || work > 19 || o->visitPlaces[3] + o->visitPlaces[4] + o->visitPlaces[5] > 19;
}

void registerNatives(VM& vm) {
  // gsGridItemController.getConnections (W:0x100e94a0): the item's connection mask as turned with it (+0x4c), 0
  // without an item; ui.script points a preview stop's direction arrow with it
  implement(vm, "gsGridItemController", u"getConnections", [](VM& v, Object* s, int, const Value*) {
    auto it = s->fieldValues.find(u"<item>");
    auto* grid = dynamic_cast<GridObj*>(s->parent);
    if (it == s->fieldValues.end() || !grid || !grid->data) return Value::integer(0);
    Info n = infoOf(v, grid, uint32_t(v.toInt(it->second)));
    return Value::integer(n.ok ? int32_t(n.conn) : 0);
  });
  // getAverageY(x, z, w, l) (0x100b5c30, handlers/batch-A.md): the mid-range (min + max) / 512 of the heights over
  // [x, x+w] × [z, z+l], clamped to the map
  implement(vm, "gsGrid", u"getAverageY", [](VM& v, Object* s, int, const Value* a) {
    auto* grid = dynamic_cast<GridObj*>(s);
    if (!grid || !grid->data) return Value::real(0);
    const GridData& g = *grid->data;
    int x = int(v.toInt(a[0])), z = int(v.toInt(a[1])), w = int(v.toInt(a[2])), l = int(v.toInt(a[3]));
    int x0 = std::max(0, x), z0 = std::max(0, z), x1 = std::min(x + w, g.width - 1), z1 = std::min(z + l, g.length - 1);
    if (x1 < x0 || z1 < z0) return Value::real(0);
    int lo = 0x7fffffff, hi = -0x7fffffff;
    for (int cz = z0; cz <= z1; ++cz)
      for (int cx = x0; cx <= x1; ++cx) {
        int h = g.height[size_t(cz) * size_t(g.width) + size_t(cx)];
        lo = std::min(lo, h), hi = std::max(hi, h);
      }
    return Value::real(float(lo + hi) * 0.001953125f);
  });
  // findNames(pos, station) (L:0x0814aef0): the names of the named items and landmark buildings within 150 units, the
  // nearest first by distance over the item's size (markers count for stations, buildings for stops); stops excluded
  implement(vm, "gsGrid", u"findNames", [](VM& v, Object* s, int argc, const Value* a) {
    auto* grid = dynamic_cast<GridObj*>(s);
    Ref<ArrayObj> out = make<ArrayObj>();
    Value result = Value::heap(VT::Array, out.get());
    if (!grid || !grid->data || argc < 2) return result;
    Value pv = v.resolve(a[0]);
    if (pv.type() != VT::Vector) return result;
    bool station = v.truthy(v.resolve(a[1]));
    const GridData& g = *grid->data;
    float cs = g.cellSize, px = pv.vec()[0], py = pv.vec()[1], pz = pv.vec()[2];
    int x0 = int(std::nearbyint((px - 150.0f + float(g.width) * cs * 0.5f) / cs));
    int x1 = int(std::nearbyint((px + 150.0f + float(g.width) * cs * 0.5f) / cs));
    int z0 = int(std::nearbyint((pz - 150.0f + float(g.length) * cs * 0.5f) / cs));
    int z1 = int(std::nearbyint((pz + 150.0f + float(g.length) * cs * 0.5f) / cs));
    std::map<std::string, float> best;
    auto offer = [&](const std::string& name, float score) {
      auto it = best.find(name);
      if (it == best.end()) best.emplace(name, score);
      else it->second = std::min(it->second, score);
    };
    for (uint32_t i : gridItemsNear(v, grid, x0, z0, x1, z1)) {
      const GridData::Item& it = g.items[i];
      GridItemShape sh = gridItemShape(v, grid, i);
      if (sh.flags & (kFlagStop | kItemTemp)) continue;
      auto t = grid->templates.find(g.types[it.type]);
      Object* tpl = t == grid->templates.end() ? nullptr : t->second.get();
      const std::string& own = i < g.names.size() ? g.names[i] : std::string();
      std::string display;
      if (tpl && landmark(tpl))
        if (auto dn = tpl->fieldValues.find(u"displayName"); dn != tpl->fieldValues.end() && !dn->second.isNone())
          display = utf16ToUtf8(v.toStr(dn->second));
      if (own.empty() && display.empty()) continue;
      float r = sh.type == kTypeMarker ? float(sh.sx) * cs * 1.66667f : float(std::max(20, sh.sx + sh.sz)) * cs * 0.5f;
      float cx = float(sh.sx + it.x * 2 - g.width) * cs * 0.5f, cy = float(it.y) * cs * 0.00390625f,
            cz = float(sh.sz + it.z * 2 - g.length) * cs * 0.5f;
      float d = ((cx - px) * (cx - px) + (cy - py) * (cy - py) + (cz - pz) * (cz - pz)) / (r * r);
      if (d >= 4.0f) continue;
      if ((sh.type == kTypeMarker) != station) d += 4.0f;
      if (!own.empty()) offer(own, d - 1.0f);
      if (!display.empty()) offer(display, d);
    }
    std::multimap<float, std::string> order;
    for (auto& [name, score] : best) order.emplace(score, name);
    for (auto& [score, name] : order) out->items.push_back(Value::string(utf8ToUtf16(name)));
    return result;
  });
}

}  // namespace

void registerObjectTools(VM& vm) {
  ToolMode stop;
  stop.move = moveCreateStop;
  stop.finish = finishPlace;
  stop.cancel = cancelPlace;
  registerToolMode(kGridModeCreateStop, stop);
  ToolMode place;
  place.move = movePlaceObject;
  place.finish = finishPlace;
  place.cancel = cancelPlace;
  registerToolMode(kGridModePlaceObject, place);
  ToolMode remove;
  remove.move = moveRemove;
  remove.finish = finishRemove;
  remove.cancel = cancelRemove;
  registerToolMode(kGridModeRemoveObjects, remove);
  registerNatives(vm);
}

}  // namespace regs::engine
