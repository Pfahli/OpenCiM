# Shadow maps (`gsShadowEffect_OGL`)

How GSystem renders sun shadows: a post effect in the main viewport's effect chain renders the scene once from a
light camera into a depth texture, warped by a **trapezoid-to-square perspective map (TSM)**, then renders the scene
from the main camera with that depth texture on texture unit 7. This document gives the light-camera fit, the warp,
the three uniforms, the depth pass and the receiver shader math. It builds on
[interfaces-2d-output.md](interfaces-2d-output.md) §1.5.3/§1.6 (effect chain, render context), [interfaces-3d.md](interfaces-3d.md)
§1.3-1.4 and §3.3-3.5 (shadow-pass queue rules, uniforms, variant reduction) and [scene-basics.md](scene-basics.md)
(matrix, quaternion, camera and sun conventions), and does not repeat them.

**Citations.** `L:0x<addr>.c:<line>` = `decompiled/Linux/functions/` (real C++ names), `W:0x<addr>.c:<line>` =
`decompiled/GSystem-v2/functions/` (Windows GSystem.dll). `L:0x081e1660@<insn>` = instruction address in the Ghidra
listing of that Linux function: the pseudocode of `input` (`L:0x081e1660`, 22 KB of x87 code) lost the operands of
nearly every by-value `gsVector` operator, so the math below was read from the listing (dumped headless from the
`CiMLinux` project with `tools/ghidra/DumpAsm.java`) and cross-checked against the Windows pseudocode. `S:<path>` = `extracted/<path>`,
`E:<line>` = `extracted/metro/scripts/engine.script`. **obs** = read in the code, **inf** = inferred. Confidence H/M/L.

**Conventions.** Row vectors, `v' = v·M`, translation in m[12..14] ([scene-basics.md](scene-basics.md) §0-2). `M ⊗ p` below
means the engine's `operator*(gsMatrix, gsVector)` (`L:0x0810deb0`): `(p, 1)·M`, divided by w, w treated as 0 when
|w| < 1e-5. A matrix uploaded with `glUniformMatrix4fv(transpose = false)` appears in GLSL as `M·v ≡ v·M_engine`.
Effect offsets are Linux; **Windows = Linux + 0x38** for every effect field (checked for all fields of §1).

---

## 0. Corrections to the existing docs (obs)

| Doc says | Engine does | Evidence |
|---|---|---|
| shadow-map format lost, inferred `DEPTH_COMPONENT` (interfaces-2d-output §3.2) | `glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, 0)`, MIN/MAG `GL_LINEAR`, wrap `GL_CLAMP_TO_BORDER`, border (1,1,1,1) | `L:0x081e97a0.c:37-40, 93-102` |
| `glPolygonOffset` in the shadow pass (interfaces-2d-output §1.5.3) | `(factor 2, units 4)`, or `(0, 0)` with `useBackSide`; **without effect** for every shipped shadow-map program, because they all write `gl_FragDepth` (§4.4) | `L:0x081e1660@081e52c8`, `@081e46eb`, `W:0x10031dd0.c:732-742` |
| shadow-pass culling: "doubled culling radius" (interfaces-3d §1.3) | the planes, AABB and camera position used for culling and LOD in the light pass are the **main camera's** (renderScene computes them before switching to the override camera); only projection, view and billboard axes come from the light camera | `L:0x082007b0.c:300-470` vs `490-640` |
| ctx+0x74 far distance "M inf" (interfaces-3d §1.7) | ctx+0x70 = near, ctx+0x74 = far, written by renderScene for each pass; the shadow effect presets them to the light near/far | `L:0x082007b0.c:309-310, 475-476, 663-664` |
| env-flag lines `0x10031dd0.c:557,723,820` | the current Windows pseudocode has them at 564 (OR 4), 739 (OR 0x20), 835 (OR 0xA) | `W:0x10031dd0.c` |

---

## 1. Script object and construction

### 1.1 Fields (obs H; registrar `L:0x081dd270.c:21-46`, defaults `L:0x081e0df0.c:15-45`)

