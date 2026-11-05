# The underground view (`GRID_VIEW_UNDERGROUND`, `gsGrid.viewMode = 0x180`)

How the game shows subway tracks and underground stations: the scripts switch `gsGrid.viewMode`, and the grid
rebuilds its divisions so that the map is cut open around the tunnels. Addresses: `L:` Linux build
(`decompiled/Linux/functions/0x….c`), `W:` GSystem.dll, `S:` game scripts under `extracted/metro/`. Confidence as in the
other specs: obs = read in the code, inf = inferred.

Constants (`docs/native-api.md`): `GRID_VIEW_DEFAULT` = 0xfe, `GRID_VIEW_UNDERGROUND` = 0x180. The data views
(`GRID_VIEW_HOMES` … `GRID_VIEW_POPULARITY`, 0x10000-0x80000, with the citizen groups 0x1000-0x7000) are or-ed onto
0xfe. Bits of the low byte that the renderer and the simulation read:

| Bit | Meaning | 0xfe | 0x180 | 0xc6 (§3) |
|---|---|---|---|---|
| 0x02 | terrain surface | yes | no (earth plane) | yes |
| 0x04 | water | yes | no | yes |
| 0x08 | trees | yes | no | no |
| 0x10 | items rooted on roads … heliport (types 2-0x12) and ALIGN_4 items | yes | no | no |
| 0x20 | all other items; yards and fields in the terrain; cover quadrants (`fixGrid`) | yes | no | no |
| 0x100 | items rooted on a TUNNEL item | no | yes | no |

## 1. Switching (scripts)

* **The layer button** of the minimap (`S:ui/minimap/minimap.script:46-75`, `toggleUndergroundView`): from any view to
  0x180 (button selected), back to the forced data mode if one is set, else to the data mode (`$dataMode`, 0xfe
  unless the data panel picked one). Afterwards `$gameUI.setCameraPos(cameraX, cameraZ)` so the camera height
  follows. A click clears `$undergroundForced`.
* **The key** `undergroundView` ("U" in `S:defaults/controls.keys:51`, `undergroundView2` unset) calls the same
  function (`S:scripts/controls.script:147-152`); the controls menu lists it with the button's tooltip.
* **Automatically with the subway tool** (`S:ui/minimap/minimap.script:297-307`): on the player's `onModeChange`,
  one frame later, `editMode == GRIDMODE_LAY_TRACK && toolMode == GRIDMETRO_SUBWAY` turns the view on (if it is not on)
  and sets `$undergroundForced`; any later mode change while it is forced turns it off again. The build menu's
  "uground" entry is `selectTool(GRIDMODE_LAY_TRACK, GRIDMETRO_SUBWAY, 0, false, true)` (`S:scripts/game.script:586`,
  levelable: the level bar sets `toolSize` −2/−3/−4, default `$lastLevel = −3`, `S:ui/toolbar/toolbar.script:19,
  150-160, 195-199`). The underground **station** (`selectStation("station_metrou0", false, true)`, `:590`) and the
  transition piece do **not** switch the view; selecting them after the subway tool switches a forced view back off.
* **The station entrance** `stairs_metrou0b` has `$doubleClickAction = toggleUndergroundView`
  (`S:objects/stations/metrou_stairs0b.script`).
* **Data views** (`setDataMode`, `forceMode`, the minimap thread) never overwrite 0x180; choosing a data mode while
  underground toggles the view off first (`:216-245`).
* **Camera** (`S:scripts/ui.script:186-200`, `setCameraPos`): the target height `y` from `getAreaY`, the shadow clip
  plane `shadows.yPlane = −y − 10` (still from the surface height), then underground `y −= 14`. The clip plane thus
  sits at world `y_target + yPlane = −24` = the earth plane (§3), the normal view at −10.
* **Ambience** (`S:scripts/ambience.script:143-147`): state 6, `6. Underground ambience1.mp3`, while 0x180.

## 2. `gsGrid.viewMode` write (`write_viewMode`, `L:0x0813ef00` = `W:0x100b7c60`, field `L:+0x1fc` = `W:+0x21c`)

obs. Nothing happens when the value is unchanged. If the change touches bits **0xfe**: `beginUpdate` over the whole
map, store the value, clear the cover quadrants (`flags & 0xf0ff`) of every cell, run `gsGridItem::fixGrid` for every
item of every division (cover only where `viewMode & 0x20`, `L:0x08184890.c:404-417`, so the underground view has no
cover at all; normal overrides and the cell flag 0x10 are written as always), `gsGridVehicle::updateVisibility` for
every vehicle and `gsGridCharacter::updateVisibility` for every character (§5), `endUpdate` (every division rebuilt).
Else, if it touches the data bits **0xf7000**: store it and `gsGridItem::updateMesh` for the items of the near
divisions (the selection/data layers).

