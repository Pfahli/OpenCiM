#pragma once
// The grid player's object tools (docs/ui/construction.md §7-9): GRIDMODE_CREATE_STOP (bus and tram stops on a road
// or tram track), GRIDMODE_PLACE_OBJECT (metro stations and the metro transition, docks, heliports; the template's
// onPlace parts) and GRIDMODE_REMOVE_OBJECTS (the bulldozer, gsGridPlayer::removeObject with the road ends it leaves),
// plus the gsGrid natives they lean on (getAverageY, findNames for the stop names).
#include "script/vm.h"

namespace regs::engine {

// registers the three modes with the tool framework (engine/tools.h) and the natives; after registerToolNatives
void registerObjectTools(script::VM& vm);

}  // namespace regs::engine
