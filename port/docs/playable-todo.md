# ReGS: what is left for a playable game

State of 2026-11-04. "Playable" means: start or load a game, build stops and tracks, create lines, buy vehicles, earn
money while the city grows, save and load, and finish scenarios, with sound, as in the original. Ordered by what
blocks play most. Each item names its spec in `docs/` where one exists, or the reverse engineering (RE) it still needs.

What already works: menus, settings and the graphics page; loading saves and new sandbox maps; the 3D map with
shadows, bloom, animation and the minimap; hovering and selecting with the info panel and the corner preview; the
simulation of the existing network (trams on their lines, background traffic, trains, 32 520 citizens with daily
trips, parking, utility trips, breakdowns and accidents, pedestrians on walk layers, passengers and fares); saving
with the live simulation (zlib-compressed like the original); sandbox traffic and population density.

## 1. Construction tools (the biggest gap)

Spec: `docs/ui/construction.md`. The tool framework is done (`engine/tools.h`): the mode dispatch of mouseMove /
mouseDown / mouseUp / finishTool / cancelTool, preview items, replaced items, `checkConflict`, `mountObject`, the
price lists the hover script reads, and the green/red highlight overlay; the select mode runs on it. The game's
modes (the editor's roads, terrain and paint tools come later):
- [x] Lay tram tracks and metro tracks (ground, elevated with pillars, underground on the three levels) with junctions,
  slopes, conflicts and prices (`engine/tool_tracks.cpp`, `docs/ui/construction.md` §6-§9); station track parts
  through `toolBlocks().track`.
- [~] Lay roads and alleys (editor only, `moveLayRoad` in `engine/tool_tracks.cpp`): small, large, avenue and express
  roads, alleys, straight, diagonal, junctions where a road crosses one, dead ends; `createRoadBlock` joins with the
  track tools' rules, not the original's own map-edge and ramp code. Bridges and tunnels (modes 0xb, 0xc) are
  `moveLayBridge`; the double track laid across a small road becomes the level crossing (checked in the editor on
  testmap, `port/run/gaps/double-track-crossing.png`), and so does a small road laid across the track. The road joins
  follow the original (roads of different kinds join, a small road ends beside a wide one at the half the cursor is
  on, the elevated ramp's heights); checked in the editor. Port (owner's request): level crossings with tram tracks,
  across large roads and across alleys (`port/docs/building-tools.md` "Level crossings").
- [x] The underground view (`GRID_VIEW_UNDERGROUND`, `docs/renderer/underground.md`): the minimap's layer button, the
  "U" key and the subway tool switch it as in the original; the map turns into bare earth with pits 24 m deep around
  the tunnel items, in which the subway tracks, underground stations with their stairs and halls, metro trains on
  tunnel nodes and the people on the platforms show; previews dig their own pits while dragging; picking, sounds and
  the camera follow. Checked on Vienna and Berlin (laying a tunnel and placing an underground station with the view
  on). Not modelled: the one-second fade of vehicles and pedestrians.
- [x] Place stops and stations: bus and tram stops, metro stations and transitions, docks, heliports
  (`GRIDMODE_CREATE_STOP`, `selectStation`; `engine/tool_objects.cpp`, construction.md §11-12). Underground stations
  placed in the underground view (Vienna). Still to see on screen: heliport, elevated stations, the metro transition. Port QoL: a stop upgrade costs the new
  price minus the old stop's demolition refund (`data/regs/extend/metro/ui/hoverinfo/hoverinfo.script`); the stop
  keeps its name and lines. The whole loop (lay track, place stops, new line, buy trams, run, save, reload) checked on
  Cim2.
- [x] Bulldoze (`GRIDMODE_REMOVE_OBJECTS`), with the demolition cost of buildings (construction.md §13).
- [x] Terraforming and paint tools (`engine/tool_terrain.cpp`): shift, level, soften, pavement, fields, ruin, grow
  trees, toggle water (the original's basin flood fill); port QoL: round brushes (setting `regsRoundBrushes`, on).
  Pavement paint with exact edges (2026-11-14): the round brush and its eraser cut the cells on the brush circle,
  painted areas (paint_edges.h, for the OSM import) cut along their outline; see building-tools.md.
  Rename (0x14, the toolbar's T: the hovered item, the game's dialog, timeline names) and variation (0x15, the
  original's debug menu) work on the hovered item; rename takes the road segment up to the junctions (right button: one piece).
- [x] Undo: the editor's undo button (timeline `removeLastAction` + base grid replay); every tool records on
  `gsGridPlayer.timeline`. The brushes' drag copy (`addUndoItem`) is put back on cancel.
- [x] Live map updates after an edit (`engine/grid_edit.h`, used by the city timeline): items added and removed in
  batches, the item and terrain meshes of the touched divisions, cell cover (`fixGrid`) and ground levelling under
  LOCK_GROUND items, the lane graph and walk map (simulation hooks), picking, the minimap raster with its transport
  marks (`docs/ui/minimap.md` §2.4, §3.2: stops and line legs from the live simulation, black edge and type colour;
  Vienna's tram, metro, water bus and helicopter lines checked in the minimap texture; the bridge and tunnel pieces'
  sampling step is the port's, 4 per piece), the full `checkConflict` (walk-map masks, diagonal corners) and
  `removeOverlappingOptionalObjects` rules (2026-11-18; checked on Vienna's own items, construction.md §10). Still
  open: undo of live edits.

## 2. Lines and vehicles

- [x] Create, edit and delete lines (`docs/ui/lines.md`, `engine/sim_lines.cpp`): new lines, the add-stop tool
  (`GRIDMODE_ADD_STOP`, `engine/tool_addstop.cpp`: hover over a stop or its bubble, click, close the ring), moving
  and deleting stops, pausing (vehicles leave the map), deleting lines; new lines route, run and survive a save and
  reload (Vienna, Berlin bus and tram lines checked). Route strips on the map (`engine/sim_paths.cpp`; water bus legs
  smoothed as the original, a C1 Bézier chain through every (nodes/8)-th node, `docs/ui/lines.md` §3,
  `port/run/gaps/boat-compare.png`), stop bubbles (`transportStopCreated`), stops in the company colour
  (`gsGridItemController.setColor`, `docs/renderer/grid-items.md` §2.5; Berlin's blue,
  `port/run/gaps/stopcolor-compare.png`), the bubbles' overlap layout (`gsTracerOrganizer`, `docs/ui/lines.md` §4.1,
  `port/run/gaps/busy-compare.png`). Still open: the strips' coverage counters.
- [x] Buy, sell and assign vehicles: shop with the turning 3D previews (`renderViewport`), depot, to/from the line,
  selling; usage and condition gauges (`gsGaugeCtrl`); condition and repairs are script-side and run.
- [x] `routeExists`, `countStops`, `countLines`, `calculateCoverage`, `countConnectedStops`.
- [x] Panels: the empty first tab was right (Cim2 has no bus line); the tram tab, line statistics and vehicle lists
  work. The shop leaves out community-mod vehicles whose templates are not loaded
  (`data/regs/extend/metro/ui/vehiclepanel/vehiclepanel.script`; the save keeps them).

## 3. The city over time

- [x] City growth: `gsGridTimeline` (`engine/timeline.cpp`, `docs/save-format/lines-timeline.md`): sandbox starts
  replay the history up to the start year (Berlin 1980: 21 403 actions in under 1 s), each game day applies its
  actions and previews the next 182 days (construction sites), saves write the timeline, terrain and network from
  the live map. Brush falloff and water edges checked against the original (2026-11-18, `applyBrush` against
  L:0x081a3ce0: the falloff's sin in double as the editor's tool, the ruin amount in double, the water edges once
  after all points of an action). Still open: the editor's timeline UI.
- [x] New maps from the editor and the scenario start dates: the new-map dialog's earliest start year goes into the
  map's .scenario (`currentDate`), the sandbox page offers it as the first year, and (port, 2026-11-18) a game
  started without the sandbox page (the editor's "Speichern & Spielen") begins in it instead of sandbox.scenario's
  1920 (`data/regs/extend/metro/scripts/main.script` loadSandboxScenario). Checked: a new 128-cell map with 1940,
  saved and played from the editor, starts "Januar 2 1940". Port: number fields (the start years) also take a
  click and typing (ui.script extension; Enter or a click elsewhere takes the value, clamped, Escape keeps the old one).

## 4. Simulation gaps

- [x] Pedestrians walking through water and at the wrong height: buildings, bridges, tunnels, platforms and raised
  bases are walk layers at their own heights (`docs/simulation/pathfinding.md` §1.4); 0 pedestrians over water.
- [x] Parking, utility trips and company fleets, event venue opening days, building doors from the walk maps.
- [x] Breakdowns (evacuation, replanning, repair) and accidents of reckless drivers (checked with raised rates).
- [x] Live map edits: the lane graph, walk layers, place pools, stops and vehicles follow added and removed items
  (`simItemsChanged`, `simTerrainChanged`); used by the city growth and later by the construction tools.
- [x] Boats and aircraft: generated water lanes (`docs/simulation/boats-aircraft.md`), background boats and planes
  (`setBoats`, `setAirplanes`), water bus and helicopter lines with helicopter separation; checked on screen in the
  Vienna save (helicopter take-off, flight and landing; water bus docking; elevated metro stations).
- [x] Metro alternative platforms; every line of the Berlin and Vienna saves routes with 0 unreachable segments.
- [x] Animals (seagulls, cows, pigs), `getPassengersByLine`, vehicle door animation from the stop cycle, the walking
  stride (checked on screen), `getUnemployment` as a fraction, `routeExists`/`countLines`/`countStops`.
- [x] Sandbox densities measured. Traffic on Cim2 (moving road vehicles after 270 s at speed 10): 0.5× → 0.58,
  2× → 2.33, 4× → 4.6 of 1× (below 1× the traffic present at load is thinned too). Population on Tutorial-Stadt:
  home slots 2 931 / 5 347 / 10 410 at 0.5× / 1× / 2× (every building keeps at least one home), and a 2× game fills
  all 10 410 within seconds.
- [x] Metro reversing at a terminus: Vienna's metro lines 3 and 5 are shuttles; 30 turn-rounds, 0 failures
  (`port/run/metro-turn-strip.png`); `REGS_TEST_TERMINUS=<line>` cuts any line to two stops for tests.
- [x] Aircraft landing gear and doors animated (not yet seen on screen).
- [x] Boarding through the doors in mood order: waiters walk to the nearest car's door once the vehicle stands with
  open doors and no higher mood tier waits; the doors stay open while they walk (`port/run/door-strip.png`). Far
  from the camera, or more than 40 units from the doors, they board at once.
- [x] Crashes seen on screen (`REGS_TEST_CRASH=<n>`, `port/run/crash-strip.png`); the 30 s clear-up not filmed yet.
- [x] Jam relief for buses and trams (port QoL, `docs/simulation/transport.md` §10): stuck 2 minutes, the cars around
  turn round or leave one every 3 s; stuck 30 s more, the vehicle moves on two stops and its riders plan again; the
  port's ticker message follows the original's. `REGS_NO_JAM_RELIEF=1` off, `REGS_TEST_JAM=<type>[:hard]` test.
  Found on the way: despawned cars left lane claims behind (a node handed to them as the queued reserver past their
  held range), which locked Berlin's and Vienna's tram and bus lines within minutes; fixed in `releaseAll`.
- [x] Jams the relief did not cover (2026-11-18, `docs/simulation/transport.md` §10.3, §10.4): cars queued behind
  the car ahead on any blocked node, even a marked one inside a junction, and were promoted onto it when it left, so
  one car after another held the crossing nodes a tram waited for; the same queuing locked two metro trains at the
  termini of Berlin's lines 0 and 4. Now a vehicle queues only where the original's platoon loop would (not marked,
  not a junction node, right after its committed range). Berlin at speed 10: 59 detections in 3600 s and 3720
  departures before, 0 detections in 3000 s and 6343 departures after; Vienna and Cim2 0 detections.
- [ ] Pedestrian walks without a path, re-measured 2026-11-18 on main (9000 frames at speed 10, debug sim log):
  Vienna 2 of 13 325 (0.02 %; one starts at street level inside the footprint of the elevated metro station
  `station_metroe0` at q-cell 1428,1823, where only 4 cells are reachable: who is put on the ground there instead of
  the platform layer is not traced); Berlin 4 of 10 021 (0.04 %); Cim2 86 of 4740 (1.8 %, was 1.7 %), all of them
  the two known cases: walks into or out of the sunken park (`tent`, `pavillion`, `city-stairs` at height −256; its
  stairs prop has no walk map and blocks its cell, the original cannot reach it either) and from the tram stop
  `stop_tram03_small` to the hotel enclosed by bushes (`hotel01`). The walk and height maps are turned as the engine
  turns them (`docs/simulation/pathfinding.md` §1.4), which puts the waiters of underground and elevated metro
  stations on their platforms.
- [x] Metro trains at the turn of a line (end stations) stop with their middle at the platform (2026-11-15: straight on
  along their own track before turning; `docs/simulation/transport.md` §4.4, port note).
- [x] Deviations revisited (2026-11-18, `docs/simulation/README.md`): water cells stay unwalkable (the user asked for
  it; the original treats them as open ground). Boats: the original's saves now load as in the original (their
  background boats carry agent flag 1 and are not placed, Vienna 4, Berlin 6; new boats fill the caps of 4 and 5 again
  at once), while the port's own saves
  write no flag 1 and keep their boats running (deliberate, the original despawns them at every save).
- [x] Saves keep the live simulation, including boats, aircraft, animals and the rest of each pedestrian's walk (the
  original's direction codes, also read from the original's saves: 99 % of Cim2's walks continue after loading).
  Round trips on Cim2, Vienna and Berlin reload matching counts.
- [x] Saves are one-way by decision: the port loads the original's saves, but port saves never have to load in the
  original game (so port-only features may change the save content). Seated state and the vehicles' transient fields
  are written as 0 (the original's reset value) because the port doesn't model them.
- [x] The `$portraitNames` script error after loading a port-written Berlin save (`level.scenario:2167`) was not a save
  problem: `gsGridCitizens.getCharacter(class)` was an unported stub that returned *unset*, which passes the scripts'
  `$character == null` test (unset ≠ null), so `$character.$portraitNames` failed. 18 random tasks of every
  scenario level (`start_sandbox_task76`…`93`) call it, so the error hit original and port saves alike, just rarely
  (a 93/10 000 draw every 9.51 s). Ported from `0x100ce7f0` (a uniform pick among the class's character records of
  the era and gender; returns the character model object, null if none). Checked with
  `REGS_TEST_SCENARIO_CALL` starting all 18 tasks on the original `berlin.gs` and on port saves of Berlin, Vienna
  and Cim2: 0 script errors; port saves of all three reload with the month's books closing exactly.

## 5. Economy, scenarios and campaign

- [x] Economy check (`docs/simulation/economy.md`, `REGS_DEBUG_ECONOMY=1`): the scripts' formulas written down
  and recomputed against the port's numbers; every month of every run closes exactly (money moved = fares +
  penalties + sales + purchases + running costs). Berlin save 38 months (maintenance, wages identical to the
  original's last month in the save; energy consistent; coverage and passengers in the save's range; board and
  customer ratings recomputed), campaign scenario 1 from the menu for 19 months (a task accepted and completed
  with its cash and reputation reward, two achievements, the annual report), a new Wien sandbox with a loan
  (annuity 223,70 and the monthly balance to the cent), the Vienna sandbox save for 25 months across a port save
  and reload, Cim2; scenario 2 from the Szenarien menu (debug list) for 13 months with a forced task completion
  (F2); the tutorial through its first task. Fixed on the way (were stubs): `gsGridCitizens.getCharacter`,
  `clearGameData`, `resetTraffic` (new games' pre-simulation) and the debug console's `trimFirstLines`. Script
  quirk kept as in the original: `monthlyProfit` uses the previous month's fares (§2 of the spec).
- [x] Demonstrations and parades: `gsGridItemController.activate`/`deactivate` set a per-item type override
  (marker hidden / none drawn); Vienna's demonstration crowd appears in front of the town hall
  (Vulkan, `REGS_TEST_SCENARIO_CALL=60:start_demonstration=30`, the `=arg` form is new) and is gone again when a
  1-day demonstration ends (GL). The parade (`start_parade=30`, one item at cell 498,492 by the cathedral) shows its
  marching columns and vans on the street; road traffic still drives through it (not compared with the original). Left: the lane/walk graph is not
  rebuilt on the call, picking ignores the override, the override is not saved. `setClusterData` (scenario 4) is
  ported (clusters.bmp, citizens pick workplaces and visit places in their home's cluster) but not run yet.
- [x] Scenarios 1–12 and the tutorial start from `REGS_START=scenario:<n>` (also `campaign`, `tutorial`; the
  instructions dialog still needs its Start click, at 782,693 in 1920×1080) and run 5000 frames at speed 3 without
  script errors or missing natives. Fixed on the way: the older grid names section 0x07020300 (scenario maps lost all
  item names), the train station stops' attractiveness and maintenance cost, achievements as no-ops. Scenario 4
  loads `clusters.bmp` (1280×1280).
- [ ] Still to run: the scenarios past their first tasks (F2 forces an objective), the tutorial past its first
  tasks, a whole campaign; the `campaign` and `tutorial` start flags are untested.
- [x] Statistics and graphs: `gsGraphCtrl` (`setArg`, `update`, `reset`, `setColor`, `getColor`, its fields and the
  automatic update) draws series strips, the grid and the value labels as the original computes them
  (`engine/ui_graphs.cpp`, spec `docs/ui/graphs.md`). The graphs panel (12 company and economy graphs, the 12–60 month
  slider, monthly updates while open) and the annual report's passenger graph work on Vienna and Berlin
  (`port/run/graphs/v-*.png`, `b-*.png`). The coverage, usage and car-usage statistics now get real data:
  `gsGridCitizens.get…Count(-1)` sums all classes, as the original does. The news ticker is script-only and already
  ran (scrolling, colours, sounds, camera jump; `docs/ui/graphs.md` §6).

## 6. Sound and music (milestone 7)

- [x] `gsSound` (`load`, `play`, `pause`, 2D and 3D sounds), music, `set_audio_volume`, `enable_sounds`, vehicle
  sounds (`gsGridVehicleController.playSound`). Done with miniaudio's device and decoders and a mixer of the port's
  own: script sounds, ambience by area, world sounds of vehicles, animals and items, speaker modes and a Headphones
  mode (`docs/audio/README.md` §18).
- [x] Door-item sounds: the level-crossing bell loops while a train holds the crossing (`engine/sim_sounds.cpp`
  `tickDoors`; `port/run/barriers/bell-f.wav`).
- [x] Separate volumes (port addition): Effects, Ambience and Interface sliders on the Audio page under the master
  volume (`$regsEffectsVolume`, `$regsAmbienceVolume`, `$regsInterfaceVolume`), and the optional menu-music fix
  `$regsMenuMusicFollowsMusicVolume` (off by default, as the original). Sounds are tagged where they are created;
  rule and measurements in `docs/audio/README.md` §18.7.
- [ ] A Prologic encoder with phase shift.

## 7. Effects and remaining graphics

- [x] Sky mesh (`gsSky.generateMesh`), sun disc, night lighting with the illumination maps, `gsTracerCtrl.enableFade`
  (`engine/sky.cpp`, `docs/renderer/sky-and-night.md`). The original has no day/night cycle (fixed time per location;
  New York and Tokyo are the night maps, lit by their DLC illumination maps and additive street-light glows, which
  now blend additively). Port option "Tag und Nacht" adds a cycle; `REGS_DEBUG_TIME=<hour>` for tests
  (`port/run/sky/*.png`). Open: the sun's glow sprite (occlusion-query halo).
- [x] Particles (`gsParticleEmitter`, `gsEmittingParticleEmitter`, `gsParticleEngine`; `engine/particles.cpp`,
  `docs/renderer/particles.md`): factory and ship smoke, fountains, fire and sparks, broken-down and crash smoke,
  helicopter dust, the demolish and construction dust; sorted lit billboards with shadows, capped pool. Port change:
  helicopters board with a still rotor (`docs/simulation/boats-aircraft.md` §6.3). Screenshots in
  `port/run/particles/`.
- [ ] Boat wakes (`gsBoatContrain`, docs/renderer/particles.md §6.1): WIP. `engine/particles.cpp` `tickWakes` keeps the
  16-point ring per boat and `collectBoatWakes` draws the ribbon with the emitter's `contrain.shader` material. Builds and
  runs on GL and Vulkan, but no wake has been seen on screen yet. `REGS_DEBUG_PARTICLES=1` logs every wake: in
  Vienna the 4 wakes are the background boats (movers 856-989, density 2/s, maxDistance 100000), all 820-955 units from
  the camera when it follows water bus `REGS_DEBUG_FOLLOW=3:1` (mover 7), so they are culled at farZ 310. Fixed: the
  wake used to need `Mover.boat`, which marks only background boats; now every mover whose model has a wake emitter
  keeps one, and the water bus has its wake (12 points, 155 units from the camera; `port`'s test frame
  `wake-fix-490` shows at most a faint streak behind it; its `planeSize` is 1.2, a ribbon ~2.4 units wide, a hairline from
  that height). Next: look at the ribbon closely (zoomed camera, moving
  boat) on GL and Vulkan and check its blend, alpha and width (`planeSize`) against the original.
- [ ] Fires as a simulation event (building pass, `docs/simulation/grid-traffic.md` §4.4).
- [x] Door items: level-crossing barriers lower while a train holds the crossing's track nodes and rise after it,
  cars wait before the crossing (`engine/sim_doors.cpp`, `docs/simulation/grid-traffic.md` §5.1.1;
  `port/run/barriers/sheet-f-rise.png`, `sheet-f-wait.png`).
- [x] Map-edge walls (were already drawn; `port/run/gaps/edge-s.png`), the foundation walls under `ENABLE_BASE` items
  on uneven ground (`docs/renderer/grid-items.md` §3.2.5: the four diagonal cuts and the straight sides, 530 items in
  Vienna, mostly houses and the river quays; `port/run/gaps/base-platform.png`), the water-edge curtain where the map's
  border meets water (`docs/renderer/terrain.md` §7.3; `port/run/gaps/water-s1.png`), the stop catchment circle (the
  grid's data layer with `dataMaterial`, `terrain.md` §5.2: every stop of a stop tool's kind and the preview while
  placing, the stop in the corner view when selected; `port/run/gaps/place-circles.png`, `sel-stop-circle.png`), the
  tracer organizer (`docs/ui/lines.md` §4.1; `port/run/gaps/busy-compare.png`). All in `engine/terrain.cpp`,
  `engine/interaction.cpp`, `engine/display.cpp`, `engine/sim_paths.cpp`.
- [ ] Scene nodes attached to bones.

## 8. Map editor

Status 2026-11-07 (branch claude/map-editor-4d5476), checked in screenshots (`port/run/editor/`, test script
`port/run/editor/run.ps1 -NewMap`):

- [x] New map: Karteneditor → Neue Karte → Kartenattribute → an empty flat map (`gsGrid.setSize`).
- [x] Terrain: raise/lower (shift), level, soften, ruin, pavement and field paint, trees; water fills a dug basin.
- [x] Objects: buildings, props and markers from the object lists, stops and stations, the bulldozer.
- [~] Roads and alleys (see §1); bridges and tunnels laid (moveLayBridge).
- [x] Undo button (timeline replay over `memory/base.grid`).
- [x] Timeline: the original shows the time bar only in debug mode (`$debug = true`, `$disableDebug = false`); there
  dated edits, the day's action list with its labels, moving between years and saving the timeline work (needed the
  clock to fire onDay when its date is set, as the original's `write_date`).
- [x] Saving to `userdata/maps`: .grid, .timeline, .scenario and the .dds minimap thumbnail
  (`gsTextureVariant_OGL.saveToFile`, A8R8G8B8 with mips).
- [x] Loading: Karte laden into the editor; the saved map in the sandbox list (with its thumbnail) and a sandbox game
  on it.
- [x] Content packages (port, owner's request): the map attributes list the installed DLC/mod packages
  (`addons/*`, `userdata/addons/*`) to tick; their buildings, props and foliage join the editor (names already
  registered are kept). The map's .scenario stores `$regsPackages` (those of its objects and location) and
  `$regsSelectedPackages`, which the original ignores. Opening a map with a package missing (editor list, sandbox
  start) names it and offers "Open anyway" or cancel; missing objects stay invisible and are kept in the files.
  Not checked yet: a DLC object actually placed showing up in `$regsPackages`; vehicle packs are not filtered.
- [x] Undo keeps working after saving (same session).
- [x] Port additions (owner's requests 2026-11-07): content packages per map, map sizes 128-2048 and enlarging,
  autosave with 5 versions, the object list's package filter and search, a vehicles category, folders and a map
  manager window (Karte laden: new folder, rename, move, delete, preview), templates (objects menu: save an area's
  terrain and objects, stamp it elsewhere; replaces what it touches; recorded on the timeline, so undo works).
  Templates: Shift adds/removes objects and areas, "Gelände speichern" option, a screenshot as the picture, and
  their own file format .cimtpl (GS10 package with template.grid, template.dds, template.info listing the DLCs of its
  objects) with export (Explorer) and import (drop into the folder), the missing-DLC check before placing, and the
  DLCs a template needs loaded when placing it. Still no rotation.
- [~] OpenStreetMap import for a new map (owner's request 2026-11-07; engine/osm.cpp, regs/ui/osmwindow.script): the
  new-map dialog has "Aus OpenStreetMap …" (a window with Esri satellite / OSM map tiles, Nominatim search, start
  view at the chosen location's city, the area as a rectangle of the map size × a scale 1-15, options terrain,
  water, trees, streets, buildings, satellite picture); the import runs when the new map stands, with a progress
  window, as one timeline group (one undo). Done: downloads with disk cache (tiles, Overpass answers for a week),
  terrain from AWS Terrarium heights, sea (coastline flood), lakes and rivers, trees (woods closed: a big tree in every cell, a second
  one in most, bushes as undergrowth, turned and shifted freely; scrub mostly bushes; orchards fruit trees in rows; parks, tree rows, single trees; owner 2026-11-08: no stones or grass tufts), road
  kinds from the tags (avenue for tree-lined or divided roads with a tree row, large for multi-lane/12 m+/primary/
  secondary, small, footpaths for pedestrian streets; divided roads merged to one centre line, the side streets
  following onto it; carriageways without a partner one-way small roads; roundabouts one-way rings). Since
  2026-11-08 the streets are curves (the curve tools of building-tools.md, owner's request): one Bézier per
  simplified leg, Catmull-Rom tangents, junctions at shared OSM nodes, legs shorter than their junctions need
  merged, arms spread to 32°, too tight bends made junctions; ways leaving the map end on two grid blocks (the edge
  piece, then a straight piece the curve starts on); water cuts ways (streams up to 40 m: a causeway across), bridges crossing streets without a shared
  node are left out, footpaths keep off roads; the ground beside the streets levelled 3 units wide (hills).
  REGS_OSM_GRID=1 builds the old block-grid network instead. Buildings fitted to footprints by category (wall-to-wall ones as the city houses / corner houses in rows), turned to
  the footprint's angle (yaw and offX/offZ of the free turn; REGS_OSM_NOTURN=1 off).
  2026-11-13 (owner's test runs, Bruchsal): road kinds from the real lane count (lanes 2-3 small, 4+ large), one kind
  per named street, kind changes as junction pieces; roundabouts rebuilt as the game's roundabouts (radius for their
  arms); bridges as the curve tool's bridge curves (OSM bridges and legs over water, decks clear of what they cross);
  short tunnels (underpasses up to 80 m) on the ground with a bridge for the track over them; railways (rail,
  light_rail, no sidings) as railway curves, parallel tracks closer than 4.5 cells merged into one double track;
  stations (railway=station/halt) as the game's railway-station beside the line; buildings lined up along their
  street (angle, front, building line), rows of wall-to-wall buildings as city houses edge to edge per street side,
  corner houses only at street corners and of the type whose corner faces the crossing street (research of the
  original maps: city houses front on model -Z, corner houses 01/02/05/06 also +X, 03/04 and small-townbuilding04
  also -X); rounded river banks except at buildings.
  2026-11-14: blocks of row houses (placeBlock): wall-to-wall buildings form a block, each building goes to the side
  along the street in front of it (curve and side, cut where the street bends > 12°), each side a straight row on the
  median building line, outer block corners (sides 60-120° apart) get a corner house both rows run up to, rows filled
  edge to edge with one house family per block (leftover taken by wider houses). Furnishing (a stage between buildings
  and trees): OSM street furniture nodes (fountain, bench, waste_basket, bicycle_parking, telephone, post_box,
  toilets, clock, street_lamp, memorial/monument, artwork, kiosk, playground) at their points facing the street;
  squares a fountain (large one from 150 cells) with benches facing it, litter bins, flower beds, old lamps along the
  edge with trees between; market places with market stands; car parks in asphalt (pavement paint 0) with rows of
  parking-space props across the long side (bay, aisle, two bays back to back, …), no parking lanes
  (parking=lane/street_side); street lamps along the curves where OSM has none (small streets one side every 28 m,
  wide roads both sides staggered with the arm lamp, footpaths the old lantern); yards behind the rows (free ground
  closed within 6000 cells, else 6 cells deep) with bushes along the houses, trees and residents' props. Signs in the
  building lists (grocerysign) are no buildings any more; no paint bands along pedestrian streets / service roads
  (the saw-tooth strips). REGS_DEBUG_OSM=1 logs what the furnishing placed.
  Templates from the game's own maps (owner 2026-11-14): at the first import the official maps (berlin, amsterdam,
  helsinki, vienna, cologne, leipzig, munich, ulm, london, paris, stpetersburg; read from the install, nothing copied
  into the port) give square templates (pavement patches with furniture) and yard templates (open ground mostly
  closed by houses, with garden things): ~1100 and ~100, 0.8 s. An OSM square or a closed yard takes the best-covering
  one (IoU ≥ 0.5 / 0.4, turned to its main axis in 90° steps) and its furniture where it lands on free cells; else the
  furnishing above. Culverted / covered rivers (tunnel=*, covered=yes, layer < 0) are no open water.
  REGS_DEBUG_TOPDOWN=x,z,height puts the camera straight above world (x, z) for screenshots.
  2026-11-14 later: tracks run to the map edge (no margin), stretches of OSM tracks running beside another only part
  of their way are kept (joined to the merged line), spurs up to 120 cells and loose pieces up to 400 cells that end in
  the open are dropped (pruneTracks; REGS_DEBUG_OSM_RAILS=<png> draws OSM's railways and the track curves). Stations
  built like Berlin's: platform pieces in two rows by the line, the railway-station behind them with its back to the
  line, an island platform to a parallel line 5.5-14 cells away, platform lamps and benches. Track cells are marked
  apart (used 7): no street furniture on tracks. Yard templates take paved ground too and bring their pavement
  paint; yards get a band of pavement along the houses' backs.
  Exact edges (paint_edges.h, main 43b7c9c): car parks (asphalt 0), squares and market places (paving stones 2) are
  painted with paintArea from their OSM rings at the terrain stage (timelineRecordPaintEdges after the patch); the
  yards' pavement too: a turned rectangle 2.2 cells deep behind each row, and the yard templates' paved cells traced
  to outlines, turned onto the yard and painted with paintAreaCells on the yard's own cells (Painter in osm.cpp); the traced
  outlines lose their stair steps (corners within 0.75 cells of their neighbours' line). Pedestrian streets get a 6 m
  band of paving stones with exact edges (a rectangle per leg, an octagon per bend; buildings may stand on it).
  Squares, market places and car parks are dry (water under them dropped). Street trams: railway=tram ways (not in
  tunnels) give the road curves along which 60 % of the samples lie within the carriageway the tram tracks (ends 0x4);
  tested in Karlsruhe (most of its centre's trams run in the tunnel now).
  Level crossings (owner 2026-11-15, osm.cpp cutLevelCrossings / placeLevelCrossings): where a railway curve crosses a
  small road, the game's track_double_cross_small (barriers, lights, bell; the road along its local x, the track along
  z) is turned freely onto the track (orient + yaw, offX/offZ); the track ends on its two track sides, the road is cut
  off and joined to its road sides by connector curves that bend it square to the track; with no room (a junction right
  by the track) a free end becomes a stub and a junction moves off the track (as near as the junction's own trim
  allows, findCurveJunctions on the moved curves). The piece's own levelling (it sets and locks its 4 × 4 cells'
  corners at its pavement height) is undone around the turned piece. Where no piece fits (larger roads, tram streets,
  flatter than 30°, junctions on both sides) the rails are let into the road: the track's stretch under the roads is a
  curve with bridge = 3 (2 is a tunnel), drawn as the street tram strip at the road's height (terrain.cpp), its lanes those of the
  double track. Tested in Münzesheim (a piece) and Unteröwisheim (rails in the road between two junctions); REGS_DEBUG_OSM
  logs each crossing, REGS_DEBUG_TOPDOWN=x,z,h,d,y looks at it from d units south.
  Trams on their own bed (owner 2026-11-15, osm.cpp tramBeds): the tram ways are merged into double tracks (both
  directions' tracks closer than 6 m, mergeTracks with the tram kind) and cut where they run along a street (within its
  half width and half a cell: street trams as before); the stretches off the streets (10 cells and longer) become curves
  of kind 9 (curve_roads.h curveIsTramBed; 8 is the ramp): drawn as the railway's double track (ballast, sleepers, bumper), their lanes
  the tram piece's (tram_small_basic, tram_small_deadend at a free end), so the player's trams can run on them; in the
  roads' join class, so a bed meets its street at a junction (the street gets the junction point; a bed's end within 9
  cells of a street is joined to it too), where the trams run on between the bed and the street's tracks. Tested at
  Durlacher Tor, Karlsruhe (9 beds, 9 ends joined).
  The import window has a checkbox for the tram tracks (owner 2026-11-15, flag 128): off, neither street trams nor
  tram beds are laid.
  The satellite picture on the ground (owner 2026-11-07; terrain.cpp collectGroundOverlay): after an import with the
  picture, a button with a satellite above the minimap's own two (editor.script extension; icon drawn by
  tools/make-satellite-icon.ps1) lays it on the ground or takes it away: one mesh over the cells whose ground is drawn
  (not under the cover of items and curves), just over the terrain and under the streets' asphalt, unlit and three
  quarters opaque. It belongs to the map it was imported onto (GridObj::generation) and is not saved.
  Generated places (owner 2026-11-15: templates for common structures with variables like their size; osm.cpp
  buildMarket, buildPlayground, buildPitch, buildAllotments, buildSeating, buildTerrace): laid out in the place's minimum
  rectangle and scaled to it, a prop only where its turned footprint lies inside and its cells are free.
  - Markets (amenity=marketplace areas, squares whose name holds "markt"/"market"/"march", squares with a marketplace
    point): rows of stands back to back with 3-cell aisles, cross aisles, tables with parasols, baskets, signs at the
    aisles' ends, a big tent at one end of large ones; OSM's fountains and monuments keep 4.5 cells free. A marketplace
    point on no square gets a small market (14 × 10 cells).
  - Playgrounds (leisure=playground areas, points: 8 × 8 cells): merry-go-round from 150 cells, sandboxes, pairs of swings,
    a sand pile, each farthest from the others; benches facing in, litter bins, bushes and trees along the edge.
  - Pitches (leisure=pitch by sport): tennis courts and basketball fields side by side, their players over every other; a
    football pitch only as the game's whole field.
  - Allotments (landuse=allotments): rows of plots 7 × 9 cells behind painted paths, each with a shed, a tree, flower
    beds, sometimes a gnome or a swing, hedges between the plots.
  - Beer gardens (amenity=biergarten; a point gets 10 × 8 cells beside it) and outdoor seating areas: tables in a grid,
    trees (beer gardens, with their sign toward the street) or parasols.
  - Terraces before cafes, restaurants, pubs and fast food (OSM's outdoor_seating, else cafes always, restaurants and
    pubs every other one): from the place toward its street, on the first free ground two tables deep, a row of tables
    (parasols between a cafe's, flower pots or a pub's sign at the ends).
  - Single props: vending machines, taxi ranks, recycling containers, shelters, picnic tables.
  - Fuller places (owner: places looked empty): markets get a second pass of stands on a lattice in the room the rows
    left; every other square gets its free paving filled (fillSquare: a lattice of 4.5 cells, about one thing per 20
    cells: trees, benches facing the middle, flower beds, bins, lanterns, bike stands by the edge, now and then an
    advertising pillar, a phone box or a kiosk).
  - Footpaths without grass beneath them (owner): the footpath texture is a gravel path on grass covering the piece's
    whole width; the footpath curves and their junctions are drawn in a mesh of their own with a copy of the footpath
    material whose grass is transparent (alpha from blue against the brighter of red and green: grass and its mix at the edge are low in blue; terrain.cpp keyedFootpathTexture),
    so the ground around the gravel shows, paving or grass. (A first try left footpaths over paving out altogether; the
    owner missed them.) A footpath over paving or asphalt for half its length or more (and a junction most of whose
    arms are) is drawn as the same path in light concrete slabs with a darker kerb (owner: nicer on asphalt).
  - Beaches (natural=beach, leisure=beach_resort, not private): the ground ramps from just above the water line at the
    water to the land's own height at the beach's landward edge (by the share of the way across, squared: low and flat in
    front, the rise at the back, no cliff); the water before it is shallow and deepens over 10 cells; the strip between
    OSM's beach outline and the water joins the beach. Its cells are sand: cell flag 0x0080 (the game never sets it; the
    timeline patches and templates carry it, mask 0x20BF), which terrain.cpp turns into a sea-floor weight over 1 and the
    terrain shader's port patch (shaders.cpp, terrain/maps.fragment) into the sea floor brightened to dry sand. Parasols
    in loose rows two cells and more from the water, a beach bar (kiosk, tables), bins and a toilet along the edge,
    motorboats off the shore. Seen at Warnemuende. The game has no deck chairs or towels.
  Patchy ground fixed (owner 2026-11-15): a footprint no single template fits on its free cells is filled with as
  many buildings of its kind as fit (fillFootprint: the own kind first, the largest first, on the footprint convex hull,
  facing the street or turned along it); what stays empty is kept from the yards (used 8), so yard paint no longer
  covers missing buildings. Karlsruhe Marktplatz: 579 buildings instead of 479 (the town hall was left out). Yards are
  paved with one paving (stones, 2) instead of the templates mix. REGS_DEBUG_OSM_PAINT=x,z,r logs the paint areas and
  the buildings left out around a point.
  Grass slivers between paved areas close (a plain ground cell with paving within 2 cells on two opposite sides takes
  their paving), after the OSM paint and again after the yards' paving (Painter::closeGaps).
  Pockets (owner 2026-11-15: random grass patches in weird shapes; Painter::fillPockets): a 4-connected region of plain
  ground (painted areas' border cells and uncovered street margins included) up to 900 cells, not at the map's edge,
  whose border is three quarters paving, buildings or streets and under a tenth OSM green (parks, gardens, lawns,
  meadows, cemeteries, playgrounds, pitches, woods: ImportData::green), is paved with its neighbours' paving.
  Karlsruhe Marktplatz: 947 pockets, 9200 cells. REGS_DEBUG_OSM_POCKETS / REGS_DEBUG_OSM_CELLS log why one stays.
  Procedural buildings (owner 2026-11-15: custom shaped buildings with windows, doors and roofs, big halls;
  engine/proc_buildings.*): a footprint from 300 cells (REGS_OSM_PROC_MIN) on free ground becomes a building of the
  port's own: walls raised from the outline (a relation's convex hull), facades repeating per floor and bay (halls:
  corrugated sheets with a window band and roller doors; houses: plaster or brick with framed windows, front doors with
  steps), doors on the walls facing the nearest street, a flat roof with a parapet and air conditioning boxes (halls:
  metal roof with skylight strips) or a tiled gabled roof on a four-cornered house up to 15 cells wide. The textures
  are drawn by the port (no game files). Stored as GridData::ProcBuilding in map.grid section 0x07F30000 and as the
  timeline action 0x070810E0 (undo, the day switch; a replay skips a building the map has). Karlsruhe Marktplatz: 10
  (the town hall in plaster, three halls), Bruchsal 6. Since 2026-11-15 (owner: more interesting roofs, windows and doors
  with depth that light up by night): windows and doors sit in recesses (reveals, mullions, sills, a step, a lamp and a
  lit fanlight by a front door, roller doors with a lamp on halls); two thirds of the panes light up warm at dusk in three
  groups one after another (emission from night01, night_lights.h vehicleNight01). Flat roofs: gravel with tar seams,
  drains and moss, glazed skylights (lit at night), solar panels in rows tilted south, a stair house with a door, air
  conditioners with a fan grille, vent pipes; halls: glazed roof lanterns with glazing bars; gabled roofs: framed roof
  windows, brick chimneys. Types and variety (owner 2026-11-15: warehouses only in industrial areas, offices and modern
  shops in town, different facades): halls only for industry, farms and railways; workplaces become office blocks
  (spandrels with ribbon windows or cladding with single windows, a glazed lobby), commercial buildings shops (lit shop
  windows and a coloured fascia sign below; a low one from 700 cells a big store with a glass entrance and its sign), a
  third of the big town houses a business house with shops below, a third of those from four levels an office. Houses:
  4 plaster tints, 3 brick colours, cross / tall / wide / paired windows, cornices, white or dark frames. Solar panels on
  45 % of flat roofs, half the halls, 40 % of gabled roofs (south slope); roof textures and panel rows follow the
  building; the walls reach 12 units into the ground (no floating wing on a slope). Not yet: shadows, picking and the bulldozer, jobs
  and homes in the simulation; a reload of a saved map with them not yet seen (the section is written: checked in the
  autosave).
  Railings by the water (owner 2026-11-15; osm.cpp riverRailings): the game's street railing every 6 units at the outer
  edge of a street's or footpath's pavement where water lies within 4.5 units beyond it (not on bridges or in junctions).
  Since then: the garden fence (a low wall with stone pillars, the owner's pick; two cells a piece, joined; the fence
  models run along their model z, the street railings along x, so fences are turned by armFront), also where the ground beside the road lies 1.5
  units and more under it (cliffs); REGS_OSM_RAILING picks another template. Streets by the water no longer dip: the
  bank lowering stops at streets and paved areas (a quay wall), and a street point near water takes the elevation
  data's height of the dry cells around it.
  Footpaths lie 0.12 under their level with a polygon offset of -3 (0.2 down clipped into unlevelled ground).
  2026-11-16 (owner's requests): paved areas (car parks, squares, market places, service areas) get rounded corners
  (roundCorners, 4 m) and keep their cut border cells: the grass-sliver closing (terrain stage and Painter::closeGaps)
  only fills gaps between two different areas (Apply::paintOwner), and street margins the street covers only partly
  count as ground. Car parks: a concrete driveway to the nearest street when they do not reach it, and the parking sign
  at the pavement beside it (parkingAccess). Fences: OSM's barrier=fence/wall/hedge (fence_type picks wooden, chain
  link, iron, split rail; walls brick, hedges the hedge; short ones first, 12000 pieces at most) and fences between the
  yards of neighbouring row houses (two joins in three, one kind a row) — placeFences / fenceLine (the fence models run
  along their model z at their cell's −x edge). Junctions (junctionSigns): traffic lights where OSM maps
  traffic_signals or two large roads cross; stop / give-way from OSM, give-way on small streets meeting large roads;
  direction signs before large junctions; old signposts at footpath junctions in parks. Sign models seen in the game
  (REGS_OSM_SIGNTEST=x,z lays them out): sign01 give way, 08 stop, 07 limit 100, 04 no stopping, 05 motorway;
  traffic-lights02's arm reaches along its front, so only traffic-lights01 is used. OSM points: town signs
  (traffic_sign=city_limit), advertising columns, bollards. Advertising (placeAds): posters and wall ads on the side
  walls of houses by gaps along the streets in town, billboards along large roads, advertising columns at junction
  corners. Standalone McDonald's / Burger King / KFC (brand, wikidata or name; a building or a point in it, at most
  three times the restaurant's size) become the game's fast food restaurant. The import holds the simulation's
  rebuilds back until it ends (gridEditHoldSimulation): on 2048 × 2048 each slice's rebuild took up to 80 s.
  2048 × 2048 (Bruchsal) now completes: 185,580 objects, 7,733 buildings, 134,799 trees; the timeline gets the objects
  in one insert (timelineRecordCreateMany; one by one took hours), the simulation's one rebuild at the end takes
  ~130 s (the game stands still meanwhile). Rendering such a map is slow (well under 10 fps seen, with a second window).
  Working traffic lights (engine/traffic_lights.cpp): traffic-lights01 near side at the stop line on the right-hand
  pavement (the zebra lies 0.17-2.88 inside the mouth), lenses on its model +x facing the oncoming traffic; lamp
  positions from the texture's red / amber / green texels on the lens side; a 40 s cycle (green 16, amber 3, red 20,
  red-amber 1), the cross direction half a cycle later, green waves along each junction's main axis at 12 units/s;
  glow sprites by day and night, at night a small pool on the street via night_lights' extraNightLights (the light map
  is monochrome: warm for red/amber). Decoration only: vehicles do not stop. One-way signs (traffic-sign09, the blue
  arrow panel, found by its UVs) at the start of one-way streets facing the junction. Restaurant tables on the
  pavement where the house stands at it. In Europe none of the Tokyo / US packages' own buildings (foreignToEurope);
  open land behind houses gets trees and bushes only, no garden things.
  Open (seen in the 2026-11-16 screenshots): the Schloss became rows of terraced houses with blank gable walls; the
  Schlossgarten's basins are dry rock pits ringed by hedges; OSM fence pieces stand alone on lawns where what they
  enclosed is missing; the Siemens roundabout not checked (the geocoder found another stretch of the street).
  Screenshot runs: the scratchpad place.ps1 imports a place, copies its autosave to maps/a1 and starts it as a sandbox
  (Neues Spiel → Sandbox → the map → Start) by day, at night (REGS_DEBUG_TIME=22), in rain (REGS_TEST_WEATHER=heavy,
  REGS_TEST_GROUND=puddles) and snow (=snow, =snow, REGS_TEST_SEASON=350); the weather mod must be enabled in the
  stand-in documents' mods/mods.json.
  2026-11-16 later (owner): procedural palaces (style 5), castles (6) and churches (7) in engine/proc_buildings.cpp.
  Palaces: building=palace, historic=castle with castle_type palace/stately/manor, "Schloss …" names, buildings in a
  historic=castle area, and wings touching a palace (up to three steps); rusticated ground floor, tall windows with
  sandstone surrounds and lintels, pilasters, string course and cornice, a slate mansard (the outline inset 3.4/2.6/1.8,
  insetPolygon) with dormers, a lantern with a copper dome over the top's widest point on big ones. Castles
  (building=castle, defensive castle types, "Burg …"): rubble stone, few deep windows, battlements, round towers with
  conical roofs (slate or red tiles) at up to six outer corners. Churches (building=church/cathedral/chapel/basilica or a
  christian place of worship): type from building:architecture, else start_date, else gothic for cathedrals and by id;
  the main axis from the outline, the tower at the wider end (apse narrower) with the portal; romanesque pyramid roof,
  gothic octagonal spire and buttresses (twin towers on big ones), baroque onion dome, modern free-standing bell tower;
  chapels a ridge turret; stained glass glows at night. Photo textures (CC0, Poly Haven) in data/regs/textures/proc
  (tools/make-proc-textures.mjs, CREDITS.txt), loaded by collectProcBuildings over the drawn stand-ins; the red roof tile
  photo also replaces the houses' drawn tiles. Small still water (ponds, basins, fountains under 250 / 900 cells) whose
  shore lies over 2 m above the lowest land became pits down to the map's one water level: now paving with a fountain.
  Offices: a third kind, curtain walls of reflecting glass (kProcCurtain: a painted sky and street, strong specular);
  entrances with a canopy on two columns, some with a revolving door. Shops: striped awnings (red / green) over the shop
  windows on two in three.
  Not checked yet: a castle and the gothic / baroque / modern churches in a screenshot (Bruchsal: palace, wing and the
  romanesque Hofkirche checked).
  2026-11-16 evening (owner): passages through procedural buildings (ProcBuilding::passages, bit 7 of the record's roof
  byte): where a road or footpath crosses a footprint (in through one wall, out through another or into a junction
  inside; its middle at least 0.6 of its half width from the walls; not one that ends inside, a path to a door) the
  osm import cuts it into straight pieces (cells under them neither block nor are taken); the mesh is clipped against
  each piece's box (anything under y1: every style's walls, windows, buttresses, towers) and lined with side walls and
  a ceiling. Clear 3.2 over footpaths, 4.5 over roads, 1.2 under the eaves at least. Castle and church pieces from 8
  cells (gatehouses, small wings: Cochem's gate passage). Castle battlements finer: a 0.3 parapet standing out on
  corbels, merlons 0.6 wide with caps every 1.5. Statues sized (GridData::Item::scale, drawn only; saved in the item
  turn section, bit 30, and the timeline's turn record): OSM's height where given (4 of 414 cached statues), else a
  usual height by kind (figure 3.8 m, bust 2.4, obelisk / stele 4.5, stone 1.5, sculpture 3, monument 5.5): the game's
  statues are 11–12 m at the import's scale. Open: passages are boxes (no arch), and only procedural buildings get
  them (row houses of game models leave the way's cells free instead).
  Later that evening: passages arched; castles in greyed Tower of London rubble (London package), real deep windows,
  stone slab wall walks; the near plane follows the camera's distance (engine.script's fixed 30 cut close models).
  The map data: the whole area downloaded once and kept by the area (a changed query needs no server;
  REGS_OSM_KEEP_CACHE keeps it for ever), servers overpass-api.de, overpass.openstreetmap.fr, overpass.kumi.systems,
  maps.mail.ru, private.coffee. People (owner: "an invisible pavilion" that draws people, at sights and in buildings
  by size): game items with GridData kItemPortHidden (0x80000 in Item::flags: not drawn, picked, on the minimap, no
  ground work or conflicts; the simulation counts their places): sculpture01 (30 leisure, no cover) at OSM sights
  (tourism museum/attraction/viewpoint/gallery/zoo/theme_park/aquarium, historic castle/ruins/...; weight 1-3 plus
  size), inside procedural buildings by floor area (castles/palaces/churches sculptures, shops market stands, offices
  office blocks, houses apartment houses; sculptures one per 900 m² of footprint, at most 5, a sight inside at most 3 more).
  Checked in a sandbox game (REGS_DEBUG_VISITS logs the pools and trips every 30 grid-seconds): on the Bruchsal palace
  map 18 sculptures hold 29 % of the leisure places and about 14 % of the visiting citizens head there; 240 live in
  carried homes. The grid file keeps the bit (it dropped it at first). The timeline does not (undo, redo show them).
  Start flags: --osm <place or lat,lon> [--osm-size n] [--osm-scale f] [--osm-name name] [--osm-options list]
  [--osm-save] (REGS_START=osm:..., main.script extension; a taken name gets -2, -3: the empty map would open it).
  Smooth shorelines (owner 2026-11-15: the Saalbach's edges looked jagged; ImportData::shoreSd / waterDraw): per corner the
  signed distance to the true water shape (river lines with their width, water areas by scanline crossings, the sea by
  the coast); the corners of water cells take their height from it (the water line −0x140 on the true shore, deep at
  once inside, a little above it outside), so the terrain cuts the water plane along the true shore inside the cells
  instead of along their staircase; cells the true shape covers well are drawn as water too (planning keeps the old
  mask). Open: steep channel walls show the cliff texture.
  Statues (owner 2026-11-15: many wrongly placed): only memorials and artworks of a figure type become statues (statue,
  bust, obelisk / stele / war memorial, sculpture, stone, each its own model; historic=monument a rider statue);
  Stolpersteine, plaques, ghost bikes, murals, graffiti and untyped ones are left out (Karlsruhe: about 100 of them had
  become statues). A statue stands on free ground or paving at its point or up to 3 cells off, never on a street's
  pavement, facing the nearest street.
  Open: tram stops on beds
  (the stop tool takes curves with street tracks only); tram bridges (a bed on a bridge has no deck yet); a tram branch
  off a bed (a merged track's kept stretch overlaps the bed instead of a switch).
- [ ] Open: express interchanges (bridges and tunnels: done 2026-11-08) (rename of curved roads: done 2026-11-14) (curves:
  tunnels, abutments, map-edge ends, express ramps with entries and exits: done 2026-11-15);
  `removeRedundantActions` (time bar button) is a no-op; "Speichern & Spielen" not tried yet.

## 9. Release (OpenRCT2 style: requires the original game)

- [ ] Find the Steam install by itself (registry, `libraryfolders.vdf`) and check for the needed archives; a clear
  message if the game is missing.
- [x] Where saves and settings live (user decision 2026-11-06): in OpenCiM's own folder `Documents\OpenCiM`
  (settings, controls, `state.gs`, saves, maps, log); the first start uses the game's defaults. The original's
  `Documents\Cities In Motion` is only read: its saves and maps are listed with "(aus Cities in Motion)" and copied
  into OpenCiM when opened (name clash: an identical copy is reused, otherwise `<name> (CiM)`). `--documents` is the
  stand-in root for tests. README "Where data lives", `engine/user_folders.cpp`. Open: copying the original's
  settings on the first start (an option, not done), Steam Cloud saves (set aside).
- [x] Display settings at run time: every option of the graphics page applies on OK without a restart (shadows,
  HDR rendering, saturation, antialiasing, anisotropy, vsync, fullscreen, resolution, UI scale; frequency and colour
  depth change nothing with borderless fullscreen). New window sizes lay the GUI out again and reopen the page
  (`docs/ui/scaling.md` §6, `engine/gui_relayout.cpp`). Open: the debug timebar's year/month/day bars (editor, only
  with `$debug`) keep their proportions only approximately; Alt-Tab behaviour of fullscreen.
- [ ] Crash reports, a release build and package without any game files, licence and readme. Installer design (owner's
  request 2026-11-17): `docs/installer.md` (own setup program in the game's menu style, art read from the install, a
  feature showcase while installing); mockup on the canvas "OpenCiM Installer". Built 2026-11-19: `opencim-setup.exe`
  and `make-release.ps1` (offline installer, zip, sha256; state and what is left in `docs/installer.md` §7). Left:
  crash reports, the port's own licence, signing, the GitHub releases and download.
- [ ] Performance on the largest maps at speed 10 with all systems running; loading time.

## 10. Parity pass (milestone 8)

- [ ] Campaigns from start to finish, the original's saves loading in the port, a regression set of scripted runs
  (`--at`) with reference screenshots.
