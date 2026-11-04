# Skeleton, animation, scene, properties and font formats (GSystem.dll)

Scope: `.skeleton` (0x02200000), `.animation` (0x02300000), `.scene` (0x02100000), `.properties` (0x02400000) and `.font` (0x03000000) as read by `GSystem.dll` of Cities in Motion (2011).

Sources:
- Ghidra 12.1.4 pseudocode in `decompiled/GSystem-v2/functions/0x<addr>.c`. Line numbers below refer to those files.
- A strict read-only parser and validator run over every shipped file: `cimanim.mjs` (parsers, engine sampler, engine matrix math) and `validate.mjs`, with JSON reports `report-shipped.json` / `report-all.json`. All are in `C:\Users\Marku\AppData\Local\Temp\claude\C--Users-Marku-Cities-in-Motion\fe525a7e-5e90-46f7-b12c-445b2bb3c3e0\scratchpad\assets-anim\` (temporary; copy to `tools/` to keep).

Confidence and evidence:
- **observed** means read directly from the reader code or measured on every shipped file.
- **inferred** means deduced from data, naming or downstream code, but not proven by a reader.

All integers and floats are little-endian. All files use the generic GSystem chunk framework: `u32 tag, u32 size, body[size]`. A body may be a fixed header followed by child chunks. Readers are `FUN_1007bd90` (next child), `FUN_1007be00` (pointer to n body bytes) and `FUN_1007c000` (skip). Every reader skips child tags it does not know, so unknown chunks are harmless to the engine.

## 1. Overview

### 1.1 How the files combine

An asset exported from 3ds Max (via FBX, see `0x02100001`) produces a set of files with a common stem, usually `<name>_scene.*`:

| File | Loaded by | Purpose at runtime |
|---|---|---|
| `X.object` | grid/vehicle code (`$grid.loadGridObject(id, "….object")`) | Mesh data (other doc). Loading it also tries `X.properties` and `X.script` with the same stem (`0x1008bd90.c:65-93`). |
| `X_scene.skeleton` | script `this.loadSkeleton("X_scene.skeleton")` (`gsObject`, handler `0x1008b7d0` -> parser `0x1006fbc0`) | Node hierarchy, i.e. every node of the exported Max scene: bones, dummies and mesh nodes. Its bind pose drives skinning and rigid attachments. |
| `*.animation` | script `.loadAnimation(slot, "file.animation", flags)` on the skeleton (`0x10070b40` -> `0x1006f080` cache -> `0x1006c730`) | One take: per-node TRS tracks bound to skeleton bones by name. |
| `X_scene.scene` | only when a `.scene` file itself is loaded as an object (`0x1008bd90.c:119`). **No shipped script does this** (0 live references; the only one, in `metro/ui/menus/animation/animation.script`, is commented out). | Exporter scene description: source FBX path, one instance node per exported mesh (`.object` + transform), locators, cameras. |
| `X_scene.properties` | automatically, for any object or scene file with the same stem (`0x1008bd90.c:65-70`, `0x100909b0.c:65-88`) | Class to instantiate (`object`, `moveableObject`), `sortAlpha`, draw distances, and an optional list of sub-objects to instantiate with transforms. It also carries exporter metadata. |
| `*.font` | font manager (`0x1003fb40`) | 256-glyph UV table for `<same stem>.dds`. |

Runtime model of an animated vehicle or person (observed in scripts and code):
1. The game loads `bus_b.object` together with `bus_b.script` (there is no `bus_b.properties`).
2. The script calls `loadSkeleton("bus_b_scene.skeleton")` and then `.loadAnimation("default", "bus_b_anim.animation", 0)`.
3. Animation file paths resolve against the skeleton's directory (batch-B.md).

The `_scene.scene` / `_scene.properties` pair is therefore mostly an exporter by-product. The skeleton is the runtime scene graph.

How the pieces link (all comparisons are exact, case-sensitive `std::string` compares):
- **animation track -> bone.** Each track name is matched to the first bone with the same name (`0x100685e0`, linear search, called from `0x1006d4f0`). Tracks without a matching bone are ignored, and bones without a track keep their bind pose.
- **bone -> mesh node.** A bone with a `0x02201010` child names a 3D child element (the `.scene` node / sub-object of the same name). That element is attached rigidly to the bone (`0x1006d290.c:52-77`). The root object itself is never matched (`param_2=1` on the top call). The name in `0x02201010` always equals the bone's own name (3585/3585).
- **bone -> skinned vertices.** gsSkeleton feeds bone index, parent and pose to the renderer skeleton `gsSkeleton_OGL` through the `gsISkeleton` interface at `+0x140` (`0x1006f7f0.c:102-103`, `0x100690e0`). Skinned sub-meshes reference bones **by index in file order** (gsSkeleton_OGL bone array, `0x10050700`). The mapping inside `.object` is out of scope here (inferred).
- **`.scene` node -> `.object`.** Node `file` is resolved against the scene's directory (`0x10090e70.c:42-47`).
- **`.properties` `0x02403100` -> `.object`.** Loads `<baseDir>/<name>.object` as a child (`0x100910b0.c:107-164`).

### 1.2 Conventions (observed)

- **Row vectors, row-major matrices** (D3DX style, `v' = v · M`). Translation sits in `m[12..14]` (`0x100867d0.c:19-21`). Products are `FUN_10084410(out,a,b) = a·b`. Matrices go to OpenGL unchanged (`glLoadMatrixf`, `0x100484c0`), because a row-major row-vector matrix has the same memory layout as a column-major column-vector matrix.
- **Bone local matrix** = `Scale(s) · RotTrans(q, t)`, i.e. `v · S · R · T`. Source: `0x10067af0.c:16-18`, renderer copy `0x10050110.c:39-48`.
- **World** = `local · parentWorld`, computed in file order (`0x1006f7f0.c:77-83`). A root's parent is the owning object's world matrix (`0x100690e0`) or, in the renderer, `S(uniform)·R·T` of the object (`0x100501f0.c:26-35`).
- **Quaternion** layout is `(x, y, z, w)`. The 3×3 built by `FUN_100859d0` is `[[1-2(y²+z²), 2(xy-zw), 2(xz+yw)], [2(xy+zw), 1-2(x²+z²), 2(yz-xw)], [2(xz-yw), 2(yz+xw), 1-2(x²+y²)]]`, stored row-major and applied to row vectors. In column-vector math (glTF, OpenGL docs) the effective rotation is therefore **conj(q) = (-x, -y, -z, w)**.
  - Checked on data: with this rule the blue-collar-male bind pose stands upright (head at y = 1.80 m, ankles at y = 0.08 m, wrists at ±0.75 m in T-pose). The opposite rule produces a broken figure (`explore1.mjs`).
