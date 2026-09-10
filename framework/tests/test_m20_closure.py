#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free composition and fail-closed checkpoint gate for M20."""

import ast
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import sys


REPOSITORY = Path(__file__).resolve().parents[2]
FRAMEWORK = REPOSITORY / "framework"
CLOSURE = FRAMEWORK / "tests" / "m20-closure-v1.json"
MATRIX = FRAMEWORK / "tests" / "test-matrix-v1.json"
RUNNER = FRAMEWORK / "tests" / "run-safe-gates.sh"
CHECKPOINT = FRAMEWORK / "tests" / "capture-green-checkpoint.sh"
ITEM_IDS = tuple("M20-%03d" % index for index in range(1, 21))
EVIDENCE_RE = re.compile(r"^([^:]+):([1-9][0-9]*)$")
ALLOWED_EVIDENCE_ROOTS = (
    "framework/", "suportando_outros_devices/", "publicando_ports/",
)


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def read(path):
    require(path.is_file() and not path.is_symlink(),
            "missing or unsafe file: %s" % path.relative_to(REPOSITORY))
    return path.read_text(encoding="utf-8")


def evidence(reference):
    require(isinstance(reference, str), "evidence reference is not a string")
    matched = EVIDENCE_RE.fullmatch(reference)
    require(matched is not None, "evidence lacks a line: %s" % reference)
    relative, line_text = matched.groups()
    require(relative.startswith(ALLOWED_EVIDENCE_ROOTS),
            "M20 evidence escaped the test scope: %s" % relative)
    logical = PurePosixPath(relative)
    require(not logical.is_absolute() and ".." not in logical.parts,
            "unsafe evidence path: %s" % relative)
    source = read(REPOSITORY.joinpath(*logical.parts))
    require(int(line_text) <= len(source.splitlines()),
            "evidence line is outside the file: %s" % reference)


def logical_shell_lines(source):
    pending = ""
    for raw_line in source.splitlines():
        stripped = raw_line.strip()
        if not pending and (not stripped or stripped.startswith("#")):
            continue
        if raw_line.rstrip().endswith("\\"):
            pending += raw_line.rstrip()[:-1] + " "
            continue
        yield pending + raw_line
        pending = ""
    require(not pending, "runner has an incomplete continuation")


def runner_gates(source):
    result = []
    for line in logical_shell_lines(source):
        stripped = line.strip()
        if not stripped.startswith("run_gate "):
            continue
        words = shlex.split(stripped, comments=True, posix=True)
        require(len(words) >= 3, "runner gate invocation is incomplete")
        result.append((words[1], words[2:]))
    return result


def require_tokens(path, tokens):
    source = read(path)
    for token in tokens:
        require(token in source,
                "%s lacks M20 evidence token: %s" %
                (path.relative_to(REPOSITORY), token))


