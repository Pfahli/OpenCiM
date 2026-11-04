# Boats, aircraft, generated water lanes and inline air lanes

How `GSystem.dll` builds the water-lane lattice, spawns and moves background boats and aircraft, and how water-bus and
helicopter line vehicles get their paths. Builds on [`pathfinding.md`](pathfinding.md) (node layout §1.2, linking
§2.3, endpoint sets §2.5, lane search §4), [`grid-traffic.md`](grid-traffic.md) (manager tick §6.1, mover §6.2-6.4a)
and [`transport.md`](transport.md) (line vehicles §4). Save-file side: [`../save-format/vehicles-movement.md`](../save-format/vehicles-movement.md).

Conventions: *obs* = read in the pseudocode or bytes; *inf* = inferred. Citations are
`decompiled/GSystem-v2/functions/<file>:<line>`; `L:` = Linux build (`decompiled/Linux/functions`). Assembly
excerpts quoted as `@<address>` come from a `tools/ghidra/DumpAsm.java` run on the saved CiM project. Grid fields:
`W` = width (`grid+0x13c`), `L` = length (`grid+0x140`), `cs` = cellSize (`grid+0x154`, 3.0 in every shipped map).
World `x = (cellX − W/2)·cs`, `z = (cellZ − L/2)·cs`. Speeds are agent speeds in cells/s (the mover moves
`3·speed` world units per second, grid-traffic.md §6.2).

---

## 1. Overview

| Thing | Built / spawned by | Lives in | Saved as |
|---|---|---|---|
| Water lattice (one-way water lanes over open water) | `gsGrid::updateSpecialSegments` `FUN_100c1840` on load and on save | lane graph + free-node list `grid+0x254` | not stored; refs `0x7f000000 \| i` (§3) |
| Background boats | `gsGridVehicles::spawnBoat` `FUN_1010ce70` | vehicles `+0x13c` list | agents + list `0x07040500` |
| Landing aircraft | `spawnLandingAirplane` `FUN_1010b430` | vehicles `+0x12c` list | agents + inline nodes, list `0x07040400` |
| Departing aircraft | `spawnDepartingAirplane` `FUN_1010c190` | same | same |
| Water-bus line vehicles (line type 3) | transport tick `FUN_101152a0` | lattice + dock lanes | agents (flag 0x200) |
| Helicopter line vehicles (line type 4) | `FUN_10113280` (flight path), `checkHelicopterPaths` `FUN_10110900` | inline air nodes | agents + inline nodes |

Names: the Linux build calls `FUN_1010b430` `gsGridVehicles::spawnLandingAirplane` (`L:0x081b27a0`) and
`FUN_1010c190` `spawnDepartingAirplane` (`L:0x081b0f60`) *obs* (same set offsets and field use, §5).

---

## 2. The generated water lattice — `gsGrid::updateSpecialSegments` (`FUN_100c1840`, `L:0x08142d00`)

### 2.1 When it runs

Only two callers *obs*:
* `gsGrid::readGridChunk` (`0x100c3310.c:119`), inside the terrain chunk `0x07010000`, right after the terrain flags
  are read. The network chunk `0x07020000` (items) comes later in the file, so items are placed onto an existing
  lattice (§2.6).
* `gsGrid::saveToFile` (`0x100c3840.c:109`), after the terrain chunk is written and before the network chunk and the
  node-ref map (`:194-199`) are built.

It is **not** run on terrain edits (water toggle, height brushes) or item placement *obs* (no other caller in either
build, no vtable or script reference). New water painted in the editor gets lanes at the next save or load.

### 2.2 Pseudocode

```
updateSpecialSegments(grid):                                             0x100c1840.c
  // 1. take lines whose effective status has bit 2 off-line (transport.md 2.2: "path display on")   :63-77
  S = { line in grid.lines.modules (lines+0x4c vector) : line.flags(+0x6c) & 2 }
  for line in S: line.clearStatus(2)          // vfunc 0xd8 -> statusUpdated clears segment bit 1, rebuilds meshes
  // 2. delete the previous lattice                                                                 :78-87
  for n in grid.freeNodes (head first, link n+0x0c): graph.removeSegment(n); n.flags |= 0x80; n.release()
  grid.freeNodes = null
  // 3. new lattice                                                                                 :88-541
  BX = W >> 2; BZ = L >> 2
  for bz in 0 .. BZ-1:                     // z outer
    for bx in 0 .. BX-1:                   // x inner
      if blockIsOpenWater(bx, bz): emitBlock(bx, bz)
  // 4. re-raise the lines: statusUpdated re-resolves each segment's stop nodes (FUN_10111410) and re-runs the
  //    lane search (FUN_100e5fe0, ignoreTraffic 1) for every segment                               :548-579
  for line in S (set order = pointer order): line.raiseStatus(2)      // vfunc 0xd4; 0x10113e00.c:15-27
```

`emit(node)` everywhere below = `node.ownerNext(+0x0c) = grid.freeNodes; grid.freeNodes = node;
graph.addSegment(node)` (`FUN_100e4dd0`: bucketing, successor/predecessor links, crossings, endpoint sets) *obs*.
Each node is linked **as it is created**, so link order follows creation order (pathfinding.md §2.3).

### 2.3 Which blocks

```
water(x, z) = terrain[(z·L + x)].flags(u16 at +2) & 0x2000          // row stride is L, see quirk Q1
blockIsOpenWater(bx, bz):                                           0x100c1840.c:96-162; L:0x08142d00.c:209-259
  x0 = 4·bx; z0 = 4·bz
  all water(x0..x0+3, z0..z0+3)                                    // the 4×4 block
  and (bz == 0    or all water(x0..x0+3, z0−1))                     // 1-cell strips on the 4 sides,
  and (bx == 0    or all water(x0−1, z0..z0+3))                     // skipped at the map border
  and (bz == BZ−1 or all water(x0..x0+3, z0+4))
  and (bx == BX−1 or all water(x0+4, z0..z0+3))
```
*obs.* Diagonal neighbour cells are not tested. The flags are the ones in memory at that moment: at load the raw
file flags, before the terrain builder's dry-out (`../renderer/terrain.md` §"Dry-out") clears `0x2000` on shallow
cells *obs (call order)*. A port must build the lattice from the flags exactly as loaded (§2.8).

### 2.4 Nodes of one block

Local cell coordinates relative to `(x0, z0)`, all points at `y = −1.25·cs` (the water surface `−0x140`,
terrain.md) *obs `:187,204,…`*. Parities: `p = bx & 2`, `q = bz & 2`, `r = (bx+bz) & 2`, `r2 = (bx−bz) & 2`
(each 0 or 2). Creation order is the row order of the table *obs*.

