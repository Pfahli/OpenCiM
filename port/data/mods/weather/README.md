# Weather (`opencim.weather`)

A built-in OpenCiM mod: rain, heavy rain and thunderstorms by game date, wet streets with puddles that dry after the
rain, spray behind vehicles, and the sounds of all that. It ships with the port in `data/mods/weather` and is
**disabled by default**. It also brings the seasons: snow in winter, autumn colours and falling leaves, bare trees,
spring green and the summer heat.

## Enable

Add its id to `mods.json` in the user mods folder (`Documents\OpenCiM\mods\mods.json`; create the file if missing):

```json
{ "enabled": ["opencim.weather"] }
```

It takes effect at the next start. To turn it off, remove the id (or start with `--no-mods`).

## What it does

* **Schedule.** Each game day has a 12 % chance of a spell starting at a random hour. Normal rain lasts 8-40 game
  hours at 35-100 % intensity; about one spell in six is heavy rain (one in three in June-August), 3-10 hours, most
  of them thunderstorms. Spells fade in and out over 1.5 game hours, and the shown intensity follows over at least
  1.5 real seconds. The draws are a hash of the date and a seed from the campaign's start date, so a game gets the
  same weather each time. The log says `weather: rain starts at <date>` / `stops`.
* **Rain.** One emitter in a box around the view (port fields `regsEmitBox`, `regsStretch`): very fine, faint
  streaks, about 9 000 at full rain, more and faster in heavy rain; their size, speed and number follow the zoom and
  the game speed. Hidden in the underground view.
* **Light.** Overcast: less sun, greyer light, a darker grey fog a little nearer; heavier still in heavy rain. Nothing
  in it brightens the ground as the rain sets in (the onset was measured: the ground's brightness only falls).
* **Ground** (the shader hook `shaders/wet.glsl`, `docs/mod-manager.md` §4.3):
  * the wet film darkens each surface evenly: wet asphalt is the dry colour, about 0.8 as bright and a little more
    saturated, the same hue; grass and earth about 0.9; pavements dry sooner than roads; no pattern while wetting or
    drying;
  * puddles grow from the standing water: first and most in a strip along the kerb on the carriageway side (the
    gutter), a few small ones in the lanes and on pavements and squares, never across a kerb (each surface has its
    own noise and every puddle fades out at its surface's edge, found in the engine's surface map); still water with
    the grey sky in it and rain ripples; they dry from their edges, the gutter's last;
  * a subtle sheen that comes with the standing water;
  * the whole ground effect never ends up brighter than the dry ground (except in a lightning flash).
* **Spray.** A soft, low, stretched mist behind moving vehicles while the roads are wet (`gsGrid.regsVehicleEmitters`),
  more behind fast vehicles, stronger in heavy rain.
* **Thunderstorms.** Lightning: one soft flash of sun and ambient (never a white-out), 10-60 real seconds apart and
  never closer than 10 s whatever the game speed. Thunder follows after 0.5-6 s by distance: near ones loud with a
  sharp crack, middle ones a rolling clap, far ones a low rumble; pitch and volume vary.
* **Sound** (Ambience group): light, steady and heavy rain loops cross-faded by intensity, wet traffic noise while
  the streets are wet, thunder; the wet tyre hiss of vehicles near the camera (`gsGrid.regsVehicleSound`, Effects).

## Sounds and credits

All sounds are recordings from freesound.org under CC0 1.0 (public domain); `sounds/LICENSES.txt` lists title,
author and page for each: light rain (soundrecorder7), slowly raining loop (unfa), heavy rain on a roof (Q.K.),
thunder blast (bajko), thunder clap (Dave Welsh), distant thunder (lennyboy), a car on a wet road (Breviceps), cars
in a rainy street (BonnyOrbit). The build cuts seamless loops from them (`regs-tool cut`: `rain-light.wav`,
`rain-normal.wav`, `rain-heavy.wav`, `wet-traffic.wav`, `thunder-mid.wav`) and draws the particle sprites
(`regs-tool softdot`: `textures/softdot.dds`; `regs-tool leaf`: `textures/leaf.dds`).

## Files

| File | Purpose |
|---|---|
| `mod.json` | the manifest, with the shader hook |
| `extend/metro/scripts/game.script` | adds the weather script to the game's `$gameScripts` (loaded and unloaded with the game) |
| `scripts/weather.script` | schedule, rain, light, ground model, spray, storm, sounds; an empty `updateERA` (game.script calls it on every game script at each new year) |
| `shaders/wet.glsl` | the ground: wet film, puddles, ripples, sheen; snow |
| `shaders/seasons.glsl` | the foliage of deciduous trees and bushes through the year (also in the shadow passes) |
| `shaders/heat.glsl` | the heat shimmer (a post hook, only on hot days) |
| `sounds/*.mp3`, `sounds/LICENSES.txt` | the CC0 recordings |

## Switches

* `REGS_TEST_WEATHER=rain` (normal rain all the time), `heavy`, `storm` (heavy rain with lightning, the first flash
  after 3 s), `dry` (never), `cycle` (12 game hours from 06:00 every second day, alternating rain and storm: start,
  stop and drying tests), `snow` (cold and snowing all the time), `snowmelt` (two cold days with a day of snow from
  the start, then the thaw), `heat` (a heat spell all the time, dry: the shimmer in the afternoons).
* `REGS_TEST_SEASON=<day of the year>` fixes the foliage's day (0 = 1 January; e.g. 200 summer, 290 turning, 305
  falling, 340 bare, 105 budding, 125 spring green); `<day>+<n>` lets it run on n days per real second (transitions).
