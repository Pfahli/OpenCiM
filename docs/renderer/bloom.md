# Bloom and tone mapping (`gsBloomEffect_OGL`)

What the "HDR lighting" option does to the main 3D viewport: the scene goes into an RGBA16F buffer, a 4-level blur
pyramid is built from it, a 64×32 level is read back for auto-exposure, and `final.shader` composites scene + bloom
with exposure, saturation and a soft clip. This file is the exact recipe a port needs. Context lives in
[interfaces-2d-output.md](interfaces-2d-output.md): the effect chain in §1.5.3, the render context in §1.6,
`gsIVideoOutput_OGL`/`gsPostEffect_OGL` in §2.8, the FBO wrapper in §2.9 and the class table in §2.12.

**Citations.** `L:0x<addr>.c:<line>` = `decompiled/Linux/functions/` (real C++ names), `W:0x<addr>.c:<line>` =
`decompiled/GSystem-v2/functions/` (Windows GSystem.dll), `E:<line>` = `extracted/metro/scripts/engine.script`,
`S:<file>` = `extracted/shaders/bloom/<file>`. **obs** = read in the code, **inf** = inferred. Confidence H/M/L.
Windows offsets = Linux + 0x38 for every bloom field (`W:+0x13C scale` = `L:+0x104`, `W:+0xFC` bypass = `L:+0xC4`).
The Windows functions `W:0x100352a0` (input), `W:0x100316d0` (output) and `W:0x100318d0` (move) match the Linux
ones statement for statement wherever checked. The Windows input writes the 8 blur passes as a loop.

Notation: `vw×vh` = the viewport size in pixels (ctx+0x10/+0x14), `texW×texH` = the allocated scene-RT size,
`E` = the adapted exposure (script field `scale`), `m` = the measured luminance of the current frame.

---

## 1. Object, fields, script use

### 1.1 Layout (Linux offsets, ctor `L:0x081e8dd0.c:15-52`, size 0x118 `L:0x081e8b60.c:46`) — obs H

| Off | Field | Default | Notes |
|---|---|---|---|
| +0x98 / +0x9C | window / video | | `create` `L:0x081de910.c:52-60` |
| +0xA0 | scene RT (`gsTextureRenderer_OGL*`) | null | made on first use by `initFrameBuffer` |
| +0xA4 | input (`gsIVideoOutput_OGL*`) | | previous effect, or the viewport |
| +0xA8 | output texture | null | stays null for bloom (§3.1) |
| +0xAC / +0xB0 | render offset (fraction) | 0 / 0 | |
| +0xB4 / +0xB8 | current viewport size `vw`, `vh` | | copied from ctx+0x10/+0x14 every frame |
| +0xBC / +0xC0 | allocated size `texW`, `texH` | | |
| +0xC4 | bypass | 0 | set on any FBO failure, never cleared |
| +0xC5 | "input is a scene" at allocation (MSAA + depth) | 0 | |
| +0xC6 / +0xC7 | float format / mipmaps | 1 / 0 | |
| +0xC8 | cube face | 0 | |
| +0xCC | `gsMoveable` (tick interface) | | |
| +0xD4..+0xE0 | A0..A3 targets: 512×256, 256×128, 128×64, 64×32 | | `create` `L:0x081de910.c:118-197` |
| +0xE4..+0xF0 | B0..B3 targets: same sizes | | `L:0x081de910.c:148-206` |
| +0xF4 / +0xF8 / +0xFC / +0x100 | `scale` / `blur` / `add` / `final` shader | | `L:0x081de910.c:62-107` |
| +0x104 | **`scale`** = adapted exposure `E` | **0.5** | script field |
| +0x108 | measured luminance `m` | 0 | internal |
| +0x10C | **`saturation`** | **1.0** | script field |
| +0x110 | **`lightness`** | **0.9** (`0x3f666666`) | script field |
| +0x114 | **`saturationWeight`** | **1.0** | script field |

The four script fields are plain floats with no setter (`L:0x081dcf50.c:18-29`). There is no `exposure`, `enabled`
or intensity field. `scale` is not a bloom intensity: it is the adaptation state of §2, and writing it from
script only sets the starting point of the next adaptation step.

### 1.2 Creation (`new_bloomEffect()` → `initBloom`, `L:0x081e8b60.c`) — obs H

