# Terrain, characters and citizens (`0x07010000`, `0x0705xxxx`, `0x0707xxxx`, `0x07061000`)

Part of the `map.grid` format ([README](README.md) §3). Decoded from the GSystem.dll pseudocode in
`decompiled/GSystem-v2/functions/` and checked against 21 sample grids (§4).
Tags in the tables: **obs** = read directly from the writer/loader pseudocode or confirmed by the sample data;
**inf** = inferred from how the engine uses the field, or from names.

## 1. Overview

| Chunk | Parent | Body | Writer | Loader |
|---|---|---|---|---|
| `0x07010000` terrain | `0x07000000` | `width × length` cells × 4 B, row-major (z outer, x inner) | `0x100c3840.c:90-92` | `0x100c3310.c:72-117` |
| `0x07050000` characters | `0x07000000` | 8-B header, then children | `0x100cb5c0.c` | `0x100cace0.c` (called from `0x100c3310.c:64-68`) |
| `0x07050100` character type names | `0x07050000` | `typeCount` C strings, zero-padded to 4 B | `0x100cb5c0.c:106-160` | `0x100cace0.c:109-195` |
| `0x07050200` character | `0x07050000` (× count) | 80-B record + one `0x07030100` child | `0x100cb5c0.c:174-201` | `0x100cace0.c:196-296` |
| `0x07050300` flying animals list | `0x07050000` | `u32 n, u32 charIndex[n]` | `0x100cb5c0.c:211-243` | `0x100cace0.c:83-108` |
| `0x07050400` ground animals list | `0x07050000` | `u32 n, u32 charIndex[n]` | `0x100cb5c0.c:245-277` | `0x100cace0.c:298-323` |
| `0x07070000` citizens | `0x07000000` | 144-B header, then children | `0x100ceb40.c:73-110` | `0x100d31a0.c:60-76` (called from `0x100c3310.c:145-150`) |
| `0x07070100` citizen | `0x07070000` (× count) | 64-B record + optional `0x07061000` child | `0x100ceb40.c:111-151` | `0x100d31a0.c:121-372` |
| `0x07061000` planned transit route | `0x07070100` | `f32 cost, u32 n, u32 leg, u32 node[n]` | `0x10111120.c:19-35` (called `0x100ceb40.c:147-149`) | `0x10116de0.c` (called `0x100d31a0.c:293-320`) |

**Tags that do not exist.** `0x07050001`, `0x07050301` and `0x07070707` appear in the pseudocode only as numbers
and are not chunk tags:
- `0x7050001` (`0x100c3310.c:63`) and `0x7050301` (`0x100cace0.c:82`) are bounds in compiled `switch` comparisons
  (`tag < 0x7050301` really means `tag ≤ 0x7050300`). Neither loader has a case for those tags, and no writer emits
  them. This build has **one** character record version (`0x07050200`).
- `0x7070707` is entry 13 of a byte-mask table in `FUN_100e7be0` (`0x100e7be0.c:26`). That function rotates each byte
  of a 32-bit connection mask by n bits (it rotates road/track connection masks for `loadGridObject`). It has no
  connection to serialization.

**Load order and flags.** The loader of the root chunk handles children in file order. `0x07050000` and `0x07070000`
are skipped when `gsGrid.ignoreCitizens` (`grid+0x24e`) is set (`0x100c3310.c:65,145`). Citizens resolve indices
into arrays built by earlier sections, so the writer's order matters:
items `0x07020000` → vehicles `0x07040000` → characters `0x07050000` → lines `0x07060000` → citizens `0x07070000` [obs, `0x100c3840.c:363-366`].

**Index conventions** [obs]. The writer builds `std::map<ptr,int>` tables. `FUN_100ba930` (`0x100ba930.c`) writes
`index+1`, or **0 for null/unknown**. On load, `FUN_100b58a0` (`0x100b58a0.c`) maps `i` to `array[i-1]` when
`1 ≤ i ≤ count`; any other value becomes null. So every object reference in these sections is **1-based, with 0 = none**:

| Reference | Array (file order) |
|---|---|
| item | `0x07020210` records (network items) |
| car | `0x07040200` records |
| character | `0x07050200` records |
| line node | `0x07060220` records, counted across all lines in file order (`0x1011a9f0.c` fills map `param_6` per line node) |
| transport vehicle | `0x07060210` records, counted across all lines (map `param_5`) |

The type index in a character record is the exception: it is **0-based** into `0x07050100`.

## 2. Chunk layouts

### 2.1 `0x07010000` terrain cells

There are `width × length` cells of 4 bytes, row-major (`index = z*width + x`). The writer dumps `grid+0x1c4` raw
(`0x100c3840.c:90-92`). The loader copies it back, then edits each cell (`0x100c3310.c:86-108`):

1. `height += 0x200 − header.version`. The grid header's `u16 0x0200` is a **height base**; files written by
   this build need no adjustment.
2. Clear flag bits `0x4000` (selection highlight) and `0x0f00` (cover quadrants).
3. Before clearing, use the flags to seed the per-cell pedestrian sub-cell map at `grid+0x1c8`:
   `0xaaaaaaaa` (all 16 quarter-cells walk code 2 = pavement) when `flags & 0x8` and `(flags & 0xf00) != 0xf00`,
   otherwise `0xffffffff` (code 3 = open ground) (`0x100c3310.c:96-102`).

