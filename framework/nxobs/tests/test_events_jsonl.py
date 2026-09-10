#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V3-OBS-01 gate: native events.jsonl input for the support bundle.

Proves that one run_id crosses launcher/extractor events into the bundle,
that hostile lines are sanitized or fail closed, and that a torn final line
(interrupted writer) is tolerated as a diagnostic instead of losing the run.
"""

import importlib.util
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
FAKE_VALUES = {
    "authorization": "nxobs-events-fixture-authorization",
    "api_key": "nxobs-events-fixture-api-key",
    "x_authentication": "nxobs-events-fixture-x-auth",
    "cookie": "nxobs-events-fixture-cookie",
    "set_cookie": "nxobs-events-fixture-set-cookie",
    "bearer": "nxobs-events-fixture-bearer",
    "query": "nxobs-events-fixture-query",
    "json": "nxobs-events-fixture-json",
    "cookie_space": "nxobs-events-fixture-cookie-space",
}


def load_tool():
    spec = importlib.util.spec_from_file_location(
        "nx_support_bundle", ROOT / "nx-support-bundle.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(condition, message):
    if not condition:
        raise SystemExit("events.jsonl gate failed: %s" % message)


def event_line(sequence, source="extractor", phase="install",
               status="ok", reason=1400, details=None):
    return json.dumps({
        "schema": "nx-event-v1",
        "schema_version": 1,
        "run_id": "porttest-1700000000-77-123",
        "sequence": sequence,
        "source": source,
        "phase": phase,
        "status": status,
        "reason_code": reason,
        "monotonic_ns": 1000 + sequence,
        "details": details or {},
    })


def main():
    tool = load_tool()
    with tempfile.TemporaryDirectory(prefix="nxobs-events-") as raw:
        work = pathlib.Path(raw)
        runtime = work / "log.txt"
        runtime.write_text(
            "run_start_utc=2026-08-26T00:00:00Z\n"
            "game exited with status 0\n", encoding="utf-8"
        )
        extractor = work / "nxextract.log"
        extractor.write_text("", encoding="utf-8")

        events = work / "events.jsonl"
        events.write_text(
            event_line(1, "bootstrap", "run", "begin", 1000) + "\n" +
            event_line(2, "extractor", "install", "begin", 1400,
                       {"recipe": "synthetic",
                        "host": "ignored",
                        "Authorization": FAKE_VALUES["authorization"],
                        "Api-Key": FAKE_VALUES["api_key"],
                        "X-Authentication": FAKE_VALUES["x_authentication"],
                        "Cookie": FAKE_VALUES["cookie"],
                        "Set-Cookie": FAKE_VALUES["set_cookie"],
                        "bearer_blob": "Bearer %s" % FAKE_VALUES["bearer"],
                        "query_blob":
                            "https://support.invalid/probe?session=%s" %
                            FAKE_VALUES["query"],
                        "json_blob": '{"auth":"%s"}' %
                                     FAKE_VALUES["json"],
                        "cookie_blob": "Cookie %s" %
                                       FAKE_VALUES["cookie_space"]}) + "\n" +
            event_line(3, "extractor", "install", "ok", 1401,
                       {"note": "/home/private/path is rejected"}) + "\n" +
            '{"schema": "nx-event-v1", "torn', encoding="utf-8"
        )

        output = work / "bundle.json"
        report = tool.build_bundle(
            str(runtime), str(extractor), str(output),
            "synthetic-stack", "synthetic-firmware",
            run_id="porttest-1700000000-77-123",
            events_file=str(events),
        )
        require(report["run_id"] == "porttest-1700000000-77-123",
                "run_id did not survive")
        items = [
            json.loads(line)
            for line in (output / "events.jsonl").read_text(
                encoding="utf-8"
            ).splitlines()
            if line.strip()
        ]
        require(all(item["run_id"] == "porttest-1700000000-77-123"
                    for item in items),
                "one run_id must cross every event")
        sources = [item["source"] for item in items]
        require("bootstrap" in sources and "extractor" in sources and
                "lifecycle" in sources,
                "launcher, extractor and runtime events must coexist")
        torn = [item for item in items
                if item["phase"] == "events-file" and
                item["details"].get("note") == "torn-final-line"]
        require(len(torn) == 1, "torn final line must become a diagnostic")
        text = "".join(
            child.read_text(encoding="utf-8")
            for child in sorted(output.iterdir())
        )
        require("/home/private" not in text,
                "absolute private path leaked into the bundle")
        require("ignored" not in json.dumps(
            [item for item in items if item["sequence"] == 2]),
            "an events item was silently marked ignored")
        require("ignored" not in text, "host detail leaked into the bundle")
        for forbidden in FAKE_VALUES.values():
            require(forbidden not in text,
                    "synthetic credential leaked into the bundle")
        privacy_rows = [item for item in items
                        if item["reason_code"] == 1400 and
                        item["source"] == "extractor"]
        require(len(privacy_rows) == 1,
                "credential fixture event was not preserved")
        privacy = privacy_rows[0]["details"]
        require(
            privacy.get("recipe") == "synthetic" and
            privacy.get("redacted_authorization") is True and
            privacy.get("redacted_cookie") is True and
            privacy.get("redacted_credential") is True and
            privacy.get("bearer_blob") == "redacted-authorization" and
            privacy.get("query_blob") == "redacted-query" and
            privacy.get("json_blob") == "redacted-authorization" and
            privacy.get("cookie_blob") == "redacted-cookie",
            "finite redaction markers lost useful event diagnostics")

        # A malformed line in the middle fails closed.
        bad = work / "bad.jsonl"
        bad.write_text(
            event_line(1) + "\n" + "not json at all\n" +
            event_line(2) + "\n", encoding="utf-8"
        )
        try:
            tool.build_bundle(
                str(runtime), str(extractor), str(work / "bad-bundle.json"),
                "synthetic-stack", "synthetic-firmware",
                run_id="porttest-1700000000-77-124",
                events_file=str(bad),
            )
        except tool.BundleError:
            pass
        else:
            raise SystemExit("malformed middle line must fail closed")

    print("nxobs events.jsonl gate passed: events=native torn_tail=1 "
          "middle_malformed=fail-closed run_id=crossing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