1. `video.shaders` (`L:+0x1b8`) false → log *"Cannot initialize bloom (requires GLSL)"*, return null (`:27-28`).
2. `create(video)` (`L:0x081de910.c`): `gsModule::create(video)` tail-calls vtable +0xC4 = `gsModule::setOwner`
   (vtable dump of 0x082aaed4), so the **video becomes the owner** and `addChild` registers the effect as a moveable
   child (`L:0x08137b00.c:34`). Loads the 4 shaders, then creates the 8 targets with
   `init(w, h, mip 0, depth 0, float 1, samples 0)`.
3. Any target failing → *"Bloom initialization failed!"*, bypass = 1; `initBloom` releases the object and
   returns null (`L:0x081e8b60.c:87-92`). Scripts test `if ($bloom)`.

### 1.3 Script use — obs H

| Where | What |
|---|---|
| `metro/defaults/settings.script:20-21` | `$bloom = true`, `$saturation = 1.0` (shipped defaults) |
| `metro/scripts/window.script:27` | `window.postProcess = $settings.$bloom` (MSAA moves from the pixel format into the FBO) |
| `E:266-269` | `if ($settings.$bloom) { $bloom = video.new_bloomEffect(); MAIN.$viewport.addEffect($bloom) }`, added **after** the shadow effect (`E:255-264`) |
| `E:271-283` (`MAIN.$scenario` set) | `saturation = saturationWeight = 0` (greyscale); thread: `wait 3.0`, then each frame `t += dt·0.33` (≈ 3.03 s): `saturation = t·S`, `saturationWeight = t`; end: `S`, 1.0 |
| `E:285-294` (no scenario) | `saturation = 0` (weight stays 1.0); `t += dt·2` (0.5 s): `saturation = t·S`; end `S` |
| | `S = MAIN.$saturation · ($settings.$saturation : 1.0)`; `MAIN.$saturation` comes from the location script (0.5 St Petersburg, 0.6 Germany, 0.8 Finland/Tokyo/New York, 1.0 others) |
| `metro/ui/secondaryview/secondaryview.script:319-324` | secondary camera view: its own bloom, `saturation = 0.3·MAIN.$saturation` (no settings factor), chain shadows → bloom → `$cameraTexture` (512×256 texture target) |
| `secondaryview.script:768`, `cameratarget.script:87` | `$bloom.scale = 0.0` when the view switches to a new target: the view starts over-exposed and adapts (§2) |
| `metro/ui/menus/graphics.script:9, 92-98, 266-267` | the menu calls bloom "HDR lighting"; the saturation slider is only active with it on; changing it while a scene exists sets `$requireRestart` |

No script sets `lightness` (always 0.9).

---

## 2. Per-tick exposure adaptation (`move`, `L:0x081dd160.c:23-35`, `W:0x100318d0.c:17-32`) — obs H

Called once per frame by the video's moveable tick (`gsVideo::move` → `gsMoveable::move` over children whose
module flag +0x48 bit 0 is set, `L:0x0813a6b0.c:14`, `L:0x08133ff0.c:28`), during the update phase, so **before** this
frame's bloom render. `dt` is the frame time (clamped to [0.0001, 0.05] s, §1.5.1 of the 2D doc). It runs during
game pause too.

```
if (E == -1.0 || dt < 0.0001)  E = m                      // snap
else {
    E = E + (1 - 0.5^dt) * (m - E)                         // exponential approach, half-life 1 s
    if (E > 100000.0 || E <= 0.0)  E = 1.0
}
```

* `m` is the value measured by the **previous** frame's readback (§3.4), so the composite lags by one frame plus
  the 1 s half-life.
* Start: `E = 0.5`, `m = 0` (ctor). The first tick moves E slightly toward 0 (still > 0), then it follows the scene.
* `scale = -1` from script snaps on the next tick. The composite also uses `m` directly while `E == -1`
  (§3.6). No shipped script uses -1.
* `scale = 0` (secondary view): the next tick gives `E = (1 − 0.5^dt)·m`. For `m ≈ 0.5` at 60 fps that is about
  0.006, so the composite gain is about `0.9/0.12 ≈ 7.5`. The view flashes bright and settles over 1–3 s. The script runs before the
  video tick in a frame (inf from 2D doc §1.5.1), so the literal 0 is never drawn.
* NaN is not caught: `100000 < NaN` and `NaN <= 0` are both false, so a NaN `m` makes `E` NaN until a script
  writes `scale` (inf, L).

---

## 3. The frame (`input` `L:0x081dfce0.c`, `output` `L:0x081dd950.c`)

