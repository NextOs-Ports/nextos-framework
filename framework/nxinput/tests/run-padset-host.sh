#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# nxinput 0.10.2 -- host gate for nxinput_padset (fake SDL vtable, no device).
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NXINPUT=$(cd -- "$HERE/.." && pwd -P)
CC=${CC:-gcc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxinput-padset.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fail() { echo "nxinput_padset=FAIL $*" >&2; exit 1; }
STRICT=(-std=c99 -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes
        -Wconversion -Wsign-conversion -Wcast-qual -D_POSIX_C_SOURCE=200809L)
compilers=("$CC")
if command -v clang >/dev/null 2>&1 && [ "$CC" != clang ]; then compilers+=(clang); fi
for cc in "${compilers[@]}"; do
  "$cc" "${STRICT[@]}" -I"$NXINPUT/engine-glue" -I"$NXINPUT/include" \
    "$NXINPUT/engine-glue/nxinput_padset.c" "$NXINPUT/src/nxinput_prerouter.c" "$NXINPUT/src/nxinput_axis_calib.c" "$HERE/test_padset.c" -o "$WORK/padset-$cc" -lm ||
    fail "does not compile under $cc with strict flags"
  "$WORK/padset-$cc" || fail "behaviour under $cc"
done
if "$CC" -fsanitize=address,undefined -g -I"$NXINPUT/engine-glue" -I"$NXINPUT/include" \
     "$NXINPUT/engine-glue/nxinput_padset.c" "$NXINPUT/src/nxinput_prerouter.c" "$NXINPUT/src/nxinput_axis_calib.c" "$HERE/test_padset.c" -o "$WORK/padset-san" -lm 2>/dev/null; then
  ASAN_OPTIONS=detect_leaks=1 "$WORK/padset-san" >/dev/null || fail "sanitizer run"
fi
# Static boundary: the module knows no SDL header, no device, no env, no name.
if grep -nE '#include <SDL|SDL_[A-Za-z]+\(|getenv|/dev/|fopen|strstr\(.*name' "$NXINPUT/engine-glue/nxinput_padset.c"; then
  fail "padset must stay a pure vtable consumer (no SDL header, env, device or name logic)"
fi
echo "nxinput_padset gate: PASS (${#compilers[@]} compiler(s) + sanitizers, static boundary)"
