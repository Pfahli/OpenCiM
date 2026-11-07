# Night lights (port feature)

Lit windows and street lamps for the port's day/night cycle (`$regsDayNight`, sky-and-night.md §6.2). The original
game has neither: the European locations have no window or lamp illumination maps at all (sky-and-night.md §4.2).
Vehicles get head and tail lights (§3). Everything is derived in memory from the shipped assets at load time; no game file is changed or shipped.
Code: `port/src/engine/night_lights.{h,cpp}`, the shader patch in `engine/shaders.cpp` (`portShaderPatch`), the
uniforms in `render/renderer3d.cpp`, the varyings in `render/glsl.cpp`.

With the cycle off (and outside `REGS_DEBUG_TIME`) every term below is zero, so the image is the original one.

## 1. Windows

### 1.1 Where the windows are: the specular maps

The building materials (templates under a `buildings/` folder: `metro/objects/buildings`, `addons/*/buildings`) almost
all carry a specular map (`*_s.dds`, 517 of 642 materials), and in it the window glass is the only bright part:
frames, walls and roofs are black or dark grey. So the specular map is the window mask, in the exact shape of each
window, shop front and door glass.

`makeWindowMask` (CPU, at map load):

1. Decode the specular map (DXT, `render/image.cpp decodeRgba`) at ≤ 512 px. Glass level T = half the 99.5th
   percentile of max(r, g, b), clamped to 0.2…0.4; no mask when that percentile is under 0.22.
2. Illumination map: texels it lights (signs) are left out; a map lighting more than 1/60 of the texture means the
   texture brings its own lit windows (the US/Tokyo DLC buildings), so it gets no mask.
3. Glass texels are grown by ~1 % of the width and labelled (4-connected), so the panes of one window, split by thin
   frames, are one window. Areas over 6 % of the texture (glass roofs, sheet metal) are dropped.
4. **Shop windows**: with the building mesh rasterised into texture space, glass that only faces within 4.2 m of the
   model's ground uses goes into its own channel (A). The other windows get one of **three random groups** and a random
   brightness 0.5…1. Each channel of the mask (BGRA8) holds coverage × brightness of its windows (soft edges: smoothstep(T ± 0.07)).
5. Box-filtered mipmaps. Because each channel is plain coverage, the mips stay exact: a distant facade shows the
   average of what is lit, never a wrong window.

Vienna: 129 masks in ~0.6 s at load (worker threads; uploads on the GL thread). `regs-tool windowmask <game>
<.material> <out.png>` shows a mask over the darkened diffuse map, one colour per group.

### 1.2 Which windows are lit

