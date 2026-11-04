# Graphs: `gsGraphCtrl`, the graphs panel and the annual report

Scope: the line-graph control `gsGraphCtrl` (data model, script API, what it draws) and the two panels that use it.
Also notes on the news ticker, which the same todo item named. Builds on, and does not repeat:

* [controls.md](controls.md): gsControl fields (§2.2), layers (§1), `mulA`, text items (§6), format codes (§9.2).
* [../renderer/interfaces-2d-output.md](../renderer/interfaces-2d-output.md): `gsIGuiPrimitive` type flags, `gsIGuiText`.

**Citations.** `L:0x<addr>` = `decompiled/Linux/functions/0x<addr>.c` (symbols from the Linux build),
`W:0x<addr>` = `decompiled/GSystem-v2/functions/0x<addr>.c` (Windows GSystem.dll). `S:<path>` = `extracted/<path>`.
**obs** = read in the code, **asm** = read in the instruction bytes where the x87 pseudocode lost operands,
**inf** = inferred.

**Conventions.** Offsets are **Linux** unless marked `W:`. gsControl fields used here (controls.md §2.2): `+0xa8/+0xac`
width/height, `+0xb0/+0xb4` desktop left/top, `+0xb8/+0xbc` client width/height, `+0xcc` text colour, `+0xd4` border
width, `+0xe8` char size, `+0xf4` spacing, `+0x16c` visibility, `+0x184` z, `+0x128 + 4·i` args. GUI units are pixels,
y down. Colours `0xAARRGGBB`.

---

## 1. Functions

| Function | Linux | Windows |
|---|---|---|
| `new_graphControl(parent)` (gsNewModule) | `0x081d3f00` | `0x100ada70` |
| ctor `gsGraphCtrl(gsObjectClassData&)` | `0x081d1a20` | (inlined) |
| `initializeClassData` (fields, methods) | `0x081d0820` | `0x100ad460` |
| `create` (4 label texts) | `0x081ce940` | `0x100ab530` |
| `destroy` | `0x081cf1d0` | `0x100ab680` |
| `move(dt)` (automatic update) | `0x081d2160` | `0x100ab810` |
| `update(int target)` (sampling) | `0x081d1cd0` | `0x100ab730` |
| `method_update` | `0x081d21f0` | `0x100ac460` |
| `method_reset` | `0x081cedd0` | `0x100ab4d0` |
| `setArg(i, v)` (vtable) | `0x081d0650` | `0x100abca0` |
| `setColor(i, c)` (vtable 0x198) / `method_setColor` | `0x081ce840` / `0x081cf6a0` | `0x100abd90` / `0x100ab490` |
| `method_getColor` | `0x081d07a0` | `0x100ac430` |
| `write_viewOffset` | `0x081d0ac0` | `0x100ac480` |
| `updateLine(i)` (vtable 0x190) | `0x081d0c40` | `0x100ac4d0` |
| `updateLines()` (vtable 0x194: all lines, texts, grid) | `0x081d2f30` | `0x100ae7c0` |
| `updateTexts()` | `0x081d2220` | `0x100adb50` |
| `statusUpdated` | `0x081d2dd0` | `0x100ae2f0` |
| `setControlIndex`, `updateDimensions`, `setFont`, `setTextShadow` | `0x081ceb70`, `0x081ceda0`, `0x081cf950`, `0x081cf710` | `0x100ab880`, `0x100ab960`, `0x100ab980`, `0x100abb40` |

`update(int)` and the ctor have no Linux-name match in the Windows set; `W:0x100ab730` is the target of
`method_update` (`W:0x100ac460`) and of `move` (`W:0x100ab810`). obs H.

## 2. Fields

