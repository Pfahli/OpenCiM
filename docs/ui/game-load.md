# Loading a saved game: from the load dialog to the in-game 3D view

Scope: what the engine does between clicking a save in the "Spiel laden" dialog and the first in-game frame of the map,
and what the port (ReGS) must implement so that the map shows with a working camera (simulation not running yet).

**Citations.** Script paths are relative to `extracted/metro/` (`scripts/main.script:209` = `extracted/metro/scripts/main.script`
line 209). `W:0x…` = `decompiled/GSystem-v2/functions/0x….c` (Windows), `L:0x…` = `decompiled/Linux/functions/0x….c`
(named Linux build). **obs** = read in scripts or pseudocode, **inf** = inferred. Confidence H/M/L where it matters.

**State machine guards and actions.** `scripts/main.states` and `ui/loading/loading.states` keep each transition's guard
and action as script source in sub-chunks `0x05020101` (guard) and `0x05020102` (action) of the transition chunk
`0x05020100` (the port reads them: `port/src/engine/natives_objects.cpp:243-247`). `docs/main-states.mmd` and
`tools/states2mermaid.mjs` drop them; the ones on this path are decoded in §1. Strings in these sub-chunks are
NUL-terminated and followed by garbage padding.

---

## 0. Summary

1. The dialog's "ok" runs the transition action `loadSavedCampaign(<folder>)`, which loads `campaign.script`. Choice
   guards then pick `GameLoading`, whose action `loadScenario()` loads `scenario.script` and the save's three scenario
   parts. All of this runs **before** any engine object exists (obs, §1-2).
2. `GameLoading` starts the loading state machine, which `load`s 8 scripts in a paused thread group `"loading"` and polls
   each script's `$loadingPos` once per frame. Order: engine, environment, economy, player, ui, debug, game, level.
   **`presimulate.script` is not loaded for a save** (`$ingame` is already true) (obs, §3).
3. The map is built in two places: `environment.script` registers every grid object template (`loadGridObject`), then
   `level.script` calls `grid.loadFromFile("userdata/saves/<n>/map.grid")`. The timeline replay is a no-op for saves
   (obs, §4).
4. The loading screen ends in state `Completed` and **waits for a click on "Spielen"**. Only then does the main machine
   enter `Game`, which attaches `MAIN.$camera`/`MAIN.$scene` to the main viewport, enables the scene, resumes the
   `game`/`gameUI` thread groups and fades in (obs, §5).
5. The in-game camera is a **`gsFloatingCamera`** created in `engine.script`. Its pose is **not saved**: every load
   starts at `setCameraPos(-120, -40)`, zoom 150, direction 0 (obs, §6).
6. Biggest risks for the port: a runtime error in a loading script after it has set `$loadingPos < 1` hangs the loading
   screen forever, and an error in the `Game` enter handler leaves the screen black (inf from obs semantics, §7).

---

## 1. State path

| # | Machine: transition | Guard (branch order) | Action | Evidence |
|---|---|---|---|---|
| 1 | main `MainMenu --loadgame--> LoadGameMenu` | | | `main.states` |
| 2 | enter `LoadGameMenu`: `$menu.showDialog(…, getSaveGamesList())` | | | `scripts/main.script:513-516`, list `:488-511` |
| 3 | dialog closes: `$menu.$resultValue` = the item key after its vertical-bar separator = save folder name, then `perform("ok")` | | | `ui/ui.script:683-684,713-715`, `ui/menus/mainmenu.script:143-147,177-187` |
| 4 | `LoadGameMenu --ok--> choice#9` | | `loadSavedCampaign($menu.$resultValue);` | `main.states` |
| 5 | `choice#9 → LoadError` | b1 `!checkCampaign()` | `unloadCampaign();` | `main.states`; `main.script:343-348` |
| 6 | `choice#9 → choice#10` | b2 `MAIN.$campaign.$campaign.$ingame` | | `main.states` |
| 7 | `choice#9 → ScenarioMenu` | b3 (none) | `loadMainMenu(); loadSubMenu("scenario");` | campaign saved between scenarios |
| 8 | `choice#10 → GameLoading` | b1 `MAIN.$campaign.$campaign.$singleScenario` | `loadScenario(); unloadMainMenu();` | `main.states` |
| 9 | `choice#10 → ContinueGame` (dialog) | b2 (none) | | campaign saves; ok → `GameLoading` with `loadScenario(); unloadMainMenu();`, cancel → same with `$ingame = false` first (restart, presimulate) |
| 10 | `GameLoading --completed--> Game` | | | fired by `$loading.onEnd` (`main.script:757`) |
| 11 | `GameLoading --error--> LoadError` | | `unloadGame(); loadMainMenu();` | fired by `$loading.enter["Error"]` (`main.script:758`) |

The three shipped sample saves (`samples/saves/{Cim2,berlin,vienna}/game.campaign`) all have `$ingame = true`,
`$singleScenario = true`, so they take 4 → 6 → 8 (obs). Loading from the pause menu (`IngameLoadMenu --ok--> choice#9`,
action `unloadGame(); unloadCampaign(); loadSavedCampaign(...)`) joins the same path (obs).

Loading machine (`ui/loading/loading.states`, initial state `Prepare`):

| Transition | Guard | Effect |
|---|---|---|
| `Prepare --prepared--> choice` | | |
| `choice → LoadNext` | b1 `$loadPosition < #$toLoad` | load the next script |
| `choice → Completed` | b2 `MAIN.$scenario : false` | always taken in game (scenario exists) |
| `choice → final` | b3 | `onEnd` (editor path) |
| `LoadNext --completed--> choice` / `LoadNext --error--> Error` | | |
| `Completed --ok--> final` | | final state fires `onEnd` → main `perform("completed")` |

