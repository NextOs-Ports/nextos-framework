#!/bin/bash
# Reproducible public AArch64 build for Retro City Rampage DX.
# Debian Buster fixes the Linux ABI below the ArkOS GLIBC_2.30 ceiling;
# the mounted NextOS sysroot supplies API headers and static zlib only.
set -e

REPO=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)

if [ -z "${IN_CONTAINER:-}" ]; then
  command -v docker >/dev/null || {
    echo "docker not found; use IN_CONTAINER=1 inside Debian Buster"
    exit 1
  }
  if [ -z "${SYSROOT:-}" ]; then
    SYSROOT=$(ls -d ${NEXTOS_SYSROOT:-/opt/nextos/sysroot} \
                   "$HOME"/NextOS-Elite-Edition/build*Amlogic-old*/toolchain/aarch64-*/sysroot 2>/dev/null | head -1)
  fi
  [ -d "$SYSROOT/usr/include/SDL2" ] || {
    echo "set SYSROOT= to an AArch64 sysroot carrying SDL2/EGL/GLES2 headers"
    exit 1
  }
  BUILDER_IMAGE=${BUILDER_IMAGE:-debian@sha256:58ce6f1271ae1c8a2006ff7d3e54e9874d839f573d8009c20154ad0f2fb0a225}
  exec docker run --rm \
    -v "$REPO":/repo -v "$SYSROOT":/sysroot:ro \
    -e IN_CONTAINER=1 -e SR=/sysroot \
    "$BUILDER_IMAGE" bash /repo/build_universal.sh
fi

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
STRIP=aarch64-linux-gnu-strip
SR=${SR:-/sysroot}
cd "$REPO"

if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  printf 'deb [trusted=yes] http://archive.debian.org/debian buster main\n' > /etc/apt/sources.list
  printf 'deb [trusted=yes] http://archive.debian.org/debian-security buster/updates main\n' >> /etc/apt/sources.list
  dpkg --add-architecture arm64
  apt-get -o Acquire::Check-Valid-Until=false \
          -o Acquire::AllowInsecureRepositories=true update -qq
  apt-get install -y -qq gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
                        zlib1g-dev:arm64 >/dev/null
fi

HDR=$(mktemp -d); STUB=$(mktemp -d); OBJDIR=$(mktemp -d)
trap 'rm -rf "$HDR" "$STUB" "$OBJDIR"' EXIT

for d in SDL2 EGL KHR GLES GLES2 GLES3; do
  [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"
done
for h in zlib.h zconf.h; do
  [ -f "$SR/usr/include/$h" ] && cp "$SR/usr/include/$h" "$HDR/$h"
done
[ -d "$HDR/SDL2" ] && [ -f "$HDR/zlib.h" ] && [ -f "$SR/usr/lib/libz.a" ] || {
  echo "header/static-zlib sysroot is incomplete"
  exit 1
}

SRCS="src/main.c src/nx_elf.c src/bionic.c src/pthread_bridge.c \
      src/jni.c src/sdl_java.c src/android.c src/egl.c src/video.c \
      src/input.c src/audio.c src/probe_ring.c src/nx_frameprobe.c \
      src/nxgl_frame_proof_adapter.c"
OBJS=""
for f in $SRCS; do
  o="$OBJDIR/$(basename "$f").o"
  $CC -O2 -g0 -std=gnu11 -fno-strict-aliasing -D_GNU_SOURCE \
      -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
      -Wno-unused-variable -Wno-comment -Wno-int-conversion \
      -Wno-incompatible-pointer-types -Isrc -idirafter "$HDR" \
      -c "$f" -o "$o"
  OBJS="$OBJS $o"
done

UND=$($NM --undefined-only $OBJS 2>/dev/null | awk '{print $NF}' | sort -u)
gen() { echo "$UND" | grep -E "$1" | sed 's/.*/void &(void){}/'; }
gen '^SDL_'    > "$STUB/sdl.c"
gen '^egl'     > "$STUB/egl.c"
gen '^gl[A-Z]' > "$STUB/gl.c"
$CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libEGL.so.1 "$STUB/egl.c" -o "$STUB/libEGL.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so.2 "$STUB/gl.c" -o "$STUB/libGLESv2.so"

$CC -rdynamic -Wl,--export-dynamic -o rcrdx-nextos $OBJS \
    -L"$STUB" -lSDL2 -lEGL -lGLESv2 "$SR/usr/lib/libz.a" \
    -ldl -lm -lpthread
$STRIP --strip-unneeded rcrdx-nextos

MAXGLIBC=$($READELF -V rcrdx-nextos | grep -oE 'GLIBC_[0-9.]+' |
             sed 's/GLIBC_//' | sort -uV | tail -1)
echo "BUILD OK -> rcrdx-nextos (GLIBC_$MAXGLIBC)"
case "$MAXGLIBC" in
  2.1[0-9]|2.2[0-9]|2.30) ;;
  *) echo "GLIBC gate failed: $MAXGLIBC > 2.30"; exit 1 ;;
esac
if $READELF -Ws rcrdx-nextos | grep -Eq ' UND +reallocarray(@|$)'; then
  echo "ABI gate failed: reallocarray must be provided by the adapter"
  exit 1
fi
