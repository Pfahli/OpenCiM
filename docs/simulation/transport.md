# Public transport: lines, stops, transit vehicles, boarding, background traffic

Engine: `GSystem.dll` (Cities in Motion 1.0.22). Evidence is `0xADDR.c:line` in
`decompiled/GSystem-v2/functions/`. Hidden x87 constants were read from the DLL bytes (file offset = VA − 0x10000000) with a
small scanner; such citations name the instruction address, e.g. `@0x101154b7`.
Tags: **[obs]** = read directly from pseudocode or bytes; **[inf]** = inferred (naming, purpose, units).

Scope: `gsTransportLines`, `gsTransportLine`, the line-stop node, `gsTransportStopController`, `gsTransportVehicle`,
the passenger side of boarding/alighting (which lives in the citizen code, `FUN_100cc990`), and the background
traffic module `gsGridVehicles`. Generic agent movement (`FUN_1010de50`), the A* path finder (`FUN_100e5fe0`) and the
citizen mode choice (`FUN_100d18f0`) are documented elsewhere; they are named here only by role.

---

## 1. Summary for a reimplementer

* A **line** is a singly linked ring of **line-stop nodes** (one node per (line, stop) pair, 0x70 bytes). Each node owns
  the **segment** from its stop to the next stop: start/end path nodes, a cached A* path and its travel cost.
* A **stop** (`gsTransportStopController`) is the script object of a stop/station grid item. It holds only aggregate
  counters (waiting passengers, mood) and catchment radius (100 for bus/tram, 150 for metro/boat/heli).
* **Vehicles** belong to one line. They drive one segment ("leg") at a time, halt at the stop, open doors, wait a
  **dwell time** `clamp(headway + 4 − timeSinceLastDeparture, 4, 15)` seconds, then load the next leg.
  `headway = Σ_segments(travelCost + 5) / vehicleCount`. Only one vehicle per (line, stop) node is "docked" at a time.
* **Passengers are driven from the citizen side.** A citizen with a transit route waits at a node, starts walking to a
  docked vehicle of that node if it has room, boards when it reaches it (fires `onGetPassenger`), rides until the
  vehicle docks at a stop that is not the next node in its route, then alights and walks on (transfer or destination).
* **Route choice** is a Dijkstra search over line-stop nodes with randomised costs (walking, waiting, riding, fare
  once per transport type). Transfers are precomputed per node: all nodes boarding within catchment of the node's
  alighting stop.
* **Breakdowns:** each second, `P(break) ≈ 0.001 / (1/(1−condition) − 0.99999)`. A broken vehicle drives to its next
  stop, dumps its passengers (they re-plan), fires `onBroken`, waits `stopTime` seconds (60 native, overwritten by the
  script), fires `onFixed`.
* **Background traffic** (`gsGridVehicles`) spawns trains, boats and aircraft up to graph-derived caps and updates all
  vehicle agents bucket by bucket (visible buckets every frame, others at 5 Hz with dt = 0.2 s).

Per-frame order (from the coordinator, not re-verified here): grid sub-steps tick graph → vehicles (agent movement)
→ characters → citizens (boarding/alighting) → lines. The line tick ends with `FUN_100a0670`, which ticks the line's
moveable children, i.e. its vehicles [obs: `0x10114f30.c:68`, `0x100a0670.c` iterates `owner+0x5c` and calls slot 0 of
each child whose `+0x68 & 1` is set]. So within one sub-step: line timers/headway first, then every vehicle of that line.

---

## 2. Data layouts

Offsets are from the object start. "tick this" = object + subobject offset (vehicle +0x144, line +0x134,
vehicles module +0xcc).

### 2.1 `gsTransportLines` (line manager, `grid+0x218`)

| off | type | name | meaning | evidence |
|---|---|---|---|---|
| 0x4c/0x50 | vector | children | module children; lines are found by RTTI cast | `0x10116300.c:40-46` |
| 0xd4 | ptr | grid | owning grid | `0x10113cb0.c:27` |
| 0xd8, 0xdc | int | bucketW, bucketH | grid size the bucket index was built for | `0x10111250.c:20-34` |
| 0xe0 | int | bucketShift | log2 cells per bucket (init call passes 3 → 8×8 cells) [obs: `0x101167a0.c:17` passes 3; caller of the real init not traced] | `0x10111250.c:30-39` |
| 0xe4, 0xe8 | int | bucketsX, bucketsZ | `(size + 2^shift − 1) >> shift` | `0x10111250.c:35-39` |
| 0xec | float | cellSize | world units per cell; catchment tests use cells × this | `0x10114560.c:58-62` |
| 0xf0 | node*[] | boardIndex | per bucket, list of line-stop nodes keyed by **their own** stop position, link `node+0x48` | `0x10119290.c:29-33` |
| 0xf4 | node*[] | alightIndex | per bucket, nodes keyed by their **next** stop position, link `node+0x4c` | `0x10119290.c:35-39` |
| 0xf8..0x104 | vector | stops | all `gsTransportStopController`s (first `+0xfc`, last `+0x100`, end `+0x104`) | `0x10119070.c:18-30`, `0x10114530.c` |

The lines-manager tick is the plain module tick (`0x101111d0` thunks to `FUN_100a0670`); it only ticks the lines.

### 2.2 `gsTransportLine` (400 bytes, ctor `FUN_10118830`)

| off | type | name | meaning | evidence |
|---|---|---|---|---|
| 0x68 / 0x6c | uint | ownFlags / flags | gsModule flags; effective = own & parent; bit 0x10000 = `active`, bit 2 = path display on [inf] | `0x100a00c0.c`, `0x1011bba0.c` (`active` bit field) |
| 0x134 | vtbl | gsMoveable | tick `0x10114f30` | RTTI |
| 0x13c | vtbl | gsRouteObject | route polyline for minimap/overlay (`0x10113ed0` count points, `0x10113f50` fill) | `0x10113ed0.c`, `0x10113f50.c` |
| 0x144 | ptr | grid | | `0x10113cb0.c:27` |
| 0x148 | node* | firstStop | head of the stop ring (link `node+0x24`) | `0x10119660.c:26-41` |
| 0x14c/0x150/0x154 | var | pathMaterial / tempPathMaterial / extraPathMaterial | rendering | `0x1011bba0.c` |
| 0x158..0x164 | vector | vehicles | `gsTransportVehicle*`, first `+0x15c`, last `+0x160` | `0x10118e80.c:16-29` |
| 0x168 | float | **headway** | `Σ_nodes(seg.travelCost + 5) / vehicleCount` (seconds) [obs formula, inf name] | `0x10114f30.c:18-26,61-62` |
| 0x16c | int | type | `TRANSPORTLINE_*` | registrar |
| 0x170 | float | attractiveness | script field, ctor 1.0 | `0x10118830.c:35` |
| 0x174 | float | **effAttractiveness** | `attractiveness × mean(vehicle.attractiveness) / (1 + 0.01 × avgWait)` | `0x10114f30.c:27-67` |
| 0x178 | int | stopCount | | `0x10119660.c:39,126` |
| 0x17c | bool | isClosed | | `0x10119660.c:72` |
| 0x180 | float | **avgWait** | EMA of boarding passengers' wait ticks: `v = 0.95 v + 0.05 wait` | `0x10110be0.c:11`, `0x100cc990.c:427` |
| 0x184 | float | ticketPrice | ctor 1.0; script sets it | `0x10118830.c:39` |
| 0x188 | int | color | setter `0x10114ec0` re-tints all vehicle agents | `0x10114ec0.c` |
| 0x18c | int | passengersTransported | +1 per normal alighting | `0x100cc990.c:172-173` |

