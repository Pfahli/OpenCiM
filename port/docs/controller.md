# Controller (gamepad) support (owner's request 2026-11-18; Steam Deck and Steam Controller 2026-11-18)

The game can be played with an Xbox- or PlayStation-style controller on Windows, alongside the mouse and keyboard,
which keep working unchanged at the same time. The pad does not add a second UI: it moves a virtual mouse cursor and
presses the mouse buttons and the game's own keys, so every menu, panel, dialog and construction tool works as it
does with a mouse. The camera gets the sticks and triggers through the smooth camera ([camera.md](camera.md)).

## Buttons

| Input | Does |
|-------|------|
| Left stick | Moves the cursor: a response curve for fine aiming, faster after a moment pushed fully (up to 2.4×), 70 % speed over panels and menus. Pushed against a screen edge, the map pans that way. |
| LB (L1) + left stick | Pans the map; the cursor stays where it is. |
| Right stick | Sideways turns the camera (stick right = the rotate-right key); up and down tilts it (up = towards the horizon). |
| R3 (click the right stick) | Tilt back to the game's own angle. |
| LT / RT (L2 / R2) | Zoom out / in. |
| A (Cross) | Left mouse button at the cursor; hold to drag (tracks, roads, sliders, windows). |
| B (Circle) | Right mouse button (turns an object being placed, stops a running drag). Held for half a second: the selection tool (leaves any tool). |
| Start (Options) | Esc: the pause menu in a game, closes dialogs. |
| Y (Triangle) | Construction menu (the `constructMenu` key). |
| X (Square) | Bulldozer (`bulldozeTool`). |
| View / Back (Create, touchpad click) | Underground view (`undergroundView`). |
| RB (R1) | Pause / resume (`pause`). |
| D-pad left / right | Game speed down / up (left from speed 1: pause). |
| D-pad up / down | Line panel / vehicle panel (`linePanel`, `vehiclePanel`). |
| L3 (click the left stick) | Selection tool (`selectionTool`). |

The keyed buttons press whatever key the player bound to that action on the controls page (`controls.keys`, else the
defaults), so rebinding a key moves the pad with it; an action without a key does nothing on the pad. With the smooth
camera off, the pad's zoom and turn go in the game's steps (1.2 per zoom step, 45 degrees per turn); pan and tilt work
either way.

## Steam Deck and Steam Controller

Both have touchpads. The port gives them the jobs Valve's own templates give them in strategy and building games:

| Input | Steam Deck | Steam Controller | Does |
|-------|------------|------------------|------|
| Right touchpad | R pad | right pad | The cursor like a trackball: 1:1 with the finger (half the pad = 0.6 window heights × the cursor speed), gliding on after a swipe (slowing over a fifth of a second); a click stops the glide. |
| Right pad click | yes | yes | Left mouse button (hold to drag). |
| Left touchpad | L pad | left pad | Drags the map: it follows the finger and glides on after a swipe. |
| Left grip + left touchpad | L4 + L pad | left grip + left pad | Turns (sideways) and tilts (up and down) the camera; the pad's click sets the tilt back. |
| Right grip | R4 | right grip | Right mouse button (turn an object while the thumb stays on the right pad). |
| L5 / R5 | free | – | Free for players' own Steam layouts. |
| Sticks, face buttons, shoulders, triggers, d-pad | as the Xbox table | stick = left stick; left pad clicked at its edge = d-pad | as the Xbox table |

The Steam Controller has one stick (the cursor); the camera turns and tilts with the left grip and the left pad.
The legend on the settings page shows each layout's own glyphs (Deck: A/B/X/Y, L1/R1, L2/R2, View, Menu, L4/R4/L5/R5,
the pads; Steam Controller: LB/RB, LT/RT, the arrow buttons beside the Steam button, the grips, the round pads).

### Under Steam (the usual case): Steam Input

