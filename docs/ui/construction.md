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

## 6. Pieces: templates by connections, item fields

**Item fields used by the laying code** (`gsGridItem`, from the template in the ctor `L:0x08187fc0`): +0x2c/+0x30/+0x34
x, y, z (y in 1/256 cells with the 0x200 shift), +0x38/+0x40 footprint after the orientation swap, +0x3c height·256,
+0x44 orientation, +0x48 flags (item | template), +0x4c `connections` turned by `gsRotateConnections(c, orient·2)`,
+0x50 `connections2` turned the same, +0x54 `diagonals` (nibbles turned by orient·4), +0x58 type, +0x1c parent,
+0x18 the next item of a `getObjects` list, +0x60 name, +0x6c variation. Template (Linux offsets): +0x1fc width, +0x200
height, +0x204 negHeight, +0x208 length, +0x20c flags, +0x210 connections, +0x214 connections2, +0x218 diagonals,
+0x21c type, +0x220 variation, +0x224/+0x228 offsetx/offsetz, +0x358 minY (default −255). Defaults: width, height,
length 1.

**Connection masks**: one byte per lane group, eight bits per byte for the directions 1 N (−z), 2 NE, 4 E (+x), 8 SE,
0x10 S, 0x20 SW, 0x40 W, 0x80 NW. Small roads and trams use byte 0, large roads byte 1, avenues byte 2, metro and train
tracks byte 3. `connections2` marks the ends of multi-block pieces (bridges, ramps) and, for tracks, exits off the map.

**`gsGrid::getObject(type, variation, connections, connections2, flags)`** (`L:0x0813f520`): a map at grid+0x1d0
filled by `loadGridObject` (`L:0x08149f70.c:620-850`): every template under its key (flags masked with 0xcf0f0:
slopes, bridge, tunnel, special0/1) with orientation 0 and, unless both masks are 0, under the masks turned by 90°
(`rot(·, 6)`) with orientations 3, 2, 1 (only one turn when both masks are 180°-symmetric). A later template with the
same key takes the entry (the orientation stays). This is how every piece is chosen: there are no names in the code.

**`createRoadItem(x, y, z, conn, conn2, type, variation, flags)`** (`L:0x0818c3b0`): the exact template, else the
type's plain piece (`(type, var, 0, 0, flags & 0xc000)`, `(type, var, 0, 0, 0)`, `(type, 0, 0, 0, 0)`: the "orphan"
pieces) marked as an error (0x200); the block corner (x, z) is moved by the template's offsets for its orientation
(blocks are 4 cells, alleys 1; `getRoadXZ` `L:0x08181510` is the inverse); bit 2 of `flags` is copied into the item
(a replacement piece, see below). Pieces with bridge, tunnel or stop flags are added to the grid at once
(`gsGrid::addObject`, off the map they get 0x200), the others are loose items until committed. A ctor with
y = −0x8000 (or ENABLE_BASE) runs **`fixY`** (`L:0x08187760`): ENABLE_BASE: the highest corner (stations rounded up to
0x80, docks 0x100/0x200); bridges without cover: the highest corner + 0xc0, rounded; tunnels: the lowest − 0x1c0;
LOCK_GROUND: the mean of the corners it stands on (a locked corner, cell flag 0x1000, wins), rounded to 0x80 (alleys
0x40); with a parent its height; else the mean; never below minY.

**`checkRoadSlopes(step)`** (`L:0x0818c840`): over the preview in order, pieces without parent and without bit 2 that
have a 1:8 slope variant follow the previous one by at most `step` (0x100 trams, 0x80 metro), taking the middle height
of their neighbours; a piece without a slope variant keeps the previous height. Backwards, a piece whose neighbour
lies higher or lower (looking two pieces ahead) becomes the slope piece (1:16/1:8 for roads of types 5-7, 1:8/1:4
for the others, by the step) at the lower height, turned toward the lower side. A bit-2 piece at another height is an
error.

## 7. Laying tram tracks (GRIDMODE_LAY_TRAM_TRACK, 0x11)

**mouseMoveLayTram** (`L:0x08193b00`): the pick (`intersectGrid` with items) gives a cell; its 4×4 block is the
cursor's block (on the odd blocks of the 8-cell checkerboard the pick is pushed to the nearer neighbour, which only
changes the cache key +0x1ac). Unchanged block: nothing. Else: without `inProgress` the start is this block (a
single-piece hover preview); the replaced set is emptied, the old preview's areas unselected and its items removed.
Then the walk from the start block to the cursor's block, one block per step:

