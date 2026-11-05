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
- [ ] Lay roads, alleys, bridges and tunnels (editor only; `createRoadBlock` is a plain stand-in), the double track's
  road crossings.
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
- [ ] Terraforming and paint tools (shift, level, soften, pavement, fields, variation, rename), also used by the map
  editor.
- [ ] Undo (`addUndoItem`/`executeUndoItem`).
- [x] Live map updates after an edit (`engine/grid_edit.h`, used by the city timeline): items added and removed in
  batches, the item and terrain meshes of the touched divisions, cell cover (`fixGrid`) and ground levelling under
  LOCK_GROUND items, the lane graph and walk map (simulation hooks), picking, the minimap raster. Still open: the
  minimap's transport marks (`docs/ui/minimap.md` §2.4), the full `checkConflict` and
  `removeOverlappingOptionalObjects` rules (simplified today), undo.

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
  the live map. Still open: brush falloff and water edges checked against the original, the editor's timeline UI.
- [ ] New maps from the editor and the scenario start dates (the sandbox start year now comes from the map).

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
- [ ] Jams the relief does not cover: metro trains of Berlin's lines 0 and 4 wait for each other for 2 to 10 minutes
  where the lines share track (they always got going again, §10.4); trams wait 2 to 10 minutes at busy junctions
  while one car after another holds the crossing nodes (why the cars do not give way to the tram's waiting marker is
  not traced); the relief's kept node only helps once the tram is detected.
- [ ] Pedestrian walks without a path: Vienna 0.5 % (22 of 4555 paths) since the walk and height maps are turned as
  the engine turns them (half a turn off the mesh, `docs/simulation/pathfinding.md` §1.4; it was 10.7 % with the
  turn of the mesh). That also puts the waiters of underground and elevated metro stations on their platforms (they
  stood at street level), reached by the stations' stairs. Cim2 not re-measured since (was 1.7 %). Left from before:
  the sunken park's stairs prop (no walk map, blocks its cell), a hotel enclosed by bushes.
- [ ] Metro trains at the turn of a line (end stations) stop with their front at the platform instead of their
  middle (`docs/simulation/transport.md` §4.4, port note); all other halts are centred since the leg after a turn
  pulls its half length too.
- [ ] Deviations to revisit: water cells are never walkable (the original treats them as open ground); boats keep
  running across a save (the original removes them).
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
  (annuity 223,70 and the monthly balance to the cent), Vienna and Cim2 sandbox saves. Fixed on the way:
  `gsGridCitizens.getCharacter` and `clearGameData` (were stubs). Script quirk kept as in the original:
  `monthlyProfit` uses the previous month's fares (§2 of the spec).
- [ ] Still to run: the tutorial past its first task, scenario 2 and later scenarios, a whole campaign; the
  demonstrations and parades (`gsGridItemController.activate`/`deactivate` are stubs, the ticker shows but the
  crowd items do not appear); `setClusterData` (scenario 4).
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
- [ ] Boat wakes (`gsBoatContrain`: a ribbon mesh per boat in the grid bucket, docs/renderer/particles.md §7) and
  fires as a simulation event (building pass, `docs/simulation/grid-traffic.md` §4.4).
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

- [ ] The editor flows (Karteneditor menu) on top of the construction tools: new map, terrain, water, objects, timeline,
  saving `userdata/maps` with the minimap thumbnail.

## 9. Release (OpenRCT2 style: requires the original game)

- [ ] Find the Steam install by itself (registry, `libraryfolders.vdf`) and check for the needed archives; a clear
  message if the game is missing.
- [ ] Decide where saves and settings live for players (the real `Documents\Cities In Motion`, shared with the
  original, or a ReGS folder) — today a stand-in folder is used on purpose.
- [x] Display settings at run time: every option of the graphics page applies on OK without a restart (shadows,
  HDR rendering, saturation, antialiasing, anisotropy, vsync, fullscreen, resolution, UI scale; frequency and colour
  depth change nothing with borderless fullscreen). New window sizes lay the GUI out again and reopen the page
  (`docs/ui/scaling.md` §6, `engine/gui_relayout.cpp`). Open: the debug timebar's year/month/day bars (editor, only
  with `$debug`) keep their proportions only approximately; Alt-Tab behaviour of fullscreen.
- [ ] Crash reports, a release build and package without any game files, licence and readme.
- [ ] Performance on the largest maps at speed 10 with all systems running; loading time.

## 10. Parity pass (milestone 8)

- [ ] Campaigns from start to finish, the original's saves loading in the port, a regression set of scripted runs
  (`--at`) with reference screenshots.
