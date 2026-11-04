# Batch B: scene, audio, material, particle, UI-control and state-machine handlers

Overview. Most of these handlers are thin wrappers. They convert arguments and then call a C++ virtual (for example gsControl slots 0x12c-0x170, gsSound slot 0x11c or gsObject slot 0x194), so the real behavior lives in the class implementation.
- **Paths.** Resource paths go through one resolver, `FUN_1007de40(out, path, base)`. A leading `/` or `~/` is stripped, which makes the path relative to the VFS root. Anything else is joined to the object's base directory (wstring at `this+0xb0`, set through vtable slot 0xcc). Some handlers skip the resolver and pass the raw path straight to the cache: material `setTexture`/`setShader`, particle `setTexture` and `setFont`.
- **Rendering.** Everything reaches the renderer through gsVideo_OGL factory slots: 0x10c getTexture (gsTextures_OGL cache, falls back to a default texture), 0x110 getShader (gsShaders_OGL cache, falls back to `shaders/default/default.shader`), 0x114 createMaterial (gsMaterial_OGL), 0x108 createParticleSystem and 0xec createGuiText. These all go into gsMaterial_OGL setters: +0x08 setTexture(slot), +0x0c setShader, +0x10 setFlags, +0x2c setShaderParam(i, vec4), +0x38 getFlags.
- **Audio** is FMOD Ex. Files are read fully from the VFS into memory and opened with `FMOD_OPENMEMORY|FMOD_SOFTWARE` plus 2D or (3D|LINEARROLLOFF). Sounds are cached by path only.
- **UI controls** keep native GUI objects: material +0x138, image primitive +0x13c, border +0x140, text +0x144, clip area +0x148. Every layout setter ends in a rebuild virtual.
- **State machines** queue action names and process them on their timer tick.

## gsSound.load
- **Signature:** `load(path: string, [positional3D: bool = true]) -> unset`
- **Behavior:** Thin wrapper that calls the object's virtual slot 0x11c `load(path, positional3D)`. For gsSound that is `0x1009b570`; for gsMusic (created by `new_music`) it is `0x1009b610`. The virtual resolves the path against the object's base directory, then gets or creates an FMOD `Sound*` from the global sound manager and stores it at `this+0x13c`. Nothing is played.
- **Details:**
  - Path resolution (observed, `FUN_1007de40`): an empty path stays empty. `"/x"` becomes `"x"` and `"~/x"` becomes `"x"` (both root-relative). Otherwise the result is `base + "/" + path`, with a leading `"./"` stripped; the base is `this+0xb0`.
  - Sound manager `DAT_10196e1c`: byte +0 is the "enabled" flag, +4 is the `FMOD::System*`, and +0x1c is a map from path to `Sound*`. The cache is checked before the mode is chosen, so the first load of a file fixes its 2D/3D and compressed mode for all later loads, including gsMusic vs gsSound (observed: lookup at 0x1009adc0:38-42 happens before the flags at :56-59).
  - FMOD mode (observed constants, decoded with FMOD Ex values):
    - base `0x840` = FMOD_SOFTWARE|FMOD_OPENMEMORY.
    - `positional3D=true` adds `0x200010` = FMOD_3D|FMOD_3D_LINEARROLLOFF; false adds `0x8` = FMOD_2D.
    - gsMusic also adds `0x200` = FMOD_CREATECOMPRESSEDSAMPLE. These are in-memory compressed samples, not streams.
  - The file is read through `FUN_100809e0` into a buffer, and `FMOD_CREATESOUNDEXINFO.length` is set to the file size. `FMOD::System::createSound` is then called, followed by `FMOD::Sound::setSoundGroup` (the group argument is lost in the decompilation).
  - Errors (observed in callee `FUN_10098f70`): failures are logged as "Error loading audio file: <path>" and "FMOD error (n) ...". No script exception is raised, and `this+0x13c` ends up null. If the sound system is disabled, the result is also null.
  - The gsSound variant resets `length` (`+0x16c`) to 0. It is recomputed on the next play. gsMusic does not reset it.
  - Scripts pass `false` for UI and ambience sounds, e.g. `.load("/metro/sounds/2. GUI menu button click1.wav", false)`. World sounds use the default (3D).
- **Evidence:** `0x10098830.c:26-36` (arg 2 defaults to 1 at :26-27; vcall +0x11c at :36). Callees: `0x1009b570.c:24-32`, `0x1009b610.c:24-27`, `0x1007de40.c:46-113`, `0x1009adc0.c:35-71`, and the string data `~/` at 0x1015dfb0 and `./` at 0x1015dfa8 (read from GSystem.dll). Vtable slot 0x11c is taken from rtti-classes.json.
- **Confidence:** high for path rules, caching and FMOD flags (observed). Medium on the sound-group purpose (inferred to be an sfx/music volume group).

## gsSound.play
- **Signature:** `play([loop: bool = false]) -> unset`
- **Behavior:**
  - Records the loop flag (`+0x145`) and stops any active channel.
  - If the module is enabled and audible, the sound system is on, and the listener distance is below `maxDistance`, it starts a new FMOD channel.
  - Calling `play(true)` on a sound that is already looping with a live channel does nothing.
