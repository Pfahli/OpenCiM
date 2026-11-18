# Performance audit (2026-11-18)

Where OpenCiM spends its time, and what to change, ranked by expected gain. Nothing here is fixed yet.

## How it was measured

- Profiler: `REGS_PROFILE=<first frame>:<frames>:<report file>` (`src/tools/profiler_win32.cpp`). It samples the main
  thread's stack about 1000 times a second and reports own time, inclusive time and a call tree. It needs a build with
  symbols: Release with `/Zi` and the linker's `/DEBUG` (the audit used a separate tree `build\x64-Release-prof`).
- Runs: 1920×1080, vsync and frame limit off, 1500 frames measured after 1500 frames of loading, the camera at the
  save's start view, the weather mod on (December: snow), RTX 3080.

| Save, engine | fps | Frame ms (avg) | Median / p99 / max ms | Render CPU ms | GPU ms |
|---|---|---|---|---|---|
| Vienna, Vulkan | 82.7 | 12.1 | 9.8 / 31.0 / 503 | 6.6 | 3.7 |
| Vienna, OpenGL | 86.3 | 11.6 | 9.9 / 26.3 / 42 | 6.3 | 4.6 |
| Berlin, Vulkan | 44.1 | 22.7 | 19.4 / 45.9 / 357 | 13.6 | 5.3 |

The game is CPU bound: the GPU is idle most of the frame. Shares below are of the whole Berlin frame on Vulkan
(1 % ≈ 0.23 ms) unless noted. Two runs meant to measure speed 10 and a zoomed-out view did not take their scripted
clicks (same numbers as the start view), so speed 10 and far views are still unmeasured.

## A. Frame time (measured)

1. **Stop bubbles look up an environment variable on every call: ~12 %.** `placeTracer` → `cameraView`
   (`engine/scene3d.cpp:170`) calls `std::getenv("REGS_DEBUG_TOPDOWN")` for every bubble every frame; MSVC's getenv
   locks and scans the whole environment (`strchr` 8.7 % own time). About 140 `getenv` calls in the code are not
   cached; several run per frame (`tickSimulation` ~6, `collectGrid`, `scene3d.cpp` 563/623/676, `temporal.cpp`,
   `hdr.cpp`). Change: read each once (`static const`), compute the camera frame once per viewport per frame in
   `organizeTracers` instead of per bubble. Small.
2. **Pose fields written into ~30,000 script objects every frame: 11–14 %** (`setPoseFields`, `engine/sim.cpp:1325`,
   from `syncCharacterFields` and `syncVehicleFields`). Each write `o->fieldValues[u"position"]` builds a heap
   `std::u16string` (keys over 7 characters) and hashes it, three keys per object. Root cause: a character controller
   is created for every pedestrian, because the spawn paths call `characterController(s, i)` before `fireCitizen`
   checks whether anyone listens (`sim_citizens.cpp:1610`, 1745, 1773, 1884, 2051; `fireCitizen` at 1203 only fires if
   the citizen has a controller). The original creates controllers lazily (`docs/ui/selection.md` §5.1). Changes: (a)
   create a character's controller only when something asks for it (the citizen has one, a hover hit, a camera target);
   (b) for the rest, cache the three `Value*` slots per object and write only when the pose changed. Also cuts the
   grid's child list that `VM::tickChildren` copies every frame. Small; behaviour unchanged (no simulation code reads
   these fields back).
3. **Weather mod: the trodden snow map is re-uploaded in large blocks: ~7 %.** `traffic_maps.cpp:511-527` rewrites
   every trodden cell and grows the dirty box every 0.2 s whether or not the byte changed; `uploadTop`
   (`engine/surface_map.cpp:124`) packs the whole box into a fresh vector and uploads it (on Vulkan with an end of
   rendering and a full barrier). Change: mark a cell dirty only when its byte changes, track 64×64 tiles, keep the
   pack buffer. Small.
