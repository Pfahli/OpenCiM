# Grid tick, traffic movement, buildings and the city timeline

Scope: the per-frame `gsGrid` tick, the round-robin building pass (traffic heat, timers, fire), the background
vehicle manager `gsGridVehicles` and the per-vehicle mover, pedestrians and animals (`gsGridCharacters`), and the
city timeline (`gsGridTimeline`). Path search and graph construction are in [pathfinding.md](pathfinding.md); citizen
decisions in [citizens.md](citizens.md); lines, stops and player vehicles in [transport.md](transport.md). Save-file
layouts of the same structures are in [`../save-format/`](../save-format/README.md).

Evidence is `0xADDR.c:line` in `decompiled/GSystem-v2/functions/`. Constants that the pseudocode hides inside
`__ftol` calls (`FUN_101210d8`) were read from the instruction bytes of `GSystem.dll` (file offset = VA − 0x10000000);
those are cited as `bytes@VA`. Tags: *obs* = read directly, *inf* = inferred.

Offsets inside tick functions are relative to the `gsMoveable` subobject (see the README). Below, all offsets are
converted to **absolute object offsets** unless written as `this+…`.

---

## 1. Data structures

### 1.1 Grid fields used by the simulation (`gsGrid`, absolute offsets)

| Offset | Type | Name (proposed / script) | Use | Evidence |
|---|---|---|---|---|
| 0x13c / 0x140 | i32 | `width` / `length` | cells | native-api |
| 0x144 | i32 | bucketShift | log2 bucket size in cells (4 → 16×16 cells) | `0x100c48c0.c:126-147` *obs* |
| 0x148 / 0x14c | i32 | bucketsX / bucketsZ | bucket grid size | `0x100c48c0.c:87-101` *obs* |
| 0x150 | Bucket** | buckets | row-major, `buckets[bz*bucketsX+bx]` | `0x100c48c0.c:92-93` *obs* |
| 0x154 | f32 | `scale` (cellSize, 3.0) | world units per cell | `0x100eb5a0.c:42-47` *obs* |
| 0x1c0 | WalkLayer | groundWalkLayer | pedestrian walk graph, ground layer | `0x100c9470.c:120` *obs* |
| 0x1c4 | {i16 h, u16 flags}[] | terrain | per cell | `0x100b6240.c` *obs* |
| 0x1e8 | u8[256·256·16] | areaRaster | category counts (§5.2) | `0x100b6240.c:48-89` *obs* |
| 0x1ec | map<string,Template*> | templates | lookup by name (vtable slot 0xec = `FUN_100bacb0`) | `0x100bacb0.c` *obs* |
| 0x208…0x218 | ptr | graph, vehicles, characters, citizens, lines | child modules | native-api |
| 0x21c | u32 | `viewMode` | bit 0x8 trees, 0x10 network, 0x20 buildings, 0x100 underground, 0xf0000 data-view selector | `0x100e8250.c`, `0x100eafd0.c` *obs* |
| 0x224 | set<gs3DCamera*> | cameras (`addCamera`) | head node at 0x228 | `0x100bd510.c`, `0x100c48c0.c:105` *obs* |
| 0x230 | f32 | `timeSpeed` | 0, 1, 4, 10 (minimap) | `0x100c48c0.c:82` *obs* |
| 0x234 | f32 | `physicTime` | ms of the last tick (profiling) | `0x100c48c0.c:522` *obs* |
| 0x238 | i32 | buildingCursor | round-robin bucket index (§4) | `0x100c48c0.c:284-287` *obs* |
| 0x23c | i32 | areaRasterCursor | §5.2 | `0x100b6240.c:41,242` *obs* |
| 0x240 | u8 | `showMarkers` | also suppresses fires | `0x100c48c0.c:326` *obs* |
| 0x241 | u8 | deferredRebuild | set during `applyChanges` | batch-A *obs* |
| 0x244 | i32 | itemSerial | next item serial (`item+0x68`) | `0x100c4190.c:45-46` *obs* |
| 0x248 | f32 | `forceInterval` | if > 0 replaces the frame dt | `0x100c48c0.c:76-78` *obs* |
| 0x24c | u8 | `startFire` | forces the next eligible fire, cleared when one starts | `0x100c48c0.c:292-294,430` *obs* |
| 0x24d | u8 | `disableEvents` | no fires, no reckless drivers | `0x100c48c0.c:328`, `0x100ceee0.c:270` *obs* |
| 0x24e | u8 | `ignoreCitizens` | load only: skip vehicle/citizen sections | `0x100c3310.c:67,137,147` *obs* |
| 0x250 | u32 | `effectConditions` | mask that particle emitters must match | `0x100eb5a0.c:55-61`, `0x1005d0a0.c:22` *obs* |
| 0x254 | list | free graph nodes | see save-format | save doc |
| 0x258 | map<Instance*,f32> | fades | target alpha per render instance (§5.3); head 0x25c | `0x100c0bd0.c`, `0x100c48c0.c:459-504` *obs* |

### 1.2 Bucket (one per 16×16 cells)

| Offset | Type | Name | Meaning | Evidence |
|---|---|---|---|---|
| 0x00 | Grid* | grid | | `0x100eb5a0.c:23` *obs* |
| 0x04 | RenderBatch* | batch | vtable 0x38 = clear; created lazily for vehicles | `0x100e0710.c:41`, `0x101078d0.c:57-66` *obs* |
| 0x08 | i32 | dirty | −1 = rebuild pending | batch-A *obs* |
| 0x0c | Item* | items | linked by `item+0x14` | `0x100c48c0.c:243-245` *obs* |
| 0x10 | Agent* | vehicles | linked by `agent+0x10` | `0x1010f1e0.c:169-185` *obs* |
| 0x14 | Character* | characters | linked by `char+0x10` | `0x100ca490.c:214-227` *obs* |
| 0x24 | u8 | active | inside a camera's radius this frame | `0x100c48c0.c:94,178` *obs* |
| 0x25 | u8 | wasActive | active state the geometry was built for | `0x100e0710.c:27-44` *obs* |
| 0x26 | u8 | detailBuilt | near-detail geometry built | `0x100e0710.c:29-32,39` *obs* |
| 0x28 | f32 | focusDist | min horizontal distance camera focus → bucket centre, ×S | `0x100c48c0.c:179-185` *obs* |
| 0x2c | f32 | eyeDist | min distance camera eye → bucket centre + 25 up, ×S | `0x100c48c0.c:186-193` *obs* |

`S` = scene scale returned by vtable slot 0x118 (`gs3DElement` delegates to its parent; `gsScene` returns
`scene+0x13c`, `0x10067000.c`, `0x10092270.c`). Its value was not traced; probably 1.0 (*inf*).

### 1.3 Grid item (0x74 bytes, ctor `FUN_100ef3b0` / `FUN_100d4a50`)

| Offset | Type | Name | Meaning | Evidence |
|---|---|---|---|---|
| 0x00 | Bucket* | bucket | | `0x100eb5a0.c:22-25` *obs* |
| 0x04 | Template* | template | `gsGridObject` | |
| 0x0c | Instance* | dataOverlay | data-view overlay instance | `0x100ec350.c:86-95` *obs* |
| 0x14 | Item* | nextInBucket | | `0x100c48c0.c:442` *obs* |
| 0x18 | Item* | nextInSlot | slot search chain | `0x10103b30.c:54` *obs* |
| 0x1c | Item* | parent | | save-format |
| 0x24 | Node* | nodes | the item's path nodes, linked by `node+0xc` | `0x100c48c0.c:297-302` *obs* |
| 0x28 | list | emitters | `{emitter, offset, next}` | `0x100eb5a0.c:55-61` *obs* |
| 0x2c,0x30,0x34 | i32 | x, y, z | cells; y in 1/256 cell | |
| 0x38, 0x40 | i32 | sizeX, sizeZ | footprint in cells (0x3c probably height) | `0x100c48c0.c:324-375` *obs* |
| 0x44 | i32 | orient | 0..3 | `0x100eb5a0.c:48-53` *obs* |
| 0x48 | u32 | flags | table below | |
| 0x58 | u32 | type | `GRID_ITEM_*` | `0x100c48c0.c:303` *obs* |
| 0x5c | u32 | word | bits 0–11 traffic heat (§4.2) or timer countdown for type 0x17 (§4.3); bits 12–15 a 4-bit building value shown by data view 0x80000 | `0x100c48c0.c:303-323`, `0x100eafd0.c` *obs* |
| 0x64 | f32 | animPhase | door/loop animation phase | `0x100eb5a0.c:63-121` *obs* |
| 0x68 | i32 | serial | from `grid+0x244` | `0x100c4190.c:45-46` *obs* |
| 0x70 | SoundInst* | sound | | `0x100eb5a0.c:122-140` *obs* |

