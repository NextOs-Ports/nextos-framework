#!/bin/sh
# Directed, hermetic gate for the reusable Godot runtime policy.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd -P)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxinput-godot-runtime.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

for cc in ${CC:-cc} ${CLANG:-clang}; do
  command -v "$cc" >/dev/null 2>&1 || continue
  "$cc" -std=c11 -O1 -Wall -Wextra -Werror -pedantic \
    -I"$ROOT/include" "$HERE/test_godot_runtime_policy.c" \
    -o "$WORK/test-${cc##*/}"
  "$WORK/test-${cc##*/}"
done
python3 -B "$HERE/test_godot_runtime_template.py"
