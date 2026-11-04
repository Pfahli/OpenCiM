# Citizens: population, daily cycle, trips, mode choice, mood

Native code of `gsGridCitizens` (registrar `0x100d3d90`) and `gsGridCitizenController` (registrar `0x100cdda0`) in
`GSystem.dll`. Evidence is cited as `0xADDR.c:line` in `decompiled/GSystem-v2/functions/`. Every claim is tagged
*obs* (read directly in the pseudocode or in the DLL bytes) or *inf* (inferred). Graph internals (road search, walking
search, transit Dijkstra) are only named here; see `pathfinding.md`. Vehicle-side boarding is in `transport.md`.

Pointer arithmetic: the citizen is an `int[24]`, so Ghidra's `param_1[0x12]` is byte offset `+0x48`. Inside the tick
`FUN_100d40e0` offsets are relative to object + 0xcc (`this+0x18` = `+0xe4`); everywhere else they are object offsets.

## 0. Time base

* `gsGridCitizens.tick(dt)` runs once per grid sub-step. It processes `n = min(count, trunc(count·dt + acc))` citizens,
  round-robin, and keeps the fraction in `acc` (`+0xe8`) (`0x100d40e0.c:27-45`; operand of `__ftol` read from the bytes
  at file offset `0xd4119-0xd413a`: `fild count; fmul dt; fadd acc` *obs*). `count` is converted to float first.
* So each citizen is processed about **once per grid-second** (scaled time `dt·timeSpeed`) *inf from the formula*.
  One game day = 8.64 grid-seconds at every game speed (`docs/simulation/README.md`, time base).
* All per-citizen counters below (`cycleTimer`, `waitTime`, patience) count **processings**, i.e. grid-seconds.
  `game.script:80-82` agrees: "time in seconds after the citizen can move again". The engine itself converts days with
  the factor `216/25 = 8.64` (`0x100d18f0.c:420`) *obs*.
* Walking and driving "costs" are seconds as well: walk time = distance·`grid.scale`/`character.speed`; road cost =
  Σ node `length/speedLimit` (`0x100e5fe0.c:223-226`) *obs*. Fares are turned into seconds by `priceSignificance`.

## 1. Data layouts

### 1.1 `gsGridCitizens` (selected fields)

| offset | type | proposed name | meaning | evidence |
|---|---|---|---|---|
| +0xcc | vtbl | moveable | tick interface | `0x100d2860.c:21-22` |
| +0xd4 | gsGrid* | grid | owner grid (`+0x208` graph, `+0x20c` vehicles, `+0x210` characters, `+0x218` lines) | `0x100d2860.c:23` |
| +0xdc | Citizen* | head | doubly linked list head (link `+0x38` prev, `+0x3c` next); new citizens are inserted at the head | `0x100d2ac0.c:52-56` |
| +0xe0 | Citizen* | cursor | round-robin position; `null` → restart at head | `0x100d40e0.c:37-43` |
| +0xe4 | int | count | script `count`; number of citizens | `0x100d2ac0.c:59`, `0x100d2c70.c:40` |
| +0xe8 | float | tickAccumulator | fractional citizens carried to the next tick | `0x100d40e0.c:27-34` |
| +0xec | std::map<u32,Citizen*> | byId | `findCitizen`, `getRandomCitizen`; head node `+0xf0`, size `+0xf4` | `0x100d2ac0.c:40,57`, `0x100ce110.c` |
| +0xf8 | vector[7] | characterPools | `{gsGridObject* tpl, u32 eraMask, i32 gender}*` per class (`addCharacter`) | `0x100ce180.c:20-31` |
| +0x168 | vector[91] | carPools | index `visitType·7 + class`; records `{parts*, partCount, weight, eraMask}` (`addCar`) | `0x100d2860.c:34`, `0x100ce250.c:26-35` |
| +0x718 | vector[7] | freeHomes | one entry per free home **slot**; element = grid item* | `0x100d2d90.c:37-49`, `0x100ce360.c` |
| +0x788 | vector[7] | freeWorkplaces | one entry per free job slot | `0x100d2d90.c:52-71`, `0x100ce420.c` |
| +0x7f8 | vector[13] | visitPlaces | one entry per visit place, never consumed | `0x100d2d90.c:76-98`, `0x100ce5d0.c` |
| +0x8c8 | int[7][4] | reasonProbability | {work, shopping, leisure, government} % | `0x100cbd50.c`, batch-A |
| +0x938 | float[7] | priceSignificance | per class | `0x100d18f0.c:201,243,262` |
| +0x954 | float[13] | utilProbability | per visit type | `0x100cbdf0.c:38` |
| +0x988 / +0x98c | int | min/maxCycleDuration | defaults 0 / 600; scripts 300 / 600 | `0x100d2860.c:41-42`, `game.script:81-82` |
| +0x990 … +0xa00 | int[7] ×5 | walk, drive, travel, canTravel, priceTooHigh | trip statistics (`getWalkCount` …) | `0x100d18f0.c:280-385` |
| +0xa1c / +0xa20 | float | home/workplaceMultiplier | default 1.0 | `0x100d2860.c:43-44`, bytes `0xd2e0b`, `0xd2e9c` |
| +0xa24 | int | joblessRate | % ; default 0 | `0x100d2ac0.c:48`, `0x100d18f0.c:69` |
| +0xa28 | u32 | modelMask | era mask; default 0xffffffff | `0x100ce180.c:27`, `0x100ce250.c:33` |
| +0xa2c | int[7] | population | citizens per class | `0x100d2ac0.c:60-61` |
| +0xa48 | int[7] | unemployed | citizens per class without workplace (includes classes that never work) | `0x100d2ac0.c:62-65` |
| +0xa64 | gsClock* | clock | script `clock`; `clock+0xdc` = `date` (day number) | `0x100ce5d0.c:48`, `docs/native-api.md` gsClock |
| +0xa68 | float | carMultiplier | default 1.0 | `0x100ce250.c:21` |
| +0xa6c | u8* | clusterData | `width×length` bytes from `setClusterData(bmp)` (first byte of each 3-byte pixel), or null | `0x100ce8d0.c:45-63` |

