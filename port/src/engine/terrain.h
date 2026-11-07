#pragma once
#include <vector>

#include "render/math3d.h"
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
class GridObj;
struct GridData;

// gsGrid::readGridChunk's cell fix-ups and the builder's water dry-out rule, applied once after a load
// (docs/renderer/terrain.md §2.1-2.2).
void prepareTerrainCells(GridData& g);

// Adds the divisions of a loaded map that are in view and within reach of the eye to the draw list: terrain
// (docs/renderer/terrain.md: corner attributes §3, triangulation §4, groups §5, water §7.1, edges §8.1) and merged
// items (docs/renderer/grid-items.md). Meshes are built on first use, item meshes nearest first within a per-frame
// budget, and freed when far away.
// viewScale and detailScale (port graphics options, 1 = the original) scale the cull/free distances and the near
// LOD band.
void collectGrid(Display& d, script::VM& vm, GridObj* grid, const render::Vec3& eye, const render::Mat4& viewProj,
                 float farZ, float viewScale, float detailScale, std::vector<render::DrawItem>& items);

// The map's surface map for the mods' shader hooks (engine/surface_map.h): its texture and (originX, originZ,
// 1 / sizeX, 1 / sizeZ) in world units; false when there is none.
// topTexture: the top map (the highest item surface above each 1 m cell, same rect)
bool gridSurfaceMap(GridObj* grid, uint32_t& texture, float rect[4], uint32_t* topTexture = nullptr);

// the map's cache (divisions, materials, grid items, surface map), built when missing or stale
void ensureGridCache(Display& d, script::VM& vm, GridObj* grid);
// regsPrewarmGrid() for the loading screen
void registerTerrainNatives(script::VM& vm);

}  // namespace regs::engine
