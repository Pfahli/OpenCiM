# Path network graph and route search

How `GSystem.dll` builds its movement networks and searches them. There are **three separate networks and three
separate search functions**. None of them is A* in the textbook sense.

| Network | Built from | Used by | Search | Result |
|---|---|---|---|---|
| Lane graph (`gsGridGraph`, `grid+0x208`) | the named paths of every placed item (`addPath`/`setPathFlags`), plus generated water lanes | cars, trams, metro, AI trains, boats, aircraft, transit-line vehicles | `FUN_100e5fe0`: goal-biased Dijkstra over directed lane segments | packed "which outgoing link" codes in a 0x18-byte path cursor |
| Walk surface (per-cell 4×4 walk-code grids on the ground and on extra layers) | item walk maps and pavement flags | pedestrians (citizen "characters") | `FUN_100e6cf0`: two-phase Dijkstra (cell corridor, then quarter-cell path) | packed direction codes in the same cursor type |
| Transit graph (`gsTransportLines`, `grid+0x218`) | (line, stop) nodes of active lines, transfer links by stop catchment | citizen mode choice and route following, `routeExists` | `FUN_1011a200`: Dijkstra with randomized costs | an array of (line, stop) nodes |

A citizen trip combines them: walk search to the first stop, transit search for the ride legs, walk search for each
transfer and the last leg; or lane-graph search for a car leg with straight-line walking at both ends (section 7).

Conventions: *[obs]* = read directly in the pseudocode or bytes; *[inf]* = inferred. Citations are
`decompiled/GSystem-v2/functions/<file>:<line>`. "Cell" = grid cell; "q-cell" = quarter cell (4×4 per cell);
height units are 1/256 cell. `cellSize` = world units per cell (`grid+0x154`, copied to `graph+0xe4` and
`lines+0xec` by `0x100b7bf0.c:14-16`). World x = (cellX − gridWidth/2)·cellSize, with the origin at the map centre.

---

## 1. Data structures

### 1.1 `gsGridGraph` (0x170 bytes, vtable `0x1016635c`)

Constructor `0x100e5da0.c`, resize `0x100e3530.c`, clear `0x100e3170.c`, destructor `0x100e5860.c`.

| Offset | Type | Name | Meaning | Evidence |
|---|---|---|---|---|
| +0xcc | gsGrid* | grid | owner | `0x100e5da0.c:24` [obs] |
| +0xd0/+0xd4 | int | width, length | grid size in cells | `0x100e3530.c:24-28` [obs] |
| +0xd8 | int | bucketShift | 3 → buckets of 8×8 cells | `0x100e5da0.c:28`; resize called with 3 from `0x100b7e10.c:83` [obs] |
| +0xdc/+0xe0 | int | bucketsX, bucketsZ | `(size + 2^shift − 1) >> shift` | `0x100e3530.c:29-34` [obs] |
| +0xe4 | float | cellSize | default 3.0, set to `grid+0x154` | `0x100e5da0.c:31`, `0x100b7bf0.c:15` [obs] |
| +0xe8 | u32 | generation | incremented by node removal if a search ran since the last increment | `0x100e5950.c:28-30` [obs] |
| +0xec | u8 | searchedSinceBump | set to 1 by every lane search | `0x100e5fe0.c:53` [obs] |
| +0xf0 | Node** | buckets | bucketsX×bucketsZ singly linked lists (link `node+0x08`), newest first | `0x100e4dd0.c:353-356` [obs] |
| +0xf4 … +0x160 | 10 × `std::set<Node*>` (12 bytes: comparator, head, size) | endpoint sets | see 2.5 | `0x100e5da0.c:34-52`, `0x100e47a0.c` (tree insert keyed on pointer) [obs] |

The class has no own simulation tick: apart from the destructor (slot 0) the only override is slot 29 `0x100e3640`,
which clears the graph and frees the buckets [obs: `docs/rtti-classes.json`, `0x100e3640.c`]. It registers one
event, `onPathError` (id 6) [obs `0x100e1b10.c`], which the pedestrian search fires (5.4).

### 1.2 Lane segment node (0x8c bytes, ctor `FUN_100e13d0`)

One node = one directed path piece (a lane, track, water lane or air lane between two points). Bytes +0x24…+0x77
are what the save format stores inline (`docs/save-format/vehicles-movement.md` §2.6).

| Offset | Type | Name | Meaning | Evidence |
|---|---|---|---|---|
| +0x00 | float | searchKey | heap key during a search: −g | `0x100e5fe0.c:81,214` [obs] |
| +0x04 | Link* | successors | singly linked `{Node* node; Link* next}` (8 bytes), order = insertion order | `0x100e4dd0.c:178-196,233-251` [obs] |
| +0x08 | Node* | bucketNext | graph bucket list | `0x100e4dd0.c:355` [obs] |
| +0x0c | Node* | ownerNext | next node of the owning item (`item+0x24` list) or of the free-node list (`grid+0x254`) | `0x100ebb20.c:99,223`, `0x100c1840.c` [obs] |
| +0x10 | Node* | queryNext / parent | result link of spatial queries; during a lane search, the parent | `0x100e2070.c:65-66`, `0x100e5fe0.c:98` [obs] |
| +0x14 | Node* | touchedNext | lane search: list of discovered nodes, for cleanup | `0x100e5fe0.c:84,189,277` [obs] |
| +0x18 | Vehicle* | reservedBy | vehicle holding the segment | `0x1010de50.c:247-249` [obs] |
| +0x1c | Vehicle* | reservedBy2 | second (queued) reservation | `0x1010de50.c:251-257` [obs] |
| +0x20 | Link* | crossings | conflict list (segments that geometrically cross or touch this one), mutual | `0x100e4dd0.c:322-343` [obs] |
| +0x24 | u32 | flags | `GRID_PATH_*` bits; low nibble = search scratch (link index); 0x80 deleted; 0x800000 sound variant; 0x1000000 blocked; 0x2000000 dynamic; 0x4000000 excluded from search (water lane under an item) | `0x100ebb20.c:101-104`, `0x100e9730.c:442-451`, `0x100e5fe0.c:97` [obs] |
| +0x28/+0x2c | int | cellX, cellZ | bucket key (2.2) | `0x100e4dd0.c:72-76` [obs] |
| +0x30 | vec3 | p0 | start, world units | `0x100ebb20.c:105-109` [obs] |
| +0x3c | quat | q0 | start orientation; forward = q·(0,0,−1) | `0x100ebb20.c:110-115`; `DAT_10198480 = (0,0,−1)` set in `0x10153150.c` [obs] |
| +0x4c | vec3 | p1 | end | `0x100ebb20.c:116-120` [obs] |
| +0x58 | quat | q1 | end orientation | `0x100ebb20.c:121-125` [obs] |
| +0x68 | float | length | straight chord \|p1−p0\| (not the curve length) | `0x100ebb20.c:126-127` [obs] |
| +0x6c | float | speedLimit | 2.1 | `0x100ebb20.c:171-221` [obs] |
| +0x70 | float | curveFactor | 2.1 | `0x100ebb20.c:167-170` [obs] |
| +0x74 | float | curvature | signed, ×0.05; used for vehicle roll | `0x100ebb20.c:182` [obs] |
| +0x78 | int | refCount | starts at 1; `FUN_100e1e90` releases | `0x100e13d0.c:35`, `0x100e1e90.c` [obs] |
| +0x80 | u32 | changedGeneration | stamped with `graph.generation` when a successor is removed | `0x100e5950.c:150` [obs] |
| +0x84 | int | inDegree | number of predecessor links | `0x100e4dd0.c:196,251`, `0x100e1450.c` [obs] |

