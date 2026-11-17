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
| 3d | water, shadows (depth targets, compare samplers), MSAA, bloom and the post effects, particles, night lights, the mods' hooks | checked against OpenGL: shadows, 4× MSAA, bloom with auto-exposure, colour style, lens blur (distance), night lights at 22:00. Not yet checked side by side: water close up, particles, tilt-shift, the mods' shader hooks (weather mod) |
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
runs at the render size. FSR not started (needs the owner's OK for the FidelityFX SDK download).

* Option "Render scale" 100 / 85 / 77 / 67 / 50 %: the 3D view renders into a smaller scene target; the GUI always
  at full size.
* **Own temporal upscaler (TAAU):** the TAA history is at output size and is fed with the jittered low-resolution
  frames (each new sample lands in the output pixel it covers, weighted by distance): the best quality without any
  SDK. Option value "Upscaler: temporal".
* **Own spatial upscaler:** for when temporal is off: edge-directed upsampling (a Lanczos-like kernel steered by the
  local gradient) and contrast-adaptive sharpening. Written here; AMD's FSR 1 shaders (MIT) are an alternative that
  would be copied with their licence.
* **AMD FSR 2/3 upscaler** (FidelityFX SDK, MIT, has a Vulkan backend): takes the same inputs (colour, depth, motion
  vectors, jitter, exposure). Needs the owner's OK for the download. Vulkan only.

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
  Not yet: soft shadows (jittered sun disc with TAA), alpha-tested foliage in the rays, ambient occlusion.

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
