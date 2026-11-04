# GSystem fonts: file formats, glyph lookup, drawing and text metrics

Scope: everything a reimplementation needs to load the shipped fonts and to lay out and measure text with exactly
the original metrics: the font cache (`gsFonts_OGL`), the two font file formats, the in-memory glyph table
(`gsFont_OGL`), the draw routine (`gsFont_OGL::renderText`) and the three measuring functions of `gsGuiText_OGL`
(`textWidth`, `textHeight`, `charWidth`) that back the script methods `getTextWidth` and `getTextHeight`.
How `gsControl` places text using those metrics is summarised in §9.

Tool: [`tools/fontparse.mjs`](../../tools/fontparse.mjs) implements every algorithm below (loader, lookup, the
three measuring functions and the draw-time line breaking) and was run on all 16 shipped font files (§10).

**Sources and conventions**

* Primary evidence is the **Linux build** pseudocode (`decompiled/Linux/functions/0x<addr>.c`, full C++ symbol
  names). Every behaviour was cross-checked against the **Windows** `GSystem.dll` pseudocode
  (`decompiled/GSystem-v2/functions/`), and where Ghidra hides x87 operands, against the raw `GSystem.dll` bytes
  (file offset = VA − 0x10000000). Where the two builds differ, the Windows build is treated as the reference.
* *Observed* = read directly in pseudocode or bytes. *Inferred* = follows from layout, data or usage.
  Confidence: H(igh), M(edium), L(ow).
* Function table (Linux ↔ Windows):

| Function | Linux | Windows |
|---|---|---|
| `gsFonts_OGL::getFont(name, extra, style)` | 0x081f98b0 | 0x10040980 |
| `gsFonts_OGL::loadFont(name, style)` | 0x081f8b80 | 0x100406b0 |
| `gsFonts_OGL::loadFontData(font, path)` (format dispatch) | 0x081f8880 / 0x081f95b0 | 0x1003fb40 |
| `gsFonts_OGL::readFontChunk` (native) | 0x081f5400 | 0x1003f260 |
| `gsFonts_OGL::readBMFdata` (BMFont v3) | 0x081f7b00 / 0x081f6da0 | 0x1003f610 |
| `gsFont_OGL::getCharData(c)` | 0x081f6410 | 0x1003d250 (+ map find 0x1003d080) |
| `gsFont_OGL::renderChar(glyph)` | 0x081f6450 | 0x1003d290 |
| `gsFont_OGL::renderText(...)` | 0x081f65b0 | 0x1003d410 |
| `gsGuiText_OGL::render` | 0x081ff100 | 0x10043df0 |
| `gsGuiText_OGL::textWidth` (= measureWidth, slot 14) | 0x081ff4d0 | 0x10044230 |
| `gsGuiText_OGL::textHeight` (= measureHeight, slot 15) | 0x081ff2b0 | 0x10043b60 |
| `gsGuiText_OGL::charWidth` (= charAdvance, slot 16) | 0x081ff930 | 0x10043d80 |
| `gsColor::gsColor(const wchar_t*)` (inline colour parser) | 0x080ee070 | 0x10074d80 |
| `gsControl::updateTextPos` | 0x081c6ae0 | 0x100a85e0 |

The Linux symbol names of the `gsIGuiText` slots are: `setZValue` (setLayer), `setAreaSize` (setBox), `setScale`
(setCharSize), `setCharOffset` (setSpacing), `setBackground`, `setShadow`, `setIndent` (setXOffset), `textWidth`,
`textHeight`, `charWidth`. This document uses the names of [interfaces-2d-output.md §2.5](../renderer/interfaces-2d-output.md).

---

## 0. Summary for implementers

* A font is a map **UTF-16 code unit → glyph** plus a list of page textures and one **font line height** `L`
  (normalised texture units). All glyph geometry is divided by `L`, so **glyph space is 1 unit = 1 line**.
* Native `.font`: 256 cells indexed by byte value (Windows-1252 layout); glyph i keeps key `i`; 7 typographic cells
  are also reachable by their Unicode code point. `L` = height of cell 0. Advance = cell width. No offsets.
* BMFont `.fnt` files ship only as *extras* merged into a native font (`ja`, `pl`, `ru`, `languages`). The engine
  adds `floor(lineHeight/20)+1` texels to every BMFont advance and centres the BMFont line inside the native line.
* Screen advance of a character: `adv/L · size.x + spacing.x` (screen units; pixels for the CiM GUI).
* `measureWidth` = widest `\n`-separated line, sum of advances **including the trailing `spacing.x`** of the last
  character, no word wrap, tab counted as glyph 9. `measureHeight` = `size.y + (lines−1)·(size.y + spacing.y)` after
  word wrap at `box.x`.
* Missing characters use the glyph with the **smallest key** (cell 0, an empty box in the native fonts).
* No kerning, no baseline handling (baked into cells), inline colour codes U+0004 + 8 hex digits and U+0005.

---

## 1. Shipped font files

All in archives: `data.gs` (verdana) and `metro.gs` (everything else). No addon or DLC archive ships fonts.
Numbers from `node tools/fontparse.mjs check "<game dir>"` (0 issues).

