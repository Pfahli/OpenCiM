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

## Citizens and speed 10 (2026-11-18)

Speed 10 is set in a test with the game's speed key: `--at <frame>:key:51` (key 3, `controls.keys`
`$timeSpeed3`, the minimap's selectSpeed(3)); `REGS_DEBUG_GROUPS=1` then logs the 'game' thread group at speed 10.
Runs: Berlin, Vulkan, 1920×1080, vsync and frame limit off, `REGS_FIXED_DT=0.016` (every frame advances the same game
time, so both builds simulate the same), the key at frames 100-250, 1400 frames measured from frame 300.

Before (profiled build): at speed 10 the grid tick was 50 % of a 29.2 ms frame: `stepCitizens` 24.6 % (`walkPathIn`
11.5 % from stepTravel, stepWalk and carArrived; `LaneGraph::search` 8 %, half of it stepDrive repeating the search
startTrip had just priced; `transitRoute` 1.9 %; `stepCharacter` 2 %) and `simTrafficMapsTick` 17 %: the mods'
trodden map, an unordered_map of every 1 m cell a visible pedestrian walked in the last minutes (175,000 cells after
five game minutes; it grows with game time at any speed), decayed node by node every grid step. At speed 1
`stepCitizens` was 13.5 % of 12.3 ms (`stepCharacter` 6.7 %: `animateCharacter` 3.2 % with `modelSkeleton` building
a heap key per call).

Done (each its own commit): character animation by slot ids with a per-skeleton slot table; the pedestrian search's
scratch kept between searches with stamps (no clearing of up to 800² q-cells per search), the heights it read reused;
the trodden map as flat arrays; the car start reusing the path its price came from (valid while `laneVersion` is
unchanged); `transitRoute` without heap keys and with each stop and line read once per search; `LaneGraph::search` on
a packed copy of the nodes (made again per `laneVersion`). Results identical: the new switch `REGS_DEBUG_SIM_DIGEST=10`
logs the random stream and hashes of the citizens, walkers with their animation, movers, line vehicles and stops every
10 citizen steps; the build before and after print the same 171 digests at speed 1 and at speed 10 (and two runs of
one build match, so the test is deterministic). Screenshots of two runs of the same build differ at speed 1 (snow
particles), so they are no proof here.

| Berlin, Vulkan | fps before | fps after | Frame ms before / after (profiled) | Grid tick share before / after |
|---|---|---|---|---|
| speed 1 | 78.2, 85.1 | 89.3, 90.4 | 12.3 / 11.5 | 28 % / 25 % |
| speed 10 | 43.0, 42.4 | 59.7, 58.6 | 29.2 / 17.9 | 50 % / 35 % |

(Two runs each, alternating builds; the machine had other sessions running, so single numbers move by about 10 %.)
After: `stepCitizens` 1.1 ms at speed 1 (was 1.7; `animateCharacter` 1.2 % of the frame, was 3.2 %), 3.7 ms at
speed 10 (was 7.2), of it the pedestrian search 1.8 ms and the trip start's lane
search 0.9 ms; the trodden map 0.6 ms (was 5). Left: the pedestrian A* itself (its expansions and tie order decide the
paths; a faster search must find the same ones), the one lane search per car trip, the render side.

## Saving, shader templates and OpenGL compiles (2026-11-18): B3, B4, B7

B7: `writePackage` takes a snapshot of the files (memory files share their buffer), builds and writes the package on one
background thread (`<file>.tmp`, then renamed over the old file) and shows the package's entries from the snapshot at
once; `Engine::frame` mounts only the finished package, no rescan; `rescan()` and the exit wait for writes. The
suspected bug was real by the code: with asynchronous writes (the autosaves) `write()` only started the thread, so the
rescan right after it could open a half-written file ("size mismatch", the save missing from the lists) or the old
file whose directory no longer matched the data being written over it. It did not show in 17 test autosaves here (the
rescan takes ~20 ms, the write usually a few ms; one background write took 88 ms). Main thread of a Berlin save
(`REGS_DEBUG_SAVE_AT`): 507 to 82 ms (Vulkan), 449 to 74 ms (OpenGL); the autosave tick 460 ms to under 250 ms. The
written save is the old build's byte for byte (only the wall-clock `$saveTime` differs) and reloads with the same counts.

B4: each shader template component is patched, converted and compiled once; the variants run that code with the flags
set as script variables (`VM::compileSource` / `loadCompiled`). All 3,814 variants of `regs-tool shaders --dump` are
byte for byte the same; expansion 2593 to 320 ms.

