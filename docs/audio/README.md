# Audio (native `GSystem.dll` + scripts)

How Cities in Motion makes sound: the script API, what each native does with FMOD Ex, the 3D model, the data, and what
a port needs to sound like the original. Scope: everything audio in `GSystem.dll` 1.0.22 (Windows) and the matching
Linux build, plus every script that uses it.

**Conventions.** `L:0x…` is a function in the Linux build (`decompiled/Linux/functions/<addr>.c`, real symbol names),
`W:0x…` the Windows `GSystem.dll` (`decompiled/GSystem-v2/functions/<addr>.c`). Struct offsets are given as **W/L**
(Windows / Linux; the Linux objects are smaller: for `gsSound` W = L + 0x38). *obs* = read in code, *inf* = inferred
from code, *unverified* = an assumption (mostly an FMOD Ex default, since the game never sets it). Script paths are in
`extracted/metro/...`. Disassembly checks: `tools/ghidra/DumpAsm.java` on project CiMLinux, and raw bytes of
`GSystem.dll` (file offset = RVA).

Caution for later work: `decompiled/GSystem-v2/linux-names.csv` mismatches several functions in this cluster
(`gsSound::setActive` → `0x1009a3e0`, `gsSound::setPlaying` → `0x1009a7d0` and `gsSound::setVelocity` → `0x1009a290`
are wrong; the first two are std::map helpers). The Windows addresses below were matched by hand.

## Contents

