#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Directed positive/negative tests for nxrelease public-final."""

import copy
import hashlib
import importlib.util
import inspect
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOL = ROOT / "nxrelease.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("nxrelease_public_final", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def expect_failure(module, label, needle, callback):
    try:
        callback()
    except module.ReleaseError as error:
        require(needle.lower() in str(error).lower(),
                "%s reported %r, expected %r" % (label, str(error), needle))
    else:
        raise AssertionError(label + " unexpectedly passed")


def json_bytes(value):
    return (json.dumps(value, sort_keys=True, indent=2) + "\n").encode("utf-8")


def single_build_wiring_fixture(module):
    """Exercise the one clean build -> current stage metadata/SBOM wiring."""
    with tempfile.TemporaryDirectory(
            prefix="nxrelease-public-final-reproduction-wiring-") as raw:
        root = pathlib.Path(raw)
        source_root = root / "source"
        source_root.mkdir()
        manifest = source_root / "release.json"
        manifest.write_text("{}\n", encoding="utf-8")
        payload = source_root / "payload.bin"
        payload.write_bytes(b"focused reproduction wiring\n")
        payload_sha256 = hashlib.sha256(payload.read_bytes()).hexdigest()
        config = {
            "candidate_lock": {
                "document_sha256": "2" * 64,
                "executable": "fixture/payload.bin",
                "schema": module.CANDIDATE_LOCK_SCHEMA,
                "schema_version": 1,
                "sha256": payload_sha256,
            },
            "candidate_lock_verified": True,
            "ceiling": "2.30",
            "compression": "deflated",
            "dependencies": [],
            "epoch": 1786492800,
            "exception_map": {},
            "launcher": "Fixture.sh",
            "launcher_chain": ["Fixture.sh"],
            "launcher_contract": {
                "config_path": "fixture/nxport.json",
                "config_sha256": "0" * 64,
                "generator": "nxbootstrap",
                "version": module.NXBOOTSTRAP_REQUIRED_VERSION,
            },
            "license": None,
            "manifest_path": manifest,
            "manifest_sha256": hashlib.sha256(
                manifest.read_bytes()).hexdigest(),
            "nxextract": {},
            "nxsplash": None,
            "package_id": "fixture",
            "package_version": "1.0.0",
            "port_dir": "fixture",
            "portmaster_metadata": {},
            "records": [{
                "expected_sha": payload_sha256,
                "kind": "payload",
                "mode": 0o644,
                "sha256": payload_sha256,
                "source": payload,
                "target": "fixture/payload.bin",
            }],
            "sdl3_exception": None,
            "source_root": source_root.resolve(),
        }
        tree = {"root": source_root.resolve(), "commit": "a" * 40}
        observed = []
        build_calls = []

        def focused_verify_stage(stage, requested_ceiling=None):
            metadata_name, _checksum_name, sbom_name = \
                module.internal_paths("fixture")
            metadata = json.loads(
                (pathlib.Path(stage) / metadata_name).read_text(
                    encoding="utf-8"))
            sbom = json.loads(
                (pathlib.Path(stage) / sbom_name).read_text(
                    encoding="utf-8"))
            observed.append((
                metadata["tool"]["version"],
                sbom["metadata"]["tools"][0]["version"],
                requested_ceiling,
            ))
            return {"config": config}

        def focused_create_archive(_stage, output):
            pathlib.Path(output).write_bytes(b"focused zip placeholder\n")
            return {
                "config": config,
                "package_id": config["package_id"],
                "package_version": config["package_version"],
            }

        replacements = {
            "_public_final_collect_build_outputs":
                lambda *_args, **_kwargs: {"game-nextos": payload},
            "_public_final_git_tree": lambda *_args, **_kwargs: dict(tree),
            "_public_final_prebuild_manifest":
                lambda *_args, **_kwargs: {
                    "epoch": config["epoch"],
                    "expected": {"game-nextos"},
                    "source_root": source_root.resolve(),
                },
            "_public_final_run_port_build":
                lambda *_args, **_kwargs: build_calls.append("build.sh"),
            "audit_record_set": lambda *_args, **_kwargs: ([], "2.17"),
            "create_archive": focused_create_archive,
            "load_manifest": lambda *_args, **_kwargs: config,
            "validate_sources": lambda *_args, **_kwargs: None,
            "verify_stage": focused_verify_stage,
        }
        originals = {
            name: getattr(module, name) for name in replacements
        }
        try:
            for name, replacement in replacements.items():
                setattr(module, name, replacement)
            result, returned_config, output, outputs = \
                module._public_final_build(
                    tree, manifest, "build.sh", root / "reproduced.zip",
                    "2.30", "focused wiring",
                    trusted_candidate_lock=config["candidate_lock"],
                )
        finally:
            for name, original in originals.items():
                setattr(module, name, original)

        require(observed == [(module.TOOL_VERSION, module.TOOL_VERSION,
                              "2.30")],
                "current tool version was not propagated through real stage "
                "metadata and SBOM")
        require(build_calls == ["build.sh"],
                "public-final did not compile exactly once")
        require(result["config"] is config and returned_config is config and
                output.is_file() and outputs == ["game-nextos"],
                "focused _public_final_build wiring did not complete")


def source_pins_fixture(module):
    return {
        "nxbootstrap": {"version": module.NXBOOTSTRAP_REQUIRED_VERSION},
        "nxsplash": {"version": module.NXSPLASH_REQUIRED_VERSION},
        "nxextract": None,
        "portmaster": {"contract": "v2"},
    }


def elf_audit_fixture(module, path, digest, kind, build_id, provenance):
    """One complete deterministic Linux ELF identity for public-final tests."""
    return {
        "architecture": "aarch64",
        "build_id": build_id,
        "build_profile": module.LOW_GLIBC_PROFILE,
        "cxxabi_max": None,
        "class": module.ARCH_CLASSES["aarch64"],
        "data": "little-endian",
        "elf_type": "DYN",
        "flags": "",
        "glibc_max": "2.17",
        "glibcxx_max": None,
        "interpreter": module.LINUX_INTERPRETERS["aarch64"],
        "kind": kind,
        "machine": module.ARCH_MACHINES["aarch64"],
        "namespace": "linux",
        "needed": [],
        "path": path,
        "provenance": provenance,
        "sha256": digest,
        "soname": None,
    }