### 1.3 Path cursor (0x18 bytes, ctor `FUN_100e17e0`)

Shared by the lane search and the walk search; fully described in `docs/save-format/vehicles-movement.md` §2.5.
`{u32* codes; int stepCount; int nibbleCount; float cost; u32 cursor; u32 generation}`. Codes are 4 bits, 8 per
word, code *i* at bits `4·(i&7)` of word `i>>3`; `cursor` counts in 1/8-code units [obs `0x100e1810.c`,
`0x100e18a0.c`]. Finished when `nibbleCount <= cursor>>3` [obs `0x100e1a70.c`].

### 1.4 Walk surface layers

The ground layer is embedded in the grid at `grid+0x1c0`; extra layers (bridges, elevated or underground floors)
hang in a list per grid slot at `slot+0x18` [obs `0x100e1b30.c:16-36`]. Each layer has the same shape:

| Index | Offset in layer | Meaning |
|---|---|---|
| [0] | +0x00 | next layer in the slot list |
| [1] | +0x04 | per cell `{i16 height; u8 ?; u8 flags}`, 4-byte stride; for extra layers the low nibble of byte 3 must be ≠ 0 for the cell to exist |
| [2] | +0x08 | per cell u32 walk codes, 2 bits per q-cell, index `(qx&3) + (qz&3)·4`, shift ×2 |
| [3] | +0x0c | per cell u32 occupancy bits (pedestrian reservation map, `docs/save-format/terrain-characters-citizens.md`) |
| [4] | +0x10 | per cell u32 search marks (scratch for the walk searches) |
| [5] | +0x14 | per cell u32 ground tint (rendering, not navigation) |
| [6],[7],[8] | +0x18,+0x1c,+0x20 | x0, z0, row stride; cell index = `(z − z0)·stride − x0 + x` |

Walk codes (meaning from `terrain-characters-citizens.md` §2.1): 0 = not walkable, 1 = road crossing,
2 = pavement, 3 = open ground. Item placement writes them from the template walk map (`template+0x2a8`) and the
pavement flag [obs `0x100e9730.c:329-345`].

Where the walk codes come from [obs]: a template's `<stem>.bmp` (`gsGridObject::loadFromFile` `L:0x0817f650.c:65-260`)
has 4×4 pixels per cell; a pixel with R = 255 gives code 2 (pavement), else G = 255 code 3 (open ground), else B = 255
code 1 (road crossing), else 0. The loader stores the cell words in all four orientations (`+0x274`, the `-y.bmp`
heights likewise in `+0x278`), and the item uses copy `[orientation]` (`item+0x44`). Copy 0 is the bitmap as stored,
copy 1 maps world cell (x', z') to bitmap cell (W−1−z', x'), copy 2 to (W−1−x', L−1−z'), copy 3 to (z', L−1−x') (W, L:
the template's width and length; `gsGridObject::loadSlotData` `L:0x081865b0`, `loadYData` `L:0x08186c20`), with the
q-cells turned along. The maps are thus half a turn off the mesh, which is turned by `(orientation + 2)·90°`. Item placement
merges the word of each covered cell into the ground layer (`0x100e9730.c:329-345`, `FUN_100e7d10`); a template without
a `.bmp` writes 0 (not walkable) under its cover. Items with BRIDGE, TUNNEL or ENABLE_BASE (`& 0xc001`) write extra layers
instead.

Layer lookup [obs]:
* `FUN_100e1b30(graph, x, y, z)` (cell level): the ground layer if its height is within y ± 0xa0 and its walk word is
  non-zero; otherwise the first extra layer in the slot list whose cell exists, whose height is within ± 0xa0 and
  whose walk word is non-zero; else null (`0x100e1b30.c:22-38`).
* `FUN_100e1d40(graph, qx, y, qz)` (q-cell level): same, but tests the 2-bit code of that q-cell
  (`0x100e1d40.c:26-45`).

The tolerance 0xa0 = 0.625 cell is how a pedestrian stays on a bridge deck rather than dropping to the ground below
[inf].

Stamping details [obs `0x100e9730.c:310-431`, `0x100e7f30.c`, `0x100e7d10.c`, `0x100d4de0.c:79-90`]:
* Ground init: `0xffffffff` (open), or `0xaaaaaaaa` on cells with terrain flag 0x8 (pavement) that are not fully
  covered. Water is **not** blocked here.
* Every item except flag 0x800000 stamps each footprint cell for which `cellCover` (0xf00 for any non-diagonal
  footprint) is non-zero. `ENABLE_BASE` is bit 0x1, so all buildings (`0xd`) and platforms take the layered branch.
* The layered branch applies to `flags & 0xc001` or a template with `-y.bmp` data (W `+0x2ac` = L `+0x278`). The
  height is the item y, `getY` per cell corner for slope flags 0xf0 unless BRIDGE/TUNNEL and COVER, plus the `-y.bmp`
  cell value. `getExtraSlot` uses the ground word when all four terrain corners equal that height. Otherwise it uses
  a layer cell of exactly that height, claiming a free one or appending a new layer.
* The merge `FUN_100e7d10(old, new, cover)` works per 2-bit code. 0 stays 0, and 1 stays 1 unless `new` is 0. Codes 2
  and 3 take `new`. A template without `.bmp` merges 0, which blocks the covered cells.
* The ground under a layered item is merged with 0 (blocked) in two cases: the item is COVER, or the item has
  `-y.bmp` data, is not a TUNNEL, and is at most 0x140 above the ground.

Port (`engine/sim_citizens.cpp` `buildWalkGrid`): follows the layered branch with decks per cell (at most 7) and
records each item's own pavement cells as door candidates (§5.5). Deviations:
* Water starts blocked (deliberate, see README.md "Deliberate deviations").
* Ground stamping replaces the cell word instead of merging.
* A non-LOCK_GROUND ground item keeps its own surface height.
* From the ground, the ground stays usable at any height difference.
* Partial (diagonal) covers use cellCover; the quadrant mask DAT_10166450 was not dumped, so the port takes the
  q-cells whose centres lie in the covered quadrants (centres on a diagonal count for both quadrants).

### 1.5 Transit structures

**Line-stop node** (0x70 bytes, ctor `FUN_10118760`). One node per stop of a line, in a circular list. Read it as a
*ride segment*: "on line L, depart stop S (`+0x0c`), arrive at the next node's stop".

| Offset | Type | Name | Meaning | Evidence |
|---|---|---|---|---|
| +0x00 | int | refCount | 1 at creation; a route holds a reference per node | `0x10118760.c:20`, `0x1011a200.c:246` [obs] |
| +0x04 | gsTransportStopController* | stop | stop controller of S (catchment `+0x174`, attractiveness `+0x17c`, type `+0x178`) | `0x10118760.c:44-46` [obs] |
| +0x08 | gsTransportLine* | line | type `+0x16c`, active bit `+0x6c & 0x10000`, quality `+0x174`, ticketPrice `+0x184` | `0x10118760.c:22`, `0x1011a200.c:84-85` [obs] |
| +0x0c | GridItem* | stopItem | S as a grid item (centre = `x + (w>>1)`, `z + (l>>1)` at `+0x2c/+0x38`, `+0x34/+0x40`) | `0x1011a200.c:87-89` [obs] |
| +0x10 | 0x14 bytes | segment | vehicle path S→next stop; `+0x20` = path cursor (1.3) whose `cost` (`+0xc`) is the drive time in seconds | `0x10114200.c:41-96`, `0x1011a200.c:94` [obs] |
| +0x24 | Node* | next | next stop on the line (circular) | `0x1011b8a0.c:13`, `0x10119290.c:21-24` [obs] |
| +0x28 | Node* | parent / queryNext | search parent; also the link of catchment query results | `0x10114560.c:71`, `0x1011a200.c:171,218` [obs] |
| +0x2c | Node* | touchedNext | search cleanup list | `0x1011a200.c:176,223,250` [obs] |
| +0x30 | u32 | flags | 1 temporary (line-editor preview), 2 touched, 4 closed, 8 removed (citizens abort) | `0x10118760.c:29`, `0x1011b8a0.c:14`, `0x1011a200.c:142-143`, `0x100cc990.c:45` [obs] |
| +0x34 | float | sinceVehicle | accumulated dt per line tick | `0x10114f30.c:22` [obs, meaning inf] |
| +0x38 | `std::vector<Node*>` | transfers | begin `+0x3c`, end `+0x40`, cap `+0x44` | `0x10119290.c:43-70`, `0x1011a200.c:182-183` [obs] |
| +0x48 | Node* | bucketNextByDeparture | spatial index by S | `0x10119290.c:29-33` [obs] |
| +0x4c | Node* | bucketNextByArrival | spatial index by next stop | `0x10119290.c:34-39` [obs] |
| +0x50 | Vehicle* | dockedVehicle | vehicle at the stop for this line | `0x100cc990.c:238-240` [obs] |
| +0x54 | float | bestCost | search scratch, −1 = unset | `0x10118760.c:37`, `0x1011a200.c:252` [obs] |
| +0x58…+0x6c | ints | waiting/boarding counters | citizen domain | `0x100cc990.c:225,274,294,342,421-422` [obs] |

**Route** (0x10 bytes, `FUN_100d4500`): `{float cost; int count; int legIndex; Node** nodes}`; nodes in travel order,
from the first boarded ride segment to the last [obs `0x1011a200.c:231-248`, `0x100cc990.c:139-153`].

**Lines spatial index** (`gsTransportLines`): `+0xe0` shift (3), `+0xe4/+0xe8` buckets X/Z, `+0xec` cellSize, `+0xf0`
buckets by departure stop (link `+0x48`), `+0xf4` buckets by arrival stop (link `+0x4c`) [obs `0x10111250.c`,
`0x10119290.c`].

---

## 2. Building the lane graph

### 2.0 The template's path map

The path map (`template+0x29c`, Linux `+0x25c`) is filled from three sources, in this order [obs]:
1. `addPath(name, flags, p0, q0, p1, q1)` in the companion script, which runs before the mesh loads
   (`gsGridObject::method_addPath` `L:0x08180e50`): a raw record, `flags` taken as given, no medium added.
2. The mesh's locators (`.object` chunk 0x02600000), when the mesh has loaded (`gsGridObject::loaded` `L:0x081808b0`,
   `W:0x100f0780`). Locators are iterated in name order; each consecutive pair `(a, b)` whose names are longer than 3,
   have `-` at index 1 and agree in all but the last character of `a` (`strncmp(a, b, len(a) − 1)`) becomes a path
   named `a` from `a` to `b`. A chain `t-a0, t-a1, t-a2` gives the paths `t-a0` and `t-a1`. The flags are the medium of
   the template's item type: 2 → 0x300; 3, 4, 7 → 0x700; 5, 6 → 0x500; 8, 9 → 0x1000; 10–12 → 0x800; 13–16 → 0x2000;
   0x11 → 0x8000; 0x12 → 0x4000; others 0. Positions are the locator positions rotated by the object's load rotation
   (`+0x1b8`, the 45° of the diagonal "b" variants); orientations are `R·loc.rot·Q` with R a quarter turn about −Y.
   A name that already exists is replaced.
