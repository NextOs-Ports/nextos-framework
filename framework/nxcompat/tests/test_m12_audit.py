#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free completeness and honesty gate for the M12 nxcompat ledger."""

import ast
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys


SELF_RELATIVE = "framework/nxcompat/tests/test_m12_audit.py"
AUDIT_RELATIVE = "framework/nxcompat/m12-audit-v1.json"
SELF_PATH = Path(__file__)
REPOSITORY = SELF_PATH.resolve().parents[3]
AUDIT_PATH = REPOSITORY / AUDIT_RELATIVE
MAX_TEXT_BYTES = 16 * 1024 * 1024
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
IPV4 = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")

REQUIREMENTS = (
    ("M12-001", "congelar API de probe/plan/report", "verified"),
    ("M12-002", "detectar arquitetura/userland sem nome de aparelho",
     "verified_synthetic_only"),
    ("M12-003", "detectar GLES/EGL por abertura e strings reais",
     "verified_boundary_only"),
    ("M12-004", "detectar drawable/display stack por capacidade",
     "verified_boundary_only"),
    ("M12-005", "detectar memória/limites sem perfis rígidos de modelo",
     "verified_synthetic_only"),
    ("M12-006", "detectar áudio por abertura real e rejeitar dummy/disk",
     "verified_boundary_only"),
    ("M12-007", "detectar controles e hotplug por APIs disponíveis",
     "verified_boundary_only"),
    ("M12-008", "preservar hints herdados antes de fallback",
     "verified_host_only"),
    ("M12-009", "remover hint inválido só depois de falha registrada",
     "verified_host_only"),
    ("M12-010", "fazer apenas uma retry controlada",
     "verified_host_only"),
    ("M12-011", "manter vídeo e áudio independentes",
     "verified_host_only"),
    ("M12-012", "não promover capability por nome de CFW/device",
     "verified_static_only"),
    ("M12-013", "representar reason codes estáveis", "verified"),
    ("M12-014", "gerar relatório para logo/log sem dado pessoal",
     "verified_synthetic_only"),
    ("M12-015", "testar probes com fixtures sintéticas",
     "verified_synthetic_only"),
    ("M12-016", "testar falhas parciais e rollback de backend",
     "verified_host_only"),
    ("M12-017", "integrar adaptador SDL2 sem tomar ownership indevido",
     "verified_boundary_only"),
    ("M12-018", "auditar concorrência/estado global",
     "verified_host_only"),
    ("M12-019", "documentar capabilities opcionais vs baseline",
     "verified"),
    ("M12-020", "fechar gate nxcompat", "verified_host_only"),
)

PIN_ROLES = {
    "framework/contracts/declarative-v1.json": "framework-contract",
    "framework/tests/test-matrix-v1.json": "automatic-gate-matrix",
    "framework/tests/run-safe-gates.sh": "canonical-safe-runner",
    "framework/tests/test_infrastructure.py": "infrastructure-policy-gate",
    "framework/nxcompat/VERSION": "version",
    "framework/nxcompat/CMakeLists.txt": "build-contract",
    "framework/nxcompat/README.md": "public-documentation",
    "framework/nxcompat/capabilities-v1.json": "capability-source",
    "framework/nxcompat/include/nxcompat.h": "public-api",
    "framework/nxcompat/src/nxcompat.c": "core-validation",
    "framework/nxcompat/src/nxcompat_internal.h": "core-internal-contract",
    "framework/nxcompat/src/nxcompat_backend.c": "backend-transaction",
    "framework/nxcompat/src/nxcompat_graphics.c": "legacy-graphics-report",
    "framework/nxcompat/src/nxcompat_plan.c": "plan-transaction",
    "framework/nxcompat/src/nxcompat_probe.c": "capability-probe",
    "framework/nxcompat/src/nxcompat_receipts.c": "typed-receipts",
    "framework/nxcompat/src/nxcompat_registry.c": "finite-registry",
    "framework/nxcompat/src/nxcompat_registry_internal.h": "registry-state",
    "framework/nxcompat/src/nxcompat_report.c": "sanitized-report",
    "framework/nxcompat/adapters/sdl2/nxcompat_sdl2.h":
        "sdl2-public-adapter",
    "framework/nxcompat/adapters/sdl2/nxcompat_sdl2.c":
        "sdl2-ownership-boundary",
    "framework/nxcompat/tests/test_nxcompat.c": "core-host-tests",
    "framework/nxcompat/tests/test_m12_probe.c": "probe-fixtures",
    "framework/nxcompat/tests/test_nxcompat_registry.c":
        "registry-receipt-tests",
    "framework/nxcompat/tests/test_sdl2_adapter.c": "sealed-sdl-tests",
    "framework/nxcompat/tests/test_install_smoke.c": "installed-link-smoke",
    "framework/nxcompat/tests/run-host.sh": "hermetic-host-gate",
    "framework/nxgl/CMakeLists.txt": "graphics-build-contract",
    "framework/nxgl/README.md": "graphics-boundary-documentation",
    "framework/nxgl/include/nxgl_nxcompat.h": "graphics-bridge-api",
    "framework/nxgl/src/nxgl_internal.h": "graphics-private-state",
    "framework/nxgl/src/nxgl_sdl2.c": "graphics-environment-transaction",
    "framework/nxgl/src/nxgl_nxcompat.c": "graphics-receipt-bridge",
    "framework/nxgl/tests/test_nxgl_environment.c":
        "sealed-graphics-environment-test",
    "framework/nxgl/tests/test_nxgl_nxcompat.c":
        "fake-graphics-bridge-test",
    "framework/nxinput/CMakeLists.txt": "input-build-contract",
    "framework/nxinput/README.md": "input-boundary-documentation",
    "framework/nxinput/include/nxinput_nxcompat.h": "input-bridge-api",
    "framework/nxinput/src/nxinput_nxcompat.c": "input-receipt-bridge",
    "framework/nxinput/tests/test_nxinput_nxcompat.c":
        "fake-input-bridge-test",
    "framework/nxinput/tests/static_no_device_name_fallback.sh":
        "input-name-policy-gate",
}

