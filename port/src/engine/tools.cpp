// The grid player's editing tools: shared state, per-mode dispatch and the helpers the modes build on (tools.h,
// docs/ui/construction.md). The select mode registers itself from interaction.cpp; the other modes from their own
// files.
#include "engine/tools.h"

#include <algorithm>
#include <map>
#include <unordered_map>

#include "core/log.h"
#include "engine/api.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/grid_render.h"
#include "engine/gui.h"
#include "engine/interaction.h"

namespace regs::engine {

using namespace regs::script;

namespace {

constexpr uint32_t kFlagCover = 0x4, kFlagStop = 0x2000, kFlagBridge = 0x4000, kFlagTunnel = 0x8000;
constexpr int kTypeMetroStation = 0x10, kTypeMarker = 0x15;
constexpr uint16_t kCellWater = 0x2000, kCellHighlight = 0x4000, kCellNegative = 0x8000;

std::unordered_map<const Object*, ToolState> g_tools;
std::map<int, ToolMode> g_modes;

Value obj(Object* o) { return o ? Value::heap(VT::Object, o) : Value::null(); }

Value member(VM& vm, Object* o, const char16_t* name) {
  try {
    return vm.resolve(vm.getMember(Value::heap(VT::Object, o), name));
  } catch (const ScriptError&) {
    return Value();
  }
}

int intField(VM& vm, const Object* o, const char16_t* name, int def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : int(vm.toInt(it->second));
}

int editMode(VM& vm, Object* p) { return intField(vm, p, u"editMode", kGridModeSelectObject); }

Value newArray(const std::vector<Value>& items) {
  Ref<ArrayObj> a = make<ArrayObj>();
  a->items = items;
  return Value::heap(VT::Array, a.get());
}

// the template object of an item (gsGridObject), for getNewItems / getReplacedItems and the instance events
Object* itemTemplate(GridObj* grid, uint32_t item) {
  const GridData& g = *grid->data;
  if (item >= g.items.size()) return nullptr;
  auto t = grid->templates.find(g.types[g.items[item].type]);
  return t == grid->templates.end() ? nullptr : t->second.get();
}

// the template's second connection mask (multi-block pieces: bridges, ramps, map-edge exits), turned with the item
// (gsGridItem +0x50: gsRotateConnections(connections2, orient·2)); gsGrid::checkConflict's join test reads this one
uint32_t itemConnections2(VM& vm, GridObj* grid, uint32_t item) {
  Object* t = itemTemplate(grid, item);
  if (!t) return 0;
  return rotateConnections(uint32_t(intField(vm, t, u"connections2", 0)), (grid->data->items[item].orient & 3) * 2);
}

// an item's base height in the engine's units (cell heights carry the same shift)
int engineY(const GridData& g, const GridData::Item& it) { return it.y + (0x200 - int(g.heightBase)); }

// the footprint, height range and cover of an item (gsGridItem +0x2c..+0x54)
struct Geo {
  int x = 0, z = 0, sx = 1, sz = 1, y = 0, top = 0, bottom = 0;
  uint32_t flags = 0;
  ItemFootprint f;
};
Geo geoOf(VM& vm, GridObj* grid, uint32_t item) {
  const GridData& g = *grid->data;
  const GridData::Item& it = g.items[item];
  Object* t = itemTemplate(grid, item);
  GridItemShape s = gridItemShape(vm, grid, item);
  Geo o;
  int w = t ? std::max(1, intField(vm, t, u"width", 1)) : 1, l = t ? std::max(1, intField(vm, t, u"length", 1)) : 1;
  o.f = itemFootprint(w, l, t ? uint32_t(intField(vm, t, u"diagonals", 0)) : 0, it.orient);
  o.x = it.x, o.z = it.z, o.sx = o.f.sx, o.sz = o.f.sz;
  o.y = engineY(g, it);
  o.top = o.y + s.height;
  o.bottom = o.y - (t ? intField(vm, t, u"negHeight", 0) : 0) * 0x100;
  o.flags = s.flags;
  return o;
}
bool geoCovers(const Geo& a, int cx, int cz) { return !a.f.diag || itemCellCover(a.f, a.flags, cx - a.x, cz - a.z) != 0; }
bool geoOverlapsRect(const Geo& a, int x, int z, int w, int l) {
  if (!(x < a.x + a.sx && z < a.z + a.sz && a.x < x + w && a.z < z + l)) return false;
  if (!a.f.diag) return true;
  for (int cz = std::max(z, a.z); cz < std::min(z + l, a.z + a.sz); ++cz)
    for (int cx = std::max(x, a.x); cx < std::min(x + w, a.x + a.sx); ++cx)
      if (geoCovers(a, cx, cz)) return true;
  return false;
}

bool isAncestorIn(const GridData& g, uint32_t item, const std::set<uint32_t>& set) {
  for (int depth = 0; depth < 64 && item < g.items.size(); ++depth) {
    uint32_t p = g.items[item].parent;
    if (!p) return false;
    item = p - 1;
    if (set.count(item)) return true;
  }
  return false;
}

void setItemFlags(GridObj* grid, uint32_t item, uint32_t flags) {
  GridData::Item& it = grid->data->items[item];
  if (it.flags == flags) return;
  it.flags = flags;
  gridEditItemChanged(grid, item);
}

// the template events of mounted items run after their batch: their controllers and the map are complete by then
void fireInstanceEvents(VM& vm, GridObj* grid, const std::vector<uint32_t>& items) {
  for (uint32_t i : items) {
    if (i >= grid->data->items.size() || grid->data->items[i].removed) continue;
    if (grid->data->items[i].flags & (kItemTemp | kItemHidden)) continue;
    Object* t = itemTemplate(grid, i);
    Object* c = gridItemController(vm, grid, i);
    if (!t || !c) continue;
    try {
      vm.fireEvent(t, u"onCreateInstance", {obj(c)});
    } catch (const std::exception& e) {
      LOG_ERROR("onCreateInstance: %s", e.what());
    }
  }
}

}  // namespace

ToolState& toolState(Object* player) { return g_tools[player]; }

ToolBlocks& toolBlocks() {
  static ToolBlocks blocks;
  return blocks;
}

void registerToolMode(int mode, ToolMode m) { g_modes[mode] = std::move(m); }

GridObj* toolGrid(VM& vm, Object* player) {
  auto* g = dynamic_cast<GridObj*>(member(vm, player, u"grid").object());
  return g && g->data ? g : nullptr;
}

uint32_t rotateConnections(uint32_t c, int n) {
  static const uint32_t mask[16] = {0xffffffff, 0x7f7f7f7f, 0x3f3f3f3f, 0x1f1f1f1f, 0x0f0f0f0f, 0x07070707,
                                    0x03030303, 0x01010101, 0x00000000, 0x80808080, 0xc0c0c0c0, 0xe0e0e0e0,
                                    0xf0f0f0f0, 0xf8f8f8f8, 0xfcfcfcfc, 0xfefefefe};
  n &= 7;
  return ((n ? c << (8 - n) : 0) & mask[n + 8]) | (c >> n & mask[n]);
}

// ---------------------------------------------------------------------------------------------------------------
// highlights

void gridSetSelected(VM& vm, GridObj* grid, uint32_t item, bool selected, bool negative) {
  if (!grid || !grid->data || item >= grid->data->items.size()) return;
  (void)vm;
  GridData::Item& it = grid->data->items[item];
  uint32_t f = it.flags & ~(kItemSelected | kItemNegative);
  if (selected) f |= kItemSelected | (negative ? kItemNegative : 0);
  if (f == it.flags) return;
  it.flags = f;  // the highlight is an overlay: the division mesh stays as it is
  if (selected) grid->highlightItems.insert(item);
  else grid->highlightItems.erase(item);
  ++grid->highlightVersion;
}

void gridSelectArea(VM& vm, GridObj* grid, int x, int z, int w, int l, bool negative) {
  if (!grid || !grid->data) return;
  GridData& g = *grid->data;
  for (int cz = z; cz < z + l; ++cz)
    for (int cx = x; cx < x + w; ++cx) {
      if (cx < 0 || cz < 0 || cx >= g.width || cz >= g.length) continue;
      size_t c = size_t(cz) * size_t(g.width) + size_t(cx);
      g.flags[c] = uint16_t(g.flags[c] | kCellHighlight | (negative ? kCellNegative : 0));
      grid->highlightCells.insert(uint32_t(c));
    }
  // the items overlapping the area (gsGrid::getObjects; the port keeps previews in the search too)
  for (uint32_t i : gridItemsNear(vm, grid, x, z, x + w - 1, z + l - 1))
    if (geoOverlapsRect(geoOf(vm, grid, i), x, z, w, l)) gridSetSelected(vm, grid, i, true, negative);
  ++grid->highlightVersion;
}

void gridUnselectArea(VM& vm, GridObj* grid, int x, int z, int w, int l) {
  if (!grid || !grid->data) return;
  GridData& g = *grid->data;
  for (int cz = z; cz < z + l; ++cz)
    for (int cx = x; cx < x + w; ++cx) {
      if (cx < 0 || cz < 0 || cx >= g.width || cz >= g.length) continue;
      size_t c = size_t(cz) * size_t(g.width) + size_t(cx);
      g.flags[c] &= uint16_t(~(kCellHighlight | kCellNegative));  // the engine keeps the low 14 bits (& 0x3fff)
      grid->highlightCells.erase(uint32_t(c));
    }
  for (uint32_t i : gridItemsNear(vm, grid, x, z, x + w - 1, z + l - 1))
    if (g.items[i].flags & kItemSelected)
      if (geoOverlapsRect(geoOf(vm, grid, i), x, z, w, l)) gridSetSelected(vm, grid, i, false, false);
  ++grid->highlightVersion;
}

std::vector<uint32_t> gridGetObjects(VM& vm, GridObj* grid, int x, int z, int w, int l) {
  std::vector<uint32_t> out;
  if (!grid || !grid->data) return out;
  for (uint32_t i : gridItemsNear(vm, grid, x, z, x + w - 1, z + l - 1)) {
    const GridData::Item& it = grid->data->items[i];
    if (it.removed || it.x >= x + w || it.z >= z + l || it.x + 64 <= x || it.z + 64 <= z) continue;  // cheap reject
    Geo a = geoOf(vm, grid, i);
    if (!(a.flags & kItemTemp) && geoOverlapsRect(a, x, z, w, l)) out.push_back(i);
  }
  return out;
}

bool gridItemsOverlap(VM& vm, GridObj* grid, uint32_t ia, uint32_t ib) {
  Geo a = geoOf(vm, grid, ia), b = geoOf(vm, grid, ib);
  if (!(b.x < a.x + a.sx && b.z < a.z + a.sz && a.x < b.x + b.sx && a.z < b.z + b.sz)) return false;
  if (!(b.bottom < a.top && a.bottom < b.top)) return false;
  if (!a.f.diag && !b.f.diag) return true;
  for (int cz = std::max(a.z, b.z); cz < std::min(a.z + a.sz, b.z + b.sz); ++cz)
    for (int cx = std::max(a.x, b.x); cx < std::min(a.x + a.sx, b.x + b.sx); ++cx)
      if (geoCovers(a, cx, cz) && geoCovers(b, cx, cz)) return true;
  return false;
}

void gridItemRect(VM& vm, GridObj* grid, uint32_t item, int& x, int& z, int& w, int& l) {
  Geo a = geoOf(vm, grid, item);
  x = a.x, z = a.z, w = a.sx, l = a.sz;
}

void gridSelectObjectArea(VM& vm, GridObj* grid, uint32_t item, bool negative) {
  if (!grid || !grid->data || item >= grid->data->items.size()) return;
  GridData& g = *grid->data;
  Geo a = geoOf(vm, grid, item);
  for (int cz = a.z; cz < a.z + a.sz; ++cz)
    for (int cx = a.x; cx < a.x + a.sx; ++cx) {
      if (cx < 0 || cz < 0 || cx >= g.width || cz >= g.length || !geoCovers(a, cx, cz)) continue;
      size_t c = size_t(cz) * size_t(g.width) + size_t(cx);
      g.flags[c] = uint16_t(g.flags[c] | kCellHighlight | (negative ? kCellNegative : 0));
      grid->highlightCells.insert(uint32_t(c));
    }
  gridSetSelected(vm, grid, item, true, negative);
  for (uint32_t o : gridGetObjects(vm, grid, a.x, a.z, a.sx, a.sz))
    if (o != item && gridItemsOverlap(vm, grid, o, item)) gridSetSelected(vm, grid, o, true, negative);
  ++grid->highlightVersion;
}

// ---------------------------------------------------------------------------------------------------------------
// conflicts and mounting

bool gridCheckConflict(VM& vm, GridObj* grid, uint32_t item, const std::set<uint32_t>& exclude) {
  GridData& g = *grid->data;
  if (item >= g.items.size() || g.items[item].removed) return true;
  const GridData::Item& it = g.items[item];
  GridItemShape s = gridItemShape(vm, grid, item);
  uint32_t flags = s.flags;
  if (flags & kItemNegative) return true;
  int x = it.x, z = it.z, w = s.sx, l = s.sz, y = engineY(g, it);
  if (x < 0 || z < 0 || x + w > g.width || z + l > g.length) return true;
  auto cell = [&](int cx, int cz) { return size_t(cz) * size_t(g.width) + size_t(cx); };
  auto dryCells = [&](long* heightSum) {
    int dry = 0;
    for (int cz = z; cz < z + l; ++cz)
      for (int cx = x; cx < x + w; ++cx) {
        if (!(g.flags[cell(cx, cz)] & kCellWater)) ++dry;
        if (heightSum) *heightSum += g.height[cell(cx, cz)] >> 7;
      }
    return dry;
  };
  // the terrain rules of the type (L:0x0813f5e0.c:77-166): stations and covering buildings of types 0x11-0x14 need
  // at least half of their cells dry; a dock-like stop of type 0x11 (STOP, not covering) at most a quarter; tram
  // tracks (types 10-13) laid on their own at least half dry and within 5 height steps (·128) of the ground's mean
  bool checkTerrain = s.type == kTypeMetroStation || (s.type >= 0x11 && s.type <= 0x14 && (flags & kFlagCover));
  if (checkTerrain) {
    if (dryCells(nullptr) < (w * l) >> 1) return true;
  } else if (s.type == 0x11 && (flags & kFlagStop)) {
    if (dryCells(nullptr) > (w * l) >> 2) return true;
  } else if (s.type >= 10 && s.type <= 13 && !it.parent) {
    long sum = 0;
    if (dryCells(&sum) < (w * l) >> 1) return true;
    if (std::abs(int(sum / (w * l)) - (y >> 7)) > 5) return true;
  }
  // tunnels below the ground everywhere, bridges above it but not more than 10 cells (L:0x0813f5e0.c:168-215)
  if ((flags & (kFlagTunnel | kFlagCover)) == kFlagTunnel) {
    for (int cz = z; cz < z + l; ++cz)
      for (int cx = x; cx < x + w; ++cx)
        if (g.height[cell(cx, cz)] < y + s.height) return true;
  } else if ((flags & (kFlagBridge | kFlagCover)) == kFlagBridge) {
    for (int cz = z; cz < z + l; ++cz)
      for (int cx = x; cx < x + w; ++cx) {
        int h = g.height[cell(cx, cz)];
        if (h > y || h < y - 0xa00) return true;
      }
  }
  // the items around (L:0x0813f5e0.c:216-367). An item with flag 2 and connections2 must meet a neighbour along one
  // of its four edges at its own height (n/e/s/w bits 1, 4, 0x10, 0x40), else it conflicts.
  uint32_t conn = itemConnections2(vm, grid, item);  // +0x50
  bool needsJoin = conn != 0 && (flags & 2);
  for (uint32_t o : gridItemsNear(vm, grid, x, z, x + w - 1, z + l - 1)) {
    if (o == item || g.items[o].removed || exclude.count(o) || isAncestorIn(g, o, exclude)) continue;
    const GridData::Item& ot = g.items[o];
    if (needsJoin) {
      uint32_t oc = itemConnections2(vm, grid, o);
      if (oc) {
        GridItemShape os = gridItemShape(vm, grid, o);
        int oy = engineY(g, ot);
        bool same = y == oy;
        if ((conn & 1) && (oc & 0x10) && x == ot.x && z == ot.z + os.sz && same) needsJoin = false;
        if ((conn & 4) && (oc & 0x40) && z == ot.z && x + w == ot.x && same) needsJoin = false;
        if ((conn & 0x10) && (oc & 1) && x == ot.x && z + l == ot.z && same) needsJoin = false;
        if ((conn & 0x40) && (oc & 4) && z == ot.z && x == ot.x + os.sx && same) needsJoin = false;
      }
    }
    uint32_t of = gridItemShape(vm, grid, o).flags;
    if ((g.items[item].flags & kItemHidden) || (of & (kItemTemp | kItemHidden)) == kItemHidden) continue;
    if (!gridItemsConflict(vm, grid, o, item)) continue;
    // an optional item gives way: the other one when this is required (or both are optional previews), else this one
    if (!(flags & kItemOptional) || (of & (kItemTemp | kItemOptional)) == (kItemTemp | kItemOptional)) {
      if (!(of & kItemOptional)) return true;
      setItemFlags(grid, o, g.items[o].flags | kItemHidden);
      GridItemShape os = gridItemShape(vm, grid, o);
      gridEditCellsChanged(grid, ot.x, ot.z, ot.x + os.sx - 1, ot.z + os.sz - 1, false);
    } else {
      setItemFlags(grid, item, g.items[item].flags | kItemHidden);
      gridEditCellsChanged(grid, x, z, x + w - 1, z + l - 1, false);
    }
  }
  return needsJoin;
}

int gridMountObject(VM& vm, GridObj* grid, uint32_t temp) {
  GridData& g = *grid->data;
  if (temp >= g.items.size() || g.items[temp].removed) return -1;
  GridData::Item it = g.items[temp];
  std::string name = temp < g.names.size() ? g.names[temp] : std::string();
  // a child of a mounted preview hangs on that preview's mounted copy (gsGridItem +0x20)
  int parent = it.parent ? int(it.parent) - 1 : -1;
  if (parent >= 0) {
    auto m = grid->mountedCopies.find(uint32_t(parent));
    if (m != grid->mountedCopies.end()) parent = int(m->second);
  }
  uint32_t flags = it.flags & ~(kItemTemp | kItemSelected | kItemNegative | 2u);  // the copy ctor clears 0x100302
  int copy = gridEditAddItem(vm, grid, g.types[it.type], it.x, engineY(g, it), it.z, it.orient, flags, parent, true);
  if (copy < 0) return -1;
  g.items[size_t(copy)].variation = it.variation;
  if (!name.empty()) {
    if (g.names.size() <= size_t(copy)) g.names.resize(size_t(copy) + 1);
    g.names[size_t(copy)] = name;
  }
  grid->mountedCopies[temp] = uint32_t(copy);
  // the engine checks the new item once more: optional items that still conflict are hidden
  gridCheckConflict(vm, grid, uint32_t(copy), {});
  return copy;
}

// ---------------------------------------------------------------------------------------------------------------
// previews

int toolAddPreview(VM& vm, Object* player, GridObj* grid, const std::string& tpl, int x, int y, int z, int orient,
                   uint32_t flags, int parent) {
  int i = gridEditAddItem(vm, grid, tpl, x, y, z, orient, flags | kItemTemp, parent, false);
  if (i >= 0) toolState(player).newItems.push_back(uint32_t(i));
  return i;
}

void toolReplace(VM& vm, Object* player, GridObj* grid, uint32_t item, bool negative) {
  toolState(player).replaced.insert(item);
  gridSetSelected(vm, grid, item, true, negative);
}

void toolClearPreview(VM& vm, Object* player, GridObj* grid) {
  ToolState& t = toolState(player);
  if (!grid) {
    t.newItems.clear();
    t.replaced.clear();
    return;
  }
  gridEditBegin(vm, grid);
  for (auto i = t.newItems.rbegin(); i != t.newItems.rend(); ++i) {
    gridSetSelected(vm, grid, *i, false, false);
    gridEditRemoveItem(vm, grid, *i);
  }
  for (uint32_t r : t.replaced) gridSetSelected(vm, grid, r, false, false);
  t.newItems.clear();
  t.replaced.clear();
  gridEditEnd(vm, grid);
}

bool toolCommit(VM& vm, Object* player, GridObj* grid,
                const std::function<void(uint32_t preview, uint32_t mounted)>& onMounted) {
  ToolState& t = toolState(player);
  if (!grid) return true;
  GridData& g = *grid->data;
  gridEditBegin(vm, grid);
  bool conflict = false;
  for (size_t k = 0; k < t.newItems.size(); ++k) {
    if (gridCheckConflict(vm, grid, t.newItems[k], t.replaced)) conflict = true;
    if (k > 0 && gridItemsConflict(vm, grid, t.newItems[k - 1], t.newItems[k])) conflict = true;
  }
  std::vector<uint32_t> mounted;
  if (!conflict)
    for (uint32_t r : t.replaced) {
      gridSetSelected(vm, grid, r, false, false);
      gridEditRemoveItem(vm, grid, r);
    }
  grid->mountedCopies.clear();
  for (uint32_t i : t.newItems) {
    int ax, az, aw, al;  // unselectArea over the preview: its cells and the items under it
    gridItemRect(vm, grid, i, ax, az, aw, al);
    gridUnselectArea(vm, grid, ax, az, aw, al);
    gridSetSelected(vm, grid, i, false, false);
    if (!conflict) {
      int m = gridMountObject(vm, grid, i);
      if (m >= 0) mounted.push_back(uint32_t(m));
      if (m >= 0 && onMounted) onMounted(i, uint32_t(m));
    }
  }
  for (auto i = t.newItems.rbegin(); i != t.newItems.rend(); ++i) gridEditRemoveItem(vm, grid, *i);
  grid->mountedCopies.clear();
  if (conflict)
    for (uint32_t r : t.replaced) gridSetSelected(vm, grid, r, false, false);
  t.newItems.clear();
  t.replaced.clear();
  gridEditEnd(vm, grid);
  (void)g;
  fireInstanceEvents(vm, grid, mounted);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// the framework

void toolMouseMove(VM& vm, ControlObj* player, float x, float y, int buttons) {
  ToolState& t = toolState(player);
  if (t.middle) return;  // +0x25a: the camera drags
  t.buttons = buttons;
  // debugging: REGS_DEBUG_HIGHLIGHT=1 highlights 5×5 cells under the cursor (green) and the item there (red)
  static const bool debugHighlight = std::getenv("REGS_DEBUG_HIGHLIGHT") != nullptr;
  if (debugHighlight)
    if (GridObj* grid = toolGrid(vm, player)) {
      static int lastX = -100, lastZ = -100, lastItem = -1;
      ToolPick p = toolPick(vm, player, x, y, false);
      if (p.hit && (p.cellX != lastX || p.cellZ != lastZ || p.item != lastItem)) {
        gridUnselectArea(vm, grid, lastX - 2, lastZ - 2, 5, 5);
        if (lastItem >= 0) gridSetSelected(vm, grid, uint32_t(lastItem), false, false);
        gridSelectArea(vm, grid, p.cellX - 2, p.cellZ - 2, 5, 5, false);
        if (p.item >= 0) gridSetSelected(vm, grid, uint32_t(p.item), true, true);
        lastX = p.cellX, lastZ = p.cellZ, lastItem = p.item;
      }
    }
  auto m = g_modes.find(editMode(vm, player));
  if (m == g_modes.end() || !m->second.move) return;
  GridObj* grid = toolGrid(vm, player);
  if (!grid) return;
  m->second.move(vm, player, grid, x, y);
}

// gsGridPlayer::mouseDown (L:0x0819c410, selection.md §4.2)
void toolMouseDown(VM& vm, ControlObj* player, float x, float y, int key) {
  ToolState& t = toolState(player);
  int mode = editMode(vm, player);
  GridObj* grid = toolGrid(vm, player);
  if (key == kMouseLeft) {
    if (!t.inProgress) {
      t.toolX = x, t.toolY = y;
      if (mode != kGridModeRemoveObjects) t.inProgress = true;
      t.startX = t.cellX, t.startY = t.cellY, t.startZ = t.cellZ, t.startW = t.cellW;
      t.cellX = t.cellZ = -1;
      player->mouseMove(vm, x, y, 0, 0, 0);  // re-pick at the press point
      if (mode >= 0 && mode < 0x16) {
        uint32_t bit = 1u << mode;
        if (bit & 0x302002) {  // modes 1, 0xd, 0x14, 0x15 apply at once
          t.inProgress = false;
          vm.fireEvent(player, u"onTryApplyTool", {});
        } else if (bit & 0x100) {  // the bulldozer applies on press (and while dragging, from onHoverDemolish)
          vm.fireEvent(player, u"onTryApplyTool", {});
        }
      }
    }
  } else if (key == kMouseRight) {
    if (mode == kGridModePlaceObject) {  // turn the object (toolMode, +0x24c); eight steps with a diagonal variant
      bool eight = player->fieldValues.count(u"toolObject2") && player->fieldValues[u"toolObject2"].object();
      int o = intField(vm, player, u"toolMode", 0);
      player->fieldValues[u"toolMode"] = Value::integer(eight ? (o + 7) & 7 : (o + 3) & 3);
      toolCancel(vm, player);
      player->mouseMove(vm, x, y, 0, 0, 0);
    } else if (mode == kGridModeRename || mode == kGridModeSetVariation) {
      t.right = true;
      t.cellX = t.cellZ = -1;
      player->mouseMove(vm, x, y, 0, 0, 0);
    } else if (mode != kGridModeSetPavement && mode != kGridModeSetField) {
      if (t.inProgress && grid) {  // the right button cancels a running tool
        gridEditBegin(vm, grid);
        toolCancel(vm, player);
        player->mouseMove(vm, x, y, 0, 0, 0);
        gridEditEnd(vm, grid);
      }
    } else if (!t.right) {  // brushes: the right button paints too
      t.right = true;
      t.toolX = x, t.toolY = y;
      t.startX = t.cellX, t.startY = t.cellY, t.startZ = t.cellZ, t.startW = t.cellW;
      t.cellX = t.cellZ = -1;
      t.cellY = 0;
      player->mouseMove(vm, x, y, 0, 0, 0);
    }
  }
  t.middle = key == kMouseMiddle;
}

// gsGridPlayer::mouseUp (L:0x0818b1e0, selection.md §4.4)
void toolMouseUp(VM& vm, ControlObj* player, float x, float y, int key) {
  ToolState& t = toolState(player);
  int mode = editMode(vm, player);
  uint32_t bit = mode >= 0 && mode < 0x16 ? 1u << mode : 0;
  if (!(bit & 0x2102)) {  // not select, bulldoze or water
    if ((bit & 0x300000) || (bit & 0x4000)) {
      if (key == kMouseRight) {
        t.right = false;
        t.cellX = t.cellZ = -1;
        player->mouseMove(vm, x, y, 0, 0, 0);
        return;
      }
    }
    if ((key == kMouseLeft && t.inProgress) || (key == kMouseRight && t.right)) {
      t.toolX = x, t.toolY = y;
      vm.fireEvent(player, u"onTryApplyTool", {});
      return;
    }
  }
  if (key == kMouseMiddle) t.middle = false;
}

// gsGridPlayer::finishTool (L:0x0819a3e0): the mode's case, then the common tail
bool toolFinish(VM& vm, Object* player) {
  ToolState& t = toolState(player);
  int mode = editMode(vm, player);
  bool ok = true;
  auto m = g_modes.find(mode);
  if (m != g_modes.end()) {
    GridObj* grid = toolGrid(vm, player);
    if (m->second.finish) ok = m->second.finish(vm, dynamic_cast<ControlObj*>(player), grid);
    else if (grid) ok = toolCommit(vm, player, grid);
  }
  t.inProgress = t.right = t.middle = false;
  t.cellX = t.cellZ = -1;
  t.upgraded.clear();
  if (mode != kGridModeSelectObject) player->fieldValues[u"hoverObject"] = Value::null();
  return ok;
}

// gsGridPlayer::cancelTool (L:0x0819bcc0): the mode's case, then finishTool
void toolCancel(VM& vm, Object* player) {
  int mode = editMode(vm, player);
  auto m = g_modes.find(mode);
  if (m != g_modes.end()) {
    GridObj* grid = toolGrid(vm, player);
    if (m->second.cancel) m->second.cancel(vm, dynamic_cast<ControlObj*>(player), grid);
    else toolClearPreview(vm, player, grid);
  }
  toolFinish(vm, player);
}

void registerToolNatives(VM& vm) {
  // canApplyTool (L:0x0818bb80): false when a preview or replaced item carries the error flag (red)
  implement(vm, "gsGridPlayer", u"canApplyTool", [](VM& v, Object* s, int, const Value*) {
    ToolState& t = toolState(s);
    GridObj* grid = toolGrid(v, s);
    if (!grid) return Value::boolean(true);
    const GridData& g = *grid->data;
    auto bad = [&](uint32_t i) { return i < g.items.size() && (g.items[i].flags & kItemNegative); };
    for (uint32_t i : t.newItems)
      if (bad(i)) return Value::boolean(false);
    for (uint32_t i : t.replaced)
      if (bad(i)) return Value::boolean(false);
    return Value::boolean(true);
  });
  // applyTool([repick]) (L:0x0819cbe0): finishTool, then a re-pick at the tool position unless false is passed
  implement(vm, "gsGridPlayer", u"applyTool", [](VM& v, Object* s, int argc, const Value* a) {
    bool ok = toolFinish(v, s);
    if (argc == 0 || v.truthy(a[0]))
      if (auto* c = dynamic_cast<ControlObj*>(s)) c->mouseMove(v, toolState(s).toolX, toolState(s).toolY, 0, 0, 0);
    return Value::boolean(ok);
  });
  implement(vm, "gsGridPlayer", u"resetTool", [](VM& v, Object* s, int, const Value*) {
    toolCancel(v, s);
    return Value();
  });
  // getNewItems / getReplacedItems: the templates (gsGridObject) of the preview and replaced items, one per item, for
  // the price; getNewObjects / getReplacedObjects: their controllers (names of new stops, the demolish effects)
  implement(vm, "gsGridPlayer", u"getNewItems", [](VM& v, Object* s, int, const Value*) {
    std::vector<Value> out;
    if (GridObj* grid = toolGrid(v, s))
      for (uint32_t i : toolState(s).newItems)
        if (Object* t = itemTemplate(grid, i)) out.push_back(obj(t));
    return newArray(out);
  });
  implement(vm, "gsGridPlayer", u"getReplacedItems", [](VM& v, Object* s, int, const Value*) {
    std::vector<Value> out;
    if (GridObj* grid = toolGrid(v, s))
      for (uint32_t i : toolState(s).replaced)
        if (Object* t = itemTemplate(grid, i)) out.push_back(obj(t));
    return newArray(out);
  });
  implement(vm, "gsGridPlayer", u"getNewObjects", [](VM& v, Object* s, int, const Value*) {
    std::vector<Value> out;
    if (GridObj* grid = toolGrid(v, s))
      for (uint32_t i : toolState(s).newItems)
        if (Object* c = gridItemController(v, grid, i)) out.push_back(obj(c));
    return newArray(out);
  });
  implement(vm, "gsGridPlayer", u"getReplacedObjects", [](VM& v, Object* s, int, const Value*) {
    std::vector<Value> out;
    if (GridObj* grid = toolGrid(v, s))
      for (uint32_t i : toolState(s).replaced)
        if (Object* c = gridItemController(v, grid, i)) out.push_back(obj(c));
    return newArray(out);
  });
  // port: regsGetUpgradedItems(), the templates of the live stops the preview would upgrade (ToolState::upgraded)
  if (ClassInfo* c = vm.findClass("gsGridPlayer")) addMethod(c, u"regsGetUpgradedItems", 0, 0);
  implement(vm, "gsGridPlayer", u"regsGetUpgradedItems", [](VM& v, Object* s, int, const Value*) {
    std::vector<Value> out;
    if (GridObj* grid = toolGrid(v, s))
      for (uint32_t i : toolState(s).upgraded)
        if (Object* t = itemTemplate(grid, i)) out.push_back(obj(t));
    return newArray(out);
  });
}

}  // namespace regs::engine