When Steam runs, Valve's pads always go through Steam Input (Valve's own pads cannot opt out). Steam gives the game an
XInput pad plus the mouse: the right pad moves the system mouse, which the port follows like a real mouse (the
virtual cursor continues from it). The port sees which pad it is from Steam's `SteamDeck=1` environment variable on a
Deck, or a Steam Controller plugged in, and shows that layout's glyphs. Recommended layout for OpenCiM added to Steam
as a non-Steam game (Steam > OpenCiM > Controller settings):

* Template **Gamepad with Mouse Trackpad** (Deck and Steam Controller): sticks, buttons and triggers stay the gamepad
  the port maps.
* Right trackpad: **As Mouse**, trackball on, click = **Left Mouse Click** (the template's default).
* Left trackpad: **Scroll Wheel** with horizontal scrolling on: vertical swipes zoom and sideways swipes turn the
  camera through the smooth camera's wheel (fractions of a notch, docs/camera.md); click = **Middle Mouse Button**,
  so holding it while moving the right pad drags the map (the game's own panning).
* L4 (Deck) / left grip: **Middle Mouse Button** (the same pan with a grip); R4 / right grip: **Right Mouse Click**.
* L5 / R5: free, e.g. keyboard Space (pause) and U (underground view).

Steam keeps such layouts per player in its own data, keyed by the shortcut's generated id, so the port cannot ship a
file Steam would pick up. The way to share one is a community layout published from Steam's configurator (a step
for a player with the hardware). Steam's action manifests (`game_actions`) belong to the Steamworks Steam Input API,
which needs the Steamworks SDK and a Steam app id. For a non-Steam shortcut it is not usable (only Valve's test
app 480 for development), so the port does not use it. It becomes the right tool if OpenCiM is ever released on Steam.

### Without Steam: Valve HID

With Steam not running (Windows on a Steam Deck, a Steam Controller on a PC without Steam), the port reads the pads
itself (`platform/steam_hid_win32.cpp`, `hid.dll` + SetupAPI, no download): the vendor HID interface (usage page
0xFF00) of 28DE:1205 (Deck), 28DE:1102 (Steam Controller, wired) and 28DE:1142 (its wireless receiver), read on a
background thread (overlapped `ReadFile`, 100 ms waits). Opening the pad turns its "lizard mode" off (the
firmware's own mouse on the right pad and keys on the buttons, which would double the input), and closing it turns
lizard mode back on:

* Off: feature report 0x81 (clear the digital mappings) and 0x87 (settings: smooth absolute mouse 0, left and right
  trackpad mode 7 = none). The Deck turns lizard mode on again by itself when the host goes quiet, so they are sent
  again every two seconds.
* On: 0x85 (default digital mappings) and 0x8E (default settings). If the port crashes, the Deck recovers by
  itself; a Steam Controller keeps lizard mode off until it is switched off and on again, or until Steam starts.
* Reports: Deck type 0x09 (buttons: 32 + 32 bits; pads x/y; triggers 0..32767; both sticks; pad pressure, which with
  the touch bits says "a finger is on the pad"); Steam Controller type 0x01 (24 button bits; triggers 0..255; the
  stick and the left pad share their fields, told apart by the touch and stick bits).
* Rules: only while the controller is on, the window has the focus, no XInput pad is there (Steam Input's included)
  and Steam is not running (checked every few seconds; when Steam starts, the port lets go). Never in scripted runs.
  `REGS_VALVE_HID=0` turns the native path off, `=1` uses it even with Steam running (for tests: Steam and the port
  then both talk to the pad). `REGS_VALVE_PIDS=1302:deck,1303:sc` adds product ids of newer pads that speak the Deck
  or the Steam Controller protocol.
* The new Steam Controller (2026): under Steam Input it works like the others (XInput and mouse; glyphs fall back to
  Xbox unless it is recognised). Its USB product id and protocol were not known here; if it speaks the Deck's
  protocol, `REGS_VALVE_PIDS` (or one line in `knownPads`) adds it.

### SteamOS / Linux later

A native Linux build would read pads through SDL2/SDL3's game-controller API, which has HIDAPI drivers for the Deck
and the Steam Controller including their touchpads (`SDL_GetGamepadTouchpadFinger`). It would get access to
hidraw through Valve's udev rules (the `steam-devices` package; SteamOS has them), and under Steam, as on Windows,
a virtual gamepad plus the mouse from Steam Input. The Windows build in Proton gets the same Steam Input path (XInput
and the mouse), so the layout above applies there too. The native HID path would need Proton's hidraw passthrough,
which Steam does not give while Steam Input owns the pad.

## Settings

Settings > Controls has a fourth category **Controller** next to Camera / Shortcuts / Map editor
(`data/regs/extend/metro/ui/menus/controls.script`). It shows whether a controller is connected and which kind, the
options below and the button legend. They apply on OK like the keys.

| Option | Setting | Values |
|--------|---------|--------|
| Play with a controller | `$regsController` | on (default) / off: the pad is ignored |
| Cursor speed | `$regsControllerCursor` | 25 .. 250 % (default 100 %) |
| Camera speed | `$regsControllerCamera` | 25 .. 250 % (default 100 %): pan, turn, tilt and zoom |
| Invert tilt | `$regsControllerInvertTilt` | off (default) / on: stick up looks down |

## How it works

* **Reading the pad** (`platform/gamepad.h`, `platform/gamepad_win32.cpp`): XInput, loaded at run time
  (`xinput1_4.dll`, else `xinput1_3.dll`, else `xinput9_1_0.dll`; the game starts without any), the first connected
  slot of four; empty slots are polled again once a second (hot-plug without XInput's cost of polling absent pads every
  frame). `XInputGetStateEx` (ordinal 100) is used when present for the Guide button. Pads under Steam Input
  (PlayStation, Switch Pro, Steam Deck) appear as XInput pads. Without an XInput pad, a DualShock 4 or DualSense
  connected directly (USB or Bluetooth) is read from its raw HID reports: the window registers for raw input of
  gamepads (usage page 1, usages 4 and 5), and `WM_INPUT` reports of Sony devices (vendor 0x054C; DualShock 4
  0x05C4/0x09CC/0x0BA0, DualSense 0x0CE6, DualSense Edge 0x0DF2) are parsed (DualShock 4 report 0x01 and Bluetooth
  0x11; DualSense USB 0x01, Bluetooth simple 0x01 and full 0x31). A Sony pad that sent nothing for a second counts as
  gone. Nothing ships besides system DLLs.
* **Engine** (`engine/gamepad.cpp`, `gamepadFrame`, called by the main loop after the OS pump and before
  `dispatchInput`): radial dead zones (XInput's 0.24 / 0.27, rescaled from the edge), trigger threshold 0.12. The
  virtual cursor starts from the real mouse's latest position each frame, so mouse and pad take turns freely; it moves
  in client pixels (0.8 window heights a second at full deflection × the setting × acceleration), stays 12 pixels off
  the edges (the game's own 5-pixel edge scrolling stays the mouse's) and pans the map when pushed further. Its moves,
  A and B go into `host().input` as ordinary `MouseMove` / `MouseDown` / `MouseUp` events, so the game sees a mouse;
  the system cursor is moved along (`Window::warpCursor`, only while the window has the focus and never in scripted
  runs). Keys go in as `KeyDown` / `KeyUp` with the engine key code (`InputEvent::engineKey`). Nothing is read while
  the window is in the background; buttons held when the pad goes away, is switched off or the window loses the focus
  are released.
* **Camera** (`engine/camera_input.cpp` `cameraPadInput`): the amounts of a frame join the smooth camera's input;
  `regsCameraInput()` returns them as elements 10..17 (pan x/y in screen heights, zoom as ln of the distance factor,
  turn in radians, tilt in radians, turning now, turns ended, tilt resets). The gameUI extension
  (`data/regs/extend/metro/scripts/ui.script`) pans screen-relative (the ground under the screen centre moves by the
  stick's share of the screen height, so the speed fits every zoom and tilt), adds the zoom to the wheel's eased goal,
  follows the turn like a mouse drag (glide and snap to the main directions when the stick is let go) and keeps a tilt
  offset on top of the game's zoom-bound tilt (0.6 + zoom × 0.0017 radians), held between 26 and 87 degrees below the
  horizon and applied after every one of the game's camera updates (`regsApplyTilt`).
* **Bindings** (natives `regsPadBind(button, keyName)`, `regsPadPressed(button)`, `regsPadHeld(button)`,
  `regsPadStatus()`): the gameUI extension binds the keyed buttons from `controls.keys` when a game starts and again
  when the controls page saves (`updateControls`); `"<button>_hold"` binds a key to holding a button for half a second
  (B). Bindings apply only while a game runs (`MAIN.$gameUI`); A, B and Start work everywhere (menus, editor). The
  d-pad's game speed is the script's (`MAIN.$minimap.selectSpeed`).

## Tests

`--at N:pad:<name>,<value>` sets the simulated pad from frame N on (it replaces the real pads until `pad:off`):
buttons `a b x y lb rb back start l3 r3 up down left right guide lpadclick rpadclick l4 r4 l5 r5` with 1 / 0, sticks
`lx ly rx ry` −1..1 (up positive), triggers `lt rt` 0..1; touchpads `lpad,<x>,<y>` / `rpad,<x>,<y>` (a finger there,
−1..1, up positive) and `lpad,off`; `rswipe,<x0>,<y0>,<x1>,<y1>,<frames>` / `lswipe,…` (a finger moving, then
lifted, so the glide shows); `layout,xbox|ps|deck|sc` (the glyphs and status shown). For example `--at 700:pad:rx,1 --at 740:pad:rx,0` turns the camera for 40 frames.
Mixed with `move:x,y` (a real mouse move) the cursor continues from there.

Checked with the simulated pad only (Vienna sandbox save, 1920 × 1080; there was no controller at this PC):
LB + left stick pans; the right stick turns (and snaps on release) and tilts both ways; R3 resets the tilt; RT / LT
zoom in and out; the left stick moves the cursor and pans at the screen edge; A clicks the pause menu's and the
settings' buttons, ticks a checkbox and drags a slider (A held while the stick moves); a tram track was built with the
construction menu (Y, then A on the tram tab and the track tool) and an A-drag on the map; Y opens the construction
menu, X the bulldozer, B held returns to the selection tool, View the underground view, RB pauses, d-pad right raises
the game speed, d-pad up opens the line panel, Start opens the pause menu; the Controller category shows the status,
saves its settings on OK; a run without the simulated pad behaves as before.
Steam Deck / Steam Controller, also with the simulated pad only (`pad:layout,deck`, simulated touchpad fingers and
swipes): a right-pad swipe moves the cursor and it glides on after the finger lifts; the right pad's click picks the
bulldozer in the toolbar and a swipe brings the cursor onto the map (the bulldozer's highlight follows); a left-pad
swipe drags the map and it glides on; L4 + left pad turns the camera (sideways) and tilts it (up and down), L4 + the
pad's click sets the tilt back; the controls page opens on the Steam Deck legend with its glyphs, the layout switches
show the others, the list scrolls by an A-drag on its scroll bar. The glyph sheet (`regs-tool padglyphs`) was looked at
as a picture.
**Not checked on real hardware:** XInput reading, hot-plug, the dead zones' feel and the speeds, the PlayStation
raw-HID parser (written from the reports' published layouts), the Valve HID path as a whole (device discovery,
lizard mode off and on, the Deck and Steam Controller report parsers, written from the protocol as SDL's drivers use
it), the touchpads' gain and glide on real pads, and the Steam Input recommendations (written from Steam's
configurator options, not tried with a Deck).

## Open

* Text entry (line, company and save names) still needs the keyboard; an on-screen keyboard (or Steam's) would close
  the last gap.
* Remappable pad buttons, rumble (e.g. on a refused placement), snapping the cursor to buttons, radial menus and button
  prompts in the HUD (see `docs/ideas.md`).
* Other platforms: SDL2's game-controller API would replace `gamepad_win32.cpp` with the ARM/Linux builds.
