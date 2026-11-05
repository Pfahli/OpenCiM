# UI scale (port feature)

Scope: the port's UI scale for high-resolution screens (setting `$regsUiScale`: 1×, 1.5×, 2×). The original game has
no such option: its GUI is laid out in window pixels, so on a 4K screen everything is tiny. Nothing here changes a
game file; the option lives on the extended graphics page (`port/data/regs/extend/metro/ui/menus/graphics.script`).
Code: `Display::chooseUiScale` / `renderFrame` / `renderTarget` (`port/src/engine/display.cpp`),
`renderViewportScene` (`scene3d.cpp`), the GUI shader and wide lines in `render/renderer2d.cpp`, DPI handling in
`platform/window_win32.cpp`.

## 1. The scripts' window

The game's scripts size everything from the window: `window.script` makes the root GUI viewport
`.pixelCoordinates = true; .setRect(0, 0, $window.width, $window.height)` and the desktop, overlays and fade layers
the viewport's size; `viewport.script` does the same for the 3D map viewport; every menu, panel and HUD element is
then placed in those pixels (`$desktop.width`, `$desktop.height`, fixed sizes such as the 1000 × 600 graphics page).

With a scale `s` the port reports `window.width/height = round(real size / s)` after `window.init()` (the only
scripts reading them are window.script and viewport.script). So the scripts lay out the GUI exactly as for a window
of that size: at 2× a 3200 × 1800 window gets the 1600 × 900 layout. The settings keep the real resolution
(`$screenwidth`/`$screenheight` are set before `init()`), so the graphics page and display modes are unaffected.

**Choice** (`chooseUiScale`, when the window opens): the setting snapped to 1, 1.5 or 2; unset: 2 on windows taller
than 1800 px (4K-class), else 1. While the scripts' window would be smaller than **1024 × 720** the scale steps down
by 0.5 (logged): the main menu's buttons run into its bottom links below about 660 script pixels, and the graphics
page is 1000 wide. So 1.5× needs at least 1536 × 1080, 2× at least 2048 × 1440 (1080p → 1.5×, 1440p and 4K → 2×).

**Applies on OK** (since the live display settings, §6): the scripts build menus and HUD once from the window size;
the engine lays them out again for the new size. The page writes `$regsUiScale` only when the player picks another
value, so an unset setting stays automatic (the page shows the scale in use, `window.regsUiScale()`).

## 2. Drawing: script pixels × s

* **Root viewports in pixel mode** (the GUI and the 3D map): window rect = script rect × s. The GUI projection maps
  the viewport's script size onto that rect (`setProjection(rect × s, units = script size)`), and the GUI pass's
  `unit` (window pixels per GUI unit, used for scissor rects) is s. Viewports in height units keep the real window
  height as their unit (debug views only).
* **3D views** (`renderViewportScene`): pixel-mode viewports, on screen or nested in the GUI, render at
  `desktop rect × s` window pixels, so the map, the menu trams and anything else keep the full resolution. Bloom,
  shadows and the style effects get those real sizes.
