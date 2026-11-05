# Grid items: from `map.grid` records to rendered meshes

Scope: how GSystem turns the placed items of a map (`map.grid` section `0x07020000`: roads, tracks, alleys, stops,
stations, buildings, props, trees) into GPU meshes. It covers template loading (`$grid.loadGridObject`), the per-item
transform, appearance variation, the two render paths (merged per grid division vs. one mesh instance per item), LOD,
materials/shaders, and what is never drawn. Terrain geometry, water and selection/data overlays share the division
mesh but are only touched where items depend on them. Builds on [interfaces-3d.md](interfaces-3d.md) (queues, LOD
code, material state), [scene-basics.md](scene-basics.md) (row-vector matrices, conjugate quaternions),
[`../asset-format/`](../asset-format/README.md) and [`../save-format/`](../save-format/README.md) §3.2.

**Citations.** `L:0x<addr>.c:<line>` = `decompiled/Linux/functions/` (real C++ names), `W:0x<addr>.c:<line>` =
`decompiled/GSystem-v2/functions/`. **obs** = read in the pseudocode (or measured on shipped data), **inf** = inferred.
Confidence H/M/L. Offsets are **Linux** unless marked W. Shipped-data counts come from a scan of all object scripts
in `extracted/` and of the Cim2 save (`samples/saves/Cim2/map.grid`, 87 244 items).

Units: `c` = cell size (`grid.scale`, 3.0); `W`,`L` = grid width/length in cells; `B` = division size in cells
= `1 << bucketShift` (16). Heights are in 1/256 cell.

---

## 0. Pipeline in one view

```
script  $grid.loadGridObject(id, "x.object"[, "base.object"[, sx, sz]])     -> gsGridObject template
          .properties (same stem, practically never present), x.script (fields, materials, onLoaded),
          x.object (render + lod1 mesh, baked by an optional 45° transform), x.bmp / x-y.bmp (logic only)
load    map.grid 0x07020210 record -> gsGridItem (0x74 B) in the division that contains its centre
render  template.storeMesh ? merged into the division mesh (one gsMesh per 16×16-cell division, LOD0 near / LOD1 far)
                           : own gsMesh instance = copy of the template mesh, placed by setTransform
```

---

## 1. Templates: `loadGridObject`

### 1.1 Steps (`gsGrid::method_loadGridObject`, L:0x08149f70.c, W:0x100c2b20.c) — obs H

| # | Step | Evidence |
|---|---|---|
| 1 | `new gsGridObject` (0x374 B Linux / 0x3f0 B W); mesh-bake offset `+0x184` := 0 | L:…:76-83 |
| 2 | argc ≥ 3 (base path given): bake rotation `+0x174` := `quat(π/4, (0,−1,0))`; argc ≥ 5: bake scale `+0x168` := `(arg3, 1, arg4)` | L:…:86-103, W:…:87-101, angle/axis from the instruction bytes at L:0x0814a025 (`push 0x3f490fdb`, `gsNegYVector`) |
| 3 | argc ≥ 3: if `<path stem>.script` exists, `path` is aliased to the base file (`gsCreateTempFileReference`), so the own `.script`/`.properties` run but the **mesh comes from the base**; otherwise `path := base` (base script too) | L:…:105-151 (alias semantics inf, M) |
| 4 | `loadFromFile(path, false, false)` = `gsGridObject::loadFromFile` (below) | L:…:153-161 |
| 5 | `idName` (`+0x27c`) := arg0, error if > 31 chars | L:…:167-192 |
| 6 | register in `grid+0x1b4` `map<string, gsGridObject*>` (W `grid+0x1ec`); a duplicate id logs "Dublicate object ID" and replaces the old template | L:…:254-619 |
| 7 | insert 4 rotated `gsGridItemSearchData` keys `{type, variation, connections, connections2, flags & 0xcf0f0, rot}` (road-tool piece lookup; not rendering) | L:…:620-798 |

`gsGridObject::loadFromFile` (L:0x0817f650.c) = `gsObject::loadFromFile` (L:0x08113f80.c) + two bitmaps:

| Order | File | Effect | Evidence |
|---|---|---|---|
| 1 | `<stem>.properties` | gsObject properties (class, draw distances, sub-objects). **Only 2 of 2 709 shipped `.object` files have a same-stem `.properties`** (shipped ones are `*_scene.properties`), so grid templates effectively never load one | L:0x08113f80.c:67; archive scan (obs) |
| 2 | `<stem>.script` | runs with `this` = template: sets fields (§1.3), `setTextureVariation`, `colorMap`, `loadSkeleton`, `onLoaded` handler. Runs **before** the mesh is parsed, so `storeMesh`/`vertexGeneration` affect loading | L:0x08113f80.c:94 |
| 3 | `<path>` chunk `0x02000000` (or `0x02100000` scene) | meshes, materials (`<objdir>/<name>.material`, one material per name per object), locators | L:0x08113f80.c:136-141 |
| 4 | `loaded()` | `gsGridObject::loaded` (L:0x081808b0.c) turns locator pairs `"x-name0"/"x-name1"` into path lines (lane class from `type`; rotated by the bake quaternion), then fires `onLoaded` (scripts set shaders/flags via `getMaterial(name)`) | L:0x08113f80.c:165 |
| 5 | `<stem>.bmp` | 4 rotated per-cell slot masks (`+0x274`) from the R/G/B = 255 channels — pathing only | L:0x0817f650.c:65-260 |
| 6 | `<stem>-y.bmp` | per-vertex Y data (`+0x278`): `R·2 − negHeight·256` (used by `getY`/`fixY` §2.3) | L:0x0817f650.c:262-300 |

