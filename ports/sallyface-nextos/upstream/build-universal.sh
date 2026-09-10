#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Build publica e deterministica do loader AArch64 do Sally Face.
#
# O toolchain cruzado do Debian Buster fixado mantem o executavel em
# GLIBC <= 2.30, teto do pacote universal. Particularidades deste port:
#   - SDL2 (nao SDL3): baseline PortMaster, presente em todo CFW. Entra aqui so'
#     como STUB que grava o SONAME (libSDL2-2.0.so.0); a lib real e' a do
#     firmware, resolvida em runtime pelo LD_LIBRARY_PATH do launcher.
#   - GLES2/EGL NAO viram DT_NEEDED: o proprio egl.c/egl_sdl.c faz dlopen e
#     seleciona o provedor por medicao (KMSDRM/Wayland vs fbdev). Nenhuma
#     libEGL/libGLESv2/libmali aparece no NEEDED.
#   - libgcc_s dobrado no binario (-static-libgcc) para manter o whitelist limpo.
#   - Headers (SDL2/EGL/GLES2/zlib) vem de um sysroot AArch64 montado read-only.
#   - Os modulos de runtime compilados junto (nxinput 0.5.1, nxcompat 0.3.0 e
#     nxgl 0.2.17) estao vendorizados em vendor/ neste mesmo repositorio.
#
# Uso:  NEXTOS_SYSROOT=<sysroot aarch64 com SDL2/EGL/GLES2/zlib> ./build-universal.sh
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NXINPUT_DIR=${NXINPUT_DIR:-$PORT_DIR/vendor/nxinput}
NXGL_DIR=${NXGL_DIR:-$PORT_DIR/vendor/nxgl}
NXCOMPAT_DIR=${NXCOMPAT_DIR:-$PORT_DIR/vendor/nxcompat}
if [ "${SF_BUSTER_IN_CONTAINER:-0}" = "1" ]; then
  NXINPUT_DIR=/repo/vendor/nxinput
  NXGL_DIR=/repo/vendor/nxgl
  NXCOMPAT_DIR=/repo/vendor/nxcompat
fi
OUTPUT=${SF_UNIVERSAL_OUTPUT:-sallyface-nextos}
BUILDER_IMAGE=playfetch-builder:buster
BUILDER_IMAGE_ID=sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf
export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

if [ "${SF_BUSTER_IN_CONTAINER:-0}" != "1" ]; then
  NEXTOS_SYSROOT=${NEXTOS_SYSROOT:-}
  [ -n "$NEXTOS_SYSROOT" ] ||
    { echo "defina NEXTOS_SYSROOT: sysroot aarch64 com SDL2, EGL, GLES2 e zlib" >&2; exit 1; }
  for header in usr/include/zlib.h usr/include/GLES2/gl2.h \
                usr/include/EGL/egl.h usr/include/SDL2/SDL.h; do
    [ -f "$NEXTOS_SYSROOT/$header" ] ||
      { echo "header ausente no sysroot: $header" >&2; exit 1; }
  done
  command -v docker >/dev/null 2>&1 ||
    { echo "docker e' necessario para a build GLIBC <= 2.30" >&2; exit 1; }
  ACTUAL_IMAGE_ID=$(docker image inspect "$BUILDER_IMAGE" --format '{{.Id}}' 2>/dev/null) ||
    { echo "imagem offline ausente: $BUILDER_IMAGE" >&2; exit 1; }
  [ "$ACTUAL_IMAGE_ID" = "$BUILDER_IMAGE_ID" ] || {
    echo "imagem do builder mudou: $ACTUAL_IMAGE_ID (esperado $BUILDER_IMAGE_ID)" >&2
    exit 1
  }

  docker run --rm --network none \
    -e SF_BUSTER_IN_CONTAINER=1 \
    -e SF_UNIVERSAL_OUTPUT="$OUTPUT" \
    -e SF_HOST_UID="$(id -u)" \
    -e SF_HOST_GID="$(id -g)" \
    -e LC_ALL=C -e TZ=UTC -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    -v "$PORT_DIR":/repo \
    -v "$NEXTOS_SYSROOT":/nxsr:ro \
    "$BUILDER_IMAGE_ID" \
    bash /repo/build-universal.sh
  exit 0
fi

