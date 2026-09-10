#!/usr/bin/env bash
set -euo pipefail

HERE=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/fp2-pthread-sem.XXXXXX")
trap 'find "$TEST_ROOT" -depth -delete 2>/dev/null || true' EXIT
fail() {
  printf 'fp2 pthread bridge semaphore test FAIL: %s\n' "$1" >&2
  exit 1
}

build_and_run() {
  local variant=$1
  shift
  local bin=$TEST_ROOT/pthread-bridge-sem-$variant

  cc -std=c11 -Wall -Wextra -Werror -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    "$@" "$HERE/test_pthread_bridge_sem.c" -o "$bin" ||
    fail "$variant harness did not compile"

  env -u FP2_SEMLOG \
    ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$bin" || fail "$variant lifecycle/refcount cases failed"
}

build_and_run diagnostic
build_and_run release -DFP2_RELEASE_BUILD=1

printf '%s\n' \
  'fp2 pthread bridge semaphore variants: PASS diagnostic release (ASan/UBSan)'
