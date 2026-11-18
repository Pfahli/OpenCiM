# Smooth camera (owner's request 2026-11-17)

The game's camera (`metro/scripts/ui.script`) turns in 45-degree steps on the rotate keys and zooms by a factor of 1.2
per wheel notch, both at once. The port adds a smooth camera, on by default, with two options on the **Game** page
of the settings (`regs/ui/game.script`, section "Camera"):

| Setting | Default | What it does |
|---------|---------|--------------|
| `$regsSmoothCamera` "Smooth camera" | on | Turning and zoom without steps, easing into place; touch and touchpad gestures. Off: the game's steps. |
| `$regsCameraSnap` "Snap to the main directions" | on | A free turn that ends within 15 degrees of one of the eight main directions glides into it, and slow turns slow down a little while passing one (a magnet). Off: the camera stays where it is left. **Shift** held at any time during a turn turns without snapping or magnet (owner's request 2026-11-17): a rotate key press then turns 45 degrees from where the camera is, a held key stops where it is let go. |

## Controls with the smooth camera

| Input | Turns | Zooms |
|-------|-------|-------|
| Rotate keys (Left/Right) | a press: one main direction on (45 degrees), gliding; held: keeps turning, 35 up to 100 degrees a second; let go: on to the next main direction ahead (snapping) or stops | |
| Zoom keys (Q/E) | | the game's 1.2 per press, gliding |
| Mouse wheel over the world | | fractions of a notch, gliding (a notch = the game's 1.2) |
| Ctrl + middle mouse drag | a quarter of a degree per pixel, 1:1 | |
| Touchpad: two fingers up/down, pinch | | as the wheel (Windows sends a pinch as Ctrl + wheel), stepless |
| Touchpad: two fingers sideways | 11.25 degrees per notch of sideways scroll, gliding | |
| Touchpad: twist two fingers | follows the fingers, after 8 degrees (so scrolls and pinches don't turn) | |
| Touchscreen: two fingers | twist turns, pinch zooms about the fingers' centre, moving them moves the map, all 1:1 | yes |

One finger on a touchscreen stays a mouse (clicks, tools, the game's panning with a held button). A turn by fingers or
mouse glides on a little when released, then snaps if it is near a main direction.

## How it works

* **Engine** (`engine/camera_input.cpp`): sees every OS event before the game (`cameraInputEvent`, called from
  `dispatchInput`). With the smooth camera on, it takes the wheel over the world (the desktop's hover control is
  `MAIN.$player` or inside it; Shift + wheel stays the place tool's turn), the sideways wheel, two-finger gestures and
  Ctrl + middle drags, and collects them; it also tracks held keys. Natives: `regsSmoothCamera(on)`,
  `regsCameraInput()` (the amounts since the last call: zoom eased/direct as ln of the distance factor, turn
  eased/direct in radians with the map counterclockwise positive, pan in pixels, the pivot, released turns, a drag in
  progress), `regsKeyHeld(name)`.
* **Window** (`platform/window_win32.cpp`): `WM_MOUSEHWHEEL`; `WM_GESTURE` for touchscreens (zoom, rotate, two-finger
  pan; single-finger pan is left to the mouse emulation, `SetGestureConfig`); precision touchpad contacts as raw HID
  input for the twist (`platform/touchpad_win32.cpp`, usage page 0x0D / touch pad 0x05, contact id, tip, X/Y per link
  collection, hybrid reports by contact count). New `InputEvent` types `MouseHWheel` and `Gesture`.
* **Script** (`regs/extend/metro/scripts/ui.script`, the gameUI extension): `$cameraDir` becomes a fraction (0..8, 45
  degrees per unit), which every user in the game's scripts already treats as an angle (`$cameraDir * 0.25 * PI`). A
  "gameUI" thread takes the input each frame, keeps goals for direction and zoom, eases towards them
  (1 - e^(-12 dt)), follows direct gestures at once, and applies through the game's own `setCameraZoom` (which sets the
  orientation from `$cameraDir`). `setCameraDirection` / `setCameraZoom` are wrapped: the rotate and zoom keys' calls
  (one step from the current value) become goals; other calls (loading, tests) jump as before. A pinch keeps the ground
  under the fingers' centre in place (the camera moves by the pivot's offset from the screen centre times 1 - factor).
* `REGS_DEBUG_CAMERA=1` logs a line per frame the camera moves.

## Tests

`--at` actions `hwheel:<notches>` and `gesture:zoom,<factor>` / `gesture:turn,<radians>` / `gesture:pan,<dx>,<dy>` /
`gesture:end` (at the mouse position) feed the same events. Checked (Vienna sandbox save and the editor test map):
a twist turns the map counterclockwise and snaps on release, the wheel zooms by fractions (4 x 0.25 notches = one
factor 1.2), a held rotate key turns continuously and snaps ahead on release, the sideways wheel and Ctrl + middle drag
turn and snap, a pinch zooms about its centre, a two-finger pan moves the map; with the option off the keys and wheel
step as in the original and gestures do nothing. Not checked on real hardware: a touchscreen and a precision
touchpad's twist (only simulated events).

## Far view and sky (2026-11-19)

The game's far plane is 160 + zoom units (`setCameraPos`), made for its own fixed tilt. Tilted toward the horizon
(the controller's or the gestures' tilt), the top of the view meets the ground much farther away or not at all, and
the town ended in fog a few hundred units off. `viewDepthRange` (engine/scene3d.cpp) now reaches the ground under the
top edge of the view (+15 %), up to 4000 units at the horizon, and only where that is past the game's own far plane,
so the game's own views keep their far plane and fog exactly; the fog is stretched by the same factor.

Above the horizon the game showed only the viewport's dark background (its sky is switched off in engine.script). The
port draws its own dome there (`addPortSky`, engine/sky.cpp, with regs/shaders/sky.shader): blue by day, grey and
closed over under the weather mod's rain or snow (read from the scene's atmosphere: `regsLightDesaturate`,
`regsFogScale`, `regsLightScale`), a warm horizon toward a low sun, stars and the moon at night (the moon where the
day/night cycle's night light comes from), drifting clouds shaded from the sun's side, and the scene's fog colour
along the horizon so the fogged town meets the sky. `REGS_NO_SKY=1` leaves it out.
