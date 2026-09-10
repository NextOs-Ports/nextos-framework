#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-GRAPHICS-04 gate: the support bundle preserves the sanitized final
GRAPHICS-EVIDENCE receipt and never promotes a pre-present state.

Before nxobs 0.4.1 the shared bundle did not interpret GRAPHICS-EVIDENCE at
all, so the post-first-present proof survived only in the raw private log.
This gate proves the final receipt (verdict OK and FAIL), the pre-present
diagnostic (OBSERVED only, never ok) and the sanitization of provider DSO
paths, end to end through build_bundle."""

import importlib.util
import json
import pathlib
import sys
import tempfile

COMPONENT = pathlib.Path(__file__).resolve().parent.parent


def require(value, message):
    if not value:
        print("nxobs V4 graphics-evidence gate FAILED: %s" % message)
        sys.exit(1)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FINAL_OK = (
    "GRAPHICS-EVIDENCE: run_id=porttest-1-2-3 generation=%s commit=abc123 "
    "cfw=fixturefw sdl=2 provider_egl=/usr/lib/private/libEGL.so "
    "provider_gles=/usr/lib/private/libGLESv2.so build_id=deadbeef12 "
    "requested=gles/es/3.0/minimum obtained=gles/es/3.1 drawable=640x480 "
    "shader_probe=pass verdict=OK reason=ok phase=post-first-present "
    "first_present=1 pre_drawable=1x1 port_id=fixtureport "
    "port_version=1.0.0 egl_build_id=beef01" % ("a" * 64)
)
FINAL_FAIL = (
    "GRAPHICS-EVIDENCE: run_id=porttest-1-2-4 generation=%s commit=abc123 "
    "cfw=fixturefw sdl=2 provider_egl=- provider_gles=- build_id=deadbeef12 "
    "requested=gles/es/3.0/minimum obtained=gles/es/3.1 drawable=1x1 "
    "shader_probe=pass verdict=FAIL reason=drawable-stuck-1x1 "
    "phase=post-first-present first_present=1 pre_drawable=1x1" % ("a" * 64)
)
PREPRESENT = ("GRAPHICS-PREPRESENT-EVIDENCE: state=awaiting-first-present "
              "final=0 drawable=1x1")


def main():
    support = load("nxobs_support_v4gfx", COMPONENT / "nx-support-bundle.py")

    # ---- unit: the runtime parser ingests all three shapes, sanitized ----
    events = []
    support.parse_runtime([PREPRESENT, FINAL_OK, FINAL_FAIL], events, "r1")
    require(len(events) == 3, "the three receipt lines were not all kept")
    pre, ok, failed = events
    require(pre["source"] == "graphics" and pre["phase"] == "pre-present" and
            pre["status"] == "observed" and pre["reason_code"] == 1602 and
            pre["details"].get("state") == "awaiting-first-present" and
            pre["details"].get("final") == "0",
            "the pre-present diagnostic was not preserved as observed")
    require(pre["status"] != "ok",
            "an awaiting-first-present state must never be represented as ok")
    require(ok["source"] == "graphics" and ok["phase"] == "evidence" and
            ok["status"] == "ok" and ok["reason_code"] == 1601,
            "the final OK receipt was not preserved")
    d = ok["details"]
    require(d.get("phase") == "post-first-present" and
            d.get("first_present") == "1" and
            d.get("drawable") == "640x480" and
            d.get("pre_drawable") == "1x1" and
            d.get("shader_probe") == "pass" and
            d.get("verdict") == "OK" and d.get("reason") == "ok" and
            d.get("build_id") == "deadbeef12" and
            d.get("egl_build_id") == "beef01" and
            d.get("commit") == "abc123" and
            d.get("port_id") == "fixtureport" and
            d.get("generation") == "a" * 64,
            "the final receipt lost sanitized identity fields")
    require("provider_egl" not in d and "provider_gles" not in d and
            all("/usr/lib" not in str(v) for v in d.values()),
            "a private provider DSO path leaked into the bundle event")
    require(failed["status"] == "failed" and
            failed["details"].get("reason") == "drawable-stuck-1x1",
            "the FAIL diagnostic receipt was not preserved as failed")

    # ---- end to end: build_bundle keeps the events and stays sanitized ----
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        runtime = root / "runtime.log"
        extractor = root / "extractor.log"
        runtime.write_text(
            "run_start_utc=2026-08-29T00:00:00Z\n%s\n%s\n"
            "game exited with status 0\n" % (PREPRESENT, FINAL_OK),
            encoding="ascii")
        extractor.write_text(
            "[2026-08-29 00:00:00] === NXExtract 1.3.0 ===\n",
            encoding="ascii")
        bundle = root / "support-bundle"
        support.build_bundle(
            runtime, extractor, bundle, "synthetic-stack", "synthetic-fw",
            run_id="v4-gfx", created_utc="2026-08-29T00:00:00Z")
        published = [
            json.loads(line) for line in
            (bundle / "events.jsonl").read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        kept = [item for item in published
                if item.get("source") == "graphics" and
                item.get("phase") in ("evidence", "pre-present")]
        require(len(kept) == 2,
                "build_bundle dropped the graphics evidence events")
        final = next(item for item in kept if item["phase"] == "evidence")
        require(final["status"] == "ok" and
                final["details"].get("phase") == "post-first-present" and
                final["details"].get("first_present") == "1",
                "the published bundle lost the post-first-present proof")
        public = b"".join(path.read_bytes()
                          for path in sorted(bundle.iterdir()))
        require(b"/usr/lib/private" not in public,
                "the published bundle leaked a provider DSO path")
        require(str(root).encode("utf-8") not in public,
                "the published bundle leaked its private source path")

    print("nxobs V4 graphics-evidence gate passed: final_ok=1 final_fail=1 "
          "pre_present_observed=1 provider_paths_sanitized=1 bundle_e2e=1")


if __name__ == "__main__":
    main()