### 1.2 Citizen record (`0x60` bytes, ctor `FUN_100cbc10`)

| off | idx | proposed name | meaning | evidence |
|---|---|---|---|---|
| +0x00 | 0 | owner | `gsGridCitizens*` | `0x100cbc10.c:13` |
| +0x04 | 1 | controller | lazily created `gsGridCitizenController` (0x140 B, citizen at `controller+0x13c`) | `0x100cc610.c` |
| +0x08 | 2 | character | visible pedestrian (characters module), `char+0x1c` = citizen | `0x100cc800.c:25-28` |
| +0x0c | 3 | car | private car agent (vehicles module), `car+0x30` = citizen | `0x100ceee0.c:91-94` |
| +0x10 | 4 | lineNode | stop node where the citizen waits | `0x100cc990.c:223-230` |
| +0x14 | 5 | transportVehicle | `gsTransportVehicle` the citizen rides | `0x100cc990.c:415-420` |
| +0x18 | 6 | drivePath | road path object (0x18 B, `FUN_100e17e0`); `+0xc` = cost | `0x100d18f0.c:143-152,201-224` |
| +0x1c | 7 | route | transit route (0x10 B): `{f32 cost; i32 nodeCount; i32 leg; Node** nodes}` | `0x100d4500.c`, `0x1011a200.c:231-248` |
| +0x20 | 8 | home | grid item | `0x100d2ac0.c:45-46` |
| +0x24 | 9 | workplace | grid item or 0 | `0x100d2ac0.c:48-51` |
| +0x28 | 10 | start | trip origin item | `0x100cc6c0.c:63-66` |
| +0x2c | 11 | target | trip destination item, 0 when none | `0x100d18f0.c:100-120` |
| +0x30 | 12 | charModel | pointer to a character-pool record `{tpl, eraMask, gender}` | `0x100ce180.c:43` |
| +0x34 | 13 | carModel | pointer to a car-pool record or 0 (= no car) | `0x100ce250.c:54-56` |
| +0x38 / +0x3c | 14/15 | prev / next | list links | `0x100d2c70.c:24-35` |
| +0x40 | 16 | driveStartNode | road node (`GRID_PATH_START|road` = 0x10100) | `0x100d18f0.c:153-170` |
| +0x44 | 17 | driveEndNode | road node (`GRID_PATH_END|road` = 0x20100); **reused while idle** as the bench node of the sit-down behaviour | `0x100d18f0.c:171-196`, `0x100cbed0.c:30-40,93` |
| +0x48 | 18 | flags | bit field, below | |
| +0x4c | 19 | cycleTimer | processings until the next trip may start; ctor `rand() % maxCycleDuration` | `0x100cbc10.c:37-38`, `0x100d3ca0.c:13-15` |
| +0x50 | 20 | id | random 30-bit unique id | `0x100d2ac0.c:35-41` |
| +0x54 | 21 | tripQueue | 6-bit `GRID_VISIT_*` codes, next trip in the low bits | `0x100cbdf0.c`, `0x100d18f0.c:102-120` |
| +0x58 | 22 | waitTime | processings spent waiting at the current stop | `0x100cc990.c:230,271` |
| +0x5c | 23 | usedTransport | bit mask of line types boarded on this trip | `0x100cc990.c:436-441` |

**Flags `+0x48`** (*obs* unless noted): `0x0f` class; `0xf0` state (0 IDLE, 0x10 WALK, 0x20 DRIVE, 0x30 TRAVEL,
0x40 REPLAN); `0x100` phase A (DRIVE: walking to the car; TRAVEL: walking to the vehicle door); `0x200` phase B
(DRIVE: walking from the parked car; TRAVEL/WALK: just spawned from a vehicle); `0x400` "pedestrian kept after the
last trip" (next path starts at the pedestrian); `0xff000` visit type of a utility/emergency trip (>5 only);
`0x100000` car parked at the current place; `0x1000000/0x2000000/0x4000000` mood 1/2/3.

### 1.3 Foreign fields the citizen code reads or writes

* **Grid item:** `+0x04` template (`gsGridObject`), `+0x2c/+0x30/+0x34` x/y/z (cells; y in 1/256), `+0x38/+0x40`
  width/length (cells), `+0x24` road-node list (link `+0xc`), `+0x48` flags (`0x80000000` = `GRID_ITEM_SHOW_CHARACTER`),
  `+0x58` type (`GRID_ITEM_*`: 1–7 road types, 0x14 building, 0x16 spawn point), `+0x5c` statistics word (§6.6).
  Centre used everywhere: `(x + (w>>1), z + (l>>1))` in cells (`0x1011bf00.c:13-16`) *obs*.
