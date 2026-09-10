#!/usr/bin/env bash
# Build the public AArch64 loader with Debian Buster (glibc 2.28).
# Target SDL/EGL/GLES headers come from the current NextOS sysroot read-only;
# none of its newer runtime libraries are linked into the result.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUTPUT=${TERRARIA_UNIVERSAL_OUTPUT:-terraria-nextos}

if [ "${TERRARIA_BUSTER_IN_CONTAINER:-0}" != "1" ]; then
  NEXTOS_ROOT=${NEXTOS_ROOT:-"$HOME/NextOS-Elite-Edition"}
  NEXTOS_TOOLCHAIN=
  while IFS= read -r candidate; do
    candidate_sysroot=$candidate/aarch64-libreelec-linux-gnu/sysroot
    # Old/stale build directories can retain a toolchain directory after the
    # sysroot payload has been removed. Select only a complete AArch64 header
    # contract instead of trusting the lexically newest directory name.
    if [ -f "$candidate_sysroot/usr/include/SDL2/SDL.h" ] &&
       [ -f "$candidate_sysroot/usr/include/EGL/egl.h" ] &&
       [ -f "$candidate_sysroot/usr/include/GLES2/gl2.h" ]; then
      NEXTOS_TOOLCHAIN=$candidate
    fi
  done < <(
    find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
      -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
      -print | sort -V
  )
  [ -n "$NEXTOS_TOOLCHAIN" ] || {
    echo "complete current NextOS AArch64 header sysroot not found below $NEXTOS_ROOT" >&2
    exit 1
  }
  NEXTOS_SYSROOT=$NEXTOS_TOOLCHAIN/aarch64-libreelec-linux-gnu/sysroot
  [ -d "$NEXTOS_SYSROOT" ] || {
    echo "NextOS sysroot not found: $NEXTOS_SYSROOT" >&2
    exit 1
  }
  command -v docker >/dev/null 2>&1 || {
    echo "docker is required for the GLIBC <= 2.30 build" >&2
    exit 1
  }

  if [ -n "${TERRARIA_BUSTER_IMAGE:-}" ]; then
    BUSTER_IMAGE=$TERRARIA_BUSTER_IMAGE
  elif docker image inspect playfetch-builder:buster >/dev/null 2>&1; then
    BUSTER_IMAGE=playfetch-builder:buster
  else
    BUSTER_IMAGE=debian:buster
  fi

  exec docker run --rm \
    -e TERRARIA_BUSTER_IN_CONTAINER=1 \
    -e TERRARIA_UNIVERSAL_OUTPUT="$OUTPUT" \
    -e TERRARIA_HOST_UID="$(id -u)" \
    -e TERRARIA_HOST_GID="$(id -g)" \
    -v "$PORT_DIR":/repo \
    -v "$NEXTOS_SYSROOT":/nxsr:ro \
    "$BUSTER_IMAGE" \
    bash /repo/build_universal.sh
fi

export DEBIAN_FRONTEND=noninteractive
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  printf '%s\n' \
    'deb http://archive.debian.org/debian buster main' \
    'deb http://archive.debian.org/debian-security buster/updates main' \
    > /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq >/dev/null
  apt-get install -y -qq --no-install-recommends \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu file >/dev/null
fi

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
cd /repo

OBJDIR=$(mktemp -d)
STUBDIR=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUBDIR"' EXIT

OBJS=()
# main.c must be compiled normally so its initialized 256-byte TLS guard stays
# in .tdata ahead of all zero-initialized compatibility TLS slots.
for source in src/*.c; do
  object="$OBJDIR/$(basename "${source%.c}").o"
  "$CC" -D_GNU_SOURCE -DTER_PUBLIC_BUILD=1 \
    -DPORT_WINDOW_TITLE='"Terraria"' \
    -I src -idirafter /nxsr/usr/include \
    -O2 -fPIC -fno-strict-aliasing -fno-omit-frame-pointer \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-implicit-function-declaration -Wno-unused-parameter \
    -Wno-unused-function -Wno-unused-variable -Wno-comment \
    -c "$source" -o "$object"
  OBJS+=("$object")
done

# Firmware supplies SDL2. This link-only stub records its stable SONAME while
# keeping the build independent of the current NextOS glibc.
UNDEFINED=$($NM --undefined-only "${OBJS[@]}" 2>/dev/null |
  awk '{print $NF}' | sort -u)
for symbol in $(printf '%s\n' "$UNDEFINED" | grep -E '^SDL_' || true); do
  printf 'void %s(void) {}\n' "$symbol"
done > "$STUBDIR/sdl.c"
"$CC" -shared -fPIC -nostdlib \
  -Wl,-soname,libSDL2-2.0.so.0 \
  "$STUBDIR/sdl.c" -o "$STUBDIR/libSDL2.so"

"$CC" -fPIE -pie -rdynamic -o "$OUTPUT" "${OBJS[@]}" \
  -L"$STUBDIR" -lSDL2 -ldl -lm -lpthread -lgcc_s

MAX_GLIBC=$(
  "$READELF" --version-info "$OUTPUT" |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' |
    sort -Vu | tail -1
)
[ -n "$MAX_GLIBC" ] || {
  echo "could not determine the GLIBC requirement of $OUTPUT" >&2
  exit 1
}
version_number=${MAX_GLIBC#GLIBC_}
major=${version_number%%.*}
rest=${version_number#*.}
minor=${rest%%.*}
if [ "$major" -gt 2 ] || {
  [ "$major" -eq 2 ] && [ "$minor" -gt 30 ]
}; then
  echo "FAIL: $OUTPUT requires $MAX_GLIBC (limit: GLIBC_2.30)" >&2
  exit 1
fi

TLS_FILESZ=$(
  "$READELF" -lW "$OUTPUT" |
    awk '$1 == "TLS" { value = $5 } END { print value }'
)
PAD_LAYOUT=$(
  "$READELF" -sW "$OUTPUT" |
    awk '$4 == "TLS" && $8 == "g_bionic_guard_pad" {
      value = $2 ":" $3
    } END { print value }'
)
[ "$PAD_LAYOUT" = "0000000000000000:256" ] || {
  echo "FAIL: Bionic guard-pad TLS layout changed ($PAD_LAYOUT)" >&2
  exit 1
}
[ "$TLS_FILESZ" = "0x000100" ] || {
  echo "FAIL: unexpected TLS template size ($TLS_FILESZ)" >&2
  exit 1
}

if [ -n "${TERRARIA_HOST_UID:-}" ] && [ -n "${TERRARIA_HOST_GID:-}" ]; then
  chown "$TERRARIA_HOST_UID:$TERRARIA_HOST_GID" "$OUTPUT" 2>/dev/null || true
fi

echo "UNIVERSAL AARCH64 BUILD OK -> $OUTPUT"
echo "maximum glibc: $MAX_GLIBC (limit: GLIBC_2.30)"
echo "Bionic TLS guard pad: offset/size=$PAD_LAYOUT template=$TLS_FILESZ"
file "$OUTPUT"
sha256sum "$OUTPUT"