1. [Summary](#1-summary)
2. [Script API reference](#2-script-api-reference)
3. [Sound system lifetime (FMOD init / shutdown)](#3-sound-system-lifetime)
4. [Speaker modes and output settings](#4-speaker-modes-and-output-settings)
5. [Loading (`load`, the sound cache)](#5-loading)
6. [The gsSound object (play, stop, pause, tick)](#6-the-gssound-object)
7. [gsMusic](#7-gsmusic)
8. [gsListener and the per-frame update](#8-gslistener-and-the-per-frame-update)
9. [The 3D model](#9-the-3d-model)
10. [Volumes and settings](#10-volumes-and-settings)
11. [Engine-driven world sounds (vehicles, characters, items)](#11-engine-driven-world-sounds)
12. [Ambience and music (scripts)](#12-ambience-and-music)
13. [Script call sites by purpose](#13-script-call-sites-by-purpose)
14. [Time, pause, focus and game speed](#14-time-pause-focus-and-game-speed)
15. [Data: archives and formats](#15-data-archives-and-formats)
16. [Port plan](#16-port-plan)
17. [Open questions](#17-open-questions)
18. [Port status (ReGS)](#18-port-status-regs)

---

## 1. Summary

* **Library.** FMOD Ex: `fmodex.dll` 4.32.08 (Windows), `libfmodex-4.36.21.so` (Linux). The game uses 32 FMOD entry
  points only: `System` create/init/close/release, driver enumeration, `setDriver`, `setSpeakerMode`,
  `setDSPBufferSize`, `set3DSettings`, `createSoundGroup`, `createSound`, `playSound`, `set3DListenerAttributes`,
  `update`; `Sound` `setSoundGroup`/`getLength`/`release`; `SoundGroup` `setVolume`/`release`; `Channel` `stop`,
  `isPlaying`, `setPaused`, `setVolume`, `getFrequency`/`setFrequency`, `setMode`, `getPosition`/`setPosition`,
  `set3DAttributes`, `set3DMinMaxDistance` (the `<EXTERNAL>::FMOD::*` thunks in `decompiled/Linux/index.csv`). **Not
  used:** `createStream`/`FMOD_CREATESTREAM`, channel groups, DSP effects, reverb, geometry/occlusion, HRTF,
  `setOutput`, `setSoftwareFormat`, `setSoftwareChannels`, `setHardwareChannels`, `set3DNumListeners`, pan,
  `set3DSpread`, `set3DPanLevel`, custom rolloff, FMOD Designer events.
* **Script surface.** 3 classes (`gsSound` with 7 fields + 5 methods, `gsMusic` = `gsSound` with another loader,
  `gsListener` with 5 fields), 8 global functions (`new_sound`, `new_music`, `new_listener`, `enable_sounds`,
  `disable_sounds`, `set_audio_volume`, `audio_devices`, `audio_speakers`), and on the grid side 7 model sound fields
  (`engineSound`, `moveSound`, `moveTunnelSound`, `doorOpenSound`, `doorCloseSound`, `hornSound`, `bellSound`), the
  template `ambience` int, `gsGridController.forceSounds`, `gsGridVehicleController.playSound(s)` and
  `gsGrid.getAreaType(x, z, d)`. No audio events exist; scripts poll `currentPos`/`length`.
* **Every sound is a software, in-memory sample.** `load` reads the whole file from the VFS and calls
  `createSound(FMOD_OPENMEMORY | FMOD_SOFTWARE | …)`; music adds `FMOD_CREATECOMPRESSEDSAMPLE` (MP3 kept compressed,
  decoded while playing). No streaming from disk.
* **3D model.** One listener at the camera eye (right-handed, forward −Z, up +Y), linear rolloff between
  `minDistance` (default 10) and `maxDistance` (default 100) in world units (1 grid cell = 3.0 units), Doppler on
  (scale 1, distance factor 1). The engine starts a sound only inside `maxDistance` and stops loops that stay outside it
  for 0.5 s.
* **Volumes.** One FMOD sound group ("default") that every sound joins: `set_audio_volume` = master. Everything else
  (music volume, ambience 0.2, UI sound levels) is a per-object `volume` the scripts set. `gsListener.volume` is stored
  but never used.
* **Player options.** Master volume, music volume, music on/off, output device, speaker mode (Default, Mono, Stereo,
  Quad, Surround 5.0, 5.1, 7.1, Prologic), and in Config.exe / the settings file "disable audio". No headphone/HRTF mode.
* **Data.** `sounds.gs` 227 files (220 WAV, all PCM 16-bit mono 44.1 kHz; 7 ambience MP3 256 kbit/s), `music.gs` 6 MP3
  (320 kbit/s, 44.1 kHz, 57 min). DLC audio only in `tokyo.gs` (59 WAV + 4 MP3) and `usa.gs` (2 MP3, one at 48 kHz).

---

## 2. Script API reference

Registered in `gsSounds_init` (L:0x0812a9b0, W:0x1009b890) and the class registrars `gsSound::initializeClassData`
(L:0x08126f70, W:0x1009a670) and `gsListener::initializeClassData` (L:0x08126e00, W:0x10098620). Use counts are from
`docs/native-api.md` (they count all same-named members, e.g. `volume`).

### 2.1 Global functions

| Name | Args | Handler W / L | Semantics |
|---|---|---|---|
| `new_sound(parent)` | 1 | W:0x100983f0 / L:0x0812cc20 (`gsNewModule<gsSound>`) | New `gsSound`; `setParent(parent)` if arg 0 is a module, else no parent. Defaults §6.1. |
| `new_music(parent)` | 1 | W:0x100984b0 / L:0x0812c950 | New `gsMusic` (§7). |
| `new_listener(parent)` | 1 | W:0x10098560 / L:0x0812cb00 | New `gsListener`; the constructor makes it **the** global listener (`listener = this`, L:0x0812a900). Only one is ever used: the last created. |
| `enable_sounds([device = "Default"], [speakers = "Default"])` | 0–2 | W:0x10099ec0 / L:0x08128760 | Initialises FMOD once (§3). A second call does nothing (init returns when the "enabled" byte is set). |
| `disable_sounds()` | 0 | W:0x1009a8d0 / L:0x0812bc10 | If enabled: releases all cached sounds, the group and the system (`gsSounds::destroy`, L:0x0812bb30, W:0x1009a5b0). Not used by any script. |
| `set_audio_volume(v)` | 1 | W:0x10097ef0 / L:0x08126cb0 | If enabled: `SoundGroup("default").setVolume(toFloat(v))` = master volume of every sound (§10). |
| `audio_devices()` | 0 | W:0x1009b6b0 / L:0x0812b9b0 → `gsEnumSoundDevices` L:0x0812b6f0 | Array of strings: `"Default"` followed by every FMOD driver name. It creates a temporary `FMOD::System`, enumerates `getNumDrivers`/`getDriverInfo(i, name, 256)` (Windows `getDriverInfoW`), then closes and releases it. Works before `enable_sounds`. |
| `audio_speakers()` | 0 | W:0x1009b7a0 / L:0x0812b570 → `gsEnumSoundSpeakers` L:0x0812af60 | Fixed array `["Default", "Mono", "Stereo", "Quad", "Surround", "5p1", "7p1", "Prologic"]`. |

### 2.2 gsSound (and gsMusic)

`gsSound : gs3DElement` (so it also has `position`, `orientation`, `visible`, `enabled`, `audible`, `owner`, …).
Script field offsets W/L; setters marked "set" push the change to a live channel at once.

| Member | Kind | W / L offset, handler | Semantics |
|---|---|---|---|
| `pitch` | float | 0x150 / 0x118, set W:0x1009a1e0, L:0x08128960 | Frequency factor. Channel frequency = `baseFreq · pitch · pitchMult` (§6.4). Default 1. |
| `volume` | float | 0x15c / 0x124, set W:0x1009a240, L:0x081288c0 | Channel volume = `volume · volumeMult` (FMOD clamps to 0..1, *unverified*). Default 1. |
| `relative` | bool | 0x146 / 0x10e, set W:0x1009a290 (→ W:0x100997d0), L:0x08128a00 | Head-relative: position and distance are taken relative to the listener (§9.4). Default false. |
| `minDistance` | float | 0x148 / 0x110, set W:0x1009a2b0, L:0x08128ab0 | Full-volume radius. Default 10.0. |
| `maxDistance` | float | 0x14c / 0x114, set W:0x1009a2d0, L:0x08128bd0 | Silence radius; also the engine's start/stop range (§6.5). Default 100.0. |
| `currentPos` | float ro | 0x168 / 0x130, read hook W:0x100978f0, L:0x08127210 | Seconds: `Channel::getPosition(FMOD_TIMEUNIT_MS) · 0.001`, 0 when no channel. |
| `length` | float ro | 0x16c / 0x134 | Seconds, `Sound::getLength(ms) · 0.001`. **Filled on the first activation only**; 0 before the first play (and again after every `gsSound.load`, §5.3). |
| `load(path, [threeD = true])` | 1–2 | W:0x10098830, L:0x08127690 → vtable `loadSound` | §5. |
| `play([loop = false])` | 0–1 | W:0x1009a0e0, L:0x0812a720 | `setPlaying(true, loop)` (§6.3). |
| `stop()` | 0 | W:0x1009a180, L:0x0812a6f0 | `setPlaying(false, false)`. |
| `pause()` | 0 | W:0x1009a1a0, L:0x0812a690 | Saves `Channel::getPosition(ms)`, stops, clears loop; the next `play` resumes there (§6.3). |
| `clone()` | 0 | W:0x100978d0, L:0x08126c50 | New object of the same class under the same owner, copying sound, relative, min/max, pitch (+mult), volume (+mult), length. Not copied: loop flag, position, play state. Returns the clone. |

No events, no `playing`/`isPlaying` field, no `loop` field (looping is the `play` argument).

### 2.3 gsListener

| Member | Kind | W / L offset | Semantics |
|---|---|---|---|
| `volume` | float | 0x13c / 0x104, set W:0x10098350, L:0x08126c90 | Stored only. Nothing reads it (only `write_volume` and the unused `gsListener::setVolume` L:0x0812a9a0 touch +0x104). `window.script:90` sets it to `$settings.$volume`. |
| `tracked` | object | 0x140 / 0x108 | Each tick the listener copies `tracked`'s global position + `tracked.orient · offset` and its orientation (§8). Set to the main camera (`engine.script:243`). |
| `tracked2` | object | 0x144 / 0x10c | Stored (and released in `destroy`, L:0x08126d30), **never used** by the tick. Scripts set it to the secondary-view camera. |
| `offset` | vector | 0x148 / 0x110 | Offset in `tracked`'s frame. Scripts keep it at `(0, 0, cameraZoom)` (`ui.script:101`) = the camera eye (§8). |
| `offset2` | vector | 0x154 / 0x11c | Stored, never used (`secondaryview.script:773` sets `(0, 0, 100)`). |

### 2.4 Grid-side members

| Member | Class | W / L offset or handler | Semantics |
|---|---|---|---|
| `engineSound`, `moveSound`, `moveTunnelSound`, `doorOpenSound`, `doorCloseSound`, `hornSound`, `bellSound` | gsGridObject (vehicle, character and item models/templates) | W 0x37c, 0x380, 0x384, 0x388, 0x38c, 0x390, 0x394 / L 0x318 … 0x330 | Template sounds (`new_sound(this)` in the object script). Never played themselves: the engine clones them under the grid at the agent (§11). |
| `ambience` | gsGridObject (int) | W 0x3b8 / L 0x354 | Area-type weight for `getAreaType` (3 industrial, 4 harbour; §12.1). |
| `forceSounds` | gsGridController (bool) | W 0x139 / L 0x101 | When true, the agent's sounds play head-relative at the listener (full volume, no pan, no range limit). Set while the agent is shown in the secondary camera view (`cameratarget.script:89`, `secondaryview.script:283,771`). |
| `playSound(s)` | gsGridVehicleController | W:0x10107b60, L:0x081adc10 | `gsGridVehicle::playSound(s, loop=false, pitch=1.0)` (L:0x081adaa0, W:0x10107a50): one-shot clone at the vehicle (§11.1). |
| `getAreaType(x, z, diameter)` | gsGrid | W:0x100b5d40, L:0x0813ca60 | Dominant ambience type 0..4 around a point (§12.1). |

---

## 3. Sound system lifetime

The global sound manager `gsSounds sounds` (L:0x0830af20, W:0x10196e1c), constructor L:0x08129120:

| W/L offset | Field | Default |
|---|---|---|
| +0x00 | `enabled` byte (the `sounds` flag every native checks) | 0 |
| +0x04 | `FMOD::System*` (W:0x10196e20, L:0x0830af24) | 0 |
| +0x08 | `FMOD::SoundGroup*` "default" (W:0x10196e24, L:0x0830af28) | 0 |
| +0x0c | speaker mode | 2 (stereo) |
| +0x10 | driver caps | 0 |
| +0x14 (W also +0x18) | floats from `getDriverCaps` (frequency values; not used elsewhere, *inf*) | 0 |
| L +0x18 / W +0x1c | `std::map<wstring, FMOD::Sound*>` path → sound cache | empty |

### 3.1 `enable_sounds` → `gsSounds::init(device, speakers)` (L:0x08129170, W:0x100991a0)

*obs* (Linux pseudocode + disassembly `asm-linux/0x08129170.asm`; Windows push sequence read from the DLL bytes at
0x100993fa, 0x10099594, 0x1009962d, 0x10099680):

```
if enabled: return
info "initializing FMOD..."
FMOD_System_Create(&system)
n = system.getNumDrivers()
for i in 0..n-1:                                     // W: getDriverInfoW, L: getDriverInfo + fromUTF8
    name = system.getDriverInfo(i, buf, 256, null)
    if name == device: system.setDriver(i); break     // "Default" matches nothing → FMOD's default driver
system.getDriverCaps(0, &caps, …, &mode)              // mode := the control-panel speaker mode (+0x0c)
mode = { "Mono":1, "Stereo":2, "Quad":3, "Surround":4, "5p1":5, "7p1":6, "Prologic":7 }[speakers]  // else unchanged
if speakers != "Default": system.setSpeakerMode(mode)
if caps & 0x2 (FMOD_CAPS_HARDWARE_EMULATED):
    system.setDSPBufferSize(1024, 10); info "Warning: Audio acceleration off."
r = system.init(256, 0x2 /*FMOD_INIT_3D_RIGHTHANDED*/, null)
if r == 0x3a (W) / 0x38 (L):                          // the "create buffer" failure of FMOD's sample code (inf)
    system.setSpeakerMode(2 /*STEREO*/); system.init(256, 0x2, null)
system.set3DSettings(1.0 doppler, 1.0 distanceFactor, 1.0 rolloffScale)
system.createSoundGroup("default", &group)
info "[done]"; enabled = 1
```

Every FMOD call goes through `ERRCHECK` (L:0x08127af0, W:0x10098da0): on a result ≠ FMOD_OK it reports
`"FMOD error (<n>): <FMOD_ErrorString>"` via `gsMain::errorMsg` (L:0x08135920: fires MAIN event 7, else the default
error handler) and returns false; `init` then stops where it is and `enabled` stays 0.

**If initialisation fails** (no device, driver error) the game runs silent: `getSound` returns null for every load,
`set_audio_volume` and the listener tick do nothing, `play` never starts a channel. No script error is raised.

`init.script:29-32` calls `enable_sounds($settings.$audiodevice : "Default", $settings.$audiospeakers : "Default")`
and `set_audio_volume($settings.$volume : 1.0)` only when `$settings.$sounds : true`. With `$sounds = false` the
game is silent and `main.script:449` hides the Audio menu button.

### 3.2 Shutdown

`gsFreeSounds` (L:0x0812bbf0, W:0x1009a660), called from `gsDestroyAll` (L:0x08107f60) at exit, runs
`gsSounds::destroy`: clear `enabled`, `Sound::release` for every cached sound, clear the map, `SoundGroup::release`,
`System::close`, `System::release`. `disable_sounds` does the same at runtime; existing gsSound objects keep stale
`Sound*` pointers afterwards (unused path; a port can ignore it).

---

## 4. Speaker modes and output settings

### 4.1 What the original offers the player

| Where | Options |
|---|---|
| In-game **Audio** menu (`ui/menus/audio.script`, only when `$sounds` is true) | "Play music" checkbox (`$music`), "Audio volume" slider 0–100 % (`$volume`), "Music volume" slider (`$musicVolume`), "Device" list (`audio_devices()`, first entry shown as "Default"), "Speakers" list (`audio_speakers()`): Default, Mono (1.0), Stereo (2.0), Quadraphonic (4.0), Surround (5.0), Surround with subwoofer (5.1), Home theatre (7.1), Prologic (encoded 5.1) (strings `en_US.strings:81-94`). `save()` (`:131-145`) applies the master volume at once (`set_audio_volume`); a changed device or speaker mode sets `$requireRestart`, because `enable_sounds` only runs once. |
| **Config.exe** (Delphi launcher in the install, 515 584 bytes; read statically with `tools/pestrings.mjs`) | Uses `GSystem.dll`'s script engine: runs `audio_devices()`/`audio_speakers()` and writes the settings with `MAIN.$settings.exportVariables(…, "sounds", "audiodevice", "audiospeakers", "music", "volume")` (string at file offset 0x5897c). Controls: "Disable audio" checkbox (`SoundsCheck` → `$sounds`), Device box, Speakers box, "Play in-game music" (`$music`), "Audio volume" track bar (`$volume`). It does not offer `$musicVolume`. Strings `$config_audio*` in `en_US.strings:4396-4409`. |
| `userdata/settings.script` (`defaults/settings.script:25-31`) | `$sounds = true`, `$music = true`, `$audiodevice = "Default"`, `$audiospeakers = "Default"`, `$volume = 0.8`, `$musicVolume = 0.5`. |

So the player gets: sound on/off (launcher/settings file only), master volume, music on/off, music volume, output
device, and a speaker layout from mono to 7.1 including a Prologic matrix mode. There is **no** separate effects or
ambience volume, no headphone/HRTF option, no sample-rate or quality option.

### 4.2 What the engine does with them (*obs*, §3.1)

* **Speaker mode values** are FMOD's `FMOD_SPEAKERMODE` numbers: MONO 1, STEREO 2, QUAD 3, SURROUND (5.0) 4,
  5POINT1 5, 7POINT1 6, and 7 for "Prologic". In older FMOD Ex headers 7 is `FMOD_SPEAKERMODE_PROLOGIC`; later 4.x
  headers give 7 to `FMOD_SPEAKERMODE_SRS5_1_MATRIX`. Which one 4.32.08 / 4.36.21 use is *unverified*.
* **"Default" never calls `setSpeakerMode`**, so FMOD Ex's own default applies. FMOD Ex documents that default as
  stereo, which would mean "Default" = stereo even on a 5.1 system (*unverified* on hardware). The control-panel mode
  that `getDriverCaps` writes into `+0x0c` is only applied when the setting holds an **unknown** string (then
  `setSpeakerMode(controlPanelMode)` runs). A port can therefore add new values to `$audiospeakers` without
  breaking the original executable: the original falls back to the Windows speaker setup for them.
* **Output type:** `setOutput` is never called → FMOD auto-detects the output (Windows DirectSound/WASAPI, Linux
  ALSA/OSS/PulseAudio, per FMOD's rules, *unverified* which).
* **Mixer format:** `setSoftwareFormat` never called → FMOD's default software mixer format (48 kHz in FMOD Ex docs,
  *unverified*). All sounds are created `FMOD_SOFTWARE`, so `setHardwareChannels` is irrelevant and never called.
* **Voices:** `init(256, …)` = 256 virtual channels. `setSoftwareChannels` is never called → FMOD Ex's default of
  64 really mixed software voices; beyond that FMOD virtualises the least audible ones (*unverified* default).
* **DSP buffer:** only changed when the driver reports emulated (non-accelerated) DirectSound:
  `setDSPBufferSize(1024, 10)` (classic FMOD sample code). Otherwise FMOD's default (*unverified*: 1024 × 4).
* **3D settings:** `set3DSettings(doppler 1.0, distance factor 1.0, rolloff scale 1.0)`; one listener;
  `FMOD_INIT_3D_RIGHTHANDED`. No `FMOD_INIT_HRTF_LOWPASS`, no `FMOD_INIT_SOFTWARE_HRTF`, no occlusion, no reverb,
  no `FMOD_SPEAKERMODE_MYEARS` (headphone) mode.

**Surround in practice:** the 3D voices (vehicles, items, voices) are panned by FMOD's software 3D panner over the
selected layout. Music, ambience and UI sounds sit on the listener (head-relative at distance 0, §9.4), so they come
out centred/front. Whether FMOD sends anything to LFE by default is *unverified*.

---

## 5. Loading

### 5.1 Call chain

`gsSound.load(path, threeD = true)` (W:0x10098830, L:0x08127690): `threeD = toBool(arg 1)` when given, then vtable
`loadSound(path, threeD)`:

* `gsSound::loadSound` (L:0x0812c7d0, W:0x1009b570): `gsExpandFileName(path)` (base-directory rules in
  `docs/handlers/batch-B.md` §gsSound.load: `/x` and `~/x` are root-relative, other paths are relative to the object's
  directory), `sound = gsSounds::getSound(path, isMusic = false, threeD)`, `length = 0`.
* `gsMusic::loadSound` (L:0x0812c890, W:0x1009b610): same with `isMusic = true`; does not reset `length`.

Loading never plays anything and never stops a channel that already plays the previous sound.

### 5.2 `gsSounds::getSound(path, isMusic, threeD)` (L:0x0812bc80 and the error-reporting copy L:0x0812c060; W:0x1009adc0, W:0x10098f70)

```
if !enabled: return null
if cache has path: return cache[path]        // key = expanded path only (case as written; std::less<wstring>)
file = gsFile(path)                           // whole file into memory from the VFS
exinfo = { cbsize 0x88, length = file.size }
mode = threeD ? 0x200850 : 0x848             // see table
if isMusic: mode |= 0x200
r = system.createSound(file.data, mode, &exinfo, &sound)
if r != OK: errorMsg "Error loading audio file: <path>", errorMsg "FMOD error (<n>): <text>"; return null   // not cached
sound.setSoundGroup(group "default")
cache[path] = sound; return sound
```

| Mode | Bits (FMOD Ex) | Used by |
|---|---|---|
| `0x200850` | `FMOD_3D (0x10) · FMOD_SOFTWARE (0x40) · FMOD_OPENMEMORY (0x800) · FMOD_3D_LINEARROLLOFF (0x200000)` | `load(path)` — world sounds, query voices |
| `0x848` | `FMOD_2D (0x8) · FMOD_SOFTWARE · FMOD_OPENMEMORY` | `load(path, false)` — UI, music, ambience |
| `+0x200` | `FMOD_CREATECOMPRESSEDSAMPLE` | gsMusic: the MP3 stays compressed in memory and is decoded while playing |

Consequences (*obs*):

* **The first load of a file fixes its mode for the whole session**, because the cache key ignores `threeD` and
  `isMusic`. Example: object scripts load query sounds with the default `threeD = true`; the same file is never also
  loaded 2D, so no clash occurs in the shipped scripts (checked with `soundscan`, §15.4: no literal path in a
  `with (… = new_sound|new_music(…)) { .load(…) }` block of the vanilla or DLC scripts is loaded in two modes or by
  both classes).
* A failed load is retried (and logged) on every later `load` of the same path.
* Shipped scripts reference one missing file: `hqpanel.script:79` loads `/metro/sounds/2. Purchase sound1.wav`, which
  is in no archive, so the HQ panel's `$acceptLoanSound` is silent (two log lines at panel creation).
* Loads are synchronous; there is no async/streaming path.

### 5.3 What a loaded object holds

`gsSound+0x13c/+0x104` = the shared `FMOD::Sound*`. Many gsSound objects share one `Sound` (e.g. every vehicle clone
of a template sound, every `$clickSound` of the different menus). Volume, pitch, distances and play state are per
object; mode, rate and data are per file.

---

## 6. The gsSound object

### 6.1 Fields and defaults (constructor L:0x08129890; same values in `gsNewModule<gsSound>` and `createInstance`)

| W / L | Field | Default |
|---|---|---|
| 0x13c / 0x104 | `FMOD::Sound*` | null |
| 0x140 / 0x108 | `FMOD::Channel*` | null |
| 0x144 / 0x10c | active (a channel was started) | false |
| 0x145 / 0x10d | loop | false |
| 0x146 / 0x10e | relative | false |
| 0x147 / 0x10f | autoFree (free the object once its channel ends; `createInstance` arg 2) | false |
| 0x148 / 0x110 | minDistance | 10.0 |
| 0x14c / 0x114 | maxDistance | 100.0 |
| 0x150 / 0x118 | pitch | 1.0 |
| 0x154 / 0x11c | pitchMult (engine-only) | 1.0 |
| 0x158 / 0x120 | baseFreq (`Channel::getFrequency` at start) | — |
| 0x15c / 0x124 | volume | 1.0 |
| 0x160 / 0x128 | volumeMult (engine-only) | 1.0 |
| 0x164 / 0x12c | out-of-range timer (s) | 0 |
| 0x168 / 0x130 | currentPos (s) | 0 |
| 0x16c / 0x134 | length (s) | 0 |
| 0x170 / 0x138 | velocity (engine-only, `setVelocity`) | 0 |
| 0x17c / 0x144 | "check channel" flag (set after each tick; then the next access asks `isPlaying`) | false |
| 0x180 / 0x148 | resume position (ms) | 0 |
| 0x184 / 0x14c | next sound in an agent's sound list | null |

Object size W 0x188 / L 0x150.

### 6.2 Channel validity (`isPlaying` L:0x081299a0, W:0x10097a90)

`channel != null`, and if the "check" flag is set, `Channel::isPlaying()` must succeed and return true; otherwise the
channel pointer is dropped (set to null). Every setter does this check before touching the channel. A finished
one-shot therefore becomes "no channel" at the next access after a tick.

### 6.3 play / stop / pause (`setPlaying(play, loop)` L:0x0812a500, W:0x10099a40)

```
setPlaying(true, loop):
    if this.loop and loop and channel valid: return          // play(true) on a live loop: no restart
    this.loop = loop; setActive(false)                      // stops the current channel (restart from 0)
    if enabled-flag and audible-flag of the module (status & 1, & 4) and sounds.enabled:
        setActive(distance() < maxDistance)                  // out of range: stays silent (a loop restarts later, §6.5)
setPlaying(false, x): this.loop = x (always false from scripts); setActive(false)
```

`setActive(on)` (L:0x08129e50, W:0x100998c0):

```
on := on and sound != null and (status & 4 /*audible*/)
if on == active: return
active = on
if on:
    system.playSound(FMOD_CHANNEL_FREE, sound, paused = true, &channel)
    if resumeMs: channel.setPosition(resumeMs, FMOD_TIMEUNIT_MS)
    check = false; baseFreq = channel.getFrequency()
    channel.setFrequency(baseFreq · pitch · pitchMult)
    channel.setVolume(volume · volumeMult)
    channel.setMode(mode)                                    // see below
    if length == 0: length = sound.getLength(FMOD_TIMEUNIT_MS) · 0.001
else:
    if channel valid: channel.stop()
    channel = null; resumeMs = 0
```

Channel mode (`gsSound::update` L:0x08129a10, W:0x10099720, also in setRelative/setActive):
`FMOD_3D | FMOD_SOFTWARE | FMOD_3D_LINEARROLLOFF | (loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF) |
(relative ? FMOD_3D_HEADRELATIVE : FMOD_3D_WORLDRELATIVE)` = `0x240051`/`0x240052` (head-relative) or
`0x280051`/`0x280052` (world). This is applied to sounds loaded 2D as well (§17 Q1).

The channel starts **paused**; the next `move` tick sets its 3D attributes and unpauses it (§6.5). A sound
therefore starts one tick late (up to one frame).

`pause()` (W:0x1009a1a0): `pos = channel.getPosition(ms)`, `loop = false`, `setActive(false)`, `resumeMs = pos`. The
next `play(loop)` starts from `pos` (the inactive `setActive(false)` inside `setPlaying` returns early and keeps
`resumeMs`). Notes:
* `stop()` after `pause()` keeps `resumeMs` too (setActive(false) on an inactive sound returns early), so the next
  `play` still resumes. *obs*
* `pause()` with no channel calls `getPosition` on a null channel; FMOD returns an error without writing the output,
  so `resumeMs` gets an uninitialised value (*inf*). A port should store 0.
* `pause()` forgets `loop`; scripts pass it again on resume (`ambience.script:102-106`).

Restart behaviour: `play()` on a sound that is still playing stops it and starts again from 0. One gsSound object
plays at most **one channel at a time**; overlapping copies need `clone()` (ambience) or the engine's per-play clones
(vehicles).

### 6.4 Live parameter changes

| Change | FMOD call | When |
|---|---|---|
| `pitch`, pitchMult | `setFrequency(baseFreq · pitch · pitchMult)` | at once |
| `volume`, volumeMult | `setVolume(volume · volumeMult)` | at once |
| `relative` | `setMode(…)` | at once |
| `minDistance`/`maxDistance` setters | re-apply frequency, volume and mode (L:0x08128ab0) | at once; the distances themselves are sent each tick |
| position, velocity | `set3DAttributes` | each tick (§6.5) |

### 6.5 Tick (`gsSound::move(dt)` L:0x0812a110, W:0x10099af0)

```
if !active and loop and distance() < maxDistance: setActive(true)          // loops come back into range
if channel valid:
    pos = globalPos(); vel = parent.velocityAtGlobalPos(pos) + this.velocity
    channel.set3DAttributes(pos, vel)
    channel.set3DMinMaxDistance(minDistance, maxDistance)
    channel.setPaused(false)
    if distance() <= maxDistance: outOfRange = 0
    else: outOfRange += dt; if outOfRange >= 0.5: setActive(false)        // loops and one-shots alike
elif autoFree: free(this); return
check = true
gsMoveable::move(dt)
```

`distance()` (`getDistance` L:0x0812a760, W:0x10097c10): relative → `|localPos|`; else `|globalPos − listener.globalPos|`,
or `|globalPos|` when there is no listener.

`statusUpdated` (L:0x0812a440, W:0x100999f0): when the module's effective flags change, a looping sound that is now
enabled + audible is restarted with `setActive(true)` (without a range check; the tick stops it 0.5 s later if it is
out of range); any other change of the enabled or audible bit (`changed & 5`) stops the sound. `updateScene(null)`
(L:0x0812a3f0) stops it when it leaves its scene; `destroy` (L:0x0812a4c0) stops it.

---

## 7. gsMusic

`gsMusic` (RTTI base `gsSound`, constructor L:0x0812a7e0) shares `gsSound`'s class data (same fields and methods).
Differences: `loadSound` passes `isMusic = true` (compressed sample, §5.2) and does not reset `length`;
`createInstance` (L:0x081274e0) does not copy the autoFree flag. Scripts use it for the menu, ending and era music and
for the seven ambience loops. The music is not "streamed" in FMOD terms: the whole MP3 (up to 34.8 MB) is held in
memory and decoded on the fly.

---

## 8. gsListener and the per-frame update

`gsListener::move(dt)` (L:0x08127e30, W:0x10099c50), *obs* (Linux disassembly):

```
if sounds.enabled:
    if tracked:
        setGlobalPos(tracked.globalPos + tracked.globalOrient · offset)       // vtable +0xfc
        setGlobalOrient(tracked.globalOrient)                                 // vtable +0x100
    vel = (tracked ?: parent).velocityAtGlobalPos(globalPos)
    forward = globalOrient · gsNegZVector (0, 0, −1)
    up      = globalOrient · gsPosYVector (0, 1, 0)
    system.set3DListenerAttributes(0, globalPos, vel, forward, up)
    system.update()                                   // the only System::update call in the program
gsMoveable::move(dt)
```

* The listener is created on `$window` at `(0, 0, −2)` (`window.script:88-91`) and ticks every frame in real time.
  In a game, `engine.script:242-243` sets `tracked = $camera` (the floating camera) and `offset = (0, 0, 150)`;
  `ui.script:101` keeps `offset = (0, 0, cameraZoom)`. The floating camera's eye is `P + rot(Q, offset)` with the same
  offset (`docs/renderer/scene-basics.md` §camera), so **the listener sits at the camera eye, oriented like the
  camera**. Zoom (camera distance) ranges 30–310 world units (`engine.script:235-236`).
* After the game (`engine.script:301`) `tracked = null`; the listener then stays where it was.
* `tracked2`/`offset2` are ignored: the secondary camera view does not move the listener. The followed vehicle is
  made audible with `forceSounds` instead (§11.1).
* Without a listener object FMOD's `update` never runs (*obs*: no other call site). The game always has one.

---

## 9. The 3D model

### 9.1 Space and units

* World units of the scene (`gs3DElement` global positions). One grid cell = `cellSize` 3.0 units
  (`docs/save-format/README.md`); a bucket = 16 cells = 48 units.
* FMOD runs right-handed (`FMOD_INIT_3D_RIGHTHANDED`), matching the OpenGL scene: listener forward −Z, up +Y,
  right = forward × up = +X in camera space.
* `set3DSettings(1, 1, 1)`: FMOD treats 1 unit as 1 metre; Doppler uses FMOD's speed of sound (340 units/s in FMOD
  Ex docs, *unverified*).

### 9.2 Attenuation (linear rolloff, `FMOD_3D_LINEARROLLOFF`)

```
d = |source − listener|
g_dist = 1                         if d <= minDistance
       = (maxDistance − d)/(maxDistance − minDistance)   if minDistance < d < maxDistance
       = 0                         if d >= maxDistance
gain = clamp(volume · volumeMult, 0, 1) · masterVolume · g_dist
```

FMOD's linear rolloff ignores the rolloff scale (*unverified*; it is 1 anyway). The engine-side range checks
(start only if `d < maxDistance`, stop after 0.5 s beyond it) coincide with `g_dist = 0`, so they only save voices.

Template values (from `extracted/metro/objects`, `soundscan` §15.4): engines min 30–50 / max 150–450, volume mostly
0.25; horns 30–50 / 150–350; doors and bells 50 / 300–350; move sounds 50 / 300–450 at volume 0.5. With the camera
30–310 units above the ground, a car engine (max 150) is heard only when zoomed in; trains and metro (max 450) from
further out.

### 9.3 Panning and Doppler

* Panning is FMOD Ex's software 3D panner: the direction of the source in listener space is mapped onto the speaker
  layout (stereo: left/right only, so front and back sound the same; surround: across all speakers). Exact pan law
  *unverified*. No HRTF, no spread, pan level 1.
* Doppler is active: source velocity = `parent.velocityAtGlobalPos(pos) + sound.velocity`. For vehicle sounds the
  engine sets `sound.velocity = orient · (0, 0, −3 · speed)` each tick (L:0x081adca0.c:773-786) (vehicle speed up to
  about 12 → 36 units/s → up to ±10 % pitch). Listener velocity comes from the tracked camera's
  `velocityAtGlobalPos` (gs3DElement passes it to its parent; for the camera it is presumably zero, *unverified*).

### 9.4 Head-relative sounds

`relative = true` → `FMOD_3D_HEADRELATIVE`: the position is in listener space and `distance()` = `|localPos|`. All
relative sounds in the game sit at the origin (UI sounds are children of `$desktop`, a 2D control, so their global
position is `(0,0,0)`; forced vehicle sounds are set to `gsZeroVector`), i.e. distance 0 → full volume, centred.

---

## 10. Volumes and settings

| Level | Who sets it | Effect |
|---|---|---|
| Master | `set_audio_volume($settings.$volume)` at start (`init.script:31`) and on Audio-menu OK (`audio.script:135`) | `SoundGroup("default").setVolume` — every sound, music included |
| Per object `volume` | scripts | music `$musicVolume` (in-game era music only), ambience 0.2, hover 0.1, click 0.5 (`ui.script:22`) or 1.0, breakdown 0.6, passenger 0.6, templates as §9.2 |
| Per object `volumeMult` | engine (vehicle engine/move sounds) | §11.1 |
| `gsListener.volume` | `window.script:90` | none (never read) |

Settings keys (`MAIN.$settings`, file `userdata/settings.script`): `$sounds` (bool, default true), `$music` (bool,
true), `$audiodevice` (string, "Default"), `$audiospeakers` (string, "Default"), `$volume` (0..1, 0.8 in the defaults
file, 1.0 as script fallback), `$musicVolume` (0..1, 0.5 in the defaults file, 0.3 as script fallback). The port reads
the same variables (`port/src/engine/settings.cpp`, `settingNumber(vm, u"volume", 1.0f)` etc.).

Quirks (*obs* in scripts):
* The **menu music ignores `$music` and `$musicVolume`**: `mainmenu.script:35-39` creates it at volume 1.0 and
  `resetMenu` (`:165`) always plays it; the same for the ending music (`ending.script:5-9, 73`). Only the master
  volume affects it.
* Ambience ignores `$music`/`$musicVolume`; it always plays at 0.2 × master.
* `$musicVolume` changes take effect when the game resumes (`ambience.resumeGame`, `ambience.script:91-108`).

---

## 11. Engine-driven world sounds

All of these clone a template sound with `createInstance(grid, autoFree = false)` (vtable +300; W:0x10097af0,
L:0x08127280) under the grid, so they tick with the grid (§14).

### 11.1 Vehicles (`gsGridVehicle::move` L:0x081adca0, W:0x1010de50; see `docs/simulation/grid-traffic.md` §6)

`gsGridVehicle::playSound(s, loop, pitch)` (L:0x081adaa0, W:0x10107a50):

```
if s and (bucket.eyeDist < s.maxDistance or controller.forceSounds):
    c = s.createInstance(grid, false); push c onto the vehicle's sound list (+0x34, linked via +0x14c)
    if forceSounds: c.relative = true; c.position = 0
    else:           c.relative = false; c.globalPos = vehicle pose position (calculatePose)
    c.pitchMult = pitch; c.setPlaying(true, loop)
```

`bucket.eyeDist` = min distance from any grid camera's eye to the bucket centre + 25 up (grid-traffic §2), so a
secondary camera also activates sounds near it (they are still attenuated from the main listener).

Each vehicle tick (L:0x081adca0.c:470-800):

| Sound | Rule |
|---|---|
| horn `hornSound` | if `waitTime > 5` s and in range (or forced) and `rand(0, 50) < dt`: one-shot, pitch `rand(0.8, 1.0)` (:470-483) |
| engine `engineSound` | loop at slot +0x38 while in range (`eyeDist < maxDistance`, not hidden by the underground view `viewMode == 0x180` unless on a tunnel node, or forced); `volumeMult = clamp(accelSmoothed, 0.4, 1.0)`, `pitchMult = clamp(speed · 0.2, 0.4, 1.0)`; out of range: `setPlaying(false)` and dropped (:576-600) |
| move `moveSound` / `moveTunnelSound` (only without `engineSound`) | loop at slot +0x38; tunnel variant while the current node has flag 0x800000 (swapped when the flag changes, vehicle flag 0x100 remembers which one plays); `volumeMult = min(speed · 0.2, 1)`, `pitchMult = min(speed · 0.1 + 0.5, 1)` (:487-560) |
| aircraft/rotor path | vehicles with animated parts use a spin-up value `r` (0..12) instead: `volumeMult = clamp(accelSmoothed, 0.4, min(1, 0.2 r))`, `pitchMult = clamp(0.1 r, 0.4, 1)` (:616-697, *partly traced*) |
| sound list upkeep | every tick: finished sounds (not playing, not the engine slot) are freed; the others get the vehicle position and `velocity = orient · (0, 0, −3 · speed)` (relative/zero when forced) (:756-790) |

Transport vehicles (`gsTransportVehicle::move` L:0x081bd7f0, W:0x101152a0; `docs/simulation/transport.md` §4):
stop announcement = the **stop item's template `bellSound`** when < 32 path steps are left (subway stations:
"7. Subway announcement1.wav"), the vehicle's `bellSound` (tram bell, metro horn) when < 8 are left, `doorOpenSound`
when the doors start opening, `doorCloseSound` when they close. All one-shots, pitch 1.

`gsGridVehicleController.playSound(s)` (W:0x10107b60) is used once: `linepanel.script:371` plays `$passengerSound`
(min 50 / max 300 / volume 0.6) at a vehicle when a passenger pays a ticket.

### 11.2 Characters (`gsGridCharacter::move` L:0x08153b90, W:0x100c9470)

While idle: if the model has `hornSound` (vanilla: only the cow and pig, "Cow moan-2.wav", "Pig moan-1.wav") and
it is in range (or forced) and `rand(0, 5) < dt`: one-shot at the character, pitch `rand(0.8, 1.0)`
(`gsGridCharacter::playSound` L:0x081548f0, W:0x1009a9f0; :240-270). Pedestrian voices exist only as query sounds
(§13).

### 11.3 Items (buildings, props; `gsGridItem::move` L:0x08182b10, W:0x100eb5a0; grid-traffic §5.1)

One sound slot per item (+0x70), only while the item's bucket is active:

* Door items (flag 0x1000): while a door opens, start `moveSound` as a loop at the item centre (level crossing bell,
  `rail-props`); when the door closes, or when the item stops animating (`doorOpenLen == 0`, or `eyeDist >= 300` with
  flag 0x4000000), free it. "Opens" means a train holds one of the crossing's track nodes (the barriers go down), so
  the bell rings from the moment the train's claims reach the crossing until its last car has left it
  (grid-traffic §5.1.1). Port: `engine/sim_sounds.cpp` `tickDoors`, from the door state of `engine/sim_doors.cpp`.
* Then, if the slot is empty, the template has `engineSound` and `eyeDist < engineSound.maxDistance`: start it as a
  loop at the item centre (church bells, market stalls, fair rides, cranes, fire, tennis; 15 vanilla templates).
* When the bucket becomes inactive, every item sound is stopped and freed (grid-traffic §3).

---

## 12. Ambience and music

Both live in `metro/scripts/ambience.script` (loaded by `game.script:44`, `MAIN.$ambience`), on `$desktop`
(real-time, not part of the scene).

### 12.1 City ambience

Seven `new_music($desktop)` loops loaded 2D and relative (`:12-45`), indexed by area type:

| Index | File (`/metro/sounds/`) | When |
|---|---|---|
| 0 | `6. Suburban ambience1 (close ).mp3` | urban (paved/covered cells) |
| 1 | `6. Countryside ambience1.mp3` | open ground |
| 2 | `6. Water ambience4 (echo).mp3` | water |
| 3 | `6. Industrial ambience1.mp3` | items with `ambience = 3` |
| 4 | `6. Harbour ambience1.mp3` | items with `ambience = 4` |
| 5 | `6. Suburban ambience1 (distant ).mp3` | type 0 and `cameraZoom > 200` |
| 6 | `6. Underground ambience1.mp3` | `grid.viewMode == GRID_VIEW_UNDERGROUND` |

Thread `"game"` (`:137-180`): every 1.03 s (game-thread time) it samples
`getAreaType(cameraX − sin(a)·30, cameraZ + cos(a)·30, cameraZoom·0.05 + 100)` with `a = cameraDir · π/4`. On a change
it clones the new loop, plays it once (`play(false)`) and cross-fades over 1 s (`elapsed += skip_frame`) at
`$ambienceVolume = 0.2`. Since the clips are one-shots, it keeps the loop going by cross-fading into a fresh clone
when `currentPos > length − 2.0`. Tokyo replaces types 0 and 5 (`addons/tokyo/scripts/game.script:122-123`, loaded
with the default 3D mode).

`gsGrid.getAreaType(x, z, diameter)` (L:0x0813ca60, W:0x100b5d40), *obs*:

```
r = round(0.5·diameter / cellSize); (cx, cz) = cell of (x, z)
count[0..4] = 0
for every 4th cell (step 4 in x and z) inside the circle of radius r:
    flags & 0x2000 (water) → count[2]++ ; flags & 0xf08 (paved/covered) → count[0]++ ; else count[1]++
for every bucket overlapping the square, every item in it with template.ambience t != −1:
    count[t] += item.w · item.l · (t == 4 ? 8 : 2)
type = count[1] > count[0] ? 1 : 0; if count[2] > max: 2; if count[3] > max: 3; if count[4] > max: 4
return type (int)
```

### 12.2 Music

* Main menu: `Cim-music-Menu.mp3`, loop, volume 1.0 (`mainmenu.script:35-39, 165`). Ending screen:
  `Cim-music-Menu (Grunge).mp3`, loop, volume 1.0 (`ending.script`). Both 2D, relative.
* In game ("playlist" = the `$musicFileNames` array, `ambience.script:47-52`; no playlist files exist): one track per
  era chosen from `MAIN.$mainClock.years` (`< 1945` Era1, `< 1970` Era2, `< 1995` Era3, else Era4), played as a loop
  at `$musicVolume` when `$settings.$music`. On `onYear` with a new era (`:110-135`): fade out over 0.5 s, delete,
  load the next file, fade in over 0.5 s (sequential, one music voice at a time).
* DLC overrides (`addons/tokyo/scripts/game.script:124-133`, `addons/usa/scripts/game.script:131-141`): replace
  `$musicFileNames` with their own era3/era4 files and delete the current music so `resumeGame` reloads it.
* `pauseGame`/`resumeGame` (`:80-108`) pause/resume ambience and music and apply `$music`/`$musicVolume` changes.

---

## 13. Script call sites by purpose

All UI sounds are `new_sound($desktop)` or `new_music($desktop)`, `load(…, false)`, `relative = true`, volume 1.0
unless stated.

| Purpose | Object (file:line of creation) | File (`/metro/sounds/` unless stated) | Played at |
|---|---|---|---|
| Menu hover | `$hoverSound` vol 0.1 (`mainmenu.script:29`, `pausemenu.script:33`; `ui.script:26` created, never played) | `2. Mouse cursor hovering3 (short).wav` | `mainmenu.script:127`, `pausemenu.script:103` (onMouseOver of menu buttons) |
| UI click | `$clickSound` (`ui.script:20` vol 0.5; also in main/pause menu, unused there) | `2. GUI menu button click1.wav` | `ui.script:249` (button), `:336` (choice button), `linepanel/vehicles.script:40`, `petitionpanel.script:63`, `scenariopanel.script:245` |
| Error | `$errorSound` (`ui.script:14`, menus) | `2. Cannot do that1.wav` | `ui.script:747` (error dialog), `scripts/ui.script:451`, `hoverinfo.script:449` (tool cannot apply) |
| Construction | `$constructSound`/`2`, `$roadStartSound`, `$roadEndSound`, `$addStopSound`, `$demolishSound`/`2` (`hoverinfo.script:47-88`) | `1. Player structure construction_2.wav`, `1. Defining line points (beginning).wav`, `1. Tracklaying2 (short end1).wav`, `2. GUI menu button click6.wav`, `1. Player structure construction_demolition_1.wav`, `1. Player structrure demolition4 (Large building).wav` | `hoverinfo.script:373, 530-532, 549, 564, 580-582`, `scripts/ui.script:454` |
| Purchase | `$purchaseSound` (`vehiclepanel.script:149`), `$startCampaignSound` (`advertising.script:166`), `$acceptLoanSound` (`banks.script:122`; `hqpanel.script:78` = missing file §5.2) | `2. Purchase sound3.wav` (`… sound1.wav` missing) | `vehiclepanel.script:200, 232`, `advertising.script:190, 226`, `banks.script:141, 165`, `hqpanel.script:208-335` |
| Achievements | `$achievementSound` (`achievement.script:75`) | `2. Retition appearance cling1.wav` | `achievement.script:198` |
| Scenario results | `$taskCompletionSound`, `$scenarioCompletionSound`, `$scenarioFailureSound` (`scenario.script:11-27`) | `2. Scenario objective completion 2.wav`, `2. Scenario completion 2 (short).wav`, `2. Scenario failure 2.wav` | `scenario.script:136, 206, 211` |
| News/ticker | `$petitionSound`, `$crashSound`, `$breakSound` (vol 0.6), `$newsSound`, `$warningSound` (`petitionbar.script:706-735`) | `2. Retition appearance cling1.wav`, `2. Warning appears 2.wav` (crash and warning), `2. Breakdown appearance 4.wav`, `2. Newspaper appears 1.wav` | `petitionbar.script:863-866` (by ticker type), `:955` |
| Query (click on an object) | `$querySound`, `$carSound`, `$playerQuerySounds[5]` (`secondaryview.script:22-58`); per-template `$querySound` (238 objects) and `$querySounds[]` (168, citizens' voices) on `MAIN.$desktop`, **3D mode** (default `load`), relative | `3. Generic query sound3.wav`, `Car query sound.wav`, `… query sound (ei kuskia).wav` (car, tram, subway, water bus, helicopter), e.g. `3. Factory1.wav`, `Tourist male1.wav` | `secondaryview.script:726-756` (`setCameraTarget($target, $citizen, $sound)`) |
| Passenger fare | `$passengerSound` 3D, min 50, max 300, vol 0.6 (`linepanel.script:31`) | `2. Menu button click positive1 (long).wav` | `linepanel.script:371` via `vehicle.playSound` |
| Menu music | `$menuMusic` (`mainmenu.script:35`) | `/metro/music/Cim-music-Menu.mp3` | `mainmenu.script:165` (loop) |
| Ending music | `$menuMusic` (`ending.script:5`) | `/metro/music/Cim-music-Menu (Grunge).mp3` | `ending.script:73` (loop) |
| In-game music | `$music` (`ambience.script:59-66, 96-100, 121-126`) | `metro/music/Cim-music-Era1..4.mp3` | `ambience.script:106, 125, 138` (loop) |
| Ambience | `$ambiences[0..6]` + clones (`ambience.script:12-45, 150, 164`) | §12.1 | `ambience.script:102-103, 152, 166` |
| Vehicle/character/item sounds | template fields in the object scripts (vehicles: 88 engine, 16 move, 9 tunnel, 84 horn, 19 bell, 34 door pairs; stations: 6 announcement `bellSound`; items: 15 `engineSound`, 1 `moveSound`; animals: 2 `hornSound`) | `4. …` / `5. …` / named loops | engine (§11) |
| Listener | `MAIN.$listener` (`window.script:88`) | — | `engine.script:242-243, 301`, `ui.script:101`, `secondaryview.script:281, 297, 705, 773`, `cameratarget.script:58, 104` |
| forceSounds | — | — | `cameratarget.script:59, 89, 105`, `secondaryview.script:283, 299, 692, 771` |
| Init / options | — | — | `init.script:29-32`, `ui/menus/audio.script`, `main.script:449` |

---

## 14. Time, pause, focus and game speed

* **Who ticks.** A sound ticks as a child of its owner (`gsModule` tick tree, `docs/simulation/README.md` §Update
  tree). UI sounds, music and ambience are on `$desktop` (real time). Engine clones are children of the grid, which
  ticks inside the scene with `dt · timeSpeed` in up to `1 + 5·dt·timeSpeed` sub-steps. The listener (and FMOD
  `update`) is on `$window`, once per frame.
* **Game speed** does not change any pitch or playback rate. Vehicle engine pitch follows the vehicle's speed in units
  per (scaled) second, which does not depend on the game speed. At speed 0 the grid still ticks with dt = 0: loops
  keep playing at their last pitch/volume, horns and voices stop (`rand < dt` never holds). The `"game"` thread group
  stops at speed 0 (`setRunSpeed("game", 0)`, `minimap.script:178`), so the ambience thread freezes: the current
  ambience clip plays to its end and then stops, because the re-loop cross-fade does not run (*inf*). At speed 4/10
  the ambience checks and fades run 4/10× faster (*inf*: `wait` and `skip_frame` are group time).
* **Pause menu / leaving the game** (`system.script:18-36` "Paused", `main.script:810-884`): `MAIN.$scene.enabled =
  false; audible = false` stops every scene sound through `statusUpdated` (loops restart from 0 when the scene becomes
  audible again); `ambience.pauseGame()` pauses ambience and music (resume from the saved position).
* **Losing focus / minimising** (`$stopWithoutFocus = true`, `system.script:9-16`): `onDeactivate` → `"deactivate"`
  → the Paused state, same as above. Desktop sounds (menu music in the main menu, UI) keep playing; FMOD mixes in its
  own thread, so they do not depend on the frame loop. In the Paused state `MAIN.waitMessages = true` (the main loop
  waits for OS messages), so FMOD `update` runs only when messages arrive (*inf*).

---

## 15. Data: archives and formats

### 15.1 Archives

| Archive | Mount | Audio entries | Unpacked | Archive size |
|---|---|---|---|---|
| `sounds.gs` | `metro/sounds/` | 227 (220 `.wav`, 7 `.mp3`) | 61 929 545 B (WAV 49 633 480, MP3 12 296 065) | 56 245 076 B |
| `music.gs` | `metro/music/` | 6 `.mp3` | 136 677 368 B | 116 434 884 B |
| `tokyo.gs` (DLC) | `addons/tokyo/` | 63 (59 `.wav` in `sounds/`, 2 ambience `.mp3`, `music/era3.mp3`, `music/era4.mp3`) | 7 435 154 B WAV + 52 848 002 B MP3 | — |
| `usa.gs` (DLC) | `addons/usa/` | 2 (`music/era3.mp3`, `music/era4.mp3`) | 54 799 898 B | — |

No other shipped archive contains audio (`regs-tool vfs <game> list`, filtered by extension; the hits in
`Schwebebahn`, `hovercabs`, `pmc` are community mods in the user's install). No `.music`, playlist, `.fsb`/`.fev`
(FMOD bank) or `.ogg` files exist.

### 15.2 Formats (headers parsed after `gstool.mjs extract` into a scratch folder; `regs-tool vfs cat` spot check of `2. GUI menu button click1.wav`: RIFF/WAVE, PCM, 1 ch, 44 100 Hz, 16 bit)

| Set | Format |
|---|---|
| 220 vanilla WAV + 59 Tokyo WAV | all **PCM (format 1), 16-bit, mono, 44 100 Hz**; chunks `fmt `+`data` (5 vanilla files also `LIST`); no `smpl` loop points (loops are whole-file). Durations 0.13–54 s, median 1.4 s, 563 s total (vanilla). |
| 7 vanilla ambience MP3 | MPEG-1 Layer III, 44 100 Hz, stereo/joint stereo, **256 kbit/s CBR**, 20.6–98.6 s |
| 6 vanilla music MP3 | MPEG-1 Layer III, 44 100 Hz, joint stereo (Menu: stereo), **320 kbit/s CBR**, ID3v2 tags (not Grunge); Era1 661.6 s, Era2 870.1 s, Era3 696.1 s, Era4 720.6 s, Menu 227.0 s, Menu (Grunge) 241.4 s; 3417 s total |
| Tokyo MP3 | 2 ambience 256 kbit/s 44.1 kHz stereo (116.7 s, 61.1 s); era3/era4 320 kbit/s 44.1 kHz (587.8 s, 592.1 s) |
| USA MP3 | era3 320 kbit/s **48 000 Hz** (575.4 s), era4 320 kbit/s 44.1 kHz (794.6 s) |

So all effects are mono (they can all be 3D-panned) and everything is 44.1 kHz except one 48 kHz DLC track.

### 15.3 References from scripts

1 920 literal `.wav`/`.mp3` loads in vanilla + DLC scripts, 287 distinct paths; 1 missing
(`2. Purchase sound1.wav`, §5.2). Every vanilla `metro/sounds` file is referenced; the four era tracks are referenced
through `$musicFileNames`.

### 15.4 Reproduction

Scripts used for this section live in the session scratchpad (not in the workspace): `soundscan.mjs` (catalog of
`with (x = new_sound|new_music(…)) { .load …; .volume …; … }` blocks), `audiofmt.mjs` (RIFF `fmt `/`data` and MP3 frame
walk), `refcheck.mjs` (literal paths vs. `regs-tool vfs list`). They are small and can be recreated from the
descriptions if needed.

---

## 16. Port plan

### 16.1 Minimal output and mixing features

| Feature | Needed to match the original | Notes |
|---|---|---|
| Output | one device, stereo default; device chosen by name from `$audiodevice` ("Default" = system default) | `audio_devices()` must return `"Default"` + device names |
| Voices | ≥ 64 mixed voices, ≥ 256 voice handles (drop or virtualise the quietest beyond 64) | typical load: UI 1–3, music 1, ambience 1–2, item loops a few, one engine/move loop per nearby vehicle + one-shots |
| Clip decode | WAV PCM16 mono 44.1 kHz (decode once); MP3 44.1/48 kHz stereo decoded while playing from the in-memory bytes | the original keeps MP3 compressed in memory; a port may also stream from the VFS |
| Resampling | variable-rate resampling per voice: `rate = fileRate · pitch · pitchMult · doppler` → device rate | pitch range seen: 0.4–1.0 (engines, move), 0.8–1.0 (horns), template pitch 0.90–0.97; 48 kHz USA track |
| Looping | whole-clip loop (no loop points) | |
| Start paused, unpause on next tick | yes (one-frame start latency; harmless to skip) | |
| 3D | one listener (camera eye, −Z forward, +Y up, right-handed); linear rolloff §9.2; head-relative mode; Doppler (scale 1, c = 340 units/s) | pan law: any constant-power azimuth pan matches FMOD closely enough for stereo |
| Volume groups | master only (`set_audio_volume`) | music/ambience/UI levels are per-object volumes set by scripts |
| Mono sources | all effects are mono | stereo sources (music, ambience) are 2D-loaded and relative at distance 0: play them unpanned in stereo (§17 Q1) |
| Timing | sounds tick with their owner (desktop real time vs. grid scaled time) | needed for the 0.5 s out-of-range stop and pause semantics |

The existing port layer `port/src/audio/audio.h` (miniaudio) has two groups `Effects`/`Music`. To match the
original, both must sit under one master gain and the Music group gain must stay 1.0 by default; `$musicVolume` is
applied by the scripts to the music object, not by a group. Its comment "every play is a voice of its own" holds for
FMOD channels, but the gsSound object stops its previous channel first (§6.3); the native must do the same.

### 16.2 Volume categories and settings keys

Keep the original keys: `$sounds`, `$music`, `$volume` (master), `$musicVolume` (script-applied), `$audiodevice`,
`$audiospeakers`. Optional port additions, named like the other port settings (`regs…`, `port/data/regs/`):
`$regsEffectsVolume`, `$regsAmbienceVolume` (would multiply the scripted 0.2), `$regsMenuMusicFollowsMusicVolume`
(fixes the §10 quirk; off by default to match the original).

### 16.3 Speaker modes the port could offer

The original's `$audiospeakers` strings stay valid. Because the original executable treats unknown strings as "use the
control panel layout" (§4.2), extra port values do not harm it.

| Option (`$audiospeakers`) | Port output | Mapping to the 3D model |
|---|---|---|
| `Default` | the device's native layout (port choice; the original most likely used stereo, §4.2) | as below for that layout |
| `Mono` | 1 channel | attenuation only, no pan; stereo clips downmixed |
| `Stereo` | 2 channels | azimuth θ of the source in listener space (x right, −z forward): constant-power pan `L = cos((θ'+π/2)/2)`, `R = sin(…)` with θ folded into [−π/2, π/2] (front/back alike, as FMOD's stereo); 2D clips pass through |
| `Quad`, `Surround` (5.0), `5p1`, `7p1` | 4 / 5 / 6 / 8 channels | pairwise constant-power (VBAP-style) panning between the two speakers around θ (FL −30°, FR +30°, C 0°, SL/SR ±110°, BL/BR ±150°; quad ±45°/±135°); LFE silent; 2D stereo clips to FL/FR |
| `Prologic` | 2 channels, matrix-encoded | pan as 5.0, then encode Lt/Rt (centre −3 dB to both, surrounds ±90° phase-shifted, −3 dB); low priority |
| new `Headphones` | 2 channels, binaural | same θ plus elevation φ: HRTF convolution (needs an HRTF set, e.g. a public-domain KEMAR set shipped with the port) or a data-free approximation (interaural time difference ≤ 0.66 ms, head-shadow low-pass on the far ear); distance attenuation unchanged |
| new `StereoWide` (optional) | 2 channels | stereo pan plus a rear cue (slight low-pass for sources behind) |

The menu needs the label strings `menu_audio<Name>_item` for new entries (`audio.script` reads
`$strings.getVariable("menu_audio"+current+"_item")`), added via the port's string overlay.

### 16.4 Natives checklist (priority order)

1. `enable_sounds`, `set_audio_volume`, `audio_devices`, `audio_speakers` (§2.1, §3) — output up, master volume,
   options menu filled.
2. `new_sound`, `new_music`, `gsSound.load` with the path-keyed cache and the 2D/3D/compressed mode (§5),
   `play(loop)`, `stop`, `volume`, `relative`, the enabled+audible gate and `statusUpdated` stop/restart (§6.3, §6.5)
   — UI sounds and menu music.
3. `pause` with resume position, `clone`, `currentPos`, `length` (filled on first play) — ambience loop and
   pause/resume of music.
4. `new_listener` with `tracked`/`offset` and the per-frame listener update (§8); `minDistance`/`maxDistance`,
   `pitch`, linear rolloff, range start/stop (§6.5, §9).
5. `gsGrid.getAreaType` (§12.1; the port currently returns 0) — city ambience by area.
6. Engine sounds: item ambient loops (§11.3), vehicle engine/move/tunnel loops with volume/pitch multipliers, horns,
   transport bells/announcements/doors, character voices, `forceSounds`, `gsGridVehicleController.playSound`
   (§11.1-11.2); sound lists freed when finished; bucket deactivation frees item sounds.
7. Doppler (source velocity `orient·(0,0,−3·speed)`), `disable_sounds`, `gsListener.volume` (store only),
   `tracked2`/`offset2` (store only).
8. Port extras: speaker modes beyond stereo, headphones, extra volume categories (§16.2-16.3).

---

## 17. Open questions

1. **2D clips under a 3D channel mode.** Every channel gets `setMode(FMOD_3D | …)`, also for sounds created
   `FMOD_2D` (music, ambience, UI). If FMOD Ex 4.32 switches such software channels to 3D, stereo music positioned at
   distance 0 might be collapsed to the centre; if it ignores the 2D→3D change, music keeps its stereo image. Needs a
   check against FMOD Ex behaviour (documentation or a recording of the original). Recommendation: keep 2D clips 2D.
2. **"Default" speaker mode** = FMOD's built-in default (stereo per FMOD Ex docs) or the control-panel layout? Only a
   run on a surround system (or the FMOD 4.32 docs) settles it.
3. **Value 7**: FMOD_SPEAKERMODE_PROLOGIC or SRS5_1_MATRIX in these FMOD versions.
4. FMOD Ex defaults the game relies on but never sets: software channel count (64?), mixer rate (48 kHz?), DSP buffer,
   resampler, output type, LFE behaviour, pan law, speed of sound.
5. Listener velocity: does the floating camera report a velocity through `velocityAtGlobalPos` (Doppler on camera
   pans), or is it zero?
6. The exact meaning of the vehicle "rotor" path (§11.1, aircraft/helicopters) and of `getDriverCaps`' two stored
   frequency floats (+0x14/+0x18).
7. Whether `gsMain::errorMsg` from failed loads is visible to the player (MAIN event 7 handler in the scripts) or only
   logged.

---

## 18. Port status (ReGS)

State of `port/` as of 2026-11-04: §16.4 steps 1–8 are done, with the deviations in §18.4.

| Part | Port files |
|---|---|
| Output, mixer, speaker layouts, capture | `port/src/audio/audio.{h,cpp}` (miniaudio's device layer and decoders, a mixer of its own) |
| Script natives: `enable_sounds` … `audio_speakers`, `gsSound`/`gsMusic`, `gsListener`, the sound cache, statusUpdated | `port/src/engine/sound.{h,cpp}` |
| Engine-driven world sounds (§11) and `gsGridVehicleController.playSound` | `port/src/engine/sim_sounds.{h,cpp}`, hooks in `sim.cpp` (`tickSimulation`, `stepVehicle`) |
| `gsGrid.getAreaType` (§12.1) | `port/src/engine/grid.cpp` |
| Label of the port's Headphones mode | `port/data/regs/extend/metro/ui/menus/audio.script`, `port/data/regs/strings/{en_US,de}.strings` |

### 18.1 What runs

| §16.4 step | State |
|---|---|
| 1 Output, master volume | `enable_sounds(device, speakers)` opens the device once (by name; "Default" or an unknown name = the system default) with the chosen layout; `set_audio_volume` is one master gain after the mix; `audio_devices()` = `"Default"` + the playback device names (a temporary context, works before init); `audio_speakers()` = the original's eight values + `"Headphones"`. A failed init leaves the game silent, as in the original. |
| 2 gsSound / gsMusic | path-keyed cache (the first load fixes 2D/3D and compressed mode; failed loads are not cached and are retried), `load` (gsSound resets `length`, gsMusic does not), `play(loop)` with the restart and "a live loop is kept" rules, `stop`, `pause` (position kept, loop cleared), `clone` (copies sound, relative, distances, pitch, volume, length), live setters for `volume`, `pitch`, `relative`, `minDistance`, `maxDistance`, `currentPos` (channel position, 0 without a channel), `length` (filled on the first activation). One channel per object. The enabled + audible gate along the owner chain, and statusUpdated (loops restart when their module becomes enabled + audible again, other sounds stop). |
| 3 Listener | `new_listener` makes the last created listener current; once per frame it takes the tracked camera's eye and orientation as the 3D pass draws them; range start (`distance < maxDistance`) and the 0.5 s out-of-range stop in the sound's own tick (desktop sounds in real time, grid clones in game time). |
| 4 getAreaType | as §12.1, with the item weights (`w·l·2`, harbour `·8`) and the tie rules of L:0x0813ca60. |
| 5 World sounds | vehicle engine loops (`volumeMult = clamp(accelSmoothed, 0.4, 1)`, `pitchMult = clamp(0.2·speed, 0.4, 1)`, hidden in the underground view off tunnel nodes), move / tunnel loops (swapped on node flag 0x800000), the rotor variant for helicopters and aircraft, horns (`waitTime > 5`, `rand(0, 50) < dt`, pitch 0.8–1.0), the sound list (one-shots follow their vehicle), stop announcements (< 32 nodes left, the stop template's `bellSound` at the stop), vehicle bells (< 8), doors (open at stopTime 0, close when the vehicle leaves the docked slot), animal voices (idle, `rand(0, 5) < dt`), item ambient loops (template `engineSound` at the item centre), the level-crossing bell (door items' `moveSound` while a train holds the crossing, §11.3), `forceSounds` (head-relative at the listener), `gsGridVehicleController.playSound` (the passenger fare). Agents farther from the listener than every sound range of their model cost one distance check per tick. |
| 6 Time, pause, focus, speed | as §14: game speed changes no pitch; at speed 0 the grid ticks with dt = 0 (loops keep their last pitch and volume, horns and voices stop); the pause menu (scene disabled and inaudible) stops every world voice, loops come back on resume; `ambience.pauseGame` / `resumeGame` pause and resume ambience and music at their position. Focus loss goes through the same Paused state (scripted `--at` runs ignore focus, so it was not run). |
| 7 Doppler and the rest | source velocity `forward · 3·speed`, `disable_sounds`, `gsListener.volume`, `tracked2`, `offset2` stored only. |
| 8 Speaker modes | §18.3. |

### 18.2 Mixer

miniaudio's `ma_engine` is not used: its spatializer pans by dot products against every channel (front sources leak
into the surrounds, LFE gets the full signal, 2D stereo is spread by its channel converter), and it has no per-ear
delay or filter for a headphone mode. The port mixes in the device callback itself:

* **Voices.** WAV decoded once to f32 PCM; MP3 (music, ambience) decoded per voice from the in-memory file while it
  plays. Linear-interpolation resampling at `fileRate · pitch · Doppler / deviceRate` (FMOD Ex's default resampler is
  linear too). Whole-file loops.
* **3D.** Listener basis from the camera (forward, up made orthogonal, right = forward × up). Linear rolloff (§9.2),
  channel volume clamped to 0..1. Doppler `f' = f·(c − vL·u)/(c − vS·u)` with c = 340, clamped to 0.5–2; the
  listener velocity is 0 (§17 Q5). Head-relative voices take their position in listener space.
* **2D.** Clips loaded 2D are never attenuated or panned (§17 Q1: kept 2D): stereo to the front pair, mono to both
  front channels at −3 dB (mono layout: the average of a stereo clip, a mono clip as it is).
* **Voice cap.** 64 voices are mixed; every frame the voices are ranked by `distance gain · volume`, the rest (and
  every voice at gain 0) is virtual: it advances without being mixed. No hard limit on voice handles.
* **Gains** are recomputed once per frame and ramped over the next device period.
* One master gain after the mix (`set_audio_volume`); no groups (the layer's former `Effects`/`Music` groups are
  gone, §16.1).

### 18.3 Speaker modes

`$audiospeakers` picks the device's channel count and map; 3D voices are panned over it, 2D stereo goes to the front
pair, LFE stays silent.

| Value | Device channels | 3D pan |
|---|---|---|
| `Default` | the device's own layout and map (port choice, §17 Q2) | by that map, as below |
| `Mono` | 1 | attenuation only |
| `Stereo` | 2 (FL FR) | constant power by the lateral component `x/d`: `L = cos((asin(x/d) + π/2)/2)`, front and back alike |
| `Quad` | 4 (FL FR BL BR) | pairwise constant power (2D VBAP) between the two speakers around the azimuth; ±45°, ±135° |
| `Surround` | 5 (FL FR FC BL BR) | VBAP; 0°, ±30°, ±110° |
| `5p1` | 6 (FL FR FC LFE SL SR) | VBAP; sides ±110° |
| `7p1` | 8 (FL FR FC LFE BL BR SL SR) | VBAP; sides ±90°, backs ±150° |
| `Prologic` | 2 | panned on a 5.0 bus, then a passive Lt/Rt matrix: centre −3 dB to both, the surrounds summed (−3 dB) and fed in antiphase; the ±90° phase shift of a real encoder is left out |
| `Headphones` (new) | 2 | data-free binaural: interaural time difference by Woodworth's formula (head radius 8.75 cm, at most 0.66 ms, a fractional delay line on the far ear), a level difference (`√(0.5·(1 ∓ 0.6·x/d))`), a head-shadow one-pole low-pass on the far ear (18 kHz in front down to 1.8 kHz at 90°) and a slight low-pass on both ears for sources behind (down to 6.3 kHz). No HRTF data. |

With 3+ speakers a source more than 45° above or below the listener's horizontal plane fades from the VBAP pair to
an even spread over all speakers (straight below the camera: everywhere). The original's executable treats unknown
`$audiospeakers` strings as "use the control-panel layout" (§4.2), so `Headphones` does not hurt it. A changed device
or speaker mode applies at the next start, as in the original (`$requireRestart`). The menu label
`menu_audioHeadphones_item` comes from the port's string overlay ("Headphones (binaural stereo)",
"Kopfhörer (binaurales Stereo)").

### 18.4 Deviations

* **Engine clones are native voices.** The original clones the template gsSound under the grid for every play
  (`createInstance`); the port plays the template's sound, volume, pitch and distances as a world voice and keeps
  the agent's slots (engine loop, sound list) in `sim_sounds.cpp`. Scripts never saw those clones, so nothing visible
  changes. They stop when the grid is disabled or inaudible (the clones' statusUpdated).
* **Range** is the distance from the listener (the main camera's eye) to the agent, not the bucket's `eyeDist`
  (minimum over the grid's cameras to the bucket centre + 25 up): a secondary camera does not activate sounds near
  it. Item loops stop after 0.5 s beyond `maxDistance`, or at once beyond `maxDistance + 100` (the stand-in for the
  bucket deactivation).
* **No one-tick paused start** (§6.3): voices start at once.
* **pause() keeps the paused voice** and the next `play` resumes it; a resume from another position seeks the MP3
  decoder on the main thread (slow for a long track; the scripts never do it).
* **Positions of script sounds:** the sound's `position` plus the `globalPos` (or `position`) of its 3D-element
  owners, orientation ignored. Every shipped script-played sound is head-relative or on a 2D control (§9.4).
* **Listener:** the camera's eye as drawn (eased), velocity 0.
* **Vehicle details:** `accelSmoothed` is updated once per grid tick with the tick's scaled time (the original: per
  sub-step); horns and animal voices roll once per tick with the tick's dt and use the port's own random stream (the
  simulation's `rand()` sequence stays untouched); the "< 32 / < 8 left" counts are track nodes to the leg end, not
  `pathStepsLeft + ringCount`. The rotor path uses the spin-up `r` of the rotor animation (`Mover::rotorSpin`,
  L:0x081adca0.c:604-690: aircraft spin up by 1/s to 12, halve per second while halted, broken or crashing).
* **statusUpdated** is polled once per frame for sounds that play, loop or wait for a resume, instead of a callback.
* **Headless** (`regs-tool run`, tests): `enable_sounds` does nothing, as if FMOD's init failed (silent; every load
  gets no sound); `audio_devices()` is `["Default"]`.

### 18.5 Open

* Prologic with a real ±90° phase shift; HRTF-based headphones (would need a data set shipped with the port).
* The optional extra volume settings of §16.2 (`$regsEffectsVolume`, …).
* Device changes while running (unplugging, a new default device) are left to miniaudio's backend; not tested.
* Industrial and harbour ambience (types 3, 4) did not come up in the test runs (Vienna); the item weighting is in
  place.

### 18.6 Checks

Vienna save, `--documents run\documents-audio`, `REGS_DEBUG_AUDIO=1`, `REGS_AUDIO_CAPTURE=<file.wav>`, RMS from the
capture.

| Check | Result |
|---|---|
| Menu | `Cim-music-Menu.mp3` loops 2D at volume 1.0; hover (vol 0.1) and click (vol 0.5) sounds play on the menu buttons. |
| Master volume | menu music from its onset, `$volume` 0.4 vs 0.8: −6.0 dB in every 2 s window. The Audio page's OK applies a new volume at once (0.8 → 0.33 logged, the mix drops) and saves `$audiospeakers`; the next start opens that layout. |
| In game | era music `Cim-music-Era4.mp3` at `$musicVolume` 0.5; city ambience by area after minimap jumps: urban 60 → suburban close, open 40 / water 18 → countryside, water 30 / open 28 → water, back to urban; UI clicks, the newspaper and achievement sounds, the purchase sound on buying a bus; passenger fares (`playSound`), tram bells, stop announcements, doors, church bells and market stalls as item loops. |
| Vehicles | with only engine sounds audible (`REGS_AUDIO_ONLY=ngine`): RMS 0.015 at zoom 150, 0.045 at zoom 75 (+9.5 dB); idle engines at rate 0.367 (= 0.4 · 44.1/48 kHz), approaching ones up to 0.96 (Doppler). The logged pans follow listener space (x −66 → L 0.042 / R 0.024; x +54 → L 0.051 / R 0.094). |
| Panning in the capture | church bells only, stereo: two churches left and right, L/R +0.4 dB; after a 180° camera turn one church on the left: L/R +5.3 dB, matching that voice's gains 0.068 / 0.037. In 5.1 after a 90° turn the church on the right: FR −38 dB, SR −42 dB, FL/SL/FC −49 dB, LFE silent. |
| Pause | pause menu: every world voice stops ("grid disabled or inaudible"), ambience and music pause, the mix is 0.0000 RMS on both channels while the menu is open; resuming continues ambience and music at their position and restarts the loops. Speed 0: 31 voices keep playing. |
| Speaker modes | captures have 1 / 2 / 4 / 6 / 8 / 2 / 2 channels for Mono / Default / Quad / 5p1 / 7p1 / Prologic / Headphones; the menu music sits on FL/FR only. `regs-tool sound … at=x,y,z layout=…` places a test voice: stereo centre 0.707/0.707, hard left 1/0; quad at (−14, 0, 14) → BL only; 5.0 at +90° → FR 0.38, BR 0.92; headphones at +90° → L 0.064, R 0.142 RMS. |
| Level crossing | Vienna, camera on crossing item 7776 (`REGS_DEBUG_CAMERA`), `REGS_AUDIO_ONLY=crossing`: "Level crossing warning bell-1.wav" starts as a loop (vol 0.5, min 50 / max 300) when a train's claims reach the crossing and stops ("door closes") when its last car has left; four rings in 136 s of capture (`port/run/barriers/bell-f.wav`): RMS 0.014–0.020 at 51–58 s, 70–87 s, 95–113 s, 115–133 s, 0.0000 in between, matching the barrier frames. |
| Regression | `regs-tool script tests\vm_basics.script` passes; `regs-tool run` stays silent ("headless, sounds stay off") and exits 0. |