* **Template (`gsGridObject`):** `+0x288` speed (characters' walking speed, 0.57–1.85 in the shipped
  character scripts), `+0x320` workVisitType[7], `+0x374` barred-visitor mask, `+0x3c0` open-days word
  `(open<<16)|period`, `+0x26c/+0x270` front/rear offset, `+0x248` length in nodes, `+0x378` colour map.
* **Line stop node (0x70 B):** `+0x00` refcount, `+0x04` stop controller, `+0x08` line, `+0x0c` stop item, `+0x20`
  segment to next node (`+0xc` cost), `+0x24` next node on the line, `+0x30` flags (bit 3 = deleted), `+0x50` docked
  vehicle, `+0x58` waiting count, `+0x5c/+0x60/+0x64` mood-1/2/3 waiters, `+0x68` boarded count, `+0x6c` total wait.
* **Stop controller:** `+0x160` passengerCount, `+0x164/+0x168/+0x16c` mood-1/2/3 waiters, `+0x170` mood.
* **Transport vehicle:** `+0x13c` agent, `+0x14c` line, `+0x154` stopA (last stop), `+0x158` stopB (next stop; docked
  there when `stopB+0x50 == vehicle`), `+0x160` stopTime, `+0x16c` capacity, `+0x170` passengerCount, `+0x174`
  passengers walking to the doors, `+0x186` broken, `+0x188` passengersTransported (`docs/save-format/lines-timeline.md`).
* **Line:** `+0x6c & 0x10000` active, `+0x16c` type, `+0x174` cost factor, `+0x184` ticketPrice, `+0x18c` passengersTransported.
* **Car agent:** `+0x24` current node, `+0x58` speed, `+0x5c` seconds without moving, `+0x78` path steps left, `+0x50`
  queued nodes, `+0x7c` flags (`&5==5` invalid path, `0x1` path stale, `0x10000` broken down, `0x40000` crashing)
  (`docs/save-format/vehicles-movement.md`).

## 2. Population

### 2.1 Place pools *(obs)*

When a grid item is placed and `item.flags & 0x700000 == 0` (not `GRID_ITEM_TEMP`, not `OPTIONAL`, not `0x400000`),
`FUN_100d2d90` registers its places (`0x100c43e0.c:35-36`; also `0x100c4190.c:54`, `0x100bd5a0.c:215,384`):

```
if item lies fully inside the grid:                               // 0x100d2d90.c:22-26
  for cls in 0..6:
    if tpl.homePlaces[cls] != 0:
      n = max(1, ftol(tpl.homePlaces[cls] * homeMultiplier + 0.5))  // bytes 0xd2dfd-0xd2e1b, 0.5f @0x101593c0
      push item n times into freeHomes[cls]
    if tpl.workPlaces[cls] != 0:
      n = max(1, ftol(tpl.workPlaces[cls] * workplaceMultiplier + 0.5))  // bytes 0xd2e90-0xd2ead
      push item n times into freeWorkplaces[cls]
  for t in 0..12: push item tpl.visitPlaces[t] times into visitPlaces[t]   // no multiplier
```

These pools are the only place the multipliers are read (no other reference to `+0xa1c/+0xa20` exists in the
pseudocode) *obs*. Removing an item (`FUN_100bff60.c:91` → `FUN_100d2fb0`) deletes every pool entry for it, then for
each citizen: start or target = item → end the trip, clear start, target and queue; workplace = item → workplace = 0,
`unemployed[cls]++`; home = item → delete the citizen (`0x100d2fb0.c:19-97`) *obs*.

Check: rebuilding the pools from the placed items of Cim2 this way (home slots `max(1, ftol(n·1.0 + 0.5))` per class) and
taking out one home and workplace per saved citizen leaves **0** free home slots for its 32 520 citizens and drops
no saved home (port `engine/sim_citizens.cpp`, `REGS_DEBUG_SIM=1`) *obs*.

### 2.2 Creation *(obs, `0x100d2ac0.c`)*

Every tick, **before** processing citizens, `while freeHomes[cls] is not empty: createCitizen(cls)` for cls 0..6
(`0x100d40e0.c:19-26`). So the population always equals the number of home slots and refills in one tick.

```
createCitizen(cls):
  c = new Citizen; c.cycleTimer = rand() % maxCycleDuration         // ctor
  do id = (rand()&0x7fff)<<15 | (rand()&0x7fff) while byId has id    // :35-41
  c.flags |= cls
  c.carModel = pickCar(c)                                             // §5.5, GENERIC pool
  c.home = popRandom(freeHomes[cls])                                  // FUN_100ce360
  if rand()%100 >= joblessRate: c.workplace = findWorkplace(c)        // :47-51
  link at head; byId[id] = c; count++; population[cls]++
  if c.workplace == 0: unemployed[cls]++
  planDay(c)                                                          // §4.2
  k = (number of 6-bit codes in queue) ? rand() % that : 0            // :67-80
  c.tripQueue >>= 6*k; code = queue & 0x3f
  c.start = code==HOME ? home : code==WORK ? workplace : findPlace(c, code)
  if found && code > 5: flags |= code << 12
  if c.start == 0: c.tripQueue = 0
```

The citizen starts "at" the place of a random trip of its first day. The first processing then pops that same trip;
for HOME/WORK the target equals the start, so it is a no-op trip (`0x100d18f0.c:123-127`) *obs*.

`popRandom(v)` = `idx = ((r2<<12) | (r1 & 0xfff)) % v.size` with r1 the first `rand()`; the slot is replaced by the
last element and the vector shrinks (`0x100ce360.c:31-40`) *obs*.

