# ReGS idea catalogue

Ideas for after the playable build (README roadmap), collected from the project owner's requests. Each entry says what
it is, what already exists, and the open decisions. Order is not priority.

## Graphics

* **New graphics engine (Vulkan).** Raytracing (shadows, reflections, GI) and DLSS/upscaling. The settings page already
  lists the engine choice (`$regsRenderer`, Vulkan shown as planned). Base: the renderer contract in `docs/renderer/`.
* **High-resolution textures.** Replacement texture packs, loaded like ReGS mods (`docs/mod-manager.md`) without
  touching the game files.
* **Style options.** Colour styles, distance lens blur and tilt-shift exist (graphics page). More can follow.
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
* **Jam relief for line vehicles.** In the original, trams (and buses) can deadlock each other and the traffic behind
  them backs up for good; the engine only fires `onJammed` after 180 s of waiting (`0x101152a0.c`, transport.md
  §4.3), which `linepanel.script` turns into a ticker message. The port could resolve it:
  1. **Detect:** a bus or tram whose head has not advanced (less than about one unit) for 2 minutes of game time,
     not counting its dwell at a stop, a breakdown or a crash.
  2. **Clear the cars:** the road vehicles that block it, or that wait in the jam around it, turn round one by one
     (one every few seconds): onto the opposite lane of their node (`oppositeNode`) with a new route, or, where no
     turn is possible, they leave the map (fade out like a finished trip).
  3. **Respawn:** if the bus or tram has still not moved 30 s later, it leaves the map (`onUnspawn`, as when a line
     is paused) and spawns again two stops further on, as soon as that platform is free. Its passengers get off at
     the vehicle's last position and plan their trip again from there.
  * The original's ticker message stays; a second message could say that the vehicle was moved.
  * Always on (the owner's decision): no player option; the times are fixed (2 minutes, then 30 s). Only an internal
    switch turns it off, for tests and comparisons with the original (a debug variable such as
    `REGS_NO_JAM_RELIEF=1`, listed with the other debug switches in the README).
  * Open: which cars count as "surrounding" (the cars on the nodes the vehicle waits for and their queues, up to a
    radius), and whether trains and metro need the same (they deadlock less, since they claim whole blocks).
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
    city), written in one go by `writePackage`, so a save is one file to upload or download, and the original game can
    read them.
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
