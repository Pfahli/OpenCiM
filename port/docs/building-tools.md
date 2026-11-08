# Building tools: curves, junctions, roundabouts, one-way streets (OpenCiM, 2026-11)

The port's own road building on top of the game's grid roads, for the map editor first (buildings are placed by hand
there). Modelled on the road tool of Cities: Skylines. The development history, test notes and decisions are in
`curved-roads.md`; this page is the reference: what the tools do, how roads are stored, and how other code (the
OpenStreetMap import) can lay them.

## 1. Using them (map editor)

Everything is in the traffic menu.

| Where | Buttons |
|---|---|
| Alley, small road, large road, avenue tabs | after the game's own buttons: **straight** (any angle), **curve** (start, bend point, end), **freeform** (start, end: keeps the starting direction) |
| Road type **Einbahnstraße / One-way** (top row) | **1 lane**, **2 lanes**; below them the three shapes. Small roads; the way you draw is the way traffic runs |
| Road type **Kreisverkehr / Roundabout** (top row) | **1 lane**, **2 lanes**: click the centre, then click to set the radius (10–40 units) |
| The game's **bulldozer** | also removes curves (shown red); in a game curves are refused, as roads are |

- Under the mouse pointer: what the next click does, the road's length and angle, "(passt nicht)" when it does not fit.
- The preview's cells are marked like the game's tools mark theirs: green fits, red does not (too short, a bend tighter
  than the kind's minimum radius, buildings in the way, an angle under 30° at a junction, no room for a junction).
- After each road the tool draws on from its end; the right button stops. Every change is on the editor's timeline
  (undo works).

### Where roads join

| Pointing at | Result |
|---|---|
| a grid road's dead end (closed side) | the dead end becomes a straight piece the curve continues; the curve leaves or arrives along the road |
| a curve's free end (same road kind) | the curves join smoothly |
| a grass tram track's dead end (small roads) | the curve gets tram tracks that run on into the track; its turning circle lies over the track block |
| the middle of a curve | the curve is cut there and a **junction** made; the new road's direction there is free |
| where curve ends meet already | the new road joins that **junction** |
| the middle of a straight grid block | that block and as many straight neighbours as the angle needs become two straight curves meeting at a **junction** |

### Junctions

Any number of arms (5-way works), at any angle of at least 30°, road kinds mixed (a large road into a small
roundabout); footpaths only among themselves. Each arm stops where the neighbouring arms' edges part (half width /
tan(angle/2) + 1 from the point), and the junction fills the middle: asphalt, rounded pavement corners, kerbs, zebra
crossings on every arm (not over a roundabout's ring) and edge lines, taken from the game's own junction piece. Lanes
run from every arm's incoming lanes into every other arm (lane to lane from the right; a one-way arm only its own way).
The tool refuses a road that the junction would use up (an arm shorter than the junction's radius).

### Road kinds

| Kind | Width | Lanes each way | Min. radius | Notes |
|---|---|---|---|---|
| small road | 12 | 1 (±1.5) | 6 | own mesh; one-way, roundabouts, stops, grass track ends |
| large road | 24 | 2 (±1.5, ±4.5) | 15 | the game's straight piece bent; trams in the outer lanes |
| avenue | 30 | 2 (±6, ±9) | 18 | bent; median; trams in the median |
| alley (footpath) | 3 | – | 3 | bent; pedestrians only |

### One-way streets

Small roads, 1 lane (on the centre line, no divider) or 2 lanes, arrows on every lane every 25 units. Where a one-way
end meets a two-way road its lanes merge into / split from the right-hand lane. Cars and trams never run against it.

### Roundabouts

A ring of four one-way quarter-circle small-road curves, counter-clockwise seen from above (right-hand traffic), 1 or
2 lanes, radius 10–40. Small rings take roads at the compass points (where the arcs meet), larger ones anywhere.

### Tram tracks

- The game's tram track tool lays tracks on a whole curve with one click (priced as street track for its length).
- One-way streets and roundabouts take a single track with the traffic (right-hand lane, or the centre line with one
  lane) at **half** the price; on a roundabout one click lays the whole ring, and trams can circle it to turn around.
- Junctions: tracks between every two tracked arms (rails and tram lanes), one-way arms their way.
- A two-way street's track running the same way joins a one-way street's track (the one-lane track swings over to it).
- Drags of the game's track tool across a curve are red; street tracks meeting a curve with tracks are joined.

### Minimap

Curves and junctions are drawn as road cells (a cell counts where its centre lies on the road; footpaths at least one
cell wide). The cells are cached between the minimap's once-a-second redraws while the curves stay the same.

### Turning objects without steps

In the place tool (buildings, props, vehicles), **Shift + mouse wheel** turns the object 5° per notch, and 0.5° with
Ctrl held too. Touchpads turn smoothly. The right button still turns by 90° (45° for objects with a diagonal variant).
Under the pointer: the hint, and once turned, the angle. A new object starts unturned.
- Only objects that stand alone turn freely: no road, track or station connections, no generated parts.
- The turn is drawn: mesh, paths and effects turn about the footprint's centre. The cells (conflicts, levelling) stay
  those of the 90° orientation.
- A turned object puts no cell cover under itself. Its own base plate shows instead of a grid-aligned square.
- Data: `GridData::Item::yaw` holds radians on top of `orient`, within half a step. It is saved in map.grid section
  `0x07F10000` (u32 count, then the saved item index and the float). Undo/redo uses timeline action `0x070810B0`
  (ObjRef + float) after the item's create. Templates and mounting copy it. Code: `toolWheel` (engine/tools.cpp),
  `movePlaceObject` (tool_objects.cpp), `itemTurn` (grid.h).

### Stops

Bus and tram stops on small-road curves: the game's stop tool, the platform bent along the curve (tram stops need
tracks). A real stop item of the game anchors each stop (invisible), so lines, picking and saving work as usual.

## 2. Data model (for code that lays roads, e.g. the OSM import)

A curve is **not** a grid item. `GridData::curves` (engine/grid.h) holds `CurveRoad`s:

| Field | Meaning |
|---|---|
| `x0, y0, z0` / `x1, y1, z1` | start / end in world units (map centre at the origin, a cell is 3 units; y = the road's item height: the pavement level) |
| `dx0, dz0` / `dx1, dz1` | unit directions: at the start into the curve, at the end the direction of travel there |
| `a`, `b` | Bézier handle lengths (control points `p0 + d0·a`, `p1 − d1·b`); y eases from y0 to y1 |
| `kind` | 0 small, 1 large, 2 avenue, 3 alley |
| `ends` | bit 0: start on a grid road block (the curve continues its straight piece), bit 1: end on a block, bit 2: tram tracks, bit 3 / 4: start / end on a grass tram track (small roads), bit 5: one-way (start → end), bit 6: with bit 5 one lane, bit 7: part of a roundabout ring |
| `stops` | `{s, side ±1, kind 0 bus / 1 tram, design}` (small roads) |

Rules the renderer, lanes and walk map rely on:
- **Joins and junctions are found by position**: curve ends of road kinds within 0.2 units of each other form a joint;
  two ends with opposite directions are a smooth join, anything else (or three and more) a junction
  (`findCurveJunctions`). So: give shared points exactly the same coordinates.
- An end on a grid block (`ends` bit 0/1) must lie on the block's edge centre with the block's axis direction, and the
  block must be a straight piece through that edge (the tool replaces dead ends: `replaceRoadBlockEnd`).
- Minimum radius per kind (table above), minimum length 6 (3 for footpaths); at junctions at least 30° between arms and
  every arm longer than the junction's radius.
- A roundabout is four arcs with `ends = 0x20 | 0x80 (| 0x40)`; handles `0.5523·r`; see `roundaboutArcs` in
  engine/tool_curves.cpp.

API (engine/curve_roads.h): `applyCurveEdit(vm, grid, c, add)` adds or removes a curve inside an edit batch (terrain
levelled, cover, walk map, lanes and the renderer updated; call between `gridEditBegin`/`gridEditEnd`), and
`timelineRecordCurve(vm, timeline, c, add)` records it for undo. `splitCurve(c, s, first, second)` cuts a curve at arc
length s (the halves meet exactly). `curveProfile(kind)`, `curveLanes(kind)` give sizes and lane offsets.

Saving: map.grid section `0x07F00000` (format 2: per curve 13 floats with `ends` as the 13th, i32 kind, u32 stop count,
8 bytes per stop); timeline action `0x070810A0`. Game saves carry the section with the map.

### Mapping OpenStreetMap ways (suggestion)

| OSM | CurveRoad |
|---|---|
| `highway=residential/unclassified/service/tertiary` | kind 0 |
| `highway=secondary/primary` | kind 1 (or 2 with a median: `dual_carriageway`, divided ways merged) |
| `highway=footway/path/pedestrian` | kind 3 |
| `oneway=yes` (`-1`: reverse the way) | `ends |= 0x20`; `lanes=1` → `| 0x40` |
| `junction=roundabout` | its ways as one-way curves with `0x80` (or replace by a ring of `roundaboutArcs` around the centroid) |
| `railway=tram` along a road | `ends |= 4` on that road's curves |
| a node shared by 3+ ways | a junction: end the curves exactly at that node; merge arms closer than 30° |

A polyline becomes a chain of cubic Béziers, one per segment, with tangents from the neighbouring points
(Catmull-Rom: `d = normalize(p[i+1] − p[i−1])`, handles a = b = segment length / 3); drop points that would make a
bend tighter than the kind's minimum radius.

## 3. Testing and debugging

| Switch | Effect |
|---|---|
| `REGS_START=editor:<map>` / `map:<map>` / `save:<name>`, `REGS_START_RUN=1` | straight into the editor or a game (main.script) |
| `REGS_DEBUG_CURVES=1` | tool previews, snaps, junction fitting, "N junctions, M turn lanes, K joined both ways", tram ends |
| `REGS_TEST_CURVE_CARS=<n>`, `REGS_TEST_CURVE_TRAMS=<n>` | n cars / trams on random trips between curve lanes (engine/snowplows.cpp) |
| `REGS_TEST_CURVE_TURN=1` | test trips back the way they came (trams turning on a roundabout) |
| `REGS_DUMP_TEMPLATES=a,b` (`REGS_DUMP_VERTICES=1`) | a template's size, mesh bounds and UVs per material, paths |

## 4. Not done

Bridges, tunnels and express roads on curves; one-way large roads and avenues; large-road or avenue roundabouts; stops
on large-road and avenue curves; yielding to traffic on a roundabout beyond the simulation's crossing rules; road
texture variations on curves.

## 5. Files

engine/curve_roads.{h,cpp} (geometry, meshes, junctions, file section), engine/tool_curves.cpp (curve, roundabout,
wrappers of the tram track, stop and bulldozer tools), engine/sim_graph.cpp (`rebuildCurveNodes`: lanes),
engine/sim_citizens.cpp (walk map), engine/terrain.cpp (`collectCurves`), engine/tool_tracks.cpp (block ends),
data/regs/extend/metro/scripts/editor.script (menu), data/regs/ui/toolbar (icons; `tools/make-toolbar-icon.ps1`).
