#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Directed gate only for nxinput's live runtime boundary.
set -euo pipefail

NXINPUT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
LIVE_WORK=$(mktemp -d)
cleanup() {
  local status=$?
  trap - EXIT
  rm -rf -- "$LIVE_WORK"
  exit "$status"
}
trap cleanup EXIT

compilers=(cc)
command -v clang >/dev/null 2>&1 && compilers+=(clang)
for compiler in "${compilers[@]}"; do
  "$compiler" -std=c99 -Wall -Wextra -Werror -O1 \
    -I "$NXINPUT_ROOT/include" \
    "$NXINPUT_ROOT/src/nxinput_gptk.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_motion.c" \
    "$NXINPUT_ROOT/src/nxinput_gptk_live.c" \
    "$NXINPUT_ROOT/tests/test_gptk_live_boundary.c" \
    -o "$LIVE_WORK/live-$compiler" -lm
  "$LIVE_WORK/live-$compiler"
done

printf 'nxinput 0.8.1 directed GPTK live boundary: PASS\n'