3. `setPathFlags(name, flags)` in `onLoaded` (`L:0x08180460`): `flags` without medium bits (`& 0x1000ff00`) replace
   only the non-medium bits, otherwise the whole word. An unknown name prints "Path not found: …".

A stop item (e.g. `stop_tram01_small`) has no locators: its platform node is a `STOP` path of the track item under it
(`tracks/small_basic.script` sets `GRID_PATH_STOP` on `t-a0` and `t-b0`). [obs]

For a reimplementation, the forward direction of a locator path end is `conj(loc.rot)·(−1, 0, 0)` in the Hamilton
convention: along the chord on straight track and along the 45° tangent at the middle locator of `tracks/large_turn`
[obs, port `engine/sim_graph.cpp`].

### 2.1 Nodes from item paths — `FUN_100ebb20(template, grid, &item.nodes, x, y, z, orientation)`

Called whenever an item's paths are (re)built (`FUN_100ecbf0`), skipped for items with flags `0x500000`
(temporary or hidden) or item type 0x15 [obs `0x100ecbf0.c:27-28`]. For every entry of the template's path map
(`std::map<std::string, PathRecord*>` at `template+0x29c`, iterated from `begin()`, so in ascending byte order of the
path name) [obs `0x100ebb20.c:74-82,225`]:

1. Transform: rotation by `(orientation + 2)·π/2` about the vertical axis [obs angle `:78`; axis inf], translation to
   the item centre `((2x − W + w')·cs/2, y·cs/256, (2z − L + l')·cs/2)` with w', l' the template size swapped for odd
   orientations [obs `:68-73,86-90`].
2. New node; `flags = path.flags`, plus `0x800000` if template flag bit 15 is set [obs `:101-104`]. Prepended to
   the item's node list, so the list ends up in **descending** path-name order [obs `:99,223`; order inf].
3. `length = |p1 − p0|` [obs `:126-127`].
4. Bézier with control points `P1 = p0 + q0·(0,0,−L/3)`, `P2 = p1 + q1·(0,0,+L/3)` [obs `:128-133`].
   `k = max(|f(0.1)|, |f(0.5)|, |f(0.9)|)` with `f = FUN_10084dd0` (a curve-deviation measure [inf]),
   clamped to [1, 8] [obs `:134-166`].
5. `curveFactor = 20/k`, ×1.5 if `flags & 0x1000ff00` ≠ 0 [obs `:167-170`].
6. `speedLimit = max(2·curveFactor, 1.5)`, capped by class `flags & 0x1000ff00` [obs `:171-221`]:

| Class | Cap |
|---|---|
| 0x1000 train, 0x2000 metro, 0x700 express road | 10 |
| 0x500 large road | 8 |
| 0x4000 air lane | 6 with `IGNORE_TRAFFIC`, else 3 |
| anything else (0x300 small road, 0x800 tram, 0x8000 water, walk) | 6 |

7. Air lanes with an endpoint below y = 80 get `curvature = 0` [obs `:219-221`].
8. Insert into the graph (2.2–2.5) [obs `:224`].

### 2.2 Bucketing