- **Axes: +Y up, metres, right-handed.** Characters and vehicles face **−Z**, and the model's right is **+X**:
  - The knee of the walk cycle bends toward −Z, and `BCm_RightHip` is at x = +0.153.
  - The bus door path `doorR0-enter` runs from x = +2.0 to −0.5. The bus is 5.5 m long along Z.
  - Root bones usually carry the Max Z-up->Y-up correction `q = (-√½, 0, 0, √½)` (2593 of 3966 roots; 1055 roots are identity).
- **Bind pose / inverse bind matrices are not stored.**
  - gsSkeleton_OGL computes the bind world of every bone from the `.skeleton` TRS when the skeleton is built, and stores `inverse(bindWorld)` (`0x10050110.c:39-52`).
  - Per frame, the skin matrix is `invBind · currentWorld` (`0x100501f0.c:57-66`).
  - Skinned vertices are therefore in skeleton model space at the bind pose.
- **Time unit: seconds.** `take.duration` is in seconds (scripts crop with `frames/25` or `/30`). Keys carry **no time stamp**: track key *i* sits at `t_i = i · duration / (keyCount − 1)` (`0x10067840.c:38-47`). Each track has its own uniform rate. Observed rates are 6, 25, 30, 60 and 120 keys/s, and many odd ones (exporter key reduction).
- **Interpolation** (`0x10067840`):
  - `f = t / duration · (n − 1)`, `i0 = floor(f)`, `i1 = trunc(f + 0.99999)` (ceiling), `a = f − i0`.
  - Translation and scale use lerp (`0x10083790`).
  - Rotation uses **nlerp**, not slerp: `q = normalize(q0 + t'·(q1 − q0))` (`0x10085da0` -> normalise `0x10085b10`).
    - `t' = −t` when `dot(q0,q1) < 0`. This was verified on the raw x87 bytes at VA 0x10085da0: `fcomp [0x10156620]=0.0; jp; fld t; fchs; fstp t`.
    - This is **not** the textbook hemisphere flip `q1 -> −q1`. It extrapolates away from q1.
    - Only 4 adjacent key pairs in shipped data have `dot < 0`. One is a pure sign flip (0.03°). The other three are the same 102° pop in `e3_tom_anim.animation` / `TOm_B_FL2` key 54, copied in 3 archives.
  - Out-of-range indices are clamped to `n−1` if the slot does not loop (`DONT_LOOP` 0x20000, `0x100690e0.c:120-122`), and wrap to key 0 if it loops.
- **Playback time** = slot crop start + slot position (`0x100690e0.c:120`).
- **Blending.** Several slots blend per bone by weight. Flag `0x40000` replaces the pose by lerp/slerp with weight w. Flag `0x10000` is additive against a reference track (`0x100690e0.c:134-170`).

## 2. Chunk tables

Column legend: `off` = offset inside the chunk body. Confidence: **H** = observed in reader code and confirmed on all data; **M** = observed in data only or partially in code; **L** = guess.

### 2.1 `.skeleton` — 0x02200000 root (header 36 B, parser `0x1006fa80`)

| off | size | type | name | meaning | evidence | conf |
|---|---|---|---|---|---|---|
| 0x00 | 32 | char[32] | name | Skeleton name; always `"Default"` (1482/1482). Copied into gsSkeleton+0x150. | `0x1006fa80.c:28-34` | H |
| 0x20 | 4 | u32 | boneCount | Number of 0x02201000 children. Allocates `boneCount × 0xF8`. Equals the chunk count in 1482/1482 files. | `0x1006fa80.c:35-46` | H |
| 0x24 | … | chunks | bones | `boneCount` × 0x02201000, in hierarchy order (parent index < child index in 32222/32222). Other child tags are skipped. | `0x1006fa80.c:54-62` | H |

### 2.2 `.skeleton` — 0x02201000 bone (header 76 B, parser `0x1006f7f0`)