Item flags (`+0x48`) used here: 0x200 created outside the grid (detached); 0x1000 animation driven by door nodes;
0x2000 transport stop; 0x100000 preview (timeline); 0x400000 hidden (skipped by the item update and the raster);
0x4000000 animate only when `bucket.eyeDist < 300`; 0x10000000 overlapped by a scheduled timeline object
(triggers `forcedDemolish`); 0x20000000 scheduled for timeline deletion; 0x40000000 free-placed detail; 0x2000000
auto-generated support. *obs* for the code paths cited in the sections below; meanings *inf*.

### 1.4 Path node (0x8c bytes, lane segment; see also save-format `0x07030200`)

| Offset | Type | Name | Meaning | Evidence |
|---|---|---|---|---|
| 0x04 | Link* | out | outgoing links (step codes index this list) | save doc |
| 0x0c | Node* | nextInItem | | `0x100c48c0.c:299` *obs* |
| 0x10 | Node* | nextInCell | list returned by `FUN_100e2070` | `0x100c9470.c:88-90` *obs* |
| 0x18 | Agent* | **occupant** | the agent holding the node | `0x100e14b0.c`, `0x10107560.c` *obs* |
| 0x1c | Agent* | **reserver** | queued second claimant (trailing carriage or follower) | same *obs* |
| 0x20 | ConflictEntry* | conflicts | `{Node* node; ConflictEntry* next}`: nodes that cross this one | `0x100e14b0.c`, `0x100e15d0.c` *obs* |
| 0x24 | u32 | flags | table below | |
| 0x28 / 0x2c | i32 | cellX / cellZ | bucket = cell >> bucketShift | `0x101078d0.c:29-39` *obs* |
| 0x30 / 0x4c | vec3 | start / end | world units | save doc |
| 0x68 | f32 | length | world units | `0x1010de50.c:314-316` *obs* |
| 0x6c | f32 | speedLimit | | `0x101075d0.c:28-35` *obs* |
| 0x70 | f32 | curveFactor | multiplied by model `turning` | `0x101075d0.c:27` *obs* |
| 0x74 | f32 | curvature | for body roll | `0x1010de50.c:376` *obs* |
| 0x78 | i32 | refCount | rings holding the node | `0x10106850.c` *obs* |
| 0x7c | i32 | pedCount | pedestrians on a crossing within 1.5 units | `0x100c9470.c:88-98,145-155` *obs* |
| 0x80 | u32 | generation | path is stale if newer than the path cursor's | `0x1010de50.c:87` *obs* |
| 0x88 | f32 | traffic | vehicle-seconds since the last building pass | `0x1010de50.c:312`, `0x100c48c0.c:299-302` *obs* |

Node flags used by the mover: 0x10 *no-stop* (junction interior, resets the straight-run counter); 0x20 *junction*
(a failed entry marks it); 0x40 emitter condition (sparks etc.); 0x80 deleted; 0x100 road-class bit (fire/crash
helpers search it); 0x1000 door trigger for item animation; 0x400000 *hidden/terminal* (agent invisible, speed scan
stops, a train locomotive detaches); 0x800000 *tunnel* (tunnel sound, visible in underground view); 0x1000000 *waiting
marker*; 0x2000000 dynamic node; 0x20000000 hard block. Class field `flags & 0x1000ff00` (0x500 car road, 0x4000 air,
0x8000 water). *obs* for each use cited below; names *inf*.

### 1.5 Vehicle agent (0x94 bytes, ctor `FUN_101089d0`)

The persisted fields are in [`../save-format/vehicles-movement.md`](../save-format/vehicles-movement.md) §2.3. Fields
that are not saved: 0x00 bucket, 0x04 model (`gsGridObject`), 0x08 render instance, 0x0c animation instance, 0x10
next in bucket, 0x2c controller (`gsGridVehicleController`, created on demand by `FUN_101074a0`), 0x30 owning citizen,
0x34 sound list, 0x38 engine sound, 0x3c emitters, 0x40 path cursor. Constructor: `ringCapacity = 2·L + 4`,
`lengthNodes L = max(model+0x248, arg)`, `straightRun = L − 1`, `lastTick = −1`; the spawn node's occupant (or
reserver) becomes the agent and `occupiedCount = 1` if it got the occupant slot (`0x101089d0.c:38-84`) *obs*.

Model fields (`gsGridObject`): 0x26c `frontOffset`, 0x270 `rearOffset`, 0x27c `acceleration`, 0x280 `braking`,
0x284 `turning`, 0x288 `speed` (a multiplier, ~0.7–2.0 in shipped scripts), 0x28c `tilting`, 0x290 `nodding`, 0x294
`speedNodding`, 0x248 length in nodes.

---

## 2. Time base

```
dt0      = frame dt from gsScene (already × scene.timeSpeed)
if grid.forceInterval > 0: dt0 = grid.forceInterval        // 0x100c48c0.c:76-78
dtS      = dt0 · grid.timeSpeed                             // :82
n        = 1 + trunc(5 · dtS)                               // :83-85, bytes@0x100c4941 (−5.0f at 0x101643ac)
step     = dtS / n                                          // :86
```
*obs.* Each sub-step covers at most 0.2 s of scaled time. `forceInterval` is set to 30/1000 only in video-capture mode
(`engine.script:222`). At `timeSpeed = 0` the sub-steps get `step = 0`, and the building pass and fades do nothing
(`:277` requires `dtS != 0`).

---

## 3. Grid tick (`FUN_100c48c0`, this = grid+0x134)

```
tick(dt):
  t0 = perfCounter()
  S_geo = grid.vfunc118()                     // scene scale, passed to the geometry builder   :81
  dtS, n, step as in §2
  for each bucket: active=0; focusDist=eyeDist=10000.0           :87-102
  S = (grid+0x40)->vfunc118()                                     :103
  for each camera c in grid.cameras:                              :106-228
      R  = max(10 · c.fov, 100) / S          // c+0x188 is the script field `fov`   :109-115, bytes@0x100c49c0
      F  = worldPos(c.target ?: c); F.y = 0  // focus                                :117-127
      E  = c.slot1()                         // eye position (target + rotated offset)
      bx0 = max(0, trunc((F.x − R + W·scale/2)/scale) >> shift);  bx1 = min(bucketsX−1, …F.x + R…)
      bz0, bz1 likewise with F.z, L                                 :128-147, bytes@0x100c4a9c..4b14
      for bz in bz0..bz1, bx in bx0..bx1:
          cx = ((bx<<shift) − W/2 + 2^(shift−1))·scale; cz likewise
          cy = terrain.h[ ((2bz+1)<<(2shift−1)) + ((2bx+1)<<(shift−1)) ] · scale/256   // see quirk Q1
          if (F.x−cx)² + (F.z−cz)² <= R²:                          :172-175
              b.active = 1
              b.focusDist = min(b.focusDist, |F − (cx,0,cz)|·S)
              b.eyeDist   = min(b.eyeDist,   |E − (cx,cy+25,cz)|·S)
  repeat n times:                                                 :231-276
      for bz, bx (row-major):
          b = bucket
          if !b.active:
              if b.wasActive: for item in b.items: stop and free item.sound      :241-251
          else:
              for item in b.items: itemUpdate(item, step)   // FUN_100eb5a0, visual/audio only (§5.1)
          bucketGeometry(b, S_geo, bx<<shift, bz<<shift)    // FUN_100e0710 (§5.4)
      tickChildren(step)   // FUN_100a0670: vehicles, characters, citizens, lines (graph has no tick)
  if bucketsX·bucketsZ != 0 and dtS != 0:
      buildingPass(dtS)                                            :277-457  (§4)
  areaRaster(dtS)                                                  :458      (§5.2)
  stepFades(dtS)                                                   :459-504  (§5.3)
  for each line in lines: if line.flags(+0x6c) & 0x20000: line.clearFlag(0x20000); line.fireEvent(2)   :505-518
  grid.physicTime = (perfCounter() − t0) in ms                     :519-522
```
*obs* unless marked. The lines step at the end is a deferred line event, not traced here (transport.md).

