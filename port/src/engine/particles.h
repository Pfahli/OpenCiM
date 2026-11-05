#pragma once
// Particles (docs/renderer/particles.md): gsParticleEmitter, gsEmittingParticleEmitter, gsParticleEngine, the grid's
// emitters on items and vehicles (gsGridItem::move / gsGridVehicle::move → gsParticleEmitter::instanceMove) and the
// billboards the 3D pass draws (gsParticleSystem_OGL).
#include "render/math3d.h"
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
class GridObj;
struct SimState;
struct SimParticles;  // per-simulation emitter instances (SimState::particles)

// After each grid tick (dt = the tick's scaled game time): the emitters of vehicles and items near the camera emit
// (conditions from the vehicle flags and grid.effectConditions).
void simParticlesTick(script::VM& vm, GridObj* grid, SimState& s, float dt);

// The visible particles of a scene for one viewport: sorted far to near, faded by distance and screen size, with the
// shared batch materials (particles.shader). halfDiagonal = sqrt(t² + (t·aspect)²), t = tan(fov/2).
void collectParticles(Display& d, script::VM& vm, script::Object* scene, const render::Vec3& eye,
                      const render::Vec3& forward, const render::Mat4& viewProj, float halfDiagonal,
                      render::ParticleDrawList& out);
// Compiles the combiner variants the batches need in a pass environment.
void prepareParticlePrograms(Display& d, script::VM& vm, render::ParticleDrawList& list, uint32_t env);

void registerParticleNatives(script::VM& vm);

}  // namespace regs::engine
