#!/bin/bash
# Builds mbedTLS and curl (HTTPS + WebSocket, static) into /opt/deps/<target> for host, arm64-v8a and x86_64
# (android/Dockerfile):  build-deps.sh <mbedtls source> <curl source>
set -euo pipefail
MBED=$1
CURLSRC=$2
API=29
for T in host arm64-v8a x86_64; do
  P=/opt/deps/$T
  X=()
  CA=(-DCURL_CA_BUNDLE=auto -DCURL_CA_PATH=auto)
  if [ $T != host ]; then
    X=(-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=$T
       -DANDROID_PLATFORM=android-$API -DCMAKE_FIND_ROOT_PATH=$P)
    # Android's own certificates; OpenCiM sets the newer APEX folder at run time when it exists
    CA=(-DCURL_CA_BUNDLE=none -DCURL_CA_PATH=/system/etc/security/cacerts)
  fi
  cmake -S $MBED -B /tmp/b-mbed-$T -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    "${X[@]}" >/dev/null
  cmake --build /tmp/b-mbed-$T >/dev/null && cmake --install /tmp/b-mbed-$T >/dev/null
  cmake -S $CURLSRC -B /tmp/b-curl-$T -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF \
    -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DMBEDTLS_INCLUDE_DIRS=$P/include \
    -DMBEDTLS_LIBRARY=$P/lib/libmbedtls.a -DMBEDX509_LIBRARY=$P/lib/libmbedx509.a -DMBEDCRYPTO_LIBRARY=$P/lib/libmbedcrypto.a \
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
    -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DENABLE_WEBSOCKETS=ON "${CA[@]}" "${X[@]}" >/dev/null
  cmake --build /tmp/b-curl-$T >/dev/null && cmake --install /tmp/b-curl-$T >/dev/null
  echo "deps $T: $(ls $P/lib/*.a | xargs -n1 basename | tr '\n' ' ')"
done
rm -rf /tmp/b-*
