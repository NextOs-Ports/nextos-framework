#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V3 audit (blocker 1): configure -> build -> install the nxcompat library, then
# compile a consumer that includes ONLY the installed headers and links ONLY
# the installed archive, calling the new V3 symbols. Proves an adapter can use
# the INSTALLED library (the host tests compile the sources directly and do not
# prove this).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nx-installed-nxcompat.XXXXXX")
cleanup() { case "$WORK" in "${TMPDIR:-/tmp}"/nx-installed-nxcompat.*) rm -rf -- "$WORK";; esac; }
trap cleanup EXIT INT TERM
PREFIX="$WORK/prefix"
cmake -S "$ROOT" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DNXCOMPAT_BUILD_TESTS=OFF -DNXCOMPAT_BUILD_TOOLS=OFF >/dev/null
cmake --build "$WORK/build" --parallel >/dev/null
cmake --install "$WORK/build" --prefix "$PREFIX" >/dev/null
CC=${CC:-cc}
"$CC" -std=c99 -Wall -Wextra -Werror -O1 \
  -I "$PREFIX/include" \
  "$ROOT/tests/consumer_installed_langset.c" \
  "$PREFIX/lib/libnxcompat.a"  \
  -o "$WORK/consumer"
"$WORK/consumer"
echo "nxcompat installed-consumer gate: PASS (linked $PREFIX/lib/libnxcompat.a only)"