The `.object` header node position is **not** applied (`applyTransform` = false); vertices are used as stored, and
shipped grid meshes are centred on the footprint centre with the base at y ≈ 0 (e.g. `small_basic` AABB
(−6,−0.2,−6)…(6,0,6) = 4×4 cells; `pbench01` (−1,0,−0.41)…(1,0.99,0.41)) — obs.

### 1.2 Generated diagonal (`…b`) variants — obs H

`environment.script:60-102`: `loadTree/loadProp($genDiag)` and `loadBuilding($w,$l)` register `<id>b` from
`<model>b.object` when it exists, else generate it from the base mesh. Vertex bake at load
(`gsObject::readVerticesChunk`, W:0x10087fb0.c:51-74, L:0x08111e50.c:68-98, also described in mesh-material.md §2.4):

```
p' = (p ⊙ (sx, 1, sz)) · M(quat(π/4, −Y)) + offset(0)        n' = rot(quat(π/4, −Y), n)       (row vectors)
   = standard column-vector Ry(+45°) applied after the scale
sx, sz = loadBuilding: targetW·√2 / w, targetL·√2 / l   (targetW = round(w/√2) unless given); trees/props: 1, 1
```

The bake applies to every mesh chunk of the file (render and lod1), and to the locators (`loaded()`). A diagonal
item is then placed like any other item with its 4 orientations (8 directions total).

### 1.3 Template fields that matter for rendering

| Script name | Offset L (W) | Default | Use |
|---|---|---|---|
| `width`, `length` | 0x1fc, 0x208 | 1, 1 | footprint in cells (before orientation swap) |
| `height`, `negHeight` | 0x200, 0x204 | 1, 0 | item `+0x3c = height·256`; `-y.bmp` offset |
| `flags` | 0x20c | 0 | `GRID_ITEM_*` (§1.4), OR-ed into the item flags |
| `type` | 0x21c | 0 (NONE) | item kind (§1.4) |
| `diagonals` | 0x218 | 0 | 4 nibbles: diagonal footprint cuts; base walls (§3.2.5) |
| `minY` | 0x358 | −255.0 | clamp in `fixY` (§2.3); effectively off |
| `colorMap` | 0x314 (W 0x378) | null | per-item random tint (§2.5) |
| texture variations 1..4 | 0x360..0x36c (W 0x3c4..) | null | `setTextureVariation(n, material)`; slot = `0x35c + 4n` (L:0x0817db90.c) |
| `storeMesh` | gsObject 0x158 | false | keep CPU copies of the meshes → merged path (§3.1) |
| `vertexGeneration` | gsObject 0x159 | false | foliage normal + triangle-order pass at load (§1.5) |
| `splitSize` | gsObject 0x15c | 0 | split meshes into xz cells (L:0x08118610.c:88); no shipped script sets it |
| `scale` | gsObject 0x160 | 1 | copied to the instance mesh scale; no shipped grid script sets it |
| stored meshes | 0x1ec + 4·lod (W 0x22c) | null | `gsIndexedMesh` copies per LOD (`storeMesh`) |
| render meshes | 0x1e0 vector (W 0x220) | | template's own `gsMesh` (LOD0 + LOD1); never drawn itself (inf, M) |
| `offsetx/offsetz`, `connections*`, `variation`, vehicle fields | | | logic only (road attach point `getRoadXZ` L:0x08181510.c, piece search) |

Field defaults: L:0x08186210.c (gsGridObject ctor), L:0x08115f40.c (gsObject ctor). Names/offsets:
L:0x0817dc40.c, L:0x081121b0.c.

### 1.4 Item kind

The kind is the template's `type` (script `this.type`, constants registered in L:0x08188450.c), plus its flags and
the three render switches. Script categories (`loadTree`, `loadProp`, `loadBuilding`) are UI lists only.

| type | value | shipped render path (scan of all object scripts) |
|---|---|---|
| NONE (props, decals, fences, service parts, vehicle models) | 0 | non-vehicle: 648 `storeMesh` (merged), 176 instance; 299 vehicle models (not grid items in maps) |
| ALLEY, SMALL_ROAD, GROUND_RAMP, ELEVATED_RAMP, LARGE_ROAD, AVENUE_ROAD, EXPRESS_ROAD | 1–7 | merged (`storeMesh`); stops (BLOCK\|STOP) and some bridge ends / express pieces: instance |
| SINGLE/DOUBLE_TRACK, SMALL/LARGE/AVENUE_TRAM, GROUND/SUBWAY/ELEVATED_METRO | 8–15 | merged; stations/stops: instance |
| METRO_STATION, DOCK, HELIPORT | 0x10–0x12 | instance |
| TREE | 0x13 | instance for 14 of 15 (`trees.shader`, 13 with `colorMap`); `grass01` merged (`wave.shader`) |
| BUILDING | 0x14 | **always instance** (874 templates; 223 with `colorMap`, 545 `vertexGeneration`, 11 skeletal) |
| MARKER, SPAWNPOINT | 0x15, 0x16 | instance, hidden unless `grid.showMarkers` (§4) |
| TIMER | 0x17 | instance (fire engines, police cars, crash decal); record low byte = timer, not variation |