* `REGS_TEST_HEAT=<0..1>`: a fixed heat shimmer (with `REGS_TEST_WEATHER=heat`; the light and haze follow it too).
* `REGS_DEBUG_SEASONS=1`: the foliage shows how the hook sees it (deciduous leaves red, twigs yellow, conifers, bark
  and the rest blue); `=2`: the heat shimmer's strength in red.
* `REGS_DEBUG_TRAFFIC_MAPS=1` (engine): where the parked cars are, walkers and trodden cells, the busiest lane, the
  snowplows and their plowed lanes. `REGS_TEST_PLOWS=<n>`: that many snowplows while the mod asks for any;
  `REGS_DEBUG_FOLLOW_PLOW=<n>` keeps the camera on the n-th plow.
* Settings (settings menu > Mods): Enable weather, Enable seasons (off: no cold spells, heat spells, autumn colours,
  bare trees, spring green or falling leaves; the summer look within seconds), Enable day/night cycle, Slow traffic in
  snow and heavy rain.
* `REGS_DEBUG_WEATHER=1`: the scheduled and shown intensity, wetness, water and puddles every game hour; `=2`: the
  ground shows the surface map (roads blue, pavements yellow, platforms magenta, grass green, edges red); `=flash`:
  each flash held at its peak for 3 s (screenshots only).

## Snow (winter)

* **Cold spells:** one to three per winter, 1-4 weeks, December to February (rarely a short one in late November or
  early March), planned per winter from the seed and logged with `REGS_DEBUG_WEATHER=1` (start, end, snowfalls).
  Inside them snowfalls replace rain; the snow stays while it is cold; after a spell a thaw melts it within a day or
  two into the film and the standing water, so it ends in the rain's puddles (gutter first), which then dry.
* **Falling snow:** fine soft dots drifting slowly with a little wind; light snowfall builds up like rain.
* **Cover:** on upward faces of the ground, grass, pavements, roofs, foliage, platforms and parked vehicles (walls and
  moving vehicles stay clear); below full white (about 0.87 in the sun), a cool tint in shade, two light levels (sun
  and shade) so the texture below only shows as a faint modulation; buildings keep their colours. On trees the snow
  follows the slope only loosely and settles needle by needle on the upper sides of the twigs (no flat bands with
  straight edges on the cards), the green showing between.
* **Streets:** the engine's lane traffic decides how much snow stays: quiet lanes white with faint tyre tracks, busy
  lanes grey slush with dark tracks at the wheels (along the lanes, following curves); banks of snow along the kerbs on
  both sides, brighter, melting last; the thaw clears roads first, then pavements, then grass and roofs.
* **Snowplows:** while snow lies and it is not thawing, 6 plows (the base game's SUV in orange with a blade, push
  arms and a flashing amber beacon the engine builds) take long trips through the city; the lane behind a plow is
  bare dark asphalt with a low rim at the strip's edge, filling in again with new snow (about 10 minutes of heavy
  snowfall); each pass over a snowy lane adds to a windrow on the kerb: higher than the little hills, chunky, lit by
  the sun, a little grey from the road. Debug view: plowed lanes orange, their heap white.
