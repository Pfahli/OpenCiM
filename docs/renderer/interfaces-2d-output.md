# GSystem renderer: 2D/output side, resources and the frame

Scope: the GUI/output interfaces (`gsIClipArea`, `gsIViewport`, `gsIGuiVideoItem`, `gsIGuiPrimitive`, `gsIGuiText`,
`gsITexture`, `gsITextureContainer`, `gsIVideoOutput_OGL`), their OpenGL implementations, the post-effect classes,
the back-end singletons (`gsVideo[_OGL]`, `gsWindow[_OGL]`, `gsTextures_OGL`, `gsFonts_OGL`) and the frame.
The 3D interfaces (scene, mesh, material, shader, camera, light, skeleton, particles) belong to a colleague's
document. They are referenced here only where the frame or the post effects call them.

**Sources and conventions**

* Evidence is Ghidra 12.1.4 pseudocode in `decompiled/GSystem-v2/functions/0x<addr>.c`. It is not original source.
  Citations are `0x<addr>.c:<line>`.
* The skeleton is `renderer-map.json`; class layouts come from `docs/rtti-classes.json`. Script names come from
  `docs/native-api.md` and `docs/handlers/batch-B.md`. Script usage comes from `extracted/metro/scripts/*.script`.
* `FUN_10086fa0` is operator new and `FUN_10087080` is operator delete. `FUN_101210d8` is `_ftol2` (float to int).
  Its float argument lives on the x87 stack, which Ghidra drops. **Many `glViewport`/`glScissor`/`glTexParameter`
  arguments are therefore missing in the pseudocode.** Values marked *inferred* in those places are reconstructions.
* "Observed" means the pseudocode shows it directly. "Inferred" means it follows from layout, call sites or usage.
  Confidence is H (high), M (medium) or L (low).
* Offsets are byte offsets from the start of the object unless a subobject is named. Many methods run with `this`
  pointing at a subobject: `gsVideoItem_OGL` sits at +4 (+8 in `gsViewport_OGL`), the `gsModule` part of post
  effects at +4, and `gsMoveable` at +0xCC. The tables convert those addresses to object offsets.
* Colours: script and engine colours are `0xAARRGGBB` uints (D3D style). GUI vertex colours are stored with R and B
  swapped (R,G,B,A byte order) and handed to `glColor4ubv`. See the porting checklist in section 4.

---

## 1. Overview

### 1.1 Back-end object model (observed)

```
gsMain (0x130 B, created by export gsInitialize 0x10082f70)
└─ gsWindow_OGL (0x1C8 B)            script: new_window(OPENGL, MAIN)   0x10058190 → ctor 0x10058070
   │   HWND +0x158, HDC +0x15C, HGLRC +0x160, gsWindow.video +0x40
   └─ gsVideo_OGL (0x250 B)          created in gsWindow_OGL::create 0x10056f10 → ctor 0x1004f8f0
      │   window +0x1A4, gsShaders_OGL +0x1A8, gsTextures_OGL +0x1B0, gsFonts_OGL +0x1B4,
      │   root-output list (std::list<gsViewport_OGL*>) +0x1DC/+0x1E0, occlusion query id +0x1D0
      ├─ gsTextures_OGL (0xDC B)     created in gsVideo_OGL::create 0x1004de30
      ├─ gsFonts_OGL    (0xD8 B)     created in gsVideo_OGL::create 0x1004de30
      ├─ gsShaders_OGL  (0x100 B)    created in gsVideo_OGL::start  0x1004df50   (colleague)
      ├─ post effects (gsBloomEffect_OGL, gsShadowEffect_OGL, gsShaderEffect_OGL, gsTextureTarget_OGL)
      │                               created by the script methods new_*Effect / new_textureTarget
      └─ script gsViewPort / gsControl modules (their native gsI* items are created through the video factory)
```

* `gsVideo` is the abstract, script-visible video module. Its vtable has 72 slots. Slots 0–55 are the generic
  gsModule slots, and slots 56–71 are pure (`_purecall` 0x10120b90): the **factory and resource interface**.
  `gsVideo_OGL` implements them and adds slots 72–73 (`renderFrame`, an empty slot). See §2.10.
* `gsWindow` is the abstract window module. It has 66 slots, and slots 56–65 are window operations, most of them
  with base implementations. `gsWindow_OGL` overrides some and adds slots 66–68 (`setFullscreen`, `windowProc`,
  `writeScreenshotBMP`). See §2.11.
* Every native GUI/3D item is created by a `gsVideo` factory slot. The factory receives a **flags/type word** and a
  **parent** (`gsIClipArea*`). Each OGL item embeds a `gsVideoItem_OGL` node (vtable 0x1015a8f4). The node holds
  a refcount, the owning video, the flags, a visible byte, a layer (sort key) and a child vector. Parent/child links
  form the render tree.
* Generic gsModule slot usage: slot 29 (+0x74) is **destroy**, slot 30 (+0x78) **registerMembers**, slot 41
  (+0xA4) **create(parent)**, slot 42 (+0xA8) **start()→bool**, slot 48 (+0xC0) **setParent** (0x100a0050).
  Every module stores the **video of its window tree at +0x40**, inherited from its parent (0x100a0050:16). Evidence:
  `gsInitialize` 0x10082f70.c calls +0xA4 then +0xA8; module start 0x100a0500 calls +0xA8 on every child.

### 1.2 Window and GL context creation (observed, `gsWindow_OGL::start` 0x10059200)

1. `wglMakeCurrent(0,0)`. Register window class `GSystemWindowOGL` once (CS_OWNDC, WndProc `0x10057da0`, icon
   `IDI_ICON1`) (0x10059200.c:160-170).
2. If `fullscreen` (+0x1B4) is set, call `setFullscreen(true)` first: `ChangeDisplaySettingsA` with width, height,
   `screenColors` (+0x1B0) and `frequency` (+0x1AC), `CDS_FULLSCREEN`. On failure it logs
   *"Switching to fullscreen-mode failed!"* and falls back to windowed (0x10058720.c:34-66).
3. Colour bits (+0x1A0) are 16 if `screenColors` ≤ 16, else 24. Depth bits (+0x1A4) default to **16**, and there is
   no stencil or alpha (ctor 0x10058070.c:30-31; 0x10059200.c:177-180).
4. Window style is `WS_POPUP|WS_CAPTION|WS_SYSMENU`. Fullscreen uses `WS_POPUP`, and a set `parentWindow` (+0x154,
   editor embedding) gives `WS_CHILD` (0x10059200.c:182-187). The window is created and resized so the client area
   is width×height.
5. Legacy `ChoosePixelFormat`/`SetPixelFormat` with `PFD_DRAW_TO_WINDOW|SUPPORT_OPENGL|DOUBLEBUFFER`, then
   `wglCreateContext` and `wglMakeCurrent` (0x10059200.c:228-238).
6. Load `wglGetExtensionsStringARB`, then `wglChoosePixelFormatARB` if `WGL_ARB_pixel_format` is present and
   `wglSwapIntervalEXT` if `WGL_EXT_swap_control` is present. Apply `vsync` immediately (0x10059200.c:239-253).
7. **MSAA through the pixel format.** It is used when `antialiasing` (+0x19C) > 0 **and** `postProcess` (+0x1B9) is
   false, or when the unregistered field +0x1A8 is non-zero, and only if `wglChoosePixelFormatARB` exists. The
   attributes are DRAW_TO_WINDOW, SUPPORT_OPENGL, FULL_ACCELERATION, COLOR_BITS, ALPHA 0, DEPTH, STENCIL 0,
   DOUBLE_BUFFER, SAMPLE_BUFFERS 1, SAMPLES = antialiasing. On failure, `antialiasing -= 2` and it retries. If a
   format is found, the window and context are destroyed and recreated with it (0x10059200.c:256-309).
   With `postProcess` enabled, MSAA is instead done in the post-effect FBOs (§1.5.3).
8. Capability probing and the log (§1.3), then **start the children** (`FUN_100a0500`). That runs
   `gsVideo_OGL::start`, which creates the shader cache, clamps `anisotropy` to `GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT`
   and creates the occlusion query (0x1004df50.c:31-39), and then `gsTextures_OGL::start`, which loads
   `textures/default.dds` (0x10043360.c:28-32).
9. Fill `window.mode` with the info text ("Video: OpenGL <GL_VERSION>", "Device: <GL_RENDERER>", "Screen: … Hz",
   "Display: … bit", "Antialias", "Filter: trilinear|Nx anisotropic", "Depth", "Stencil", "Accumulation", "GLSL",
   "Motionblur"). Fill `window.version` with GSystem.dll's FileDescription and version (0x10059200.c:565-1084).
10. `ShowWindow`. If the requested size exceeds the desktop (`GetDeviceCaps` HORZRES/VERTRES), it is clamped
    through `setSize`. Then `created` (+0x1B5) is set to 1 (0x10059200.c:1206-1219).

Teardown (`gsWindow_OGL::destroy` 0x10059060) makes the context current and destroys the children (so GL objects
are freed with a live context). It leaves fullscreen, then `wglDeleteContext`, `ReleaseDC` and `DestroyWindow`.

### 1.3 Required vs optional GL features and fallbacks (observed, 0x10059200.c:334-555)

