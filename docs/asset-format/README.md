# Cities in Motion 3D asset formats

Status: **all formats decoded and validated on every shipped file** (0 structural errors).

| Document | Covers |
|---|---|
| [mesh-material.md](mesh-material.md) | `.object` meshes (vertices, skin, second UV set, indices, LODs, embedded materials), `.material` |
| [skeleton-animation-scene.md](skeleton-animation-scene.md) | `.skeleton`, `.animation`, `.scene`, `.properties`, `.font` |

Tool: `tools/assetparse.mjs`

```
node tools/assetparse.mjs check <gameDir>                          parse every shipped asset, report issues
node tools/assetparse.mjs export <file.object> <out.glb> [out.png]  mesh -> glTF 2.0 (+ shaded preview)
node tools/assetparse.mjs info <file.object|file.material>          decoded structure as JSON
```

## How an asset is put together

A typical model folder (vehicle, building or character) contains:

* `<name>.object` — the mesh: one render mesh, optionally a lower-detail `lod1` mesh, plus named locators.
* `<material>.material` — appearance per submesh, matched by name. Submesh ranges always come from the `.object`.
* `<name>_scene.skeleton` — the node and bone hierarchy (also used for rigid parts such as bus doors).
* `*.animation` — takes with one track per bone, bound by bone name.
* `<name>_scene.properties` — the class to instantiate, draw distances and sub-objects.
* `<name>_scene.scene` — an exporter by-product. GSystem never loads it at run time.
* `<name>.script` — the game logic that loads all of the above.

Everything was exported from 3ds Max: default names like "01 - Default" and "Take 001", and source paths
`G:\GSystem\…` that the engine never reads.

## Conventions

* Right-handed, +Y up, metres, counter-clockwise front faces; models face −Z with their right side on +X.
* Texture V is stored negated: glTF `v = v_file + 1`.
* Quaternions are stored `x, y, z, w` using the engine's row-vector maths. For glTF use the conjugate
  `(−x, −y, −z, w)`. This reproduces the engine's bind matrices for all 32 222 shipped bones.
* Bone local matrix = `scale · rotation · translation` (row vectors), world = `local · parentWorld`. Parents always
  precede children.
* Inverse bind matrices are not stored; the engine computes `inverse(bindWorld)`.

## Mesh `.object` (root `0x02000000`)

* **Header (60 B):** name, then the 3ds Max node's position and rotation; vertices are local to that node.
* **Mesh chunks:** `0x02010000` render mesh and `0x02040000` lod1. Collision, hit and lod2/lod3 tags exist in the
  loader but never ship.
* **Vertices** `0x02011000`: `u32 count`, then 36-byte vertices: position `f32[3]`, normal `f32[3]`, colour RGBA8,
  uv `f32[2]`.
* **Optional children of the vertex chunk:**
  * skin `0x02011100`: 4 influences per vertex, each `{u16 bone, u16 0, f32 weight}`; bones are indexed in
    `.skeleton` order;
  * second UV set `0x02011200`, used by the ambient-occlusion map.
* **Indices** `0x02012000`: `u32 count`, then `u32` absolute indices forming a triangle list.
* **Materials** `0x02013000`: `u32 count`, then one 72-byte material header per submesh. Each header gives the
  submesh's vertex and index range plus default appearance.

## Material `.material` (root `0x02013100`)

* **72-byte header:** name, option bytes (decal, UV2-primary, two-sided, opacity, specular level, glossiness,
  self-illumination, reflection), ambient/diffuse/specular/emissive RGBA colours, and the submesh ranges.
* **Up to five texture slots,** each a 40-byte header with a disabled flag, alpha usage, alpha bit depth and a
  32-character texture file name:

| Tag | Slot |
|---|---|
| `0x02013110` | diffuse |
| `0x02013120` | normal |
| `0x02013130` | specular |
| `0x02013140` | illumination |
| `0x02013150` | ambient occlusion (second UV set) |

## Skeleton, animation, properties, font

| File | Layout |
|---|---|
| `.skeleton` `0x02200000` | `{name[32], u32 boneCount}`; bones `0x02201000` (76 B): `name[32], i32 parent, t[3], s[3], q[4]`; optional `0x02201010` marks a bone that carries a rigidly attached mesh node of the same name |
| `.animation` `0x02300000` | `{name[32], u32 trackCount, f32 duration}`; tracks `0x02301000`: `{boneName[32], u32 keyCount}`; keys `0x02301010` (40 B): `t[3], s[3], q[4]` with no time field — key i is at `i·duration/(n−1)`; translation and scale lerp; rotation is a normalised lerp with a hemisphere sign flip |
| `.properties` `0x02400000` | `name[32], className[32]` (`"object"` or `"moveableObject"`), alpha sorting, min/max draw distance; sub-objects to instantiate (`0x02403xxx`) |
| `.font` `0x03000000` | 256 cp1252 glyph rectangles (`f32 u, v, w, h`) for the same-named `.dds`; files starting `BMF\x03` are AngelCode BMFont |

## Validation

Two independent parsers agree on every count: each decoder's own validation scripts, and `tools/assetparse.mjs`.

| Format | Files | Notes |
|---|---|---|
| `.object` | 2 747 | 3 141 meshes (394 with lod1), 236 skinned, 5.13 M vertices, 3.15 M triangles; every index in range; submesh ranges contiguous and complete |
| `.material` | 1 556 | |
| `.skeleton` | 1 482 | 32 222 bones; parents always earlier |
| `.animation` | 250 | 4 301 tracks, 1.85 M keys; worst quaternion norm error 2·10⁻⁷ |
| `.scene` | 1 467 | |
| `.properties` | 1 482 | |
| `.font` | 5 | |

* **Rendered meshes:** shaded previews of the Cologne cathedral and both Humble-pack bus meshes decode to clean,
  correctly connected geometry. They are kept locally in `samples/previews`.
* **Semantic checks:** the bus door turns 90° about +Y on exactly the timing its script expects, and a pedestrian's
  bind pose stands upright at 1.80 m.
* **Data the engine tolerates:**
  * seven animations declare more tracks than they contain;
  * fifteen animations target bones absent from their folder's skeleton (none is paired by a script);
  * 123 scene references point at missing `.object` files (`.scene` is never loaded);
  * a few hand-edited mod `.material` files have stray bytes.

## Open questions

* A few material bytes are unread, including byte `0x20` and flag `0x08000000`, which appears only on road surfaces.
* The space rigidly attached meshes are authored in.
* Fields of the unused camera (`0x02700000`), collision (`0x02401000`/`0x02402000`) and unit-scale (`0x02405000`)
  chunks, which no shipped file uses or the engine never reads.
