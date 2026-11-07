#pragma once
// The map editor's terrain tools (docs/ui/construction.md §15): the brushes shift, level, soften, pavement, field and
// ruin, growing trees and toggling water (GRIDMODE 2, 3, 4, 0xf, 0x10, 0x13, 0xe, 0xd), recorded on the timeline.
#include "script/vm.h"

namespace regs::engine {

// registers the modes with the tool framework (engine/tools.h); after registerToolNatives
void registerTerrainTools(script::VM& vm);

}  // namespace regs::engine
