#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""C5B positive control gate: the official bundle against an independent
GUID -> capabilities oracle.

WHAT THE 116A AUDIT REFUSED, AND WHAT CHANGED
---------------------------------------------
Rejected: two generic profiles reused for every GUID, a row count multiplied
by profile and by engine ("525/1050"), a probe that reauthored the line before
testing it, and a pure-C program reported as REAL_API_HOST.

Now: one row per GUID, never per engine and never per shape. The capability of
a GUID is what the kernel answered on the node it created (build_guid_
capability_corpus.py). A GUID with no record is PENDING_NO_CAPABILITIES and
counts as nothing. The bundle bytes are read by the probe itself, straight out
of the sealed file, delimiters included.

THE ORACLE MUST BE ABLE TO FAIL. The shapes differ, so official lines that
bind a hat this shape has not, or an axis past its count, are REFUSED -- and
this gate requires at least one such refusal. An oracle that cannot fail
proves nothing, which is precisely why the old one was rejected.

CLAIM CLASS: FIXTURE_HOST. No engine runs here.
"""
import argparse
import collections
import hashlib
import json
import pathlib
import re
import subprocess
import sys

ROW = re.compile(
    r"^CORPUS guid=([0-9a-f]{32}) shape=(\S+) bytes=(\d+) godot3=(\S+) "
    r"intact3=(\d) godot4=(\S+) intact4=(\d) buttons=(\d+) axes=(\d+) "
    r"hats=(\d+) meta=(\d+) unreachable=(\d+) reason3=(.*)$")

ADMITTED = {"byte-intact", "converted"}
BLOCKED_OK = {"unreachable-blocked", "absinfo-blocked", "hat-blocked"}


def fail(message):
    print("godot_corpus_gate FAILED: %s" % message)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True)
    ap.add_argument("--bundle", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--records-out", required=True)
    ap.add_argument("--min-swapped", type=int, default=2)
    ap.add_argument("--min-nintendo", type=int, default=4)
    ap.add_argument("--min-shapes", type=int, default=6)
    args = ap.parse_args()

    corpus = json.loads(pathlib.Path(args.corpus).read_text())
    if corpus.get("schema") != "org.nextos.c5b.guid-capabilities" or \
            corpus.get("schema_version") != 2:
        fail("the corpus is not the C5B schema version 2")

    raw = pathlib.Path(args.bundle).read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != corpus["source_bundle_sha256"]:
        fail("the bundle is not the one the corpus was measured against (%s)"
             % digest)

    # The corpus pins each line by hash. Re-derive it from the bundle: a
    # record that no longer matches its line must not be used as an oracle.
    lines = {}
    for chunk in raw.split(b"\n"):
        fields = chunk.split(b",")
        if len(fields) >= 3 and len(fields[0]) == 32:
            lines[fields[0].decode()] = chunk

    records = []
    for rec in corpus["records"]:
        guid = rec["guid"]
        if guid not in lines:
            fail("corpus record %s has no line in the sealed bundle" % guid)
        got = hashlib.sha256(lines[guid]).hexdigest()
        if got != rec["official_line_sha256"]:
            fail("corpus record %s no longer matches its bundle line" % guid)
        caps = rec["capabilities"]
        absinfo = ";".join(
            "%s:%d:%d:%d" % (code, info["minimum"], info["maximum"],
                             info["flat"])
            for code, info in sorted(caps["absinfo"].items(),
                                     key=lambda kv: int(kv[0])))
        records.append(
            "REC guid=%s shape=%s KEY=%s ABS=%s ABSINFO=%s\n"
            % (guid, rec["shape"],
               ";".join(str(c) for c in caps["ev_key"]),
               ";".join(str(c) for c in caps["ev_abs"]),
               absinfo))
    pathlib.Path(args.records_out).write_text("".join(records),
                                              encoding="utf-8")

    run = subprocess.run([args.probe, args.bundle, args.records_out],
                         stdout=subprocess.PIPE, text=True)
    print(run.stdout, end="")
    if run.returncode != 0:
        fail("the corpus probe did not run cleanly")

    by_guid = {}
    shapes = set()
    total = None
    for line in run.stdout.splitlines():
        if line.startswith("CORPUS-TOTAL"):
            total = int(line.split("rows=")[1])
            continue
        match = ROW.match(line)
        if not match:
            if line.startswith("CORPUS"):
                fail("unexpected probe line: %s" % line)
            continue
        guid, shape, _bytes, r3, i3, r4, i4 = match.groups()[:7]
        if guid in by_guid:
            fail("guid %s was counted twice: the row count is being "
                 "multiplied" % guid)
        by_guid[guid] = (shape, r3, int(i3), r4, int(i4))
        shapes.add(shape)

    if total is None or total != len(by_guid):
        fail("the probe's own total (%s) disagrees with the rows counted (%d)"
             % (total, len(by_guid)))
    if len(by_guid) != len(corpus["records"]):
        fail("%d records went in and %d rows came out"
             % (len(corpus["records"]), len(by_guid)))
    if len(shapes) < args.min_shapes:
        fail("only %d hardware shapes were exercised" % len(shapes))

    meta = {r["guid"]: r for r in corpus["records"]}
    counts = collections.Counter()
    swapped_intact = 0
    nintendo = 0
    refused = 0
    for guid, (shape, r3, i3, r4, i4) in sorted(by_guid.items()):
        rec = meta[guid]
        counts[r3] += 1
        if r3 != r4:
            fail("guid %s: godot3 said %s and godot4 said %s; the two majors "
                 "share this domain and must agree" % (guid, r3, r4))
        if r3 in ADMITTED:
            if r3 == "byte-intact" and not (i3 and i4):
                fail("guid %s was reported byte-intact but the bytes moved"
                     % guid)
            if b"a:b1,b:b0" in lines[guid]:
                # ANTI-MASKING. The audit's central point: an official line
                # that says `a` sits on physical b1 must survive untouched.
                # If it were ever rewritten, the layout inference this
                # release removed would be back.
                if r3 != "byte-intact" or not i3 or not i4:
                    fail("guid %s carries the official a:b1,b:b0 and it was "
                         "not preserved byte for byte (%s)" % (guid, r3))
                swapped_intact += 1
            if rec["selected_because"] == "Nintendo layout":
                nintendo += 1
        elif r3 in BLOCKED_OK:
            refused += 1
        else:
            fail("guid %s produced %s, which is neither an admission nor a "
                 "capability refusal" % (guid, r3))

    if refused == 0:
        fail("no official line was refused by any shape; an oracle that "
             "cannot fail is not an oracle")
    if swapped_intact < args.min_swapped:
        fail("only %d official a:b1,b:b0 lines were admitted byte-intact "
             "(wanted %d)" % (swapped_intact, args.min_swapped))
    if nintendo < args.min_nintendo:
        # Nintendo lines that a shape legitimately refuses are still evidence,
        # so count the ones present rather than demanding they all admit.
        present = sum(1 for g in by_guid
                      if meta[g]["selected_because"] == "Nintendo layout")
        if present < args.min_nintendo:
            fail("only %d Nintendo layouts are in the corpus" % present)

    print("godot_corpus_gate: %d GUIDs, one row each, %d shapes"
          % (len(by_guid), len(shapes)))
    for name, n in sorted(counts.items()):
        print("  %-22s %d" % (name, n))
    print("  a:b1,b:b0 preserved byte-intact: %d" % swapped_intact)
    print("  refused by their shape's real capability: %d" % refused)
    print("  bundle sha256: %s" % digest)
    print("godot_corpus_gate: PASS (FIXTURE_HOST; no engine ran here)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
