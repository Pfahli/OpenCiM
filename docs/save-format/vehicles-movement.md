# Save format: AI traffic vehicles (0x0704) and agent movement/path state (0x0703)

Engine: `GSystem.dll` (Cities in Motion 1.0.22), Ghidra v2 pseudocode in `decompiled/GSystem-v2/functions/0x<addr>.c`
(evidence is given as `file:line` in those files; `W` = writer `0x1010f8a0.c`, `L` = loader `0x1010d330.c`).
Chunk framework: `u32 tag, u32 size, body`, little endian. Validated against all 21 sample grids (section 4).

## 1. Overview

`0x07040000` is the serialized state of the **background-traffic vehicle manager** (`gsGridVehicles` module,
registrar `0x1010f110`; script methods `spawnVehicle`, `setPassengerTrains`, `setCargoTrains`, `setAirplanes`,
`setBoats`). It is written by `FUN_1010f8a0` (called from the grid writer `0x100c3840.c:363`, first simulation
section after the network `0x07020000`) and read by `FUN_1010d330` (grid loader `0x100c3310.c:136-139`; skipped when
the grid's "no simulation" byte `grid+0x24e` is set). City maps (`amsterdam.grid`, …) contain the section with both
counts 0; scenario maps and saves contain full traffic.

The serialized unit is not a script object but the internal 0x94-byte **vehicle agent** (`new(0x94)` + ctor
`FUN_101089d0`). One agent = one car, lorry cab, lorry trailer, tram/metro/bus body, train carriage, aircraft or
boat. Multi-part vehicles are agent chains linked by `next` (+0x18) and `prev` (+0x1c). Agents sit in the grid's
16×16-cell buckets (`2^bucketShift` cells; `grid+0x150`, bucket list head `bucket+0x10`, link `agent+0x10`) and drive
along **path nodes** — the 0x8c-byte lane segments of the traffic graph (ctor `FUN_100e13d0`). All agents of the
city are stored here, including agents owned by player transport vehicles (`gsTransportVehicle`, flag 0x200) and
parked cars of citizens; other sections refer to them by the 1-based vehicle index defined below (the writer passes
its vehicle→index map on to the 0x0706 and 0x0707 writers, `0x100c3840.c:363-366`).

```
0x07040000  vehicle manager
  header  {u32 modelCount, u32 vehicleCount}
  0x07040100  model-name table: modelCount NUL-terminated strings, table padded to 4
  0x07040200  × vehicleCount   one agent each:
     88-byte record + ringCount × u32 path-node refs
     0x07030200 × k   inline ("dynamic") path nodes referenced as 0x80000000|i by this record
     0x07030100       the agent's movement/path state (always present, always last)
  0x07040300  background trains   {u32 n, n × u32 vehicleRef}
  0x07040400  aircraft            {u32 n, n × u32 vehicleRef}
  0x07040500  boats               {u32 n, n × u32 vehicleRef}
```

`0x07030100` (movement/path state) is a generic agent sub-chunk; it also appears in every character record
`0x07050200` (writer `0x100cb5c0.c:200`, reader `0x100cace0.c:283-284`). Its layout is owned by this document.

### Reference encodings

| kind | encoding | resolver | evidence |
|---|---|---|---|
| vehicle ref | `0` = none, else 1-based index of the `0x07040200` record in file order | W `FUN_100ba930` (`map[ptr]+1`); L `FUN_100b58a0` (`arr[ref-1]` if `1<=ref<=count`, else null) | W `:94-98,183-198,305`; `0x100ba930.c`; `0x100b58a0.c` |
| path-node ref | `0` = none | W `FUN_100ba8f0` (map built in `0x100c3840.c:150-199`); L `FUN_100b5840` | `0x100b5840.c:14-28` |
| — network lane node | `(item1 << 8) \| lane` (top byte ≠ 0x7f, bit 31 clear): node number `lane` (0-based) in the node list (`item+0x24`, link `+0xc`) of network item `item1` = 1-based index into the `0x07020210` item table (file order) | | `0x100c3840.c:155-167` |
| — free graph node | `0x7f000000 \| i`: i-th node of the graph's free-node list `grid+0x254` (link `+0xc`); this list is regenerated at load time from the terrain/network, it is **not** stored in the file | | `0x100c3840.c:194-199`; `0x100c3310.c:114-124` |
| — inline node | `0x80000000 \| i`: i-th `0x07030200` child of the same `0x07040200` record | L before `FUN_100b5840` (`i*4` overflows bit 31 away) | W `:220-236,243-249`; L `:271-277,334-340,355-358` |

Bucket of an agent on load = `clamp(node.cellX >> bucketShift, 0, bucketsX-1)`, same for Y, taken from its
`curNode` (L `:280-296`). The record order is bucket row-major (y outer, x inner) and list order inside the bucket;
the loader prepends, so the in-bucket order reverses on every save/load (L `:395-396`).

## 2. Chunk layouts

Confidence: **H** = writer and loader agree and samples confirm; **M** = named from consistent use in callees;
**L** = inferred. "obs" = confirmed by the sample data; "inf" = from code only.

### 2.1 `0x07040000` — vehicle manager root

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 4 | u32 | modelCount | number of distinct vehicle models used by the agents (`std::map<model*,int>` size) | W `:94-95,108-110`; L `:69-72` | H, obs |
| 4 | 4 | u32 | vehicleCount | number of `0x07040200` records (all agents in all grid buckets) | W `:81-110`; L `:71` | H, obs |
| 8 | … | chunk[] | children | writer order: `0x07040100`, vehicleCount × `0x07040200`, `0x07040300`, `0x07040400`, `0x07040500`. Loader iterates children (`FUN_1007bd90`) and dispatches by tag; any order accepted, unknown tags skipped. `0x07040100` must precede the records (they index its table) | W `:111-391`; L `:90-112,483-486` | H, obs |

**About `0x07040301`:** there is no such chunk and no second record version. The loader's
`if (tag < 0x7040301)` (L `:112`) is the pivot of the compiler's binary-search `switch` (`tag <= 0x07040300` →
compare with 0x07040300/0x07040100/0x07040200, else with 0x07040400/0x07040500). No branch tests equality with
0x07040301, no other function in GSystem references the value, the writer never emits it and no sample contains it.

