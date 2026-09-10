#!/usr/bin/env python3
"""Directed fail-closed regressions for the M124 receipt validator."""

import copy
import datetime as dt
import importlib.util
import os
from pathlib import Path
import tempfile


RUNNER = Path(__file__).with_name("run_aarch64_icache_attestation.py")
SPEC = importlib.util.spec_from_file_location("m124_runner", RUNNER)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)
CLOSURE = Path(__file__).with_name("test_090_closure.py")
CLOSURE_SPEC = importlib.util.spec_from_file_location("nxloader_090_closure",
                                                      CLOSURE)
closure = importlib.util.module_from_spec(CLOSURE_SPEC)
CLOSURE_SPEC.loader.exec_module(closure)


def require_failure(callback, label):
    try:
        callback()
    except runner.AttestationError:
        return
    raise AssertionError("validator accepted %s" % label)


def write(path, payload):
    path.write_bytes(payload)
    os.chmod(path, 0o500)


def valid_runtime_line():
    contract = runner.load_contract()
    tokens = list(contract["required_runtime_tokens"].values())
    if "same_mapping_rewrite=1" not in tokens or "patch_window_rw=1" not in tokens:
        raise AssertionError("same-mapping patch scenarios are absent")
    tokens.extend([
        "direct_result_a=1",
        "hook_result_b=1",
        "positive_mprotect_harness=0",
        "finalize_only_loader_cache_clear=1",
        "external_guest_elf_loaded=0",
        "guest_initializers_executed=0",
        "device_access=0",
        "hardware_claim=none",
        "core_dump=0",
        "page_size=4096",
        "cache_line_size=64",
    ])
    return "aarch64-cross: PASS " + " ".join(tokens)


