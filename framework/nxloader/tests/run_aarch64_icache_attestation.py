#!/usr/bin/env python3
"""Collect and validate the M124 AArch64 I-cache attestation.

Evidence class is always explicit. The owner-local commands emit either
COMPILED_ONLY/PENDING without target execution or EMULATED/PASS through the
canonical GCC+Clang/LLD QEMU gate. PHYSICAL remains PENDING and cannot be
emitted or promoted until an external trust anchor is specified and implemented.
"""

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import secrets
import signal
import stat
import subprocess
import sys
import time


SCHEMA = "nxloader-aarch64-icache-attestation-v1"
METADATA_SCHEMA = "nxloader-m124-gate-metadata-v2"
TERMINAL_PREFIX = "NXLOADER_M124_ATTESTATION"
LOADER_ROOT = Path(__file__).resolve().parents[1]
CONTRACT_PATH = LOADER_ROOT / "m124-aarch64-icache-attestation-v1.json"
GATE_PATH = LOADER_ROOT / "tests/run-aarch64-cross.sh"
SOURCE_RELATIVE = (
    "VERSION",
    "CMakeLists.txt",
    "CHANGELOG.md",
    "README.md",
    "include/nxloader.h",
    "src/nxloader_internal.h",
    "src/nxloader.c",
    "src/nxloader_elf32.c",
    "src/nxloader_elf64.c",
    "src/nxloader_hooks.c",
    "src/nxloader_protect.c",
    "src/nxloader_registry.c",
    "tests/test_aarch64_cross.c",
    "tests/run-aarch64-cross.sh",
    "tests/run_aarch64_icache_attestation.py",
    "tests/test_m124_attestation.py",
    "tests/test_090_closure.py",
    "m124-aarch64-icache-attestation-v1.json",
    "release-0.8.0-closure-v1.json",
    "release-0.9.0-closure-v1.json",
)
RECEIPT_KEYS = {
    "schema", "schema_version", "run_id", "evidence_class",
    "architecture", "status", "hardware_ran", "emulator",
    "physical_pending", "source_manifest_sha256", "runner_sha256",
    "artifacts", "toolchain", "loader", "measurements", "scenarios",
    "execution", "started_at", "finished_at", "reason",
    "physical_context_sha256",
}
HOSTILE = re.compile(
    r"(?:/home/|/mnt/|/Users/|[A-Za-z]:\\|\b(?:\d{1,3}\.){3}\d{1,3}\b|"
    r"\b(?:hostname|device_name)\b)", re.IGNORECASE)
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX32 = re.compile(r"^[0-9a-f]{32}$")
SAFE_LABEL = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ._:+()/-]{0,159}$")
UTC_FORMAT = "%Y-%m-%dT%H:%M:%SZ"


class AttestationError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise AttestationError(message)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_json_unique(path):
    raw = path.read_bytes()
    require(raw and raw.endswith(b"\n"), "JSON is empty or truncated")

    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key: %s" % key)
            result[key] = value
        return result

    try:
        text = raw.decode("utf-8")
        decoder = json.JSONDecoder(object_pairs_hook=unique)
        document, end = decoder.raw_decode(text)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AttestationError("invalid JSON: %s" % error) from error
    require(not text[end:].strip(), "duplicate/trailing JSON document")
    require(isinstance(document, dict), "JSON root is not an object")
    return document


def canonical_bytes(document):
    return (json.dumps(document, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True) + "\n").encode("ascii")


def write_exclusive(path, payload, mode=0o600):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(str(path), flags, mode)
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        os.close(descriptor)


def regular_unlinked(path):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return stat.S_ISREG(info.st_mode) and not path.is_symlink()


def parse_utc(value):
    require(isinstance(value, str), "timestamp is not a string")
    try:
        return dt.datetime.strptime(value, UTC_FORMAT).replace(
            tzinfo=dt.timezone.utc)
    except ValueError as error:
        raise AttestationError("timestamp is not canonical UTC") from error


def utc_now():
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0)


