# Minimap: `gsGrid::renderMap`, `gsMapCtrl` and its children

Scope: how the in-game minimap is produced and drawn. That covers the 256×256 map texture (`$grid.renderMap`) and
the raster it reads, the map control's world ↔ map transform and shader, the camera footprint
(`gsMapViewportCtrl`), pins (`gsMapItemCtrl`) and the selected-line ribbon (`gsMapRouteCtrl`). Builds on, and does
not repeat:

* [controls.md](controls.md): gsControl fields, image quad (§4), layers, `setImage`/`setShader`, visibility.
* [../renderer/interfaces-2d-output.md](../renderer/interfaces-2d-output.md): `gsIGuiPrimitive` (§2.4, type flags,
  `setMaterialParam`), `gsITexture::uploadBGRA` (§2.6), GUI blend state.
* [../renderer/terrain.md](../renderer/terrain.md): cell flags, `cellCover` (§2.4). [../renderer/grid-items.md](../renderer/grid-items.md):
  item types, record → item fields, hidden flag. [../save-format/README.md](../save-format/README.md) §3: cells and items.
* [../simulation/transport.md](../simulation/transport.md) §3.5 (same raster, Windows offsets) and
  [../simulation/grid-traffic.md](../simulation/grid-traffic.md) §5.2 (per-frame sweep budget), §4.2 (traffic word).

**Citations.** `L:0x<addr>.c:<line>` = `decompiled/Linux/functions/`, `W:0x<addr>.c:<line>` = `decompiled/GSystem-v2/functions/`.
`L:0x<fn>@<insn>` = instruction in the Ghidra listing of that Linux function (dumped headless from project `CiMLinux`
with `tools/ghidra/DumpAsm.java`), used where the x87/by-value-struct pseudocode lost operands. `S:<path>` =
`extracted/<path>`. **obs** = read in the code, **inf** = inferred. Confidence H/M/L.

**Conventions.** Offsets are **Linux** (gsControl fields: Linux = Windows − 0x38 / − 0x68, see controls.md §2.2;
gsGrid: map raster `L:+0x1b0` = `W:+0x1e8`, mode `L:+0x200`, sweep cursor `L:+0x228` = `W:+0x23c`). Colours are
`0xAARRGGBB` (`gsColor` stores B,G,R,A bytes, `L:0x080edf50.c`). `W`,`L` = grid width/length in cells, `cs` = cell
size (3.0). GUI units are pixels, y down.

---

## 0. Pipeline in one view

```
every 1 s / on mode change:  $grid.renderMap($texture, 256, 256, mode)
   ├ finish the raster sweep (grid+0x1b0: 256×256 × 16-byte counters) for `mode`, reset the sweep cursor  (§2)
   ├ per pixel: noise ground colour, lerp toward field/tree/water/pavement/network colours, data colour,
   │            black outline + line colour from the transport counters                                   (§3)
   └ texture.uploadBGRA(pixels, 256, 256)                                                                  (§3.6)
gsMapCtrl (256×256 control, minimap.shader): image quad, uv = custom0.xy + uv·custom0.z                    (§4)
 ├ gsMapViewportCtrl: camera frustum ∩ plane y=0 → 5-vertex white GL_LINE_STRIP                           (§5)
 ├ gsMapItemCtrl ×n: pin control centred on worldToMap(target)                                             (§6)
 └ gsMapRouteCtrl: selected line polyline → 2-px white pulsing GL_QUAD_STRIP                               (§7)
```

---

## 1. Script side (obs H)

### 1.1 Controls (`S:metro/ui/minimap/minimap.script`)

| Object | Setup | Lines |
|---|---|---|
| `$minimap = new_mapControl($bg.$content)` | rect (0,0,256,256); `setShader(minimap.shader)`; `targetModule = MAIN.$scene`; `offset = 0`; `scale = −1/(3·1024)`; `zoom 1`; `zoomFactor 0`; `minVelocity 0`; `maxDistance 1`; `canActivate false` | :77-89 |
| `level.script` override after the map is loaded | `$minimap.scale = −1/(3·$grid.width)`, then `update()` | `S:metro/scripts/level.script:61-64` |
| `$overlay` | sibling control, rect content −2 / 260×260, `setBorder($borderColor, 2)` (frame) | :91-97 |
| `$texture = video.new_texture()` | first `renderMap(tex, 256, 256, 0)`, then `$minimap.setImage($texture)` | :198-201 |
| `new_mapViewport($minimap)` | `.viewport = MAIN.$viewport` | :191-196 |
| `createRoute($line)` | `new_mapRoute`, `.route = line`, `setShader(/metro/shaders/path/route.shader)`, `sendToBack()`; called by the line panel for the selected line (`S:metro/ui/linepanel/linepanel.script:487`) | :255-265 |
| `createPin(obj, colour, name, pos)` | `new_mapItem`, rect (0,0,32,32), image `<name>-pin.dds` or `map-pin.dds` + `background = colour`, `targetModule = obj`, `targetPos = pos`, font `metro/ui/minimap/font` (5,5), margins (22,4,1,21), `canRotate = false` | :267-286 |
| mouse | `mouseDown["MouseLeft"]` / `onMouseDrag`: `pos = mapToWorldPosition(x, y)` → `gameUI.setCameraPos(pos.x, pos.z[, true])` | :181-189 |

