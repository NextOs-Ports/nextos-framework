#!/usr/bin/env bash
# Build the single universal AArch64 binary for the public release.
#
# The Debian Buster cross toolchain keeps the executable below GLIBC_2.30.
# SDL2 and the graphics drivers are supplied by the target firmware.  The
# current NextOS sysroot is mounted read-only for API headers only; none of its
# glibc-2.43 libraries are linked into the result.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUTPUT=${HGO_UNIVERSAL_OUTPUT:-build/hitmango-nextos}

if [ "${HGO_BUSTER_IN_CONTAINER:-0}" != "1" ]; then
  NEXTOS_ROOT=${NEXTOS_ROOT:-"$HOME/NextOS-Elite-Edition"}
  NEXTOS_TOOLCHAIN=$(
    find -H "$NEXTOS_ROOT" -maxdepth 2 -type d \
      -path '*/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-*/toolchain' \
      -print | sort -V | tail -1
  )
  [ -n "$NEXTOS_TOOLCHAIN" ] || {
    echo "current NextOS toolchain not found below $NEXTOS_ROOT" >&2
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

  if [ -n "${HGO_BUSTER_IMAGE:-}" ]; then
    BUSTER_IMAGE=$HGO_BUSTER_IMAGE
  elif docker image inspect playfetch-builder:buster >/dev/null 2>&1; then
    BUSTER_IMAGE=playfetch-builder:buster
  else
    BUSTER_IMAGE=debian:buster
  fi

  exec docker run --rm \
    -e HGO_BUSTER_IN_CONTAINER=1 \
    -e HGO_UNIVERSAL_OUTPUT="$OUTPUT" \
    -e HGO_HOST_UID="$(id -u)" \
    -e HGO_HOST_GID="$(id -g)" \
    -v "$PORT_DIR":/repo \
    -v "$NEXTOS_SYSROOT":/nxsr:ro \
    "$BUSTER_IMAGE" \
    bash /repo/build_universal.sh
fi

export DEBIAN_FRONTEND=noninteractive
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || \
   [ ! -e /usr/lib/aarch64-linux-gnu/libz.so ]; then
  dpkg --add-architecture arm64
  printf '%s\n' \
    'deb [arch=amd64,arm64] http://archive.debian.org/debian buster main' \
    'deb [arch=amd64,arm64] http://archive.debian.org/debian-security buster/updates main' \
    > /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq >/dev/null
  apt-get install -y -qq --no-install-recommends \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    zlib1g-dev:arm64 file >/dev/null
fi

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
cd /repo
mkdir -p -- "$(dirname -- "$OUTPUT")"

OBJDIR=$(mktemp -d)
STUBDIR=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUBDIR"' EXIT

SOURCES=(
  src/main.c
  src/nx_elf.c
  src/bionic.c
  src/pthread_bridge.c
  src/android.c
  src/egl.c
  src/egl_sdl.c
  src/etc2_decode.c
  src/input_layout.c
  src/input_lifecycle.c
  src/motion_stream.c
  src/touch_arbiter.c
  src/jni.c
  src/input.c
  src/audio.c
  src/nxgl_frame_proof_adapter.c
)
OBJS=()
for source in "${SOURCES[@]}"; do
  object="$OBJDIR/$(basename "${source%.c}").o"
  "$CC" -I src -idirafter /nxsr/usr/include \
    -O2 -g -fPIC -fno-strict-aliasing -fno-omit-frame-pointer \
    -Wall -Wextra -Wno-unused-parameter \
    -c "$source" -o "$object"
  OBJS+=("$object")
done

# The target firmware provides SDL2.  This link-only stub records its stable
# SONAME without importing the newer glibc used by the current NextOS build.
UNDEFINED=$($NM --undefined-only "${OBJS[@]}" 2>/dev/null |
  awk '{print $NF}' | sort -u)
for symbol in $(printf '%s\n' "$UNDEFINED" | grep -E '^SDL_' || true); do
  printf 'void %s(void) {}\n' "$symbol"
done > "$STUBDIR/sdl.c"
"$CC" -shared -fPIC -nostdlib \
  -Wl,-soname,libSDL2-2.0.so.0 \
  "$STUBDIR/sdl.c" -o "$STUBDIR/libSDL2.so"

"$CC" -fPIE -pie -rdynamic -o "$OUTPUT" "${OBJS[@]}" \
  -L"$STUBDIR" -lSDL2 -ldl -lm -lpthread -lz -lgcc_s

FORBIDDEN_DYNAMIC_IMPORTS=$(
  "$READELF" -Ws "$OUTPUT" |
    awk '$7 == "UND" {
      name = $8
      sub(/@.*/, "", name)
      if (name == "reallocarray" ||
          name == "SDL_JoystickGetVendor" ||
          name == "SDL_JoystickGetProduct")
        print name
    }' | sort -u
)
[ -z "$FORBIDDEN_DYNAMIC_IMPORTS" ] || {
  printf 'FAIL: forbidden public compatibility import(s):\n%s\n' \
    "$FORBIDDEN_DYNAMIC_IMPORTS" >&2
  exit 1
}

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

if [ -n "${HGO_HOST_UID:-}" ] && [ -n "${HGO_HOST_GID:-}" ]; then
  chown "$HGO_HOST_UID:$HGO_HOST_GID" "$OUTPUT" 2>/dev/null || true
fi

echo "UNIVERSAL AARCH64 BUILD OK -> $OUTPUT"
echo "maximum glibc: $MAX_GLIBC (limit: GLIBC_2.30)"
echo "Bionic TLS guard pad: offset/size=$PAD_LAYOUT template=$TLS_FILESZ"
file "$OUTPUT"
sha256sum "$OUTPUT"
