#pragma once
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

}  // namespace regs::engine
