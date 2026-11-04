# Asset format: meshes (`.object`) and materials (`.material`)

Engine: `GSystem.dll` (Cities in Motion 1.0.22). Evidence is `file:line` in the Ghidra v2 pseudocode,
`decompiled/GSystem-v2/functions/0x<addr>.c`, unless another path is given. `[obs]` = checked against data,
`[code]` = read from the loader or renderer, `[inf]` = inferred (no direct proof). Confidence: high / medium / low.

Status: **both formats are fully decoded at the byte level.** A reference parser and validator
(section 4) reads all **2 747 `.object` and 1 556 `.material` files** shipped in the 17 top-level game archives
that contain them. Every chunk is consumed exactly. All structural checks pass (one file-placement caveat for
skinned meshes, section 4). A few header bytes have no engine-side name; they are listed in section 5.

Scope: everything a `.object` can contain (mesh, LOD, skin, UV2, material list, locator chunks) and the
`.material` texture-slot format. Skeletons (`0x0220xxxx`), animations and `.scene` files are other formats.
They appear here only where a mesh refers to them.

---

## 1. Overview

### 1.1 Chunk framework and loader call graph

Every structure is `u32 tag, u32 size, body` (little endian; `size` excludes the 8-byte chunk header). A body can
start with a fixed header followed by child chunks. Reader primitives: `FUN_1007bd90(reader,&hdr,parent,0)` gets the next
child, `FUN_1007be00(reader,hdr,n)` returns a pointer to the next `n` body bytes, and `FUN_1007c000` skips to the chunk end.
Unknown children are skipped silently, so the format is forward compatible.

All loading happens in `gsObject` virtual methods (vtable `0x1015ed5c`). gsMoveableObject, gsGridObject and
gsAreaDefinition inherit the same loader slots; only gsAreaDefinition overrides +0x188 (`0x1005ed20`, which keeps
positions and indices only):

```
gsObject::load(path)                     +0x18c 0x1008bd90  first chunk 0x02000000 -> +0x150, 0x02100000 -> +0x144
  readObjectRoot                         +0x150 0x10087aa0  reads the 60-byte root header
    loadObjectBody                       +0x154 0x1008ba60  dispatches root children (section 2.2)
      loadMeshChunk                      +0x164 0x10087770  allocates a 44-byte mesh-data struct (0x100a5900)
        parseMesh                        +0x160 0x100882c0  sets mesh flags, dispatches 0x0201x000 children
          vertices        0x02011000     +0x170 0x10087fb0
            skin          0x02011100     +0x16c 0x10087ce0
            uv2           0x02011200     +0x168 0x100876f0
          indices         0x02012000     +0x174 0x10087730
          material list   0x02013000     +0x178 0x100881a0
            material      0x02013100     +0x17c 0x1008b8e0  -> +0x180 0x10090170 -> FUN_1008ec70 (texture slots)
        buildRenderMesh                  +0x188 0x1008dd80  -> gsVideo_OGL +0xf4 createMesh (gsMesh_OGL / gsSubMesh_OGL)
      locator             0x02600000     +0x11c 0x1008fdc0
      camera              0x02700000            0x10073310  (creates a gsFixedCamera)
loadMaterial(path) (script native)              0x1008faf0  reads a .material file directly
```

### 1.2 Coordinate system, units, winding

| property | value | evidence | confidence |
|---|---|---|---|
| handedness | **right-handed**. The view matrix is a standard gluLookAt (`z = normalize(eye - target)`, `x = y × z`), uploaded with `glLoadMatrixf`. | `0x10086350.c:41-80`, `0x10044ef0.c:167` | high [code] |
| up axis | **+Y**. Median mesh `min.y` is 0.000 (5th–95th percentile −1.5 … 0.003). The tallest landmarks are tall in Y (Sky Tree 235 m, Empire State 228 m, Eiffel Tower 207 m). | validator | high [obs] |
| units | **1 unit ≈ 1 m**: bus_b 2.42 × 2.64 × 6.51, metro car 3.3 × 3.1 × 13.1, characters 2.0–2.16 tall, Kölner Dom 60 × 106.5 × 103. Assets are scaled a little loosely. | validator | high [obs] |
| forward axis of vehicles | along ±Z (vehicles are long in Z). The sign is not determined here. | validator | medium [obs] |
| front faces | **counter-clockwise** (GL default). There is no `glFrontFace` call, and `glCullFace(GL_BACK)` is used except in mirrored passes. In the data, `(b−a)×(c−a)` points the same way as the vertex normals for 99.92 % of all 3.15 M triangles (every file has a clear majority). | `0x10044ef0.c:376-377`, validator | high |
| two-sided | the material flag `MATERIAL_TWOSIDED` (0x1000) disables culling | `0x100478f0.c:39-54` | high [code] |
| matrices | row-vector convention in the math library (translation in m[12..14]) | `0x10084370.c`, `0x10084410.c` | medium [code] |
| quaternions | stored `(x, y, z, w)`. For example, a 90° turn about Y is `(0, 0.7071, 0, 0.7071)`. | validator | high [obs] |
| texture V | **stored negated**: 99 % of `v` values lie in [−1, 0], while `u` lies in [0, 1]. The engine passes UVs unchanged to `glTexCoordPointer`, and DDS rows are uploaded top row first, so `v_file ≡ 1 − v_max (mod 1)`. For glTF (origin top-left, same as DDS), use **`v_gltf = v_file + 1`**, or keep the value and rely on REPEAT. | `0x10049710.c:133`, `0x10048160.c:52`, validator | high [obs], medium for the export rule [inf] |

Vertex positions are **node-local**. The root header (section 2.1) holds the 3ds Max node placement, for example
`metro_b` sits at (6.43, 0, −1.97) while its mesh is centred on the origin. The engine applies that placement only
when `load(path, …, applyTransform)` is called with the third argument set.

### 1.3 Asset pipeline facts (observed)

* Exported from 3ds Max: material names like `01 - Default`, `Standard_8`, `Material #77`, `Undefined`. Source paths
  look like `G:\GSystem\addons\humble\bus\dev\bus_b.psd`. Slot header bytes look like Max map settings.
* A mesh keeps its geometry and a **list of material records** (only name + ranges + default colours).
  Appearance and textures come from a separate file **`<object dir>/<material name>.material`**
  (`0x1008b8e0.c:39-62`). If that file is missing (69 of 4 141 records), the embedded defaults are used and no
  textures are bound.
* The 0x02040000 LOD mesh uses its own material names (e.g. `diagonalhouse02LOD`).

---

## 2. Chunk reference

