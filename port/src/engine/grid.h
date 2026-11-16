#pragma once
#include <array>
#include <cmath>
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
    // port: a free turn in radians on top of orient (the editor's place tool, Shift + mouse wheel; within ±45°, ±22.5°
    // for objects with a diagonal variant), saved in the port's section 0x07F10000 of map.grid; only drawn (paths and
    // effects turn with it), the cells stay those of orient
    float yaw = 0;
    // port: a shift in world units off the footprint's centre (|x|, |z| ≤ half a cell: an object snapped to a turned
    // neighbour's grid), saved and drawn with yaw
    float offX = 0, offZ = 0;
    // port: a uniform size factor on the template's model (2026-11-16, owner: statues sized as OSM has them), drawn only
    // (the cells stay the template's); saved with yaw (bit 30 of the section's count)
    float scale = 1;
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
    uint8_t ends = 0;  // bit 0: the start lies on a road block (a straight piece the tool made of a dead end), bit 1: the end;
                       // bit 2: tram tracks along it; bit 3: the start's block is a tram track on the grass (no road:
                       // its turning circle lies over the track, the tracks run on through it), bit 4: the end's;
                       // bit 5: one-way (both lanes run from the start to the end: roundabouts), bit 6 with it: a
                       // single lane on the centre line; bit 7: part of a roundabout's ring
    // stops along it: a bus or tram stop centred at arc length s on one side, its platform bent to the curve; a real stop
    // item of the game (the anchor, stop_bus0<design>_small / stop_tram01_small at stopAnchor's cell) carries it for
    // lines, picking and saving, and is not drawn (GridData::curveAnchors)
    struct Stop {
      float s = 0;
      int8_t side = 1;     // +1 right of the curve's direction, −1 left
      uint8_t kind = 0;    // 0 bus, 1 tram
      uint8_t design = 1;  // the stop template's number
    };
    std::vector<Stop> stops;
    // 1: a bridge (2026-11-13, the OSM import's bridge=yes ways): the deck at the curve's own heights over the ground or
    // water, which stay as they are (no levelling, no cover), drawn with the kind's bridge span and pillars; saved in
    // the second byte of the kind in section 0x07F00000 and the timeline bytes
    // 2: a tunnel (main, the curve tool's tunnels)
    // 3 on a track (2026-11-15, the OSM import's level crossings without the crossing piece; 2 until main's tunnels): its
    // rails let into the road it crosses, drawn as the street tram strip at the road's height (no bed); like a bridge it
    // leaves the ground alone
    uint8_t bridge = 0;
    // the street name (2026-11-14: the map editor's rename tool over curves; findNames offers it for stop names), UTF-8,
    // empty when unnamed; saved after the stops (format 3)
    std::string name;
  };
  std::vector<CurveRoad> curves;
  // the stop items that anchor curve stops (and the stop tool's preview of one): not drawn, no walk map of their own
  // (refreshCurveAnchors, engine/curve_roads.h)
  std::set<uint32_t> curveAnchors;
  // per cell: 1 where a curve put the cell cover (applyCurveCover); picking takes such cells as ground (no item there)
  std::vector<uint8_t> curveCover;
  // port: the edges of painted areas (owner's request 2026-11-14: painted cells along a slanted outline make a saw
  // tooth). A cell with an edge is cut along one or two straight lines: the part inside takes the pavement paint
  // `type` (0..7, as flags & 7 of a painted cell), the part outside is drawn as the cell's own flags say. Lines in the
  // cell's own coordinates u, v (0..1 from its corner (x, z)): inside where l[k][0]·u + l[k][1]·v + l[k][2] >= 0; two
  // lines join as both (join 0) or either (join 1). Key paintEdgeKey(x, z). Painting the cell by hand drops its edge.
  struct PaintEdge {
    uint8_t type = 0;
    uint8_t lines = 1;  // 1 or 2
    uint8_t join = 0;   // 0: inside both lines, 1: inside either
    float l[2][3] = {{0, 0, 0}, {0, 0, 0}};
  };
  std::map<uint32_t, PaintEdge> paintEdges;
  // port: procedural buildings (engine/proc_buildings.h, owner's request 2026-11-15): a footprint outline raised to walls
  // with a roof, the port's own textures
  struct ProcBuilding {
    std::vector<std::pair<float, float>> outline;  // world x, z: a simple polygon (either winding)
    float y = 0;                                   // the ground floor's level (world)
    float height = 8;                              // the eaves over y
    float roofHeight = 0;                          // a gabled roof's ridge over the eaves (0: from the width)
    uint8_t roof = 0;                              // 0 flat with a parapet, 1 gabled (four corners only)
    uint8_t style = 0;                             // 0 hall, 1 plaster, 2 brick
    float frontX = 0, frontZ = 0;                  // toward the street (doors on the walls facing it); 0, 0 unknown
    // passages through it (2026-11-16, owner: roads and footpaths through buildings get tunnels): a straight run from
    // (x0, z0) to (x1, z1), half as wide as `half`, clear from y0 (the way's level) to y1; the building is cut away there
    // and lined (side walls, a ceiling)
    struct Passage {
      float x0 = 0, z0 = 0, x1 = 0, z1 = 0, half = 2, y0 = 0, y1 = 3;
    };
    std::vector<Passage> passages;
  };
  std::vector<ProcBuilding> procBuildings;
};
using CurveRoad = GridData::CurveRoad;
using PaintEdge = GridData::PaintEdge;