`cellX = ftol((p0.x + p1.x)/cellSize + width + 0.5) >> 1`, same for z: the cell of the segment midpoint
[obs from bytes `0x100e4df5`–`0x100e4e4b`: `fld [p0.x]; fadd [p1.x]; fdiv [graph+0xe4]; fild width; fstp t; fadd t;
fadd 0.5; call __ftol; sar 1`]. Bucket = `(cellX >> 3, cellZ >> 3)`, clamped into the bucket grid; the node is pushed at the
**head** of the bucket list [obs `0x100e4dd0.c:77-87,353-356`].

Spatial query `FUN_100e2070(graph, x, z, w, l, mask)`: all nodes with `cellX ∈ [x−3, x+w+3]`,
`cellZ ∈ [z−3, z+l+3]` and `(flags & mask) == mask`, returned as a list through `+0x10`, ordered by bucket row
(z ascending), bucket column (x ascending), then bucket-list order [obs `0x100e2070.c:22-70`]. This order decides
adjacency order and every "random among candidates" pick below.

### 2.3 Successor links — `FUN_100e4dd0(graph, N)`

Two nodes are compatible when `((A.flags ^ B.flags) & 0x1000f900) == 0`: same WALK bit, same WATER, RUNWAY, METRO,
TRAIN, TRAM bits and same road bit 0x100 [obs `0x100e4dd0.c:166,221`]. Bits 0x200/0x400 are not compared, so small,
large and express roads all connect to each other [inf from the mask].

"Air" below means `(flags & 0x1000f900) == 0x4000` [obs `:89`].

**Predecessors** of N: candidates C from the query around the cell of `p0` (`ftol(p0/cellSize) + size/2`; window
0×0 → ±3 cells; air: 12×12 offset by −6 → ±9 cells) [obs from bytes at `0x100e5054`–`0x100e507d` and
`0x100e4f26`–`0x100e4f41`]. Link C → N if:
* compatible, and
* `dot(fwd(N.q0), fwd(C.q1)) > 0.1` [obs `:167-170`], and
* `|C.p1 − N.p0|² < T`, where T = 0.01 (0.1 units); if C has `CONNECT_FROM` or N has `CONNECT_TO`, T = 20
  (4.47 units), or 500 (22.4 units) for air [obs `:171-177`; `FUN_100834f0` returns the squared distance].

N is **appended at the tail** of C's successor list (skipped if already present), and `N.inDegree++`
[obs `:178-196`].

**Successors** of N: candidates C from the query around the cell of `p1`; same tests with roles swapped
(`dot(fwd(C.q0), fwd(N.q1)) > 0.1`; `|N.p1 − C.p0|²`; `CONNECT_FROM` on N or `CONNECT_TO` on C). C is appended to
N's list in query order, `C.inDegree++` [obs `:202-256`].

So adjacency order is the order of insertion: links found when N was inserted come first, in query order; links
added later by newer nodes are appended. Saved paths store link indices (section 4.4), so replaying a save needs the
same insertion order [inf, agrees with `vehicles-movement.md` open question 1].

### 2.4 Crossing (conflict) links

Only for N without `PARK` [obs `:257`]. Candidates: query `(cellX−1, cellZ−1, 2, 2, 0)` → cells ±4 [obs `:267`];
skip C if C is a successor of N, N a successor of C, or C has `PARK` [obs `:269-276`]. **No medium check**: lanes of
different classes (road and tram, road and walk path) can conflict [obs].

Each path is approximated by two chords, `p0→M` and `M→p1`, where M is the Bézier point at t = 0.5 [obs `:258-266,
277-287`]. `d = min` of the four chord-to-chord squared distances (`FUN_10085240`) [obs `:288-316`]. If
`d ≤ 0.25` (0.5 units), or `d ≤ 5.0` (2.24 units) when either node has `CONNECT_TO|CONNECT_FROM`, then N and C are
pushed at the **head** of each other's crossing list [obs `:317-343`]. If N is a `BLOCKER` and C is reserved, the
reserving vehicle gets flags `|= 5` (path invalid) [obs `:344-347`].

Crossings are what `FUN_100e14b0` ("may enter") checks: a node may be entered unless it is reserved, it is
`BLOCKER`, it has 0x1000000 without `LANE_CHANGE` (unless forced), or a crossing node is reserved by another
vehicle, blocked or a `BLOCKER` [obs `0x100e14b0.c:14-32`]. Traffic behaviour itself belongs to the vehicle-movement
write-up.

### 2.5 Endpoint sets

Nodes with `START`/`END` of some classes are also inserted into the graph's sets, which the AI spawners sample
uniformly with `rand() % size` [obs `0x100e4dd0.c:88-154`; consumers `0x1010ae20.c:85-140`, `0x1010b430.c:87-111`,
`0x1010c190.c:85-92`, `0x1010ce70.c:48-64`]:

| Set (head, size) | Content | Consumer |
|---|---|---|
| +0xf4 (+0xf8, +0xfc) | train START, not PARK | AI trains |
| +0x100 (+0x104, +0x108) | train END, not PARK | AI trains |
| +0x10c (+0x110, +0x114) | train START, not STOP | AI trains (non-stopping) |
| +0x118 (+0x11c, +0x120) | train END, not STOP | AI trains (non-stopping) |
| +0x124 (+0x128, +0x12c) | air START, not STOP | arriving aircraft |
| +0x130 (+0x134, +0x138) | air END, STOP | arriving aircraft |
| +0x13c (+0x140, +0x144) | air START, STOP | departing aircraft |
| +0x148 (+0x14c, +0x150) | air END, not STOP | departing aircraft |
| +0x154 (+0x158, +0x15c) | water START | boats (start) |
| +0x160 (+0x164, +0x168) | water END | boats (end; must be ≥ 200 units from the start, `0x1010ce70.c:70`) |

### 2.6 Removal and path invalidation — `FUN_100e5950(graph, N)`

1. If a search ran since the last bump (`+0xec`), `generation++` [obs `:28-30`].
2. Remove N from its endpoint set and its bucket [obs `:31-110`].
3. Remove N from each crossing partner's list; free its own [obs].
4. For every candidate predecessor (query around `p0`, ±3 cells, air ±9) within squared distance 20.1 (air 500.1)
   of `N.p0`: set `C.changedGeneration = generation` and unlink N from C's successors [obs `:118-160`].
5. Free N's successor links (decrementing their in-degree) [obs].

A vehicle's path is stale when a node it uses has `changedGeneration` newer than the path's `generation`
(`docs/save-format/vehicles-movement.md` §2.5). Adding nodes never invalidates paths [obs: `FUN_100e4dd0` does not
touch `generation`]. When an item is rebuilt, its old nodes get flag 0x80, and vehicles standing on them are moved to
a new node with the same endpoints (within 0.2 units, `FUN_100e25b0`), or get flags `|= 5` if none exists
[obs `0x100ecbf0.c:16-40`, `0x101068f0.c:20-74`, `0x100e25b0.c`].

### 2.7 Generated water lanes

