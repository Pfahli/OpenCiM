#!/bin/bash
# Packs the Mac release (docs/macos.md) on a Mac:
#   macos/package.sh <build dir> <out dir> <MoltenVK dylib>
# OpenCiM.app (regs and regs-tool in Contents/MacOS, data and licences in Contents/Resources, MoltenVK in
# Contents/Frameworks), signed, in OpenCiM-<version>-macos.dmg (+ .sha256), and the debug symbols as a zip.
# Signing: MACOS_SIGN_IDENTITY ("Developer ID Application: Name (TEAMID)", in the keychain) signs with the hardened
# runtime; without it the app is signed ad hoc (it runs after the player allows it once). Notarization when
# APPLE_ID, APPLE_TEAM_ID and APPLE_APP_PASSWORD (an app-specific password) are set too.
set -euo pipefail
B=$(cd "$1" && pwd)
OUT=$2
MVK=$3
ROOT=$(cd "$(dirname "$0")/.." && pwd)        # port/
REPO=$(cd "$ROOT/.." && pwd)
VER=$(sed -n 's/.*REGS_VERSION_STRING "\([^"]*\)".*/\1/p' "$B/generated/build_info.h")
BUILD=$(sed -n 's/.*REGS_BUILD_NUMBER \([0-9]*\).*/\1/p' "$B/generated/build_info.h")
NAME=OpenCiM-$VER
STAGE=$(mktemp -d)
APP=$STAGE/dmg/OpenCiM.app
mkdir -p "$OUT" "$APP/Contents/MacOS" "$APP/Contents/Resources/licences" "$APP/Contents/Frameworks"
OUT=$(cd "$OUT" && pwd)

sed -e "s/@VERSION@/$VER/" -e "s/@BUILD@/$BUILD/" "$ROOT/macos/Info.plist.in" > "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
cp "$B/regs" "$B/regs-tool" "$APP/Contents/MacOS/"
cp -R "$B/data" "$APP/Contents/Resources/"
cp "$MVK" "$APP/Contents/Frameworks/libMoltenVK.dylib"
cp "$ROOT/README.md" "$APP/Contents/Resources/"
cp "$REPO/LICENSE" "$APP/Contents/Resources/LICENSE.txt"
cp "$REPO/LICENSE-EXCEPTION.txt" "$APP/Contents/Resources/"
cp "$ROOT/third_party/glslang/LICENSE.txt" "$APP/Contents/Resources/licences/glslang.txt"
cp "$ROOT/third_party/vulkan-headers/LICENSE.md" "$APP/Contents/Resources/licences/vulkan-headers.md"
cp "$ROOT/third_party/fidelityfx-fsr/LICENSE.txt" "$APP/Contents/Resources/licences/fidelityfx-fsr.txt"
cp "$ROOT/third_party/SDL3/LICENSE.txt" "$APP/Contents/Resources/licences/SDL3.txt"
sed -n '/This software is available as a choice of the following licenses/,$p' "$ROOT/third_party/miniaudio/miniaudio.h" > "$APP/Contents/Resources/licences/miniaudio.txt"
sed -n '/This software is available under 2 licenses/,$p' "$ROOT/third_party/stb/stb_image.h" > "$APP/Contents/Resources/licences/stb_image.txt"
printf 'MoltenVK (Vulkan on Metal), Apache License 2.0: https://github.com/KhronosGroup/MoltenVK/blob/main/LICENSE\n' > "$APP/Contents/Resources/licences/MoltenVK.txt"
printf 'OpenCiM %s (build %s, macOS)\nNeeds the original Cities in Motion; nothing of the game is in this download.\n' "$VER" "$BUILD" > "$APP/Contents/Resources/VERSION.txt"

# debug symbols aside (dSYM), then the programs stripped
SYM=$STAGE/$NAME-macos-symbols
mkdir -p "$SYM"
for f in regs regs-tool; do
  dsymutil "$APP/Contents/MacOS/$f" -o "$SYM/$f.dSYM" 2>/dev/null || true
  strip -S -x "$APP/Contents/MacOS/$f"
done
(cd "$STAGE" && zip -qry "$OUT/$NAME-macos-symbols.zip" "$NAME-macos-symbols")

# signing: inner code first, then the bundle
ID=${MACOS_SIGN_IDENTITY:--}
OPTS=(--force --sign "$ID")
if [ "$ID" != "-" ]; then OPTS+=(--options runtime --timestamp); fi
codesign "${OPTS[@]}" "$APP/Contents/Frameworks/libMoltenVK.dylib"
codesign "${OPTS[@]}" "$APP/Contents/MacOS/regs-tool"
codesign "${OPTS[@]}" "$APP"
codesign --verify --strict --verbose=2 "$APP"

# the disk image: the app, a link to Applications, and a short note
ln -s /Applications "$STAGE/dmg/Applications"
cat > "$STAGE/dmg/Read me.txt" <<EOF
OpenCiM $VER for macOS (Apple Silicon and Intel, macOS 12 or newer)

Drag OpenCiM into Applications, then start it. It needs your own Cities in Motion: install it once with Steam
(the Mac or the Windows version); OpenCiM finds it there. Saves and settings go to Documents/OpenCiM.

More: https://github.com/Pfahli/OpenCiM/blob/main/port/docs/macos.md
EOF
DMG="$OUT/$NAME-macos.dmg"
rm -f "$DMG"
for try in 1 2 3; do  # (hdiutil on CI machines sometimes finds the volume busy)
  hdiutil create -volname "OpenCiM $VER" -srcfolder "$STAGE/dmg" -ov -format UDZO "$DMG" >/dev/null && break
  [ $try = 3 ] && exit 1
  sleep 5
done
codesign "${OPTS[@]}" "$DMG"

if [ "$ID" != "-" ] && [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ] && [ -n "${APPLE_APP_PASSWORD:-}" ]; then
  echo "notarizing $DMG ..."
  xcrun notarytool submit "$DMG" --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD" --wait
  xcrun stapler staple "$DMG"
  spctl --assess --type open --context context:primary-signature -v "$DMG" || true
else
  echo "(not notarized: signed $([ "$ID" = "-" ] && echo 'ad hoc' || echo "as $ID") only)"
fi
(cd "$OUT" && shasum -a 256 "$(basename "$DMG")" > "$(basename "$DMG").sha256")
rm -rf "$STAGE"
ls -la "$OUT"/$NAME-macos*
