#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Build universal aarch64 do Beach Buggy Racing (loader so-loader da engine Vu).
#
# Molde: ports/ff4/build_universal.sh (aprovado na onda v2). O toolchain
# cruzado do Debian Buster mantem o executavel em GLIBC <= 2.30, teto do
# pacote publico. Diferencas deste port:
#   - SDL3 nao existe em CFW nenhuma: a libSDL3.so.0 universal (Buster,
#     KMSDRM+ALSA+Pulse) e' BUNDLADA no pacote e entra aqui so' como stub que
#     grava o SONAME; o launcher poe lib/ no LD_LIBRARY_PATH.
#   - GLES2 NAO vira DT_NEEDED: framework/nxgl/src/nxgl_gles2.c resolve os
#     142 pontos de entrada em runtime (despacho estavel p/ o GOT da engine).
#   - EGL NAO vira DT_NEEDED: ponte local src/egl_bridge.c (caminho legado).
#
# Uso no host:  ./build_universal.sh
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUTPUT=${BB_UNIVERSAL_OUTPUT:-beachbuggy}
BUILDER_IMAGE=playfetch-builder:buster
BUILDER_IMAGE_ID=sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf
export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

if [ "${BB_BUSTER_IN_CONTAINER:-0}" != "1" ]; then
  REPOSITORY_ROOT=$(git -C "$PORT_DIR" rev-parse --show-toplevel)
  SDL3_SOURCE=${BB_SDL3_SOURCE:-"$HOME/SDL3-342"}
  [ -f "$SDL3_SOURCE/include/SDL3/SDL.h" ] ||
    { echo "fonte SDL3 com headers nao encontrada: $SDL3_SOURCE" >&2; exit 1; }
  NEXTOS_ROOT=${NEXTOS_ROOT:-"$HOME/NextOS-Elite-Edition"}
  NEXTOS_TOOLCHAIN=""
  for candidate in $(
    find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
      -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
      -print | sort -V -r
  ); do
    if [ -f "$candidate/aarch64-libreelec-linux-gnu/sysroot/usr/include/zlib.h" ] &&
       [ -f "$candidate/aarch64-libreelec-linux-gnu/sysroot/usr/include/GLES2/gl2.h" ]; then
      NEXTOS_TOOLCHAIN=$candidate
      break
    fi
  done
  [ -n "$NEXTOS_TOOLCHAIN" ] ||
    { echo "toolchain NextOS com headers zlib+GLES2 nao encontrado em $NEXTOS_ROOT" >&2; exit 1; }
  NEXTOS_SYSROOT=$NEXTOS_TOOLCHAIN/aarch64-libreelec-linux-gnu/sysroot
  command -v docker >/dev/null 2>&1 ||
    { echo "docker e' necessario para a build GLIBC <= 2.30" >&2; exit 1; }
  ACTUAL_IMAGE_ID=$(docker image inspect "$BUILDER_IMAGE" --format '{{.Id}}' 2>/dev/null) ||
    { echo "imagem offline ausente: $BUILDER_IMAGE" >&2; exit 1; }
  [ "$ACTUAL_IMAGE_ID" = "$BUILDER_IMAGE_ID" ] || {
    echo "imagem do builder mudou: $ACTUAL_IMAGE_ID (esperado $BUILDER_IMAGE_ID)" >&2
    exit 1
  }

  exec docker run --rm --network none \
    -e BB_BUSTER_IN_CONTAINER=1 \
    -e BB_UNIVERSAL_OUTPUT="$OUTPUT" \
    -e BB_HOST_UID="$(id -u)" \
    -e BB_HOST_GID="$(id -g)" \
    -e LC_ALL=C -e TZ=UTC -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    -v "$PORT_DIR":/repo \
    -v "$REPOSITORY_ROOT/framework":/framework:ro \
    -v "$SDL3_SOURCE":/sdl3:ro \
    -v "$NEXTOS_SYSROOT":/nxsr:ro \
    "$BUILDER_IMAGE_ID" \
    bash /repo/build_universal.sh
fi

for tool in aarch64-linux-gnu-gcc aarch64-linux-gnu-nm aarch64-linux-gnu-readelf; do
  command -v "$tool" >/dev/null 2>&1 ||
    { echo "ferramenta ausente na imagem fixada: $tool" >&2; exit 1; }
done

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
FRAMEWORK_ROOT=${BB_FRAMEWORK_ROOT:-/framework}
cd /repo

