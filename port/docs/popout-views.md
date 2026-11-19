# Pop-out views

Owner's request (2026-11-19): pop-out windows that can be placed anywhere (another screen), with scrolling and camera
moves of their own while they have the focus, either fullscreen or as a resizable window.

## For players

* **Opening:** F7 in the game, or the new top button in the icon column beside the minimap (a window with an arrow).
  Each press opens one more view ("OpenCiM – View 2", "View 3", ...). Only while a map is shown.
* **Where it opens:** the first time in the middle of another monitor at three quarters of its size (beside the game's
  window when there is only one). After that each view reopens where it was last closed (`<OpenCiM folder>\popouts.cfg`),
  as long as that place is still on a monitor.
* **Window** (like the setup program, owner's request 2026-11-19): no Windows title bar, round corners with the system's
  thin edge (Windows 11; a rounded region on Windows 10), and two round buttons at the top right: **fullscreen** (four
  corners; blue under the pointer; in fullscreen its corners point inward and it goes back to the window) and **close**
  (the cross; red under the pointer). Drag the **band along the top** (it darkens under the pointer) to move the window,
  double-click it to maximise or go back; drag the **edges and corners** to resize. **F11** or **Alt+Enter** also
  switch to fullscreen on the monitor it is on, **Esc** leaves fullscreen.
* Moving and resizing are done by the game itself, not by Windows' own move/size loop: that loop holds the program
  until the mouse is let go, so both windows froze while dragging (owner: "the window is severely lagging"). The price:
  no snapping to the screen's halves while dragging (Win + arrow keys still snap).
* **Camera** (only while the view's window has the focus; the game's camera is not touched):

| Input | Does |
|---|---|
| Left or middle drag | moves the map with the pointer |
| Right drag | turns about the point in the middle (sideways), tilts (up and down) |
| Mouse wheel, + / − | zoom |
| W A S D, arrow keys | move (Shift: faster) |
| Q / E | turn |
| R / F, Page Up / Page Down | tilt |
| Home | back to where the game camera is looking |
| F7 | one more view |

A view starts where the game camera looks, and starts there again when another map is loaded. In the menus it shows a
dark picture until a game is shown again.

**What it shows:** the map as the game view has it, with sky, fog, weather, day and night, night lights, particles and
shadows (MSAA as set), but without the interface and without the passes only the main view has (bloom, TAA/render
scale/FSR, the colour styles, raytraced shadows and reflections). **Cost:** the map is drawn once more per view; one
1920x1080 view beside a 1920x1080 game window took the game from 147 to 96 fps (Vulkan, RTX 3080, median frame 6.2 →
9.4 ms).

## How it works

* `engine/popout.cpp` keeps the views: an OS window each (`platform::Window` with `WindowDesc::secondary` and
  `resizable`), an orbit camera (point on the ground, heading, tilt, distance; eased toward its goals like the smooth
  camera) and a scene target of the window's size. `popoutsPump` (main loop, after the game window's events) handles
  each view's own events; `popoutsRender` (`Display::renderFrame`, after the GUI) draws them.
* Drawing: `renderViewportScene(d, vm, mapViewport, &ViewOverride)` (`engine/scene3d.cpp`) draws the map viewport's
  scene again with the view's eye, point and planes (near a tenth of the distance, far 160 + distance and out to the
  ground under the top edge, the fog stretched against the main view's far plane) into the view's target. The game's
  own camera object and its easing are left alone. `lastMapViewport()` is the on-screen viewport that drew a grid in
  the last frame.
* Showing: `gpu::Device::addWindow` / `showInWindow` / `removeWindow`. Vulkan: a surface and swapchain per view
  (mailbox, else immediate, so a view never waits a vertical blank of its own); the picture is blitted onto an acquired
  image in the frame's commands and presented in the same `vkQueuePresentKHR` as the main image. OpenGL: the view's
  window gets the main window's pixel format and no context of its own; the one context is made current on it, the
  target blitted into its back buffer, swapped (swap interval 0) and the main window made current again.
* Focus: a window losing the focus to another window of the game lets go of the keys held in it (their key-up messages
  would go to the other window), so held arrow keys in the game do not scroll on.
* The borderless window (`WindowDesc::borderless`, `platform/window_win32.cpp`): a sizing frame style without a caption,
  `WM_NCCALCSIZE` gives the whole window to the client, every hit test is client; the window class itself finds the
  band, edges and buttons (`zoneAt`, `buttonAt`), sets the sizing cursors, moves and sizes the window from its mouse
  messages, and sends `Close` / `ToggleFullscreen` events for the buttons. `Window::chrome` / `chromeHot` tell the
  engine where the buttons are and what the pointer is on; `drawChrome` (`engine/popout.cpp`) draws the band's shade
  and the buttons (pictures made in code at the window's DPI) over the view through `Renderer2D::beginTarget`. Round
  corners: `DWMWA_WINDOW_CORNER_PREFERENCE` as in the setup program, square in fullscreen and maximised.

## Tests

`REGS_POPOUT=<n>` opens n views by themselves once a map is shown (`REGS_POPOUT_RECT=x,y,w,h` places the first). `--at`
actions: `popout` (one more view), `popshot:<file.png>` (the first view's next picture), `popkey:<vk>`,
`popkeydown:<vk>`, `popkeyup:<vk>`, `popwheel:<notches>`, `popmove:x,y`, `popdown:<button>`, `popup:<button>` (input
of the first view's window; in scripted runs real input to the views is ignored as for the game window),
`popmsg:<msg>,<wparam>,<x>,<y>` (a Win32 message handled by the first view's window as if Windows had sent it, then a
log line with its place, size, state and what the pointer is on; posting real messages from the test sandbox is
blocked by Windows).

Checked 2026-11-19 with the campaign driver (Vulkan and OpenGL): the view starts on the game camera's picture, turns,
zooms, tilts and pans by its own input while the game view stays, goes fullscreen (3840x2160 on the 4K screen) and back,
the minimap button opens another view on the other monitor, the window on screen shows the picture, its close box closes
it and its place is saved. Borderless window (popmsg): the band moves the window by the pointer's way, the right edge
resizes it, the hover states show, the fullscreen button goes to 3840x2160 and back to the same place, a double-click
on the band maximises to the work area (3840x2088) and back, the close button closes; a screen capture shows the
round corners.

Not done yet: views of something else than the map (minimap, panels, a vehicle to follow), the interface (tools,
selection) inside a view, a view's frame on the minimap.
