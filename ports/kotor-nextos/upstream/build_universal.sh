#!/bin/bash
# Build ARMHF multi-device do KOTOR (Aspyr/Odyssey, SDL2+FMOD+GLES2) para
# ArkOS / RK3326 (R36S) e demais CFWs de baixa glibc.
#
# Fonte estrutural: ports/asm2_127/build_buster_arkos.sh (TASM2 1.2.7d, port
# ARMHF aprovado e validado fisicamente no R36S). O contrato e o mesmo: loader
# armhf nao-PIE, so-loader resolvendo simbolos a partir do executavel, SDL2/EGL/
# GLESv2 reais vindos do firmware em runtime e substituidos por stubs no link.
#
# Este é o único runtime público: um ELF ARMHF de baixa glibc usado por todos
# os firmwares suportados, com teto GLIBC_2.30.
#
# Run (host):
#   ./build_universal.sh
set -euo pipefail

CC=arm-linux-gnueabihf-gcc
NM=arm-linux-gnueabihf-nm
OD=arm-linux-gnueabihf-objdump
REPO=/repo
SR=/sysroot
OUTPUT=kotor-nextos
BUILDER_IMAGE=codboz-armhf-builder:debian-buster
BUILDER_IMAGE_ID=sha256:58e229c82e8270fd69b0c307654e31793b781218884ce868844597ec4b8c9fef
export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

if [ ! -d "$REPO" ]; then
  # fora do container: sobe o docker com o repo e o sysroot NextOS montados
  cd "$(dirname "$0")"
  HOSTSR=$(find -H "$HOME/NextOS-Elite-Edition" -maxdepth 5 -type d \
    -path '*Amlogic-old*/toolchain/*gnueabihf/sysroot' -print | sort -V | tail -1)
  [ -d "$HOSTSR" ] || { echo "sysroot armhf (headers SDL2/EGL/GLES) nao encontrado"; exit 1; }
  ACTUAL_IMAGE_ID=$(docker image inspect "$BUILDER_IMAGE" --format '{{.Id}}')
  [ "$ACTUAL_IMAGE_ID" = "$BUILDER_IMAGE_ID" ] || {
    echo "imagem ARMHF mudou: $ACTUAL_IMAGE_ID"; exit 1;
  }
  echo "sysroot headers: $HOSTSR"
  exec docker run --rm --network none \
       -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" -e LC_ALL=C -e TZ=UTC \
       -e KOTOR_HOST_UID="$(id -u)" -e KOTOR_HOST_GID="$(id -g)" \
       -v "$PWD":/repo -v "$HOSTSR":/sysroot:ro \
       "$BUILDER_IMAGE_ID" bash "/repo/$(basename "$0")" "$@"
fi

command -v "$CC" >/dev/null 2>&1 || {
  echo "toolchain ARMHF ausente na imagem fixada"; exit 1;
}

echo "CC: $($CC --version | head -1)"
cd "$REPO"

SRCS="src/main.c src/imports.c src/so_util.c src/util.c src/error.c \
      src/pthr.c src/libc_shim.c src/softfp_shim.c src/opensles_shim.c \
      src/jni_fake.c src/kotor_language.c src/kotor_input.c \
      src/kotor_framework.c \
      vendor/nxloader/src/nxloader.c \
      vendor/nxloader/src/nxloader_elf32.c \
      vendor/nxloader/src/nxloader_elf64.c \
      vendor/nxloader/src/nxloader_hooks.c \
      vendor/nxloader/src/nxloader_protect.c \
      vendor/nxloader/src/nxloader_registry.c \
      vendor/nxcompat/src/nxcompat.c \
      vendor/nxcompat/src/nxcompat_backend.c \
      vendor/nxcompat/src/nxcompat_graphics.c \
      vendor/nxcompat/src/nxcompat_probe.c \
      vendor/nxcompat/src/nxcompat_plan.c \
      vendor/nxcompat/src/nxcompat_receipts.c \
      vendor/nxcompat/src/nxcompat_registry.c \
      vendor/nxcompat/src/nxcompat_report.c \
      vendor/nxgl/src/nxgl_diagnostics.c \
      vendor/nxgl/src/nxgl_logic.c \
      vendor/nxgl/src/nxgl_metrics.c \
      vendor/nxgl/src/nxgl_present.c \
      vendor/nxgl/src/nxgl_sdl2.c \
      vendor/nxgl/src/nxgl_nxcompat.c \
      vendor/nxinput/src/nxinput.c \
      vendor/nxinput/src/nxinput_core.c \
      vendor/nxinput/src/nxinput_nxcompat.c \
      vendor/nxaudio/src/nxaudio.c \
      vendor/nxandroid/src/nxandroid.c \
      vendor/nxandroid/src/nxandroid_imports.c"

