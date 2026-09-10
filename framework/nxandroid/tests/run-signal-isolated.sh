#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# The nxandroid signal fixture has no host fallback. It is built and executed
# only after the canonical sealed user/PID/mount namespace guard succeeds.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
NXANDROID_ROOT=$(cd -- "$TEST_DIR/.." && pwd -P)
PROJECT_ROOT=$(cd -- "$NXANDROID_ROOT/../.." && pwd -P)
NXBOOTSTRAP_TEST_DIR=$PROJECT_ROOT/framework/nxbootstrap/tests
PRIVATE_GUARD=$NXBOOTSTRAP_TEST_DIR/private-pid-namespace.sh
NAMESPACE_WATCHDOG=$NXBOOTSTRAP_TEST_DIR/namespace-watchdog.py

assert_line_once() {
  local output=$1 expected=$2 count
  count=$(grep -Fxc -- "$expected" "$output" || true)
  [[ $count -eq 1 ]] || {
    printf 'nxandroid signal gate: expected exactly once: %s (found %s)\n' \
      "$expected" "$count" >&2
    return 1
  }
}

assert_case_once() {
  local output=$1 case_name=$2 count
  count=$(grep -Ec \
    "^nxandroid-signal: case=${case_name} child_pid=[1-9][0-9]* pidfd=[0-9]+ PASS$" \
    "$output" || true)
  [[ $count -eq 1 ]] || {
    printf 'nxandroid signal gate: invalid %s case proof (found %s)\n' \
      "$case_name" "$count" >&2
    return 1
  }
}

audit_output() {
  local output=$1
  assert_case_once "$output" active
  assert_case_once "$output" early
  assert_line_once "$output" nxandroid_signal_test=PASS
  assert_line_once "$output" private_pid_namespace=1
  assert_line_once "$output" signal_authority=pidfd
  assert_line_once "$output" active_forward_exactly_once=1
  assert_line_once "$output" early_rollback_exactly_once=1
  assert_line_once "$output" sibling_intact=1
  assert_line_once "$output" guest_code_executed=0
  assert_line_once "$output" guest_initializers_executed=0
  assert_line_once "$output" guest_jni_onload_executed=0
  assert_line_once "$output" device_access=0
  assert_line_once "$output" network_access=0
  assert_line_once "$output" hardware_ran=0
  ! grep -Eq '(^|[[:space:]])(FAIL|SKIP)([[:space:]]|$)' "$output"
}

run_compiler_gate() {
  local label=$1 compiler=$2
  local build_dir=$NXANDROID_SIGNAL_WORK_ROOT/build-$label
  local output=$NXANDROID_SIGNAL_WORK_ROOT/output-$label.txt
  local binary=$build_dir/nxandroid_signal_test
  local status

  printf 'nxandroid signal gate: configuring compiler=%s path=%s\n' \
    "$label" "$(command -v "$compiler")"
  cmake -S "$NXANDROID_ROOT" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$compiler" \
    -DNXANDROID_BUILD_TESTS=OFF \
    -DNXANDROID_BUILD_SIGNAL_TEST=ON \
    -DNXANDROID_ENABLE_SANITIZERS=OFF \
    -DNXANDROID_WARNINGS_AS_ERRORS=ON
  cmake --build "$build_dir" --parallel 2 --target nxandroid_signal_test

  set +e
  "$binary" >"$output" 2>&1
  status=$?
  set -e
  cat -- "$output"
  if [[ $status -eq 77 ]]; then
    printf 'nxandroid signal gate: SKIP pidfd/namespace authority unavailable\n' \
      >&2
    exit 77
  fi
  [[ $status -eq 0 ]] || {
    printf 'nxandroid signal gate: %s fixture exited %s\n' \
      "$label" "$status" >&2
    return 1
  }
  audit_output "$output"
  printf 'nxandroid_signal_%s_binary_sha256=%s\n' "$label" \
    "$(sha256sum -- "$binary" | awk '{print $1}')"
  printf 'nxandroid_signal_%s_gate=PASS\n' "$label"
}

inside_suite() {
  # shellcheck source=../../nxbootstrap/tests/private-pid-namespace.sh
  source "$PRIVATE_GUARD"
  nxbootstrap_require_private_pid_namespace || exit $?
  case ${NXANDROID_SIGNAL_WORK_ROOT:-} in
    /tmp/nxandroid-signal-isolated.*) ;;
    *)
      printf 'nxandroid signal gate: unsafe or missing owned work root\n' >&2
      exit 77
      ;;
  esac
  [[ -d $NXANDROID_SIGNAL_WORK_ROOT && ! -L $NXANDROID_SIGNAL_WORK_ROOT ]] || {
    printf 'nxandroid signal gate: owned work root is unavailable\n' >&2
    exit 77
  }

  cd -- "$PROJECT_ROOT"
  printf 'nxandroid_signal_source_manifest_begin=1\n'
  sha256sum -- \
    framework/nxandroid/CMakeLists.txt \
    framework/nxandroid/VERSION \
    framework/nxandroid/README.md \
    framework/nxandroid/include/nxandroid.h \
    framework/nxandroid/src/nxandroid.c \
    framework/nxandroid/src/nxandroid_imports.c \
    framework/nxandroid/tests/test_signal.c \
    framework/nxandroid/tests/run-signal-isolated.sh \
    framework/nxbootstrap/tests/private-pid-namespace.sh \
    framework/nxbootstrap/tests/namespace-watchdog.py
  printf 'nxandroid_signal_source_manifest_end=1\n'
  printf 'nxandroid_signal_gcc_version=%s\n' "$(gcc -dumpfullversion -dumpversion)"
  printf 'nxandroid_signal_clang_version=%s\n' \
    "$(clang --version | sed -n '1p')"

  run_compiler_gate gcc gcc
  run_compiler_gate clang clang

  printf 'nxandroid_signal_isolated_gate=PASS\n'
  printf 'sealed_user_pid_mount_namespace=1\n'
  printf 'host_fallback=0\n'
  printf 'signal_authority=pidfd\n'
  printf 'sibling_signaled=0\n'
  printf 'guest_code_executed=0\n'
  printf 'guest_initializers_executed=0\n'
  printf 'guest_jni_onload_executed=0\n'
  printf 'device_access=0\n'
  printf 'network_access=0\n'
  printf 'hardware_ran=0\n'
}

