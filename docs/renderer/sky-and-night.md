# Sky, sun disc, time of day and night lighting

What the engine does for the sky dome (`gsSky`), the sun disc (`gsSun`), the time of day, the "night" maps and their
self-illumination, glow sprites, and `gsTracerCtrl.enableFade`; then what the port adds on top (a day/night cycle,
which the original does not have). Addresses: `L:` Linux build (`decompiled/Linux/functions/0x….c`, real symbols),
`W:` GSystem.dll (`decompiled/GSystem-v2/functions/0x….c`). Confidence: H = read in the decompile, M = inferred from
the decompile, inf = reasoning. Related: `scene-basics.md` §4-5 (sun direction, light colours, fog),
`interfaces-3d.md` §1.1-1.3 (mesh flags, queues, glow sprites), `asset-format/mesh-material.md` (illumination slot).

## 0. Summary

* **There is no day/night cycle in Cities in Motion 1.** The sun follows `MAIN.$sceneClock`, a clock created with
  speed 0 (`metro/scripts/engine.script:13-16`); each location script sets its fixed time and light once
  (§2). The main clock (`$mainClock`, 10000× real time, a game day in 8.6 s) only drives the date and the simulation.
* **Night exists only as two locations**: New York (US DLC) and Tokyo set a near-black fog, a dark sky and a dim blue
  "moon" light. Their buildings and street lights carry illumination maps and additive glow meshes, which glow at
  any time; they stand out because the scene is dark (§4). The European locations (Vienna, Berlin, ...) have
  almost no illumination maps (signs only), so the original has no lit windows there.
* **The sky dome is hidden in normal play** (`engine.script:28-35` sets `.visible = false`; the floating camera's
  pitch never reaches the horizon). It is shown only by the debug screenshot mode (F5 with `$debug`, §1.4).
* **Glow sprites** exist only for `MATERIAL_GLOW` submeshes; the only one is the sun disc (§3). Street lights have none.
* `gsTracerCtrl.enableFade(a, b, c, d)` fades a tracer (the citizen mood icons) by its camera distance (§5).

## 1. gsSky

### 1.1 Object (obs, H)

| | Linux | Windows | Notes |
|---|---|---|---|
| ctor | `L:0x081254f0` | `W:0x10096070` | material = null, mesh = null, `fogColor` 0xB3CCFF, `fogOffset` 0, `fogDistance` 400 |
| create | `L:0x081252c0` | `W:0x10095fe0` | creates the `gsMaterialVariant` (`material`, +0x104 / W +0x13c) when there is a video |
| move | `L:0x081251f0` | `W:0x10096100` | fog (scene-basics.md §5), then `mesh->setScale(0.99)` (gsIMesh slot 9, `0x3f7d70a4`) |
| statusUpdated | `L:0x08124b50` | `W:0x100960c0` | passes the visible bit on to the mesh |
| generateMesh | `L:0x08125ee0` → `generateBackground` `L:0x08125580` | `W:0x10097720` → `W:0x10096810` | §1.2 |

Fields (`docs/native-api.md`): `fogColor` int (W +0x144 / L +0x10c), `fogOffset` float (+0x148 / +0x110),
`fogDistance` float (+0x14c / +0x114), `material` var read-only (+0x13c / +0x104), `generateMesh(n)`. The fog is
set every tick while the sky is **enabled**, whether visible or not, and only when `fogDistance > 0`.

### 1.2 generateBackground(n) (obs, H; `L:0x08125580`)

An indexed mesh with mesh flag `0x400` (scale × the viewport far distance, interfaces-3d.md §1.3), created through the
video with item flag `0x1000` (camera-attached: its position is set to the eye each frame, skipped in shadow-map
passes):