### 2.2 `0x07040100` — model-name table

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | var | cstr × modelCount | modelName[i] | vehicle model name (`model+0x2b0`), NUL-terminated, e.g. `era1-sedan-01`, `diesel-locomotive`, `tug-01-a`. Index i = `modelIndex` of `0x07040200`. Order = map order by model *pointer* (heap address), i.e. arbitrary | W `:111-129`; L `:139-223` | H, obs |
| end | 0..3 | bytes | pad | zero padding of the whole table to a multiple of 4 (not per string) | W `:162-165` | H, obs |

The loader resolves names through the model registry `grid+0x1ec` (`std::map<string,model*>`); an unknown name gives
a null model, which the agent ctor then dereferences (`model+0x248`) — every name must exist in the loaded content.

### 2.3 `0x07040200` — vehicle agent

Body = 88-byte record + `ringCount` × u32 + child chunks (W `:179-275`, L `:226-430`). Before writing, `FUN_101068f0`
(W `:200`) swaps deleted path nodes (node flag 0x80) for their replacements; on failure the agent gets flags |5.

| offset | size | type | name | agent field | meaning | evidence | confidence |
|---|---|---|---|---|---|---|---|
| 0 | 2 | u16 | modelIndex | +0x04 model* | index into `0x07040100` | W `:180-182`; L `:297,305` | H, obs |
| 2 | 2 | u16 | ringCapacity | +0x44 | allocated length of the node ring buffer (ctor `2*lengthNodes+4`, grows by 2) | W `:202`; L `:308,329-330`; `0x101089d0.c:39-43` | H, obs |
| 4 | 2 | u16 | lengthNodes | +0x48 | agent length in path nodes = max(model+0x248, spawn arg); ctor argument on load | W `:201`; L `:305,309`; `0x101089d0.c:44-48` | H, obs |
| 6 | 2 | u16 | ringHead | +0x4c | ring index of the first queued node | W `:203`; L `:310,341` | H, obs |
| 8 | 2 | u16 | ringCount | +0x50 | number of queued upcoming nodes = number of u32 refs after the record | W `:205,238-254`; L `:228,311` | H, obs |
| 10 | 2 | u16 | colorSeed | +0x80 | `rand() & 0xffff` at spawn (obs ≤ 32767 = RAND_MAX); picks the paint variation via `model+0x378` (`FUN_100755d0`) | W `:204`; L `:312,398-400`; `0x1010ae20.c:235,249` | H, obs |
| 12 | 4 | i32 | posOnNode | +0x54 | distance along the current node in 16.16 fixed-point world units (`×1/65536`); < 0 = waiting before the node start (obs down to −75.7) | W `:207`; L `:313`; `0x10106a10.c:53-59`; `0x1010de50.c:310` | H, obs |
| 16 | 4 | u32 | nextCarRef | +0x18 | vehicle ref of the following carriage/trailer (train, lorry trailer, multi-car tram/metro) | W `:183-190`; L `:94-105,413`; `0x10108b30.c` | H, obs |
| 20 | 4 | u32 | prevCarRef | +0x1c | vehicle ref of the leading unit (lorry cab, player-vehicle head); 0 for AI trains | W `:191-198`; L `:94-105,414`; `0x101152a0.c:155`; `0x1010a260.c:23-27` | H, obs |
| 24 | 4 | f32 | speed | +0x58 | current speed, world units/s (obs 0..12) | W `:208`; L `:314`; `0x1010de50.c:274-300` | H, obs |
| 28 | 4 | u32 | zero | — | always 0, not read by the loader | W `:221`; L — | H, obs |
| 32 | 4 | f32 | waitTime | +0x5c | seconds without moving (reset when moving); boats are despawned after 60 s (`0x1010f1e0.c:130`); horn after 5 s | W `:206`; L `:315`; `0x1010de50.c:304-309,355-362` | H, obs |
| 36 | 4 | f32 | accelSmoothed | +0x60 | low-pass longitudinal acceleration (rate ±4/s²), drives body pitch and engine sound | W `:210`; L `:316`; `0x1010de50.c:366-375,606-614` | M |
| 40 | 4 | f32 | lateralSmoothed | +0x64 | low-pass `node.curvature × speed`, drives body roll | W `:211`; L `:317`; `0x1010de50.c:376-384,595-603` | M |
| 44 | 4 | i32 | reservedCount | +0x68 | number of ring nodes already claimed (node occupant/reserver slots +0x18/+0x1c) ahead of the agent | W `:209`; L `:318,385-394`; `0x1010de50.c:239-258` | H |
| 48 | 4 | i32 | occupiedCount | +0x6c | nodes whose occupant slot (+0x18) is this agent: current node + first `occupiedCount−1` ring nodes | W `:213`; L `:319,366-384` | H, obs |
| 52 | 4 | i32 | straightRun | +0x70 | reservation look-ahead counter: nodes since the last node with flag 0x10, compared with lengthNodes | W `:214`; L `:320`; `0x1010de50.c:171-179` | M |
| 56 | 4 | i32 | lastTick | +0x74 | manager tick of the last update (double-update guard); −1 = never updated (parked); transient | W `:212`; L `:321`; `0x1010de50.c:58-66` | H, obs |
| 60 | 4 | i32 | pathStepsLeft | +0x78 | nodes still to pull from the movement path; −1 = not self-driving (trailer/carriage driven by its head, or parked car) | W `:215`; L `:322`; `0x1010de50.c:68-96`; `0x10108b30.c`; `0x100ceee0.c:360` | H, obs |
| 64 | 4 | u32 | flags | +0x7c | agent flags, table below | W `:216`; L `:323,343` | H/M, obs |
| 68 | 4 | u32 | curNodeRef | +0x24 | path-node ref of the node the agent is on (never 0 in samples) | W `:220-228`; L `:271-277,351-354` | H, obs |
| 72 | 4 | u32 | prevNodeRef | +0x28 | path-node ref of the node it just left (orientation blending) | W `:229-236`; L `:355-365`; `0x1010de50.c:344-347` | H, obs |
| 76 | 12 | f32[3] | crashOffset | +0x84 | world-space displacement added to the rendered position; set to a random drift when a crash starts (flag 0x40000), else 0 | W `:217-219`; L `:324-326`; `0x1010a260.c:33-41`; `0x10106a10.c:225-233` | M, obs |
| 88 | 4·ringCount | u32[] | ringNodeRef[] | ring +0x20 | queued upcoming path nodes, **unrolled** from ringHead: entry j = `ring[(ringHead+j) % ringCapacity]`; the loader writes them back to the same modulo slots | W `:238-254`; L `:329-350` | H, obs |
| … | var | chunk[] | children | k × `0x07030200` in ref order (curNode, prevNode, ring[0..]), then one `0x07030100` | W `:256-274`; L `:236-270` | H, obs |

