#!/bin/bash
# Packs the APK in the build image (docs/android.md):
#   android/package.sh <data dir> <out dir> [<keystore> <alias>]   (KEY_PASS: the keystore's password)
# from build/android-<abi>/libmain.so (android/build.sh; each ABI that was built), the Java sources (SDL's activity and
# android/java), android/res, the setup screen's pictures and the port's data (a desktop build's data/ with its
# generated files). The native libraries go in uncompressed and 16 KB aligned (extractNativeLibs=false); their debug
# symbols are kept beside the APK. Without a keystore a debug key is made in the out dir (tests only).
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
DATA=$(cd "$1" && pwd)
mkdir -p "$2"
OUT=$(cd "$2" && pwd)
KS=${3:-}
ALIAS=${4:-}
BT=$ANDROID_HOME/build-tools/35.0.0
JAR=$ANDROID_HOME/platforms/android-35/android.jar
STRIP=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
W=$(mktemp -d)
ABIS=()
for A in arm64-v8a x86_64; do [ -f "$P/build/android-$A/libmain.so" ] && ABIS+=("$A"); done
[ ${#ABIS[@]} -gt 0 ] || { echo "no libmain.so: run android/build.sh first" >&2; exit 1; }
INFO="$P/build/android-${ABIS[0]}/generated/build_info.h"
VER=$(sed -n 's/.*REGS_VERSION_STRING "\([^"]*\)".*/\1/p' "$INFO")
BUILD=$(sed -n 's/.*REGS_BUILD_NUMBER \([0-9]*\).*/\1/p' "$INFO")
NAME=OpenCiM-$VER-android

# the manifest with the version
sed -e "s/@VERSION@/$VER/" -e "s/@BUILD@/$BUILD/" "$P/android/AndroidManifest.xml" > "$W/AndroidManifest.xml"

# Java: SDL's activity classes and OpenCiM's own, into one classes.dex
mkdir -p "$W/classes"
if ! javac -nowarn -Xlint:none -source 11 -target 11 -encoding UTF-8 -classpath "$JAR" -d "$W/classes" \
     $(find "$P/third_party/SDL3/android-project/app/src/main/java" "$P/android/java" -name '*.java') >"$W/javac.log" 2>&1; then
  grep -v '^Note:' "$W/javac.log" >&2
  exit 1
fi
"$BT/d8" --release --min-api 29 --lib "$JAR" --output "$W" $(find "$W/classes" -name '*.class')

# assets: the port's data (unpacked on the first start, listed in data.list) and the setup screen's pictures
mkdir -p "$W/assets/setup"
cp -R "$DATA" "$W/assets/data"
(cd "$W/assets/data" && find . -type f ! -name '.*' | sed 's|^\./||' | sort > "$W/assets/data.list")
cp "$P/src/setup/show/bg-welcome.jpg" "$P/src/setup/res/title.png" "$W/assets/setup/"

# resources and the base APK
"$BT/aapt2" compile --dir "$P/android/res" -o "$W/res.zip"
"$BT/aapt2" link -o "$W/base.apk" -I "$JAR" --manifest "$W/AndroidManifest.xml" -A "$W/assets" "$W/res.zip" \
  --min-sdk-version 29 --target-sdk-version 35 --version-code "$BUILD" --version-name "$VER" -0 jpg -0 png
(cd "$W" && zip -q base.apk classes.dex)

# native libraries: stripped into the APK (stored, not compressed), the full ones kept for crash reports
mkdir -p "$W/sym"
for A in "${ABIS[@]}"; do
  mkdir -p "$W/lib/$A" "$W/sym/$A"
  cp "$P/build/android-$A/libmain.so" "$W/sym/$A/libmain.so"
  "$STRIP" --strip-debug -o "$W/lib/$A/libmain.so" "$P/build/android-$A/libmain.so"
done
(cd "$W" && zip -q -0 -r base.apk lib)
"$BT/zipalign" -f -P 16 4 "$W/base.apk" "$W/aligned.apk"

# signing: the given key, else a debug key for tests
if [ -z "$KS" ]; then
  KS="$OUT/debug.keystore"
  ALIAS=debug
  export KEY_PASS=android
  [ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias debug -keyalg RSA \
    -keysize 2048 -validity 10000 -dname "CN=OpenCiM Debug" >/dev/null 2>&1
  NAME=$NAME-debug
fi
"$BT/apksigner" sign --ks "$KS" --ks-key-alias "$ALIAS" --ks-pass env:KEY_PASS --key-pass env:KEY_PASS \
  --out "$OUT/$NAME.apk" "$W/aligned.apk"
"$BT/apksigner" verify "$OUT/$NAME.apk"
(cd "$W/sym" && tar -czf "$OUT/$NAME-symbols.tar.gz" .)
(cd "$OUT" && sha256sum "$NAME.apk" > "$NAME.apk.sha256")
rm -rf "$W"
ls -la "$OUT/$NAME"*
