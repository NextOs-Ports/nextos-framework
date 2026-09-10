#!/bin/sh
set -eu

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
OUT=${TMPDIR:-/tmp}/rcrdx-video-provider-test.$$
PROVIDER=${TMPDIR:-/tmp}/rcrdx-video-provider.$$.so
trap 'rm -f "$OUT" "$PROVIDER"' EXIT HUP INT TERM

cc -shared -fPIC -std=gnu11 -Wall -Wextra -Werror \
  "$TEST_DIR/fake_gl_provider.c" -o "$PROVIDER"

cc -std=gnu11 -O2 -ffunction-sections -fdata-sections \
  -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable \
  -I"$TEST_DIR/../src" "$TEST_DIR/test_video_provider.c" \
  -Wl,--gc-sections -ldl -o "$OUT"
"$OUT" "$PROVIDER"
echo "rcrdx video provider gate: PASS"
