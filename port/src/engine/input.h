#pragma once
#include <string>

#include "script/vm.h"

namespace regs::platform {
struct InputEvent;
}

namespace regs::engine {

class Engine;
class WindowObj;

// Delivers this frame's OS events (host().input) to the game: window events, the input targets and the desktop's
// controls (docs/ui/input.md).
void dispatchInput(Engine& eng);

// Start of the window tick: input-target snapshot, then the mouse position to the target (mousePos) while the app
// is active and the game captures the mouse.
void pollMouse(script::VM& vm, WindowObj* w);

// the desktop's held modifier keys (0x100 Shift, 0x200 Control, 0x400 Alt), for the tools
int inputModifiers();

// Windows virtual key → the engine's key code (0 = none)
int engineKeyOf(int vk);

// Smooth camera (camera_input.cpp): sees every OS event first and takes the ones meant for the camera (true: not
// passed on to the game); its script natives (regsSmoothCamera, regsCameraInput, regsKeyHeld)
bool cameraInputEvent(script::VM& vm, WindowObj* w, const platform::InputEvent& e);
void registerCameraInputNatives(script::VM& vm);
// the pointer is over the world (the desktop's hover control is MAIN.$player or inside it), not a panel
bool overWorld(script::VM& vm, WindowObj* w);

// the controller's camera amounts of a frame (gamepad.cpp → camera_input.cpp → regsCameraInput, the gameUI extension)
struct CameraPad {
  float panX = 0, panY = 0;  // screen heights, screen right / down
  float zoom = 0;            // ln of the distance factor (nearer negative), eased
  float turn = 0;            // radians, the map counterclockwise positive, followed
  float tilt = 0;            // radians, looking further down positive
  bool turning = false;      // the turn stick is out of its dead zone
  bool tiltReset = false;
};
void cameraPadInput(const CameraPad& p);

// Game controllers (gamepad.cpp, port/docs/controller.md): once a frame after the OS pump, before dispatchInput. Reads
// the pad (or the simulated one), moves the virtual cursor and presses buttons and keys by adding OS-style events to
// host().input, so the game sees a mouse and keys; camera amounts go to cameraPadInput. realInput: false in scripted
// runs (no real pad is read then, only the simulated one, and the system cursor stays where it is)
void gamepadFrame(Engine& eng, float dt, bool realInput);
// --at N:pad:<name>,<value> (tests): the simulated pad (a, b, x, y, lb, rb, back, start, l3, r3, up, down, left,
// right, guide, lpadclick, rpadclick, l4, r4, l5, r5: 1 pressed / 0 released; lx, ly, rx, ry: -1..1, up positive;
// lt, rt: 0..1; lpad / rpad,<x>,<y> a finger on a touchpad, lpad / rpad,off lifted; lswipe / rswipe,<x0>,<y0>,<x1>,
// <y1>,<frames> a finger moving then lifted; layout,xbox|ps|deck|sc; off: the real pads again)
void gamepadSimulate(const std::string& arg);
void registerGamepadNatives(script::VM& vm);

}  // namespace regs::engine