Agent `flags` (+0x7c):

| bit | meaning | evidence |
|---|---|---|
| 0x1 | path stale/ended (graph changed after path finding, or arrived); boats with it are despawned | `0x1010de50.c:87-89`; `0x101068f0.c:42,72`; `0x100cc990.c:211`; `0x1010f1e0.c:130` |
| 0x2 | carriage reversed (rendered backwards: rear engines, reversed metro cars) | `0x10106a10.c:236`; `0x1010ae20.c:230,246`; `0x101152a0.c:157` |
| 0x4 | alone: parked/finished (set by character code); with 0x1 (`&5==5`): agent invalid, skipped by update and render | `0x100cbed0.c:92`; `0x1010de50.c:63`; L `:343` |
| 0x100 | engine-sound variant toggle (node flag 0x800000) | `0x1010de50.c:404-413` |
| 0x200 | agent of a player transport vehicle (`gsTransportVehicle`) | `0x101152a0.c:159` |
| 0x400 | player vehicle halted at a stop | `0x101152a0.c:309` |
| 0x800 | checks the node ahead before entering (stop-and-go) | `0x1010de50.c:316-318` |
| 0x1000 | node acceptance delegated to the controller `agent+0x2c` (transport type 3) | `0x101152a0.c:102,152`; `0x1010de50.c:154-160` |
| 0x2000 | parked car spawned for a citizen character (pathStepsLeft −1, lastTick −1) | `0x100ceee0.c:359-361`; `0x10106780.c` |
| 0x10000 | player vehicle broken down and halted (set while `broken`, cleared on repair; corrected from "dwelling at a terminus") | `0x101152a0.c:251-255,270-273,446` |
| 0x40000 | crashing: decelerates 4/s, drifts by crashOffset (`onCrashBegin/End`) | `0x1010a260.c:32`; `0x1010de50.c:283-299,673` |

