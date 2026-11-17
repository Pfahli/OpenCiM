# Vulkan render backend: plan

Status (2026-11-17): plan written; §8 tracks the steps. The game draws through OpenGL 3.3 core only. This document
says where a second backend plugs in, how the game's GLSL reaches SPIR-V, how Vulkan comes into the build, and how a
missing or failing Vulkan device falls back to OpenGL. The reverse-engineered contract of the original renderer is in
`docs/renderer/` at the repository root (`README.md`, `interfaces-3d.md`, `interfaces-2d-output.md`); this is the
port's side.

Goals, in order: (1) the same picture as OpenGL for every scene the port draws, chosen with the existing setting
`$regsRenderer` (graphics page: "OpenGL 3.3" / "Vulkan"); (2) OpenGL keeps working unchanged and stays the default and
the fallback; (3) a base for raytracing, DLSS and multiview stereo later (§9). Not a goal for the first version: being
faster than OpenGL. Parity first, then the Vulkan-only features.

## 1. Where OpenGL is called today

| File | GL call sites | What |
|---|---|---|
| `src/render/renderer3d.cpp` | ~420 | meshes (VAO/VBO/IBO), render targets (FBO, MSAA renderbuffers, depth textures), the default shader, the combiner programs and their uniforms, the queues, particles, post quads, blits, readback |
| `src/render/renderer2d.cpp` | ~130 | GUI batches, textures from DDS, GUI material programs, scissor, projection, screenshots |
| `src/engine/traffic_maps.cpp` | 33 | float/byte data textures for the shader hooks (lanes, footprints) |
| `src/engine/surface_map.cpp` | 26 | surface-class and top-map textures |
| `src/engine/effects.cpp` | 25 | binding post-effect targets, viewport and scissor around `drawQuad` |
| `src/engine/night_lights.cpp` | 5 | the light map's lamp half (`TexSubImage2D`) |
| `src/engine/display.cpp` | 4 | `gl::load`, reading a target back (`dumpTarget`) |
| `src/platform/window_win32.cpp` | — | the window, WGL context, `SwapBuffers`, vsync |
| `src/tools/regs_tool.cpp` | — | a hidden window and context for `regs-tool shaders` (compile checks) |

The engine above `render/` holds GL **names** (`uint32_t`) in a few structs: `GpuMaterial::textures`,
`GpuMesh::vao/vbo/ibo`, `RenderTarget::fbo/color/depth`, the combiner variants' program numbers, `TextureObj::gl`,
the hook maps in `SceneView`. Everything else (queues, sort keys, uniforms per pass, the combiner, the translator) is
already backend-neutral logic that happens to call GL.

## 2. The boundary: `render/gpu.h`

A device interface at the level the renderer already speaks: a small state machine with handles, shaped like the
subset of GL 3.3 the port uses. Not a "high-level renderer" interface (draw a scene, draw a GUI): that logic is the
part both backends share, and duplicating it would make parity a moving target. The handles stay `uint32_t`, so the
structs above keep their types and the engine code only changes where it calls GL itself.

```
gpu::Device (abstract)            GL device: one-line pass-throughs     Vulkan device: emulation (§5)
  textures   create(kind 2D/cube/renderbuffer, format, w, h, mips, samples), upload(level, face, rect, data),
             sampler state (filter, wrap, anisotropy, border, depth compare), read back, destroy
  buffers    create(vertex/index, size, data, static/stream), update(offset, data) with GL's orphan semantics
  vertex     input layouts (GL's VAO): attribute slot → buffer, format, stride, offset; index buffer
  programs   build(vs, fs GLSL 3.30, attribute names → slots), uniform location by name, set uniform on the current
             program (1i/1f/3f/4f/3fv/4fv/mat3/mat4, arrays), use
  targets    framebuffer(colour texture, depth texture or renderbuffer), bind (0 = the back buffer), blit (colour/depth,
             nearest), read pixels
  state      viewport, scissor (on/off, rect), clear (colour/depth), blend (on/off, src/dst factors), depth test, depth
             write, depth func, cull (on/off, face), polygon offset, colour mask
  draw       arrays / elements (triangles, lines) on the bound layout
  frame      begin, present (vsync), info string, limits (max anisotropy, texture units)
```

The GL device is literally the existing calls moved behind the interface, so the OpenGL picture cannot change
(checked by comparing screenshots before/after, §8 step 2). The renderer files lose `#include "render/gl.h"`;
`gl.h` becomes private to `render/gpu_gl.cpp`.