### 2.1 `.object` root — `0x02000000` (60-byte header + children)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 32 | char[32] | name | object name, NUL padded. It differs from the file stem in 48 files (e.g. `cab.object` → `hackney`). Copied to `this+0x140`. | `0x1008ba60.c:29-34` | high [code+obs] |
| 0x20 | 12 | f32[3] | nodePosition | 3ds Max node position. Stored at `this+0x1e4`; applied via vtable +0xf0 (setPosition) only if load flag `this+0x19a` is set. Non-zero in 2 209 files. | `0x1008ba60.c:18-20,25-27` | high [code] |
| 0x2c | 16 | f32[4] | nodeRotation | quaternion (x,y,z,w). Stored at `this+0x1d4`; applied via +0xf4 if `this+0x19a` is set. 20 files have a non-unit norm (1.65 / 2.07, scale baked into the quaternion: `horses01/02`, `merry-go-round`, `rootBone`). | `0x1008ba60.c:21-24,28` | high [code+obs] |
| 0x3c | … | chunks | children | section 2.2 | `0x10087aa0.c:12-13` | high |

The header is read as exactly `0x3c` bytes (`0x10087aa0.c:12`). The loader recognises only one other root tag,
`0x02100000` (children `0x02080000`, `0x02600000`, `0x02700000`; `0x1008bd90.c:116-121`, `0x100885c0.c:17-27`).
No shipped `.object` uses it.

### 2.2 Root children (dispatch in `0x1008ba60.c:59-79`)

The switch in `0x1008ba60` was compiled as a binary search, so it contains **comparison-only split values that are not
tags**: `0x02050000 <` (line 59), `< 0x02600001` (line 60) and `< 0x02030001` (line 75). **`0x02030001` is not a
chunk tag**, it is just the upper bound for the 0x0201…0x0203 group. **`0x02070000` is a real tag**: it is tested with `==`
(line 64) and goes to the mesh loader.

| tag | name | mesh flags set (`meshData[8]`) | handled by | shipped count | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x02010000 | render mesh, LOD 0 | `MESH_RENDER` 0x02 | mesh loader → gsMesh_OGL LOD 0 | 2 747 (every file, exactly one) | `0x100882c0.c:32-34` | high |
| 0x02020000 | collision mesh | `MESH_COLLISION` 0x01 | parsed; gsObject builds no render mesh (`flags&2`==0) | 0 | `0x100882c0.c:35-37`, `0x1008dd80.c:67` | high [code] |
| 0x02030000 | hit (picking) mesh | `MESH_HIT` 0x04 | parsed; no render mesh | 0 | `0x100882c0.c:38-40` | high [code] |
| 0x02040000 | LOD 1 | `MESH_LOW1\|MESH_RENDER` 0x12 | render mesh LOD 1 | 394 | `0x100882c0.c:41-43`, `0x1008dd80.c:67-79` | high |
| 0x02050000 | LOD 2 | `MESH_LOW2\|MESH_RENDER` 0x22 | render mesh LOD 2 | 0 | `0x100882c0.c:44-46` | high [code] |
| 0x02060000 | LOD 3 | `MESH_LOW3\|MESH_RENDER` 0x42 | render mesh LOD 3 | 0 | `0x100882c0.c:47-49` | high [code] |
| 0x02070000 | mesh without flags | none (0) | parsed; no render mesh | 0 | `0x1008ba60.c:64` | high that it is a tag; purpose unknown |
| 0x02600000 | locator / dummy | — | +0x11c `0x1008fdc0` (section 2.9) | 17 790 in 2 747 files | `0x1008ba60.c:61-63` | high |
| 0x02700000 | camera (gsFixedCamera) | — | `0x10073310`, 88-byte record (section 2.9) | 0 | `0x1008ba60.c:66-68`, `0x10072dd0.c:20` | high (class) / low (fields) |

Mesh-type index for the renderer: `flags&0x10` → 1, `&0x20` → 2, `&0x40` → 3, else 0 (`0x1008dd80.c:67-79`).
If `gsObject+0x1a0` (a cell size) is non-zero, the render mesh is additionally split into a spatial grid of submeshes
(`0x1008dd80.c:82-340`). With `+0x19c` set, a CPU copy is kept per LOD for hit tests (`0x1008dd80.c:353-365`).
The `MESH_*` constant values are from `docs/native-api.md`. `MESH_SKINNED` (0x100) is ORed in by the skin chunk.

### 2.3 Mesh chunk — `0x0201/02/03/04/05/06/07 0000` (no header, children only)

Shipped order is always `0x02011000, 0x02012000, 0x02013000`, one of each (3 141 meshes). The loader does not need
this order. Runtime mesh-data struct (44 bytes, `0x100a5900.c`): `[0]` vertices, `[1]` vertexCount,
`[2]` indices, `[3]` indexCount, `[4]` submeshes, `[5]` submeshCount, `[6]` uv2, `[7]` skin (expanded), `[8]`
flags, `[9]` 0, `[10]` = `0x5a0e1bca` (constant; the same value is used as the gsObject seed `+0x20c`).

| child | name | handled by | evidence |
|---|---|---|---|
| 0x02011000 | vertex list | +0x170 `0x10087fb0` | `0x100882c0.c:53-55` |
| 0x02012000 | index list | +0x174 `0x10087730` | `0x100882c0.c:56-58` |
| 0x02013000 | material (submesh) list | +0x178 `0x100881a0` | `0x100882c0.c:59-61` |

### 2.4 Vertex list — `0x02011000`

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 4 | u32 | vertexCount | number of vertices | `0x10087fb0.c:30-32` | high [code+obs] |
| 0x04 | 36·n | Vertex[n] | vertices | stride **36**, see the table below | `0x10087fb0.c:33` | high |
| 4+36n | … | chunks | children | `0x02011100` skin, `0x02011200` UV2 (either, both or none) | `0x10087fb0.c:35-47` | high |

**File vertex format (stride 36 = 0x24, all little endian):**

| offset | size | type | components | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|---|
| 0x00 | 12 | f32 | 3 | position | node-local position (m) | `0x10049710.c:121-124`, `0x10048160.c:84` | high [code+obs] |
| 0x0c | 12 | f32 | 3 | normal | unit normal. 5 132 982 of 5 133 079 are within 1 % of unit; 97 are zero. Converted to `GL_SHORT` for the GPU. | `0x10049710.c:126-131`, `0x10087fb0.c:69-72` | high |
| 0x18 | 4 | u8 | 4 | color | vertex colour **R,G,B,A** in byte order (copied raw to a `glColorPointer(4,GL_UNSIGNED_BYTE)` stream). `0xFFFFFFFF` = none (the color array is enabled only if some vertex ≠ 0xFFFFFFFF). | `0x10049710.c:63-71,125`, `0x10048160.c:49,59` | high |
| 0x1c | 8 | f32 | 2 | uv0 | texture coordinate, V negated (§1.2) | `0x10049710.c:133-134`, `0x10048160.c:52` | high |

