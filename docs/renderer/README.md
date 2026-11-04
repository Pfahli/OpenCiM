# GSystem renderer: the contract for a new graphics backend

The game never calls OpenGL directly from gameplay code. All rendering goes through abstract C++ interfaces (`gsI*`)
whose only implementations are OpenGL classes (`*_OGL`). A replacement backend implements the same interfaces.

| Document | Covers |
|---|---|
| this file | interface inventory, OpenGL feature surface, extensions |
| [interfaces-3d.md](interfaces-3d.md) | scene, mesh, sub-mesh, skeleton, camera, light, primitive, particles, material, shader: every method, plus the shader contract |
| [interfaces-2d-output.md](interfaces-2d-output.md) | viewport, clip areas, GUI primitives and text, textures, post effects, render targets, window/context setup and the frame order |
| [renderer-map.json](renderer-map.json) | machine-made map: every interface slot → implementing function → GL calls (`tools/renderer-map.mjs`) |
| [scene-basics.md](scene-basics.md) | worked example (main-menu trams): quaternions, fixed camera, object transforms, sun/light/material values, fog, the expanded default-shader GLSL, texture-target rendering and GUI composite |
| [grid-items.md](grid-items.md) | map items (roads, tracks, buildings, props, trees): `loadGridObject` templates, item transform, variations and tints, merged division meshes vs. per-item instances, LOD, foliage, what is skipped, port checklist |
| [terrain.md](terrain.md) | map terrain: divisions, LODs, per-vertex attributes, material groups (terrain/yard/field/water/edges), terrain/field/water shaders, port checklist |
| [shadows.md](shadows.md) | sun shadow map (`gsShadowEffect_OGL`): script fields, light-camera fit to the view footprint, trapezoid warp (TSM), `shadowTransform`/`shadowWarp`/`shadowParams`, depth pass and receiver shader math, worked example, port checklist |
| [bloom.md](bloom.md) | bloom / "HDR lighting" post effect: script fields and defaults, per-tick exposure adaptation, render targets, the scene pass and the 10 bloom passes with exact `custom0` and uv mapping, luminance readback, final composite and tone map, bloom on vs off, quirks, port checklist |

## Interfaces and implementations

Interfaces were found through RTTI; the base offsets in each `*_OGL` class identify which vtable implements which
interface. Slot counts include inherited base slots. `gsIVideoItem` and `gsIVideoElement` are the common bases and
have no vtable of their own.

| Interface | Slots | OpenGL implementation |
|---|---|---|
| gsIScene | 4 | gsScene_OGL |
| gsIMesh | 26 | gsMesh_OGL |
| gsISubMesh | 4 | gsSubMesh_OGL, gsHWSkinSubMesh_OGL (hardware skinning) |
| gsISkeleton | 9 | gsSkeleton_OGL |
| gsICamera | 9 | gsCamera_OGL |
| gsILight | 7 | gsLight_OGL |
| gsIPrimitive | 6 | gsPrimitive_OGL |
| gsIParticleSystem | 8 | gsParticleSystem_OGL |
| gsIMaterial | 25 | gsMaterial_OGL |
| gsIShader | 12 | gsShaderProgram_OGL (base gsShaderProgramBase_OGL) |
| gsITexture / gsITextureContainer | 4 / 1 | gsTexture_OGL / gsTextureVariant_OGL |
| gsIClipArea | 5 | gsClipArea_OGL, gsViewport_OGL |
| gsIViewport | 19 | gsViewport_OGL |
| gsIGuiVideoItem | 19 | gsGuiPrimitive_OGL, gsGuiText_OGL |
| gsIGuiPrimitive | 8 | gsGuiPrimitive_OGL |
| gsIGuiText | 18 | gsGuiText_OGL |
| gsIVideoOutput_OGL | 2 | gsViewport_OGL and the post effects gsShadowEffect_OGL, gsBloomEffect_OGL, gsShaderEffect_OGL, gsTextureTarget_OGL |

Back-end singletons (script-visible modules): gsVideo_OGL (factory and owner of every video item), gsWindow_OGL
(window and WGL context), gsTextures_OGL, gsShaders_OGL, gsShaderCombiner_OGL, gsFonts_OGL and
gsTextureRenderer_OGL.

## OpenGL feature surface (116 functions)

The engine uses **116 distinct GL/WGL functions**. 58 of them are loaded by name at start-up with
`wglGetProcAddress`; the rest are imported from `opengl32.dll`. Counts are call sites in the decompiled code
(`renderer-map.json` → `glUsage`).