| Linux | Windows | Script field | Type | Ctor default | Meaning |
|---|---|---|---|---|---|
| `+0x19c + 0x10·i` | | — | gsIGuiPrimitive* | 0 | line strip of series i (type 0x23 = line strip, created by `setArg`) |
| `+0x1a0 + 0x10·i` | | — | float[segmentCount] | 0 | ring buffer of series i (zeroed on allocation in `setArg`) |
| `+0x1a8 + 0x10·i` | | `setColor(i, c)` / `getColor(i)` | gsColor | DefColors | colour of series i |
| `+0x23c..+0x248` | | — | gsIGuiText*×4 | — | labels: zero, maximum, minimum, current value |
| `+0x24c` | | — | gsIGuiPrimitive* | 0 | grid (type 0x13 = lines), created on first need |
| `+0x250` | `+0x2b8` | — | int | 0 | ring index of the newest sample |
| `+0x254` | `+0x2bc` | `segmentCount` | int | 0x100 | ring size (samples kept) |
| `+0x258` | `+0x2c0` | `viewOffset` | int | 0 | ring positions hidden on the left; setter clamps to `0..segmentCount−2` |
| `+0x25c` | `+0x2c4` | `maxValue` | float | 0 | top of the value range |
| `+0x260` | `+0x2c8` | `minValue` | float | 0 | bottom of the value range |
| `+0x264` | | — | float | 0 | automatic-update clock |
| `+0x268` | | — | float | 10.0 | automatic-update rate (samples per second) |
| `+0x26c` | | — | float | 0.002 | not read by any gsGraphCtrl function (inf) |
| `+0x270` | `+0x2d8` | `automaticUpdate` | bool | **true** | sample every 1/rate s in `move` |
| `+0x274` | `+0x2dc` | `textFormat` | wstring | "" | pattern of the labels (format codes, controls.md §9.2); empty = no labels |
| `+0x278` | `+0x2f8` | `verticalLines` | int | 0 | every n-th vertical grid line is major; 0 = no vertical lines |
| `+0x27c` | `+0x2fc` | `horizontalLines` | int | 0 | base of the horizontal grid step; 0 = no horizontal lines |

Ctor `L:0x081d1a20`, defaults table `L:0x080d0df0` (`DefColors` = white, red, blue, yellow, lime, fuchsia, aqua,
orange). The ten series colours are white, red, blue, yellow, lime, fuchsia, aqua, orange, white, red. obs H.

The ring buffers are allocated with the `segmentCount` of the time; `segmentCount` is a plain field store
(`initField` without setter), so changing it after the first `setArg` would overrun the buffers. Scripts set it
before. inf H.

## 3. Data flow

### 3.1 Series and samples

A series is a control argument (`setArg(i, v)`, i = 0..9). `setArg` (`L:0x081d0650`):

1. allocates the series' ring buffer (`segmentCount` floats, zeroed) if it has none;
2. creates its line strip if it has none (owner's client area, layer `z·256 + 10 + i`, `segmentCount − viewOffset`
   vertices, visible = effective visible flag);
3. `updateLine(i)`, then `gsControl::setArg(i, v)` (the variant is kept as given: a value, an `@` reference or a macro).

`update(target)` (`L:0x081d1cd0`) steps the ring index from `current` to `target`; at each step every series whose
argument is not `none` stores `arg.toFloat()` at the new index, and `maxValue = max(maxValue, v)`,
`minValue = min(minValue, v)`. So the value range only **widens**: the script's `minValue`/`maxValue` act as the
smallest range shown, data outside it extends it. Then `current = target` and, if visible, `updateLines()`.

* `update()` (script) = `update((current + 1) % segmentCount)`: one sample.
* `move(dt)` (`L:0x081d2160`): with `automaticUpdate`, `clock += rate·dt`, `idx = ftol(clock) % segmentCount`,
  `update(idx)` when it changed. Only the debug FPS graph relies on it (`S:scripts/debug/debug.script:214`).
* `reset()` (`L:0x081cedd0`): deletes every line strip and ring buffer; `current = 0`, `maxValue = minValue = 0`,
  clock 0. The arguments and colours stay. (An `update` after `reset` without a new `setArg` would write into a
  freed buffer; the scripts always call `setArg` first.)
* `setColor(i, c)` stores the colour and calls `updateLine(i)` (the series must have a line: scripts call it after
  `setArg`). `getColor(i)` returns the colour (`0` → the shared zero variant).

### 3.2 Time ranges

