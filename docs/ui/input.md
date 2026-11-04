# GSystem input: from OS events to script events

Scope: how mouse and keyboard input travels from the OS (Win32 messages in `GSystem.dll`, SDL2 events in the Linux
build) through `gsWindow` / `gsWindow_OGL`, the `gsInputObject` interface and the window's input-target list into
`gsDesktop`, and on to `gsControl` script events. Also covered: the in-game mouse pointer, `systemCursor`,
keyboard focus (the "active" chain) and `gsEditCtrl` text entry, the latter in summary only.

**Sources and conventions**

* `W:<addr>:<line>` = `decompiled/GSystem-v2/functions/<addr>.c` (Windows `GSystem.dll`, Ghidra 12.1.4).
  `L:<addr>:<line>` = `decompiled/Linux/functions/<addr>.c` (Linux build with symbols). Line numbers are real file
  lines. Script paths are relative to `extracted/`.
* Windows addresses are authoritative. The Linux build is used to name things and to recover arguments that Ghidra
  lost on the x87 stack in the Windows pseudocode (for example the first `mouseMove` argument in `W:0x100a7e50:58`).
  The Linux and Windows logic matches unless a difference is called out.
* **Observed** means the pseudocode shows it directly. **Inferred** means it follows from layout, call sites or
  script usage. Unmarked statements are observed.
* Offsets are Windows object offsets unless marked Linux. Vtable slots are MSVC indices (one destructor slot).
  Linux (Itanium ABI) has two destructor slots, so its offsets differ.
* The engine's key code type is called **gsKey** here (Linux symbol `gsKey`). Key names are strings such as `"Esc"`.

---

## 0. The path at a glance

```
Win32 msg / SDL event ──► gsWindow_OGL (windowProc / HandleSDLEvents)          per message (message pump)
   │  key, button, wheel, char      ├─► window script events keyDown/keyUp/keyRepeat[keyName]  (unless disableInputEvents)
   │                                └─► inputTargetSnapshot->keyDown/keyRepeat/keyUp/keyChar(gsKey | char)
   │
gsWindow_OGL::tick ── mouse poll ─► inputTarget->mouseMove(dx,dy) | mousePos(x,y)       once per tick
                                       (only while app active && window.mouseCapture)
inputTarget = highest-priority gsInputObject from window.addInputTarget(...)   (in CiM: always MAIN.$desktop)
   │
gsDesktop (gsInputObject at +0x204)
   ├─ mouse:    pointer control follows, hit test controlAt(), hover (onMouseOver/onMouseOut, bubbles up),
   │            capture on button down, mouseDown/mouseUp[keyName](x,y), onMouseMove/onMouseDrag(x,y,dx,dy)
   └─ keys:     modifier mask, Ctrl shortcut, then keyDown/keyUp/keyRepeat/keyChar travel DOWN the active chain:
                desktop → desktop.activeChild → its activeChild → …  (each level fires its own events)
```

---

## 1. OS layer

### 1.1 Windows: `gsWindow_OGL::windowProc` (vtable slot 67, `W:0x10056970`)

Ghidra mislabels this function as `gsWindow_OGL::setFullScreen`. The window class `GSystemWindowOGL` uses
`style = CS_OWNDC (0x20)` and `hCursor = NULL` (`W:0x10059200:162-172`). It therefore receives **no double-click
messages** (no `CS_DBLCLKS`), and Windows never resets the cursor shape by itself. The engine never calls
`SetCapture`, `ClipCursor`, raw input or DirectInput. The only cursor APIs are `SetCursor`, `GetCursorPos` and
`SetCursorPos`.

Every message is first forwarded with `SendMessageA` to `parentWindow` (+0x154) when that field is set
(`W:0x10056970:22-24`). "Target" below means the **input-target snapshot** at +0x1BC. The snapshot is a copy of
`inputTarget` (+0x14C) taken at the start of every window tick (`W:0x10057160:48`).

| Message | Handling | Evidence |
|---|---|---|
| `WM_KEYDOWN` 0x100 | `key = vkToGsKey(wParam)` (+0x1C0). If `key == 0`, return 0 (ignored). If lParam bit 30 (auto-repeat) is set: fire window event **keyRepeat[keyName]** (unless `disableInputEvents`), then `target->keyRepeat(key)` (slot 4). Otherwise fire **keyDown[keyName]**, then `target->keyDown(key)` (slot 3). | `W:0x10056970:88-107` |
| `WM_KEYUP` 0x101 | `key = vkToGsKey(wParam)`. If 0, ignored. Fire **keyUp[keyName]**, then `target->keyUp(key)` (slot 6). | `:80-86`, `:193-201` |
| `WM_CHAR` 0x102 | `target->keyChar(wParam)` (slot 5, one UTF-16 code unit). No script event, no `disableInputEvents` check, `key` not updated. | `:71-77` |
| `WM_LBUTTONDOWN` 0x201 / `WM_MBUTTONDOWN` 0x207 / `WM_RBUTTONDOWN` 0x204 | `key = 143 / 144 / 145` (MouseLeft/Middle/Right), fire **keyDown[keyName]**, then `target->keyDown(key)`. The cursor position in lParam is **ignored**. | `:110-114`, `:139-153`, `:122-133` |
| `WM_LBUTTONUP` 0x202 / `WM_MBUTTONUP` 0x208 / `WM_RBUTTONUP` 0x205 | Same keys, fire **keyUp[keyName]**, then `target->keyUp(key)`. | `:115-119`, `:154-158`, `:134-138`, `:193-201` |
| `WM_MOUSEWHEEL` 0x20A | `acc (+0x198) += (short)HIWORD(wParam) / 120.0`. While `acc ≥ 1`: `acc −= 1`, `key = 146` (MouseWheelUp), **keyDown[…]** + `target->keyDown(146)`. While `acc ≤ −1`: `acc += 1`, `key = 147` (MouseWheelDown), same. **No keyUp is ever sent for the wheel.** | `:159-191` |
| `WM_ACTIVATEAPP` 0x1C | `wParam == 0`: `appActive (+0x150) = 0`, fire **onDeactivate**. Otherwise `appActive = 1`. If `mouseCapture`, `SetCursorPos(captureCentre)` (+0x190/+0x194). Fire **onActivate**. | `:35-47` |
| `WM_SYSCOMMAND` 0x112 | `SC_SCREENSAVE`/`SC_MONITORPOWER` are swallowed when `preventScreenSaver` is set. Everything else goes to `DefWindowProcW`. | `:26-32` |
| `WM_CLOSE` 0x10, `WM_DESTROY` 2 | `PostQuitMessage(0)` if there is no parent window, then `DefWindowProcW`. | `:49-54` |
| `WM_PAINT` 0xF | Full window tick with dt = 1e-7 (so the mouse is also polled), then `DefWindowProcA`. With a parent window it also calls `glFinish` and sends `WM_USER+1` to the parent. | `:55-62` |
| `WM_ERASEBKGND` 0x14 | Sets the "needs clear" byte (+0x1B7) and returns 0. | `:63-65` |
| everything else | `DefWindowProcW`. This includes **`WM_MOUSEMOVE`** (the mouse is polled, see §1.3), **`WM_SYSKEYDOWN/UP`** (so **Alt and F10 never reach the engine on Windows**), `WM_SYSCHAR`, `WM_XBUTTON*`, `WM_MOUSEHWHEEL` and `WM_SETFOCUS`/`WM_KILLFOCUS`. | — |

