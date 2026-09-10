#!/bin/bash
# Build UNIVERSAL (release publica) do Castle of Illusion — AArch64, GLIBC <= 2.30.
#
# Roda dentro de um container debian:buster porque o piso publico e' a glibc do
# ArkOS (2.30) e buster entrega 2.28. Todo ELF nosso que entra no ZIP publico
# passa por aqui; build ligado a glibc corrente do NextOS e' artefato SEPARADO.
#
# Uso (host):
#   ./build_universal.sh                      # cuida do docker sozinho
#   IN_CONTAINER=1 ./build_universal.sh       # ja dentro do buster
#
# Headers SDL2/EGL/GLES2 sao arch-neutros e vem do proprio repo (src/khr/).
# As libs reais vem do device em runtime; no link usamos STUBS gerados a partir
# de src/runtime-symbols.txt — a lista e' versionada para o build nao depender
# de nenhum toolchain privado nem de um binario pre-existente.
set -e

REPO=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)

if [ -z "${IN_CONTAINER:-}" ]; then
  command -v docker >/dev/null ||
    { echo "docker nao encontrado; use IN_CONTAINER=1 dentro de um buster"; exit 1; }
  [ -n "${SYSROOT:-}" ] || {
    echo "defina SYSROOT= com um sysroot AArch64 que tenha os headers SDL2/EGL/GLES2"
    exit 1
  }
  exec docker run --rm \
    -v "$REPO":/repo -v "$SYSROOT":/sysroot:ro \
    -e IN_CONTAINER=1 -e SR=/sysroot \
    debian:buster bash /repo/build_universal.sh
fi

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
SR=${SR:-/sysroot}
cd "$REPO"

if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  printf "deb http://archive.debian.org/debian buster main\n" > /etc/apt/sources.list
  printf "deb http://archive.debian.org/debian-security buster/updates main\n" >> /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq
  apt-get install -y -qq gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu >/dev/null
fi

echo "CC: $($CC --version | head -1)"

HDR=$(mktemp -d); STUB=$(mktemp -d)
trap 'rm -rf "$HDR" "$STUB"' EXIT

for d in SDL2 EGL KHR GLES GLES2 GLES3; do
  [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"
done
[ -d "$HDR/SDL2" ] || { echo "sysroot sem headers SDL2 em $SR/usr/include"; exit 1; }

# Stubs de link a partir da lista VERSIONADA (nao de um binario existente).
gen() { grep -E "$1" src/runtime-symbols.txt | sed 's/.*/void &(void){}/'; }
gen '^SDL_'   > "$STUB/sdl.c"
gen '^egl'    > "$STUB/egl.c"
gen '^gl[A-Z]' > "$STUB/gles.c"

"$CC" -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libEGL.so "$STUB/egl.c" -o "$STUB/libEGL.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so "$STUB/gles.c" -o "$STUB/libGLESv2.so"

SRCS="src/main.c src/so_util.c src/imports.c src/pthread_bridge.c \
      src/egl_shim.c src/android_shim.c src/opensles_shim.c src/jni_shim.c src/coi_shims.c \
      src/etc2_decode.c src/etc1_encode.c src/util.c src/error.c src/coi_profile.c src/coi_physics.c"

"$CC" -fPIE -pie -O2 -fPIC -fno-omit-frame-pointer -rdynamic -D_GNU_SOURCE \
  -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
  -Wno-comment -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
  -o castleofillusion $SRCS \
  -Isrc -I"$HDR" -I"$HDR/SDL2" \
  -Wl,--export-dynamic -Wl,--as-needed \
  -L"$STUB" -lSDL2 -lEGL -lGLESv2 \
  -ldl -lm -lpthread -lgcc

MAXGLIBC=$("$READELF" -V castleofillusion | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)
echo "BUILD OK -> castleofillusion"
echo "  arch:      $(file castleofillusion 2>/dev/null | cut -d, -f1-3)"
echo "  GLIBC max: $MAXGLIBC"
echo "  tamanho:   $(stat -c%s castleofillusion) bytes"
"$READELF" -d castleofillusion | grep NEEDED

# GATE: acima de 2.30 o binario nao pode entrar no ZIP publico.
case "$MAXGLIBC" in
  2.1[0-9]|2.2[0-9]|2.30) echo "GATE GLIBC OK ($MAXGLIBC <= 2.30)" ;;
  *) echo "GATE GLIBC REPROVADO: $MAXGLIBC > 2.30"; exit 1 ;;
esac
