#!/bin/bash
# Build ARMHF multi-device do asm2_127 (The Amazing Spider-Man 2 1.2.7d) para
# ArkOS / RK3326 (R36S), NextOS e demais CFWs com glibc >= 2.28.
#
# Esta e a receita publica/multi-device. O mesmo ELF de baixa glibc pode ser
# empacotado nas rotas NextOS e PortMaster; build.sh permanece apenas como
# variante local para o sysroot atual do NextOS.
#
# Run (host):
#   SR=/absolute/path/to/a/sysroot/with/SDL2-EGL-GLES-headers
#   sudo docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro debian:buster bash /repo/build_buster_arkos.sh
#
# Headers SDL2/EGL/GLES sao arch-neutros -> vem do sysroot NextOS (/sysroot).
# glibc/libgcc vem do buster. SDL2/EGL/GLESv2 reais sao do device em runtime;
# no link usamos STUBS gerados dos undefined (mesmo padrao do smhd/sonic4).
set -e

export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

CC=arm-linux-gnueabihf-gcc
NM=arm-linux-gnueabihf-nm
OD=arm-linux-gnueabihf-objdump
STRIP=arm-linux-gnueabihf-strip
REPO=/repo
SR=/sysroot

if ! command -v "$CC" >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  printf 'deb http://archive.debian.org/debian buster main\n' > /etc/apt/sources.list
  printf 'deb http://archive.debian.org/debian-security buster/updates main\n' >> /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq >/dev/null
  apt-get install -y -qq gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf \
                        binutils-arm-linux-gnueabihf file >/dev/null
fi

echo "CC: $($CC --version | head -1)"
cd "$REPO"

SRCS="src/main.c src/imports.c src/so_util.c src/util.c src/error.c \
      src/bionic_compat.c src/softfp_bridge.c src/platform_shims.c \
      src/opensl_bridge.c \
      src/pthread_bridge.c src/jni_bridge.c src/android_callbacks.c \
      src/installer_compat.c src/startup_compat.c src/audio_compat.c \
      src/shop_compat.c \
      src/video.c src/first_accept.c src/input.c \
      src/setjmp_bridge.S"

OBJDIR=$(mktemp -d); STUB=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUB"' EXIT

# 1) objetos. -idirafter: headers da glibc BUSTER ganham nos padrao; os do
#    sysroot NextOS so resolvem SDL2/EGL/GLES (buster nao os tem).
OBJS=""
for f in $SRCS; do
  o="$OBJDIR/$(basename "$f").o"
  $CC -std=gnu11 -march=armv7-a -mfpu=neon -mfloat-abi=hard \
      -D_GNU_SOURCE -DASM2_ARMHF_AUDIO_ALSA_FALLBACK=1 -Isrc \
      -idirafter "$SR/usr/include/SDL2" -idirafter "$SR/usr/include" \
      -O2 -fPIC -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter \
      -c "$f" -o "$o"
  OBJS="$OBJS $o"
done

# 2) stubs .so p/ SDL2/EGL/GLESv2 (soname do device; simbolos = os que usamos)
UND=$($NM --undefined-only $OBJS 2>/dev/null | awk '{print $NF}' | sort -u)
gen() { echo "$UND" | grep -E "$1" | sed 's/.*/void &(void){}/'; }
gen '^SDL_'    > "$STUB/sdl.c";  $CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c"  -o "$STUB/libSDL2.so"
gen '^egl'     > "$STUB/egl.c";  $CC -shared -fPIC -nostdlib -Wl,-soname,libEGL.so        "$STUB/egl.c"  -o "$STUB/libEGL.so"
gen '^gl[A-Z]' > "$STUB/gl.c";   $CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so     "$STUB/gl.c"   -o "$STUB/libGLESv2.so"

# 3) link final. NAO-PIE (Type: EXEC, igual ao binario validado no Mali-450) +
#    --export-dynamic (o so-loader resolve simbolos a partir do executavel).
$CC -no-pie -Wl,--export-dynamic -Wl,--no-as-needed -o asm2_127-universal $OBJS \
    -L"$STUB" -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread
$STRIP --strip-unneeded asm2_127-universal

MAXV=$($OD -T asm2_127-universal 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
[ -n "$MAXV" ] || {
  echo "release publica sem versao GLIBC auditavel" >&2
  exit 1
}
MAXNUM=${MAXV#GLIBC_}
HIGHEST=$(printf '%s\n%s\n' 2.30 "$MAXNUM" | sort -V | tail -1)
[ "$HIGHEST" = 2.30 ] || {
  echo "build ARMHF excede o teto publico: $MAXV" >&2
  exit 1
}
echo "BUSTER BUILD OK -> asm2_127-universal"
echo "  $(file asm2_127-universal | cut -d, -f1-4)"
echo "  glibc max = $MAXV   (release publica: <= GLIBC_2.30)"