Vertex colour use [code+obs]: the default shader ignores it (`shaders/default/color.vertex` sets `color = vec4(1)`).
The trees shader multiplies by `gl_Color * 2.0` (`metro/shaders/trees/trees.vertex:216`), so `0x80808080` is neutral there.
The loader's vegetation pass (`gsObject+0x19d`, `0x100882c0.c:66-134`) treats black (`rgb==0`) vertices as foliage that
gets a spherical normal (normalised position). It moves triangles whose three vertices have `R==0 && B==0` to the end
and sorts them by height. Observed values: 1 303 meshes are uniformly `0x80808080`, 877 uniformly `0xFFFFFFFF`, and
939 meshes mix values (mostly `0xFF00FF00` green and `0xFF000000` black markers).

The loader post-processes vertices in memory (`0x10087fb0.c:48-84`). It bakes the object's mesh transform into them:
scale `+0x1ac` (constructor 1,1,1), rotation `+0x1b8` (identity) and offset `+0x1c8` (0), so this is the identity on a
first load. It also accumulates the local AABB (`+0x15c`), the world AABB (`+0x112`) and the bounding radius
max |p| (`+0x12c`).

### 2.5 Skin weights — `0x02011100` (child of 0x02011000)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 2 | u16 | influencesPerVertex (n) | always 4 in data; the engine uses at most 4 (`min(n,4)`) but steps by n | `0x10087ce0.c:36,42,46-49` | high |
| 0x02 | 2 | u16 | reserved | 0 in all 236 chunks; not read | validator | high [obs] |
| 0x04 | 8·n·V | Influence[V][n] | influences | per vertex, n records in vertex order | `0x10087ce0.c:42` | high |

Influence record (8 bytes):

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x0 | 2 | u16 | bone | bone index into the object's skeleton: the order of `0x02201000` records in the `*_scene.skeleton`, with the mesh node itself at index 0 (bus_b: `bus_b, door-bone, root-bone` → max index 2) | `0x10087ce0.c:65-69`, validator | high (index < count) / medium (exact mapping) |
| 0x2 | 2 | u16 | reserved | 0 in all 2 012 004 records; not read | validator | high [obs] |
| 0x4 | 4 | f32 | weight | the engine renormalises the first min(n,4) weights to sum 1 (sum 0 is left as is) | `0x10087ce0.c:45-56,71-72` | high |

Unused slots are stored as `bone 0, weight 0` (1 463 742 records). The chunk sets `MESH_SKINNED` (0x100, line 31).
It expands the data to 32 bytes per vertex (`{u32 bone, f32 weight}×4`, zero padded, lines 57-101) and builds a
bone-used table of size maxBone+1 (`+0x190/+0x194`, lines 103-125). gsHWSkinSubMesh_OGL later splits the bones into shader palettes.

### 2.6 Second UV set — `0x02011200` (child of 0x02011000)

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 2 | u16 | type | must be **2**, otherwise the body is ignored | `0x100876f0.c:14-15` | high |
| 0x02 | 2 | u16 | reserved | 0 in all 1 759 chunks | validator | high [obs] |
| 0x04 | 8·V | f32[2][V] | uv1 | second texture coordinate per vertex (V negated like uv0) | `0x100876f0.c:16-17`, `0x10049710.c:263-288` | high |

Use [code]: GPU texcoord unit 1. The default shader samples the **ambient-occlusion map** with it
(`shaders/default/maps.fragment:46-47` → `v_texCoord.zw`), and so do the MIX2/MIX3 blend weights. 481 of 485
AO-mapped meshes carry this chunk. Material flag `0x01000000` (header byte 0x21 bit 2) swaps the channels:
uv1 → texcoord0 and uv0 → texcoord1 (`0x10049710.c:139-147,264-288`). 120 of 121 such materials sit on meshes with UV2.

### 2.7 Index list — `0x02012000`

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 4 | u32 | indexCount | multiple of 3 in all meshes | `0x10087730.c:14-16` | high |
| 0x04 | 4·n | u32[n] | indices | **absolute** vertex indices (into the whole mesh, not the submesh). Primitive is a triangle list. | `0x10087730.c:17`, `0x100480d0.c:14-31` | high |

The renderer rebases indices per submesh (`index − firstVertex`) and uses `GL_UNSIGNED_SHORT` when the submesh has
fewer than 65 536 vertices, `GL_UNSIGNED_INT` otherwise (`0x10049710.c:83-104`, `0x10048160.c:86-94`). The draw mode
comes from submesh `+0x18`: 0 → `GL_TRIANGLES`, 1 → `GL_LINES` (`0x10049710.c:319-324`). Nothing in the file sets it,
so it is always triangles.

### 2.8 Material list — `0x02013000` and material record `0x02013100`

`0x02013000`:

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 4 | u32 | count | number of submesh records; 28-byte runtime records are allocated for it. Equal to the number of 0x02013100 children in all files. | `0x100881a0.c:24-38` | high |
| 0x04 | … | chunks | 0x02013100 × count | the i-th child becomes submesh i (extra children are ignored) | `0x100881a0.c:40-55` | high |

