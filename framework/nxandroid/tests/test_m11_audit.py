#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free completeness and honesty gate for the M11 master ledger."""

import ast
import copy
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import shlex


SELF_PATH = Path(__file__)
REPOSITORY = SELF_PATH.resolve().parents[3]
AUDIT_RELATIVE = "framework/nxandroid/m11-audit-v1.json"
AUDIT_PATH = REPOSITORY / AUDIT_RELATIVE
MAX_TEXT_BYTES = 16 * 1024 * 1024

REQUIREMENTS = (
    ("M11-001", "registrar imports Bionic/JNI explicitamente", "verified"),
    ("M11-002", "implementar stubs somente quando contrato é conhecido",
     "verified"),
    ("M11-003", "rejeitar import crítico desconhecido", "verified"),
    ("M11-004", "executar DT_INIT antes de init_array na ordem ELF",
     "verified_synthetic_only"),
    ("M11-005", "chamar JNI_OnLoad no ponto nativo correto",
     "verified_synthetic_only"),
    ("M11-006", "construir VM/env/Activity compatíveis por adapter",
     "verified_adapter_acceptance"),
    ("M11-007", "reproduzir criação de Surface/window na ordem comprovada",
     "verified_static_only"),
    ("M11-008", "reproduzir resume/focus sem pular callbacks",
     "verified_static_only"),
    ("M11-009", "não forçar GL antes da engine pedir contexto", "verified"),
    ("M11-010", "não chamar entry point fora de ordem", "verified"),
    ("M11-011", "encaminhar input somente depois de objetos prontos",
     "verified"),
    ("M11-012", "reproduzir pause antes de save/shutdown", "verified"),
    ("M11-013", "persistir save/config pelo caminho da engine",
     "verified_adapter_acceptance"),
    ("M11-014", "executar shutdown nativo comprovado",
     "verified_adapter_acceptance"),
    ("M11-015",
     "usar saída terminal estreita só quando destruidores guest são inseguros e provado",
     "verified_adapter_acceptance"),
    ("M11-016", "testar lifecycle normal, erro inicial e sinal externo",
     "verified_synthetic_only"),
    ("M11-017",
     "bloquear reentrância de callbacks e testar múltiplas entradas/saídas sem estado estático vazando",
     "verified"),
    ("M11-018", "documentar sequência por engine", "verified_static_only"),
    ("M11-019",
     "comparar com traces/referências Android reais quando disponíveis",
     "limitation_recorded"),
    ("M11-020", "fechar gate de lifecycle", "verified"),
)

PIN_ROLES = {
    "framework/nxandroid/CMakeLists.txt": "build-contract",
    "framework/nxandroid/VERSION": "version",
    "framework/nxandroid/README.md": "public-documentation",
    "framework/nxandroid/CONTRACT.md": "adapter-primitives-contract",
    "framework/nxandroid/CHANGELOG.md": "component-changelog",
    "framework/nxandroid/REGRESSION-MATRIX.md": "regression-matrix",
    "framework/nxandroid/include/nxandroid.h": "public-contract",
    "framework/nxandroid/include/nxandroid_jni_adapter.h":
        "adapter-primitives-public-contract",
    "framework/nxandroid/src/nxandroid.c": "lifecycle-core",
    "framework/nxandroid/src/nxandroid_imports.c": "import-policy",
    "framework/nxandroid/src/nxandroid_jni_adapter.c":
        "adapter-primitives-implementation",
    "framework/nxandroid/tests/test_nxandroid.c": "host-contract-tests",
    "framework/nxandroid/tests/run-host.sh": "host-gate",
    "framework/nxandroid/tests/test_signal.c": "signal-fixture",
    "framework/nxandroid/tests/run-signal-isolated.sh": "signal-gate",
    "framework/nxbootstrap/tests/isolated-suite.sh":
        "canonical-isolated-suite",
    "framework/nxandroid/references/m11-android-guests-v1.json":
        "static-guest-inventory",
    "framework/nxandroid/references/REFERENCE-AUDIT.md": "reference-audit",
    "framework/nxandroid/tools/inventory_m11_guests.py":
        "inventory-validator",
    "framework/nxandroid/references/m11-adapter-profiles-v1.json":
        "adapter-profiles",
    "framework/nxandroid/tests/test_m11_adapter_profiles.py":
        "adapter-profile-gate",
    "framework/nxandroid/references/m16-adapter-contract-v1.json":
        "adapter-acceptance-contract",
    "framework/nxandroid/references/m16-runtime-receipt-v1.json":
        "adapter-runtime-acceptance",
    "framework/nxandroid/tests/test_m16_adapter_contract.py":
        "adapter-acceptance-gate",
    "framework/nxandroid/references/jni-adapter-primitives-v1.json":
        "adapter-primitives-evidence",
    "framework/nxandroid/tests/test_020_adapter_contract.py":
        "adapter-primitives-gate",
    "framework/nxloader/include/nxloader.h": "loader-public-contract",
    "framework/nxloader/VERSION": "loader-version",
    "framework/nxloader/src/nxloader.c": "loader-lifecycle",
    "framework/nxloader/src/nxloader_elf32.c":
        "loader-armv7-initializers",
    "framework/nxloader/src/nxloader_elf64.c":
        "loader-aarch64-initializers",
    "framework/nxloader/tests/test_m11_lifecycle_cross.c":
        "synthetic-lifecycle-fixture",
    "framework/nxloader/tests/run-m11-armv7-lifecycle-cross.sh":
        "synthetic-armv7-gate",
    "framework/nxloader/tests/run-m11-aarch64-lifecycle-cross.sh":
        "synthetic-aarch64-gate",
    "framework/nxloader/m11-audit-v1.json": "loader-subledger",
    "framework/nxloader/tests/test_m11_audit.py": "loader-subgate",
    "framework/nxloader/tests/m10-aarch64-guests-evidence-v1.json":
        "prior-m10-guest-relocation-evidence",
    "framework/contracts/declarative-v1.json": "framework-contract",
    "framework/tests/test-matrix-v1.json": "automatic-gate-matrix",
    "framework/tests/run-safe-gates.sh": "canonical-safe-runner",
    "framework/tests/test_infrastructure.py": "infrastructure-policy-gate",
}

