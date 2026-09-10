#!/bin/bash
set -euo pipefail

PORT_DIR=$(cd "$(dirname "$0")" && pwd)
TOOLCHAIN_ROOT=$(ls -d "$HOME"/NextOS-Elite-Edition/build*Amlogic-old*/toolchain 2>/dev/null | head -1)
CC="$TOOLCHAIN_ROOT/bin/armv8a-emuelec-linux-gnueabihf-gcc"
SYSROOT="$TOOLCHAIN_ROOT/armv8a-emuelec-linux-gnueabihf/sysroot"

if [ ! -x "$CC" ]; then
  echo "ARMHF toolchain not found: $CC" >&2
  exit 1
fi

cd "$PORT_DIR"
"$CC" --sysroot="$SYSROOT" \
  -std=gnu11 -march=armv7-a -mfpu=neon -mfloat-abi=hard \
  -D_GNU_SOURCE -Isrc -O2 -fPIC -fno-omit-frame-pointer \
  -Wall -Wextra -Wno-unused-parameter \
  -Wl,--export-dynamic -Wl,--no-as-needed -Wl,-s \
  -o asm2_127 \
  src/main.c src/imports.c src/so_util.c src/util.c src/error.c \
  src/bionic_compat.c src/softfp_bridge.c src/platform_shims.c \
  src/opensl_bridge.c \
  src/pthread_bridge.c src/jni_bridge.c src/android_callbacks.c \
  src/installer_compat.c src/startup_compat.c src/audio_compat.c \
  src/shop_compat.c \
  src/video.c src/first_accept.c src/input.c \
  src/setjmp_bridge.S \
  -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread

file asm2_127