Mask `0x50400` disables pulling new path nodes (`0x1010de50.c:68`).

### 2.4 `0x07040300` / `0x07040400` / `0x07040500` — manager lists

One layout for all three (W `:284-385`; L `:113-138,432-482`).

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0 | 4 | u32 | count | number of entries | W `:298,315`; L `:114-116` | H, obs |
| 4 | 4·count | u32[] | vehicleRef[] | 1-based vehicle refs of the *head* agents; 0 entries are skipped on load | W `:305`; L `:120-121` | H, obs |

| tag | manager vector | contents | spawner / evidence |
|---|---|---|---|
| `0x07040300` | `mgr+0x120` | background trains (head agent; carriages via nextCarRef) — passenger (`setPassengerTrains`, list +0xe0) or cargo (`setCargoTrains`, +0xf0). obs: diesel/steam/el-locomotives | `FUN_1010ae20` `0x1010ae20.c:262-272` |
| `0x07040400` | `mgr+0x130` | aircraft (`setAirplanes`, list +0x100; at most min(2, graph+0x12c) at once). obs: `aircraft-01/02/04` | `FUN_1010b430` `:442-451`, `FUN_1010c190` `:412-421`; `0x1010f1e0.c:50-76` |
| `0x07040500` | `mgr+0x140` | boats (`setBoats`, list +0x110; count limited by graph+0x15c). obs: `tug-01-*`, `cargo-ship-01/02` | `FUN_1010ce70` `0x1010ce70.c:168` (`FUN_1010a190(mgr+0x13c)`) |

Road cars, lorries and parked cars are in no list (bucket only); trailers/carriages are reached through nextCarRef.

### 2.5 `0x07030100` — movement / path state (shared with characters)

Object: 0x18-byte path cursor (ctor `FUN_100e17e0`; `agent+0x40`, character `+0x70`). Writer `FUN_100e1a90`
(`0x100e1a90.c:18-26`); reader `FUN_100e1fb0` (vehicles pass `minWords=0`, characters 8). Produced by the A* path
finder `FUN_100e5fe0`, consumed by `FUN_100e1810` (pop next node) and `FUN_100e1a70` (finished test).

| offset | size | type | name | field | meaning | evidence | confidence |
|---|---|---|---|---|---|---|---|
| 0 | 4 | i32 | stepCount | +0x04 | number of nodes on the path; −1 for a direct path between two `0x4040` nodes (no steps). obs: equals the step count decoded from the codes in 100 % of records | W `:23`; R `0x100e1fb0.c:32`; `0x100e5fe0.c:72-75,233` | H, obs |
| 4 | 4 | i32 | nibbleCount | +0x08 | number of 4-bit step codes; −1 with the direct path | W `:21`; R `:28`; `0x100e5fe0.c:226-236` | H, obs |
| 8 | 4 | f32 | travelCost | +0x0c | A* cost of the path ≈ Σ node.length/node.speedLimit (seconds); direct path: distance/6. obs: float bit patterns | W `:19`; R `:29`; `0x100e5fe0.c:74,225` | H, obs |
| 12 | 4 | u32 | cursor | +0x10 | read position in 1/8-code units: code index `cursor>>3`, `cursor&7` = progress inside a run code | W `:20`; R `:30`; `0x100e1810.c:18-41`; `0x100e1a70.c` | H, obs |
| 16 | 4 | u32 | graphGeneration | +0x14 | traffic-graph generation (`graph+0xe8`) at path-finding time; path is stale when a node's +0x80 is newer. The vehicle loader discards it (sets 0, L `:242`) | W `:22`; R `:33`; `0x100e5fe0.c:66`; `0x1010de50.c:87` | H |
| 20 | 4·⌈nibbleCount/8⌉ | u32[] | steps | +0x00 (u32*) | packed codes, 8 per u32, code i at bits `4*(i&7)` of word `i>>3`; unused high nibbles 0 (obs) | W `:25`; R `:34-49`; encode `0x100e5fe0.c:245-273`; decode `0x100e1810.c:20-41` | H, obs |

