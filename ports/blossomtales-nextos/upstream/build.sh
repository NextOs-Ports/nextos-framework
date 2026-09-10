#!/bin/bash
# build.sh -- BlossomTales (MonoGame/.NET Android) Mono so-loader, aarch64.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain

if [ -x "$TC/bin/aarch64-libreelec-linux-gnu-gcc" ]; then
  CC="$TC/bin/aarch64-libreelec-linux-gnu-gcc"
  SYSROOT="--sysroot=$TC/aarch64-libreelec-linux-gnu/sysroot"
  OBJDUMP="$TC/bin/aarch64-libreelec-linux-gnu-objdump"
  echo "toolchain: NextOS ($CC)"
else
  CC="/usr/bin/aarch64-linux-gnu-gcc"
  SYSROOT=""
  OBJDUMP="/usr/bin/aarch64-linux-gnu-objdump"
  echo "toolchain: sistema fallback ($CC, glibc 2.43)"
fi

SRCS="src/main.c src/so_util.c src/jni_shim.c src/imports.gen.c \
      src/bionic_shims.c src/pthread_bridge.c src/sdv_egl_bridge.c \
      src/present_fbo.c src/mono_trace.c src/fmod_shim.c src/aaudio_shim.c src/astc_shim.c src/etc1.c \
      src/media_player.c src/util.c src/error.c"

$CC $SYSROOT -I src -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -DPORT_WINDOW_TITLE='"BlossomTales"' \
    -Wno-unused-parameter -Wno-unused-function \
    -o blossomtales $SRCS \
    -ldl -lm -lpthread

echo "OK: $HERE/blossomtales"
"$OBJDUMP" -p blossomtales 2>/dev/null | grep NEEDED || true
