# Upscaled textures (port feature)

Sharper textures for OpenCiM: every texture of the player's own install gets a 2× level made on their machine,
switched on and off on the graphics page ("Schärfere Texturen (2x hochskaliert)" / "Sharper textures (2x
upscaled)", setting `$regsTextureUpscale`). Nothing from the game is shipped or committed, upscaled or not; the
copies live in a cache below the player's OpenCiM folder.

Code: `port/src/engine/texture_upscale.{h,cpp}` (filter, block compression, cache files, generator),
`port/src/engine/display.cpp` (`Display::loadTextureImage`, `syncTextureUpscale`), `Renderer2D::createTexture`
and its `reuse` parameter (`port/src/render/renderer2d.cpp`), `port/src/tools/texture_tools.cpp` (regs-tool commands), the graphics page
extension `port/data/regs/extend/metro/ui/menus/graphics.script`.

## 1. What the game's textures are

The install (game and DLC, as the port mounts them) holds 5 301 DDS textures:

| Kind | Typical | Format | Notes |
|---|---|---|---|
| building, vehicle and prop diffuse maps | 512² (DLC: 1024²) atlases | DXT1, DXT5 when the alpha is a colourise mask | a whole building's facade, roof and details share one 512² atlas: about 64 texels per storey |
| normal maps (`_n`) | as the diffuse | DXT5, tangent-space RGB, alpha holds a separate mask | the shader uses `normalize(TBN * (rgb - 0.5))` |
| specular, illumination, ambient-occlusion masks (`_s`, `_i`, `_ao`) | as the diffuse | DXT1 | night lights read the specular maps from the install, not from the GPU |
| alpha-tested foliage, fences, rails | 256–512² | DXT1 with 1-bit alpha, DXT5 | the 3D shaders discard below alpha 0.2 (shadow pass) or 0.001 |
| terrain tiles (`metro/graphics`) | 512–1024², tiled | DXT1/DXT5 | repeat across the map, so the filter must wrap |
| UI and HUD art, portraits, location illustrations | any size, often not powers of two | BGRA8 without mipmaps, some DXT | drawn at UI scale 1, 1.5 or 2 |
| fonts, sky, particles, envmap (cube), lookup ramps | | | left alone (§3) |

All 3D textures have full mipmap chains.

## 2. Where a 2× texture can show at all

A texture only looks sharper where the GPU *magnifies* it, i.e. where one texel covers more than one screen pixel.
Where it is minified it samples the mipmaps, and a bigger level 0 changes nothing.

* At 1600×900 the closest city camera already sees most surfaces slightly minified (a storey is ~64 texels and ~45
  pixels high). Measured: switching the upscaled textures on changes 9 % of the pixels of the Vienna start view,
  by 0.6/255 on average; the visible gain is limited to the closest surfaces.
