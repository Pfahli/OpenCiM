# Particles: `gsParticleEmitter`, `gsParticleEngine`, the grid's effects and `gsParticleSystem_OGL`

How the engine emits, moves and draws particles: the script classes and their fields, emission (rate, on/off cycles,
cone, ring), the engine's physics, how the grid runs the emitters of items and vehicles (conditions, distance), and
the billboards the renderer draws (sorting, blending, lighting, fog, shadows). The port's implementation is
`port/src/engine/particles.cpp` (objects, natives, emission, physics, collection) and
`port/src/render/renderer3d.cpp` `Renderer3D::drawParticles`.

Evidence: `L:` = Linux build pseudocode (`decompiled/Linux/functions/<addr>.c`, real names), `W:` = Windows
(`decompiled/GSystem-v2/functions/`), `S:` = shipped scripts. *obs* = read in the code, *inf* = inferred.
Field offsets: Linux first, Windows (`docs/native-api.md`) in brackets.

---

## 0. Summary

* **Three script classes.** `gsParticleEmitter` (`new_particleEmitter(parent)`, 179 uses) holds the parameters and
  emits; `gsEmittingParticleEmitter` (2 uses: fire sparks, city-hall fireworks) gives each of its particles its
  child emitters, which then stream from the moving particle or burst where it dies; `gsParticleEngine`
  (`engine.script:5`: `$scene.particleEngine = new_particleEngine($scene)`) owns and moves every particle of a scene.
  `gsBoatContrain` derives from the emitter but builds a wake ribbon on the water instead of particles (§7).
* **Particles live in the scene's engine**, not in the emitter: `emitParticle` returns nothing when the emitter's scene
  (`+0x98`) has no `particleEngine` (`+0x108` [W `+0x140`]). So emitters in scenes without an engine (vehicle shop
  previews, the menu) never emit.
* **Grid templates are emitter templates.** Vehicle and building scripts create emitters under their `gsGridObject`
  template (`new_particleEmitter(this)`); each item or vehicle instance keeps one timer per emitter
  (`gsGridObjectEffect`, `createInstanceData`) and calls `instanceMove(dt, timer, pos, orient, velocity,
  conditions)`. An emitter emits for an instance only if `(emitter.conditions & conditions) == emitter.conditions`.
* **Drawing:** one `gsParticleSystem_OGL` per emitter (material `shaders/particles/particles.shader`), CPU billboards
  facing the camera, each particle sent to the sorted blend queue; non-additive particles also go into the shadow
  map.

---

## 1. Script API

### 1.1 `gsParticleEmitter` (ctor `L:0x0822daf0`, fields `L:0x0822b140`, `W:0x1005e550`)

| Field | L [W] | Type | Default | Meaning |
|---|---|---|---|---|
| `active` | 0x180 [0x1c4] | bool | true | stored (`setActive L:0x081753d0`); not read by the paths below *obs* |
| `autoemit` | 0x181 [0x1c5] | bool | false | the timer emits (§2.2); off: only `instantEmit` / bursts |
| `planeSize` | 0x120 [0x158] | float | 0 | radius of the ring particles start on (§2.1) |
| `particleSize` | 0x124 [0x15c] | float | 1 | half extent of the billboard |
| `particleAirResist` | 0x154 [0x18c] | float | 0.1 | drag towards the wind (§3) |
| `density` | 0x158 [0x190] | float | 20 | particles per second (+ density terms) |
| `particleBlend` | 0x194 [0x1dc] | int | 1 | `BLEND_ALPHA` 1, `BLEND_ADDITIVE` 2, `BLEND_ADDITIVE_ALPHA` 4 (§4.1) |
| `sortParticles` | 0x198 [0x1e0] | bool | false | stored; the system is always created sorted (flags 0xc0) *obs* |
| `ownerVelocity` | 0x1a8 [0x1f4] | float | 1 | factor on the owner's velocity added to each particle |
| `maxDistance` | 0x1ac [0x1f8] | float | 0 | emission range for grid instances; draw fade/cut (`write_maxDistance` → `system.setMaxDistance`) |
| `gravitation` | 0x114 [0x14c] | vector | (0, −9.81, 0) | constant acceleration |
| `direction` | 0x108 [0x140] | vector | +Y | normalised on write (`setDirection L:0x08175440`) |
| `min/maxParticleSpeed` | 0x134/0x138 [0x16c/0x170] | float | 3 / 10 | |
| `min/maxParticleAngle` | 0x13c/0x140 [0x174/0x178] | float | 0 / 0.5 | cone half angle range (rad) |
| `min/maxParticleOrient` | 0x144/0x148 [0x17c/0x180] | float | 0 / 2π | billboard rotation |
| `min/maxParticleLife` | 0x14c/0x150 [0x184/0x188] | float | 5 / 7 | seconds |
| `min/maxOnTime`, `min/maxOffTime` | 0x1b4…0x1c0 [0x200…0x20c] | float | −1 | on/off cycles; `minOnTime = −1` = continuous |
| `conditions` | 0x1c4 [0x210] | uint | 0 | required condition bits (§5) |
| (inherited) `position`, `orientation`, `globalPos`, `visible` | gs3DElement | | | `visible` = the system's visibility |