EXPECTED_CLOSURE = {
    "requirements_accounted": 20,
    "completed_requirement_count": 20,
    "deferred_requirement_count": 0,
    "master_milestone_complete": True,
    "host_gate_closed": True,
    "physical_device_evidence": False,
    "physical_followup_milestones": ["M12A", "M22"],
}

EXPECTED_EXECUTION_BOOLEANS = {
    "physical_device_evidence": False,
    "real_gpu_or_display_opened": False,
    "real_audio_device_opened": False,
    "real_controller_enumerated": False,
    "external_guest_code_executed": False,
    "hardware_ran": False,
    "device_access": False,
    "network_access": False,
}

EXPECTED_CAPABILITIES = (
    ("host.portmaster", "preflight", "probe", "observed", "observation"),
    ("host.armhf-libs", "preflight", "probe", "observed", "observation"),
    ("host.aarch64-libs", "preflight", "probe", "observed", "observation"),
    ("host.i386-libs", "preflight", "probe", "observed", "observation"),
    ("host.session-runtime", "preflight", "probe", "observed", "observation"),
    ("host.short-memory", "preflight", "probe", "observed", "observation"),
    ("host.fuse-like-filesystem", "preflight", "probe", "observed", "observation"),
    ("host.network-filesystem", "preflight", "probe", "observed", "observation"),
    ("host.fbdev", "preflight", "probe", "observed", "observation"),
    ("host.drm", "preflight", "probe", "observed", "observation"),
    ("host.drm-connected", "preflight", "probe", "observed", "observation"),
    ("host.wayland", "preflight", "probe", "observed", "observation"),
    ("host.x11", "preflight", "probe", "observed", "observation"),
    ("audio.pulse-socket", "preflight", "probe", "observed", "observation"),
    ("audio.pipewire-socket", "preflight", "probe", "observed", "observation"),
    ("audio.alsa", "preflight", "probe", "observed", "observation"),
    ("graphics.window", "graphics", "nxgl", "opened", "baseline-graphics"),
    ("graphics.gles2", "graphics", "nxgl", "opened", "baseline-graphics"),
    ("graphics.gles3", "graphics", "nxgl", "opened", "optional-enhancement"),
    ("graphics.egl", "graphics", "nxgl", "opened", "baseline-graphics"),
    ("graphics.egl-config", "graphics", "nxgl", "opened", "baseline-graphics"),
    ("graphics.drawable", "graphics", "nxgl", "opened", "baseline-graphics"),
    ("graphics.etc1", "graphics", "nxgl", "opened", "optional-enhancement"),
    ("graphics.etc2", "graphics", "nxgl", "opened", "optional-enhancement"),
    ("graphics.astc", "graphics", "nxgl", "opened", "optional-enhancement"),
    ("graphics.npot-full", "graphics", "nxgl", "opened", "optional-enhancement"),
    ("audio.output-open", "audio", "sdl2-audio", "opened", "port-declared"),
    ("input.controller-mapping", "input", "probe", "observed", "port-declared"),
    ("input.controller-api", "input", "nxinput", "active", "port-declared"),
    ("input.controller-connected", "input", "nxinput", "active", "optional-runtime"),
    ("input.hotplug", "input", "nxinput", "active", "optional-runtime"),
    ("audio.embedded-openal", "preflight", "probe", "observed", "port-declared"),
    # anexada no fim: espelho posicional do registry C, cujos ids ja'
    # viajaram em recibos gravados
    ("graphics.gles1", "graphics", "nxgl", "opened", "baseline-graphics"),
)