Step codes, applied starting from the agent's last queued node (`ring[head+count-1]`, or curNode if the ring is
empty; `0x1010de50.c:77-91`): code `0..7` → follow adjacency link `code+1` of the node's outgoing-link list
(`node+4`, singly linked); code `8..15` → follow link 0 ("straight on") `code−7` times (runs of 1..8). Each pop
advances cursor by 8, except inside runs: first pop of code v adds `16−v`, later pops add 1 until the next code.
Size rule: `size = 20 + 4*((nibbleCount+7)>>3)`, 0 words when nibbleCount ≤ 0. (`FUN_100e1fb0` copies 4× too many
words from the read buffer — a harmless over-read; only the bytes above are in the file.)
Invariant for AI agents (obs 100 % of self-driving non-player agents): `pathStepsLeft = stepCount − steps consumed by
cursor`. Player-vehicle agents (flag 0x200) set pathStepsLeft independently.

### 2.6 `0x07030200` — inline (dynamic) path node

Object: 0x8c-byte path node (`FUN_100e13d0`); bytes = node+0x24..+0x77. Only nodes carrying flag `0x02000000`
(run-time segments not owned by a network item) are stored inline; all others are refs. Writer `FUN_100e1eb0`
(`0x100e1eb0.c:63-90`), reader `FUN_100e1680` (`0x100e1680.c:13-34`). A node referenced twice by one agent is written
twice (no de-duplication); the loaded copy starts with refcount 1 and is released after the record (L `:417-420`).
In all samples every inline node is an **air-lane** segment (class 0x4000, altitude 6..197) used by aircraft.

| offset | size | type | name | node field | meaning | evidence | confidence |
|---|---|---|---|---|---|---|---|
| 0 | 4 | u32 | flags | +0x24 | node flags: low nibble = path-finder scratch; 0x10 reservation break; 0x20 junction; 0x40 stop/signal; 0x80 deleted; class `flags & 0x1000ff00` (0x500, 0x700, 0x1000, 0x2000, 0x4000 = air, 0x8000 = water, …); 0x200000 fast class; 0x400000; 0x800000 sound variant; 0x1000000 blocked; **0x2000000 dynamic** (always set here). obs: `0x02224000`, `0x02214000`, `0x02004000` | W `:68`; R `:13`; `0x100ebb20.c:101-104,186-222`; `0x100e5fe0.c`; `0x1010de50.c:571` | H (0x2000000), L (others) |
| 4 | 4 | i32 | cellX | +0x28 | grid cell x (bucket = cellX >> bucketShift); obs ≈ worldX/cellSize + width/2 (−0.75..+0.75); may lie outside the grid for aircraft | W `:69`; R `:14`; L `:280-286`; `0x100e4dd0.c:72-78` | H, obs |
| 8 | 4 | i32 | cellY | +0x2c | grid cell y ≈ worldZ/cellSize + length/2 | W `:70`; R `:15`; L `:287-292` | H, obs |
| 12 | 12 | f32[3] | startPos | +0x30 | segment start, world units (y = height) | W `:71-73`; `0x100ebb20.c:105-109` | H, obs |
| 24 | 16 | f32[4] | startRot | +0x3c | orientation quaternion at the start (obs norm 1) | W `:74-77`; `0x100ebb20.c:110-115` | H, obs |
| 40 | 12 | f32[3] | endPos | +0x4c | segment end | W `:78-80` | H, obs |
| 52 | 16 | f32[4] | endRot | +0x58 | orientation quaternion at the end | W `:81-84` | H, obs |
| 68 | 4 | f32 | length | +0x68 | |endPos−startPos| (obs ratio 1.000); posOnNode runs 0..length | W `:85`; `0x100ebb20.c:127`; `0x10106a10.c:59-74` | H, obs |
| 72 | 4 | f32 | speedLimit | +0x6c | segment speed cap (class-dependent 1.5..10; obs 1..6) | W `:86`; `0x100ebb20.c:170-222` | H, obs |
| 76 | 4 | f32 | curveFactor | +0x70 | `20/clamp(curveDeviation,1,8)`, ×1.5 for classed lanes (obs 20..30) | W `:87`; `0x100ebb20.c:167-169` | M |
| 80 | 4 | f32 | curvature | +0x74 | signed lateral curvature ×0.05; agents use `curvature×speed` for roll | W `:88`; `0x100ebb20.c:182`; `0x1010de50.c:376` | M |