OBJDIR=$(mktemp -d)
STUBDIR=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUBDIR"' EXIT

COMMON_INCLUDES=(
  -I src
  -I "$FRAMEWORK_ROOT/nxgl/include"
  -I "$FRAMEWORK_ROOT/nxgl/adapters"
  -I /sdl3/include
)

OBJS=()
compile_source() {
  group=$1
  source=$2
  object="$OBJDIR/${group}_$(basename "${source%.c}").o"
  "$CC" -D_GNU_SOURCE -std=gnu11 "${COMMON_INCLUDES[@]}" \
    -idirafter /sdl3/src/video/khronos \
    -idirafter /nxsr/usr/include \
    -O2 -fPIC -fno-omit-frame-pointer \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-discarded-qualifiers -Wno-unused-parameter -Wno-unused-function \
    -Wno-unused-result \
    -c "$source" -o "$object"
  OBJS+=("$object")
}

for source in src/*.c; do
  compile_source port "$source"
done
for source in \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_gles2.c \
  "$FRAMEWORK_ROOT"/nxgl/adapters/nxgl_frame_proof_adapter.c; do
  compile_source nxgl "$source"
done

# ---- stubs de link: gravam o SONAME certo sem trazer lib nenhuma ----
# libSDL3.so.0 vem BUNDLADA no pacote (lib/); libz.so.1 e' do firmware.
UNDEFINED=$("$NM" --undefined-only "${OBJS[@]}" 2>/dev/null | awk '{print $NF}' | sort -u)

stub_lib() {
  stub_name=$1; stub_soname=$2; stub_regex=$3
  : > "$STUBDIR/$stub_name.c"
  for symbol in $(printf '%s\n' "$UNDEFINED" | grep -E "$stub_regex" || true); do
    printf 'void %s(void) {}\n' "$symbol" >> "$STUBDIR/$stub_name.c"
  done
  "$CC" -shared -fPIC -nostdlib -Wl,-soname,"$stub_soname" \
    "$STUBDIR/$stub_name.c" -o "$STUBDIR/lib$stub_name.so"
}
stub_lib SDL3 libSDL3.so.0 '^SDL_'
stub_lib z    libz.so.1    '^(inflate|deflate|uncompress|compress|crc32|adler32|zlib|zError|gz)'

"$CC" -fPIE -pie -rdynamic -o "$OUTPUT" "${OBJS[@]}" \
  -L"$STUBDIR" -lSDL3 -lz -ldl -lm -lpthread

# Sem RPATH/RUNPATH: o launcher poe lib/ (SDL3 bundlada) no LD_LIBRARY_PATH.

# ---- trava 1: GLIBC <= 2.30 ----
MAX_GLIBC=$("$READELF" --version-info "$OUTPUT" |
  grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sort -Vu | tail -1)
[ -n "$MAX_GLIBC" ] || { echo "nao foi possivel determinar a versao GLIBC" >&2; exit 1; }
version_number=${MAX_GLIBC#GLIBC_}
major=${version_number%%.*}; rest=${version_number#*.}; minor=${rest%%.*}
if [ "$major" -gt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -gt 30 ]; }; then
  echo "FALHA: $OUTPUT exige $MAX_GLIBC (limite GLIBC_2.30)" >&2
  exit 1
fi

# ---- trava 2: nenhum DT_NEEDED fora da linha de base universal ----
# GL/EGL aqui significaria regressao: o port nao abriria no Mali G31.
ALLOWED='^(libSDL3\.so\.0|libz\.so\.1|libdl\.so\.2|libm\.so\.6|libpthread\.so\.0|libc\.so\.6|ld-linux-aarch64\.so\.1)$'
BAD=$("$READELF" -d "$OUTPUT" | awk '/NEEDED/ {gsub(/[][]/,"",$NF); print $NF}' |
  grep -Ev "$ALLOWED" || true)
if [ -n "$BAD" ]; then
  echo "FALHA: DT_NEEDED fora da linha de base universal:" >&2
  printf '  %s\n' $BAD >&2
  exit 1
fi

chown "${BB_HOST_UID:-0}:${BB_HOST_GID:-0}" "$OUTPUT" 2>/dev/null || true
printf '%s universal: GLIBC max %s\n' "$OUTPUT" "$MAX_GLIBC"
"$READELF" -d "$OUTPUT" | awk '/NEEDED/ {gsub(/[][]/,"",$NF); printf "  NEEDED %s\n", $NF}'