### 2.3 Deletion and checks per processing *(obs, `0x100d3ca0.c`)*

```
if cycleTimer != 0: cycleTimer--
if home == 0 and (home = popRandom(freeHomes[cls])) == 0: delete; return
if charModel == 0 and (charModel = pickCharacter(c, any gender)) == 0: delete; return
dispatch on flags & 0xf0
```

`delete` (`FUN_100d2c70`) pushes home back to `freeHomes`, workplace back to `freeWorkplaces` (or `unemployed--`),
unlinks, `count--`, `population--`, detaches the controller and ends the trip (`0x100d2c70.c:14-48`). The freed home
is re-occupied by a **new** citizen on the next tick. If no character model matches `modelMask` for a class, its
citizens are deleted on their first processing and immediately re-created, over and over *inf*.

`pickCharacter(c, gender)` (`FUN_100ce180`): uniform among pool records with `eraMask & modelMask != 0` and
(`gender == -1` or equal); 1 `rand()` if any match *obs*.

## 3. Home, workplace, destination choice

### 3.1 Distance acceptance (shared) *(obs)*

`findWorkplace` (`FUN_100ce420`) and `findPlace` (`FUN_100ce5d0`) try up to **32** random candidates. The reference
point is always the citizen's **home**, not its current location. With centre differences in cells
`dx = ((cand.w − home.w) >> 1) − home.x + cand.x`, `dz` likewise with length/z:

```
accept  if  (rand() & 0xfff) <= 0x20000000 / (dx² + dz² + 0x20000)      // 64-bit signed division
```

i.e. p(d) ≈ min(1, 131072/(d²+131072)): 1.0 at d=0, 0.8 at 181 cells, 0.5 at 362 cells, 0.33 at 512 cells
(`0x100ce420.c:46-53`, `0x100ce5d0.c:51-58`). Within a pool every slot is equally likely, so bigger buildings
(more slots) attract proportionally more citizens *inf*.

Cluster filter (only if `clusterData` is set, e.g. `scenario04.scenario:95`): candidate accepted only if
`clusterData[home.z·gridW + home.x] == clusterData[cand.z·gridW + cand.x]` (top-left cells) (`0x100ce420.c:40-45`).

### 3.2 Workplace *(obs, `0x100ce420.c`)*

Requires `reasonProbability[cls].work != 0` (so tourists, pensioners, unemployed never work) and a non-empty pool.
Per attempt: 2 `rand()` for the index, cluster test, then 1 `rand()` for the distance test. On success the slot is
removed from `freeWorkplaces[cls]` (swap with last). Returns 0 after 32 failures.

Employment changes only at the start of a new day plan (`0x100d18f0.c:66-84`):
`if rand()%100 < joblessRate: (employed → put job back into the pool, unemployed++, workplace = 0)` else
`if unemployed: workplace = findWorkplace(); if found unemployed--`. Script: `joblessRate = int(1 + (3 − 0.6·h)²)`
(`economy.script:95-96`).

### 3.3 Visit destination *(obs, `0x100ce5d0.c:20-66`)*

Candidate from `visitPlaces[type]` (2 `rand()`), accepted if all hold, in this order:
1. cluster filter (§3.1);
2. `(tpl.barredMask & (1 << class)) == 0` (`preventVisitors`);
3. for types 3–5 (shopping, leisure, government) with `tpl.openDays != 0`:
   `(clock.date + cand.z + cand.x) % period < open`;
4. distance test (1 `rand()`).

Returns 0 after 32 failed attempts. Visit places are never consumed.

## 4. Daily cycle

### 4.1 Trip reason roll *(obs, `FUN_100cbd50`)*

`p = reasonProbability[cls]`. Employed: `r = rand() % 100`. No workplace: `r = rand() % (100 − p.work) + p.work`
(division by zero if `work == 100`). Then `r < work` → WORK (2), `< work+shopping` → SHOPPING (3),
`< work+shopping+leisure` → LEISURE (4), else GOVERNMENT (5). The fourth value never matters. With the shipped values
(sums ≥ 105) GOVERNMENT is never rolled (`economy.script:88-94`).

### 4.2 Day plan *(obs, `FUN_100cbdf0`)*

```
queue = 0; pos = 0
repeat 3 times:
  r = rollReason()
  if r already in queue: continue                    // duplicates dropped
  queue |= r << pos; pos += 6
  if r == WORK and (u = workplace.tpl.workVisitType[cls]) != -1
     and random(0,1) < utilProbability[u]:
       queue |= u << pos; queue |= WORK << (pos+6); pos += 12
queue |= HOME << pos
```

Examples: `0x42` = WORK, HOME; `0x1102` = WORK, LEISURE, HOME; with a utility trip WORK, u, WORK, …, HOME. At most 6
codes, HOME at bit ≤ 30, so the word never overflows *inf*. `random(a,b) = a + (b−a)·(rand()/32767.0f)`
(`0x10083250.c`) *obs*.

### 4.3 Cycle timer *(obs)*

A trip can only start when `cycleTimer == 0`; it is decremented on every processing whatever the state, so it also
runs during the trip. It is set at the **departure** of each real trip (`0x100d18f0.c:405-425`):

```
if tripType in {3,4,5} and target.tpl.openDays != 0:
   d = (period − (clock.date + target.z + target.x) % period) − open      // unsigned
   cycleTimer = max(10, (d · 216) / 25)                                   // days × 8.64, unsigned division
else:
   cycleTimer = rand() % (maxCycle − minCycle + 1) + minCycle             // 300..600 in game
```

