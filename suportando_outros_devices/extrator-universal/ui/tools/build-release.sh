#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
UI_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)"
TOOLCHAIN_ROOT="${NXEXTRACT_UI_TOOLCHAIN_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/nxextract-ui/toolchains}"
ZIG_VERSION=0.16.0
ZIG_ARCHIVE="zig-x86_64-linux-$ZIG_VERSION.tar.xz"
ZIG_URL="https://ziglang.org/download/$ZIG_VERSION/$ZIG_ARCHIVE"
ZIG_SHA256=70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00
ZIG_ROOT="$TOOLCHAIN_ROOT/zig-$ZIG_VERSION"
ZIG="${NXEXTRACT_UI_ZIG:-$ZIG_ROOT/zig}"

mkdir -p "$TOOLCHAIN_ROOT" "$TOOLCHAIN_ROOT/cache"
if [ ! -x "$ZIG" ]; then
  command -v curl >/dev/null 2>&1 || {
    echo "nxextract-ui build: curl is required to fetch the pinned Zig toolchain" >&2
    exit 1
  }
  archive="$TOOLCHAIN_ROOT/$ZIG_ARCHIVE"
  [ -f "$archive" ] || curl -fL "$ZIG_URL" -o "$archive"
  actual_sha=$(sha256sum "$archive" | awk '{print $1}')
  [ "$actual_sha" = "$ZIG_SHA256" ] || {
    echo "nxextract-ui build: Zig archive hash mismatch" >&2
    exit 1
  }
  extract_root=$(mktemp -d "$TOOLCHAIN_ROOT/.zig-extract.XXXXXX")
  tar -xf "$archive" -C "$extract_root"
  [ ! -e "$ZIG_ROOT" ] || {
    echo "nxextract-ui build: non-executable Zig directory already exists" >&2
    exit 1
  }
  mv "$extract_root/zig-x86_64-linux-$ZIG_VERSION" "$ZIG_ROOT"
fi

[ "$("$ZIG" version)" = "$ZIG_VERSION" ] || {
  echo "nxextract-ui build: Zig version mismatch" >&2
  exit 1
}

export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1786665600}"
export ZIG_GLOBAL_CACHE_DIR="$TOOLCHAIN_ROOT/cache/global"
export ZIG_LOCAL_CACHE_DIR="$TOOLCHAIN_ROOT/cache/local"

build_one() {
  local architecture=$1 target=$2 output
  output="$UI_DIR/release/$architecture/nxextract-ui"
  mkdir -p "$(dirname -- "$output")"
  "$ZIG" cc \
    -target "$target" \
    -std=gnu11 \
    -D_GNU_SOURCE \
    -O2 \
    -fPIE \
    -pie \
    -s \
    -ffile-prefix-map="$UI_DIR"=. \
    -fno-ident \
    -Wall \
    -Wextra \
    -Werror \
    -Wformat=2 \
    -Wshadow \
    -Wstrict-prototypes \
    -Wconversion \
    -Wl,--as-needed \
    -Wl,--build-id=sha1 \
    -Wl,-z,relro,-z,now \
    -o "$output" \
    "$UI_DIR/nxextract_ui.c" \
    -ldl
  chmod 0755 "$output"
}

build_one aarch64 aarch64-linux-gnu.2.17
build_one armv7 arm-linux-gnueabihf.2.17
build_one x86_64 x86_64-linux-gnu.2.17
build_one i386 x86-linux-gnu.2.17

python3 "$SCRIPT_DIR/release-manifest.py" --write
python3 "$SCRIPT_DIR/release-manifest.py" --verify
echo "nxextract-ui build: PASS"
