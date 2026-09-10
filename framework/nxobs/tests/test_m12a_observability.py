#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Hermetic M12A host gate: no device, network, session or guest code."""

from __future__ import print_function

import argparse
import errno
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile


ROOT = Path(__file__).resolve().parents[3]
COMPONENT = ROOT / "framework" / "nxobs"
LEDGER = COMPONENT / "m12a-observability-v1.json"
EXAMPLES = COMPONENT / "references" / "m12a-real-examples-v1.json"
SCHEMA = COMPONENT / "schema-v1.json"
TOOL_PATH = COMPONENT / "nx-support-bundle.py"
SHA = "0123456789abcdef"
FAKE_CREDENTIALS = {
    "authorization": "nxobs-fixture-authorization-value",
    "api_key": "nxobs-fixture-api-key-value",
    "x_authentication": "nxobs-fixture-x-auth-value",
    "cookie": "nxobs-fixture-cookie-value",
    "set_cookie": "nxobs-fixture-set-cookie-value",
    "token": "nxobs-fixture-token-value",
    "key": "nxobs-fixture-key-value",
    "session": "nxobs-fixture-session-value",
    "auth": "nxobs-fixture-auth-value",
    "header": "nxobs-fixture-header-value",
    "json": "nxobs-fixture-json-value",
    "query": "nxobs-fixture-query-value",
    "bearer": "nxobs-fixture-bearer-value",
    "space_header": "nxobs-fixture-space-header-value",
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def strict_json(path):
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key: %s" % key)
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=no_duplicates)


