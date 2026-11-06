#pragma once
// Render-side state of a loaded map, shared by the terrain builder (engine/terrain.cpp) and the item builder
// (engine/grid_items.cpp). One entry per n×n-cell division; meshes are built on demand and freed out of range.
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/math3d.h"
#include "render/mesh.h"
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
class GridObj;
class ObjectObj;
struct GridData;
struct SurfaceMap;

struct GridDivision {
  render::GpuMesh terrain[2], items[2];  // per LOD
  bool terrainBuilt[2] = {false, false}, itemsBuilt[2] = {false, false};
  render::Mat4 world;   // translation to the division centre
  render::Vec3 center;  // world, y = 0
  float radius = 0;
  // the data layer's custom0..7 (gsGridDivision::updateDataLayer L:0x081635d0): the 8 area highlights nearest to the
  // division centre as (x, 0, z, radius), unused ones (0, 0, 0, −1000); refreshed while circles are shown
  float layerCustom[8][4] = {};
};

// An item template as the item builder needs it (gsGridObject fields, grid-items.md §1.3).
struct GridTemplate {
  ObjectObj* object = nullptr;
  int width = 1, length = 1, type = 0;
  uint32_t flags = 0;
  uint32_t diagonals = 0;  // gsGridObject.diagonals (4 nibbles per half), before the item's rotation
  bool storeMesh = false, vertexGeneration = false, bake = false;
  bool skinned = false;  // skeleton + skinned mesh: drawn per item and animated (collectSkinnedItems), not merged
  float animStart = 0, animLength = 0;  // doorOpenStart / doorOpenLen: the loop of gsGridItem::move
  float bakeScaleX = 1, bakeScaleZ = 1;
  int materialBase = -1;          // first of its materials in TerrainCache::materials
  int variation[4] = {-1, -1, -1, -1};  // texture variation materials 1..4
  const class ColormapObj* colorMap = nullptr;
};

struct TerrainCache {
  uint64_t generation = ~0ull;
  int n = 16, cols = 0, rows = 0;
  std::vector<render::GpuMaterial> materials;  // terrain groups first, then the template materials
  std::vector<GridDivision> divisions;
  std::vector<int> typeTemplate;               // map type index → templates index, −1 when unusable
  std::vector<GridTemplate> templates;
  std::vector<std::vector<uint32_t>> divisionItems;  // item indices per division (by footprint centre)
  std::vector<uint32_t> itemColor;                   // per item: colorMap tint (0 = none)
  // live map edits (engine/grid_edit.h): items bucketed so far, shared texture-variation materials, and the divisions
  // whose terrain or item meshes must be rebuilt
  size_t itemsBucketed = 0;
  std::map<std::string, int> fileMaterials;
  std::set<int> dirtyTerrain, dirtyItems;
  // the tool highlight overlay (engine/tools.h): highlighted cells and copies of highlighted items in world space,
  // with positiveMaterial / negativeMaterial (indices into materials), rebuilt when GridObj::highlightVersion moves
  int positiveMaterial = -1, negativeMaterial = -1;
  int dataMaterial = -1;  // grid.dataMaterial (selection/data.shader): the catchment circles' layer
  uint64_t highlightVersion = ~0ull;
  render::GpuMesh highlight;
  bool highlightBuilt = false;
  // the view mode the division meshes were built for (GridObj::viewMode / viewVersion), and in the underground view
  // the divisions that hold tunnel items (gsGridDivision +0x30 > 0: a TUNNEL item's footprint grown by one cell
  // overlaps them), recounted after live edits (GridObj::itemsVersion)
  uint32_t viewMode = 0xFE;
  uint64_t viewVersion = ~0ull, tunnelItemsVersion = ~0ull;
  std::vector<uint8_t> tunnelDivision;
  // the surface classes seen from above for the mods' shader hooks (engine/surface_map.h); null without hooks
  std::shared_ptr<SurfaceMap> surfaceMap;
};