### 3.1 Where it sits

`gsViewport_OGL::renderContent` (`L:0x082032e0.c:18-39`) runs `effect.input(ctx, prev)` for every effect in
`addEffect` order, then sets the viewport rect on the current framebuffer and calls `last.output(ctx)`:

```
glScissor (ctx.scissor.x, winH - ctx.scissor.h - ctx.scissor.y, ctx.scissor.w, ctx.scissor.h)     :26-29
glViewport(ctx.vp.x,      winH - ctx.vp.h      - ctx.vp.y,      ctx.vp.w,      ctx.vp.h)          :30-33
```

(`winH` = ctx+4.) For the main viewport that is `(0, 0, winW, winH)` of the back buffer. In the main game the
chain is `[shadow, bloom]`: `shadow.input` makes the shadow map, `bloom.input(ctx, shadow)` does everything in
§3.3–3.5, and `bloom.output` is the composite (§3.6). Then the viewport's scene overlay pass follows. The GUI root
viewport is drawn afterwards and never goes through bloom.

### 3.2 Render targets — obs H

| Target | Size | Storage | Filter / wrap | Created by |
|---|---|---|---|---|
| scene RT | `texW×texH` = viewport size at the last (re)allocation (NPOT, grow-only, §3.3) | colour RB **RGBA16F**, multisampled with `samples`, + **DEPTH_COMPONENT24** RB (same samples), resolved into an RGBA16F texture | LINEAR / LINEAR, CLAMP_TO_EDGE, anisotropy 1.0 | `L:0x081ea0a0.c:69-117`, `L:0x081e97a0.c:36-46,62-64` |
| A0, B0 | 512×256 | RGBA16F texture attached directly (no RB, no depth) | same | `L:0x081ea0a0.c:119-122` |
| A1, B1 | 256×128 | same | same | |
| A2, B2 | 128×64 | same | same | |
| A3, B3 | 64×32 | same | same | |

Every target is cleared to (0,0,0,0) once at creation (`L:0x081ea0a0.c:124-128`). Afterwards **only the quad
sub-rectangle is ever written**, so texels outside it keep 0 (or stale data after an aspect change, §4).
`startRender(x, y, sx, sy)` binds the FBO and sets `glScissor` and `glViewport` to
`(int)(x·W), (int)(y·H), (int)(sx·W), (int)(sy·H)`. The scissor is in the pseudocode (`L:0x081e95e0.c:16-22`). The
viewport args are lost there but visible in the bytes: identical expressions, rounding mode = truncate, tail jump to
`glViewport` at 0x081e96ef (obs H). The passes call `startRender(0, 0, 1, 1)`, i.e. the full target.
`endRender` blits main → resolve FBO over the full texture size, `COLOR_BUFFER_BIT`, `NEAREST`, when a resolve FBO
exists (`L:0x081e9480.c:17-31`).

**The quad helper** `renderQuad(x1, y1, s, t, tex0..tex3)` (`L:0x081df1c0.c:34-111`) — obs H:

* projection `glOrtho(0, 1, 1, 0, −1, 1)` (y = 0 at the **top**), modelview identity (`:35-39`);
* state: disable CULL_FACE, DEPTH_TEST, LIGHTING, COLOR_MATERIAL, ALPHA_TEST, **BLEND**; enable DITHER,
  TEXTURE_2D; `glColor4ub(255,255,255,255)` (`:40-49`); textures on units 0..3 in argument order (`:50-60, 79-81`);
* `glInterleavedArrays(GL_T2F_V3F)`, `glDrawArrays(GL_QUADS, 0, 4)` with (`:62-76`):

| vertex | position (ortho) | uv |
|---|---|---|
| 0 | (0, 0) top-left | (0, t) |
| 1 | (x1, 0) | (s, t) |
| 2 | (x1, y1) | (s, 0) |
| 3 | (0, y1) bottom-left | (0, 0) |

So uv v = 0 is at the bottom of the quad and the image is upright (GL convention: texel row 0 = bottom). There are
no flips anywhere. In target pixels the quad covers columns `[0, x1·W]` and rows `[(1−y1)·H, H]`, i.e. it is
anchored at the **top** edge.

**Sub-rectangle of the 2:1 bloom targets** (computed identically in input and output, `L:0x081dfce0.c:53-62`,
`L:0x081dd950.c:63-72`, `W:0x100352a0.c:61-69`):