Flag constants (L:0x08188450.c): ENABLE_BASE 1, COVER 4, LOCK_GROUND 8, SLOPE_1p16..1p2 0x10..0x80, ERROR 0x200,
CONCAVE 0x400, CROSS_TRACK 0x1000, STOP 0x2000, BRIDGE 0x4000, TUNNEL 0x8000, BLOCK 0x10000, DISABLE_TRAM 0x20000,
SPECIAL0/1 0x40000/0x80000, TEMP 0x100000, OPTIONAL 0x200000, PILLAR 0x800000, PAVEMENT 0x1000000, HIDE_LOD
0x4000000, ALIGN_4 0x8000000, SHOW_CHARACTER 0x80000000. Engine-only item bits used below: 0x800 base walls
needed, 0x400000 hidden, 0x2000000 auto-generated child (pillars, terminal parts, `tennisanim`), 0x40000000
free-placed detail, 0x10000000/0x100 selection highlight (save-format, grid-traffic §1.3; Cim2 counts in §4).

### 1.5 Mesh-load post-processing (`gsObject::readMeshChunk` L:0x081151a0.c, `addMesh` L:0x08118610.c) — obs H

| Switch | Effect |
|---|---|
| chunk tag → LOD | `0x02010000` → LOD 0, `0x02040000` → LOD 1 (also 0x02050000/0x02060000 → 2/3, never shipped) (L:0x081151a0.c:43-65, L:0x08118610.c:76-87) |
| `vertexGeneration` | per submesh (L:0x081151a0.c:80-155, W:0x100882c0.c): (a) every vertex with colour RGB = 0 gets `normal := normalize(position)` (spherical foliage normal, in the baked template space); (b) triangles whose three vertices all have R = 0 and B = 0 are moved behind the others; (c) each part is sorted ascending by `trunc(y0+y1+y2)` (`gsSortPolygonsByY` L:0x08114fe0.c; qsort, unstable). Colour bytes are the file's R,G,B,A |
| `storeMesh` | after creating the render mesh, keep a CPU `gsIndexedMesh` copy per LOD at `0x1ec + 4·lod` (L:0x08118610.c:408-419) |
| render mesh | `addRenderMesh` (L:0x081183c0.c): `createMesh(data, 0x800, ownerItem, lod)`, LOD1 added with `rebuildLod`; min/max draw distance, scale, group copied from the object |

Example (obs): `tree01.object` render 430 v (274 black foliage, 156 white trunk) + lod1 177 v; `bush01` render 69 +
lod1 57; roads mix black and green (0xFF00FF00) but do not use `vertexGeneration`.

---

## 2. Placement

### 2.1 Record → item (`gsGrid::readObjectsChunk` L:0x08146470.c, item ctor L:0x08187fc0.c) — obs H

| Item field | Value | Evidence |
|---|---|---|
| orient `+0x44` | `u16 >> 14` (0..3) | L:0x08146470.c:197 |
| x `+0x2c`, z `+0x34` | record x, z (cells; footprint min corner) | :190-197 |
| y `+0x30` | record y + `(0x200 − heightBase)` (header u16; 0x200 in shipped files → +0) | L:0x08147220.c:51,159 |
| sizeX `+0x38`, sizeZ `+0x40` | `width`, `length`; **swapped when orient is odd** (bit 14) | L:0x08187fc0.c:52-55,80-83; L:0x08146470.c:172 |
| flags `+0x48` | `(record u32 & 0xffffff00) \| template.flags`; `ENABLE_BASE` → also `0x800` | L:0x08187fc0.c:56-58,77-79 |
| variation `+0x6c` | record u32 & 0xff (TIMER: goes to `+0x5c` instead) | L:0x08146470.c:204-207 |
| parent `+0x1c` | record parent; dropped if the parent is OPTIONAL or this is a MARKER | :180-187 |
| y rounding | METRO_STATION or ALIGN_4: `y = (y + 0x7f) & ~0x7f` | L:0x08187fc0.c:73-75 |
| division | `bx = (x + sizeX/2) >> shift`, `bz = (z + sizeZ/2) >> shift` (integer halves) | L:0x08146470.c:190-196 |

After the load, every parentless item with flags `0x40200000` (free-placed OPTIONAL detail) is attached to the nearest
(squared centre distance) non-optional item taller than 2 cells with `cand.y ≤ y ≤ cand.y + cand.height + 0x100`
(roof props) (L:0x08146470.c:100-130).
Unknown type names log "Unknown object" and the record is skipped (:307).

### 2.2 World transform — obs H

Both render paths produce the same placement (instance path `gsGridItem::updateMesh` L:0x081820f0.c:150-161, 222-233,
W:0x100ec350.c:131-138; merged path L:0x08166080.c:2004-2014 + 5028-5031):

```
θ = (orient + 2) · π/2                       (merged path writes orient·π/2 + π: identical)
q = quat(θ, (0,−1,0))  (engine convention)   ≡ standard column-vector rotation Ry(+θ)
T = ( (x + sizeX/2 − W/2) · c,   y · c/256,   (z + sizeZ/2 − L/2) · c )        (sizeX/sizeZ after the swap)
world(p) = p · M(q) + T        i.e.  x' = x·cosθ + z·sinθ,  z' = −x·sinθ + z·cosθ,  then + T
```