// The view mode a division is built with (gsGridDivision::update, L:0x08166080.c:309-345): in the underground view
// (0x180) divisions without tunnel items are drawn as 0xc6 (bare terrain in earthMaterial and water, no items).
inline uint32_t divisionViewMode(const TerrainCache& tc, int division) {
  if (tc.viewMode != 0x180) return tc.viewMode;
  return size_t(division) < tc.tunnelDivision.size() && tc.tunnelDivision[size_t(division)] ? 0x180u : 0xC6u;
}

// gsGridItem::isVisible(viewMode) (L:0x08185c50): TEMP items always; trees with bit 8; otherwise by the root parent:
// TUNNEL with bit 0x100, roads … heliport (types 2..0x12) and ALIGN_4 with 0x10, everything else with 0x20.
bool gridItemVisible(const GridData& g, const TerrainCache& tc, uint32_t item, uint32_t viewMode);

// The tool highlight overlay of the map (cells and items flagged by gsGrid::selectArea / gsGridItem::setSelected).
void buildHighlightOverlay(const GridData& g, const GridObj& grid, const TerrainCache& tc, render::MeshData& out);

// Registers the templates the map uses (materials, tints) and buckets the items per division.
void initGridItems(Display& d, script::VM& vm, GridObj* grid, TerrainCache& tc);
// After live edits: registers types and buckets items added since, marking their divisions in dirtyItems.
void syncGridItems(Display& d, script::VM& vm, GridObj* grid, TerrainCache& tc);
// The division an item is (or, removed, was) drawn in by its footprint centre, or −1 when the divisions do not draw it.
int itemDivision(const GridData& g, const TerrainCache& tc, uint32_t item);
// fixGrid for the cells [x0, x1] × [z0, z1] (inclusive) after live edits: their cover quadrants (0xF00) and normal
// overrides re-stamped from every item overlapping them, by the rules of fixGridCover; templates read from the grid.
void fixGridCoverArea(script::VM& vm, GridObj* grid, int x0, int z0, int x1, int z1);

// An item's footprint after the orientation swap, with the diagonals rotated (item ctor), and the covered quadrants
// of its local cell (x, z): 0xF00 for a whole cell, 0 when the cell is not part of the item (gsGridItem::cellCover).
struct ItemFootprint {
  int sx = 1, sz = 1;
  uint32_t diag = 0;
};
ItemFootprint itemFootprint(int width, int length, uint32_t diagonals, int orient);
uint32_t itemCellCover(const ItemFootprint& f, uint32_t flags, int x, int z);

// gsGridItem::fixGrid over all items: cover quadrants in the cell flags and the normal override map (GridData::normals),
// before any terrain is built. Cover quadrants only in views with bit 0x20 (not underground).
void fixGridCover(script::VM& vm, GridData& g, const TerrainCache& tc, uint32_t viewMode);

// The merged item mesh of one division and LOD in division space, one submesh per (material, tint).
void buildGridItems(const GridData& g, const TerrainCache& tc, int division, int lod, render::MeshData& out);

// An item's placement in division space: T · Ry((orient+2)·90°) · [Ry(45°) · scale of "b" variants] (grid-items.md §2.2)
render::Mat4 itemPlacement(const GridData& g, const TerrainCache& tc, uint32_t item, int division);

// The skinned items of a division (docs/renderer/skinning.md §4): each drawn with its template's mesh, posed by
// gsGridItem::move / animate: a loop of doorOpenLen seconds from doorOpenStart on the grid clock, the first
// animation slot at weight 1; bind pose from 400 units on and for items without a loop. Door items (0x1000) take
// their clip time from doorTimes (the simulation's, engine/sim_doors.cpp; −1 or absent = bind pose).
// poses caches one pose per template for the frame.
void collectSkinnedItems(Display& d, script::VM& vm, const GridData& g, const TerrainCache& tc, int division,
                         const render::Mat4& divisionWorld, float distance, int lod, double gridTime, bool shadowOnly,
                         const std::unordered_map<uint32_t, float>* doorTimes,
                         std::map<const ObjectObj*, std::shared_ptr<const std::vector<render::Mat4>>>& poses,
                         std::vector<render::DrawItem>& out);

}  // namespace regs::engine