```
if (2·vh < vw) { bx = 1;            by = 2·vh / vw }     // wider than 2:1
else           { bx = vw / (2·vh);  by = 1 }
```

| Viewport | bx | by | A0 columns written | A3 columns written |
|---|---|---|---|---|
| 16:9 (1600×900, 1920×1080) | 0.8889 | 1 | 455 of 512 | 57 of 64 |
| 16:10 | 0.8 | 1 | 410 | 51 |
| 4:3 / 5:4 | 0.6667 / 0.625 | 1 | 341 / 320 | 43 / 40 |
| 21:9 (2560×1080) | 1 | 0.84375 | 512 | 64 (rows: top 27 of 32) |

### 3.3 Pass 0: scene into the RT (`gsPostEffect_OGL::initFrameBuffer` `L:0x081df950.c`, base input `L:0x081dd7f0.c:17-24`) — obs H

1. `ctx+0x2D = 1` for the duration (`L:0x081dfce0.c:44, 51`). The mesh renderer then skips the glow queue for
   `MATERIAL_GLOW` submeshes and draws them as ordinary opaque geometry (`L:0x082084d0.c:384`; interfaces-3d.md
   §1.3). Bloom is the only writer of this flag.
2. (Re)allocate when `input.isScene() != +0xC5`, or `vw > texW`, or `vh > texH`, or there is no RT yet
   (`:37-41`). The new size is exactly `(vw, vh)` (`:45-49`). The max with the output texture's size is a no-op
   because +0xA8 is null: `getTexture()` is read before `init` creates the texture (`:70-72`, `L:0x081e9790.c`). The
   RT therefore only grows, except that a change of `isScene` re-allocates at the current size.
3. `samples = isScene ? window.antialiasing (window+0x104) : 0`. Then
   `init(texW, texH, mip 0, depth = isScene, float 1, samples)`; on failure `samples −= 2` and retry; below 0 →
   *"Framebuffer initialization failed!"* and a permanent bypass (`:89-129`).
   `isScene` = `gsViewport_OGL::isScene` = 1 (`L:0x082006a0.c:14`); the shadow effect forwards it
   (`L:0x081dcd60`). A working post effect returns 0 (`L:0x081dd590.c:17`), so an effect after bloom gets
   neither MSAA nor depth.
4. `+0xB4/+0xB8 = (vw, vh)` (`:132-133`).
5. `startRender(0, 0, vw/texW, vh/texH)` → scissor = viewport = `(0, 0, vw, vh)` (bottom-left corner of the RT);
   `selectFace(0)`; `input.output(ctx)` (shadow binds its map on unit 7 and renders the viewport's scene; the
   viewport clears depth/stencil and colour when `clearColor` alpha ≠ 0, `L:0x082007b0.c:160-168`; it does not call
   `glViewport` itself); `endRender` (MSAA resolve).

The scene RT therefore holds unclamped HDR colour in `[0, vw/texW] × [0, vh/texH]` of its uv space. The extent is
(1, 1) unless the window has shrunk since allocation.

### 3.4 Passes 1–10 (`L:0x081dfce0.c:63-238`) — obs H

Each pass runs `target.startRender(0,0,1,1)`, `shader.use(ctx, material, true)`, `renderQuad(...)`, `unuse`,
`target.endRender()`. `use` uploads `custom0` from material+0x9C (`L:0x081eab50.c:122`) and sets the sampler
uniforms `textureN = N` (`:44-72`). The material is a stack `gsMaterial_OGL` whose custom0 is overwritten before
every pass.

