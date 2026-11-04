#pragma once
// Live edits of a loaded map: the city timeline now, the construction tools later (gsGrid::addObject / removeObject
// L:0x08148680 / L:0x08147c60, beginUpdate / endUpdate, beginHoldUpdates / endHoldUpdates). Edits are batched; when the
// outermost batch ends, everything derived from the map is brought up to date: cell cover and normal overrides, the
// division meshes, the picking buckets (GridObj::itemsVersion), item controllers of removed items, and the simulation
// (simItemsChanged / simTerrainChanged in sim.h). Item indices stay stable: removed items are tombstones
// (GridData::Item::removed) until the map is saved and reloaded.
#include <cstdint>
#include <string>
#include <vector>

#include "script/vm.h"

namespace regs::engine {

class GridObj;

void gridEditBegin(script::VM& vm, GridObj* grid);
void gridEditEnd(script::VM& vm, GridObj* grid);

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

// gsGrid.findItem / getRandomItem over the live items
void registerGridEditNatives(script::VM& vm);

}  // namespace regs::engine
