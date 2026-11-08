// The map editor's templates (port, owner's request 2026-11-07): select an area, save its terrain and objects as a
// template, and stamp it somewhere else. Two tool modes and a few natives (script side:
// data/regs/extend/metro/scripts/editor.script):
// * kModeSelectArea: drag a rectangle (left button); Shift adds rectangles and objects (click) or takes them out
//   (regsSelection / regsClearSelection).
// * gsGrid.regsSaveTemplate(path): the selection's cells (heights, ground flags) and objects (with their parts)
//   into a small map.grid, and its minimap picture as <path>.dds. Heights are stored relative to the area's mean height (rounded
//   to half a cell, so buildings stay aligned): the template's ground level is 0x200.
// * regsSetTemplate(path): the template the place mode stamps; kModePlaceTemplate shows its outline under the cursor
//   and, on click, replaces the area with it: the objects touching it go (shown red beforehand), the terrain takes the template's
//   heights around the target area's mean height, the template's objects are added. Everything is recorded on the
//   timeline as one group (a terrain patch action, deletions, creations), so undo and saving work as for any edit.
#include "engine/tool_templates.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/grid_save.h"
#include "engine/gui.h"
#include "engine/input.h"
#include "engine/interaction.h"
#include "engine/minimap.h"
#include "engine/timeline.h"
#include "engine/tools.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

namespace {

constexpr int kModeSelectArea = 0x40, kModePlaceTemplate = 0x41;
constexpr uint32_t kGroundFlags = 0x203F;  // ground style, pavement, field, water
// a template saved without its terrain: every height holds this mark, and placing it leaves the ground as it is
constexpr int16_t kNoTerrain = INT16_MIN;

struct Rect {
  int x = 0, z = 0, w = 0, l = 0;
  bool valid() const { return w > 0 && l > 0; }
};

Rect g_shown;                       // the highlighted rectangle (both modes)
std::shared_ptr<GridData> g_template;
std::string g_templateName;

int shiftOf(const GridData& g) { return 0x200 - int(g.heightBase); }

void show(VM& vm, GridObj* grid, Rect r, bool negative) {
  gridEditBegin(vm, grid);
  if (g_shown.valid()) gridUnselectArea(vm, grid, g_shown.x, g_shown.z, g_shown.w, g_shown.l);
  if (r.valid()) gridSelectArea(vm, grid, r.x, r.z, r.w, r.l, negative);
  gridEditEnd(vm, grid);
  static const bool dbg = std::getenv("REGS_DEBUG_TOOLS") != nullptr;
  if (dbg) LOG_INFO("template tool: shown %d,%d %dx%d -> %d,%d %dx%d, %zu items highlighted", g_shown.x, g_shown.z, g_shown.w, g_shown.l, r.x, r.z, r.w, r.l, grid->highlightItems.size());
  g_shown = r;
}

void hide(VM& vm, GridObj* grid) { show(vm, grid, Rect{}, false); }

// the mean of the corners of a rectangle, rounded to half a cell
int meanHeight(const GridData& g, Rect r) {
  long sum = 0, n = 0;
  for (int z = r.z; z <= r.z + r.l && z < g.length; ++z)
    for (int x = r.x; x <= r.x + r.w && x < g.width; ++x)
      if (x >= 0 && z >= 0) sum += g.height[size_t(z) * size_t(g.width) + size_t(x)], ++n;
  int m = n ? int(sum / n) : 0x200;
  return (m + 0x40) & ~0x7f;
}

// roots and their parts (children of any depth), in index order (parents before their children)
std::vector<uint32_t> withParts(const GridData& g, const std::vector<uint32_t>& roots) {
  std::vector<bool> keep(g.items.size(), false);
  for (uint32_t i : roots)
    if (i < keep.size()) keep[i] = true;
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < g.items.size(); ++i) {
    const GridData::Item& it = g.items[i];
    if (it.removed || (it.flags & kItemTemp)) continue;
    if (!keep[i] && it.parent && it.parent <= g.items.size() && keep[it.parent - 1]) keep[i] = true;
    if (keep[i]) out.push_back(i);
  }
  return out;
}

