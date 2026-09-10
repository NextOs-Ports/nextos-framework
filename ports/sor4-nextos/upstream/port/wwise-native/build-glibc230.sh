#!/usr/bin/env bash
# Build the public multi-device Wwise/OpenAL wrapper for glibc 2.30 systems.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1783900800}

fail() {
    printf 'wwise build error: %s\n' "$*" >&2
    exit 1
}

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)
OUTPUT=${1:-"$PORT_DIR/build/host_pkg/libs/libWwise.so"}
BUILDER_IMAGE=${SOR4_WWISE_BUILDER_IMAGE:-sor4-wwise-builder:glibc230}

for command in docker file grep install mkdir mktemp nm readelf rm sed sha256sum sort \
               strings tail; do
    command -v "$command" >/dev/null 2>&1 || fail "missing host command: $command"
done

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/sor4-wwise.XXXXXX")
cleanup() {
    rm -rf -- "$TMP_ROOT"
}
trap cleanup EXIT INT TERM

if [[ -z "${SOR4_WWISE_BUILDER_IMAGE:-}" ]]; then
    docker build --file "$SCRIPT_DIR/Dockerfile.glibc230" \
        --tag "$BUILDER_IMAGE" \
        "$SCRIPT_DIR"
fi

docker run --rm \
    -e SOURCE_DATE_EPOCH \
    -v "$SCRIPT_DIR:/src:ro" \
    -v "$TMP_ROOT:/out" \
    "$BUILDER_IMAGE" bash -eu -o pipefail -c '
aarch64-linux-gnu-gcc -D_GNU_SOURCE -I /src -I /opt/sdl/usr/include \
    -O2 -fPIC -fno-omit-frame-pointer \
    -shared -Wl,-E -Wl,--build-id=sha1 \
    -L /opt/sdl/usr/lib/aarch64-linux-gnu \
    -o /out/libWwise.so \
    /src/android_shim.c \
    /src/audioout.c \
    /src/bionic_shims.c \
    /src/error.c \
    /src/imports.gen.c \
    /src/jni_shim.c \
    /src/opensles_shim.c \
    /src/pthread_fake.c \
    /src/sem_shim.c \
    /src/so_util.c \
    /src/util.c \
    /src/wwise_native.c \
    -lSDL2 -ldl -lm -lpthread
'

WRAPPER="$TMP_ROOT/libWwise.so"
[[ -f "$WRAPPER" ]] || fail "compiler did not produce libWwise.so"
readelf -h "$WRAPPER" | grep -F 'Machine:                           AArch64' \
    >/dev/null || \
    fail "wrapper is not AArch64"
[[ "$(nm -D "$WRAPPER" | grep -c ' T native_wwise_')" == 21 ]] || \
    fail "wrapper does not export the expected 21 native_wwise functions"
nm -D "$WRAPPER" | grep -E ' [TW] ao_set_rtpc_volume$' >/dev/null || \
    fail "wrapper is missing the OpenAL RTPC volume bridge"

MAX_GLIBC=$(readelf --version-info "$WRAPPER" |
    sed -n 's/.*Name: GLIBC_\([0-9][0-9.]*\).*/\1/p' |
    sort -Vu | tail -n 1)
[[ -n "$MAX_GLIBC" ]] || fail "could not determine the wrapper GLIBC requirement"
[[ "$(printf '%s\n%s\n' 2.30 "$MAX_GLIBC" | sort -V | tail -n 1)" == 2.30 ]] || \
    fail "wrapper requires GLIBC_$MAX_GLIBC, above the 2.30 device ceiling"

if strings -a "$WRAPPER" | grep -E \
        '/home/|/root/|/mnt/|WWISE_FORCEVOL|WWISE_VOLFLOOR' >/dev/null; then
    fail "wrapper contains a workstation path or retired forced-volume control"
fi

mkdir -p -- "$(dirname -- "$OUTPUT")"
install -m 0644 -- "$WRAPPER" "$OUTPUT"
printf 'Wwise wrapper: %s\n' "$(file "$OUTPUT")"
printf 'Maximum GLIBC: %s\n' "$MAX_GLIBC"
sha256sum -- "$OUTPUT"
