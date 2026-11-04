# Transport lines (0x0706) and city timeline (0x0708)

Decoded from the Ghidra pseudocode of GSystem.dll (`decompiled/GSystem-v2/functions/0x<addr>.c`) and checked against every sample under `samples/` with a read-only parser (`…/scratchpad/save-lines/parse.mjs`, see §4).

Confidence tags: **obs** = read directly from writer/loader pseudocode or seen in sample bytes; **inf** = inferred (naming, purpose, or only partly visible). All values are little-endian.

---

## 1. Overview

### 1.1 Transport lines: chunk `0x07060000` inside `map.grid`

The player's public-transport network: the lines, the stops each line serves, and the vehicles running on it. It is a child of the grid root `0x07000000`. It comes after `0x07050000` and before `0x07070000` (citizens) (`0x100c3840.c:365`, loader dispatch `0x100c3310.c:142`). Writer `FUN_1011a9f0`, loader `FUN_1011b110`; `this` is the `gsTransportLines` module (`grid+0x218`).

```
0x07060000  TransportLines           16-byte header {modelCount, lineCount, vehicleCount, stopCount}
  0x07060100  VehicleModelNames      modelCount NUL-terminated names, zero-padded to 4
  0x07060200  TransportLine          × lineCount: 36-byte header, then children:
    0x07060220  LineStop             × line.stopCount  (all stops first, in route order)
    0x07060210  LineVehicle          × line.vehicleCount (60 bytes + 4 × unitCount)
```

Cross-references (all **1-based, 0 = none**, made by `FUN_100ba930` = map lookup + 1 when writing and `FUN_100b58a0(arr,n,i)` = `arr[i-1]` if `1 ≤ i ≤ n` when reading):

| Field | Points to | Evidence |
|---|---|---|
| `LineStop.networkItem` | the n-th 16-byte network item record in `0x07020210`, in file order (the stop/station building) | writer `0x1011a9f0.c:202` (`param_2` = item map `local_c4`, also used for item parent indices in `0x100c3840.c:270-275`, passed at `:365`); loader `0x1011b110.c:283` (`param_3/4` = network item array from `0x07020000`, `0x100c3310.c:131,143`) |
| `LineVehicle.gridVehicle` | the n-th `0x07040200` grid-vehicle record (the moving entity on the map), in file order | writer `0x1011a9f0.c:215` (`param_3` = map filled by `FUN_1010f8a0` in `0x07040000` order, `0x1010f8a0.c:96-97`); loader `0x1011b110.c:228-231` (array from `0x07040000`, `0x100c3310.c:137-140`) |
| `LineVehicle.stopA/stopB/dockedAt` | 1-based index into **this line's** stop list (the `0x07060220` children of the same `0x07060200`) | writer `0x1011a9f0.c:216-234`; loader `0x1011b110.c:217-227` |
| `VehicleUnit.modelIndex` | **0-based** index into `0x07060100` | writer `0x1011a9f0.c:262-263` (map value = 0-based write order, `:121`); loader `0x1011b110.c:204-205,256-257` |
| *(outgoing)* global vehicle / stop arrays | the loader builds arrays of all vehicles (`vehicleCount`) and all stops (`stopCount`) in file order. It passes them to the `0x07070000` (citizens) loader, so citizens refer to line vehicles and line stops by global file-order index | `0x1011b110.c:59-74, 276, 289`; `0x100c3310.c:143-150` |

Not stored in this chunk (scripts or recomputation supply them): the line `color` (`+0x188`, the script sets it to the company colour, `linepanel.script:772`), the `active` bit, the line name and the vehicle names (in `gamedata.script` `$lines`, same order as the `0x07060200` chunks), line `attractiveness` (`+0x170`), and vehicle `condition` (`+0x180`). Vehicle `capacity` (`+0x16c`) is written but the loader ignores it and recomputes it from the unit models (`0x1011b110.c:270`).

### 1.2 City timeline: `<map>.timeline` / `map.timeline` (root `0x07080000`)

This is the city's **scripted history from 1920 on**. The map editor records every edit (placing, deleting, renaming or re-varying objects, laying roads and tracks, terrain brushes, water) under the in-game day it should happen. During play the game replays those edits day by day, so the city grows from its 1920 state (the shipped `<map>.grid`) to roughly the present (2008–2020, depending on the map).

* **Day index:** days since **1 Jan 1920**, as a u32. Scripts compute `currentDay = date − mkdate(1,1,1920)` (`game.script:9,672-673`, `level.script:36-38`, `editor.script:286`, `timebar.script:33,110-111`). This is *not* the absolute day number (days since year 0) used elsewhere. In the samples the last group lands exactly on 1 Jan 2013/2017/2020 or on 31 Dec 2008/2020, and 552 of the 712 day groups in the six base maps fall on the 1st of a month. That confirms the 1920 epoch.
* **Replay:** `gsGridTimeline.applyChanges(currentDay+1, today, 182)` runs every in-game day (`game.script:671-673`). Each day's actions are applied for real, and the actions 182 days ahead are applied in "preview" mode (mode 1: e.g. scaffolding/markers; createObject ORs `0x500000` into the new item's flags, deleteObject only sets `0x20000000` on the target). See `batch-A.md` §gsGridTimeline.applyChanges.
* **Apply order:** a day's action list, and a group's child list, are applied **in reverse file order**: `FUN_10105460` builds a vector and iterates from the back, and `applyChanges` does the same. The editor stores newest-first. Sample data confirms this: in berlin.timeline, 2 427 `createObject+parent` records find their parent's creation *later* in the same day's list, against 25 earlier (§4.3).
* **Saves:** a save's `map.timeline` is a copy of the map's timeline with `currentDay` = the save date (e.g. saves/berlin: day 4729 = 1932-12-12). The editor writes timelines with `currentDay = 0` (`main.script:273-274`); scenario maps carry their scenario start day. A free-play/sandbox save (Cim2) has an empty timeline: `dayCount 0`, `currentDay 33019` (2010-05-27).

```
0x07080000  Timeline        {u32 dayCount, u32 currentDay}
  0x07081000  Day           × (days that have actions, ascending): {u32 day, u32 0}, then action chunks
    0x07081010  group         32-byte label + child action chunks   (gsGridTimelineAction_group)
    0x07081020  createObject  ObjRef                                 (gsGridTimelineAction_createObject)
    0x07081030  createObject  ObjRef + parent ObjRef                 (same class, has-parent variant)
    0x07081040  deleteObject  ObjRef                                 (gsGridTimelineAction_deleteObject)
    0x07081050  brush         8-byte header + n × {i16 x, i16 z}     (gsGridTimelineAction_brush)
    0x07081060  water         8 bytes                                (gsGridTimelineAction_water)
    0x07081070  name (v0)     ObjRef + 8-bit (Latin-1) string        (gsGridTimelineAction_name)
    0x07081071  name (v1)     ObjRef + UTF-8 string                  (gsGridTimelineAction_name)
    0x07081080  variation     ObjRef + u32                           (gsGridTimelineAction_variation)
```