| off | size | type | name | meaning | evidence | conf |
|---|---|---|---|---|---|---|
| 0x00 | 32 | char[32] | name | Node name, e.g. `root-bone`, `BCm_RightKnee`, or the mesh node name (`bus_b`). Not unique in 391 bones; lookups take the first match. | `0x1006f7f0.c:28-37`, `0x100685e0` | H |
| 0x20 | 4 | i32 | parent | Parent bone index, or −1 for a root. Roots per file: 1 in 788 files, 2 in 513, up to 98. Always valid, acyclic, and < own index. | `0x1006f7f0.c:38,76-84` | H |
| 0x24 | 12 | f32[3] | t | Bind-pose local translation (metres, parent space). Copied to bind (+0x20) and current pose (+0x48). | `0x1006f7f0.c:39-42,53-56`, `0x100867d0.c:19-21` | H |
| 0x30 | 12 | f32[3] | s | Bind-pose local scale; applied before rotation. Non-unit in 1223 bones (229 non-uniform, 64 negative); range −2.54…100. | `0x1006f7f0.c:43-46`, `0x10084370` | H |
| 0x3C | 16 | f32[4] | q | Bind-pose local rotation (x,y,z,w); unit length (max error 1.3e-7). Effective rotation in column-vector math is conj(q). | `0x1006f7f0.c:47-52`, `0x100859d0` | H |
| 0x4C | … | chunks | children | 0..1 × 0x02201010 (other tags skipped). | `0x1006f7f0.c:92-99` | H |

### 2.3 `.skeleton` — 0x02201010 attached node (32 B, parser `0x1006f730`)

| off | size | type | name | meaning | evidence | conf |
|---|---|---|---|---|---|---|
| 0x00 | 32 | char[32] | node | Name of a 3D child element to attach rigidly to this bone. Pushed into the bone's name list (+0xC8); matched against child element names in `0x1006d290` and attached via list +0xD8. Always equals the bone's own name (3585/3585). 3458 of these resolve to a node of the same-stem `.scene`; the rest are skinned or unexported meshes. | `0x1006f730.c:26-35`, `0x1006d290.c:52-77` | H (mechanism) / M (use) |

In-memory bone (0xF8 bytes, for reference):
- `+0x00` name (std::string)
- `+0x1C` parent
- `+0x20/+0x2C/+0x38` bind t/s/q
- `+0x48/+0x54/+0x60` current t/s/q
- `+0x70/+0x7C` noise state
- `+0x88` world matrix
- `+0xC8` attached-name vector
- `+0xD8` attached-element list
- `+0xE4` animation entry list
- `+0xF0` use count
- `+0xF4` group mask

### 2.4 `.animation` — 0x02300000 take (header 40 B, parser `0x1006a980`)

| off | size | type | name | meaning | evidence | conf |
|---|---|---|---|---|---|---|
| 0x00 | 32 | char[32] | name | Take name, usually `"Take 001"` (229/250). Not NUL-terminated in 5 files (32-char names). Unused by the engine. | `0x1006a980.c:28-34` | H |
| 0x20 | 4 | u32 | trackCount | Allocates `trackCount × 0x28` track slots. Is ≥ the actual number of track chunks: equal in 243 files, larger in 7 (empty slots stay unnamed with 0 keys). Never smaller. | `0x1006a980.c:36,42-53` | H |
| 0x24 | 4 | f32 | duration | Take length in **seconds** (0.001 … 76.0). Default crop length; divisor in the sampler. | `0x1006a980.c:37`, `0x10067840.c:38-41`, batch-B.md | H |
| 0x28 | … | chunks | tracks | 0x02301000 × n (other tags skipped). | `0x1006a980.c:57-65` | H |

### 2.5 `.animation` — 0x02301000 track (header 36 B, parser `0x1006a850`)

| off | size | type | name | meaning | evidence | conf |
|---|---|---|---|---|---|---|
| 0x00 | 32 | char[32] | bone | Target bone (node) name; exact, case-sensitive match. | `0x1006a850.c:27-33`, `0x100685e0` | H |
| 0x20 | 4 | u32 | keyCount | Number of 0x02301010 keys, = chunk count in all 4301 tracks. Distribution: 1 key: 77 tracks, 2 keys: 621, 3: 28, more: 3575. | `0x1006a850.c:34-49` | H |
| 0x24 | … | chunks | keys | keyCount × 0x02301010, uniformly spaced over `[0, duration]`. | `0x1006a850.c:57-66` | H |

### 2.6 `.animation` — 0x02301010 key (40 B, parser `0x100677c0`)

| off | size | type | name | meaning | evidence | conf |
|---|---|---|---|---|---|---|
| 0x00 | 12 | f32[3] | t | Local translation (parent space, metres); replaces bone t. | `0x100677c0.c:15-20`, sampled into bone+0x48 (`0x100690e0.c:120-122`) | H |
| 0x0C | 12 | f32[3] | s | Local scale; replaces bone s (non-unit in 43720 keys). | `0x100677c0.c:21-24` -> bone+0x54 | H |
| 0x18 | 16 | f32[4] | q | Local rotation (x,y,z,w), unit (max error 2.1e-7). Only 4 adjacent key pairs lie in opposite hemispheres; for them the sampler negates t (see §1.2). | `0x100677c0.c:25-29` -> bone+0x60 | H |
| — | — | — | time | **Not stored.** `t_i = i·duration/(keyCount−1)`. For keyCount = 1 the pose is constant. | `0x10067840.c:38-47` | H |

Key 0 equals the skeleton bind pose in 3774 of 4229 bound tracks. Clips are therefore usually baked from the bind pose, but this is not guaranteed.

### 2.7 `.scene` — 0x02100000 root (no header, parser `0x100885c0` = gsObject vslot 0x144)

| child tag | size | layout | meaning | evidence | conf |
|---|---|---|---|---|---|
| 0x02100001 | var | char[size], NUL-terminated | Source file of the export, e.g. `G:\GSystem\addons\humble\bus\dev\bus_b_anim.FBX`. Exactly 1 per file, always `.FBX`. **Not read by GSystem.** | not handled in `0x100885c0.c:19-27` | M |
| 0x02080000 | 128 | see 2.8 | Object instance node | `0x100885c0.c:19-20` -> vslot 0x148 = `0x10090e70` | H |
| 0x02600000 | 64 | see 2.9 | Named locator (dummy/helper transform) | `0x100885c0.c:22-23` -> vslot 0x11c = `0x1008fdc0` | H |
| 0x02700000 | ≥88 | see 2.10 | Fixed camera | `0x100885c0.c:25-26` -> `0x10073310` | M (none shipped) |

