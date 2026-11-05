#pragma once
#include <vector>

#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
class ControlObj;

// gsSky and gsSun meshes, the port's day/night cycle and gsTracerCtrl.enableFade (docs/renderer/sky-and-night.md).

// The camera-attached meshes of a scene: the dome of a visible, enabled sky (gsSky::generateBackground) and the disc
// of each visible sun with a mesh (gsSun::generateMesh), placed at the eye and scaled by the far distance (mesh
// flags 0x1000 | 0x400). They carry MATERIAL_NO_SHADOW, so shadow passes skip them as the engine does.
void addSkyItems(Display& d, script::VM& vm, script::Object* sky, const std::vector<script::Object*>& suns,
                 const render::Vec3& eye, float farDistance, std::vector<render::DrawItem>& items);

// The emission factor of the port's day/night cycle for a scene with this sky: 1 by day and without the cycle (the
// engine's constant self-illumination), up to kNightEmission at night so the illumination maps (lit windows, lamp
// heads, signs) stand out.
float skyEmissionScale(script::Object* sky);

// gsTracerCtrl::move (L:0x081cfc00): with enableFade(a, b, c, d) the tracer's visibility follows the distance from
// the camera eye to its target point: 0 up to a, rising to 1 at b, 1 up to c, falling to 0 at d.
void tracerFade(script::VM& vm, ControlObj* tracer, const render::Vec3& eye, const render::Vec3& target);

void registerSkyNatives(script::VM& vm);

}  // namespace regs::engine