| Feature | Test | Loaded entry points | If missing |
|---|---|---|---|
| **Baseline** | `GL_SGIS_generate_mipmap`, `GL_ARB_texture_compression`, `GL_EXT_texture_compression_s3tc`, `GL_ARB_multitexture` | `glCompressedTexImage2DARB`, `glActiveTexture`, `glClientActiveTexture`, `glMultiTexCoord2f` | Error *"Minimun OpenGL requirements not supported"* (sic). It is only reported through gsMain `onErrorMsg`/MessageBox (`FUN_100a0320`). Init continues and is not aborted. |
| Occlusion query (`video.occlusionQuery`, +0x1F6) | `GL_ARB_occlusion_query` (`FUN_10055fb0`) | gen/delete/begin/end/getQueryObjectuiv | Flag cleared, *"Occlusion Query not supported"*. The LOD calibration (§1.5.4) returns 1.0. |
| Point sprites (`video.pointSprites`, +0x1F7) | `GL_ARB_point_parameters` + `GL_ARB_point_sprite` (`FUN_10056140`) | `glPointParameterf[v]ARB` | Flag cleared, *"Point Sprites not supported"* (particles: colleague). |
| Vertex attributes (+0x1F8, always probed) | `GL_ARB_vertex_program` (`FUN_10056030`) | `gl{En,Dis}ableVertexAttribArrayARB`, `glVertexAttribPointerARB`, `gl{Get,Bind}AttribLocationARB` | Flag cleared, *"Vertex attributes not supported"*. |
| VBO (+0x1F9, always probed) | `GL_ARB_vertex_buffer_object` (`FUN_100560b0`) | gen/bind/data/delete/map/unmap | Flag cleared, *"Vertex buffers not supported"* (mesh path: colleague). |
| **FBO** (+0x1FC, always probed) | `GL_EXT_framebuffer_object` (`FUN_100561b0`) | also loads `glBlitFramebufferEXT`, `glRenderbufferStorageMultisampleEXT` (EXT_framebuffer_blit/multisample) and `glGenerateMipmapEXT`. All 13 must resolve. | *"FrameBuffers not supported"*. `gsTextureRenderer_OGL::create` returns false, so every post effect stays bypassed (*"Framebuffer initialization failed!"*, *"Bloom initialization failed!"*, *"Shadowmap framebuffer creation failed"*). Texture targets then render nothing. |
| **GLSL** (`video.shaders`, +0x1F4, only if requested) | `GL_ARB_shading_language_100` + `GL_ARB_fragment_shader` + `GL_ARB_shader_objects` + `GL_ARB_vertex_shader` (`FUN_10055db0`) | 19 `*ARB` program/uniform functions, including `glUniformMatrix3fv/4fvARB`, which are missing from renderer-map's 56 | `shaders` set to false, *"GLSL not supported"*. `new_bloomEffect`/`new_shadowEffect`/`new_shaderEffect` then return null with *"Cannot initialize bloom / shadows / post-processing effect (requires GLSL)"* (0x10034c10, 0x100348c0, 0x100347b0). |
| Cg (+0x1F5) | none | none | Forced off and logs *"CG not supported"*. Dead code. |
| Hardware skinning (+0x1FB) | needs GLSL or Cg | none | *"Shaders needed for hardware skinning"* (colleague). |
| MSAA pixel format | `WGL_ARB_pixel_format` | `wglChoosePixelFormatARB` | No MSAA on the default framebuffer. |
| Vsync | `WGL_EXT_swap_control` | `wglSwapIntervalEXT` | Ignored. |
| Anisotropy | not tested by name | `glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT)` | Value clamped to the maximum. Also exposed as script `max_anisotropy(OPENGL)` 0x10057830. |
| GPU memory (`getVideoMemoryUsage`) | not tested | `glGetIntegerv(0x9048/0x9049)` (NVX_gpu_memory_info) | Returns 0 or garbage on non-NVIDIA (0x1004e640). |

Implicit requirements with no test: `GL_EXT_texture_filter_anisotropic`, `GL_ARB_texture_cube_map`,
`GL_ARB_depth_texture`/`ARB_shadow` (shadow compare mode, 0x10030b00.c:37-39) and `ARB_texture_float`/half-float
renderbuffers (bloom uses `GL_RGBA16F_ARB` targets, 0x10035c70.c:74). `GL_NVX_gpu_memory_info` is also used
without a test.

### 1.4 Resource caches (observed)

* **Textures**: `gsTextures_OGL` holds `std::map<path, gsTexture_OGL*>` at +0xCC/+0xD0 and the default texture
  (`textures/default.dds`) at +0xD8. `getTexture(path)` (video slot 67 → `FUN_100431c0`) has three outcomes:
  * An empty path returns the default texture.
  * A cache hit increments the refcount (+0x14) and returns the cached texture.
  * Otherwise a new `gsTexture_OGL` is created and `FUN_10041f30` loads the DDS. On success it is inserted into the
    map; on failure the temporary is deleted and the default texture is returned with its ref incremented.
  * `gsTexture_OGL`'s deleting destructor removes itself from the map (0x10042f20.c:18-21).
  * `gsTextures_OGL::destroy` (0x10042f80) detaches all live textures (`owner = 0`), clears the map and releases
    the default texture.
* **Fonts**: `gsFonts_OGL` holds `std::map<name, font*>` at +0xCC. `getFont(name, extra, style)` (`FUN_10040980`):
  * A miss loads `<name>/normal.fnt` and falls back to `<name>/normal.font` (`FUN_100406b0`; the style table has
    the single entry `normal`).
  * A non-empty `extra` merges glyphs from `<name>/<extra>.fnt|.font` into the same (shared) font.
  * Formats (`FUN_1003fb40`): AngelCode BMFont binary (magic `BMF\x03`, `FUN_1003f610`) or a native format (version
    `0x03000000`, `FUN_1003f260`) with a same-name `.dds` page. Glyph pages always come through the texture cache.
  * Shipped files: `fonts/verdana/normal.font`, `metro/fonts/*/normal.font`, plus 11 BMFont extras
    `metro/fonts/*/{ja,pl,ru,languages}.fnt` merged via `extra` = `MAIN.$language`.
  * Full formats, lookup rules and metrics: [docs/ui/fonts.md](../ui/fonts.md).
* **Shaders**: `gsShaders_OGL` (+0x1A8) via video slot 68 `getShader(path, a, b)`. An empty path means
  `shaders/default/default.shader` (0x1004fcd0). Colleague's scope.
* **Statistics**: global texture bytes `DAT_10194490` are adjusted by each upload/delete (w×h×4, or the
  compressed size). Each video tick copies them into `allocatedVertexBufferMemory/IndexBufferMemory/TextureMemory`
  (video +0x20C..+0x214, 0x1004f080.c:19-21).

### 1.5 The frame

#### 1.5.1 Main loop and timing (observed)

| Step | Function | What happens |
|---|---|---|
| 0 | export `gsMainLoop` 0x10082f00 | SEH wrapper (Steam minidump filter `FUN_10082ad0`), then `FUN_10082da0`. |
| 1 | `FUN_10082da0` 0x10082da0.c:29-56 | QueryPerformanceCounter timer. Loop until `DAT_10195be8` (quit). Each iteration measures **dt clamped to [0.0001, 0.05] s**, so the game runs in slow motion below 20 fps (lines 33-42). |
| 2 | `FUN_10082cd0` | Win32 pump with `PeekMessageW`/`TranslateMessage`/`DispatchMessageW`. `WM_QUIT` ends the loop. If `gsMain.waitMessages` (+0x100) is set, it first calls `MsgWaitForMultipleObjects(…,1000 ms,QS_ALLINPUT)` (idle until input). `init.script:26` sets it false. |
| 3 | gsMain `gsMoveable` slot 0 = `0x100a0700` (called at 0x10082da0.c:45) | Records memory stats and runs the **script VM scheduler** `FUN_1002c5d0(dt)` (timers/threads). Then it ticks the child modules (`FUN_100a0670`, children with flag bit 0 at +0x68). If `forceFramerate(fps)` was called (+0x108), children get a fixed dt of `1/fps` (+0xFC) while the VM keeps real dt (0x100a0700.c:17-22, 0x1009fd30). |
| 4 | `gsWindow_OGL` `gsMoveable` slot 0 = `0x10057160` | See below. Returns after `SwapBuffers`. |
| 5 | back in `FUN_10082da0:47-53` | **FPS limiter**: if `gsMain.fpsLimit` > 0 (stored as 1/fps at +0xFC, setter 0x1009fda0) and the frame took less, it sleeps via `FUN_100a3eb0(ms)`. That arms a 1 ms `timeSetEvent` multimedia timer and blocks on a critical section until the countdown expires (0x100a3e50.c:16-22). The ms argument is lost in the pseudocode (*inferred*: `(1/fps − elapsed)·1000`). `init.script:27` sets `fpsLimit` from settings. |

`gsWindow_OGL::tick(dt)` (0x10057160):

1. Timing (lines 25-49): frame counter and accumulated time. `fps` (+0x12C) is recomputed every ≥ 0.1 s,
   `avgFps` (+0x130) is frames/total time, and `frameTime` (+0x134) is in ms.
2. Cursor handling (lines 50-94): hides the cursor unless `systemCursor` is set. With `mouseCapture` and an active
   window, the cursor is re-centred and relative motion goes to `inputTarget` (+0x14C), normalised by window
   **height**. Otherwise the absolute position / height goes there.
3. `video->slot45()` (gsModule "move to end of the parent's tick list", 0x100a2720), then **tick all window
   children** (`FUN_100a0670`). This is the update phase. It includes `gsVideo_OGL::tick` (0x1004f080), which
   counts frames, copies stats, advances viewport crossfade timers and ticks the video's children: script
   viewports, controls, scenes and so on. **All updates finish before any rendering.**
4. `wglMakeCurrent` if this window is not current (`DAT_101948f0` caches the current HWND).
5. If the "needs clear" byte (+0x1B7) is set, `glClearColor(background)` and `glClear(COLOR)`, then reset the byte.
   The byte is set by the constructor and by `WM_ERASEBKGND`. **The back buffer is not cleared every frame.**
6. Reset `video` poly/submesh counters (+0x1BC/+0x1C4), call `video->renderFrame()` (slot 72 → 0x1004f2c0 →
   `renderAll` 0x1004f150), then publish `polyCount`/`submeshCount` (+0x1B8/+0x1C0).
7. `SwapBuffers(hdc)` (line 118). Vsync is applied here through the swap interval.

Script `window.update()` (0x1009fc80) calls slot 56 = `gsWindow_OGL::present` (0x10057720), which repeats steps
4-7 without ticking. Loading screens use it. While embedded in a parent window, `WM_PAINT` runs a full tick with
dt = 1e-7, then `glFinish` and posts `WM_USER+1` to the parent (0x10056970.c:51-58).

#### 1.5.2 `gsVideo_OGL::renderAll(viewport* only)` 0x1004f150 (observed)

* Builds the render context `ctx` on the stack (≈0x2C8 bytes, §1.6): window size, viewport rect = scissor = full
  window, identity matrices, tint 0xFFFFFFFF, visibleGroups mask.
* `glEnable(GL_SCISSOR_TEST)` (line 72).
* Renders each entry of the **root output list** (`video+0x1E0`) whose visible byte is set, through its
  `gsVideoItem_OGL::render(ctx)` (slot 4). The order is the list order (lines 77-84). The list holds every
  `gsViewport_OGL` that has **no parent**. `sendToBack` moves a viewport to the front of the list and
  `bringToFront` to the back.
* `renderViewport(vp)` (script, 0x1004f5d0) calls the same function for one viewport only. That is how texture
  targets are filled on demand: minimap thumbnails, the secondary view and the object list
  (`ui/objectlist/renderer.script`, `ui/secondaryview/secondaryview.script`).

