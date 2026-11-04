#pragma once
#include "script/vm.h"

namespace regs::engine {

class Engine;
class WindowObj;

// Delivers this frame's OS events (host().input) to the game: window events, the input targets and the desktop's
// controls (docs/ui/input.md).
void dispatchInput(Engine& eng);

// Start of the window tick: input-target snapshot, then the mouse position to the target (mousePos) while the app
// is active and the game captures the mouse.
void pollMouse(script::VM& vm, WindowObj* w);

}  // namespace regs::engine