* **Render targets** (`gsTextureTarget_OGL`: the corner preview, the vehicle shop's 3D previews, the menu trams): the
  framebuffer is `texture size × s` and the view inside it `viewport size × s`. The scripts' crop and tiling rects are
  fractions of the texture (`setCropRect(0, h/256 − 1, w/512, −h/256)` in secondaryview.script, `setTiling(1, −1)`
  in objectlist/renderer.script), so they stay right while each texel is one window pixel.
* **Minimap**: its texture is the 256-cell map raster sampled by `minimap.shader` on the scaled quad: the map itself
  is drawn at full resolution. The camera footprint lines are drawn `s` pixels wide (`Renderer2D::lineWidth`: lines
  become quads when wider than 1 px; core profiles only guarantee 1-px GL lines).
* **FPS readout** (`regsShowFps`): drawn in script pixels with the same projection, so it scales too.
* **Mouse cursor**: the game draws its own pointer, a GUI control (`metro/ui/mouse`), so it scales with the GUI. The
  system cursor (`$systemCursor`) is the OS's and follows the OS's own size.

## 3. Input and picking

The mouse needs no extra mapping: the window tick hands the desktop the pointer as a fraction of the real window
(`pollMouse`: `x / realWidth`), and `gsDesktop.mousePos` multiplies it by the desktop's script size
(`DesktopObj::inputMousePos`). The desktop pointer is therefore `real pixel / s` (a float), hit tests run in script
pixels, and `--at` actions keep giving real window pixels.

3D picking (`pickSegment`: hover, selection, tools, `getWorldPosition`) and the projected overlays (`placeTracer`:
pins and bubbles over vehicles and stops; `placeMapPin`, the minimap footprint) work in the viewport's normalised
coordinates `(x − left) / width`, which are the same in script pixels and in window pixels. Checked in game: a stop
placed with the build tool at 1×, 1.5× and 2× lands under the cursor and in the same place on the map.

## 4. Text and images

The game's fonts are bitmaps (`docs/ui/fonts.md` §1): native `.font` cell tables with one 256² or 512² page each
(tahoma-8 and default-9 at 15 px line height, blair-18 34 px, verdana 38 px) and BMFont extras for ja/pl/ru at
17–39 px. None is vector or SDF, and no larger sizes ship: the UI text (tahoma-8, size 15) is drawn 1:1 at 1×, so at
2× each glyph texel covers 2 × 2 pixels.

GUI textures are sampled with **sharp bilinear** when `s > 1` (`sampleGui` in renderer2d.cpp's fragment shader):
per axis, with `k` = window pixels per texel (from the derivatives), each texel keeps a flat core and blends with its
neighbour over about one pixel (`region = 0.5 − 0.5/k`). This applies only to texels the scale magnified,
`k < 1.25 · s`; textures stretched far more (backgrounds, gradients) and minified ones (mipmapped fonts such as
verdana) keep plain bilinear, as at 1×. At 1× the filter is off and drawing is unchanged.

| Scale | Plain bilinear (`REGS_UI_FILTER=linear`) | Sharp bilinear (default) |
|---|---|---|
| 2× | soft, blurred glyph edges | exact 2 × 2 copy of each texel: identical to the 1× frame doubled (checked pixel for pixel on the main menu's buttons) |
| 1.5× | soft | crisp; some strokes one pixel wider than others (a 2-1-2 pattern), seams blended |

3D views and render targets are not affected (their texels already match the window's pixels).

## 5. DPI

`regs.exe` is per-monitor DPI aware (v2) from the first line of `main()` and again in `Win32Window::create` for other
hosts, so Windows never bitmap-stretches the window; the log line `display: 144 dpi (150 %), per-monitor DPI aware`
confirms it. Moving the window to a monitor with another DPI keeps its pixel size (`WM_GETDPISCALEDSIZE` returns the
current size, `WM_DPICHANGED` is ignored): the resolution and UI scale were chosen at start.

## 6. Display changes while the game runs (port)

The graphics page applies fullscreen, resolution and UI scale on OK (`graphics.script` extension → native
`window.regsApplyDisplay(fullscreen, width, height, true)`, `Display::applyDisplay`):

1. **Window**: the same HWND gets a new style and size (`Window::setMode`: borderless over its monitor, or a captioned
   window with the resolution as client size, centred when leaving fullscreen). The pixel format and the GL context
   stay, so every texture, font page, mesh, program and VAO stays valid. The pixel format carries no MSAA any more:
   views without post effects draw into a multisampled RGBA8 target (`msaaTarget`) resolved into the back buffer,
   so the antialiasing setting can change too.
2. **Scale and targets**: the UI scale is chosen again for the new size (`chooseUiScale`). A new scale drops the
   render-target textures (corner preview, shop previews, menu trams; made again at texture size × scale on next use);
   a new window size drops the bloom scene, style and MSAA targets (`resetSizedEffects`). The shadow map does not
   depend on the window.
3. **GUI** (only when the scripts' window size changes; `window.width/height` then report the new size):
   `relayoutGui` (`engine/gui_relayout.cpp`) walks the root viewports that fill the scripts' window (the GUI viewport
   of window.script, the 3D view of viewport.script) and every control in them. Per axis, against the container's old
   client extent: a control spanning it (flush with both edges, or equal margins of at most 64 px and at least half
   the extent) grows with it; one centred on it (within 1 px) stays centred with the scripts' `int((W − w) · 0.5)`
   rounding; one nearer the far edge than the near edge moves with the far edge; others stay. Every HUD formula of the
   game is one of these (minimap and its icons `h − 300`/`h − 125`, data panel `h − 171`, corner preview `w − 342`,
   line panel `w − 357`, toolbar `450 + (w − 900 − tw)/2` = centred, dialogs and the graphics page centred, the
   desktop, overlays, fade, player and HUD layers and the edge-scroll strips spanning) except four, which re-place
   themselves on the desktop's `onChangeRect` (port extensions, with `window.regsSetRect`, which lays a resized
   control's children out by the same rules): the level bar (top and height from `clamp(h − 768, 0, 80)` and
   `clamp(h − 381, 339, 375)`, button spacing from its height), the money bar (`min(59, w − 1010)`), the petition bar
   (width from `w`) and the line panel (height `min(900, h) − 228`, its line and stop lists half each, with the
   script's one-pixel frame tweaks). A move alone fires no `onChangeRect` (ui.script's `createHudElement` handler
   would reset a customised `$content`, e.g. the minimap's icon column); a resize lays out the children first and
   then fires it.
4. **Menus**: the main and pause menus place their buttons from the height in ways the rules cannot follow
   (`max(270, h/2 − 190)`), so `main.script`'s extension (`regsRelayoutMenus`) loads the open menu again and performs
   `back`, `settings`, `graphics`: the player lands on the graphics page again, laid out for the new size, with the
   game, its camera and open panels kept (the main menu's music restarts).

Checked in game (Vienna, `port/run/livegfx/s3-*.png`): 1600 × 900 → 1280 × 720 → 1600 × 1200 → 1.5× (1067 × 800)
→ fullscreen 3840 × 2160 at 1.5× → 1600 × 900; the HUD at 1600 × 900 after the round trip matches the start, and
1280 × 720 with the line panel open matches a fresh start at 1280 × 720. Not exact: the debug timebar of the editor
(visible only with `$debug`), whose year, month and day bars split the width proportionally.
