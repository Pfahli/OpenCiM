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
* Boats (0x8000): a smoothed curve through every eighth node.
* Each strip also counts its cells into the coverage map `grid+0x1e8` (transport.md §3.5).

Port: general and helicopter strips (helicopters with 24 steps and only the lift); boats use the general strip; the
coverage counters are not written.

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
* Not ported: `gsGridItemController.setColor` (the stops' company colour on their own mesh), the stop bubbles'
  overlap layout (`gsTracerOrganizer`).
