#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free validator for the hash-pinned M11 adapter profiles.

This test reads JSON, Markdown, Python and the public C header as inert bytes.
It never opens a guest ELF and has no subprocess, device or network facility.
"""

import ast
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import re


SELF_PATH = Path(__file__)
REPOSITORY = SELF_PATH.resolve().parents[3]
PROFILE_RELATIVE = Path(
    "framework/nxandroid/references/m11-adapter-profiles-v1.json")

EXPECTED_PROFILE_IDS = (
    "bully2",
    "sonic4ep2",
    "horizonchase",
    "kotor",
    "tasm2_127",
)
EXPECTED_ABIS = {
    "bully2": "NXANDROID_ABI_AARCH64_BIONIC",
    "sonic4ep2": "NXANDROID_ABI_AARCH64_BIONIC",
    "horizonchase": "NXANDROID_ABI_AARCH64_BIONIC",
    "kotor": "NXANDROID_ABI_ARMV7_BIONIC",
    "tasm2_127": "NXANDROID_ABI_ARMV7_BIONIC",
}
EXPECTED_EVIDENCE_PREFIX = {
    "bully2": "ports/bully2/",
    "sonic4ep2": "ports/sonic4/",
    "horizonchase": "ports/horizonchase/",
    "kotor": "ports/kotor/",
    "tasm2_127": "ports/asm2_127/",
}
EXPECTED_GRAPHICS_RELATION = {
    "bully2": "gl_ready_before_surface_up",
    "sonic4ep2": "gl_ready_before_surface_up",
    "horizonchase": "gl_ready_before_surface_up",
    "kotor": "delegated_not_visible",
    "tasm2_127": "gl_ready_after_surface_up",
}
EXPECTED_PHASES = (
    "NXANDROID_PHASE_MODULE_INITIALIZED",
    "NXANDROID_PHASE_MODULE_JNI",
    "NXANDROID_PHASE_ACTIVITY_CREATE",
    "NXANDROID_PHASE_GRAPHICS_REQUEST",
    "NXANDROID_PHASE_SURFACE_UP",
    "NXANDROID_PHASE_SURFACE_CHANGED",
    "NXANDROID_PHASE_GL_READY",
    "NXANDROID_PHASE_RESUME",
    "NXANDROID_PHASE_FOCUS_GAIN",
    "NXANDROID_PHASE_ENTRY",
    "NXANDROID_PHASE_OBJECTS_READY",
    "NXANDROID_PHASE_INPUT_ENABLE",
    "NXANDROID_PHASE_RUN_LOOP",
    "NXANDROID_PHASE_INPUT_DISABLE",
    "NXANDROID_PHASE_FOCUS_LOSS",
    "NXANDROID_PHASE_PAUSE",
    "NXANDROID_PHASE_SURFACE_DOWN",
    "NXANDROID_PHASE_SAVE",
    "NXANDROID_PHASE_NATIVE_SHUTDOWN",
    "NXANDROID_PHASE_TERMINAL",
    "NXANDROID_PHASE_RUNTIME_DELEGATED",
)
EXPECTED_TERMINAL_POLICIES = (
    "NXANDROID_TERMINAL_NONE",
    "NXANDROID_TERMINAL_RETURN",
    "NXANDROID_TERMINAL_ADAPTER",
)
EXPECTED_FLAGS = (
    "NXANDROID_PROFILE_ALLOW_ADAPTER_TERMINAL",
    "NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME",
    "NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK",
)
EXPECTED_PROFILE_FLAGS = {
    "bully2": [],
    "sonic4ep2": [
        "NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK"],
    "horizonchase": [],
    "kotor": ["NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME"],
    "tasm2_127": [],
}
EXPECTED_SONIC_PRE_SURFACE_PHASES = (
    "NXANDROID_PHASE_GL_READY",
    "NXANDROID_PHASE_ENTRY",
    "NXANDROID_PHASE_OBJECTS_READY",
    "NXANDROID_PHASE_SURFACE_UP",
)
MODULE_PHASES = {
    "NXANDROID_PHASE_MODULE_INITIALIZED",
    "NXANDROID_PHASE_MODULE_JNI",
}
CYCLE_PHASES = {
    "NXANDROID_PHASE_GRAPHICS_REQUEST",
    "NXANDROID_PHASE_SURFACE_UP",
    "NXANDROID_PHASE_SURFACE_CHANGED",
    "NXANDROID_PHASE_GL_READY",
    "NXANDROID_PHASE_RESUME",
    "NXANDROID_PHASE_FOCUS_GAIN",
    "NXANDROID_PHASE_ENTRY",
    "NXANDROID_PHASE_OBJECTS_READY",
    "NXANDROID_PHASE_INPUT_ENABLE",
    "NXANDROID_PHASE_RUN_LOOP",
    "NXANDROID_PHASE_INPUT_DISABLE",
    "NXANDROID_PHASE_FOCUS_LOSS",
    "NXANDROID_PHASE_PAUSE",
    "NXANDROID_PHASE_SURFACE_DOWN",
}
EXPECTED_PIN_PATHS = {
    "inventory": "framework/nxandroid/references/m11-android-guests-v1.json",
    "inventory_audit": "framework/nxandroid/references/REFERENCE-AUDIT.md",
    "inventory_tool": "framework/nxandroid/tools/inventory_m11_guests.py",
    "nxandroid_header": "framework/nxandroid/include/nxandroid.h",
    "nxandroid_version": "framework/nxandroid/VERSION",
}
EXPECTED_SAFETY = {
    "static_files_only": True,
    "validator_process_free": True,
    "guest_files_opened": False,
    "guest_code_executed": False,
    "guest_initializers_executed": False,
    "guest_jni_onload_executed": False,
    "resolve_executed": False,
    "finalize_executed": False,
    "device_access": False,
    "network_access": False,
}
EXPECTED_INVENTORY_SAFETY = {
    "guest_code_executed": False,
    "guest_mapped": False,
    "imports_resolved": False,
    "guest_initializers_executed": False,
    "guest_jni_onload_executed": False,
    "guest_lifecycle_executed": False,
    "device_access": False,
    "network_access": False,
    "guest_files_copied": False,
    "legacy_tasm2_used": False,
}
EXPECTED_CORE_OWNS = {
    "java_vm": False,
    "jni_env": False,
    "activity": False,
    "surface": False,
    "graphics": False,
    "input": False,
    "engine_loop": False,
    "process_terminal": False,
}
EXPECTED_VOCABULARY = {
    "availability": ["proven", "not_available", "not_applicable"],
    "classification": [
        "source_proven",
        "common_boundary_adapter_implementation",
        "proven_limitation",
        "static_evidence_only",
        "delegated_not_visible",
    ],
    "module_applicability": ["required", "conditional", "not_applicable"],
    "jni_version_acceptance": [
        "exact", "positive_return", "not_validated", "not_applicable"],
    "graphics_surface_relation": [
        "gl_ready_before_surface_up", "gl_ready_after_surface_up",
        "delegated_not_visible"],
}
BASE_PROFILE_KEYS = {
    "id", "inventory_port_id", "adapter_family", "abi", "modules",
    "vm_jni_activity_boundary", "android_runtime_trace", "execution",
    "graphics_surface_order", "proven_phase_projection", "gaps",
    "terminal_contract", "rollback_policy",
}
MODULE_KEYS = {
    "id", "sha256", "inventory_role", "applicability",
    "module_initialized", "module_jni", "jni_onload_export", "jni_version",
}
JNI_VERSION_KEYS = {
    "acceptance", "exact_value", "exact_name",
    "exact_version_availability", "classification", "evidence_refs",
}
OBJECT_RECORD_KEYS = {
    "owner", "availability", "classification", "semantics", "evidence_refs",
}
EXECUTION_KEYS = {
    "core_executable", "validation_scope", "flags", "variants", "reason",
}
VARIANT_KEYS = {"id", "included_modules", "enabled_conditions"}
GRAPHICS_KEYS = {
    "relation", "availability", "classification", "semantics",
    "evidence_refs",
}
PHASE_RECORD_KEYS = {
    "order", "phase", "module_id", "cycle_id", "contract_id",
    "terminal_policy", "condition", "rollback_contract_id",
    "rollback_group", "closes_rollback_group", "availability",
    "classification", "semantics", "evidence_refs",
}
GAP_KEYS = {
    "phases", "availability", "classification", "semantics", "evidence_refs",
}
TERMINAL_KEYS = {
    "policy", "availability", "classification", "required_flag_if_completed",
    "contract_id", "semantics", "evidence_refs",
}
DELEGATED_KEYS = {
    "phases", "availability", "classification", "semantics", "evidence_refs",
}
CONTRACT_ID = re.compile(r"[a-z0-9][a-z0-9-]{0,254}\Z")
EVIDENCE_REF = re.compile(r"ports/[A-Za-z0-9_./-]+:[0-9][0-9,-]*\Z")
IPV4 = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")


class GateError(Exception):
    """A deterministic validation failure."""


def require(condition, message):
    if not condition:
        raise GateError(message)


def reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise GateError("duplicate JSON key: %s" % key)
        result[key] = value
    return result


def validate_relative_path(value):
    require(isinstance(value, str) and value != "", "invalid pinned path")
    pure = PurePosixPath(value)
    require(not pure.is_absolute() and ".." not in pure.parts,
            "pinned path must be repository-relative")
    require(str(pure) == value, "pinned path is not canonical")


def repository_file(relative, maximum_bytes=16 * 1024 * 1024):
    validate_relative_path(relative)
    path = REPOSITORY / relative
    require(not path.is_symlink(), "pinned file must not be a symlink")
    require(path.is_file(), "missing pinned file")
    size = path.stat().st_size
    require(0 < size <= maximum_bytes, "pinned file size is invalid")
    return path


def read_bytes(relative):
    return repository_file(relative).read_bytes()


def sha256_bytes(value):
    return hashlib.sha256(value).hexdigest()


def load_json_bytes(raw):
    require(len(raw) <= 16 * 1024 * 1024, "JSON exceeds bounded input size")
    try:
        return json.loads(raw.decode("utf-8"),
                          object_pairs_hook=reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise GateError("invalid JSON: %s" % error) from error


def walk_values(value):
    yield value
    if isinstance(value, dict):
        for key, child in value.items():
            yield key
            yield from walk_values(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk_values(child)


def validate_privacy_and_prohibitions(document, raw_text):
    lower = raw_text.lower()
    for forbidden in ("/home/", "/users/", "/mnt/", "/tmp/", "file://",
                      "ports/tasm2"):
        require(forbidden not in lower, "private or legacy token present")
    require(IPV4.search(raw_text) is None, "IP address present")
    require(re.search(r"\+0x[0-9a-f]+", lower) is None,
            "code-relative address present")
    for value in walk_values(document):
        if isinstance(value, str):
            require("offset" not in value.lower(),
                    "offset field or claim is prohibited")


def collect_inventory_evidence(inventory):
    return {
        value for value in walk_values(inventory)
        if isinstance(value, str) and EVIDENCE_REF.fullmatch(value)
    }


def validate_evidence_refs(container, allowed, prefix):
    refs = container.get("evidence_refs")
    require(isinstance(refs, list) and refs, "missing evidence_refs")
    require(len(refs) == len(set(refs)), "duplicate evidence reference")
    for reference in refs:
        require(isinstance(reference, str) and
                EVIDENCE_REF.fullmatch(reference) is not None,
                "invalid evidence line reference")
        require(reference in allowed,
                "evidence line is not pinned by the inventory")
        require(reference.startswith(prefix),
                "evidence line belongs to another adapter")


def validate_pins(document):
    pins = document.get("source_pins")
    require(isinstance(pins, dict) and set(pins) == set(EXPECTED_PIN_PATHS),
            "unexpected source pin set")
    loaded = {}
    for pin_id, expected_path in EXPECTED_PIN_PATHS.items():
        pin = pins[pin_id]
        require(isinstance(pin, dict), "invalid pin record")
        expected_keys = {"path", "sha256"}
        if pin_id == "inventory":
            expected_keys.update({"schema", "schema_version"})
        elif pin_id == "nxandroid_version":
            expected_keys.add("value")
        require(set(pin) == expected_keys, "unexpected pin fields")
        require(pin.get("path") == expected_path, "unexpected pinned path")
        raw = read_bytes(expected_path)
        require(pin.get("sha256") == sha256_bytes(raw),
                "pinned source hash mismatch")
        loaded[pin_id] = raw
    inventory = load_json_bytes(loaded["inventory"])
    inventory_pin = pins["inventory"]
    require(inventory_pin.get("schema") == inventory.get("schema"),
            "inventory schema pin mismatch")
    require(inventory_pin.get("schema_version") ==
            inventory.get("schema_version") == 1,
            "inventory schema version mismatch")
    version_text = loaded["nxandroid_version"].decode("ascii").strip()
    require(pins["nxandroid_version"].get("value") ==
            version_text == "0.5.0", "nxandroid version pin mismatch")
    return inventory, loaded["nxandroid_header"].decode("utf-8")


def validate_public_contract(document, header_text):
    contract = document.get("public_contract")
    require(isinstance(contract, dict) and set(contract) == {
        "api_version", "version", "phases", "terminal_policies",
        "profile_flags", "core_owns", "runtime_claim"},
        "missing or unexpected public contract field")
    require(contract.get("api_version") == 1, "wrong API version")
    require(contract.get("version") == "0.5.0", "wrong public version")
    require(tuple(contract.get("phases", ())) == EXPECTED_PHASES,
            "public phase vocabulary changed")
    require(tuple(contract.get("terminal_policies", ())) ==
            EXPECTED_TERMINAL_POLICIES, "terminal policy vocabulary changed")
    require(tuple(contract.get("profile_flags", ())) == EXPECTED_FLAGS,
            "profile flag vocabulary changed")
    require(contract.get("core_owns") == EXPECTED_CORE_OWNS,
            "core object ownership overclaim")
    for token in EXPECTED_PHASES + EXPECTED_TERMINAL_POLICIES + EXPECTED_FLAGS:
        require(re.search(r"\b%s\b" % re.escape(token), header_text) is not None,
                "public token missing from pinned header")
    require(re.search(
                r"\bNXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK\s*=\s*"
                r"1u\s*<<\s*2\b", header_text) is not None,
            "pre-Surface-callback opt-in is not public bit 2")
    require("#define NXANDROID_API_VERSION 1u" in header_text,
            "pinned header API version changed")
    require("#define NXANDROID_VERSION \"0.5.0\"" in header_text,
            "pinned header version changed")


def inventory_module_map(port):
    modules = port.get("modules")
    require(isinstance(modules, list) and modules, "inventory module list missing")
    result = {}
    for module in modules:
        module_id = module.get("id")
        require(isinstance(module_id, str) and module_id not in result,
                "duplicate inventory module")
        result[module_id] = module
    return result


def expected_applicability(role):
    if "static-evidence" in role:
        return "not_applicable"
    if role.startswith("optional-"):
        return "conditional"
    return "required"


def validate_jni_version(profile_id, module, inventory_module, allowed_refs,
                         evidence_prefix):
    version = module.get("jni_version")
    require(isinstance(version, dict) and set(version) == JNI_VERSION_KEYS,
            "missing or unexpected JNI version contract field")
    validate_evidence_refs(version, allowed_refs, evidence_prefix)
    acceptance = version.get("acceptance")
    exact_value = version.get("exact_value")
    exact_name = version.get("exact_name")
    exact_availability = version.get("exact_version_availability")
    classification = version.get("classification")
    onload = inventory_module["elf"]["jni_exports"]["jni_onload_present"]
    applicability = module["applicability"]
    if applicability == "not_applicable" or not onload:
        require(acceptance == "not_applicable" and exact_value is None and
                exact_name is None and exact_availability == "not_applicable",
                "non-JNI module acquired a JNI version claim")
        return
    if profile_id == "tasm2_127":
        require(acceptance == "exact" and exact_value == 65540 and
                exact_name == "JNI_VERSION_1_4" and
                exact_availability == "proven" and
                classification == "source_proven",
                "TASM2 exact JNI 1.4 contract changed")
    elif profile_id == "kotor":
        require(module["id"] == "kotor-fmod" and
                acceptance == "positive_return" and exact_value is None and
                exact_name is None and exact_availability == "not_available" and
                classification == "source_proven",
                "KOTOR FMOD positive-version contract changed")
    else:
        require(acceptance == "not_validated" and exact_value is None and
                exact_name is None and exact_availability == "not_available" and
                classification == "proven_limitation",
                "fail-open JNI version was promoted")


def validate_modules(profile, inventory_port, allowed_refs):
    inventory_modules = inventory_module_map(inventory_port)
    modules = profile.get("modules")
    require(isinstance(modules, list) and modules, "profile modules missing")
    require([module.get("id") for module in modules] ==
            list(inventory_modules), "profile module IDs/order differ from inventory")
    prefix = EXPECTED_EVIDENCE_PREFIX[profile["id"]]
    for module in modules:
        require(isinstance(module, dict) and set(module) == MODULE_KEYS,
                "missing or unexpected module field")
        source = inventory_modules[module["id"]]
        require(module.get("sha256") == source.get("sha256"),
                "module hash differs from inventory")
        require(module.get("inventory_role") == source.get("role"),
                "module role differs from inventory")
        applicability = expected_applicability(source["role"])
        require(module.get("applicability") == applicability,
                "module applicability differs from inventory role")
        expected_initialized = (
            "not_applicable" if applicability == "not_applicable" else
            "conditional" if applicability == "conditional" else "required")
        require(module.get("module_initialized") == expected_initialized,
                "module initializer applicability is wrong")
        onload = source["elf"]["jni_exports"]["jni_onload_present"]
        require(module.get("jni_onload_export") is onload,
                "JNI_OnLoad presence differs from inventory")
        expected_jni = (
            "required" if onload and applicability != "not_applicable"
            else "not_applicable")
        require(module.get("module_jni") == expected_jni,
                "MODULE_JNI applicability is wrong")
        validate_jni_version(profile["id"], module, source, allowed_refs,
                             prefix)
    return {module["id"]: module for module in modules}


def validate_object_boundary(profile, allowed_refs):
    boundary = profile.get("vm_jni_activity_boundary")
    require(isinstance(boundary, dict) and
            set(boundary) == {"java_vm", "jni_env", "activity"},
            "invalid VM/JNI/Activity boundary")
    prefix = EXPECTED_EVIDENCE_PREFIX[profile["id"]]
    for name, record in boundary.items():
        require(isinstance(record, dict) and set(record) == OBJECT_RECORD_KEYS,
                "missing or unexpected object boundary field")
        require(record.get("owner") == "adapter",
                "nxandroid core cannot own %s" % name)
        require(record.get("availability") in
                {"proven", "not_available", "not_applicable"},
                "invalid object availability")
        require(record.get("classification") in
                {"source_proven", "proven_limitation",
                 "delegated_not_visible"}, "invalid object classification")
        require(isinstance(record.get("semantics"), str) and
                0 < len(record["semantics"]) <= 1024,
                "invalid object semantics")
        validate_evidence_refs(record, allowed_refs, prefix)
    require(boundary["java_vm"]["availability"] == "proven" and
            boundary["jni_env"]["availability"] == "proven",
            "VM/JNIEnv boundary must be explicit")
    if profile["id"] == "sonic4ep2":
        require(boundary["activity"]["availability"] == "not_available" and
                boundary["activity"]["classification"] == "proven_limitation",
                "Sonic Activity gap was hidden")
    else:
        require(boundary["activity"]["availability"] == "proven",
                "proven Activity boundary missing")


def validate_phase_record(profile, step, module_map, allowed_refs,
                          contracts, orders):
    require(isinstance(step, dict) and set(step) == PHASE_RECORD_KEYS,
            "missing or unexpected phase record field")
    order = step.get("order")
    require(isinstance(order, int) and not isinstance(order, bool) and order > 0,
            "invalid phase order")
    require(not orders or order > orders[-1], "phase order is not increasing")
    orders.append(order)
    phase = step.get("phase")
    require(phase in EXPECTED_PHASES, "unknown or private phase")
    contract_id = step.get("contract_id")
    require(isinstance(contract_id, str) and CONTRACT_ID.fullmatch(contract_id),
            "invalid phase contract ID")
    require(contract_id not in contracts, "duplicate phase contract ID")
    contracts.add(contract_id)
    require(step.get("availability") == "proven",
            "unavailable boundary entered proven projection")
    require(step.get("classification") in
            {"source_proven", "common_boundary_adapter_implementation"},
            "invalid proven phase classification")
    require(isinstance(step.get("semantics"), str) and
            0 < len(step["semantics"]) <= 1024,
            "invalid phase semantics")
    require(step.get("rollback_contract_id") is None and
            step.get("rollback_group") == 0 and
            step.get("closes_rollback_group") == 0,
            "unproved rollback was declared")
    condition = step.get("condition")
    require(condition == "always" or
            (profile["id"] == "kotor" and condition == "kotor-with-hidapi"),
            "unknown phase condition")
    module_id = step.get("module_id")
    cycle_id = step.get("cycle_id")
    require(isinstance(cycle_id, int) and not isinstance(cycle_id, bool),
            "invalid cycle ID")
    if phase in MODULE_PHASES:
        require(module_id in module_map and cycle_id == 0,
                "invalid module phase target")
        if phase == "NXANDROID_PHASE_MODULE_JNI":
            require(module_map[module_id]["module_jni"] == "required",
                    "MODULE_JNI declared for an inapplicable module")
    else:
        require(module_id is None, "non-module phase names a module")
        if phase in CYCLE_PHASES:
            require(cycle_id > 0, "cycle phase needs a non-zero epoch")
        else:
            require(cycle_id == 0, "non-cycle phase has an epoch")
    terminal_policy = step.get("terminal_policy")
    require(terminal_policy in EXPECTED_TERMINAL_POLICIES,
            "unknown terminal policy")
    if phase == "NXANDROID_PHASE_TERMINAL":
        require(terminal_policy in
                {"NXANDROID_TERMINAL_RETURN", "NXANDROID_TERMINAL_ADAPTER"},
                "TERMINAL lacks an allowed terminal policy")
    else:
        require(terminal_policy == "NXANDROID_TERMINAL_NONE",
                "non-terminal phase carries a terminal policy")
    validate_evidence_refs(step, allowed_refs,
                           EXPECTED_EVIDENCE_PREFIX[profile["id"]])


def validate_gaps_and_terminal(profile, allowed_refs, proven_phases):
    profile_id = profile["id"]
    prefix = EXPECTED_EVIDENCE_PREFIX[profile_id]
    gaps = profile.get("gaps")
    require(isinstance(gaps, list), "missing gap list")
    gap_phases = []
    for gap in gaps:
        require(isinstance(gap, dict) and set(gap) == GAP_KEYS,
                "missing or unexpected gap field")
        require(gap.get("availability") == "not_available" and
                gap.get("classification") == "proven_limitation",
                "gap was not classified as proven_limitation/not_available")
        phases = gap.get("phases")
        require(isinstance(phases, list) and phases,
                "empty phase gap")
        for phase in phases:
            require(phase in EXPECTED_PHASES, "gap uses a private phase")
            require(phase not in proven_phases,
                    "phase is both proven and unavailable")
            require(phase not in gap_phases, "duplicate phase gap")
            gap_phases.append(phase)
        require(isinstance(gap.get("semantics"), str) and gap["semantics"],
                "gap semantics missing")
        validate_evidence_refs(gap, allowed_refs, prefix)

    terminal = profile.get("terminal_contract")
    require(isinstance(terminal, dict) and set(terminal) == TERMINAL_KEYS,
            "terminal contract missing or has unexpected fields")
    require(terminal.get("policy") in EXPECTED_TERMINAL_POLICIES,
            "terminal policy is not allowlisted")
    require(CONTRACT_ID.fullmatch(terminal.get("contract_id", "")) is not None,
            "invalid terminal contract ID")
    require(terminal.get("required_flag_if_completed") in EXPECTED_FLAGS,
            "terminal/delegated flag is not allowlisted")
    validate_evidence_refs(terminal, allowed_refs, prefix)

    if profile_id == "kotor":
        require(gaps == [], "delegated KOTOR acquired fictitious gaps")
        delegated = profile.get("delegated_boundaries")
        require(isinstance(delegated, dict) and set(delegated) == DELEGATED_KEYS and
                delegated.get("availability") == "not_applicable" and
                delegated.get("classification") == "delegated_not_visible",
                "KOTOR delegated boundaries are not explicit")
        require(isinstance(delegated.get("phases"), list) and
                "NXANDROID_PHASE_TERMINAL" in delegated["phases"] and
                "NXANDROID_PHASE_RUN_LOOP" in delegated["phases"],
                "KOTOR delegated phase set is incomplete")
        require(not set(delegated["phases"]).intersection(proven_phases),
                "KOTOR fabricates a delegated phase")
        validate_evidence_refs(delegated, allowed_refs, prefix)
        require(terminal.get("policy") == "NXANDROID_TERMINAL_NONE" and
                terminal.get("availability") == "not_applicable" and
                terminal.get("classification") == "delegated_not_visible" and
                terminal.get("required_flag_if_completed") ==
                "NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME",
                "KOTOR must end in delegated runtime, not TERMINAL")
    else:
        require("delegated_boundaries" not in profile,
                "non-KOTOR profile uses delegated boundaries")
        require(profile_id in {"bully2", "sonic4ep2", "horizonchase",
                               "tasm2_127"}, "unexpected incomplete profile")
        require(not profile["execution"]["core_executable"],
                "incomplete profile was marked executable")
        require("NXANDROID_PHASE_TERMINAL" in gap_phases and
                "NXANDROID_PHASE_NATIVE_SHUTDOWN" in gap_phases,
                "real close gaps are not explicit")
        require(terminal.get("policy") == "NXANDROID_TERMINAL_ADAPTER" and
                terminal.get("availability") == "not_available" and
                terminal.get("classification") == "proven_limitation" and
                terminal.get("required_flag_if_completed") ==
                "NXANDROID_PROFILE_ALLOW_ADAPTER_TERMINAL",
                "incomplete terminal was promoted")


def module_ready(module_id, initialized, jni_done, module_map):
    return module_id in initialized and (
        module_map[module_id]["module_jni"] != "required" or
        module_id in jni_done)


def validate_delegated_variant(profile, variant, projection, module_map):
    require(isinstance(variant, dict) and set(variant) == VARIANT_KEYS,
            "missing or unexpected variant field")
    included = variant.get("included_modules")
    enabled = variant.get("enabled_conditions")
    require(isinstance(included, list) and included and
            len(included) == len(set(included)), "invalid variant module set")
    require(isinstance(enabled, list) and len(enabled) == len(set(enabled)),
            "invalid variant conditions")
    for module_id in included:
        require(module_id in module_map and
                module_map[module_id]["applicability"] != "not_applicable",
                "variant includes an inapplicable module")
    steps = [step for step in projection
             if step["condition"] == "always" or
             step["condition"] in enabled]
    require(steps and steps[-1]["phase"] ==
            "NXANDROID_PHASE_RUNTIME_DELEGATED",
            "delegated runtime is not the final callback")
    require(sum(step["phase"] == "NXANDROID_PHASE_RUNTIME_DELEGATED"
                for step in steps) == 1,
            "delegated runtime count is not one")
    forbidden = {
        "NXANDROID_PHASE_GRAPHICS_REQUEST",
        "NXANDROID_PHASE_SURFACE_UP",
        "NXANDROID_PHASE_SURFACE_CHANGED",
        "NXANDROID_PHASE_GL_READY",
        "NXANDROID_PHASE_FOCUS_GAIN",
        "NXANDROID_PHASE_ENTRY",
        "NXANDROID_PHASE_OBJECTS_READY",
        "NXANDROID_PHASE_INPUT_ENABLE",
        "NXANDROID_PHASE_RUN_LOOP",
        "NXANDROID_PHASE_INPUT_DISABLE",
        "NXANDROID_PHASE_FOCUS_LOSS",
        "NXANDROID_PHASE_PAUSE",
        "NXANDROID_PHASE_SURFACE_DOWN",
        "NXANDROID_PHASE_SAVE",
        "NXANDROID_PHASE_NATIVE_SHUTDOWN",
        "NXANDROID_PHASE_TERMINAL",
    }
    require(not forbidden.intersection(step["phase"] for step in steps),
            "KOTOR delegated profile fabricates hidden lifecycle")
    initialized = []
    jni_done = set()
    activity = False
    resumed = False
    for index, step in enumerate(steps):
        phase = step["phase"]
        module_id = step["module_id"]
        if phase == "NXANDROID_PHASE_MODULE_INITIALIZED":
            require(not activity and module_id in included and
                    module_id not in initialized,
                    "invalid delegated module initialization")
            initialized.append(module_id)
        elif phase == "NXANDROID_PHASE_MODULE_JNI":
            require(not activity and module_id in initialized and
                    module_id not in jni_done and
                    module_map[module_id]["module_jni"] == "required",
                    "invalid delegated module JNI phase")
            jni_done.add(module_id)
        elif phase == "NXANDROID_PHASE_ACTIVITY_CREATE":
            require(not activity and initialized == included and
                    all(module_ready(item, initialized, jni_done, module_map)
                        for item in included),
                    "Activity precedes a ready selected module")
            activity = True
        elif phase == "NXANDROID_PHASE_RESUME":
            require(activity and not resumed, "invalid delegated resume")
            resumed = True
        elif phase == "NXANDROID_PHASE_RUNTIME_DELEGATED":
            require(activity and index + 1 == len(steps),
                    "invalid delegated runtime position")
        else:
            raise GateError("unexpected delegated phase")
    require(activity and resumed and initialized == included,
            "delegated profile did not reach its owner")


def validate_execution(profile, projection, module_map):
    execution = profile.get("execution")
    require(isinstance(execution, dict) and set(execution) == EXECUTION_KEYS,
            "execution contract missing or has unexpected fields")
    require(isinstance(execution.get("reason"), str) and execution["reason"],
            "execution rationale missing")
    flags = execution.get("flags")
    variants = execution.get("variants")
    require(isinstance(flags, list) and len(flags) == len(set(flags)) and
            set(flags).issubset(EXPECTED_FLAGS), "invalid execution flags")
    require(isinstance(variants, list), "invalid execution variants")
    require(flags == EXPECTED_PROFILE_FLAGS[profile["id"]],
            "profile acquired a flag not proven for its adapter")
    if profile["id"] != "kotor":
        require(execution.get("core_executable") is False and
                variants == [] and
                execution.get("validation_scope") ==
                "source_phase_projection_only",
                "incomplete profile acquired executable state")
        if profile["id"] == "sonic4ep2":
            phases = [step["phase"] for step in projection]
            require(all(phases.count(phase) == 1
                        for phase in EXPECTED_SONIC_PRE_SURFACE_PHASES),
                    "Sonic pre-Surface phase set changed")
            start = phases.index("NXANDROID_PHASE_GL_READY")
            require(tuple(phases[
                        start:start + len(EXPECTED_SONIC_PRE_SURFACE_PHASES)]) ==
                    EXPECTED_SONIC_PRE_SURFACE_PHASES,
                    "Sonic source-proven GL/entry/objects/Surface order changed")
            source_steps = projection[
                start:start + len(EXPECTED_SONIC_PRE_SURFACE_PHASES)]
            require(all(step["cycle_id"] == 1 and
                        step["availability"] == "proven" and
                        step["classification"] == "source_proven"
                        for step in source_steps),
                    "Sonic pre-Surface relation is not source-proven cycle 1")
        return 0

    require(execution.get("core_executable") is True and
            execution.get("validation_scope") ==
            "structural-source-derived-delegated-only" and
            flags == ["NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME"],
            "KOTOR delegated opt-in changed")
    require([item.get("id") for item in variants] ==
            ["kotor-without-hidapi", "kotor-with-hidapi"],
            "KOTOR variant set changed")
    expected_modules = {
        "kotor-without-hidapi": [
            "kotor-lzma", "kotor-miniz", "kotor-freetype", "kotor-fmod",
            "kotor-android-port", "kotor-main"],
        "kotor-with-hidapi": [
            "kotor-lzma", "kotor-miniz", "kotor-freetype", "kotor-fmod",
            "kotor-hidapi", "kotor-android-port", "kotor-main"],
    }
    expected_conditions = {
        "kotor-without-hidapi": [],
        "kotor-with-hidapi": ["kotor-with-hidapi"],
    }
    for variant in variants:
        variant_id = variant["id"]
        require(variant.get("included_modules") == expected_modules[variant_id],
                "KOTOR variant module order changed")
        require(variant.get("enabled_conditions") ==
                expected_conditions[variant_id],
                "KOTOR variant condition changed")
        validate_delegated_variant(profile, variant, projection, module_map)
    return len(variants)


def validate_profile(profile, inventory_port, allowed_refs):
    profile_id = profile.get("id")
    require(profile_id in EXPECTED_PROFILE_IDS, "unknown profile ID")
    expected_keys = set(BASE_PROFILE_KEYS)
    if profile_id == "kotor":
        expected_keys.add("delegated_boundaries")
    require(isinstance(profile, dict) and set(profile) == expected_keys,
            "missing or unexpected profile field")
    require(profile.get("inventory_port_id") == profile_id ==
            inventory_port.get("id"), "profile/inventory ID mismatch")
    require(profile.get("abi") == EXPECTED_ABIS[profile_id],
            "profile ABI changed")
    require(isinstance(profile.get("adapter_family"), str) and
            profile["adapter_family"], "adapter family missing")
    module_map = validate_modules(profile, inventory_port, allowed_refs)
    validate_object_boundary(profile, allowed_refs)

    trace = profile.get("android_runtime_trace")
    require(isinstance(trace, dict) and set(trace) == {
                "availability", "classification", "semantics"} and
            trace.get("availability") == "not_available" and
            trace.get("classification") == "proven_limitation" and
            isinstance(trace.get("semantics"), str) and trace["semantics"],
            "Android runtime trace absence was hidden")

    graphics = profile.get("graphics_surface_order")
    require(isinstance(graphics, dict) and set(graphics) == GRAPHICS_KEYS and
            graphics.get("relation") == EXPECTED_GRAPHICS_RELATION[profile_id],
            "graphics/surface order differs from source")
    if profile_id == "kotor":
        require(graphics.get("availability") == "not_applicable" and
                graphics.get("classification") == "delegated_not_visible",
                "KOTOR graphics internals were exposed")
    else:
        require(graphics.get("availability") == "proven" and
                graphics.get("classification") == "source_proven",
                "source-proven graphics relation missing")
    validate_evidence_refs(graphics, allowed_refs,
                           EXPECTED_EVIDENCE_PREFIX[profile_id])

    projection = profile.get("proven_phase_projection")
    require(isinstance(projection, list) and projection,
            "phase projection missing")
    contracts = set()
    orders = []
    for step in projection:
        validate_phase_record(profile, step, module_map, allowed_refs,
                              contracts, orders)
    phases = {step["phase"] for step in projection}
    if profile_id == "kotor":
        require(projection[-1]["phase"] ==
                "NXANDROID_PHASE_RUNTIME_DELEGATED",
                "KOTOR delegated owner is not final")
    else:
        require("NXANDROID_PHASE_RUNTIME_DELEGATED" not in phases,
                "delegated runtime leaked into a driven adapter")
    variants = validate_execution(profile, projection, module_map)
    validate_gaps_and_terminal(profile, allowed_refs, phases)
    require(isinstance(profile.get("rollback_policy"), str) and
            profile["rollback_policy"], "rollback policy missing")
    return variants


def validate_document(document, raw_text, inventory, header_text):
    expected_top = {
        "schema", "schema_version", "milestone", "scope", "method",
        "source_pins", "safety", "public_contract",
        "classification_vocabulary", "profiles",
    }
    require(isinstance(document, dict) and set(document) == expected_top,
            "unexpected top-level profile schema")
    require(document.get("schema") == "nxandroid-adapter-profiles-v1" and
            document.get("schema_version") == 1 and
            document.get("milestone") == "M11",
            "profile schema identity mismatch")
    require(document.get("safety") == EXPECTED_SAFETY,
            "safety declaration changed")
    require(inventory.get("safety") == EXPECTED_INVENTORY_SAFETY,
            "inventory static-only safety contract changed")
    validate_privacy_and_prohibitions(document, raw_text)
    validate_public_contract(document, header_text)

    require(document.get("classification_vocabulary") == EXPECTED_VOCABULARY,
            "classification vocabulary changed")

    inventory_ports = inventory.get("ports")
    profiles = document.get("profiles")
    require(isinstance(inventory_ports, list) and
            [port.get("id") for port in inventory_ports] ==
            list(EXPECTED_PROFILE_IDS), "inventory port identity changed")
    require(isinstance(profiles, list) and
            [profile.get("id") for profile in profiles] ==
            list(EXPECTED_PROFILE_IDS), "profile identity/order changed")
    allowed_refs = collect_inventory_evidence(inventory)
    require(len(allowed_refs) >= 60, "inventory evidence set is unexpectedly small")
    variant_count = 0
    for profile, inventory_port in zip(profiles, inventory_ports):
        variant_count += validate_profile(profile, inventory_port, allowed_refs)
    require(variant_count == 2, "expected exactly two KOTOR module variants")
    return variant_count


def validate_process_free_source():
    require(not SELF_PATH.is_symlink(), "validator must not be a symlink")
    source = SELF_PATH.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=SELF_PATH.name)
    forbidden_modules = {
        "asyncio", "http", "multiprocessing", "os", "requests", "socket",
        "subprocess", "urllib",
    }
    forbidden_calls = {
        "system", "popen", "fork", "forkpty", "spawnl", "spawnle",
        "spawnlp", "spawnlpe", "spawnv", "spawnve", "spawnvp", "spawnvpe",
        "execv", "execve", "execvp", "execvpe",
    }
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            require(not any(alias.name.split(".")[0] in forbidden_modules
                            for alias in node.names),
                    "process/network module imported")
        elif isinstance(node, ast.ImportFrom):
            require((node.module or "").split(".")[0] not in forbidden_modules,
                    "process/network module imported")
        elif isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
            require(node.func.attr not in forbidden_calls,
                    "process execution call present")
        elif isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
            require(node.func.id not in forbidden_calls | {"__import__"},
                    "process execution call present")


def expect_rejected(label, document, inventory, header_text, mutation):
    candidate = copy.deepcopy(document)
    mutation(candidate)
    raw = json.dumps(candidate, sort_keys=True)
    try:
        validate_document(candidate, raw, inventory, header_text)
    except GateError:
        return
    raise GateError("negative self-test was accepted: %s" % label)


def add_kotor_fake_surface(document):
    profile = document["profiles"][3]
    step = copy.deepcopy(profile["proven_phase_projection"][-1])
    step.update({
        "order": 95,
        "phase": "NXANDROID_PHASE_SURFACE_UP",
        "cycle_id": 1,
        "contract_id": "kotor-fabricated-surface-v1",
        "semantics": "Negative fixture only.",
        "evidence_refs": ["ports/kotor/src/main.c:342-358"],
    })
    profile["proven_phase_projection"].insert(-1, step)


def swap_sonic_entry_and_surface(document):
    projection = document["profiles"][1]["proven_phase_projection"]
    entry = next(index for index, step in enumerate(projection)
                 if step["phase"] == "NXANDROID_PHASE_ENTRY")
    surface = next(index for index, step in enumerate(projection)
                   if step["phase"] == "NXANDROID_PHASE_SURFACE_UP")
    projection[entry]["phase"], projection[surface]["phase"] = (
        projection[surface]["phase"], projection[entry]["phase"])


def add_pre_surface_flag(document, profile_index):
    document["profiles"][profile_index]["execution"]["flags"].append(
        "NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK")


def run_negative_self_tests(document, inventory, header_text):
    tests = [
        ("private-path", lambda value: value["profiles"][0].update(
            {"rollback_policy": "/home/user/private"})),
        ("ip", lambda value: value["profiles"][0].update(
            {"rollback_policy": "connect 192.168.1.5"})),
        ("legacy-tasm2", lambda value: value["profiles"][4].update(
            {"rollback_policy": "ports/tasm2 is forbidden"})),
        ("code-address", lambda value: value["profiles"][0].update(
            {"rollback_policy": "guest+0x1234"})),
        ("module-hash", lambda value: value["profiles"][0]["modules"][0].update(
            {"sha256": "0" * 64})),
        ("foreign-lines", lambda value: value["profiles"][0][
            "graphics_surface_order"].update(
                {"evidence_refs": ["ports/sonic4/src/main.c:1714-1724"]})),
        ("private-phase", lambda value: value["profiles"][0][
            "proven_phase_projection"][0].update(
                {"phase": "NXANDROID_PHASE_GAME_SPECIFIC"})),
        ("core-owns-vm", lambda value: value["public_contract"][
            "core_owns"].update({"java_vm": True})),
        ("promote-incomplete", lambda value: value["profiles"][2][
            "execution"].update({"core_executable": True})),
        ("sonic-missing-pre-surface-flag", lambda value: value["profiles"][1][
            "execution"].update({"flags": []})),
        ("sonic-pre-surface-order", swap_sonic_entry_and_surface),
        ("bully-pre-surface-flag", lambda value: add_pre_surface_flag(value, 0)),
        ("horizon-pre-surface-flag", lambda value: add_pre_surface_flag(value, 2)),
        ("kotor-pre-surface-flag", lambda value: add_pre_surface_flag(value, 3)),
        ("tasm2-pre-surface-flag", lambda value: add_pre_surface_flag(value, 4)),
        ("kotor-fake-surface", add_kotor_fake_surface),
        ("kotor-terminal", lambda value: value["profiles"][3][
            "terminal_contract"].update(
                {"policy": "NXANDROID_TERMINAL_ADAPTER"})),
    ]
    for label, mutation in tests:
        expect_rejected(label, document, inventory, header_text, mutation)
    try:
        load_json_bytes(b'{"duplicate":1,"duplicate":2}')
    except GateError:
        pass
    else:
        raise GateError("duplicate JSON key self-test was accepted")
    return len(tests) + 1


def main():
    validate_process_free_source()
    require(not (REPOSITORY / PROFILE_RELATIVE).is_symlink(),
            "profile JSON must not be a symlink")
    raw = read_bytes(PROFILE_RELATIVE.as_posix())
    document = load_json_bytes(raw)
    inventory, header_text = validate_pins(document)
    variant_count = validate_document(document, raw.decode("utf-8"), inventory,
                                      header_text)
    negative_count = run_negative_self_tests(document, inventory, header_text)
    print("M11 adapter profile gate passed: profiles=5 "
          "core_executable=1 delegated_variants=%d negative_tests=%d "
          "subprocesses=0 guest_files_opened=0 guest_code_executed=0 "
          "initializers_executed=0 jni_onload_executed=0 "
          "device_access=0 network_access=0" %
          (variant_count, negative_count))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
