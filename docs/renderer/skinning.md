# Skeletal animation and skinning: `gsSkeleton`, animation slots, the grid's animated objects

How the engine animates skeletons and skins meshes: the script objects and their animation slots, the per-frame pose,
the rules the grid applies to animated items, vehicles and characters, and the GPU skinning. The file formats are in
[`../asset-format/skeleton-animation-scene.md`](../asset-format/skeleton-animation-scene.md) (`.skeleton`,
`.animation`, sampling and blending conventions §1.2) and
[`../asset-format/mesh-material.md`](../asset-format/mesh-material.md) §2.5 (skin chunk). The renderer side (bone
groups, `gsSkeleton_OGL`) is in [`interfaces-3d.md`](interfaces-3d.md) §1.5 and §2.5.

Evidence: `L:` = Linux build pseudocode (`decompiled/Linux/functions/<addr>.c`, real names), `W:` = Windows
(`decompiled/GSystem-v2/functions/`), `S:` = shipped scripts. *obs* = read in the code, *inf* = inferred.

---

## 0. Summary

* **Two skeletons per animated model.** The script-side `gsSkeleton` (a `gs3DElement`, created by
  `gsObject.loadSkeleton`) holds the bones and the named animation slots. Its renderer twin `gsSkeleton_OGL`
  (`gsSkeleton+0x108`) holds the bind pose and the bone palette. Grid instances (items, vehicles, characters) share the
  template's `gsSkeleton` and get their own `gsSkeleton_OGL` **clone** for their mesh copy (`clone`, vtbl `+0x20`).
* **A slot is a loaded take** (`gsSkelAnim`, a `std::map<std::string, gsSkelAnim>` at `+0x124`, so the slots are in
  name order). `playAnimation` only sets its speed and loop flag. A slot's **blend weight starts at 0**, so
  `playAnimation` alone does not show anything. The weight comes from the native owner: the grid sets it.
* **Pose:** `gsSkeleton::animateBones` samples, per bone, the slots in load order. The first enabled slot with a
  weight and a track for the bone sets the local TRS, and later ones blend towards theirs. Bones without a track keep
  their current pose (the bind pose). The result goes to `gsSkeleton_OGL::setBoneLocal`; skin = `invBind · world`.
* **Skinning is always on the GPU.** The combiner variant has `MESH_SKINNED`, and `default.shader` adds
  `WEIGHTx4|INDEXx4` + `bones[48]` with `$indexingInShaders`, else `WEIGHTx8` + `bones[8]`.

---

## 1. Script objects (obs)