Cell heights are **vertex** heights: the cell `(x,z)` quad uses `h(x,z)`, `h(x+1,z)` and `h(x,z+1)` [obs, `0x100b5640.c:36-46`].

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 2 | i16 | height | Vertex height in 1/256 cell (world Y = h × cellSize / 256). Samples range from −2433 to 16920. The brush clamps noise at ≥ −0x600. | `0x100b5c30.c:47-63`, `0x100c3310.c:95`, `0x10101800.c:84-90` | high (obs) |
| 2 | 2 | u16 | flags | Bit field, see the next table. | below | per bit |

**Cell flag bits** (second u16):

| bits | mask | name | meaning | evidence | confidence |
|---|---|---|---|---|---|
| 0-2 | `0x0007` | style | Style index 0..7. With `pavement`: pavement material variant (brush codes 1-8 store `code-1`). With `field`: field/yard material variant (brush codes 10-17 store `code-10`; samples use 0-3). Otherwise: wild-ground texture variation (brush code 22 changes it randomly on cells with neither pavement nor field). | `0x10101800.c:93-105,306,310-311` | high (obs) for the writes; inf for the material lookup (`getFieldMaterial`/`getYardMaterial`) |
| 3 | `0x0008` | pavement | Paved cell (`GRIDMODE_SET_PAVEMENT`). Seeds walk code 2. Set by items with `GRID_ITEM_PAVEMENT` (`0x1000000`) in a 3-cell margin, which clears `0x27` (`& 0xffd8 \| 8`). | `0x10101800.c:310-314`, `0x100e9730.c:147,209`, `0x100c3310.c:96` | high (obs) |
| 4 | `0x0010` | blocked | Set under items with `GRID_ITEM_BLOCK` (`0x10000`), or tunnel/bridge-type items (`0x4000`) whose y − 0x400 is below the cell. Probably "ground occupied/no placement". | `0x100e9730.c:323-325` | medium (obs set, inf meaning) |
| 5 | `0x0020` | field | Field/yard texture (`GRIDMODE_SET_FIELD`). Brush codes 10-17 set it; code 18 clears it (`& 0xd8` on the low byte). Never combined with `0x8` in the samples. | `0x10101800.c:301-306` | high (obs) |
| 6 | `0x0040` | pavementPreview | Pavement painted in timeline *preview* mode (`param_2`): `(f & 0xffd0) \| 0x40`, only on unpaved cells. Seen only in maps that have pending timeline work (vienna, scenario03/06/07/09/11). | `0x10101800.c:316-317` | medium |
| 7 | `0x0080` | — | Never set by any code found, and never set in the samples. | — | low |
| 8-11 | `0x0f00` | coverQuadrants | Which parts of the cell are hidden under an item with `GRID_ITEM_COVER`. Four triangle/quadrant bits from `FUN_100e8f50`, where `0xf00` means the whole cell. Written only while `viewMode & 0x20` (part of `GRID_VIEW_DEFAULT` `0xfe`), recomputed whenever the view mode changes, and cleared on load. With `0xf00` set, terrain picking ignores the cell. Values in the samples: 0, `0xf`, and the half-cell values `0x3`/`0x6`/`0x9`/`0xc`. | `0x100e9730.c:318,329`, `0x100e8f50.c`, `0x100b7c60.c:21-33`, `0x100b5640.c:34`, `0x100c3310.c:103` | high (obs) — **transient**: a writer may store 0 |
| 12 | `0x1000` | groundLocked | Height is fixed by an item with `GRID_ITEM_LOCK_GROUND` (`0x8`). The item may also write the height. Terrain brushes skip these cells; removing the item clears the bit. | `0x100e9730.c:101-114`, `0x10101800.c:74,188,274`, `0x100d4de0.c:59` | high (obs) |
| 13 | `0x2000` | water | Water cell (`GRIDMODE_TOGGLE_WATER`, `FUN_100c0440`). Height brushes clear it once the cell and its 3 neighbours rise above −0x141. `getAreaType` counts it as area type 2. | `0x100c0440.c:58-66`, `0x10101800.c:355-375`, `0x100b5d40.c:66` | high (obs) |
| 14 | `0x4000` | highlight | Selection or placement highlight (`FUN_100b8ef0`/`FUN_100b90b0` set it; `FUN_100b8fe0` clears it). Cleared on load. Never present in the samples. | `0x100b8ef0.c:23`, `0x100b8fe0.c:35`, `0x100c3310.c:94` | high (obs) — transient |
| 15 | `0x8000` | highlightNegative | Highlight drawn with the "negative" (invalid) material. Set together with `0x4000`; `FUN_100b8fe0` clears it. Never present in the samples. | same | medium |

Common full values: `0x0000` plain ground, `0x0001..0x0007` wild-ground variation, `0x0020-0x0023` field, `0x0008`
plain pavement, `0x000d` = pavement style 5, `0x1000` locked ground, `0x1f00` locked and fully covered (under a
building), `0x1f08`/`0x1f0d` the same on pavement, `0x2000` water.

`getAreaType` (`0x100b5d40.c:66-72`) buckets cells as: water → type 2; `flags & 0xf08` (paved or covered) → type 0
("urban"); everything else → type 1. Item templates then add their own area weight (`+0x3b8`).

### 2.2 `0x07050000` characters (visible pedestrians and animals)

Owner: `gsGridCharacters` (`grid+0x210`). Characters are kept in per-slot linked lists (`slot+0x14`, next at
`char+0x10`). The writer walks slots row-major and writes each slot's list head-first (`0x100cb5c0.c:67-101,164-207`).
Each human character belongs to exactly one citizen (citizen record `+0x18`). Animals belong to no citizen.