// port: the painted areas' edges (GridData::paintEdges) in map.grid: u32 count, then per cell u16 x, u16 z, u8 type,
// u8 lines | join << 4, u16 0, six floats (the two lines)
constexpr uint32_t kPaintEdgeSectionTag = 0x07F20000;
inline uint32_t paintEdgeKey(int x, int z) { return uint32_t(z) << 16 | uint32_t(x & 0xFFFF); }
// inside the edge at the cell's own u, v (0..1)
inline bool paintEdgeInside(const PaintEdge& e, float u, float v) {
  bool a = e.l[0][0] * u + e.l[0][1] * v + e.l[0][2] >= 0;
  if (e.lines < 2) return a;
  bool b = e.l[1][0] * u + e.l[1][1] * v + e.l[1][2] >= 0;
  return e.join ? (a || b) : (a && b);
}

// An item's turn in its placement x' = x·c + z·s, z' = −x·s + z·c (grid-items.md §2.2): (orient + 2)·90° and the port's
// free turn (GridData::Item::yaw)
inline void itemTurn(const GridData::Item& it, float& c, float& s) {
  static const float kCos[4] = {-1, 0, 1, 0}, kSin[4] = {0, -1, 0, 1};
  if (it.yaw == 0) {
    c = kCos[it.orient & 3], s = kSin[it.orient & 3];
    return;
  }
  float a = 3.14159265f + float(it.orient & 3) * 1.57079633f + it.yaw;
  c = std::cos(a), s = std::sin(a);
}

// port: placed off the grid: turned freely or shifted (GridData::Item::yaw, offX, offZ)
inline bool itemOffGrid(const GridData::Item& it) { return it.yaw != 0 || it.offX != 0 || it.offZ != 0 || it.scale != 1; }

// port: the items' free turns and shifts in map.grid: u32 count (bit 31: with the shifts, bit 30: with the sizes), then
// per item off the grid its 1-based saved index (savedItemRefs), the angle, (bit 31) offX, offZ and (bit 30) the scale
// as floats
constexpr uint32_t kItemTurnSectionTag = 0x07F10000;

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
  uint64_t procVersion = 0;  // port: bumped when GridData::procBuildings changed (the renderer rebuilds)
  bool hasCurvePreview = false;
  GridData::CurveRoad curvePreview;
  bool curvePreviewBad = false;  // drawn red-tinted: the ends do not fit
  std::vector<GridData::CurveRoad> curvePreviewMore;  // more preview curves with it (a roundabout's arcs), drawn without caps
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
  // port: the round brush's circle (corner coordinates x, z and radius): while highlightCircleVersion equals
  // highlightVersion, highlighted cells on its rim are drawn clipped to it (the paint and its eraser cut there too)
  float highlightCircle[3] = {0, 0, 0};
  uint64_t highlightCircleVersion = ~uint64_t(0);
  // the same for a convex outline instead of the circle (x, z pairs in corner coordinates; the pavement brush's Shift
  // strip): used while not empty and highlightCircleVersion equals highlightVersion
  std::vector<float> highlightPoly;
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
