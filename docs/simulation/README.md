# Simulation (native, `GSystem.dll`)

How the native simulation runs: what is ticked, in which order, with which time step, and where each subsystem's
code lives. The per-subsystem traces are in separate files:

| File | Scope |
|---|---|
| [pathfinding.md](pathfinding.md) | path network graph, route search, `routeExists` |
| [citizens.md](citizens.md) | population, daily cycle, trip reasons, destination and mode choice, mood |
| [transport.md](transport.md) | lines, stops, transport vehicles, boarding/alighting, background traffic |
| [grid-traffic.md](grid-traffic.md) | grid tick, movement along paths, traffic, buildings, city timeline |
| [economy.md](economy.md) | the scripts' books: fares, monthly costs, loans, reputation, company value, annual report, objectives |

Evidence is cited as `0xADDR.c:line` in `decompiled/GSystem-v2/functions/`. Claims are tagged *observed* (read
directly) or *inferred*. `tools/callgraph.mjs` lists direct callees/callers in that pseudocode.

## Key findings

**Citizens** ([citizens.md](citizens.md))
* Population is determined by housing. Every free home slot gets a citizen on the next citizens tick. Home slots
  per building are `max(1, round(homePlaces × homeMultiplier))`, fixed when the building is placed.
* Each citizen is processed about once per scaled second. All citizen timers (cycle, waiting, patience) count those
  updates, and a game day is 8.64 of them.
* Homes, jobs and destinations are drawn at random with a distance falloff: accept if
  `(rand() & 0xfff) ≤ 2²⁹ / (dx² + dz² + 2¹⁷)`, measured from home, with up to 32 tries.
* Mode choice:
  * Transit wins if it is cheaper than both car and walking.
  * Otherwise the car, if one is usable.
  * Otherwise walking, up to 250 world units.
  * Beyond that the citizen is moved straight to its destination (counted as a walk).
  * "Price too high" means transit would have won without the fare.

**Pathfinding** ([pathfinding.md](pathfinding.md))
* There are three separate networks and searches, none of them A*:
  * a lane graph for cars and transit vehicles, a Dijkstra with a goal-biased cost multiplier;
  * a two-phase cell search for pedestrians;
  * a Dijkstra over line-stop nodes for transit, with randomised costs and the fare charged once per transport
    type.
* Every search consumes the single C `rand()` stream.

**Public transport** ([transport.md](transport.md))
* Vehicles only dock and open doors; boarding and alighting happen in the citizen tick.
* Dwell time is `clamp(headway + 4 − sinceDeparture, 4, 15)` s, where `headway = Σ(segment cost + 5) / vehicles`.
* Riders get off wherever their route's next node is not the current stop.
* Breakdowns are rolled once per second from `condition`.

**Grid, traffic and the city** ([grid-traffic.md](grid-traffic.md))
* Traffic uses node claims: each lane node has an occupant and a reserver, conflicting nodes are listed per node,
  and there is a waiting-priority bit. There are no traffic lights and no lane-change manoeuvre.
* Crashes only involve cars of citizens flagged as reckless.
* The whole map simulates everywhere. Near a camera, vehicles update every frame; far away they update round robin
  at 5 Hz with dt = 0.2 s. Only visuals and cell geometry are limited to the camera area.
* The building pass maintains a per-building traffic heat value (the traffic view). Fires are cosmetic timed objects.
* City growth is the timeline: dated create/delete actions add buildings, buildings add home and work slots, and
  new home slots get citizens.

**For a reimplementation:**
* Results depend on frame dt (truncations, round-robin windows) and on the exact order of `rand()` calls.
  Bit-exact replays would need the original update order. A port can instead fix the tick rate and accept
  statistically equivalent behaviour.
* The quirk and open-question sections of each file list the suspected engine bugs: the wrong terrain row in the
  eye-distance sample, the ring-capacity modulo in the platoon check, stop counters drifting when a stop is deleted,
  and `boardingReserved` going negative.

## Update tree and time

Every tickable object implements a secondary interface (`gsMoveable` in RTTI) whose slot 0 is `bool tick(float dt)`.
It is called with `this` pointing at the secondary subobject: object + 0xcc for `gsModule`-derived classes, + 0x134 for
`gs3DElement`-derived ones (+0x144 for `gsTransportVehicle`). Offsets inside tick code are relative to that subobject.

