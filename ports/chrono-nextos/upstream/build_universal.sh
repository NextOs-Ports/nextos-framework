#!/usr/bin/env bash
# Build do binario universal AArch64 do Chrono Trigger para a release publica.
#
# Molde estrutural: ports/huntdown/build_r36s.sh e ports/hitmango/build_universal.sh
# (ambos aprovados). O toolchain cruzado do Debian Buster mantem o executavel em
# GLIBC <= 2.30, que e' o teto do pacote publico (runtime-abi-glibc.md). SDL2,
# GLESv2 e FreeType sao fornecidos pelo FIRMWARE do aparelho: sao ligados apenas
# contra stubs que gravam o SONAME correto, nunca contra as libs do sysroot
# NextOS (glibc 2.43). O sysroot NextOS entra somente-leitura so' por HEADERS.
#
# Uso no host:   ./build_universal.sh
# Dentro do container (manual): CT_BUSTER_IN_CONTAINER=1 ./build_universal.sh
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUTPUT=${CT_UNIVERSAL_OUTPUT:-chrono-nextos}
BUILDER_IMAGE=playfetch-builder:buster
BUILDER_IMAGE_ID=sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf
export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

if [ "${CT_BUSTER_IN_CONTAINER:-0}" != "1" ]; then
  NEXTOS_ROOT=${NEXTOS_ROOT:-"$HOME/NextOS-Elite-Edition"}
  # Only a toolchain whose sysroot really carries the SDL2 headers is usable:
  # an in-progress NextOS build leaves a skeleton toolchain that sorts last.
  NEXTOS_TOOLCHAIN=""
  for candidate in $(
    find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
      -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
      -print | sort -V -r
  ); do
    if [ -f "$candidate/aarch64-libreelec-linux-gnu/sysroot/usr/include/SDL2/SDL.h" ]; then
      NEXTOS_TOOLCHAIN=$candidate
      break
    fi
  done
  [ -n "$NEXTOS_TOOLCHAIN" ] ||
    { echo "toolchain NextOS atual nao encontrado em $NEXTOS_ROOT" >&2; exit 1; }
  NEXTOS_SYSROOT=$NEXTOS_TOOLCHAIN/aarch64-libreelec-linux-gnu/sysroot
  [ -d "$NEXTOS_SYSROOT" ] ||
    { echo "sysroot NextOS nao encontrado: $NEXTOS_SYSROOT" >&2; exit 1; }
  command -v docker >/dev/null 2>&1 ||
    { echo "docker e' necessario para a build GLIBC <= 2.30" >&2; exit 1; }
  ACTUAL_IMAGE_ID=$(docker image inspect "$BUILDER_IMAGE" \
    --format '{{.Id}}' 2>/dev/null) ||
    { echo "imagem offline M17 ausente: $BUILDER_IMAGE" >&2; exit 1; }
  [ "$ACTUAL_IMAGE_ID" = "$BUILDER_IMAGE_ID" ] || {
    echo "imagem M17 mudou: $ACTUAL_IMAGE_ID (esperado $BUILDER_IMAGE_ID)" >&2
    exit 1
  }

  exec docker run --rm --network none \
    -e CT_BUSTER_IN_CONTAINER=1 \
    -e CT_UNIVERSAL_OUTPUT="$OUTPUT" \
    -e CT_HOST_UID="$(id -u)" \
    -e CT_HOST_GID="$(id -g)" \
    -e LC_ALL=C -e TZ=UTC -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    -v "$PORT_DIR":/repo \
    -v "$NEXTOS_SYSROOT":/nxsr:ro \
    "$BUILDER_IMAGE_ID" \
    bash /repo/build_universal.sh
fi

for tool in aarch64-linux-gnu-gcc aarch64-linux-gnu-nm \
            aarch64-linux-gnu-readelf file; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "ferramenta ausente na imagem M17 fixada: $tool" >&2
    exit 1
  }