`FUN_100c1840` (run after the terrain is loaded or edited) first deletes the previous free nodes (`grid+0x254`),
then lays a lattice of one-way WATER (0x8000) lanes over every 4×4-cell block that is all water (cell flag 0x2000)
with water neighbours: y = −1.25·cellSize, speedLimit 6, curveFactor 20; lane direction alternates with block parity;
lanes at the map border get `START` or `END`; corner connectors are built with `FUN_100e1730` (start of one lane,
end of another) [obs `0x100c1840.c:76-240`, `0x100e1730.c`; the exact lattice geometry is only skimmed, inf]. Items
under the water line mark water nodes in their footprint with 0x4000000, which removes them from searches
[obs `0x100e9730.c:441-453`, `0x100e5fe0.c:97`].

---

## 3. Lane graph lookups

| Function | What it does | Evidence |
|---|---|---|
| `FUN_100e21e0(graph, x, y, z, randomNear, mask, size)` | `size ≠ 0`: one query of a size×size square around (x, z); a uniformly random node with all `mask` bits, or null. `size == 0`: squares of 0, 8, 16, … 128 until one has a match; then the node minimising `dx² + 16·dy² + dz²` between the node midpoint and the query point (world units, y weighted ×4 in distance). With `randomNear`, a uniformly random node among those with squared distance ≤ 1.5 × the best | `0x100e21e0.c:29-128` [obs] |
| `FUN_100e24f0(graph, item, mask, randomNear, size)` | a uniformly random node with `mask` from the item's own list; if none, `FUN_100e21e0` at the item centre | `0x100e24f0.c:20-40` [obs]; return value used by callers [inf] |
| `FUN_10110c10(line, stopItem, alt)` | stop node of a line: nearest node (size 0) to the stop centre with `STOP | class` (`STOP|ALTERNATIVE_STOP | class` when `alt`); class bus 0x100, tram 0x800, metro 0x2000, boat 0x8000 at y −1.25, helicopter 0x4000 at y −1.25 | `0x10110c10.c:17-48` [obs] |
| `FUN_100e25b0` / `FUN_100e2630` | node with the same / reversed endpoints (≤ 0.2 units) and the same medium | [obs] |
| `FUN_100e1600(node)` | the successor whose chord direction has the largest dot product with the node's (straight on) | [obs] |

---

## 4. Lane search — `FUN_100e5fe0(graph, start, goal, Cursor* out, bool ignoreTraffic)`

### 4.1 Algorithm

A best-first search over nodes ordered by accumulated cost g. The open list is a `std::priority_queue<Node*>` keyed on
`node+0x00 = −g` (max-heap, so the smallest g pops first) [obs `0x100e5f00.c`, `0x100e36c0.c`, `0x100e2c40.c`,
`0x100e2d60.c`]. There is **no heuristic term in the priority**; instead each edge cost is multiplied by a factor that
falls as the edge gets closer to the goal, which makes the search goal-biased. It is not A*.

Important differences from textbook Dijkstra [obs `0x100e5fe0.c:85-219`]:
* A node is closed **when it is first discovered**: its link index is written into the low nibble of its flags and its
  parent is fixed. There is no relaxation or decrease-key. Because the edge cost depends almost only on the target
  node, the first discovery comes from the cheapest popped parent, so this is nearly exact. The one exception is the
  intersection penalty, which depends on the parent [inf].
* The goal test is on pop.
* No iteration, node or distance limit [obs].

### 4.2 Edge cost (entering successor S from popped node P)

```
dz = |S.p0.z − goal.p0.z|      dx = |S.p0.x − goal.p0.x|      dGoal   = max(dx, dz)
az = |S.p0.z|                  ax = |S.p0.x|                  dCentre = max(ax, az)   // world origin = map centre
c  = (S.length / S.speedLimit) · (2.0 + 0.002·dGoal − 0.001·dCentre)
c *= 0.4                         if (S.flags & 0x1000ff00) == 0x700        // express road
c += 4.0                         if S has INTERSECTION (0x10) and P does not
c += 6.0                         if S has LANE_CHANGE (0x20)
if !(S.flags & IGNORE_TRAFFIC) && !ignoreTraffic:
    c += 0.2 if S.reservedBy;  c += 0.2 if S.reservedBy2
```
[obs `0x100e5fe0.c:101-188`; the absolute-coordinate terms checked in bytes at `0x100e613d`–`0x100e6261`:
`fld [S+0x38]; fcomp 0` (no subtraction) for `az`, `ax`; constants 0.002 at `0x10163dd0`, 2.0 at `0x10163dd8`,
0.001 at `0x10159598`, 0.4 at `0x10163d58`, 4.0 at `0x1015e87c`, 6.0 at `0x1015e870`, 0.2 at `0x1015aa30`].

The `−0.001·dCentre` term makes segments far from the map centre cheaper, so routes are pushed around the centre
[inf]. For dCentre > 2000 + 2·dGoal the factor turns negative; no clamp was seen [obs absence; inf effect].

### 4.3 Successor filters

S is considered only if `(S.flags & 0x400000f) == 0` (not yet discovered, not excluded) and (S is the goal or S has
at least one successor) [obs `:97`]. Before pushing a non-goal S, a one-step lookahead [obs `:192-212`]: if S has
exactly one successor, or exactly two of which only one is viable (goal or has successors), and that one is already
discovered or excluded, S is discovered (flag and parent set) but not pushed. The link index counter runs over all
links, including skipped ones [obs `:94,218`].

### 4.4 Result

* Found: `out.cost = Σ length/speedLimit` over the path from goal back to (not including) start, i.e. plain
  travel time without the multipliers or penalties [obs `:223-225`]; `stepCount` = nodes after start [obs `:233`].
  Codes, written backwards from the goal: a node reached by link 1 (the first successor) extends a run; any other
  node emits `linkIndex − 2` (0..7 = take 0-based link code+1); runs emit `8 + (n−1)` for n = 1..8 (follow link 0 n
  times) [obs `:221-275`; decoder `0x100e1810.c:16-45`].
* Direct air path: if `(start.flags & 0x1003f940) == 0x4040` (air lane with STOP and nothing else of the class bits)
  the search is skipped: `stepCount = nibbleCount = −1`, `cost = |start.p1 − goal.p0| / 6`, returns true
  [obs `:71-77`].
* Not found: returns false, no codes; `cost` holds the last popped g [obs `:85-87,89`].
* `out.generation = graph.generation` at the start [obs `:66`]. Scratch flags are cleared through the touched list
  [obs `:277-279`].
* Quirk: the link index is ORed into the low nibble. Index ≥ 10 cannot be encoded correctly, and index ≥ 16 would
  spill into flag bit 0x10 [obs mechanism; whether any node has ≥ 10 successors is unknown].

### 4.5 Callers

| Caller | start → goal | ignoreTraffic |
|---|---|---|
| `0x100d18f0.c:197-199` citizen mode choice (car cost) | random START\|road node of the origin item (or nearest to the parked car) → END\|road node of the destination, near a random PARK spot | 0 |
| `0x100ceee0.c:137,261` citizen car spawn / re-route | same nodes; re-route from the last queued node | 0 |
| `0x10114200.c:41-81` transit line stop-to-stop path | stop node → next stop node; metro, boat and `FIND_ALTERNATIVE` stops try all four primary/alternative combinations and keep the cheapest | 1 |
| `0x1010ae20.c:152` AI trains, `0x1010b430.c:140` / `0x1010c190.c:117` aircraft, `0x1010ce70.c:73` boats | random endpoints from the sets in 2.5 | 1 |
| `0x101152a0.c`, `0x10113e00.c` | transport vehicle re-paths | (see vehicle write-up) |

---