CAPABILITY_ENUMS = (
    "HOST_PORTMASTER", "HOST_ARMHF_LIBS", "HOST_AARCH64_LIBS",
    "HOST_I386_LIBS", "HOST_SESSION_RUNTIME", "HOST_SHORT_MEMORY",
    "HOST_FUSE_LIKE_FILESYSTEM", "HOST_NETWORK_FILESYSTEM", "HOST_FBDEV",
    "HOST_DRM", "HOST_DRM_CONNECTED", "HOST_WAYLAND", "HOST_X11",
    "AUDIO_PULSE_SOCKET", "AUDIO_PIPEWIRE_SOCKET", "AUDIO_ALSA",
    "GRAPHICS_WINDOW", "GRAPHICS_GLES2", "GRAPHICS_GLES3", "GRAPHICS_EGL",
    "GRAPHICS_EGL_CONFIG", "GRAPHICS_DRAWABLE", "GRAPHICS_ETC1",
    "GRAPHICS_ETC2", "GRAPHICS_ASTC", "GRAPHICS_NPOT_FULL",
    "AUDIO_OUTPUT_OPEN", "INPUT_CONTROLLER_MAPPING", "INPUT_CONTROLLER_API",
    "INPUT_CONTROLLER_CONNECTED", "INPUT_HOTPLUG",
    "AUDIO_EMBEDDED_OPENAL",
    "GRAPHICS_GLES1",
)

