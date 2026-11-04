#pragma once
// Engine-driven world sounds of the simulation (docs/audio/README.md §11; engine/sim_sounds.cpp).
#include "script/vm.h"

namespace regs::engine {

class GridObj;
struct SimState;
struct SimSounds;  // per-simulation sound slots (SimState::sounds)

// After each grid tick (dt = the tick's scaled game time, 0 at game speed 0): vehicle loops follow speed and
// acceleration, horns and animal voices roll, item loops start and stop with the listener's distance.
void simSoundsTick(script::VM& vm, GridObj* grid, SimState& s, float dt);

// transport vehicle events (gsTransportVehicle::move, transport.md §4.3)
enum class SimSoundEvent { Announce, Bell, DoorOpen, DoorClose };
void simSoundVehicleEvent(script::VM& vm, SimState& s, int vehicle, SimSoundEvent e);

// gsGridVehicleController.playSound
void registerSimSoundNatives(script::VM& vm);

}  // namespace regs::engine
