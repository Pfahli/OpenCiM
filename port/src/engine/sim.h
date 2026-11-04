#pragma once
// The native simulation (docs/simulation/): the lane graph built from the items' paths, the transport lines of a
// loaded save and their vehicles. Slice 1 runs the line vehicles; citizens, passengers and background traffic come
// later.
#include <cstdint>
#include <functional>
#include <vector>

#include "render/math3d.h"
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
class GridObj;

// One live vehicle unit (a car of a tram or metro set, a bus) in map world space, for picking and cameras.
struct SimVehicleView {
  script::Object* object = nullptr;  // its gsTransportVehicle (one per vehicle, shared by all units); null for traffic
  int unit = 0;                      // 0 = the leading unit of its vehicle
  render::Vec3 position;             // the model origin (on the track, at the unit's pivot)
  render::Vec3 forward;              // unit vector in the direction of travel
  float yaw = 0;                     // radians about +Y: forward = (sin yaw, 0, cos yaw) when level
  render::Vec3 center;               // world centre of the model's bounds
  render::Vec3 halfExtents;          // half size of the model's bounds along (side, up, forward)
  float radius = 0;                  // bounding sphere radius around center
};

// Calls fn for every unit of every spawned vehicle of the grid's simulation, line vehicles first.
void forEachSimVehicle(GridObj* grid, const std::function<void(const SimVehicleView&)>& fn);

// One visible pedestrian (gsGridCharacters) in map world space.
struct SimCharacterView {
  script::Object* object = nullptr;  // its gsGridCharacterController (created on first use, cached; isCharacter,
                                     // <model>, <instance>, position / globalPos / orientation, getCitizen())
  int citizen = -1;                  // the citizen it belongs to (simCitizenController), −1 for none
  render::Vec3 position;             // feet, on the walk surface
  float yaw = 0;                     // radians about +Y: forward = (sin yaw, 0, cos yaw)
  render::Vec3 center;               // world centre of the model's bounds
  render::Vec3 halfExtents;          // half size of the model's bounds along (side, up, forward)
  float radius = 0;
};

// Calls fn for every visible pedestrian of the grid's simulation.
void forEachSimCharacter(GridObj* grid, const std::function<void(const SimCharacterView&)>& fn);

// The gsGridCitizenController of a citizen (index as in SimCharacterView::citizen), created on first use and cached,
// parented to the grid; null for an unknown index. Its natives (getID, getClass, getState, getMood, getHome,
// getWorkplace, getStart, getTarget, getCharModel, getCarModel, getCharacter, getVehicle, usedTransport) and events
// (onSpawnCharacter, onSpawnVehicle, onIdle) are the simulation's.
script::Object* simCitizenController(GridObj* grid, int citizen);

// The gsTransportStopController of a stop item (the one in the gsTransportLines stop vector, parented to the grid
// with <item> and <model> set), or null for other items.
script::Object* simStopController(GridObj* grid, int item);

// ---- engine hooks ----

// gsGridObject.setPathFlags / addPath, gsTransportLines / gsTransportLine / gsTransportVehicle natives.
void registerSimNatives(script::VM& vm);

// gsGrid.loadFromFile: builds the lane graph and reads the transport lines and their vehicles from map.grid.
void loadSimulation(script::VM& vm, GridObj* grid, const std::vector<uint8_t>& file);

// gsGrid.tick (0x100c48c0): dt·timeSpeed in 1 + trunc(5·dt·timeSpeed) sub-steps.
void tickSimulation(script::VM& vm, GridObj* grid, float dt);

// A live change of the map's items (gsGridTimeline.applyChanges, construction tools), once per batch, before the
// script events of the batch fire: `added` are new indices into GridData::items (already appended), `removed` items
// are tombstones (GridData::Item::removed, indices stay valid). The lane graph, walk layers, place pools, line stops
// and the vehicles and pedestrians on removed items follow (addObject / removeItem, citizens.md §2.3).
void simItemsChanged(script::VM& vm, GridObj* grid, const std::vector<uint32_t>& added, const std::vector<uint32_t>& removed);

// Terrain heights or water flags (0x2000) of the cells [x0, x1] × [z0, z1] changed: their walk layers follow.
void simTerrainChanged(script::VM& vm, GridObj* grid, int x0, int z0, int x1, int z1);

// Adds the spawned vehicles within farZ of the eye that are in view to the draw list.
void collectSimVehicles(Display& d, script::VM& vm, GridObj* grid, const render::Vec3& eye,
                        const render::Mat4& viewProj, float farZ, std::vector<render::DrawItem>& items);

}  // namespace regs::engine
