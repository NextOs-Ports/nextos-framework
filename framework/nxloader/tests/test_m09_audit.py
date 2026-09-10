#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free completeness gate for nxloader ARMv7 / milestone M09."""

import hashlib
import json
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
LOADER_ROOT = REPO_ROOT / "framework/nxloader"
AUDIT_PATH = LOADER_ROOT / "m09-audit-v1.json"
PHYSICAL_PATH = LOADER_ROOT / "m09-armv7-physical-evidence-v1.json"
MATRIX_PATH = REPO_ROOT / "framework/tests/test-matrix-v1.json"
ABI_DEFINITIONS = REPO_ROOT / "framework/catalog/abi-check-definitions-v1.json"
ABI_VARIANTS = REPO_ROOT / "framework/catalog/abi-variants-v1.json"
ABI_ROWS = REPO_ROOT / "framework/catalog/abi-checks-v1.tsv"
M09_PRIVACY_FILES = (
    LOADER_ROOT / "README.md",
    LOADER_ROOT / "REFERENCE-AUDIT.md",
    LOADER_ROOT / "CMakeLists.txt",
    LOADER_ROOT / "include/nxloader.h",
    LOADER_ROOT / "src/nxloader.c",
    LOADER_ROOT / "src/nxloader_elf32.c",
    LOADER_ROOT / "src/nxloader_softfp.c",
    LOADER_ROOT / "tests/test_nxloader.c",
    LOADER_ROOT / "tests/test_softfp.c",
    LOADER_ROOT / "tests/test_armv7_cross.c",
    LOADER_ROOT / "tests/test_armv7_cache_sync.c",
    LOADER_ROOT / "tests/run-armv7-cross.sh",
    AUDIT_PATH,
    PHYSICAL_PATH,
    ABI_DEFINITIONS,
    ABI_VARIANTS,
    ABI_ROWS,
    MATRIX_PATH,
    REPO_ROOT / "framework/tests/run-safe-gates.sh",
)


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


def reject_sensitive_location(text, label):
    private_ip = re.compile(
        r"(?<![0-9.])(?:"
        r"(?:10|127)\.(?:[0-9]{1,3}\.){2}[0-9]{1,3}|"
        r"169\.254\.[0-9]{1,3}\.[0-9]{1,3}|"
        r"192\.168\.[0-9]{1,3}\.[0-9]{1,3}|"
        r"172\.(?:1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}\.[0-9]{1,3}"
        r")(?=$|[^0-9.])")
    require(private_ip.search(text) is None,
            "%s contains a private/local address" % label)
    require("/home/" not in text and "/mnt/" not in text,
            "%s contains a personal path" % label)
    require("password" not in text.lower() and
            "private_key" not in text.lower() and
            "access_token" not in text.lower(),
            "%s contains credential material" % label)


def validate_evidence(audit):
    requirements = audit.get("requirements")
    expected_ids = ["M09-%03d" % number for number in range(1, 21)]
    require(isinstance(requirements, list) and
            [item.get("id") for item in requirements] == expected_ids,
            "M09 audit IDs are incomplete or reordered")
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


