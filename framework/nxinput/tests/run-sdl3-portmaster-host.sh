#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

HERE=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
COMPONENT=$(CDPATH= cd -- "$HERE/.." && pwd -P)
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/nxinput-sdl3-pm.XXXXXX")
trap 'rm -rf -- "$BUILD_DIR"' EXIT INT TERM

CC_BIN=${CC:-cc}
SDL3_FLAGS=()
if [[ -n ${NXINPUT_SDL3_TEST_INCLUDE:-} ]]; then
  test -f "$NXINPUT_SDL3_TEST_INCLUDE/SDL3/SDL.h" || {
    printf '%s\n' "missing SDL3 header under explicit test include" >&2
    exit 1
  }
  SDL3_FLAGS=(-I"$NXINPUT_SDL3_TEST_INCLUDE")
elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl3; then
  # shellcheck disable=SC2207
  SDL3_FLAGS=($(pkg-config --cflags sdl3))
else
  printf '%s\n' \
    'missing SDL3 headers: install sdl3.pc or set NXINPUT_SDL3_TEST_INCLUDE' >&2
  exit 77
fi

common_flags=(
  -std=c11 -O1 -g
  -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes
  -I"$COMPONENT/include"
)

"$CC_BIN" "${common_flags[@]}" \
  -DNXINPUT_SDL3_PM_CORE_ONLY \
  "$COMPONENT/src/nxinput_sdl3_portmaster.c" \
  "$HERE/test_sdl3_portmaster_mapping.c" \
  -o "$BUILD_DIR/test-sdl3-portmaster-mapping"
"$BUILD_DIR/test-sdl3-portmaster-mapping"

"$CC_BIN" "${common_flags[@]}" \
  "${SDL3_FLAGS[@]}" -pthread \
  "$COMPONENT/src/nxinput_sdl3_portmaster.c" \
  "$HERE/test_sdl3_portmaster_manager.c" \
  -Wl,--wrap=open -Wl,--wrap=fstat -Wl,--wrap=ioctl -Wl,--wrap=close \
  -o "$BUILD_DIR/test-sdl3-portmaster-manager"
"$BUILD_DIR/test-sdl3-portmaster-manager"

printf '%s\n' 'NXINPUT SDL3 PORTMASTER HOST: PASS'