4. **Vulkan draw submission: ~14 % in `VkDeviceImpl::draw`.**
   - Up to five heap vectors per draw (`gpu_vk.cpp:2454-2456`, 2476-2477): `operator new`/`free` ~3.7 %. Use stack
     arrays or member scratch vectors. Small.
   - Every draw re-records the pipeline bind and nine dynamic states, re-binds vertex and index buffers and pushes the
     UBO and every sampler (`gpu_vk.cpp:2430-2527`), and rebuilds and FNV-hashes the 32-byte `PipeKey` (2311-2324).
     Keep the last recorded state and skip unchanged ones; push only changed descriptors. Small to medium, 2–4 %.
5. **Program switches redo the whole per-pass setup: ~3.5 %.** The opaque queue is not sorted
   (`render/renderer3d.cpp:1007-1030`), so almost every draw switches program and runs `setupProgram` (~60 uniforms, the
   lights) and `hookState` (a 4×4 inverse, map lookups by string per mod uniform), and recomputes `mv`/`mvp`/normal
   matrix. Change: stable-sort opaque draws by (program, mesh) and set each program up once per view. `locs_` is a
   `std::map<uint32_t, ProgramLocs>` looked up 2–3 times per draw (`renderer3d.h:331`); index it by handle. Small.
6. **The shadow pass is not culled for the light: up to most of its draw CPU.** `scene3d.cpp:635`
   (`SceneView lv = v; // culling, LOD and sorting stay with the main camera`) draws the whole main-camera list (up to
   880 m) into the shadow map. Sphere-test parts against the light's view-projection or the fitted shadow distance.
   Small.
7. **Hover picking poses every vehicle and character every frame: 3–4.5 %.** `intersectGrid`
   (`interaction.cpp:218-251`, `sim.cpp:2803-2820`) computes each unit's pose before testing it against the ray's cell
   rectangle. The original re-picks every tick even with a still mouse (selection.md §3.1), so keep the rate but
   pre-cull on the node position before computing poses, and reuse the last hit when mouse, camera, map version and
   simulation step are unchanged (exact while paused). Small.
8. **Per-frame class lookups by name: 2.6 %.** `collect` in `scene3d.cpp:295/324` calls `vm.findClass("gsSky")` and
   `findClass("gsSun")` for every scene node (`std::map<std::string>` with a temporary string). Look them up once per
   frame. Small.
9. **Lane colours repainted with a full-map sort whenever an aircraft adds a graph node: up to 2.7 %, spikes.**
   `traffic_maps.cpp:340` repaints when `graph.nodes.size()` changes; `newInlineNode` (`sim_air.cpp`) grows the node
   list in normal play although those nodes are never lanes. Trigger on a graph-edit version instead. Small.
10. **Character animation bookkeeping for every walker: ~2 %.** `animateCharacter` (`skinning.cpp:152`) looks up the
    skeleton by a heap key and the animation slots in a `std::map<std::string>` each step. Cache the pointers per
    model. Small.
11. **Script member access hashes several times per access** (`VM::getMember`, `vm.cpp:479-517`; `findField` per
    class level, then the getter hashes `fieldValues` again; natives build `std::u16string` temporaries from literals).
    Heterogeneous lookup (`u16string_view`) and a flattened per-class name cache with an epoch would remove most of it
    without changing semantics. Medium. (Script execution itself is only 2.5 % of the frame.)