With scripts' 300–600, a citizen leaves at most every 35–69 game days *inf*. For an event venue (`theatre`
10 of 50 days) `d` is 31–40 days. If `open > period − phase` the subtraction wraps and the timer becomes huge
(about 1.7·10⁸): the citizen never travels again *inf from unsigned arithmetic*. A trip that turns out to be a no-op
(target = start, or no target) does not set the timer, so the next code in the queue is handled on the next
processing (`0x100d18f0.c:123-127`).

## 5. Idle state: trip start and mode choice (`FUN_100d18f0`, state 0)

### 5.1 Waiting *(obs, :48-65)*

If `cycleTimer != 0`, or the pedestrian exists and has not finished its path: remove the car if the node it stands on
is deleted (node flag `0x80`), run the idle pedestrian behaviour (`FUN_100cbed0`: with probability 1/16 per
processing, if the model has a `sitdown` animation, walk to a `GRID_PATH_PARK|GRID_PATH_WALK` (0x18000000) node
within 6 and sit) and return.

### 5.2 Choosing the next trip *(obs, :66-127)*

* `tripQueue == 0` → new day: employment update (§3.2), `planDay`, `start = home`, `target = 0`.
* Non-utility citizen: with probability 1/32 (`rand() & 0x1f == 0`) remove the car, re-pick the character model with
  the same gender and re-pick the car model (`:89-99`).
* Pop codes until a target is found: HOME → home, WORK → workplace (may be 0), other → `findPlace`; for codes > 5 a
  found target sets `flags |= code << 12` (utility trip). The trailing HOME guarantees termination.
* `target == start` or one of them 0 → `endTrip(keepCar=1)`, return.

### 5.3 Costs *(obs)*

Let `cls` = class, `ps = priceSignificance[cls]`, `v = charModel.tpl.speed`, `s = grid.scale` (default 3.0,
`0x100c3140.c:30`).

* **Walk time** (non-utility only, else −1): `W = sqrt(dx²+dz²)·s/v`, start→target centres (`:128-139`).
* **Drive** (computed if no drive path yet and ((carModel ≠ 0 and W ≥ 100.0) or utility trip)) (`:141-226`):
  * start node: if a car exists and has a current node, the `GRID_PATH_START` road node nearest to that car
    (`FUN_100e21e0(graph, x, y, z, 0, 0x10100, 0)`), else `FUN_100e24f0(graph, start, 0x10100, 1, 0.0)`;
  * end node: non-utility first looks for a parking node `FUN_100e24f0(graph, target, 0x8000100, 1, target.l + 4 +
    target.w)` and takes the `GRID_PATH_END` road node nearest to it; otherwise (or for utility trips)
    `FUN_100e24f0(graph, target, 0x20100, 1, 0.0)`;
  * if both exist: `FUN_100e5fe0(graph, startNode, endNode, path, 0)` (return value **ignored**), then
    `fuel = ps · path.cost · 0.1`, `D = path.cost + fuel + (|startCentre − startNode| + |targetCentre − endNode|)·s/v`.
* **Transit** (non-utility only) (`:227-263`): `FUN_1011bf00(lines, start, target, v, route, ps, 0)` →
  `FUN_1011a200(lines, startCentre, targetCentre, v, route, ps, directOnly=0, typeMask=0xffffffff)`. `route.cost`
  `T` includes walking time to the first stop and from the last one (seconds), randomised waiting/ride terms and
  `ps·max(1, ticketPrice − 200)` once per line type (`0x1011a200.c:93-104,196-207`; see `pathfinding.md`).
  The idle code recomputes the fare part alone: `F = ps · Σ_{distinct line types on the route} max(1, ticketPrice − 200)`.

### 5.4 Decision *(obs, :264-387)*

```
if route.nodeCount > 0 and (no drivePath or T < D) and T < W:      → TRAVEL (0x30)
     travel[cls]++, canTravel[cls]++, building stat "transit"; remove car
elif no drivePath or no startNode or no endNode or (W <= D and W != -1 and no car):
     building stat "other"; walk[cls]++
     if route.nodeCount > 0: canTravel[cls]++; if T − F < W: priceTooHigh[cls]++
     if W == -1 or W·v > 250.0:      → no movement: fire onCitizenArrive, endTrip(keepCar=0)   // "teleport"
     else:                            → WALK (0x10); remove car
else:                                 → DRIVE (0x20)
     if not utility: building stat "other"; drive[cls]++
        if route.nodeCount > 0: canTravel[cls]++; if T − F < D − fuel: priceTooHigh[cls]++
free drivePath unless DRIVE; free route unless TRAVEL; set cycleTimer (§4.3)
```

Consequences *inf from the code above*:
* A trip shorter than 100 s of walking is never driven; a trip longer than 250 world units (≈83 cells) is never walked.
* A citizen whose car is parked at the start always drives when a road path object exists, even if walking is cheaper.
* `W ≤ D` with no car and `W·v > 250` → the citizen teleports although it could drive.
* "Price too high" = transit was available, was not chosen, and would have won without fares (against walking, or
  against driving without its fuel term). The teleport branch counts as a walk.
* Utility trips never walk or use transit: they drive, or teleport (counted as `walk`).
* If start or end road node is missing, `drivePath.cost` stays 0, so `T < D` fails and transit can never win; the
  citizen walks or teleports.
* If the road search fails, `path.cost` is left at the cost of the last explored node (`0x100e5fe0.c:85-89`), a finite
  value; driving may then be chosen with an unusable path, which the DRIVE state abandons (§7).