| # | Shader | Target | Source(s) → unit | Quad `(x1, y1)` | uv `(s, t)` | `custom0` | Lines |
|---|---|---|---|---|---|---|---|
| 1 | scale | A0 512×256 | scene RT → 0 | (bx, by) | (vw/texW, vh/texH) | (0.25/512, 0.25/256, 0, 0) = (1/2048, 1/1024, 0, 0) | 65-83 |
| 2 | blur | B0 512×256 | A0 → 0 | (bx, by) | (bx, by) | (1/512, 0, bx, by) | 84-99 |
| 3 | blur | A0 | B0 | (bx, by) | (bx, by) | (0, 1/256, bx, by) | 100-115 |
| 4 | blur | B1 256×128 | A0 | (bx, by) | (bx, by) | (1/256, 0, bx, by) | 116-131 |
| 5 | blur | A1 256×128 | B1 | (bx, by) | (bx, by) | (0, 1/128, bx, by) | 132-147 |
| 6 | blur | B2 128×64 | A1 | (bx, by) | (bx, by) | (1/128, 0, bx, by) | 148-163 |
| 7 | blur | A2 128×64 | B2 | (bx, by) | (bx, by) | (0, 1/64, bx, by) | 164-179 |
| 8 | blur | B3 64×32 | A2 | (bx, by) | (bx, by) | (1/64, 0, bx, by) | 180-195 |
| 9 | blur | A3 64×32 | B3 | (bx, by) | (bx, by) | (0, 1/32, bx, by) | 196-211 |
| R | readback of A3 | — | — | — | — | — | 212-229 |
| 10 | add | B0 512×256 | A0→0, A1→1, A2→2, A3→3 | (bx, by) | (bx, by) | none (material = null; `add.shader` has no parameters) | 230-238 |

The blur offset unit `o = custom0.xy` is one texel of the **target**. In passes 4, 6 and 8 the source is twice as
large, so the taps are 2 source texels per unit. The single bilinear tap per output texel does the 2:1 downsample.

Shader maths (`S:scale.fragment`, `S:blur.fragment`, `S:add.fragment`), with uv = interpolated `texCoord`:

```
scale:  c = Σ_{(±1,±1)} tex0(uv + (±custom0.x, ±custom0.y));   out = c·c·0.0625      // = (mean of 4 taps)², all 4 channels
blur:   out = Σ_k w_k · tex0(uv − o·d_k) + Σ_k w_k · tex0(min(uv + o·d_k, custom0.zw))
        d = (0.631579, 2.307692, 4.153846), w = (0.306302, 0.174646, 0.019052)   // Σ of all 6 weights = 1.0
add:    out = 0.40·tex0 + 0.30·tex1 + 0.20·tex2 + 0.10·tex3                     // same uv for all 4 levels
```

* The scale taps sit ±(texW/2048, texH/1024) scene pixels from the sample point, about ±0.8–1.0 px at 1600–1920
  wide. With bilinear filtering this approximates a 4×4 box. **The output is the squared colour**, so the pyramid
  holds ≈ `blur(scene²)`.
* Only the + side of the blur is clamped to the written sub-rectangle `(bx, by)`. The − side relies on
  CLAMP_TO_EDGE at uv 0. The add pass upsamples A1..A3 bilinearly.

**Readback / measurement** (`:212-229`, `W:0x100352a0.c:124-144`):

```
A3.use();  glGetTexImage(GL_TEXTURE_2D, 0, GL_LUMINANCE, GL_FLOAT, buf[2048]);  A3.release();
sum = Σ_{i<2048} buf[i]                 // all 64×32 texels, including the unwritten strip
m   = sqrt(sum / 2048)                  // 0.00048828125
if (m > 100000 || m <= 0)  m = 1.0      // a black frame gives m = 1, not 0
```

For an RGBA texture, `GL_LUMINANCE` in `glGetTexImage` returns **L = R**. This is the spec's GetTexImage rule
and Mesa's behaviour; `glReadPixels` instead gives `R+G+B`. GL_FLOAT means no clamping. So `m` is the RMS of the
**red** channel over the whole 2:1 target (inf M: driver behaviour, see open questions). The GPU waits for the
readback here.

### 3.5 Uniform-scene sanity check (inf from the maths above, H)

For a flat grey scene of value `S` covering a fraction `a = bx·by` of the target (16:9: a = 0.889):
`A_i = S²` inside, `m = S·√a`, bloom `B = S²`, and once adapted `E = m`. The composite (§3.6) then gives
`c = 0.9·(S + S²) / (3·S·√a + 0.1)`: S = 0.5 → 0.446, S = 1 → 0.615, S = 2 → 0.94 at 16:9. The result is a strong
contrast compression toward mid-grey, which a port can check numerically.

### 3.6 Pass 11: composite (`gsBloomEffect_OGL::output`, `L:0x081dd950.c`, `W:0x100316d0.c:33-66`) — obs H

Bypassed → `input.output(ctx)` (`:56-62`). Otherwise, into the framebuffer and viewport left by `renderContent`
(§3.1):