EXPECTED_CLOSURE = {
    "requirements_accounted": 20,
    "completed_requirement_count": 20,
    "deferred_requirement_count": 0,
    "master_milestone_complete": True,
    "scoped_core_gate_closed": True,
    "deferred_requirement_ids": [],
    "deferred_completion_dependencies": [],
    "approved_adapter_runtime_acceptance_validated": True,
    "universal_adapter_runtime_validated": False,
    "physical_device_evidence": False,
    "real_android_runtime_trace_count": 0,
    "approved_profile_count": 5,
    "core_executable_profile_count": 1,
    "incomplete_profile_count": 4,
    "delegated_profile": "kotor",
}

EXPECTED_EXECUTION = {
    "host_mock_contexts_completed": 2000,
    "test_owned_synthetic_elf_loaded": True,
    "synthetic_elf_initializers_executed": True,
    "synthetic_elf_jni_onload_executed": True,
    "external_guest_elf_loaded_in_m11_lifecycle_runs": False,
    "external_guest_code_executed": False,
    "external_guest_initializers_executed": False,
    "external_guest_jni_onload_executed": False,
    "external_guest_lifecycle_executed": False,
    "prior_m10_external_guest_elfs_loaded_and_relocated": 5,
    "prior_m10_guest_initializers_executed": False,
    "prior_m10_guest_jni_onload_executed": False,
    "imported_m16_approved_adapter_acceptance": True,
    "m16_manual_gameplay_revalidation_claimed": False,
    "signal_execution": "canonical_isolated_suite_only",
    "device_access": False,
    "network_access": False,
    "hardware_ran": False,
}

EXPECTED_SUBGATES = (
    ("nxloader-lifecycle-boundary", "M11-NXL-001..010"),
    ("nxandroid-adapter-profiles", "profile-source-projections"),
    ("nxandroid-host-contract", "host-mock-contract"),
    ("nxandroid-isolated-signal", "sealed-signal-contract"),
    ("nxandroid-m16-adapter-acceptance", "M16-001..020"),
)

EXPECTED_PROFILE_IDS = (
    "bully2", "sonic4ep2", "horizonchase", "kotor", "tasm2_127")
INCOMPLETE_PROFILE_IDS = {
    "bully2", "sonic4ep2", "horizonchase", "tasm2_127"}
