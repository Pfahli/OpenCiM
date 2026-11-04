#pragma once
#include "script/vm.h"

namespace regs::engine {

class ControlObj;
class GridObj;

// Picking and selection in the 3D view (docs/ui/selection.md): gsGrid::intersectGrid, gsGridPlayer hover
// (onHoverObject), the select click (onTryApplyTool → applyTool → onSelectObject), editMode, getWorldPosition, and the
// grid controllers (gsGridItemController, gsTransportStopController) scripts get for picked items.
void registerInteractionNatives(script::VM& vm);

// The controller scripts get for a map item (gsGridItemController, or the simulation's gsTransportStopController for a
// line stop), created on first use and kept for the life of the map; the same object hover and selection return.
script::Object* gridItemController(script::VM& vm, GridObj* grid, uint32_t item);
// A map item was removed (live edit): its controller, if any, is destroyed.
void gridItemControllerRemoved(script::VM& vm, GridObj* grid, uint32_t item);

// gsGridPlayer's native mouse handlers (vtable slots 65-67, §3-4), run before the gsControl base behaviour of the
// same call; they do nothing for other controls.
void gridPlayerMouseMove(script::VM& vm, ControlObj* c, float x, float y);
void gridPlayerMouseDown(script::VM& vm, ControlObj* c, float x, float y, int key);
void gridPlayerMouseUp(script::VM& vm, ControlObj* c, int key);

// gsTracerCtrl::move (L:0x081cfc00): before it is drawn, a tracer control (the selection arrow, citizen pins) moves
// so that its anchor lies on its target's screen point in targetViewport, or off screen when the point is not in view
void placeTracer(script::VM& vm, ControlObj* c);

}  // namespace regs::engine