**72-byte material header** (identical layout as the embedded 0x02013100 body in `.object` and the root header of a `.material` file):

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 32 | char[32] | name | material name, NUL padded. Key of the per-object material cache (`+0x210`, shared across LODs/submeshes); forms the file name `<dir>/<name>.material` | `0x1008b8e0.c:37-44`, `0x10090170.c:46-71` | high |
| 0x20 | 1 | u8 | b20 | `== 0x1F` → material flag **0x08000000** (no MATERIAL_* name). Observed 0 or 31; 31 only on road-surface materials `Standard_8/Standard_9` in `metro/objects/roads`. The grid road builder keeps such materials instead of substituting the road-style material (`0x100d6ab0.c:651,1156`). | `0x1008ec70.c:127-129` | high (decode) / low (meaning) |
| 0x21 | 1 | u8 | b21 options | bit1 (2) → `MATERIAL_DECAL` 0x8000 + depth bias −1.0 (gsMaterial_OGL +0x24); DECAL also clears ALPHABLEND. bit2 (4) → flag **0x01000000** "UV2 is primary" (§2.6). Observed 0 / 2 / 4. | `0x1008ec70.c:130-136,393-395` | high |
| 0x22 | 1 | u8 | twoSided | ≠0 → `MATERIAL_TWOSIDED` 0x1000. 0 in all shipped files. | `0x1008ec70.c:121-123` | high [code] |
| 0x23 | 1 | u8 | opacity | 0–255; ≠255 → diffuse alpha ×= opacity/256 and `MATERIAL_ALPHABLEND` (1). Observed 13, 51, 77, 128, 255. | `0x1008ec70.c:89,93-97` (flags = `[esp+0xc]` in the binary at 0x1008ecbd) | high |
| 0x24 | 1 | u8 | specularLevel | specular colour ×= level/256. `> 4` and specular rgb ≠ 0 → `MATERIAL_SPECULAR` (4). Typical 128. | `0x1008ec70.c:113,118-120` | high |
| 0x25 | 1 | u8 | glossiness | specular exponent `max(1, value)`. Observed 4 / 8 / 16 / 32. | `0x1008ec70.c:104-109` | high |
| 0x26 | 1 | u8 | selfIllumination | emissive factor = value × 0.1, so 10 → 1.0. Observed 0–50. | `0x1008ec70.c:115-117`, `0x1004dac0.c`, `0x10075200.c` | high |
| 0x27 | 1 | u8 | reflection | value/255 is passed to setSpecular as the reflection amount (`mat+0x6c`); ≠0 → `MATERIAL_REFLECTION` 0x2000 and the cube map `textures/envmap.dds` in the last texture unit. Its histogram matches 0x24 almost exactly (1 044 × 128). | `0x1008ec70.c:110-114,124-126,332-350` | high (decode) |
| 0x28 | 4 | u8[4] | ambient | RGBA → gsMaterial_OGL +0x14 (`GL_AMBIENT`, `mat+0x24`) | `0x1008ec70.c:99-101`, `0x1004d9f0.c`, `0x100478f0.c:104` | high |
| 0x2c | 4 | u8[4] | diffuse | RGBA (alpha × opacity) → +0x18 (`GL_DIFFUSE`, `mat+0x34`) | `0x1008ec70.c:92-103`, `0x1004da30.c` | high |
| 0x30 | 4 | u8[4] | specular | RGBA → +0x1c (`GL_SPECULAR`, `mat+0x54`), scaled by specularLevel. **Replaced by white×level when a specular map is present.** | `0x1008ec70.c:105-114,251-260`, `0x1004da70.c` | high |
| 0x34 | 4 | u8[4] | emissive | RGBA → +0x20 (`GL_EMISSION`, `mat+0x44`) × selfIllumination×0.1 | `0x1008ec70.c:115-117`, `0x1004dac0.c` | high |
| 0x38 | 4 | u32 | firstVertex | submesh vertex range start → submesh +0x0c. **Only the copy embedded in the `.object` is used.** | `0x10090170.c:97`, binary `0x10090335` | high [code+obs] |
| 0x3c | 4 | u32 | vertexCount | → submesh +0x10 | `0x10090170.c:98` | high |
| 0x40 | 4 | u32 | firstIndex | index range start → submesh +0x04 | `0x10090170.c:99` | high |
| 0x44 | 4 | u32 | indexCount | → submesh +0x08 | `0x10090170.c:100` | high |

Colour byte order [code]: `FUN_10074d50` swaps bytes 0 and 2 into the internal BGRA dword, and `FUN_10075040` turns
mem[2],mem[1],mem[0],mem[3] into float R,G,B,A. So **the file stores R,G,B,A**, consistent with warm emissive colours like
`FF5A00FF` orange and `FFF0C8FF` warm white.

**Which header supplies what** (confirmed in the machine code, because the pseudocode drops the call arguments).
`0x1008b8e0` reads the embedded 72-byte record into `edi`, then tries `<dir>/<name>.material` (`0x1008b8e0.c:36-62`).
It then calls vtable +0x180 = `0x10090170(reader, chunk, submesh, rangeHeader, materialHeader, meshData)`:

* external file found (bytes `0x1008b9eb-0x1008ba14`): `(extReader, extChunk, submesh, edi = embedded, extHeader, meshData)`
* no external file (`0x1008ba16-0x1008ba23`): `(meshReader, embeddedChunk, submesh, embedded, embedded, meshData)`

`0x10090170` (`ret 0x18`, 6 arguments) parses the **appearance** fields (0x20–0x37) and the texture slots from argument 5,
via `FUN_1008ec70`. It copies the **submesh ranges** (0x38–0x47) from argument 4, the **embedded** record, into the
submesh (bytes `0x10090335-0x10090357`: `mov eax,[esp+0x4c]` = arg4; `mov ecx,[esp+0x48]` = arg3;
`[ecx+0xc]=[eax+0x38]`, `[ecx+0x10]=[eax+0x3c]`, `[ecx+4]=[eax+0x40]`, `[ecx+8]=[eax+0x44]`). The pseudocode
`0x10090170.c:97-100` mislabels these as param_2/param_3. The data agree: in 3 181 of 4 072 pairs the external ranges are
stale (e.g. Kölner Dom: embedded 29 253 / 50 772, external 28 596 / 49 017), while the embedded ranges always tile the
mesh exactly. Embedded records always hold the exporter defaults: b25 = 4, ambient = diffuse = `808080FF`
(56 × `7F7F7F7F`), specular `FFFFFFFF`, emissive `FFFFFFFF` (56 × 0), all other bytes 0.

After parsing, `0x10090170.c:73-77` ORs in `MATERIAL_SORTALPHA` (0x40) when ALPHABLEND is set and the object's
`+0x184` flag is set. The shader is then requested as `getShader("", flags, 0)` (`0x1008ec70.c:403`), which means the
uber-shader combiner keys on the flags.

**Submesh runtime record** (28 bytes, `0x100a58d0.c`): `+0` index i, `+4` firstIndex, `+8` indexCount,
`+0xc` firstVertex, `+0x10` vertexCount, `+0x14` gsMaterial_OGL*, `+0x18` primitive (0 = triangles).

### 2.9 Locator — `0x02600000` (64 bytes), camera — `0x02700000` (88 bytes)

Locator (dummy / helper node; stored in a map at `gsObject+0x134` keyed by name). The writer is `0x10088e10.c`.

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 4 | u32 | id | 0 in all 17 790 | `0x1008fdc0.c:31-35`, `0x10088e10.c:24` | high (decode) / low (meaning) |
| 0x04 | 32 | char[32] | name | e.g. `t-a0`, `e-b1` (track and road connection points). The writer forces a NUL at byte 31. | `0x1008fdc0.c:44-50`, `0x10088e10.c:27,34,42-43` | high |
| 0x24 | 12 | f32[3] | position | node-local | `0x1008fdc0.c:36-38` | high |
| 0x30 | 16 | f32[4] | rotation | quaternion (x,y,z,w) | `0x1008fdc0.c:39-42` | high |

`0x02700000` = **camera** (not shipped). `0x10073310.c:39-75` reads 88 bytes and creates a
**gsFixedCamera** (constructor `0x10072dd0`, vtable `0x1015d7a4` per `docs/rtti-classes.json`). Field use, from
pseudocode with a confused stack, so confidence is low: name `char[]` at `+0x04` (→ camera `+0x14c`), vec3 at `+0x2c`
and f32 at `+0x38` (eye position?), vec3 at `+0x3c` rotated by the object rotation `+0x1d4` (target or direction),
i32 at `+0x48` (→ camera `+0x188`), f32 at `+0x4c` in degrees turned into an axis-angle quaternion (roll), and
`+0x50..0x57` unread. The camera's position and rotation are set via vtable +0xf0/+0xf4.

### 2.10 `.material` file — root `0x02013100`

