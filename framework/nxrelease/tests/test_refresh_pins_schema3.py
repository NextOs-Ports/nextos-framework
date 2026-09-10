#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Directed regression gate for schema-3 post-build pin refresh."""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


TESTS = Path(__file__).resolve().parent
NXRELEASE = TESTS.parent
FRAMEWORK = NXRELEASE.parent
REFRESH = NXRELEASE / "nx-refresh-pins.py"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(
        json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )


def run_refresh(port, check=False, expected=0):
    command = [
        sys.executable,
        "-B",
        REFRESH,
        "--port-dir",
        port,
        "--framework-root",
        FRAMEWORK,
    ]
    if check:
        command.append("--check")
    result = subprocess.run(
        [str(part) for part in command],
        text=True,
        capture_output=True,
        check=False,
    )
    require(
        result.returncode == expected,
        "refresh returned %d, expected %d:\n%s\nstdout:\n%s\nstderr:\n%s"
        % (result.returncode, expected, " ".join(map(str, command)),
           result.stdout, result.stderr),
    )
    return result


def schema3_project(runtime_path="fixture-nextos", mode="0755",
                    digest=None, runtime_root="runtime"):
    return {
        "schema_version": 3,
        "runtime_root": runtime_root,
        "nxport": {
            "schema_version": 3,
            "architecture": "aarch64",
            "generation_runtime": [
                {
                    "role": "executable",
                    "path": runtime_path,
                    "mode": mode,
                    "sha256": digest if digest is not None else "0" * 64,
                },
            ],
        },
    }


def make_port(root, project=None):
    port = root / "port"
    runtime = port / "runtime"
    runtime.mkdir(parents=True)
    write_json(port / "nxrelease.json", {})
    if project is not None:
        write_json(port / "nxproject.json", project)
    launcher = port / "Fixture.sh"
    launcher.write_bytes(b"sealed launcher\n")
    launcher.chmod(0o755)
    return port, runtime, launcher


def assert_post_build_refresh(root):
    port, runtime, launcher = make_port(root, schema3_project())
    # Schema 3 renders nxrelease.json only inside the fresh candidate.  The
    # source tree must not need an old release manifest to seal final bytes.
    (port / "nxrelease.json").unlink()
    executable = runtime / "fixture-nextos"
    executable.write_bytes(b"final build bytes v1\n")
    executable.chmod(0o755)
    installation = port / "INSTALLATION.md"
    installation.write_bytes(b"[PT-BR]\nContrato autoral.\n[EN]\nAuthored contract.\n")
    installation.chmod(0o644)
    project = json.loads(
        (port / "nxproject.json").read_text(encoding="utf-8")
    )
    project["package_payload"] = [{
        "path": "INSTALLATION.md",
        "mode": "0644",
        "sha256": "0" * 64,
        "kind": "payload",
    }]
    write_json(port / "nxproject.json", project)

    project_path = port / "nxproject.json"
    project_before = project_path.read_bytes()
    launcher_before = (launcher.read_bytes(), launcher.stat().st_mode & 0o777)

    stale = run_refresh(port, check=True, expected=1)
    require("pins are stale" in stale.stdout,
            "--check did not report stale schema-3 runtime pins")
    require(project_path.read_bytes() == project_before,
            "--check rewrote nxproject.json")

    run_refresh(port)
    project = json.loads(project_path.read_text(encoding="utf-8"))
    member = project["nxport"]["generation_runtime"][0]
    require(member["sha256"] == sha256(executable),
            "post-build executable SHA-256 was not refreshed")
    require(project["package_payload"][0]["sha256"] == sha256(installation),
            "authored package payload SHA-256 was not refreshed")
    require(member["mode"] == "0755",
            "post-build executable mode pin was not preserved")
    require((launcher.read_bytes(), launcher.stat().st_mode & 0o777) ==
            launcher_before,
            "schema-3 refresh changed an already sealed launcher")
    run_refresh(port, check=True)

    installation.write_bytes(
        b"[PT-BR]\nContrato autoral atualizado.\n[EN]\nUpdated authored contract.\n"
    )
    payload_changed = run_refresh(port, check=True, expected=1)
    require("package payload INSTALLATION.md" in payload_changed.stdout,
            "--check did not identify stale package_payload")
    run_refresh(port)
    project = json.loads(project_path.read_text(encoding="utf-8"))
    require(project["package_payload"][0]["sha256"] == sha256(installation),
            "updated package_payload was not re-pinned")

    executable.write_bytes(b"final build bytes v2\n")
    changed = run_refresh(port, check=True, expected=1)
    require("generation runtime fixture-nextos" in changed.stdout,
            "--check did not identify the rebuilt runtime member")
    run_refresh(port)
    project = json.loads(project_path.read_text(encoding="utf-8"))
    require(project["nxport"]["generation_runtime"][0]["sha256"] ==
            sha256(executable),
            "second final build was not re-pinned")

    executable.chmod(0o644)
    mismatch = run_refresh(port, check=True, expected=1)
    require("source mode is 0644, expected 0755" in mismatch.stderr,
            "post-build mode divergence was not rejected")