In CiM the main 3D viewport (`viewport.script`) calls `.sendToBack()`, so it renders first. The GUI root viewport
(`window.script`, `pixelCoordinates = true`, rect = window) renders after it, on top.

#### 1.5.3 Per viewport: `gsViewport_OGL` (observed unless noted)

```
gsVideoItem_OGL::render  0x10044980   (this = viewport+8)
 ├ compute pixel rect into ctx+0x08..0x14:
 │    pixelCoordinates → origin + rect      else → origin + rect × parentHeight
 │    if a gsTextureTarget is attached (+0x7C) → rect × target.height, origin 0 (render-target space)
 ├ ctx.scissor (+0x18..0x24) = that rect   (NOT intersected with the parent's clip; see §4 quirks)
 ├ ctx.pixelCoords(+0x2E), ctx.lodFactor(+0x28), ctx.currentViewport(+0x22C) = this
 ├ if target texture is a cube map and a camera exists: 6× { temporary gsCamera_OGL fov 90°, target.face = i,
 │                                                          renderOutput } (0x10044980.c:110-170)
 │ else renderOutput (slot 19)
 └ if nested in another viewport: restore the parent's GUI state (glViewport, glOrtho, blend…, glScissor)

renderOutput  slot 19  0x100463a0   (only if the viewport has a scene or GUI children)
 ├ out = this (gsIVideoOutput_OGL subobject at +4)
 ├ for effect in effects (addEffect order):  effect.setup(ctx, out); out = effect          ← offscreen passes
 ├ glScissor(rect); glViewport(rect)                       (y flip from ctx window height: inferred)
 ├ out.render(ctx)                                          ← final composite into the current framebuffer
 └ if scene: overlayPass (slot 21 0x100447e0): glOrtho(0,aspect,1,0), unbind shader, fixed state,
            gsScene_OGL slot 4 (scene 2D overlay, colleague)

gsIVideoOutput_OGL::render of the viewport itself  0x10044950
 ├ scene  → slot 20 0x10044ef0  3D pass: glClear(depth|stencil [|color if clearColor.alpha≠0]),
 │                                projection from the camera (fov, near/far clamped by the camera), mirror,
 │                                viewport sub-rect, view matrix, frustum → ctx; fixed GL state;
 │                                ctx.visibleGroups = viewport.visibleGroups; gsScene_OGL render (colleague)
 └ GUI children → slot 22 0x100471a0  GUI pass: glViewport, glOrtho(0, w/h, 1, 0, −1, 1)
                                (×height when pixelCoordinates), disable cull/depth/lighting/fog/alpha test,
                                enable blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA, colour material, dither;
                                render children sorted by layer (FUN_10046ea0)
```

**Post-effect chain, with `engine.script:255-270` (shadows, then bloom) as the example:**

1. `shadow.setup(ctx, viewport)` (0x10031dd0): **shadow pass**.
   * Sets up the light camera (`light`, `safeMargin`, `maxDistance`, `clipYPlane`, `useBackSide`).
   * `ctx.overrideCamera(+0x230) = lightCam`, `ctx.envFlags(+0x234) |= 4` (ENVIROMENT_SHADOWMAP_RENDER).
   * `glEnable(POLYGON_OFFSET_FILL)` and `glPolygonOffset`.
   * Depth-only FBO: begin, `viewport.render(ctx)` (scene from the light), end.
   * Optional `screenSpaceBlur`: renders the scene with flags `2|8` into a screen RT, then blurs it twice with
     `shaders/shadow/blur.shader`.
2. `bloom.setup(ctx, shadow)` (0x100352a0): base setup **renders the input into the effect's FBO**.
   * The FBO is multisampled with `samples = window.antialiasing` (stepping down by 2 on failure) when the input
     reports `wantsMultisample()` (§2.8). `ctx+0x2D = 1` during this pass.
   * Rendering `shadow` binds the shadow map to texture unit 7 with `GL_COMPARE_R_TO_TEXTURE`/`LEQUAL` and sets
     envFlag 2 (SHADOWMAP), or binds the blurred screen-space term with flag 0x10 (SHADOW). Then it renders the
     main camera's scene.
   * Then: downsample (`shaders/bloom/scale.shader`) into a 512×256 RGBA16F target, then a separable blur over 4
     levels (512×256 … 64×32, `blur.shader`). The **64×32 level is read back to the CPU** (`glGetTexImage`,
     LUMINANCE/FLOAT) to get the average luminance (auto exposure). Finally `add.shader` sums the 4 levels.
3. `glScissor`/`glViewport`, then `bloom.render(ctx)` (0x100316d0): `final.shader` composites the scene RT and the
   bloom with `(lightness/(exposure·3+0.1), saturation, (1−saturation)·saturationWeight)` and tone-maps
   (`final.fragment`).
4. Scene overlay pass (slot 21).

#### 1.5.4 Odd one-time work

* `FUN_1004e8a0` (viewport `lodFactor`, cached at viewport+0x4C) draws a test quad with an occlusion query.
  It returns 1.0 without query support. Its exact purpose (a pixel-coverage/LOD calibration) is *inferred*.
* After each DDS upload the loader draws a tiny textured triangle (0x10041f30.c:163-178). *Inferred*: this forces
  the driver to make the texture resident.

### 1.6 Render context (`ctx`, built in 0x1004f150, passed to every `render(ctx)`)

| Offset | Meaning | Evidence | Conf. |
|---|---|---|---|
| +0x00/+0x04 | window width/height (float, px) | 0x1004f150.c:46-47 | H |
| +0x08/+0x0C | current viewport origin x/y (px, top-left origin) | 0x10044980.c:65-74 | H |
| +0x10/+0x14 | current viewport width/height (px). **+0x14 is the GUI unit when not in pixel mode.** | 0x10044980.c:67-94, 0x10046f20.c:30 | H |
| +0x18..+0x24 | current scissor rect x,y,w,h (px, top-left origin) | 0x10046f20.c:24-98 | H |
| +0x28 | lodFactor of the current viewport | 0x10044980.c:101, 0x10044ef0.c:88-91 | M |
| +0x2C | byte = 1 (unknown) | 0x1004f150.c:61 | L |
| +0x2D | "rendering into a post-effect buffer" (bloom sets it while drawing its input) | 0x100352a0.c:55-57 | M |
| +0x2E | current viewport `pixelCoordinates` | 0x10044980.c:96 | H |
| +0x30..+0x1DF | 3D camera/frustum block (eye, planes, view +0x90, projection +0xD0, screen-mapping +0x110…), saved and restored per 3D pass (colleague) | 0x10044ef0.c:155-358 | M |
| +0x1E0 | 4×4 matrix (copied from `DAT_101944d0`) | 0x1004f150.c:54-60 | M |
| +0x220 | float 1.0 (unknown; global alpha?) | 0x1004f150.c:64 | L |
| +0x224 | **visibleGroups mask**: mesh render skips items with `(mesh+0x88 & mask)==0` | 0x10044ef0.c:181, 0x1004a4d0.c:76 | H |
| +0x228 | current GUI parent node during child rendering | 0x10046ea0.c:25-36 | H |
| +0x22C | current viewport (for nested-viewport state restore) | 0x10044980.c:102,188 | H |
| +0x230 | override camera (light camera during the shadow pass) | 0x10031dd0.c:565, 0x10044ef0.c:380 | H |
| +0x234 | environment flags (`ENVIROMENT_SHADOWMAP`=2, `_SHADOWMAP_RENDER`=4, `_SHADOW_RENDER`=8, `_SHADOW`=0x10, 0x20 = back-face shadow) passed to shader selection | 0x10030b00.c:17-48, 0x10031dd0.c:557,723,820 | H |

---

## 2. Interfaces

Column legend: **introduced by** is the class whose vtable first declares the slot. **Name** is ours. **Sig** is
inferred from the implementation and call sites (`this` omitted).

### 2.0 Common bases and the internal render node

`gsIVideoItem` declares slot 0 `release()`. `gsIVideoElement` adds slot 1 `setVisible(bool)`. Neither has its own
vtable in RTTI. These are inferred from the identical implementations across all `*_OGL` classes, for example
`gsTexture_OGL` slot 0 = refcount release (0x10040c30) and `gsMesh_OGL`/`gsCamera_OGL` slot 1 = 0x10044680.

**`gsVideoItem_OGL` node** (vtable 0x1015a8f4, ctor 0x10047340). It is the OGL render-tree node embedded in every
item. Layout relative to the node: +4 refcount (starts at 1), +8 video, +0xC flags (factory arg 1), +0x10 visible
(default 1), +0x14 layer, +0x18 count of children with non-zero layer, +0x1C "needs sort", +0x24..+0x2C child
`vector`.

| Slot | Off | Name | Sig | Implementation | Evidence | Conf |
|---|---|---|---|---|---|---|
| 0 | 0x0 | deleting dtor | `void* (byte)` | frees the child vector | 0x100469b0, 0x10046670 | H |
| 1 | 0x4 | addChild | `void (node* child)` | refcount++, push_back, track non-zero layers → dirty | 0x10047630.c:16-33 | H |
| 2 | 0x8 | removeChild | `void (node* child)` | erase, refcount-- and delete self at 0 | 0x100466d0.c:14-31 | H |
| 3 | 0xC | childLayerChanged | `void (node*, int old, int new)` | update the non-zero count → dirty | 0x10044630.c:11-19 | H |
| 4 | 0x10 | **render** | `void (ctx*)` | base: if dirty, **stable sort children by layer ascending** (`FUN_10046b60`, comparator 0x10044660). Set ctx+0x228 = this, render visible children, restore. Overridden by each item. | 0x10046ea0.c:18-37 | H |

### 2.1 `gsIClipArea` (vtable 0x1015a888, 5 slots; impls `gsClipArea_OGL` 0x48 B, `gsViewport_OGL`)

`gsClipArea_OGL` layout: +0 iface, +4 node, +0x34 parent node, +0x38..+0x44 rect (x,y,w,h floats).

