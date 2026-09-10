#!/usr/bin/env bash
set -euo pipefail

PORT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/asm2-first-accept-test.XXXXXX")
trap 'rm -rf -- "$BUILD_DIR"' EXIT INT TERM

${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$PORT_DIR/src" \
  "$PORT_DIR/tests/first_accept_geometry_test.c" \
  "$PORT_DIR/src/first_accept.c" \
  -o "$BUILD_DIR/first_accept_geometry_test"

"$BUILD_DIR/first_accept_geometry_test"

# Controller label mappings vary across handhelds. The update-log recovery
# must never forward the triggering face button as B/X/Y.
grep -Fq 'send_hid(input, ASM2_HID_A, 1.0);' "$PORT_DIR/src/input.c"
grep -Fq 'send_hid(input, ASM2_HID_A, 0.0);' "$PORT_DIR/src/input.c"
printf 'first-run logical confirmation: PASS\n'