| Native | Effect | Evidence |
|---|---|---|
| `gsObject.loadSkeleton(file)` | loads `<object dir>/<file>`, makes the `gsSkeleton` (child of the object, field `skeleton`) and its `gsSkeleton_OGL` | `L:0x08113d10`, `L:0x0811bb90` |
| `gsSkeleton.loadAnimation(name, file, [flags], [group names…])` | slot `name` = take from `<skeleton dir>/<file>` (`gsAnimations` cache); tracks attach to the first bone of their name (and in the groups' bone mask, if given) | `L:0x080e3f30`, `L:0x080df020` |
| `cropAnimation(name, start, length)` | `start = clamp(start, 0, duration)`, `length = clamp(length, 0, duration − start)`, position clamped to `[0, length]`; clamp(x, 0, m) = x if 0 < x < m, m if x ≥ m, else 0. **The third argument is a length.** | `L:0x080dce80`, `L:0x080df820` |
| `playAnimation(name, speed, [loop = true], [speed source])` | sets speed (`+0x1c`) and loop (`+0x5c`), replaces the speed source | `L:0x080dd1b0`, `W:0x1006a200` |
| `blendAnimation` / `setAnimationBlend` | blend weight, optionally as a timed fade with an ease curve (`moveAnimations`) | `L:0x080dbd00`, `W:0x1006a270` |
| `stop/rewind/enable/disable/unloadAnimation`, `setAnimationPos` | slot flags / position | `L:0x080dd030…0x080db800` |

Slot defaults (`gsSkelAnim`, `L:0x080e3f30.c:156-185`, `W:0x100707b0.c:63-93`):

| Offset | Field | Default |
|---|---|---|
| +0x00 | enabled | 1 |
| +0x04 | take (`+0xc` duration) | — |
| +0x08 | flags | arg 3 (`0x10000` additive, `0x20000` DONT_LOOP, `0x40000` replace) |
| +0x0c | crop start | 0 |
| +0x10 | length | take duration |
| +0x14 | blend weight | **0** |
| +0x18 | effective weight (blend × weight source) | 0 |
| +0x1c / +0x20 | speed / speed factor | 1 / 1 |
| +0x40 | position (s) | 0 |
| +0x44…+0x58 | fade from, to, rate, curve exponents, time | 0, 0, 0, 1, 1, 0 |
| +0x5c | loop | `!(flags & 0x20000)` |

Scripts (`S:metro/objects/characters/*/…script`, `S:…/amusement-park/*.script`): characters load `walk`, `idle`, and
for some `sitting`, `sitdown`, `getup` from one take and crop them in frames/25; `walk` is played at
`1 / this.speed`. Rides load `default` and set `this.doorOpenStart = 0; this.doorOpenLen = <loop length>`.

---

## 2. Per-frame pose (obs)

`gsSkeleton::move(dt)` (`L:0x080e1370`) = `moveStates` (state machine, unused by the base game), `moveAnimations`,
`animateBones(dt, own gsSkeleton_OGL)`. It runs for skeletons in a scene. The grid's instances call `animateBones`
directly with the instance's clone (§3).

`moveAnimations` (`L:0x080e0280`): per slot, `pos += speed · factor · dt` (or the sync source's position). With loop
the position wraps into `[0, length)`; without loop it clamps at `length`. A fade with rate ≠ 0 moves `blend` along an
ease curve (`pow` exponents `+0x50/+0x54`), and the effective weight is `blend × weight source`.

`animateBones(dt, target)` (`L:0x080e06d0`):

```
target.setRootTransform(0, identity)                 // bones in model space; the mesh world places the instance
for bone in bones (file order):
  first = true
  for entry in bone.tracks (attach order = loadAnimation order):
    slot = entry.slot;  w = slot.weight · entry.groupWeight
    if !slot.enabled or w == 0: continue
    (t, s, q) = sample(entry.track, time = slot.cropStart + slot.pos, clamp = flags & DONT_LOOP)   // gsTrack::getTransforms
    if first:   bone.(t, s, q) = sampled; first = false
    elif flags & 0x10000 and base track: additive delta against the base track
    else:       bone.t = lerp(bone.t, t, w); bone.s = lerp(bone.s, s, w); bone.q = interpolate(bone.q, q, w)
  noise (noiseAmount ≠ 0): random walk on q, only for groups in the noise mask
  target.setBoneLocal(i, t, q, s)
  attached elements (scene nodes named like the bone) follow the bone's world matrix
```

`gsTrack::getTransforms` (`L:0x080de180`) is the sampler of asset-format §1.2: `f = (n−1)·time/duration`, keys
`trunc(f)` and `trunc(f + 0.99999)`, lerp t/s, nlerp q. An index past the end is the last key when clamped, else
key 0. No keys gives identity.

---

## 3. The grid's animated objects (obs)

All three clone the template's `gsSkeleton_OGL` for the instance on first use. Beyond the distance limit or with
time −1 they drop the clone, so the mesh draws in bind pose.

### 3.1 Items (`gsGridItem::move` `L:0x08182b10`, `gsGridItem::animate` `L:0x08181ff0`)

* Template fields (Linux `+0x334…+0x340` = Windows `+0x398…+0x3a4`): `doorOpenStart`, `doorOpenLen`,
  `doorCloseStart`, `doorCloseLen`.
* `move`: nothing to do when `doorOpenLen == 0` and the template has no `engineSound`/`moveSound` and the item no
  particles; hidden items (`0x400000`) never move.
* Without item flag `0x1000`: `phase += dt` and wraps at `doorOpenLen`, then `animate(phase + doorOpenStart)`.
  Items start at phase 0, so all items of a template run in step.
* With `0x1000` (door items) the phase runs 0…1 while a docked vehicle's door flag (`0x10`) is set and back otherwise.
  It is mapped to the open/close windows.
* Skipped at distance ≥ 300 for flag `0x4000000`.
* `animate(time)`: division distance ≥ 400 → drop the clone. Else **the first slot in name order** gets enabled = 1,
  effective weight = 1, position = `time · speed`. Then `animateBones(0, clone)`.

### 3.2 Vehicles (`gsGridVehicle::animate(time, factor)` `L:0x081ab5f0`)

Per unit of the chain (`+0x18`): distance ≥ 300 → drop. Else the first slot gets weight 1 and a position:
* factor = −1: the position is `time`, or `duration − time` for units flagged reversed (`+0x7c & 2`). Used for the
  doors.
* Else: `acc += time · factor · speed`, wrapped at the duration.

### 3.3 Characters (`gsGridCharacter::animate(name, dt, factor)` `L:0x081529d0`, `gsGridCharacterAnimations` `L:0x08151ea0-0x08151f60`)

* State at `+0x88`: name, current slot, previous slot, positions, fade, factors.
* Distance ≥ 300 → drop the clone.
* A new name:
  * back to the previous slot: swap the two and set fade = 1 − fade;
  * otherwise: current → previous, the new slot starts at position 0 with fade = 0.001;
  * an unknown name clears both.
* `move(dt, factor)`:
  * `fade += 2·dt`, so the cross-fade takes 0.5 s; at 1 the previous slot ends.
  * Each slot's `pos += factor · dt · slot.speed`.
  * factor ≥ 0 wraps once past the length (`pos − length` if that is below the length, else the length).
  * factor < 0 plays forward once and stops at the length.
  * Returns true at the wrap or end.
* Slot weights: current = fade, previous = 1 − fade, all others disabled. Then `animateBones(0, clone)`.
* Callers (`gsGridCharacter::move` `L:0x08153b90.c:235-299`):
  * `walk` with factor = |velocity| (m/s). With the script speed `1/this.speed` this keeps one stride per distance.
  * `idle` and `sitting` with 1.
  * `sitdown` and `getup` with −1.

---

## 4. Skinning (obs, interfaces-3d.md §1.5)

* Skin chunk: 4 influences {bone, weight} per vertex, renormalised. Bones are indices into the `.skeleton` file
  order.
* Submeshes are split into bone groups that fit the palette; vertices carry palette slots.
* Per group the draw uploads `bones[k] = invBind[palette[k]] · world[palette[k]]` (row-vector order). In
  column-vector terms that is `world · invBind`.
* `geometry.vertex` sums `bones[i]·gl_Vertex·w`; weights arrive as shorts ×32767 and the shader scales them by
  1/32767.
* The bind palette is the identity, so a skinned mesh without animation renders as stored.
* Conventions: bone local = `S·R(q)·T` row-vector, which is `T(t)·R(conj q)·S(s)` with column vectors (asset-format
  §1.2).
* Check: the port's `regs-tool skintest` on `white-collar-male01` gives a bind palette within 2.4·10⁻⁷ of identity,
  and the cropped walk keeps the feet at y = 0 with the stride swinging between ±0.35 and ±0.65 m.

---

## 5. Port (2026-11-04)

* `render/skeleton.*`: `.skeleton` and `.animation` loaders, the sampler, the bone and palette maths.
* `render/mesh.*`: the skin chunk.
* `render/renderer3d.*`: bone groups at upload (`SkinLayout`: 48 indexed or 8 non-indexed, from `$indexingInShaders`)
  and per-group `bones` uniforms.
* `engine/skinning.*`:
  * the `gsSkeleton` natives of §1;
  * `poseFirstAnimation` (§3.1, §3.2);
  * `animateCharacter` / `poseCharacter` (§3.3);
  * `collectSkinned`, which makes draw items with the skinning variant (`GpuMaterial::meshFlags`).
* `engine/grid_items.cpp`, `engine/terrain.cpp`:
  * skinned templates are drawn per item instead of merged (the original keeps them as instances);
  * they are posed per §3.1 on a grid clock (wall time × `grid.timeSpeed`), one pose per template per frame.

Not yet:
* door items (`0x1000`) stay in bind pose;
* additive (`0x10000`) and noise blending;
* timed blend fades;
* the state machine (`states`, `setState*`);
* scene objects' own `gsSkeleton::move` (no slot gets a weight there without a blend call);
* bones' attached scene nodes;
* vehicles and characters until the simulation draws them with `collectSkinned`.
