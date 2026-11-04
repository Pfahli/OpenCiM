# Picking and selection in the 3D view: `intersectGrid`, hover, click, controllers, corner preview

Scope: what happens between the mouse moving over the in-game 3D view and (a) the hover tooltip naming the object under
the cursor, (b) a left click opening that object's data in the corner HUD and pointing the corner preview camera at it.
That covers the screen → world segment, `gsGrid::intersectGrid` (terrain, items, vehicles, characters), the hover field
and event 17 `onHoverObject`, the click path to event 20 `onSelectObject`, the script controller objects the engine
hands out, and the script side (`hoverinfo.script`, `secondaryview.script`). Builds on, and does not repeat:

* [input.md](input.md): desktop hit test, per-tick `mouseMove` (§3.2), button dispatch (§3.10), event dispatch (§4.1).
* [game-load.md](game-load.md) §6: main viewport, `gsFloatingCamera`, `setCameraPos`, `getWorldPosition` use.
* [minimap.md](minimap.md) §6: map pins (`gsMapItemCtrl`, used for the selection arrow on the minimap).
* [../renderer/scene-basics.md](../renderer/scene-basics.md) §2 (camera, projection, view), §7 (viewport into a
  texture target and GUI composite). [../renderer/terrain.md](../renderer/terrain.md) §2.1 (cells, cover quadrants
  `0xf00`, `cellCover`). [../renderer/grid-items.md](../renderer/grid-items.md) §1.3-2.1 (template and item fields).
* [../script-runtime/semantics.md](../script-runtime/semantics.md) §4 (event binding and firing).
* [../save-format/vehicles-movement.md](../save-format/vehicles-movement.md) §2.3 (vehicle agent fields),
  [../simulation/transport.md](../simulation/transport.md) (player vehicles and stops).

**Citations.** `L:0x<addr>.c:<line>` = `decompiled/Linux/functions/` (real names), `W:0x<addr>` =
`decompiled/GSystem-v2/functions/`, `L:0x<fn>@<insn>` = instruction in the Ghidra listing of that Linux function (dumped
headless from project `CiMLinux` with `tools/ghidra/DumpAsm.java`, read-only), used where the x87 / struct-return
pseudocode lost operands or constants. `S:<path>:<line>` = `extracted/metro/<path>`. **obs** = read in code or listing,
**inf** = inferred. Confidence H/M/L where it matters. All float constants below were read from the Linux listing
(`FLD float ptr [0x…]` / immediate stores), not from Ghidra's x87 reconstruction; the Windows build has the same
constants (`W:0x100b8020.c:106-147`).

**Conventions.** Offsets are **Linux**. Script-visible field offsets in `native-api.md` are **Windows** (gsGridPlayer:
Linux = Windows − 0x68 up to `toolLine`, − 0x60 from `hoverObject` on; gsGridController family: Linux = Windows − 0x38 for
`forceSounds`, − 0x38/−0x50 for the subclass fields). `W×L` = grid size in cells (grid `+0x104/+0x108`), `cs` = cell
size (grid `+0x11c`, 3.0), heights in 1/256 cell. `trunc` = C cast to int (x87 `FISTP` with the control word's
rounding field set to 11 = toward zero, `MOV AH,0xc` before every `FISTP` in these functions). Vectors are row vectors
(`p' = p·M`), as in scene-basics.md §2.2.

---

## 0. Summary

1. Every tick the desktop calls the hovered control's `mouseMove`; over the 3D view that is `MAIN.$player`
   (`gsGridPlayer`, full-screen, `S:scripts/player.script:27-34`). In `editMode` 1 (`GRIDMODE_SELECT_OBJECT`, the
   constructor default) it runs `mouseMoveSelectObject(x, y)` (obs, §3.1).
2. The pick segment runs from the camera eye to the point on the **far plane** under the cursor:
   `end = (x, y, 1.0)·inverse(V·P·S)` (S = viewport pixel transform), so `t ∈ [0, 1]` along it (obs, §1).