---

## 2. Phase A: the transition actions (before any engine object exists)

Runs inside the main machine's transition, thread group 0. Order (obs):

1. `loadSavedCampaign($n)` (`main.script:209-219`) sets `MAIN.$saveGameName`, and points `$campaignFileName`,
   `$scenarioFileName`, `$locationFileName`, `$defaultFileName`, `$dataFileName`, `$gridFileName`, `$timelineFileName`
   at `userdata/saves/<n>/{game.campaign, level.scenario, map.scenario, default.scenario, gamedata.script, map.grid,
   map.timeline}`. The `.gs` archive must be mounted as that folder (`docs/save-format/README.md` §1).
2. `load "/metro/scripts/campaign.script"`: `MAIN.$campaign = this`; `loadCampaign()` loads `game.campaign` and defaults
   `$privateCars` from the difficulty (`scripts/campaign.script:5,8-15,43`). Its `$grid = MAIN.$grid` reads a not yet
   existing member and gets valueless; harmless.
3. Guards: `checkCampaign()` needs `defined $saveVersion` and `file_exists($campaign.$filename)` (e.g.
   `metro/scenarios/sandbox.scenario`) (`main.script:343-348`).
4. `loadScenario()` (`main.script:221-253`); `$ingame` is true, so the save's paths are kept:
   * `load "/metro/scripts/scenario.script"`: `MAIN.$scenario = this`, three `new_sound($desktop)` + `.load()`,
     then `$scenario = loadScenario(MAIN.$scenarioFileName)` (`scripts/scenario.script:6,11-27,374`).
   * `scenario.loadScenario(path)` (`:337-352`): `load` the file; `$levelName` (saved as
     `userdata/saves/<n>/map.grid`) is copied to `MAIN.$gridFileName`; `requireThread` ×3; `startScenario` creates one
     `thread "game"` per saved `$currentThreads` entry (e.g. `["mainloop", 0]`). The `game` group is still paused
     (new named groups start with pause count 1, `docs/script-runtime/semantics.md` §5.2), so they run only after `Game`
     enter. A `thread "gameUI"` (wait 1 s, reopen pending scenario-end petitions) is queued the same way (`:376-389`).
   * `$scenario.$location = loadScenario(map.scenario, true)` and `$default = loadScenario(default.scenario, true)`.
     `map.scenario` carries `$locationSetup` (e.g. `metro/locations/germany`), city name, seal, illustration.
   * `MAIN.$gridCellSize = 768`, `$gridLocation = "metro/locations/germany"`, `$gridStartingDate`, `$gridDescription`
     (`main.script:249-252`): fallbacks only.
5. `unloadMainMenu()` (`main.script:60-70`): deletes the menu (and its tram scene), removes the desktop input target,
   hides and releases the mouse, `$viewport.visible = false`.

None of this needs a new native beyond `load`, `file_exists`, `new_sound` (an inert object is enough) and thread groups.

---

## 3. Phase B: the loading screen machine

`enter GameLoading` (`main.script:737-759`): `$desktop.visible = false`; `$loading = loadStateMachine("/metro/ui/loading/loading.states", this)`;
`$toLoad` = 8 (path, weight) pairs; `presimulate.script` (0.5) is appended **only if `!$ingame`** and then `$ingame = true`
(`:751-755`); `$loading.prepare($toLoad)`; bind `onEnd` → `perform("completed")`, `enter["Error"]` → `perform("error")`.

`ui/loading/loading.script` (obs):

| Lines | What | Natives that must return values |
|---|---|---|
| 10-148 | Black background on `MAIN.$backOverlay`, illustration (`animation.script`), top/bottom bars, progress bar, city seal, description, titles (from `MAIN.$scenario`/`$campaign`) | `new_control`, `setImage`, `setCropRect`, fonts, **`getTextHeight()` (`:105`)**, `clock()` + `.years` (`:139-141`) |
| 150-162 | `prepare()`: totals, `skip_frame`, `perform("prepared")` | |
| 164-188 | `enter LoadNext`: `MAIN.$gameScripts[] = $script = load "loading" :: path; resume "loading";` then once per frame: break if `!defined $script.$loadingPos` or `>= 1.0`, else update the bar and `skip_frame`; then `pause "loading"`; `perform("error")` if `$script.$loadingError : false`, else `perform("completed")` | `load g :: path` (runs the top level immediately up to its first yield, then continues in group `g`) |
| 190-201 | `enter Completed`: hide "Lade ...", reparent `$background` to `$desktop` (`.owner =`), create the **"Spielen"** button (`$menu_loading_button`) at `(W-220, H-88, 220×64)` relative to the window, show the desktop and the mouse | `MAIN.$ui.createButton` |
| 203-208 | button: disable, `MAIN.$fade.blend(1.0, 0.2)`, `wait 0.2`, `perform("ok")` | |

Rules that follow (inf from the code above and `semantics.md` §5.1, §9.1, H):

* A script without `$loadingPos` (engine, economy, player, debug) counts as finished as soon as its top level returns or
  first yields. A script with `$loadingPos` is finished only when it writes `>= 1.0` (each does so on its last line).
