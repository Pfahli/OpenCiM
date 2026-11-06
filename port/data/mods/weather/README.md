# Weather (`opencim.weather`)

A built-in OpenCiM mod: rain spells by game date. It ships with the port in `data/mods/weather` and is **disabled by
default**. Seasons (autumn colours, winter, snow) are planned in this same mod.

## Enable

Add its id to `mods.json` in the user mods folder (`Documents\OpenCiM\mods\mods.json`; create the file if missing):

```json
{ "enabled": ["opencim.weather"] }
```

It takes effect at the next start. To turn it off, remove the id (or start with `--no-mods`).

## What it does

* **Schedule.** Each game day has a 12 % chance of a rain spell (about 3.6 a month) that starts at a random hour,
  lasts 8 to 40 game hours with an intensity of 35-100 %, and fades in and out over 1.5 game hours; 45 % of spells
  bring thunder. The draws are a hash of the game date and a seed from the campaign's start date, so a game gets the
  same weather each time it is played from a save. The shown intensity follows the schedule over at least 3 real
  seconds, so changes do not jump at high game speed. The log says `weather: rain starts at <date>` and
  `weather: rain stops at <date>`.
* **Rain.** One particle emitter under the scene with the port fields `regsEmitBox` (a box around the view: a third
  of the way from the look-at point to the eye, 0.8 × the camera distance wide, from the ground to 60 % of the eye
  height) and `regsStretch` (streaks along the fall). Fine streaks about a pixel wide and 15-20 pixels long at the
  look-at distance, slightly translucent (additive alpha, `effects/particles/lightsmoke.dds` from the game), up to
  about 8 000 at full rain (about 3 900 drawn per frame at the usual zoom); their size, speed and number follow the
  zoom, and their speed and life are divided and multiplied by the particle engine's `timeSpeed`, so the rain looks
  the same at every game speed. Hidden in the underground view. CPU cost: about 0.2 ms particle simulation and
  0.5 ms collection per frame (`REGS_DEBUG_PARTICLES=1`).
* **Wet look.** The scene's atmosphere fields: less direct sun (× 0.65 at full rain), more ambient (× 1.4), greyer
  light, a grey fog tint with the fog 40 % nearer, surfaces 18 % darker and specular highlights × 2.5 (roofs, roads
  and walls whose materials have them).
* **Sound.** A looped rain hiss and occasional distant thunder (every 9-29 real seconds in thunder spells above
  half intensity) through `new_music`, so the Ambience volume slider applies; they stop in the pause menu with the
  scene. The sounds are procedural (filtered noise, `regs-tool synth`), made into `sounds/` by the build; no
  recorded audio ships.

## Files

| File | Purpose |
|---|---|
| `mod.json` | the manifest (`docs/mod-manager.md` §2) |
| `extend/metro/scripts/game.script` | adds the weather script to the game's `$gameScripts` (loaded and unloaded with the game) |
| `scripts/weather.script` | schedule, rain emitter, wet look, sounds; an empty `updateERA` (game.script calls it on every game script at each new year) |
| `sounds/rain.wav`, `sounds/thunder.wav` | generated at build time (not in the source tree) |

## Switches

* `REGS_TEST_WEATHER=rain`: always raining, with thunder; `=dry`: never; `=cycle`: 12 game hours of rain from 06:00
  every second day (start and stop tests).
* `REGS_DEBUG_WEATHER=1`: the scheduled and shown intensity every game hour, and each thunder clap.

## Open

* Gameplay effects stay off. The hook is `MAIN.$weather.$rain` (0..1): a later option could raise transit ridership
  or slow walkers in rain from there.
* Puddles and real reflections need the new renderer; the wet look is a colour and gloss change.
* Seasons and snow: the same structure (a `$season` next to `$rain`, terrain and tree colour shifts through
  in-memory shader adjustments).