### 2.3 Line-stop node (0x70 bytes, ctor `FUN_10118760(line, item, isTemp)`)

| off | type | name | meaning | evidence |
|---|---|---|---|---|
| 0x00 | int | refCount | 1 at creation; +1 per waiting citizen and per route holding it; freed at 0 | `0x10118760.c:15`, `0x100cc990.c:224`, `0x10116ad0.c` |
| 0x04 | ptr | stop | `gsTransportStopController` of the item (dynamic cast of `item+8`) | `0x10118760.c:45-48` |
| 0x08 | ptr | line | | `0x10118760.c:17` |
| 0x0c | ptr | item | stop grid item | `0x10118760.c:18` |
| 0x10 | struct | segment | §2.4 | `0x10118760.c:19` |
| 0x24 | node* | next | next node; 0 at the end of an open line | `0x10119660.c` |
| 0x28 | node* | parent / catchment-list link | Dijkstra predecessor; also the result-list link of catchment queries | `0x1011a200.c:171,218,237`, `0x10114560.c:71` |
| 0x2c | node* | visitedLink | Dijkstra cleanup list | `0x1011a200.c:176,250` |
| 0x30 | uint | flags | 1 = temporary stop, 2 = touched (search), 4 = settled (search), 8 = deleted | `0x10118760.c:23`, `0x1011a200.c:143,175,251`, `0x10119980.c:86` |
| 0x34 | float | **sinceDeparture** | seconds since a vehicle last left this node (+dt every line tick, 0 at departure) | `0x10114f30.c:22`, `0x101152a0.c:338-340` |
| 0x38..0x44 | vector | transfers | nodes reachable by transfer after riding this node's segment (§3.4) | `0x10119290.c:56-72` |
| 0x48 / 0x4c | node* | boardLink / alightLink | bucket links (§2.1) | `0x10119290.c:32,38` |
| 0x50 | vehicle* | **docked** | vehicle currently in the doors-open window here (0 = none) | `0x101152a0.c:391-393,403-406` |
| 0x54 | float | bestCost | Dijkstra, −1 = unset | `0x10118760.c:32`, `0x1011a200.c:252` |
| 0x58 | int | waiting | citizens waiting at this node | `0x100cc990.c:225`, `0x100cbc80.c:45` |
| 0x5c / 0x60 / 0x64 | int | impatient / angry / gaveUp | citizens currently in each wait tier (§5.3) | `0x100cc990.c:274,294,342` |
| 0x68 | int | boarded | passengers who boarded here (`getPassengersTransported(i)`) | `0x100cc990.c:421` |
| 0x6c | int | waitSum | Σ wait ticks of those passengers (`getWaitingTime(i)`) | `0x100cc990.c:422` |

### 2.4 Segment (embedded at node+0x10, init `FUN_101107d0`)

| off | type | name | meaning | evidence |
|---|---|---|---|---|
| +0x00 | pathNode* | start | platform node at this stop (refcounted `+0x78`) | `0x101131f0.c:20-29` |
| +0x04 | pathNode* | end | platform node at the next stop | `0x101131f0.c:30-39` |
| +0x08 | obj* | mesh | rendered path strip | `0x101119d0.c:120-126` |
| +0x0c | uint | flags | 1 = display enabled, 2 = coverage overlay applied | `0x10113e00.c:19,35`, `0x101119d0.c:132`, `0x10111410.c:54-56` |
| +0x10 | cursor* | path | 0x18-byte path cursor (`+4` stepCount, `+0xc` travelCost, `+0x14` graph generation) | `0x101107d0.c:26-34`; cursor layout in `docs/save-format/vehicles-movement.md` §2.5 |

### 2.5 `gsTransportStopController` (ctor `FUN_10110ea0`)

| off | type | name | meaning | evidence |
|---|---|---|---|---|
| 0x134 | ptr | grid | | `0x10119070.c:18` |
| 0x13c | ptr | item | stop grid item | `0x10119070.c:31` |
| 0x160 | int | passengerCount | citizens waiting at this stop, all lines | `0x100cc990.c:226-229`, `0x100cbc80.c:45-51` |
| 0x164 / 0x168 / 0x16c | int | impatient / angry / gaveUp | sums of the node tiers over all lines | `0x100cc990.c:276,296,344` |
| 0x170 | int | mood | 0..3, §5.3 | `0x101110c0.c` |
| 0x174 | float | catchmentArea | 100.0 (bus, tram) or 150.0 (metro, boat, heli), set at registration | `0x10119070.c:54-58` |
| 0x178 | int | type | `GRIDSTOP_*` from item type: road 2/5/6 → 0, tram 10/11/12 → 1, metro 13/14/15 → 2, dock 0x11 → 3, heliport 0x12 → 4 | `0x10119070.c:32-53` |
| 0x17c | float | attractiveness | script field, ctor 1.0; used in route costs | `0x10110ea0.c:27`, `0x1011a200.c:95,198` |

### 2.6 `gsTransportVehicle` (0x1a0 bytes, ctor `FUN_10118d50(grid, line, model, name)`)

Inherits `gsGridVehicleController` (`+0x13c` agent, `+0x140` isVehicle). Tick `this` = obj+0x144.

| off | tick off | type | name | meaning | evidence |
|---|---|---|---|---|---|
| 0x6c | −0xd8 | uint | flags | 0x10000 = effective `active` | `0x101152a0.c:44` |
| 0x134 | −0x10 | ptr | grid | | `0x101152a0.c:226-236` |
| 0x13c | −0x08 | agent* | agent | head 0x94-byte vehicle agent; 0 = not spawned | `0x101152a0.c:32,100` |
| 0x14c | +0x08 | line* | line | | `0x10118d50.c:20` |
| 0x150 | +0x0c | segment* | curSegment | segment being driven | `0x101152a0.c:124,201-202` |
| 0x154 | +0x10 | node* | stopA | node just left (leg start) | `0x101152a0.c:125,374` |
| 0x158 | +0x14 | node* | stopB | node being approached / docked at | `0x101152a0.c:126,375-379` |
| 0x15c | +0x18 | pathNode* | stopBPlatform | `stopA.segment.end`; its flag 0x40000 selects door set 2 | `0x101152a0.c:128,384,396` |
| 0x160 | +0x1c | float | stopTime | dwell timer; −1..0 door opening when broken; repair countdown | §4.3, §4.5 |
| 0x164 | +0x20 | float | respawnRepathTimer | while unspawned: re-path every >1 s | `0x101152a0.c:64-78` |
| 0x168 | +0x24 | int | nextLegSteps | step count of the pre-loaded next leg, applied at departure | `0x101152a0.c:216,348` |
| 0x16c | +0x28 | int | capacity | Σ unit model `capacity` (`+0x298`) | `0x10118d50.c:44`, `0x10118c10.c` |
| 0x170 | +0x2c | int | passengerCount | | `0x100cc990.c:417-418` |
| 0x174 | +0x30 | int | **boardingReserved** | citizens walking to the doors | `0x100cc990.c:368-369,419` |
| 0x178 | +0x34 | float | attractiveness | script, ctor 1.0 | `0x10118d50.c:32` |
| 0x17c | +0x38 | float | travelDistance | §4.6 | `0x101152a0.c:203,344,458` |
| 0x180 | +0x3c | float | condition | script-maintained, ctor 1.0, not saved | `0x10118d50.c:34` |
| 0x184 | +0x40 | bool | bellRung | vehicle bell played (< 8 nodes left) | `0x101152a0.c:244-248` |
| 0x185 | +0x41 | bool | announced | stop's bell played (< 32 nodes left) | `0x101152a0.c:221-242` |
| 0x186 | +0x42 | bool | broken | | `0x101152a0.c:442` |
| 0x188 | +0x44 | int | passengersTransported | +1 per normal alighting | `0x100cc990.c:171` |
| 0x18c | +0x48 | float | breakdownClock | 1-second accumulator for the breakdown roll | `0x101152a0.c:436-439` |
| 0x190..0x19c | +0x4c | vector | units | `{gsGridObject* model; bool reversed}` (8 bytes), first `+0x194` | `0x10118d50.c:41-43`, `0x10118c10.c` |

