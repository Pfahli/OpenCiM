# GSystem 3D basics: the main-menu tram scene, end to end

Scope: everything a port needs to reproduce the main menu's 3D animation (`metro/ui/menus/animation/animation.script`):
two `menu-tram.object` instances lit by one sun light, seen by a fixed camera, rendered through a `gsTextureTarget_OGL`
into a texture and composited by a GUI quad with `metro/shaders/menu/anim.shader`. It builds on
[interfaces-3d.md](interfaces-3d.md) (queues, shader contract §3) and [interfaces-2d-output.md](interfaces-2d-output.md)
(viewports, post effects §2.8/§2.12) and does not repeat them.

**Citations.** `L:0x<addr>.c:<line>` = `decompiled/Linux/functions/0x<addr>.c` (Linux build, real C++ names),
`W:0x<addr>.c:<line>` = `decompiled/GSystem-v2/functions/0x<addr>.c` (Windows GSystem.dll). **obs** = read in the
pseudocode, **inf** = inferred. Confidence H/M/L. Two Windows labels from `linux-names.csv` are wrong (identical-code
folding): `gsArgument` at `W:0x100840e0` is the **quaternion product**, and `gsTransportStopController::checkMood` as
called from `W:0x100964c0`/`W:0x10048890` is **vector × scalar**.

**Matrices** are row-major `float[16]`, row vectors (`v' = v·M`, translation in m[12..14]); the same memory is what GL
loads with `glLoadMatrixf`/`glUniformMatrix*(transpose = GL_FALSE)`.

---

## 0. Corrections to the brief (all obs)

| Brief says | Engine does | Evidence |
|---|---|---|
| material flags DIFFUSEMAP\|SPECULARMAP\|NORMALMAP\|COLORIZE\|ALPHATEST\|SPECULAR | **also `MATERIAL_REFLECTION`** (header byte 0x27 = 128 ≠ 0): reflection amount 128/255, cube map `textures/envmap.dds` in texture unit 3. Final `matFlags = 0x1601E` | `W:0x1008ec70.c:129-130, 337-342`; header bytes dumped from `menu-tram.material` |
| target is 256×128 | 256×128 is only the **minimum**. In `pixelCoordinates` mode the FBO and the texture's GL storage are re-allocated to `max(viewport px size, 256×128)`; the menu viewport is always 2:1 (e.g. 1800×900 at 1600×900) | §7.1 |
| sky invisible → fog? | fog is **on**: the sky module ticks while invisible and pushes start = `fogOffset` = 200, end = `fogDistance` = 500 every tick | §5 |
| `clearColor = 0xFF444444` | RGB 0x44/255 but **alpha written = 0** (always), so the target is transparent outside the trams | `L:0x082007b0.c:161-168` |

`menu-tram.object` (from the archives) is one submesh, 4 722 vertices, 3 692 triangles, two coupled tram cars, local
AABB x −65.70…1.33, y −14.66…14.66, z −100.68…0.41: the cars run along (−0.64, 0, −1), the same direction as the script
moves them. It has a UV2 chunk (unused here), no skin, all vertex colours 0xFFFFFFFF.

---

## 1. Quaternions and the script constructor (obs, H)

Storage `(x, y, z, w)`.

| Operation | Formula | Evidence |
|---|---|---|
| `quat(angle, axis)` (script, 2 args) | `a = axis / length(axis)` (length 0 → a = 0), `q = (a·sin(angle/2), cos(angle/2))`; angle in **radians**, as float | `L:0x0810bbe0.c:29-35`, `L:0x08120830.c:41-52` |
| `quat(x, y, z, w)` (4 args) | components as given | `L:0x08120830.c:84` |
| `quat(q)` (1 arg) | q if it is a quaternion, otherwise identity; 3 args → runtime error 11 | `L:0x08120830.c:70-106` |
| product `qmul(a, b)` | Hamilton `a ⊗ b`: `x = aw·bx + ax·bw + ay·bz − az·by`, `y = aw·by − ax·bz + ay·bw + az·bx`, `z = aw·bz + ax·by − ay·bx + az·bw`, `w = aw·bw − ax·bx − ay·by − az·bz` | `W:0x100840e0.c:63-67` |
| rotate `rot(q, v)` | `q* ⊗ (v,0) ⊗ q` (special case v = (0,0,z) inlined) | `W:0x10085e40.c:30-52` |
| matrix `M(q)` | row-major standard rotation matrix: `m0 = 1−2(y²+z²)`, `m1 = 2(xy−zw)`, `m2 = 2(xz+yw)`, `m4 = 2(xy+zw)`, `m5 = 1−2(x²+z²)`, `m6 = 2(yz−xw)`, `m8 = 2(xz−yw)`, `m9 = 2(yz+xw)`, `m10 = 1−2(x²+y²)` | `L:0x0810c970.c:34-46` |
| matrix → quat | inverse of the above (`x = (m9−m6)/4w` …) | `W:0x10085650.c` |

Consequences: `rot(q, v) = v·M(q)` (row vector) = the **conjugate** of the textbook active rotation, as documented in
`docs/asset-format/README.md`. Composition child-then-parent is `q_child ⊗ q_parent` (`rot(a⊗b, v) = rot(b, rot(a, v))`).
`quat(3.1415926536, (0,1,0))` = `(0, 1, 0, −4.37e−8)` (float π); `M ≈ diag(−1, 1, −1)`. A 180° turn is its own
conjugate, so **for this scene the convention cannot be got wrong**.

---

