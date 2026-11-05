# Lines and vehicles: the line panel, the add-stop tool, the vehicle shop and depot

How the game's line and vehicle management works between the scripts (`metro/ui/linepanel/linepanel.script`,
`metro/ui/vehiclepanel/vehiclepanel.script`, `linevehicles.script`, `scripts/ui.script`, `ui/hoverinfo/hoverinfo.script`)
and the engine, and what the port does. Linux addresses (`L:`) are in `decompiled/Linux/functions/`; the simulation
side (segments, the stop cycle, boarding) is `docs/simulation/transport.md`. Port: `port/src/engine/sim_lines.cpp`
(line and vehicle natives), `tool_addstop.cpp` (mode 9), `sim_paths.cpp` (route strips), `ui_lines.cpp` (gauges,
previews).

## 1. Who does what

The panels are script code; the engine only keeps the lines and vehicles. Everything about money, vehicle ownership
and names is script data in `MAIN.$gameData`:

| Script data | Meaning |
|---|---|
| `$gameData.$lines[i]` | `map[index, name, active, visible, vehicles[], new]`, in the order of `grid.lines.children()` (`initLines` pairs them by position, `linepanel.script:1032-1055`) |
| `$gameData.$ownVehicles[id]` | every bought vehicle, `id = "<model>:<n>"`: name, index, condition, travelDistance, passengersTransported, money fields |
| `$line.$data["vehicles"]` | the ids of the line's vehicles, in the order of `line.getVehicles()` |
| depot | the owned vehicles on no line (`showDepotItems`, at most 6 shown) |

So the engine must keep two orders stable: the lines module's children (creation order, deletions removed) and each
line's vehicle vector (append on `addVehicle`, removal on destroy). The save writes lines and vehicles in those orders.

Flows (all script):

* **New line** `createNewLine` → `grid.lines.createLine()`, then `type`, `active = false`, `visible = true`, `color`,
  materials (`addLine`: `pathMaterial` path.shader + `metro/graphics/path.dds` in `$pathColors[type]`,
  `tempPathMaterial` in `$tempPathColors[type]`, both `DECAL | TWOSIDED`).
* **Add stops** `toggleAddStop`: pauses the line, `toolbar.selectTool(GRIDMODE_ADD_STOP, line.type)` (so
  `player.toolMode` = the line type), `player.toolLine = line` (§2).
* **Delete / move stops** `deleteStop` / `moveStop` pause the line first. Deleting on a closed ring opens it first:
  `deleteStop(0)` on a closed ring only opens it; with ≤ 2 stops or stop 0 the script calls `deleteStop(0)` before the
  real deletion (`linepanel.script:866-883`).
* **Activate** `togglePauseLine` refuses open lines and lines without vehicles; sets `active` (achievements, the
  "new line" ticker).
* **Delete line** confirmation dialog, then `delete $line` (the engine object; its vehicles go with it) and the
  `$gameData.$lines` entry.
* **Buy** `purchaseVehicle` (price, `spendMoney`, a new `$ownVehicles` entry), then `doAddVehicle` → `createBus`:
  `line.addVehicle(template)`, `attractiveness`, `condition`, `travelDistance`, `passengersTransported`, trailers
  (`addTrailer`), and the vehicle events (`addBus`: tracer, fares in `onGetPassenger`, repair cost in `onBroken`).
* **Sell / to the depot** `deleteVehicle(id)`: `delete $vehicle`; selling also pays `price · condition · 0.9`.
* **Condition** is script-maintained monthly (`player.script:303-317`: target from travelDistance, reliability and
  maintenance; `condition` and `attractiveness` written to the vehicle); the engine only reads it for the breakdown
  roll (transport.md §4.5). Repairs: `onBroken` sets `stopTime = 120 − 100·employeeHappiness[2]` and charges
  `price · condition · 0.1`; the engine counts it down and fires `onFixed`.

## 2. GRIDMODE_ADD_STOP (9)

### 2.1 mouseMove case 9 (`L:0x0819f2a0.c:599-733`, `W:0x100fb460.c:582-690`)

```
if hoverObject is an item controller of a STOP item (flag 0x2000) whose type serves toolMode
        (0: 2..6, 1: 10..12, 2: 13..15, 3: 0x11, 4: 0x12):
    cell = (item.x + sx/2, item.z + sz/2)                    // the stop tracers of ui.script set hoverObject
else:
    intersectGrid(eye, ray, &x, &z, &y, item = null, …, terrainOnly = false) or return   // items ARE hit (:278),
    cell = (x, z), pickY = y                                                              // just not reported
if cell == cached cell: return
unselectArea of every temporary stop's item; line.removeTempStops()
if no hover stop: the stop items overlapping the 3×3 cells around (gsGrid::getObjects L:0x0813fdb0: non-TEMP
    items of the divisions around whose footprint overlaps), serving toolMode; nearest |item.y − pickY| wins,
    the first on a tie
if a stop: node = line.addStop(item, temp = true); if node: item flag 0x100 (highlight), updateMesh
event 26 onHoverAddStop(controller, or null when there is no stop or addStop refused it)
cache the cell
```

