#pragma once
#include "script/vm.h"

namespace regs::engine {

class Display;

// HDR output (settings regsHdr, regsHdrPaperWhite, regsHdrPeak; docs/renderer/vulkan.md §9.6). The Vulkan device
// switches to an HDR10 swapchain and a float back buffer when the display offers HDR (render/gpu.h setHdr). Then:
//  * hdrExpandView, after a 3D view is in the back buffer and before the GUI over it: the game's picture is SDR, so its
//    brightest parts are lifted above paper white (an inverse tone curve on the top of the range, values already
//    above 1.0 from bloom and lights carry on further); the GUI drawn afterwards stays at paper white.
//  * hdrEncode, right before the present: the whole back buffer from gamma-2.2 paper-white-relative values to PQ
//    (BT.2020 primaries, a soft roll-off into the peak brightness).
// Both do nothing while the device is not in HDR (OpenGL, Windows HDR off, the option off).
void hdrSync(Display& d, script::VM& vm);  // the settings → the device; once per frame before the back buffer begins
void hdrExpandView(Display& d, script::VM& vm, int x, int y, int w, int h, int backW, int backH);  // top-left rect
void hdrEncode(Display& d, script::VM& vm, int backW, int backH);
void hdrReset(Display& d);  // the targets are made again (window size changed)

}  // namespace regs::engine
