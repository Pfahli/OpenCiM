#pragma once
#include <cstdint>
#include <string>

#include "script/vm.h"

namespace regs::engine {

class Display;

// Glasses-free 3D on Leia SR displays (owner's request 2026-11-26: "support LeiaSR and auto detection";
// docs/stereo-3d.md): Samsung Odyssey 3D, Acer SpatialLabs, Lenovo ThinkVision 3D and the other panels of the SR
// Platform. The game draws both eyes as for side by side; the SR runtime installed with the monitor ("weaver") turns
// them into the lenticular picture for where the viewer's eyes are, and its eye tracker reports them. Nothing of Leia's
// is part of OpenCiM: the runtime's DLLs are looked up and loaded at run time (LICENSE: the additional permission for
// the Leia SR runtime).
struct LeiaSrInfo {
  bool runtime = false;  // the SR Platform runtime is installed (HKLM\SOFTWARE\Dimenco\Simulated Reality)
  bool display = false;  // an attached monitor is an SR panel (its EDID id in the runtime's product list, or the
                         // runtime's manual product entry)
  bool usable = false;   // both
  std::string monitor;   // e.g. "Samsung Odyssey 3D (SAM7860)"
  std::wstring bin;      // the runtime's DLL folder
};
// Found once at the first call (registry, files, the monitors' ids); REGS_LEIASR=0 hides it, =1 claims a display.
const LeiaSrInfo& leiaSrDetected();

// Display::renderFrame for mode kStereoLeiaSr, after both eyes were kept (stereoKeepEye): the eyes side by side
// handed to the weaver, the woven picture in the back buffer (w × h). False when the runtime could not be started
// (the reason is logged once; leiaSrFailed() then tells stereo to stop asking).
bool leiaSrPresent(Display& d, script::VM& vm, int w, int h);
bool leiaSrFailed();
// The viewer's head (the middle of both eyes) in cm from the screen's centre, x right, y up, z out of the screen, as
// the SR eye tracker sees it now; false while the weaver is not running or no face is tracked.
bool leiaSrHead(float& x, float& y, float& z);
// mode left / game closing: the weaver, context and the lens request let go (the panel goes back to 2D).
// gpuAlive false at the game's end (the render device may be gone): its objects are not touched.
void leiaSrStop(Display& d, bool gpuAlive = true);
// the mode chosen anew: after a failure, the next frame tries to start again
void leiaSrRetry();

}  // namespace regs::engine