Objects are referred to **by value, not by index**. An `ObjRef` holds the template name, position and orientation, and the apply code finds the live item by searching the grid slot at (x, z). This lets the timeline survive edits to the base grid.

**Tag → class map** (vtable slot 3 = write, slot 4 = read, slot 1 = apply, slot 2 = label; vtables from `docs/rtti-classes.json`, constructors store them):

| Tag(s) | Class | vtable | ctor (stores vtable) | write / read / apply / label | obj size |
|---|---|---|---|---|---|
| 0x07081010 | gsGridTimelineAction_group | 0x101675d0 | 0x101029d0 | 0x10100db0 / 0x10103d30 / 0x10105460 / 0x10103cc0 (returns its own label) | 0x2c |
| 0x07081020, 0x07081030 | gsGridTimelineAction_createObject | 0x10167748 | 0x101027b0 | 0x10100bf0 / 0x10100c50 / 0x10105a50 / 0x10103ad0 (`%!11`) | 0x60 |
| 0x07081040 | gsGridTimelineAction_deleteObject | 0x1016776c | 0x10102900 | 0x10100ca0 / 0x10100ce0 / 0x10103b30 / 0x10103c60 (`%!12`) | 0x34 |
| 0x07081050 | gsGridTimelineAction_brush | 0x101675f4 | 0x10100e00 | 0x10100ed0 / 0x10100f20 / 0x10101800 / 0x10103df0 (`%!13`…`%!20`, default `%!21`) | 0x18 |
| 0x07081060 | gsGridTimelineAction_water | 0x10167618 | 0x10100fb0 | 0x101010e0 / 0x10101120 / 0x10101020 / 0x10103f40 (`%!22` on, `%!23` off) | 0x10 |
| 0x07081070, 0x07081071 | gsGridTimelineAction_name | 0x1016763c | 0x10102a50 | 0x101011b0 / 0x10101200 / 0x10103fc0 / 0x10104180 (`%!24`) | 0x38 |
| 0x07081080 | gsGridTimelineAction_variation | 0x10167790 | 0x10102b50 | 0x101012b0 / 0x101012f0 / 0x101041e0 / 0x10104350 (`%!25`) | 0x38 |
| — | gsGridTimelineAction (abstract base) | 0x101675ac | — | pure-virtual stubs 0x10120b90 | — |

Factory: `FUN_101036f0(tag)` (`0x101036f0.c:26-136`). Unknown tags return null and the chunk is skipped. The writer only ever emits 0x07081071 for names; 0x07081070 is the older 8-bit format and is still read (found in the scenario maps).

---

## 2. Chunk layouts

Offsets are relative to the chunk body (after the 8-byte `tag,size` header).

### 2.1 `0x07060000` TransportLines header (16 bytes)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 4 | u32 | modelCount | number of distinct vehicle unit models used by all line vehicles (= names in 0x07060100) | `0x1011a9f0.c:100` (`local_ac` = size of model map), loader `0x1011b110.c:58` | high (obs) |
| 4 | 4 | u32 | lineCount | number of 0x07060200 chunks (incremented per `gsTransportLine` child) | `0x1011a9f0.c:102,278`; loader ignores it | high (obs) |
| 8 | 4 | u32 | vehicleCount | total vehicles across all lines; sizes the global vehicle array | `0x1011a9f0.c:101` (`param_5` map size), loader `:59,62-67` | high (obs) |
| 12 | 4 | u32 | stopCount | total line-stop nodes across all lines; sizes the global stop array | `0x1011a9f0.c:103` (`param_6` map size), loader `:60,68-74` | high (obs) |

Children: one 0x07060100, then lineCount × 0x07060200. The writer always emits 0x07060100, even when it is empty (base maps: 0/0/0/0).

### 2.2 `0x07060100` VehicleModelNames

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | var | cstr × modelCount | names | vehicle model (template) names, e.g. `tram-05`, `tram-05b`, `metro-01`; written from `model+0x2b0` (std::string), each NUL-terminated | `0x1011a9f0.c:105-130` | high (obs) |
| end | 0–3 | bytes | pad | zero padding to a 4-byte boundary | `0x1011a9f0.c:156-160` | high (obs) |

Names are written in model-map order (ordered by pointer value, so effectively arbitrary). The index is the 0-based position. The loader resolves each name in the vehicle-model registry (`ctx+0xd4 → +0x1ec`). An unknown name maps to null (`0x1011b110.c:104-172`).

### 2.3 `0x07060200` TransportLine (36-byte header + children)

One per `gsTransportLine` child of the line manager, in module-child order. On load a new `gsTransportLine` (400 bytes, ctor `FUN_10118830`) is created.

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 4 | f32 | headway | line `+0x168`, restored. Samples: 1.7–360. Recomputed every line tick as `Σ(segment travelCost + 5) / vehicleCount` seconds; drives dwell time (see `../simulation/transport.md` §2) | w `:181`, r `:189`; `0x10114f30.c:18-26,61-62` | high (obs) |
| 4 | 4 | u32 | type | `TRANSPORTLINE_*`: 0 bus, 1 tram, 2 metro, 3 boat, 4 helicopter (`+0x16c`, script field `type`) | w `:179`, r `:190`; samples 0–4 | high (obs) |
| 8 | 4 | f32 | effAttractiveness | line `+0x174`, restored; 0.07–0.68 in samples. Recomputed every line tick: `attractiveness × mean(vehicle.attractiveness) / (1 + 0.01·avgWait)`; the route search uses it as a per-line cost factor | w `:180`, r `:191`; `0x10114f30.c:62-66` | high (obs) |
| 12 | 4 | u32 | vehicleCount | number of 0x07060210 children (`+0x15c..0x160` vector size) | w `:182-187`; loader ignores it | high (obs) |
| 16 | 4 | u32 | stopCount | number of 0x07060220 children (`+0x178`, script `stopCount`); the loader uses it to size the line-local stop table | w `:189`, r `:195,288` | high (obs) |
| 20 | 4 | f32 | avgWait | line `+0x180`, restored; 0–264 in samples. Moving average of boarding passengers' wait ticks: `v = 0.95·v + 0.05·wait` | w `:190`, r `:192`; `0x10110be0.c:11` | high (obs) |
| 24 | 4 | f32 | ticketPrice | `+0x184` (script `ticketPrice`); samples 270–1150 | w `:188`, r `:193` | high (obs) |
| 28 | 4 | i32 | passengersTransported | `+0x18c` (script `passengersTransported`) | w `:191`, r `:194` | high (obs) |
| 32 | 1 | u8 | isClosed | `+0x17c`: the route is a closed loop. On load, if set, the first stop's item is added again, which closes the loop (no extra stop record is stored) | w `:192`, r `:200-201,297-298`; `FUN_10119660` | high (obs) |
| 33 | 3 | bytes[3] | pad | always 0 | w `:193-195` | high (obs) |

Children: all 0x07060220 stops first (the writer walks the circular stop list from `+0x148`), then all 0x07060210 vehicles (vector `+0x15c`).

### 2.4 `0x07060220` LineStop (20 bytes)

