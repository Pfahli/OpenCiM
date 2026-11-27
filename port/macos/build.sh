#!/bin/bash
# Builds and packs OpenCiM on a Mac (docs/macos.md), the same steps as the GitHub Actions workflow
# (.github/workflows/macos.yml):
#   macos/build.sh [<pad glyph dir>]
# Needs the Xcode command line tools (xcode-select --install) and CMake + Ninja (brew install cmake ninja). MoltenVK is
# downloaded from Khronos' release into build/moltenvk the first time. The controller glyphs' letters are drawn with
# Windows fonts: give a Windows build's data/regs/ui/pad as the argument, or the build goes without them.
# Result: build/release/OpenCiM-<version>-macos.dmg (signed ad hoc unless MACOS_SIGN_IDENTITY is set, see package.sh).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
MVK_VERSION=v1.4.2
if [ ! -f build/moltenvk/libMoltenVK.dylib ]; then
  mkdir -p build/moltenvk
  curl -L --fail -o build/moltenvk/mvk.tar "https://github.com/KhronosGroup/MoltenVK/releases/download/$MVK_VERSION/MoltenVK-macos.tar"
  tar -xf build/moltenvk/mvk.tar -C build/moltenvk
  cp "$(find build/moltenvk -path '*dylib/macOS/libMoltenVK.dylib' | head -1)" build/moltenvk/libMoltenVK.dylib
fi
PAD=()
if [ $# -ge 1 ]; then PAD=("-DREGS_PAD_GLYPHS_DIR=$(cd "$1" && pwd)"); fi
cmake -S . -B build/macos-Release -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" ${PAD[@]+"${PAD[@]}"}  # (macOS bash 3.2: empty arrays under set -u)
cmake --build build/macos-Release
macos/package.sh build/macos-Release build/release build/moltenvk/libMoltenVK.dylib
