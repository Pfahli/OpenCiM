#pragma once
// Traffic maps for the mods' shader hooks (render/glsl.h, port/docs/mod-manager.md §4.3): where vehicles and people
// went lately, for snow that stays on quiet streets, tyre tracks along the lanes and trodden paths.
//
// * Lanes: the road and street-tram lanes (lane graph nodes with kPathRoad or kPathTram, not in tunnels, not parking,
//   not lane changes, flagged or not) get numbers of their own (TrafficMaps::laneId). They are vector lanes: the
//   lane texture (regs_LaneTraffic, RGBA32F, 1024 wide) holds per lane its values and its curve's Bézier control
//   points, and a hook measures a point's exact offset from the curves (regs_laneCurve); the lane map (regs_LaneMap,
//   RGBA16UI, one texel per 2 m cell) only says which lanes are near: per cell (start, near count, all count) into a
//   list of lane numbers stored after the lanes' rows in the lane texture, nearest first; complete for every lane
//   that can matter anywhere in the cell, so nothing drops out at a cell edge (regs_lanesAround). Built once from the
//   lane graph (live edits: on the next graph change).
// * Lane traffic: per lane node a decaying count of the time vehicles spent on it (road and rail units on road and
//   tram lanes, τ = 60 s of simulation, 10 s for the first 20 s so a loaded map is up to date at once), blurred along
//   the lanes, as 1 − exp(−count / 4 s) in the lane texture's values (R, regs_laneData(i, 0)). G: the
//   snowplows (engine/snowplows.h), 1 where one drove just now, filling in again with new snow at the footprints'
//   pace (gsGrid.regsFootprints); B: the heap its blade pushed to the kerb, growing with each pass over a lane that
//   had snow on it again (regs_lanePlowed). Both are cleared once the footprints are off (no snow).
// * Trodden map: an R8 texture, 1 m per texel, of a decaying count of the time visible pedestrians spent in each
//   cell (τ = 300 s), as 1 − exp(−count / 1.5 s) (regs_troddenAt). Only cells people were in are kept (a sparse list),
//   so updating it costs what the walkers cost.
// Built only while a shader hook is registered; uploaded at most 5 times a second.
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "render/math3d.h"
#include "script/vm.h"

namespace regs::engine {

class GridObj;
struct SimState;

struct TrafficMaps {
  // lane map
  int laneW = 0, laneH = 0;
  float originX = 0, originZ = 0;  // world position of texel (0, 0)'s corner (also the trodden map's)
  int listW = 0, listH = 0;        // the lane lists' cells (2 m)
  std::vector<uint16_t> lanes;     // per cell: list start (low, high 16 bits), near count, all count
  std::vector<float> listIds;      // the lists: lane numbers, nearest first
  uint32_t cellBase = 0;  // the lane texture's first texel of the cells (gridTrafficMaps)
  bool lanesBuilt = false, lanesUploaded = false;
  uint64_t laneVersion = 0;      // the LaneGraph::laneVersion painted
  std::vector<int32_t> laneId;   // per graph node: its lane number in the maps (−1: not a painted lane)
  std::vector<int> laneNodes;    // per lane number: the graph node
  // lane traffic
  std::vector<float> laneCount;    // per node, simulation seconds
  std::vector<float> lanePlow;     // per node: cleared by a snowplow, 1 just now, filling in again with new snow
  std::vector<float> laneHeap;     // per node: the snow its plows pushed to the kerb, 0..1
  bool plowAny = false;
  int laneTrafficRows = 0;         // the lane texture: RGBA32F 768 × rows (values, curve, curve; gridTrafficMaps)
  bool laneCurvesDirty = true;
  uint32_t laneTrafficTexture = 0;
  int trafficRows = 0;
  // trodden map
  // the trodden cells, cell (z << 16 | x) and seconds, in no order, and where each cell's entry is: a decay pass over
  // flat arrays every grid step (a map walked node by node cost a sixth of a speed-10 frame on Berlin)
  std::vector<uint32_t> troddenCells;
  std::vector<float> troddenTime;
  std::unordered_map<uint32_t, uint32_t> troddenIndex;
  std::vector<uint8_t> troddenBytes;            // laneW × laneH
  bool troddenSynced = false;  // copied into the top map's texture once (then the dirty tiles)
  const void* troddenTarget = nullptr;  // the surface map it was copied into
  // the bytes changed since the last copy: a flag per tile of kTroddenTile² (markTrodden)
  static constexpr int kTroddenTile = 64;
  int troddenTilesW = 0, troddenTilesH = 0;
  std::vector<uint8_t> troddenDirty;
  bool troddenAnyDirty = false;
  void markTrodden(int x, int z) {
    troddenDirty[size_t(z / kTroddenTile) * size_t(troddenTilesW) + size_t(x / kTroddenTile)] = 1;
    troddenAnyDirty = true;
  }
  float age = 0;
  bool laneTrafficDirty = false;
  double lastUpload = -1;  // steady-clock seconds of the last upload
  // footprint map: single steps of the walkers near the camera, 5 cm texels in tiles of 32² (1.6 m), kept only where
  // people stepped lately: an atlas of 64 × 64 tiles (R8; when full, the least recently stepped tile is reused) and
  // an index of the map's tiles (R16, atlas slot + 1, 0 = no steps). Switched by the mods: gsGrid.regsFootprints =
  // 0 off (and cleared), else the time constant (simulation seconds) in which steps fill in again (snowfall)
  float footFill = 0;
  int tilesX = 0, tilesZ = 0;
  std::vector<uint16_t> tileIndex;  // tilesX × tilesZ
  std::vector<uint8_t> atlas;       // 2048 × 2048
  std::vector<int32_t> slotTile;    // per slot: the tile (z · tilesX + x), -1 free
  std::vector<float> slotStamp;     // per slot: age at the last step (reuse order)
  std::vector<uint8_t> slotDirty;
  int freeSlots = 0;
  bool atlasAllDirty = false;
  int indexX0 = 0, indexZ0 = 0, indexX1 = -1, indexZ1 = -1;
  struct Walker {
    render::Vec3 last;
    float stride = 0;
    uint8_t side = 0;
    bool valid = false;
  };
  std::vector<Walker> walkers;  // per character slot
  float fadeClock = 0;
  uint32_t footAtlasTexture = 0;  // the atlas and below it the tile index (gridFootprints)
};

// once per simulation update (engine/sim.cpp): counts the vehicles and pedestrians, decays, builds the lane map
void simTrafficMapsTick(script::VM& vm, GridObj* grid, SimState& s, float dt);
// at drawing time (render thread, engine/scene3d.cpp): uploads what changed (at most 5 times a second) and hands out
// the textures and the world rect (originX, originZ, 1 / sizeX, 1 / sizeZ); false when there are none
bool gridTrafficMaps(GridObj* grid, uint32_t& laneTraffic, float rect[4], float cells[4]);
// the footprint map's textures and rect (originX, originZ, texel size in m, 0); false when there are no steps
bool gridFootprints(GridObj* grid, uint32_t& atlas, float rect[4]);
// gsGrid.regsFootprints (see TrafficMaps::footFill)
void registerTrafficMapNatives(script::VM& vm);

}  // namespace regs::engine
