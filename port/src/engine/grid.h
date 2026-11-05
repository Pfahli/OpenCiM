#pragma once
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
};

bool parseGridFile(const std::vector<uint8_t>& file, GridData& out, std::string* error);

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
  bool underground() const { return viewMode == 0x180; }

  // gsGrid.tick (0x100c48c0): the simulation in sub-steps, then the modules
  void tick(script::VM& vm, float dt) override;
};

void registerGridNatives(script::VM& vm);

}  // namespace regs::engine
