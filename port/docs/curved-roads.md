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