def main():
    document = json.loads(read(CLOSURE))
    require(set(document) == {
        "schema", "schema_version", "milestone", "status", "scope",
        "method", "safety", "items",
    }, "M20 closure schema changed")
    require(document["schema"] == "nxframework-m20-closure-v1" and
            document["schema_version"] == 1 and
            document["milestone"] == "M20" and
            document["status"] == "closed_for_host_validation_infrastructure",
            "M20 closure identity changed")
    require(document["safety"] == {
        "game_execution": False,
        "external_guest_code_execution": False,
        "device_access": False,
        "network_access": False,
        "hardware_ran": False,
        "physical_advance_authorized": False,
        "real_fat_mount_used": False,
    }, "M20 safety claims changed")
    items = document["items"]
    require(isinstance(items, list) and len(items) == 20,
            "M20 must account for exactly twenty items")
    require(tuple(item.get("id") for item in items) == ITEM_IDS,
            "M20 item IDs or ordering changed")
    for item in items:
        require(set(item) == {
            "id", "status", "evidence_refs", "guarantees",
            "limitations", "tests",
        }, "%s has malformed fields" % item.get("id"))
        require(item["status"] == "closed",
                "%s is not closed" % item["id"])
        for field in ("evidence_refs", "guarantees", "limitations", "tests"):
            require(isinstance(item[field], list) and item[field],
                    "%s lacks %s" % (item["id"], field))
        for reference in item["evidence_refs"]:
            evidence(reference)

    ast.parse(read(Path(__file__)), filename=str(Path(__file__)))
    matrix = json.loads(read(MATRIX))
    gates = matrix["gates"]
    by_id = {gate["id"]: gate for gate in gates}
    required = {
        "test-infrastructure", "tooling-filesystem", "bootstrap-static-safety",
        "bootstrap-isolated", "nxcompat-host", "nxgl-m13-host",
        "nxinput-m15-contract", "nxloader-host", "nxloader-armv7-cross",
        "nxloader-aarch64-cross", "nxextract-python",
        "nxextract-runtime-env", "nxextract-release", "nxabi-m17-gate",
        "nxrelease", "nxgenerator-host", "nxgenerator-framework-pin",
        "nxgenerator-m19-closure",
        "m20-closure", "diff-check", "m20-green-checkpoint",
    }
    require(required <= set(by_id), "M20 required gate set is incomplete")
    for gate in gates:
        if gate["class"] == "hardware":
            require(gate["automatic"] is False and gate["command"] is None,
                    "hardware gate became automatic: %s" % gate["id"])

    runner_source = read(RUNNER)
    require("set -euo pipefail" in runner_source,
            "safe runner no longer stops on red")
    ordered = runner_gates(runner_source)
    ordered_ids = [gate_id for gate_id, _ in ordered]
    require(ordered_ids[0] == "test-infrastructure",
            "infrastructure is not the first gate")
    require(ordered_ids[-1] == "m20-green-checkpoint",
            "green checkpoint is not the final gate")
    require(ordered_ids.index("nxgenerator-host") <
            ordered_ids.index("nxgenerator-framework-pin") <
            ordered_ids.index("nxgenerator-m19-closure") <
            ordered_ids.index("m20-closure") <
            ordered_ids.index("diff-check") <
            ordered_ids.index("m20-green-checkpoint"),
            "M19/M20/diff/checkpoint order is unsafe")
    expected_pin_command = [
        "python3", "-B",
        "framework/nxgenerator/tests/test_framework_pin.py",
    ]
    require(by_id["nxgenerator-framework-pin"]["class"] == "filesystem" and
            by_id["nxgenerator-framework-pin"]["automatic"] is True and
            by_id["nxgenerator-framework-pin"]["command"] ==
            expected_pin_command,
            "framework source pin matrix contract changed")
    expected_checkpoint_command = [
        "bash", "framework/tests/capture-green-checkpoint.sh",
        "--checkpoint-root", "$LOG_ROOT/checkpoints",
    ]
    require(by_id["m20-green-checkpoint"]["class"] == "filesystem" and
            by_id["m20-green-checkpoint"]["automatic"] is True and
            by_id["m20-green-checkpoint"]["command"] ==
            expected_checkpoint_command,
            "green checkpoint matrix contract changed")

    checkpoint_source = read(CHECKPOINT)
    require("set -euo pipefail" in checkpoint_source and
            'exec "$CAPTURE"' in checkpoint_source and
            "-- framework suportando_outros_devices publicando_ports" in
            checkpoint_source,
            "green checkpoint wrapper is incomplete")
    require("rm -" not in checkpoint_source,
            "green checkpoint wrapper gained deletion authority")

    require_tokens(FRAMEWORK / "tests" / "test_infrastructure.py", (
        "bash", "ast.parse", "trailing whitespace", "check_safe_runner",
    ))
    require_tokens(FRAMEWORK / "nxloader" / "tests" / "run-host.sh", (
        "NXLOADER_ENABLE_SANITIZERS=ON", "--write-fuzz-corpus", "-runs=20000",
    ))
    require_tokens(FRAMEWORK / "nxcompat" / "tests" / "run-host.sh", (
        "NXINPUT_BUILD_NATIVE_TESTS=OFF", "nxinput-static-gate",
        "hardware_ran=0", "device_access=0",
    ))
    require_tokens(FRAMEWORK / "nxgl" / "tests" / "run-m13-host.sh", (
        "NXGL_BUILD_NATIVE_TESTS=OFF", "real_gpu_or_display_opened=0",
        "hardware_ran=0", "device_access=0",
    ))
    require_tokens(FRAMEWORK / "nxbootstrap" / "tests" /
                   "test-nxbootstrap.sh", (
        "pm_finish did not run exactly once",
        "cleanup could execute PortMaster finish more than once",
        "an intermediate symlink escaped",
        "runtime chmod followed or accepted a hardlinked executable",
        "two copies of the same port received different instance locks",
    ))
    require_tokens(REPOSITORY / "suportando_outros_devices" /
                   "extrator-universal" / "tests" /
                   "test_runtime_env.sh", (
        "FAT/exFAT", "chmod 0644", "bash",
    ))
    require_tokens(FRAMEWORK / "nxrelease" / "tests" /
                   "test_nxrelease.sh", (
        "deterministic archives differ", "TOCTOU mutation was not rejected",
        "race unexpectedly published", "bundle ZIP is not deterministic",
    ))
    require_tokens(FRAMEWORK / "nxgenerator" / "framework_pin.py", (
        "GIT_NO_REPLACE_OBJECTS", "GIT_NO_LAZY_FETCH", "TREE_DIGEST",
        "verified_git_object", "rename_noreplace", "FRAMEWORK-SOURCE.json",
    ))
    require_tokens(FRAMEWORK / "nxgenerator" / "tests" /
                   "test_framework_pin.py", (
        "dirty_checkout", "object_integrity=1", "replace_refs=blocked",
        "filters=not_executed", "no_overwrite=1",
    ))
    require_tokens(FRAMEWORK / "tools" / "run-logged.sh", (
        "command_argv_sha256", "result.txt", "MANIFEST.sha256",
    ))
    require_tokens(FRAMEWORK / "tools" / "capture-checkpoint.sh", (
        "source-snapshot.tar.gz", "source-files.sha256", "MANIFEST.sha256",
    ))

    print("M20 closure gate passed: items=20 status=closed "
          "hardware_ran=0 device_access=0 physical_advance_authorized=0")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError, ValueError, KeyError, SyntaxError) as error:
        print("M20 closure gate failed: %s" % error, file=sys.stderr)
        raise SystemExit(1)
