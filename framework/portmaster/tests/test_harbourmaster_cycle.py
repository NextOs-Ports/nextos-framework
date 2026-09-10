#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Hermetic regression tests for the pinned real HarbourMaster cycle."""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOL = REPO_ROOT / "framework/portmaster/tools/harbourmaster-cycle.py"
MIXED_EXECUTION_ROLES = {
    "extractor": {
        "architecture": "aarch64", "closure": "host",
        "executable": "nxextract/nxextract-ui", "executor": "native",
        "interpreter": "/lib/ld-linux-aarch64.so.1",
    },
    "game": {
        "architecture": "armv7", "closure": "firmware-and-port",
        "executable": "cycle-nextos", "executor": "native-or-loader",
        "interpreter": "/lib/ld-linux-armhf.so.3",
    },
    "helpers": [],
    "splash": {
        "architecture": "armv7", "closure": "firmware",
        "executable": "nxsplash-nextos", "executor": "native-or-loader",
        "interpreter": "/lib/ld-linux-armhf.so.3",
    },
}


def metadata(runtime=None):
    return {
        "attr": {
            "arch": ["aarch64"],
            "min_glibc": "2.17",
            "runtime": runtime,
            "title": "Cycle Fixture",
        },
        "items": ["Cycle Fixture.sh", "cycle-fixture/"],
        "items_opt": [],
        "name": "cycle-fixture.zip",
        "version": 1,
    }


def zip_info(name, executable=False):
    info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = ((0o100755 if executable else 0o100644) << 16)
    return info


def build_archive(path, port_metadata=None, raw_metadata=None, extra_members=None):
    if raw_metadata is None:
        raw_metadata = json.dumps(
            port_metadata if port_metadata is not None else metadata(),
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        ) + "\n"
    members = {
        "Cycle Fixture.sh": (
            "#!/bin/bash\n# PORTMASTER: cycle-fixture, Cycle Fixture.sh\nexit 0\n"
        ).encode("utf-8"),
        "cycle-fixture/INSTALLATION.md": b"# Installation / Instalacao\n",
        "cycle-fixture/port.json": raw_metadata.encode("utf-8"),
        "cycle-fixture/version.txt": b"1\n",
    }
    members.update(extra_members or {})
    with zipfile.ZipFile(str(path), "w") as package:
        for name in sorted(members):
            executable = (
                name.endswith(".sh") or
                name.endswith("/files/runtime/cycle-nextos") or
                name.endswith("/files/runtime/nxsplash-nextos")
            )
            package.writestr(zip_info(name, executable), members[name])


def sha256(payload):
    return hashlib.sha256(payload).hexdigest()