if [[ ${1:-} == --inside-private-pid-namespace ]]; then
  [[ $# -eq 1 ]] || exit 2
  # shellcheck source=../../nxbootstrap/tests/private-pid-namespace.sh
  source "$PRIVATE_GUARD"
  nxbootstrap_require_private_pid_namespace || exit $?
  [[ $$ -eq 1 ]] || {
    printf 'nxandroid signal gate: SKIP namespace runner is not PID 1\n' >&2
    exit 77
  }
  [[ -n ${NXBOOTSTRAP_TEST_NAMESPACE_MARKER:-} ]] || exit 77
  printf 'private-pid-namespace-ok\n' >"$NXBOOTSTRAP_TEST_NAMESPACE_MARKER"
  printf 'nxandroid signal gate: pid_ns=%s user_ns=%s mount_ns=%s pid=%s\n' \
    "$(readlink /proc/self/ns/pid)" "$(readlink /proc/self/ns/user)" \
    "$(readlink /proc/self/ns/mnt)" "$$"
  exec python3 -B "$NAMESPACE_WATCHDOG" \
    --wall-seconds 180 --grace-seconds 5 --cpu-seconds 120 \
    --memory-mib 2048 --file-mib 512 --max-processes 128 -- \
    bash "$0" --inside-suite
fi

if [[ ${1:-} == --inside-suite ]]; then
  [[ $# -eq 1 ]] || exit 2
  inside_suite
  exit 0
fi

[[ $# -eq 0 ]] || {
  printf 'usage: %s\n' "${0##*/}" >&2
  exit 2
}

for required_command in unshare python3 cmake gcc clang sha256sum awk grep sed; do
  command -v "$required_command" >/dev/null 2>&1 || {
    printf 'nxandroid signal gate: SKIP required command unavailable: %s\n' \
      "$required_command" >&2
    exit 77
  }
done
[[ -r $PRIVATE_GUARD && -r $NAMESPACE_WATCHDOG ]] || {
  printf 'nxandroid signal gate: SKIP canonical namespace harness unavailable\n' \
    >&2
  exit 77
}

exec {host_pid_ns_fd}</proc/self/ns/pid || exit 77
exec {host_user_ns_fd}</proc/self/ns/user || exit 77
exec {host_mount_ns_fd}</proc/self/ns/mnt || exit 77
host_pid_ns=$(readlink "/proc/self/fd/$host_pid_ns_fd" 2>/dev/null || true)
host_user_ns=$(readlink "/proc/self/fd/$host_user_ns_fd" 2>/dev/null || true)
host_mount_ns=$(readlink "/proc/self/fd/$host_mount_ns_fd" 2>/dev/null || true)
[[ -n $host_pid_ns && -n $host_user_ns && -n $host_mount_ns ]] || {
  printf 'nxandroid signal gate: SKIP cannot seal host namespace identities\n' \
    >&2
  exit 77
}

work_root=$(mktemp -d /tmp/nxandroid-signal-isolated.XXXXXX)
marker=$work_root/entered
cleanup() {
  case $work_root in
    /tmp/nxandroid-signal-isolated.*)
      rm -rf -- "$work_root"
      ;;
  esac
}
trap cleanup EXIT INT TERM

set +e
NXBOOTSTRAP_TEST_PRIVATE_PID_NS=1 \
NXBOOTSTRAP_TEST_HOST_PID_NS=$host_pid_ns \
NXBOOTSTRAP_TEST_HOST_USER_NS=$host_user_ns \
NXBOOTSTRAP_TEST_HOST_MOUNT_NS=$host_mount_ns \
NXBOOTSTRAP_TEST_HOST_PID_NS_FD=$host_pid_ns_fd \
NXBOOTSTRAP_TEST_HOST_USER_NS_FD=$host_user_ns_fd \
NXBOOTSTRAP_TEST_HOST_MOUNT_NS_FD=$host_mount_ns_fd \
NXBOOTSTRAP_TEST_NAMESPACE_MARKER=$marker \
NXANDROID_SIGNAL_WORK_ROOT=$work_root \
  unshare --user --map-root-user --pid --fork --kill-child=KILL \
    --mount-proc \
    bash "$0" --inside-private-pid-namespace
status=$?
set -e

if [[ ! -f $marker ]]; then
  printf 'nxandroid signal gate: SKIP private namespace could not start\n' >&2
  exit 77
fi
exit "$status"