// the live items whose footprint lies fully inside the rectangle, each with its parts (children)
std::vector<uint32_t> itemsInside(VM& vm, GridObj* grid, Rect r) {
  const GridData& g = *grid->data;
  std::vector<uint32_t> roots, out;
  for (uint32_t i : gridItemsNear(vm, grid, r.x, r.z, r.x + r.w - 1, r.z + r.l - 1)) {
    const GridData::Item& it = g.items[i];
    if (it.removed || it.parent || (it.flags & kItemTemp)) continue;
    int x, z, w, l;
    gridItemRect(vm, grid, i, x, z, w, l);
    if (x >= r.x && z >= r.z && x + w <= r.x + r.w && z + l <= r.z + r.l) roots.push_back(i);
  }
  std::sort(roots.begin(), roots.end());
  // parts in index order after their parent (children are created after parents)
  std::vector<bool> keep(g.items.size(), false);
  for (uint32_t i : roots) keep[i] = true;
  for (uint32_t i = 0; i < g.items.size(); ++i) {
    const GridData::Item& it = g.items[i];
    if (it.removed || (it.flags & kItemTemp)) continue;
    if (!keep[i] && it.parent && it.parent <= g.items.size() && keep[it.parent - 1]) keep[i] = true;
    if (keep[i]) out.push_back(i);
  }
  return out;
}

// the live items touching the rectangle, by their root (removing a root takes its parts)
std::vector<uint32_t> rootsTouching(VM& vm, GridObj* grid, Rect r) {
  const GridData& g = *grid->data;
  std::vector<uint32_t> out;
  for (uint32_t i : gridGetObjects(vm, grid, r.x, r.z, r.w, r.l)) {
    uint32_t root = i;
    for (int guard = 0; guard < 32 && g.items[root].parent && g.items[root].parent <= g.items.size(); ++guard)
      root = g.items[root].parent - 1;
    if (!g.items[root].removed && !(g.items[root].flags & kItemTemp) && std::find(out.begin(), out.end(), root) == out.end())
      out.push_back(root);
  }
  return out;
}

// ---- select area ----
// The selection is an area (its terrain) and a set of objects (by their root item). Dragging without Shift starts a
// new selection: the rectangle and the objects lying fully inside it. With Shift held, a drag adds its rectangle and
// objects, and a click on an object adds it (the area grows to cover it) or takes it out again. The selection stays
// highlighted until it is saved, cleared or the tool changes.
Rect g_area;                      // the selection's terrain area
std::vector<uint32_t> g_items;    // the selected objects (roots)
Rect g_drag;                      // the rectangle being dragged
bool g_dragShift = false;

Rect unite(Rect a, Rect b) {
  if (!a.valid()) return b;
  if (!b.valid()) return a;
  int x0 = std::min(a.x, b.x), z0 = std::min(a.z, b.z);
  int x1 = std::max(a.x + a.w, b.x + b.w), z1 = std::max(a.z + a.l, b.z + b.l);
  return Rect{x0, z0, x1 - x0, z1 - z0};
}

uint32_t rootOf(const GridData& g, uint32_t i) {
  for (int guard = 0; guard < 32 && g.items[i].parent && g.items[i].parent <= g.items.size(); ++guard) i = g.items[i].parent - 1;
  return i;
}

std::vector<uint32_t> rootsInside(VM& vm, GridObj* grid, Rect r) {
  const GridData& g = *grid->data;
  std::vector<uint32_t> out;
  for (uint32_t i : itemsInside(vm, grid, r))
    if (!g.items[i].parent) out.push_back(i);
  return out;
}