* At 3840×2160 (the owner's screen, 150 % DPI) each screen pixel covers less of the texture, so most near surfaces
  are magnified: in four Vienna views 46–68 % of the pixels change, by 1.4–5.9/255 on average, and facades, roof
  tiles, window frames and paving are visibly crisper (§8). With the automatic UI scale 2× the HUD art is magnified
  too.

So the feature is worth most at high resolutions and in close-ups, which is where the original looks blurry.

## 3. Options compared

| | Quality | Speed (all 5 300 textures) | Setup for a player | Licence |
|---|---|---|---|---|
| **AI upscalers run locally**: Real-ESRGAN (x4plus, animevideo-v3) as the ncnn-Vulkan build, other ESRGAN models, waifu2x, chaiNNer pipelines | best on photographic diffuse maps: invents plausible detail (brick, tile, grain); can also invent wrong detail, smear text and logos, and is unpredictable on atlases with hard part borders; useless or harmful on normal maps and masks (they must stay data) | measured with x4plus: ~1 s per texture on an RTX 3080, so roughly an hour for the install's colour and UI textures (§8.1); minutes per texture on CPU | a separate download of the tool and its model weights (Real-ESRGAN Windows build ~45 MB); a Vulkan GPU | Real-ESRGAN code and its official weights BSD-3-Clause, the ncnn-Vulkan wrapper MIT, waifu2x MIT; community ESRGAN models each have their own licence, several are non-commercial (CC BY-NC-SA) and cannot be bundled; chaiNNer is GPL-3 |
| **Classical filter offline** (chosen): Lanczos-3 with anti-ringing clamp, contrast-adaptive sharpening, per-kind handling | crisper edges and joints, no new detail; never invents anything; DXT block grain gets slightly more visible | 3 min with 11 threads on a 6-core i5-11600K (171–176 s measured for 5 071 textures, DXT compression included); in the game, with half the threads, about 6 min | none: built into regs.exe, made in the background the first time the option is on | the port's own code (CAS after AMD FidelityFX CAS, MIT) |
| xBR / ScaleFX / HQx | made for pixel art: on photographic textures they posterise and draw false edges | fast | none | MIT / public domain |
| **Runtime shader**: sharpening (CAS) or bicubic sampling per texture fetch | similar to the classical filter at best; sharpens the DXT noise at every pixel every frame; the mip chain stays blurry; every material shader (including the game's own combiner shaders and mod hooks) would need the change | costs every frame (one to four extra taps per texture sample) | none | own code |
| **FSR-style frame upscaling** | upscales the rendered frame, not the textures: it helps when rendering below screen resolution and adds nothing to texture detail at native resolution | per frame | none | MIT |

Decision:

1. **Built-in classical filter, made offline into a cache, used as an extra top level.** It works for every player
   without downloads, is deterministic, licence-clean, quick enough to run in the background while they play, and
   keeps every texture kind valid (normals renormalised, masks unsharpened, 1-bit alpha crisp).
2. **An AI upscaler can be plugged in** for the colour textures (`regs-tool upscale-textures … --external`, §6).
   The port does not ship one; players who want the extra detail point it at their own copy. Normal maps, masks and
   alpha always come from the built-in filter.
3. No runtime shader: it would cost every frame, touch every shader, and look no better than the cached filter.

## 4. Per texture kind

The kind comes from the materials first: every `.material` and `.object` of the install is read once (3 s) and its
texture slots give diffuse (alpha-tested when the material is), specular, normal, illumination and AO maps. Textures
no material names (script materials: terrain, roads, texture variations) are sorted by folder and suffix
(`texup::classify`).

| Kind | Filter | Why |
|---|---|---|
| colour / diffuse, terrain | Lanczos-3 2×, clamped to the 2×2 source texels around each output texel (no ringing), then CAS 0.6 on RGB; wraps at the edges; alpha scaled as data and colours under alpha 0 kept | crisper without halos; terrain tiles stay seamless because the filter wraps like the sampler; the alpha of plain diffuse maps is often a colourise mask (liveries, roofs) with real colour under alpha 0 |
| alpha-tested (foliage, fences) | as colour; colours bled into the transparent texels first (DXT1 stores black there); DXT1 1-bit alpha re-thresholded at 0.5 after scaling | no dark fringes, and the cut-out edge gets the new resolution instead of 2× staircase steps |
| normal maps | Lanczos-3 clamped, no sharpening, RGB renormalised (z ≥ 0) when the source texels are unit vectors (mean deviation ≤ 0.1; a few `_n` files hold something else); alpha (a mask) scaled as a mask | stays a valid unit normal; sharpening normals makes lighting sparkle |
| specular / illumination / AO masks | Lanczos-3 clamped, no sharpening | masks are data; night lights read the originals anyway |
| UI art, portraits, illustrations | Lanczos-3 clamped, no wrap, CAS 0.35; the original kept as level 1 | at UI scale 1 the GPU picks level 1 (the original, exactly), at 1.5/2 the upscaled level |
| skipped | fonts (bitmap glyph pages), cube maps (envmap), sky, particles, effects, `textures/`, saved minimaps, anything under 16 px on a side (ramps, lookups) or over 1024 px (already sharp; 4× VRAM again) | 230 textures |

Tileable atlases with parts side by side (building atlases) bleed at most half a source texel across part borders
because of the 2×2 clamp, the same as the GPU's bilinear filter already does.

## 5. DDS, mipmaps, compression and VRAM

* **Levels**: the cached file is the source with one level added on top. Level 0 is the 2× image, levels 1.. are the
  source's own levels, byte for byte. Wherever the original was minified the GPU samples the very same texels as
  before (far views are identical, alpha-test coverage in the distance cannot change), and only one new level has to
  be made and compressed. Sources without mipmaps (UI) get the original as level 1.
* **Format**: the same as the source (DXT1 stays DXT1, DXT3/DXT5 stay, BGRA8/BGR8 stay), so the existing loader and
  both renderers take the files unchanged. The new level is compressed by the port's own encoder (principal axis,
  inset, three least-squares refits, and for near-flat blocks the two 565 colours around the mean so dark panels keep
  their hue; DXT1 punch-through blocks where alpha < 128; DXT5 alpha by min/max).
  Recompressing at 2× costs little: each original block becomes four.
* **VRAM**: a texture with full mips grows from 4/3 to 16/3 of its level 0, i.e. 4×. Measured in the Vienna start
  view at 3840×2160: §8. BC7 would look better at the same size but needs a new format path in both renderers;
  left for later.
* **Disk**: 3.4 GB for the whole install (the US, Tokyo and London DLC are 2 GB of it: their atlases are 1024²).

## 6. Cache, generator and the setting

* **Where**: `<documents>\OpenCiM\texture-cache\x2\<game path>` (`texup::cacheRoot`), e.g.
  `…\texture-cache\x2\metro\objects\buildings\tenements\tenement01.dds`. Test runs with `--documents` use their
  stand-in folder.
* **Validity**: each cache file carries `RGSU`, the cache version and a 64-bit FNV-1a stamp of the exact source bytes
  in the DDS header's reserved words. The game uses a cached file only when the stamp matches the file it just read
  from the install; a patched or changed texture shows its original until it is made again. `complete.txt` records
  the install's signature (archive names, sizes, times); a bump of `kCacheVersion` (filter changes) invalidates
  every file.
* **In the game**: with the option on, `Display::texture` uploads the cached file when there is one. Switching the
  option (OK on the graphics page, applied live) uploads every loaded texture again into the same GL name
  (`Renderer2D::createTexture(img, reuse)`), from the cache (on) or from the original file (off: the very upload the port did
  before, so the look is exactly the original). If the cache is not complete, a background thread (half the cores)
  makes the missing files, the textures the game has loaded first; finished textures are swapped in a few per frame.
  The graphics page shows "Texturen werden vorbereitet: n %" meanwhile. Quitting stops the thread after the current
  texture; the next start carries on (finished files are kept, only unchanged ones are skipped).
* **regs-tool** (for developers and players who want an AI model):

```powershell
# the whole cache (what the game makes in the background), all cores
regs-tool upscale-textures $game "$env:USERPROFILE\Documents\OpenCiM\texture-cache\x2"
# options: --only <path prefix>, --force (make everything again), --no-ui, --threads <n>
# with an external upscaler for the colour textures: it is run once per 256 textures with a folder of PNGs ({in})
# and must write PNGs of the same names into {out}, at any scale (4× results are boxed down to 2×), e.g.
regs-tool upscale-textures $game <cache> --force --external "C:\tools\realesrgan\realesrgan-ncnn-vulkan.exe -i {in} -o {out} -m C:\tools\realesrgan\models -n realesrgan-x4plus -s 4"
# one texture: GPU-style bilinear 2× | cached upscale, side by side
regs-tool upscale-compare $game metro/objects/buildings/tenements/tenement01.dds cmp.png [x y size]
```

## 7. Test aids

* `REGS_TEXTURE_UPSCALE=0|1` overrides the setting (before/after runs).
* `REGS_FRAME_STATS=<from>:<to>` logs the frame times between two frames, the bytes of the uploaded file textures
  (and how many are upscaled) and the driver's video memory in use (NVIDIA `GL_NVX_gpu_memory_info`).
* `regs-tool png-diff a.png b.png [x y w h]`: share of differing pixels, mean difference and mean colours.
* `regs-tool ddspng <file.dds> <out.png> [level]`: one level of a DDS file on disk (a cache file) as PNG.
* `regs-tool upscale-check $game <cache> [prefix]`: every cached texture's 2× level boxed back down against the
  source's level 0, per channel (UI colour weighted by alpha). It found the two bugs fixed before the first commit:
  colour bled under colourise masks (roofs and bus liveries lost their tint) and renormalised `_n` maps that are not
  normal maps. Now: mean 1.24/1.24/1.27/0.48 (RGBA, out of 255) over the 5 071 textures.

## 8. Measurements

Machine: i5-11600K (6 cores), RTX 3080 10 GB, NVIDIA 621.01, Windows 11; Vienna sandbox save, 3840×2160 fullscreen,
UI scale 2×, antialiasing 4×, anisotropic 8×, vsync off from frame 1460, frames 1500–2100 of the start view.

| | upscaling off | upscaling on |
|---|---|---|
| frame time, mean (quiet runs) | 7.31, 7.41 ms | 7.30, 7.42, 7.60 ms |
| frame time, median | 7.08, 7.11 ms | 7.07, 7.09, 7.15 ms |
| file textures loaded (upscaled) | 773 (0) | 774 (750) |
| their GPU bytes | 205 MB | 722 MB |
| video memory in use, whole system (NVX) | 2 449 MB | 2 992–2 996 MB |

* No frame-time cost within the run-to-run noise. Some runs of both kinds came out near 10 ms (off 9.94, on 9.99
  twice) because of other load on the machine; they are left out of the table.
* +0.5 GB of video memory for the city view at 4K; a 4 GB card has room, a 2 GB card may not.
* Making the cache: 171–176 s for all 5 071 textures with regs-tool (11 threads); 3.4 GB on disk. In the game the
  background thread uses half the cores (150 textures in the first ~10 s of a menu session).
* Switching live: 39 menu textures in 142 ms (on, read from disk) and 32 ms (off); a city with ~770 loaded textures
  takes about 3 s to switch on (estimate from the per-texture time), once, when the player presses OK.
* Pixels changed by switching on (start view): 9 % at 1600×900 (mean 0.6/255); 46–68 % at 3840×2160 in four views
  (mean 1.4–5.9/255). Mean colours of whole frames stay within 0.3/255.

Before/after crops (3840×2160 shown at 2×, off left, on right) were sent to the owner on 2026-11-17: market square
roofs, palace facade, city block with parked cars, a park, the HUD. Not in the repository (they show game art).

### 8.1 Real-ESRGAN tried (2026-11-17)

With the owner's OK, `realesrgan-ncnn-vulkan-20220424-windows.zip` (v0.2.5.0, 45.5 MB, SHA-256 `abc02804…412802d`) was
run through `--external` from a scratch folder (never in the repository) on the base game's 3D object and terrain
textures (`metro/objects/`, `metro/graphics/`).

* **realesrgan-x4plus** (4×, boxed down to 2×): 518 colour textures in about 9 minutes on the RTX 3080 (~1 s each;
  the whole install's colour and UI textures would take roughly an hour). It removes the DXT block noise and draws
  very clean edges (window frames, roof tiles), but it also wipes out the painted grime and grain the game's art
  relies on: facades become smooth plaster, paving loses its texture, grass turns painterly. In the 4K views it
  differs from the built-in filter by 1.9–5.9/255 on average (34–64 % of the pixels); whole-frame colours stay
  within 1/255. A "remastered" look rather than a sharper original.
* **realesr-animevideov3** (2×): every run ended in `VK_ERROR_DEVICE_LOST` on this machine (also with `-t 128`) and
  wrote black images.
* That broken run led to a safeguard in the generator: an external result whose colours differ from the built-in
  result by more than 24/255 on average is rejected (logged, built-in used). It also caught three small UI previews
  where x4plus shifted the colours.

Decision unchanged: the built-in filter is the default and the only thing the game makes; AI stays an optional
generator mode for players who like the cleaner look. The cache was restored to the built-in filter afterwards.

### 8.2 Real-CUGAN and waifu2x tried (2026-11-17)

Owner's OK for two more tools, unpacked in a scratch folder only and run only on PNGs exported from the install:
`realcugan-ncnn-vulkan-20220728-windows.zip` (43.8 MB, MIT, SHA-256 `c6e08d46…def400e01`) and
`waifu2x-ncnn-vulkan-20250915-windows.zip` (33.9 MB, MIT, SHA-256 `7425be94…5adfbdc87`), both from nihui's GitHub
releases. Six textures went through `--external` (building `tenement01`, `tram_04`, `foliage-final`, `terrain`,
`paving_checker`, the UI icon `bus01-icon`) and were compared texel for texel against the built-in filter and x4plus.

| upscaler (2×) | 6 textures | look |
|---|---|---|
| built-in | 5 s in total incl. the material scan | keeps grain and grime, slightly crisper |
| Real-ESRGAN x4plus | 8.3 s | smooth; the grass terrain becomes a flat green surface |
| Real-CUGAN se, no-denoise (`-n 0`) | 1.4 s | invents worm-shaped blobs on grass, a yellow fringe on the tram's top edge, smooths facades; its UI icon shifted colours and was rejected (28/255) |
| Real-CUGAN se, conservative (`-n -1`) | 1.3 s | softer, smears the grime |
| waifu2x CUnet, no denoise (`-n -1`) | 1.2 s | crispest edges (paving diagonals, window frames) and keeps the grain, but **unreliable here**: the same tram PNG came back fine once, dark blue (62/255 off, rejected) three times and all black once |
| waifu2x upconv_7 photo, noise 0 | 1.0 s | closest to the built-in result with slightly cleaner edges and lettering; no failures in these runs |

(Timings include each tool's model load of ~0.5 s; the per-texture cost is well under 0.2 s for the 2× models.)

The anime-trained models (CUGAN, CUnet) read the game's photo-sourced grain as noise or line art. waifu2x photo is the
only one that adds sharpness without changing the art, but the difference from the built-in filter is small, too
small to justify a download for players. Decision unchanged; the safeguard (§8.1) proved necessary again, catching
the broken CUnet output and the CUGAN icon.

## 9. Later

* An in-game choice "built-in / AI" would need the port to fetch or bundle Real-ESRGAN (BSD-3 code and weights, MIT
  wrapper: bundling is allowed) and a fallback for GPUs where it fails like animevideov3 did here.
* BC7 for the new level (both renderers), 4× for the 512² building atlases at 4K, a lighter cache by skipping DLC the
  player does not own or play.
* The Vulkan backend needs the same `createTexture(img, reuse)` (re-specify a texture under the same handle) and the
  NVX memory query (`Renderer2D::videoMemoryInUseMB`, measurements only).