* **Per building**: merged building vertices carry a seed (1…255, a hash of the item's cell position) in the vertex
  colour alpha (`grid_items.cpp`; the default shader never reads vertex colours). Every vertex stage passes it on as
  `regs_Seed` (`glsl.cpp`).
* **Per group**: hash(seed, group) gives each of the four groups a switch point r in 0…1; the group is lit while the
  share of the hour exceeds r (smoothstep over ±0.03, so a group fades over a few game minutes instead of popping).
  Groups go on and off at their own time through the evening and night.
* **Share of the hour** (`windowShare`): 0.45 at 17:30, ~0.57 from 19:00 to 21:00, then people go to bed: 0.48 at
  22:00, 0.3 at 23:00, 0.15 at midnight, 0.05 from 2:30 to 4:30; early risers from 5:30 (0.36 at 6:30).
* **Colour**: per group and building, warm incandescent (1, 0.6, 0.3) … soft white (1, 0.84, 0.62); 6 % cool
  (0.55, 0.64, 0.82, TV/fluorescent). Brightness × (0.7…1.3) per group, × (0.6 + 1.5·luminance of the diffuse
  texel) so curtains and window detail stay visible.
* **Shop windows**: brighter and whiter (1.6 × (1, 0.82…0.95, 0.6…0.88)), on their own schedule (0.9 of them from 17:00
  to 21:00, 0.72 at 22:30, 0.45 at midnight, 0.32 at 3:00), switched per building.
* **Strength** = smoothstep(0.18, 0.72, night01) of the cycle (night01 = 1 − day weight), so windows come on through
  the dusk and go out with the dawn.

### 1.3 Shader

`maps.fragment` (default shader) gets `regs_windows(uv, diffuse)`: mask on unit 8 (`regs_WindowMask`, bound per
material with `regs_WindowOn`), `regs_Night` (x = window strength, y = share). `color.fragment` adds the result after
the lighting and before fog. Not in shadow passes or additive materials.

## 2. Street lamps

### 2.1 Which templates are lamps, where the heads are

* Lamps by name: the mesh file name contains `lamp`, `light` or `lantern`, not `traffic` or `lighthouse`
  (`street-light01`, `street-light-large`, `avenue-lights(b)`, `oldstreetlamp`, `platformlamp01/02`,
  `railyardlamp01`, `lightingtower`, `apronlamp`). Templates with a `glow` material (the DLC street lights with their
  own additive pool, sky-and-night.md §4.2) keep their own light.
* Heads (`findLampHeads`, template space): the triangles in the upper 45 % of the model whose normals point down
  (ny < −0.6) are the underside of the lamp housing. Faces whose footprints touch (0.2 m) are one head, so a long
  housing on an arm is one light; an underside spanning the pole axis on both sides (V-shaped double arms) is split
  into its two sides; heads closer than 1.2 m are merged; heads under 25 % of the largest area are dropped. The light
  sits at the area-weighted centre, at the lowest point of the underside. Results (`regs-tool lamps <game> <.object>`,
  `REGS_LAMP_VIEW=<dir>` draws front/top views):

| model | heads |
|---|---|
| street-light01 | (0, 4.54, 0) |
| street-light-large | (2.13, 9.83, 0) arm end |
| avenue-lights | (±2.5, 11.70, 0) both arms |
| oldstreetlamp | (1.19, 7.47, 0) bracket lantern |
| platformlamp01/02, railyardlamp01 | top |
| apronlamp | 4 floodlights |

### 2.2 Light on the ground: the lamp map

One RGBA8 texture over the whole map (1.5 m per texel, ≤ 2048², Vienna 2048² for 3072 m): for every lamp head a pool
of warm light (1, 0.78, 0.5) × (0.7·e^(−d²/16) + 0.3·e^(−d²/72)) · (1 − d²/14²)², d in metres from the point under
the head: a bright core of ~4 m and a long faint tail that joins the pools along a street. Overlaps add up, softly
limited (v / (1 + 0.35 v)). Alpha = the light-weighted head height (fixed range around the map's heads).

The default shader's colour stage samples it at the fragment's world position (`regs_WorldPos`, computed in every
vertex stage from clip space: eye z = −w, so skinned and animated vertices are right too) and adds
`surface colour × light × strength`, for fragments below the head (fading 0.5 m above to 1 m below it) and above
the ground under it (fading out 11–18 m below the head). Roads, pavements, terrain, trees, cars, people and the
walls next to a lamp are lit; roofs above the lamps are not. Strength = 1.9 × smoothstep(0.32, 0.62, night01).

Built once per map while the cycle is on (Vienna: 2523 heads in ~50 ms); after live edits only the area of lamps
added or removed is redone. Off in the underground view.

### 2.3 The lamp glass

No sprites: each lamp model's own glass glows (`lampGlassMask`, the shader's window path, kind 2). The mesh is
rasterised into its texture; texels of the head (within 0.8 m of a light point, from 1.5 m below it, for hanging
lanterns, to 1 m above) that face down, or look like glass (light, unsaturated, lighter than the housing, or shiny in
the specular map), glow warm (2.6, 2.15, 1.5) × (0.65 + 0.7 · luminance). Merged lamps carry a seed in the vertex
alpha, so each switches on at its own point of the dusk (night01 0.36…0.50), as the pools fade in.

### 2.4 Lit signs and advertisements

Ad props (`ad-props/`, not graffiti or ads painted on walls) and shop/wall signs (`wallsign*`, casino, beer garden,
market, grocery signs) by name. Their panel, the faces along the model's main direction (the direction holding the
most triangle area, either side) that are not tiny, glows in its own colours like a light box (albedo × 1.7), on
60 % of the items (per item seed), switching on at night01 0.3…0.5. Texture variations (other posters) get their own
copy of the shared variation material with the panel mask. Materials with an illumination map (some roof ads) keep the
game's own light.