- **Details:**
  - Gate (observed): module flags `this+0x6c` bit 1 (`enabled`) and bit 4 (`audible`) must both be set. The `visible` flag is not required.
  - Distance (`FUN_10097c10`, observed):
    - If `relative` is set (+0x146), it is the length of the local position.
    - Otherwise it is the world distance to the global listener `DAT_10196cc8`, or the length of the world position if there is no listener.
    - Out of range means no start, but the loop flag stays set. A later update presumably starts the sound (inferred).
  - Channel start (`FUN_100998c0(this,1)`, observed):
    - Calls `FMOD::System::playSound` (channel FREE; Ghidra garbled the argument list, apparently paused=1) into `+0x140`.
    - Restores the saved position `+0x180` with `Channel::setPosition`, then reads `Channel::getFrequency`.
    - `FUN_10099720` applies `setFrequency`, `setVolume` and `setMode` (from pitch, volume and loop).
    - Fills `length` = `Sound::getLength(ms) * 0.001` the first time.
  - Stop path: `Channel::stop`, which clears `+0x140` and the resume position `+0x180`. A plain `play()` therefore restarts from 0. After `pause()` (which saves the position into +0x180 and stops the channel), `play()` resumes from the saved position (observed in 0x1009a1a0).
  - Does nothing if `load` failed (`+0x13c` null).
- **Evidence:** `0x1009a0e0.c:18-43`. Callees: `0x100998c0.c:28-52`, `0x10097a90.c`, `0x10097c10.c`, `0x10099720.c`, and the pause handler `0x1009a1a0.c` for resume semantics. Module flag bits come from registrar `0x100a0750.c:16-18`.
- **Confidence:** high for control flow. Medium for the paused/unpause detail and the exact FMOD arguments, because Ghidra dropped arguments on the FMOD thiscalls.

## gsSkeleton.loadAnimation
- **Signature:** `loadAnimation(name: string, file: string, [flags: int], [group1: string, group2: string, ...]) -> unset`
- **Behavior:**
  - Resolves `file` against the skeleton's base directory (`+0xb0`). `gsObject.loadSkeleton` sets that to the directory of the .skeleton file.
  - Loads the file through a global refcounted animation cache (`FUN_1006f080(&DAT_10195118, path)`, keyed by path).
  - Installs it under `name` in the skeleton's animation map (`+0x174`). An animation already stored under that name is released and replaced.
- **Details:**
  - Initial slot state (observed in `FUN_100707b0`): start = 0, length = anim duration (`anim+0x24`), speed = 1.0, several weights = 1.0. `loop = !(flags & 0x20000)`; 0x20000 is the script constant `DONT_LOOP`. A loaded animation therefore plays immediately at speed 1 (inferred from speed initialised to 1.0 and `stopAnimation` setting speed 0).
  - Extra args 4+ are bone-group names looked up in the group map `+0x180` (filled by `addGroup`). The decompiler lost how the matches are accumulated (passed as `param_4` to `FUN_1006d4f0`, which binds the slot to groups). Unknown names appear to be skipped.
  - `flags` is read with `toInt` (line 41), but the value is only visible as an unaffected register. The default when omitted is unknown (probably 0).
  - Scripts always use 3 args, e.g. `.loadAnimation("walk", "unm_anim.animation", 0)`. The file is relative to the skeleton directory.
  - No renderer calls. This is pure animation-state CPU code (gsISkeleton/gsSkeleton_OGL is not touched here).
- **Evidence:** `0x10070b40.c:39-79` (group loop 44-71, name 73, path resolve 75-77, call 79). Callees: `0x100707b0.c:53-92` (DONT_LOOP at :73), `0x1006f080.c`, and the constants list in native-api.md.
- **Confidence:** medium-high. The group-selector encoding and the default `flags` are uncertain.

## gsSkeleton.cropAnimation
- **Signature:** `cropAnimation(name: string, start: float, length: float) -> unset`
- **Behavior:** Restricts the named animation to a sub-range. `start` is clamped to [0, duration]. `length` is clamped to [0, duration - start]. The current playback position (`slot+0x68`) is clamped into [0, length]. Unknown names are ignored.
- **Details:**
  - The third argument is a length, not an end time (observed). For example, `.cropAnimation("walk", 10.0/25.0, 28.0/25.0)` means frames 10 to 38 at 25 fps.
  - Units are seconds, matching the animation duration field `anim+0x24`.
- **Evidence:** `0x1006b420.c:25-31`; callee `0x1006a300.c:23-37`.
- **Confidence:** high.

## gsSkeleton.playAnimation
- **Signature:** `playAnimation(name: string, speed: float, [loop: bool = true], [onEnd: value]) -> unset`
- **Behavior:** For the named animation slot, sets the playback speed multiplier (`+0x44`) and the loop flag (`+0x84`). The loop flag defaults to true and overrides `DONT_LOOP` from loading. Any previous callback in `+0x5c` is released and replaced with arg 4, acquired through variant slot +0x14, or cleared.
- **Details:**
  - It does not rewind or reset the position (observed). The only effect is that speed becomes non-zero. `stopAnimation` (0x1006adb0) is the inverse: speed=0, loop=1, callback cleared.
  - The callback is presumably invoked when a non-looping animation finishes (inferred, not traced). No shipped script passes 4 args.
  - Unknown names are ignored silently.
  - Typical use: `.playAnimation("walk", 1.0 / this.speed, true)`.
