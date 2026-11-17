#pragma once
// Working traffic lights (owner 2026-11-16: "Kann man die Ampeln leuchten lassen? Mit wechselnden Rot-Grün-Wellen"): the
// game's traffic-light props get a glowing lamp on their head, by day and by night, that runs through a signal cycle.
// Not an engine feature; decoration only (vehicles do not stop for them).
//
// * The lights of a map are found by their model path (traffic-lights*) and cached until the items change.
// * Lights within 30 units of each other form a junction. Its main axis is the facing of its first light; lights
//   facing along it (either way) run phase A, the others phase B, half a cycle later.
// * The cycle (40 s): green 16 s, yellow 3 s, red 20 s, red and yellow 1 s.
// * Green waves: each junction's cycle starts later by its position along its main axis over 12 units per second, so
//   traffic going that way at that speed meets green from one junction to the next.
#include <vector>

#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
class GridObj;

// adds the lit lamps of the traffic lights in view to the particle list (an additive glow batch)
void addTrafficLights(Display& d, script::VM& vm, const std::vector<GridObj*>& grids, const render::SceneView& v,
                      render::ParticleDrawList& particles);

}  // namespace regs::engine
