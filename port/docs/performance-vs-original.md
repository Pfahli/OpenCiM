# OpenCiM against the original (2026-11-19)

The owner's own save (Cim2, 87,000 items) and settings (MSAA 4, anisotropy 8, shadows 2, bloom off), 2560x1440
windowed, vsync off, on the owner's PC (Core i5-11600K, RTX 3080, 4K screen at 59 Hz). The port with its own extras
off (no day/night cycle, no weather mod, no autosave); the original with its FPS counter (`$fps = true`). Speed 1 and the
fastest speed (key 3).

| | Speed 1 | Fastest speed |
|---|---|---|
| Original (1.0.22) | 60 fps, steady (held at the screen's 60 Hz) | 47-70 fps, about 60 on average, swinging |
| OpenCiM, Vulkan | 213 fps (worst 1 % of frames 7 ms, slowest 15 ms) | 172 fps (worst 1 % 14 ms, slowest 33 ms) |
| OpenCiM, OpenGL 3.3 | 171 fps (worst 1 % 9 ms, slowest 16 ms) | 140 fps (worst 1 % 16 ms, slowest 35 ms) |

Port numbers: `REGS_BENCH` over 2,000 frames after loading, one run each; the in-game date moved from 30 May to
16 June over the fast run, so the speed-up was on. Original numbers: its FPS counter read every 5 s for a minute; it
shows a running average, so short stalls do not show in it, and its frame times could not be measured. At speed 1 the
original was held at 60 fps although vsync was off, so its headroom there is unknown; at the fastest speed it falls
below that and swings, where the port keeps 140-170 fps.

Where the port's time goes at speed 10: `docs/performance-audit.md` (citizens, pedestrian search, traffic).

## Other platforms: how hard

The engine is portable C++ (about 79,000 lines): the script VM, simulation, file system, audio (miniaudio) and both
renderers (Vulkan, OpenGL 3.3 core) have no Windows code. What is Windows-only is small and kept apart:
`platform/window_win32.cpp` (window, input, ~460 lines), `platform/gamepad_win32.cpp`, `steam_hid_win32.cpp` and
`touchpad_win32.cpp` (controllers, touchpad), `engine/platform_win32.cpp`, the game search in `core/game_find.cpp`, the
crash handler and profiler under `tools/`, and the setup program. Players need the game's data from their own copy on
every platform; Cities in Motion was also sold for Linux and macOS (Steam depots 206722-5 and 73015), whose archives
the port reads the same way.

* **Linux: the easiest, about one to two weeks.** A window and input backend (SDL2 covers window, keyboard, mouse,
  controllers and touchpads in one), Steam library and Proton/native game search, case-sensitive file names for loose
  files (the archives are not affected), a crash handler on signals. Vulkan and OpenGL run as they are. Building with
  GCC/Clang will turn up some MSVC-only code. A Steam Deck build follows from it.
* **macOS on Apple Silicon (ARM): about two to four weeks.** The same SDL backend plus an ARM64 build; Vulkan through
  MoltenVK (Metal underneath), which lacks some features the port uses optionally (ray queries for the raytraced
  reflections, possibly parts of FSR 3), so those stay off there; OpenGL works but Apple froze it at 4.1. Then an app
  bundle, code signing and notarization, which need an Apple developer account (99 USD a year).
* **Android: the hardest, several months.** Vulkan works on most phones and tablets, but OpenGL 3.3 does not exist
  there (OpenGL ES would need its own backend, or Vulkan only). The game's interface is made for a mouse; the port
  already has touch gestures for the camera, but menus, construction tools and small buttons need a touch layout. The
  game data has to be copied from the player's PC install onto the device (it cannot be shipped), app lifecycle (pause,
  resume, losing the surface), memory limits and mobile GPUs with 100,000 items per map all need work.