Window-level script events use key **names**: the event is keyed by `gsKeyName(key)`, so the handler is selected by
string (§4.1). `disableInputEvents` (+0x1B8) suppresses **only** these window events. The input target still
receives everything.

### 1.2 Linux: `gsWindow_OGL::HandleSDLEvents` (`L:0x08227aa0`), polled from `gsWindow_OGL::move` (`L:0x08228170:31-123`)

Linux field offsets: inputTarget +0xC8, snapshot +0x124, appActive +0xCC, mouseCapture +0xCD, key +0x128,
disableInputEvents +0x120, systemCursor +0x12D, wheel accumulator +0x100, "mouse moved" flag +0xE0.

| SDL event | Handling | Evidence |
|---|---|---|
| `SDL_QUIT` 0x100 | `_applicationQuitting = 1`. Returns −1, which ends the poll loop, and `move` returns false. | `L:0x08227aa0:108-112` |
| `SDL_WINDOWEVENT` 0x200 | `ENTER`: hide the OS cursor if `!systemCursor`. `LEAVE`: show it if `!systemCursor`. `SHOWN`/`RESTORED`/`FOCUS_GAINED`: if inactive, re-enter fullscreen when `fullscreen`, then `appActive = 1` and fire **onActivate**. `HIDDEN`/`MINIMIZED`/`FOCUS_LOST`: if active, leave fullscreen and minimise when `fullscreen`, then `appActive = 0` and fire **onDeactivate**. | `:65-105` |
| `SDL_KEYDOWN` 0x300 | `key = getGsKey(keysym.sym)` (`L:0x08227470`, SDL keycodes). If 0, ignored. If `repeat != 0`: **keyRepeat** + `target->keyRepeat`. Otherwise **keyDown** + `target->keyDown`. | `:114-133` |
| `SDL_KEYUP` 0x301 | **keyUp** + `target->keyUp`. | `:135-143`, `:230-237` |
| `SDL_TEXTINPUT` 0x303 | `target->keyChar(firstChar(fromUTF8(text)))`. Only the **first** character of the string is delivered. | `:42-63` |
| `SDL_MOUSEMOTION` 0x400 | Sets the "moved" flag only. The position is read in `move`. | `:171-175` |
| `SDL_MOUSEBUTTONDOWN` 0x401 | SDL button 1→143 (MouseLeft), 2→144 (MouseMiddle), 3→145 (MouseRight). Other buttons leave `key` **unchanged** (a bug that re-sends the previous key). Fires **keyDown** + `target->keyDown`. | `:146-168` |
| `SDL_MOUSEBUTTONUP` 0x402 | Same mapping. Fires **keyUp** + `target->keyUp`. | `:217-237` |
| `SDL_MOUSEWHEEL` 0x403 | `acc += wheel.y` (whole notches), then the same loops as Windows (146/147, keyDown only). | `:178-215` |

Differences from Windows: Alt arrives as a key (SDL left Alt maps to **LeftAlt = 36**, not Alt = 10), there is
**no relative-mouse mode** (§1.3), and the window is grabbed (`SDL_SetWindowGrab`) while `systemCursor` is false.

### 1.3 Per-tick mouse polling and cursor (`gsWindow_OGL::tick`)

Windows `W:0x10057160` (`this` = gsMoveable sub-object at +0xCC; offsets below are object offsets):

```
snapshot(+0x1BC) = inputTarget(+0x14C)                                         :48
if (!systemCursor) {                                                           :51
    SetCursor(NULL)                                    // OS cursor hidden while over the window
    if (appActive && mouseCapture) {                                           :53
        c = point computed from GetWindowRect: left + (2*clientW − outerW)/2, same for y   :54-58
            // exact centre in borderless fullscreen; only approximately centred with a border
        GetCursorPos(&p)
        if (c == storedCentre(+0x190,+0x194)) {                                :61
            if (p != c) SetCursorPos(c)                                        :62-64
            if (inputTarget) inputTarget->mouseMove((p.x−c.x)/height, (p.y−c.y)/height)   :65-70
        } else { storedCentre = c; SetCursorPos(c) }   // no motion this tick        :73-79
    }
} else if (!appActive || !mouseCapture) SetCursor(NULL)                        :82-84
else {
    SetCursor(IDC_ARROW (+0x168, loaded in W:0x10056f10:42))                   :86
    p = ScreenToClient(GetCursorPos())
    if (inputTarget) inputTarget->mousePos(p.x/width, p.y/height)              :87-93
}
video->moveToLast(); gsMoveable::move(dt)  // tick children (controls etc.), then render  :96-97
```

* **Relative mode** (Windows, `systemCursor == false`, the default from `defaults/settings.script:22`): the OS cursor is
  hidden and warped back to the window centre every tick. The target gets `mouseMove(dx, dy)` with **both axes
  divided by the window height** (pixels / height). The call happens **every tick, even when dx = dy = 0**. The
  live `inputTarget` is used here, not the snapshot.
* **Absolute mode** (Windows with `systemCursor == true`, and always on Linux): `mousePos(x, y)` with
  **x / window width and y / window height** (0..1, unclamped). Linux reads the position with
  `SDL_GetMouseState` (only after a motion event) and calls `mousePos` every tick while `appActive && mouseCapture`
  (`L:0x08228170:66-97`). On Linux, `systemCursor == false` hides the cursor and grabs the window
  (`:54-65`). `systemCursor == true` releases the grab and shows the cursor only while active and captured
  (`:71-87`).