def validate_contract_and_backend():
    require((LOADER_ROOT / "VERSION").read_text(
        encoding="utf-8").strip() == "0.9.0",
        "nxloader VERSION is not the reviewed 0.9.0 successor")
    header = (LOADER_ROOT / "include/nxloader.h").read_text(encoding="utf-8")
    require('#define NXLOADER_VERSION_STRING "0.9.0"' in header and
            "#define NXLOADER_API_VERSION_MAJOR 1u" in header and
            "#define NXLOADER_API_VERSION_MINOR 3u" in header and
            "NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS" in header,
            "public ARMv7/API successor contract is incoherent")
    legacy_values = (
        "NXLOADER_ECALLBACK = -14", "NXLOADER_STATE_EMPTY = 0",
        "NXLOADER_STATE_LOADED = 1", "NXLOADER_STATE_RELOCATED = 2",
        "NXLOADER_STATE_RESOLVED = 3", "NXLOADER_STATE_FINALIZED = 4",
        "NXLOADER_STATE_INITIALIZED = 5", "NXLOADER_STATE_ERROR = 6",
        "NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS = 1u << 2",
    )
    require(all(token in header for token in legacy_values) and
            "NXLOADER_EREENTRANT = -15" in header and
            "NXLOADER_STATE_INITIALIZING = 7" in header and
            "NXLOADER_STATE_JNI_LOADING = 8" in header and
            "NXLOADER_STATE_READY = 9" in header and
            "NXLOADER_STATE_PATCHING = 10" in header,
            "API 1.3 renumbered an M09 value or failed to append M11 values")
    parser = (LOADER_ROOT / "src/nxloader_elf32.c").read_text(encoding="utf-8")
    require("SHT_" not in parser and "e_shoff" not in parser and
            "e_shnum" not in parser and "e_shentsize" not in parser,
            "ARMv7 runtime parser regained a section-header dependency")
    for token in ("R_ARM_REL32", "R_ARM_THM_CALL", "R_ARM_CALL",
                  "R_ARM_JUMP24", "R_ARM_IRELATIVE", "STT_GNU_IFUNC",
                  "type == 129", "type == 130"):
        require(token in parser, "ARMv7 backend lacks %s" % token)
    require(parser.count("if (!nxloader_arm_supported_relocation(type))") >= 2,
            "local/import ARM relocation allowlists are incomplete")
    require(parser.index("if (!nxloader_arm_supported_relocation(type))") <
            parser.index("nxloader_apply_relocation_hook(module"),
            "unknown ARM relocations can reach the callback")
    require("ARM GNU IFUNC symbols are unsupported" in parser and
            "NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS" in parser,
            "IFUNC or narrow ARM textrel policy is missing")


def validate_cross_gate():
    runner = LOADER_ROOT / "tests/run-armv7-cross.sh"
    source = runner.read_text(encoding="utf-8")
    require(runner.stat().st_mode & 0o111,
            "ARMv7 cross runner is not executable")
    for token in ("NXLOADER_WARNINGS_AS_ERRORS=ON",
                  "NXLOADER_BUILD_TESTS=ON", "ARMHF_GCC",
                  "ARMHF_CLANG", "ARMHF_LLD", "ARMHF_QEMU",
                  "ARMHF_INTERPRETER=/lib/ld-linux-armhf.so.3",
                  "version_is_above \"$version\" 2.30",
                  "softfp_provider_tests=2",
                  "nxloader_softfp_tests",
                  "guest_elf_loaded=0", "guest_initializers_executed=0"):
        require(token in source, "ARMv7 cross gate lacks %s" % token)
    cross_test = (LOADER_ROOT / "tests/test_armv7_cross.c").read_text(
        encoding="utf-8")
    cache_test = (LOADER_ROOT / "tests/test_armv7_cache_sync.c").read_text(
        encoding="utf-8")
    require("nxloader_module_call_initializers" not in cross_test + cache_test,
            "an ARMv7 ABI/cache test can execute a guest initializer")
    require("softfp_float=1 softfp_double=1" in cross_test and
            "stack_align_8=1" in cross_test and
            "vfp_d8_preserved=1" in cross_test,
            "ARMv7 cross test lacks a real ABI boundary assertion")
    reference = (LOADER_ROOT / "REFERENCE-AUDIT.md").read_text(
        encoding="utf-8")
    require("audita 26 ELFs" in reference and
            "temporários (7 loadables e 19 relocatables)" in reference,
            "ARMv7 reference audit has stale cross-build counts")