### 2.7 Citizen fields used by transit (citizen record, `int* c` in `FUN_100cc990`)

Only what this document needs; the full layout belongs to the citizens document.

| off | name | meaning | evidence |
|---|---|---|---|
| +0x08 | character | walking character agent (0 when riding) | `0x100cc990.c:110-116,410-411` |
| +0x10 | waitNode | line-stop node the citizen is waiting at | `0x100cc990.c:223` |
| +0x14 | vehicle | vehicle ridden | `0x100cc990.c:416` |
| +0x1c | route | `{f32 cost; i32 count; i32 index; node** nodes}` (16 bytes, dtor `FUN_10116ec0`) | `0x1011a200.c:231-248`, `0x10116ec0.c`, `0x100d18f0.c:231-243` |
| +0x28 | origin | trip origin item (first walk starts here) | `0x100cc990.c:51-57`, `0x100d18f0.c:241` |
| +0x2c | destination | destination item | `0x100cc990.c:156,203` |
| +0x48 | flags | low nibble class; 0xf0 state (0x30 TRAVEL, 0x40 re-plan); 0x100 walking to doors; 0x200 just alighted; 0x1000000 impatient; 0x2000000 angry; 0x4000000 gave up / failed | `0x100cc990.c` passim, `0x100d3ca0.c` |
| +0x50 | id | citizen id (map key); low byte seeds patience [inf] | `0x100d31a0.c` (`FUN_100d1820(…, c+0x50)`) |
| +0x58 | waitTicks | u32, citizen updates spent waiting at the node | `0x100cc990.c:230,271-272` |
| +0x5c | paidTypes | bit (1 << line.type) per transport type already paid this trip; cleared at trip end | `0x100cc990.c:436-440`, `0x100cc6c0.c:70` |

Each citizen is updated about once per simulated second: the citizens tick runs `N·dt` updates round-robin
(`0x100d40e0.c:18-35`). So `waitTicks` ≈ seconds [inf].

### 2.8 Vehicle-agent flags set by transit code

| bit | meaning (corrected) | evidence |
|---|---|---|
| 0x2 | car reversed | `0x101152a0.c:105-107,156-157` |
| 0x5 | invalid → despawn (line stop deleted, or no next leg) | `0x101152a0.c:181-183,349-352` |
| 0x200 | agent belongs to a `gsTransportVehicle` (`agent+0x2c` = vehicle) | `0x101152a0.c:109-111,159` |
| 0x400 | halted at a stop (dwell) | `0x101152a0.c:309,355` |
| 0x1000 | boat: node acceptance delegated to the controller | `0x101152a0.c:101-103` |
| 0x10000 | **broken down and halted** (not "dwelling at a terminus" as in `vehicles-movement.md`) | `0x101152a0.c:255,274,446` |

Mask `0x50400` stops the movement code from pulling new path nodes (see `vehicles-movement.md`), which is how
halting at a stop or for a breakdown works.

---

## 3. Line construction

### 3.1 Adding a stop: `FUN_10119660(line, item, isTemp)` [obs]

Called by the player tool and by the save loader. Returns the new node or 0.

```
if line.first == 0: first = new Node(line, item, isTemp); stopCount++; return first          // :28-41
if !line.isClosed:
    walk to the last node (next == 0); reject (return 0) if item is already on the line (except as first)  // :44-50
    if item == first.item and last != first:                                                 // :52-73
        last.next = first; isClosed = true; if isTemp: first.flags |= 1     (no new node)
    else: append new node after last (next = 0)                                               // :54-62
else (closed):                                                                                 // :90-123
    choose the edge (n, n.next) minimising  d(n,new) + d(new,n.next) − d(n,n.next)
        where d = segment cost FUN_10114200(line, a, b, null); both new legs must be reachable (≠ 0)
    insert new node between n and n.next (reject if no reachable edge or item already present)
then: unregister prev from the transfer index, prev.next = new, re-register prev,
      compute prev's segment; if new.next != 0 and new != first: register new, compute its segment   // :133-142
stopCount++ (not when merely closing)
```

The script refuses to activate a line that is not closed (`linepanel.script:659-663`), so active lines are rings.

### 3.2 Segment path: `FUN_10114200(line, itemA, itemB, nodeOrNull) → travelCost` [obs]

* Platform node of an item: `FUN_10110c10(line, item, alt)` asks the graph (`FUN_100e21e0`) for the nearest path node
  at the item centre with flags `GRID_PATH_STOP` (0x40), or `GRID_PATH_ALTERNATIVE_STOP` (0x40000) when `alt`, plus
  the class bit: bus 0x100, tram 0x800, metro 0x2000, boat 0x8000 (y = −1.25), heli 0x4000 (y = −1.25)
  [`0x10110c10.c:17-50`].
* Metro and boat lines, or any stop whose primary platform has flag 0x40000000 (`GRID_PATH_FIND_ALTERNATIVE`), try
  all four combinations primary/alternative × primary/alternative and keep the cheapest successful A* path
  [`0x10114200.c:33-77`]. Other lines use primary → primary only [`:79-88`].
* With a node, the path and both platform nodes are stored in the node's segment (`FUN_101131f0`), and the display mesh
  is rebuilt (`FUN_101119d0`) [`:44-96`]. If either item is null the segment is cleared and 0 is returned [`:100-115`].
* The returned value is the A* travel cost (cursor `+0xc`, ≈ Σ length/speedLimit, seconds [inf]).

Because each segment picks its platforms independently, the outbound segment of a stop may start on a different
platform than the inbound segment ended on; vehicles then reverse or jump (§4.4).

### 3.3 Deleting and moving stops [obs]

* `deleteStop(i)` → `FUN_10119980(line, node)`: unlink the node, recompute the predecessor's segment (or clear it if
  the predecessor becomes the end), subtract the node's counters from its stop, set `flags |= 8`, release the
  reference [`0x10119980.c:18-94`]. Deleting the first node of a closed ring does not delete it: the last node's
  `next` becomes 0 (the ring opens), `isClosed = 0` and the first node's temp bit is cleared [`:24-31,49-53`]. Vehicles heading to a deleted node invalidate their agent and despawn (`0x101152a0.c:181-183`); waiting
  and riding citizens abort when they see flag 8 (`0x100cc990.c:194,234,310-312`).