- **Evidence:** `0x1006aac0.c:25-42`; callee `0x1006a200.c:18-29`; contrast with `0x1006adb0.c`.
- **Confidence:** high for the stored state. Low for when and how `onEnd` fires.

## gsObject.loadSkeleton
- **Signature:** `loadSkeleton(file: string) -> gsSkeleton | unset`
- **Behavior:**
  - Resolves `file` against the object's base directory (`+0xb0`, via `FUN_1008a6f0`) and calls virtual slot 0x194 (`0x1008a560`).
  - That virtual reads the file. If the file is non-empty, it destroys any old skeleton and creates a new one via slot 0x128 (`FUN_1005b740`, which allocates a gsSkeleton of 0x1a0 bytes). It sets the new skeleton's base directory to the skeleton file's directory (slot 0xcc, `FUN_1007ce30` dirname) and parses the data (`FUN_1006fbc0`).
  - Returns the `skeleton` field (`+0x188`) with a new reference, or unset if there is none.
- **Details:**
  - A missing or empty file changes nothing and returns the existing skeleton or unset (observed: `local_30 != 0` guard). No error is raised.
  - After parsing, it calls vfunc +0x30 on each entry of the object's list at `+0x220`. These are mesh parts and dependents; rebinding them to the skeleton is inferred. It also calls the skeleton's secondary-base vfunc at `+0x134` (not read).
  - Returning the skeleton enables `with (this.loadSkeleton("bus_b_scene.skeleton")) { .loadAnimation(...) }`.
  - The renderer is reached indirectly through mesh rebinding (gsISubMesh/gsHWSkinSubMesh_OGL are likely). This was not verified at this depth.
- **Evidence:** `0x1008b7d0.c:30-53`. Callees: `0x1008a6f0.c` (copies `+0xb0`), `0x1008a560.c:32-60`, `0x1005b740.c`, `0x10034510.c` (slot 0xcc sets `+0xb0`), and `0x1007ce30.c`.
- **Confidence:** high for flow and return value. Medium for what the `+0x220` list and the `+0x134` call do.

## gsObject.getMaterial
- **Signature:** `getMaterial(name: string) -> gsMaterialVariant | null`
- **Behavior:** Looks up `name` (narrow string, exact match) in the object's material map at `this+0x210`, keyed by the material names from the mesh file (e.g. `"glow"` or `"17 - Default"`). It returns the gsMaterialVariant with a new reference, or the null singleton if the name is not found.
- **Details:**
  - Scripts use it as a test: `if ($mat = this.getMaterial("glow")) { $mat.flags |= MATERIAL_ADDITIVE_A; }`.
  - The map is filled during mesh loading, presumably before `onLoaded` (inferred).
  - Not determined: whether the underlying gsMaterial_OGL is shared between instances of the same mesh.
- **Evidence:** `0x1008a250.c:23-35` (map find via `FUN_10089ea0`, end sentinel `+0x214`, value at node+0x28).
- **Confidence:** high for the lookup. Low on sharing semantics.

## gsMaterialVariant.setTexture
- **Signature:** `setTexture(slot: int, path: string) -> unset`
- **Behavior:** Loads a texture through the video object (`this+0x14`, gsVideo_OGL slot 0x10c → gsTextures_OGL cache `FUN_100431c0`) and assigns it to texture `slot` of the native material (`this+0x18`, gsMaterial_OGL +0x08). It then re-reads the material flags into the script-visible `flags` field (`+0x3c`). Slot 0 also sets `MATERIAL_DIFFUSEMAP` (0x4000) on both the field and the native material (+0x10).
- **Details:**
  - The path is not resolved against a base directory. It is VFS-root-relative as given, e.g. `.setTexture(0, "metro/graphics/contrain.dds")`.
  - Texture cache (observed): map from path to gsTexture_OGL, refcount at `tex+0x14`, loaded using the video's `anisotropy` (`video+0x1e8`). An empty path or a load failure returns the shared default texture (`+0xd8`), so the call never fails.
  - gsMaterial_OGL stores slots at `+0x80 + slot*4`. Neither the handler nor `FUN_1004d990` bounds-checks `slot`. Only 7 slots fit before the shader parameters at `+0x9c`, so a slot of 7 or more would overwrite them (inferred from the layout).
  - Releases its temporary texture reference through texture vfunc 0.
- **Evidence:** `0x100893a0.c:28-45`. Callees: gsVideo_OGL slot 0x10c `0x1004e4f0.c` → `0x100431c0.c:31-73`, gsMaterial_OGL `0x1004d990.c`, `0x1004dbb0.c`, `0x1004d9e0.c`; gsTexture_OGL vtable seen in `0x100412f0.c:12`.
- **Confidence:** high.