| Slot | Off | Introduced by | Name | Signature | OGL implementation (gsClipArea_OGL) | GL | Evidence | Conf |
|---|---|---|---|---|---|---|---|---|
| 0 | 0x0 | gsIVideoItem | release | `void ()` | node refcount-- (obj+8), delete at 0 | — | 0x1004ffe0.c:13-17 | H |
| 1 | 0x4 | gsIVideoElement | setVisible | `void (bool)` | node visible byte (obj+0x14) | — | 0x10044680.c:11 | H |
| 2 | 0x8 | gsIClipArea | setParent | `void (gsIClipArea* parent)` | removeChild from the old parent, `dynamic_cast<gsVideoItem_OGL>`, addChild | — | 0x100435f0.c:13-21 | H |
| 3 | 0xC | gsIClipArea | setRect | `void (float x, float y, float w, float h)` | stores the rect. Units are those of the enclosing viewport: px in pixel mode, else multiples of viewport height. **Absolute within the viewport, not relative to the parent clip.** | — | 0x100448c0.c:12-15 | H |
| 4 | 0x10 | gsIClipArea | setLayer | `void (int)` | notify parent node slot 3, store the node layer | — | 0x10044690.c:11-17 | H |
| node 4 | — | (gsVideoItem_OGL) | render | `void (ctx*)` | new scissor = parent scissor ∩ (origin + rect·scale); in pixel mode origin + rect. **Right/bottom edges omit the origin** (see quirks). If non-empty: `glScissor`, render children, restore the parent `glScissor`. **No translation**: children keep viewport coordinates. | glScissor | 0x10046f20.c:24-98 | H (rect math), M (y flip lost) |

Engine use (gsControl): `video.newClipArea(0, parentClip)`, then `setLayer(z·256+0x80)` (0x100a67d0.c:21-23).
`setRect(desktopLeft+marginL, desktopTop+marginT, clientW, clientH)` comes through slot 0xF4 (batch-B
setMargins).

### 2.2 `gsIViewport` (vtable 0x1015a8a4, 19 slots; impl `gsViewport_OGL` 0x9C B)

`gsViewport_OGL` layout:

| Offset | Field |
|---|---|
| +0 | gsIViewport |
| +4 | gsIVideoOutput_OGL |
| +8 | node |
| +0x38 | parent node |
| +0x3C..+0x48 | rect |
| +0x4C | lodFactor |
| +0x50 | gsCamera_OGL* |
| +0x54 | gsScene_OGL* |
| +0x58/+0x5C | near/far, default 0.1/400 |
| +0x60/+0x61 | mirror x/y |
| +0x64..+0x6C | crossfade state, default (0, −1, 0) |
| +0x70/+0x74 | effect list |
| +0x7C | texture target |
| +0x80 | clear colour, default 0xFF000000 |
| +0x84 | visibleGroups, default 0xFFFFFFFF |
| +0x88..+0x94 | viewport sub-rect, default (0,0,1,1) |
| +0x98 | pixelCoordinates |

Ctor: 0x100476a0. The script side is `gsViewPort`, which holds the native viewport at +0x20C.

| Slot | Off | Introduced by | Name | Signature | OGL implementation | GL | Evidence | Conf |
|---|---|---|---|---|---|---|---|---|
| 0 | 0x0 | gsIVideoItem | release | `void ()` | refcount at obj+0xC | — | 0x100446c0 | H |
| 1 | 0x4 | gsIVideoElement | setVisible | `void (bool)` | obj+0x18 | — | 0x100446d0 | H |
| 2 | 0x8 | gsIClipArea | setParent | `void (gsIClipArea*)` | detach from the old parent, or from the root list when parentless. A null new parent **appends to the video root output list**. | — | 0x10047450.c:18-38 | H |
| 3 | 0xC | gsIClipArea | setRect | `void (x,y,w,h)` | obj+0x3C..0x48 | — | 0x100448f0 | H |
| 4 | 0x10 | gsIClipArea | setLayer | `void (int)` | as clip area | — | 0x100446e0 | H |
| 5 | 0x14 | gsIViewport | setCamera | `void (gsICamera*)` | `dynamic_cast<gsCamera_OGL>` → +0x50 | — | 0x10044710; caller 0x100b47b0 | H |
| 6 | 0x18 | gsIViewport | setScene | `void (gsIScene*)` | `dynamic_cast<gsScene_OGL>` → +0x54 | — | 0x10044740; caller 0x100b4830 | H |
| 7 | 0x1C | gsIViewport | setMirror | `void (bool x, bool y)` | +0x60/+0x61. The 3D pass scales projection x/y by −1 and swaps the cull face. | — | 0x10044770; caller `gsViewPort.mirror` 0x100b42e0; use 0x10044ef0.c:116-134,377 | H |
| 8 | 0x20 | gsIViewport | setDepthRange | `void (float near, float far)` | +0x58/+0x5C (`minDistance`/`maxDistance`) | — | 0x10044790; callers 0x100b48c0/0x100b4900 | H |
| 9 | 0x24 | gsIViewport | sendToBack | `void ()` | move to the **front** of the root list (drawn first) | — | 0x100474f0; caller 0x100b4530 | H |
| 10 | 0x28 | gsIViewport | bringToFront | `void ()` | move to the **back** of the root list (drawn last) | — | 0x10047550; caller 0x100b4550 | H |
| 11 | 0x2C | gsIViewport | addEffect | `void (gsModule* effect)` | `dynamic_cast<gsPostEffect_OGL>`, push_back. +0x7C = `dynamic_cast<gsTextureTarget_OGL>` (null for others). No visible addRef. | — | 0x100475b0.c:14-24 | H |
| 12 | 0x30 | gsIViewport | removeEffect | `void (gsModule*)` | clear +0x7C if it matches, list remove | — | 0x100468a0 | H |
| 13 | 0x34 | gsIViewport | clearEffects | `void ()` | list clear | — | 0x10046390; caller 0x100b4330 | H |
| 14 | 0x38 | gsIViewport | setClearColor | `void (const uint* argb)` | +0x80. **An alpha byte of 0 disables the colour clear.** | — | 0x100447b0; 0x10044ef0.c:78-87 | H |
| 15 | 0x3C | gsIViewport | setVisibleGroups | `void (uint mask)` | +0x84 → ctx+0x224 during the 3D pass | — | 0x10043b40 (folded code); caller 0x100b4390 | H |
| 16 | 0x40 | gsIViewport | setViewportRect | `void (float x,y,w,h)` | +0x88..0x94. Off-centre projection (zoomed sub-window of the frustum). | — | 0x10044920; 0x10044ef0.c:136-154 | H |
| 17 | 0x44 | gsIViewport | setPixelCoordinates | `void (bool)` | +0x98 | — | 0x100447c0; caller 0x100b43c0 | H |
| 18 | 0x48 | gsIViewport | addRef | `void ()` | refcount++ | — | 0x100447d0; caller 0x100b43f0 | H |
| 19 | 0x4C | gsViewport_OGL | renderOutput | `void (ctx*)` | effect chain, then final output (§1.5.3) | glScissor, glViewport | 0x100463a0.c:18-50 | H |
| 20 | 0x50 | gsViewport_OGL | render3D | `void (ctx*)` | clear, projection/view, state, scene render (colleague for details) | glClear*, glDepthMask, glDepthFunc(LEQUAL), glHint, glLightModelfv, glCullFace, glMatrixMode/Push/Pop/LoadMatrixf, glAlphaFunc… | 0x10044ef0 | H |
| 21 | 0x54 | gsViewport_OGL | renderSceneOverlay | `void (ctx*)` | ortho 0..aspect × 0..1 (ignores pixel mode), alpha test GREATER 0.001, scene slot 4 | glOrtho, glAlphaFunc, glEnable/Disable | 0x100447e0 | H |
| 22 | 0x58 | gsViewport_OGL | renderGui | `void (ctx*)` | GUI pass state and children | glViewport, glOrtho, glBlendFunc, glColor4f, glColorMaterial, glActiveTexture | 0x100471a0 | H |
| node 4 | — | gsVideoItem_OGL | render | `void (ctx*)` | §1.5.3 | glViewport, glOrtho, glScissor | 0x10044980 | H |

### 2.3 `gsIGuiVideoItem` (no own vtable; base of primitive/text)

| Slot | Off | Introduced by | Name | Signature | Implementation | Evidence | Conf |
|---|---|---|---|---|---|---|---|
| 0 | 0x0 | gsIVideoItem | release | `void ()` | 0x1004ffe0 | 0x1004ffe0.c | H |
| 1 | 0x4 | gsIVideoElement | setVisible | `void (bool)` | 0x10044680 | — | H |
| 2 | 0x8 | gsIGuiVideoItem | setLayer | `void (int)` | 0x10044690 (shared with the clip area) | — | H |

Both `gsGuiPrimitive_OGL` (slot 8) and `gsGuiText_OGL` (slot 18) also have an **OGL-only** extra virtual,
`0x100435f0` `setParent(gsIClipArea*)`. It is not part of the interfaces, so reparenting a GUI item is not
possible through the interface. Parents are fixed at creation.

### 2.4 `gsIGuiPrimitive` (vtable 0x1015a840, 8 slots; impl `gsGuiPrimitive_OGL` 0x60 B, ctor 0x10044350)

Layout:

| Offset | Field |
|---|---|
| +0x34 | parent |
| +0x38 | vertex array (32 B/vertex) |
| +0x3C | count |
| +0x40 | capacity |
| +0x44 | rotation angle |
| +0x48..+0x50 | rotation pivot |
| +0x54 | gsMaterial_OGL* |
| +0x58 | GL primitive mode |
| +0x5C | triangle count (stats) |

**Vertex (32 B)**: `+0 float3 position`, `+0xC uint colour (R,G,B,A bytes)`, `+0x10 float2 uv`,
`+0x18..+0x1F unused` (*inferred*).

**Type flags** (node +0xC): 4 → `GL_QUADS`, 8 → `GL_QUAD_STRIP`, 0x10 → `GL_LINES`, 0x20 → `GL_LINE_STRIP`,
otherwise `GL_TRIANGLES`. Engine callers pass 7, 0xB and 0x23. Bits 1/2 are always set and never read
(*inferred*: they meant "has colour/uv").