* **Quirk:** when subtracting the node counters from the stop, all three tier counters are decremented by
  `node.impatient` (`+0x5c`), not by `angry`/`gaveUp` respectively [`0x10119980.c:57-60,72-75`; same in the line
  clear `0x10116b10.c:24-27`]. The stop counters can therefore drift.
* `moveStop(i, j)` → `FUN_10119b90`: swaps the two nodes in the ring and recomputes every affected segment
  [`0x10119b90.c:15-91`].

### 3.4 Transfer index [obs]

`FUN_10119290(lines, n)` runs whenever a node gets a successor [`0x10119290.c:21-72`]:

1. Insert `n` into `boardIndex` at `n.stop` and into `alightIndex` at `n.next.stop`.
2. For every node `k` whose **next** stop lies within `max(k.stop.catchment, n.stop.catchment)` of `n.stop`
   (`FUN_10114720`) and `k.next ≠ n`: append `n` to `k.transfers`.
3. For every node `k` whose **own** stop lies within `max(k.stop.catchment, n.stop.catchment)` of `n.next.stop`
   (`FUN_10114560`) and `k ≠ n.next`: append `k` to `n.transfers`.

So `n.transfers` = every node (any line, any direction, also the same line) that boards within catchment of the stop
where `n`'s ride ends, except simply staying on board. Distances are `cells × cellSize` between item centres, test
`d² ≤ r²`; the bucket search box is ±50 cells. `FUN_10116880` is the exact inverse (unlink, remove `n` from others'
lists, free `n.transfers`).

### 3.5 Coverage overlay (display only) [obs]

`grid+0x1e8` is a 256×256 array of 16-byte cells (minimap / info view). Byte 0 counts any line, byte `type+1` counts
lines of that type. Stops add a plus-shaped 5-cell stamp at registration and remove it on destruction
(`0x10119070.c:59-76`, `0x10116440.c:23-38`); segments add/remove their path cells (`FUN_101119d0`,
`FUN_10111410`). No simulation code reads it [inf: only render/minimap functions reference `+0x1e8`].

---

## 4. Vehicles

### 4.1 Creation and attachment [obs]

* `line.addVehicle(model)`: `model` is a `gsGridObject` or an idName looked up in `grid+0x1ec`. Creates the vehicle
  with unit 0 = model and `capacity = model.capacity` (`+0x298`), then attaches it to the line (vtable `+0xa4` → slot
  41 `0x10118e80`: becomes a child module of the line, is placed at the first stop's platform, is appended to
  `line.vehicles`) [`0x10118f20.c`, `0x10118e80.c:14-28`, `0x10118d50.c:41-44`].
