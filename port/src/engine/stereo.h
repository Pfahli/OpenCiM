#pragma once
#include "render/math3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;

// Stereoscopic 3D (owner's request 2026-11-23, docs/renderer/stereo.md; settings regsStereo, regsStereoDepth,
// regsStereoConvergence, regsStereoSwap on the image quality page). The whole frame, scene and interface, is drawn once
// per eye into the back buffer and copied aside; stereoCompose then puts both into the back buffer the way the mode
// wants it (side by side, top and bottom, interleaved rows / columns, checkerboard, anaglyph). Each eye's camera sits
// beside the game camera, both looking the same way (parallel axes), with the frustum shifted so the point the game
// camera looks at lies on the screen plane: nearer things come out of the screen, farther ones go behind it.
enum StereoMode {
  kStereoOff = 0,
  kStereoSideBySide = 1,    // half width each (3D TVs and projectors: "Side by Side")
  kStereoTopBottom = 2,     // half height each ("Top and Bottom" / "Over-Under")
  kStereoRows = 3,          // rows interleaved, the top row left (passive 3D monitors with polarised glasses)
  kStereoColumns = 4,       // columns interleaved (autostereoscopic panels)
  kStereoCheckerboard = 5,  // pixels alternating (DLP 3D projectors and TVs)
  kStereoRedCyan = 6,       // anaglyph glasses: red left, cyan right (Dubois colours)
  kStereoGreenMagenta = 7,  // green left, magenta right (Dubois)
  kStereoAmberBlue = 8,     // amber left, blue right (ColorCode 3-D; Dubois)
  kStereoRedCyanGray = 9,   // red/cyan in grey (least ghosting, no colour)
  kStereoModeCount = 10
};

// Display::renderFrame: the mode for this frame (settings, REGS_STEREO=<mode> overrides); 0 = draw once as before.
int stereoFrameMode(script::VM& vm);
// the eye being drawn: 0 none (mono or another view), −1 left, +1 right
int stereoEye();
void stereoSetEye(int eye);
// true while the frame's first (or only) pass is drawn: per-frame work (camera easing, bloom adaptation, the frame-rate
// measurement) runs only then
inline bool stereoFirstPass() { return stereoEye() <= 0; }
// An on-screen view's camera for the eye being drawn: eye and look moved sideways, the projection sheared so the look
// point stays on the screen plane. Nothing when no eye is drawn.
void stereoCamera(script::VM& vm, render::Vec3& eye, render::Vec3& look, const render::Vec3& up, render::Mat4& proj);
// after an eye's pass: the back buffer (w × h) copied aside
void stereoKeepEye(Display& d, int eye, int w, int h);
// both eyes into the back buffer for the mode; false (back buffer left as the right eye) when the program is missing
bool stereoCompose(Display& d, script::VM& vm, int mode, int w, int h);
void stereoReset(Display& d);
// tests (--at stereo:<mode>): that mode instead of the settings' from now on
void stereoTestMode(int mode);

}  // namespace regs::engine
