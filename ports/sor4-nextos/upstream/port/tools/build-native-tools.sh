#!/usr/bin/env bash
set -euo pipefail

PORT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
REPO_DIR=$(cd -- "$PORT_DIR/../.." && pwd -P)
OUT_DIR=${1:-"$PORT_DIR/build"}
mkdir -p -- "$OUT_DIR"

build_with() {
    local cc=$1
    "$cc" -O2 -s "$PORT_DIR/port/tools/sor4probe.c" -ldl -o "$OUT_DIR/sor4probe"
    "$cc" -O2 -s "$PORT_DIR/port/tools/sor4splash.c" -ldl -o "$OUT_DIR/sor4splash"
}

if command -v docker >/dev/null 2>&1 &&
   docker image inspect gtactw-arm64-builder:debian-buster >/dev/null 2>&1; then
    rel_out=$(realpath --relative-to="$REPO_DIR" "$OUT_DIR")
    docker run --rm -v "$REPO_DIR:/src" -w /src \
        gtactw-arm64-builder:debian-buster bash -lc \
        "set -e; aarch64-linux-gnu-gcc -O2 -s ports/sor4/port/tools/sor4probe.c -ldl -o '$rel_out/sor4probe'; aarch64-linux-gnu-gcc -O2 -s ports/sor4/port/tools/sor4splash.c -ldl -o '$rel_out/sor4splash'"
else
    build_with "${CC_AARCH64:-aarch64-linux-gnu-gcc}"
fi

for binary in "$OUT_DIR/sor4probe" "$OUT_DIR/sor4splash"; do
    machine=$(readelf -h "$binary" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
    [ "$machine" = AArch64 ] || { echo "$binary is not AArch64" >&2; exit 1; }
    newest=$(readelf --version-info "$binary" 2>/dev/null |
        sed -n 's/.*Name: GLIBC_\([0-9][0-9.]*\).*/\1/p' | sort -Vu | tail -n 1)
    if [ -n "$newest" ] && [ "$(printf '%s\n%s\n' 2.17 "$newest" | sort -V | tail -n 1)" != 2.17 ]; then
        echo "$binary requires GLIBC_$newest (maximum allowed is 2.17)" >&2
        exit 1
    fi
done

printf 'native tools: %s\n' "$OUT_DIR"
