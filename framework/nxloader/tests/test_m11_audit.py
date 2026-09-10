#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free completeness gate for nxloader lifecycle milestone M11."""

import json
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
LOADER_ROOT = REPO_ROOT / "framework/nxloader"
AUDIT_PATH = LOADER_ROOT / "m11-audit-v1.json"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_json(path):
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result,
                    "duplicate JSON key %s in %s" % (key, path))
            result[key] = value
        return result

    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=no_duplicates)


def validate_evidence(audit):
    requirements = audit.get("requirements")
    expected_ids = ["M11-NXL-%03d" % number for number in range(1, 11)]
    require(isinstance(requirements, list) and
            [item.get("id") for item in requirements] == expected_ids,
            "M11 requirement IDs are incomplete or reordered")
    for item in requirements:
        require(set(item) == {"id", "implementation", "tests"},
                "%s has unknown fields" % item.get("id"))
        for group in ("implementation", "tests"):
            references = item.get(group)
            require(isinstance(references, list) and references,
                    "%s lacks %s evidence" % (item["id"], group))
            for reference in references:
                require(set(reference) == {"path", "token"},
                        "%s has malformed evidence" % item["id"])
                relative = Path(reference.get("path", ""))
                require(not relative.is_absolute() and
                        ".." not in relative.parts,
                        "%s evidence escapes repository" % item["id"])
                path = REPO_ROOT / relative
                require(path.is_file() and not path.is_symlink(),
                        "%s evidence missing/linked: %s" %
                        (item["id"], relative))
                token = reference.get("token")
                text = path.read_text(encoding="utf-8")
                superseded_version = False
                if (item["id"] == "M11-NXL-001" and
                        str(relative) == "framework/nxloader/VERSION" and
                        token == "0.8.0" and token not in text):
                    closure = load_json(
                        LOADER_ROOT / "release-0.9.0-closure-v1.json")
                    superseded_version = (
                        text.strip() == "0.9.0" and
                        closure.get("version") == "0.9.0" and
                        closure.get("api") == {"major": 1, "minor": 3} and
                        closure.get("safety", {}).get(
                            "api_080_preserved") is True)
                require(isinstance(token, str) and token and
                        (token in text or superseded_version),
                        "%s token absent from %s: %r" %
                        (item["id"], relative, token))


