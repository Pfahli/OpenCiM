#pragma once
// Traffic maps for the mods' shader hooks (render/glsl.h, port/docs/mod-manager.md §4.3): where vehicles and people
// went lately, for snow that stays on quiet streets, tyre tracks along the lanes and trodden paths.
//
// * Lane map: a top-down RGBA8 texture, 1 m per texel, of the road and street-tram lanes (lane graph nodes with
//   kPathRoad or kPathTram, not in tunnels, not parking): per texel the lane node (16 bits, 0 = none), the signed
//   lateral offset of the texel centre from the lane's centre line (B: (offset + 2.56 m) / 0.02 m) and the lane's
//   direction (A: angle mod π in 256 steps), so a hook can rebuild the exact lateral offset of any point inside the
//   texel (regs_laneAt). Painted once from the lane graph (live edits: on the next graph change).
// * Lane traffic: per lane node a decaying count of the time vehicles spent on it (road and rail units on road and
//   tram lanes, τ = 60 s of simulation, 10 s for the first 20 s so a loaded map is up to date at once), as
//   1 − exp(−count / 12 s) in an R8 texture of 256 nodes per row (regs_laneTraffic).
// * Trodden map: an R8 texture, 1 m per texel, of a decaying count of the time visible pedestrians spent in each
//   cell (τ = 300 s), as 1 − exp(−count / 1.5 s) (regs_troddenAt). Only cells people were in are kept (a sparse list),
//   so updating it costs what the walkers cost.
// Built only while a shader hook is registered; uploaded at most 5 times a second.
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

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
  // lane traffic
  std::vector<float> laneCount;    // per node, simulation seconds
  std::vector<uint8_t> laneValue;  // 256 × rows, R8
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
};

// once per simulation update (engine/sim.cpp): counts the vehicles and pedestrians, decays, builds the lane map
void simTrafficMapsTick(script::VM& vm, GridObj* grid, SimState& s, float dt);
// at drawing time (render thread, engine/scene3d.cpp): uploads what changed (at most 5 times a second) and hands out
// the textures and the world rect (originX, originZ, 1 / sizeX, 1 / sizeZ); false when there are none
bool gridTrafficMaps(GridObj* grid, uint32_t& lanes, uint32_t& laneTraffic, uint32_t& trodden, float rect[4]);

}  // namespace regs::engine