### 3.1 What runs near the camera vs. everywhere

| Work | Near the camera | Elsewhere | Evidence |
|---|---|---|---|
| Item visuals (animation, emitters, ambient sound) | every sub-step, active buckets only | not run; sounds freed | `0x100c48c0.c:241-258` *obs* |
| Bucket geometry (`FUN_100d6ab0`) | built on activation; near detail if `eyeDist < 400 && focusDist < 400` | released | `0x100e0710.c` *obs* |
| Vehicles | every sub-step with `step`, buckets with `active` | round robin, each bucket 5× per scaled second with dt = 0.2 | `0x1010f1e0.c:145-215` *obs* |
| Pedestrians/animals (characters) | every sub-step with `step`, buckets with `eyeDist < 310` | round robin, 5× per scaled second with dt = 0.2 | `0x100ca490.c:186-253` *obs* |
| Citizens | — | all, once per scaled second each | `0x100d40e0.c:28-45` *obs* |
| Building pass (traffic heat, timers, fire) | — | all buckets, ~1 sweep per scaled second | §4 *obs* |
| Vehicle/character animation and sounds | only if `eyeDist < 300` / sound range | off | `0x101081f0.c`, `0x1010de50.c:386-475` *obs* |

The far-field schedule keeps simulated time consistent (5 updates × 0.2 s per second), so a reimplementation can run
everything at a fixed 0.2 s step and treat the near-camera full-rate path as a smoothing detail (*inf*). Movement
results differ between the two paths (step size, the 50 % path-pull roll in §6.2), so the original is not exactly
camera-independent (*inf*).

---

## 4. Building pass (round robin over buckets)

### 4.1 Schedule

```
count = trunc(bucketsX·bucketsZ · dtS)                 // :278-281, bytes@0x100c4f03 (no remainder kept)
repeat count times:
    grid.buildingCursor = (cursor + 1) % (bucketsX·bucketsZ)
    b = buckets[cursor]
    fireCounter = ((rand() & 0x1f) << 15) | (rand() & 0x7fff)   // 20-bit, :289-291 (2 rand() per visit, always)
    if grid.startFire: fireCounter = 0                           :292-294
    expired = null
    for item in b.items:
        s = Σ node.traffic over item.nodes; node.traffic = 0     :297-302
        if item.type == 0x17 (TIMER):
            if expired == null and --item.word == 0: expired = item          :303-311
        else:
            trafficHeat(item, s)                                  §4.2
            maybeFire(item)                                       §4.4
    if expired:
        if template has event 13 (onRemoveInstance): fire it     :444-449
        removeItem(expired)                // FUN_100bff60
```
*obs.* The count has no fractional accumulator, so on small maps at high frame rates it can be 0 every frame (e.g.
16 buckets × 0.0167 s). Shipped maps are 768² or 1280² cells (2 304 or 6 400 buckets), where a full sweep takes
≈1.0–1.07 scaled seconds (*inf* from the formula).

### 4.2 Traffic heat (the "ageing word")

```
w    = item.word & 0xfff
new  = min(0xfff, ((w + trunc(15·s)) · 15) >> 4)          // bytes@0x100c4fee: fmul −15.0f, sub, imul 15, sar 4
if new != w: item.word = (item.word & ~0xfff) | new
             if grid.viewMode & 0xf0000: refreshDataView(item)   // FUN_100ec350
```
*obs* (`0x100c48c0.c:313-323`). `s` is the vehicle-seconds spent on the item's nodes since the last visit: every agent
update (heads and carriages) adds its `dt` to its current node (`0x1010de50.c:312-313`). This is an exponential
moving average with factor 15/16 per sweep; for a constant `s` per sweep it settles at `225·s` and saturates at
`s ≈ 18.2` (*inf*). It is not ageing: the data view 0x60000 (traffic) colours road items (types 2–15) from
`(word >> 4) & 0xff` divided by `max(sizeX,4)·max(sizeZ,4)` (`0x100eafd0.c:24-55`) *obs*. Bits 12–15 are preserved;
data view 0x80000 shows them for buildings (`(word >> 8) & 0xf0`); their writer is in the citizen code (*inf*).

### 4.3 Timer items (type 0x17)

Countdown in bucket visits (≈ scaled seconds). Only the first item reaching 0 in a visit is removed, and timer items
after it in the same visit are not decremented (*obs*, `:303-311`). Writers set the start value: fire and fire trucks
120 (`:379,426`), crash marker and emergency vehicle 60 (`0x10109d70.c:67,95`). The templates `fire-medium`, `crash`
and the emergency/fire vehicle models must therefore be type 0x17 (*inf*).

### 4.4 Fire

```
maybeFire(item):
  if item.type == 0x14 && item.sizeX > 3 && item.sizeZ > 3 && !grid.showMarkers:
      if fireCounter-- != 0: return              // decremented per qualifying building     :324-328
      if grid.disableEvents: return
      inhabited = any(template.homes[0..6] or template.work[0..6] != 0)   // +0x2e8 / +0x304   :329-338
      fireT  = grid.template("fire-medium")                                                  :342-344
      truck  = first entry of citizens+0x5c8 with (entry.mask & citizens.modelMask) && entry.model   :353-364
      if !inhabited or !fireT or !truck: return
      fx = item.sizeX > 6 ? item.x + rand() % (item.sizeX − 6) : item.x − 1                  :367-376
      fz = item.sizeZ > 6 ? item.z + rand() % (item.sizeZ − 6) : item.z − 1
      fire = addObject(fireT, fx, item.y, fz, orient 0, flags 0x40000000)   // FUN_100c4190    :377-378
      fire.word = 120; fire onCreateInstance (event 12)                                       :379-384
      for k in 0..2:                                                                          :385-429
          nd = graphNodeNear(fire, mask 0x100)     // FUN_100e24f0: random own node with the mask, else nearest
          if nd within <3 cells in both x and z of an earlier pick: skip
          addObject(truck, nd.cellX − 1, trunc((nd.start.y + nd.end.y)·128/scale), nd.cellZ − 1,
                    orient rand() & 3, flags 0x40000000).word = 120
      grid.startFire = 0                                                                     :430
```
*obs.* The fire is a pair of timed decorations; nothing damages or removes the building. Probability per bucket
visit is `K / 2^20` for `K` qualifying buildings in the bucket. With ≈1 visit per scaled second and a game day of
8.64 s (README), each building burns with probability ≈ 8.2·10⁻⁶ per game day (*inf*). Fire engines come from the
citizen car pool at `citizens+0x5c8`, filtered by the era mask (`game.script:381-387`). `startFire` forces
`fireCounter = 0` per bucket visit until a fire actually starts.

---

## 5. Other per-frame grid work

### 5.1 Item update (`FUN_100eb5a0`, active buckets, every sub-step)

Visual and audio only (*obs*):
* Skips markers/spawn points (types 0x15, 0x16) unless `showMarkers`, hidden items (0x400000), items with nothing to
  animate, and items not visible in `viewMode` (`FUN_100e8250`).
* Emitters: each emitter updates while `bucket.eyeDist < emitter+0x1f8`, at the item centre, rotated
  `(orient+2)·π/2`, with `grid.effectConditions` (`:42-62`). An emitter emits only if
  `(emitter.conditions & effectConditions) == emitter.conditions` (`0x1005d0a0.c:22`). `game.script:639-643` sets bit 1
  for 15 s at each new year.
* Animation (template `doorOpenStart/Len` 0x398/0x39c, `doorCloseStart/Len` 0x3a0/0x3a4): without item flag 0x1000 a
  loop (`phase = (phase + dt) mod openLen`). With 0x1000 a door: it opens over 1 s while any of the item's nodes with
  flag 0x1000 has an occupant, and closes over 1 s otherwise (`:63-121`; §5.1.1).