```
E' = (E == -1) ? m : E                                                      :74-77
custom0 = ( lightness / (E'·3 + 0.1),  saturation,  (1 − saturation)·saturationWeight,  0 )   :79-82
final.use(ctx, material, true)                                              :87
same ortho + state as renderQuad (no blend, no depth test)                  :94-108
unit 0 = scene RT texture, unit 1 = B0 (the add result)                     :109-114
glInterleavedArrays(GL_T4F_V4F), GL_QUADS, full viewport                    :115-137
```

| vertex | position | `texCoord.xy` (scene RT) | `texCoord.zw` (bloom B0) |
|---|---|---|---|
| 0 | (0, 0) top-left | (0, vh/texH) | (0, by) |
| 1 | (1, 0) top-right | (vw/texW, vh/texH) | (bx, by) |
| 2 | (1, 1) bottom-right | (vw/texW, 0) | (bx, 0) |
| 3 | (0, 1) bottom-left | (0, 0) | (0, 0) |

This is the 6-float `renderQuad(1, 1, s, t, bx, by, …)` (`L:0x081df640.c:73-95`; Windows calls it as
`W:0x1002f980`), inlined.
`final.fragment` (`S:final.fragment:7-15`), applied to all 4 channels:

```
c  = (tex0(xy) + tex1(zw)) · custom0.x
lo = min(c.r, c.g, c.b);  hi = max(c.r, c.g, c.b)
c  = mix(vec4((hi + lo)·0.5), c, custom0.y + custom0.z·(hi − lo))      // f > 1 oversaturates
k  = max(c.r, c.g, c.b, 1.0)
out = (c + k − 1) / k                                                  // identity when all ≤ 1; else the max channel → 1
```

The result overwrites the viewport rect, alpha included, in the 8-bit UNORM back buffer. Nothing is blended.
`final.shader` declares TEXTURE0..3; units 2/3 are unused. The saturation factor works out to:
`f = saturation + (1 − saturation)·saturationWeight·(hi − lo)`. At 1.0/1.0 this is 1 (no change). At 0/0 it is 0
(greyscale `(hi+lo)/2`). At 0.6/1.0 it is `0.6 + 0.4·(hi−lo)`: grey-ish pixels lose colour, strongly coloured ones
keep it (vibrance-like).

---

## 4. Bloom on vs. off, and the shadow effect

| | Bloom off (`$bloom = false`, or `new_bloomEffect` returned null) | Bloom on |
|---|---|---|
| `window.postProcess` | false: MSAA from the pixel format | true: no MSAA on the default framebuffer; MSAA only in the scene RT |
| Scene target | back buffer, RGBA8 (clamped), 16-bit depth (2D doc §4.3) | RGBA16F MSAA RB + DEPTH24 RB → RGBA16F texture |
| Colour pipeline | shader output written directly; no exposure, no saturation, no soft clip; **`MAIN.$saturation` and `$settings.$saturation` have no effect** | §3.6 |
| Glow materials | glow queue + occlusion queries + glow sprites | `ctx+0x2D = 1`: drawn as opaque HDR geometry. The glow queue stays empty, so no sprites (inf M, open q. 4) |
| Shadows | `shadow.output` renders the scene into the back buffer | the same `shadow.output`, rendered into the bloom RT (§3.3 step 5). Bloom only adds `ctx+0x2D` and the MSAA/depth choice through `shadow.isScene()` |
| FBO failure at runtime | — | permanent bypass: `output` forwards to the input. This looks like bloom off, but without any MSAA because `postProcess` is still true |

**Quirks** (obs H unless noted; a port should reproduce them, or fix them on purpose):

1. **Wider than 2:1 is misregistered.** Quads are anchored at the top of the target (rows `[(1−by)H, H]`), but
   passes 2–11 read uv `t ∈ [0, by]`, the bottom band. With `by < 1` every pass shifts the bloom by `(1−by)·H`
   and pulls in cleared rows. For ≤ 2:1 viewports `by = 1` and all is consistent. Fix if wanted: anchor the
   bloom-target quads at ortho y ∈ `[1−by, 1]`.
2. **The luminance mean includes the unwritten strip** (zeros): at 16:9, 57 of 64 columns are valid, so `m` is
   ≈ 0.943 × the true RMS. To reproduce, average over the full 64×32.
3. **The + side blur clamp lands on the border texel.** At `uv = bx` bilinear filtering mixes the last written
   column with the first unwritten one, which slightly darkens the bloom's right edge. Identical coordinates
   reproduce this automatically.
4. **Stale strip.** Targets are cleared only at creation. After the aspect ratio narrows, the old content stays in
   the now-unused strip and feeds quirks 2 and 3 (L impact).
