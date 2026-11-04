# Batch A: grid and simulation handlers

The grid is a cell lattice (`gsGrid.width` × `length` cells, `scale` world units per cell). Items sit at integer cell coordinates (x, z), with y stored in 1/256-cell steps. World position = `(cell - gridSize/2) * scale` (world origin at the grid centre). Items are bucketed into square "slots" of 2^shift cells (`grid+0x148/0x14c` slot dims, `+0x150` slot array). Each item points at a shared `gsGridObject` template, registered under its `idName` in a map at `grid+0x1ec`. The script-facing controller for an item is created the first time something asks for it.
Templates carry the simulation's demand/supply tables: home places per citizen class, work places per class with an optional "utility trip" visit type, visit places per `GRID_VISIT_*`, a mask of classes barred from visiting, and an "open n of every m days" rule. `gsGridCitizens` holds the matching per-class tuning: trip-reason percentages, price sensitivity, utility-trip probability, vehicle/character model pools filtered by an era bitmask, and trip statistics.
A transport line is a circular list of (stop, line) nodes. `routeExists` runs a Dijkstra search over those nodes with randomised costs (walking distance, stop attractiveness, ticket price), and the citizens' own route choice appears to use the same search. Timeline actions are replayed day by day with a 182-day "preview" pass ahead of the real one.
Tags: **[obs]** = seen in pseudocode, **[inf]** = inferred (naming, purpose, or behaviour not directly visible).

## gsTransportLines.routeExists
- **Signature:** `routeExists(mask: int, posA: vec3, [posB: vec3], [directOnly: bool]) -> int | bool`
- **Behavior:** With 2 args, it returns an **int**: the number of (line, stop) memberships whose stop catchment circle covers `posA`, counting only active lines whose type bit is in `mask` [obs]. With 3–4 args, it returns **true/false**: whether a public-transport route exists from `posA` to `posB` using lines in `mask` [obs]. `directOnly = true` forbids changing lines, so the route must be one line [obs]. Scripts mostly call it as `routeExists(TRANSPORTLINE_MASK_ALL, $a.position, $b.position, true)`.
- **Details:**
  - The mask test is `mask & (1 << line.type)` (line `+0x16c`) plus the active bit `line+0x6c & 0x10000` [obs]. The 2-arg count can count one line more than once if several of its stops cover the point [inf].
  - The vec3 args are world positions rounded to grid cells with `_ftol`. The x87 arithmetic doing the world→cell conversion is not visible [inf].
  - Start set: stop nodes within ±50 cells whose distance to `posA` (× cell size `this+0xec`) is ≤ the stop's `catchmentArea` (stop `+0x174`) [obs, `FUN_10114560`].
  - Goal test: any reached node whose stop item centre lies inside a ±50-cell square around `posB`. **No catchment check at the destination** (asymmetric) [obs].
  - Edge costs call `FUN_10083250` = random float in [a,b]. Inputs are walking distance, line float `+0x174` (effective attractiveness: `attractiveness × mean vehicle attractiveness / (1 + 0.01·avgWait)`, recomputed by the line tick; see `../simulation/transport.md`), stop attractiveness `+0x17c`, segment length (`node[8]+0xc`), and `max(ticketPrice - 200, 1)` (`+0x184`) [obs]. The price is charged at the first boarding and, on transfers, only for a line type not yet in the route's paid-type mask [obs/inf: the heap entry is `{cost, paidMask, node}`, but Ghidra reuses the stack slots]. Whether a route exists does not depend on the costs [inf]. However, each call **advances the C `rand()` state**, which matters for a deterministic reimplementation [obs].
  - Nodes the search marks as visited are reset afterwards (flags `&= ~6`, best cost = -1.0) [obs]. The found path is put in a temporary that is freed straight away; nothing is returned except the bool [obs].
- **Evidence:** `0x1011b8e0.c:34` (mask), `35-61` (3–4 arg path, `FUN_1011a200`, returns `DAT_10193ab8`/`ab4`), `62-75` (2-arg count via `FUN_10114720`, `FUN_1000a560`). Callees read: `0x1011a200.c:61-259` (Dijkstra, heap `FUN_10118b90`/`FUN_10114db0`, transfer loop 180-228 gated by `param_8`), `0x10114560.c`, `0x10114720.c` (catchment queries), `0x100932e0.c` (variant→vec3, falls back to `VEC_ZERO`), `0x10083250.c` (random range), `0x10116d70.c`.
- **Confidence:** high for the return types and the effect of mask/directOnly. Medium for the exact cost formula (x87 operands are hidden) and for the cell-conversion maths. The difference between the two node buckets `+0xf0` and `+0xf4` (next pointer `+0x48` vs `+0x4c`) is unknown.