### 2.8 `.scene` — 0x02080000 node (128 B, `0x10090e70`)

| off | size | type | name | meaning | evidence | conf |
|---|---|---|---|---|---|---|
| 0x00 | 4 | u32 | unk0 | Always 0 (3076/3076); ignored by the reader. | `0x10090e70.c` reads from +4 | M |
| 0x04 | 32 | char[32] | name | Element name; equals the file stem (3076/3076). Used for bone attachment (0x02201010). | `0x10090e70.c:61-68` | H |
| 0x24 | 64 | char[64] | file | `.object` path relative to the scene directory (always `.object`). Instantiated through `.properties` class lookup (`0x100909b0`). 2953 of 3076 exist in the archives; 123 are missing, harmless because scenes are not loaded. | `0x10090e70.c:42-47` | H |
| 0x64 | 12 | f32[3] | pos | Instance position = **bind world translation of the same-named bone** (3067/3067). | `0x10090e70.c:70-72` | H |
| 0x70 | 16 | f32[4] | rot | Instance orientation (x,y,z,w). Equals the bone's bind world rotation for child nodes (134/134). For root nodes it is the world rotation with the Z-up->Y-up correction removed (`(+√½,0,0,√½) ⊗ q_world`, 2077 cases) or equal to it (830). **Not always unit**: 26 nodes have scale folded in (‖q‖ 0.71…2.07). This is consistent with a trace-based matrix->quat (like `FUN_10085650`) applied to a scaled world matrix (inferred). Normalise before use. | `0x10090e70.c:73-76` | H (fields) / M (relation) |

The node loader is skipped entirely when the object's `.properties` contains a `0x02403000` list (flag +0x199, `0x10088670.c:16`, `0x10090e70.c:40`). That holds for 1480 of 1482 properties files.

### 2.9 `.scene` — 0x02600000 locator (64 B, `0x1008fdc0`)

| off | size | type | name | meaning | evidence | conf |
|---|---|---|---|---|---|---|
| 0x00 | 4 | u32 | unk0 | Copied into the locator record; always 0 (1069/1069). | `0x1008fdc0.c:35` | M |
| 0x04 | 32 | char[32] | name | Map key in gsObject+0x134. Looked up by script `locate(name, element)` (vslot 0x124 = `0x1008a1e0`), which sets element position/orientation. 10 names fill all 32 bytes (no NUL). | `0x1008fdc0.c:44-54`, `0x1008a1e0.c` | H |
| 0x24 | 12 | f32[3] | pos | Locator position (object space). | `0x1008fdc0.c:36-38` | H |
| 0x30 | 16 | f32[4] | rot | Locator orientation (x,y,z,w); 174 of 1069 are not unit (scale folded in). | `0x1008fdc0.c:39-42` | H |

Locators appear in 60 shipped scenes (1069 total), e.g. `BP1_Head_End` in basketball-field.

### 2.10 `.scene` — 0x02700000 camera (≥88 B, `0x10073310`)

Creates a `gsFixedCamera` (vtable 0x1015d7a4):
- `+0x04` char name[]
- `+0x2C` f32[3] position
- `+0x38` f32 (unknown)
- `+0x3C` f32[3] target or direction
- `+0x48` u32 (copied to camera+0x188)
- `+0x4C` f32 angle in degrees (×π/180)

Not present in any shipped file (confidence **L**).

### 2.11 `.properties` — 0x02400000 root (header 76 B, `0x1008bcd0` = vslot 0x158; class probe `0x100909b0.c:74-88`)

| off | size | type | name | meaning | evidence | conf |
|---|---|---|---|---|---|---|
| 0x00 | 32 | char[32] | name | Object name -> `gsObject.name` (+0x140). Exporter writes the scene file name (`bus_b_scene.scene`, truncated to 31 chars; 1427/1482 match `<stem>.scene`). | `0x1008bcd0.c:17-23` | H |
| 0x20 | 32 | char[32] | className | Script class to instantiate for this file: `object` (1481) or `moveableObject` (1). | `0x100909b0.c:76-88` (`FUN_10090800`) | H |
| 0x40 | 1 | u8 | sortAlpha | -> gsObject.sortAlpha (+0x184); always 0. | `0x1008bcd0.c:31` | H |
| 0x41 | 1 | u8 | autoDrawDistance | If ≠0, the explicit distances are ignored and `maxDrawDistance = ‖bbox diagonal‖·100` is computed after load. Always 0xFF. | `0x1008bcd0.c:24-29`, `0x1008bd90.c:125-129` | H / M (formula garbled) |
| 0x42 | 2 | u8[2] | pad | Uninitialised exporter memory (non-zero garbage in 831 files); ignore. | data | M |
| 0x44 | 4 | f32 | minDrawDistance | -> +0x208 when autoDrawDistance = 0; always 0.0. | `0x1008bcd0.c:27` | H |
| 0x48 | 4 | f32 | maxDrawDistance | -> +0x20C when autoDrawDistance = 0; 100.0 (1480) or 1000.0 (2). | `0x1008bcd0.c:28` | H |
| 0x4C | … | chunks | children | Each child is passed to vslot 0x15c = `0x10088670`, which only understands 0x02403000. | `0x1008bcd0.c:32-38` | H |

### 2.12 `.properties` — children

