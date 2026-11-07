// The map editor's terrain tools (docs/ui/construction.md §15): gsGridPlayer's brushes GRIDMODE_SHIFT_TERRAIN (2),
// LEVEL_TERRAIN (3), SOFTEN_TERRAIN (4), SET_PAVEMENT (0xf), SET_FIELD (0x10), RUIN (0x13), GROW_TREES (0xe) and
// TOGGLE_WATER (0xd). Decoded from the Linux build: the inline cases of gsGridPlayer::mouseMove (L:0x0819f2a0.c:118-506
// and :1373-1417), mouseDown (L:0x0819c410: the undo copy of shift/ruin, the trees' timeline group), finishTool
// (L:0x0819a3e0: the brush actions, the water toggle) and cancelTool (L:0x0819bcc0: shift/ruin restore their copy).
// Every edit is recorded on gsGridPlayer.timeline when the editor set it (engine/timeline.h).
#include "engine/tool_terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/gui.h"
#include "engine/timeline.h"
#include "engine/tools.h"

namespace regs::engine {

using namespace regs::script;

namespace {

constexpr uint16_t kCellPaved = 0x8, kCellField = 0x20, kCellLocked = 0x1000, kCellWater = 0x2000,
                   kCellHighlight = 0x4000;
constexpr int kTypeTree = 0x13;

int intField(VM& vm, const Object* o, const char16_t* name, int def) {
  auto it = o->fieldValues.find(name);
  return it == o->fieldValues.end() || it->second.isNone() ? def : int(vm.toInt(it->second));
}
int brushRadius(VM& vm, Object* p) { return std::clamp(intField(vm, p, u"toolSize", 0), 0, 60); }
int editMode(VM& vm, Object* p) { return intField(vm, p, u"editMode", 0); }

// the undo copies of gsGridPlayer (+0x1c0, a ring of 32 items; addUndoItem / executeUndoItem): the cells of a brush
// area as they were when the button went down
struct CellCopy {
  int x = 0, z = 0, w = 0, l = 0;
  std::vector<int16_t> height;
  std::vector<uint16_t> flags;
  std::vector<uint8_t> inside;  // the cell was on the map
};
std::unordered_map<const Object*, std::deque<CellCopy>> g_undo;

CellCopy copyCells(const GridData& g, int x, int z, int w, int l) {
  CellCopy c{x, z, w, l, {}, {}, {}};
  c.height.resize(size_t(w) * size_t(l));
  c.flags.resize(c.height.size());
  c.inside.resize(c.height.size());
  for (int dz = 0; dz < l; ++dz)
    for (int dx = 0; dx < w; ++dx) {
      int cx = x + dx, cz = z + dz;
      size_t k = size_t(dz) * size_t(w) + size_t(dx);
      if (cx < 0 || cz < 0 || cx >= g.width || cz >= g.length) continue;
      size_t cell = size_t(cz) * size_t(g.width) + size_t(cx);
      c.height[k] = g.height[cell];
      c.flags[k] = g.flags[cell];
      c.inside[k] = 1;
    }
  return c;
}

void restoreCells(GridObj* grid, const CellCopy& c) {
  GridData& g = *grid->data;
  for (int dz = 0; dz < c.l; ++dz)
    for (int dx = 0; dx < c.w; ++dx) {
      size_t k = size_t(dz) * size_t(c.w) + size_t(dx);
      if (!c.inside[k]) continue;
      size_t cell = size_t(c.z + dz) * size_t(g.width) + size_t(c.x + dx);
      g.height[cell] = c.height[k];
      g.flags[cell] = c.flags[k];
    }
  gridEditCellsChanged(grid, c.x - 1, c.z - 1, c.x + c.w, c.z + c.l, true);
}

uint16_t* cellAt(GridData& g, int x, int z) {
  if (x < 0 || z < 0 || x >= g.width || z >= g.length) return nullptr;
  return &g.flags[size_t(z) * size_t(g.width) + size_t(x)];
}
int16_t* heightAt(GridData& g, int x, int z) {
  if (x < 0 || z < 0 || x >= g.width || z >= g.length) return nullptr;
  return &g.height[size_t(z) * size_t(g.width) + size_t(x)];
}

// the brush's cosine falloff: 1 at the centre, 0 at the rim (sin((d²/r² + 0.5)·π)·0.5 + 0.5)
float falloff(int d2, int r) {
  float r2 = r * r > 0 ? float(r * r) : 1.0f;
  return float(std::sin(double((float(d2) / r2 + 0.5f) * 3.1415927f))) * 0.5f + 0.5f;
}

// the highlight of a painted brush cell (flag 0x4000) the overlay draws; unselectArea clears it
void highlightCell(GridObj* grid, int x, int z) {
  GridData& g = *grid->data;
  uint16_t* f = cellAt(g, x, z);
  if (!f) return;
  *f |= kCellHighlight;
  grid->highlightCells.insert(uint32_t(size_t(z) * size_t(g.width) + size_t(x)));
}

// ---- hover and the click brushes: modes 3, 4, 0xf, 0x10 (and 2, 0xe, 0x13 while no drag runs) ----
// L:0x0819f2a0.c:343-506: pick the cell; when it changed, the old highlight goes, a held tool works the disc (level,
// soften, paint), the timeline gets the dab, and the disc (radius r + 1, clipped to r) is highlighted.
void moveBrush(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  ToolState& t = toolState(player);
  ToolPick p = toolPick(vm, player, x, y, false);
  if (!p.hit || (p.cellX == t.cellX && p.cellZ == t.cellZ)) return;
  GridData& g = *grid->data;
  const int mode = editMode(vm, player), r = brushRadius(vm, player), toolMode = intField(vm, player, u"toolMode", 0);
  const int px = p.cellX, pz = p.cellZ;
  gridEditBegin(vm, grid);
  if (t.cellX != -1) gridUnselectArea(vm, grid, t.cellX - r, t.cellZ - r, 2 * r + 1, 2 * r + 1);
  if (t.inProgress && mode == kGridModeSoftenTerrain) toolSoftenArea(grid, px, pz, r);
  if (Object* tl = playerTimeline(vm, player)) {
    if (mode == kGridModeSetPavement) {
      if (t.inProgress) timelineRecordBrush(vm, tl, toolMode + 1, r, px, 0, pz);
      else if (t.right) timelineRecordBrush(vm, tl, 9, r, px, 0, pz);
    } else if (mode == kGridModeSetField) {
      if (t.inProgress) timelineRecordBrush(vm, tl, toolMode + 10, r, px, 0, pz);
      else if (t.right) timelineRecordBrush(vm, tl, 0x12, r, px, 0, pz);
    } else if (mode == kGridModeLevelTerrain && t.inProgress) {
      timelineRecordBrush(vm, tl, 0x13, r, px, t.cellY, pz);
    } else if (mode == kGridModeSoftenTerrain && t.inProgress) {
      timelineRecordBrush(vm, tl, 0x15, r, px, 0, pz);
    }
  }
  bool heights = false;
  for (int dz = -r; dz <= r + 1; ++dz)
    for (int dx = -r; dx <= r + 1; ++dx) {
      uint16_t* f = cellAt(g, px + dx, pz + dz);
      if (!f) continue;
      int ex = dx - (dx > 0), ez = dz - (dz > 0), d2 = ex * ex + ez * ez;
      if (d2 <= r * r && !(*f & kCellLocked) && t.inProgress && mode == kGridModeLevelTerrain) {
        int16_t* h = heightAt(g, px + dx, pz + dz);
        *h = int16_t(int(std::nearbyint(falloff(d2, r) * float(t.cellY - *h))) + *h);
        heights = true;
      }
      if (dx * dx + dz * dz > (r + 1) * (r + 1) || dz > r || dx > r) continue;
      uint16_t v = *f;
      if (t.inProgress) {
        if (mode == kGridModeSetPavement) v = uint16_t((v & 0xFFD8) | (toolMode & 7) | kCellPaved);
        else if (mode == kGridModeSetField) v = uint16_t((v & 0xFFF0) | (toolMode & 7) | kCellField);
      }
      if (t.right) {
        if (mode == kGridModeSetPavement) v &= 0xFFF0;
        else if (mode == kGridModeSetField) v &= 0xFFD8;
      }
      *f = v;
      highlightCell(grid, px + dx, pz + dz);
    }
  ++grid->highlightVersion;
  gridEditCellsChanged(grid, px - r - 1, pz - r - 1, px + r + 2, pz + r + 2, heights);
  gridEditEnd(vm, grid);
  t.cellX = px, t.cellZ = pz;
  if (!t.inProgress)  // the height the level brush will pull to: the cell's, rounded to half a cell
    if (int16_t* h = heightAt(g, px, pz)) t.cellY = (int(*h) + 0x40) & ~0x7f;
}

// ---- the dragged brushes: shift (2) and ruin (0x13) from their undo copy, grow trees (0xe) ----
// L:0x0819f2a0.c:118-341: the strength follows the mouse from the press point (trees: from the last move), rounded down
// to half a cell; the disc around the press cell is worked.
void growTrees(VM& vm, ControlObj* player, GridObj* grid, int sx, int sz, int r);

void moveDrag(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  ToolState& t = toolState(player);
  if (!t.inProgress) {
    moveBrush(vm, player, grid, x, y);
    return;
  }
  GridData& g = *grid->data;
  const int mode = editMode(vm, player), r = brushRadius(vm, player);
  t.cellY = int(std::lrint((t.toolY - y) * 16.384f + 64.0f)) & ~0x7f;
  const int sx = t.startX, sz = t.startZ;
  gridEditBegin(vm, grid);
  if (mode == kGridModeShiftTerrain || mode == kGridModeRuin) {
    auto& undo = g_undo[player];
    if (!undo.empty()) restoreCells(grid, undo.back());
    for (int dz = -r; dz <= r + 1; ++dz)
      for (int dx = -r; dx <= r + 1; ++dx) {
        uint16_t* f = cellAt(g, sx + dx, sz + dz);
        if (!f || (*f & kCellLocked)) continue;
        int ex = dx - (dx > 0), ez = dz - (dz > 0), d2 = ex * ex + ez * ez;
        if (d2 > r * r) continue;
        float k = falloff(d2, r);
        if (mode == kGridModeShiftTerrain) {
          int16_t* h = heightAt(g, sx + dx, sz + dz);
          *h = int16_t(std::max(-0x600, int(std::lrint(float(t.cellY) * k)) + *h));
        } else if (!(*f & (kCellPaved | kCellField))) {
          int wear = int(std::lrint(float(t.cellY) * k * 0.0001f * float((std::rand() & 0x1f) + 0x44))) + (*f & 7);
          *f = uint16_t((*f & 0xFFF8) | (wear >= 7 ? 7 : wear < 0 ? 0 : wear));
        }
      }
    gridEditCellsChanged(grid, sx - r - 1, sz - r - 1, sx + r + 2, sz + r + 2, mode == kGridModeShiftTerrain);
  } else {
    growTrees(vm, player, grid, sx, sz, r);
    t.toolY = y;
  }
  gridEditEnd(vm, grid);
  t.cellX = sx, t.cellZ = sz;
}

// L:0x0819f2a0.c:160-330: per cell of the disc, by chance: pull mouse (strength > 0) plants a tree of toolObject where
// no tree stands within 1 (1 in 8) or 2 cells, which stays when it does not conflict (OPTIONAL); push removes trees
void growTrees(VM& vm, ControlObj* player, GridObj* grid, int sx, int sz, int r) {
  ToolState& t = toolState(player);
  GridData& g = *grid->data;
  Object* tl = playerTimeline(vm, player);
  auto tplIt = player->fieldValues.find(u"toolObject");
  Object* tpl = tplIt == player->fieldValues.end() ? nullptr : vm.resolve(tplIt->second).object();
  std::string name;
  if (tpl)
    if (auto n = tpl->fieldValues.find(u"idName"); n != tpl->fieldValues.end()) name = utf16ToUtf8(n->second.toString());
  auto isTree = [&](uint32_t i) { return gridItemShape(vm, grid, i).type == kTypeTree; };
  for (int dz = -r; dz <= r; ++dz)
    for (int dx = -r; dx <= r; ++dx) {
      int cx = sx + dx, cz = sz + dz;
      uint16_t* f = cellAt(g, cx, cz);
      int d2 = dx * dx + dz * dz;
      if (!f || d2 > r * r || (*f & (kCellLocked | kCellPaved | kCellField))) continue;
      int s = t.cellY, amount = s < 0 ? -0xFFFF : (s != 0 ? 0xFFFF : 0);
      if (r != 0)
        amount = int(std::lrint(double(s) * (std::sin((double(float(d2) / float(r * r)) + 0.4) * 3.1415927) * 0.5 + 0.5)));
      if (amount < 1) {
        if ((std::rand() & 0x3ff) >= -amount) continue;
        for (uint32_t i : gridGetObjects(vm, grid, cx, cz, 1, 1)) {
          if (!isTree(i)) continue;
          timelineRecordDelete(vm, tl, grid, i);
          gridEditRemoveItem(vm, grid, i);
          break;
        }
      } else {
        if ((std::rand() & 0x3fff) >= amount || (*f & kCellWater) || name.empty()) continue;
        bool near = (std::rand() & 7) == 0;
        bool blocked = false;
        for (uint32_t i : near ? gridGetObjects(vm, grid, cx - 1, cz - 1, 3, 3) : gridGetObjects(vm, grid, cx - 2, cz - 2, 5, 5))
          if (isTree(i)) blocked = true;
        if (blocked) continue;
        // on the ground: the mean of the cell's corners (gsGridItem::fixY for a 1×1 item)
        int sum = 0, n = 0;
        for (int oz = 0; oz <= 1; ++oz)
          for (int ox = 0; ox <= 1; ++ox)
            if (int16_t* h = heightAt(g, std::min(cx + ox, g.width - 1), std::min(cz + oz, g.length - 1))) sum += *h, ++n;
        int item = gridEditAddItem(vm, grid, name, cx, n ? sum / n : 0, cz, std::rand() & 3, 0, -1, false);
        if (item < 0) continue;
        if (gridCheckConflict(vm, grid, uint32_t(item), {})) {
          gridEditRemoveItem(vm, grid, uint32_t(item));
          continue;
        }
        g.items[size_t(item)].flags |= kItemOptional;
        gridEditItemChanged(grid, uint32_t(item));
        timelineRecordCreate(vm, tl, grid, uint32_t(item));
      }
    }
}

// mouseDown for shift and ruin (L:0x0819c410.c:68-130): the strength starts at 0 and the area (2r + 2)² around the
// hovered cell is copied for the drag and the cancel
void downCopy(VM& vm, ControlObj* player, GridObj* grid) {
  ToolState& t = toolState(player);
  t.cellY = 0;
  int r = brushRadius(vm, player);
  auto& undo = g_undo[player];
  undo.push_back(copyCells(*grid->data, t.cellX - r, t.cellZ - r, 2 * r + 2, 2 * r + 2));
  if (undo.size() > 32) undo.pop_front();
}

// finishTool cases 2 and 0x13 (L:0x0819a3e0.c:115-122, 667-683): the drag's dab goes to the timeline (shift 0x14,
// ruin 0x16) when it moved the terrain; the highlight goes
bool finishDrag(VM& vm, ControlObj* player, GridObj* grid) {
  if (!grid) return true;
  ToolState& t = toolState(player);
  int mode = editMode(vm, player), r = brushRadius(vm, player);
  if (t.cellY != 0)
    if (Object* tl = playerTimeline(vm, player))
      timelineRecordBrush(vm, tl, mode == kGridModeShiftTerrain ? 0x14 : 0x16, r, t.cellX, t.cellY, t.cellZ);
  gridEditBegin(vm, grid);
  gridUnselectArea(vm, grid, t.cellX - r, t.cellZ - r, 2 * r + 2, 2 * r + 2);
  gridEditEnd(vm, grid);
  return true;
}

// cancelTool cases 2 and 0x13 (L:0x0819bcc0.c:36-48): a running drag puts its copy back
void cancelDrag(VM& vm, ControlObj* player, GridObj* grid) {
  ToolState& t = toolState(player);
  auto& undo = g_undo[player];
  if (t.inProgress && !undo.empty() && grid && grid->data) {
    gridEditBegin(vm, grid);
    restoreCells(grid, undo.back());
    gridEditEnd(vm, grid);
    undo.pop_back();
  }
  t.cellY = 0;
}

// finishTool cases 3, 4, 0xf, 0x10 (L:0x0819a3e0.c:123-133): only the highlight goes
bool finishBrush(VM& vm, ControlObj* player, GridObj* grid) {
  if (!grid) return true;
  ToolState& t = toolState(player);
  int r = brushRadius(vm, player);
  gridEditBegin(vm, grid);
  gridUnselectArea(vm, grid, t.cellX - r, t.cellZ - r, 2 * r + 2, 2 * r + 2);
  gridEditEnd(vm, grid);
  return true;
}

// finishTool case 0xe (L:0x0819a3e0.c:661-666): a drag closes its timeline group, then as the brushes
bool finishTrees(VM& vm, ControlObj* player, GridObj* grid) {
  if (toolState(player).inProgress)
    if (Object* tl = playerTimeline(vm, player)) timelineEndGroup(vm, tl);
  return finishBrush(vm, player, grid);
}

// ---- toggle water (0xd) ----
// mouseMove L:0x0819f2a0.c:1373-1417: the cell under the cursor is highlighted, red when no corner is below the water
// line; finishTool L:0x0819a3e0.c:621-655 (on press): such a cell toggles the water of its whole basin
void moveWater(VM& vm, ControlObj* player, GridObj* grid, float x, float y) {
  ToolState& t = toolState(player);
  ToolPick p = toolPick(vm, player, x, y, false);
  if (!p.hit || (p.cellX == t.cellX && p.cellZ == t.cellZ)) return;
  GridData& g = *grid->data;
  gridEditBegin(vm, grid);
  if (t.cellX != -1) gridUnselectArea(vm, grid, t.cellX, t.cellZ, 1, 1);
  int16_t *a = heightAt(g, p.cellX, p.cellZ), *b = heightAt(g, p.cellX + 1, p.cellZ), *c = heightAt(g, p.cellX, p.cellZ + 1),
          *d = heightAt(g, p.cellX + 1, p.cellZ + 1);
  if (a && b && c && d) {
    bool dry = *a > -0x141 && *b > -0x141 && *c > -0x141 && *d > -0x141;
    gridSelectArea(vm, grid, p.cellX, p.cellZ, 1, 1, dry);
  }
  gridEditEnd(vm, grid);
  t.cellX = p.cellX, t.cellZ = p.cellZ;
}

bool finishWater(VM& vm, ControlObj* player, GridObj* grid) {
  ToolState& t = toolState(player);
  if (!grid || !grid->data || t.cellX == -1 || t.cellZ == -1) return true;
  GridData& g = *grid->data;
  int16_t *a = heightAt(g, t.cellX, t.cellZ), *b = heightAt(g, t.cellX + 1, t.cellZ), *c = heightAt(g, t.cellX, t.cellZ + 1),
          *d = heightAt(g, t.cellX + 1, t.cellZ + 1);
  gridEditBegin(vm, grid);
  if (a && b && c && d && (*a < -0x140 || *b < -0x140 || *c < -0x140 || *d < -0x140)) {
    bool on = !(*cellAt(g, t.cellX, t.cellZ) & kCellWater);
    if (Object* tl = playerTimeline(vm, player)) timelineRecordWater(vm, tl, t.cellX, t.cellZ, on);
    gridSetWaterFlag(grid, t.cellX, t.cellZ, on);
  }
  int r = brushRadius(vm, player);
  gridUnselectArea(vm, grid, t.cellX - r, t.cellZ - r, 2 * r + 2, 2 * r + 2);
  gridEditEnd(vm, grid);
  return true;
}

void noCancel(VM&, ControlObj*, GridObj*) {}

}  // namespace

void registerTerrainTools(VM&) {
  ToolMode brush;  // level, soften, pavement, field
  brush.move = moveBrush;
  brush.finish = finishBrush;
  brush.cancel = noCancel;
  for (int m : {kGridModeLevelTerrain, kGridModeSoftenTerrain, kGridModeSetPavement, kGridModeSetField}) registerToolMode(m, brush);
  ToolMode drag;  // shift, ruin
  drag.move = moveDrag;
  drag.finish = finishDrag;
  drag.cancel = cancelDrag;
  drag.down = downCopy;
  registerToolMode(kGridModeShiftTerrain, drag);
  registerToolMode(kGridModeRuin, drag);
  ToolMode trees;
  trees.move = moveDrag;
  trees.finish = finishTrees;
  trees.cancel = noCancel;
  trees.down = [](VM& vm, ControlObj* player, GridObj*) {
    if (Object* tl = playerTimeline(vm, player)) timelineBeginGroup(vm, tl, "%1");
  };
  registerToolMode(kGridModeGrowTrees, trees);
  ToolMode water;
  water.move = moveWater;
  water.finish = finishWater;
  water.cancel = noCancel;
  registerToolMode(kGridModeToggleWater, water);
}

}  // namespace regs::engine