## gsTransportLines.countLines
- **Signature:** `countLines(mask: int, activeOnly: bool) -> int`
- **Behavior:** Counts child modules of the line manager that are `gsTransportLine`s with `(1 << type) & mask` set. When `activeOnly` is true, it also requires the line's active bit (`+0x6c & 0x10000`) [obs].
- **Details:**
  - It iterates the module child vector `this+0x4c..0x50`; non-line children are skipped by an RTTI cast [obs].
- **Evidence:** `0x10116700.c:16-17` (args), `19-28` (loop, cast, type/active test), `29` (newInt).
- **Confidence:** high. The active-bit = `active` field mapping is inferred (registrar lists `active` as a bit field at `0x6c`).

## gsTransportLines.countStops
- **Signature:** `countStops(mask: int, onLinesOnly: bool, [stopName: string]) -> int`
- **Behavior:**
  - `onLinesOnly == false`: counts every non-temporary stop in the stop list (`this+0xfc`) whose stop `type` (`+0x178`) is in `mask` [obs].
  - `onLinesOnly == true`: counts **distinct** stops served by active lines whose type is in `mask`. It walks each line's circular stop-node list and puts each stop into a `std::set` [obs].
  - `stopName`, when given, must equal the stop template's `displayName` exactly (`gsGridObject+0x2cc`) [obs].
- **Details:**
  - The temporary check is item flag `0x100000` (`GRID_ITEM_TEMP`) on the stop's item (`stop+0x13c`) [obs]. Only the `false` mode skips temp stops; the line mode does not check it [obs].
  - Quirk in `false` mode: if `stopName` is given, a stop whose type is **not** in `mask` is still counted when its name matches. With no name given, such a stop counts only if its displayName is empty [obs: the condition is `typeMatch && noFilter || compare(displayName, filter)==0`].
  - `FUN_100118e0` is `wstring::compare` (0 = equal) [obs].
- **Evidence:** `0x10119dd0.c:42-59` (args, optional string), `60-83` (stop-list mode, temp flag at 67, compare at 76), `85-123` (line mode, cast at 91, node walk 96-114, set insert `FUN_10117bc0` at 101, set size → `FUN_10011860` at 117). Callee `0x100118e0.c`.
- **Confidence:** high. The mismatched-type quirk is observed but looks unintended.

## gsTransportLines.getStop
- **Signature:** `getStop(index: int) -> gsTransportStopController`
- **Behavior:** Returns `this->stops[index]` from the stop vector at `+0xfc`, with its reference count raised (tail-call to vtable `+0x04` addRef) [obs].
- **Details:**
  - **No bounds check.** An out-of-range index reads past the vector [obs]. Scripts loop `0 .. getStopCount()-1` or use `random(getStopCount())`.
  - The vector holds stop controllers (fields `+0x13c` item and `+0x178` type are used by `countStops`) [inf].
- **Evidence:** `0x10115080.c:13` (toInt), `14-16` (indexed addRef/jump).
- **Confidence:** high, apart from addRef's return value, which is assumed to be `this` [inf].

## gsTransportLine.calculateLength
- **Signature:** `calculateLength() -> float`
- **Behavior:** Returns the total length of the line in world units by walking its circular stop-node list (`+0x148`, next `+0x24`) [obs]. Scripts compare it to `km*1000.0`, so the unit is probably metres [inf].
- **Details:**
  - Helicopter lines (`type == 4`): sum of straight 3D distances between consecutive stops. Stop centres are converted to world space with the grid at `line+0x144`: `((x - w/2)*scale, y*scale/256, (z - l/2)*scale)` [obs]. Because the list is circular, the leg from the last stop back to the first is included [obs].
  - Other types: for each stop node with a path to the next stop (`node+0x20`), sums the segment lengths `|p(+0x30) - p(+0x4c)|` over the path's decoded segment chain (`FUN_100e1810` decodes a 4-bit-per-step direction stream) [obs]. As a side effect it resets the path's iterator field `path+0x10 = 0` [obs].
  - If the line has no stops, it returns 0.0 [obs].
- **Evidence:** `0x101137f0.c:21-54` (loop), `25-43` (helicopter branch), `45-52` (path branch), `55` (newFloat). Callees `0x10084ff0.c` (vec3 distance), `0x100e1810.c` (path iterator).
- **Confidence:** high for the structure; the unit (metres) is inferred.