A node of the line's circular stop list: a `(stop, line)` membership (node size 0x70, ctor `FUN_10118760`).

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 4 | u32 | networkItem | 1-based index of the stop's grid item in 0x07020210 (file order); 0 = none. In all samples it points at `stop_*`, `station_*`, `dock-platform` or `heliport` items | w `:202`, r `:283-284` | high (obs) |
| 4 | 4 | u32 | flags | node `+0x30`; bit 0 = temporary stop (`hasTempStops` tests it). **Written but not restored**: the loader re-adds the stop with temp = false | w `:201`; ctor `0x10118760.c` (`+0x30 = param_3 != 0`); `0x10110b80.c` | high (obs) |
| 8 | 4 | f32 | sinceDeparture | node `+0x34`, restored; 0–307 in samples. Seconds since a vehicle last left this stop (advanced by the line tick); used in the dwell-time formula | w `:203`, r `:285`; `0x10114f30.c:22`, `0x101152a0.c:316` | high (obs) |
| 12 | 4 | i32 | passengersTransported | node `+0x68`, returned by `gsTransportLine.getPassengersTransported(stopIndex)` | w `:205`, r `:286`; `0x10113750.c` | high (obs) |
| 16 | 4 | i32 | waitingTime | node `+0x6c`, returned by `gsTransportLine.getWaitingTime(stopIndex)`; probably accumulated | w `:204`, r `:287`; `0x101137a0.c` | high layout; medium meaning |

### 2.5 `0x07060210` LineVehicle (60 bytes + 4 × unitCount)

