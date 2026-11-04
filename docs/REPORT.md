# Cities in Motion — Reverse-Engineering Report

Target: Steam install at `C:\Program Files (x86)\Steam\steamapps\common\Cities In Motion` (build `1.0.22`,
engine `GSystem.dll` PE timestamp 2011-08-10). Analysis is static: shipped binaries, archives and the
user-data folder were read; the game was never run. Native code was decompiled with Ghidra 12.1.4:

* **v1** via REA `decompile-all` (7 742 functions + 258 imports, Evidence E9) in `decompiled/GSystem/functions/`.
  Default analysis missed 2 516 functions and truncated 27 % of bodies after calls to `operator delete`.
* **v2 (use this one)** via a direct Ghidra headless pass (`tools/ghidra/`), because REA's `decompile-all` cannot set
  analysis options or add entry points. **12 026 functions**, every body complete, 12 025 decompiled.
  * 2 516 recovered from data references (vtable methods and other callbacks).
  * 1 715 recovered from instruction operands, mostly `push offset handler` in native-API registration code.
    Of those, 1 109 are MSVC exception-frame stubs, 140 are exception funclets and 466 are ordinary functions.
  * Output is `decompiled/GSystem-v2/functions/` with `index.csv`. Addresses match v1 where both have a function.

Findings marked **decompiled** come from that pseudocode (function addresses given).

