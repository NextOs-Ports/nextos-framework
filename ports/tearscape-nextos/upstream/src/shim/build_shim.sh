#!/bin/bash
# Build the Tearscape GLES3->GLES2 shim pair for Mali-450.
#
# CC and OUT are deliberately overridable.  The public build recipe points CC
# at the pinned Arm GNU toolchain whose sysroot has an old glibc; a host build
# may still use the distro cross compiler for device-only experiments.
set -euo pipefail

SHIM_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
CC=${CC:-aarch64-linux-gnu-gcc}
OUT=${OUT:-"$SHIM_DIR/../../shimlib"}

COMMON_FLAGS=(
  -O2
  -fPIC
  -shared
  -Wall
  -Wextra
  -Wno-unused-function
  -Wno-unused-parameter
  -nostartfiles
  -I"$SHIM_DIR/include"
  -DEGL_NO_X11
  -DMESA_EGL_NO_X11_HEADERS
  -Wl,--as-needed
  -Wl,--no-undefined
  -Wl,-s
  -Wl,-z,noexecstack
  -Wl,-z,relro
  -Wl,-z,now
)

mkdir -p "$OUT"
"$CC" "${COMMON_FLAGS[@]}" \
  -Wl,-soname,libtearscape-egl-shim.so \
  -o "$OUT/libEGL.so" \
  "$SHIM_DIR/nx_egl.c" "$SHIM_DIR/nx_egl_sdl.c" \
  "$SHIM_DIR/nx_common.c" -ldl
"$CC" "${COMMON_FLAGS[@]}" \
  -Wl,-soname,libtearscape-gles2-shim.so \
  -o "$OUT/libGLESv2.so" \
  "$SHIM_DIR/nx_gles_gen.c" "$SHIM_DIR/nx_gles3_core.c" \
  "$SHIM_DIR/nx_glsl.c" "$SHIM_DIR/nx_common.c" -ldl -lm

printf 'shim built in %s\n' "$OUT"
ls -la "$OUT"
