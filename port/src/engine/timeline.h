#pragma once
// The city timeline (gsGridTimeline, docs/save-format/lines-timeline.md §1.2, §2.6-2.15): the map's scripted history
// as dated actions (create, delete, rename and re-vary objects, terrain brushes, water). applyChanges replays them day
// by day onto the grid through the live map edits of engine/grid_edit.h, with the actions half a year ahead applied as
// previews (hidden placeholders, construction-site marks, conflict marks). Natives: loadFromFile, saveToFile,
// applyChanges, checkDay, currentDay, removeDay, removeLastAction, removeAction, getActions, removeRedundantActions.
#include "script/vm.h"

namespace regs::engine {

void registerTimelineNatives(script::VM& vm);

}  // namespace regs::engine
