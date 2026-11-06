#pragma once
// Snowplows for the mods (port/docs/mod-manager.md §4.3): gsGrid.regsSnowplows = how many plows drive the streets
// (0 = none; the ones still out finish their trip). A plow is the base game's SUV (era4-suv, else era3-suv-01) in
// municipal orange with a plough blade, its frame and a flashing amber beacon that the engine builds itself (no game
// files are changed or shipped). Plows take long road trips (at least 400 m) like the background traffic and are not
// counted as road traffic. The lanes they drive are marked in the traffic maps (engine/traffic_maps.h: cleared, and
// the heap their blade pushed to the kerb), which the shader hooks read as regs_lanePlowed.
#include <vector>

#include "render/math3d.h"
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
class GridObj;
struct SimState;

namespace sim {
struct Mover;
struct PlacedPose;
struct UnitModel;
}  // namespace sim

// once per simulation update (engine/sim.cpp): spawns plows up to gsGrid.regsSnowplows
void stepSnowplows(script::VM& vm, GridObj* grid, SimState& s, float dt);
// at drawing time: the blade, its frame and the beacon in front of / on top of a plow's car
void collectPlowParts(Display& d, const sim::UnitModel& model, const sim::PlacedPose& p, double time,
                      std::vector<render::DrawItem>& items);
// gsGrid.regsSnowplows
void registerSnowplowNatives(script::VM& vm);

}  // namespace regs::engine