## 3. Vehicles

Every moving car, bus, lorry, tram and surface train (not parked cars, boats, aircraft or units in tunnels;
`simVehicleLights` in `engine/sim.cpp`). Light points come from each model's body at lamp height (the vertices
between 20 % and 55 % of its height within 75 % of its half width: front and rear surface, half width), so they sit on
the bumpers and not at the bounding box, which mirrors and other parts can widen. The consist's leading end gets the
headlights, its trailing end the tail lights (reversed units use their other end).

* **Switching on**: each vehicle at its own point of the dusk (night01 0.12…0.55 from a hash of its colour seed and
  mover), over a moment; so lights come on one by one as it gets darker.
* **The lamps themselves** (`makeVehicleLampMask`): the mesh is rasterised into texture space, so each texel knows
  where it sits on the model. Texels on front-facing faces at the front surface and at lamp height that are bright,
  nearly white and clearly brighter than the rest of the front (or very shiny in the specular map) are head lamps;
  texels on the rear faces that are clearly red are tail lamps. Only compact spots count (no thin chrome trims, nothing
  over 3 % of the texture), of each kind those at least a quarter of the largest. Found lamps grow by one texel (on the same
  end's faces), and the shader keeps a lamp's light when it is smaller than a pixel (its emission × the squared texel
  footprint, up to 16×): the 1-3-texel lamps of the base-game cars otherwise flickered in and out as the cars moved. The mask (R front, G rear) goes
  through the window path of the shader (`windowKind` 1, `DrawItem::lampCode` per part: head white, tail red,
  brighter red while standing). `regs-tool vehiclelamps <game> <outDir> <.object>…` shows what is found.
  Rear lamps are found reliably (red); front lamps are 1–3 texels on most base-game textures and are missed on some
  models, and a tram's white destination board can count as one. Ends of models without found lamps get a small glow
  sprite on the bumper instead (the only sprites left).
* **Cabin light** (buses, trams, trains; not cars): the window glass on the vehicle's texture, from its specular map
  (above 60 % of its brightest level; the paint is shiny too, but well below the glass; window-sized areas only),
  mask channel B, lit warm (0.78, 0.7, 0.56) on every car of the consist: at half strength by day while the cycle is on
  (`DrawItem::cabinLight` 0.5), rising to full as the vehicle switches its lights on. The driver's cab
  stays dark: on cab cars (head lamps found at the front) the light fades from 0 at 1.5 m to full 4.5 m behind the
  front; rail vehicles (names with tram, metro, train) have a cab at the rear too, so it also fades towards a rear
  with tail lamps. Middle sections are lit evenly.
* **Passengers**: silhouettes in the lit windows, drawn in the shader in the vehicle's model space (`regs_ModelPos`,
  every vertex stage passes the model position), at the game's character scale (its people are ~1.6x life size): per
  side a seat every 1.3 m with a seated head-and-shoulders shape (head radius 0.15-0.18 m) at 48-58 % of the window
  band's height (the band: 10th-90th percentile of the glass texels' model height, per
  material), standing figures between the seats (head at 84-94 %) once the vehicle is over 50-95 % full. Each figure
  has a random rank and shows when the occupancy (line vehicle passengers ÷ capacity) passes it; the occupancy is
  eased per vehicle (0.8/s), so figures fade in and out as people board and leave. Darkens the cabin light by 85 %.
