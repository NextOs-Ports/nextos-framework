#!/bin/sh
# Directed, windowless gate for the Godot frame-proof hook order.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd -P)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxgl-godot-proof.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

for cc in ${CC:-cc} ${CLANG:-clang}; do
	command -v "$cc" >/dev/null 2>&1 || continue
	"$cc" -std=c99 -O1 -Wall -Wextra -Werror -pedantic \
		-I"$ROOT/adapters" -I"$ROOT/engine-glue" \
		"$HERE/test_godot_frame_proof.c" -o "$WORK/test-${cc##*/}"
	"$WORK/test-${cc##*/}"
done