def source_manifest(root=LOADER_ROOT):
    lines = []
    for relative in SOURCE_RELATIVE:
        path = root / relative
        require(regular_unlinked(path), "source missing or linked: %s" % relative)
        lines.append((sha256_file(path) + "\n").encode("ascii"))
    return hashlib.sha256(b"".join(lines)).hexdigest()


def load_contract():
    contract = load_json_unique(CONTRACT_PATH)
    expected = {
        "schema", "schema_version", "component_version", "api",
        "runtime_probe", "runner", "validator_test", "canonical_finalize",
        "evidence_classes", "scenarios", "required_runtime_tokens",
        "negative_claims",
    }
    require(set(contract) == expected, "contract fields changed")
    require(contract.get("schema") ==
            "nxloader-m124-attestation-contract-v1", "wrong contract schema")
    require(type(contract.get("schema_version")) is int and
            contract["schema_version"] == 1 and
            contract.get("component_version") == "0.9.0" and
            contract.get("api") == {"major": 1, "minor": 3} and
            contract.get("canonical_finalize") == "nxloader_module_finalize",
            "contract version/API/finalize is incoherent")
    classes = contract.get("evidence_classes")
    require(isinstance(classes, dict) and
            classes.get("COMPILED_ONLY") == {
                "status": "PENDING", "hardware_ran": False,
                "emulator": False, "physical_pending": True} and
            classes.get("EMULATED") == {
                "status": "PASS", "hardware_ran": False,
                "emulator": True, "physical_pending": True} and
            classes.get("PHYSICAL") == {
                "status": "PENDING", "hardware_ran": False,
                "emulator": False, "physical_pending": True,
                "requires_trust_anchor": True, "runner_available": False},
            "evidence classes permit a false hardware/compiled PASS")
    scenarios = contract.get("scenarios")
    tokens = contract.get("required_runtime_tokens")
    require(isinstance(scenarios, list) and len(scenarios) == 16,
            "scenario matrix is incomplete")
    require(isinstance(tokens, dict) and set(tokens) == set(scenarios),
            "runtime token matrix differs from scenarios")
    require(len(scenarios) == len(set(scenarios)), "duplicate scenario")
    return contract


def parse_metadata(path):
    entries = {}
    raw = path.read_text(encoding="ascii")
    require(raw.endswith("\n"), "gate metadata is truncated")
    for line in raw.splitlines():
        require(line.count("=") == 1, "malformed gate metadata line")
        key, value = line.split("=", 1)
        require(key not in entries, "duplicate gate metadata key")
        entries[key] = value
    expected = {
        "schema", "evidence_class", "source_manifest_sha256", "gcc_binary_sha256",
        "clang_binary_sha256", "gcc_returncode", "clang_returncode",
        "terminal_sha256", "page_size", "cache_line_size", "max_glibc",
        "elf_count",
    }
    require(set(entries) == expected, "gate metadata fields changed")
    require(entries["schema"] == METADATA_SCHEMA, "wrong metadata schema")
    evidence_class = entries["evidence_class"]
    require(evidence_class in {"COMPILED_ONLY", "EMULATED"},
            "metadata evidence class is invalid")
    for key in ("source_manifest_sha256", "gcc_binary_sha256",
                "clang_binary_sha256", "terminal_sha256"):
        require(HEX64.fullmatch(entries[key]) is not None,
                "malformed metadata hash: %s" % key)
    if evidence_class == "COMPILED_ONLY":
        require(entries["gcc_returncode"] == "not-run" and
                entries["clang_returncode"] == "not-run" and
                entries["page_size"] == "not-measured" and
                entries["cache_line_size"] == "not-measured",
                "compiled-only metadata claims target execution")
    else:
        for key in ("gcc_returncode", "clang_returncode"):
            require(entries[key] == "0", "non-zero compiler runtime result")
        for key in ("page_size", "cache_line_size"):
            require(entries[key].isdigit() and int(entries[key]) > 0,
                    "invalid numeric metadata: %s" % key)
    require(entries["elf_count"] == "20",
            "gate metadata ELF count is not canonical")
    glibc_match = re.fullmatch(r"([0-9]+)\.([0-9]+)",
                               entries["max_glibc"])
    require(glibc_match is not None and
            (int(glibc_match.group(1)), int(glibc_match.group(2))) <= (2, 30),
            "gate metadata exceeds GLIBC_2.30")
    return entries