### 5.5 Car ownership *(obs, `FUN_100ce250`)*

`carModel` is picked at creation, on the 1/32 refresh and by `resetTraffic`. Pool = `carPools[type·7 + cls]` where
`type = (flags >> 12) & 0xff` (0 for normal trips). For type 0, first `if random(0,1) > carMultiplier: no car`.
Then a weighted pick over records with `eraMask & modelMask`; a picked record with `partCount == 0` means "no car".
`game.script:115` adds such an empty record (weight 72) next to real cars (weight 7 each), so ownership is
`carMultiplier · Σw(cars)/Σw(all)` *inf*.

### 5.6 Building statistics `item+0x5c` *(obs)*

On the trip's start item: bits 20–23 `T` (transit trips), 16–19 `N` (other trips), 12–15 `S` (transit share in 1/16):
* transit: `T == 15` → clear T and N, set S = 15 (OR 0xf000); else `T++`;
* other: `N == 15` → `r = (T<<16)/(T+15)`, cap 0xf000, `v = (v & 0xff000fff) | r`; else `N++`.

`r` is not masked, so bits 0–11 receive the fraction (e.g. T = 3 → `0x2aaa`) *obs*. `S` is displayed by the data view
`GRID_VIEW_POPULARITY` (0x80000) as `(v >> 8) & 0xf0` (`0x100eafd0.c:111-112`). Utility drives do not count.

## 6. WALK state (0x10, `FUN_100cc800`) *(obs)*

* No pedestrian: spawn at the start item's entrance (`FUN_100e6610`) with `FUN_100c8e40(characters, charModel.tpl,
  x, y, z)` and path to the target's entrance with `FUN_100e6cf0(graph, from, to, char.path)`; fire controller
  `onSpawnCharacter`.
* Path done: if `flags & 0x600` (pedestrian came from a vehicle or was kept) clear them and path from the
  pedestrian's position to the target; else fire `onCitizenArrive`, `endTrip(0)`.

## 7. DRIVE state (0x20, `FUN_100ceee0`) *(obs)*

1. **Enter** (no phase bit, no car or parked car `0x100000`): unless the start item is a road (type 1–7), spawn or
   re-path a pedestrian to a walkable cell next to the start road node or the parked car (`:190-249`); set `0x100`.
2. **Phase `0x100`** (`:42-155`): wait for the pedestrian, delete it; wait until `FUN_100e14b0(startNode)` allows a
   spawn; model = carModel (utility: `pickCar` from the utility pool). No model or no start node → `endTrip(0)`.
   Spawn the car (`FUN_10108b30(vehicles, part0, startNode, 0, ftol(Σ(front+rear)/6 + 0.99))`, trailers with
   `posOnNode = −Σlen·65536`; bytes `0xcefe8-0xcf0fe`), paint by `colorMap` seeded with the citizen id, create the road
   path if missing, give it to the car, fire `onSpawnVehicle`.
3. **Driving** (`:253-436`):
   * `car.flags & 5 == 5`, or stuck > 30 s (`car+0x5c`), or the car has no current node (`car+0x24`), or no
     target → `onCitizenArrive`, `endTrip(0)` (car removed, citizen at target).
   * `car.flags & 1` (path stale) → re-route `FUN_100e5fe0(graph, lastQueuedNode or currentNode, endNode, car+0x40)`.
   * still moving (`+0x78 ≠ 0`, or more than `len+1` nodes queued, or speed ≥ 0.5) → return (with a 1/131072 chance
     per processing at speed > 1 to set agent flag `0x800`).
   * arrived, target is a road → `onCitizenArrive`, `endTrip(0)`.
   * arrived, target not a road: remove the moving car; for a non-utility one-node car, pick a random free
     `GRID_PATH_PARK` node of the target (else in a 6×6 area around the car) and spawn a parked car there
     (`0x2000`, `flags |= 0x100000`); spawn a pedestrian at the car, path to the target, set `0x200`. If the spawn
     cell is outside the grid → `onCitizenArrive`, `endTrip(1)`.
4. **Phase `0x200`**: when the pedestrian arrives → `onCitizenArrive`, `endTrip(keepCar=1)`; the parked car stays
   until the next trip (it is removed for walk/transit, or replaced by a new car for a drive).

## 8. TRAVEL state (0x30, `FUN_100cc990`)

Route semantics *(obs from `0x1011a200.c:158-173` and the leg logic below)*: `route.nodes` lists the boarding node
and every intermediate node of each ride. A node means "ride from this stop to `node.next`"; the alighting stop is
`lastNode.next` and is **not** in the list. `route.leg` indexes the next node.

### 8.1 Walking to a stop *(obs, :40-70,184-232)*
* No pedestrian (or `0x400`): if `nodes[0]` is deleted → `endTrip(0)`. Spawn at the start (or re-path the kept
  pedestrian) and path to `nodes[0].stopItem`; fire `onSpawnCharacter`.
* Path done with `0x200` (just alighted): if `leg < n`, path to `nodes[leg].stopItem` (deleted → `endTrip`); else
  fire **`onCitizenArrive`** (state is TRAVEL here) and path to the target.
* Path done otherwise: if `leg < n` start waiting: clear mood bits (firing `onCitizenMood` if any were set),
  `lineNode = nodes[leg]`, `node.refcount++`, `node.waiting++`, `stop.passengerCount++`, `waitTime = 0`. If
  `leg ≥ n` (final walk done) → `endTrip(0)`.

### 8.2 Waiting, patience and mood *(obs, :234-352)*

`patience p = (id & 0xff) + 256` (256…511). Each processing while the docked vehicle is absent, full
(`capacity ≤ passengers + boarding`) or broken:

```
if waitTime & 7 == 0 and line type not yet in usedTransport:
    Wst = |stopItemCentre − targetCentre|·s/v
    if Wst·0.4 < (ticketPrice − 300)·ps: waitTime = max(waitTime, p)   // gives up next
