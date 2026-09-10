#!/bin/bash
# build aarch64 do FF4 The After Years (engine Matrix Software "cuore" so-loader)
# toolchain NextOS Amlogic-old aarch64 -> Mali-450 fbdev (libGLESv1_CM do sysroot).
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }
SRCS=$(ls src/*.c)
$CC --sysroot="$SR" -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wall -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-unused-parameter \
    -o ff4a $SRCS \
    -lSDL2 -lGLESv1_CM -lEGL -lz -ldl -lm -lpthread -lstdc++ -lgcc_s
echo "BUILD OK -> $(file ff4a | cut -d, -f1-3)"
