#!/usr/bin/env bash
# Pikmin - NextOS / Mali-450 GLES2 cross-configure (host -> aarch64).
# Uses the compiler and sysroot produced by the current NextOS build tree.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PIKMIN_BUILD_DIR:-$ROOT/build/nextos-gles2}"
NEXTOS_ROOT="${NEXTOS_ROOT:-$HOME/NextOS-Elite-Edition}"
NEXTOS_TOOLCHAIN_ROOT="${NEXTOS_TOOLCHAIN_ROOT:-$(
  find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
    -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
    -print | sort -V | tail -1
)}"
export NEXTOS_TOOLCHAIN_ROOT
# SDL3 source tree used for the vendored build. Point this at your local
# Mali-fbdev SDL3 checkout (see README, "Building").
SDL_SRC="${PIKMIN_SDL_SRC:-$ROOT/../SDL3-mali-current}"
# The Mali driver to link GLES against. It is proprietary and is NOT shipped
# here: copy your device's /usr/lib/libMali.so into build/sysroot/lib/ and
# symlink libGLESv2.so and libEGL.so to it (see README, "Building").
GLESV2="$ROOT/build/sysroot/lib/libGLESv2.so"

[ -x "$NEXTOS_TOOLCHAIN_ROOT/bin/aarch64-libreelec-linux-gnu-gcc" ] || {
  echo "Current NextOS AArch64 toolchain not found: $NEXTOS_TOOLCHAIN_ROOT" >&2
  echo "Set NEXTOS_TOOLCHAIN_ROOT to the toolchain of your NextOS build tree." >&2
  exit 1
}

[ -d "$SDL_SRC" ] || {
  echo "SDL3 source tree not found: $SDL_SRC" >&2
  echo "Set PIKMIN_SDL_SRC to your Mali-fbdev SDL3 checkout." >&2
  exit 1
}

[ -e "$GLESV2" ] || {
  echo "Mali GLES driver not found: $GLESV2" >&2
  echo "Copy libMali.so from your device into build/sysroot/lib/ and link" >&2
  echo "libGLESv2.so / libEGL.so to it. It is not redistributed here." >&2
  exit 1
}

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/pikmin-aarch64.toolchain.cmake" \
  -DPIKMIN_NEXTOS_TOOLCHAIN_ROOT="$NEXTOS_TOOLCHAIN_ROOT" \
  -DCMAKE_SKIP_RPATH=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DAURORA_GLES2=ON \
  -DAURORA_ENABLE_DVD=ON -DAURORA_ENABLE_CARD=ON -DAURORA_ENABLE_RMLUI=OFF -DAURORA_ENABLE_GX=ON \
  -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=shared \
  -DFETCHCONTENT_SOURCE_DIR_SDL="$SDL_SRC" \
  -DAURORA_GLESV2_LIB="$GLESV2" \
  -DRust_CARGO_TARGET=aarch64-unknown-linux-gnu \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenGL=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_zstd=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE \
  -DSDL_MALI=ON -DSDL_GPU=OFF -DSDL_UNIX_CONSOLE_BUILD=ON \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_VULKAN=OFF -DSDL_RENDER_VULKAN=OFF \
  -DSDL_OPENGL=OFF -DSDL_OPENGLES=ON \
  -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_OSS=OFF -DSDL_DISKAUDIO=OFF \
  -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_RPI=OFF -DSDL_ROCKCHIP=OFF \
  -DSDL_HIDAPI=OFF -DSDL_LIBUDEV=OFF -DSDL_HIDAPI_LIBUSB=OFF -DSDL_LIBURING=OFF \
  -DSDL_CAMERA=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_SENSOR=ON -DSDL_POWER=OFF \
  -DSDL_DUMMYAUDIO=ON -DSDL_DUMMYVIDEO=ON -DSDL_OFFSCREEN=ON \
  -DSDL_RENDER=ON -DSDL_LOADSO=ON -DSDL_LIBC=ON -DSDL_SYSTEM_ICONV=ON