def legacy_generation_fixture(module, physical=False, mode="external-legacy"):
    pid = "fixture"
    generation_id = "a" * 64
    project_nxport = {
        "schema_version": 2,
        "id": pid,
        "title": "Fixture",
        "launcher_name": "Fixture.sh",
        "architecture": "aarch64",
        "executable": "fixture-nextos",
        "argument_mode": "game-dir-and-passthrough",
        "home_mode": "preserve",
        "nxextract": {"mode": "yes", "version": "1.3.0"},
        "required_files": ["fixture-nextos"],
        "private_library_paths": [],
        "prepare_script": "",
        "required_capabilities": ["graphics.gles2"],
        "enabled_quirks": [],
        "runtime_report": "log-and-logo",
    }
    actions = [{"id": "fixture.confirm", "kind": "button",
                "sinks": ["fixture.input.confirm"]}]
    contexts = {"menu": {"A": "fixture.confirm"}}
    graphics = {
        "uses_gl": True,
        "api": "gles",
        "profile": "es",
        "version": "2.0",
        "version_policy": "minimum",
        "shader_dialect": "essl100",
        "drawable_ready_timeout_ms": 8000,
        "adopt_single_channel": True,
        "required_devices": ["mali-450"],
    }
    promotion_claims = {
        "release_ready": True,
        "physical_support_proven": physical,
        "adapter_lifecycle_implemented": True,
    }
    project = {
        "schema_version": 3,
        "nxport": project_nxport,
        "language_access": {"mode": "none", "supported": [],
                            "fallback": None, "sinks": []},
        "graphics": graphics,
        "controls": {"actions": actions, "contexts": contexts},
        "promotion": {"adapter_contract": "promotion/adapter.json",
                      "claims": promotion_claims},
    }
    adapter = {
        "schema": "nxadapter-skeleton-v1",
        "schema_version": 1,
        "status": "implemented_release",
        "release_ready": True,
        "lifecycle": {"sequence": ["JNI_OnLoad", "render"],
                      "source_evidence": ["src/main.c:JNI_OnLoad"]},
        "language_access": project["language_access"],
        "graphics": graphics,
        "input": {"actions": actions, "contexts": contexts},
        "audio": {"format": "callback", "callbacks": ["audio_cb"]},
        "terminal": {"action": "SELECT+START", "evidence": ["src/exit.c"]},
    }
    packaged_nxport = copy.deepcopy(project_nxport)
    packaged_nxport["required_files"].insert(1, "nxsplash-nextos")
    project_payload = json_bytes(project)
    adapter_payload = json_bytes(adapter)
    nxport_payload = json_bytes(packaged_nxport)
    runtime_member = "%s/.nxruntime/generations/%s/manifest.json" % (
        pid, generation_id)
    runtime = {
        "schema": "nxruntime-generation-v1",
        "schema_version": 1,
        "generation_id": generation_id,
        "identity_basis": {
            "schema": "org.nextos.nxruntime.generation-identity",
            "schema_version": 1,
            "nxport_sha256": hashlib.sha256(nxport_payload).hexdigest(),
        },
        "components": {
            "nxport.json": {
                "mode": "0644",
                "sha256": hashlib.sha256(nxport_payload).hexdigest(),
            }
        },
    }
    runtime_payload = json_bytes(runtime)
    claims = dict(promotion_claims)
    claims["deterministic_scaffold"] = True
    generation = {
        "schema": "nxgenerator-receipt-v1",
        "schema_version": 1,
        "generator": {
            "name": "nxgenerator",
            "version": module.NXGENERATOR_REQUIRED_VERSION,
        },
        "project_manifest_sha256": hashlib.sha256(project_payload).hexdigest(),
        "source_pins": source_pins_fixture(module),
        "artifacts": [
            {"path": pid + "/adapter/adapter-contract.json", "mode": "0644",
             "sha256": hashlib.sha256(adapter_payload).hexdigest()},
            {"path": pid + "/nxproject.json", "mode": "0644",
             "sha256": hashlib.sha256(project_payload).hexdigest()},
            {"path": runtime_member, "mode": "0644",
             "sha256": hashlib.sha256(runtime_payload).hexdigest()},
        ],
        "claims": claims,
        "generation_id": generation_id,
    }
    documents = {
        "adapter": adapter,
        "adapter_bytes": adapter_payload,
        "generation": generation,
        "generation_mode": mode,
        "generation_sha256": hashlib.sha256(json_bytes(generation)).hexdigest(),
        "nxport": packaged_nxport,
        "nxport_bytes": nxport_payload,
        "project": project,
        "project_bytes": project_payload,
        "runtime_bytes": runtime_payload,
        "runtime_manifest": runtime,
        "runtime_member": runtime_member,
    }
    config = {
        "package_id": pid,
        "package_version": "1.0.0",
        "port_dir": pid,
        "inventory": {
            pid + "/fixture-nextos": {
                "kind": "project-linux", "mode": 0o755,
                "sha256": "b" * 64,
            }
        },
        "elf_audit": {
            "files": [{"path": pid + "/fixture-nextos",
                       "sha256": "b" * 64, "build_id": "c" * 40}]
        },
    }
    return documents, config


