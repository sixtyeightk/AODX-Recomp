# Build environment for the Linux package (tools/build-linux-docker.sh runs tools/build-linux.sh
# in it). Ubuntu 22.04: glibc 2.35, so the program runs on that and anything newer (Ubuntu
# 22.04+, Debian 12+, Fedora 36+, SteamOS 3, Arch...).
#
# SDL2 is built from source as a static library, with its usual run-time loading of the
# system's X11 / Wayland, PipeWire / PulseAudio / ALSA, udev and OpenGL: the game then needs
# nothing but glibc, and uses whatever the desktop has. The -dev packages below are only
# for SDL's build (headers); none of them is linked.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config python3 python3-pil \
        ca-certificates curl file \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
        libxinerama-dev libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols \
        libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev \
        libpulse-dev libasound2-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev \
    && rm -rf /var/lib/apt/lists/*

ARG SDL_VERSION=2.32.10
ARG SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
RUN curl -fsSL -o /tmp/sdl.tar.gz \
        "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL2-${SDL_VERSION}.tar.gz" \
    && echo "${SDL_SHA256}  /tmp/sdl.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/sdl.tar.gz -C /tmp \
    && cmake -S /tmp/SDL2-${SDL_VERSION} -B /tmp/sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/sdl2 -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON \
        -DSDL_TEST=OFF \
    && cmake --build /tmp/sdl-build && cmake --install /tmp/sdl-build \
    && rm -rf /tmp/sdl.tar.gz /tmp/SDL2-${SDL_VERSION} /tmp/sdl-build
ENV CMAKE_PREFIX_PATH=/opt/sdl2
# tools/make-guide.py needs Pillow >= 10.1 (Ubuntu 22.04's python3-pil is 9.0).
RUN apt-get update && apt-get install -y --no-install-recommends python3-pip \
    && rm -rf /var/lib/apt/lists/* && pip3 install --no-cache-dir Pillow==12.3.0