## gsTransportLine.getVehicles
- **Signature:** `getVehicles() -> array<gsTransportVehicle>`
- **Behavior:** Returns a new script array with every vehicle in the line's vehicle vector (`+0x15c..0x160`), each addRef'd, in vector order [obs].
- **Details:**
  - Array creation is `FUN_1000a6a0`. Elements are stored with the array's vtable `+0x40(index, value)`, a set-at-index slot not in the known list [obs/inf].
  - The vector holding vehicles is inferred from the `gsTransportLine.addVehicle` pairing and the script usage.
- **Evidence:** `0x10114e70.c:18-24`; callee `0x1000a6a0.c` (allocates a 0x18-byte array variant).
- **Confidence:** high.

## gsGrid.getRandomItem
- **Signature:** `getRandomItem(template: gsGridObject) -> gsGridItemController | null` or `getRandomItem(kind: int, citizenClass: int, min: int, max: int) -> gsGridItemController | null`
- **Behavior:** Picks a random grid slot (`rand() % (slotsX*slotsZ)`), up to 4096 times, and returns the controller of the **first** suitable item in that slot [obs].
  - Template form (argc < 4): the item's template must be the given object [obs].
  - 4-arg form: the item must be a building (`item+0x58 == 0x14`, `GRID_ITEM_BUILDING`) whose template value is within `[min, max]` [obs]. The value is:
    - `kind == 1` (`GRID_VISIT_HOME`): `homePlaces[class]` (`+0x2e8`).
    - `kind == 2` (`GRID_VISIT_WORK`): `workPlaces[class]` (`+0x304`).
    - Any other kind: `visitPlaces[kind]` (`+0x33c`), and the class bit must not be set in the template's `preventVisitors` mask (`+0x374`).
- **Details:**
  - Items with flag `0x400000` are always skipped. That flag has no `GRID_ITEM_*` name; it may mean ruined or hidden [inf].
  - With 2 or 3 args the template path is still used; args 2–3 are ignored [obs]. If arg0 is not a `gsGridObject` the result is `null` [obs].
  - The choice is not uniform: slots are equally likely, but within a slot the first match always wins [obs]. `kind == GRID_VISIT_EMERGENCY` (8) will normally find nothing, because `addVisitPlaces` spreads type 8 onto 9/10/11 [inf].
  - The return value comes from `FUN_100ec260`, which lazily creates and caches the item's controller at `item+8`: a `gsTransportStopController` for stop items (flag `0x2000`), otherwise a `gsGridItemController` [obs]. It can return C `0` instead of script null if the item has no grid [obs].
  - Consumes `rand()` [obs].
- **Evidence:** `0x100b6060.c:22-49` (template form, cast at 25, loop 32-46), `51-89` (4-arg form, building test 63, homes 65, work 75, visit and preventVisitors 69-71). Callees `0x100ec260.c`, `0x100ce5d0.c` (confirms `+0x374` preventVisitors and `+0x3c0` semantics).
- **Confidence:** high for the logic; the meaning of flag `0x400000` is unknown.

## gsGrid.getGridObject
- **Signature:** `getGridObject(idName: string) -> gsGridObject | null`
- **Behavior:** Looks up a template registered by `loadGridObject` in the grid's `std::map<std::string, gsGridObject*>` at `+0x1ec`. It returns the object addRef'd, or `null` if the name is not registered [obs].
- **Details:**
  - The key is converted to a narrow (byte) string with `FUN_1002eeb0`, so the lookup is case-sensitive and exact [obs/inf].
- **Evidence:** `0x100babf0.c:23-36`; callee `0x1002eeb0.c` (variant→`std::string`).
- **Confidence:** high.