def main():
    closure.main()
    base = os.environ.get("NXLOADER_TEST_TMPDIR")
    with tempfile.TemporaryDirectory(prefix="nxloader-m124-validator.",
                                     dir=base) as temporary:
        root = Path(temporary)
        gcc = root / "nxloader-m124-gcc-aarch64"
        clang = root / "nxloader-m124-clang-aarch64"
        write(gcc, b"\x7fELFgcc-owner-fixture")
        write(clang, b"\x7fELFclang-owner-fixture")
        log = root / "m124-attestation.log"
        log.write_text("M124 directed fixture\n", encoding="utf-8")
        now = runner.utc_now()
        receipt_path = root / "m124-aarch64-icache-receipt-v1.json"
        document = runner.build_receipt(
            "EMULATED", now, now, runner.source_manifest(),
            [
                {"name": gcc.name, "sha256": runner.sha256_file(gcc),
                 "executed": True, "returncode": 0},
                {"name": clang.name, "sha256": runner.sha256_file(clang),
                 "executed": True, "returncode": 0},
            ],
            {"gcc": "gcc pinned", "clang": "clang pinned",
             "lld": "lld pinned", "qemu": "qemu pinned"},
            valid_runtime_line(),
            {"page_size": 4096, "cache_line_size": 64,
             "max_glibc": "2.28", "elf_count": 20},
            log.name, runner.sha256_file(log))
        runner.validate_document(document, receipt_path, now=now)
        guards = 1

        compiled_line = (
            "aarch64-cross: COMPILED_ONLY gcc=1 clang=1 lld=1 qemu=0 "
            "executed=0 hardware_ran=0 device_access=0 physical=PENDING")
        compiled = runner.build_receipt(
            "COMPILED_ONLY", now, now, runner.source_manifest(),
            [
                {"name": gcc.name, "sha256": runner.sha256_file(gcc),
                 "executed": False, "returncode": None},
                {"name": clang.name, "sha256": runner.sha256_file(clang),
                 "executed": False, "returncode": None},
            ],
            {"gcc": "gcc pinned", "clang": "clang pinned",
             "lld": "lld pinned", "qemu": "not-used"},
            compiled_line,
            {"page_size": None, "cache_line_size": None,
             "max_glibc": "2.28", "elf_count": 20},
            log.name, runner.sha256_file(log))
        runner.validate_document(compiled, receipt_path, now=now)
        guards += 1

        compiled_gate = "\n".join([
            "aarch64-cross: clang=clang pinned",
            "aarch64-cross: lld=lld pinned",
            "aarch64-cross: qemu=not-used",
            "aarch64-linux-gnu-gcc (Debian 8.3.0) 8.3.0",
            compiled_line,
            "",
        ])
        parsed_compiled = runner.parse_compiled_gate_output(compiled_gate)
        if parsed_compiled["terminal_line"] != compiled_line:
            raise AssertionError("compiled-only gate parser lost its terminal")
        guards += 1

        metadata = root / "compiled.metadata"
        metadata.write_text("\n".join([
            "schema=nxloader-m124-gate-metadata-v2",
            "evidence_class=COMPILED_ONLY",
            "source_manifest_sha256=" + "1" * 64,
            "gcc_binary_sha256=" + "2" * 64,
            "clang_binary_sha256=" + "3" * 64,
            "gcc_returncode=not-run",
            "clang_returncode=not-run",
            "terminal_sha256=" + runner.hashlib.sha256(
                compiled_line.encode("ascii")).hexdigest(),
            "page_size=not-measured",
            "cache_line_size=not-measured",
            "max_glibc=2.28",
            "elf_count=20",
            "",
        ]), encoding="ascii")
        parsed_metadata = runner.parse_metadata(metadata)
        if parsed_metadata["evidence_class"] != "COMPILED_ONLY":
            raise AssertionError("compiled-only metadata class was lost")
        guards += 1
        forged_metadata = metadata.read_text(encoding="ascii").replace(
            "evidence_class=COMPILED_ONLY", "evidence_class=PHYSICAL")
        metadata.write_text(forged_metadata, encoding="ascii")
        require_failure(lambda: runner.parse_metadata(metadata),
                        "physical gate metadata forgery")
        guards += 1

        cases = []
        changed = copy.deepcopy(document)
        changed["source_manifest_sha256"] = "0" * 64
        cases.append((changed, "source divergence"))
        changed = copy.deepcopy(document)
        changed["artifacts"][0]["sha256"] = "0" * 64
        cases.append((changed, "binary divergence"))
        changed = copy.deepcopy(document)
        changed["artifacts"][1] = copy.deepcopy(changed["artifacts"][0])
        cases.append((changed, "duplicate artifact"))
        changed = copy.deepcopy(document)
        changed["scenarios"].pop()
        cases.append((changed, "missing scenario"))
        changed = copy.deepcopy(document)
        changed["execution"]["returncode"] = 124
        cases.append((changed, "timeout return code"))
        changed = copy.deepcopy(document)
        changed["execution"]["log_sha256"] = "0" * 64
        cases.append((changed, "battery log divergence"))
        changed = copy.deepcopy(document)
        changed["measurements"]["max_glibc"] = "2.31"
        cases.append((changed, "GLIBC ceiling forgery"))
        changed = copy.deepcopy(document)
        changed["measurements"]["elf_count"] = 19
        cases.append((changed, "ELF count forgery"))
        changed = copy.deepcopy(document)
        changed["execution"]["terminal_line"] += " range_negative=1"
        changed["execution"]["terminal_sha256"] = runner.hashlib.sha256(
            changed["execution"]["terminal_line"].encode("ascii")).hexdigest()
        cases.append((changed, "duplicate runtime token"))
        changed = copy.deepcopy(document)
        changed["toolchain"]["qemu"] = "/home/owner/qemu"
        cases.append((changed, "private path"))
        changed = copy.deepcopy(compiled)
        changed["status"] = "PASS"
        changed["execution"]["executed"] = True
        changed["execution"]["returncode"] = 0
        cases.append((changed, "compiled-only execution forgery"))
        changed = copy.deepcopy(document)
        changed["evidence_class"] = "PHYSICAL"
        changed["hardware_ran"] = True
        changed["emulator"] = False
        changed["physical_pending"] = False
        changed["physical_context_sha256"] = "f" * 64
        cases.append((changed, "physical forged context"))
        stale_now = now + dt.timedelta(days=2)
        require_failure(lambda: runner.validate_document(
            document, receipt_path, max_age_seconds=86400, now=stale_now),
                        "stale receipt")
        guards += 1

        require_failure(lambda: runner.build_receipt(
            "PHYSICAL", now, now, runner.source_manifest(), [], {}, "", {},
            log.name, runner.sha256_file(log), context_sha="f" * 64),
                        "owner-local physical receipt emission")
        guards += 1
        help_text = runner.parser().format_help()
        if "compile-only" not in help_text or "run-physical" in help_text:
            raise AssertionError("runner command classes are unsafe")
        guards += 1
        for changed, label in cases:
            require_failure(lambda value=changed: runner.validate_document(
                value, receipt_path, now=now), label)
            guards += 1

        duplicate_json = root / "duplicate.json"
        duplicate_json.write_text('{"schema":1,"schema":1}\n',
                                  encoding="utf-8")
        require_failure(lambda: runner.load_json_unique(duplicate_json),
                        "duplicate JSON key")
        guards += 1
        trailing_json = root / "trailing.json"
        trailing_json.write_text('{}\n{}\n', encoding="utf-8")
        require_failure(lambda: runner.load_json_unique(trailing_json),
                        "duplicate JSON document")
        guards += 1
        truncated_json = root / "truncated.json"
        truncated_json.write_text('{"schema":1}', encoding="utf-8")
        require_failure(lambda: runner.load_json_unique(truncated_json),
                        "truncated JSON")
        guards += 1

    print("nxloader M124 attestation subgate passed: guards=%d "
          "classes=COMPILED_ONLY,EMULATED hardware_ran=0 physical=PENDING" %
          guards)


if __name__ == "__main__":
    main()