B3: OpenGL queues the programs a draw list misses (`gpu::Device::queueProgram`, `assignPrograms`) before it asks for
any status, with `GL_KHR_parallel_shader_compile` on; linked programs are kept as driver binaries in
`<user folder>\cache\gl\<driver>\`. Berlin, OpenGL, RTX 3080, first view (67 programs) with a private empty driver
cache (`__GL_SHADER_DISK_CACHE_PATH`): 34.5 s before, 40.8 s one by one, 13.1 s batched, 3.7 s with the program
binaries; with the driver's cache warm 0.70 s before, 0.59 s now, 0.55 s with the binaries. Vulkan first view: 1.6-2.3 s
before, 1.5-2.0 s now (only the template part applies; its pipelines are the Vulkan cache item above).

## Vulkan caches and draw state (2026-11-18)

SPIR-V disk cache with the reflection data (`<documents>\OpenCiM\cache\spirv`), a `VkPipelineCache` saved to
`cache\pipelines.bin` (checked against the GPU), plain string scans instead of `std::regex` in `vk_shader.cpp` (same
output on all programs), `draw()` recording only changed state and descriptors (`REGS_VK_RECORD_ALL=1` for A/B), and
the loading-screen pre-draw also drawing the post-hook target, which had made 25 pipelines in one frame during play.
`REGS_VK_CACHE=0` turns the caches off, `REGS_VK_LOG_PIPELINES=1` logs every pipeline made. Details in
`docs/renderer/vulkan.md`. Berlin first view with both caches 0.56-0.64 s (was 1.6-1.8 s); no frame over 250 ms
caused by shaders or pipelines in the measured runs; render CPU about 0.7 ms (10 %) lower from the state skipping.

## Where it stands after both rounds

| Berlin, 1920x1080, start view | Start of the audit | After both rounds |
|---|---|---|
| Vulkan fps | 44.1 | 109.5 |
| Vulkan worst frame | 357 ms | 58 ms |
| Vulkan render CPU | 13.6 ms | 4.8 ms |
| Speed 10, Vulkan fps | about 43 | 65.9 |
| Grid items at load (Vienna) | 4.4 s | 0.7 s |
| Save, main-thread stall | about 500 ms | about 80 ms |

(After-numbers from one merged run each with `REGS_FIXED_DT=0.016`; other sessions were running.) Left: the
pedestrian A* (it must find exactly the same paths), bloom's per-frame readback wait, Vulkan programs still built one
by one, the editor's tool-preview rescans (C2), `restampWalk` after big edits, and the script compile cache (B8).

## Bloom readback (2026-11-18)

The bloom's 64×32 luminance read no longer waits for the GPU (`gpu::Device::queueReadRed` / `takeReadRed`, used by
`bloomFinish` in `effects.cpp`). Vulkan copies into the frame's ring and converts the copy once that frame's fence has
passed: the previous frame's fence when it is already signalled, at the latest when `presentTail` waits for the slot
(one frame of delay, two when the GPU is a frame behind). OpenGL reads into a ring of four pixel pack buffers, each
with a `glFenceSync`, polled with a zero timeout; the newest finished one is used. The measurement `m` now lags one or
two frames more than in the engine; `E` follows it with a 1 s half-life, so the picture does not change visibly.

Berlin, 1920x1080, `$bloom = true`, `REGS_BENCH=300:1400`, RTX 3080, old and new builds alternated:

| | fps old | fps new | render CPU ms old | new | GPU ms median old | new |
|---|---|---|---|---|---|---|
| Vulkan, run 1 | 44.8 | 96.4 | 15.28 | 5.64 | 7.83 | 2.97 |
| Vulkan, run 2 | 73.1 | 115.4 | 9.55 | 4.70 | 5.78 | 2.65 |
| Vulkan, run 3 | 72.1 | 114.3 | 9.68 | 4.71 | 6.14 | 2.62 |
| OpenGL, run 1 | 70.4 | 78.6 | 7.88 | 6.55 | 5.32 | 4.13 |
| OpenGL, run 2 | 69.6 | 78.8 | 8.37 | 6.94 | 5.42 | 4.20 |

With bloom off nothing changed (Vulkan 116.6 / 117.1 fps, OpenGL 89.1 / 89.3 fps). On Vulkan bloom now costs about
2 fps instead of about 45; the old GPU time included the idle gap at the wait inside the frame. Screenshots at frame
900 (`REGS_FIXED_DT=0.016`): with bloom on, 3.3 % of the pixels differ by one step of 255 from the old build on both
renderers (mean brightness 66.36 → 66.35, the extra frame of exposure delay); two runs of the old build differ by
0.2-0.4 % of the pixels (moving objects). With bloom off, the old and new pictures are the same. The Vulkan
validation layer is not installed on this PC, so that check was not run.

## Vulkan programs in parallel (2026-11-18)

The Vulkan device now implements `queueProgram`: the programs a draw list misses (`assignPrograms`) are read from the
SPIR-V cache or compiled by glslang on worker threads (hardware threads − 1, at most 8; the main thread helps while it
waits) and finished in queue order, so the program handles stay as before. Pipelines: before a draw list, when
programs were made since its target's formats last had one or the formats are new, `Renderer3D::draw` runs the list
once with the device only collecting the pipeline keys it lacks (the draw's own key code, so exact keys, not guesses),
the device creates them side by side through the shared `VkPipelineCache` (internally synchronized), then the list is
drawn for real. `REGS_VK_SERIAL=1` brings back one-by-one compiles and draw-time pipelines. Details in
`docs/renderer/vulkan.md` §3 and §5.

Berlin, Vulkan, 1920×1080, `REGS_FIXED_DT=0.016`, two runs per build, alternating (other sessions were running):

| | Before | After |
|---|---|---|
| First view, cold (`REGS_VK_CACHE=0`) | 1.79 s, 1.79 s (3.66 s with the machine loaded) | 0.83 s, 0.89 s (0.91 s loaded) |
| of it: programs (72) / pipelines (55) | 1.04 s / 0.31 s | 0.22 s / 0.22 s (batches of 2, 8, 21, 22) |
| First view, caches on but empty | 1.79 s | 0.98 s |
| First view, warm caches | 0.46 s, 0.54 s | 0.45 s, 0.40 s |
| Worst frame of `REGS_BENCH=300:1400`, cold | 53.5, 54.2 ms | 62.0, 53.2 ms |
| Worst frame, warm | 53.9, 54.0 ms | 53.1, 53.3 ms |

The worst frames hold no shader work (no programs or pipelines made in them; the 62 ms one was a single hitch while
other sessions ran). Pipelines scale less than compiles (22 in 90 ms on 9 threads, ~11 ms each one by one): the
driver serializes part of that work. Same output: the SPIR-V cache files of a cold run (both stages and the
reflection, 70 programs) are byte for byte the old serial build's; screenshots at frames 400 and 1705 without the
weather mod are pixel-identical across two runs of each build; with the weather mod the snow tyre tracks (and in one
cold run a traffic light's phase) differ by a few levels from run to run in both builds alike. OpenGL is unaffected
(one virtual call per draw list): first view with its program binaries 0.66 s in both builds, 98-99 fps in the
undisturbed runs. Left: particles, post-effect quads and the GUI still make their few pipelines at first draw, and a
program first needed during play (the snow particles) still compiles in its frame, now on a worker thread.

## Pedestrian path search (2026-11-18)

`walkSearch` (in `sim_citizens.cpp`) makes exactly the expansions of the search before it, in the same order, so every
path, every expansion count (the cap) and every failure stays the same; the old search is kept as `walkSearchReference`.
What changed: the open list holds 64-bit keys, f's bits over the q-cell's box position (z, x, layer), which order as the
old `std::pair<float, int>(f, index)` (f ≥ 0; equal keys are the same q-cell and f, so any heap pops the same sequence),
in a 4-ary heap with a one-entry slot in front of it for a key pushed below everything in the heap (a step towards the
goal at its parent's f comes next without going through the heap); a q-cell's cost, parent, stamp and height are one
16-byte record, the layers one after another, the ground record also caching the ground height (the old search kept
five arrays and interleaved the layers, about 2.9 in a Berlin box); an entry for a q-cell already expanded at its
present cost (left from a cost at most 1e-3 higher, which the stale test lets through) counts as an expansion but skips
the neighbours, which that cost already reached. In Berlin a search averages 15,000 expansions over 316,000 box q-cells;
about 7 % of the narrow searches fail and are repeated in the wide box, and those wide searches are 40 % of the time.

Sameness: `REGS_DEBUG_WALK_COMPARE=1` runs both searches for every request (in turns first) and logs any difference in
the result, path or expansion count, with both times every 1000 searches. Berlin and Vienna, speed 10,
`REGS_FIXED_DT=0.016`, 5000 frames: 0 differences in over 2000 (Berlin) and 5000 (Vienna) searches; the same search time
1.80× (Berlin, 3.87 s against 6.96 s) and 1.76× (Vienna, 5.33 s against 9.36 s) faster. The old and new build print the
same 491 digests (`REGS_DEBUG_SIM_DIGEST=10`, 5000 frames) on both saves, and so do the compare runs.

| Berlin, speed 10, Vulkan (profiled) | before | after |
|---|---|---|
| fps (two runs each, alternating) | 63.0, 61.9 | 55.3, 64.3 |
| pedestrian search, ms a frame | 2.00, 1.97 | 1.27, 1.15 |
| `stepCitizens`, ms a frame | 3.90, 3.93 | 3.63, 3.23 |

(The first new run had other load: render CPU 8.9 ms against 7.2-7.5 ms in the others.) The fps change is within the
noise of this machine; the search lost about 0.75 ms a frame (40 %). Left in the search: the ground height
(`layerHeight` for each new q-cell, a third of the remaining time) and the narrow searches that fail before the wide one.