**Header** (8 B) [obs `0x100cb5c0.c:103-105`, `0x100cace0.c:60-65`]:

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 4 | u32 | typeCount | Number of names in `0x07050100`. The writer gets it from the size of a `std::map<template*,idx>`, so the order follows pointer values and is not stable between saves. | `0x100cb5c0.c:103` | high |
| 4 | 4 | u32 | count | Number of `0x07050200` records. Sizes the character index array that citizens use. | `0x100cb5c0.c:104`, `0x100cace0.c:62-72` | high |

**`0x07050100` type names**: `typeCount` NUL-terminated template idNames (`gsGridObject+0x2b0`), concatenated and
padded with zeros to a 4-byte multiple (`0x100cb5c0.c:112-161`). The loader resolves each name in the grid's
template map (`grid+0x1ec`). An unknown name gives a null template (`0x100cace0.c:185-191`). Examples: `seagull`,
`pig`, `cow`, `era3-business-male`, …

**`0x07050200` character record**: an 80-byte body, then child chunks. The record's world object (`0xa8` bytes,
constructor `FUN_100c8b20`) is filled as listed below. Units: "q" = quarter-cell (cell × 4 + sub-cell), so the world
coordinate is `((q − 2·size) · 0.25 + 0.125) · cellSize` [obs, `0x100c8b20.c`, `0x100cace0.c:262-266`]. The object
offset is given in brackets.

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 2 | u16 | typeIndex | 0-based index into `0x07050100`. | `0x100cb5c0.c:178`, `0x100cace0.c:212` | high (obs) |
| 0x02 | 2 | u16 | anchor [+0x80] | Animals only: home block `(x>>5 & 0xff) \| (z>>5 & 0xff)<<8` (8-cell blocks) that birds drift back toward. Always 0 for pedestrians and always ≠ 0 for animals in the samples. | `0x100cb5c0.c:179`, `0x100cace0.c:242`, `0x100ca490.c:73-78,134-159` | high (obs) |
| 0x04 | 4 | i32 | x [+0x20] | Grid x in q units: the walk-graph point the character is at or heading to. Sets the slot: `x >> (bucketShift+2)`. | `0x100cace0.c:198-203`, `0x100c9470.c:61` | high (obs) |
| 0x08 | 4 | i32 | yLayer [+0x24] | Height (1/256 cell) of the walk-graph cell. Selects the graph layer (ground, bridge, tunnel) in `FUN_100e1d40(graph,x,y,z)`. Birds: target altitude = ground + `0x600 + rand(0x7ff)`. | `0x100c9470.c:109-118`, `0x100ca490.c:151` | high (obs) |
| 0x0c | 4 | i32 | yGround [+0x28] | Ground height interpolated at the sub-cell. The render target Y. | `0x100c9470.c:120-135` | high (obs) |
| 0x10 | 4 | i32 | z [+0x2c] | Grid z in q units. Used for the slot. | `0x100cace0.c:204-209` | high (obs) |
| 0x14 | 4 | i32 | frameStamp [+0x30] | Frame counter of the last update (`characters+0xc & 0xffff`). Constructor sets −1. Transient. | `0x100c9470.c:56-57` | medium (obs) |
| 0x18 | 12 | i32[3] | crossingCell [+0x34] | q-cell (x,y,z) of the road-crossing walk cell (walk code 1) the pedestrian is on. While set, road path segments within 1.5 units have their pedestrian counter (`segment+0x7c`) raised, so traffic waits. `x = −1` means none; only x is reset, so y and z can be stale. The loader re-raises the counters. | `0x100c9470.c:72-101,137-159`, `0x100cace0.c:252-277` | high (obs) |
| 0x24 | 12 | i32[3] | reservedCell [+0x40] | q-cell claimed in the walk-graph occupancy bitmap (collision avoidance). Usually equals (x,y,z). −1 means none. The loader sets the occupancy bit again. | `0x100c71d0.c:39-65,113-151`, `0x100cace0.c:243-251` | high (obs) |
| 0x30 | 12 | f32[3] | position [+0x4c] | World position. Matches `x,z` within one cell, except for birds in flight. | `0x100c8a10.c` (setPosition `+0x13`), `0x100c9470.c:219` | high (obs) |
| 0x3c | 12 | f32[3] | velocity [+0x64] | Smoothed movement vector. Reset to zero when the turn is larger than π/2. | `0x100c9470.c:193-219` | medium (obs) |
| 0x48 | 4 | f32 | heading [+0x78] | Yaw in radians, 0..2π. The loader copies it to the current heading (+0x74) as well. The constructor's current heading is the sentinel −10.0. | `0x100cace0.c:239-240`, `0x100c9470.c:205-207,252-265` | high (obs) |
| 0x4c | 4 | u32 | flags [+0x7c] | bit0 `0x1` = obeys crossings (cleared while boarding or alighting a vehicle); bit2 `0x4` = sitting down; bit3 `0x8` = seated; bit4 `0x10` = getting up. Constructor default 1. Samples: `0x1`, `0x0`, `0x9`, `0x5`. | `0x100c9470.c:69,174-251`, `0x100c71d0.c:56`, `0x100cc990.c:116` | medium (obs) |
| 0x50 | var | chunk | movement | Exactly one `0x07030100` child (the path step stream of `char+0x70`). Its layout belongs to the `0x0703` document. The loader passes it to `FUN_100e1fb0(path, reader, chunk, 8)` and ignores other children. | `0x100cb5c0.c:200`, `0x100cace0.c:280-289` | high (obs) |

Not saved: the slot and template pointers, render instance (`+0x08`), citizen back-pointer (`+0x1c`, restored from
the citizen record), seat target vec3 (`+0x58`) and animation state (`+0x84..+0xa4`).