def validate_physical_evidence():
    evidence_text = PHYSICAL_PATH.read_text(encoding="utf-8")
    reject_sensitive_location(evidence_text, "physical evidence")
    evidence = load_json(PHYSICAL_PATH)
    require(set(evidence) == {
        "schema_version", "milestone", "gate_id", "execution_date_utc",
        "authorization", "physical_arm_execution", "kernel_architecture",
        "process_abi", "pt_interp", "glibc_max", "source_sha256",
        "binary_sha256", "binary_size", "observed_output", "results",
        "safety", "privacy", "durable_evidence",
    }, "physical evidence has an unknown or missing field")
    require(evidence.get("schema_version") == 1 and
            evidence.get("milestone") == "M09-012" and
            evidence.get("gate_id") == "nxloader-armv7-physical" and
            evidence.get("authorization") == "current-session-explicit" and
            evidence.get("physical_arm_execution") is True,
            "physical evidence header or authorization is invalid")
    require(evidence.get("process_abi") ==
            "ELF32 little-endian EM_ARM EABI5 hard-float" and
            evidence.get("kernel_architecture") == "aarch64" and
            evidence.get("pt_interp") == "/lib/ld-linux-armhf.so.3" and
            evidence.get("glibc_max") == "2.4",
            "physical probe ABI is not the audited ARMHF binary")
    for field in ("source_sha256", "binary_sha256"):
        require(re.fullmatch(r"[0-9a-f]{64}", evidence.get(field, "")),
                "physical evidence has invalid %s" % field)
    cache_source = LOADER_ROOT / "tests/test_armv7_cache_sync.c"
    source_digest = hashlib.sha256(cache_source.read_bytes()).hexdigest()
    require(evidence.get("source_sha256") == source_digest ==
            "058cb408554fcf74890e06dce62470e717b09f00b5dce7026fa872131a136c04",
            "physical evidence source hash diverges from the cache probe")
    require(evidence.get("binary_sha256") ==
            "0d63a947fada6b6f9bd3b47c1982f16d7ccea711161f1669335fdc03193fb10b" and
            evidence.get("binary_size") == 12776 and
            evidence.get("observed_output") ==
            "armv7-cache-sync: PASS arm_rewrite=1 thumb_rewrite=1 "
            "wx_mapping=0 guest_elf_loaded=0 guest_initializers_executed=0",
            "physical binary identity or observed output changed")
    require(evidence.get("results") == {
        "arm_rewrite": True,
        "thumb_rewrite": True,
        "wx_mapping": False,
        "remote_temporary_file_removed": True,
    }, "physical cache-sync results are incomplete")
    require(evidence.get("safety") == {
        "game_launched": False,
        "service_changes": False,
        "signals_sent": False,
        "guest_elf_loaded": False,
        "guest_initializers_executed": False,
    }, "physical evidence overclaims safety")
    require(evidence.get("privacy") == {
        "device_address_recorded": False,
        "hostname_recorded": False,
        "credential_recorded": False,
    }, "physical evidence recorded device/private identity")
    durable = evidence.get("durable_evidence", {})
    require(set(durable) == {
        "run_id", "manifest_sha256", "console_sha256", "result_sha256",
        "command_status_sha256", "command_status", "tee_status",
        "received_signal",
    }, "physical durable evidence has an unknown or missing field")
    require(durable.get("run_id") ==
            "20260808T200559Z-pid2955299-1833629567" and
            durable.get("manifest_sha256") ==
            "9f319e87e9e4226c481a6b5a85fe1e90cfef706f8f923d2c234a47dbad78bbe7" and
            durable.get("console_sha256") ==
            "f782f6701f7997dbc8c27e7ee98fa65996ecd98fc62a8f35cc0db118006b7f30" and
            durable.get("result_sha256") ==
            "840dcce930160c9ff959f036d6f2bbff5860be2a5e1fc648c73e18d18b3c0394" and
            durable.get("command_status_sha256") ==
            "9a271f2a916b0b6ee6cecb2426f0b3206ef074578be55d9bc94f6f3fe3ab86aa" and
            durable.get("command_status") == 0 and
            durable.get("tee_status") == 0 and
            durable.get("received_signal") == "none",
            "physical durable evidence is incomplete")


