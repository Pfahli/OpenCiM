# ReGS: a source port of the Cities in Motion engine

ReGS reimplements `GSystem.dll`, the engine of Cities in Motion (2011), in portable C++20. It runs the game's own
scripts and data from an **installed copy**; nothing from the game is part of this tree, and nothing is copied out of
the install. The reverse-engineered specifications it follows are in [`../docs/`](../docs/REPORT.md).

The goal is a playable build first, then the improvements the original engine can't take: a modern renderer,
native ARM64 builds, better performance, quality-of-life features, a mod loader and multiplayer.

## Build (Windows)

Needs the Visual Studio 2022 Build Tools with the C++ workload (MSVC, CMake and Ninja are taken from there).
There are no third-party dependencies yet.

```powershell
.\build.ps1                 # Release x64 into build\x64-Release
.\build.ps1 -Config Debug   # or RelWithDebInfo; -Arch x86; -Clean
.\build.ps1 -Name gfx       # a separate tree build\x64-Release-gfx, for parallel work on the same sources
```

## Run the game

```powershell
$game = "C:\Program Files (x86)\Steam\steamapps\common\Cities In Motion"
.\build\x64-Release\regs.exe $game --windowed
```

### Where data lives

OpenCiM keeps all player data in its own folder, `Documents\OpenCiM`: `settings.script`, `controls.keys`, the game
state `state.gs` (trophies, unlocked scenarios), `saves\*.gs`, user maps `maps\<name>.{grid,timeline,dds,scenario}`
and the game's log `log_metro.txt`. The Documents folder is found as the original finds it (`SHGetSpecialFolderPathW`,
`engine/platform_win32.cpp`). The game's `metro/config.script` asks for `<documents>\Cities In Motion`; the port's
`init_userdata` answers with `<documents>\OpenCiM` instead (`engine/user_folders.cpp`), so the scripts are used as
installed. On the first start the folder is created and filled with the game's defaults (`metro/defaults`, as the
original does for an empty folder: language choice, desktop resolution); the original's settings are not copied.

The original game's folder `Documents\Cities In Motion` is only read, never written, renamed or deleted. Its saves
(`saves\*.gs`) and maps (`maps\*.grid` and their files) appear in the load lists (Spiel laden in the main and pause
menu, the sandbox map list, the map editor's load list) with the note "(aus Cities in Motion)" / "(from Cities in
Motion)". Opening one copies it into the OpenCiM folder and loads the copy. If the name is taken there by different
content, the copy is named `<name> (CiM)` (then `<name> (CiM 2)`, …; a renamed save's own paths in its
`level.scenario` are rewritten to the new name); nothing in OpenCiM is overwritten. Originals that already have a copy
in OpenCiM (same size and content hash, or an unchanged copy recorded in `OpenCiM\regs-imports.txt`) are listed only
once, as the OpenCiM file. Disk changes by scripts (`write`, `delete_file`, `mkdir`, `fappend`, `freset`) are refused
outside the OpenCiM folder.

