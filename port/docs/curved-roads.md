# Curved roads (prototype, 2026-11-07)

Owner's request: real curved roads, for the map editor first (buildings are placed by hand there). The tool works like
the road tool of Cities: Skylines, which the owner named as the model.

## Using it

Map editor, traffic menu: the button **Kurve / Curve** in the alley (footpath), small-road, large-road and avenue tabs
draws that kind of road. A panel (top right) offers three modes:

| Mode | Clicks |
|---|---|
| Straight (Gerade) | start, end |
| Curved (Kurve) | start, bend point, end |
| Freeform (Freihand) | start, end: the road keeps its starting direction and bends to the end |

- After each road the tool draws on from its end, in that end's direction, until the right button stops it.
- Starts and ends snap to the closed side of a small road's dead end and to the free ends of other curves. A dead end it
  meets becomes a straight piece the curve continues. A road from or to a snapped end leaves or arrives along it; with
  both ends snapped the join is a smooth curve or S of its own.
- Ends also snap to the closed side of a tram track's dead end on the grass (axis-aligned blocks). The track's dead
  end becomes a straight piece, the curve gets tram tracks that run on into it, and the road keeps its
  turning circle over the track block (cars turn there, trams run straight through); the curve's other end needs street tracks (or another track
  end) for trams to pass. End flags bits 3/4 in the map file.
- A free end gets a turning circle (U-turn lane, round end).
- The preview is green when the road fits, red when it is shorter than 6 units, bends tighter than radius 6 or runs
  over buildings. Trees under it are cleared.
- Right button: stops drawing. A fourth mode, Remove (Entfernen), removes the curve under the cursor with a click (shown
  red; its blocks become dead
  ends again), as the bulldozer does in Cities: Skylines.
- Every change is recorded on the editor's timeline (undo, day switch).
- The panel shows the next click's meaning, the length (m) and the angle between the ends.

## How it works

