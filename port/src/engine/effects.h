#pragma once
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;

// gsBloomEffect_OGL (docs/renderer/bloom.md). With a bloom effect on the viewport the scene renders into the
// effect's HDR target (bloomSceneTarget), then bloomFinish runs the downsample/blur/add chain, measures the exposure
// for the next frame and composites into the back-buffer rect (x, y, w, h with a top-left origin).
// bloomSceneTarget returns null when the effect cannot run (it is then bypassed, like a failed FBO).
const render::RenderTarget* bloomSceneTarget(Display& d, script::VM& vm, script::Object* bloom, int w, int h);
// out: composite into this target (bottom-left w×h) instead of the back buffer (port style effects follow).
void bloomFinish(Display& d, script::VM& vm, script::Object* bloom, int x, int y, int w, int h, int backW, int backH,
                 const render::RenderTarget* out = nullptr);

// Port style effects on the game view (settings regsColorStyle and regsLensBlur, shaders in port/data/regs/shaders):
// a lens blur (by distance from the camera target, or tilt-shift) and a colour grade after the scene and bloom.
// styleActive: any of them is on. styleTarget: the w×h input with a depth texture (the scene with `samples` MSAA,
// or the bloom composite with 0), null when the effects cannot run. styleFinish: the passes into the back-buffer
// rect (x, y, w, h with a top-left origin).
bool styleActive(script::VM& vm);
const render::RenderTarget* styleTarget(Display& d, script::VM& vm, int w, int h, int samples);
// nearZ/farZ: the view's depth range; focus: eye-to-camera-target distance (the distance lens blur).
void styleFinish(Display& d, script::VM& vm, int x, int y, int w, int h, int backW, int backH, float nearZ, float farZ,
                 float focus);
// The style input already holds the bloom composite: take the scene depth from bloom's scene target.
void styleTakeDepth(Display& d, const render::RenderTarget& scene, int w, int h);

// The per-tick exposure adaptation (gsBloomEffect_OGL::move, bloom.md §2), once per frame before rendering.
void bloomTick(script::VM& vm, script::Object* bloom, float dt);

// Field defaults of a new effect object (bloom: scale 0.5, saturation 1.0, lightness 0.9, saturationWeight 1.0;
// shadow: maxDistance −1).
void initEffectFields(script::Object* effect);

// gsShadowEffect_OGL light-camera fit (docs/renderer/shadows.md §3): pure math, checked against §3.7 by
// `regs-tool shadowtest`. Matrices are the port's column-vector ones (memory-identical to the engine's row-vector
// matrices).
struct ShadowFitIn {
  render::Vec3 eye, target, up;  // main camera (getPosition/getTarget/getUp)
  float fov = 60, aspect = 1;    // vertical fov in degrees; viewport width / height
  float near = 1, far = 400;     // near = max(viewport, camera); far = min(viewport, camera, effect maxDistance)
  bool clipYPlane = false;
  float yPlane = 0, safeMargin = 0, maxDistance = -1;
  render::Vec3 light;            // the light's global position (the sun at 20000 units)
  int mapWidth = 1024, mapHeight = 1024;
};
struct ShadowFitOut {
  render::Mat4 lightView, lightProj;  // V_L, P_L
  render::Mat4 warp;                  // shadowWarp
  float params[4] = {0, 0, 0, 0};     // shadowParams
  float lightNear = 0, lightFar = 0, lightFov = 0;
  render::Vec3 axisZ, axisX, axisY;
  float projected[4][2] = {}, projectedEye[2] = {}, warpedCorners[4][2] = {};  // P0..P3 after the margin
};
bool fitShadow(const ShadowFitIn& in, ShadowFitOut& out);

// The effect's depth map (mapWidth × mapHeight from new_shadowEffect), created on first use; null = bypassed.
const render::RenderTarget* shadowTarget(Display& d, script::Object* shadow);

}  // namespace regs::engine