* **`mouseCapture` (+0x151) is a plain bool with no side effect in its setter** (`W:0x10057fa0`). It means "the game
  wants mouse input". While it is false, **no motion reaches any target** and the OS cursor stays hidden. Mouse
  *buttons* are still delivered through `WM_*BUTTON*` (§1.1). Scripts toggle it through a reference count in
  `metro/ui/mouse/mouse.script:37-47` (`requireMouse`/`releaseMouse`).
* Defaults: `appActive = 1`, `mouseCapture = 0`, `inputTarget = null` (`W:0x100a2110:49-51`); `systemCursor = 0`,
  `disableInputEvents = 0`, `key = 0`, wheel accumulator = 0 (`W:0x10058070:29-45`). On window creation the cursor
  is set to the centre if `mouseCapture` is already set (`W:0x10059200:212`).
* Quirk (Windows, relative mode): when `mouseCapture` becomes true while the OS cursor is away from the stored centre,
  the first delta is `cursor − centre`, so the pointer jumps.

### 1.4 Key codes (gsKey) and key names

The engine uses its own key enum of **186 codes** (`0xBA`, `gsGetKey` `L:0x08108210:31`). `gsKeyName(k)` returns
entry `k` of a static `std::string` table (`W:0x100828f0`, `DAT_101903b8`, stride 0x1C). The names come from the Linux
static initialiser `L:0x080c73a0`. `gsGetKey(name)` does the reverse lookup and returns 0 for unknown names. There are
**no `KEY_*` script constants**. Scripts only see key **names**.

| Codes | Names |
|---|---|
| 0–12 | Undefined, Left, Right, Up, Down, Backspace, Enter, Space, Tab, **Control (9)**, **Alt (10)**, **Shift (11)**, Esc |
| 13–31 | Pause, Clear, Select, Print, Execute, PrintScreen, Help, Multiply, Add, Separator, Subtract, Decimal, Divide, CapsLock, NumLock, ScrollLock, LeftWin, RightWin, Applications |
| 32–49 | LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt, Play, Zoom, Sleep, Cancel, Convert, NonConvert, Accept, ModeChange, Kana, Junja, Final, Hanja |
| 50–72 | Plus, Comma, Minus, Period, Oem1…Oem8 (54–61), Ax, Oem102, IcoHelp, Ico00, NumpadEqual, Insert, Delete, Home, End, PageUp, PageDown |
| 73–98 / 99–108 / 109–118 | A…Z / 0…9 / Numpad0…Numpad9 |
| 119–142 | F1…F24 |
| 143–147 | **MouseLeft, MouseMiddle, MouseRight, MouseWheelUp, MouseWheelDown** |
| 148–185 | AxisX/Y/Z, RotationX/Y/Z, Button0…Button31 (joystick; never generated) |

Windows VK → gsKey (`FUN_100562c0`, `W:0x100562c0`) is essentially "same-named key". The full list is:
03→41, 08→5, 09→8, 0C→14, 0D→6, **10→11, 11→9, 12→10**, 13→13, 14→26, 15→46, 17→47, 18→48, 19→49, 1B→12,
1C–1F→42–45, 20→7, 21→71, 22→72, 23→70, 24→69, 25→1, 26→3, 27→2, 28→4, 29→15, 2A→16, 2B→17, 2C→18,
2D→67, 2E→68, 2F→19, '0'–'9'→99–108, 'A'–'Z'→73–98, 5B→29, 5C→30, 5D→31, 5F→40, 60–69→109–118, 6A→20, 6B→21,
6C→22, 6D→23, 6E→24, 6F→25, 70–87→119–142, 90→27, 91→28, 92→66, A0–A5→32–37, BA→54, BB→50, BC→51, BD→52,
BE→53, BF→55, C0→56, DB→57, DC→58, DD→59, DE→60, DF→61, E1→62, E2→63, E3→64, E4→65, FA→38, FB→39. Everything else
maps to 0 and is dropped. Because `WM_KEYDOWN` reports generic `VK_SHIFT`/`VK_CONTROL`, Windows produces Shift (11)
and Control (9), never the Left/Right variants. Linux maps both SDL Ctrl keys to Control (9), both Shift keys to
Shift (11), and left Alt to LeftAlt (36) (`L:0x08227470`).

Key names that the shipped scripts bind include `"Esc"`, `"Enter"`, `"Tab"`, `"Up"`, `"Down"`, `"Pause"`, `"F1"`–`"F12"`,
`"MouseLeft"`, `"MouseMiddle"`, `"MouseRight"`, `"MouseWheelUp"` and `"MouseWheelDown"`
(`metro/defaults/controls.keys`: zoom = wheel, panning = MouseMiddle).

### 1.5 Window script surface (input part)

| Member | Kind | Meaning |
|---|---|---|
| `keyDown`, `keyUp`, `keyRepeat` | keyed events (gsModule ids 1/2/3, `W:0x100a0750:51-53`) | Fired by the window for keys, mouse buttons (Down/Up) and wheel notches (Down only), keyed by key name, **0 args**, before the target is called. Example: `event $window.keyDown::["F11"]()` (`scripts/console/console.script`). |
| `onActivate`, `onDeactivate` | events 6/7 (`W:0x10057a50:31-32`) | App activation (§1.1/§1.2). No other input side effect: the target is **not** told. CiM pauses through `system.script:10-15`, which removes the desktop target and so triggers `focus(false)`. |
| `inputTarget` | var, ro (+0x14C) | Current target (§2.2). |
| `mouseCapture` | bool (+0x151) | §1.3. |
| `systemCursor` | bool (+0x1C5) | §1.3 and §6. |
| `disableInputEvents` | bool (+0x1B8) | Suppresses the window's keyDown/keyUp/keyRepeat events only. |
| `key` | int (+0x1C0) | Last gsKey seen (keys, buttons, wheel; not `WM_CHAR`). Written before the `disableInputEvents` check. |
| `addInputTarget(obj[, prio=0])`, `removeInputTarget(obj)` | methods | §2.2. |

---

## 2. `gsInputObject` and the input-target list

### 2.1 Interface (vtable of the `gsInputObject` sub-object)

| Win slot | Linux off | Method | Default (`gsInputObject`) | gsDesktop (sub-object at +0x204, vtable 0x10162da8) |
|---|---|---|---|---|
| 0 | 0x0/0x4 | dtor | — | — |
| 1 | 0x08 | `mouseMove(float dx, float dy)` | empty | `0x100a7e50` §3.2 |
| 2 | 0x0C | `bool mousePos(float x, float y)` | returns false (`L:0x080ea510`) | `0x100a68b0` §3.3 |
| 3 | 0x10 | `keyDown(gsKey)` | owner `keyDown[name]` event (`L:0x08108050`) | `0x100a6930` §3.6 |
| 4 | 0x14 | `keyRepeat(gsKey)` | owner `keyRepeat[name]` | `0x100a6b30` |
| 5 | 0x18 | `keyChar(uint ch)` | empty | `0x100a6910` |
| 6 | 0x1C | `keyUp(gsKey)` | owner `keyUp[name]` | `0x100a6b50` |
| 7 | 0x20 | `focus(bool)` | empty | `0x100a6d60` §3.8 |