## gsGrid.loadGridObject
- **Signature:** `loadGridObject(idName: string, objectFile: string, [fallbackFile: string], [scaleX: float, scaleZ: float]) -> gsGridObject`
- **Behavior:** Creates a new `gsGridObject` (0x3f0 bytes), loads it from `objectFile` through the object's virtual load (vtable `+0x18c`), and stores `idName` in its `idName` field (`+0x2b0`). It then registers the object in the grid's id map (`+0x1ec`) and in a connection-pattern map (`+0x1f8`), and returns the object [obs]. Running the `.object` file's script is what fills in width, flags, connections and home/work places [inf].
- **Details:**
  - 3+ args: gives the object a 45° rotation (quaternion from π/4, stored at `+0x1b8`) [obs]. If `<objectFile minus extension>.script` exists, `objectFile` is registered as a virtual-file alias of `fallbackFile` (`FUN_10080890`); otherwise `fallbackFile` is loaded instead [obs/inf]. This is how the "b" diagonal variants of buildings are made.
  - 5 args: sets a vec3 `(scaleX, 1.0, scaleZ)` at `+0x1ac` [obs]. Scripts pass `target*√2/size`, which fits a load-time scale for the 45° variant [inf].
  - `idName` longer than 31 bytes logs `"Object ID too long: <id>"` as an error, and loading continues [obs].
  - If the id already exists it logs `"Warning: Dublicate object ID: <id>"`, releases the old object and replaces the map entry [obs]. Entries in `+0x1f8` that still point at the old object are not removed, so the pointer may dangle [inf].
  - Connection map: the key uses `flags & 0xcf0f0`, `connections` (`+0x250`) and `connections2` (`+0x254`). If either connection mask is non-zero, the 90°-rotated masks (`FUN_100e7be0(mask, 6)`) are registered for rotations 3, 2, 1. It stops after the first rotation if the masks are symmetric under 180° (`FUN_100e7be0(mask,4) == mask`) [obs]. This drives auto-selection of road and track pieces from neighbour connectivity [inf].
  - A grid byte flag (`+0x206`) triggers an extra call (`FUN_1007d4c0`), perhaps debug timing [inf].
- **Evidence:** `0x100c2b20.c:64-71` (alloc/ctor), `79-133` (path args, rotation 84, scale 91-97, `.script` probe 103-108, alias/fallback 123-127), `137` (load), `141-170` (idName, length check 152), `175-198` (duplicate), `199-221` (map insert, rotation loop), `223` (addRef return). Callees `0x1007cfc0.c` (replace extension), `0x100805f0.c` (file exists), `0x10080890.c` (VFS alias), `0x100f04e0.c` (ctor defaults).
- **Confidence:** medium. Ghidra lost track of the stack frame (`unaff_retaddr` = argc, `iStack_10` = grid), so the variable roles are partly reconstructed. Unknown: the exact key layout of `+0x1f8` and the meaning of the `+0x1c8` default vec3.

## gsGrid.findItem
- **Signature:** `findItem(template: gsGridObject) -> controller | null`, or `findItem(name: string, [pos: vec3]) -> controller | null`
- **Behavior:**
  - Template form: scans every slot and returns the controller of the first item that uses the template [obs].
  - Name form: returns the first item whose custom name (wstring at `item+0x60`) equals `name` exactly [obs]. With `pos`, only items returned by an area query around `pos` (`FUN_100b6ba0(x-1, z-1, 2, 2)`) whose world-space centre lies within distance 1.0 of `pos` are considered [obs].
- **Details:**
  - Template form skips items with flag `0x400000`; the name forms do not [obs].
  - Item world centre = `((2x + w - gridW)*scale/2, y*scale/256, (2z + l - gridL)*scale/2)` [obs].
  - A non-`gsGridObject` object argument falls through to the string path, via its string conversion [obs].
  - Scripts use the `(name, pos)` form for mission targets such as `findItem($task["arg0"], $task["pos0"])`.
- **Evidence:** `0x100b9e80.c:36-64` (template form), `68` (toString), `71-96` (global name scan), `98-133` (positional, distance test 115). Callees `0x100ec260.c`, `0x100118e0.c`.
- **Confidence:** high for behaviour. Medium for the argc test, which Ghidra rendered as `param_1 < 2` because of the stack confusion. Medium for whether `FUN_100834f0` is plain distance or squared distance; either way the threshold is 1.0.

## gsGrid.getAverageY
- **Signature:** `getAverageY(x: int, z: int, w: int, l: int) -> float`
- **Behavior:** Scans the terrain height map (`+0x1c4`, int16 heights with a 4-byte stride and row stride `width`) over cells `[x .. x+w] × [z .. z+l]`, both ends inclusive and clamped to the grid. It returns `(min + max) / 512`: the **mid-range** in cell height units, not a true average [obs].
- **Details:**
  - The low bounds are clamped to ≥ 0 and the high bounds to `width-1` / `length-1`. The high bound is computed from the *unclamped* x/z [obs].
  - An empty range returns 0.0 [obs].
  - Multiply by `grid.scale` to get world Y [inf]. Scripts compare the result against item-relative heights.
- **Evidence:** `0x100b5c30.c:26-38` (args, clamps), `39-62` (min/max scan, address at 47), `63` (newFloat with ×0.001953125).
- **Confidence:** high.