```
vertices: n rings × (4n + 1) + 1 pole                          (36-byte gsIndexedMesh vertices)
ring r = 0..n−1, f = r/n, φ = f·π/2; segment s = 0..4n, θ = s/(4n)·2π
  position = (cos θ·cos φ, sin φ, sin θ·cos φ); ring 0: y −= 0.5        (the lowest ring dips under the horizon)
  normal   = (0, −1, 0)                                          (gsNegYVector)
  uv       = (s/(4n), 0.95 − 0.9·f)
  colour   = gsColor(128 − round(128·f), same, 255, 255)
pole: position (0, 1, 0), uv (0, 0.05), colour (0, 0, 255, 255)
indices: per ring pair and segment (a, a+1, b), (b, a+1, b+1), a = r·(4n+1)+s, b = a + 4n+1;
         cap (a, a+1, pole) on the last ring             → 24n(n−1) + 12n indices
material (the variant's native material):
  setTexture(0, "/textures/envmap.dds")                  (the reflection cube map)
  setShader("/effects/sky/sky.shader")
  setAmbient(black), setDiffuse(white), setSpecular(black, 0, 0), setIllumination(0x202020, brightness 0)
                                                          (W:0x10096810.c:286-294; gsBlack/gsWhite = DAT_10196c34/c84,
                                                           initialised in W:0x1014ed40 / W:0x1014ed60)
```

`effects/sky/sky.shader` takes only `POSITION` and `TEXTURE0`; the vertex shader passes `normalize(gl_Vertex.xyz)`, the
fragment shader writes `textureCube(texture0, dir) · gl_FrontMaterial.diffuse`. No lighting, no fog. Vertex colours
and uvs are unused. With the defaults the dome shows the environment cube unchanged; New York and Tokyo darken it with
`MAIN.$sky.material.diffuse = 0x101010` / `0x111010` (diffuse alpha 0: the alpha is not used, the material is opaque).

Placement per frame: world = S(0.99 · far)·T(eye) (mesh scale from `move`, flag 0x400, flag 0x1000), so the dome sits
just inside the far plane around the camera and every scene object occludes it. It goes into the opaque queue.

### 1.3 Script use (obs, H)

| Script | Calls | visible |
|---|---|---|
| `metro/scripts/engine.script:28-35` (the game) | `generateMesh(5)`, fog 0xCCC8BB 300-1500 | **false** |
| `metro/ui/menus/animation/animation.script:53-59` (menu trams) | `generateMesh(5)`, fog 0xDCD1C3 200-500 | false |
| `metro/ui/objectlist/renderer.script:15-21` (object previews) | `generateMesh(5)`, fog 0xCCCCBB 1000-2000 | false |
| `metro/scripts/debug.script:44-55` (F5) | `MAIN.$sky.visible = true`, fog 1750, viewport 1500, camera max 1500, `$screenShotMode` | true |

`engine.script:30` keeps `//.material.setTexture(0, "textures/cloudmap.dds")` commented out. The location scripts set
the fog colour (and for the night locations the sky diffuse), §2.

### 1.4 When the dome is seen

In play the camera pitch is `0.6 + 0.0017·zoom` rad (41.7°-83.1° down, `docs/ui/game-load.md` §6.3) with a 40° field
of view, so the view never reaches the horizon; beyond the far plane the viewport's `clearColor` 0xFF373028 would show.
With `$debug = true` and `$disableDebug = false` in settings.script, F5 (`debug.script:44`) switches the screenshot
mode on: the sky becomes visible, the view reaches 1500, and Up/Down (`controls.script:18-23`) tilt the camera
(`ui.script:83-87`: `quat($cameraTilt, X)`, 0 = horizontal, 0.1 steps, 0..1.5).

## 2. Sun and time of day

### 2.1 gsSun (obs, H)