`+4` of the sub-object points to the owning script object, which is used for events and for reference counting.
`gsInputObject` implementers are `gsDesktop`, `gsEditor` (derives from gsDesktop), `gsBufferedInput` and
`gsFlyingCamera` (a debug camera through `gsBufferedInput`). `gsBufferedInput` additionally keeps a 186-byte "held"
array that is cleared on `focus(false)` (`L:0x08108120`, `L:0x08108090`). Only the desktop matters for the UI.

### 2.2 Target list (`gsWindow` +0x140: `std::list<{int prio; gsInputObject* obj}>`, head +0x144, size +0x148)

* `addInputTarget(obj, prio)` (script `W:0x100a01b0`). The argument must dynamic-cast to `gsInputObject`, otherwise
  the call silently does nothing. `prio` is `int(arg2)` if given, else 0 (`:26-32`). The window then calls
  `addInputTarget` (slot 62, `W:0x100a2260`): add a reference, **append** `{prio, obj}`, call `updateInputTarget(null)`.
  Duplicates are allowed: each add is one list entry, and the scripts pair add and remove calls.
* `removeInputTarget(obj)` (slot 63, `W:0x100a0e50`): finds the **last** entry for `obj`, releases it, unlinks it,
  then calls `updateInputTarget(null)`.
* `updateInputTarget(t)` (slot 61, Linux name `setInputTarget`, `W:0x100a0dc0`). With `t == null` it scans the list
  front to back with `if (best <= e.prio) { t = e.obj; best = e.prio; }`, where `best` starts at 0. The result is the
  **highest priority, and among equals the most recently added entry**. Entries with **negative** priority can never be
  chosen. If `t != inputTarget`: `old->focus(false)`, release, store, add a reference, `new->focus(true)`
  (`:22-39`).
* The target is only *read* by the window: live for mouse polling, and through the per-tick snapshot for
  message-driven key, button and char events.

CiM only ever targets `MAIN.$desktop`: `main.script:56/79/564…`, `system.script:30/41`, `loading.script:198`, and the
console with priority 100. The desktop therefore gains and loses focus as menus, the game and pause come and go.

---

## 3. `gsDesktop`

### 3.1 State (`W:` offsets; Linux = Windows − 0x68)

| Off | Name | Notes |
|---|---|---|
| 0x20C / 0x210 | `mouseX`, `mouseY` (script, ro) | Pointer position in **desktop-local units** (desktop width × height; CiM sets the desktop to the window size in pixels, `metro/scripts/window.script:57-62`). |
| 0x214 | `mouse` (script) | The pointer control (§6). |
| 0x218 | capture ("mouse-down control") | Set on any mouse-button down. |
| 0x21C | hover | Last control under the pointer. |
| 0x220 | buttons/modifier mask | bit0 MouseLeft, bit1 MouseMiddle, bit2 MouseRight, 0x100 Shift, 0x200 Control, 0x400 Alt. Passed to control handlers as `buttons`; not script-visible. |
| 0x224 | lastDownKey | Mouse key of the latest button-down. |
| 0x228 | shortcutMode (byte) | Set while Control or Alt is held. |

