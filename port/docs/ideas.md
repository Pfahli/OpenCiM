# ReGS idea catalogue

Ideas for after the playable build (README roadmap), collected from the project owner's requests. Each entry says what
it is, what already exists, and the open decisions. Order is not priority.

## Graphics

* **New graphics engine (Vulkan).** Raytracing (shadows, reflections, GI) and DLSS/upscaling. The settings page already
  lists the engine choice (`$regsRenderer`, Vulkan shown as planned). Base: the renderer contract in `docs/renderer/`.
* **High-resolution textures.** Replacement texture packs, loaded like ReGS mods (`docs/mod-manager.md`) without
  touching the game files.
* **Style options.** Colour styles, distance lens blur and tilt-shift exist (graphics page). More can follow.
* **Night lights for the day/night option.** The base game's buildings, lamps and vehicles have no lights (only the
  New York and Tokyo DLC assets carry illumination maps, `docs/renderer/sky-and-night.md`), so the port's night is
  dark. Port-made lights:
  * Street lamps: a light cone or pool on the ground and a glow on the lamp head, placed from the lamp props' meshes
    (the head's position from the model's bounds or a known bone or material).
  * Vehicles: headlights and tail lights (glow sprites at the front and rear of the body from the model's bounds,
    a short light pool ahead), tram and metro interior light.
  * Windows: found from the building textures, by finding the window areas in each facade's diffuse map (dark,
    rectangular, repeated cells; a one-time analysis step that writes a generated illumination mask per texture into
    the port's cache, never shipped as game data). At night the masked windows glow, each window lit or dark by a
    random seed per building and window.
  * Timing: random buildings light up through the evening, most go dark late at night with only a few windows left
    lit, and some light up again in the morning before sunrise.
  * Open: how well the window detection works on the various facades (a manual mask per texture as a fallback for
    the landmarks), the cost of many light pools (a deferred or clustered light pass in the planned new renderer).
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

## Platforms

* **ARM builds.** The port is plain C++20; its one third-party library, miniaudio (audio, `third_party/`), already
  runs on ARM, Linux and macOS. The OpenGL path and the Win32 platform layer are the parts to port (SDL2 would cover
  the windowing).

## Mods

* **Mod manager.** ReGS mods with manifests, load order, script extensions and an options API; designed in
  `docs/mod-manager.md`, including installing mods from a GitHub catalogue (§7.1). Community mods for the original game
  stay out of scope (not loaded by default, `--community-addons`).

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