Pins in the shipped scripts: stops of the selected line (`$pinColors = [0xFF0060FF, 0xFFFF0000, 0xFF00FF00,
0xFFFFFF00, 0xFFFF00FF]` by line type, text = stop number, `linepanel.script:895,914`), petitions/scenario markers
(`petition`, `0xA0FFFFFF`), news, and the secondary view's target (`arrow`, `cameratarget.script:24`). No script sets
`canRotate = true`.

### 1.2 When `renderMap` runs and with which mode

| Caller | Mode | Lines |
|---|---|---|
| creation | `0` | :200 |
| thread `"gameUI"`, `wait 1.0` loop | `$forcedMode` if ≠ 0, else `$dataMode` | :203-214 |
| `setDataMode(m)` (data panel), `forceMode(m)`, `update()` | same rule | :216-253 |
| data panel item × group | `GRID_VIEW_DEFAULT \| view \| group` | `S:metro/ui/minimap/datapanel.script:52-65` |
| stop tool active (`GRIDMODE_CREATE_STOP`) | `forceMode(0xfe \| GRID_VIEW_CATCHMENT \| ((toolMode+1) << 12))`, `toolMode` = `GRIDSTOP_*` 0..4 | `S:metro/scripts/ui.script:618,628` |

Mode bits used by the engine: `mode & 0xF0000` = data view, `(mode & 0x7000) >> 12` = class/stop selector `c`
(0 = all). Constants (`docs/native-api.md`): views HOMES 0x10000, WORKPLACES 0x20000, SHOPPING 0x30000, LEISURE
0x40000, GOVERNMENT 0x50000, TRAFFIC 0x60000, CATCHMENT 0x70000, POPULARITY 0x80000; groups BLUECOLLAR 0x1000 …
UNEMPLOYED 0x7000 (`c − 1` = `GRID_CITIZEN_*`); catchment selectors BUS 0x1000, TRAM 0x2000, METRO 0x3000, WATERBUS
0x4000, SPECIAL 0x5000 (`c − 1` = `GRIDSTOP_*`). The `0xfe` bits are ignored by `renderMap`. Traffic and popularity
keep the last chosen group bits; they do not affect those views. Switching `grid.viewMode` (underground view, the 3D
data layer) does not change the minimap mode.

---

## 2. The map raster (`grid+0x1b0`, obs H)

`gsGrid::create` allocates 0x100000 bytes = **256×256 records of 16 bytes**, zeroed (`L:0x0813af10.c:245-247`). Record
index = `py·256 + px`. The size is fixed and does not depend on the grid size.

| Byte | Meaning | Written by |
|---|---|---|
| 0 | number of transport marks (any type) | stops, line segments (§2.4) |
| 1..5 | marks per transport type `t+1` (0 bus, 1 tram, 2 metro, 3 boat, 4 heli) | stops, line segments |
| 6 | unused (type 5 would land here) | — |
| 7 | cells with field flag (§2.2) | sweep |
| 8 | tree proximity count | sweep |
| 9 | water cells | sweep |
| 10 | cells with flag 0x40 | sweep |
| 11 | pavement cells + covered cells of buildings | sweep |
| 12 | covered cells of items with type < 0x13 | sweep |
| 13 | number of data samples | sweep |
| 14..15 | u16 sum of data values | sweep |

### 2.1 Cell → pixel (obs H, `L:0x0813c2d0.c:61-62,105,115`)

```
sx = trunc(0x1000000 / W);  sz = trunc(0x1000000 / L)
px(x) = (x·sx) >> 16;       py(z) = (z·sz) >> 16          // code: ((z·sz) >> 8) & 0xff00 = py·256; record = py·256 + px
```

The truncated reciprocal matters: for W = 768 (sx = 21845) cell 3 still maps to pixel 0, so pixel 0 holds cells 0–3,
pixels 1–254 hold `3p+1 … 3p+3`, pixel 255 holds 766–767; same for 1280 (pixel 0 = cells 0–5, pixel 255 = 4 cells).
512 and 1024 divide exactly. Shipped grids are 512, 768, 1024 or 1280 cells square.