| part | layout | evidence | confidence |
|---|---|---|---|
| root | `0x02013100 size` + the 72-byte header of §2.8 + slot chunks. The engine asks the reader for a chunk with tag 0x02013100 (`FUN_1007bd90(…,0,0x2013100)`) and never looks past it, so trailing bytes are harmless [inf]. | `0x1008b8e0.c:59-62`, `0x1008faf0.c:59-64` | high |
| slot chunks | `0x02013110` diffuse, `0x02013120` normal, `0x02013130` specular, `0x02013140` **illumination** (self-illumination map), `0x02013150` ambient occlusion. Each is a 40-byte slot header followed by source-path chunks. The switch uses `tag − 0x02013110 ∈ [0,0x40]` with a byte table (binary at 0x1008ee50), so no other slot tags exist. | `0x1008ec70.c:143-327` | high |
| order | any order; the shipped files mostly list 110,130,120,(150),(140). 73 files have no slot, 5 is the maximum. | validator | high [obs] |

**40-byte slot header** (`FUN_1007be00(…,0x28)`, one per slot chunk):

| offset | size | type | name | meaning | evidence | confidence |
|---|---|---|---|---|---|---|
| 0x00 | 1 | u8 | disabled | must be 0, otherwise the whole slot is ignored (no texture, no flag). 0 in all files. | `0x1008ec70.c:147` | high [code] |
| 0x01 | 1 | u8 | amount (?) | 255 in all files; not read | validator | low |
| 0x02 | 1 | u8 | alphaUsage | diffuse: 0 = alpha is opacity, 1 = alpha is a **colorize mask**. Normal: 1 = alpha is a colorize mask. Other slots: 0. | `0x1008ec70.c:174-183,216-218` | high (decode) |
| 0x03 | 1 | u8 | alphaBits | alpha depth of the texture: 0 none, 1 one-bit, 8 full. Correlates with DDS format: b3=8 ↔ DXT5 in 527 of 530 cases, b3=1 ↔ DXT1 in 594 of 605. | `0x1008ec70.c:175-182`, validator | high |
| 0x04 | 2 | u8[2] | ? | 0xFF 0xFF in all files; not read | validator | low |
| 0x06 | 2 | u8[2] | ? | 0 in all files; not read | validator | low |
| 0x08 | 32 | char[32] | texture | file name, resolved like script paths against the base directory of the `.object`/`.material` (`FUN_1007de40`; a leading `/` or `~/` means VFS root). Empty → flag only, no texture. 4 351 of 4 398 resolve to a `.dds` in the archives; the 47 that do not are in `metro/objects/bridges` and `blockhouses`. | `0x1008ec70.c:148-172` | high |

Slot semantics (flags from §2.11; *key* = texture-map key, which decides the unit order):

| tag | slot | key | flags set | header-driven extras | evidence |
|---|---|---|---|---|---|
| 0x02013110 | diffuse | 0 | `DIFFUSEMAP` 0x4000; b2=0,b3=1 → `ALPHATEST` 2; b2=0,b3>1 → `ALPHABLEND` 1; b2=1,b3≠0 → `COLORIZE` 0x10000 | — | `0x1008ec70.c:145-186` |
| 0x02013130 | specular | 1 | `SPECULARMAP` 0x10 | setSpecular(white × b24, max(1,b25), b27/255) | `0x1008ec70.c:222-263` |
| 0x02013120 | normal | 2 | `NORMALMAP` 8; b2=1,b3≠0 → `COLORIZE` 0x10000 | — | `0x1008ec70.c:187-221` |
| 0x02013140 | illumination | 3 | `ILLUMINATIONMAP` 0x20000 | — | `0x1008ec70.c:264-295` |
| 0x02013150 | ambient occlusion | 4 | `AMBIENTOCCLUSIONMAP` 0x100000 | — | `0x1008ec70.c:296-326` |
| (none) | environment cube | 5 | when `REFLECTION` is set: `textures/envmap.dds` | — | `0x1008ec70.c:332-350` |

**Texture units are assigned in ascending key order, packed** (`0x1008ec70.c:352-391`: map iteration, setTexture(i++)).
For example, diffuse + normal → units 0, 1. At most 7 maps; more than that logs `Too many maps used in material`. This matches
`shaders/default/maps.fragment` (diffuse → specular → normal → illumination → AO), with the reflection cube in the
next unit (`reflection.fragment`).

**Source-path chunks** (children of a slot, never read by the engine):

| tag | content | where | evidence |
|---|---|---|---|
| 0x02013111 | NUL-terminated Windows path of the source image (`G:\GSystem\…\bus_b.png`). The chunk size is the string length + NUL rounded up to 4. | every slot (4 398) | validator [obs] |
| 0x02013112 | path of the **alpha-channel source**: e.g. `colognedom_a.png` (opacity) for diffuse (800 slots), `bus_c.png` (colorize mask, next to `bus_n.png` in 0x02013111) for normal (351 slots) | diffuse and normal only | validator [obs] |

### 2.11 Material flags produced by the loader

| bit | constant (native-api.md) | source |
|---|---|---|
| 0x00000001 | MATERIAL_ALPHABLEND | header opacity ≠ 255, or diffuse slot b2=0,b3>1. Cleared if DECAL. |
| 0x00000002 | MATERIAL_ALPHATEST | diffuse slot b2=0,b3=1 |
| 0x00000004 | MATERIAL_SPECULAR | specularLevel > 4 and specular rgb ≠ 0 |
| 0x00000008 | MATERIAL_NORMALMAP | slot 0x02013120 |
| 0x00000010 | MATERIAL_SPECULARMAP | slot 0x02013130 |
| 0x00000040 | MATERIAL_SORTALPHA | ALPHABLEND and gsObject+0x184 (`0x10090170.c:74-76`) |
| 0x00001000 | MATERIAL_TWOSIDED | header 0x22 |
| 0x00002000 | MATERIAL_REFLECTION | header 0x27 ≠ 0 |
| 0x00004000 | MATERIAL_DIFFUSEMAP | slot 0x02013110 |
| 0x00008000 | MATERIAL_DECAL | header 0x21 bit 1 |
| 0x00010000 | MATERIAL_COLORIZE | slot b2=1 and b3≠0 (diffuse or normal) |
| 0x00020000 | MATERIAL_ILLUMINATIONMAP | slot 0x02013140 |
| 0x00100000 | MATERIAL_AMBIENTOCCLUSIONMAP | slot 0x02013150 |
| 0x01000000 | (unnamed) UV2 primary | header 0x21 bit 2 |
| 0x08000000 | (unnamed) keep own material on roads | header 0x20 == 0x1F |

The other MATERIAL_* bits (ADDITIVE, GLOW, MIX2/3MAP, POINT, SWAP_UV 0x200000 = swap u/v, LAYER, LAYERABLE, NO_SHADOW,
ZERO_RADIUS) are only set by scripts or by game code, never by the file loader.

