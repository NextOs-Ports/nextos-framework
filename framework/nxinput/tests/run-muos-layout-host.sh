#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Standalone host gate for the nxinput 0.10.0 muOS layout authority:
# NEXTOS_CONTROLLERS/3 (FACE_LAYOUT), the pre-init GPTK boundary, the bounded
# live-database wait (injected clock; never sleeps for real), the semantic
# domain classification, the seam livedb/receipt integration and the P7
# zero-stick cursor opt-in. Plain C99 toolchain; no device, no network. The
# REAL-SDL half of this boundary is tests/muos_layout_gate.py, which runs
# inside the single C6 battery (run-sdl-c6-host.sh), never here.
set -euo pipefail

NXINPUT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)

command -v cc >/dev/null 2>&1 || {
  printf 'nxinput-muos-layout-host: missing required command: cc\n' >&2
  exit 77
}

SDL2_CFLAGS=""
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2; then
  SDL2_CFLAGS=$(pkg-config --cflags sdl2)
else
  printf 'nxinput-muos-layout-host: sdl2 headers unavailable\n' >&2
  exit 77
fi

WORK=$(mktemp -d)
[[ -d $WORK && ! -L $WORK ]] || {
  printf 'nxinput-muos-layout-host: mktemp did not create a safe work tree\n' >&2
  exit 1
}
cleanup() {
  local status=$?
  trap - EXIT
  rm -rf -- "$WORK"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

CC_FLAGS=(-std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O1
          -I "$NXINPUT_ROOT/include")

printf 'nxinput-muos-layout-host: gate 1/7 NEXTOS_CONTROLLERS/3 parser\n'
cc "${CC_FLAGS[@]}" \
  "$NXINPUT_ROOT/tests/test_gptk_v3.c" \
  "$NXINPUT_ROOT/src/nxinput_gptk.c" \
  "$NXINPUT_ROOT/src/nxinput_gptk_motion.c" \
  -o "$WORK/test_gptk_v3" -lm
"$WORK/test_gptk_v3"

printf 'nxinput-muos-layout-host: gate 2/7 pre-init boundary\n'
cc "${CC_FLAGS[@]}" \
  "$NXINPUT_ROOT/tests/test_gptk_preinit.c" \
  "$NXINPUT_ROOT/src/nxinput_gptk_preinit.c" \
  "$NXINPUT_ROOT/src/nxinput_gptk.c" \
  "$NXINPUT_ROOT/src/nxinput_gptk_loader.c" \
  "$NXINPUT_ROOT/src/nxinput_gptk_motion.c" \
  -o "$WORK/test_gptk_preinit" -lm
"$WORK/test_gptk_preinit"

printf 'nxinput-muos-layout-host: gate 3/7 bounded live-database wait\n'
cc "${CC_FLAGS[@]}" \
  "$NXINPUT_ROOT/tests/test_livedb.c" \
  "$NXINPUT_ROOT/src/nxinput_livedb.c" \
  -o "$WORK/test_livedb"
"$WORK/test_livedb"

printf 'nxinput-muos-layout-host: gate 4/7 semantic domain classification\n'
cc "${CC_FLAGS[@]}" \
  "$NXINPUT_ROOT/tests/test_portmaster_domain.c" \
  "$NXINPUT_ROOT/src/nxinput_portmaster.c" \
  "$NXINPUT_ROOT/src/nxinput_sdl.c" \
  -o "$WORK/test_portmaster_domain" -lm
"$WORK/test_portmaster_domain"

printf 'nxinput-muos-layout-host: gate 5/7 seam livedb/receipt integration\n'
cc "${CC_FLAGS[@]}" \
  "$NXINPUT_ROOT/tests/test_sdl_seam.c" \
  "$NXINPUT_ROOT/src/nxinput_sdl_seam.c" \
  "$NXINPUT_ROOT/src/nxinput_sdl.c" \
  "$NXINPUT_ROOT/src/nxinput_portmaster.c" \
  "$NXINPUT_ROOT/src/nxinput_sovereign.c" \
  "$NXINPUT_ROOT/src/nxinput_authority.c" \
  "$NXINPUT_ROOT/src/nxinput_livedb.c" \
  "$NXINPUT_ROOT/src/nxinput_decision.c" \
  "$NXINPUT_ROOT/src/nxinput_provider.c" \
  "$NXINPUT_ROOT/src/nxinput_godot.c" \
  -o "$WORK/test_sdl_seam" -lm
"$WORK/test_sdl_seam"

printf 'nxinput-muos-layout-host: gate 6/7 P7 zero-stick cursor opt-in\n'
# shellcheck disable=SC2086
cc "${CC_FLAGS[@]}" $SDL2_CFLAGS \
  "$NXINPUT_ROOT/tests/test_cursor_dpad_p7.c" \
  "$NXINPUT_ROOT/src/nxinput_core.c" \
  -o "$WORK/test_cursor_dpad_p7" -lm
"$WORK/test_cursor_dpad_p7"

printf 'nxinput-muos-layout-host: gate 7/7 generic-fallback quarantine\n'
bash "$NXINPUT_ROOT/tests/static_no_device_name_fallback.sh" "$NXINPUT_ROOT"

printf 'nxinput-muos-layout-host: ALL PASS\n'
