#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Guard shared by every nxbootstrap test that creates or supervises processes.

nxbootstrap_require_private_pid_namespace() {
  local current_ns init_ns current_user_ns current_mount_ns
  local host_ns=${NXBOOTSTRAP_TEST_HOST_PID_NS:-}
  local host_user_ns=${NXBOOTSTRAP_TEST_HOST_USER_NS:-}
  local host_mount_ns=${NXBOOTSTRAP_TEST_HOST_MOUNT_NS:-}
  local host_pid_fd=${NXBOOTSTRAP_TEST_HOST_PID_NS_FD:-}
  local host_user_fd=${NXBOOTSTRAP_TEST_HOST_USER_NS_FD:-}
  local host_mount_fd=${NXBOOTSTRAP_TEST_HOST_MOUNT_NS_FD:-}
  local fd_target

  [[ ${NXBOOTSTRAP_TEST_PRIVATE_PID_NS:-0} == 1 ]] || {
    printf 'nxbootstrap process test refused: private PID namespace marker is absent\n' >&2
    return 77
  }
  [[ -n $host_ns && -n $host_user_ns && -n $host_mount_ns ]] || {
    printf 'nxbootstrap process test refused: parent namespace identities are absent\n' >&2
    return 77
  }
  for descriptor in "$host_pid_fd" "$host_user_fd" "$host_mount_fd"; do
    case $descriptor in ''|*[!0-9]*)
      printf 'nxbootstrap process test refused: sealed namespace descriptor is absent\n' >&2
      return 77
      ;;
    esac
    [[ -r /proc/self/fd/$descriptor ]] || {
      printf 'nxbootstrap process test refused: sealed namespace descriptor is closed\n' >&2
      return 77
    }
  done
  fd_target=$(readlink "/proc/self/fd/$host_pid_fd" 2>/dev/null || true)
  [[ $fd_target == "$host_ns" ]] || {
    printf 'nxbootstrap process test refused: host PID namespace descriptor mismatch\n' >&2
    return 77
  }
  fd_target=$(readlink "/proc/self/fd/$host_user_fd" 2>/dev/null || true)
  [[ $fd_target == "$host_user_ns" ]] || {
    printf 'nxbootstrap process test refused: host user namespace descriptor mismatch\n' >&2
    return 77
  }
  fd_target=$(readlink "/proc/self/fd/$host_mount_fd" 2>/dev/null || true)
  [[ $fd_target == "$host_mount_ns" ]] || {
    printf 'nxbootstrap process test refused: host mount namespace descriptor mismatch\n' >&2
    return 77
  }
  [[ -r /proc/self/status && -r /proc/1/status ]] || {
    printf 'nxbootstrap process test refused: private procfs is unavailable\n' >&2
    return 77
  }
  current_ns=$(readlink /proc/self/ns/pid 2>/dev/null || true)
  init_ns=$(readlink /proc/1/ns/pid 2>/dev/null || true)
  [[ -n $current_ns && $current_ns == "$init_ns" ]] || {
    printf 'nxbootstrap process test refused: procfs does not belong to the current PID namespace\n' >&2
    return 77
  }
  [[ $current_ns != "$host_ns" ]] || {
    printf 'nxbootstrap process test refused: still inside the host PID namespace\n' >&2
    return 77
  }
  current_user_ns=$(readlink /proc/self/ns/user 2>/dev/null || true)
  [[ -n $current_user_ns && $current_user_ns != "$host_user_ns" ]] || {
    printf 'nxbootstrap process test refused: still inside the host user namespace\n' >&2
    return 77
  }
  current_mount_ns=$(readlink /proc/self/ns/mnt 2>/dev/null || true)
  [[ -n $current_mount_ns && $current_mount_ns != "$host_mount_ns" ]] || {
    printf 'nxbootstrap process test refused: still inside the host mount namespace\n' >&2
    return 77
  }
  [[ $(stat -f -c %T /proc 2>/dev/null || true) == proc ]] || {
    printf 'nxbootstrap process test refused: /proc is not a procfs mount\n' >&2
    return 77
  }
  return 0
}
