# Cities in Motion save and map format

Status: **every section is decoded.** `tools/gridparse.mjs` parses all of them and checks exact chunk consumption,
header counts and every cross-section reference. It reports **0 issues on 150 files**: your save, the shipped Berlin
and Vienna saves, and every `.grid` and `.timeline` in the game's 24 archives. A few individual fields are still
unnamed (listed in the sub-documents). Archives are handled by `tools/gstool.mjs`.

| Document | Covers |
|---|---|
| this file | container, text parts, grid header, terrain, network |
| [vehicles-movement.md](vehicles-movement.md) | AI traffic `0x0704`, movement/path state `0x0703` |
| [terrain-characters-citizens.md](terrain-characters-citizens.md) | terrain bits, characters `0x0705`, citizens `0x0707`, planned routes `0x07061000` |
| [lines-timeline.md](lines-timeline.md) | transport lines `0x0706`, timeline `0x0708` |

```
node tools/gridparse.mjs summary <map.grid|map.timeline> ...      counts and validation
node tools/gridparse.mjs dump <file> <section> [limit]            records as JSON (header, types, items, names, ...)
```

## 1. Container

A savegame is a GS10 archive (format: [`../REPORT.md`](../REPORT.md) §2) at `Documents\Cities In Motion\saves\<name>.gs`.
It has no mount chunk and no trailer comment, so the engine mounts it as the folder `userdata/saves/<name>/`.

Saving (`saveGameData`, `metro/scripts/main.script:306-336`) writes every part into the RAM file system (`memory/…`)
and then bundles them with `writePackage("userdata/saves/<name>.gs", files)`:

| Entry | Written by | Format | When |
|---|---|---|---|
| `game.campaign` | `MAIN.$campaign.saveCampaign` | script source (variable dump) | always |
| `level.scenario` | `MAIN.$scenario.saveScenario($scenario)` | script source | in game |
| `map.scenario` | `saveScenario($location)` | script source | in game, if the map has a location layer |
| `default.scenario` | `saveScenario($default)` | script source | in game, if present |
| `gamedata.script` | `MAIN.$playerData.saveData` | script source (~90 economy/player variables) | in game |
| `map.grid` | `MAIN.$grid.saveToFile` (native) | binary chunk tree, family `0x07` | in game |
| `map.timeline` | `MAIN.$timeline.saveToFile` (native) | binary chunk tree, `0x0708` | in game |

Outside a running game (`$campaign.$ingame` false) the archive holds only `game.campaign`.

Loading (`loadSavedCampaign`, `main.script:209-219`) points the file-name globals at
`userdata/saves/<name>/…` and loads them through the normal campaign/scenario path.

User-made maps from the editor (`saveMapData`, `main.script:261-291`) are separate files in `userdata/maps/`:
`<name>.grid`, `<name>.timeline`, `<name>.dds` (minimap texture) and `<name>.scenario` (location setup, current date,
cell size, short description).

## 2. Text parts

The script-source parts are written by `exportVariablesTo` / `saveScenario`. `exportVariablesTo` uses the target
script as a template: it rewrites or appends `$name = value;` lines (CRLF). Values are exported as script literals
(numbers, strings, arrays, maps). Objects that cannot be exported become `null /* cannot export object */`.
Loading a part executes it as a script, so the text parts are both data and code. Scenario coroutines are saved as
`[functionName, param]` pairs and restarted on load. Details: [`../handlers/batch-C.md`](../handlers/batch-C.md).

## 3. `map.grid`

All integers little-endian. Each structure is a chunk `u32 tag, u32 size, body`. Two record formats changed during
development and the loader still reads the older form (the writer emits the newer one): network items
`0x07020200` → `0x07020210`, street names `0x07020300` → `0x07020310`, and timeline names `0x07081070` → `0x07081071`.

Some values that look like tags in the loaders are not chunk types. `0x07020211`, `0x07040301`, `0x07050001` and
`0x07050301` are only the split values of compiled `switch` comparisons, and `0x07070707` is a mask-table entry.
None occurs in any file. (Earlier versions of the report listed some of them as format variants; that was wrong.)

```
0x07000000  grid
  header (24 B): u32 width, u32 length, f32 cellSize (3.0), u32 bucketShift (4), u16 heightBase (0x0200), u16 0, u32 0
  0x07010000  terrain            width×length cells × 4 B
  0x07020000  network            every placed item: roads, tracks, stops, buildings, props, trees
  0x07040000  vehicles           AI traffic: cars, trains, aircraft, boats    -> vehicles-movement.md
  0x07050000  characters         visible pedestrians and animals              -> terrain-characters-citizens.md
  0x07060000  lines              player transport lines, stops, vehicles      -> lines-timeline.md
  0x07070000  citizens           the simulated population                     -> terrain-characters-citizens.md
```

Sections appear in this order, and later sections reference earlier ones by **1-based index in file order**:

* line stops point at network items, and line vehicles at AI vehicle records;
* citizens point at characters, AI vehicles (their car), line stops, line vehicles and network items (home,
  workplace, trip start, target);
* AI-vehicle path nodes are `(item << 8) | lane`, or inline air-lane nodes.

Units: positions are grid cells (world = `(cell − size/2) × cellSize`); heights are in 1/256 cell. A grid flag
(`+0x24e`) makes the loader skip vehicles, characters and citizens. Base city maps (`metro/maps/*.grid`) are the
1920 state with empty simulation sections; the timeline then grows the city.

### 3.1 Terrain `0x07010000`