## gsGrid.loadFromFile
- **Signature:** `loadFromFile(path: string) -> unset`
- **Behavior:** A thin wrapper around `FUN_100c3760(grid, path)`. That opens the file, checks for root chunk `0x07000000` and parses it with `FUN_100c3310` [obs]. Loading clears citizens, graph and lines, resizes the grid from the 0x18-byte header, then reads chunks: `0x07010000` heights, `0x07020000` items, `0x07040000` vehicles, `0x07050000` characters, `0x07060000` lines, `0x07070000` citizens [obs].
- **Details:**
  - It fires `onLoadProgress` (event id 8) with 0.1, 0.3 and 0.9 during parsing and 1.0 at the end. The 1.0 event fires even if the file was missing or had the wrong root chunk [obs].
  - A bad or missing file fails silently, with no script error [obs].
  - The vehicle and citizen chunks are skipped when `ignoreCitizens` (`+0x24e`) is set [obs].
  - Paths go through the engine's virtual file system, which is why scripts can use `memory/base.grid` [inf].
- **Evidence:** `0x100c4660.c:24-32`; callee `0x100c3760.c` (header check, progress event); `0x100c3310.c:37-48` (reset), `62-153` (chunk loop).
- **Confidence:** high for the flow; the contents of individual chunks were not read.

## gsGrid.saveToFile
- **Signature:** `saveToFile(path: string) -> unset`
- **Behavior:** A thin wrapper around `FUN_100c3840(grid, path)`. It writes the chunked grid file: `0x07000000` root (0x18-byte header), `0x07010000` raw height map, and `0x07020000` items with sub-chunks `0x07020100` (template idName table), `0x07020210` (16-byte item records, persisted flags masked with `0x72500000`) and `0x07020310` (item custom names). It then asks vehicles (`+0x20c`), characters (`+0x210`), lines (`+0x218`) and citizens (`+0x214`) to serialise themselves using pointer→index maps [obs].
- **Details:**
  - Returns unset. No error is reported to the script [obs].
- **Evidence:** `0x100c46e0.c:24-32`; callee `0x100c3840.c` (chunk ids at lines 81, 90, 107, 204, 260, 311; subsystem writers 363-366).
- **Confidence:** medium: only the outline was skimmed (403 lines), and the record layouts were not documented.

## gsGridObject.setPathFlags
- **Signature:** `setPathFlags(pathName: string, flags: int) -> unset`
- **Behavior:** Finds a named path on the template (map `+0x29c`, filled from the loaded mesh and by `addPath`) and sets its flags (the first dword of the path record) [obs].
  - If `flags` contains any "medium" bits (`0x1000ff00` = `GRID_PATH_WALK` plus the road/tram/train/metro/runway/water byte), the path flags are replaced completely [obs].
  - Otherwise the path keeps its existing medium bits and all other bits are replaced by `flags`. Old modifier flags are dropped, not ORed [obs].
- **Details:**
  - An unknown path logs `"Path not found: <object name> <sep> <pathName>"` at error level and is otherwise ignored, with no script error [obs]. The separator string `DAT_101665dc` was not resolved.
- **Evidence:** `0x100edb20.c:44-54` (lookup), `55-99` (error log), `101-107` (flag merge).
- **Confidence:** high.

## gsGridObject.addVisitPlaces
- **Signature:** `addVisitPlaces(visitType: int, count: int, [openDays: int, periodDays: int]) -> unset`
- **Behavior:** Adds `count` to `visitPlaces[visitType]` (int[13] at `+0x33c`) [obs]. `GRID_VISIT_EMERGENCY` (8) is special: it adds `count` to INJURY (9), FIRE (10) and POLICE (11) instead [obs].
- **Details:**
  - With 4 args it stores `(openDays << 16) | periodDays` at `+0x3c0`, overwriting any earlier value [obs]. Citizen destination choice (`FUN_100ce5d0`) uses it for visit types 3–5 (shopping, leisure, government): the place is eligible only on days where `(clock.day + item.x + item.z) % periodDays < openDays` [obs]. So `addVisitPlaces(GRID_VISIT_LEISURE, 300, 10, 50)` is a venue open 10 days out of every 50, staggered by location.
  - If shopping + leisure + government places exceed 19, it sets the byte flag `+0x370`. `findNames` uses that flag to offer the building's displayName as a stop name ("landmark") [obs/inf].
  - There is no bounds check on `visitType` [obs].