## 5. Pedestrian search — `FUN_100e6cf0(graph, qx0, y0, qz0, qx1, y1, qz1, Cursor* out)`

Coordinates are q-cells (cell·4 + sub-cell); y is the layer height in 1/256 cell. Both endpoints come from the
door-snapping helpers (5.5).

### 5.1 Direction tables [obs `:64-115`]

| dir | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| dx | +1 | 0 | −1 | 0 | +1 | +1 | −1 | −1 |
| dz | 0 | +1 | 0 | −1 | +1 | −1 | +1 | −1 |

Decoder `0x100e18a0.c:43-66` agrees. Edge masks per cell direction (which q-cells touch that cell edge):
dir 0 `0xc0c0c0c0`, 1 `0xff000000`, 2 `0x03030303`, 3 `0x000000ff`. Through-cell tables `T1[prev][dir]`
(`local_d8[0x10..0x1f]`) and `T2[prev][dir]` (`local_d8[0x20..0x2f]`), indexed `dir + prev·4`:

```
T1 = 0c0c0c0c 03030c0c 00000000 0c0c0303   3030c0c0 00ff0000 0c0c0303 00000000
     00000000 c0c03030 0c0c0c0c 3030c0c0   c0c03030 00000000 03030c0c 00ff0000
T2 = 30303030 0ff00000 00000000 0000f00f   00000ff0 0000ff00 0000f00f 00000000
     00000000 f00f0000 30303030 00000ff0   f00f0000 00000000 0ff00000 0000ff00
```
U-turn entries (dir = prev ^ 2) are 0, so phase 1 never reverses within a cell [obs].

### 5.2 Phase 1: cell corridor

Dijkstra over cells, 4-connected; open list is a heap of `{float −g, Entry*}`; entry = 0x18 bytes
`{x, y, z, enteredDir, parent, allocNext}` [obs `0x100e1e60.c`, `:136-148,232-240`]. Marks go in layer[4]:
start `0xaaaaaaaa`, visited `0x55555555` [obs `:150-152,267`].

Expanding cell P (walk word m) in direction d to neighbour Q requires [obs `:212-231`]:
* `edge[d] & m ≠ 0`; and if P has an entry direction p: `T1[p][d] & m ≠ 0` and `T2[p][d] & m ≠ 0`;
* Q inside the grid and within ±100 cells of the goal cell in x and z (`goalX±100`, `goalZ±100`) [obs; the `−0x19`
  in the pseudocode is pointer arithmetic on a `uint*`, i.e. −100];
* a layer exists at Q for the current y (`FUN_100e1b30`, ±0xa0); Q.y becomes that layer's height;
* `edge[d^2] & Q.walk ≠ 0` and Q's mark is 0.

Cost of the step [obs `:243-266`]:
```
base = 1.0 if d == P.enteredDir else 0.70710677     // a turn approximates a diagonal
c = base + 0.02·|Q.y − P.y|
if Q has no walk-code-2 q-cell:  c *= (Q has a code-1 q-cell) ? 2 : 4
if Q.occupancy word ≠ 0:         c += 1
c += hash((Q.x + Q.z·0x800) · (addressof(out) >> 2)) · 3e-5     // 0 … 0.98
```
`hash(s) = ((s·0x343fd + 0x269ec3) >> 16) & 0x7fff` (`FUN_10083280`). The `0x800` multiplier and the seed come from the
bytes just before the call at `0x100e72f4`: `shl edi,0xb; add edi,ecx; mov ecx,[ebp+0x20]; sar ecx,2; imul edi,ecx` [obs].

Goal: popped cell == (qx1>>2, y1, qz1>>2) with y equal [obs `:199-207`]. Failure → marks cleared, returns false,
**no** event [obs `:277-294`].

### 5.3 Phase 2: q-cell path inside the corridor

All cells on the found cell path are re-marked `0xaaaaaaaa` (code 2 in every q-cell = allowed) [obs `:296-305`].
Dijkstra over q-cells, 8-connected, allowed only where the mark code is 2; a pushed q-cell's mark becomes 3
[obs `:362-376,433-434`]. The neighbour needs a layer at its q-cell for the current y (`FUN_100e1d40`) with walk
code v ≠ 0.

```
c = (dir < 4 ? 1.0 : 1.4142135) + 0.05·|Q.y − P.y|
c *= 3 if v == 1 (road crossing);  c *= 4 if v == 3 (open ground);  pavement (2) ×1
c += 1 if Q's 2-bit occupancy ≠ 0
c += hash((Q.qx + Q.qz·0x2000) · (addressof(out) >> 2)) · 1.5e-5   // 0 … 0.49
```
[obs `:392-417`; `shl edi,0xd` just before the call at `0x100e7850`]. No corner-cutting test for diagonals [obs absence].

Goal: exact (qx1, y1, qz1) on pop [obs `:359-360`]. When the heap runs empty, the search still succeeds if the
**last popped** entry lies in the goal cell with the same y [obs `:441-442`]; otherwise it fires `onPathError`
(event 6) and returns false [obs `:512`]. The shipped script only logs "calculatePath problem"
(`extracted/metro/scripts/game.script:563-566`).

### 5.4 Result

Codes are written backwards from the goal: a direction code 0..7, followed by `8 + (n−1)` when the same direction
repeats n more times (n ≤ 8); a run longer than 8 starts a new direction code [obs `:444-509`; decoder
`0x100e18a0.c:20-38`]. y is not stored: the walker re-resolves the layer at every step from its current y
[inf, `0x100c9470.c:107`]. `out.cost` = g of the goal entry (phase-2 cost) [obs `:277,348`]. Marks are cleared and
all entries freed at the end [obs `:517-531`].

The noise is seeded by the **address** of the citizen's path cursor, so it is stable for one pedestrian and differs
between pedestrians. A reimplementation needs some per-agent seed; exact replay of the original is impossible.

### 5.5 Endpoint snapping

* `FUN_100e26b0(graph, &qx, &y, &qz, item, insideItem, requirePavement)`. With an item and `insideItem`, it collects
  the cells of the item footprint whose (template-masked) walk word has a pavement q-cell and picks one uniformly
  with `rand()`, taking its first code-2 q-cell [obs `:45-131`]. Otherwise it walks an outward square spiral from the
  point (radius up to 25 cells, `4r+2` legs) and takes the first cell, inside or outside the footprint as requested,
  with a code-2 q-cell; with `requirePavement == 0` a fully open cell (`0xffffffff`) also works, using q-cell 0
  [obs `:137-236`].
* `FUN_100e6610(graph, item, &qx, &y, &qz)`: door point = `FUN_100e26b0(item centre, inside, pavement)`. It then runs a
  small cell-level Dijkstra (costs 1.0 straight, 0.707 turn, no other terms) from that point until it reaches a cell
  with pavement **outside** the footprint. If it cannot, it re-snaps with `insideItem = 0` [obs `0x100e6610.c:53-243`].
  Used for every building door and stop.