* Ambient sounds `engineSound` 0x37c / `moveSound` 0x380 start when `eyeDist < sound.range` (`:122-140`).

#### 5.1.1 Door items (level crossings)

A **door item** is an item with flag 0x1000 and a template `doorOpenLen ≠ 0`. Item flag 0x1000 is the template flag
`GRID_ITEM_CROSS_TRACK`: the item ctor sets `item+0x48 = ctorFlags | template.flags` (L:0x08187fc0, W+0x20c), so the
map files store 0 and the flag comes from the template. Node flag 0x1000 is `GRID_PATH_TRAIN_TRACK`. Two vanilla
templates have `GRID_ITEM_CROSS_TRACK`, both `double_cross_small` (4×4 cells, small road over a double train track,
`GRID_ITEM_DISABLE_TRAM`): `metro/objects/rail-props/` (registered by every road envset as
`track_double_cross_small`, `default-roads.script:179`) and the unused `metro/objects/tracks/` copy (no skeleton, no
door fields, so not a door). The add-on station tracks set `GRID_PATH_TRAIN_TRACK` only. So the only door item is the
**level crossing** (Vienna 2, Berlin 6, Cim2 7 placed); trams never trigger it (tram track is 0x800). *obs*

The crossing (`rail-props/double_cross_small.script`): `loadSkeleton("crossing_scene.skeleton")`, slot `default` =
`crossing_anim.animation` (one take of 2.1667 s, speed 1.0); `doorOpenStart = 0`, `doorOpenLen = 30/30`,
`doorCloseStart = 35/30`, `doorCloseLen = 30/30`; the road paths `t-a*`, `t-b*` are `INTERSECTION | ROAD_SMALL`, the
track paths `t-c0/c1/d0/d1` `INTERSECTION | TRAIN_TRACK`; `moveSound` = "Level crossing warning bell-1.wav",
volume 0.5, min 50, max 300. The clip turns the bones `crossing_barrier01/02` (children of `crossing_main`) about
their X axis from 0° (bind pose: barrier up) to 90° (down, across the road) over 0..0.83 s, holds 90° to 1.167 s, and
turns back to 0° at 2.167 s. *obs* (asset decode); up/down *obs* in the port's renders.

```
doorUpdate(item, dt):                       gsGridItem::move L:0x08182b10.c:96-167 (W:0x100eb5a0.c:63-121)
  T = item.template
  if T.doorOpenLen == 0 or (bucket.eyeDist >= 300 and item.flags & 0x4000000):
      if T.moveSound and item.sound: free(item.sound); item.sound = 0            // :96-104
      goto ambient
  if item.flags & 0x1000 == 0: loop animation (§5.1)
  for node in item.nodes:                                                        // :122
      if node.flags & 0x1000 and node.occupant:                                  // a train holds a track node
          p = item.phase; if p < 0: p += 1                                      // a closing door turns round
          item.phase = min(p + dt, 1)
          animate(item, T.doorOpenStart + item.phase·T.doorOpenLen)
          if T.moveSound and !item.sound and bucket.eyeDist < T.moveSound.maxDistance:   // :136-144
              item.sound = T.moveSound.createInstance(grid); setRelative(false); position = item centre
              setPlaying(true, loop)
          goto ambient
  if item.phase > 0: item.phase -= 1                                             // an opening door turns round
  item.phase -= dt
  if item.phase <= -1: item.phase = -1; animate(item, -1)                        // rest: the skeleton goes
  else: animate(item, T.doorCloseStart − item.phase·T.doorCloseLen)
  if item.sound: free(item.sound); item.sound = 0                                // :166-167
ambient: engineSound loop as in §5.1 (:169-179; the crossing has none)

animate(item, t):                           gsGridItem::animate L:0x08181ff0 (grid-items.md §3.3)
  if bucket.eyeDist >= 400 or t == −1: remove the item's skeleton clone (bind pose)
  else: clone the template skeleton if needed; first slot weight 1, time t·speed; animateBones
```
*obs*. Meaning:
* **Trigger:** the occupant slot (+0x18) of the crossing's own track nodes, nothing else. Agents claim nodes ahead
  of them (§6.3: `look = speed/(6·braking) + 1 + L` nodes, braking 0.1–0.12 for the vanilla trains), so the barrier
  starts to lower when the train's claims reach the crossing, a few seconds before it arrives, and rises as soon as
  the last car released the last track node. There is no distance check and no timer.
* **Timing:** phase 0→1 in 1 s of grid time (clip 0..1 s: barriers down), held at clip 1.0 while occupied; then
  phase 0→−1 in 1 s (clip 1.167..2.167 s: barriers up), then the bind pose. A direction change mid-way continues
  from the mirrored phase (±1), so the barrier never jumps far.
* **Initial state quirk:** the ctor sets `phase = 0`, which is the start of the close clip. The first update of each
  crossing (when its bucket first becomes active) therefore poses it at clip 1.167 s (barriers down) and raises the
  barriers over the next second. At game speed 0 the grid ticks with dt = 0 (§2), so a game loaded paused shows the
  crossings near the camera with their barriers down until it is unpaused (*inf* from the code; the port does the
  same, not checked against the original).
* **Only near the camera:** the update runs in active buckets only (§3.1); elsewhere the phase is frozen and the
  sound is freed when the bucket goes inactive.
* **Road traffic:** the barrier does not block anything. No code sets node flag 0x20000000 at run time (its only
  writer is `gsGridPlayer::createTrackBlock`, the track tools' end block; readers `gsGridPathSegment::isFree`
  L:0x08175700/0x081757e0). Cars stop because the crossing's road nodes cross its track nodes: `canEnter` refuses a
  node whose conflict list holds an occupied node (§6.3), and the road nodes are no-stop junction nodes (0x10), so a
  car waits before the crossing, never on it, exactly while a train holds a track node, and it may drive while the
  barrier is still rising. The reverse holds too: a train cannot claim the track nodes while a car is on the crossing.
* **Sound:** the bell (`moveSound`) loops at the item centre from the first update with an occupied track node while
  the bucket's eyeDist < 300 (its maxDistance), at volume 0.5, pitch 1, linear rolloff 50..300; the sound's own tick
  stops it 0.5 s out of range and starts the loop again in range (audio §6.5). It is freed when the door starts to
  close, so the bell stops as the barriers rise.

