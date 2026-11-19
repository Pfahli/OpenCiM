#pragma once
#include <memory>
#include <string>
#include <vector>

#include "render/mesh.h"
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
class ControlObj;

// gsObject and subclasses: a loaded .object with its GPU copy (uploaded on first draw) and the colorize colour.
class ObjectObj : public script::Object {
 public:
  using Object::Object;
  std::shared_ptr<const render::ObjectData> data;
  std::shared_ptr<render::GpuMesh> gpu;
  std::shared_ptr<std::vector<render::GpuMaterial>> materials;
  uint32_t colorizeArgb = 0xFFFFFFFF;
  std::string pendingPath;  // grid templates load their mesh on first use
  // load-time transform of grid templates: the "b" diagonal variants turn by π/4 (+0x1b8) and may scale (+0x1ac)
  float loadYaw = 0.0f;
  float loadScale[3] = {1.0f, 1.0f, 1.0f};
  // grid templates: places from addHomePlaces / addWorkPlaces / addVisitPlaces, barred visitor classes
  // (preventVisitors); read by the minimap's data views (docs/ui/minimap.md §3.5) and later the simulation
  int homePlaces[7] = {}, workPlaces[7] = {}, visitPlaces[13] = {};
  uint32_t barredVisitors = 0;
};

// The current eye, look point and up of a camera as the 3D pass uses it (eased for floating cameras).
void cameraFrame(script::VM& vm, script::Object* cam, render::Vec3& eye, render::Vec3& look, render::Vec3& up);
// The point a camera turns about and looks at (its position, eased for floating cameras, or its target's).
render::Vec3 cameraPivot(script::VM& vm, script::Object* cam);
// The near and far plane of a viewport looking through a camera, as the 3D pass uses them (before the port's view
// distance option).
// gameFar (when given): the far distance the game itself asked for, before the port reaches farther toward the
// horizon (the fog is stretched by farZ / gameFar)
void viewDepthRange(script::VM& vm, script::Object* viewport, script::Object* cam, float& nearZ, float& farZ,
                    float* gameFar = nullptr);

// .object files by resolved VFS path, loaded once (null when missing or invalid).
std::shared_ptr<const render::ObjectData> loadObjectCached(script::VM& vm, const std::string& path);

// gsObject::load runs <stem>.script on the object before its mesh loads (0x1008bd90.c:65-93). The .properties
// companion (class and sub-objects of exported scenes) is not read yet.
void runCompanionScript(script::VM& vm, script::Object* o, const std::u16string& objectPath);

// A gsMaterialVariant (grid materials, sky, ...) as the renderer binds it; null gives the engine's default material.
render::GpuMaterial scriptMaterial(Display& d, script::VM& vm, script::Object* variant);

// A mesh material from a .material record, with the overrides scripts made through object.getMaterial(name).
render::GpuMaterial objectMaterial(Display& d, script::VM& vm, script::Object* object, const render::MaterialDef& m);

// The program of a combiner variant (empty shader = default.shader), compiled on first use; 0 when it fails.
uint32_t combinerProgram(Display& d, script::VM& vm, const std::string& shader, uint32_t matFlags, uint32_t env);

// A view of a viewport's scene through another camera into a target of its own (pop-out views, engine/popout.cpp):
// the scene, its sky, fog, weather, lights and shadows as the viewport has them, without the screen-only passes
// (bloom, TAA, colour styles, raytracing).
struct ViewOverride {
  render::Vec3 eye, look, up;
  float fov = 60;
  float distance = 100;  // the eye's distance to the point it looks at (near and far plane like the game camera's)
  const render::RenderTarget* target = nullptr;  // drawn into its bottom-left w × h, always cleared
  int w = 1, h = 1;
};
// Renders the 3D scene of a gsViewPort control into its texture target, if it has a scene, a camera and a
// gsTextureTarget_OGL effect. Called by the GUI pass when it reaches the viewport. With an override: that camera and
// target instead (the viewport's own camera is left as it is).
void renderViewportScene(Display& d, script::VM& vm, ControlObj* viewport, const ViewOverride* ov = nullptr);
// The on-screen viewport that drew a map (a scene with a grid) in the last frame, and its camera; null in the menus.
void sceneFrameBegin();  // Display::renderFrame, once a frame (lastMapViewport's age)
script::Object* viewportScene(script::VM& vm, ControlObj* viewport);
ControlObj* lastMapViewport();
script::Object* lastMapCamera();
// seconds per frame phase (load-time measurements): 1 scene nodes, 2 grid, 3 sim vehicles, 4 particles, 5 night lights,
// 6 programs
extern double gPhaseSeconds[8];

void registerSceneNatives(script::VM& vm);

}  // namespace regs::engine
