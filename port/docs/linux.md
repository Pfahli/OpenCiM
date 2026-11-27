# OpenCiM on Linux

OpenCiM runs natively on Linux (x86-64) since version 0.2.1: the same engine, renderers (Vulkan and OpenGL 3.3) and
game as on Windows, with SDL3 for the window, keyboard, mouse and controllers. It still needs your own copy of Cities
in Motion; the game's files are read from its install folder and never changed.

## Requirements

- x86-64 Linux with glibc 2.35 or newer: Ubuntu 22.04+, Debian 12+, Fedora 36+, Linux Mint 21+, Arch, SteamOS 3
  (Steam Deck), and similar
- A Vulkan driver (Mesa RADV/ANV, NVIDIA's driver) or OpenGL 3.3
- libcurl 4 (installed almost everywhere; only for the OpenStreetMap import and the SteamCMD download)
- X11 or Wayland, and PipeWire, PulseAudio or ALSA for sound
- Cities in Motion from Steam. Install it once with the Steam client for Linux: Steam runs the Windows game under
  Proton, but its files land in the normal `steamapps/common/Cities In Motion` folder, which OpenCiM uses.

## Install

1. Download `OpenCiM-<version>-linux-x64.tar.gz` from the
   [latest release](https://github.com/Pfahli/OpenCiM/releases/latest).
2. Unpack it where you want to keep it:
   ```sh
   tar -xzf OpenCiM-<version>-linux-x64.tar.gz -C ~/Games
   ```
3. Optional: add OpenCiM to the application menu and the `opencim` command (for your user only, no root):
   ```sh
   ~/Games/OpenCiM-<version>/install.sh
   ```
   `install.sh --uninstall` removes both again. Run `install.sh` again after moving the folder.
4. Start it from the menu, with `opencim`, or with `~/Games/OpenCiM-<version>/regs`.

To update, unpack the new version and run its `install.sh`. Your saves and settings are not in the program folder.

## Finding the game

OpenCiM looks for Cities in Motion in the Steam libraries of the native Steam client (`~/.steam/steam`,
`~/.local/share/Steam`), of the Flatpak (`~/.var/app/com.valvesoftware.Steam`) and of the Snap, including the extra
library folders listed in `libraryfolders.vdf` (other drives, SD cards). Elsewhere, give the folder once:

```sh
./regs "/path/to/Cities In Motion"
```

or write it into `install.cfg` next to `regs` (a line `game=/path/to/Cities In Motion`). `regs --print-paths` shows
which folders OpenCiM uses.

Without the Steam client, `./regs --download-game` fetches your own copy with Valve's SteamCMD (the account must own
the game) into `~/.local/share/OpenCiM/Cities In Motion`. This path is not tested on Linux yet.

## Your files

- Saves, maps, settings, mods and logs: `~/Documents/OpenCiM` (the XDG documents folder, e.g. `~/Dokumente/OpenCiM`
  on a German desktop)
- Saves and maps of the original game: OpenCiM lists them with a note and copies one in when you open it. On Linux
  they are in the Proton prefix of the game
  (`<library>/steamapps/compatdata/73010/pfx/drive_c/users/steamuser/Documents/Cities In Motion`), which OpenCiM finds
  by itself.
- Crash reports: `~/Documents/OpenCiM/crashes/crash-<date>.txt`. Please attach the file to a bug report.

Saves are the same files on Windows and Linux, so you can move them between the two.

## Steam Deck

Unpack OpenCiM in Desktop Mode (for example to `~/Games`) and run `install.sh`. For Game Mode, add `regs` in Steam
with *Add a Non-Steam Game*: the controller works with the game's own controller support (stick cursor, button
glyphs for the Deck), and the touchscreen works as a mouse. Steam Input should be set to *Gamepad* for the shortcut.

## Differences from Windows

- No Windows setup program: the tar.gz is the release. Updating means unpacking the new one.
- No glasses-free 3D on Leia SR displays (the Leia SR runtime exists only for Windows). The other stereo 3D modes
  are there, and head tracking through opentrack (UDP).
- Controllers through SDL3: Xbox, PlayStation, Switch, Steam Deck, Steam Controller and Steam Input's virtual pads.
  The Windows-only raw HID paths for Sony and Valve pads are not needed.
- Crash reports are text files with the stack (no minidump). `regs-tool symbolize <report>` names the stack lines
  with the release's symbols (`OpenCiM-<version>-linux-x64-symbols.tar.gz`, needs `addr2line` from binutils).
- HDR output and AMD FSR 3 frame generation depend on the Vulkan driver and the compositor; they are untested on Linux.

## Building

The Linux build is made from Windows in Docker (`linux/Dockerfile`: Ubuntu 22.04 with GCC 13), which keeps the
binaries compatible with older distributions:

```powershell
cd port
.\build.ps1                # the Windows build first: the controller glyphs' letters are drawn with Windows fonts
.\build-linux.ps1          # build/linux-Release (needs Docker Desktop running)
.\make-release-linux.ps1   # build/release/OpenCiM-<version>-linux-x64.tar.gz, .sha256 and the symbols
```

On Linux itself, with GCC 13 or Clang 16 and the development packages from `linux/Dockerfile`:

```sh
cd port
cmake -S . -B build/linux-Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-Release
```

A Linux-only build has no controller glyph pictures (`data/regs/ui/pad`) unless `-DREGS_PAD_GLYPHS_DIR=` points at a
Windows build's copy.

### What is different in the code

- `src/platform/window_sdl.cpp` and `gamepad_sdl.cpp` replace `window_win32.cpp`, `gamepad_win32.cpp`,
  `steam_hid_win32.cpp` and `touchpad_win32.cpp`. Keys still reach the engine as Windows virtual-key codes.
- `src/engine/platform_posix.cpp` (shared with macOS) replaces `platform_win32.cpp`: libcurl for HTTPS, stb_image for PNG and JPEG, SDL
  for display modes and the user's folders.
- `src/tools/crash_posix.cpp` (shared with macOS) replaces `crash_win32.cpp`; there is no sampling profiler (`REGS_PROFILE`) on Linux.
- The Vulkan device loads `libvulkan.so.1` and lets the window make its surface (`Window::vulkanSurface`).
- Paths: `core/file.h` (`openFile`, `seekFile`, `tellFile`) and the path helpers in `core/text.h` and
  `core/game_find.h`; `wchar_t` is 4 bytes on Linux, so a path's `wstring()` is never taken as UTF-16.
- Third-party: SDL 3.4 (`third_party/SDL3`, zlib licence) and stb_image (`third_party/stb`, public domain), both only
  in the Linux build. The FidelityFX SDK gets `src/render/ffx_compat.h` force-included and a larger context size
  (`FFX_SDK_DEFAULT_CONTEXT_SIZE`, made overridable in `ffx_types.h`).
