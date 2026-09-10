#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pure, process-free contract gate for nxandroid 0.2.0 adapter primitives."""

import hashlib
import json
from pathlib import Path
import re


SELF = Path(__file__).resolve()
REPOSITORY = SELF.parents[3]
LEDGER_PATH = REPOSITORY / (
    "framework/nxandroid/references/jni-adapter-primitives-v1.json")


class ContractError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def reject_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ContractError("duplicate JSON key: %s" % key)
        result[key] = value
    return result


def read_text(relative):
    path = REPOSITORY / relative
    require(path.is_file() and not path.is_symlink(),
            "missing or linked contract file: %s" % relative)
    return path.read_text(encoding="utf-8")


def sha256(relative):
    return hashlib.sha256((REPOSITORY / relative).read_bytes()).hexdigest()


def validate_ledger(document):
    require(document.get("schema") == "nxandroid-jni-adapter-primitives-v1",
            "wrong ledger schema")
    require(document.get("schema_version") == 1,
            "wrong ledger schema version")
    require(document.get("component_version") == "0.2.0",
            "wrong component version")
    contract = document.get("public_contract")
    require(contract == {
        "core_api_version": 1,
        "adapter_api_version": 1,
        "opt_in": True,
        "java_vm_owned": False,
        "jni_env_owned": False,
        "vtable_installed": False,
        "java_dispatch_implemented": False,
        "store_lock_owned": False,
        "port_specific_workaround_promoted": False,
    }, "adapter ownership boundary changed")

    sources = document.get("sources")
    require(isinstance(sources, list) and
            [item.get("id") for item in sources] ==
            ["bombchicken-1.1.6", "asphalt8-90009", "dtp2"],
            "approved source set/order changed")
    for source in sources:
        require(source.get("classification") ==
                "approved-positive-bounded", "unapproved positive source")
        require(re.fullmatch(r"[0-9a-f]{40}", source.get("commit", "")),
                "invalid source commit")
        require(isinstance(source.get("limits"), list) and
                source["limits"], "source limitations are missing")
        for evidence in source.get("evidence", []):
            require(re.fullmatch(r"[0-9a-f]{64}",
                                 evidence.get("sha256", "")),
                    "invalid evidence SHA-256")

    local_evidence = {
        "asphalt8-90009":
            "ports/asphalt8_90009/src/jni_bridge.c",
        "dtp2": "ports/dtp2/src/java/nx/NxPrefs.java",
    }
    for source in sources:
        local_path = local_evidence.get(source["id"])
        if local_path is not None:
            require(source["evidence"][0]["path"] == local_path,
                    "local evidence path changed")
            require(source["evidence"][0]["sha256"] == sha256(local_path),
                    "local approved evidence hash mismatch")

    observations = document.get("validation_observations")
    require(observations == [{
        "id": "bombchicken-fresh-process-resume",
        "classification": "project-observation-not-static-proof",
        "provenance": "Bomb Chicken 1.1.6 community report and release validation",
        "observation": "same-session managed caching masked the persisted GetString defect; persistence acceptance must kill the old PID and read in a new PID before any write",
        "limits": "This observation is not claimed as a fact proved by the pinned source lines or by the nxandroid host gate.",
    }], "fresh-process lesson lost its honest observation boundary")

    specs = document.get("normative_specs")
    require(isinstance(specs, list) and len(specs) == 4,
            "normative specification set changed")
    require(all(item.get("url", "").startswith("https://") for item in specs),
            "normative specification URL is not HTTPS")
    excluded = document.get("excluded_positive_sources")
    require(excluded == [{
        "id": "angrybirds2",
        "classification": "wip-negative-only",
        "reason": "A global reused map can invalidate earlier snapshots/iterators and the implementation does not cover all six value families.",
    }], "WIP exclusion changed")
    boundary = document.get("evidence_boundary")
    require(boundary == {
        "host_contract_tested": True,
        "guest_code_executed": False,
        "jni_vtable_installed": False,
        "physical_get_all_migration_proven": False,
        "adopting_port_requires_new_release_and_device_proof": True,
    }, "evidence boundary overclaims runtime proof")


