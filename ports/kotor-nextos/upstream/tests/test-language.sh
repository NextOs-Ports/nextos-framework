#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/kotor-language.XXXXXX")
cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/kotor-language.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/src/kotor_language.c" "$ROOT/tests/test_language.c" \
  -o "$TEST_ROOT/test-language"
"$TEST_ROOT/test-language"
printf 'KOTOR language adapter: PASS\n'