## gsMaterialVariant.setShader
- **Signature:** `setShader(path: string) -> unset`
- **Behavior:** Gets a shader program from gsVideo_OGL slot 0x110, `getShader(path, 0, 0)` (gsShaders_OGL cache at `video+0x1a8`, `FUN_1003cad0`). It assigns the program to the native material (gsMaterial_OGL +0x0c, refcounted) and refreshes the `flags` field from the material.
- **Details:**
  - An empty path makes the video substitute `shaders/default/default.shader` (observed in `0x1004fcd0`).
  - The path is used raw, without base-directory resolution. Scripts use `"metro/shaders/water/contrain.shader"`, though `"/metro/ui/menus/logo.shader"` also appears.
  - Whether a raw leading `/` is accepted by the shader cache is not verified.
  - The two extra `getShader` arguments (0, 0) are probably define and variant flags (inferred).
- **Evidence:** `0x10089470.c:26-38`. Callees: `0x1004fcd0.c` and gsMaterial_OGL `0x1004d9c0.c`.
- **Confidence:** high for flow. Medium on the meaning of the extra getShader arguments.

## gsMaterialVariant.setShaderParams
- **Signature:** `setShaderParams(index: int, value: quat) -> unset`
- **Behavior:**
  - Converts arg 2 to a 4-float vector with `FUN_10093350`. This only accepts the quaternion custom type; any other type yields a global default quaternion, probably identity, which is set at runtime from BSS.
  - If the vector differs from the cached value at `this+0x44+index*16`, it stores the value, pushes it to the native material (gsMaterial_OGL +0x2c, stored at `mat+0x9c+index*16`) and refreshes `flags`.
- **Details:**
  - It is a no-op when the value is unchanged (observed: `FUN_10084210` returns 0 when equal).
  - There are 8 vec4 params in the variant (constructor loop at 0x10087bf0). `index` is not bounds-checked.
  - These are the shader uniforms read by the effect shaders, e.g. `.setShaderParams(1, quat(0.0, 0.2, -0.9, 1.0))`.
- **Evidence:** `0x100870d0.c:19-30`. Callees: `0x10093350.c`, `0x10084210.c`, gsMaterial_OGL `0x1004db60.c`, and the constructor `0x10087bf0.c:32-38`.
- **Confidence:** high.

## gsParticleEmitter.setTexture
- **Signature:** `setTexture(path: string) -> unset`
- **Behavior:**
  - Loads the texture via the emitter's video object (`this+0x40`, slot 0x10c, raw path) and sets it as slot 0 of the emitter's material (gsMaterialVariant at `this+0x1fc`, native material at variant `+0x18`).
  - Then calls `FUN_1005d770`. That helper creates the material with `shaders/particles/particles.shader` if it is missing, sets the material flags from `particleBlend` and hands the material to the native particle system.
- **Details:**
  - Flags (observed): `0x4200` (MATERIAL_DIFFUSEMAP|MATERIAL_POINT). `particleBlend == 2` (BLEND_ADDITIVE) adds `0x80` MATERIAL_ADDITIVE. `particleBlend == 4` (BLEND_ADDITIVE_ALPHA) adds `0x100` MATERIAL_ADDITIVE_A.
  - The native particle system at `+0x13c` (gsIParticleSystem → gsParticleSystem_OGL, created by video slot 0x108 in `FUN_1005dc80`) receives the material via its vfunc +0x0c.
  - The handler dereferences `+0x1fc` before calling the helper that would create it. Calling it on an emitter that is not yet attached to a video would dereference null (inferred risk). Shipped scripts call it inside `with (new_particleEmitter(parent))`, after attachment.
  - Typical use: `.setTexture("effects/particles/lightsmoke.dds")` (189 uses).
- **Evidence:** `0x1005dbb0.c:26-37`. Callees: `0x1005d770.c:30-70` and `0x1005dc80.c`.
- **Confidence:** high.

## gsParticleEmitter.addColorRange
- **Signature:** `addColorRange(color0: int ARGB, color1: int ARGB) -> unset`
- **Behavior:** Appends the pair `(color0, color1)` as two raw 32-bit ARGB values to a `std::vector<pair<uint,uint>>` at `this+0x1c8`. Calls accumulate; nothing clears the list.
- **Details:**
  - Probable meaning: start and end colour over particle life (inferred from usage such as `.addColorRange(0x80333333, 0x00333333)`, which fades alpha 0x80→0).
  - With several ranges, one is probably picked per particle (inferred). The consumer was not read.
- **Evidence:** `0x1005e1e0.c:18-24`; callees `0x10074ce0.c`, `0x10075330.c`, `0x1005dff0.c`.
- **Confidence:** high for storage. Medium for the semantics.

## gsParticleEmitter.setParticleScale
- **Signature:** `setParticleScale(growthRate: float, growStart: float, growEnd: float) -> unset`
- **Behavior:** Stores three floats at `+0x160/+0x164/+0x168`. These are copied into each particle at spawn and drive particle size over age: `size = particleSize * (1 + growthRate * (min(age, growEnd) - growStart))` when `age > growStart`, otherwise `size = particleSize`.
- **Details:**
  - Defaults are `(0, 0, 1000)`, set by the constructor at 0x1005dcf0:31-33.
  - Example: `.setParticleScale(1.0, 0.1, 6.0)` grows smoke linearly from 0.1 s to 6 s, reaching about 6.9× its size.