### 2.2 Sweep (obs H, `gsGrid::incrementalUpdate` `L:0x0813c2d0`, `W:0x100b6240`)

The grid's buckets (16×16-cell divisions, `bx·by` of them) are processed one at a time starting at the cursor
`grid+0x228`; `grid.move` spends a frame budget on it (grid-traffic §5.2), `renderMap` finishes it (§3.1).

```
when the cursor is at bucket 0: clear bytes 7..15 of all 65 536 records                      :76-94
bucket b: cells [x0, x0+16) × [z0, z0+16), x0 = (b % bx) << shift, z0 = (b / bx) << shift  :95-100
  per cell f = flags(x, z):                                                                   :110-139
     f & 0x2000 → [9]++   else f & 0x20 → [7]++   else f & 0x40 → [10]++   else f & 0x08 → [11]++
  per item in the bucket's list (bucket = footprint centre), skipping item flag 0x400000:    :146-152
     v = getObjectDataValue(item, grid+0x200)                                                 (§3.5)
     type == 0x13 (TREE): for every cell of [x−2, x+2] × [z−2, z+2] ∩ grid: [8]++            :153-191
     else if type < 0x13 or type == 0x14 or v != −1:                                          :194-217
        for every cell (cx, cz) of [x, x+sizeX−1] × [z, z+sizeZ−1] ∩ grid
           with cellCover(cx−x, cz−z) != 0  (terrain.md §2.4; 0xf00 for non-diagonal items):  :236-336
             type == 0x14: [11]++      type < 0x13: [12]++      (other types: nothing)
             if v != −1:   [13]++, [14..15] += v
cursor++ (stops at bx·by)
```

`x, z, sizeX, sizeZ` are the item fields (`+0x2c, +0x34, +0x38, +0x40`, sizes swapped for odd orientations,
grid-items.md §2.1). Trees use the 5×5 box around their min corner and ignore `v`. Items outside the grid are in no
bucket and never count. All counters are u8 and **wrap** (`char + 1`). Cell counters stay ≤ 36 (6×6 cells in pixel 0
of a 1280 grid), but the tree counter adds up to 25 per tree and can pass 255 in dense forest; reproduce the wrap.
The cell flags are read as stored (no terrain dry-out needed, §3.7).

### 2.3 Cell flag meanings (terrain.md)

0x2000 water, 0x20 field (fields also need `!(f & 0x40)` in the 3D view, here 0x20 wins first), 0x40 pavement
preview (none in shipped grids), 0x08 pavement/yard.

### 2.4 Transport marks (bytes 0..5, obs H; maintained incrementally, never cleared by the sweep)

* **Stops** (`gsTransportStopController::create` `L:0x081c2120.c:78-94`, undone in `::destroy` `L:0x081b9790.c:28-40`):
  centre `cx = item.x + (sizeX >> 1)`, `cz = item.z + (sizeZ >> 1)`, record `p = ((cz·65536)/L & 0xff00) | ((cx·256)/W)`
  (exact division, not the §2.1 reciprocal). `+1` on bytes 0 and `t+1` of `p`, `p−256`, `p−1`, `p+1`, `p+256` (plus
  shape, no bounds check). Stop type `t` from the item type: roads 2/5/6 → 0, tram 10–12 → 1, metro 13–15 → 2,
  dock 0x11 → 3, heliport 0x12 → 4 (`:44-76`).
* **Line segments** (`gsTransportSegment::updateMesh` `L:0x081ba9f0.c:447-463,640-648,688-721`, undone in
  `::clearRoute` `L:0x081b86e0.c`): only when segment flag bit 2 is clear (`:309`). Each path node's cell
  (`node+0x28`, `+0x2c`) and the midpoint of consecutive nodes (`(a+b)·s >> 17`) are mapped with the §2.1
  reciprocal (`+1` on bytes 0 and `type+1`, midpoint skipped if it is the same record). Bridge (0x4000) and tunnel
  (0x8000) pieces mark sampled positions `(int)((W·cs·0.5 + wx)/cs)` along the curve instead (the conversions run
  under a truncating x87 control word, `L:0x081ba9f0@081bb4c2-081bb4d0`). M: summarised, not traced in full.

---

## 3. `gsGrid::renderMap(tex, w, h, mode)` (`L:0x08143f60`, `W:0x100b71f0`; obs H)

### 3.1 Prologue (`L:0x08143f60.c:55-69`)