| tag | size | layout | meaning | evidence | conf |
|---|---|---|---|---|---|
| 0x02403000 | var | (no header) n × 0x02403100 | **Sub-object list.** Sets gsObject+0x199 = 1, which disables `.scene` node instancing. Present in 1480/1482 files. | `0x10088670.c:15-25` | H |
| 0x02403100 | 36 + children | `u32 unk0` (always 0, unread); `char name[32]` at +4; then 0..1 × 0x02403101 | Sub-object `<baseDir>/<name>.object`, instantiated after load (`0x100910b0`, vslot 0x13c). Names equal the same-stem `.scene` node names in 1466 of 1467 cases (one case differs only in letter case, `Koelnmesse`). | `0x10090370.c:34-60`, `0x100910b0.c:107-164` | H |
| 0x02403101 | 28 | `f32 pos[3]; f32 rot[4]` (x,y,z,w) | Sub-object transform in scene space. Converted into the parent's local frame: `pos·inverse(parentM)`, `rot ⊗ quat(inverse(parentM))`, where parentM is built from the parent's stored rot/pos +0x1D4/+0x1E4. Present on every 0x02403100 (3098/3098) and identical to the scene node transform (3075/3075). Without it, the child's own stored transform (+0x1D4/+0x1E4) is converted the same way. | `0x10090370.c:47-56`, `0x100910b0.c:96-125,163-164` | H |
| 0x02404000 | var | (no header) n × 0x02404100 | Source-file list (exporter metadata). **No reader in GSystem.** Present in 1480 files: empty (size 0) in 1259, 1 entry in 215, 2 in 6. | none found | M |
| 0x02404100 | var | (no header) 1 × 0x02404101 | One source entry. | data | M |
| 0x02404101 | var | char[size] | Path of a source FBX (`G:\GSystem\…\bus_b_anim.FBX`); 227 paths, all `.fbx`. | data | M |
| 0x02405000 | 16 | `f32 unitScale; f32 unk1; u32 unk2; u32 unk3` | Exporter settings, **not read**. unitScale is 0.01 (cm, 928 files), 0.0254 (inch, 324), 1.0 (163), 0.00393701 (12), 100 (6) and others (missing in 2 files). Vertex data is metres regardless. unk1 ∈ {0, 10, 20} (not related to key rate). unk2 ∈ {0, 1}; unk3 = 0. | data | M / L |
| 0x02401000 | 108 | `u8 flags[4]` (01 01 FF 00); `f32[6]` zero; `f32[4]` = 1.0; `char collisionFile[64]` (`arrow.collision`) | Collision/physics metadata; **not read by GSystem**. Only `metro/objects/misc/arrow.properties` and `horse_carriage_scene-combined.properties`. | data | L |
| 0x02402000 | 40 | f32[10] = 100, 0.03, 0.05, 0, 1, 1, 1, 0, 0, 0 | Physics parameters (mass? friction? …); not read. Same 2 files. | data | L |

Shipped child order is always one of two forms:
- `0x02403000, 0x02404000, 0x02405000` (1480 files);
- `0x02401000, 0x02402000` (2 files; these have no sub-object list, so their `.scene` nodes, if any, would be instanced).

### 2.13 `.font` — 0x03000000 (no header, `0x1003fb40` -> `0x1003f260`)

- If the file starts with `"BMF\x03"` (u32 0x03464D42) it is read as an AngelCode BMFont binary v3 instead (`0x1003fb40.c:44-56`, reader `0x1003f610`); no shipped `.font` uses that, but the 11 shipped `.fnt` language extras (`metro/fonts/*/{ja,pl,ru,languages}.fnt`) are BMFont v3, decoded in [docs/ui/fonts.md](../ui/fonts.md) §4.
- Otherwise the texture is `<font stem>.dds` in the same folder (`0x1003fb40.c:58-63`).

| child tag | size | layout | meaning | evidence | conf |
|---|---|---|---|---|---|
| 0x03010000 | 4096 | 256 × `f32 u, v, w, h` | Glyph table indexed by **Windows-1252** code. Holds the texture rectangle in normalised UV (top-left u,v, size w,h; all within [0,1]). In-memory glyph = `{u, v, w, h, 0, 0, advance = w, lineHeight, page}`. lineHeight defaults to glyph[0].h. | `0x1003f260.c:51-75` | H (fields) / M (u,v,w,h naming) |
| 0x03020000 | 4096 | 256 × `i32 u, v, w, h` / 1024 | Same in 22.10 fixed point (`×0.0009765625`). Used only by `metro/ui/minimap/font/normal.font`. | `0x1003f260.c:116-140` | H |

Extra code points are aliased to cp1252 slots (`0x1003f260.c:76-110`): 0x85->U+2026, 0x8C->U+0152, 0x92->U+2019, 0x93->U+201C, 0x94->U+201D, 0x96->U+2013, 0x9C->U+0153. `fonts/verdana/normal.font` = 8 + 8 + 4096 = 4112 bytes: 256 glyphs × 16 B. The task statement's 4104 is the root body size.

## 3. Machine-readable layout