| Font name (script path) | File | Format | Keys | Pages | `L` (normalised) = px | Used by |
|---|---|---|---|---|---|---|
| `fonts/verdana` | `normal.font` | native f32 | 263 | 1 × 512² RGBA32 (10 mips) | 0.07421875 = 38 px | gsGuiText default; console, debug, fps, capture scripts (size `0.015·desktop.height` etc.) |
| `metro/fonts/blair-18` | `normal.font` | native f32 | 263 | 1 × 512² RGBA32 | 0.06640625 = 34 px | `setSmallHeaderFont` (20, −2), `setLargeHeaderFont` (30, −3), credits (25/30) |
| | `ja.fnt` | BMFont 3 | 3272 | 18 × 512² DXT5 | own lineHeight 39 | Japanese extra |
| | `pl.fnt` | BMFont 3 | 323 | 1 × 512² RGBA32 | 39 | Polish extra (not selectable in this install) |
| | `ru.fnt` | BMFont 3 | 227 | 1 × 512² RGBA32 | 39 | Russian extra (idem) |
| `metro/fonts/default-9` | `normal.font` | native f32 | 263 | 1 × 256² RGBA32 | 0.05859375 = 15 px | `setTitleFont` (15, −2, 0) |
| | `ja.fnt` / `pl.fnt` / `ru.fnt` / `languages.fnt` | BMFont 3 | 3272 / 323 / 227 / 15 | 14 / 1 / 1 / 1 × 256² RGBA32 | 18 / 17 / 18 / 18 | extras |
| `metro/fonts/tahoma-8` | `normal.font` | native f32 | 263 | 1 × 256² RGBA32 | 0.05859375 = 15 px | `setTextFont` (15, −2, 3) — most UI text |
| | `ja.fnt` / `pl.fnt` / `ru.fnt` / `languages.fnt` | BMFont 3 | 3272 / 323 / 227 / 16 | 14 / 1 / 1 / 1 × 256² RGBA32 | 17 / 17 / 17 / 17 | extras |
| `metro/ui/minimap/font` | `normal.font` | native **fixed** (0x03020000) | 263 | 1 × 32² RGBA32 | 0.15625 = 5 px | minimap labels (5, 5); only the digits have non-zero cells |

* Every `.fnt` is "Arial Unicode MS" (info size −40 for the blair-18 set, 18 or 17 for default-9, 17 for
  tahoma-8), padding 1,1,1,1, spacing 1,1, no kerning block (blocks 1-4 only, each file ends exactly after
  block 4). **(observed, H)**
* `hqpanel.script` asks for `fonts/pala`, which does not exist anywhere (§2.4). **(observed)**
* `pl`/`ru` are "not selectable" because the language menu lists `metro/strings/*.strings` and this install has
  only `de`, `en_US`, `es`, `fr` and `ja` (inferred from the archive listing).
* [skeleton-animation-scene.md §2.13](../asset-format/skeleton-animation-scene.md) used to say no shipped font
  uses BMFont; 11 `.fnt` extras do (corrected there).

---

## 2. Font cache, file lookup and extras

### 2.1 `getFont(name, extra, style)` (observed, H)

`gsFonts_OGL` keeps `std::map<wstring name, gsFont_OGL*>` (Linux +0x94, Windows +0xCC). The key is the font *name*
exactly as passed (no normalisation, no base-path resolution; `setFont` passes the script string unchanged).

```
font = cache[name]
if missing: font = loadFont(name, style); cache[name] = font        // also cached when nothing could be loaded
if extra != "" and extra not in font.extras:
    if   exists(name + "/" + extra + ".fnt"):  loadFontData(font, that); font.extras.insert(extra)
    elif exists(name + "/" + extra + ".font"): loadFontData(font, that); font.extras.insert(extra)
    // neither exists: nothing happens and nothing is remembered (the existence test repeats on every call)
return font
```

`loadFont(name, style)` allocates an empty font (`L = −1`, no pages, no glyphs), then loads
`name + "/" + styleName[style] + ".fnt"` if that file exists, otherwise `… + ".font"` (loaded even if it does
not exist; the load then silently does nothing). `styleName` is a one-element table `{ L"normal" }`
(`DAT_1018fa2c`, initialised in 0x10146ec0), and every caller passes style 0, so the main file is always
`<name>/normal.fnt` or `<name>/normal.font`. The separator is `/` (`DAT_1015a41c`).

### 2.2 Extras are merged into the shared font (observed, H; consequence inferred)

`gsGuiText_OGL::setFont(name, extra)` calls `getFont(name, extra, 0)`. The UI helpers pass `MAIN.$language` as
`extra` (`metro/ui/ui.script:920-940`), i.e. `"en_US"`, `"de"`, `"fr"`, `"es"`, `"ja"`, or `"languages"` while
the language menu is open (`metro/scripts/main.script:580`). Consequences:

* `en_US`, `de`, `fr`, `es` have no file → nothing merged.
* `ja` merges `ja.fnt`; `languages` merges `languages.fnt` (the language names in their own scripts).
* Merged glyphs **overwrite** existing keys and stay in the cached font for the rest of the session, for every
  text object using that font name. `pl.fnt` overrides all 95 printable ASCII keys (so Polish text, Latin
  letters included, gets Arial metrics); `ja.fnt` overrides only U+2019, U+201C, U+201D, U+2026; `ru.fnt` and
  `languages.fnt` override nothing (measured with `fontparse`, see §10.6).