`--documents <dir>` stands in for the Documents folder (tests): the same layout lives under it, `<dir>\OpenCiM` and
`<dir>\Cities In Motion`, so test runs never touch the real folders. Without it the real Documents folder is used.
`--print-paths` prints the resolved folders and quits without creating or reading anything. Stand-in folders made
before this change keep their settings in `<dir>\Cities In Motion`; copy `settings.script` (and `controls.keys`) into
`<dir>\OpenCiM` to keep using them, or the first start shows the language choice. The port loads the game and its DLC from the install's own
archives only: community addons and loose override folders in the install (`addons\`, `metro\`) and in
`userdata\addons` stay off, so mods for the original game cannot change it; `--community-addons` mounts them as the
original does (for comparisons). `--frames <n>` quits after n frames and `--screenshot <file.png>`
saves the last one (used for checks). `--at <frame>:<action>` injects input for screenshot checks: `move:x,y`,
`click:left|right|middle`, `down:…`/`up:…`, `key:<VK>` (press and release), `keydown:<VK>`/`keyup:<VK>` (hold), `text:<chars>` (typed into edit controls),
`shot:<file.png>`, `vsync:on|off` (e.g. off after a frame-timed load, for frame-time measurements), `wheel:<notches>`
(mouse wheel at the mouse position, negative zooms out), `cam:x,z` (the game camera over a world point through
`MAIN.$gameUI.setCameraPos`; clamped to the map, so `cam:0,99999` shows the south edge). Loading the save
`Cim2` and entering the game, for example:

```powershell
.\build\x64-Release\regs.exe $game --windowed --frames 3000 --at 800:move:160,348 --at 820:click:left `
  --at 900:move:760,371 --at 910:click:left --at 950:move:630,566 --at 960:click:left `
  --at 2500:move:1490,844 --at 2510:click:left --at 2600:keydown:68 --at 2700:keyup:68 --screenshot map.png
```

Debugging switches (environment variables): `REGS_NO_COMBINER=1` (built-in shader only), `REGS_DEBUG_SHADE=1..4`
(fallback shader views), `REGS_DUMP_TARGET=<file.png>` (render-target contents), `REGS_DEBUG_VIEWPORT=1` (viewport
scene/camera and large GUI images, once each), `REGS_DEBUG_MATERIALS=1` (failed `getMaterial` lookups), `REGS_MINIMAP_CHECK=amsterdam,tutorialcity` (renders
shipped maps with the loaded templates and compares them with the editor's saved minimaps, `metro/maps/<map>.dds`),
`REGS_DEBUG_PICK=1` (logs each newly hovered item or vehicle), `REGS_DEBUG_TOOLS=1` (pieces and time of each rebuilt track preview; `=2` lists the pieces), `REGS_DEBUG_HALT=1` (where each line vehicle stands at a halt: front, rear and middle along the platform node; how many nodes it pulled past the platform), `REGS_REPORT_MISSING=1` (lists the unimplemented natives
the scripts called, at exit), `REGS_DEBUG_GROUPS=1` (the script thread groups' pause count, speed and time every 300
frames), `REGS_DEBUG_SAVE_AT=<ticks>:<name>` (saves the game after that many simulation ticks, for round trips),
`REGS_DEBUG_FOLLOW=<line type>[:n]` (the main camera follows the n-th line vehicle of that transport type, given as
a number: 0 bus, 1 tram, 2 metro, 3 boat, 4 helicopter; a screenshot burst `follow-turn-<n>.png` when it turns round,
`follow-bustram-<n>.png` when a followed bus halts at a tram stop), `REGS_DEBUG_CAMERA=<item>` (puts the camera on that
map item once, 30 ticks into the game; every line vehicle halting at that stop item films a burst
`halt-<line type>-<k>-<n>.png`), `$regsBusAtTramStops = false;` in `settings.script` (bus lines take no tram stops),
`REGS_TEST_CRASH=<n>` (drivers near the camera turn reckless until n crashes happened), `REGS_NO_JAM_RELIEF=1`
(turns the port's jam relief for buses and trams off, as in the original; detections are still logged, with how each
jam ended by itself; `docs/simulation/transport.md` §10), `REGS_TEST_JAM=<line type>[:hard][:save]` (parks a car
across the way of the followed bus `0` or tram `1`; `:hard` pins it so the relief must respawn the vehicle, `:save`
saves as `jamsave` in the middle of the relief; screenshots `jam-<event>-<n>.png` per relief step and of the ticker
after the respawn), `REGS_DEBUG_TIME=<hour>`
(starts the game at that time of day, e.g. `20.5`, with the day/night light model; it stays there unless the
day/night option is on, then it runs from there), `REGS_DEBUG_SKY=1` (logs the day/night time, sun elevation and
weights every quarter hour of the cycle), `REGS_DEBUG_AUDIO=1` (logs every
sound load, play, stop, pause and resume with path, mode, volume, pitch, position, distance and range, the world
sounds of vehicles and items, `getAreaType` votes, each play's volume group, and once per second the voice count, the
RMS per output channel and per volume group, and the four loudest 3D voices with their listener-space position and
channel gains), `REGS_AUDIO_CAPTURE=<file.wav>`
(also writes the final mix to a 16-bit WAV with the output's channel count), `REGS_AUDIO_ONLY=<text>` (only sounds
whose file name contains the text are heard, e.g. `ngine` or `Church bells`, so a capture measures them alone). Test
saves with every transport type: `node tools/gstool.mjs pack samples/saves/vienna "<documents>/OpenCiM/saves/vienna.gs"`
(keep the folder name: the save's `level.scenario` points at it), `REGS_DEBUG_SIM=1` (logs the stop cycle, which tram
stops bus lines may take, replays the saved traffic routes, and logs each level crossing near the camera as its
barriers go down or up, with the trains holding it and the cars waiting), `REGS_NO_SIM=1` (simulation off).
Particles (`docs/renderer/particles.md` §6): `REGS_NO_PARTICLES=1` (none emitted or drawn), `REGS_DEBUG_PARTICLES=1`
(items with emitters, live/drawn counts and CPU times; `=2` draws them opaque magenta), `REGS_TEST_BREAKDOWN=<s>` (the
followed vehicle breaks down after that much simulated time), `REGS_TEST_FIRE=<item>` (a fire at that item),
`REGS_TEST_FIREWORKS=1` (the new-year fireworks bit stays on). Runs with `--at` actions ignore the real mouse, keyboard and
window focus, so scripted clicks land the same with or without focus.

### Port files and graphics options

The port needs an installed copy of the game and ships none of its files, like OpenRCT2. Its own files live in
`data/` (copied next to `regs.exe` by the build; `--data <dir>` points elsewhere) and are mounted over the install:

* `data/regs/extend/<game path>` **extends** a game script without replacing it: the part before a line `//@base`
  is compiled before the installed file and the rest after it, in the same script object; `//@rename f g` renames a
  function of the original so the extension can wrap it (`VM::extendSource`). This is also the planned basis for mods.
* `data/regs/strings/<language>.strings` (`en_US` is the fallback), `data/regs/shaders/` (the port's post effects).

The game's graphics page is extended this way (`data/regs/extend/metro/ui/menus/graphics.script`):

| Option | Setting | Values | Applies |
|---|---|---|---|
| Graphics preset | (derived) | Low, Medium, Original (the game's defaults), High, Ultra; "Custom" otherwise | sets the options below |
| Shadows | `$shadows` | off, 1024², 2048² (the game's maximum), 4096², 8192² | on OK (the game's engine.script makes a new shadow effect; the original asked for a restart in game) |
| Antialiasing | `$antialiasing` | off, 2×, 4×, 8× MSAA (the window has no samples itself: views draw into multisampled targets) | on OK (the original: after a restart) |
| Anisotropic filtering | `$anisotropy` | off … 16× | on OK, every loaded texture (the original: after a restart) |
| View distance | `$regsViewDistance` | 100–300 % of the far plane and the map's cull distance (shadows keep the game's range) | at once |
| Detail distance | `$regsDetailDistance` | 100–300 % of the near-LOD band | at once |
| Frame rate limit | `$fpsLimit` | off, 30–144 (the game read it but never offered it) | at once |
| Colour style | `$regsColorStyle` | Original, Vibrant, Warm, Cool, Cinematic, Monochrome (`grade.fragment`) | at once |
| Lens blur | `$regsLensBlur` | off; light / strong by distance from the camera target (thin lens on the depth buffer, `dof.fragment`: zoomed in, the distance blurs); tilt-shift (`tiltshift.fragment`) | at once |
| UI scale | `$regsUiScale` | 1× (default), 1.5×, 2×: menus, HUD, panels, text, tooltips, pins and the cursor grow; the map, the corner preview, the shop's 3D previews and the minimap's map keep the window's full resolution. Text and images drawn 1:1 by the scripts stay sharp (exact 2×2 texels at 2×, near-sharp at 1.5×). Unset: 2× on windows taller than 1800 px. A scale needs a window of at least 1024 × 720 × the scale, else the next smaller one is used. How it works: [docs/ui/scaling.md](../docs/ui/scaling.md); `REGS_UI_FILTER=linear` compares with plain bilinear | on OK: the GUI is laid out again for the new size and the page opens again (scaling.md §6) |
| Graphics engine | `$regsRenderer` | OpenGL 3.3; Vulkan listed as planned (raytracing, DLSS) | — |
| Performance readout | `$regsShowFps` | off (default) / on: FPS and frame time averaged over 0.5 s, min/max FPS over the last second, drawn by the engine over the GUI at the top edge right of the toolbar (`Display::drawFpsReadout`); never in headless runs, nothing measured while off. The game's own "show frame rate" (`$fps`, a script overlay in the top right corner) stays as it was | at once |
| Day and night | `$regsDayNight`, `$regsDayLength` | off (default: the location's fixed time of day, as in the original) / on: the time of day runs with the game, `$regsDayLength` real minutes per day at normal speed (default 12, settings.script only), with dusk, a moonlit night and brighter self-illumination at night (`docs/renderer/sky-and-night.md` §6.2) | at once |

The game's own options on that page all apply on OK too; the original showed its "restart needed" notice for every
one of them except the cursor, vsync and the frame-rate display, and the port no longer does:

| Option | Setting | Applies in the port |
|---|---|---|
| HDR rendering (bloom) | `$bloom` | on OK: engine.script's extension makes or removes the bloom effect (`regsApplyEffects`) |
| Saturation | `$saturation` | on OK (bloom's saturation; only with HDR rendering, as in the original) |
| Vsync | `$vsync` | on OK (the original set `window.vsync`; the port now applies the field every frame) |
| Fullscreen | `$fullscreen` | on OK: the same window becomes borderless over the monitor or a captioned window again (`--windowed` only holds at start) |
| Resolution | `$screenwidth`, `$screenheight` | on OK: the window's size when not fullscreen |
| Colour depth, frequency | `$screencolors`, `$frequency` | stored; nothing to change (fullscreen keeps the desktop's display mode) |

When fullscreen, resolution or UI scale give the scripts another window size, the engine lays the GUI out again
(each control keeps its edge, centre or span; the line panel, level bar, money bar and petition bar re-place
themselves with their own formulas), the open main or pause menu is loaded again and the graphics page opens again;
the game, its camera and open panels stay (`docs/ui/scaling.md` §6). The GL context is never recreated, so textures,
fonts, meshes, the minimap and the 3D previews stay; the effect and preview targets are made again at the new size.

The audio page (`data/regs/extend/metro/ui/menus/audio.script`) gets the port's volume groups under the original's
audio (master) and music volumes; the master volume stays on top of all of them (`docs/audio/README.md` §18.7):

| Option | Setting | Values | Applies |
|---|---|---|---|
| Effects volume | `$regsEffectsVolume` | 0–100 % (default 100 %): the world: vehicles, horns, bells, doors, stop announcements, buildings' and items' loops, animals, passenger fares | on OK, like the original's sliders |
| Ambience volume | `$regsAmbienceVolume` | 0–100 %: the city and area ambience loops | on OK |
| Interface volume | `$regsInterfaceVolume` | 0–100 %: UI clicks and hovers, errors, construction and tool sounds, purchases, achievements, news, scenario jingles, query sounds | on OK |
| Menu music follows the music settings | `$regsMenuMusicFollowsMusicVolume` | off (default, as the original: menu and ending music at full volume) / on: they play at `$musicVolume`, silent with music off | on OK |
| Speaker mode Headphones | `$audiospeakers = "Headphones"` | data-free binaural stereo (`docs/audio/README.md` §18.3) | after a restart |

New sandbox games also take a **traffic density** and a **population density** (0.5×, 1× = the original, 2×, 4×) on the
setup page (`data/regs/extend/metro/ui/menus/sandbox.script`). They are stored in the save's `game.campaign`
(`regsTrafficDensity`, `regsPopulationDensity`, written by the `campaign.script` extension), applied before the map
loads (`environment.script` extension → `gsGridCitizens.regsPopulationDensity`, `gsGridVehicles.regsTrafficDensity`)
and cannot be changed during a game. Population scales the home slots per building; traffic scales car use and
background traffic.

**Bus lines at tram stops** (gameplay QoL, a deviation from the original; on by default): a bus line's add-stop mode
also takes a tram stop that sits in a street (small and large roads, where the tram runs in the curb lane), and its
buses halt there in the tram's lane and serve passengers like at a bus stop; tram lines keep the stop. Tram stops in an
avenue's middle strip or on their own track stay refused. Settings key `$regsBusAtTramStops` in `settings.script`
(`false` turns it off for the tool; saves with such lines keep running either way). Rules and data: `docs/ui/lines.md` §6.

## Tools

```powershell
$game = "C:\Program Files (x86)\Steam\steamapps\common\Cities In Motion"
.\build\x64-Release\regs-tool.exe vfs $game stats      # mounted archives and records
.\build\x64-Release\regs-tool.exe vfs $game verify     # unpack every archive entry
.\build\x64-Release\regs-tool.exe vfs $game cat metro/scripts/init.script
.\build\x64-Release\regs-tool.exe parse-all $game      # parse and compile every script-source file
.\build\x64-Release\regs-tool.exe parse some.script    # dump one file's syntax tree
.\build\x64-Release\regs-tool.exe run $game 120 <sandbox>      # boot init.script headless for 120 frames
.\build\x64-Release\regs-tool.exe script tests\vm_basics.script   # VM self-test
.\build\x64-Release\regs-tool.exe script tests\vm_threads.script  # threads across groups, array_splice in place
.\build\x64-Release\regs-tool.exe shader $game "" 0x1601E 0 0x1000   # one combiner variant as generated GLSL
.\build\x64-Release\regs-tool.exe shaders $game     # generate and compile the shipped shader variants (hidden window)
.\build\x64-Release\regs-tool.exe shadowtest        # the shadow light-camera fit vs. docs/renderer/shadows.md §3.7
```

`run` points `userdata/` at `<sandbox>\OpenCiM` instead of `Documents\OpenCiM`. The file system also refuses
writes outside `userdata/`, so test runs never touch your saves, your settings or the install.

Generated sources (re-run when the docs change):

```powershell
node ..\tools\gen-port-api.mjs ..\docs\native-api.json "$game\GSystem.dll" src\engine\api_generated.cpp
node ..\tools\gen-port-operators.mjs ..\docs\script-runtime\operators.json src\script\operators_table.inc
```

## Layout

| Path | Contents |
|---|---|
| `src/core/` | logging, text decoding (UTF-16 strings, Windows-1252 files), zlib inflate |
| `src/vfs/` | GS10 archives and the virtual file system (mount rules, loose overrides, `memory/`, `userdata/`) |
| `src/script/` | script language: lexer, parser, compiler, bytecode VM (threads, events, macros, `load`), values and operators (`vm_ops.cpp`; the dispatch table is generated from the engine's) |
| `src/engine/` | engine instance and `MAIN`, the generated native API (every class, field, method, event and constant), hand-written natives, the display (window tick, GUI pass) and input dispatch |
| `src/platform/` | OS window and OpenGL context behind an interface (Win32/WGL now; SDL or others later) |
| `src/render/` | OpenGL 3.3 core loader, DDS decoding, the batched 2D renderer |
| `src/app/` | `regs`, the game executable |
| `data/` | the port's own game-side files: script extensions, strings, post-effect shaders (mounted over the install) |
| `tests/` | script self-tests |
| `src/tools/` | `regs-tool` developer commands |

## Status

| Area | State | Check |
|---|---|---|
| Archives and file system | done | all 36 archives in the test install mount (including third-party mods with wrong size fields, which the engine tolerates); 22 848 entries unpack with 0 failures (`regs-tool vfs`). The game mounts the 24 official archives only (16 DLC packs load as addons) unless `--community-addons` is given |
| Script parser | done | 4 113 of 4 116 script-source files parse in 0.8 s; the 3 failures are mod files the original engine rejects too (missing `;`, a stray `2`) |
| Script compiler and VM | done | All shipped scripts compile. `tests/vm_basics.script` passes; it covers operators, conversions, containers, scoping, functions, events, threads and thread groups. `init.script` runs to the end headless. |
| Object model and native API | boot reaches `MainMenu` headless | Boot reaches `MainMenu` with no runtime errors, loading all addons and mods. All 123 classes are registered. Implemented so far: the module tree tick, clocks, timers, state machines with guards and actions, textures (real DDS sizes), GUI control geometry, fades and arguments, texture targets, and owner-created sub-objects. Audio, 3D model loading and `writePackage` are still stubs. |
| Window, input and 2D GUI | main menu works | `regs.exe` opens an OpenGL 3.3 window and runs the menus: DDS textures, both font formats with the engine's exact text metrics, control images, borders, text alignment and `%` formatting, fades, hover, clicks, keyboard routing. Text editing is not done yet. |
| 3D | menu scene works | The main menu's animated trams render: `.object`/`.material` loading, the sun from the scene clock, the fixed camera, fog, render-to-texture. **The shader combiner is ported**: `.shader` scripts, `setBaseShader`, `onProgram` and the component templates run through the port's own VM and produce the engine's GLSL 1.10 byte for byte; a translator turns it into GLSL 3.30 core with the fixed-function state as uniforms (alpha test included). `regs-tool shaders` compiles 3 749 variants: every shipped shader with the 78 material flag sets the 3 166 shipped objects use, the default shader also skinned and in the shadow passes. Only the terrain and water shaders fail, and only with those object flag sets, which they never get. |
| Game load and map | a save loads and its map renders | Loading `Cim2` from the menu runs the real load flow (`docs/ui/game-load.md`) into the `Game` state with the HUD. Grid templates run their companion scripts and `onLoaded`, load their meshes, texture variations and colour maps. The map draws in the main viewport per 16×16-cell division: terrain with its materials and shaders (`docs/renderer/terrain.md`: grass variants, cliffs, sea floor, yards, fields, water, map-edge walls, the water-edge curtain where the border meets water, foundation walls under `ENABLE_BASE` items on uneven ground), cover quadrants and normal overrides from items (`fixGrid`), and all 87 146 placed items (roads, tracks, props, buildings, trees) merged per division and material (`docs/renderer/grid-items.md`). The floating camera eases as in the engine; WASD, edge scroll and panning move it. Sun shadows, bloom/HDR and window MSAA follow the settings (`docs/renderer/shadows.md`, `bloom.md`). The minimap (`docs/ui/minimap.md`) renders the map texture as the game does (tutorialcity bit-exact, amsterdam and helsinki 99.9 % against the shipped thumbnails), with the camera footprint, pins and click-to-jump; the corner preview shows its masked empty state. Stops take the company colour (`gsGridItemController.setColor`). Not yet: transit lines on the minimap. |
| Saving | saves are written | The pause menu saves: `writePackage` writes the GS10 package zlib-compressed like the engine (`core/deflate.cpp`; Cim2 3.06 MB vs the original's 2.97 MB), `gsGrid.saveToFile` writes the loaded `map.grid` back section by section with the simulation's sections from its live state (`engine/grid_save.cpp`, `sim_save.cpp`: vehicles, parked cars, trains, boats, aircraft, animals, pedestrians with the rest of their walk, lines, citizens and routes; round trips on Cim2, Vienna and Berlin reload matching counts, `tools/gridparse.mjs` reads the saves with 0 issues), terrain, network and timeline come from the live map. Seated pedestrians and a few transient vehicle values are written as 0, the original's reset value, because the port doesn't model them. Text fields take typing (`gsEditCtrl`: caret, Backspace/Delete, arrows, `maxLength`, `onCheckInput`), and the save list shows the save dates. |
| Selection | hover, click, corner preview | The grid player picks like the engine (`docs/ui/selection.md`: `intersectGrid` over vehicles, items and terrain, cell cover, the 0xfe view mode): hovering shows the game's tooltip ("Karussell", "Mietshaus"), a click fires `onSelectObject`, and the corner HUD shows the live preview camera on the selection, its data rows, the selection arrow over it (`gsTracerCtrl`) and the minimap arrow. Item controllers (`gsGridItemController`, `gsTransportStopController`) carry name, model, position and place counts. A selected stop and every stop of a stop tool's kind (the preview too) show their catchment circle (the grid's data layer, `docs/renderer/terrain.md` §5.2); the stop bubbles and pins on the front HUD are laid out by the tracer organizer (`docs/ui/lines.md` §4.1). Not yet: citizen data pages. |
| Audio | sound, music and world sounds play | Own mixer on a miniaudio device (`src/audio/`, `engine/sound.cpp`, `engine/sim_sounds.cpp`; spec and port status: `docs/audio/README.md` §16, §18): menu and era music, city ambience by area (`gsGrid.getAreaType`), UI sounds, vehicle engine / move / tunnel loops with speed and acceleration, horns, bells, stop announcements, doors, animal voices, item loops, passenger fares; linear rolloff, Doppler, 64 mixed voices; pause menu and speed 0 behave as in the original; speaker modes Mono to 7.1, Prologic and the port's Headphones (data-free binaural); the port's effects, ambience and interface volumes and an optional menu-music fix. Checked with captures (`REGS_AUDIO_CAPTURE`). |
| Simulation | a loaded city runs | The save's transport lines, stops and vehicles load into the lane graph (`docs/simulation/`); trams drive their lines, dock with the documented dwell times, follow the speed buttons, and the date advances. Background traffic runs: the save's 771 moving cars, lorries and trains drive their saved routes, claim the lanes ahead (queuing and giving way at junctions, trams included), and new trips keep the count steady; named vehicles (e.g. "Taxi") can be hovered and selected, and the corner preview follows them. Citizens live their days (`docs/simulation/citizens.md`): 32 520 residents fill exactly the save's 32 520 home slots, plan trips, choose walking, car or tram, wait at stops, board, ride and pay fares (the money counter and line statistics move), and pedestrians walk the pavements with skeletal animation (`docs/renderer/skinning.md`; the merry-go-round and Ferris wheel turn too). Hovering a pedestrian shows its kind ("Tourist"); selecting it shows the citizen page (name, portrait, pins) with the preview on the person. Also running: parking, utility trips and company fleets, breakdowns and accidents (`REGS_TEST_CRASH` films one), walk layers for buildings, bridges, tunnels and platforms (no pedestrians over water), boats and aircraft with water bus and helicopter lines, metro platforms and turning at a terminus, animals, vehicle doors and boarding through them in mood order, level crossings (barriers down while a train holds the crossing, cars waiting, the warning bell), city growth from the timeline, and the sandbox densities. Port change: jam relief for buses and trams (stuck for 2 minutes, the cars around turn round one by one; still stuck 30 s later, the vehicle moves on two stops and its riders plan again; `docs/simulation/transport.md` §10). Walk and height maps are turned as the engine turns them (half a turn off the mesh), so station platforms connect to the street by their stairs and waiters stand on the platforms; on Vienna 0.5 % of walks find no path (was 10.7 %). Not yet: the rest of the failed walks (`docs/playable-todo.md` §4); aircraft gear and doors not seen on screen. |
| Statistics and graphs | graphs panel and annual report draw | `gsGraphCtrl` (`engine/ui_graphs.cpp`, `docs/ui/graphs.md`): ring buffers per series fed by `setArg`/`update`, the range widened by the data, series strips, the major/minor grid and the zero/max/min/current labels laid out as the original's `updateLines`/`updateTexts`. The graphs panel shows all 12 company and economy graphs with the 12–60 month slider and adds a sample each month while open; the annual report's passengers graph opens from its petition icon. The news ticker (script-only) scrolls, colours, plays and jumps to its place. |
| Particles | smoke, dust, fountains, fire, demolish | `gsParticleEmitter`, `gsEmittingParticleEmitter` and `gsParticleEngine` are native (`engine/particles.cpp`, spec `docs/renderer/particles.md`): emission (density, on/off cycles, cone, ring, colour ranges, scale over age, child emitters for sparks and fireworks), the engine's physics with wind, the grid's emitters on items (factory smoke, fountains, chimneys, fires, new-year fireworks via `effectConditions`) and vehicles (broken-down smoke, crash smoke, helicopter dust while the rotor turns over the pad, ship smoke) near the camera, and the script emitters (the bulldozer's demolish and construction dust). Drawn as sorted camera-facing billboards with `particles.shader` (lit, fogged, shadowed, additive fire), batched into one stream buffer per pass; blended ones cast shadows. Capped at 24 000 particles. Port change: helicopters board with a still rotor and spin up before take-off (`docs/simulation/boats-aircraft.md` §6.3). Not yet: boat wakes (`gsBoatContrain`), fires as a simulation event. |
| Everything else | not started | |

## Roadmap to a playable build

Each milestone ends with a check that can be run. RE work that a milestone still needs is listed with it; the rest
is already specified in `docs/`.

1. **Script VM.** Compile the syntax tree to bytecode and run it. This covers the variant types and operator rules,
   functions and default parameters, `with`/`foreach`/`current`, events, macros, `load` (script files as objects),
   cooperative threads (`wait`, `wait_until` watchers, `skip_frame`, thread groups with `pause`/`resume`), and
   `exportVariables`.
   **Done.** Semantics follow `docs/script-runtime/` (`semantics.md`, `values.md`, `operators.json`).
   Known deviations from the engine:
   * `wait_until` re-checks its condition every frame, instead of waking when a variable in it is written. This is
     equivalent for shipped scripts, which only wait on plain variables.
   * Objects sort and print by creation number instead of memory address.

   *Check:* `metro/scripts/init.script` runs headless to "Engine initialization finished". The remaining errors
   come from stubbed natives such as `new_texture` and `loadStateMachine`.
2. **Object model and native API.**
   * The `gsModule` tree with tick order (`docs/simulation/README.md`).
   * Field bindings with setters, hooks and read-only flags (`docs/native-api.json`).
   * All 131 global functions and 72 classes, first as logging stubs, then implemented.
   * Clocks and state machines (`.states`).

   *Needs RE:* the ~310 handlers not yet read.
   *Check:* boot reaches the `MainMenu` state headless. **Reached**, with no runtime errors (`regs-tool run`;
   the sandbox settings need a `$language`, as on any non-first launch).
3. **Window, input and 2D.**
   * The platform layer: a Win32 window, input, and an OpenGL 3.3 context behind an interface, so SDL or other
     backends can follow for ARM and other systems.
   * DDS textures (DXT1/3/5) and `.font` text.
   * GUI controls (`gsControl` family), following `docs/renderer/interfaces-2d-output.md`.

   *Check:* the main menu draws and responds to the mouse. **Reached.** The menus, submenus and dialogs draw with
   their fonts, borders, hover highlights and fades and react to mouse and keyboard; `regs --at …` scripts clicks for
   screenshot checks. Specs: `docs/ui/controls.md`, `docs/ui/input.md`, `docs/ui/fonts.md`. Deliberate differences:
   * The mouse is fed in absolute mode, as in the official Linux build. Windows warped the OS cursor to the window
     centre and sent relative motion.
   * Fullscreen is a borderless window over the monitor instead of a display-mode change (so the frequency and colour
     depth settings change nothing, and the resolution sizes the window when it is not fullscreen).
   * The back buffer is cleared every frame (the engine cleared it only on demand).

   Still open: render targets and material shaders (they need the 3D renderer, for example the main menu's
   animated trams), text editing in `gsEditCtrl`, and the chart, map and tracer controls used in game.
4. **3D scene.**
   * Meshes, materials, skeletons and animation (`docs/asset-format/`).
   * The GLSL 1.10 shaders run with the fixed-function state passed as uniforms.
   * Camera, terrain and cell geometry, sky and sun, particles, skinning.

   *Needs RE:* animation blending.
   *Check:* a saved game's map renders. **Reached.** The main menu's tram scene renders as in the original
   (`docs/renderer/scene-basics.md`), through the ported shader combiner (`engine/shaders.cpp`, `render/glsl.cpp`);
   a hand-written GLSL 3.30 version of the default shader stays as a fallback (`REGS_NO_COMBINER=1` forces it). A
   loaded save shows its terrain, water, roads, buildings and trees (`engine/terrain.cpp`, `engine/grid_items.cpp`).
   Known deviations:
   * All items are merged per division and material, as the engine does for `storeMesh` templates. The engine draws
     buildings, trees and stations as one instance each; placement and materials are the same, but the tree wind
     (`trees.shader`) sees division-space instead of model-space heights, so trunks sway a little more.
   * Upward item normals are not replaced by the terrain normal, and the `vertexGeneration` triangle order is not
     applied (only its spherical foliage normals).
   * The fade between 800 and 880 m and the near/in-range rules of `gsGrid::move` are replaced by distance bands
     around the eye with frustum culling; item meshes are built nearest first, six divisions per frame.

   **Shadows and bloom** follow `docs/renderer/shadows.md` and `docs/renderer/bloom.md` (`engine/effects.cpp`):
   the trapezoid-warped sun shadow map (`$shadows` × 1024, light pass with the `SHADOWMAP_RENDER` variants, receivers
   with `SHADOWMAP`), and the bloom chain (RGBA16F scene with MSAA, squared downsample, 4-level blur, red-channel RMS
   exposure with a 1 s half-life, tone-mapping composite). `regs-tool shadowtest` checks the light-camera fit
   against the worked example of `shadows.md` §3.7. Without bloom the view gets `$antialiasing` samples (the engine put
   them into the window's pixel format, which cannot change, so the port resolves a multisampled target into the
   window and the setting applies live). Deviations: the shadow pass reuses the main pass's draw list (division culling with a generous
   radius instead of the doubled per-mesh radius); trees in the light pass use the same merged LOD as the main pass;
   the unused `clipYPlane = false` fit (`gsFitTrapezoid`) and `screenSpaceBlur` are not ported.

   **Sky and night** follow `docs/renderer/sky-and-night.md` (`engine/sky.cpp`): the sky dome of
   `gsSky.generateMesh` (environment cube, `effects/sky/sky.shader`, camera-attached at 0.99 × far; the game hides it,
   the debug screenshot mode F5 shows it), the sun disc of `gsSun.generateMesh`, `gsTracerCtrl.enableFade` (the mood
   icons fade out at 300-320 units), and the material blend functions (additive street-light glows of the New York
   and Tokyo night maps). The original has no day/night cycle: each location has a fixed time and light, New York and
   Tokyo are night scenes lit by their illumination maps. The port's optional day/night cycle (graphics page) blends
   between the location's light and the shipped night or day preset, with a moon after sunset and stronger
   self-illumination at night. Not drawn: the sun's glow sprite (occlusion-query halo, the only `MATERIAL_GLOW` item).

   Base walls under items on slopes, the water-edge curtain, skinning, skeletal item animation and particles are
   done since.
5. **Simulation.**
   * Grid load and save (`docs/save-format/`).
   * Lane, pedestrian and transit graphs and searches; citizens, vehicles, lines and stops; the city timeline
     (`docs/simulation/`).

   *Check:* a loaded save runs, and its walk/drive/transit counts and line statistics stay close to the original
   game's over the same period.
6. **Construction tools.** Track and stop placement (`gsGridPlayer`; `docs/ui/construction.md`): the tool framework
   (`engine/tools.*`), tram and metro track laying (`tool_tracks.cpp`), stops, stations, docks and the bulldozer
   (`tool_objects.cpp`), the line panel's add-stop mode (`tool_addstop.cpp`); lines and vehicles in `sim_lines.cpp`.
   The underground view (`docs/renderer/underground.md`) shows the subway level. Still open: the editor's road,
   terrain and paint tools, undo.
   *Check:* passed on Cim2: lay track, place stops, create a line, buy trams, carry passengers, save and reload.
7. **Audio.** WAV/MP3 playback with 2D/3D sounds and music, replacing FMOD; Steam features become no-ops.
   **Reached** for sound (`docs/audio/README.md` §18): every script and engine sound of the original plays through
   the port's mixer, with the original's speaker modes and a new Headphones mode. Still open: door-item sounds
   (level crossings), a Prologic encoder with phase shift. `regs-tool sound <game> <path> [seconds] [stream]
   [layout=<name>] [at=x,y,z] [capture=<file.wav>]` plays one file (as a 3D voice with `at=`) for layout checks.
   *Mods:* a mod manager for ReGS mods (manifests, load order, script extensions, options API) is designed in
   `docs/mod-manager.md`; its base, the script-extension mechanism, already exists.
8. **Parity pass.** The original's saves load in the port (one-way: port saves need not load in the original),
   campaigns run start to finish, and mods load.

The concrete todo list to a playable game, by priority, is `docs/playable-todo.md`.

Ideas for after that (new graphics engine, high-res textures, a stereoscopic 3D mode, a map generator from
OpenStreetMap, ARM, mod manager,
multiplayer, cloud saves, RetroAchievements, …) are
collected in `docs/ideas.md`.

Third-party code in the tree: miniaudio 0.11.21 (`third_party/miniaudio`, public domain / MIT-0) for the audio device
and the WAV/MP3 decoders. Others that would speed things up (SDL2 for windows and input on every platform) are not
used yet; adding them means downloading them, which is a separate decision.