```
main loop (FUN_10082da0)                      dt = wall time, clamped to [0.0001, 0.05] s
└─ gsMain.tick(dt)            0x100a0700      a forced fixed step (+0x30 of the subobject) replaces dt when enabled
   ├─ script thread groups    0x1002c5d0      group.time += dt·group.speed; wake threads whose time has come
   └─ children in order       0x100a0670      each enabled child's tick(dt); a child returning false is removed
      ├─ gsWindow … gsScene.tick 0x10092470   x = dt·timeSpeed; n = max(1, ftol(x/maxFrameTime + 0.99)); n × children(x/n)
      │  └─ gsGrid.tick       0x100c48c0      x = dt·timeSpeed; n = 1 + trunc(5·x); per step: cells, then children
      │     ├─ gsGridGraph     (no tick)
      │     ├─ gsGridVehicles  0x1010f1e0
      │     ├─ gsGridCharacters 0x100ca490
      │     ├─ gsGridCitizens  0x100d40e0
      │     └─ gsTransportLines 0x101111d0      only ticks its children (the lines) via 0x100a0670
      └─ gsClock.tick ×2      0x10073f80      $sceneClock, $mainClock: ms += dt·speed; fires onHour/onDay/onMonth/onYear
```

*Observed:*
* The main loop clamps dt to [0.0001, 0.05] s (`0x10082da0.c`).
* `gsScene` defaults are `timeSpeed = 1.0` and `maxFrameTime = 0.1` (`0x100920d0.c:30-31`). The scene sub-step
  count is `ftol(dt·timeSpeed / maxFrameTime + 0.99)`, at least 1. The constant 0.99f sits at `0x10159594`, and the
  instructions are at file offset `0x92470`.
* The grid sub-step count is `1 − ftol(dt·timeSpeed · −5.0)`, i.e. `1 + trunc(5·dt·timeSpeed)`. The constant
  −5.0f sits at `0x101643ac`, and the instructions are at file offset `0xc491c-0xc494f`. Each grid step therefore
  covers at most about 0.2 s of scaled time.
* The grid creates its modules when it is attached to a parent (`FUN_100b4f90`, the override of vtable slot 41 at
  `+0xa4`). The order is graph `+0x208`, vehicles `+0x20c`, characters `+0x210`, citizens `+0x214`, lines `+0x218`.
  Each is attached with `child->setParent(grid)`, and that order is the tick order.
* `gsGrid.physicTime` (`+0x234`) is the wall-clock milliseconds the last grid tick took (`0x100c48c0.c:522`). It is a
  profiling value, not simulation time.
* Objects created by script without an explicit parent, such as clocks from `clock()`, become children of the root
  `gsMain`. `clock()` (`0x10074280.c`) attaches the new clock to `FUN_1009fdf0()`, which returns the object stored by
  `gsMain`'s `setParent` (`0x100a15c0.c:25`).
  * The scene's parent is the window (`new_scene($window)`, `engine.script:4`).
  * The window is created before the clocks, so the scene subtree ticks before `$sceneClock` and `$mainClock`. This
    order is *inferred* from creation order.
* `gsClock.tick` (`0x10073f80.c`, `0x10073c40.c`):
  * It adds `dt·speed` seconds to an accumulator and moves whole milliseconds into `time`, which is the ms since
    midnight.
  * Days roll over at 86 400 000 ms.
  * It then recomputes the hour, day, month and year and fires `onHour` (event 9), `onDay` (8), `onMonth` (7) and
    `onYear` (6) when they change.

*Game speed* (`metro/ui/minimap/minimap.script:174-176`): one setting drives three separate time bases.
* `grid.timeSpeed = gameSpeed` (0, 1, 4 or 10)
* `mainClock.speed = gameSpeed · 10000`
* `particleEngine.timeSpeed = max(1, gameSpeed)`

Consequences:
* One real second at speed 1 is 10 000 game seconds, so a game day lasts 8.64 real seconds.
* The simulation itself (movement, boarding, citizens) advances in *scaled real seconds*, not in game time. A vehicle
  moving at a given speed covers the same distance per game day at every game speed.
