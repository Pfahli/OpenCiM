#pragma once
#include "script/vm.h"

namespace regs::engine {

class ControlObj;
class Display;

// The in-game minimap (docs/ui/minimap.md). Natives: gsGrid.renderMap, gsMapCtrl.worldToMapPosition /
// mapToWorldPosition and the template place counts (addHomePlaces, addWorkPlaces, addVisitPlaces, preventVisitors).
void registerMinimapNatives(script::VM& vm);

// GUI pass hooks for the map controls (called by display.cpp's GUI pass):
// the gsMapCtrl image's custom0 for minimap.shader (§4.3); false for other controls
bool mapImageParams(script::VM& vm, ControlObj* c, float custom0[4]);
// gsMapItemCtrl: moves the pin onto its target (§6), before it is drawn
void placeMapPin(script::VM& vm, ControlObj* c);
// gsMapViewportCtrl: draws the main camera's ground footprint (§5); true when c is one (it has nothing else to draw)
bool drawMapFootprint(Display& d, script::VM& vm, ControlObj* c);

}  // namespace regs::engine
