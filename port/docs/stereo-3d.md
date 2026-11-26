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
| 12 Glasses-free 3D (Leia SR) | Leia SR displays: Samsung Odyssey 3D, Acer SpatialLabs, Lenovo ThinkVision 3D, ... | both eyes side by side, woven by the SR runtime for where the viewer's eyes are (below) |

Without a stored choice (a new player, or the setting never touched) the mode is **12 when an SR display and its
runtime are found**, else off: the game comes up in 3D on such a monitor by itself (owner 2026-11-26: "auto
detection"). The settings page then names the display in place of the 3D TV note.

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

## Glasses-free 3D on Leia SR displays (mode 12) and head tracking

Owner's question (2026-11-23) and request (2026-11-26: "support LeiaSR and auto detection"). The Samsung Odyssey 3D,
Acer SpatialLabs, Sony Spatial Reality and Lenovo ThinkVision 3D displays are **Leia SR** panels: the SR Platform
runtime installed with the monitor (`HKLM\SOFTWARE\Dimenco\Simulated Reality` → `C:\Program Files\LeiaSR\Platform`)
holds a "weaver" that turns two views into the lenticular picture for where the eye tracker predicts the viewer's eyes,
and the "SR Service" that runs the tracker. `engine/leiasr.cpp` drives it directly; no Leia SDK, header or library is
used or shipped. The calling sequence comes from the owner's OdySee project (Odyssey 3D research, runtime 1.36.4).

* **Detection** (`leiaSrDetected`, once): the runtime's DLLs in its `bin` folder, and an active monitor whose EDID id
  (`EnumDisplayDevices`: `MONITOR\SAM7860\...`) is in the runtime's product table (Odyssey 3D: SAM 0xE067-0xE072 and
  0x7859-0x7866; Acer ACR 0x0000-0x0003; Lenovo, Dell, BenQ, ViewSonic, LG, ASUS and the SR laptop panels), or the
  runtime's manual product entry (`...\Simulated Reality\Manual`). `REGS_LEIASR=0` hides it, `=1` claims a display.
  The log names what was found ("Leia SR: runtime found, display Samsung Odyssey 3D (SAM7860)"); the settings page
  gets it through `window.regsGraphicsCaps()` bit 64 and `window.regsLeiaSrDisplay()`.
* **Weaving:** the frame is drawn for both eyes as for side by side; then, instead of `stereoCompose` into the back
  buffer, `leiaSrPresent`:
  * OpenGL: the views side by side (half width each) into a texture, `SR::CreateGLWeaver`'s weaver
    (`SimulatedRealityOpenGL.dll`) weaves it into the window's framebuffer; the device then forgets its GL state.
  * Vulkan: the runtime has no Vulkan weaver. A Direct3D 11 device on the same GPU (by LUID) makes two shared
    textures (NT handles) that the Vulkan device imports (`importD3D11Texture`, `VK_KHR_external_memory_win32`):
    Vulkan composes the views into the first (rows turned to Direct3D's order) and waits (`finishGpu`),
    `SR::CreateDX11Weaver`'s weaver (`SimulatedRealityDirectX.dll`) weaves it into the second, D3D11 flushes and
    waits for an event query, and Vulkan copies the second into its back buffer (rows turned back).
  * The weaver knows the window (HWND) and weaves for its place on the screen, windowed or fullscreen (runtime ≥ 1.34).
  * Start order as in the SDK's examples: `SR::SRContext::create` (non-blocking client: fails at once without the
    service), the weaver, then `SRContext::initialize`. The runtime's interfaces are declared in `leiasr.cpp` with the
    runtime's virtual method order; the factories are found by their exported names.
  * The panel's lens: a "switchable lens hint" through the runtime's C functions (`createSwitchableLensHint`,
    `lensEnableHint`) while the mode runs, released when it ends or the game closes, so the panel goes back to 2D. On
    the Odyssey, Samsung's Odyssey 3D Hub turns the lens request (or the weaving) into the monitor's 3D switch.
  * When the runtime cannot start (no service, no weaver), the reason is logged once, that frame shows side by side,
    and the mode is off until it is chosen again.
* **Eye tracking:** the weaver's predicted eye positions (mm, display coordinates) are the head tracking's source while
  glasses-free 3D runs (head tracking on): the middle of both eyes, as the OpenTrack packets give it otherwise.
* **Cost:** the frame is drawn twice as for every stereo mode; under Vulkan each frame also waits for the GPU twice
  (before and after the D3D11 weave).
* HDR with glasses-free 3D: the views are handed over in 8 bits (highlights above paper white are clipped).
* **Without the runtime**, the side-by-side mode (1) with [StereoLoom](https://github.com/effcol/SR-Loom) (MIT, tray
  app) still works: it captures the game's window and weaves it.
* **Head tracking** (settings `$regsHeadTracking` on / off, `$regsHeadTrackingStrength` 0 .. 200 %, image quality page
  under the stereo rows; `REGS_HEADTRACK=0|1`): in glasses-free 3D from the SR eye tracker (above); otherwise
  `engine/headtrack.cpp` listens for OpenTrack's "UDP over network"
  packets on 127.0.0.1:4242 (48 bytes: six little-endian doubles, x, y, z cm, yaw, pitch, roll degrees) as StereoLoom
  sends the SR display's head pose, and opentrack does from any tracker (webcam, TrackIR clip, phone). The map view's
  camera turns about the point it looks at: sideways by atan(x / 60 cm) × strength about the world's up axis, up and
  down by atan(y / 60 cm) × strength (at most 15°, never below 3° above the horizon or past straight down), so moving
  the head looks around the town, in 2D and in stereo. Lightly smoothed; without packets for half a second it eases
  back to the middle. Only the drawn view turns: the game's camera, and so clicking, stays as it is (a head held far
  to the side points a little beside what it shows). Loopback only, so nothing comes from the network and Windows
  asks no firewall question.

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
`--at N:head:x,y` sets a head offset (cm) as if OpenTrack had sent it; `opencim-tests\stereo\opentrack_send.py x y s`
sends real packets;
`REGS_START=quality` opens the advanced settings page over the main menu (screenshots of the page).

Checked 2026-11-23 (`opencim-tests\stereo\stereo.ps1`: campaign, every mode in turn, Vulkan and OpenGL): all nine modes
compose as described (rows, columns and checkerboard alternate per pixel), the interface has no parallax, the map's
disparity is +8 px at the top of the default view (behind the screen), 0 at the look point and −8 px nearer (in
front). The settings page shows the section with the stored values, and the mode list opens over the page.
Miniature (`opencim-tests\stereo\mini.ps1`, same view): the top of the picture 54-56 px apart instead of 8-10, the
lower part −20 to −24 px instead of −12 to −14; red/green has no blue, red/blue no green.
