#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free completeness, provenance and honesty gate for M13 nxgl."""

import ast
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys


SELF_RELATIVE = "framework/nxgl/tests/test_m13_audit.py"
AUDIT_RELATIVE = "framework/nxgl/m13-audit-v1.json"
REFERENCE_RELATIVE = "framework/nxgl/references/m13-video-evidence-v1.json"
SELF_PATH = Path(__file__)
REPOSITORY = SELF_PATH.resolve().parents[3]
AUDIT_PATH = REPOSITORY / AUDIT_RELATIVE
MAX_TEXT_BYTES = 16 * 1024 * 1024
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
IPV4 = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")

REQUIREMENTS = (
    ("M13-001", "adotar Mali-450/GLES2 como piso real",
     "verified_static_only"),
    ("M13-002", "distinguir ownership SDL e EGL raw",
     "verified_boundary_only"),
    ("M13-003", "resolver entry points do mesmo stack que abriu o contexto",
     "verified_boundary_only"),
    ("M13-004", "rejeitar desktop GL quando guest exige GLES",
     "verified_synthetic_only"),
    ("M13-005", "recriar janela por tentativa de depth/stencil quando necessário",
     "verified_synthetic_only"),
    ("M13-006", "obter EGLConfig realmente entregue",
     "verified_boundary_only"),
    ("M13-007", "obter drawable real positivo", "verified_boundary_only"),
    ("M13-008", "usar drawable real como dimensão de render/present",
     "verified_boundary_only"),
    ("M13-009", "manter ladder RGBA/depth/stencil explícita",
     "verified_host_only"),
    ("M13-010", "preservar scissor/masks/state em fixes de present",
     "verified_synthetic_only"),
    ("M13-011", "implementar alpha-one como quirk Amlogic opt-in comprovado",
     "verified_synthetic_only"),
    ("M13-012", "deixar NPOT wrap fix desligado por padrão",
     "verified_static_only"),
    ("M13-013", "detectar silhueta preta e auditar sampler/wrap antes de shader",
     "verified_synthetic_only"),
    ("M13-014", "não aplicar glFinish globalmente", "verified_static_only"),
    ("M13-015", "não limpar FBO, trocar shader ou converter textura globalmente",
     "verified_static_only"),
    ("M13-016", "validar perda/recriação de contexto e resize/focus",
     "verified_synthetic_only"),
    ("M13-017", "validar SDL/fbdev, KMSDRM e Wayland por capacidade",
     "verified_boundary_only"),
    ("M13-018", "registrar vendor/renderer/version/extensions relevantes",
     "verified_boundary_only"),
    ("M13-019", "criar testes de state preservation e present",
     "verified_synthetic_only"),
    ("M13-020", "cruzar Bully2/Sonic4/Chrono/Castle/LEGO sem generalizar quirks",
     "verified_static_only"),
    ("M13-021", "medir escala/textura sem assumir resolução",
     "verified_synthetic_only"),
    ("M13-022", "expandir matriz de vídeo por stack", "verified_static_only"),
    ("M13-023", "documentar fallback e motivo observado",
     "verified_host_only"),
    ("M13-024", "fechar gate nxgl", "verified_host_only"),
)

PIN_ROLES = {
    "framework/contracts/declarative-v1.json": "framework-contract",
    "framework/nxbootstrap/tests/test-manifest-contract.py":
        "declarative-contract-version-parser",
    "framework/tests/test-matrix-v1.json": "automatic-gate-matrix",
    "framework/tests/run-safe-gates.sh": "canonical-safe-runner",
    "framework/tests/test_infrastructure.py": "infrastructure-policy-gate",
    "framework/nxcompat/tests/run-host.sh": "hermetic-host-gate",
    "framework/nxgl/tests/run-m13-host.sh": "canonical-m13-host-runner",
    "framework/nxgl/VERSION": "version",
    "framework/nxgl/CMakeLists.txt": "build-contract",
    "framework/nxgl/CHANGELOG.md": "component-changelog",
    "framework/nxgl/REGRESSION-MATRIX.md": "regression-matrix",
    "framework/nxgl/README.md": "public-documentation",
    "framework/nxgl/include/nxgl.h": "public-api",
    "framework/nxgl/include/nxgl_provider_recovery.h":
        "opt-in-provider-recovery-api",
    "framework/nxgl/include/nxgl_nxcompat.h": "nxcompat-bridge-api",
    "framework/nxgl/src/nxgl_internal.h": "private-state-and-boundary",
    "framework/nxgl/src/nxgl_arbiter.c": "shared-process-arbiter",
    "framework/nxgl/src/nxgl_logic.c": "pure-policy",
    "framework/nxgl/src/nxgl_sdl2.c": "sdl-egl-runtime-boundary",
    "framework/nxgl/src/nxgl_present.c": "opt-in-present-policy",
    "framework/nxgl/src/nxgl_diagnostics.c": "passive-lifecycle-diagnostics",
    "framework/nxgl/src/nxgl_sdl_hint.c":
        "opt-in-target-sdl-hint-sanitizer",
    "framework/nxgl/src/nxgl_metrics.c": "surface-metrics",
    "framework/nxgl/src/nxgl_nxcompat.c": "typed-capability-bridge",
    "framework/nxgl/src/nxgl_provider_recovery.c":
        "opt-in-provider-recovery-transaction",
    "framework/nxgl/tests/test_nxgl.c": "core-and-present-host-tests",
    "framework/nxgl/tests/test_nxgl_environment.c":
        "sealed-environment-test",
    "framework/nxgl/tests/test_nxgl_open_v2.c":
        "hermetic-open-v2-transaction-tests",
    "framework/nxgl/tests/test_nxgl_diagnostics.c":
        "pure-lifecycle-diagnostic-tests",
    "framework/nxgl/tests/test_nxgl_sdl_hint.c":
        "target-sdl-hint-sanitizer-tests",
    "framework/nxgl/tests/test_nxgl_metrics.c": "pure-metrics-tests",
    "framework/nxgl/tests/test_nxgl_present_v2.c":
        "sealed-present-state-tests",
    "framework/nxgl/tests/test_nxgl_nxcompat.c":
        "fake-graphics-boundary-tests",
    "framework/nxgl/tests/test_nxgl_provider_recovery.c":
        "provider-recovery-transaction-tests",
    "framework/nxgl/tests/fake_provider_good.c":
        "coherent-fake-provider",
    "framework/nxgl/tests/fake_provider_dependency.c":
        "mixed-provider-dependency",
    "framework/nxgl/tests/fake_provider_mixed.c":
        "mixed-provider-fixture",
    REFERENCE_RELATIVE: "sanitized-stack-and-provenance-source",
}

