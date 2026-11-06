# Weather mod (`opencim.weather`): requirements, decisions, state

The owner's specification for the weather mod, collected from the design sessions of 2026-11-06, and where the work
stands. Read this before continuing on the mod. The mod's own README (`port/data/mods/weather/README.md` on the work
branch) describes its files and switches.

## Where the work is

- Branch `worktree-agent-a7313af0d186ce799` (worktree `.claude/worktrees/agent-a7313af0d186ce799`), **not merged** into
  `construction-tools`. The owner reviews it before merging.
- Commits: `54edd6f` (puddles the owner liked + engine hooks + CC0 sounds), `283e73f` (back to known-good transitions),
  `baf2910` (uniform wetting/drying — **the accepted rain base**), `4d983b7` (snow first version, seasonal rain table),
  later commits: the snow round (tracks, banks, trails, shelter, slowdown) — check `git log` on the branch; anything
  marked WIP continues next session.
- Screenshots of each round: `port/run/weather/` in that worktree (git-ignored).

## Principles (owner's decisions)

- **Weather and seasons are a mod**, not part of the base port. Shipped with the port in `port/data/mods/weather`,
  disabled by default, enabled in `<user root>\OpenCiM\mods\mods.json`.
- Mods only use **generic engine hooks** (documented in `port/docs/mod-manager.md` §4.2–4.3); no game files are
  modified or shipped; shaders are only adjusted in memory.
- **Visual quality rules** (learned the hard way): transitions are smooth and slow; nothing flashes, pops or brightens
  (wet ground is never brighter than dry); **no blotchy / round / noisy patterns** anywhere (the owner rejected blotchy
  wetting, round spray and "blobby" looks); colours stay natural (wet = same hue, darker, slightly more saturated; no
  grey/blue cast over the scene). Change one thing at a time and commit known-good steps, so a version the owner liked
  can always be restored. Show before/after crops at normal zoom.
- Verify visual transitions with a 60 fps recording (ffmpeg gdigrab, see memory `test-windows`) and per-frame
  brightness curves, not single screenshots. One game window at a time.

## Rain (accepted, base `baf2910`)

- **Drops very fine**: about 1 px streaks at normal zoom, faint, see-through; density makes the rain, not size.
- **Intensity builds gradually**: a spell starts with a few drops and grows; tapers off at the end.
- **Season table** (per month: chance per day, typical length, heavy share) in the weather script. **Less rain in
  summer** (about half of spring/autumn; heavy rain and storms may still happen in summer); most rain in spring and
  autumn; in winter rain only outside the snow cold spells.
- **Heavy rain** (rarer, about 1 in 5–6 spells): denser, darker light, closer grey fog, wetter ground, stronger spray,
  **thunderstorm**: soft lightning flashes at least ~10 s apart (never a strobe, capped at fast forward), thunder
  0.5–6 s after the flash by distance (close crack / clap / distant rumble). Open: a lightning bolt in the sky.
- **Getting wet** (accepted after several rounds): first the asphalt darkens **evenly** (one value per surface, no noise
  pattern), colour = dry colour ×~0.8 and slightly more saturated, grass/earth ×~0.85; the sheen comes **much later**
  and builds slowly; puddles appear only after a while of steady rain, driven by **accumulated water**, not by the
  intensity; drizzle barely forms puddles; heavy rain sooner.
- **Puddles** (owner: "the puddles are great" — do not change their look): small, irregular, scattered patches (car
  length at most), mostly in the **gutter on the road side of the kerb** (roads lie lower than pavements), fewer in lane
  centres; **never connected across the kerb** between road and paved areas (separate masks per surface, faded at the
  kerb); only on streets and paved/asphalt places, **not on grass** (grass only darkens); they reflect the sky; rain
  ripples while raining.
- **Drying**: the sheen fades first; puddles shrink in place from their edges (gutter ones last); the darkening lifts
  slowly and evenly; pavements dry before roads.
- **Spray behind vehicles**: a soft, low, elongated mist (fine stretched particles, no visible circles), denser with
  speed, fades within one or two car lengths; tyre hiss sounds near the camera. Open: more visible on fast cars.