DEFERRED_REQUIREMENT_IDS = set()
RUN_ID = re.compile(r"[0-9]{8}T[0-9]{6}Z-pid[0-9]+-[0-9]+\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
IPV4 = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")


class GateError(Exception):
    """A deterministic audit failure."""


def require(condition, message):
    if not condition:
        raise GateError(message)


def reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: %s" % key)
        result[key] = value
    return result


def canonical_relative_path(value):
    require(isinstance(value, str) and value, "empty evidence path")
    pure = PurePosixPath(value)
    require(not pure.is_absolute() and ".." not in pure.parts,
            "evidence path escapes repository: %s" % value)
    require(str(pure) == value, "evidence path is not canonical: %s" % value)
    return pure


def repository_file(relative):
    pure = canonical_relative_path(relative)
    path = REPOSITORY / pure
    require(not path.is_symlink(), "evidence is a symlink: %s" % relative)
    require(path.is_file(), "evidence file is missing: %s" % relative)
    size = path.stat().st_size
    require(0 < size <= MAX_TEXT_BYTES,
            "evidence file size is invalid: %s" % relative)
    return path


def read_bytes(relative):
    return repository_file(relative).read_bytes()


def read_text(relative):
    raw = read_bytes(relative)
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise GateError("non-UTF-8 evidence: %s" % relative) from error


def load_json_bytes(raw, label):
    require(len(raw) <= MAX_TEXT_BYTES, "%s exceeds size bound" % label)
    try:
        return json.loads(raw.decode("utf-8"),
                          object_pairs_hook=reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise GateError("invalid JSON in %s: %s" % (label, error)) from error


def validate_process_free_python(relative):
    source = read_text(relative)
    try:
        tree = ast.parse(source, filename=relative)
    except SyntaxError as error:
        raise GateError("invalid Python source: %s" % relative) from error
    forbidden_modules = {
        "subprocess", "socket", "urllib", "http", "requests", "ftplib",
    }
    forbidden_calls = {
        "system", "popen", "fork", "forkpty", "execv", "execve", "execvp",
        "execvpe", "spawnl", "spawnle", "spawnlp", "spawnlpe", "spawnv",
        "spawnve", "spawnvp", "spawnvpe", "Popen",
    }
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            require(not any(alias.name.split(".")[0] in forbidden_modules
                            for alias in node.names),
                    "process/network import in %s" % relative)
        elif isinstance(node, ast.ImportFrom):
            require((node.module or "").split(".")[0] not in forbidden_modules,
                    "process/network import in %s" % relative)
        elif isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
            require(node.func.attr not in forbidden_calls,
                    "process execution call in %s" % relative)


def validate_privacy(raw_text):
    lowered = raw_text.lower()
    for forbidden in ("/home/", "/mnt/", "/tmp/", "file://", "felipe"):
        require(forbidden not in lowered,
                "ledger contains private/external path token")
    require(IPV4.search(raw_text) is None, "ledger contains an IP address")


def validate_pins(document):
    pins = document.get("evidence_pins")
    require(isinstance(pins, list) and len(pins) == len(PIN_ROLES),
            "evidence pin count changed")
    by_path = {}
    decoded = {}
    for pin in pins:
        require(isinstance(pin, dict) and
                set(pin) == {"path", "sha256", "role"},
                "malformed evidence pin")
        path = pin.get("path")
        require(path in PIN_ROLES and path not in by_path,
                "unexpected or duplicate evidence pin: %s" % path)
        require(pin.get("role") == PIN_ROLES[path],
                "evidence role changed: %s" % path)
        digest = pin.get("sha256")
        require(isinstance(digest, str) and SHA256.fullmatch(digest),
                "invalid SHA-256: %s" % path)
        raw = read_bytes(path)
        require(hashlib.sha256(raw).hexdigest() == digest,
                "evidence hash mismatch: %s" % path)
        try:
            decoded[path] = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise GateError("pinned evidence is not UTF-8: %s" % path) from error
        by_path[path] = pin
    require(set(by_path) == set(PIN_ROLES), "evidence pin set changed")
    require(decoded["framework/nxandroid/VERSION"].strip() == "0.5.0",
            "nxandroid version pin changed")
    # M11 closed against nxloader 0.7.2. What it protects is the loader
    # contract nxandroid consumes, not the patch number, so the pin accepts the
    # closed version or a later one whose VERSION and header agree. A
    # disagreement between the two is still a hard failure.
    loader_version = decoded["framework/nxloader/VERSION"].strip()
    loader_parts = loader_version.split(".")
    require(len(loader_parts) == 3 and
            all(part.isdigit() for part in loader_parts) and
            tuple(int(part) for part in loader_parts) >= (0, 7, 2) and
            '#define NXLOADER_VERSION_STRING "%s"' % loader_version in
            decoded["framework/nxloader/include/nxloader.h"],
            "nxloader version pin changed")
    contract = load_json_bytes(
        read_bytes("framework/contracts/declarative-v1.json"),
        "framework declarative contract")
    components = {item.get("id"): item
                  for item in contract.get("components", [])}
    # Same rule as above: the declared nxloader version must agree with the
    # VERSION file this gate already validated, at or above the closed one.
    require(contract.get("schema_version") == 1 and
            components.get("nxloader", {}).get("current_version") ==
            loader_version and
            components.get("nxloader", {}).get("version_file") ==
            "framework/nxloader/VERSION" and
            components.get("nxandroid", {}).get("current_version") ==
            "0.5.0",
            "framework nxloader/nxandroid contract pin changed")
    return by_path, decoded


def validate_reference(reference, pins, decoded, requirement_id):
    require(isinstance(reference, dict) and
            set(reference) == {"path", "tokens"},
            "%s has malformed evidence" % requirement_id)
    path = reference.get("path")
    require(path in pins, "%s cites unpinned evidence: %s" %
            (requirement_id, path))
    require(path not in {AUDIT_RELATIVE,
                         "framework/nxandroid/tests/test_m11_audit.py"},
            "%s uses circular audit evidence" % requirement_id)
    tokens = reference.get("tokens")
    require(isinstance(tokens, list) and 1 <= len(tokens) <= 8 and
            len(tokens) == len(set(tokens)),
            "%s has invalid evidence token list" % requirement_id)
    for token in tokens:
        require(isinstance(token, str) and 0 < len(token) <= 512,
                "%s has invalid evidence token" % requirement_id)
        require(token in decoded[path],
                "%s token absent from %s: %r" %
                (requirement_id, path, token))


def validate_requirements(document, pins, decoded):
    requirements = document.get("requirements")
    require(isinstance(requirements, list) and
            len(requirements) == len(REQUIREMENTS),
            "requirement count changed")
    expected_keys = {
        "id", "requirement", "disposition", "claim", "implementation",
        "tests", "limitations",
    }
    for item, expected in zip(requirements, REQUIREMENTS):
        expected_id, expected_text, expected_disposition = expected
        require(isinstance(item, dict) and set(item) == expected_keys,
                "%s schema changed" % expected_id)
        require(item.get("id") == expected_id and
                item.get("requirement") == expected_text and
                item.get("disposition") == expected_disposition,
                "%s identity/text/disposition changed" % expected_id)
        claim = item.get("claim")
        require(isinstance(claim, str) and 20 <= len(claim) <= 1024,
                "%s claim is invalid" % expected_id)
        for group in ("implementation", "tests"):
            references = item.get(group)
            require(isinstance(references, list) and references,
                    "%s lacks %s evidence" % (expected_id, group))
            for reference in references:
                validate_reference(reference, pins, decoded, expected_id)
        limitations = item.get("limitations")
        require(isinstance(limitations, list) and
                len(limitations) == len(set(limitations)),
                "%s limitation list is invalid" % expected_id)
        for limitation in limitations:
            require(isinstance(limitation, str) and
                    10 <= len(limitation) <= 1024,
                    "%s limitation is invalid" % expected_id)
        if expected_disposition != "verified":
            require(limitations, "%s hides its scoped limitation" % expected_id)
    require(requirements[18]["disposition"] == "limitation_recorded" and
            "zero" in requirements[18]["limitations"][0].lower(),
            "M11-019 must preserve the zero-trace limitation")
    deferred = document.get("closure", {}).get("deferred_requirement_ids")
    require(isinstance(deferred, list) and
            set(deferred) == DEFERRED_REQUIREMENT_IDS and
            len(deferred) == len(DEFERRED_REQUIREMENT_IDS),
            "deferred master requirement set changed")
    require(requirements[14]["disposition"] ==
            "verified_adapter_acceptance" and
            any("exact-version adapter data" in limitation.lower() and
                "kotor" in limitation.lower()
                for limitation in requirements[14]["limitations"]),
            "M11-015 adapter-specific terminal boundary was hidden")


def validate_subgates(document, pins, decoded):
    subgates = document.get("subgates")
    require(isinstance(subgates, list) and len(subgates) == 5,
            "subgate set changed")
    for subgate, expected in zip(subgates, EXPECTED_SUBGATES):
        require(isinstance(subgate, dict) and set(subgate) == {
            "id", "ledger_path", "validator_path", "requirement_namespace",
            "claim",
        }, "malformed subgate")
        require((subgate.get("id"), subgate.get("requirement_namespace")) ==
                expected, "subgate identity/namespace changed")
        validator = subgate.get("validator_path")
        require(validator in pins, "subgate validator is not pinned")
        ledger = subgate.get("ledger_path")
        require(ledger is None or ledger in pins,
                "subgate ledger is not pinned")
        require(isinstance(subgate.get("claim"), str) and
                20 <= len(subgate["claim"]) <= 512,
                "subgate claim is invalid")

    loader = load_json_bytes(
        read_bytes("framework/nxloader/m11-audit-v1.json"),
        "nxloader M11 subledger")
    loader_ids = [item.get("id") for item in loader.get("requirements", [])]
    require(loader_ids == ["M11-NXL-%03d" % number
                           for number in range(1, 11)],
            "nxloader subledger collides with master M11 IDs")
    validate_process_free_python(
        "framework/nxloader/tests/test_m11_audit.py")
    validate_process_free_python(
        "framework/nxandroid/tests/test_m11_adapter_profiles.py")
    validate_process_free_python(
        "framework/nxandroid/tests/test_m16_adapter_contract.py")

    m16 = load_json_bytes(
        read_bytes("framework/nxandroid/references/"
                   "m16-adapter-contract-v1.json"),
        "M16 adapter acceptance contract")
    require(m16.get("schema") == "nxandroid-m16-adapter-contract-v1" and
            m16.get("approved_adapters") == [
                "bully2", "sonic4ep2", "horizonchase", "kotor",
                "asm2_127"],
            "M16 adapter acceptance scope changed")
    m16_items = {item.get("id"): item for item in m16.get("m16_items", [])}
    require(m16_items.get("M16-013", {}).get("status") ==
            "closed_with_approved_acceptance" and
            m16_items.get("M16-014", {}).get("status") ==
            "closed_with_approved_acceptance" and
            m16_items.get("M16-020", {}).get("status") == "closed",
            "M16 lifecycle/persistence/shutdown acceptance is not closed")

    m16_receipt = load_json_bytes(
        read_bytes("framework/nxandroid/references/"
                   "m16-runtime-receipt-v1.json"),
        "M16 runtime acceptance receipt")
    require(m16_receipt.get("schema") ==
            "nxandroid-m16-runtime-receipt-v1" and
            [entry.get("id") for entry in m16_receipt.get("adapters", [])] ==
            ["bully2", "sonic4ep2", "horizonchase", "kotor",
             "asm2_127"] and
            all(entry.get("acceptance") == "accepted_reference"
                for entry in m16_receipt.get("adapters", [])) and
            m16_receipt.get("closure", {}).get("m16_020") ==
            "closed_for_framework",
            "M16 runtime acceptance receipt changed")

    cmake = decoded["framework/nxandroid/CMakeLists.txt"]
    require("add_executable(nxandroid_signal_test tests/test_signal.c)" in
            cmake, "signal target missing")
    require(re.search(r"add_test\s*\([^)]*nxandroid_signal", cmake,
                      re.IGNORECASE | re.DOTALL) is None,
            "signal fixture was registered for direct CTest execution")
    host_runner = decoded["framework/nxandroid/tests/run-host.sh"]
    require("-DNXANDROID_BUILD_SIGNAL_TEST=OFF" in host_runner,
            "host gate enables signal fixture")
    isolated = decoded["framework/nxbootstrap/tests/isolated-suite.sh"]
    require(isolated.count(
        'run-signal-isolated.sh" --inside-suite') == 1,
        "canonical isolated suite signal invocation changed")
    signal_runner = decoded[
        "framework/nxandroid/tests/run-signal-isolated.sh"]
    for token in ("nxbootstrap_require_private_pid_namespace",
                  "--inside-suite", "host_fallback=0",
                  "signal_authority=pidfd"):
        require(token in signal_runner, "signal gate token missing: %s" % token)


def validate_profiles_and_external_scope(document):
    profiles = load_json_bytes(
        read_bytes("framework/nxandroid/references/"
                   "m11-adapter-profiles-v1.json"),
        "M11 adapter profiles")
    profile_list = profiles.get("profiles")
    require(isinstance(profile_list, list) and
            tuple(item.get("id") for item in profile_list) ==
            EXPECTED_PROFILE_IDS, "adapter profile IDs/order changed")
    incomplete = set()
    executable = []
    for profile in profile_list:
        trace = profile.get("android_runtime_trace", {})
        require(trace.get("availability") == "not_available" and
                trace.get("classification") == "proven_limitation",
                "Android trace absence was promoted: %s" % profile.get("id"))
        execution = profile.get("execution", {})
        if execution.get("core_executable") is False:
            incomplete.add(profile.get("id"))
            require(execution.get("variants") == [],
                    "incomplete profile acquired executable variants")
        elif execution.get("core_executable") is True:
            executable.append(profile)
        else:
            raise GateError("profile core_executable is not boolean")
    require(incomplete == INCOMPLETE_PROFILE_IDS,
            "incomplete profile set changed")
    sonic = next(item for item in profile_list
                 if item.get("id") == "sonic4ep2")
    sonic_phases = [step.get("phase")
                    for step in sonic.get("proven_phase_projection", [])]
    sonic_relation = [
        "NXANDROID_PHASE_GRAPHICS_REQUEST", "NXANDROID_PHASE_GL_READY",
        "NXANDROID_PHASE_ENTRY", "NXANDROID_PHASE_OBJECTS_READY",
        "NXANDROID_PHASE_SURFACE_UP",
    ]
    require(sonic.get("execution", {}).get("flags") ==
            ["NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK"] and
            all(phase in sonic_phases for phase in sonic_relation) and
            [sonic_phases.index(phase) for phase in sonic_relation] ==
            sorted(sonic_phases.index(phase) for phase in sonic_relation),
            "Sonic pre-Surface entry relation or opt-in changed")
    require(len(executable) == 1 and executable[0].get("id") == "kotor",
            "KOTOR must be the only executable profile")
    kotor = executable[0]
    require(kotor["execution"].get("flags") ==
            ["NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME"] and
            len(kotor["execution"].get("variants", [])) == 2 and
            kotor.get("proven_phase_projection", [])[-1].get("phase") ==
            "NXANDROID_PHASE_RUNTIME_DELEGATED",
            "KOTOR delegated contract changed")
    require(all(step.get("phase") != "NXANDROID_PHASE_TERMINAL"
                for step in kotor["proven_phase_projection"]),
            "KOTOR acquired a fabricated terminal phase")

    core_owns = profiles.get("public_contract", {}).get("core_owns", {})
    require(core_owns and not any(core_owns.values()),
            "nxandroid profile overclaims adapter object ownership")
    profile_safety = profiles.get("safety", {})
    require(profile_safety and
            all(value is False for key, value in profile_safety.items()
                if key not in {"static_files_only", "validator_process_free"})
            and profile_safety.get("static_files_only") is True and
            profile_safety.get("validator_process_free") is True,
            "adapter profile safety claim changed")

    inventory = load_json_bytes(
        read_bytes("framework/nxandroid/references/"
                   "m11-android-guests-v1.json"),
        "M11 static guest inventory")
    inventory_safety = inventory.get("safety", {})
    require(inventory_safety and all(value is False
                                     for value in inventory_safety.values()),
            "static inventory overclaims guest/device execution")
    require(document["closure"] == EXPECTED_CLOSURE,
            "closure/profile counts changed")


def validate_execution_scope(document, decoded):
    require(document.get("execution_claims") == EXPECTED_EXECUTION,
            "execution claim set changed")
    lifecycle = decoded[
        "framework/nxloader/tests/test_m11_lifecycle_cross.c"]
    for token in ("nxloader_module_call_initializers(module)",
                  "nxloader_module_call_jni_onload(module", "external_guest=0"):
        require(token in lifecycle,
                "synthetic lifecycle proof token missing: %s" % token)
    for path in ("framework/nxloader/tests/"
                 "run-m11-armv7-lifecycle-cross.sh",
                 "framework/nxloader/tests/"
                 "run-m11-aarch64-lifecycle-cross.sh"):
        source = decoded[path]
        require("test_owned_guest=2 external_guest=0" in source and
                "initializers_exactly_once=2" in source and
                "jni_exactly_once=2" in source,
                "cross gate synthetic/external distinction changed")
    imports = decoded["framework/nxandroid/src/nxandroid_imports.c"]
    require("dlsym" not in imports, "generic dlsym fallback entered core")
    lifecycle_core = decoded["framework/nxandroid/src/nxandroid.c"]
    require("_exit(" not in lifecycle_core and "exit(" not in lifecycle_core,
            "nxandroid core acquired process termination")
    header = decoded["framework/nxandroid/include/nxandroid.h"]
    for token in ("keep both callback code and userdata alive",
                  "Destruction order is consumers",
                  "NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK"):
        require(token in header, "public lifetime/order contract missing: %s" %
                token)
    readme = decoded["framework/nxandroid/README.md"]
    for token in ("Callback code and `ops.userdata` are borrowed",
                  "required destruction order is consumers first, catalog",
                  "NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK"):
        require(token in readme,
                "documented lifetime/order contract missing: %s" % token)
    host_test = decoded["framework/nxandroid/tests/test_nxandroid.c"]
    for token in ("kotor_modules_without_hidapi",
                  "kotor_modules_with_hidapi",
                  "ARRAY_SIZE(kotor_modules_without_hidapi) == 6u",
                  "ARRAY_SIZE(kotor_modules_with_hidapi) == 7u",
                  "kotor-fmod-jni-onload-positive-v1",
                  "kotor-hidapi-initialized-v1",
                  "kotor-obb-sdl-main-delegated-runtime-v1",
                  "test_sonic_entry_before_surface_callback"):
        require(token in host_test, "host lifecycle fixture missing: %s" %
                token)

    m10 = load_json_bytes(
        read_bytes("framework/nxloader/tests/"
                   "m10-aarch64-guests-evidence-v1.json"),
        "prior M10 guest relocation evidence")
    guests = m10.get("guests")
    safety = m10.get("safety", {})
    require(m10.get("milestone") == "M10" and
            m10.get("status") == "PASS" and
            isinstance(guests, list) and len(guests) == 5 and
            safety.get("guest_initializers_executed") == 0 and
            safety.get("guest_jni_onload_executed") == 0 and
            safety.get("finalize_executed") == 0 and
            safety.get("resolve_executed") == 0,
            "prior M10 five-guest scope changed")
    for guest in guests:
        inspector = guest.get("inspector", {})
        registry = guest.get("registry", {})
        require(inspector.get("status") == "PASS" and
                inspector.get("guest_initializers_executed") == 0 and
                inspector.get("guest_jni_onload_executed") == 0 and
                registry.get("status") == "PASS" and
                registry.get("relocate_executed") == 1 and
                registry.get("guest_initializers_executed") == 0 and
                registry.get("guest_jni_onload_executed") == 0,
                "prior M10 load/relocation scope changed: %s" %
                guest.get("id"))


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
    require(not pending, "safe runner has an incomplete continuation")


def validate_global_gate_wiring(decoded):
    matrix = load_json_bytes(
        read_bytes("framework/tests/test-matrix-v1.json"), "test matrix")
    gates = matrix.get("gates")
    require(isinstance(gates, list) and gates, "test matrix has no gates")
    by_id = {gate.get("id"): gate for gate in gates}
    require(len(by_id) == len(gates), "test matrix gate IDs are duplicated")
    required = {
        "nxloader-m11-audit": (
            "pure", ["python3", "-B",
                     "framework/nxloader/tests/test_m11_audit.py"]),
        "nxandroid-profile-audit": (
            "pure", ["python3", "-B",
                     "framework/nxandroid/tests/test_m11_adapter_profiles.py"]),
        "nxandroid-m11-audit": (
            "pure", ["python3", "-B",
                     "framework/nxandroid/tests/test_m11_audit.py"]),
        "nxandroid-020-adapter-contract": (
            "pure", ["python3", "-B",
                     "framework/nxandroid/tests/"
                     "test_020_adapter_contract.py"]),
        "nxloader-host": (
            "filesystem", ["bash", "framework/nxloader/tests/run-host.sh"]),
        "nxloader-m11-armv7-lifecycle-cross": (
            "filesystem",
            ["bash", "framework/nxloader/tests/"
             "run-m11-armv7-lifecycle-cross.sh"]),
        "nxloader-m11-aarch64-lifecycle-cross": (
            "filesystem",
            ["bash", "framework/nxloader/tests/"
             "run-m11-aarch64-lifecycle-cross.sh"]),
        "nxandroid-host": (
            "filesystem", ["bash", "framework/nxandroid/tests/run-host.sh"]),
        "bootstrap-isolated": (
            "process", ["bash", "framework/nxbootstrap/tests/run-isolated.sh"]),
    }
    for gate_id, (gate_class, command) in required.items():
        gate = by_id.get(gate_id)
        require(isinstance(gate, dict), "required automatic gate missing: %s" %
                gate_id)
        require(gate.get("class") == gate_class and
                gate.get("command") == command and
                gate.get("automatic") is True and
                gate.get("logged") is True,
                "required gate wiring changed: %s" % gate_id)
        if gate_class == "process":
            require(gate.get("namespace_required") is True and
                    gate.get("signals"),
                    "process gate escaped sealed classification")
        else:
            require(gate.get("namespace_required") is False and
                    gate.get("signals") == [],
                    "non-process M11 gate gained signal authority")

    inventory_path = "framework/nxandroid/tools/inventory_m11_guests.py"
    signal_path = "framework/nxandroid/tests/test_signal.c"
    inventory_locations = []
    signal_locations = []
    for gate in gates:
        for field in ("command", "sources", "support_files"):
            values = gate.get(field) or []
            if inventory_path in values:
                inventory_locations.append((gate.get("id"), field))
            if signal_path in values:
                signal_locations.append((gate.get("id"), field))
    require(inventory_locations == [("bootstrap-isolated", "support_files")],
            "inventory is not isolated-only support: %r" %
            inventory_locations)
    require(signal_locations == [("bootstrap-isolated", "sources")],
            "signal fixture escaped bootstrap process gate: %r" %
            signal_locations)

    safe_runner = decoded["framework/tests/run-safe-gates.sh"]
    actual = []
    for line in logical_shell_lines(safe_runner):
        stripped = line.strip()
        # run_gate_external has the same invocation shape and only tolerates
        # exit 77 for a gate needing an owner-supplied artifact.
        if (stripped.startswith("run_gate ") or
                stripped.startswith("run_gate_external ")):
            try:
                words = shlex.split(stripped, comments=True, posix=True)
            except ValueError as error:
                raise GateError("safe runner parse failure") from error
            require(len(words) >= 3, "safe runner gate is incomplete")
            actual.append((words[1], words[2:]))
    actual_by_id = {gate_id: command for gate_id, command in actual}
    expected_by_id = {gate["id"]: gate["command"]
                      for gate in gates if gate.get("automatic")}
    require(len(actual_by_id) == len(actual),
            "safe runner gate IDs are duplicated")
    require(set(actual_by_id) == set(expected_by_id),
            "safe runner gate IDs differ from automatic matrix")
    for gate_id, command in expected_by_id.items():
        require(actual_by_id[gate_id] == command,
                "safe runner command diverges: %s" % gate_id)
    actual_ids = [gate_id for gate_id, _command in actual]
    require(actual_ids.index("nxandroid-m11-audit") <
            actual_ids.index("nxandroid-host") <
            actual_ids.index("bootstrap-isolated"),
            "process gate runs before nxandroid static/host prerequisites")
    first_process = min(actual_ids.index(gate["id"])
                        for gate in gates
                        if gate.get("automatic") and
                        gate.get("class") == "process")
    for prerequisite in ("test-infrastructure", "bootstrap-static-safety",
                         "tooling-filesystem"):
        require(actual_ids.index(prerequisite) < first_process,
                "infrastructure/tooling runs after process authority: %s" %
                prerequisite)
    forbidden_commands = {
        "ssh", "scp", "smbclient", "systemctl", "loginctl", "qdbus",
        "dbus-send", "shutdown", "reboot", "poweroff", "setsid", "pkill",
        "killall",
    }
    for gate_id, command in actual:
        require(not forbidden_commands.intersection(command),
                "unsafe command in safe runner gate: %s" % gate_id)

    inventory = decoded[inventory_path]
    main_start = inventory.find("def main(argv: list[str]) -> int:")
    require(main_start >= 0, "inventory main is missing")
    main_body = inventory[main_start:]
    require(main_body.find("require_sealed_namespace()") <
            main_body.find("update_inventory(") and
            "raise SystemExit(77)" in main_body,
            "direct inventory execution is not fail-closed before inspection")
    infrastructure = decoded["framework/tests/test_infrastructure.py"]
    for token in ("bounded inventory supervisor escaped the sealed process gate",
                  "safe runner command diverges",
                  "nxandroid signal fixture lost pidfd authority"):
        require(token in infrastructure,
                "infrastructure meta-gate token missing: %s" % token)


def validate_run_receipts(document):
    receipts = document.get("run_receipts")
    require(isinstance(receipts, dict) and
            set(receipts) == {"normative", "note", "runs"} and
            receipts.get("normative") is False,
            "run receipts became normative")
    require(isinstance(receipts.get("note"), str) and
            "source pins" in receipts["note"], "run receipt note changed")
    runs = receipts.get("runs")
    require(isinstance(runs, list) and len(runs) == 8,
            "run receipt count changed")
    seen = set()
    for index, receipt in enumerate(runs):
        require(isinstance(receipt, dict) and
                set(receipt) == {"run_id", "scope", "command_status",
                                 "provenance_status"},
                "run receipt contains a path or unknown field")
        run_id = receipt.get("run_id")
        require(isinstance(run_id, str) and RUN_ID.fullmatch(run_id) and
                run_id not in seen, "invalid/duplicate run receipt")
        seen.add(run_id)
        require(isinstance(receipt.get("scope"), str) and
                receipt["scope"] and receipt.get("command_status") == 0,
                "run receipt is malformed or unsuccessful")
        expected_status = ("superseded-by-canonical-isolated-suite"
                           if index == 0 else "supplemental")
        require(receipt.get("provenance_status") == expected_status,
                "run receipt provenance status changed")


def validate_document(document, raw_text):
    expected_top = {
        "schema", "schema_version", "milestone", "scope", "closure",
        "execution_claims", "subgates", "run_receipts", "evidence_pins",
        "requirements",
    }
    require(isinstance(document, dict) and set(document) == expected_top,
            "master audit top-level schema changed")
    require(document.get("schema") == "nxandroid-m11-master-audit-v1" and
            document.get("schema_version") == 1 and
            document.get("milestone") == "M11",
            "master audit identity changed")
    require(document.get("scope") ==
            "explicit-import-policy-loader-lifecycle-boundary-and-adapter-owned-android-order",
            "master audit scope changed")
    validate_privacy(raw_text)
    pins, decoded = validate_pins(document)
    validate_requirements(document, pins, decoded)
    validate_subgates(document, pins, decoded)
    validate_profiles_and_external_scope(document)
    validate_execution_scope(document, decoded)
    validate_global_gate_wiring(decoded)
    validate_run_receipts(document)


def expect_rejected(label, document, mutation):
    candidate = copy.deepcopy(document)
    mutation(candidate)
    raw = json.dumps(candidate, ensure_ascii=False, sort_keys=True)
    try:
        validate_document(candidate, raw)
    except GateError:
        return
    raise GateError("negative audit mutation accepted: %s" % label)


def run_negative_self_tests(document):
    mutations = (
        ("missing-id", lambda value: value["requirements"].pop()),
        ("renumber-master", lambda value: value["requirements"][0].update(
            {"id": "M11-NXL-001"})),
        ("promote-boundary", lambda value: value["requirements"][5].update(
            {"disposition": "verified"})),
        ("hide-zero-traces", lambda value: value["closure"].update(
            {"real_android_runtime_trace_count": 1})),
        ("demote-master", lambda value: value["closure"].update(
            {"master_milestone_complete": False})),
        ("invent-deferred", lambda value: value["closure"][
            "deferred_requirement_ids"].append("M11-006")),
        ("hide-m16-acceptance", lambda value: value["execution_claims"].update(
            {"imported_m16_approved_adapter_acceptance": False})),
        ("external-guest", lambda value: value["execution_claims"].update(
            {"external_guest_elf_loaded_in_m11_lifecycle_runs": True})),
        ("signal-host-fallback", lambda value: value[
            "execution_claims"].update({"signal_execution": "host"})),
        ("bad-pin", lambda value: value["evidence_pins"][0].update(
            {"sha256": "0" * 64})),
        ("missing-evidence-token", lambda value: value["requirements"][0][
            "implementation"][0]["tokens"].append("absent-token")),
        ("hide-limitation", lambda value: value["requirements"][18].update(
            {"limitations": []})),
        ("receipt-path", lambda value: value["run_receipts"]["runs"][0].update(
            {"path": "external-log"})),
    )
    for label, mutation in mutations:
        expect_rejected(label, document, mutation)
    try:
        load_json_bytes(b'{"duplicate":1,"duplicate":2}', "duplicate fixture")
    except GateError:
        pass
    else:
        raise GateError("duplicate JSON key fixture was accepted")
    return len(mutations) + 1


def validate_inventory_availability_classifier():
    path = REPOSITORY / "framework/nxandroid/tools/inventory_m11_guests.py"
    spec = importlib.util.spec_from_file_location(
        "nxandroid_m11_inventory_availability", path
    )
    require(spec is not None and spec.loader is not None,
            "cannot load M11 guest inventory classifier")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    count = len(module.EXPECTED_ABIS)
    require(module.classify_guest_presence([False] * count) == "absent",
            "all-absent owner data is not an explicit optional skip")
    require(module.classify_guest_presence([True] * count) == "complete",
            "complete owner data is not audited")
    try:
        module.classify_guest_presence([True] + [False] * (count - 1))
    except module.InventoryError as error:
        require("partial owner guest set is forbidden" in str(error),
                "partial owner-data failure is not explicit")
    else:
        raise GateError("partial owner guest data was accepted")
    source = path.read_text(encoding="utf-8")
    for token in ("--require-guests", "partial_sets_fail=1",
                  "args.emit or args.require_guests"):
        require(token in source,
                "M11 guest inventory availability contract lacks: %s" % token)
    return 3


def main():
    validate_process_free_python(
        "framework/nxandroid/tests/test_m11_audit.py")
    raw = read_bytes(AUDIT_RELATIVE)
    document = load_json_bytes(raw, "M11 master audit")
    validate_document(document, raw.decode("utf-8"))
    availability_tests = validate_inventory_availability_classifier()
    negative_count = run_negative_self_tests(document)
    dispositions = {}
    for requirement in document["requirements"]:
        disposition = requirement["disposition"]
        dispositions[disposition] = dispositions.get(disposition, 0) + 1
    require(dispositions == {
        "verified": 9,
        "verified_synthetic_only": 3,
        "verified_adapter_acceptance": 4,
        "verified_static_only": 3,
        "limitation_recorded": 1,
    }, "disposition totals changed")
    print("M11 master audit gate passed: accounted=20 completed=20 "
          "deferred=0 master_complete=1 verified=9 "
          "verified_synthetic_only=3 verified_adapter_acceptance=4 "
          "verified_static_only=3 limitation_recorded=1 pins=%d "
          "subgates=5 profiles=5 "
          "core_executable=1 incomplete=4 delegated=kotor "
          "real_android_traces=0 synthetic_initializers=1 "
          "synthetic_jni_onload=1 external_guest_m11_lifecycle=0 "
          "prior_m10_guest_relocations=5 prior_m10_init_jni=0 "
          "signal=canonical_isolated_suite_only negative_tests=%d "
          "owner_data_availability_tests=%d "
          "subprocesses=0 device_access=0 network_access=0 hardware_ran=0" %
          (len(PIN_ROLES), negative_count, availability_tests))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
