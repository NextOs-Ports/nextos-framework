#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free completeness gate for nxloader AArch64 / milestone M10."""

import hashlib
import json
import re
import struct
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
LOADER_ROOT = REPO_ROOT / "framework/nxloader"
AUDIT_PATH = LOADER_ROOT / "m10-audit-v1.json"
EXPECTED_GUESTS_PATH = LOADER_ROOT / "tests/m10-approved-aarch64-guests-v1.json"
GUEST_EVIDENCE_PATH = LOADER_ROOT / "tests/m10-aarch64-guests-evidence-v1.json"
GUEST_LOG_PATH = LOADER_ROOT / "tests/m10-aarch64-guests-evidence-v1.log"
MATRIX_PATH = REPO_ROOT / "framework/tests/test-matrix-v1.json"
CONTRACT_PATH = REPO_ROOT / "framework/contracts/declarative-v1.json"
ABI_VARIANTS = REPO_ROOT / "framework/catalog/abi-variants-v1.json"
ABI_ROWS = REPO_ROOT / "framework/catalog/abi-checks-v1.tsv"
GUEST_RUNNER_PATH = LOADER_ROOT / "tests/run_m10_aarch64_guest_gate.py"
GUEST_RUNNER_TEST_PATH = (
    LOADER_ROOT / "tests/test_m10_aarch64_guest_gate_runner.py")

FINAL_GUEST_EVIDENCE_SHA256 = (
    "310e0d9153720d132be6a7e72421709cc12163e93f98d697abc8efb96f694cc4")
FINAL_GUEST_LOG_SHA256 = (
    "ea0f3f925c9f275505cd8a8271f3cf86ee66fc1ae9c79ea5f2572b2f96d9eb3a")
FINAL_EXPECTED_GUESTS_SHA256 = (
    "e6a46cb0f142a4ab6eb681295bccbdfd9b57f6bd6c4d95ee61338428db33ff13")
FINAL_GUEST_RUNNER_SHA256 = (
    "911d84ee214a3958d60d080b439cc15f9248492cd68e9de89b03edcf95be8822")
FINAL_GUEST_RUNNER_TEST_SHA256 = (
    "390679d499d0123ef20ad070e86d29254268047d2d5164986bc1c475470c7d0e")
FINAL_SOURCE_SNAPSHOT_SHA256 = (
    "05518af3c8463f6fd19db5141a96f84cf17809e7f3be55fd327f21c62948e8fc")
FINAL_INSPECTOR_SHA256 = (
    "429986130ba71a51930a1e5aa5b98da353fbf10c1d831ea770e1f8a5bd068423")
SOURCE_SNAPSHOT_DOMAIN = b"NXLOADER_SOURCE_SNAPSHOT_V1\0"
SOURCE_SNAPSHOT_PATHS = (
    "VERSION",
    "CMakeLists.txt",
    "include/nxloader.h",
    "src/nxloader_internal.h",
    "src/nxloader.c",
    "src/nxloader_elf32.c",
    "src/nxloader_elf64.c",
    "src/nxloader_hooks.c",
    "src/nxloader_protect.c",
    "src/nxloader_registry.c",
    "tools/nxloader_inspect.c",
)

EXPECTED_GUESTS = {
    "bully2-libgame": {
        "sha256": "345f6411bebbd2310b2cb5d2a271341847a054c2b7f6d5ffba8030cd53140d76",
        "symbols": 50356, "relocations": 75033, "init_array": 506,
        "exports": 49938,
    },
    "sonic4ep2-libfox": {
        "sha256": "ca07163ad1e92d767016d43048a2c13eede7b9d6217ed4f032ca4d6d8e342a1a",
        "symbols": 29153, "relocations": 54962, "init_array": 46,
        "exports": 28776,
    },
    "horizonchase-libunity": {
        "sha256": "7c46972d06b6b85f953bf61b477fdbf5bf22f7be55b57e4b54b4403f05478359",
        "symbols": 646, "relocations": 54707, "init_array": 433,
        "exports": 222,
    },
    "horizonchase-libil2cpp": {
        "sha256": "acf414c77853aa0e7f8d8963f307f65f3d156c8d6d806661b738df3385e48cdc",
        "symbols": 2658, "relocations": 305750, "init_array": 24,
        "exports": 2369,
    },
    "horizonchase-libmain": {
        "sha256": "a2dfe24e0170af0ae90b13630fc6d4e788be2c5216b4bddb17a6e0ca3c4c5af8",
        "symbols": 13, "relocations": 20, "init_array": 0,
        "exports": 1,
    },
}