Per cell, row-major: `i16 height` (1/256 cell; the loader adds `0x200 − heightBase`; ground level in shipped maps is
512 = 2.0 cells) and a `u16` of flags:

| Bits | Meaning |
|---|---|
| `0x0007` | ground-style variant (pavement, field or wild-ground texture) |
| `0x0008` | pavement |
| `0x0010` | blocked under block-type items (meaning uncertain) |
| `0x0020` | field |
| `0x0040` | pavement painted in the timeline preview |
| `0x0f00` | cover quadrants under covering buildings; recomputed and cleared on load (transient) |
| `0x1000` | ground locked by an item |
| `0x2000` | water |
| `0x4000`, `0x8000` | selection highlight (transient, never in files) |

So the common value `0x1f00` is locked and fully covered ground, `0x1f08` is the same on pavement, and `0x2000` is open
water.

### 3.2 Network `0x07020000` (decoded, validated)

| Part | Layout |
|---|---|
| body header | `u32 typeCount, u32 itemCount` |
| `0x07020100` types | `typeCount` NUL-terminated template `idName`s (e.g. `road_small_diag_crossing`), padded once at the end to 4 bytes |
| `0x07020210` items | `itemCount` × 16 B (below); `0x07020211` is a newer variant; `0x07020200` is an old 8-byte format |
| `0x07020310` names | one entry per item: UTF-8 NUL-terminated name padded to 4 bytes; unnamed items are 4 zero bytes |
| `0x07020300` names (old) | one Latin-1 NUL-terminated string per item, no per-entry padding, table padded once at the end (used by the 10 older campaign scenario maps) |

Item record (16 bytes), indexed 0-based in file order:

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | bits 0-13 type index into `0x07020100`; bits 14-15 orientation (bit 14 swaps width/length) |
| 2 | i16 | x (cell) |
| 4 | i16 | y (1/256 cell) |
| 6 | i16 | z (cell) |
| 8 | u32 | parent item index + 1 (0 = none); the loader creates parents first |
| 12 | u32 | bits 0-7 variation (for item kind 0x17 a different sub-value); upper bits persisted flags, mask `0x72500000` |

Writer `FUN_100c3840`, loader `FUN_100bd5a0`. Validated: Cim2 save 723 types / 87 244 items / 8 188 names, Cologne map
481 / 27 162 / 2 603, every chunk consumed exactly, all type and parent indices in range.

### 3.3 Other sections at a glance

Full field tables are in the sub-documents.

| Section | Structure |
|---|---|
| vehicles `0x07040000` | `{modelCount, count}`; model names; per vehicle an 88-byte record (model, carriage links, speed, flags, current/previous path node), a ring of path-node refs, optional inline air-lane nodes `0x07030200` (84 B), and a movement chunk `0x07030100`; then lists of trains, aircraft and boats (`0x07040300/0400/0500`) |
| movement `0x07030100` | `{stepCount, nibbleCount, f32 travelCost, cursor, graphGeneration}` and 4-bit step codes (0–7: take link code+1; 8–15: repeat link 0 code−7 times); the decoded steps equal `stepCount` in every record |
| characters `0x07050000` | `{typeCount, count}`; type names; 80-byte records (position in quarter cells, height, velocity, heading, flags) each with one movement chunk; lists of flying and ground animals |
| lines `0x07060000` | `{modelCount, lineCount, vehicleCount, stopCount}`; vehicle model names; per line a 36-byte header (type bus/tram/metro/boat/helicopter, ticket price, passenger total, closed flag), then 20-byte stops and 60+4n-byte vehicles |
| citizens `0x07070000` | 144-byte header (count plus walk/drive/travel/can-travel/price-too-high counts per class); 64-byte records (class, state, visit type, mood, references) with an optional planned route `0x07061000` |

## 4. `map.timeline`

The timeline is the editor-recorded growth of the city, replayed from the 1920 base grid with a 182-day preview window.

* `0x07080000 {dayCount, currentDay}` holds day groups `0x07081000 {day, 0}`. Days count from **1 Jan 1920**, not
  the absolute day numbers `mkdate` returns.
* Each group holds actions applied in reverse file order:

| Tag | Action | Body |
|---|---|---|
| `0x07081010` | group | 32-byte label + child actions |
| `0x07081020` | create object | 44-byte object reference |
| `0x07081030` | create object with parent | 2 object references (88 B) |
| `0x07081040` | delete object | object reference |
| `0x07081050` | brush (paint yards/fields, level/shift/soften terrain) | `{type, radius, count, strength}` + count × `{x, z}` |
| `0x07081060` | water on/off | `{x, z, on}` |
| `0x07081071` / `0x07081070` | name an object | object reference + UTF-8 / Latin-1 string |
| `0x07081080` | set variation | object reference + u32 |

An object reference is `{char name[32], i16 x, i16 y, i16 z, u16 orient, u32 flags}`, so actions match objects by
value, not by index. Berlin's timeline runs from 1924-05-01 to 2017-01-01.

## 5. Validation

`tools/gridparse.mjs` and the per-section scripts written by the decoders agree on every count. Highlights:

* 337 of 337 line stops point at stop, station, dock or heliport items.
* AI-vehicle movement chunks decode to exactly `stepCount` steps in every record.
* Every citizen's character model matches the citizen's class.
* Homes resolve to residential buildings and workplaces to offices or factories.
* Timeline days are strictly ascending, and `dayCount = last day + 1` in every file.

Remaining unknowns, all individual fields, not structure:

* a few line, line-vehicle and stop fields;
* some node and citizen flag bits;
* the terrain `0x10`/`0x80` bits;
* the link order needed to replay a saved path step by step.
