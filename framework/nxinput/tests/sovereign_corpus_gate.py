#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C3 corpus gate (mission 114A).

EVERY artifact of the sealed C1 corpus goes through parsing, classification
and deduplication. For every gamecontrollerdb artifact, EVERY GUID entry --
not a truncated sample of three -- runs the full pipeline: authority
resolution with measured capabilities and an effective readback, compared
against an INDEPENDENT reference (`sovereign_reference.py`) that decides for
itself which entry wins, with which bindings, and what the runtime must hold.

The driver contributes no verdict: it prints only the outcome and the bytes
the runtime consumer ended up holding after the real setter+readback. The
reference decides what those bytes should be. A resolver that swaps, drops or
rewrites a binding the reference preserves therefore fails this gate -- proved
by the mutant run in run-sovereign-corpus-host.sh.

Large artifacts (content not embedded in the corpus) are read from the pinned
PortMaster-New git store (env NX_PM_GIT) by their recorded blob id; skipping
them silently is a failure, not a pass.
"""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sovereign_reference as ref  # noqa: E402

GPTK_LINE = re.compile(
    r"^\s*$|^\s*#|^\s*//|^\s*\[[^\]]*\]\s*$|"
    r"^\s*[A-Za-z0-9_.+-]+\s*=\s*.*$")

# The physically captured GO-Super chain from C1 (mapping chosen by the real
# control.txt/get_controls). The differential proof must keep it byte-intact.
GOSUPER_GUID = "190000004b4800000011000000010000"
GOSUPER_LINE = (
    "190000004b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,back:b12,"
    "dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,"
    "leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,"
    "rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b2,y:b3,"
    "platform:Linux,")

GUID_LINE = re.compile(r"^[0-9a-f]{32},")
EMIT = re.compile(
    r"^EMIT guid=([0-9a-f]{32}) caps=(\d+)/(\d+)/(\d+) source=(\S+) "
    r"reason=(\S+) readback=(.*)$")


def fail(message):
    print("sovereign_corpus_gate FAILED: %s" % message)
    sys.exit(1)


def artifact_content(artifact, git_dir):
    if artifact.get("content_embedded"):
        return artifact["content"]
    if not git_dir:
        fail("artifact %s is not embedded and NX_PM_GIT was not supplied"
             % artifact["sha256"][:16])
    blob = subprocess.run(
        ["git", "--git-dir", git_dir, "cat-file", "blob",
         artifact["git_blob"]],
        stdout=subprocess.PIPE, check=True).stdout
    if hashlib.sha256(blob).hexdigest() != artifact["sha256"]:
        fail("git blob for %s does not match the sealed sha256"
             % artifact["sha256"][:16])
    return blob.decode("utf-8", "replace")


def differential(driver, path, content, digest, counters):
    """Run EVERY GUID entry of one database through the resolver and judge it
    with the independent reference."""
    result = subprocess.run([driver, "emit-all", str(path)],
                            stdout=subprocess.PIPE, text=True)
    if result.returncode != 0:
        fail("emit-all could not read artifact %s" % digest[:16])
    records = []
    total = None
    for line in result.stdout.splitlines():
        match = EMIT.match(line)
        if match:
            records.append(match)
        elif line.startswith("EMIT-ALL entries="):
            total = int(line.split("=", 1)[1])
    if total is None:
        fail("emit-all produced no summary for %s" % digest[:16])
    # No silent truncation: the reference counts the entries itself.
    expected_entries = [raw for raw in content.split("\n")
                        if GUID_LINE.match(raw)]
    if total != len(expected_entries) or len(records) != total:
        fail("emit-all covered %d/%d entries of %s (records=%d)"
             % (total, len(expected_entries), digest[:16], len(records)))

    for match, raw in zip(records, expected_entries):
        guid, buttons, axes, hats = (match.group(1), int(match.group(2)),
                                     int(match.group(3)), int(match.group(4)))
        source, reason, readback = (match.group(5), match.group(6),
                                    match.group(7))
        if guid != raw[:32]:
            fail("emit-all drifted from the file order in %s" % digest[:16])
        # The capabilities are derived independently and must agree.
        if (buttons, axes, hats) != ref.caps_of_line(ref._trim(raw)):
            fail("capabilities disagree with the reference for %s in %s"
                 % (guid, digest[:16]))
        exp_source, exp_reason, exp_line, exp_bindings = \
            ref.expected_db_resolution(content, guid, (buttons, axes, hats))
        if source != exp_source or reason != exp_reason:
            fail("outcome disagrees with the reference for %s in %s: "
                 "resolver=%s/%s reference=%s/%s"
                 % (guid, digest[:16], source, reason, exp_source, exp_reason))
        if exp_source != "cfw-db-guid":
            counters["refused"] += 1
            if readback:
                fail("a refused entry still installed a mapping: %s in %s"
                     % (guid, digest[:16]))
            continue
        # BYTE-INTACT: the runtime must hold exactly the official entry.
        if readback != exp_line:
            fail("the runtime holds bytes the reference did not authorize "
                 "for %s in %s" % (guid, digest[:16]))
        # And the BINDINGS the consumer ended up with are measured from the
        # readback by the reference parser, never taken from the resolver.
        got_reason, got_bindings = ref.parse_line(readback,
                                                  (buttons, axes, hats))
        if got_reason != ref.OK:
            fail("the readback for %s in %s is not a valid mapping (%s)"
                 % (guid, digest[:16], got_reason))
        if got_bindings != exp_bindings:
            differing = sorted(
                set(got_bindings.items()) ^ set(exp_bindings.items()))[:6]
            fail("BINDING DIVERGENCE for %s in %s: %s"
                 % (guid, digest[:16], differing))
        counters["bindings"] += len(got_bindings)
        counters["admitted"] += 1

    # A directed negative on this very artifact: the first entry resolved
    # against capabilities one button short must be refused, never trimmed
    # into something playable.
    first = ref._trim(expected_entries[0])
    buttons, axes, hats = ref.caps_of_line(first)
    if buttons > 1:
        probe = subprocess.run(
            [driver, "unreachable", str(path), first[:32], str(buttons - 1),
             str(axes), str(hats)], stdout=subprocess.PIPE, text=True)
        match = EMIT.match(probe.stdout.strip().splitlines()[-1])
        if match is None:
            fail("the unreachable probe produced no record for %s"
                 % digest[:16])
        exp_source, exp_reason, _, _ = ref.expected_db_resolution(
            content, first[:32], (buttons - 1, axes, hats))
        if (match.group(5), match.group(6)) != (exp_source, exp_reason):
            fail("the unreachable probe disagrees with the reference in %s: "
                 "%s/%s vs %s/%s" % (digest[:16], match.group(5),
                                     match.group(6), exp_source, exp_reason))
        counters["unreachable_probes"] += 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--git-dir", default=None)
    args = parser.parse_args()
    work = pathlib.Path(args.work)

    corpus = json.loads(pathlib.Path(args.corpus).read_text(encoding="utf-8"))
    artifacts = corpus["artifacts"]
    if len(artifacts) != corpus["counts"]["unique_control_artifacts"]:
        fail("the corpus artifact map disagrees with its own sealed counts")

    processed = 0
    gptk_files = 0
    gptk_junk_lines = 0
    junk_catalog = []
    db_files = 0
    db_lines_ok = 0
    db_lines_bad = 0
    bad_catalog = []
    counters = {"admitted": 0, "refused": 0, "bindings": 0,
                "unreachable_probes": 0}

    for digest in sorted(artifacts):
        artifact = artifacts[digest]
        content = artifact_content(artifact, args.git_dir)
        # Deduplication proof: the sealed sha256 is the content address.
        if artifact.get("content_embedded"):
            if hashlib.sha256(content.encode("utf-8")).hexdigest() != digest:
                fail("embedded artifact %s drifted from its content address"
                     % digest[:16])
        if not artifact["used_by"]:
            fail("artifact %s lost its used_by port list" % digest[:16])
        processed += 1

        if artifact["kind"] == "portmaster-gptk":
            gptk_files += 1
            for line in content.splitlines():
                if not GPTK_LINE.match(line):
                    gptk_junk_lines += 1
                    if len(junk_catalog) < 32:
                        junk_catalog.append(
                            (digest[:12], line.strip()[:60]))
        elif artifact["kind"] == "sdl-gamecontrollerdb":
            db_files += 1
            path = work / ("db-%s.txt" % digest[:16])
            path.write_text(content, encoding="utf-8")
            result = subprocess.run(
                [args.driver, "parse-db", str(path)],
                stdout=subprocess.PIPE, text=True)
            summary = result.stdout.strip().splitlines()[-1]
            if result.returncode != 0:
                fail("parse-db could not read artifact %s" % digest[:16])
            match = re.search(r"ok=(\d+) bad=(\d+)", summary)
            db_lines_ok += int(match.group(1))
            db_lines_bad += int(match.group(2))
            for line in result.stdout.splitlines():
                if line.startswith("BADLINE") and len(bad_catalog) < 16:
                    bad_catalog.append((digest[:12], line[8:68]))
            differential(args.driver, path, content, digest, counters)
        else:
            fail("artifact %s has an unknown kind %r"
                 % (digest[:16], artifact["kind"]))

    if processed != len(artifacts):
        fail("not every artifact was processed")
    # The junk allowance is a frozen, visible number: real upstream typos
    # exist (e.g. 'guide /\"'), but growth beyond the sealed level fails.
    if gptk_junk_lines > 40:
        for item in junk_catalog:
            print("JUNK %s %s" % item)
        fail("gptk junk lines grew beyond the sealed allowance: %d"
             % gptk_junk_lines)

    # Real upstream typos exist (a missing comma in one 8BitDo entry); the
    # allowance is frozen and every bad line is catalogued -- growth fails.
    for item in bad_catalog:
        print("DB-BADLINE %s %s" % item)
    if db_lines_bad > 4:
        fail("gamecontrollerdb bad lines grew beyond the sealed allowance: %d"
             % db_lines_bad)

    # The physically captured GO-Super chain, end to end, judged by the same
    # independent reference.
    gosuper = work / "gosuper-db.txt"
    gosuper.write_text(GOSUPER_LINE + "\n", encoding="utf-8")
    differential(args.driver, gosuper, GOSUPER_LINE + "\n", "gosuper" * 3,
                 counters)
    if counters["admitted"] == 0:
        fail("the differential proof admitted nothing at all")

    print("sovereign_corpus_gate passed: artifacts=%d gptk_files=%d "
          "gptk_junk_lines=%d db_files=%d db_lines_ok=%d db_lines_bad=%d "
          "differential_admitted=%d differential_refused=%d "
          "bindings_compared=%d unreachable_probes=%d gosuper_physical=1"
          % (processed, gptk_files, gptk_junk_lines, db_files, db_lines_ok,
             db_lines_bad, counters["admitted"], counters["refused"],
             counters["bindings"], counters["unreachable_probes"]))


if __name__ == "__main__":
    main()