Direction, light colours: scene-basics.md §4.1-4.2 (`gsSun::move` `L:0x08124bc0`, `W:0x100964c0`). The time of day is
read from `gsSun.clock` (`d = (time/1000 mod 86400)/86400`; 0.6 without a clock). Elevation of `rot(Q, +Z)` with the
default latitude π/4 (computed with the port's copy of the formula, `REGS_DEBUG_SKY=1`): 68° at noon, 23° at 6:00 and
18:00, 0° at about 4:30 and 19:30, −67° at midnight. (The angle `a` of §4.1 equals the elevation only at 6:00, 12:00
and 18:00.)

`gsSun::generateMesh(n, size)` (`L:0x08125fc0`, `W:0x10096fa0`): sets the direction vector to +Z (+0x108; before it is
(0,0,0), so a sun without a mesh casts its light from the origin) and builds the disc:

```
rim i = 0..n: position (cos t·size, −sin t·size, 1), t = 2π·i/n; normal (0,−1,0); uv (cos t, −sin t); white
centre: position (0, 0, 1), uv (0.5, 0.5); indices (i, i+1, centre)
mesh flag 0x400, item flag 0x1000; material: texture "effects/sky/sun.dds", shader "effects/sky/sun.shader",
setFlags(0x20 = MATERIAL_GLOW), ambient/diffuse black, specular black, setIllumination((255, 255, 224), 30)
```

`gsSun::move` then orients the disc with the sun quaternion (`setOrientation(Q, 0)`, slot 7) and **scales** it by
0.94 (`setScale`, gsIMesh slot 9, `0x3f70a3d7`; scene-basics.md §4.1 calls this call `setAlpha`, which is slot 3).
`sun.shader`: `texture2D(texture0, uv) · 6.0`, no lighting. All game scripts call `generateMesh(40, 0.04)`.

### 2.2 The clocks (obs, H)

| Clock | Created | Speed | Drives |
|---|---|---|---|
| `MAIN.$sceneClock` | `engine.script:13-16`, `clock(mktime(8,30,0), mkdate(1,1,1920))` | 0.0, never changed | `MAIN.$sun.clock` |
| `MAIN.$mainClock` | `engine.script:21-24` | 10000 × game speed (`minimap.script:171-180`: speeds 0, 1, 4, 10) | date, simulation, ticker |

Each location script sets `MAIN.$sceneClock.time` once while the level loads (`environment.script` → `location.script`).
Nothing else writes it, so the light, the fog and the shadow direction stay fixed for the whole game.

### 2.3 Locations (obs, H; `metro/locations/*/location.script`, `metro/location-data/*`, `addons/*/locations/*`)

| Location | Time | Fog | Light diffuse / ambient / specular × magnitude | Other |
|---|---|---|---|---|
| austria (Vienna), england, london, paris, california | 8:30 | 0xCCC8BB | 0xFFFFF0D0 / 0xFF202030 / 0xFFFFFFDD × 2.5 | |
| germany (Berlin), germany2, munich, ulm | 14:30 | 0xCCD8BB | 0xFFFFF8E0 / 0xFF606070 / 0xFFFFFFDD × 1.5 | |
| netherlands | 16:00 | 0xCCC0BB | 0xFFFFE0D0 / 0xFF202040 / 0xFFFFFFDD × 2.5 | |
| finland | 8:30 | 0xCCCCCC | 0xFFFFFFFF / 0xFF202040 / 0xFFFFFFDD × 2.5 | |
| russia (St. Petersburg) | 14:00 | 0xCCCCBB | 0xFFF0F0D0 / 0xFF808080 / 0xFFFFFFDD × 1.5 | |
| **newyork** (US DLC) | 12:00 | **0x080808** | 0xFF8080FF / 0xFF706080 / 0xFFDDDDFF × 1.0 | sky diffuse 0x101010, latitude 23° |
| **japan** (Tokyo) | 12:00 | **0x090808** | 0xFF8080FF / 0xFF706080 / 0xFFDDDDFF × 1.0 | sky diffuse 0x111010, latitude 23° |

All use fog offset 300 and distance 1500. The two night locations keep the sun at noon: it acts as a high moon.
The shadow map (`$shadows`) and its strength follow that light like any other: there is no separate shadow
strength, and no "switch-over to night" anywhere in the scripts or the engine.

## 3. Glow sprites (obs, H; inf where marked)

A submesh whose material has `MATERIAL_GLOW` (0x20) and a non-zero brightness goes into the glow queue (interfaces-3d.md
§1.2-1.3), is drawn like an opaque one inside an occlusion query, and after the scene the viewport's overlay pass draws
a star-shaped additive fan for each visible one (`gsSubMesh_OGL::processGlowHalo` `L:0x08204d20`, `renderGlowHalo`
`L:0x08204c60` / `W:0x10049280`, `gsVideo_OGL::renderGlow` `L:0x08214d10` / `W:0x1004e6c0`, `glow.shader`):

```
v      = visibility from the query (+0x90; scaled by the submesh size over its distance and an angle term)
centre = projected submesh centre (+0x94, +0x98)
size   = brightness·0.0005 + v·15;  strength = sqrt(v)·brightness·10;  centre alpha = sqrt(strength)·0.15
fan    = centre (colour = material illumination ARGB, uv (1, alpha)) + 49 rim vertices (alpha 0, uv (0, alpha)) at
         radius (spike(i)·size²·0.1 + 0.9)·size, spike(i) = (i odd ? 0 : 1) + (i % 8 == 0) + (i % 4 == 0)
glow.fragment: rgb = colour, a = uv.x² · uv.y
```

`gsVideo_OGL::testMaxOcclusion` (`L:0x08214f20`) measures the sample count of a full-screen quad once. No shipped
script or `.material` sets `MATERIAL_GLOW` (grid-items.md §5 table; the `.material` loader never sets 0x20), so the
**sun disc is the only glow submesh**. The street lights' "glow" is a different mechanism (§4.2).

## 4. Self-illumination ("illumination maps")

### 4.1 Material and shader (obs, H)

* `.material` slot chunk `0x02013140` = illumination map → texture key 3 and `MATERIAL_ILLUMINATIONMAP` (0x20000)
  (mesh-material.md §2.11). Emission = header emissive colour (0x34) × selfIllumination byte (0x26) × 0.1
  (`W:0x1008ec70.c:115-117`, `setIllumination` `W:0x1004dac0`).
* Default shader (`shaders/default/`): `maps.fragment` samples the map; `color.fragment:58-60` adds
  `gl_FrontMaterial.emission.rgb × illuminationMap.rgb` after lighting; without a map `lighting.vertex:21-25` adds the
  emission to the ambient term. No uniform or flag scales it with the time of day: the strength is constant.
* Scripts can set `gsMaterialVariant.illumination` (ARGB, `W:0x10087250`) and the brightness through the native
  material, but no shipped script changes it over time.

### 4.2 Which assets light up (obs; `.material` scan of `extracted/`)

* Base game (`metro/objects`): 11 illumination maps, all small: roof and text advertisements (`roofads_i`,
  `textads01_i`), shop signs of the office buildings (`officebuilding01_i`: one sign strip), `officebuildings_i`,
  `cinema_i`, `deptstore_i`, the roller coaster, the airport taxiway lights and one element of `city-props_i` /
  `cityprops02_i`. **No residential windows, no street-light heads.**
* US and Tokyo DLC (`addons/usa`, `addons/tokyo`): 209 materials, including the windows and shop fronts of the
  apartment buildings, block houses, tenements, detached houses, offices, the car lights (`era2_sedan01_i`) and the
  amusement park. Their street lights (`streetlight`, `sf-streetlight*`, `avenue-lights`, `oldstreetlamp`,
  `street-light-large`, ...) have a `glow` material (`glow.material`: illumination map `glow.dds`, selfIllum 10) on
  which their scripts set `MATERIAL_ADDITIVE_A` in `onLoaded` (`addons/usa/props/streetlight.script:11-15`): drawn
  blended with (SRC_ALPHA, ONE), they light the ground around each lamp.

So the night look of New York and Tokyo is the normal pipeline with a dark light plus these assets.

## 5. gsTracerCtrl.enableFade (obs, H)

`method_enableFade(a, b, c, d)` (`L:0x081ce870`, `W:0x100abdb0`) sets +0x1c9 = 1 and stores the four floats at
+0x1cc..+0x1d8 (ctor defaults 0, 100, 10000, 11000, `L:0x081d38a0`). `gsTracerCtrl::move` (`L:0x081cfc00`), after
placing the control over its target and only while the target is in view, measures the distance from the viewport
camera's position to the target point and calls `setVisibility` (vtable 0x174 = `gsControl::setVisibility`
`L:0x081c7830`, read from the vtable at `0x082a88c8`):

