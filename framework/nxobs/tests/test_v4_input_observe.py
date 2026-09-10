#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C2 gate: the support bundle preserves the sanitized
NXINPUT-* observability receipts as OBSERVED events without promoting any
state, and never lets a path, IP or free-form name ride along."""

import importlib.util
import json
import pathlib
import sys
import tempfile

COMPONENT = pathlib.Path(__file__).resolve().parent.parent


def require(value, message):
    if not value:
        print("nxobs V4 input-observe gate FAILED: %s" % message)
        sys.exit(1)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HEADER = "schema=nx-input-observe/1 run=r1 gen=g1 consumer=godot3 "
LINES = [
    "NXINPUT-LOAD: " + HEADER + "seq=1 source=portmaster-env entries=1 "
    "map_sha256=aabb guid_requested=- "
    "guid_selected=190000004b4800000011000000010000 priority=0 "
    "result=selected",
    "NXINPUT-CAPABILITIES: " + HEADER + "seq=2 pad=0 buttons=17 axes=4 "
    "hats=0 ordinal_max=708 low_keys=3 gamepad_range=304-318 analog_axes=4 "
    "digest=1a2b3c4d",
    "NXINPUT-BINDING: " + HEADER + "seq=3 pad=0 control=L2 physical=b6 "
    "kind=button ordinal=6 semantic=legacy-unmanaged "
    "sink=sdl-gamecontroller reachable=1 trigger_kind=button",
    "NXINPUT-CHORD: " + HEADER + "seq=4 pair=L2+R2 exit=denied",
    "NXINPUT-EVENT: " + HEADER + "seq=5 pad=0 control=A phase=press "
    "physical=b1 context=gameplay sink=hermetic first=1",
    "NXINPUT-CONSUMER: " + HEADER + "seq=6 control=A action=jump "
    "state=press context=gameplay delivery=pending/not-instrumented",
]


def main():
    support = load("nxobs_support_v4input", COMPONENT / "nx-support-bundle.py")

    events = []
    support.parse_runtime(LINES, events, "r1")
    require(len(events) == 6, "the six receipt kinds were not all kept")
    phases = [item["phase"] for item in events]
    require(phases == ["observe-load", "observe-capabilities",
                       "observe-binding", "observe-chord", "observe-event",
                       "observe-consumer"],
            "the receipt kinds lost their phases")
    require(all(item["source"] == "input" and item["status"] == "observed"
                for item in events),
            "ingestion must stay OBSERVED and never promote any state")
    load_details = events[0]["details"]
    require(load_details.get("source") == "portmaster-env" and
            load_details.get("guid_selected") ==
            "190000004b4800000011000000010000" and
            load_details.get("result") == "selected",
            "NXINPUT-LOAD lost its provenance fields")
    binding = events[2]["details"]
    require(binding.get("control") == "L2" and
            binding.get("trigger_kind") == "button" and
            binding.get("semantic") == "legacy-unmanaged",
            "NXINPUT-BINDING lost the trigger kind or semantic state")
    require(events[3]["details"].get("pair") == "L2+R2" and
            events[3]["details"].get("exit") == "denied",
            "the chord negative attestation was lost")
    require(events[5]["details"].get("delivery") ==
            "pending/not-instrumented",
            "a pending consumer must stay pending in the bundle")

    # A hostile line with an unknown key never smuggles it through; the
    # allowlist drops it while the known fields survive.
    events = []
    support.parse_runtime(
        ["NXINPUT-LOAD: schema=nx-input-observe/1 run=r1 gen=g1 "
         "consumer=x seq=7 source=cfw-file entries=1 map_sha256=cc "
         "guid_requested=- guid_selected=g priority=0 result=selected "
         "hostname=leaky.example ip=10.0.0.9 path=_roms_ports"],
        events, "r1")
    require(len(events) == 1 and
            "hostname" not in events[0]["details"] and
            "ip" not in events[0]["details"] and
            "path" not in events[0]["details"] and
            events[0]["details"].get("source") == "cfw-file",
            "reserved/unknown keys must be dropped by the allowlist")

    # End to end through build_bundle: events survive, sanitized.
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        runtime = root / "runtime.log"
        extractor = root / "extractor.log"
        runtime.write_text(
            "run_start_utc=2026-08-29T00:00:00Z\n" + "\n".join(LINES) +
            "\ngame exited with status 0\n", encoding="ascii")
        extractor.write_text(
            "[2026-08-29 00:00:00] === NXExtract 1.3.0 ===\n",
            encoding="ascii")
        bundle = root / "support-bundle"
        support.build_bundle(
            runtime, extractor, bundle, "synthetic-stack", "synthetic-fw",
            run_id="v4-input", created_utc="2026-08-29T00:00:00Z")
        published = [
            json.loads(line) for line in
            (bundle / "events.jsonl").read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        kept = [item for item in published if item.get("source") == "input"]
        require(len(kept) == 6,
                "build_bundle dropped the input observability events")
        require(all(item["status"] == "observed" for item in kept),
                "the published bundle promoted an input state")
        public = b"".join(path.read_bytes()
                          for path in sorted(bundle.iterdir()))
        require(str(root).encode("utf-8") not in public,
                "the published bundle leaked its private source path")

    print("nxobs V4 input-observe gate passed: kinds=6 observed_only=1 "
          "allowlist=1 bundle_e2e=1")


if __name__ == "__main__":
    main()
