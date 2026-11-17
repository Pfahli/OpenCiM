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
| metro, ground | 6 (track bed) | – | 20 | bent `metro_ground_basic`; metro tab of the build menu |
| metro, elevated | 6 | – | 20 | bent `metro_elevated_basic`, pillars under every joint; ground left as it is |
| railway (double track) | 6 | – | 20 | bent `track_double_basic`; track tab of the traffic menu |
| express road | 30 | 3 (±5, ±9, ±13) | 30 | bent `road_express_basic`, median, no pavement; express tab; joins only express roads, smoothly (no junctions), no pedestrians; bridges with `bridge_rexpress_gap` |
| ramp | 6 | 1 on the centre line, used both ways (as the game's) | 12 | bent `road_ramp_basic`; express tab; joins roads at junctions and express roads at their entries and exits (below); elevated: `ramp_elevated_basic`; no pedestrians |

### Bridges (curves)

A curve can be a bridge (`CurveRoad::bridge = 1`). It is meant for the OSM import's `bridge=yes` ways and legs over water.
- The deck runs at the curve's own heights: y0 and y1, eased along it. The ground or water under it stays as it is (no
  levelling, no cell cover).
- It is drawn with the kind's bridge span bent along it, railings included: `bridge_rsmall_gap`, `bridge_rlarge_gap`,
  `bridge_ravenue_gap`, and `bridge_tdouble_gap` for the railway. The span's pillar stands every 24 units, its foot
  24 units under the deck. Footpaths and metro tracks keep their plain piece and get no pillars.
- A bridge shorter than 3/4 of a span piece after the junctions' trims draws as the kind's plain road at deck height
  (a span squeezed that much shows its girders). `REGS_DEBUG_CURVES=1` logs every bridge's length and trims.