| orient | θ | model (x, z) → | model front (−Z) faces |
|---|---|---|---|
| 0 | 180° | (−x, −z) | +Z |
| 1 | 270° | (−z, x) | +X |
| 2 | 0° (identity) | (x, z) | −Z |
| 3 | 90° | (z, −x) | −X |

* Pivot = model origin = footprint centre; no other offset (template `offsetx/offsetz` are road attach points).
* No scale (instance mesh scale = template `scale` = 1).
* Instance path: `setTransform(T, q, relative = 0)`, parent = the grid's scene item (L:0x081820f0.c:189-196).
* Merged path: vertices are transformed on the CPU into division space
  `T_div = ((2(x−x0) + sizeX − B)·c/2, y·c/256, (2(z−z0) + sizeZ − B)·c/2)` and the division mesh is placed at
  `((x0 − (W−B)/2)·c, 0, (z0 − (L−B)/2)·c)` (`x0 = bx·B`), which sums to the same T.

### 2.3 Height: item y vs terrain (`gsGridItem::fixY` L:0x08187760.c) — obs H, practical relevance inf

y is absolute (same units and offset as terrain heights, so ground 0x200 → 6.0 m). The division rebuild re-runs
`fixY` for every item **without** `PILLAR|BRIDGE|TUNNEL|LOCK_GROUND` (L:0x08166080.c:1018-1021); the constructor runs
it when y = −0x8000 or ENABLE_BASE. Terrain samples are the `(sizeX+1)×(sizeZ+1)` grid points of the footprint
(clipped to the map), `h` from `grid+0x18c` `{i16 h, u16 flags}`.

| Condition (first match) | New y | Line |
|---|---|---|
| ENABLE_BASE | keep (if −0x8000: max h; METRO_STATION/ALIGN_4 round up to 0x80; DOCK: 0x200 if max > 0x180 else 0x100); set flag 0x800 when the footprint is not flat at y, else clear it | :66-110 |
| BRIDGE, not COVER/BLOCK/0x40000000 | `(max h + 0xc0) & ~0x7f` | :112-137 |
| TUNNEL, not BRIDGE/COVER/… | `(min h − 0x1c0) & ~0x7f` | :139-162 |
| LOCK_GROUND, not 0x40000000 | mean h over points where `-y.bmp` data exists (a point with terrain flag 0x1000 wins); ALLEY `(y+0x20)&~0x3f`, else `(y+0x40)&~0x7f`; DOCK as above | :165-220 |
| 0x40000000 (free-placed) | keep | :223 |
| has parent | parent y | :224-227 |
| else | mean h (no rounding) | :228-248 |
| always | `y = max(y, round(minY·256))` | :252 |

Shipped saves were written after the same rules, so using the file y directly should match (inf, M). The items whose
y a rebuild actually recomputes are those without PILLAR/BRIDGE/TUNNEL/LOCK_GROUND that are neither ENABLE_BASE nor
free-placed (0x40000000): parented ones take the parent y, the rest the mean terrain height (mostly trees and props).

### 2.4 Parent/child — obs H