```
if grid+0x200 != mode: grid+0x200 = mode; cursor = 0          // a new mode restarts the sweep
if cursor < bx·by: incrementalUpdate(−1.0)                    // −1 = process every remaining bucket now
cursor = 0                                                    // the next frames sweep again
w = min(w, W); h = min(h, L); if w == 0 or h == 0: return
K = trunc(0x10000 / (trunc(W/w) · trunc(L/h)))               // asm L:0x08143f60@0814400b-0814402f
```

For the port this is equivalent to: clear bytes 7..15, run §2.2 over all buckets with `mode`, then render.

### 3.2 Per pixel (row-major, `y` = 0..h−1 outer, `x` = 0..w−1 inner; `L:…c:85-187`, asm `@081441bf-08144645`)

```
seed = 0                                                                  // once per call
for each pixel (x, y), r = record (row stride w, neighbours at ±16 / ±0x1000 bytes):
  seed = seed·0x343FD + 0x269EC3  (u32);   n = (seed >> 16) & 0xFF        // @081441bf-081441dd
  wt(c)  = min(255, (c·K) >> 8);   wt2(c) = min(255, (c·K) >> 9)
  col = lerp(0xD0495226, 0xD0414C20, n)                                   // ground
  if r[7]:  col = lerp(col, lerp(0xD070601A, 0xD0605010, n), wt(r[7]))    // field
  if r[8]:  col = lerp(col, 0xD0202C0A, wt2(r[8]))                         // trees (>> 9)
  if r[10]: col = lerp(col, 0xFF92491C, wt(r[10]))                         // flag 0x40
  if r[9]:  col = lerp(col, 0xD0295383, wt(r[9]))                          // water
  if r[11]: col = lerp(col, 0xFF42391C, wt(r[11]))                         // pavement + buildings
  if r[12]: col = lerp(col, 0xFF605D58, wt(r[12]))                         // network, props, stations …
  if r[13]: col = dataColour(u16 r[14..15] / r[13])                        // replaces (§3.4)
  if (x > 0 and left[0]) or (y > 0 and up[0]) or (x < 255 and right[0]) or (y < 255 and down[0]):
        col = 0xFF000000                                                   // outline (first hit wins)
  if r[0]: col = first of r[1..5] != 0 → [0xFF4040FF, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00, 0xFFFF00FF]
  out[y·w + x] = col
```

* The order is field, trees, 0x40, water, pavement/buildings, network (`:96-127`). The noise `n` is the same value for
  the ground and the field colour.
* The outline and line tests hard-code 256 records per row and the 255 limits (`@0814413b`, `@0814414d`), so `w` must
  be 256 (it is). With `w < 256` the records would be read as if rows were `w` long (quirk, unused).
* Line priority: bus > tram > metro > boat > heli. A pixel next to a line pixel is opaque black; the line pixel itself
  gets its type colour, so every line is drawn with a one-pixel black edge. Pixels with `r[0]` but no type byte keep
  the colour computed so far.

### 3.3 Colours and `lerp`

| Layer | Colour(s) | RGB, alpha | Weight |
|---|---|---|---|
| ground | 0xD0495226 → 0xD0414C20 by noise | (73,82,38) → (65,76,32), α 208 | `n` |
| field | 0xD070601A → 0xD0605010 by noise | (112,96,26) → (96,80,16), α 208 | `wt(r7)` |
| trees | 0xD0202C0A | (32,44,10), α 208 | `wt2(r8)` |
| flag 0x40 | 0xFF92491C | (146,73,28) | `wt(r10)` |
| water | 0xD0295383 | (41,83,131), α 208 | `wt(r9)` |
| pavement, buildings | 0xFF42391C | (66,57,28) | `wt(r11)` |
| types 0..0x12 | 0xFF605D58 | (96,93,88) | `wt(r12)` |
| outline | 0xFF000000 | | replace |
| bus, tram, metro, boat, heli | 0xFF4040FF, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00, 0xFFFF00FF | | replace |

`lerp(a, b, t)` = `gsInterpolate(gsColor, gsColor, uint)` (`L:0x080ee4d0.c`), a two-lane SWAR blend in u32 arithmetic
(wrap-around, logical shifts):

```
lo = ((((b & 0x00FF00FF) − (a & 0x00FF00FF)) · t) >> 8) + (a & 0x00FF00FF)
hi = ((((b & 0xFF00FF00) >> 8) − ((a & 0xFF00FF00) >> 8)) · t) + (a & 0xFF00FF00)
result = (lo & 0x00FF00FF) | (hi & 0xFF00FF00)
```

This equals, per channel, `a + floor((b − a)·t / 256)` with **floor also for negative differences** (checked on
200 000 random inputs). Rounding to nearest or truncating toward zero would differ.

Alpha is blended like a channel. Full coverage (`t = 255`) of an opaque colour over α 0xD0 gives **α 0xFE**, not
0xFF (seen in the shipped thumbnails).