Port: `port/src/engine/sim_doors.cpp` (phase and clip time per door item, every sub-step and with dt = 0 at speed 0),
`grid_items.cpp` `collectSkinnedItems` (the item's own pose), `sim_sounds.cpp` `tickDoors` (the bell). The active
bucket is approximated by the horizontal eye distance < 450 (the same stand-in as the pedestrians' real walks).
Checked on Vienna's crossing item 7776 (`port/run/barriers/sheet-e1.png` lowering, `sheet-f-rise.png` rising behind
the last wagon, `sheet-f-wait.png` a car waiting through a train and crossing once the barriers are up): the
barriers start down several seconds before a cargo train at 7 cells/s reaches the crossing (its claims run about
`7/0.6 + 1 + L` nodes ahead) and are up about 1 s after its last wagon left. Vienna's other crossing (1751, by the
freight yard) stays down for minutes: trains standing south of it at the yard keep their claims across it, and
cars queue on both sides (*obs* in the port; whether the original's trains hold claims that long is not checked).

### 5.2 Area raster (`FUN_100b6240`)

While `grid+0x23c < bucketCount` it processes `trunc(bucketCount · dtS · 1.05)` buckets per frame (bytes@0x100b62c9),
or all of them if `dt == −1`. Per cell it counts water, field, painted and pavement flags, and per item its footprint
category, into a 256×256 raster of 16-byte counters. It clears the raster when it starts at bucket 0 (`:48-89`). It
only runs again after something resets the cursor to 0 (`0x100b71f0.c:62-66`, `0x100c3140.c:61`). The consumer is
`gsGrid::renderMap` (`0x100b71f0`), the minimap texture: layout and colours in [../ui/minimap.md](../ui/minimap.md) §2-3 *obs*.

### 5.3 Fades (`grid+0x258`)

`FUN_100c0bd0(grid, target, instance)` queues a visibility fade. Each frame, each entry moves by `dtS` toward its
target, then is set, released and erased (`0x100c48c0.c:459-504`) *obs*. Vehicles fade out on a node with 0x400000,
while `posOnNode < 0`, or in underground view 0x180 off a tunnel node (`FUN_10107220`); pedestrians use the same
mechanism (`FUN_100c77e0`).

### 5.4 Bucket geometry (`FUN_100e0710` → `FUN_100d6ab0`)

`near = eyeDist < 400 && focusDist < 400`. On a change of `active` vs `wasActive`, or when `near` and the detail is
not built, an active bucket builds (`FUN_100d6ab0(b,…,0)`, plus `…,1` if it was inactive), sets `wasActive`, and
places instances (`FUN_100d6450`). An inactive one clears its batch (*obs*). `FUN_100d6ab0` (40 KB) is a mesh builder
(60 calls to a vertex emitter `FUN_100d43a0`, render-instance creation); it does not touch simulation state (*inf*,
skipped).

---

## 6. Vehicles

### 6.1 Manager tick (`FUN_1010f1e0`, this = vehicles+0xcc)

```
vehiclesTick(dt):
  // background spawns (one attempt each per sub-step)
  if trains.size < (graph+0xfc + graph+0x114) / 4: spawnTrain()        // FUN_1010ae20      :26-38
  if boats.size  < trunc(sqrt(graph+0x15c)):         spawnBoat()        // FUN_1010ce70      :39-54, bytes@0x1010f273
  if aircraft.size < min(graph+0x12c, 2):                               //                   :55-76
      if vehicles+0x190: spawnAircraftA()                               // FUN_1010b430
      elif vehicles+0x188 == 0 and rand() even: spawnAircraftA() else spawnAircraftB()   // FUN_1010c190
  for train in trains (from the back):                                 :82-110, 264-276
      animate(train, dt, train.speed)                                  // FUN_101081f0, visual
      if 0 < ringCount <= model.lengthNodes:          // near the end of its queued path
          if !train.next: despawn
          elif curNode.flags & 0x400000:                                // hidden/terminal node
              next.pathStepsLeft = 10000000; next.speed = train.speed; next.prev = 0 (and every later car's prev = 0)
              list entry = next; train.next = 0; despawn(train)         // locomotive drops off
          elif train.waitTime > 0: despawn
      elif train.waitTime > 60 or flags & 1: despawn
  for plane in aircraft (from the back):                               :217-262
      pose(plane) → altitude y
      if waitTime > 60 or flags & 1: despawn
      elif ringCount > L+2 or speed >= 0.1:       // flying / rolling
          gear animation from altitude: off if y <= 20, else openStart2 + clamp((y−20)·0.1, 0, 1)·openLen2
      else:                                       // parked at the end
          if waitTime > 6 or y > 20: despawn
          if waitTime > 2: speed = 0; doors = openStart + clamp(waitTime−2, 0, 1)·openLen
  for boat in boats: if (0 < ringCount <= L) or waitTime > 60 or flags & 1: despawn     :124-139
  tickCounter = (tickCounter + 1) & 0xffff                              :141
  N = bucketsX·bucketsZ;  acc += N·dt·5;  k = min(N, trunc(acc));  acc −= k            :146-153
  for each bucket index i (row-major):
      if bucket.active: dtB = dt
      elif i in [cursor, cursor+k) modulo N: dtB = 0.2
      else continue
      for agent in bucket.vehicles:
          if agent.pathStepsLeft != −1: agentUpdate(agent, dtB, tickCounter)   // FUN_1010de50
          if moveToBucketOfNextNode(agent): unlink here                        // FUN_101078d0
  cursor = (cursor + k) % N                                             :215
  tickChildren(dt)
```
*obs.* `despawn` = `FUN_10108180`: unlink the agent and all its trailing cars from their buckets and destroy them;
the destructor fires `onCrashEnd` (vehicles event 7) if the agent was crashing (`0x10107dd0.c:18-21`). Agents with
`pathStepsLeft == −1` (carriages, parked cars) are never updated by the bucket loop. Carriages are updated by their
head. Road cars, lorries and buses are not spawned here: citizens spawn their cars (`FUN_100ceee0`, citizens.md) and
lines spawn theirs (`FUN_101152a0`, transport.md), both through `FUN_10108b30`. That function refuses an occupied
node unless the agent is chained to a leading car (`0x10108b30.c:17-19`). The graph counters (`+0xfc/+0x114`
passenger/cargo rail entry points, `+0x12c`, `+0x15c`) belong to pathfinding.md.

### 6.2 Agent update (`FUN_1010de50(agent, dt, tick)`)

```
agentUpdate(a, dt, tick):
  if a.lastTick == tick or (a.flags & 5) == 5: return                        :60-66
  a.lastTick = tick
  if !fixDeletedNodes(a): return          // FUN_101068f0: swap deleted nodes (flag 0x80) for replacements, else flags |= 5
  selfDriven = a.pathStepsLeft != −1
  if selfDriven and !(a.flags & 0x50400) and (a.flags & 0x200 or (rand() & 1) == 0):     :72-73
      // ---- 1. pull path steps into the ring ----                              :74-98
      while a.ringCount < a.ringCapacity and a.pathStepsLeft != 0:
          last = ringCount ? ring[ringCount−1] : a.curNode
          if path finished: break
          if path.generation < last.generation: a.flags |= 1 (stale); break
          nd = popStep(path, last); if !nd: break
          pushRing(a and every trailing car, nd); nd.refCount++                      // FUN_10106850
          if a.pathStepsLeft > 0: a.pathStepsLeft--
      // ---- 2. claim nodes ahead (§6.3) ----                                       :99-261
      claimNodes(a)
  // ---- 3. speed ----                                                            :263-311
  vOld = a.speed
  if selfDriven:
      target = targetSpeed(a, dt, head=1)
      for car in a.next…: target = min(target, targetSpeed(car, dt, head=0))         :266-275
      if !(a.flags & 0x40000):                                                       :276-285
          cand = a.speed + model.acceleration·dt
          a.speed = cand <= target ? cand : max(target, 0)
      else:  // crashing                                                            :286-301
          cand = a.speed − 4·dt
          a.speed = cand < target ? max(cand, 0) : target
          a.crashOffset += normalize(a.crashOffset) · (a.speed·dt·0.4)
          if vOld >= 1 and a.speed < 1: crashSettled(a)                               // FUN_10109d70 (§6.6)
      d = trunc(dt · a.speed · 196608)          // 16.16 world units: 3·speed·dt     bytes@0x1010e3ca
      a.waitTime = d == 0 ? a.waitTime + dt : 0
      a.posOnNode += d
  a.curNode.traffic += dt                                                          :312-313
  // ---- 4. advance to the next node ----                                         :314-354
  if a.posOnNode > trunc(a.curNode.length · 65536):                               bytes@0x1010e412
      if a.occupiedCount < 2:          // next node not held
          if (a.flags & 0x40800) == 0x800 and ring[head] and !canEnter(ring[head], a, 1):
              crash(a, ring[head])     // §6.6
      elif a.reservedCount > 0:
          a.posOnNode −= trunc(curNode.length · 65536)
          releaseNode(a, curNode)                           // FUN_10107560 (§6.3)
          unref(a.prevNode); a.prevNode = a.curNode; a.curNode = ring[head]
          a.ringCount--; a.reservedCount--; head = (head + 1) % capacity
  updateVisibilityFade(a)                                   // FUN_10107220
  horn: if model.hornSound and a.waitTime > 5 and (bucket.eyeDist < range or controller.forceSounds)
        and rand(0,50) < dt: play horn, pitch rand(0.8,1.0)                        :356-363
  if dt != 0:                                                                      :364-385
      a.accelSmoothed   += clamp((a.speed − vOld)/dt − a.accelSmoothed, −4dt, 4dt)
      a.lateralSmoothed += clamp(curNode.curvature·a.speed − a.lateralSmoothed, −4dt, 4dt)
  engine/move sounds, wheel rotation, pose (FUN_10106a10; boats bob with sin(t·2.701)/sin(t·3.123)), emitters   :386-669
  if selfDriven:                                                                   :670-689
      for car in a.next…:
          if crashing: car.crashOffset += normalize(car.crashOffset)·dt·a.speed·0.2; crashSettled(car) on the same 1.0 crossing
          car.posOnNode += d
          agentUpdate(car, dt, tick)
          if car.flags & 1: a.flags |= 5
```
*obs* (line ranges as given). Units: `speed` is in cells per second (the distance factor 196608 = 3 × 65536 hardcodes
cellSize 3); `posOnNode`, node lengths and offsets are world units (*inf* from the constant). An agent advances at
most one node per update. Carriages move by exactly the head's distance; their own node changes still go through
claims and releases, so a train holds a contiguous chain of nodes.

The 50 % roll gates **both** path pulling and claiming for AI agents. Each AI vehicle therefore extends its claims
only on about every second update, and every such update consumes one `rand()` (*obs*, `:73`). Flags 0x400 (halted
at a stop), 0x10000 (dwelling) and 0x40000 (crashing) freeze claims.

### 6.3 Node claiming: car-following, gap keeping and right of way

Each node has two slots, `occupant` (+0x18) and `reserver` (+0x1c). An agent holds `occupiedCount` nodes as occupant
(its current node plus the first `occupiedCount−1` ring entries) and `reservedCount` ring entries in total. The leading
agent's target speed only looks across nodes it occupies (§6.4), so **the gap to the vehicle ahead is node-based**:
a follower can never be on or enter a node another agent occupies.

```
claimNodes(a):                                                       0x1010de50.c:99-261
  look = trunc(a.speed / (6·model.braking) + 0.5) + 1 + L           // bytes@0x1010df4c (6.0f, 0.5f)
  ignoreMarks = (a.flags & 0x200) ? 1 : isMarked(curNode)            // player vehicles ignore markers
  for i in 1 .. occupiedCount−1 while !ignoreMarks and i <= ringCount: ignoreMarks = isMarked(ring[i−1])
  if !ignoreMarks and occupiedCount > 0 and occupiedCount+1 < ringCount and ring[occ−1].flags & 0x20:
      ignoreMarks = isMarked(ring[occ−1])
  i = safe = a.reservedCount
  while i < ringCount and min(a.straightRun, safe) < look:
      nd = ring[i]
      if nd.occupant and nd.pedCount == 0 and (ignoreMarks or !isMarked(nd)):
          if nd.occupant.curNode != nd: break
          // platoon: queue behind the leader on nodes it holds, while both rings agree
          leader = nd.occupant; j = 0
          loop: accept nd if nd.occupant == leader and nd.reserver == 0 and j+1 < leader.reservedCount
                       and nd.pedCount == 0 and (ignoreMarks or !isMarked(nd))
                advance i, straightRun (as below); nd = ring[i];
                stop unless nd == leader.ring[(leader.head + j) % a.ringCapacity]; j++     // quirk Q3
          break
      ok = (a.flags & 0x1000 and a.controller) ? canEnterSameLine(nd, a, ignoreMarks, a.controller.lineId)
                                               : canEnter(nd, a, ignoreMarks)
      if !ok:
          if a.flags & 0x200 or nd.flags & 0x20: nd.flags |= 0x1000000     // waiting marker
          break
      i++
      if nd.flags & 0x10: a.straightRun = 0
      else: a.straightRun++; if a.straightRun >= L: safe = i
  commit = safe
  if i == ringCount and safe <= L:
      commit = i
      if ringCount == ringCapacity: grow ring by 2; commit = safe
  for k in a.reservedCount .. commit−1:               // make the claims real
      nd = ring[k]; a.reservedCount++
      if nd.occupant == 0:
          nd.occupant = a; a.occupiedCount++
          if a.next: nd.reserver = a.next; if a.next.curNode != nd: a.next.reservedCount++
      elif nd.reserver == 0: nd.reserver = a

canEnter(nd, a, ignoreMarks):                         FUN_100e14b0
  if nd.occupant: false
  if (nd.flags & 0x1000020) == 0x1000000 and !ignoreMarks: false     // marked, not a junction node
  if nd.flags & 0x20000000: false
  for c in nd.conflicts:
      if (c.occupant and c.occupant != a) or (c.flags & 0x1000000 and !ignoreMarks) or c.flags & 0x20000000: false
  true
isMarked(nd) = nd.flags & 0x1000000 or any(c.flags & 0x1000000 for c in nd.conflicts)     FUN_100e15d0

releaseNode(a, nd):                                   FUN_10107560
  if nd.reserver == a: nd.reserver = a.next (a.next.reservedCount++ if a.next not on nd) or 0
  if nd.occupant == a:
      nd.occupant = nd.reserver                       // promote the queued claimant
      if it exists: occupant.occupiedCount++; nd.reserver = occupant.next (…reservedCount++) or 0
      a.occupiedCount--
  if a.flags & 0x200 or nd.flags & 0x20: nd.flags &= ~0x1000000
```
*obs* (`0x100e14b0.c`, `0x100e1530.c`, `0x100e15d0.c`, `0x10107560.c`). Meaning (*inf*):
* **Look-ahead** grows with speed: `speed/(6·braking)` nodes plus the vehicle's own length plus one.
* **No blocking the box:** claims through no-stop nodes (0x10) only become real when the agent can also claim `L`
  ordinary nodes after them (`safe` lags behind `i`). An agent that cannot clear a junction waits before it.
* **Intersections:** a node lists the nodes that cross it. It can only be entered while none of them is held by
  another agent. There are **no traffic lights**: no code toggles node flags on a timer. The only other blocking flag,
  0x20000000, has no timer-driven writer in the code read.
* **Anti-starvation:** an agent that fails to enter a junction node (or any node, for player vehicles) sets the
  waiting marker 0x1000000 on it. Agents without `ignoreMarks` may not enter that node or any node crossing it. The
  marker is cleared when the node's occupant leaves it (for junction nodes or player vehicles).
* **Following:** when the node ahead is held by a vehicle that is physically on it, the follower registers as
  `reserver` along the leader's claimed chain. On release the reserver is promoted to occupant immediately, so a queue
  advances first-in first-out and cross traffic cannot slip into it. Port note: the promoted node can lie past the
  reserver's held range (it queued for a node behind junction nodes it could enter but had not committed yet), so the
  port's `releaseAll` also releases every later node of the track the mover occupies or reserves; without that,
  despawned cars kept such nodes for good (transport.md §10.5). Since 2026-11-18 the port queues only where the
  original's platoon loop would and commits it: not on a marked node (unless marks are ignored), not on a junction
  node, and right after the committed range; queuing anywhere let cars take a junction's crossing nodes one after
  another past a waiting tram's marker (transport.md §10.3).
* **Lane changes:** there is no lateral manoeuvre. An agent only ever moves to `ring[head]`, which is a node chosen by
  the path (step codes index `node.out`). Lane changes exist only as graph links chosen by the path finder.
* **Same-line passing (0x1000):** for transport type 3 the controller variant `FUN_100e1530` treats nodes held by
  agents of the same line (controller vfunc 0x128) as free (transport.md).

### 6.4 Target speed (`FUN_101075d0(agent, dt, isHead)`)

```
cap(nd) = (t = model.turning · nd.curveFactor) < nd.speedLimit ? max(t, 1.5) : nd.speedLimit
d = curNode.length − posOnNode − model.rearOffset − model.frontOffset − 1.0     // free distance, world units  :25-26
v = cap(curNode)
n = (isHead and !(flags & 0x800)) ? min(occupiedCount − 1, ringCount) : ringCount   :37-46
for i in 0 .. n−1:
    if d >= 10 / model.braking: return v · model.speed                             :49-50
    nd = ring[head + i]
    lim = v
    if isHead and nd.pedCount != 0 and d > −1:                                      :57-68
        lim = min(v, max(0.2, max(d·braking, a.speed − dt·braking·10)))
    v = min(lim, cap(nd) + max(d·braking, 0))
    if nd.flags & 0x400000: return v · model.speed                                  :88-90
    d += nd.length
if d >= 10 / model.braking: return v · model.speed
b = d · braking
return (v <= b ? v : max(b, 0)) · model.speed                                       :93-100
```
*obs.* The allowed speed rises **linearly** with free distance (`cap + braking·distance`), not with its square root.
Braking itself is instantaneous: §6.2 only rate-limits acceleration. A head with flag 0x800 scans its whole ring
instead of its occupied nodes, so it does not plan to stop for nodes it does not hold. That is what makes reckless
drivers crash (§6.6). Pedestrians on a crossing slow the head to a crawl of at least 0.2 (before the model factor),
decelerating at most `10·braking` per second. Note the mixed units in that line: `a.speed` is absolute while the
other terms are before the `model.speed` factor (*obs*; harmless because `model.speed ≈ 1`).

### 6.4a Pose (`gsGridVehicle::calculatePose` `L:0x081abc70`, `W:0x10106a10`)

```
if posOnNode < 0: pose = (curNode.p0, curNode.q0)                    // waiting before the node
s1 = posOnNode + rearOffset − rearAnchor      → point A on the queued nodes (walking on through the ring)
s2 = s1 + rearAnchor + frontAnchor            → point B
each node's curve: ends moved to the joint midpoints with the previous / next node, end tangents the slerp of
                   both nodes' orientations at the joint, handles length/3
position = A + (B − A) · rearAnchor / (frontAnchor + rearAnchor)
dir = B − A (inverted with agent flag 2); with both anchors < 0.11 dir.y = 0
orientation = gsQuaternion(+Y, −dir): the model's front is −Z
```
*obs* (field offsets: Linux `+0x230/+0x234/+0x238` = Windows `+0x270/+0x274/+0x278` = rearOffset, frontAnchor,
rearAnchor). So `posOnNode` is the **rear** of the body and the model origin sits `rearOffset` ahead of it. The tram-05
mesh spans z −2.75…3.57 with frontOffset 2.8, rearOffset 3.4, which confirms the front at −Z [obs].

Paint: `gsGridVehicle::setColor` takes `gsColorMap::getRandomColor(model.colorMap, colorSeed)` (`L:0x080ef430`,
`W:0x100755d0`): pixel `gsPseudoRandom15(seed) % (w·h)` of the model's colour map, the colour the colorize materials
are tinted with; line vehicles take the line colour instead (`gsTransportLine::write_color`, `W:0x10114ec0`) [obs].

### 6.5 Waiting, jams and despawning

* `waitTime` counts seconds without movement (`:302-309`).
* AI road vehicles: a horn after 5 s (random, mean once per 50 s). The citizen layer re-paths a stale car while
  `waitTime <= 30` and otherwise leaves it; arrival despawns the car (`0x100ceee0.c:253-264`, citizens.md).
* Player transport vehicles: if the path is not finished and `waitTime > 180`, the event `onJammed` (13) fires and
  `waitTime` resets (`0x101152a0.c:192-197`) *obs*.
* Background trains, aircraft, boats: the despawn rules in §6.1 (60 s, 6 s, end of path).
* Stale paths (`flags & 1`) come from a graph generation newer than the path, and from `FUN_101068f0` when a replaced
  node has flag 0x8000 (probably water class, which would explain why only boats carry flag 1 in saves; *inf*).

### 6.6 Crashes

```
reckless flag: citizen update, while its car moves (speed > 1) and is not near its path end:
    if !grid.disableEvents and (rand() & 0x7fff) == 0 and (rand() & 3) == 0 and !(car.flags & 0x40401):
        car.flags |= 0x800                                     0x100ceee0.c:266-292   (p = 1/131072 per citizen update)
crash(a, nd):   // a reached the end of its node, does not hold the next one, flag 0x800, canEnter(nd) false
    controller(a); vehicles.fireEvent(6 = onCrashBegin)                          0x1010de50.c:322-325
    startCrash(a); startCrash(nd.occupant); startCrash(c.occupant) for c in nd.conflicts     :326-338
startCrash(x):  head = first car of x's chain; if head not crashing:              FUN_1010a260
    for car in chain: flags |= 0x40000
                      crashOffset = (rand(−1,1), 0, rand(−1,1)) · (dt · head.speed · 0.4)
                      if head.speed < 1: crashSettled(car)
crashSettled(x):                                                                  FUN_10109d70
    t = grid.template("crash"); if t: addObject(t, at the vehicle pose, centred, flags 0x40000000).word = 60
    with p = 1/4: model = first citizens+0x638 entry matching modelMask;
                  addObject(model, near a 0x100-class node of the crash marker, orient rand()&3).word = 60
```
*obs.* Crashes therefore only happen to cars a citizen has randomly marked reckless. Citizens update once per
scaled second (`0x100d40e0.c`), so the rate is about one reckless flag per 131 072 driving-citizen-seconds (*inf*). The
script counts collisions in `onCrashBegin` (`game.script:702-709`). `onCrashEnd` (7) fires when a crashed agent is
destroyed; the script handler is commented out. Crashed agents stop at 4 cells/s² and stay until the owner removes
them. The citizen side skips crashed cars (`0x100cc990.c:100`); the exact removal path is in citizens.md.

---

## 7. Characters (`gsGridCharacters`, tick `FUN_100ca490`, this = characters+0xcc)

```
charactersTick(dt):
  if spawnPoints (set at characters+0x114) not empty:                         :65-90
      sp = first; t = sp.template
      if t+0x264 == 0: bird at (4·sp.x, sp.y + 0x600 + (rand() & 0xfff), 4·sp.z), pushed to birds (+0xf4)
      elif t+0x264 == 1: animal at (4·sp.x+1, sp.y, 4·sp.z+1), pushed to groundAnimals (+0x104)
      removeItem(sp)                   // each spawn-point item spawns at most one animal
  birdAcc += birds.size · dt; per 0.25: next bird (cursor down, wrap):        :92-171
      if |target − pos|² < 64 (xz): target.x += rand()%5 − 2 + trunc(15·dir.x/scale) + sign(anchorX − x>>5); same for z
                                     target.y = ground + 0x600 + (rand() & 0x7ff)
  animalAcc += animals.size · dt; per 10.0: next animal: if its path is finished,       :172-183, 258-330
      write 64 random 3-bit direction steps over walk-code-3 sub-cells (turn ±1 on a blocked step, stop on a second one)
  tickCounter = (tickCounter + 1) & 0xffff
  N, acc, k as for vehicles (acc += N·dt·5)                                   :188-201
  for each bucket i: if bucket.eyeDist < 310: update chars with dt                    :230-245
                     elif i in round-robin window: update chars with 0.2              :210-229
                     after each: moveToBucket(char)    // FUN_100c8080
  tickChildren(dt)
```
*obs.* Pedestrian update (`FUN_100c9470`), positions on the quarter-cell walk grid:
* **Following the path.** While the walk target is within 1 unit (`|target − pos|² < 1`), the pedestrian pops the next
  walk step. First it reserves the next sub-cell in the walk layer's 2-bit occupancy map (`FUN_100c71d0`). When it is
  close (`d² < 0.04`) and the next sub-cell is taken, it reserves the first free one further along the path instead.
  If none is free, it waits (`:71-104`).
* **Crossings.** A pedestrian that obeys crossings (flag 1) waits at a road-crossing sub-cell (walk code 1) while any
  road node within 1.5 units of it has an occupant (`0x100c71d0.c:73-95`). When its target becomes a crossing cell,
  it raises `pedCount` on every road node within 1.5 units. It lowers the count when it moves on (`:72-101`,
  `:137-159`). That count is what slows vehicles (§6.4). Vehicles do not stop for pedestrians; pedestrians wait for
  vehicles.
* **Walking** (`:191-220`):
  * `vDes = dir · speed·(|Δ| − 0.1)/0.9`, where `speed = model.speed`.
  * `vel = lerp(vel, vDes, 4·dt)`.
  * The velocity is zeroed when the heading error exceeds π/2.
  * `pos += vel·dt`.
  * The heading turns at most 2 rad/s (`:252-266`).
* **Idle** (`d² ≤ 0.02`). A voice line plays with probability `dt/5` per update when in sound range (`:178-190`).
* **Sitting states.** Flags 0x4, 0x8 and 0x10 play the "sitdown"/"getup" animations (`:222-251`).

---

## 8. Citizens tick (interface only; citizens.md owns it)

`FUN_100d40e0` (this = citizens+0xcc) does two things:
* It first drains the seven per-class **new-home-slot** vectors (`citizens+0x71c + 16·class`): each pending slot
  creates one citizen (`FUN_100d2ac0`).
* It then updates citizens round robin: `acc += count·dt; k = min(count, trunc(acc))`. Each citizen gets
  `FUN_100d3ca0` about once per scaled second (`:12-45`).

*obs.* This is where city growth becomes population (§9.4).

---

## 9. City timeline (`gsGridTimeline`)

### 9.1 Storage

`+0xd0` dayCount, `+0xd8` array of per-day action lists (newest first), `+0xdc` `currentDay` (days since 1920-01-01),
`+0xcc` grid. File layout, action classes, `ObjRef` matching and apply order are in
[`../save-format/lines-timeline.md`](../save-format/lines-timeline.md) §1.2, §2.6–2.12 and
[`../handlers/batch-A.md`](../handlers/batch-A.md) §applyChanges. Not repeated here.

### 9.2 Driving it

* `onDay` runs `applyChanges(currentDay+1, today−1920-01-01, 182, true)` and then sets `currentDay` (`game.script:671-673`).
  The same call runs at level load (`level.script:37-38`) and in the editor from day 0 (`editor.script:287`).
* `applyChanges(from, to, look)`: for each day `d`, it first applies day `d+look` in **preview** mode and then day `d`
  for real. On `d == 1` the preview covers days 1..1+look (batch-A).
* `checkDay(day)` returns true if `day == 0` or the day has actions. `checkDay(from, to)` returns true if `from == 0`
  or any day in `[from, min(to, dayCount−1)]` has actions (`0x10102230.c:16-41`) *obs*. Only the timebar UI uses it.

### 9.3 What the actions do to the city

| Action | Real (mode 0) | Preview (mode 1) | Evidence |
|---|---|---|---|
| createObject | `FUN_100c4380` → `addObject(template, x,y,z, orient, flags, …, connect=1)`. Types 0, 0x13, 0x15–0x17: removed again if they collide (`FUN_100bf420`). Network and buildings (1–0x12, 0x14): every overlapping item within ±1 bucket that is not under the same root is removed. If it carries 0x10000000, grid event 10 `forcedDemolish(controller)` fires first. Items whose controller has bit 0 of `+8` set are kept. Fires `onCreateInstance` (12). | Created with flags `|0x500000`; non-network types are removed at once. Overlapping items not already flagged 0x20200000 get 0x10000000, and their `sizeX·sizeZ`-weighted centres add to the returned "zoning conflict" position. Covering buildings paint cell flag 0x40. Fires `onConstructInstance` (11). | `0x10105a50.c:102-136,165-217,218-330,340-362,368-392` *obs* |
| deleteObject | `removeItem` (matched on template, x, z, orient) | sets item flag 0x20000000 | `0x10103b30.c:46-63` *obs* |
| brush / water / name / variation | terrain paint and height, water cells, rename, variation | see save doc | save doc |

When the real object arrives 182 days after its preview, the preview copy is an exact duplicate (same template,
x, z, orient, y). The overlap scan removes it (*inf* from `0x10105a50.c:252-260`).

### 9.4 Growth chain: buildings → capacities → citizens

```
addObject (FUN_100c4190):                                                 0x100c4190.c:26-75
  bucket from footprint centre; outside the grid → detached item (flag 0x200), nothing else
  item = new item in bucket; serial from grid+0x244 unless preview-only
  if connect and real and (flags & 0x200004) == 4 or item has nodes, and type != 0x15: graph.attach(item)   // FUN_100c1570
  if (flags & 0x700000) == 0:                    // real, not preview
      citizens.registerBuilding(item)            // FUN_100d2d90
      if type == 0x16: characters.spawnPoints.insert(item)
  if !(flags & 0x500000): template event 12 (onCreateInstance)
registerBuilding (FUN_100d2d90), only if the footprint is inside the grid:
  for class c in 0..6:
      homes[c] (+0x2e8): push item max(1, round(homes[c]·citizens.homeMultiplier)) times onto newHomeSlots[c] (citizens+0x718)
      work[c]  (+0x304): push item max(1, round(work[c]·citizens.workplaceMultiplier)) times onto workSlots[c] (citizens+0x788)
  for visit type t in 0..12: push item visits[t] (+0x33c) times onto visitSlots[t] (citizens+0x7f8)
```
*obs* (rounding = `fadd 0.5` before `__ftol`, bytes@0x100d2e17/0x100d2ea8). `removeItem` (`FUN_100bff60`) undoes this
(`FUN_100d2fb0`), removes stops from lines (flag 0x2000) and drops spawn points (`0x100bff60.c:55-104`) *obs*.
Combined with §8: each new home slot spawns one citizen on the next citizens tick, so **population follows the
timeline's buildings** (*inf*; the slot-pop inside `FUN_100d2ac0` is citizens.md's). The era mask
`citizens.modelMask` (`game.script:381-387`) selects which car, fire-engine and emergency models can appear.

