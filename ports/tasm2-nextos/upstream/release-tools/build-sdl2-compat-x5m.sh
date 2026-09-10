#!/usr/bin/env bash
# Rebuild AArch64 sdl2-compat against the target NextOS SDL3 headers.
# Public release builds must use a sysroot whose GLIBC is at most 2.30.
set -euo pipefail

export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1785628800}

SOURCE_DIR=${1:?usage: build-sdl2-compat-x5m.sh SOURCE_DIR BUILD_DIR}
BUILD_DIR=${2:?usage: build-sdl2-compat-x5m.sh SOURCE_DIR BUILD_DIR}
AARCH64_CC=${AARCH64_CC:-aarch64-linux-gnu-gcc}
AARCH64_STRIP=${AARCH64_STRIP:-aarch64-linux-gnu-strip}
SDL3_CMAKE_DIR=${SDL3_CMAKE_DIR:?set SDL3_CMAKE_DIR to SDL3Config.cmake directory}
SDL3_INCLUDE_DIRS=${SDL3_INCLUDE_DIRS:?set SDL3_INCLUDE_DIRS to target SDL3 headers}
TARGET_SYSROOT=${TARGET_SYSROOT:?set TARGET_SYSROOT to a low-glibc AArch64 target sysroot}
AARCH64_READELF=${AARCH64_READELF:-aarch64-linux-gnu-readelf}
EXPECTED_SHA256=${EXPECTED_SHA256:-}

[ -d "$SOURCE_DIR" ] || {
  printf 'sdl2-compat source directory not found: %s\n' "$SOURCE_DIR" >&2
  exit 1
}
[ -d "$TARGET_SYSROOT" ] || {
  printf 'target sysroot not found: %s\n' "$TARGET_SYSROOT" >&2
  exit 1
}
[ -d "$SDL3_CMAKE_DIR" ] || {
  printf 'SDL3 CMake directory not found: %s\n' "$SDL3_CMAKE_DIR" >&2
  exit 1
}
for tool in awk cmake find ninja sed sha256sum "$AARCH64_CC" \
            "$AARCH64_READELF" "$AARCH64_STRIP"; do
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
  -DSDL3_DIR="$SDL3_CMAKE_DIR" \
  -DSDL3_INCLUDE_DIRS="$SDL3_INCLUDE_DIRS" \
  -DSDL2COMPAT_LIBC=OFF \
  -DSDL2COMPAT_X11=OFF \
  -DSDL2COMPAT_STATIC=OFF \
  -DSDL2COMPAT_TESTS=OFF \
  -DSDL2COMPAT_INSTALL=OFF
cmake --build "$BUILD_DIR"

library=$(find "$BUILD_DIR" -maxdepth 1 -type f \
  -name 'libSDL2-2.0.so.0.*' -print -quit)
[ -n "$library" ] || {
  printf 'sdl2-compat output not found\n' >&2
  exit 1
}

"$AARCH64_STRIP" --strip-unneeded "$library"

machine=$("$AARCH64_READELF" -h "$library" |
  sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
[ "$machine" = AArch64 ] || {
  printf 'unexpected sdl2-compat architecture: %s\n' "$machine" >&2
  exit 1
}

actual_sha256=$(sha256sum "$library" | awk '{print $1}')
newest=$(
  "$AARCH64_READELF" --version-info "$library" 2>/dev/null |
    grep -oE 'GLIBC_[0-9]+([.][0-9]+)*' | sed 's/^GLIBC_//' |
    sort -Vu | tail -1
)
maximum=$(printf '%s\n%s\n' 2.30 "$newest" | sort -V | tail -1)
[ "$maximum" = 2.30 ] || {
  printf 'sdl2-compat exceeds public ABI ceiling: GLIBC_%s\n' "$newest" >&2
  exit 1
}
if [ -n "$EXPECTED_SHA256" ] && [ "$actual_sha256" != "$EXPECTED_SHA256" ]; then
  printf 'sdl2-compat hash mismatch: %s\n' "$actual_sha256" >&2
  exit 1
fi
printf 'glibc max = GLIBC_%s\n' "$newest"
printf '%s  %s\n' "$actual_sha256" "$library"
