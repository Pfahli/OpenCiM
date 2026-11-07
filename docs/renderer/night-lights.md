# Night lights (port feature)

Lit windows and street lamps for the port's day/night cycle (`$regsDayNight`, sky-and-night.md §6.2). The original
game has neither: the European locations have no window or lamp illumination maps at all (sky-and-night.md §4.2).
Everything is derived in memory from the shipped assets at load time; no game file is changed or shipped.
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
4. Each window gets one of **four random groups** and a random brightness 0.5…1. Channel k of the mask (BGRA8) holds
   coverage × brightness of the group-k windows (soft edges: smoothstep(T ± 0.07)).
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

### 2.3 Glow on the head

Each head within 700 m and in view gets an additive camera-facing sprite (1.5 m half size, colour (1, 0.88, 0.69),
a soft core and a faint halo; the particle batch path, `particles.shader`), moved 0.9 m toward the eye so the housing
does not hide it. Each lamp switches on at its own point of the dusk (night01 0.36…0.50), over ~2 game minutes.

## 3. Cost

One texture read per default-shader fragment (lamp map) and one more on building materials (window mask); the glow
sprites are a few hundred quads. Vienna, 1600×900, vsync off: 2646 FPS at 14:00 (terms idle) vs 2607 FPS at 23:00.

## 4. Checks (port/run/night/, run.ps1 there)

* `t1-22-3000.png`: first result at 22:00; `zoom2-2900/3150/3420.png`: default, far and close view at 23:00.
* `cyc-*.png`: the cycle running from 18:30 (2-minute days), dusk to night, smooth (mean luminance per shot).
* `bed21/23/1/3-2950.png`: the same street at 21:00, 23:00, 1:00, 3:00 (windows going out).
* `t2-3200.png`: underground view, no lamp light. `fps14/fps23-3350.png`: frame rate.
