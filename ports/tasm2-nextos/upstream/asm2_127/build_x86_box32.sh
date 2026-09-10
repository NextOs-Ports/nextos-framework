#!/bin/sh
set -eu

cd "$(dirname "$0")"

SDL32_DIR=${ASM2_SDL32_DIR:-"$HOME/.local/share/Steam/ubuntu12_32/steamapps/content/app_105600/depot_105602/lib"}
SDL32_LIBRARY="$SDL32_DIR/libSDL2-2.0.so.0"
STRIP32=${ASM2_STRIP32:-strip}

if [ ! -f "$SDL32_LIBRARY" ]; then
  echo "32-bit SDL2 link library not found: $SDL32_LIBRARY" >&2
  exit 1
fi

gcc -m32 -std=gnu11 -O2 -fno-pie -no-pie -static-libgcc \
  -D_GNU_SOURCE=1 -Wall -Wextra -Werror \
  -Wl,--export-dynamic -Wl,--no-as-needed \
  -Wl,--allow-shlib-undefined -Wl,-rpath-link,"$SDL32_DIR" \
  -I src -I/usr/include/SDL2 \
  src/main.c src/imports.c src/so_util_x86.c src/util.c src/error.c \
  src/bionic_compat.c src/platform_shims.c src/opensl_bridge.c \
  src/pthread_bridge.c src/jni_bridge.c src/android_callbacks.c \
  src/installer_compat.c src/startup_compat.c src/audio_compat.c \
  src/shop_compat.c src/video.c src/first_accept.c src/input.c \
  src/x86_runtime_compat.c \
  src/capture_x86.c \
  src/setjmp_bridge_x86.S src/x86_gl_lazy.S \
  -L"$SDL32_DIR" -l:libSDL2-2.0.so.0 -ldl -lm -pthread \
  -o asm2_127_x86_box32

"$STRIP32" --strip-unneeded asm2_127_x86_box32

file asm2_127_x86_box32
sha256sum asm2_127_x86_box32
readelf -d asm2_127_x86_box32 | grep NEEDED