Why a GL-shaped interface and not Vulkan-shaped (explicit pipelines, descriptor sets, command buffers) for the first
version: the game's shaders are data. The combiner builds GLSL 1.10 per material and pass at load time, mods add
hook functions, and the uniforms are set by name per draw (`regs_FrontMaterial.diffuse`, `custom0`…`custom7`,
`bones[48]`, the night-light and hook uniforms). Re-expressing all of that as explicit Vulkan objects is the same work
as an emulation layer, done in every caller instead of once. The emulation (§5) keeps the explicit Vulkan objects
inside the device, where a later, more explicit path (raytracing, §9) can use them directly.

## 3. Shaders: GLSL to SPIR-V at run time

The shaders cannot be compiled at build time: the game's `.shader` combiner scripts generate the GLSL 1.10 variants
while the game loads (several thousand possible, a few hundred used per city), and the mods' hooks are only known at
start-up. So:

1. **One source for both backends.** The existing translator (`render/glsl.cpp`, GLSL 1.10 → GLSL 3.30 core with the
   fixed-function state as `regs_*` uniforms) stays the only rewrite of the game's shaders. Vulkan gets the same 3.30
   text plus a few mechanical edits made by the Vulkan device, never a second translator:
   * `layout(location = N)` on the vertex inputs, from the same name → slot table the GL path passes to
     `glBindAttribLocation` (`regs_Vertex` 0 … `regs_MultiTexCoord4` 7; the GUI's 0-2);
   * at the end of the vertex `main`: `gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;` (GL clip depth −1..1
     → Vulkan 0..1; the depth values that land in the depth buffer are then the same as GL's window depth, so the
     shadow comparisons and the post effects' depth reads match).
2. **glslang as a library** compiles it to SPIR-V with the *relaxed Vulkan rules* (`setEnvInputVulkanRulesRelaxed`,
   the mode ANGLE-style GL-on-Vulkan layers use): loose `uniform` variables go into one default uniform block,
   samplers get automatic bindings, `in`/`out` locations are matched by name across the two stages
   (`TProgram::mapIO`). Its reflection (`buildReflection`) gives every uniform's offset, type and array stride in that
   block, and every sampler's binding: that is what `uniformLocation(name)` returns on Vulkan.
3. **Cache.** Compiling a few hundred variants costs seconds; SPIR-V is cached by a hash of the final GLSL text in
   `Documents\OpenCiM\cache\spirv\` (a later step; the GL driver has its own cache today).
4. The port's own built-in shaders (2D GUI, the default-shader fallback, post hooks) go the same way, so there is no
   separate offline step and no SPIR-V binaries in the repository.

## 4. Coordinates: no flip inside the frame

GL and Vulkan differ in three conventions. The device keeps every engine-visible value in GL's convention:

* **Clip y and memory rows.** With a positive viewport height, Vulkan maps NDC y = −1 to framebuffer row 0, exactly
  as GL maps it to window row 0 (GL's "bottom"). So a target rendered by Vulkan has the same rows in memory as one
  rendered by GL, texture sampling of targets matches, `gl_FragCoord`, `dFdy` and the engine's GL-style rectangles
  (viewport and scissor counted from the bottom, `backH − y − h`) carry over unchanged. The only flip is at the very
  end: the frame is drawn into an offscreen back-buffer image and copied to the swapchain image upside down
  (`vkCmdBlitImage` with mirrored y). Screenshots read that back buffer as GL's `glReadPixels` reads its own.
* **Winding.** With the same coordinates, Vulkan's area formula has the opposite sign, so GL's counter-clockwise
  front faces are `VK_FRONT_FACE_CLOCKWISE`.
* **Depth range:** the vertex-shader edit above (GL −1..1 → 0..1). `VK_EXT_depth_clip_control` would do the same
  without the edit but is not available everywhere.

## 5. The Vulkan device

* **Loader:** `vulkan-1.dll` is loaded at run time (`LoadLibrary` + `vkGetInstanceProcAddr`, like the GL loader), so
  `regs.exe` starts on machines without Vulkan and falls back (§7). Headers come from Vulkan-Headers, compiled with
  `VK_NO_PROTOTYPES`.
* **Instance and device:** Vulkan 1.3 (dynamic rendering, synchronization2, extended dynamic state are core); a
  discrete GPU first; required features: BC (DXT) textures, sampler anisotropy, `fillModeNonSolid` not needed,
  `independentBlend` not needed. `VK_KHR_push_descriptor` when present (it is on NVIDIA, AMD and Intel desktop
  drivers), else one descriptor pool per frame. Validation layers when `REGS_VK_VALIDATION=1` and installed.
* **Frames:** two frames in flight; per frame a command buffer, a fence, and ring buffers for the default uniform
  blocks and for stream vertex data (GL's `BufferData(nullptr)` orphaning becomes "take new space in the ring").
* **Draw state → pipelines.** Viewport, scissor, cull mode, front face, depth test/write/compare and depth bias are
  dynamic state; blend state, primitive topology, the vertex layout, the program, the attachment formats and the
  sample count form a pipeline key, and pipelines are built on first use and cached (plus a `VkPipelineCache` saved
  next to the SPIR-V cache).
* **Uniforms:** each program keeps a CPU copy of its default block; `setUniform` writes into it at the reflected
  offset (std140, mat3 as three vec4 columns); a draw copies the block into the frame's ring and binds it at a
  dynamic offset. Sampler uniforms (`texture0` = unit 0 …) are remembered as unit numbers, and a draw pushes the
  images bound to those units (GL's 16 units are emulated as a table in the device).
* **Targets:** dynamic rendering (`vkCmdBeginRendering`) on the bound target's attachments. Anything that cannot
  happen inside rendering (a texture upload, a blit, a resolve, a readback) ends the current rendering, records the
  copy with its barriers, and resumes with `LOAD`: this keeps GL's in-order meaning (a draw before an upload sees the
  old texels). Clears inside a scissor use `vkCmdClearAttachments` with the rectangle.
* **Formats:** `GL_RGBA8` with BGRA data → `B8G8R8A8_UNORM` (no sRGB anywhere, like the engine); BGR8 DDS is
  expanded to BGRA on upload; DXT1/3/5 → `BC1_RGBA`/`BC2`/`BC3`; `RGBA16F`, `RGBA32F`, `R8`, `RG8`, `RG16` as is;
  depth `D24_UNORM_S8_UINT` where supported, else `D32_SFLOAT`. Shadow maps: a sampler with `compareEnable` LEQUAL and
  an opaque-white border (shadows.md §4.1). Cube maps: six layers, `CUBE_COMPATIBLE`; Vulkan cubes are always
  seamless, as the port asks GL for.
* **MSAA:** multisampled colour and depth images; `blit` from a multisampled target is a resolve (colour:
  `vkCmdResolveImage` or a resolve attachment; depth: a resolve with `SAMPLE_ZERO`, matching GL's NEAREST blit).
* **Readback:** `readPixels` and `readTexture` wait for the GPU (as GL does). The bloom's per-frame 64×32 luminance read
  (bloom.md) is the one hot path; it can later read the previous frame's copy without waiting.
* **Present:** FIFO when vsync is on, else MAILBOX or IMMEDIATE; window resize and the graphics page's mode changes
  recreate the swapchain (the textures and programs stay, as with GL's context).

## 6. Build

* `port/third_party/vulkan-headers` (Khronos Vulkan-Headers, Apache-2.0) and `port/third_party/glslang` (Khronos
  glslang, BSD-3/Apache-2.0), at pinned release tags, added with `add_subdirectory` (glslang with its tools, tests and
  the optimizer off). No SDK is needed to build or run; the SDK only adds the validation layers for debugging.
* CMake option `REGS_VULKAN` (on by default when `third_party/glslang` is present). Off: the Vulkan device is not
  built and the setting falls back to OpenGL with a log line.
* `regs.exe` does not link `vulkan-1.lib` (§5, run-time loading).

## 7. Choosing the backend and falling back

* `$regsRenderer` (settings): 0 = OpenGL 3.3, 1 = Vulkan. Read once in `window.init()`; the graphics page's choice
  takes effect at the next start (the page says so; switching APIs while running would mean recreating every texture,
  mesh and program). `REGS_RENDERER=gl|vulkan` in the environment overrides the setting for test runs.
* Vulkan start-up failures (no `vulkan-1.dll`, no 1.3 device with BC textures, no surface/swapchain, the built-in
  shaders not compiling) log the reason and open OpenGL instead. The Win32 window is created again for OpenGL: a
  window that has had a Vulkan swapchain should not get a WGL pixel format afterwards.
* A failure while running (device lost) cannot fall back in place: the game logs it, writes `vulkanFailed` into the
  port's state file and quits; the next start uses OpenGL and the graphics page shows why.
* The window title's info string and `window.mode` say which backend runs (`Video: Vulkan 1.4 / NVIDIA GeForce RTX
  3080`), so logs and screenshots of bug reports tell them apart.

## 8. Steps

| # | Step | Status |
|---|---|---|
| 1 | This plan | done |
| 2 | `gpu::Device` with the GL implementation; every GL call outside `gpu_gl.cpp` moved behind it; screenshots of the menu and a Vienna save identical before and after | done (2026-11-17): static pixels identical; only moving vehicles, people, trees and the build string differ between runs |
| 3a | Vulkan device: loader, instance, device, swapchain, offscreen back buffer, present; clear colour on screen; fallback to GL on failure | done (2026-11-17, `render/gpu_vk.cpp`): the Vienna save loads under Vulkan (every texture and mesh uploaded), the 3D view's clear colour reaches the screen and the screenshot; without Vulkan-Headers in the build `REGS_RENDERER=vulkan` falls back to OpenGL with the reason in the log. Textures, buffers, targets, blits, readback, pipelines and descriptors are written but unexercised until shaders compile |
| 3b | glslang in the build; programs, uniforms, textures, buffers: the 2D GUI (main menu readable) | done (2026-11-17): Vulkan-Headers and glslang in `third_party/` (owner's OK; `third_party/README.md` has the licences). The main menu matches OpenGL except its animated spot |
| 3c | 3D: meshes, the default shader and the combiner variants, render targets (the menu's trams, terrain, objects) | done: the Vienna save and the map editor match OpenGL; `regs-tool shaders` with `REGS_RENDERER=vulkan` compiles the same 3 700 of 3 814 variants as OpenGL (the same 114 terrain/water variants fail in both, with flag sets the game never uses) |
| 3d | water, shadows (depth targets, compare samplers), MSAA, bloom and the post effects, particles, night lights, the mods' hooks | checked against OpenGL: shadows, 4× MSAA, bloom with auto-exposure, colour style, lens blur (distance), night lights at 22:00, the mods' shader hooks (weather mod: snow cover with the surface, traffic and footprint maps). Not yet checked side by side: water close up, particles, tilt-shift |
| 3e | parity check on the Vienna save; SPIR-V and pipeline caches | parity: remaining differences are 1-8 levels of texture-filtering noise (the drivers' anisotropic sampling), in about half the pixels. Caches not done (each start compiles the programs again, a few seconds) |
| 4 | Beyond parity: TAA, upscaling, raytracing, frame generation, DLSS (§9) | §9 |

How the Vulkan picture was brought to parity (for whoever touches `vk_shader.cpp`): the game's combiner shaders
declare their varyings section by section, in a different order per stage, so glslang's automatic locations
mismatched them (flat untextured surfaces); varyings now get locations matched by name. Struct-array uniforms need
`EShReflectionStrictArraySuffix` to be found as `regs_LightSource[0].position`. Post effects sample depth textures
with a plain `sampler2D`, so depth textures may be bound to non-shadow samplers.

### Building and testing the Vulkan device today

* The default build has no Vulkan code (`gpu_vk_stub.cpp`) until `third_party/vulkan-headers` exists. To compile the
  device against another copy of the headers without vendoring it, configure a separate tree with
  `-DREGS_VULKAN_HEADERS=<dir containing vulkan/vulkan.h>` (e.g. `build\x64-Release-vk`).
* Run with `REGS_RENDERER=vulkan` (test script: `-Env @{REGS_RENDERER='vulkan'}`); `REGS_VK_VALIDATION=1` turns the
  Khronos validation layer on when the Vulkan SDK is installed.

## 9. Beyond parity: TAA, upscaling, raytracing, frame generation, DLSS

Scope added by the owner on 2026-11-17 ("Also add features like Raytracing, Lossless Scaling etc"). Order: (1) motion
vectors and TAA, (2) our own upscaler and FSR, (3) raytraced shadows and ambient occlusion, (4) frame generation,
(5) DLSS. Each effect is its own graphics option with a raster fallback. Vendor SDKs (FSR, DLSS/Streamline, XeSS)
need the owner's OK before any download; nothing from them is committed unless its licence allows it.

What is backend-neutral and what is not: steps 1, 2 (own upscalers) and 4 (own interpolation) are post passes over
the scene's colour and depth, written against `gpu::Device`, so they run under OpenGL and Vulkan alike. Raytracing,
FSR 2/3 and DLSS need Vulkan (the vendor libraries have no OpenGL path; ray queries need acceleration structures).

### 9.1 Motion vectors, jittered camera, TAA

Status (2026-11-17): done for phase 1 in `engine/temporal.cpp` and `data/regs/shaders/taa.fragment`, setting
`$regsTaa` on the settings page Image quality (`REGS_TAA=0|1` for tests). Checked on the Vienna save under OpenGL and
Vulkan (the two agree as closely as without TAA): edges and texture shimmer smooth, moving vehicles without trails.
Known: the scrolling arrows of route strips are averaged away (texture animation has no motion vectors; overlays
would have to be drawn after the resolve). Phase 2 (object motion vectors) not started.

* **Jitter:** the game view's projection gets a sub-pixel offset each frame (Halton 2,3 over 8 frames, in clip space
  `2·offset / size` added to the projection's third column). Only the game's 3D view; GUI views, render-to-texture
  views and shadow maps stay unjittered.
* **Motion vectors, phase 1 (camera):** reprojection from depth: the world position of each pixel (inverse
  view-projection of this frame, without jitter) projected with last frame's view-projection gives where the pixel
  was. Exact for everything that does not move by itself (terrain, buildings, roads: most of a city).
* **Motion vectors, phase 2 (objects):** vehicles, people and animated parts need their own: each `DrawItem` keeps
  last frame's world matrix (and bone palette), and the combiner programs get a second output with the screen-space
  velocity, added by the translator like the shader hooks. Needed for frame generation and DLSS; TAA copes with
  phase 1 plus neighbourhood clamping.
* **TAA resolve:** an RGBA16F history at output size; per pixel: reproject the history, clamp it to the current
  frame's 3×3 neighbourhood (variance clipping in YCoCg), blend about 10 % of the new frame, a light sharpen; history
  rejected where the reprojected position leaves the screen. Runs before bloom/styles so they see the clean image.
  Option "Antialiasing: TAA" next to the MSAA choices (TAA replaces MSAA when chosen).

### 9.2 Upscaling ("render scale")

Status (2026-11-17): render scale and both own upscalers done (`$regsRenderScale`, `REGS_RENDER_SCALE=<percent>`);
temporal at 67 % is about as sharp as the spatial upscaler, which keeps the 100 % look closest. With bloom on, bloom
runs at the render size. AMD FSR 3.1 upscaling done on Vulkan (`$regsUpscaler` = 1, `REGS_UPSCALER=1`; below).

* Option "Render scale" 50 .. 100 % (a slider in 1 % steps): the 3D view renders into a smaller scene target; the GUI always
  at full size.
* **Own temporal upscaler (TAAU):** the TAA history is at output size and is fed with the jittered low-resolution
  frames (each new sample lands in the output pixel it covers, weighted by distance): the best quality without any
  SDK. Option value "Upscaler: temporal".
* **Own spatial upscaler:** for when temporal is off: edge-directed upsampling (a Lanczos-like kernel steered by the
  local gradient) and contrast-adaptive sharpening. Written here; AMD's FSR 1 shaders (MIT) are an alternative that
  would be copied with their licence.
* **AMD FSR 3.1 upscaler** (done, Vulkan only): option "Upscaler: ReGS / AMD FSR 3.1" on the Image quality page
  (a drop-down list, room for more upscalers; `$regsUpscaler` 0 / 1, `REGS_UPSCALER`). FSR takes the place of TAA and of the own upscalers and also runs at
  100 % (native antialiasing). The code:
  * `third_party/fidelityfx-fsr`: the FSR parts of the FidelityFX SDK v1.1.4 (MIT; v2.x has a different licence,
    see `third_party/README.md`), built into the static library `regs_ffx` with the SDK's own Vulkan backend.
  * Shaders: AMD's build uses prebuilt compiler tools; ours is `src/tools/ffx_shadergen.cpp` (glslang plus a small
    SPIR-V reflection), run by CMake for the 10 upscaler passes. It writes the `*_permutations.h` headers the SDK's
    blob accessors include, with only the permutations our flags need (LDR colour, motion vectors at render size
    without jitter, depth 0 near .. 1 far; Lanczos type and sharpening vary). Like AMD's tool it lists only the
    resources a shader uses (the effect's binding tables know only those).
  * `src/render/fsr_vk.cpp`: context, dispatch, jitter. The backend calls a few Vulkan functions by name; they are
    defined there and forward to the functions the device loaded (nothing links `vulkan-1.lib`). Its device-function
    lookup maps `...KHR` names to the core names (a Vulkan 1.3 device without those extensions returns null). FSR
    3.1 needs three app-owned shared resources (dilated depth and motion vectors, reconstructed previous depth),
    created on a second backend context like the SDK's FSR 3 component does.
  * Device (`gpu.h`): `fsrAvailable`, `fsrUpscale`, `fsrJitter`. Depth is now always `D32_SFLOAT` (the SDK views
    depth as that format), and RGBA16F/32F images get storage usage (FSR writes its output with compute).
  * Engine (`temporal.cpp`): FSR's jitter sequence instead of Halton; `motion.fragment` writes camera motion vectors
    (uv of last frame − uv now, from depth and `prevViewProj · inverse(viewProj)`) at the render size; FSR writes an
    RGBA16F output that is copied to the colour styles' input or the back buffer. Near, far and field of view come from
    the projection matrix; sharpening (RCAS) 0.4 when upscaling, 0.2 at 100 %.
  * Limits: motion vectors carry camera motion only (moving vehicles and people rely on FSR's own disocclusion and
    luma checks; per-object vectors are a later step for both FSR and TAA); no reactive mask for particles and
    transparent effects yet.

### 9.3 Raytraced shadows and ambient occlusion (Vulkan only)

Status (2026-11-17): raytraced sun shadows done, setting `$regsRtShadows` (Image quality page; `REGS_RT_SHADOWS=1`,
and `=2` as a check that keeps the ray programs but switches the rays off). How it is built:
* Device (`gpu_vk.cpp`): `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, buffer device addresses (`REGS_VK_NO_RT`
  turns them off); `createBlas`, `destroyBlas`, `buildScene` on `gpu::Device` (no-ops on OpenGL); the frame's TLAS is
  pushed to programs with a uniform `accelerationStructureEXT regs_Scene`.
* Renderer (`renderer3d.cpp`): `buildRayScene` turns the frame's draw list into TLAS instances of per-mesh BLAS (built
  the first time a mesh is drawn, freed with it): every shadow-casting part except foliage (`kSurfaceFoliage`), blended,
  skinned, water and overlay parts. Alpha-tested materials other than foliage count as opaque (most buildings carry the
  flag); their cut-outs (railings) cast solid shadows.
* Shaders (`glsl.cpp`): the program variant for the port pass bit `kEnvRayShadows` (never passed to the game's
  combiner) replaces `regs_shadow2D` in the fragment stage by the shadow map times a ray query from `regs_WorldPos`
  toward the sun (offset along the derivative normal); GLSL 4.60 for those programs. The shadow-map pass draws only the
  leftovers (foliage, blended, skinned), so trees keep their cut-out shadows from the map.
* Checked on the Vienna save: the same shadows as the map, crisper at contact and outside the map's range, no acne.
  Not yet: soft shadows (jittered sun disc with TAA), alpha-tested foliage in the rays.

Raytraced ambient occlusion done too, setting `$regsRtAo` (`REGS_RT_AO=1`, `=2` shows the occlusion alone): it runs in
the temporal chain (`engine/temporal.cpp`, which is then active even at 100 % without TAA) on the drawn frame's depth:
position and normal per pixel, 4 cosine-weighted rays of 2.5 m into the same frame scene (`ao.fragment`, turned per
pixel and frame), a 5×5 depth-aware blur (`aoblur.fragment`), multiplied into the colour before TAA. Rays start off the
surface and ignore hits within 10 cm: paving lies a few centimetres over the terrain and otherwise darkened every
square. It multiplies the whole colour, not only the ambient term (a post pass cannot separate them).

* Device features: `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `bufferDeviceAddress`; vertex and index
  buffers get the acceleration-structure input usage. The RTX 3080 has all of them; other GPUs fall back to raster.
* **Acceleration structures:** one BLAS per uploaded `GpuMesh` (static: built once at upload; skinned people and
  animated vehicle parts: a BLAS per frame or a static pose); a TLAS rebuilt each frame from the same `DrawItem` list
  `Renderer3D::draw` gets, culled to the shadow light's reach. Alpha-tested foliage and fences need the texture's
  alpha in the ray query's candidate loop (material and uv data reached through buffer device addresses).
* **Shadows:** the combiner's `shadow_output` component reads the shadow map today; with the option on, a ray query
  toward the sun (jittered over the sun disc for soft edges, cleaned up by TAA) replaces the lookup. No resolution
  limit, no acne, no trapezoid warp. Option "Shadows: raytraced" next to the existing shadow quality.
* **Ambient occlusion:** a full-screen pass: world position and normal from depth, a few short rays (2-4 m) per pixel,
  a depth-aware blur, multiplied into the ambient term (the scene has no AO today). Option "Ambient occlusion: off /
  raytraced".
* Later: reflections on water and glass (ray queries in the water shader and for windows), then global illumination.

### 9.4 Frame generation

* **Own interpolation:** with full motion vectors (9.1 phase 2) a generated frame between two rendered ones: each
  pixel warped halfway along its motion, holes filled from the frame that sees them (depth decides), the GUI drawn
  fresh on top of every presented frame (it is cheap and must not smear). Doubles the presented frame rate at the
  cost of a frame of latency. Option "Frame generation: off / on".
* **AMD FSR 3 frame generation** (FidelityFX SDK, MIT, Vulkan): an alternative with its own optical flow; owner's OK
  needed. NVIDIA DLSS Frame Generation needs an RTX 40 card, so it is not an option on the RTX 3080.

### 9.5 DLSS

* NVIDIA DLSS Super Resolution through the Streamline SDK (or the NGX DLSS SDK). Same inputs as FSR 2 (9.2). The
  runtime DLL (`nvngx_dlss.dll`) may be shipped under NVIDIA's DLSS licence terms; the SDK download and the licence need
  the owner's OK first. Vulkan only, RTX cards only; other cards keep the other upscalers.

### 9.6 HDR output (Vulkan only)

Owner's request 2026-11-17, placed before frame generation (FSR 3's frame generation replaces the presentation, so it
has to be built for the HDR swapchain from the start). Settings on the Image quality page: `$regsHdr` (0 / 1),
`$regsHdrPaperWhite` (slider, 80 .. 400 nits, default 200), `$regsHdrPeak` (slider, 400 .. 4000 nits, default 1000); test switches
`REGS_HDR`, `REGS_HDR_PAPER`, `REGS_HDR_PEAK`. OpenGL has no dependable HDR path on Windows, so it stays SDR there.

* **Swapchain** (`gpu_vk.cpp`): with `VK_EXT_swapchain_colorspace` the surface lists `A2B10G10R10` +
  `HDR10_ST2084` while Windows HDR is on for that display. HDR on and offered: that swapchain, and the back buffer
  becomes RGBA16F (blitted onto the 10-bit image at present, the blit converts). `VK_EXT_hdr_metadata`, when present,
  tells the display the content's peak. HDR off in Windows, or switched off in the game: the 8-bit SDR swapchain as
  before; the log says which (`vulkan: HDR10 output available / not offered`).
* **Back-buffer values**: gamma 2.2, relative to paper white (1.0 = paper white, above = brighter). The GUI draws as
  always (its white is paper white).
* **Highlights** (`hdrexpand.fragment`, after each on-screen 3D view, before its GUI): the game is drawn for SDR, so
  the top of its range is lifted. In linear light the brightest channel follows the original up to a knee (60 %),
  then a curve that starts with the same slope and puts SDR white at a third of the way from paper white to the peak;
  values already above 1.0 (bloom, light glows and night lights in the float targets) go on further. Hue is kept
  (all channels scaled together).
* **Encode** (`hdrencode.fragment`, the last pass before present, after any screenshot): BT.709 → BT.2020, × paper
  white, a soft roll-off above 75 % of the peak, PQ (ST 2084).
* Screenshots and `readPixels` stay SDR (the float back buffer clamped to 0..1, before the encode).
* Still to do: FSR's HDR mode (the upscaler sees the SDR scene now, which is right while the lift comes after it);
  per-light HDR intensities (lamps, windows, the sun's specular) instead of lifting the finished image.

### 9.7 Raytraced reflections (Vulkan only)

Owner's order 2026-11-17: water, puddles and glass after FSR. Status: water and glass done (setting
`$regsRtReflections`, Image quality page; `REGS_RT_REFLECTIONS=0|1`); wet streets and puddles not yet.

* **Where:** the game reflects with an environment cube on water (`metro/shaders/water/water.fragment`) and on every
  material with the reflection flag (`shaders/default/reflection.fragment`, masked by the specular map: window glass),
  both as `textureCube(cube, reflect(view, normal) * vieworient)`, a world-space direction. With the option on, the
  pass gets the port bit `kEnvRayReflections`; variants whose fragment source reads a cube are built with
  `kRayReflections` (render/glsl.h), and the translator turns those `textureCube` calls into `regs_reflectCube`.
* **The ray** starts at the surface (`regs_WorldPos`) along a calmer direction than the shader's: the reflection about
  the flat surface (from screen derivatives and the camera, `regs_EyePos`), bent 25 % toward the rippled one, so the
  water's strong normal maps do not scatter single rays into speckles. A miss reads the cube as before.
* **A hit** is shaded from the device's **hit table** (one 32-byte record per bottom-level geometry: vertex and index
  buffer addresses, base texture handle, diffuse colour; the instance custom index points at the mesh's first record,
  the geometry index picks the part): the triangle's indices and vertices are read through buffer references
  (`GL_EXT_buffer_reference`), the UV interpolated with the barycentrics, the texture sampled from the **bindless
  array** (set 1: every 2D texture at its handle, update-after-bind, partially bound; a fixed linear sampler; the mip
  from the hit distance), lit by the sun with one shadow ray and the scene ambient, and fogged by the hit distance.
* **Device:** descriptor indexing features (Vulkan 1.2) when ray queries are on; `REGS_VK_NO_BINDLESS` switches it off.
  Programs that read `regs_Images` get a two-set pipeline layout; the device writes the hit table's address into
  `regs_HitTable` before each draw and binds the bindless set.
* **Check:** `REGS_RT_REFL_DEBUG=1` (a constant colour instead: the programs draw), `=2` (green where a ray hits),
  `=3` (the hit record's colour).
* **Seen on Vienna:** bridges and piers mirror in the Danube; the far banks show at grazing angles. The base game's old
  facades barely use the reflection flag, so glass shows where modern buildings carry it.
* **Open:** foliage is not in the ray scene (alpha-tested cut-outs need an any-hit test; the same holds for the shadow
  rays), lit windows and lamps at night are not in the hit shading yet (the night-light masks), wet streets and
  puddles (they need the weather mod's wetness on road and pavement surfaces), performance not measured yet.

## 10. Benchmarks

Measured 2026-11-17 on the owner's machine (RTX 3080, driver 621.01), Vienna sandbox save (`run\documents-bench`: 4×
MSAA, shadows "High", anisotropy 8×, bloom off, vsync and frame limit off), the save's own camera with the game clock
running, 1 500 frames after an 1 800-frame warm-up. Tool: `toolsench.ps1 -Docs <sandbox> -Sizes 1600x900,2560x1440`
(engine switch `REGS_BENCH=<first>:<frames>`). Frame = present to present (simulation, scripts, rendering, waiting);
render CPU = the render call up to the present; GPU = timestamps at the frame's start and end (OpenGL timer queries,
Vulkan timestamp queries), a few frames old.

| Configuration | Window | FPS | Median frame ms | p99 ms | Render CPU ms | GPU ms |
|---|---|---|---|---|---|---|
| OpenGL | 1600×900 | 122.1 | 7.69 | 17.12 | 3.88 | 1.60 |
| Vulkan | 1600×900 | 127.5 | 7.47 | 16.26 | 3.87 | 1.48 |
| OpenGL + TAA | 1600×900 | 123.9 | 7.69 | 16.47 | 3.85 | 2.43 |
| Vulkan + TAA | 1600×900 | 125.2 | 7.58 | 16.71 | 3.96 | 2.21 |
| OpenGL + TAA, 67 % | 1600×900 | 126.7 | 7.50 | 16.27 | 3.72 | 2.27 |
| Vulkan + TAA, 67 % | 1600×900 | 119.8 | 7.91 | 16.89 | 4.10 | 1.76 |
| Vulkan + RT shadows | 1600×900 | 135.9 | 6.92 | 15.83 | 3.33 | 2.50 |
| Vulkan + RT shadows + RT AO + TAA | 1600×900 | 136.6 | 6.94 | 15.82 | 3.37 | 3.07 |
| OpenGL | 2560×1440 | 125.7 | 7.57 | 16.25 | 3.77 | 2.31 |
| Vulkan | 2560×1440 | 125.1 | 7.51 | 17.32 | 3.91 | 2.26 |
| OpenGL + TAA | 2560×1440 | 125.7 | 7.55 | 16.24 | 3.78 | 2.48 |
| Vulkan + TAA | 2560×1440 | 123.2 | 7.54 | 17.21 | 3.97 | 2.44 |
| OpenGL + TAA, 67 % | 2560×1440 | 124.9 | 7.56 | 16.40 | 3.78 | 2.42 |
| Vulkan + TAA, 67 % | 2560×1440 | 124.3 | 7.59 | 16.65 | 3.96 | 2.43 |
| Vulkan + RT shadows | 2560×1440 | 135.7 | 6.98 | 15.98 | 3.42 | 4.37 |
| Vulkan + RT shadows + RT AO + TAA | 2560×1440 | 127.8 | 7.35 | 16.51 | 3.60 | 6.42 |

What it says:
* **The game is CPU-bound on this machine.** Every configuration runs at about 125 fps: a frame is ~7.5 ms, of which
  the render call takes ~3.8 ms of CPU and the simulation and scripts the rest, while the GPU works 1.5-2.5 ms. More
  frames per second have to come from the CPU side (simulation, scripts, the render call's work per draw), not from the
  graphics API. Differences of ±3 fps between single runs are noise (a first round with two runs each agreed within that;
  one OpenGL + TAA run at 94-106 fps there did not repeat).
* **OpenGL and Vulkan are even.** Same frame rate, same GPU time (2.31 vs 2.26 ms at 1440p); Vulkan's render CPU is
  0.1-0.2 ms higher, the cost of emulating GL's per-call state machine (§2). A Vulkan path designed around pipelines,
  descriptor sets and multithreaded command recording would be where Vulkan pulls ahead on the CPU.
* **TAA costs ~0.2 ms of GPU** at 1440p, nothing visible in the frame rate.
* **Render scale does not save GPU time in this scene**: the GPU's work here is not pixel-bound (shadow map, many small
  draws). It pays off on weaker GPUs or at 4K and above, and it is the base for FSR.
* **Raytraced shadows make the game faster** (+8 % fps, 135.9 vs 125): the shadow map then draws only foliage and
  skinned casters, so the render CPU drops by ~0.5 ms, while the rays add ~1-2 ms of GPU time the GPU has to spare.
* **Raytraced AO adds ~2 ms of GPU at 1440p** (with shadows and TAA 6.4 ms in all), still under the CPU's frame time.
* **Loading:** the first view is drawn ~0.4 s later under Vulkan (973 vs 563 ms), glslang compiling the programs; a
  SPIR-V cache (§8 step 3e) would remove that.