Rendering (for a reimplementation): the agent pose is a cubic Bézier along the node: control points
`startPos`, `startPos + startRot·(0,0,length/3)`, `endPos − endRot·(0,0,length/3)`, `endPos`, blended with the
previous/next node at the joints, evaluated at `posOnNode` offset by model axle distances
(`model+0x270/+0x274/+0x278`), plus `crashOffset` (`FUN_10106a10`).

## 3. Machine-readable layout

```json
{
  "notes": "little-endian; every chunk = {u32 tag, u32 size, body}; field offsets are relative to the chunk body",
  "chunks": [
    {"tag": "0x07040000", "name": "vehicleManager",
     "fields": [
       {"name": "modelCount", "type": "u32", "offset": 0},
       {"name": "vehicleCount", "type": "u32", "offset": 4}],
     "childrenOffset": 8,
     "children": [
       {"tag": "0x07040100", "count": 1},
       {"tag": "0x07040200", "count": "vehicleCount"},
       {"tag": "0x07040300", "count": 1},
       {"tag": "0x07040400", "count": 1},
       {"tag": "0x07040500", "count": 1}]},
    {"tag": "0x07040100", "name": "modelNames",
     "fields": [
       {"name": "modelName", "type": "cstr", "offset": 0, "count": "parent.modelCount"},
       {"name": "pad", "type": "bytes[n]", "offset": "afterPrevious", "count": "(4 - consumed % 4) % 4"}],
     "children": []},
    {"tag": "0x07040200", "name": "vehicleAgent",
     "fields": [
       {"name": "modelIndex", "type": "u16", "offset": 0},
       {"name": "ringCapacity", "type": "u16", "offset": 2},
       {"name": "lengthNodes", "type": "u16", "offset": 4},
       {"name": "ringHead", "type": "u16", "offset": 6},
       {"name": "ringCount", "type": "u16", "offset": 8},
       {"name": "colorSeed", "type": "u16", "offset": 10},
       {"name": "posOnNode", "type": "i32", "offset": 12, "scale": 1.52587890625e-05},
       {"name": "nextCarRef", "type": "u32", "offset": 16, "ref": "vehicle1"},
       {"name": "prevCarRef", "type": "u32", "offset": 20, "ref": "vehicle1"},
       {"name": "speed", "type": "f32", "offset": 24},
       {"name": "zero", "type": "u32", "offset": 28},
       {"name": "waitTime", "type": "f32", "offset": 32},
       {"name": "accelSmoothed", "type": "f32", "offset": 36},
       {"name": "lateralSmoothed", "type": "f32", "offset": 40},
       {"name": "reservedCount", "type": "i32", "offset": 44},
       {"name": "occupiedCount", "type": "i32", "offset": 48},
       {"name": "straightRun", "type": "i32", "offset": 52},
       {"name": "lastTick", "type": "i32", "offset": 56},
       {"name": "pathStepsLeft", "type": "i32", "offset": 60},
       {"name": "flags", "type": "u32", "offset": 64},
       {"name": "curNodeRef", "type": "u32", "offset": 68, "ref": "pathNode"},
       {"name": "prevNodeRef", "type": "u32", "offset": 72, "ref": "pathNode"},
       {"name": "crashOffset", "type": "f32", "offset": 76, "count": 3},
       {"name": "ringNodeRef", "type": "u32", "offset": 88, "count": "ringCount", "ref": "pathNode"}],
     "childrenOffset": "88 + 4*ringCount",
     "children": [
       {"tag": "0x07030200", "count": "number of refs with bit31 set among [curNodeRef, prevNodeRef, ringNodeRef...]", "order": "ref order; ref low 31 bits = child index"},
       {"tag": "0x07030100", "count": 1, "position": "last"}]},
    {"tag": "0x07040300", "name": "trainList",
     "fields": [
       {"name": "count", "type": "u32", "offset": 0},
       {"name": "vehicleRef", "type": "u32", "offset": 4, "count": "count", "ref": "vehicle1"}],
     "children": []},
    {"tag": "0x07040400", "name": "aircraftList",
     "fields": [
       {"name": "count", "type": "u32", "offset": 0},
       {"name": "vehicleRef", "type": "u32", "offset": 4, "count": "count", "ref": "vehicle1"}],
     "children": []},
    {"tag": "0x07040500", "name": "boatList",
     "fields": [
       {"name": "count", "type": "u32", "offset": 0},
       {"name": "vehicleRef", "type": "u32", "offset": 4, "count": "count", "ref": "vehicle1"}],
     "children": []},
    {"tag": "0x07030100", "name": "movementPath",
     "fields": [
       {"name": "stepCount", "type": "i32", "offset": 0},
       {"name": "nibbleCount", "type": "i32", "offset": 4},
       {"name": "travelCost", "type": "f32", "offset": 8},
       {"name": "cursor", "type": "u32", "offset": 12},
       {"name": "graphGeneration", "type": "u32", "offset": 16},
       {"name": "steps", "type": "u32", "offset": 20, "count": "nibbleCount <= 0 ? 0 : (nibbleCount + 7) >> 3", "packing": "4-bit codes, code i at bits 4*(i&7) of word i>>3"}],
     "children": []},
    {"tag": "0x07030200", "name": "inlinePathNode",
     "fields": [
       {"name": "flags", "type": "u32", "offset": 0},
       {"name": "cellX", "type": "i32", "offset": 4},
       {"name": "cellY", "type": "i32", "offset": 8},
       {"name": "startPos", "type": "f32", "offset": 12, "count": 3},
       {"name": "startRot", "type": "f32", "offset": 24, "count": 4},
       {"name": "endPos", "type": "f32", "offset": 40, "count": 3},
       {"name": "endRot", "type": "f32", "offset": 52, "count": 4},
       {"name": "length", "type": "f32", "offset": 68},
       {"name": "speedLimit", "type": "f32", "offset": 72},
       {"name": "curveFactor", "type": "f32", "offset": 76},
       {"name": "curvature", "type": "f32", "offset": 80}],
     "size": 84,
     "children": []}
  ],
  "refs": {
    "vehicle1": "0 = none; else 1-based index of 0x07040200 in file order",
    "pathNode": "0 = none; bit31 set: (ref & 0x7fffffff)-th 0x07030200 child of the same record; (ref>>>24)==0x7f: (ref & 0xffffff)-th free graph node (grid+0x254, regenerated, not in file); else node number (ref & 0xff) of network item (ref>>>8) (1-based index into 0x07020210)"
  }
}
```