Children store **absolute** x/y/z; there is no transform inheritance. The parent only affects: visibility (the root
item's type/flags decide, §4), the colour of `0x2000000` children (§2.5), `fixY` (§2.3), selection. Cim2: 8 225
parented items, e.g. `parking-space`/`street-tree`/fences on roads and platforms.

### 2.5 Appearance variation — obs H

| Mechanism | Path | Rule | Evidence |
|---|---|---|---|
| **Texture variation** (record low byte 1..4) | merged only | for each submesh whose material lacks flag `0x08000000`, use the template's variation material `n` (a full `.material` loaded by `loadMaterial`); submeshes with `0x08000000` (`.material` header byte 0x20 = 0x1F) keep their own. The instance path ignores the variation | L:0x08166080.c:1053-1060, 1983-1989; W:0x100d6ab0.c:653-655 |
| example | | `small_basic`: `Standard_9` (asphalt) and `Standard_8` (marking decal) carry 0x08000000; `walkways` → `road_variation1/2/3.material` (sidewalks-grass / -concrete / roads-tiles). Cim2: variation 1/2/3 on 5 524/3 947/63 items (alleys, roads) | material bytes (obs) |
| **colorMap tint** | instance only | `seed = 101·z + 13·x + y`; `r = (seed·0x687fa + 0x4d3d86) >> 17` (u32, `gsPseudoRandom15` L:0x08108fb0.c); pixel `r mod (w·h)` of the colour map (BMP rows stored **top-down**, bytes R,G,B; L:0x080eecb0.c); colour `0xFFRRGGBB`; materials are cloned per item (`copyFrom(src, shareMaterials = colorMap == null)`) | W:0x100ec350.c:154-156; L:0x080ef430.c; L:0x081820f0.c:174-210 |
| parent colour | instance | flag `0x2000000` with a parent instance: colour := parent's submesh-0 material `colorize` | L:0x081820f0.c:199-208 |
| default | instance | colour still 0 → own submesh-0 material `colorize` | :217-221 |
| apply | instance | `setColor` → `setColorize(colour)` on every submesh material of LOD 0 and 1 (L:0x08181de0.c); visible only on `MATERIAL_COLORIZE` materials: `diffuse.rgb *= mix(1, colorize.rgb, mask)` with mask = normal-map alpha if a normal map exists, else diffuse alpha (`shaders/default/colorize.fragment`) | obs |

**Script colour** (`gsGridItemController.setColor(colour)`, `method_setColor` `L:0x08181f90` = `W:0x100e9430`, obs H):
`gsGridItem::setColor(gsColor(colour), true)` (`L:0x08181de0` = `W:0x100e8db0`) sets the colorize colour of every
submesh of the item's own mesh (`item+0xc`, LOD 0 and 1); with the second argument it then walks the divisions from one
before to one after the **root** item's footprint (follow `+0x1c` parents to the top) and calls `setColor(colour,
false)` on every other item whose root is that root, or which is the root itself, and whose template has no colour map
(`template+0x314 == 0`) (`:51-97`). The game scripts call it for every stop controller in `transportStopCreated`
(`ui.script:323-327`): the scenario's `$companyColor`, else 0xFFFF0000; the shipped sandbox campaign uses 0xFF000000.
**Port** (`engine/interaction.cpp` setColor → `GridObj::itemColorRequests`, applied in `engine/terrain.cpp`
`applyItemColors` after the item sync): the colour becomes the item's tint in the merged division mesh
(`TerrainCache::itemColor`) and the division rebuilds; the port paints merged items too (only COLORIZE parts change).
Berlin's metro entrance in its company blue: `port/run/gaps/stopcolor-compare.png` (left with, right without).

Foliage: `palette.bmp` 256×128 24-bit, used by 13 trees/bushes; `foliage-final.material` has diffuse (1-bit alpha →
ALPHATEST) and a normal map whose alpha is a colour mask (→ COLORIZE), so the tint is masked by the normal-map alpha.

---

## 3. Rendering structure

### 3.1 Path decision (`updateMesh` L:0x081820f0.c:66-110; division update L:0x08166080.c:1013-1116) — obs H

| Template | Path | Instance mesh |
|---|---|---|
| `storeMesh` (LOD0 copy exists) | **merged** into the division mesh | only while selected/highlighted (`flags & 0x10000100`) or in a data view (`viewMode & 0xf0000`): copy + selection/data layer |
| otherwise | **instance** | `createMesh(null, flags = TREE ? 0x4000 : 0, sceneItem)`, `copyFrom(templateMesh LOD0+1)`, `setMaxDrawDistance(800)`, `growRadius(max(sizeX,sizeZ)·c/2)`, `setVisible(inside the map && !(flags & 0x400000))`; HIDE_LOD → `rebuildLod(empty, 1)` |

Divisions (`gsGridDivision`, one per B×B cells): `+0x04` mesh, `+0x0c` item list, `+0x24` active, `+0x25` was
active, `+0x26` near, `+0x28` focusDist, `+0x2c` eyeDist. A division is **active** when its centre is within
`R = max(10·fov, 100)/S` of a grid camera's focus (fov 40 → 400 m; grid-traffic §3); **near** when
`eyeDist < 400 && focusDist < 400` (L:0x08174290.c:23-26). Inactive divisions release their mesh (`clearAllLods`);
item instances exist from load on, independent of activity (inf from the code paths, H).

### 3.2 Merged division mesh (`gsGridDivision::update(parent, x0, z0, lod)` L:0x08166080.c, W:0x100d6ab0.c)

Built twice per (re)activation: `lod 0` (near detail) and `lod 1` (far) (L:0x08174290.c:27-28,
L:0x081743b0.c:45-48). Terrain, water, edges and base walls go into the same mesh; this section covers the items.

#### 3.2.1 Item selection — obs H

| Rule | Line |
|---|---|
| skip `flags & 0x400000` | :1014-1017 |
| `isVisible(viewMode)` false → skip (and `setAlpha(0)` on an existing instance; visible instance items get `setAlpha(1)`) | :1023-1044 |
| source mesh = stored LOD `lod`; if null: HIDE_LOD → skip, else stored LOD0 | :1033-1036, 1973-1977 |
| LOD0 build merges items only when the division is near (`+0x26`); the LOD1 build always | :1046, 1977 |

#### 3.2.2 Grouping and materials — obs H

* One output submesh (`gsIndexedGroup`) per **material**: an item submesh joins the first group whose material
  `equals()` it (same shader, flags, colours, textures, params; interfaces-3d §2.10), else a new group (:1067-1108).
* Material = source material, or the texture-variation material (§2.5).
* Non-tree materials get `MATERIAL_LAYERABLE` (0x400000) OR-ed into the **shared template material** (:1061-1066),
  so selection/data layers can be added later. Trees (type 0x13) are left alone.
* Shader = whatever the material carries (default.shader unless the template's `onLoaded` set one, e.g.
  `wave.shader`, set in 78 template script lines incl. `grass01`). `matrix` uniform = the division's world matrix
  (pure translation), so shader effects that read `pos` see division-space positions.

#### 3.2.3 Vertices — obs H (:2032-2196, W:0x100d6ab0.c:1225-1395)

```
M = [R(θ) | T_div]                                   (θ, T_div from §2.2; θ = orient·π/2 + π)
position' = position · M
normal'   = normal · R                              if normal.y ≤ 0.7 or type == TREE
          = terrainNormal(position')                 otherwise (upward faces of roads, alleys, props …)
colour, uv0 copied;  uv1 = source uv2 (or (0,0) if the mesh has none);  TREE: uv1 = (T_div.y, 0)
indices   = group.firstVertex + group.vertexCount(before) − src.firstVertex + srcIndex
```

`terrainNormal` (W:0x100d6ab0.c:1243-1395, `gsGridGraph::getNormal` L:0x08177720.c): grid point
`(ix, iz) = trunc(p.x/c + x0 + B/2, p.z/c + z0 + B/2)`, `iy = trunc(p.y/c·256)`; take the packed normal of that
point from the first elevated cell plane of the division whose height is within ±0xa0 of `iy` (bridges/decks), else
from the ground plane (`grid+0x19c`) if the ground height there is within ±0xa0; also add the neighbour point(s)
`ix−1`/`ix+1`, `iz−1`/`iz+1` when the vertex lies within 0.1 m of that grid line; packed `0x00XXYYZZ` →
`(b/127 − 1)`; sum and normalise. If no sample exists, use `normal · R`. Effect: road/footpath tops shade exactly
like the terrain under them.

Trees in a merged group (only `grass01` ships this): the group's first vertex is a dummy at `(0,20,0)·M` of the
first tree, normal +Y, never indexed (bound padding, inf) (:2015-2031, :1089-1091).

#### 3.2.4 Division mesh object — obs H (:5017-5031)

`createMesh(data, 0x800, sceneItem, lod)` the first time, `rebuildLod(data, lod)` afterwards; `setMaxDrawDistance(800)`;
`growRadius(B·c·0.707)` (≈ 33.9 m); `setPosition(((x0 − (W−B)/2)·c, 0, (z0 − (L−B)/2)·c), relative = 0)`. After
each build `updateMesh` runs on every item of the division (instances, highlights).

#### 3.2.5 Base walls (ENABLE_BASE items on uneven ground) — obs H

Items with flag 0x800 (set by `fixY`, §2.3) get vertical strips along their footprint perimeter (straight edges and
the diagonal cuts from `diagonals` nibbles) from the terrain height at each perimeter grid point up to the item y,
material `grid.baseMaterial` (`metro/graphics/base.dds`, `engine.script:143-146`), vertex colour 0xFF808080,
normals ±X/±Z or (±√½, 0, ±√½), uv = ((x+z)/4 or z·0.3536, −y/1024) (`createBaseVertex` L:0x08165b90.c; loop
L:0x08166080.c:3737-4249; group :1840-1858).

Enumeration (obs H, `L:0x08166080.c:3737-4249`; the matching Windows body is inside `gsGridDivision::update`
`W:0x100d6ab0`). The loop runs over the division's items with `(flags & 0x400000) == 0`, `isVisible(viewMode)` and
flag 0x800. Item fields: x `+0x2c`, y `+0x30`, z `+0x34`, size `+0x38`/`+0x40` (after the orientation swap), rotated
diagonals `+0x54` (nibbles d0..d3 of the low half). Each strip is `count + 1` perimeter points `P_k`, two vertices per
point (ground `h(P_k)` = the clamped cell height, then the item's y) and two triangles per step
`(b, b+1, b+2, b+2, b+1, b+3)`; the normal is chosen per point by `y < h(P_k)` (sunk) or not (raised):

| Strip | count | P_k | normal sunk / raised | uv |
|---|---|---|---|---|
| diagonal 0 (corner x+sx, z) | d0 | (x+sx−d0+k, z+k) | (−√½,0,√½) / (√½,0,−√½) | z·0.3536 |
| diagonal 1 (x+sx, z+sz) | d1 | (x+sx−k, z+sz−d1+k) | (−√½,0,−√½) / (√½,0,√½) | |
| diagonal 2 (x, z+sz) | d2 | (x+d2−k, z+sz−k) | (√½,0,−√½) / (−√½,0,√½) | |
| diagonal 3 (x, z) | d3 | (x+k, z+d3−k) | (√½,0,√½) / (−√½,0,−√½) | |
| north side | sx−d3−d0 | (x+d3+k, z) | +Z / −Z | (x+z)/4 |
| east side | sz−d1−d0 | (x+sx, z+d0+k) | −X / +X | |
| south side | sx−d2−d1 | (x+sx−d1−k, z+sz) | −Z / +Z | |
| west side | sz−d3−d2 | (x, z+sz−d2−k) | +X / −X | |

Strips with count ≤ 0 are skipped. The winding needs no flip for sunk items: the ground vertex is then above the top
one, which turns the triangles round together with the normal. `fixY` (`L:0x08187760.c:37-109` = `W:0x100ed530`)
clears 0x800 for items not fully inside the map and sets it when the min or max of the clamped (sx+1)·(sz+1) footprint
heights differs from y. **Port** (`engine/terrain.cpp` `baseWalls`, `kBase` group of the terrain mesh, both LODs since
baseMaterial is LAYER): the flag is recomputed at build time by the same rule; `REGS_DEBUG_BASE=1` lists the walled
items. Vienna has 530, mostly houses with walls of a few decimetres and the river quays (`platform*`, 6 m above an
8-cell-deep bed); `port/run/gaps/base-platform.png` (the bridge stairs' foundation over the Danube) and
`base-farm.png`.

### 3.3 Instance items — obs H

* Mesh = clone of the template mesh (submeshes share VBO/IBO; materials shared unless `colorMap`), so per-submesh
  materials/shaders as loaded (+ `onLoaded` changes). Trees: `trees.shader` on the foliage material.
* Skeletal templates (`loadSkeleton`, 11 buildings, `tennisanim`, animals): while the division's `eyeDist < 400` the
  item gets a clone of the template's renderer skeleton, set on its mesh, and the skeleton's first slot is posed at
  `phase·speed` (`gsGridItem::animate` L:0x08181ff0.c; phase loop = `doorOpenLen`, grid-traffic §5.1); otherwise
  the skeleton is removed (bind pose). Door items (flag 0x1000 `GRID_ITEM_CROSS_TRACK`: the level crossing
  `track_double_cross_small`) are posed instead at the clip time of their door state, which their train-track nodes'
  occupancy drives: barriers down over clip 0..1 s, up over 1.167..2.167 s, bind pose (up) at rest
  (grid-traffic §5.1.1).
* Particle emitters (`createEffects`, chimney smoke etc.) are separate scene objects (grid-traffic §5.1).

### 3.4 LOD and distances — obs H (mesh collect W:0x1004a4d0.c, interfaces-3d §1.3)

`D = 800` for both division meshes and item instances (`0x44480000`); `d = |eye − bounding-sphere centre| ×
distanceFactor` (video +0x218, only `debug.script` changes it; default 1.0 inf); `blendLod` = false
(`window.script:43`).

| Mesh | LOD0 | LOD1 | Beyond |
|---|---|---|---|
| instance, template with lod1 | d < 266.4 | 266.4 ≤ d < 880, alpha `((800−d)/800)·10 + 1` above 800 (alpha < 1 → blend queue) | culled ≥ 880 |
| instance, no lod1 | d < 880 (same fade) | – | culled ≥ 880 |
| instance, HIDE_LOD | d < 266.4 | empty | – |
| division mesh | d < 266.4: near build (terrain + items' LOD0) | 266.4 ≤ d < 880: far build (terrain + items' LOD1, else LOD0 unless HIDE_LOD), same fade | culled ≥ 880 (flag 0x800) |

Shadow-map pass: trees (creation flag 0x4000) use their coarsest LOD. Camera: max 310 m from focus, viewport far
600 (`engine.script`).

### 3.5 Materials, shaders, sorting — obs H unless marked

| Topic | Rule |
|---|---|
| shader | per material; `.material` loader → `default.shader`; template scripts replace it (`trees.shader`, `wave.shader`, `propeller`, `rotation`, `move`, `anim-ad`, …) |
| road style flag | `0x08000000` = keep own material when a texture variation is applied (§2.5); nothing else reads it in the item path |
| decals | road markings etc. (`.material` decal bit) → `MATERIAL_DECAL`, depth offset −1, blend queue (interfaces-3d) |
| alpha sort | per submesh by the renderer queues; a merged group is one submesh per material per division, so its key is the group centre. Inside a submesh the only order is the `vertexGeneration` triangle order (§1.5) |
| `MATERIAL_SORTALPHA` | needs `gsObject.sortAlpha` (from `.properties`, not loaded for grid templates) → never set (inf, H) |
| glow | no shipped script or material sets `MATERIAL_GLOW` (grep: 0 hits; loader never sets 0x20, L:0x0811a250.c:100-150) → no glow sprites on items. Lamp "glow" meshes are plain `MATERIAL_ADDITIVE_A` (`oldstreetlamp.script`). Self-illumination = `ILLUMINATIONMAP` × emission (`.material` illum colour × byte 0x26·0.1), constant (no day/night switching found) |
| vertex colour | ignored by `default.shader` (`color.vertex`: `color = 1`); used by `wave.shader` (`max(0, R−B)·2` sway weight) and by the `vertexGeneration` pass |

### 3.6 Trees and foliage — obs H

* Instance, creation flag 0x4000, visible only with `viewMode & 8`, not LAYERABLE.
* `vertexGeneration`: spherical normals on black foliage vertices; trunk triangles first, foliage after, each sorted
  bottom-up.
* Material: ALPHATEST diffuse (alpha-test queue: blend + test `GREATER 0.2`, sorted far→near) + COLORIZE via the
  normal-map alpha; tint from `palette.bmp` (§2.5).
* `trees.shader` (`metro/shaders/trees/trees.shader`) = `default.shader` + vertex `alter` component
  (`alter.vertex`), with `SHADER_TIMER | MATRIX`. In model space (`pos` before the modelview):

```
y = pos.y
m = y·0.002·(1 + sin(timer·0.20862139 + (pos.z + matrix[3][2])·0.021 + (pos.x + matrix[3][0])·0.017))
o = (sin(timer·0.69949524 + (pos.z + matrix[3][2])·0.17), 0, sin(timer·1.30081570 + (pos.x + matrix[3][0])·0.11))·m
o.y = sqrt(max(0, 1 − o·o)) − 1
pos.xyz += o·y            (matrix[3] = world translation; timer = effect timer, interfaces-3d §3.3)
```

  Port (ReGS): trees are merged into the division meshes, where `pos` is division space; the sway would then grow
  with (terrain height + tree height)². The merged tree vertices carry `uv1 = (base height, 0)` (the engine's own
  merged-tree layout, §3.2.3) and the port switches on the subtraction the shipped line has commented out
  (`float y = pos.y - gl_MultiTexCoord1.x;`), in memory when the component loads (`engine/shaders.cpp`
  `portShaderPatch`). The phase uses the vertex's world position instead of model position + item position, and the
  offset is not turned with the item; both only shift the random-looking sway. `gsObject.getMaterial` matches the
  `.material` header name (`tree01`/`tree07` reference `foliage-final.material`, named `foliage`).

  `trees.vertex`/`trees.fragment` (vertex colour ×2, `uv1.x` base height) are referenced only inside the
  commented-out block of `trees.shader`; they are dead code (so mesh-material.md's note "trees shader multiplies by
  gl_Color·2" does not apply at run time).

---

## 4. What is not drawn — obs H

| Case | Rule | Evidence |
|---|---|---|
| hidden items | item flag `0x400000` (persisted): no instance, not merged. Cim2: 82 (pillars, parking spaces, trees, fences) | L:0x081820f0.c:66-67; L:0x08166080.c:1014 |
| markers, spawn points | type 0x15/0x16: no instance unless `grid.showMarkers` (`editor.script:298` sets it). Includes the London `basketball-anim` template (type MARKER) | L:0x081820f0.c:66 |
| view mode (`isVisible`, L:0x08185c50.c:19-31) | TEMP (0x100000) items always visible; TREE needs `viewMode & 8`; otherwise use the **root** parent: underground view (`& 0x100`) + root TUNNEL → visible; root type 2..0x12 (roads … heliport) needs `& 0x10`; everything else (NONE, ALLEY, BUILDING, markers, timers) needs `& 0x20`, except root ALIGN_4 → `& 0x10`. `GRID_VIEW_DEFAULT` = 0xfe shows all; `GRID_VIEW_UNDERGROUND` = 0x180 shows tunnel-rooted items only, and only in divisions with tunnel items ([underground.md](underground.md) §3-4) | obs |
| outside the map | instance `setVisible(false)` if x<0, z<0, x+sizeX>W or z+sizeZ>L | L:0x081820f0.c:241-246 |
| HIDE_LOD | not in LOD1 band / far division build | §3.4 |
| locators | path/lane markers (`s-a0`, `e-b1`, `t-…`, `c-…`) become graph lines; never meshes | L:0x081808b0.c |
| `.bmp`, `-y.bmp` | logic data | §1.1 |
| template meshes | the template's own `gsMesh` is not shown (only copies are) | inf, M |
| selection layers | `flags & 0x10000100` (positive/negative material) and data views (`viewMode & 0xf0000`, `dataMaterial`) are overlays; none in a normal saved game (Cim2: 0 items with 0x10000000/0x100) | L:0x081820f0.c:93-147 |

---

## 5. Port checklist (render a shipped map's items like the original)

1. **Templates.** Run the envset scripts; implement `loadGridObject(id, path[, base[, sx, sz]])`: same-stem `.script`
   before the mesh, `.object` render + lod1, per-name `.material`, `onLoaded`; base-path variant = base mesh baked
   with `(p⊙(sx,1,sz))·Ry(+45°)` (normals rotated). Store `width, length, height, flags, type, diagonals, storeMesh,
   vertexGeneration, colorMap, texture variations 1..4`.
2. **vertexGeneration pass** (spherical normals for RGB=0 vertices; R=B=0 triangles last; Y-sort) — needed for tree,
   building and prop shading.
3. **Items.** From each record: orient = `u16>>14`; swap width/length for odd orient; flags = record | template;
   y += `0x200 − heightBase`; ALIGN_4/METRO_STATION y rounding; variation = low byte (not for TIMER).
4. **Transform** `p·Ry((orient+2)·90°) + ((x+sizeX/2−W/2)·c, y·c/256, (z+sizeZ/2−L/2)·c)`.
5. **Skip** flag 0x400000, MARKER/SPAWNPOINT, out-of-map items; apply `isVisible` with `viewMode = 0xfe`.
6. **Merged items** (`storeMesh`): bake into one mesh per 16×16 division and LOD, grouped by material equality,
   texture variation on non-0x08000000 submeshes, upward normals from the terrain normal (or keep rotated normals as a
   first approximation), LOD0 from the items' LOD0 meshes, LOD1 from lod1 meshes (fallback LOD0 unless HIDE_LOD).
   A per-item instanced renderer is acceptable if it reproduces the same materials and normals (inf).
7. **Instance items**: own mesh (LOD0/LOD1), D = 800 (LOD switch 266.4, fade 800–880, cull 880), HIDE_LOD drops LOD1,
   trees: shadow LOD = coarsest.
8. **Tint**: colorMap colour per item (seeded hash §2.5) → `colorize` on COLORIZE materials (cloned per item);
   0x2000000 children copy the parent's colour.
9. **Shaders**: `default.shader` combiner already ported; add `trees.shader` alter (wind), `wave.shader` (vertex-colour
   sway) and the other `metro/shaders/effects/*` alters used by props; supply `timer` and `matrix` uniforms.
10. **Later**: skeletal item animation near the camera, emitters, selection/data layers (all done since; base walls
    in §3.2.5).

---

## 6. Open questions

* Exact semantics of `gsCreateTempFileReference` for diagonal templates that ship their own `<id>b.script` (step 3 of
  §1.1): inferred that the b-script runs with the base mesh.
* The terrain-cell edge walls built when `viewMode & 2` is clear (L:0x08166080.c:3292-3736): not traced (the
  underground view clears the cover, so none are built there; underground.md).
* Where the packed terrain normals (`grid+0x19c`) and elevated cell-plane normals are computed (terrain/graph code).
* Whether the template's own render mesh is ever visible (its visible bit `+0x4c & 2` was not traced for templates).
* `distanceFactor` default (assumed 1.0).
* The London `basketball-anim` template has type MARKER and therefore never renders in game; whether that is
  intended (the tennis variant has its type commented out) is a content question.