## 2. Camera

### 2.1 Fields and per-tick update (obs, H)

`gs3DCamera` (Windows offsets / Linux offsets): `fov` +0x188/+0x138 (default 60), `offset` vec3 +0x16C/+0x11C,
`target` object ref +0x168/+0x118, `lookback` +0x184/+0x134, `ignoreOrient` +0x18C/+0x13C, `minDistance`
+0x190/+0x140, `maxDistance` +0x194/+0x144 (both default −1 = "use the viewport's"); `position`/`orientation` are the
inherited gs3DElement local pose (`W:0x10072950.c`, `L:0x080e7d80.c`, defaults `L:0x080e9c00.c`). There is **no `up`
field**: up is derived from the orientation. `target` is an object, not a point.

Every tick (`gs3DCamera::move` `L:0x080e7ab0.c`) runs `gsFixedCamera::updateFinalOrient` then
`gsCamera::updateCamera` (`L:0x080e7580.c`):

```
finalOffset = offset; finalMin = minDistance; finalMax = maxDistance          W:0x10071b00.c:25-29
Q (final global orientation):
  ignoreOrient            -> Q = localOrient                                   :30-32
  flag +0x111 == 0        -> keep the current global orientation               :34
                             (0 only after a direct setGlobalPos/Orient; setLocal* sets it to 1,
                              W:0x10066bd0.c, W:0x10066c50.c; this function re-sets it to 1 at :82)
  R = target ? target : parent3D       (parent3D = the owner scene here)
  R                       -> Q = (localOrient ⊗ R.getFront()) ⊗ quat(R.getMatrix())   :37-64
                             (getFront() = identity for plain elements, L:0x080d8f80.c)
  else                    -> Q = localOrient
  lookback                -> Q = quat(π, +Y) ⊗ Q                                :45, :58, :72
  setGlobalOrient(Q)                                                           :81
P    = target ? target.globalPos : own globalPos        (globalPos = localPos · parentMatrix)   W:0x100719f0.c:20-29
eye  = P + rot(Q, finalOffset)                                                  W:0x100719f0.c
look = P + rot(Q, (offset.x, offset.y, min(offset.z − 1, 0)))                   W:0x10072270.c:21-43
up   = rot(Q, (0, 1, 0))                                                        W:0x10071a80.c:17, W:0x1014a8d0.c:12
gsCamera_OGL: setPosition(eye) setTarget(look) setUp(up) setFov(fov) setMinDistance(finalMin) setMaxDistance(finalMax)
```

The scene module's matrix and orientation are identity (gsModule matrix ctor `L:0x08135500.c`), so for scene-owned
cameras `Q = orientation`, `P = position`.

### 2.2 Projection and view (obs, H; `renderScene` `L:0x082007b0.c`, `W:0x10044ef0.c`)

```
near   = vp.minDistance;  if cam.near != −1 && cam.near >= near: near = cam.near      (= max)   L:…:176-184
far    = vp.maxDistance;  if cam.far  != −1: far = min(cam.far, vp.maxDistance)                 :185-188
aspect = ctx.w / ctx.h          (viewport size in px; for a texture target see §7.1)            :190
f      = 1 / tan(fov·0.017453292 / 2)          (fov in degrees, vertical)
P = [ f/aspect 0 0 0 ; 0 f 0 0 ; 0 0 (far+near)/(near−far) −1 ; 0 0 2·far·near/(near−far) 0 ]   L:0x0810f7a0.c
   then optional mirror (x/y × −1) and viewport sub-rect (both unused here)
z = normalize(eye − look);  y = normalize(up − (up·z)·z);  x = y × z
V = [ x.x y.x z.x 0 ; x.y y.y z.y 0 ; x.z y.z z.z 0 ; −x·eye −y·eye −z·eye 1 ]                L:0x0810f510.c
```

GL clip z ∈ [−1, 1]. `gsViewPort` defaults: minDistance 0.1, maxDistance 400 (`L:0x081dc440.c`).

### 2.3 The menu camera (values computed with the formulas above)

`position (0,0,−100)`, `orientation quat(π, +Y)`, offset 0, fov 60, camera min/max 1/1000, viewport min/max 0.1/1000.

| | Value |
|---|---|
| eye / look / up | (0, 0, −100) / (−8.7e−8, 0, −99) / (0, 1, 0) → **forward = +Z**, screen-right = **−X** |
| near / far / aspect | 1 / 1000 / 2.0 (viewport is always 2:1, §7.1) |
| V | `[−1 0 0 0; 0 1 0 0; 0 0 −1 0; 0 0 −100 1]` (±9e−6 noise from float π) |
| P | `[0.866025 0 0 0; 0 1.732051 0 0; 0 0 −1.002002 −1; 0 0 −2.002002 0]` |

**Trams in view** (inf, H: mesh AABB corners projected; t = script phase):

| Tram | t | origin | NDC x | NDC y | eye distance | fog factor |
|---|---|---|---|---|---|---|
| 1 (drives toward the camera) | 0 | (268.4, −9.1, 433.8) | −0.54…−0.33 | −0.10…0.02 | 478–599 | 0.93–1 |
| 1 | 0.5 | (75.9, −9.1, 133.8) | −0.50…−0.04 | −0.31…0.07 | 134–248 | 0–0.16 |
| 1 | 0.72–0.89 | crosses the camera plane z = −100 at x ≈ −75 | passes ≈ 75 m to the camera’s right (world −X), leaves the frame on the right | | | |
| 2 (drives away, rotated 180°) | 1 → 0 | (−129.6…255.4, −9.1, −166.2…433.8) | enters from behind-right (crosses z = −100 at x ≈ −87), recedes to −0.52…−0.35 | −0.08…0.02 at t=0 | 591–711 at t=0 | 1 |

