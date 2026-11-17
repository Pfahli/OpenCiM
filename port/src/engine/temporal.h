#pragma once
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;

// Temporal antialiasing and render scale for the game's map view (settings regsTaa and regsRenderScale;
// docs/renderer/vulkan.md §9.1-9.2). Backend-neutral: post passes on gpu::Device, so OpenGL and Vulkan both run it.
//
// temporalBegin decides for this frame (after culling and the shadow pass, right before the main pass): with TAA the
// view's projection gets this frame's sub-pixel jitter; with a render scale below 100 % the scene is drawn smaller.
// The caller draws the scene into `input` (bottom-left rw × rh, with depth), then temporalFinish resolves it: TAA
// (history reprojected through the depth, clipped to the current neighbourhood, blended; at the output size, so a
// smaller render size becomes temporal upscaling) or, without TAA, the spatial upscaler. The result goes into `out`
// (the colour styles' w × h input, colour and depth) or onto the back-buffer rect (x, y, w, h, top-left origin).
struct TemporalPass {
  bool active = false;  // TAA on or a render scale below 100 %
  bool taa = false;
  int rw = 0, rh = 0;   // render size
  const render::RenderTarget* input = nullptr;
};
TemporalPass temporalBegin(Display& d, script::VM& vm, int w, int h, render::SceneView& v);
void temporalFinish(Display& d, script::VM& vm, const TemporalPass& t, int x, int y, int w, int h, int backW, int backH,
                    const render::RenderTarget* out);
// render targets are made again at the next frame (window size or UI scale changed)
void temporalReset(Display& d);

}  // namespace regs::engine
