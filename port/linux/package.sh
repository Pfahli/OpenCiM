#!/bin/bash
# Packs the Linux release (docs/linux.md), run in the build container by make-release-linux.ps1:
#   package.sh <build dir> <out dir> <version> <build number>
# OpenCiM-<version>-linux-x64.tar.gz with the stripped programs, data/, README, licences and install.sh; its .sha256;
# OpenCiM-<version>-linux-x64-symbols.tar.gz with the debug info (for crash reports: regs-tool symbolize, addr2line).
set -euo pipefail
B=$1 OUT=$2 VER=$3 BUILD=$4
NAME=OpenCiM-$VER
STAGE=/tmp/stage/$NAME
SYM=/tmp/stage/$NAME-symbols
rm -rf /tmp/stage
mkdir -p "$STAGE/licences" "$SYM" "$OUT"
cp "$B/regs" "$B/regs-tool" "$STAGE/"
cp -r "$B/data" "$STAGE/"
cp /src/port/README.md "$STAGE/"
cp /src/LICENSE "$STAGE/LICENSE.txt"                      # OpenCiM itself: GPL-3.0-or-later
cp /src/LICENSE-EXCEPTION.txt "$STAGE/LICENSE-EXCEPTION.txt"
cp /src/port/third_party/glslang/LICENSE.txt "$STAGE/licences/glslang.txt"
cp /src/port/third_party/vulkan-headers/LICENSE.md "$STAGE/licences/vulkan-headers.md"
cp /src/port/third_party/fidelityfx-fsr/LICENSE.txt "$STAGE/licences/fidelityfx-fsr.txt"
cp /src/port/third_party/SDL3/LICENSE.txt "$STAGE/licences/SDL3.txt"
# miniaudio's and stb_image's licences are notices at the end of their headers (public domain or MIT)
sed -n '/This software is available as a choice of the following licenses/,$p' /src/port/third_party/miniaudio/miniaudio.h > "$STAGE/licences/miniaudio.txt"
sed -n '/This software is available under 2 licenses/,$p' /src/port/third_party/stb/stb_image.h > "$STAGE/licences/stb_image.txt"
cp /src/port/linux/install.sh "$STAGE/"
printf 'OpenCiM %s (build %s, Linux x86-64)\nNeeds the original Cities in Motion; nothing of the game is in this download.\n' "$VER" "$BUILD" > "$STAGE/VERSION.txt"
# debug info aside, then the programs stripped (a third of the size)
for f in regs regs-tool; do
  objcopy --only-keep-debug "$STAGE/$f" "$SYM/$f.debug"
  strip --strip-unneeded "$STAGE/$f"
  objcopy --add-gnu-debuglink="$SYM/$f.debug" "$STAGE/$f"
done
find "$STAGE" -type d -exec chmod 755 {} +
find "$STAGE" -type f -exec chmod 644 {} +
chmod 755 "$STAGE/regs" "$STAGE/regs-tool" "$STAGE/install.sh"
TAR="$OUT/$NAME-linux-x64.tar.gz"
tar -C /tmp/stage --owner=0 --group=0 --numeric-owner -czf "$TAR" "$NAME"
(cd "$OUT" && sha256sum "$(basename "$TAR")" > "$(basename "$TAR").sha256")
tar -C /tmp/stage --owner=0 --group=0 --numeric-owner -czf "$OUT/$NAME-linux-x64-symbols.tar.gz" "$NAME-symbols"
ls -la "$OUT"/$NAME-linux-x64*
