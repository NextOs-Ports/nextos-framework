#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1786665600}

fail() {
  printf 'magicrampage build error: %s\n' "$*" >&2
  exit 1
}

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
SDK=${MAGICRAMPAGE_SDK:?set MAGICRAMPAGE_SDK to the AArch64 SDL2 development sysroot}
SDK=$(cd -- "$SDK" && pwd -P)
IMAGE=${MAGICRAMPAGE_BUILDER_IMAGE:-magicrampage-builder:glibc230}
OUTPUT="$ROOT/build/magicrampage-nextos"

[[ -f "$SDK/usr/include/SDL2/SDL_mixer.h" ]] || fail "SDL_mixer headers missing"
[[ -f "$SDK/usr/include/zip.h" ]] || fail "libzip headers missing"
[[ -e "$SDK/usr/lib/libSDL2.so" ]] || fail "SDL2 link library missing"
[[ -e "$SDK/usr/lib/libSDL2_mixer.so" ]] || fail "SDL2_mixer link library missing"
[[ -e "$SDK/usr/lib/libzip.so" ]] || fail "libzip link library missing"
SDL2_LIB=$(readlink -f "$SDK/usr/lib/libSDL2.so")
SDL2_MIXER_LIB=$(readlink -f "$SDK/usr/lib/libSDL2_mixer.so")
LIBZIP_LIB=$(readlink -f "$SDK/usr/lib/libzip.so")

mkdir -p "$ROOT/build"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build --file "$ROOT/Dockerfile.glibc230" --tag "$IMAGE" "$ROOT"
fi

docker run --rm \
  -e SOURCE_DATE_EPOCH \
  -v "$ROOT:/src:ro" \
  -v "$ROOT/build:/out" \
  -v "$SDK:/sdk:ro" \
  -v "$SDL2_LIB:/sdk-libs/libSDL2.so:ro" \
  -v "$SDL2_MIXER_LIB:/sdk-libs/libSDL2_mixer.so:ro" \
  -v "$LIBZIP_LIB:/sdk-libs/libzip.so:ro" \
  "$IMAGE" bash -eu -o pipefail -c '
aarch64-linux-gnu-gcc \
  -D_GNU_SOURCE -D_REENTRANT \
  -I /src/src -idirafter /sdk/usr/include \
  -O2 -fPIE -fno-omit-frame-pointer -rdynamic \
  -Wl,--build-id=sha1 -Wl,--allow-shlib-undefined -pie \
  -o /out/magicrampage-nextos \
  /src/src/main.c \
  /src/src/audio_backend.c \
  /src/src/jni_min.c \
  /src/src/stubs.c \
  /src/src/so_util.c \
  /src/src/pthread_bridge.c \
  /src/src/util.c \
  /src/src/error.c \
  /sdk-libs/libSDL2_mixer.so \
  /sdk-libs/libSDL2.so \
  /sdk-libs/libzip.so \
  -ldl -lm -lpthread
aarch64-linux-gnu-strip --strip-unneeded /out/magicrampage-nextos
'

MAX_GLIBC=$(readelf --version-info "$OUTPUT" |
  sed -n 's/.*Name: GLIBC_\([0-9][0-9.]*\).*/\1/p' |
  sort -Vu | tail -n 1)
[[ -n "$MAX_GLIBC" ]] || fail "could not determine GLIBC requirement"
[[ "$(printf '%s\n%s\n' 2.30 "$MAX_GLIBC" | sort -V | tail -n 1)" == 2.30 ]] ||
  fail "GLIBC_$MAX_GLIBC exceeds GLIBC_2.30"

file "$OUTPUT"
printf 'Maximum GLIBC: %s\n' "$MAX_GLIBC"
sha256sum "$OUTPUT"