Port (`engine/sim_citizens.cpp` `doorOf`, `walkPath`): both ends of a walk snap to the nearest walkable q-cell within
10 cells when they lie on a blocked one (a car parked inside a multi-storey garage, the middle of a station's tracks).
Metro stations (type 0x10) and the platform stop pieces on elevated or underground track (0xe/0xf with STOP) take
their door on the platform like every item: the platform leads out onto the rest of the station's platform, and the
station's stairs items (`stairs_metrou0a05` rising 0x80 per cell, the hall `service_metrou0`, the entrance
`stairs_metrou0b`; elevated: `stairs_metroe0*`) connect it to the street within ±0xa0 per step. Waiters stand and
board there; alighting passengers walk up or down. (Until 2026-11-05 the port read the walk and height maps half a
turn off (§1.4), which put the platforms on the track side and cut the stairs; it then took the stations' doors at
street level.) The search runs in the ends' box plus 16 cells first and, without a way there, again in the
original's phase-1 range of ±100 cells around the goal. Failed walks (Vienna, ~7000 frames at speed 3 with
`REGS_DEBUG_SIM=1`): 408 of 3807 paths with the maps half a turn off, 22 of 4555 with the maps as the engine turns
them. Re-measured 2026-11-18 (Vienna, 6000 frames at speed 3): 10 of 6319 (0.16 %), all in one place: walks from
open q-cells (code 3) at x 1427-1428, z 1821-1896 (cell x 356-357, z 455-474) to the q-cells (1427,1854) and (1427,1821),
which are blocked (code 0); both searches end after 4 expanded cells, so the starts are enclosed too (not traced
what item it is). Berlin, same run length: 6 of 4465 (0.13 %), from four places, e.g. (2432,2488) → (2444,2533)
after 420 858 expanded cells. In both cities **every failed walk ends on a blocked q-cell** (code 0), so the next
step is to find where those goals come from (door or stop cells inside their item's blocked area) and whether the
original snaps such a goal to open ground. Earlier notes: Cim2's sunken park (its stairs prop has no walk map and blocks its cell, so the original cannot
reach it either) and a hotel enclosed by bush props.

---

## 6. Transit search — `FUN_1011a200(lines, x0, z0, x1, z1, walkSpeed, Route* out, priceSig, directOnly, typeMask)`

Origin and destination are integer cells. Wrappers: `FUN_1011bf00(lines, itemA, itemB, …)` uses both item centres;
`FUN_1011bb50(lines, x, z, itemB, …)` starts at a point [obs]. Both pass `typeMask = 0xffffffff`. `routeExists` passes
`walkSpeed = 1`, `priceSig = 1` and the script mask [obs `0x1011b8e0.c:50-51`].

### 6.1 The graph

Built incrementally by `FUN_10119290(lines, N)` whenever a stop node gets a successor [obs; callers
`0x10119660`, `0x10119980`, `0x10119b90`]:
* N is bucketed by its departure stop S and by its arrival stop S' = N.next.stopItem [obs `:21-39`].
* Every node M whose **arrival** stop lies within `max(catchment(N.stop), catchment(M.stop))` world units of S, with
  `M.next ≠ N`, gets N appended to `M.transfers` [obs `:40-55`, query `FUN_10114720`].
* Every node M whose **departure** stop lies within the same radius of S', with `N.next ≠ M`, is appended to
  `N.transfers` [obs `:56-72`, query `FUN_10114560`].

Catchment queries scan buckets covering ±50 cells and test `(Δcells·cellSize)² ≤ r²` [obs `0x10114560.c:31-73`].
Default stop catchment: 100 for bus and tram stops, 150 for metro, boat and helicopter [obs `0x10119070.c:24-58`].
No duplicate check on transfer vectors [obs absence].

### 6.2 Line quality q (`line+0x174`)

Computed every line tick by `FUN_10114f30` (gsMoveable vtable `0x10167d74`, `this = line+0x134`)
[obs `0x10114f30.c:19-67`]:
```
headway (line+0x168) = Σ_stops (segment.driveTime + 5.0) / vehicleCount
q (line+0x174)       = line.attractiveness(+0x170) · mean(vehicle.attractiveness +0x178)
                       / (1 + 0.01 · line.avgWait(+0x180))
avgWait ← 0.95·avgWait + 0.05·ticksWaited     (on each boarding, FUN_10110be0, 0x100cc990.c:427)
```
Scripts set vehicle attractiveness to `model × condition` (`player.script:316`) and stop attractiveness to
`model × maintenance factor` (`player.script:355`). The search uses q; it never uses the headway.

### 6.3 Costs (seconds when walkSpeed is the character speed)

`w = cellSize / walkSpeed` [obs `:61`; stored at `[esp+0x20]` by bytes `0x1011a225`–`0x1011a239`]. `U(a,b) =
a + (b−a)·rand()/32767` (`FUN_10083250`). `fare(L) = max(L.ticketPrice − 200, 1) · priceSig`.

| Edge | Cost | Evidence |
|---|---|---|
| Board B (origin within catchment of B's departure stop) | `w·dist(origin, S_B) + 6·U(0, 1 − 0.8·attr(S_B)) + U(1, 2 − 0.8·q_L)·drive(B) + fare(L)` | `0x1011a200.c:87-104`; multiplier `[esp+0x20]` from bytes after `0x1011a327` [obs] |
| Ride on: P → C = P.next | `U(1, 2 − 0.8·q_L)·drive(C) + 5.0` | `:158-163`; 5.0 from bytes after `0x1011a64c` [obs] |
| Transfer P → T ∈ P.transfers | `w·dist(S'_P, S_T) + 6·U(2, 3 − 0.8·attr(S_T)) + U(1, 2 − 0.8·q_T)·drive(T) + (fare(T.line) if T's type is not in the paid mask)` | `:181-209`; multiplier and 3.0/2.0 from bytes after `0x1011a7ac`, `0x1011a7dd` [obs] |
| Alight at P's arrival stop S'_P, if S'_P is within ±50 cells (square) of the destination | `w·dist(S'_P, dest)` | `:144-157` [obs] |

`dist` is Euclidean in cells between stop centres; `drive(X)` is the stop-to-stop lane path time of node X (4.5).
A node's g therefore includes the ride to its arrival stop. The fare is charged at the first boarding and then only
once per transport type: the heap entry is `{−g, paidTypeMask, node}` [obs `:100-106,199-215`]. With stop
attractiveness 0, a transfer adds 12–18 s of waiting against 0–6 s at the first boarding; higher attractiveness
lowers both ranges. The destination side uses a 50-cell box, not the stop
catchment [obs].

### 6.4 Algorithm

Dijkstra with lazy deletion: a node is closed when popped (flag 4) and relaxed with `g' < best || best == −1`
[obs `:142-143,164,210`]. Heap: 12-byte entries, max-heap on key; on equal keys the entry with the smaller paid mask rises
[obs sift-up `0x10114d20.c:16-18`]. Termination: before closing a popped node, stop when a destination candidate exists and
`bestTotal ≤ g(popped)` [obs `:141`]. Eligibility: line type in `typeMask`, line active, node not temporary/closed,
next node not temporary [obs `:84-86,159-160,186-188`]. `directOnly` skips transfers [obs `:180`]. **Every edge
evaluation draws fresh random numbers**: 2 `rand()` per boarding candidate, 1 per ride relaxation, 2 per transfer
relaxation [obs].

### 6.5 Result

`out = {bestTotal, count, 0, nodes[]}` with the nodes from the boarding node to the alighting node (parent chain
reversed), each `refCount++`; touched nodes are reset (`flags &= ~6`, `best = −1`) [obs `:231-253`]. Returns true
when a destination candidate was found.

---

