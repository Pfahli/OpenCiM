#pragma once
// Traffic maps for the mods' shader hooks (render/glsl.h, port/docs/mod-manager.md §4.3): where vehicles and people
// went lately, for snow that stays on quiet streets, tyre tracks along the lanes and trodden paths.
//
// * Lane map: a top-down RGBA8 texture, 1 m per texel, of the road and street-tram lanes (lane graph nodes with
//   kPathRoad or kPathTram, not in tunnels, not parking, not lane changes): per texel the lane's own number + 1 (18 bits, 0 = none; TrafficMaps::laneId), the signed
//   lateral offset of the texel centre from the lane's centre line (B, low 6 bits: (offset + 2.56 m) / 0.08 m; its top 2 bits are the number's bits 16-17) and the lane's
//   direction (A: angle mod π in 256 steps), so a hook can rebuild the exact lateral offset of any point inside the
//   texel (regs_laneAt). Painted once from the lane graph (live edits: on the next graph change).
// * Lane traffic: per lane node a decaying count of the time vehicles spent on it (road and rail units on road and
//   tram lanes, τ = 60 s of simulation, 10 s for the first 20 s so a loaded map is up to date at once), as
//   1 − exp(−count / 4 s) in the R channel of an RGBA8 texture of 256 lanes per row (regs_laneTraffic). G: the
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
  std::vector<uint8_t> lanes;      // RGBA per texel
  uint32_t laneTexture = 0;
  bool lanesBuilt = false, lanesUploaded = false;
  size_t graphNodes = 0;
  std::vector<int32_t> laneId;   // per graph node: its lane number in the maps (−1: not a painted lane)
  std::vector<int> laneNodes;    // per lane number: the graph node
  // lane traffic
  std::vector<float> laneCount;    // per node, simulation seconds
  std::vector<float> lanePlow;     // per node: cleared by a snowplow, 1 just now, filling in again with new snow
  std::vector<float> laneHeap;     // per node: the snow its plows pushed to the kerb, 0..1
  bool plowAny = false;
  std::vector<uint8_t> laneValue;  // 256 × rows, RGBA8 (traffic, plowed, heap, 0)
  uint32_t laneTrafficTexture = 0;
  int trafficRows = 0;
  // trodden map
  std::unordered_map<uint32_t, float> trodden;  // cell (z << 16 | x) → seconds
  std::vector<uint8_t> troddenBytes;            // laneW × laneH
  uint32_t troddenTexture = 0;
  int dirtyX0 = 0, dirtyZ0 = 0, dirtyX1 = -1, dirtyZ1 = -1;
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
  uint32_t footIndexTexture = 0, footAtlasTexture = 0;
};

// once per simulation update (engine/sim.cpp): counts the vehicles and pedestrians, decays, builds the lane map
void simTrafficMapsTick(script::VM& vm, GridObj* grid, SimState& s, float dt);
// at drawing time (render thread, engine/scene3d.cpp): uploads what changed (at most 5 times a second) and hands out
// the textures and the world rect (originX, originZ, 1 / sizeX, 1 / sizeZ); false when there are none
bool gridTrafficMaps(GridObj* grid, uint32_t& lanes, uint32_t& laneTraffic, uint32_t& trodden, float rect[4]);
// the footprint map's textures and rect (originX, originZ, texel size in m, 0); false when there are no steps
bool gridFootprints(GridObj* grid, uint32_t& index, uint32_t& atlas, float rect[4]);
// gsGrid.regsFootprints (see TrafficMaps::footFill)
void registerTrafficMapNatives(script::VM& vm);

}  // namespace regs::engine