// the highlight: the area's cells and the selected objects in green, the rectangle being dragged too
Rect g_litCells;
std::vector<uint32_t> g_litItems;
void showSelection(VM& vm, GridObj* grid) {
  gridEditBegin(vm, grid);
  if (g_litCells.valid()) gridUnselectArea(vm, grid, g_litCells.x, g_litCells.z, g_litCells.w, g_litCells.l);
  for (uint32_t i : g_litItems) gridSetSelected(vm, grid, i, false, false);
  g_litItems.clear();
  Rect cells = unite(g_area, g_drag);
  if (cells.valid()) {
    gridSelectArea(vm, grid, cells.x, cells.z, cells.w, cells.l, false);
    // gridSelectArea lit every object touching the cells: only the selected ones stay lit
    for (uint32_t i : gridGetObjects(vm, grid, cells.x, cells.z, cells.w, cells.l)) gridSetSelected(vm, grid, i, false, false);
  }
  const GridData& g = *grid->data;
  for (uint32_t r : g_items)
    for (uint32_t i : withParts(g, {r})) {
      gridSetSelected(vm, grid, i, true, false);
      g_litItems.push_back(i);
    }
  gridEditEnd(vm, grid);
  g_litCells = cells;
}

void clearSelection(VM& vm, GridObj* grid) {
  g_area = g_drag = Rect{};
  g_items.clear();
  if (grid && grid->data) showSelection(vm, grid);
}

Rect dragRect(const ToolState& t, int cx, int cz) {
  return Rect{std::min(t.startX, cx), std::min(t.startZ, cz), std::abs(cx - t.startX) + 1, std::abs(cz - t.startZ) + 1};
}

void moveSelect(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  ToolState& t = toolState(player);
  ToolPick p = toolPick(vm, player, x, y, false);
  if (!p.hit) return;
  if (p.cellX == t.cellX && p.cellZ == t.cellZ) return;
  t.cellX = p.cellX, t.cellZ = p.cellZ;
  if (!t.inProgress || t.startX < 0) return;
  g_drag = dragRect(t, p.cellX, p.cellZ);
  showSelection(vm, grid);
}

void downSelect(VM&, ControlObj* player, GridObj*) {
  ToolState& t = toolState(player);
  t.startX = t.cellX, t.startZ = t.cellZ;
  g_dragShift = (inputModifiers() & 0x100) != 0;
}

void upSelect(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  ToolState& t = toolState(player);
  const GridData& g = *grid->data;
  ToolPick cell = toolPick(vm, player, x, y, false);
  bool click = cell.hit && cell.cellX == t.startX && cell.cellZ == t.startZ;
  if (g_dragShift && click) {
    // Shift + click: the object under the cursor in or out
    ToolPick p = toolPick(vm, player, x, y, false);
    if (p.hit && p.item >= 0) {
      uint32_t r = rootOf(g, uint32_t(p.item));
      auto it = std::find(g_items.begin(), g_items.end(), r);
      if (it != g_items.end()) {
        g_items.erase(it);
      } else {
        g_items.push_back(r);
        Rect ir;
        gridItemRect(vm, grid, r, ir.x, ir.z, ir.w, ir.l);
        g_area = unite(g_area, ir);
      }
    }
  } else if (cell.hit && t.startX >= 0) {
    Rect r = dragRect(t, cell.cellX, cell.cellZ);
    if (!g_dragShift) {
      g_area = r;
      g_items = rootsInside(vm, grid, r);
    } else {
      g_area = unite(g_area, r);
      for (uint32_t i : rootsInside(vm, grid, r))
        if (std::find(g_items.begin(), g_items.end(), i) == g_items.end()) g_items.push_back(i);
    }
  }
  g_drag = Rect{};
  showSelection(vm, grid);
  static const bool dbg = std::getenv("REGS_DEBUG_TOOLS") != nullptr;
  if (dbg) LOG_INFO("select: up start %d,%d hit %d cell %d,%d shift %d -> area %d,%d %dx%d, %zu objects", t.startX, t.startZ, cell.hit, cell.cellX, cell.cellZ, g_dragShift, g_area.x, g_area.z, g_area.w, g_area.l, g_items.size());
}