waitTime++
if waitTime == p>>1:     mood 1  (node.mood1++, stop.mood1++)
elif waitTime == 3p>>2:  mood 1 → 2
elif waitTime > p:       give up: mood → 3, route.leg = n,
                         pedestrian walks to the stop where it last alighted (nodes[leg−1].next) or back to start
each mood change: stop mood update (FUN_101110c0) and onCitizenMood
```

A gave-up citizen keeps `lineNode` until its pedestrian arrives, then `endTrip(0)`: **its start becomes the trip
target**, as if it had arrived (`0x100cc990.c:399-404,452-453`, `0x100cc6c0.c:63`) *obs*.

### 8.3 Boarding *(obs, :353-449)*
A vehicle is docked with room and not broken. The citizen starts boarding only if the pedestrian is within 3.0 units
of its cell position (`FUN_100c7870`), the vehicle's `stopTime < 10.0`, no mood-2 waiter is ahead unless it is mood 2
itself, and no mood-1 waiter is ahead unless it is mood ≥ 1. Then `vehicle+0x174++`, `flags |= 0x100`, the
pedestrian walks to the doors (`FUN_10108350`), and mood 1/2 is cleared (with `onCitizenMood`). When the pedestrian
arrives and the vehicle is still docked: delete the pedestrian, `transportVehicle = vehicle`, `passengerCount++`,
`vehicle+0x174−−`, `node.boarded++`, `node.totalWait += waitTime`, `FUN_10110be0(line, waitTime)`, leave the node
(`FUN_100cbc80`), fire vehicle `onGetPassenger(citizen, waitTime, alreadyPaid)` where `alreadyPaid` = this line type
was already in `usedTransport`, then set its bit, fire controller `onSpawnVehicle(vehicle)`. If the line became
inactive or the vehicle left → `endTrip(0)`.

Port (`engine/sim_citizens.cpp` `stepTravel`, `sim.cpp` `vehicleDoorPoint` and the stop cycle): as above. The mood
order uses the stop node's mood counters; the doors are the point beside the nearest car on the pedestrian's side
(from the model bounds, not door locators); while `vehicle+0x174 ≠ 0` the dwell is held at `D − 1` and cleared after
30 s of the agent's wait (transport.md §3.4). Pedestrians far from the camera (timed walks) or more than 40 units from
the doors board at once. Checked on Vienna: 110 door walks completed, departures still at the documented dwell.

### 8.4 Riding *(obs, :72-182)*
* vehicle deleted (`+8 & 1`), inactive or without agent → `endTrip(0)`.
* agent flag `0x10000` (broken down) or `0x40000` (crashing) and speed < 0.1 → **evacuation**: spawn the pedestrian at
  stopB's item (or, when crashing, at a walkable cell near a random car of the train), `flags |= 0x200`, leave the
  vehicle, set mood 3 + `onCitizenMood`, free the route, state 0x40.
* vehicle moving (`stopB.docked ≠ vehicle`): if `leg < n` and `stopA == nodes[leg]` → `leg++`.
* vehicle docked at stopB: if `leg < n` and `stopB == nodes[leg]` → stay. Otherwise alight: pedestrian at stopB's
  item, `flags |= 0x200`, `vehicle.passengersTransported++`, `line.passengersTransported++`, `passengerCount−−`,
  `onSpawnCharacter`.

## 9. State 0x40 REPLAN (`FUN_100cd5c0`) *(obs)*

Reached only by evacuation. When the pedestrian's path is done: `W` from the pedestrian's cell to the target, new
route `FUN_1011bb50(lines, x, z, target, v, route, ps, 0)` from that position. `route.nodeCount > 0 and T < W` →
TRAVEL; else `W·v ≤ 250` → WALK; else `endTrip(0)`. No statistics, no `onCitizenArrive` in this function.

## 10. Trip end (`FUN_100cc6c0(c, keepCar)`) *(obs)*

Delete the pedestrian unless the target has `GRID_ITEM_SHOW_CHARACTER`; remove the car unless `keepCar`; free
drive path and route; leave a stop (and undo a pending door walk); leave a vehicle; clear flags bits 4–11 and 24–27
(and 12–19 when the target is the workplace, ending a utility round trip); set `0x400` if a pedestrian remains;
`start = target` (home if none); `target = 0`; clear drive nodes, `usedTransport`, `waitTime`; fire controller
`onIdle`.

## 11. Mood and events *(obs)*

| event | object | id | fired | argument |
|---|---|---|---|---|
| onCitizenMood | gsGridCitizens | 6 | mood set/cleared at a stop, evacuation, load | citizen controller *inf* |
| onCitizenArrive | gsGridCitizens | 7 | walk arrival; transit: when leaving the **last** vehicle; drive arrival/abort; idle teleport | citizen controller *inf* |
| onSpawnCharacter | controller | 7 | pedestrian spawned | character controller (`FUN_100c7f80`) *inf* |
| onSpawnVehicle | controller | 8 | car spawned / transit vehicle boarded | vehicle |
| onIdle | controller | 9 | every `endTrip` | none |
| onGetPassenger | gsTransportVehicle | 9 | boarding | (citizen, waitTime, alreadyPaid) |
| onMoodChange | stop controller | 8 | stop mood changed | new mood |

The events are fired only if a handler exists (`FUN_1001c330`); creating the controller is a side effect.
`getMood`: 3 if `0x4000000`, 2 if `0x2000000`, 1 if `0x1000000`, else 0 (`0x100cd800.c`). Stop mood: 3 if ≥ 2
mood-3 waiters, else 2 if any mood-2, else 1 if any mood-1, else 0 (`0x101110c0.c:13-31`). On load, waiting
citizens' moods are rebuilt from `waitTime` with the same thresholds (`0x100d31a0.c:328-369`). Scripts: mood 3 lowers
`customerHappiness[class]` (`ui.script:572-576`); `onCitizenArrive` in state TRAVEL counts passengers
(`player.script:183-186`).

## 12. Per-citizen state machine (pseudocode)

```
process(c):                                         // FUN_100d3ca0
  if c.cycleTimer: c.cycleTimer--
  ensure home and charModel, else delete(c)
  switch c.state:
   IDLE:
     if c.cycleTimer or (c.char and !c.char.pathDone): idleBehaviour(c); return
     if c.queue == 0: updateEmployment(c); planDay(c); c.start = c.home; c.target = 0
     if !utility and rand()&31 == 0: repickLooks(c)
     while !c.target: code = c.queue & 63; c.target = resolve(code); c.queue >>= 6
     if c.target == c.start or !c.start: endTrip(c, 1); return
     W, D, T, F = costs(c); choose mode (§5.4); set cycleTimer
   WALK:   spawn/path pedestrian; on arrival: onCitizenArrive; endTrip(c, 0)
   DRIVE:  phases enter → 0x100 (walk to car, spawn car) → drive → park → 0x200 (walk) → onCitizenArrive; endTrip(c, 1)
   TRAVEL: walk to stop → wait (patience, mood, fare check) → board → ride (leg++) → alight
           → (transfer: walk, wait…) → last alight: onCitizenArrive → final walk → endTrip(c, 0)
           failures: deleted node/vehicle, inactive line → endTrip(c, 0); breakdown → REPLAN
   REPLAN: when pedestrian stops: re-route → TRAVEL / WALK / endTrip(c, 0)