### 2.12 Runtime GPU vertex formats (reference only; built from the 36-byte file vertex)

`gsSubMesh_OGL` (`0x10049710.c`) uploads one of three interleaved VBO layouts. `0x10048160.c:24-84` binds them.
Tangents are **computed at load time from UVs** (`0x10048890.c`); the file stores none.

| layout | when | attributes (offset: type) |
|---|---|---|
| 32 B "A" | no tangents, no UV2, no skin | 0x00 pos 3×f32 · 0x0c color 4×u8 · 0x10 normal 3×s16 (+pad) · 0x18 uv0 2×f32 |
| 32 B "B" | tangents (normal map), no vertex colours, no UV2, no skin | 0x00 pos · 0x0c normal 3×s8 (+pad) · 0x10 tangent 4×s16 (TEXCOORD2) · 0x18 uv0 |
| 64 B | skin, UV2 in use, or normal map with colours | 0x00 pos · 0x0c color · 0x10 normal 3×s16 · 0x18 tangent 4×s16 (TEXCOORD2) · 0x20 uv0 · 0x28 uv1 · 0x30 4×s16 weights (TEXCOORD3, ×1/32767) · 0x38 4×s16 bone palette indices (TEXCOORD4) |

The shader side (`shaders/default/geometry.vertex:32-36`) confirms weights = TEXCOORD3 × 1/32767 and indices = TEXCOORD4.

---

## 3. Machine-readable layout