| Technique | Functions (call sites) | Modern equivalent |
|---|---|---|
| Fixed-function transform | `glMatrixMode` 22, `glLoadIdentity` 21, `glLoadMatrixf` 8, `glOrtho` 8, push/pop, translate/scale | uniform matrices |
| Fixed-function lighting/fog | `glLightfv` 6, `glLightf` 3, `glLightModelfv`, `glMaterialfv` 24, `glMaterialf`, `glColorMaterial` 3, `glFog*` 5 | shader lighting/fog |
| Immediate mode (GUI, overlays) | `glBegin` 7 / `glEnd` 9, `glVertex2f` 10, `glVertex3f/fv` 9, `glTexCoord2f/fv` 15, `glColor4f/ub` 11 | dynamic vertex buffers |
| Vertex arrays | `glVertexPointer`, `glNormalPointer` 3, `glColorPointer` 2, `glTexCoordPointer` 8, `glInterleavedArrays` 5, client states, `glDrawArrays` 6, `glDrawElements` 1 | vertex/index buffers |
| Buffer objects (`ARB_vertex_buffer_object`) | `glGenBuffersARB`, `glBindBufferARB` 13, `glBufferDataARB` 4, `glMapBufferARB`/`glUnmapBufferARB` | buffers |
| Shaders (`ARB_shader_objects`, GLSL 1.10) | create/compile/attach/link, `glUseProgramObjectARB`, `glGetUniformLocationARB` 27, `glUniform*ARB` 22, `glUniformMatrix3fvARB`/`4fvARB` (bone matrices), `glBindAttribLocationARB`, vertex-attrib arrays | shader modules + descriptor sets |
| Multitexture | `glActiveTexture` 41, `glClientActiveTexture` 11, `glMultiTexCoord2f` | descriptor bindings |
| Textures | `glGenTextures`, `glBindTexture` 32, `glTexImage2D` 11, `glCompressedTexImage2DARB` 2 (S3TC/DXT), `glTexParameteri/f/fv` 36, `glGenerateMipmapEXT` 2, `glGetTexImage` 6 (readback) | images + samplers |
| Render targets (`EXT_framebuffer_object`) | framebuffers 34, renderbuffers 16, `glRenderbufferStorageMultisampleEXT` 2 + `glBlitFramebufferEXT` (MSAA resolve), `glDrawBuffer` 4 | render passes / attachments |
| Occlusion queries (`ARB_occlusion_query`) | gen/begin/end/get/delete, 3 sites each | queries |
| Point sprites (`ARB_point_parameters`) | `glPointParameterfARB/fvARB` | instanced quads |
| Raster state | `glEnable` 66 / `glDisable` 103, `glBlendFunc` 17, `glAlphaFunc` 11 (alpha test), `glDepthMask` 8, `glDepthFunc`, `glCullFace`, `glPolygonOffset` 5 (decals, shadows), `glScissor` 5, `glViewport` 6, `glClear*` | pipeline state objects |
| Readback | `glReadPixels` 1 (screenshots) | copy to buffer |
| Context (WGL) | `wglCreateContext`, `wglMakeCurrent` 10, `wglChoosePixelFormatARB` (multisampled pixel formats), `wglSwapIntervalEXT` (vsync), `wglGetExtensionsStringARB` | swapchain |

**Capability strings checked at start-up:**
* `GL_ARB_shader_objects`, `GL_ARB_vertex_shader`, `GL_ARB_fragment_shader`, `GL_ARB_shading_language_100`
  (GLSL; the engine reports *"Cannot initialize shadows / bloom / post-processing (requires GLSL)"* without it);
* `GL_ARB_vertex_program`, `GL_ARB_multitexture`, `GL_ARB_vertex_buffer_object`, `GL_EXT_framebuffer_object`;
* `GL_ARB_texture_compression`, `GL_EXT_texture_compression_s3tc`, `GL_SGIS_generate_mipmap`;
* `GL_ARB_occlusion_query`, `GL_ARB_point_parameters`, `GL_ARB_point_sprite`;
* `WGL_ARB_pixel_format`, `WGL_EXT_swap_control`.

Which of these are required and which degrade gracefully is in [interfaces-2d-output.md](interfaces-2d-output.md).

## The frame

Reconstructed by the 2D/output reader; details and function addresses are in
[interfaces-2d-output.md](interfaces-2d-output.md).

1. **Tick:** `gsMainLoop` → loop body `FUN_10082da0` clamps the frame time to 0.0001–0.05 s, pumps window messages,
   then runs script threads and ticks every module.
2. **Update:** the window ticks all children (video, viewports, controls). All updates finish before any rendering.
3. **Render:** for each top-level viewport, in list order:
   1. each post effect runs its off-screen pass (shadow map, bloom into an HDR framebuffer);
   2. the 3D scene renders:
      * GL lights (point lights, no attenuation) and linear fog are set up;
      * objects are culled with an octree plus a per-object frustum test, and each mesh picks one of 4 LOD levels;
      * sub-meshes go into **four queues**: opaque (sorted by shader program and textures), glow (wrapped in
        occlusion queries), alpha-tested, and blended. The last two are drawn far to near;
      * glow flares are drawn afterwards as 2D sprites;
   3. the last effect composites the result, then the GUI is drawn.
