# Construction tools: the grid player's editing modes

How `gsGridPlayer` builds, places and removes things: the tool state, the mouse protocol, previews, conflicts and
committing. Selection (mode 1) is in `selection.md`. Linux build addresses (`L:`), functions in
`decompiled/Linux/functions/`. Port: `port/src/engine/tools.h/.cpp` (framework), one file per mode family.

## 1. Modes

`gsGridPlayer.editMode` (`GRIDMODE_*`): 1 select, 2 shift / 3 level / 4 soften terrain, 5 lay road, 6 lay (metro)
track, 7 place object, 8 remove objects, 9 add stop (line panel), 0xa create stop, 0xb bridge, 0xc tunnel, 0xd toggle
water, 0xe grow trees, 0xf pavement, 0x10 field, 0x11 lay tram track, 0x12 lay alley, 0x13 ruin, 0x14 rename,
0x15 variation.

The game's build menu (`metro/scripts/game.script:56-58, 582-636`) uses only: select, bulldoze (8, toolMode 1), create
stop (bus and tram stops, `toolMode` = `GRIDSTOP_*`, `toolType` = stop variant), lay tram track (0x11), lay track (6,
`toolMode` = `GRIDMETRO_GROUND/SUBWAY/ELEVATED`), place object (7: metro transition `metro_subway_ground`, stations
`station_metrog0/u0/e0`, `dock`, `heliport`; `toolObject` = the template, `toolObject2` = its "b" (diagonal) variant),
and add stop (9) from the line panel. Roads, bridges, tunnels, alleys, terrain and paint tools are editor-only
(`metro/scripts/editor.script:31-147`). `toolbar.script:142-163` `selectTool(tool, mode, variation, sizeable,
levelable, texture)` sets `editMode`, `toolMode`, `toolType`, `toolLine = null` and `toolSize`.

## 2. Tool state (gsGridPlayer fields)

| Offset | Port (`ToolState`) | Meaning |
|---|---|---|
| +0x248 | `editMode` field | the mode |
| +0x24c / +0x250 | `orient`, `orientExtra` | placement orientation (right click turns it in mode 7) |
| +0x254 | `brush` | brush radius (terrain tools) |
| +0x258 | `inProgress` | the left button started the tool (a drag or a placement) |
| +0x259 | `right` | right button held (rename, variation, brushes) |
| +0x25a | `middle` | middle button held: the camera drags, the tools do nothing |
| +0x1a8..+0x1b4 | `cellX/Y/Z/W` | the cell of the last pick; −1 makes the next move rebuild the preview |
| +0x1b8 / +0x1bc | `toolX/Y` | press (or release) position, player-local |
| +0x25c..+0x268 | `startX/Y/Z/W` | the cached cell when the button went down |
| +0x270 / +0x274 / +0x278 | `toolObject`, `toolObject2`, `toolLine` fields | template(s) to place, the line for mode 9 |
| +0x27c | `newItems` | preview items (`vector<gsGridItem*>`), in creation order |
| +0x288 | `replaced` | items the tool would remove (`set<gsGridItem*>`) |
| +0x2a0 | `hoverObject` | hovered controller |
| +0x2a8 | `timeline` field | the editor's timeline; null in game (no timeline actions are recorded) |

## 3. Mouse protocol

**mouseMove** (`L:0x0819f2a0`): nothing while `middle`; else the mode's case (`mouseMoveSelectObject`,
`mouseMoveLayRoad`, `mouseMoveLayTrack`, `mouseMovePlaceObject`, `mouseMoveCreateStop`, `mouseMoveLayTram`,
`mouseMoveLayAlley`, inline code for 2/3/4/8/9/0xb/0xc/0xd/0xe/0xf/0x10/0x13/0x14/0x15); then the gsControl base. A
mode rebuilds its preview only when the picked cell differs from the cached one, then fires its hover event: 22
`onHoverConstruct` (building modes), 23 `onHoverDemolish(mouseDown)` (8), 26 `onHoverAddStop(stop)` (9), 18
`onHoverRename` / 19 `onHoverVariation`.