```json
{
  "endianness": "little",
  "chunk": { "tag": "u32", "size": "u32", "body": "u8[size]" },
  "string": "char[N], NUL-terminated within N except where noted; bytes after the NUL may be garbage; engine uses unbounded strlen, so always clamp to N",
  "conventions": {
    "vectors": "row vectors, v' = v * M; matrices row-major (same memory layout as OpenGL column-major)",
    "quaternion": ["x", "y", "z", "w"],
    "quaternionToColumnVectorMath": "conjugate: (-x,-y,-z,w)",
    "boneLocal": "S(s) * R(q) * T(t)  (row-vector order)",
    "boneWorld": "local * parentWorld, file order, parent index < child index",
    "inverseBind": "inverse(bindWorld) computed from .skeleton; not stored",
    "skinMatrix": "inverseBind * currentWorld",
    "axes": { "up": "+Y", "forward": "-Z", "right": "+X", "handedness": "right", "units": "metres" },
    "timeUnit": "seconds",
    "keyTime": "t_i = i * duration / (keyCount - 1); keyCount == 1 -> constant",
    "interpolation": { "translation": "linear", "scale": "linear", "rotation": "normalize(q0 + t'*(q1-q0)), t' = (dot(q0,q1) < 0 ? -t : t)" },
    "endOfClip": "index >= n: clamp to n-1 if DONT_LOOP(0x20000) else wrap to key 0",
    "nameMatching": "exact, case-sensitive; first bone with the name wins; unmatched tracks ignored"
  },
  "formats": {
    "skeleton": {
      "root": "0x02200000",
      "chunks": {
        "0x02200000": { "headerSize": 36, "fields": [
          { "off": 0, "name": "name", "type": "char[32]" },
          { "off": 32, "name": "boneCount", "type": "u32" } ],
          "children": { "0x02201000": { "as": "bones", "count": "boneCount" } } },
        "0x02201000": { "headerSize": 76, "fields": [
          { "off": 0, "name": "name", "type": "char[32]" },
          { "off": 32, "name": "parent", "type": "i32", "note": "-1 = root" },
          { "off": 36, "name": "translation", "type": "f32[3]" },
          { "off": 48, "name": "scale", "type": "f32[3]" },
          { "off": 60, "name": "rotation", "type": "f32[4]", "note": "x,y,z,w" } ],
          "children": { "0x02201010": { "as": "attachedNodes", "max": 1 } } },
        "0x02201010": { "headerSize": 32, "fields": [
          { "off": 0, "name": "node", "type": "char[32]", "note": "name of child element attached rigidly; equals bone name" } ] }
      }
    },
    "animation": {
      "root": "0x02300000",
      "chunks": {
        "0x02300000": { "headerSize": 40, "fields": [
          { "off": 0, "name": "name", "type": "char[32]", "note": "may lack NUL when 32 chars long" },
          { "off": 32, "name": "trackCount", "type": "u32", "note": ">= number of track chunks" },
          { "off": 36, "name": "duration", "type": "f32", "unit": "s" } ],
          "children": { "0x02301000": { "as": "tracks" } } },
        "0x02301000": { "headerSize": 36, "fields": [
          { "off": 0, "name": "bone", "type": "char[32]" },
          { "off": 32, "name": "keyCount", "type": "u32" } ],
          "children": { "0x02301010": { "as": "keys", "count": "keyCount" } } },
        "0x02301010": { "headerSize": 40, "fields": [
          { "off": 0, "name": "translation", "type": "f32[3]" },
          { "off": 12, "name": "scale", "type": "f32[3]" },
          { "off": 24, "name": "rotation", "type": "f32[4]", "note": "x,y,z,w" } ] }
      }
    },
    "scene": {
      "root": "0x02100000",
      "chunks": {
        "0x02100000": { "headerSize": 0, "children": {
          "0x02100001": { "as": "sourcePath" }, "0x02080000": { "as": "nodes" },
          "0x02600000": { "as": "locators" }, "0x02700000": { "as": "cameras" } } },
        "0x02100001": { "headerSize": 0, "fields": [ { "off": 0, "name": "path", "type": "char[size]" } ], "engineReads": false },
        "0x02080000": { "headerSize": 128, "fields": [
          { "off": 0, "name": "unk0", "type": "u32", "note": "always 0, unread" },
          { "off": 4, "name": "name", "type": "char[32]" },
          { "off": 36, "name": "file", "type": "char[64]", "note": "relative to scene dir" },
          { "off": 100, "name": "position", "type": "f32[3]" },
          { "off": 112, "name": "rotation", "type": "f32[4]", "note": "x,y,z,w; may be non-unit (scale folded in)" } ] },
        "0x02600000": { "headerSize": 64, "fields": [
          { "off": 0, "name": "unk0", "type": "u32", "note": "always 0" },
          { "off": 4, "name": "name", "type": "char[32]", "note": "may lack NUL" },
          { "off": 36, "name": "position", "type": "f32[3]" },
          { "off": 48, "name": "rotation", "type": "f32[4]", "note": "may be non-unit" } ] },
        "0x02700000": { "headerSize": 88, "confidence": "low", "fields": [
          { "off": 4, "name": "name", "type": "char[40]" },
          { "off": 44, "name": "position", "type": "f32[3]" },
          { "off": 56, "name": "unk38", "type": "f32" },
          { "off": 60, "name": "target", "type": "f32[3]" },
          { "off": 72, "name": "unk48", "type": "u32" },
          { "off": 76, "name": "fovDegrees", "type": "f32" } ] }
      }
    },
    "properties": {
      "root": "0x02400000",
      "chunks": {
        "0x02400000": { "headerSize": 76, "fields": [
          { "off": 0, "name": "name", "type": "char[32]" },
          { "off": 32, "name": "className", "type": "char[32]", "note": "object | moveableObject | ..." },
          { "off": 64, "name": "sortAlpha", "type": "u8" },
          { "off": 65, "name": "autoDrawDistance", "type": "u8" },
          { "off": 66, "name": "pad", "type": "u8[2]", "note": "garbage" },
          { "off": 68, "name": "minDrawDistance", "type": "f32" },
          { "off": 72, "name": "maxDrawDistance", "type": "f32" } ],
          "children": { "0x02403000": { "as": "subObjects" }, "0x02404000": { "as": "sources" },
                        "0x02405000": { "as": "exportSettings" }, "0x02401000": { "as": "collision" }, "0x02402000": { "as": "physics" } } },
        "0x02403000": { "headerSize": 0, "children": { "0x02403100": { "as": "items" } } },
        "0x02403100": { "headerSize": 36, "fields": [
          { "off": 0, "name": "unk0", "type": "u32", "note": "always 0, unread" },
          { "off": 4, "name": "name", "type": "char[32]", "note": "loads <dir>/<name>.object" } ],
          "children": { "0x02403101": { "as": "transform", "max": 1 } } },
        "0x02403101": { "headerSize": 28, "fields": [
          { "off": 0, "name": "position", "type": "f32[3]" },
          { "off": 12, "name": "rotation", "type": "f32[4]" } ] },
        "0x02404000": { "headerSize": 0, "engineReads": false, "children": { "0x02404100": { "as": "items" } } },
        "0x02404100": { "headerSize": 0, "engineReads": false, "children": { "0x02404101": { "as": "path" } } },
        "0x02404101": { "headerSize": 0, "engineReads": false, "fields": [ { "off": 0, "name": "path", "type": "char[size]" } ] },
        "0x02405000": { "headerSize": 16, "engineReads": false, "fields": [
          { "off": 0, "name": "unitScale", "type": "f32" }, { "off": 4, "name": "unk1", "type": "f32" },
          { "off": 8, "name": "unk2", "type": "u32" }, { "off": 12, "name": "unk3", "type": "u32" } ] },
        "0x02401000": { "headerSize": 108, "engineReads": false, "confidence": "low", "fields": [
          { "off": 0, "name": "flags", "type": "u8[4]" }, { "off": 4, "name": "unk", "type": "f32[6]" },
          { "off": 28, "name": "unk2", "type": "f32[4]" }, { "off": 44, "name": "collisionFile", "type": "char[64]" } ] },
        "0x02402000": { "headerSize": 40, "engineReads": false, "confidence": "low", "fields": [
          { "off": 0, "name": "values", "type": "f32[10]" } ] }
      }
    },
    "font": {
      "root": "0x03000000",
      "altMagic": { "u32": "0x03464D42", "format": "AngelCode BMFont binary v3" },
      "texture": "<same stem>.dds",
      "chunks": {
        "0x03000000": { "headerSize": 0, "children": { "0x03010000": { "as": "glyphs" }, "0x03020000": { "as": "glyphsFixed" } } },
        "0x03010000": { "headerSize": 4096, "fields": [ { "off": 0, "name": "glyphs", "type": "f32[256][4]", "note": "u,v,w,h normalised; index = cp1252 code" } ] },
        "0x03020000": { "headerSize": 4096, "fields": [ { "off": 0, "name": "glyphs", "type": "i32[256][4]", "scale": 0.0009765625 } ] }
      },
      "cp1252Aliases": { "0x85": "U+2026", "0x8C": "U+0152", "0x92": "U+2019", "0x93": "U+201C", "0x94": "U+201D", "0x96": "U+2013", "0x9C": "U+0153" }
    }
  },
  "gltfExport": {
    "nodes": "one node per bone, file order; children from parent index",
    "translation": "bone.translation",
    "rotation": "[-qx, -qy, -qz, qw]",
    "scale": "bone.scale (may be negative/non-uniform)",
    "inverseBindMatrices": "inverse of engine bindWorld; the engine's row-major float[16] can be written verbatim as glTF column-major",
    "animationSampler": "input = i*duration/(n-1); LINEAR; rotation outputs conjugated like nodes",
    "trackTarget": "first node whose name == track.bone; drop unmatched tracks",
    "optionalYaw180": "glTF assets face +Z; CiM assets face -Z"
  }
}
```

