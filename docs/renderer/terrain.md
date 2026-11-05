# Terrain rendering (gsGrid / gsGridDivision)

How GSystem turns `map.grid` terrain (`0x07010000`, see [../save-format/README.md](../save-format/README.md) §3.1 and
[../save-format/terrain-characters-citizens.md](../save-format/terrain-characters-citizens.md) §2.1) into meshes,
materials and shader inputs. Builds on [interfaces-3d.md](interfaces-3d.md) (queues, LOD, material state, shader
contract) and [scene-basics.md](scene-basics.md) (conventions, combiner).

**Citations.** `L:0x<addr>.c:<line>` = `decompiled/Linux/functions/` (real C++ names), `W:0x<addr>.c:<line>` =
`decompiled/GSystem-v2/functions/` (Windows GSystem.dll), `E:<line>` = `extracted/metro/scripts/engine.script`,
`S:<file>` = `extracted/metro/shaders/<file>`. **obs** = read in the code, **inf** = inferred. For the `gsGrid` members used
here, Windows = Linux + 0x38 for the size fields, material slots and cell planes (up to `L:+0x19c`; cells
`L:+0x18c` = `W:+0x1c4`, the offset the save docs use) and Linux + 0x20 from the sub-objects on (graph `L:+0x1e8` =
`W:+0x208`, viewMode `L:+0x1fc` = `W:+0x21c`). The Windows builder `W:0x100d6ab0` (= `gsGridDivision::update`)
and its helpers match the Linux code line for line wherever checked (§3, §7, §8).

Notation: `W×L` grid size in cells, `cs` = cellSize (3.0, header), `n = 1 << bucketShift` (header value 4, so n = 16),
`h(x,z)`/`f(x,z)` = cell height (i16, 1/256 cell) and flags (u16) with x clamped to `[0,W−1]` and z to `[0,L−1]`.
A **corner** `(gx,gz)`, `0 ≤ gx ≤ W`, `0 ≤ gz ≤ L`, takes its data from cell `(gx,gz)` (clamped): heights are corner
heights.

---

## 1. Overview (obs)