def generation_members(seed, schema_version=2, legacy_32=False,
                       execution_roles=None, duplicate_launcher_id=False,
                       launcher_suffix=b""):
    """Return a genuinely complete nxbootstrap generation fixture and its id."""
    marker = seed.encode("ascii")
    nxport_document = {
        "architecture": "aarch64",
        "argument_mode": "none",
        "enabled_quirks": [],
        "executable": "cycle-nextos",
        "home_mode": "port",
        "id": "cycle-fixture",
        "launcher_name": "Cycle Fixture.sh",
        "nxextract": {"mode": "no"},
        "prepare_script": "",
        "private_library_paths": [],
        "required_capabilities": [],
        "required_files": ["cycle-nextos", "nxsplash-nextos"],
        "runtime_report": "log",
        "schema_version": 2,
        "seed": seed,
        "title": "Cycle Fixture",
    }
    if execution_roles is not None:
        nxport_document["execution_roles"] = execution_roles
    nxport = (
        json.dumps(nxport_document, indent=2, sort_keys=True,
                   ensure_ascii=False) + "\n"
    ).encode("utf-8")
    splash = b"fixture nxsplash " + marker + b"\n"
    nxbootstrap = {
        "generator_sha256": sha256(b"generator " + marker),
        "launcher_template_sha256": sha256(b"template " + marker),
        "version": "0.7.0",
    }
    nxsplash = {
        "architecture": "aarch64",
        "sha256": sha256(splash),
        "version": "0.1.2",
    }
    if schema_version == 1:
        if legacy_32:
            identity = "nxport-canonical-sha256"
            generation_id = sha256(nxport)[:32]
        else:
            identity = {
                "nxbootstrap": nxbootstrap,
                "nxport_sha256": sha256(nxport),
                "nxsplash": nxsplash,
                "schema": "org.nextos.nxruntime.generation-identity",
                "schema_version": 1,
            }
            identity_bytes = json.dumps(
                identity, sort_keys=True, separators=(",", ":"),
                ensure_ascii=False,
            ).encode("utf-8")
            generation_id = sha256(identity_bytes)
        launcher = (
            b"#!/bin/bash\nGENERATION_ID=" + generation_id.encode("ascii") +
            b"\nexit 0\n" + launcher_suffix
        )
        component_payloads = {
            "launcher/Cycle Fixture.sh": (launcher, "0755"),
            "nxport.json": (nxport, "0644"),
        }
        components = {
            path: {"mode": mode, "sha256": sha256(payload)}
            for path, (payload, mode) in component_payloads.items()
        }
        runtime_controls = {}
    else:
        launcher_preimage = (
            b"#!/bin/bash\nGENERATION_ID=" + (b"0" * 64) + b"\n" +
            ((b"SECOND_GENERATION_ID=" + (b"0" * 64) + b"\n")
             if duplicate_launcher_id else b"") +
            b"exit 0\n" + launcher_suffix
        )
        executable = b"fixture executable " + marker + b"\n"
        runtime_data = b"fixture managed runtime data " + marker + b"\n"
        runtime_components = [
            {
                "mode": "0755", "path": "cycle-nextos",
                "role": "executable", "sha256": sha256(executable),
            },
            {
                "mode": "0644", "path": "lib/runtime/System.Private.CoreLib.dll",
                "role": "runtime-data", "sha256": sha256(runtime_data),
            },
            {
                "mode": "0755", "path": "nxsplash-nextos",
                "role": "nxsplash", "sha256": sha256(splash),
            },
        ]
        runtime_records = "".join(
            "{role}\t{mode}\t{sha256}\t{path}\n".format(**record)
            for record in runtime_components
        ).encode("utf-8")
        identity_components = [
            {
                "mode": "0755", "path": "Cycle Fixture.sh",
                "role": "launcher", "sha256": sha256(launcher_preimage),
            },
            {
                "mode": "0644", "path": "nxport.json",
                "role": "nxport", "sha256": sha256(nxport),
            },
        ] + runtime_components
        identity = {
            "components": identity_components,
            "launcher_preimage_sha256": sha256(launcher_preimage),
            "nxbootstrap": nxbootstrap,
            "nxport_sha256": sha256(nxport),
            "nxsplash": nxsplash,
            "runtime_records_sha256": sha256(runtime_records),
            "schema": "org.nextos.nxruntime.generation-identity",
            "schema_version": 2,
        }
        identity_bytes = (
            json.dumps(identity, indent=2, sort_keys=True, ensure_ascii=False) +
            "\n"
        ).encode("utf-8")
        generation_id = sha256(identity_bytes)
        launcher = launcher_preimage.replace(
            b"0" * 64, generation_id.encode("ascii")
        )
        component_payloads = {
            "launcher/Cycle Fixture.sh": (launcher, "0755"),
            "nxport.json": (nxport, "0644"),
            "runtime/cycle-nextos": (executable, "0755"),
            "runtime/lib/runtime/System.Private.CoreLib.dll": (
                runtime_data, "0644"),
            "runtime/nxsplash-nextos": (splash, "0755"),
        }
        components = [
            {
                "mode": "0755", "path": "Cycle Fixture.sh",
                "role": "launcher", "sha256": sha256(launcher),
            },
            {
                "mode": "0644", "path": "nxport.json",
                "role": "nxport", "sha256": sha256(nxport),
            },
        ] + runtime_components
        components_v2 = "".join(
            "{role}\t{mode}\t{sha256}\t{path}\n".format(**record)
            for record in components
        ).encode("utf-8")
        runtime_controls = {
            "components.v2": components_v2,
            "format": b"nxruntime-generation-v2\n",
            "identity-runtime.v2": runtime_records,
            "identity.json": identity_bytes,
        }

    root = "cycle-fixture/.nxruntime/generations/%s/" % generation_id
    manifest = (
        json.dumps({
            "components": components,
            "generation_id": generation_id,
            "identity_basis": identity,
            "schema": "nxruntime-generation-v%d" % schema_version,
            "schema_version": schema_version,
        }, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    checksums = "".join(
        "%s  %s\n" % (sha256(payload), path)
        for path, (payload, _mode) in sorted(component_payloads.items())
    ).encode("utf-8")
    store = {
        "Cycle Fixture.sh": launcher,
        "cycle-fixture/a-b.txt": b"hyphen inventory anchor\n",
        "cycle-fixture/a/b.txt": b"directory inventory anchor\n",
        "cycle-fixture/nxport.json": nxport,
        root + "commit": (generation_id + "\n").encode("ascii"),
        root + "components.sha256": checksums,
        root + "manifest.json": manifest,
    }
    store.update({root + name: payload for name, payload in runtime_controls.items()})
    for relative, (payload, _mode) in component_payloads.items():
        store[root + "files/" + relative] = payload

    component_modes = {
        root + "files/" + relative: mode
        for relative, (_payload, mode) in component_payloads.items()
    }
    component_modes["Cycle Fixture.sh"] = "0755"
    generator_version = (
        "0.2.13" if legacy_32 else
        ("0.2.15" if schema_version == 1 else "0.2.19")
    )
    if tuple(int(part) for part in generator_version.split(".")) < (0, 2, 19):
        inventory = sorted(store.items(), key=lambda item: Path(item[0]).parts)
    else:
        inventory = sorted(store.items())
    artifacts = []
    for logical, payload in inventory:
        artifacts.append({
            "mode": component_modes.get(logical, "0644"),
            "path": logical,
            "sha256": sha256(payload),
        })
    receipt = {
        "artifacts": artifacts,
        "claims": {},
        "generation_id": generation_id,
        "generator": {"name": "nxgenerator", "version": generator_version},
        "project_manifest_sha256": "0" * 64,
        "schema": "nxgenerator-receipt-v1",
        "schema_version": 1,
        "source_pins": {},
    }
    if execution_roles is not None:
        receipt["execution_roles"] = execution_roles
    store["cycle-fixture/GENERATION.json"] = (
        json.dumps(receipt, indent=2, sort_keys=True).encode("utf-8") + b"\n"
    )
    return store, generation_id


def refresh_receipt_artifact(members, logical):
    receipt_path = "cycle-fixture/GENERATION.json"
    receipt = json.loads(members[receipt_path].decode("utf-8"))
    for artifact in receipt["artifacts"]:
        if artifact["path"] == logical:
            artifact["sha256"] = sha256(members[logical])
            break
    else:
        raise AssertionError("fixture artifact is absent: %s" % logical)
    members[receipt_path] = (
        json.dumps(receipt, indent=2, sort_keys=True).encode("utf-8") + b"\n"
    )


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class HarbourMasterCycleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hm-cycle-tests.")
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def run_cycle(self, archive, previous=None):
        command = [sys.executable, "-B", str(TOOL), str(archive)]
        if previous is not None:
            command.extend(["--previous", str(previous)])
        environment = os.environ.copy()
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        return subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
            check=False,
        )

    def assert_rejected(self, archive, token, previous=None):
        result = self.run_cycle(archive, previous)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(token, result.stderr)

    def test_install_discovery_uninstall_reinstall_is_offline_and_immutable(self):
        archive = self.root / "candidate.zip"
        build_archive(archive)
        before = digest(archive)
        result = self.run_cycle(archive)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("install=1 update=0 uninstall=1 reinstall=1", result.stdout)
        self.assertIn("guest_execution=0 network=0", result.stdout)
        self.assertEqual(digest(archive), before)

    def test_clean_update_cycle_passes(self):
        previous = self.root / "previous.zip"
        candidate = self.root / "candidate.zip"
        build_archive(previous, extra_members={"cycle-fixture/revision.txt": b"old\n"})
        build_archive(candidate, extra_members={"cycle-fixture/revision.txt": b"new\n"})
        result = self.run_cycle(candidate, previous)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("update=1", result.stdout)

    def test_generation_update_preserves_authenticated_rollback_root(self):
        previous = self.root / "previous-generation.zip"
        candidate = self.root / "candidate-generation.zip"
        previous_members, _previous_id = generation_members("previous-v2")
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        result = self.run_cycle(candidate, previous)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("update=1 uninstall=1 reinstall=1", result.stdout)
        self.assertIn("retained_generations=1", result.stdout)

    def test_generation_store_first_adoption_from_plain_zip_passes(self):
        previous = self.root / "previous-plain.zip"
        candidate = self.root / "candidate-first-generation.zip"
        candidate_members, _candidate_id = generation_members("first-adoption")
        build_archive(previous)
        build_archive(candidate, extra_members=candidate_members)
        result = self.run_cycle(candidate, previous)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("retained_generations=0", result.stdout)

    def test_generation_store_clean_install_passes(self):
        candidate = self.root / "candidate-generation.zip"
        members, _generation_id = generation_members("candidate-clean-install")
        build_archive(candidate, extra_members=members)
        result = self.run_cycle(candidate)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("install=1 update=0 uninstall=1 reinstall=1", result.stdout)

    def test_candidate_generation_receipt_without_store_is_rejected(self):
        candidate = self.root / "candidate-receipt-only.zip"
        members, generation_id = generation_members("candidate-receipt-only")
        store_prefix = (
            "cycle-fixture/.nxruntime/generations/%s/" % generation_id
        )
        members = {
            path: payload for path, payload in members.items()
            if not path.startswith(store_prefix)
        }
        build_archive(candidate, extra_members=members)
        self.assert_rejected(
            candidate,
            "candidate ZIP has a generation store/receipt split",
        )

    def test_previous_generation_receipt_without_store_is_not_first_adoption(self):
        previous = self.root / "previous-receipt-only.zip"
        candidate = self.root / "candidate-generation.zip"
        previous_members, previous_id = generation_members(
            "previous-receipt-only"
        )
        previous_prefix = (
            "cycle-fixture/.nxruntime/generations/%s/" % previous_id
        )
        previous_members = {
            path: payload for path, payload in previous_members.items()
            if not path.startswith(previous_prefix)
        }
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "previous ZIP has a generation store/receipt split",
            previous=previous,
        )

    def test_candidate_generation_store_without_receipt_is_rejected(self):
        candidate = self.root / "candidate-store-only.zip"
        members, _generation_id = generation_members("candidate-store-only")
        del members["cycle-fixture/GENERATION.json"]
        build_archive(candidate, extra_members=members)
        self.assert_rejected(
            candidate,
            "candidate ZIP has a generation store/receipt split",
        )

    def test_previous_generation_store_without_receipt_is_not_first_adoption(self):
        previous = self.root / "previous-store-only.zip"
        candidate = self.root / "candidate-generation.zip"
        previous_members, _previous_id = generation_members(
            "previous-store-only"
        )
        del previous_members["cycle-fixture/GENERATION.json"]
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "previous ZIP has a generation store/receipt split",
            previous=previous,
        )

    def test_generation_store_cannot_be_removed_by_update(self):
        previous = self.root / "previous-generation.zip"
        candidate = self.root / "candidate-plain.zip"
        previous_members, _previous_id = generation_members("removed-contract")
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate)
        self.assert_rejected(
            candidate,
            "candidate removed the authenticated generation contract",
            previous=previous,
        )

    def test_generation_update_preserves_authenticated_legacy_v1_root(self):
        previous = self.root / "previous-generation-v1.zip"
        candidate = self.root / "candidate-generation-v2.zip"
        previous_members, previous_id = generation_members(
            "previous-v1", schema_version=1, legacy_32=True,
            execution_roles=MIXED_EXECUTION_ROLES,
        )
        candidate_members, _candidate_id = generation_members("candidate-v2")
        self.assertEqual(len(previous_id), 32)
        legacy_receipt = json.loads(
            previous_members["cycle-fixture/GENERATION.json"].decode("utf-8")
        )
        legacy_paths = [item["path"] for item in legacy_receipt["artifacts"]]
        self.assertNotEqual(legacy_paths, sorted(legacy_paths))
        self.assertEqual(
            legacy_paths,
            sorted(legacy_paths, key=lambda value: Path(value).parts),
        )
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        result = self.run_cycle(candidate, previous)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("retained_generations=1", result.stdout)

    def test_generation_legacy_execution_roles_drift_is_rejected(self):
        previous = self.root / "previous-generation-v1-roles.zip"
        candidate = self.root / "candidate-generation-v2.zip"
        previous_members, _previous_id = generation_members(
            "previous-v1-roles", schema_version=1, legacy_32=True,
            execution_roles=MIXED_EXECUTION_ROLES,
        )
        receipt_path = "cycle-fixture/GENERATION.json"
        receipt = json.loads(previous_members[receipt_path].decode("utf-8"))
        receipt["execution_roles"]["game"]["architecture"] = "aarch64"
        previous_members[receipt_path] = (
            json.dumps(receipt, indent=2, sort_keys=True).encode("utf-8") + b"\n"
        )
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "execution_roles differ from stored nxport.json",
            previous=previous,
        )

    def test_generation_like_stale_member_without_receipt_auth_is_rejected(self):
        previous = self.root / "previous-generation-ghost.zip"
        candidate = self.root / "candidate-generation.zip"
        members, old = generation_members("previous-ghost")
        members[
            "cycle-fixture/.nxruntime/generations/%s/ghost.txt" % old
        ] = b"not authenticated\n"
        build_archive(previous, extra_members=members)
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "generation artifact closure is incomplete",
            previous=previous,
        )

    def test_generation_store_hash_drift_is_rejected(self):
        previous = self.root / "previous-generation-drift.zip"
        candidate = self.root / "candidate-generation.zip"
        members, old = generation_members("previous-drift")
        members[
            "cycle-fixture/.nxruntime/generations/%s/manifest.json" % old
        ] += b"drift"
        build_archive(previous, extra_members=members)
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "generation artifact hash is stale",
            previous=previous,
        )

    def test_generation_store_unknown_root_is_rejected(self):
        previous = self.root / "previous-generation-extra-root.zip"
        candidate = self.root / "candidate-generation.zip"
        members, old = generation_members("previous-extra-root")
        unknown = "4" * 64
        members[
            "cycle-fixture/.nxruntime/generations/%s/commit" % unknown
        ] = (unknown + "\n").encode("ascii")
        build_archive(previous, extra_members=members)
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "generation store contains an unauthenticated root",
            previous=previous,
        )

    def test_generation_store_same_id_identical_closure_passes(self):
        previous = self.root / "previous-generation-same-id.zip"
        candidate = self.root / "candidate-generation-same-id.zip"
        previous_members, generation_id = generation_members("same-id")
        candidate_members, candidate_id = generation_members("same-id")
        self.assertEqual(generation_id, candidate_id)
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        result = self.run_cycle(candidate, previous)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("retained_generations=0", result.stdout)

    def test_generation_store_same_id_cannot_change_bytes(self):
        previous = self.root / "previous-generation-same-id.zip"
        candidate = self.root / "candidate-generation-same-id.zip"
        previous_members, generation_id = generation_members(
            "same-id-v1", schema_version=1
        )
        candidate_members, candidate_id = generation_members(
            "same-id-v1", schema_version=1,
            launcher_suffix=b"# independently valid divergent launcher\n",
        )
        self.assertEqual(generation_id, candidate_id)
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "same generation id has a different path/mode/hash closure",
            previous=previous,
        )

    def test_generation_v2_duplicate_launcher_id_is_rejected(self):
        previous = self.root / "previous-generation-duplicate-id.zip"
        candidate = self.root / "candidate-generation.zip"
        previous_members, _previous_id = generation_members(
            "duplicate-launcher-id", duplicate_launcher_id=True
        )
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "launcher does not carry exactly one generation id",
            previous=previous,
        )

    def test_generation_store_partial_closure_is_rejected(self):
        previous = self.root / "previous-generation-partial.zip"
        candidate = self.root / "candidate-generation.zip"
        previous_members, generation_id = generation_members("previous-partial")
        missing = (
            "cycle-fixture/.nxruntime/generations/%s/components.sha256" %
            generation_id
        )
        del previous_members[missing]
        receipt_path = "cycle-fixture/GENERATION.json"
        receipt = json.loads(previous_members[receipt_path].decode("utf-8"))
        receipt["artifacts"] = [
            artifact for artifact in receipt["artifacts"]
            if artifact["path"] != missing
        ]
        previous_members[receipt_path] = (
            json.dumps(receipt, indent=2, sort_keys=True).encode("utf-8") + b"\n"
        )
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "v2 control components.sha256 is stale",
            previous=previous,
        )

    def test_generation_artifact_mode_mismatch_is_rejected(self):
        previous = self.root / "previous-generation-mode.zip"
        candidate = self.root / "candidate-generation.zip"
        previous_members, generation_id = generation_members("previous-mode")
        logical = (
            "cycle-fixture/.nxruntime/generations/%s/files/runtime/"
            "cycle-nextos" % generation_id
        )
        receipt_path = "cycle-fixture/GENERATION.json"
        receipt = json.loads(previous_members[receipt_path].decode("utf-8"))
        for artifact in receipt["artifacts"]:
            if artifact["path"] == logical:
                artifact["mode"] = "0644"
        previous_members[receipt_path] = (
            json.dumps(receipt, indent=2, sort_keys=True).encode("utf-8") + b"\n"
        )
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "generation artifact mode is stale",
            previous=previous,
        )

    def test_generation_artifact_order_mismatch_is_rejected(self):
        previous = self.root / "previous-generation-order.zip"
        candidate = self.root / "candidate-generation.zip"
        previous_members, _generation_id = generation_members("previous-order")
        receipt_path = "cycle-fixture/GENERATION.json"
        receipt = json.loads(previous_members[receipt_path].decode("utf-8"))
        receipt["artifacts"][0], receipt["artifacts"][1] = (
            receipt["artifacts"][1], receipt["artifacts"][0]
        )
        previous_members[receipt_path] = (
            json.dumps(receipt, indent=2, sort_keys=True).encode("utf-8") + b"\n"
        )
        candidate_members, _candidate_id = generation_members("candidate-v2")
        build_archive(previous, extra_members=previous_members)
        build_archive(candidate, extra_members=candidate_members)
        self.assert_rejected(
            candidate,
            "artifact inventory is not canonically ordered",
            previous=previous,
        )

    def test_missing_runtime_reproduces_and_is_rejected_before_packaging(self):
        archive = self.root / "missing-runtime.zip"
        value = metadata()
        del value["attr"]["runtime"]
        build_archive(archive, value)
        self.assert_rejected(archive, "pinned port_info_load rejected candidate")

    def test_absent_runtime_in_v4_completes_the_real_cycle(self):
        archive = self.root / "runtime-absent-v4.zip"
        value = metadata()
        value["version"] = 4
        del value["attr"]["runtime"]
        build_archive(archive, value)
        result = self.run_cycle(archive)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("install=1 update=0 uninstall=1 reinstall=1",
                      result.stdout)

    def test_wrong_runtime_type_is_rejected(self):
        archive = self.root / "wrong-runtime.zip"
        value = metadata()
        value["attr"]["runtime"] = 7
        build_archive(archive, value)
        self.assert_rejected(archive, "port.json schema rejected metadata")

    def test_duplicate_json_key_is_rejected(self):
        archive = self.root / "duplicate-key.zip"
        raw = json.dumps(metadata(), sort_keys=True)
        raw = raw.replace('"runtime": null', '"runtime": null, "runtime": []')
        build_archive(archive, raw_metadata=raw)
        self.assert_rejected(archive, "duplicate key 'runtime'")

    def test_truncated_json_is_rejected(self):
        archive = self.root / "truncated.zip"
        build_archive(archive, raw_metadata='{"version": 1, "attr": ')
        self.assert_rejected(archive, "malformed JSON")

    def test_item_case_mismatch_is_rejected(self):
        archive = self.root / "case.zip"
        value = metadata()
        value["items"][1] = "Cycle-Fixture/"
        build_archive(archive, value)
        self.assert_rejected(archive, "required item is absent")

    def test_missing_declared_item_is_rejected(self):
        archive = self.root / "missing-item.zip"
        value = metadata()
        value["items"][1] = "missing/"
        build_archive(archive, value)
        self.assert_rejected(archive, "required item is absent")

    def test_item_traversal_is_rejected(self):
        archive = self.root / "traversal.zip"
        value = metadata()
        value["items"][1] = "../escape"
        build_archive(archive, value)
        self.assert_rejected(archive, "port.json schema rejected metadata")

    def test_stale_member_after_overlay_update_is_rejected(self):
        previous = self.root / "previous.zip"
        candidate = self.root / "candidate.zip"
        build_archive(previous, extra_members={"cycle-fixture/retired.txt": b"old\n"})
        build_archive(candidate)
        self.assert_rejected(
            candidate,
            "candidate-install left stale installed member",
            previous=previous,
        )


if __name__ == "__main__":
    unittest.main()