def assert_unsafe_and_divergent_closure_rejected(root):
    lexical, _, _ = make_port(root / "lexical", schema3_project("../escape"))
    rejected = run_refresh(lexical, expected=1)
    require("is not a safe relative path" in rejected.stderr,
            "lexically unsafe generation_runtime path was accepted")

    missing, _, _ = make_port(root / "missing", schema3_project("missing"))
    rejected = run_refresh(missing, expected=1)
    require("source is missing" in rejected.stderr,
            "declared member missing from the source closure was accepted")

    duplicate_project = schema3_project()
    duplicate_project["nxport"]["generation_runtime"].append(
        dict(duplicate_project["nxport"]["generation_runtime"][0])
    )
    duplicate, runtime, _ = make_port(root / "duplicate", duplicate_project)
    payload = runtime / "fixture-nextos"
    payload.write_bytes(b"payload\n")
    payload.chmod(0o755)
    rejected = run_refresh(duplicate, expected=1)
    require("duplicate paths" in rejected.stderr,
            "duplicate runtime closure member was accepted")

    link_port, link_runtime, _ = make_port(
        root / "symlink-member", schema3_project("linked-nextos")
    )
    outside = root / "outside-nextos"
    outside.write_bytes(b"outside\n")
    outside.chmod(0o755)
    os.symlink(outside, link_runtime / "linked-nextos")
    rejected = run_refresh(link_port, expected=1)
    require("source is not" in rejected.stderr and
            "regular file" in rejected.stderr,
            "symlink runtime member was accepted")

    parent_link_port, parent_link_runtime, _ = make_port(
        root / "symlink-member-parent",
        schema3_project("nested/payload"),
    )
    parent_outside = root / "member-outside"
    parent_outside.mkdir()
    parent_payload = parent_outside / "payload"
    parent_payload.write_bytes(b"outside through parent\n")
    parent_payload.chmod(0o755)
    os.symlink(
        parent_outside,
        parent_link_runtime / "nested",
        target_is_directory=True,
    )
    rejected = run_refresh(parent_link_port, expected=1)
    require("source path contains a non-directory or symlink" in rejected.stderr,
            "intermediate symlink in runtime member path was accepted")

    root_link_port, root_runtime, _ = make_port(
        root / "symlink-root", schema3_project()
    )
    root_runtime.rmdir()
    os.symlink(root, root_runtime, target_is_directory=True)
    rejected = run_refresh(root_link_port, expected=1)
    require("runtime_root contains a non-directory or symlink" in rejected.stderr,
            "symlink runtime_root was accepted")

    root_parent_port, root_parent_runtime, _ = make_port(
        root / "symlink-root-parent", project=None
    )
    root_parent_runtime.rmdir()
    runtime_outside = root / "runtime-root-outside" / "runtime"
    runtime_outside.mkdir(parents=True)
    runtime_payload = runtime_outside / "fixture-nextos"
    runtime_payload.write_bytes(b"outside runtime root\n")
    runtime_payload.chmod(0o755)
    os.symlink(
        runtime_outside.parent,
        root_parent_port / "linked",
        target_is_directory=True,
    )
    write_json(
        root_parent_port / "nxproject.json",
        schema3_project(runtime_root="linked/runtime"),
    )
    rejected = run_refresh(root_parent_port, expected=1)
    require("runtime_root contains a non-directory or symlink" in rejected.stderr,
            "intermediate symlink in runtime_root was accepted")

    project_link_port, _, _ = make_port(root / "symlink-project", project=None)
    project_outside = root / "outside-project.json"
    write_json(project_outside, schema3_project())
    os.symlink(project_outside, project_link_port / "nxproject.json")
    rejected = run_refresh(project_link_port, expected=1)
    require("nxproject.json cannot be a symlink" in rejected.stderr,
            "symlink nxproject.json was silently treated as legacy")

    payload_link_project = schema3_project()
    payload_link_project["package_payload"] = [{
        "path": "NOTICE.md", "mode": "0644", "sha256": "0" * 64,
        "kind": "license-notice",
    }]
    payload_link_port, payload_link_runtime, _ = make_port(
        root / "symlink-package-payload", payload_link_project
    )
    payload_runtime = payload_link_runtime / "fixture-nextos"
    payload_runtime.write_bytes(b"runtime\n")
    payload_runtime.chmod(0o755)
    notice_outside = root / "notice-outside"
    notice_outside.write_bytes(b"notice\n")
    notice_outside.chmod(0o644)
    os.symlink(notice_outside, payload_link_port / "NOTICE.md")
    rejected = run_refresh(payload_link_port, expected=1)
    require("source is not a private regular file" in rejected.stderr,
            "symlink package_payload member was accepted")

    payload_hard_project = schema3_project()
    payload_hard_project["package_payload"] = [{
        "path": "NOTICE.md", "mode": "0644", "sha256": "0" * 64,
        "kind": "license-notice",
    }]
    payload_hard_port, payload_hard_runtime, _ = make_port(
        root / "hardlink-package-payload", payload_hard_project
    )
    payload_runtime = payload_hard_runtime / "fixture-nextos"
    payload_runtime.write_bytes(b"runtime\n")
    payload_runtime.chmod(0o755)
    hard_source = root / "notice-hard-source"
    hard_source.write_bytes(b"notice\n")
    hard_source.chmod(0o644)
    os.link(hard_source, payload_hard_port / "NOTICE.md")
    rejected = run_refresh(payload_hard_port, expected=1)
    require("source is not a private regular file" in rejected.stderr,
            "hardlink package_payload member was accepted")


def assert_legacy_smoke(root):
    port, _, launcher = make_port(root, project=None)
    before = (launcher.read_bytes(), launcher.stat().st_mode & 0o777)
    result = run_refresh(port, check=True)
    require("every pin already matches" in result.stdout,
            "legacy no-op manifest did not converge")
    require((launcher.read_bytes(), launcher.stat().st_mode & 0o777) == before,
            "legacy no-op refresh changed the launcher")


def main():
    with tempfile.TemporaryDirectory(prefix="nx-refresh-schema3-") as temp:
        root = Path(temp)
        assert_post_build_refresh(root / "refresh")
        assert_unsafe_and_divergent_closure_rejected(root / "negative")
        assert_legacy_smoke(root / "legacy")
    print("nx-refresh-pins schema-3 gate passed: positive=2 negatives=10 legacy=1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