- **Evidence:** `0x100e8500.c:16-26` (add / emergency spread), `27-32` (packed days), `33-35` (landmark flag). Callees read: `0x100ce5d0.c:44-51` (reader of `+0x3c0` and `+0x374`), `0x100c62b0.c:100-104,179` (reader of `+0x370`).
- **Confidence:** high. The "landmark naming" purpose of `+0x370` is inferred from `findNames`.

## gsGridObject.addHomePlaces
- **Signature:** `addHomePlaces(citizenClass: int, count: int) -> unset`
- **Behavior:** `homePlaces[citizenClass] += count` (int[7] at `+0x2e8`) [obs]. If `homePlaces[GRID_CITIZEN_TOURIST]` (index 4, `+0x2f8`) exceeds 9, it sets the landmark flag `+0x370`, so big hotels become stop-name candidates [obs/inf].
- **Details:**
  - There is no bounds check on the class [obs].
  - `getRandomItem(GRID_VISIT_HOME, class, min, max)` reads this table [obs].
- **Evidence:** `0x100e8430.c:14-21`.
- **Confidence:** high.

## gsGridObject.addWorkPlaces
- **Signature:** `addWorkPlaces(citizenClass: int, count: int, [utilityVisitType: int]) -> unset`
- **Behavior:** `workPlaces[citizenClass] += count` (int[7] at `+0x304`) [obs]. The optional third arg **sets** (does not add) `workVisitType[citizenClass]` (int[7] at `+0x320`, constructor default -1) [obs]. If the total work places exceed 19, it sets the landmark flag `+0x370` [obs].
- **Details:**
  - `workVisitType` drives utility trips. When a worker of this class picks a WORK trip and the value is not -1, then with probability `citizens.utilProbability[workVisitType]` an extra trip of that visit type is queued. Examples: lorry drivers make `GRID_VISIT_HEAVY_INDUSTRIAL` trips, police make `EMERGENCY_POLICE` trips [obs in `FUN_100cbdf0`].
- **Evidence:** `0x100e8470.c:16-26`. Callees read: `0x100cbdf0.c:20-50` (reader), `0x100f04e0.c` (ctor sets `+0x320[i] = -1`).
- **Confidence:** high.

## gsGridObject.addPath
- **Signature:** `addPath(name: string, flags: int, startPos: vec3, startRot: quat, endPos: vec3, endRot: quat) -> unset`
- **Behavior:** Creates a 0x3c-byte path record `{u32 flags; vec3 p0; quat q0; vec3 p1; quat q1}` and stores it in the template's path map (`+0x29c`) under `name`. Any existing record with that name is freed and replaced [obs]. Unlike `setPathFlags`, the flags are stored exactly as given [obs].
- **Details:**
  - Arguments that are not a vector/quaternion fall back to `VEC_ZERO` / `QUAT_IDENT` [obs].
  - The positions are in object-local space; scripts use them for door enter/exit paths (`GRID_PATH_START`/`END`) [inf].
- **Evidence:** `0x100f0340.c:29-34` (lookup/free), `35-46` (alloc), `48-67` (fill), `68-69` (map insert). Callees `0x100932e0.c`, `0x10093350.c`.
- **Confidence:** high.

## gsGridObject.setTextureVariation
- **Signature:** `setTextureVariation(index: int, material: gsMaterialVariant | null) -> unset`
- **Behavior:** Replaces material slot `index` (pointer array based at `+0x3c0`, so index 1 is `+0x3c4`) with `material`. The previous material is released; the new one is addRef'd through the vtable `+0x14` resolve [obs]. A null or non-material argument stores 0 [obs].
- **Details:**
  - Valid indices are 1–4 (`+0x3c4..0x3d0`, all zeroed by the constructor) [inf from layout]. **Index 0 aliases the `addVisitPlaces` open-days word at `+0x3c0`**, and the handler would call release on that value [obs]. Higher indices run into `alternateId` [inf].
  - The variation is selected per item by its `variation` field and `GRIDMODE_SET_VARIATION` [inf].
- **Evidence:** `0x100e96b0.c:15-31`; constructor `0x100f04e0.c` (zeroes `+0x3c4..0x3d0`).
- **Confidence:** high for the mechanics; the index range is inferred.

## gsGridCitizens.getCharacter
- **Signature:** `getCharacter(citizenClass: int, [gender: int]) -> gsGridObject | null`
- **Behavior:** Picks, uniformly at random, a character model registered with `addCharacter` for this class whose era mask overlaps `citizens.modelMask` (`+0xa28`). When `gender` is given (and not -1), the model's gender value must equal it [obs]. Returns the template addRef'd, or `null` if nothing matches [obs].
- **Details:**
  - The pool is the vector at `this + 0xf8 + class*16` of `{gsGridObject*, eraMask, gender}` records [obs].
  - There is no class bounds check [obs]. Consumes `rand()` [obs].