| Item | Value | Evidence |
|---|---|---|
| Chunking | the grid is split into `ceil(W/n) × ceil(L/n)` **divisions** of n×n cells (`gsGridDivision`, 0x38 B), one `gsIMesh` each | `L:0x0813eb50.c:61-80` (`setSize`), `L:0x08147220.c:50` (n from header bucketShift) |
| Mesh per division | `video->createMesh(data, flags 0x800, parent, lod)` or `mesh->rebuildLod(data, lod)`; `setMaxDrawDistance(800)`; `growRadius(n·cs·0.707)`; `setPosition(((DX − ((W−n)>>1))·cs, 0, (DZ − ((L−n)>>1))·cs))` where DX, DZ = first cell of the division | `L:0x08166080.c:5015-5031`, `W:0x100d6ab0.c:4458-4475` |
| LODs | each division builds **two** LODs: pass `lod 0` (detailed, plus water, walls, highlights) and `lod 1` (coarse) | `L:0x08174290.c:27-28` |
| Submeshes | one indexed group per material: terrain, 8 yard, 8 field, highlight ±, base walls, edge walls, water, water edge (+ the division's static items, batched into the same mesh) | `L:0x08166080.c:1220-1932` |
| Vertex data | `Vertex36` {pos f32×3, normal f32×3, colour u32 `gsColorABGR`, uv0 f32×2} + separate uv2 f32×2 array (`TEXCOORD1`); u32 indices; triangles | `L:0x08166080.c:1938-1962` |
| World position | `world = ((gx − W/2)·cs, h·cs/256, (gz − L/2)·cs)` for even W, L (all shipped maps: 768–1280) | local + mesh position above |

Colour packing: the u32 is `A<<24 | B<<16 | G<<8 | R`, i.e. bytes R,G,B,A in memory = `glColorPointer(4,
UNSIGNED_BYTE)` order. The terrain shader reads `gl_Color.r`, the water shader `gl_Color.a`; both match this layout
(obs from what each writer stores and each shader reads).

---

## 2. What the builder reads, and when it runs

### 2.1 Inputs

| Input | Linux / Windows | Notes | Conf |
|---|---|---|---|
| cells `{i16 h, u16 f}` | grid `+0x18c` / `+0x1c4` (`gsGridCellPlane+4`) | loader adds `0x200 − heightBase`, clears `0x4000` and `0x0f00` (`L:0x08147220.c:51,118,125`) | H obs |
| per-cell normal override (u32) | `+0x19c` / `+0x1d4` (`gsGridCellPlane+0x14`) | `0x00XXYYZZ`, component = `byte/127 − 1`; written by `gsGridItem::fixGrid` for cells under items (`L:0x08184890.c:440-447`); **all zero without items**, not saved | H obs |
| cover quadrants `f & 0x0f00` | in the flags | `0x100` = north (z−) triangle, `0x200` east (x+), `0x400` south (z+), `0x800` west (x−) of the cell's 4-triangle fan; recomputed from items (`fixGrid`, only when viewMode & 0x20) | H obs |
| `viewMode` | `+0x1fc` / `+0x21c`, default `0xfe` (`L:0x0813d6a0.c:56`) | bits used here: `0x02` draw terrain surface, `0x04` draw water, `0x20` yards/fields + cover; `GRID_VIEW_UNDERGROUND` = `0x180` (none of them) | H obs |
| water level | constant **−0x140** (−1.25 cells = −3.75 m) | hard-coded in the builder and in `endUpdate` | H obs |

### 2.2 Water flag maintenance (obs)

* **Dry-out** (in the builder, lod-0 pass): a cell with `0x2000` whose four corners `h(x,z), h(x+1,z), h(x,z+1),
  h(x+1,z+1)` are all `≥ −0x140` loses `0x2000` and is built as land (`L:0x08166080.c:481-539`). So: **water quad iff
  `f & 0x2000` and min(4 corners) ≤ −0x141.** This mutates the grid; a port should run it once over all cells after
  load (the engine applies it per division as divisions are built, so neighbours read across a division border may
  briefly still carry the flag).
* **Flood** (`gsGrid::endUpdate`, edits only): from every water cell in the edited area, neighbours with any corner
  `< −0x140` become water, breadth-first (`L:0x0813deb0.c`). Not needed to render a loaded save.

### 2.3 Rebuild triggers (obs unless noted)

| Trigger | Effect | Evidence |
|---|---|---|
| load (`readGridChunk`) | `setSize`, `beginUpdate(all)`, read cells, `endUpdate(all)` | `L:0x08147220.c:49-90` |
| edit of cells `[x,x+w)×[z,z+l)` | `beginUpdate` increments the lock count (`div+8`) of every division touching the area ±1 cell, `endUpdate` decrements it and rebuilds at 0 (border-neighbour divisions too) | `L:0x0813ddf0.c`, `L:0x0813deb0.c` |
| `beginHoldUpdates` | division marked −1, rebuilt in `endHoldUpdates` | `L:0x0813f150.c` |
| viewMode change in bits `0xfe` | full begin/end update + `fixGrid` of every item (recomputes cover bits and normal overrides) | `L:0x0813ef00.c` |
| camera moves (`gsGrid::move`) | per camera: divisions whose centre is within R of the camera target are **in range** (`div+0x24`); R = max(10·camera distance, 100)/distanceFactor (inf); out-of-range divisions are cleared (`clearAllLods`). **Near** (`div+0x26`) = both camera distances `div+0x28`, `+0x2c` < 400 (inf: distance to the centre at y 0, and to the centre 25 m above ground). A change of near status rebuilds lod 0; becoming in range builds both | `L:0x08148e60.c:97-98,121-127,237-278`, `L:0x081743b0.c:28-48` |

Only in-range divisions are ever built (`L:0x08174290.c:21`).

### 2.4 Cover quadrants and normal overrides from items (`gsGridItem::fixGrid`, `L:0x08184890.c:250-420`) — obs H

Run per item (file order; later items overwrite normals) when it is placed, on load and on viewMode changes:

| Step | Rule |
|---|---|
| flags | `cc` = item flags (record \| template); tram tracks (types 10–12) with a parent: `cc = (cc \| 0xc000) & parent.flags` |
| footprint | cells `[x, x+sizeX) × [z, z+sizeZ)` (sizes swapped for odd orient), only if inside the map |
| diagonals | item `+0x54` = template `diagonals` with each 16-bit half rotated right by `4·orient` (item ctor `L:0x08187fc0.c:120-126`) |
| quadrants per cell | `0xf00`, or for diagonal items `cellCover(x, z)` (`L:0x08183120`): CONCAVE items cover the cell (`0xf00`) unless `max(z, sx−1−x) < d0`, `max(sx−1−x, sz−1−z) < d1`, `max(sz−1−z, x) < d2` or `max(z, x) < d3` (then 0); otherwise the 4 grid points are tested with `pointCover` (`L:0x08182ff0`, the cut sizes in the nibbles, extra concave nibbles 4–7) and the quadrants N/E/S/W whose two points are both inside are covered; a lone quadrant only at the tip of a cut (N: `x = d2−1 = sx−d1`; E: `z = d3−1 = sz−d2`; S: `x = d3−1 = sx−d0`; W: `z = d0−1 = sz−d1`). A result of 0 skips the cell |
| cover | `cc & COVER` and `viewMode & 0x20` → cell flags `\|=` quadrants |
| normal override | `cc & (BRIDGE\|TUNNEL\|ENABLE_BASE) == 0`, no `-y.bmp` data, and (`flags & (ENABLE_BASE\|COVER)` or type 1–0x12) → cell override := packed up normal `0x7ffe7f`, or for SLOPE_1p16..1p2 (s = 1/16 … 1/2, not for BRIDGE/TUNNEL cover items) `normalize` of orient 0 `(−s,1,0)`, 1 `(0,1,s)`, 2 `(s,1,0)`, 3 `(0,1,−s)` (diagonal items: `(∓s/2, 1, ±s/2)` per orient 0..3 = `(−,+)`, `(+,+)`, `(+,−)`, `(−,−)`), packed `round((c+1)·127)` per byte |
| elevated items | items with `-y.bmp` data or BRIDGE/TUNNEL/ENABLE_BASE write elevated cell planes (`getExtraSlot`) instead; not traced |

In Cim2 this covers about 465 000 cell quadrant sets. Without it the terrain stays under roads and z-fights with the
road decks.

---

## 3. Corner vertex `V(gx,gz)` (`gsGridDivision::createVertex(gsVertex&, gsUVCoord&, …)`)

`L:0x081649b0.c` (= `W:0x100d5290.c`). Let `(x,z)` = `(gx,gz)` clamped to the cells, `f = f(x,z)`, and the **4 cells
around the corner** `Q = {(x,z), (x−1,z) if x>0, (x,z−1) if z>0, (x−1,z−1) if x>0 && z>0}`.

| Attribute | Formula | Evidence | Conf |
|---|---|---|---|
| local pos | `((gx−DX)·cs − n·cs/2, h(x,z)·cs/256, (gz−DZ)·cs − n·cs/2)` (x/z unclamped, height clamped) | `:63-80` | H obs |
| `covered` | any cell of Q has a non-zero normal override | `:81-91` | H obs |
| normal, not covered | `normalize((h(x−1,z) − h(x+1,z))/512, 1, (h(x,z−1) − h(x,z+1))/512)`; a neighbour outside the map is replaced by the cell itself (so border slopes are halved); both terms 0 → `(0,1,0)` | `:92-128`, `W:0x100d5290.c:79,88` | H obs |
| normal, covered | `normalize(Σ decode(o))` over the non-zero overrides o of Q | `:129-170` | H obs |
| uv0 (`TEXCOORD0`) | `(gx·0.125, gz·0.125)` | `:172-173`, `W:…5290.c:138-139` | H obs |
| colour R ("ruin") | `f & 0x40` (timeline-preview pavement): `covered ? 0 : 255`; else `(f & 0x28)` (pavement/field) or covered: 0; else `(f & 7) << 5` (wild-ground variant 0..7 → 0..224). G = B = 0, A = 255 | `:174-185`, `W:…5290.c:140-151` | H obs |
| uv2.x (`TEXCOORD1.x`) | if `f & 0x20` and not covered: **field weight** = 1 iff every cell of Q has `0x20` and the same `f & 7`, else 0. Otherwise: **sea-floor weight** = 1 iff not covered and any cell of Q has `0x2000`, else 0 | `:186-239` | H obs |
| uv2.y (`TEXCOORD1.y`) | **cliff weight** = covered ? 0 : `clamp(4 − 4.2·normal.y, 0, 1)` (0 for slopes up to n.y 0.952, 1 from n.y 0.714) | `:240-252` | H obs |

All attributes are a pure function of the corner position, so every material group that shares a corner gets the same
values (each group keeps its own copy).

### 3.1 Centre vertex `C` of cell (x,z) (`createVertex(gsIndexedMesh const&, …)`)

`L:0x08165160.c` (= `W:0x100d5a40.c`). Corners TL `(x,z)`, TR `(x+1,z)`, BL `(x,z+1)`, BR `(x+1,z+1)`; a corner is
absent when both quadrants touching it are covered (§4).

| Case | Result | Evidence |
|---|---|---|
| position / uv0 | `TL.pos + (cs/2, 0, cs/2)`, `TL.uv0 + 0.0625` (from BR with minus signs if TL is absent) | `:88-131` |
| all four corners | `y` = mean of the 4 y; normal = normalize(sum of 4); colour = bytewise mean (`sum>>2`); uv2.x = mean of 4 | `:157-197` |
| a corner K absent | `y = h(K)` (the **missing** corner's height, read from the cells); then if TL and BR exist → normal/colour/uv2.x = mean of that pair (`>>1`), else if TR and BL exist → mean of that pair, else normal = override(x,z) decoded or (0,1,0), colour = TL's (else BR's), uv2.x = 0 | `:88-156, 199-237` |
| always | uv2.y = `clamp(4 − 4.2·n.y, 0, 1)`; if uv2.x == 0 and `f(x,z) & 0x20` → uv2.x = 1 (isolated field cells get a field centre) | `:250-260` |

With no items (cover bits 0) only the "all four" row applies.

---

## 4. Cell triangulation and index layout (obs, `L:0x08166080.c:2205-2372`)

Per cell and material group, vertices are allocated lazily in row-major cell order through an (n+1)² corner map
(shared within the group), then the centre vertex. Corner skip rules (covered cells only):
TL skipped if `(f & 0x900) == 0x900`, TR if `0x300`, BL if `0xc00`, BR if `0x600`.

| Case | Triangles (CCW seen from +y) |
|---|---|
| lod 1 and `f & 0xf00 == 0` | `TL,BL,TR` · `TR,BL,BR` (2 tris, no centre) |
| lod 0, or any cover bit | fan around C, one triangle per **uncovered** quadrant, in this order: N `TL,C,TR` (skip if 0x100) · W `BL,C,TL` (0x800) · E `TR,C,BR` (0x200) · S `BR,C,BL` (0x400) |
| `f & 0xf00 == 0xf00` | nothing (cell fully under an item) |

A plain 16×16 division: lod 0 = 289 corners + 256 centres, 1024 triangles; lod 1 = 289 vertices, 512 triangles.

---

## 5. Material groups: which cells go where

Material pointers are `gsMaterialVariant` (`+0x18` = `gsIMaterial`). Offsets Linux / Windows; script setup from
`engine.script`.

| Group | Cells / geometry | lod | Material | Evidence |
|---|---|---|---|---|
| **terrain** | viewMode & 2; not fully covered; `(f & 0x40) ∨ ¬(f & 0x28) ∨ ¬(viewMode & 0x20)`; lod 0 only if the division is **near** | 0, 1 | `material` `0x120/0x158` if viewMode & 0x20, else `earthMaterial` `0x124/0x15c` with uv0 × 0.5 | count `L:0x08166080.c:822-1000`, group `:1217-1245`, material `:1232-1236`, uv `:2360-2370` |
| **yard[i]** (pavement) | viewMode & 0x20; `(f & 0x48) == 8`; `f & 7 == i` | 0 (near), 1 | `getYardMaterial(i)` `0x128+4i / 0x160+4i` | `:832, 1272-1530, 2503` |
| **field[i]** | viewMode & 0x20; `(f & 0x60) == 0x20`; `f & 7 == i` | 0 (near), 1 | `getFieldMaterial(i)` `0x160+4i / 0x198+4i` | `:833, 1531-1787, 2676` |
| **water** | §7 | 0 | `waterMaterial` `0x158/0x190` | `:1886-1910` |
| **water edge** | §7.3 | 0 | `waterEdgeMaterial` `0x15c/0x194` | `:1912-1935` |
| **edge walls** | map border, §8.1 | 0 | `edgeMaterial` `0x14c/0x184` | `:1175-1190, 1863-1885` |
| base walls | underground cover edges; item foundations, §8.3 | 0 | `baseMaterial` `0x148/0x180` | `:1840-1862` |
| earth plane | underground only (`¬(viewMode & 2)`), §8.3 | 0 | `earthMaterial` | `:567-590, 1247-1266` |
| highlight + / − | `f & 0x4000`, `0x8000` selects negative; same fan, `createVertex` | 0 | `positiveMaterial` `0x150/0x188`, `negativeMaterial` `0x154/0x18c` | `:658-800, 1788-1839` |
| data layer | clone of every `MATERIAL_LAYERABLE` submesh (`gsIMesh::addLayer`) with `custom0..7` = 8 data points | 0 or 1 | `dataMaterial` `0x180/0x1b8` | `L:0x081635d0.c` |
| — | `reservedMaterial` `0x184/0x1bc` is created and scripted but **no renderer reader was found** | | | `L:0x0813af10.c`, grep |

Pavement and field cells therefore never appear in the terrain group in the normal view; the yard/field triangles
butt against terrain triangles with independent vertices (hard material edge, blended only by the shared corner
attributes).

### 5.1 Material setup (all from `engine.script`, obs)

`setTexture(0, …)` also sets `MATERIAL_DIFFUSEMAP` (0x4000) (`L:0x08110ca0.c`); other slots set no flag.

| Material | Lines | Shader | Units 0..n | Flags | Colours / other |
|---|---|---|---|---|---|
| `material` | E:47-58 | `terrain/terrain.shader` | terrain, terrain-damage-combined, seafloor, cliff (DXT5, alpha used), cliff_n | DIFFUSEMAP\|LAYERABLE\|NORMALMAP | amb/diff 0xFF808080, depthOffset 1 |
| `earthMaterial` | E:59-63 | default | earth | DIFFUSEMAP\|LAYER | 0xFF808080, depthOffset 1 |
| `reservedMaterial` | E:64-68 | default | earth | DIFFUSEMAP\|LAYERABLE | 0xFF808080, depthOffset 1 |
| yard 0..3 | E:83-110 | default | pavement_test / ind_concrete / paving / paving_checker | DIFFUSEMAP\|LAYERABLE | 0xFF808080, depthOffset 1 |
| yard 4..7 | E:111-142 | default | paving_arches / concreteplates / concreteplates03 / concreteplates04, + `_n` normal map in unit 1 | + NORMALMAP | same |
| `baseMaterial` | E:143-146 | default | base | DIFFUSEMAP\|LAYERABLE\|LAYER | 0xFF808080 |
| `edgeMaterial` | E:147-151 | default | edge | DIFFUSEMAP\|LAYERABLE\|LAYER | 0xFF808080, depthOffset 2 |
| positive / negative | E:152-159 | `selection/selection.shader` | selection | DIFFUSEMAP\|DECAL\|LAYER | diffuse 0x5000FF00 / 0x50FF0000 |
| `waterMaterial` | E:160-169 | `water/water.shader` | water-n, water-n2, `textures/envmap.dds` (cube) | DIFFUSEMAP\|ZERO_RADIUS\|ALPHABLEND\|LAYER | amb/diff 0xAA0C1010, specular 0x606060, glossiness 128, reflection 0.2 |
| `waterEdgeMaterial` | E:170-174 | `water/waterEdge.shader` | — | ZERO_RADIUS\|ALPHABLEND\|TWOSIDED\|LAYER | amb 0xC0102040, diff 0xC0102030, glossiness 64 |
| `dataMaterial` | E:175-177 | `selection/data.shader` | — | DECAL\|LAYER | diffuse 0xC0FFFFFF |
| field 0..3 | E:178-213 | `terrain/field.shader` | terrain, terrain-damage-combined, field01 (0,1) / field02 (2,3), cliff | DIFFUSEMAP\|LAYERABLE, + SWAP_UV for 1 and 3 | diffuse 0xFF808080, **no depthOffset** |

Field materials 4..7 are never scripted and keep engine defaults (default shader, no texture); shipped maps use field
styles 0..3 only. Texture paths are `metro/graphics/*.dds` in `metro.gs` except the env map; all are DXT1 except
`cliff.dds` (DXT5, 1024²). The `showGrid` key toggle swaps `material` to `terrain/grid.shader` + `terrain2.dds`
(`metro/scripts/controls.script:292-301`); it is a debug view (height-band colours from `alter-color.*`), not needed
for normal rendering.

---

## 6. Shader programs and pass state

### 6.1 Terrain group (`terrain.shader` on top of `default.shader`)

Default `onProgram` runs first (POSITION|NORMAL|TEXCOORD0, +TANGENT for NORMALMAP, TEXTURE0|1), then
`terrain.shader` adds `TEXCOORD1|COLOR`, `MATRIX`, `TEXTURE0..4` and replaces the components `uvcoord`
(`terrain/uvcoord.vertex`), `color` (`terrain/color.vertex`) and fragment `maps` (`terrain/maps.fragment`)
(`S:terrain/terrain.shader:5-21`). The other default components (geometry with tangent-space normal map, lighting,
shadow, fog, colour.fragment) are unchanged. `terrain.vertex`/`terrain.fragment` are dead (the program that used them
is commented out). Tangents are generated by the mesh upload (interfaces-3d §3.6) because NORMALMAP requests TANGENT.

```glsl
// vertex
v_texCoord   = gl_MultiTexCoord0 * 0.5;                    // = (gx, gz) / 16: terrain textures repeat every 16 cells (48 m)
v_texCoord2  = vec3(gl_Color.r, gl_MultiTexCoord1.x, gl_MultiTexCoord1.y);   // (ruin, seafloor, cliff)
gl_FrontColor = vec4(1.0);                                 // vertex colour is NOT a tint
// fragment (maps_input), D=terrain, R=damage, S=seafloor, C=cliff (all at v_texCoord.xy)
k    = (C.a - 0.7) * 0.5;                                  // cliff.dds alpha = noise for all transitions
sea  = v_texCoord2.y + k;
ruin = v_texCoord2.x + k + 1.5 * v_texCoord2.z;
cl   = smoothstep(0.45, 0.55, v_texCoord2.z + k);
diffusemap = D * smoothstep(0.8, 0.0, ruin) + R * smoothstep(0.0, 0.6, ruin);
diffusemap = mix(diffusemap, S, smoothstep(0.3, 0.7, sea));
diffusemap.rgb = mix(diffusemap.rgb, C.rgb, cl);
normalmap = mix(vec4(0.5,0.5,1,1), texture4(v_texCoord.xy), cl);   // only when the pass has lights
```

Then `color.fragment`: `rgb = (v_ambient + diffuse) · diffusemap.rgb` (+specular if `MATERIAL_SPECULAR`, not set),
`a = diffusemap.a` (DXT1 → 1), fog mix. Material ambient/diffuse 0.5 grey, sun `diffuse 0xFFFFF0D0 × magnitude 2.5`
(`E:39-42`).

**Port note:** `smoothstep(0.8, 0.0, x)` has edge0 > edge1, which GLSL leaves undefined; GSystem relied on the usual
`t = clamp((x−e0)/(e1−e0))` formula. Exact rewrite: `1.0 − smoothstep(0.0, 0.8, x)` (also in `field-maps.fragment`).

### 6.2 Field groups (`field.shader`)

Adds `TEXCOORD1|COLOR`, `TEXTURE0..3`, components `uvcoord` (`field-uvcoord.vertex`), `color` (`terrain/color.vertex`),
`maps` (`field-maps.fragment`); no MATRIX, no normal map (`S:terrain/field.shader`).

```glsl
v_texCoord = vec4(SWAP_UV ? tc0.yx*0.5 : tc0.xy*0.5, tc1.x /*field*/, gl_Color.r /*ruin*/);
k    = (C.a - 0.7) * 0.7;
ruin = v.w + k + 1.6 * v.z;      fa = v.z + k;
diffusemap = D*smoothstep(0.8,0.0,ruin) + R*smoothstep(0.0,0.6,ruin);
diffusemap = mix(diffusemap, F /*field01|02*/, smoothstep(0.4, 0.6, fa));
```

So a field shows terrain at its outer corners, dirt in the transition and the field texture inside; SWAP_UV turns the
furrows 90°. Field vertices ignore uv2.y (no cliff rock on fields).

### 6.3 Default-shader groups

| Group | uv0 seen by the shader | Texture repeat |
|---|---|---|
| yard | `(gx, gz)·0.125` | 8 cells (24 m); styles 4–7 also normal-mapped (unit 1, generated tangents) |
| edge wall | §8.1 | 1 per division edge horizontally, 1 over the full wall height |
| earth (underground / no-0x20 terrain) | §8.3 / `(gx,gz)·0.0625` | |
| base wall | §8.3 | |

The default `color.vertex` writes `gl_FrontColor = 1`, so vertex colours are ignored by these groups.

### 6.4 Pass state per group (from interfaces-3d §1.2-1.3, §2.10; obs there)

| Group | Queue | Depth / blend | Polygon offset | Shadow map |
|---|---|---|---|---|
| terrain, yards, earth | opaque | write on | `glPolygonOffset(2,2)` (depthOffset 1 → 2·d) so road/item decks on the ground win | drawn |
| fields | opaque | write on | **none** | drawn |
| edge walls | opaque | write on | (4,4) | drawn |
| water | blend | write off, `SRC_ALPHA, 1−SRC_ALPHA`, alpha func > 0.001 | (−2,−2) (blend path) | **not drawn** (`water.shader` returns no components for `SHADOWMAP_RENDER` → invalid variant → skipped) |
| water edge | blend, **cull off** (TWOSIDED) | same | (−2,−2) | not drawn (components `null`) |

`MATERIAL_ZERO_RADIUS` sets the submesh radius to −100 (`build`, interfaces-3d slot 26), so the blend-queue key
`(d − r)·|d − r|` treats water as 100 m farther: it is drawn before other blended submeshes at similar distance.

### 6.5 LOD selection for a division mesh (obs, `W:0x1004a4d0.c:158-330`, `W:0x100494e0.c:55`)

D = 800 (× video `distanceFactor`, distance camera → mesh centre):

* d ≥ 1.1·D = 880 → whole mesh culled (mesh flag 0x800).
* lod 1 (terrain 2 tris/cell, yards, fields) drawn for 0.333·D = 266.4 ≤ d < 880, fading out between 800 and 880;
  lod 0 (fan terrain) for d < 266.4.
* A submesh whose material has **`MATERIAL_LAYER`** (0x800000; submesh `+0xbc`) is drawn at alpha 1 whenever its LOD
  is reached in the walk from coarsest to finest, i.e. lod-0 LAYER submeshes (**water, water edge, edge walls,
  earth, base walls**) are drawn at every distance < 880 even though lod 0 itself is not selected.
* lod 0 of a division that is not near holds no terrain/yards/fields (they are skipped in pass 0,
  `L:0x08166080.c:826`), so it never shows holes: near (< 400 m) is a superset of the lod-0 band (inf: both measure
  roughly camera → division centre).

`MATERIAL_LAYERABLE` (0x400000) only marks submeshes that `addLayer(material, layerableOnly=true)` may clone for the
data overlay (`W:0x1004c340.c`).

---

## 7. Water

### 7.1 Surface (`createWaterVertex`, `L:0x08165e10.c` = `W:0x100d62c0.c`; built `L:0x08166080.c:4540-4680`)

For each water cell (viewMode & 4, §2.2 rule): a quad over the cell's four corners, indices `TL,BL,TR · TR,BL,BR`,
corners shared within the division.

| Attribute | Value |
|---|---|
| position | corner x/z, **y = −0x140·cs/256 = −3.75 m** (flat, no per-cell water height) |
| normal | (0,1,0) |
| uv0 | `(gx, gz)·cs·0.5` (= 1.5 per cell) |
| colour | RGB 0x80, **A = clamp((−0x140 − h(corner)) >> 2, 0, 255)** = depth below the surface (255 from ~4 cells = 12 m) |
| uv2 | 0 |

### 7.2 `water.shader` behaviour (obs, `S:water/water.*`)

Own program (no default base): attributes POSITION|NORMAL|TEXCOORD0|COLOR, textures 0,1,2 (2 = cube),
uniforms `timer`, `reflection` (material +0x6c = 0.2), `vieworient` (view 3×3); shadow-map params when
`ENVIROMENT_SHADOWMAP`.

* Vertex: wave `y += sin(x·π/8 + 2.301·t)·sin(z·π/8 + 1.301·t)·0.25` (object-space x,z, i.e. division-local; period
  16 m, so seamless across 48 m divisions);
  `gl_FrontColor = gl_Color·2` (rgb ≈ 1, alpha = 2·depth/255), alpha × (1 + 0.1·wave); `v_texCoord = uv0`.
* Fragment: normal = `n1·n2 + n3·n4` from four scrolled samples of water-n/water-n2 (scales 0.0527, 0.03125, 0.0898,
  0.0703); per-pixel point-light diffuse + specular (glossiness 128); env-cube reflection
  `textureCube(reflect(view,n)·vieworient)·reflection·(2 + 1.4·dot(n,view))`; colour = `(ambient + diffuse)·gl_Color
  + specular + reflection`, `a = (diffuse.a_material + r)·gl_Color.a`; then a **shore foam** mix towards
  `(0.55,0.55,0.60)` with factor `clamp(mat.a·gl_Color.a·8 − normal.z·8 + 2, 0.3, 1)` (shallow = low alpha = foam);
  fog from `length(v_view)` (the declared `v_fog` varying is unused).
* Translator notes: `water.fragment` tests `$shadowMapRender`, which it never defines (must evaluate false);
  `waterEdge.shader` sets components to `null` (not `""`) for the shadow-map pass.

### 7.3 Water edge curtain (built `L:0x08166080.c:4684-4979`)

Only on **map-border** sides of border divisions (in the normal view). For each border side with water cells:
`2(n+1)` vertices (`createWaterVertex` with `param_8 = −0x400`): per border corner a **bottom** vertex at
`min(h, −0x140)` and a **top** vertex at −0x140; normal = outward axis (`±x`/`±z`); colour 0xFF808080;
`uv0 = (y < −0x140 ? 0 : 1, 0)` so only the top row waves (`waterEdge.vertex`: `y += wave·uv0.x`). Two triangles per
water cell along the side (`b_i, b_i+1, t_i · t_i, b_i+1, t_i+1`). `waterEdge.shader`: per-pixel lit, colour =
`(ambient + diffuse)·0.5 + specular`, alpha = 0.75 (material diffuse alpha 0xC0); two-sided.

### 7.4 What does not exist

There is **no separate sea plane** and no water outside the map: all water is the per-cell surface above. The
`water/contrain.*` shaders are boat wakes, set by vehicle scripts (`metro/objects/vehicles/*/…script`). Outside the map
the viewport clear colour `0xFF373028` (`E:26`) and fog (`E:32-33`, start 300, distance 1500) show.

---

## 8. Cliffs, sea floor, map sides

### 8.1 Map-edge walls (`createEdgeVertex`, `L:0x08165ce0.c` = `W:0x100d6160.c`; built `L:0x08166080.c:4250-4537`)

Lod 0 of every division touching the map border, one wall per touching side, `edgeMaterial` (edge.dds, default
shader):

| Item | Value |
|---|---|
| vertices | per border corner `i = 0..n`: top at the terrain height `h(corner)`, bottom at **−0x2000** (−32 cells = −96 m) |
| normal | outward axis: x=0 side (−1,0,0), x=W (+1,0,0), z=0 (0,0,−1), z=L (0,0,+1) |
| uv0 | `u = (lx + lz)·0.0625` (division-local corner coords → 0..1 per division side), `v = 1 − (y + 0x2000)/max(top + 0x2000, 1)` (0 at the top, 1 at the bottom; heights in 1/256 cell) |
| colour / uv2 | 0xFF808080 / 0 |
| indices | 2 triangles per cell along the side, **CCW seen from outside** (left/right sides list each pair top-first resp. bottom-first; checked: all four face outward); material is one-sided, so the walls are invisible from inside the map |
| underground (viewMode 0x180) | top clamped to ≤ −0x800; interior walls towards divisions flagged `+0x30` (not needed for the normal view) |

The map therefore looks like a block cut out of the ground: brown `edge.dds` sides down to −96 m, sea water closed
by the water-edge curtain (§7.3).

### 8.2 Cliffs and sea floor (inside the terrain shader)

Not separate geometry: the terrain group blends `cliff.dds` (+ `cliff_n.dds` normal map) by uv2.y (slope from the
vertex normal, §3) and `seafloor.dds` by uv2.x (corner touches a water cell). Both transitions are dithered by
`cliff.a`. Cells under item normal overrides get neither.

### 8.3 Underground view and item foundations (the view in full: [underground.md](underground.md))

* Underground (`viewMode` 0x180, no bit 2), only in divisions with tunnel items (the others are built as 0xc6: the
  terrain in earthMaterial, water, no items): the terrain surface is replaced by a flat **earth plane** at
  y = −0x800·cs/256 = −24 m (every cell, `createEarthVertex` clone, normal +y, uv `(gx,gz)·cs/64`, earthMaterial);
  water surfaces are not drawn (no bit 4); walls face inward towards divisions without tunnels; cover-quadrant
  edges would get short base walls (none: the view clears the cover) (`h` to `h − 0xc0`) with
  `baseMaterial` (`createBaseVertex`: `u = (x+z)·0.25` or `z·0.354` for diagonals, `v = −h/1024`)
  (`L:0x08166080.c:2375-2470, 3292-3736`, `L:0x08165a90.c`, `L:0x08163810.c`, `L:0x08165b90.c`).
* Items with runtime flag `0x800` at `item+0x48` ("needs base", set by `gsGridItem::fixY` when the ground under a
  `GRID_ITEM_ENABLE_BASE` footprint is uneven, `L:0x08187760.c:106-108`) get foundation walls between the item's y
  and the terrain along the footprint, in every view (`L:0x08166080.c:3737-4249`). This belongs to item rendering.

---

## 9. Port checklist (minimal, shipped map/save, normal view, no items yet)

1. **Load** cells as now (`port/src/engine/grid.*`): add `0x200 − heightBase`, clear `0x4000|0x0f00`. Normal override
   map = 0. Run the §2.2 dry-out rule once over all cells.
2. **Divisions**: n = `1 << bucketShift` (16); one mesh per division, positioned at its centre (§1), vertices
   division-local. Only the water waves use object-space x/z; division-local is seamless because a division
   (48 m) is exactly 3 wave periods (16 m), and world coordinates would only shift the phase by a constant.
3. **Corner attributes** exactly as §3 (pos, height-gradient normal, uv0 = g·0.125, R = variant<<5 / 0 / 255, uv2.x
   field-or-seafloor, uv2.y cliff). Centre vertices §3.1 (all-four-corners case).
4. **Groups**: terrain (`grid.material`) for cells that are not pavement/field (or are preview pavement `0x40`);
   `yard[f&7]` for `(f&0x48)==8`; `field[f&7]` for `(f&0x60)==0x20`; water quads (§7.1); map-edge walls (§8.1);
   water-edge curtains (§7.3). Lod 0 = fan (§4) for near divisions, lod 1 = 2 tris/cell; or build lod 0 everywhere
   for a first version.
5. **Materials** come from running `engine.script` against `new_grid`: expose `material`, `earthMaterial`,
   `reservedMaterial`, `baseMaterial`, `edgeMaterial`, `positiveMaterial`, `negativeMaterial`, `waterMaterial`,
   `waterEdgeMaterial`, `dataMaterial` and `getYardMaterial(i)`/`getFieldMaterial(i)` (i < 8) as material variants;
   `setTexture(0)` must set `DIFFUSEMAP`.
6. **Vertex streams**: terrain/field programs need `TEXCOORD1` (uv2) and `COLOR` (RGBA8, R in byte 0); terrain and
   yards 4–7 need generated tangents (NORMALMAP).
7. **State**: polygon offset `(2·depthOffset, 2·depthOffset)` for opaque groups with a depth offset; water and water
   edge in the blended pass (depth write off, offset −2,−2, sorted far→near with the −100 radius bias), water edge
   without culling; skip water/water edge in the shadow-map pass.
8. **Uniforms**: `timer` (water, effect timer), `reflection` (0.2), `vieworient`, `texture0..4`; `matrix` is
   requested by `terrain.shader`/`grid.shader` (MATRIX bit) but no live shipped terrain component declares it (only
   the dead `terrain.vertex`); plus the fixed-function emulation the port already does (lights, material, fog).
9. **Translator fixes**: reversed-edge `smoothstep` (§6.1); undefined `$shadowMapRender` in `water.fragment`;
   unused `varying float v_fog` in `water.fragment`.
10. **Distances** (optional): cull divisions at 880 m, lod switch at 266.4 m, LAYER groups always (§6.5).
11. **Later, with items**: cover quadrants and normal overrides from `gsGridItem::fixGrid`, the centre-vertex
    partial cases (§3.1), foundation walls (§8.3), highlights and the data overlay.

---

## 10. Open questions

1. Exact metric for the near flag (`div+0x28`, `+0x2c`) and the in-range radius R (`gsGrid::move`): which camera
   points are used and what video vtable `+0x120` (Linux) returns. Only affects which LOD exists, not the look (inf).
2. `reservedMaterial`: no reader found in the renderer, in the item code or in the scripts (inf: unused leftover).
3. `div+0x30/+0x34`: the tunnel-item counter of a division and its copy at build time, see [underground.md](underground.md) §3.
4. The centre vertex of a partly covered cell takes the **missing** corner's height (§3.1). That is what both
   builds do (`L:0x08165160.c:95,137,146,155`); it was not checked in game whether this produces visible kinks.
5. Vertex-colour byte order is inferred from the writers and the shader reads (consistent for both terrain `.r` and
   water `.a`); the upload code (`W:0x10049710`) was not re-read for a swizzle.
