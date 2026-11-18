# ReGS idea catalogue

Ideas for after the playable build (README roadmap), collected from the project owner's requests. Each entry says what
it is, what already exists, and the open decisions. Order is not priority.

## Graphics

* **New graphics engine (Vulkan).** Done on branch claude/vulkan-renderer-903225 (2026-11-17): Vulkan 1.3 next to
  OpenGL behind one device interface, chosen with `$regsRenderer` (graphics page), the same picture as OpenGL. Plan,
  status and what follows: `docs/renderer/vulkan.md`.
* **TAA and render scale.** Done (same branch): temporal antialiasing and a render scale with temporal or spatial
  upscaling, on the new settings page Image quality; both graphics engines (`docs/renderer/vulkan.md` §9.1-9.2). Open:
  per-object motion vectors (vehicles, people), route-strip arrows smear under TAA (texture animation).
* **Raytraced shadows.** Done (same branch): sun shadows by ray queries on Vulkan, option on the Image quality page
  (`docs/renderer/vulkan.md` §9.3), and raytraced ambient occlusion. Open: foliage still from the shadow map (alpha in
  the rays), soft shadows, reflections on water and glass, global illumination.
* **FSR 3 frame generation.** Done (same branch): a generated frame between two real ones on Vulkan with the FSR
  upscaler, presented by the device's own present thread (`docs/renderer/vulkan.md` §9.4). Open: display-timed pacing,
  per-object motion vectors.
* **Raytraced reflections.** Water and glass done (same branch): the game's environment-cube reflections are traced
  against the city on Vulkan, option on the Image quality page (`docs/renderer/vulkan.md` §9.7). The weather mod's puddles are
  traced too. Open: foliage and night lights in the reflections, a sheen on wet streets.
* **HDR output.** Done (same branch): HDR10 output on Vulkan with Windows HDR on, settings on the Image quality page
  (on/off, paper white, peak); the SDR picture's highlights are lifted, menus stay at paper white
  (`docs/renderer/vulkan.md` §9.6). Open: real HDR intensities for lamps, windows and the sun.
* **AMD FSR 3.1 upscaling.** Done (same branch): option "Upscaler: AMD FSR 3.1" on the Image quality page, Vulkan only
  (FidelityFX SDK v1.1.4, MIT, in `third_party/fidelityfx-fsr`; `docs/renderer/vulkan.md` §9.2). Open: per-object
  motion vectors, a reactive mask for particles; FSR 3 frame generation next, then raytraced reflections.
* **Raytracing, frame generation, DLSS, FSR.** Owner's scope 2026-11-17 ("Also add features like Raytracing, Lossless
  Scaling etc"): raytraced shadows and ambient occlusion first, then reflections; frame generation (own interpolation,
  or FSR 3); AMD FSR 2/3 and NVIDIA DLSS upscalers (SDK downloads need the owner's OK). Plan: `docs/renderer/vulkan.md`
  §9.3-9.5.
* **High-resolution textures.** Replacement texture packs, loaded like ReGS mods (`docs/mod-manager.md`) without
  touching the game files.
  * **Done (branch claude/texture-upscale-bcb6d4, `../docs/renderer/texture-upscaling.md`):** "Sharper textures (2x
    upscaled)" on the graphics page: every texture of the player's install upscaled 2× on their machine (built-in
    filter, made once in the background, cached in `OpenCiM\texture-cache`), switched live. An AI upscaler such as
    Real-ESRGAN can be plugged into the generator (`regs-tool upscale-textures --external`); not shipped.
* **High Res UI.** Sharp interface art at UI scale 1.5× and 2× (4K screens): icons, toolbar and menu buttons, the
  cursor, panels, portraits, city seals, company logos, and the full-screen art of the loading screens and the main
  menu, each with a 2× version so the scaled UI shows real detail instead of enlarged texels.
  * **Done (branch claude/texture-upscale-bcb6d4, `../docs/renderer/texture-upscaling.md` §8.3):** part of "Sharper
    textures": the built-in filter makes the 2× UI art on the player's machine, the 2048-wide loading and menu art
    included; at UI scale 1 the GPU still shows the original texels. With waifu2x (MIT) the generator can make the UI
    art instead (`--external-ui-only`), which removes the block noise of the loading pictures and draws cleaner icon
    outlines; tried on the owner's machine, not shipped.
  * Owner's decision (2026-11-17): no separate switch, High Res UI stays part of "Sharper textures".
  * Open: shipping or fetching waifu2x for the UI (the player's GPU, ~2 minutes for all UI art, its runs fail now and
    then and are retried).