4. **Present:** `SwapBuffers`, then a sleep-based frame-rate limiter (`fpsLimit`).

## Key behaviours a backend must reproduce

**3D** (from [interfaces-3d.md](interfaces-3d.md))
* **GLSL is effectively mandatory.** With shaders off, no mesh is ever queued for drawing.
* **Skinning is GPU-only.** Skinned meshes are split into bone groups whose size comes from the shader's bone array:
  `bones[48]` or `bones[8]` in the default shader, `bones[56]` in the bump shader. There is no CPU fallback.
* **The shaders read fixed-function state.** The 81 shipped GLSL files use `gl_FrontMaterial` 37×, `gl_Fog` 30×,
  `gl_ModelViewProjectionMatrix` 27×, `gl_NormalMatrix` 18×, `gl_LightSource` 11× and `gl_ModelViewMatrix` 8×.
  A modern port must pass all of these as explicit uniforms.
* **Vertex data:** VBOs bound through fixed-function arrays, with tangents, skin weights and bone indices on
  texture-coordinate units 2–4.
* **Texture units and uniforms:** material slot *i* maps to texture unit *i* in loader order (diffuse, specular,
  normal, illumination, ambient occlusion, environment cube); unit 7 is the shadow map. There are 13 engine
  uniforms: `texture0`–`7`, `matrix`, `eyepos`, `timer`, `custom0`–`7` and others.
* **Engine quirks a port must reproduce or fix:**
  * the camera's default field of view is stored in radians but read as degrees;
  * the screen-space shadow lookup is hard-coded for 1600×900;
  * alpha-tested geometry is also alpha-blended;
  * the light count is not capped at `GL_MAX_LIGHTS`;
  * reading occlusion query results stalls the CPU.

**2D and output** (from [interfaces-2d-output.md](interfaces-2d-output.md))
* **GUI coordinates:** origin top-left, y down. One unit equals the viewport height (`glOrtho(0, w/h, 1, 0, −1, 1)`)
  unless a viewport uses pixel coordinates.
* **Clipping:** clip areas only scissor; they do not translate their children.
* **Clearing:** a viewport clear colour with alpha 0 skips the colour clear.
* **Colour:** no sRGB anywhere, so use UNORM (not sRGB) surfaces. Colours are `0xAARRGGBB`, with red and blue
  swapped for GUI vertices.
* **Textures:** only DDS loads (DXT1/3/5, BGRA8, BGR8, cube maps). Saving writes BMP or uncompressed DDS.
* **Missing features fail silently.** Without GLSL the effect constructors return null; without framebuffer
  objects, post effects pass the scene through unchanged and texture targets render nothing.
* **Bugs in the game:** `window.captureScreen()` is an empty vtable slot, and bloom reads 64×32 floats back from
  the GPU every frame for auto-exposure, which stalls the pipeline.

## Verification

Claims re-checked against the code, independently of the readers:

| Claim | Check |
|---|---|
| Frame time clamped to 0.0001–0.05 s, then an fps-limit wait | `0x10082da0.c` |
| GUI projection `glOrtho(0, w/h, 1, 0, −1, 1)` | `0x100447e0.c:13` |
| Bone matrices are uploaded with `glUniformMatrix3fvARB`/`4fvARB` | `0x10055db0.c:64-67`; added to the map, now 58 extensions |
| Shaders depend on fixed-function built-ins | counted in all 81 shipped GLSL files (table above) |
| Bone group sizes 8 / 48 | `shaders/default/geometry.vertex:9-11`; the bump shader uses 56 |
| Texture code reads `gsVideo_OGL.anisotropy` (`+0x1E8`) where a mip-skip count is expected, and nothing reads `textureDetail` (`+0x21C`) | the upload function `0x10059200` reads `+0x1E8` once and `+0x21C` never. **Open:** decompiler mislabel or real bug; needs the raw instructions |

## What this means for a port

* **The seam is narrow.** Gameplay and scripts talk to `gsVideo`/`gsWindow` modules and the `gsI*` interfaces,
  so a new backend replaces the `*_OGL` classes, about 20 classes and the methods listed in the two interface
  documents, without touching simulation code.
* **Most work is replacing fixed-function state.** Lighting, fog, matrix stacks, alpha test and immediate-mode GUI
  drawing need to become shaders and buffers. The asset data itself (36-byte vertices, `u32` indices, DXT textures)
  already suits modern APIs.
* **Shaders are data.** GLSL sources ship as text files (`shaders/`, `metro/shaders/`) and are compiled at run time,
  so a port can translate them, for example through SPIR-V cross-compilation, or replace them.
