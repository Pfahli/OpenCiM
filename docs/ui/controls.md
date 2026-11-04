# GSystem GUI controls: how a `gsControl` becomes pixels

Scope: the script class `gsControl` and the subclasses the main menu uses (`gsDesktop`, `gsViewPort` as a control,
`gsTextSelCtrl`/`gsEditCtrl` for dialog inputs). It covers the fields, the native GUI items each control owns, the
exact geometry and colour of every item, text layout and formatting, and what happens per frame. Chart, map and
tracer controls are out of scope (section 11).

This document builds on, and does not repeat:

* `docs/renderer/interfaces-2d-output.md` ("2D doc"): render tree, layer sort, clip-area scissor, `gsIGuiPrimitive`,
  `gsIGuiText`, GUI coordinate system, GL state of the GUI pass.
* `docs/handlers/batch-B.md`: the script handlers `setRect`, `setCropRect`, `setImage`, `setShader`, `setArg`,
  `setMargins`, `blend`, `setBorder`, `setFont`, `hide`/`show`.
* `docs/native-api.md`: the script-visible members and events.

**Sources and conventions**

* Windows evidence: `decompiled/GSystem-v2/functions/0x<addr>.c:<line>` (Ghidra pseudocode of GSystem.dll).
  Linux evidence: `L:0x<addr>` = `decompiled/Linux/functions/0x<addr>.c` (same game version, GCC build with symbols).
  Function names come from the Linux build (`linux-names.csv`).
* x87 float arguments that Ghidra drops were recovered from the disassembly (Ghidra headless, read-only, script
  `DumpAsm.java`). Those spots are marked **(asm)**.