| # | node | condition | p0 (cells) | p1 (cells) | yaw θ (§2.5) | flags | evidence |
|---|---|---|---|---|---|---|---|
| 1 | **A** (z-lane, x = 2) | always | (2, 2p) | (2, 4−2p) | (2−p)·π/2 | 0x8000, START/END on the first/last block row (below) | `:166-221` |
| 2 | **B** (x-lane, z = 2) | always | (2q, 2) | (4−2q, 2) | (3−q)·π/2 | 0x8000, START/END on the first/last block column | `:222-280` |
| 3 | A→B | always | A.p0 | B.p1 | q0 = A.q0, q1 = B.q1 | 0x8030 | `:281-294` |
| 4 | B→A | always | B.p0 | A.p1 | B.q0, A.q1 | 0x8030 | `:295-308` |
| 5 | **E** (anti-diagonal) | `((bx ^ bz) & 1) == 0` | (4−2r, 2r) | (2r, 4−2r) | (2−r)·π/2 − π/4 | 0x8000 | `:309-363` |
| 6,7 | A→E, E→A | and `p == r` | A.p0 → E.p1; E.p0 → A.p1 | | from the parts | 0x8010 | `:364-393` |
| 8,9 | B→E, E→B | and `q != r` | B.p0 → E.p1; E.p0 → B.p1 | | | 0x8010 | `:394-423` |
| 10 | **F** (main diagonal) | `((bx ^ bz) & 1) == 0` | (4−2r2, 4−2r2) | (2r2, 2r2) | (4−r2)·π/2 + π/4 | 0x8000 | `:424-477` |
| 11,12 | A→F, F→A | and `p != r2` | A.p0 → F.p1; F.p0 → A.p1 | | | 0x8010 | `:478-507` |
| 13,14 | B→F, F→B | and `q != r2` | B.p0 → F.p1; F.p0 → B.p1 | | | 0x8010 | `:508-537` |

Fields of every lattice node *obs*: `length = |p1 − p0|` (`FUN_10084ff0`), `speedLimit = 6.0` (`0x40c00000`),
`curveFactor = 20.0` (`0x41a00000`), `curvature = 0` (`:213-218`, `:272-277`, …). The connectors ("X→Y") are
`gsGridPathSegment::combine(new, X, Y)` (`FUN_100e1730`): flags, p0, q0, speedLimit, curveFactor, curvature from X,
p1 and q1 from Y, length = chord; then the flags word is overwritten with 0x8030 / 0x8010 *obs `0x100e1730.c:14-33`*.
0x10 = `INTERSECTION`, 0x20 = `LANE_CHANGE`: corner turns cost +4 +6 in the lane search, diagonal hops +4
(pathfinding.md §4.2), and both count as junction nodes for the mover (grid-traffic.md §6.3) *inf*.

`START`/`END` *obs `:175-180, 233-238`*:

```
A: if bz == 0:      flags |= (p == 0) ? START : END        // lane flows into the map at z = 0 when p == 0
   elif bz == BZ−1: flags |= (p == 0) ? END   : START
B: if bx == 0:      flags |= (q == 0) ? START : END
   elif bx == BX−1: flags |= (q == 0) ? END   : START
```

So every border lane pointing into the map is a `START`, every one pointing out an `END`. Columns of A-lanes flow
+z when `bx & 2 == 0` and −z otherwise: **bands two blocks (8 cells, 24 units) wide alternate direction**; same for
rows of B-lanes with `bz & 2` *obs*. Lengths with cs = 3: A, B 12; A→B, B→A 8.49; E, F 16.97; diagonal connectors
13.42 units *inf (from the coordinates)*.

Connectivity *inf from geometry + pathfinding.md §2.3*: A of block `(bx, bz)` ends where A of `(bx, bz±1)` starts
(same column parity), so A-lanes chain into full columns and B-lanes into rows. E and F chain from corner to corner
into diagonals across blocks of the same checkerboard colour (`(bx+bz)` and `(bx−bz)` are constant along them, so is
their direction). Turns A↔B happen only through the corner connectors, A/B↔diagonal only through the 0x8010
connectors, whose conditions pick exactly the directions that are compatible (dot > 0.1). A lane whose next block is
not open water simply ends; the lane search prunes such dead ends (pathfinding.md §4.3).

Average per open block: 4 nodes + ½·(2 diagonals + 4 connectors) = **7** *inf* (both-even blocks get one A pair
and 0 or 4 B connectors, both-odd blocks 0 or 4 A connectors and one B pair).

### 2.5 Orientations

