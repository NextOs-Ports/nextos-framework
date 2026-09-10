#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C4 corpus gate.

NEXTOSCONTROLLERS.gptk is a NextOS-own format, NOT a gptokeyb file. This gate
uses the sealed C1 corpus -- the real PortMaster `.gptk` artifacts collected
from 1375 ports -- to prove the V2 parser can never mistake one for ours:

  1. EVERY gptokeyb artifact in the corpus is REJECTED as a NextOS map. Not
     one of the 921 may parse.
  2. EVERY distinct key token that appears on the left-hand side of those
     files is put in the control position of an otherwise valid
     NEXTOS_CONTROLLERS/2 skeleton and must be REJECTED, unless it happens to
     be one of our own 18 symbolic names spelled exactly. This is the check
     that catches an accidental alias: `back`, `l2`, `left_analog_up`,
     `hotkey` and friends are upstream vocabulary, not ours.

The probe prints outcomes only; this file decides what they must be.
"""

import argparse
import collections
import hashlib
import json
import pathlib
import re
import subprocess
import sys

# Our complete vocabulary, spelled exactly as V2 requires.
OURS = frozenset((
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
    "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
    "LEFT_STICK", "RIGHT_STICK"))

KEY_LINE = re.compile(r"^\s*([A-Za-z0-9_.+-]+)\s*=")
PARSE = re.compile(r"^PARSE code=(-?\d+) schema=(\d+) error=(.*)$")
CONTROL = re.compile(r"^CONTROL token=(\S+) code=(-?\d+) error=(.*)$")


def fail(message):
    print("gptk_v2_corpus_gate FAILED: %s" % message)
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--git-dir", default=None)
    args = parser.parse_args()
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    corpus = json.loads(pathlib.Path(args.corpus).read_text(encoding="utf-8"))
    artifacts = corpus["artifacts"]
    if len(artifacts) != corpus["counts"]["unique_control_artifacts"]:
        fail("the corpus artifact map disagrees with its own sealed counts")

    tokens = collections.Counter()
    gptk_files = 0
    rejected = 0
    codes = collections.Counter()

    for digest in sorted(artifacts):
        artifact = artifacts[digest]
        if artifact["kind"] != "portmaster-gptk":
            continue
        content = artifact_content(artifact, args.git_dir)
        gptk_files += 1
        path = work / ("gptk-%s.txt" % digest[:16])
        path.write_text(content, encoding="utf-8")
        result = subprocess.run([args.probe, "parse", str(path)],
                                stdout=subprocess.PIPE, text=True)
        if result.returncode != 0:
            fail("the probe could not read artifact %s" % digest[:16])
        match = PARSE.match(result.stdout.strip().splitlines()[-1])
        if match is None:
            fail("the probe produced no PARSE record for %s" % digest[:16])
        code = int(match.group(1))
        if code == 0:
            fail("a gptokeyb artifact PARSED as a NextOS map: %s"
                 % digest[:16])
        codes[code] += 1
        rejected += 1
        for line in content.splitlines():
            hit = KEY_LINE.match(line)
            if hit:
                tokens[hit.group(1)] += 1
        path.unlink()

    if gptk_files == 0:
        fail("the corpus carried no portmaster-gptk artifact at all")

    # 2. Every upstream token, in the control position of a valid V2 file.
    accepted_ours = 0
    refused_upstream = 0
    for token in sorted(tokens):
        probe = subprocess.run([args.probe, "control", token],
                               stdout=subprocess.PIPE, text=True)
        match = CONTROL.match(probe.stdout.strip().splitlines()[-1])
        if match is None:
            fail("the probe produced no CONTROL record for %r" % token)
        code = int(match.group(2))
        if token in OURS:
            if code != 0:
                fail("our own control %r was refused in a valid V2 file (%d)"
                     % (token, code))
            accepted_ours += 1
        else:
            if code == 0:
                fail("upstream gptokeyb token %r was ACCEPTED as a NextOS "
                     "control" % token)
            refused_upstream += 1

    print("gptk_v2_corpus_gate passed: gptk_files=%d all_rejected=%d "
          "reject_codes=%s distinct_tokens=%d upstream_tokens_refused=%d "
          "our_tokens_accepted=%d"
          % (gptk_files, rejected,
             ",".join("NXI%04d:%d" % (code, count)
                      for code, count in sorted(codes.items())),
             len(tokens), refused_upstream, accepted_ours))


if __name__ == "__main__":
    main()