def validate_public_surface():
    version = read_text("framework/nxandroid/VERSION").strip()
    core_header = read_text("framework/nxandroid/include/nxandroid.h")
    header = read_text("framework/nxandroid/include/nxandroid_jni_adapter.h")
    source = read_text("framework/nxandroid/src/nxandroid_jni_adapter.c")
    cmake = read_text("framework/nxandroid/CMakeLists.txt")
    require(version == "0.5.0" and
            '#define NXANDROID_VERSION "0.5.0"' in core_header and
            "project(nxandroid VERSION 0.5.0" in cmake,
            "component version surfaces disagree")
    for token in (
            "#define NXANDROID_JNI_ADAPTER_API_VERSION 1u",
            "#define NXANDROID_JNI_SLOT_GET_STRING_REGION 220u",
            "#define NXANDROID_JNI_SLOT_GET_STRING_UTF_REGION 221u",
            "nxandroid_utf16_copy_region",
            "nxandroid_prefs_snapshot_create",
            "NXANDROID_PREFS_STRING",
            "NXANDROID_PREFS_STRING_SET",
            "NXANDROID_PREFS_INT32",
            "NXANDROID_PREFS_INT64",
            "NXANDROID_PREFS_FLOAT",
            "NXANDROID_PREFS_BOOL"):
        require(token in header, "missing public adapter token: %s" % token)
    require("src/nxandroid_jni_adapter.c" in cmake and
            "include/nxandroid_jni_adapter.h" in cmake,
            "adapter source/header missing from build/install")
    for forbidden in ("JavaVM", "JNIEnv", "RegisterNatives", "FindClass",
                      "CallObjectMethod"):
        require(forbidden not in source,
                "adapter substrate acquired Java dispatch: %s" % forbidden)
    require("memmove(output" in source,
            "UTF-16 region lost overlap-safe copy")
    require("unit_length > source->unit_count - start_index" in source,
            "UTF-16 region lost subtraction bounds")
    require("nxandroid_prefs_snapshot_destroy(&snapshot)" in source,
            "snapshot failure is no longer transactional")


def validate_tests_and_docs():
    tests = read_text("framework/nxandroid/tests/test_nxandroid.c")
    runner = read_text("framework/nxandroid/tests/run-host.sh")
    matrix = json.loads(read_text("framework/tests/test-matrix-v1.json"),
                        object_pairs_hook=reject_duplicates)
    for token in ("0x00e9u", "0x4e2du", "0xd83du", "INT32_MAX",
                  "NXANDROID_PREFS_STRING_SET", "0x7fc12345",
                  "NXANDROID_JNI_ADAPTER_EDUPLICATE",
                  "nxandroid_prefs_snapshot_destroy(&first)"):
        require(token in tests, "focused host case missing: %s" % token)
    require(runner.count("src/nxandroid_jni_adapter.c") == 2,
            "new source is not in both analyzer loops")
    require("installed adapter header missing" in runner and
            "nxandroid_utf16_copy_region" in runner and
            "nxandroid_prefs_snapshot_create" in runner,
            "installed adapter API lacks link smoke")
    gates = {gate.get("id"): gate for gate in matrix.get("gates", [])}
    gate = gates.get("nxandroid-020-adapter-contract")
    require(gate is not None and gate.get("class") == "pure" and
            gate.get("command") == [
                "python3", "-B",
                "framework/nxandroid/tests/test_020_adapter_contract.py"],
            "0.2.0 pure gate missing from matrix")
    host = gates.get("nxandroid-host")
    require("framework/nxandroid/src/nxandroid_jni_adapter.c" in
            host.get("support_files", []) and
            "framework/nxandroid/include/nxandroid_jni_adapter.h" in
            host.get("support_files", []),
            "host matrix omits adapter implementation")
    for relative, tokens in {
        "framework/nxandroid/README.md":
            ("Passive JNI adapter primitives", "no process-global store"),
        "framework/nxandroid/CONTRACT.md":
            ("Ownership boundary", "physical"),
        "framework/nxandroid/CHANGELOG.md":
            ("0.2.0", "Existing ports remain"),
        "framework/nxandroid/REGRESSION-MATRIX.md":
            ("surrogate", "new PID"),
        "suportando_outros_devices/checklist-novo-port.md":
            ("PID antigo morreu", "antes de qualquer nova escrita"),
        "suportando_outros_devices/diagnostico-validacao.md":
            ("outro processo", "cache da mesma sessão"),
    }.items():
        text = read_text(relative)
        for token in tokens:
            require(token in text, "%s lacks %s" % (relative, token))


def main():
    document = json.loads(LEDGER_PATH.read_text(encoding="utf-8"),
                          object_pairs_hook=reject_duplicates)
    validate_ledger(document)
    validate_public_surface()
    validate_tests_and_docs()
    print("nxandroid_020_adapter_contract=PASS approved_sources=3 "
          "wip_positive_sources=0 guest_code_executed=0 device_access=0 "
          "network_access=0")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ContractError as error:
        print("nxandroid 0.2.0 adapter contract failed: %s" % error)
        raise SystemExit(1)
