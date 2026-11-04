#pragma once
// GRIDMODE_ADD_STOP (9): adding stops to the line being edited (gsGridPlayer.toolLine) by hovering and clicking
// (docs/ui/lines.md §2). Registered with the tool framework (engine/tools.h).
#include "script/vm.h"

namespace regs::engine {

void registerAddStopTool(script::VM& vm);

}  // namespace regs::engine