### 3.4 `gsGetDataColor(v)` (`L:0x081812e0@081812fb-0818138a`, `W:0x100e7c90.c:21-28`)

```
v ≤ 127: lerp(0xFFFF0000, 0xFFFFFF00, 2v)            red → yellow
v ≥ 128: lerp(0xFFFFFF00, 0xFF00FF00, 2v − 256)      yellow → green (v = 255 → t = 254)
```

The average is `u16 sum / u8 count` (integer). Data pixels are opaque and hide the terrain colours.

### 3.5 Data values `gsGridItem::getObjectDataValue(item, mode)` (`L:0x081818b0.c`, `W:0x100eafd0.c`; obs H)

`g = mode & 0xF0000`, `c = (mode & 0x7000) >> 12`, `area = sizeX·sizeZ`, divisions are signed integer, results
clamped to ≤ 255. Template fields (`item+4`; Linux / Windows offsets): `homes[7]` +0x284/+0x2e8 (`addHomePlaces`),
`work[7]` +0x2a0/+0x304 (`addWorkPlaces`), `visits[13]` +0x2d8/+0x33c (`addVisitPlaces`), barred mask +0x310/+0x374
(`preventVisitors`). `word` = item `+0x5c` (traffic heat and building statistics, grid-traffic §4.2, citizens.md §5.6).

| g | Items | Value (else −1) | Lines |
|---|---|---|---|
| 0x10000 homes | BUILDING | `(c ? homes[c−1] : Σ homes) · 1024 / area` | :205-222 |
| 0x20000 workplaces | BUILDING | `(c ? work[c−1] : Σ work) · 1024 / area` | :178-192 |
| 0x30000 shopping | BUILDING, not barred for `c−1` (if `c`) | `visits[3] · 2048 / area` | :193-204 |
| 0x40000 leisure | same | `visits[4] · 2048 / area` | :45-56 |
| 0x50000 government | same | `visits[5] · 1024 / area` | :94-106 |
| 0x60000 traffic | types 2..15 | `u = (word >> 4) & 0xFF`; `q = (u < 32 ? u·1024 : ((u>>1)+0x70)·256) / (max(sizeX,4)·max(sizeZ,4))`; `255 − min(255, q)` | :58-92 |
| 0x70000 catchment | BUILDING | over all stops with type `c−1`: `d = |centre(item) − centre(stop item)|·cs` (centres `x + (size >> 1)`), keep `best = max(r − d)` for `d² < r²` (`r` = stop catchment, 100 for bus/tram, 150 otherwise); `v = trunc(best·255 / ((sizeX+sizeZ)·cs·0.25))`, clamped to 0..255; 0 if no stop reaches it | :108-168, asm `@08181d11-08181d25` (truncating `fistp`) |
| 0x80000 popularity | BUILDING | `(word & 0xF000) >> 8` (0..240) | :169-174 |
| anything else (0, 0xfe) | — | −1 | |

So every building gets a data colour in the building views (red when it has none of the counted places), roads get one
in the traffic view, and everything else keeps its normal colour. Mode 0 / `GRID_VIEW_DEFAULT` = no data colours.

### 3.6 Upload (obs H)

`tex->uploadBGRA(pixels, w, h)` (`gsITexture` slot 1, `L:0x08143f60.c:188`): the u32 array is BGRA bytes in memory,
uploaded as RGBA8 with LINEAR/LINEAR, REPEAT, no mipmaps (interfaces-2d-output §2.6). Row 0 of the array is texture
row 0 (`v = 0`). The map editor saves this texture as `<map>.dds` (A8R8G8B8 + CPU mips, `S:metro/scripts/main.script:276`),
which gives shipped reference images.

**Orientation:** pixel column ↔ cell x ↔ world x (left → right), pixel row ↔ cell z ↔ world z (top → bottom). No flips.

### 3.7 Validation (obs)

A JavaScript re-implementation of §2.2–§3.4 (`tools/minimap-check.mjs`; mode 0xfe, cells and items from the `.grid`,
item types and sizes from the template scripts via `tools/minimap-templates.mjs <extracted> <out.json> <location>`) was
compared with the shipped thumbnails in `maps.gs`, skipping pixels touched by a diagonal
(non-zero `diagonals`) or generated 45° template. **amsterdam and tutorialcity match bit-exactly** (61 244/61 244 and
59 223/59 223 pixels), helsinki on 59 438/59 456; 17 of its 18 differing pixels are within 2 px of skipped pixels
(generated templates whose footprint the check approximated, inf). berlin and vienna differ in whole
regions: `saveMapData` writes the 1920 base grid but the minimap of the editor's current date
(`main.script:265-276`), so those thumbnails show later timeline states (inf, M). The terrain dry-out
(terrain.md §2) makes no difference.

