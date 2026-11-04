# GSystem 3D renderer: interfaces, OpenGL implementation, port contract

Scope: the 3D path of `GSystem.dll` (Cities in Motion 1.0.22) — base interfaces `gsIVideoItem` / `gsIVideoElement` /
`gsI3DVideoItem`, `gsIScene`, `gsIMesh`, `gsISubMesh`, `gsISkeleton`, `gsICamera`, `gsILight`, `gsIPrimitive`,
`gsIParticleSystem`, `gsIMaterial`, `gsIShader`, their `*_OGL` implementations, the internal bases `gsVideoItem_OGL` /
`gs3DVideoItem_OGL`, the shader system (`gsShaders_OGL`, `gsShaderCombiner_OGL`, `gsShaderProgram_OGL`) and skinning.
GUI, viewport, textures and post effects are covered by other documents; they appear here only where the 3D path
touches them.

Sources: Ghidra 12.1.4 pseudocode in `decompiled/GSystem-v2/functions/0x<addr>.c` (cited as `0x<addr>.c:<line>`),
`docs/rtti-classes.json`, the slot skeleton `docs/renderer/renderer-map.json`, the asset docs in `docs/asset-format/`,
`docs/native-api.md`, `docs/handlers/batch-B.md`, and the shader sources in `extracted/shaders/` and
`extracted/metro/shaders/`.

Legend:

* **obs** = read directly in the pseudocode. **inf** = inferred from layout, callers or naming.
* Confidence: **H** high, **M** medium, **L** low.
* Offsets: every 3D `*_OGL` object has two vtable pointers. **outer** = the interface pointer (`gsIMesh*` etc., vptr
  at +0). **sub** = the internal `gs3DVideoItem_OGL` subobject at outer+4. Interface methods use outer offsets; the
  internal render/traverse methods use sub offsets (sub+X = outer+X+4). Offsets in this document are outer unless
  marked "sub".
* Matrices are 4x4 `float[16]` in row-vector convention (`v' = v·M`, translation in m[12..14]). This is the same
  memory layout OpenGL expects, so they are passed to `glLoadMatrixf` / `glUniformMatrix4fv(transpose=GL_FALSE)`
  unchanged.
* `renderer-map.json` misses two extension pointers used here: `DAT_1019486c` = `glUniformMatrix3fvARB` and
  `DAT_10194870` = `glUniformMatrix4fvARB` (`0x10055db0.c:63-68`).

---

## 1. Overview

### 1.1 Object model

```
gsVideo_OGL (one per window)            factory methods, render queues, shader cache, texture cache
 ├─ gsShaders_OGL           video+0x1a8  path -> gsShaderCombiner_OGL -> (mat,mesh,env) -> gsShaderProgram_OGL
 ├─ gsTextures_OGL          video+0x1b0  (texture doc)
 └─ render queues           video+0x220 .. +0x248 (rebuilt every scene render)

gsScene_OGL  (root gs3DVideoItem; owns an octree + light list + fog)
 └─ children: gsMesh_OGL / gsSkeleton_OGL / gsCamera_OGL / gsPrimitive_OGL / gsParticleSystem_OGL (tree via setParent)
      gsMesh_OGL ── 4 LOD vectors of gsSubMesh_OGL / gsHWSkinSubMesh_OGL (each: VBO, IBO, material, program variant)
                 └─ optional gsSkeleton_OGL (bone palette source for hardware skinning)
gsMaterial_OGL (not a scene node): 7 texture slots, colours, flags, base shader program, 8 custom vec4
```

* **Interface vs implementation.** Each `gsIX` is the script/engine-facing interface (pure virtual, all slots
  `_purecall` in RTTI). Every 3D `*_OGL` class also derives from the internal `gs3DVideoItem_OGL` (at outer+4), whose
  5-slot vtable is the renderer's own traversal protocol (§2.13). `gsIMaterial`, `gsIShader` and `gsISubMesh` derive
  only from `gsIVideoItem`. (RTTI `baseOffsets`; `docs/rtti-classes.json`.)
* **Factories** (gsVideo_OGL vtable at `0x1015aed4`; slot offsets are byte offsets into it):