## 3. Divisions (`gsGridDivision::update`, `L:0x08166080` = `W:0x100d6ab0`)

obs. The builder reads the view into a local (`L:0x08166080.c:299`) and, for 0x180, decides per division
(`:309-345`, `W:0x100d6ab0.c:271-273`):

* **`+0x30` = 0** (no tunnel item near): the division is built with **0xc6**: the terrain surface of every cell (no
  cover) in `earthMaterial` with `uv0 · 0.5` (`:1232-1236`, `:2360-2370`; there are no yard or field groups without
  0x20), water as usual, the normal map-edge walls, and items through `isVisible(item, 0xc6)`: only TEMP items.
* **`+0x30` > 0** (a tunnel division): built with 0x180: no terrain surface and no water; instead the **earth plane**,
  a flat square per cell at **y = −0x800** (−8 cells, −24 m), normal +y, colour 0xff808080, uv `(gx, gz) · cs/64`,
  `earthMaterial` (`createEarthVertex` `L:0x08165a90`, emitted `:2375-2470` in lod 0 only; the y argument is lost in
  the Linux pseudocode, `W:0x100d6ab0.c:1605-1640` passes −0x800). Walls (`createEdgeVertex`, `edgeMaterial`):
  * on the map border as usual but with the top clamped to `min(h, −0x800)` (`:4300-4309`, `W:…:4117-4271`);
  * on each side whose neighbour division has `+0x30 == 0` (`local_42c`/`44c`/`408`/`448` = −1): a wall from the
    terrain height `h` of the border corners down to −0x2000, **facing into the division** (the index order is
    reversed, normal = −outward); uv as the map-edge walls (`v = 1 − (top + 0x2000)/(h + 0x2000)`, 1 at the bottom).
  * Items through `isVisible(item, 0x180)`: tunnel-rooted ones and TEMP previews.
  * The cover-edge base walls (`:3293-3736`, `h` to `h − 0xc0` along covered/uncovered quadrant edges) need cover
    bits, which the underground view clears (§2): none in practice.
  * The tool highlight of cells (`flags & 0x4000`) is built in this branch too, from the normal terrain vertices
    (`:2840-2900`, `createVertex(…, bool)` `L:0x081649b0`): at the **surface** height, above the pit.

`+0x30` is a counter kept by `gsGridDivision::addObject` / `removeObject` (`L:0x08163f80.c:41-75`,
`L:0x081643b0.c:190-215`): every item with TUNNEL (0x8000) in its flags (template | record) adds 1 to each division
that its footprint grown by one cell on every side overlaps, cells `[x−1, x+sx+1] × [z−1, z+sz+1]`, and calls
`beginUpdate`/`endUpdate` around it. This includes hidden items and the tool's TEMP previews: a subway preview digs
its own pit while the player drags it. `+0x34` caches `+0x30` at build time (`updateNearStatus`, `L:0x081743b0`).

The result: a brown, item-free landscape (water stays), with rectangular pits 24 m deep along every subway line, in
which the tunnels, stations and their stairs and halls stand on the earth plane; looking into a pit one sees its
inward walls. The −14 m camera target looks into the pits.

## 4. Items (`gsGridItem::isVisible`, `L:0x08185c50` = `W:0x100e8250`)

obs. TEMP (0x100000) → visible. Type TREE (0x13) → `viewMode & 8`. Otherwise the **root** parent decides: TUNNEL
(0x8000) and `viewMode & 0x100` → visible; root type 2..0x12 or root ALIGN_4 (0x08000000) → `viewMode & 0x10`; else
`viewMode & 0x20`. Underground: everything below a tunnel item (track pieces, underground stations with their
platform pieces, stairs, hall `service_metrou0` and entrance `stairs_metrou0b`, all TUNNEL), the tool previews, and
nothing else (roads, buildings, trees, elevated and ground metro, tram tracks are hidden).

Readers of `isVisible` and the grid's view:

| Reader | Effect underground | Address |
|---|---|---|
| division build (merged and instance items) | §3 | `L:0x08166080.c:1023-1044, 1971, 3740` |
| `gsGridItem::move` | an item the **grid's** view hides (0x180, not the division's) skips its emitters, skeletal animation and its `engineSound` / door `moveSound` loops (returns early) | `L:0x08182b10.c:53-70` |
| `gsGrid::intersectGrid` | picks only visible items (§6) | `L:0x081402f0.c:297-299` |
| `getAreaY` | items above 0x600 raise the camera only when visible | `L:0x0813ff00.c:104-105` |