Each finding is labelled **observed** (read directly from bytes or REA Evidence), **corroborated** (an
inference confirmed by a second independent source), or **inferred**. REA Evidence IDs are listed in
[§11](#11-rea-evidence-index); the full bundle is `docs/rea-evidence-bundle.json`.

---

## 1. Architecture at a glance

```
Cities In Motion.exe (49 KB loader, MSVC)
  └─ LoadLibrary("GSystem.dll") → gsInitialize("GSystem.dll -i metro/scripts/init.script [-p …]")
                                 → gsMainLoop() → gsDestroy()
GSystem.dll (1.7 MB, C++, x86)  — Colossal Order's in-house "GSystem" engine
  ├─ OpenGL 1.x + ARB/GLSL extensions (renderer)     ├─ FMOD Ex (audio, 3D sound)
  ├─ zlib 1.2.3 via zlibwapi.dll (compress/uncompress)├─ Steamworks (achievements, user, stats)
  ├─ Win32 window/input, clipboard, file system       └─ embedded script VM (parser → compiler → linker → bytecode)
*.gs archives (24 files, 20 900 entries, ~2.2 GB unpacked)
  └─ scripts (.script/.scenario/.campaign/.strings) hold the game rules and UI;
     binary chunk formats hold maps, meshes, animation, state machines
Config.exe (Borland Delphi/VCL launcher-settings tool)
```

| Fact | Status | Source |
|---|---|---|
| The exe is a pure loader: imports only KERNEL32 (`LoadLibraryA`/`GetProcAddress`); its strings are `GSystem.dll -i metro/scripts/init.script`, `gsInitialize`, `gsMainLoop`, `gsDestroy`; PDB path `g:\GSystem\Source\Shortcuts\Cities In Motion\…` | observed | E1, strings |
| GSystem exports 86 symbols: lifecycle (`gsInitialize/gsMainLoop/gsDestroy`) and a C embedding API for the script VM (`compileScript`, `newScript`, `newThread`, `runThreads`, `execFunction`, `execMethod`, `registerFunction`, `registerConstant`, `get/setVariable`, `get/setProperty`, variant constructors/accessors, `objectEvent`, `setErrorProc`, `setOutputProc`) | observed | E3 |
| GSystem imports OpenGL32, FMOD Ex (32 imports, C++ API), zlibwapi ordinals 46/2/26 = `compressBound`/`compress`/`uncompress` (zlib 1.2.3 by its version strings), steam_api, WINMM timers, SHELL32 `SHGetSpecialFolderPathW` | observed | E2, E4 |
| Resources are tagged language 1035 (Finnish); PDB `G:\GSystem\Source\GSystem\Release\GSystem.pdb` | observed | E5, strings |
| /GS cookie + SafeSEH table (1 113 handlers); no CFG; no TLS callbacks | observed | E6 |
| Config.exe is native, not .NET; import pattern (no ILTs, `GetKeyboardType`, oleaut32 Variant API, comctl32 ImageList) is the Delphi VCL signature | observed / inferred (Delphi) | E7, E8 |

### 1.1 The Linux build (symbols)

The game also ships official Linux and macOS builds (Steam app 73010).
* **Depots:** Linux base 206722–206725, Linux DLC 271380–271389, macOS 73015 plus DLC.
* **Binary:** the Linux depot 206722 holds `Cities In Motion.bin`, a 32-bit x86 ELF built by GCC 4.6.3 for the same
  game version 1.0.22. The engine is statically linked into it and runs on SDL2 (window, input), FMOD Ex and
  `steam_api`.
* **Not stripped:** its symbol table has 10 660 symbols, including 5 222 functions with full C++ names and
  parameter types, file-local statics, and the engine's 90 source file names. Examples: `gsCompiler.cpp`,
  `gsGridCitizen.cpp`, `gsVideoOGL.cpp`, the platform layer `gsLinux.cpp` and `gsCommonPlatform.cpp`, and
  leftovers of an older engine (`gsCar`, `gsHuman`, `gsWeapons`, `gsPhysics`).
* **Decompile:** `decompiled/Linux/` (Ghidra project `CiMLinux`, `tools/ghidra/run-ghidra.ps1 -Binary … -ProjectName
  CiMLinux -OutDir …`) has 5 786 of 5 788 functions, named as in the source; for example
  `gsStateMachine::enterState(gsState*)`.

It is the preferred reference for reading engine code. The Windows addresses in this report still identify the
same functions in `GSystem.dll`.

**Names on Windows.** `tools/match-builds.mjs` paired 1 996 `GSystem.dll` functions with their Linux twins:
58 % of the Linux engine functions, and no wrong name among 21 functions identified by hand. The names are
applied in the Windows Ghidra project and in `decompiled/GSystem-v2/functions/`. Method, validation and gaps:
[linux-names.md](linux-names.md).

## 2. `GS10` archive format (`*.gs`)

All integers little-endian `u32`. Every structure is a **chunk**: `tag, size, body[size]`.

```
"GS10"      size                       size = file length − 8
0x01000000  size                       root; entry offsets are relative to its body (file offset 0x10)
  0x01100000 size                      directory
    u32 count
    [0x01130000 128]                   parent prefix (NUL-padded)
    [0x01140000 128]                   absolute mount (NUL-padded)
    count × 0x0111xxxx|0x0112xxxx 144  entry: magic[4] offset size packedSize name[128]
  0x01200000 size                      data, 4-byte aligned
0x00000001  size                       trailer comment (shipped archives only)
```

Reader: `FUN_1007f4d0` (open + magic) → `FUN_1007ef80` (directory) → `FUN_1007e200` (entry read). **Decompiled**.

* Header check: `magic == 'GS10'` and `size == fileLength − 8`.
* **Entry tag selects compression:** `0x01110000` = stored (`fread`), `0x01120000` = zlib
  (`uncompress`; on failure *"File '…' in '…' is corrupted!"*). All 20 900 shipped entries use `0x0112`;
  `packedSize == size` does not imply stored (`metro.gs:shaders/path/route.fragment`, 54 → 54 B zlib).
* **Mount rule:**
  * `0x01140000` present → virtual path = `prefix + name` (`data.gs` "" → `effects/…`; DLC `addons/usa/…`; `ja.gs` `metro/`).
  * otherwise → `[0x01130000 prefix] + <archive path minus ".gs"> + "/" + name`
    (`metro.gs` → `metro/…`; `sounds.gs` + `metro/` → `metro/sounds/…`; `objects.gs` → `metro/objects/…`;
    `userdata/saves/berlin.gs` → `userdata/saves/berlin/…`).
  * Corroborated by script references (`/metro/sounds/3. Shopping3 (shop).wav`, `/metro/objects/vehicles/bus-07/…`)
    and by `metro/config.script`, which copies `defaults/berlin` to `saves/berlin.gs` then tests `saves/berlin/map.grid`.
* **Duplicates:** paths go into a global map; a path already present is not replaced (first archive wins).
  Each entry stores `{archive path, absolute offset, size, packedSize, tag}` (0xF8-byte record).
* `magic` = first 4 unpacked bytes (observed for 20 900 / 20 900 entries).
* Trailer of every shipped archive: `"If you are reading this, you have no life :)"`. Saves have none.
* **`validatePackage(path)`** (script function, `FUN_1007c4e0` → `FUN_1007c3a0`) checks the header and
  **deletes the file with `DeleteFileW` if it fails**. Only the in-game shop calls it, on downloads
  (`metro/ui/menus/shop.script:223`).

Validation (`tools/gsverify.mjs`): 24 archives, 20 900 entries, 0 decode failures, 0 size mismatches.

## 3. Virtual file system and user data

* All archives are merged into one virtual tree: `/metro/…` (base game, with `metro/objects`, `metro/maps`,
  `metro/sounds`, `metro/music`, `metro/location-data` from their own archives), `/addons/<pack>/…` (DLC),
  root (`effects/`, `shaders/`, `fonts/`, `scripts/` from `data.gs`). Loose folders in the install
  (`addons/`, `metro/`) appear in the same tree; that is how mods ship. **Observed** (paths).
* **Priority (decompiled):** a loose file always replaces an archive entry at the same path, whatever the scan
  order (`FUN_1007f5e0`). Between archives, the first one mounted wins.
  * The map is case-sensitive.
  * `memory/` holds RAM files that survive rescans.
  * Absolute and `/`-prefixed paths bypass the map and go straight to disk.
  * Writes never create directories.
  * Details are in [`handlers/batch-C.md`](handlers/batch-C.md).
* The archive scanner (`FUN_1007f5e0`) also mounts `.gs` files under `userdata/`. **Decompiled**.
* `metro/config.script` calls `init_userdata(getMyDocumentsFolder()+"/Cities In Motion")`; user files
  appear as `userdata/…`. Archives dropped there are mounted after `updateFileSystem()`. **Observed**.
* Your `Documents\Cities In Motion` holds `settings.script` (plain script), `controls.keys`, `state.gs`
  (GS10 → `state/score.script`), `saves\Cim2.gs`, and `log_metro.txt` (shows addons such as
  `metro/addons/moneyget_1.1.1` loading).

## 4. The universal chunk format

GSystem serializes nearly all binary data as nested `tag/size/body` chunks; the tag's **top byte is the
format family**, lower bytes are a type path. A body may be a fixed header followed by sub-chunks.

| Family | Root tag | Files | Status |
|---|---|---|---|
| `0x01` | `0x01000000` | `.gs` archives, savegames, `state.gs` | fully decoded |
| `0x02` | `0x02000000` / `0x02100000` / `0x02200000` / `0x02300000` / `0x02400000` | `.object` mesh / `.scene` / `.skeleton` / `.animation` / `.properties` (and `.material`) | fully decoded (§4.3) |
| `0x03` | `0x03000000` | `.font` | identified |
| `0x05` | `0x05000000` | `.states` state machines | fully decoded |
| `0x07` | `0x07000000` / `0x07080000` | `.grid` map + simulation state / `.timeline` | tree mapped |

Text formats: `.script`, `.scenario`, `.campaign`, `.strings`, `.shader` (script), `.vertex`/`.fragment`
(GLSL), `.fnt` (AngelCode BMFont binary), `.keys`.

`.material` is also family `0x02`: root `0x02013100` (the same tag as a mesh's embedded material) with one
sub-chunk per texture slot. Observed slots are `0x02013110` diffuse, `0x02013120` normal, `0x02013130` specular
and `0x02013150` ambient occlusion; each records the artist's source path in `0x02013111`/`0x02013112`.
My first survey mislabelled it as unchunked because its first tag's low bits aren't zero.

### 4.1 `.grid` — map and live simulation state

Observed on the shipped `metro/defaults/berlin` and on your own `Cim2.gs` (identical layout):

```
0x07000000  header{u32 width=1280, u32 height=1280, f32 cell=3.0, u32 bucketShift=4, u16 0x0200, u16 0, u32 0}
  0x07010000  terrain: width×height × 4 B  (= 6 553 600 B; 1280 × 3.0 → 3.84 km square)
  0x07020000  network  header{u32 typeCount, u32 itemCount}
     0x07020100  type-name table, NUL-terminated ("alley_footpath_basic", "road_small_diag_crossing", …)
     0x07020210  items, 16 B each           (116 509 × 16 = 1 864 144 B, exact)
     0x07020310  street-name table ("Charlottenburger Chaussee", …)
  0x07040000  AI traffic (cars, trains, aircraft, boats)  {modelCount=23, vehicleCount=1673}
  0x07050000  characters: visible pedestrians and animals  {17 types, 5527}
  0x07060000  player transport lines  {5 models, 8 lines, 76 vehicles, 67 stops}
  0x07070000  citizens: the simulated population  {30 255 in Berlin}, 64 B each
```

**The format is now fully decoded and validated.** The complete specification is in
[`save-format/`](save-format/README.md), and `tools/gridparse.mjs` parses every section. It reports 0 issues on
150 files: your save, the shipped saves, and every `.grid`/`.timeline` in all 24 archives. Key points:

* Every section is written through three primitives: `FUN_1007d530(tag)` opens a chunk, `FUN_1007d770(bytes)`
  writes data, and `FUN_1007c090` closes it by back-patching the size.
* Later sections refer to earlier ones by 1-based index in file order. Line stops point at network items; citizens
  point at characters, their car, stops, line vehicles, and their home/workplace items.
* Terrain cells are `i16 height` (1/256 cell) plus flags: pavement, field, water, locked ground and ground style.
* Two formats changed during development, and the loader still reads the old forms: network items
  (`0x07020200` → `0x07020210`) and street names (`0x07020300` Latin-1 → `0x07020310` UTF-8). Ten campaign
  scenario maps still use the old street-name table.
* **Correction:** `0x07020211`, `0x07040301`, `0x07050001` and `0x07050301` are only split values of compiled
  `switch` comparisons, not chunk versions as an earlier version of this report said.
* Header: `bucketShift` (4) maps positions into the grid's spatial-hash buckets; `0x0200` is a height base the
  loader subtracts.
* Network item, 16 B:

| Offset | Type | Meaning |
|---|---|---|
| +0 | u16 | type index into `0x07020100` (bits 0-13) \| orientation (bits 14-15; bit 14 swaps width/length) |
| +2, +4, +6 | i16 | x, y (height, plus a loader offset), z |
| +8 | u32 | 1-based index of the item it is attached to (0 = none); the loader builds parents first |
| +12 | u32 | low byte = variation/sub-value (stored in different fields for item kinds 0x16/0x17); upper bits = flags (`0x400000`, `0x500000`, `0x700000` change how the item is built) |

* Serializers (writer / loader): vehicles `FUN_1010f8a0` / `FUN_1010d330`; characters `FUN_100cb5c0` /
  `FUN_100cace0`; lines `FUN_1011a9f0` / `FUN_1011b110`; citizens `FUN_100ceb40` / `FUN_100d31a0`;
  timeline `FUN_10101650` / `FUN_101036f0`.

### 4.2 `.timeline`
The editor-recorded growth of the city, replayed from the 1920 base map with a 182-day preview window. Day groups
are keyed by days since 1 Jan 1920. They hold create, delete, brush, water, name and variation actions, which refer
to objects by name and position rather than by index. Details: [`save-format/`](save-format/README.md) §4.

### 4.3 3D assets (fully decoded)

Full specification in [`asset-format/`](asset-format/README.md); `tools/assetparse.mjs` parses everything and
exports meshes to glTF 2.0. Every shipped `.object`, `.material`, `.skeleton`, `.animation`, `.scene`,
`.properties` and `.font` parses with 0 structural errors, and two independent parsers agree on all counts.

* **Meshes:** 36-byte vertices (position, normal, RGBA, UV), optional 4-influence skinning and a second UV set,
  `u32` triangle indices, a full-detail mesh plus an optional lower-detail one, and one material range per submesh.
  The coordinate system is right-handed, +Y up and in metres, and texture V is stored negated.
* **Materials:** five texture slots (diffuse, normal, specular, illumination, ambient occlusion) plus 3ds Max-style
  shading parameters.
* **Skeletons and animations:** bones `{name, parent, translation, scale, rotation}`. Keys have no time field
  (evenly spaced over the take duration). Rotation uses a normalised lerp, and glTF rotation is the conjugate of the
  stored quaternion; this reproduces the engine's bind matrices for all 32 222 bones.
* **`.properties`:** the class to instantiate, draw distances and sub-objects. `.scene` is an exporter by-product
  the engine never loads.
* **Checks:** shaded renders of decoded meshes come out clean, and the bus-door animation matches its script's
  timing exactly.

### 4.4 `.states` — state machines (fully decoded)

```
0x05000000
  0x05010000 → 0x05010100 state (44 B):  name[32] flags:u32 param:u32 layout{i8 x,i8 y,u8 w,u8 h}
  0x05020000 → 0x05020100 transition (48 B + sub-chunks): name[32] flags:u32 branch:u32 from:u16 to:u16 layout[4]
                 0x05020101 guard:  script expression fragment (e.g. "$loadPosition < #$toLoad")
                 0x05020102 action: script block fragment (e.g. "unloadSubMenu();")
```

* **State flags are bits** (decompiled, 2026-11-02):
  * 1: choice. On entry, its empty-named transitions are tried and the first true one is taken.
  * 2: timed. `param` is an f32 number of seconds; "timeout" is queued when it expires.
  * 4: initial. The first state in name order with this bit is entered at load and on `reset()`.
  * 8: final. Entering it ends the machine (`onEnd`).

  Intro is 6 (timed + initial). The two unnamed 8s in `main.states` are exits.
* **Transitions:**
  * Flags bit 1 means "return": go back to the state that entered the previous one (`back`, `cancel`, system
    `activate`). Each state keeps a return link that is set on entry and unwound on re-entry.
  * Lookup is a multimap keyed by (from-state, action, **branch**): candidates are tried in branch order, so a choice
    state's guard-less "else" branch is tried last.
  * Guards and actions are compiled into the machine's own code, together with the companion script
    `<name>.script` (`main.states` + `main.script`); the machine object **is** that script.
  * `main.states` holds 125 such fragments (mostly actions).
* **Runtime** (`0x1009e900` tick, `0x1009ce70` process, `0x1009bd10` transition, `0x1009d060` enter,
  `0x1009d780` initial):
  * `perform()` only queues the action; the tick advances the timeout, then drains about half the queue per tick (a
    loop quirk).
  * A transition fires `exit` (filtered by the old state's name), runs the action, then enters the target.
  * Entering a state fires `enter` (filtered by its name).

  Implemented in `port/src/engine/natives_objects.cpp`.
* Verified by meaning: `newgame: MainMenu→NewGameMenu`, `completed: GameLoading→Game`,
  `error: GameLoading→LoadError`, `deactivate: Default→Paused`.
* `main.states`: 75 states, 168 transitions (the whole menu/game/editor/shop flow).
  Rendered in `docs/main-states.mmd`. Scripts bind handlers by name:
  `event this.enter::["Paused"]() { … }`.

## 5. The scripting language

A dynamically typed, JavaScript-like language compiled at load time to bytecode by GSystem.

**Reserved words (40, from the engine's keyword table):** `array assert break continue current defined
delete do echo else event exit export false for foreach foreach_reverse function global if keys load
local loop macro map null pause resume return skip_frame this thread true values wait wait_until
wait_while while with`.

| Construct | Example from shipped scripts |
|---|---|
| Variables are `$`-prefixed members; `MAIN` is the global root | `MAIN.$settings = $settings;` |
| Default operator `:` (value if defined) | `$settings.$fpsLimit : 0.0` |
| Script-as-object: `load` executes a file and returns its object | `MAIN.$strings = load "/metro/strings/"+lang+".strings";` |
| `with` block sets members with leading `.` | `with ($s = new_sound(MAIN.$desktop)) { .load("…wav"); .volume = 1.0; }` |
| Events | `event this.onDestroy::() { delete $querySound; }` |
| Deferred expressions | `macro :: scrollVehicles(-1)` |
| Cooperative coroutines and thread groups | `thread`, `wait`, `wait_until`, `skip_frame`, `pause "game"` / `resume "game"` |
| Maps and arrays | `map ["version", [6, 1], "name", "metro-mainline"]` |
| Iteration | `foreach (list) { … current … }` |

* Pipeline from engine messages: *Parser error* → *Compiler error* → *Linker error* → *Runtime error*.
  Limits: string literal ≤ 255 chars, ≤ 255 parameters. The VM stack has no limit: *"Stack size exceeded!"* has a
  message but no throw site (`script-runtime/semantics.md` §3).
* **Runtime semantics (decompiled):**
  * [`script-runtime/semantics.md`](script-runtime/semantics.md): names and scoping, calls, events, threads and
    groups, `load`, macros, errors.
  * [`script-runtime/values.md`](script-runtime/values.md): conversions, operators, containers, export.
  * `script-runtime/operators.json`: the full 995-cell operator table.

  The port implements them in `port/src/script/`.
* **Bytecode VM (decompiled, fully mapped — see [`vm-opcodes.md`](vm-opcodes.md)):** a stack machine with
  60 opcodes (0x00-0x3B) plus `0xFF` END in one interpreter loop (`FUN_1002ae90`). It runs cooperative threads
  (states: running / finished / sleeping until a time / blocked on watched variables). `wait_until` really
  registers watchers on the variables in its condition instead of polling. Every opcode is matched to the
  compiler code that emits it (`FUN_100067c0`, emitter `FUN_100166d0`). The reference also covers the
  43 operators with precedences from the lexer, and the variant categories used for operator dispatch.
* Language facts found along the way: `skip_frame` evaluates to the frame's delta time (`$angle += skip_frame * 0.5`);
  `@x` makes a live reference and `#x` gives the element count; `?` and `:` are separate binary operators,
  so `x : y` doubles as "y if x is unset"; `thread "group" :: { … }` and `load "group" :: path` place
  threads in named groups that `pause`/`resume` control.
* Scripts ship as source and are compiled at load time, so no bytecode exists on disk. A reimplementation needs
  the language semantics and the native API, not bytecode compatibility.
* Native functions are registered in tables such as `FUN_10081e80` (file system: `updateFileSystem`,
  `init_userdata`, `list_dirs`, `list_files`, `extractFileName/Dir/Path/Ext`, `changeFileExt`,
  `writePackage`, `validatePackage`) via `FUN_1002e170(fn, argCount, …)`.
* A variant exporter writes values back as script source (`null /* cannot export object */`); this is
  how save state and settings are persisted (`exportVariables`, `$settings.exportVariables([...])`,
  `gamedata.script` inside every save). **Observed** strings and usage.
* **Native API (extracted from code — see [`native-api.md`](native-api.md), data in `native-api.json`):**
  * Size: **131 global functions, 336 constants and 72 script classes with 833 members** (440 fields,
    297 methods, 96 events), with handler addresses, argument ranges, field types/offsets and member ids.
  * Field side effects are recorded too: 78 read-only fields (their setter `FUN_1001bfc0` throws), 91 fields
    with a custom setter and 17 with a pre-assign hook (`setter`/`hook`/`readOnly` in `native-api.json`).
  * Globals live in one name→variant map (`FUN_1000f5f0`).
  * Each class declares its members in a virtual `registerMembers()` (vtable slot 30), which first chains to
    its parent class's registrar.
  * The `new_*` constructors map to RTTI classes (`new_sound` → `gsSound`, `new_grid` → `gsGrid`, …).
  * Cross-check: every native global function, constant and member used by the 3 601 shipped scripts is in
    the extracted set.
* **Calling conventions (decompiled):**
  * Global handlers are `gsVariant* handler(int argc, gsVariant** args)`; the wrapper checks `argc` against the
    registered range (*"Wrong number of arguments"*).
  * Methods are `handler(object, argc, args)`, with a *"Trying to use destroyed object!"* guard.
  * Arguments go through the `gsVariant` virtual interface:
    * lifetime: `+0x04`/`+0x08` add-ref/release, `+0x10` dereference, `+0x18` category;
    * conversions: `+0x1C` bool, `+0x20` int, `+0x24` float, `+0x28` string, `+0x2C` object;
    * containers: `+0x34` export, `+0x38` count, `+0x44` assign, `+0x48` call, `+0x54`/`+0x58` set/get member,
      `+0x60` index, `+0x64`/`+0x68` keys/values, `+0x70` defined.
  * Object arguments are checked with RTTI `dynamic_cast`, so expected classes are visible in the code.
  * The exported C embedding functions `newObject` and `registerFunction` are a bare `ret 8`. The linker folded
    identical empty functions together, so in this build they do nothing.
* **Simulation (traced, see [`simulation/`](simulation/README.md)):**
  * The update tree and time model are mapped:
    * main loop → script threads → scene (≤ 0.1 s sub-steps) → grid (`n = 1 + trunc(5·dt·timeSpeed)` sub-steps);
    * the grid then ticks vehicles, characters, citizens and lines in that order;
    * the game clock runs separately at 10 000 × game speed.
  * Citizens: housing-driven population, a distance-falloff choice of home, job and destination, a mode choice
    between transit, car, walk and a direct move to the target, and patience and mood while waiting.
  * Pathfinding: three Dijkstra-style searches (lane graph, pedestrian cells, transit line-stop nodes) with
    randomised, goal-biased costs.
  * Public transport: dwell times from headway, boarding and alighting run from the citizen side, and breakdown rolls.
  * Traffic and city: node-claim traffic without signals, simulation over the whole map (5 Hz away from the camera),
    traffic heat per building, cosmetic fires, and city growth through the timeline.
* **Renderer (mapped, see [`renderer/`](renderer/README.md)):**
  * 18 `gsI*` interfaces, each with one OpenGL implementation; every slot is named, with its GL calls and evidence.
  * One scene end to end ([scene-basics.md](renderer/scene-basics.md), the main menu's trams): quaternion and camera
    conventions, the sun's direction from the clock, light and material values, fog, the shader template language
    with the exact generated default shader, and render-to-texture.
  * The shader combiner is confirmed by reimplementation: the port runs the shipped `.shader` scripts and
    templates through its own VM, reproduces the expanded default shader byte for byte, and compiles 3 749
    variants after translating GLSL 1.10 to 3.30 (`port/README.md`, `regs-tool shaders`).
  * The engine uses 116 GL functions (58 loaded as extensions) in an OpenGL 1.5/2.0 + ARB GLSL style.
  * The frame order is reconstructed. Rendering uses four queues: opaque, glow, alpha-tested, blended.
  * Skinning is GPU-only, and the shaders read fixed-function state.
  * There is a port checklist of behaviours and engine bugs to reproduce.
* **GUI (specified, see [`ui/`](ui/)):** how controls become pixels ([controls.md](ui/controls.md): field layout,
  image/border/text items, alignment, the `%` formatter, fades), how input reaches scripts
  ([input.md](ui/input.md): key codes and names, input targets, hit test, hover, capture, the active chain) and the
  font formats with the exact text metrics ([fonts.md](ui/fonts.md)). The port implements all three.
* **Handler semantics:** argument types and behaviour of the most-used handlers are in
  [`handlers/`](handlers/) (one file per batch: simulation/grid, scene/audio/UI, globals/save system).
* **RTTI is present** (`tools/rtti.mjs`, `rtti-classes.json`): 257 C++ type names and 243 classes with vtables
  and full base chains.
  * The value system: `gsVariant` and its subclasses, the `gs*FieldVariant` field bindings,
    `gsOpaqueFunction`/`gsTransparentFunction` native functions.
  * Script objects: `gsModule` > `gsScript` > `gsObjectVariant`.
  * Simulation: `gsGrid`, `gsGridObject`, and `gsGridCitizenController`/`gsGridVehicleController`/
    `gsTransportStopController`.
  * **The renderer sits behind abstract interfaces** (`gsIScene`, `gsIMesh`, `gsITexture`, `gsIShader`,
    `gsICamera`, `gsIViewport`, …) implemented by `*_OGL` classes. That is the seam for a new graphics backend.
* Earlier usage estimate (`tools/apisurface.mjs`, `docs/script-api-surface.json`): over 3 601 shipped
  scripts, 101 native global functions, 576 native members and 207 engine constants are used; scripts
  define 2 207 functions of their own. Native classes are created by `new_*` constructors (50 names in
  the engine's strings:
  `new_grid`, `new_gridPlayer`, `new_gridTimeline`, `new_stateMachine`, `new_scene`, `new_sound`,
  `new_particleEmitter`, `new_viewport`, `new_control`, …). Most-used native members: `position`,
  `lines`, `routeExists`, `getRandomItem`, `setPathFlags`, `getGridObject`, `loadGridObject`,
  `addHomePlaces`/`addWorkPlaces`/`addVisitPlaces`, `storeMesh`. Constant families: `GRID_CITIZEN_*`,
  `GRID_VISIT_*`, `GRID_ITEM_*`, `GRID_PATH_*`, `GRIDMODE_*`, `GRIDROAD_*`, `GRIDMETRO_*`, `GRIDSTOP_*`,
  `TRANSPORTLINE_*`, `MATERIAL_*`, `SHADER_*`. The match is name-based, so membership is **inferred**.

## 6. Boot sequence (observed in `metro/scripts/init.script`)

1. `metro/config.script`: set user-data root, seed `settings.script`, `controls.keys`, `state.gs`.
2. Open log `userdata/log_metro.txt`; load `/userdata/settings.script` (reset from defaults if broken).
3. `metro/build.script` → `$version = "1.0.22"`, `$demo`, `$beta`.
4. Audio (`enable_sounds`, `set_audio_volume`), then `window.script`, `viewport.script`.
5. Debug console/FPS overlays if `$settings.$debug` / `$fps`.
6. Language: `metro/strings/<lang>.strings` (fallback `en_US`).
7. `loadStateMachine("/metro/scripts/system.states")` and `…/main.states` → Intro → logos → MainMenu.

## 7. Gameplay logic (scripts)

The rules live in script; the engine provides simulation primitives. Paths below are virtual paths under
`extracted/`. All money is integer **cents** (`scenario01.scenario:5` sets `$money = 20000*100`). Note that
much logic sits in UI scripts, e.g. fares are collected in `metro/ui/linepanel/linepanel.script`.
Key lines were spot-checked against the files; the rest comes from a script-reading pass and keeps its citations.

**Flow and time.**
* Main menu → campaign/scenario/sandbox (`.campaign` files) → `GameLoading`, which loads the engine,
  environment, economy, player, ui, game and level scripts (`metro/scripts/main.script:737-759`).
* `$mainClock = clock($time,$date)` runs at `.speed = 10000.0` and defaults to 1 Jan 1920
  (`metro/scripts/engine.script:21-22`). Game speeds `[0, 1, 4, 10]` scale the clock, `grid.timeSpeed`
  and the `"game"` thread group (`metro/ui/minimap/minimap.script:159-178`).
* New games pre-simulate 125 × `$scene.tick(1.0)` with events off (`presimulate.script:6-19`).
* Eras set a `modelMask` bit per period (≤1947, 1943-72, 1968-97, ≥1993), which picks which vehicle
  and character models appear (`game.script:381-458`).
* Clock events: `onYear` unlocks stop types (1926, 1931, 1946, 1957, 1965, 1981); `onDay` applies the
  city timeline 182 days ahead (`game.script:612-696`).

**Fares (observed).**
* A passenger dodges the fare when
  `random(0,1) < (1.2 − h[−7]·0.1) · cheat[class]`, with `cheat = [0.10, 0.05, 0.01, 0.20, 0.10, 0.05, 0.25]`
  (`linepanel.script:316,357`).
* A dodger is caught if `random(0,10) < employeeHappiness[1]` and pays 15× the fare.
* The fare is credited at once (`earnMoney`, `:370`). Defaults are `[380, 420, 520, 650, 1250]` cents
  per transport type (`gamedata.script:34`).

**Monthly costs** (`player.script:254-301`):
* energy: fuel and electricity consumption × price
* vehicle maintenance: `int(level·price·0.0005)`
* wages: `−n·w0 − ⌈n/5⌉·w1 − ⌈n/3⌉·w2 − ⌈n/4⌉·w3`, metro vehicles counted twice (`:272`)
* stop maintenance
* loans: annuity `pow(1+i,n)·i/(pow(1+i,n)−1)·amount` with `i = interest/1200` (`metro/ui/hqpanel/hqpanel.script:59-62`)
* breakdowns are charged when they happen

Vehicle condition decays toward `1/(1+dist/(200000·(0.1+0.9·reliability)·(0.1+0.009·maint)))` (`player.script:312-313`).

**Macro economy** (`economy.script`):
* A random-walk economy index drives fuel price `int(120·(1+h·rand(.03,.08)))`, electricity
  `int(70·(1+h·rand(.02,.04)))`, general prices, and wages with a 4-month lag.
* The same index sets per-class fare sensitivity `[0.11, 0.09, 0.03, 0.15, 0.07, 0.13, 0.20] · (1.7 − 0.14·h)`
  and trip-purpose weights, both passed to native `citizens.setPriceSignificance` / `setReasonProbability`.

**Ratings.**
* Company value = money + base + Σ vehicle price × condition + ads − loans.
* Board rating = `clamp(int(Δvalue/(100000+value^0.6))+2, 0, 4)`; customer rating = `clamp(int(5·happiness), 0, 4)`
  (`metro/ui/annualreport/annualreport.script:263-266`).
* Customer happiness on boarding: `h·0.98 + 0.02/(1+wait·0.01)·employeeHappiness[0]` (`linepanel.script:406`).

**Native vs. script.** Path finding, mode choice (walk/drive/transit), boarding and mood are native; they are traced
in [`simulation/`](simulation/README.md). Scripts see them through events (`onCitizenArrive`, `onGetPassenger`, `onCitizenMood`, `onBroken`) and counters
(`getCanTravelCount`, `getPriceTooHighCount`). Building capacity comes from per-object scripts
(`addHomePlaces`, `addWorkPlaces`, `addVisitPlaces`).

**Scenarios.**
* Three layers (scenario, location, `default.scenario`) are called by function name (`scenario.script:280-295`).
* Tasks start on dates through coroutines and turn into petitions.
* Every 1.13 s, `checkTaskCondition` evaluates boolean `condition_<task>()` functions such as
  `routeExists(TRANSPORTLINE_MASK_ALL, pos)` or `$money >= balance && arrived >= total`.
* Rewards: cash × bank-level multiplier, and reputation.
* `scenarioFailed` / `objectiveFailed` exist but nothing in the shipped scripts calls them. In practice
  there is no lose condition; negative money only triggers subsidies (`default.scenario:86-95`).

**Saving** (`main.script:306-336`):
* Each part is written as script source with `exportVariablesTo` into an in-memory `memory/` folder, then
  bundled with `writePackage("userdata/saves/<name>.gs", files)`.
* `gamedata.script` holds about 90 variables (`player.script:97-110`).
* Running scenario coroutines are saved as `[function, param]` pairs and restarted on load (`scenario.script:310-372`).
* Live simulation (terrain, network, vehicles, citizens, lines, objects) goes to the binary `map.grid` (§4.1).

**Addons** (`addons.script:19-73`):
* Disabled in demo/beta builds.
* Otherwise every folder under `addons/`, `metro/addons/` and `userdata/addons/` is an addon. For each one the game:
  * merges `strings/<lang>.strings`
  * collects `maps/*.grid`, `campaigns/*.campaign`, `scenarios/*.scenario`
  * registers `scripts/{environment,game,editor,newsticker}.script`
  * runs `scripts/init.script`
* There is no manifest. Neither the scripts nor the engine strings mention `.modinfo`, so the
  `metro-mainline.modinfo` in your install is metadata the game never reads.

## 8. Tools delivered (`tools/`, Node ≥ 18, no dependencies)

| Tool | Purpose |
|---|---|
| `gstool.mjs info\|list\|extract <archive.gs> [outDir] [--filter re]` | read/extract GS10 archives, saves, `state.gs` |
| `gsverify.mjs <gameDir> [--json out]` | inflate-and-check every entry; per-extension inventory |
| `chunkscan.mjs survey\|tree\|treefile` | identify chunk families; dump chunk trees of any binary asset |
| `states2mermaid.mjs <file.states> [--json]` | decode state machines to JSON / Mermaid |
| `apisurface.mjs <extracted> <GSystem.dll> [out.json]` | estimate the native script API from usage |
| `pestrings.mjs <file> [min]` | ASCII + UTF-16 strings with offsets |
| `gridparse.mjs summary\|dump <file> …` | decode `.grid` saves/maps and `.timeline` files completely, with validation |
| `assetparse.mjs check\|export\|info …` | decode meshes, materials, skeletons and animations; export meshes to glTF 2.0 with a shaded PNG preview |
| `meshpreview.mjs` | dependency-free PNG rasterizer and `.glb` writer used by `assetparse.mjs` |
| `fontparse.mjs info\|measure\|check …` | decode native `.font` and BMFont `.fnt` fonts; reproduce the engine's `getTextWidth`/`getTextHeight` and line breaking ([ui/fonts.md](ui/fonts.md)) |
| `renderer-map.mjs <rtti.json> <functionsDir> [out]` | map renderer interface slots to their OpenGL implementations and GL/extension calls |
| `callgraph.mjs <functionsDir> reach\|callers\|own …` | direct-call graph over the pseudocode: reachable callees, callers, root ownership partition |
| `gen-port-api.mjs <native-api.json> <GSystem.dll> <out.cpp>` | generate the port's registration of the whole native API (class tree from RTTI, all members, constants read from the DLL) |
| `gen-port-operators.mjs <operators.json> <out.inc>` | generate the port's operator dispatch table |
| `match-builds.mjs <win features> <linux features> <dll> <linux bin> <native-api.json> <rtti.json> <out.csv>` | carry the Linux build's symbol names over to GSystem.dll functions ([linux-names.md](linux-names.md)) |
| `peoff2va.mjs <file> [0xoffset …]` | PE section table; file offset → virtual address |
| `vtables.mjs <dll> <functionsDir> <ctor…> \| --vt <vtable…>` | resolve a class's vtable and decode trivial methods (type/category getters) |
| `codeptrs.mjs <dll> <decompile-index.json \| functionsDir> [out]` | list code addresses referenced from data that the decompile does not cover |
| `rtti.mjs <dll> [out.json]` | recover MSVC RTTI: class names, vtables, virtual-method addresses, base classes |
| `nativeapi.mjs <functionsDir> <rtti.json> <dll> [surface.json] [out.json]` | extract the native script API (globals, constants, classes, members) |
| `nativeapi-md.mjs <native-api.json> <rtti.json> <out.md>` | render the API reference Markdown |

`extracted/` holds every text-format file (scripts, scenarios, strings, shaders, materials, …) from all
24 archives, laid out by the engine's virtual paths (mount rules from §2). Re-extract everything with
`node tools/gstool.mjs extract <archive.gs> <outDir>` (≈2.2 GB for all archives).

`decompiled/GSystem-v2/functions/0x<address>.c` holds Ghidra pseudocode for every function. `index.csv` lists
each function's size, whether it was recovered, its no-return flag and decompile status. `recovery.csv` gives
the outcome per candidate and `noreturn.txt` lists the non-returning functions. No symbols or PDB ship with
the Windows game. Function names come from three sources: the Linux build (`gsClock::move`, see §1.1),
FID library names, or Ghidra's default `FUN_<address>`.

| Ghidra tool (`tools/ghidra/`) | Purpose |
|---|---|
| `run-ghidra.ps1 [-Threads n] [-Reuse]` | runs headless Ghidra on GSystem.dll through the Java launcher (paths with spaces/`(x86)` are safe); project saved in `C:\Users\Marku\Ghidra\projects\CiM` |
| `CimPreAnalysis.java` | pre-analysis: disables *Non-Returning Functions - Discovered* |
| `RecoverAndDecompile.java` | applies the known no-return list, recovers functions from the candidate list, decompiles everything in parallel |
| `DumpFeatures.java <out.jsonl>` | per-function matching features (calls, strings, constants, size) for `match-builds.mjs`; run with `-Script` |
| `ApplyNames.java <linux-names.csv>` | renames `FUN_…` functions after their Linux twins and comments every match; run with `-Script` |

### REA / Ghidra setup on this machine
Ghidra 12.1.4 is at `C:\Users\Marku\Ghidra\ghidra_12.1.4_PUBLIC` (SHA-256 of the zip verified against the
GitHub release). The REA server registration in `~/.claude.json` now sets `GHIDRA_INSTALL_DIR` and
`JAVA_HOME=C:\Program Files\Java\jdk-23`; the pre-change copy is
`~/.claude.json.bak-rea-ghidra-2026-11-01T22-17-19-669Z`. The REA MCP server picks this up after a reconnect or app
restart. Until then the CLI works:
`set GHIDRA_INSTALL_DIR=… & set JAVA_HOME=… & npx -y rea-agents@6.8.0 decompile <path> --procedure 0x…`.

## 9. Unknowns and limits

* **Native code is decompiled but mostly unread.** So far the analysis covers the archive reader, mount
  rules, `validatePackage`, the save-chunk writer, the network loader, the VM dispatcher and the
  file-system registrations, the VM opcodes, the renderer contract and the simulation (`simulation/`). The
  simulation docs leave open: the scene scale factor in the camera activity radius, the aircraft and boat spawners,
  a few node flags, and how crashed cars are finally removed. Nothing in the simulation was checked against the
  running game.
* **Decompile coverage: resolved in v2.**
  * Data-reference gap: v1 left 2 553 code addresses referenced from data outside every function
    (`tools/codeptrs.mjs`). The v2 pass defined 2 516 new functions there; the other 37 were existing entries or
    now sit inside complete bodies. The new functions break down as 56 exception-unwind stubs, 179 tiny methods
    (≤ 8 B, e.g. type getters), 424 explicit `__thiscall`/`__fastcall` methods and 1 857 other functions.
  * Operand-reference gap: 198 of the 407 registered native handlers were also missing, because code references
    them only as immediates. A second pass defines a function at every instruction operand that points at code
    outside any function, repeated until nothing new appears (one round sufficed). It added 1 715 functions;
    all 407 handlers now have pseudocode.
  * Truncation: v1's false "does not return" cuts (2 976 call sites, from `FUN_10087080` = `operator delete` and
    its thunk) are gone; every remaining warning is a genuine throw, `terminate`, `exit` or `abort`.
  * Ghidra's known no-return list is now applied with normalized names, so `__CxxThrowException@8`, `terminate`
    and `___crtExitProcess` are marked.
  * Leftovers, both MSVC runtime internals: one SEH block of the CRT exit routine (`0x10125da0`) is in no
    function, and the SSE helper `FUN_1011fa20` fails to decompile (*"Overlapping input varnodes"*).
* Save and map format: structure fully decoded. The line, line-stop and line-vehicle fields are now named from
  the simulation trace (headway, effective attractiveness, wait average, boarding reservations, bells); a few flag
  bits remain. Replaying a saved path needs the per-node link order, which is not in the file (lane links are kept in
  insertion order, `simulation/pathfinding.md`).
* 3D assets: structure fully decoded. Still unknown are a few unread material bytes and the fields of the camera,
  collision and unit-scale chunks, which no shipped file uses or the engine never reads.
* Handler semantics cover the 93 most-used handlers (`handlers/`). The other ~310 registered handlers are listed
  in `native-api.md` but not yet read.
* Nothing was observed at runtime.

## 10. Suggested next steps

1. Extend handler semantics beyond the 93 key handlers in `handlers/` (the remaining global functions, and the
   method handlers of the citizen and vehicle controllers), and go deeper into the medium-confidence entries.
2. Renderer interfaces are mapped in [`renderer/`](renderer/README.md). Next: prototype a backend against that
   contract, for example a translation layer that implements the `gsI*` interfaces on a modern API and runs the
   GLSL 1.10 shaders through a cross-compiler with the fixed-function state as uniforms.
3. Simulation is traced in [`simulation/`](simulation/README.md). Next: a headless re-simulation harness. Load a
   save with `gridparse.mjs`, run the documented tick for N steps, and compare aggregate statistics
   (walk/drive/travel counts, line passengers) with the same save advanced in the real game.
4. Build a save editor or map viewer on `gstool.mjs` + `gridparse.mjs`.

## 11. REA evidence index

| ID | Evidence |
|---|---|
| E1 | `ev_93420f5d667811344573be00cb40f4b3561a801a42e7976f377a9089bf864176` — loader imports |
| E2 | `ev_db7e47022bd65c9e866e952a7178998a35326b9d9f69c63742ef7bd5d5a365cb` — GSystem imports |
| E3 | `ev_ee783a082f9ddef5515bf85cd62f1ee924f92e234047279d1fe1fa768a8a458e` — GSystem exports (86) |
| E4 | `ev_53eab18a8743d809940a49cdebb9560790b46cbffda2873388924084e8c11056` — zlibwapi exports (ordinals) |
| E5 | `ev_fecae5cb7fcccc2c9d759e0b68f4c94d729f60626ece1f86e8773406df58acd0` — GSystem resources |
| E6 | `ev_f6797cdd7e7b4f2c1c45fae8600c1158723213f352fd79ffead73851375cb225` — GSystem load config |
| E7 | `ev_a6bb2a73eb21e8b0be43f4b039edff675a3ed4c21ebb6e991400fb0fc0672d3d` — Config.exe not managed |
| E8 | `ev_50e3a22e76c7d2184a295d671c08cd767e193826b2ee000fcddfb0947d5a7a6f` — Config.exe imports |
| E9 | `ev_00d79eb8b25cf75511d42dafa811d33c1535413149d12432abe18f0faa3df2f8` — Ghidra 12.1.4 `decompile-all` of GSystem.dll (manifest `decompiled/GSystem-manifest.json`) |

SHA-256: `GSystem.dll` 1daf5be3…02602 · `Cities In Motion.exe` ec50cf32…0e1b2 · `Config.exe` 96b92bce…c2c4b ·
`zlibwapi.dll` 21582b3a…84d00.