* **Style options.** Colour styles, distance lens blur and tilt-shift exist (graphics page). More can follow.
* **Night lights for the day/night option.** The base game's buildings, lamps and vehicles have no lights (only the
  New York and Tokyo DLC assets carry illumination maps, `docs/renderer/sky-and-night.md`), so the port's night is
  dark. Port-made lights:
  * **Done (branch claude/night-lights-bb517b, `docs/renderer/night-lights.md`):** street lamps (pools on the ground
    from a lamp map, glows on the heads found from the lamp meshes) and lit windows (masks from the buildings'
    specular maps, made in memory at load; four random groups per building that go on and off over the evening, most
    dark after midnight, early risers from 5:30).
  * Vehicles: done on the same branch (lamps found on the models' textures, beams as a light map, high beams on free
    roads, lights switching on one by one at dusk). Open: tram and metro interior light; better front-lamp finding.
  * Open: the cost of many vehicle light pools (a deferred or clustered light pass in the planned new renderer).
* **Seasons and weather.** **A mod, rain started** (user's decision: weather and seasons are a mod, not the base
  port): the built-in mod `data/mods/weather` (`opencim.weather`, disabled by default) has rain spells by game date,
  streaks, wet surfaces, grey light, rain and thunder sounds, all through generic port hooks
  (`docs/mod-manager.md` §4.2); seasons, snow and the gameplay touches below are open in the same mod.
  The original's look never changes over the year. The port could follow the game's date:
  * Spring and summer as today; autumn turns grass, fields and tree foliage yellow, orange and brown (a colour shift
    in the terrain and tree shaders by season, per tree species where the textures allow it); in winter deciduous
    trees lose their leaves (foliage hidden or thinned, bare branches from the trunk meshes; conifers keep theirs) and
    snow covers the ground, roofs and trees (a white blend on upward-facing surfaces, cleared on roads and tracks
    where traffic runs).
  * Weather: rain (falling streaks as particles, wet darker ground with reflections, puddles), snowfall, fog days;
    random weather by season and map climate.
  * Gameplay touches as an option: fewer cyclists and walkers in bad weather, more transit riders, slower road
    traffic in snow.
  * Sound: rain and wind ambience from the new mixer; no game data needed.
  * An option on the graphics page (seasons off / seasons / seasons and weather); the shaders are adjusted in memory
    only (`portShaderPatch`), never shipped as copies.
  * Open: bare trees need branch meshes (the game's trees are trunk plus foliage cards; leafless trees could be the
    trunk alone, a generated branch mesh, or thinned foliage), and the maps' climates (Tokyo and New York winters).
