#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Standalone host gate for the nxandroid input-sink interface. It compiles and
# runs only the new input-sink translation unit and its unit test. It never
# loads guest code, sends signals, accesses devices or opens the network.
set -euo pipefail

NXANDROID_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)

for required_command in cc grep; do
  command -v "$required_command" >/dev/null 2>&1 || {
    printf 'nxandroid-input-sinks-host: missing required command: %s\n' \
      "$required_command" >&2
    exit 77
  }
done

NXANDROID_WORK=$(mktemp -d /tmp/nxandroid-input-sinks.XXXXXX)
[[ -d $NXANDROID_WORK && ! -L $NXANDROID_WORK ]] || {
  printf 'nxandroid-input-sinks-host: mktemp did not create a safe work tree\n' >&2
  exit 1
}
cleanup() {
  local status=$?
  trap - EXIT
  case $NXANDROID_WORK in
    /tmp/nxandroid-input-sinks.??????)
      rm -rf -- "$NXANDROID_WORK" || status=1
      ;;
    *)
      printf 'nxandroid-input-sinks-host: refused cleanup outside owned mktemp tree\n' >&2
      status=1
      ;;
  esac
  if [[ -e $NXANDROID_WORK ]]; then
    printf 'nxandroid-input-sinks-host: cleanup_failed=1\n' >&2
    status=1
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

NXANDROID_TEST_BINARY=$NXANDROID_WORK/test_input_sinks
NXANDROID_TEST_LOG=$NXANDROID_WORK/test_input_sinks.log

cc -std=c99 -Wall -Wextra -Werror -O1 \
  -I "$NXANDROID_ROOT/include" \
  "$NXANDROID_ROOT/src/nxandroid_input_sinks.c" \
  "$NXANDROID_ROOT/tests/test_input_sinks.c" \
  -o "$NXANDROID_TEST_BINARY"

"$NXANDROID_TEST_BINARY" 2>&1 | tee "$NXANDROID_TEST_LOG"

grep -q '^nxandroid_input_sink_tests=PASS$' "$NXANDROID_TEST_LOG" || {
  printf 'nxandroid-input-sinks-host: PASS claim missing from test output\n' >&2
  exit 1
}

printf 'nxandroid input sinks host tests: PASS\n'