**`0x07050300` / `0x07050400` animal lists** (`u32 n`, then `n × u32` 1-based character indices). Both are written
from vectors in `gsGridCharacters`: `+0xf8` → `0x07050300` and `+0x108` → `0x07050400`. Index 0 and out-of-range
indices are dropped on load [obs `0x100cace0.c:90-104`].
- `0x07050300` = **flying animals**. They are spawned for templates whose `+0x264 == 0`. Every 0.25 s per list cycle
  each bird picks a new target near its anchor at altitude ground + `0x600..0xdff` (`0x100ca490.c:66-79,102-171`).
  The samples contain only `seagull`.
- `0x07050400` = **ground animals**. They are spawned for templates whose `+0x264 == 1` at `(4x+1, y, 4z+1)`. Each
  one random-walks 64 three-bit direction steps over walk-code-3 sub-cells (`0x100ca490.c:81-89,258-330`). The
  samples contain only `cow` and `pig`.

The lists must follow every `0x07050200`, because they index the records loaded so far [inf from loader order].

### 2.3 `0x07070000` citizens

Owner: `gsGridCitizens` (`grid+0x214`). Citizens are a doubly linked list (`+0xdc` head, next `+0x3c`). The writer
walks it from the head. The loader inserts each record at the head, so **a load/save round trip reverses the order**
[obs `0x100ceb40.c:111`, `0x100d31a0.c:321-325`].

**Header** (144 B). The loader skips the whole section if the body is shorter than `0x90` (`0x100d31a0.c:60`):

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 4 | u32 | count | Number of citizens (`+0xe4`). Not read on load; the loader recounts. | `0x100ceb40.c:76`, `0x100d31a0.c:371` | high (obs) |
| 0x04 | 28 | u32[7] | walkCount | Trip statistics per class (`+0x990`, `getWalkCount`). | `0x100ceb40.c:74-109`, `0x100d31a0.c:64-76` | high (obs) |
| 0x20 | 28 | u32[7] | driveCount | `+0x9ac`, `getDriveCount`. | same | high |
| 0x3c | 28 | u32[7] | travelCount | `+0x9c8`, `getTravelCount` (public transport trips). | same | high |
| 0x58 | 28 | u32[7] | canTravelCount | `+0x9e4`, `getCanTravelCount`. | same | high |
| 0x74 | 28 | u32[7] | priceTooHighCount | `+0xa00`, `getPriceTooHighCount`. | same | high |

The arrays are indexed by class (`GRID_CITIZEN_BLUE_COLLAR`=0 … `UNEMPLOYED`=6). `resetTripCounts` zeroes them
(`docs/handlers/batch-A.md`).

