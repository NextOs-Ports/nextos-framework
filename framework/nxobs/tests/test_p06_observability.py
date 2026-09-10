#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Hermetic P06 crash/phase observability gate."""

from __future__ import print_function

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
COMPONENT = ROOT / "framework/nxobs"
CONTRACT = COMPONENT / "p06-crash-contract-v1.json"
CRASH_SCHEMA = COMPONENT / "crash-schema-v1.json"
CRASH_KEYS = {
    "schema", "schema_version", "signal", "status", "phase", "frame",
    "thread", "pc", "lr", "sp", "fault_address", "module", "build_id",
    "module_offset", "last_asset", "last_graphics_call", "provider", "maps",
}


def require(value, message):
    if not value:
        raise AssertionError(message)


def load(name, path):
    specification = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def strict_record(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate receipt key")
            result[key] = value
        return result

    lines = path.read_bytes().splitlines()
    require(len(lines) == 1, "receipt is not one atomic record")
    return json.loads(lines[0].decode("ascii"), object_pairs_hook=unique,
                      parse_constant=lambda value: (_ for _ in ()).throw(
                          AssertionError("non-JSON constant %s" % value)))


def compile_fixture(root):
    binary = root / "nxobsfixture"
    subprocess.check_call([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-g",
        "-I" + str(COMPONENT / "include"),
        str(COMPONENT / "src/nxobs_crash.c"),
        str(COMPONENT / "tests/crash-fixture.c"),
        "-ldl", "-o", str(binary),
    ])
    subprocess.check_call([
        "aarch64-linux-gnu-gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-O2", "-I" + str(COMPONENT / "include"), "-c",
        str(COMPONENT / "src/nxobs_crash.c"),
        "-o", str(root / "nxobs_crash-aarch64.o"),
    ])
    return binary


def contract_gate():
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    schema = json.loads(CRASH_SCHEMA.read_text(encoding="utf-8"))
    require(contract["schema"] == "nxobs-p06-crash-contract-v1" and
            contract["component"] == {"name": "nxobs", "version": "0.3.0"} and
            contract["status"] == "closed_component_scope" and
            [item["id"] for item in contract["items"]] ==
            ["OBS-%03d" % value for value in range(41, 55)] and
            all(item["status"] == "closed" and item["basis"]
                for item in contract["items"]),
            "P06 nxobs contract no longer closes OBS-041..054 exactly")
    require(set(schema["required"]) == CRASH_KEYS and
            schema["properties"]["schema"]["const"] == "nx-crash-v1" and
            schema["additionalProperties"] is False,
            "nx-crash-v1 schema boundary changed")
    source = (COMPONENT / "src/nxobs_crash.c").read_text(encoding="utf-8")
    handler = source.split("static void nxobs_crash_handler", 1)[1].split(
        "static void nxobs_restore_handlers", 1
    )[0]
    for forbidden in ("malloc(", "calloc(", "realloc(", "free(",
                      "printf(", "snprintf(", "backtrace(", "dladdr("):
        require(forbidden not in handler,
                "signal handler regained unsafe call %s" % forbidden)
    for required in ("SA_RESETHAND", "kill(getpid(), signal_number)",
                     "fsync(g_receipt_fd)", "module_offset", "build_id"):
        require(required in source, "native crash source lacks %s" % required)
    documentation = (COMPONENT / "README.md").read_text(encoding="utf-8")
    for token in ("never the DWARF source", "public_zip_member=false",
                  "re-raises the original signal", "changes no visual interface"):
        require(token in documentation,
                "nxobs documentation lacks %s" % token)


def run_crash(binary, root, mode, expected_signal):
    directory = root / mode
    directory.mkdir(mode=0o700)
    process = subprocess.run(
        [str(binary), str(directory), mode],
        env=dict(os.environ, NXOBS_FIXTURE_BAD_ADDRESS="1"),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        universal_newlines=True, timeout=15,
    )
    require(process.returncode == -expected_signal,
            "%s signal was swallowed/relabelled: %s %s" %
            (mode, process.returncode, process.stderr))
    receipts = list(directory.glob("nxobsfixture-crash.*.jsonl"))
    maps = list(directory.glob("nxobsfixture-maps.*.txt"))
    require(len(receipts) == 1 and len(maps) == 1,
            "%s did not leave one crash/maps pair" % mode)
    for path in receipts + maps:
        info = path.lstat()
        require(stat.S_IMODE(info.st_mode) == 0o600 and
                stat.S_ISREG(info.st_mode) and not path.is_symlink(),
                "crash evidence permissions are not private")
    receipt = strict_record(receipts[0])
    require(set(receipt) == CRASH_KEYS and
            receipt["schema"] == "nx-crash-v1" and
            receipt["schema_version"] == 1 and
            receipt["signal"] == expected_signal and
            receipt["status"] == 128 + expected_signal and
            receipt["thread"] > 0,
            "%s receipt identity is invalid" % mode)
    require(receipt["maps"] == maps[0].name and
            "/" not in maps[0].read_text(encoding="ascii") and
            str(root).encode("utf-8") not in receipts[0].read_bytes(),
            "%s leaked an unsanitized maps/path value" % mode)
    for field in ("pc", "lr", "sp", "fault_address", "module_offset"):
        require(receipt[field].startswith("0x"),
                "%s lacks %s" % (mode, field))
    return receipts[0], receipt


def gate():
    contract_gate()
    root = Path(tempfile.mkdtemp(prefix="nxobs-p06."))
    try:
        binary = compile_fixture(root)
        clean = root / "clean"
        clean.mkdir(mode=0o700)
        subprocess.check_call([str(binary), str(clean), "clean"])
        require(not list(clean.iterdir()),
                "normal uninstall retained crash-only evidence")

        public_dir = root / "public"
        public_dir.mkdir(mode=0o755)
        public_dir.chmod(0o755)
        invalid = subprocess.run(
            [str(binary), str(public_dir), "clean"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        require(invalid.returncode == 65 and not list(public_dir.iterdir()),
                "non-private runtime directory was accepted")
        linked = root / "linked"
        linked.symlink_to(clean, target_is_directory=True)
        invalid = subprocess.run(
            [str(binary), str(linked), "clean"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        require(invalid.returncode == 65,
                "symlink runtime directory was accepted")

        early_path, early = run_crash(binary, root, "early", signal.SIGSEGV)
        late_path, late = run_crash(binary, root, "late", signal.SIGSEGV)
        abort_path, aborted = run_crash(binary, root, "abort", signal.SIGABRT)
        require(early["phase"] == "runtime-start" and early["frame"] == 0,
                "early crash lost phase/frame")
        require(late["phase"] == "draw" and late["frame"] == 42 and
                late["last_asset"] == "ferryman-atlas" and
                late["last_graphics_call"] == "glDrawElements" and
                late["provider"] == "wayland-gles2" and
                late["module"] == binary.name and
                late["build_id"] != "unavailable",
                "late crash lost structured draw context")
        require(aborted["phase"] == "abort-boundary",
                "SIGABRT crash lost phase")

        symbolizer = load("nxobs_symbolizer",
                          COMPONENT / "nx-symbolize-crash.py")
        symbolized_path = root / "symbolized.json"
        symbolized = symbolizer.symbolize(late_path, binary, symbolized_path)
        require(symbolized["paths_sanitized"] is True and
                symbolized["module"] == binary.name and
                symbolized["function"] != "unknown" and
                "/" not in symbolized["source_basename"] and
                stat.S_IMODE(symbolized_path.stat().st_mode) == 0o600,
                "offline symbolization is incomplete or leaked a path")
        wrong_binary = root / "wrong-binary"
        wrong_binary.write_bytes(binary.read_bytes() + b"different")
        wrong_binary.chmod(0o700)
        try:
            symbolizer.symbolize(late_path, wrong_binary,
                                  root / "wrong-symbolization.json")
            raise AssertionError("wrong symbol binary unexpectedly passed")
        except symbolizer.SymbolizeError:
            pass

        private_root = root / "private-symbols"
        private_root.mkdir(mode=0o700)
        archiver = load("nxobs_archiver", COMPONENT / "nx-archive-symbols.py")
        archive_path = private_root / "build-test"
        archive = archiver.archive_symbols(
            private_root, archive_path, [binary], "ad1cf2753244b"
        )
        require(archive["public_zip_member"] is False and
                len(archive["symbols"]) == 1 and
                stat.S_IMODE(archive_path.stat().st_mode) == 0o700,
                "private symbol archive contract failed")
        archived_binary = archive_path / archive["symbols"][0]["path"]
        require(hashlib.sha256(archived_binary.read_bytes()).hexdigest() ==
                archive["symbols"][0]["sha256"] and
                stat.S_IMODE(archived_binary.stat().st_mode) == 0o600,
                "private symbol archive bytes/mode changed")

        support = load("nxobs_support", COMPONENT / "nx-support-bundle.py")
        runtime = root / "runtime.log"
        extractor = root / "extractor.log"
        runtime.write_text(
            "NXEVENT {\"schema\":\"nx-event-v1\",\"source\":"
            "\"lifecycle\",\"phase\":\"loop\",\"status\":\"ok\","
            "\"reason_code\":1800}\n", encoding="ascii"
        )
        extractor.write_text(
            "[2026-08-16 00:00:00] === NXExtract 1.2.9 ===\n",
            encoding="ascii",
        )
        bundle = root / "support-bundle"
        report = support.build_bundle(
            runtime, extractor, bundle, "synthetic-stack", "synthetic-fw",
            run_id="p06-crash", created_utc="2026-08-16T00:00:00Z",
            crash_receipts=[late_path],
        )
        require(report["last_completed"]["status"] == "failed" and
                report["last_completed"]["phase"] == "draw" and
                report["sources"]["crash_receipts"][0]["bytes"] > 0 and
                report["components"]["nxobs"] ==
                    (COMPONENT / "VERSION").read_text(
                        encoding="utf-8").strip(),
                "support bundle did not preserve terminal crash evidence")
        public = b"".join(path.read_bytes() for path in bundle.iterdir())
        require(str(root).encode("utf-8") not in public,
                "support bundle leaked its private source path")

        # The launcher writes TWO shapes of nx-event-v1. The NXU/NXR receipt
        # families carry a string `code` and NO reason_code, and every V4
        # install emits NXU0012 when it rebuilds its cache from the seed. This
        # reader used to refuse that record and fail the whole bundle closed --
        # on exactly the ports that had something worth diagnosing.
        events = []
        field_shapes = (
            b'{"schema":"nx-event-v1","schema_version":1,"run_id":"r1",'
            b'"source":"bootstrap","component":"nxbootstrap","phase":'
            b'"generation","status":"observed","code":"NXU0012","details":'
            b'{"note":"runtime cache rebuilt from seed"}}\n'
            b'{"schema":"nx-event-v1","schema_version":1,"run_id":"r1",'
            b'"source":"bootstrap","phase":"runtime","status":"observed",'
            b'"code":"NXR0003","details":{}}\n'
            b'{"schema":"nx-event-v1","schema_version":1,"run_id":"r1",'
            b'"source":"bootstrap","phase":"preflight","status":"begin",'
            b'"reason_code":6190,"details":{}}\n'
        )
        support.parse_events_jsonl(field_shapes, events, "r1")
        require(len(events) == 3,
                "a real events.jsonl was refused by the support bundle")
        require(events[0]["reason_code"] == 7012 and
                events[0]["details"]["code"] == "NXU0012" and
                events[1]["reason_code"] == 8003 and
                events[2]["reason_code"] == 6190,
                "receipt codes were not mapped onto their own namespace")
        # Namespaces stay disjoint from the phase codes, so nothing conflates.
        require(support.reason_code_from_code("NXU0012") !=
                support.reason_code_from_code("NXR0012"),
                "NXU and NXR collapsed onto the same numeric code")
        # MAX_EVENTS is a memory bound against hostile input, not a claim that
        # a longer file is malformed. The runtime's events budget is 1 MiB,
        # which holds thousands of records, so a healthy long session used to
        # take the whole bundle down here.
        many = b"".join(
            (b'{"schema":"nx-event-v1","schema_version":1,"run_id":"r1",'
             b'"source":"lifecycle","phase":"loop","status":"ok",'
             b'"reason_code":1800,"details":{"note":"n%d"}}\n' % index)
            for index in range(support.MAX_EVENTS * 3)
        )
        kept = []
        support.parse_events_jsonl(many, kept, "r1")
        require(0 < len(kept) <= support.MAX_EVENTS,
                "a long healthy events file was not bounded")
        require(kept[0]["reason_code"] == 1902 and
                kept[0]["details"]["dropped_events"] > 0,
                "dropping older events was not reported")
        # The window keeps the TAIL, because the failure being diagnosed is at
        # the end, and the sequence numbers stay dense.
        require(kept[-1]["details"]["note"] ==
                "n%d" % (support.MAX_EVENTS * 3 - 1),
                "the window kept the head instead of the tail")
        require([event["sequence"] for event in kept] ==
                list(range(1, len(kept) + 1)),
                "sequence numbers are not dense after windowing")
        # A runtime log truncated at the FRONT leaves half a record that
        # still begins with the NXEVENT marker. That is a torn line, not
        # hostile input, and it used to cost the whole bundle -- including
        # when the truncation was the launcher's own budget trim.
        whole = ('NXEVENT {"schema":"nx-event-v1","source":"lifecycle",'
                 '"phase":"loop","status":"ok","reason_code":1800}')
        torn = []
        support.parse_runtime([whole[:40], whole], torn, "r1")
        require([event["reason_code"] for event in torn] == [1903, 1800],
                "a torn first line was not tolerated as a diagnostic")
        # Anywhere else it is still a refusal.
        refused = False
        try:
            support.parse_runtime([whole, whole[:40], whole], [], "r1")
        except support.BundleError:
            refused = True
        require(refused, "a malformed record mid-log was accepted")

        # Same bound, same fix, the OTHER caller: the runtime log is the
        # bundle's primary input and a 4 MiB one holds far more records than
        # MAX_EVENTS. Fixing events.jsonl alone left this standing.
        long_log = [whole] * (support.MAX_EVENTS * 3)
        bounded = []
        support.parse_runtime(long_log, bounded, "r1")
        require(0 < len(bounded) <= support.MAX_EVENTS,
                "a long healthy runtime log was not bounded")
        require(bounded[0]["reason_code"] == 1902 and
                bounded[0]["details"]["dropped_events"] > 0,
                "dropping older runtime events was not reported")
        require([event["sequence"] for event in bounded] ==
                list(range(1, len(bounded) + 1)),
                "runtime sequence numbers are not dense after windowing")

        # Third reader, same bound. The compact extractor log is budgeted at
        # 2 MiB and a recipe with hundreds of payload candidates emits far
        # more than MAX_EVENTS matching lines.
        rejected = "[2026-08-29 10:00:00] payload rejected: not for this abi"
        extracted = []
        support.parse_extractor([rejected] * (support.MAX_EVENTS * 2),
                                extracted, "r1")
        require(0 < len(extracted) <= support.MAX_EVENTS,
                "a long healthy extractor log was not bounded")
        require(extracted[0]["reason_code"] == 1902,
                "dropping older extractor events was not reported")
        # No reader may be added that skips the shared window: this defect was
        # fixed three times because three copies drifted apart.
        source = (COMPONENT / "nx-support-bundle.py").read_text(
            encoding="utf-8")
        for reader in ("parse_runtime", "parse_events_jsonl",
                       "parse_extractor"):
            body = source.split("def %s(" % reader, 1)[1].split("\ndef ", 1)[0]
            require("EventWindow(events)" in body and "window.flush(" in body,
                    "%s does not go through the bounded window" % reader)

        # The PERF and VSYNC receipts are the whole deliverable of two debts
        # and the bundle used to drop both: the numbers survived only in a raw
        # log nobody is asked to send. Proven against the REAL C emitter, not
        # a string typed here -- a format drift would otherwise be invisible.
        emitter = """
#include <stdio.h>
#include "nxobs_perf.h"
int main(void) {
  nxobs_perf perf;
  char buf[1024];
  if (nxobs_perf_init(&perf) != 0) return 1;
  nxobs_perf_mark(&perf, NXOBS_PERF_LAUNCHER_START);
  nxobs_perf_mark(&perf, NXOBS_PERF_NXEXTRACT_START);
  nxobs_perf_declare_swap_interval(&perf, 1, 0);
  nxobs_perf_present(&perf);
  nxobs_perf_present(&perf);
  if (nxobs_perf_receipt(&perf, buf, sizeof buf) > 0) printf("%s\\n", buf);
  if (nxobs_perf_vsync_receipt(&perf, buf, sizeof buf) > 0) printf("%s\\n", buf);
  return 0;
}
"""
        compiler = shutil.which(os.environ.get("CC", "")) or \
            shutil.which("cc") or shutil.which("gcc")
        require(compiler is not None, "no C compiler for the perf emitter")
        emit_source = root / "emit_perf.c"
        emit_source.write_text(emitter, encoding="utf-8")
        emit_binary = root / "emit_perf"
        build = subprocess.run(
            [compiler, "-std=c99", "-D_POSIX_C_SOURCE=200809L",
             "-Wall", "-Werror",
             "-I", str(COMPONENT / "include"), "-o", str(emit_binary),
             str(emit_source), str(COMPONENT / "src" / "nxobs_perf.c")],
            capture_output=True, text=True)
        require(build.returncode == 0,
                "the real perf emitter did not build: %s"
                % build.stderr.strip().splitlines()[-1:])
        emitted = subprocess.run([str(emit_binary)], capture_output=True,
                                 text=True)
        require(emitted.returncode == 0, "the real perf emitter failed to run")
        receipts = [line for line in emitted.stdout.splitlines() if line]
        require(len(receipts) == 2,
                "the emitter produced %d receipts, expected PERF and VSYNC"
                % len(receipts))
        ingested = []
        support.parse_runtime(receipts, ingested, "r1")
        codes = {event["reason_code"] for event in ingested}
        require(1810 in codes and 1811 in codes,
                "the bundle dropped the PERF/VSYNC receipts the runtime "
                "printed")
        vsync = [e for e in ingested if e["reason_code"] == 1811][0]
        # The point of the VSYNC receipt: a driver that ignores the request
        # has to be visible, so requested and effective are carried apart.
        require(vsync["details"]["requested"] == 1 and
                vsync["details"]["effective"] == 0 and
                vsync["details"]["honored"] == 0,
                "the bundle lost the requested/effective split")
        perf_event = [e for e in ingested if e["reason_code"] == 1810][0]
        require("threads" in perf_event["details"] and
                "rss_kib" in perf_event["details"],
                "the bundle lost the perf sample fields")

        # A record with neither a reason_code nor a well-formed code is still
        # hostile input and still fails closed.
        for hostile in (b'"status":"observed","details":{}}',
                        b'"status":"observed","code":"XXX0001","details":{}}',
                        b'"status":"observed","code":"NXU12","details":{}}'):
            line = (b'{"schema":"nx-event-v1","schema_version":1,'
                    b'"run_id":"r1","source":"bootstrap","phase":"x",'
                    + hostile + b"\n")
            refused = False
            try:
                support.parse_events_jsonl(line + field_shapes, [], "r1")
            except support.BundleError:
                refused = True
            require(refused, "a codeless event record was accepted")

        malformed = late_path.read_bytes().replace(
            b'"schema_version":1',
            b'"schema_version":1,"schema_version":1', 1,
        )
        malformed_path = root / "duplicate-crash.jsonl"
        malformed_path.write_bytes(malformed)
        try:
            support.build_bundle(
                runtime, extractor, root / "bad-support", "synthetic-stack",
                "synthetic-fw", crash_receipts=[malformed_path],
            )
            raise AssertionError("duplicate crash key unexpectedly passed")
        except support.BundleError:
            pass

        bad_receipts = []
        for label, mutate in (
                ("status", lambda value: value.update({"status": 0})),
                ("path", lambda value: value.update({"module": "../private"})),
                ("unknown", lambda value: value.update({"extra": True}))):
            value = dict(late)
            mutate(value)
            path = root / ("bad-crash-%s.jsonl" % label)
            path.write_text(json.dumps(value, sort_keys=True) + "\n",
                            encoding="utf-8")
            bad_receipts.append(path)
        bom = root / "bad-crash-bom.jsonl"
        bom.write_bytes(b"\xef\xbb\xbf" + late_path.read_bytes())
        bad_receipts.append(bom)
        for index, path in enumerate(bad_receipts):
            try:
                support.build_bundle(
                    runtime, extractor, root / ("bad-support-%d" % index),
                    "synthetic-stack", "synthetic-fw",
                    crash_receipts=[path],
                )
                raise AssertionError("malformed crash receipt unexpectedly passed")
            except support.BundleError:
                pass
        try:
            support.build_bundle(
                runtime, extractor, root / "too-many-crashes",
                "synthetic-stack", "synthetic-fw",
                crash_receipts=[late_path] * 9,
            )
            raise AssertionError("unbounded crash receipt list unexpectedly passed")
        except support.BundleError:
            pass

        require(early_path != abort_path,
                "independent crashes reused one evidence path")
    finally:
        shutil.rmtree(str(root))


if __name__ == "__main__":
    gate()
    print("nxobs P06 host gate: PASS crashes=3 signals_preserved=3 "
          "cross_aarch64=1 symbolized=1 private_symbol_archive=1 "
          "receipt_negatives=6")