## 4. Validation

Script: `C:\Users\Marku\AppData\Local\Temp\claude\C--Users-Marku-Cities-in-Motion\fe525a7e-5e90-46f7-b12c-445b2bb3c3e0\scratchpad\save-vehicles\validate.mjs`
(per-file JSON report; reuses `tools/gridparse.mjs` `Reader`), `runall.mjs` (all 21 sample grids), `cursor.mjs`
(pathStepsLeft/cursor invariant), `cats.mjs` (flag/model categories). Read-only.

Checks per file: root counts vs. records; writer child order; name table consumes exactly modelCount strings + correct
zero padding; every record = 88 + 4·ringCount + children, children = exactly the expected number of `0x07030200`
(one per bit-31 ref, indices sequential) followed by one `0x07030100`; modelIndex < modelCount; ringHead <
ringCapacity, ringCount ≤ ringCapacity; vehicle refs in 1..vehicleCount; item refs in 1..itemCount; occupiedCount ≤
ringCount+1, reservedCount ≤ ringCount; field 28 = 0; every nextCarRef target has pathStepsLeft = −1; list sizes =
4+4·n; movement size formula, cursor ≤ end, decoded step total = stepCount, zero tail nibbles; inline node size 84,
dynamic flag, cell vs. world position, quaternion norm, length vs. chord.

Result: **0 violations of any structural check in all 21 files** (3 saves, 12 scenario maps, 6 empty city maps).