Both trams are on screen in the left half of the image near the horizon for most of their cycle, emerging from (tram 1)
or vanishing into (tram 2) the fog.

---

## 3. Objects (`new_object`, gsObject → gsMesh_OGL)

| Item | Rule | Evidence |
|---|---|---|
| owner | `new_object($scene)` makes the scene both scene and 3D parent; meshes are created as children of the scene's `gsScene_OGL` | `L:0x080d7fa0.c:23-33`, `L:0x081183c0.c:30-38` (obs) |
| mesh flags | 0x800 (0x2800 with `forceVisible`) | `L:0x081183c0.c:38` (obs) |
| draw distances | min 0, max `0x5A0E1BCA` = 1e16 (no `.properties` file) → no LOD fade, no distance cull, no ALPHABLEND from fading | `L:0x08115f40.c:57-58` (obs) |
| `position` / `orientation` | gs3DElement local pose; forwarded to every mesh as `setPosition(p, relative=1)` / `setOrientation(q, 1)` | `L:0x08110ed0.c:27`, `L:0x08110f40.c:27` (obs) |
| world matrix | `world = S(scale)·M(q)·T(p)` (scale 1 → `M(q)` with translation row p), then `· ctx.world` because the relative flags are set; ctx.world is identity for scene children | `W:0x1004a4d0.c:94-125` (obs), identity `W:0x10147f80.c` (inf) |
| modelview | `world · V` loaded per submesh | interfaces-3d §1.2 step 4 |

Menu: tram 1 `world = [I | p]`; tram 2 `world = [diag(−1,1,−1) | p]` with p updated every frame by the script.

`colorize(argb)` (`L:0x08113a20.c:44-50, 137`, obs): for every material of the object (and recursively of child
objects) whose stored colour differs: `gsMaterial_OGL::setColorization(argb)` → material +0x70 =
`(R, G, B, A)/255` from `0xAARRGGBB` (`W:0x1004db20.c`, `gsColorf` `L:0x080ee6c0`). No gamma conversion; alpha is
stored (1.0 here) but unused by the shader. Uniform `colorize` = material +0x70, re-sent on every apply
(`W:0x100365f0.c:75`). Materials are created **per object** (`L:0x0811b430.c:109, 158`), so the two trams colour
independently.

---

## 4. Sun, light and material

### 4.1 Sun direction (obs, H; `W:0x100964c0.c`, `L:0x08124bc0.c`)

```
d   = clock ? ((clock.timeMs / 1000) mod 86400) / 86400 : 0.6         W:…:27-29   (date ignored)
lat = latitude                                  (default 0x3F490FDB = π/4, L:0x08125f10.c:25)
q1  = quat(2π·d + π, +Y)                                               :36
a   = (lat − π/2) + 2·(π/2 − lat)·(1 − 2·|0.5 − d|) + 0.40142575       :40   (0.4014 rad = 23°)
Q   = q2 ⊗ q1,  q2 = quat(a, +X)                                       :41
dir = rot(Q, sunVec);  sunVec = (0,0,1) once generateMesh ran, else (0,0,0)   :50, L:0x08125fc0.c:57
light.setGlobalPos(dir · 20000); light.setGlobalOrient(Q)              :48-53
sun disc mesh: setOrientation(Q, 0), setAlpha(0.94)                    :56
```

Menu (`clock(mktime(6,0,0), …)`, timeMs = 21 600 000, `L:0x080eb900`): d = 0.25, a = 0.401426, q1 = (0, 0.707107, 0,
−0.707107), q2 = (0.199368, 0, 0, 0.979925), Q = (−0.140974, 0.692911, 0.140974, −0.692911), **dir = (0.920505,
0.390731, 0)** (east-ish, 23° up), light world position (18410.1, 7814.6, 0), eye space (−18410.1, 7814.6, −100.0).
The sun disc (camera-attached mesh, flag 0x1000, `MATERIAL_GLOW`, `effects/sky/sun.shader`, `L:0x08125fc0.c:225`) is
90° off the view axis and never visible here.

### 4.2 Light colours (obs, H)

`gsLight` keeps ARGB ints (diffuse +0x100, ambient +0x104, specular +0x108, magnitude +0x10C Linux) and sends
`(R,G,B)·magnitude/255, A/255` to the native light (`W:0x10075200.c:17-21`, `W:0x10091f00/f50/fa0.c:19`); writing
`magnitude` re-sends all three (`L:0x0811f0f0.c:20-31`). `gsSun::create` presets diffuse (0.6,0.6,0.55), specular
(1,1,0.9), ambient (0.2,0.2,0.2), magnitude 2 (`L:0x08124ef0.c:29-38`); the script then overwrites all four.

| GL value (`glLightfv(GL_LIGHT0, …)`, `L:0x0821e910.c:100-109`) | Menu value |
|---|---|
| `GL_DIFFUSE` (0xFFFFE0C0 × 3) | (3.0, 2.635294, 2.258824, 1) — **not clamped** with shaders on (clamp only when `video.shaders` is off, `:93-97`) |
| `GL_AMBIENT` (0xFF403830 × 3) | (0.752941, 0.658824, 0.564706, 1) |
| `GL_SPECULAR` (0xFFFFFFDD × 3) | (3.0, 3.0, 2.6, 1) |
| `GL_POSITION` | `(lightWorld · V, 1)` = (−18410.1, 7814.6, −100.0, 1); the modelview must be identity at that moment (inf) |
| attenuation | constant 1, linear 0, quadratic 0 (`:107-109`) |
| `GL_LIGHT_MODEL_AMBIENT` | (0.1, 0.1, 0.1, 1) (`L:0x082007b0.c:482-483`) |