Internal: emission timer `+0x190` (L), density / speed / alpha terms (vectors of `{float weight, gsVariant*}` at
`+0x15c`, `+0x168`, `+0x174`), colour ranges `+0x184`, emit positions `+0x19c`, system `+0x104`, material `+0x1b0`,
scale `+0x128..+0x130` (growth 0, start 0, end 1000).

| Method | L | Behaviour |
|---|---|---|
| `addColorRange(start, end)` | 0x0822bea0 | ARGB pair; one range is picked per particle at random; none = white → white |
| `addEmitPosition(v, [relative = true])` | 0x0822c090 | extra emit points (§2.3); 4 uses (an add-on robot) |
| `setParticleScale(growth, start, end)` | 0x0822ab00 | size over age (§3) |
| `addDensityTerm / addSpeedTerm / addAlphaTerm(weight, variable)` | 0x0822c440 / c300 / c3a0 | `weight × value(variable)` added to the density, to each particle's speed, or multiplying both colours' alpha; 0 shipped uses |
| `instantEmit([count = 1])` | 0x0822c720 | emits `count` particles now (§2.3) |
| `setTexture(path)` | 0x0822df60 | the raw path to the texture cache (not resolved against the script directory); `updateMaterial` |

`updateMaterial` (`L:0x0822dd50`): creates the `gsMaterialVariant` with `shaders/particles/particles.shader` once, sets
flags `0x4200` (DIFFUSEMAP | POINT), `0x4280` for blend 2, `0x4300` for blend 4, and gives it to the system.
`createSystem` (`L:0x0822dee0`): `video.createParticleSystem(0xc0, parent scene item)` (0x80 sorted, 0x40 two-sided),
`setMaxDistance(maxDistance)`.

### 1.2 `gsParticleEngine` (ctor `L:0x0822cd10`)

Fields `timeSpeed` (`+0x10c`, default 1) and `windVelocity` (`+0x110`). The game keeps
`timeSpeed = max(1, game speed)` (`S: minimap.script:175`), so particles keep moving at speed 0, and sets the wind to
(1.5, 0, −1) (`S: engine.script:6`). Particle list head `+0x104`, count `+0x108`.

### 1.3 Constants

`GRIDVEHICLE_HELICOPTER_DUST` 1, `GRIDVEHICLE_BROKEN` 0x10000, `GRIDVEHICLE_CRASHED` 0x40000
(`gsGridVehicles_init L:0x081af8d0`). Shipped emitters (`S: metro`, 87 emitters): 34 `BROKEN` (every bus, tram,
metro, ferry and helicopter), 2 `CRASHED` (`environment.script:111,138`: added to every traffic and player vehicle
model), 4 `HELICOPTER_DUST` (two per helicopter), 1 `conditions = 1` (city-hall fireworks, the new-year bit of
`grid.effectConditions`), the rest unconditional (factory and ship smoke, fountains, fire, chimneys).

---

## 2. Emission

### 2.1 One particle — `gsParticleEmitter::emitParticle(inst, pos, orient, ownerVel)` (`L:0x0822d2c0`, `W:0x1005c3f0`)