| Part | Where |
|---|---|
| Data: `GridData::CurveRoad` (cubic Bézier: ends at block-edge centres / free points, unit end directions, handle lengths, end flags) | `engine/grid.h`, `engine/curve_roads.h` |
| Map file: port section `0x07F00000` in map.grid (so maps, autosaves, game saves and the editor's base.grid carry it) | `grid.cpp` parse, `grid_save.cpp` write |
| Timeline action `0x070810A0` (add / remove a curve), replayed by undo | `timeline.cpp` |
| Mesh: small_basic's own materials and texture regions (asphalt, markings decal, walkways, kerbs), turning circles | `curve_roads.cpp` buildCurveMesh, drawn by `terrain.cpp` collectCurves |
| Terrain: levelled to 0.25 under the road, rim eased over 3 units; cells wholly under it covered (cover 0xF00) like items | `curve_roads.cpp` levelTerrainUnderCurve / applyCurveCover |
| Lanes: one each way 1.5 right of the centre, ≤5-unit pieces, U-turns at free ends; linked with the blocks' lanes by the graph's own rules | `sim_graph.cpp` rebuildCurveNodes |
| Pedestrians: pavements walk code 2, carriageway blocked but a 3-unit crossing in the middle | `sim_citizens.cpp` stampWalkCurves |
| Simulation update after edits | `sim.cpp` simCurvesChanged (from gridEditEnd) |
| Tool | `engine/tool_curves.cpp`; block swap helpers in `tool_tracks.cpp` |
| Editor button, panel, texts | `data/regs/extend/metro/scripts/editor.script`, `data/regs/strings/*.strings` |

Debug: `REGS_DEBUG_CURVES=1` logs every preview (length, smallest radius, items in the way) and right click.

## Checked (editor, empty 512 map)

Curved mode with bend point; two straight roads joined by an S-curve (dead ends became straight pieces, 4 lane links each
way); freeform drawing on from the last end; refusing a too-tight bend (red); removal by right click (the neighbour's
end gets its turning circle back); undo (curve gone, dead ends back); the editor's autosave carries the curve section;
no terrain showing through at the joins.

## Not done / not checked

- Not checked in a game: traffic and pedestrians actually using curves, a sandbox started on a map with curves,
  saving and loading a game with curves.
- No bridges, tunnels, express roads or ramps on curves; footpath and road variations (textures) are not offered.
- No junctions on curves (a curve meets roads only at dead ends or other curves' ends); stops only on small-road
  curves; no parking on curves.
- The bulldozer does not remove curves (the curve tool's right button does).
- Walkway texture variations of the map are not applied (always the plain pavement).
- Game saves: a vehicle on a curve lane is not saved (it is dropped, as other unsaved movers).

## Game test (2026-11-07, sandbox via the editor's "Speichern & Spielen")

- The map with an S-curve saved and loaded into a game: curve drawn, its lanes linked to both roads (4 links each way).
- Bus stop: on the straight road green and placed; on the curve no preview at all (stops need road blocks).
- Tram stop: the same (green on the tracked straight road, nothing on the curve).
- Tram track tool: on the straight road fine; dragged across the curve it lays a straight diagonal free-standing track
  over grass and the curve, and accepts it (green). Curves have no tram tracks and do not block the track tool.

## Other road kinds (2026-11-07)

Large road, avenue and footpath curves (CurveRoad::kind 1, 2, 3; `curveProfile` in `curve_roads.cpp`) bend copies of
the type's own straight piece along the curve: a whole number of pieces (24 units for large roads and avenues, 3 for
footpaths), stretched to fit, the mesh cut into 1.5-unit slabs first (`sliceMeshAlongX`) so it follows the bend. Their
lanes are the straight piece's paths on every copy (building access paths and stop marks left out), so they meet the
straight blocks' lanes exactly; a free end gets the type's dead-end piece (mesh and U-turn paths). With tram tracks the
street tram piece (tram_large_basic / tram_avenue_basic) is bent the same way. The small road keeps its own mesh and
lanes. Sizes come from `REGS_DUMP_TEMPLATES=name,name,...` (logs a template's size, mesh bounds per material and paths).

Checked in the editor: a large road, an avenue and a footpath each with a freeform curve from its dead end (snapped,
the dead end became a straight piece; large and avenue 7 lane links in, 8 out), free ends with the dead-end piece (the
avenue's U-turn around the median). In a game: tram tracks laid on a large-road curve (rails on the outer lanes,
priced as street track).

## Junctions at any angle (2026-11-07)

Where three or more curve ends of one kind meet (or two at an angle) the arms stop short and a junction fills the
middle (`findCurveJunctions`, `buildJunctionMesh` in `curve_roads.cpp`):

- The arms are cut where neighbouring arms' edges part: hw / tan(angle / 2) + 1.5 from the point.
- Fill: the small road's asphalt, tiled with the texture plane fitted to small_basic's own asphalt (`fitPlanarUV`);
  footpaths with the footpath texture. Pavement corners rounded between the arms, kerbs, outer walls.
- Markings as on the game's junction pieces (small_junction4.object, road-marks.dds): zebra crossings across every arm
  at its mouth (bars 0.61 wide every 0.93, 2.71 deep, 0.17 inside the edge) and edge lines along the kerbs.
- Lanes: from every arm's incoming lanes into the same lanes of every other arm (Bézier, flagged as intersection;
  crossing links keep crossing turns apart). With tram tracks on two or more arms, tram lanes and rails between them.
- Walk map: crossable carriageway, walkable corners; cover and terrain under the outline.

Making one with the curve tool (free direction at the point, at least 30° from the roads there):
- start or end on the middle of a curve: it is cut in two there;
- start or end where curve ends already meet;
- start or end on the middle of a straight road block: it and as many straight neighbours along the road as the
  angle needs become two straight curves meeting at the point (refused when the road ends or turns too near).
The new road and the halves of a cut curve must reach past the junction's radius.

Test switches: `REGS_TEST_CURVE_CARS=<n>` / `REGS_TEST_CURVE_TRAMS=<n>` keep n cars / trams on random trips between
curve lanes (engine/snowplows.cpp); `REGS_DEBUG_CURVES` logs where they are, junction fitting, and "N junctions, M turn
lanes, K joined both ways" after each lane rebuild.

Checked: small-road junctions on a curve (Y) and on a straight road (T at about 60°), all turn lanes joined both ways;
cars (era4 SUVs) and trams (tram-01) driving through them in a game; zebra crossings, edge lines and rails in the
junction. Not checked: a large-road or avenue junction with long arms (no room on the test map; one with a too-short
arm showed the dead end's missing inner-lane U-turn), trams on a real tram line through a junction.

## Roundabouts (2026-11-07)

The "Kreisel / Round" button in the small-road tab has its own panel (1 lane / 2 lanes): click the centre, then click again
to set the radius (10–40 units). The ring is four quarter-circle small-road curves (Bézier handles 0.5523·r), one-way (end flag
bit 5) counter-clockwise as seen from above (right-hand traffic); one-lane rings (bit 6) have a single lane on the
centre line and no lane divider, two-lane rings both lanes running forward. Roads join the ring like any curve: ending
on it cuts it there and makes a junction. Junction lanes keep to one-way arms (an incoming ring arm only feeds, an
outgoing one is only fed; lanes pair from the right, so entering cars join the outer lane) and there is no zebra over
the ring.

Checked: one- and two-lane roundabouts with three roads each, all junction lanes joined (5 per entry with two lanes, 3
with one), cars circulating counter-clockwise through them (REGS_TEST_CURVE_CARS). Not done: roundabouts of large roads
or avenues (the modes always build small-road rings), yielding to traffic on the ring beyond the simulation's own
crossing rules.

### Trams on roundabouts

The tram track tool on any piece of a roundabout lays tracks on the whole ring (priced for its length, the preview
shows all of it). On a one-way curve the track runs with the traffic only: in the outer lane of a two-lane ring, on the
centre line of a one-lane ring. Junction tram lanes keep to one-way arms (`junctionTramEnd`), so a tram enters from a
tracked road, goes round, and leaves on any tracked road, the one it came from too: trams turn around on the ring.

Checked: a two-lane ring with tracks and two tracked roads, all tram turn lanes joined; trams circulating
counter-clockwise; with `REGS_TEST_CURVE_TURN=1` (test trams sent back the way they came) every trip went in, round
past the other junctions and out on the same road. Not checked: a real tram line using the ring.

### Sizes and mixed roads (2026-11-07)

- Roundabouts from radius 10: small ones take roads at the four compass points (where the ring's arcs meet). The tool
  refuses an entry when a road at the point would be used up by the junctions at both its ends.
- Junctions may mix road kinds (small, large, avenue; footpaths stay among themselves): every arm keeps its own width,
  lanes, kerbs and zebra, and lanes pair from the right (a large road's two lanes onto a two-lane ring's two). The curve
  tool snaps across road kinds at joints and in the middle of curves.
- Two arms whose edges meet only beyond their mouths (a ring on its island side) get a straight side, not a rounded
  corner.

Checked: two-lane and one-lane rings of radius 10 with four small roads (20 / 12 turn lanes, all joined); a two-lane
ring of radius 20 with a large road and three small roads (6 turn lanes at the large road's junction, all joined).

## Toolbar icons (2026-11-08)

The Curve buttons (alley, small road, large road, avenue) and the Roundabout button have icons in the game's style:
isometric tiles generated with Magnific (Freepik AI) from four of the game's road icons as a style reference, then
fitted by `tools/make-toolbar-icon.ps1` into the game's own diamond (44x36 at 10,16 in 64x64; hover 49x41) inside the
border of a game icon of the same kind, with the selected state at 55 % brightness as the game's are. Files:
`data/regs/ui/toolbar/{curve-small,curve-large,curve-avenue,curve-alley,roundabout}{,_se,_ho}.dds` (uncompressed
32-bit DDS). The source pictures are in the owner's Magnific library.

## One-way streets (2026-11-08)

Curve panel, second row (small roads): "Beide Richtungen / Einbahn 1 Spur / Einbahn 2 Spuren". A one-way curve runs
the way it is drawn (end flag bit 5, one lane bit 6; roundabout rings carry bit 7 besides, so only they lose the zebra
crossings and get tracks all round with one click). Lanes: two lanes forward, or one on the centre line; where a one-way
end meets a two-way road (a road block, a two-way curve, a free end; not a junction or another one-way curve) its lanes
merge into / split from the right-hand lane on the piece next to it. Markings: the game's straight arrow
(road-marks.dds) on every lane every 25 units. Tram tracks: a single track with the traffic (right-hand lane, or the
centre line with one lane), priced at half the street track's price.

Checked: a two-lane one-way street in three pieces from a grid road's east end round to its west end (4 lane links in at
the start, 4 out at the end, none the wrong way); cars routed round the loop instead of against it; tracks on the
one-way pieces and test trams running only the one-way direction. Then a one-lane one-way loop with a grid tram track on the two-way road: the street's track running toward the one-way
street joins its track at one end and the one-way track joins the street track running away at the other (the
one-lane track swings from the centre line to the right-hand track on the piece next to each end, as its tram lane
does); tram links at both ends; test trams running the whole loop one way round (down the one-way street, along the
street's eastbound track, up the other side), cars likewise. The tram track tool now takes a point beyond a curve's
end on a road block as that block (a drag can start there). Not checked: the halved price on screen.

## Editor UI (2026-11-08)

- Traffic menu, small-road tab: Curve, One-way 1 lane, One-way 2 lanes, Roundabout (icons; the one-way buttons open the
  curve tool set to that). Alley, large-road and avenue tabs: Curve.
- Curve panel: icon rows "Form" (straight = the game's straight road icon, curve, freeform, remove = the game's
  bulldozer) and "Richtung (kleine Straßen)" (both ways, one-way 1 lane, one-way 2 lanes), tooltips on every icon.
- Roundabout panel: icons for 1 and 2 lanes.
- New icons (Magnific, fitted by tools/make-toolbar-icon.ps1; the one-way and two-way tiles zoomed to their middle so
  the arrows stay readable at 46 px): oneway-1, oneway-2, twoway, curve-free, roundabout-1.
