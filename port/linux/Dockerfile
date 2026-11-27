# The Linux build environment (docs/linux.md): Ubuntu 22.04 LTS with GCC 13 (ubuntu-toolchain-r PPA), so the game needs
# only glibc 2.35 and runs on Ubuntu 22.04+, Debian 12+, Fedora 36+, Arch, SteamOS 3 (Steam Deck). libstdc++ and
# libgcc are linked statically. SDL3 is built from third_party/SDL3 and linked statically; it loads X11, Wayland,
# PipeWire/PulseAudio/ALSA and libudev at run time, so only their headers are needed here.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends software-properties-common gpg-agent ca-certificates \
    && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && apt-get update && apt-get install -y --no-install-recommends \
      gcc-13 g++-13 make cmake ninja-build git pkg-config file binutils \
      libcurl4-openssl-dev \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
      libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev libdrm-dev libgbm-dev \
      libdbus-1-dev libudev-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libusb-1.0-0-dev \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100 \
    && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-13 100 \
    && rm -rf /var/lib/apt/lists/*
RUN git config --global --add safe.directory '*'
WORKDIR /src
