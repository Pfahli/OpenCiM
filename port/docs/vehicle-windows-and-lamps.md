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
* **Lineup** `REGS_TEST_LINEUP=<x>,<z>[,<load>]`: every bus, tram and metro part the game has loaded, in three rows
  (buses, trams, metros, 40 apart) around that point, lamps and cabin light on, the given load (default full); the log
  lists each part with its x. With an empty map (`REGS_START=map:addons/none/empty.grid`) and `REGS_DEBUG_TIME=22`
  the whole fleet can be checked in a few shots (`testing/shots/out/lineup4-*.jpg`, 2026-11-24: 19 bus, 16 tram and 12
  metro parts, all lit and full).
* **Double-deckers:** the window band comes from the glass's triangles on the model (points spread over each
  triangle, the ones on lit glass), not from the texture, since both decks may use the same panes; a clear gap in the
  heights splits it into a lower and an upper deck band (`windowY2..3`, `regs_CabinUpper`), each deck with its own
  people. Until then a double-decker's figures stood across both floors. Found two decks: the double-decker, bus-07,
  bus-12, tram-02. Left: bus-07's upper windows are found as glass in one pane only.
* **The driver** (2026-11-24, owner's request): the cab is lit at half the cabin light (it was dark), and the
  leading end's windscreen shows a citizen seen from the front, seated on the vehicle's left (the viewer's right). The
  windscreen glass is marked in the mask's alpha (255 out of the model's front, 128 out of its rear, kept through the
  mip levels); the shader takes the end whose lamps are head lamps (the model's front by day). Windscreens are
  glass facing out of an end within the front quarter of the length (old buses have theirs behind a bonnet).
  `regs-tool vehiclelamps` counts the windscreen texels. On a double-decker the cab is downstairs only: the upper deck's front is passenger seats at full light, its front glass marked 64, where the shader draws two passengers seen from the front (the second from half full); glass facing out of an end no longer gets the side windows' rows of figures. Lineup facing the camera: `REGS_TEST_LINEUP=0,0,1,180`.
  Seen with a driver: bus-08, bus-10a, bus-12, bus-13a, bus-14, bus-15, bus80, the double-decker, tram-01, tram-04,
  tram-05, tram-07, metro-03. Not visible yet: the old buses bus-01 to bus-07, tram-02, tram-03, metro-01, metro-02,
  metro-04 to metro-06 (windscreen found but the figure sits outside the glass, or no windscreen found).
* **Driver's cab:** an end counts as a cab when glass faces out of it (a windscreen), not when lamps were found there;
  a pane the texture repeats along the car takes the brightest of its places; ends found at lamp height that span
  less than half the model are not its ends (tram05e). The last car of tram-05 is lit all along now.
* **More glass found** (2026-11-24): the search took only glass areas of 1.5-15 % of the texture, so windows split by
  their bars into small panes (the old trams: 0.5 % each) were left out and those vehicles had no lit cabin. Now panes
  from 0.3 % count too when they are solid rectangles (fill ≥ 85 %, aspect ≤ 8). `regs-tool vehiclelamps` (glass drawn
  blue) over the 145 base-game vehicles: tram-01 to tram-04, bus-01 and bus-03 to bus-07, the double-decker, metro-01
  to metro-04, the water buses and the el-train gained a lit cabin; the cars' masks changed too, but only line vehicles
  use the cabin light. Articulated trams: a middle section without a body at lamp height across its ends (tram05b,
  tram05c) no longer ends the search before the windows; the driver's-cab fade (dark up to 1.5 m behind a cab end, full
  from 4.5 m) shrinks on short cars to 12-30 % of their length (tram05e, 7 m with lamps at both ends, was dark all
  over). The red articulated tram on the player's save now has lit windows with passengers in every section. Seen on
  the player's save at night: seated passengers' heads and shoulders in a lit window
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