for tool in aarch64-linux-gnu-gcc aarch64-linux-gnu-nm aarch64-linux-gnu-readelf; do
  command -v "$tool" >/dev/null 2>&1 ||
    { echo "ferramenta ausente na imagem fixada: $tool" >&2; exit 1; }
done

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
cd /repo

OBJDIR=$(mktemp -d)
STUBDIR=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUBDIR"' EXIT

OBJS=()
for source in src/*.c "$NXINPUT_DIR"/src/nxinput_gptk.c \
              "$NXINPUT_DIR"/src/nxinput_gptk_motion.c \
              "$NXCOMPAT_DIR"/src/nxcompat_settings.c \
              "$NXGL_DIR"/src/nxgl_quality.c \
              "$NXGL_DIR"/adapters/nxgl_frame_proof_adapter.c; do
  object="$OBJDIR/$(basename "${source%.c}").o"
  "$CC" -D_GNU_SOURCE -std=gnu11 \
    -I src -I "$NXINPUT_DIR/include" -I "$NXCOMPAT_DIR/include" \
    -I "$NXGL_DIR/include" -I "$NXGL_DIR/adapters" \
    -idirafter /nxsr/usr/include \
    -O2 -fPIE -fno-strict-aliasing -fno-omit-frame-pointer \
    -Wno-unused-parameter -Wno-unused-result \
    -c "$source" -o "$object"
  OBJS+=("$object")
done

# ---- stubs de link: gravam o SONAME certo sem trazer lib nenhuma ----
# libSDL2-2.0.so.0 e' do firmware (baseline PortMaster); libz.so.1 idem.
UNDEFINED=$("$NM" --undefined-only "${OBJS[@]}" 2>/dev/null | awk '{print $NF}' | sort -u)

stub_lib() {
  stub_out=$1; stub_soname=$2; stub_regex=$3
  : > "$STUBDIR/$stub_out.c"
  for symbol in $(printf '%s\n' "$UNDEFINED" | grep -E "$stub_regex" || true); do
    printf 'void %s(void) {}\n' "$symbol" >> "$STUBDIR/$stub_out.c"
  done
  "$CC" -shared -fPIC -nostdlib -Wl,-soname,"$stub_soname" \
    "$STUBDIR/$stub_out.c" -o "$STUBDIR/lib$stub_out.so"
}
stub_lib SDL2 libSDL2-2.0.so.0 '^SDL_'
stub_lib z    libz.so.1        '^(inflate|deflate|uncompress|compress|crc32|adler32|zlib|zError|gz)'

"$CC" -fPIE -pie -rdynamic -static-libgcc -o "$OUTPUT" "${OBJS[@]}" \
  -L"$STUBDIR" -lSDL2 -lz -ldl -lm -lpthread

# Sem RPATH/RUNPATH: o launcher poe as libs do firmware no LD_LIBRARY_PATH.

# ---- auditoria do TLS guard (TPIDR_EL0+0x28 estavel) ----
guard_offset=$("$READELF" -sW "$OUTPUT" |
  awk '$8 == "g_bionic_guard_pad" && !found { value = $2; found = 1 }
       END { if (found) print value }')
[ "$guard_offset" = 0000000000000000 ] || {
  echo "FALHA: TLS guard fora do offset zero: $guard_offset" >&2; exit 1; }

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
ALLOWED='^(libSDL2-2\.0\.so\.0|libz\.so\.1|libdl\.so\.2|libm\.so\.6|libpthread\.so\.0|libc\.so\.6|ld-linux-aarch64\.so\.1)$'
BAD=$("$READELF" -d "$OUTPUT" | awk '/NEEDED/ {gsub(/[][]/,"",$NF); print $NF}' |
  grep -Ev "$ALLOWED" || true)
if [ -n "$BAD" ]; then
  echo "FALHA: DT_NEEDED fora da linha de base universal:" >&2
  printf '  %s\n' $BAD >&2
  exit 1
fi

chown "${SF_HOST_UID:-0}:${SF_HOST_GID:-0}" "$OUTPUT" 2>/dev/null || true
printf '%s universal: GLIBC max %s, TLS guard offset 0\n' "$OUTPUT" "$MAX_GLIBC"
"$READELF" -d "$OUTPUT" | awk '/NEEDED/ {gsub(/[][]/,"",$NF); printf "  NEEDED %s\n", $NF}'
