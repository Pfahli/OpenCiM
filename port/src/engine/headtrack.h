#pragma once
#include "render/math3d.h"
#include "script/vm.h"

namespace regs::engine {

// Head tracking (owner's request 2026-11-23, docs/stereo-3d.md): the head pose from OpenTrack's "UDP over network"
// output, as StereoLoom sends it from the eye tracker of a Leia SR display (Samsung Odyssey 3D, Acer SpatialLabs) and
// opentrack itself from any tracker (webcam, TrackIR clip, phone): 48-byte packets of six little-endian doubles, x, y,
// z (cm) and yaw, pitch, roll (degrees), on 127.0.0.1:4242. The map view's camera turns about the point it looks at as
// the head moves (sideways and up / down), so moving the head looks around the town, in 2D and in stereo 3D.
// Settings regsHeadTracking (0 off, 1 on) and regsHeadTrackingStrength (0 .. 200 %); REGS_HEADTRACK=0|1 overrides.

// Display::renderFrame, once a frame: the settings, the socket (opened while on), the packets, the smoothing.
void headTrackFrame(script::VM& vm, float dt);
// The map view's camera for the head: eye turned about look (both world units). Nothing while off or without a pose.
void headTrackCamera(render::Vec3& eye, const render::Vec3& look);
// tests (--at head:x,y): a head offset in cm as if a packet had come
void headTrackTest(float xCm, float yCm);
// for the settings page: 0 off, 1 waiting for packets, 2 receiving
int headTrackState();

}  // namespace regs::engine
