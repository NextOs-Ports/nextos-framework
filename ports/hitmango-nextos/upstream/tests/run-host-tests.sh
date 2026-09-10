#!/usr/bin/env bash
set -euo pipefail

REPOSITORY_ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/hitmango-tests.XXXXXX")
cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/hitmango-tests.*) rm -rf -- "$TEST_ROOT" ;;
    *) printf 'refusing unsafe cleanup target: %s\n' "$TEST_ROOT" >&2 ;;
  esac
}
trap cleanup EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror -I "$REPOSITORY_ROOT/src" \
  "$REPOSITORY_ROOT/src/motion_stream.c" \
  "$REPOSITORY_ROOT/tests/test_motion_stream.c" \
  -o "$TEST_ROOT/test-motion-stream"
"$TEST_ROOT/test-motion-stream"

cc -std=c11 -Wall -Wextra -Werror -I "$REPOSITORY_ROOT/src" \
  "$REPOSITORY_ROOT/src/input_lifecycle.c" \
  "$REPOSITORY_ROOT/tests/test_input_lifecycle.c" \
  -o "$TEST_ROOT/test-input-lifecycle"
"$TEST_ROOT/test-input-lifecycle"

cc -std=c11 -Wall -Wextra -Werror -I "$REPOSITORY_ROOT/src" \
  "$REPOSITORY_ROOT/src/input_layout.c" \
  "$REPOSITORY_ROOT/src/input_lifecycle.c" \
  "$REPOSITORY_ROOT/src/motion_stream.c" \
  "$REPOSITORY_ROOT/src/touch_arbiter.c" \
  "$REPOSITORY_ROOT/tests/test_input_contract.c" \
  -o "$TEST_ROOT/test-input-contract"
"$TEST_ROOT/test-input-contract"

cc -std=c11 -Wall -Wextra -Werror -ffunction-sections \
  -I "$REPOSITORY_ROOT/src" \
  "$REPOSITORY_ROOT/src/egl_sdl.c" \
  "$REPOSITORY_ROOT/tests/test_egl_provider_retry.c" \
  -Wl,--gc-sections -o "$TEST_ROOT/test-egl-provider-retry"
for scenario in normal window-retry init-retry explicit; do
  "$TEST_ROOT/test-egl-provider-retry" "$scenario"
done