```json
{
  "format": "GSystem mesh/material chunks (Cities in Motion 1.0.22)",
  "endianness": "little",
  "chunk": { "tag": "u32", "size": "u32 (body bytes, excludes 8-byte header)" },
  "conventions": {
    "handedness": "right", "up": "+Y", "unit": "metre (approx.)", "frontFace": "CCW", "triangleList": true,
    "quaternion": ["x", "y", "z", "w"], "color": "RGBA8 in byte order",
    "uv": "v stored negated (v_file = -v_max); glTF v = v_file + 1 (or keep with REPEAT)"
  },
  "files": {
    ".object": { "root": "0x02000000" },
    ".material": { "root": "0x02013100", "lookup": "<object dir>/<material name>.material" }
  },
  "chunks": {
    "0x02000000": { "name": "object", "header": [
        { "name": "name", "type": "char", "count": 32, "offset": 0 },
        { "name": "nodePosition", "type": "f32", "count": 3, "offset": 32 },
        { "name": "nodeRotation", "type": "f32", "count": 4, "offset": 44, "note": "quaternion xyzw, may be non-unit" }
      ], "headerSize": 60,
      "children": ["0x02010000", "0x02020000", "0x02030000", "0x02040000", "0x02050000", "0x02060000", "0x02070000", "0x02600000", "0x02700000"] },
    "0x02010000": { "name": "mesh.render", "meshFlags": "0x02", "lod": 0, "headerSize": 0, "children": ["0x02011000", "0x02012000", "0x02013000"] },
    "0x02020000": { "name": "mesh.collision", "meshFlags": "0x01", "headerSize": 0, "children": ["0x02011000", "0x02012000", "0x02013000"], "rendered": false, "shipped": 0 },
    "0x02030000": { "name": "mesh.hit", "meshFlags": "0x04", "headerSize": 0, "children": ["0x02011000", "0x02012000", "0x02013000"], "rendered": false, "shipped": 0 },
    "0x02040000": { "name": "mesh.lod1", "meshFlags": "0x12", "lod": 1, "headerSize": 0, "children": ["0x02011000", "0x02012000", "0x02013000"] },
    "0x02050000": { "name": "mesh.lod2", "meshFlags": "0x22", "lod": 2, "headerSize": 0, "children": ["0x02011000", "0x02012000", "0x02013000"], "shipped": 0 },
    "0x02060000": { "name": "mesh.lod3", "meshFlags": "0x42", "lod": 3, "headerSize": 0, "children": ["0x02011000", "0x02012000", "0x02013000"], "shipped": 0 },
    "0x02070000": { "name": "mesh.unflagged", "meshFlags": "0x00", "headerSize": 0, "children": ["0x02011000", "0x02012000", "0x02013000"], "rendered": false, "shipped": 0 },
    "0x02011000": { "name": "vertices", "header": [
        { "name": "vertexCount", "type": "u32", "offset": 0 }
      ], "array": { "countField": "vertexCount", "stride": 36, "format": "vertex36" },
      "children": ["0x02011100", "0x02011200"] },
    "0x02011100": { "name": "skin", "header": [
        { "name": "influencesPerVertex", "type": "u16", "offset": 0 },
        { "name": "reserved", "type": "u16", "offset": 2 }
      ], "array": { "count": "vertexCount * influencesPerVertex", "stride": 8, "format": "influence8", "order": "vertex-major" },
      "setsMeshFlags": "0x100", "engineMaxInfluences": 4 },
    "0x02011200": { "name": "uv2", "header": [
        { "name": "type", "type": "u16", "offset": 0, "mustEqual": 2 },
        { "name": "reserved", "type": "u16", "offset": 2 }
      ], "array": { "count": "vertexCount", "stride": 8, "format": "uv8" } },
    "0x02012000": { "name": "indices", "header": [
        { "name": "indexCount", "type": "u32", "offset": 0 }
      ], "array": { "countField": "indexCount", "stride": 4, "type": "u32", "note": "absolute vertex indices, triangle list" } },
    "0x02013000": { "name": "materialList", "header": [
        { "name": "count", "type": "u32", "offset": 0 }
      ], "children": ["0x02013100"] },
    "0x02013100": { "name": "material", "headerSize": 72, "header": "materialHeader72",
      "children": ["0x02013110", "0x02013120", "0x02013130", "0x02013140", "0x02013150"],
      "note": "embedded in .object: 72 bytes, no children; ranges used from here. In .material: appearance + slots used from here." },
    "0x02013110": { "name": "slot.diffuse", "textureKey": 0, "flag": "0x4000", "headerSize": 40, "header": "slotHeader40", "children": ["0x02013111", "0x02013112"] },
    "0x02013130": { "name": "slot.specular", "textureKey": 1, "flag": "0x10", "headerSize": 40, "header": "slotHeader40", "children": ["0x02013111"] },
    "0x02013120": { "name": "slot.normal", "textureKey": 2, "flag": "0x8", "headerSize": 40, "header": "slotHeader40", "children": ["0x02013111", "0x02013112"] },
    "0x02013140": { "name": "slot.illumination", "textureKey": 3, "flag": "0x20000", "headerSize": 40, "header": "slotHeader40", "children": ["0x02013111"] },
    "0x02013150": { "name": "slot.ambientOcclusion", "textureKey": 4, "flag": "0x100000", "texcoord": 1, "headerSize": 40, "header": "slotHeader40", "children": ["0x02013111"] },
    "0x02013111": { "name": "sourcePath", "type": "cstring", "readByEngine": false },
    "0x02013112": { "name": "alphaSourcePath", "type": "cstring", "readByEngine": false },
    "0x02600000": { "name": "locator", "size": 64, "fields": [
        { "name": "id", "type": "u32", "offset": 0 },
        { "name": "name", "type": "char", "count": 32, "offset": 4 },
        { "name": "position", "type": "f32", "count": 3, "offset": 36 },
        { "name": "rotation", "type": "f32", "count": 4, "offset": 48 }
      ] },
    "0x02700000": { "name": "camera (gsFixedCamera)", "size": 88, "shipped": 0, "fields": [
        { "name": "name", "type": "char", "count": 40, "offset": 4, "confidence": "medium" },
        { "name": "vecA", "type": "f32", "count": 3, "offset": 44, "confidence": "low", "note": "eye position?" },
        { "name": "fA", "type": "f32", "offset": 56, "confidence": "low" },
        { "name": "vecB", "type": "f32", "count": 3, "offset": 60, "confidence": "low", "note": "target/direction, rotated by object rotation" },
        { "name": "intParam", "type": "i32", "offset": 72, "confidence": "low" },
        { "name": "rollDeg", "type": "f32", "offset": 76, "confidence": "low" }
      ] }
  },
  "structs": {
    "vertex36": { "stride": 36, "attributes": [
        { "name": "POSITION", "type": "f32", "offset": 0, "components": 3 },
        { "name": "NORMAL", "type": "f32", "offset": 12, "components": 3 },
        { "name": "COLOR_0", "type": "u8", "normalized": true, "offset": 24, "components": 4, "order": "RGBA", "none": "0xFFFFFFFF" },
        { "name": "TEXCOORD_0", "type": "f32", "offset": 28, "components": 2, "vNegated": true }
      ] },
    "uv8": { "stride": 8, "attributes": [
        { "name": "TEXCOORD_1", "type": "f32", "offset": 0, "components": 2, "vNegated": true }
      ] },
    "influence8": { "stride": 8, "attributes": [
        { "name": "bone", "type": "u16", "offset": 0, "components": 1, "note": "index into skeleton 0x02201000 records (mesh node = 0)" },
        { "name": "reserved", "type": "u16", "offset": 2, "components": 1 },
        { "name": "weight", "type": "f32", "offset": 4, "components": 1, "note": "renormalise first 4 to sum 1" }
      ] },
    "materialHeader72": { "size": 72, "fields": [
        { "name": "name", "type": "char", "count": 32, "offset": 0 },
        { "name": "b20", "type": "u8", "offset": 32, "note": "0x1F -> flag 0x08000000" },
        { "name": "options", "type": "u8", "offset": 33, "bits": { "1": "DECAL 0x8000 + depth bias -1", "2": "UV2 primary 0x01000000" } },
        { "name": "twoSided", "type": "u8", "offset": 34, "flag": "0x1000" },
        { "name": "opacity", "type": "u8", "offset": 35, "note": "!=255 -> ALPHABLEND, diffuse.a *= v/256" },
        { "name": "specularLevel", "type": "u8", "offset": 36, "note": "spec *= v/256; >4 -> SPECULAR" },
        { "name": "glossiness", "type": "u8", "offset": 37, "note": "exponent max(1,v)" },
        { "name": "selfIllumination", "type": "u8", "offset": 38, "note": "emissive *= v*0.1" },
        { "name": "reflection", "type": "u8", "offset": 39, "note": "v/255; !=0 -> REFLECTION + envmap" },
        { "name": "ambient", "type": "u8", "count": 4, "offset": 40, "order": "RGBA" },
        { "name": "diffuse", "type": "u8", "count": 4, "offset": 44, "order": "RGBA" },
        { "name": "specular", "type": "u8", "count": 4, "offset": 48, "order": "RGBA" },
        { "name": "emissive", "type": "u8", "count": 4, "offset": 52, "order": "RGBA" },
        { "name": "firstVertex", "type": "u32", "offset": 56 },
        { "name": "vertexCount", "type": "u32", "offset": 60 },
        { "name": "firstIndex", "type": "u32", "offset": 64 },
        { "name": "indexCount", "type": "u32", "offset": 68 }
      ] },
    "slotHeader40": { "size": 40, "fields": [
        { "name": "disabled", "type": "u8", "offset": 0, "note": "slot used only if 0" },
        { "name": "amount", "type": "u8", "offset": 1, "note": "always 255, unread" },
        { "name": "alphaUsage", "type": "u8", "offset": 2, "note": "0 opacity, 1 colorize mask" },
        { "name": "alphaBits", "type": "u8", "offset": 3, "note": "0, 1 or 8" },
        { "name": "unknown4", "type": "u8", "count": 2, "offset": 4, "note": "always FF FF" },
        { "name": "unknown6", "type": "u8", "count": 2, "offset": 6, "note": "always 0" },
        { "name": "texture", "type": "char", "count": 32, "offset": 8, "note": "relative to the asset directory" }
      ] }
  },
  "materialFlags": {
    "ALPHABLEND": "0x1", "ALPHATEST": "0x2", "SPECULAR": "0x4", "NORMALMAP": "0x8", "SPECULARMAP": "0x10",
    "SORTALPHA": "0x40", "TWOSIDED": "0x1000", "REFLECTION": "0x2000", "DIFFUSEMAP": "0x4000", "DECAL": "0x8000",
    "COLORIZE": "0x10000", "ILLUMINATIONMAP": "0x20000", "AMBIENTOCCLUSIONMAP": "0x100000",
    "UV2_PRIMARY(unnamed)": "0x1000000", "ROAD_KEEP_MATERIAL(unnamed)": "0x8000000"
  },
  "textureUnitOrder": ["diffuse", "specular", "normal", "illumination", "ambientOcclusion", "envmapCube"]
}
```

---

## 4. Validation

