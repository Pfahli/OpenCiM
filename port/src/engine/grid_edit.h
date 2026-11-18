#pragma once
// Live edits of a loaded map: the city timeline now, the construction tools later (gsGrid::addObject / removeObject
// L:0x08148680 / L:0x08147c60, beginUpdate / endUpdate, beginHoldUpdates / endHoldUpdates). Edits are batched; when the
// outermost batch ends, everything derived from the map is brought up to date: cell cover and normal overrides, the
// division meshes, the picking buckets (GridObj::itemsVersion), item controllers of removed items, and the simulation
// (simItemsChanged / simTerrainChanged in sim.h). Item indices stay stable: removed items are tombstones
// (GridData::Item::removed) until the map is saved and reloaded.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "script/vm.h"

namespace regs::engine {

class GridObj;

void gridEditBegin(script::VM& vm, GridObj* grid);
void gridEditEnd(script::VM& vm, GridObj* grid);
// holds the simulation's share of the edits back (the OSM import on a big map: a rebuild of the walk areas after every
// slice took a minute each, owner 2026-11-15); off again: everything held back goes to the simulation at once
void gridEditHoldSimulation(script::VM& vm, GridObj* grid, bool hold);

// gsGrid::addObject(template, x, y, z, orient, flags, parent): a new item of the named template, y in the engine's
// 1/256 cells (as items and ObjRefs carry it), parent an item index or −1. Optional items (OPTIONAL 0x200000) under a
// new covering item are removed as removeOverlappingOptionalObjects does. Returns the new index, or −1 for an unknown
// template.
int gridEditAddItem(script::VM& vm, GridObj* grid, const std::string& templateName, int x, int y, int z, int orient,
                    uint32_t flags, int parent, bool removeOptional = true);
// gsGrid::removeObject: the item and its non-temporary children (auto-generated pillars, attached details).
void gridEditRemoveItem(script::VM& vm, GridObj* grid, uint32_t item);
// an item's look changed (variation, flags): its division is rebuilt
void gridEditItemChanged(GridObj* grid, uint32_t item);
// terrain cells [x0, x1] × [z0, z1] (inclusive) whose heights (heights = true), water or ground flags changed
void gridEditCellsChanged(GridObj* grid, int x0, int z0, int x1, int z1, bool heights);

// gsGrid::setWaterFlag (L:0x08142090): sets or clears the water flag of cell (x, z) and floods it over the
// 4-connected cells that differ and have a corner below the water line (−0x140): the whole basin fills or drains.
// Returns true when a cell changed (the cells are queued as changed with heights, which rebuilds the water).
bool gridSetWaterFlag(GridObj* grid, int x, int z, bool on);
// port: the round brush (owner's request 2026-11-07: the original's brushes work a disc of whole cells around a cell
// corner with a clipped rim, which leaves jagged edges). Centre (cx, cz) in corner coordinates (fractional; corner i
// lies at x = i), radius R in cells; each corner's weight is smootherstep(1 − d/R), so the edit eases out to nothing
// at the rim. type is the original's brush type (1-8 pavement, 9 no pavement, 10-17 field, 18 no field, 19 level to
// `strength`, 20 shift by `strength`, 21 soften, 22 ruin); field paints take the cells whose centre is inside the circle; the
// pavement paint and its eraser cut the cells on the circle along it (engine/paint_edges.h paintCircle).
// Heights skip locked corners (0x1000); water stays only where a corner is still below the water line. preview: only
// the pavement paints mark their cells (0x40), as the timeline's previews. The cells are queued as changed.
// shape: the brush shape (engine/paint_edges.h kBrushCircle / Square / Diamond): its distance makes the weights and
// decides the cells, the pavement paints cut along its rim
// from (pavement paints 1-9 only): the stroke's previous dab; the paint covers the shape's sweep from there to (cx, cz)
// (the convex hull of the shape at both points), so a square stroke gets straight diagonal sides
void gridRoundBrush(GridObj* grid, int type, float R, float cx, float cz, int strength, bool preview, int shape = 0,
                    const float* from = nullptr);
// the cells whose centre lies inside the round brush (for the highlight)
std::vector<std::pair<int, int>> gridRoundBrushCells(GridObj* grid, float R, float cx, float cz, int shape = 0);
// the live items whose footprint corner is cell (x, z) (gsGrid::getObjects slot search for ObjRef matching)
std::vector<uint32_t> gridItemsAt(script::VM& vm, GridObj* grid, int x, int z);
// the live items whose footprint corner lies in the divisions around [x0, x1] × [z0, z1], grown by one division (the
// engine's neighbourhood for conflicts)
std::vector<uint32_t> gridItemsNear(script::VM& vm, GridObj* grid, int x0, int z0, int x1, int z1);
// template facts the edits need, from the grid's registered gsGridObject (name → index into GridData::types, −1 if
// the template is unknown)
int gridTypeIndex(script::VM& vm, GridObj* grid, const std::string& templateName);
struct GridItemShape {
  int sx = 1, sz = 1, type = 0, height = 0;  // footprint after orientation, GRID_ITEM type, height in 1/256 cells
  uint32_t flags = 0;                        // item | template flags
};
GridItemShape gridItemShape(script::VM& vm, GridObj* grid, uint32_t item);
// gsGridItem::checkConflict(a, b), simplified (docs/simulation/grid-traffic.md, port notes): two live items conflict
// when their covered cells overlap at overlapping heights, unless they are related, markers, hidden previews or
// optional items under previews, or on different layers (tunnel vs. surface)
bool gridItemsConflict(script::VM& vm, GridObj* grid, uint32_t a, uint32_t b);
// REGS_DEBUG_CONFLICTS=1: logs the conflicts gridItemsConflict finds among a loaded map's own items (once)
void gridDebugConflicts(script::VM& vm, GridObj* grid);

// gsGrid.findItem / getRandomItem over the live items
void registerGridEditNatives(script::VM& vm);

}  // namespace regs::engine
