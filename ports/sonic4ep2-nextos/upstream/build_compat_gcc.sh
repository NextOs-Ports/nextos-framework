#!/bin/bash
# Build Sonic 4 EP2 armhf loader with old glibc symbols for PortMaster packages.
#
# Run from host:
#   SR=$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/armv8a-emuelec-linux-gnueabihf/sysroot
#   sudo docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro debian:buster bash /repo/build_compat_gcc.sh
#
# The resulting binary must not need a bundled runtime. Device provides real SDL2,
# EGL/GLES, mpg123/vorbis/ogg, libc, libm and libstdc++.
set -e

CC=arm-linux-gnueabihf-gcc
NM=arm-linux-gnueabihf-nm
READELF=arm-linux-gnueabihf-readelf
REPO=/repo
SR=/sysroot

if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  printf "deb http://archive.debian.org/debian buster main\n" > /etc/apt/sources.list
  printf "deb http://archive.debian.org/debian-security buster/updates main\n" >> /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq
  apt-get install -y -qq gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf binutils-arm-linux-gnueabihf >/dev/null
fi

echo "CC: $($CC --version | head -1)"
ldd --version | head -1

cd "$REPO"
[ -f ./sonic4 ] || { echo "preciso do ./sonic4 native para extrair simbolos dos stubs"; exit 1; }

HDR=$(mktemp -d)
STUB=$(mktemp -d)
trap 'rm -rf "$HDR" "$STUB"' EXIT

for d in SDL2 EGL KHR GLES GLES2 GLES3 vorbis ogg; do
  [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"
done
for f in mpg123.h fmt123.h; do
  [ -f "$SR/usr/include/$f" ] && cp "$SR/usr/include/$f" "$HDR/$f"
done

gen() {
  "$NM" -D --undefined-only ./sonic4 |
    awk '{print $NF}' |
    grep -E "$1" |
    sort -u |
    sed 's/.*/void &(void){}/'
}

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

SRCS="src/main.c src/setup_splash.c src/so_util.c src/util.c src/error.c \
      src/imports.c src/pthread_bridge.c src/jni_shim.c \
      src/sonic_audio.c \
      src/egl_shim.c src/android_shim.c \
      src/opensles_shim.c src/stdio_shim.c src/softfp_shim.c"

"$CC" -fPIE -pie -O2 -fPIC -fno-omit-frame-pointer -rdynamic -D_GNU_SOURCE \
  -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
  -Wno-comment -Wno-unused-function -Wno-unused-variable \
  -o sonic4.compat.gcc $SRCS \
  -Isrc -I"$HDR" -I"$HDR/SDL2" \
  -Wl,--as-needed \
  -L"$STUB" -lSDL2 -lmpg123 -lvorbisfile -lvorbis -logg -lGLESv2 -lEGL \
  -ldl -lm -lpthread -lstdc++

echo "BUILD OK -> sonic4.compat.gcc"
echo "  GLIBC max:   $($READELF -V sonic4.compat.gcc | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "  GLIBCXX max: $($READELF -V sonic4.compat.gcc | grep -oE 'GLIBCXX_[0-9.]+' | sed 's/GLIBCXX_//' | sort -uV | tail -1)"
echo "  tamanho:     $(stat -c%s sonic4.compat.gcc) bytes"
echo "  needed:"
"$READELF" -d sonic4.compat.gcc | grep NEEDED
