#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "script/vm.h"

namespace regs::engine {

// The parts of map.grid the renderer needs (docs/save-format/README.md §3): header, terrain cells, network items.
struct GridData {
  int width = 0, length = 0;
  float cellSize = 3.0f;
  uint32_t bucketShift = 4;
  uint16_t heightBase = 0x200;
  std::vector<int16_t> height;   // per cell, 1/256 cell, already shifted by 0x200 − heightBase
  std::vector<uint16_t> flags;   // per cell
  std::vector<uint32_t> normals; // per cell: packed 0x00XXYYZZ normal override from items (gsGridItem::fixGrid), 0 = none
  struct Item {
    uint16_t type = 0;           // index into types
    uint8_t orient = 0;          // bits 14-15 of the type word
    int16_t x = 0, y = 0, z = 0; // cells, 1/256 cell, cells
    uint32_t parent = 0;         // parent index + 1
    uint32_t variation = 0, flags = 0;
    bool removed = false;        // live map edits: a tombstone (indices stay stable while the map is loaded; not saved)
  };
  std::vector<std::string> types;
  std::vector<Item> items;
  std::vector<std::string> names;
  // port: curved roads (engine/curve_roads.h), saved in the port's own section 0x07F00000 of map.grid
  struct CurveRoad {
    float x0 = 0, y0 = 0, z0 = 0, dx0 = 1, dz0 = 0;  // start: the end block's edge centre (world units, y = the
                                                     // block's item height), and the unit direction into the curve
    float x1 = 0, y1 = 0, z1 = 0, dx1 = 1, dz1 = 0;  // end: edge centre and the direction of travel there (into the
                                                     // end block)
    float a = 0, b = 0;                              // the Bézier's handle lengths at the start and the end
    int kind = 0;                                    // 0: small road
    uint8_t ends = 0;  // bit 0: the start lies on a road block (a straight piece the tool made of a dead end), bit 1: the end
  };
  std::vector<CurveRoad> curves;
};
using CurveRoad = GridData::CurveRoad;

bool parseGridFile(const std::vector<uint8_t>& file, GridData& out, std::string* error);

// The content package of a game file: "addons/<name>" (also metro/addons, userdata/addons), "" for the base game.
std::string contentPackageOf(const std::string& path);

// A map.grid of an empty map (gsGrid.setSize): header, flat terrain at 0x200, an empty network.
std::vector<uint8_t> emptyGridFile(int width, int length, int bucketShift = 4);

// Per item: its 1-based index in a map.grid written now (removed items dropped, the rest in order), 0 if removed.
// Every item reference a section writer emits goes through it.
std::vector<uint32_t> savedItemRefs(const GridData& g);

// colormap(path): BMP pixels top-down as 0xFFRRGGBB.
class ColormapObj : public script::Object {
 public:
  using Object::Object;
  int width = 0, height = 0;
  std::vector<uint32_t> argb;
};

// 24/32-bit and 8-bit palettised uncompressed BMP → top-down 0xFFRRGGBB.
bool parseBmp(const std::vector<uint8_t>& file, int& width, int& height, std::vector<uint32_t>& argb);

struct TerrainCache;  // engine/grid_render.h
struct SimState;      // engine/sim.cpp

// gsGrid: item templates registered by loadGridObject, and the loaded map.
class GridObj : public script::Object {
 public:
  using Object::Object;
  std::map<std::string, script::Ref<script::Object>> templates;  // idName → gsGridObject
  std::vector<std::string> templateOrder;  // idNames in loadGridObject order (the template search map keeps the last)
  std::shared_ptr<GridData> data;
  std::shared_ptr<const std::vector<uint8_t>> sourceFile;  // the loaded map.grid; saveToFile writes it back by section
  uint64_t generation = 0;  // bumped on every load (render caches rebuild)
  uint64_t itemsVersion = 0;  // bumped after each batch of live map edits (engine/grid_edit.h)
  // port curved roads: bumped when GridData::curves or the tool's preview curve changed (the renderer rebuilds)
  uint64_t curvesVersion = 0;
  bool hasCurvePreview = false;
  GridData::CurveRoad curvePreview;
  bool curvePreviewBad = false;  // drawn red-tinted: the ends do not fit
  // curves added or removed in the running edit batch: gridEditEnd hands them to the simulation (simCurvesChanged)
  // with the cell rectangles (x0, z0, x1, z1) whose walk map they touch
  bool curvesDirty = false;
  std::vector<std::array<int, 4>> curveRects;
  std::shared_ptr<TerrainCache> terrain;  // division meshes, owned by the renderer
  std::shared_ptr<SimState> sim;          // lane graph, lines and vehicles (engine/sim.h)
  // tool highlights (engine/tools.h): cells with flag 0x4000 (0x8000 = red) and items with flag 0x100 (0x200 = red),
  // drawn as an overlay with positiveMaterial / negativeMaterial; the version changes with every change
  std::set<uint32_t> highlightCells, highlightItems;
  uint64_t highlightVersion = 0;
  // gsGrid::mountObject: preview item → its mounted copy while a tool commits (children follow their parent's copy)
  std::map<uint32_t, uint32_t> mountedCopies;
  // gsGrid.viewMode (+0x1fc, GRID_VIEW_*; docs/renderer/underground.md): 0xfe (GRID_VIEW_DEFAULT) from the constructor,
  // 0x180 the underground view. viewVersion moves when a change in bits 0xfe rebuilt the cover (write_viewMode).
  uint32_t viewMode = 0xFE;
  uint64_t viewVersion = 0;
  // gsGridItemController.setColor (gsGridItem::setColor L:0x08181de0): colourise requests the renderer applies to the
  // item tints after its next item sync (engine/terrain.cpp); recurse = also the items of the same root around it
  struct ItemColorRequest {
    uint32_t item = 0, argb = 0;
    bool recurse = true;
  };
  std::vector<ItemColorRequest> itemColorRequests;
  bool underground() const { return viewMode == 0x180; }

  // gsGrid.tick (0x100c48c0): the simulation in sub-steps, then the modules
  void tick(script::VM& vm, float dt) override;
};

void registerGridNatives(script::VM& vm);

}  // namespace regs::engine
