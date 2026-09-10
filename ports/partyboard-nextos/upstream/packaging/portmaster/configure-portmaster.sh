#!/usr/bin/env bash
# PartyBoard — PortMaster / SDL2-shim cross-configure, run INSIDE the focal
# container (see build-in-docker.sh). Targets the same device set as the
# Dusklight PortMaster package: aarch64, Ubuntu 20.04 glibc, firmware SDL2.
#
# The renderer is our GLES2 backend, not Dawn, so none of Dusklight's Dawn
# patches apply here. What we do share is the SDL3 "sdl2" video driver: it
# dlopens the firmware's libSDL2 and publishes its borrowed EGL display,
# context and getProc as window properties, which lib/gl/device.cpp picks up.
set -euo pipefail

WORK="${WORK:-/work}"
SDL_SRC="${SDL_SRC:-/sdl}"
BUILD_DIR="${BUILD_DIR:-$WORK/build/portmaster-aarch64-focal-sdl2shim}"

[ -d "$SDL_SRC/src/video/sdl2" ] || {
  echo "SDL source has no sdl2 shim driver: $SDL_SRC" >&2
  echo "Expected the patched bmdhacks/SDL tree (see packaging/portmaster/patches)." >&2
  exit 1
}

cmake -S "$WORK" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$WORK/packaging/portmaster/aarch64-linux-gnu-gcc10.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SKIP_RPATH=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DAURORA_GLES2=ON \
  -DAURORA_ENABLE_DVD=ON -DAURORA_ENABLE_CARD=ON -DAURORA_ENABLE_RMLUI=ON -DAURORA_ENABLE_GX=ON \
  -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=shared \
  -DFETCHCONTENT_SOURCE_DIR_SDL="$SDL_SRC" \
  -DAURORA_GLESV2_LIB=/usr/lib/aarch64-linux-gnu/libGLESv2.so \
  -DRust_CARGO_TARGET=aarch64-unknown-linux-gnu \
  -DRust_RUSTUP_INSTALL_MISSING_TARGET=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenGL=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_zstd=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=TRUE \
  -DSDL_SDL2_BACKEND=ON -DSDL_SDL2_BACKEND_JOYSTICK=OFF \
  -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_GPU=OFF \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_VULKAN=OFF -DSDL_RENDER_VULKAN=OFF \
  -DSDL_OPENGL=OFF -DSDL_OPENGLES=ON \
  -DSDL_ALSA=OFF -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_OSS=OFF \
  -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_RPI=OFF -DSDL_ROCKCHIP=OFF \
  -DSDL_HIDAPI=OFF -DSDL_HIDAPI_LIBUSB=OFF -DSDL_LIBURING=OFF \
  -DSDL_CAMERA=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_SENSOR=OFF -DSDL_POWER=OFF \
  -DSDL_DUMMYAUDIO=ON -DSDL_DUMMYVIDEO=ON -DSDL_OFFSCREEN=ON \
  -DSDL_RENDER=ON -DSDL_LOADSO=ON -DSDL_LIBC=ON

cmake --build "$BUILD_DIR" --target partyboard -j"$(nproc)"
file "$BUILD_DIR/partyboard"
