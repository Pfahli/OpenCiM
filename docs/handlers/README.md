# Native handler semantics

What the most-used native script functions and methods of `GSystem.dll` do, read from the Ghidra v2 pseudocode
(`decompiled/GSystem-v2/functions/`). Handlers were chosen by usage in the shipped scripts and by their role in a
reimplementation. Every entry gives a typed signature, behaviour, evidence (file and line numbers) and confidence,
with claims tagged *observed* or *inferred*.

| File | Scope | Entries | Confidence |
|---|---|---|---|
| [batch-A.md](batch-A.md) | simulation and grid: routes, grid objects, citizens, lines, timeline, grid save/load | 29 | 26 high, 3 medium |
| [batch-B.md](batch-B.md) | scene, audio, materials, particles, UI controls, viewport effects, state machines | 29 | 26 high, 3 medium |
| [batch-C.md](batch-C.md) | global functions (math, dates, strings, files, packages, materials, clocks) and the save-system methods of `gsScript` | 35 | 32 high, 3 medium |

The lists of handlers in each batch are in `batch-*.txt`. Calling conventions and the `gsVariant` slot table are in
[`../REPORT.md`](../REPORT.md) §5; class and field layouts are in [`../native-api.md`](../native-api.md).

## Spot checks

These claims were re-read against the pseudocode independently of the batch readers:

| Claim | Check |
|---|---|
| `routeExists` with 2 args counts lines covering a point (int); with 3–4 args runs a path search and returns a bool | `0x1011b8e0.c:34-75`: the `argc > 2` branch calls `FUN_1011a200` and returns true/false; otherwise returns `newInt(count)` |
| `addHomePlaces(class, n)` adds to `homes[class]` at object `+0x2e8` | `0x100e8430.c`: also sets `+0x370` once tourist homes (`homes[4]`) exceed 9 |
| `gsSound.load(path, [flag])` defaults the flag to true | `0x10098830.c`: `argc < 2 → 1`, else `args[1].toBool()` |
| loose files override archive entries | `0x1007f5e0.c:131-157`: a loose file deletes and replaces an existing record, while archive mounting only inserts absent keys (`0x1007ef80.c`) |
| `.material` files are chunked | `chunkscan` on `01 - Default.material`: `0x02013100` with texture-slot sub-chunks `0x…10` diffuse, `0x…20` normal, `0x…30` specular, `0x…50` ambient occlusion |

## Findings that matter for a reimplementation

**Simulation (A)**
* `routeExists(mask, from, [to], [directOnly])` has two modes (see above). The path search uses randomized costs,
  so every call advances `rand()`. The destination accepts any stop within ±50 cells.
* `gsGridObject` template tables sit at fixed offsets: homes[7] `+0x2e8`, work[7] `+0x304`, visits[13] `+0x33c`.
  There is also a barred-visitor class mask at `+0x374` and an "open N of every M days" word at `+0x3c0`.
* The citizen tuning calls (`setPriceSignificance`, `setReasonProbability`, `setUtilProbability`, `addCar`,
  `addCharacter`) fill per-class tables. The fourth value of `setReasonProbability` does not affect the trip-reason
  roll. `addCar` throws on an unknown id, while `addCharacter` ignores it.

**Scene, audio and UI (B)**
* One path resolver: a leading `/` or `~/` means the archive root; anything else is relative to the object's base
  directory (`+0xb0`). Material texture/shader, particle texture and font paths go straight to the cache.
* Missing textures and shaders never fail: the engine substitutes a default texture or
  `shaders/default/default.shader`.
* Audio uses FMOD Ex. Files are read whole into memory, opened 2D or 3D with linear rolloff, and cached by path,
  so the first load of a file fixes its 2D/3D mode.
* `gsStateMachine.perform` only queues the action; the queue drains on the timer tick.

**Globals and saves (C)**
* `mkdate` returns days since 1 Jan year 0 in the proleptic Gregorian calendar (1 Jan 1970 = 719528); `mktime`
  returns milliseconds since midnight. **Encoder bug:** years like 1901–1903 skip their offset and encode as
  1 Jan 1900. The clock's decoder is correct, and shipped scripts never use those years.
* File system:
  * The virtual file system is one case-sensitive map; `userdata/` maps to the folder given to `init_userdata`.
  * Writes to `memory/` create RAM files that survive `updateFileSystem`.
  * Absolute and `/`-prefixed paths go straight to disk.
  * Writes never create directories.
  * **Loose files override archive entries.**
* `exportVariables[To]` uses the script's own source file as a template: it rewrites or appends `$name = value;`
  lines (CRLF). `writePackage` stores bare file names and registers the new entries immediately.
* Runtime error code `0xf` is *"Calling a function with invalid argument(s)!"*.

## Weakest entries

* Batch A: `loadGridObject` (Ghidra lost the stack frame), and the record layouts behind `saveToFile` and
  `applyChanges`.
* Batch B: how `loadAnimation` combines bone groups, when the `playAnimation` callback fires, how `setArg` formats
  text, and ownership in `addEffect`.
* Batch C: `loadMaterial`'s field layout, and the role of the companion script loaded by `loadStateMachine`.