def safe_label(value, field):
    require(isinstance(value, str) and SAFE_LABEL.fullmatch(value),
            "%s is not a safe label" % field)
    require(HOSTILE.search(value) is None, "%s contains private data" % field)
    return value


def parse_runtime_line(line, contract):
    prefix = "aarch64-cross: PASS "
    require(line.startswith(prefix), "runtime terminal line is absent")
    pairs = {}
    for token in line[len(prefix):].split():
        require(token.count("=") == 1, "malformed runtime token")
        key, value = token.split("=", 1)
        require(key not in pairs, "duplicate runtime token: %s" % key)
        require(re.fullmatch(r"[a-z0-9_]+", key) is not None,
                "hostile runtime key")
        require(re.fullmatch(r"[A-Za-z0-9_.:+/-]+", value) is not None,
                "hostile runtime value")
        pairs[key] = value
    for scenario, token in contract["required_runtime_tokens"].items():
        key, expected = token.split("=", 1)
        require(pairs.get(key) == expected,
                "runtime did not prove scenario %s" % scenario)
    invariants = {
        "direct_result_a": "1",
        "hook_result_b": "1",
        "same_mapping_rewrite": "1",
        "patch_window_rw": "1",
        "positive_mprotect_harness": "0",
        "finalize_only_loader_cache_clear": "1",
        "external_guest_elf_loaded": "0",
        "guest_initializers_executed": "0",
        "device_access": "0",
        "hardware_claim": "none",
        "core_dump": "0",
    }
    for key, expected in invariants.items():
        require(pairs.get(key) == expected,
                "runtime invariant failed: %s" % key)
    page_size = pairs.get("page_size", "")
    line_size = pairs.get("cache_line_size", "")
    require(page_size.isdigit() and int(page_size) >= 4096,
            "invalid measured page size")
    require(line_size.isdigit() and int(line_size) >= 16 and
            (int(line_size) & (int(line_size) - 1)) == 0,
            "invalid measured cache-line size")
    return pairs


def parse_compiled_line(line):
    expected = ("aarch64-cross: COMPILED_ONLY gcc=1 clang=1 lld=1 qemu=0 "
                "executed=0 hardware_ran=0 device_access=0 physical=PENDING")
    require(line == expected, "compiled-only terminal line changed")
    return line


def stream_process(command, cwd, environment, timeout_seconds):
    process = subprocess.Popen(
        command, cwd=str(cwd), env=environment, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8",
        errors="strict", bufsize=1, start_new_session=True)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout_seconds
    chunks = []
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                raise AttestationError("execution deadline exceeded")
            events = selector.select(min(remaining, 0.25))
            for key, _ in events:
                line = key.fileobj.readline()
                if line:
                    sys.stdout.write(line)
                    sys.stdout.flush()
                    chunks.append(line)
            if process.poll() is not None:
                remainder = process.stdout.read()
                if remainder:
                    sys.stdout.write(remainder)
                    sys.stdout.flush()
                    chunks.append(remainder)
                break
    finally:
        selector.close()
    return process.returncode, "".join(chunks)


def prepare_output(path):
    require(not path.exists() and not path.is_symlink(),
            "output directory already exists")
    path.mkdir(mode=0o700, parents=False)
    require(path.is_dir() and not path.is_symlink(),
            "output directory is not private/regular")