* **All offsets are Windows offsets** from the start of the object. Linux offsets differ: for gsControl fields
  `Linux = Win − 0x38` for `+0xD4..+0x14C`, raw text `+0x168` is Linux `0x118`, and `Linux = Win − 0x68` from
  `+0x184` on (MSVC `std::wstring` is 0x1C bytes, GCC's is 4). §2.2 lists both. Linux vtable offsets also differ, so
  vtable slots are given as Windows slots.
* `metro/...` and `scripts/...` paths are under `extracted/`.
* "Observed" means the code shows it. "Inferred" is marked and explained. Confidence H/M/L where it matters.
* Colours are `0xAARRGGBB`. `c % a` means "colour c with its alpha byte replaced by a" (`FUN_10074e90`).
  `mulA(c, v)` means `gsColor::multAlpha` (`FUN_10074ef0`): RGB unchanged,
  `A' = (uint8_t)_ftol((float)A * v)`, i.e. **truncation toward zero, no clamping** (asm 0x10074f15-0x10074f2a).
  `v` outside [0,1] wraps through the low byte.
* Units: whatever the enclosing viewport uses. CiM's GUI root viewport has `pixelCoordinates = true`
  (`metro/scripts/window.script:49-54`), so in practice **every gsControl number is in pixels**, origin top-left,
  y down. Fonts are sized in pixels (`setFont(..., 15, 15, ...)`).

---

## 1. Mental model

```
gsViewPort (script control, native gsIViewport at +0x20C)      ← getClientArea() of the viewport = the viewport
└─ gsDesktop D (z = its index in the viewport's child list)
   ├ image  D.img   layer z·256        ┐ live in the OWNER's client area
   ├ border D.brd   layer z·256+1      ┘
   └ clip   D.clip  layer z·256+0x80   ← D's own client area (rect = client rect, scissor)
        ├ text   D.txt   layer −2      ← own text, always under the children
        ├ child0.img 0, child0.brd 1, child0.clip 0x80 [child0.txt −2, grandchildren …]
        ├ child1.img 256, child1.brd 257, child1.clip 0x180 [ … ]
        └ …
```

* A control's **image and border are drawn in its parent's client area**; its **text and its children are drawn in its
  own client area**. Children of the same parent are ordered by their index `z` in the parent's child vector.
* Parent text (layer −2) is drawn **before** all children (layers ≥ 0) of that parent.
* Clip areas do not translate (2D doc §4.3). The control therefore stores absolute "desktop" coordinates.
* Nothing is created eagerly. A plain `new_control` owns no GUI item until a setter needs one (section 2).

---

## 2. Field layout (gsControl, 0x204 bytes, ctor `0x100a8d90`)

Size: 0x204 is inferred (gsTextSelCtrl's first own field is at +0x204, `0x100aff70.c`). Defaults are from the ctor
`0x100a8d90.c:28-90` / `L:0x081cc940`. The white/black constants are `DAT_101974b4 = 0xFFFFFFFF` (`0x10150550`)
and `DAT_10197464 = 0xFF000000` (`0x10150530`).

### 2.1 Inherited gsModule fields used here

| Win off | Meaning |
|---|---|
| +0x40 | gsVideo of the window tree (null → no GUI items are ever created) |
| +0x44 | parent module |
| +0x4C/+0x50 | child vector (begin/end). **Index in this vector = child's `z`** (all modules count, see §8) |
| +0x68 / +0x6C | own / effective flags: bit0 enabled, **bit1 visible**, bit2 audible. Default `0xFFFF0005` = **hidden** (`L:0x08135500`); scripts set `.visible = true`. Effective = own & parent effective. |
| +0xCC | gsMoveable sub-object (per-frame tick, `0x100aa2f0` runs with `this = control+0xCC`) |

### 2.2 gsControl fields

| Win off | Linux | Type | Default | Script / setter | Meaning |
|---|---|---|---|---|---|
| +0xD4 | 0x9C | gsControl* | 0 | (parent on `new_control`) | **owner control**: `dynamic_cast<gsControl>(parent)` in `setOwner` (`L:0x081c8df0`). Null if the parent is not a control. |
| +0xD8..+0xE4 | 0xA0 | float×4 | 0 | `left top width height`, `setRect/setPosition/setSize` | rect relative to the owner's **client** origin |
| +0xE8/+0xEC | 0xB0 | float | 0 | `desktopLeft/Top` (ro) | absolute position (§3) |
| +0xF0/+0xF4 | 0xB8 | float | 0 | `clientWidth/Height` (ro) | `width−mL−mR`, `height−mT−mB` |
| +0xF8 | 0xC0 | ARGB | `0x00FFFFFF` | `background` (sets **both** +0xF8 and +0xFC) | image vertex colour |
| +0xFC | 0xC4 | ARGB | `0x00FFFFFF` | `hotBackground` (sets +0xFC only) | image colour while hot or active |
| +0x100 | 0xC8 | ARGB | `0x00000000` (black % 0) | `setBorder` arg 0 | border colour |
| +0x104 | 0xCC | ARGB | `0xFFFFFFFF` | `setFont` arg 1 | text colour |
| +0x108 | 0xD0 | ARGB | `0x80000000` (black % 0x80) | `setTextShadow` arg 0 | text shadow colour |
| +0x10C | 0xD4 | float | 0 | `setBorder` arg 1 | border width |
| +0x110 | 0xD8 | uint | **0** (method default 0xF) | `setBorder` arg 2 | sides: TOP 1, BOTTOM 2, LEFT 4, RIGHT 8 |
| +0x114 | 0xDC | vec3 | (0,0,0) | — | computed text position (§6.2) |
| +0x120 | 0xE8 | vec3 | (0.015, 0.015, 1) | `setFont` args 2,3 | char size (x, y, 1) |
| +0x12C | 0xF4 | vec3 | (0,0,0) | `setFont` args 4,5 | char spacing (x = extra advance, y = extra line gap) |
| +0x138 | 0x100 | gsMaterial_OGL* | 0 | `setImage`/`setShader` (lazy) | image material, fixed-function by default (shader +0xC null, `L:0x08214280`) |
| +0x13C | 0x104 | gsIGuiPrimitive* | 0 | — | **image** (§4) |
| +0x140 | 0x108 | gsIGuiPrimitive* | 0 | — | **border** (§5) |
| +0x144 | 0x10C | gsIGuiText* | 0 | — | **text item** (§6) |
| +0x148 | 0x110 | gsIClipArea* | 0 | — | **own client area** (§7) |
| +0x14C | 0x114 | wstring | "" | — | **display text** = formatted `text` (what is drawn and measured) |
| +0x168 | 0x118 | wstring | "" | `text` | raw text as set by script |
| +0x184 | 0x11C | bool | 1 (0 in gsTextSelCtrl/gsEditCtrl) | — | `%`-formatting enabled |
| +0x185 | 0x11D | bool | 0 | — | text contains format codes → reformat every tick |
| +0x186 | 0x11E | bool | 0 | `active` → `setActive` | active (selects hotBackground) |
| +0x187 | 0x11F | bool | **1** | `canActivate` | |
| +0x188 | 0x120 | bool | 0 | `clickThrough` | hit test skips the control and its subtree |
| +0x189 | 0x121 | bool | 0 | `clickThroughSelf` | hit test may return children, not the control |
| +0x18A | 0x122 | bool | 0 | — | **hot** (mouse over this control or a descendant) |
| +0x18B | 0x123 | bool | 0 | — | left button held on the control (drag) |
| +0x18C | 0x124 | bool | 0 | — | **clipped** (§3) |
| +0x190..+0x1B4 | 0x128 | gsVariant*[10] | `none` | `setArg(i, v)` / `getArg` | format arguments |
| +0x1B8/+0x1BC | 0x150 | float | (uninit) | — | mouse-down position (drag origin) |
| +0x1C0 | 0x158 | gsControl* | 0 | — | active child |
| +0x1C4 | 0x15C | gsControl* | 0 | `dragTarget` | control moved while dragging |
| +0x1C8 | 0x160 | bool | 0 | — | blend running |
| +0x1CC | 0x164 | float | 1.0 | `blend` arg 0 | blend target |
| +0x1D0 | 0x168 | float | 1.0 | — | blend rate per second |
| +0x1D4 | 0x16C | float | 1.0 | `visibility` | **alpha factor of this control's own items** |
| +0x1D8..+0x1E4 | 0x170 | float×4 | 0 | `setMargins(l,t,r,b)` | margins |
| +0x1E8 | 0x180 | uint | **9** (TOP\|LEFT) | `textAlign` | `ALIGN_*` flags (§6.2) |
| +0x1EC | 0x184 | int | 0 | — | **z**: index in the owner's child vector |
| +0x1F0..+0x1FC | 0x188 | float×4 | (0, 0, 1, 1) | `cropLeft/Top/Width/Height` (ro), `setCropRect`, `setTiling(w,h)` | uv rect of the image |
| +0x200 | 0x198 | int | −1 | `maxTextLength` | truncation of **formatted** text only (§9) |

`setTiling(u, v)` is not a separate feature: it writes `cropWidth = u`, `cropHeight = v` (`0x100a6360`,
`L:0x081c5f20`). `setTiling(1, -1)` flips vertically and `setTiling(-1, 1)` mirrors (relies on `GL_REPEAT`, §4).

### 2.3 Script constants (registered by `gsControls_init`, `L:0x081cdc50`)

`BORDER_TOP 1, BORDER_BOTTOM 2, BORDER_LEFT 4, BORDER_RIGHT 8`; `ALIGN_TOP 1, ALIGN_MIDDLE 2, ALIGN_BOTTOM 4,
ALIGN_LEFT 8, ALIGN_CENTER 0x10, ALIGN_RIGHT 0x20`. There are **no other TEXT_\*/wrap/multiline constants**: wrapping is
implied by left alignment (§6.2). The same init registers `new_control`, `new_desktop`, `expandPattern`,
`escapePattern`.

---

## 3. Layout: `updateDimensions` (slot 0xE4, `0x100a95a0`, `L:0x081c7020`)

Called by `setDimensions` (slot 0x12C) only **when one of left/top/width/height actually changed**, then the event
`onChangeRect` (16) fires. `setPosition`/`setSize` and the field setters go the same way (`L:0x081c75f0`).

```
P = owner (+0xD4)
if P == null:  desktopL = left; desktopT = top; clipped = false
else:
   desktopL = P.marginL + P.desktopL + left          // 0x100a95a0.c:25-28
   desktopT = P.marginT + P.desktopT + top
   clipped  = left >= P.width || top >= P.height     // P's full size, not its client size
           || left + width <= 0 || top + height <= 0
clientW = width  - marginL - marginR
clientH = height - marginT - marginB
if image  exists: updateBackGround()     (slot 0xE8)
if clip   exists: updateClientArea()     (slot 0xF4)
if text   exists: updateTextPos()        (slot 0xEC)
if border exists: updateBorder()         (slot 0xF0)
for each child that is a gsControl: child.updateDimensions()
```

* `clipped` only compares against the immediate owner. It is an early-out. Real clipping is the scissor of the
  owner's client area.
* `setMargins` (`0x100a6f10`) recomputes client size, text position and clip rect, but **does not relayout children**
  (batch-B). Their desktop position, which includes the parent margins, updates at the next `updateDimensions`.
* Effective visibility used by every item: `vis = (flags+0x6C & 2) && !clipped`.

---

## 4. Image primitive (+0x13C): `updateBackGround` (slot 0xE8, `0x100a7860`, `L:0x081c7910`)

Returns immediately if video (+0x40) is null.

```
c  = (hot +0x18A || active +0x186) ? hotBackground : background        // 0x100a7860.c:55-58
c  = mulA(c, visibility +0x1D4)                                         // (asm) 0x100a789b: arg = [ESI+0x1D4]
wl = sides&LEFT ? bw : 0;  wt = sides&TOP ? bw : 0                       // bw = border width +0x10C
wr = sides&RIGHT? bw : 0;  wb = sides&BOTTOM? bw : 0
L = desktopL; T = desktopT; W = width; H = height; (u,v,cw,ch) = crop
v0 = (L+wl,     T+wt,     0)  uv (u,    v   )
v1 = (L+W-wr,   T+wt,     0)  uv (u+cw, v   )
v2 = (L+W-wr,   T+H-wb,   0)  uv (u+cw, v+ch)
v3 = (L+wl,     T+H-wb,   0)  uv (u,    v+ch)
all four vertices: colour c
```

* **Margins do not shrink the image**; the border insets it (so image and border do not overlap).
* Creation on first call: `video.newGuiPrimitive(7, owner ? owner.getClientArea() : null)` (`7` → `GL_QUADS`),
  `setVertexCount(4)`, `setLayer(z << 8)` (`0x100a7860.c:150-158`). With a null owner the primitive has no parent and
  is never drawn.
* Every call: `if material: prim.setMaterial(material)`, `prim.setVisible(vis)`, `prim.setVertices(0, 4, v)`.
  No rotation is ever set on a control image (angle stays 0).
* Rendering (2D doc §2.4, `L:0x081ff990`): fixed function, `GL_MODULATE` (no `glTexEnv` call, so the GL default),
  texture unit 0 = material texture 0 if present, otherwise **untextured** (solid colour). Units 1-3 only matter for
  material shaders (`setShader`, e.g. `metro/shaders/menu/anim.shader`). Blend `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`.
* Texture wrap: negative or >1 crop values (`setTiling(1,-1)`, the menu animation's scrolling
  `setCropRect(-t·0.005, 0, 1, 1)`) need **`GL_REPEAT`**. That is the GL default and what `uploadBGRA` sets. The DDS
  loader's own `glTexParameter` arguments are lost (2D doc open question 2), so REPEAT for DDS is *inferred* (M).
* **Who creates the image**: writing `background`/`hotBackground` (`0x100a6340`/`0x100a63d0` call 0xE8
  unconditionally), `setImage`, `setShader`. A control that never got a colour or image has **no image primitive**.
  `background = 0` creates a fully transparent quad.
* `setImage(tex|path)` (batch-B) binds the texture to material slot `n` and then sets
  **background = hotBackground = 0xFFFFFFFF** (`0x100a8b22`: `PUSH 0xff` (asm) → `white % 0xFF`,
  `0x100a62c0.c:24-27`). A background written after `setImage` tints the image. `setImage(null)` with an existing
  material clears the texture **and** sets the colours to opaque white, giving a white quad (observed data flow; the
  visual result is inferred).

**Hot.** `mouseOver` sets +0x18A, calls `owner.mouseOver()` first (so every ancestor becomes hot too), and rebuilds
the image only if `hotBackground != background` (`0x100a6220.c:17-23`). `mouseOut` mirrors it. `setActive` rebuilds
under the same condition (`L:0x081ca110`). Hover target changes are driven by `gsDesktop::mouseMove`
(`L:0x081c6330`): old target `mouseOut`, new target `mouseOver`.

---

## 5. Border primitive (+0x140): `updateBorder` (slot 0xF0, `0x100a7190`, `L:0x081c7e80`)

```
c = mulA(borderColor +0x100, visibility)                 // 0x100a7190.c:77, (asm) 0x100a71a7
wl, wt, wr, wb as in §4 (0 for a side not in the mask)
Pi (inner) / Po (outer) corners:
  oTL=(L,T)        oTR=(L+W,T)        oBR=(L+W,T+H)        oBL=(L,T+H)
  iTL=(L+wl,T+wt)  iTR=(L+W-wr,T+wt)  iBR=(L+W-wr,T+H-wb)  iBL=(L+wl,T+H-wb)
16 vertices = 4 quads (GL_QUADS), uv = 0, z = 0:
  q0 TOP    : iTL oTL oTR iTR      colour c, alpha forced 0 unless sides & TOP
  q1 BOTTOM : iBR oBR oBL iBL      … unless sides & BOTTOM
  q2 LEFT   : iTL oTL oBL iBL      … unless sides & LEFT
  q3 RIGHT  : oTR iTR iBR oBR      … unless sides & RIGHT
```

* Disabled sides are **not skipped**: their quads are still submitted with alpha 0 (`0x100a7190.c:96-101`: the loop
  clears the alpha byte of vertex `i` when `!(sides & (1 << (i>>2)))`).
* Corners are mitred (each side is a trapezoid sharing the corner diagonal). Width is in GUI units (px), drawn
  **inside** the control rect.
* No material, so it is untextured. Parent = owner's client area, layer `z·256+1`, `setVertexCount(0x10)`,
  visible = `vis` (`0x100a7190.c:207-224`).
* Created by the first `setBorder` call (slot 0x154 always calls 0xF0, `L:0x081c5fd0`), even with width 0. Later
  rebuilt by layout and `setVisibility` only if it exists.

Example (`mainmenu.script:88-94`): `$leftEdge` rect (0,0,5,H), `setBorder(0xFF5C5147, 2, BORDER_LEFT)`,
`background = 0xFF18120E`. Image quad x ∈ [2,5]; left border quad (2,0)(0,0)(0,H)(2,H) in `0xFF5C5147`; the other
three quads are transparent (top/bottom are degenerate, height 0).

---

## 6. Text item (+0x144)

### 6.1 Creation and the colour/size state

`needText` (`FUN_100a7bf0`, `L:0x081ca270`) and the inline copy in `setText` (`0x100aa4f0.c:80-93`):

```
if text == null && video != null:
   text = video.newGuiText(3, this.getClientArea())     // OWN client area (creates it, §7)
   text.setVisible(vis)
   text.setLayer(-2)                                    // (asm) 0x100a7c48 PUSH -0x2
```

Created by `setText` (`text = …`), `setFont`, `setTextShadow` (`0x100a8978` calls it unconditionally). The item's
own defaults (2D doc §2.5) are font `fonts/verdana`, size 0.015, colour white, **shadow colour 0**, shadow offset
(0.001, 0.001). The control pushes its state only through these calls:

| Call | Pushed to the text item |
|---|---|
| `setFont(font, color, sx, sy, [spx=0], [spy=0], [extra=""])` (slot 0x158, `0x100a8870`) | stores +0x104/+0x120/+0x12C, then `setFont(font, extra)`, `setColor(mulA(textColor, vis))`, `setCharSize((sx,sy,1))`, `setSpacing((spx,spy,0))`, `updateTextPos()`. Defaults for the optional args are 0, 0, "" (**asm** 0x100a9b16-0x100a9b46: `EBX = 0`, `EBP = 0`, `DAT_10156888 = ""`). |
| `setTextShadow(color, dx, dy)` (slot 0x15C, `0x100a8960`) | stores +0x108; `setShadowColor(mulA(shadow, vis))`; `setShadowOffset((dx,dy,0))` (offset is **not** stored in the control) |
| `setVisibility(v)` (slot 0x16C, `0x100a6440`) | `setColor(mulA(+0x104, v))`, `setShadowColor(mulA(+0x108, v))` |
| `updateText` (slot 0x100, `0x100a85b0`) | `setText(display string +0x14C)` |
| `updateTextPos` (slot 0xEC) | `setBox`, `setVisible(vis)`, `setPosition` (§6.2) |

Consequences (observed data flow):

* **Without `setFont` the text uses the item defaults**: verdana at 0.015 units. In CiM's pixel GUI that is 0.015 px,
  i.e. invisible. All shipped UI calls `setFont` (`ui.script:920-940`).
* The 7th `setFont` argument is a glyph supplement: scripts pass `MAIN.$language` (`ui.script:921`). The font cache
  merges `<font>/<extra>.fnt|.font` glyphs into the font (2D doc §1.4).
* **The control's default shadow (`0x80000000`) is not applied until `setVisibility` or `setTextShadow` runs.** After
  any `.visibility = x` or `blend()`, the item gets `mulA(0x80000000, v)` with the item's default offset
  (0.001, 0.001), i.e. a half-transparent black copy under the text, offset by 1/1000 px. *Inferred visual effect*:
  slightly darker glyph edges. A port must reproduce the state, not "no shadow".
* `visibility` modulates text and shadow alpha only through these calls. Inline-coloured runs are scaled by the base
  colour's alpha in the renderer (§6.4).

### 6.2 Alignment and position: `updateTextPos` (slot 0xEC, `0x100a85e0`, `L:0x081c6ae0`)

Only does anything if the text item exists. `s` = display string (+0x14C). `tw(s)`/`th(s)` = the item's
`measureWidth`/`measureHeight` (§6.3). `cs` = control char size (+0x120).

```
pos = (0,0,0)
if align & ALIGN_RIGHT (0x20):
     pos.x = (L + W - mR) - tw(s);                    box = (10000, 10000, 0)
elif align & ALIGN_CENTER (0x10):
     d = (cW - tw(s)) * 0.5;  if cs.x >= 4.0: d = trunc(d)
     pos.x = d + mL + L;                              box = (10000, 10000, 0)
else (LEFT, the default):
     pos.x = mL + L;                                  box = (cW, cH, 0)      ← enables word wrap at cW
text.setBox(box)                                      // BEFORE the vertical measurement
if align & ALIGN_BOTTOM (4):
     pos.y = (T + H - mB) - th(s)
elif align & ALIGN_MIDDLE (2):
     d = (cH - th(s)) * 0.5;  if cs.y >= 4.0: d = trunc(d)
     pos.y = d + mT + T
else (TOP):
     pos.y = mT + T
text.setVisible(vis); text.setPosition(pos)
```

Evidence: `0x100a85e0.c:36-136`; constants 4.0 (`[0x1015e87c]`), 0.5 (`[0x101593c0]`) and 10000 (`0x461c4000`) from
the asm; `trunc` is `_ftol2` (asm 0x100a86a8, 0x100a87d0).

* Priority: RIGHT > CENTER > LEFT and BOTTOM > MIDDLE > TOP. The LEFT and TOP bits are never tested.
* **Wrapping happens only for left-aligned text** (box = client size). Centred/right text never wraps; each line
  starts at `pos.x`, so a multi-line centred block is centred by its widest line and the lines are left-aligned within
  the block.
* The `>= 4.0` test is a "pixel mode" heuristic: in pixel GUIs, centred/middle offsets snap to whole pixels.
  **Windows truncates toward zero; the Linux build rounds to nearest (x87 `FIST`, `ROUND` in `L:0x081c6ae0`).** For
  negative `d` (text larger than the box) and `.5` cases the builds differ. Port Windows.
* `box.y` (cH) is stored but never read by the renderer (only `box.x` is the wrap width).
* Text is clipped to the own client area (§7), so margins also clip text.

Main-menu example (`mainmenu.script:122-131` + `ui.script:213-221,928-931`): button rect (4, 26+38n, 322, 32),
`ALIGN_MIDDLE|ALIGN_CENTER`, font `metro/fonts/default-9` 15×15, spacing (−2, 0), colour `0xFFD6D6D6`, text
uppercased. `th` = 15 (one line), so `pos.y = T + trunc((32−15)·0.5) = T + 8` (Linux also gives 8: 8.5 rounds to
even). `pos.x = L + trunc((322 − tw)·0.5)`.

### 6.3 Measuring (gsGuiText_OGL; Windows `0x10044230`/`0x10043b60`/`0x10043d80`, Linux `L:0x081ff4d0`/`L:0x081ff2b0`/`L:0x081ff930`)

Glyph data per char (`gsCharData_OGL`, node+0x14): `u, v, w, h` (uv rect), `xo, yo` (offset), `adv`, `lh` (line
height), `page`. All are in texture-normalised units. For the native `.font` format `xo = yo = 0`, `adv = w` and `lh`
is a **font-wide** value, the `h` of glyph record 0 (`L:0x081f5400`, lines 236-290). The BMFont path is not covered
here. Missing glyphs fall back to the font's **lowest-code glyph** (map begin, `L:0x081f6410`).

```
adv(c)  = adv_c / lh_c * size.x + spacing.x              (world units)
measureWidth(s):  widest line; lines split at '\n'; skip code 4 (+8 payload chars), 5, '\r';
                  every other char, '\t' included, adds adv(c)          ('\t' → fallback glyph!)
measureHeight(s): h = size.y; x = 0; tab = 0
   '\n'           : h += size.y + spacing.y; x = tab = 0
   '\t'           : tab += 3·size.x + 3·spacing.x; x = tab
   4 (+8) / 5 / '\r': nothing
   ' '            : x += adv(' '); look ahead over the next word (chars > 0x20 with lh ≠ 0, skipping 4/5):
                    if x + word > box.x → h += size.y + spacing.y; x = tab = 0   (the space is consumed)
   other c, lh_c>0: x += adv(c); if x > box.x → h += size.y + spacing.y; x = adv(c)
charAdvance(c):   0 for 4, 5, '\n', '\r'; else adv(c)
```

An empty string measures `size.y` high and 0 wide. `getTextWidth()`/`getTextHeight()` return these values for the
display string, and 0 if there is no text item (`L:0x081cac30`). `visibleLines()` = `floor(height / charSize.y)`.

### 6.4 Drawing (`gsGuiText_OGL::render` `0x10043df0`, `gsFont_OGL::renderText` `0x1003d410` / `L:0x081f65b0`)

The 2D doc §2.5 describes the GL calls. Exact semantics (Linux switch, confirmed by Windows asm offsets):

```
k = 1 / size.x
if shadowColor.A != 0:                                   // shadow pass first
   glTranslate(pos + shadowOffset); glScale(size)
   renderText(shadowColor, bg, x0 = xOffset·k, inline = false, wrap = box.x·k, spx = spacing.x·k, spy = spacing.y·k)
glTranslate(pos); glScale(size)
renderText(color, bg, xOffset·k, inline = true, box.x·k, spacing.x·k, spacing.y·k)
```

`renderText`, in glyph units (1 unit = 1 char height), pen `x` starting at `x0`, line `y` starting at 0:

| char | action |
|---|---|
| 4 | inline colour: next **8 chars = hex `AARRGGBB`** (`gsColor(wchar*)`, `L:0x080ee070`). In the main pass `colour = rgb(code), A = (A_code · A_base) >> 8`; skip 9 chars. In the shadow pass it is skipped only. |
| 5 | main pass: back to the base colour. Shadow pass: skipped. |
| `\t` | `tab += 3·spx + 3; x = tab` (absolute tab stops counted per line; **not** "next multiple", and it can move left) |
| `\n` | `x = 0, tab = 0, y += 1 + spy` |
| `\r`, glyph with `lh ≤ 0` | skipped |
| `' '` | word-wrap look-ahead as in measureHeight; on wrap: new line, space not drawn; else `x += adv` |
| other | `a = adv/lh + spx`; if `x + a > wrap`: new line first. If `bg.A ≠ 0`: untextured quad (x,0)-(x+a,1) in `bg`. Then the glyph quad (below); `x += a` |

Glyph quad (`renderChar` `0x1003d290`): with `T` = page texture size field (+0x20, height per the 2D doc's texture
layout), `e = 1/(T·lh)` and `t = 1/T`, relative to the pen:
`(xo/lh−e, yo/lh−e)` uv `(u−t, v−t)`, `(xo/lh−e, (yo+h)/lh+e)` uv `(u−t, v+h+t)`,
`((xo+w)/lh+e, (yo+h)/lh+e)` uv `(u+w+t, v+h+t)`, `((xo+w)/lh+e, yo/lh−e)` uv `(u+w+t, v−t)`.
The pad is **one texel** on each side (the 2D doc says half a texel; this is the correction).

Quirks to keep:

* Line advance when drawing is `size.y · (1 + spacing.y/size.x)`, but measuring uses `size.y + spacing.y`. They agree
  only for `size.x == size.y`, which holds for every shipped `setFont` call.
* `(255·255) >> 8 = 254`: an opaque inline colour on opaque text gets alpha 254.
* The background highlight quad is also drawn in the shadow pass, offset by the shadow offset.

---

## 7. Own client area (+0x148): `getClientArea` (slot 0x17C, `0x100a67d0`) / `updateClientArea` (slot 0xF4, `0x100a7120`)

```
getClientArea():
   if clip == null:
      clip = video.newClipArea(0, owner ? owner.getClientArea() : null)
      clip.setLayer(z·256 + 0x80)
      updateClientArea()
   return clip
updateClientArea():
   clip.setVisible(vis)
   clip.setRect(desktopL + marginL, desktopT + marginT, clientW, clientH)
```

* **When a control gets one**: lazily, the first time something must be drawn inside it. That means its own text
  (setText/setFont/setTextShadow), any child creating an image/border/text/clip area, a nested `gsViewPort`
  (`L:0x081dbf90`) and the gsTextSelCtrl/gsEditCtrl selection and caret items. The chain is recursive up to the
  root.
* `gsViewPort` overrides nothing here: its +0x148 is set at create time to its native `gsIViewport`
  (`L:0x081dbf90`: `this+0x110 = viewport.addRef()`), so the viewport node *is* its client area. The inherited
  `updateClientArea` then keeps the viewport rect = the client rect.
* Scissor = parent scissor ∩ rect (2D doc §2.1, including the "right edge omits origin" quirk inside offset nested
  viewports).

---

## 8. Z order (`setControlIndex`, slot 0xE0, `0x100a6140`)

`gsControl::updateChildren` (slot 0x9C, `0x100a9750`) calls `child.setControlIndex(i)` for every child that is a
gsControl, with `i` = **position in the module child vector, counting non-control children too** (sounds, scenes and
so on). `setControlIndex` stores `z` (+0x1EC) and relayers the existing items: clip `z·256+0x80`, border `z·256+1`,
image `z·256`. Slot 0x9C runs after child add/remove and after `moveToLast`/`moveToFirst` (`0x100a2720`), which back
`bringToFront`/`sendToBack`.

**Activation reorders.** `setActive(true)` (needs `canActivate`, which defaults to **true**) fires `onActivate`,
calls `owner.setActiveChild(this)`, then `bringToFront()` and `show()` (`L:0x081ca110`). A mouse-button key
(0x8F-0x91; 0x8F is the left button per `L:0x081cab30`) in `gsDesktop::keyDown` (`L:0x081c65e0`) calls `activate()`
on the hit control, which activates it and then recursively its owner (`L:0x081c6180`). Clicking a control that can be activated therefore moves it (and its
activatable ancestors) to the top of their siblings. Main-menu scripts set `canActivate = false` almost everywhere.

---

## 9. Text formatting

### 9.1 `setText` (slot 0x160, `0x100aa4f0`, `L:0x081ca740`)

```
raw (+0x168) = s; display (+0x14C) = s
if formatEnabled (+0x184):
   hasCodes (+0x185) = false; sawEscape = false
   for i over raw: if raw[i]=='%':
        if i+1 < len and raw[i+1] != '%': hasCodes = true; break
        sawEscape = true; i += 1                     // "%%" pair, or a trailing '%'
   if !hasCodes and sawEscape:                      // static text: collapse escapes once
        for i over display: if display[i]=='%': erase(i, 1)   // then i += 1
ensure text item (§6.1); if none (no video) → return
if hasCodes: display = ""                           // filled on the next tick (§10)
updateText(); updateTextPos()
```

* Static text with `%%` shows `%`. A lone trailing `%` is also removed (`"50%"` → `"50"`).
* Dynamic text (`hasCodes`) shows **nothing until the next tick**. `getTextWidth()` right after `.text = "%0"` returns 0.
* gsTextSelCtrl/gsEditCtrl clear +0x184, so their text is never formatted.
* `maxTextLength` (+0x200) is applied only to dynamic text after formatting: `if max != −1 && len > max: erase(max, end)`
  (`0x100aa2f0.c:51-64`).

### 9.2 Format grammar: `gsControl::formatText(pattern, args, argc)` (`0x100a9cb0`, `L:0x081cb490`)

The same static function serves dynamic control text (`args = +0x190`, `argc = 10`, asm 0x100aa328 `PUSH 0xa`) and
the script function `expandPattern(pattern, a0, a1, …)` (`0x100aa680`, `argc = nargs − 1`). Output is built left to
right. Characters other than `%` are copied. After a `%`:

| Code | Meaning | Evidence |
|---|---|---|
| `%%`, `%x` (x not listed below) | outputs `x` (the `%` is dropped) | `0x100a9cb0.c:273`; `L:0x081cd2a0` copies the char |
| `%D` (D = `0`-`9`) | `args[D].toString()` (variant slot +0x28). Out of range or null → nothing. | `:136-160` |
| `%!DD` | two-digit index `10·D1 + D2` (only useful for `expandPattern` with > 10 args) | `:109-134` |
| `%i…D` | integer: `args[D].toInt()` (slot +0x20), with options read before the index digit: | `:168-232`, `:LAB_100aa0d1` |
| ↳ `tX` | thousands separator `X` (inserted every 3 integer digits) | `:182-195` |
| ↳ `pN` | minimum total width `N` (single digit), padded with `'0'` **on the left of everything, including the `-`** | `:197-205` |
| ↳ `dXN` | decimal separator `X`, `N` (digit) fraction digits: the value is fixed-point, `value / 10^N` | `:207-226` |
| `%f[dXN]D` | float `args[D].toFloat()`, intended `"%#.Nf"` with default N = 3. **Broken in both builds** (below). | `:236-268` |

Options may come in any order and repeat. The first char that is not `t`, `p`, `d` is the argument index. If the
pattern ends inside a code, **formatting stops and the rest is dropped** (for example `de.strings` "…%it,d.." ends the
string there).

Integer algorithm (Windows `0x100a9cb0.c:LAB_100aa0d1`-`LAB_100aa1a8`, verified by hand on the cases below):

```
n = |v|; neg = v < 0; out = "" (built right to left); pos = 1
dec = (d given) ? N+1 : 0;  grp = dec
do {
   q = n / 10; prepend('0' + n%10); pos++; n = q
   if pos == dec:                       prepend(decSep); pos++
                                        if q == 0: dec = grp = dec+1  (keeps looping to print the leading "0")
   elif (pos - grp) % 4 == 0 and q != 0 and thSep: prepend(thSep); pos++
} while (q != 0 || pos <= grp)
if neg: prepend('-'); pos++
while pos <= pad: prepend('0'); pos++   // total length = pad
```

| Pattern | Arg | Result |
|---|---|---|
| `%it,0` | 1234567 | `1,234,567` |
| `%it,d.20` | 123456 | `1,234.56` |
| `%it,d.20` | 5 | `0.05` |
| `%it,d.20` | 0 | `0.00` |
| `%it,d.21` | 125 (arg 1) | `12.5` |
| `%ip25` | 7 (arg 5) | `07` |
| `%i0%%` | 42 | `42%` |
| `%ip30` | −5 | `0-5` (pad goes before the sign) |

The table was checked by running a literal transcription of the decompiled loop.

Shipped uses (`metro/strings/en_US.strings`): `"%it,0"` (money, counts), `"%it,d.22"`/`"%it,d.23"` (banks, value in
cents), `"%it,d.10"` (roster distance), `"%0 {0:- %1.%2.%ip43 %4:%ip25}."` (save list: year padded to 4, minutes to 2),
`"Show last %i0 months"`. `%f` is never used in shipped content.

**`%f` bug (asm 0x100a9f17/0x100a9f56):** the precision char is converted with `c − '0'` and then again with
`− '0'` while building the format string `{'%','#',sep,prec,'f',0}`. The precision wchar becomes `3 − 0x30 = 0xFFD3`
for the default, so `swprintf(buf, 32, L"%#.ￓf", x)` is an invalid conversion. The MSVC CRT behaviour (invalid
parameter handler) and glibc's (`__swprintf_chk`) are untested. The Linux build has the same bug
(`L:0x081cd2a0`). Recommendation: implement the intent (`%#.{N}f` with separator `.`) and log it.

### 9.3 Inline colours and links (script side)

* `formatText(text, colors[])` (script, `0x10075ad0` / `L:0x080ed820`) rewrites `{N:` to `\x04` + 8 uppercase hex of
  `colors[N]` (ARGB) and `}` to `\x05`. Any other `{` syntax raises runtime error 0xF. These are the codes of §6.4.
* `escapePattern(s)` (`0x100a9a50`) doubles every `%`. Usage:
  `formatText(escapePattern(expandPattern($strings.$menu_loadGame_pattern, …)), [0x80FFFFFF])` (`main.script:506`).
* `getFormatIdAt(x, y)` (`L:0x081cd130`): character index under (x,y), then returns the **ordinal of the `\x04` run**
  containing it (0, 1, …; not the `N` of `{N:`), or −1 outside any run.

### 9.4 Localisation

There is **no engine-side string lookup**. `.strings` files are scripts that define variables
(`$menu_ok_button = "Ok";`). They are loaded into `MAIN.$strings` (`init.script:58`), and UI scripts assign
`.text = $strings.$xxx` and `.setArg(i, value)`. Locale-specific separators are written literally into the patterns.
`$thousand_separator` and `$decimal_separator` exist in the strings files but the engine never reads them.

---

## 10. Per frame and rebuild triggers

### 10.1 Tick (`gsControl::move`, gsMoveable slot 0, `0x100aa2f0`, `L:0x081cc690`)

```
if hasCodes (+0x185) && effective-visible (flags & 2):        // clipped is NOT checked
    display = formatText(raw, args, 10); apply maxTextLength
    if text item: updateText(); updateTextPos()               // every frame, including both measurements
if blending (+0x1C8):
    v = dt·rate + visibility
    if (rate > 0 && v >= target) || (rate < 0 && v <= target):
        blending = false; v = target
        if target == 0: clearStatus(2)                         // AUTO-HIDE: .visible = false
    setVisibility(v)                                           // rebuild image + border, recolour text
gsMoveable::move(dt)
```

* `blend(target, duration)` (slot 0x170, `L:0x081c6030`): if not visible, `visibility = 0` first; then `show()`;
  `rate = (target − visibility) / duration`. The argument order is confirmed by `L:0x081c57f0` (target, duration).
* Edge cases: `rate == 0` (already at target) never terminates and keeps calling `setVisibility(v)` every frame.
  `duration == 0` gives ±inf (finishes next tick) or NaN (`0/0`, never finishes; `mulA` of NaN → `_ftol` = 0x80000000
  → alpha 0).
* Writing `visibility` cancels a running blend (`L:0x081c5d70`).
* `visibility` is **not inherited**: a parent fading out does not fade its children's items. Only the hide at
  target 0 propagates, through the visible flag.
* Hidden dynamic text is not reformatted. It shows stale text for one frame when shown again.
* `setArg` only stores; dynamic text picks it up at the next tick.

### 10.2 What rebuilds what

| Trigger | image 0xE8 | border 0xF0 | text pos 0xEC | text content | clip 0xF4 |
|---|---|---|---|---|---|
| rect change (setRect/Position/Size, left/top/width/height) | if exists | if exists | if exists | – | if exists (+children relayout, `onChangeRect`) |
| `setMargins` | – | – | yes | – | if exists |
| `background=` / `hotBackground=` | **creates** | – | – | – | – |
| `setImage` / `setShader` | **creates** (colours → white) | – | – | – | – |
| `setCropRect` / `setTiling` | if exists | – | – | – | – |
| `setBorder` | if exists | **creates** | – | – | – |
| `visibility=` / blend tick | if exists | if exists | – | colours | – |
| hover / active change | if hot ≠ bg | – | – | – | – |
| `textAlign=` | – | – | yes | – | – |
| `text=` | – | – | yes | **creates**, sets | (created) |
| `setFont` / `setTextShadow` | – | – | yes / – | **creates** | (created) |
| show/hide (`statusUpdated`) | `setVisible` | `setVisible` | – | `setVisible` | `setVisible` |
| `clipped` change | through the rect-change row | | | | |
| z change (§8) | relayer | relayer | – | – (text stays −2) | relayer |

`statusUpdated` (`0x100a66a0`, `L:0x081c7470`) sets `setVisible(vis)` on clip, text, image and border (no
geometry rebuild), and deactivates the control when it becomes invisible.

---

## 11. Subclasses

### gsDesktop (`new_desktop`, ctor `L:0x081cdbc0`; registrar `0x100ab3f0`)

Draws exactly like gsControl. Extras (Windows offsets inferred as Linux + 0x68, except those in native-api.md):
`mouseX/mouseY` (+0x20C/+0x210), `mouse` (+0x214), the cursor control, plus hover/pressed targets and the button mask.

* Input dispatch: hit test `controlAt` (`L:0x081c6e30`), searched back to front in child order. Visible+enabled
  children whose **client rect** contains the point qualify (the test subtracts the child's margins), `clickThrough`
  skips the subtree, and `clickThroughSelf` lets only children match.
* The cursor is an ordinary control: `gsDesktop::mouseMove` calls `mouse.setPosition(x, y)` (`L:0x081c6330`). It
  uses the position stored *before* applying the current motion delta, and so does the hover hit test; the delta is
  stored at the end. *Observed*; the effect is a one-event lag of cursor and hover.

### gsViewPort as a control (`new_viewport(parent)`, `L:0x081dbf90`)

A gsControl whose client area is the native viewport (§7), so GUI children of a viewport are drawn in its GUI pass.
Its layout is the gsControl layout; the viewport rect follows `updateClientArea`. It can also carry an image: the
menu animation does `setShader(anim.shader)`, `setImage(textureTarget.getTexture())`, `setTiling(1,-1)` on the
viewport control (`animation.script:119-124`), so the image quad (in the parent's client area, layer `z·256`) shows
the render target flipped. 3D details: 2D doc §2.2.

### gsTextSelCtrl (`new_textControl`) and gsEditCtrl (`new_editControl`, used by menu dialogs `ui.script:644`)

Layout (Windows from ctor `0x100aff70` and `updateCaret` `0x100afcd0`; Linux − 0x80 for the edit fields):

| Win off | Field | Default |
|---|---|---|
| +0x204 / +0x208 | `selStart` / `selLength` | 0 / 0 |
| +0x20C | wstring: selected substring | "" |
| +0x228 | gsIGuiText*: selection text item | 0 |
| +0x22C | selection anchor (*inferred*) | −1 |
| +0x230 | `blinkSpeed` (edit) | 2.0 |
| +0x234 | caret blink phase | 0 |
| +0x238 | gsIGuiPrimitive*: caret | 0 |
| +0x23C | `caretColor` | white |
| +0x240 / +0x244 / +0x248 | `maxLength` / `rejectInput` / `hideChar` | −1 / 0 / 0 |

* No `%` formatting (+0x184 = 0). `setText` clamps `selStart` to the length (`L:0x081d4a30`).
* `gsEditCtrl::updateText` (`L:0x081d4d10`): if `hideChar ≠ 0`, every char of the display string is replaced by it
  (password fields) before it reaches the text item.
* **Selection highlight** (`setFont` override `L:0x081d4880`, `updateSelText` `L:0x081d5a90`): a second gsGuiText in
  the own client area (flags 3, layer 0, created lazily) with colour = **inverted** text colour, background colour =
  text colour, the same font/size/spacing, text = `display.substr(selStart, selLength)`, box = (cW, 10000) if
  left-aligned else (10000, 10000), position = the main text position moved down one line per `'\n'`/wrap before
  `selStart`, and `xOffset` = the pen x of `selStart` within its line (computed with `charAdvance`, tabs as
  `3·(size.x+spacing.x)`, the same word-wrap look-ahead). Rebuilt on setText, layout (if selLength > 0) and selection
  changes.
* **Caret** (`updateCaret` `L:0x081d4d80`/`0x100afcd0`): untextured quad, own client area, layer −1, visible = the
  visible flag (clipped is ignored). `x0 = textPos.x + Σ charAdvance(display[0..selStart))` (no line handling),
  `x1 = x0 + 0.12·charSize.x`, `y = textPos.y … textPos.y + charSize.y`. Colour =
  `caretColor % |trunc(phase·255)|` (alpha replaced, **not** multiplied by visibility; Linux rounds). Tick while
  active (`L:0x081d4770`): `phase += blinkSpeed·dt; while phase > 1: phase −= 2` (a triangle wave in alpha).
  Deactivation resets `phase = 0` and `selLength = 0`.
* Layer order inside an edit control's clip area: text (−2), caret (−1), selection (0), children.

### Out of scope

`gsGraphCtrl`, `gsMapCtrl`/`gsMapRouteCtrl`, `gsTracerCtrl`, `gsGaugeCtrl`, `gsPinCtrl`, `gsPointerCtrl`: they
override layout and text (for example gsGraphCtrl's `setArg`/`updateTexts`) and draw their own primitives. None of
them is instantiated by `metro/ui/menus/*` or `metro/ui/ui.script` (`new_tracerControl` appears only in in-game
`scripts/ui.script` and `scripts/editor.script`).

---

## 12. Cross-build differences (Windows is the reference)

| Spot | Windows | Linux |
|---|---|---|
| centred/middle snap in `updateTextPos` | `_ftol2` truncation | x87 round-to-nearest-even |
| caret alpha `phase·255` | truncation | rounding |
| `%f` | broken | broken the same way |
| vtable layout | as cited | GCC dual dtors and a different overload order (`setVisibility` Linux 0x174 vs Win 0x16C) |

---

## 13. Open questions

1. What the shipped CRT does with the broken `%f` format (crash via the invalid-parameter handler, or −1/empty). It is
   unused by shipped content.
2. DDS textures' wrap mode (REPEAT assumed for negative/scrolling crop rects). Same as 2D doc open question 2.
3. The BMFont glyph path (`xo/yo/adv` from BMFont records) was not traced. Only the native `.font` metrics are given.
4. The font page size field used for the 1-texel pad (+0x20) is width or height by the 2D doc's texture layout. Fonts
   with non-square pages would differ.
5. The visual effect of the latent default shadow (§6.1) has not been checked against a screenshot.
6. gsDesktop's Windows offsets beyond `mouse`/`mouseX`/`mouseY` are inferred (+0x68 from Linux).

---

## 14. Port checklist

1. **Data**: the §2.2 fields with the ctor defaults (hidden by default, `canActivate = true`, `textAlign = 9`,
   `visibility = 1`, crop (0,0,1,1), colours bg/hot `0x00FFFFFF`, border `0`, text `0xFFFFFFFF`, shadow `0x80000000`,
   char size 0.015).
2. **Lazy items** with exactly these owners and layers: image `z·256` and border `z·256+1` in the **owner's** client
   area; clip `z·256+0x80` in the owner's client area; text `−2` (caret −1, selection 0) in the **own** clip area.
   Stable ascending layer sort.
3. **z** = index in the owner's full child vector; recompute on add/remove/bringToFront/sendToBack/activation.
4. **Layout** (§3) with the change check, `clipped`, and `setMargins` not relaying out children.
5. **Image quad** (§4): border-inset rect, crop uvs (negative allowed, REPEAT), hot ⇔ (hover-chain ∨ active),
   `mulA` truncation by own `visibility`, white reset on `setImage`, untextured when there is no texture.
6. **Border** (§5): 16 vertices, mitred trapezoids, disabled sides with alpha 0, untextured.
7. **Text**: `updateTextPos` (§6.2) including setBox-before-measure, the 4.0 snap with truncation, no wrap for
   centre/right; measurement and drawing (§6.3-6.4) including tab stops, word-wrap look-ahead, codes 4/5,
   shadow-first, 1-texel glyph pad, line advance `size.y·(1+spy/size.x)`.
8. **Colour state flow** (§6.1): only `setFont`/`setTextShadow`/`setVisibility` push colours to the text item; keep
   the latent default shadow.
9. **Formatter** (§9): setText scan, `%%` collapse for static text, per-tick formatting of dynamic text with 10 args,
   `maxTextLength`, the `%D`/`%!DD`/`%i[t p d]`/`%f` grammar, early stop at end of string; script `formatText`,
   `expandPattern`, `escapePattern`, `getFormatIdAt`.
10. **Tick** (§10.1): blend with auto-hide at 0, `visibility` write cancels the blend, no inheritance of visibility.
11. **Edit controls**: hideChar masking, selection item (inverted colours), blinking caret (triangle wave).
12. Test scenes: the main menu (`metro/ui/menus/mainmenu.script`) exercises background colours, images with crop and
    tiling, left/right borders, centred and left/middle text with margins, hover image swaps (script-driven) and
    nested viewports. The credits menu exercises `visibility` and `setTextShadow`. The load-game list exercises
    `expandPattern`, `%ip`, inline colours and `getFormatIdAt`.