| Slot | Off | Introduced by | Name | Signature | OGL implementation | GL | Evidence | Conf |
|---|---|---|---|---|---|---|---|---|
| 0–2 | | (see 2.3) | release / setVisible / setLayer | | | | | H |
| 3 | 0xC | gsIGuiPrimitive | setVertexCount | `void (int n)` | grow the array (copy old), derive mode and tri count from the flags | — | 0x10044050.c:26-89 | H |
| 4 | 0x10 | gsIGuiPrimitive | setVertices | `void (int first, int count, const Vertex*)` | memcpy 32 B each | — | 0x10043640.c:16-29 | H |
| 5 | 0x14 | gsIGuiPrimitive | setMaterial | `void (gsIMaterial*)` | release the old (slot 0), addRef the new (material slot 1) | — | 0x10043680.c:11-17; caller 0x100a7860.c:158 | H |
| 6 | 0x18 | gsIGuiPrimitive | setRotation | `void (float angle, const float3* pivot)` | angle ≠ 0 → render loads a rotation matrix about the pivot | — | 0x100436b0; use 0x10043720.c:39-48 | M (axis constant) |
| 7 | 0x1C | gsIGuiPrimitive | setMaterialParam | `void (int i, const float4*)` | writes material+0x9C+16·i (shader `custom` params) | — | 0x100436e0.c:13-18 | H |
| node 4 | — | gsVideoItem_OGL | render | `void (ctx*)` | Reset the material state (`FUN_10047860`). Bind the material shader if it has one. Material colours: ambient/diffuse 1, emission/specular 0. Texture unit 0 = material tex 0 (else disable), units 1-3 = material tex 1-3. **Immediate mode** `glBegin(mode)`; per vertex `glColor4ubv`, `glTexCoord2fv`, `glVertex3fv`; `glEnd`. Unbind units and shader, video polyCount += tris. | glBegin/End, glColor4ubv, glTexCoord2fv, glVertex3fv, glBindTexture, glActiveTexture, glMaterialfv, glLoadMatrixf/LoadIdentity, glEnable/Disable | 0x10043720.c:27-100 | H |

Engine use: the control image is one quad (4 verts, uv = crop rect, layer z·256) and the border is 4 quads
(16 verts, layer z·256+1). Both live in the **parent's** clip area (0x100a7860.c:146-167, 0x100a7190.c:200-218).

### 2.5 `gsIGuiText` (vtable 0x1015a7f4, 18 slots; impl `gsGuiText_OGL` 0x90 B, ctor 0x10044420)

Layout:

| Offset | Field | Default |
|---|---|---|
| +0x34 | parent | |
| +0x38 | font | `fonts/verdana` |
| +0x3C | wchar text | |
| +0x40 | capacity | |
| +0x44 | position | |
| +0x50 | box | 10000, 10000 (x = wrap width) |
| +0x5C | char size | 0.015, 0.015, 1 |
| +0x68 | spacing (x = extra advance, y = extra line gap) | |
| +0x74 | shadow offset | 0.001, 0.001 |
| +0x80 | colour | white |
| +0x84 | background colour | 0 |
| +0x88 | shadow colour | 0 |
| +0x8C | float x offset | |

| Slot | Off | Introduced by | Name | Signature | OGL implementation | Evidence | Conf |
|---|---|---|---|---|---|---|---|
| 0–2 | | (2.3) | release / setVisible / setLayer | | | | H |
| 3 | 0xC | gsIGuiText | setText | `void (const wchar_t*)` | copy into its own buffer | 0x10043a00; caller 0x100a85b0 | H |
| 4 | 0x10 | gsIGuiText | setFont | `void (const wstring& name, const wstring& extra)` | `gsFonts_OGL::getFont(name, extra, 0)` | 0x10043a60; caller 0x100a8870.c:27 | H |
| 5 | 0x14 | gsIGuiText | setPosition | `void (const float3*)` | +0x44 | 0x10043a90; caller 0x100a85e0.c:133 | H |
| 6 | 0x18 | gsIGuiText | setBox | `void (const float3*)` | +0x50, `x` = wrap width | 0x10043ab0; use 0x10043b60.c:46 | M |
| 7 | 0x1C | gsIGuiText | setCharSize | `void (const float3*)` | +0x5C (glyph height = y) | 0x10043ad0; caller 0x100a8870.c:30 | H |
| 8 | 0x20 | gsIGuiText | setSpacing | `void (const float3*)` | +0x68 | 0x10043af0; caller 0x100a8870.c:31 | H |
| 9 | 0x24 | gsIGuiText | setShadowOffset | `void (const float3*)` | +0x74 | 0x10043b10 | M |
| 10 | 0x28 | gsIGuiText | setColor | `void (uint argb)` | +0x80 (pre-multiplied by control visibility on the engine side) | 0x10043b30; caller 0x100a6440.c:24 | H |
| 11 | 0x2C | gsIGuiText | setBackgroundColor | `void (uint argb)` | +0x84. Alpha ≠ 0 draws an untextured quad behind each glyph (selection highlight). | 0x10043b40; 0x1003d410.c:92-104 | M |
| 12 | 0x30 | gsIGuiText | setShadowColor | `void (uint argb)` | +0x88. Alpha ≠ 0 enables the shadow pass. | 0x10043b50; caller 0x100a6440.c:26 | H |
| 13 | 0x34 | gsIGuiText | setXOffset | `void (float)` | +0x8C, an initial pen x (scroll/indent) | 0x1004fef0; 0x10043df0.c:29 | M |
| 14 | 0x38 | gsIGuiText | measureWidth | `float (const wchar_t*)` | widest line (`advance = glyph.w/glyph.h·size.x + spacing.x`) | 0x10044230; caller `getTextWidth` 0x100a8050 | H |
| 15 | 0x3C | gsIGuiText | measureHeight | `float (const wchar_t*)` | height after word-wrap at box.x (`line = size.y + spacing.y`) | 0x10043b60; caller `getTextHeight` 0x100a80a0 | H |
| 16 | 0x40 | gsIGuiText | charAdvance | `float (wchar_t)` | single-glyph advance (cursor/selection) | 0x10043d80; caller 0x100a80f0 | H |
| 17 | 0x44 | gsIGuiText | getPositionPtr | `float3* ()` | returns &+0x44 | 0x10043de0 | H |
| node 4 | — | gsVideoItem_OGL | render | `void (ctx*)` | needs font+text. If the shadow colour has alpha: `glTranslatef(pos+shadowOfs)`, `glScalef(size)`, `font.draw(…, shadowColour, inline colours off)`. Then the main pass at `pos` with `colour`. polyCount += glyphs. | 0x10043df0.c:16-50 | H |

**Font draw** `FUN_1003d410(text, colour, bgColour, x0, applyInlineColours, wrapW, spacingX, spacingY)`:
* Glyph space is 1 unit high (one font line height `L`). `glTranslatef(x0)` sets the start; each glyph is an
  immediate-mode `GL_QUADS` with a **one-texel** pad (1/page height, `FUN_1003d290`). The page texture is
  re-bound only when the page changes.