def generation_fixture(module, physical=False, mode="embedded"):
    if mode == "external-legacy":
        return legacy_generation_fixture(module, physical, mode)

    pid = "fixture"
    executable_sha256 = "b" * 64
    splash_sha256 = "d" * 64
    notice_payload = b"Fixture third-party notice.\n"
    notice_sha256 = hashlib.sha256(notice_payload).hexdigest()
    project_runtime = [{
        "role": "executable",
        "path": "fixture-nextos",
        "mode": "0755",
        "sha256": executable_sha256,
    }]
    project_nxport = {
        "schema_version": 3,
        "id": pid,
        "title": "Fixture",
        "launcher_name": "Fixture.sh",
        "architecture": "aarch64",
        "executable": "fixture-nextos",
        "argument_mode": "game-dir-and-passthrough",
        "home_mode": "preserve",
        "nxextract": {"mode": "no", "version": "1.3.0"},
        "required_files": ["fixture-nextos"],
        "private_library_paths": [],
        "prepare_script": "",
        "required_capabilities": ["graphics.gles2"],
        "enabled_quirks": [],
        "runtime_report": "log-and-logo",
        "generation_runtime": project_runtime,
    }
    actions = [{"id": "fixture.confirm", "kind": "button",
                "sinks": ["fixture.input.confirm"]}]
    contexts = {"menu": {"A": "fixture.confirm"}}
    graphics = {
        "uses_gl": True,
        "api": "gles",
        "profile": "es",
        "version": "2.0",
        "version_policy": "minimum",
        "shader_dialect": "essl100",
        "drawable_ready_timeout_ms": 8000,
        "adopt_single_channel": True,
        "required_devices": ["mali-450"],
    }
    promotion_claims = {
        "release_ready": True,
        "physical_support_proven": physical,
        "adapter_lifecycle_implemented": True,
    }
    project = {
        "schema_version": 3,
        "nxport": project_nxport,
        "package_payload": [{
            "path": "NOTICE.md",
            "mode": "0644",
            "sha256": notice_sha256,
            "kind": "license-notice",
        }],
        "language_access": {"mode": "none", "supported": [],
                            "fallback": None, "sinks": []},
        "graphics": graphics,
        "controls": {"actions": actions, "contexts": contexts},
        "promotion": {"adapter_contract": "promotion/adapter.json",
                      "claims": promotion_claims},
    }
    adapter = {
        "schema": "nxadapter-skeleton-v1",
        "schema_version": 1,
        "status": "implemented_release",
        "release_ready": True,
        "lifecycle": {"sequence": ["JNI_OnLoad", "render"],
                      "source_evidence": ["src/main.c:JNI_OnLoad"]},
        "language_access": project["language_access"],
        "graphics": graphics,
        "input": {"actions": actions, "contexts": contexts},
        "audio": {"format": "callback", "callbacks": ["audio_cb"]},
        "terminal": {"action": "SELECT+START", "evidence": ["src/exit.c"]},
    }
    packaged_nxport = copy.deepcopy(project_nxport)
    packaged_nxport["required_files"].insert(1, "nxsplash-nextos")
    packaged_nxport["generation_runtime"].append({
        "role": "nxsplash",
        "path": "nxsplash-nextos",
        "mode": "0755",
        "sha256": splash_sha256,
    })
    project_payload = json_bytes(project)
    adapter_payload = json_bytes(adapter)
    nxport_payload = json_bytes(packaged_nxport)
    nxport_sha256 = hashlib.sha256(nxport_payload).hexdigest()

    launcher_preimage = (
        "#!/usr/bin/env bash\n"
        "NXBOOTSTRAP_GENERATION_ID=%s\n" % ("0" * 64)
    ).encode("ascii")
    launcher_preimage_sha256 = hashlib.sha256(launcher_preimage).hexdigest()
    runtime_records = "".join(
        "%s\t%s\t%s\t%s\n" % (
            member["role"], member["mode"], member["sha256"], member["path"]
        )
        for member in packaged_nxport["generation_runtime"]
    ).encode("utf-8")
    identity_components = [
        {"role": "launcher", "path": "Fixture.sh", "mode": "0755",
         "sha256": launcher_preimage_sha256},
        {"role": "nxport", "path": "nxport.json", "mode": "0644",
         "sha256": nxport_sha256},
    ] + copy.deepcopy(packaged_nxport["generation_runtime"])
    identity = {
        "schema": "org.nextos.nxruntime.generation-identity",
        "schema_version": 2,
        "nxport_sha256": nxport_sha256,
        "launcher_preimage_sha256": launcher_preimage_sha256,
        "runtime_records_sha256": hashlib.sha256(runtime_records).hexdigest(),
        "nxbootstrap": {
            "version": module.NXBOOTSTRAP_REQUIRED_VERSION,
            "generator_sha256": module.sha256_file(
                module.NXBOOTSTRAP_GENERATOR_PATH),
            "launcher_template_sha256": module.sha256_file(
                module.NXBOOTSTRAP_GENERATOR_PATH.parent.parent /
                "templates" / "launcher.sh.in"),
        },
        "components": identity_components,
        "nxsplash": {
            "version": module.NXSPLASH_REQUIRED_VERSION,
            "architecture": "aarch64",
            "sha256": splash_sha256,
        },
    }
    identity_payload = json_bytes(identity)
    generation_id = hashlib.sha256(identity_payload).hexdigest()
    launcher_payload = launcher_preimage.replace(
        ("0" * 64).encode("ascii"), generation_id.encode("ascii")
    )
    launcher_sha256 = hashlib.sha256(launcher_payload).hexdigest()
    runtime_components = copy.deepcopy(identity_components)
    runtime_components[0]["sha256"] = launcher_sha256
    runtime = {
        "schema": "nxruntime-generation-v2",
        "schema_version": 2,
        "generation_id": generation_id,
        "identity_basis": identity,
        "components": runtime_components,
    }
    runtime_payload = json_bytes(runtime)
    runtime_member = "%s/.nxruntime/generations/%s/manifest.json" % (
        pid, generation_id)
    components_v2 = "".join(
        "%s\t%s\t%s\t%s\n" % (
            member["role"], member["mode"], member["sha256"], member["path"]
        )
        for member in runtime_components
    ).encode("utf-8")
    checksums = []
    for member in runtime_components:
        if member["role"] == "launcher":
            path = "launcher/" + member["path"]
        elif member["role"] == "nxport":
            path = "nxport.json"
        else:
            path = "runtime/" + member["path"]
        checksums.append((path, member["sha256"]))
    components_sha256 = "".join(
        "%s  %s\n" % (digest, path)
        for path, digest in sorted(checksums)
    ).encode("utf-8")
    controls = {
        "commit": (generation_id + "\n").encode("ascii"),
        "components.sha256": components_sha256,
        "components.v2": components_v2,
        "format": b"nxruntime-generation-v2\n",
        "identity-runtime.v2": runtime_records,
        "identity.json": identity_payload,
        "manifest.json": runtime_payload,
    }

    root = "%s/.nxruntime/generations/%s" % (pid, generation_id)
    inventory = {
        "Fixture.sh": {"kind": "launcher", "mode": 0o755,
                       "sha256": launcher_sha256},
        pid + "/adapter/adapter-contract.json": {
            "kind": "payload", "mode": 0o644,
            "sha256": hashlib.sha256(adapter_payload).hexdigest()},
        pid + "/nxproject.json": {
            "kind": "payload", "mode": 0o644,
            "sha256": hashlib.sha256(project_payload).hexdigest()},
        pid + "/nxport.json": {"kind": "nxbootstrap-config", "mode": 0o644,
                               "sha256": nxport_sha256},
        pid + "/fixture-nextos": {"kind": "project-linux", "mode": 0o755,
                                  "sha256": executable_sha256},
        pid + "/NOTICE.md": {"kind": "license-notice", "mode": 0o644,
                              "sha256": notice_sha256},
        pid + "/nxsplash-nextos": {"kind": "nxsplash-linux", "mode": 0o755,
                                   "sha256": splash_sha256},
    }
    artifact_records = {
        pid + "/adapter/adapter-contract.json": {
            "mode": "0644", "sha256": hashlib.sha256(adapter_payload).hexdigest()},
        pid + "/nxproject.json": {
            "mode": "0644", "sha256": hashlib.sha256(project_payload).hexdigest()},
        pid + "/NOTICE.md": {
            "mode": "0644", "sha256": notice_sha256},
    }
    for name, payload in controls.items():
        path = root + "/" + name
        digest = hashlib.sha256(payload).hexdigest()
        inventory[path] = {"kind": "nxruntime-generation", "mode": 0o644,
                           "sha256": digest}
        artifact_records[path] = {"mode": "0644", "sha256": digest}
    component_paths = [
        (root + "/files/launcher/Fixture.sh", "Fixture.sh",
         "0755", launcher_sha256, "nxruntime-generation"),
        (root + "/files/nxport.json", pid + "/nxport.json",
         "0644", nxport_sha256, "nxruntime-generation"),
        (root + "/files/runtime/fixture-nextos", pid + "/fixture-nextos",
         "0755", executable_sha256, "nxruntime-generation-linux"),
        (root + "/files/runtime/nxsplash-nextos", pid + "/nxsplash-nextos",
         "0755", splash_sha256, "nxruntime-generation-linux"),
    ]
    for store_path, live_path, mode_value, digest, store_kind in component_paths:
        inventory[store_path] = {
            "kind": store_kind, "mode": int(mode_value, 8),
            "sha256": digest,
        }
        artifact_records[store_path] = {
            "mode": mode_value, "sha256": digest,
        }
        artifact_records[live_path] = {
            "mode": mode_value, "sha256": digest,
        }

    claims = dict(promotion_claims)
    claims["deterministic_scaffold"] = True
    generation = {
        "schema": "nxgenerator-receipt-v1",
        "schema_version": 1,
        "generator": {"name": "nxgenerator",
                      "version": module.NXGENERATOR_REQUIRED_VERSION},
        "project_manifest_sha256": hashlib.sha256(project_payload).hexdigest(),
        "source_pins": source_pins_fixture(module),
        "artifacts": [
            {"path": path, "mode": record["mode"],
             "sha256": record["sha256"]}
            for path, record in sorted(artifact_records.items())
        ],
        "claims": claims,
        "generation_id": generation_id,
    }
    generation_payload = json_bytes(generation)
    inventory[pid + "/GENERATION.json"] = {
        "kind": "payload", "mode": 0o644,
        "sha256": hashlib.sha256(generation_payload).hexdigest(),
    }
    live_executable_elf = elf_audit_fixture(
        module, pid + "/fixture-nextos", executable_sha256,
        "project-linux", "c" * 40, "directed project ELF fixture",
    )
    live_splash_elf = elf_audit_fixture(
        module, pid + "/nxsplash-nextos", splash_sha256,
        "nxsplash-linux", "e" * 40, "directed NXSplash ELF fixture",
    )
    store_executable_elf = dict(
        live_executable_elf,
        path=root + "/files/runtime/fixture-nextos",
        kind="nxruntime-generation-linux",
    )
    store_splash_elf = dict(
        live_splash_elf,
        path=root + "/files/runtime/nxsplash-nextos",
        kind="nxruntime-generation-linux",
    )
    documents = {
        "adapter": adapter,
        "adapter_bytes": adapter_payload,
        "generation": generation,
        "generation_mode": mode,
        "generation_bytes": generation_payload,
        "generation_sha256": hashlib.sha256(generation_payload).hexdigest(),
        "launcher_bytes": launcher_payload,
        "notice_bytes": notice_payload,
        "nxport": packaged_nxport,
        "nxport_bytes": nxport_payload,
        "project": project,
        "project_bytes": project_payload,
        "runtime_bytes": runtime_payload,
        "runtime_controls": controls,
        "runtime_manifest": runtime,
        "runtime_member": runtime_member,
    }
    config = {
        "package_id": pid,
        "package_version": "1.0.0",
        "port_dir": pid,
        "launcher": "Fixture.sh",
        "launcher_contract": {"version": module.NXBOOTSTRAP_REQUIRED_VERSION},
        "nxsplash": {
            "version": module.NXSPLASH_REQUIRED_VERSION,
            "architecture": "aarch64",
            "sha256": splash_sha256,
        },
        "inventory": inventory,
        "elf_audit": {
            "files": sorted(
                [live_executable_elf, live_splash_elf,
                 store_executable_elf, store_splash_elf],
                key=lambda item: item["path"],
            )
        },
    }
    return documents, config