def parse_gate_output(output, contract):
    lines = output.splitlines()
    runtime = [line for line in lines
               if line.startswith("aarch64-cross: PASS lp64=1 ")]
    require(len(runtime) == 2 and runtime[0] == runtime[1],
            "expected two identical compiler runtime lines")
    runtime_pairs = parse_runtime_line(runtime[0], contract)
    summaries = [line for line in lines
                 if line.startswith("aarch64-cross: PASS gcc=1 ")]
    require(len(summaries) == 1, "gate summary is missing or duplicated")

    def one(prefix):
        matches = [line[len(prefix):] for line in lines
                   if line.startswith(prefix)]
        require(len(matches) == 1, "toolchain line changed: %s" % prefix)
        return safe_label(matches[0], prefix.rstrip("="))

    gcc_lines = [line for line in lines
                 if line.startswith("aarch64-linux-gnu-gcc (")]
    require(len(gcc_lines) == 1, "pinned GCC version line changed")
    return {
        "runtime_line": runtime[0],
        "runtime_pairs": runtime_pairs,
        "summary_sha256": hashlib.sha256(
            summaries[0].encode("ascii")).hexdigest(),
        "gcc": safe_label(gcc_lines[0], "gcc"),
        "clang": one("aarch64-cross: clang="),
        "lld": one("aarch64-cross: lld="),
        "qemu": one("aarch64-cross: qemu="),
    }


def parse_compiled_gate_output(output):
    lines = output.splitlines()
    terminals = [line for line in lines
                 if line.startswith("aarch64-cross: COMPILED_ONLY ")]
    require(len(terminals) == 1, "compiled-only terminal line is missing")
    parse_compiled_line(terminals[0])

    def one(prefix):
        matches = [line[len(prefix):] for line in lines
                   if line.startswith(prefix)]
        require(len(matches) == 1, "toolchain line changed: %s" % prefix)
        return safe_label(matches[0], prefix.rstrip("="))

    gcc_lines = [line for line in lines
                 if line.startswith("aarch64-linux-gnu-gcc (")]
    require(len(gcc_lines) == 1, "pinned GCC version line changed")
    require(one("aarch64-cross: qemu=") == "not-used",
            "compiled-only gate invoked QEMU")
    return {
        "terminal_line": terminals[0],
        "gcc": safe_label(gcc_lines[0], "gcc"),
        "clang": one("aarch64-cross: clang="),
        "lld": one("aarch64-cross: lld="),
        "qemu": "not-used",
    }


def build_receipt(evidence_class, started, finished, source_sha, artifacts,
                  toolchain, runtime_line, measurements, log_name, log_sha,
                  context_sha=None):
    contract = load_contract()
    require(evidence_class in {"COMPILED_ONLY", "EMULATED"},
            "PHYSICAL is unavailable without a trusted attestation anchor")
    require(context_sha is None,
            "untrusted physical context cannot enter an owner-local receipt")
    executed = evidence_class == "EMULATED"
    return {
        "schema": SCHEMA,
        "schema_version": 1,
        "run_id": secrets.token_hex(16),
        "evidence_class": evidence_class,
        "architecture": "aarch64",
        "status": "PASS" if executed else "PENDING",
        "hardware_ran": False,
        "emulator": executed,
        "physical_pending": True,
        "source_manifest_sha256": source_sha,
        "runner_sha256": sha256_file(Path(__file__).resolve()),
        "artifacts": artifacts,
        "toolchain": toolchain,
        "loader": {"version": "0.9.0", "api_major": 1, "api_minor": 3},
        "measurements": measurements,
        "scenarios": contract["scenarios"],
        "execution": {
            "executed": executed,
            "returncode": 0 if executed else None,
            "terminal_line": runtime_line,
            "terminal_sha256": hashlib.sha256(
                runtime_line.encode("ascii")).hexdigest(),
            "log_name": log_name,
            "log_sha256": log_sha,
        },
        "started_at": started.strftime(UTC_FORMAT),
        "finished_at": finished.strftime(UTC_FORMAT),
        "reason": ("qemu_flow_passed_physical_pending" if executed else
                   "compiled_without_execution_physical_pending"),
        "physical_context_sha256": None,
    }


