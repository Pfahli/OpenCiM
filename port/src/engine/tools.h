#pragma once
// The grid player's editing tools (docs/ui/construction.md): the shared tool state of gsGridPlayer, the per-mode
// dispatch of mouseMove / mouseDown / mouseUp / finishTool / cancelTool, and the helpers every mode builds on: preview
// items (temporary grid items, flag TEMP 0x100000), the set of items a tool would replace, the green/red highlight of
// cells and items (gsGrid::selectArea, gsGridItem::setSelected), gsGrid::checkConflict and gsGrid::mountObject.
//
// A mode is a set of functions registered with registerToolMode; the framework calls them where the engine's switch
// statements do (L:0x0819f2a0 mouseMove, L:0x0819a3e0 finishTool, L:0x0819bcc0 cancelTool). The script side of every
// tool is the same (hoverinfo.script): a mode's move builds the preview and fires its hover event (22 onHoverConstruct,
// 23 onHoverDemolish, 26 onHoverAddStop); a click or release fires 24 onTryApplyTool; the script checks canApplyTool,
// prices getNewItems / getReplacedItems, pays, names getNewObjects and calls applyTool, which runs finishTool.
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "render/math3d.h"
#include "script/vm.h"

namespace regs::engine {

class ControlObj;
class GridObj;

// GRIDMODE_* (gsGridPlayer.editMode)
enum ToolModeId : int {
  kGridModeNone = 0,
  kGridModeSelectObject = 1,
  kGridModeShiftTerrain = 2,
  kGridModeLevelTerrain = 3,
  kGridModeSoftenTerrain = 4,
  kGridModeLayRoad = 5,
  kGridModeLayTrack = 6,
  kGridModePlaceObject = 7,
  kGridModeRemoveObjects = 8,
  kGridModeAddStop = 9,
  kGridModeCreateStop = 10,
  kGridModeLayBridge = 0xb,
  kGridModeLayTunnel = 0xc,
  kGridModeToggleWater = 0xd,
  kGridModeGrowTrees = 0xe,
  kGridModeSetPavement = 0xf,
  kGridModeSetField = 0x10,
  kGridModeLayTramTrack = 0x11,
  kGridModeLayAlley = 0x12,
  kGridModeRuin = 0x13,
  kGridModeRename = 0x14,
  kGridModeSetVariation = 0x15,
};

// item flags the tools use (gsGridItem +0x48)
constexpr uint32_t kItemSelected = 0x100;      // highlighted (setSelected)
constexpr uint32_t kItemNegative = 0x200;      // highlighted red; an error: canApplyTool fails, checkConflict conflicts
constexpr uint32_t kItemTemp = 0x100000;       // a preview item of a tool
constexpr uint32_t kItemOptional = 0x200000;   // trees, props: hidden or removed under new items
constexpr uint32_t kItemHidden = 0x400000;     // not drawn, not in the cell cover
constexpr uint32_t kItemAutoChild = 0x2000000; // generated part of its parent (pillars, a station's extra tracks)

// gsGridPlayer's tool state (fields +0x1a8 .. +0x2a8). Positions are player-local, as the engine's mouse handlers get
// them.
struct ToolState {
  std::vector<uint32_t> newItems;  // +0x27c: the preview items (TEMP), in creation order
  std::set<uint32_t> replaced;     // +0x288: live items the tool would remove when applied
  // port: live stops the create-stop preview would upgrade in place (regsGetUpgradedItems; the hover script's port
  // extension credits their demolition refund against the new stop's price)
  std::vector<uint32_t> upgraded;
  bool inProgress = false;         // +0x258: the left button started a tool (drag, placement)
  bool right = false;              // +0x259: right button held (rename, variation, brushes)
  bool middle = false;             // +0x25a: middle button held: the camera drags, the tools rest
  float toolX = 0, toolY = 0;      // +0x1b8 / +0x1bc: where the tool was pressed (or released)
  // +0x1a8 .. +0x1b4: the cell of the last pick (x, y, z, extra); −1 forces the next move to rebuild its preview
  int cellX = -1, cellY = 0, cellZ = -1, cellW = 0;
  // +0x25c .. +0x268: that cell when the button went down (the start of a drag)
  int startX = -1, startY = 0, startZ = -1, startW = 0;
  // +0x24c / +0x250 / +0x254 are the script fields toolMode, toolType and toolSize (the player's fieldValues): in
  // GRIDMODE_PLACE_OBJECT toolMode is the orientation (right click turns it) and toolType the height offset; the
  // brush tools read toolSize as their radius. These three stay for modes that keep a private copy.
  int orient = 0;
  int orientExtra = 0;
  int brush = 0;
  int buttons = 0;  // the mouse buttons of the mouseMove being handled (gsGridPlayer::mouseMove's mask: 1 = left)
  // port: the place tool's free turn (Shift + mouse wheel, toolWheel): radians on top of toolMode's steps, within half
  // a step; turnable when the previewed object may turn freely (no connections, no parts); turnObject: the toolObject
  // it was set for (another object starts at 0)
  float yaw = 0;
  bool turnable = false;
  const script::Object* turnObject = nullptr;
  float lastX = 0, lastY = 0;  // the last mouseMove's position (player-local), to rebuild the preview after a turn
  // port: the preview snapped to a neighbour off the grid (tool_objects.cpp snapToNeighbour): its orientation, turn and
  // shift, and the total turn in degrees for the cursor line
  bool snapped = false;
  int snapOrient = 0;
  float snapYaw = 0, snapOffX = 0, snapOffZ = 0, snapAngle = 0;
  // port: objects getReplacedObjects returns after the replaced items' controllers (the rename tool's stand-in for
  // curves; kept alive by the tool that sets them)
  std::vector<script::Object*> extraReplaced;
};

// port: Shift + mouse wheel over the player (the control under the mouse; notches, positive away from the user; modifiers: the desktop's 0x100 Shift, 0x200 Ctrl)
// turns the place tool's object freely, 5° per notch, 0.5° with Ctrl too. False when no tool takes it (the wheel then
// zooms as usual).
bool toolWheel(script::VM& vm, ControlObj* player, float notches, int modifiers);
ToolState& toolState(script::Object* player);

// A mode's functions. Missing ones fall back to the framework's defaults (noted per field).
struct ToolMode {
  // mouseMove's case: build or update the preview for the mouse at (x, y), fire the mode's hover event. Default: none.
  std::function<void(script::VM&, ControlObj* player, GridObj* grid, float x, float y)> move;
  // finishTool's case: apply the tool; its result is applyTool's. Default: toolCommit.
  std::function<bool(script::VM&, ControlObj* player, GridObj* grid)> finish;
  // cancelTool's case: drop the preview. Default: toolClearPreview. cancelTool then runs finishTool, as the engine.
  std::function<void(script::VM&, ControlObj* player, GridObj* grid)> cancel;
  // mouseDown's case for the left button, before the tool starts (toolX/toolY are the press point, cellX/cellZ still
  // the hovered cell): the shift and ruin brushes keep an undo copy of their area, grow trees opens a timeline group.
  // Default: none.
  std::function<void(script::VM&, ControlObj* player, GridObj* grid)> down;
  // port: the left button's release for modes that apply on their own (the editor's templates) instead of firing
  // onTryApplyTool to the hover script (no price). Default: none.
  std::function<void(script::VM&, ControlObj* player, GridObj* grid, float x, float y)> up;
  // port: the right button's press for modes that handle it themselves (the curve tool: cancel, remove). Default: none
  // (the framework cancels a running tool).
  std::function<void(script::VM&, ControlObj* player, GridObj* grid, float x, float y)> right;
};
void registerToolMode(int mode, ToolMode m);
// a registered mode's functions (empty when none), for a port wrapper around it
ToolMode toolModeOf(int mode);
// port: the map editor's curved-road tool (engine/tool_curves.cpp)
void registerCurveTool(script::VM& vm);

// softenArea(cx, cz, r, 1) (L:0x0818cfb0): one pass of the 5-tap mean over the unlocked corners of a disc (tool_tracks)
void toolSoftenArea(GridObj* grid, int cx, int cz, int r);

// gsGridPlayer::createTrackBlock / createRoadBlock (L:0x08195400 / L:0x08190700): the preview pieces of track or road
// for one cell and connection mask, as the laying tools make them and as placeObject makes a placed object's road and
// track parts (L:0x08196b00.c:289, :359). The track tools register them; they are empty until then.
struct ToolBlocks {
  // (player, grid, x, z, connections, flagA, flagB, track type, y): createTrackBlock(x, z, conn, a, b, type, y)
  std::function<void(script::VM&, ControlObj*, GridObj*, int, int, uint32_t, bool, bool, int, int)> track;
  // (player, grid, x, z, connections, flagA, flagB, variant, y): createRoadBlock(x, z, conn, a, b, variant, y)
  std::function<void(script::VM&, ControlObj*, GridObj*, int, int, uint32_t, bool, bool, int, int)> road;
};
ToolBlocks& toolBlocks();

// ---- helpers for the modes ----

// the player's grid (gsGridPlayer.grid)
GridObj* toolGrid(script::VM& vm, script::Object* player);

// a temporary preview item: gsGrid::addObject(template, x, y, z, orient, flags | TEMP, parent) as the tools call it
// (y in the engine's 1/256 cells, as cell heights), appended to newItems. Returns the item, −1 for an unknown template.
int toolAddPreview(script::VM& vm, script::Object* player, GridObj* grid, const std::string& templateName, int x, int y,
                   int z, int orient, uint32_t flags, int parent = -1);
// remove every preview item (and its highlight), clear the replaced set (cancelTool's case for the building modes)
void toolClearPreview(script::VM& vm, script::Object* player, GridObj* grid);
// mark a live item as replaced: it is highlighted (red when negative) and listed by getReplacedItems
void toolReplace(script::VM& vm, script::Object* player, GridObj* grid, uint32_t item, bool negative = false);
// finishTool's case for the laying modes (L:0x0819a3e0.c:134-298) and the core of placing: when no preview item
// conflicts (gsGrid::checkConflict against the map without the replaced items, and each with the one before it), the
// replaced items are removed and every preview item is mounted; the previews are removed either way. Returns true.
// onMounted(preview, mounted) runs after each mount (the laying modes soften the ground there).
bool toolCommit(script::VM& vm, script::Object* player, GridObj* grid,
                const std::function<void(uint32_t preview, uint32_t mounted)>& onMounted = {});

// gsGrid::mountObject (L:0x08148470): a permanent copy of a preview item (TEMP cleared, the controller's name carried
// over), its optional overlaps removed; returns the new item or −1
int gridMountObject(script::VM& vm, GridObj* grid, uint32_t tempItem);
// gsGrid::checkConflict(item, exclude) (L:0x0813f5e0): true when the item cannot stand where it is: an error flag, off
// the map, water or terrain rules of its type, a tunnel above or a bridge below the ground, or a conflicting live item
// that is not in `exclude` (nor below an excluded one). Conflicting optional items are hidden instead (0x400000).
bool gridCheckConflict(script::VM& vm, GridObj* grid, uint32_t item, const std::set<uint32_t>& exclude);
// gsGridItem::setSelected(selected, negative) (L:0x08185a70): the highlight flags of one item
void gridSetSelected(script::VM& vm, GridObj* grid, uint32_t item, bool selected, bool negative);
// gsGrid::selectArea / unselectArea (L:0x08141df0 / L:0x08141cb0): the cells [x, x+w) × [z, z+l) and the items
// overlapping them (gridGetObjects) get (or lose) the highlight; negative = red
void gridSelectArea(script::VM& vm, GridObj* grid, int x, int z, int w, int l, bool negative);
void gridUnselectArea(script::VM& vm, GridObj* grid, int x, int z, int w, int l);
// gsGrid::getObjects(x, z, w, l) (L:0x0813fdb0): the live (not TEMP) items around whose footprint overlaps the area
// (gsGridItem::overlap: the rectangle, and a covered cell for diagonal footprints)
std::vector<uint32_t> gridGetObjects(script::VM& vm, GridObj* grid, int x, int z, int w, int l);
// gsGridItem::overlap(a, b) (L:0x081834e0): footprints and height ranges (y − negHeight·256 .. y + height) overlap
bool gridItemsOverlap(script::VM& vm, GridObj* grid, uint32_t a, uint32_t b);
// gsGrid::selectObjectArea(item, negative) (L:0x08141ae0): the item's covered cells, the item and the live items
// overlapping it get the highlight
void gridSelectObjectArea(script::VM& vm, GridObj* grid, uint32_t item, bool negative);
// an item's footprint (x, z, sx, sz after the orientation swap), for unselectArea over a preview
void gridItemRect(script::VM& vm, GridObj* grid, uint32_t item, int& x, int& z, int& w, int& l);
// gsRotateConnections (L:0x08181230): each byte of a connection mask (8 directions, 45° per bit) rotated right by n
uint32_t rotateConnections(uint32_t connections, int n);

// a pick under the player-local mouse position: gsGrid::intersectGrid without vehicles and characters
struct ToolPick {
  bool hit = false;
  int cellX = -1, cellZ = -1;  // the terrain cell under the cursor (or under the hit item point)
  int item = -1;               // the item hit (after the parent walk), −1 for terrain
  render::Vec3 point;          // the hit point in world space
  int y = -0x8000;             // intersectGrid's height out: the item's y, else the ground rounded ((h + 0x80) & ~0xff)
};
ToolPick toolPick(script::VM& vm, ControlObj* player, float x, float y, bool terrainOnly);
// intersectGrid with a height (planeY != −0x8000): only tunnel items, the terrain as the plane at planeY (subway level)
ToolPick toolPickLevel(script::VM& vm, ControlObj* player, float x, float y, int planeY, bool terrainOnly = false);

// ---- the framework (called by interaction.cpp's player hooks and natives) ----
void toolMouseMove(script::VM& vm, ControlObj* player, float x, float y, int buttons = 0);
void toolMouseDown(script::VM& vm, ControlObj* player, float x, float y, int key);
void toolMouseUp(script::VM& vm, ControlObj* player, float x, float y, int key);
bool toolFinish(script::VM& vm, script::Object* player);
void toolCancel(script::VM& vm, script::Object* player);
void registerToolNatives(script::VM& vm);

}  // namespace regs::engine
