#!/usr/bin/env bash
set -euo pipefail

HERE=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/fp2-frame-proof.XXXXXX")
trap 'find "$TEST_ROOT" -depth -delete 2>/dev/null || true' EXIT
BIN=$TEST_ROOT/frame-proof-partial

fail() {
  printf 'fp2 frame-proof test FAIL: %s\n' "$1" >&2
  exit 1
}

cc -std=c11 -Wall -Wextra -Werror -O1 \
  "$HERE/test_frame_proof_partial.c" \
  "$HERE/../src/nxgl_frame_proof_adapter.c" -ldl -o "$BIN" ||
  fail 'harness did not compile'

run_case() {
  env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT \
    NXLAUNCH_FRONTEND=1 "$BIN" "$1"
}

out=$(run_case partial-three)
grep -q 'visible_non_black=0.2%' <<<"$out" ||
  fail "partial pixels were not measured: $out"
grep -q 'IMAGE PROOF' <<<"$out" &&
  fail "three partial frames became fatal BLACK: $out"
grep -q 'verdict=BLACK' <<<"$out" &&
  fail "partial frames published BLACK: $out"

out=$(run_case zero-three)
grep -q 'IMAGE PROOF: black-streak=3' <<<"$out" ||
  fail "three genuinely empty frames did not become BLACK: $out"
grep -q 'verdict=BLACK' <<<"$out" ||
  fail "zero-three omitted the BLACK verdict: $out"

out=$(run_case black-partial-black)
grep -q 'IMAGE PROOF' <<<"$out" &&
  fail "a partial frame failed to reset the black streak: $out"
grep -q 'verdict=BLACK' <<<"$out" &&
  fail "black-partial-black published BLACK: $out"

out=$(run_case ok-then-zero-three)
grep -q 'gl: frame proof verdict=OK' <<<"$out" ||
  fail "OK-then-black scenario never established the initial OK: $out"
grep -q 'IMAGE PROOF: black-streak=3' <<<"$out" ||
  fail "OK followed by three empty frames did not become fatal: $out"
grep -q 'gl: frame proof verdict=BLACK' <<<"$out" ||
  fail "OK followed by three empty frames omitted the fatal BLACK verdict: $out"
grep -q 'VIDEO: .*verdict=BLACK reason=all-black' <<<"$out" ||
  fail "OK followed by three empty frames kept a nonfatal VIDEO result: $out"
grep -q '"status":"fail","reason_code":6301' <<<"$out" ||
  fail "OK followed by three empty frames omitted the BLACK failure event: $out"

out=$(run_case rgb-alpha-zero)
grep -q 'IMAGE PROOF: black-streak=3' <<<"$out" ||
  fail "RGB with zero alpha stopped being a BLACK candidate: $out"
grep -q 'reason=alpha-zero' <<<"$out" ||
  fail "RGB with zero alpha lost its specific receipt reason: $out"

out=$(env -u SSH_CONNECTION -u SSH_TTY -u SSH_CLIENT \
  NXLAUNCH_FRONTEND=1 NXGL_IMAGE_PROOF=0 "$BIN" proof-off)
grep -q 'frame probe' <<<"$out" &&
  fail "NXGL_IMAGE_PROOF=0 still sampled frames: $out"

printf '%s\n' \
  'fp2 frame-proof test: PASS cases=6 partial-visible never BLACK; zero/alpha-zero remain fatal'