5. One frame of exposure latency, the 1 s half-life, and adaptation that keeps running while paused (§2).
6. The CPU stall from the readback (2D doc §4.3). An asynchronous readback one or two frames late is invisible at
   a 1 s half-life.

**Corrections to [interfaces-2d-output.md](interfaces-2d-output.md)** (from the Linux symbols and code):
§2.8 slot 0 is `isScene()`, not `wantsMultisample`. It selects both MSAA and the depth RB.
§2.9 `begin` is `startRender`; its scissor and viewport are now observed (§3.2). §1.5.3's y-flip is observed in
`renderContent` (§3.1). §2.12: `scale == −1` is not an "auto exposure" mode; adaptation always runs and −1 only
snaps it.

---

## 5. Port checklist

1. **Object**: `new_bloomEffect()` → null without shaders or when a target fails. Script fields `scale` (0.5),
   `saturation` (1.0), `lightness` (0.9), `saturationWeight` (1.0), no setters. Owner = video, ticked every frame.
2. **Tick**: §2 exactly (`pow(0.5, dt)`, snap on −1 or `dt < 1e-4`, clamp to 1.0 outside (0, 1e5]), before
   rendering, using last frame's `m`. Guarding NaN is a free improvement.
3. **Targets**: scene RT RGBA16F at viewport size, grow-only, MSAA = `window.antialiasing` (−2 fallback),
   DEPTH24, resolved before sampling; A0..A3/B0..B3 RGBA16F 512×256 → 64×32. All LINEAR + CLAMP_TO_EDGE, cleared to 0
   once, never again.
4. **Scene pass**: viewport/scissor `(0,0,vw,vh)` in the RT; set the "in post effect" flag so glow submeshes
   go to the opaque queue; render the effect input (shadow → viewport scene).
5. **Passes 1–10** per the table in §3.4: same quad geometry (top-anchored `[0,bx]×[0,by]`, uv `(0..s, 0..t)` with
   v = 0 at the bottom), same `custom0`, the shipped `bloom/*.fragment` through the GLSL 1.10 → 3.30 translator.
   GL 3.3 core has no GL_QUADS: use two triangles with the same corner uv.
6. **Measurement**: `m = sqrt(Σ R / 2048)` over the whole 64×32 A3 (core profile: `glGetTexImage(GL_RED,
   GL_FLOAT)` gives exactly the R channel, or use a GPU reduction with the same divisor); clamp per §3.4.
7. **Composite** into the back-buffer viewport rect (`y = winH − h − y_top`): unit 0 scene RT, unit 1 B0, uv per
   §3.6, `custom0 = (lightness/(3E'+0.1), saturation, (1−saturation)·saturationWeight, 0)`, no blend, no depth.
   The scene overlay pass and the GUI come after.
8. **Window**: with `$bloom`, request no MSAA on the swapchain; without it, skip the whole effect (the scene goes
   straight to the back buffer and the saturation settings do nothing).
9. **Check**: the user's settings (and the copy in `port/run/documents`) have `$bloom = false`, so the original
   shows no bloom with them. Compare against the original with "HDR lighting" on, at 16:9: game start is grey for
   3 s and gains colour over 3 s (scenario); §3.5 gives target values for flat areas.
10. **Decide quirks 1–4** (§4). Reproduce 2–4 by using identical coordinates; fix 1 only if wide windows matter.

---

## 6. Open questions

1. **Readback channel on the 2011 drivers.** Spec and Mesa: `glGetTexImage(GL_LUMINANCE)` on RGBA returns R,
   unclamped for FLOAT. A driver returning `R+G+B` (the ReadPixels rule) would make `m` up to √3× larger and the
   image darker. Check with an apitrace capture of the original or by matching a screenshot (§3.5 numbers).
2. Wider-than-2:1 viewports (quirk 1) are only derived from the code; nobody has looked at it running.
3. The order of script threads vs. the video tick in a frame (it decides whether `scale = 0` is ever drawn as 0) is
   taken from 2D doc §1.5.1, not re-checked on Linux.
4. Glow sprites with bloom on: confirm that the visible-glow list is empty when nothing enters the glow queue
   (`gsScene_OGL` sprite pass, interfaces-3d.md §1.3 step 3).
5. The scene RT's alpha (shader output, cleared to 0) passes through the composite into the back buffer.
   Assumed unused; nothing reads destination alpha.