- **Sounds**: CC0 sounds from freesound.org (downloaded with the owner's approval 2026-11-06), with `LICENSES.txt`:
  light rain loops, heavy rain, three thunders, wet-road car sounds. The heavy-rain recording with built-in thunder is
  not used (its thunder would not match the flashes). No downloads without asking the owner.

## Snow (in progress)

- **When**: mainly **December, January, February**, as **cold spells of 1–4 weeks**; rarely short ones in late November
  or early March. Several snowfalls within a cold spell; between them the snow stays (no melting while cold). Outside
  cold spells: normal rain schedule. Deterministic per save; `REGS_DEBUG_WEATHER=1` logs the winter plan.
- **Falling snow**: fine, slowly drifting flakes, starting lightly and building up.
- **Accumulation**: slowly builds up on upward-facing surfaces (ground, grass, pavements, roofs, tree foliage,
  platforms); walls and steep faces stay clear; starts finely uneven (never blotchy), then an even cover. Snow colour:
  below pure white, lit by the scene light and shadows, ground texture faintly showing through, cool tint in shade;
  **only snow-covered parts change colour** (buildings keep their colours) — an early version was blown-out white and
  washed out the scene.
- **Parked cars** accumulate snow on roofs and bonnets; moving vehicles stay mostly clear.
- **Streets depend on traffic**: quiet streets keep more snow, busy streets less (a decaying count of vehicles per lane);
  **tyre tracks**: two dark strips per lane at the wheel positions, following the lane through curves, stronger with
  traffic; slush on busy streets. **Snow banks** next to the streets along the kerbs / road edges (whiter, thicker
  look), melting last.
- **Pedestrian tracks**: trodden grey paths on busy pavements, faint trails from single walkers that fill in with new
  snow, desire lines across squares and parks (a low-resolution trodden map from the citizens' walks).
- **Sheltered areas**: no snow (and no rain wetness or puddles) on surfaces with something above them — under market
  tents/stalls, in building gateways (Einfahrten). Via a top-down occlusion map (depth from straight above). **Snow
  reaches about 1 m in** under the edge (a fade from the open edge), rain wetness about half a metre; never puddles
  under shelter. Roofs and tents themselves do get snow on top.
- **UI overlays never get snow or wetness**: the transport lines' route strips with arrows keep their colours, as do
  catchment circles, construction previews, selection markers and the data layer — the hooks apply only to real
  surface materials.
- **Thaw**: when the cold spell ends the snow melts over a day or two **into puddles** (the rain mod's water), which
  then dry as after rain; roads clear first, then pavements, then grass and roofs; banks last.
- **Sound**: quiet; the city ambience a little muffled; no new sound without the owner's approval.

## Gameplay setting: slower traffic

- Mod option **"Verkehr bei Schnee und Starkregen verlangsamen" / "Slow traffic in snow and heavy rain"**
  (`$regsWeatherSlowTraffic`, default on), shown in a **Mods** section of the settings menu (the first mod options
  page).
- Road traffic **including buses and trams** slows down: heavy rain ~0.8 of normal speed, snowfall ~0.7, snow-covered
  roads after snowfall ~0.85 until cleared or melted; smooth ramps. Metro, trains, boats and helicopters unaffected.
  Generic engine hook (a road-traffic speed factor); must not trigger jam relief falsely.
- **News ticker**: one message when the slowdown starts (e.g. "Glätte und Schnee: Der Verkehr und der öffentliche
  Nahverkehr sind langsamer unterwegs." / "Snow and ice: traffic and public transport are slower."), the same for
  heavy rain, and one when it is over — once per weather event.

## Later (same mod)

- Seasons: autumn colours (grass, fields, foliage), bare deciduous trees in winter, spring green.
- Optional gameplay effects (more transit riders in rain: hook `MAIN.$weather.$rain`, off).
- Puddle reflections of buildings and real-time reflections with the planned new renderer.
- A clean frame-time measurement with no other windows open; the surface map's memory on the largest maps (~37 MB on
  Vienna).