**Vulkan hitches (340–500 ms frames; OpenGL's worst is 42 ms):** shader variants are compiled with glslang the first
time they are drawn (`scene3d.cpp:241-276` → `vk_shader.cpp:146-183`, with eight `std::regex` objects per program),
and pipelines are created at draw time with no `VkPipelineCache` (`gpu_vk.cpp:2324` → 2411,
`vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, …)`). The pipeline key includes the target formats and blend state,
so the shadow pass, each scene target and toggling bloom, TAA, MSAA or night each create new pipelines. Change: a
SPIR-V disk cache with the reflection data, a `VkPipelineCache` saved to disk, plain string scans instead of regexes,
and pre-creating the likely pipelines after a program is built. Medium. (`docs/renderer/vulkan.md` §5 describes a
pipeline cache and a dynamic-offset UBO that the code does not have.)

**Bloom (when on) reads a value back every frame and waits for the GPU** (`effects.cpp:711` `readRed` →
`gpu_vk.cpp:1988` `submitAndWait`; GL `glGetTexImage`): the CPU idles for the GPU's shadow and scene passes and loses
the overlap. The value is for the next frame anyway: read it from a per-frame-slot buffer whose fence has passed (GL:
a PBO and a fence). Small to medium. Not in the profiles above (bloom was off).

## B. Loading and saving (from the code and an existing load log)

Vienna map load log: `grid items … in 4488 ms (materials 3427 ms, night masks 127 ms)`, `night lights: 129 window masks
in 685 ms`, `first view drawn ahead in 1117 ms (88 shader programs)`.

1. **Every texture is read and inflated twice at load, on the main thread.** `getTexture`
   (`engine/natives_ui.cpp:58`) inflates the whole DDS only to read its size; `loadTextureImage`
   (`display.cpp:263/301`) inflates it again for the upload. This is most of "materials 3427 ms". Change: inflate only
   the header in `getTexture` (or keep the bytes), and decode a map's textures on worker threads as the night masks
   already do. Small (header) to medium (workers); expected 3.4 s → under 1 s.
2. **The zlib decoder is slow.** `core/inflate.cpp:58-71` decodes Huffman codes one bit per loop iteration, copies
   matches byte by byte (`:95`) and computes adler32 with two `%` per byte (`:183`); `vfs/archive.cpp` reopens the
   archive file on every read. Every byte loaded goes through it (412 MB packed textures, 300 MB objects, 400 MB
   grids). Change: a table-driven decoder or libdeflate, one handle or memory map per archive. Small to medium; likely
   3–8× faster inflation.
3. **Shaders: no disk cache, compiles serialized.** Vulkan as above; OpenGL queries compile and link status right after
   each call (`gpu_gl.cpp:341/435`), so the driver cannot compile in parallel (a cold driver cache meant about 30 s of
   black screen, `terrain.cpp:1614`). Change: issue all programs before querying status
   (`GL_KHR_parallel_shader_compile`), optionally program binaries. Medium.
4. **Shader templates are re-read, re-patched and recompiled as scripts for every variant**
   (`engine/shaders.cpp:272-313`: read from the VFS, `portShaderPatch`, written to a memory file, `vm.load` again,
   although only six prepended flag lines differ). Cache the compiled template per component and set the flags as
   variables. Small to medium.
5. **Night-light masks read their source textures on the main thread** (`night_lights.cpp:912-913`; only the mask
   job runs on the eight workers): move the reads into the job. Small; 685 ms → about a third.
6. **Each template's .object is inflated twice** (`scene3d.cpp:401` and again in `sim_graph.cpp:100-102` only for its
   locator chunks): parse the locators once. Small.
7. **Saving and autosave compress on the main thread, then rescan the whole file system** (`natives_core.cpp:428-440`:
   `buildPackage` deflates every file serially, then `vfs().rescan()` reopens every archive and save). Change: build
   and write the package on a worker and mount only the new package. Medium. **Possible bug:** with asynchronous writes
   on, `rescan` can run while the package is still being written and skip it as "size mismatch".
8. **No script compile cache** (`script/vm.cpp:202`, "every load compiles again"): the UI and addons at start, ~900
   companion scripts per map load, every shader template. An in-memory cache by path and source hash first. Medium.
9. **Memory:** `objectCache()` and `textureCache()` are never cleared, so every map loaded in a session keeps its
   templates' CPU meshes and textures; division mesh building copies the vertex data three times without `reserve`
   (`grid_items.cpp:479-608` → `renderer3d.cpp:256-269`). Small.

## C. Map edits and the rebuild after an import

1. **Merging the touched rectangles is quadratic** (`sim.cpp:2427-2437`: after every merge the pair scan restarts at
   the beginning and `rects.erase` moves the rest). After a 185,000-object import this is the prime suspect for the
   ~130 s rebuild. Change: mark touched blocks in a bitmap and take connected components (linear). Small. Then
   `restampWalk` (`sim_citizens.cpp:2512-2523`) loops over all items per rectangle: use the existing per-division item
   lists. Medium.
