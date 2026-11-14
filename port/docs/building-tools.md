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

Bus and tram stops on small-road curves, bus stops on large-road and avenue curves (2026-11-14): the game's stop tool,
the stop's model bent along the curve at the kerb (tram stops need tracks). A real stop item of the game anchors each
stop (invisible; `stop_bus0N_small` / `_large` / `_avenue` at the pavement's middle), so lines, picking and saving work
as usual; on large roads and avenues the kerb lane on the stop's side gets STOP where the stop is (bentNodes), and buses
halt there (tested with REGS_TEST_LINE=0 on both kinds).

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
minimap counts a cut cell as paved when its paint holds the cell's centre. **Shift + drag** with the pavement brush paints a straight strip as wide as the brush from the press point to the release
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
| `REGS_TEST_LINE=<type>` (`REGS_TEST_LINE_MODEL=<vehicle>`) | 60 ticks into a game: a line of that type (0 bus, 1 tram, 2 metro) through every stop of it on the map, with one vehicle, switched on (engine/sim.cpp) |
| `REGS_TEST_CURVE_TURN=1` | test trips back the way they came (trams turning on a roundabout) |
| `REGS_TEST_PAINT_AREA=1` or `=x,z` | at map load: a turned car park in asphalt, a round square and an L in paving stones around the map centre (or cell x, z), with cut edges (engine/grid.cpp) |
| `REGS_DUMP_TEMPLATES=a,b` (`REGS_DUMP_VERTICES=1`) | a template's size, mesh bounds and UVs per material, paths |

## 4. Not done

Tunnels on curves, express-road junctions and ramps, abutments at bridge ends; underground metro, switches and stations on track curves; one-way large roads and avenues; large-road or avenue roundabouts; tram stops
on large-road and avenue curves; yielding to traffic on a roundabout beyond the simulation's crossing rules; road
texture variations on curves.

## 5. Files

engine/curve_roads.{h,cpp} (geometry, meshes, junctions, file section), engine/tool_curves.cpp (curve, roundabout,
wrappers of the tram track, stop and bulldozer tools), engine/sim_graph.cpp (`rebuildCurveNodes`: lanes),
engine/sim_citizens.cpp (walk map), engine/terrain.cpp (`collectCurves`), engine/paint_edges.{h,cpp} (painted areas' edges), engine/tool_tracks.cpp (block ends),
data/regs/extend/metro/scripts/editor.script (menu), data/regs/ui/toolbar (icons; `tools/make-toolbar-icon.ps1`).