* **Beam on the road** (headlight light map): each frame the beams are splatted on the CPU into a 1024² map (0.625 m
  texels) around the point the camera looks at, snapped to whole texels; the default shader samples it like the lamp
  map (unit 10, `regs_CarMap`) and lights whatever is there between 1.5 m below and ~3 m above the vehicle's ground:
  road, kerbs, cars and walls ahead, with soft edges. (A flat additive quad was tried first; kerbs and raised
  surfaces cut it with a hard line.) Low beam 15 m (buses 17 m), cone 3.4 m half width. A beam stops at the first building
  cell on its centre line (not the walls beside the road), its reach eased per vehicle so it never jumps; beams fade
  out between 520 and 650 m from the eye and within 90 m of the map window's edge (960 m across, 0.94 m texels), and
  the height encoding moves in whole 16 m steps, so nothing clips or jitters from frame to frame (measured with
  `fl-*.png`, every 2nd frame: the only one-frame changes are the game's ticker, route arrows and walkers).
* **High beam (Fernlicht)**, road vehicles only: on when no building cell (±6 m) and no lamp-lit street is in the 56 m
  ahead and no other vehicle (front or back point, either direction) is in a cone 90 m ahead; the beam then reaches
  42 m and is wider and brighter; the switch eases over ~⅓ s. Test: `REGS_DEBUG_NIGHT=1` logs the count every 120
  frames (Vienna centre: 235 lit, 12-18 on high beam; outskirts: 2, 1 on high beam).

## 4. Market stands and clocks

* **Market stands** (`marketstand*`): a warm lamp (1, 0.74, 0.45) under each canopy, in the same per-frame light map as
  the headlights: a pool of the stand's size × 1.3 + 1 m, its height set 0.6 m below the ground so the map's height
  window ends ~2.6 m up: goods, counter and the ground around are lit, the canopy is not. On in the evening and early
  morning (share of stands 0.6-0.9 from 17:00 to 21:30 and 5:30 to 8:00), each stand at its own point (seed), and only
  while it is dark (night01 0.1…0.4).
* **Clocks** (`*clock*`, e.g. `streetclock`): the dial (light, unsaturated texels of the model) glows (1.3, 1.2, 0.95) ×
  surface colour at night (night01 0.3…0.5). Glow only.

## 5. Cost

One texture read per default-shader fragment (lamp map) and one more on building materials (window mask); the glow
sprites are a few hundred quads. Vienna, 1600×900, vsync off: 2646 FPS at 14:00 (terms idle) vs 2607 FPS at 23:00.

## 6. Checks (port/run/night/, run.ps1 there)

* `t1-22-3000.png`: first result at 22:00; `zoom2-2900/3150/3420.png`: default, far and close view at 23:00.
* `cyc-*.png`: the cycle running from 18:30 (2-minute days), dusk to night, smooth (mean luminance per shot).
* `bed21/23/1/3-2950.png`: the same street at 21:00, 23:00, 1:00, 3:00 (windows going out).
* `veh1-2950.png` (first vehicle lights), `veh2-2950/3250.png` (lamps on the models, light-map beams), `dusk-*.png`
  (vehicles switching on one by one), `hb-*.png` with `hb.log` (high beams on the outskirts).
* `ls1-*.png`, `ls3-2950.png` (all signs lit for the check), `fin-2950.png`: lamp glass, shop windows, signs at 21:00.
* `sil-3100.png`, `sil-3350.png`: passenger silhouettes in a tram.
* `tram4-3100.png` (`REGS_DEBUG_FOLLOW=1`): tram cabin light, both cabs dark. `tl-2950.png`: stand lamps at 20:00.
  `ck-2950.png`: the clock dial on the square.
* `t2-3200.png`: underground view, no lamp light. `fps14/fps23-3350.png`: frame rate.