* Control codes (observed; the Linux build's `switch` shows the values):
  * Code 4: inline colour, 8-wchar `AARRGGBB` payload parsed by `FUN_10074d80` (on Windows only the first digit of
    each pair is used), alpha = `(inline.a·base.a) >> 8`. Applied only in the main pass; always skips 9 units.
  * Code 5: reset to the base colour. Code 13 (`\r`): ignored.
  * Tab (9): the *k*-th tab of a line moves the pen to `k·3·(spacingX+1)`, regardless of the text before it.
  * `\n` (10): new line at x = 0 (the indent `x0` applies to the first line only).
  * Space (32): look ahead one word and wrap if space + word would end beyond `wrapW`; any glyph that would itself
    end beyond `wrapW` also wraps. Spaces are never drawn.
  * Missing glyphs fall back to the glyph with the smallest key (cell 0 in the native fonts).
* Evidence: 0x1003d410.c:37-175; Linux `gsFont_OGL::renderText` 0x081f65b0. Full algorithm, measuring functions
  and worked examples: [docs/ui/fonts.md](../ui/fonts.md).

### 2.6 `gsITexture` (4 slots; impl `gsTexture_OGL` 0x24 B, vtable 0x1015a4c4, ctor 0x100412f0)

Layout: +4 bytes accounted, +8 owner cache, +0xC cache map node, +0x10 **GL name** (`glGenTextures` in the ctor),
+0x14 refcount (starts at 1), +0x18 isCubeMap, +0x1C width, +0x20 height.

| Slot | Off | Introduced by | Name | Signature | OGL implementation | GL | Evidence | Conf |
|---|---|---|---|---|---|---|---|---|
| 0 | 0x0 | gsIVideoItem | release | `void ()` | refcount-- and call slot 3(1) at 0 | — | 0x10040c30.c:13-17 | H |
| 1 | 0x4 | gsITexture | uploadBGRA | `void (const void* bgra, int w, int h)` | 2D only. RGBA8 from **BGRA/UNSIGNED_BYTE**, MIN/MAG LINEAR, WRAP REPEAT, anisotropy 1, no mipmaps. Accounting w·h·4. | glBindTexture, glTexParameteri/f, glTexImage2D | 0x10040c40.c:13-28 | H |
| 2 | 0x8 | gsITexture | readAlpha | `void (uint8_t** out, int* w, int* h)` | allocates w·h bytes, `glGetTexImage(GL_ALPHA, UNSIGNED_BYTE)`. Cube maps return null. Callers not traced. | glBindTexture, glGetTexImage | 0x10040d40.c:14-27 | H (L for caller) |
| 3 | 0xC | gsITexture | deletingDtor | `void* (byte)` | un-account, `glDeleteTextures`, erase from the cache map | glDeleteTextures | 0x10042f20.c:14-24 | H |

Helpers used everywhere: `bind` `FUN_10040dd0` and `unbind` `FUN_10040e20` (`glEnable`+`glBindTexture` on
`GL_TEXTURE_2D` or `GL_TEXTURE_CUBE_MAP`).

### 2.7 `gsITextureContainer` (vtable 0x1015a434, 1 slot; impl `gsTextureVariant_OGL` 0x28 B)

| Slot | Off | Introduced by | Name | Signature | Implementation | Evidence | Conf |
|---|---|---|---|---|---|---|---|
| 0 | 0x0 | gsITextureContainer | getTexture | `gsITexture* ()` | returns obj+0x1C, no addRef | 0x10040fa0 | H |

`gsTextureVariant_OGL` is the script value returned by `video.new_texture([path])` (0x1004f470) and
`textureTarget.getTexture()/resetTexture()`. `gsControl.setImage` accepts it (batch-B). Without a path,
`FUN_10041340` creates an empty uncached `gsTexture_OGL`. Script members (registrar 0x10041a60):

* `width`, `height` (getters 0x10040f10/0x10040f20).
* `saveToFile(path)` (0x10041930 → `FUN_100413c0`, §3.5).
* `saveToColormap(colormap, x, y, [flipY])` (0x10040fc0, §3.5).
* `fill()` (0x10040e60): `glTexSubImage2D(…, BGRA, …, NULL)` with no PBO bound. It is effectively broken or a
  no-op and unused by scripts.

### 2.8 `gsIVideoOutput_OGL` (vtable 0x101591b8, 2 slots) and `gsPostEffect_OGL` slot 2

Implementers: `gsViewport_OGL` (subobject +4), `gsPostEffect_OGL` and its subclasses `gsBloomEffect_OGL`,
`gsShadowEffect_OGL`, `gsShaderEffect_OGL`, `gsTextureTarget_OGL` (all at +0; the gsModule part is at +4).

Common post-effect layout (base ctor 0x10031180):

| Offset | Field |
|---|---|
| +0xD0 | window |
| +0xD4 | video |
| +0xD8 | gsTextureRenderer_OGL* (FBO) |
| +0xDC | input output |
| +0xE0 | output gsTexture_OGL |
| +0xE4/+0xE8 | render offset (texture target: `renderOffsetX/Y`) |
| +0xEC/+0xF0 | current size px |
| +0xF4/+0xF8 | allocated size |
| +0xFC | **bypass**, set when the FBO is not ready or failed |
| +0xFD | multisampled |
| +0xFE | float format (default 1) |
| +0xFF | mipmaps |
| +0x100 | cube face |

| Slot | Off | Introduced by | Name | Signature | Implementations | Evidence | Conf |
|---|---|---|---|---|---|---|---|
| 0 | 0x0 | gsIVideoOutput_OGL | wantsMultisample | `bool ()` | Viewport: **true** (0x100d47b0). Post effect: `bypass && input ? input.wantsMultisample() : false` (0x1002f730). Shadow: `input ? input.wantsMultisample() : false` (0x10030ae0). Used to choose an MSAA FBO (0x10034530.c:38-44). | 0x1002f730, 0x10030ae0, 0x100d47b0 | M (name inferred from the single use) |
| 1 | 0x4 | gsIVideoOutput_OGL | render | `void (ctx*)` | Viewport: 3D or GUI pass (0x10044950). Post effect base: bypass → `input.render`, else a fullscreen quad of its FBO texture (0x100312b0). Shader effect: bind its shader (gsIShader slot 3), quad, unbind (0x10031390). Bloom: final composite (0x100316d0). Shadow: bind the shadow term on unit 7 and render the input (0x10030b00). Texture target: **nothing** (0x10067c00 is an empty export stub). | see impls | H |
| 2 | 0x8 | gsPostEffect_OGL | setup | `void (ctx*, gsIVideoOutput_OGL* input)` | Base (0x100351f0): input = arg; `ensureRenderTarget` (0x10034530); if ready: `fbo.begin(off, size/texSize)`, set the cube face, `input.render(ctx)`, `fbo.end()` (resolve, mipmaps). Bloom 0x100352a0 and shadow 0x10031dd0 override it (§1.5.3). | 0x100351f0.c:13-25 | H |

`ensureRenderTarget` (0x10034530) (re)creates the FBO when the multisample need changes or the viewport grows
(sizes only grow). The size is at least the output texture's size. It tries
`create(w, h, mip, msaa, float, samples)` with samples = `window.antialiasing`, −2 per retry, until 0. On failure
it logs *"Framebuffer initialization failed!"* and stays bypassed.

`FUN_1002f750(x1, y1, s, t, tex0..tex3)` is the fullscreen-quad helper: `glOrtho(0,1,1,0)`, fixed state,
`glInterleavedArrays(GL_T2F_V3F)`, `glDrawArrays(GL_QUADS, 0, 4)`. Its uv scale `s,t = size/texSize` because
render targets can be larger than the viewport.

### 2.9 `gsTextureRenderer_OGL` (internal FBO wrapper, vtable 0x10159ae8, 0x38 B, ctor 0x100357b0)

Layout:

| Offset | Field |
|---|---|
| +4 | window |
| +8 | main FBO |
| +0xC/+0x10 | resolve FBOs (+0x10 holds cube faces 3-5) |
| +0x14 | colour renderbuffer |
| +0x18 | depth renderbuffer |
| +0x1C/+0x20 | size |
| +0x24 | mipmaps |
| +0x25 | float |
| +0x26 | needs depth |
| +0x28 | samples |
| +0x2C | cube face |
| +0x30 | target gsTexture_OGL |
| +0x34 | depth-only |

| Slot | Off | Name | Signature | Implementation | Evidence | Conf |
|---|---|---|---|---|---|---|
| 0 | 0x0 | deletingDtor | | | 0x10036470 | H |
| 1 | 0x4 | createColorTarget | `bool (int w, int h, bool mip, bool depth, bool float16, int samples)` | Returns false without FBO support. If samples = 0, 2D and no depth: the texture is attached directly. Else colour RB (RGBA8 or RGBA16F, MSAA via `glRenderbufferStorageMultisampleEXT`) on the main FBO, the texture on the resolve FBO (cube: 6 faces across 2 FBOs × 3 attachments), optional DEPTH_COMPONENT24 RB. Clear, then check `GL_FRAMEBUFFER_COMPLETE`. | 0x10035c70.c:25-135 | H |
| 2 | 0x8 | createDepthTarget | `bool (int w, int h)` | depth texture on DEPTH_ATTACHMENT, an RGB colour RB, `glDrawBuffer(NONE)` (shadow map) | 0x10036060.c:19-79 | H |
| 3 | 0xC | setCubeFace | `void (int)` | +0x2C | 0x100362c0 | H |
| 4 | 0x10 | begin | `void (float x, float y, float sx, float sy)` | bind the main FBO, draw buffer, scissor + viewport (arguments lost) | 0x100362d0 | M |
| 5 | 0x14 | end | `void ()` | if resolving: `glBlitFramebufferEXT` main → resolve (COLOR, NEAREST) into the face. Unbind. If mip: `glGenerateMipmapEXT`. | 0x10036380.c:15-36 | H |
| 6 | 0x18 | bindTexture | `void ()` | `FUN_10040dd0(target)` | 0x10036450 | H |
| 7 | 0x1C | unbindTexture | `void ()` | `FUN_10040e20(target)` | 0x10036460 | H |

A non-virtual `setTexture` (0x10036490) re-attaches a new target texture (used by `resetTexture`).

### 2.10 `gsVideo` / `gsVideo_OGL` virtual factory (vtable 0x1015aed4)

| Slot | Off | Introduced by | Name | Signature (inferred) | OGL implementation | Evidence | Conf |
|---|---|---|---|---|---|---|---|
| 56 | 0xE0 | gsVideo (pure) | newViewport | `gsIViewport* (uint flags, gsIClipArea* parent)` | `new gsViewport_OGL` (0x9C) | 0x1004e020; caller 0x100b43f0.c:22 | H |
| 57 | 0xE4 | gsVideo (pure) | newClipArea | `gsIClipArea* (uint flags, gsIClipArea* parent)` | `new gsClipArea_OGL` (0x48) | 0x1004e090; caller 0x100a67d0.c:21 | H |
| 58 | 0xE8 | gsVideo (pure) | newGuiPrimitive | `gsIGuiPrimitive* (uint typeFlags, gsIClipArea* parent)` | `new gsGuiPrimitive_OGL` (0x60) | 0x1004e100; callers 0x100a7190.c:207, 0x100b31e0.c:81 | H |
| 59 | 0xEC | gsVideo (pure) | newGuiText | `gsIGuiText* (uint flags, gsIClipArea* parent)` | `new gsGuiText_OGL` (0x90) | 0x1004e170; caller 0x100a7bf0.c:19 | H |
| 60 | 0xF0 | gsVideo (pure) | newScene | (colleague) | gsScene_OGL | 0x1004e1e0 | H |
| 61 | 0xF4 | gsVideo (pure) | newMesh | 4 args (colleague) | gsMesh_OGL | 0x1004e250 | H |
| 62 | 0xF8 | gsVideo (pure) | newSkeleton | (colleague) | gsSkeleton_OGL | 0x1004e2c0 | H |
| 63 | 0xFC | gsVideo (pure) | newPrimitive3D | (colleague) | gsPrimitive_OGL | 0x1004e330 | H |
| 64 | 0x100 | gsVideo (pure) | newLight | (colleague) | gsLight_OGL | 0x1004e3a0 | H |
| 65 | 0x104 | gsVideo (pure) | newCamera | (colleague) | gsCamera_OGL | 0x1004e410 | H |
| 66 | 0x108 | gsVideo (pure) | newParticleSystem | (colleague) | gsParticleSystem_OGL | 0x1004e480 | H |
| 67 | 0x10C | gsVideo (pure) | getTexture | `gsITexture* (const wstring& path)` | `gsTextures_OGL::getTexture` (§1.4) | 0x1004e4f0, 0x100431c0 | H |
| 68 | 0x110 | gsVideo (pure) | getShader | `gsIShader* (const wstring& path, uint, uint)` | shader cache; default shader for an empty path | 0x1004fcd0 | H |
| 69 | 0x114 | gsVideo (pure) | newMaterial | `gsIMaterial* ()` | gsMaterial_OGL (0x120) | 0x1004ef80 | H |
| 70 | 0x118 | gsVideo (pure) | getDistanceFactor | `float ()` | +0x218 (script `distanceFactor`) | 0x1004fab0 | H |
| 71 | 0x11C | gsVideo (pure) | getEffectTimer | `float ()` | shaders+0xFC | 0x1004e510; script 0x1004e620 | H |
| 72 | 0x120 | gsVideo_OGL | renderFrame | `void ()` | `renderAll(null)`, clear "first frame" +0x1FD | 0x1004f2c0 | H |
| 73 | 0x124 | gsVideo_OGL | (empty) | `void ()` | returns | 0x1004e500 | H |

gsModule overrides:

* 29 destroy (0x1004df20): deletes the occlusion query.
* 41 create (0x1004de30): window cast, textures, fonts.
* 42 start (0x1004df50): shaders, anisotropy clamp, query.

Script members (registrar 0x1004f630):

* Fields: `anisotropy` f +0x1E8, `shaders` b +0x1F4, `polyCount` +0x1B8, `submeshCount` +0x1C0, `nearPlane` f
  +0x1EC (default 0.1), `motionBlur` f +0x1F0, `hardwareSkinning` +0x1FB, `maxVertexWeights` +0x200 (4),
  `optimizeSkinGroups` +0x204, `gridLodSize` +0x208 (16), `pointSprites` +0x1F7, `occlusionQuery` +0x1F6,
  `allocated*Memory` +0x20C..0x214, `distanceFactor` f +0x218 (1.0), `blendLod` +0x1FF (1), `textureDetail` +0x21C.
* Methods: `new_bloomEffect()`, `new_shaderEffect(path)`, `new_shadowEffect(w,h)`,
  `new_textureTarget(path | w,h,cube)`, `new_texture([path])`, `renderViewport(vp)`, `getEffectTimer()`,
  `getVideoMemoryUsage()`.
* `motionBlur` is only printed in the log; no other reader exists in the pseudocode, so it is **effectively
  unused**. For `textureDetail`, see §5.

### 2.11 `gsWindow` / `gsWindow_OGL` (vtable 0x1015ba9c)

| Slot | Off | Introduced by | Name | Signature | gsWindow → gsWindow_OGL | Evidence | Conf |
|---|---|---|---|---|---|---|---|
| 56 | 0xE0 | gsWindow | present | `void ()` | nop → make current, clear if needed, `video.renderFrame`, `SwapBuffers` | 0x10057720.c:11-29 | H |
| 57 | 0xE4 | gsWindow | setWidth | `void (int)` | → setSize(w, height) | 0x10057f60 | H |
| 58 | 0xE8 | gsWindow | setHeight | `void (int)` | → setSize(width, h) | 0x10057f80 | H |
| 59 | 0xEC | gsWindow | setSize | `void (int w, int h)` | store → also re-centre the capture point and `SetWindowPos` (client size) | 0x10057590 / 0x10057630 | H |
| 60 | 0xF0 | gsWindow | setTitle | `void (const wstring&)` | store → `SetWindowTextW` | 0x100582a0 / 0x10058870 | H |
| 61 | 0xF4 | gsWindow | updateInputTarget | `void (target*)` | input routing (out of scope) | 0x100a0dc0 | M |
| 62 | 0xF8 | gsWindow | addInputTarget | `void (target*, int prio)` | (input) | 0x100a2260 | M |
| 63 | 0xFC | gsWindow | removeInputTarget | `void (target*)` | (input) | 0x100a0e50 | M |
| 64 | 0x100 | gsWindow | setMouseCapture | `void (bool)` | +0x151 | 0x10057fa0 | H |
| 65 | 0x104 | gsWindow | captureScreen | `void (const string& path)` | **empty in both classes** (0x10067c00) | caller 0x100a0be0.c:25 | H |
| 66 | 0x108 | gsWindow_OGL | setFullscreen | `void (bool)` | `ChangeDisplaySettingsA` (see §1.2) | 0x10058720 | H |
| 67 | 0x10C | gsWindow_OGL | windowProc | `LRESULT (UINT, WPARAM, LPARAM)` | activation events, screensaver block, close/quit, `WM_PAINT` tick when embedded, `WM_ERASEBKGND` → clear flag, keyboard/mouse/wheel events | 0x10056970 | H |
| 68 | 0x110 | gsWindow_OGL | writeScreenshotBMP | `void (const char* path)` | `glReadPixels(BGR)` of the back buffer → 24-bit BMP. **No caller found.** | 0x10057000.c:39-53 | H (L for reachability) |

gsWindow script fields: `fps`, `avgFps`, `frameTime`, `width`, `height`, `title`, `mode` (renderer info text),
`version`, `video`, `inputTarget`, `mouseCapture`, `background` (ARGB).

gsWindow_OGL fields: `antialiasing`, `frequency` (default 85), `fullscreen` (default true, setter → slot 66),
`preventScreenSaver`, `parentWindow`, `windowHandle`, `screenColors` (default 32), `key`, `disableInputEvents`,
`postProcess`, `vsync` (default true, setter calls `wglSwapIntervalEXT` once the window exists, 0x10056ed0),
`systemCursor`, events `onActivate`/`onDeactivate`, and method `crossFade(viewport, time)` (0x100575d0; see §5).

Globals: `display_modes()` (0x100588b0) enumerates `EnumDisplaySettingsA` (default orientation only), sorted by
`FUN_100a4c40` (sort key not checked), as
`gsDisplayModeVariant{width,height,bits,freq}`. `current_display_mode()` (0x100579c0) uses
`ENUM_CURRENT_SETTINGS`. `max_anisotropy(OPENGL)` (0x10057830).

### 2.12 Post-effect classes

| Class | Size / ctor | Script members | Specific behaviour | Evidence |
|---|---|---|---|---|
| gsPostEffect_OGL | base 0x10031180 | — | §2.8 | — |
| gsShaderEffect_OGL | 0x124 / 0x100343f0 | — | `new_shaderEffect(path)`. create loads the shader from `path` (+0x108) into +0x104. render = shader quad of the input FBO. Only `shaders/post_process/vertblur.shader` exists, referenced only in commented-out script. | 0x10031320, 0x10031390 |
| gsBloomEffect_OGL | 0x150 / 0x100315d0 | `scale` (+0x13C, default 0.5, −1 = auto exposure), `saturation` (+0x144, 1.0), `lightness` (+0x148, 0.9), `saturationWeight` (+0x14C, 1.0) | Shaders `shaders/bloom/{scale,blur,add,final}.shader`. 8 RGBA16F targets: A0-3 at +0x10C, B0-3 at +0x11C, 512×256 ÷2ⁱ. CPU luminance readback each frame → +0x140. A failed create returns null from `new_bloomEffect`. | 0x10034d40, 0x100352a0, 0x100316d0, 0x10034c10.c:55-58 |
| gsShadowEffect_OGL | 0x138 / 0x10031450 | `viewport` +0x104, `light` +0x108, `screenSpaceBlur` +0x118, `safeMargin` +0x128, `clipYPlane` +0x12C, `useBackSide` +0x12D, `maxDistance` +0x130 (−1), `yPlane` +0x134 (registrar 0x10031cb0.c:23-37; native-api.md is missing `yPlane`). | `new_shadowEffect(w,h)` → depth FBO w×h (`$shadows`×1024), light camera (gsCamera_OGL) +0x114, blur shader +0x124 | 0x100349e0, 0x10031dd0, 0x10030b00 |
| gsTextureTarget_OGL | 300 B / 0x100341c0 (w,h,cube), 0x10034470 (path) | `renderOffsetX/Y`, `getTexture()`, `resetTexture()` | The output texture is a new w×h texture (cube if requested) or an existing cached texture file. render = nothing (the viewport only draws into the texture). A cube target makes the viewport render 6 faces with a 90° camera. | 0x10031a60, 0x100319b0, 0x10030e60, 0x10044980.c:103-170 |

---

## 3. Texture pipeline

### 3.1 Accepted files (observed, `FUN_10041cd0` / `FUN_10041f30`)

* **Only DDS** ("DDS " magic and a 124-byte header) is loaded by the texture cache. Anything else logs
  *"Invalid texture file: '<path>'"* and yields the default texture (0x10041f30.c:272). No BMP or TGA loader
  exists. BMP and DDS are only written (§3.5).
* FourCC `DXT1` → `GL_COMPRESSED_RGBA_S3TC_DXT1_EXT`, `DXT3` → `…DXT3`, `DXT5` → `…DXT5`. `DXT2`/`DXT4` are
  rejected with *"DXTn texture format is not supported!"* (0x10041cd0.c:49-103).
* Uncompressed formats need RGB bit counts 8/8/8 with alpha 8 → `GL_BGRA`, or alpha 0 → `GL_BGR`. **Only the bit
  counts are checked, not the masks**, so the layout is assumed to be A8R8G8B8/R8G8B8. Everything else is
  invalid (0x10041cd0.c:86-101).
* `DDSCAPS2_CUBEMAP` (caps2 & 0x200) means a cube map with 6 faces in DDS order, uploaded to
  `GL_TEXTURE_CUBE_MAP_POSITIVE_X + i` (0x10041f30.c:180-263).
* Mip count = header `dwMipMapCount` (0 → 1). A short file logs *"Warning: invalid image data"* and stops the mip
  loop (0x10041f30.c:119-143).
* Rows are uploaded as stored (DDS top row = v 0). GUI uv (0,0) is the image's top-left, so a D3D/Vulkan-style
  backend needs no flip.

### 3.2 Pixel formats / internal formats

| Path | Internal format | Notes |
|---|---|---|
| DDS DXT1/3/5 | S3TC compressed (`glCompressedTexImage2DARB`) | block bytes 8 (DXT1) or 16 |
| DDS BGRA/BGR | `glTexImage2D` (internal format argument lost; *inferred* RGBA8/RGB8) | |
| `gsITexture::uploadBGRA` | `GL_RGBA` ← BGRA8 | used by script-created textures |
| Post-effect FBO | RGBA8, or **RGBA16F** for bloom and other float targets | §2.9 |
| Shadow map | depth texture (format lost, *inferred* `DEPTH_COMPONENT`) with an RGB colour RB | compare mode set at bind time |

### 3.3 Mipmap and filtering policy

* **Mip skip**: the loader drops the first `N` mip levels (`level < N` is skipped unless it is the last level) and
  uploads the rest from level 0 (0x10041f30.c:87-91, 145-147). `N` is read from `video+0x1E8` by the cache. See §5.
* **Filtering**: the six `glTexParameter` calls in the DDS loader lost their arguments. The log line prints
  "trilinear" when `anisotropy` == 0 and "Nx anisotropic" otherwise (0x10059200.c:870-880).
  *Inferred*: MIN = `LINEAR_MIPMAP_LINEAR`, MAG = `LINEAR`, WRAP = `REPEAT`, `MAX_ANISOTROPY = video.anisotropy`,
  and `GENERATE_MIPMAP` off (SGIS is required but only used for FBO/upload paths).
* Script uploads (`uploadBGRA`): LINEAR/LINEAR, REPEAT, anisotropy 1, **no mipmaps** (observed).
* FBO textures: mipmaps only when the effect requests them (`glGenerateMipmapEXT` after the resolve). No shipped
  effect does.
* `window.script:33-34`: `anisotropy = $settings.$anisotropy` (default 8) and
  `textureDetail = (anisotropy == 0) ? 1 : 0` ("low quality" means half-resolution textures plus trilinear).

### 3.4 Render targets / FBOs

The only code that touches FBOs is `gsTextureRenderer_OGL` (§2.9), driven by post effects (§2.8/2.12):

* Colour targets: 2D or cube, optional MSAA (renderbuffer + blit resolve), optional depth RB (24-bit), optional
  RGBA16F, optional mipmaps.
* Depth target: the shadow map.
* Sizes only grow (the effect keeps max(viewport size, texture size)). The viewport uses a sub-rect with uv scale
  `size/texSize`.
* Texture targets used by scripts:
  * minimap: `debug.script`/`editor.script` `new_textureTarget(2048,2048,false)` / 256².
  * secondary view: 512×256.
  * animation menu: 256×128.
  * object-list thumbnails: `renderer.script`.
  * env map: `new_textureTarget("textures/envmap.dds")`, commented out.

### 3.5 Readback (observed)

| API | Implementation | Format |
|---|---|---|
| `textureVariant.saveToFile(path)` | `FUN_100413c0`: `.bmp` → 24-bit BMP via `glGetTexImage(GL_BGR)`. Any other extension → **uncompressed DDS** A8R8G8B8 with a full mip chain generated on the CPU by 2×2 box filter from `glGetTexImage(GL_BGRA)` level 0 (cube: 6 faces, level 0 only). | used for minimap thumbnails: `main.script:276` saves `$minimap.$texture` as `.dds` |
| `textureVariant.saveToColormap(cm, x, y, [flip])` | `glGetTexImage(GL_BGR)` copied into a gsColorMap 24-bit buffer at (x,y), optional vertical flip | editor/debug minimap assembly (`editor.script:208`) |
| `gsITexture::readAlpha` | `glGetTexImage(GL_ALPHA)` | caller not traced |
| Bloom luminance | `glGetTexImage(GL_LUMINANCE, GL_FLOAT)` of 64×32 every frame | GPU→CPU stall per frame |
| `window.captureScreen(path)` | **no-op** (slot 65 is empty) | — |
| gsWindow_OGL slot 68 | `glReadPixels(BGR)` → 24-bit BMP of the back buffer | unreachable from script? |

---

## 4. Backend port checklist (2D/output scope)

### 4.1 What a replacement must implement

1. **Window/context**: create, resize (`setSize` sets the client area), title, fullscreen toggle (exclusive mode
   change to width×height×bits@freq, with fallback to windowed), present/swap with vsync, embedding as a child
   window (`parentWindow`; `WM_PAINT` drives a frame). Keep the WndProc input behaviour or route it to the same
   events.
2. **gsVideo factory slots 56–71** (§2.10) returning objects with the exact interface vtables (§2). Engine code
   calls them by slot offset, so **slot order and calling conventions (`__thiscall`, float args on the stack)
   must be preserved**, or the callers must be patched.
3. **Render tree**: parent/child nodes with refcounts, a visible flag and a layer with **stable ascending sort**
   (ties keep insertion order). Root outputs are ordered by `sendToBack`/`bringToFront`.
4. **Viewports**: pixel vs normalised coordinates, sub-rect projection, mirror, depth range, clear colour with
   "alpha 0 = no colour clear", visibleGroups, effect chain, render-to-texture (2D and cube), and nesting of
   viewports in GUI.
5. **GUI items**: clip-area scissor, primitive lists (quads, quad strips, lines, line strips, triangles), materials
   with up to 4 textures plus one shader and per-item shader params (`custom0..`), rotation about a pivot, text
   (§2.5) with measuring functions that **must match the renderer's metrics exactly** (layout depends on them).
6. **Textures**: DDS DXT1/3/5, BGRA8, BGR8, cube maps, mip skip, anisotropy, the default texture fallback, the
   path-keyed cache with refcounts, BGRA upload, readbacks (§3.5) and saving BMP/DDS.
7. **Fonts**: BMFont binary v3 and the native `.font` v3 format, DDS pages, inline colour/reset codes, tab, wrap,
   background highlight, shadow pass.
8. **Post effects**: the bloom (4 shaders, 8 HDR targets, luminance), shadows (depth map + compare, optional
   screen-space blur), shader effect, texture target. All are gated on "shaders" support, and all return null or
   pass through on failure.
9. **Stats and info**: `polyCount`/`submeshCount` per frame, allocated memory counters, `window.mode`/`version`
   strings, `getVideoMemoryUsage`, `getEffectTimer`, `display_modes`, `current_display_mode`, `max_anisotropy`.

### 4.2 Settings to honour (from `defaults/settings.script` → `window.script`/`engine.script`)

| Setting | Engine field | Effect |
|---|---|---|
| `$screenwidth/$screenheight` (0 = auto-pick from `display_modes`) | window width/height | client size |
| `$screencolors` (32) | `screenColors` | colour bits 16 or 24; fullscreen bpp |
| `$frequency` (60) | `frequency` | fullscreen refresh |
| `$fullscreen` (true) | `fullscreen` | exclusive mode change |
| `$vsync` (true) | `vsync` | swap interval |
| `$antialiasing` (4) | `antialiasing` | MSAA samples: pixel format if `postProcess` is off, else FBO MSAA; −2 steps on failure |
| `$bloom` (true) | `postProcess` and `new_bloomEffect` | moves MSAA into FBOs and adds the bloom effect |
| `$anisotropy` (8) | `video.anisotropy` (clamped to the HW max); `textureDetail = anisotropy==0` | filtering and mip skip |
| `$shaders` (true) | `video.shaders` | GLSL; gates effects and skinning |
| `$hardwareSkinning`, `$occlusionQuery`, `$pointSprites` | video flags | capability-gated |
| `$shadows` (2) | `new_shadowEffect($shadows*1024, …)` | shadow map size |
| `$saturation` | bloom `saturation` | colour grading in the final shader |
| `$systemCursor` | `systemCursor` | OS cursor vs hidden |
| `$fpsLimit` (0) | `MAIN.fpsLimit` | sleep-based limiter |
| `$nearPlane`, `$viewdistance`, `$motionBlur` | `video.nearPlane`, viewport `maxDistance`; motionBlur unused | |

### 4.3 Quirks to reproduce, or deliberately fix

* **GUI coordinate system**: origin top-left, y down. The projection is `glOrtho(0, w/h, 1, 0, −1, 1)`, so in
  non-pixel viewports one unit = the **viewport height** (x runs to the aspect ratio). With
  `pixelCoordinates = true` (used by the CiM root GUI and the main viewport) units are pixels. Mouse input is also
  normalised by window height. Text default size 0.015 = 1.5 % of the height.
* **Clip areas don't translate**: children use absolute viewport coordinates, so gsControl precomputes desktop
  positions.
* **Clip rect math**: `left = origin + x`, `right = x + w` (origin omitted), then intersected with the parent.
  This only matters inside nested viewports with a non-zero origin (observed bug).
* **Nested viewports reset the scissor** to their own rect without intersecting the parent clip, then restore the
  parent's GL state afterwards.
* **Layer convention used by gsControl**: image `z·256`, border `z·256+1`, own clip area `z·256+0x80`, text `−2`
  inside its clip area.
* **Back buffer is cleared only on demand** (first frame, `WM_ERASEBKGND`). Frames rely on viewports covering the
  screen. A modern backend should clear every frame to `window.background`.
* **Viewport colour clear is skipped** when the clear colour's alpha is 0. Depth and stencil are always cleared
  even though the pixel format has 16-bit depth and no stencil.
* **Colour byte orders**: ARGB uints everywhere in the API; R/B-swapped for GUI vertices. Clear colours are read
  as bytes [2],[1],[0] = R,G,B.
* **Blend state for GUI**: alpha blending with `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, no alpha test, no depth, colour
  material on, `glColor4f(1,1,1,1)`. Textured and untextured items are distinguished only by whether texture
  unit 0 is enabled.
* **Text**: immediate-mode quads, one unit per font line height `L`, glyph width = `w/L`. The shadow pass is drawn first.
  The background highlight quad is also drawn in the shadow pass (with the shadow offset).
* **Gamma**: no sRGB handling anywhere (no `GL_FRAMEBUFFER_SRGB`, no sRGB formats; 0x8C4D is only checked as a
  block size). Everything is in gamma space. The bloom final shader tone-maps with `(c+m−1)/m`. A new backend
  must use **UNORM, not sRGB** swapchain/textures to match.
* **dt clamp** [0.0001, 0.05] s and the sleep-based `fpsLimit`. Vsync is the only other pacing.
* **Bloom auto-exposure** reads back 64×32 floats every frame. Replace it with a GPU reduction, but keep the
  formula `lightness / (lum·3 + 0.1)` with `lum = sqrt(mean)`, clamped to 1.0 if > 100000 or ≤ 0
  (0x100352a0.c:118-137). `FUN_1011fd74` is sqrt; the 3D pass also calls it on `x²+y²` (0x10044ef0.c:215-216).
* **MSAA**: the default framebuffer has MSAA only without post-processing. With bloom, the scene is rendered into
  an MSAA FBO and resolved with NEAREST blits. GUI is never multisampled in that mode.
* **Texture upload "warm-up" draw** after each DDS load can be dropped.
* `textureVariant.fill()` and `window.captureScreen()` are no-ops in this build.
* Shadow and bloom need FBO, GLSL, depth textures and float targets. When anything fails the effects silently pass
  through, which a backend should mimic.

---

## 5. Open questions

1. **Mip skip source.** The cache passes `*(int*)(video+0x1E8)` (the `anisotropy` *float* field) as the
   mip-skip count (0x100431c0.c:47). Taken literally, anisotropy 8.0 would skip every level but the last.
   Meanwhile `textureDetail` (+0x21C) has no reader in the pseudocode, and `window.script` clearly intends it as
   the skip count. Check the disassembly of 0x100431c0 and 0x10041f30 (parameter order and stack layout look
   mis-decompiled; `unaff_retaddr` appears).
2. The six DDS `glTexParameter` calls and the shadow-map internal format lost their arguments. What are the exact
   filter, wrap and anisotropy values (and LOD bias)?
3. `glViewport`/`glScissor` y-flip arithmetic is lost in every case. The top-left → bottom-left conversion with
   ctx+4 (window height) is assumed.
4. `crossFade(viewport, seconds)`: the timer fields (+0x64..+0x6C) are advanced (0x1004f080.c:26-31) and touched
   in 0x10044980.c:173-178, but no blend draw was found. Is it unfinished?
5. `gsIVideoOutput_OGL` slot 0's real name: the only observed use is choosing a multisampled FBO.
6. The meaning of gsVideoItem flags bits 1/2 (always set by callers), ctx+0x2C, ctx+0x220 and video+0x1FD
   ("first frame"). (The gsGuiText control codes and the colour payload are resolved in
   [docs/ui/fonts.md](../ui/fonts.md) §7.)
7. `gsWindow_OGL` slot 68 (BMP screenshot): reachable from any key or event?
8. Ownership of effects added with `addEffect` (no visible addRef; batch-B also flags this).
9. The callers and purpose of `gsITexture::readAlpha` (slot 2).
10. The `lodFactor` calibration (0x1004e8a0): what exactly is measured and where ctx+0x28 is consumed (the 3D side,
    colleague).