- **Evidence:** `0x1005b9f0.c:13-18`. Consumers: `0x1005c3f0.c:171-173` (copied into the particle at +0x44..+0x4c) and `0x1005c020.c:84-96` (size formula).
- **Confidence:** high (formula observed).

## gsVideo_OGL.new_texture
- **Signature:** `new_texture([path: string]) -> gsTextureVariant_OGL`
- **Behavior:**
  - With a path: resolves it against the video object's base directory (`+0xb0`) and loads it through the texture cache (`this+0x1b0`, gsTextures_OGL; the default texture on failure).
  - Wraps the result in a new gsTextureVariant_OGL (0x28 bytes), which holds its own reference, and returns the variant subobject (`obj+4`).
  - Without a path: the variant creates a fresh, uncached, empty gsTexture_OGL.
- **Details:**
  - `FUN_10041340` registers members, stores the owning video at `+0x18` and creates the blank texture if `+0x1c` is null (observed).
  - The result is an `gsITextureContainer`, the type `gsControl.setImage` accepts as a non-string image.
  - Typical uses: `new_texture("/metro/objects/stops/bus-stop03_small.dds")` and `new_texture("/"+current)`.
- **Evidence:** `0x1004f470.c:33-72`. Callees: `0x1007de40.c`, `0x100431c0.c`, `0x10041180.c` (vtables 0x1015a4bc/0x1015a43c = gsTextureVariant_OGL) and `0x10041340.c`.
- **Confidence:** high.

## gsControl.setRect
- **Signature:** `setRect(left: float, top: float, width: float, height: float) -> unset`
- **Behavior:** Calls virtual slot 0x12c (`0x100a6510`, not overridden by any gsControl subclass). If any value changed, it stores `left/top/width/height` (`+0xd8/+0xdc/+0xe0/+0xe4`), runs the layout virtual 0xe4 and fires event id 16 (`onChangeRect`) via `FUN_1001f960`.
- **Details:**
  - Layout (`0x100a95a0`, overridden by gsEditCtrl, gsGraphCtrl, gsPinCtrl, gsTextSelCtrl and gsViewPort):
    - `desktopLeft/Top` = parent margins + parent desktop position + own left/top.
    - A "clipped" byte `+0x18c` is set when the control lies fully outside its parent.
    - `clientWidth/Height` = size minus margins.
    - Rebuilds the image primitive (0xe8), clip area (0xf4), text (0xec) and border (0xf0), then recurses into the children.
  - Setting identical values has no effect and fires no event.
- **Evidence:** `0x100a6e90.c:16-20`. Callees: `0x100a6510.c:11-18` and `0x100a95a0.c`. Event id from native-api.md.
- **Confidence:** high.

## gsControl.setCropRect
- **Signature:** `setCropRect(u: float, v: float, uWidth: float, vHeight: float) -> unset`
- **Behavior:** Calls slot 0x148 (`0x100a6390`). It stores `cropLeft/cropTop/cropWidth/cropHeight` (`+0x1f0..+0x1fc`) and, if the control already has an image primitive (`+0x13c`), rebuilds it (slot 0xe8).
- **Details:**
  - Values are normalized texture coordinates. The constructor default is `(0,0,1,1)`, and scripts use values like `9.0/64.0`. This is inferred from the defaults and usage.
  - It selects a sub-rectangle of the bound texture for the image quad (gsGuiPrimitive_OGL).
  - Ghidra types the parameters as int, but they are float fields.
- **Evidence:** `0x100a5f20.c:17-25`; callee `0x100a6390.c:11-16`; constructor defaults `0x100a8d90.c` (`+0x1f8/+0x1fc = 1.0f`).
- **Confidence:** high.

## gsControl.setImage
- **Signature:** `setImage(image: string | gsITextureContainer | null, [slot: int = 0]) -> unset`
- **Behavior:**
  - String: slot 0x140 (`0x100a90a0`) resolves the path against the control's base directory, loads the texture via video slot 0x10c, binds it to texture `slot` of the control's material (`+0x138`) and releases the temporary reference. The material is created lazily via video slot 0x114 → gsMaterial_OGL.
  - Texture container: the handler dynamic-casts the value to `gsITextureContainer` (from `new_texture`, or `gsTextureTarget_OGL.getTexture()`), gets the texture via container vfunc 0 and passes it to slot 0x13c (`0x100a62c0`), which binds it the same way.
  - Anything else, including null: calls `0x13c(0, ...)`, which clears the texture if a material exists.
  - Both paths then reset `background` and `hotBackground` (`+0xf8/+0xfc`) to an opaque base colour (`DAT_101974b4` with alpha 0xff, presumably white) and rebuild the image primitive (slot 0xe8).