* **Wider zoom, clouds and a sky.** The original has no clouds (a `cloudmap.dds` line in `engine.script` is commented
  out and the texture was never shipped) and hides its sky dome, since the camera never reaches the horizon
  (`docs/renderer/sky-and-night.md`).
  * Zoom further out than the original's limit, so a whole city fits on screen (needs the view distance and the
    item/terrain LOD bands to follow, and a far-zoom fallback for the item meshes).
  * Clouds appear above a zoom level: a cloud layer between the camera and the city that fades in as the camera
    rises, drifting with the wind, with matching cloud shadows moving over the map (also visible at normal zoom, as
    an extra). Procedural (noise in a shader), so no art has to ship; an option on the graphics page ("Wolken": off /
    shadows only / clouds and shadows).
  * A proper sky: a procedural atmosphere (sky colour from the sun's height, haze at the horizon, sun glow) instead
    of the plain dome, shown when the camera looks towards the horizon (a wider tilt range as an option) and at far
    zoom; it follows the day/night option (stars and moon at night).
  * Open: the far-zoom limit on the largest maps (performance), how the fog and the sky blend at the horizon, and
    whether the minimap view and the far zoom meet.
* **Stereoscopic 3D mode.** Play in 3D on 3D TVs, monitors, projectors and headsets; a "3D mode" option on the
  graphics page with the output format, a depth strength slider and "swap eyes".
  * Formats: side-by-side (half and full width), top-and-bottom, row-interlaced (passive 3D monitors), anaglyph
    (red/cyan, with an optimised colour matrix such as Dubois), and later frame packing or a VR headset through OpenXR.
  * How: the 3D viewport is rendered twice from two cameras offset sideways (eye separation scaled with the zoom, the
    zero-parallax plane at the camera's look point, so the map sits at screen depth and buildings come out of it);
    shadow maps are shared, bloom and the style effects run per eye, and the depth blur is off or reduced in 3D. The
    HUD is drawn at screen depth in both halves; tooltips, selection arrows and pins get the depth of their object so
    they don't fight with the scene. The corner preview and the minimap stay flat.
  * Cost: about twice the scene work per frame; the planned Vulkan renderer could use multiview to render both eyes in
    one pass.
  * Open decisions: which formats first (SBS and anaglyph are the cheapest to build and test), a comfort limit for
    the depth at high zoom, and how the mouse picks in SBS (one half as the input view).
* **VR mode for Meta Quest headsets.** Play the city as a table-top model or walk through it at street level.
  * How: OpenXR, which the Quest supports over Quest Link / Air Link and Steam Link on a PC; a native Quest build is
    a later, bigger step (Android/ARM, see Platforms). Builds on the stereoscopic mode (two eye cameras, shared
    shadow maps, per-eye effects) but takes the eye poses from the headset at its refresh rate (72-120 Hz), so the
    renderer must hold that frame rate or use the runtime's reprojection.
  * Views: a table-top view (the map as a model in front of the player, scaled and turned with the controllers) suits
    the game best; a street-level view (stand at a stop, ride along with a vehicle, using the follow camera) as an
    extra.
  * Input: the controllers as laser pointers for the HUD and the tools (the HUD on a panel in the scene), grab to
    move and scale the map; hand tracking later.
  * Sound: the 3D sound follows the head pose (the listener), and headphone (HRTF) output fits VR.
  * Open decisions: which views first, comfort options (snap turning, vignette at street level), how the 2D HUD and
    dialogs move into the scene, and the minimum PC performance at Quest resolutions.

## Gameplay and comfort

* **Controller support.** Play with a gamepad (Xbox, PlayStation, Switch Pro, Steam Deck).
  * **Done (2026-11-18), first version: [controller.md](controller.md).** XInput (+ DualShock 4 / DualSense raw HID)
    on Windows, a virtual cursor on the left stick (camera pan at the screen edge or with LB), right stick turn and
    tilt, triggers zoom, the game's keys on the buttons, a Controller page in the controls settings. Still open from
    this list: text entry, radial menus, snapping to buttons, button prompts, rumble, remapping, SDL2 for other
    platforms; not yet tried on a real pad.
  * How: XInput on Windows, or SDL2's game-controller API, which covers all pads and the later ARM/Linux builds
    (`docs/ui/input.md` describes the input path the port feeds today). The left stick pans the camera, the right
    stick turns and tilts it, the triggers zoom; a virtual cursor (stick or touchpad) drives the HUD, the tools and
    selection, snapping to buttons and pins; shoulder buttons cycle the build menu and the line panel; Start opens the
    pause menu.
  * Comfort: radial menus for the build tools and game speed, larger hit areas while a pad is in use, button prompts
    in place of mouse hints, rumble on errors (red placement) as an option.
  * Fits: the Steam Deck (a good handheld target for this game) and the VR idea (Quest controllers use the same
    pointer model).
  * Open decisions: SDL2 (a dependency, also wanted for ARM) or XInput first, how text entry works (line and company
    names), and remappable bindings next to the game's `controls.keys`.

* **Quality-of-life improvements.** To be collected as they come up.
* **Traffic side (left- or right-hand traffic), chosen before a game starts.** The original drives on the right
  everywhere, also on the London and Tokyo maps. A setting in the new-game screen (sandbox and scenario start, stored
  in the save so a game keeps its side) mirrors the traffic:
  * The lanes of every road and tram piece are mirrored at load (lane graph and stop lanes), so cars, buses and
    trams keep to the other side, turn the other way at junctions and give way by the mirrored rule.
  * Bus and tram stops serve the mirrored side; the vehicles' door sides follow: the door paths and the door
    animation of buses and trams are mirrored (model mirrored or its door paths swapped), so passengers board on
    the kerb side.
  * Road markings and stop props that show a side (arrows, shelters) would need mirrored placement or meshes.
  * Open: which pieces carry side-specific geometry (stop bays, tram platforms on one side, motorway ramps), and
    whether the lane flags allow a pure mirror without per-piece data. (Saves need not load in the original game.)
* **Jam relief for line vehicles:** **done** (`docs/simulation/transport.md` §10, `engine/sim_jams.cpp`). A bus or
  tram that has stood 2 minutes (dwell, breakdown and crash not counted) gets the road vehicles of the jam around it
  (within 60 units) turned round onto the opposite lane, one every 3 s, or sent off the map where no turn is possible,
  and the node it waits for kept for it; still standing 30 s later, it leaves the map and spawns two stops on, its
  riders getting off and planning again. The original's ticker message stays, a second one says the vehicle was moved.
  Always on; `REGS_NO_JAM_RELIEF=1` turns it off (README). Metro trains are only watched: their jams ended by themselves
  in every measured case (§10.4); background trains already give up after 60 s.
* **Sandbox densities.** Traffic and population density (0.5×-4×) for new sandbox games: **done** (README, "Port files
  and graphics options").

## Maps

* **Map generator from OpenStreetMap.** Generates a playable map of a real place: streets, terrain, water, trees and
  buildings where they are on real maps, with a scale the player chooses (real metres per map cell, e.g. 1:1 for a
  district or compressed to fit a whole city into the largest map).
  * What there is: the map format is fully decoded and the port writes it (`docs/save-format/`, `grid_save.cpp`, the
    reference parser `tools/gridparse.mjs`); the minimap renderer can create the map thumbnail; street names fit the
    item-name table (`0x07020310`). Output would be the usual set in `userdata/maps/` (`.grid`, `.timeline`,
    `.scenario`, `.dds`), so the maps play in sandbox mode and in the original game.
  * Steps:
    1. **Input:** an OSM extract for the chosen area (`.osm.pbf`, e.g. from Geofabrik or the Overpass API) and an
       elevation model (e.g. Copernicus DEM or SRTM) for the terrain, both fetched with the player's consent.
    2. **Terrain:** heights resampled to the cell grid at the chosen scale; water (`natural=water`, `waterway`,
       coastline) as water cells below the water level.
    3. **Network:** `highway` classes mapped to the game's road types (motorway → express road, primary → large road
       or avenue, residential → small road, footway → alley), `railway=tram/rail/subway` to tram, train and metro
       tracks, bridges and tunnels from the tags; the hard part is fitting the real geometry to the game's grid (8
       directions and the shipped curve and junction pieces), i.e. a simplification and snapping step that keeps the
       topology.
    4. **Buildings:** each OSM footprint matched to a template of similar size and use (residential, commercial,
       industrial, landmark from `building`/`landuse`/`amenity`), turned to the nearest of the grid's orientations;
       remaining plots filled by landuse.
    5. **Trees and fields:** `landuse=forest`, `natural=wood`, single trees, parks and farmland.
    6. **Names and setup:** street names, a city name and a start date for the scenario file.
  * Open decisions: run it as a tool (`regs-tool osm2map`) or as a menu in the game; how far to simplify at small
    scales; OSM's licence (ODbL) requires the credit "© OpenStreetMap contributors" with the map and keeps shared
    generated maps under share-alike terms.

* **Buildings with a year and a construction site (owner's idea 2026-11-07).** In the editor, a placed building
  gets options: from which year it exists, and whether it is preceded by a construction site (a pit with a crane and
  a fence around the footprint). A map can then start tiny in 1920 and grow towards 1960 as its buildings appear.
  * What there is: the editor's timeline already records every placement on the day it was made, and the game
    replays it with half a year of previews (hidden placeholders, the 0x40 construction-site ground, conflict marks;
    `engine/timeline.cpp`); the debug time bar picks the day. What is missing is a way to set the year per object
    without paging through the time bar, and a visible site.
  * How: an "Exists from" field in the object's info panel moves its create action to that date in the timeline; the
    construction site is a set of props (pit decal, crane, fence) the preview phase places around the footprint and
    removes when the building appears. Needs the timeline simulation in games (it runs) and the editor's date bar.

## Platforms

* **ARM builds.** The port is plain C++20; its one third-party library, miniaudio (audio, `third_party/`), already
  runs on ARM, Linux and macOS. The OpenGL path and the Win32 platform layer are the parts to port (SDL2 would cover
  the windowing).

## Mods

* **Mod manager.** ReGS mods with manifests, load order, script extensions and an options API; designed in
  `docs/mod-manager.md`, including installing mods from a GitHub catalogue (§7.1). Community mods for the original game
  stay out of scope (not loaded by default, `--community-addons`).
* **MCP bridge (owner's idea 2026-11-07).** A mod that exposes the running game and the map editor over the Model
  Context Protocol, so Claude or another agent can build and change maps on its own.
  * Tools it could offer: read the map (size, terrain heights, water, items in an area, the timeline), apply editor
    actions (terrain brushes, water, place or remove objects, lay roads and tracks, set the date, save and load maps),
    take a screenshot of the current view or the minimap, and read the game state in a running game (lines, money).
  * How: the edits go through the same tool functions as the mouse (`engine/tools.h`, `tool_terrain.cpp`, timeline
    recording), so undo, the timeline and saving keep working; a local MCP server in the mod (stdio or a localhost
    port) queues the calls and runs them on the game thread between frames.
  * Open questions: safety (local only, off by default, a visible indicator while an agent is connected), how much
    of a map fits in one answer (areas, summaries), and whether it should also drive a running game (lines, vehicles).

* **Template sharing in the OpenCiM Hub (owner's idea 2026-11-07).** A section of the mod manager where players share
  their editor templates. The file format is ready: a .cimtpl is a GS10 package (template.grid, template.dds as the
  picture, template.info with size, terrain flag and the DLC packages of its objects); it is mounted as a folder from
  userdata/templates, and the template window already flags templates whose DLCs are missing.

## Online

* **Multiplayer.** Needs a deterministic simulation (fixed tick, one random stream per system, see
  `docs/simulation/README.md` "For a reimplementation") or a server-authoritative model; open.
* **Cloud saves.** Keep save games in sync between computers.
  * What there is: saves are single GS10 packages (`userdata/saves/<name>.gs`, zlib-compressed, about 3 MB for a large
    city), written in one go by `writePackage`, so a save is one file to upload or download.
  * Options, from simplest to most work:
    1. **Synced folder:** let the player pick a saves folder inside OneDrive, Dropbox, Google Drive or similar; the
       service does the sync. Almost no code: a setting next to `--documents`.
    2. **Steam Cloud:** the game is owned on Steam and ships `steam_api.dll`; the port could use the installed copy at
       run time (Steam Remote Storage) if the app has cloud quota. No extra account for the player.
    3. **A ReGS service or a GitHub repository:** works without Steam, but needs accounts and hosting, and must never
       store credentials in plain text.
  * Open decisions: conflict handling when two computers changed a save (keep both, newest wins, or ask), what to do
    with saves made by the original game, opt-in and privacy (saves contain only game state and the company name).
* **RetroAchievements.** Achievements on retroachievements.org, next to the game's own trophies.
  * What there is: the game already has local trophies ("Auszeichnungen", `metro/ui/menus/trophies.script`) and
    achievement checks in the scenario scripts (`checkAchievements` threads), which show what a set could reward.
  * How: their open-source client library rcheevos (`rc_client`) evaluates achievement conditions on a block of
    memory the game exposes; a native port would publish a small, stable block of game state (date, money, lines,
    passengers, satisfaction, …) for the conditions to read, and the player logs in with their RetroAchievements
    account in the port's options (credentials handled by rcheevos, never stored in plain text).
  * Open questions: whether RetroAchievements accepts a native PC port rather than an emulated system (check their
    rules for standalone integrations), who designs the set (their developers create and review achievement sets),
    and hardcore rules (no achievements with mods or cheats such as the moneyget addon).
