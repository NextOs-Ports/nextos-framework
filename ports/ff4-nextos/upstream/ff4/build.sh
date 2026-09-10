#!/bin/bash
# Build aarch64 do so-loader do Final Fantasy IV (3D remake base).
# Toolchain NextOS Amlogic-old aarch64 -> Mali-450 fbdev (GLES1 do sysroot).
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
STRIP=$TC/bin/aarch64-libreelec-linux-gnu-strip
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }
[ -x "$STRIP" ] || { echo "strip nao encontrado: $STRIP"; exit 1; }

SRCS=$(ls src/*.c)
$CC --sysroot="$SR" -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wall -Wno-unused-parameter -Wno-incompatible-pointer-types \
    -o ff4 $SRCS \
    -lSDL2 -lGLESv1_CM -lEGL -lz -ldl -lm -lpthread

"$STRIP" --strip-unneeded ff4

echo "BUILD OK -> $(file ff4 | cut -d, -f1-3)"