A `gsTransportVehicle` (0x1a0 bytes, ctor `FUN_10118d50(ctx, line, firstModel, …)`). Writer block `0x1011a9f0.c:213-266`, loader `0x1011b110.c:199-279`.

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 4 | u32 | gridVehicle | 1-based index of the moving entity in 0x07040200 (`veh+0x13c`; its `+0x2c` points back). 0 = not spawned (scenario04 depot vehicles) | w `:215`, r `:228-231` | high (obs) |
| 4 | 4 | u32 | stopA | 1-based line-local stop index of `veh+0x154` (stop just left / current); the loader also sets `veh+0x15c = stop+0x14` | w `:224-226`, r `:217-221` | high (obs) |
| 8 | 4 | u32 | stopB | 1-based line-local stop index of `veh+0x158` (next stop); in samples = stopA+1, wrapping to 1 | w `:227-229`, r `:222-224` | high (obs) |
| 12 | 4 | u32 | dockedAt | 1-based index of the stop whose `+0x50` (docked vehicle) is this vehicle; 0 = not docked | w `:230-232`, r `:225-227` | high (obs) |
| 16 | 4 | f32 | stopTime | `+0x160` (script `stopTime`); −1, 0 or seconds | w `:236`, r `:233` | high (obs) |
| 20 | 4 | f32 | respawnRepathTimer | `+0x164`, restored; 0.02–0.25. While unspawned, the vehicle re-paths every > 1 s | w `:237`, r `:234`; `0x101152a0.c:64-78` | medium (obs, see `../simulation/transport.md` §2) |
| 24 | 4 | i32 | nextLegSteps | `+0x168`, restored; 0–104. Path-step count of the pre-loaded next leg, applied at departure | w `:238`, r `:235`; `0x101152a0.c:216,348` | medium (obs, see `../simulation/transport.md` §2) |
| 28 | 4 | i32 | capacity | `+0x16c` (script `capacity`). **Ignored on load**: recomputed as Σ model `+0x298` | w `:239`, r `:270` | high (obs) |
| 32 | 4 | i32 | passengerCount | `+0x170` (script `passengerCount`) | w `:241`, r `:236` | high (obs) |
| 36 | 4 | i32 | boardingReserved | `+0x174`, restored; small ints, nonzero only while docked. Citizens who committed to board and are walking to the doors; boarding needs `capacity > boardingReserved + passengerCount` | w `:242`, r `:237`; `0x100cc990.c:239-240,368-369,419` | high (obs) |
| 40 | 4 | f32 | attractiveness | `+0x178` (script `attractiveness`) | w `:240`, r `:238` | high (obs) |
| 44 | 4 | f32 | travelDistance | `+0x17c` (script `travelDistance`) | w `:243`, r `:239` | high (obs) |
| 48 | 4 | u32 | unitCount | number of 4-byte unit entries that follow (vector `+0x194`, 8-byte elements) | w `:244-249`, r `:254` | high (obs) |
| 52 | 1 | u8 | bellRung | bool `+0x184`: vehicle bell played (fewer than 8 path nodes left to the stop) | w `:250`, r `:240`; `0x101152a0.c:243-247` | high (obs) |
| 53 | 1 | u8 | announced | bool `+0x185`: the stop's bell/announcement played (fewer than 32 path nodes left) | w `:251`, r `:241`; `0x101152a0.c:221-242` | high (obs) |
| 54 | 1 | u8 | atStopB | derived: `veh+0x150 == stop(+0x158)+0x10`. On load `+0x150 = (atStopB ? stopB : stopA)->+0x10` (the current path position belongs to that stop's segment) | w `:252-255`, r `:244-252` | high (obs) |
| 55 | 1 | u8 | broken | `+0x186` (script `broken`) | w `:257`, r `:242` | high (obs) |
| 56 | 4 | i32 | passengersTransported | `+0x188` (script `passengersTransported`) | w `:256`, r `:243` | high (obs) |
| 60 | 4×n | VehicleUnit[] | units | see below. The loader reads unit 0 separately (its model goes to the constructor) and appends units 1…n−1 | w `:259-265`, r `:204-205,254-274` | high (obs) |

**VehicleUnit (4 bytes):** `u16 modelIndex` (0-based into 0x07060100) · `u8 reversed` (bool, the second element of the 8-byte vector entry; 1 on the trailing car of metro sets) · `u8 pad` (0). Writer `CONCAT12((char)entry[1], modelIdx)` at `0x1011a9f0.c:263`; loader `uStack_40 = (char)puVar11[1] != 0` at `0x1011b110.c:258`. The flag of unit 0 is not read back; it is 0 in every sample. Confidence high (obs).

The vehicle's `condition` (`+0x180`) is **not saved** (the writer jumps from `+0x17c` to the unit count).

### 2.6 `0x07080000` Timeline root (8-byte header)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 4 | u32 | dayCount | `this+0xd0`: length of the per-day list array = last day with actions + 1. **The loader ignores it** and recomputes it from the groups | w `0x10101650.c:32`; r `0x101043b0.c:25-69` | high (obs) |
| 4 | 4 | u32 | currentDay | `this+0xdc` (script `currentDay`): last day already applied, as days since 1920-01-01 | w `:33`, r `0x101043b0.c:26` | high (obs) |

Children: 0x07081000 per non-empty day, in ascending day order (`0x10101650.c:35-49`). The loader (`0x10104c90.c:34`) requires the file's first chunk to be 0x07080000.

### 2.7 `0x07081000` Day (8-byte header + actions)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 4 | u32 | day | day index since 1920-01-01 (0-based; day 0 = 1 Jan 1920) | w `0x10101650.c:39-41`, r `0x101043b0.c:31-32`; scripts (§1.2) | high (obs) |
| 4 | 4 | u32 | reserved | always written 0, not read | w `:40` | high (obs) |

Children: action chunks, in the in-memory list order (newest first). The loader appends them in file order (`0x101043b0.c:71-79`). A day that occurs twice would overwrite the list head; writers never produce that.

### 2.8 ObjRef: the 44-byte object reference shared by actions 0x07081020/30/40/70/71/80

Filled by the action constructors from a live grid item (`0x101027b0.c:40-57`, `0x10102900.c`, `0x10102a50.c`, `0x10102b50.c`). Matching on apply: the grid slot list at (x, z) is searched (`FUN_100b6ba0(grid,x,z,8,8)`), comparing template, x, z, orient and (for name/variation) y and `flags & 0x2000000`.

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 32 | bytes[32] (cstr) | name | object template name (`template+0x2b0` std::string), NUL-terminated, rest zero | `0x101027b0.c:40-50` | high (obs) |
| 32 | 2 | i16 | x | cell x (`item+0x2c`) | `0x101027b0.c:53` | high (obs) |
| 34 | 2 | i16 | y | height in 1/256 cell (`item+0x30`); 512 = ground level of the default terrain | `:54`; samples −2048…6418 | high (obs) |
| 36 | 2 | i16 | z | cell z (`item+0x34`) | `:55` | high (obs) |
| 38 | 2 | u16 | orient | rotation 0–3 (`item+0x44`, same as the network item `orient<<14`) | `:56`; samples 0–3 only | high (obs) |
| 40 | 4 | u32 | flags | `item+0x48 & 0x42000000` (createObject main ref) or `& 0x02000000` (all other refs). 0x02000000 marks auto-generated support objects (`ramp_elevated_pillar*`, `bridge_*_pillar`, animation helpers); 0x40000000 appears on free-placed details (scaffold, wall signs, rooftop items) | `:57,73`; `0x10102900.c` | layout high (obs); flag meanings inf |

### 2.9 `0x07081010` group (32 bytes + child actions)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 32 | bytes[32] (cstr) | label | undo/timebar label, copied with `strncpy(…,0x20)` (not terminated if it is 32 chars long). Format `%N` or `%!NN` (two digits) plus an optional object name, e.g. `%9 tree3` | w `0x10100db0.c:16`, r `0x10103d30.c:29-34`, ctor `0x101029d0.c` | high (obs) |
| 32 | var | chunk[] | actions | child actions (any action tag; the reader is recursive). Applied in **reverse** order | w `0x10100db0.c:17-19`, r `0x10103d30.c:35-46`, apply `0x10105460.c` | high (obs) |

The editor's begin-group call (`FUN_10102c60`) increments a nesting counter instead of nesting groups, so samples have nesting depth 1 only. `%N` is the index into the 27 titles passed to `expandPattern` in `timebar.script:188-215` (inf: argument N ↔ `%N`):

`%0` remove · `%1` growTrees · `%2` renameObjects · `%3` layTrack · `%4` layBridge · `%5` layTunnel · `%6` layTram · `%7` layAlley · `%8` layRoad · `%9` place (+name) · `%10` placeStop · `%11` createObject · `%12` deleteObject · `%13` setYard · `%14` unsetYard · `%15` setField · `%16` unsetField · `%17` levelTerrain · `%18` shiftTerrain · `%19` softenTerrain · `%20` ruinTerrain · `%21` brush · `%22` waterOn · `%23` waterOff · `%24` renameObject · `%25` setVariation · `%26` setVariations.

Editor sources: tool modes 6/0xb/0xc/0x11/0x12/other → `%3/%4/%5/%6/%7/%8`; mode 7 → `"%9 " + name`; mode 10 → `%!10` (`0x100fd820.c:121-156,269-280`); `"%0 " + name` (`0x100fa330.c:122`); `%2` (`0x100fd820.c:655`). The label strings are at `0x101677b0…0x10167820` (`gsystem-strings.txt:14000-14014`).

### 2.10 `0x07081020` createObject (44 bytes) and `0x07081030` createObject with parent (88 bytes)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 44 | ObjRef | object | object to create; flags mask 0x42000000 | w `0x10100bf0.c:15`, r `0x10100c50.c:15-21`, ctor `0x101027b0.c:40-57` | high (obs) |
| 44 | 44 | ObjRef | parent | **0x07081030 only**: the object it attaches to (`item+0x1c`, the same parent link as `parentIndex` in 0x07020210); flags mask 0x02000000. The tag is chosen by `parent.name[0] != 0` | w `:14,16-18`, r `:22-28`, ctor `:58-74` | high (obs) |

Apply (`0x10105a50.c`): if there is a parent, find it (template, x, z, orient), climb while the item has flag 0x200000, then `FUN_100c4380(grid, name, x, y, z, orient, flags | (preview ? 0x500000 : 0), parent)`. An unknown template logs `"Unknown object: "`. Buildings that cover cells mark them (cell flag 0x40 in preview), and other overlapping items are resolved.

### 2.11 `0x07081040` deleteObject (44 bytes)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 44 | ObjRef | object | item to delete; matched on template + x + z + orient (**y is ignored**). Preview mode only sets `0x20000000` on the item | w `0x10100ca0.c:14`, r `0x10100ce0.c:15`, apply `0x10103b30.c:46-63` | high (obs) |

### 2.12 `0x07081050` brush (8 bytes + 4 × count)

Terrain and ground-paint brush stroke. Consecutive dabs with the same type, radius and strength are merged into one action (`FUN_10102480`).

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 1 | u8 (signed char) | brushType | see the table below | w `0x10100ed0.c:14`, r `0x10100f20.c:22-25`, ctor `0x10100e00.c` | high (obs) |
| 1 | 1 | i8 | radius | radius in cells (circle test `dx²+dz² ≤ r²`); samples 0–25 | apply `0x10101800.c` | high (obs) |
| 2 | 2 | u16 | count | number of points (`this+10`; capped below 0xffff by the merger) | w `:15-16`, r `:24-35` | high (obs) |
| 4 | 2 | i16 | strength | type 19: target height (1/256 cell; samples −1024…1280); type 20: height delta; type 22: wear amount; 0 for paint types | apply `0x10101800.c` | high (obs) |
| 6 | 2 | u16 | reserved | ctor writes 0; always 0 in samples | `0x10100e00.c` | high (obs) |
| 8 | 4×count | {i16 x, i16 z}[] | points | brush centres in cells (one sample point is (−1,−1); out-of-map cells are skipped at apply) | w `:16`, r `:31-35` | high (obs) |

| brushType | label | effect on the terrain cell (`i16 height, u16 flags`, chunk 0x07010000) |
|---|---|---|
| 1–8 | %13 setYard | `flags = (flags & 0x98) \| (type−1) \| 0x08` (8 yard textures); preview sets 0x40 |
| 9 | %14 unsetYard | `flags &= 0xf0` |
| 10–17 | %15 setField | `flags = (flags & 0xf0) \| (type−10) \| 0x20` |
| 18 | %16 unsetField | `flags &= 0xd8` |
| 19 | %17 levelTerrain | blend height toward `strength` with a cosine falloff |
| 20 | %18 shiftTerrain | add `strength` with a cosine falloff, clamp ≥ −0x600 |
| 21 | %19 softenTerrain | 5-tap average smoothing |
| 22 | %20 ruinTerrain | change the low 3 bits (wear level) of cells without 0x08/0x28 by a randomised amount scaled by strength (uses `rand()`), clamp 0–7 |

After types 19–21 the water flag 0x2000 is cleared where all four corners are above −0x141. Evidence: `0x10101800.c:48-120` (20/22), `121-233` (21), `234-337` (1–19; 19 at `:275`, 18 at `:300`), `338-389` (water fix-up, `:355,374-375`); labels `0x10103df0.c:19-66`. Confidence high for the mapping; the exact falloff maths is hidden in x87 code (medium).

### 2.13 `0x07081060` water (8 bytes). Not present in any sample

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 2 | i16 | x | cell x | ctor `0x10100fb0.c`, apply `0x10101020.c` | high (obs) |
| 2 | 2 | u16 | reserved | ctor writes 0 | ctor | high (obs) |
| 4 | 2 | i16 | z | cell z | apply | high (obs) |
| 6 | 2 | u16 | on | ≠0 → water on (`%!22`), 0 → off (`%!23`). Applied only if one of the 4 corner heights < −0x140: `FUN_100c0440(grid,x,z,on)`; no-op in preview | `0x10101020.c`, `0x10103f40.c` | high (obs) |

### 2.14 `0x07081070` / `0x07081071` name (44 bytes + string)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 44 | ObjRef | object | item to rename (matched including y and flag 0x2000000) | w `0x101011b0.c:15`, r `0x10101200.c:18-25`, apply `0x10103fc0.c` | high (obs) |
| 44 | rest | cstr | text | the custom item name (`item+0x60`, the same strings as 0x07020310: street names, `$cinema`, …). **0x07081071**: UTF-16 code units encoded as UTF-8 (1–3 bytes; 0xFFFF as 4 bytes) by `FUN_1007d8f0`, NUL included, zero-padded to 4. **0x07081070**: one byte per character (Latin-1, zero-extended). The reader decodes all remaining bytes, so the padding becomes extra NULs. An empty string removes the custom name | w `:16-17`, r `:33-50`, codec `0x1007d8f0.c` / `0x1007be40.c` | high (obs) |

### 2.15 `0x07081080` variation (48 bytes)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 44 | ObjRef | object | target item (matched including y and flag 0x2000000) | w `0x101012b0.c:14`, r `0x101012f0.c:15-21` | high (obs) |
| 44 | 4 | u32 | variation | new `item+0x6c` (the network item record's `variation`); samples 0–3. The item's footprint is refreshed | w `:15`, r `:22-23`, apply `0x101041e0.c` | high (obs) |

---

## 3. Machine-readable layout

Conventions: offsets are relative to the chunk body. A `count` that is a string names an earlier field of the same chunk (`"parent.X"` = a field of the enclosing chunk). `"record"` is a repeated fixed-size sub-structure with its own `fields`. `"i8"` extends the requested type list. `cstr` + `"align":4` means NUL-terminated and zero-padded to a multiple of 4 counted from the start of the string block. `"rest":true` means the field runs to the end of the chunk body. `children` lists the allowed child chunk tags, in the order the writer emits them. `"status"` is `obs` or `inf`.

```json
{
  "format": "CiM GSystem chunk layouts: transport lines (0x0706) and timeline (0x0708)",
  "conventions": {
    "endianness": "little",
    "chunkHeader": [{"name": "tag", "type": "u32"}, {"name": "size", "type": "u32"}],
    "indexBase": "references marked oneBased use 1..n, 0 = null",
    "dayEpoch": "1920-01-01 (day 0)"
  },
  "structs": {
    "ObjRef": {"size": 44, "fields": [
      {"name": "name", "type": "bytes[32]", "offset": 0, "encoding": "latin1-cstr", "status": "obs"},
      {"name": "x", "type": "i16", "offset": 32, "unit": "cell", "status": "obs"},
      {"name": "y", "type": "i16", "offset": 34, "unit": "1/256 cell", "status": "obs"},
      {"name": "z", "type": "i16", "offset": 36, "unit": "cell", "status": "obs"},
      {"name": "orient", "type": "u16", "offset": 38, "status": "obs"},
      {"name": "flags", "type": "u32", "offset": 40, "note": "item flags & 0x42000000 (create) or & 0x02000000", "status": "obs"}
    ]}
  },
  "chunks": [
    {"tag": "0x07060000", "name": "TransportLines", "parent": "0x07000000", "fields": [
      {"name": "modelCount", "type": "u32", "offset": 0, "status": "obs"},
      {"name": "lineCount", "type": "u32", "offset": 4, "status": "obs"},
      {"name": "vehicleCount", "type": "u32", "offset": 8, "status": "obs"},
      {"name": "stopCount", "type": "u32", "offset": 12, "status": "obs"}
    ], "children": [{"tag": "0x07060100", "count": 1}, {"tag": "0x07060200", "count": "lineCount"}]},

    {"tag": "0x07060100", "name": "VehicleModelNames", "parent": "0x07060000", "fields": [
      {"name": "names", "type": "cstr", "offset": 0, "count": "parent.modelCount", "encoding": "latin1", "align": 4, "status": "obs"}
    ], "children": []},

    {"tag": "0x07060200", "name": "TransportLine", "parent": "0x07060000", "fields": [
      {"name": "headway", "type": "f32", "offset": 0, "status": "obs", "meaning": "line+0x168: sum(segment travelCost + 5) / vehicleCount, seconds"},
      {"name": "type", "type": "u32", "offset": 4, "enum": {"0": "bus", "1": "tram", "2": "metro", "3": "boat", "4": "helicopter"}, "status": "obs"},
      {"name": "effAttractiveness", "type": "f32", "offset": 8, "status": "obs", "meaning": "line+0x174: attractiveness * mean vehicle attractiveness / (1 + 0.01*avgWait); route-cost factor"},
      {"name": "vehicleCount", "type": "u32", "offset": 12, "status": "obs"},
      {"name": "stopCount", "type": "u32", "offset": 16, "status": "obs"},
      {"name": "avgWait", "type": "f32", "offset": 20, "status": "obs", "meaning": "line+0x180: 0.95/0.05 moving average of boarding wait ticks"},
      {"name": "ticketPrice", "type": "f32", "offset": 24, "status": "obs"},
      {"name": "passengersTransported", "type": "i32", "offset": 28, "status": "obs"},
      {"name": "isClosed", "type": "u8", "offset": 32, "status": "obs"},
      {"name": "pad", "type": "bytes[3]", "offset": 33, "status": "obs"}
    ], "children": [{"tag": "0x07060220", "count": "stopCount"}, {"tag": "0x07060210", "count": "vehicleCount"}]},

    {"tag": "0x07060220", "name": "LineStop", "parent": "0x07060200", "size": 20, "fields": [
      {"name": "networkItem", "type": "u32", "offset": 0, "ref": "0x07020210 record (file order)", "oneBased": true, "status": "obs"},
      {"name": "flags", "type": "u32", "offset": 4, "note": "bit0 = temporary stop; not restored on load", "status": "obs"},
      {"name": "sinceDeparture", "type": "f32", "offset": 8, "status": "obs", "meaning": "node+0x34: seconds since a vehicle last left this stop"},
      {"name": "passengersTransported", "type": "i32", "offset": 12, "status": "obs"},
      {"name": "waitingTime", "type": "i32", "offset": 16, "status": "obs"}
    ], "children": []},

    {"tag": "0x07060210", "name": "LineVehicle", "parent": "0x07060200", "fields": [
      {"name": "gridVehicle", "type": "u32", "offset": 0, "ref": "0x07040200 record (file order)", "oneBased": true, "status": "obs"},
      {"name": "stopA", "type": "u32", "offset": 4, "ref": "line-local 0x07060220", "oneBased": true, "status": "obs"},
      {"name": "stopB", "type": "u32", "offset": 8, "ref": "line-local 0x07060220", "oneBased": true, "status": "obs"},
      {"name": "dockedAt", "type": "u32", "offset": 12, "ref": "line-local 0x07060220", "oneBased": true, "status": "obs"},
      {"name": "stopTime", "type": "f32", "offset": 16, "status": "obs"},
      {"name": "respawnRepathTimer", "type": "f32", "offset": 20, "status": "obs", "meaning": "veh+0x164: re-path timer while unspawned"},
      {"name": "nextLegSteps", "type": "i32", "offset": 24, "status": "obs", "meaning": "veh+0x168: step count of the pre-loaded next leg"},
      {"name": "capacity", "type": "i32", "offset": 28, "status": "obs", "note": "ignored on load"},
      {"name": "passengerCount", "type": "i32", "offset": 32, "status": "obs"},
      {"name": "boardingReserved", "type": "i32", "offset": 36, "status": "obs", "meaning": "veh+0x174: citizens walking to the doors"},
      {"name": "attractiveness", "type": "f32", "offset": 40, "status": "obs"},
      {"name": "travelDistance", "type": "f32", "offset": 44, "status": "obs"},
      {"name": "unitCount", "type": "u32", "offset": 48, "status": "obs"},
      {"name": "bellRung", "type": "u8", "offset": 52, "status": "obs", "meaning": "veh+0x184: vehicle bell played"},
      {"name": "announced", "type": "u8", "offset": 53, "status": "obs", "meaning": "veh+0x185: stop bell played"},
      {"name": "atStopB", "type": "u8", "offset": 54, "status": "obs"},
      {"name": "broken", "type": "u8", "offset": 55, "status": "obs"},
      {"name": "passengersTransported", "type": "i32", "offset": 56, "status": "obs"},
      {"name": "units", "type": "record", "size": 4, "offset": 60, "count": "unitCount", "fields": [
        {"name": "modelIndex", "type": "u16", "offset": 0, "ref": "0x07060100 (0-based)", "status": "obs"},
        {"name": "reversed", "type": "u8", "offset": 2, "status": "obs"},
        {"name": "pad", "type": "u8", "offset": 3, "status": "obs"}
      ]}
    ], "children": []},

    {"tag": "0x07080000", "name": "Timeline", "parent": null, "fields": [
      {"name": "dayCount", "type": "u32", "offset": 0, "status": "obs", "note": "maxDay+1, ignored on load"},
      {"name": "currentDay", "type": "u32", "offset": 4, "unit": "days since 1920-01-01", "status": "obs"}
    ], "children": [{"tag": "0x07081000", "count": "*"}]},

    {"tag": "0x07081000", "name": "TimelineDay", "parent": "0x07080000", "fields": [
      {"name": "day", "type": "u32", "offset": 0, "unit": "days since 1920-01-01", "status": "obs"},
      {"name": "reserved", "type": "u32", "offset": 4, "status": "obs"}
    ], "children": [{"tag": "@action", "count": "*", "applyOrder": "reverse"}]},

    {"tag": "0x07081010", "name": "Action_group", "class": "gsGridTimelineAction_group", "group": "@action", "fields": [
      {"name": "label", "type": "bytes[32]", "offset": 0, "encoding": "latin1-cstr", "status": "obs"}
    ], "children": [{"tag": "@action", "count": "*", "applyOrder": "reverse"}]},

    {"tag": "0x07081020", "name": "Action_createObject", "class": "gsGridTimelineAction_createObject", "group": "@action", "size": 44, "fields": [
      {"name": "object", "type": "struct:ObjRef", "offset": 0, "status": "obs"}
    ], "children": []},

    {"tag": "0x07081030", "name": "Action_createObjectWithParent", "class": "gsGridTimelineAction_createObject", "group": "@action", "size": 88, "fields": [
      {"name": "object", "type": "struct:ObjRef", "offset": 0, "status": "obs"},
      {"name": "parent", "type": "struct:ObjRef", "offset": 44, "status": "obs"}
    ], "children": []},

    {"tag": "0x07081040", "name": "Action_deleteObject", "class": "gsGridTimelineAction_deleteObject", "group": "@action", "size": 44, "fields": [
      {"name": "object", "type": "struct:ObjRef", "offset": 0, "status": "obs"}
    ], "children": []},

    {"tag": "0x07081050", "name": "Action_brush", "class": "gsGridTimelineAction_brush", "group": "@action", "fields": [
      {"name": "brushType", "type": "u8", "offset": 0, "status": "obs"},
      {"name": "radius", "type": "i8", "offset": 1, "status": "obs"},
      {"name": "count", "type": "u16", "offset": 2, "status": "obs"},
      {"name": "strength", "type": "i16", "offset": 4, "status": "obs"},
      {"name": "reserved", "type": "u16", "offset": 6, "status": "obs"},
      {"name": "points", "type": "record", "size": 4, "offset": 8, "count": "count", "fields": [
        {"name": "x", "type": "i16", "offset": 0, "status": "obs"},
        {"name": "z", "type": "i16", "offset": 2, "status": "obs"}
      ]}
    ], "children": []},

    {"tag": "0x07081060", "name": "Action_water", "class": "gsGridTimelineAction_water", "group": "@action", "size": 8, "fields": [
      {"name": "x", "type": "i16", "offset": 0, "status": "obs"},
      {"name": "reserved", "type": "u16", "offset": 2, "status": "obs"},
      {"name": "z", "type": "i16", "offset": 4, "status": "obs"},
      {"name": "on", "type": "u16", "offset": 6, "status": "obs"}
    ], "children": []},

    {"tag": "0x07081070", "name": "Action_name_v0", "class": "gsGridTimelineAction_name", "group": "@action", "fields": [
      {"name": "object", "type": "struct:ObjRef", "offset": 0, "status": "obs"},
      {"name": "text", "type": "cstr", "offset": 44, "rest": true, "encoding": "latin1", "status": "obs"}
    ], "children": []},

    {"tag": "0x07081071", "name": "Action_name", "class": "gsGridTimelineAction_name", "group": "@action", "fields": [
      {"name": "object", "type": "struct:ObjRef", "offset": 0, "status": "obs"},
      {"name": "text", "type": "cstr", "offset": 44, "rest": true, "encoding": "utf8(ucs2)", "align": 4, "status": "obs"}
    ], "children": []},

    {"tag": "0x07081080", "name": "Action_variation", "class": "gsGridTimelineAction_variation", "group": "@action", "size": 48, "fields": [
      {"name": "object", "type": "struct:ObjRef", "offset": 0, "status": "obs"},
      {"name": "variation", "type": "u32", "offset": 44, "status": "obs"}
    ], "children": []}
  ]
}
```

---

## 4. Validation on the samples

Script: `C:\Users\Marku\AppData\Local\Temp\claude\C--Users-Marku-Cities-in-Motion\fe525a7e-5e90-46f7-b12c-445b2bb3c3e0\scratchpad\save-lines\parse.mjs`, plus `analyze.mjs` (ordering and flag analysis) and `basecheck.mjs` (timeline targets compared with the base grid). The scripts reuse `tools/gridparse.mjs` `Reader`, open files read-only, and write nothing outside the scratch folder. Raw outputs: `lines-results.jsonl` and `timeline-results.jsonl` in the same folder.

### 4.1 Transport lines

Checks run on every file: chunk sizes consumed exactly (`0x07060220` = 20, `0x07060210` = 60 + 4·unitCount, `0x07060100` names + pad = chunk size). Header counts equal the counted chunks (lines, Σ stops, Σ vehicles, model names). Per-line `stopCount`/`vehicleCount` equal its child counts; stop chunks come before vehicle chunks. `networkItem` is in 1..itemCount, and the referenced item's type name is a stop type. `gridVehicle` is in 0..gridVehicleCount, and the referenced 0x07040200 record's model (u16 index into 0x07040100) equals the vehicle's first unit model. `modelIndex` is less than modelCount. `stopA/stopB/dockedAt` ≤ line stop count.

| file | map | net items | grid veh | header {models, lines, vehicles, stops} | units/vehicle | gridVehicle model match | issues |
|---|---|---|---|---|---|---|---|
| saves/Cim2/map.grid | 1280² | 87 244 | 2 002 | {4, 1, 2, 19} | 5×2 | 2/2 | 0 |
| saves/berlin/map.grid | 1280² | 116 509 | 1 673 | {5, 8, 76, 67} | 1×46, 4×30 | 76/76 | 0 |
| saves/vienna/map.grid | 1024² | 72 845 | 1 884 | {10, 6, 18, 38} | 1×7, 5×11 | 18/18 | 0 |
| maps scenario01 | 1280² | 116 096 | 2 024 | {1, 3, 5, 19} | 1×5 | 5/5 | 0 |
| maps scenario02 | 768² | 24 814 | 575 | {0, 0, 0, 0} | – | – | 0 |
| maps scenario03 | 1024² | 58 852 | 1 060 | {6, 4, 7, 19} | 1×5, 2×2 | 7/7 | 0 |
| maps scenario04 | 1280² | 118 787 | 504 | {3, 4, 5, 7} | 1×2, 6×3 | 0 (all 5 have gridVehicle = 0, unspawned; lines open, isClosed = 0) | 0 |
| maps scenario05 | 1024² | 29 352 | 1 515 | {2, 2, 2, 4} | 6×2 | 2/2 | 0 |
| maps scenario06 | 1024² | 69 783 | 1 944 | {3, 4, 4, 12} | 1×2, 6×2 | 4/4 | 0 |
| maps scenario07 | 1280² | 118 178 | 2 220 | {7, 4, 6, 20} | 1×2, 2×2, 6×2 | 6/6 | 0 |
| maps scenario08 | 768² | 24 096 | 967 | {3, 3, 8, 24} | 1×3, 2×5 | 8/8 | 0 |
| maps scenario09 | 1024² | 57 071 | 484 | {2, 4, 26, 37} | 1×26 | 26/26 | 0 |
| maps scenario10 | 1024² | 30 309 | 1 558 | {1, 1, 2, 7} | 1×2 | 2/2 | 0 |
| maps scenario11 | 1024² | 61 370 | 1 231 | {2, 4, 28, 43} | 2×28 | 28/28 | 0 |
| maps scenario12 | 1280² | 119 806 | 2 201 | {7, 3, 11, 21} | 1×3, 5×8 | 11/11 | 0 |
| maps base grids (cologne, amsterdam, berlin, helsinki, tutorialcity, vienna) | – | – | 0 | {0, 0, 0, 0} | – | – | 0 |

Totals: 51 lines (47 closed, 4 open), 337 stop references, **337/337 to stop-type items** (`stop_bus*`, `stop_tram*`, `station_metrou*/metroe*`, `dock-platform`, `heliport`). 200 vehicles: 195 grid-vehicle model matches, 5 unspawned, **0 mismatches**. Line types seen: 0–4. Pad bytes always 0. The unit `reversed` flag is 1 only on the last car of metro/tram sets. Line order matches `gamedata.script $lines` (berlin: 13/13/8/2/17/8/4/11 vehicles).

### 4.2 Timelines

Checks: every chunk consumed exactly. Fixed sizes: 0x07081020 = 44, 0x07081030 = 88, 0x07081040 = 44, 0x07081050 = 8 + 4·count, 0x07081060 = 8, 0x07081070/71 = 44 + n with n % 4 = 0, NUL present and ≤ 3 bytes after it, 0x07081080 = 48. Day groups strictly ascending; `dayCount == lastDay + 1`; no unknown tags; coordinates inside the sibling grid's width/length.

| file | dayCount | currentDay (date) | first → last group day | days | actions | tag counts (grp/create/create+p/delete/brush/name/variation) | issues |
|---|---|---|---|---|---|---|---|
| maps/addons/germany/cologne | 33 970 | 0 (1920-01-01) | 1922-04-01 → 2013-01-01 | 73 | 17 288 | 5 852 / 4 311 / 2 057 / 2 293 / 171 / 1 478 (71) / 1 126 | 0 |
| maps/metro/amsterdam | 32 508 | 0 | 1925-01-01 → 2008-12-31 | 46 | 5 629 | 1 891 / 1 349 / 170 / 1 720 / 364 / 122 (71) / 13 | 0 |
| maps/metro/berlin | 35 431 | 0 | 1924-05-01 → 2017-01-01 | 67 | 29 782 | 10 681 / 9 377 / 3 401 / 3 940 / 387 / 1 374 (71) / 622 | 0 |
| maps/metro/helsinki | 36 526 | 0 | 1923-01-01 → 2020-01-01 | 204 | 7 660 | 2 392 / 2 076 / 146 / 1 454 / 157 / 1 275 (71) / 160 | 0 |
| maps/metro/tutorialcity | 32 918 | 0 | 2009-09-01 → 2010-02-14 | 2 | 59 | 27 / 26 / 1 / 0 / 2 / 0 / 3 | 0 |
| maps/metro/vienna | 36 891 | 0 | 1921-01-01 → 2020-12-31 | 320 | 87 734 | 31 672 / 24 943 / 9 541 / 9 415 / 4 671 / 4 007 (71) / 3 485 | 0 |
| scenario01, 07, 12 (berlin) | 35 431 | 77 (1920-03-18) / 23 835 (1985-04-04) / 34 857 (2015-06-08) | 1924-05-01 → 2017-01-01 | 63 | 29 774 | names use **0x07081070** (1 374) | 0 |
| scenario02, 08 (amsterdam) | 32 508 | 12 818 (1955-02-04) / 27 792 (1996-02-03) | 1925-01-01 → 2008-12-31 | 46 | 5 629 | names 0x07081071 | 0 |
| scenario03, 06, 09, 11 (vienna) | 36 526 | 14 680 (1960-03-11) / 29 317 (2000-04-07) / 2 418 (1926-08-15) / 18 590 (1970-11-24) | 1921-01-01 → 2020-01-01 | 315 | 87 724 | names 0x07081070 | 0 |
| scenario04 (berlin) | 35 431 | 21 550 (1979-01-01) | 1924-05-01 → 2017-01-01 | 64 | 32 732 | names 0x07081070 | 0 |
| scenario05, 10 (helsinki) | 36 526 | 25 631 (1990-03-05) / 9 270 (1945-05-19) | 1923-01-01 → 2020-01-01 | 198 | 7 619 | names 0x07081070 | 0 |
| saves/berlin | 35 431 | 4 729 (1932-12-12) | 1924-05-01 → 2017-01-01 | 67 | 29 782 | identical to maps/berlin | 0 |
| saves/vienna | 36 891 | 33 228 (2010-12-22) | 1921-01-01 → 2020-12-31 | 320 | 87 734 | identical to maps/vienna | 0 |
| saves/Cim2 (16 bytes) | 0 | 33 019 (2010-05-27) | – | 0 | 0 | – | 0 |

Tag coverage in the samples: 0x07081010, 0x07081020, 0x07081030, 0x07081040, 0x07081050, 0x07081070, 0x07081071 and 0x07081080 all occur. **0x07081060 (water) never occurs**, so it is validated from code only. Group nesting depth is 1 in every file; one empty group (vienna). Top-level non-group actions exist (berlin 493, vienna 4 999: brush and variation records made outside an editor group). Brush types seen: 1–9 and 18–22; radius 0–25; reserved field always 0. Out-of-map coordinates: one brush point (−1, −1) on day 19 237 in the berlin family; everything else is inside the map. Object `orient` is always 0–3. ObjRef flags seen: 0, 0x02000000 and 0x40000000 (create); parent and delete refs only 0 or 0x02000000.

Group labels (berlin): `%9` 8 330, `%0` 1 606, `%8` 385, `%1` 116, `%7` 105, `%2` 98, `%4` 17, `%26` 16, `%5` 8. No `%10`/`%3`/`%6` in berlin; `%3` occurs in vienna and amsterdam.

### 4.3 Semantic cross-checks

* **Apply order is reverse file order.** For `createObject+parent`, the parent is created on the same day *later* in the file 2 427× (berlin), 6 968× (vienna) and 994× (cologne), against 25 / 72 / 0 *earlier*. For name/variation records whose target is created the same day, the target is later in the file 1 469 / 5 364 / 2 139× against 10 / 3 / 1. A delete whose target is created the same day finds it later in the file 222 / 340 / 243× against 5 / 4 / 18. The few counter-examples are same-key duplicates (inf).
* **Base grid = 1920 state.** Delete targets are found in the sibling base `.grid` (template, x, z, orient; y ignored as the delete code does): berlin 3 540/3 940, vienna 5 802/9 415, cologne 1 783/2 293. The rest are created by earlier timeline days. Create targets are almost never in the base grid (10/9 377 berlin). (`basecheck.mjs`.)
* **Dates:** last group days fall exactly on 1 Jan 2013 / 2017 / 2020 or on 31 Dec 2008 / 2020 (amsterdam, vienna), and 552 of the 712 groups across the six base maps lie on the 1st of a month (berlin 65/67, vienna 305/320, cologne 69/73, amsterdam 40/46, helsinki 72/204, tutorialcity 1/2). That confirms the 1920-01-01 epoch and the 0-based day index. Vienna's groups spread evenly over the decades 1920–2020, which looks like gradual city growth.
* **Name strings:** street names such as `Hanns-Braun-Straße`, `Türkenstrasse`, `Sporthallenweg` and placeholders like `$cinema` / `$stadium` decode correctly in both the UTF-8 (0x07081071) and Latin-1 (0x07081070) variants.

---

## 5. Open questions

1. **Unnamed line floats** `+0x168`, `+0x174`, `+0x180` (header offsets 0, 8, 20). All three are restored on load. Cim2's single 19-stop/2-vehicle line has 360 at `+0x168`, against 5–72 for busy lines, which hints at a headway or interval. `+0x174` feeds the `routeExists` cost. The functions that update them have not been read.
2. **Vehicle fields** `+0x164` (f32 0.02–0.25), `+0x168` (i32 0–104), `+0x174` (i32, nonzero only while docked), and the bools `+0x184`/`+0x185`. The writer and loader are clear; the meaning needs the vehicle update code (`gsTransportVehicle` vtable).
3. **Stop node `+0x34`** (f32 0–307): perhaps time since the last vehicle left; not confirmed.
4. **Vehicle `condition` (`+0x180`) is not persisted.** Either it resets on load or another chunk or script restores it; check `gamedata.script` and the `0x07040200` grid-vehicle record.
5. **ObjRef flag bits:** 0x02000000 appears on generated support objects (pillars, animation helpers); 0x40000000 on free-placed details. Exact names come from the network item flag set (`item+0x48`), documented with 0x07020210.
6. **`expandPattern` index mapping** (`%N` → N-th title argument, 0-based) is inferred from the argument order in `timebar.script`; the `expandPattern` native has not been read.
7. **Water action (0x07081060)** has no sample. Its layout comes from code only (ctor/apply/label); the 2-byte field at offset 2 is assumed to be always 0.
8. **Global index use by 0x07070000:** that citizens refer to line vehicles and stops by 1-based global file-order index is inferred from the arrays passed out of `FUN_1011b110`. Confirm it in the citizens section.
9. **Preview-mode effects** (mostly answered): 0x500000 is TEMP (0x100000) plus HIDDEN (0x400000), so a preview copy is never drawn, and the `gsGridItem::fixGrid` wrapper (`L:0x08185a40`) skips it, so it covers no cells. What players see is the cell site mark 0x40 that a preview of a LOCK_GROUND (8) template paints on cells without 0x8: brown construction ground in the terrain and the orange areas on the minimap. The real apply clears 0x40 again (`L:0x081a81b0`). The port reproduces this (`port/src/engine/timeline.cpp`). Still open: whether `onConstructInstance` scripts add visible scaffolding of their own.
