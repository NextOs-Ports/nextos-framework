#!/usr/bin/env bash
# NextOS-native AArch64 build. Public multi-device packages also need the
# separate ArkOS build produced by build_r36s.sh.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
NEXTOS_ROOT=${NEXTOS_ROOT:-"$HOME/NextOS-Elite-Edition"}
NEXTOS_TOOLCHAIN=${NEXTOS_TOOLCHAIN:-$(
  find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
    -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
    -print | sort -V | tail -1
)}
OUTPUT=${SS_NEXTOS_OUTPUT:-summertimesaga-nextos}

[ -n "$NEXTOS_TOOLCHAIN" ] ||
  { echo "toolchain NextOS atual não encontrado em $NEXTOS_ROOT" >&2; exit 1; }

CC=$NEXTOS_TOOLCHAIN/bin/aarch64-libreelec-linux-gnu-gcc
READELF=$NEXTOS_TOOLCHAIN/bin/aarch64-libreelec-linux-gnu-readelf
SYSROOT=$NEXTOS_TOOLCHAIN/aarch64-libreelec-linux-gnu/sysroot
LIBC=$SYSROOT/usr/lib/libc.so.6

[ -x "$CC" ] || { echo "compilador não encontrado: $CC" >&2; exit 1; }
[ -x "$READELF" ] || { echo "readelf não encontrado: $READELF" >&2; exit 1; }
[ -s "$LIBC" ] || { echo "libc do sysroot não encontrada: $LIBC" >&2; exit 1; }

cd "$PORT_DIR"
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/summertime-nextos.XXXXXX")
trap 'rm -rf -- "$TMP_ROOT"' EXIT INT TERM
STUB_DIR=$TMP_ROOT/stubs
mkdir -p "$STUB_DIR"

# SDL2 comes from the target firmware. The link stub records only its stable
# SONAME and prevents the executable from inheriting a board-specific SDL ABI.
SDL_SYMBOLS=(
  SDL_CreateWindow SDL_DestroyWindow SDL_GetDesktopDisplayMode SDL_GetError
  SDL_GetCurrentVideoDriver SDL_GL_GetDrawableSize SDL_SetHint
  SDL_GL_CreateContext SDL_GL_DeleteContext SDL_GL_GetProcAddress
  SDL_GL_MakeCurrent SDL_GL_SetAttribute SDL_GL_SetSwapInterval
  SDL_GL_SwapWindow SDL_Init SDL_OpenAudioDevice SDL_CloseAudioDevice
  SDL_PauseAudioDevice SDL_LockAudioDevice SDL_UnlockAudioDevice
)
for symbol in "${SDL_SYMBOLS[@]}"; do
  printf 'void %s(void) {}\n' "$symbol"
done > "$STUB_DIR/sdl.c"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 \
  "$STUB_DIR/sdl.c" -o "$STUB_DIR/libSDL2.so"

mapfile -t SOURCES < <(find src -maxdepth 1 -type f -name '*.c' -print | sort)
TMP_OUTPUT=$TMP_ROOT/$OUTPUT
"$CC" --sysroot="$SYSROOT" \
  -D_GNU_SOURCE -I src -I "$SYSROOT/usr/include" \
  -O2 -fPIE -pie -fno-omit-frame-pointer -rdynamic \
  -Wno-int-conversion -Wno-incompatible-pointer-types \
  -o "$TMP_OUTPUT" "${SOURCES[@]}" \
  -L "$STUB_DIR" -lSDL2 -lGLESv2 -lGLESv1_CM \
  -ldl -lm -lpthread -lstdc++ -lgcc_s \
  -Wl,-rpath,'$ORIGIN'

if "$READELF" -Ws "$TMP_OUTPUT" |
    awk '$7 == "UND" && $8 ~ /^glDrawTexfOES(@|$)/ { found = 1 }
         END { exit !found }'; then
  echo "$OUTPUT ainda exige glDrawTexfOES do firmware" >&2
  exit 1
fi

SYSROOT_GLIBC=$(
  "$READELF" -V "$LIBC" |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' |
    sort -Vu | tail -1
)
MAX_GLIBC=$(
  "$READELF" --version-info "$TMP_OUTPUT" |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' |
    sort -Vu | tail -1
)
[ -n "$SYSROOT_GLIBC" ] && [ -n "$MAX_GLIBC" ] ||
  { echo "não foi possível auditar a glibc" >&2; exit 1; }

selected=$(printf '%s\n%s\n' "${SYSROOT_GLIBC#GLIBC_}" \
  "${MAX_GLIBC#GLIBC_}" | sort -V | tail -1)
[ "$selected" = "${SYSROOT_GLIBC#GLIBC_}" ] ||
  { echo "$OUTPUT excede o sysroot ($MAX_GLIBC > $SYSROOT_GLIBC)" >&2; exit 1; }

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

install -m 0755 "$TMP_OUTPUT" "$PORT_DIR/$OUTPUT"
echo "NEXTOS BUILD OK -> $OUTPUT"
echo "sysroot glibc: $SYSROOT_GLIBC"
echo "binário glibc máxima: $MAX_GLIBC"
echo "TLS guard pad: $PAD_LAYOUT / memsz=$TLS_MEMSZ"
file "$OUTPUT"
sha256sum "$OUTPUT"