```
dist ≤ a or dist ≥ d → 0;  a < dist < b → 1 − (b − dist)/(b − a);  b ≤ dist ≤ c → 1;  c < dist < d → 1 − (dist − c)/(d − c)
```

The only caller is `metro/scripts/ui.script:559`, the citizens' mood icons: `enableFade(0, 10, 300, 320)`, so the
icons vanish between 300 and 320 units from the camera (a commented call in `secondaryview/bubbletarget.script:17`).

## 6. The port

### 6.1 Engine parity (port/src/engine/sky.cpp)

* `gsSky.generateMesh(n)`: the dome of §1.2 and its material on the sky's `material` variant; drawn when the sky is
  visible and enabled, at the eye with scale 0.99 × the projection's far distance, with `MATERIAL_NO_SHADOW` so the
  shadow pass skips it (the engine skips flag-0x1000 items there).
* `gsSun.generateMesh(n, size)`: the disc of §2.1, oriented by the sun quaternion, scale 0.94 × far. The glow sprite of
  §3 is **not** drawn (it needs the occlusion-query visibility; only the sun has one).
* `gsTracerCtrl.enableFade`: §5, applied in `placeTracer` (`engine/interaction.cpp`).
* Material blend functions in the blended queue (`render/renderer3d.cpp`, interfaces-3d.md §2.10): `ADDITIVE`
  (ONE, ONE), `ADDITIVE_A` (SRC_ALPHA, ONE), `MODULATE` (DST_COLOR, ZERO), `MODULATE_2` (DST_COLOR, SRC_COLOR). Before,
  every blended part used (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), so the DLC street-light glows darkened instead of lit.