- Lanes and the walk map are as on any curve of the kind, at the deck's height.
- Its ends join other curves and junctions by the same point rule.
- Editor: the small-road, large-road and avenue tabs end with a bridge button (the game's bridge icon of the tab): the
  curve tool in curve mode (start, bend point, end) laying bridges, two cells over the ground, passing over anything a
  cell lower than the deck. The other shape buttons switch bridges off. Script: `regsCurveToolBridge(on)`; test switch
  `REGS_TEST_CURVE_BRIDGE=1`.
- Checked (2026-11-13): small-road, large-road and avenue bridges (spans, parapets, pillar posts) over the S-curve; the
  OSM import's Saalbach bridge in Bruchsal.
- Abutments (2026-11-15): a bridge's ends that do not continue on another bridge curve (on land: a road, a junction, a
  free end) take the kind's end span instead of the gap span (`bridge_rsmall_end`, `_rlarge_end`, `_ravenue_end`,
  `_tdouble_end`, `_rexpress_end`): its abutment wall on the land side (the template's local −x), as the game's
  bridge tool puts it (`curveBridgeEnd`). Checked on a large-road bridge.

### Tunnels (curves)

A curve can be a tunnel (`CurveRoad::bridge = 2`, 2026-11-15): the kind's tunnel tube bent along it (`tunnel_rsmall_gap`,
`_rlarge_gap`, `_ravenue_gap`, `_tdouble_gap`, `_rexpress_gap`) and its portal (`tunnel_*_end`: the mouth with its
frame 6 high on the template's −x side, wing walls falling to the open road) on the first and last piece where the
tunnel does not continue on another tunnel curve.
- The road runs at the curve's own heights (y0 to y1), like a bridge's deck. Only the portals cut the ground: under
  their pieces it is levelled to the road and covered; a cell behind each mouth (and beside the road there) it comes
  down to the portal's top, so the frame stands clear in front of the hill. Over the tube the ground stays as it is: no
  cover, no walk map, items over it are not in the way (`curveTunnelOpen`).
- Editor: the small-road, large-road, avenue and express tabs get a tunnel button (the tab's tunnel icon): the curve
  tool in curve mode laying tunnels (`regsCurveToolBridge(2)`). The ground over the tube must stand at least 6 over
  the road (the portal's height), or the preview is red. Test switches `REGS_TEST_CURVE_BRIDGE=2`, `REGS_TEST_HILL`.
- Lanes are the kind's as on any curve.
- Checked (2026-11-15): a large-road tunnel through a test hill (both portals, the frames clear, the hill whole over the
  tube).

### Map edge

A curve end on the map's border leaving it straight out (within 0.5 units of the edge, within 18° of the outward
direction; `curveEndOffMap`) leads off the map, as the game's map-edge pieces (`road_*_porta/b`, `road_small_port`,
`track_double_port`, which are plain road with EXTERNAL lanes) do: no dead end there, and the lanes starting on the
border are the traffic's way in (START), those ending there its way out (END), both EXTERNAL. Roads, express roads and
the railway (`curveKindLeavesMap`). The curve tool snaps an end within the road's half width + 6 of the border onto it.
Checked (2026-11-15): a large road to the east edge; in a game the traffic sources take its two lanes in and out, and a
route runs in from the edge, round the far dead end and out (`REGS_DEBUG_SIM=1` logs "traffic sources").

### Express ramps

Ramps (curve kind 8) join an express curve where the game's entry and exit pieces would be (`RampPort`, 2026-11-15).
Every express piece has four ports: on each side an entry (half a block before the piece's centre, in the traffic's
direction on that side) and an exit (half a block after it), 18 out from the centre line, the ramp meeting the road at
45°. A ramp end on a port turns that express piece into `road_express_enter` or `road_express_exit`, bent along the
curve (`rampPortBend`: the entry piece's road lies 3 to −z of its origin, the exit's 3 to +z; on the left side, and for
exits on the right, turned round); its slip lane joins or leaves the outer lane of that side, and the ramp's lane
links into it. One ramp per express piece.
- Editor: the express tab ends with a ramp and an elevated ramp button (the game's icons), the curve tool in curve mode.
  A ramp end snaps to the nearest free port the ramp reaches going on (an entry from behind, an exit from ahead).
- Ramps join roads (small, large, avenue) at junction points like a road (no zebra crossing over a ramp arm).
- Checked (2026-11-15): an entry and an exit on a straight express road (lanes linked, "curved roads: ramp entry/exit"
  with `REGS_DEBUG_CURVES=1`); test cars (`REGS_TEST_CURVE_CARS`) drove down the ramp and merged into the outer lane.
- The elevated ramp stands on `ramp_elevated_pillar` columns every 24 units (checked from a low camera, 2026-11-15).
- Express road to large road or avenue (2026-11-15): an express curve's end met smoothly by a large-road or avenue
  curve's end takes the game's transition piece (`road_express_large_basic` / `road_express_avenue_basic`, the express
  side inward) in place of its last piece, lanes included (`curveExpressTransition`); the curve tool lets the two
  kinds' free ends snap to each other. Checked: an avenue drawn on into an express road; test cars drove through.
- Not checked: ramps on curved express pieces and on express bridges.

### Tracks

The build menu's metro tab ends with straight, curve and freeform for ground and elevated metro tracks, and the traffic
menu's track tab with the same three for the railway.
- Tracks join only tracks of their own kind: a grid track's dead end (it becomes a straight piece) or a track curve's
  free end, always smoothly.
- There are no junctions or switches on curves, and no tram tracks or stops on them.
- A free end gets the track's dead-end piece, where trains turn back.
- Elevated tracks start two cells over the ground and pass over anything a cell lower than they are.
- The lanes are the pieces' own paths, bent; metro trains run on them. Test them with `REGS_TEST_CURVE_METRO=<n>`.
- Checked (2026-11-12): a curve from a grid metro track's dead end joins it seamlessly, its lanes linked both ways
  (the "tracks: n in, n out" count in the "curved roads:" log line); railway curves; metro cars on ground and elevated
  curves. Like all curves, track curves are hidden in the underground view.
- Checked (2026-11-13): a metro line between two ground stations joined by a 225 m U-shaped track curve (grid stubs
  off the stations' ends, the curve between their dead ends): the line's route runs over the curve and its train
  shuttles between the platforms (`REGS_TEST_LINE=2`).

### Trams on railway tracks

Trams run on the railway's tracks too (owner's request 2026-11-15, like Karlsruhe's tram-trains), on the grid's double
track and on railway curves alike (engine/sim_graph.cpp `addRailTwin`, `linkTramsToRail`):
- Every railway lane gets tram lanes on the same curve, both ways (as tram tracks have both ways on each track; the way
  against the railway's own direction at an eighth of the speed, so routes keep right and use it only to turn at a
  stop). Not START / END / PARK: trams do not enter the map there; a station's platform tracks keep STOP. They come
  after the item's own lanes, so saved lane numbers stay. Nothing is drawn.
- At a railway track's dead end a tram turning loop joins the end of one track to the other track, so a line turns at
  the end of the railway as on a loop.
- Where a tram track ends at a railway track (a street or grass tram track's dead end within 14 units, heading the same
  way) a tram connector joins them both ways; ends that meet exactly join by themselves.
- The tram lane and the railway lane under it cross each other, so a tram and a train never hold the same piece: each
  waits for the other (`REGS_DEBUG_SIM=1` counts the waits in the traffic summary).
- Checked (2026-11-15): two small roads with tram tracks joined by a railway curve, a tram line from a stop on one road
  over the railway to a stop on the other (`REGS_TEST_LINE=1`); with a test train on the same railway
  (`REGS_TEST_CURVE_TRAINS=1`) both kept running, waited for each other and never got stuck. Vienna's save loads as
  before (all vehicles placed).
- Tram stops on railway (2026-11-15): the tram stop tool takes straight grid railway blocks (a small-road tram stop of
  the chosen design beside the track bed; engine/tool_objects.cpp `stopPreview`, conflicts in grid_edit.cpp) and
  railway curves (curve stops, `curveTakesStop`). The stop's platform is the railway's tram lane on its side, running
  with the platform on its right; the other way on the same track is its second platform, where trams turn round
  (sim.cpp `stopPlatforms`; legs take either, `segmentBetween`).
- Train stations are tram stops (2026-11-15): every `station-track` and `terminal-track` item registers as a tram
  stop (its bubble, catchment, line tool), its platform the tram lane of its STOP track (`simIsTrainStation`).
  Vienna and Berlin have 23 each.
- Checked (2026-11-15): two tram stops on a grid railway (one each side) and two on a railway curve, a tram line
  between them (turning at the railway's end); in Vienna a loop between a terminal and a through station, about 13
  game minutes, passengers boarding, no jams, the city's own lines as before. `REGS_TEST_LINE_ITEMS=a,b` builds a test
  loop over those items, `REGS_DEBUG_LEGS=1` logs each stop's platforms and legs.
- Background passenger trains do not stop for tram passengers; trams and trains still wait for each other.

### One-way streets

Small roads, 1 lane (on the centre line, no divider) or 2 lanes, arrows on every lane every 25 units. Where a one-way
end meets a two-way road its lanes merge into / split from the right-hand lane. Cars and trams never run against it.

One-way large roads and avenues (2026-11-15): the one-way tab's third and fourth lane buttons (the large road's and the
avenue's curve icons) draw them; all four lanes run in the drawing direction (the piece's paths against the curve turned
round, sim_graph.cpp `bentNodes`). Where an end meets a two-way road the left-hand lanes split from / merge into the
right-hand ones on the piece next to it; at junctions all their lanes go one way. The road looks as the two-way one
(no arrows yet). Checked: a one-way large road merging into a two-way one, test cars only in its direction.

### Roundabouts

A ring of four one-way quarter-circle small-road curves, counter-clockwise seen from above (right-hand traffic), 1 or
2 lanes, radius 10–40. Small rings take roads at the compass points (where the arcs meet), larger ones anywhere.

Large-road and avenue roundabouts (2026-11-15): the roundabout tab's third and fourth buttons (the large road's and the
avenue's curve icons) lay the same ring of that kind, all lanes one way (one-way large roads and avenues). A large-road
ring has a radius of 18–60 (half width plus a 6-unit island).
An avenue ring has no inner carriageway (owner's request 2026-11-15): it is the avenue's outer half only (`curveHalfRing`:
half the middle strip, the two outer lanes, the pavement), its centre line the island's edge, radius (the island's)
8–60. Its footprint (ground, cover, walk map, minimap, picking, `curveAcross`), lanes and junction arms (outer lanes
only; the outline follows the island's edge between the ring's two arms) leave the island alone, and a road ending at a
junction no longer claims ground past its end (`pastJoinedEnd`, which kept the island's edge flat and bare).
Checked: a large-road ring with two large roads joined (20 turn lanes, all linked) and an avenue ring with two avenues
joined (12 turn lanes), test cars round both without jams.

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
- **Magnetic placement:** near a turned (or shifted) object, the next freely turnable object takes that object's turn,
  plus whichever 90° step is nearest the tool's own turn. It stands on the neighbour's grid, turned with it: edges
  flush with the neighbour's, whole cells along its axes. The pointer line says "am Nachbarn ausgerichtet". Hold Ctrl
  to place freely. (`snapToNeighbour`, tool_objects.cpp: the nearest such object whose edge is within half the new
  object's size plus a cell of the pointer.)
- Such an object usually sits off the cell grid: `GridData::Item::offX` / `offZ` shift it in world units, at most half
  a cell, off its footprint's centre.
- Conflicts between items off the grid (`itemOffGrid`) compare their drawn ground rectangles (separating axes, a
  0.1-unit sliver allowed), not their cells (`gridItemsConflict`, grid_edit.cpp). Snapped neighbours therefore fit
  side by side. The items checked are still the ones whose cells lie near.
- Data: `GridData::Item::yaw` holds radians on top of `orient`, within half a step.
- map.grid section `0x07F10000`: u32 count (bit 31: with shifts), then per item the saved item index, the angle and
  (bit 31) offX, offZ as floats.
- Undo/redo: timeline action `0x070810B0` (ObjRef + yaw, offX, offZ) after the item's create. Templates and mounting
  copy all three.
- Code: `toolWheel` (engine/tools.cpp), `movePlaceObject` / `snapToNeighbour` (tool_objects.cpp), `itemTurn` and
  `itemOffGrid` (grid.h).

### Stops

Bus and tram stops on small-road, large-road and avenue curves (2026-11-14): the game's stop tool,
the stop's model bent along the curve at the kerb (tram stops need tracks). A real stop item of the game anchors each
stop (invisible; `stop_bus0N_small` / `_large` / `_avenue` at the pavement's middle), so lines, picking and saving work
as usual; on large roads and avenues the kerb lane on the stop's side gets STOP where the stop is (bentNodes), and buses
halt there (tested with REGS_TEST_LINE=0 on both kinds). Tram stops on large roads stand at the kerb beside the kerb
lane's track (`stop_tram0N_large`); on avenues in the middle strip beside its track, with the game's zebra crossing
over the carriageway (`stop_tram0N_avenue`, `curveStopOffset` 4.5); the tram lane on the stop's side gets STOP (tested
with REGS_TEST_LINE=1 on both kinds). Checked on bent curves too (2026-11-14): tram stops 02 and 03 on a large road, bus stops 02 and 03 on both sides
of an avenue, tram stops on both sides of an avenue's middle strip; the test lines halt at every stop. A tram turns
round only at a stop on the same track (the large road's kerb track; reverseConsist), so tram stops on both sides of
an avenue need a loop or a junction to reach each other, as in the game, whose avenue dead end has no tram turn either.
A large road or avenue curve with tracks ends at a free end in the game's tram end piece (`tram_large_deadend`,
`tram_avenue_deadend`, no lanes), its open edge on the road dead end's (`curveTramDeadend`).

### Street names

The game's rename tool (T in the editor's toolbar) names curves too (2026-11-14): pointing at a curve marks it and the
curves of its kind joined on at its ends (through no junction), as the game marks a road segment; with the right button
held the curve alone. A click opens the game's rename dialog; the name goes on every marked curve (undo takes it back).
`findNames` offers a curve's name from its nearest point, scored as a road piece, so stops placed in a game near a
named curve take its name. Engine/tool_curves.cpp (`renameMove`, a stand-in `gsGridItemController` with `<curveName>`
for the dialog), timeline action `0x070810D0`. Checked (2026-11-14): naming a large-road curve, the hover text, the
name after saving and loading, undo, and a bus stop placed in a game beside it offered the name.

## 2. Data model (for code that lays roads, e.g. the OSM import)

A curve is **not** a grid item. `GridData::curves` (engine/grid.h) holds `CurveRoad`s:

| Field | Meaning |
|---|---|
| `x0, y0, z0` / `x1, y1, z1` | start / end in world units (map centre at the origin, a cell is 3 units; y = the road's item height: the pavement level) |
| `dx0, dz0` / `dx1, dz1` | unit directions: at the start into the curve, at the end the direction of travel there |
| `a`, `b` | Bézier handle lengths (control points `p0 + d0·a`, `p1 − d1·b`); y eases from y0 to y1 |
| `kind` | 0 small, 1 large, 2 avenue, 3 alley, 4 metro (ground), 5 metro (elevated), 6 railway, 7 express road |
| `ends` | bit 0: start on a grid road block (the curve continues its straight piece), bit 1: end on a block, bit 2: tram tracks, bit 3 / 4: start / end on a grass tram track (small roads), bit 5: one-way (start → end), bit 6: with bit 5 one lane, bit 7: part of a roundabout ring |
| `stops` | `{s, side ±1, kind 0 bus / 1 tram, design}` (small roads) |
| `bridge` | 1: a bridge (see above); saved in the second byte of the kind (section and timeline bytes) |
| `name` | the street name (UTF-8, empty when unnamed); `findNames` offers it for stop names |

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

Saving: map.grid section `0x07F00000` (format 3: per curve 13 floats with `ends` as the 13th, i32 kind, u32 stop count,
8 bytes per stop, u16 name length and the name; format 2 had no name); timeline action `0x070810A0` (the same bytes
after an add/remove byte), renames `0x070810D0`. Game saves carry the section with the map.

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

The OSM import does this (engine/osm.cpp, namespace `streets`, since 2026-11-08); it adds all its curves at once
(`addCurvesBulk`: one redraw, the junctions levelled once) and records each for undo. `findCurveJunctions` takes an
optional list of points (only the junctions there) and groups the ends in an x-sorted window, so maps with
thousands of curves stay quick to edit and undo.

A polyline becomes a chain of cubic Béziers, one per segment, with tangents from the neighbouring points
(Catmull-Rom: `d = normalize(p[i+1] − p[i−1])`, handles a = b = segment length / 3); drop points that would make a
bend tighter than the kind's minimum radius.

### Painted areas with exact edges (for the OSM import's car parks, squares, bands)

The pavement paint is per cell, so an area with a slanted or round outline used to end in a saw tooth of whole cells.
The cells its outline crosses can now carry an **edge** (`GridData::paintEdges`, engine/grid.h): one or two straight
lines in the cell's own coordinates; the part inside takes the edge's paint, the rest of the cell is drawn as its
flags say (grass, or another paint). The terrain mesh cuts the cell's four triangles along the lines
(`Builder::edgeCell`, engine/terrain.cpp), so the border follows the outline at any angle; shader hooks see it too.

API (engine/paint_edges.h):
- `paintArea(g, rings, type, x0, z0, x1, z1)`: paints an outline (rings in corner coordinates: cell (x, z) spans x..x+1;
  several rings even-odd, for holes) in pavement paint `type` (0..7 as `flags & 7`; the game's asphalt 0, paving stones
  2): whole cells get the paint, crossed cells an edge (the best one or two lines, fitted by sampling the outline in the
  cell; a second line covers corners, also inner ones). Water cells are skipped. Returns the cells and their rect.
- `paintAreaCells(W, L, rings)`: the same without touching a grid (the caller decides per cell).
- `timelineRecordPaintEdges(vm, timeline, g, x, z, w, l)` (timeline.h): records the rect's edges for undo; record the
  cells' flags as before (e.g. the import's terrain patch), then the edges. Call `gridEditCellsChanged` for live edits.

The terrain menu's round pavement brush and its eraser (right button) cut the cells on the brush circle the same way
(`paintCircle`): strokes get round, smooth borders, a stroke over another paint's edge joins it (where three surfaces
meet in one cell a small notch stays: a cell holds one cut). Templates (.cimtpl) keep the edges of their cells; the
minimap counts a cut cell as paved when its paint holds the cell's centre. 

**Brush shapes** (2026-11-14): a second column beside the brush sizes picks the shape, with drawn symbols
(`data/regs/ui/toolbar/brush-*.dds`, `tools/make-brush-icons.ps1`; script: the toolbar extension, native
`regsBrushShape([n])`): circle, square (along the grid), diamond (turned 45°) and line (a strip as wide as the brush
from the press to the release; pavement and fields). Every brush (pavement, fields, level, soften, shift, ruin) takes the
shape: its distance gives the height brushes' falloff, the field brush paints the cells whose centre is inside, the
pavement brush cuts along the rim. The green preview is the shape itself (rim cells clipped to it: `highlightCircle` /
`highlightPoly`). A pavement stroke paints the shape's sweep between two mouse positions (the convex hull of the shape at
both: straight sides at any angle) and records it as point pairs (strength 2), so undo replays the same; each stroke is
one undo step (`timelineCloseBrush`). The shape goes in the brush action's radius byte (top two bits).

**Shift + drag** with the pavement brush paints a straight strip as wide as the brush from the press point to the release
point, at any angle with exact edges (`paintArea`; the preview is clipped to the strip; one undo step). The field brush still paints whole cells: cut
field cells were tried (2026-11-14) but the field texture's soil rim, blended from the cell corners, came out ragged
along the cut.

Saving: map.grid section `0x07F20000` (u32 count; per cell u16 x, u16 z, u8 type, u8 lines | join << 4, u16 0, six
floats), timeline action `0x070810C0` (the rect and its edges). Painting a cell by hand (terrain menu, either brush)
drops its edge (the old square brush) or cuts it (the round brush); map resizes move the edges. The simulation still
reads the cell flags only (a cut cell counts as its flags say).

## 3. Testing and debugging

| Switch | Effect |
|---|---|
| `REGS_START=editor:<map>` / `map:<map>` / `save:<name>`, `REGS_START_RUN=1` | straight into the editor or a game (main.script) |
| `REGS_DEBUG_CURVES=1` | tool previews, snaps, junction fitting, "N junctions, M turn lanes, K joined both ways", tram ends |
| `REGS_TEST_CURVE_CARS=<n>`, `REGS_TEST_CURVE_TRAMS=<n>`, `REGS_TEST_CURVE_METRO=<n>` | n cars / trams / metro cars on random trips between curve lanes (engine/snowplows.cpp) |
| `REGS_TEST_CURVE_TRAINS=<n>` | n locomotives end to end over the railway curves (trams on railway tracks) |
| `REGS_TEST_LINE=<type>` (`REGS_TEST_LINE_MODEL=<vehicle>`) | 60 ticks into a game: a line of that type (0 bus, 1 tram, 2 metro) through every stop of it on the map, with one vehicle, switched on (engine/sim.cpp) |
| `REGS_TEST_CURVE_TURN=1` | test trips back the way they came (trams turning on a roundabout) |
| `REGS_TEST_HILL=x,z,r,h` | at map load: a smooth hill of radius r and height h around world point x, z (unlocked corners; tunnel tests, engine/grid.cpp) |
| (note) | the editor's undo right after loading a map has nothing to undo: saving on the first day folds that day's actions into the map file (`removeDay`, then the base grid), as in the original |
| `--at N:tilt:<t>`, `N:zoom:<z>`, `N:dir:<n>` | test runs: the camera's tilt (screenshot mode; 0 looks level, about 0.2 low over the ground), zoom and direction (app/main.cpp). Zoom and direction reset the tilt to the zoom's own: give tilt last |
| `REGS_TEST_PAINT_AREA=1` or `=x,z` | at map load: a turned car park in asphalt, a round square and an L in paving stones around the map centre (or cell x, z), with cut edges (engine/grid.cpp) |
| `REGS_DUMP_TEMPLATES=a,b` (`REGS_DUMP_VERTICES=1`) | a template's size, mesh bounds and UVs per material, paths |

## 4. Not done

Express roads splitting or joining each other (interchanges); underground metro, switches and stations on track curves; arrows on one-way large roads and avenues; yielding to traffic on a roundabout beyond the simulation's crossing rules; road
texture variations on curves.

## 5. Files

engine/curve_roads.{h,cpp} (geometry, meshes, junctions, file section), engine/tool_curves.cpp (curve, roundabout,
wrappers of the tram track, stop and bulldozer tools), engine/sim_graph.cpp (`rebuildCurveNodes`: lanes),
engine/sim_citizens.cpp (walk map), engine/terrain.cpp (`collectCurves`), engine/paint_edges.{h,cpp} (painted areas' edges), engine/tool_tracks.cpp (block ends),
data/regs/extend/metro/scripts/editor.script (menu), data/regs/ui/toolbar (icons; `tools/make-toolbar-icon.ps1`).
