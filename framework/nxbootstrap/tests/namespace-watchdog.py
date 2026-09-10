#!/usr/bin/env python3
"""Run one exact child with limits, exclusively inside the sealed test namespace."""

import argparse
import os
import resource
import signal
import subprocess
import sys


REFUSED = 77
TIMEOUT = 124


def refuse(message):
    print("namespace watchdog refused: %s" % message, file=sys.stderr)
    raise SystemExit(REFUSED)


def namespace_link(path):
    try:
        return os.readlink(path)
    except OSError:
        return ""


def require_private_namespace():
    if os.environ.get("NXBOOTSTRAP_TEST_PRIVATE_PID_NS") != "1":
        refuse("private namespace marker is absent")
    expected = {
        "pid": os.environ.get("NXBOOTSTRAP_TEST_HOST_PID_NS", ""),
        "user": os.environ.get("NXBOOTSTRAP_TEST_HOST_USER_NS", ""),
        "mnt": os.environ.get("NXBOOTSTRAP_TEST_HOST_MOUNT_NS", ""),
    }
    descriptors = {
        "pid": os.environ.get("NXBOOTSTRAP_TEST_HOST_PID_NS_FD", ""),
        "user": os.environ.get("NXBOOTSTRAP_TEST_HOST_USER_NS_FD", ""),
        "mnt": os.environ.get("NXBOOTSTRAP_TEST_HOST_MOUNT_NS_FD", ""),
    }
    for kind in ("pid", "user", "mnt"):
        if not expected[kind] or not descriptors[kind].isdigit():
            refuse("sealed %s namespace identity is absent" % kind)
        descriptor_target = namespace_link(
            "/proc/self/fd/%s" % descriptors[kind])
        if descriptor_target != expected[kind]:
            refuse("sealed %s namespace descriptor mismatch" % kind)
        current = namespace_link("/proc/self/ns/%s" % kind)
        if not current or current == expected[kind]:
            refuse("current %s namespace is not private" % kind)
    if namespace_link("/proc/self/ns/pid") != namespace_link("/proc/1/ns/pid"):
        refuse("private procfs does not belong to the current PID namespace")
    try:
        with open("/proc/1/status", "rb") as stream:
            if not stream.read(1):
                refuse("private PID 1 status is empty")
    except OSError:
        refuse("private PID 1 is unavailable")


def process_starttime(pid):
    try:
        with open("/proc/%d/stat" % pid, "r", encoding="ascii") as stream:
            stat = stream.read()
    except (OSError, UnicodeError):
        return None
    marker = stat.rfind(") ")
    if marker < 0:
        return None
    fields = stat[marker + 2:].split()
    if len(fields) < 20 or not fields[19].isdigit():
        return None
    return fields[19]


def same_process(pid, expected_starttime):
    return (expected_starttime is not None and
            process_starttime(pid) == expected_starttime)


class FenceError(Exception):
    """A required resource fence could not be established."""


def lower_limit(limit_name, desired):
    # V3-HARDENING-01 fail-closed: a limit that cannot be established is a
    # refusal, never a silent no-op. Running the child unfenced is exactly the
    # state these limits exist to prevent.
    if desired is None:
        return
    limit_id = getattr(resource, limit_name, None)
    if limit_id is None:
        raise FenceError("%s is unavailable on this host" % limit_name)
    try:
        _soft, hard = resource.getrlimit(limit_id)
    except (OSError, ValueError) as error:
        raise FenceError("%s cannot be read: %s" % (limit_name, error))
    target = desired
    if hard != resource.RLIM_INFINITY:
        target = min(target, hard)
    try:
        resource.setrlimit(limit_id, (target, target))
    except (OSError, ValueError) as error:
        raise FenceError("%s cannot be lowered: %s" % (limit_name, error))
    try:
        soft_now, _hard_now = resource.getrlimit(limit_id)
    except (OSError, ValueError) as error:
        raise FenceError("%s cannot be confirmed: %s" % (limit_name, error))
    if soft_now != target:
        raise FenceError("%s did not take the requested value" % limit_name)


def apply_limits(options):
    # NXBOOTSTRAP_TEST_FENCE_FAULT names one limit that must be simulated as
    # unavailable, so the fail-closed path itself is provable.
    fault = os.environ.get("NXBOOTSTRAP_TEST_FENCE_FAULT", "")
    for name, value in (
        ("RLIMIT_CORE", 0),
        ("RLIMIT_CPU", options.cpu_seconds),
        ("RLIMIT_AS", options.memory_mib * 1024 * 1024),
        ("RLIMIT_FSIZE", options.file_mib * 1024 * 1024),
        ("RLIMIT_NPROC", options.max_processes),
    ):
        if name == fault:
            raise FenceError("%s was injected as unavailable" % name)
        lower_limit(name, value)


def send_if_owned(process, starttime, selected_signal):
    if process.poll() is not None or not same_process(process.pid, starttime):
        return False
    os.kill(process.pid, selected_signal)
    return True


def parse_args(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--wall-seconds", type=int, default=180)
    parser.add_argument("--grace-seconds", type=int, default=5)
    parser.add_argument("--cpu-seconds", type=int, default=120)
    parser.add_argument("--memory-mib", type=int, default=2048)
    parser.add_argument("--file-mib", type=int, default=512)
    parser.add_argument("--max-processes", type=int, default=512)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    options = parser.parse_args(argv)
    if options.command and options.command[0] == "--":
        options.command = options.command[1:]
    for name in ("wall_seconds", "grace_seconds", "cpu_seconds",
                 "memory_mib", "file_mib", "max_processes"):
        value = getattr(options, name)
        if value < 1 or value > 4096:
            parser.error("--%s is outside the safe range" % name.replace("_", "-"))
    if not options.command:
        parser.error("a command is required after --")
    return options


def main(argv=None):
    options = parse_args(argv)
    require_private_namespace()
    try:
        process = subprocess.Popen(
            options.command,
            close_fds=False,
            preexec_fn=lambda: apply_limits(options),
        )
    except (subprocess.SubprocessError, FenceError, OSError) as error:
        refuse("resource fence could not be established: %s" % error)
    starttime = process_starttime(process.pid)
    if starttime is None:
        print("namespace watchdog: child starttime unavailable; failing closed",
              file=sys.stderr)
        return 125
    print("namespace watchdog: child_pid=%d starttime=%s wall=%ss cpu=%ss memory=%sMiB" %
          (process.pid, starttime, options.wall_seconds, options.cpu_seconds,
           options.memory_mib))
    try:
        return process.wait(timeout=options.wall_seconds)
    except subprocess.TimeoutExpired:
        print("namespace watchdog: wall timeout; TERM exact child pid=%d starttime=%s" %
              (process.pid, starttime), file=sys.stderr)
        send_if_owned(process, starttime, signal.SIGTERM)
        try:
            process.wait(timeout=options.grace_seconds)
        except subprocess.TimeoutExpired:
            print("namespace watchdog: grace timeout; KILL exact child pid=%d starttime=%s" %
                  (process.pid, starttime), file=sys.stderr)
            send_if_owned(process, starttime, signal.SIGKILL)
            process.wait()
        return TIMEOUT


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print("namespace watchdog failed: %s" % error, file=sys.stderr)
        sys.exit(125)