* `vehicle.addTrailer(model, [reversed])`: appends a unit and adds its capacity [`0x10118c10.c`].
* Deactivating (effective flag 0x10000 cleared, which also happens when the parent line is deactivated, because
  effective flags = own & parent's, `0x100a00c0.c`, `0x100a0550.c`) runs slot 40 `0x10110840`: `boardingReserved =
  0`, undock, despawn the agent chain, `stopTime = 0`, fire **onUnspawn** [`0x10110840.c:12-29`].
* Destruction (slot 29 `0x101151b0`) removes it from `line.vehicles`, undocks, frees the agent.

### 4.2 Spawning ("depot" behaviour) [obs: `0x101152a0.c:48-178`]

A vehicle without an agent (new, re-activated, or despawned after a failure) tries every tick:

```
if broken: stopTime += dt; if stopTime >= 60: broken = 0; stopTime = 0; fire onFixed     // :32-43 (no spawn while broken)
n = first node of the ring whose segment.start platform node is free (FUN_100e14b0(start, 0, true))
if n:
    start.flags |= 0x1000000                          // blocks the node for other traffic [inf: reservation]
    if path generation of n.segment is older than start node, or respawnRepathTimer > 1:
        re-run A* for n.segment; on failure fire onCannotReach(n.segment.end.position)    // :64-77
        respawnRepathTimer = 0
    respawnRepathTimer += dt
    if start free and (heli or first path node after start is free) and path ok:
        lengthNodes = trunc(Σ_units(frontOffset + rearOffset) / 6 + 0.99)                 // :87-95, @0x101154b7
        head = new agent(unit0.model, start, lengthNodes if type >= 2 else 0)             // :96-99
        each further unit k: agent behind the previous one, posOnNode = trunc(−65536 · Σ_{j=1..k} len_j)
                             (16.16 fixed point), last unit of metro/boat/heli also gets lengthNodes   // :131-163, @0x101156b7
        boats: flags |= 0x1000; reversed units: flags |= 2; all: flags |= 0x200, agent.controller = vehicle
        copy n.segment.path into the agent; curSegment = n.segment; stopA = n; stopB = n.next
        heli: build flight path (§4.7); else agent.pathStepsLeft = (lengthNodes >> 1) + path.stepCount
        fire onSpawn
```

`lengthNodes` divides by 6, i.e. path nodes are treated as 6 world units long [inf]. Bus and tram heads use the
model's `length` field (`+0x248`) instead (agent ctor takes `max(model.length, arg)`, `0x101089d0.c`).

### 4.3 The stop cycle

Leg model: `pathStepsLeft` counts nodes the agent may still pull. At spawn it is the first leg's step count plus half
the train length in nodes; at each departure it is the next leg's step count. As soon as the agent's path cursor is
exhausted (all nodes of the leg queued), the next leg is pre-loaded into the cursor, but its step count is parked in
`nextLegSteps` until departure; the extra half-length is pulled from the start of the next leg. Net effect: the head
halts half a train length past each platform node, i.e. the train's middle is at the platform [inf from the
`(lengthNodes>>1)` and `chainLength/12` terms].

Pseudocode of the spawned, not-crashing, path-valid branch (`0x101152a0.c:185-452`); `dt` is the sub-step time:

```
if type == HELI: heliSeparation()                                    // :187-188, FUN_10110900
elif agent.cursor finished and cursor non-empty:                    // :191-219  pre-load next leg
    travelDistance += agent.cursor.stepCount * 6                    // :200-203
    curSegment = stopB.segment
    A*(stopB.segment.start -> stopB.segment.end) into curSegment
        on failure: fire onCannotReach(stopB.segment.end.pos); agent.pathStepsLeft = 0
    agent.cursor = copy(curSegment.path); nextLegSteps = agent.cursor.stepCount
elif agent.waitTime > 180: fire onJammed; agent.waitTime = 0          // :192-198

left = agent.pathStepsLeft + agent.ringCount
if left < 32 and !announced: announced = 1; play stopB.item.template.bellSound at the stop   // :221-242
if left < 8  and !bellRung:  bellRung = 1;  play head.model.bellSound                         // :244-248

if broken: brokenCycle(); return                                    // §4.5

arrived = agent.pathStepsLeft == 0 and agent.ringCount <= head.model.length + 1 and agent.speed < 0.1   // :300-303
if arrived and (stopB.docked in {0, self} or agent.flags & 0x400) and (type != HELI or agent.curNode == stopB.segment.start):
    agent.flags |= 0x400
    if stopTime == 0: play doorOpenSound                            // :311-314
    D = clamp(line.headway + 4 - stopB.sinceDeparture, 4, 15)       // :316-321
    stopTime += dt
    if boardingReserved != 0:                                       // :323-330
        stopTime = min(stopTime, D - 1)
        if agent.waitTime >= 30: boardingReserved = 0
    if stopTime >= D:                                               // :331-388  DEPART
        doors closed
        if stopB.docked == self: play doorCloseSound; stopB.docked = 0
        if stopB.docked == 0: stopB.sinceDeparture = 0
        if HELI: travelDistance += |stopA.segment.start.endPos - stopA.segment.end.startPos|
        else:    agent.pathStepsLeft = nextLegSteps; if nextLegSteps == 0: agent.flags |= 5
        agent.flags &= ~0x400; nextLegSteps = 0
        if stopA and stopB.segment.start != stopA.segment.end:      // platform change at this stop
            if agent.curNode is water (class 0x8000):
                teleport agent to stopB.segment.start (FUN_10107f20); pathStepsLeft += lengthNodes/2
            else:
                agent = reverseConsist(agent, stopB.segment.start)  (FUN_101101e0)
                pathStepsLeft += trunc(chainLength/12 + 0.5)       // @0x101157e4
        stopA = stopB; stopB = stopB.next or line.first
        if HELI: buildFlightPath()
        stopTime = 0; stopBPlatform = stopA.segment.end; bellRung = announced = 0
    elif stopTime >= 1:
        if stopTime <= D - 1:                                       // doors fully open: the boarding window
            if stopB.docked == 0: stopB.docked = self
            doorPos = doorOpenStart + doorOpenLen
        else:                                                       // closing
            if stopB.docked == self: play doorCloseSound; stopB.docked = 0
            doorPos = doorCloseStart + (stopTime - D + 1) * doorCloseLen
    else:                                                           // opening
        doorPos = doorOpenStart + stopTime * doorOpenLen
    (door set 2 = doorOpenStart2/Len2/doorCloseStart2/Len2 when stopBPlatform has flag 0x40000)
    setDoorAnimation(doorPos)                                       // FUN_101081f0
    return
breakdownRoll()                                                     // §4.5
```

Timing summary for one stop, with `D = clamp(H + 4 − t_since, 4, 15)`:

| stopTime | doors | `stopB.docked` | passengers |
|---|---|---|---|
| 0 | open sound | — | — |
| 0 … 1 | opening (`doorOpenStart + t·doorOpenLen`) | — | — |
| 1 … D−1 | open | self (if free) | citizens may commit while `stopTime < 10` (§5.4) |
| held at D−1 | open | self | while `boardingReserved ≠ 0`, at most until the agent has stood 30 s |
| D−1 … D | closing, close sound | cleared | — |
| ≥ D | depart | — | — |

Headway regulation: a vehicle arriving soon after the previous departure dwells longer (up to 15 s); one arriving
after the full headway dwells 4 s. The second vehicle of the same line at the same stop does not open its doors
while another is docked, unless it already holds flag 0x400 [obs `:304-308`].

### 4.4 Terminus behaviour [obs/inf]

Rings never end, but at a terminus the outbound segment usually starts on the other platform (or the other track at a
metro end station). Then rail vehicles and buses call `FUN_101101e0`, which re-links the agent chain so that the last
car becomes the head and returns the new head [inf from structure: it re-parents `prev`, flips flag 2 if any car was
reversed, re-seats each car on `FUN_100e2630(curNode)` — the opposite-direction node — and moves the cursor to the
new head]. Boats teleport to the outbound start node (`FUN_10107f20`: release all queued nodes, `curNode = start`).

### 4.5 Condition, breakdowns, repair [obs]

Roll (`0x101152a0.c:435-451`, constants `@0x101160d0` = 0.99999, `@0x10116130` = 0.001):

```
if condition < 0.99999:
    breakdownClock += dt
    if breakdownClock >= 1:
        breakdownClock -= 1
        r = (rand()/32767) * (1/(1 - condition) - 0.99999)      // FUN_10083250(0, X)
        if r < 0.001:
            broken = 1; stopTime = -1
            if agent.curNode == stopB.segment.start: agent.flags |= 0x10000
```

So per simulated second `P = min(1, ⌈32.767/X⌉/32768)` with `X = 1/(1−c) − 0.99999`; ≈ `0.001·(1−c)/c` for
moderate `c`. `c = 0.99999` or more never breaks; `c → 0` breaks every second. The roll consumes one `rand()` per
second per active spawned vehicle with `c < 0.99999`. `condition` itself is script-maintained (`player.script:312-313`:
`target = 1/(1 + travelDistance/(200000·(0.1+0.9·reliability)·(0.1+0.009·maintenance)))`, `c = 0.8c + 0.2·target`).

Broken cycle (`0x101152a0.c:249-298`): the vehicle keeps driving until `pathStepsLeft == 0` and its queue is short
(heli: until on the stop node), i.e. it stops at the next stop, but **does not dock**:

```
agent.flags |= 0x10000
if stopTime < 0:                       // opening doors, stopTime runs -1 -> 0
    stopTime += dt; doors = doorOpenStart + min(stopTime+1, 1)*doorOpenLen
    if stopTime >= 0: stopTime = 60; fire onBroken          // script overwrites stopTime = 120 - 100*employeeHappiness[2]
else:                                  // repair countdown
    stopTime -= dt
    if stopTime > 0: doors open, closing during the last second
    else: broken = 0; agent.flags &= ~0x10000; stopTime = 0; boardingReserved = 0;
          agent.waitTime = 0; doors off; fire onFixed
```

While `agent.flags & 0x50000` (broken-halted or crashing) and speed < 0.1, every rider is dumped (§5.6). After the
repair the normal stop cycle runs at that stop. An unspawned broken vehicle is repaired after 60 s without driving
(`:32-43`). `linepanel.script:409-419` charges the repair cost in `onBroken`.

### 4.6 Despawn and failure events [obs]

* Agent flag 1 (path stale, set by the movement code when the graph changed): `travelDistance += (stepCount −
  pathStepsLeft)·6`; re-path from the last queued node to the current leg's end (or to `stopB.segment.end` if the
  platforms match), re-run the leg path; keep running if the path has steps and `agent.waitTime ≤ 120`; otherwise
  despawn [`:456-512`].
* Crash flag 0x40000: despawn once `agent.waitTime > 60` [`:454`].
* Despawn [`:514-521`]: fire **onUnspawn**, undock, free the agent chain, `stopTime = 0`. The vehicle then goes back to
  §4.2 and re-enters at the first free platform of the ring.
* **onCannotReach(pos)**: A* failure for a leg; `pos` = start position of the unreachable platform node (vec3)
  [`:71-72,117-118,210-211`]. **onJammed()**: agent stood still > 180 s outside the leg-change branch [`:194-197`].
* `travelDistance` grows by 6 per path node at each leg change (so saved values are multiples of 6, e.g. 2196 in
  `scenario01/gamedata.script`) and by the straight distance per helicopter leg (fractional values, scenario06)
  [`:203,344,458`].

### 4.7 Helicopters [obs]

* At spawn and departure `FUN_10113280` builds the leg as `N = 5 + trunc(d/20)` dynamic air nodes (flags
  `0x2004000`) on a cubic Bézier from the departure pad to the arrival pad, with the inner control points lifted
  100 units and pushed 50 units along the pad directions [`0x10113280.c:50-79`, `@0x101133c3` = −0.05]. Node speed
  limit `6 − 5·(2t−1)^4` (1 at the pads, 6 mid-flight), curve factor 30 [`:137-140`].
* Separation `FUN_10110900`: the next nodes are normally left unreserved; if another `gsTransportVehicle` agent
  bound to the same stop is within 30 units (`dist² < 900`) of the next 3–4 nodes, they are reserved for this one
  [`0x10110900.c:20-72`].
* Arrival requires `agent.curNode == stopB.segment.start` [`0x101152a0.c:307-308`].

---

## 5. Passengers (citizen side)

### 5.1 Transit route search: `FUN_1011a200` [obs; costs partly from bytes]

Called by the mode choice with `(origin cell, destination cell, walkSpeed = citizenModel.speed (+0x288),
route*, priceWeight = citizens+0x938[class], directOnly = false, mask = all)` [`0x100d18f0.c:241-243`,
`0x1011bf00.c`]; `routeExists` calls it too (batch-A). `walk(d) = d_cells · cellSize / walkSpeed`
[`0x1011a200.c:61`]. `R(a,b)` = `rand()/32767·(b−a)+a` (one `rand()` per call). `price(line) = max(ticketPrice − 200, 1)`.

* **Start nodes** (`FUN_10114560`): nodes whose stop's catchment covers the origin; line type in mask, line active,
  node and node.next not temporary [`:72-86`].
  `cost0 = walk(|origin − stop|) + 6·R(0, 1 − 0.8·stop.attr) + R(1, 2 − 0.8·line.effAttr)·seg.travelCost
  + price·priceWeight`; `paidTypes = 1 << type` [`:93-103`, bytes `@0x1011a344..a3b7`].
* **Ride edge** n → n.next (n.next and n.next.next not temporary, not settled):
  `+ R(1, 2 − 0.8·line.effAttr)·n.next.seg.travelCost + 5` [`:158-173`, `@0x1011a65a` = 5].
* **Transfer edge** n → m for m in n.transfers (only if !directOnly; m's line in mask and active, m not temp/settled,
  m.next not temp): `+ walk(|m.stop − S|) + 6·R(2, 3 − 0.8·m.stop.attr) + R(1, 2 − 0.8·m.line.effAttr)·m.seg.travelCost
  + (type ∉ paidTypes ? price(m.line)·priceWeight : 0)`, `paidTypes |= bit` [`:180-220`; walk factor confirmed from
  bytes `0x1011a829-0x1011a83c`]. `S` = stop where n's ride ends.
* **Goal:** when n is settled, if `S = n.next.stop` lies in a ±50-cell box around the destination:
  `total = cost(n) + walk(|S − dest|)`; keep the minimum. No catchment test at the destination. The search stops when
  the popped cost ≥ best total [`:141-157`].
* Heap entries are `{−cost, paidTypes, node}` (max-heap of negated costs). Settled nodes get flag 4; touched nodes are
  reset afterwards (`flags &= ~6`, `bestCost = −1`) [`:250-253`].
* **Result:** `route.nodes` = path from the start node to the best node (inclusive), each node's refcount +1;
  `route.cost = best total` [`:231-248`]. The last node is the one *before* the alighting stop.

The citizen takes transit if a route exists and `route.cost` beats its car and walk costs; it then counts
`canTravel`, and `priceTooHigh` when the route would win without the fare (`0x100d18f0.c:266-319`; details in the
citizens document).

### 5.2 Getting to the stop and waiting [obs: `0x100cc990.c:42-71,214-231`]

* The citizen walks (character agent) to `route.nodes[index].item`. On arrival, if that node is not deleted, it
  starts waiting: `waitNode = node`, node refcount +1, `node.waiting++`, `stop.passengerCount++`, `waitTicks = 0`.

### 5.3 Patience, mood and the price check [obs: `0x100cc990.c:238-351`, `0x101110c0.c`]

Per citizen update while waiting and no vehicle with room is docked (`docked == 0 or docked.broken or
docked.capacity ≤ docked.boardingReserved + docked.passengerCount`):

```
patience = (citizen.id & 0xff) + 256                                 // 256..511 updates ≈ seconds
if (waitTicks & 7) == 0 and line.type not in paidTypes and destination != 0:
    walkTime = |stop item centre − destination centre|_cells · cellSize / walkSpeed
    if 0.4·walkTime < (line.ticketPrice − 300) · priceWeight[class]: waitTicks = max(waitTicks, patience)
waitTicks++
if waitTicks == patience/2:   impatient (node+0x5c, stop+0x164 ++; flag 0x1000000)
if waitTicks == patience*3/4: impatient → angry (node+0x60, stop+0x168; flag 0x2000000)
if waitTicks >  patience:     give up: walk back to the trip origin (c+0x28), or, when waiting at a transfer,
                              to the stop where the previous ride ended (nodes[index-1].next.item);
                              route.index = count, tiers undone, node+0x64 / stop+0x16c ++, flag 0x4000000;
                              when that walk ends the trip is closed (FUN_100cc6c0)          // :305-351
```

Stop mood = 3 if `gaveUp ≥ 2`, else 2 if `angry > 0`, else 1 if `impatient > 0`, else 0; **onMoodChange(mood)** fires
on change [`0x101110c0.c:13-31`]. Counters are live counts of citizens still attached to the node (a giving-up citizen
stays attached while walking away, `FUN_100cbc80`). `getPassengersByLine()` returns `{line: waiting | tierBits}` with
tierBits 0x3000000 / 0x2000000 / 0x1000000 by the highest non-empty tier of that node [`0x10116300.c:48-68`].

### 5.4 Boarding rules [obs: `0x100cc990.c:353-445`]

A waiting citizen (not flagged 0x100 or 0x4000000) commits to board when all hold:

1. `node.docked = V` exists, not deleted, not broken, and `V.capacity > V.boardingReserved + V.passengerCount`;
2. its character is within 3.0 world units of its walk target (`FUN_100c7870`) [`:354-357`];
3. `V.stopTime < 10` [`:359`];
4. priority: if the node has angry waiters the citizen must be angry; if it has impatient waiters the citizen must
   be impatient or angry [`:362-367`].

Then `V.boardingReserved++`, flag 0x100, and the character walks to the nearest "enter" door marker (model sub-object
flag 0x20000) over all cars (`FUN_10108350`); tier counters are undone. When the walk ends: if the node still has a
docked vehicle that is not deleted and the line is active → **board** [`:405-445`]:

```
remove character; citizen.vehicle = V; V.passengerCount++; V.boardingReserved--
node.boarded++; node.waitSum += waitTicks; line.avgWait = 0.95·line.avgWait + 0.05·waitTicks
leave the stop (FUN_100cbc80: node.waiting--, stop.passengerCount--, tiers, refcount)
alreadyPaid = paidTypes & (1 << line.type); paidTypes |= that bit
fire V.onGetPassenger(citizenController, float(waitTicks), alreadyPaid)
```

If the vehicle left in the meantime, the trip is aborted (`FUN_100cc6c0`) [`:405-411,451-452`]. Nothing checks *which*
vehicle; any vehicle docked at the node is of the right line and direction because nodes are per (line, stop).

Script side (`linepanel.script:346-406`, not re-documented here): the fare and fare dodging are applied only when
`!alreadyPaid` — one ticket per transport type per trip, matching the route cost model.

### 5.5 Riding, alighting, transfers [obs: `0x100cc990.c:137-212`]

While riding, `route.index` points at the next route node:

```
if V.stopB.docked != V:                        // driving, or waiting with doors closed
    if index < count and V.stopA == nodes[index]: index++     // passed the node we boarded/rode
    return
// V is docked at stopB
if index < count:
    if V.stopB == nodes[index]: return         // intermediate stop of the planned ride: stay on
    target = nodes[index].item                 // a transfer: next leg boards there
else: target = destination
alight at V.stopB: character at the nearest "exit" door marker (flag 0x10000, FUN_101084f0),
    V.passengersTransported++, line.passengersTransported++, V.passengerCount--, flag 0x200
then walk to target; on arrival either wait at nodes[index] (transfer) or end the trip
```

Alighting therefore happens at the first docking whose node is not the next route node. Since the route lists all
nodes up to the one before the alighting stop, this is the alighting stop or the transfer stop.

### 5.6 Forced alighting [obs: `0x100cc990.c:75-136`]

If the vehicle is broken-halted or crashing (`agent.flags & 0x50000`) and nearly stopped (speed < 0.1): each rider gets
off from a random car (`rand() % carCount`) at the walk node nearest to `stopB.item` (or to the car if crashing / no
stop); `passengerCount--`; no `passengersTransported` credit; flag 0x4000000; state → 0x40 (re-plan from here,
`FUN_100cd5c0`); the route is freed. A vehicle that is deleted, inactive or unspawned aborts its riders' trips
[`:73-74`].

---

## 6. Script API notes (transport classes)

| member | behaviour | evidence |
|---|---|---|
| `gsTransportLines.createLine()` | new 400-byte line, attached to the manager | `0x10118940.c` |
| `getStopCount()` / `getStop(i)` | size / element of the stop vector (no bounds check) | `0x10114530.c`, batch-A |
| `stopExists(type, name)` | any stop of `type` whose item custom name (`item+0x60`) equals `name` | `0x101165f0.c:44-58` |
| `routeExists`, `countStops`, `countLines` | see batch-A; `routeExists` uses §5.1 with `walkSpeed` arg | `0x1011b8e0.c` |
| `line.getStop(i)` | stop controller of the i-th node, null if out of range | `0x10110b30.c` |
| `line.getPassengersTransported(i)` / `getWaitingTime(i)` | `node.boarded` / `node.waitSum` (int ticks) | `0x10113750.c`, `0x101137a0.c` |
| `line.hasTempStops()` | any node with flag 1 | `0x10110b80.c` |
| `line.calculateCoverage()` | fraction of all `GRID_ITEM_BUILDING` items whose cell corner lies strictly inside any stop's catchment (positions use item `x,z`, not centres); 0/0 if no buildings | `0x101139c0.c:57-122` |
| `line.countConnectedStops(mask)` | nodes whose transfer list contains a node of an active line in `mask` | `0x10116190.c` |
| `line.calculateLength()` | batch-A | `0x101137f0.c` |
| `vehicle.isSpawned()` | agent ≠ 0 | `0x10113260.c` |
| `gsGridVehicleController.getCitizen()` | owner citizen of the agent (`agent+0x30`), for cars | `0x10107280.c` |
| `getID()` | the agent's 16-bit colour seed (`agent+0x80`) | `0x10107bd0.c` |
| `playSound(s)` | play at the agent | `0x10107b60.c` |

Events: `onSpawn()`, `onUnspawn()`, `onGetPassenger(citizen, waitTime, alreadyPaid)`, `onBroken()`, `onFixed()`,
`onCannotReach(pos)`, `onJammed()` on vehicles; `onMoodChange(mood)` on stops; `onCrashBegin(vehicle)` /
`onCrashEnd(vehicle)` on `gsGridVehicles`. Firing helper `FUN_1001f960(obj, id, argc, argv)` / `FUN_1001faf0(obj, id,
arg)`.

---

## 7. Background traffic module `gsGridVehicles` (`grid+0x20c`)

### 7.1 Fields (module offsets; tick `this` = +0xcc)

| off | name | meaning | evidence |
|---|---|---|---|
| 0xd4 | grid | | `0x1010f1e0.c:35` |
| 0xd8 | tick | 16-bit update counter passed to the movement code | `:141` |
| 0xdc..0xe8 | passengerTrains | consists `{unit* units; int n}`, unit = `{model, reversed}` | `0x1010a360.c` |
| 0xec..0xf8 | cargoTrains | same | `0x1010a710` (same shape) |
| 0xfc..0x108 | airplanes | model list | `0x1010aac0.c` |
| 0x10c..0x118 | boats | consist list | `0x1010abe0`, `0x1010ce70.c:43,76-83` |
| 0x11c..0x128 | trains | live head agents | `0x1010ae20.c:262-273` |
| 0x12c..0x138 | aircraft | live agents | `0x1010b430.c:442-451` |
| 0x13c..0x148 | boatsLive | live head agents | `0x1010ce70.c` |
| 0x14c | rrIndex | next bucket of the round robin | `0x1010f1e0.c:215` |
| 0x150 | rrAccum | fractional bucket budget | `:147-156` |
| 0x154 | time | accumulated dt | `:157` |
| 0x158..0x164 | trainSpawn | mode (1 passenger, 0 cargo), pending entry node, exit node, retry counter | `0x1010ae20.c:36-149` |
| 0x180..0x190 | airSpawn | pending runway nodes of the two aircraft spawners | `0x1010c190.c:76-130` |

### 7.2 Tick `FUN_1010f1e0` [obs]

1. **Trains:** if `#trains < (graph+0xfc + graph+0x114) >> 2` → `FUN_1010ae20` [`:29-38`]. The two graph counts are the
   passenger and cargo rail entry points [inf].
2. **Boats:** if `#boats < trunc(sqrt(graph+0x15c))` → `FUN_1010ce70` [`:39-54`] (`+0x15c` = number of water entry nodes [inf]).
3. **Aircraft:** cap `min(graph+0x12c, 2)`; spawner `FUN_1010b430` or `FUN_1010c190` (50/50 via `rand() & 1`
   unless one of them has a spawn pending) [`:55-77`].
4. **Train list** [`:84-110,263-275`]: wheel animation (`FUN_101081f0(agent, dt, speed)`). While the head still has
   more queued nodes than its length: free the train if stalled > 60 s or flag 1. Once the queue has drained to the
   head's length (end of path): a single car is freed; if the head stands on a node with flag 0x400000
   (`GRID_PATH_EXTERNAL`) the head is freed and the next car becomes the head with `pathStepsLeft = 10 000 000` and
   the old speed (trains leave the map car by car); otherwise the train is freed once it has stood still at all.
5. **Aircraft list:** despawn if stalled > 60 s or flag 1; while moving, landing-gear animation driven by altitude
   above 20; after stopping on the ground: doors open after 2 s, despawn after 6 s or if above 20 units [`:117-262`].
6. **Boat list:** despawn when the queue has drained to the hull length, stalled > 60 s, or flag 1 [`:121-139`].
7. **Agent update:** `budget += buckets·dt·5`; `k = min(trunc(budget), buckets)`; `budget −= k`. Buckets whose
   visible flag (`slot+0x24`) is set update every agent every tick with the real `dt`; other buckets are updated only
   inside the round-robin window `[rrIndex, rrIndex+k)` (wrapping) with fixed `dt = 0.2`; `rrIndex = (rrIndex+k) mod
   buckets` [`:140-215`]. Effect: every off-screen agent advances 5 times per simulated second in 0.2 s steps.
   Agents with `pathStepsLeft == −1` (trailers, parked) are skipped; `FUN_101078d0` moves agents between buckets.

### 7.3 Spawners [obs]

* **Trains `FUN_1010ae20`:** mode passenger if `rand()` even, else cargo (needs a non-empty consist list). Entry and exit
  path nodes are chosen uniformly from the graph's lists for that class (`+0xf4/+0x100` passenger, `+0x10c/+0x118`
  cargo); the entry must be free, retries up to ±15 ticks, A* entry → exit. Consist chosen uniformly;
  `lengthNodes = trunc(Σ len / 6 + 0.99)`; cars placed behind the head at `posOnNode = trunc(−65536·Σlen)`; colour seed
  `rand() & 0xffff` per car [`0x1010ae20.c:57-281`, `@0x1010b172`, `@0x1010b32b`].
