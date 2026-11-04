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
  road crossings, the underground view (`GRID_VIEW_UNDERGROUND`) to see subway tracks.
- [x] Place stops and stations: bus and tram stops, metro stations and transitions, docks, heliports
  (`GRIDMODE_CREATE_STOP`, `selectStation`; `engine/tool_objects.cpp`, construction.md §11-12). Still to see on
  screen: heliport, elevated and underground stations, the metro transition. Port QoL: a stop upgrade costs the new
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
  reload (Vienna, Berlin bus and tram lines checked). Route strips on the map (`engine/sim_paths.cpp`), stop bubbles
  (`transportStopCreated`). Still open: boat strips smoothed as the original, the strips' coverage counters,
  `gsGridItemController.setColor` (stops in the company colour), the bubbles' overlap layout.
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
- [ ] Pedestrian walks without a path: Cim2 1.7 %, Vienna 6.2 % (was 3.4 % and 9.3 %); all remaining failures are
  real disconnections. Left: the sunken park's stairs prop (no walk map, blocks its cell), a hotel enclosed by
  bushes, goals between Vienna's elevated station pillars (needs the original's door check: a door must have a way
  out).
- [ ] Deviations to revisit: water cells are never walkable (the original treats them as open ground); boats keep
  running across a save (the original removes them).
- [x] Saves keep the live simulation, including boats, aircraft, animals and the rest of each pedestrian's walk (the
  original's direction codes, also read from the original's saves: 99 % of Cim2's walks continue after loading).
  Round trips on Cim2, Vienna and Berlin reload matching counts.
- [ ] Saves: try loading a port save in the original game. Seated state and the vehicles' transient fields are written
  as 0 (the original's reset value) because the port doesn't model them.

## 5. Economy, scenarios and campaign

- [ ] Run each campaign scenario and the sandbox for a few game years and compare with the original: monthly costs,
  maintenance, loans, ticket prices, company value, reputation, the objectives and their completion, trophies
  (`docs/ui/game-load.md`, the scenario scripts' `checkAchievements` threads).
- [ ] Statistics and graphs (`gsGraphCtrl.update`, `reset`, `setColor` are missing) and the news ticker.

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

- [ ] Particles (`gsParticleEmitter`: smoke, fires, fountains, sparks; `addColorRange`, `setTexture`,
  `setParticleScale`).
- [ ] Sky mesh (`gsSky.generateMesh`), night lighting with the illumination maps, `gsTracerCtrl.enableFade`.
- [x] Door items: level-crossing barriers lower while a train holds the crossing's track nodes and rise after it,
  cars wait before the crossing (`engine/sim_doors.cpp`, `docs/simulation/grid-traffic.md` §5.1.1;
  `port/run/barriers/sheet-f-rise.png`, `sheet-f-wait.png`).
- [ ] Map-edge base walls and the water-edge curtain, scene nodes attached to bones, the stop catchment
  circle, the tracer organizer layout for overlapping pins.

## 8. Map editor

- [ ] The editor flows (Karteneditor menu) on top of the construction tools: new map, terrain, water, objects, timeline,
  saving `userdata/maps` with the minimap thumbnail.

## 9. Release (OpenRCT2 style: requires the original game)

- [ ] Find the Steam install by itself (registry, `libraryfolders.vdf`) and check for the needed archives; a clear
  message if the game is missing.
- [ ] Decide where saves and settings live for players (the real `Documents\Cities In Motion`, shared with the
  original, or a ReGS folder) — today a stand-in folder is used on purpose.
- [ ] Display settings at run time (resolution, fullscreen, Alt-Tab), crash reports, a release build and package
  without any game files, licence and readme.
- [ ] Performance on the largest maps at speed 10 with all systems running; loading time.

## 10. Parity pass (milestone 8)

- [ ] Campaigns from start to finish, saves exchanged between both engines, a regression set of scripted runs
  (`--at`) with reference screenshots.