void cancelSelect(VM& vm, ControlObj*, GridObj* grid) { clearSelection(vm, grid); }
bool finishSelect(VM& vm, ControlObj*, GridObj* grid) {
  clearSelection(vm, grid);
  return true;
}

// ---- place template ----
Rect placeRect(const GridData& g, int cx, int cz) {
  if (!g_template) return Rect{};
  int w = g_template->width - 1, l = g_template->length - 1;
  Rect r{cx - w / 2, cz - l / 2, w, l};
  (void)g;
  return r;
}

void movePlace(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  ToolState& t = toolState(player);
  ToolPick p = toolPick(vm, player, x, y, true);
  if (!p.hit || !g_template) return;
  Rect r = placeRect(*grid->data, p.cellX, p.cellZ);
  if (r.x == g_shown.x && r.z == g_shown.z && r.valid() == g_shown.valid()) return;
  const GridData& g = *grid->data;
  bool outside = r.x < 0 || r.z < 0 || r.x + r.w >= g.width || r.z + r.l >= g.length;
  show(vm, grid, r, outside);
  gridEditBegin(vm, grid);
  for (uint32_t i : rootsTouching(vm, grid, r)) gridSetSelected(vm, grid, i, true, true);  // they will be replaced
  gridEditEnd(vm, grid);
  t.cellX = p.cellX, t.cellZ = p.cellZ;
}

void upPlace(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  movePlace(vm, player, grid, x, y);
  if (!g_template || !g_shown.valid()) return;
  GridData& g = *grid->data;
  const GridData& tp = *g_template;
  Rect r = g_shown;
  if (r.x < 0 || r.z < 0 || r.x + r.w >= g.width || r.z + r.l >= g.length) return;  // red: off the map
  hide(vm, grid);
  Object* tl = playerTimeline(vm, player);
  if (tl) timelineBeginGroup(vm, tl, "%9 " + g_templateName);
  gridEditBegin(vm, grid);
  // what touches the area goes (with its parts)
  std::vector<uint32_t> old = rootsTouching(vm, grid, r);
  for (uint32_t i : old) {
    gridSetSelected(vm, grid, i, false, false);
    if (g.items[i].removed) continue;
    timelineRecordDelete(vm, tl, grid, i);
    gridEditRemoveItem(vm, grid, i);
  }
  // the terrain: the template's heights around the target's mean height, its ground flags
  int base = meanHeight(g, r), delta = base - 0x200;
  int W = r.w + 1, L = r.l + 1;
  bool withTerrain = tp.height.empty() || tp.height[0] != kNoTerrain;
  if (withTerrain) {
  std::vector<int16_t> heights(size_t(W) * size_t(L));
  std::vector<uint16_t> flags(heights.size());
  for (int z = 0; z < L; ++z)
    for (int x = 0; x < W; ++x) {
      size_t k = size_t(z) * size_t(W) + size_t(x), s = size_t(z) * size_t(tp.width) + size_t(x);
      size_t c = size_t(r.z + z) * size_t(g.width) + size_t(r.x + x);
      int h = std::clamp(int(tp.height[s]) + delta, -0x7000, 0x7000);
      heights[k] = int16_t(h);
      flags[k] = uint16_t(tp.flags[s] & kGroundFlags);
      g.height[c] = int16_t(h);
      // the last row and column are only corners: their cells keep their own flags
      if (x < r.w && z < r.l) g.flags[c] = uint16_t((g.flags[c] & ~kGroundFlags) | flags[k]);
      else flags[k] = uint16_t(g.flags[c] & kGroundFlags);
    }
  gridEditCellsChanged(grid, r.x - 1, r.z - 1, r.x + W, r.z + L, true);
  if (tl) timelineRecordPatch(vm, tl, r.x, r.z, W, L, heights, flags);
  }
  // the template's objects, parents first (as stored)
  std::vector<int> made(tp.items.size(), -1);
  int shiftT = shiftOf(tp);
  for (size_t i = 0; i < tp.items.size(); ++i) {
    const GridData::Item& it = tp.items[i];
    if (it.type >= tp.types.size()) continue;
    int parent = it.parent && it.parent <= made.size() ? made[it.parent - 1] : -1;
    if (it.parent && parent < 0) continue;  // its parent did not come
    int idx = gridEditAddItem(vm, grid, tp.types[it.type], r.x + it.x, it.y + shiftT + delta, r.z + it.z, it.orient,
                              it.flags & ~(kItemTemp | kItemSelected | kItemNegative | kItemHidden), parent, false);
    if (idx < 0) continue;
    made[i] = idx;
    g.items[size_t(idx)].variation = it.variation;
    g.items[size_t(idx)].yaw = it.yaw;  // port: the free turn
    if (i < tp.names.size() && !tp.names[i].empty()) {
      if (g.names.size() <= size_t(idx)) g.names.resize(size_t(idx) + 1);
      g.names[size_t(idx)] = tp.names[i];
    }
  }
  gridEditEnd(vm, grid);
  for (size_t i = 0; i < made.size(); ++i)
    if (made[i] >= 0) timelineRecordCreate(vm, tl, grid, uint32_t(made[i]));
  if (tl) timelineEndGroup(vm, tl);
  // the template events of the new objects (their controllers exist now)
  for (int i : made) {
    if (i < 0 || g.items[size_t(i)].parent) continue;
    auto t = grid->templates.find(g.types[g.items[size_t(i)].type]);
    Object* c = gridItemController(vm, grid, uint32_t(i));
    if (t == grid->templates.end() || !c) continue;
    try {
      vm.fireEvent(t->second.get(), u"onCreateInstance", {Value::heap(VT::Object, c)});
    } catch (const std::exception& e) {
      LOG_ERROR("onCreateInstance: %s", e.what());
    }
  }
  LOG_INFO("template %s placed at %d,%d (%dx%d), %zu objects replaced", g_templateName.c_str(), r.x, r.z, r.w, r.l, old.size());
  g_shown = Rect{};
}