---

## 10. Script-visible switches (summary)

| Field | Effect in native code |
|---|---|
| `grid.timeSpeed` | multiplies all grid time; 0 freezes movers, the building pass and fades |
| `grid.forceInterval` | fixed frame dt (video capture) |
| `grid.startFire` | next bucket visit ignites its first eligible building; self-clearing |
| `grid.disableEvents` | no fires, no reckless drivers (set during pre-simulation) |
| `grid.ignoreCitizens` | load-time only: skip vehicles/characters/citizens sections |
| `grid.effectConditions` | bit mask required by particle emitters (new-year effects) |
| `grid.showMarkers` | shows markers/spawn points and suppresses fires |
| `vehicles/characters/citizens.enabled` | the module is skipped by `FUN_100a0670` |

## 11. Determinism notes for a reimplementation

* Everything draws from the single MSVC `rand()` stream. Per frame, in order:
  * the bucket loop of every sub-step: one roll per AI agent update (`:73`), horn rolls, and character rolls;
  * citizen updates;
  * then 2 rolls per building-pass visit, plus the fire rolls.

  Bit-exact replays need this exact order and the same bucket and list orders. Lists are LIFO: spawns prepend, and
  loads reverse the order (save doc).
* The building pass and the round-robin windows depend on frame `dt` (truncation), so results differ with frame rate.
* `__ftol` truncates toward zero everywhere above.