def validate_version_and_stable_values():
    version = (LOADER_ROOT / "VERSION").read_text(encoding="utf-8").strip()
    header = (LOADER_ROOT / "include/nxloader.h").read_text(encoding="utf-8")
    cmake = (LOADER_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require(version == "0.9.0", "M11 VERSION is not 0.9.0")
    for token in (
            '#define NXLOADER_VERSION_STRING "0.9.0"',
            "#define NXLOADER_API_VERSION_MAJOR 1u",
            "#define NXLOADER_API_VERSION_MINOR 3u",
            "NXLOADER_ECALLBACK = -14",
            "NXLOADER_EREENTRANT = -15",
            "NXLOADER_STATE_EMPTY = 0",
            "NXLOADER_STATE_INITIALIZED = 5",
            "NXLOADER_STATE_ERROR = 6",
            "NXLOADER_STATE_INITIALIZING = 7",
            "NXLOADER_STATE_JNI_LOADING = 8",
            "NXLOADER_STATE_READY = 9"):
        require(token in header, "public API/state token missing: %s" % token)
    require("project(nxloader VERSION 0.9.0" in cmake,
            "CMake project version is stale")


def function_body(source, signature, next_signature):
    start = source.find(signature)
    require(start >= 0, "function is missing: %s" % signature)
    end = source.find(next_signature, start + len(signature))
    require(end > start, "function boundary is missing: %s" % signature)
    return source[start:end]


def validate_callback_enforcement():
    common = (LOADER_ROOT / "src/nxloader.c").read_text(encoding="utf-8")
    hooks = (LOADER_ROOT / "src/nxloader_hooks.c").read_text(encoding="utf-8")
    registry = (LOADER_ROOT / "src/nxloader_registry.c").read_text(
        encoding="utf-8")
    parser32 = (LOADER_ROOT / "src/nxloader_elf32.c").read_text(
        encoding="utf-8")
    parser64 = (LOADER_ROOT / "src/nxloader_elf64.c").read_text(
        encoding="utf-8")
    protect = (LOADER_ROOT / "src/nxloader_protect.c").read_text(
        encoding="utf-8")
    require(common.count("nxloader_module_callback_guard(module)") >= 18,
            "same-module public API guard coverage regressed")
    require("nxloader_module_callback_guard(module)" in hooks and
            "nxloader_module_callback_guard(module)" in registry,
            "hook or add_module escaped callback guard")
    for token in ("module->callback_active = 1",
                  "module->callback_violation = 1",
                  "return violated ? NXLOADER_EREENTRANT : NXLOADER_OK",
                  "module->config.log(module->config.userdata, level, buffer)"):
        require(token in common, "callback guard token missing: %s" % token)
    for source, label in ((parser32, "ELF32"), (parser64, "ELF64"),
                          (protect, "protect")):
        require("log_result" in source and
                "NXLOADER_EREENTRANT" in common,
                "%s does not propagate guarded logging" % label)
    resolve = function_body(common, "nxloader_result nxloader_module_resolve(",
                            "nxloader_result nxloader_module_finalize(")
    require("nxloader_resolution_report local_report" in resolve and
            "result != NXLOADER_EREENTRANT" in resolve,
            "resolve report is not staged across reentrant failure")


def validate_initializer_preflight():
    for filename, plan_token in (("nxloader_elf32.c",
                                  "nxloader_initializer_plan32"),
                                 ("nxloader_elf64.c",
                                  "nxloader_initializer_plan64")):
        source = (LOADER_ROOT / "src" / filename).read_text(encoding="utf-8")
        start = source.find("nxloader_result nxloader_call_initializers_")
        require(start >= 0, "%s initializer entry is missing" % filename)
        body = source[start:]
        dt_init = body.find("if (module->dynamic.init_vma)")
        init_array = body.find("for (index = 0;")
        transient = body.find("module->state = NXLOADER_STATE_INITIALIZING")
        execute = body.find("function();")
        require(plan_token in source and
                0 <= dt_init < init_array < transient < execute,
                "%s no longer preflights DT_INIT then INIT_ARRAY" % filename)
        require("nxloader_apply_initializer_filter" in source and
                "nxloader_process_arch()" in body,
                "%s initializer validation is incomplete" % filename)


def validate_jni_boundary():
    common = (LOADER_ROOT / "src/nxloader.c").read_text(encoding="utf-8")
    body = function_body(common,
                         "nxloader_result nxloader_module_call_jni_onload(",
                         "nxloader_result nxloader_module_get_info(")
    for token in ("options->java_vm", "options->reserved",
                  "accepted_version_count",
                  "NXLOADER_JNI_ONLOAD_MAX_ACCEPTED_VERSIONS",
                  'nxloader_find_export_elf64(module, "JNI_OnLoad"',
                  'nxloader_find_export_elf32(module, "JNI_OnLoad"',
                  "NXLOADER_JNI_ONLOAD_OPTIONAL",
                  "NXLOADER_STATE_JNI_LOADING",
                  "NXLOADER_STATE_READY"):
        require(token in body, "JNI boundary token missing: %s" % token)
    require("nxloader_apply_alias" not in body,
            "JNI_OnLoad lookup must not pass through alias callback")
    require(body.find("module->state != NXLOADER_STATE_INITIALIZED") <
            body.find("function(options->java_vm, NULL)"),
            "JNI dispatch is not gated after INITIALIZED")


def validate_synthetic_tests_and_scope(audit):
    tests = (LOADER_ROOT / "tests/test_nxloader.c").read_text(encoding="utf-8")
    for token in ("test_m11_log_reentrancy_guard();",
                  "test_m11_relocation_and_alias_reentrancy();",
                  "test_m11_initializer_preflight_and_reentrancy();",
                  "test_m11_jni_onload_contract();",
                  "test_m11_jni_onload_option_validation();",
                  "M11_INITIALIZER_SKIP_ALL",
                  "M11_INITIALIZER_RUN_THEN_REJECT"):
        require(token in tests, "M11 synthetic regression missing: %s" % token)
    require("static int32_t JNI_OnLoad" not in tests and
            "static int JNI_OnLoad" not in tests,
            "host tests must not execute a synthetic JNI_OnLoad")
    require(audit.get("physical_device_evidence") is False and
            audit.get("guest_initializers_executed") is False and
            audit.get("guest_jni_onload_executed") is False,
            "M11 audit overclaims guest/device execution")
    privacy_text = "\n".join(
        path.read_text(encoding="utf-8") for path in
        (AUDIT_PATH, LOADER_ROOT / "README.md",
         LOADER_ROOT / "REFERENCE-AUDIT.md"))
    require(re.search(r"(?:192\.168\.|10\.[0-9]+\.|/home/|/mnt/)",
                      privacy_text) is None,
            "M11 public docs/audit contain a local address or path")


def main():
    audit = load_json(AUDIT_PATH)
    require(audit.get("schema_version") == 1 and
            audit.get("milestone") == "M11",
            "M11 audit header is invalid")
    validate_evidence(audit)
    validate_version_and_stable_values()
    validate_callback_enforcement()
    validate_initializer_preflight()
    validate_jni_boundary()
    validate_synthetic_tests_and_scope(audit)
    print("nxloader M11 subgate passed: API 1.3, guarded callbacks, "
          "preflight, JNI")


if __name__ == "__main__":
    main()