REFERENCE_SOURCE_ROLES = {
    "framework/catalog/port-checks-v1.tsv":
        "canonical-port-evidence-ledger",
    "suportando_outros_devices/video-backend.md":
        "canonical-video-contract",
    "suportando_outros_devices/ports-aprovados.md":
        "approved-reference-policy",
    "suportando_outros_devices/padrao-universal.md":
        "pilot-and-baseline-policy",
    "ports/horizonchase/README.md": "horizon-release-evidence",
    "ports/horizonchase/src/egl_shim.c": "horizon-video-source",
    "ports/castleofillusion/README.md": "castle-release-evidence",
    "ports/castleofillusion/src/imports.c": "castle-sampler-source",
    "ports/sonic4/package/sonic4ep2/README.md":
        "sonic4ep2-release-scope",
    "ports/sonic4/src/egl_shim.c": "sonic4ep2-video-source",
    "ports/bully2/src/egl_shim.c": "bully2-historical-narrow-source",
    "ports/lswtfa/src/hooks/egl.c": "lego-alpha-quirk-provenance-only",
}

EXPECTED_FALSE_EXECUTION = {
    "physical_device_evidence": False,
    "real_gpu_or_display_opened": False,
    "real_egl_or_gles_driver_opened": False,
    "external_guest_code_executed": False,
    "hardware_ran": False,
    "device_access": False,
    "network_access": False,
    "sdl_video_initialized": False,
    "window_created": False,
    "context_created": False,
}

REFERENCE_ORDER = (
    "horizonchase",
    "castleofillusion",
    "sonic4ep2",
    "bully2",
    "lego_star_wars_tfa_alpha_quirk",
    "chrono_pilot",
)

REFERENCE_CLASS = {
    "horizonchase": ("positive_multi_stack_strong", True),
    "castleofillusion": ("positive_multi_device_scoped", True),
    "sonic4ep2": ("positive_release_scoped_by_abi_and_line", True),
    "bully2": ("historical_delisted_narrow_fact_only", False),
    "lego_star_wars_tfa_alpha_quirk":
        ("narrow_quirk_provenance_only", False),
    "chrono_pilot": ("negative_designed_only", False),
}

REFERENCE_SNAPSHOT = {
    "horizonchase": "59d2d38dc496ae0a71726181d3ccc80923e4144d",
    "castleofillusion": "46ffe044818d08c59af25b456f8fa0f40c9ba951",
    "sonic4ep2": "e162654d074dc2f98185a4cf65c1ec47513146bc",
    "bully2":
        "historical-delisted-ledger-79561e7a05d377c4479c9ee42a635ae2bec72b73",
    "lego_star_wars_tfa_alpha_quirk":
        "historical-commit-3572b18fa2c416ba4086dcfce397ffd662a113ee",
    "chrono_pilot": "designed-only-no-framework-release",
}


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
    require("\\" not in value and "\0" not in value,
            "non-canonical evidence path: %s" % value)
    pure = PurePosixPath(value)
    require(not pure.is_absolute() and ".." not in pure.parts,
            "evidence path escapes repository: %s" % value)
    require(str(pure) == value, "non-canonical evidence path: %s" % value)
    return pure


def repository_file(relative):
    pure = canonical_relative_path(relative)
    path = REPOSITORY
    for part in pure.parts:
        path = path / part
        require(not path.is_symlink(),
                "evidence path contains a symlink: %s" % relative)
    require(path.is_file(), "evidence file is missing: %s" % relative)
    size = path.stat().st_size
    require(0 < size <= MAX_TEXT_BYTES,
            "evidence file size is invalid: %s" % relative)
    return path


def read_bytes(relative):
    return repository_file(relative).read_bytes()


def read_text(relative):
    try:
        return read_bytes(relative).decode("utf-8")
    except UnicodeDecodeError as error:
        raise GateError("non-UTF-8 evidence: %s" % relative) from error


