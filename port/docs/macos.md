# OpenCiM on macOS

OpenCiM runs natively on macOS 12 or newer, on Apple Silicon and Intel Macs (one universal app). It is the same engine
and game as on Windows and Linux, with SDL3 for the window, keyboard, mouse, trackpad and controllers, Vulkan through
MoltenVK (Vulkan on Metal, shipped inside the app) and OpenGL 4.1 as the fallback. It needs your own copy of Cities in
Motion; the game's files are read from its install and never changed.

## Install

1. Download `OpenCiM-<version>-macos.dmg` from the [latest release](https://github.com/Pfahli/OpenCiM/releases/latest).
2. Open it and drag **OpenCiM** into **Applications**.
3. Start OpenCiM from Applications or Launchpad.

Once the signing secrets are set up (see below), releases are signed with a Developer ID and notarized by Apple, so
macOS opens them without warnings. A build that is only signed ad hoc (your own build, or a workflow run without the signing secrets) is stopped the first time: open it
with right-click > Open, or allow it in System Settings > Privacy & Security.

## Finding the game

Install Cities in Motion once with the Steam client for Mac. OpenCiM looks in Steam's libraries
(`~/Library/Application Support/Steam` and the extra library folders in `libraryfolders.vdf`):

- the Mac version keeps its archives inside its app bundle (`Cities In Motion.app/Contents/MacOS`); OpenCiM looks there
  by itself
- the Windows version (for example fetched with `--download-game`, which uses SteamCMD) works the same way

Elsewhere, start OpenCiM once from Terminal with the folder:

```sh
/Applications/OpenCiM.app/Contents/MacOS/regs "/path/to/Cities In Motion"
```

The folder chosen by `--download-game` is kept in `~/Library/Application Support/OpenCiM/install.cfg`. `regs
--print-paths` shows which folders OpenCiM uses.

## Your files

- Saves, maps, settings, mods and logs: `~/Documents/OpenCiM`
- Saves and maps of the original game, when they are in `~/Documents/Cities In Motion`: listed with a note, copied in
  when you open one (where the Mac version of the original keeps its saves is not checked yet)
- Crash reports: `~/Documents/OpenCiM/crashes/crash-<date>.txt`. Please attach the file to a bug report.

Saves are the same files on all three systems.

## Controls on the Mac

- **Command** works as Ctrl (Cmd+Z, Cmd-click and the other Ctrl shortcuts); Cmd+Q asks to quit like closing the window.
- The trackpad: two fingers scroll (zoom), pinching zooms too.
- Controllers through SDL3: Xbox, PlayStation, Switch and others; the game's controller support works as on Windows.

## Differences from Windows

- Graphics: Vulkan through MoltenVK. No raytracing (Metal through MoltenVK offers no ray queries to Vulkan), no HDR10
  output. AMD FSR depends on what MoltenVK supports. When Vulkan cannot start, OpenCiM uses OpenGL 4.1.
- No glasses-free 3D on Leia SR displays (Windows only). The other stereo 3D modes are there.
- No setup program: the DMG is the release. Updating means dragging the new app over the old one.
- Crash reports are text files with the stack (no minidump). `regs-tool symbolize <report>` names the stack lines
  with `atos` and the release's symbols (`OpenCiM-<version>-macos-symbols.zip`).
- **Not tested on a real Mac yet** (2026-11-27): the app is built and packed by GitHub Actions; first reports welcome.

## Building

### GitHub Actions

`.github/workflows/macos.yml` builds the universal app on a macOS runner:

- every push to main that touches `port/`: the DMG as a build artifact of the run (Actions > macOS > the run > Artifacts)
- when a GitHub release is published: the DMG of its tag is attached to that release
- by hand: Actions > macOS > Run workflow

A Windows job draws the controller glyphs first (their letters use Windows fonts) and hands them to the Mac job.

### Signing and notarization

Without secrets the workflow signs ad hoc. For Developer ID signing and notarization, add these repository secrets
(Settings > Secrets and variables > Actions > New repository secret):

| Secret | What |
|---|---|
| `MACOS_CERTIFICATE` | your **Developer ID Application** certificate with its private key, exported from Keychain Access as `.p12`, then base64: `base64 -i cert.p12 \| pbcopy` |
| `MACOS_CERTIFICATE_PASSWORD` | the password you gave the `.p12` |
| `MACOS_SIGN_IDENTITY` | the certificate's name, e.g. `Developer ID Application: Jane Doe (AB12CD34EF)` (`security find-identity -v -p codesigning`) |
| `APPLE_ID` | your Apple Account e-mail |
| `APPLE_TEAM_ID` | the 10-character team ID (developer.apple.com > Account > Membership) |
| `APPLE_APP_PASSWORD` | an app-specific password for notarization (account.apple.com > Sign-In and Security > App-Specific Passwords) |

The certificate is created at developer.apple.com > Certificates > + > Developer ID Application (with a certificate
request from Keychain Access > Certificate Assistant).

### On a Mac

With the Xcode command line tools (`xcode-select --install`) and `brew install cmake ninja`:

```sh
cd port
macos/build.sh [<a Windows build's data/regs/ui/pad>]   # build/release/OpenCiM-<version>-macos.dmg
```

`build.sh` downloads MoltenVK from Khronos' release the first time. Set `MACOS_SIGN_IDENTITY` (and `APPLE_ID`,
`APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` for notarization) to sign like the workflow; see `macos/package.sh`.

### What is different in the code

- The SDL3 platform layer of the Linux build (`window_sdl.cpp`, `gamepad_sdl.cpp`, `platform_posix.cpp`,
  `crash_posix.cpp`), with `#if defined(__APPLE__)` where the Mac differs: Command as Ctrl, forward-compatible
  OpenGL core contexts, MoltenVK from `Contents/Frameworks` (`SDL_HINT_VULKAN_LIBRARY`), `_NSGetExecutablePath`,
  `sysctl` for the crash report, `atos` for symbols.
- The Vulkan device takes `vkGetInstanceProcAddr` from SDL (`Window::vulkanGetInstanceProcAddr`) and enables
  `VK_KHR_portability_enumeration` / `VK_KHR_portability_subset` when the driver offers them (MoltenVK).
- `src/app/main.cpp`: data from `Contents/Resources/data`, `install.cfg` in Application Support.
- `src/core/game_find.cpp`: Steam's Mac folder; `gameDataFolder` looks inside a game app bundle.
- The bundle (`macos/Info.plist.in`), signing, the DMG and notarization: `macos/package.sh`.