## 4. Validation

Script: `validate.mjs` (with `cimanim.mjs`), read-only and in memory, over all 24 top-level `*.gs` archives of the install. Reports: `report-shipped.json`; `report-all.json` adds the `addons\*.gs` mods.

### 4.1 Totals (shipped, 24 archives)

| type | files | content | structural errors (chunk not consumed exactly, unknown tag, count < chunks, bad size) |
|---|---|---|---|
| .skeleton | 1482 | 32 222 bones (1…2560 per file; road/track kits are the big ones), 3585 attached-node chunks | **0** |
| .animation | 250 | 4301 tracks, 1 848 802 keys | **0** |
| .scene | 1467 | 3076 nodes, 1069 locators, 0 cameras, 1467 source paths | **0** |
| .properties | 1482 | 3098 sub-objects (all with transform), 227 source paths, 1482 export-settings, 2 collision + 2 physics chunks | **0** |
| .font | 5 | 4 float tables, 1 fixed-point table; all `.dds` present; all UVs in [0,1] | **0** |

With the addon mods (36 archives; 4 mod archives cannot be opened by `gstool.mjs`):
- totals are 1511 / 253 / 1494 / 1508 / 5 files;
- there are still **0 structural errors**;
- the addons add 8 more missing scene `.object` references and one more sub-object/scene name mismatch (`magnet` vs `marketstand01`).

### 4.2 Checks