def validate_document(document, receipt_path, max_age_seconds=86400,
                      now=None):
    require(set(document) == RECEIPT_KEYS, "receipt fields changed")
    require(document.get("schema") == SCHEMA and
            type(document.get("schema_version")) is int and
            document.get("schema_version") == 1, "wrong receipt schema")
    require(isinstance(document.get("run_id"), str) and
            HEX32.fullmatch(document["run_id"]), "invalid run id")
    evidence_class = document.get("evidence_class")
    require(evidence_class in {"COMPILED_ONLY", "EMULATED", "PHYSICAL"},
            "invalid evidence class")
    require(evidence_class != "PHYSICAL",
            "PHYSICAL remains PENDING until a trust anchor is implemented")
    expected_status = "PASS" if evidence_class == "EMULATED" else "PENDING"
    require(document.get("architecture") == "aarch64" and
            document.get("status") == expected_status,
            "invalid status/architecture")
    booleans = ("hardware_ran", "emulator", "physical_pending")
    require(all(type(document.get(key)) is bool for key in booleans),
            "evidence flags are not booleans")
    expected_flags = {
        "COMPILED_ONLY": (False, False, True),
        "EMULATED": (False, True, True),
    }[evidence_class]
    require(tuple(document[key] for key in booleans) == expected_flags,
            "evidence class flags are contradictory")
    require(document.get("physical_context_sha256") is None,
            "owner-local receipt contains an untrusted physical context")

    for key in ("source_manifest_sha256", "runner_sha256"):
        require(isinstance(document.get(key), str) and
                HEX64.fullmatch(document[key]), "invalid receipt hash: %s" % key)
    require(document["source_manifest_sha256"] == source_manifest(),
            "receipt is stale or sources diverged")
    require(document["runner_sha256"] == sha256_file(Path(__file__).resolve()),
            "runner hash diverged")

    contract = load_contract()
    require(document.get("scenarios") == contract["scenarios"],
            "scenario list missing, reordered or duplicated")
    execution = document.get("execution")
    require(isinstance(execution, dict) and set(execution) ==
            {"executed", "returncode", "terminal_line", "terminal_sha256",
             "log_name", "log_sha256"},
            "execution fields changed")
    expected_executed = evidence_class == "EMULATED"
    require(type(execution["executed"]) is bool and
            execution["executed"] is expected_executed,
            "execution flag contradicts evidence class")
    require(execution["returncode"] == (0 if expected_executed else None),
            "execution return code contradicts evidence class")
    if expected_executed:
        runtime_pairs = parse_runtime_line(execution["terminal_line"], contract)
    else:
        parse_compiled_line(execution["terminal_line"])
        runtime_pairs = None
    require(hashlib.sha256(execution["terminal_line"].encode("ascii")).hexdigest()
            == execution["terminal_sha256"], "terminal line hash mismatch")
    require(execution["log_name"] == "m124-attestation.log" and
            isinstance(execution["log_sha256"], str) and
            HEX64.fullmatch(execution["log_sha256"]),
            "attestation log claim is malformed")
    log_path = receipt_path.parent / execution["log_name"]
    require(regular_unlinked(log_path) and
            sha256_file(log_path) == execution["log_sha256"],
            "attestation log is missing, linked or divergent")

    measurements = document.get("measurements")
    require(isinstance(measurements, dict) and set(measurements) ==
            {"page_size", "cache_line_size", "max_glibc", "elf_count"},
            "measurement fields changed")
    if expected_executed:
        require(type(measurements["page_size"]) is int and
                measurements["page_size"] == int(runtime_pairs["page_size"]),
                "page-size claim differs from runtime")
        require(type(measurements["cache_line_size"]) is int and
                measurements["cache_line_size"] ==
                int(runtime_pairs["cache_line_size"]),
                "cache-line claim differs from runtime")
    else:
        require(measurements["page_size"] is None and
                measurements["cache_line_size"] is None,
                "compiled-only receipt claims runtime measurements")
    require(type(measurements["elf_count"]) is int and
            measurements["elf_count"] == 20, "invalid ELF count")
    max_glibc = safe_label(measurements["max_glibc"], "max_glibc")
    glibc_match = re.fullmatch(r"([0-9]+)\.([0-9]+)", max_glibc)
    require(glibc_match is not None and
            (int(glibc_match.group(1)), int(glibc_match.group(2))) <= (2, 30),
            "receipt exceeds GLIBC_2.30")

    loader = document.get("loader")
    require(isinstance(loader, dict) and
            type(loader.get("api_major")) is int and
            type(loader.get("api_minor")) is int and
            loader == {"version": "0.9.0", "api_major": 1,
                       "api_minor": 3}, "loader version/API claim changed")
    toolchain = document.get("toolchain")
    require(isinstance(toolchain, dict) and set(toolchain) ==
            {"gcc", "clang", "lld", "qemu"}, "toolchain fields changed")
    for key, value in toolchain.items():
        safe_label(value, key)
    require((toolchain["qemu"] == "not-used") is (not expected_executed),
            "QEMU claim contradicts evidence class")

    artifacts = document.get("artifacts")
    require(isinstance(artifacts, list) and len(artifacts) == 2,
            "artifact list is invalid")
    names = set()
    for artifact in artifacts:
        require(isinstance(artifact, dict) and set(artifact) ==
                {"name", "sha256", "executed", "returncode"},
                "artifact fields changed")
        name = artifact["name"]
        require(re.fullmatch(r"nxloader-m124-(?:gcc|clang)-aarch64", name),
                "artifact name is not canonical")
        require(name not in names, "duplicate artifact")
        names.add(name)
        require(isinstance(artifact["sha256"], str) and
                HEX64.fullmatch(artifact["sha256"]) and
                type(artifact["executed"]) is bool and
                artifact["executed"] is expected_executed and
                artifact["returncode"] == (0 if expected_executed else None),
                "artifact claim malformed")
        path = receipt_path.parent / name
        require(regular_unlinked(path), "artifact missing or linked")
        require(sha256_file(path) == artifact["sha256"],
                "artifact binary hash diverged")
        with path.open("rb") as stream:
            require(stream.read(4) == b"\x7fELF", "artifact is not ELF")

    started = parse_utc(document.get("started_at"))
    finished = parse_utc(document.get("finished_at"))
    require(started <= finished, "timestamps are reversed")
    current = now or utc_now()
    require(finished <= current + dt.timedelta(seconds=5),
            "receipt timestamp is in the future")
    if max_age_seconds > 0:
        require(current - finished <= dt.timedelta(seconds=max_age_seconds),
                "receipt is stale")
    expected_reason = ("qemu_flow_passed_physical_pending" if expected_executed
                       else "compiled_without_execution_physical_pending")
    require(document.get("reason") == expected_reason, "invalid reason")
    require(HOSTILE.search(canonical_bytes(document).decode("ascii")) is None,
            "receipt contains a private path, host or address")


