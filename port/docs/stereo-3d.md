# Stereoscopic 3D

Owner's request (2026-11-23): stereoscopic 3D in the graphics settings, with several modes such as side by side.

## For players

Settings > Graphics > Advanced settings, section **Stereoscopic 3D** (applies on OK, while the game runs):

| Row | Setting | Values |
|---|---|---|
| 3D mode | `$regsStereo` | drop-down list, see below (default off) |
| Depth | `$regsStereoDepth` | 0 .. 100 % (default 50): how far apart the eyes' pictures of the horizon are; 100 % = 5 % of the picture's width |
| Screen plane | `$regsStereoConvergence` | 50 .. 200 % (default 100) of the camera's distance to the point it looks at: that distance lies on the screen, nearer things come out of it, farther ones go behind it. Higher: more comes out |
| Swap eyes | `$regsStereoSwap` | off / on: left and right exchanged (cross-eyed viewing, rows starting on the other line, glasses the other way round) |
| Miniature effect | `$regsStereoMiniature` | 0 .. 200 % (default 100): the eyes as far apart as a person's over a model on a table (100 %), so the town looks like a model; the screen plane goes to the middle of what is in view and the depth setting caps both ends (1.5 × its distance). Looking down on the town that is about seven times the plain depth; toward the horizon it closes in. 0 = the plain depth (screen plane at the look point, the horizon at the depth setting). Owner 2026-11-23: "treated like a miniature to really have a 3D effect" |

| Mode | For | Picture |
|---|---|---|
| 1 Side by side | 3D TVs and projectors ("Side by Side" / "SBS" on the device) | left eye in the left half, right eye in the right, each squeezed to half width; the device stretches them back |
| 2 Top and bottom | 3D TVs and projectors ("Top and Bottom", "Over-Under") | left eye on top, right eye below, each half height |
| 3 Interleaved rows | passive 3D monitors with polarised glasses (LG Cinema 3D, Zalman, ...) | the top row and every second one the left eye's; in fullscreen, so the rows meet the panel's (else "swap eyes") |
| 4 Interleaved columns | autostereoscopic panels with column barriers | every second column |
| 5 Checkerboard | DLP 3D TVs and projectors | the pixels alternate like a chessboard, the top-left one the left eye's |
| 6 Anaglyph red/cyan | red/cyan paper or plastic glasses | Dubois' least-squares colours (less ghosting and truer colours than plain channel splitting) |
| 7 Anaglyph green/magenta | green/magenta glasses (TriOviz) | Dubois |
| 8 Anaglyph amber/blue | amber/blue glasses (ColorCode 3-D) | Dubois |
| 9 Anaglyph red/cyan grey | red/cyan glasses | in grey: no colour, the least ghosting |
| 10 Anaglyph red/green | old red/green glasses | in grey (red and green filters pass no third colour) |
| 11 Anaglyph red/blue | old red/blue glasses | in grey |

* The interface is drawn in both eyes' pictures at the same place, so it lies on the screen plane; the map is in depth
  around it. The game's own pointer (the default, "system cursor" off) is part of the interface and so appears in both
  halves of side by side / top and bottom; the Windows pointer (system cursor on) only at its real place.
* While stereo is on, TAA, render scale, FSR, raytraced ambient occlusion and frame generation are off (they keep a
  history from frame to frame, which two cameras would mix up). Everything else (bloom, shadows, colour styles,
  raytraced shadows and reflections, HDR) works in both eyes.
* **Cost:** the frame is drawn twice, so expect a little over half the frame rate (not measured yet: on 2026-11-23
  other test runs had the machine; mono was 121 fps at the campaign start, Vulkan, RTX 3080, 1920x1080, vsync off).
* Not offered: frame-sequential output for active shutter glasses (needs quad-buffered stereo, which consumer drivers
  no longer offer for Vulkan or OpenGL) and full-width side by side (each eye at half width with its own layout).

## How it works

* `engine/stereo.cpp`; settings read every frame by `stereoFrameMode` (`REGS_STEREO=<mode>`,
  `REGS_STEREO_DEPTH`, `REGS_STEREO_CONVERGENCE`, `REGS_STEREO_SWAP`, `REGS_STEREO_MINIATURE` override them).
* `Display::renderFrame` draws the whole frame, views and GUI, twice: left eye, copy of the back buffer into an eye
  target (`stereoKeepEye`), right eye, copy, then `stereoCompose` draws both into the back buffer with
  `regs/shaders/stereo.fragment`. The second pass begins without a new device frame (`Renderer2D::beginFrame(...,
  deviceFrame = false)`: no second swapchain image). The screenshot, the pop-out views, the HDR encode and the present
  come after, once.
* The eyes' cameras (`stereoCamera`, called by `renderViewportScene` for on-screen views): this is real stereo, the
  scene drawn from two viewpoints (not a depth effect laid over one picture). The eye and its look point move
  sideways along the camera's right axis by b, both looking the same way (parallel axes, no keystone), and the
  projection is sheared by c = b·P00 / D (x_clip += c·w_clip), so a point at the screen-plane distance D (camera
  distance to its look point × screen plane setting) lands where the game camera has it. A point at view depth z then
  lies b·P00·(1/D − 1/z) picture widths apart between the eyes (positive: behind the screen).
  * Plain depth: b = B·D / P00, B = depth setting × 5 %: the horizon gets B.
  * Miniature: b = miniature × 6.5 % of the camera distance (100 % = eyes 13 % of the distance apart, 6.5 cm at
    50 cm over a model). The ground under the picture's top edge (farthest, the camera has no roll; infinite when the
    horizon shows) and its bottom edge (15 % nearer, for what stands on it), from the camera's height over its look
    point, pitch and field of view, give the depth range in view: D goes to its middle (1/D = the mean of 1/near and
    1/far, × the screen plane setting) and b is cut so neither end lies more than 1.5 × B apart.
  Culling uses each eye's matrices; level of detail, billboards, sky and the shadow fit keep the game camera's own
  eye, so both pictures hold the same things and the same shadow map.
* Once a frame, not per eye: the camera's easing (`updateFloatingCamera`), bloom's adaptation (`bloomTick`), the
  frame-rate readout's measurement. Temporal passes are skipped (`temporalBegin` not called), frame generation is
  switched off when a mode starts.
* Side by side and top and bottom sample the eye's frame between two texels, so linear filtering averages both (a
  2:1 box filter). The interleaved modes count pixels from the top left. The eye targets are RGBA16F in HDR.
* Pop-out views stay mono.

## Tests

`--at N:stereo:<mode>` switches the mode during a run (then the settings are not read for it any more);
`REGS_START=quality` opens the advanced settings page over the main menu (screenshots of the page).

Checked 2026-11-23 (`opencim-tests\stereo\stereo.ps1`: campaign, every mode in turn, Vulkan and OpenGL): all nine modes
compose as described (rows, columns and checkerboard alternate per pixel), the interface has no parallax, the map's
disparity is +8 px at the top of the default view (behind the screen), 0 at the look point and −8 px nearer (in
front). The settings page shows the section with the stored values, and the mode list opens over the page.
Miniature (`opencim-tests\stereo\mini.ps1`, same view): the top of the picture 54-56 px apart instead of 8-10, the
lower part −20 to −24 px instead of −12 to −14; red/green has no blue, red/blue no green.
