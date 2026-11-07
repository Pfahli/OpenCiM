# Curved roads (prototype, 2026-11-07)

Owner's request: real curved roads, for the map editor first (buildings are placed by hand there). The tool works like
the road tool of Cities: Skylines, which the owner named as the model.

## Using it

Map editor, traffic menu, small-road tab: the button **Kurve / Curve**. A panel (top right) offers three modes:

| Mode | Clicks |
|---|---|
| Straight (Gerade) | start, end |
| Curved (Kurve) | start, bend point, end |
| Freeform (Freihand) | start, end: the road keeps its starting direction and bends to the end |

- After each road the tool draws on from its end, in that end's direction, until the right button stops it.
- Starts and ends snap to the closed side of a small road's dead end and to the free ends of other curves. A dead end it
  meets becomes a straight piece the curve continues. A road from or to a snapped end leaves or arrives along it; with
  both ends snapped the join is a smooth curve or S of its own.
- A free end gets a turning circle (U-turn lane, round end).
- The preview is green when the road fits, red when it is shorter than 6 units, bends tighter than radius 6 or runs
  over buildings. Trees under it are cleared.
- Right button: cancels the road being drawn; with none, removes the curve under the cursor (its blocks become dead
  ends again).
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
- Only the small road; no large roads, avenues, tram tracks, bridges or tunnels on curves.
- No junctions on curves (a curve meets roads only at dead ends or other curves' ends); no stops or parking on curves.
- The bulldozer does not remove curves (the curve tool's right button does).
- Walkway texture variations of the map are not applied (always the plain pavement).
- Game saves: a vehicle on a curve lane is not saved (it is dropped, as other unsaved movers).
