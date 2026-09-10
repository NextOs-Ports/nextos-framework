#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1786665600}

fail() {
  printf 'actionsquad build error: %s\n' "$*" >&2
  exit 1
}

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
SDK=${ACTIONSQUAD_SDK:?set ACTIONSQUAD_SDK to the AArch64 SDL2 development sysroot}
SDK=$(CDPATH= cd -- "$SDK" && pwd -P)
IMAGE=${ACTIONSQUAD_BUILDER_IMAGE:-actionsquad-builder:glibc230}
OUTPUT="$ROOT/build/actionsquad-nextos"

[[ -f $SDK/usr/include/SDL2/SDL.h ]] || fail "SDL2 headers missing"
[[ -f $SDK/usr/include/EGL/egl.h ]] || fail "EGL headers missing"
[[ -f $SDK/usr/include/GLES2/gl2.h ]] || fail "GLES2 headers missing"
mkdir -p "$ROOT/build"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build --file "$ROOT/Dockerfile.glibc230" --tag "$IMAGE" "$ROOT"
fi

docker run --rm \
  -e SOURCE_DATE_EPOCH \
  -v "$ROOT:/src:ro" \
  -v "$ROOT/build:/out" \
  -v "$SDK:/sdk:ro" \
  "$IMAGE" bash -eu -o pipefail -c '
STUB=$(mktemp -d)
cleanup() { rm -rf -- "$STUB"; }
trap cleanup EXIT
gen_stub() {
  pattern=$1 output=$2
  { grep "$pattern" /src/src/runtime-symbols.txt || true; } |
    sed "s/.*/void &(void){}/" > "$output"
}
gen_stub "^SDL_" "$STUB/sdl.c"
for symbol in SDL_Delay SDL_GameControllerGetJoystick SDL_JoystickInstanceID; do
  printf "void %s(void){}\n" "$symbol" >> "$STUB/sdl.c"
done
gen_stub "^egl" "$STUB/egl.c"
gen_stub "^gl[A-Z]" "$STUB/gles.c"
aarch64-linux-gnu-gcc -shared -fPIC -nostdlib \
  -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"
aarch64-linux-gnu-gcc -shared -fPIC -nostdlib \
  -Wl,-soname,libEGL.so.1 "$STUB/egl.c" -o "$STUB/libEGL.so"
aarch64-linux-gnu-gcc -shared -fPIC -nostdlib \
  -Wl,-soname,libGLESv2.so.2 "$STUB/gles.c" -o "$STUB/libGLESv2.so"

aarch64-linux-gnu-gcc \
  -D_GNU_SOURCE -D_REENTRANT \
  -fPIE -pie -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
  -Wno-int-conversion -Wno-incompatible-pointer-types \
  -Wno-implicit-function-declaration -Wno-comment -Wno-unused-function \
  -Wno-unused-variable -Wno-unused-parameter \
  -Isrc -idirafter /sdk/usr/include -idirafter /sdk/usr/include/SDL2 \
  -Wl,--build-id=sha1 -Wl,--export-dynamic -Wl,--as-needed -L"$STUB" \
  -o /out/actionsquad-nextos \
  /src/src/main.c /src/src/so_util.c /src/src/util.c /src/src/error.c \
  /src/src/imports.c /src/src/as_shims.c /src/src/pthread_bridge.c \
  /src/src/jni_shim.c /src/src/egl_shim.c /src/src/android_shim.c \
  /src/src/nxgl_frame_proof_adapter.c \
  /src/src/opensles_shim.c /src/src/coi_shims.c /src/src/etc1_encode.c \
  /src/src/etc2_decode.c \
  -lSDL2 -lGLESv2 -lEGL -ldl -lm -lpthread -lgcc
aarch64-linux-gnu-strip --strip-unneeded /out/actionsquad-nextos
'

MAX_GLIBC=$(readelf --version-info "$OUTPUT" |
  sed -n 's/.*Name: GLIBC_\([0-9][0-9.]*\).*/\1/p' | sort -Vu | tail -n 1)
[[ -n $MAX_GLIBC ]] || fail "could not determine GLIBC requirement"
[[ $(printf '%s\n%s\n' 2.30 "$MAX_GLIBC" | sort -V | tail -n 1) == 2.30 ]] ||
  fail "GLIBC_$MAX_GLIBC exceeds GLIBC_2.30"
file "$OUTPUT"
printf 'Maximum GLIBC: %s\n' "$MAX_GLIBC"
sha256sum "$OUTPUT"
