// The map editor's templates (port, owner's request 2026-11-07): select an area, save its terrain and objects as a
// template, and stamp it somewhere else. Two tool modes and a few natives (script side:
// data/regs/extend/metro/scripts/editor.script):
// * kModeSelectArea: drag a rectangle (left button); on release it is kept for regsTakeSelectedArea().
// * gsGrid.regsSaveTemplate(path, x, z, w, l): the cells' heights and ground flags and the objects lying fully inside
//   (with their attached parts) into a small map.grid. Heights are stored relative to the area's mean height (rounded
//   to half a cell, so buildings stay aligned): the template's ground level is 0x200.
// * regsSetTemplate(path): the template the place mode stamps; kModePlaceTemplate shows its outline under the cursor
//   and, on click, replaces the area with it: the objects touching it go (shown red beforehand), the terrain takes the template's
//   heights around the target area's mean height, the template's objects are added. Everything is recorded on the
//   timeline as one group (a terrain patch action, deletions, creations), so undo and saving work as for any edit.
#include "engine/tool_templates.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/grid_save.h"
#include "engine/gui.h"
#include "engine/interaction.h"
#include "engine/timeline.h"
#include "engine/tools.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

namespace {

constexpr int kModeSelectArea = 0x40, kModePlaceTemplate = 0x41;
constexpr uint32_t kGroundFlags = 0x203F;  // ground style, pavement, field, water

struct Rect {
  int x = 0, z = 0, w = 0, l = 0;
  bool valid() const { return w > 0 && l > 0; }
};

Rect g_shown;                       // the highlighted rectangle (both modes)
Rect g_selected;                    // the last finished selection, until the script takes it
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
void moveSelect(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  ToolState& t = toolState(player);
  ToolPick p = toolPick(vm, player, x, y, true);
  if (!p.hit) return;
  Rect r{p.cellX, p.cellZ, 1, 1};
  if (t.inProgress && t.startX >= 0) {
    r.x = std::min(t.startX, p.cellX), r.z = std::min(t.startZ, p.cellZ);
    r.w = std::abs(p.cellX - t.startX) + 1, r.l = std::abs(p.cellZ - t.startZ) + 1;
  }
  if (r.x == g_shown.x && r.z == g_shown.z && r.w == g_shown.w && r.l == g_shown.l) return;
  show(vm, grid, r, false);
  t.cellX = p.cellX, t.cellZ = p.cellZ;
}

void downSelect(VM&, ControlObj* player, GridObj*) {
  ToolState& t = toolState(player);
  t.startX = t.cellX, t.startZ = t.cellZ;
}

void upSelect(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  moveSelect(vm, player, grid, x, y);
  g_selected = g_shown;
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
  select.cancel = cancelBoth;
  select.finish = finishBoth;
  registerToolMode(kModeSelectArea, select);
  ToolMode place;
  place.move = movePlace;
  place.up = upPlace;
  place.cancel = cancelBoth;
  place.finish = finishBoth;
  registerToolMode(kModePlaceTemplate, place);
  vm.setGlobal(u"REGS_GRIDMODE_SELECT_AREA", Value::integer(kModeSelectArea));
  vm.setGlobal(u"REGS_GRIDMODE_PLACE_TEMPLATE", Value::integer(kModePlaceTemplate));

  // regsTakeSelectedArea(): [x, z, w, l] of the last finished selection (once), or null
  implementGlobal(vm, u"regsTakeSelectedArea", [](VM&, Object*, int, const Value*) {
    if (!g_selected.valid()) return Value::null();
    auto a = make<ArrayObj>();
    for (int v : {g_selected.x, g_selected.z, g_selected.w, g_selected.l}) a->items.push_back(Value::integer(v));
    g_selected = Rect{};
    return Value::heap(VT::Array, a.get());
  }, 0, 0);

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
    std::string name = path.substr(path.find_last_of('/') + 1);
    g_templateName = name.substr(0, name.rfind('.'));
    return Value::boolean(true);
  }, 1, 1);

  // gsGrid.regsSaveTemplate(path, x, z, w, l): the area as a template (see the file comment)
  if (ClassInfo* c = vm.findClass("gsGrid")) addMethod(c, u"regsSaveTemplate", 5, 5);
  implement(vm, "gsGrid", u"regsSaveTemplate", [](VM& v, Object* s, int, const Value* a) {
    auto* grid = dynamic_cast<GridObj*>(s);
    if (!grid || !grid->data) return Value::boolean(false);
    const GridData& g = *grid->data;
    Rect r{v.toInt(a[1]), v.toInt(a[2]), v.toInt(a[3]), v.toInt(a[4])};
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
        t.height[k] = int16_t(int(g.height[c]) - base + 0x200);
        t.flags[k] = uint16_t(g.flags[c] & kGroundFlags);
      }
    std::vector<uint32_t> items = itemsInside(v, grid, r);
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
    LOG_INFO("template %s: %dx%d cells, %zu objects%s", path.c_str(), r.w, r.l, t.items.size(), ok ? "" : " (not written)");
    return Value::boolean(ok);
  });
}

}  // namespace regs::engine
