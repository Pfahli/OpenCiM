# Mod manager: design

Status: §8 step 1 implemented (2026-11-06, `engine/mods.cpp`; what exactly: §9): mod folders with manifests in the
user and built-in mods folders, the enabled list `mods.json`, VFS layers and chained script extensions, plus the
first port fields for mods (§4.2). The first mod is the port's built-in weather mod (`data/mods/weather`). Goal: one
place to install, order, enable and update **ReGS mods**, in a form that works for a public release that requires
the original game (like OpenRCT2).

**Scope:** the port's own mods. The official DLC in `addons/` keeps loading through the game's addon loader as it
does now. Existing community addons are out of scope for now (decision 2026-11-03: the port will get better mods
of its own); they are neither managed nor fixed, and §2 "Old addons" and §8 step 2 are optional later work.

## 1. What exists today

| Mechanism | Who uses it | How it loads |
|---|---|---|
| **Original addons** | DLC (`addons/london`, `tokyo`, …) and ~30 community mods in a typical install (`Schwebebahn.gs`, `buildmod`, `lineStatisticsPlus.gs`, …) | `metro/scripts/addons.script`: every folder or `.gs` archive under `addons/`, `metro/addons/`, `userdata/addons/` contributes strings (merged into `$strings`), maps, campaigns, scenarios, locations, company logos and scripts (`scripts/environment.script`, game/editor/news scripts). Archives can also mount at an absolute path (chunk `0x01140000`) and so **replace game files** silently. |
| **Port data** | the port itself | `data/` next to `regs.exe`, mounted over the install (`Vfs::setOverlayRoot`). |
| **Script extensions** | the port's graphics page | `data/regs/extend/<game path>`: prefix before `//@base`, suffix after, `//@rename f g` to wrap a function. Only port-written code ships. |

What the original addon system lacks, and ReGS mods get: enabling and disabling without deleting, a load order
(directory order decides today), visible file-override conflicts, versions and dependencies, and saves that know
which mods they need.

## 2. Mod package

