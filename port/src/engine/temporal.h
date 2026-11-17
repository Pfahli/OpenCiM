#pragma once
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;

// Temporal antialiasing and render scale for the game's map view (settings regsTaa, regsRenderScale and regsUpscaler;
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
  bool ao = false;      // raytraced ambient occlusion runs on the input (the frame's ray scene must be built)
  bool fsr = false;     // AMD FSR 3.1 resolves the input instead of the port's TAA / upscaler (Vulkan)
  int rw = 0, rh = 0;   // render size
  const render::RenderTarget* input = nullptr;
};
TemporalPass temporalBegin(Display& d, script::VM& vm, int w, int h, render::SceneView& v);
void temporalFinish(Display& d, script::VM& vm, const TemporalPass& t, int x, int y, int w, int h, int backW, int backH,
                    const render::RenderTarget* out);
// render targets are made again at the next frame (window size or UI scale changed)
void temporalReset(Display& d);
// FSR 3 frame generation (setting regsFrameGen; docs/renderer/vulkan.md §9.4), with the FSR upscaler: after the game
// view is finished (its post effects, the HDR lift) and before the GUI, temporalHudless keeps the view (a full-window
// view only); right before the present, temporalFrameGenSubmit hands it to the device (PQ-encoded too in HDR)
void temporalHudless(Display& d, script::VM& vm, int x, int y, int w, int h, int backW, int backH);
void temporalFrameGenSubmit(Display& d, script::VM& vm, int backW, int backH);

}  // namespace regs::engine
