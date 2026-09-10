#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""launcher configuration -> GPTK V2 -> mapping + origin -> the engine.

The 116A audit refused the owner-swap and `null` evidence because the swapped
mapping had simply been written by hand: there was no chain from what a port
owner actually configured to what the engine received. This closes it.

  1. the port SHIPS a NEXTOSCONTROLLERS V2 file (the base profile);
  2. the OWNER edits their copy -- that is the launcher configuration;
  3. c5b_v2_decide (the FRAMEWORK's own parser and dispatcher) reports the
     live decision for all eighteen controls in both files;
  4. this script projects those decisions onto the sovereign SDL line:
       null    the control's binding is REMOVED, so the engine has nothing to
               deliver on any route;
       native  the binding stays under its own name and is recorded as native;
       swap    two controls whose ACTIONS traded places also trade their
               physical bindings, so the owner's intent reaches the pad;
  5. it writes the seam declaration -- domain, provider, receipt, generation,
     GUID and the SHA-256 of the exact mapping bytes -- which is the only
     thing the in-engine seam will accept.

Nothing here decides anything about V2 semantics: step 3 is the framework.

CLAIM CLASS: FIXTURE_HOST for this projection; what the ENGINE then does with
the mapping is what the matrix gate measures.
"""
import argparse
import hashlib
import pathlib
import re
import subprocess
import sys

# One V2 control, the SDL control name(s) that carry it in a mapping line.
V2_TO_SDL = {
    "A": ["a"], "B": ["b"], "X": ["x"], "Y": ["y"],
    "L1": ["leftshoulder"], "R1": ["rightshoulder"],
    "L2": ["lefttrigger"], "R2": ["righttrigger"],
    "L3": ["leftstick"], "R3": ["rightstick"],
    "START": ["start"], "SELECT": ["back"],
    "UP": ["dpup"], "DOWN": ["dpdown"],
    "LEFT": ["dpleft"], "RIGHT": ["dpright"],
    "LEFT_STICK": ["leftx", "lefty"],
    "RIGHT_STICK": ["rightx", "righty"],
}

DECISION = re.compile(r"^V2 control=(\S+) decision=(\S+) action=(\S+)$")


def decide(tool, path, context):
    run = subprocess.run([tool, str(path), context], stdout=subprocess.PIPE,
                         text=True)
    if run.returncode != 0:
        raise SystemExit("c5b_v2_decide refused %s:\n%s" % (path, run.stdout))
    out = {}
    for line in run.stdout.splitlines():
        match = DECISION.match(line)
        if match:
            out[match.group(1)] = (match.group(2), match.group(3))
    if len(out) != len(V2_TO_SDL):
        raise SystemExit("the decider reported %d controls, not %d"
                         % (len(out), len(V2_TO_SDL)))
    return out


def split_mapping(mapping):
    """(head, [(key, value)]) preserving order and the trailing comma."""
    parts = mapping.split(",")
    head = parts[:2]
    fields = []
    for field in parts[2:]:
        if not field:
            continue
        key, _, value = field.partition(":")
        fields.append((key, value))
    return head, fields


def join_mapping(head, fields):
    return ",".join(head + ["%s:%s" % kv for kv in fields]) + ","


def project(mapping, base, owner):
    """Apply the owner's V2 decisions to the sovereign SDL line."""
    head, fields = split_mapping(mapping)
    by_key = dict(fields)
    notes = {"null": [], "native": [], "swapped": []}

    # Owner swap: two controls whose ACTIONS traded places trade bindings.
    for left in V2_TO_SDL:
        for right in V2_TO_SDL:
            if left >= right:
                continue
            b_left, b_right = base.get(left), base.get(right)
            o_left, o_right = owner.get(left), owner.get(right)
            if None in (b_left, b_right, o_left, o_right):
                continue
            if b_left[0] != "action" or b_right[0] != "action":
                continue
            if o_left[0] != "action" or o_right[0] != "action":
                continue
            if o_left[1] == b_right[1] and o_right[1] == b_left[1] and \
                    b_left[1] != b_right[1]:
                names_l = V2_TO_SDL[left]
                names_r = V2_TO_SDL[right]
                if len(names_l) != len(names_r):
                    continue
                for name_l, name_r in zip(names_l, names_r):
                    if name_l in by_key and name_r in by_key:
                        by_key[name_l], by_key[name_r] = \
                            by_key[name_r], by_key[name_l]
                notes["swapped"].append("%s<->%s" % (left, right))

    # null removes the binding entirely; native is recorded, not removed.
    drop = set()
    for control, (decision, _action) in owner.items():
        if decision == "null":
            for name in V2_TO_SDL[control]:
                if name in by_key:
                    drop.add(name)
            notes["null"].append(control)
        elif decision == "native":
            notes["native"].append(control)

    rebuilt = [(k, by_key[k]) for k, _ in fields if k not in drop]
    return join_mapping(head, rebuilt), notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--decider", required=True)
    ap.add_argument("--base-v2", required=True)
    ap.add_argument("--owner-v2", required=True)
    ap.add_argument("--mapping", required=True,
                    help="the sovereign SDL line, as stored")
    ap.add_argument("--context", default="gameplay")
    ap.add_argument("--guid", required=True)
    ap.add_argument("--domain", default="godot")
    ap.add_argument("--provider", default="portmaster-gui")
    ap.add_argument("--receipt", required=True)
    ap.add_argument("--generation", default="c3-nxinput-authority-v1")
    ap.add_argument("--out-declaration", required=True)
    ap.add_argument("--out-mapping")
    args = ap.parse_args()

    base = decide(args.decider, args.base_v2, args.context)
    owner = decide(args.decider, args.owner_v2, args.context)
    mapping = pathlib.Path(args.mapping).read_text().strip()
    served, notes = project(mapping, base, owner)

    digest = hashlib.sha256(served.encode("utf-8")).hexdigest()
    declaration = (
        "domain=%s\n" % args.domain +
        "provider=%s\n" % args.provider +
        "receipt=%s\n" % args.receipt +
        "generation=%s\n" % args.generation +
        "guid=%s\n" % args.guid +
        "mapping_sha256=%s\n" % digest +
        "mapping=%s\n" % served)
    pathlib.Path(args.out_declaration).write_text(declaration,
                                                  encoding="utf-8")
    if args.out_mapping:
        pathlib.Path(args.out_mapping).write_text(served + "\n",
                                                  encoding="utf-8")
    print("V2CHAIN base=%s owner=%s" % (args.base_v2, args.owner_v2))
    print("V2CHAIN swapped=%s" % (",".join(notes["swapped"]) or "-"))
    print("V2CHAIN null=%s" % (",".join(sorted(notes["null"])) or "-"))
    print("V2CHAIN native=%s" % (",".join(sorted(notes["native"])) or "-"))
    print("V2CHAIN mapping_sha256=%s" % digest)
    print("V2CHAIN mapping=%s" % served)
    return 0


if __name__ == "__main__":
    sys.exit(main())