done

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
FRAMEWORK_ROOT=${CT_FRAMEWORK_ROOT:-/repo/vendor}
cd /repo

OBJDIR=$(mktemp -d)
STUBDIR=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUBDIR"' EXIT

COMMON_INCLUDES=(
  -I src
  -I "$FRAMEWORK_ROOT/nxloader/include"
  -I "$FRAMEWORK_ROOT/nxloader/src"
  -I "$FRAMEWORK_ROOT/nxcompat/include"
  -I "$FRAMEWORK_ROOT/nxcompat/src"
  -I "$FRAMEWORK_ROOT/nxgl/include"
  -I "$FRAMEWORK_ROOT/nxgl/src"
  -I "$FRAMEWORK_ROOT/nxinput/include"
  -I "$FRAMEWORK_ROOT/nxinput/src"
  -I "$FRAMEWORK_ROOT/nxaudio/include"
)

OBJS=()
compile_source() {
  group=$1
  source=$2
  object="$OBJDIR/${group}_$(basename "${source%.c}").o"
  "$CC" -D_GNU_SOURCE -std=gnu11 "${COMMON_INCLUDES[@]}" \
    -idirafter /nxsr/usr/include \
    -idirafter /nxsr/usr/include/SDL2 \
    -idirafter /nxsr/usr/include/freetype2 \
    -O2 -fPIC -fno-omit-frame-pointer \
    -Wa,-I/repo \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-unused-parameter -Wno-unused-function \
    -c "$source" -o "$object"
  OBJS+=("$object")
}

# font_embed.c faz .incbin de fonts/NotoSans-Regular.ttf (piso de texto da UI:
# o muOS nao tem fonte de firmware e o 1.1.1 nao embalava fonts/).
[ -f fonts/NotoSans-Regular.ttf ] ||
  { echo "fonts/NotoSans-Regular.ttf ausente: a fonte embutida e' obrigatoria" >&2; exit 1; }