* The road of a block: the first item over its centre 2×2 of types 2-6 with COVER (after a bridge piece: one at the
  previous height with `connections2`). A large road or avenue advances two blocks along its axis, and the incoming
  connection goes to its second (large) or third byte (avenue) when the piece has the lanes for it. A multi-block
  piece is resolved to the block under the walk; on 8-wide roads the start and end snap to the road's blocks.
* A slope or bridge piece is passed as a whole: the walk continues behind its far end (and the drag's end is pushed
  there); a drag starting on one starts at its end facing the drag unless that end already has a tram (`checkTram`
  `L:0x0818d690`). A block whose road piece does not start there, or a road with DISABLE_TRAM, is an error. Blocks
  without road get a free-standing track.
* The step: along the longer remaining axis, diagonal while both are about equal (within 4 cells on two-block roads).
* **createTramBlock** (`L:0x081926b0`): type 10/11/12 by the road (small, large, avenue). At the ends of the drag
  (first and last block) the tram tracks within 7 cells are joined: a track on the same block of the same type is
  merged (the connections ORed, the piece replaces it, bit 2); a neighbour along an axis (4 or 8 cells away, in the
  lane byte of its road kind) or diagonal (also the large roads' 8-cell diagonals, SPECIAL0/1 pieces) that has no
  connection at 45° or 90° next to the new one gets the junction piece of its connections plus the one toward us
  (a replacement with bit 2, same parent, its old piece replaced) and our piece the connection back; stops are never
  joined, and a joint whose pieces have no template is dropped. Then the piece: on a flat road at the road's height,
  on a sloped road its slope variant turned with the road, always a child of the road (with the road's name). The end
  block's piece is inserted between the replacement pieces made for the start and those for the end.
* Afterwards `checkRoadSlopes(0x100)`, then per piece `gsGrid::checkConflict(piece, replaced)` and the conflict with
  the piece before; one error makes every piece red (`selectObjectArea(piece, error)`), then event 22.

## 8. Laying metro tracks (GRIDMODE_LAY_TRACK, 6)

`toolMode` picks the kind: GRIDMETRO_GROUND (7) → type 0xd, SUBWAY (8) → 0xe with TUNNEL flags, ELEVATED (9) → 0xf
with BRIDGE flags (other values: 9, double track). **mouseMoveLayTrack** (`L:0x0819cc70`):

* The pick: underground, `intersectGrid` with the height `toolSize << 9` (the level bar: −2, −3, −4 → −0x400,
  −0x600, −0x800): only tunnel items are hit and the terrain is the plane at that height; elevated, the ground plus 2
  cells, the top of the item hit + 1 cell (rounded to 0x80), or an elevated or ground track's own height; ground
  tracks take their height from fixY. The cache key is block x, z and that height.
* The walk: from the start block toward the cursor's block, straight or 45° diagonal (diagonal while |Δx| = |Δz|), never
  turning more than 45° per block (a sharper direction is clamped to 135° from the way back). Each block gets
  `createTrackBlock(x, z, (back | forward) << 24, isStart, isEnd, 0, y)`; underground the blocks between the ends lie
  at the level, elevated at the start's height while that is lower than the cursor's.
* **createTrackBlock** (`L:0x08195400`): at the map's edge the piece gets `connections2` toward the edge (W 0x40 at
  x = 0, E 4 at x = width − 4, N 1, S 0x10) and SPECIAL0 (0x40000): the map-exit pieces. At the drag's ends the
  tracks within 5 cells of the same kind (ground and elevated join each other; same height underground and elevated;
  no stops) are joined as for trams: a track on the block is merged (a drag starting on a track end turns its first
  connection to meet it at 135° and returns it, so the walk turns too), axis and diagonal neighbours get their
  junction piece. Same insertion order as trams.
* Elevated and underground pieces then rise over the items under them (not the replaced ones; one cell clearance)
  and two cells over the ground and the water (0x200), and neighbours differ by at most half a cell (forward and
  backward); then `checkRoadSlopes(0x80)`.
* **placeObject** (`L:0x08196b00`) for every piece: the template's `onPlace(parts, x, y/256, z, orient)` lists five
  values per part: a template name or a road/track type (| variation << 16), x, y (cells), z and the orientation (for
  blocks: the connections), in the item's frame; names become TEMP children (0x2000000) of the piece — the elevated
  pillars `metro_elevated_pillar` —, types become road or track blocks (both ends joined).
* **checkConflicts** (`L:0x0818d5c0`): an optional piece (pillars) that conflicts is hidden; any other conflict makes
  the preview red. Event 22.

## 9. Committing (finishTool 5/6/0x11) and cancelling

Without `inProgress` only the 12×12 cells around the cursor's block are unselected. Else (§5) the previews are checked
again, the replaced items removed and the previews mounted; a mounted piece without parent softens the ground around
its centre: **softenArea(cx, cz, 4, 0x10)** (`L:0x0818cfb0`) gives every unlocked corner in the disc of radius 4 the
mean of itself and its four neighbours. The engine copies the heights once and then makes 16 passes from that copy,
which equals one pass. Not while the grid shows markers (+0x22c, the editor). **cancelTool**: the previews' areas
unselected, the previews removed.

The script side (§3) prices `getNewItems` minus `getReplacedItems` (a joined neighbour costs its new piece minus the
old one), pays and calls `applyTool`. The dead-end pieces (`*_deadend`, `*_orphan`) show a tracer sign from their
`onCreateInstance` and remove it in the instance's `onDestroy`.

## 10. Port status

`port/src/engine/tools.h` holds the state, the dispatch (`registerToolMode(mode, {move, finish, cancel})`), the
natives of §4 and the helpers: `toolAddPreview`, `toolReplace`, `toolClearPreview`, `toolCommit`,
`gridMountObject`, `gridCheckConflict`, `gridSetSelected`, `gridSelectArea` / `gridUnselectArea`,
`rotateConnections`, `toolPick`. The select mode registers from `interaction.cpp`. The highlight is one overlay mesh
per map (`buildHighlightOverlay`, `grid_items.cpp`), drawn with the grid's positive/negative materials. Tool previews
do not reach the simulation (`grid_edit.cpp`). `REGS_DEBUG_HIGHLIGHT=1` highlights the cells and item under the cursor
in any mode (overlay test).

Deviations: the pairwise conflict test (`gridItemsConflict`, `grid_edit.cpp`) follows `gsGridItem::checkConflict`
(`L:0x08183aa0`: relations, stop rules, tram tracks on their road's layer, bridges, tunnels, pillars, ground pieces
meeting at their corners, height ranges) but does not read the templates' per-cell masks (+0x274), so overlapping
covered cells always conflict; no timeline actions or undo are recorded (the game has no timeline object).

**Tracks** (§6-§9) are `port/src/engine/tool_tracks.cpp` (modes 6 and 0x11, `toolBlocks().track` / `.road`). A
preview is built as a list of pieces (as the engine's loose items) and added to the map as TEMP items in one batch;
rebuilding a 50-block preview takes 2-4 ms. `toolBlocks().track(x, z, conn, a, b, type, y)` appends to the calling
tool's `newItems` (variation: the player's `toolType`). Deviations:
* `createRoadBlock` (`L:0x08190700`, roads: editor only; the game's objects have no road parts) lays the plain piece
  of the connections without joining its neighbours. The double track's road crossings (createTrackBlock type 9) and
  the editor's bridge and tunnel modes (0xb, 0xc) are not ported.
* All preview pieces are TEMP items in the map (the engine adds only bridge, tunnel, stop and auto-child pieces to
  it); `gsGrid::checkConflict` therefore also sees the preview's other pieces through `gridItemsConflict` (related
  and sibling pieces do not conflict).
* As in the engine, checking a preview hides the optional items (trees) it conflicts with, also when the preview is
  then moved away or cancelled; the port does not draw the highlight of hidden items (the engine took them out of
  their division).
* The subway level is picked as in the engine, but the port has no underground view yet to see the tracks.

Framework changes made with the track tools: `gridSelectArea`/`gridUnselectArea` take the items overlapping the area
(engine getObjects) instead of those with their corner in it; new `gridGetObjects`, `gridItemsOverlap`,
`gridSelectObjectArea`, `gridItemRect`; `toolCommit` unselects each preview's area (its cells kept the highlight)
and has an `onMounted` hook; `gsGrid::checkConflict`'s join test reads `connections2` (+0x50); mounted copies drop
bit 2; TEMP items keep bit 2 (`gridEditAddItem`); `toolPickLevel` / `ToolPick::y` (intersectGrid's height in/out);
`GridObj::templateOrder` (loadGridObject order for the search map); template height defaults to one cell.