OBJDIR=$(mktemp -d); STUB=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUB"' EXIT

# 1) objetos. -idirafter: headers da glibc BUSTER ganham dos padrao; os do
#    sysroot NextOS so resolvem SDL2/EGL/GLES (buster nao os tem).
OBJS=""
for f in $SRCS; do
  o="$OBJDIR/$(printf '%s' "$f" | tr '/.' '__').o"
  $CC -std=gnu11 -march=armv7-a -mfpu=neon -mfloat-abi=hard \
      -D_GNU_SOURCE -Isrc \
      -Ivendor/nxloader/include -Ivendor/nxloader/src \
      -Ivendor/nxcompat/include -Ivendor/nxcompat/src \
      -Ivendor/nxgl/include -Ivendor/nxgl/src \
      -Ivendor/nxinput/include -Ivendor/nxinput/src \
      -Ivendor/nxaudio/include \
      -Ivendor/nxandroid/include -Ivendor/nxandroid/src \
      -idirafter "$SR/usr/include/SDL2" -idirafter "$SR/usr/include" \
      -O2 -fPIC -fno-omit-frame-pointer \
      -Wno-int-conversion -Wno-incompatible-pointer-types \
      -Wno-implicit-function-declaration -Wno-unused-parameter \
      -Wno-unused-function -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
      -Wno-attributes \
      -c "$f" -o "$o"
  OBJS="$OBJS $o"
done

# 2) stubs .so p/ SDL2/EGL/GLESv2 (soname do device; simbolos = os que usamos)
UND=$($NM --undefined-only $OBJS 2>/dev/null | awk '{print $NF}' | sort -u)
gen() { echo "$UND" | grep -E "$1" | sed 's/.*/void &(void){}/'; }
gen '^SDL_'    > "$STUB/sdl.c";  $CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"
gen '^egl'     > "$STUB/egl.c";  $CC -shared -fPIC -nostdlib -Wl,-soname,libEGL.so.1      "$STUB/egl.c" -o "$STUB/libEGL.so"
gen '^gl[A-Z]' > "$STUB/gl.c";   $CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so.2   "$STUB/gl.c"  -o "$STUB/libGLESv2.so"

# 3) link final. NAO-PIE (Type: EXEC, igual ao binario validado no Mali-450) +
#    --export-dynamic (o so-loader resolve simbolos a partir do executavel).
$CC -no-pie -Wl,--export-dynamic -Wl,--no-as-needed -o "$OUTPUT" $OBJS \
    -L"$STUB" -L/usr/arm-linux-gnueabihf/lib \
    -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread -Wl,-l:libstdc++.so.6

for b in "$OUTPUT"; do
  MAXV=$($OD -T "$b" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
  echo "BUSTER BUILD OK -> $b"
  echo "  $(arm-linux-gnueabihf-readelf -h "$b" | grep -E 'Type:|Machine:' | tr -s ' ' | tr '\n' ' ')"
  echo "  glibc max = $MAXV   (runtime publico: <= GLIBC_2.30)"
done

if [ -n "${KOTOR_HOST_UID:-}" ] && [ -n "${KOTOR_HOST_GID:-}" ]; then
  chown "$KOTOR_HOST_UID:$KOTOR_HOST_GID" "$OUTPUT" 2>/dev/null || true
fi