def load_json_bytes(raw, label):
    require(len(raw) <= MAX_TEXT_BYTES, "%s exceeds size bound" % label)
    try:
        return json.loads(raw.decode("utf-8"),
                          object_pairs_hook=reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise GateError("invalid JSON in %s: %s" % (label, error)) from error


def load_json(relative):
    return load_json_bytes(read_bytes(relative), relative)


def validate_process_free_python(relative):
    source = read_text(relative)
    try:
        tree = ast.parse(source, filename=relative)
    except SyntaxError as error:
        raise GateError("invalid Python source: %s" % relative) from error
    allowed_modules = {"ast", "hashlib", "json", "pathlib", "re", "sys"}
    forbidden_calls = {
        "system", "popen", "fork", "forkpty", "execv", "execve",
        "execvp", "execvpe", "spawnl", "spawnle", "spawnlp", "spawnlpe",
        "spawnv", "spawnve", "spawnvp", "spawnvpe", "Popen",
        "__import__", "eval", "exec",
    }
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            require(all(alias.name.split(".")[0] in allowed_modules
                        for alias in node.names),
                    "non-process-free import in %s" % relative)
        elif isinstance(node, ast.ImportFrom):
            require((node.module or "").split(".")[0] in allowed_modules,
                    "non-process-free import in %s" % relative)
        elif isinstance(node, ast.Call):
            if isinstance(node.func, ast.Attribute):
                require(node.func.attr not in forbidden_calls,
                        "process execution call in %s" % relative)
            elif isinstance(node.func, ast.Name):
                require(node.func.id not in forbidden_calls,
                        "process execution call in %s" % relative)


def validate_privacy(raw_text, label):
    lowered = raw_text.lower()
    for forbidden in ("/home/", "/mnt/", "/tmp/", "file://", "felipe"):
        require(forbidden not in lowered,
                "%s contains a private path or identity token" % label)
    require(IPV4.search(raw_text) is None, "%s contains an IP address" % label)


def validate_token_list(tokens, text, label):
    require(isinstance(tokens, list) and 1 <= len(tokens) <= 8 and
            len(tokens) == len(set(tokens)),
            "%s has an invalid token list" % label)
    for token in tokens:
        require(isinstance(token, str) and 0 < len(token) <= 512,
                "%s has an invalid evidence token" % label)
        require(token in text, "%s token is absent: %r" % (label, token))


def validate_pins(document, allow_preflight):
    pins = document.get("evidence_pins")
    require(isinstance(pins, list) and len(pins) == len(PIN_ROLES),
            "evidence pin count changed")
    by_path = {}
    decoded = {}
    pending = 0
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
        if digest == "PENDING":
            require(allow_preflight,
                    "unfrozen evidence pin outside --preflight: %s" % path)
            pending += 1
        else:
            require(isinstance(digest, str) and SHA256.fullmatch(digest),
                    "invalid SHA-256: %s" % path)
        raw = read_bytes(path)
        if digest != "PENDING":
            require(hashlib.sha256(raw).hexdigest() == digest,
                    "evidence hash mismatch: %s" % path)
        try:
            decoded[path] = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise GateError("pinned evidence is not UTF-8: %s" % path) from error
        by_path[path] = pin
    require(set(by_path) == set(PIN_ROLES), "evidence pin set changed")
    if allow_preflight:
        require(pending == len(PIN_ROLES),
                "preflight must keep every top-level evidence pin PENDING")
    else:
        require(pending == 0, "final gate has pending evidence pins")
    return by_path, decoded, pending


def validate_reference(reference, pins, decoded, requirement_id):
    require(isinstance(reference, dict) and
            set(reference) == {"path", "tokens"},
            "%s has malformed evidence" % requirement_id)
    path = reference.get("path")
    require(path in pins, "%s cites unpinned evidence: %s" %
            (requirement_id, path))
    require(path not in {AUDIT_RELATIVE, SELF_RELATIVE},
            "%s uses circular audit evidence" % requirement_id)
    validate_token_list(reference.get("tokens"), decoded[path],
                        "%s/%s" % (requirement_id, path))


def require_requirement_tokens(item, field, path, required_tokens):
    matches = [evidence for evidence in item[field]
               if evidence.get("path") == path]
    require(len(matches) == 1,
            "%s lost required %s evidence: %s" %
            (item["id"], field, path))
    require(set(required_tokens).issubset(set(matches[0]["tokens"])),
            "%s weakened required evidence tokens: %s" %
            (item["id"], path))


def validate_reference_source(document):
    raw_text = read_text(REFERENCE_RELATIVE)
    validate_privacy(raw_text, "M13 reference source")
    require(isinstance(document, dict) and set(document) == {
        "schema", "schema_version", "milestone", "scope",
        "source_of_truth_for", "evidence_policy", "source_pins",
        "references", "backend_matrix", "quirk_boundaries",
        "global_prohibitions",
    }, "M13 reference source top-level schema changed")
    require(document["schema"] == "nxgl-m13-video-evidence-v1" and
            document["schema_version"] == 1 and
            document["milestone"] == "M13",
            "M13 reference identity changed")
    require(document["source_of_truth_for"] == ["M13-020", "M13-022"],
            "M13 reference source ownership changed")

    policy = document["evidence_policy"]
    require(isinstance(policy, dict) and set(policy) == {
        "principle", "m13_physical_device_evidence", "m13_device_access",
        "m13_network_access", "m13_external_guest_code_executed",
        "imported_physical_history_is_not_m13_execution",
        "support_claims_created_by_this_file",
    }, "M13 reference evidence policy changed")
    for key in ("m13_physical_device_evidence", "m13_device_access",
                "m13_network_access", "m13_external_guest_code_executed",
                "support_claims_created_by_this_file"):
        require(policy[key] is False, "M13 reference overclaims %s" % key)
    require(policy["imported_physical_history_is_not_m13_execution"] is True,
            "imported history boundary was weakened")

    source_pins = document["source_pins"]
    require(isinstance(source_pins, list) and
            len(source_pins) == len(REFERENCE_SOURCE_ROLES),
            "reference source pin count changed")
    seen = set()
    for pin in source_pins:
        require(isinstance(pin, dict) and set(pin) == {
            "path", "sha256", "role", "tokens",
        }, "malformed reference source pin")
        path = pin["path"]
        require(path in REFERENCE_SOURCE_ROLES and path not in seen,
                "unexpected or duplicate reference source pin: %s" % path)
        require(pin["role"] == REFERENCE_SOURCE_ROLES[path],
                "reference source role changed: %s" % path)
        require(isinstance(pin["sha256"], str) and
                SHA256.fullmatch(pin["sha256"]),
                "reference source hash is not frozen: %s" % path)
        raw = read_bytes(path)
        require(hashlib.sha256(raw).hexdigest() == pin["sha256"],
                "reference source hash mismatch: %s" % path)
        try:
            source_text = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise GateError("reference source is not UTF-8: %s" % path) from error
        validate_token_list(pin["tokens"], source_text,
                            "reference source/%s" % path)
        seen.add(path)
    require(seen == set(REFERENCE_SOURCE_ROLES),
            "reference source pin set changed")

    references = document["references"]
    require(isinstance(references, list) and len(references) == 6,
            "reference count changed")
    require([item.get("canonical_id") for item in references] ==
            list(REFERENCE_ORDER), "reference order or IDs changed")
    by_id = {}
    for item in references:
        require(isinstance(item, dict) and set(item) == {
            "canonical_id", "title", "reference_class",
            "positive_reference", "approved_scope", "source_snapshot",
            "source_paths", "physical_evidence_imported", "physical_stacks",
            "allowed_reuse", "forbidden_generalization", "limitations",
        }, "malformed M13 reference entry")
        reference_id = item["canonical_id"]
        wanted_class, wanted_positive = REFERENCE_CLASS[reference_id]
        require(item["reference_class"] == wanted_class and
                item["positive_reference"] is wanted_positive,
                "reference class changed: %s" % reference_id)
        require(item["source_snapshot"] == REFERENCE_SNAPSHOT[reference_id],
                "reference snapshot changed: %s" % reference_id)
        require(isinstance(item["approved_scope"], str) and
                30 <= len(item["approved_scope"]) <= 400,
                "invalid approved scope: %s" % reference_id)
        require(isinstance(item["source_snapshot"], str) and
                8 <= len(item["source_snapshot"]) <= 160,
                "invalid source snapshot: %s" % reference_id)
        paths = item["source_paths"]
        require(isinstance(paths, list) and paths and
                len(paths) == len(set(paths)),
                "invalid source path list: %s" % reference_id)
        for path in paths:
            canonical_relative_path(path)
            require(path in REFERENCE_SOURCE_ROLES,
                    "reference uses unpinned source: %s" % path)
        for field in ("allowed_reuse", "forbidden_generalization",
                      "limitations"):
            values = item[field]
            require(isinstance(values, list) and values and
                    len(values) == len(set(values)) and
                    all(isinstance(value, str) and 20 <= len(value) <= 500
                        for value in values),
                    "invalid %s: %s" % (field, reference_id))
        stacks = item["physical_stacks"]
        require(isinstance(stacks, list),
                "invalid physical stack list: %s" % reference_id)
        for stack in stacks:
            require(isinstance(stack, dict) and set(stack) == {
                "release_line", "stack_key", "backend", "ownership", "gles",
                "evidence_level", "result_scope", "same_artifact_group",
            }, "malformed physical stack: %s" % reference_id)
            require(all(isinstance(value, str) and value
                        for value in stack.values()),
                    "empty physical stack fact: %s" % reference_id)
        require(item["physical_evidence_imported"] is bool(len(stacks)),
                "imported physical evidence flag changed: %s" % reference_id)
        by_id[reference_id] = item

    require({item["canonical_id"] for item in references
             if item["positive_reference"]} ==
            {"horizonchase", "castleofillusion", "sonic4ep2"},
            "positive reference set changed")
    require(by_id["chrono_pilot"]["physical_stacks"] == [] and
            by_id["chrono_pilot"]["source_paths"] ==
            ["suportando_outros_devices/padrao-universal.md"],
            "Chrono gained positive or source-code evidence")
    require(len(by_id["bully2"]["allowed_reuse"]) == 1 and
            by_id["bully2"]["source_paths"] ==
            ["framework/catalog/port-checks-v1.tsv",
             "ports/bully2/src/egl_shim.c"],
            "Bully2 scope widened")
    require(any("color mask" in limitation
                for limitation in
                by_id["lego_star_wars_tfa_alpha_quirk"]["limitations"]),
            "LEGO source limitation was lost")
    require(by_id["sonic4ep2"]["physical_stacks"][0]["same_artifact_group"] !=
            by_id["sonic4ep2"]["physical_stacks"][1]["same_artifact_group"],
            "Sonic loader lines were merged")

    matrix = document["backend_matrix"]
    require(isinstance(matrix, list) and len(matrix) == 4,
            "video backend matrix size changed")
    require([row.get("stack_key") for row in matrix] == [
        "sdl-mali-fbdev", "sdl-kmsdrm", "sdl-wayland", "sdl-unknown",
    ], "video backend matrix order changed")
    for row in matrix:
        require(isinstance(row, dict) and set(row) == {
            "stack_key", "opened_backend", "selection_fact",
            "ownership_policy", "entry_point_policy", "present_policy",
            "drawable_policy", "baseline", "evidence_sources",
            "evidence_level", "m13_physical_validation",
        }, "malformed video backend row")
        require(row["m13_physical_validation"] is False,
                "backend row claims M13 physical validation")
        require(isinstance(row["evidence_sources"], list) and
                len(row["evidence_sources"]) ==
                len(set(row["evidence_sources"])) and
                all(source in by_id for source in row["evidence_sources"]),
                "invalid backend provenance: %s" % row["stack_key"])
    require(matrix[1]["ownership_policy"].startswith("SDL owns") and
            matrix[2]["ownership_policy"].startswith("SDL owns"),
            "KMSDRM/Wayland ownership was weakened")
    require(matrix[3]["evidence_sources"] == [] and
            "fail closed" in matrix[3]["ownership_policy"],
            "unknown stack no longer fails closed")

    quirks = document["quirk_boundaries"]
    require(isinstance(quirks, list) and len(quirks) == 2 and
            [item.get("quirk") for item in quirks] == [
                "amlogic-default-backbuffer-alpha-one",
                "global-npot-wrap-rewrite",
            ], "quirk boundary set changed")
    for quirk in quirks:
        require(isinstance(quirk, dict) and set(quirk) == {
            "quirk", "default_enabled", "activation", "scope",
            "required_state_restore", "provenance",
        }, "malformed quirk boundary")
        require(quirk["default_enabled"] is False,
                "quirk became enabled by default: %s" % quirk["quirk"])
    require(set(quirks[0]["required_state_restore"]) ==
            {"framebuffer", "scissor enablement", "color mask", "clear color"},
            "alpha-one state restoration contract changed")

    require(document["global_prohibitions"] == [
        "no global glFinish",
        "no global FBO clear",
        "no global shader substitution",
        "no global texture conversion",
        "no support inference from a GPU, firmware or device name",
    ], "global video prohibitions changed")


def validate_closure(document, allow_preflight):
    closure = document.get("closure")
    require(isinstance(closure, dict) and set(closure) == {
        "state", "requirements_accounted", "modeled_requirement_count",
        "completed_requirement_count", "pending_requirement_count",
        "master_milestone_complete", "host_gate_closed",
        "evidence_pins_frozen", "physical_device_evidence",
        "physical_followup_milestones",
    }, "M13 closure schema changed")
    require(closure["requirements_accounted"] == 24 and
            closure["modeled_requirement_count"] == 24 and
            closure["physical_device_evidence"] is False and
            closure["physical_followup_milestones"] == ["M22"],
            "M13 closure accounting changed")
    if allow_preflight:
        require(closure == {
            "state": "preflight_pending_core_freeze",
            "requirements_accounted": 24,
            "modeled_requirement_count": 24,
            "completed_requirement_count": 0,
            "pending_requirement_count": 24,
            "master_milestone_complete": False,
            "host_gate_closed": False,
            "evidence_pins_frozen": False,
            "physical_device_evidence": False,
            "physical_followup_milestones": ["M22"],
        }, "M13 preflight closure overclaims completion")
    else:
        require(closure == {
            "state": "host_closed_physical_deferred",
            "requirements_accounted": 24,
            "modeled_requirement_count": 24,
            "completed_requirement_count": 24,
            "pending_requirement_count": 0,
            "master_milestone_complete": True,
            "host_gate_closed": True,
            "evidence_pins_frozen": True,
            "physical_device_evidence": False,
            "physical_followup_milestones": ["M22"],
        }, "M13 final closure is incomplete or overclaimed")


def validate_execution_claims(document):
    claims = document.get("execution_claims")
    require(isinstance(claims, dict) and set(claims) == {
        "automatic_gate_scope", "historical_physical_evidence_imported",
        "process_free_gate", *EXPECTED_FALSE_EXECUTION.keys(),
    }, "M13 execution claim schema changed")
    require(isinstance(claims["automatic_gate_scope"], str) and
            40 <= len(claims["automatic_gate_scope"]) <= 600,
            "M13 automatic gate scope is invalid")
    require(claims["historical_physical_evidence_imported"] is True,
            "M13 lost imported-history disclosure")
    require(claims["process_free_gate"] is True,
            "M13 audit is not declared process-free")
    for key, expected in EXPECTED_FALSE_EXECUTION.items():
        require(claims[key] is expected, "M13 overclaims %s" % key)


def validate_framework_contract(decoded):
    contract_path = "framework/contracts/declarative-v1.json"
    contract = load_json_bytes(decoded[contract_path].encode("utf-8"),
                               contract_path)
    require(isinstance(contract, dict) and
            contract.get("schema_version") == 1,
            "M13 framework contract schema changed")
    components = contract.get("components")
    require(isinstance(components, list),
            "M13 framework component lock is missing")
    nxgl_components = [item for item in components
                       if isinstance(item, dict) and item.get("id") == "nxgl"]
    require(len(nxgl_components) == 1,
            "M13 framework nxgl component lock changed")
    nxgl_component = nxgl_components[0]
    # M13 closed on nxgl 0.2.6. What the lock actually protects is the API,
    # not the minor number: the contract says "API 2 is additive". Pinning the
    # 0.2.x minor would have forced an API bump for a purely additive V4
    # release, which is the opposite of what the lock is for. So it accepts
    # any version at or above the closed one while api_version stays 2, and
    # still refuses an API bump or a regression below M13.
    locked_version = str(nxgl_component.get("current_version", ""))
    version_parts = locked_version.split(".")
    version_ok = (len(version_parts) == 3 and
                  all(part.isdigit() for part in version_parts) and
                  tuple(int(part) for part in version_parts) >= (0, 2, 6))
    require(nxgl_component.get("version_file") == "framework/nxgl/VERSION" and
            version_ok and
            nxgl_component.get("api_version") == 2 and
            "API 2 is additive" in nxgl_component.get("compatibility", "") and
            "SDL_ResetHint optional" in
            nxgl_component.get("compatibility", "") and
            "target-ABI pre-video SDL_VIDEODRIVER sanitizer" in
            nxgl_component.get("compatibility", ""),
            "M13 framework nxgl version/API lock changed")
    # Same rule as the component lock: at or above the closed version, on the
    # same additive API 2, and equal to the declared component version.
    file_version = decoded["framework/nxgl/VERSION"].strip()
    file_parts = file_version.split(".")
    require(len(file_parts) == 3 and all(p.isdigit() for p in file_parts) and
            tuple(int(p) for p in file_parts) >= (0, 2, 6) and
            file_version == locked_version,
            "M13 nxgl VERSION changed")
    header = decoded["framework/nxgl/include/nxgl.h"]
    for pattern in (
        r"^#define NXGL_API_VERSION 1u$",
        r"^#define NXGL_API_VERSION_V1 1u$",
        r"^#define NXGL_API_VERSION_V2 2u$",
        r"^#define NXGL_API_CURRENT_VERSION NXGL_API_VERSION_V2$",
        r'^#define NXGL_VERSION "%s"$' % re.escape(file_version),
    ):
        require(re.search(pattern, header, re.MULTILINE) is not None,
                "M13 nxgl public version/API defines changed")
    cmake = decoded["framework/nxgl/CMakeLists.txt"]
    require(re.search(r"^project\(nxgl VERSION %s LANGUAGES C\)$" %
                      re.escape(file_version), cmake, re.MULTILINE) is not None,
            "M13 nxgl CMake version changed")
    manifest_gate = decoded[
        "framework/nxbootstrap/tests/test-manifest-contract.py"]
    require("contract.get(\"contract_version\")" in
            manifest_gate and
            '"NXGL_API_CURRENT_VERSION"' in manifest_gate,
            "M13 manifest gate no longer validates the current nxgl API")
    require("nxgl_plan_sdl_provider_pair" in header and
            "nxgl_plan_sdl_precontext_recovery_v2" in header and
            "nxgl_sanitize_sdl_video_hint_v2" in header and
            "nxgl_complete_sdl_video_hint_receipt_v2" in header and
            "nxgl_format_sdl_video_hint_receipt_v2" in header,
            "M13 nxgl additive startup APIs are missing")
    recovery_header = decoded[
        "framework/nxgl/include/nxgl_provider_recovery.h"]
    require("nxgl_probe_sdl_provider_v2" in recovery_header and
            "nxgl_reexec_sdl_provider_pair_v2" in recovery_header and
            "NXGL_PROVIDER_RECOVERY_MARKER" in recovery_header and
            "callers must set enabled=1" in recovery_header and
            "video_torn_down" in recovery_header and
            "This function never changes LD_PRELOAD" in recovery_header,
            "M13 nxgl provider-recovery API is missing")


def validate_m13_host_runner(decoded):
    path = "framework/nxgl/tests/run-m13-host.sh"
    runner = decoded[path]
    for token in (
        "m13_executables=(",
        "seal_fake_executable",
        "check_public_archive",
        "check_provider_recovery_archive",
        "link_install_smoke",
        "for compiler in gcc clang",
        "^nxgl-m13-(open-v2|present-v2|metrics|diagnostics|sdl-hint|provider-recovery)$",
        "guest_code_executed=0",
        "physical_device_evidence=0",
        "device_access=0",
        "network_access=0",
        "session_access=0",
        "provider_recovery=1",
        "sdl_hint=1",
    ):
        require(token in runner, "M13 host runner lost seal: %s" % token)
    require("NXGL_BUILD_NATIVE_TESTS=OFF" in runner and
            "SDL_VIDEODRIVER=dummy" in runner and
            "unset DISPLAY WAYLAND_DISPLAY" in runner,
            "M13 host runner may reach a native display path")


def validate_m13_gate_integration(decoded):
    matrix_path = "framework/tests/test-matrix-v1.json"
    matrix = load_json_bytes(decoded[matrix_path].encode("utf-8"), matrix_path)
    gates = matrix.get("gates") if isinstance(matrix, dict) else None
    require(isinstance(gates, list), "M13 automatic gate matrix is malformed")
    gate_ids = [gate.get("id") for gate in gates if isinstance(gate, dict)]
    require(len(gate_ids) == len(gates) and
            len(gate_ids) == len(set(gate_ids)),
            "M13 automatic gate IDs are malformed or duplicated")
    by_id = {gate["id"]: gate for gate in gates}
    host = by_id.get("nxgl-m13-host")
    require(isinstance(host, dict) and
            host.get("class") == "filesystem" and
            host.get("command") ==
            ["bash", "framework/nxgl/tests/run-m13-host.sh"] and
            host.get("sources") == [
                "framework/nxgl/tests/test_nxgl_open_v2.c",
                "framework/nxgl/tests/test_nxgl_present_v2.c",
                "framework/nxgl/tests/test_nxgl_metrics.c",
                "framework/nxgl/tests/test_nxgl_diagnostics.c",
                "framework/nxgl/tests/test_nxgl_sdl_hint.c",
                "framework/nxgl/tests/test_nxgl_provider_recovery.c",
            ] and host.get("support_files") == [
                "framework/nxgl/tests/run-m13-host.sh",
                "framework/nxgl/CMakeLists.txt",
                "framework/nxgl/include/nxgl_provider_recovery.h",
                "framework/nxgl/src/nxgl_arbiter.c",
                "framework/nxgl/src/nxgl_sdl_hint.c",
                "framework/nxgl/src/nxgl_provider_recovery.c",
                "framework/nxgl/tests/fake_provider_good.c",
                "framework/nxgl/tests/fake_provider_dependency.c",
                "framework/nxgl/tests/fake_provider_mixed.c",
            ] and host.get("logged") is True and
            host.get("automatic") is True and
            host.get("namespace_required") is False and
            host.get("signals") == [],
            "nxgl-m13-host matrix boundary changed")
    audit = by_id.get("nxgl-m13-audit")
    require(isinstance(audit, dict) and audit.get("class") == "pure" and
            audit.get("command") == [
                "python3", "-B", "framework/nxgl/tests/test_m13_audit.py",
            ] and audit.get("sources") == [
                "framework/nxgl/tests/test_m13_audit.py",
            ] and audit.get("support_files") == [
                "framework/nxgl/m13-audit-v1.json",
                "framework/nxgl/references/m13-video-evidence-v1.json",
            ] and audit.get("logged") is True and
            audit.get("automatic") is True and
            audit.get("namespace_required") is False and
            audit.get("signals") == [],
            "nxgl-m13-audit matrix boundary changed")

    safe_runner = decoded["framework/tests/run-safe-gates.sh"]
    require(safe_runner.count("run_gate nxgl-m13-host") == 1 and
            safe_runner.count("run_gate nxgl-m13-audit") == 1 and
            "bash framework/nxgl/tests/run-m13-host.sh" in safe_runner and
            "python3 -B framework/nxgl/tests/test_m13_audit.py" in safe_runner,
            "canonical safe runner lost an M13 gate")
    infrastructure = decoded["framework/tests/test_infrastructure.py"]
    for token in (
        "def check_m13_host_gate(matrix):",
        'host = by_id.get("nxgl-m13-host")',
        'audit = by_id.get("nxgl-m13-audit")',
        '"physical_device_evidence=0"',
        '"test-nxgl-provider-recovery"',
        '"test-nxgl-sdl-hint"',
        "check_m13_host_gate(matrix)",
    ):
        require(token in infrastructure,
                "infrastructure gate lost M13 seal: %s" % token)


def validate_requirements(document, pins, decoded, allow_preflight):
    requirements = document.get("requirements")
    require(isinstance(requirements, list) and len(requirements) == 24,
            "M13 must account for exactly 24 requirements")
    require([item.get("id") for item in requirements] ==
            [item[0] for item in REQUIREMENTS],
            "M13 requirement order or IDs changed")
    expected = {item[0]: item[1:] for item in REQUIREMENTS}
    cited_paths = set()
    by_id = {}
    for item in requirements:
        require(isinstance(item, dict) and set(item) == {
            "id", "requirement", "disposition", "status", "claim",
            "implementation", "tests", "references", "limitations",
        }, "malformed M13 requirement entry")
        requirement_id = item["id"]
        wanted_requirement, wanted_disposition = expected[requirement_id]
        require(item["requirement"] == wanted_requirement,
                "%s requirement text changed" % requirement_id)
        require(item["disposition"] == wanted_disposition,
                "%s disposition changed" % requirement_id)
        require(item["status"] ==
                ("pending_core_freeze" if allow_preflight else "verified"),
                "%s status does not match gate mode" % requirement_id)
        require(isinstance(item["claim"], str) and
                40 <= len(item["claim"]) <= 1000,
                "%s has an invalid claim" % requirement_id)
        require(isinstance(item["implementation"], list) and
                item["implementation"],
                "%s lacks implementation evidence" % requirement_id)
        require(isinstance(item["tests"], list) and item["tests"],
                "%s lacks test evidence" % requirement_id)
        require(isinstance(item["references"], list),
                "%s references are invalid" % requirement_id)
        require(isinstance(item["limitations"], list) and
                len(item["limitations"]) == len(set(item["limitations"])) and
                all(isinstance(value, str) and 15 <= len(value) <= 600
                    for value in item["limitations"]),
                "%s limitations are invalid" % requirement_id)
        for field in ("implementation", "tests", "references"):
            paths = [evidence.get("path") for evidence in item[field]
                     if isinstance(evidence, dict)]
            require(len(paths) == len(set(paths)),
                    "%s has duplicate %s evidence paths" %
                    (requirement_id, field))
            for evidence in item[field]:
                validate_reference(evidence, pins, decoded, requirement_id)
                cited_paths.add(evidence["path"])
        by_id[requirement_id] = item

    require(REFERENCE_RELATIVE in
            {entry["path"] for entry in by_id["M13-020"]["references"]},
            "M13-020 does not cite the provenance source of truth")
    require(REFERENCE_RELATIVE in
            {entry["path"] for entry in by_id["M13-022"]["implementation"]},
            "M13-022 does not use the video stack source of truth")
    for requirement_id in ("M13-011", "M13-012", "M13-013"):
        require(REFERENCE_RELATIVE in {
            entry["path"] for entry in by_id[requirement_id]["references"]
        }, "%s lost narrow quirk provenance" % requirement_id)
    require_requirement_tokens(
        by_id["M13-002"], "implementation", "framework/nxgl/include/nxgl.h",
        {"NXGL_STACK_OWNER_V2_SDL_EGL", "NXGL_STACK_OWNER_V2_RAW_EGL",
         "stack_userdata must remain alive and valid", "until nxgl_close_v2()"})
    require_requirement_tokens(
        by_id["M13-006"], "implementation", "framework/nxgl/src/nxgl_sdl2.c",
        {"nxgl_v2_egl_valid", "required_renderable_bit",
         "requested->gles_major >= 3"})
    require_requirement_tokens(
        by_id["M13-006"], "tests",
        "framework/nxgl/tests/test_nxgl_open_v2.c",
        {"test_egl_renderable_bit_tracks_requested_api"})
    require_requirement_tokens(
        by_id["M13-017"], "implementation",
        "framework/nxgl/include/nxgl.h",
        {"nxgl_provider_name_compatible", "wayland-gbm"})
    require_requirement_tokens(
        by_id["M13-017"], "implementation",
        "framework/nxgl/src/nxgl_diagnostics.c",
        {"nxgl_provider_name_compatible", "has_direct", "has_wayland"})
    require_requirement_tokens(
        by_id["M13-017"], "tests",
        "framework/nxgl/tests/test_nxgl_diagnostics.c",
        {"test_provider_name_prefilter_is_transport_aware",
         "libmali-bifrost-g31-rxp0-wayland-gbm.so"})
    require_requirement_tokens(
        by_id["M13-017"], "implementation",
        "framework/nxgl/src/nxgl_sdl_hint.c",
        {"SDL_GetNumVideoDrivers", "SDL_GetVideoDriver",
         'unsetenv("SDL_VIDEODRIVER")',
         "nxgl_complete_sdl_video_hint_receipt_v2"})
    require_requirement_tokens(
        by_id["M13-017"], "tests",
        "framework/nxgl/tests/test_nxgl_sdl_hint.c",
        {"test_no_hint_keeps_autodetection",
         "test_supported_hint_is_preserved_and_completed",
         "test_unsupported_hint_is_cleared",
         "test_another_supported_backend_is_never_forced",
         "test_only_canonical_hint_may_change"})
    require_requirement_tokens(
        by_id["M13-023"], "implementation",
        "framework/nxgl/src/nxgl_sdl2.c",
        {"The terminal callback is part of the open transaction",
         "both caller outputs unpublished"})
    require_requirement_tokens(
        by_id["M13-023"], "tests",
        "framework/nxgl/tests/test_nxgl_open_v2.c",
        {"fake_terminal_status_locked", "acquired == 0", "NXGL_ERROR_BUSY"})
    require_requirement_tokens(
        by_id["M13-023"], "tests",
        "framework/nxgl/tests/test_nxgl_present_v2.c",
        {"test_busy_is_byte_atomic_and_callback_free",
         "test_present_v2_callback_cannot_reenter_v1",
         "test_v1_rejects_v2_context_when_uncontended",
         "provider.reentry_result == NXGL_ERROR_BUSY",
         "memcmp(&result, &before_result"})
    require_requirement_tokens(
        by_id["M13-024"], "implementation",
        "framework/nxgl/include/nxgl_provider_recovery.h",
        {"callers must set enabled=1", "nxgl_probe_sdl_provider_v2",
         "nxgl_reexec_sdl_provider_pair_v2",
         "video_torn_down", "This function never changes LD_PRELOAD",
         "not adversarial TOCTOU protection"})
    require_requirement_tokens(
        by_id["M13-024"], "implementation",
        "framework/nxgl/src/nxgl_provider_recovery.c",
        {"RTLD_NOLOAD", "dladdr", "SDL_VIDEO_EGL_DRIVER",
         "SDL_VIDEO_GL_DRIVER", 'nxgl_provider_execv("/proc/self/exe"'})
    require_requirement_tokens(
        by_id["M13-024"], "tests",
        "framework/nxgl/tests/test_nxgl_provider_recovery.c",
        {"test_probe_is_explicit_and_fail_closed",
         "NXGL_PROVIDER_RECOVERY_V2_MIXED_OBJECTS",
         "NXGL_ERROR_BUSY", "memcmp(&receipt",
         "test_partial_environment_failure_rolls_back",
         "nxgl-test-preload-sentinel",
         "test_terminate_failure_poison", "unexpected_exec_calls == 0"})
    provider_test = decoded[
        "framework/nxgl/tests/test_nxgl_provider_recovery.c"]
    require(all(token in provider_test for token in {
        "NXGL_PROVIDER_RECOVERY_V2_CANDIDATE_UNAVAILABLE",
        "NXGL_PROVIDER_RECOVERY_V2_EGL_SYMBOLS_MISSING",
        "NXGL_PROVIDER_RECOVERY_V2_ENGINE_SYMBOLS_MISSING",
        "NXGL_PROVIDER_RECOVERY_V2_VIDEO_NOT_TORN_DOWN",
        "NXGL_PROVIDER_RECOVERY_V2_INHERITED_PROVIDER_OVERRIDE",
        "NXGL_PROVIDER_RECOVERY_V2_EGL_TERMINATE_FAILED",
        "memcmp(&result", "result.environment_restored == 1",
        "observing_exec_hook",
    }), "M13-024 provider recovery coverage weakened")
    require({
        "framework/tests/test-matrix-v1.json",
        "framework/tests/run-safe-gates.sh",
        "framework/tests/test_infrastructure.py",
        "framework/nxcompat/tests/run-host.sh",
        "framework/nxgl/tests/run-m13-host.sh",
    }.issubset({entry["path"] for entry in by_id["M13-024"]["tests"]}),
            "M13-024 does not cite all host gate boundaries")
    require({
        "framework/contracts/declarative-v1.json",
        "framework/nxgl/VERSION",
        "framework/nxgl/include/nxgl.h",
        "framework/nxgl/include/nxgl_provider_recovery.h",
        "framework/nxgl/src/nxgl_provider_recovery.c",
    }.issubset({entry["path"]
                for entry in by_id["M13-024"]["implementation"]}) and
            "framework/nxbootstrap/tests/test-manifest-contract.py" in
            {entry["path"] for entry in by_id["M13-024"]["tests"]},
            "M13-024 does not cite the version/API contract closure")
    require(set(PIN_ROLES) - cited_paths <= {
        "framework/nxgl/VERSION",
        "framework/nxgl/CMakeLists.txt",
        "framework/tests/run-safe-gates.sh",
        "framework/contracts/declarative-v1.json",
    }, "uncited M13 evidence pins changed")


def validate_document(document, allow_preflight):
    require(isinstance(document, dict) and set(document) == {
        "schema", "schema_version", "milestone", "scope", "closure",
        "execution_claims", "evidence_pins", "requirements",
    }, "M13 audit top-level schema changed")
    require(document["schema"] == "nxgl-m13-master-audit-v1" and
            document["schema_version"] == 1 and
            document["milestone"] == "M13",
            "M13 audit identity changed")
    require(isinstance(document["scope"], str) and
            20 <= len(document["scope"]) <= 300,
            "M13 audit scope is invalid")
    validate_closure(document, allow_preflight)
    validate_execution_claims(document)
    pins, decoded, pending = validate_pins(document, allow_preflight)
    validate_framework_contract(decoded)
    validate_m13_host_runner(decoded)
    validate_m13_gate_integration(decoded)
    reference_document = load_json(REFERENCE_RELATIVE)
    validate_reference_source(reference_document)
    validate_requirements(document, pins, decoded, allow_preflight)
    return pending


def main():
    require(sys.argv[1:] in ([], ["--preflight"]),
            "usage: test_m13_audit.py [--preflight]")
    allow_preflight = sys.argv[1:] == ["--preflight"]
    validate_process_free_python(SELF_RELATIVE)
    audit_raw = read_bytes(AUDIT_RELATIVE)
    try:
        audit_text = audit_raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise GateError("M13 audit is not UTF-8") from error
    validate_privacy(audit_text, "M13 audit")
    document = load_json_bytes(audit_raw, AUDIT_RELATIVE)
    pending = validate_document(document, allow_preflight)
    if allow_preflight:
        print("M13 audit preflight PASS: requirements=24 pending_pins=%d "
              "process_free=1 physical_device_evidence=0" % pending)
    else:
        print("M13 audit PASS: requirements=24 pinned=%d process_free=1 "
              "physical_device_evidence=0" % len(PIN_ROLES))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GateError as error:
        print("M13 audit FAIL: %s" % error, file=sys.stderr)
        raise SystemExit(1)