PRIVACY_FILES = (
    AUDIT_PATH,
    EXPECTED_GUESTS_PATH,
    GUEST_EVIDENCE_PATH,
    GUEST_LOG_PATH,
    LOADER_ROOT / "README.md",
    LOADER_ROOT / "REFERENCE-AUDIT.md",
    LOADER_ROOT / "tests/run_m10_aarch64_guest_gate.py",
    LOADER_ROOT / "tests/test_m10_aarch64_guest_gate_runner.py",
    LOADER_ROOT / "tests/test_aarch64_cross.c",
    LOADER_ROOT / "tests/run-aarch64-cross.sh",
    ABI_VARIANTS,
    ABI_ROWS,
    MATRIX_PATH,
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


def sha256_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_current_source_snapshot():
    """Recreate the evidence runner's ordered, path-separated source lock."""
    aggregate = hashlib.sha256()
    aggregate.update(SOURCE_SNAPSHOT_DOMAIN)
    files = []
    for relative_name in SOURCE_SNAPSHOT_PATHS:
        path = LOADER_ROOT / relative_name
        require(path.is_file() and not path.is_symlink(),
                "source snapshot member is missing/linked: %s" %
                relative_name)
        content = path.read_bytes()
        path_bytes = relative_name.encode("utf-8")
        aggregate.update(struct.pack(">I", len(path_bytes)))
        aggregate.update(path_bytes)
        aggregate.update(struct.pack(">Q", len(content)))
        aggregate.update(content)
        files.append({
            "path": relative_name,
            "size": len(content),
            "sha256": hashlib.sha256(content).hexdigest(),
        })
    return {
        "schema_version": 1,
        "algorithm": "sha256-domain-u32be-path-u64be-content-v1",
        "ordered": True,
        "file_count": len(files),
        "sha256": aggregate.hexdigest(),
        "files": files,
    }


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
    lowered = text.lower()
    for token in ("private_key", "access_token", "authorization: bearer"):
        require(token not in lowered,
                "%s contains credential material" % label)


def validate_references(audit):
    requirements = audit.get("requirements")
    expected_ids = ["M10-%03d" % number for number in range(1, 21)]
    require(isinstance(requirements, list) and
            [item.get("id") for item in requirements] == expected_ids,
            "M10 audit IDs are incomplete or reordered")
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
            "#define NXLOADER_API_VERSION_MINOR 3u" in header and
            "callbacks are trusted adapter code." in header and
            "CALL/JUMP24/THM_CALL use only the checked default codec" in
            header and
            "must outlive the registry and all" in header,
            "public nxloader successor version/API is incoherent")
    legacy_values = (
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
            "API 1.3 renumbered an M10 value or failed to append M11 values")
    cmake = (LOADER_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require("project(nxloader VERSION 0.9.0" in cmake and
            "NXLOADER_BUILD_AARCH64_CROSS_TEST" in cmake and
            "nxloader_pending_tests" in cmake,
            "M10 CMake contract is incomplete")
    contract = load_json(CONTRACT_PATH)
    components = {item["id"]: item
                  for item in contract.get("components", [])}
    require(contract.get("schema_version") == 1 and
            components.get("nxloader", {}).get("current_version") ==
            "0.9.0" and
            components.get("nxloader", {}).get("api_version") == 1,
            "live declarative successor contract is incoherent")


def validate_backend_and_transactions():
    common = (LOADER_ROOT / "src/nxloader.c").read_text(encoding="utf-8")
    internal = (LOADER_ROOT / "src/nxloader_internal.h").read_text(
        encoding="utf-8")
    parser32 = (LOADER_ROOT / "src/nxloader_elf32.c").read_text(
        encoding="utf-8")
    parser64 = (LOADER_ROOT / "src/nxloader_elf64.c").read_text(
        encoding="utf-8")
    hooks = (LOADER_ROOT / "src/nxloader_hooks.c").read_text(encoding="utf-8")
    registry = (LOADER_ROOT / "src/nxloader_registry.c").read_text(
        encoding="utf-8")
    tests = (LOADER_ROOT / "tests/test_nxloader.c").read_text(
        encoding="utf-8")
    pending_tests = (LOADER_ROOT / "tests/test_pending.c").read_text(
        encoding="utf-8")
    require(all(token not in parser64 for token in
                ("SHT_", "e_shoff", "e_shnum", "e_shentsize")),
            "AArch64 parser regained a section-header dependency")
    supported = re.search(
        r"static int nxloader_aarch64_supported_relocation\([^}]+\}",
        parser64, re.DOTALL)
    require(supported is not None, "AArch64 relocation allowlist is missing")
    allowlist = supported.group(0)
    for token in ("R_AARCH64_NONE", "R_AARCH64_ABS64",
                  "R_AARCH64_GLOB_DAT", "R_AARCH64_JUMP_SLOT",
                  "R_AARCH64_RELATIVE"):
        require(token in allowlist, "AArch64 allowlist lacks %s" % token)
    for token in ("PREL", "ADR_", "ADD_", "LDST", "JUMP26", "CALL26"):
        require(token not in allowlist,
                "unproven instruction relocation entered the allowlist")
    require(parser64.count(
        "if (!nxloader_aarch64_supported_relocation(type))") >= 2 and
        parser64.index(
            "if (!nxloader_aarch64_supported_relocation(type))") <
        parser64.index("nxloader_apply_relocation_hook(module"),
        "unknown AArch64 relocations can reach the callback")
    for token in ("type == R_AARCH64_IRELATIVE", "STT_GNU_IFUNC",
                  "nxloader_aarch64_tls_relocation",
                  "nxloader_add_signed64"):
        require(token in parser64, "AArch64 backend lacks %s" % token)
    require(parser64.count("nxloader_pending_commit(&context.pending)") == 2 and
            parser32.count("nxloader_pending_commit(&context.pending)") == 2,
            "ELF32/ELF64 do not propagate both transactional commits")
    pending_add = common[common.index("nxloader_result nxloader_pending_add"):
                         common.index("static int nxloader_pending_compare")]
    require("for (" not in pending_add and "while (" not in pending_add and
            "nxloader_pending_sort" in common and
            "nxloader_pending_validate" in common,
            "pending append is not O(1) plus a single ordered validation")
    for token in ("dynamic_vma", "sysv_hash_size", "gnu_hash_size",
                  "nxloader_validate_relocation_metadata_target"):
        require(token in internal + common,
                "metadata transaction protection lacks %s" % token)
    require(parser32.count(
        "nxloader_validate_relocation_metadata_target(") >= 2 and
        parser64.count(
            "nxloader_validate_relocation_metadata_target(") >= 1 and
        parser64.count(
            "nxloader_aarch64_validate_relocation_target(") >= 3,
            "a relocation phase can bypass metadata target protection")
    require("#define NXLOADER_MAX_DYNAMIC_NAME_LENGTH 4096u" in internal and
            "NXLOADER_MAX_DYNAMIC_NAME_LENGTH + 1u" in common,
            "dynamic strings are not globally bounded to 4096 bytes")
    require("nxloader_segment_for_vma_range" in internal and
            common.count("nxloader_segment_for_vma_range(") >= 4 and
            parser32.count("nxloader_segment_for_vma_range(") >= 1 and
            parser64.count("nxloader_segment_for_vma_range(") >= 1,
            "ordered PT_LOAD lookups can bypass the binary range helper")
    file_mapping = common[
        common.index("int nxloader_file_mapping_matches"):
        common.index("nxloader_result nxloader_validate_relro")]
    relro_start = common.index("nxloader_result nxloader_validate_relro")
    relro = common[
        relro_start:
        common.index("static void nxloader_file_back_text", relro_start)]
    require("nxloader_segment_for_vma_range" in file_mapping and
            "for (" not in file_mapping and
            relro.count("for (") == 1,
            "file mapping/RELRO validation regained a quadratic segment scan")
    require("nxloader_arm_supported_symbol_type" in parser32 and
            parser32.count(
                "if (!nxloader_arm_supported_symbol_type(symbol_type))") == 2,
            "ARMv7 symbol types can bypass the pre-hook allowlist")
    binary_index = registry[
        registry.index("static int nxloader_registry_binary_index"):
        registry.index("static int nxloader_registry_invariant")]
    registry_lookup = registry[
        registry.index("nxloader_result nxloader_registry_lookup"):
        registry.index("nxloader_result nxloader_registry_add_module")]
    require("nxloader_registry_hash" not in registry and
            "while (low < high)" in binary_index and
            "nxloader_registry_binary_index" in registry_lookup and
            "nxloader_registry_invariant" not in registry_lookup,
            "registry lookup is not collision-independent O(log n)")
    require("nxloader_registry_candidate_sort(candidates, total)" in registry and
            "nxloader_registry_candidate_sift_down" in registry and
            "nxloader_registry_clear(registry);\n  *registry = staged;" in registry and
            "NXLOADER_MAX_DYNAMIC_NAME_LENGTH + 1u" in registry,
            "registry batch lost bounded names, heapsort or atomic publish")
    add_module = registry[
        registry.index("nxloader_result nxloader_registry_add_module"):]
    require("return NXLOADER_ESTATE;" in add_module and
            'case NXLOADER_ECALLBACK: return "callback rejected operation";' in
            common,
            "callback/state diagnostics diverged from the preserved M10 contract")
    exports32 = parser32[
        parser32.index("nxloader_result nxloader_add_exports_elf32"):
        parser32.index("typedef struct nxloader_initializer_plan32")]
    exports64 = parser64[
        parser64.index("nxloader_result nxloader_add_exports_elf64"):
        parser64.index("typedef struct nxloader_initializer_plan64")]
    require(exports32.count("nxloader_registry_add_provider(") == 1 and
            exports64.count("nxloader_registry_add_provider(") == 1,
            "ELF32/ELF64 add_module no longer ingests exports in one batch")
    for parser, suffix in ((parser32, "32"), (parser64, "64")):
        for token in ("maximum_bucket", "chain_scan_budget",
                      "scan_budget = module->image_size / sizeof(uint32_t)",
                      "nxloader_validate_needed%s" % suffix,
                      "nxloader_needed%s_sift_down" % suffix):
            require(token in parser,
                    "ELF%s bounded dynamic-table validation lacks %s" %
                    (suffix, token))
    for token in ("#define MANY_NEEDED_COUNT 2048u",
                  "test_linear_gnu_hash_scan",
                  "chain_entries=8 work_budget=5120",
                  "test_many_needed_scaling",
                  "test_dynamic_name_bounds_and_overlapping_offsets",
                  "test_many_segment_lookup_and_relro_scaling",
                  "test_arm_symbol_type_rejection_is_pre_hook",
                  "REGISTRY_FNV_COLLISION_COUNT 2048u",
                  "REGISTRY_FNV_COLLISION_MASK UINT64_C(0x0fff)",
                  "test_registry_fnv_collision_scaling",
                  "test_registry_public_name_bounds",
                  "callback rejected operation",
                  "phase-invalid",
                  "MANY_SEGMENT_COUNT 4096u",
                  "OVERLAPPING_NEEDED_COUNT 512u"):
        require(token in tests,
                "hostile/scaling regression lacks %s" % token)
    require("STRESS_WRITES = 16384" in pending_tests and
            "validation=O(n-log-n) stress=16384 atomic_overlap=1" in
            pending_tests,
            "pending-write stress/atomicity regression changed")
    require("displacement < -(INT64_C(1) << 27)" in hooks and
            "module->trampoline_pool_used += 16" in hooks,
            "AArch64 hook range/capacity contract changed")


def validate_cross_gate():
    runner = LOADER_ROOT / "tests/run-aarch64-cross.sh"
    source = runner.read_text(encoding="utf-8")
    require(runner.stat().st_mode & 0o111,
            "AArch64 cross runner is not executable")
    for token in (
            "BUSTER_IMAGE_ID=sha256:", "--network none", "--read-only",
            "NXLOADER_WARNINGS_AS_ERRORS=ON", "EXPECTED_ELF_COUNT=20",
            "EXPECTED_LOADABLE_ELF_COUNT=5",
            "EXPECTED_RELOCATABLE_ELF_COUNT=15",
            "INTERPRETER=/lib/ld-linux-aarch64.so.1",
            "version_is_above \"$version\" 2.30",
            "CMAKE_C_COMPILER_TARGET=aarch64-linux-gnu",
            "PASS gcc=1 clang=1 lld=1 qemu=1", "work_tree_cleaned=1",
            "hook_execution=2", "loader_cache_finalize=2",
            "icache_primed_entry=2", "icache_primed_pool=2",
            "post_finalize_reexecution=2",
            "finalize_only_loader_cache_clear=2",
            "synthetic_test_elf_loaded=2", "external_guest_elf_loaded=0",
            "device_access=0", "hardware_ran=0"):
        require(token in source, "AArch64 cross gate lacks %s" % token)
    test = (LOADER_ROOT / "tests/test_aarch64_cross.c").read_text(
        encoding="utf-8")
    require("nxloader_module_call_initializers" not in test and
            "JNI_OnLoad" not in test,
            "AArch64 cross test can execute guest lifecycle code")
    for token in ("nx_callee_saved_probe", "stack_align_16=1",
                  "cache_rewrite=1", "wx_mapping=0",
                  "nx_synthetic_loader_hook_gate",
                  "primed_entry_result = entry_function(execution_input)",
                  "primed_pool_result = pool_function(execution_input)",
                  "pool_execution_result = pool_function(execution_input)",
                  "finalize_only_loader_cache_clear=1"):
        require(token in test, "AArch64 ABI/cache test lacks %s" % token)
    compact_test = re.sub(r"\s+", "", test)
    for token in ("nxloader_module_install_hook(module",
                  "nxloader_module_finalize(module"):
        require(token in compact_test,
                "AArch64 ABI/cache test lacks %s" % token)


def validate_abi_matrix():
    variants = load_json(ABI_VARIANTS).get("variants", [])
    aarch64 = [item for item in variants if item.get("abi") == "aarch64"]
    require([item.get("variant_id") for item in aarch64] == [
        "bully2-aarch64", "sonic4ep2-aarch64", "horizonchase-aarch64"],
        "positive AArch64 variant set changed")
    rows = ABI_ROWS.read_text(encoding="utf-8").splitlines()
    require(len(rows) == 141 and rows[0].startswith("id\tport\tabi\t"),
            "ABI TSV is not header plus 140 rows")
    aarch64_rows = [line for line in rows[1:] if "\taarch64\t" in line]
    require(len(aarch64_rows) == 84 and all(
        "\trecorded\t" in line for line in aarch64_rows),
        "ABI TSV is not 84 recorded AArch64 checks")
    for prefix in ("ABI-bully2-aarch64-", "ABI-sonic4ep2-aarch64-",
                   "ABI-horizonchase-aarch64-"):
        require(sum(line.startswith(prefix) for line in aarch64_rows) == 28,
                "%s does not have exactly 28 checks" % prefix)


def validate_guest_runner():
    runner = GUEST_RUNNER_PATH.read_text(encoding="utf-8")
    selftest = GUEST_RUNNER_TEST_PATH.read_text(encoding="utf-8")
    require(sha256_file(GUEST_RUNNER_PATH) == FINAL_GUEST_RUNNER_SHA256 and
            sha256_file(GUEST_RUNNER_TEST_PATH) ==
            FINAL_GUEST_RUNNER_TEST_SHA256,
            "guest runner/selftest hash changed")
    for token in (
            "DEFAULT_TIMEOUT_SECONDS = 120.0",
            "DEFAULT_OUTPUT_LIMIT_BYTES = 256 * 1024",
            "MAX_TIMEOUT_SECONDS = 3600.0",
            "MAX_OUTPUT_LIMIT_BYTES = 16 * 1024 * 1024",
            "time.monotonic_ns()", "threading.Condition()",
            "os.pidfd_open(process.pid, 0)",
            "signal.pidfd_send_signal(pidfd, selected_signal)",
            "os.wait4(process.pid, 0)",
            "SOURCE_SNAPSHOT_DOMAIN = b\"NXLOADER_SOURCE_SNAPSHOT_V1\\0\"",
            "aggregate.update(struct.pack(\">I\", len(path_bytes)))",
            "aggregate.update(struct.pack(\">Q\", len(content)))",
            "def inspect_exports(",
            '"inspector_modes": ["--relocate", "--exports"]',
            '"registry_add_module_executed": 1',
            "parse_export_report"):
        require(token in runner, "supervised guest gate lacks %s" % token)
    for forbidden in ("WNO" + "HANG", "set" + "sid", "kill" + "pg"):
        require(forbidden not in runner,
                "guest supervisor regained unsafe/busy primitive %s" %
                forbidden)
    for token in (
            "test_timeout_uses_kill_fallback_and_leaves_sibling_alive",
            "test_output_limit_terminates_and_caps_capture",
            "test_registry_export_inventory_matches_provider_rules",
            "test_export_report_parser_accepts_exact_success",
            "test_export_report_parser_rejects_missing_malformed_or_duplicate",
            "test_exports_subprocess_is_limited_and_lifecycle_stays_zero",
            "test_source_snapshot_is_deterministic_ordered_and_path_free",
            "signal.SIG_IGN", "stdout_truncated"):
        require(token in selftest,
                "guest supervisor regression suite lacks %s" % token)
    require(len(re.findall(r"^\s+def test_", selftest, re.MULTILINE)) == 9,
            "guest supervisor regression suite is not exactly 9 selftests")


def validate_guest_evidence():
    require(sha256_file(GUEST_EVIDENCE_PATH) ==
            FINAL_GUEST_EVIDENCE_SHA256,
            "final guest evidence JSON hash changed")
    require(sha256_file(GUEST_LOG_PATH) == FINAL_GUEST_LOG_SHA256,
            "final guest evidence log hash changed")
    require(sha256_file(EXPECTED_GUESTS_PATH) ==
            FINAL_EXPECTED_GUESTS_SHA256,
            "approved guest manifest hash changed")
    expected_text = EXPECTED_GUESTS_PATH.read_text(encoding="utf-8")
    evidence_text = GUEST_EVIDENCE_PATH.read_text(encoding="utf-8")
    log_text = GUEST_LOG_PATH.read_text(encoding="utf-8")
    expected = load_json(EXPECTED_GUESTS_PATH)
    evidence = load_json(GUEST_EVIDENCE_PATH)
    require(set(evidence) == {
        "completed_utc", "created_utc", "expected_manifest_sha256",
        "guests", "inspector", "limits", "milestone", "safety",
        "schema_version", "scope", "source_snapshot", "status",
        "total_elapsed_ns",
    }, "guest evidence top-level schema changed")
    require(expected.get("schema_version") == 1 and
            expected.get("milestone") == "M10" and
            expected.get("scope") ==
            "hash-pinned-local-read-only-aarch64-reference-gate",
            "approved guest manifest header changed")
    expected_safety = expected.get("safety")
    require(expected_safety == {
        "inspector_modes": ["--relocate", "--exports"], "resolve": False,
        "finalize": False, "guest_initializers_executed": False,
        "guest_jni_onload_executed": False, "device_access": False,
        "guest_files_copied": False,
    }, "approved guest safety contract changed")
    expected_items = {item.get("id"): item
                      for item in expected.get("guests", [])}
    require(set(expected_items) == set(EXPECTED_GUESTS),
            "approved guest identity set changed")
    for guest_id, contract in EXPECTED_GUESTS.items():
        item = expected_items[guest_id]
        expected_exports = {
            "eligible": contract["exports"],
            "added": contract["exports"],
            "equivalent": 0,
            "replaced_lower_priority": 0,
            "ignored_lower_priority": 0,
            "collisions": 0,
        }
        require(item.get("sha256") == contract["sha256"] and
                item.get("dynamic_symbol_count") == contract["symbols"] and
                sum(item.get("relocations", {}).values()) ==
                contract["relocations"] and
                item.get("init_array_entries") == contract["init_array"] and
                item.get("registry_exports") == expected_exports,
                "%s approved inventory changed" % guest_id)
        require(set(item.get("relocations", {})).issubset({
            "R_AARCH64_ABS64", "R_AARCH64_GLOB_DAT",
            "R_AARCH64_JUMP_SLOT", "R_AARCH64_RELATIVE"}),
            "%s gained an unapproved relocation" % guest_id)

    require(evidence.get("schema_version") == 1 and
            evidence.get("milestone") == "M10" and
            evidence.get("scope") == expected.get("scope") and
            evidence.get("status") == "PASS",
            "approved guest evidence did not finish PASS")
    require(evidence.get("expected_manifest_sha256") ==
            sha256_file(EXPECTED_GUESTS_PATH),
            "guest evidence is detached from its expected manifest")
    require(evidence.get("safety") == {
        "device_access": 0, "guest_files_copied": 0,
        "guest_initializers_executed": 0,
        "guest_jni_onload_executed": 0, "resolve_executed": 0,
        "finalize_executed": 0,
    }, "guest evidence overclaims or executed a forbidden phase")
    require(evidence.get("limits") == {
        "output_limit_bytes_per_stream": 262144,
        "term_grace_seconds": 2.0,
        "timeout_seconds": 120.0,
    }, "guest gate runtime limits changed")
    current_snapshot = build_current_source_snapshot()
    frozen_snapshot = evidence.get("source_snapshot")
    if current_snapshot["sha256"] != FINAL_SOURCE_SNAPSHOT_SHA256:
        closure = load_json(LOADER_ROOT / "release-0.9.0-closure-v1.json")
        frozen_files = {
            item["path"]: item["sha256"]
            for item in frozen_snapshot.get("files", [])}
        current_files = {
            item["path"]: item["sha256"]
            for item in current_snapshot.get("files", [])}
        changed = {path for path in current_files
                   if frozen_files.get(path) != current_files[path]}
        require(closure.get("version") == "0.9.0" and
                closure.get("api") == {"major": 1, "minor": 3} and
                closure.get("safety", {}).get("api_080_preserved") is True and
                changed.issubset({
                    "VERSION", "CMakeLists.txt", "include/nxloader.h",
                    "src/nxloader_internal.h", "src/nxloader.c",
                    "src/nxloader_hooks.c", "src/nxloader_protect.c"}),
                "0.9.0 changed an M10 guest-inspector input outside its "
                "additive patch boundary")
    else:
        require(frozen_snapshot == current_snapshot,
                "guest evidence is detached from the current inspector sources")
    require(frozen_snapshot.get("sha256") == FINAL_SOURCE_SNAPSHOT_SHA256,
            "guest evidence lost its frozen inspector source snapshot")
    inspector_contract = evidence.get("inspector", {})
    require(inspector_contract.get("source_snapshot_sha256") ==
            frozen_snapshot["sha256"] and
            inspector_contract.get("timeout_seconds") == 120.0 and
            inspector_contract.get("output_limit_bytes_per_stream") ==
            262144 and
            inspector_contract.get("relocate_supported") is True and
            inspector_contract.get("exports_supported") is True and
            inspector_contract.get("sha256") == FINAL_INSPECTOR_SHA256 and
            inspector_contract.get("preflight_timed_out") is False and
            inspector_contract.get("preflight_output_limit_exceeded") is
            False and
            inspector_contract.get("preflight_termination_signal") is None and
            inspector_contract.get("preflight_max_rss_source") ==
            "wait4-ru_maxrss",
            "inspector preflight/source lock is incomplete")
    require(isinstance(evidence.get("total_elapsed_ns"), int) and
            0 < evidence["total_elapsed_ns"] < 10_000_000_000,
            "guest gate duration is absent or outside its reviewed budget")
    evidence_items = {item.get("id"): item
                      for item in evidence.get("guests", [])}
    require(set(evidence_items) == set(EXPECTED_GUESTS),
            "guest evidence identity set changed")
    guest_subprocesses = 0
    registry_exports = 0
    for guest_id, contract in EXPECTED_GUESTS.items():
        item = evidence_items[guest_id]
        inspector = item.get("inspector", {})
        registry = item.get("registry", {})
        expected_exports = {
            "eligible": contract["exports"],
            "added": contract["exports"],
            "equivalent": 0,
            "replaced_lower_priority": 0,
            "ignored_lower_priority": 0,
            "collisions": 0,
        }
        require(item.get("sha256") == contract["sha256"] and
                item.get("dynamic_symbol_count") == contract["symbols"] and
                item.get("relocation_count") == contract["relocations"] and
                item.get("init_array_entries") == contract["init_array"] and
                item.get("static_validation") == "PASS",
                "%s evidence inventory changed" % guest_id)
        require(inspector.get("mode") == "--relocate" and
                inspector.get("status") == "PASS" and
                inspector.get("resolve_executed") == 0 and
                inspector.get("finalize_executed") == 0 and
                inspector.get("guest_initializers_executed") == 0 and
                inspector.get("guest_jni_onload_executed") == 0 and
                inspector.get("max_rss_source") == "wait4-ru_maxrss" and
                inspector.get("timeout_seconds") == 120.0 and
                inspector.get("output_limit_bytes_per_stream") == 262144 and
                inspector.get("timed_out") is False and
                inspector.get("output_limit_exceeded") is False and
                inspector.get("stdout_truncated") is False and
                inspector.get("stderr_truncated") is False and
                inspector.get("termination_signal") is None and
                0 <= inspector.get("stdout_bytes", -1) <= 262144 and
                0 <= inspector.get("stderr_bytes", -1) <= 262144 and
                isinstance(inspector.get("elapsed_ns"), int) and
                0 < inspector["elapsed_ns"] < 5_000_000_000 and
                isinstance(inspector.get("max_rss_kib"), int) and
                0 < inspector["max_rss_kib"] < 512 * 1024,
                "%s inspector evidence is incomplete or unsafe" % guest_id)
        require(item.get("registry_exports") == expected_exports and
                registry.get("mode") == "--exports" and
                registry.get("status") == "PASS" and
                registry.get("eligible") == contract["exports"] and
                registry.get("added") == contract["exports"] and
                registry.get("equivalent") == 0 and
                registry.get("static_inventory") == expected_exports and
                registry.get("reported_exports") == {
                    "added": contract["exports"], "equivalent": 0} and
                registry.get("registry_create_executed") == 1 and
                registry.get("registry_add_module_executed") == 1 and
                registry.get("relocate_executed") == 1 and
                registry.get("resolve_executed") == 0 and
                registry.get("finalize_executed") == 0 and
                registry.get("guest_initializers_executed") == 0 and
                registry.get("guest_jni_onload_executed") == 0 and
                registry.get("device_access") == 0 and
                registry.get("guest_files_copied") == 0 and
                registry.get("max_rss_source") == "wait4-ru_maxrss" and
                registry.get("timeout_seconds") == 120.0 and
                registry.get("output_limit_bytes_per_stream") == 262144 and
                registry.get("timed_out") is False and
                registry.get("output_limit_exceeded") is False and
                registry.get("stdout_truncated") is False and
                registry.get("stderr_truncated") is False and
                registry.get("termination_signal") is None and
                0 <= registry.get("stdout_bytes", -1) <= 262144 and
                0 <= registry.get("stderr_bytes", -1) <= 262144 and
                isinstance(registry.get("elapsed_ns"), int) and
                0 < registry["elapsed_ns"] < 5_000_000_000 and
                isinstance(registry.get("max_rss_kib"), int) and
                0 < registry["max_rss_kib"] < 512 * 1024,
                "%s exports/registry evidence is incomplete or unsafe" %
                guest_id)
        guest_subprocesses += 2
        registry_exports += registry["added"]
        require(set(item.get("prohibitions", {}).values()) == {0},
                "%s has a forbidden ELF feature" % guest_id)
    require(guest_subprocesses == 10 and registry_exports == 81306 and
            log_text.count("relocate=PASS") == 5 and
            log_text.count("exports=PASS") == 5 and
            "modes=--relocate,--exports" in log_text and
            ("source_snapshot_sha256=%s files=11" %
             frozen_snapshot["sha256"]) in log_text and
            "timeout_seconds=120.0 output_limit_bytes_per_stream=262144" in
            log_text and
            "PASS guests=5" in log_text and
            "guest_initializers_executed=0" in log_text and
            "guest_jni_onload_executed=0" in log_text and
            "device_access=0" in log_text,
            "sanitized guest log is incomplete")
    require("/tmp/" not in evidence_text and "/tmp/" not in log_text,
            "guest evidence leaks a temporary host path")
    reject_sensitive_location(expected_text, "approved guest manifest")
    reject_sensitive_location(evidence_text, "approved guest evidence")
    reject_sensitive_location(log_text, "approved guest log")


def validate_matrix():
    matrix = load_json(MATRIX_PATH)
    gates = {item["id"]: item for item in matrix.get("gates", [])}
    require(gates.get("nxloader-m10-audit", {}).get("class") == "pure" and
            gates.get("nxloader-m10-audit", {}).get("automatic") is True and
            gates.get("nxloader-m10-audit", {}).get("signals") == [],
            "M10 audit is not an automatic signal-free pure gate")
    require(gates.get("nxloader-aarch64-cross", {}).get("class") ==
            "filesystem" and
            gates.get("nxloader-aarch64-cross", {}).get("automatic") is True,
            "AArch64 cross gate is not automatic/filesystem")
    require("framework/nxloader/tests/test_pending.c" in
            gates.get("nxloader-host", {}).get("sources", []),
            "pending transaction test is not assigned to the host gate")


def main():
    audit = load_json(AUDIT_PATH)
    require(set(audit) == {
        "schema_version", "milestone", "scope", "physical_device_evidence",
        "approved_guest_relocation_evidence", "guest_initializers_executed",
        "guest_jni_onload_executed", "requirements",
    }, "M10 audit has an unknown top-level field")
    require(audit.get("schema_version") == 1 and
            audit.get("milestone") == "M10" and
            audit.get("scope") ==
            "aarch64-rela-aapcs64-cross-and-approved-read-only-guest-relocation" and
            audit.get("physical_device_evidence") is False and
            audit.get("approved_guest_relocation_evidence") is True and
            audit.get("guest_initializers_executed") is False and
            audit.get("guest_jni_onload_executed") is False,
            "M10 audit header/evidence status changed")
    validate_references(audit)
    validate_contract()
    validate_backend_and_transactions()
    validate_cross_gate()
    validate_abi_matrix()
    validate_guest_runner()
    validate_guest_evidence()
    validate_matrix()
    for path in PRIVACY_FILES:
        require(path.is_file() and not path.is_symlink(),
                "M10 privacy input is missing/linked: %s" % path)
        reject_sensitive_location(path.read_text(encoding="utf-8"),
                                  str(path.relative_to(REPO_ROOT)))
    print("M10 audit gate passed: 20 requirements, 84 AArch64 ABI rows, "
          "approved_guests=5 guest_subprocesses=10 registry_exports=81306 "
          "guest_runner_selftests=9 physical_device_evidence=0 "
          "guest_initializers_executed=0 guest_jni_onload_executed=0")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, json.JSONDecodeError) as error:
        print("M10 audit gate failed: %s" % error)
        raise SystemExit(1)