`gsTransportLine::addStop` (`L:0x081c2fa0`, transport.md §3.1) refuses an item already on the line; on an open line
the first stop again closes the ring (a temporary closing marks the first node temporary); on a closed ring the stop
goes into the edge whose detour `d(a,new) + d(new,b) − d(a,b)` is smallest among edges with both new legs reachable.

The hover script (`hoverinfo.script:318-364`) puts a 30 % pin on the hovered stop and shows "add stop" or "close
line". `ui.script:383-387` makes the stop bubbles set `hoverObject` and call `player.mouseMove(0, 0)` in mode 9, and
a click on a bubble applies the tool directly (`:439-449`).

**Port deviation:** with the option `$regsBusAtTramStops` (default on), toolMode 0 also serves tram stops (10..12)
that sit in a street (§6).

### 2.2 finishTool case 9 (`L:0x0819a3e0.c:554-618`)

A click (mouse up → 24 `onTryApplyTool` → `tryApplyTool` → `applyTool` → `finishTool`):

```
for each node from first.next round to first (closed) or the end:
    if temporary and (the leg into it has no path, or it has a next node and its own leg has no path):
        unselect it; removeTempStops; break                // the click does nothing
every remaining temporary node: unselect its item, clear flag 1 → added
rebuild the segment meshes; event 26(null); common tail (hover released)
if added: event 25 onAddStop()                            // after the tail
```

`linepanel.script:194-209` answers 25: selects the new last stop, redraws the stop list, and leaves the mode when
the line is now closed.

### 2.3 cancelTool case 9 (`L:0x0819bcc0.c:194-197`)

`removeTempStops(toolLine)`, then `finishTool` (so event 26 with null). Leaving the mode (`editMode =` anything)
cancels with the old mode first (selection.md §4.5).

**Port deviation:** cancelling also unselects the temporary stops' items; the engine leaves their 0x100 flag set.

## 3. Route strips (`gsTransportSegment::updateMesh`, `L:0x081ba9f0`)

Every segment of a line whose display flag is on (the line's `visible` module bit) has a ribbon mesh in the grid's
decal layer with `pathMaterial`, or `tempPathMaterial` when its node or the next node is temporary. path.shader
scrolls the arrow texture with the timer and mixes the diffuse colour towards white by `custom0.x` (the script sets
it to 1 on the selected line: it pulses).

* General lanes (`:767-860`): per path node after the start platform, the Bézier `p0, p0 + q0·(0,0,−L/3), p1 −
  q1·(0,0,−L/3), p1` at t = 0, ¼, ½, ¾, 1; two vertices one unit either side (u = 0 / 1), v += L/16 per step.
* Helicopters (class 0x4000): one arc between the pads, round(0.6·cost) + 20 steps, control points lifted 100.
* Boats (0x8000, `L:0x081ba9f0.c:466-663` = `W:0x101119d0.c:320-509`, obs H): `N = nodes >> 3` cubic Bézier pieces
  (16 steps each, 17 vertex pairs and 96 indices per piece, no join between pieces), `v += 2` per step:
  * start `A = first node's start`, `B = A + (0, 8, 0)`; the walk first advances `nodes / N` nodes;
  * piece k < N: anchor node = the current one, `D = its start + (0, 6, 0)`; the walk advances to node
    `(k+1)·nodes / N` (kept at the last node at the path's end) and `C = D − (that node's start − A)·0.167`
    (`0x3e2b020c`);
  * piece N: `D = the current (last) node's end`, `C = D + (0, 8, 0)`;
  * after each piece `A = D`, `B = 2D − C` (C1 joins). Each step: point `bezier(A, B, C, D, t)`, side
    `normalize(cross(tangent, +Y))`, vertices `p ± side` with u 0 / 1; the tangent is taken at `t = 1/16` for the
    first point and `15/16` for the last.
  * Legs under 16 nodes (N clamps to 1) advance past the path's end in the first walk (`gsGridPath::getNext`
    `L:0x08175d40` returns null) and get no strip (inf).
* Each strip also counts its cells into the coverage map `grid+0x1e8` (transport.md §3.5).

Port: general, helicopter and boat strips (helicopters with 24 steps and only the lift; boat legs under 16 nodes keep
the general strip instead of none, `engine/sim_paths.cpp`); the coverage counters are not written. Vienna's water bus
before/after: `port/run/gaps/boat-compare.png`.

## 4. GUI pieces the panels need

