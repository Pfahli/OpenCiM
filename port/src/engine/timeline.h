#pragma once
// The city timeline (gsGridTimeline, docs/save-format/lines-timeline.md §1.2, §2.6-2.15): the map's scripted history
// as dated actions (create, delete, rename and re-vary objects, terrain brushes, water). applyChanges replays them day
// by day onto the grid through the live map edits of engine/grid_edit.h, with the actions half a year ahead applied as
// previews (hidden placeholders, construction-site marks, conflict marks). Natives: loadFromFile, saveToFile,
// applyChanges, checkDay, currentDay, removeDay, removeLastAction, removeAction, getActions, removeRedundantActions,
// clear.
#include <cstdint>
#include <string>

#include "script/vm.h"

namespace regs::engine {

class GridObj;

void registerTimelineNatives(script::VM& vm);

// ---- recording (the map editor) ----
// The editor sets gsGridPlayer.timeline (metro/ui/timebar/timebar.script dateChanged); the tools then record what
// they do on the timeline's current day, newest first, as gsGridPlayer's finishTool / mouseMove do. Each record call
// is a no-op for a null timeline.
script::Object* playerTimeline(script::VM& vm, script::Object* player);  // gsGridPlayer.timeline or null
// beginGroup / endGroup (L:0x081a73c0 / L:0x081a7490): later actions go into one group with a label ("%3" lay track,
// "%!10" place stop, ...; timebar.script expands it); nested calls count; an empty group is dropped when it ends
void timelineBeginGroup(script::VM& vm, script::Object* timeline, const std::string& label);
void timelineEndGroup(script::VM& vm, script::Object* timeline);
// gsGridTimelineAction_createObject (L:0x081a6ca0): the item (flags & 0x42000000) and its parent (flags & 0x2000000);
// a variation action follows when the item has one (finishTool's pattern)
void timelineRecordCreate(script::VM& vm, script::Object* timeline, GridObj* grid, uint32_t item);
void timelineRecordDelete(script::VM& vm, script::Object* timeline, GridObj* grid, uint32_t item);
void timelineRecordVariation(script::VM& vm, script::Object* timeline, GridObj* grid, uint32_t item, uint32_t variation);
void timelineRecordName(script::VM& vm, script::Object* timeline, GridObj* grid, uint32_t item, const std::string& name);
// addBrushAction(type, radius, x, strength, z) (L:0x081a7590): a dab of a brush; extends the newest action when it is a
// brush of the same type, radius and strength and nothing else was recorded since
void timelineRecordBrush(script::VM& vm, script::Object* timeline, int type, int radius, int x, int strength, int z);
void timelineRecordWater(script::VM& vm, script::Object* timeline, int x, int z, bool on);

}  // namespace regs::engine