| file | models | agents | trains/air/boats | node refs item / free / inline / null | inline nodes | vehicle movement (size ok / steps decode ok) | character `0x07030100` (size ok / steps ok) |
|---|---|---|---|---|---|---|---|
| saves/Cim2 (player) | 43 | 2002 | 23 / 2 / 7 | 21144 / 266 / 517 / 888 | 517 (all `0x02224000`) | 2002/2002, 840/840 | 1576/1576, 1552/1552 |
| saves/berlin | 23 | 1673 | 14 / 2 / 7 | 15836 / 290 / 303 / 552 | 303 | 1673/1673, 925/925 | 5527/5527, 5201/5201 |
| saves/vienna | 45 | 1884 | 15 / 2 / 4 | 17816 / 146 / 474 / 826 | 474 | 1884/1884, 796/796 (5 direct paths −1/−1) | 2165/2165, 2038/2038 |
| maps scenario01..12 | 15..48 | 484..2220 | e.g. 6 / 1 / 55 (sc05) | all in range | 0..872 | all size-ok, all steps-ok | all size-ok, all steps-ok |
| amsterdam, berlin, helsinki, tutorialcity, vienna, cologne (.grid) | 0 | 0 | 0 / 0 / 0 | — | — | — | — |

Value ranges (Cim2 | berlin | vienna): speed 0..12 in all; waitTime 0..29 | 0..58.2 | 0..45.2; posOnNode
0..196.1 | −75.7..21.6 | −73.8..13.6 world units; lengthNodes 1..396 | 1..309 | 1..264; ringCapacity 6..796 | 6..622 |
6..532; ringCount 0..371 | 0..285 | 0..264; lane number max 115 | 79 | 114; colorSeed max 32651 | 32490 | 32595
(≤ RAND_MAX); pathStepsLeft −1: 1160 | 746 | 1081; lastTick = current tick (7006/7007, 30531..30534, 31432..31435)
or −1 (parked, 869 | 524 | 799); crashOffset ≠ 0 in exactly one agent (berlin, flags `0x00040800` = crashing).

Semantic checks: all list entries are self-driving heads with model families matching the list (trains:
`diesel-locomotive`, `steam-locomotive`, `eltrain-engine`; aircraft: `aircraft-01/02/04`; boats: `tug-01-*`,
`cargo-ship-0*`); trailers with prevCarRef ≠ 0 are lorry trailers (`era*-truck-trailer-*`) and player tram/metro cars;
flag 0x200 agents are only player models (`tram-*`, `metro-*`, `bus02`, `waterbus-*`, `helicopter-03`); flag 0x2 only
on reversed engines/metro cars; flag 0x2000 (Cim2 only) only on parked cars with pathStepsLeft = lastTick = −1;
flag 0x1 only on boats. Invariant `pathStepsLeft = stepCount − consumed(cursor)` holds for every non-player
self-driving agent (Cim2 838, berlin 849, vienna 783, scenario01 1053); the only exceptions are flag-0x200 player
agents (2, 76, 13, 5). Inline nodes: cellX/cellY − world/cellSize − size/2 within [−0.75, 0.75]; quaternion norms 1;
length/chord 1.000; 237 (Cim2) of them have cells outside the grid (aircraft approach paths), the loader clamps
their bucket.

## 5. Open questions

1. ~~**Adjacency order.**~~ Resolved: rebuilding the links as `../simulation/pathfinding.md` §2.0-2.3 describes
   (items in file order) reproduces the engine's order; all 803 saved traffic paths of Cim2 replay to an END/PARK
   node (port `engine/sim.cpp`, `REGS_DEBUG_SIM=1`).
2. **Free-node refs (`0x7f000000|i`)** index `grid+0x254`, which the loader regenerates after the terrain chunk
   (`0x100c3310.c:114-124`, `FUN_100c1840`). Which nodes are on that list and in what order (max index seen 55111)
   belongs to the graph-generation analysis, not to the file format.
3. ~~**Lane numbering**~~ Resolved: the item's node list holds one node per template path, prepended in ascending
   path-name order, so lane `n` is the n-th path in **descending** name order (`../simulation/pathfinding.md` §2.0-2.1).
   Checked on Cim2: the saved nodes of a 5-car tram resolve to positions 6.40 / 9.40 / 15.80 / 21.90 units behind the
   head, exactly the spawn spacing Σ(frontOffset + rearOffset) of the cars.
4. Node flag bits other than 0x2000000/0x80 and the class field are named from usage only (L confidence);
   `curveFactor` and `curvature` semantics are M.
5. `straightRun` (+0x70) and `accelSmoothed`/`lateralSmoothed` are named from the update loop; exact units unverified.
6. Flag 0x2000 (parked citizen car) appears only in the player save; the shipped saves have parked cars
   (pathStepsLeft = lastTick = −1) with flags 0 — possibly saved by an older build. The owning character link
   (`agent+0x30`) is not in this section; it is presumably restored from the character/citizen sections (0x0705/0x0707),
   which receive the vehicle index map.
7. The loader ignores `graphGeneration` (forces 0) and the field at record offset 28; `lastTick` is transient.
   A writer should still emit the original values for byte-identical round trips.