Light count = 1 (only the sun's gsLight joins the scene light list) → `envFlags = 1 << 12 = 0x1000`
(`W:0x10051120.c`; inf M for "only one light": no other light-creating module in the script).

### 4.3 Material → `gl_FrontMaterial` (obs, H; loader `W:0x1008ec70.c`, bind `W:0x100478f0.c:100-109`)

`menu-tram.material` header bytes 0x20..0x27 = `00 00 00 FF 80 20 00 80`; ambient/diffuse `80 80 80 FF`, specular and
emissive `FF FF FF FF`; slots diffuse `tram03.dds` (alphaUsage 0, alphaBits 1 → ALPHATEST), specular `tram03_s.dds`,
normal `tram03_n.dds` (alphaUsage 1, alphaBits 8 → COLORIZE).

| Field | Rule | Menu value |
|---|---|---|
| ambient (`GL_AMBIENT`) | header 0x28 RGBA/255, alpha × object alpha | (0.501961 ×3, 1) |
| diffuse (`GL_DIFFUSE`) | header 0x2C, alpha × opacity/256 if opacity ≠ 255 | (0.501961 ×3, 1) |
| specular (`GL_SPECULAR`) | with a specular map: white × level `>> 8` per channel (`W:0x1008ec70.c:263`; white = `DAT_1019666c`, set to 0xFFFFFFFF in `W:0x1014dc40.c`) | 127/255 = (0.498039 ×3, 1) |
| emission (`GL_EMISSION`) | header 0x34 × selfIllum·0.1 (`FUN_10075200`) | (0, 0, 0, 1) |
| shininess (`GL_SHININESS`) | `max(1, byte 0x25)` | 32 |
| reflection (uniform) | byte 0x27 / 255 | 0.501961 |
| flags | ALPHATEST 2 \| SPECULAR 4 \| NORMALMAP 8 \| SPECULARMAP 0x10 \| REFLECTION 0x2000 \| DIFFUSEMAP 0x4000 \| COLORIZE 0x10000 | **0x1601E** |
| texture units | key order diffuse 0, specular 1, normal 2, env cube 5 → packed | unit 0 `tram03.dds` (512×256 DXT1), 1 `tram03_s.dds` (DXT1), 2 `tram03_n.dds` (DXT5, A = paint mask), 3 `textures/envmap.dds` (cube 512², DXT1, 10 mips) |

Resulting GL built-ins (component-wise products, GLSL 1.10 definitions):

| Built-in | Value |
|---|---|
| `gl_FrontLightProduct[0].ambient` | (0.377947, 0.330704, 0.283460, 1) |
| `gl_FrontLightProduct[0].diffuse` | (1.505882, 1.322814, 1.133841, 1) |
| `gl_FrontLightProduct[0].specular` | (1.494118, 1.494118, 1.294902, 1) |
| `v_ambient.rgb` = emission + LM.ambient·mat.ambient + FLP.ambient | (0.428143, 0.380900, 0.333656) |

---

## 5. Fog (obs, H)

`gsSky::move` (`L:0x081251f0.c:22-27`) runs every tick for an **enabled** sky (status bit 0, default on; `.visible =
false` only hides the dome mesh, `L:0x08124b50.c`):
`if fogDistance > 0: scene.setFog(gsColorf(fogColor), start = fogOffset, end = fogDistance)` →
`gsScene_OGL::setFog(on=1, rgba, start, end)` (`L:0x0811f2c0.c:18-27`, `L:0x08215660`). Defaults: colour 0xB3CCFF,
offset 0, distance 400 (`L:0x081254f0.c:21-23`).

Scene render (`L:0x0821e910.c:117-127`): `GL_FOG_MODE = GL_LINEAR`, `GL_FOG_START = start`, `GL_FOG_END = end` (the
viewport far plane instead when an override camera is active, i.e. shadow passes only), `GL_FOG_COLOR = rgba`.

Menu: start 200, end 500, `gl_Fog.scale = 1/(end − start) = 1/300`, colour (0.862745, 0.819608, 0.764706, **0**)
(fogColor has no alpha byte). The default shader always emits fog code (no `ENVIROMENT_FOG` test; that env bit is never
set by the engine), computed radially in eye space:
`v_fog = max(0, (|posEye| − 200)/300)`, `rgb = mix(rgb, fogColor, clamp(v_fog, 0, 1))`.

---

## 6. The default shader for the tram

### 6.1 Template language (obs, H; `parseTemplate` `L:0x0826a8f0.c`, `gsTemplate` `L:0x0826a420`/`0x0826a540`, `preprocess` `L:0x081ebe90.c`)

Each component file is converted line by line into a GSystem script, run once per variant:

| Source line (after stripping leading bytes 0x01–0x20; empty lines vanish) | Becomes |
|---|---|
| `#once#text` | `this.output_once("text\n");` — emitted only if this exact string (including the leading space after `#once#`) was not yet emitted **in this stage** (one `std::set` per vertex/fragment program) |
| `#code` | `code` (raw script: `if`, `for`, `$var = …`, `{`/`}`) |
| any other `text` | `this.output("text\n");` |

Inside text lines `$name` (`[A-Za-z0-9_]+`) becomes `" + $name + "` (e.g. `texture$tx_index` → `texture2`). Output
lines therefore have **no indentation** (a trailing CR from the CRLF files is probably kept, inf: the append lengths are lost; harmless). A GLSL `#…` directive is
impossible in a template. Variables set before each run: `$matFlags`, `$meshFlags`, `$envFlags`,
`$directionalCount = env>>8 & 15`, `$omniCount = env>>12 & 15`, `$spotCount = env>>16 & 15` (0 is the shared zero
variant). Literal strings: `L:` ELF at 0x82B35F0…0x82B360F (`this.output("`, `" + `, ` + "`, `\n");`,
`this.output_once("`).

Components come from the combiner's `vertexComponent`/`fragmentComponent`. A map is iterated through its `values` in
**key order** (std::map, strings by code unit, `docs/script-runtime/values.md` §5.4), so `"!base"` is first and the rest
are alphabetical (`L:0x081f2410.c`). All outputs of one stage are concatenated and compiled as **one** shader object
(GLSL 1.10, no `#version`).

### 6.2 Variant and program bits (obs, H)

Key = (`mat & matMask`, `mesh & meshMask`, `env & envMask`) = **(0x1601E, 0, 0x1000)** → omniCount 1. The tram is in
`$diffuse`, `$specular`, `$pixelLight`, `$normalMap`, `$specularMap`, `$reflection`, `$colorize` paths;
`$textureCount = 3`. `onProgram` (`default.shader`) yields:

| Field | Value |
|---|---|
| `attributes` | POSITION \| NORMAL \| TEXCOORD0 \| TANGENT = 0x03010100 |
| `textures` | TEXTURE0..3 = 0xF (count = diffuse + specular + normal + reflection) |
| `parameters` | SHADER_REFLECTION 0x800 \| SHADER_COLORIZE 0x2000 \| SHADER_VIEWORIENT 0x4000 = 0x6800 |

Vertex order: `!base, alter, color, fog, geometry, lighting, shadow, uvcoord`.
Fragment order: `!base, alter, color, colorize, fog, geometry, lighting, maps, reflection, shadow`.

### 6.3 Generated vertex shader (exact text; produced by re-implementing 6.1 over `extracted/shaders/default/*`)

```glsl
void pre_alter();
void post_alter();
void uvcoord_input();
void uvcoord_output();
void geometry_input();
void geometry_transform();
void geometry_output();
void lighting_output();
void shadow_transform();
void shadow_output();
void color_input();
void color_output();
void fog_output();
void main(void) {
uvcoord_input();
geometry_input();
color_input();
pre_alter();
geometry_transform();
shadow_transform();
post_alter();
uvcoord_output();
geometry_output();
lighting_output();
shadow_output();
color_output();
fog_output();
}
void pre_alter() {
}
void post_alter() {
}
 vec4 color;
void color_input() {
color = vec4(1.0);
}
void color_output() {
gl_FrontColor = color;
}
 varying float v_fog;
 vec4 pos;
void fog_output() {
v_fog = max(0.0, (length(pos.xyz) - gl_Fog.start) * gl_Fog.scale);
}
 varying mat3 v_matrix;
 vec4 tangent;
 vec3 normal;
 vec4 projectedPos;
void geometry_input() {
pos = gl_Vertex;
normal = gl_Normal;
tangent = gl_MultiTexCoord2;
tangent.xyz *= 0.00003051850947599719;
}
void geometry_transform() {
normal = gl_NormalMatrix * mix(vec3(0.0, 1.0, 0.0), normal, length(normal));
tangent.xyz = gl_NormalMatrix * tangent.xyz;
projectedPos = gl_ModelViewProjectionMatrix * pos;
pos = gl_ModelViewMatrix * pos;
}
void geometry_output() {
v_matrix[0] = tangent.xyz;
v_matrix[1] = cross(normal, tangent.xyz * tangent.w);
v_matrix[2] = normal;
}
 varying vec3 v_view;
 varying vec4 v_ambient;
void lighting_output() {
vec4 ambient_diffuse = gl_FrontMaterial.emission + gl_LightModel.ambient * gl_FrontMaterial.ambient;
ambient_diffuse += gl_FrontLightProduct[0].ambient;
v_ambient = ambient_diffuse;
v_view = pos.xyz;
}
void shadow_transform() {
}
void shadow_output() {
gl_Position = projectedPos;
}
 varying vec4 v_texCoord;
 vec4 uvcoord;
void uvcoord_input() {
uvcoord.xy = gl_MultiTexCoord0.xy;
}
void uvcoord_output() {
v_texCoord = uvcoord;
}
```

### 6.4 Generated fragment shader (exact text)

```glsl
void color_input();
void maps_input();
void colorize_input();
void reflection_input();
void geometry_input();
void lighting_input();
void pre_alter();
void shadow_input();
void shadow_transform();
void shadow_output();
void post_alter();
void color_output();
void reflection_output();
void fog_output();
 vec4 color;
void main (void) {
color_input();
maps_input();
shadow_input();
colorize_input();
lighting_input();
pre_alter();
shadow_transform();
post_alter();
color_output();
reflection_output();
fog_output();
shadow_output();
gl_FragColor.rgb = color.rgb;
gl_FragColor.a = clamp(color.a, 0.0, 1.0);
}
void pre_alter() {
}
void post_alter() {
}
 varying vec4 v_ambient;
 vec4 diffusemap;
 vec4 specularmap;
 vec4 illuminationMap;
 vec4 diffuse;
 vec4 specular;
void color_input() {
color = gl_Color;
}
void color_output() {
color.a = diffusemap.a;
color.rgb = (v_ambient + diffuse).rgb * color.rgb * diffusemap.rgb;
specular *= specularmap;
color.rgb += specular.rgb;
}
 uniform vec4 colorize;
 vec4 normalmap;
void colorize_input() {
diffusemap.rgb *= mix(vec3(1.0), colorize.rgb, normalmap.a);
}
 varying float v_fog;
void fog_output() {
color.rgb = mix(color.rgb, gl_Fog.color.rgb, clamp(v_fog, 0.0, 1.0));
}
 varying mat3 v_matrix;
 varying vec3 v_view;
 vec3 normal;
 vec3 view;
void geometry_input() {
normal = normalize(v_matrix * (normalmap.rgb - 0.5));
view = normalize(v_view);
}
void lighting_input() {
vec3 light =
gl_LightSource[0].position.xyz - v_view;
diffuse =
clamp(dot(normal, normalize(light)), 0.0, 1.0) * gl_FrontLightProduct[0].diffuse;
specular =
gl_FrontLightProduct[0].specular * pow(clamp(dot(normalize(reflect(light, normal)), view), 0.0, 1.0), gl_FrontMaterial.shininess);
}
 uniform sampler2D texture0;
 uniform sampler2D texture1;
 uniform sampler2D texture2;
 varying vec4 v_texCoord;
void maps_input() {
diffusemap = texture2D(texture0, v_texCoord.xy);
specularmap = texture2D(texture1, v_texCoord.xy);
normalmap = texture2D(texture2, v_texCoord.xy);
geometry_input();
reflection_input();
}
 uniform samplerCube texture3;
 uniform mat3 vieworient;
 uniform float reflection;
 vec4 reflectionMap;
void reflection_input() {
float r = reflection * (2.0 + dot(normal, view) * 1.8);
reflectionMap = textureCube(texture3, reflect(view, normal) * vieworient) * r;
}
void reflection_output() {
reflectionMap *= specularmap;
color.rgb += reflectionMap.rgb;
}
void shadow_input() {
}
void shadow_transform() {
}
void shadow_output() {
}
```

Equivalent per-fragment formula (for a hand port): `dm, sm, nm` = textures 0..2 at `uv`;
`N = normalize(TBN·(nm.rgb − 0.5))`, `Vd = normalize(posEye)`; `dm.rgb *= mix(1, colorize.rgb, nm.a)`;
`L = lightEye − posEye`; `diff = sat(N·normalize(L))·FLP.diffuse`;
`spec = FLP.specular·pow(sat(normalize(reflect(L, N))·Vd), 32)`;
`refl = textureCube(env, reflect(Vd, N)·vieworient)·0.501961·(2 + 1.8·(N·Vd))`;
`rgb = (v_ambient + diff).rgb·dm.rgb + spec.rgb·sm.rgb + refl.rgb·sm.rgb`; `rgb = mix(rgb, fogColor, sat(v_fog))`;
`a = sat(dm.a)`. Nothing is clamped before the final write (lights reach 3.0).

### 6.5 Inputs and where they come from

| Input | Source | Evidence |
|---|---|---|
| `gl_Vertex` | float3 position (node-local, as in the file) | interfaces-3d §3.1 |
| `gl_Normal` | normal as signed byte/short via `glNormalPointer` (GL-normalized) | §3.1 |
| `gl_MultiTexCoord0` | file uv0, unchanged (V stored negated; textures need REPEAT) | mesh-material §1.2 |
| `gl_MultiTexCoord2` | tangent as **non-normalized** `SHORT×4`: xyz = T·32767, w = handedness −1/0/+1 (raw) | §6.6 |
| `gl_Color` | colour array not requested; the vertex shader writes `gl_FrontColor = vec4(1.0)` anyway (material bind also sets `glColor4f(1,1,1,1)`) | `W:0x100478f0.c:111` |
| `gl_ModelViewMatrix` | `world·V` | §3 |
| `gl_ModelViewProjectionMatrix`, `gl_NormalMatrix` | `world·V·P`, inverse-transpose of the 3×3 of `world·V` | GL |
| `gl_LightSource[0].position`, `gl_FrontLightProduct[0].*`, `gl_LightModel.ambient`, `gl_FrontMaterial.*` | §4 | |
| `gl_Fog.start/scale/color` | §5 | |
| `texture0..3` | sampler units 0..3 (set on program change) | `W:0x100365f0.c:16-27` |
| `colorize` | material +0x70, every apply | `W:0x100365f0.c:75` |
| `reflection` | material +0x6C = 0.501961, every apply | `:69` |
| `vieworient` | rows 0..2 / columns 0..2 of V as 9 floats, `glUniformMatrix3fv(transpose = false)` → GLSL `r * vieworient` = eye→world rotation; set on program change | `:55-57`, `W:0x10084ad0.c:21-27` |

### 6.6 Tangent generation (`gsCalculateTextureTangents`, `W:0x10048890.c`, `L:0x08205c70.c`; obs H)

Per triangle (v0, v1, v2), `e1 = p1 − p0`, `e2 = p2 − p0`, `du1 = u1 − u0`, `dv1 = v1 − v0`, `du2`, `dv2` likewise
(file uv). With `inv(x) = x ≠ 0 ? 1/x : 0`:

```
a = dv1 ≠ 0 ? inv(du2 − dv2·du1/dv1) : 0      b = dv2 ≠ 0 ? inv(du1 − dv1·du2/dv2) : 0      W:…:114-130
c = du1 ≠ 0 ? inv(dv2 − dv1·du2/du1) : 0      d = du2 ≠ 0 ? inv(dv1 − dv2·du1/du2) : 0
T = a·e2 + b·e1          B = c·e2 + d·e1                (= the usual dP/du, dP/dv)
w = sign(dot((e2 × e1) × T, B)) ∈ {−1, 0, +1}                                                       :157-170
for each corner vertex: acc.xyz += T; acc.w += w                                                    :171-176
```

Per vertex (N = file normal): `t = normalize((N × acc.xyz) × N)`; store `short(clamp(t·32767, ±32767))` ×3 and
`short(sign(acc.w))` (−1, 0 or +1, **not** scaled) (`:187-239`). A vertex whose signs cancel gets w = 0, so its
shader bitangent is zero.

### 6.7 Draw state for the tram submesh (obs, H)

ALPHATEST material with alpha 1 → **alpha-test queue** (interfaces-3d §1.3), drawn far→near after the opaque and glow
queues with `GL_BLEND` **on** (`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, also applied to the alpha channel),
`GL_ALPHA_TEST GREATER 0.2`, depth test `LEQUAL`, depth write on, back-face culling (`W:0x10052ec0.c:175-191`,
`W:0x100478f0.c:52-74, 101`). A port emulates this with `if (a <= 0.2) discard;` plus normal alpha blending. Edge texels of
the 1-bit DXT1 alpha (bilinear between 0 and 1) are therefore partially transparent.

---

## 7. Render target and composite

### 7.1 Viewport rect → target (obs, H)

`gsViewport_OGL::render` (`L:0x082027b0.c:66-90`):

| Case | ctx rect (x, y, w, h) |
|---|---|
| no target, pixel mode | parent origin + rect |
| no target, unit mode | parent origin + rect × parentHeight |
| **target, pixel mode** | **rect as is** (origin ignored for FBO placement) |
| target, unit mode | rect × target height (`+0xD8`, e.g. (0,0,2,1) → 256×128) |

`gsPostEffect_OGL::initFrameBuffer` (`L:0x081df950.c:46-133`): allocated size = `max(ctx.w, tex.w) × max(ctx.h,
tex.h)` with tex = the target's 256×128 (sizes only grow; re-init also when the multisample need changes); current
size = ctx size. `gsTextureRenderer_OGL::init` → `initTexture` **re-specifies the target texture** with
`glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, allocW, allocH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL)` (RGBA16F/FLOAT only for
float targets), MIN/MAG `GL_LINEAR`, no mipmaps, wrap **not set** (GL default `REPEAT`) (`L:0x081e97a0.c:51-62`).
The texture object's own width/height fields stay 256/128.

The menu viewport is the gsViewPort control's client rect: `(desktopL, desktopT, $w, $h)` with
`$w = max(W, 2H)`, `$h = max(H, W/2)` (window W×H) → **aspect exactly 2**, e.g. 1800×900 at 1600×900 (desktop x = −100).

### 7.2 FBO and per-frame pass (obs H except where marked)

| Step | Detail | Evidence |
|---|---|---|
| attachments | the viewport input reports `wantsMultisample() = true` → colour renderbuffer `GL_RGBA` (MSAA with `window.antialiasing` samples, −2 per failed try, 0 allowed) + `GL_DEPTH_COMPONENT24` renderbuffer on FBO A; texture on resolve FBO B | `L:0x081df950.c:91-95`, `L:0x081ea0a0.c:75-113` |
| init clear | colour (0,0,0,0) once at creation | `L:0x081ea0a0.c:127-128` |
| begin | bind FBO A, draw buffer COLOR0, `glScissor(offX·allocW, offY·allocH, curW, curH)`; `glViewport` same (inf M: its arguments are lost in both builds; offsets `renderOffsetX/Y` = 0) | `L:0x081e95e0.c:18-22`, `L:0x081dd7f0.c:17-24` |
| clear | `glDepthMask(1)`, `glClearDepth(1)`, `glClearColor(R/255, G/255, B/255, 0)` from `clearColor` bytes, `glClear(COLOR\|DEPTH\|STENCIL)` if the clear colour's alpha byte ≠ 0 else `DEPTH\|STENCIL` → menu: **(0.266667, 0.266667, 0.266667, 0)** | `L:0x082007b0.c:158-168`, `W:0x10044ef0.c:81-91` |
| 3D | §2.2 matrices, state (`LEQUAL`, cull back, light-model ambient), scene render §4–§6 | `L:0x082007b0.c:476-490, 654` |
| end | `glBlitFramebuffer(0,0,allocW,allocH → same, COLOR, NEAREST)` A → B, unbind | `L:0x081e9480.c:27` |
| output | `gsTextureTarget_OGL::output` draws nothing; then the viewport's scene overlay (glow sprites) is drawn to the **screen** framebuffer — empty here | `L:0x082032e0.c:23-34`, `L:0x081dcd80` |

Result: an RGBA8 texture, colour un-premultiplied, alpha 0 where nothing was drawn, ≈1 on tram pixels, `a²` on
alpha-tested edge pixels (blend of `a` over 0).

### 7.3 Orientation and `setTiling(1, −1)` (obs H, reasoning inf H)

The 3D pass uses plain GL conventions (no Y flip for targets), so texture row 0 (`t = 0`) is the **bottom** of the
rendered image. A GUI image quad maps its top edge to `v = cropTop` (§4 of `docs/ui/controls.md`), which would show the
image upside down. `setTiling(1, −1)` sets `cropHeight = −1`: top v = 0, bottom v = −1; with `GL_REPEAT`,
`v ∈ (−1, 0)` reads rows top-down, i.e. upright. A port whose render targets have row 0 at the top (D3D/Vulkan/Metal)
must either flip the 3D projection's Y for this target or sample render-target textures with `v' = 1 − v`, and must
keep REPEAT (or remap v + 1) for the negative v.

### 7.4 GUI composite with `anim.shader` (obs, H)

* The gsViewPort control owns a material: `setShader("metro/shaders/menu/anim.shader")` (base program, masks 0 → one
  variant: attributes POSITION|TEXCOORD0, textures TEXTURE0, parameters 0), `setImage(target.getTexture())` → material
  slot 0, background = hotBackground = 0xFFFFFFFF (batch-B).
* Image quad (controls.md §4): `GL_QUADS`, 4 vertices in desktop pixels `(L,T) (L+W,T) (L+W,T+H) (L,T+H)` with uv
  `(0,0) (1,0) (1,−1) (0,−1)`, colour 0xFFFFFFFF; e.g. L = −100, T = 0, W = 1800, H = 900.
* `gsGuiPrimitive_OGL::render` (`L:0x081ff990.c`): `useShader` binds the material's base program (`L:0x081eda50`),
  `texture0 = 0`; unit 0 = target texture; immediate mode `glColor4ubv` / `glTexCoord2fv` (→ `gl_MultiTexCoord0`) /
  `glVertex3fv` (`:38-99`). Projection = the root GUI ortho `glOrtho(0, W, H, 0, −1, 1)` (pixel mode), modelview
  identity; blend `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, no depth, no cull (renderGui, interfaces-2d §1.5.3).
* Shader (files are plain GLSL, no `#` lines): `gl_Position = MVP·gl_Vertex; v_texCoord = gl_MultiTexCoord0.xy;`
  fragment = 0.6·tex(uv) + 0.1·tex(uv + (−0.0005, −0.001)) + 0.1·tex(uv + (−0.001, +0.0005)) +
  0.1·tex(uv + (+0.001, −0.0005)) + 0.1·tex(uv + (+0.0005, +0.001)), alpha included. `gl_Color` is ignored, so the
  control's visibility fade does **not** affect this image.
* Final screen colour: `dst = blur.rgb·blur.a + dst·(1 − blur.a)` over the parallax layer images.

---

## 8. Port checklist (this scene)

1. Script math: `quat(angle, axis)`, `qmul`, `rot(q, v) = q*vq`, `M(q)` row-major/row-vector (§1).
2. Camera: final orientation, eye/look/up (§2.1); projection with `near = max`, `far = min`, aspect from the viewport
   px size; lookAt as in §2.2.
3. Objects: world = M(q) + translation; per-object materials; `colorize` → `(R,G,B,A)/255`.
4. Sun: §4.1 formula (6:00 → dir (0.9205, 0.3907, 0)); light colours × magnitude/255, unclamped; light in eye space.
5. Fog 200→500 radial, colour 0xDCD1C3, always on.
6. Shader: either run the template expander (§6.1) and feed the built-ins as uniforms, or hard-code §6.3/§6.4;
   material flags **0x1601E** including the env cube on unit 3; tangents per §6.6.
7. Alpha-test queue: discard `a ≤ 0.2`, blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA, depth write on.
8. Target: allocate `max(viewport px, 256×128)`, clear (0.2667, 0.2667, 0.2667, **0**) + depth 1, optional MSAA,
   LINEAR, REPEAT; GL bottom-up row order (§7.3).
9. Composite: GUI quad with uv (0,0)…(1,−1), 5-tap blur, straight alpha blend.

---

## 9. Open questions

1. `glViewport` arguments inside `gsTextureRenderer_OGL::startRender` are lost in both builds (`L:0x081e95e0.c:22`,
   `W:0x100362d0.c`); assumed equal to the scissor. Disassemble to confirm.
2. Frame ordering (inf M): the image quad (layer z·256) sits in the same parent clip area as the native viewport
   (z·256+0x80), so the quad probably samples **last frame's** texture, and on the very first frame a texture without
   storage (GL returns (0,0,0,1) for an incomplete texture → one black frame?). Verify with a capture.
3. `GL_POSITION` is transformed by the modelview current at `glLightfv` time; the engine pre-multiplies by V, so the
   modelview must be identity then. The `glPushMatrix` in `renderScene` suggests it is the GUI identity; not proven.
4. Handedness: with V stored negated in the files, which sign of `w` do shipped meshes get, and does the shader's
   `cross(N, T·w)` bitangent then match the normal maps? Needs a visual check of a normal-mapped mesh.
5. Light count = 1 assumes no other gsLight under `$scene` (only the sun's). Count the scene light list at run time to
   be sure.
6. Whether the trams really appear fully fog-coloured at the far end (fog factor ≥ 0.93 for tram 1 at t = 0) could be
   checked against a screenshot of the original game.
7. DDS sampler state (filter, wrap, anisotropy) for `tram03*.dds`/`envmap.dds` is lost (interfaces-2d §5 q2); REPEAT
   is required by the negative V of the mesh.