---

## 4. `gsMapCtrl` (obs H)

### 4.1 Fields (`initializeClassData` `L:0x081d90f0.c`, ctor `L:0x081dade0.c:16-26`)

| Linux | Script | Default | Meaning |
|---|---|---|---|
| +0x19c | `targetModule` | null | gs3DElement whose global position is the map centre |
| +0x1a0 | `offset` | 0 | world offset of the **image only** (§4.3) |
| +0x1ac | — | 0 | current centre `C` = target global pos (0 without target) |
| +0x1b8 | `scale` | 0.001 | `s`, map widths per world unit (negative in game) |
| +0x1bc | `zoom` | 0.15 | base zoom |
| +0x1c0 | `zoomFactor` | 0.01 | zoom per unit of target speed |
| +0x1c4 | — | 0.15 | current zoom `Z` |
| +0x1c8 | `minVelocity` | 5 | speed dead zone |
| +0x1cc | `maxDistance` | 0.4 | pin clamp radius in map widths (§6) |
| methods | `worldToMapPosition(v \| x, z)`, `mapToWorldPosition(v \| x, y)` | | §4.2 |

### 4.2 Transform (`L:0x081daf40.c`, `L:0x081daec0.c`, methods `L:0x081d99c0.c`, `L:0x081d9520.c`)