`q = gsQuaternion(θ, axis)` with the axis `DAT_10197d88 = (0, −1, 0)` (`0x10151d90.c:12`; `L:` `gsNegYVector`)
*obs*. In the engine's convention this yields the forward `q·(0,0,−1) = (−sin θ, 0, −cos θ)`: that is the only
reading under which all six yaw formulas point along their chords *inf*. Port: store `q0 = q1 = look(+Y, p1 − p0)`
(the same rotation; only the forward axis is used by the mover's Bézier handles, grid-traffic.md §6.4a).

### 2.6 Items over water: flag 0x4000000

`gsGridItem::fixGrid` (`0x100e9730.c:441-453`), run for every item placement:

```
if item.y < 0 and item.y + item.height·256 > −0x200:          // item+0x30 < 0, item+0x30 + item+0x3c > −512
    for n in graph.getSegments(item.x, item.z, item.sizeX, item.sizeZ, mask 0x8000):   // window ±3 cells
        if item.x <= n.cellX <= item.x + sizeX and item.z <= n.cellZ <= item.z + sizeZ:  // inclusive
            n.flags |= 0x4000000                               // never cleared
```
*obs* (`item+0x3c = template.height << 8`, `0x100ef3b0.c:50`). The lane search skips successors with 0x4000000
(`0x100e5fe0.c:97`). Nothing clears the bit *obs* (no `& ~0x4000000` writer in the graph code); it disappears only
with the node at the next rebuild. Typical case: the dock platform (`objects/stations/dock-platform.script`, type
`GRID_ITEM_DOCK`, height 2) placed below ground level hides the lattice under it, and its own water paths (`t-a0`
`STOP`, `t-b0` `ALTERNATIVE_STOP`, `t-e0..j1` `LANE_CHANGE`) take over *obs flags, inf role*; the dock is
`GRID_ITEM_ALIGN_4`, i.e. aligned to the 4-cell lattice *obs `dock.script`*.

### 2.7 Quirks

* **Q1 stride.** The block test indexes the terrain with row stride `L` (`grid+0x140`), the rest of the engine with
  `W` *obs (`:101,139`; loader `0x100c3310.c:100-115`)*. Identical on square maps (all shipped maps).
* **Q2 partial blocks.** `W >> 2` and `L >> 2` drop a last partial block; on sizes not divisible by 4 the "border"
  START/END lanes stop short of the edge *obs*.
* **Q3 save loses the 0x4000000 marks.** `saveToFile` regenerates the lattice but does not re-run `fixGrid`, so after
  a save the lanes under docks are searchable again until the next load *inf from the absence of a call*.
* **Q4 save kills boats.** The rebuild deletes every lattice node. The writer's `FUN_101068f0` (`0x1010f8a0.c:200`)
  swaps each deleted node an agent holds for the new node with the same endpoints (`FUN_100e25b0`) and, because the
  replacement has the water class, sets agent flag 1 (`0x101068f0.c:42-43,72-73`) *obs*. AI boats with flag 1 are
  despawned on the next manager tick (§4.5), both in the running game and after loading that save; water buses
  re-path (transport.md §4.6). Confirmed in the samples: **every** boat and water-bus agent standing on a lattice
  node carries flag 1 (Cim2 10 agents, berlin 13, vienna 7); water buses on dock nodes do not *obs (scratchpad
  `boatcheck.mjs`)*.
* **Q5** `blockIsOpenWater` ignores heights; a 4×4 block of flagged cells anywhere (even a lake) gets lanes, but only
  border lanes carry START/END, so lakes never get boats *inf*.

### 2.8 Port recipe and validation

Port: build the lattice right after reading the terrain chunk, from those flags, before any dry-out; emit in the
order of §2.4 (bz outer, bx inner, table order); keep a vector in **creation** order and resolve free refs as
`nodes[count − 1 − i]` (the engine prepends, §3). Rebuild on save the same way (and accept Q3/Q4 or deliberately
deviate). The successor lists of lattice nodes are formed in creation order before any item exists; items added later
append (pathfinding.md §2.3).

Validation *obs*: a read-only script (`<scratchpad>/lattice.mjs`; `<scratchpad>` =
`C:\Users\Marku\AppData\Local\Temp\claude\c7349dd7da19c1f6\fe525a7e-5e90-46f7-b12c-445b2bb3c3e0\scratchpad`, which
also holds `boatcheck.mjs`, `aircheck.mjs`, `landcheck.mjs` and the `asm/` dumps) generated the lattice from the saved terrain of
Cim2, berlin and vienna (1280², 1280², 1024²; 63 504 / 55 124 / 44 420 nodes) and resolved every `0x7f…` ref of
every agent: all refs in range (max index 35 455 / 55 111 / 32 343), all water class, and all 672 consecutive
free-node pairs (prevNode → curNode → ring…) chain end-to-start within 0.01 units. This confirms block test,
geometry, creation order and list order.

---

## 3. How saves reference generated and inline nodes

Path-node refs in `0x07040200` records (`vehicles-movement.md` §1) *obs*:

| ref | meaning | writer | loader |
|---|---|---|---|
| `0x7f000000 \| i` | i-th node of `grid+0x254` counted from the head, i.e. the `(count−1−i)`-th lattice node in creation order | map built after the rebuild: `0x100c3840.c:194-199` (`i` from 0 at the head) | `0x100c3310.c:119-126` builds the array by walking the list after its own rebuild; `FUN_100b5840` returns `arr[i]` if `i < count`, else 0 (`0x100b5840.c:16-19`) |
| `0x80000000 \| i` | i-th `0x07030200` child of the same record (inline node) | `0x1010f8a0.c:220-249` | `0x1010d330.c:250-268` collects the children; a ref with bit 31 indexes them (`(ref·4)` overflows bit 31 away): curNode `:277-282`, ring `:341-349`, prevNode `:362-366` |
| `(item1 << 8) \| lane` | network node | | `FUN_100b5840` |

* Lattice nodes are never written; a loader must regenerate them **exactly** (§2.8). A null result for a ring entry
  sets agent flags `|= 5` (`0x1010d330.c:349`).
* Inline nodes are the `0x2000000` ("dynamic") nodes of aircraft and helicopters. They are not in the graph (no
  bucket, no successors, no crossings, not in any endpoint set). Each reference writes a copy (no de-duplication);
  the loader creates one node per child with refCount 1, the ring/cur/prev refs add theirs, and the creation ref is
  released after the record (`0x1010d330.c:423-426`) *obs*. Occupancy is restored by the generic
  `occupiedCount`/`reservedCount` replay (`:370-399`).

---

## 4. Background boats

### 4.1 Model lists

`setBoats(...)` (`gsGridVehicles::method_setBoats` `0x1010abe0.c`): no argument clears the list
(`vehicles+0x10c` vector, begin `+0x110`, end `+0x114`); each argument is either a string (one-unit consist) or an
array of strings (one consist, units in order); units are `{model, reversed = 0}` (8 bytes), a consist is
`{Unit* units; int n}` (8 bytes); consists are **appended** *obs `:35-111`*. Models are looked up by name in the
template registry (`grid` vfunc 0xec); an unknown name stores null and would crash at spawn *obs/inf*. No `!`
reversal prefix (unlike `setPassengerTrains`).

The only caller is `metro/scripts/game.script:447-453`, in `updateERA(year)` (run at game start `:727` and every
`onYear` `:613`), after clearing at `:391` *obs*:

| consist | models |
|---|---|
| 1 | `cargo-ship-01` (height 2 → bridge check) |
| 2 | `cargo-ship-02` (height 2) |
| 3 | `tug-01-a` |
| 4 | `tug-01-a`, `tug-01-b` |
| 5 | `tug-01-a`, `tug-01-c` |

### 4.2 How many

Manager tick (`0x1010f1e0.c:49-59`): `if boats.size < trunc(sqrt(graph+0x15c))) spawnBoat()` — one attempt per
sub-step *obs*. `graph+0x15c` is the size of the **water-START set** (`graph+0x154`): every water node whose class
`flags & 0x1000ff00 == 0x8000` and which has `START`; a node with both START and END goes to the START set only
(`0x100e4dd0.c:145-153`). In shipped content only lattice border lanes carry water START *obs (dock scripts set no
START/END)*. Samples: Cim2 61 STARTs → cap 7 (7 boats saved); vienna 18 → 4 (4); berlin 28 → 5 but 7 boats saved
(open question 1).

### 4.3 `spawnBoat` (`FUN_1010ce70`, `L:0x081afe60`)

```
spawnBoat(mgr):                                                          0x1010ce70.c
  if boatConsists empty: return                                          :46-49
  if |waterSTART| == 0 or |waterEND| == 0: return                        :50-54
  start = waterSTART[rand() % |waterSTART|]       // std::set in pointer order, iterate from begin   :56-61
  if !canEnter(start, null, ignoreMarks 0): return                       // FUN_100e14b0           :62-64
  end = waterEND[rand() % |waterEND|]                                    :65-71
  if |start.p0 − end.p1|² < 40000.0 (@0x10167b74): return               // >= 200 units apart     :72-73
  if !laneSearch(start, end, cursor, ignoreTraffic 1): return            // FUN_100e5fe0           :74-78
  consist = boatConsists[rand() % count]                                 :79-85
  if consist.units[0].model.height (+0x240, int) > 1:                    // tall ship: low-bridge test   :86-97
      for n = start, then each node popped from the cursor:
          for m in graph.getSegments(n.cellX, n.cellZ, 0, 0, mask 0):    // ±3 cells, every class
              if (m.flags & 0x1000f900) != 0x8000 and −8.5 < m.p0.y < 10.5: return   // @0x10167b70, @0x10167b6c
  L = trunc(Σ_units (model.rearOffset + model.frontOffset) / 6.0 + 0.99)   // @0x1015e870, @0x10159594   :98-115
  if start.occupant (+0x18) != 0: return                                 :116
  head = new agent(bucketOf(start), units[0].model, start, L)            // FUN_101089d0; prepended to the bucket list
  head.colorSeed = rand() & 0xffff                                       :148-149
  head.path = copy(cursor)   // FUN_100e19f0 copies codes/counts/cost/generation, cursor position 0
  head.pathStepsLeft = cursor.stepCount                                  :150-151
  acc = 0; prev = head
  for i in 1 .. n−1:                                                      :152-173
      car = FUN_10108b30(mgr, units[i].model, start, prev, L)            // trailing unit, prev link
      car.colorSeed = rand() & 0xffff; if model.colorMap: setColor
      acc += units[i].model.rearOffset + frontOffset
      car.posOnNode = trunc(−65536 · acc)                                 // @0x10165900; 16.16 behind the head
      prev = car
  boats.push_back(head)                                                   :174
```
*obs.* `bucketOf(node)` = `clamp(node.cellX >> grid.bucketShift)`, same for z. Each failed attempt costs a full lane
search and the rolls made so far; there is no pending state for boats. The bridge test rejects any non-water node
(roads, tracks, **walk paths**, items' paths) whose start is between −8.5 and 10.5 units high within ±3 cells of any
path node, i.e. also quay roads near the lane *inf from the mask*. Initial speed 0, pathStepsLeft = step count,
flags 0 (no 0x800: boats never get the reckless crash path) *obs*.

### 4.4 Movement

Boats use the generic mover (grid-traffic.md §6.2-6.4) with no special case except the pose wobble (§4.6).
Lattice cap per node: `min(turning·20, 6)` = 6 for all shipped boats (turning 1.5–2.0), times `model.speed`
(tugs and cargo ships 0.4) → **2.4 cells/s = 7.2 units/s** *inf from §2.4 + grid-traffic.md §6.4*; acceleration 1.0,
braking 0.2. Crossing lanes (A × B at the block centre, diagonals, connectors) are crossing-linked like road lanes
(chord distance ≤ 0.5 units, no medium check), so boats yield to each other there; the connector flags make turns
behave like junctions *inf*.

### 4.5 Despawn (manager tick `0x1010f1e0.c:124-144`)

```
for boat in boats (from the back, swap-remove):
    if (0 < boat.ringCount <= boat.model.length (+0x248)) or boat.waitTime > 60.0 or boat.flags & 1:
        boats.remove(boat); deleteVehicle(boat)       // FUN_10108180, frees the whole chain
```
*obs.* The ring drains only after all steps are pulled, so a boat disappears `model.length` nodes before its END
lane (tug 2, cargo ship 4 nodes, ~24-48 units before the border).

### 4.6 Wobble (in the mover, not in the pose)

In `gsGridVehicle::move` (`0x1010de50.c:556-580`, `L:0x081adca0.c:705-716`), after `calculatePose`, when the agent's
current node has water class (`curNode.flags & 0x1000ff00 == 0x8000`; boats and water buses) *obs*:

```
t     = 0.0001 · (int)agentAddress + vehicles.time      // vehicles+0x154 = Σ manager dt
roll  = 0.6 · sin(3.123 · t) / model.frontOffset         // about +Z (L: gsPosZVector)
pitch = 0.3 · sin(2.701 · t) / model.frontOffset         // about −X (L: gsNegXVector)
orientation = pose · (Q(roll, +Z) · Q(pitch, −X))         // multiplication order inf
```
*obs* constants; the position is not changed (no heave) and the usual body roll/pitch from `tilting`/`nodding` is
skipped for water nodes *obs (`else` branch `:581-613`)*. The phase uses the heap address (not reproducible); a port
can use any stable per-agent phase. Tug: roll ±2.6°, pitch ±1.3°.

---

## 5. Background aircraft

### 5.1 Airport items and endpoint sets

An air node has class `(flags & 0x1000f900) == 0x4000` (`GRID_PATH_RUNWAY`). `addSegment` files it into one of four
sets (`0x100e4dd0.c:90,123-142`; pathfinding.md §2.5) *obs*:

| set (Windows head / size) | condition | shipped producer | used as |
|---|---|---|---|
| `+0x124` (`+0x128` / **`+0x12c`**) | START, not STOP | `runway-end` `t-a0` (`IGNORE_TRAFFIC\|RUNWAY\|START`) | landing **entry** |
| `+0x130` (`+0x134` / `+0x138`) | END, STOP | `large-terminal` `t-b3`, `t-d3` | landing **gate** |
| `+0x13c` (`+0x140` / `+0x144`) | START, STOP | `large-terminal` `t-a0`, `t-c0`; `platform` `t-a0`, `t-b1` | departing **gate** |
| `+0x148` (`+0x14c` / `+0x150`) | END, not STOP | `runway-end` `t-b4` | departing **exit** |

Sources: `metro/objects/buildings/airport/*.script` (`setPathFlags`) *obs*. Runway nodes have `IGNORE_TRAFFIC` (speed
cap 6), taxiway/terminal nodes not (cap 3) (pathfinding.md §2.1). A helipad `STOP` node (`heliport.script` `t-a0`,
flags 0x4040) is in none of the sets.

### 5.2 Model list

`setAirplanes(...)` (`0x1010aac0.c:28-65`): no argument clears `vehicles+0xfc` (begin `+0x100`, end `+0x104`); each
argument is a model **name** (`gsGridObject*` appended) *obs*. `game.script:390,427-446` (per `updateERA(year)`):
`aircraft-01` if year < 1950, `aircraft-02` 1930–1969, `aircraft-03` 1960–1989, `aircraft-04` ≥ 1980 (overlapping
windows give two entries) *obs*. Shipped aircraft: `length` 2, front/rearOffset 15, frontAnchor 4, rearAnchor 0,
acceleration 0.5, braking 0.1, speed 2.0, turning 1.2–1.3; aircraft-03/04 set `doorOpenStart2/Len2` (gear) *obs*.

### 5.3 Cap, choice, pending state

```
cap = min(graph+0x12c, 2)                       // number of runway entries, at most 2      0x1010f1e0.c:60-67
if aircraft.size < cap:                                                                       :68-82
    if vehicles+0x190 != 0:                spawnLanding()      // a landing is pending
    elif vehicles+0x188 == 0 and (rand() & 1) == 0: spawnLanding()
    else:                                  spawnDeparting()    // also when a departure is pending
```
*obs.* Pending state (both spawners write it, only one can be pending):

| field | landing | departing |
|---|---|---|
| `+0x168..+0x17f` | path cursor `T → gate` (`+0x16c` stepCount, `+0x178` read position) | path cursor `gate → T'` |
| `+0x180` | entry (runway START) | gate (START∧STOP) |
| `+0x184` | gate (END∧STOP) | exit (runway END) |
| `+0x188` | — | **L'** = exit stepped back 15 times (liftoff node); non-zero = departure pending |
| `+0x18c` | — | **T'** = L' stepped back 15 more (start of the take-off roll) |
| `+0x190` | **R** = entry stepped forward 15 times (touchdown); non-zero = landing pending | — |
| `+0x194` | **T** = R stepped forward 15 more (end of the landing roll) | — |

While pending, the four nodes hold one reference each (`refCount++`) *obs (`0x1010b430.c:146-153`,
`0x1010c190.c:127-136`)*; they are released when the spawn completes (`0x1010b430.c:458-466`,
`0x1010c190.c:430-438`). A pending spawn that keeps failing the free checks retries every tick forever (no timeout)
*obs*. Not saved.

### 5.4 Helpers

```
straightOn(n)        = gsGridPathSegment::findForwardConnection (FUN_100e1600): the successor s maximising
                       dot(s.p1 − s.p0, n.p1 − n.p0) (unnormalised), only if > 0; first wins ties; else null
straightBack(n)      = FUN_100e2e50(n, graph): among graph.getSegments(cell of n.p0, 0, 0, mask 0) the node c whose
                       successor list contains n, maximising dot(c chord, n chord) > 0; else null
perp(v)              = FUN_100851b0: |v.x| > |v.y| ? (−v.z, 0, v.x) : (0, v.z, −v.y)
look(up, dir)        = FUN_10086630: rotation whose −Z points along dir (Z = normalize(−dir),
                       X = normalize(up × Z), Y = Z × X); up = DAT_101988b4 = (0,1,0) unless stated
rot(q, v)            = FUN_10085e40; q·(0,0,−s) = s·forward(q)
bez(P0,P1,P2,P3,t)   = cubic Bézier (FUN_10084bc0 = gsBezier); bezD1 = first derivative (FUN_10084cc0 =
                       gsBezierTangent); bezD2 = second derivative (FUN_10084dd0 = gsBezierCurve)
bezLen(P0..P3)       = FUN_100861f0: polyline length over t = 0, 1/8, …, 1 (8 chords)
gsRandom(a, b)       = a + (b − a)·rand()/32767.0   (FUN_10083250)
newAirNode(prev, p1, q1, flags, cf):          // one inline node
    n = new Node (FUN_100e13d0, refCount 1); n.p0 = prev.p1; n.q0 = prev.q1; n.p1 = p1; n.q1 = q1
    n.flags = flags; n.length = |p1 − p0|; n.speedLimit = 6.0; n.curveFactor = cf; n.curvature = 0
    n.cellX = trunc((p0.x + p1.x)/graph.cellSize + graph.width + 0.5) >> 1   // = pathfinding.md §2.2, may be off-grid
    n.cellZ likewise with z and graph.length
nodeCurvature(n)     = 2 · dot(bezD2(n.p0, n.p0 + rot(n.q0,(0,0,−len/3)), n.p1 + rot(n.q1,(0,0,len/3)), n.p1, 0.5),
                           rot(n.q1, (1,0,0)))          // DAT_10198950 = +X
pushRing(a, n)       = a.ring[a.ringCount++] = n; n.occupant(+0x18) = a     // graph nodes also n.refCount++
```
*obs* for every helper (`0x100e1600.c`, `0x100e2e50.c`, `0x100851b0.c`, `0x10086630.c`, `0x10083250.c`,
`0x100861f0.c`, Linux `gsBezier*` bodies, `0x10154050.c` / `0x10154070.c` for the constant vectors). For
`air flags` below: `F(src) = (src.flags & ~0x1000000) | 0x2000000` (Windows `& 0xfeffffff`, Linux `& 0xfcffffff`,
same result) *obs*.

### 5.5 `spawnLandingAirplane` (`FUN_1010b430`)

```
if airplaneModels empty: return                                          0x1010b430.c:79-82
if R == null:                                                            :84-160
    if |airSTART¬STOP| == 0 or |airEND∧STOP| == 0: return
    entry = airSTART¬STOP[rand() % size]
    R = entry; repeat 15 while R: R = straightOn(R)
    T = R;     repeat 15 while T: T = straightOn(T)
    gate = airEND∧STOP[rand() % size]
    if R and T and laneSearch(T, gate, pendingCursor, ignoreTraffic 0): refCount++ on entry, R, T, gate
    else: clear entry, R, T, gate; return
// all nodes the plane will drive must be free now (otherwise retry next tick)                     :165-180
n = R; repeat 15: if !canEnter(n, null, 0): return; n = straightOn(n)       // R .. R+14
pendingCursor.pos = 0; for n = T, pop(cursor, n)…: if !canEnter(n, null, 0): return            // T .. gate
// geometry                                                                                       :181-223
E  = entry.p0 + (0, 15, 0)                                   // @0x1015aa3c = 15.0
d  = normalize(E.x, 0, E.z)                                  // map centre -> entry
d += perp(d) · gsRandom(−0.5, 0.5)                           // not renormalised
dA = d + perp(d) · gsRandom(−0.5, 0.5)
qA = look(+Y, dA)                                            // heading at the start: towards the map
qB = look(+Y, R.p0 − E)                                      // heading at E: down the glide path
S  = (−0.8·W·cs·d.x, gsRandom(150, 200), −0.8·W·cs·d.z)      // @0x10167b68 = −0.8: beyond the opposite edge
len = |S − E|
P1 = S + rot(qA, (0, 0, −0.333·len))                         // @0x1016634c
P2 = E + rot(qB, (0, 0, +0.333·len))                         // @0x10166350
n  = trunc(bezLen(S, P1, P2, E) / 10.0)                      // @0x1015aa38 (Linux shows ROUND; ftol truncates)
model = airplaneModels[rand() % count]                                                            :224-232
// first node: a 12-unit lead-in that ends at S                                                   :233-269
first = new Node; first.p0 = S + 12·normalize(S − P1); first.p1 = S; first.q0 = first.q1 = qA
first.flags = F(entry); length; speedLimit 6; curveFactor 20; cells as newAirNode
a = new agent(bucketOf(first), model, first, lengthArg = (n + pendingCursor.stepCount + 30) >> 1)   :270-307
a.colorSeed = rand() & 0xffff
prev = first
for i in 1 .. n:                                              // flight; t uniform, node lengths vary  :308-373
    t = i · (1/n)
    nd = newAirNode(prev, bez(S,P1,P2,E,t), look(+Y, bezD1(S,P1,P2,E,t)), F(entry), cf 20)
    nd.curvature = nodeCurvature(nd); pushRing(a, nd); prev = nd
for k in 1 .. 15:                                             // straight glide E -> R.p0            :374-422
    nd = newAirNode(prev, lerp(E, R.p0, k · 0.0666667), qB, F(entry), cf 20)   // @0x10167b60 = 1/15
    pushRing(a, nd); prev = nd
g = R; repeat 15: pushRing(a, g); g = straightOn(g)           // runway R .. R+14 (refCount++)      :424-431
pendingCursor.pos = 0; for g = T, pop(cursor, g)…: pushRing(a, g)                                  :432-441
a.pathStepsLeft = 0; a.occupiedCount = a.ringCount + 1; a.reservedCount = a.ringCount; a.speed = 15.0
aircraft.push_back(a); release entry, R, T, gate, and the creation ref of `first`; clear pending   :442-466
```
*obs* (line ranges as given; constants from `@0x1010b69f…@0x1010c0d5`). The plane therefore owns (occupant) its
whole route from spawn: nobody can enter any of its runway/taxiway nodes, and the mover never claims or pulls
anything (`pathStepsLeft = 0`, `reservedCount = ringCount`). `speed = 15` is cut to the target speed on the first
update (`2.0·6 = 12` cells/s on inline nodes, 36 units/s) *inf from grid-traffic.md §6.2*. Note `S` lies
`0.8·W·cs·|d|` from the centre on the side **opposite** the runway: arriving planes cross the whole map.

### 5.6 `spawnDepartingAirplane` (`FUN_1010c190`)

```
if airplaneModels empty: return                                          0x1010c190.c:74-77
if L' == null:                                                           :79-136
    if |airSTART∧STOP| == 0 or |airEND¬STOP| == 0: return
    gate = airSTART∧STOP[rand() % size]
    exit = airEND¬STOP[rand() % size]
    L' = exit; repeat 15 while L': L' = straightBack(L')
    T' = L';   repeat 15 while T': T' = straightBack(T')
    if L' and T' and laneSearch(gate, T', pendingCursor, ignoreTraffic 0): refCount++ on gate, L', T', exit
    else: clear gate, L', T' (and exit); return
pendingCursor.pos = 0; for n = gate, pop(cursor, n)…: if !canEnter(n): return                       :140-149
n = straightOn(T'); repeat 15: if !canEnter(n): return; n = straightOn(n)    // T'+1 .. T'+15        :150-160
E' = exit.p1 + (0, 10, 0)                                    // @0x1015aa38 = 10.0                :162-163
d  = normalize(E'.x, 0, E'.z); d += perp(d)·gsRandom(−0.5, 0.5)
dZ = −d + perp(d)·gsRandom(−0.5, 0.5)
qZ = look(+Y, dZ)                                            // heading at the far end: away from the map
qA = look(+Y, (E' − L'.p1) + (0, 10, 0))                     // climb-out heading at E'
S' = (−0.8·W·cs·d.x, gsRandom(150, 200), −0.8·W·cs·d.z)
len = |S' − E'|
P1 = E' + rot(qA, (0, 0, −0.333·len)); P2 = S' + rot(qZ, (0, 0, +0.333·len))
n  = trunc(bezLen(S', P2, P1, E') / 10.0)                    // @0x1010c651
c  = 0.333·|L'.p1 − E'|
H1 = L'.p0 + c·normalize(L'.p1 − L'.p0)                      // sic: from p0, not p1 (@0x1010c6a7-0x1010c6c5)
H2 = E' + rot(qA, (0, 0, +c))
model = airplaneModels[rand() % count]
if gate.occupant != 0: (cannot happen after the check) skip spawning, still release
a = new agent(bucketOf(gate), model, gate, (pendingCursor.stepCount + 30 + n) >> 1)               :231-271
a.colorSeed = rand() & 0xffff
pendingCursor.pos = 0; for g = pop(cursor, gate) …: pushRing(a, g)          // gate+1 .. T'        :272-281
g = T'; repeat 15: g = straightOn(g); pushRing(a, g)                        // take-off roll ..L'  :284-292
prev = g
for k in 1 .. 15:                                             // climb-out, Bézier L'.p1 -> E'      :293-343
    t = k · 0.0666667
    nd = newAirNode(prev, bez(L'.p1, H1, H2, E', t), look(+Y, bezD1(L'.p1, H1, H2, E', t)), F(exit), cf 30)
    pushRing(a, nd); prev = nd
for i in 1 .. n:                                              // flight E' -> S'                    :344-414
    nd = newAirNode(prev, bez(E', P1, P2, S', i/n), look(+Y, bezD1(E', P1, P2, S', i/n)), F(exit), cf 30)
    nd.curvature = nodeCurvature(nd); pushRing(a, nd); prev = nd
a.pathStepsLeft = 0; a.occupiedCount = a.ringCount + 1; a.reservedCount = a.ringCount   // speed stays 0
aircraft.push_back(a); release gate, L', T', exit; clear pending                                   :415-438
```
*obs.* The climb replaces the last 15 runway nodes (L'+1 .. exit) by a Bézier ending 10 units above the end of the
runway END node; curveFactor 30 (`0x41f00000`) on all departing inline nodes, 20 on landing ones *obs*. The plane
starts at rest (acceleration 0.5 cells/s²) and taxis at `3·2 = 6` cells/s on non-runway nodes *inf*.

### 5.7 Flight, gear, doors, despawn (manager tick `0x1010f1e0.c:217-266`)

```
for plane in aircraft (from the back, swap-remove):
    y = calculatePose(plane).position.y                     // absolute world height
    if plane.waitTime > 60.0 or plane.flags & 1: despawn; continue                  // @0x1015c174
    if plane.ringCount > plane.model.length + 2 or plane.speed >= 0.1:              // flying or rolling
        if y <= 20.0: animate(plane, −1, −1)                                         // rest pose (gear down)
        else:         animate(plane, doorOpenStart2 + min((y − 20)·0.1, 1)·doorOpenLen2, −1)   // gear up 20..30
    else:                                                    // stopped near the end of its route
        if plane.waitTime > 6.0 or y > 20.0: despawn; continue
        if plane.waitTime > 2.0:
            plane.speed = 0
            animate(plane, doorOpenStart + clamp(waitTime − 2, 0, 1)·doorOpenLen, −1)  // doors open 2..3 s
```
*obs* (`@0x101665d0` = 20, `@0x1015661c` = 0.1, `@0x1015e870` = 6, `@0x10163dd8` = 2; `animate(t, −1)` sets the
skeleton clip time, mirrored for reversed agents, and is forced to −1 when the bucket's eye distance ≥ 300 or the
agent has no render instance, `0x101081f0.c:23-64`). `model.length` is the template's (2), not the agent's long
`lengthNodes`. Outcomes *inf*: a landed plane stands at its gate, opens the doors after 2 s and vanishes after 6 s;
a departing plane drains its ring at `S'` (150–200 high, off the map), stops and is removed by the `y > 20` rule.
Pose, target speed and claims are the generic ones (grid-traffic.md §6.4/§6.4a); aircraft anchors (4 / 0) keep the
pitch of the path.

### 5.8 Validation against saves *obs*

Read-only scripts (`<scratchpad>/aircheck.mjs`, `landcheck.mjs`) on the sample saves:
* Departing planes (Cim2 2, berlin 2, vienna 1): inline flags `0x02224000` (runway-end `t-b4` `0x224000` + dynamic),
  speedLimit 6, curveFactor 30; last node ends at `|xz| = 3153 / 3296 / 3389 / 3135` (1280² maps:
  `0.8·W·cs = 3072`, `|d| ≤ 1.118`) and 2459 on the 1024² map (2458), altitudes 155–196.
  `occupiedCount = ringCount + 1`, `reservedCount = ringCount`, `ringCapacity = 2·lengthNodes + 4`.
* Landing plane (vienna): 218 inline nodes `0x02214000` (runway-end `t-a0`), curveFactor 20, then 46 graph nodes
  (15 runway + T + 30 path); the last 15 inline nodes are 12.04 units long and drop exactly 1.0 unit each from
  y = 21 to y = 6 (`entry.y + 15` → `R.p0.y`); Bézier node lengths vary smoothly (11.4 → 5.1 → 8.8), confirming
  uniform-t sampling.
* Helicopters (vienna, flags 0x200): inline flags `0x02004000`, curveFactor 30, speed limits rising from ~1.9 or
  falling to ~1 along the leg (§6.2); ring = inline nodes + 1 graph node (the arrival pad).

---

## 6. Line vehicles on water and in the air

### 6.1 Water bus (line type 3)

* **Stop nodes:** `gsTransportLine::getSegment` (`FUN_10110c10`) asks `getClosestSegment` at the stop item centre,
  height −1.25 cells, mask `STOP | 0x8000`, or `ALTERNATIVE_STOP | 0x8000` for the alternative
  (`(-(alt) & 0x3ffc0) + 0x40`: 0x40 or 0x40000, **not** both) *obs `0x10110c10.c:36-47`*. On shipped content these
  are the dock platform's `t-a0` / `t-b0`.
* **Leg path:** `FUN_10114200` runs the lane search four times (primary/alternative × primary/alternative,
  ignoreTraffic 1) and keeps the cheapest: a later result replaces the kept one when the kept one has 0 steps or a
  higher cost and the new one has > 0 steps *obs `0x10114200.c:37-80`*. The path runs over dock lanes and the lattice
  (lanes under the dock excluded, §2.6).
* **Spawn and stop cycle:** transport.md §4.2-4.3 unchanged: `lengthNodes = trunc(Σlen/6 + 0.99)`,
  `pathStepsLeft = (lengthNodes >> 1) + steps`, flag `0x200`, plus flag `0x1000` for type 3
  (`0x101152a0.c:107-108,156-157`) *obs*.
* **Flag 0x1000** selects `FUN_100e1530` instead of `canEnter`: the node itself must still be unoccupied, but
  **crossing** nodes held by agents of the same line (controller vfunc 0x128 = line) do not block *obs
  `0x100e1530.c:14-33`*. (Shipped saves' water-bus agents lack 0x1000, open question 2.)
* **Platform change:** at departure, if the next leg starts on another stop node and the agent stands on a water
  node, the boat is **teleported** to `stopB.segment.start` (`FUN_10107f20`: releases curNode, prevNode and all
  queued nodes) and `pathStepsLeft += lengthNodes/2` *obs `0x101152a0.c:367-368`*; rail vehicles reverse instead.
* **Docking** is the generic one: arrival when `pathStepsLeft == 0`, `ringCount <= model.length + 1`, speed < 0.1
  (transport.md §4.3). Water buses wobble (§4.6) and are hit by save quirk Q4.

### 6.2 Helicopter (line type 4)

* **Stop node:** mask `STOP | 0x4000` at −1.25 cells; the heliport's `t-a0` has flags exactly 0x4040.
* **Leg path:** the lane search returns a "direct" result without searching when
  `(start.flags & 0x1003f940) == 0x4040`: `stepCount = nibbleCount = −1`, `cost = |start.p1 − goal.p0| / 6`
  (pathfinding.md §4.4). Airport gates (STOP with START/END) do not qualify.
* **Flight path** `FUN_10113280(vehicle)`, run at spawn (`0x101152a0.c:169-170`, instead of setting
  `pathStepsLeft`) and at every departure (`:386-387`):

```
A = stopA.segment.start; B = stopA.segment.end                 // vehicle+0x154 = stopA         0x10113280.c:52-61
P0 = A.p1; P3 = B.p0; d = |P0 − P3|
P1 = P0 + 50·normalize(A.p1 − A.p0) + (0, 100, 0)              // 0x42480000, 0x42c80000          :62-70
P2 = P3 + 50·normalize(B.p0 − B.p1) + (0, 100, 0)                                                 :71-79
n  = 5 − trunc(−0.05·d) = 5 + trunc(d/20)                      // @0x10167d58 = −0.05             :80-84
grow ring to ringCount + n + 1 if needed (unrolled copy, head = 0)                                :85-98
prev = A
for i in 1 .. n:                                                                                  :99-155
    t = i/n
    nd = new Node; nd.p0 = prev.p1; nd.q0 = prev.q1; nd.p1 = bez(P0,P1,P2,P3,t); cells as newAirNode
    nd.flags = 0x2004000; up = bezD2(P0..P3, t); nd.q1 = look(up, bezD1(P0..P3, t))
    nd.length = chord; nd.curveFactor = 30
    nd.speedLimit = 6 − 5·(2t − 1)^4                           // 1 at both pads, 6 mid-way; @0x101643b0, @0x1015e870
    nd.curvature = 0.5 · dot(up, rot(nd.q1, (1,0,0)))          // @0x101593c0
    ring[(head + ringCount++) % cap] = nd                      // occupant NOT set: claimed by the mover later
    prev = nd
ring[(head + ringCount++) % cap] = B; B.refCount++
checkHelicopterPaths(vehicle)
```
*obs.* `look(up = B'', …)` makes the node "up" the curve's acceleration; harmless, because the pose always uses +Y
as up and the node quaternions only feed the forward axis of the Bézier handles *inf (grid-traffic.md §6.4a)*.
`pathStepsLeft` stays 0; the agent's cursor is the direct result (−1/−1) *obs (vienna: 5 such cursors)*.

* **Separation** `gsTransportVehicle::checkHelicopterPaths` (`FUN_10110900`), every tick while spawned
  (`0x101152a0.c:192-193`) and after each flight build:

```
a = vehicle.agent; k = a.reservedCount
if k + 2 < a.ringCount:                                                   0x10110900.c:24-81
    n0, n1, n2 = ring[head+k], ring[head+k+1], ring[head+k+2]; n3 = ring[head+k+3] if k+3 < ringCount else null
    n0.occupant = n0.reserver = n1.occupant = n1.reserver = null            // undo last tick's hold
    for o in vehicles.getVehicles(n2.cellX − 6, n2.cellZ − 6, 12, 12), o != a:
        sameStop = o.controller is a gsTransportVehicle and o.controller.stopB.stop == vehicle.stopB.stop
        if dist²(o.curNode.p0, n0.p0 | n1.p0 | n2.p0) < 900:  if sameStop: n0.occupant = n0.reserver = a
        elif n3 and dist²(o.curNode.p0, n3.p0) < 900:     if sameStop: n1.occupant = n1.reserver = a
```
*obs.* Marking its own next unclaimed node as held by itself makes `claimNodes` stop in front of it (an occupant
that is not standing on the node ends the claim loop, grid-traffic.md §6.3), so a helicopter **holds** while another
one bound for the same stop is within 30 units of its next three nodes (or lets it move one node closer when the
other is only near the fourth) *inf from the claim code*. This corrects transport.md §4.7 ("reserved for this one").
* **Arrival** additionally requires `agent.curNode == stopB.segment.start` (the pad); departure adds
  `|stopA.segment.start.p1 − stopA.segment.end.p0|` to `travelDistance` (transport.md §4.3).

---

## 7. Determinism notes

* Boat spawn rolls, in order: start, end (only if the start is free), consist (only after a found path), head
  colour, one colour per trailing unit *obs*.
* Landing: entry, gate (only when not pending); then perturbation ×2, altitude, model, colour. Departing: gate, exit;
  then the same five *obs*. The tick's 50/50 roll happens only when nothing is pending.
* Set picks iterate `std::set<Node*>` in **pointer order** (heap addresses), so which node `rand() % size` selects is
  not reproducible from a save; any fixed order is equally faithful *obs/inf*.
* Every AI boat and aircraft update rolls once for the 50 % claim gate (grid-traffic.md §6.2), also for aircraft,
  whose claims are already complete.

## 8. Corrections to other documents

1. `pathfinding.md` §2.7: lane direction alternates every **two** blocks (`bx & 2`, `bz & 2`), not with block
   parity; the full geometry is §2 here. §3 (`FUN_10110c10`): the alternative mask is `ALTERNATIVE_STOP | class`
   without `STOP`.
2. `grid-traffic.md` §6.1/§13 q.4: spawner names and pending fields are in §5.3; the boat wobble is in
   `gsGridVehicle::move` after `calculatePose`, not in `FUN_10106a10` (§4.6). §6.3 "same-line passing": only crossing
   nodes are exempt, the node itself must be free (§6.1).
3. `transport.md` §4.7: separation holds the helicopter itself (§6.2); §7.1 `0x180..0x190` is the shared pending state
   of §5.3 (fields up to `+0x194`); §7.3 aircraft now traced (§5).
4. `vehicles-movement.md` §5 q.2 answered: §2.4/§3 (free list = lattice only, newest first).

## 9. Open questions

1. **berlin boat count.** 7 boats saved, but the lattice built from the saved terrain has 28 water STARTs
   (cap 5). Candidates: the session's lattice was built from undried flags at load and the terrain builder's dry-out
   later removed water cells before the save, or the save comes from an older build.
2. **Water-bus flag 0x1000** is set at spawn in 1.0.22, yet the shipped water-bus agents (berlin, vienna) have flags
   0x201/0x601/0x600 without it. Older build, or a path that clears it?
3. **Dock lanes.** The `dock-platform` locator geometry (`.object` not extracted) was not checked. Do its
   `LANE_CHANGE` paths meet lattice endpoints exactly, or within the 4.47-unit `CONNECT_*` tolerance?
4. **Line status bit 2** around the rebuild: which lines have it (transport.md guesses "path display")? Lines without
   it keep segment paths through deleted lattice nodes until their next leg re-path.
5. **Quaternion convention** of `gsQuaternion(θ, axis)` (§2.5) and the multiplication order of the wobble (§4.6) are
   inferred, not traced through `gsQuaternion`'s body.
6. `objects/misc/airpaths.script` (type `GRID_ITEM_HELIPORT`, 16×16, height 33, no `setPathFlags`): its locator air
   lanes would get class 0x4000 only and join no endpoint set. Is it placed anywhere?
7. The default `height` of tug templates (not set by their scripts) decides whether they also get the bridge test;
   only `cargo-ship-01/02` set 2.