- **Details:**
  - The second argument is a material texture slot, used for masks and glow layers with custom shaders. For example, `.setImage("/metro/ui/secondaryview/viewport-mask.dds", 1)` (observed: passed as the slot to gsMaterial_OGL +0x08).
  - Relative paths without a leading `/` resolve against the control's base directory, not the VFS root.
  - Slot 0x13c with a null texture and no material returns early, without touching the colours.
- **Evidence:** `0x100a8ac0.c:31-69`. Callees: `0x100a90a0.c:24-65`, `0x100a62c0.c:24-35` and gsVideo_OGL slot 0x114 `0x1004ef80.c` (vtable 0x1015ab5c = gsMaterial_OGL).
- **Confidence:** high. The exact base colour constant is BSS and was not readable statically.

## gsControl.setShader
- **Signature:** `setShader(path: string, [p0: float, p1: float, p2: float, p3: float]) -> unset`
- **Behavior:** Slot 0x14c (`0x100a91d0`; gsMapRouteCtrl overrides it) resolves the path against the control's base directory, gets the shader via video slot 0x110, assigns it to the control's lazily created material (`+0x138`, gsMaterial_OGL +0x0c) and rebuilds the image primitive. With exactly 5 args and an existing material, it also sets shader parameter 0 to `(p0,p1,p2,p3)` via gsMaterial_OGL +0x2c.
- **Details:**
  - With 2-4 args, the extra arguments are silently ignored (observed: the condition is `argc > 4`).
  - Ghidra labels the 4-float struct construction as `CRect::CRect` and adds spurious `(int)` casts. This is a folded identical constructor; the values are floats (inferred).
  - Usage: `.setShader("/metro/shaders/ui/sharpen.shader", 1.0/53.0, 1.0/53.0, 0.0, 0.0)`.
- **Evidence:** `0x100a8be0.c:34-57`; callee `0x100a91d0.c:35-61`.
- **Confidence:** high, apart from the CRect artifact interpretation (medium-high).

## gsControl.setArg
- **Signature:** `setArg(index: int, value: any) -> unset`
- **Behavior:** Slot 0x164 (`0x100a64d0`; gsGraphCtrl overrides it) stores the value, acquired through variant slot +0x14, into a 10-entry argument array at `this+0x190`, releasing the previous entry. In plain controls the args feed the `%`-format codes in `text`. For example, `.text = "Total wage cost: %it,d.20"; .setArg(0, $total)` (from en_US.strings). In gsGraphCtrl the args are sampled as float data sources (`0x100ab730` calls `toFloat` on each).
- **Details:**
  - The array is initialised to the valueless singleton (constructor `0x100a8d90`). `index` is not bounds-checked.
  - Usages like `.setArg(1, @$window.avgFps)` suggest that references and macros are kept live and re-evaluated at draw time (inferred; depends on slot +0x14 semantics).
  - The text formatter itself was not read.
- **Evidence:** `0x100a5e40.c:15-19`. Callees: `0x100a64d0.c:13-19`, `0x100a8d90.c:82-89` and gsGraphCtrl `0x100ab730.c`.
- **Confidence:** high for storage. Medium for the formatting semantics.

## gsControl.setMargins
- **Signature:** `setMargins(left: float, top: float, right: float, bottom: float) -> unset`
- **Behavior:**
  - Stores the margins at `+0x1d8/+0x1dc/+0x1e0/+0x1e4`.
  - Recomputes `clientWidth = width - left - right` and `clientHeight = height - top - bottom`.
  - Re-lays out the text (slot 0xec, `0x100a85e0`, which handles alignment) and, if a clip area exists (`+0x148`), updates it (slot 0xf4: gsIClipArea +0x0c with `(desktopLeft+left, desktopTop+top, clientW, clientH)`).
- **Details:**
  - The decompiled code is done inline in the handler, not through a virtual.
  - Unlike `setRect`, it does not re-lay out the children, even though their desktop position includes the parent's margins. Children update on the next layout pass (observed omission; possible quirk).
  - Ghidra's `(int)(float)` casts are artifacts of the int-typed `this`. The fields are floats, for example `width` at +0xe0 is a float.
- **Evidence:** `0x100a6f10.c:16-28`; callees `0x100a7120.c` (clip area) and `0x100a85e0.c` (text).
- **Confidence:** high.

## gsControl.blend
- **Signature:** `blend(targetVisibility: float, duration: float) -> unset`
- **Behavior:** Starts a visibility fade (slot 0x170, `0x100a7c50`).
  - If the control is not currently visible, `visibility` (`+0x1d4`) is first set to 0.
  - The control is shown (setFlag 2).
  - Then blending-active = 1 (`+0x1c8`), target = arg0 (`+0x1cc`) and rate = `(target - visibility) / duration` per second (`+0x1d0`).
- **Details:**
  - Ghidra mis-assigned the float parameters (the target appears as `unaff_retaddr`). The order is inferred from the formula and from usage: `.blend(1.0, 0.1)` fades in and `.blend(0.0, 0.2)` fades out.
  - Fading to 0 still shows the control first. Auto-hide at 0 was not traced (the tick was not read).
  - `duration = 0` divides by zero, giving an infinite rate.
  - `visibility` presumably multiplies GUI primitive alpha (inferred).