* **Boats `FUN_1010ce70`:** random free water entry (`graph+0x158`, count `+0x15c`), random exit (`+0x164`, count
  `+0x168`), `|entry − exit|² ≥ 40000` (≥ 200 units); random consist; boats whose model `height` (`+0x240`) > 1 reject
  paths that pass a non-water path node with −8.5 < y < 10.5 (low bridges) [`0x1010ce70.c:43-106`].
* **Aircraft:** two spawners (approach/departure [inf]) using runway node lists `graph+0x134..0x138` and the model list
  `+0xfc` [`0x1010b430.c:76-111,222-229`, `0x1010c190.c:71-130`]; not traced further.
* **`spawnVehicle(name, x, y, z)`:** one agent of model `name` on the nearest path node to (x, y, z) if that node is
  unoccupied; no path, not listed [`0x10108e70.c`]. Unused by the shipped scripts.
* **`unspawnVehicles()`:** frees every agent in every grid bucket (`FUN_100d4970` per slot) [`0x101073e0.c`].
* **`setPassengerTrains/setCargoTrains(...)`:** no args clears the list; each arg is a model name or an array of names
  (one consist); a leading `!` marks a reversed car. Args are *appended* [`0x1010a360.c:38-176`].
  `setAirplanes(...)` stores model names [`0x1010aac0.c`]; `setBoats` (`0x1010abe0`, 572 bytes) stores boat consists
  [inf: same shape as trains, read by `0x1010ce70.c:79-106`].

