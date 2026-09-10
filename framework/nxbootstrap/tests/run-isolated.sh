#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# The process-bearing tests have no host fallback. If a private user/PID/mount
# namespace cannot be created, this runner exits 77 without starting the suite.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

if [[ ${1:-} == --inside-private-pid-namespace ]]; then
  inner_mode=${2:-}
  # shellcheck source=private-pid-namespace.sh
  source "$TEST_DIR/private-pid-namespace.sh"
  nxbootstrap_require_private_pid_namespace || exit $?
  [[ $$ -eq 1 ]] || {
    printf 'nxbootstrap isolated tests: SKIP (namespace runner is not PID 1)\n' >&2
    exit 77
  }
  [[ -n ${NXBOOTSTRAP_TEST_NAMESPACE_MARKER:-} ]] || exit 77
  printf 'private-pid-namespace-ok\n' > "$NXBOOTSTRAP_TEST_NAMESPACE_MARKER"
  printf 'nxbootstrap isolated tests: pid_ns=%s user_ns=%s mount_ns=%s host_pid_ns=%s pid=%s\n' \
    "$(readlink /proc/self/ns/pid)" "$(readlink /proc/self/ns/user)" \
    "$(readlink /proc/self/ns/mnt)" "$NXBOOTSTRAP_TEST_HOST_PID_NS" "$$"
  if [[ $inner_mode == --probe-only ]]; then
    printf 'nxbootstrap isolated tests: private namespace probe passed\n'
    exit 0
  fi
  [[ -z $inner_mode ]] || exit 2
  # The suite includes boot-fastpath, which materializes a 4205-member closure
  # twice. Keep a finite emergency wall fuse without treating host contention
  # as a product regression; per-process CPU and test-level invariants remain
  # the effective scaling guards.
  exec python3 -B "$TEST_DIR/namespace-watchdog.py" \
    --wall-seconds 4096 --grace-seconds 5 --cpu-seconds 240 \
    --memory-mib 2048 --file-mib 512 --max-processes 512 -- \
    bash "$TEST_DIR/isolated-suite.sh"
fi

mode=${1:-}
case $mode in ''|--probe-only) ;; *)
  printf 'usage: %s [--probe-only]\n' "${0##*/}" >&2
  exit 2
  ;;
esac

command -v unshare >/dev/null 2>&1 || {
  printf 'nxbootstrap isolated tests: SKIP (unshare is unavailable)\n' >&2
  exit 77
}

exec {host_pid_ns_fd}</proc/self/ns/pid || exit 77
exec {host_user_ns_fd}</proc/self/ns/user || exit 77
exec {host_mount_ns_fd}</proc/self/ns/mnt || exit 77
host_pid_ns=$(readlink "/proc/self/fd/$host_pid_ns_fd" 2>/dev/null || true)
host_user_ns=$(readlink "/proc/self/fd/$host_user_ns_fd" 2>/dev/null || true)
host_mount_ns=$(readlink "/proc/self/fd/$host_mount_ns_fd" 2>/dev/null || true)
[[ -n $host_pid_ns && -n $host_user_ns && -n $host_mount_ns ]] || {
  printf 'nxbootstrap isolated tests: SKIP (cannot seal host namespaces)\n' >&2
  exit 77
}

guard_root=$(mktemp -d "${TMPDIR:-/tmp}/nxbootstrap-namespace-guard.XXXXXX")
marker=$guard_root/entered
cleanup() {
  case $guard_root in
    "${TMPDIR:-/tmp}"/nxbootstrap-namespace-guard.*)
      rm -rf -- "$guard_root"
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
  unshare --user --map-root-user --pid --fork --kill-child=KILL \
    --mount-proc \
    bash "$0" --inside-private-pid-namespace "$mode"
status=$?
set -e

if [[ ! -f $marker ]]; then
  printf 'nxbootstrap isolated tests: SKIP (private PID namespace could not start)\n' >&2
  exit 77
fi
exit "$status"
