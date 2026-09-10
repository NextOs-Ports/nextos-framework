#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V3 audit (blocker 1): configure -> build -> install the nxinput library, then
# compile a consumer that includes ONLY the installed headers and links ONLY
# the installed archive, calling the new V3 symbols. Proves an adapter can use
# the INSTALLED library (the host tests compile the sources directly and do not
# prove this).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nx-installed-nxinput.XXXXXX")
cleanup() { case "$WORK" in "${TMPDIR:-/tmp}"/nx-installed-nxinput.*) rm -rf -- "$WORK";; esac; }
trap cleanup EXIT INT TERM
PREFIX="$WORK/prefix"
cmake -S "$ROOT" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DNXINPUT_BUILD_TESTS=OFF >/dev/null
cmake --build "$WORK/build" --parallel >/dev/null
cmake --install "$WORK/build" --prefix "$PREFIX" >/dev/null
CC=${CC:-cc}
"$CC" -std=c99 -Wall -Wextra -Werror -O1 \
  -I "$PREFIX/include" \
  "$ROOT/tests/consumer_installed_gptk.c" \
  "$PREFIX/lib/libnxinput-gptk.a" -lm \
  -o "$WORK/consumer"
"$WORK/consumer"
echo "nxinput installed-consumer gate: PASS (linked $PREFIX/lib/libnxinput-gptk.a only)"