* A runtime error kills only the erroring thread. If that is a loading script's top level after it set `$loadingPos`
  below 1.0, the `LoadNext` loop spins forever: **the loading screen hangs**. Errors inside functions the script calls
  synchronously (other scripts' functions included) count, because they run in the same thread.
* If `loading.script`'s own top level dies before line 164, the `LoadNext` handler is never bound and the machine stops
  in `LoadNext`.
* `level.script` refreshes the bar during the synchronous `grid.loadFromFile` through `grid.onLoadProgress` →
  `updateLoadingPosition` + `MAIN.$window.update()` (present without tick) (`scripts/level.script:6-13`). The engine
  fires that event with 0.1, 0.3, 0.9, 1.0 (`docs/handlers/batch-A.md` §loadFromFile). Optional for the port.
* The bar weights sum to 2.229 for a save (2.729 with presimulate) (`main.script:741-753`).

---

## 4. Phase C: the loaded scripts, in order

`R` = needed for the map to render, `V` = must return a usable value or the load hangs/breaks (§7), `S` = may stay a
no-op stub for the first milestone. "dies at" = what an error there costs.

| # | Script (weight) | `$loadingPos` | Creates / does | Render-relevant natives |
|---|---|---|---|---|
| 1 | `engine.script` (0.127) | none | scene, clocks, sky, sun, grid, timeline, camera, effects | see 4.1 |
| 2 | `environment.script` (1.007) | 0 → 1 (`:58,181,191,232`) | `MAIN.$level`, location, names, **all grid object templates** | `loadGridObject` (R, V), `getGridObject` (V) |
| 3 | `economy.script` (0.001) | none | `MAIN.$economy`, functions only | — |
| 4 | `player.script` (0.001) | none | `MAIN.$playerData`, `MAIN.$gameData` (save's `gamedata.script`), **`MAIN.$player = new_gridPlayer($desktop)`** with `.grid/.camera/.viewport`, HUD layers `MAIN.$hud/$backHud/$frontHud` | `new_gridPlayer` (R for input), `new_tracerOrganizer` |
| 5 | `ui.script` (0.062) | 0 → 1 (`:10,38,40,42,657`) | `MAIN.$gameUI` (camera model, §6.3), minimap, secondary view, hover info, `controls.script` key bindings, 4 edge-scroll strips | `new_texture` ×16, `getAreaY` (via `setCameraPos`) |
| 6 | `debug.script` (0.001) | none | console commands and debug keys if `$settings.$debug` | — |
| 7 | `game.script` (0.228) | 0 → 1 (`:10-60,736`) | 15 HUD panels + ambience, toolbar and build menu, citizen/car tables, `updateERA`, enables vehicles/characters/citizens/lines | none for rendering; several V (§7) |
| 8 | `level.script` (0.802) | 0 → 1 (`:3,30,41,81`) | **`grid.loadFromFile`**, timeline, UI init, **initial camera** | `loadFromFile` (R), `width` (V), `getAreaY` (V) |

### 4.1 `engine.script` (obs; dies at = later objects missing, no hang)

| Lines | Object | Settings | Class |
|---|---|---|---|
| 4-10 | `MAIN.$scene = new_scene($window)`, `.particleEngine = new_particleEngine($scene)` | visible, **enabled = false**, audible false | R |
| 12-16 | `MAIN.$sceneClock = clock(8:30, 1.1.1920)` | speed 0 (location.script then sets 14:30) | R (sun direction) |
| 17-24 | `MAIN.$mainClock = clock(scenario.$currentTime, scenario.$currentDate)` | speed 10000, paused | V (fields read) |
| 26 | `MAIN.$viewport.clearColor = 0xFF373028` | | R |
| 28-35 | `MAIN.$sky = new_sky($scene)`, `generateMesh(5)` | fog 0xCCC8BB / 300 / 1500, **visible = false** | R (fog only) |
| 37-45 | `MAIN.$sun = new_sun($scene)`, `generateMesh(40, 0.04)`, light colours, `.clock = $sceneClock` | | R |
| 47-220 | `MAIN.$grid = new_grid($scene)`: `material` = terrain.shader + 5 textures, earth/reserved/base/edge/positive/negative/water/waterEdge/data materials, `getYardMaterial(0..7)`, `getFieldMaterial(0..3)`; `vehicles/characters/citizens/lines.enabled = false`, `citizens.clock = $mainClock`, `visible = true` | | R; sub-modules and the two getters V (an error at `:214` loses timeline, camera, `addCamera`) |
| 224-226 | `MAIN.$timeline = new_gridTimeline($window)`, `.grid` | | S (object V) |
| 228-239 | **`MAIN.$camera = new_floatingCamera($scene)`** | offset (0,0,150), position 0, orientation `quat(0.855,X)*quat(0.001,Y)`, fov 40, smoothness 1e-6, min 30, max 310, smoothPosition, `moveToFirst()` | R |
| 241-243 | `$grid.addCamera($camera)`; `MAIN.$listener.offset`, `.tracked = $camera` | | R (bucket activation, §6.6); listener S |
| 247-249 | `grid.timeSpeed = 1`, `particleEngine.timeSpeed = 1`, `setRunSpeed("game", 1)` | | S |
| 251 | `MAIN.$viewport.maxDistance = 600` (overrides `viewport.script:8`) | | R |
| 255-265 | if `$settings.$shadows` (default 2): `video.new_shadowEffect(2048, 2048)`, `.viewport/.light/...`, `yPlane = -10`, `viewport.addEffect` | | S; but `new_shadowEffect` must return an object or the script dies at `:257` (effects and `onDestroy` lost) |
| 266-297 | if `$settings.$bloom` (default true): `new_bloomEffect()`, `addEffect`, saturation fade-in thread in `gameUI` | | S (guarded by `if ($bloom)`) |

### 4.2 `environment.script` (obs)

1. `:3-4` `$grid.citizens.homeMultiplier/workplaceMultiplier` (sub-module V).
2. `:11-52` `$location = load "<locationSetup>/location.script"` (from `map.scenario`, else `MAIN.$gridLocation`, else
   germany); a missing file sets `$loadingError` → loading `Error` → `LoadError`. `location.script` sets
   `$envsets`, banks, `MAIN.$saturation`, `MAIN.$sceneClock.time = 14:30`, sky fog (germany 0xCCD8BB / 300 / 1500), sun
   light colours (`locations/germany/location.script`). Names script → `MAIN.$names`.
3. `:183-216` each `$location.$envsets` entry is `load`ed (nested; an error kills only that envset). Envsets call
   `$level.loadTree/loadProp/loadBuilding/loadAIVehicle/loadPlayerVehicle` (`:61-160`) or `$grid.loadGridObject`
   directly: about 1 070 loader calls for germany (`envsets/*.script`), and more templates than that because many
   loaders also register a "b" (diagonal) variant. Then markers, `crash`, `fire-medium`, and
   `MAIN.$addons.$environments`.
4. `loadGridObject(id, file[, baseFile[, sx, sz]])` (`docs/handlers/batch-A.md` §loadGridObject): creates the template,
   **loads the `.object` and runs its companion `.script`/`.properties` immediately**
   (`docs/asset-format/skeleton-animation-scene.md`, `W:0x1008bd90.c:65-93`). The companion script sets `width`,
   `length`, `height`, `flags`, `type`, `connections`, `diagonals`, texture variations and script variables such as
   `$price` (e.g. `objects/roads/small_basic.script`, `objects/buildings/CE-ind-l-074/CE_ind_l_074.script`). 3 args:
   45° rotation (π/4) and the "b" alias; 5 args: scale `(sx, 1, sz)`.
5. `:218-222` `if ($santa = $grid.getGridObject("santa-claus"))`: must return a template or **null**, not valueless.

Templates must exist before `level.script`: the grid loader resolves each type name of the map and logs
`Unknown object: <name>` for missing ones (`L:0x08146470.c:303-315`, obs).

### 4.3 `player.script`, `ui.script`, `game.script` (obs)

* `player.script:8` `MAIN.$gameData = load "/userdata/saves/<n>/gamedata.script"`; `:15-22` `MAIN.$scenario.initEngineData()`
  (empty in shipped scenarios); `:27-34` `new_gridPlayer($desktop)` full-screen, `visible`, `active`, `.grid`,
  `.camera = MAIN.$camera`, `.viewport = MAIN.$viewport`; `:36-57` HUD controls (needed by `MAIN.$ui.createHudElement`,
  `ui/ui.script:61`).
* `ui.script:11-15` camera model defaults; `:37-42` minimap, secondary view, hover info (nested loads); `:44`
  `controls.script` (§6.7); `:52-74` edge strips as children of `MAIN.$player`.
* `game.script:14-45` HUD panels and `ambience.script` (nested loads); `:47-58` toolbar; `:62-379` citizen setup
  (`clearGameData`, `addCharacter`, `addCar`, `setUtilProbability`: S); `:84` `MAIN.$economy.updateCitizenEconomy()`
  (V, §7); `:582-597` build-menu buttons; `:612-709` clock/grid event bindings; `:723-725` addon game scripts; `:727`
  `updateERA` (`vehicles.setCargoTrains` etc.: S); `:728-732` enable vehicles/characters/citizens/lines,
  `lines.visible = true`; `:734` `MAIN.$gameUI.startGame()` (strips to front).

### 4.4 `level.script` (obs)

| Lines | Call | For a save | Class |
|---|---|---|---|
| 6-13 | bind `grid.onLoadProgress` | bar + `window.update()` | S |
| 19-28 | `$grid.loadFromFile(MAIN.$gridFileName)`; `MAIN.$gridCellSize = $grid.width` | the map: header, terrain `0x0701`, network `0x0702` (+ names), then vehicles, characters, lines, citizens | **R** (terrain + network); the other sections S for a static map |
| 32-38 | `timeline.loadFromFile`; `applyChanges(currentDay+1, mainClock.date − mkdate(1,1,1920), 182)`; set `currentDay` | a save's timeline has `currentDay` = save day, so `fromDay > toDay`: **no-op** (`docs/save-format/lines-timeline.md` "Saves"; `batch-A.md` §applyChanges) | S (`currentDay` field V) |
| 47-55 | economy catch-up if `#economyHistory < 60` | skipped (saves have ≥ 60 entries) | — |
| 57-59 | `MAIN.$linepanel.initLines()` | iterates `$grid.lines.children()` | V |
| 61-64 | `MAIN.$minimap.$minimap.scale = -1.0 / (3.0 * $grid.width)`; `minimap.update()` → `grid.renderMap` | division: width must be non-zero | V (`renderMap` S) |
| 66-69 | `MAIN.$timebar` | not loaded in game (editor only) | — |
| 71-73 | `MAIN.$secondaryView.initBubbles()` | `citizens.findCitizen` per customer profile (empty in the samples) | V |
| 75-77 | `MAIN.$playerData.updateCompanyValue()` | reads `getGridObject(name).$price` for every owned vehicle (`scripts/player.script:155-180`) | V: needs the companion scripts run (4.2.4) |
| **79** | **`MAIN.$gameUI.setCameraPos(-120.0, -40.0, true)`** | initial camera, §6.3 | **R** |

New games differ: `presimulate.script` (`resetTraffic`, then 125 × `$scene.tick(1.0)` with the scene enabled, then
disabled again) and, on base maps, the timeline replay is what builds the city from its 1920 state (obs). Neither is on
the save path.

---

## 5. Phase D: `Completed` → `Game`

1. Click "Spielen" (centre ≈ `(W-110, H-56)` in window pixels; `loading.script:193`). The fade goes to black over 0.2 s,
   `perform("ok")` → final state → `onEnd` → main `perform("completed")`.
2. `exit GameLoading` (`main.script:761-766`): `delete $loading` (its `onDestroy` deletes the screen and releases the
   mouse), `$desktop.visible = true`.
3. `enter Game` (`main.script:807-829`), in this order:
   `$viewport.camera = MAIN.$camera; $viewport.scene = MAIN.$scene; $viewport.visible = true;`
   `MAIN.$player.visible = true; .active = true;` then, first time only: `requireMouse`, `addInputTarget($desktop)`,
   `showMouse`, **`MAIN.$scene.enabled = true`**, `audible = true`, `MAIN.$ambience.resumeGame()`,
   **`resume "game"; resume "gameUI";`**, `MAIN.$mainClock.paused = false`, `$playTimeClock.paused = false`; then
   `wait 0.1; MAIN.$fade.blend(0.0, 0.2)`.
4. From now on: the scene ticks (camera first, then the grid builds bucket geometry around it, §6.6); the clock runs
   (onDay → `timeline.applyChanges`, `scenario.dateChanged`; onMonth/onYear handlers); scenario threads (`mainloop`,
   achievements) and the bloom saturation fade start. For "map renders, simulation not running" these can stay no-ops,
   but their natives must not return valueless where scripts branch on them (errors there only kill those threads).

---

## 6. The in-game 3D view

### 6.1 Viewports (obs)

| Viewport | Created | Content | Order |
|---|---|---|---|
| GUI root | `scripts/window.script:49-54` (local `$viewport`, pixel coords, full window) | `MAIN.$desktop`, `$backOverlay`, `$overlay`/`$fade` | rendered second (on top) |
| main 3D `MAIN.$viewport` | `scripts/viewport.script:5-13` at boot: `pixelCoordinates`, full window, `maxDistance = $settings.$viewdistance : 400`, **`sendToBack()`**, `canActivate = false`, visible | camera + scene set on `Game` enter | rendered first: `sendToBack` moves it to the front of the root output list (`docs/renderer/interfaces-2d-output.md` §1.5.2) |

In game: `clearColor 0xFF373028` (alpha ≠ 0, so the colour buffer is cleared), `maxDistance 600`, effects shadows then
bloom (§4.1). `gsViewPort` defaults min 0.1 / max 400 (`docs/renderer/scene-basics.md` §2.2). `gsGridPlayer` is a GUI
control on the desktop (full screen); it holds the 3D viewport and camera only for picking and input.

### 6.2 Scene contents (obs)

`MAIN.$scene` children: particle engine, sky (invisible, but its fog values apply every tick, `scene-basics.md` §5),
sun (direction from `$sceneClock` = 14:30, speed 0; light colours from `location.script`), grid, camera (moved first).
`$saturation` (0.6 for germany) only feeds the bloom pass.

### 6.3 Script camera model (`MAIN.$gameUI`, `scripts/ui.script`, obs H)

State: `$cameraX`, `$cameraZ` (world units), `$cameraDir` ∈ 0..7 (45° steps), `$cameraZoom` ∈ [75, 500], `$cameraTilt`
(screenshot mode only). Defaults `0, 0, 0, 150, 1.0` (`:11-15`). None of it is saved (the scenario export list,
`scripts/scenario.script:359`, and `player.script:98-108` contain no camera field), so **every load starts from the
same pose**.

```
setCameraPos(x, z, force=false)                                     ui.script:186-201
  a = dir·π/4
  cameraX = clamp(x, −1.465·grid.width, +1.465·grid.width)          // world units, grid centred on 0
  cameraZ = clamp(z, −1.465·grid.length, +1.465·grid.length)
  y = grid.getAreaY(cameraX − 30·sin a, cameraZ + 30·cos a, 0.05·zoom + 100, 1 − (zoom−75)/425)   // §6.5
  y = max(0, y − 0.3·(zoom − 75))
  if MAIN.$shadows: shadows.yPlane = −y − 10
  if grid.viewMode == GRID_VIEW_UNDERGROUND: y −= 14
  camera.position    = (cameraX, y, cameraZ)
  camera.maxDistance = 160 + zoom + 1.5·y                           // far plane
  camera.minDistance = 23/425·(zoom − 75) + 25                      // near plane
  if screenShotMode: camera.maxDistance = 2500
  if force: camera.reset()                                          // snap, no smoothing
setCameraZoom(v): zoom = v; camera.offset = (0,0,zoom);
                  camera.orientation = quat(0.6 + 0.0017·zoom, X) * quat(dir·π/4, Y);
                  listener.offset = (0,0,zoom); setCameraPos(cameraX, cameraZ)        :90-104
setCameraDirection(v): dir = v & 7; orientation as above; setCameraPos(cameraX, cameraZ)  :76-81
```

**Initial pose of a loaded save** (`level.script:79` → `setCameraPos(-120, -40, true)`, zoom 150, dir 0; orientation
still the one from `engine.script:231`; values computed with `scene-basics.md` §1-2.2, inf H):

| Quantity | Value |
|---|---|
| position (look-at point P) | `(-120, y0, -40)`, `y0 = max(0, getAreaY(-120, -10, 107.5, 0.8235) − 22.5)`; flat ground at the default 512 (= 6.0 world units) gives `y0 = 0` |
| orientation Q | `quat(0.855, X) ⊗ quat(0.001, Y)`: pitch 49.0° down, heading −Z |
| offset | `(0, 0, 150)` → eye = P + rot(Q, offset) ≈ `P + (−0.10, 113.19, 98.43)` |
| look / up | look = P (offset.z − 1 > 0, so the look offset is 0); up ≈ `(0.0008, 0.6562, −0.7546)`; screen right ≈ +X |
| fov / near / far | 40° vertical / 29.06 (camera `minDistance`, larger than the viewport's 0.1) / `min(310 + 1.5·y0, 600)` |
| visible ground (y0 = 0, centre column) | about z ∈ [−146, 15], i.e. ~160 units (~53 cells) deep |

### 6.4 `gsFloatingCamera` per-tick update (obs H unless marked)

Fields (Windows offsets; Linux = −0x50): gs3DElement `localPos +0xd9`, `localOrient +0xe5`, `globalPos +0xf5`,
`globalOrient +0x101`, update flag `+0x111`; gs3DCamera `target +0x168`, `offset +0x16c`, `finalOffset +0x178`,
`lookback +0x184`, `fov +0x188` (default 60), `ignoreOrient +0x18c`, `minDistance +0x190`, `maxDistance +0x194`
(defaults −1), `finalMin +0x198`, `finalMax +0x19c`; gsFloatingCamera `currentOrient +0x1a0` (identity),
`currentPosition +0x1b0` (script field, 0), `lockVertical +0x1bc`, `lockOrient +0x1bd`, `smoothness +0x1c0`
(default **0.1**), `smoothPosition +0x1c4` (false). Defaults: `L:0x080e9c00.c`, `L:0x080e9f50.c`; registrar
`L:0x080e8070.c`.

`tick(dt)` = `gs3DCamera::move` (`W:0x100719c0.c`): `updateFinalOrient(dt)`, then `gsCamera::updateCamera` (eye/look/up,
`scene-basics.md` §2.1), then children. `reset()` (`W:0x10071940.c`) = `updateFinalOrient(−1)` + `updateCamera`.

```
updateFinalOrient(dt)                                              W:0x10072390.c
  if ignoreOrient: setGlobalOrient(localOrient); finalOffset = offset;
                   if smoothPosition: setGlobalPos(localPos); finalMin/Max = min/maxDistance; flag=1; return   :35-46
  if flag(+0x111) == 0: return                                                                                :48
  R = target ?: parent3D                       // the owning scene here: identity matrix and orientation
  Qt = R ? localOrient ⊗ (R.front ⊗ R.globalOrient) : localOrient          // = orientation for this camera   :51-132
  Pt = R ? localPos · R.matrix : localPos                                  // = position
  (lockVertical, only with R: Qt = localOrient ⊗ quat(heading(R orientation) + π/2, Y); not used in CiM)
  (setGlobalOrient / setGlobalPos below are vtable +0xfc / +0xf8)
  if dt >= 0:
     if !lockOrient:                                                                                          :134-160
        f = pow(smoothness, dt)                // weight of the OLD value (L:0x080e8e00.c: pow(+0x170, dt))
        currentOrient   = slerp(Qt, currentOrient, f)       // FUN_10085c10; argument roles inf (M)
        if smoothPosition: currentPosition = (1−f)·Pt + f·currentPosition       // FUN_10083790
        finalOffset     = (1−f)·offset + f·finalOffset
        finalMin        = (1−f)·minDistance + f·finalMin                       // FUN_100831f0
        finalMax        = (1−f)·maxDistance + f·finalMax
  else:  currentOrient = Qt; finalOffset = offset; currentPosition = Pt; finalMin/Max = min/max   :162-176  (reset)
  setGlobalOrient(lookback ? (quat(π,Y) composed with currentOrient) : currentOrient)                        :178-195
  if smoothPosition: setGlobalPos(currentPosition)                                                            :196-198
  flag(+0x111) = 1
```

`setGlobalPos/Orient` write only the global pose (`W:0x10066c50.c`, `W:0x10066c90.c`); the script's `position` /
`orientation` (local pose) stay the target. With `smoothness = 1e-6` the camera keeps 1e-6 of its lag per second:
10 % after 0.17 s, 1 % after 0.33 s, frame-rate independent. The view uses the smoothed values: eye =
`globalPos + rot(globalOrient, finalOffset)`, near/far = `finalMin/finalMax` (`W:0x100719f0.c`, `scene-basics.md` §2.1-2.2).
A port may snap (use the targets directly) for the first milestone; it only loses the easing.

The camera ticks only while the scene is enabled (from `Game` enter); during loading only `reset()` moves it.
`moveToFirst()` puts it first among the scene's children (`L:0x08137d50.c`), so it updates before the grid reads it (inf).

### 6.5 `gsGrid.getAreaY(x, z, diameter, itemWeight)` (`L:0x0813ff00.c`, obs H; items part M)

```
s = grid.scale (3.0); cx = round((x + width·s/2)/s); cz = round((z + length·s/2)/s); r = round(0.5·diameter/s)
h = 0
for every 4th cell (i, j) in [cx−r, cx+r] × [cz−r, cz+r] (clamped to the grid), with d² = (i−cx)² + (j−cz)² < r²:
    h = max(h, round(terrainHeight[j·width + i] · (cos(π·d²/r²) + 1)/2))       // heights in 1/256 cell, after the load shift
for each item overlapping that square with item+0x3c > 0x600 and visible in viewMode:
    d² from (cx, cz) to the nearest point of the item rectangle; if d² < r²:
    h = max(h, round((round((item+0x3c − 0x600)·itemWeight) + item.y) · (cos(π·d²/r²) + 1)/2))
return h · s / 256                                                               // world units
```

So hills and items taller than 6 cells near the look-at point raise the camera target. A stub returning 0.0 is exact on
flat ground at the default zoom (§6.3) and keeps `setCameraPos` working.

### 6.6 Grid and camera (obs from `docs/simulation/grid-traffic.md` §3, §5.4)

`grid.addCamera` adds the camera to the grid's camera set (`W:0x100bd510.c`). Each grid tick, for each camera, buckets
(`gsGridDivision`, 2^4 = 16 cells square) within `R = max(10·fov, 100)/S` (400 for fov 40, if the scene scale S is 1) of
the focus point `F = camera globalPos, y = 0` are active; active buckets build their geometry (terrain, water, edges,
item instances) in `gsGridDivision::update` (`W:0x100d6ab0` = `L:0x08166080`, 28 KB, with `createVertex`,
`createEarthVertex`, `createBaseVertex`, `createEdgeVertex`, `createWaterVertex`, `addObject`), near detail when
`eyeDist < 400 && focusDist < 400`. Inactive buckets free their geometry. The builder is specified in
[`docs/renderer/terrain.md`](../renderer/terrain.md) (terrain, water, edges) and
[`docs/renderer/grid-items.md`](../renderer/grid-items.md) (items); the cover quadrants and normal overrides it reads
come from `gsGridItem::fixGrid` (`L:0x08184890`, with `cellCover`/`pointCover` `L:0x08183120`/`L:0x08182ff0`), as
implemented in `port/src/engine/grid_items.cpp` (`fixGridCover`).

### 6.7 Controls (obs; defaults from `defaults/controls.keys`, overrides from `userdata/controls.keys`)

`controls.script` binds `MAIN.$player.keyDown/keyUp/keyRepeat[<key name>]`. These reach the grid player through the
desktop's active chain (`docs/ui/input.md` §3.7), so `MAIN.$player.active = true` and `addInputTarget($desktop)` matter.
The wheel sends `MouseWheelUp/Down` as keyDown only; mouse buttons are keys too (`input.md` §1).

| Action | Default keys | Effect (`ui.script`) |
|---|---|---|
| rotate | `Left` → dir+1, `Right` → dir−1 | `setCameraDirection` (45° steps) |
| zoom in / out | `MouseWheelUp`/`Q`, `MouseWheelDown`/`E` (keyDown and keyRepeat) | `zoom = max(75, zoom/1.2)` / `min(500, zoom·1.2)`; pitch = 0.6 + 0.0017·zoom rad (41.7° … 83.1°) |
| move | `D` bit 1, `S` bit 2, `A` bit 4, `W` bit 8 (keyDown sets, keyUp clears) | `scrollKey`: a `gameUI` thread moves by `skip_frame·zoom` per frame along `(cos a, sin a)` for x and `(cos(a+π/2), sin(a+π/2))` for z, `a = dir·π/4`; dir 0: W = −Z (forward), D = +X (`:123-141`) |
| edge scroll | mouse within 5 px of an edge | strips `onMouseOver/onMouseOut` and `onMouseMove` start/stop a thread: speed `zoom·max(0, 1 − 0.2·px from edge)` per second, direction `arg(mx/W − 0.5, my/H − 0.5) + a` (`:165-184`, `:203-221`) |
| pan | `MouseMiddle` held | `startPanning`: `P0 = player.getWorldPosition(mouse)`, `C0 = camera.currentPosition`; on move: `setCameraPos(P0 − worldPos(mouse with currentPosition = C0) + C0)` (`:150-172`) |
| tilt | `Up`/`Down` | only in screenshot mode |
| pause menu | `Esc`, `Pause` | `MAIN.$main.perform("cancel")` |

`gsGridPlayer.getWorldPosition(x, y)` (`L:0x0818bd80.c`, M): unprojects the screen point through the viewport's screen
matrix from the camera eye, intersects the ray with the grid (`gsGrid::intersectGrid`), falling back to the plane y = 0;
returns a world vector. Only panning needs it.

---

## 7. Blockers

| # | Symptom | Cause | Evidence | Port action |
|---|---|---|---|---|
| B1 | loading bar stops, never ends | runtime error in `environment`/`ui`/`game`/`level` top level (or a function it calls) after `$loadingPos` was set below 1 | `loading.script:171-178`; `semantics.md` §9.1 | make the natives below return values; log the error with script and line; a debug watchdog for a stalled `$loadingPos` helps |
| B1a | hang at game 0.95 | `updateCitizenEconomy`: `citizens.count == 0` → `getHomeCount()` used in `<` and arithmetic | `economy.script:98-104` via `game.script:84` | `count` int, `getHomeCount()` → int (0 is fine) |
| B1b | hang at level 0.9 | `setCameraPos`: `getAreaY` valueless in `y − …` | `ui.script:190-191` via `level.script:79` | implement §6.5 or return 0.0 |
| B1c | hang at level 0.9 | `-1.0 / (3.0 * $grid.width)`: float division by zero is an error | `level.script:62`; `values.md` "Division by zero" | `width/length` from the loaded grid |
| B1d | hang at level 0.9 | `updateCompanyValue`: `getGridObject(name).$price` valueless (saves own vehicles) | `player.script:161-162` | run the template companion scripts in `loadGridObject` |
| B1e | hang at level 0.9 | `initLines`: `$grid.lines.children()` / `line.getVehicles()` must be arrays | `ui/linepanel/linepanel.script:1034,1047` | return `[]` until lines load; create line objects only with their natives |
| B1f | hang at level 0.9 (only with customer profiles) | `findCitizen` valueless in `if` | `ui/secondaryview/secondaryview.script:855-856` | return null |
| B1g | hang at environment ≥ 0.9 | `if ($santa = getGridObject(...))` valueless | `environment.script:218` | return null for unknown ids |
| B2 | nothing renders, many follow-up errors | `engine.script` (no `$loadingPos`, so no stall) dies before `:228-243`: no camera, timeline, `addCamera` | `engine.script:214-243` | grid sub-modules, material getters and every `new_*` constructor must yield objects |
| B3 | stuck on the loading screen with a full bar | `Completed` waits for the "Spielen" click | `loading.script:190-208` | click it (`--at`) at ≈ (W−110, H−56) |
| B4 | screen stays black after "Spielen" | error in `enter Game` before `MAIN.$fade.blend(0.0, 0.2)` (needs `MAIN.$camera`, `$scene`, `$player`, `$playTimeClock`, `$mainClock`, `$ambience.resumeGame` → `new_music` object) | `main.script:807-829`; fade set to 1 at `loading.script:205` | make these exist; check the log |
| B5 | `LoadNext` never runs | `loading.script` top level died before binding the `enter` handlers (e.g. `getTextHeight()` valueless at `:105`) | `loading.script:105,164` | `getTextHeight` returns a number |
| B6 | buildings/roads missing, "Unknown object" | template not registered before `loadFromFile` (envset error, missing file) | `L:0x08146470.c:303-315` | check the env-set loads |
| B7 | map loads but no geometry near the camera (faithful renderer only) | buckets activate only around cameras added with `addCamera` and only while the scene ticks (`enabled` from `Game`) | §6.6 | tick the scene; honour the camera set |
| B8 | `LoadError` right after the dialog | `checkCampaign`: `$saveVersion` undefined or `$filename` missing | `main.script:343-348` | `file_exists` over the merged VFS |

Valueless is the port's default return of an unimplemented method and of an unset `var` field (`port/src/engine/api.cpp:12-58`);
in the engine a valueless operand in arithmetic, comparisons or `if` is a runtime error (`docs/script-runtime/values.md`).
Constructors, sounds, music, particle emitters, effects and simulation setters can be inert objects / no-ops.

---

## 8. Port checklist: click a save → map on screen with a working camera

1. **Menu → GameLoading.** Run the decoded guards/actions (§1); verify `MAIN.$campaign`, `MAIN.$scenario`
   (`$scenario/$location/$default`) and `MAIN.$gridFileName = userdata/saves/<n>/map.grid` after the click.
2. **Loading machine.** `load "loading" :: path` with group `loading` paused at creation, `resume`/`pause`, per-frame
   `$loadingPos` polling, bar updates; log every runtime error with its script, line and current `$loadingPos`.
3. **`engine.script` objects.** Scene (enabled flag gates ticking), both clocks, sky fog fields, sun + light, grid with
   sub-modules and all materials, timeline object, floating camera with all fields, `moveToFirst`, `addCamera`,
   listener fields, viewport `clearColor`/`maxDistance`; shadow/bloom effects as inert objects (or settings `$shadows = 0`,
   `$bloom = false`).
4. **Templates.** `loadGridObject` registers the template and runs its `.script`/`.properties` at once (fields, `$price`,
   `displayName`); 3-arg π/4 rotation + alias, 5-arg scale; `getGridObject` → template or null. Meshes may load lazily.
5. **Value-returning stubs** for B1a–B1g and B5; everything else in §4.3/§5 can stay a no-op.
6. **Map load.** `grid.loadFromFile`: header, terrain, network items and names; set `width/length/scale`; skip vehicles,
   characters, lines and citizens for now; unknown types → `Unknown object` and skip. Timeline natives may stay stubs for
   saves.
7. **Camera.** `setCameraPos` path with `getAreaY` (§6.5); `gs3DCamera.reset`; `gsFloatingCamera` update (§6.4, snapping is
   acceptable at first); eye/look/up and projection per `scene-basics.md` §2.1-2.2 with near = camera `minDistance`, far =
   min(camera `maxDistance`, viewport 600), fov 40, aspect = window.
8. **Game enter.** Click "Spielen"; `viewport.camera/scene/visible`, `scene.enabled`, fade-in. Check: after ~0.3 s the fade
   is gone and the frame shows the 3D viewport behind the HUD.
9. **On-screen 3D viewport.** Root output order (main viewport first, GUI second), colour + depth clear, scene pass with
   fog (germany 0xCCD8BB, 300–1500) and the sun at 14:30.
10. **Grid geometry.** Terrain (terrain/field/yard materials), water, edges, and item instances from template meshes at
    their cell positions and orientations, at least for the buckets within ~400 units of the camera focus
    (`terrain.md`, `grid-items.md`). Check: around `(-120, -40)` the save's roads and buildings appear where the
    minimap / original screenshots show them. **Done in the port** (Cim2 starts at an amusement park by a pond).
11. **Input.** Grid player active in the desktop chain; key names `W A S D Q E Left Right MouseWheelUp MouseWheelDown
    MouseMiddle`; `keyRepeat` for zoom; edge strips' `onMouseOver/onMouseOut`; `$desktop.mouseX/Y`; then
    `getWorldPosition` for panning. Check: WASD moves 150 units/s at zoom 150, the wheel zooms between 75 and 500 with
    the pitch change, the arrows rotate in 45° steps.

---

## 9. Open questions

1. ~~Grid geometry~~ — resolved in [`terrain.md`](../renderer/terrain.md) and
   [`grid-items.md`](../renderer/grid-items.md); the port renders a loaded save with them.
2. The scene scale `S` in the bucket radius (`grid.vfunc118`) and whether it is 1 in game (inf: yes).
3. `FUN_10085c10` argument roles (slerp toward the target with weight `f` on the current value) are inferred from the two
   linear helpers; the decompile is garbled (M).
4. `getAreaY` item fields `+0x3c` (height in 1/256 cell) and `+0x30` (base y) are inferred from the arithmetic (M).
5. The hang in B1 is derived from the documented error semantics and the polling loop; not yet observed in the original
   (inf, H).
6. `getWorldPosition` input units (pixels of the grid player control) and the fallback-plane branch are M.