def emit_terminal(document, receipt_path, action):
    print("%s action=%s schema=%s class=%s status=%s hardware_ran=%d "
          "emulator=%d physical=%s receipt_sha256=%s" % (
              TERMINAL_PREFIX, action, SCHEMA, document["evidence_class"],
              document["status"], int(document["hardware_ran"]),
              int(document["emulator"]), "PENDING",
              sha256_file(receipt_path)))


def command_run(arguments):
    test_environment = os.environ.copy()
    test_environment["PYTHONDONTWRITEBYTECODE"] = "1"
    started = utc_now()
    before_source = source_manifest()
    test_returncode, test_transcript = stream_process(
        [sys.executable, "-B", str(LOADER_ROOT / "tests/test_m124_attestation.py")],
        LOADER_ROOT, test_environment, arguments.timeout_seconds)
    require(test_returncode == 0,
            "receipt validator self-test returned %d" % test_returncode)
    require(source_manifest() == before_source,
            "source changed while the validator self-test was running")
    output = arguments.output_dir.resolve()
    prepare_output(output)
    metadata_path = output / ".gate-metadata"
    write_exclusive(metadata_path, b"", 0o600)
    environment = os.environ.copy()
    environment["NXLOADER_M124_METADATA_PATH"] = str(metadata_path)
    environment["NXLOADER_M124_ARTIFACT_DIR"] = str(output)
    environment["NXLOADER_QEMU_TIMEOUT_SECONDS"] = str(
        arguments.qemu_timeout_seconds)
    returncode, gate_transcript = stream_process(
        ["bash", str(GATE_PATH)], LOADER_ROOT, environment,
        arguments.timeout_seconds)
    require(returncode == 0, "canonical gate returned %d" % returncode)
    require(source_manifest() == before_source,
            "source changed while the gate was running")
    transcript = test_transcript + gate_transcript
    log_path = output / "m124-attestation.log"
    write_exclusive(log_path, transcript.encode("utf-8"), 0o600)
    parsed = parse_gate_output(gate_transcript, load_contract())
    metadata = parse_metadata(metadata_path)
    require(metadata["evidence_class"] == "EMULATED",
            "canonical run did not produce emulated metadata")
    require(metadata["source_manifest_sha256"] == before_source,
            "gate source manifest differs from runner")
    require(metadata["terminal_sha256"] == hashlib.sha256(
        parsed["runtime_line"].encode("ascii")).hexdigest(),
        "runtime line differs from gate metadata")
    require(int(metadata["page_size"]) ==
            int(parsed["runtime_pairs"]["page_size"]) and
            int(metadata["cache_line_size"]) ==
            int(parsed["runtime_pairs"]["cache_line_size"]),
            "runtime measurements differ from gate metadata")
    artifacts = []
    for compiler in ("gcc", "clang"):
        name = "nxloader-m124-%s-aarch64" % compiler
        artifact = output / name
        expected = metadata[compiler + "_binary_sha256"]
        require(regular_unlinked(artifact) and sha256_file(artifact) == expected,
                "exported %s artifact diverged" % compiler)
        artifacts.append({"name": name, "sha256": expected,
                          "executed": True, "returncode": 0})
    finished = utc_now()
    receipt = build_receipt(
        "EMULATED", started, finished, before_source, artifacts,
        {key: parsed[key] for key in ("gcc", "clang", "lld", "qemu")},
        parsed["runtime_line"],
        {"page_size": int(metadata["page_size"]),
         "cache_line_size": int(metadata["cache_line_size"]),
         "max_glibc": metadata["max_glibc"],
         "elf_count": int(metadata["elf_count"])},
        log_path.name, sha256_file(log_path))
    receipt_path = output / "m124-aarch64-icache-receipt-v1.json"
    write_exclusive(receipt_path, canonical_bytes(receipt), 0o600)
    metadata_path.unlink()
    validate_document(receipt, receipt_path, arguments.max_age_seconds)
    emit_terminal(receipt, receipt_path, "run")


