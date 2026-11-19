#pragma once
#include <string>
#include <vector>

#include "platform/window.h"
#include "script/vm.h"

namespace regs::engine {

class Display;

// Pop-out views (owner's request 2026-11-19, docs/popout-views.md): extra OS windows that show the map from a camera of
// their own. Each can go anywhere (another monitor), be sized freely or fill its monitor (F11 / Alt+Enter), and is
// steered by mouse and keys while it has the focus, independently of the game's camera. Opened with F7 in the game,
// the button beside the minimap or regsPopoutOpen(); closed with their window's close box.

// Opens one more pop-out view (only while a map is shown); false when none could be made.
bool popoutOpen(script::VM& vm);
int popoutCount();
// Once a frame after the main window's events were pumped: the pop-outs' own events (camera, fullscreen, close) and
// F7 in the main window's. scripted: a run driven by --at actions, where real mouse and keys are ignored.
void popoutsPump(script::VM& vm, const std::vector<platform::InputEvent>& mainInput, bool scripted);
// Display::renderFrame, after the main view and GUI: every pop-out drawn and shown in its window.
void popoutsRender(Display& d, script::VM& vm);
// The windows closed (their places remembered in <user folder>\popouts.cfg) before the display and device go.
void popoutsShutdown();
// Tests (--at): an event as if it came from the first pop-out's window; its next picture saved as a PNG.
void popoutInject(const platform::InputEvent& e);
void popoutScreenshot(const std::string& file);
// tests: a window message to the first pop-out's window ("msg,wparam,x,y", client pixels; numbers decimal or 0x..)
// and a line in the log with its place, size, fullscreen state and what the pointer is on
void popoutTestMessage(const std::string& spec);

void registerPopoutNatives(script::VM& vm);

}  // namespace regs::engine
