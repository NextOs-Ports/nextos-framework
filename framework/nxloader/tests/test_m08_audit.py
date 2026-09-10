#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free completeness gate for the common nxloader / M08 contract."""

import json
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
LOADER_ROOT = REPO_ROOT / "framework/nxloader"
AUDIT_PATH = LOADER_ROOT / "m08-audit-v1.json"
CONTRACT_PATH = REPO_ROOT / "framework/contracts/declarative-v1.json"
MATRIX_PATH = REPO_ROOT / "framework/tests/test-matrix-v1.json"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_json(path):
    def no_duplicates(pairs):
        value = {}
        for key, item in pairs:
            require(key not in value,
                    "duplicate JSON key %s in %s" % (key, path))
            value[key] = item
        return value

    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=no_duplicates)


def validate_evidence(audit):
    requirements = audit.get("requirements")
    expected_ids = ["M08-%03d" % number for number in range(1, 21)]
    require(isinstance(requirements, list) and
            [item.get("id") for item in requirements] == expected_ids,
            "M08 audit IDs are incomplete or reordered")
    for item in requirements:
        require(set(item) == {"id", "implementation", "tests"},
                "%s has an unknown field" % item.get("id"))
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
                        "%s evidence escapes the repository" % item["id"])
                evidence = REPO_ROOT / relative
                require(evidence.is_file() and not evidence.is_symlink(),
                        "%s evidence is missing/linked: %s" %
                        (item["id"], relative))
                token = reference.get("token")
                require(isinstance(token, str) and token and
                        token in evidence.read_text(encoding="utf-8"),
                        "%s token is absent from %s: %r" %
                        (item["id"], relative, token))


def validate_contract():
    require((LOADER_ROOT / "VERSION").read_text(
        encoding="utf-8").strip() == "0.9.0",
        "nxloader VERSION is not the reviewed 0.9.0 successor")
    header = (LOADER_ROOT / "include/nxloader.h").read_text(encoding="utf-8")
    require('#define NXLOADER_VERSION_STRING "0.9.0"' in header and
            "#define NXLOADER_API_VERSION_MAJOR 1u" in header and
            "#define NXLOADER_API_VERSION_MINOR 3u" in header,
            "public nxloader successor version/API is incoherent")
    legacy_values = (
        "NXLOADER_OK = 0", "NXLOADER_EINVAL = -1",
        "NXLOADER_ENOMEM = -2", "NXLOADER_EIO = -3",
        "NXLOADER_EFORMAT = -4", "NXLOADER_EARCH = -5",
        "NXLOADER_EBOUNDS = -6", "NXLOADER_ESTATE = -7",
        "NXLOADER_EPROTECT = -8", "NXLOADER_ERELOC = -9",
        "NXLOADER_EUNRESOLVED = -10", "NXLOADER_ECOLLISION = -11",
        "NXLOADER_EOVERFLOW = -12", "NXLOADER_EUNSUPPORTED = -13",
        "NXLOADER_ECALLBACK = -14", "NXLOADER_STATE_EMPTY = 0",
        "NXLOADER_STATE_LOADED = 1", "NXLOADER_STATE_RELOCATED = 2",
        "NXLOADER_STATE_RESOLVED = 3", "NXLOADER_STATE_FINALIZED = 4",
        "NXLOADER_STATE_INITIALIZED = 5", "NXLOADER_STATE_ERROR = 6",
    )
    require(all(token in header for token in legacy_values) and
            "NXLOADER_EREENTRANT = -15" in header and
            "NXLOADER_STATE_INITIALIZING = 7" in header and
            "NXLOADER_STATE_JNI_LOADING = 8" in header and
            "NXLOADER_STATE_READY = 9" in header and
            "NXLOADER_STATE_PATCHING = 10" in header,
            "API 1.3 renumbered an M08 value or failed to append M11 values")
    cmake = (LOADER_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require("project(nxloader VERSION 0.9.0" in cmake and
            "NXLOADER_BUILD_FUZZER" in cmake and
            "fuzzer-no-link" in cmake,
            "CMake version or parser instrumentation is incomplete")
    contract = load_json(CONTRACT_PATH)
    by_id = {item["id"]: item for item in contract.get("components", [])}
    require(contract.get("schema_version") == 1 and
            by_id.get("nxloader", {}).get("current_version") == "0.9.0" and
            by_id.get("nxloader", {}).get("api_version") == 1,
            "integrated declarative 0.9.0 contract is incoherent")


def validate_independence_and_safety():
    parser32 = (LOADER_ROOT / "src/nxloader_elf32.c").read_text(
        encoding="utf-8")
    parser64 = (LOADER_ROOT / "src/nxloader_elf64.c").read_text(
        encoding="utf-8")
    parser_text = parser32 + parser64
    require("SHT_" not in parser_text and "e_shoff" not in parser_text and
            "e_shnum" not in parser_text and "e_shentsize" not in parser_text,
            "runtime parser regained a section-header dependency")
    loader_sources = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((LOADER_ROOT / "src").glob("*.c"))
    )
    require("RTLD_DEFAULT" not in loader_sources and
            "dlsym(" not in loader_sources,
            "nxloader regained implicit process-global symbol lookup")
    fuzz = (LOADER_ROOT / "tests/test_fuzz_nxloader.c").read_text(
        encoding="utf-8")
    require("LLVMFuzzerTestOneInput" in fuzz and
            "nxloader_module_call_initializers" not in fuzz,
            "fuzzer is missing or can execute guest initializers")
    runner = LOADER_ROOT / "tests/run-host.sh"
    runner_text = runner.read_text(encoding="utf-8")
    require(runner.stat().st_mode & 0o111 and
            "-runs=20000" in runner_text and
            "hardware_ran=0 device_access=0" in runner_text,
            "host gate is not executable, bounded or explicit about devices")


def validate_matrix():
    matrix = load_json(MATRIX_PATH)
    gates = {item["id"]: item for item in matrix.get("gates", [])}
    require(gates.get("nxloader-audit", {}).get("class") == "pure" and
            gates.get("nxloader-audit", {}).get("automatic") is True,
            "M08 static audit is not an automatic pure gate")
    require(gates.get("nxloader-host", {}).get("class") == "filesystem" and
            gates.get("nxloader-host", {}).get("automatic") is True and
            gates.get("nxloader-host", {}).get("signals") == [],
            "nxloader host suite is not a signal-free filesystem gate")


def main():
    audit = load_json(AUDIT_PATH)
    require(set(audit) == {
        "schema_version", "milestone", "scope", "physical_device_evidence",
        "guest_initializers_executed", "requirements",
    }, "M08 audit has an unknown top-level field")
    require(audit.get("schema_version") == 1 and
            audit.get("milestone") == "M08" and
            audit.get("scope") ==
            "host-parser-registry-protection-and-approved-reference-inspection",
            "M08 audit header changed")
    require(audit.get("physical_device_evidence") is False and
            audit.get("guest_initializers_executed") is False,
            "M08 audit overclaims device or guest execution evidence")
    audit_text = AUDIT_PATH.read_text(encoding="utf-8")
    require(re.search(
        r"(?:^|[^0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?:[^0-9]|$)",
        audit_text) is None, "M08 audit contains an address")
    validate_evidence(audit)
    validate_contract()
    validate_independence_and_safety()
    validate_matrix()
    print("M08 audit gate passed: 20 requirements, sectionless parsers, "
          "physical_device_evidence=0 guest_initializers_executed=0")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, json.JSONDecodeError) as error:
        print("M08 audit gate failed: %s" % error)
        raise SystemExit(1)
