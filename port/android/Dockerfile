# The Android build environment (docs/android.md): Ubuntu 22.04 with OpenJDK 17, the Android command-line tools, the
# NDK r27c, platform android-35 and build-tools 35.0.0 (the Android SDK License is accepted while building the image).
# curl 8 (HTTPS and WebSocket) on mbedTLS 3.6 is built here as static libraries for both ABIs of the APK (arm64-v8a for
# phones and tablets, x86_64 for the emulator) and for this Linux host (the Steam client's tests run here without an
# emulator); /opt/deps/<abi> holds include/ and lib/.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends software-properties-common gpg-agent ca-certificates \
    && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && apt-get update && apt-get install -y --no-install-recommends \
      openjdk-17-jdk-headless unzip zip curl xz-utils bzip2 python3 git make cmake ninja-build file \
      gcc-13 g++-13 zlib1g-dev \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100 \
    && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-13 100 \
    && rm -rf /var/lib/apt/lists/*

ENV ANDROID_HOME=/opt/android-sdk
ENV ANDROID_NDK=/opt/android-sdk/ndk/27.2.12479018
ARG CMDLINE_TOOLS=13114758
RUN mkdir -p $ANDROID_HOME/cmdline-tools && cd /tmp \
    && curl -fsSLo tools.zip https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS}_latest.zip \
    && unzip -q tools.zip && mv cmdline-tools $ANDROID_HOME/cmdline-tools/latest && rm tools.zip \
    && yes | $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --licenses >/dev/null \
    && $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --install "platforms;android-35" "build-tools;35.0.0" \
         "ndk;27.2.12479018" "platform-tools" >/dev/null \
    && rm -rf $ANDROID_HOME/.temp /root/.android/cache
ENV PATH=$PATH:$ANDROID_HOME/build-tools/35.0.0:$ANDROID_HOME/platform-tools

# mbedTLS and curl, static, for each target
ARG MBEDTLS=3.6.2
ARG CURL=8.11.1
COPY build-deps.sh /tmp/build-deps.sh
RUN cd /tmp && curl -fsSLo mbedtls.tar.bz2 https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS}/mbedtls-${MBEDTLS}.tar.bz2 \
    && curl -fsSLo curl.tar.xz https://curl.se/download/curl-${CURL}.tar.xz \
    && tar -xjf mbedtls.tar.bz2 && tar -xJf curl.tar.xz \
    && bash /tmp/build-deps.sh /tmp/mbedtls-${MBEDTLS} /tmp/curl-${CURL} \
    && rm -rf /tmp/mbedtls* /tmp/curl* /tmp/build-deps.sh
RUN git config --global --add safe.directory '*'
WORKDIR /src