| check | result |
|---|---|
| Every chunk consumed exactly; header sizes 36/76/32/40/36/40/128/64/76/36/28/16/4096 | pass, all files |
| `boneCount` = bone chunks | 1482/1482 |
| Parent index valid (−1 or 0…n−1, ≠ self), acyclic, parent < child | 32 222/32 222 (0 invalid, 0 cycles, 0 forward references) |
| Bone quaternions unit | max ‖q‖ error 1.3e-7 |
| Key quaternions unit | max error 2.1e-7 over 1.85 M keys |
| Key times monotonic | implicit by construction (uniform spacing). Duration > 0 in 250/250 (min 0.001 s). Adjacent keys in opposite hemispheres: 4. |
| `keyCount` = key chunks | 4301/4301 |
| `trackCount` = track chunks | 243/250 equal; **7 header > actual** (e3_bcm_walk01 16 vs 15, e3_pem_walk01 17 vs 16, metro_04(_b)_MRA 6 vs 4; including the copies in the tokyo/usa archives). Engine-tolerated. |
| Tracks vs skeleton, script-paired (`loadSkeleton` + `loadAnimation` in the same `with` block, 108 pairs) | **108/108 all tracks bind; 108/108 all bones animated** |
| Tracks vs best skeleton in the same folder (all 250) | 235 fully bound. 15 have unbound tracks: props/helpers such as `Event 04`, `UNf_B_prop`, `Camera01`; `bcf_anim.animation` belongs to an unshipped rig. None of these 15 is paired by a script with that skeleton. |
| Key 0 = bind pose | 3774/4229 bound tracks |
| Scene node `.object` exists | 2953/3076. The 123 missing are exporter leftovers, e.g. `cab_scene.scene -> hackney.object`; harmless because scenes are not instantiated at runtime. |
| Scene node pos = bind world translation of the same-named bone | 3067/3067 |
| Scene / properties / skeleton triple with the same stem | 1467/1467 scenes |
| Properties sub-object names = scene node names | 1466/1467 (one differs only in case) |
| Properties sub-object transform = scene node transform | 3075/3075 |
| 0x02201010 name = bone name | 3585/3585; 3458 resolve to a same-stem scene node |
| Quaternion in scene nodes / locators unit | **26 nodes, 174 locators non-unit** (scale folded in by exporter) |
| NUL-terminated strings | 5 take names and 10 locator names fill their 32 bytes without a NUL; 53 property strings contain garbage after the NUL |
| `loadSkeleton` targets exist | 225/226. The one miss is `addons/stpetersburg/scripts/game.script`, which runs with a vehicle's base directory, not the script's. |
| glTF conversion: `T·R(conj q)·S`, `world = parent·local` reproduces the engine bind world matrices (transposed) | 32 222/32 222 bones, max relative error 0 (`gltfcheck-all.mjs`) |
| Semantic check, `bus_b_anim.animation` door, sampled with the engine sampler | Door leaf swings 90° about world +Y at the hinge (1.03, ·, −1.02). It opens over 0–1.0 s, holds to 1.167 s, and is closed at 2.167 s. This matches `bus_b.script` exactly: `doorOpenStart=0`, `doorOpenLen=30/30`, `doorCloseStart=35/30`, `doorCloseLen=30/30`. It confirms seconds, uniform key spacing, the quaternion convention, and that +X is the door (right) side (`doorcheck.mjs`). |
| Semantic check, blue-collar-male bind pose and walk | Upright 1.80 m figure, Y-up. The walk is in place (root x/z = 0) with knees bending toward −Z (`explore1.mjs`, `explore10.mjs`). |

## 5. Open questions

1. **Skin binding inside `.object`.** Skinned sub-meshes (`gsHWSkinSubMesh_OGL`) take a `gsISkeleton*` (`0x10089a10`) and almost certainly index bones in `.skeleton` file order. The per-vertex index/weight layout and any palette remapping must be confirmed in the `.object` decoding.
2. **Rigid attachment space.** In the update loop (`0x100690e0`, near the end), attached elements receive the bone's world transform:
   - position = `bone.t · parentWorld` (`FUN_100860f0`), set via vslot 0xf8;
   - orientation = `bone.q ⊗ quat(parentWorld)` (Hamilton product `FUN_100840e0`, matrix->quat `FUN_10085650`), set via vslot 0xfc;
   - the bone's own scale is not passed on.

   That means an attached mesh must be authored in bone-local space. The `.scene` root nodes, however, store the rotation **without** the root Z-up correction (§2.8). Whether rigidly attached meshes would come out rotated is therefore untested. Shipped content does not exercise the path, because scenes are never instantiated and the root object is excluded.
3. **Scene node rotation for root bones.**
   - It is `(+√½,0,0,√½) ⊗ q_bone` in 2077 cases. In 2017 of them the root carries exactly the Z-up fix, so the node rotation is identity; 52 have other roots; 8 have identity roots, which gives +90° about X.
   - It is `q_bone` in 830 cases (824 identity roots, 6 others).
   - Child nodes always store `q_world` (134).
   - The exporter's rule for the 8 + 6 exceptions is unknown. The field is unused at runtime.
4. **`0x02405000`** fields 2–4 (`unk1` ∈ {0,10,20}, `unk2` ∈ {0,1}) and the exact meaning of `unitScale` (exporter-side only). `0x02401000`/`0x02402000` (collision/physics) are not read by GSystem; they may be read by the exe or an editor (not checked).
5. **Font metrics.** Resolved in [docs/ui/fonts.md](../ui/fonts.md): advance = glyph width, line height = glyph[0].h, all glyph geometry divided by the line height, no kerning; the BMFont v3 reader (`0x1003f610`) and the measuring functions are decoded there, with `tools/fontparse.mjs`.
6. **0x02700000 camera** fields other than name/position/FOV are guesses (no shipped instance).
7. **Script frame rates.** Scripts crop clips with `/25` (e.g. `cropAnimation("walk", 10/25, 30/25)`) while many clips are sampled at 30 keys/s. Time is in seconds either way; which frame rate the artists intended is unknown.
