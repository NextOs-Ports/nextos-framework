#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/namespace-watchdog-test.XXXXXX")
FORBIDDEN_MARKER=$TEST_ROOT/host-fallback-ran

cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/namespace-watchdog-test.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'namespace watchdog test failed: %s\n' "$*" >&2
  exit 1
}

set +e
timeout_output=$(python3 -B "$TEST_DIR/namespace-watchdog.py" \
  --wall-seconds 1 --grace-seconds 2 --cpu-seconds 10 \
  --memory-mib 256 --file-mib 16 --max-processes 64 -- \
  bash -c 'trap "exit 0" TERM; while :; do sleep 1; done' 2>&1)
timeout_status=$?
set -e
[[ $timeout_status -eq 124 ]] ||
  fail "timeout returned $timeout_status instead of 124"
grep -F 'TERM exact child pid=' <<< "$timeout_output" >/dev/null ||
  fail 'watchdog timeout did not log exact PID/starttime ownership'

set +e
refusal_output=$(env -u NXBOOTSTRAP_TEST_PRIVATE_PID_NS \
  python3 -B "$TEST_DIR/namespace-watchdog.py" --wall-seconds 1 -- \
  bash -c 'printf forbidden > "$1"' _ "$FORBIDDEN_MARKER" 2>&1)
refusal_status=$?
set -e
[[ $refusal_status -eq 77 && ! -e $FORBIDDEN_MARKER ]] ||
  fail 'watchdog did not fail closed when its namespace marker was absent'
grep -F 'private namespace marker is absent' <<< "$refusal_output" >/dev/null ||
  fail 'watchdog refusal did not identify the missing namespace marker'

# V3-HARDENING-01 fail-closed: a required resource fence that cannot be
# established refuses the run. The child must never execute unfenced.
for fenced in RLIMIT_CORE RLIMIT_CPU RLIMIT_AS RLIMIT_FSIZE RLIMIT_NPROC; do
  rm -f -- "$FORBIDDEN_MARKER"
  set +e
  fence_output=$(NXBOOTSTRAP_TEST_FENCE_FAULT=$fenced \
    python3 -B "$TEST_DIR/namespace-watchdog.py" --wall-seconds 5 -- \
    bash -c 'printf forbidden > "$1"' _ "$FORBIDDEN_MARKER" 2>&1)
  fence_status=$?
  set -e
  [[ $fence_status -eq 77 ]] ||
    fail "$fenced fence failure returned $fence_status instead of 77"
  [[ ! -e $FORBIDDEN_MARKER ]] ||
    fail "$fenced fence failure still executed the child"
  grep -F 'resource fence could not be established' <<< "$fence_output" >/dev/null ||
    fail "$fenced fence failure did not identify itself"
done
rm -f -- "$FORBIDDEN_MARKER"

printf 'namespace watchdog timeout/refusal/fence test passed\n'