### 7.4 Crashes [obs]

Detection is in the movement update: an agent with flag 0x800 (stop-and-go), not yet crashing, `occupiedCount < 2`,
whose next ring node is not free when it needs it fires **onCrashBegin(controller)** and calls `FUN_1010a260` on itself
and on the blocking agent(s) [`0x1010de50.c:316-336`]. `FUN_1010a260` marks every car of the chain 0x40000 and sets
`crashOffset = (R(−1,1), 0, R(−1,1)) · dt·speed·0.4` [`0x1010a260.c:16-35`]. A crashing agent decelerates 4/s; when its
speed drops to ≤ 1 (immediately if it already was), `FUN_10109d70` places a `crash` grid object (timer 60) at the spot and, with probability 1/4
(`rand() & 3 == 0`), schedules an emergency visit [`0x10109d70.c`, `0x1010de50.c:283-299`]. **onCrashEnd(controller)**
fires when a crashing agent is destroyed [`0x10107dd0.c:18-20`]. Transport vehicles that crash dump their riders
(§5.6) and despawn after 60 s (§4.6).

---

## 8. Corrections to existing documents

1. `save-format/vehicles-movement.md` agent flag 0x10000 is "broken down and halted", not "dwelling at a terminus"
   (`0x101152a0.c:255,446`).
