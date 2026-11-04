// Live edits of a loaded map (city timeline, later the construction tools): items added and removed, terrain cells
// changed, in batches; the end of the outermost batch updates everything derived from the map (grid_edit.h).
#include "engine/grid_edit.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/grid.h"
#include "engine/grid_render.h"
#include "engine/interaction.h"
#include "engine/scene3d.h"
#include "engine/sim.h"

namespace regs::engine {

using namespace regs::script;

namespace {

constexpr uint32_t kFlagCover = 0x4, kFlagBridge = 0x4000, kFlagTunnel = 0x8000, kFlagTemp = 0x100000,
                   kFlagOptional = 0x200000, kFlagHidden = 0x400000;
constexpr int kTypeTree = 0x13, kTypeMarker = 0x15;

int fieldInt(VM& vm, const Object* o, const char16_t* name, int def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toInt(it->second);
}

float fieldFloat(VM& vm, const Object* o, const char16_t* name, float def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toFloat(it->second);
}

struct TypeInfo {
  bool valid = false;
  int width = 1, length = 1, type = 0, height = 0;
  uint32_t flags = 0, diagonals = 0;
};

struct Rect {
  int x0 = 1 << 30, z0 = 1 << 30, x1 = -(1 << 30), z1 = -(1 << 30);
  bool empty() const { return x1 < x0 || z1 < z0; }
  void add(int ax0, int az0, int ax1, int az1) {
    x0 = std::min(x0, ax0), z0 = std::min(z0, az0), x1 = std::max(x1, ax1), z1 = std::max(z1, az1);
  }
};

// per grid: the batch in progress, the type table and the item buckets (by the division of the footprint corner)
struct EditState {
  std::weak_ptr<GridData> data;
  std::vector<TypeInfo> types;
  std::unordered_map<std::string, int> typeIndex;
  int shift = 4, cols = 0, rows = 0;
  std::vector<std::vector<uint32_t>> buckets;
  std::unordered_map<uint32_t, std::vector<uint32_t>> children;  // parent index → child indices
  int depth = 0;
  std::vector<uint32_t> added, removed;
  std::set<uint32_t> changed;
  Rect cells, heightCells;
};

std::unordered_map<const GridObj*, EditState> g_edits;

TypeInfo typeInfoOf(VM& vm, GridObj* grid, const std::string& name) {
  TypeInfo t;
  auto it = grid->templates.find(name);
  auto* o = it == grid->templates.end() ? nullptr : it->second.get();
  if (!o) return t;
  t.valid = true;
  t.width = std::max(1, fieldInt(vm, o, u"width", 1));
  t.length = std::max(1, fieldInt(vm, o, u"length", 1));
  t.type = fieldInt(vm, o, u"type", 0);
  t.flags = uint32_t(fieldInt(vm, o, u"flags", 0));
  t.diagonals = uint32_t(fieldInt(vm, o, u"diagonals", 0));
  t.height = int(fieldFloat(vm, o, u"height", 0) * 256.0f);
  return t;
}

int bucketOf(const EditState& st, int x, int z) {
  int bx = std::clamp(x >> st.shift, 0, st.cols - 1), bz = std::clamp(z >> st.shift, 0, st.rows - 1);
  return bz * st.cols + bx;
}

EditState& state(VM& vm, GridObj* grid) {
  EditState& st = g_edits[grid];
  if (!grid->data || st.data.lock() == grid->data) return st;
  int depth = st.depth;
  st = EditState{};
  st.depth = depth;
  st.data = grid->data;
  const GridData& g = *grid->data;
  for (size_t t = 0; t < g.types.size(); ++t) {
    st.types.push_back(typeInfoOf(vm, grid, g.types[t]));
    st.typeIndex.emplace(g.types[t], int(t));
  }
  st.shift = int(std::min(g.bucketShift, 8u));
  st.cols = std::max(1, (g.width + (1 << st.shift) - 1) >> st.shift);
  st.rows = std::max(1, (g.length + (1 << st.shift) - 1) >> st.shift);
  st.buckets.assign(size_t(st.cols) * size_t(st.rows), {});
  for (uint32_t i = 0; i < g.items.size(); ++i) {
    const GridData::Item& it = g.items[i];
    if (it.removed) continue;
    st.buckets[size_t(bucketOf(st, it.x, it.z))].push_back(i);
    if (it.parent) st.children[it.parent - 1].push_back(i);
  }
  return st;
}

GridItemShape shapeOf(const EditState& st, const GridData::Item& it) {
  GridItemShape s;
  if (it.type >= st.types.size() || !st.types[it.type].valid) return s;
  const TypeInfo& t = st.types[it.type];
  s.sx = (it.orient & 1) ? t.length : t.width;
  s.sz = (it.orient & 1) ? t.width : t.length;
  s.type = t.type;
  s.height = t.height;
  s.flags = it.flags | t.flags;
  return s;
}

ItemFootprint footprintOf(const EditState& st, const GridData::Item& it) {
  const TypeInfo& t = st.types[it.type];
  return itemFootprint(t.width, t.length, t.diagonals, it.orient);
}

void touchItem(EditState& st, const GridData::Item& it) {
  GridItemShape s = shapeOf(st, it);
  st.cells.add(it.x, it.z, it.x + s.sx - 1, it.z + s.sz - 1);
}

// does the new covering item cover any cell of the other item's footprint
bool coversAny(const EditState& st, const GridData::Item& a, const GridData::Item& b) {
  GridItemShape sa = shapeOf(st, a), sb = shapeOf(st, b);
  int x0 = std::max(a.x, b.x), z0 = std::max(a.z, b.z), x1 = std::min(a.x + sa.sx, b.x + sb.sx),
      z1 = std::min(a.z + sa.sz, b.z + sb.sz);
  if (x1 <= x0 || z1 <= z0) return false;
  ItemFootprint fa = footprintOf(st, a), fb = footprintOf(st, b);
  for (int z = z0; z < z1; ++z)
    for (int x = x0; x < x1; ++x)
      if (itemCellCover(fa, sa.flags, x - a.x, z - a.z) && itemCellCover(fb, sb.flags, x - b.x, z - b.z)) return true;
  return false;
}

// the live items in the buckets around a footprint, grown by one division as the engine searches
template <typename F>
void forItemsNear(EditState& st, int x0, int z0, int x1, int z1, F&& fn) {
  int bx0 = std::max(0, (x0 >> st.shift) - 1), bz0 = std::max(0, (z0 >> st.shift) - 1);
  int bx1 = std::min(st.cols - 1, (x1 >> st.shift) + 1), bz1 = std::min(st.rows - 1, (z1 >> st.shift) + 1);
  for (int bz = bz0; bz <= bz1; ++bz)
    for (int bx = bx0; bx <= bx1; ++bx)
      for (uint32_t i : std::vector<uint32_t>(st.buckets[size_t(bz * st.cols + bx)])) fn(i);
}

// gsGridItem::getY (0x100e90e0) at footprint corner (px, pz): the base height, raised along slopes (SLOPE 0x10-0x80:
// 16-128 per cell), a bridge ramp from its low end, diagonal footprints on their own planes (the same rules as the
// simulation's walk layers, sim_citizens.cpp rampY)
int32_t rampY(int32_t y, int sx, int sz, int orient, uint32_t flags, uint32_t diag, int px, int pz) {
  int sh = (flags & 0x10) ? 4 : (flags & 0x20) ? 5 : (flags & 0x40) ? 6 : (flags & 0x80) ? 7 : 0;
  if (!sh) return y;
  if (flags & kFlagBridge) y -= ((orient & 1) ? sz : sx) * (1 << sh);
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

// gsGridItem::fixGrid (L:0x08184890.c:60-100) for a newly placed item: a LOCK_GROUND item (8; tram tracks combine with
// their road) levels the corners under it to its getY and locks them (0x1000); the height is written for covering
// items without a base (flags & 5 == 4) and for BLOCK items (0x10000) not standing on a bridge or tunnel parent
void levelGround(EditState& st, GridData& g, uint32_t index) {
  const GridData::Item& it = g.items[index];
  GridItemShape s = shapeOf(st, it);
  if ((s.flags & 0x500000) || s.type == kTypeMarker) return;
  uint32_t cc = s.flags, parentFlags = 0;
  if (it.parent && it.parent <= g.items.size()) parentFlags = shapeOf(st, g.items[it.parent - 1]).flags;
  if (it.parent && s.type >= 10 && s.type <= 12) cc = (cc | 0xC000) & parentFlags;
  if (!(cc & 0x8) || it.x < 0 || it.z < 0 || it.x + s.sx > g.width || it.z + s.sz > g.length) return;
  ItemFootprint f = footprintOf(st, it);
  bool setHeight = (s.flags & 5) == 4 || ((s.flags & 0x10000) && (!it.parent || !(parentFlags & 0xC000)));
  int32_t y = it.y + (0x200 - int(g.heightBase));
  int px1 = std::min(s.sx, g.width - it.x - 1), pz1 = std::min(s.sz, g.length - it.z - 1);
  for (int pz = 0; pz <= pz1; ++pz)
    for (int px = 0; px <= px1; ++px) {
      if (f.diag) {  // a corner outside the diagonal cuts has no height (getY 0x7fffffff)
        bool inside = false;
        for (int dz = -1; dz <= 0 && !inside; ++dz)
          for (int dx = -1; dx <= 0 && !inside; ++dx)
            if (px + dx >= 0 && pz + dz >= 0 && px + dx < s.sx && pz + dz < s.sz && itemCellCover(f, s.flags, px + dx, pz + dz))
              inside = true;
        if (!inside) continue;
      }
      size_t cell = size_t(it.z + pz) * size_t(g.width) + size_t(it.x + px);
      if (setHeight) g.height[cell] = int16_t(rampY(y, s.sx, s.sz, it.orient, s.flags, f.diag, px, pz));
      g.flags[cell] |= 0x1000;
    }
  if (setHeight) st.heightCells.add(it.x, it.z, it.x + px1, it.z + pz1);
  st.cells.add(it.x, it.z, it.x + px1, it.z + pz1);
}

void removeItem(VM& vm, GridObj* grid, EditState& st, uint32_t index, int depth) {
  GridData& g = *grid->data;
  if (index >= g.items.size() || g.items[index].removed || depth > 64) return;
  GridData::Item& it = g.items[index];
  it.removed = true;
  auto& b = st.buckets[size_t(bucketOf(st, it.x, it.z))];
  b.erase(std::remove(b.begin(), b.end(), index), b.end());
  if (it.parent) {
    auto& siblings = st.children[it.parent - 1];
    siblings.erase(std::remove(siblings.begin(), siblings.end(), index), siblings.end());
  }
  touchItem(st, it);
  st.removed.push_back(index);
  // children lose their parent; the non-temporary ones go with it (gsGrid::removeObject L:0x08147c60)
  auto kids = st.children.find(index);
  if (kids == st.children.end()) return;
  std::vector<uint32_t> list = std::move(kids->second);
  st.children.erase(kids);
  for (uint32_t c : list) {
    if (c >= g.items.size() || g.items[c].removed) continue;
    g.items[c].parent = 0;
    if (!(shapeOf(st, g.items[c]).flags & kFlagTemp)) removeItem(vm, grid, st, c, depth + 1);
  }
}

}  // namespace

void gridEditBegin(VM& vm, GridObj* grid) { ++state(vm, grid).depth; }

void gridEditEnd(VM& vm, GridObj* grid) {
  EditState& st = state(vm, grid);
  if (st.depth > 0 && --st.depth > 0) return;
  if (!grid->data) return;
  GridData& g = *grid->data;
  if (st.added.empty() && st.removed.empty() && st.changed.empty() && st.cells.empty()) return;
  for (uint32_t i : st.changed) touchItem(st, g.items[i]);
  // cover quadrants and normal overrides of every touched cell, re-stamped from the items now there
  if (!st.cells.empty()) fixGridCoverArea(vm, grid, st.cells.x0, st.cells.z0, st.cells.x1, st.cells.z1);
  // the renderer: terrain of the divisions around the cells (the builder reads one cell beyond), items of the
  // divisions that drew removed or changed items (added ones are bucketed when the renderer syncs)
  if (TerrainCache* tc = grid->terrain.get(); tc && tc->cols > 0) {
    if (!st.cells.empty()) {
      int n = tc->n;
      int dx0 = std::max(0, (st.cells.x0 - 1) / n), dz0 = std::max(0, (st.cells.z0 - 1) / n);
      int dx1 = std::min(tc->cols - 1, (st.cells.x1 + 1) / n), dz1 = std::min(tc->rows - 1, (st.cells.z1 + 1) / n);
      for (int dz = dz0; dz <= dz1; ++dz)
        for (int dx = dx0; dx <= dx1; ++dx) tc->dirtyTerrain.insert(dz * tc->cols + dx);
    }
    for (uint32_t i : st.removed)
      if (int d = itemDivision(g, *tc, i); d >= 0) tc->dirtyItems.insert(d);
    for (uint32_t i : st.changed)
      if (int d = itemDivision(g, *tc, i); d >= 0) tc->dirtyItems.insert(d);
  }
  ++grid->itemsVersion;
  std::vector<uint32_t> added = std::move(st.added), removed = std::move(st.removed);
  Rect heights = st.heightCells;
  st.added.clear();
  st.removed.clear();
  st.changed.clear();
  st.cells = Rect{};
  st.heightCells = Rect{};
  // the simulation first (graph, walk layers, pools), then the controllers of removed items (their onDestroy
  // handlers may look at the map). Tool previews (TEMP) are not part of the simulated city: the graph and the stops
  // skip them anyway, and a preview is rebuilt on every mouse move.
  auto real = [&](const std::vector<uint32_t>& list) {
    std::vector<uint32_t> out;
    for (uint32_t i : list)
      if (i < g.items.size() && !(shapeOf(st, g.items[i]).flags & kFlagTemp)) out.push_back(i);
    return out;
  };
  std::vector<uint32_t> simAdded = real(added), simRemoved = real(removed);
  if (!simAdded.empty() || !simRemoved.empty()) simItemsChanged(vm, grid, simAdded, simRemoved);
  if (!heights.empty()) simTerrainChanged(vm, grid, heights.x0, heights.z0, heights.x1, heights.z1);
  for (uint32_t i : removed) gridItemControllerRemoved(vm, grid, i);
}

int gridEditAddItem(VM& vm, GridObj* grid, const std::string& templateName, int x, int y, int z, int orient,
                    uint32_t flags, int parent, bool removeOptional) {
  if (!grid->data || !grid->templates.count(templateName)) return -1;
  EditState& st = state(vm, grid);
  GridData& g = *grid->data;
  auto known = st.typeIndex.find(templateName);
  int type;
  if (known == st.typeIndex.end()) {
    type = int(g.types.size());
    g.types.push_back(templateName);
    st.types.push_back(typeInfoOf(vm, grid, templateName));
    st.typeIndex.emplace(templateName, type);
  } else {
    type = known->second;
  }
  GridData::Item it;
  it.type = uint16_t(type);
  it.orient = uint8_t(orient & 3);
  it.x = int16_t(x);
  it.y = int16_t(y - (0x200 - int(g.heightBase)));  // items keep the file's y; the engine's carries the shift
  it.z = int16_t(z);
  it.parent = parent >= 0 && size_t(parent) < g.items.size() ? uint32_t(parent) + 1 : 0;
  it.flags = flags & ~0xFFu;
  uint32_t index = uint32_t(g.items.size());
  g.items.push_back(it);
  if (g.names.size() < g.items.size()) g.names.resize(g.items.size());
  st.buckets[size_t(bucketOf(st, it.x, it.z))].push_back(index);
  if (it.parent) st.children[it.parent - 1].push_back(index);
  touchItem(st, it);
  st.added.push_back(index);
  levelGround(st, g, index);
  // removeOverlappingOptionalObjects (L:0x081481b0): a placed, covering, non-optional item clears the optional details
  // (trees, props) under it
  GridItemShape s = shapeOf(st, it);
  if (removeOptional && !(s.flags & kFlagTemp) && (s.flags & (kFlagOptional | kFlagCover)) == kFlagCover &&
      s.type != kTypeMarker) {
    forItemsNear(st, it.x, it.z, it.x + s.sx - 1, it.z + s.sz - 1, [&](uint32_t o) {
      if (o == index || g.items[o].removed) return;
      if (!(shapeOf(st, g.items[o]).flags & kFlagOptional)) return;
      if (coversAny(st, g.items[index], g.items[o])) removeItem(vm, grid, st, o, 0);
    });
  }
  return int(index);
}

void gridEditRemoveItem(VM& vm, GridObj* grid, uint32_t item) {
  if (!grid->data) return;
  removeItem(vm, grid, state(vm, grid), item, 0);
}

void gridEditItemChanged(GridObj* grid, uint32_t item) { g_edits[grid].changed.insert(item); }

void gridEditCellsChanged(GridObj* grid, int x0, int z0, int x1, int z1, bool heights) {
  EditState& st = g_edits[grid];
  st.cells.add(x0, z0, x1, z1);
  if (heights) st.heightCells.add(x0, z0, x1, z1);
}

std::vector<uint32_t> gridItemsAt(VM& vm, GridObj* grid, int x, int z) {
  std::vector<uint32_t> out;
  if (!grid->data) return out;
  const EditState& st = state(vm, grid);
  for (uint32_t i : st.buckets[size_t(bucketOf(st, x, z))])
    if (grid->data->items[i].x == x && grid->data->items[i].z == z && !grid->data->items[i].removed) out.push_back(i);
  return out;
}

std::vector<uint32_t> gridItemsNear(VM& vm, GridObj* grid, int x0, int z0, int x1, int z1) {
  std::vector<uint32_t> out;
  if (!grid->data) return out;
  forItemsNear(state(vm, grid), x0, z0, x1, z1, [&](uint32_t i) {
    if (!grid->data->items[i].removed) out.push_back(i);
  });
  return out;
}

int gridTypeIndex(VM& vm, GridObj* grid, const std::string& templateName) {
  if (!grid->data) return -1;
  const EditState& st = state(vm, grid);
  auto t = st.typeIndex.find(templateName);
  return t == st.typeIndex.end() ? -1 : t->second;
}

GridItemShape gridItemShape(VM& vm, GridObj* grid, uint32_t item) {
  EditState& st = state(vm, grid);
  return item < grid->data->items.size() ? shapeOf(st, grid->data->items[item]) : GridItemShape{};
}

bool gridItemsConflict(VM& vm, GridObj* grid, uint32_t a, uint32_t b) {
  EditState& st = state(vm, grid);
  const GridData& g = *grid->data;
  if (a == b || a >= g.items.size() || b >= g.items.size()) return false;
  const GridData::Item &ia = g.items[a], &ib = g.items[b];
  if (ia.removed || ib.removed) return false;
  // related items (gsGridItem::checkConflict L:0x08183aa0, first rules)
  if (ia.parent == b + 1 || ib.parent == a + 1) return false;
  GridItemShape sa = shapeOf(st, ia), sb = shapeOf(st, ib);
  if (ia.parent && ia.parent == ib.parent && !((sa.flags ^ sb.flags) & kFlagTemp)) return false;
  if (sa.type == kTypeMarker || sb.type == kTypeMarker) return false;
  if ((sa.flags & 0x500000) == 0x500000 && (sb.flags & kFlagOptional) && sb.type != kTypeTree) return false;
  if ((sb.flags & 0x500000) == 0x500000 && (sa.flags & kFlagOptional) && sa.type != kTypeTree) return false;
  if ((sa.flags & 0x500000) == kFlagHidden || (sb.flags & 0x500000) == kFlagHidden) return false;
  // port simplification of the layer rules: tunnels only meet tunnels; elsewhere the height ranges must overlap
  if ((sa.flags & kFlagTunnel) != (sb.flags & kFlagTunnel)) return false;
  int shift = 0x200 - int(g.heightBase);
  int ya = ia.y + shift, yb = ib.y + shift;
  int ha = std::max(sa.height, 0x80), hb = std::max(sb.height, 0x80);
  if (((sa.flags | sb.flags) & kFlagBridge) && !(ya < yb + hb && yb < ya + ha)) return false;
  return coversAny(st, ia, ib);
}


// gsGrid.findItem and gsGrid.getRandomItem (docs/handlers/batch-A.md): queries over the live items that return the
// item's controller. Port: the template and name scans run in file order, not in slot order (differs only when
// several items match); getRandomItem draws slots with its own rand() sequence.
void registerGridEditNatives(VM& vm) {
  static auto controllerValue = [](VM& v, GridObj* grid, uint32_t i) {
    Object* c = gridItemController(v, grid, i);
    return c ? Value::heap(VT::Object, c) : Value::null();
  };
  auto templateOf = [](GridObj* grid, const GridData::Item& it) -> Object* {
    const GridData& g = *grid->data;
    if (it.type >= g.types.size()) return nullptr;
    auto t = grid->templates.find(g.types[it.type]);
    return t == grid->templates.end() ? nullptr : t->second.get();
  };
  implement(vm, "gsGrid", u"findItem", [templateOf](VM& v, Object* s, int argc, const Value* a) {
    auto* grid = dynamic_cast<GridObj*>(s);
    if (!grid || !grid->data || argc < 1) return Value::null();
    const GridData& g = *grid->data;
    Value arg = v.resolve(a[0]);
    if (Object* tpl = arg.object(); tpl && dynamic_cast<ObjectObj*>(tpl)) {  // template form: skips flag 0x400000
      for (uint32_t i = 0; i < g.items.size(); ++i) {
        const GridData::Item& it = g.items[i];
        if (!it.removed && !(it.flags & kFlagHidden) && templateOf(grid, it) == tpl)
          return controllerValue(v, grid, i);
      }
      return Value::null();
    }
    std::string name = utf16ToUtf8(v.toStr(arg));
    Value pos = argc > 1 ? v.resolve(a[1]) : Value();
    EditState& st = state(v, grid);
    for (uint32_t i = 0; i < g.items.size() && i < g.names.size(); ++i) {
      const GridData::Item& it = g.items[i];
      if (it.removed || g.names[i] != name) continue;
      if (pos.type() == VT::Vector) {  // the item's world centre within 1.0 of pos
        ItemFootprint fp = footprintOf(st, it);
        float cs = g.cellSize;
        float dx = float(2 * it.x + fp.sx - g.width) * cs * 0.5f - pos.vec()[0];
        float dy = float(it.y) * cs / 256.0f - pos.vec()[1];
        float dz = float(2 * it.z + fp.sz - g.length) * cs * 0.5f - pos.vec()[2];
        if (dx * dx + dy * dy + dz * dz > 1.0f) continue;
      }
      return controllerValue(v, grid, i);
    }
    return Value::null();
  });
  implement(vm, "gsGrid", u"getRandomItem", [templateOf](VM& v, Object* s, int argc, const Value* a) {
    static uint32_t seed = 1;
    auto rnd = [] { seed = seed * 0x343FDu + 0x269EC3u; return int((seed >> 16) & 0x7FFF); };
    auto* grid = dynamic_cast<GridObj*>(s);
    if (!grid || !grid->data || argc < 1) return Value::null();
    const GridData& g = *grid->data;
    EditState& st = state(v, grid);
    Object* tpl = nullptr;
    int kind = 0, cls = 0, lo = 0, hi = 0;
    if (argc < 4) {
      tpl = v.resolve(a[0]).object();
      if (!dynamic_cast<ObjectObj*>(tpl)) return Value::null();
    } else {
      kind = v.toInt(a[0]), cls = v.toInt(a[1]), lo = v.toInt(a[2]), hi = v.toInt(a[3]);
    }
    auto fits = [&](const GridData::Item& it) {
      if (it.removed || (it.flags & kFlagHidden)) return false;
      Object* t = templateOf(grid, it);
      if (tpl) return t == tpl;
      auto* o = dynamic_cast<ObjectObj*>(t);
      if (!o || fieldInt(v, o, u"type", 0) != 0x14) return false;  // GRID_ITEM_BUILDING
      int value;
      if (kind == 1) value = cls >= 0 && cls < 7 ? o->homePlaces[cls] : 0;
      else if (kind == 2) value = cls >= 0 && cls < 7 ? o->workPlaces[cls] : 0;
      else {
        if (kind < 0 || kind >= 13 || (cls >= 0 && cls < 32 && (o->barredVisitors >> cls & 1))) return false;
        value = o->visitPlaces[kind];
      }
      return value >= lo && value <= hi;
    };
    int slots = int(st.buckets.size());
    for (int n = 0; n < 4096 && slots > 0; ++n) {
      int r = ((rnd() << 15) | rnd()) % slots;
      for (uint32_t i : st.buckets[size_t(r)])
        if (fits(g.items[i])) return controllerValue(v, grid, i);
    }
    return Value::null();
  });
}

}  // namespace regs::engine