```

## 13. `rand()` consumption (for deterministic replays) *(obs)*

* Creation: id 2×(per attempt), `pickCar` (1 float + 1 int if weights > 0), home 2, jobless 1, workplace 2–3 per
  attempt, `planDay` 3 rolls + 1 float per WORK with util type, trip index 1 (if > 0 codes), `findPlace`.
* Idle: jobless 1 + workplace on a new day; `planDay`; 1/32 refresh (1, then character 1 + car); `findPlace`; the
  transit Dijkstra (many `random()` calls); cycle timer 1 (normal case only). `rand()` is shared with every other
  subsystem; the order depends on the round-robin order of citizens.

## 14. Failure paths (summary)

| situation | result | evidence |
|---|---|---|
| no home slot / no character model | citizen deleted | `0x100d3ca0.c:16-29` |
| no target for a code | next code in queue | `0x100d18f0.c:102-120` |
| target = start | no-op trip, no timer | `0x100d18f0.c:123-127` |
| no mode, distance > 250 | teleport, counted as walk, `onCitizenArrive` | `0x100d18f0.c:294-326` |
| route null / first stop deleted / stop node deleted | teleport, no event | `0x100cc990.c:36-38,45,234,452` |
| vehicle deleted or line inactive while riding | teleport, no event | `0x100cc990.c:73-74` |
| waited > patience or fare check failed | mood 3, walk away, then logically at target | §8.2 |
| vehicle broken/crashing | evacuate, mood 3, REPLAN | `0x100cc990.c:75-135` |
| car path invalid / stuck > 30 s | `onCitizenArrive`, car removed | `0x100ceee0.c:253-255,437-445` |
| no car model at spawn | teleport, no event | `0x100ceee0.c:151` |
| building of start/target removed | trip ended, plan cleared | `0x100d2fb0.c:82-87` |

## 15. Open questions

1. The exact transit cost terms (hidden x87 operand `fStack_78` in `0x1011a200.c:102`, meaning of line `+0x174`)
   belong to `pathfinding.md`; the comparison here assumes all terms are seconds.
2. `FUN_100e24f0` search radius units (cells or quarter cells) for the parking search `target.l + 4 + target.w`.
3. Item `+0x5c` low 12 bits on buildings: the unmasked share fraction lands there; whether anything reads them is
   unknown (roads use bits 4–11 for traffic).
4. Whether `onCitizenArrive` / `onCitizenMood` receive the controller (inferred from `FUN_100cc610`'s return value in
   EAX feeding `FUN_1001faf0`'s vararg; the push is not visible in the pseudocode).
5. Vehicle `stopTime ≥ 10` blocking boarding: whether `stopTime` counts up or down is a transport-side question.
6. `getRandomCitizen` picks the first id ≥ a random 30-bit number (wrapping to the smallest), so it is biased by id
   gaps (`0x100ce110.c`); not verified at run time.
7. The 1/131072 per-processing flag `0x800` while driving (`0x100ceee0.c:273-292`) looks like a stop-and-go
   behaviour; its effect lives in the vehicle code.
