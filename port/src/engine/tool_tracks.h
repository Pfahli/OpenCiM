#pragma once
// Laying tram and metro tracks (docs/ui/construction.md §6-§9): gsGridPlayer's GRIDMODE_LAY_TRAM_TRACK (0x11) and
// GRIDMODE_LAY_TRACK (6) with their previews, commits and cancels, and the track and road blocks other tools build
// on (toolBlocks(): a placed station's track parts).
#include "script/vm.h"

namespace regs::engine {

void registerTrackTools(script::VM& vm);

}  // namespace regs::engine
