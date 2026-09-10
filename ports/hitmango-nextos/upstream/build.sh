#!/usr/bin/env bash
# Development build against the current NextOS Amlogic-old sysroot.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NEXTOS_ROOT=${NEXTOS_ROOT:-"$HOME/NextOS-Elite-Edition"}
TOOLCHAIN=${NEXTOS_TOOLCHAIN:-$(
  find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
    -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
    -print | sort -V | tail -1
)}
[ -n "$TOOLCHAIN" ] || {
  echo "current NextOS toolchain not found below $NEXTOS_ROOT" >&2
  exit 1
}

CC=$TOOLCHAIN/bin/aarch64-libreelec-linux-gnu-gcc
SYSROOT=$TOOLCHAIN/aarch64-libreelec-linux-gnu/sysroot
cd "$PORT_DIR"
OUTPUT=${HGO_DEV_OUTPUT:-build/hitmango-nextos-dev}
mkdir -p -- "$(dirname -- "$OUTPUT")"

mapfile -t SOURCES < <(find src -maxdepth 1 -type f -name '*.c' -print | sort)
"$CC" --sysroot="$SYSROOT" -I src -I "$SYSROOT/usr/include" \
  -O2 -g -fPIE -pie -fno-strict-aliasing -fno-omit-frame-pointer -rdynamic \
  -Wall -Wextra -Wno-unused-parameter \
  -o "$OUTPUT" "${SOURCES[@]}" \
  -lSDL2 -ldl -lm -lpthread -lz -lgcc_s

file "$OUTPUT"
sha256sum "$OUTPUT"