| Field (id) | Linux / Windows | Type | Default | Game (`E:255-264`) | Meaning |
|---|---|---|---|---|---|
| `viewport` (7) | +0xCC / +0x104 | gsViewPort | null | `MAIN.$viewport` | the viewport whose camera the shadow map is fitted to; the effect does nothing while null |
| `light` (8) | +0xD0 / +0x108 | gs3DElement | null | `$sun.light` | only its **global position** is read (the sun puts it at `sunDir·20000`, scene-basics §4.1); no orientation, no colour |
| `screenSpaceBlur` (9) | +0xE0 / +0x118 | bool | false | false | §6 |
| `safeMargin` (10) | +0xF0 / +0x128 | float | 0 | 0.4 | max. growth of the warp trapezoid toward the eye, in light NDC units (§3.5) |
| `clipYPlane` (11) | +0xF4 / +0x12C | bool | false | **true** | fit to the view footprint on a horizontal plane (§3.2) and use the safe-margin trapezoid (§3.5) |
| `useBackSide` (12) | +0xF5 / +0x12D | bool | false | false | env flag 0x20 (cull front faces) and polygon offset (0,0) in the light pass |
| `maxDistance` (13) | +0xF8 / +0x130 | float | −1 | −1 | caps the main far distance for the fit and the light-pass culling; changes `shadowParams` (§3.6) |
| `yPlane` (14) | +0xFC / +0x134 | float | 0 | −10, then `−$y − 10` every camera move (`S:metro/scripts/ui.script:192`) | clip plane at world `y = cameraTarget.y + yPlane` (≈ −10 in game, −24 in the underground view) |

Other members: +0xA0 screen-space RT, +0xA4 input (the previous output), +0xA8 depth texture (ref-counted),
+0xC4 bypass, +0xD4/+0xD8 map width/height, +0xDC light camera (`gsCamera_OGL`), +0xE4 depth renderer
(`gsTextureRenderer_OGL`), +0xE8 second blur RT, +0xEC `shaders/shadow/blur.shader`.

### 1.2 Creation (obs H)

* `video.new_shadowEffect(w, h)` (`L:0x08213410.c`) → `initShadowEffect(video, w, h)` (`L:0x081e0bf0.c`): null with
  *"Cannot initialize shadows (requires GLSL)"* unless `video.shaders` (`:24`). The game passes `$settings.$shadows × 1024`
  for both, i.e. **2048×2048** at the default `$shadows = 2` (1 → 1024; 0 = no effect) (`E:255-256`).
* `create` (`L:0x081de690.c:43-95`): loads the blur shader, makes a `gsTextureRenderer_OGL` and calls
  `initDepth(w, h)`; on failure logs *"Shadowmap framebuffer creation failed"* and sets bypass (+0xC4 = 1); always
  creates the light camera `gsCamera_OGL(video, 0, null)` (its near/far stay −1).
* `initDepth` (`L:0x081e9dc0.c:58-80`): FBO with the depth texture (§0 row 1) on `GL_DEPTH_ATTACHMENT` and an
  RGB (`GL_RGB`) renderbuffer on `GL_COLOR_ATTACHMENT0`, `glDrawBuffer(GL_NONE)`, depth cleared once.

---

## 2. Per-frame flow (obs H)

The viewport's `renderOutput` runs `effect.setup(ctx, out); out = effect` in `addEffect` order, then `out.render(ctx)`
(interfaces-2d-output §1.5.3). The game adds the shadow effect first, then bloom (`E:264`, `E:267-269`):

1. **`shadow.input(ctx, viewport)`** (`L:0x081e1660`, slot "setup"): if bypass, no light or no viewport → return.
   Otherwise: fit the light camera (§3), render the **light pass** (§4) into the depth FBO, compute the uniforms
   (§3.6) into ctx, restore the viewport's `maxDistance` and return.
2. **`bloom.input(ctx, shadow)`**: renders its input, i.e. calls `shadow.output(ctx)`, into the bloom FBO (`ctx+0x2D = 1`),
   then runs the bloom chain. Without bloom, step 2 is the final `out.render(ctx)` and draws straight to the frame.