```
engine = emitter.scene.particleEngine; if none: return null
p = new gsParticle (0x60 B)
p.pos   = pos
speed   = gsRandom(minSpeed, maxSpeed) + Σ speedTerms
tilt    = gsRandom(minAngle, maxAngle);  spin = rand()/RAND_MAX · 2π
axis    = gsAnyCrossingVector(direction)                   // some perpendicular
d       = orient · (Q(spin, direction) · Q(tilt, axis) · direction)
if planeSize != 0:                                         // a ring around the particle's own direction
    a = rand()/RAND_MAX · 2π;  u = gsPerpendicular(d);  w = u × d
    p.pos += planeSize · (cos a · u + sin a · w)
p.vel   = speed · d + ownerVelocity · ownerVel
p.life  = gsRandom(minLife, maxLife); p.age = 0; p.orient = gsRandom(minOrient, maxOrient)
p.colors = colorRanges[rand() % n] or (white, white); with alpha terms both alphas × Σ alphaTerms
p.airResist, p.gravity, p.size = particleSize, p.scale = (growth, start, end)
p.handle = emitter.system.createParticle(); push p at the engine's list head; moveParticle(p, 0)
```
`gsRandom(a, b) = a + (b − a)·rand()·2⁻³¹` (`L:0x08108f80`). Every particle draws from the C library `rand()`, the
same stream the simulation uses *obs*.

### 2.2 The timer — `move(dt)` (`L:0x0822c770`) and `instanceMove(dt, timer, …)` (`L:0x0822e040`, `W:0x1005d0a0`)

```
rate = density + Σ densityTerms
if minOnTime == −1:                                        // continuous
    if autoemit and rate > 0:
        timer += dt; while timer ≥ 1/rate: timer −= 1/rate; emit 1
else:                                                      // on/off cycles, timer > 0 on, < 0 off
    if timer == 0: timer = gsRandom(−maxOffTime, maxOnTime)          // a random phase to start
    elif timer < 0: timer += dt; if timer ≥ 0: timer = gsRandom(minOnTime, maxOnTime)
    elif autoemit and rate > 0:
        emit trunc(timer·rate) − trunc((timer − dt)·rate); timer −= dt
        if timer ≤ 0: timer = −gsRandom(minOffTime, maxOffTime)
```
`instanceMove` first checks `(conditions & cond) == conditions`, then runs the same timer on the instance's float.
`move` (the module tick) does not check conditions; it emits at the emitter's own place.

### 2.3 Where — `instantEmit`

Script emitter (`L:0x0822c4e0`, `W:0x1005c8c0`): for each emit position (one pass at the emitter when there are
none) `count` particles at: the emitter's global position; a non-relative position *as a world position*; a relative
one at `parentMatrix · (localPos + v)`. Orientation = the emitter's global orientation, owner velocity = the parent's
velocity at that point (zero for scenes).

Grid instance (`W:0x1005ca80`): the same with the instance placement `M = T(pos)·R(orient)`: `M · emitter.globalPos`
(the template sits at the origin, so this is its local position), a non-relative position as is, a relative one at
`M · (globalPos + v)`; orientation = the instance's, owner velocity = the instance's.