## 12. Quirks

* **Q1** The eye-distance height sample indexes the terrain with `((2bz+1) << (2·shift−1)) + ((2bx+1) << (shift−1))`.
  That is row `cz·2^(shift−1)`, not `cz·width`, so for real map widths it reads an unrelated cell
  (`0x100c48c0.c:167-171`) *obs*. The effect is a small error in `eyeDist`.
* **Q2** The building-pass count has no remainder accumulator (§4.1).
* **Q3** The platoon check indexes the leader's ring modulo the follower's ring capacity (`0x1010de50.c:210`) *obs*.
  This is wrong when the two capacities differ.
* **Q4** `checkDay`/`applyChanges` do not range-check `from` (batch-A).

## 13. Open questions

1. The scene scale `S` (`scene+0x13c`, and the object at `grid+0x40`): its value and writer. It scales the
   activation radius and all LOD distances.
2. The meaning of node flags 0x20000000 and 0x400000, and which graph builder sets them (pathfinding.md). (Not at run
   time by level crossings: its only writer is `gsGridPlayer::createTrackBlock`, §5.1.1.)
3. How crashed agents (0x40000) are finally removed, and the citizen-side rule (citizens.md).
4. The aircraft spawners `FUN_1010b430`/`FUN_1010c190`: the approach/departure geometry built from inline air-lane
   nodes, and the state at vehicles+0x180…0x190. The boat spawner `FUN_1010ce70`, and the train route choice in
   `FUN_1010ae20` beyond the entry/exit pick.
5. The writer of `item.word` bits 12–15 (data view 0x80000), and the consumers of the area raster `grid+0x1e8`.
6. The bird target step uses a normalised direction from `FUN_10085050`; its exact semantics (normalise vs. clamp)
   are unconfirmed.
7. Whether `FUN_100d2ac0` consumes exactly one new-home slot per call (citizens.md) and how a removed building's
   pending slots are cleaned up.
8. Speed units: `speed` is treated as cells/s via the hardcoded 3 × 65536. Does any map use a cellSize other than 3?
   All samples have 3.0.