**`0x07070100` citizen record**: a 64-byte body, then an optional `0x07061000` child. The citizen object is `0x60`
bytes (`0x100d31a0.c:150-183`). The object offset is given in brackets.

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 4 | u32 | flags [+0x48] | Bit field, see the next table. | `0x100ceb40.c:113`, `0x100d31a0.c:185` | per bit |
| 0x04 | 4 | u32 | id [+0x50] | Unique citizen ID: the key of the `findCitizen` map (`+0xec`) and the value of `getID`. Up to about 2³⁰; unique in every sample. Its low byte sets the patience while waiting: 256 + (id & 0xff). | `0x100d31a0.c:186,326-327,333-335`, `0x100ce7a0.c`, `0x100cd7a0.c` | high (obs) |
| 0x08 | 4 | u32 | tripQueue [+0x54] | Pending trips: 6-bit `GRID_VISIT_*` codes, next trip in the low bits, consumed by `>> 6`; 0 = plan a new day. Examples: `0x42` = work then home, `0x1102` = work, leisure, home. | `0x100d18f0.c:66-120` | high (obs) |
| 0x0c | 4 | u32 | usedTransport [+0x5c] | Bit mask of transport line types used on the current trip (bit = line type: 0 bus, 1 tram, 2 metro, 3 boat, 4 helicopter). Read by `usedTransport()`; cleared when a trip ends. | `0x100cd850.c`, `0x100cc6c0.c:70` | high (obs read), inf (type numbering from `GRIDSTOP_*`) |
| 0x10 | 4 | i32 | cycleTimer [+0x4c] | Day-cycle phase; the constructor default is `rand() % maxCycleDuration` (`+0x98c`). Samples: 0..600. | `0x100d31a0.c:181-182,189` | medium (obs) |
| 0x14 | 4 | u32 | waitTime [+0x58] | Time spent waiting at the current stop. On load it is compared with the patience to rebuild the mood bits: > p → 3; ≥ ¾p → 2; ≥ ½p → 1. On boarding it is added to the node's wait total (`node+0x6c`). | `0x100d31a0.c:332-369`, `0x100cc990.c:421-422` | high (obs) |
| 0x18 | 4 | u32 | character [+0x08] | 1-based `0x07050200` index of the visible pedestrian, or 0. The loader sets `char+0x1c = citizen`. | `0x100ceb40.c:119`, `0x100d31a0.c:191-195` | high (obs) |
| 0x1c | 4 | u32 | car [+0x0c] | 1-based `0x07040200` index of the private car driven or parked, or 0. The loader sets `vehicle+0x30 = citizen`. | `0x100ceb40.c:120`, `0x100d31a0.c:196-211` | high (obs) |
| 0x20 | 4 | u32 | lineNode [+0x10] | 1-based global `0x07060220` index of the line stop-node where the citizen is **waiting**, or 0. Never set together with `transportVehicle` in the samples. The node is ref-counted, and its waiting counters (`+0x58`; mood counters `+0x5c/0x60/0x64`; the stop's `+0x160..0x16c`) are rebuilt on load. | `0x100ceb40.c:121`, `0x100d31a0.c:212-221` | high (obs) |
| 0x24 | 4 | u32 | transportVehicle [+0x14] | 1-based global `0x07060210` index of the public-transport vehicle the citizen is in, or 0 (addRef'd). `getVehicle` returns it before `car`. | `0x100ceb40.c:122`, `0x100d31a0.c:222-226`, `0x100cc350.c` | high (obs) |
| 0x28 | 4 | u32 | home [+0x20] | 1-based item index (`getHome`). The loader takes the citizen out of its class's free-home pool. If the home is not in the pool, the reference is **dropped to 0**. | `0x100ceb40.c:123`, `0x100d31a0.c:123-136`, `0x100cc2b0.c` | high (obs) |
| 0x2c | 4 | u32 | workplace [+0x24] | 1-based item index or 0 (`getWorkplace`). Taken from the free-workplace pool in the same way. If 0, the class's unemployment counter `+0xa48` is raised. | `0x100d31a0.c:124,137-149,254-257` | high (obs) |
| 0x30 | 4 | u32 | start [+0x28] | 1-based item index of the trip origin (`getStart`): home or the previous target. | `0x100d31a0.c:229-230`, `0x100cc6c0.c:63-66` | high (obs) |
| 0x34 | 4 | u32 | target [+0x2c] | 1-based item index of the trip destination (`getTarget`), 0 when idle. | `0x100d31a0.c:231-233` | high (obs) |
| 0x38 | 4 | u32 | charModel [+0x30] | Index into the class's character pool (`addCharacter` order, `+0xf8+class*16`). The loader clamps it to the pool size − 1. | `0x100ceb40.c:127-136`, `0x100d31a0.c:234-246` | high (obs) |
| 0x3c | 4 | u32 | carModel [+0x34] | Index into the class's `GRID_VISIT_GENERIC` car pool (`addCar`, `+0x168+class*16`), or `0xffffffff` = none. **Not bounds-checked on load.** A record whose vehicle `+4` is null also becomes none. | `0x100ceb40.c:137-145`, `0x100d31a0.c:247-251` | high (obs) |
| 0x40 | var | chunk | route | Optional `0x07061000` (§2.4). Present only when `+0x1c` ≠ null; only the first one is used. | `0x100ceb40.c:147-149`, `0x100d31a0.c:293-320` | high (obs) |

**Citizen flag bits** (record `+0x00`):

| bits | mask | meaning | evidence | confidence |
|---|---|---|---|---|
| 0-3 | `0xf` | Class 0..6 (`GRID_CITIZEN_*`, `getClass`). | `0x100cd7c0.c` | high (obs) |
| 4-7 | `0xf0` | State (`getState`): 0 idle, `0x10` WALK, `0x20` DRIVE, `0x30` TRAVEL; `0x40` is a short-lived state after leaving a vehicle with route legs remaining. | `0x100cd7e0.c`, `0x100cd5c0.c:57,64`, `0x100cc990.c:128` | high (obs); `0x40` medium |
| 8 | `0x100` | Boarding: walking to the vehicle doors at the stop (counted in `vehicle+0x174`). | `0x100cc990.c:370,429`, `0x100cc6c0.c:41-45` | medium (obs) |
| 9 | `0x200` | Just left a vehicle; the pedestrian was spawned at the stop. | `0x100cc990.c:114,168,190` | medium (obs) |
| 10 | `0x400` | Trip ended while the pedestrian still exists; the next walk path starts from the character's position. | `0x100cc6c0.c:60-61`, `0x100cc990.c:42,70` | medium (inf) |
| 12-19 | `0xff000` | Visit type of the current trip (`GRID_VISIT_*`). Set only for types > 5 (utility or emergency trips, e.g. `0x6000` heavy industrial, `0xb000` police). It selects the car pool (`(type*7+class)`). | `0x100d18f0.c:110-112`, `0x100ce250.c:20-26` | high (obs) |
| 20 | `0x100000` | Car parked at the destination; cleared when the car is removed. | `0x100ceee0.c:375`, `0x100cc6c0.c:25` | medium (obs set; meaning inf) |
| 24-26 | `0x7000000` | Mood while waiting: `0x1000000` → 1, `0x2000000` → 2, `0x4000000` → 3 (`getMood`). Recomputed on load when the citizen is at a stop (`& 0xf0ffffff`, then re-derived from waitTime). | `0x100cd800.c`, `0x100d31a0.c:329-369` | high (obs) |
| 27-31 | — | Not seen (always 0 in the samples). | — | — |

Not saved: `+0x18` (walk path object), `+0x38/+0x3c` (list links) and `+0x40/+0x44` (DRIVE path endpoints, rebuilt
on load for state `0x20` via `FUN_100e24f0`, `0x100d31a0.c:258-291`).

**Load-time pool reconciliation** [obs `0x100d31a0.c:77-117,377-488`]. Before the records are read, the loader turns
each class's free-home vector (`+0x71c+cls*16`) and free-workplace vector (`+0x78c+cls*16`) into multisets. Each
record removes one copy of its home and workplace. The leftovers become the new free vectors. Those vectors are
built from item templates while the `0x07020000` items are placed, so a reimplementation must place the items and
build the same pools first, or home and workplace references will drop to 0.

### 2.4 `0x07061000` planned transit route

The citizen's chosen public-transport route (`citizen+0x1c`, a 16-byte object from `FUN_100d4500`). The writer is
`FUN_10111120`; the loader is `FUN_10116de0`.

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 4 | f32 | cost | Total generalized cost of the route. The route chooser compares it with the cost of the other options. Samples: 90..1964. | `0x10111120.c:20-23`, `0x100d18f0.c:313-316,379-382` | high (obs) |
| 0x04 | 4 | u32 | nodeCount | Number of entries that follow. | `0x10111120.c:21,33`, `0x10116de0.c:21-24` | high (obs) |
| 0x08 | 4 | u32 | currentLeg | Index of the leg being travelled, 0..nodeCount. The trip logic checks `leg < nodeCount`. | `0x100cc990.c:192,215` | high (obs) |
| 0x0c | 4·n | u32[n] | node | 1-based global `0x07060220` line-node indices (map `param_6` = `local_84`). Each node is ref-counted. **If any entry fails to resolve, the loader discards the whole route.** | `0x10111120.c:24-33`, `0x10116de0.c:26-38`, `0x100d31a0.c:307-315` | high (obs) |

Samples: routes appear only in state TRAVEL `0x30` (a handful in `0x10`). The citizen's `lineNode` equals
`node[leg]` or `node[leg-1]` in 94-100% of cases.

## 3. Machine-readable layout

`type` values: `u8 u16 i16 u32 i32 f32 bytes[n] cstr chunk`. Extra keys: `count` (literal number or a field path),
`stride` (a repeated grid), `pad` (align the end to n bytes), `ref` (what a 1-based index points to), `base`
(`1` = 1-based with 0 = none, `0` = 0-based), `bits` (named masks), `transient` (cleared or recomputed on load),
`repeat` (`"*"` = child repeated any number of times).

```json
[
  {
    "tag": "0x07010000",
    "parent": "0x07000000",
    "fields": [
      {"name": "cells", "type": "bytes[4]", "offset": 0, "count": "header.width*header.length", "stride": 4,
       "struct": [
         {"name": "height", "type": "i16", "offset": 0, "unit": "1/256 cell", "note": "add (0x200 - header.version) on load"},
         {"name": "flags", "type": "u16", "offset": 2, "bits": {
           "style": "0x0007", "pavement": "0x0008", "blocked": "0x0010", "field": "0x0020", "pavementPreview": "0x0040",
           "coverQuadrants": "0x0f00", "groundLocked": "0x1000", "water": "0x2000", "highlight": "0x4000", "highlightNegative": "0x8000"},
           "transient": ["0x0f00", "0x4000", "0x8000"]}
       ]}
    ],
    "children": []
  },
  {
    "tag": "0x07050000",
    "parent": "0x07000000",
    "fields": [
      {"name": "typeCount", "type": "u32", "offset": 0},
      {"name": "count", "type": "u32", "offset": 4}
    ],
    "childrenOffset": 8,
    "children": [
      {"tag": "0x07050100", "fields": [
        {"name": "typeNames", "type": "cstr", "offset": 0, "count": "parent.typeCount", "pad": 4}]},
      {"tag": "0x07050200", "repeat": "parent.count", "fields": [
        {"name": "typeIndex", "type": "u16", "offset": 0, "ref": "0x07050100", "base": 0},
        {"name": "anchor", "type": "u16", "offset": 2, "note": "animals: (x>>5&0xff)|(z>>5&0xff)<<8; humans 0"},
        {"name": "x", "type": "i32", "offset": 4, "unit": "quarter cell"},
        {"name": "yLayer", "type": "i32", "offset": 8, "unit": "1/256 cell"},
        {"name": "yGround", "type": "i32", "offset": 12, "unit": "1/256 cell"},
        {"name": "z", "type": "i32", "offset": 16, "unit": "quarter cell"},
        {"name": "frameStamp", "type": "i32", "offset": 20, "transient": true},
        {"name": "crossingCell", "type": "i32", "offset": 24, "count": 3, "unit": "quarter cell / 1/256", "note": "x=-1 none"},
        {"name": "reservedCell", "type": "i32", "offset": 36, "count": 3, "note": "x=-1 none"},
        {"name": "position", "type": "f32", "offset": 48, "count": 3, "unit": "world"},
        {"name": "velocity", "type": "f32", "offset": 60, "count": 3, "unit": "world"},
        {"name": "heading", "type": "f32", "offset": 72, "unit": "rad"},
        {"name": "flags", "type": "u32", "offset": 76, "bits": {"obeyCrossings": "0x1", "sittingDown": "0x4", "seated": "0x8", "gettingUp": "0x10"}}
      ], "childrenOffset": 80,
        "children": [{"tag": "0x07030100", "repeat": 1, "opaque": true, "note": "movement path; see 0x0703 document"}]},
      {"tag": "0x07050300", "fields": [
        {"name": "n", "type": "u32", "offset": 0},
        {"name": "flyingAnimals", "type": "u32", "offset": 4, "count": "n", "ref": "0x07050200", "base": 1}]},
      {"tag": "0x07050400", "fields": [
        {"name": "n", "type": "u32", "offset": 0},
        {"name": "groundAnimals", "type": "u32", "offset": 4, "count": "n", "ref": "0x07050200", "base": 1}]}
    ]
  },
  {
    "tag": "0x07070000",
    "parent": "0x07000000",
    "fields": [
      {"name": "count", "type": "u32", "offset": 0},
      {"name": "walkCount", "type": "u32", "offset": 4, "count": 7},
      {"name": "driveCount", "type": "u32", "offset": 32, "count": 7},
      {"name": "travelCount", "type": "u32", "offset": 60, "count": 7},
      {"name": "canTravelCount", "type": "u32", "offset": 88, "count": 7},
      {"name": "priceTooHighCount", "type": "u32", "offset": 116, "count": 7}
    ],
    "childrenOffset": 144,
    "children": [
      {"tag": "0x07070100", "repeat": "parent.count", "fields": [
        {"name": "flags", "type": "u32", "offset": 0, "bits": {
          "class": "0x0000000f", "state": "0x000000f0", "boarding": "0x00000100", "alighted": "0x00000200",
          "characterLingering": "0x00000400", "visitType": "0x000ff000", "carParked": "0x00100000", "mood": "0x07000000"}},
        {"name": "id", "type": "u32", "offset": 4},
        {"name": "tripQueue", "type": "u32", "offset": 8, "note": "6-bit GRID_VISIT_* codes, low first"},
        {"name": "usedTransport", "type": "u32", "offset": 12},
        {"name": "cycleTimer", "type": "i32", "offset": 16},
        {"name": "waitTime", "type": "u32", "offset": 20},
        {"name": "character", "type": "u32", "offset": 24, "ref": "0x07050200", "base": 1},
        {"name": "car", "type": "u32", "offset": 28, "ref": "0x07040200", "base": 1},
        {"name": "lineNode", "type": "u32", "offset": 32, "ref": "0x07060220(global)", "base": 1},
        {"name": "transportVehicle", "type": "u32", "offset": 36, "ref": "0x07060210(global)", "base": 1},
        {"name": "home", "type": "u32", "offset": 40, "ref": "0x07020210", "base": 1},
        {"name": "workplace", "type": "u32", "offset": 44, "ref": "0x07020210", "base": 1},
        {"name": "start", "type": "u32", "offset": 48, "ref": "0x07020210", "base": 1},
        {"name": "target", "type": "u32", "offset": 52, "ref": "0x07020210", "base": 1},
        {"name": "charModel", "type": "u32", "offset": 56, "ref": "classCharacterPool", "base": 0},
        {"name": "carModel", "type": "i32", "offset": 60, "ref": "classGenericCarPool", "base": 0, "note": "-1 none"}
      ], "childrenOffset": 64,
        "children": [
          {"tag": "0x07061000", "repeat": "0..1", "fields": [
            {"name": "cost", "type": "f32", "offset": 0},
            {"name": "nodeCount", "type": "u32", "offset": 4},
            {"name": "currentLeg", "type": "u32", "offset": 8},
            {"name": "node", "type": "u32", "offset": 12, "count": "nodeCount", "ref": "0x07060220(global)", "base": 1}]}
        ]}
    ]
  }
]
```

## 4. Validation

Scripts (read-only):
`C:\Users\Marku\AppData\Local\Temp\claude\C--Users-Marku-Cities-in-Motion\fe525a7e-5e90-46f7-b12c-445b2bb3c3e0\scratchpad\save-citizens\validate.mjs`
(the main check), plus these helpers in the same folder: `compact.mjs` (one-line summaries), `crossref.mjs`
(class ↔ model, item type names), `route.mjs` (route semantics), `rec2.mjs` (the `tripQueue` histogram),
`lines.mjs` (line node/vehicle counts) and `nodes.mjs` (stop types of line-node references). They reuse `Reader`
from `tools/gridparse.mjs`. Run them with `node validate.mjs <file.grid>...`.

Checks run on all 21 grids (3 saves, `cologne`, 17 metro maps and scenarios). **No problems found:**
- Every `0x07010000` is exactly `W·L·4` bytes.
- In every `0x07050000`, `count` equals the number of `0x07050200` records, the name table consumes its chunk
  exactly (with 4-byte padding), every `typeIndex < typeCount`, and every record is 80 B + exactly one `0x07030100`.
- `0x07050300`/`0x07050400` sizes are exactly `4 + 4n`, with all indices in `1..count`.
- In every `0x07070000`, `count` equals the number of `0x07070100` records, every record is 64 B + child sizes,
  every `0x07061000` is exactly `12 + 4n`, and every reference is in range.
- Classes are 0..6, states are `0x00/0x10/0x20/0x30` (`0x40` is never saved), and IDs are unique.
- Character positions are inside the grid (0 ≤ x < 4W, 0 ≤ z < 4L).
- Child order is always `0100, 0200*, 0300, 0400`.

| sample | W×L | terrain heights | water cells | chars (types) | flying / ground animals | citizens | char / car / node / tvehicle refs set | routes |
|---|---|---|---|---|---|---|---|---|
| saves/Cim2 | 1280² | −2048..10792 | 176 401 | 1576 (17) | 6 seagull / 156 cow+pig | 32 520 | 1414 / 1677 / 154 / 76 | 341 |
| saves/berlin | 1280² | −1536..1792 | 153 151 | 5527 (17) | 259 / 36 | 30 255 | 5232 / 1352 / 2199 / 990 | 5321 |
| saves/vienna | 1024² | −2433..8676 | 121 132 | 2165 (17) | 107 / 21 | 16 572 | 2037 / 1563 / 270 / 259 | 1460 |
| addons/germany/cologne | 768² | −1536..2310 | 78 214 | 0 | 0 / 0 | 0 (header only) | — | 0 |
| metro scenario01 | 1280² | −1536..1792 | 153 156 | 1398 (17) | 259 / 36 | 30 255 | 1103 / 1872 / 21 / 20 | 67 |
| metro scenario02 | 768² | −1536..2183 | 122 281 | 638 (14) | 0 / 0 | 12 775 | 638 / 513 / 0 / 0 | 0 |
| metro scenario05 | 1024² | −1536..3840 | 401 177 | 845 (16) | 0 / 25 | 11 900 | 820 / 1333 / 20 / 10 | 138 |
| metro scenario11 | 1024² | −2433..8676 | 121 062 | 1728 (31) | 110 / 21 | 11 749 | 1597 / 999 / 536 / 438 | 1375 |

The other metro scenarios (03, 04, 06-10, 12) look the same and are all clean. The plain city maps (amsterdam,
berlin, helsinki, tutorialcity, vienna, cologne) have empty `0x07050000` (40 B: header 0/0, empty `0100`, `0300`/`0400` with n=0)
and a header-only `0x07070000` (144 zero bytes).

**Terrain flag distribution** (second u16): Cim2 top values are `0x0000` 549 876, `0x1f00` 372 717, `0x2000` 158 006,
`0x0001` 67 575, `0x0002` 60 365, `0x0003` 46 761, `0x1000` 40 689 and `0x0020` 31 634. Per-bit counts for Cim2:
`0x1` 267k, `0x2` 204k, `0x4` 139k, `0x8` 108k, `0x10` 26k, `0x20` 91k, `0x100..0x800` 434k each, `0x1000` 507k,
`0x2000` 176k.
- Bits `0x80`, `0x4000` and `0x8000` are never set in any sample, which fits their transient/unused status.
- `0x40` appears only in maps with pending timeline work.
- `pavement` and `field` are never set together.
- The cover quadrant nibble takes only the values 0, `0xf`, `0x3`, `0x6`, `0x9`, `0xc` (plus a few dozen 3-quadrant
  values).
- Cover bits without `0x1000` are rare (4–5k cells per map).
- Water cells above −0x141: 4–7% of water cells. The rule that dries them out runs only under the height brushes.
- Pavement style (bits 0-2) in Cim2: 0 (54k), 5 (38k), then 6, 2, 3, 7, 1, 4. Field style: 0..3 only.

**Character checks:**
- `anchor ≠ 0` exactly for the animals (Cim2: 1414 humans with 0, 162 animals with a value).
- Heading ∈ [0, 2π].
- `reservedCell == (x,y,z)` for 85-95% of characters; the rest are −1 or the next cell.
- `|position − f(x,z)|` stays within one cell for every pedestrian; only flying birds differ (up to 10 units).
- `flags`: `0x1` dominates; `0x9` (seated) and `0x0` occur.
- The `0x07050300` list holds only `seagull` and `0x07050400` only `cow`/`pig`.

**Citizen cross-references** (`crossref.mjs`):
- Every citizen's character uses a model whose name contains the citizen's class (Cim2 1414/1414, berlin 5232/5232,
  …). This confirms `class = flags & 0xf` and the 1-based character index.
- No character is referenced twice, and no citizen points to an animal.
- Homes resolve to residential templates (`cityhouse*`, `apartment*`, `tenement*`); workplaces to `officebuilding*`,
  `college`, `toolfactory`, `government-offices`; targets to `shoppingcenter`, `deptstore`, `railway-terminal`, homes.
  This confirms that item references are 1-based in `0x07020210` order.
- Classes 4-6 (tourist, pensioner, unemployed) never have a workplace.
- `carModel` is −1 for 85-95% of citizens; `charModel` takes only a few values per map (e.g. 2/3 in era-3 maps).
- `waitTime` is 0..511, `cycleTimer` 0..600.
- `tripQueue` values: 0, 1, 66/67/68 (work/shopping/leisure, then home), `0x1102`, ….

**Line references** (`lines.mjs`, `nodes.mjs`):
- The number of `0x07060220` records across all lines equals `0x07060000` header word 3, and the number of
  `0x07060210` records equals word 2. Cim2: 19/2; berlin: 67/76; vienna: 38/18; scenario11: 43/28.
- Every citizen `lineNode` and every `0x07061000` node resolves to a `0x07060220` record whose stop item (record
  word 0, a 1-based item index) is a stop or station template: `stop_tram01_small`, `station_metrou0a`,
  `stop_bus02_avenue`, ….
- `lineNode` and `transportVehicle` are never both set (0 of 32 520 / 30 255 / 16 572 / 11 749 citizens). A citizen
  either waits at a node or rides a vehicle.

## 5. Open questions

1. **Terrain bit 4 (`0x10`)**: it is set under BLOCK and bridge/tunnel-type items. No reader was found to name it
   precisely (placement blocker? tree suppression?). Bit 7 (`0x80`) has no writer.
2. **`DAT_10166450`**: the 16-entry table that maps the cover-quadrant nibble to a 2-bit-per-quarter-cell walk mask.
   It was not dumped. A reimplementation that rebuilds `grid+0x1c8` needs it, and also `FUN_100e7f30`/`FUN_100e7d10`
   for multi-layer walk graphs.
3. **Character type-table order** follows a pointer-keyed map and so is non-deterministic. A writer may use any
   order, as long as `typeIndex` matches.
4. **Citizen `+0x400` flag and state `0x40`**: the meanings are inferred from one or two call sites.
   `cycleTimer`'s exact role in the day cycle (`FUN_100cbed0`/`FUN_100ceee0`) was not traced.
5. **Line-node / transport-vehicle numbering**: the lines writer (`0x1011a9f0.c`, maps `param_6`/`param_5`) numbers
   nodes line by line along each circular list from `line+0x148`, and vehicles along each line's vehicle vector. It
   writes `0x07060220`/`0x07060210` in the same order. The counts and the stop-type check (§4) confirm this. Whether
   the lines *loader* (`FUN_1011b110`) builds its arrays in the same order should be confirmed by the lines/timeline
   document.
6. **The `0x07030100` movement chunk** is opaque here. Characters always carry exactly one, written by `FUN_100e1a90`
   and read with mode 8 by `FUN_100e1fb0`.
7. The engine's `0x07050200` loader does not check that `typeIndex < typeCount` (it reads past the array). It also
   does not check the car-model index on load. A reimplementation should validate both.