def command_compile_only(arguments):
    test_environment = os.environ.copy()
    test_environment["PYTHONDONTWRITEBYTECODE"] = "1"
    started = utc_now()
    before_source = source_manifest()
    test_returncode, test_transcript = stream_process(
        [sys.executable, "-B", str(LOADER_ROOT / "tests/test_m124_attestation.py")],
        LOADER_ROOT, test_environment, arguments.timeout_seconds)
    require(test_returncode == 0,
            "receipt validator self-test returned %d" % test_returncode)
    require(source_manifest() == before_source,
            "source changed while the validator self-test was running")
    output = arguments.output_dir.resolve()
    prepare_output(output)
    metadata_path = output / ".gate-metadata"
    write_exclusive(metadata_path, b"", 0o600)
    environment = os.environ.copy()
    environment["NXLOADER_M124_COMPILE_ONLY"] = "1"
    environment["NXLOADER_M124_METADATA_PATH"] = str(metadata_path)
    environment["NXLOADER_M124_ARTIFACT_DIR"] = str(output)
    returncode, gate_transcript = stream_process(
        ["bash", str(GATE_PATH)], LOADER_ROOT, environment,
        arguments.timeout_seconds)
    require(returncode == 0, "canonical compile-only gate returned %d" % returncode)
    require(source_manifest() == before_source,
            "source changed while the compile-only gate was running")
    parsed = parse_compiled_gate_output(gate_transcript)
    metadata = parse_metadata(metadata_path)
    require(metadata["evidence_class"] == "COMPILED_ONLY",
            "compile-only gate produced the wrong evidence class")
    require(metadata["source_manifest_sha256"] == before_source,
            "gate source manifest differs from runner")
    require(metadata["terminal_sha256"] == hashlib.sha256(
        parsed["terminal_line"].encode("ascii")).hexdigest(),
        "compiled-only terminal differs from gate metadata")
    transcript = test_transcript + gate_transcript
    log_path = output / "m124-attestation.log"
    write_exclusive(log_path, transcript.encode("utf-8"), 0o600)
    artifacts = []
    for compiler in ("gcc", "clang"):
        name = "nxloader-m124-%s-aarch64" % compiler
        artifact = output / name
        expected = metadata[compiler + "_binary_sha256"]
        require(regular_unlinked(artifact) and sha256_file(artifact) == expected,
                "exported %s artifact diverged" % compiler)
        artifacts.append({"name": name, "sha256": expected,
                          "executed": False, "returncode": None})
    finished = utc_now()
    receipt = build_receipt(
        "COMPILED_ONLY", started, finished, before_source, artifacts,
        {key: parsed[key] for key in ("gcc", "clang", "lld", "qemu")},
        parsed["terminal_line"],
        {"page_size": None, "cache_line_size": None,
         "max_glibc": metadata["max_glibc"],
         "elf_count": int(metadata["elf_count"])},
        log_path.name, sha256_file(log_path))
    receipt_path = output / "m124-aarch64-icache-receipt-v1.json"
    write_exclusive(receipt_path, canonical_bytes(receipt), 0o600)
    metadata_path.unlink()
    validate_document(receipt, receipt_path, arguments.max_age_seconds)
    emit_terminal(receipt, receipt_path, "compile-only")