for source in src/*.c; do
  compile_source chrono "$source"
done

for source in \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_elf32.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_elf64.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_hooks.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_protect.c \
  "$FRAMEWORK_ROOT"/nxloader/src/nxloader_registry.c; do
  compile_source nxloader "$source"
done

for source in \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_backend.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_graphics.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_probe.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_plan.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_receipts.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_registry.c \
  "$FRAMEWORK_ROOT"/nxcompat/src/nxcompat_report.c; do
  compile_source nxcompat "$source"
done

for source in \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_diagnostics.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_logic.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_metrics.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_present.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_sdl2.c \
  "$FRAMEWORK_ROOT"/nxgl/src/nxgl_nxcompat.c; do
  compile_source nxgl "$source"
done

for source in \
  "$FRAMEWORK_ROOT"/nxinput/src/nxinput.c \
  "$FRAMEWORK_ROOT"/nxinput/src/nxinput_core.c \
  "$FRAMEWORK_ROOT"/nxinput/src/nxinput_nxcompat.c; do
  compile_source nxinput "$source"
done

compile_source nxaudio "$FRAMEWORK_ROOT/nxaudio/src/nxaudio.c"

# ---- stubs de link: gravam o SONAME certo sem importar a glibc do NextOS ----
# O aparelho fornece libSDL2-2.0.so.0, libGLESv2.so.2 e libfreetype.so.6.
UNDEFINED=$("$NM" --undefined-only "${OBJS[@]}" 2>/dev/null |
  awk '{print $NF}' | sort -u)

stub_lib() {
  stub_name=$1; stub_soname=$2; stub_regex=$3
  : > "$STUBDIR/$stub_name.c"
  for symbol in $(printf '%s\n' "$UNDEFINED" | grep -E "$stub_regex" || true); do
    printf 'void %s(void) {}\n' "$symbol" >> "$STUBDIR/$stub_name.c"
  done
  "$CC" -shared -fPIC -nostdlib -Wl,-soname,"$stub_soname" \
    "$STUBDIR/$stub_name.c" -o "$STUBDIR/lib$stub_name.so"
}
stub_lib SDL2    libSDL2-2.0.so.0 '^SDL_'
stub_lib GLESv2  libGLESv2.so.2   '^(gl|egl)[A-Z]'
stub_lib freetype libfreetype.so.6 '^FT_'

"$CC" -fPIE -pie -rdynamic -o "$OUTPUT" "${OBJS[@]}" \
  -L"$STUBDIR" -lSDL2 -lGLESv2 -lfreetype -ldl -lm -lpthread -lgcc_s

# ---- trava de portabilidade: GLIBC <= 2.30 ----
MAX_GLIBC=$(
  "$READELF" --version-info "$OUTPUT" |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' |
    sort -Vu | tail -1
)
[ -n "$MAX_GLIBC" ] ||
  { echo "nao foi possivel determinar a versao GLIBC de $OUTPUT" >&2; exit 1; }
version_number=${MAX_GLIBC#GLIBC_}
major=${version_number%%.*}
rest=${version_number#*.}
minor=${rest%%.*}
if [ "$major" -gt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -gt 30 ]; }; then
  echo "FALHA: $OUTPUT exige $MAX_GLIBC (limite GLIBC_2.30)" >&2
  exit 1
fi

# ---- trava do canario bionic ----
# libchrono le a stack-guard de tpidr_el0+0x28. Em aarch64 a TCB tem 16 bytes,
# entao a variavel TLS de offset 0 cobre tpidr+0x10..+0x110 e o slot cai dentro
# do pad nunca-escrito. O invariante a proteger e' exatamente esse: pad em
# offset 0 com 256 bytes, e um bloco TLS estatico grande o bastante.
TLS_MEMSZ=$(
  "$READELF" -lW "$OUTPUT" | awk '$1 == "TLS" { value = $6 } END { print value }'
)
PAD_LAYOUT=$(
  "$READELF" -sW "$OUTPUT" |
    awk '$4 == "TLS" && $8 == "g_bionic_guard_pad" { value = $2 ":" $3 } END { print value }'
)
[ "$PAD_LAYOUT" = "0000000000000000:256" ] ||
  { echo "FALHA: layout TLS do guard pad mudou ($PAD_LAYOUT)" >&2; exit 1; }
[ $((TLS_MEMSZ)) -ge 256 ] ||
  { echo "FALHA: bloco TLS estatico menor que o guard pad ($TLS_MEMSZ)" >&2; exit 1; }

# ---- nenhum DT_NEEDED alem do que o firmware garante ----
NEEDED=$("$READELF" -dW "$OUTPUT" | awk -F'[][]' '/NEEDED/ {print $2}' | sort)
EXPECTED=$(printf '%s\n' libSDL2-2.0.so.0 libGLESv2.so.2 libfreetype.so.6 \
  libdl.so.2 libm.so.6 libpthread.so.0 libgcc_s.so.1 libc.so.6 | sort)
if [ "$NEEDED" != "$EXPECTED" ]; then
  echo "FALHA: DT_NEEDED inesperado:" >&2
  printf '  obtido:  %s\n' "$(echo "$NEEDED" | tr '\n' ' ')" >&2
  printf '  esperado: %s\n' "$(echo "$EXPECTED" | tr '\n' ' ')" >&2
  exit 1
fi

if [ -n "${CT_HOST_UID:-}" ] && [ -n "${CT_HOST_GID:-}" ]; then
  chown "$CT_HOST_UID:$CT_HOST_GID" "$OUTPUT" 2>/dev/null || true
fi

echo "UNIVERSAL AARCH64 BUILD OK -> $OUTPUT"
echo "glibc maxima: $MAX_GLIBC (limite: GLIBC_2.30)"
echo "guard pad TLS: offset/tamanho=$PAD_LAYOUT bloco=$TLS_MEMSZ"
echo "DT_NEEDED: $(echo "$NEEDED" | tr '\n' ' ')"
file "$OUTPUT"
sha256sum "$OUTPUT"
