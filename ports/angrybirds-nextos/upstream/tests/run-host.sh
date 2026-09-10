#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/angrybirds-input-test.XXXXXX")
cleanup() {
  find "$WORK" -depth -mindepth 1 -delete 2>/dev/null || true
  rmdir "$WORK" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for compiler in gcc clang; do
  command -v "$compiler" >/dev/null
  "$compiler" -std=c11 -O2 -Wall -Wextra -Werror \
    "$ROOT/tests/test_cursor_buttons.c" -o "$WORK/test-$compiler"
  "$WORK/test-$compiler"
  "$compiler" -std=c11 -O2 -Wall -Wextra -Werror \
    "$ROOT/tests/test_exit_chord.c" -o "$WORK/exit-$compiler"
  "$WORK/exit-$compiler"
  "$compiler" -std=c11 -O2 -Wall -Wextra -Werror \
    "$ROOT/tests/test_audio_policy.c" -o "$WORK/audio-$compiler"
  "$WORK/audio-$compiler"
done

python3 -B "$ROOT/tests/test_input_contract.py"