3. **`shadow.output(ctx)`** (`L:0x081dd5c0.c:20-55`): the **main pass** (§5): unit 7 ← depth texture with compare
   mode, `ctx+0x234 |= 2`, `input.render(ctx)` (the viewport's normal 3D pass with the main camera), then unit 7 back
   to `DEPTH_TEXTURE_MODE = LUMINANCE`, `COMPARE_MODE = NONE`, texture unbound and `GL_TEXTURE_2D` disabled, flags
   restored. When the effect is inactive it only calls `input.render(ctx)`.
4. `bloom.output` composites; then the scene overlay pass (glow sprites).

So the scene is drawn twice per frame: light pass, then main pass. `output` does nothing else (no composite of its
own). `input` saves and restores ctx+0x08..+0x28 (rect, scissor, lodFactor), the whole camera block ctx+0x30..+0x1DF,
ctx+0x230 and ctx+0x234 around the light pass (`L:0x081e1660@081e1b12-081e1bb0`, `@081e477c-081e4803`). The uniforms
ctx+0x238/+0x278/+0x2B8 are **not** restored; they stay valid for step 3.

---

## 3. Light camera, warp and uniforms

### 3.1 Main-camera frustum (obs H; `L:0x081e1660@081e16c0-081e1b0d`, `W:0x10031dd0.c:200-268`)

`cam` = `viewport.camera` (the script `gsCamera`, interface slots +8 `getPosition`, +0xC `getTarget`, +0x10 `getUp`,
+0x18 `getFOV`, +0x1C `getMinDistance`, +0x20 `getMaxDistance`; scene-basics §2.1 for their formulas).

```
eye = cam.getPosition();  tgt = cam.getTarget();  up = cam.getUp()
F   = norm(tgt − eye)
t   = tanf(fov·0.0174532924·0.5)                        fov in degrees (vertical)
a   = viewport.width / viewport.height                  gsControl +0xA8/+0xAC (the main viewport: window pixels)
R   = norm(F × up) · (t·a)                              gsVector::normalize(len)
U   = norm(R × F) · t
near = max(vp.minDistance, cam.minDistance if ≠ −1)                                      @081e6e08
far  = min(vp.maxDistance, cam.maxDistance if ≠ −1, this.maxDistance if ≠ −1)            @081e6de0, @081e4b90
viewport.setMaxDistance(far)        // restored to the old value when input() returns   @081e18fa, @081e5067
```

`setMaxDistance` also pushes near/far to the OGL viewport, so the light pass culls with this far (§4.3). In game:
`vp.maxDistance = 600` (`E:251`), camera far `160 + zoom + 1.5·y` (310 at start), camera near ≈ 25-48
(`S:metro/scripts/ui.script:195-196`). If the viewport has a sub-rect (`setViewportRect`, +0x1B8..+0x1C4, default
(0,0,1,1); only `debug.script` changes it): `F −= R·cx·0.25 + U·cy·0.25` with `c = 2(0.5 − x)/w − 1` (and y/h),
`F = norm(F)`, `R *= 1.25·w`, `U *= 1.25·h` (`@081e1978-081e1b0d`).

### 3.2 Frustum points (obs H; `@081e1c25-081e2673`)

Corner directions `d0 = F+R+U, d1 = F+R−U, d2 = F−R+U, d3 = F−R−U` (top-right, bottom-right, top-left, bottom-left of
the screen).

| `clipYPlane` | Points C0..C7 |
|---|---|
| **true** (game) | `Ci = eye + di·far` (i < 4), then each is moved onto the plane `y = tgt.y + yPlane` along its ray: if `(Ci − eye).y ≠ 0`, `s = (tgt.y + yPlane − eye.y)/(Ci − eye).y`, `Ci = eye + (Ci − eye)·s` (no range check: rays above the horizon give s < 0, a point behind the camera; a horizontal ray keeps its far point) (`@081e1f2b`, `@081e6218`). Then **C4..C7 = C0..C3**. Near distance unused. |
| false | `C(2k) = eye + dk·far`, `C(2k+1) = eye + dk·near`, k = 0..3 (`@081e20c0-081e2673`) |

### 3.3 Light basis and fit (obs H; `@081e2689-081e3661`, `W:0x10031dd0.c:444-539`)

```
Ls = light.getGlobalPos()                      the sun: sunDir·20000 (a point, not a direction)
M  = (C0 + … + C7)·0.125
Z  = norm(M − Ls);   X = norm((1,0,0) × Z);   Y = X × Z
repeat 3 times:
    umin = vmin = 1e5,  gmin = 1e6,  umax = vmax = −1e5,  gmax = 0
    for each Ci:  (α, β, γ) = gsSolve(Y, X, Z, Ci − Ls)      Ci − Ls = α·Y + β·X + γ·Z (Cramer, L:0x0810b020)
                  u = α/γ, v = β/γ                           tangents of the angles from Z
                  update min/max of (u, v, γ)
    Z = norm(Z + Y·(umin+umax)/2 + X·(vmin+vmax)/2)          aim at the centre of the bounds
    Y = norm(X × Z);   X = Y × Z
```

The last min/max were measured before the last re-aim (inf: harmless, the change is tiny). `X` flips sign every
iteration (`(X×Z)×Z = −X`); only the final values matter. Degenerate if Z ∥ (1,0,0) (sun on the east/west horizon):
X = 0 and the solve returns 0, so u, v become NaN (inf).

### 3.4 Light camera and projection (obs H; `@081e3667-081e3885`, `W:0x10031dd0.c:540-579`)

| Quantity | Value |
|---|---|
| fov (degrees) | `max(3·atan(max(umax−umin, vmax−vmin)/2)·57.29578, 0.001)`: 1.5× the full angle of the bounding square |
| light camera | `setPosition(Ls)`, `setTarget(Ls + Z·gmax)`, `setUp(X)`, `setFov(fov)` (slots +0xC..+0x18) |
| ctx | `+0x70 = 0.99·gmin` (near), `+0x74 = 1.01·gmax` (far), `+0x234 \|= 4`, `+0x08/+0x0C = 0`, `+0x10/+0x14 = map w/h`, scissor = same, `+0x230 = light camera` |
| `V_L` | `gsViewMatrix(Ls, Ls + Z·gmax, X)`: axes x = Y, y = X, z = −Z |
| `P_L` | `gsProjectionMatrix(fov·0.017453292, aspect = w/h = 1, 0.99·gmin, 1.01·gmax)` (scene-basics §2.2), times the viewport sub-rect matrix `S(1/w,1/h,1)·T(2(0.5−x)/w−1, 2(0.5−y)/h−1, 0)` if the viewport has one |
| `VP` | `V_L·P_L` |

renderScene builds the identical `P_L`/`V_L` from the override camera (`L:0x082007b0.c:490-603`): near =
`max(ctx+0x70, cam.near)`, far = `min(ctx+0x74, cam.far)`, the light camera's near/far being −1. It also applies the
viewport **mirror** to the projection, which `input` does not (§8 q3).

### 3.5 Warp, `clipYPlane = true` (obs H; `@081e3aa6-081e44ea`; Windows `W:0x10031dd0.c:596-698`, `gsTrapezoidToSquare` = `W:0x1002fcb0`)

```
Pi = (VP ⊗ Ci).xy  (z := 0),  E = (VP ⊗ eye).xy           light NDC; the footprint fills about ±0.67
B = (P1 + P3)/2   T = (P0 + P2)/2                         bottom (near) / top (far) edge midpoints
D = norm(B − T)   W = norm(P1 − P3)                       k(x) = clamp(x, 0, safeMargin)
P1 += D·k(D·(E−B)) + W·k(W·(E−B))
P3 += D·k(D·(E−B)) − W·k(W·(B−E))
D' = norm(T − B)  W' = norm(P0 − P2)                      (B, T, W, W' all from the unmodified points)
P0 += D'·k(D'·(E−T)) + W'·k(W'·(E−T))
P2 += D'·k(D'·(E−T)) − W'·k(W'·(T−E))
shadowWarp = gsTrapezoidToSquare(t0 = P3, t1 = P1, t2 = P0, t3 = P2)
```

Each corner moves outward by at most `safeMargin` along the edge and away from the opposite edge, and only when the
projected eye lies beyond it. The result is a general quadrilateral, not a true trapezoid.

**`gsTrapezoidToSquare(t0, t1, t2, t3)`** (`L:0x081e0f00.c`, start checked in the listing): builds rows r0 (x),
r1 (y), r3 (w) of a column-vector matrix acting on (x, y, ·, 1), r2 = (0,0,1,0), then transposes:

```
a  = (t2 − t3)/2;   r0 = (a.x, a.y | ·),   r1 = (a.y, −a.x | ·)            (xy part | constant term)
i  = intersection of line t0–t3 with line t1–t2   (standard 2-line formula; parallel → return identity)
r0.c = −r0·i;   r1.c = −r1·i                                               x' = a·(p−i), y' = a⊥·(p−i)
k  = −(x'(t3) + x'(t2)) / (y'(t2) + y'(t3));   r0 += k·r1                  shear: top edge symmetric
r0 /= x'(t2);   r1 /= y'(t2);   r3 = r1;   r1.c += 1                       w = y'/y'(t2); x/w = ±1 on the side lines
c  = −0.5·(Y(t2)/W(t2) + Y(t0)/W(t0));   r1 += c·r3                         centre y
r1 *= −W(t0)/Y(t0)                                                         t0 → y = −1, top edge → y = +1
any zero denominator → identity
```

Result: t3 → (−1, 1), t2 → (1, 1), t0 → (−1, −1), t1 → (1, y₁) with y₁ ≈ −1 (exactly −1 only if the bottom edge is
parallel to the top edge). The side lines map to x = ±1, lines parallel to the top edge to horizontal lines, and the
region near the projection centre i (the near edge) gets the most texels. Memory layout as uploaded:
`m[0..3] = (r0.x, r1.x, 0, r3.x)`, `m[4..7] = (r0.y, r1.y, 0, r3.y)`, `m[8..11] = (0,0,1,0)`,
`m[12..15] = (r0.c, r1.c, 0, r3.c)`. z passes through unchanged.

**`clipYPlane = false`** (not used by the game, M): `axis = norm(P0+P2+P4+P6 − P1−P3−P5−P7)`. If zero the trapezoid
is (P7, P3, P2, P6). Otherwise `gsFitTrapezoid` (`W:0x10030310.c`, inlined at `@081e5320-081e6214`) tries side-line
slopes `s = −1.0, −0.95, …, 0.5` around `axis` and keeps the best one; no safe margin. Not specified further here.

### 3.6 Uniforms as uploaded (obs H; `@081e44f2-081e4907`, `W:0x10031dd0.c:700-773`)

| ctx | Uniform | Value |
|---|---|---|
| +0x238 | `shadowTransform` | `inverse(V_c)·V_L·P_L` where `V_c = gsViewMatrix(eye, tgt, up)` of the main camera (re-read after the light pass): maps **main-camera eye space → light clip space** (unwarped) |
| +0x278 | `shadowWarp` | §3.5 |
| +0x2B8 | `shadowParams` | `maxDistance = −1`: `(0.5/mapWidth, −far_L, −far_L, 1.5e-10·far_L)` with `far_L = 1.01·gmax`; else `(0.5/mapWidth, −0.7·maxDistance, −maxDistance, 1.5e-10·maxDistance)` |

Uploaded on program change (interfaces-3d §3.3). Every `onProgram` that handles `ENVIROMENT_SHADOWMAP` requests all
three plus `TEXTURE7`; `SHADOWMAP_RENDER` requests `shadowWarp` only (`S:shaders/default/default.shader:55-62`).

### 3.7 Worked example (inf: computed with the formulas above in double precision, not captured in game)

Inputs: camera of `E:228-237` at pivot (0,0,0): `quat(0.855, X)·quat(0.001, Y)`, offset (0,0,150) → eye
(−0.0984, 113.186, 98.433), tgt (0,0,0), up (0.000755, 0.656219, −0.754571); fov 40, aspect 16/9, far 310;
`yPlane` −10 (plane y = −10); sun at 09:00 (`d = 0.375`, latitude π/4) → dir (0.495618, 0.713250, 0.495618),
Ls = (9912.35, 14265.0, 9912.35); safeMargin 0.4; map 2048.

| Step | Result |
|---|---|
| C0..C3 on the plane | (154.68, −10, −123.76), (80.19, −10, 51.20), (−154.43, −10, −124.07), (−80.29, −10, 51.04) |
| final Z, X(up), Y | (−0.493895, −0.713371, −0.497162), (0.0000046, −0.571767, 0.820416), (−0.869522, 0.405197, 0.282396) |
| u, v, γ ranges | ±0.006694, ±0.003594, 19942.0…20145.0 |
| fov, near, far | 1.150648°, 19742.60, 20346.48 |
| P0..P3 / E | (−0.6667, −0.3579), (−0.0981, 0.3579), (0.6667, −0.3565), (0.5961, 0.3559) / (0.5684, 0.2005) |
| after safe margin | P2 → (1.0667, −0.3560), P3 → (0.9159, 0.3549); P0, P1 unchanged |
| `shadowWarp` rows (memory) | (−1.15423, 0.002435, 0, 0.000634), (0.336292, −2.23167, 0, −0.580840), (0,0,1,0), (0.350898, 0.202832, 0, 0.792519) |
| warped P3, P1, P0, P2 | (−1,−1), (1, −1.0199), (1,1), (−1,1) |
| `shadowParams` | (0.000244141, −20346.5, −20346.5, 3.05197e-6) |
| lookup of world (0,0,0) | map uv (0.6127, 0.4565), stored depth 0.433533, compared depth 0.432875 (bias 0.0002 + 150·3.05e-6) |

---

## 4. Light pass (depth map)

### 4.1 Target and state (obs H)

| Item | Value | Evidence |
|---|---|---|
| depth texture | w×h `GL_DEPTH_COMPONENT24` (`GL_FLOAT` upload), `GL_LINEAR`/`GL_LINEAR`, `CLAMP_TO_BORDER`, border 1.0 (outside = lit), no mipmaps, aniso 1 | `L:0x081e97a0.c:37-44, 93-102` |
| bind for rendering | `startRender(0,0,1,1)`: bind FBO, `glDrawBuffer(GL_NONE)`, `glScissor(0,0,w,h)`, `glViewport` (args lost; inf = scissor) | `L:0x081e95e0.c`, `L:0x081e1660@081e473f` |
| clear | renderScene: `glClearDepth(1)`, `glClear(DEPTH \| STENCIL)` (+COLOR if the viewport clear colour has alpha; no colour buffer is written anyway) | `L:0x082007b0.c:160-169` |
| polygon offset | `glEnable(GL_POLYGON_OFFSET_FILL)`, `glPolygonOffset(2, 4)` (useBackSide: (0,0)); disabled after the pass | `@081e46cb`, `@081e52c8`, `@081e4777` |
| cull | `glCullFace(GL_BACK)`; `GL_FRONT` with env 0x20 (useBackSide) or an odd mirror count | `L:0x082007b0.c:488-489` |
| end | `endRender`: unbind FBO (no MSAA resolve, no mipmaps for the depth target) | `L:0x081e9480.c` |
| matrices | projection `P_L`, modelview `world·V_L` (fixed-function, read by the shaders) | `L:0x082007b0.c:490-610` |
| env flags | `ctx+0x234 = saved \| 4` (`\| 0x20` with useBackSide); no light count added, no GL lighting | `@081e37a5`, `@081e46eb`, interfaces-3d §1.2 |

### 4.2 What is drawn (obs H unless marked)

Everything the main viewport would draw, through the same scene traversal, with the shadow-pass rules of
interfaces-3d §1.3-1.4: `MATERIAL_NO_SHADOW` (grass, the secondary-view arrow) and decals skipped; glow submeshes
drawn as plain opaque; blended submeshes go to the alpha-test queue; variant key reduced to
`mat & (DIFFUSEMAP|ALPHABLEND|ALPHATEST)`, `env & 4` (§3.5 there). Additionally:

* camera-attached meshes (mesh flag 0x1000: sky dome, sun disc) are skipped (`W:0x1004a4d0.c:84-90`);
* water and water edge are skipped (their shaders return no components for `SHADOWMAP_RENDER`; terrain.md §6.4);
* trees (creation flag 0x4000) use their coarsest LOD (grid-items.md §3.4);
* shaders whose `envMask` lacks `SHADOWMAP_RENDER` (`bump`: FOG \| LIGHTS; `path`, `route`, `selection`, `data`: 0) would
  draw their **normal** variant here (unwarped `gl_Position`, no `gl_FragDepth`); whether any of them reaches this pass
  in game is open (§8 q2).

### 4.3 Culling and LOD use the main camera (obs H)

renderScene computes the frustum planes (ctx+0x150), the octree AABB (ctx+0x78), the camera position (ctx+0x30) and
the far distance from the **viewport's own camera** first (`L:0x082007b0.c:300-470`) and only then replaces projection,
view (ctx+0x90/+0xD0), ctx+0x110 and the billboard axes with the override camera (`:490-640`). So casters are those
inside the main frustum with the bounding radius **doubled** (`W:0x1004a4d0.c:131-152`), cut at the far distance of
§3.1, with LOD distances from the main eye. A caster outside the main frustum (even if its shadow falls inside) is
not drawn (inf, follows from the code).

### 4.4 Shadow-map program (obs H; `S:shaders/default/shadow.vertex:12-39`, `shadow.fragment:74-81`)

```glsl
// vertex (geometry_transform first: projectedPos = gl_ModelViewProjectionMatrix * pos = clip_L)
v_shadowPos = projectedPos;                       // unwarped light clip position
gl_Position = shadowWarp * projectedPos;          // warped xy and w
gl_Position.z = 0.0;                              // no near/far clipping at all
// fragment
gl_FragDepth = v_shadowPos.z / v_shadowPos.w * 0.5 + 0.5        // unwarped light NDC depth in [0,1]
             + step(color.a, 0.2);               // only if ALPHABLEND|ALPHATEST: cut-out texels write ≥ 1 (clamped to 1 = empty)
```

Terrain and trees use these default components (terrain.md §6.1, grid-items.md §3.6; trees add only the sway of
`alter.vertex`, applied in both passes); particles write the same depth with a `step(colormap.a, 0.2)` cut-out
(`S:shaders/particles/particles.fragment:37`); water is not drawn. Because the depth is written explicitly,
the polygon offset of §4.1 never applies (GL rule: offset modifies the rasterized depth, which `gl_FragDepth`
replaces; inf H), casters nearer than `near_L` clamp to depth 0, and alpha-tested geometry is also discarded by the
fixed-function alpha test `> 0.2` of the alpha-test queue.

---

## 5. Main pass (receivers)

### 5.1 Binding (obs H; `L:0x081dd5c0.c:20-55`, `W:0x10030b00.c`)

`glActiveTexture(GL_TEXTURE7)`, `glEnable(GL_TEXTURE_2D)`, bind the depth texture,
`GL_DEPTH_TEXTURE_MODE = GL_INTENSITY`, `GL_TEXTURE_COMPARE_MODE = GL_COMPARE_R_TO_TEXTURE`,
`GL_TEXTURE_COMPARE_FUNC = GL_LEQUAL` (lit = 1 when `r ≤ stored`), back to unit 0, `ctx+0x234 |= 2`
(`ENVIROMENT_SHADOWMAP`), render the viewport. With `GL_LINEAR` the hardware filters the comparison
(2×2 PCF on common drivers; implementation-defined in GL 2.1, inf).

### 5.2 Receiver math, default shader (obs H; `S:shaders/default/shadow.vertex:20-31`, `shadow.fragment:39-72`)

```glsl
// vertex, after geometry_transform (pos = gl_ModelViewMatrix * pos = main eye space)
shadowPos      = shadowTransform * pos;                 // light clip, unwarped
v_shadowCoord  = shadowWarp * shadowPos;
v_shadowCoord.z = shadowPos.z / shadowPos.w;            // light NDC depth (replaces the warped z)
// fragment
vec4 c = v_shadowCoord / max(0.01, v_shadowCoord.w) * 0.5 + 0.5;
c.z    = v_shadowCoord.z * 0.5 + 0.4998 + v_view.z * shadowParams.w;   // v_view = eye-space position (z < 0)
o      = shadowParams.x;                                               // half a texel
shadow = 0.25 * Σ shadow2D(texture7, c.xyz + (±o, ±o, 0))              // 4 taps
diffuse.rgb  *= 0.2 + 0.8 * shadow.rgb;                  // only these three terms are shadowed
specular     *= shadow;
reflectionMap *= 0.5 + 0.5 * shadow;
```

Depth bias toward the light: 0.0002 plus `|z_eye|·1.5e-10·far_L` (≈ 3e-6 per unit of view depth with `far_L` ≈ 20 000).
The `$pixelLight` branch in `shadow.fragment:49-53` (fade `mix(1, shadow·0.25, smoothstep(shadowParams.z,
shadowParams.y, v_view.z))`) is not taken: `$pixelLight` is never set in that component, and components are separate
template scripts (scene-basics §6.1; the port's combiner emits `shadow *= 0.25`, M, §8 q1).

Other shipped receivers (obs H):

| Shader | Difference |
|---|---|
| trees (`S:metro/shaders/trees/alter.vertex:20-23`, default components otherwise) | `post_alter: shadowPos.z −= shadowPos.w·0.02` (extra bias 0.01 in depth) |
| water (`S:metro/shaders/water/water.fragment:37-45, 80-84`) | `c = v_shadowCoord / v_shadowCoord.w` (no max), z constant **0.49995**, depth from `v_shadowPos` (unwarped); **always** applies the fade `smoothstep(shadowParams.z, shadowParams.y, v_view.z)`, which with `y = z = −far_L` is a zero-width edge (undefined in GLSL; on common GPUs it evaluates to 1 for every visible point, i.e. full shadow); then `diffuse *= 0.2 + 0.8·s`, `specular *= s`, `ref *= 0.5 + 0.5·s` |
| particles (`S:shaders/particles/particles.vertex:38-48`) | sampled **per vertex** (4 taps in the vertex shader), z constant 0.49995, same zero-width fade |
| `metro/shaders/terrain/terrain.*` | dead (terrain.md §6.1); the live terrain uses the default components |

---

## 6. `screenSpaceBlur = true` (not used by the game; obs M)

After the light pass, the effect (re)creates two screen-sized RTs when the viewport grew, renders the scene with
`ctx+0x234 |= 0xA` (`SHADOWMAP | SHADOW_RENDER`: receivers output only the shadow term) into the first and blurs it
twice with `shaders/shadow/blur.shader` (`@081e490d-081e5060`). `output` then binds that colour RT on unit 7 with
`|= 0x10` (`ENVIROMENT_SHADOW`), and the shaders read it at `gl_FragCoord.xy·(1/1600, 1/900)` (hard-coded size).

---

## 7. Port checklist

1. **Object**: script fields and defaults of §1.1; `new_shadowEffect(w, h)` null without GLSL; bypass (pass-through)
   when the depth target fails or `light`/`viewport` is null.
2. **Target**: w×h 24-bit depth texture, linear filtering with compare `LEQUAL` (a comparison sampler), clamp to a
   border of 1.0 (or clamp to edge plus `uv` outside [0,1] → lit). Clear to 1 each pass.
3. **Fit (per frame, before the main pass)**: §3.1 (including the temporary viewport far), §3.2 clip-plane points,
   §3.3 three-iteration fit, §3.4 light camera (fov ×3 half-angle, near 0.99·γmin, far 1.01·γmax, aspect w/h),
   §3.5 safe-margin quad and `gsTrapezoidToSquare` exactly as written (identity fallbacks included), §3.6 uniforms.
   §3.7 gives numbers to unit-test against.
4. **Light pass**: same scene traversal as the main pass but with `V_L`/`P_L`; culling, LOD and distance from the
   **main** camera with doubled radius; env 4 variants; skip NO_SHADOW, decals, flag-0x1000 meshes and water; trees at
   coarsest LOD; back-face culling (front with `useBackSide`). In the shader: `gl_Position = shadowWarp·clip`, no depth
   clipping (z = 0, or depth clamp), depth = `clip.z/clip.w·0.5 + 0.5` written per fragment (cut-out ≤ 0.2 → discard).
   Polygon offset can be omitted (it has no effect in the original).
5. **Main pass**: env flag 2, unit/binding 7 = the comparison sampler, `shadowTransform`/`shadowWarp`/`shadowParams`
   as in §3.6, shader math of §5.2 (translate `shadow2D` to a vec4 of the comparison result; give water/particles'
   `smoothstep(e, e, x)` a defined result, `step(e, x)` = 1).
6. **Order**: shadow `input` before bloom's; the main pass is the shadow effect's `output`, called by bloom's input
   (or directly as the final output without bloom). Restore the viewport far distance after the fit.
7. **Clip-space conventions**: the engine computes depth as `ndc·0.5 + 0.5` in both shaders, so keep GL-style
   `P_L` and the explicit mapping; on D3D/Vulkan only the texture `v` of the lookup must follow the y convention used
   when rasterizing the map (keep write and read consistent).
8. **Uniform refresh**: the three uniforms change every frame but are sent only on program change in the original
   (interfaces-3d §3.3); a port must send them per frame/pass.

---

## 8. Open questions

1. `$pixelLight` in `shadow.fragment` (M): unset in that component, so the distance fade is never compiled, if
   template variables are per component as in the port's combiner. Confirm the engine's gsTemplate variable scope
   (`L:0x0826a420`/`0x0826a540`); if they were shared, lit receivers would fade shadows with the degenerate
   `smoothstep(−far_L, −far_L, z)` like water.
2. Which items with an `envMask` lacking `SHADOWMAP_RENDER` (path/route/selection/data overlays, bump) are in the
   scene and visible-group mask during the light pass, and do primitives/particles without shadow variants draw there?
   They would write unwarped depth into the map.
3. Viewport mirror: renderScene mirrors `P_L` but `input` does not include the mirror in `shadowTransform`/`warp`;
   only matters for mirrored main viewports (none in game).
4. Camera pitch: in clip mode a corner ray above the horizon lands behind the camera (§3.2). Does the floating
   camera's pitch range ever allow that (it would flip the warp quad)?
5. Hardware PCF: the look depends on the driver filtering a `GL_LINEAR` compare texture (2×2 bilinear PCF vs. a
   single nearest comparison).
6. `startRender`'s `glViewport` arguments were lost (inferred = the scissor rect, the full map).
7. The `clipYPlane = false` trapezoid search (`gsFitTrapezoid`, metric `local_120`) was not specified; it is unused by
   the shipped scripts.
8. The fit uses `Ls` = the sun light at 20 000 units from the world origin; when the sun is below the horizon (night)
   the map is rendered from below the ground. Whether the sky/sun scripts dim or disable shadows then was not checked.