- **Evidence:** `0x100a5e00.c:15-19`; callee `0x100a7c50.c:13-19`.
- **Confidence:** medium-high (argument order inferred).

## gsControl.setBorder
- **Signature:** `setBorder(color: int ARGB, width: float, [sides: int = 15]) -> unset`
- **Behavior:** Slot 0x154 (`0x100a63f0`) stores the border colour (`+0x100`), width (`+0x10c`) and side mask (`+0x110`). The side mask uses `BORDER_TOP` 1 | `BORDER_BOTTOM` 2 | `BORDER_LEFT` 4 | `BORDER_RIGHT` 8, and the default 0xf means all sides. It rebuilds the border primitive (slot 0xf0) and, if an image primitive exists, the image primitive (slot 0xe8).
- **Details:**
  - The 3rd argument is only read when argc is exactly 3.
  - Usage: `.setBorder(MAIN.$ui.$borderColor, 2, BORDER_LEFT | BORDER_TOP | BORDER_RIGHT)`.
  - The border is drawn by the `+0x140` primitive, presumably a gsGuiPrimitive_OGL (inferred).
- **Evidence:** `0x100a5d50.c:17-29`; callee `0x100a63f0.c:11-17`.
- **Confidence:** high.

## gsControl.setFont
- **Signature:** `setFont(font: string, color: int ARGB, sizeX: float, sizeY: float, [spacingX: float], [spacingY: float = 0.0], [extra: string = ""]) -> unset`
- **Behavior:** Slot 0x158 (`0x100a8870`; gsEditCtrl, gsTextSelCtrl and gsGraphCtrl override it):
  - Stores the text colour (`+0x104`), size vector `(sizeX, sizeY, 1)` at `+0x120` and spacing vector `(spacingX, spacingY, 0)` at `+0x12c`.
  - Lazily creates the text object (`FUN_100a7bf0` → video slot 0xec(3, layer) → gsGuiText_OGL at `+0x144`).
  - Then calls gsIGuiText +0x10 setFont(font, extra), which goes through the font cache `video+0x1b4` (gsFonts_OGL, `FUN_10040980`). It also calls +0x28 setColor, +0x1c setSize and +0x20 setSpacing, and re-lays out the text (slot 0xec).
- **Details:**
  - The font path is not base-resolved (e.g. `"fonts/verdana"`, `"metro/fonts/blair-18"`, no extension).
  - Default size is `(0.015, 0.015)` (constructor `0x3c75c28f`). Scripts pass pixels or `0.015 * $desktop.height`.
  - The value used when only 4 args are given is lost in the decompilation (probably 0.0).
  - The meaning of the 7th string is unknown: it is passed to the font cache lookup and never used by shipped scripts.
  - The colour passed to gsIGuiText is `+0x104` combined via `FUN_10074ef0` (probably visibility-modulated; inferred).
- **Evidence:** `0x100a9af0.c:40-78`. Callees: `0x100a8870.c:16-32`, `0x100a7bf0.c`, gsGuiText_OGL +0x10 `0x10043a60.c`, and the default string `DAT_10156888 = ""`.
- **Confidence:** medium-high. The defaults for args 5 and 7 are uncertain.

## gsViewPort.addEffect
- **Signature:** `addEffect(effect: gsModule /* a post effect */) -> unset`
- **Behavior:** If the viewport has a native viewport object (`this+0x20c`, gsIViewport → gsViewport_OGL) and the argument is a gsModule, it calls gsIViewport +0x2c. `gsViewport_OGL::addEffect` (`0x100475b0`) dynamic-casts the effect to `gsPostEffect_OGL` and appends it to its effect list (`+0x70`, `std::list`). If the effect is a `gsTextureTarget_OGL`, it records it as the viewport's render target (`+0x7c`); otherwise `+0x7c` is cleared.
- **Details:**
  - Effects come from gsVideo_OGL: `new_bloomEffect`, `new_shadowEffect`, `new_shaderEffect("shaders/post_process/vertblur.shader")` and `new_textureTarget("textures/envmap.dds")`. Order of addition is presumably the render order (inferred).
  - Non-module arguments, non-post-effects and a missing native viewport are all silently ignored.
  - The handler releases its temporary reference. `0x100475b0` does not visibly addRef the stored pointer (insert helper not read), so script code may need to keep effects alive (uncertain).
  - This is the main seam between scripts and the post-processing chain (gsIVideoOutput_OGL implementers).
- **Evidence:** `0x100b4630.c:14-23`; callee `0x100475b0.c:14-24`; gsViewport_OGL vtable 0x1015a964 slot 0x2c (rtti-classes.json).
- **Confidence:** high for flow. Low on ownership and refcount.

## gsModule.children
- **Signature:** `children() -> array`
- **Behavior:** Builds a new script array (`FUN_1000a6a0`) and fills it with each direct child module from the child vector `this+0x4c..+0x50`. Each child gets a new reference (variant slot +0x04), and the array is filled through array vfunc +0x40 `(index, value)`.
- **Details:**
  - The array is a snapshot taken at call time.
  - Order is child-vector order. This is presumably the add order, as modified by `moveToFirst`/`moveToLast` (inferred).
  - Only direct children are included; there is no recursion.