Relevant gsControl fields (from `docs/native-api.md` plus code): status/effective status +0x68/+0x6C
(bit0 = enabled, bit1 = visible; the effective status is own & owner's effective, `W:0x100a00c0`), parent control
+0xD4 (dynamic-cast owner, `W:0x100a6110`), `left/top/width/height` +0xD8…+0xE4, `desktopLeft/Top` +0xE8/+0xEC
(= parent.desktopLeft + parent.marginLeft + left, `W:0x100a95a0:19-26`), `background`/`hotBackground` +0xF8/+0xFC,
`active` +0x186, `canActivate` +0x187 (default **true**), `clickThrough` +0x188, `clickThroughSelf` +0x189,
hot +0x18A, dragging +0x18B, outside-parent +0x18C, drag origin +0x1B8/+0x1BC, activeChild +0x1C0, `dragTarget` +0x1C4,
margins L/T/R/B +0x1D8…+0x1E4 (from `setMargins`, `W:0x100a6f10:23-27`). Constructor defaults are in
`W:0x100a8d90`.

**Note:** `gsViewPort` is itself a `gsControl`. The desktop's parent control is therefore the GUI viewport
(`window.script:49-57`), and parent-walking operations (hover bubbling, `activate`) continue up to it.

### 3.2 `mouseMove(dx, dy)` (slot 1, `W:0x100a7e50`, clearer in `L:0x081c6330`)

```
H = desktop.height, W = desktop.width, (ox, oy) = (mouseX, mouseY)
nx = clamp(ox + dx*H, 0, W)          // ≥W → W, ≤0 → 0 (inclusive range)           W:…:22-30
ny = clamp(oy + dy*H, 0, H)                                                     :31-39
if (mouse) mouse->setPosition(ox, oy)     // pointer goes to the OLD position     :40-43 (L:…:45)
cap = capture
if (cap == null || cap == desktop) {
    hit = desktop->controlAt(ox, oy)      // hit test at the OLD position         :46 (L:…:51)
    if (hit != hover) {
        if (hover) hover->mouseOut()                                            :51
        hover = hit
        if (hit) hit->mouseOver()                                               :55
    }
    if (hit && !hit->mouseMove(nx − hit.desktopLeft, ny − hit.desktopTop,
                               nx − ox, ny − oy, buttons))                      :57-61 (L:…:64-67)
        (nx, ny) = (ox, oy)               // handler refused: pointer does not move
} else {
    if (!cap->mouseMove(nx − cap.desktopLeft, ny − cap.desktopTop, nx − ox, ny − oy, buttons))
        (nx, ny) = (ox, oy)                                                     :63-71
    // hover is NOT updated while a control is captured
}
(mouseX, mouseY) = (nx, ny)                                                     :78-79
```

* The pointer control and the hit test both use the position from **before** this tick's delta: there is a
  one-tick lag, and the pointer and the hit test agree with each other.
* `gsControl::mouseMove` always returns true. Overrides can return false to pin the pointer. Example:
  `gsEditor::mouseMove` returns 0 while rotating (`L:0x081d79a0`).
* Hover is **re-evaluated every tick** because the window calls `mouseMove`/`mousePos` every tick while
  `appActive && mouseCapture` (§1.3). A control that appears under a still pointer gets `onMouseOver` on the next tick.

### 3.3 `mousePos(x, y)` (slot 2, `W:0x100a68b0:22-24`)

If `H != 0`: `this->mouseMove((x*W − mouseX)/H, (y*H − mouseY)/H)`. This converts the absolute 0..1 position into
the delta that lands exactly on `(x*W, y*H)` (then clamped). Returns true.

### 3.4 Hit test `controlAt(px, py)` (gsControl slot 63, `W:0x100a97a0`; no subclass overrides it)

`(px, py)` are in the control's own coordinates (relative to its outer top-left):

```
lx = px − marginLeft, ly = py − marginTop                       // into client space   :20-21
for i = children.size−1 down to 0:                              // last child first = front-most   :22-33
    c = dynamic_cast<gsControl>(children[i]);  skip if null      // sounds, scenes, … are ignored
    skip if c.clickThrough                                      :35
    skip if (c.effectiveStatus & 3) != 3                       // must be visible AND enabled (own and all owners) :36
    skip unless c.left + c.marginLeft <= lx <= c.left + c.width − c.marginRight     :37-41
            and c.top + c.marginTop <= ly <= c.top + c.height − c.marginBottom        // inclusive bounds
    r = c->controlAt(lx − c.left, ly − c.top);  if (r) return r // recurse; a null result falls through to siblings :43
return clickThroughSelf ? null : this                           :31
```

Consequences a port must reproduce:

* **The hit area is the client rect** (rect minus margins), not the outer rect. Buttons created with
  `setMargins(14,0,0,0)` (`ui.script` OK/Cancel) cannot be clicked on their left 14 px. Descendants are effectively
  clipped to every ancestor's client rect.
* Flags that **do not** matter for hit testing: `canActivate`, `visibility` (alpha; a fully transparent control is
  still hit), the outside-parent flag +0x18C, the control's image or background.
* `clickThrough`: the control and its whole subtree are skipped. `clickThroughSelf`: the control is skipped but
  its children can still be hit. The root (desktop) is never bounds- or status-checked, so with no other hit it is
  returned itself (unless `desktop.clickThroughSelf`).
* Z-order: the child vector order is also the drawing order. `updateChildren` assigns `setControlIndex(i)` (layer z),
  `bringToFront` = `moveToLast`, `sendToBack` = `moveToFirst` (`W:0x100a6830`, `W:0x100a1f30`). Hence "last = on
  top" (*inferred* that the renderer sorts by that index, H).

### 3.5 Hover: `mouseOver()` / `mouseOut()` (slots 68/69, `W:0x100a6220`, `W:0x100a6270`)

```
mouseOver(): if (parent) parent->mouseOver();  hot = 1;  if (hotBackground != background) updateBackGround();  fire onMouseOver()
mouseOut():  if (parent) parent->mouseOut();   hot = 0;  same refresh;                                           fire onMouseOut()
```

Hover **bubbles to every ancestor**, up to and including the desktop and the GUI viewport. The ancestors fire
**first** (root first). Moving between two sibling buttons therefore produces root…parent…A `onMouseOut`, then
root…parent…B `onMouseOver`. The scripts guard against these repeats with `.$hovered` flags (`ui.script:256-286`).
The background uses `hotBackground` while `hot || active` (`W:0x100a7860:56`).

### 3.6 Keys and buttons into the desktop: `keyDown` (slot 3, `W:0x100a6930`) / `keyUp` (slot 6, `W:0x100a6b50`)

```
keyDown(k):
    mask |= {Control:0x200, Alt:0x400, Shift:0x100, MouseLeft:1, MouseMiddle:2, MouseRight:4}[k]   :19-40
    if k in {MouseLeft, MouseMiddle, MouseRight}:                                                    :42
        lastDownKey = k
        capture = controlAt(mouseX, mouseY)        // stale position from the last tick
        if (capture != hover) { hover?.mouseOut(); hover = capture; hover?.mouseOver(); }            :44-57
        capture?.activate()                        // §3.9, may run onActivate scripts              :58-59
        capture?.mouseDown(mouseX − c.desktopLeft, mouseY − c.desktopTop, k, mask)                   :62-65
    else if k in {Control, Alt}: shortcutMode = 1                                                     :68-69
    else if shortcutMode && this->shortCut(k): return   // consumed: NO keyDown events at all         :71-73
    this->keyDown(k, mask)                          // gsControl slot 71: the active-chain walk, §3.7 :75

keyUp(k):
    mask &= ~bit(k)
    if k is a mouse button:
        if (capture) { if (lastDownKey == k) capture = null;
                       oldCapture->mouseUp(mouseX − desktopLeft, mouseY − desktopTop, k, mask) }      :42-49
        if (lastDownKey == k) { re-hit-test at (mouseX, mouseY); update hover (out/over) }             :51-62
    if k in {Control, Alt}: shortcutMode = 0                                                          :66-67
    this->keyUp(k, mask)                            // slot 74, active-chain walk                      :69
```

* **There is no click, double-click or wheel-to-control event.** Scripts act on `mouseDown["MouseLeft"]`
  (`ui.script:238`, `:330`). `mouseUp` goes to the capture control wherever the pointer is.
* Any of the three buttons sets the capture. Only releasing the button stored in `lastDownKey` clears it. With
  overlapping presses (L down, R down, L up), the first control never gets its `mouseUp`.
* While captured, motion goes to the capture control in its local coordinates, which may be outside its rect. That
  is how sliders and scroll bars drag (§3.10).
* Mouse buttons and the wheel also travel the **keyboard path**. `MouseLeft` produces `keyDown["MouseLeft"]` and
  `onKeyDown("MouseLeft")` on the desktop and down the active chain (`console.script:23`; the key-binding menu
  accepts mouse keys). The wheel only produces keyDown and only along the active chain (zoom is bound on
  `MAIN.$player.keyDown`). It is never delivered to the hovered control.
* `keyRepeat(k)` and `keyChar(ch)` go straight to the active-chain walk with `mask` (`W:0x100a6b30:14`,
  `W:0x100a6910:14`). There is no shortcut check and no mask update.
* Alt never reaches the desktop on Windows (§1.1), and on Linux it is LeftAlt. In practice only **Ctrl**
  enters shortcut mode.

### 3.7 Keyboard routing: the active chain

`gsControl::keyDown(k, mask)` (slot 71, `W:0x100a89e0`; `gsDesktop` slot 71 just calls it, `W:0x100a8aa0`):
fire **keyDown[name]()** (`:21`), then **onKeyDown(name)** with one string argument (`:22-24`), then
`activeChild?->keyDown(k, mask)` (`:26`). `keyUp` is the same with **keyUp[name]()** + **onKeyUp(name)**
(`W:0x100a8a40`, desktop copy `W:0x100a8ab0:21-26`). `keyRepeat` (slot 72, `W:0x100a6d20`) fires
**keyRepeat[name]()** only and forwards. `keyChar` (slot 73, `W:0x100a6760`) forwards only, with no event.
`shortCut(k)` (slot 70, `W:0x100a6740`) returns `activeChild?->shortCut(k)`, else false.

Events therefore fire **top-down** along desktop → activeChild → … → leaf, once per level. Nothing bubbles up and
nothing goes to the hovered control. Focus is "the active chain".

### 3.8 `focus`, `resetMouseDown`, `removeControl`, `setMouse`

* `focus(true)` → `mouseMove(0, 0)` (an immediate hover refresh, `W:0x100a6d60:17`). `focus(false)` → if capture:
  `capture->mouseUp(local, lastDownKey, mask)` and clear it; if hover: `hover->mouseOut()` and clear it (`:19-31`).
  The button mask is **not** cleared.
* Script `desktop.resetMouseDown()` (`W:0x100a6850`) sets `mask = 0` (modifiers too) and, if there is a capture,
  clears it and calls `mouseUp(local, lastDownKey, 0)`. `ui.script:587,616` calls it before opening modal dialogs.
* `removeControl(c)` (slot 94, `W:0x100a6dd0`) silently nulls hover, capture or `mouse` if they equal `c`, without
  events. `gsControl::removeChild` calls it up the parent chain whenever a control is removed, and also clears
  `activeChild` if it was the removed child (`L:0x081ca060`).
* `desktop.mouse = ctrl` (`W:0x100a7d90` → `setMouse` slot 98, `W:0x100a6e10`) swaps references, forces
  `ctrl.clickThrough = 1` and **initialises `mouseX/mouseY` from `ctrl.left/top`** (`:20-22`).

### 3.9 Activation (`active`, `canActivate`, `activate`)

* `activate()` (slot 93, `W:0x100a6780`): `setActive(true)`, then `parent?->activate()`. A click activates the hit
  control and **all its ancestors**.
* `setActive(v)` (slot 90, `W:0x100a7ca0`) does nothing unless `canActivate && active != v` (`:18`).
  * `v = false`: fire **onDeactivate** (`:21`). If the parent's activeChild is this control, null it. Then
    `activeChild?->setActive(false)` (the whole sub-chain deactivates).
  * `v = true`: fire **onActivate** (`:31`). Then `parent->setActiveChild(this)` (slot 62, which deactivates the
    previous active sibling, `W:0x100a6670`), **`bringToFront()`** (`:35`), `raiseStatus(2)` (**makes the control
    visible**, `:36`), and re-activate `activeChild` if it is visible, otherwise null it (`:37-43`).
  * Then the hot/background refresh (`:45-47`).
* `statusUpdated` (`W:0x100a66a0:17-36`): a control that becomes not effectively visible, or is entirely outside its
  parent, is `setActive(false)`.
* CiM marks nearly every widget `canActivate = false` (desktop, viewport, buttons, panels), so clicks normally only
  raise and focus windows such as dialogs (`ui.script:633-636`) and `MAIN.$player`. Script-side `.active = true`
  (for example a modal key catcher, `menus/controls.script:134-138`) makes a control the keyboard focus without a click.

### 3.10 `gsControl` mouse handlers (what scripts receive)

* `mouseMove(x, y, dx, dy, buttons)` (slot 65, `W:0x100a7660`): if `dragging` (+0x18B): when `dragTarget` is set,
  `dragTarget->setPosition(dragTarget.left + x − dragOrigin.x, dragTarget.top + y − dragOrigin.y)` (window dragging;
  `console.script:30`), then fire **onMouseDrag(x, y, dx, dy)**. Always fire **onMouseMove(x, y, dx, dy)**. Return
  true (`:25-50`).
* `mouseDown(x, y, key, buttons)` (slot 66, `W:0x100a7770`): if `key == MouseLeft`, set `dragOrigin = (x, y)` and
  `dragging = 1`. Fire **mouseDown[keyName](x, y)**.
* `mouseUp(x, y, key, buttons)` (slot 67, `W:0x100a77f0`): if `key == MouseLeft`, `dragging = 0`. Fire
  **mouseUp[keyName](x, y)**.
* The script method `ctrl.mouseMove(x, y[, dx, dy[, buttons]])` calls slot 65 directly and returns its bool
  (`W:0x100a6fd0`; used as `MAIN.$player.mouseMove(0.0, 0.0)`).
* Overrides: `gsTextSelCtrl`/`gsEditCtrl` replace all three. They set the caret and selection on **any** button and
  fire **no** mouse events (`L:0x081d4a80`, `L:0x081d46f0`, `L:0x081d4c10`). `gsGridPlayer` and `gsEditor` add
  game and editor logic and call the base (`L:0x0819f2a0`, `L:0x081d79a0`).

---

## 4. Script-visible input events

| Object | Event (id) | Keyed? | Args as scripts receive them | When |
|---|---|---|---|---|
| window | `keyDown` (1) / `keyUp` (2) / `keyRepeat` (3) | yes, key name | none | Every OS key/button/wheel message, unless `disableInputEvents`. |
| window | `onActivate` (6) / `onDeactivate` (7) | no | none | App (de)activation. |
| any control incl. desktop | `keyDown` (1) / `keyUp` (2) / `keyRepeat` (3) | yes | none | Active-chain walk (§3.7). |
| any control | `onKeyDown` (6) / `onKeyUp` (7) | no | `(keyName: string)` | Same walk, right after the keyed event. |
| any control | `mouseDown` (8) / `mouseUp` (9) | yes, `"MouseLeft"`/`"MouseMiddle"`/`"MouseRight"` | `(x, y)` floats, local to the control's outer top-left, desktop units | Button down on the hit control / button up on the capture control. |
| any control | `onMouseOver` (12) / `onMouseOut` (13) | no | none | Hover change; fired on the control and **all ancestors, root first**. |
| any control | `onMouseMove` (14) | no | `(x, y, dx, dy)`: local position after the move; delta in desktop units | Every tick on the hovered or captured control (also with dx = dy = 0). |
| any control | `onMouseDrag` (15) | no | `(x, y, dx, dy)` | Same tick, before onMouseMove, while MouseLeft went down on this control. |
| any control | `onActivate` (10) / `onDeactivate` (11) | no | none | `setActive` transitions. |
| gsEditCtrl | `onCheckInput` (17) | no | `(charCode: int)` | Before inserting a char ≥ 0x20; setting `rejectInput = true` drops it. |
| gsEditCtrl | `onChange` (18) | no | none | After an insert or delete. |

Registrations: `W:0x100a0750:51-55` (gsModule), `W:0x100aae80:82-92` (gsControl), `W:0x10057a50:31-32`,
`W:0x100b1110:20-21`. The `buttons` mask is never passed to scripts.

### 4.1 Dispatch semantics (`gsObjectVariant::event`, `L:0x08265d70` plain, `L:0x08266f50` keyed; Windows `FUN_1001f960`, `0x100201c0`)

* Each class event slot has a "keyed" flag. A keyed event is raised with a key string and runs only the handlers
  bound with that key (`std::multimap` `equal_range`, `L:0x08266f50:32-36`), as in
  `event .mouseDown::["MouseLeft"]() {…}`. A plain event runs all its handlers (`L:0x08265d70:28`).
* Each handler runs as a **new script thread that executes immediately** (`gsThread::execute`) until it finishes or
  waits. Input handlers therefore run synchronously inside the message dispatch (keys and buttons) or inside the
  window tick (motion and hover). If a handler `wait`s, it continues later from the VM scheduler
  (`ui.script:256-275`).
* Argument count passed = `min(argsRaised, paramsDeclared)` (`L:0x08266f50:44`). `onMouseMove::($x, $y)` gets the
  first two of four, and `mouseDown::[…]()` gets none.
* Natives check `eventAssigned(id)` before building arguments. That is an optimisation with no visible effect.

---

## 5. Timing: per message vs per tick (Windows `moveLoop` `W:0x10082da0`)

1. `dt` is measured and clamped to [0.0001, 0.05].
2. **Message pump** (`W:0x10082cd0`, `PeekMessageW`/`TranslateMessage`/`DispatchMessageW`; with `MAIN.waitMessages`
   it first waits up to 1 s for input). Keys, buttons, wheel and chars are processed here, synchronously, against the
   **snapshot target** and the **stale `mouseX/Y` from the previous tick**. Their script handlers run here.
3. `MAIN` tick: the VM scheduler (timers, waiting threads), then child modules, which include the window.
4. Window tick (§1.3): take the snapshot, update the cursor, poll the mouse, then
   `desktop.mouseMove` → pointer placement, hit test, `onMouseOver/Out`, `onMouseDrag/Move`. Then the controls are
   ticked and the frame is rendered and swapped.

So **button and key events of a frame are handled before that frame's motion**. Hover, pointer placement and
`onMouseMove` are **per tick**. Everything else is **per message**. Linux does the same inside
`gsWindow_OGL::move`: it drains SDL events first, then polls the mouse.

---

## 6. The mouse pointer and `systemCursor`

* The in-game pointer is an ordinary control, `metro/ui/mouse/mouse.script:8-15`. `$mouse` is a 500×500 child of
  `MAIN.$overlay` (a full-screen sibling of the desktop under the GUI viewport, drawn later, so on top). It contains
  `$icon` (62×62 at 0,0, an animated arrow strip), which is visible only when `!systemCursor`. The script registers
  it with `$desktop.mouse = $mouse` (`:88`). The engine then makes it `clickThrough` and seeds `mouseX/Y` from its
  position, which is the screen centre (`:9`).
* Each desktop `mouseMove` calls `mouse.setPosition(mouseX, mouseY)` with the pre-move position (§3.2). The hotspot
  is the control's top-left. Tooltips and hover infos are created as children of `$desktop.mouse`, so they follow the
  pointer (`hoverinfo.script:139`).
* `showMouse`/`hideMouse` toggle `$mouse` visibility, and `requireMouse`/`releaseMouse` reference-count
  `window.mouseCapture` (`mouse.script:23-47`). `main.script` brackets every interactive phase with
  `requireMouse()`, `addInputTarget($desktop)` and `showMouse()` (`:54-57`).
* `systemCursor = true` (settings checkbox, `menus/graphics.script:255-257`) shows the Windows arrow, switches to
  absolute positioning (§1.3) and hides `$icon`. The `$mouse` container still moves, so hover-info children still
  follow.
* Edge scrolling reads `$desktop.mouseX/mouseY` against `width`/`height` (`metro/scripts/ui.script:208-209`). This
  relies on the inclusive clamp `[0, W] × [0, H]`.

---

## 7. Keyboard text entry (`gsEditCtrl`, summary)

* Characters (`WM_CHAR` / `SDL_TEXTINPUT`) arrive as `keyChar(ch)` on the desktop and travel the active chain. An edit
  control receives them only if it is in that chain, either set `.active = true` by script or clicked with
  `canActivate` (the default).
* `gsEditCtrl::keyChar(ch, mask)` (`L:0x081d5500:26-69`; Windows `W:0x100b0c30`): if `ch > 0x1F`: if
  `onCheckInput` is bound, set `rejectInput = false`, fire `onCheckInput(ch)`, and drop the char if it was rejected.
  Delete the selection. If `maxLength == −1 || length < maxLength`, insert at the caret, advance the caret and fire
  `onChange`. Always forward to its own activeChild.
* `keyDown`/`keyRepeat` = `keyPress(k)` then the base event walk (`L:0x081d6540`, `L:0x081d64f0`). `keyPress`
  handles Left/Right (caret, clears the selection), Backspace and Delete, each firing `onChange`
  (`L:0x081d62f0:28-65`). Enter and Esc are left to scripts (`event .keyDown::["Enter"]`).
* Ctrl shortcuts through `shortCut`: C copies (`gsTextSelCtrl`, `W:0x100af890:17`). X cuts and V pastes, running
  `onCheckInput` per char (`W:0x100b0d30:32`, `:44`). A handled shortcut suppresses all keyDown events for that key.
* `hideChar`, `caretColor` and `blinkSpeed` are display-only.

---

## 8. What the shipped scripts depend on

* Main menu (`metro/ui/menus/mainmenu.script` + `metro/ui/ui.script` `createButton`): `mouseDown["MouseLeft"]()`
  fires the action **on press**; `mouseUp["MouseLeft"]()` resets the image; `onMouseOver()`/`onMouseOut()` set hover
  images and play the hover sound (`mainmenu.script:126`); `$desktop.keyDown["Esc"]()` (`:10`). Decorative children
  use `clickThrough = true` (`ui.script:305`) so they do not steal hits from their button.
* Logos are skipped with `$window.keyDown["Esc"]` (`metro/ui/logos/co-logo.script:4`). This works before any input
  target exists.
* Modal dialogs: a full-screen `canActivate = false` layer catches clicks (`mouseDown[...]` → flash) and the box is
  `.active = true`, so it receives `keyDown["Enter"/"Esc"]` (`ui.script:614-643`).
* Drag: `onMouseDrag` on the scroll bar, slider handle and minimap (`ui.script:421, 536`, `minimap.script:186`),
  and `dragTarget` on the console title bar.
* Game: `MAIN.$player` (a `gsGridPlayer` control filling the desktop) takes `keyDown/keyUp/keyRepeat[...]`
  bindings from `controls.keys`, including the wheel and MouseMiddle, plus `onMouseMove($x, $y)` and `mouseDown`.

---

## 9. Port checklist (ReGS), in order of importance for a responsive main menu

1. **Script event plumbing**: keyed events (dispatch by key-name string) and plain events, handlers run synchronously
   as threads with args truncated to the declared parameter count (§4.1). Without this nothing below is observable.
2. **Window input-target list**: `addInputTarget(obj, prio=0)` appends, `removeInputTarget` removes the last
   match, selection = highest prio with ties going to the newest and negatives never chosen, `focus(false/true)` on
   change, per-tick snapshot for key events (§2.2).
3. **Per-tick mouse feed**: while `appActive && mouseCapture`, call `target.mousePos(x/W, y/H)` every tick
   (SDL absolute, as in the official Linux build; Windows relative mode is optional, §1.3). Hide the OS cursor unless
   `systemCursor`. Mouse position only changes in the tick, never per message.
4. **`gsDesktop::mouseMove`** exactly as in §3.2: clamp to the inclusive `[0,W]×[0,H]` range, set the pointer to the
   old position, hit-test at the old position, hover out/over, call `mouseMove(local, delta, mask)` on the hit or
   captured control, revert on false. Also `mousePos`→`mouseMove`, and `desktop.mouse` setter semantics (§3.8).
5. **`controlAt`** (§3.4): reverse child order, skip `clickThrough`, require effective visible + enabled, client-rect
   inclusive bounds, recurse, `clickThroughSelf`. Desktop root returned when nothing else is hit.
6. **Hover**: `mouseOver`/`mouseOut` with ancestor bubbling (root first), `hot` flag and `hotBackground` refresh,
   `onMouseOver`/`onMouseOut` events (§3.5).
7. **Buttons**: map OS buttons to keys 143/144/145 → window `keyDown/keyUp[name]` → desktop `keyDown/keyUp`:
   mask, capture = hit, hover refresh, `activate()` chain, `mouseDown[name](x,y)`; on release `mouseUp[name](x,y)` to
   the capture, clear only on a matching `lastDownKey`, re-hit-test; then the active-chain key walk (§3.6). Plus
   drag (`dragging`, `dragTarget`, `onMouseDrag`) and `onMouseMove` (§3.10).
8. **Keyboard**: OS key → gsKey (§1.4 table; drop unmapped), window `keyDown/keyRepeat/keyUp[name]` unless
   `disableInputEvents`, `key` field. Desktop modifier mask and Ctrl shortcut mode, then the active-chain walk with
   `keyDown[name]` + `onKeyDown(name)` / `keyUp` + `onKeyUp` / `keyRepeat` (§3.6-3.7). Wheel = keyDown 146/147 per
   notch.
9. **Activation**: `setActive`/`activate`/`setActiveChild` incl. **bringToFront + make visible** on activate,
   `statusUpdated` deactivation, `removeChild` → `removeControl` reference clearing (§3.8-3.9).
10. **`focus(true/false)`** and `resetMouseDown()` (§3.8). They are needed for menu ↔ game ↔ pause transitions and
    dialogs.
11. **Window activation**: `onActivate`/`onDeactivate` from SDL focus events (§1.2). CiM pauses on deactivate.
12. **Text entry**: `keyChar` routing plus `gsEditCtrl` insert, `onCheckInput`/`rejectInput`, `maxLength`,
    `onChange`, caret keys and Ctrl C/X/V (§7). This is needed for save names and login, not for the main menu.

Deliberate-deviation candidates (decide explicitly): Alt being dead on Windows, the Linux mouse-button-4/5 key reuse
bug, `WM_PAINT` running an extra tick, the pointer lag of one tick, and margins shrinking the hit area.

---

## 10. Open questions and confidence

| Topic | Status |
|---|---|
| OS message/event tables (§1.1-1.2), key enum and VK map (§1.4) | Observed, **H** |
| Mouse units: relative = px / window height on both axes; absolute = x/W, y/H; desktop rescale (§1.3, §3.2-3.3) | Observed, **H** |
| Target list selection and focus calls (§2.2) | Observed in both builds, **H** |
| `gsDesktop::mouseMove` order and old-position hit test | Observed; the first argument of the hit-control call is lost on Windows (x87) and taken from Linux `L:0x081c6330:64-67`. **H** |
| Hit area = client rect (margins) | Observed in `controlAt` (`W:0x100a97a0:37-41`). Unusual enough to deserve a runtime check. **M-H** |
| Status bits (1 = enabled, 2 = visible) | Inferred from the `visible`/`enabled` setters `W:0x1009faa0`/`0x1009fad0` → `clearStatus(2)`/`(1)`. **H** |
| Z-order = child index (front-most hit first) | Inferred (`setControlIndex` from `updateChildren`, `bringToFront` = `moveToLast`). The GUI renderer's sort by that index has not been re-checked. **M** |
| Static WndProc `FUN_10057da0` → slot 67 | Not read. Assumed to look up the object by HWND (`W:0x10059200` registers the map) and forward. **M** |
| `gsGridPlayer`/`gsEditor` mouse overrides | Only skimmed: they call the base handler, and the editor returns false while rotating. Their game logic is out of scope. |
| Linux `getGsKey` | Partially enumerated (the Ghidra switch has range tricks for F3/F5 and the right-hand modifiers). Mapping by name is safe. |
| `gsEditCtrl` Windows offsets and `setActive` caret behaviour | Taken from the Linux build; not cross-checked line by line. **M** |
| Whether `gsControl::move` (tick) touches input state | Grep found no hover, capture or drag fields in it. Assumed no. **M** |