### 2.3 Format dispatch `loadFontData(font, path)` (observed, H)

The file is opened through the VFS. If its first u32 is `0x03464D42` (`"BMF\x03"`) it is read as BMFont binary v3
with page directory = `gsExtractFilePath(path)` (everything up to and including the last `/` or `\`). Otherwise a
chunk header is read; only if the root tag is `0x03000000` does the engine push the page texture
`gsChangeFileExt(path, ".dds")` onto `font.pages` (through the texture cache, so a missing DDS becomes the default
texture) and call `readFontChunk`. Any other content is ignored without error.

### 2.4 Missing font (observed; effect inferred, L)

A name with neither file (shipped case: `fonts/pala`) still produces a cached font with **no glyphs and no
pages**. `getCharData` then returns the map's header node, whose "glyph" is not a glyph (Windows: the
uninitialised value storage of the MSVC head node; Linux: the font's own trailing fields, so `adv` = `L` = −1,
plus bytes past the end of the 0x44-byte font object for `lh` and `page`). Measured widths and drawing for such
a font are undefined in the original. A port should treat it as an empty font (all advances 0, nothing drawn) or
fall back to `fonts/verdana`, and log it.

---

## 3. Native format `.font` (chunk family 0x03)

Little-endian GSystem chunk file: `u32 tag, u32 size, u8 body[size]`, children directly inside the root body
(no header). File size of every shipped native font: 4112 bytes = 8 + 8 + 4096.

| Offset | Field | Notes |
|---|---|---|
| 0 | `u32 0x03000000` | root tag (required) |
| 4 | `u32 size` | 4104 in all shipped files |
| 8 | `u32 tag` | `0x03010000` (float table) or `0x03020000` (fixed-point table); other tags are skipped |
| 12 | `u32 size` | 4096 |
| 16 | `glyph[256]` | 16 bytes each, indexed by byte value 0..255 |

Glyph entry (16 B): `u, v, w, h` — the cell rectangle in **normalised texture coordinates** of the page, top-left
origin (`u,v` = top-left corner, `w,h` = size). `0x03010000` stores `f32`; `0x03020000` stores `i32` in 22.10
fixed point (value × 1/1024). Only the minimap font uses the fixed-point form.

Decoding (`readFontChunk`, observed H):

```
if font.L < 0: font.L = glyph[0].h               // only the first file of a font sets L
page = font.pages.size - 1                          // the .dds pushed just before
for i in 0..255:
    g = { u, v, w, h, xoff: 0, yoff: 0, adv: w, lh: font.L, page }
    font.glyphs[i] = g                              // key = byte value, NOT the cp1252 → Unicode mapping
    if i in ALIAS: font.glyphs[ALIAS[i]] = g        // second key for the same cell
ALIAS = { 0x85:U+2026 …, 0x8C:U+0152 Œ, 0x92:U+2019 ’, 0x93:U+201C “, 0x94:U+201D ”, 0x96:U+2013 –, 0x9C:U+0153 œ }
```

So a native font has 256 + 7 = **263 keys**. The cells for the other cp1252 characters in 0x80–0x9F (€ ‚ ƒ „ † ‡
ˆ ‰ Š ‹ Ž ‘ • — ˜ ™ š › ž Ÿ) exist in the texture but are reachable only through the code units U+0080–U+009F
(see §5.3). The 7 aliases match exactly the typographic characters the German/French/Spanish string tables use,
except U+2018 ‘ (3× in `de.strings`, 2× in `es.strings`), which therefore renders as the missing-glyph box.

Shipped native fonts are **fixed-height cell fonts**: every cell is exactly `L` high, cells include their side
bearings, and there are no offsets. In the four text fonts, codes 0x00–0x1F and 0x7F share one cell (an empty box)
that is also cell 0, the missing-glyph fallback. The minimap font defines only `0`–`9`; every other cell has
`w = 0` (advance 0; `h = 0` too, except cell 0 whose `h` = 5/32 defines `L`) and still draws a degenerate quad,
which the one-texel pad (§7.1) turns into a 2 × 2-texel quad at the page's top-left corner.

**Worked example** (`metro/fonts/tahoma-8/normal.font`, page 256 × 256): entry `'A'` (0x41) at file offset
16 + 0x41·16 = 0x420 is `00 00 2b 3f | 00 00 a8 3d | 00 00 20 3d | 00 00 70 3d` → u = 0.66796875 (171 px),
v = 0.08203125 (21 px), w = 0.0390625 (10 px), h = 0.05859375 (15 px). Cell 0 has h = 0.05859375, so
`L` = 15/256 and `'A'` advances `w/L` = 10/15 = 0.6667 line units.

---

## 4. BMFont binary v3 `.fnt` (`readBMFdata`, observed H)

Standard AngelCode layout: magic `BMF\x03`, then blocks `u8 type, i32 size, body`. The engine reads:

| Block | Engine behaviour |
|---|---|
| 1 info | skipped (face, size, padding, … unused) |
| 2 common | reads `u16 lineHeight`, skips `base`, reads `u16 scaleW`, `u16 scaleH`, `u16 pages`, skips `size − 10` bytes |
| 3 pages | reads exactly `pages` NUL-terminated names; page *k* = `<font dir>/<name>` → texture cache → `font.pages` |
| 4 chars | stops the block loop; `count = size / 20` records |
| 5 kerning | **never read** (reading ends at block 4). No shipped file has one. |
| other | skipped |

On the common block, with `sx = 1/scaleW`, `sy = 1/scaleH` (stored as `f32`):

```
if font.L >= 0:  yAdj = sy · trunc((font.L / sy − lineHeight) · 0.5)   // centre this file's line in the existing line
else:            font.L = lineHeight · sy;  yAdj = 0                   // a .fnt loaded as the main file
advPad = (floor(lineHeight / 20) + 1) · sx                             // extra advance, in texels of this file
```

`trunc` is toward zero in **both** builds: Windows calls `_ftol2` on `(L/sy − lineHeight) · 0.5` (bytes at
0x1003f75e-0x1003f77a: `fld [ecx+0x2c]; fdiv sy; fild lineHeight; fsub; fmul 0.5`; constant 0x101593c0 = 0.5),
and the Linux `(int)ROUND(…)` is really an `fldcw`/`fistp`/`fldcw` truncation sequence (0x081f7e05-0x081f7e13).

Each 20-byte char record is read as `u32 id; s16 x, y, width, height, xoffset, yoffset, xadvance; u8 page; u8 chnl`
(`chnl` skipped, all fields signed):

```
g = { u: x·sx, v: y·sy, w: width·sx, h: height·sy,
      xoff: xoffset·sx, yoff: yoffset·sy + yAdj,
      adv: xadvance·sx + advPad, lh: font.L, page: page + pagesBefore }   // pagesBefore = font.pages.size at entry
font.glyphs[id] = g;  if id in ALIAS: font.glyphs[ALIAS[id]] = g          // same 7 aliases as §3
```

* Windows keys the map with a 16-bit `wchar_t`: `id` is truncated to its low 16 bits. Shipped files have at most
  id 0xFFE3, plus exactly one record with id −1 (BMFont's "invalid char" glyph) in **every** shipped `.fnt`,
  which becomes key U+FFFF on Windows. **On Linux the key stays −1, becomes the smallest key, and therefore
  replaces cell 0 as the missing-glyph fallback once such an extra is merged** (build difference; follow Windows).
* `base`, the info block and the channel/packing fields are ignored: vertical placement comes only from `yoffset`.

Shipped extras (all pages square, so `sx = sy`):

| Base font (`L` px) | Extra (lineHeight) | `yAdj` px | `advPad` px |
|---|---|---|---|
| blair-18 (34) | ja, pl, ru (39) | trunc(−2.5) = −2 | 2 |
| default-9 (15) | ja, ru, languages (18) / pl (17) | trunc(−1.5) = −1 / −1 | 1 |
| tahoma-8 (15) | ja, pl, ru, languages (17) | −1 | 1 |

Example: `metro/fonts/default-9/pl.fnt` record `'P'`: x 122, y 146, w 12, h 13, xoffset −1, yoffset 3,
xadvance 10, page 0 → merged into default-9: `yoff` = 3 − 1 = 2 px, `adv` = 10 + 1 = 11 px (/256), page 1.
At size 15 its advance is 11/15·15 = 11 px (vs. 10 px for the native `'P'`).

---

## 5. Glyph record and lookup

### 5.1 In-memory glyph `gsCharData_OGL` (36 bytes, observed H)

| +0 | +4 | +8 | +0xC | +0x10 | +0x14 | +0x18 | +0x1C | +0x20 |
|---|---|---|---|---|---|---|---|---|
| u | v | w | h | xoff | yoff | adv | lh (= font `L`) | page index (int) |

All `f32`, normalised texture units. The font object holds `style`, `vector<gsTexture_OGL*> pages`,
`map<wchar_t, gsCharData_OGL> glyphs`, `set<wstring> extras` and `float L` (Linux +0x40, Windows +0x2C,
initialised to −1.0).

### 5.2 `getCharData(c)` and the missing-glyph rule (observed H)

Exact match on the key; otherwise the **first map node, i.e. the glyph with the smallest key** (`begin()`; Windows
`head->_Left`). For native fonts this is cell 0 (the box shared with 0x01–0x1F and 0x7F). The same lookup is
inlined in `renderText`, `textWidth`, `textHeight` and `charWidth`; none of them has another fallback.

### 5.3 Which code units reach which cell (observed H for the engine; consequence for the port)

Strings are UTF-16 code units (Windows `wchar_t`). Script and `.strings` files are decoded by
`gsParser::parseSource` (Linux 0x08259e60, Windows 0x10016440): `FF FE` → UTF-16LE, `EF BB BF` → UTF-8, **anything
else → one byte per code unit, zero-extended (Latin-1), not Windows-1252.** So a byte 0x92 in an 8-bit file
(`en_US.strings` has 241) becomes U+0092 and hits the native cell 0x92 directly; the same character in a UTF-16 file
is U+2019 and hits it through the alias. Both give the same glyph. A port that decodes 8-bit files as Windows-1252
(as `port/src/core/text.h` currently does) maps 0x80/0x91/0x97… to U+20AC/U+2018/U+2014…, which are *not*
aliased and fall back to the box: different glyphs and widths from the original (e.g. `‘` is 3 px in
tahoma-8 at size 15 via its cell, 11 px via the box). To match, decode BOM-less 8-bit files as Latin-1 (byte →
same code unit). Adding the 20 missing cp1252 aliases to the font instead would be wrong in the other direction:
U+2018 from the UTF-16 string tables would then find a cell, where the original shows the box.

---

## 6. Units, parameters and the coordinate system

**Glyph space** (inside `renderText`): x and y in *line units*; one line is 1.0 high. A glyph occupies
`[xoff/L, (xoff+w)/L] × [yoff/L, (yoff+h)/L]` relative to the pen, and the pen advances by `adv/L + spX`
(`spX` = `spacing.x/size.x`, see below).
y grows downward (GUI projection, top-left origin). There is no baseline: native cells span the whole line
(y 0..1); BMFont glyphs are placed by `yoff`. Horizontal and vertical texture units are mixed (`w` is in 1/pageWidth,
`L` in 1/pageHeight); all shipped pages are square so glyphs keep their pixel aspect.

**gsGuiText parameters** (`gsIGuiText`, Windows offsets; Linux = −4):

| Field | Offset | Default | Meaning |
|---|---|---|---|
| position | +0x44 | 0,0,0 | screen position of the top-left of the first line's cell |
| box | +0x50 | 10000, 10000 | `box.x` = wrap width (screen units); `box.y` is never read |
| size | +0x5C | 0.015, 0.015, 1 | `size.y` = line height on screen; `size.x` = horizontal scale of one line unit |
| spacing | +0x68 | 0, 0, 0 | `spacing.x` added to every advance, `spacing.y` added between lines (screen units) |
| shadow offset | +0x74 | 0.001, 0.001 | |
| colour / background / shadow colour | +0x80 / +0x84 / +0x88 | white / 0 / 0 | ARGB |
| indent | +0x8C | 0 | extra pen x of the **first line only** (screen units) |

`gsGuiText_OGL::render` (observed H, both builds) does `glTranslatef(position)`, `glScalef(size.x, size.y,
size.z)` and calls `renderText` with every length divided by **`size.x`**:

```
renderText(text, colour, bgColour, x0 = indent/size.x, inlineColours, wrapW = box.x/size.x,
           spX = spacing.x/size.x, spY = spacing.y/size.x)
```

So on screen: advance = `(adv/L + spacing.x/size.x)·size.x` = `adv/L·size.x + spacing.x`; wrap at `box.x`;
**line advance = `size.y · (1 + spacing.y/size.x)`**. The measuring functions use `size.y + spacing.y` instead;
the two agree only when `size.x = size.y`, which holds for every shipped `setFont` call (quirk, observed H).

In the CiM GUI the root viewport uses pixel coordinates, so all of these are pixels; scripts pass sizes such as
`15, 15` (tahoma-8 cell height 15 px → 1:1) or `0.015 * $desktop.height`.

---

## 7. Drawing: `gsFont_OGL::renderText` (observed H)

Called twice by `gsGuiText_OGL::render`: first, if the shadow colour's alpha ≠ 0, at `position + shadowOffset`
with `colour = shadowColour` and inline colours **off**; then at `position` with `colour` and inline colours
**on**. Both passes use the same background colour. It returns the number of quads drawn (glyphs plus background
quads), added to the viewport's polygon statistics.

```
glColor(colour); glTranslatef(x0, 0, 0)
pen = x0; tabAcc = 0; boundPage = −1; i = 0
while text[i] != 0:
  c = text[i]
  switch c:
  0x04 (inline colour):  if inlineColours:
                            k = parseColour(text[i+1 .. i+8])                  // §7.2
                            glColor( (k & 0x00FFFFFF) | ((k.a · colour.a) >> 8) << 24 )
                         i += 9
  0x05 (colour reset):   if inlineColours: glColor(colour)
                         i += 1
  0x09 (tab):            tabAcc = (spX + 1)·3 + tabAcc;  translate(tabAcc − pen); pen = tabAcc;  i += 1
  0x0A (newline):        translate(−pen, spY + 1); pen = 0; tabAcc = 0;  i += 1
  0x0D:                  i += 1
  0x20 (space):          g = getCharData(0x20); a = g.adv/g.lh + spX
                         look = pen + a; save = look; j = i + 1
                         while text[j] != 0:                                   // look ahead over the next word
                            d = text[j]
                            if d == 4: j += 8
                            elif d != 5:
                               h = getCharData(d)
                               if d <= 0x20 or h.lh == 0 or look > wrapW: break
                               look = h.adv/h.lh + look + spX
                            j += 1
                         if look <= wrapW: translate(a); pen = save
                         else:             translate(−pen, spY + 1); pen = 0; tabAcc = 0   // the space is dropped
                         i += 1
  default (glyph):       g = getCharData(c)
                         if g.lh <= 0: i += 1; continue                        // not drawn, no advance
                         bind page g.page if it differs from the last bound page
                         a = g.adv/g.lh + spX
                         if pen + a > wrapW: translate(−pen, spY + 1); pen = 0; tabAcc = 0   // per-glyph wrap
                         if bgColour.a != 0: untextured quad (0,0)-(a,1) in bgColour      // selection highlight
                         renderChar(g); translate(a); pen += a
                         i += 1
```

Rules worth stating separately:

* **Tab**: the *k*-th tab of a line moves the pen to the absolute position `k · 3 · (1 + spX)` line units
  (`k · 3 · (size.x + spacing.x)` on screen). It ignores the text before it — after a long prefix the pen moves
  **backwards** and later glyphs overprint earlier ones (example §10.4). Not "next multiple". The stop counter
  resets at a newline or a wrap, and the first-line indent is not included.
* **Word wrap** has two triggers: at a space, when `space + next word` would end beyond `wrapW` (the word ends at
  the next code unit ≤ 0x20, at a glyph with `lh == 0`, or at the end; colour codes inside it are skipped); and at
  any glyph that would itself end beyond `wrapW` (so over-long words break between letters). The wrap test uses
  `>` (a line may end exactly at `wrapW`). After a wrap the new line starts at x = 0 and the space is not drawn.
* **Spaces are never drawn** (no quad, no background quad); every other code unit with `lh > 0` is drawn,
  including control codes 0x01–0x08, 0x0B, 0x0C, 0x0E–0x1F, which show the box cell.
* **Indent** applies to the first line only; after `\n` or a wrap the pen restarts at 0.
* Code 4 is consumed as 9 code units in every pass even when inline colours are off; nothing checks for a NUL
  inside the 8 digits (a truncated code reads past the string end).

### 7.1 Glyph quad `renderChar` (observed H)

Immediate-mode `GL_QUADS`, vertices in glyph space relative to the pen, with a pad of **one full texel** (not half):

```
T = page[g.page].height (the DDS header height, int)      // gsTexture_OGL +0x20; used for u AND v
p = 1/T;  q = 1/(T·g.lh)
(u − p, v − p)        → (xoff/lh − q,       yoff/lh − q)
(u − p, v + h + p)    → (xoff/lh − q,       (yoff+h)/lh + q)
(u + w + p, v + h + p)→ ((xoff+w)/lh + q,   (yoff+h)/lh + q)
(u + w + p, v − p)    → ((xoff+w)/lh + q,   yoff/lh − q)
```

The pad therefore samples one texel of the surrounding atlas area on each side. In the four native text fonts the
cells are 4 px apart and at least 2 px from the page edge (measured), so the padded quads never reach a neighbouring
cell; whether the gap texels are fully transparent was not checked. The colour comes from `glColor` (texture
modulated, *inferred*).

### 7.2 Inline colour payload (observed H; build difference)

Produced by script `formatText(text, colors)`: `{d:` → U+0004 + 8 uppercase hex digits `AARRGGBB` of `colors[d]`,
`}` → U+0005 ([batch-C.md](../handlers/batch-C.md#globalformattext)). Parsing: if any of the 8 code units is NUL,
the colour is left unparsed (Linux: uninitialised stack value; Windows: also uninitialised). `hexCharToInt`:
`0-9`, `A-F`, `a-f` → 0..15, anything else → 0.

* **Linux** (`gsColor::gsColor(const wchar_t*)`): byte = `hex(c[2n])·16 + hex(c[2n+1])` — the intended parse.
* **Windows** (`0x10074d80`, verified in the bytes at 0x10074da1-0x10074dc0: both `hexCharToInt` calls receive the
  same register): byte = `hex(c[2n])·17`, i.e. **only the first digit of each pair is used**. `FF8AC744` is drawn
  as `FF88CC44`, `FFE80028` as `FFEE0022`. Colours with equal nibble pairs (`FF00FF00`, `80FFFFFF`) are unaffected.
* Alpha is multiplied as `(inline.a · base.a) >> 8`, so 0xFF·0xFF gives 0xFE.

The shadow pass skips colour codes (the shadow keeps its colour) and never changes the background colour.

---

## 8. Measurement

All three functions read the gsGuiText's current `size`, `spacing`, `box` and font; they do **not** consider
`indent`. Arithmetic: Windows x87 with 53-bit precision; every value the code keeps in a `float` variable is
rounded to `f32` at that point (verified in the disassembly of 0x10044230 and 0x10043b60). `fontparse.mjs`
reproduces this with `Math.fround`; comparisons are done on the unrounded sum, as in the bytes. In C++ the
equivalent is: evaluate each expression in `double` from the `float` inputs, assign the results to `float`
variables, and compare the `double` sum before assigning it.

### 8.1 `charWidth(c)` (slot 16, cursor/selection hit-testing)

`0` for c ∈ {4, 5, 10, 13}; otherwise `g.adv/g.lh · size.x + spacing.x` (no `lh > 0` test; tab is glyph 9).

### 8.2 `measureWidth(text)` (slot 14, script `getTextWidth`)

```
if text == null: return 0
max = 0; cur = 0
for each code unit c until NUL:
  4:      skip it and the 8 digits
  5, 13:  nothing
  10:     max = max(max, cur); cur = 0
  other:  g = getCharData(c); cur = f32(g.adv/g.lh · size.x + spacing.x + cur)    // includes 9 (tab), 32, controls
return max(max, cur)
```

It returns the **widest line**; it **includes the trailing `spacing.x`** after the last character of each line
(so with spacing −2 the result is 2 px less than the sum of the advances); it **does not wrap** at `box.x`; it
counts a **tab as glyph 9** (the box cell: 11 px in tahoma-8 at 15/−2) instead of the tab stop; it does not skip
glyphs with `lh ≤ 0`. The empty string gives 0.

### 8.3 `measureHeight(text)` (slot 15, script `getTextHeight`)

```
if text == null: return 0
h = size.y; x = 0; tab = 0                                   // "" → size.y (one line)
newline(): h = f32(spacing.y + size.y + h); x = 0; tab = 0
for each code unit c until NUL:
  4: skip 9 units      5, 13: nothing      10: newline()
  9:  tab = f32((spacing.x + size.x)·3 + tab); x = tab
  32: g = getCharData(32); w = f32(g.adv/g.lh·size.x + spacing.x); xs = f32(w + x); look = xs
      for following units d until NUL: (4: skip 8 more; 5: continue)
          gd = getCharData(d); if d <= 0x20 or gd.lh == 0 or look > box.x: break
          look = f32(gd.adv/gd.lh·size.x + spacing.x + look)
      if look <= box.x: x = xs  else newline()
  other: g = getCharData(c); if g.lh <= 0: continue
         w = f32(g.adv/g.lh·size.x + spacing.x)
         if w + x > box.x: newline()
         x = f32(w + x)
return h
```

Result: `size.y + (lines − 1)·(size.y + spacing.y)`. No trailing `spacing.y`. The line breaking is identical to
`renderText`'s (same rules, same order of tests), so `measureHeight` matches the drawn line count whenever
`indent = 0` and `size.x = size.y`.

### 8.4 Measure vs. draw differences (observed H)

| Aspect | `renderText` (draw) | `measureWidth` | `measureHeight` |
|---|---|---|---|
| Word wrap at `box.x` | yes | **no** | yes |
| Tab | tab stop `k·3·(size.x+spacing.x)` | **glyph 9 advance** | tab stop |
| Glyph with `lh ≤ 0` | skipped | **counted** (÷0 if `lh = 0`) | skipped |
| Indent | first line | ignored | ignored |
| Line advance | `size.y·(1 + spacing.y/size.x)` | — | `size.y + spacing.y` |

---

## 9. How gsControl uses the metrics (observed H unless noted)

* `setFont(font, colour, sizeX, sizeY, [spacingX], [spacingY], [extra])` stores size `(sizeX, sizeY, 1)` and
  spacing `(spacingX, spacingY, 0)` and forwards them unchanged ([batch-B.md](../handlers/batch-B.md#gscontrolsetfont)).
* `getTextWidth()` / `getTextHeight()` measure the control's **displayed** text (Linux +0x114, which `setText`
  derives from the raw text +0x118 by its `%` handling; M) with its gsGuiText's current settings; they return 0
  when the control has no text object.
* `updateTextPos` (0x100a85e0; `textAlign` bits from `docs/native-api.md`):

| Horizontal | text x | box (wrap) |
|---|---|---|
| default / `ALIGN_LEFT` | `desktopLeft + marginLeft` | `(clientWidth, clientHeight)` → wraps |
| `ALIGN_CENTER` 0x10 | `desktopLeft + marginLeft + c`, `c = (clientWidth − W)·0.5`; if `size.x ≥ 4`: `c = trunc(c)` | 10000 → no wrap |
| `ALIGN_RIGHT` 0x20 | `desktopLeft + width − marginRight − W` (not rounded) | 10000 → no wrap |

| Vertical | text y |
|---|---|
| default / `ALIGN_TOP` | `desktopTop + marginTop` |
| `ALIGN_MIDDLE` 2 | `desktopTop + marginTop + m`, `m = (clientHeight − H)·0.5`; if `size.y ≥ 4`: `m = trunc(m)` |
| `ALIGN_BOTTOM` 4 | `desktopTop + height − marginBottom − H` |

  `W` = `measureWidth`, `H` = `measureHeight` measured **after** the box is set (so left-aligned text is wrapped
  at `clientWidth`). `trunc` is toward zero (`_ftol2`; bytes at 0x100a8699-0x100a86a8 and 0x100a87c1-0x100a87d0
  show `call measure; fsubr client; fmul 0.5; call _ftol2`). `ALIGN_LEFT` (8) and `ALIGN_TOP` (1) set no tested bit.
* `visibleLines()` = `(int)floor(height / size.y)` — ignores `spacing.y` and the margins (Linux 0x081cdb80).

---

## 10. Worked examples (all produced by `tools/fontparse.mjs measure … --game "<game dir>"`)

### 10.1 Single line, tahoma-8 (`setTextFont`: size 15, spacing −2, 3)

Line height `L` = 15 px; advance = cell px · 15/15 − 2.

```
"Hello World": H 8 + e 7 + l 3 + l 3 + o 7 + ' ' 3 + W 11 + o 7 + r 5 + l 3 + d 7
measureWidth = 64      measureHeight = 15      10 quads drawn (the space is not drawn)
```

The last `−2` is included: the pen ends at 64 even though the `d` cell ends at 66.

### 10.2 Word wrap (box.x = 60)

```
"The quick brown fox", size 15, spacing (−2, 3), box 60
line 0  y 0   "The quick"   T@0 h@7 e@14 q@24 u@31 i@38 c@41 k@47   (ends at 54)
line 1  y 18  "brown fox"   b@0 r@7 o@12 w@19 n@28 f@38 o@42 x@49
measureHeight = 15 + 18 = 33;  measureWidth = 113 (unwrapped)
```

At the space after "quick" the pen is 54; 54 + 3 + "brown" (7+5+7+9+7 = 35) = 92 > 60, so the line breaks there.

### 10.3 Newline plus per-glyph wrap (box.x = 100)

```
"Line one\nLonger second line\tX" → 3 lines, measureHeight = 15 + 2·18 = 51, measureWidth = 123
line 2 is "line" then the tab: the first tab stop is 3·(15 − 2) = 39 px, so X is drawn at 39.
```

### 10.4 Tab stops ignore preceding text

```
"ab\tc\nabcdefghijklmnop\tX" (15, −2): line 0: c@39; line 1: p ends at 101, the tab moves the pen
back to 39 and X overprints 'f'.  measureWidth = 119 (counts each tab as the 11 px box glyph).
```

### 10.5 Inline colour codes are zero-width

```
"Cost: " U+0004 "FFFF0000" "-500" U+0005 " EUR"  → measureWidth = 82, same as without the codes.
```

### 10.6 Extras change metrics

```
default-9 "Polski" (15, −2):  34 without extra;  36 with extra "pl" ('P' 9 instead of 8, from pl.fnt + 1 px pad)
tahoma-8 + "ja": "日本語AB" (15, −2) = 12 + 12 + 12 + 8 + 7 = 51   (ja.fnt: xadvance 13 + 1 pad = 14 px of 15 → 14 − 2)
```

### 10.7 Fractional sizes

```
blair-18 "Cities in Motion" (30, −3): C = 30/34·30 − 3 = 23.470589 …  measureWidth = 257.29412841796875
fonts/verdana "Hello World" at 0.015·768 = 11.52: measureWidth = 56.08421325683594, measureHeight = f32(11.52)
minimap font "12 345" (5, 0): digits 4 px except '1' 3 px, space 0 → 19
```

### 10.8 Missing characters

```
tahoma-8 "€5 ‘5’" (15, −2): U+20AC and U+2018 are not keys → box glyph, 11 px each; U+2019 is aliased → 3 px.
measureWidth = 42.
```

---

## 11. Corrections made to earlier documents

* [interfaces-2d-output.md §2.5](../renderer/interfaces-2d-output.md): the glyph pad is **one texel** (1/page
  height), not half; the control codes are observed (4, 5, 9, 10, 13, 32), not inferred; tab is the *k*-th stop,
  not the next multiple; wrapping also happens per glyph; the missing-glyph fallback is the smallest key. The
  background quad is drawn in both passes, as already stated there. §4.3 "width = glyph w/h" now reads `w/L`;
  open question 6 no longer lists the control codes.
* [interfaces-2d-output.md §1.4](../renderer/interfaces-2d-output.md): the style suffix is always `normal`; the
  11 shipped `.fnt` extras are listed.
* [skeleton-animation-scene.md §2.13 and open question 5](../asset-format/skeleton-animation-scene.md): BMFont is
  used by shipped files; metrics are traced here.

---

## 12. Open questions

1. **x87 precision** (M): the measuring code assumes the CRT default 53-bit precision control. If something in the
   process sets 24-bit precision, sums are rounded slightly differently. Results differ in the last float bit at
   most; it matters only where the engine (centring with `size ≥ 4`) or a script (`int(...)`) truncates a measured
   value that lands within ~1e-5 px of an integer. With the shipped pixel sizes (15 for 15-px cells) the widths
   are exact integers either way.
2. **Missing font** `fonts/pala` (`hqpanel.script`): undefined behaviour in the original (§2.4). Whether that
   panel is ever visible was not checked.
3. **Unparsed colour code** (a U+0004 followed by fewer than 8 code units): the colour is an uninitialised local
   in both builds. Never produced by `formatText`; a port can keep the previous colour.
4. **Windows colour-digit bug** (§7.2): observed in the bytes, but not confirmed visually in the running game.
5. **Texture filtering and blending** of font pages (mipmaps on verdana, DXT5 on blair-18 `ja`) belong to the
   texture pipeline document; this document does not cover how the page's colour channels are used.
6. **Linux-only differences** (`ROUND` vs truncation are not differences — both truncate): negative BMFont ids
   become the fallback glyph on Linux (§4), and the Linux colour parser reads both digits (§7.2).
