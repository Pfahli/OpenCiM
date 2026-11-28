#!/bin/bash
# The Android build in the build image (android/Dockerfile, docs/android.md):
#   android/build.sh [<abi>...]   (default: arm64-v8a x86_64)
# libmain.so for each ABI in build/android-<abi>. KEEP_GOING=1 shows all compile errors at once.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
ABIS=${*:-"arm64-v8a x86_64"}
for A in $ABIS; do
  B=$P/build/android-$A
  cmake -S $P -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=$A \
    -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DREGS_DEPS_DIR=/opt/deps/$A >/dev/null
  cmake --build $B --target main -- ${KEEP_GOING:+-k 0}
  echo "built: $B/libmain.so ($(du -h $B/libmain.so | cut -f1))"
done
