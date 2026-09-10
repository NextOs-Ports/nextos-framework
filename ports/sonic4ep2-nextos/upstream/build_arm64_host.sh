#!/bin/bash
# Build Sonic 4 EP2 AARCH64 no HOST (toolchain aarch64-linux-gnu do sistema, headers /usr/include).
# Libs reais (SDL2/EGL/GLES/mpg123/vorbis/libc/stdc++) vem do device em runtime -> aqui so stubs.
set -e
CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
cd "$(dirname "$0")"
[ -f ./sonic4 ] || { echo "preciso do ./sonic4 (32-bit) p/ extrair nomes de simbolos undefined"; exit 1; }

# Headers das libs (arch-neutros) num dir LIMPO — NUNCA -I/usr/include (headers x86_64
# do host contaminam uintptr_t=32-bit -> "cast pointer->int different size" no arm64).
# <stdint.h>/<stddef.h> etc. vem do sysroot do proprio cross-gcc (uintptr_t=64-bit).
HDR=$(mktemp -d)
if [ -d /home/nextos/sdl2-aarch64/include/SDL2 ]; then cp -r /home/nextos/sdl2-aarch64/include/SDL2 "$HDR/SDL2"
else cp -r /usr/include/SDL2 "$HDR/SDL2"; fi
for d in GLES2 GLES3 EGL KHR vorbis ogg; do [ -d /usr/include/$d ] && cp -r /usr/include/$d "$HDR/$d"; done
for f in mpg123.h fmt123.h; do [ -f /usr/include/$f ] && cp /usr/include/$f "$HDR/"; done
STUB=$(mktemp -d); trap 'rm -rf "$STUB" "$HDR"' EXIT
gen() { "$NM" -D --undefined-only ./sonic4 | awk '{print $NF}' | grep -E "$1" | sort -u | sed 's/.*/void &(void){}/'; }
{ gen '^SDL_' | sed 's/void \(SDL_[A-Za-z0-9_]*\).*/\1/'; printf '%s\n' SDL_Init SDL_CreateRenderer SDL_DestroyRenderer SDL_RenderClear SDL_RenderPresent SDL_RenderFillRect SDL_RenderDrawRect SDL_SetRenderDrawColor SDL_GetRendererOutputSize SDL_GetRendererInfo SDL_GetCurrentVideoDriver SDL_Delay SDL_Quit; } | sort -u | sed 's/.*/void &(void){}/' > "$STUB/sdl.c"
gen '^egl' > "$STUB/egl.c"
gen '^gl[A-Z]' > "$STUB/gles.c"
gen '^mpg123_' > "$STUB/mpg123.c"
gen '^(ov_|vorbis_|ogg_|_ov_)' > "$STUB/vorbisogg.c"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libEGL.so "$STUB/egl.c" -o "$STUB/libEGL.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so "$STUB/gles.c" -o "$STUB/libGLESv2.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libmpg123.so.0 "$STUB/mpg123.c" -o "$STUB/libmpg123.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libvorbisfile.so.3 "$STUB/vorbisogg.c" -o "$STUB/libvorbisfile.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libvorbis.so.0 "$STUB/vorbisogg.c" -o "$STUB/libvorbis.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libogg.so.0 "$STUB/vorbisogg.c" -o "$STUB/libogg.so"

SRCS="src/main.c src/setup_splash.c src/so_util_arm64.c src/util.c src/error.c \
      src/imports.c src/pthread_bridge.c src/jni_shim.c \
      src/sonic_audio.c src/egl_shim.c src/android_shim.c \
      src/opensles_shim.c src/stdio_shim.c"

"$CC" -fPIE -pie -O2 -fPIC -fno-omit-frame-pointer -rdynamic -D_GNU_SOURCE \
  -DSO_ARM64=1 \
  -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
  -Wno-comment -Wno-unused-function -Wno-unused-variable \
  -o sonic4.arm64 $SRCS \
  -Isrc -I"$HDR" -I"$HDR/SDL2" \
  -Wl,--as-needed \
  -L"$STUB" -lSDL2 -lmpg123 -lvorbisfile -lvorbis -logg -lGLESv2 -lEGL \
  -ldl -lm -lpthread -lstdc++
echo "BUILD OK -> sonic4.arm64"
file sonic4.arm64 | grep -oE 'ELF [0-9]+-bit.*aarch64'
"$READELF" -d sonic4.arm64 | grep NEEDED