* **gsGaugeCtrl** (`L:0x081cf040`): every tick `width = offset + arg0 · multiplier` (multiplier +0x19c, ctor 1;
  offset +0x1a0, ctor 0); the vehicle lists bind arg 0 to `passengerCount / capacity` and the condition.
* **Shop previews** (`metro/ui/objectlist/renderer.script`): a 128² `gsTextureTarget_OGL` on a hidden viewport with
  its own scene, sun and `gsFixedCamera`; `resetTexture()` gives the target a new texture per shop item and
  `video.renderViewport(vp)` draws the vehicle template (temporarily owned by the scene) at once; the selected item
  is redrawn every frame turning. Engine defaults this depends on: `gs3DCamera.minDistance/maxDistance` = −1
  (`L:0x080e9c00`, "use the viewport's"), `gsGridObject.width/height/length` = 1 (`L:0x08186210`), viewport
  `clearColor` = 0xFF000000 (`L:0x08204470.c:42`, so the target's colour is cleared each render).
* **Stop bubbles**: the grid fires event 6 `transportStopCreated(controller)` when a STOP item's controller is made
  (`gsGridItem::gsGridItem`, `L:0x08187fc0.c:152-161`; markers get event 7); `ui.script:317-470` makes the bubble
  (passenger count, mood), sets the stop's `attractiveness` from maintenance, and binds `onMoodChange`.

### 4.1 The tracer organizer (`gsTracerOrganizer`) — obs H

`MAIN.$frontHud = new_tracerOrganizer($hud)` (`player.script:51-57`, full-viewport, `clickThroughSelf`); every tracer
created as its child (stop bubbles and direction arrows `ui.script:333, 358`, the line panel's bus tracer and stop
pins `linepanel.script:240, 916`, track end markers) is laid out by it. There is no add method and no state: the
object is a plain gsControl (0x19c bytes, `gsNewModule<gsTracerOrganizer>` `L:0x081d4440`), its ctor
`L:0x081d39f0` only sets the vtables and it registers no fields.

`gsTracerOrganizer::move(dt)` (`L:0x081d14c0` = `W:0x100af3d0`):
1. `gsControl::move` first (it returns 0 → stop), which moves the children: each `gsTracerCtrl::move`
   (`L:0x081cfc00`) projects its target, `setPosition(proj − size·anchor − parent.desktop)`, or `(−2w, −2h)` when the
   point is behind the camera or out of the frustum.
2. Collect (`:47-103`) the children that `dynamic_cast` to gsTracerCtrl (gsPinCtrl too) with `organize` (`+0x1cb`,
   ctor default 1, `L:0x081d38a0.c:29`) and `left > −width`, in a stack-local `multimap<float, gsTracerCtrl*>` keyed by
   `left` (equal keys after earlier ones). Visibility, fade and the screen's other edges are not checked.
3. Three passes (`:104-160`): for each entry A in key order and each later B while `A.left + A.width > B.key` (the key
   is B's left at collection time):
   * `d7 = (B.bottom − A.top)·0.33 + 0.001`, `d6 = (A.bottom − B.top)·0.33 − 0.001`; only when both are > 0:
   * `d5 = (A.right − B.left)·0.33 − 0.002`;
   * `d7 < d6 && d7 < d5`: A down by d7, B up by d7; else `d5 ≤ d6`: A left by d5, B right by d5; else A up by d6,
     B down by d6 (each through the virtual `setPosition`, A's values re-read after every pair).
   Constants as floats: 0.33 (`0x082a916c`), 0.001 (`0x0829df84`), 0.002 (`0x082a54c4`).
4. Nothing is clamped or smoothed over time: the next frame re-projects every tracer and lays them out again.

The scripts switch a bubble out of the layout while the mouse is over it (`ui.script:395-397`: `organize = false`,
`anchorY = 0.5`, `bringToFront`) and back on leaving (`:425-426`). **Port** (`engine/interaction.cpp`
`organizeTracers`, called from the GUI pass in `engine/display.cpp` when it reaches the organizer; its children are
then drawn without being placed again): as above; an unset `organize` counts as true. Before/after on a Berlin
corner: `port/run/gaps/busy-compare.png` (left without, right with).

## 5. Port notes

* Line nodes are vectors in ring order (`LineState::stops`); every edit maps old positions to new ones (passengers'
  global node ids, transfers, docked vehicles, the vehicles' `stopA/stopB`). Edits that remove a node end the trips
  through it; a line edited under running vehicles takes them off when a stop of their leg went or the ring opened
  (they re-enter at a free platform, §4.2) — the scripts pause the line first anyway.
* A ring reduced to one stop is open (the engine keeps it closed onto itself).
* Deleted lines and vehicles stay as tombstones (indices stay valid) and are not saved; the save numbers vehicles in
  line order, as citizens refer to them.
* `active = false` takes every vehicle off the map with `onUnspawn` (statusUpdated of the 0x10000 bit,
  `0x10110840.c`); new vehicles spawn at the first stop of the ring whose platform is free; a vehicle taken off while
  broken is repaired after 60 s without spawning (`0x101152a0.c:32-43`).
* Saves made with community vehicle mods (e.g. `Cim2`) list vehicles in `$availableVehicles` whose templates exist
  only with `--community-addons`; the vehicle panel stopped with "Undefined member $type", as the original does
  without those mods. A port extension (`data/regs/extend/metro/ui/vehiclepanel/vehiclepanel.script`) now leaves
  vehicles without a template out of the shop (the save keeps them), so such saves can buy vehicles without the mods.
* Stops take the company colour through `gsGridItemController.setColor` (renderer/grid-items.md §2.5, "Script
  colour") and show their catchment circle while visible (renderer/terrain.md §5.2); the bubbles are laid out by the
  organizer (§4.1).

## 6. Port QoL: bus lines at tram stops in a street (deviation from the original)

The original's add-stop tool gives a bus line (type 0) only bus stops (item types 2-6, §2.1). The port lets a bus line
also take a **tram stop that sits in a street**, so buses and trams can share a stop. Port option, on by default:
settings key `$regsBusAtTramStops` in `<documents>\Cities In Motion\settings.script` (`= false;` or `= 0;` turns it off;
no menu entry, the only port options page is the graphics one). Saves with a bus line through a tram stop are a
port-only state: the file format is unchanged (a line stop is just an item id), but the original game would look for
a bus platform at such a stop with its plain nearest-STOP-node rule and could pick a wrong lane.

**Which tram stops qualify.** A stop item owns no lanes. The lanes come from the pieces under it: the road piece
(`road_small_*`, `road_large_*`, `road_avenue_*`) and the tram track piece (`tram_small_*`, …). Every road piece has
a road STOP lane (flags `0x340` small road, `0x540` large road and avenue: the curb lane at the middle of the piece;
a bus stop item only marks where buses use it), and the tram track piece has a tram STOP lane (`0x840`) at its
platform. Data from the Vienna and Berlin saves (`REGS_DEBUG_SIM=1` lists every tram stop at load):

| Tram stop kind | Lanes in the stop's footprint | Bus lines |
|---|---|---|
| small road, straight or diagonal (`stop_tram0x_small`, `_smallb` over `road_small_*`) | tram STOP lane = the road's curb STOP lane (same ends, same direction) | qualify |
| large road, straight or diagonal (`_large`, `_largeb` over `road_large_*`) | the tram runs in the curb lane: tram STOP lane = road STOP lane | qualify |
| avenue (`_avenue` over `road_avenue_basic` + `tram_avenue_basic`) | tram tracks in the middle strip; the footprint reaches one carriageway, whose STOP lane is its curb lane on the far side, away from the platform | refused |
| own track (tram piece only, in grass or a tram-only track: Vienna items 71515/71516, Berlin 115954) | no road lane, at most a neighbouring road piece's curb connector | refused |

Rule (`busPlatformInFootprint`, `engine/sim.cpp`): among the STOP lanes whose chord midpoint lies in the stop item's
own footprint, a road-class lane (not parking) whose two ends coincide with a tram STOP lane's (within 0.5 units, the
same direction) is the bus platform; the one nearest to the item centre if there are several. No such pair: the stop
is not for buses. Lanes of neighbouring pieces or of the other carriageway are never considered. The add-stop tool
(`tool_addstop.cpp` `isStopOf`) admits a tram stop for a bus line only with the option on and a bus platform found;
refused stops are not highlighted, as before. The simulation applies the rule to every bus line stop at a tram item
(also with the option off, so a save keeps running): `stopPlatforms` takes that lane as the platform, no alternative
platform (a tram stop serves one direction, like a bus stop), and the legs, the route strip (§3) and the stop cycle are
the bus line's usual ones. A bus halts in the tram's lane at the platform; a tram behind it waits, as behind any
vehicle.

**Passengers and scripts.** Waiting, boarding and alighting are per line node (transport.md §8), so one stop item
serving a tram and a bus line needs nothing new: each line's waiters board that line's vehicles, transfers between
the two lines at the same stop work as between any two stops in reach, and fares are paid once per transport type.
The stop keeps its own type (1, tram): its bubble, colour, name check (`stopExists`), maintenance (`player.script`,
`policy.script`) and the stops roster stay the tram's; the line panel's pins and the roster's per-line rows use the
line's type. `countStops(mask, true)` counts the tram stop for the bus mask too when an active bus line serves it.
One script change: the add-stop hover pin (`hoverinfo.script` `onHoverAddStop`) is coloured by the stop's type; the
extension `data/regs/extend/metro/ui/hoverinfo/hoverinfo.script` recolours it with the tool line's colour.