## 5. Vehicles, characters, citizens' cars

obs. Meshes are faded with `gsGrid::blendMesh(alpha, mesh)` (`L:0x0814ce80`): a map of target alphas that
`gsGrid::move` steps toward the target each tick (`L:0x08148e60.c:767-810`; inf: by the tick's dt, about one second
for a full fade).

* **`gsGridVehicle::updateVisibility`** (`L:0x081ad310` = `W:0x10107220`), per car of a consist (each has its own
  node, `+0x24`): alpha 0 if `+0x54 < 0` or its node has flag 0x400000; else, in view **exactly 0x180**, 1 only if the
  node has flag **0x800000** (a node of a TUNNEL item), else 0; in every other view 1. Called on view changes and by
  every `gsGridVehicle::move` (`L:0x081adca0.c:469`); `gsGridVehicles::readData` does the same on load
  (`W:0x1010d330.c:411`).
* **`gsGridCharacter::updateVisibility`** (`L:0x08153ae0` = `W:0x100c77e0`): in view 0x180 a character shows only
  when it is on the map and `y + 0x80 < height(cell)` (its feet more than half a cell below the terrain of its
  cell: pedestrians on underground platforms, stairs and halls); else alpha 1.
* **Citizens' cars** (`gsGridCitizen::tickCar`, `L:0x0815b9d0.c:415-419`): when a car is put on the road its mesh
  gets alpha 0 in view 0x180, 1 otherwise; from then on the vehicle rule applies.
* Trams, buses, boats, aircraft and elevated or ground metro trains are therefore hidden; a metro train shows car by
  car while its cars are on tunnel nodes. The transition piece `metro_subway_ground` (`metroug.script`: COVER |
  LOCK_GROUND | ENABLE_BASE, type METRO_STATION) is not a TUNNEL item: neither it nor a train on its ramp shows.

## 6. Picking

* `intersectGrid` (`L:0x081402f0`) tests only items `isVisible` in the grid's view; with no cover in the underground
  view, no cell counts as fully covered, and the terrain is hit at its **real** height (the surface, not the earth
  plane), so the query tool picks the surface under a pit when no tunnel item is hit.
* The subway track tool and the underground station pick on the level plane: `intersectGrid` with the height
  `toolSize << 9` (−0x400/−0x600/−0x800), tunnel items only, the terrain as the plane at that height
  (`docs/ui/construction.md` §8). That does not depend on the view.

## 7. Sounds

* Vehicle loops (`gsGridVehicle::move`, `L:0x081adca0.c:487-494, 565-571`): the `engineSound` loop **and** the
  `moveSound`/`moveTunnelSound` loop play only while the view is not 0x180 or the vehicle's node has 0x800000 (or the
  vehicle is forced) (`docs/audio/README.md` §11.1).
* Item loops and door bells: see `gsGridItem::move` in §4.
* The ambience: §1.

## 8. Not affected

* **Minimap**: `gsGrid::renderMap` (`L:0x08143f60`) does not read `viewMode`; the minimap script passes its own data
  mode. The minimap looks the same in both views.
* **Shaders**: no shader or material variant for the view (nothing under `metro/shaders/` mentions it); the earth
  plane and the pit walls use `earthMaterial` and `edgeMaterial` (`S:scripts/engine.script:59-63, 147-151`).
* **Shadows**: the shadow camera is unchanged; hidden items do not cast. The clip plane moves to −24 (§1).
* **Simulation**: nothing in the traffic or citizen logic reads the view, except the cover bits (walk maps are built
  from items, not from the cell flags).

## 9. Port (`port/`)

`GridObj::viewMode`/`viewVersion` (engine/grid.h) with the field setter in engine/interaction.cpp (§2: cover cleared
and re-stamped by `fixGridCoverArea`, which stamps cover only with 0x20); `divisionViewMode`, `gridItemVisible`
(engine/grid_render.h, grid_items.cpp); the division builder (engine/terrain.cpp: `kEarth` group, earth plane, inward
walls, tunnel divisions recounted after every edit including previews); vehicles per car and characters
(engine/sim.cpp `unitOnTunnel`, sim_citizens.cpp `collectCharacters`); sound loops (engine/sim_sounds.cpp). The
normal view keeps its fast path (no visibility test, same meshes).

Deviations: visibility changes at once instead of the ~1 s `blendMesh` fade; the earth plane is built in both LODs
(the original only in lod 0, leaving far tunnel divisions without a floor); `getAreaY` does not count items in any
view; the data-view bits do not rebuild anything (no data layer).