3. `intersectGrid` tests, in this order and keeping the smallest `t`: **vehicles** (one horizontal rectangle per unit at
   0.8 above its pivot, 2.6 wide, length from the model's `frontOffset/rearOffset` stretched by `1 + 0.15·speed`,
   pulled 2 world units towards the camera), **characters** (within 1 unit of the segment around a point 1 above the
   feet, pulled ~2 units towards the camera), **items** (a horizontal rectangle over the footprint, 0.05 larger per
   side, **near the item's base**: `y + min(height>>4, 64)` in 1/256 cell, +1 for child items, then `cellCover`) and the
   **terrain** (cells walked from the eye, each a parallelogram through 3 corner heights). No meshes, no template
   bounding boxes (obs H, §2).
4. Only items, vehicles and characters whose template has a `displayName`, plus all network/station types 1..0x12,
   can be picked (markers and spawn points only with `grid.showMarkers`); fully covered terrain cells (`0xf00`) are
   skipped, so the item on them is what the ray hits (obs H, §2.5-2.6).
5. The result is turned into a **controller** (script object) with `gsGridItem/Vehicle/Character::getController`,
   created lazily and cached on the engine object (§5). The hover field `hoverObject` (`+0x2a0`, Windows `+0x300`, one
   field) holds one reference. **Event 17 `onHoverObject($object)` fires when the controller changes, and on every call
   (= every tick) while nothing is under the cursor** (with `null`) (obs H, §3).
6. A left press calls `mouseMove` again at the press point and fires event 24 `onTryApplyTool` (no args). The script
   (`hoverinfo.script:440-594`) calls `$player.applyTool()`, whose `finishTool` fires **event 20
   `onSelectObject($object)` with the hover controller**, then clears the hover and re-picks (so event 17 fires again
   with the same object). mouseUp does nothing in select mode (obs H, §4).
7. `secondaryview.script` answers event 20 with `clickObject($object)`: the corner HUD's info rows are filled from the
   controller and its model, a `gsFloatingCamera` (`ignoreOrient`, offset 200, pitch 1 rad, fov 2-7° or
   `(w+l+h)·0.4`) is targeted at the controller and renders through a 327×202 pixel viewport into a 512×256 texture
   target; a tracer arrow over the object, a minimap arrow pin and a double-click "centre camera" complete it (obs, §6).

### 0.1 Corrections to the brief (obs H)

* `+0x2a0` and `+0x300` are **the same field** (`hoverObject`): `+0x2a0` is the Linux offset
  (`L:0x0818b520.c:67`), `+0x300` the Windows one (`W:0x100f1b70.c:53-61`, native-api.md).
* The camera call at vtable `+0x128` is `gs3DCamera::getPosition` (the eye, `L:0x080e95a0`), resolved from the
  `gsFloatingCamera` vtable `0x0829d6a8` read from the ELF. It takes no bool: the `1` in the pseudocode is the stale
  third stack slot of the previous `getScreenMatrix(true)` call (`L:0x0818d3a0@0818d3b5-0818d3e8`).
* `gsVector::normalize(this+0x26c)` (`+0x26c` = 300.0, ctor `L:0x0818c120.c:37`) returns a new vector that is
  **discarded**; the segment end passed to `intersectGrid` is the unnormalised far-plane point
  (`L:0x0818d3a0@0818d453-0818d4b5`: `ESI` = the `operator*` result is passed, the normalize output at `+0xf0` is not).
* No other edit mode calls `mouseMoveSelectObject`; only case 1 of `mouseMove` does (`L:0x0819f2a0.c:114-115`).
  `GRIDMODE_ADD_STOP` (9) reads `hoverObject`, which the stop tracer **scripts** set
  (`S:scripts/ui.script:383-386,410-413`), and otherwise runs its own terrain-only pick (§4.1).
* mouseDown/mouseUp never fire event 20 themselves; mouseDown fires 24, the script's `applyTool()` fires 20 (§4).
* Side finding: `getWorldPosition` intersects the plane **y = 0**, not the terrain (§2.9); the port's
  `scene3d.cpp:614-655` marches the terrain instead.

---

## 1. Screen → world segment (`mouseMoveSelectObject` `L:0x0818d3a0.c:33-40`, obs H)

```
S_M  = viewport.getScreenMatrix(force = true)            // viewport = player.viewport (+0x1a4)    :34
eye  = camera.getPosition()                              // camera = player.camera (+0x1a0), vtbl +0x128   :35-36
p    = (x, y, 1.0)                                       // x, y = player-local mouse position = desktop pixels   :37
end  = p · inverse(S_M)            (4×4 with perspective divide, operator* L:0x0810deb0)              :38-39
intersectGrid(grid, eye, end, &cx, &cz, &item, &vehicle, &character, false)   // short overload    :44-46
```

`gsViewPort::getScreenMatrix(force)` (`L:0x081dc530.c:53-122`):

```
if cached (+0x1c9) and !force: return the matrix at +0x1ca                                 :53-69
cam = viewport.camera (+0x19c, gsCamera interface of the gs3DCamera)
cam.calculate() (+0x24); up = getUp() (+0x10); look = getTarget() (+0xc); eye = getPosition() (+0x8)   :74-82
V    = gsViewMatrix(eye, look, up)                                       (scene-basics §2.2)          :83
far  = vp.maxDistance (+0x1ac); f = cam.getMaxDistance() (+0x20); if f != −1 and f < far: far = f    :85-90
near = vp.minDistance (+0x1a8)                     // the camera's minDistance is NOT applied here
P    = gsProjectionMatrix(cam.getFOV()·0.017453292, near, far, aspect = vp.width / vp.height)        :93-95
Svp  = scale (w/2, −h/2, 0.5) then translate (w/2 + desktopLeft, h/2 + desktopTop, 0.5)   // +0xa8/+0xac, +0xb0/+0xb4   :97-103
M    = V · P · Svp;   cache it; cached flag = !force                                              :104-122
```

So screen depth 1.0 is NDC z = +1, the far plane, and `end` lies at view depth `far` under the cursor. Near does not
change `end` (inf H). In game the main viewport and the desktop are both pixel-coordinate, full-window, at (0, 0)
(`S:scripts/viewport.script:5-13`, `window.script:57-62`), so desktop pixels feed the matrix directly. With the
game camera `far = min(160 + zoom + 1.5·y, viewport 600)` (game-load.md §6.3): **nothing farther than the far plane can
be picked**, and `t` is a fraction of the eye → far-plane segment.

Port shortcut (equivalent, inf H): `ndc = (2(x − left)/w − 1, 1 − 2(y − top)/h, 1, 1)`,
`end = ndc · inverse(V·P)` with the projection the renderer already builds, except `near = viewport.minDistance` (any
positive near gives the same `end`).

---

## 2. `gsGrid::intersectGrid`

### 2.1 Signature (obs H)

```
bool intersectGrid(const vec3& start, const vec3& end, int& cellX, int& cellZ, int& height,
                   gsGridItem** item, gsGridVehicle** vehicle, gsGridCharacter** character, bool terrainOnly)
                                                                                     L:0x081402f0 (W:0x100b8020)
void intersectGrid(start, end, cellX, cellZ, item, vehicle, character, terrainOnly)   L:0x081416d0 (W:0x100b9a70)
     = the long form with a local height = −0x8000 (discarded)                       L:0x081416d0.c:20-21
```

* `height` is **in/out**. In: `−0x8000` = use the terrain heights; any other value = treat every cell as flat at that
  height (1/256 cell) and test only items with `TUNNEL` (0x8000). Out: the hit's height (§2.3-2.6).
* `item/vehicle/character`: null pointer = that class is not tested (vehicles, characters) or not reported (items).
  The caller zeroes the out-values; at most one ends up non-null (each later hit clears the others it may have set).
* `terrainOnly` (last bool): **true skips the item loop entirely** (`L:0x081402f0.c:278`, `@08140a4c`). Vehicles and
  characters are skipped only through null pointers. Callers: `getWorldPosition` passes true; everything else false;
  only `mouseMoveSelectObject` passes vehicle and character pointers (grep of all callers).
* Returns `bestT < 1.0`, i.e. "something was hit" (`:399, :526`).

### 2.2 Setup (`L:0x081402f0.c:114-158`, `@081402fa-0814051b`)

```
hW = W·cs·0.5,  hL = L·cs·0.5                          (float, not W>>1)
x0 = trunc((start.x + hW)/cs), z0 = trunc((start.z + hL)/cs)      // start cell
x1 = trunc((end.x + hW)/cs),   z1 = trunc((end.z + hL)/cs)        // end cell
dx = x1 − x0, dz = z1 − z0
steps = (|dz| > |dx| ? |dz| : |dx|) + 1
stepX = (end.x − start.x)/steps,  stepZ = (end.z − start.z)/steps   // world units per step, ≤ 1 cell
len   = |end − start|;  invLen = len < 0.1 ? 10.0 : 1/len           // world distance → t
best  = 1.0
rect  = cells [min(x0,x1), min(z0,z1)] size (|dx|+1, |dz|+1)       // candidate area for vehicles/characters/items
```

Candidate lists (obs H): `gsGridVehicles::getVehicles(rect)` (`L:0x081af770`) and `gsGridCharacters::getCharacters`
(`L:0x08154c80`) take every agent of every division overlapping the rect **grown by 3 cells** (division range
`(minX−3)>>shift .. (minX+w+3)>>shift`, clamped; per-division lists `division+0x10` / `+0x14`, no per-agent filter).
`gsGrid::getObjects(rect)` (`L:0x0813fdb0`) takes the divisions overlapping the rect grown by one division, skips items
with `TEMP` (0x100000) and keeps those whose footprint overlaps the rect (`gsGridItem::overlap` `L:0x081833c0`:
half-open; diagonal items need one covered cell in the overlap). Orders: divisions z-major then x, list order inside.

### 2.3 Vehicles (`:159-222`, `@08140527-0814088e`; only if `vehicle != null`)

```
for v in vehicles:                                   // linked by v+0x14
  if v.mesh (+0x8) == null: continue                 // no render instance → not pickable
  m = v.model (+0x4, gsGridObject template)
  if m.displayName (+0x280) == "" and !grid.showMarkers (+0x22c): continue
  pos = v.mesh.getPosition() (vtbl +0x58), R = v.mesh.getOrientation() (vtbl +0x5c)   // the rendered pose
  s   = 1.0 + 0.15·v.speed (+0x58)
  C   = pos + rot(R, (−1.3, 0.8, m.rearOffset (+0x230)·s))
  e1  = rot(R, (2.6, 0, 0));  e2 = rot(R, (0, 0, −s·(m.frontOffset (+0x22c) + m.rearOffset)))
  t   = gsLineRectIntersect(e1, e2, start − C, end − start) − 2·invLen
  if t ≥ 0 and t < best:
     best = t; cellX = trunc((pos.x + hW)/cs); cellZ = trunc((pos.z + hL)/cs); height = trunc(pos.y/cs·256)
     *vehicle = v.leader (+0x1c) ?: v                // one level: the leading unit of a player set / lorry
```

So each unit is a flat 2.6-wide rectangle at local height 0.8, spanning local z ∈ [−front·s, rear·s]; template
defaults front = rear = 1.5 (`L:0x08186210.c:15-16`). The `−2·invLen` bias ranks the vehicle as if it were 2 world
units nearer, so it beats the road, terrain or item it is up to 2 units in front of. The `−1` "parallel/outside"
return of the rect test becomes `−1 − 2·invLen`, still rejected. AI train carriages have no leader (`+0x1c` = 0, vehicles-movement.md §2.3), so each carriage is its own
pick; player sets resolve to the head unit. The pose is the one `gsGridVehicle::move` pushes to the mesh every tick
(`L:0x081adca0.c:700-760`).

### 2.4 Characters (`:224-277`, `@0814089c-08140a48`, `@08141320-0814145a`; only if `character != null`)

```
for c in characters:                                 // linked by c+0x14
  if c.model (+0x4).displayName == "" and !showMarkers: continue
  L  = c.position (+0x4c) + (0, 1, 0)
  d2 = gsDistanceFromLineSq(end − start, start − L)   // squared distance of L from the infinite line   L:0x08109950
  if d2 < 1.0:
     t = (|L − start| − 2.0 + sqrt(d2))·invLen
     if t ≥ 0 and t < best:
        best = t; cellX/Z = trunc((L.x + hW)/cs), trunc((L.z + hL)/cs); height = trunc(L.y/cs·256)
        *vehicle = null; *character = c
```

A vertical "radius 1 around the point 1 above the feet" test, ranked by the distance to that point minus 2 (so a
character beats terrain/items up to ~2 units behind it). No mesh is required.

### 2.5 Items (`:278-398`, `@08140a4c-08140ed3`, `@081414d0-081416b6`; skipped when `terrainOnly`)

```
for it in getObjects(rect):                          // linked by it+0x18
  f = it.flags (+0x48)
  if f & 0x500000: continue                          // TEMP | hidden (0x400000)
  if heightIn != −0x8000 and !(f & 0x8000): continue // fixed-height picks see only TUNNEL items
  if !it.isVisible(grid.viewMode (+0x1fc)): continue // L:0x08185c50, below
  type = it.type (+0x58)
  if type in {0x15 MARKER, 0x16 SPAWNPOINT}: if !showMarkers: continue
  elif type == 0 or type ≥ 0x13:                     // NONE, TREE, BUILDING, TIMER
       if template.displayName == "" and !showMarkers: continue
  // types 1..0x12 (roads, tracks, tram, metro, stations, docks, heliports) are always tested
  x = it.x (+0x2c), z = it.z (+0x34), sx = it.sizeX (+0x38), sz = it.sizeZ (+0x40)   // footprint after orientation
  hgt = it.height (+0x3c, template height·256)
  if type == MARKER and sx ≥ 2 and hgt > 0x3ff:      // markers only: the 2×2 core
       x += (sx>>1) − 1; z += (sz>>1) − 1; sx = sz = 2
  off = hgt > 0x3ff ? 0x40 : hgt >> 4                // = min(hgt>>4, 64): at most a quarter cell
  top = (it.y (+0x30) + off)·cs/256  (+1.0 if it.parent (+0x1c) != null)
  C   = ((x − (W>>1))·cs − 0.05, top, (z − (L>>1))·cs − 0.05)
  t   = gsLineRectIntersect((sx·cs + 0.1, 0, 0), (0, 0, sz·cs + 0.1), start − C, end − start)
  if t ≥ 0 and t < best:
     hx = clamp(trunc((start.x + (end.x − start.x)·t + hW)/cs), x, x + sx − 1)
     hz = clamp(trunc((start.z + (end.z − start.z)·t + hL)/cs), z, z + sz − 1)
     if it.cellCover(hx − x, hz − z) != 0:          // L:0x08183740: 0xf00 when it.diagonals (+0x54) == 0,
                                                     // else the diagonal cut test of terrain.md §2.1 (L:0x08183120)
        cellX = hx; cellZ = hz; height = it.y
        if item: r = it; while (r.flags & 0x6000000) == 0x6000000 and r.parent: r = r.parent; *item = r
        *vehicle = *character = null; best = t
```

`isVisible(viewMode)` (`L:0x08185c50`): TEMP items → true; trees (0x13) → `viewMode & 0x08`; otherwise take the root
ancestor; underground view (`& 0x100`) and root `TUNNEL` → true; root type 2..0x12 or root flag 0x8000000 →
`viewMode & 0x10`; else `viewMode & 0x20`. The game default `0xfe` shows everything.

**Consequence (inf H):** the item plane sits 0..0.75 world units above the item's base, so a ray that enters a tall
building's facade high up and leaves through the back before reaching ground level picks whatever is behind; most
picks of buildings happen through the footprint seen from above. Trees, props and buildings without a `displayName`
are invisible to picking.

### 2.6 Terrain (`:399-525`, `@08140ee1-08141313`)

Runs if `(best ≥ 1.0 or viewMode & 0x02) and steps > 0`: with the terrain surface hidden (`viewMode & 2` clear, e.g.
the underground view) terrain is tested only when nothing else was hit.

```
x = x0; z = z0
fx = start.x − (x0 − (W>>1))·cs;  fz = start.z − (z0 − (L>>1))·cs      // offset inside the start cell
repeat steps times:                                                      // the start cell itself is never tested
  fx += stepX; fz += stepZ
  nx = x; if fx ≥ cs: fx −= cs, nx = x + 1  elif fx < 0: fx += cs, nx = x − 1
  nz = z; if fz ≥ cs: fz −= cs, nz = z + 1  elif fz < 0: fz += cs, nz = z − 1
  if nx != x and nz != z: testCell(nx, z); testCell(x, nz)               // corner step: both side cells first
  testCell(nx, nz)
  x = nx; z = nz

testCell(cx, cz):
  t = intersectCell(start, end, cx, cz, heightIn)
  if t ≥ 0 and t < best:
     best = t; cellX = cx; cellZ = cz
     height = heightIn == −0x8000 ? (h(cx,cz) + 0x80) & ~0xff : heightIn       // rounded to whole cells
     *item = *vehicle = *character = null
```

The walk does not stop at the first hit; all cells along the segment are tried and the minimum `t` wins (so a hill in
front hides an item behind it, and terrain behind an item never replaces it).

`gsGrid::intersectCell(start, end, x, z, heightIn)` (`L:0x0813f280.c:34-71`, `@0813f280-0813f4d4`):

```
if x < 0 or z < 0 or x ≥ W or z ≥ L: return −1
c = cell(x, z)  (grid+0x18c, {i16 h, u16 f}, row-major)
if heightIn == −0x8000 and (c.f & 0xf00) == 0xf00: return −1        // cell fully covered by items
if heightIn == −0x8000:
   h00 = c.h·cs/256
   h10 = (x < W−1 ? cell(x+1, z).h : c.h)·cs/256
   h01 = (z < L−1 ? cell(x, z+1).h : c.h)·cs/256
else h00 = h10 = h01 = heightIn·cs/256
C = ((x − (W>>1))·cs, h00, (z − (L>>1))·cs)
return gsLineRectIntersect((cs, h10 − h00, 0), (0, h01 − h00, cs), start − C, end − start)
```

The cell is the parallelogram through its corners (x,z), (x+1,z), (x,z+1); the fourth corner's real height is ignored.
Heights are corner heights in the sense of terrain.md (corner (gx,gz) = cell (gx,gz)).

### 2.7 Helpers (obs H)

`gsLineRectIntersect(e1, e2, a, D)` (`L:0x0810ad20.c`): a parallelogram `C + u·e1 + v·e2`, `a = origin − C`:

```
n = e1 × e2;  det = D·n;  if det == 0: return −1
inv = (float)(1/det)
u = D·(a × e2)·inv;   v = D·(e1 × a)·inv
return (0 ≤ u ≤ 1 and 0 ≤ v ≤ 1) ? −(a·n)·inv : −1          // t along D; two-sided; t may be negative
```

(Checked by hand: e1 = (1,0,0), e2 = (0,0,1), a = (0.5, 10, 0.5), D = (0,−1,0) → u = v = 0.5, t = 10.) Linux computes
in x87 long double with `inv` rounded to float.

Not used by picking (callers grepped): `gsLineTriangleIntersect` (`L:0x0810ab20`, the same with `u + v ≤ 1`) and
`gsLineSphereIntersect(o, d, r, &t0, &t1)` (`L:0x0810af10`: roots of `|o − t·d|² = r²`, `t0,1 = (2(o·d) ∓ √disc)/(2|d|²)`,
false if `|d|² = 0` or `disc < 0`) are used only by `gsMeshDecals::paintDecal` (`L:0x080f3f20`); `gsIntersect(boxA,
boxB)` (`L:0x0810b930`, inclusive AABB overlap, min at +0, max at +0xc) by `gs3DElement::getObjects` (`L:0x080d8060`).
`gsDistanceFromLineSq(D, w) = |D × w|²/|D|²` (0 if `|D| = 0`, `L:0x08109950`).

### 2.8 Priority and ties (obs H)

Classes are tested vehicles → characters → items → terrain, each with a strict `t < best`; equal `t` keeps the earlier
class. The biases (vehicles −2 world units, characters −2 + distance from the axis) make moving objects win over the
road or ground they stand on. `mouseMoveSelectObject` then prefers item, vehicle, character in that order, but only
one of them can be set.

### 2.9 Side finding: `getWorldPosition(x, y)` (`L:0x0818bd80.c`, `@0818bd80-0818c0d0`; obs H)

Same segment as §1 (screen x, y from the two arguments), then `intersectGrid(eye, end, …, height = 0, null, null,
null, terrainOnly = true)`: flat-cell mode at height 0, no items. Result: if `eye.y > end.y`,
`eye + (end − eye)·(eye.y − height·cs/256)/(eye.y − end.y)` with `height` = 0 in both the hit and the no-hit branch,
i.e. **the intersection with the plane y = 0**; if the ray does not descend, the far point (no hit) or
`((cx − (W>>1))·cs, 0, (cz − (W>>1))·cs)` (hit; note `W>>1` for z too). Shipped ground level is 512 = 6 world units
(save-format README §3.1), so the result lies a few units beyond the visible ground point; scripts use it for panning
deltas (game-load.md §6.7), where the plane height mostly cancels. The port should return the y = 0 intersection.

---

## 3. Hover: `mouseMoveSelectObject` second half (`L:0x0818d3a0.c:41-77`, `@0818d4ec-0818d5b0`; obs H)

### 3.1 When it runs

`gsGridPlayer::mouseMove(x, y, dx, dy, buttons)` (`L:0x0819f2a0`) first checks `this+0x25a` (middle button held, set by
mouseDown with key 144, cleared by its mouseUp) and `editMode > 0x15`: either → base `gsControl::mouseMove` only
(`@0819f2b1-0819f2c1`). Otherwise it switches on `editMode` (§4.1); case 1 calls `mouseMoveSelectObject(this, x, y)`
(`@0819f64c-0819f665`) and then the base `gsControl::mouseMove` (script `onMouseMove`). The desktop calls the hovered
control's `mouseMove` **every tick**, also with dx = dy = 0 (input.md §3.2, §5), so the hover follows moving vehicles
under a still cursor. Over a HUD control (a child of the player) the player gets no `mouseMove`, so the hover keeps its
last value (inf H).

### 3.2 Controller swap and event 17

```
ctrl = item      ? item.getController(null)       // L:0x08181390, grid taken from the item's division
     : vehicle   ? vehicle.getController()        // L:0x081aac80
     : character ? character.getController()      // L:0x08152350
     : null                                       // each returns +1 reference (addRef on the cached path)
old = this.hoverObject (+0x2a0)
if ctrl != null and ctrl == old: ctrl.release(); return            // unchanged: no event
if old: old.release()
this.hoverObject = ctrl                           // the field keeps getController's reference
arg = ctrl ? ctrl.reference() : gsVariant::null   // reference() = ++refcount, returns this (L:0x0826fd10)
event(17 onHoverObject, arg)                      // gsObjectVariant::event(id, gsVariant*) L:0x08266140
```

**When 17 fires:** whenever the controller under the cursor differs from the field (including changes to `null`), and
**every tick while nothing pickable is under the cursor** (the `null == null` case is not filtered,
`@0818d5a8-0818d5b0` → `@0818d51b`), and also every tick while the hit object returns no controller.

**What the handler receives:** `event(id, v)` calls `event(id, 1, &v)` (`L:0x08266140.c`). For each bound handler a
new thread gets `min(1, nParams)` arguments copied (add-ref'd) into locals, the player is pushed as `current`, the
handler runs immediately; afterwards the argument's reference is released (`L:0x08265d70.c:42-59`, semantics.md §4.2).
So `event $player.onHoverObject::($object)` gets `$object` = the controller object (a `gsGridItemController`,
`gsTransportStopController`, `gsGridVehicleController`, `gsTransportVehicle` or `gsGridCharacterController`) or `null`;
`this` in the body is the binding script, `current` is the player.

### 3.3 Other writers of the hover field (obs H)

| Who | Effect | Event 17? |
|---|---|---|
| `cancelTool` in mode 1 (`L:0x0819bcc0.c:38-43`) | release + clear | **no** |
| `setEditMode` (`L:0x0819c2c0.c:39-43`) | release + clear if still set | yes, `null` (only when the old mode was not 1: cancelTool already cleared it) |
| `finishTool` mode 1 (`L:0x0819a3e0.c:98-114`, tail `:825-834`) | fires 20, then release + clear | no |
| `destroy` (`L:0x0819ca70.c:31-35`) | release + clear | no |
| scripts: `MAIN.$player.hoverObject = …` (`S:scripts/ui.script:385,412,456`) | plain var field write | no (mode 9 only) |

`cancelTool` runs from `resetTool()`, `setEditMode`, `setActive(true)` when the player was inactive
(`L:0x0819c270.c`), and right-button/escape paths of other modes.

---

## 4. Click → `onSelectObject`

### 4.1 `mouseMove` modes that pick (`L:0x0819f2a0.c`, obs H)

| editMode | Pick | Fires |
|---|---|---|
| 1 SELECT_OBJECT | `mouseMoveSelectObject` (items, vehicles, characters, terrain) | 17 `onHoverObject` |
| 8 REMOVE_OBJECTS | `intersectGrid(…, &item, null, null)` → item set highlight (`:520-597`) | 23 `onHoverDemolish` |
| 9 ADD_STOP | uses `hoverObject` if it is a stop controller of the line's transport type (set by the stop tracers), else terrain-only cell + nearest stop item in 3×3 cells (`:599-733`) | 26 `onHoverAddStop` |
| 0x14 RENAME / 0x15 SET_VARIATION | `intersectGrid(…, &item, null, null)` → road segment highlight (`:1426-1483`) | 18 / 19 with the item controller |
| all other tool modes (2-7, 0xa-0x13) | own `intersectGrid` calls (`mouseMoveLay*`, `PlaceObject`, `CreateStop`, brushes): terrain cell and at most an item, never vehicles or characters | their own events (not traced here) |

Every case ends in the base `gsControl::mouseMove` (`:1485-1494`).

### 4.2 `gsGridPlayer::mouseDown(x, y, key, buttons)` (`L:0x0819c410.c`, obs H)

For key 143 (MouseLeft) in mode 1:

```
if this+0x258 (tool in progress): skip to the end                                         :38-39
toolPos (+0x1b8/+0x1bc) = (x, y); this+0x258 = 1                                           :131-133, 65
save the cached cell (+0x1a8..+0x1b4 → +0x25c..+0x268), cached x/z = −1                    :135-142
this.mouseMove(x, y, 0, 0, 0)          // vtbl +0x10c: re-pick at the press point (may fire 17)   :143
mode bit in 0x302002 (modes 1, 0xd, 0x14, 0x15): this+0x258 = 0; event(24 onTryApplyTool)  :144-154
this+0x25a = (key == 144); gsControl::mouseDown(…)   // script mouseDown["MouseLeft"](x, y)  :211-215
```

Right button (145) in mode 1 only cancels a running tool (`:182-189`); middle (144) sets `+0x25a`.

### 4.3 Script → `applyTool` → `finishTool` (obs H)

`hoverinfo.script` (loaded by `S:scripts/ui.script:41`) binds `event $player.onTryApplyTool::() tryApplyTool();`
(`S:ui/hoverinfo/hoverinfo.script:440-442`). `tryApplyTool($mouseMove = true)` (`:446-594`) in select mode:
`canApplyTool()` (true: no new/replaced item carries `ERROR` 0x200, `L:0x0818bb80`) → `$price` is 0 →
`getNewItems/getReplacedItems/getNewObjects/getReplacedObjects` (empty) → `$player.applyTool(true)` →
`MAIN.$playerData.updateCompanyValue(); $player.active = true`.

`method_applyTool(arg)` (`L:0x0819cbe0.c`): `ok = finishTool()`; if no argument or a true argument,
`this.mouseMove(toolPos.x, toolPos.y, 0, 0, 0)`; returns `ok`. `finishTool` case 1 (`L:0x0819a3e0.c:98-114`,
`@0819a6b7-0819a6ee`):

```
if hoverObject == null: clear +0x258/+0x259/+0x25a, cached x/z = −1, +0x2a4 = 0; return true      // no event
event(20 onSelectObject, hoverObject.reference())
common tail: clear the same flags; hoverObject.release(); hoverObject = null; return true          :825-837
```

The re-pick in `applyTool` then finds the same controller (still referenced by the scripts) with an empty field and
fires event 17 again with it.

### 4.4 `mouseUp(x, y, key, buttons)` (`L:0x0818b1e0.c:21-58`, obs H)

Modes 1, 8, 0xd (mask 0x2102) skip all tool logic; only key 144 clears `+0x25a`; then the base `gsControl::mouseUp`
(script `mouseUp[…]`). Other modes: left up with `+0x258` set → store the position, fire 24; modes 0x14/0x15 and 0xe
(after `endGroup` of the timeline) cancel on right up.

### 4.5 `setEditMode` / `write_editMode` (`L:0x0819c2c0.c`, `L:0x0819c3e0.c`; obs H)

Script `player.editMode = m` → `write_editMode` → `setEditMode(m)`:

```
old = editMode; cancelTool()                         // with the OLD mode (mode 1: silent hover clear)
toolMode = toolType = 0; editMode = m
release + clear toolObject (+0x270), toolObject2 (+0x274), toolLine (+0x278)
if hoverObject: release, clear, event(17, null)
event(21 onModeChange, old ? new gsIntVariant(old) : gsVariant::zero)
```

`hoverinfo.script:596-602` answers 21 with `hideText()`, which is what removes the tooltip when leaving select mode.

### 4.6 One click, in order (obs H, timing input.md §5)

1. Message pump: WM_LBUTTONDOWN → desktop → hit control = player → `mouseDown(x, y, 143)`.
2. Re-pick (§1-3) at the press point; maybe event 17.
3. Event 24 → `tryApplyTool()` → `applyTool(true)` → `finishTool` → **event 20 `onSelectObject(ctrl)`** →
   `secondaryview.script:809` `clickObject($object)` (§6.2), synchronously.
4. Hover cleared, re-pick → event 17 `(ctrl)` → tooltip shown again.
5. Script `mouseDown["MouseLeft"]` on the player (none bound for selection). On release nothing happens in mode 1.

---

## 5. Controllers

### 5.1 Who returns what (obs H)

| Engine object | getController | Cache field | Class created | Size | Pose at creation | Pose updates |
|---|---|---|---|---|---|---|
| `gsGridItem` | `getController(grid)` `L:0x08181390` (= `W:0x100ec260`); regparm twin `L:0x0817ee20` | item `+0x8` | `gsTransportStopController` if item flag `STOP` (0x2000), else `gsGridItemController` | 0x130 / 0x110 | footprint centre `((2x + sx − W)·cs/2, y·cs/256, (2z + sz − L)·cs/2)`, `quat(orient·π/2, −Y)` (`gsGridItemController::create` `L:0x0817e950`) | none (items do not move) |
| `gsGridVehicle` | `L:0x081aac80` | vehicle `+0x2c` | `gsGridVehicleController`; for a spawned **player** vehicle the field already holds its `gsTransportVehicle` (`gsTransportVehicle::move` `L:0x081bd7f0.c:171`), which is returned | 0x10c | `calculatePose` (`L:0x081ac8e0`) | `setGlobalPose` every vehicle tick (`L:0x081adca0.c:748-752`) |
| `gsGridCharacter` | `L:0x08152350` | character `+0x18` | `gsGridCharacterController` | 0x10c | position `+0x4c`, `quat(angle +0x74, +Y)` | each character tick (inf M, `L:0x08153b90`) |
| `gsGridCitizen` | `L:0x081599d0` | citizen `+0x4` | `gsGridCitizenController` | 0x108 | — | — |

Citizens are never picked; their controllers come from `getCitizen()` of vehicle/character controllers
(`L:0x081a9bb0`: vehicle `+0x30`; `L:0x08151b00`: character `+0x1c`).

### 5.2 Lifetime and references (obs H)

* **Lazy, cached:** created on the first `getController` (any caller: picking, scripts' `getCitizen`, line stops,
  `getNewObjects`, …), stored in the cache field, `+0x104` = the engine object, `+0x100` ("auto") = true
  (`gsGridController::gsGridController` `L:0x08165ff0`), then `create(grid)` (vtbl `+0xa8`): `gsScript::create`,
  `gs3DElement::setOwner(grid)` (vtbl `+0xc4`), so the controller is a child of the grid; the subclass `create` writes
  the back pointer and the pose.
* **Returned with +1:** the create path returns the new object (refcount 1 = the caller's) plus the grid's child
  reference; the cached path add-refs.
* **Auto-destroy:** `gsGridController::release` (vtbl `+0xc`, `L:0x08163460`): if auto and the refcount is exactly 2
  (the caller's plus the grid's), `free()` first: `onDestroy` (event 0), `destroy()` (vtbl `+0x78`: clears the back
  pointer, e.g. item `+0x8` = 0, `L:0x0817e920`), destroyed flag; then the release. So **a controller lives exactly as
  long as something besides the grid refers to it** (the hover field, a script variable, a camera `target`, a line).
  The next `getController` creates a new object; script `$fields` set on the old one are gone.
* **Engine object removed:** `itemDeleted()` (`L:0x08163450`) runs the same destroy sequence → `onDestroy` reaches
  `cameratarget.script:47-68`, which empties the preview.
* `hasInstance()` (vtbl `+0x128`): true for all controllers except `gsTransportVehicle`, where it means "spawned"
  (`L:0x081b68d0`: `+0x104 != 0`). `getModel()` (vtbl `+0x124`): the engine object's template (`+0x4`) for items,
  vehicles and characters (`L:0x0817d920`, `L:0x081a9900`, `L:0x08151a10`); `gsTransportVehicle` returns its model
  (`L:0x081b68c0`); citizens return null (`L:0x08163430`).

### 5.3 Members per class (native-api.md; "uses" = all shipped scripts; ★ = used on the hover/select path, §6)

| Class | Members (uses) | Native semantics worth knowing |
|---|---|---|
| gs3DElement (base) | ★`position` (19259), ★`globalPos` (14), `orientation` (14), `forceVisible` (0) | controllers are scene elements: position = §5.1 pose |
| gsObjectVariant (base) | ★`hasMember` (15), ★`destroyed` (11), ★`onDestroy` (1071) | `hasMember("isVehicle")` is how scripts tell the kinds apart |
| gsGridController | ★`getModel` (44), ★`hasInstance` (2), ★`forceSounds` (7, L `+0x101`: the vehicle's sounds play listener-relative), ★`itemUpdated` event 6 (2; e.g. after rename `L:0x0819a3e0.c:449`) | |
| gsGridItemController | ★`name` (39, wstring; read hook copies item `+0x60`, `L:0x0817eb10`), `variation` (242, item `+0x6c`), `setColor` (5), ★`getPos` (3: `z<<16 \| x`, `L:0x0817f130`), `getConnections` (3), ★`getStats` (1: item `+0x5c` traffic word), `getSize` (0: `sx<<16 \| sz`), `getFlags` (7), `activate`/`deactivate` (1/1), `onError` (1) | |
| gsTransportStopController | ★`passengerCount` (6), ★`type` (2258, shared name), ★`attractiveness` (12), `mood` (1), ★`catchmentArea` (10), `getPassengersByLine` (3), `onMoodChange` (1); ★`visible` (gsModule) | `visible = true` draws the catchment circle (`statusUpdated` `L:0x081b6e70` → `grid.addAreaHighlight(centre, radius)`) |
| gsGridVehicleController | ★`isVehicle` (0, marker field), ★`getCitizen` (3), `playSound` (1), ★`getID` (9: vehicle `+0x80` colour seed) | |
| gsTransportVehicle | ★`passengerCount` (6), ★`line` (5), ★`attractiveness` (12), `travelDistance` (5), ★`capacity` (173), `condition` (3), `broken` (0), `stopTime` (1), `passengersTransported` (5), `addTrailer` (1), `isSpawned` (2), ★`onSpawn`/★`onUnspawn` (3/2), … ; script fields ★`$id`, ★`$type` set by the game scripts | |
| gsGridCharacterController | ★`isCharacter` (0, marker), ★`getCitizen` (3) | no `getID`: `secondaryview.script:604` would fail for a character model with `$objectinfoPattern` (none in the base game, inf) |
| gsGridCitizenController | ★`getID` (9), ★`getCharModel` (5), ★`getCarModel` (1), ★`getHome`/★`getWorkplace` (4/4), ★`getStart` (10), ★`getTarget` (3), ★`getClass` (5), ★`getState` (6), `getMood` (2), ★`getCharacter` (689), ★`getVehicle` (3), `usedTransport` (3), ★`onSpawnCharacter`/★`onSpawnVehicle`/★`onIdle` (2/2/0) | |

Model (`gsGridObject`) members read on the path: `displayName`, `type`, `width`, `length`, `height`, `frontOffset`,
`rearOffset`, `getHomePlaces/getWorkPlaces/getVisitPlaces`, `getFunction("customObjectInfo")`, and script fields
`$type`, `$price`, `$icon`, `$querySound(s)`, `$objectinfoPattern/Range/Text`, `$occupations`, `$maintenanceCost`,
`$portraits`, `$gender`, `$doubleClickAction`.

---

## 6. Script side

### 6.1 `hoverinfo.script` (obs H)

* `event $player.onHoverObject::($object) { hoverObject($object); $hoverObject = $object; }` (`:174-177`):
  `$object` non-null → `displayText(MAIN.$secondaryView.getItemName($object))`, else `hideText()` (`:166-172`).
* `displayText` (`:133-156`) creates the tooltip once as `new_control($desktop.mouse)` at `(0, 42, 200, 100)` (a child
  of the mouse pointer control, so it follows the cursor), `MAIN.$mouse.startAnimation()`, font via
  `MAIN.$ui.setTextFont`, `text = …`, `setArg(0, |price|)`; `hideText` deletes it and ends the animation.
* Visibility follows the player's and HUD's `onMouseOver/Out` and `onActivate/Deactivate` (`:623-674`).
* `getItemName($item)` (`secondaryview.script:364-391`) is what the tooltip shows:
  * `hasMember("isVehicle")`: own vehicle (`hasMember("passengerCount")`) → `$roster_vehicleName_pattern` with
    `$vehicle_name_patterns[model.$type]` and `MAIN.$gameData.$ownVehicles[$item.$id]["index"]`; AI vehicle →
    `getModelName(model)`.
  * `hasMember("isCharacter")` → `getModelName(model)`.
  * items: `name` starting with `$` → `$strings.getVariable("name_…")`; other non-empty `name` →
    `$roster_vehicleHint_pattern(modelName, name)` (Russian: `latinToRussian`); else `getModelName(model)`.
  * `getModelName` (`:334-345`): `displayName`, else by type: alley, road, track, tram track, metro track, metro station,
    tree, "unnamed".

### 6.2 `secondaryview.script`: selection and the corner HUD (obs H)

Loaded by `S:scripts/ui.script:39`. One HUD element `$bg` (342×228) at the bottom right
(`createHudElement($desktop.width − 342, $desktop.height − 228, …)`, `:68`) holds two pages: the **camera page**
(`$cameraArea` 4,19,327,202, `:95-99`) and the **data page** (`$infoArea`, `:162`), switched by `$cameraButton` /
`$dataButton(2)` (`showCamera` `:275-288`, `showData` `:290-302`). There is no separate window: the "info window" of
a selection is this panel's rows (`createInfoItem` `:199-233`, ids `:815-837`), plus, for own vehicles, the line
vehicle panel (`MAIN.$linevehicles.trySelectVehicle($target.$id)` `:588`, "manage vehicles" button `:251-259`). The
page shown at start is the camera (`showCamera()` `:839`).

`event $player.onSelectObject::($object) clickObject($object);` (`:809`). `clickObject` (`:793-807`): a second click
within 0.3 s (`$timerThread = thread :: { wait 0.3; … }`) on the camera's current target centres the main camera
(`MAIN.$gameUI.setCameraPos(x, z, false)`) and runs `model.$doubleClickAction()` if defined; otherwise
`setCameraTarget($object, null, true)`. Pins and stop tracers call `clickObject(.targetModule)` too
(`ui.script:459`, `linepanel.script:293`, `bubbletarget.script:41`).

`setCameraTarget($target, $citizen = null, $sound = false)` (`:682-783`):

1. Same target (and same citizen) → return. Otherwise tear down the old one: stop `visible = false` (catchment),
   `forceSounds = false`, delete `$uiArrow`/`$minimapArrow`, `camera.target = null`, delete `$cameraTarget`, viewport
   invisible, image back to `empty-viewport.dds`, `MAIN.$listener.tracked2 = null`.
2. Per kind: vehicle → `fov` = 5.0 (`model.$type` 4), 7.0 (3), 4.5 (others with `$type`), else
   `max(4, (frontOffset + rearOffset)·0.3)`; `position = (0, 1, 0)`; `$citizen = target.getCitizen()`; query sound.
   Character → `fov 2.0`, `position (0, 1, 0)`, `getCitizen()`, `model.$querySounds`. Item → `fov = (width + length +
   height)·0.4`, `position = (0, height·1.5, 0)`, stops: `visible = true`.
3. `$cameraTarget = load "cameratarget.script"; initTarget(camera, target)` (below). If `target.hasInstance()`:
   viewport visible, `$cameraImage.setImage($cameraTexture.getTexture(), 0)`, `bloom.scale = 0`, GUI layer visible,
   `$secondaryCamera.reset()`, `forceSounds` while the camera page is shown. `listener.offset2 = (0, 0, 100)`.
4. Data/character buttons and `updateInfoItems($citizen, $target)` (`:512-680`): citizen → `citizentarget.script`
   (re-targets the camera on `onSpawnCharacter/onSpawnVehicle/onIdle`, clears on `onDestroy`), name from
   `MAIN.$names` by `getID()`, portrait `model.$portraits[(id >> 10) % n]`, rows home/workplace/occupation/car/
   source/destination/method and four `createPin`s (`new_pinControl` on `MAIN.$frontHud` + `MAIN.$minimap.createPin`);
   own vehicle → name/line/condition/attractiveness/income/expenses/onboard (live `macro`s), info text, icons;
   AI vehicle/character → model name + `$objectinfoPattern` text seeded by `getID()`; stop → names, attractiveness,
   passengers, expenses; network item → names + traffic `getStats()·100 / (max(4,w)·max(4,l)·20)`; building →
   names, homes/workplaces (sum of `getHomePlaces/getWorkPlaces` over the 7 citizen classes), visit reasons, info
   text seeded by `getPos()` and the clock, `customObjectInfo`.

`cameratarget.script` `initTarget($c, $t)` (`:5-108`): if `hasInstance()`: `camera.target = $t`, minimap pin
`createPin($t, 0xFFFFFFFF, "arrow")`, a `new_tracerControl(MAIN.$backHud)` arrow (`targetViewport = MAIN.$viewport`,
`targetPosition = offset·0.5` with offset `(0, height·3 + 0.5, 0)` or 5/4 for `$type` 4/3, `anchorX 0.47`,
`anchorY 1.1`, 64×64 `arrow.dds`, `organize = false`, `targetModule = $t`). Binds `$t.onDestroy` (empty the
preview, refresh rows), `$t.itemUpdated` (refresh rows), and for transport vehicles `onSpawn`/`onUnspawn`
(re-attach / detach the camera).

Clicking the preview (`$secondaryViewport.mouseDown["MouseLeft"]`, `:785-791`) centres the main camera on
`camera.target.globalPos` (or the camera's own `globalPos`).

### 6.3 The corner preview camera (obs H unless marked)

| Piece | Value | Evidence |
|---|---|---|
| camera | `new_floatingCamera(MAIN.$scene)`: `offset (0, 0, 200)`, `position 0`, `orientation = quat(1.0, X)·quat(0.2, Y)`, `fov 15` (until a target), `smoothness 1e-6`, `ignoreOrient = true`, `minDistance 150`, `maxDistance 400`, `reset()` | `:187-197` |
| viewport | `new_viewport($cameraArea)`: `pixelCoordinates`, rect (0, 0, 327, 202), `minDistance 150`, `maxDistance 600`, `visibleGroups 1`, invisible until a target has an instance; `scene = MAIN.$scene`, `camera = $secondaryCamera` | `:104-112, 304-305` |
| effects | own shadow effect (`$settings.$shadows·256` square, `yPlane −10`, `clipYPlane`), bloom if enabled, then `$cameraTexture = new_textureTarget(512, 256, false)`; `video.renderViewport($secondaryViewport)` once | `:101, 307-329` |
| image | `$cameraImage` (child of `$cameraArea`, click-through): slot 0 = target texture (or `empty-viewport.dds`), slot 1 = `viewport-mask.dds`, `viewport.shader` (`rgb = tex0.rgb·mask.g + 0.227·mask.b`, `a = mask.r`, both sampled with the same uv), `setCropRect(0, 202/256 − 1, 327/512, −202/256)` | `:122-131`, `ui/secondaryview/viewport.fragment` |
| grid cameras | `$grid.addCamera($secondaryCamera)` on `showCamera`, `removeCamera` on `showData`: the grid's camera set (`grid+0x204`, `L:0x08145c40`/`L:0x08145cd0`) that `gsGrid::move` walks to keep divisions around every camera updated (`L:0x08148e60.c:107-175`; purpose inf M) | `:287, 291` |

View with a target (inf H from scene-basics.md §2.1 and `gs3DCamera::getPosition/getTarget`
`L:0x080e95a0`, `L:0x080e9660`): `ignoreOrient` → `Q = localOrient`, `finalOffset = offset`, no smoothing; eye =
`target.globalPos + rot(Q, (0, 0, 200))`, look = `target.globalPos`. With the game-load.md §6.3 convention
(quat(0.855, X) gives eye − P = (·, 113.2, 98.4) for offset 150) this is **eye − target ≈ (−21.5, 168.3, 105.9)**: 200
units away, 57.3° down, heading 0.2 rad. The camera's own `position` (set to `(0, 1, 0)` / `(0, height·1.5, 0)`) does not
enter the view while `target` is set (inf M; open question 2). Projection: near = max(150, 150), far = min(400, 600),
aspect = 327/202, vertical fov as set (e.g. a 4×4×3 building: 4.4° → ~15 world units of height in view). The target
allocation follows scene-basics.md §7.1: `max(327, 512) × max(202, 256)` = 512×256, current size 327×202, image rows
from the bottom (hence the negative crop height).

### 6.4 What the port lacks for this path

Grep of `port/src/engine/*.cpp` for `implement(vm, "<class>", u"<member>"` (and the `video` loop in
`natives_ui.cpp:332-349`), 2026-11-03. Implemented and used here: `gsObjectVariant.hasMember`,
`gsScript.getVariable/getFunction`, `gs3DCamera.reset`, `gsViewPort.addEffect`, `gsVideo(_OGL).new_texture/
new_textureTarget/new_shadowEffect/new_bloomEffect`, `gsTextureTarget_OGL.getTexture`, the `gsControl` methods,
`gsGridPlayer.getWorldPosition` (but see §2.9), `gsGrid.getGridObject`, `gsGridCitizens.findCitizen`.

| Missing native | Used by | Needed for |
|---|---|---|
| `gsGridPlayer` slot-65/66/67 overrides (`mouseMove`/`mouseDown`/`mouseUp`, not script methods) | engine | everything here (`ControlObj::mouseMove` is not virtual, `gui.h:85-87`) |
| `gsGridPlayer.canApplyTool`, `applyTool`, `resetTool`, `getNewItems`, `getReplacedItems`, `getNewObjects`, `getReplacedObjects` | `hoverinfo.script:446-594`, `toolbar.script:196` | the click (event 20 comes from `applyTool`) |
| `gsGridPlayer.editMode` write hook (`setEditMode`: event 17 null, event 21) | toolbar, linepanel | tooltip reset on mode change |
| `gsGridController.getModel`, `hasInstance` | tooltip, camera target, rows | hover text, preview |
| `gsGridItemController.getPos`, `getStats` (+ `name` read hook from the item name, `variation`) | rows | building / road data |
| `gsGridVehicleController.getCitizen`, `getID`; `gsGridCharacterController.getCitizen` | `setCameraTarget`, rows | vehicles, characters |
| `gsGridCitizenController.getID`, `getCharModel`, `getCarModel`, `getHome`, `getWorkplace`, `getStart`, `getTarget`, `getClass`, `getState`, `getCharacter`, `getVehicle` (events `onSpawnCharacter/onSpawnVehicle/onIdle`) | `setInfoTexts`, `citizentarget.script` | citizen data page |
| `gsGridObject.getHomePlaces`, `getWorkPlaces`, `getVisitPlaces` | building rows | building data page |
| `gsGrid.addCamera`, `removeCamera` | `showCamera`/`showData` | can be no-ops while the port renders everything |
| `gsVideo_OGL.renderViewport` | `:329` | no-op is fine (the port renders visible viewports every frame) |
| `gsSound.load`, `play` (only `clone` exists) | query sounds | audio only |
| behaviour, no method: `gsTracerCtrl` / `gsPinCtrl` projection of `targetModule` + `targetPosition` into `targetViewport` | selection arrow, citizen pins, stop tracers | the arrow over the selection |
| behaviour: controller objects themselves (§5) with live `position/globalPos`, `onDestroy`, `itemUpdated` | all | everything here |
| behaviour: `gsTransportStopController.visible` → catchment highlight | stops | stop selection |
| transport fields `passengerCount`, `line`, `capacity`, `attractiveness`, `onSpawn/onUnspawn` | own vehicles, stops | parallel vehicle effort |

---

## 7. Port checklist: hover a building → tooltip; click → data + corner preview

Port status (2026-11-03, `port/src/engine/interaction.cpp`): steps 1-7, 9 and the tracer arrow of 10 are done and checked
in game on Cim2 (tooltip, select, preview, data rows, arrow, minimap pin); step 8 covers the place counts; step 11 picks
line vehicles through `forEachSimVehicle` (unit rectangles from the model bounds, no speed stretch). Two engine defaults
the scripts never set had to be supplied: `gsGrid.viewMode` 0xfe (`L:0x0813d6a0.c:56`) and `gsGridPlayer.editMode` 1.

1. **Player overrides.** Route the desktop's per-tick `mouseMove` and button calls on `MAIN.$player` to native
   `gsGridPlayer` handlers that run the mode logic and then the base `gsControl` behaviour (input.md §3.10). Keep
   `editMode` default 1 and the middle-button skip (`+0x25a`).
2. **Segment.** §1: eye from the camera's view eye, end = far-plane point under the cursor (`near = viewport
   minDistance`, `far = min(viewport, camera max)`, aspect = viewport w/h). Check: `end − eye` points at the cursor's
   ground point (compare with `getWorldPosition`, which must become the y = 0 intersection, §2.9).
3. **Items in `intersectGrid`.** §2.2 candidate rect, §2.5 filters and footprint rectangle, `cellCover` (terrain.md
   §2.1), parent walk; port item records need `x, y, z, sizeX, sizeZ, height·256, flags, type, parent, diagonals`,
   template `displayName`. Check: a building with a `displayName` is hit when the cursor is over its footprint.
4. **Terrain.** §2.6 walk + `intersectCell` with the `0xf00` skip and `viewMode & 2`. Check: hills occlude items
   behind them; the ground around a building yields no item.
5. **Controllers.** §5: `gsGridItemController` / `gsTransportStopController` script objects, cached per item, pose =
   footprint centre, child of the grid, auto-destroyed when unreferenced (or simply kept while referenced), `name` from
   the map's item name, `getModel` = template, `hasInstance` = true, `getPos`, `getStats`.
6. **Hover event.** §3.2 swap rule and event 17 with the controller or null, every tick. Check: hovering a named
   building shows its tooltip (`getItemName`: "Model: name" or the display name) at the cursor; moving off hides it.
7. **Click.** §4.2-4.3: re-pick, event 24, `canApplyTool` true, empty `getNew*/getReplaced*` arrays, `applyTool`
   → `finishTool` → event 20 → re-pick. `setEditMode` events 17/21. Check: `onSelectObject` reaches `clickObject` once
   per click; a quick second click centres the main camera.
8. **Corner HUD rows.** Item paths of `updateInfoItems` (§6.2 step 4): `getHomePlaces/getWorkPlaces/getVisitPlaces`,
   `getFunction("customObjectInfo")`, strings. Check: clicking a building fills name, homes/workplaces, reasons, info.
9. **Corner preview.** Camera `target` follows `target.globalPos` (scene3d.cpp:149 uses `position`: same for grid
   children), `ignoreOrient` view of §6.3, the 327×202 viewport into the 512×256 target with the mask shader and crop.
   Check: the preview shows the building from ~200 units, 57° down, centred, and follows nothing else.
10. **Arrow and pins.** `gsTracerCtrl` above the target in the main viewport; minimap arrow pin (minimap.md §6).
11. **Vehicles and characters** (with the vehicle effort): §2.3/§2.4 tests on the rendered pose, leader resolution,
    `gsGridVehicleController` / `gsTransportVehicle` / `gsGridCharacterController` with per-tick pose, `getCitizen`,
    `hasInstance` = spawned, `onSpawn/onUnspawn`, citizen controllers for the data page. Check: clicking a moving tram
    keeps it centred in the preview; clicking an AI car shows its driver's data page.

---

## 8. Open questions

1. `W:0x100f1b70` (Windows `mouseMoveSelectObject`) pseudocode has no vehicle branch ("Removing unreachable block
   0x100f1c5b"); the Linux listing clearly calls `gsGridVehicle::getController`. Probably a Ghidra stack-aliasing
   artefact (the vehicle out-pointer is `&stack0xffffff44`); worth one look at the Windows listing.
2. The scripts set `$secondaryCamera.position` per target kind (`(0, 1, 0)`, `(0, height·1.5, 0)`), but with a `target`
   neither `getPosition` nor `getTarget` reads the camera's own position. Check in the running game whether the preview
   centres on the building's base (as derived) or 1.5 heights above it (would mean a path not found here, e.g. a
   parent link set by `target`).
3. `gsGrid::move`'s use of the extra cameras (`grid+0x204`): exact effect (division visibility/LOD, vehicle animation,
   item mesh streaming) not traced; matters only once the port culls by camera.
4. `gsGridCharacter` controller pose updates per tick: `L:0x08153b90` writes vtbl `+0x104` on the controller (found by
   grep), not read in detail.
5. `gsTracerCtrl` / `gsPinCtrl` projection and `organize` layout are not specified anywhere yet.
6. Minor, harmless: `gsLineRectIntersect` has no `t ≤ 1` check, so the vehicle/character biases let an object up to
   2 units beyond the far plane be picked (`t − 2/len < 1`); everything else beyond it is rejected by `t < best = 1`.