Map coordinates are control-local pixels (origin = control's outer top-left; `Wc`, `Hc` = control width/height).

```
worldToMap(P) = ( (0.5 − (P.x − C.x)·s / Z) · Wc ,  (0.5 − (P.z − C.z)·s / Z) · Hc , 0 )
mapToWorld(m) = ( (0.5 − m.x/Wc)·Z/s + C.x ,  0 ,  (0.5 − m.y/Hc)·Z/s + C.z )
```

Script forms: `worldToMapPosition(vector)` uses `.x/.z`, `(x, z)` two numbers; `mapToWorldPosition(vector)` uses
`.x/.y`, `(x, y)` two numbers. y of the world result is 0. In game (`Wc = Hc = 256`, `Z = 1`, `C = 0`,
`s = −1/(3W)`): `m.x = 256·(0.5 + P.x/(3W))` = `256·cellX/W`, same for z. There is **no rotation**: the map stays
axis-aligned (world +x right, world +z down) whatever the camera does.

### 4.3 Per tick (`gsMapCtrl::move` `L:0x081d8f60.c:28-70`, asm `@081d8f77-081d90bf`)

Only when the image primitive exists (`+0x104`, created by `setShader`/`setImage`):

```
C = targetModule ? targetModule.getGlobalPos() : (0,0,0)
if zoomFactor == 0 or !targetModule: Z = zoom
else: sp = max(0, |targetModule.vslot+0xd4(C)| − minVelocity)          // velocity-like vector, unused in game
      k = 0.5^dt;  Z = k·Z + (1 − k)·(zoom + sp·zoomFactor)
custom0 = ( (offset.x − C.x)·s + 0.5 − Z/2 ,  (offset.z − C.z)·s + 0.5 − Z/2 ,  Z ,  1 )
image.setMaterialParam(0, custom0)                                     // gsIGuiPrimitive slot 7
gsControl::move(dt)
```

### 4.4 Drawing

The control is a normal gsControl image quad (controls.md §4): rect = the control, uv = crop (0,0)…(1,1), vertex
colour = white (`setImage` sets the background to 0xFFFFFFFF), layer `z·256` in the parent's client area, blend
`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`. The material uses `S:metro/ui/minimap/minimap.shader` (attributes POSITION |
TEXCOORD0, textures 0|1, parameter `SHADER_CUSTOM0`):

```
vertex:   gl_Position = MVP · gl_Vertex;   v_mapCoord = custom0.xy + gl_MultiTexCoord0.xy · custom0.z
fragment: gl_FragColor = texture2D(texture0, v_mapCoord)        // vertex colour ignored; texture1 unused
```

So texel `u = 0.5 + (offset.x − P.x)·s`, i.e. `u = cellX/W` in game, consistent with §4.2 only while `offset = 0`
(a non-zero offset shifts the image but not pins, footprint or clicks). With `Z = 1`, `C = 0` the 256-px quad shows
the 256² texture 1:1. The ground and water alpha 0xD0 lets 18 % of the HUD panel behind the map show through.

---

## 5. `gsMapViewportCtrl` – camera footprint (`L:0x081d9b50`, `W:0x100b31e0`; obs H, read from the listing)

Fields: `+0x19c` owner map control (`setOwner` dynamic_cast, `L:0x081d8b10.c`), `+0x1a0` `viewport` (gsViewPort),
`+0x1a4` line primitive.

```
needs map, viewport and viewport.camera (gsViewPort +0x19c, a gsCamera)          @081d9b5a-081d9b85
once: prim = video.newGuiPrimitive(0x23 = GL_LINE_STRIP, map.getClientArea())     @081da630-081da664
      prim.setVertexCount(5); prim.setVisible(own effective flags & 2); prim.setLayer(z·256 + 10)
per tick:
  cam.calculate()                                         gsCamera slot +0x24
  E = cam.eye (+8), T = cam.target (+0xc), U = cam.up (+0x10), fov = cam.fov (+0x18, degrees)
  d = normalize(T − E);  r = normalize(cross(d, U));  u = cross(d, r)
  t = tan(fov·0.017453292·0.5);  u ·= t;  r ·= t · viewport.width / viewport.height    @081d9c71-081d9cbc
  rays, in this order: d+r+u, d+r−u, d−r−u, d−r+u
  for each ray q: P = q.y == 0 ? E + q : E + q·((0 − E.y)/q.y)          // ground plane y = 0, @081da498
      vertex = (map.desktopLeft, map.desktopTop, 0) + worldToMap(P), colour 0xFFFFFFFF, uv 0
  prim.setVertices(0, 5, [v0, v1, v2, v3, v0])                          @081da45e-081da471
```

* A closed 1-px white quadrilateral (no `glLineWidth` anywhere in the engine, so GL default width 1; untextured, no
  material). It lives in the map's client area, so the scissor clips it to the 256×256 map.
* `viewport.width/height` are the gsViewPort control's size (Linux +0xa8/+0xac). The frustum is intersected with
  world `y = 0`, not with the terrain. There is no check for rays that point upward (`t < 0` puts the corner behind
  the camera).
* `statusUpdated` forwards visibility changes to the primitive (`L:0x081d8a50.c`); the layer is set once at creation.

---

## 6. `gsMapItemCtrl` – pins (`L:0x081d96b0.c:35-95`, `W:0x100b2ec0`; obs H)

Fields: `+0x19c` owner map, `+0x1a0` `targetModule`, `+0x1a4` `targetPos`, `+0x1b0` `canRotate` (ctor default
**true**, `L:0x081dafd0.c`; scripts set false).

```
P = targetModule ? targetModule.getGlobalPos() : targetPos
m = worldToMap(P)                                                   (map-local px)
dx = m.x − map.W/2;  dy = m.y − map.H/2;  ρ = sqrt(dx² + dy²)
if ρ > maxDistance·map.W:  m = (dx·map.W·maxDistance/ρ + map.W/2,  map.H·dy·maxDistance/ρ + map.H/2)
if own width ≥ 1: setPosition(trunc(m.x − width/2), trunc(m.y − height/2))      // truncation, @081d9910-081d993b
else:             setPosition(m.x − width/2, m.y − height/2)
if image and canRotate:
   f = targetModule ? rotate(globalOrient, (0,0,−1)) : (0,0,−1)
   angle = gsArgument(f.z, f.x)       // = atan2(f.x, f.z) in [0, 2π): 0 for +z, π for −z (L:0x08108e00.c)
   image.setRotation(angle, (desktopLeft + width/2, desktopTop + height/2, 0))
gsControl::move(dt)
```

With `maxDistance = 1` and a 256-px map the clamp radius is 256 px, larger than the map's half-diagonal, so pins
inside the map are never moved; a pin pushed past the map's edge is hidden by the control `clipped` test or the
scissor. The 32×32 pin is centred on the point (no anchor at the pin tip). The pin is a normal control (image,
`background` tint, text with the minimap font), drawn at layer `z·256` in the map's client area.

---

## 7. `gsMapRouteCtrl` – selected-line ribbon (`L:0x081da6d0.c`, `W:0x100b3740`; obs H, brief)

Fields: `+0x19c` map, `+0x1a0` `route` (the line's `gsRouteObject`: slot +8 point count, slot +0xc fill world
points; transport.md §2.2, line `+0x13c`), `+0x1a4` primitive, `+0x1a8` material from `setShader` (`L:0x081d93d0.c`).

```
once: prim = newGuiPrimitive(0x0b = GL_QUAD_STRIP, map.getClientArea()); setVisible; setLayer(z·256 + 5);
      setMaterial(route material)                                                     :78-90
n = route.count(); if n < 2: setVertexCount(0) else setVertexCount(2n)               :92-96
k = map.height > 1 ? 0.25 : 0.00025                                                   :97-102, asm @081da73c-081da75e
m_i = worldToMap(p_i);  next = normalize(m_1 − m_0)
for i in 0..n−1:
   prev = next;  if i < n−1: next = normalize(m_{i+1} − m_i)
   w = (dot(prev, next) + 1)·k;   S = prev + next
   v_{2i}   = map.desktop + (S.y·w, −S.x·w) + m_i
   v_{2i+1} = map.desktop + (−S.y·w, S.x·w) + m_i          colour 0xFFFFFFFF, uv 0    :150-212
```

A straight run is 2 px wide (`S = 2d`, `w = 0.5`); at a bend of angle θ the half-width shrinks to `cos³(θ/2)`
px (no miter). `route.shader` colours it with `gl_FrontMaterial.diffuse` (the GUI pass sets diffuse = (1,1,1,1),
interfaces-2d-output §2.4) and alpha × `0.5 + 0.5·sin(6·timer)`: a **white ribbon pulsing with a 1.05 s period**,
drawn under the pins (`sendToBack`), over the map image. Loaded saves have no selected line until the player selects
one in the line panel.

---

## 8. Port checklist

1. **Raster.** Keep a 256×256×16-byte array per grid. Bytes 0..5: `+1/−1` on stop create/destroy (plus stamp, exact
   division) and on line-segment mesh build/clear (§2.4). Bytes 7..15: recompute in `renderMap` with the given mode
   (clear, then all cells and all non-hidden items in buckets, §2.2). Use the truncated reciprocal for cells → pixels.
2. **Item rules.** TREE: 5×5 box from the min corner, ignore data. BUILDING: covered cells → byte 11. Types 0..0x12:
   covered cells → byte 12. Covered = `cellCover != 0`, the same function as fixGrid (terrain.md §2.4). Data value
   per covered cell when `≠ −1` (§3.5).
3. **Pixels.** Weights `K = 65536 / (cells per pixel)`, `min(255, c·K >> 8)` (`>> 9` for trees); per-call LCG noise
   from seed 0; layer order and colours of §3.2/§3.3; the SWAR `lerp` exactly; data colour replaces; then outline,
   then line colour.
4. **Upload** BGRA bytes as RGBA8, linear, repeat, no mips; row 0 = top. Test: render amsterdam, helsinki and
   tutorialcity with mode 0xfe and compare with `metro/maps/<map>.dds` level 0 from `maps.gs` (bit-exact except
   pixels under diagonal items, which depend on the port's `cellCover`).
5. **Script hooks.** `renderMap` every second from the `"gameUI"` thread and on mode changes; `level.script` sets
   `scale = −1/(3·W)` after loading.
6. **Map control.** `move`: `C`, `Z`, `custom0 = ((o−C)·s + 0.5 − Z/2, …, Z, 1)` into the image material;
   `minimap.shader` samples `custom0.xy + uv·custom0.z`; ignore vertex colour; alpha blend. `worldToMap`/`mapToWorld`
   as §4.2 (control-local px; mouse events are already control-local, input.md §4).
7. **Footprint.** Camera eye/target/up/fov, aspect from the main viewport control, 4 corner rays ∩ y = 0, mapped with
   `worldToMap` + map desktop position, 5-vertex white 1-px line strip at layer `z·256+10` in the map's client area.
8. **Pins.** Centre on `worldToMap(target)`, truncate to integers when width ≥ 1, clamp radius `maxDistance·W`.
   Rotation only if `canRotate` (unused).
9. **Route.** Quad strip ribbon (§7), white, pulsing alpha, layer `z·256+5`.

---

## 9. Open questions

1. Diagonal and generated 45° templates were not pixel-checked (≈ 6–10 % of the thumbnail pixels on the checked maps).
   The code path is the shared `cellCover`, so a port that matches fixGrid should match here (M).
2. berlin/vienna thumbnails: confirm the timeline explanation by applying the `.timeline` up to the editor date the
   thumbnail was saved at (date unknown).
3. Line-segment marks (§2.4): segment flag bit 2, and the exact sampling for bridge/tunnel curves, were only skimmed.
   Needed once lines exist in the port.
4. gs3DElement slot `+0xd4` used by the zoom smoothing (velocity?) is unidentified; inert with `zoomFactor = 0`.
5. Screen direction of `setRotation` (axis constant, interfaces-2d-output §2.4 M) for `canRotate` pins; no shipped
   pin rotates.
6. Footprint rays above the horizon (camera pitch near 0) produce corners behind the camera; whether the game's
   camera limits allow it was not checked (same question as shadows.md §8.4).
7. Whether window MSAA / line smoothing affects the 1-px footprint lines in the original GL context (GUI pass state
   does not touch `GL_LINE_SMOOTH`, inf).