A mod is a folder or a `.zip` in the user mods folder `<user root>/mods/` (`Documents\OpenCiM\mods`, since the
port's user folder moved; before that `<documents>/Cities In Motion/regs/mods/` was planned), or a folder in the port's
built-in mods folder `data/mods/` next to the executable (optional mods shipped with the port, disabled by default),
with a manifest:

```json
// mod.json
{
  "id": "de.example.better-trams",      // unique, reverse-DNS style
  "name": "Better trams",
  "version": "1.2.0",                   // semver
  "authors": ["…"],
  "description": { "en_US": "…", "de": "…" },
  "requires": { "regs": ">=0.5", "game": ">=1.0.22", "mods": { "de.example.tram-pack": "^2.0" } },
  "conflicts": ["com.other.tram-overhaul"],
  "loadAfter": ["de.example.tram-pack"], // ordering hints; requires implies loadAfter
  "saves": "required",                   // what a save made with the mod depends on it (§6):
                                         //   "none" (cosmetic), "whenUsed" (content: only if the save uses it),
                                         //   "required" (changes rules or simulation)
  "saveCompat": ">=1.0 <2.0",            // mod versions that load saves made with this version (default: same major)
  "repository": "example/better-trams"   // GitHub owner/name: where updates come from (§7)
}
```

Contents (all optional, paths as in the game's VFS):

| Folder | Effect |
|---|---|
| `files/<game path>` | replaces that file (textures, meshes, sounds, high-res texture packs). Listed as an override so conflicts are visible. |
| `extend/<game path>` | extends a game script (prefix/suffix/rename), as `data/regs/extend` does. |
| `addon/` | a classic addon folder (strings, maps, scripts/environment.script, …), loaded through the game's own `addons.script` so old addons work unchanged. |
| `shaders/`, `effects/` | port post effects and shader overrides (the graphics options can list effects that mods add). |
| `options.script` | settings the mod adds to the options pages (§4). |

(Optional, later) Old addons need no manifest: the manager could list every folder/archive found by the game's three
addon paths as an "addon" entry (id = path, version unknown) so they can be enabled, disabled and ordered too.

## 3. Loading

1. The manager reads `mods.json` in the user mods folder (enabled list and order) and every manifest, checks versions, dependencies and
   conflicts, and sorts topologically (`requires`/`loadAfter`, then the user's order). Problems are shown, not fatal:
   a mod with a missing dependency is skipped.
2. VFS layers, lowest first: install (with its addons, loaded as the game does) → port `data/` → mods (`files/`, in
   order) → userdata. A later layer wins; every overridden path is
   recorded so the UI can show "A overrides B".
3. Script extensions chain: for a game script, the port's extension is applied first, then each mod's in load order,
   each on the result of the previous (renames apply to the accumulated source), so two mods can wrap the same
   function.
4. `addon/` content is handed to the game's addon loader, so map lists, campaigns and environment scripts behave as
   in the original.

## 4. Script API for mods (`MAIN.$regs`)

Stable, versioned entry points instead of patching game scripts where possible:

* `MAIN.$regs.version`, `MAIN.$regs.mods` (the active list),
* events: `onGameLoaded`, `onGameSaved`, `onNewMonth`, `onTick` (throttled), `onMenu`,
* options: `MAIN.$regs.addOption(page, id, type, default, labels…)` (slider, checkbox, choice); the graphics and
  game pages render them in a "Mods" section and store them as `$settings.$mod_<id>_<option>`,
* UI: helpers to add toolbar buttons and panels without editing `ui.script`.

The extension mechanism stays available for everything the API does not cover.

### 4.1 Implemented so far

* `regsMods()`: the enabled mods in load order, each a map `{id, name, version, path, builtIn}` (`path` is the mod's
  VFS root, `/regs/mods/<id>/`, for `load` and file paths).
* A mod's own files are read by their VFS path, `/regs/mods/<id>/<file>` (scripts, sounds, textures).
* `getenv(name)` (a game native) for test switches.

### 4.2 Port fields for mods

Generic engine capabilities added for mods (first user: the weather mod). They are port fields on the game's own
classes; unset means the original behaviour, so nothing changes without a mod.

| Class | Field | Meaning |
|---|---|---|
| `gsParticleEmitter` | `regsEmitBox` (vector) | half extents of a box around the emit point; each particle starts at a random point in it (rain or snow over an area from one emitter) |
| `gsParticleEmitter` | `regsStretch` (float, s) | streaks: the particle is drawn along its velocity, half length = speed × this, `particleSize` wide, turned towards the camera about the motion (0: the original's billboard). With a soft sprite and `particleBlend = 4` this gives fine rain streaks |
| `gs3DCamera` | `regsEyePosition`, `regsLookPosition` (vector, read-only) | the camera's eye and look-at point as the 3D pass draws them (the floating camera's eye is otherwise not visible to scripts) |
| `gsScene` | `regsLightScale` (1) | × the lights' diffuse and specular (direct sun) |
| `gsScene` | `regsAmbientScale` (1) | × the lights' ambient and the light-model ambient |
| `gsScene` | `regsLightDesaturate` (0) | 0..1: light colours towards grey |
| `gsScene` | `regsFogTint` (0) | ARGB: the fog colour is mixed towards this RGB by alpha / 255 |
| `gsScene` | `regsFogScale` (1) | × the fog start and end distance |
| `gsScene` | `regsSurfaceDiffuse` (1) | × every material's ambient and diffuse colour (wet, darker surfaces) |
| `gsScene` | `regsSurfaceSpecular` (1) | × every material's specular colour (glossier; only materials with `MATERIAL_SPECULAR` show highlights) |
| `gsScene` | `regsLightTint` (0) | ARGB: the direct light (diffuse, specular) × this RGB / 255, by alpha / 255 (a warm summer sun) |
| `gsScene` | `regsPostHooks` (0) | > 0: the mods' post hooks (§4.3) run on this scene's map views; a full-screen pass, so a mod sets it only while its effect shows |
| `gsGrid` | `regsVehicleEmitters` (array of emitters) | each emits behind the rear of every moving road and rail vehicle near the camera (at most 24, within the emitter's `maxDistance`, not boats, aircraft or tunnels): `position` is an offset in the vehicle's frame (x right, y up, z back from the rear end), `direction` turns with the vehicle, the owner velocity is the vehicle's, `density` is particles per second at 10 m/s, scaled with the square of the speed. Listed emitters stop emitting at their own place (`engine/particles.cpp`) |
| `gsGrid` | `regsTreeEmitters` (array of emitters) | each emits from the crowns of the trees near the camera (at most 500 nearest within the emitter's `maxDistance`, 200 m when 0; none while trees are hidden or in the underground view): `density` is particles per second and tree (per game second: the grid's tick), from a random point at 45-90 % of the tree's mesh height within 0.7 of its radius. Listed emitters stop emitting at their own place (`engine/particles.cpp`) |
| `gsParticleEmitter` | `regsTrees` (array of strings) | the tree meshes a tree emitter takes, by their `.object` mesh name (`"tree01"`, `"bush02"`; empty: every tree) |
| `gsParticleEmitter` | `regsLand` (s, 0) | tree emitters: a particle that falls to its tree's base height lands there and lies still for this long, fading over the last third (falling leaves) |
| `gsParticleEmitter` | `regsFlutter` (m/s, 0) | a sideways swing and a slow turn while the particle falls (leaves) |
| `gsGrid` | `regsVehicleSound` (a loaded 3D `gsSound`), `regsVehicleSoundGain` (0) | the sound plays as a one-shot following moving vehicles near the listener (at most 5 at a time, each vehicle again after the sound's length plus 2-8 s, faster than 3 m/s, inside the sound's `maxDistance`), volume × gain × speed / 15 (0.3-1), Effects group (`engine/sim_sounds.cpp`) |

The scene fields apply when the scene is drawn (`engine/scene3d.cpp` `applyAtmosphere`), after the location's light
and fog and the port's day/night cycle, so a mod and the cycle do not fight over the light fields. Particle physics
run with the scene's `particleEngine.timeSpeed` (max(1, game speed)); a mod that wants real-time motion divides its
speeds and multiplies its lives by it.

Script natives: `regsMods()` (§4.1) and `regsShaderUniform(name, value)`: sets a `vec4` uniform of the shader hooks
(float → (v, 0, 0, 0), vector → (x, y, z, 0), quat → (x, y, z, w)), applied to every hooked program from the next frame.
`regsShaderTexture(sampler, path)`: a texture of the mod's own for its hooks, read as `uniform sampler2D <sampler>;` in
the hook file. It gets one of the three texture units kept for mods (13-15); returns false and logs who holds them
when all are taken. The port keeps everything within the 16 units every GL 3.3 GPU has (Macs, Raspberry Pi and many
ARM GPUs have no more): materials 0-5, the night lights' light map 6, the shadow map 7, the hooks' maps 8-12 (the
environment cube; the surface classes; the lanes with their lists and cells in one texture; the top map with the
trodden map as its second channel; the footprints with their tile index), mods 13-15 (`render/renderer3d.h`).

### 4.3 Shader hooks

A mod declares GLSL hooks in its manifest: `"shaderHooks": [{"file": "shaders/wet.glsl", "function": "weather_wet_surface"}]`.
Each file is GLSL 3.30 core (no `gl_` built-ins) defining `void <function>(inout vec4 color, RegsSurface s)`; the
engine appends every enabled mod's hooks to all combiner programs (the game's own shaders, translated by
`render/glsl.cpp`) and calls them, in load order, on the final colour (after the game's fog, before the alpha test) in
the colour passes of map views; never in menus. Without a registered hook the programs are exactly the plain
translation. Hooks are fixed at start-up, before any program is built.

* `"shadowPass": true` in a hook's entry: it also runs in the shadow-map passes (`RegsSurface.shadow` = 1; only its
  alpha counts there: alpha at or under the alpha test's reference casts no shadow), so a hook that cuts away
  geometry (bare winter trees) cuts its shadow too. In those passes `pos`, `normal` and `view` are the light's.
* `"post": true`: a post hook, `void <function>(inout vec3 color, RegsPost p)`, run per screen pixel on the finished
  map view (after bloom, before the port's colour styles and lens blur) while the scene's `regsPostHooks` is set.
  `RegsPost`: `uv` (0..1, bottom left), `pos` (world), `view`, `dist`, `depth` (0..1), `sky` (1 where nothing was
  drawn), `time`, `texel` (one pixel in uv). `vec3 regs_postColor(uv)`, `float regs_postDepth(uv)`,
  `vec3 regs_postWorld(uv)` and `vec4 regs_postData(uv)` read the scene anywhere, `vec3 regs_postProject(world)`
  gives a world point's image coordinate (xy) and depth (z, < 0 behind the camera), so a hook can follow a ray
  across the image (the weather mod's reflections); `regs_Fog` is the scene's fog; the surface hooks' files,
  uniforms and the engine's maps and helpers below are there too (`render/glsl.cpp` `postHookFragment`,
  `engine/effects.cpp` `styleFinish`).
* Hook data: a surface hook may set the global `vec4 regs_HookOut` (0..1, 0 by default) to tell the post hooks
  something about its pixel; it goes to a second colour attachment (RGBA8, location 1 of every translated fragment
  shader) of the scene target, resolved with the colour, and `regs_postData(uv)` reads it. Opaque and cut-out draws
  write theirs (0 where no hook set it: cars, people, foliage); of the blended draws only walkable ground (classes
  3-5) does, decals, glass, route strips and particles leave the data under them. One vec4 for all mods: the weather
  mod's wet ground uses it (film, puddle, ripple tilt). Allocated only while a post hook is registered.

* `RegsSurface`: `pos` (world, from the interpolated clip position: correct for terrain, merged items and skinned
  meshes alike), `normal` (world, from screen derivatives, facing the eye), `view` (unit, eye → point), `dist`,
  `fog` (0..1 at this distance: a hook fades its change by `1 - fog`, since the colour is already fogged),
  `surfaceClass`, `time` (the effect timer, s), `item` (below), `uv` and `uv1` (the mesh's texture coordinates 0
  and 1 as the vertex shader received them; merged trees carry (base height, a per-tree number 0..1 from the tree's
  cell) in `uv1`, so a hook can vary trees one by one), `shadow` (1 in a shadow-map pass).
* `vec4 regs_texel0(vec2 uv)`: the material's first texture (texture unit 0, the diffuse map of the game's
  materials) at a texture coordinate, as stored (before the shader's colorize and lighting): a hook can tell parts of a
  texture atlas apart, or read another part of it.
* `surfaceClass` (`GpuMaterial::surfaceClass`, `render/renderer3d.h`): 0 other (buildings, props, vehicles, people),
  1 terrain (grass, earth, sea floor, cliffs), 2 field, 3 pavement (yard cells, and the walkways of road items: the
  materials a texture variation replaces), 4 road (the carriageway materials of road, alley and street-tram items,
  types 1-7 and 10-12, those with the keep-on-variation flag), 5 platform (stations, docks, heliports, PAVEMENT items),
  6 water, 9 foliage (the materials drawn with the trees' wind shader, `metro/shaders/trees/trees.shader`: the leaves,
  needles, twigs and bark of the base game's trees and bushes, one atlas).
* `int regs_surfaceAt(vec2 xz)`: the class of the walkable surface seen from above at a world point, from the map's
  surface map (`engine/surface_map.cpp`: 0.5 m texels painted from the division meshes as they are built, items over
  terrain; -1 outside the map), so a hook can measure the distance to its surface's edge (a kerb, a square's
  border) by sampling around the point. The weather mod's `weather_edges` searches 8 rays out to 2.2 m.
* Classes 7 (simulated vehicles; `RegsSurface.item` = 1 when parked, per draw) and 8 (overlay: route strips,
  highlights, previews, markers, the catchment layer, people, the sky, particles): the hooks never run on class 8.
* `bool regs_laneAt(xz, out traffic, out lateral, out dir)`, `float regs_laneTrafficSmooth(xz)`,
  `float regs_laneSoft(xz, edge, out onLane, out lateral, out dir)` (the lanes near a point by their own Bézier curves
  (`regs_laneCurve`, `regs_laneData`; per 2 m cell a complete list of the lanes near it, `regs_laneCell`,
  `regs_laneListEntry`), `regs_laneSoftTracks(xz, edge, gauge, width2, …)` (also the wheel lines of every lane there), no
  1 m steps), `float regs_coveredSoft(xz, h)`,
  `float regs_footprintAt(xz)` (single steps near the camera, 5 cm texels; switched on with `gsGrid.regsFootprints` =
  the fill-in time constant in simulation seconds, 0 = off and cleared), `float regs_lanePlowed(xz, reach, out heap)`
  (the snowplows of `gsGrid.regsSnowplows` = how many, engine/snowplows.h: how freshly the lane under a point was
  cleared, filling in at the footprints' pace, and the heap pushed out of the nearest lanes up to `reach` m beyond
  them; cleared with the footprints), `float regs_roadEdge(xz)`,
  `float regs_troddenAt(xz)` (engine/traffic_maps.h: the road lanes with their exact lateral offset and direction,
  the recent traffic per lane, where people walked) and `float regs_shelterDepth(pos)` (the top map, painted from
  everything with an area seen from above except overlays and foliage, so tree crowns are no roof: 0 in the open,
  else the distance to the nearest open spot, up to 1.5 m, for snow or rain reaching in under an edge).
* `samplerCube regs_EnvMap` (the game's `textures/envmap.dds`, unit 8) and the hook's own uniforms
  (`regsShaderUniform`). `REGS_DEBUG_WEATHER=2` shows the surface map in the weather mod.

Cost (RTX 3080, 1600×900, the weather mod in rain): within the frame-time noise of the other test windows; the hook
returns at once for class 0 and when the mod's wetness is 0. The seasons hook (class 9 only) and the heat post hook
(only on hot days) keep the game at 60 fps as well, and so do the reflections in rain (a screen-space ray of 22
steps on wet ground pixels; 58-59 fps in the test window against 59 without, the slowest frame of the readout 33-38 fps against 51).

## 5. Manager UI

An in-game page ("Mods", next to Graphics/Audio in the settings menu, built as a port extension): list with
enable/disable, drag order, details (description, version, dependencies, overridden files), warnings, and "restart
required" when the set changes. On the command line `--mods <dir>` (another mods folder with its `mods.json`) and
`--no-mods` (safe mode) for troubleshooting; both exist (§9).

## 6. Dependencies of mods and of saves

**Mods on mods.** `requires.mods` (id → version range) and `conflicts` are resolved at load (§3): missing, too old or
conflicting mods disable the dependent mod with a visible reason; the manager offers to enable or install (§7.1) what
is missing. Dependencies are transitive; cycles are an error shown on both mods.

**Saves on mods.** A save records the mods it depends on, decided by each mod's `saves` field:

| `saves` | Recorded in a save when … | Example |
|---|---|---|
| `"none"` | never | graphics, UI, sounds |
| `"whenUsed"` | the save uses its content: a placed object, vehicle, line or texture variation from the mod (the save writer collects template and model ids per mod) | building and vehicle packs |
| `"required"` | always while the mod is active | rule changes, simulation mods, new transport types |

The list (id, version, `saveCompat`, the source repository) is a `regs-mods.json` entry inside the save package; the
original game only reads the entries it knows, so port saves stay loadable there as long as they use no mod content.
Dependencies of a recorded mod are recorded with it.

**Loading a save:** the load list marks saves with missing or incompatible dependencies. Loading checks the list
before anything is created: a missing or out-of-range mod blocks the load with the list and the offer to enable,
update or install the mods (from the catalogue, on an explicit click); mods active now but not recorded in the save
are allowed only if their `saves` is `"none"` or `"whenUsed"`. Like the sandbox densities, the save-relevant mod set
is fixed for a running game: changing it needs a restart and applies to new games and to saves whose dependencies
still match.

**Multiplayer (later):** all players need the same set of `"required"` mods and the `"whenUsed"` mods the session
uses; the host compares a hash of the recorded list.

## 7. Later phases

* **Online catalogue from GitHub** (prepared now, built later), see §7.1.
* **Native plugins**: C ABI DLL/SO with a narrow interface (renderer passes, simulation hooks), off by default and
  marked as running with full rights.
* **High-res textures**: texture packs as `files/` mods; the renderer needs BC7/large DDS support for them.

### 7.1 Mods from GitHub

Two levels, both plain GitHub, no server of our own:

| Level | Where | Content |
|---|---|---|
| catalogue | one index repository (e.g. `regs-mods/index`), file `index.json` on the default branch | per mod: `id`, `name`, `repository` (owner/name), short description, tags, `regs` version range |
| packages | each mod's own repository, as **GitHub releases** | tag = mod version (`v1.2.0`); asset `<id>-<version>.zip` (the mod folder with `mod.json`) plus `<id>-<version>.sha256` |

Flow in the manager:

1. **Refresh** (on user request, or at most once a day if enabled): fetch `index.json` from
   `https://raw.githubusercontent.com/<index repo>/<branch>/index.json`; for listed and installed mods ask the
   releases API (`https://api.github.com/repos/<owner>/<name>/releases`) for versions; unauthenticated requests
   are limited to 60 per hour, so results are cached in `regs/cache/` with their ETags.
2. **Install / update** only on an explicit click: download the release zip, check the `.sha256`, check `mod.json`
   (id matches, `requires` satisfiable), unpack to `regs/mods/<id>/` (the previous version kept as `<id>.old`
   until the next start succeeds, for rollback), add it to `mods.json` disabled-by-default until the user enables it.
3. Dependencies are resolved from the catalogue and offered together; nothing installs or runs without the user
   seeing the list first. Mods never download anything themselves; the script API has no network access.

What is prepared already: the manifest's `repository` field, versioned install folders, and the rule that only
port-written or mod-written files ship (no game files), which keeps catalogue mods distributable. Needed later: an
HTTPS client (WinHTTP on Windows, a small portable one elsewhere), zip extraction, and the catalogue repository
itself with a pull-request workflow (CI validates `mod.json`, checksums and the version range).

## 8. Order of work

1. Mod folder scan, manifests, `mods.json`, VFS layers and chained extensions (no UI; edit `mods.json`). **Done in
   part** (§9): no `.zip` packages, versions, dependencies, conflicts or `addon/` folders yet.
2. (Optional, later) original addons under manager control (enable/disable/order).
3. In-game manager page.
4. `MAIN.$regs` option and event API.
5. Save records.
6. GitHub catalogue and updates (§7.1); then native plugins.

## 9. Implemented (step 1, `engine/mods.cpp`)

* **Folders.** Built-in mods: `data/mods/<folder>/` next to `regs.exe` (copied by the build like `data/`; the data
  overlay skips `data/mods`, so a built-in mod mounts only when enabled). User mods: `<user root>/mods/<folder>/`,
  where the user root is the port's user folder `Documents\OpenCiM` (`modsUserRoot()`: `host().userFolder`, or
  `<documents>\OpenCiM` before `init_userdata` runs, since mods mount before the first script). `--mods <dir>`
  stands in for the user mods folder (tests), `--no-mods` mounts none. A folder is a mod when it holds `mod.json`.
* **Manifest.** Read: `id` (required; letters, digits, `.`, `-`, `_`), `name`, `version`, `saves`. The rest of §2 is
  accepted and not checked yet. A JSON reader with `//` and `/* */` comments (`engine/mods.cpp`). Broken manifests
  are logged and skipped. A user mod with a built-in mod's id replaces the built-in copy.
* **Enabled list.** `<user mods folder>/mods.json`: `{ "enabled": ["<id>", …] }`, in load order; ids not installed
  are logged and skipped. No file: nothing enabled.
* **VFS layers** (`Vfs::addModLayer`), per enabled mod in load order, after the port's `data/` and before userdata:
  the whole mod folder as `regs/mods/<id>/` (its scripts, sounds, textures, `extend/`), and its `files/` at the root,
  replacing game and port files of the same path (later mods win; the log counts each mod's overrides,
  `Vfs::overrides()` keeps the list). Rescans (`updateFileSystem`) keep the layers.
* **Script extensions** chain (§3.3): `VM::extensionRoots` = `regs/extend/` (the port) then
  `regs/mods/<id>/extend/` per mod; each is applied to the result of the previous one with the same `//@base` and
  `//@rename` rules, so a mod can wrap a function the port already wraps.
* **Script API**: `regsMods()`, `regsShaderUniform()`, the port fields of §4.2 and the shader hooks of §4.3.
* **Build-time assets** for the built-in mods: `regs-tool cut` (seamless WAV loops from a mod's own recordings) and
  `regs-tool softdot` (a soft particle sprite), run by `CMakeLists.txt` `regs_data`.
* **Example**: the weather mod (`data/mods/weather`, README there) extends `metro/scripts/game.script` with one line
  that loads its script into the game's `$gameScripts`, so the game unloads it with the others.
* **Second example, script-only**: Cheats in Motion (`data/mods/cheats`, README there), described in §10.

Not yet: `.zip` packages, version and dependency checks, conflicts, `addon/` folders, `options.script`, the manager
page, the `MAIN.$regs` events, save records.

## 10. Built-in mod: Cheats in Motion (`opencim.cheats`)

A cheat window, script-only (no engine fields, no shader hooks), disabled by default like every built-in mod
(enable: `{ "enabled": ["opencim.cheats"] }` in `mods.json`). Details, the reasons behind the limits and the file list
are in `data/mods/cheats/README.md`.

* **Where:** a magic wand in the game's left bar (between the roster and the settings wheel; the bar grows by one
  button) opens the window as one of the left panels (the HQ panel's place, frame, heading bar and close button).
* **Cheats:** money (+10 000, +100 000, +1 million, a typed amount; `earnMoney(amount, false)`), unlimited money
  (the account held at the original's cap, saved with the game), skip one day, a week or to the next month (one day
  per frame, so every dated event and the monthly accounts run), turn the clock back within the current month,
  complete the next task (the original's F2), maximum reputation, pay off all loans, unlock every vehicle model.
* **How it hooks in** (examples of the extension mechanism, §9): a one-line `game.script` suffix loads its script into
  `$gameScripts`; the `levelbar.script` extension adds the button, re-places the bar after the port's own resize
  handler and wraps `hideOtherLeftPanels` (`//@rename`); the `scenario.script` extension wraps `dateChanged`. Its
  saved state lives in the scenario's `customData` map (keys `opencimCheats…`), which the save writes and the scenario
  scripts ignore, so a save stays loadable without the mod.
* **Turning the clock back** stays inside the month: writing an earlier date fires `onMonth`/`onYear` when they change
  (in either direction), and `onMonth` runs the monthly accounts. Within the month only `onDay` fires; the days already
  passed keep their dated scenario events and the map timeline's changes from coming again until the clock passes
  the latest date reached.