def receipt_fixture(module, archive, config, contract, commit):
    archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
    generation = contract["generation_id"]
    run_id = "fixture-run-1"
    symbols = {
        "audio_cb", "fixture_sink", "main",
        "nxaudio_backend_recovery_format",
        "nxaudio_backend_recovery_run",
        "nxaudio_liveness_tick",
        "nxaudio_receipt_format",
        "nxgl_graphics_contract_adapter_shader_probe",
        "nxgl_graphics_contract_evidence_receipt",
        "nxgl_graphics_contract_validate",
        "request_exit",
    }
    gptk_declared = (
        contract["project"]["controls"].get("runtime_mapping") ==
        "nxinput-gptk"
    )
    if gptk_declared:
        symbols.update({
            "nxinput_gptk_dispatcher_feed_source",
            "nxinput_gptk_dispatcher_feed_button",
            "nxinput_gptk_dispatcher_register",
            "nxinput_gptk_dispatcher_register_sink",
            "nxinput_gptk_dispatcher_set_primary_mask",
            "nxinput_gptk_load_at",
            "nxinput_gptk_load_receipt_json",
            "nxinput_gptk_parse",
            "nxinput_gptk_source_guard_init",
        })
    module._DYNAMIC_SYMBOLS[contract["executable_path"]] = (set(), symbols)
    evidence = (
        "GRAPHICS-EVIDENCE: run_id=%s generation=%s commit=%s cfw=test "
        "build_id=%s shader_probe=pass verdict=OK reason=ok "
        "obtained=gles/es/2.0 drawable=640x480" %
        (run_id, generation, commit, "d" * 40)
    )
    value = {
        "schema": "nxrelease-public-final-receipt-v1",
        "schema_version": 1,
        "sanitized": True,
        "artifact": {
            "package_id": config["package_id"],
            "package_version": config["package_version"],
            "port_commit": commit,
            "zip_sha256": archive_hash,
            "zip_size": archive.stat().st_size,
            "executable_path": contract["executable_path"],
            "executable_sha256": "b" * 64,
            "executable_build_id": "c" * 40,
            "generation_id": generation,
        },
        "run": {"run_id": run_id, "device_id": "mali-450", "verdict": "PASS"},
        "integrations": {
            "lifecycle": {"completed": True,
                          "health_evidence": (
                              "UPDATE NXU0006: generation %s proved healthy "
                              "receipt_run=%s" % (generation, run_id)),
                          "symbols": ["main"],
                          "evidence_sha256": "1" * 64},
            "graphics": {
                "contract_initialized": True,
                "evidence": evidence,
                "evidence_sha256": "2" * 64,
                "frame_proof": {"verdict": "OK", "sample_count": 3,
                                "non_black_percent": 71.5},
                "symbols": sorted([
                    "nxgl_graphics_contract_adapter_shader_probe",
                    "nxgl_graphics_contract_evidence_receipt",
                    "nxgl_graphics_contract_validate",
                ]),
            },
            "input": ({
                "gptk_loaded": True,
                "gptk_sha256": "3" * 64,
                "parser": "nxinput_gptk",
                "dispatcher": "nxinput_gptk_dispatcher",
                "delivery_count": 1,
                "double_input": False,
                "ab_swap_observed": True,
                "start_select_observed": True,
                "symbols": sorted([
                    "nxinput_gptk_dispatcher_feed_source",
                    "nxinput_gptk_dispatcher_register",
                    "nxinput_gptk_dispatcher_set_primary_mask",
                    "nxinput_gptk_load_at",
                    "nxinput_gptk_load_receipt_json",
                    "nxinput_gptk_parse",
                    "nxinput_gptk_source_guard_init",
                ]),
                "sink_symbols": ["fixture_sink"],
                "evidence_sha256": "4" * 64,
            } if gptk_declared else None),
            "audio": {"callback_alive": True, "recovery_state": "not-needed",
                      "symbols": ["audio_cb", "nxaudio_liveness_tick",
                                  "nxaudio_receipt_format"],
                      "evidence_sha256": "5" * 64},
            "terminal": {"independent": True, "completed": True,
                         "symbols": ["request_exit"],
                         "evidence_sha256": "6" * 64},
        },
    }
    return value