REASON_CODES = {
    "NXCOMPAT_REASON_NONE": 0,
    "NXCOMPAT_REASON_PROBE_COMPLETE": 1,
    "NXCOMPAT_REASON_INVALID_ARGUMENT": 100,
    "NXCOMPAT_REASON_UNSUPPORTED_API": 101,
    "NXCOMPAT_REASON_STRUCT_TOO_SMALL": 102,
    "NXCOMPAT_REASON_ARBITER_BUSY": 103,
    "NXCOMPAT_REASON_PROVIDER_CONTRACT": 104,
    "NXCOMPAT_REASON_OBSERVATION_ABSENT": 200,
    "NXCOMPAT_REASON_OBSERVATION_MALFORMED": 201,
    "NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE": 202,
    "NXCOMPAT_REASON_PROCESS_ARCH_VERIFIED": 210,
    "NXCOMPAT_REASON_KERNEL_ARCH_VERIFIED": 211,
    "NXCOMPAT_REASON_USERLAND_ARCH_VERIFIED": 212,
    "NXCOMPAT_REASON_MEMAVAILABLE_VERIFIED": 220,
    "NXCOMPAT_REASON_CGROUP_V1_LIMIT_VERIFIED": 221,
    "NXCOMPAT_REASON_CGROUP_V2_LIMIT_VERIFIED": 222,
    "NXCOMPAT_REASON_RLIMIT_AS_VERIFIED": 223,
    "NXCOMPAT_REASON_EFFECTIVE_MEMORY_MINIMUM": 224,
    "NXCOMPAT_REASON_EFFECTIVE_MEMORY_UNKNOWN": 225,
    "NXCOMPAT_REASON_LIMIT_UNBOUNDED": 226,
    "NXCOMPAT_REASON_BACKEND_INHERITED_OK": 300,
    "NXCOMPAT_REASON_BACKEND_AUTODETECT_OK": 301,
    "NXCOMPAT_REASON_BACKEND_ATTEMPT_RETRYABLE": 302,
    "NXCOMPAT_REASON_BACKEND_ATTEMPT_BUSY": 303,
    "NXCOMPAT_REASON_BACKEND_ATTEMPT_FATAL": 304,
    "NXCOMPAT_REASON_BACKEND_CLEANUP_FAILED": 305,
    "NXCOMPAT_REASON_ENV_CLEAR_FAILED": 306,
    "NXCOMPAT_REASON_ENV_RESTORE_FAILED": 307,
    "NXCOMPAT_REASON_BACKEND_NAME_REJECTED": 308,
    "NXCOMPAT_REASON_BACKEND_FAKE_OUTPUT": 309,
    "NXCOMPAT_REASON_VIDEO_OPEN_FAILED": 310,
    "NXCOMPAT_REASON_AUDIO_OPEN_FAILED": 311,
    "NXCOMPAT_REASON_BACKEND_FOREIGN_OWNED": 312,
    "NXCOMPAT_REASON_AUDIO_DEVICE_INVALID": 313,
    "NXCOMPAT_REASON_PLAN_CAPABILITY_UNAVAILABLE": 400,
    "NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED": 401,
    "NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED": 402,
    "NXCOMPAT_REASON_PLAN_POLICY_NOT_NEEDED": 403,
    "NXCOMPAT_REASON_ENV_LATE_VALUE_PRESERVED": 404,
    "NXCOMPAT_REASON_ENV_APPLIED": 405,
    "NXCOMPAT_REASON_ENV_APPLY_FAILED": 406,
    "NXCOMPAT_REASON_ENV_ROLLED_BACK": 407,
    "NXCOMPAT_REASON_ENV_ROLLBACK_FAILED": 408,
    "NXCOMPAT_REASON_PLAN_COMPLETE": 409,
    "NXCOMPAT_REASON_ENV_APPLY_COMPLETE": 410,
    "NXCOMPAT_REASON_CAPABILITY_PUBLISHED": 500,
    "NXCOMPAT_REASON_CAPABILITY_LOST": 501,
    "NXCOMPAT_REASON_CAPABILITY_STALE": 502,
    "NXCOMPAT_REASON_REQUIREMENT_UNKNOWN": 510,
    "NXCOMPAT_REASON_REQUIREMENT_DUPLICATE": 511,
    "NXCOMPAT_REASON_REQUIREMENT_PENDING": 512,
    "NXCOMPAT_REASON_REQUIREMENT_MISSING": 513,
    "NXCOMPAT_REASON_REQUIREMENT_SATISFIED": 514,
    "NXCOMPAT_REASON_GRAPHICS_WINDOW_OPENED": 520,
    "NXCOMPAT_REASON_GRAPHICS_GLES_OPENED": 521,
    "NXCOMPAT_REASON_GRAPHICS_EGL_OPENED": 522,
    "NXCOMPAT_REASON_GRAPHICS_EGL_CONFIG_OPENED": 523,
    "NXCOMPAT_REASON_GRAPHICS_DRAWABLE_OPENED": 524,
    "NXCOMPAT_REASON_AUDIO_OUTPUT_OPENED": 530,
    "NXCOMPAT_REASON_INPUT_CONTROLLER_API_ACTIVE": 540,
    "NXCOMPAT_REASON_INPUT_CONTROLLER_CONNECTED": 541,
    "NXCOMPAT_REASON_INPUT_HOTPLUG_ACTIVE": 542,
    "NXCOMPAT_REASON_INPUT_CONTROLLER_LOST": 543,
    "NXCOMPAT_REASON_INPUT_CONTROLLER_MAPPING_RESOLVED": 544,
    "NXCOMPAT_REASON_REPORT_SANITIZED": 550,
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
    pure = PurePosixPath(value)
    require(not pure.is_absolute() and ".." not in pure.parts,
            "evidence path escapes repository: %s" % value)
    require(str(pure) == value, "non-canonical evidence path: %s" % value)
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
    forbidden_modules = {
        "subprocess", "socket", "urllib", "http", "requests", "ftplib",
        "multiprocessing", "asyncio", "ctypes",
    }
    forbidden_calls = {
        "system", "popen", "fork", "forkpty", "execv", "execve",
        "execvp", "execvpe", "spawnl", "spawnle", "spawnlp", "spawnlpe",
        "spawnv", "spawnve", "spawnvp", "spawnvpe", "Popen",
    }
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            require(not any(alias.name.split(".")[0] in forbidden_modules
                            for alias in node.names),
                    "process/network import in %s" % relative)
        elif isinstance(node, ast.ImportFrom):
            require((node.module or "").split(".")[0] not in forbidden_modules,
                    "process/network import in %s" % relative)
        elif isinstance(node, ast.Call):
            if isinstance(node.func, ast.Attribute):
                require(node.func.attr not in forbidden_calls,
                        "process execution call in %s" % relative)
            elif isinstance(node.func, ast.Name):
                require(node.func.id not in forbidden_calls,
                        "process execution call in %s" % relative)


def validate_privacy(raw_text):
    lowered = raw_text.lower()
    for forbidden in ("/home/", "/mnt/", "/tmp/", "file://", "felipe"):
        require(forbidden not in lowered,
                "ledger contains a private path or identity token")
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
    return by_path, decoded


def validate_reference(reference, pins, decoded, requirement_id):
    require(isinstance(reference, dict) and
            set(reference) == {"path", "tokens"},
            "%s has malformed evidence" % requirement_id)
    path = reference.get("path")
    require(path in pins, "%s cites unpinned evidence: %s" %
            (requirement_id, path))
    require(path not in {AUDIT_RELATIVE, SELF_RELATIVE},
            "%s uses circular audit evidence" % requirement_id)
    tokens = reference.get("tokens")
    require(isinstance(tokens, list) and 1 <= len(tokens) <= 8 and
            len(tokens) == len(set(tokens)),
            "%s has an invalid evidence token list" % requirement_id)
    for token in tokens:
        require(isinstance(token, str) and 0 < len(token) <= 512,
                "%s has an invalid evidence token" % requirement_id)
        require(token in decoded[path],
                "%s token absent from %s: %r" %
                (requirement_id, path, token))


def validate_requirements(document, pins, decoded):
    requirements = document.get("requirements")
    require(isinstance(requirements, list) and len(requirements) == 20,
            "M12 must account for exactly 20 requirements")
    expected = {item[0]: item[1:] for item in REQUIREMENTS}
    require([item.get("id") for item in requirements] ==
            [item[0] for item in REQUIREMENTS],
            "M12 requirement order or IDs changed")
    for item in requirements:
        require(isinstance(item, dict) and set(item) == {
            "id", "requirement", "disposition", "claim", "implementation",
            "tests", "limitations",
        }, "malformed requirement entry")
        requirement_id = item["id"]
        wanted_requirement, wanted_disposition = expected[requirement_id]
        require(item["requirement"] == wanted_requirement,
                "%s requirement text changed" % requirement_id)
        require(item["disposition"] == wanted_disposition,
                "%s disposition changed" % requirement_id)
        require(isinstance(item["claim"], str) and
                40 <= len(item["claim"]) <= 800,
                "%s has an invalid claim" % requirement_id)
        require(isinstance(item["implementation"], list) and
                item["implementation"],
                "%s lacks implementation evidence" % requirement_id)
        require(isinstance(item["tests"], list) and item["tests"],
                "%s lacks test evidence" % requirement_id)
        require(isinstance(item["limitations"], list) and
                all(isinstance(value, str) and value
                    for value in item["limitations"]),
                "%s has malformed limitations" % requirement_id)
        for reference in item["implementation"] + item["tests"]:
            validate_reference(reference, pins, decoded, requirement_id)

    by_id = {item["id"]: item for item in requirements}
    boundary_words = {
        "M12-003": ("fake boundary", "not physical gpu evidence"),
        "M12-004": ("host coverage", "does not claim a real display"),
        "M12-006": ("injected sealed", "opens no real audio device"),
        "M12-007": ("test-owned", "without native controller enumeration"),
        "M12-017": ("sealed host boundary", "not during a physical"),
        "M12-020": ("host-only", "no physical-device evidence"),
    }
    for requirement_id, words in boundary_words.items():
        combined = (by_id[requirement_id]["claim"] + " " +
                    " ".join(by_id[requirement_id]["limitations"])).lower()
        for word in words:
            require(word in combined,
                    "%s overclaims boundary evidence: missing %r" %
                    (requirement_id, word))


def extract_enum(text, enum_name):
    pattern = (r"typedef\s+enum\s+" + re.escape(enum_name) +
               r"\s*\{(?P<body>.*?)\}\s*[A-Za-z_][A-Za-z0-9_]*\s*;")
    match = re.search(pattern, text, re.DOTALL)
    require(match is not None, "missing enum %s" % enum_name)
    return match.group("body")


def validate_api_and_reasons(decoded):
    header = decoded["framework/nxcompat/include/nxcompat.h"]
    version = decoded["framework/nxcompat/VERSION"].strip()
    contract = load_json("framework/contracts/declarative-v1.json")
    require(version == "0.5.3" and '#define NXCOMPAT_VERSION "0.5.3"' in header,
            "nxcompat version contract changed")
    require("#define NXCOMPAT_API_VERSION_V1 1u" in header and
            "#define NXCOMPAT_API_VERSION_V2 2u" in header and
            "#define NXCOMPAT_API_VERSION NXCOMPAT_API_VERSION_V2" in header,
            "API 2/current or API 1 compatibility macros changed")
    require("#define NXCOMPAT_PROBE_OPTIONS_V1_SIZE" in header and
            "API-v1 discovery remains layout-compatible" in header,
            "API 1 prefix/discovery compatibility declaration changed")
    components = {item.get("id"): item
                  for item in contract.get("components", [])}
    nxcompat = components.get("nxcompat", {})
    registry = contract.get("nxport", {}).get("capability_registry", {})
    require(contract.get("schema_version") == 1 and
            nxcompat.get("current_version") == "0.5.3" and
            nxcompat.get("api_version") == 2 and
            nxcompat.get("version_file") == "framework/nxcompat/VERSION",
            "framework nxcompat contract changed")
    require(registry == {
        "path": "framework/nxcompat/capabilities-v1.json",
        "schema_version": 1,
        "registry_version": "1.0.0",
        "count": 33,
        "default_required": False,
    }, "framework capability registry contract changed")

    body = extract_enum(header, "nxcompat_reason_code")
    parsed = {name: int(value) for name, value in re.findall(
        r"\b(NXCOMPAT_REASON_[A-Z0-9_]+)\s*=\s*([0-9]+)\b", body)}
    require(parsed == REASON_CODES, "public reason-code map changed")


def validate_capability_registry(decoded):
    document = load_json("framework/nxcompat/capabilities-v1.json")
    require(set(document) == {
        "schema_version", "registry_version", "default_required", "states",
        "phases", "sources", "roles", "capabilities",
    }, "capability registry top-level fields changed")
    require(document["schema_version"] == 1 and
            document["registry_version"] == "1.0.0" and
            document["default_required"] is False,
            "capability registry identity changed")
    require(document["states"] ==
            ["absent", "observed", "opened", "active", "lost"] and
            document["phases"] ==
            ["preflight", "graphics", "audio", "input", "ready"] and
            document["sources"] ==
            ["probe", "nxgl", "sdl2-audio", "nxinput", "engine-adapter"] and
            document["roles"] == [
                "observation", "baseline-graphics", "port-declared",
                "optional-enhancement", "optional-runtime"],
            "capability registry vocabulary changed")
    capabilities = document["capabilities"]
    require(isinstance(capabilities, list) and len(capabilities) == 33,
            "capability count changed")
    observed = []
    for item in capabilities:
        require(isinstance(item, dict) and set(item) == {
            "id", "phase", "source", "minimum_evidence", "role"},
            "malformed capability entry")
        observed.append((item["id"], item["phase"], item["source"],
                         item["minimum_evidence"], item["role"]))
    require(tuple(observed) == EXPECTED_CAPABILITIES,
            "capability IDs, order or metadata changed")

    header = decoded["framework/nxcompat/include/nxcompat.h"]
    body = extract_enum(header, "nxcompat_capability_id_v1")
    enum_entries = re.findall(
        r"\bNXCOMPAT_CAPABILITY_([A-Z0-9_]+)\s*=\s*([0-9]+)\b", body)
    require(tuple(name for name, _value in enum_entries) == CAPABILITY_ENUMS and
            tuple(int(value) for _name, value in enum_entries) == tuple(range(33)),
            "capability public numeric enum changed")
    require("#define NXCOMPAT_CAPABILITY_COUNT 33u" in header,
            "public capability count changed")

    registry_source = decoded["framework/nxcompat/src/nxcompat_registry.c"]
    rows = re.findall(
        r"\{\s*([0-9]+)u,\s*\"([^\"]+)\",\s*"
        r"(NXCOMPAT_PHASE_[A-Z0-9_]+),\s*"
        r"(NXCOMPAT_SOURCE_[A-Z0-9_]+),\s*"
        r"(NXCOMPAT_EVIDENCE_[A-Z0-9_]+),\s*"
        r"(NXCOMPAT_ROLE_[A-Z0-9_]+)\s*\}",
        registry_source, re.DOTALL)
    phase_map = {value: "NXCOMPAT_PHASE_" + value.upper()
                 for value in document["phases"]}
    source_map = {value: "NXCOMPAT_SOURCE_" + value.upper().replace("-", "_")
                  for value in document["sources"]}
    evidence_map = {value: "NXCOMPAT_EVIDENCE_" + value.upper()
                    for value in document["states"]}
    role_map = {value: "NXCOMPAT_ROLE_" + value.upper().replace("-", "_")
                for value in document["roles"]}
    expected_rows = tuple(
        (str(index), item[0], phase_map[item[1]], source_map[item[2]],
         evidence_map[item[3]], role_map[item[4]])
        for index, item in enumerate(EXPECTED_CAPABILITIES))
    require(tuple(rows) == expected_rows,
            "C registry is not an exact positional mirror of capability JSON")
    require("device_model" not in registry_source and
            "os_id" not in registry_source and "firmware" not in registry_source,
            "finite registry contains a label-based policy input")


def require_tokens(text, tokens, label):
    for token in tokens:
        require(token in text, "%s missing token %r" % (label, token))


def validate_build_and_boundaries(decoded):
    cmake = decoded["framework/nxcompat/CMakeLists.txt"]
    nxgl_cmake = decoded["framework/nxgl/CMakeLists.txt"]
    nxinput_cmake = decoded["framework/nxinput/CMakeLists.txt"]
    runner = decoded["framework/nxcompat/tests/run-host.sh"]
    backend = decoded["framework/nxcompat/src/nxcompat_backend.c"]
    plan = decoded["framework/nxcompat/src/nxcompat_plan.c"]
    receipts = decoded["framework/nxcompat/src/nxcompat_receipts.c"]
    sdl = decoded["framework/nxcompat/adapters/sdl2/nxcompat_sdl2.c"]
    nxgl = decoded["framework/nxgl/src/nxgl_nxcompat.c"]
    nxinput = decoded["framework/nxinput/src/nxinput_nxcompat.c"]

    require_tokens(cmake, [
        "add_library(nxcompat STATIC ${NXCOMPAT_CORE_SOURCES})",
        "add_library(nxcompat-test-core STATIC ${NXCOMPAT_CORE_SOURCES})",
        "NXCOMPAT_CORE_TESTING=1", "NXCOMPAT_SDL2_TESTING=1",
        "install(TARGETS nxcompat", "install(FILES capabilities-v1.json",
    ], "nxcompat CMake")
    require("FORCE" not in cmake and "FORCE" not in nxgl_cmake and
            "FORCE" not in nxinput_cmake,
            "embedded CMake cache option uses FORCE")
    require_tokens(nxgl_cmake, [
        'option(NXGL_BUILD_NATIVE_TESTS',
        '"Build tests that link the native SDL/GL implementation" OFF)',
        "add_test(NAME nxgl-nxcompat COMMAND test-nxgl-nxcompat)",
    ], "nxgl CMake")
    require_tokens(nxinput_cmake, [
        'option(NXINPUT_BUILD_NATIVE_TESTS',
        '"Build tests that enumerate the native SDL controller subsystem" OFF)',
        "add_test(NAME nxinput-nxcompat COMMAND test-nxinput-nxcompat)",
    ], "nxinput CMake")

    require_tokens(backend, [
        "Only at this point may inherited hints be removed",
        "nxcompat_snapshot_environment", "nxcompat_restore_environment",
        "nxcompat_global_arbiter_try_acquire",
    ], "backend transaction")
    require(backend.count("outcome = nxcompat_attempt_once(options, result, &report)")
            == 2, "API 2 backend no longer has an exact two-attempt ceiling")
    require_tokens(plan, [
        "nxcompat_global_arbiter_try_acquire", "NXCOMPAT_ACTION_V2_ROLLED_BACK",
        "NXCOMPAT_REASON_ENV_ROLLBACK_FAILED",
    ], "plan transaction")
    require_tokens(receipts, [
        "nxcompat_registry_publish_graphics_ex",
        "nxcompat_registry_publish_audio_ex",
        "nxcompat_registry_publish_input_ex",
        "NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE",
        "NXCOMPAT_INPUT_PROOF_RESCAN_ACTIVE",
    ], "receipt publishers")
    require_tokens(sdl, [
        "A test build is a sealed fake boundary",
        "SDL_OpenAudioDevice", "SDL_CloseAudioDevice",
        "NXCOMPAT_REASON_BACKEND_FAKE_OUTPUT",
    ], "SDL boundary")
    require_tokens(nxgl, [
        "SDL_GL_GetCurrentContext() != sdl_context",
        "NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL",
        "NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED",
        "NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE",
    ], "nxgl receipt bridge")
    require_tokens(nxinput, [
        "NXCOMPAT_INPUT_PROOF_CONTROLLER_SUBSYSTEM_ACTIVE",
        "NXCOMPAT_INPUT_PROOF_INITIAL_SCAN_DONE",
        "NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE",
    ], "nxinput receipt bridge")

    require_tokens(runner, [
        "SDL_AUDIODRIVER=dummy", "SDL_VIDEODRIVER=dummy",
        "NXGL_BUILD_NATIVE_TESTS=OFF", "NXINPUT_BUILD_NATIVE_TESTS=OFF",
        "SDL_(OpenAudioDevice|CloseAudioDevice|GetNumAudioDevices)",
        "SDL_(InitSubSystem|CreateWindow|GL_CreateContext)",
        "SDL_(NumJoysticks|GameControllerOpen|JoystickOpen)",
        "guest_code_executed=0", "hardware_ran=0", "device_access=0",
        "network_access=0",
    ], "hermetic host runner")
    require(runner.index('if nm -u "$build/test-nxcompat-sdl2"') <
            runner.index('ctest --test-dir "$build" --output-on-failure'),
            "SDL symbol barrier runs after nxcompat CTest")
    nxgl_ctest = runner.index("-R '^nxgl-(environment|nxcompat)$'")
    require(runner.index('if nm -u "$build/test-nxgl-nxcompat"') <
            nxgl_ctest,
            "graphics symbol barrier runs after nxgl CTest")
    require(runner.index('if nm -u "$build/test-nxgl-environment"') <
            nxgl_ctest,
            "video environment symbol barrier runs after nxgl CTest")
    require_tokens(runner, [
        'readelf -d "$build/test-nxgl-environment"',
        "nxgl environment fixture can reach a graphics API",
        "nxgl environment fixture links a graphics provider",
    ], "sealed nxgl environment fixture")
    require(runner.index('if nm -u "$build/test-nxinput-nxcompat"') <
            runner.index("-R '^(nxinput-static-gate|nxinput-nxcompat|nxinput-evdev-chord)$'"),
            "input symbol barrier runs after nxinput CTest")
    smoke = decoded["framework/nxcompat/tests/test_install_smoke.c"]
    require_tokens(smoke, [
        "Never execute it", "nxcompat_reason_name",
        "nxcompat_sdl2_negotiate_audio_v2", "nxgl_open_options_init",
        "nxgl_nxcompat_publish_context", "nxinput_config_init",
        "nxinput_nxcompat_publish_context",
    ], "installed link-only smoke")


def validate_matrix_and_runner(decoded):
    matrix = load_json("framework/tests/test-matrix-v1.json")
    gates = matrix.get("gates")
    require(isinstance(gates, list), "matrix gates missing")
    require(all(isinstance(gate, dict) and isinstance(gate.get("id"), str)
                for gate in gates), "malformed matrix gate")
    require(len({gate["id"] for gate in gates}) == len(gates),
            "duplicate matrix gate ID")
    by_id = {gate["id"]: gate for gate in gates}
    audit = by_id.get("nxcompat-m12-audit", {})
    require(audit.get("class") == "pure" and
            audit.get("command") ==
            ["python3", "-B", SELF_RELATIVE] and
            audit.get("sources") == [SELF_RELATIVE] and
            audit.get("support_files") == [AUDIT_RELATIVE] and
            audit.get("automatic") is True and audit.get("logged") is True and
            audit.get("namespace_required") is False and
            audit.get("signals") == [],
            "nxcompat M12 audit is not a canonical automatic pure gate")
    host = by_id.get("nxcompat-host", {})
    require(host.get("class") == "filesystem" and
            host.get("command") == ["bash", "framework/nxcompat/tests/run-host.sh"] and
            host.get("automatic") is True and host.get("signals") == [],
            "nxcompat host gate matrix contract changed")
    nxgl_native = by_id.get("nxgl-native", {})
    require(nxgl_native.get("automatic") is False and
            nxgl_native.get("command") is None,
            "nxgl native test became automatic")
    nxinput_native = by_id.get("nxinput-native", {})
    require(nxinput_native.get("class") == "hardware" and
            nxinput_native.get("automatic") is False and
            nxinput_native.get("command") is None,
            "nxinput native hardware test became automatic")

    safe_runner = decoded["framework/tests/run-safe-gates.sh"]
    require_tokens(safe_runner, [
        "run_gate nxcompat-host", "run_gate nxcompat-m12-audit",
        "python3 -B framework/nxcompat/tests/test_m12_audit.py",
    ], "canonical safe runner")


def validate_document(document, raw_text):
    require(isinstance(document, dict) and set(document) == {
        "schema", "schema_version", "milestone", "scope", "closure",
        "execution_claims", "evidence_pins", "requirements",
    }, "audit top-level fields changed")
    require(document["schema"] == "nxcompat-m12-master-audit-v1" and
            document["schema_version"] == 1 and
            document["milestone"] == "M12",
            "audit identity changed")
    require(document["scope"] ==
            "capability-probe-transactional-plan-typed-receipts-and-sanitized-runtime-report",
            "audit scope changed")
    require(document["closure"] == EXPECTED_CLOSURE,
            "M12 closure accounting changed")
    execution = document["execution_claims"]
    require(isinstance(execution, dict) and set(execution) == {
        "host_gate_scope", "graphics_evidence", "audio_evidence",
        "input_evidence", *EXPECTED_EXECUTION_BOOLEANS.keys(),
    }, "execution claim fields changed")
    for key, value in EXPECTED_EXECUTION_BOOLEANS.items():
        require(execution.get(key) is value,
                "execution claim overstates %s" % key)
    for key in ("host_gate_scope", "graphics_evidence", "audio_evidence",
                "input_evidence"):
        require(isinstance(execution[key], str) and len(execution[key]) >= 40,
                "execution boundary is not explicit: %s" % key)
    combined = " ".join(execution.values() if False else [
        execution["host_gate_scope"], execution["graphics_evidence"],
        execution["audio_evidence"], execution["input_evidence"]]).lower()
    require("fake" in combined and "test-owned" in combined and
            "sealed" in combined,
            "synthetic/fake execution boundaries are not explicit")
    validate_privacy(raw_text)


def main():
    validate_process_free_python(SELF_RELATIVE)
    raw = read_bytes(AUDIT_RELATIVE)
    raw_text = raw.decode("utf-8")
    document = load_json_bytes(raw, AUDIT_RELATIVE)
    validate_document(document, raw_text)
    pins, decoded = validate_pins(document)
    validate_api_and_reasons(decoded)
    validate_capability_registry(decoded)
    validate_build_and_boundaries(decoded)
    validate_matrix_and_runner(decoded)
    validate_requirements(document, pins, decoded)
    print("nxcompat_m12_audit=PASS requirements=20 capabilities=31 "
          "api_current=2 api_v1_compatible=1 physical_device_evidence=0 "
          "guest_code_executed=0 hardware_ran=0 device_access=0 "
          "network_access=0 process_free=1")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GateError as error:
        print("nxcompat_m12_audit=FAIL %s" % error, file=sys.stderr)
        raise SystemExit(1)