Scripts (read-only, in-memory, Node ≥ 20) in
`C:\Users\Marku\AppData\Local\Temp\claude\C--Users-Marku-Cities-in-Motion\fe525a7e-5e90-46f7-b12c-445b2bb3c3e0\scratchpad\assets-mesh\`:
`parse.mjs` (reference parser), `validate.mjs` (all checks; `--addons` adds the third-party mod archives,
`--json out.json` writes statistics), `iter.mjs` (archive iteration with `tools/gstool.mjs`), `survey.mjs`
(chunk-path census), and `jsoncheck.mjs`, a generic walker driven **only by the JSON block of section 3**. Results:
`result-shipped.json`, `result-with-addons.json`.

**Shipped set: 2 747 `.object` + 1 556 `.material` from the 17 top-level `.gs` archives that contain them**
(objects.gs 1 875, usa 901, tokyo 860, london 251, stpetersburg 149, paris 132, …).

| check | result |
|---|---|
| every chunk consumed exactly (no overrun, no trailing bytes, all sizes match the formulas) | **0 issues** in 4 303 files |
| section-3 JSON alone drives an exact parse (`jsoncheck.mjs`) | 4 303 files, 50 740 chunks, 0 errors; 654 912 sampled vertex components decoded, all finite |
| tags seen | root 0x02000000 only; meshes 0x02010000 ×2 747, 0x02040000 ×394; locator 0x02600000 ×17 790; slots 0x02013110/20/30/40/50. 0x02020000/30000/50000/60000/70000, 0x02700000 and 0x02013112 outside slots never occur. |
| `4 + vertexCount·36 + children == size(0x02011000)` | 3 141 / 3 141 |
| `4 + 4·indexCount == size(0x02012000)` | 3 141 / 3 141 |
| skin `4 + V·n·8 == size`, UV2 `4 + V·8 == size` | 236 / 236, 1 759 / 1 759 |
| index < vertexCount | 0 violations (5 133 079 vertices, 3 146 195 triangles; largest mesh 68 450 vertices) |
| indexCount % 3 == 0 / zero-area triangles | 0 / 0 |
| positions, normals, UV0, UV1 finite | 0 non-finite values |
| normals unit length (±1 %) | 5 132 982 yes, 97 zero-length, 0 other |
| winding vs normals | 3 143 722 agree, 2 473 disagree (0.08 %); every file has an agreeing majority |
| bounding boxes | Y-up, median min.y 0; extents median 12 × 3.4 × 12 m, p95 58 × 39 × 44 m. One outlier: `usa/…/apartment_building05_apartment_building05.object`, 4 474 × 4 885 × 6 731 (not inspected; probably stray vertices). |
| submesh ranges | 4 141 records in 3 141 meshes (527 multi-material); index and vertex ranges are contiguous and sum exactly to the totals in **3 141 / 3 141**. Every index lies in its submesh vertex range (0 violations). |
| material-list count == children | 3 141 / 3 141; every embedded record is exactly 72 bytes |
| bone index < bone count (`0x02201000` records in a `.skeleton` in the object's dir or `<dir>/<stem>/`) | **236 / 236**. The first pass failed 3 `ferris-wheel.object` copies whose skeleton lives in a `ferris-wheel/` subfolder (29 bones, max index 26). |
| skin weights | n = 4 everywhere; weights ≥ 0; sum = 1 ± 0.001 for 502 090 vertices, 0 for 637 (unskinned), 0.80–0.999 for 274 (the engine renormalises) |
| reserved u16s (skin header, influence pad, UV2 header) | always 0 |
| external `.material` found for embedded name | 4 072 / 4 141 (69 missing → untextured defaults) |
| external vs embedded ranges | equal 891, **differ 3 181** → engine must use the embedded ranges (§2.8) |
| `.material` slot texture resolves to an archive `.dds` | 4 351 / 4 398 |
| slot `alphaBits` vs DDS | b3=8: 527 DXT5 / 3 DXT1; b3=1: 594 DXT1 / 11 DXT5; b3=0: mostly DXT1 |
| UV ranges | u ∈ [0,1] 99.1 %; v ∈ [−1,0) 98.9 %; tiling outliers up to ±2·10⁵ in a few London buildings |

**With third-party mods** (`--addons`: Mainline-station, Schwebebahn, TranspropsComplete, ashcampaign, pmc;
4 more archives use an unsupported directory layout): 3 025 objects, 1 735 materials, same tag set. The only failures
are 4 hand-edited `.material` files: `pmc/…/coolingTower.material` (×2) and `solarArray.material` carry 4–5 trailing
bytes after the root chunk (harmless for the engine), and `ashcampaign/sign/01___Default.material` declares its root
3 bytes longer than the file (the engine's behaviour on such a file was not tested).

---

## 5. Open questions

1. **Header byte 0x20** (`b20`): only `0x1F` has an effect (flag 0x08000000). Is it a 3ds Max shader/material-type
   enum? What is the exact semantics of 0x08000000 in the road builder (`0x100d6ab0.c:651,1156,2655,3024`)?
2. **Slot header bytes 1, 4–7** (always `FF / FF FF / 00 00`) are never read. They are probably Max map amount and
   tiling/flags. Byte 0 ("disabled") was never non-zero.
3. **Reflection byte 0x27** has almost the same histogram as specularLevel 0x24, so about 1 044 shipped materials get
   `MATERIAL_REFLECTION` and the cube map. Was that the artists' intent, or did the exporter write specular level into
   both bytes? (The engine behaviour itself is certain.)
4. **0x02070000** is a real tag (tested with `==`) that sets no mesh flag, so gsObject builds no render mesh from it.
   It may be meant for a subclass override; none of the four classes sharing `0x100882c0` overrides +0x188 except
   gsAreaDefinition (positions + indices only).
5. **0x02700000** camera fields (88 bytes) come from one reader with a confused stack, and no sample exists. The
   same goes for the **0x02100000** root and its **0x02080000** child (handled by +0x148 `0x10090e70`, not analysed).
6. **Bone index ↔ skeleton**: indices are below the `0x02201000` record count in every case. The exact mapping (record order
   vs. name lookup through `0x02201010` mesh links) belongs to the skeleton format document.
7. **Locator `id`** (u32 at 0) is always 0; its purpose is unknown.
8. Root-quaternion **non-unit norms** (20 files) imply baked uniform scale. Whether the engine normalises them when
   `applyTransform` is set has not been checked.
9. The **vegetation pass** flag `gsObject+0x19d` is never set in the constructor. Which script property sets it was not
   traced, so it is not known which object classes use the black / green vertex-colour markers (§2.4).

### Appendix: notes for a glTF exporter

* Axes and winding need no conversion (right-handed, +Y up, CCW). Set `v = v_file + 1` for TEXCOORD_0/1.
* One glTF primitive per embedded material record, using `indices[firstIndex … +indexCount]`. The indices can be kept
  absolute (shared vertex buffer) or rebased by `firstVertex`.
* Material: diffuse slot → `baseColorTexture` (alpha mode from §2.11: ALPHATEST → MASK, ALPHABLEND → BLEND); diffuse
  colour × opacity → `baseColorFactor`; normal slot → `normalTexture` (alpha may hold a colorize mask); AO slot →
  `occlusionTexture` with **`texCoord: 1`**; illumination slot → `emissiveTexture`, `emissiveFactor = emissive × b26 × 0.1`;
  specular slot → `KHR_materials_specular` or a spec-gloss approximation (exponent = b25); TWOSIDED → `doubleSided`.
* Skin: JOINTS_0 = bone (u16), WEIGHTS_0 = renormalised weight; the joint order comes from the skeleton file.
* COLOR_0 only for vegetation/tree shaders (×2); the default shader ignores vertex colours.
* Locators → empty nodes (`extras.locator = true`) with name, translation and rotation.