2. `save-format/lines-timeline.md` unnamed fields: line `f168` = headway estimate (s); `f174` = effective
   attractiveness; `f180` = EMA of boarding wait ticks. LineStop `f34` = seconds since last departure; `waitingTime` =
   Σ wait ticks of boarders. LineVehicle `f164` = re-path timer while unspawned; `i168` = next-leg step count applied at
   departure; `i174` = boarding reservations; `b184`/`b185` = vehicle bell rung / stop bell rung. `condition` is not
   saved but is restored by the script from `$ownVehicles` (`linepanel.script:224`).
3. `handlers/batch-A.md` (routeExists): the "unnamed line float `+0x174`" is the effective attractiveness; node buckets
   `+0xf0`/`+0xf4` index nodes by boarding stop / alighting stop; transfers come from per-node lists (§3.4).

---

## 9. Open questions

1. **Bucket shift.** `FUN_10111250(lines, 0, 0, 3)` in the clear path suggests 8×8-cell buckets, but the call that
   sizes the index for a loaded grid was not traced; the ±50-cell search box only covers a 150-unit catchment if
   `cellSize ≥ 3`.
2. **Platform change mid-leg.** When the next leg starts on another platform, the pre-loaded cursor (§4.3) starts at a
   node other than the agent's ring tail until the reversal at departure. Whether the movement code pulls any of those
   steps before `pathStepsLeft` reaches 0 (and how `FUN_101101e0` re-bases the cursor) needs the movement analysis.
3. **`start.flags |= 0x1000000`** on the spawn platform (§4.2) is set every tick while waiting to spawn and is cleared
   only for the first stop on deactivate/destroy; who clears it after a successful spawn (movement code when a 0x200
   agent leaves, cf. `0x101101e0.c:111-113`) is not confirmed.
4. **`boardingReserved` can go negative**: it is zeroed after 30 s standing, yet citizens already walking still
   decrement it on boarding. A negative value also holds the doors until the next 30-s reset. Unclear if intended.
5. **Stop counter drift** from the `impatient`-for-all subtraction in `FUN_10119980`/`FUN_10116b10` (§3.3): does the
   loader (`0x100d31a0.c:325-360`, which rebuilds the counters) hide it in practice?
6. **Aircraft spawners** `FUN_1010b430` / `FUN_1010c190` (approach vs. take-off, runway lists, gear/door timing) and
   `setBoats` were only skimmed.
7. **Units.** `waitTicks` ≈ seconds assumes the citizens module updates each citizen once per second (`N·dt` per tick);
   the travel cost is assumed to be seconds; the 6-unit path node length is inferred from the `/6`, `×6` constants.
8. **Door markers.** The enter/exit marker flags (0x20000 / 0x10000 on model sub-objects at `model+0x2a0`) were read
   from `FUN_10108350`/`FUN_101084f0`; how they are authored in `.object` files was not checked.