def main():
    module = load_tool()
    single_build_wiring_fixture(module)
    public_source = inspect.getsource(module.command_public_final)
    require(public_source.count("_public_final_build(") == 1 and
            "build_a" not in public_source and "build_b" not in public_source,
            "public-final must perform exactly one clean compilation")
    require("verify_archive(archive" in public_source and
            "historical_authority" not in public_source,
            "public-final must verify under current policy, never quarantine")
    require("_public_final_external_candidate_lock(" in public_source,
            "public-final trusts only the lock projected inside the ZIP")
    with tempfile.TemporaryDirectory(
            prefix="nxrelease-public-final-authority-") as authority_raw:
        authority_root = pathlib.Path(authority_raw)
        authority_source = authority_root / "source"
        authority_source.mkdir()
        lock_path = authority_root / "candidate-lock.json"
        lock_path.write_text(json.dumps({
            "schema": module.CANDIDATE_LOCK_SCHEMA,
            "schema_version": 1,
            "executable": "fixture/fixture-nextos",
            "sha256": "1" * 64,
        }) + "\n", encoding="utf-8")
        lock_path.chmod(0o444)
        loaded_lock = module.load_candidate_lock(lock_path, authority_source)
        tested_config = {
            "candidate_lock": loaded_lock,
            "candidate_lock_verified": True,
        }
        require(module._public_final_external_candidate_lock(
            lock_path, authority_source, tested_config) == loaded_lock,
            "public-final lost the original external candidate authority")
        projected_only = dict(tested_config,
                              candidate_lock=dict(loaded_lock,
                                                  sha256="2" * 64))
        expect_failure(
            module, "embedded candidate projection cannot replace authority",
            "differs from the tested archive projection",
            lambda: module._public_final_external_candidate_lock(
                lock_path, authority_source, projected_only),
        )

    documents, config = generation_fixture(module, physical=True)
    contract = module._public_final_generation_contract(documents, config)
    require(contract["claims"]["physical_support_proven"] is True,
            "public-final lost the committed physical-support claim")

    def generation_negative(label, needle, mutate):
        candidate = copy.deepcopy(documents)
        mutate(candidate)
        expect_failure(
            module, label, needle,
            lambda: module._public_final_generation_contract(candidate, config),
        )

    generation_negative(
        "scaffold", "unimplemented_nonrelease",
        lambda value: value["adapter"].update(
            {"status": "unimplemented_nonrelease"}),
    )
    generation_negative(
        "adapter release false", "release_ready=false",
        lambda value: value["adapter"].update({"release_ready": False}),
    )
    generation_negative(
        "generation release false", "release_ready=false",
        lambda value: value["generation"]["claims"].update(
            {"release_ready": False}),
    )
    generation_negative(
        "generation physical support false", "physical_support_proven=false",
        lambda value: value["generation"]["claims"].update(
            {"physical_support_proven": False}),
    )
    generation_negative(
        "empty lifecycle", "lifecycle",
        lambda value: value["adapter"]["lifecycle"].update({"sequence": []}),
    )
    generation_negative(
        "input drift", "actions/contexts",
        lambda value: value["adapter"]["input"].update({"actions": []}),
    )
    generation_negative(
        "runtime generation drift", "generation_id",
        lambda value: value["runtime_manifest"].update(
            {"generation_id": "e" * 64}),
    )
    generation_negative(
        "stale generation control", "control file is stale",
        lambda value: value["runtime_controls"].update(
            {"components.v2": value["runtime_controls"]["components.v2"] +
             b"stale\n"}),
    )
    generation_negative(
        "incomplete runtime components", "components differ",
        lambda value: value["runtime_manifest"]["components"].pop(),
    )
    generation_negative(
        "embedded runtime v1", "closed generation-v2",
        lambda value: value["runtime_manifest"].update(
            {"schema": "nxruntime-generation-v1", "schema_version": 1}),
    )
    generation_negative(
        "ghost generation artifact", "artifact inventory differs",
        lambda value: value["generation"]["artifacts"].append({
            "path": "fixture/ghost.txt", "mode": "0644",
            "sha256": "0" * 64,
        }),
    )
    generation_negative(
        "uppercase package payload hash", "lowercase SHA-256",
        lambda value: value["project"]["package_payload"][0].update({
            "sha256": value["project"]["package_payload"][0]["sha256"].upper()
        }),
    )
    package_inventory_drift = copy.deepcopy(config)
    package_inventory_drift["inventory"]["fixture/NOTICE.md"]["kind"] = \
        "payload"
    expect_failure(
        module, "package payload inventory drift", "package_payload",
        lambda: module._public_final_generation_contract(
            documents, package_inventory_drift),
    )

    def remove_notice_artifact(value):
        value["generation"]["artifacts"] = [
            record for record in value["generation"]["artifacts"]
            if record["path"] != "fixture/NOTICE.md"
        ]

    generation_negative(
        "package payload receipt drift", "does not bind package_payload",
        remove_notice_artifact,
    )
    old_embedded_generator = copy.deepcopy(documents)
    old_embedded_generator["generation"]["generator"]["version"] = "0.2.18"
    expect_failure(
        module, "old embedded generator", "nxgenerator 0.4.5",
        lambda: module._public_final_generation_contract(
            old_embedded_generator, config),
    )
    external_v3 = copy.deepcopy(documents)
    external_v3["generation_mode"] = "external-legacy"
    external_v3["generation"]["generator"]["version"] = "0.2.14"
    expect_failure(
        module, "schema3 external downgrade", "only in quarantine",
        lambda: module._public_final_generation_contract(external_v3, config),
    )
    unicode_runtime = copy.deepcopy(documents["runtime_manifest"])
    unicode_runtime["components"][-1]["path"] = "jogo-é-nextos"
    require(
        b"jogo-\\u00e9-nextos" in
        module._public_final_runtime_manifest_bytes(unicode_runtime),
        "runtime manifest lost nxbootstrap ensure_ascii=True bytes",
    )
    generation_root = "%s/.nxruntime/generations/%s" % (
        config["port_dir"], contract["generation_id"])
    store_executable_path = (
        generation_root + "/files/runtime/fixture-nextos"
    )
    hybrid_config = copy.deepcopy(config)
    hybrid_config["inventory"][store_executable_path]["sha256"] = "0" * 64
    expect_failure(
        module, "hybrid live/store bytes", "bytes different",
        lambda: module._public_final_generation_contract(
            documents, hybrid_config),
    )
    wrong_store_kind_config = copy.deepcopy(config)
    wrong_store_kind_config["inventory"][store_executable_path]["kind"] = \
        "nxruntime-generation"
    expect_failure(
        module, "generation ELF store kind", "wrong inventory kind",
        lambda: module._public_final_generation_contract(
            documents, wrong_store_kind_config),
    )
    store_mode_drift_config = copy.deepcopy(config)
    store_mode_drift_config["inventory"][store_executable_path]["mode"] = 0o644
    expect_failure(
        module, "generation ELF store mode", "wrong mode",
        lambda: module._public_final_generation_contract(
            documents, store_mode_drift_config),
    )
    store_metadata_drift_config = copy.deepcopy(config)
    next(
        item for item in store_metadata_drift_config["elf_audit"]["files"]
        if item["path"] == store_executable_path
    )["provenance"] = "drifted immutable-store provenance"
    expect_failure(
        module, "generation ELF metadata drift", "metadata differs",
        lambda: module._public_final_generation_contract(
            documents, store_metadata_drift_config),
    )
    store_audit_path_drift_config = copy.deepcopy(config)
    next(
        item for item in store_audit_path_drift_config["elf_audit"]["files"]
        if item["path"] == store_executable_path
    )["path"] = generation_root + "/files/runtime/renamed-nextos"
    expect_failure(
        module, "generation ELF audit path drift", "metadata differs",
        lambda: module._public_final_generation_contract(
            documents, store_audit_path_drift_config),
    )
    extra_store_config = copy.deepcopy(config)
    extra_store_config["inventory"][generation_root + "/unexpected"] = {
        "kind": "nxruntime-generation", "mode": 0o644,
        "sha256": "0" * 64,
    }
    expect_failure(
        module, "extra generation member", "one exact closed generation",
        lambda: module._public_final_generation_contract(
            documents, extra_store_config),
    )
    legacy, legacy_config = generation_fixture(
        module, physical=True, mode="external-legacy"
    )
    legacy["generation"]["generator"]["version"] = "0.2.14"
    expect_failure(
        module, "legacy helper publication", "only in quarantine",
        lambda: module._public_final_generation_contract(
            legacy, legacy_config),
    )

    with tempfile.TemporaryDirectory(prefix="nxrelease-public-final-test-") as raw:
        root = pathlib.Path(raw)
        package_archive = root / "package-shaped.zip"
        generation_root = "%s/.nxruntime/generations/%s" % (
            config["port_dir"], contract["generation_id"])
        with zipfile.ZipFile(package_archive, "w") as package:
            package.writestr("Fixture.sh", documents["launcher_bytes"])
            package.writestr(
                "fixture/nxproject.json", documents["project_bytes"])
            package.writestr(
                "fixture/adapter/adapter-contract.json",
                documents["adapter_bytes"],
            )
            package.writestr("fixture/nxport.json", documents["nxport_bytes"])
            package.writestr(
                "fixture/GENERATION.json", documents["generation_bytes"])
            package.writestr("fixture/NOTICE.md", documents["notice_bytes"])
            for name, payload in documents["runtime_controls"].items():
                package.writestr(generation_root + "/" + name, payload)
        archive_documents = module._public_final_documents(
            package_archive, config
        )
        archive_contract = module._public_final_generation_contract(
            archive_documents, config
        )
        require(
            archive_contract["generation_id"] == contract["generation_id"],
            "archive document reader lost generation-v2 identity",
        )

        missing_control_archive = root / "missing-control.zip"
        with zipfile.ZipFile(missing_control_archive, "w") as package:
            package.writestr("Fixture.sh", documents["launcher_bytes"])
            package.writestr(
                "fixture/nxproject.json", documents["project_bytes"])
            package.writestr(
                "fixture/adapter/adapter-contract.json",
                documents["adapter_bytes"],
            )
            package.writestr("fixture/nxport.json", documents["nxport_bytes"])
            package.writestr(
                "fixture/GENERATION.json", documents["generation_bytes"])
            package.writestr("fixture/NOTICE.md", documents["notice_bytes"])
            for name, payload in documents["runtime_controls"].items():
                if name != "identity-runtime.v2":
                    package.writestr(generation_root + "/" + name, payload)
        expect_failure(
            module, "archive missing v2 control", "absent from the final ZIP",
            lambda: module._public_final_documents(
                missing_control_archive, config),
        )

        archive = root / "tested.zip"
        archive.write_bytes(b"exact tested archive bytes")
        commit = "f" * 40
        receipt = receipt_fixture(module, archive, config, contract, commit)
        receipt_path = root / "receipt.json"

        def write_and_validate(value, selected_contract=contract):
            receipt_path.write_text(
                json.dumps(value, sort_keys=True, indent=2) + "\n",
                encoding="utf-8",
            )
            return module._public_final_validate_receipts(
                [receipt_path], archive, config, selected_contract, commit
            )

        summaries = write_and_validate(receipt)
        require(summaries[0]["device_id"] == "mali-450",
                "native-passthrough receipt lost device identity")

        gptk_contract = copy.deepcopy(contract)
        gptk_contract["project"]["controls"]["runtime_mapping"] = \
            "nxinput-gptk"
        gptk_contract["adapter"]["input"]["runtime_mapping"] = \
            "nxinput-gptk"
        gptk_receipt = receipt_fixture(
            module, archive, config, gptk_contract, commit
        )
        write_and_validate(gptk_receipt, gptk_contract)

        invented_native_input = copy.deepcopy(receipt)
        invented_native_input["integrations"]["input"] = copy.deepcopy(
            gptk_receipt["integrations"]["input"]
        )
        expect_failure(
            module, "native receipt invents GPTK", "undeclared input integration",
            lambda: write_and_validate(invented_native_input),
        )
        missing_gptk_input = copy.deepcopy(gptk_receipt)
        missing_gptk_input["integrations"]["input"] = None
        expect_failure(
            module, "declared GPTK receipt is null", "must be a JSON object",
            lambda: write_and_validate(missing_gptk_input, gptk_contract),
        )

        missing_health = copy.deepcopy(receipt)
        del missing_health["integrations"]["lifecycle"]["health_evidence"]
        expect_failure(module, "missing health", "health_evidence",
                       lambda: write_and_validate(missing_health))
        for label, evidence in (
            ("prehealth failure",
             "UPDATE NXU0005: no valid run-bound health receipt"),
            ("stable no-health",
             "UPDATE NXU0007: stable generation produced no valid health receipt"),
            ("wrong health run",
             "UPDATE NXU0006: generation %s proved healthy receipt_run=other-run" %
             contract["generation_id"]),
            ("wrong health generation",
             "UPDATE NXU0006: generation %s proved healthy receipt_run=%s" %
             ("0" * 64, receipt["run"]["run_id"])),
        ):
            bad_health = copy.deepcopy(receipt)
            bad_health["integrations"]["lifecycle"]["health_evidence"] = evidence
            expect_failure(module, label, "run-bound NXU0006",
                           lambda value=bad_health: write_and_validate(value))

        recovered = copy.deepcopy(receipt)
        recovered["integrations"]["audio"]["recovery_state"] = "recovered"
        recovered["integrations"]["audio"]["symbols"].extend([
            "nxaudio_backend_recovery_format",
            "nxaudio_backend_recovery_run",
        ])
        recovered["integrations"]["audio"]["symbols"].sort()
        write_and_validate(recovered)

        black = copy.deepcopy(receipt)
        black["integrations"]["graphics"]["frame_proof"]["verdict"] = "BLACK"
        expect_failure(module, "black frame", "conclusively BLACK",
                       lambda: write_and_validate(black))
        duplicate = copy.deepcopy(gptk_receipt)
        duplicate["integrations"]["input"]["delivery_count"] = 2
        expect_failure(module, "double delivery", "exactly-one delivery",
                       lambda: write_and_validate(duplicate, gptk_contract))
        invented_input = copy.deepcopy(gptk_receipt)
        invented_input["integrations"]["input"]["symbols"] = [
            "nxinput_gptk_dispatcher_feed_button",
            "nxinput_gptk_dispatcher_register_sink",
            "nxinput_gptk_parse",
        ]
        expect_failure(module, "invented input API", "parser/dispatcher symbols",
                       lambda: write_and_validate(invented_input, gptk_contract))
        incomplete_recovery = copy.deepcopy(receipt)
        incomplete_recovery["integrations"]["audio"]["recovery_state"] = \
            "recovered"
        expect_failure(module, "unimplemented audio recovery",
                       "canonical bounded recovery symbols",
                       lambda: write_and_validate(incomplete_recovery))
        wrong_zip = copy.deepcopy(receipt)
        wrong_zip["artifact"]["zip_sha256"] = "0" * 64
        expect_failure(module, "wrong ZIP", "exact ZIP/ELF/generation",
                       lambda: write_and_validate(wrong_zip))
        missing_symbol = copy.deepcopy(receipt)
        missing_symbol["integrations"]["lifecycle"]["symbols"] = ["absent"]
        expect_failure(module, "missing symbol", "absent from the final ELF",
                       lambda: write_and_validate(missing_symbol))
        private = copy.deepcopy(receipt)
        private["run"]["run_id"] = "192.168.1.7"
        expect_failure(module, "private receipt", "private host information",
                       lambda: write_and_validate(private))

        provenance_path = root / "BUILD-PROVENANCE.json"
        provenance = {
            "schema": module.PUBLIC_FINAL_PROVENANCE_SCHEMA,
            "schema_version": 1,
            "sanitized": True,
            "verdict": "PUBLIC-FINAL-PASS",
        }
        module._public_final_write_provenance(provenance_path, provenance)
        require(provenance_path.is_file() and
                (provenance_path.stat().st_mode & 0o777) == 0o644,
                "BUILD-PROVENANCE was not created safely")
        expect_failure(
            module, "provenance no-overwrite", "already exists",
            lambda: module._public_final_write_provenance(
                provenance_path, provenance),
        )

        repo = root / "clean-tree"
        repo.mkdir()
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        subprocess.run(["git", "-C", str(repo), "config", "user.name", "Test"],
                       check=True)
        subprocess.run(["git", "-C", str(repo), "config", "user.email",
                        "test@example.invalid"], check=True)
        (repo / "tracked").write_text("clean\n", encoding="utf-8")
        build_script = repo / "build.sh"
        build_script.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        build_script.chmod(0o755)
        (repo / ".gitignore").write_text("/ignored-elf\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(repo), "add", "tracked", "build.sh",
                        ".gitignore"],
                       check=True)
        subprocess.run(["git", "-C", str(repo), "commit", "-qm", "fixture"],
                       check=True)
        tree = module._public_final_git_tree(repo, "fixture")
        require(len(tree["commit"]) == 40, "clean worktree lost full commit")
        (repo / "ignored-elf").write_bytes(b"ignored build residue")
        expect_failure(
            module, "ignored build residue", "ignored/untracked build output",
            lambda: module._public_final_git_tree(repo, "fixture"),
        )
        (repo / "ignored-elf").unlink()
        external_output = root / "external-output"
        external_output.mkdir()
        require(module._public_final_run_port_build(
            tree, "build.sh", external_output, 1786492800,
            "fixture") == "build.sh",
            "public-final did not run the versioned build script")
        shutil.copyfile("/bin/true", external_output / "game-nextos")
        external_hash = hashlib.sha256(
            (external_output / "game-nextos").read_bytes()
        ).hexdigest()
        nxengine = module.NXEXTRACT_ENGINES["1.3.0"]
        prebuild_manifest = repo / "release.json"
        clean_manifest = {
            "schema_version": module.SCHEMA_VERSION,
            "source_root": ".",
            "package": {
                "id": "fixture",
                "version": "1.0.0",
                "profile": module.PROFILE,
                "launcher": "Fixture.sh",
                "launcher_chain": ["Fixture.sh"],
                "launcher_contract": {
                    "generator": "nxbootstrap",
                    "version": module.NXBOOTSTRAP_REQUIRED_VERSION,
                    "config_path": "fixture/nxport.json",
                    "config_sha256": "0" * 64,
                },
                "port_dir": "fixture",
                "license": {
                    "spdx_id": "GPL-3.0-only",
                    "source_url": "https://example.invalid/source",
                    "file": "fixture/LICENSE",
                },
            },
            "release": {
                "source_date_epoch": 1786492800,
                "max_glibc": "2.30",
                "compression": "deflated",
            },
            "nxextract": {
                "path": "fixture/nxextract/nxextract.py",
                "version": "1.3.0",
                "minimum_version": "1.3.0",
                "sha256": nxengine["engine_sha256"],
                "runner_path": "fixture/nxextract/run-extractor.sh",
                "runner_sha256": nxengine["runner_sha256"],
                "runtime_env_path":
                    "fixture/nxextract/nxextract-runtime-env.sh",
                "runtime_env_sha256": nxengine["runtime_env_sha256"],
                "ui_path": "fixture/nxextract/nxextract-ui",
                "ui_sha256": "1" * 64,
                "recipe_path": "fixture/extractor.json",
                "recipe_sha256": "2" * 64,
            },
            "portmaster_metadata": {},
            "dependencies": [],
            "sdl3_exception": None,
            "files": [{
                "source": "game-nextos",
                "target": "fixture/game-nextos",
                "kind": "project-linux",
                "mode": "0755",
                "sha256": external_hash,
                "architecture": "aarch64",
                "build_profile": module.LOW_GLIBC_PROFILE,
                "provenance": "directed clean external build fixture",
                "needed": [],
                "soname": None,
            }],
            "exceptions": [],
        }
        prebuild_manifest.write_text(
            json.dumps(clean_manifest, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        require(not (repo / "game-nextos").exists(),
                "fresh-source fixture accidentally contains a prebuilt ELF")
        preflight = module._public_final_prebuild_manifest(
            prebuild_manifest, "fixture"
        )
        require(preflight["expected"] == {"game-nextos"} and
                preflight["source_root"] == repo,
                "prebuild manifest did not tolerate the intentionally absent ELF")
        overrides = module._public_final_collect_build_outputs(
            preflight, external_output, "fixture"
        )
        require(sorted(overrides) == ["game-nextos"] and
                overrides["game-nextos"].is_file(),
                "external project ELF was not bound into the release build")

        # The complete validator must accept a clean source tree only through
        # the exact one-for-one project-linux override. Keep unrelated package
        # layout validation outside this directed unit; expand_inputs and its
        # hash/kind/source checks remain real.
        old_metadata = module.validate_portmaster_metadata_manifest
        old_dependencies = module.validate_dependencies_manifest
        old_members = module.validate_package_members
        module.validate_portmaster_metadata_manifest = lambda *_args: {}
        module.validate_dependencies_manifest = lambda *_args: []
        module.validate_package_members = lambda _config: None
        try:
            loaded = module.load_manifest(
                prebuild_manifest, project_linux_overrides=overrides
            )
            require(
                loaded["records"][0]["source"] == overrides["game-nextos"],
                "load_manifest did not consume the external project ELF",
            )
            expect_failure(
                module, "missing project override", "source does not exist",
                lambda: module.load_manifest(prebuild_manifest),
            )

            wrong_hash = copy.deepcopy(clean_manifest)
            wrong_hash["files"][0]["sha256"] = "9" * 64
            prebuild_manifest.write_text(
                json.dumps(wrong_hash, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            expect_failure(
                module, "external hash drift", "source hash mismatch",
                lambda: module.load_manifest(
                    prebuild_manifest, project_linux_overrides=overrides
                ),
            )

            non_project = copy.deepcopy(clean_manifest)
            non_project["files"].insert(0, {
                "source": "missing-payload",
                "target": "fixture/missing-payload",
                "kind": "payload",
                "mode": "0644",
                "sha256": external_hash,
            })
            prebuild_manifest.write_text(
                json.dumps(non_project, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            expect_failure(
                module, "override non-project", "only for kind project-linux",
                lambda: module.load_manifest(
                    prebuild_manifest,
                    project_linux_overrides={
                        "missing-payload": overrides["game-nextos"],
                        "game-nextos": overrides["game-nextos"],
                    },
                ),
            )
            expect_failure(
                module, "missing non-project input", "source does not exist",
                lambda: module.load_manifest(
                    prebuild_manifest, project_linux_overrides=overrides
                ),
            )
        finally:
            module.validate_portmaster_metadata_manifest = old_metadata
            module.validate_dependencies_manifest = old_dependencies
            module.validate_package_members = old_members
            prebuild_manifest.write_text(
                json.dumps(clean_manifest, sort_keys=True) + "\n",
                encoding="utf-8",
            )

        (external_output / "extra").write_text("unexpected\n", encoding="utf-8")
        expect_failure(
            module, "extra external output", "differs from project-linux",
            lambda: module._public_final_collect_build_outputs(
                preflight, external_output, "fixture"),
        )
        (external_output / "extra").unlink()
        (external_output / "game-nextos").unlink()
        expect_failure(
            module, "missing external output", "differs from project-linux",
            lambda: module._public_final_collect_build_outputs(
                preflight, external_output, "fixture"),
        )
        (external_output / "game-nextos").symlink_to("/bin/true")
        expect_failure(
            module, "symlink external output", "symlink",
            lambda: module._public_final_collect_build_outputs(
                preflight, external_output, "fixture"),
        )
        prebuild_manifest.unlink()
        (repo / "untracked").write_text("dirty\n", encoding="utf-8")
        expect_failure(module, "dirty source", "not clean",
                       lambda: module._public_final_git_tree(repo, "fixture"))

    parser = module.build_parser()
    choices = next(action for action in parser._actions
                   if action.dest == "command").choices
    require("public-final" in choices, "public-final subcommand is absent")
    public_options = {action.dest for action in choices["public-final"]._actions}
    require({"source", "manifest", "build", "candidate_lock"}.issubset(
                public_options) and
            not {"source_a", "source_b", "build_a", "build_b"} &
            public_options,
            "public-final parser still exposes the retired build-A/build-B flow")
    require(not {"generation_receipt", "allow_external_generation"} &
            public_options,
            "public-final parser still exposes legacy publication options")
    legacy_arguments = type("Arguments", (), {
        "generation_receipt": "legacy-GENERATION.json",
        "allow_external_generation": False,
    })()
    expect_failure(
        module, "legacy generation cannot become current public-final",
        "only in quarantine",
        lambda: module.command_public_final(legacy_arguments),
    )
    print("nxrelease public-final gate passed: positive=17 negatives=41 "
          "single-build=1 legacy-publication=blocked")


if __name__ == "__main__":
    main()