### 6.2 Day and night (port option, not in the original)

Graphics page checkbox "Tag und Nacht" (`$regsDayNight`, default on since 2026-11-18; off keeps the location's fixed time of day as in the original), cycle length `$regsDayLength` real minutes per
day at normal speed (default 12; settings.script only). Debug switch `REGS_DEBUG_TIME=<hour>` sets the time of day at
the start of the game and holds it unless the option is on. Implemented in `SkyObj::dayNight` (the game sky's tick):

* **Time**: its own hours, advanced by `dt · (mainClock.speed / 10000) · 24 / (60 · length)`; 0 while paused. It starts
  at the location's time (night locations: at midnight, §2.3).
* **Light ends**: the location's own values (sun light, fog, sky diffuse) are one end; the other end is shipped data:
  the New York night (§2.3) for the day locations, the `engine.script` daylight (0xFFFFF0D0 / 0xFF202030 / 0xFFFFFFDD
  × 2.5, fog 0xCCC8BB, sky white) for the night locations (fog luminance < 0.15).
* **Weights** from the sun elevation e of §2.1: day = smoothstep(−0.14, 0.14, e); dusk tint = smoothstep(−0.14, 0, e) ·
  (1 − smoothstep(0.05, 0.45, e)), warm (1, 0.68, 0.42) on the direct light, (1, 0.85, 0.75) on the ambient,
  (0.95, 0.62, 0.45) × daylight fog for the fog; a sky-light term adds 0.45 · (sin 45° − sin e) · day of the daylight
  colour (half the warm tint) to the ambient while the sun is low, since the direct light on the ground falls with
  sin e and the locations' fixed times have the sun at 40-50°.
* **Moon**: while e < −0.04 the scene clock is turned by 12 hours, so the light (and the shadows) come from a moon
  opposite the sun; the direct light fades to 0 on both sides of the switch (smoothstep(−0.04, 0.10, e) for the sun,
  smoothstep(0.04, 0.18, −e) for the moon), so the jump is not seen.
* **Self-illumination**: the material emission is multiplied by `SceneView::emissionScale`: up to 2.5 at night on day
  locations (signs and DLC windows stand out), 0.4 by day on night locations (lit windows fade in daylight); 1 at the
  location's own time, so the option off or at that time gives the original image.
* The results are written into the script objects (`MAIN.$sun.light`, `MAIN.$sky.fogColor`, `MAIN.$sky.material
  .diffuse`, `MAIN.$sceneClock.time`), so the shadow pass and the corner preview follow; switching the option off
  restores the location's values.

* **Night lights**: lit windows (from the buildings' specular maps) and street lamps (pools on the ground, glows on
  the heads) while the cycle is on: night-lights.md.

### 6.3 Checks (port/run/sky/, helper scripts there)

* Day / dusk / night, Vienna save, `REGS_DEBUG_TIME`: `vienna-14.png`, `vienna-19.png`, `vienna-20_5.png`,
  `vienna-23.png`; Berlin: `berlin-original.png`, `berlin-20_25.png`, `berlin-23.png`.
* New York sandbox (the DLC night map): `ny-original.png` (option off) and `ny-0.png` (midnight with the model) are the
  same image; `ny-14.png` day, `ny-20_5.png` dusk with lit windows and the additive street-light pools.
* The cycle running (`$regsDayLength = 1`, normal speed): `sunset-sheet.png` (19:20 → 23:00), `cycle-sheet.png`
  (dusk → night → dawn).
* The dome in the screenshot mode (F5, tilt to the horizon): `sky19-sheet.png` (eight directions at 19:00, the sun
  disc in `sky19-dir6.png`), `sky-day-night-sheet.png` (14:00 and 23:00).
* `REGS_REPORT_MISSING=1`: `gsSky.generateMesh` and `gsTracerCtrl.enableFade` are no longer listed.