- **Evidence:** `0x100a04b0.c:17-24`.
- **Confidence:** high.

## gsModule.hide
- **Signature:** `hide() -> unset`
- **Behavior:** Calls virtual 0xd8 `clearFlag(2)` (`0x100a0110`). It clears the `visible` bit in the module's own flags (`+0x68`), recomputes the effective flags (`+0x6c` = own & parent effective; parent at `+0x44`) and, if they changed, calls slot 0xa0 `onFlagsChanged(set, cleared)`.
- **Details:**
  - The base `onFlagsChanged` (`0x100a0550`) propagates the effective-flag recomputation recursively to all children.
  - gsControl's override (`0x100a66a0`) also calls setVisible on its GUI objects (`+0x13c` image, `+0x140` border, `+0x144` text, `+0x148` clip area; vfunc +0x04) and deactivates the control if it was active.
  - Many classes override `onFlagsChanged` (gsSound/gsMusic, gsSkeleton, gsSky, gsObject and others), so hiding affects rendering and audio per class.
  - Same effect as `.visible = false` (setter `0x1009faa0`).
- **Evidence:** `0x1009fa00.c:11`; callees `0x100a0110.c`, `0x100a0550.c`, `0x100a66a0.c`; slot overrides from rtti-classes.json.
- **Confidence:** high.

## gsModule.show
- **Signature:** `show() -> unset`
- **Behavior:** Calls virtual 0xd4 `setFlag(2)` (`0x100a00c0`). It sets the own `visible` bit; effective visibility still requires every ancestor to be visible. Changes propagate through `onFlagsChanged` exactly as for `hide`.
- **Details:**
  - The flag bits are 1 = enabled, 2 = visible and 4 = audible (registrar `0x100a0750.c:16-18`).
  - Equivalent to `.visible = true`. `enable()` (0x1009fa20) is the same pattern with bit 1.
- **Evidence:** `0x1009f9e0.c:11`; callee `0x100a00c0.c`.
- **Confidence:** high.

## gsModule.clear
- **Signature:** `clear() -> unset`
- **Behavior:** Calls virtual 0xac (`0x100a0a30`), which destroys every child module from last to first and then frees the child vector (`+0x4c..+0x54`) and a second vector (`+0x5c..+0x64`).
- **Details:**
  - Each child is destroyed with `FUN_1001c2e0`: mark it destroying, fire event 0 (`onDestroy`), call its virtual 0x74 teardown, mark it destroyed, and delete it if its refcount is below 1.
  - Script variables that still reference a child keep a destroyed object. Using it then raises error 0xe (inferred from the destroyed flag).
  - gsGridTimeline overrides slot 0xac (`0x10101550`, not read).
  - Used to empty UI lists before rebuilding them.
- **Evidence:** `0x1009f9c0.c:11`; callees `0x100a0a30.c` and `0x1001c2e0.c`.
- **Confidence:** high.

## gsStateMachine.perform
- **Signature:** `perform(action: string) -> unset`
- **Behavior:** Converts the action to a narrow string and pushes it onto the machine's action queue (`std::deque<std::string>` at `this+0xec`, via `FUN_1009d810`). Nothing happens synchronously. The queue is drained on the machine's timer tick (`0x1009e900`, the secondary vtable at `+0xcc`).
- **Details:**
  - Tick (observed):
    - If the current state (`+0x100`) has a timeout (state flag 2), the state time (`+0x108`) is advanced by `dt`. On reaching the timeout, `"timeout"` is enqueued.
    - Queued actions are then processed by `FUN_1009ce70`.
    - The loop compares a growing counter with a shrinking queue size, so only about ceil(n/2) actions are processed per tick; the rest wait for later ticks (observed loop shape; possible quirk).
  - Transition lookup (`FUN_1009ce70`): equal-range on the multimap `+0xe0`, keyed by (current state, action).
    - An entry with guard index -1 transitions unconditionally.
    - Otherwise the guard script function is run, and a truthy result transitions.
    - No match means the action is silently dropped.
  - Transition (`FUN_1009bd10`): calls the listener (`+0x10c`) onExit, fires event 6 (`exit`) and remembers the previous state. It runs the transition's script action if there is one, calls the listener's onTransition, and enters the target state (`FUN_1009d060`; the `enter` event is presumed). A flag selects "return to previous state".
  - Transition tables come from `.states` files (`loadFromFile`, e.g. `metro/scripts/main.states`).
  - Typical actions: `"ok"`, `"cancel"`, `"error"`, `"completed"`, `"timeout"`.
- **Evidence:** `0x1009e870.c:22-24`. Callees: `0x1009d810.c`, `0x1009e900.c:19-67` (timeout at :30, processing at :43), `0x1009ce70.c` and `0x1009bd10.c`; the constructor `0x1009f420.c` gives the deque layout.
- **Confidence:** high for queuing and transition lookup. Medium for enter/exit event details and narrow-string conversion specifics (`FUN_10024a20` not read).
