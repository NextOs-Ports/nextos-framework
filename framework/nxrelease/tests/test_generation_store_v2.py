#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Directed gate for packaging the real nxbootstrap 0.8.4 generation v2.

The fixture is emitted by the production generator, not reconstructed by this
test.  NXBOOTSTRAP_076_ROOT exists only so the nxrelease version worktree can
run this gate against the integrated 0.8.4 framework commit; the
historical NXBOOTSTRAP_072_ROOT alias remains accepted.  In the integrated
tree the repository-local nxbootstrap is used automatically.
"""

import hashlib
import importlib.util
import json
import os
import shutil
import tempfile
from pathlib import Path


TESTS = Path(__file__).resolve().parent
NXRELEASE = TESTS.parent
REPOSITORY = NXRELEASE.parents[1]
RENDERER = NXRELEASE / "nx-render-manifest.py"
BOOTSTRAP_ROOT = Path(os.environ.get(
    "NXBOOTSTRAP_076_ROOT",
    os.environ.get(
        "NXBOOTSTRAP_072_ROOT",
        str(REPOSITORY / "framework" / "nxbootstrap"),
    ),
)).resolve()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    require(spec is not None and spec.loader is not None,
            "cannot load module %s" % path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_json(value, ensure_ascii):
    return json.dumps(
        value, indent=2, sort_keys=True, ensure_ascii=ensure_ascii
    ) + "\n"


def write_runtime_file(root, relative, payload, mode):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    path.chmod(mode)
    return path


def runtime_record(root, role, relative, mode):
    return {
        "role": role,
        "path": relative,
        "mode": mode,
        "sha256": sha256(root / relative),
    }


def build_real_fixture(root, generator):
    runtime = root / "runtime"
    runtime.mkdir()
    write_runtime_file(
        runtime, "fixture-nextos", b"#!/bin/sh\nexit 0\n", 0o755
    )
    write_runtime_file(
        runtime, "lib/libprivate.so", b"private fixture library\n", 0o644
    )
    write_runtime_file(
        runtime, "lib/runtime/System.Private.CoreLib.dll",
        b"managed runtime fixture\n", 0o644
    )
    write_runtime_file(
        runtime, "port-env.sh", b"export NX_FIXTURE=1\n", 0o644
    )
    manifest = {
        "schema_version": 3,
        "id": "generation-v2-fixture",
        "title": "Generation V2 Fixture",
        "launcher_name": "Generation V2 Fixture.sh",
        "architecture": "aarch64",
        "executable": "fixture-nextos",
        "argument_mode": "none",
        "home_mode": "preserve",
        "nxextract": {"mode": "no", "version": "1.3.0"},
        "required_files": [
            "fixture-nextos", "lib/libprivate.so", "port-env.sh",
        ],
        "private_library_paths": ["lib"],
        "prepare_script": "",
        "required_capabilities": [],
        "enabled_quirks": [],
        "runtime_report": "log",
        "generation_runtime": [
            runtime_record(
                runtime, "executable", "fixture-nextos", "0755"
            ),
            runtime_record(
                runtime, "private-library", "lib/libprivate.so", "0644"
            ),
            runtime_record(
                runtime, "runtime-data",
                "lib/runtime/System.Private.CoreLib.dll", "0644"
            ),
            runtime_record(
                runtime, "runtime-hook", "port-env.sh", "0644"
            ),
        ],
    }
    manifest_path = root / "nxport-input.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    output = root / "generated"
    generator.generate(manifest_path, output, False, runtime)
    stores = list((
        output / manifest["id"] / ".nxruntime" / "generations"
    ).iterdir())
    require(len(stores) == 1, "generator did not emit exactly one generation")
    return stores[0]


def expect_failure(renderer, fixture, cases, label, needle, mutate):
    case = cases / label / fixture.name
    case.parent.mkdir()
    shutil.copytree(fixture, case)
    mutate(case)
    try:
        renderer.generation_store_modes(case, BOOTSTRAP_ROOT.parent)
    except SystemExit as error:
        message = str(error)
        require(needle.lower() in message.lower(),
                "%s reported %r, expected %r" % (label, message, needle))
    else:
        raise AssertionError(label + " unexpectedly passed")


def main():
    version_path = BOOTSTRAP_ROOT / "VERSION"
    require(version_path.is_file() and not version_path.is_symlink(),
            "nxbootstrap root is missing VERSION: %s" % BOOTSTRAP_ROOT)
    require(version_path.read_text(encoding="utf-8").strip() == "0.8.4",
            "test requires the real nxbootstrap 0.8.4 tree")
    generator = load_module(
        BOOTSTRAP_ROOT / "tools" / "generate-port.py",
        "nxrelease_generation_v2_generator",
    )
    renderer = load_module(RENDERER, "nxrelease_generation_v2_renderer")
    require(generator.NXBOOTSTRAP_VERSION == "0.8.4",
            "loaded generator is not nxbootstrap 0.8.4")
    require(renderer.GENERATION_V2_BOOTSTRAP_VERSION == "0.8.4",
            "renderer generation-v2 pin drifted")

    with tempfile.TemporaryDirectory(
            prefix="nxrelease-generation-v2-") as temporary:
        root = Path(temporary)
        fixture = build_real_fixture(root, generator)
        modes = renderer.generation_store_modes(
            fixture, BOOTSTRAP_ROOT.parent
        )
        expected_modes = {
            "commit": "0644",
            "components.sha256": "0644",
            "components.v2": "0644",
            "format": "0644",
            "identity-runtime.v2": "0644",
            "identity.json": "0644",
            "manifest.json": "0644",
            "files/launcher/Generation V2 Fixture.sh": "0755",
            "files/nxport.json": "0644",
            "files/runtime/fixture-nextos": "0755",
            "files/runtime/lib/libprivate.so": "0644",
            "files/runtime/lib/runtime/System.Private.CoreLib.dll": "0644",
            "files/runtime/port-env.sh": "0644",
            "files/runtime/nxsplash-nextos": "0755",
        }
        require(modes == expected_modes,
                "real 0.8.4 generation modes/closure differ: %r" % modes)
        require(sha256(fixture / "identity.json") == fixture.name,
                "real fixture identity is not its directory name")

        cases = root / "negative"
        cases.mkdir()
        negatives = 0

        def negative(label, needle, mutate):
            nonlocal negatives
            expect_failure(
                renderer, fixture, cases, label, needle, mutate
            )
            negatives += 1

        def duplicate_manifest_member(case):
            path = case / "manifest.json"
            text = path.read_text(encoding="utf-8")
            path.write_text(
                text.replace(
                    "{\n", "{\n  \"schema\": \"duplicate\",\n", 1
                ),
                encoding="utf-8",
            )

        negative(
            "duplicate-manifest-member", "duplicate JSON member",
            duplicate_manifest_member,
        )

        def duplicate_identity_member(case):
            path = case / "identity.json"
            text = path.read_text(encoding="utf-8")
            path.write_text(
                text.replace(
                    "{\n", "{\n  \"schema\": \"duplicate\",\n", 1
                ),
                encoding="utf-8",
            )

        negative(
            "duplicate-identity-member", "duplicate JSON member",
            duplicate_identity_member,
        )

        def nonfinite_manifest_number(case):
            path = case / "manifest.json"
            text = path.read_text(encoding="utf-8")
            path.write_text(
                text.replace('"schema_version": 2', '"schema_version": NaN'),
                encoding="utf-8",
            )

        negative(
            "nonfinite-manifest-number", "non-finite JSON number",
            nonfinite_manifest_number,
        )
        negative(
            "missing-identity-json", "identity.json is missing",
            lambda case: (case / "identity.json").unlink(),
        )
        negative(
            "missing-runtime-identity", "identity-runtime.v2 is missing",
            lambda case: (case / "identity-runtime.v2").unlink(),
        )
        negative(
            "control-mode", "expected 0644",
            lambda case: (case / "identity.json").chmod(0o755),
        )

        def manifest_identity_disagreement(case):
            path = case / "manifest.json"
            value = json.loads(path.read_text(encoding="utf-8"))
            value["identity_basis"]["nxport_sha256"] = "0" * 64
            path.write_text(
                canonical_json(value, ensure_ascii=True), encoding="utf-8"
            )

        negative(
            "identity-disagreement", "identity.json disagree",
            manifest_identity_disagreement,
        )
        negative(
            "stale-runtime-records", "identity-runtime.v2 is stale",
            lambda case: (case / "identity-runtime.v2").write_text(
                (case / "identity-runtime.v2").read_text(encoding="utf-8") +
                "runtime-hook\t0644\t%s\textra.sh\n" % ("0" * 64),
                encoding="utf-8",
            ),
        )
        negative(
            "stale-components-v2", "components.v2 is stale",
            lambda case: (case / "components.v2").write_text(
                (case / "components.v2").read_text(encoding="utf-8") + "x",
                encoding="utf-8",
            ),
        )
        negative(
            "stale-components-sha", "components.sha256 is stale",
            lambda case: (case / "components.sha256").write_text(
                (case / "components.sha256").read_text(encoding="utf-8") +
                "%s  extra\n" % ("0" * 64),
                encoding="utf-8",
            ),
        )
        negative(
            "stale-commit", "commit marker is stale",
            lambda case: (case / "commit").write_text(
                "0" * 64 + "\n", encoding="ascii"
            ),
        )

        def swap_runtime_order(case):
            path = case / "manifest.json"
            value = json.loads(path.read_text(encoding="utf-8"))
            value["components"][2], value["components"][3] = (
                value["components"][3], value["components"][2]
            )
            path.write_text(
                canonical_json(value, ensure_ascii=True), encoding="utf-8"
            )

        negative(
            "runtime-order", "canonical order", swap_runtime_order,
        )

        def unsafe_runtime_path(case):
            path = case / "manifest.json"
            value = json.loads(path.read_text(encoding="utf-8"))
            value["components"][2]["path"] = "../escape"
            path.write_text(
                canonical_json(value, ensure_ascii=True), encoding="utf-8"
            )

        negative(
            "unsafe-runtime-path", "safe relative path", unsafe_runtime_path,
        )
        negative(
            "runtime-byte-drift", "component bytes differ",
            lambda case: (case / "files/runtime/fixture-nextos").write_bytes(
                b"#!/bin/sh\nexit 9\n"
            ),
        )
        negative(
            "runtime-mode-drift", "expected 0755",
            lambda case: (case / "files/runtime/fixture-nextos").chmod(0o644),
        )

        def nxport_runtime_drift(case):
            path = case / "files/nxport.json"
            value = json.loads(path.read_text(encoding="utf-8"))
            value["generation_runtime"][0]["sha256"] = "0" * 64
            path.write_text(
                canonical_json(value, ensure_ascii=False), encoding="utf-8"
            )

        negative(
            "nxport-runtime-drift", "component bytes differ",
            nxport_runtime_drift,
        )

        def runtime_symlink(case):
            path = case / "files/runtime/port-env.sh"
            path.unlink()
            path.symlink_to("/bin/true")

        negative(
            "runtime-symlink", "not a private regular file", runtime_symlink,
        )
        negative(
            "extra-store-file", "store is not closed",
            lambda case: (case / "unexpected").write_text(
                "extra\n", encoding="utf-8"
            ),
        )

        print(
            "nxrelease generation-v2 renderer: PASS "
            "generator=0.8.4 positive=1 negatives=%d files=%d" %
            (negatives, len(expected_modes))
        )


if __name__ == "__main__":
    main()
