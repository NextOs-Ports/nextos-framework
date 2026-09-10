#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Standalone host gate for the NEXTOSCONTROLLERS.gptk core. It compiles the
# parser/dispatcher plus its unit test with a plain C99 toolchain (no SDL, no
# CMake) and runs the binary. It never touches devices or the network.
set -euo pipefail

NXINPUT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)

command -v cc >/dev/null 2>&1 || {
  printf 'nxinput-gptk-host: missing required command: cc\n' >&2
  exit 77
}

GPTK_WORK=$(mktemp -d)
[[ -d $GPTK_WORK && ! -L $GPTK_WORK ]] || {
  printf 'nxinput-gptk-host: mktemp did not create a safe work tree\n' >&2
  exit 1
}
cleanup() {
  local status=$?
  trap - EXIT
  rm -rf -- "$GPTK_WORK"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

COMPILERS=(cc)
if command -v clang >/dev/null 2>&1; then
  COMPILERS+=(clang)
fi
if command -v gcc >/dev/null 2>&1; then
  COMPILERS+=(gcc)
fi

CORPUS_COUNT=0

for compiler in "${COMPILERS[@]}"; do
  printf 'nxinput-gptk-host: compiler %s\n' "$compiler"

  "$compiler" -std=c99 -Wall -Wextra -Werror -O1 \
    -I "$NXINPUT_ROOT/include" \
    "$NXINPUT_ROOT/src/nxinput_gptk.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_motion.c" \
    "$NXINPUT_ROOT/tests/test_gptk.c" \
    -o "$GPTK_WORK/test_gptk_$compiler" -lm

  "$GPTK_WORK/test_gptk_$compiler"

  "$compiler" -std=c99 -Wall -Wextra -Werror -O1 \
    -I "$NXINPUT_ROOT/include" \
    "$NXINPUT_ROOT/src/nxinput_gptk.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_motion.c" \
    "$NXINPUT_ROOT/tests/test_gptk_motion.c" \
    -o "$GPTK_WORK/test_gptk_motion_$compiler" -lm

  "$GPTK_WORK/test_gptk_motion_$compiler"

  "$compiler" -std=c99 -Wall -Wextra -Werror -O1 \
    -I "$NXINPUT_ROOT/include" \
    "$NXINPUT_ROOT/src/nxinput_gptk.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_motion.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_loader.c" \
    "$NXINPUT_ROOT/tests/test_gptk_loader.c" \
    -o "$GPTK_WORK/test_gptk_loader_$compiler" -lm

  "$GPTK_WORK/test_gptk_loader_$compiler"

  "$compiler" -std=c99 -Wall -Wextra -Werror -O1 \
    -I "$NXINPUT_ROOT/include" \
    "$NXINPUT_ROOT/src/nxinput_gptk.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_motion.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_loader.c" \
    "$NXINPUT_ROOT/tests/test_gptk_live_remap.c" \
    -o "$GPTK_WORK/test_gptk_live_remap_$compiler" -lm

  "$GPTK_WORK/test_gptk_live_remap_$compiler"

  "$compiler" -std=c99 -Wall -Wextra -Werror -O1 \
    -I "$NXINPUT_ROOT/include" \
    "$NXINPUT_ROOT/src/nxinput_gptk.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_motion.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_live.c" \
    "$NXINPUT_ROOT/tests/test_gptk_live_boundary.c" \
    -o "$GPTK_WORK/test_gptk_live_boundary_$compiler" -lm

  "$GPTK_WORK/test_gptk_live_boundary_$compiler"

  "$compiler" -std=c99 -Wall -Wextra -Werror -O1 \
    -I "$NXINPUT_ROOT/include" \
    "$NXINPUT_ROOT/src/nxinput_exit_chord.c" \
    "$NXINPUT_ROOT/tests/test_exit_chord.c" \
    -o "$GPTK_WORK/test_exit_chord_$compiler"

  "$GPTK_WORK/test_exit_chord_$compiler"

  # V3-HARDENING-01: deterministic adversarial corpus replay through the
  # real parser (ok-* accepted, bad-* rejected fail-closed, never crash).
  "$compiler" -std=c99 -Wall -Wextra -Werror -O1 \
    -I "$NXINPUT_ROOT/include" \
    "$NXINPUT_ROOT/src/nxinput_gptk.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_motion.c" \
    "$NXINPUT_ROOT/tests/corpus_replay_gptk.c" \
    -o "$GPTK_WORK/corpus_replay_gptk_$compiler" -lm

  CORPUS_COUNT=$(timeout 120 "$GPTK_WORK/corpus_replay_gptk_$compiler" \
    "$NXINPUT_ROOT/tests/corpus")
done

# The legacy SDL2 single-header wrapper and the new version-neutral authority
# hand-off share the same independent evdev fallback. Exercise it when the host
# has SDL2; the core tests above remain SDL-free and mandatory.
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2; then
  # shellcheck disable=SC2207
  SDL_FLAGS=($(pkg-config --cflags --libs sdl2))
  cc -std=c99 -Wall -Wextra -O1 -I "$NXINPUT_ROOT/include" \
    "$NXINPUT_ROOT/tests/test_evdev_chord_map.c" \
    -o "$GPTK_WORK/test_evdev_chord_map" "${SDL_FLAGS[@]}"
  SDL_VIDEODRIVER=dummy "$GPTK_WORK/test_evdev_chord_map"
else
  printf 'nxinput-gptk-host: SDL2 wrapper test SKIP (pkg-config sdl2 absent)\n'
fi

printf 'NEXTOSCONTROLLERS V3 gptk host tests: PASS (loader + parser + dispatch + motion + chord) corpus=%s\n' \
  "$CORPUS_COUNT"
