#!/bin/bash
# Builds steam-test (src/tools/steam_test.cpp) for this Linux host in the Android build image and runs it:
#   docker run --rm -it -v <port>:/src/port opencim-android-build bash /src/port/android/steam-test.sh anon /tmp/x 1007
set -euo pipefail
P=/src/port
D=/opt/deps/host
B=/tmp/steam-test
mkdir -p $B
if [ ! -x $B/steam-test ] || [ -n "${REBUILD:-}" ] || find $P/src/core $P/src/tools/steam_test.cpp -newer $B/steam-test | grep -q .; then
  gcc -O2 -c $P/third_party/lzma/LzmaDec.c -o $B/LzmaDec.o -I$P/third_party/lzma
  Z=$P/third_party/zstd
  for f in $Z/common/{debug,entropy_common,error_private,fse_decompress,xxhash,zstd_common}.c $Z/decompress/*.c; do
    gcc -O2 -DZSTD_DISABLE_ASM -c $f -o $B/zstd_$(basename $f .c).o
  done
  g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -I$P/src -I$P/third_party/lzma -I$P/third_party/qrcodegen -I$Z -I$D/include \
    $P/src/tools/steam_test.cpp $P/src/core/steam_client.cpp $P/src/core/steam_crypto.cpp $P/src/core/net_curl.cpp \
    $P/src/core/log.cpp $P/src/core/crash.cpp $P/src/core/inflate.cpp $P/third_party/qrcodegen/qrcodegen.cpp $B/LzmaDec.o $B/zstd_*.o \
    $D/lib/libcurl.a $D/lib/libmbedtls.a $D/lib/libmbedx509.a $D/lib/libmbedcrypto.a $D/lib/libeverest.a $D/lib/libp256m.a \
    -lz -lpthread -o $B/steam-test
fi
exec $B/steam-test "$@"
