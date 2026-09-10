#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Host-only nxloader gate. It writes exclusively below its owned mktemp tree,
# sends no signals, opens no display/audio device and never runs guest code.
set -euo pipefail

NXLOADER_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
NXLOADER_BUILD_JOBS=${NXLOADER_BUILD_JOBS:-2}
NXLOADER_KEEP_WORK=${NXLOADER_KEEP_WORK:-0}

case $NXLOADER_BUILD_JOBS in
  ''|*[!0-9]*|0) printf 'nxloader-host: invalid build job count\n' >&2; exit 2 ;;
esac
if (( NXLOADER_BUILD_JOBS > 8 )); then
  printf 'nxloader-host: build job count exceeds safe maximum 8\n' >&2
  exit 2
fi
case $NXLOADER_KEEP_WORK in
  0|1) ;;
  *) printf 'nxloader-host: NXLOADER_KEEP_WORK must be 0 or 1\n' >&2; exit 2 ;;
esac

NXLOADER_WORK=$(mktemp -d /tmp/nxloader-host.XXXXXX)
NXLOADER_GCC_BUILD=$NXLOADER_WORK/gcc
NXLOADER_CLANG_BUILD=$NXLOADER_WORK/clang-sanitized
NXLOADER_CORPUS=$NXLOADER_WORK/corpus

cleanup() {
  local status=$?
  trap - EXIT
  if [[ $NXLOADER_KEEP_WORK == 1 ]]; then
    printf 'nxloader-host: retained_work_tree=%s\n' "$NXLOADER_WORK"
  else
    case $NXLOADER_WORK in
      /tmp/nxloader-host.??????)
        find "$NXLOADER_WORK" -depth -delete 2>/dev/null || status=1
        ;;
      *)
        printf 'nxloader-host: refused cleanup outside owned mktemp\n' >&2
        status=1
        ;;
    esac
    if [[ -e $NXLOADER_WORK ]]; then
      printf 'nxloader-host: cleanup_failed=1\n' >&2
      status=1
    else
      printf 'nxloader-host: work_tree_cleaned=1\n'
    fi
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

command -v cmake >/dev/null
command -v gcc >/dev/null
command -v clang >/dev/null
command -v find >/dev/null

cmake -S "$NXLOADER_ROOT" -B "$NXLOADER_GCC_BUILD" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc \
  -DNXLOADER_BUILD_TESTS=ON \
  -DNXLOADER_BUILD_TOOLS=ON \
  -DNXLOADER_BUILD_FUZZER=OFF
cmake --build "$NXLOADER_GCC_BUILD" --parallel "$NXLOADER_BUILD_JOBS"
ctest --test-dir "$NXLOADER_GCC_BUILD" --output-on-failure

cmake -S "$NXLOADER_ROOT" -B "$NXLOADER_CLANG_BUILD" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DNXLOADER_BUILD_TESTS=ON \
  -DNXLOADER_BUILD_TOOLS=ON \
  -DNXLOADER_BUILD_FUZZER=ON \
  -DNXLOADER_ENABLE_SANITIZERS=ON
cmake --build "$NXLOADER_CLANG_BUILD" --parallel "$NXLOADER_BUILD_JOBS"
ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:strict_string_checks=1:quarantine_size_mb=32 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir "$NXLOADER_CLANG_BUILD" --output-on-failure

mkdir -- "$NXLOADER_CORPUS"
"$NXLOADER_CLANG_BUILD/nxloader_tests" \
  --write-fuzz-corpus "$NXLOADER_CORPUS"
ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:strict_string_checks=1:quarantine_size_mb=32 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$NXLOADER_CLANG_BUILD/nxloader_fuzz" \
    -runs=20000 -max_len=65536 "$NXLOADER_CORPUS"

clang --analyze -std=c99 -Wall -Wextra -Wpedantic \
  -I"$NXLOADER_ROOT/include" -I"$NXLOADER_ROOT/src" \
  "$NXLOADER_ROOT/src/nxloader.c" \
  "$NXLOADER_ROOT/src/nxloader_elf32.c" \
  "$NXLOADER_ROOT/src/nxloader_elf64.c" \
  "$NXLOADER_ROOT/src/nxloader_hooks.c" \
  "$NXLOADER_ROOT/src/nxloader_protect.c" \
  "$NXLOADER_ROOT/src/nxloader_registry.c"

printf 'nxloader-host: PASS gcc=1 clang_asan_ubsan=1 fuzz_runs=20000 analyze=1\n'
printf 'nxloader-host: build_jobs=%s hardware_ran=0 device_access=0 ' \
  "$NXLOADER_BUILD_JOBS"
printf 'guest_initializers_executed=0 guest_jni_onload_executed=0\n'
