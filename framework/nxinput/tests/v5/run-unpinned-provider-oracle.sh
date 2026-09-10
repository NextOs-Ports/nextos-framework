#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# nxinput 0.11.1 -- the review's RED case for an UNPINNED provider (matrix M1c
# case 2): the host's libSDL2 is copied with 16 bytes appended (same ELF,
# different sha256 => no pin, no ById => UNKNOWN to the framework) and put
# first in LD_LIBRARY_PATH. Before 0.11.1 the seam blocked the pad (mute).
# Now: stock mode -- the CFW line stays in the environment (or is reinstated
# under legacy staging), the pad is admitted, and every position delivers
# what stock SDL delivers on THIS provider (the oracle knows the host's real
# numbering; the framework does not). Needs /dev/uinput and the host SDL2.
# usage: run-unpinned-provider-oracle.sh <out-dir> [host-domain=sdl2-evdev]
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NX=$(cd -- "$HERE/../.." && pwd -P)
OUT=${1:?out dir}; DOMAIN=${2:-sdl2-evdev}
mkdir -p "$OUT/lib"
real=$(python3 - <<'PY'
import ctypes.util, subprocess, os
n = ctypes.util.find_library("SDL2-2.0") or "libSDL2-2.0.so.0"
p = subprocess.run(["sh", "-c", "ldconfig -p | grep -m1 '%s' | sed 's/.*=> //'" % n], capture_output=True, text=True).stdout.strip()
print(os.path.realpath(p))
PY
)
[ -f "$real" ] || { echo "no host libSDL2 found" >&2; exit 2; }
cp "$real" "$OUT/lib/libSDL2-2.0.so.0"
printf 'NXINPUT-UNPINNED\0' >> "$OUT/lib/libSDL2-2.0.so.0"   # same code, different bytes: no pin can match
echo "real=$(sha256sum "$real" | cut -c1-16) unpinned=$(sha256sum "$OUT/lib/libSDL2-2.0.so.0" | cut -c1-16)"
bash "$NX/tests/v5/build-harness.sh" "$OUT/harness" "${CC:-gcc}" >/dev/null
for mode in "" "--legacy-stage"; do
  d="$OUT/oracle${mode:+-legacy}"
  LD_LIBRARY_PATH="$OUT/lib" python3 "$NX/tests/v5/run-host-oracle.py" "$NX/tests/v5/fixtures/incident-fp2-muos-h700.json" "$OUT/harness" "$d" --stock "$DOMAIN" $mode > "$d.log" 2>&1 || { tail -12 "$d.log"; echo "unpinned-provider-oracle: FAIL (${mode:-stage-v5})"; exit 1; }
  grep -E 'UNKNOWN|stock|reinstated|LEFT' "$d.log" | head -6
done
echo "unpinned-provider-oracle: PASS (stage-v5 + legacy-stage) domain=$DOMAIN"
