// GRIDMODE_ADD_STOP (9), docs/ui/lines.md §2: gsGridPlayer::mouseMove case 9 (L:0x0819f2a0.c:599-733), finishTool
// case 9 (L:0x0819a3e0.c:554-618) and cancelTool case 9 (L:0x0819bcc0.c:194-197). The hovered stop joins the line as
// a temporary stop (node flag 1) and is highlighted; a click (onTryApplyTool → applyTool → finishTool) keeps it; moving
// on or leaving the mode drops it.
#include "engine/tool_addstop.h"

#include <cmath>
#include <cstdlib>

#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/gui.h"
#include "engine/interaction.h"
#include "engine/sim.h"
#include "engine/tools.h"

namespace regs::engine {

using namespace regs::script;

namespace {

constexpr uint32_t kFlagStop = 0x2000;  // GRID_ITEM_STOP

Value obj(Object* o) { return o ? Value::heap(VT::Object, o) : Value::null(); }

Object* toolLine(Object* player) {
  auto it = player->fieldValues.find(u"toolLine");
  Object* l = it == player->fieldValues.end() ? nullptr : it->second.object();
  return l && !l->destroyed && simLineType(l) >= 0 ? l : nullptr;
}

int intField(VM& vm, const Object* o, const char16_t* name, int def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : vm.toInt(it->second);
}

// the stop item types a line type serves in the original (GRID_ITEM_*: bus stops 2-6, tram 10-12, metro 13-15, dock
// 0x11, heliport 0x12); isStopOf adds the port's street tram stops for buses
bool servesType(int lineType, int itemType) {
  switch (lineType) {
    case 0: return itemType >= 2 && itemType <= 6;
    case 1: return itemType >= 10 && itemType <= 12;
    case 2: return itemType >= 13 && itemType <= 15;
    case 3: return itemType == 0x11;
    case 4: return itemType == 0x12;
    default: return false;
  }
}

bool isStopOf(VM& vm, GridObj* grid, uint32_t item, int lineType) {
  if (!grid->data || item >= grid->data->items.size() || grid->data->items[item].removed) return false;
  GridItemShape sh = gridItemShape(vm, grid, item);
  // port: a train station's track serves tram lines (trams on railway tracks)
  const GridData& g = *grid->data;
  if (lineType == 1 && g.items[item].type < g.types.size() && simIsTrainStation(g.types[g.items[item].type])) return true;
  if (!(sh.flags & kFlagStop)) return false;
  if (servesType(lineType, sh.type)) return true;
  // port QoL (docs/ui/lines.md §6, settings regsBusAtTramStops): a bus line also takes a tram stop in a street, one
  // with a road lane through its platform where a bus can halt; not one on its own track or an avenue's middle strip
  return lineType == 0 && servesType(1, sh.type) && simBusAtTramStops(vm) && simBusPlatformAtTramStop(vm, grid, int(item)) >= 0;
}

void unselectItem(VM& vm, GridObj* grid, int item) {
  if (item < 0 || !grid->data || size_t(item) >= grid->data->items.size()) return;
  const GridData::Item& it = grid->data->items[size_t(item)];
  GridItemShape sh = gridItemShape(vm, grid, uint32_t(item));
  gridUnselectArea(vm, grid, it.x, it.z, sh.sx, sh.sz);
}

// mouseMove case 9: the stop under the mouse joins the line as a temporary stop, event 26 onHoverAddStop(controller)
void addStopMove(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  Object* line = toolLine(player);
  if (!line || !grid->data) return;
  const GridData& g = *grid->data;
  int lineType = intField(vm, player, u"toolMode", 0);  // selectTool(GRIDMODE_ADD_STOP, line.type)
  ToolState& t = toolState(player);
  // the stop whose tracer the mouse is over (ui.script sets hoverObject), if it is one of the line's type
  int item = -1, cx = -1, cz = -1, pickY = -0x8000;
  Value& hv = player->fieldValues[u"hoverObject"];
  if (Object* h = hv.object()) {
    if (h->destroyed) {
      hv = Value::null();
    } else if (auto at = h->fieldValues.find(u"<item>"); at != h->fieldValues.end()) {
      int hi = vm.toInt(at->second);
      if (hi >= 0 && isStopOf(vm, grid, uint32_t(hi), lineType)) {
        GridItemShape sh = gridItemShape(vm, grid, uint32_t(hi));
        item = hi;
        cx = g.items[size_t(hi)].x + (sh.sx >> 1);
        cz = g.items[size_t(hi)].z + (sh.sz >> 1);
      }
    }
  }
  if (item < 0) {  // else the cell under the mouse: intersectGrid with items (terrainOnly false, L:0x081402f0 :278), the
                   // item hit not reported, only its cell and height
    ToolPick p = toolPick(vm, player, x, y, false);
    if (!p.hit) return;
    cx = p.cellX, cz = p.cellZ;
    pickY = int(std::lround(p.point.y * 256.0f / g.cellSize));
  }
  if (cx == t.cellX && cz == t.cellZ) return;
  for (int i : simLineTempStopItems(line)) unselectItem(vm, grid, i);
  simLineRemoveTempStops(vm, line);
  if (item < 0) {
    // the stop items overlapping the 3×3 cells around it (gsGrid::getObjects L:0x0813fdb0: non-temporary items of the
    // divisions around whose footprint overlaps the rectangle); the one nearest in height wins, the first on a tie
    int bestDy = 0;
    for (uint32_t i : gridItemsNear(vm, grid, cx - 1, cz - 1, cx + 1, cz + 1)) {
      const GridData::Item& it = g.items[i];
      if (it.removed || !isStopOf(vm, grid, i, lineType)) continue;
      GridItemShape sh = gridItemShape(vm, grid, i);
      if ((sh.flags & 0x100000) || it.x > cx + 1 || it.x + sh.sx <= cx - 1 || it.z > cz + 1 || it.z + sh.sz <= cz - 1) continue;
      int dy = std::abs(it.y - pickY);
      if (item < 0 || dy < bestDy) item = int(i), bestDy = dy;
    }
  }
  Object* hovered = nullptr;
  if (item >= 0 && simLineAddStop(vm, line, item, true) >= 0) {
    gridSetSelected(vm, grid, uint32_t(item), true, false);
    hovered = gridItemController(vm, grid, uint32_t(item));
  }
  vm.fireEvent(player, u"onHoverAddStop", {obj(hovered)});
  t.cellX = cx, t.cellZ = cz;
}

// finishTool case 9: the temporary stops stay (unless a leg to one has no path), event 26 with nothing, and 25
// onAddStop when a stop was added (the engine fires it after the common tail)
bool addStopFinish(VM& vm, ControlObj* player, GridObj* grid) {
  Object* line = toolLine(player);
  bool added = false;
  if (line && grid) {
    std::vector<int> unselect;
    added = simLineCommitTempStops(vm, line, unselect);
    for (int i : unselect) unselectItem(vm, grid, i);
  }
  vm.fireEvent(player, u"onHoverAddStop", {Value::null()});
  if (added) vm.fireEvent(player, u"onAddStop", {});
  return true;
}

// cancelTool case 9: the temporary stops go. Port: their highlight goes too (the engine leaves the item flag 0x100
// set until something unselects that area)
void addStopCancel(VM& vm, ControlObj* player, GridObj* grid) {
  Object* line = toolLine(player);
  if (!line) return;
  if (grid)
    for (int i : simLineTempStopItems(line)) unselectItem(vm, grid, i);
  simLineRemoveTempStops(vm, line);
}

}  // namespace

void registerAddStopTool(VM& vm) {
  (void)vm;
  ToolMode m;
  m.move = addStopMove;
  m.finish = addStopFinish;
  m.cancel = addStopCancel;
  registerToolMode(kGridModeAddStop, m);
}

}  // namespace regs::engine
