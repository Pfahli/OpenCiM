# OpenCiM on Android

OpenCiM runs on Android phones and tablets (Android 10 or newer, 64-bit ARM; x86-64 for the emulator): the same engine,
game and interface as on Windows, full screen in landscape, with Vulkan through the device's driver. It needs your own
copy of Cities in Motion, which the app downloads from Steam itself (or copies from a folder on the device).

**Status (2026-11-28): tested in the Android emulator only.** Real devices, the Steam login with a real account and
controllers on Android have not been tried yet.

## Requirements

- Android 10 (API 29) or newer, 64-bit (arm64-v8a)
- **Vulkan 1.3** with dynamic rendering and synchronization2: most phones and tablets from about 2022 on (Android 13/14
  devices). Push descriptors and BC (DXT) textures are used when the driver has them; without them (many Mali and
  PowerVR GPUs) OpenCiM makes descriptor sets per draw and decodes the game's DXT textures to RGBA8 when loading
  (slower loading, more memory).
- About 2 GB free storage (the game 1.3 GB, the app 70 MB) and 4 GB RAM or more

## Install

1. Download `OpenCiM-<version>-android.apk` from the [latest release](https://github.com/Pfahli/OpenCiM/releases/latest)
   and open it on the device (allow installing from this source when Android asks).
2. Start OpenCiM. The first time it shows the setup screen to get the game:
   - **Sign in to Steam** with account name and password. Steam Guard then asks for a confirmation in the Steam app,
     or for the code from the app or the e-mail. OpenCiM downloads the game's Windows files and the DLCs the account
     owns (about 1.3 GB). It keeps a login token in the app's private storage, never the password.
   - **Sign in with a QR code**: scan the code with the Steam app on another device (Steam Guard > the QR symbol).
   - **Copy from a folder**: copy the `Cities In Motion` folder from a PC to the device first (USB, a memory card), then
     choose it. OpenCiM copies it into its own storage.
3. Afterwards OpenCiM starts the game directly. Long-press the app icon > **Game files** for the setup screen again
   (update or repair from Steam, sign out, copy again).

## Your files

In the app's folder on the shared storage, `Android/data/io.github.pfahli.opencim/files` (reachable from a PC over USB):

- `OpenCiM/`: saves, maps, settings, mods, the log (`opencim.log`) and crash reports (`crashes/`)
- `Cities In Motion/`: put saves and maps of the original game here (copied from a PC's `Documents\Cities In Motion`);
  OpenCiM lists them and copies one in when you open it
- `Game/`: the game files

Saves are the same files as on Windows, Linux and macOS. Uninstalling the app deletes this folder.

## Controls

- **Tap**: click. **Drag with one finger**: drag (lay roads and tracks, move sliders). **Hold a finger still** (half a
  second): right click.
- **Two fingers**: move the camera; pinch to zoom, turn them to rotate the view.
- **Back**: Escape (closes windows and tools, opens the game menu).
- Text fields bring up the on-screen keyboard. A Bluetooth or USB keyboard, mouse and controller work as on the PC.
- The interface is drawn at 1.5× on 1080p phones and 2× on large tablets (the graphics page has 1×, 1.5×, 2×).

## Differences from Windows

- Vulkan only (no OpenGL fallback). No raytracing, no HDR output, no AMD FSR yet (its shaders are built by a host tool
  the Android build does not run yet).
- No glasses-free 3D, no pop-out windows (one window on Android).
- The game pauses while the app is in the background and continues when it returns.

## Building

The Android build runs in Docker on Windows, like the Linux build:

```powershell
cd port
.\build.ps1           # Windows first: the controller glyphs' letters are drawn with Windows fonts
.\build-linux.ps1     # the port's data with its generated files (build/linux-Release/data) goes into the APK
.\build-android.ps1   # build/android-release/OpenCiM-<version>-android[-debug].apk, .sha256 and the symbols
```

`android/Dockerfile` makes the image: Ubuntu 22.04, OpenJDK 17, the Android command-line tools with the NDK r27c,
platform android-35 and build-tools 35.0.0 (building the image accepts the Android SDK License), and curl 8 on mbedTLS
3.6 built for arm64-v8a, x86_64 and the host (`android/build-deps.sh`). `android/build.sh` builds `libmain.so` for each
ABI with the NDK's CMake toolchain (`android/android.cmake`); `android/package.sh` compiles the Java code (SDL's activity
classes from `third_party/SDL3/android-project` and `android/java`), the resources and assets, puts the native
libraries in uncompressed and 16 KB aligned, and signs the APK.

Signing: `.\build-android.ps1 -Keystore <file> -KeyAlias <alias>` with the keystore's password in `$env:KEY_PASS`.
Without a keystore the APK gets a debug key (tests only; Android refuses to update an app signed with another key, so
releases must always use the same release key).

### The emulator

The Android emulator (Android Studio's SDK) runs the x86_64 build with the PC's GPU (`-gpu host`); its Vulkan driver
has no push descriptors, so it also tests that path. `adb push` puts files into the app's folder as the shell user:
`adb shell chmod -R 777 /sdcard/Android/data/io.github.pfahli.opencim/files/Game` makes them readable for the app.
Test switches (environment variables such as `REGS_VK_NO_BC=1`) go in with
`adb shell setprop debug.opencim.env "NAME=value;NAME2=value"`; the log is `adb logcat -s OpenCiM SDL`.

### The Steam client

`src/core/steam_client.cpp` is OpenCiM's own Steam client: a WebSocket to a Steam connection manager (protobuf
messages, field numbers from github.com/SteamDatabase/Protobufs), the Authentication service for the login (password
RSA-encrypted with Steam's key, Steam Guard, QR), the client logon with the refresh token, PICS for the app's depots,
the depot keys, then the content servers over HTTPS: manifests (zip), chunks (AES-256 with the depot key, then LZMA
"VZ", zstd "VSZa" or zip, checked with Steam's Adler-32). Files already there are checked chunk by chunk (SHA-1), so an
interrupted download continues and a repair fetches only what differs. `src/tools/steam_test.cpp` drives it from a
terminal in the build image:

```powershell
docker run --rm -it -v "${PWD}:/src/port" opencim-android-build bash /src/port/android/steam-test.sh anon /tmp/x 1007
docker run --rm -it -v "${PWD}:/src/port" opencim-android-build bash /src/port/android/steam-test.sh qr /tmp/cim
```

(`anon` downloads free content with Steam's anonymous account, e.g. app 1007; `qr` and `password` sign in and download
Cities in Motion.)

Third-party code of the Android build: SDL 3.4 (zlib), curl (curl licence) and mbedTLS (Apache 2.0, both only in the
build image), the LZMA SDK decoder (`third_party/lzma`, public domain), zstd's decoder (`third_party/zstd`, BSD) and
Nayuki's QR code generator (`third_party/qrcodegen`, MIT).

### What is different in the code

- `src/platform/window_sdl.cpp`: always full screen; fingers become clicks, drags, right clicks and two-finger camera
  gestures; the on-screen keyboard while a text field is active (`Window::textInput`, from the display's GUI pass);
  a new surface after the background (`Window::surfaceGeneration`); Back is Escape.
- `src/render/gpu_vk.cpp`: descriptor sets per draw without `VK_KHR_push_descriptor`, DXT decoded without BC textures,
  the swapchain unrotated in the window's size (the compositor turns it), the surface made again after the background.
- `src/app/main.cpp`: `SDL_main` in `libmain.so`; output into logcat (`src/platform/android_jni.cpp`).
- `src/engine/display.cpp`: the largest UI scale that fits by default.
- `src/tools/crash_posix.cpp`: the stack through the unwinder (no `execinfo.h` before Android 13).
- `android/java`: `SetupActivity` (the setup screen, the data unpacking), `GameActivity` (SDL's activity), `SteamBridge`
  (the JNI functions), `Paths`.
