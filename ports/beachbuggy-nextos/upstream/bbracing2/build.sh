#!/bin/bash
set -e

TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
STRIP=$TC/bin/aarch64-libreelec-linux-gnu-strip
SR=$TC/aarch64-libreelec-linux-gnu/sysroot

cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain not found: $CC"; exit 1; }

FW="$(cd ../../framework && pwd)"
SRCS="src/main.c src/cursor_bridge.c src/so_util.c src/asset_shim.c src/jni_shim.c src/bionic_shims.c src/opensles_shim.c src/pthread_bridge.c src/util.c src/error.c src/egl_bridge.c $FW/nxgl/src/nxgl_gles2.c $FW/nxgl/adapters/nxgl_frame_proof_adapter.c"
OPT_FLAGS="-O2"
[ -z "${BUILD_DEBUG:-}" ] || OPT_FLAGS="-O0 -g3"

"$CC" --sysroot="$SR" -D_GNU_SOURCE \
    -I src -I "$FW/nxgl/include" -I "$FW/nxgl/adapters" -I "$SR/usr/include" -I "$SR/usr/include/SDL3" \
    $OPT_FLAGS -fPIC -fno-omit-frame-pointer -rdynamic -fuse-ld=bfd \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-unused-result \
    -o bbracing2-nextos $SRCS \
    -lSDL3 -lz -ldl -lm -lpthread -lstdc++ -lgcc_s

if [ -z "${BUILD_DEBUG:-}" ]; then
    "$STRIP" --strip-unneeded bbracing2-nextos
fi

file bbracing2-nextos
sha256sum bbracing2-nextos