The ring keeps `segmentCount` samples; `viewOffset` hides the oldest. Points drawn = `n = segmentCount − viewOffset`
(≥ 2 by the setter's clamp), the newest at the right edge. `write_viewOffset` re-runs `updateLines` when visible.

## 4. Drawing

Everything is a GUI item in the **owner's** client area (`(*owner)->getClientArea()`, vtable 0x184), like the
control's image and border (controls.md §1), at these layers: series strips `z·256 + 10 + i`, grid `z·256 + 0x1e`,
labels `z·256 + 0x1f`. So the grid lies **over** the series, the labels over both, and all of it under the control's
own client area (`z·256 + 0x80`). `updateLines` runs from `updateDimensions` (rect changes), `statusUpdated` (show /
hide; it also toggles the strips' visibility), `write_viewOffset` and `update`.

Let `b` = border width, `L,T,W,H` = desktop rect,

```
py   = maxValue > minValue ? (H·0.8 − 2b) / (maxValue − minValue) : 0     // pixels per value unit
base = minValue·py + (H·0.9 + T − b)                                       // y(v) = base − v·py
dx   = (W − 2b) / (n − 1)
```

`maxValue` maps to `T + 0.1·H + b`, `minValue` to `T + 0.9·H − b`: a 10 % margin above and below.

### 4.1 Series (`updateLine`, `L:0x081d0c40`)

Line strip of `n` vertices: vertex `k` (ring position `p = viewOffset + k`) at
`x = k·dx + L + b`, `y = base − py · data[(current + p + 1) % segmentCount]`, colour = series colour **without** the
visibility factor. The newest sample (`data[current]`) is the last vertex. GL line width 1. obs H.

### 4.2 Grid (`updateLines`, `L:0x081d2f30`, asm `L:0x081d32fa`, `0x081d3379`, `0x081d35f8`)

Only when `verticalLines ≠ 0` or `horizontalLines ≠ 0`. One `GL_LINES` item, two vertices per line.

Colours, all from the **text colour** `+0xcc` with `mulA` (constants read from the instruction bytes; the x87
pseudocode drops them): major = `mulA(text, 0.4·vis)` (`[0x082a4dc4] = 0.4`, × `+0x16c`);
vertical minor = `mulA(text, 0.15·vis·min(1, dx·(dx < 1 ? 100 : 0.1)))` (`[0x082a4374] = 0.15`, `[0x0829d134] = 100`,
`[0x0829f638] = 0.1`); horizontal minor = `mulA(text, 0.15·vis·max(0, 1 − nh / (nh·horizontalLines)))`, i.e.
`0.15·(1 − 1/horizontalLines)`. obs (asm) H.

* **Vertical**: `nv = n` lines when `verticalLines ≠ 0`, line `i` at `x = i·dx + L + b` from `T + b` to `T + H − b`;
  major when `(nv − 1 − i) % verticalLines == 0`, i.e. counted from the newest sample (with monthly samples and
  `verticalLines = 12`, one major line per year back from now). Dense grids fade: at 60 samples in ~313 px the minor
  lines get 0.53 of the 0.15.
* **Horizontal** (only when `horizontalLines = h ≠ 0` and `minValue < maxValue`): a decimal-like step search in base
  `h`:

  ```
  scale = 1 / h^6 (int power)
  do { k = ftol(scale · (max − min)); if (h <= 2k) break; scale *= h; } while (scale < 1000)
  (if the loop ran out: k = ftol(scale · (max − min)))
  nh = (6k + 4) / 5 + 2
  for j in 0..nh−1: s = ftol(scale·min − 0.5) − nh/10 + j;  y = base − (s / scale)·py;
                    line from L + b to L + W − b; major when s % h == 0
  ```

  The lines are at multiples of `1/scale` (a power of `h`), with `k` between `h/2` and `h²/2` steps over the range,
  so 4..61 lines; the ones beyond the range are clipped by the owner's client area. obs H.

`ftol` = `_ftol` (`FUN_101210d8`, truncation) in the Windows build; the Linux build rounds (`fistp`). The port follows
Windows.

### 4.3 Labels (`updateTexts`, `L:0x081d2220`)

Four `gsIGuiText` items created by `create` (owner's client area, layer `z·256 + 0x1f`, hidden), sized to the
control's client area (`setSize(+0xb8, +0xbc)`). `setFont` / `setTextShadow` pass the font, `mulA(text colour,
vis)`, char size, spacing and shadow on to them (`L:0x081cf950`, `L:0x081cf710`). Hidden when the control is not
visible or `textFormat` is empty. Each label's y is `base − v·py − charSizeY/2`, truncated when `charSizeY ≥ 1`;
text = `formatText(textFormat, [v], 1)`.

| Item | Value v | x | Shown when |
|---|---|---|---|
| `+0x23c` zero | `0` (int) | `L` | `minValue ≤ 0 ≤ maxValue` |
| `+0x240` maximum | `maxValue` | `L` | `maxValue > minValue` and not (`minValue < 0` and `maxValue / −minValue ≤ 0.2`) |
| `+0x244` minimum | `minValue` | `L` | `maxValue > minValue` and not (`maxValue > 0` and `minValue / −maxValue ≤ 0.2`) |
| `+0x248` current | `data₀[current]` (series 0) | `L + W − textWidth` | always (with a format) |

So with `minValue = 0` the minimum label never shows (the zero label stands for it); a label is dropped when it would
sit within 20 % of the zero line. There is no hover, tooltip or legend in the control. obs H.

## 5. Script users

### 5.1 Graphs panel (`S:metro/ui/graphspanel/graphspanel.script`)

Left HUD bar, 4th button (`levelbar.script:56`; hotkeys `graphsPanel`/`graphsPanel2`, `controls.script:208`). Two
type buttons (company / economy), six graph buttons each, a time-scale slider and the graph:

* control: `setRect(0, 4, clientW, clientH − 28)` in a 252 px area with border and `0x80000000` background,
  `setTextFont` (`ui.script:933`: Tahoma 8 at 15 px, colour `0xFFCCCCCC`, which is also the grid colour),
  `setBorder(0, 1, 0)` (border width 1, no visible sides: insets the plot by 1 px),
  `segmentCount = 60`, `viewOffset = 35`, `automaticUpdate = false`, `verticalLines = 12`, `horizontalLines = 10`.
* `selectGraphItem`: `reset()`, `textFormat`, `minValue`, `maxValue` from the graph's entry, `setArg(0, 0.0)`,
  `update()`, `setColor(0, 0xFF8DC73F)` (green), then one `setArg(0, h); update()` per history entry. The ring
  therefore holds a 0 and then the history (older entries drop out after 59).
* slider 1..5 (start 2): `viewOffset = max(0, 59 − round(4·value)·3)`: 12, 24, … 60 months (`graphs_time_pattern`
  "Show last %i0 months").
* `updateGraphs()` (from the monthly update in `player.script:400`, after the histories are appended): while the panel
  is open, `setArg(0, last entry); update()`.

Graphs (`addGraph(type, name, hint, pattern, history, min, max)`):

| Type | Graph | History (`MAIN.$gameData`) | Pattern | Range |
|---|---|---|---|---|
| company | coverage | `$serviceCoverageHistory` | percent `%i0%%` | 0..100 |
| company | usage | `$serviceUsageHistory` | percent | 0..100 |
| company | profit | `$totalMonthlyProfitHistory` | money `%it,d.20` (cents) | 0..100000 |
| company | reputation | `$companyReputationHistory` | percent | 0..100 |
| company | debt | `$companyDebtHistory` | money | 0..100000 |
| company | value | `$companyValueHistory` | money | 0..2000000 |
| economy | economy | `$economyHistory` | percent | −5..5 |
| economy | electricity | `$electricityPriceHistory` | money | 0..100 |
| economy | fuel | `$fuelPriceHistory` | money | 0..200 |
| economy | population | `$populationHistory` | number `%it,0` | 0..10000 |
| economy | unemployment | `$unemploymentHistory` | percent | 0..40 |
| economy | carusage | `$carUsageHistory` | percent | 0..100 |

### 5.2 Annual report (`S:metro/ui/annualreport/annualreport.script`)

`onYear` adds an "annualreport" petition; clicking it (`petitionbar.script:1074`) shows the 700×420 report. Its graph
(300×300, `segmentCount = 13`, `viewOffset = 0`, 12/10 grid lines) shows `$passengersCarriedHistory`: it feeds
entries `0 … #h − $mainClock.months`, and the 13-sample ring keeps the last 13 of them, i.e. the months up to the end
of the reported year. Number pattern, range 0..100, same green.

### 5.3 Debug overlay (`S:scripts/debug/debug.script:214`)

F9 FPS graph: two series bound by reference (`setArg(0, @$window.fps)`, `setArg(1, @$window.avgFps)`), automatic
update at the default 256 samples / 10 per second, series 1 grey. Not loaded by the metro game.

## 6. News ticker

The ticker is script-only (`S:metro/ui/petitionbar/petitionbar.script:749-960`): `addTickerText` queues
`[type, text, position, id, repeat, new]` in `MAIN.$gameData.$tickerItems`, a `"gameUI"` thread scrolls a text control
(`getTextWidth`, `left`), colours by type (`$tickerColors`), plays the news / warning / crash / breakdown sounds,
and the button moves the camera to the item (`MAIN.$gameUI.setCameraPos`). An idle thread posts random news. It needs
no native beyond what the port already has (threads, `array_splice`, text width, sounds): it scrolls, colours and
plays in the port.

## 7. Port

`port/src/engine/ui_graphs.cpp` (GraphObj: fields, `setArg`, `setColor`, `getColor`, `update`, `reset`, `move`;
`graphGeometry` computes strips, grid and labels from the state above at draw time instead of keeping GUI items),
drawn by `GuiPass::graph` in `port/src/engine/display.cpp` after the control's image and border. Deviations: the
geometry is rebuilt every frame (same result, as every input change triggers `updateLines` in the original);
`segmentCount` changes resize existing buffers, and `update` after `reset` skips series without a buffer, where the
original would overrun or crash.

Found while testing: the coverage, usage and car-usage histories ask `gsGridCitizens.getCanTravelCount(-1)` (and
`getTravelCount`, `getDriveCount`, `getWalkCount`, `getPriceTooHighCount`) for **all** classes; `−1` sums the seven
class counters (`L:0x081589e0`). The port returned 0 for it, so every month added a 0 % coverage sample.
