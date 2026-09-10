#!/usr/bin/env bash
# ArkOS/R36S AArch64 build using Debian Buster glibc 2.28. Headers for SDL2
# and GLES come read-only from the current NextOS sysroot; target libraries are
# represented by SONAME-only stubs and are resolved on the handheld.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
OUTPUT=${SS_R36S_OUTPUT:-summertimesaga-r36s}

if [ "${SS_BUSTER_IN_CONTAINER:-0}" != "1" ]; then
  NEXTOS_ROOT=${NEXTOS_ROOT:-"$HOME/NextOS-Elite-Edition"}
  NEXTOS_TOOLCHAIN=${NEXTOS_TOOLCHAIN:-$(
    find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
      -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
      -print | sort -V | tail -1
  )}
  [ -n "$NEXTOS_TOOLCHAIN" ] ||
    { echo "toolchain NextOS atual não encontrado em $NEXTOS_ROOT" >&2; exit 1; }
  NEXTOS_SYSROOT=$NEXTOS_TOOLCHAIN/aarch64-libreelec-linux-gnu/sysroot
  [ -d "$NEXTOS_SYSROOT" ] ||
    { echo "sysroot NextOS não encontrado: $NEXTOS_SYSROOT" >&2; exit 1; }
  command -v docker >/dev/null 2>&1 ||
    { echo "docker é necessário para a build ArkOS" >&2; exit 1; }

  exec docker run --rm \
    -e SS_BUSTER_IN_CONTAINER=1 \
    -e SS_R36S_OUTPUT="$OUTPUT" \
    -v "$PORT_DIR":/repo \
    -v "$NEXTOS_SYSROOT":/nxsr:ro \
    debian:buster \
    bash /repo/build_r36s.sh
fi

export DEBIAN_FRONTEND=noninteractive
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  printf '%s\n' \
    'deb http://archive.debian.org/debian buster main' \
    'deb http://archive.debian.org/debian-security buster/updates main' \
    > /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq >/dev/null
  apt-get install -y -qq gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu file >/dev/null
fi

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
cd /repo

OBJ_DIR=$(mktemp -d)
STUB_DIR=$(mktemp -d)
trap 'rm -rf -- "$OBJ_DIR" "$STUB_DIR"' EXIT INT TERM

OBJECTS=()
for source in src/*.c; do
  object=$OBJ_DIR/$(basename "${source%.c}").o
  "$CC" -D_GNU_SOURCE -I src -idirafter /nxsr/usr/include \
    -O2 -fPIC -fno-omit-frame-pointer \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-unused-parameter -Wno-unused-function \
    -c "$source" -o "$object"
  OBJECTS+=("$object")
done

UNDEFINED=$("$NM" --undefined-only "${OBJECTS[@]}" 2>/dev/null |
  awk '{print $NF}' | sort -u)
generate_stubs() {
  pattern=$1
  for symbol in $(printf '%s\n' "$UNDEFINED" | grep -E "$pattern" || true); do
    printf 'void %s(void) {}\n' "$symbol"
  done
}

generate_stubs '^SDL_' > "$STUB_DIR/sdl.c"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 \
  "$STUB_DIR/sdl.c" -o "$STUB_DIR/libSDL2.so"

generate_stubs '^gl[A-Z]' > "$STUB_DIR/gl.c"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so \
  "$STUB_DIR/gl.c" -o "$STUB_DIR/libGLESv2.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libGLESv1_CM.so \
  "$STUB_DIR/gl.c" -o "$STUB_DIR/libGLESv1_CM.so"

TMP_OUTPUT=$OBJ_DIR/$OUTPUT
"$CC" -fPIE -pie -rdynamic -o "$TMP_OUTPUT" "${OBJECTS[@]}" \
  -L"$STUB_DIR" \
  -Wl,--no-as-needed -lSDL2 -lGLESv2 -lGLESv1_CM -Wl,--as-needed \
  -ldl -lm -lpthread -lstdc++ -lgcc_s \
  -Wl,-rpath,'$ORIGIN'

if "$READELF" -Ws "$TMP_OUTPUT" |
    awk '$7 == "UND" && $8 ~ /^glDrawTexfOES(@|$)/ { found = 1 }
         END { exit !found }'; then
  echo "$OUTPUT ainda exige glDrawTexfOES do firmware" >&2
  exit 1
fi

MAX_GLIBC=$(
  "$READELF" --version-info "$TMP_OUTPUT" |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' |
    sort -Vu | tail -1
)
[ -n "$MAX_GLIBC" ] ||
  { echo "não foi possível auditar a glibc de $OUTPUT" >&2; exit 1; }
newest=${MAX_GLIBC#GLIBC_}
selected=$(printf '%s\n%s\n' 2.30 "$newest" | sort -V | tail -1)
[ "$selected" = 2.30 ] ||
  { echo "$OUTPUT exige $MAX_GLIBC (limite GLIBC_2.30)" >&2; exit 1; }

TLS_MEMSZ=$("$READELF" -lW "$TMP_OUTPUT" |
  awk '$1 == "TLS" { value = $6 } END { print value }')
PAD_LAYOUT=$("$READELF" -sW "$TMP_OUTPUT" |
  awk '$4 == "TLS" && $8 == "g_bionic_guard_pad" {
    value = $2 ":" $3
  } END { print value }')
[ "$PAD_LAYOUT" = "0000000000000000:256" ] ||
  { echo "layout TLS do guard pad mudou: $PAD_LAYOUT" >&2; exit 1; }
[ -n "$TLS_MEMSZ" ] ||
  { echo "segmento TLS ausente" >&2; exit 1; }

install -m 0755 "$TMP_OUTPUT" "/repo/$OUTPUT"
echo "ARKOS/R36S BUILD OK -> $OUTPUT"
echo "glibc máxima: $MAX_GLIBC (limite GLIBC_2.30)"
echo "TLS guard pad: $PAD_LAYOUT / memsz=$TLS_MEMSZ"
file "$OUTPUT"
sha256sum "$OUTPUT"