| video slot | addr | method (named here) | creates | evidence |
|---|---|---|---|---|
| 0xf0 | 0x1004e1e0 | `createScene(uint flags, gsI3DVideoItem* parent)` | gsScene_OGL (0xa4 B) | 0x1004e1e0.c:21-27 |
| 0xf4 | 0x1004e250 | `createMesh(MeshData* data, uint flags, gsI3DVideoItem* parent, int lod)` | gsMesh_OGL (0x128 B); calls build() if data | 0x1004e250.c:22-28, 0x1004d820.c |
| 0xf8 | 0x1004e2c0 | `createSkeleton(uint flags, gsI3DVideoItem* parent)` | gsSkeleton_OGL (0x120 B) | 0x1004e2c0.c |
| 0xfc | 0x1004e330 | `createPrimitive(uint flags, gsI3DVideoItem* parent)` | gsPrimitive_OGL (0x80 B) | 0x1004e330.c |
| 0x100 | 0x1004e3a0 | `createLight(uint flags, gsI3DVideoItem* parent)` | gsLight_OGL (0xa4 B) | 0x1004e3a0.c |
| 0x104 | 0x1004e410 | `createCamera(uint flags, gsI3DVideoItem* parent)` | gsCamera_OGL (0xdc B) | 0x1004e410.c |
| 0x108 | 0x1004e480 | `createParticleSystem(uint flags, gsI3DVideoItem* parent)` | gsParticleSystem_OGL (0x80 B) | 0x1004e480.c |
| 0x10c | 0x1004e4f0 | `getTexture(path)` | cached gsTexture_OGL | 0x1004e4f0.c |
| 0x110 | 0x1004fcd0 | `getShader(path, matFlags, meshFlags)` → `gsIShader*` (empty path → `shaders/default/default.shader`) | cached program | 0x1004fcd0.c:29-41 |
| 0x114 | 0x1004ef80 | `createMaterial()` | gsMaterial_OGL (0x120 B) | 0x1004ef80.c:20-26 |
| 0x118 | 0x1004fab0 | `getDistanceFactor()` (video+0x218) | | 0x1004fab0.c |
| 0x11c | 0x1004e510 | `getEffectTimer()` (shaders+0xfc, the `timer` uniform) | | 0x1004e510.c |

  Callers: `gsObject` creates meshes with flags `0x800` (or `0x2800` when the element's `forceVisible` is set)
  (`0x1008db60.c:26-36`); particle emitters use `createParticleSystem(0xc0, …)` (`0x1005dc80.c:21`); debug lines use
  `createPrimitive(0x12, …)` (`0x10067b40.c:17`).
* **Ownership.** Intrusive refcounts: 3D items at sub+4 (outer+8), materials at +4, textures at +0x14,
  programs are owned by the shader cache (`release` is a no-op, `0x10001000.c`). An item starts with refcount 1
  (`0x10047340.c:11`). `addChild` increments the *parent's* refcount (`0x10047630.c:16`) and `removeChild` decrements
  it (`0x100466d0.c:27-30`), so a parent lives as long as it has children. Submeshes keep a ref on their source
  submesh when cloned and share its VBO/IBO (`0x100494e0.c:62-83`). A material holds refs on its textures and on
  nothing else; a submesh holds a ref on its material.
* **Creation flags** (sub+0xc / outer+0x10, the `flags` factory argument; obs in the cited readers):

| bit | meaning | evidence |
|---|---|---|
| 0x04 / 0x08 / 0x10 / 0x20 | primitive topology: quads / quad strip / lines / line strip (default triangles) | 0x10050870.c:61-85 |
| 0x40 | two-sided (primitive, particles: cull face off) | 0x10050dd0.c:41, 0x10051150.c:28 |
| 0x80 | depth-sorted: primitive/particles go into the sorted blend queue | 0x10050dd0.c:24-30, 0x10051560.c:47-60 |
| 0x800 | apply max draw distance (`(flags & 0x2800) == 0x800`) | 0x1004a4d0.c:156-158 |
| 0x1000 | camera-attached (sky): position := camera position; skipped in shadow-map pass | 0x1004a4d0.c:80-87 |
| 0x2000 | "force visible": not put in the octree, no frustum test, no distance cull | 0x100537e0.c:35, 0x1004a4d0.c:127 |
| 0x4000 | use coarsest LOD in the shadow-map pass (LOD factor 1e-5 instead of 0.333) | 0x1004a4d0.c:189-192 |

### 1.2 How a frame's 3D part is drawn

1. **Viewport** (`gsViewport_OGL` slot 20, `0x10044ef0`, other document): builds the projection
   (`FUN_10086570`, gluPerspective-equivalent, fov in **degrees**, GL depth range [-1,1]) into ctx+0xd0 and the view
   matrix (`FUN_10086350`, gluLookAt-equivalent from camera position/target/up) into ctx+0x90; ctx+0x110 = view·proj·
   screen mapping (used to place glow sprites); camera position → ctx+0x30, camera axes → ctx+0x48/+0x54/+0x60. GL
   state before the scene: `glDepthFunc(GL_LEQUAL)`, light-model ambient (0.1,0.1,0.1,1), cull face on with
   `GL_BACK`/`GL_FRONT` chosen by mirror parity XOR ctx+0x234 bit 0x20, `glColorMaterial(FRONT_AND_BACK,
   AMBIENT_AND_DIFFUSE)` but disabled (`0x10044ef0.c:157-180, 365-380`). It then calls the scene's internal
   `render(ctx)` (`0x10044ef0.c:510`).
2. **Scene render** `gsScene_OGL` sub slot 4 = `0x10052ec0`:
   * unbind program and material; enable depth test, disable blend and alpha test (`0x10052ec0.c:54-58`);
   * unless the pass is a shadow pass (ctx+0x234 & 0xc): enable lighting and upload every scene light as
     `GL_LIGHT0+i`: ambient/diffuse/specular (clamped to [0,1] when GLSL is off), position = light position
     transformed by the view matrix with w=1 (point light), attenuation (1,0,0) (`0x10052ec0.c:59-103`);
   * if scene fog is on: `GL_LINEAR` fog, start = scene fog start, end = scene fog end or (if ctx+0x230) the
     viewport's far distance ctx+0x74, colour = scene fog colour (`0x10052ec0.c:104-117`);
   * clear the four render queues on gsVideo_OGL; `glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`,
     `glAlphaFunc(GREATER, 0.2)` (`0x10052ec0.c:118-124`);
   * **collect**: query the octree with the view AABB (ctx+0x78) (`FUN_10052550`) and call `render(ctx)` on every hit
     that is visible; then traverse the scene's direct children (lights/particles/force-visible items)
     (`0x10052ec0.c:129-148`). For a mesh, `render` does not draw: it culls, picks LODs and pushes submeshes into
     queues (§1.3);
   * **draw passes** (`0x10052ec0.c:149-292`):

| order | queue (on gsVideo_OGL) | contents | sort | state |
|---|---|---|---|---|
| 1 | +0x220 (ptr array, count +0x224) | opaque submeshes | `FUN_100477a0`: non-decal first; decals by submesh index; then by material base program, then by the 8 words at material+0x80 (7 texture pointers + first custom-param word), then by address | blend off, depth write on |
| 2 | +0x22c (count +0x230) | glow submeshes (`MATERIAL_GLOW`, wrapped in occlusion queries) | `FUN_10047810` by submesh+0x80 (never written → effectively address order, inf), drawn in reverse order | as opaque |
| 3 | +0x238 (12-B entries, count +0x23c) | alpha-tested submeshes (and blended ones in the shadow-map pass) | `FUN_1004fea0` on key, drawn **descending** (far→near) | blend **and** alpha test on, `GREATER 0.2` → alpha-tested edges are also alpha-blended (normal pass) |
| 4 | +0x244 (12-B entries, count +0x248) | alpha-blended submeshes, decals, faded LODs, sorted primitives, individual sorted particles | same, far→near | `GREATER 0.001`, ctx+0x2c=1, depth write off for blended/decal; consecutive particles of one system with equal texture/flags/size batched in one `glBegin(GL_QUADS)` |

Queue entry {float key, gs3DVideoItem_OGL* item(sub), Particle* p}. Key = `d·|d|` (signed squared) with
d = distance(camera, world submesh centre) − submesh radius; decals use `0.9999·d·|d|` so they are drawn just after
(on top of) coplanar geometry (`0x1004a4d0.c:336-419`). After the passes:

   * end: depth mask off, disable alpha test/blend/fog/lighting/depth test (`0x10052ec0.c:284-292`); then for each glow
     submesh read its occlusion query (`GL_QUERY_RESULT`, a CPU stall) and keep only visible ones
     (`0x10052ec0.c:293-308`, `0x10048f50.c`).
3. **Glow sprites**: the viewport's overlay pass (`0x100447e0.c`) sets an ortho projection and calls
   `gsScene_OGL` slot 4 (`0x100505f0`), which draws one additive (`SRC_ALPHA, ONE`) star-shaped triangle fan per
   visible glow submesh with `shaders/glow/glow.shader` (`0x100505f0.c:25-42`, `0x10049280.c`, `0x1004e6c0.c`).
4. **Draw of one submesh** (sub slot 4 of the submesh, `0x100484c0`): ctx+0x1e0 := mesh world;
   `glLoadMatrixf(world·view)`; bind program (`FUN_10036940` → `gsIShader::apply`); bind material state
   (`FUN_10047bc0`, delta against the previously bound material); optional `glBeginQuery(GL_SAMPLES_PASSED)`;
   `drawRange` (`glDrawElements` on VBO/IBO with fixed-function client arrays); end query (`0x100484c0.c:19-52`).

### 1.3 Culling, LOD and transparency decisions (mesh collect, `0x1004a4d0`)

* Skip if mesh alpha (+0x120) is 0, or `(group mask +0x8c & ctx+0x224 visibleGroups) == 0` (`:73-78`).
* World matrix = `S(scale)·R(quat)·T(pos)`; if either "relative" byte (+0x11c position, +0x11d orientation) is set,
  `world = local · ctx+0x1e0` (parent world) (`:88-125`). Mesh flag 0x400 multiplies the scale by ctx+0x74 (far
  distance; sky domes).
* Frustum: bounding sphere (centre +0x3c transformed, radius +0x48) against the 6 planes at ctx+0x150 (point, normal
  pairs); in the shadow-map pass the radius is doubled (`:126-153`). Skipped for flags 0x3000.
* Distance = |camera − centre| × video `distanceFactor` (+0x218); culled beyond 1.1 × max draw distance (+0xd8) when
  flags 0x800 (`:154-159`).
* LOD (up to 4, count +0x124, vectors at +0x94+16·i; D = max draw distance +0xd8): LOD `n−1` (coarsest) covers
  [0.333·D, 1.1·D), each lower index the next band down by a factor 0.333 (factor 1e-5 in the shadow-map pass for
  flag 0x4000), LOD 0 everything nearer. Objects fade out between D and 1.1·D with alpha `((D−d)/D)·10+1` (clamped to
  ≤ the inherited alpha). With video `blendLod` (+0x1ff) the coarser LOD fades in over the 10 % below each band
  boundary B (`((d−B)/B)·10+1`) while the finer LOD keeps drawing up to 1.1·B, i.e. a cross-fade; partially faded
  submeshes take the blended path (`:193-297`). Submeshes flagged `MATERIAL_LAYER` (submesh +0xbc) are drawn at
  alpha 1 regardless of LOD visibility (`:306-310`).
* Per submesh: program variant := material base program `->getVariant(prev, matFlags | (alpha≠1 ? ALPHABLEND : 0),
  meshFlags, envFlags)`; skipped unless the variant is a linked GLSL program (`isValid`, slot 11) (`:313-322`). Queue
  choice (`:324-420`):
  * `MATERIAL_NO_SHADOW` and a shadow pass → not drawn;
  * alpha == 1, not ALPHABLEND: GLOW (+ material brightness ≠ 0, ctx+0x2d == 0, normal pass) → glow queue; DECAL → blend queue
    (normal pass only); ALPHATEST and not shadow-map pass → alpha-test queue; else opaque;
  * alpha == 1, ALPHABLEND → blend queue (alpha-test queue in the shadow-map pass);
  * alpha < 1: material not blended and mesh alpha == 1 (pure LOD fade) → opaque queue; else blend queue.
* envFlags = ctx+0x234 (pass flags) | (number of scene lights << 12) outside shadow passes (`0x10051120.c`).

### 1.4 Shadows (as visible from the 3D path)

The pass type lives in ctx+0x234 (`ENVIROMENT_*` constants): `SHADOWMAP_RENDER` 4 = depth from the light (shaders
write `gl_FragDepth`, alpha-cutout via `step(color.a,0.2)`); `SHADOW_RENDER` 8 (set together with `SHADOWMAP` 2) =
screen-space shadow-factor render; `SHADOW` 0x10 = main pass sampling the blurred screen-space shadow texture at
`gl_FragCoord.xy * (1/1600, 1/900)`; `FOG` 1; 0x20 = flip cull face. `gsShadowEffect_OGL` sets these
(`0x10031dd0.c:557, 723, 820`). In shadow passes: no GL lighting, material bind only binds texture unit 0 for
alpha-tested/blended materials (`0x100478f0.c:27-48`), decals and `MATERIAL_NO_SHADOW` submeshes are skipped, glow
submeshes are drawn as plain opaque geometry (no query, no flare), blended submeshes go to the alpha-test queue in the
shadow-map pass, and variant masks are reduced (§3.5) (`0x1004a4d0.c:324-420`).

### 1.5 Skinning

* A mesh whose data has skin weights (MeshData[7] ≠ 0) is built with `gsHWSkinSubMesh_OGL` for every submesh;
  otherwise `gsSubMesh_OGL` (`0x1004d0e0.c:75, 176-196`). Weights are first renormalised over the first
  `maxVertexWeights` (video+0x200, default 4) influences and the rest zeroed (`0x1004d0e0.c:76-133`).
* **There is no CPU skinning path in the renderer** (obs: `hardwareSkinning`, video+0x1fb, default 0, is only written
  by the registrar/constructor and checked at window init, where it is forced off without GLSL —
  `0x1004f8f0.c:47`, `0x10059200.c:536-549`; no reader in the 3D path). Skinning is always done in the vertex shader;
  without a skinning-capable program a skinned mesh renders in bind pose (inf).
* At upload, `gsHWSkinSubMesh_OGL::buildIndices` (`0x100552d0`) partitions the submesh's triangles into **bone groups**
  of at most `BONEx<n>` bones (the program's parameters low byte: 8 or 48 in `default.shader`), greedily, after dropping
  the weakest influences of triangles that need more than `n − optimizeSkinGroups` (video+0x204) bones. Each group =
  {first index, index count, palette int[n]} (vector at +0xcc, 12-byte entries). Per vertex it writes either 8 weights
  in palette order (non-indexed, `WEIGHTx8`, units 3+4) or 4 weights + 4 palette indices (`WEIGHTx4|INDEXx4`, units
  3 and 4) as `short` (weight × 32767, shader rescales by 1/32767). Mismatch logs "Error in skinning shader: …"
  (`0x100552d0.c:67-111`).
* Draw (`0x10054140`): if the mesh has no skeleton → plain submesh draw. Else world = identity when skeleton and mesh
  share the same parent (bones already carry the placement, inf) or the mesh world otherwise; per group:
  `program->setBones(skeleton, palette, n)` → `glUniformMatrix4fvARB(bones, n, GL_FALSE, …)` with matrices
  `inverseBind · boneWorld` (`0x10036d70.c`, `0x100501f0.c`), then `drawRange(first, count)`.
* Bone maths (`gsSkeleton_OGL`): bone world = `S·R·T·parentWorld`, root = `S(rootScale)·R·T` from slot 6;
  `captureBindPose()` stores `inverse(world)` per bone (`0x10050110.c`); skinning matrix = `invBind · world`
  recomputed lazily when the dirty byte +0x70 is set (`0x100501f0.c:25-73`). Quaternion → matrix
  (`FUN_100859d0`) puts the column-vector formula in row-major storage and uses it with row vectors, i.e. it applies
  the **conjugate** rotation — same convention as documented in `docs/asset-format/README.md`.

### 1.6 GL state model the OGL backend assumes

* **Hybrid fixed-function + GLSL 1.10.** Transforms (`glMatrixMode`/`glLoadMatrixf`), lights (`glLightfv`),
  materials (`glMaterialfv`), fog (`glFog*`) and vertex colour (`glColor`) are set through fixed-function state, and
  the GLSL shaders read them back through built-ins (`gl_ModelViewMatrix`, `gl_LightSource[]`,
  `gl_FrontLightProduct[]`, `gl_FrontMaterial`, `gl_Fog`, `gl_MultiTexCoordN`…). A port must turn that state into
  explicit uniforms (§3.3).
* **Vertex data:** VBO + IBO per submesh (`GL_ARB_vertex_buffer_object`, `GL_STATIC_DRAW`, or `GL_DYNAMIC_DRAW` when
  mesh flag 0x200), bound through **fixed-function client arrays** (`glVertexPointer`, `glNormalPointer`,
  `glColorPointer`, `glTexCoordPointer` on client units 0-4), not generic attributes (`0x10048160.c`). 16-bit indices
  when the submesh has < 65 536 vertices, else 32-bit (`0x10048160.c:86-94`).
* **Immediate mode** (`glBegin/glEnd`) for `gsPrimitive_OGL`, particles (CPU billboards, `GL_QUADS`) and glow sprites.
* **Textures**: `glActiveTexture` units 0-6 = material slots 0-6, unit 7 = shadow map; per-unit
  `glEnable(GL_TEXTURE_2D | GL_TEXTURE_CUBE_MAP)` (`0x10040dd0.c`).
* **Required in practice:** `GL_ARB_multitexture`, `GL_ARB_texture_compression`, `GL_EXT_texture_compression_s3tc`,
  `GL_SGIS_generate_mipmap`, `glActiveTexture`/`glClientActiveTexture`/`glMultiTexCoord2f`/`glCompressedTexImage2DARB`
  — a miss only logs "Minimun OpenGL requirements not supported" and continues (`0x10059200.c:314-378`); VBOs (submesh ctor calls `glGenBuffersARB` unconditionally,
  `0x100494e0.c:57-59`); **GLSL** (`GL_ARB_shader_objects` / `vertex_shader` / `fragment_shader` /
  `shading_language_100` + `glUniformMatrix3fv/4fv`): with `shaders` off, `getShader` returns the null program
  `gsShaderProgramBase_OGL`, whose `isValid()` is false, so the mesh collect enqueues nothing
  (`0x1003cad0.c:17-28`, `0x10036a00.c`, `0x1004a4d0.c:321`). Only primitives, particles and glow sprites would
  still draw.
* **Optional:** `GL_ARB_occlusion_query` (`occlusionQuery`, video+0x1f6; glow flares fall back to an angle
  heuristic, `0x10048f50.c:101-137`), point sprites (+0x1f7, not used by the 3D path), FBOs (+0x1fc, post effects),
  `GL_ARB_vertex_program` attribute calls (+0x1f8, not used by the 3D path), CG (+0x1f5, always forced off).

### 1.7 Render context (`ctx`, the `param_1` of every render call)

Built per viewport render; owned outside this scope (inf: gsWindow/gsVideo frame state). Fields read by the 3D path:

| off | type | meaning |
|---|---|---|
| +0x10/+0x14 | float | viewport width / height (aspect) |
| +0x28 | float | screen-area factor for glow intensity |
| +0x2c | byte | "drawing the sorted blend queue" (primitives/particles draw instead of enqueue) |
| +0x2d | byte | glow disabled for this pass |
| +0x30 | vec3 | camera position (`eyepos`) |
| +0x3c | vec3 | point on the near plane (camera + forward·near, inf) |
| +0x48 / +0x54 / +0x60 | vec3 | camera forward / right / up (particle billboards) |
| +0x6c | float | half-diagonal of the view frustum at distance 1, `sqrt(t² + (t·aspect)²)`, t = tan(fov/2) (particle size culling) |
| +0x74 | float | far distance (fog end override, sky scale) — M inf, the decompiler reuses the stack slot |
| +0x78 | float[6] | AABB of the 8 frustum corners, for the octree query |
| +0x90 | mat4 | view matrix |
| +0xd0 | mat4 | projection matrix |
| +0x110 | mat4 | view·projection·screen (glow sprite placement) |
| +0x150 | 6×{vec3 p, vec3 n} | frustum planes |
| +0x1e0 | mat4 | current world matrix (`matrix` uniform; parent world for relative children) |
| +0x220 | float | current alpha (LOD fade × mesh alpha) |
| +0x224 | uint | visible group mask (viewport `visibleGroups`) |
| +0x228 | ptr | item currently traversing children |
| +0x230 | int | use far distance as fog end |
| +0x234 | uint | pass flags (`ENVIROMENT_*`, §1.4) |
| +0x238 / +0x278 / +0x2b8 | mat4 / mat4 / vec4 | `shadowTransform` / `shadowWarp` / `shadowParams` |

---

## 2. Interfaces

### 2.1 Base interfaces (which slot each introduces)

RTTI has no vtable of its own for `gsIVideoItem`, `gsIVideoElement`, `gsI3DVideoItem` (they only appear as bases),
so the split is derived by comparing implementers (obs):

* `gsISubMesh`, `gsIMaterial`, `gsIShader`, `gsITexture` derive only from `gsIVideoItem`; their slot 1 is unrelated in
  each (setGeometry / addRef / deleting dtor / upload) but slot 0 is always "decrement refcount, delete at 0"
  → **gsIVideoItem introduces slot 0 only**.
* Every `gsIVideoElement` implementer (3D, GUI, clip area, viewport) has slot 1 = "store bool visible" → **gsIVideoElement
  introduces slot 1**.
* Slot 2 differs by family: 3D items cast the argument from `gsI3DVideoItem` (`0x10051450.c:16`), clip areas from
  `gsIClipArea` (`0x100435f0.c`), GUI items use slot 2 for a sort key (`0x10044690.c`) → **gsI3DVideoItem introduces
  slot 2**. `gsIScene` (4 slots) proves gsI3DVideoItem adds nothing beyond slot 2.

| slot | off | introduced by | method | signature | OGL implementation | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|
| 0 | 0x00 | gsIVideoItem | `release` | `void release()` | refcount−−; at 0 call the deleting dtor. 3D: count at outer+8, dtor via sub vtable slot 0; material: +4, own slot 24; texture: +0x14, slot 3; programs: no-op | – | 0x1004ffe0.c:14-16, 0x1004d970.c, 0x10040c30.c, 0x10001000.c | H obs |
| 1 | 0x04 | gsIVideoElement | `setVisible` | `void setVisible(bool)` | outer+0x14 (= sub+0x10, checked by traversal) | – | 0x10044680.c:11, 0x10046ea0.c:32 | H obs |
| 2 | 0x08 | gsI3DVideoItem | `setParent` | `void setParent(gsI3DVideoItem* parent)` | old parent `removeChild(this)`; `dynamic_cast<gs3DVideoItem_OGL*>`; store at +0x34; propagate scene pointer to the subtree (`FUN_100513e0`); new parent `addChild(this)` (scene override puts it into octree / light list) | – | 0x10051450.c, 0x100513e0.c | H obs |

There is no virtual `addRef` in the base: `gsIMesh` slot 25 and `gsIMaterial` slot 1 are type-specific addRefs; other
items are ref'ed by touching the counter directly (e.g. `0x10048680.c:23`).

### 2.2 gsIScene → gsScene_OGL (iface vt 0x1015b10c, impl 0x1015b294 / sub 0x1015b27c)

Layout: +0x68 octree root, +0x70 octree size, +0x7c light list (std::list of gsLight_OGL*), +0x84 fog on, +0x85
fog colour float[4] (unaligned), +0x98 fog start, +0x9c fog end, +0xa0 glow program. Ctor `0x10052b80` sets
scene pointer +0x38 = this and loads `shaders/glow/glow.shader` (`0x10052b80.c:47, 60-64`).

| slot | off | introduced by | method | signature | OGL implementation | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|
| 0-1 | | bases | release, setVisible | | shared | | 0x1004ffe0, 0x10044680 | H |
| 2 | 0x08 | gsI3DVideoItem | setParent | as base | variant without scene propagation (scene is its own root) | – | 0x1004fe00.c | H obs |
| 3 | 0x0c | gsIScene | `setFog` | `void setFog(bool on, const float rgba[4], float start, float end)` | stores +0x84/+0x85/+0x98/+0x9c; applied in render as GL linear fog | (glFog* in render) | 0x1004fe50.c:13-18, 0x10052ec0.c:104-117 | H obs |
| 4 | 0x10 | gsScene_OGL only | `renderGlowSprites` | `void (RenderContext*)` | additive blend, temp material with glow program, each visible glow submesh draws its sprite (submesh slot 8) | glEnable(BLEND), glBlendFunc, glBegin/glEnd via callee | 0x100505f0.c:25-42 | H obs |
| sub 1 | | gs3DVideoItem_OGL | `addChild` override | `(gsVideoItem_OGL*)` | light → light list; particle system → children; other 3D item with `(flags & 0x3000)==0` → octree (`FUN_10053720`, node stored at item sub+0x60); else children | – | 0x100537e0.c:18-42 | H obs |
| sub 2 | | gs3DVideoItem_OGL | `removeChild` override | | inverse of the above | – | 0x10052dd0.c | H obs |
| sub 4 | | gs3DVideoItem_OGL | `render` | `(RenderContext*)` | the frame pipeline of §1.2 | glEnable/Disable, glLight*, glFog*, glBlendFunc, glAlphaFunc, glDepthMask, glEnd | 0x10052ec0.c | H obs |

### 2.3 gsIMesh → gsMesh_OGL (iface vt 0x1015aa54, impl 0x1015aae4 / sub 0x1015aacc)

Layout (outer): +0x3c local bound centre, +0x48 radius, +0x4c world AABB, +0x64 octree node, +0x68 skeleton,
+0x6c position, +0x78 orientation quat (x,y,z,w), +0x88 scale, +0x8c group mask (1), +0x90 mesh flags,
+0x94 4 × `vector<gsSubMesh_OGL*>` (LODs), +0xd4 min draw distance, +0xd8 max draw distance (10 000),
+0xdc world matrix, +0x11c/+0x11d "position/orientation relative to parent", +0x11e skinned, +0x11f "share materials
on copy", +0x120 alpha (1.0), +0x124 LOD count (`0x1004d820.c`).

| slot | off | introduced by | method | signature | OGL implementation | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|
| 0-2 | | bases | release, setVisible, setParent | | setParent variant `0x100485f0` (skips if unchanged) | – | 0x100485f0.c | H |
| 3 | 0x0c | gsIMesh | `setAlpha` | `void (float)` | +0x120; 0 hides, <1 forces blended path | – | 0x10048650.c, 0x1004a4d0.c:73 | H obs |
| 4 | 0x10 | gsIMesh | `setMinDrawDistance` | `void (float)` | +0xd4; not read by the OGL render (obs) | – | 0x10048660.c | M |
| 5 | 0x14 | gsIMesh | `setMaxDrawDistance` | `void (float)` | +0xd8; LOD/fade/cull distance | – | 0x10048670.c, 0x1004a4d0.c:156 | H obs |
| 6 | 0x18 | gsIMesh | `setPosition` | `void (const vec3*, bool relative)` | +0x6c, +0x11c; recompute world AABB; move in octree | – | 0x1004cd20.c | H obs |
| 7 | 0x1c | gsIMesh | `setOrientation` | `void (const quat*, bool relative)` | +0x78, +0x11d; same | – | 0x1004ce50.c | H obs |
| 8 | 0x20 | gsIMesh | `setTransform` | `void (const vec3*, const quat*, bool relative)` | both | – | 0x1004cf80.c | H obs |
| 9 | 0x24 | gsIMesh | `setScale` | `void (float)` | +0x88 (uniform) | – | 0x10043b50.c | H obs |
| 10 | 0x28 | gsIMesh | `setGroupMask` | `void (uint)` | +0x8c, ANDed with viewport `visibleGroups` | – | 0x1004fef0.c, 0x1004a4d0.c:76 | H obs |
| 11 | 0x2c | gsIMesh | `rebuildLod` | `void (MeshData*, int lod)` | release LOD's submeshes, then `build` (slot 26) | – | 0x1004b1b0.c | H obs |
| 12 | 0x30 | gsIMesh | `setSkeleton` | `void (gsISkeleton*)` | +0x68, refcounted | – | 0x10048680.c | H obs |
| 13 | 0x34 | gsIMesh | `growRadius` | `void (float)` | radius = max(radius, r) | – | 0x100493c0.c | H obs |
| 14 | 0x38 | gsIMesh | `clearAllLods` | `void ()` | release all submeshes of all LODs | – | 0x1004b230.c | H obs |
| 15 | 0x3c | gsIMesh | `addSubMeshCopy` | `gsISubMesh* (gsISubMesh* src, int lod)` (decompiler merged args) | `src->clone(this)` appended to LOD vector, index stored at +0xa4 | – | 0x1004c2b0.c | M inf |
| 16 | 0x40 | gsIMesh | `createSubMesh` | `gsISubMesh* (const MeshData*, const SubMeshDesc*, int lod)` | new gsSubMesh_OGL, upload, bound centre, append | glGenBuffersARB, glGenQueriesARB (ctor) | 0x1004c140.c | M (arg order inf) |
| 17 | 0x44 | gsIMesh | `addLayer` | `gsISubMesh* (gsIMaterial*, bool layerableOnly, int lod)` | clone every non-decal (and, if requested, `MATERIAL_LAYERABLE`) submesh of `lod` with the given material, push into LOD 0, mark always-drawn (+0xbc) and chain via +0xb8; returns last clone | – | 0x1004c340.c | H obs |
| 18 | 0x48 | gsIMesh | `removeLayer` | `void (gsISubMesh* layer, int lod)` | remove the clone chain created by addLayer | – | 0x1004b290.c | H obs |
| 19 | 0x4c | gsIMesh | `copyFrom` | `void (gsIMesh* src, bool shareMaterials)` | clear, clone all submeshes of all LODs, copy scale/radius/flags/distances/skinned/LOD count; `shareMaterials` → +0x11f (false clones materials, `FUN_1004ed10`) | – | 0x1004c490.c, 0x100494e0.c:67-73 | M obs (src arg recovered via RTTI cast) |
| 20 | 0x50 | gsIMesh | `getSubMeshCount` | `int (int lod)` | | – | 0x100493f0.c | H obs |
| 21 | 0x54 | gsIMesh | `getSubMesh` | `gsISubMesh* (int index, int lod)` | | – | 0x1004a4b0.c | H obs |
| 22 | 0x58 | gsIMesh | `getPosition` | `const vec3* ()` | &+0x6c | – | 0x100486c0.c | H obs |
| 23 | 0x5c | gsIMesh | `getOrientation` | `const quat* ()` | &+0x78 | – | 0x100486d0.c | H obs |
| 24 | 0x60 | gsIMesh | `getAlpha` | `float ()` | +0x120 | – | 0x100486e0.c | H obs |
| 25 | 0x64 | gsIMesh | `addRef` | `void ()` | outer+8 ++ | – | 0x100486f0.c | H obs |
| 26 | 0x68 | gsMesh_OGL only | `build` | `void (MeshData*, int lod)` | renormalise skin weights; copy mesh flags/min/max distance; per SubMeshDesc (glow materials split into islands by `FUN_10088750`) create gsSubMesh_OGL or gsHWSkinSubMesh_OGL, upload, compute centre/radius (`MATERIAL_ZERO_RADIUS` → radius −100); LOD 0 sets mesh bound & octree | glGenBuffersARB, glGenQueriesARB (ctors) | 0x1004d0e0.c | H obs |
| sub 4 | | gs3DVideoItem_OGL | `render` (collect) | `(RenderContext*)` | §1.3 | – | 0x1004a4d0.c | H obs |

`MeshData` as used by the renderer (obs from `0x1004d0e0`, `0x10049710`, `0x100480d0`): [0] Vertex36* (pos f32×3,
normal f32×3, RGBA8 at +24, uv f32×2 at +28), [1] vertex count, [2] u32 absolute indices, [4] SubMeshDesc[28 B]
array, [5] submesh count, [6] uv2 float[2] per vertex or 0, [7] skin {i32 bone, f32 weight}×4 per vertex or 0,
[8] mesh flags, [9] min / [10] max draw distance. `SubMeshDesc`: +4 first index, +8 index count, +0xc first vertex,
+0x10 vertex count, +0x14 gsIMaterial*, +0x18 primitive (0 triangles, 1 lines).

### 2.4 gsISubMesh → gsSubMesh_OGL / gsHWSkinSubMesh_OGL (iface vt 0x1015aa20; impl 0x1015a9f0 / 0x1015b2c4)

Layout: +0x68 owner mesh, +0x6c material, +0x70 current program variant, +0x74 VBO, +0x78 IBO, +0x7c alpha,
+0x84 local centre, +0x48 radius, +0x90 occlusion query, +0x94 glow intensity, +0x98 glow screen pos, +0xa4 index in
LOD, +0xa8 clone source, +0xac vertex count, +0xb0 index count, +0xb4 GL primitive, +0xb8 layer link, +0xbc always
draw (`MATERIAL_LAYER`), +0xbd has uv2, +0xbe has tangents, +0xbf skinned, +0xc0 has colours, +0xc4/+0xc8 VB/IB bytes;
HW: +0xcc bone groups, +0xdc palette size (`0x100494e0.c`, `0x100550d0.c`).

| slot | off | introduced by | method | signature | OGL implementation | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|
| 0 | 0x00 | gsIVideoItem | release | | | – | 0x1004ffe0.c | H |
| 1 | 0x04 | gsISubMesh | `setGeometry` | `void (const MeshData*, const SubMeshDesc*)` | AABB over the vertex range, `upload` (slot 4), store centre | – (callee: glBindBuffer/glBufferData) | 0x10049320.c:22-32 | H obs |
| 2 | 0x08 | gsISubMesh | `setMaterial` | `void (gsIMaterial*)` | +0x6c, refcounted | – | 0x100485b0.c | H obs |
| 3 | 0x0c | gsISubMesh | `getMaterial` | `gsIMaterial* ()` | | – | 0x100485e0.c | H obs |
| 4 | 0x10 | gsSubMesh_OGL | `upload` | `void (const MeshData*, const SubMeshDesc*)` | choose vertex format from the program's attributes (§3.6), build 32- or 64-byte vertices (normals/tangents → short, tangents generated by `FUN_10048890`), IBO 16/32-bit, `glBufferDataARB` (STATIC or DYNAMIC); primitive from desc+0x18 | glBindBufferARB, glBufferDataARB | 0x10049710.c | H obs |
| 5 | 0x14 | gsSubMesh_OGL | `buildIndices` | plain: `(MeshData*, SubMeshDesc*, void* out, bool u16)`; HW: `+ (short* weights, short* indices)` | plain: rebase indices by first vertex; HW: bone-group partition + weight/index packing (§1.5) | – | 0x100480d0.c, 0x100552d0.c | H obs |
| 6 | 0x18 | gsSubMesh_OGL | `drawRange` | `void (RenderContext*, int firstIndex, int count)` | bind VBO; client arrays per format; `glDrawElements`; stats (video+0x1bc tris, +0x1c4 draws) | glBindBufferARB, glEnable/DisableClientState, glClientActiveTexture, gl*Pointer, glDrawElements | 0x10048160.c | H obs (register args lost) |
| 7 | 0x1c | gsSubMesh_OGL | `updateGlow` | `bool (RenderContext*)` | project centre to screen (ctx+0x110); with occlusion queries: samples → intensity = sqrt(samples·k²·r³/area); else angle heuristic; returns visible | glGetQueryObjectuivARB | 0x10048f50.c | M obs |
| 8 | 0x20 | gsSubMesh_OGL | `drawGlowSprite` | `void ()` | star-shaped fan (centre + 49 rim vertices) around the projected screen point; colour = material illumination ARGB (+0x18), centre alpha = 0.15·sqrt(size), size ∝ sqrt(intensity)·brightness (+0x64)·10 | glBegin, glColor4f, glTexCoord2f, glVertex2f, glEnd (callee 0x1004e6c0) | 0x10049280.c, 0x1004e6c0.c | M obs |
| 9 | 0x24 | gsSubMesh_OGL | `clone` | `gsISubMesh* (gsIMesh* newOwner)` | new object of the same class sharing VBO/IBO/bone groups | – | 0x1004a270.c, 0x100551e0.c | H obs |
| sub 4 | | gs3DVideoItem_OGL | `draw` | `(RenderContext*)` | §1.2 step 4; HW variant §1.5 | glLoadMatrixf, glBeginQueryARB, glEndQueryARB, glUniformMatrix4fvARB (HW) | 0x100484c0.c, 0x10054140.c | H obs |

### 2.5 gsISkeleton → gsSkeleton_OGL (iface vt 0x1015b0a4, impl 0x1015b1b0)

Layout: +0x68 bone count, +0x6c bones (0xac B each: parent ptr, t vec3 +4, q quat +0x10, s vec3 +0x20, world/skin
mat4 +0x2c, inverse bind mat4 +0x6c), +0x70 dirty, +0x74 root pseudo-bone (t +0x78, q +0x84, s +0x94, world +0xa0).
No draw of its own (sub render = default traversal).

| slot | off | introduced by | method | signature | OGL implementation | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|
| 0-2 | | bases | | | | – | | H |
| 3 | 0x0c | gsISkeleton | `setBoneCount` | `void (int n)` | reallocate n identity bones parented to root | – | 0x10050700.c | H obs |
| 4 | 0x10 | gsISkeleton | `setBoneParent` | `void (int bone, int parent /* -1 = root */)` | | – | 0x1004fff0.c | H obs |
| 5 | 0x14 | gsISkeleton | `setBoneLocal` | `void (int bone, const vec3* t, const quat* q, const vec3* s)` | sets TRS, dirty | – | 0x10050030.c | H obs |
| 6 | 0x18 | gsISkeleton | `setRootTransform` | `void (const vec3* t, const quat* q, float scale)` | root TRS, dirty | – | 0x100500a0.c | H obs |
| 7 | 0x1c | gsISkeleton | `captureBindPose` | `void ()` | world = S·R·T·parent per bone; invBind = inverse(world) (root treated as identity) | – | 0x10050110.c | H obs |
| 8 | 0x20 | gsISkeleton | `clone` | `gsISkeleton* ()` | copy bones (TRS, parents, invBind), unparented | – | 0x10051cf0.c | H obs |
| – | | internal | `getSkinMatrix` | `void (int bone, mat4* out)` | lazy update; `invBind·world` | – | 0x100501f0.c | H obs |

### 2.6 gsICamera → gsCamera_OGL (iface vt 0x1015b05c, impl 0x1015b138)

The camera is a passive record; the viewport reads it (`0x10044ef0.c:92-158`).

| slot | off | introduced by | method | signature | OGL implementation | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|
| 0-2 | | bases | | | | – | | H |
| 3 | 0x0c | gsICamera | `setPosition` | `void (const vec3*)` | +0x68 (default 0) | – | 0x10043af0.c | H obs |
| 4 | 0x10 | gsICamera | `setTarget` | `void (const vec3*)` | +0x74 look-at point (default (0,0,−1)) | – | 0x10043b10.c, 0x10044ef0.c:157 | H obs |
| 5 | 0x14 | gsICamera | `setUp` | `void (const vec3*)` | +0x80 (default (0,1,0)) | – | 0x1004fed0.c | H obs |
| 6 | 0x18 | gsICamera | `setFov` | `void (float degrees)` | +0x8c (vertical; viewport multiplies by π/180; ctor default 1.0472 is a radians value used as degrees) | – | 0x1004fef0.c, 0x10051740.c:30, 0x10044ef0.c:105 | H obs |
| 7 | 0x1c | gsICamera | `setNear` | `void (float)` | +0x90, −1 = use viewport `minDistance`; a camera value only raises it | – | 0x1004ff00.c, 0x10044ef0.c:96-99 | H obs |
| 8 | 0x20 | gsICamera | `setFar` | `void (float)` | +0x94, −1 = use viewport `maxDistance`; a camera value only lowers it | – | 0x1004ff10.c, 0x10044ef0.c:100-103 | H obs |

### 2.7 gsILight → gsLight_OGL (iface vt 0x1015b084, impl 0x1015b178)

| slot | off | introduced by | method | signature | OGL implementation | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|
| 0-2 | | bases | | | parenting under the scene puts it in the scene light list | – | 0x100537e0.c:18-27 | H |
| 3 | 0x0c | gsILight | `setPosition` | `void (const vec3*)` | +0x68 (world space) | – | 0x10043af0.c | H obs |
| 4 | 0x10 | gsILight | `setAmbient` | `void (const float rgba[4])` | +0x74 | – | 0x1004ff20.c | H obs |
| 5 | 0x14 | gsILight | `setDiffuse` | `void (const float rgba[4])` | +0x84 | – | 0x1004ff40.c | H obs |
| 6 | 0x18 | gsILight | `setSpecular` | `void (const float rgba[4])` | +0x94 | – | 0x1004ff70.c | H obs |

Applied by the scene as GL point lights (§1.2). Script caller: `gsLight.ambient/diffuse/specular` (ARGB ints) are
sent as `rgb · magnitude / 255, a / 255` (`FUN_10075200`) to slots 4/5/6 (`0x10091f00.c:15-19`,
`0x10091f50.c:15-19`, `0x10091fa0.c:15-19`), so colours exceed 1 when `magnitude` > 1; they are clamped only for
fixed-function (`0x10075630.c`). Nothing caps the number of lights at `GL_MAX_LIGHTS`; envFlags give 4 bits (≤ 15).

### 2.8 gsIPrimitive → gsPrimitive_OGL (iface vt 0x1015b0cc, impl 0x1015b1f0)

Layout: +0x68 Vertex36 array (same 36-byte layout as meshes, **world space**), +0x6c count, +0x70 capacity,
+0x74 material, +0x78 GL mode, +0x7c triangle count for stats.

| slot | off | introduced by | method | signature | OGL implementation | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|
| 0-2 | | bases | | | | – | | H |
| 3 | 0x0c | gsIPrimitive | `setVertexCount` | `void (int n)` | grow array; mode from creation flags (§1.1) | – | 0x10050870.c | H obs |
| 4 | 0x10 | gsIPrimitive | `setVertices` | `void (int first, int count, const Vertex36*)` | copy, update octree bounds | – | 0x100538b0.c | H obs |
| 5 | 0x14 | gsIPrimitive | `setMaterial` | `void (gsIMaterial*)` | +0x74 (only texture slot 0 used) | – | 0x10050360.c | H obs |
| sub 4 | | gs3DVideoItem_OGL | `render` | | flags 0x80 outside the sorted pass → enqueue (+0x244); else fixed-function immediate mode: view matrix only, white material, lighting off, no program, `GL_COLOR_MATERIAL`, colour ARGB→RGBA per vertex | glLoadMatrixf, glMaterialfv, glDisable/Enable, glBindTexture, glBegin, glColor4ubv, glNormal3fv, glTexCoord2fv, glVertex3fv, glEnd | 0x10050dd0.c | H obs |

### 2.9 gsIParticleSystem → gsParticleSystem_OGL (iface vt 0x1015b0e8, impl 0x1015b224)

Layout: +0x68 material, +0x6c particle count, +0x70 particle list (32-B nodes: pos vec3, size, angle, ARGB colour,
next, prev), +0x74 scene it registered with, +0x78 max distance, +0x7c program variant.

| slot | off | introduced by | method | signature | OGL implementation | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|
| 0-1 | | bases | | | | – | | H |
| 2 | 0x08 | gsI3DVideoItem | setParent | override | also (un)registers with the scene while particles exist so the scene traverses it once | – | 0x100514a0.c | H obs |
| 3 | 0x0c | gsIParticleSystem | `setMaterial` | `void (gsIMaterial*)` | +0x68, variant = base program `getVariant(flags,0,0)` | – | 0x10050390.c | H obs |
| 4 | 0x10 | gsIParticleSystem | `setMaxDistance` | `void (float)` | +0x78, 0 = unlimited | – | 0x100503e0.c, 0x10050a60.c:38-53 | H obs |
| 5 | 0x14 | gsIParticleSystem | `createParticle` | `Particle* ()` | push node, count++, refcount++ | – | 0x100509a0.c | H obs |
| 6 | 0x18 | gsIParticleSystem | `destroyParticle` | `void (Particle*)` | unlink, count−−, unregister when empty, release | – | 0x100503f0.c | H obs |
| 7 | 0x1c | gsIParticleSystem | `setParticle` | `void (Particle*, const vec3* pos, float size, float angle, const uint* argb)` | writes node fields | – | 0x10050460.c | H obs |
| sub 4 | | gs3DVideoItem_OGL | `render` | | draws only when the current traversal parent (ctx+0x228) is the scene it registered with (or its parent), so each system is drawn once; skipped in the shadow-map pass for additive materials; drops particles whose size² ≥ d²·(ctx+0x6c)²/4 (larger than half the screen diagonal) plus a view-direction test (`FUN_10083990`); sorted (flags 0x80) → one blend-queue entry per particle; else begin/draw/end immediately. Draw = CPU billboard quad (camera right/up rotated by the particle angle, half-size = size), alpha scaled by `d²·k²/(4·size²) − 1` (fades as the particle approaches screen size), hard cut at max distance | glBegin(GL_QUADS), glColor4ubv, glNormal3fv, glTexCoord2fv, glVertex3fv, glEnd, glBlendFunc, glMaterialfv, glBindTexture, glLoadMatrixf | 0x10051560.c:26-84, 0x10051150.c, 0x10050a60.c, 0x100504a0.c | M obs (begin() args partly lost) |

### 2.10 gsIMaterial → gsMaterial_OGL (vt 0x1015ab5c)

Layout: +4 refcount, +8 base program (`gsIShader*`), +0xc flags (`MATERIAL_*`), +0x10..+0x20 ARGB ints (ambient,
diffuse, illumination, specular, colorize), +0x24 ambient rgba, +0x34 diffuse rgba, +0x44 emission rgba
(= illumination × brightness), +0x54 specular rgba, +0x64 brightness / glow size, +0x68 glossiness (`GL_SHININESS`),
+0x6c reflection, +0x70 colorize rgba, +0x80 texture slots[7], +0x9c custom vec4[8], +0x11c depth offset.
Defaults: ambient/diffuse 0.5 grey, specular/emission black, colorize red (`0x1004ebc0.c`). ARGB ints are
`0xAARRGGBB`; `FUN_10075040` converts to float rgba.

| slot | off | introduced by | method | signature | OGL implementation | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|
| 0 | 0x00 | gsIVideoItem | release | | | – | 0x1004d970.c | H |
| 1 | 0x04 | gsIMaterial | `addRef` | `void ()` | | – | 0x1004d980.c | H obs |
| 2 | 0x08 | gsIMaterial | `setTexture` | `void (int slot, gsITexture*)` | +0x80+4·slot, no bounds check | – | 0x1004d990.c | H obs |
| 3 | 0x0c | gsIMaterial | `setShader` | `void (gsIShader* baseProgram)` | +8 | – | 0x1004d9c0.c | H obs |
| 4 | 0x10 | gsIMaterial | `setFlags` | `void (uint)` | +0xc | – | 0x1004d9e0.c | H obs |
| 5 | 0x14 | gsIMaterial | `setAmbient` | `void (const uint* argb)` | +0x10 / +0x24 | – | 0x1004d9f0.c | H obs |
| 6 | 0x18 | gsIMaterial | `setDiffuse` | `void (const uint* argb)` | +0x14 / +0x34 | – | 0x1004da30.c | H obs |
| 7 | 0x1c | gsIMaterial | `setSpecular` | `void (const uint* argb, float glossiness, float reflection)` | +0x1c / +0x54, +0x68, +0x6c | – | 0x1004da70.c | H obs |
| 8 | 0x20 | gsIMaterial | `setIllumination` | `void (const uint* argb, float brightness)` | +0x18, +0x44 = rgb·brightness/255, +0x64 | – | 0x1004dac0.c, 0x10075200.c | H obs |
| 9 | 0x24 | gsIMaterial | `setDepthOffset` | `void (float)` | +0x11c → `glPolygonOffset(2d, 2d)`; loader sets −1 for decals | – | 0x1004db10.c, 0x1008ec70.c:130-133 | H obs |
| 10 | 0x28 | gsIMaterial | `setColorize` | `void (const uint* argb)` | +0x20 / +0x70 (`colorize` uniform) | – | 0x1004db20.c | H obs |
| 11 | 0x2c | gsIMaterial | `setShaderParam` | `void (int i, const float v[4])` | +0x9c+16i (`custom<i>` uniform) | – | 0x1004db60.c | H obs |
| 12 | 0x30 | gsIMaterial | `getTexture` | `gsITexture* (int slot)` | | – | 0x1004db90.c | H obs |
| 13 | 0x34 | gsIMaterial | `getShader` | `gsIShader* ()` | | – | 0x1004dba0.c | H obs |
| 14 | 0x38 | gsIMaterial | `getFlags` | `uint ()` | | – | 0x1004dbb0.c | H obs |
| 15 | 0x3c | gsIMaterial | `getAmbient` | `void (uint* argb)` | | – | 0x1004dbc0.c | H obs |
| 16 | 0x40 | gsIMaterial | `getDiffuse` | `void (uint*)` | | – | 0x1004dbe0.c | H obs |
| 17 | 0x44 | gsIMaterial | `getSpecular` | `void (uint*, float* gloss, float* refl)` | | – | 0x1004dc00.c | H obs |
| 18 | 0x48 | gsIMaterial | `getIllumination` | `void (uint*, float* brightness)` | | – | 0x1004dc40.c | H obs |
| 19 | 0x4c | gsIMaterial | `getColorize` | `void (uint*)` | | – | 0x1004dc70.c | H obs |
| 20 | 0x50 | gsIMaterial | `getDepthOffset` | `float ()` | | – | 0x1004dc90.c | H obs |
| 21 | 0x54 | gsIMaterial | `getShaderParam` | `void (int i, float out[4])` | | – | 0x1004dca0.c | H obs |
| 22 | 0x58 | gsIMaterial | `equals` | `bool (const gsIMaterial*)` | same combiner, flags, colours, brightness, gloss, reflection, depth offset, textures, custom params | – | 0x1004dcd0.c | H obs |
| 23 | 0x5c | gsIMaterial | `clone` | `gsIMaterial* ()` | deep copy (texture refs ++) | – | 0x1004f070.c → 0x1004ed10.c | H obs |
| 24 | 0x60 | gsIMaterial | deleting dtor | `void* (uint flags)` | releases textures | – | 0x1004e5e0.c | H obs |

**Material GL state** (full bind `0x100478f0`, delta bind `0x10047bc0` against the global current material
`DAT_10194360`; unbind `0x10047860`):

| condition | GL | evidence |
|---|---|---|
| `TWOSIDED` 0x1000 | cull face off, else on | 0x100478f0.c:50-56 |
| not `ALPHABLEND`/`DECAL` and alpha 1 | blend off unless `ALPHATEST`, which sets `glAlphaFunc(GREATER, 0.2)` | :57-64 |
| `ALPHABLEND` 1 or `DECAL` 0x8000 or alpha < 1 | blend on, `glAlphaFunc(GREATER, 0.001)`, polygon offset fill with (−2,−2) | :65-70 |
| depth write | `glDepthMask(!(flags & (ALPHABLEND \| DECAL)))` | :71-72 |
| blend func | `MODULATE` 0x40000 → (DST_COLOR, ZERO); `MODULATE_2` 0x80000 → (DST_COLOR, SRC_COLOR); `ADDITIVE` 0x80 → (ONE, ONE); `ADDITIVE_A` 0x100 → (SRC_ALPHA, ONE); else (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) | :73-99 |
| `glMaterialfv(FRONT_AND_BACK, …)` | AMBIENT = ambient.rgb, ambient.a × object alpha; DIFFUSE; EMISSION; SPECULAR; SHININESS = glossiness; `glColor4f(1,1,1,1)` | :100-109 |
| textures | unit i (6..0) ← slot i, 2D or cube (`texture+0x18`), disabled when empty | :110-123 |
| depth offset ≠ 0 | polygon offset fill (2d, 2d) | :124-128 |
| shadow-map pass | only unit 0 for alpha-tested/blended, cull per TWOSIDED, diffuse, white | :27-48 |

**Material from a `.material` file** (`0x1008ec70`): textures are collected into a map keyed diffuse 0, specular 1,
normal 2, illumination 3, ambient occlusion 4, environment cube (`textures/envmap.dds`, when the reflection byte is
set) 5, then assigned to material slots **0..n in that order** (compact; "Too many maps" after 7). Flags:
diffuse → `DIFFUSEMAP` (+ `ALPHABLEND` if alpha bits > 1, `ALPHATEST` if 1 bit, `COLORIZE` if alpha is a colour
mask); normal → `NORMALMAP`; specular → `SPECULARMAP` (+colour/gloss); illumination → `ILLUMINATIONMAP`; AO →
0x100000; two-sided → 0x1000; reflection → 0x2000; specular level > 4 with non-black colour → `SPECULAR`; decal
bit → `DECAL` (clears `ALPHABLEND`) + depth offset −1; byte 0x20 == 0x1f → 0x8000000; option bit 4 → 0x1000000
("UV2 primary": upload puts uv2 into the uv0 slot). Base program = `getShader(<constant path, presumably empty →
default.shader>, flags, 0)`; scripts later replace it with `gsMaterialVariant.setShader` (`0x1008ec70.c:113-136,
334-403`; `docs/handlers/batch-B.md`).

### 2.11 gsIShader → gsShaderProgram_OGL / gsShaderProgramBase_OGL (vt 0x10159b50 / 0x10159b10)

`gsShaderProgram_OGL` (0x9c B): +4 combiner, +8/+0xc/+0x10 variant key (mat/mesh/env), +0x1c GL program, +0x20
attributes, +0x24 textures, +0x28 parameters, +0x2c..+0x94 uniform locations, +0x98 unknown. `gsShaderProgramBase_OGL`
is the null program used when GLSL is off (`0x10037190.c:27-39`).

| slot | off | introduced by | method | signature | gsShaderProgram_OGL | Base (null) | GL | evidence | conf |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 0x00 | gsIVideoItem | release | | no-op | no-op | – | 0x10001000.c | H |
| 1 | 0x04 | gsIShader | deleting dtor | `void* (uint)` | `glDeleteObjectARB(program)` | | glDeleteObjectARB | 0x10036eb0.c, 0x10036ca0.c | H obs |
| 2 | 0x08 | gsIShader | `getVariant` | `gsIShader* (gsIShader* prev, uint mat, uint mesh, uint env)` | mask with combiner masks (shadow passes reduce, §3.5); reuse `prev` if same key else `combiner->variant(...)` (compile on miss) | returns this | – | 0x1003ca40.c, 0x1003c9c0.c | H obs |
| 3 | 0x0c | gsIShader | `apply` | `void (RenderContext*, gsMaterial_OGL*, bool programChanged)` | if changed: `glUseProgram`, sampler ints, per-frame uniforms; always: per-material uniforms (§3.3) | no-op | glUseProgramObjectARB, glUniform1iARB/1f/3fv/4fv, glUniformMatrix3fv/4fv | 0x100365f0.c | H obs |
| 4 | 0x10 | gsIShader | `unbind` | `void ()` | `glUseProgramObjectARB(0)` | no-op | glUseProgramObjectARB | 0x100368f0.c | H obs |
| 5 | 0x14 | gsIShader | `setBones` | `void (gsSkeleton_OGL*, const int* palette, int n)` | skin matrices → `bones` | no-op | glUniformMatrix4fvARB | 0x10036d70.c | H obs |
| 6 | 0x18 | gsIShader | `getAttributes` | `uint ()` | +0x20 | 0x3000100 (POSITION \| NORMAL \| TEXCOORD0) | – | 0x10036d00.c, 0x100369d0.c | H obs |
| 7 | 0x1c | gsIShader | `getTextures` | `uint ()` | +0x24 | 1 | – | 0x10036d10.c | H obs |
| 8 | 0x20 | gsIShader | `getParameters` | `uint ()` | +0x28 (low byte = bone count) | 0 | – | 0x10036d20.c | H obs |
| 9 | 0x24 | gsIShader | `usesTextureUnit` | `bool (int unit)` | bit test on +0x24 | unit == 0 | – | 0x10036d30.c | H obs |
| 10 | 0x28 | gsIShader | `setUserData` | `void (int)` | +0x98 (reader not found) | no-op | – | 0x10036d50.c | L |
| 11 | 0x2c | gsIShader | `isValid` | `bool ()` | GL program ≠ 0 | false | – | 0x10036d60.c, 0x10036a00.c | H obs |

### 2.12 gsShaders_OGL / gsShaderCombiner_OGL (shader cache and variant generator)

* `gsShaders_OGL` (video+0x1a8): +0xe4 map path → combiner, +0xf0 currently bound program, +0xf4 null program,
  +0xf8 GLSL enabled, +0xfc effect timer (`0x10037190.c`). `getShader(path, mat, mesh, env)` (`0x1003cad0`):
  with GLSL off returns the null program; else loads the combiner script on first use (`FUN_1003bd80`) and returns
  `combiner->variant(mat, mesh, env)`. Bind = `FUN_10036940(shaders, ctx, material, program)` → `program->apply(ctx,
  material, program != current)`; unbind = `FUN_10036980`.
* `gsShaderCombiner_OGL` is a script object (the `.shader` file is a GSystem script). Fields: `matMask` +0x118,
  `meshMask` +0x11c, `envMask` +0x120, `attributes` +0x124, `textures` +0x128, `parameters` +0x12c, `vertexComponent`
  +0x130, `fragmentComponent` +0x134, base combiner +0xcc, variant map +0x10c keyed by the masked triple
  (`0x1003c9c0.c`).
* **`setBaseShader(path)`** (`0x1003bf40`): path resolved against the combiner's directory, base combiner looked up /
  loaded through the same cache and ref'ed, and its three masks copied into this combiner. When a variant is generated
  (`FUN_10036b80`), the base's `onProgram` runs first (recursively), its attributes/textures/parameters and copies of
  its component maps are inherited, and then this combiner's own `onProgram($matFlags, $meshFlags, $envFlags)` event
  (id 6) runs and may override entries, e.g. `this.vertexComponent["alter"] = "/metro/shaders/effects/alter-wave.vertex"`.
* Variant compile (`0x1003c270`): components are either one file each or ordered maps name → file; each file is a
  template run through the GSystem script VM (`FUN_100382a0`): lines starting `#` are script (`#if`, `#for`,
  `#$var = …`), other lines are emitted, `#once#` lines are emitted once per program (shared declarations across
  components). Variables: `$matFlags`, `$meshFlags`, `$envFlags`, `$directionalCount` = env>>8 & 0xf, `$omniCount` =
  env>>12 & 0xf, `$spotCount` = env>>16 & 0xf (`0x100382a0.c:40-112`). The concatenated vertex and fragment sources are
  compiled, attached, linked; uniform locations are looked up only for the bits set in `textures`/`parameters`
  (`0x1003a640.c`). Link failure logs `Linking shader "<path>" failed:`; the program handle stays non-zero, so
  `isValid()` still reports it usable (inf, `0x1003a640.c:37-58`).

### 2.13 Internal bases gsVideoItem_OGL (vt 0x1015a8f4) / gs3DVideoItem_OGL (vt 0x1015b044)

sub layout: +4 refcount, +8 gsVideo_OGL*, +0xc creation flags, +0x10 visible, +0x14 sort key, +0x18 number of
children with a sort key, +0x1c children-dirty, +0x20 `vector<gsVideoItem_OGL*>` children; gs3DVideoItem_OGL adds
+0x30 parent, +0x34 scene, +0x38 bound centre, +0x44 radius, +0x48 world AABB, +0x60 octree node
(`0x10047340.c`, `0x10050540.c`).

| slot | method | signature | default implementation | overridden by | evidence | conf |
|---|---|---|---|---|---|---|
| 0 | scalar deleting dtor | `void* (uint)` | | every class | 0x100469b0.c, 0x100505d0.c | H |
| 1 | `addChild` | `void (gsVideoItem_OGL*)` | parent refcount++, push, track sort keys | gsScene_OGL | 0x10047630.c | H obs |
| 2 | `removeChild` | `void (gsVideoItem_OGL*)` | erase, parent refcount−− (may delete) | gsScene_OGL | 0x100466d0.c | H obs |
| 3 | `onChildSortKeyChanged` | `void (child, int old, int new)` | marks children dirty | – | 0x10044630.c | H obs |
| 4 | `render` | `void (RenderContext*)` | sort children by sort key (sub+0x14) if dirty, ctx+0x228 = this, call `render` on visible children | scene, mesh, submeshes, primitive, particles | 0x10046ea0.c | H obs |

Camera, light and skeleton keep the default `render` (pure traversal). Child transforms are not inherited through
skeletons or scenes; only a mesh pushes its world matrix into ctx+0x1e0 for its children (`0x1004a4d0.c:160-207`).

---

## 3. Shader interface contract

### 3.1 Attribute flags (`this.attributes`) and how the engine feeds them

| flag | value | GL source in `drawRange` | GLSL built-in | VBO data |
|---|---|---|---|---|
| POSITION | 0x1000000 | `glVertexPointer(3, FLOAT)` @0 | `gl_Vertex` | float3 |
| NORMAL | 0x2000000 | `glNormalPointer(SHORT)` @0x10 (or `BYTE` @0xc in the tangent layout) | `gl_Normal` | normalised short3/byte3 |
| COLOR | 0x4000000 | `glColorPointer(4, UNSIGNED_BYTE)` @0xc | `gl_Color` | RGBA8; also forced on when any vertex colour ≠ 0xFFFFFFFF |
| TEXCOORD0 | 0x100 | unit 0, float2 @0x18 (32-B) / @0x20 (64-B) | `gl_MultiTexCoord0` | uv (or uv2 with "UV2 primary") |
| TEXCOORD1 | 0x200 | unit 1, float2 @0x28 | `gl_MultiTexCoord1` | uv2 (copy of uv when absent) |
| TANGENT | 0x10000 | unit 2, `SHORT`×4 @0x10 (32-B) / @0x18 (64-B) | `gl_MultiTexCoord2` (xyz × 1/32767, w = handedness) | generated (`FUN_10048890`) |
| WEIGHTx<n> | n (low 4 bits) | unit 3, `SHORT`×4 @0x30 (+ unit 4 @0x38 for weights 5-8) | `gl_MultiTexCoord3`/`4` × 1/32767 | §1.5 |
| INDEXx<n> | 0x10·n | unit 4, `SHORT`×4 @0x38 | `int(gl_MultiTexCoord4.x..w)` | palette indices |
| POINTSIZE | 0x20000 | not used by the 3D path | – | – |

Vertex layouts written by `upload` (`0x10049710.c`): **A** 32 B {pos, rgba8 @0xc, normal short4 @0x10, uv @0x18} when
no uv2/skin/tangents; **B** 32 B {pos, normal byte4 @0xc, tangent short4 @0x10, uv @0x18} when tangents but no
colours; **C** 64 B {pos, rgba8 @0xc, normal short4 @0x10, tangent short4 @0x18, uv0 @0x20, uv1 @0x28,
weights short4 @0x30, indices/weights2 short4 @0x38} otherwise. Layout choice uses the program variant the submesh
had at upload time (material flags, mesh flags, env 0).

### 3.2 Texture units

* Material slot i → texture unit i → sampler `texture<i>` = i (`glUniform1iARB`, `0x100365f0.c:16-39`), bound as 2D or
  cube. Unit 7 = shadow map (`texture7`, `sampler2DShadow` for `ENVIROMENT_SHADOWMAP`, plain 2D screen-space shadow
  for `ENVIROMENT_SHADOW`), bound by the shadow effect.
* Order expected by `default.shader` (`maps.fragment`): diffuse, mix maps (`MIX2MAP` 1, `MIX3MAP` 2), specular,
  normal, illumination, ambient occlusion, then the reflection cube at index `textureCount`. The `.material` loader
  produces exactly this compact order (§2.10). Asset-format slot → unit therefore depends on which maps exist: e.g.
  diffuse+normal+AO → units 0, 1, 2.

### 3.3 Uniforms (names from `0x15a028..0x159f78`, lookup order in `0x1003a640.c`)

| uniform | type | enabled by `parameters`/`textures` bit | value | set when | evidence |
|---|---|---|---|---|---|
| `texture0`..`texture7` | sampler | TEXTURE0..7 (1<<i) | unit index i | program change | 0x100365f0.c:16-39 |
| `matrix` | mat4 | `MATRIX` 0x100 | ctx+0x1e0 current object world | every apply | :68-70 |
| `eyepos` | vec3 | `EYEPOS` 0x200 | camera world position ctx+0x30 | program change | :40-42 |
| `timer` | float | `SHADER_TIMER` 0x400 | shaders+0xfc effect timer | program change | :43-46 |
| `reflection` | float | `SHADER_REFLECTION` 0x800 | material +0x6c | every apply | :65-67 |
| `worldup` | vec3 | `SHADER_WORLDUP` 0x1000 | view rotation applied to DAT_10193d84 (inf: (0,1,0)) | program change | :47-50 |
| `colorize` | vec4 | `SHADER_COLORIZE` 0x2000 | material +0x70 | every apply | :71-73 |
| `vieworient` | mat3 | `SHADER_VIEWORIENT` 0x4000 | upper 3×3 of the view matrix | program change | :51-54 |
| `shadowTransform` | mat4 | `SHADER_SHADOWTRANS` 0x8000 | ctx+0x238 | program change | :55-57 |
| `shadowWarp` | mat4 | `SHADER_SHADOWWARP` 0x10000 | ctx+0x278 | program change | :58-60 |
| `shadowParams` | vec4 | `SHADER_SHADOWPARAMS` 0x20000 | ctx+0x2b8 (x offset, y/z fade range, w depth bias) | program change | :61-63 |
| `bones` | mat4[n] | `BONEx<n>` (low byte) | skin matrices of the palette | per bone group | 0x10036d70.c |
| `custom0`..`custom7` | vec4 | `SHADER_CUSTOM0..7` (0x1000000<<i) | material +0x9c+16i | every apply | :74-98 |

Note: per-frame uniforms are only re-sent when the program changes; a port that caches state per program must still
refresh them every frame/pass (the engine relies on the unbind at the start of each pass to force `programChanged`).

Fixed-function state the shipped GLSL reads (must become uniforms in a port): `gl_ModelViewMatrix`,
`gl_ModelViewProjectionMatrix`, `gl_NormalMatrix`; `gl_LightSource[i].position` (view space),
`gl_FrontLightProduct[i].{ambient,diffuse,specular}`, `gl_LightModel.ambient` (0.1 grey); `gl_FrontMaterial.{ambient
(alpha = object fade), diffuse, emission, specular, shininess}`; `gl_Fog.{start, scale, color}`; `gl_Color`
(vertex colour, else (1,1,1,1)); `gl_FragCoord` (screen-space shadow lookup with hard-coded 1/1600, 1/900).

### 3.4 Flags fed to the combiner

* `matFlags` = material flags (+ `ALPHABLEND` when the object fades); `meshFlags` = mesh flags (+ `MESH_SKINNED`
  0x100 when the mesh is skinned *and* has a skeleton); `envFlags` = pass flags | lightCount<<12.
* `default.shader` masks: mat = DIFFUSEMAP|SPECULAR|NORMALMAP|SPECULARMAP|MIX2MAP|MIX3MAP|REFLECTION|COLORIZE|
  ILLUMINATIONMAP|AMBIENTOCCLUSIONMAP|SWAP_UV|ALPHABLEND|ALPHATEST|ADDITIVE|ADDITIVE_A; mesh = MESH_SKINNED;
  env = FOG|LIGHTS|SHADOWMAP|SHADOW_RENDER|SHADOWMAP_RENDER|SHADOW. Skinning: `$settings.$indexingInShaders` →
  `WEIGHTx4|INDEXx4` + `BONEx48`, else `WEIGHTx8` + `BONEx8`.

### 3.5 Variant key reduction (`getVariant`, `0x1003ca40.c`)

| pass | mat key | env key |
|---|---|---|
| shadow-map render (env & 4) | mat & matMask & (DIFFUSEMAP \| ALPHABLEND \| ALPHATEST) | env & envMask & 4 |
| screen-space shadow render (env & 8) | mat & matMask & DIFFUSEMAP | env & envMask & (SHADOWMAP \| SHADOW_RENDER) |
| normal | mat & matMask | env & envMask |

mesh key = mesh & meshMask in all passes.

---

## 4. Backend port checklist (3D scope)

**Objects to implement**

1. Factories on the video object (§1.1) returning objects that implement the interface slots of §2 with the same
   vtable order (scripts and engine code call by slot), including the hidden extras the engine calls by offset:
   `gsScene` slot 4, `gsMesh` slot 26, `gsSubMesh` slots 4-9, the internal render protocol.
2. Refcount semantics: parent pinned by children; submesh clones share GPU buffers; programs never freed by release.
3. A scene graph with octree culling for static items, a children list for force-visible/dynamic ones, a light list
   and fog; mesh LOD (4 levels, 0.333 bands, fade-out between D and 1.1·D, optional cross-fade) and group masks.
4. Four render queues with the exact ordering and sort keys of §1.2 (opaque by decal/program/textures; alpha-test and
   blend far→near on signed squared distance minus radius; decal bias 0.9999; particles merged per particle into
   the blend queue).
5. Geometry: convert `Vertex36` + uv2 + skin to GPU buffers (normals/tangents as snorm16 or snorm8, weights as
   snorm16, indices 16/32-bit by vertex count); generate tangents like `FUN_10048890`; primitive type triangles or
   lines; "UV2 primary" swap; `GL_DYNAMIC_DRAW` hint for mesh flag 0x200.
6. GPU skinning with bone-group partitioning, or (simpler) a single draw with all bones in a storage buffer — the
   result must equal `Σ wᵢ · (v · invBindᵢ · worldᵢ)` with weights renormalised over `maxVertexWeights`.
7. Immediate-mode replacements: dynamic vertex buffers for primitives (world space, untransformed, unlit, vertex
   colour), particle billboards and glow sprites.
8. Occlusion queries for glow flares (or the angle fallback) — read back one frame later instead of stalling.

**Behaviours to reproduce**

* Row-vector matrices; `local = S·R·T`, `world = local·parentWorld`, modelview = `world·view`; quaternions applied
  as conjugates by `FUN_100859d0` (matches asset docs); camera = gluLookAt(position, target, up); projection =
  gluPerspective(fov **degrees**, aspect, near, far) with GL clip-space z ∈ [−1, 1] — remap to [0, 1] and flip Y
  for D3D12/Vulkan/Metal; mirror flags flip cull order.
* Texture V: the engine uploads uv unchanged (`0x10049710.c:133-134`, no V flip anywhere in the 3D path); asset V is
  stored negated and works because textures use `GL_REPEAT` (`0x10040c40.c:21-22`) with the DDS top row at t = 0 —
  keep REPEAT addressing (or add 1 to V) and upload DDS rows in file order.
* Colours: ARGB ints `0xAARRGGBB` → RGBA (`FUN_10075040`, `FUN_1004e520`); fixed-function light colours clamped to
  [0,1], GLSL ones not.
* Depth/blend rules of §2.10: blended and decal materials do not write depth; blended/decal/fading draw with polygon
  offset (−2,−2); material depth offset d → (2d, 2d); alpha test `> 0.2` for `ALPHATEST`, `> 0.001` otherwise;
  alpha-tested geometry is drawn with blending still enabled (soft cut-out edges) and depth write on;
  object fade passed through `gl_FrontMaterial.ambient.a`; `MODULATE`, `MODULATE_2`, `ADDITIVE`, `ADDITIVE_A`
  blend modes; additive materials fog towards black (fog.fragment).
* Lighting: up to 15 point lights, view-space position, no attenuation, plus light-model ambient 0.1 and material
  emission; fog linear with `gl_Fog.scale = 1/(end−start)`.
* Shadow pass reductions (§3.5), `MATERIAL_NO_SHADOW`, coarsest LOD for flag 0x4000, doubled culling radius,
  sky objects skipped.
* Shader system: either keep the script-driven combiner (it only needs a GLSL compiler and the uniform/attribute
  contract of §3) and translate GLSL 1.10 with fixed-function built-ins to the target (e.g. inject a prelude that
  declares the built-ins as uniforms, then glslang → SPIR-V → SPIRV-Cross), or hand-port `default.shader` and its
  ~60 component files. Variants are keyed by (mat & mask, mesh & mask, env & mask) and compiled lazily on first use;
  pre-warming avoids hitches.
* Keep the hard-coded 1600×900 screen-space shadow lookup in mind (`shadow.fragment`): a resolution-independent port
  should pass the real target size.

---

## 5. Open questions

1. `gsIMesh` slots 15/16: Ghidra merged stack arguments; the exact parameter order (`lod` position) needs a disassembly
   check (`0x1004c2b0.c:17`, `0x1004c140.c:57`).
2. Glow queue sort key submesh+0x80 is only ever zeroed (`0x100494e0.c:39`) — dead feature or written through an
   unrecognised alias?
3. `gsSubMesh_OGL::drawRange` (`0x10048160`) lost the register-held booleans that select colour/uv2/skin arrays; the
   mapping to +0xbd..+0xc0 is inferred from `upload`.
4. HW skin draw uses an identity world when skeleton and mesh share a parent (`0x10054140.c:35-46`): confirm that the
   script layer then writes the full world transform into `setRootTransform`.
5. Bone-group partitioning writes per-vertex weights per group; a vertex shared by triangles in two groups keeps the
   last group's palette mapping (`0x100552d0.c:409-441`). Do shipped skinned meshes ever hit this (artefact) or does
   the exporter split such vertices?
6. `gsIShader` slot 10 (`+0x98`) has no reader found.
7. `MATERIAL_*` 0x8000000 (byte 0x20 == 0x1f, road surfaces) and 0x1000000 (UV2 primary) are not named script
   constants; 0x8000000 has no reader in the 3D path.
8. Camera ctor default fov 1.0472 is interpreted as degrees by the viewport (`0x10051740.c:30`,
   `0x10044ef0.c:105`); harmless only if scripts always set `fov`.
9. Particle `begin` (`0x10051150`) lost its GL arguments (blend factors, depth mask); values above are inferred from
   the matching `end` (`0x100504a0`) and the material bind.
10. Which object owns the render context (ctx) and how many contexts exist per frame (reflection/shadow viewports) is
    outside this scope.