def command_validate(arguments):
    require(regular_unlinked(arguments.receipt),
            "receipt is missing or linked")
    receipt_path = arguments.receipt.resolve()
    document = load_json_unique(receipt_path)
    validate_document(document, receipt_path, arguments.max_age_seconds)
    emit_terminal(document, receipt_path, "validate")


def parser():
    root = argparse.ArgumentParser(description=__doc__)
    subparsers = root.add_subparsers(dest="command", required=True)
    run = subparsers.add_parser("run", help="run canonical QEMU owner gate")
    run.add_argument("--output-dir", type=Path, required=True)
    run.add_argument("--timeout-seconds", type=int, default=900)
    run.add_argument("--qemu-timeout-seconds", type=int, default=20)
    run.add_argument("--max-age-seconds", type=int, default=86400)
    run.set_defaults(handler=command_run)
    compile_only = subparsers.add_parser(
        "compile-only", help="build both artifacts without target execution")
    compile_only.add_argument("--output-dir", type=Path, required=True)
    compile_only.add_argument("--timeout-seconds", type=int, default=900)
    compile_only.add_argument("--max-age-seconds", type=int, default=86400)
    compile_only.set_defaults(handler=command_compile_only)
    validate = subparsers.add_parser("validate", help="fail-closed receipt check")
    validate.add_argument("--receipt", type=Path, required=True)
    validate.add_argument("--max-age-seconds", type=int, default=86400)
    validate.set_defaults(handler=command_validate)
    return root


def main(argv=None):
    arguments = parser().parse_args(argv)
    if hasattr(arguments, "timeout_seconds"):
        require(1 <= arguments.timeout_seconds <= 3600,
                "timeout_seconds is outside its safe range")
    if hasattr(arguments, "qemu_timeout_seconds"):
        require(1 <= arguments.qemu_timeout_seconds <= 120,
                "qemu_timeout_seconds is outside its safe range")
    if hasattr(arguments, "max_age_seconds"):
        require(1 <= arguments.max_age_seconds <= 86400,
                "max_age_seconds is outside its safe range")
    arguments.handler(arguments)


if __name__ == "__main__":
    try:
        main()
    except (AttestationError, OSError, subprocess.SubprocessError) as error:
        reason = re.sub(r"[^A-Za-z0-9_.:-]+", "_", str(error))[:160]
        print("%s status=FAIL reason=%s" % (TERMINAL_PREFIX, reason),
              file=sys.stderr)
        sys.exit(1)