void cancelBoth(VM& vm, ControlObj*, GridObj* grid) {
  if (grid && grid->data) hide(vm, grid);
}

bool finishBoth(VM& vm, ControlObj*, GridObj* grid) {
  if (grid && grid->data) hide(vm, grid);
  return true;
}

}  // namespace

void registerTemplateTools(VM& vm) {
  ToolMode select;
  select.move = moveSelect;
  select.down = downSelect;
  select.up = upSelect;
  select.cancel = cancelSelect;
  select.finish = finishSelect;
  registerToolMode(kModeSelectArea, select);
  ToolMode place;
  place.move = movePlace;
  place.up = upPlace;
  place.cancel = cancelBoth;
  place.finish = finishBoth;
  registerToolMode(kModePlaceTemplate, place);
  vm.setGlobal(u"REGS_GRIDMODE_SELECT_AREA", Value::integer(kModeSelectArea));
  vm.setGlobal(u"REGS_GRIDMODE_PLACE_TEMPLATE", Value::integer(kModePlaceTemplate));

  // regsSelection(): [x, z, w, l, objects] of the current selection, or null; regsClearSelection()
  implementGlobal(vm, u"regsSelection", [](VM&, Object*, int, const Value*) {
    if (!g_area.valid()) return Value::null();
    auto a = make<ArrayObj>();
    for (int v : {g_area.x, g_area.z, g_area.w, g_area.l, int(g_items.size())}) a->items.push_back(Value::integer(v));
    return Value::heap(VT::Array, a.get());
  }, 0, 0);
  implementGlobal(vm, u"regsClearSelection", [](VM& v, Object*, int argc, const Value* a) {
    GridObj* grid = argc > 0 ? dynamic_cast<GridObj*>(v.resolve(a[0]).object()) : nullptr;
    if (grid && grid->data) {
      clearSelection(v, grid);
    } else {
      g_area = g_drag = Rect{};
      g_items.clear();
    }
    return Value();
  }, 0, 1);

  // regsCaptureTemplatePreview(path): the next frame's centre square, scaled to 128 × 128, as <path> (DDS); the
  // script hides the interface and the selection highlight for that frame (a screenshot as the template's picture)
  implementGlobal(vm, u"regsCaptureTemplatePreview", [](VM& v, Object*, int, const Value* a) {
    Display* d = display();
    if (!d) return Value::boolean(false);
    std::string path = utf16ToUtf8(v.toStr(a[0]));
    VM* vmp = &v;
    d->captureNext = [vmp, path](const std::vector<uint8_t>& px, int w, int h) {
      const int P = 128;
      int side = std::min(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
      std::vector<uint8_t> dds;
      auto p32 = [&](uint32_t x) { dds.insert(dds.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4); };
      for (uint32_t x : {0x20534444u, 0x7Cu, 0x100Fu, uint32_t(P), uint32_t(P), uint32_t(P * 4), 0u, 1u}) p32(x);
      for (int i = 0; i < 11; ++i) p32(0);
      for (uint32_t x : {0x20u, 0x41u, 0u, 0x20u, 0xFF0000u, 0xFF00u, 0xFFu, 0xFF000000u, 0x1000u, 0u, 0u, 0u, 0u}) p32(x);
      for (int y = 0; y < P; ++y)
        for (int x = 0; x < P; ++x) {
          // the mean of the screen pixels this preview pixel covers
          int sx0 = x0 + x * side / P, sx1 = x0 + (x + 1) * side / P, sy0 = y0 + y * side / P, sy1 = y0 + (y + 1) * side / P;
          uint32_t r = 0, g = 0, b = 0, n = 0;
          for (int yy = sy0; yy < std::max(sy1, sy0 + 1); ++yy)
            for (int xx = sx0; xx < std::max(sx1, sx0 + 1); ++xx) {
              const uint8_t* q = &px[(size_t(yy) * size_t(w) + size_t(xx)) * 4];
              r += q[0], g += q[1], b += q[2], ++n;
            }
          p32(0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n));
        }
      vmp->vfs().write(path, dds);
      vmp->vfs().rescan();
    };
    return Value::boolean(true);
  }, 1, 1);

  // regsSetTemplate(path): the template the place mode stamps; false when the file is no template
  implementGlobal(vm, u"regsSetTemplate", [](VM& v, Object*, int, const Value* a) {
    std::string path = utf16ToUtf8(v.toStr(a[0]));
    std::vector<uint8_t> file;
    auto data = std::make_shared<GridData>();
    std::string err;
    if (!v.vfs().read(path, file) || !parseGridFile(file, *data, &err) || data->width < 2 || data->length < 2) {
      LOG_WARN("template %s: %s", path.c_str(), err.empty() ? "not found" : err.c_str());
      g_template.reset();
      return Value::boolean(false);
    }
    g_template = data;
    std::string base = path;
    if (base.size() > 14 && base.compare(base.size() - 14, 14, "/template.grid") == 0) base.resize(base.size() - 14);  // a .cimtpl
    std::string name = base.substr(base.find_last_of('/') + 1);
    g_templateName = name.substr(0, name.rfind('.'));
    return Value::boolean(true);
  }, 1, 1);

  // gsGrid.regsSaveTemplate(path): the selection as a template (see the file comment), and next to it <path>.dds,
  // its minimap picture (128 × 128) for the template list
  // regsSaveTemplate(path, [withTerrain = true]): without the terrain only the objects are kept (kNoTerrain)
  if (ClassInfo* c = vm.findClass("gsGrid")) addMethod(c, u"regsSaveTemplate", 1, 2);
  implement(vm, "gsGrid", u"regsSaveTemplate", [](VM& v, Object* s, int argc, const Value* a) {
    bool withTerrain = argc < 2 || v.truthy(a[1]);
    auto* grid = dynamic_cast<GridObj*>(s);
    if (!grid || !grid->data) return Value::boolean(false);
    const GridData& g = *grid->data;
    Rect r = g_area;
    r.x = std::max(0, r.x), r.z = std::max(0, r.z);
    r.w = std::min(r.w, g.width - 1 - r.x), r.l = std::min(r.l, g.length - 1 - r.z);
    if (!r.valid()) return Value::boolean(false);
    GridData t;
    t.width = r.w + 1, t.length = r.l + 1;
    t.cellSize = g.cellSize, t.bucketShift = g.bucketShift, t.heightBase = 0x200;
    int base = meanHeight(g, r);
    t.height.resize(size_t(t.width) * size_t(t.length));
    t.flags.resize(t.height.size());
    for (int z = 0; z < t.length; ++z)
      for (int x = 0; x < t.width; ++x) {
        size_t c = size_t(r.z + z) * size_t(g.width) + size_t(r.x + x), k = size_t(z) * size_t(t.width) + size_t(x);
        t.height[k] = withTerrain ? int16_t(int(g.height[c]) - base + 0x200) : kNoTerrain;
        t.flags[k] = withTerrain ? uint16_t(g.flags[c] & kGroundFlags) : uint16_t(0);
      }
    std::vector<uint32_t> items = withParts(g, g_items);
    std::vector<int64_t> index(g.items.size(), -1);
    std::map<uint16_t, uint16_t> typeMap;
    int shift = shiftOf(g);
    for (uint32_t i : items) {
      GridData::Item it = g.items[i];
      auto tm = typeMap.find(it.type);
      if (tm == typeMap.end()) {
        tm = typeMap.emplace(it.type, uint16_t(t.types.size())).first;
        t.types.push_back(g.types[it.type]);
      }
      it.type = tm->second;
      it.x = int16_t(it.x - r.x), it.z = int16_t(it.z - r.z);
      it.y = int16_t(it.y + shift - base + 0x200);
      it.parent = it.parent && it.parent <= index.size() && index[it.parent - 1] >= 0 ? uint32_t(index[it.parent - 1] + 1) : 0;
      it.flags &= ~(kItemTemp | kItemSelected | kItemNegative);
      index[i] = int64_t(t.items.size());
      t.items.push_back(it);
      t.names.push_back(i < g.names.size() ? g.names[i] : std::string());
    }
    std::string path = utf16ToUtf8(v.toStr(a[0]));
    bool ok = v.vfs().write(path, gridFileOf(t));
    // the preview: the template's own minimap, stretched to a square, as an uncompressed DDS (one level)
    if (ok) {
      const int P = 128;
      // a picture of its own in the minimap's colours: per cell the ground (grass, field, pavement, water), what
      // covers it (buildings, roads and tracks, trees) and a light from the north-west on the heights; the longer
      // side fills the square, the rest stays dark
      const int W = t.width - 1, L = t.length - 1;
      std::vector<uint32_t> cellCol(size_t(std::max(1, W)) * size_t(std::max(1, L)), 0xFF495226u);
      auto at = [&](int x, int z) -> uint32_t& { return cellCol[size_t(z) * size_t(W) + size_t(x)]; };
      for (int z = 0; z < L; ++z)
        for (int x = 0; x < W; ++x) {
          uint16_t f = t.flags[size_t(z) * size_t(t.width) + size_t(x)];
          uint32_t col = 0xFF495226u;
          if (f & 0x2000) col = 0xFF295383u;
          else if (f & 0x20) col = 0xFF6A5A18u;
          else if (f & 0x08) col = 0xFF7A766Eu;
          at(x, z) = col;
        }
      for (const GridData::Item& it : t.items) {
        auto tpl = grid->templates.find(t.types[it.type]);
        if (tpl == grid->templates.end()) continue;
        const auto& fv = tpl->second->fieldValues;
        auto fi = [&](const char16_t* n, int d) { auto i = fv.find(n); return i == fv.end() ? d : i->second.toInt(); };
        int type = fi(u"type", 0), w = std::max(1, fi(u"width", 1)), l = std::max(1, fi(u"length", 1));
        if (it.orient & 1) std::swap(w, l);
        uint32_t col = type == 0x13 ? 0xFF202C0Au : type < 0x13 ? 0xFF605D58u : 0xFF42391Cu;
        int r = type == 0x13 ? 1 : 0;
        for (int z = it.z - r; z < it.z + (r ? r + 1 : l); ++z)
          for (int x = it.x - r; x < it.x + (r ? r + 1 : w); ++x)
            if (x >= 0 && z >= 0 && x < W && z < L) at(x, z) = col;
      }
      int side = std::max(W, L);
      std::vector<uint8_t> dds;
      auto p32 = [&](uint32_t x) { dds.insert(dds.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4); };
      for (uint32_t x : {0x20534444u, 0x7Cu, 0x100Fu, uint32_t(P), uint32_t(P), uint32_t(P * 4), 0u, 1u}) p32(x);
      for (int i = 0; i < 11; ++i) p32(0);
      for (uint32_t x : {0x20u, 0x41u, 0u, 0x20u, 0xFF0000u, 0xFF00u, 0xFFu, 0xFF000000u, 0x1000u, 0u, 0u, 0u, 0u}) p32(x);
      for (int y = 0; y < P; ++y)
        for (int x = 0; x < P; ++x) {
          int sx = x * side / P, sy = y * side / P;
          uint32_t col = 0xFF202020u;
          if (sx < W && sy < L) {
            col = at(sx, sy);
            // the light: the slope towards the north-west brightens, away darkens
            auto hh = [&](int cx, int cz) {
              int16_t v2 = t.height[size_t(std::min(cz, L)) * size_t(t.width) + size_t(std::min(cx, W))];
              return v2 == kNoTerrain ? 0x200 : int(v2);
            };
            int d = (hh(sx, sy) - hh(sx + 1, sy + 1)) / 8;
            auto ch = [&](int sh) { return uint32_t(std::clamp(int((col >> sh) & 0xFF) + d, 0, 255)) << sh; };
            col = 0xFF000000u | ch(16) | ch(8) | ch(0);
          }
          p32(col);
        }
      std::string pre = path.substr(0, path.rfind('.')) + ".dds";
      v.vfs().write(pre, dds);
    }
    // <path>.info: what the template is and needs, as a script the game can load (the .cimtpl package's template.info)
    if (ok) {
      std::set<std::string> packages;
      for (const std::string& type : t.types)
        if (auto tp = grid->templates.find(type); tp != grid->templates.end())
          if (auto pk = tp->second->fieldValues.find(u"<package>"); pk != tp->second->fieldValues.end()) {
            std::string p = utf16ToUtf8(pk->second.toString());
            if (!p.empty()) packages.insert(p);
          }
      const std::string nl = "\r\n";
      std::string info = "// OpenCiM template (.cimtpl)" + nl + "$format = 1;" + nl;
      info += "$width = " + std::to_string(r.w) + ";" + nl + "$length = " + std::to_string(r.l) + ";" + nl;
      info += "$objects = " + std::to_string(t.items.size()) + ";" + nl;
      info += std::string("$terrain = ") + (withTerrain ? "true" : "false") + ";" + nl;
      info += "// the content packages of its objects (DLCs: addons/<folder>)" + nl + "$packages = [";
      bool first = true;
      for (const std::string& p : packages) {
        info += (first ? "\"" : ", \"") + p + "\"";
        first = false;
      }
      info += "];" + nl;
      v.vfs().write(path.substr(0, path.rfind('.')) + ".info", std::vector<uint8_t>(info.begin(), info.end()));
    }
    LOG_INFO("template %s: %dx%d cells, %zu objects%s", path.c_str(), r.w, r.l, t.items.size(), ok ? "" : " (not written)");
    return Value::boolean(ok);
  });
}

}  // namespace regs::engine
