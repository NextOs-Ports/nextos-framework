#!/usr/bin/env bash
# Rebuild the scoped AArch64 Box32 host from the bundled patched Box64 source.
# Public release builds must use a sysroot whose GLIBC is at most 2.30.
set -euo pipefail

export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

SOURCE_DIR=${1:?usage: build-box32-x5m.sh SOURCE_DIR BUILD_DIR}
BUILD_DIR=${2:?usage: build-box32-x5m.sh SOURCE_DIR BUILD_DIR}
AARCH64_CC=${AARCH64_CC:-aarch64-linux-gnu-gcc}
TARGET_SYSROOT=${TARGET_SYSROOT:?set TARGET_SYSROOT to a low-glibc AArch64 target sysroot}
AARCH64_STRIP=${AARCH64_STRIP:-aarch64-linux-gnu-strip}
AARCH64_READELF=${AARCH64_READELF:-aarch64-linux-gnu-readelf}
EXPECTED_SHA256=${EXPECTED_SHA256:-}

[ -d "$SOURCE_DIR" ] || {
  printf 'Box64 source directory not found: %s\n' "$SOURCE_DIR" >&2
  exit 1
}
[ -d "$TARGET_SYSROOT" ] || {
  printf 'target sysroot not found: %s\n' "$TARGET_SYSROOT" >&2
  exit 1
}
for tool in awk cmake install ninja sed sha256sum "$AARCH64_CC" \
            "$AARCH64_STRIP" "$AARCH64_READELF"; do
  command -v "$tool" >/dev/null 2>&1 || {
    printf 'required build tool not found: %s\n' "$tool" >&2
    exit 1
  }
done

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER="$AARCH64_CC" \
  -DCMAKE_SYSROOT="$TARGET_SYSROOT" \
  -DCMAKE_FIND_ROOT_PATH="$TARGET_SYSROOT" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
  -DARM_DYNAREC=ON \
  -DBOX32=ON \
  -DBOX32_BINFMT=OFF \
  -DLD80BITS=OFF \
  -DSTATICBUILD=OFF
cmake --build "$BUILD_DIR"

install -m 0755 "$BUILD_DIR/box64" "$BUILD_DIR/box64.unstripped"
"$AARCH64_STRIP" --strip-unneeded \
  -o "$BUILD_DIR/box64" "$BUILD_DIR/box64.unstripped"

machine=$("$AARCH64_READELF" -h "$BUILD_DIR/box64" |
  sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
[ "$machine" = AArch64 ] || {
  printf 'unexpected Box32 host architecture: %s\n' "$machine" >&2
  exit 1
}

actual_sha256=$(sha256sum "$BUILD_DIR/box64" | awk '{print $1}')
newest=$(
  "$AARCH64_READELF" --version-info "$BUILD_DIR/box64" 2>/dev/null |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sed 's/^GLIBC_//' |
    sort -Vu | tail -1
)
maximum=$(printf '%s\n%s\n' 2.30 "$newest" | sort -V | tail -1)
[ "$maximum" = 2.30 ] || {
  printf 'Box32 host exceeds public ABI ceiling: GLIBC_%s\n' "$newest" >&2
  exit 1
}
if [ -n "$EXPECTED_SHA256" ] && [ "$actual_sha256" != "$EXPECTED_SHA256" ]; then
  printf 'Box32 host hash mismatch: %s\n' "$actual_sha256" >&2
  exit 1
fi
printf 'glibc max = GLIBC_%s\n' "$newest"
printf '%s  %s\n' "$actual_sha256" "$BUILD_DIR/box64"
