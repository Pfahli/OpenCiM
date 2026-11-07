#pragma once
// Laying tram and metro tracks (docs/ui/construction.md §6-§9): gsGridPlayer's GRIDMODE_LAY_TRAM_TRACK (0x11) and
// GRIDMODE_LAY_TRACK (6) with their previews, commits and cancels, and the track and road blocks other tools build
// on (toolBlocks(): a placed station's track parts).
#include <cstdint>
#include <vector>

#include "render/math3d.h"
#include "script/vm.h"

namespace regs::engine {

class ControlObj;
class GridObj;

void registerTrackTools(script::VM& vm);

// port curved roads (engine/tool_curves.cpp): an edge of a small-road block where a curve can meet it
struct RoadEnd {
  uint32_t item = 0;
  render::Vec3 point;  // the edge centre, y = the block's item height (world units)
  render::Vec3 dir;    // the way out of the block through that edge (unit, axis-aligned)
  bool deadEnd = false;  // a dead end's closed side (else an edge of a straight piece)
};
// the small-road block edges within radius of (wx, wz), nearest first: dead ends' closed sides and both edges of
// straight pieces
std::vector<RoadEnd> findSmallRoadEnds(script::VM& vm, ControlObj* player, GridObj* grid, float wx, float wz, float radius);
// the block of an end replaced inside an edit batch: open = a straight piece through the edge (a curve joins), else a
// dead end closed there; recorded on the timeline (may be null). Returns the new item, −1 when there is no such piece.
int replaceRoadBlockEnd(script::VM& vm, ControlObj* player, GridObj* grid, const RoadEnd& end, bool open,
                        script::Object* timeline);

}  // namespace regs::engine