`gsEmittingParticleEmitter::emitParticle` (`L:0x0822d980`) emits like the base class and links every child emitter
(`addChild L:0x0822adb0` keeps a list of the child `gsParticleEmitter`s) into the new particle (`p+0x5c`, linked
through the child's `+0x1c8`), placing the children at the parent emitter's global position and orientation.

---

## 3. The engine — `gsParticleEngine::move` (`L:0x0822d180`) / `moveParticle` (`L:0x0822cd90`, `W:0x1005c020`)

```
move(dt): t = dt · timeSpeed; n = max(1, trunc(t/0.2 + 0.99)); repeat n times: for p in list: if moveParticle(p, t/n): unlink + destroy
moveParticle(p, dt):
    p.age += dt
    if p.age < p.life:
        p.pos += p.vel·dt
        p.vel += (p.gravity − airResist·(p.vel − wind))·dt
        colour = interpolate(p.colors, age/life)            // per byte, rounded (gsColorRange::interpolate L:0x080ee9c0)
        size   = age ≤ start ? s : s·(1 + (min(age, end) − start)·growth)
        system.setParticle(p.handle, p.pos, size, p.orient, colour)
        for child in p.children with autoemit:              // a trail: rate per second from the particle
            p.childTimer += dt; while p.childTimer > 1/rate: p.childTimer −= 1/rate; child.emitParticle(0, p.pos, child.globalOrient, p.vel)
        return false
    for child in p.children without autoemit:               // a burst where it dies
        repeat trunc(rate) times: child.emitParticle(0, p.pos, child.globalOrient, p.vel)
    return true
```
*obs* except the interpolation parameter `age/life` (*inf*: its argument is lost in both decompiles; colour ranges
like `0x80333333 → 0x00333333` fade out over life). The fire's sparks (`fire-medium.script`) use a streaming child
(`autoemit`), the fireworks (`cityhall.script`) a 200-particle burst child.

---

## 4. Drawing — `gsParticleSystem_OGL` (`L:0x08219360` render, `0x08218b60` start, `0x08218ee0` particle, `0x08219720` end)

Interfaces in [`interfaces-3d.md`](interfaces-3d.md) §2.9. Per frame and pass, per system:

* Drawn once per scene traversal (the scene it registered with). In the **shadow-map pass** (`ENVIROMENT_SHADOWMAP_RENDER`
  4) additive materials (0x180) are skipped, the others drawn at once with the alpha test on; in the normal pass the
  sorted system (flags 0x80, always) puts **each particle** into the blend queue (`video+0x208`, key = squared
  distance), drawn far to near with the other blended geometry.
* Culling per particle: `size² < d²·k`, `k = 0.25·f²`, `f = ctx+0x6c` (half-diagonal of the frustum at distance 1:
  `sqrt(t² + (t·aspect)²)`, `t = tan(fov/2)`), i.e. particles larger than half the view are dropped; plus a test
  against the view direction (`gsDistanceFromSegmentSideSq`, *inf*: in front of the camera).
* Alpha (`renderParticle`): without max distance `a = min(1, k·d²/size² − 1)` (fades as it grows towards half the
  view); with one, `d ≥ maxDistance` is not drawn and `a = min(a, (maxDistance − d)/maxDistance · 10)` (the last
  tenth fades). The colour's alpha is multiplied by `a` (`gsColor::multAlpha`).
* Billboard: `c = cos(orient)·size`, `s = sin(orient)·size`, `A = c·right + s·up`, `B = c·up − s·right` (camera axes,
  `ctx+0x54/+0x60`), corners `p+A+B (1,0)`, `p−A+B (0,0)`, `p−A−B (0,1)`, `p+A−B (1,1)`, normal +Y, `GL_QUADS`.
* State (`startRendering`): blend `ONE, ONE` for 0x80, `SRC_ALPHA, ONE` for 0x100, else the queue's
  `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`; cull face off (0x40); material colours: blended ambient = diffuse = 0.5 grey,
  emission 0; additive ambient = diffuse = 0, emission 1. Shader variant: material flags, environment = pass flags +
  light count << 12 for blended particles (0 for additive ones), so blended smoke is lit (ambient + diffuse, no N·L)
  and receives sun shadows (`ENVIROMENT_SHADOWMAP`: four shadow taps in the vertex shader); every particle gets fog
  (`particles.fragment`: `mix(colour, fog, (length − start)·scale)`).
* `particles.fragment` in the shadow-map pass pushes the depth of texels with `a ≤ 0.2` to the far plane, so thick
  smoke and dust cast shadows.
* Bloom: particles are part of the scene colour the bloom chain reads; nothing particle-specific.

---

## 5. The grid's emitters

### 5.1 Items — `gsGridItem::move` (`L:0x08182b10.c:84-91`, `W:FUN_100eb5a0`)

Active buckets, every sub-step, items visible in the view mode (grid-traffic.md §5.1): each emitter of the template
runs `instanceMove` while `bucket.eyeDist < emitter.maxDistance`, at the item centre
(`((2x + sx − gridW)·cs/2, y·cs/256, (2z + sz − gridL)·cs/2)`), turned `(orient + 2)·π/2` about −Y, velocity 0,
with `grid.effectConditions` (`+0x23c` [W `+0x250`]; `game.script:639-643` sets bit 1 for 15 s at every new year).

### 5.2 Vehicles — `gsGridVehicle::move` (`L:0x081adca0.c:606-652, 790-808`)

Each agent of the chain (head and trailers) runs its model's emitters while `bucket.eyeDist < maxDistance`, at its
pose, velocity `orient·(0, 0, −3·speed)`, with the conditions

```
cond = agent.flags & 0xffff0000                 // 0x10000 broken down and halted, 0x40000 crashing
if the model has animated parts (rotor):
    r = spin (§ boats-aircraft.md 6.2)
    if r > 1 (moving) or r > 5 (halted, broken, crashing: flags 0x50400):
        if curNode is a STOP (0x40)
           or (nextNode is a STOP and posOnNode < rearOffset)
           or (the previous path node is a STOP and curNode.length − posOnNode < frontOffset + rearOffset):
            cond |= 1                           // GRIDVEHICLE_HELICOPTER_DUST
```
So the dust shows while a helicopter's body overlaps its pad node with the rotor turning: on landing, at the pad while
the rotor runs down, and at take-off.

---

## 6. Port (`engine/particles.cpp`, `render/renderer3d.cpp`)

* Objects: `gsParticleEmitter` (and its subclasses) and `gsParticleEngine` get native objects with the fields above
  as C++ members (get/set hooks), `position`/`globalPos` relative to the parent; the methods are implemented.
* Engine: a pool of particles per engine, **capped at 24 000** (emission stops at the cap and is counted); systems
  are the emitters, kept by reference, their particles dropped when the emitter goes away.
* Grid: `simParticlesTick` after each grid tick (scaled dt): per mover unit and per item with emitters, one timer per
  emitter; the camera distance to the vehicle or item centre stands in for the bucket's eyeDist; vehicles are first
  rejected with one distance check against the largest range of their models (as the world sounds do). The
  underground view (viewMode 0x180) runs only vehicles on tunnel nodes and no item effects.
* Drawing: per viewport the visible particles are culled (behind the camera, size test, frustum with a margin,
  maxDistance), faded, sorted far to near and merged into batches by (texture, blend); `drawParticles` builds the
  quads for the pass's camera into one stream buffer and draws runs of consecutive sprites of one batch with the
  combiner's `particles.shader` variants. They are drawn **after** the blended queue instead of interleaved with it
  (a smoke particle behind a blended window or water is drawn over it). Shadow-map pass: blended particles with
  depth writes and alpha test 0.2.
* The pass and material uniforms are set once per program and pass; a run then only switches program, blend func
  and texture. Cost (RTX 3080, 1600×900, 4× MSAA, shadows 2048², vsync off, Vienna): the start view 7.4–7.5 ms per
  frame with particles vs 6.2–8.8 ms without (within the noise); a zoomed-in factory with a test fire and its
  chimney (≈ 2 100 live, 1 900 drawn per frame) 8.1–8.7 ms vs 6.5–8.7 ms; simulation tick 0.15–0.2 ms, collection
  0.35 ms. Drawing each run through the generic material path cost 12.8–21 ms in the same scene.
* Random numbers: the port's own stream (`std::minstd_rand`), not `rand()`, so particles do not shift the
  simulation's sequence.
* Switches: `REGS_NO_PARTICLES=1` (no emission, nothing drawn: for frame-time comparisons), `REGS_DEBUG_PARTICLES=1`
  (items with emitters, live/drawn counts and CPU times every 300 frames; `=2` draws every particle opaque magenta at
  three times its size), `REGS_TEST_BREAKDOWN=<s>` (the vehicle `REGS_DEBUG_FOLLOW` follows breaks down after that
  much simulated time), `REGS_TEST_FIRE=<item>` (a `fire-medium` placed at that item after 60 ticks),
  `REGS_TEST_FIREWORKS=1` (keeps the new-year bit of `grid.effectConditions` on).
* Port fields for mods (`port/docs/mod-manager.md` §4.2): `regsEmitBox` (vector: particles start at a random point
  of a box of these half extents around the emit point) and `regsStretch` (seconds: the particle is drawn as a streak
  along its velocity, half length speed × stretch plus `particleSize`, `particleSize` wide, facing the camera about
  the motion axis; culled with its length). Unset, emitters behave as in the original. Used by the weather mod's rain.
* Port change (user's request): helicopter line vehicles board with a still rotor and spin up on the pad before they
  take off; the dust follows the rotor with the original's thresholds, so there is no dust while boarding
  ([`../simulation/boats-aircraft.md`](../simulation/boats-aircraft.md) §6.3).

## 7. Open

* `gsBoatContrain` (`L:0x081748b0` emitParticle): a 16-point ring of positions per boat turned into a quad strip
  (`planeSize` wide, alpha by age) that is inserted as a mesh into the grid bucket under the boat, with the script's
  `metro/shaders/water/contrain.shader` material. Not ported: the port creates the object but emits nothing.
* Fires as a simulation event (grid-traffic.md §4.4, the building pass) are not ported; `REGS_TEST_FIRE` places one.
* `sortParticles` and `active` are stored but have no effect, as in the code read.