* **Pedestrians:** trodden grey paths where people walk (the engine's trodden map), single walkers faint, filled in by
  new snow.
* **Shelter:** no snow, wetness or puddles under roofs, tents and passages (the engine's top map); snow reaches about a
  metre in under an edge, rain about half a metre. Tree crowns are no shelter: snow lies under the trees.
* **Overlays** (route strips, highlights, previews, markers, the catchment layer, people, the sky) are never touched.

## Seasons

* **Foliage** (`shaders/seasons.glsl`, the engine's foliage class 9: every material the trees' wind shader draws). All
  the base game's trees and bushes share one texture atlas; the hook tells its parts apart by the texture coordinate
  and the atlas texel: the two conifers (`tree04`, `tree05`) keep their needles all year, bark, tree pits and grass
  stay as they are; the grey leaves of the deciduous clusters change. Each tree has its own number from the engine
  (`RegsSurface.uv1.y`), so trees are up to a week apart and each has its autumn colour; each leaf (an 8-texel cell of
  the atlas) has its own moment, the leaves that turn first fall first.
  * **Autumn:** from about 19 September each leaf turns on its own from green through a yellowish green to its
    tree's colour: orange on most trees, golden yellow on some, red on a few, a little brighter than the green (by
    hue, never through a dull olive); from late October they go a little browner and fall, bare by late November.
  * **Winter:** bare. A leaf card whose leaves have fallen shows the atlas's bare bush (brown twigs) at the same place,
    so a winter crown is a net of branches; snow settles on them. The shadows follow (the hook runs in the shadow
    passes too): bare trees cast thin branch shadows.
  * **Spring:** the leaves come out from early April (full by the end of April), bright yellowish green, deepening
    to the summer green by mid June.
* **Falling leaves** (`gsGrid.regsTreeEmitters`): small leaf blades (`textures/leaf.dds`, drawn by `regs-tool leaf`
  with a crisp edge, no glow) in muted ochre, orange, rust and brown drift and swing down from the crowns of
  the deciduous trees near the camera (mid October to late November, most at the start of November; storms shake off
  more), lie under the tree for 45 s and fade away.
* **Summer heat:** one to three heat spells per summer (10 June to 31 August, 3-9 days, no rain), afternoons of
  30-35 °C. Above 26 °C a heat shimmer (`shaders/heat.glsl`, a post hook: the scene read a fraction of a pixel away
  along a slowly boiling turbulence anchored on the ground) makes edges over the asphalt wobble gently, less over
  pavements and grass, fading with distance; a slightly warmer, stronger sun and a pale warm haze in the distance. Only
  while it shows does the engine run the extra screen pass (`gsScene.regsPostHooks`).

## Status

Requirements and decisions: `port/docs/weather-mod.md` (construction-tools).

| Feature | State | Commits |
|---|---|---|
| Mod loading, built-in mods, `mods.json`, chained extensions | done | f4949d4 |
| Rain streaks, light and fog, rain and thunder sounds | done | f4949d4, 54edd6f |
| Shader hooks, surface classes, surface map | done | 54edd6f |
| Puddles (gutter strip, per surface, never across a kerb), ripples | done | 54edd6f |
| Heavy rain and thunderstorms (flashes, thunder delays) | done | 54edd6f |
| Spray behind vehicles, tyre hiss | done (spray very faint, see open) | 54edd6f |
| CC0 freesound sounds and loops | done | 54edd6f |
| Uniform wetting and drying, wet colour | done | baf2910 |
| Seasonal rain table | done | 4d983b7 |
| Snow: cold spells, snowfall, cover, thaw into puddles, parked cars | done | 4d983b7 |
| Snow by traffic: lane and trodden maps, tyre tracks, banks, trails | done (trails faint with few walkers) | abf78ed |
| Snow colour, overlays excluded, shelter (top map) | done | the last commit of this round |
| Snowplows: cleared lanes, kerb windrows; 18-bit lane numbers (all lanes on large maps) | done | this commit |
| Traffic slowdown in snow and heavy rain, ticker, options page | done | 3648968 |
| Seasons: autumn colours, falling and lying leaves, bare winter trees, spring green, heat shimmer | done | this commit |
| Falling streaks and flakes skipped under shelter | open | - |
| Sleet around freezing | open | - |

## Open

* Gameplay effects stay off. The hooks are `MAIN.$weather.$rain` (0..1) and `$wet`: a later option could raise
  transit ridership or slow walkers in rain from there.
* Optional lightning bolt in the sky: not drawn.
* Seasons: grass and fields keep their colour all year (autumn and dry-summer grass are not done); trees of other
  atlases (DLC, community content) are not recognised.
