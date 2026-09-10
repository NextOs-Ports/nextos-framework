#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""One C5B attempt: one exclusive directory, one log, one command, once.

WHAT THE 116A AUDIT REFUSED
---------------------------
The first C5A battery failed on word-split paths. The second truncated and
overwrote the SAME C5A-FINAL-BATTERY.log, so the immutable evidence of the
failure was destroyed, and a green log afterwards could not restore it. The
runner also re-executed fixtures just to collect a tail, while claiming each
gate ran exactly once.

WHAT THIS DOES
--------------
* The attempt directory is created with `os.makedirs` and no `exist_ok`, and
  the log with O_CREAT|O_EXCL: if either exists the attempt REFUSES to start.
  Nothing here can truncate or reuse a previous log, whatever its name.
* The manifest is written BEFORE the run: attempt id, UTC, heads, trees, the
  hashes of every script, binary and fixture, the single command, the ordered
  gate ids expected, and where the log will be.
* The battery is invoked ONCE. Its stdout and stderr are captured in that one
  execution; nothing is re-run to summarise, tail or collect.
* Afterwards each gate id must appear exactly once. Zero or twice fails.
* The log is closed and made read-only, and its SHA-256, size and the TRUE
  status are published -- on failure exactly as on success.

CLAIM CLASS: this file is the one-shot boundary; it makes no claim about
input at all.
"""
import argparse
import collections
import datetime
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys

GATE = re.compile(r"gate_id=(\w+)")

EXPECTED_GATES = ["provenance", "pristine", "domains", "mapping_pure",
                  "parser_negatives", "seam_fail_closed", "corpus",
                  "v2_chain", "matrix"]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(repo, *args):
    try:
        return subprocess.check_output(["git", "-C", repo] + list(args),
                                       text=True).strip()
    except (subprocess.CalledProcessError, OSError):
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nxinput-repo", required=True)
    ap.add_argument("--mmw-repo", required=True)
    ap.add_argument("--attempt-root", required=True)
    ap.add_argument("--number", required=True,
                    help="the attempt number, two digits")
    ap.add_argument("--runner", required=True)
    args = ap.parse_args()

    try:
        max_workers = int(os.environ.get("NXC5B_MAX_WORKERS", "2"))
    except ValueError:
        raise SystemExit("NXC5B_MAX_WORKERS must be an integer")
    if max_workers < 1 or max_workers > 2:
        raise SystemExit("C5B permits at most two build/test workers")
    niceness = os.getpriority(os.PRIO_PROCESS, 0)
    if niceness < 5:
        raise SystemExit("C5B must run at low priority (niceness >= 5)")

    nx_head = git(args.nxinput_repo, "rev-parse", "HEAD")
    nx_tree = git(args.nxinput_repo, "rev-parse", "HEAD^{tree}")
    mmw_head = git(args.mmw_repo, "rev-parse", "HEAD")
    mmw_tree = git(args.mmw_repo, "rev-parse", "HEAD^{tree}")
    if not nx_head or not mmw_head:
        raise SystemExit("both repositories must resolve a HEAD")
    nx_dirty = git(args.nxinput_repo, "status", "--porcelain")
    mmw_dirty = git(args.mmw_repo, "status", "--porcelain")
    if nx_dirty or mmw_dirty:
        raise SystemExit("the worktrees must be clean before an attempt:\n"
                         "nxinput:\n%s\nmmw:\n%s" % (nx_dirty, mmw_dirty))

    utc = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y%m%dT%H%M%SZ")
    name = "C5B-attempt-%s-%s-%s-%s" % (args.number, nx_head[:12],
                                        mmw_head[:12], utc)
    attempt = pathlib.Path(args.attempt_root) / name
    # Exclusive creation. If this attempt id already exists, it is a previous
    # attempt's immutable evidence and must not be touched.
    os.makedirs(attempt)
    log_path = attempt / "C5B-FINAL-BATTERY.log"
    log_fd = os.open(str(log_path), os.O_CREAT | os.O_EXCL | os.O_WRONLY,
                     0o644)

    here = pathlib.Path(args.runner).resolve().parent
    root = here.parent
    inputs = {}
    for relative in sorted(
            [p.relative_to(root) for p in (root / "tests").glob("*.py")] +
            [p.relative_to(root) for p in (root / "tests").glob("*.c")] +
            [p.relative_to(root) for p in (root / "tests").glob("*.sh")] +
            [p.relative_to(root) for p in (root / "src").glob("nxinput_godot*.c")] +
            [p.relative_to(root) for p in (root / "include").glob("nxinput_godot*.h")] +
            [p.relative_to(root) for p in
             (root / "tests" / "corpus").glob("c5b-*.gptk")] +
            [pathlib.Path("tests/corpus/guid-capabilities.json"),
             pathlib.Path("tests/godot-source-pins.json"),
             pathlib.Path("engine-patches/C5B-ENGINE-PROVENANCE.json"),
             pathlib.Path("engine-patches/godot3-nxc5b-seam.patch"),
             pathlib.Path("engine-patches/godot4-nxc5b-seam.patch")]):
        target = root / relative
        if target.is_file():
            inputs[str(relative)] = sha256(target)

    environment = {}
    for key in ("NXC5B_GODOT3", "NXC5B_GODOT4", "NXC5B_G3_SRC",
                "NXC5B_G4_SRC", "NXC5B_SDL2_SRC", "NXC5B_BUNDLE"):
        value = os.environ.get(key, "")
        environment[key] = {"path": value}
        if value and os.path.isfile(value):
            environment[key]["sha256"] = sha256(value)

    work = attempt / "work"
    command = ["sh", str(pathlib.Path(args.runner).resolve())]
    manifest = {
        "schema": "org.nextos.c5b.attempt",
        "schema_version": 1,
        "attempt_id": name,
        "attempt_number": args.number,
        "utc": utc,
        "nxinput": {"head": nx_head, "tree": nx_tree,
                    "worktree": os.path.abspath(args.nxinput_repo)},
        "mmw": {"head": mmw_head, "tree": mmw_tree,
                "worktree": os.path.abspath(args.mmw_repo)},
        "single_command": " ".join(command),
        "log": str(log_path),
        "work_directory": str(work),
        "expected_gate_ids_in_order": EXPECTED_GATES,
        "input_sha256": inputs,
        "environment": environment,
        "scheduling": {"niceness": niceness,
                       "max_build_test_workers": max_workers,
                       "runner_is_sequential": True},
        "rule": ("each gate_id runs exactly once in this one execution; "
                 "nothing may be re-run to collect output, and this log is "
                 "never truncated, renamed or reused"),
    }
    (attempt / "C5B-ATTEMPT-MANIFEST.json").write_text(
        json.dumps(manifest, indent=1, sort_keys=True) + "\n",
        encoding="utf-8")

    header = ("C5B ATTEMPT %s\nutc=%s\nnxinput=%s tree=%s\nmmw=%s tree=%s\n"
              "command=%s\n%s\n"
              % (name, utc, nx_head, nx_tree, mmw_head, mmw_tree,
                 " ".join(command), "-" * 72))
    os.write(log_fd, header.encode("utf-8"))

    env = dict(os.environ)
    env["NXC5B_WORK"] = str(work)
    run = subprocess.run(command, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, env=env)
    os.write(log_fd, run.stdout)

    text = run.stdout.decode("utf-8", "replace")
    counts = collections.Counter(GATE.findall(text))
    problems = []
    for gate in EXPECTED_GATES:
        if counts[gate] == 0:
            problems.append("gate_id=%s did not run" % gate)
        elif counts[gate] > 1:
            problems.append("gate_id=%s ran %d times" % (gate, counts[gate]))
    for gate in sorted(set(counts) - set(EXPECTED_GATES)):
        problems.append("gate_id=%s was not declared in the manifest" % gate)

    status = "PASS" if run.returncode == 0 and not problems else "FAIL"
    footer = ["", "-" * 72, "gate counters:"]
    for gate in EXPECTED_GATES:
        footer.append("  %-18s %d" % (gate, counts[gate]))
    for problem in problems:
        footer.append("PROBLEM %s" % problem)
    footer.append("battery exit code: %d" % run.returncode)
    footer.append("attempt status: %s" % status)
    os.write(log_fd, ("\n".join(footer) + "\n").encode("utf-8"))
    os.close(log_fd)
    os.chmod(log_path, 0o444)

    digest = sha256(log_path)
    result = {"attempt_id": name, "status": status,
              "battery_exit_code": run.returncode,
              "gate_counts": dict(counts), "problems": problems,
              "log": str(log_path), "log_sha256": digest,
              "log_bytes": os.path.getsize(log_path)}
    (attempt / "C5B-ATTEMPT-RESULT.json").write_text(
        json.dumps(result, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=1, sort_keys=True))
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