* At speed 10 the grid gets up to 0.5 s per frame in 3 sub-steps.
* Pre-simulation (`presimulate.script`) calls `$scene.tick(1.0)` 125 times with events off. That is 125 s of grid
  time in 0.1 s scene steps, while the clock does not advance because it is not under the scene.

## Deliberate deviations of the port (ReGS)

The port deviates from the original on purpose in these two places. They are not open questions.
* **Water is not walkable.** The original starts the ground walk layer as open ground everywhere, including water
  cells (pathfinding.md §1.4). Pedestrians then wade through lakes and rivers when that is shorter. The port starts
  water cells (terrain flag 0x2000) blocked, so pedestrians walk around the water or over piers, bridges and decks.
  The user asked for this.
* **Boats survive a save.** The original rebuilds the generated water lanes when it saves. Every agent on them gets
  agent flag 1 and is despawned on the next tick (boats-aircraft.md §2.7 Q4). The port keeps its lanes and boats
  running. It writes the boats' lane references into the lanes the next load will lay from the flags written now. A
  boat on a lane that will not exist then is not written. It writes no agent flag 1. Loading the original's saves
  matches the original: their background boats all carry flag 1 and are not placed (Vienna: 4 of 4), and new boats
  spawn as usual (2026-11-18). Water buses (line vehicles) are placed either way.

Test and debugging switches of the port's simulation (environment variables, `engine/sim*.cpp`):
* `REGS_DEBUG_SIM=1` turns on the periodic logs.
* `REGS_DEBUG_FOLLOW=<line type>[:n]` keeps the camera on the n-th line vehicle of a transport type (a number:
  0 bus, 1 tram, 2 metro, 3 boat, 4 helicopter) and films its turns at a terminus.
* `REGS_DEBUG_SAVE_AT=<ticks>:<name>` saves the game after that many ticks.
* `REGS_TEST_TERMINUS=<line>` removes all stops of the line except the first two, so its trains must turn round.
* `REGS_TRAFFIC_DENSITY` and `REGS_POPULATION_DENSITY` override the density knobs.
* `REGS_STRIDE_SHOTS=<prefix>` takes walk-cycle close-ups.
* `REGS_TEST_CRASH=<n>` makes drivers within 150 units of the camera reckless until n crashes happened, to film one.
* With `REGS_DEBUG_SIM=1` the log also counts door walks and reserved seats, classifies failed walk searches (search
  cap or no connection) with the items at both ends, and reports how many loaded pedestrians continue a saved walk.

Port simplifications that stay (not deviations of behaviour):
* Pedestrians far from the camera walk by time (no path); they board without walking to the doors.
* Agent fields the port does not model are saved as 0, the original's reset value (smoothed acceleration, lateral,
  straight run, last tick, re-path timer, next-leg steps, bell / announce flags, the pedestrians' seated bits).

## Code map

The simulation code is the block `0x100b4000–0x1011c000`: 1 064 functions, 417 KB. It is ordered by class, and each
registrar (`registerMembers`, vtable slot 30) sits near the end of its class's code:

| Range (approx.) | Class | Registrar |
|---|---|---|
| 0x100b4000–0x100c6c00 | `gsGrid` | 0x100c6b20 |
| 0x100c6c00–0x100cc000 | `gsGridCharacterController`, `gsGridCharacters` | 0x100c87a0 |
| 0x100cc000–0x100cde00 | `gsGridCitizenController` | 0x100cdda0 |
| 0x100cde00–0x100d4200 | `gsGridCitizens` | 0x100d3d90 |
| 0x100d4200–0x100d6700 | `gsGridController` | 0x100d6650 |
| 0x100d6700–0x100e6000 | `gsGridGraph` (incl. 40 KB cell builder `0x100d6ab0`) | 0x100e1b10 |
| 0x100e6000–0x100f0b00 | `gsGridObject`, `gsGridItemController` | 0x100f0ae0 / 0x100ef830 |
| 0x100f0b00–0x10100900 | `gsGridPlayer` (editing tools) | 0x10100850 |
| 0x10100900–0x10106700 | `gsGridTimeline` and actions | 0x10106630 |
| 0x10106700–0x10110000 | `gsGridVehicleController`, `gsGridVehicles` | 0x10108000 / 0x1010f110 |
| 0x10110000–0x1011c000 | `gsTransportLine(s)`, `gsTransportStopController`, `gsTransportVehicle` | 0x1011bba0 … |