## 7. How the legs combine (interfaces to the citizen and vehicle write-ups)

From `FUN_100d18f0` (mode choice, citizen domain) [obs, cited lines]:
* Walk estimate: `walkT = |origin centre − dest centre|(cells) · cellSize / walkSpeed`; −1 for utility trips
  [`:128-139`].
* Car estimate (citizen has a car and `walkT ≥ 100`, or a utility trip): start = `FUN_100e24f0(origin,
  START|0x100)` or the node nearest the parked car; goal = END|0x100 node nearest a random PARK spot in a
  (w+l+4) square around the destination, else `FUN_100e24f0(dest, END|0x100)` [`:153-196`]. `carT = lanePath.cost ·
  (1 + 0.1·priceSig) + (dist(origin, startNode) + dist(dest, goalNode))·cellSize/walkSpeed` [`:197-224`]. The
  walking ends of a car trip are straight-line estimates, not walk searches.
* Transit estimate: `FUN_1011bf00(origin item, dest item, walkSpeed, priceSig, directOnly = 0)` [`:241-243`].
* Choice [`:268-387`]:
  * Transit, if a transit route was found and it is cheaper than both the car (if any) and walkT.
  * Otherwise the car, if a car path with start and end nodes exists, and either `carT < walkT`, walkT = −1, or the
    citizen's car is already out. The exact condition is in `citizens.md` §5.4.
  * Otherwise walk, if the straight distance is ≤ 250 world units.
  * Otherwise (also for utility trips, where walkT = −1) the citizen is moved straight to its target. That still
    counts as a walk and fires the arrival event.

  The test at `:297-298` is Ghidra's `(x < 250) == (x == 250)` idiom, which means `x > 250`. *Corrected by the
  coordinator; matches `citizens.md` §5.4.*

Execution:
* Walk trip: door points by `FUN_100e6610` at both items, spawn the pedestrian, `FUN_100e6cf0` [obs
  `0x100cc800.c:21-29`].
* Transit trip: walk search to the first route node's stop door; at each alighting, walk search to the next node's
  stop (transfer) or to the destination [obs `0x100cc990.c:46-69,191-211,309-324`]. The route is not recomputed while
  riding.
* Mid-trip re-plan: `FUN_100cd5c0` runs `FUN_1011bb50` from the pedestrian's cell; it takes transit if cheaper than
  walking the rest, walks if ≤ 250 units, else gives up [obs `0x100cd5c0.c:33-69`].
* Car trip: the cached lane path is copied into the vehicle's cursor (`FUN_100e19f0`); a stale path (vehicle flag 1)
  is re-searched from the last queued node with `ignoreTraffic = 0` [obs `0x100ceee0.c:131-139,256-264`].

---

## 8. Pseudocode summary

```
// ---------- lane search (FUN_100e5fe0) ----------
laneSearch(start, goal, out, ignoreTraffic):
  graph.searched = 1; out = {gen: graph.generation}
  if !start or !goal: return false
  if (start.flags & 0x1003f940) == 0x4040: out = direct(|start.p1-goal.p0|/6); return true
  start.key = 0; start.flags |= 1; heap.push(start); touched = [start]
  while heap:
    P = heap.pop(); g = -P.key
    if P == goal: break
    i = 1
    for S in P.successors:                       // insertion order
      if (S.flags & 0x400000F)==0 and (S==goal or S.successors):
        S.parent = P; S.flags |= i; touched += S
        c = edgeCost(P, S, goal, ignoreTraffic)  // 4.2
        if S==goal or not deadEndLookahead(S): S.key = -(g + c); heap.push(S)
      i += 1
  if found: out.cost = Σ len/speedLimit; encode link indices (runs of link 0 → 8+n-1)
  clear low nibble of touched
// ---------- pedestrian search (FUN_100e6cf0) ----------
walkSearch(a, b, out):
  corridor = dijkstraCells(a, b)        // 4-neighbour, window ±100 cells, cost 5.2; fail → false
  mark corridor cells "allowed"
  path = dijkstraQCells(a, b, corridor) // 8-neighbour, cost 5.3
  if !path and lastPopped not in b's cell: fire onPathError; return false
  encode directions (dir, then 8+n-1 repeats)
// ---------- transit search (FUN_1011a200) ----------
transitSearch(o, d, walkSpeed, priceSig, directOnly, mask):
  w = cellSize/walkSpeed
  for B in nodesDepartingWithinCatchment(o): if ok(B): push(cost_board(B), typeBit(B.line), B)
  best = none
  while heap:
    (k, paid, P) = pop(); g = -k
    if best and best.total <= g: break
    if P.closed: continue; P.closed = true
    S' = P.next.stop
    if |S' - d|∞ <= 50: best = min(best, g + w*dist(S', d))
    C = P.next; relax(C, g + U(1,2-.8q)*drive(C) + 5, paid)
    if !directOnly: for T in P.transfers if ok(T):
        relax(T, g + w*dist(S',T.stop) + 6*U(2,3-.8attr) + U(1,2-.8q)*drive(T)
                    + (paid has T.type ? 0 : fare(T)), paid | T.type)
  return chain(best.node)
```

---

## 9. Side note: `FUN_100d6ab0`

Confirmed as rendering, not simulation. `FUN_100e0710` calls it twice per cell (passes 0 and 1) when a cell becomes
dirty or comes within 400 units of the camera (`0x100e0710.c:15-38`). Its main callee `FUN_100d43a0` emits vertices
with position from cell coordinates × cellSize, normal, colour 0xffffffff and UVs [obs `0x100d43a0.c`]. The graph
lookup it makes (`FUN_100e1c10`) reads layer[5], the ground tint [obs].

---

## 10. Open questions

1. `FUN_10084dd0` (the curvature probe in 2.1) is the cubic-Bézier second derivative (`gsBezierCurve`
   `L:0x0810e920`: `6(1−t)P0 + (18t−12)P1 + (6−18t)P2 + 6tP3`) [obs]. `FUN_10085240` (segment distance) is still
   read only far enough for its role.
2. ~~Adjacency order after loading a save~~ — resolved: nodes inserted per item in file order (`0x07020210`), the
   item's paths in name order, with the queries of 2.2-2.3, give the engine's order. The port replays the saved paths
   of all 803 self-driving traffic agents in Cim2 from their last queued node: every code names an existing link and
   every path ends on an `END` or `PARK` node (`REGS_DEBUG_SIM=1`, port `engine/sim.cpp`) [obs].
3. Phase-1 walk costs mix float and int in the pseudocode (`FUN_100afc10` "abs" applied to a float slot). The
   formula above assumes integer height differences; check the bytes before the call at `0x100e72f4`.
4. The water-lane lattice in `FUN_100c1840` (spacing, which border lanes get START/END, the turn connectors) is only
   summarised.
5. Node flag 0x1000000 (blocked) and 0x4000000 (excluded): who sets 0x1000000, and whether 0x4000000 is ever
   cleared (removing the item does not appear to clear it).
6. `FUN_100e24f0` is declared `void` by Ghidra; the node is returned in EAX [inf from callers]. Confirm in the
   bytes.
7. The transit heap tie-break (equal keys ordered by paid mask) may make results depend on mask values; check
   whether the node pointer also takes part.
8. The walk search seeds its noise with a heap address. Pick a deterministic per-agent seed when reimplementing;
   exact replay of the original is not possible.