2. **A tool preview moving with the mouse bumps `grid->itemsVersion`** (`grid_edit.cpp:297`), which makes picking,
   particles, sounds, built-up areas and the lamp map each walk all items again. Give preview-only batches their own
   version and apply real edits incrementally. Medium; roughly 10–40 ms per change on large maps.

## Suggested order

1. Quick wins in one pass, each small and measurable with the profiler: A1, A2, A3, A4 (allocations), A5, A6, A8,
   A9, B1 (header), B5, B6, C1. Together these address roughly 45–55 % of the measured Berlin frame and several seconds
   of loading; expect Berlin well above 60 fps.
2. Vulkan caches (SPIR-V, pipeline cache) for the hitches and start-up.
3. The faster inflate and parallel texture decode for loading.
4. Then the VM lookup work (A11), the save path (B7) and the edit path (C2).

Still to measure: speed 10 (citizen `process` work scales with game speed), a zoomed-out view, and a profile of a map
load and of the rebuild after an import.

## Results of the first round (2026-11-18)

Done: A1 (cached getenv, core/env.h), A2, A3, A4 (allocations only), A5 (once-per-view program setup and the
indexed program table; the opaque queue is still unsorted: sorting would change which of two coplanar parts wins),
A6 (caster culling: correct, but it culls almost nothing in the tested views), A8, A9, B1 (header read and worker
decode), B2 (table-driven inflate, 133 to 482 MB/s, output identical on all 22,848 archive entries; the install
archives keep a file handle), B5, B6, C1. New test switches: REGS_FIXED_DT, REGS_DEBUG_PROGRAM_SETUP,
REGS_DEBUG_NO_CASTER_CULL.

| Save, engine | fps before | fps after | Render CPU ms before / after |
|---|---|---|---|
| Berlin, Vulkan | 44.1 | 81.5 | 13.6 / 7.1 |
| Berlin, OpenGL | (not measured) | 88.7 | (not measured) / 6.3 |
| Vienna, Vulkan | 82.7 | 119.5 | 6.6 / 4.5 |

Same method as above; the after runs had another session compiling in the background, so they are if anything low.
Loading: Vienna grid items 4.4 s to 0.7 s, Berlin 3.0 s to 0.35 s; night masks about 2.6 times faster.

Next by the new profile (Berlin, Vulkan): the stop bubbles still spend ~13 % in script member lookups
(`placeTracer` reading about 15 fields by name per bubble per frame; A11 and a per-frame camera cache), the
Vulkan draw path (~16 %, the state and descriptor skipping of A4), `stepCitizens` (~14 %), and the Vulkan hitches
(still up to 720 ms; pipeline and SPIR-V caches).

## Results of the second round (2026-11-18)

Done: A7 (hover pick pre-cull in `forEachSimVehicle` / `forEachSimCharacter`; the last-hit reuse was left out, it
only helps while paused), A11 in part (`U16Map`: heterogeneous lookup on `fieldValues`, `varIndex`, the class
maps, `functionIndex` and the globals; the generated field getters keep their name's hash; no flattened per-class
cache), and the per-pass camera cache for the tracers (`TracerViews`, shared by organized and unorganized tracers,
cleared when a viewport inside the pass eases its camera). New test switch: REGS_DEBUG_NO_PICK_CULL (=check).

| Save, view, engine | fps before | fps after |
|---|---|---|
| Berlin, start view, Vulkan | 86.3 / 86.3 / 86.3 | 104.9 / 104.9 / 103.4 |
| Berlin, start view, OpenGL | 86.1 / 88.2 | 100.5 / 98.7 |
| Berlin, zoomed out over the lines (~40 bubbles), Vulkan | 69.7 / 69.2 | 81.3 / 80.8 |

REGS_BENCH=300:1400, REGS_FIXED_DT=0.016, before and after runs alternated on a quiet machine. Profile of the start
view after: `placeTracer` 5.0 % (was ~15 %; what is left are the bubbles' own field reads), `intersectGrid` 2.7 %
(was 3-5 %; now mostly the per-mover cull test itself). Screenshots (Vulkan, OpenGL) and a 1,034-position pick sweep
are identical to the build before, apart from the few snow pixels that also differ between two runs of one build.