- **Evidence:** `0x100ce7f0.c:20-24` (args/pool), `28-33` (count matches), `35-50` (random pick, addRef tail-call), `54` (null).
- **Confidence:** high. The 0/1 "gender" meaning comes from script ids such as `era1-blue-collar-male` / `-female` [inf].

## gsGridCitizens.addCar
- **Signature:** `addCar(visitType: int, citizenClass: int, eraMask: int, weight: int, objId: string, [trailerId: string, ...]) -> unset`
- **Behavior:** Builds a vehicle record `{gsGridObject* parts[argc-4], partCount, weight, eraMask}` from the template ids (looked up in the grid's id map at `grid+0x1ec`, with the grid at `this+0xd4`). It appends the record to the pool at `this + 0x168 + (visitType*7 + class)*16` [obs].
- **Details:**
  - Unknown id: the record is freed and script error **code 0xf** is thrown [obs]. Code 0xf is not in the known list; it probably means "object not found" [inf].
  - Selection (`FUN_100ce250`) is a weighted random pick by `weight` among records whose `eraMask & modelMask` is non-zero [obs]. For `GRID_VISIT_GENERIC` trips, a car is used only with probability `carMultiplier` (`+0xa68`) [obs].
  - Multiple ids form one articulated vehicle (cab + trailers) [inf].
  - Templates are stored without addRef [obs]. There are no bounds checks on visitType or class [obs].
- **Evidence:** `0x100d2630.c:37-55` (record), `56-97` (id lookup, throw at 91), `100-115` (pool push). Callee `0x100ce250.c` (reader).
- **Confidence:** high.

## gsGridCitizens.addCharacter
- **Signature:** `addCharacter(citizenClass: int, eraMask: int, gender: int, objId: string) -> unset`
- **Behavior:** Looks up `objId` in the grid's template map. If found, appends `{template, eraMask, gender}` to the class's character pool (`this + 0xf8 + class*16`); `getCharacter` reads that pool [obs].
- **Details:**
  - An unknown `objId` is **silently ignored** (contrast `addCar`, which throws) [obs].
  - There is no addRef on the template [obs].
  - The 2-argument calls in `game.script` are inside a comment block [obs in script].
- **Evidence:** `0x100d2530.c:30-38` (lookup), `39-55` (record, push via `FUN_100d18a0`).
- **Confidence:** high.

## gsGridCitizens.setPriceSignificance
- **Signature:** `setPriceSignificance(citizenClass: int, value: float) -> unset`
- **Behavior:** Sets `priceSignificance[class]` (float[7] at `+0x938`) [obs]. Citizen route evaluation multiplies ticket-price terms by it (read in `FUN_100d18f0`/`FUN_100cd5c0`) [obs/inf]. Scripts use 0.03 (business) to 0.20 (unemployed) × a difficulty multiplier.
- **Details:**
  - There is no bounds check [obs].
- **Evidence:** `0x100cc4b0.c:14-19`; readers found by grep: `0x100d18f0.c:201,243,262`, `0x100cd5c0.c:55`.
- **Confidence:** high for the store; medium for the exact use in the price formula.

## gsGridCitizens.setReasonProbability
- **Signature:** `setReasonProbability(citizenClass: int, work: int, shopping: int, leisure: int, government: int) -> unset`
- **Behavior:** Stores the four ints (percentages) at `+0x8c8 + class*16` [obs].
- **Details:**
  - Trip-reason roll (`FUN_100cbd50`): employed citizens roll `r = rand()%100`; citizens without a workplace roll `r ∈ [work, 100)` [obs]. Then `r < work` → WORK (2), `< work+shopping` → SHOPPING (3), `< work+shopping+leisure` → LEISURE (4), else GOVERNMENT (5) [obs].
  - The fourth value is added only after the last band is already decided, so it never changes that roll [obs]. With the shipped values (e.g. 55/20/35/4) the sum passes 100 and GOVERNMENT is never picked [obs/inf].
  - `work != 0` also gates workplace assignment for the class (`FUN_100ce420`) [obs].
- **Evidence:** `0x100cc470.c:14-25`; readers `0x100cbd50.c:8-44`, `0x100ce420.c:23`.
- **Confidence:** high. Other readers of the fourth value were not searched exhaustively.

## gsGridCitizens.setUtilProbability
- **Signature:** `setUtilProbability(visitType: int, probability: float) -> unset`
- **Behavior:** Sets `utilProbability[visitType]` (float[13] at `+0x954`) [obs]. It is the chance in [0,1] that a worker whose workplace has `workVisitType == visitType` (from `addWorkPlaces`' third arg) adds a utility trip of that type after a WORK trip [obs].
- **Details:**
  - There is no bounds check [obs].
- **Evidence:** `0x100cc4e0.c:14-19`; reader `0x100cbdf0.c:36-45`.
- **Confidence:** high.

## gsGridCitizens.getCanTravelCount
- **Signature:** `getCanTravelCount(citizenClass: int) -> int`
- **Behavior:** Returns `canTravel[class]` (int[7] at `+0x9e4`), or the sum over all 7 classes when `class == -1` [obs]. The counter goes up in route decision (`FUN_100d18f0`) when a public-transport option existed [inf]. Scripts compute service coverage as `canTravel / (travel + drive + walk)` and usage as `travel / canTravel`.
- **Details:**
  - Other negative or out-of-range classes read out of bounds [obs].
- **Evidence:** `0x100cdce0.c:13-21`; incrementer `0x100d18f0.c:282,314-315`.
- **Confidence:** high for the read; the counter's exact meaning is inferred.

## gsGridCitizens.getPriceTooHighCount
- **Signature:** `getPriceTooHighCount(citizenClass: int) -> int`
- **Behavior:** Returns `priceTooHigh[class]` (int[7] at `+0xa00`), or the sum over all classes for `-1` [obs]. These are probably trips where public transport was possible but rejected on price [inf]. Scripts warn when it exceeds 20% of `getCanTravelCount(-1)`.
- **Evidence:** `0x100cdd40.c:13-21`.
- **Confidence:** high for the read; the meaning is inferred from the name and script use.

## gsGridCitizens.resetTripCounts
- **Signature:** `resetTripCounts() -> unset`
- **Behavior:** Zeroes all five per-class trip statistics arrays (7 ints each): walk `+0x990`, drive `+0x9ac`, travel `+0x9c8`, canTravel `+0x9e4`, priceTooHigh `+0xa00` [obs]. The array names come from the sibling getters `getWalkCount`/`getDriveCount`/`getTravelCount` (offsets checked) [obs].
- **Evidence:** `0x100cc510.c:11-46`; getters `0x100cdbc0.c`, `0x100cdc20.c`, `0x100cdc80.c` (offset grep only).
- **Confidence:** high.

## gsGridTimeline.applyChanges
- **Signature:** `applyChanges(fromDay: int, toDay: int, lookaheadDays: int, [ignored]) -> vec3 | unset`
- **Behavior:** Replays the timeline's recorded actions (per-day linked lists at `+0xd8`, day count `+0xd0`) for days `fromDay .. min(toDay, dayCount-1)` onto the grid (`+0xcc`).
  - Each action is called as `action->vfunc[1](grid, mode, &weightSum, &posSum, 1)`, in reverse list order [obs].
  - For each day `d > 0`, the actions of day `d + lookahead` are first applied in **preview mode** (mode 1) [obs]. On `d == 1` this covers every day from 1 to `1 + lookahead`, which fills the window [obs]. Then day `d` itself is applied for real (mode 0) [obs].
  - Returns `posSum / weightSum` as a new vec3 if any action contributed weight, otherwise unset [obs]. `game.script` uses `defined $result` to react to changes.
- **Details:**
  - The grid's deferred-rebuild flag (`grid+0x241`) is set before the first action. Afterwards `FUN_100b5570` clears it and rebuilds every slot marked dirty (`slot+8 == -1`) [obs].
  - The 4th argument (scripts pass `true`) is never read [obs]. `currentDay` (`+0xdc`) is not updated; scripts set it themselves [obs].
  - `fromDay` is not range-checked: a negative value would index before the array [obs]. `fromDay > toDay` → no-op, returns unset [obs].
  - Mode 1 probably marks future construction or announcements; scripts always pass 182, about six months [inf].
- **Evidence:** `0x10105560.c:45-52` (args/clamp), `58-114` (lookahead/preview loop, `d==1` case at 67, mode-1 call 105), `116-163` (day apply, mode-0 call 152), `166-168` (end batch), `169-180` (average position via `FUN_100928d0`), `184`. Callees `0x100b5560.c`, `0x100b5570.c`, `0x100928d0.c`.
- **Confidence:** medium-high. The action classes' `vfunc[1]` implementations, and so what preview mode actually does, were not read. The initial `posSum` (`DAT_10198890`) is assumed to be zero.
