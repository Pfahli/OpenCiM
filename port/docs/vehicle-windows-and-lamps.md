# Vehicle windows and lamps at night

## Passengers in the windows (2026-11-24)

Buses, trams and trains whose window glass the lamp-mask search finds (`makeVehicleLampMask`, channel B) show their
passengers as dark figures against the lit cabin.

* **The figures** are the game's own citizens: every citizen model of the base game
  (`metro/objects/characters/<era>/<type>/`, 27 of them), posed in the middle of the idle animation its script crops,
  and seen from eight angles round (front, 45°, side, …). `engine/passenger_atlas.cpp` rasterises them at 4×4 samples
  per texel into one R8 atlas (a row per model, a column per angle, 44×92 texels a figure, two mip levels), once, the
  first frame a scene is drawn. `regs-tool passengers <gameDir> <out.pgm>` writes it as an image.
* **The shader** (`engine/shaders.cpp`, `regs_passengers`): a seat every 1.3 m per side and a standing place between,
  each with its own model, angle and mirror from a hash. Each figure is alive: every 9-25 s it turns 45° one way and
  later back (the two views crossfaded over about a second), and it sways along the car and bobs a little; seated
  figures move half as much and turn half as often.
* **The load in steps the eye can count:** the vehicle's passengers over its capacity, rounded up to 0, 25, 50, 75 or
  100 % (empty only when nobody is aboard), eased over a few seconds and snapped to the step at the end
  (`engine/sim.cpp`). Seats fill first: half of them at 25 %, all at 50 %; the standing places fill from 50 to 100 %.
  An empty car shows nobody.
* **Units:** the atlas has texture unit 15 of its own (`kPassengerUnit`); most vehicle materials fill units 0-5. The
  mods keep units 13-14.
* Without the atlas (no citizen models found) the old plain figures (a disc and a box) stay.
* `REGS_TEST_PASSENGERS=<0..1>` gives every line vehicle that load (screenshots).
* **More glass found** (2026-11-24): the search took only glass areas of 1.5-15 % of the texture, so windows split by
  their bars into small panes (the old trams: 0.5 % each) were left out and those vehicles had no lit cabin. Now panes
  from 0.3 % count too when they are solid rectangles (fill ≥ 85 %, aspect ≤ 8). `regs-tool vehiclelamps` (glass drawn
  blue) over the 145 base-game vehicles: tram-01 to tram-04, bus-01 and bus-03 to bus-07, the double-decker, metro-01
  to metro-04, the water buses and the el-train gained a lit cabin; the cars' masks changed too, but only line vehicles
  use the cabin light. Seen on the player's save at night: seated passengers' heads and shoulders in a lit window
  (`testing/shots/out/cim2b-window.png`).

## Lamps

* **Corrections** (`kLampFixes` in `engine/night_lights.cpp`, by the diffuse texture): the 1920s Berlin tram
  (`tram-01`) has its two roof-corner lamps (one lens on the texture, above the search's height band) and the lamp in
  the middle of the dash; the search had taken the cream panel under the windows for its only lamp (one large glow,
  off centre, far too bright). Checked with `regs-tool vehiclelamps` over the 145 base-game vehicle models: only
  tram-01 changed.
* **No flicker:** the lamp channels of a vehicle mask keep the brightest texel in their smaller mip levels
  (`buildMips(…, 0b0110)`), so a lamp of a texel or two keeps its light at any distance. The shader's boost of up to
  16× from the pixel's footprint, which made up for an averaged mip and flickered as cars moved, is gone. On Berlin at
  night, six consecutive frames: 135 → 106 blinking bright pixels per frame, the spikes (221) gone.