def load_tool():
    spec = importlib.util.spec_from_file_location("nx_support_bundle", TOOL_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest_valid(directory):
    for line in (directory / "MANIFEST.sha256").read_text(
            encoding="ascii").splitlines():
        value, name = line.split("  ", 1)
        require(digest(directory / name) == value,
                "bundle manifest mismatch: %s" % name)


def fixture_logs(root, explicit_failures=False):
    runtime = root / "runtime.log"
    extractor = root / "nxextract.log"
    events = []
    phases = (
        ("bootstrap", "run"), ("extractor", "validation"),
        ("loader", "relocations"), ("graphics", "context"),
        ("audio", "output"), ("input", "controller"),
        ("lifecycle", "loop"), ("diagnostic", "crash"),
    )
    for index, (source, phase) in enumerate(phases):
        details = {"attempt": index + 1, "label": "bounded"}
        if index == 0:
            details.update({
                "Authorization": FAKE_CREDENTIALS["authorization"],
                "Api-Key": FAKE_CREDENTIALS["api_key"],
                "X-Authentication": FAKE_CREDENTIALS["x_authentication"],
                "Cookie": FAKE_CREDENTIALS["cookie"],
                "Set-Cookie": FAKE_CREDENTIALS["set_cookie"],
                "access_token": FAKE_CREDENTIALS["token"],
                "key": FAKE_CREDENTIALS["key"],
                "session": FAKE_CREDENTIALS["session"],
                "auth": FAKE_CREDENTIALS["auth"],
                "header_blob": "Authorization: Bearer %s" %
                               FAKE_CREDENTIALS["header"],
                "json_blob": '{"api_key":"%s"}' %
                             FAKE_CREDENTIALS["json"],
                "query_blob": "https://support.invalid/probe?token=%s&mode=test" %
                              FAKE_CREDENTIALS["query"],
                "bearer_blob": "Bearer %s" % FAKE_CREDENTIALS["bearer"],
                "space_blob": "Authorization %s" %
                              FAKE_CREDENTIALS["space_header"],
            })
        item = {
            "schema": "nx-event-v1", "source": source, "phase": phase,
            "status": "failed" if explicit_failures else "ok",
            "reason_code": 2000 + index, "monotonic_ns": 1000 + index * 100,
            "duration_ns": 50, "source_time": "2026-08-09T00:00:00Z",
            "details": details,
        }
        events.append("NXEVENT " + json.dumps(item, sort_keys=True))
    report = {
        "nxcompat_version": "0.2.0", "api_version": 2,
        "sanitized": True, "report_reason_code": 550, "phase": "input",
        "host": {"device_model": "private-device", "os_id": "test",
                 "process_arch": "aarch64", "kernel_arch": "aarch64",
                 "libc": "glibc 2.30", "memory_class": "short",
                 "filesystem_class": "fuse-like"},
        "capabilities": [
            {"id": "graphics.window", "state": "opened", "reason_code": 520},
            {"id": "input.controller-api", "state": "active", "reason_code": 540},
        ],
        "receipts": {
            "graphics": {"source": "nxgl", "generation": 1,
                         "proof_flags": 127, "window": [640, 480],
                         "drawable": [640, 480], "gles": [3, 2],
                         "backend": "KMSDRM", "gl_renderer": "Mali-G31"},
            "audio": {"source": "engine-adapter", "generation": 1,
                      "frequency": 44100, "channels": 2, "samples": 4096,
                      "backend": "alsa"},
            "input": {"source": "nxinput", "generation": 2,
                      "connected_count": 1, "proof_flags": 47},
        },
    }
    events.extend([
        "NXCOMPAT_REPORT " + json.dumps(report, sort_keys=True),
        "NXLOADER[2] loaded libguest.so",
        "NXLOADER prepared libguest.so imports=1",
        "constructors -> JNI_OnLoad...",
        "NXLOADER game READY JNI=0x10004",
        "VIDEO backend=KMSDRM window=640x480 drawable=640x480",
        "AUDIO: output opened",
        "Entering main loop...",
        "private error path=/home/alice/save ip=192.168.1.2 token=secret",
        "failed Authorization: Bearer %s" % FAKE_CREDENTIALS["header"],
        "Api-Key: %s" % FAKE_CREDENTIALS["api_key"],
        "X-Authentication: %s" % FAKE_CREDENTIALS["x_authentication"],
        "Cookie: nxid=%s" % FAKE_CREDENTIALS["cookie"],
        "Set-Cookie: nxsession=%s" % FAKE_CREDENTIALS["set_cookie"],
        "GET /probe?token=%s&key=%s&session=%s&auth=%s" % (
            FAKE_CREDENTIALS["token"], FAKE_CREDENTIALS["key"],
            FAKE_CREDENTIALS["session"], FAKE_CREDENTIALS["auth"]),
        '{"Authorization":"Bearer %s","api_key":"%s"}' % (
            FAKE_CREDENTIALS["authorization"], FAKE_CREDENTIALS["json"]),
        "[nxbootstrap] game exited with status 0",
    ])
    runtime.write_text("\n".join(events) + "\n", encoding="utf-8")
    extractor.write_text(
        "[2026-08-09 00:00:00] === NXExtract 1.2.6 ===\n"
        "[2026-08-09 00:00:01] fast validation marker accepted; no source scan needed\n",
        encoding="utf-8")
    return runtime, extractor


def functional_gate():
    tool = load_tool()
    root = Path(tempfile.mkdtemp(prefix="nxobs-m12a."))
    try:
        runtime, extractor = fixture_logs(root)
        artifact = root / "artifact.zip"
        artifact.write_bytes(b"not-a-real-zip-but-fingerprinted")
        first = root / "bundle-a"
        report = tool.build_bundle(
            runtime, extractor, first, "synthetic-stack", "synthetic-fw",
            artifact=artifact, run_id="support-test-fixed",
            created_utc="2026-08-09T00:00:00Z", payload_abi="aarch64-bionic",
            rom_root_kind="portmaster")
        require(report["event_count"] >= 20 and
                report["timed_event_count"] == 8 and
                report["payload_abi"] == "aarch64-bionic" and
                report["rom_root_kind"] == "portmaster" and
                report["privacy"]["raw_logs_included"] is False and
                report["privacy"]["credential_redaction"] == "fail-closed",
                "support report lost its bounded contract")
        require(set(path.name for path in first.iterdir()) == {
            "MANIFEST.sha256", "SUMMARY.txt", "events.jsonl", "report.json",
            "schema.json"}, "bundle published an unexpected file")
        public_schema = strict_json(first / "schema.json")
        require(
            public_schema["credential_redaction"]["mode"] == "fail-closed" and
            public_schema["credential_redaction"]["raw_value_retained"] is False and
            public_schema["credential_redaction"]["text_markers"] == [
                "redacted-authorization", "redacted-cookie",
                "redacted-credential", "redacted-query"],
            "public schema lost the credential redaction contract")
        manifest_valid(first)
        public = b"".join(path.read_bytes() for path in first.iterdir())
        for forbidden in (b"192.168", b"/home/alice", b"token=secret",
                          b"private-device"):
            require(forbidden not in public, "bundle leaked adversarial input")
        for forbidden in FAKE_CREDENTIALS.values():
            require(forbidden.encode("ascii") not in public,
                    "bundle leaked a synthetic credential")
        event_rows = [json.loads(line) for line in
                      (first / "events.jsonl").read_text(
                          encoding="utf-8").splitlines()]
        require([item["sequence"] for item in event_rows] ==
                list(range(1, len(event_rows) + 1)) and
                {item["run_id"] for item in event_rows} ==
                {"support-test-fixed"}, "events are mixed or out of order")
        privacy_event = next(
            item for item in event_rows if item["reason_code"] == 2000)
        privacy_details = privacy_event["details"]
        require(
            privacy_details.get("redacted_authorization") is True and
            privacy_details.get("redacted_cookie") is True and
            privacy_details.get("redacted_credential") is True and
            privacy_details.get("header_blob") == "redacted-authorization" and
            privacy_details.get("json_blob") == "redacted-credential" and
            privacy_details.get("query_blob") == "redacted-query" and
            privacy_details.get("bearer_blob") == "redacted-authorization" and
            privacy_details.get("space_blob") == "redacted-authorization" and
            privacy_details.get("attempt") == 1 and
            privacy_details.get("label") == "bounded",
            "credential redaction lost its finite diagnostic markers")

        second = root / "bundle-b"
        tool.build_bundle(
            runtime, extractor, second, "synthetic-stack", "synthetic-fw",
            artifact=artifact, run_id="support-test-fixed",
            created_utc="2026-08-09T00:00:00Z", payload_abi="aarch64-bionic",
            rom_root_kind="portmaster")
        require(digest(first / "MANIFEST.sha256") ==
                digest(second / "MANIFEST.sha256"),
                "fixed inputs do not produce a deterministic bundle")
        try:
            tool.build_bundle(runtime, extractor, first, "synthetic-stack",
                              "synthetic-fw")
            raise AssertionError("existing output was overwritten")
        except tool.BundleError:
            pass

        linked = root / "runtime-link"
        linked.symlink_to(runtime)
        try:
            tool.build_bundle(linked, extractor, root / "linked-output",
                              "synthetic-stack", "synthetic-fw")
            raise AssertionError("symlink input was followed")
        except tool.BundleError:
            pass
        oversized = root / "oversized.log"
        oversized.write_bytes(b"x" * (tool.MAX_LINE_BYTES + 1) + b"\n")
        try:
            tool.build_bundle(oversized, extractor, root / "oversized-output",
                              "synthetic-stack", "synthetic-fw")
            raise AssertionError("oversized line was accepted")
        except tool.BundleError:
            pass

        failures_root = root / "failures"
        failures_root.mkdir()
        failed_runtime, failed_extractor = fixture_logs(
            failures_root, explicit_failures=True)
        failed_output = root / "failure-bundle"
        tool.build_bundle(failed_runtime, failed_extractor, failed_output,
                          "synthetic-stack", "synthetic-fw",
                          run_id="support-failures",
                          created_utc="2026-08-09T00:00:00Z")
        failures = [json.loads(line) for line in
                    (failed_output / "events.jsonl").read_text(
                        encoding="utf-8").splitlines()]
        require(len([item for item in failures if item["status"] == "failed" and
                     2000 <= item["reason_code"] <= 2007]) == 8,
                "failure injection did not cover every event source")

        writes = [0]
        def enospc_writer(path, value):
            writes[0] += 1
            if writes[0] == 2:
                raise OSError(errno.ENOSPC, "injected full filesystem")
            tool.write_file(path, value)
        full_output = root / "full-output"
        try:
            tool.build_bundle(runtime, extractor, full_output,
                              "synthetic-stack", "synthetic-fw",
                              writer=enospc_writer)
            raise AssertionError("injected full filesystem succeeded")
        except OSError as error:
            require(error.errno == errno.ENOSPC and not full_output.exists(),
                    "ENOSPC published a partial bundle")
        require(not list(root.glob(".full-output.tmp.*")),
                "ENOSPC left a temporary bundle")

        parent_file = root / "not-a-directory"
        parent_file.write_text("x", encoding="ascii")
        try:
            tool.build_bundle(runtime, extractor,
                              parent_file / "unwritable-output",
                              "synthetic-stack", "synthetic-fw")
            raise AssertionError("invalid output parent succeeded")
        except (tool.BundleError, OSError):
            pass
        try:
            tool.build_bundle(runtime, extractor, root / "bad-id-output",
                              "../../bad", "synthetic-fw")
            raise AssertionError("invalid public ID succeeded")
        except tool.BundleError:
            pass
        try:
            tool.build_bundle(
                runtime, extractor, root / "credential-metadata-output",
                "synthetic-stack", "synthetic-fw",
                run_id="Bearer-nxobs-fixture-run")
            raise AssertionError("credential-like public metadata succeeded")
        except tool.BundleError:
            pass
    finally:
        shutil.rmtree(str(root))


def contract_gate(preflight):
    ledger = strict_json(LEDGER)
    schema = strict_json(SCHEMA)
    examples = strict_json(EXAMPLES)
    require(ledger["schema"] == "nxobs-m12a-observability-v1" and
            ledger["schema_version"] == 1 and ledger["milestone"] == "M12A" and
            ledger["status"] == "closed_implementation_and_two_real_examples",
            "M12A ledger identity changed")
    require(schema["maximum_input_bytes"] == 8388608 and
            schema["maximum_line_bytes"] == 65536 and
            schema["maximum_events"] == 2048 and
            schema["privacy"] == {
                "raw_logs_in_bundle": False, "absolute_paths": False,
                "network_addresses": False, "hostnames": False,
                "credentials": False, "save_data": False},
            "M12A schema/privacy boundary changed")
    require(schema["credential_redaction"] == {
                "mode": "fail-closed",
                "structured_classes": [
                    "authorization", "api-key", "x-authentication",
                    "cookie", "set-cookie", "token", "key", "session",
                    "auth"],
                "embedded_syntax": ["bearer", "header", "json", "query"],
                "text_markers": [
                    "redacted-authorization", "redacted-cookie",
                    "redacted-credential", "redacted-query"],
                "raw_value_retained": False},
            "credential redaction contract changed")
    items = ledger["items"]
    require(len(items) == 40 and
            [item["id"] for item in items] ==
            ["M12A-%03d" % number for number in range(1, 41)] and
            all(item["status"] == "closed" and item["basis"] for item in items),
            "M12A must account for exactly 40 closed items")
    require(ledger["verification"]["real_stack_examples"] == 2 and
            ledger["verification"]["device_access"] is False and
            ledger["verification"]["network_access"] is False and
            ledger["privacy"]["raw_log_lines"] is False,
            "M12A verification boundary changed")
    require(examples["artifact_sha256"] ==
            "886cefaef1a2be2ff276c4d276d430e35ce2162bd47c4af5d29a62863638b27a" and
            examples["device_contacted_by_m12a"] is False and
            examples["physical_evidence_inherited_from_m22"] is True and
            examples["raw_logs_published"] is False and
            [item["stack_id"] for item in examples["examples"]] ==
            ["nextos-mali450-fbdev", "arkos-rk3326-kmsdrm"],
            "M12A real example scope changed")
    for item in examples["examples"]:
        require(item["event_count"] in (44, 45) and
                item["timed_event_count"] == 0 and
                item["last_runtime_milestone"] == "lifecycle/shutdown/ok" and
                len(item["categories"]) == 7,
                "M12A real example facts changed")
        require(len(item["bundle_manifest_sha256"]) == 64 and
                all(char in SHA for char in item["bundle_manifest_sha256"]),
                "M12A real bundle manifest is missing")
    checkpoint = ledger["checkpoint"]
    if preflight:
        require(checkpoint == {"id": "PENDING", "manifest_sha256": "PENDING"},
                "M12A preflight checkpoint changed")
    else:
        require(checkpoint["id"] != "PENDING" and
                len(checkpoint["manifest_sha256"]) == 64 and
                all(char in SHA for char in checkpoint["manifest_sha256"]),
                "M12A checkpoint is missing")
    bootstrap = (ROOT / "ports/chrono/nxbootstrap.sh").read_text(encoding="utf-8")
    for token in ("nxbootstrap_open_fresh_log_fd", "link_count",
                  "run_start_utc=", "game exited with status"):
        require(token in bootstrap, "runtime log boundary lacks: %s" % token)
    m23 = strict_json(ROOT / "ports/chrono/references/m23-promotion-v1.json")
    require(len(m23["comparison"]["common_regressions_added"]) == 3,
            "M12A capability/reason/regression feedback evidence changed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preflight", action="store_true")
    args = parser.parse_args()
    functional_gate()
    contract_gate(args.preflight)
    print("nxobs_m12a_host={} items=40 synthetic_failures=8 real_examples=2 "
          "raw_logs_public=0 device_access=0 network_access=0 session_access=0 "
          "guest_code_executed=0".format(
              "PREFLIGHT" if args.preflight else "PASS"))


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, KeyError, TypeError,
            json.JSONDecodeError) as error:
        print("nxobs M12A gate failed: %s" % error)
        raise SystemExit(1)
