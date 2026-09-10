#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# V4-GRAPHICS-04 hermetic Wayland fixture.
#
# Starts an ISOLATED headless Wayland compositor on a private socket (never
# the user session), then runs a REAL SDL2/GLES guest through the two-phase
# gate:
#   1. preflight before any present is PENDING and returns control;
#   2. the guest reaches draw + its first REAL SDL_GL_SwapWindow;
#   3. only after that commit does the drawable count and the one-shot
#      receipt appear, carrying phase=post-first-present;
#   4. a window that NEVER presents stays pending forever: no OK, no receipt.
# No sleep is ever declared as success and no present is fabricated: the
# verdict comes from the gate observing the guest's own commit.
#
# This is a host proof of the Wayland lifecycle contract; it is NOT a claim
# about any physical firmware.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NXGL=$(cd -- "$HERE/.." && pwd -P)
CC=${CC:-gcc}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxgl-v4-wayland.XXXXXX")
COMPOSITOR_PID=""
SOCKET="nxgl-v4-gate-$$"
cleanup() {
  if [ -n "$COMPOSITOR_PID" ] && kill -0 "$COMPOSITOR_PID" 2>/dev/null; then
    kill "$COMPOSITOR_PID" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      kill -0 "$COMPOSITOR_PID" 2>/dev/null || break
      sleep 0.2
    done
    kill -9 "$COMPOSITOR_PID" 2>/dev/null || true
  fi
  rm -f "${XDG_RUNTIME_DIR:-/tmp}/$SOCKET" \
        "${XDG_RUNTIME_DIR:-/tmp}/$SOCKET.lock" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

fail() { echo "nxgl_v4_wayland=FAIL $*" >&2; exit 1; }

[ -n "${XDG_RUNTIME_DIR:-}" ] || fail "XDG_RUNTIME_DIR is required"
command -v pkg-config >/dev/null || fail "pkg-config is required"
pkg-config --exists sdl2 || fail "SDL2 development files are required"

STRICT=(-std=c99 -Wall -Wextra -Werror -Wformat=2 -Wshadow
        -Wstrict-prototypes)

# ---------------------------------------------------------------- compositor
# Weston headless when available; otherwise mutter --headless with a virtual
# monitor. Both are real Wayland compositors on an isolated private socket.
if command -v weston >/dev/null 2>&1; then
  weston --backend=headless-backend.so --socket="$SOCKET" \
    --width=800 --height=600 --idle-time=0 \
    > "$WORK/compositor.log" 2>&1 &
  COMPOSITOR_PID=$!
  COMPOSITOR_KIND=weston-headless
elif command -v mutter >/dev/null 2>&1; then
  mutter --headless --wayland --no-x11 --virtual-monitor 800x600 \
    --wayland-display="$SOCKET" > "$WORK/compositor.log" 2>&1 &
  COMPOSITOR_PID=$!
  COMPOSITOR_KIND=mutter-headless
else
  fail "no headless Wayland compositor (weston or mutter) is available"
fi

for _ in $(seq 1 50); do
  [ -S "$XDG_RUNTIME_DIR/$SOCKET" ] && break
  kill -0 "$COMPOSITOR_PID" 2>/dev/null || fail "the compositor died at start"
  sleep 0.2
done
[ -S "$XDG_RUNTIME_DIR/$SOCKET" ] || fail "the compositor socket never appeared"

# -------------------------------------------------------------------- build
"$CC" "${STRICT[@]}" -D_POSIX_C_SOURCE=200809L \
  -I "$NXGL/include" -I "$NXGL/adapters" $(pkg-config --cflags sdl2) \
  -o "$WORK/gate-guest" \
  "$HERE/wayland_gate_guest.c" \
  "$NXGL/adapters/nxgl_graphics_contract_adapter.c" \
  "$NXGL/src/nxgl_graphics_present_gate.c" \
  "$NXGL/src/nxgl_graphics_contract.c" \
  $(pkg-config --libs sdl2) -lGLESv2 -lpthread -ldl ||
  fail "the Wayland fixture guest did not compile cleanly"

run_guest() { # $1 = mode, $2 = log
  env -i \
    HOME="$HOME" PATH="$PATH" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    WAYLAND_DISPLAY="$SOCKET" \
    SDL_VIDEODRIVER=wayland \
    NXOBS_RUN_ID="waylandfixture-$$" \
    NX_GENERATION="hermetic-host-fixture" \
    NX_PORT_ID="nxglv4fixture" \
    NX_PORT_VERSION="0.3.1" \
    timeout -s KILL 60 "$WORK/gate-guest" "$1" > "$2" 2>&1
}

# --------------------------------------------------- 1. real present path
run_guest present "$WORK/present.log" ||
  fail "the present path did not conclude (see $WORK/present.log)"
grep -Fq 'VIDEO-DRIVER: wayland' "$WORK/present.log" ||
  fail "the guest did not run on the wayland backend"
grep -Eq 'PREFLIGHT: status=awaiting-first-present' "$WORK/present.log" ||
  fail "the preflight did not come back pending"
grep -Fq 'VERDICT: status=proved' "$WORK/present.log" ||
  fail "the gate did not prove after the first commit"
grep -Eq 'RECEIPT: GRAPHICS-EVIDENCE: .*phase=post-first-present' \
  "$WORK/present.log" || fail "the receipt lacks phase=post-first-present"
grep -Eq 'RECEIPT: .*first_present=1' "$WORK/present.log" ||
  fail "the receipt lacks first_present=1"
grep -Eq 'RECEIPT: .*verdict=OK reason=ok' "$WORK/present.log" ||
  fail "the receipt verdict is not OK"
grep -Fq 'ONE-SHOT: ok' "$WORK/present.log" ||
  fail "the conclusion was not one-shot"
# The proven drawable must be usable (>1x1).
DRAW=$(sed -n 's/^VERDICT: .*drawable=\([0-9]*x[0-9]*\) .*/\1/p' \
  "$WORK/present.log" | head -1)
case "$DRAW" in
  ''|0x*|*x0|1x1) fail "the proven drawable '$DRAW' is not usable" ;;
esac

# ------------------------------------------------ 2. window that never presents
set +e
run_guest no-present "$WORK/no-present.log"
NO_PRESENT_RC=$?
set -e
[ "$NO_PRESENT_RC" -eq 3 ] ||
  fail "the no-present window did not hold fail-closed (rc=$NO_PRESENT_RC)"
grep -Fq 'NO-PRESENT: state=awaiting-first-present final=0 receipt=absent' \
  "$WORK/no-present.log" ||
  fail "the no-present hold was not recorded"
! grep -Fq 'RECEIPT:' "$WORK/no-present.log" ||
  fail "a window without a present produced a receipt"

echo "nxgl_v4_wayland=PASS compositor=$COMPOSITOR_KIND drawable=$DRAW present=1 no_present_fail_closed=1"