**mouseDown** (`L:0x0819c410`), left (143), unless `inProgress`: store `toolX/Y`; `inProgress = true` except in
mode 8; copy the cached cell to the start cell and reset it; re-run mouseMove at the press point; then modes 1, 0xd,
0x14, 0x15 (mask 0x302002) clear `inProgress` and fire 24 `onTryApplyTool` at once, mode 8 fires 24 (and the hover
event's script applies while the button is held). Modes 0xe/2/0x13 open a timeline group or an undo snapshot first
(editor). Right (145): mode 7 turns `orient` ((o+3)&3, or (o+7)&7 with a `toolObject2`), cancels and re-picks;
0x14/0x15 set `right`; brushes 0xf/0x10 start a right-button paint; every other mode cancels a running tool
(`inProgress`) and re-picks. Middle sets `middle`.

**mouseUp** (`L:0x0818b1e0`): modes 1, 8, 0xd (mask 0x2102) only clear `middle`. Others: left up with `inProgress`
(or right up with `right`) stores the position and fires 24; for 0x14/0x15 (and 0xe, which ends its timeline group)
right up clears `right` and re-picks instead.

**The script** (`metro/ui/hoverinfo/hoverinfo.script`): 22/23 price the preview (`getPrice` per template of
`getNewItems()` minus `getReplacedItems()`, `MAIN.$economy.getPrice`; demolishing player structures costs the
campaign's `demolishCost` %, the city's buildings a quarter) and show it; 24 → `tryApplyTool`: `canApplyTool()` else
`resetTool()` + error sound; mode 8 checks stops in use and asks before large demolitions; `spendMoney($price)`; names
`getNewObjects()[0]` (stops: `findStopName`, stations: `findStationName`); effects and sounds; `applyTool($mouseMove)`;
`updateCompanyValue()`.

## 4. Natives

| Native | Engine | Behaviour |
|---|---|---|
| `canApplyTool()` | `L:0x0818bb80` | false if a new or replaced item has flag 0x200 (negative) |
| `applyTool([repick])` | `L:0x0819cbe0` | `finishTool()`; re-run mouseMove at `toolX/Y` unless false is passed; returns finishTool's result |
| `resetTool()` | `L:0x0819cbc0` | `cancelTool()` |
| `getNewItems()` / `getReplacedItems()` | `L:0x0818b430` / `L:0x0818bc00` | the template (`gsGridObject`) of each new / replaced item |
| `getNewObjects()` / `getReplacedObjects()` | `L:0x0818b360` / `L:0x0818bcc0` | their controllers (items without one are skipped) |

## 5. Previews, highlight, conflicts, committing

* **Preview items** are real grid items with flag TEMP (0x100000), added by `gsGrid::addObject(template, x, y, z,
  orient, flags | 0x100000, parent, check)` and kept in `newItems`. They are drawn (TEMP items are always visible), not
  picked, not in the cell cover, not simulated. Parts a template adds through `onPlace` (event 10, `placeObject`
  `L:0x08196b00`: 5 values per part: name, y, x, z, orientation; road and track parts become blocks) are previews
  too, flagged 0x2000000 with the placed item as parent.
* **Highlight**: `gsGridItem::setSelected(sel, negative)` (`L:0x08185a70`) sets item flags 0x100 (selected) and 0x200
  (negative = red, also the error flag); `gsGrid::selectArea(x, z, w, l, negative)` (`L:0x08141df0`) sets cell flags
  0x4000 (+0x8000 for red) and selects the items whose corner lies in the area; `unselectArea` clears both
  (`& 0x3fff`). The terrain draws highlighted cells with `positiveMaterial` (green, 0x5000FF00) or `negativeMaterial`
  (red, 0x50FF0000), `selection.shader` decals; highlighted items get a layer copy with the same materials.
* **`gsGrid::checkConflict(item, exclude)`** (`L:0x0813f5e0`): true when the item has 0x200, lies off the map, breaks
  its type's terrain rule (metro stations and covering types 0x11-0x14: at least half the cells dry; a non-covering
  STOP of type 0x11 (dock): at most a quarter dry; tram track types 10-13 without parent: half dry and within 5·128 of
  the mean ground height), is a tunnel (0x8000 without COVER) with ground lower than its top anywhere, or a bridge
  (0x4000 without COVER) with ground above it or more than 0xa00 below; or when a live item around it (not in
  `exclude`, nor a descendant of one) conflicts pairwise (`gsGridItem::checkConflict`). An optional item (0x200000)
  in such a conflict is hidden (0x400000) instead: the other one when the checked item is required, the checked one
  when it is optional itself. An item with flag 2 and connections conflicts unless a neighbour joins it along an
  edge (n/e/s/w connection bits 1, 4, 0x10, 0x40 against 0x10, 0x40, 1, 4) at the same height.
* **Committing a laying mode** (finishTool cases 5, 6, 0xb, 0xc, 0x11, 0x12, `L:0x0819a3e0.c:134-298`): if nothing was
  started (`inProgress` false) only the 12×12 cells around the cursor are unselected. Else: any preview conflicting
  (against the map without `replaced`, or with the preview before it) cancels the build; otherwise the replaced items
  are removed and each preview is mounted (`gsGrid::mountObject` `L:0x08148470`: a permanent copy in its division
  with a new id, optional overlaps removed, handed to the citizens' simulation, checked once more so remaining optional
  conflicts hide, then the template's event 12 `onCreateInstance($instance)`); items on the ground (no parent) soften
  the terrain around them (`softenArea(cx, cz, 4, 0x10)`) unless the grid is in editor mode (+0x22c). The previews
  are removed either way, and `replaced` is cleared.
* **cancelTool** (`L:0x0819bcc0`) removes the previews (laying, placing, stops: unselect their area, remove them),
  clears `replaced`, and for mode 9 removes the line's temporary stops; it always ends with `finishTool()`.
* **finishTool's common tail**: clears `inProgress`, `right`, `middle`, the cached cell and the hover; event 25
  `onAddStop` when mode 9 added stops.

## 6. Port status

`port/src/engine/tools.h` holds the state, the dispatch (`registerToolMode(mode, {move, finish, cancel})`), the
natives of §4 and the helpers: `toolAddPreview`, `toolReplace`, `toolClearPreview`, `toolCommit`,
`gridMountObject`, `gridCheckConflict`, `gridSetSelected`, `gridSelectArea` / `gridUnselectArea`,
`rotateConnections`, `toolPick`. The select mode registers from `interaction.cpp`. The highlight is one overlay mesh
per map (`buildHighlightOverlay`, `grid_items.cpp`), drawn with the grid's positive/negative materials. Tool previews
do not reach the simulation (`grid_edit.cpp`). `REGS_DEBUG_HIGHLIGHT=1` highlights the cells and item under the cursor
in any mode (overlay test).

Deviations: the pairwise conflict test is the port's `gridItemsConflict` (`grid_edit.h`, simplified); committing
does not soften the terrain yet; no timeline actions or undo are recorded (the game has no timeline object).