def validate_abi_matrix():
    definitions = load_json(ABI_DEFINITIONS)
    variants = load_json(ABI_VARIANTS)
    require(definitions.get("checks_per_variant") == 28 and
            len(definitions.get("checks", [])) == 28,
            "ABI matrix does not define exactly 28 checks")
    variant_items = [item for item in variants.get("variants", [])
                     if item.get("abi") == "armv7"]
    require([item.get("variant_id") for item in variant_items] ==
            ["kotor-armv7", "tasm2_127-armv7"],
            "positive ARMv7 variant set changed")
    rows = ABI_ROWS.read_text(encoding="utf-8").splitlines()
    require(rows and rows[0].startswith("id\tport\tabi\t"),
            "ABI TSV header changed")
    armv7_rows = [line for line in rows[1:] if "\tarmv7\t" in line]
    require(len(armv7_rows) == 56 and
            sum(line.startswith("ABI-kotor-armv7-")
                for line in armv7_rows) == 28 and
            sum(line.startswith("ABI-tasm2_127-armv7-")
                for line in armv7_rows) == 28,
            "ABI TSV is not 28 checks per positive ARMv7 variant")
    require(all("\tarmv7\t" in line and "\trecorded\t" in line
                for line in armv7_rows),
            "ABI TSV contains an unrecorded/non-ARMv7 claim")


def validate_matrix():
    matrix = load_json(MATRIX_PATH)
    gates = {item["id"]: item for item in matrix.get("gates", [])}
    require(gates.get("nxloader-m09-audit", {}).get("class") == "pure" and
            gates.get("nxloader-m09-audit", {}).get("automatic") is True,
            "M09 audit is not an automatic pure gate")
    require(gates.get("nxloader-armv7-cross", {}).get("class") == "filesystem" and
            gates.get("nxloader-armv7-cross", {}).get("automatic") is True,
            "ARMv7 cross gate is not automatic/filesystem")
    require("framework/nxloader/tests/test_softfp.c" not in
            gates.get("nxloader-host", {}).get("sources", []) and
            "framework/nxloader/tests/test_softfp.c" in
            gates.get("nxloader-armv7-cross", {}).get("sources", []),
            "softfp provider test is assigned to a gate that does not run it")
    require(gates.get("nxloader-armv7-physical", {}).get("class") == "hardware" and
            gates.get("nxloader-armv7-physical", {}).get("automatic") is False and
            gates.get("nxloader-armv7-physical", {}).get("command") is None,
            "physical ARMv7 gate became automatic")


def main():
    audit_text = AUDIT_PATH.read_text(encoding="utf-8")
    reject_sensitive_location(audit_text, "M09 audit")
    audit = load_json(AUDIT_PATH)
    require(set(audit) == {
        "schema_version", "milestone", "scope", "physical_device_evidence",
        "guest_initializers_executed", "requirements",
    }, "M09 audit has an unknown top-level field")
    require(audit.get("schema_version") == 1 and
            audit.get("milestone") == "M09" and
            audit.get("scope") ==
            "armv7-rel-softfp-cross-abi-and-authorized-physical-cache-sync" and
            audit.get("physical_device_evidence") is True and
            audit.get("guest_initializers_executed") is False,
            "M09 audit header/evidence status changed")
    validate_evidence(audit)
    validate_contract_and_backend()
    validate_cross_gate()
    validate_physical_evidence()
    validate_abi_matrix()
    validate_matrix()
    for path in M09_PRIVACY_FILES:
        require(path.is_file() and not path.is_symlink(),
                "M09 privacy input is missing/linked: %s" % path)
        reject_sensitive_location(path.read_text(encoding="utf-8"),
                                  str(path.relative_to(REPO_ROOT)))
    print("M09 audit gate passed: 20 requirements, 56 ABI rows, "
          "physical_device_evidence=1 guest_initializers_executed=0")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, json.JSONDecodeError) as error:
        print("M09 audit gate failed: %s" % error)
        raise SystemExit(1)
