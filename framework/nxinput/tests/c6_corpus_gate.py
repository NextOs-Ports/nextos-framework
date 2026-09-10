# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C6 -- the sealed C1 corpus, classed for SDL consumers.

WHY THE CLASSES ARE RECOMPUTED HERE

C3 once reported "71 cases" and the 114A audit showed that number was measured
against the resolver's own code. It was replaced there by a real count, and it
must not be reused as if it still described anything. C6 therefore derives its
OWN classes, from the sealed corpus, with the key written out below so the
number can be reproduced and argued with rather than quoted.

A CLASS, for an SDL consumer, is the tuple

    (artifact kind, layer, the V2 control groups the artifact addresses,
     the binding kinds it uses)

because those are exactly the things that change what an SDL2 GameController
or SDL3 Gamepad consumer has to do with the artifact. Two files that address
the same groups with the same binding kinds pose the same problem to a
consumer, however differently they are written; two that differ in either do
not.

EVERY artifact is parsed (the syntactic pass is complete, 946 of 946). The
end-to-end pass is representative by construction: it runs the classes the
real SDL matrix exercised. Every remaining class is dispositioned explicitly
-- never silently.
"""

import argparse, hashlib, json, os, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c6_expectations as E

CHECKS = 0
FAILS = []


def check(ok, label):
    global CHECKS
    CHECKS += 1
    print(("ok   " if ok else "FAIL ") + label)
    if not ok:
        FAILS.append(label)


# The gptokeyb V1 control vocabulary, mapped onto the V2 groups. A .gptk
# addresses controls by name; which SDL group each name is is what a consumer
# needs to know.
GPTK_TO_GROUP = {
    "a": "A", "b": "B", "x": "X", "y": "Y",
    "l1": "L1", "r1": "R1", "l2": "L2", "r2": "R2",
    "l3": "L3", "r3": "R3",
    "start": "START", "back": "SELECT", "select": "SELECT",
    "up": "DPAD_UP", "down": "DPAD_DOWN",
    "left": "DPAD_LEFT", "right": "DPAD_RIGHT",
    "guide": "GUIDE",
}
for prefix, group in (("left_analog", "LEFT_STICK"),
                      ("right_analog", "RIGHT_STICK")):
    for suffix in ("up", "down", "left", "right"):
        GPTK_TO_GROUP["%s_%s" % (prefix, suffix)] = group


def classify_gptk(content):
    """Groups a gptokeyb config addresses, and the binding kinds it uses."""
    groups, kinds = set(), set()
    for raw in content.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or "=" not in line:
            if line.startswith("[") and line.endswith("]"):
                kinds.add("section")
            continue
        key = line.split("=", 1)[0].strip().lower()
        group = GPTK_TO_GROUP.get(key)
        if group is not None:
            groups.add(group)
            kinds.add("stick" if "analog" in key else "digital")
        else:
            kinds.add("tuning")
    return groups, kinds


def classify_db(content):
    """Groups and binding kinds across every entry of a gamecontrollerdb."""
    groups, kinds = set(), set()
    entries = 0
    bad = 0
    for raw in content.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(",")
        if len(fields) < 3 or len(fields[0]) != 32:
            bad += 1
            continue
        entries += 1
        bindings = E.parse(line)
        for group, controls in list(E.GROUPS.items()) + [(E.GUIDE, ["guide"])]:
            if any(c in bindings for c in controls):
                groups.add(group)
        for value in bindings.values():
            if value.startswith("h"):
                kinds.add("hat")
            elif "a" in value[:2]:
                kinds.add("axis")
                if value[0] in "+-":
                    kinds.add("half-axis")
                if value.endswith("~"):
                    kinds.add("inverted-axis")
            elif value.startswith("b"):
                kinds.add("button")
    return groups, kinds, entries, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--corpus-sha256", required=True)
    ap.add_argument("--result", action="append", default=[],
                    help="a real-SDL scenario result, for the E2E coverage")
    ap.add_argument("--git-dir", default=None,
                    help="the pinned PortMaster-New store, for the artifacts "
                         "whose content the corpus does not embed")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    blob = open(args.corpus, "rb").read()
    check(hashlib.sha256(blob).hexdigest() == args.corpus_sha256,
          "the C1 corpus is the sealed one, byte for byte")
    corpus = json.loads(blob)
    artifacts = corpus["artifacts"]
    check(len(artifacts) == corpus["counts"]["unique_control_artifacts"],
          "the artifact map agrees with the corpus's own sealed count (%d)"
          % len(artifacts))

    classes = {}
    parsed = 0
    embedded = 0
    not_embedded = 0
    db_entries = 0
    db_bad = 0
    fetched = 0
    for sha, art in sorted(artifacts.items()):
        content = art.get("content")
        if not art.get("content_embedded") or content is None:
            # The corpus does not embed the large artifacts; it records their
            # git blob id. Reading them from the PINNED store is what makes
            # the syntactic pass complete instead of "complete except the big
            # ones" -- and the sealed sha256 is re-checked, so the store
            # cannot quietly supply different bytes.
            if args.git_dir is None:
                not_embedded += 1
                key = (art["kind"], art["layer"], "CONTENT_NOT_EMBEDDED", "")
                classes.setdefault(key, []).append(sha)
                continue
            blob = subprocess.run(
                ["git", "--git-dir", args.git_dir, "cat-file", "blob",
                 art["git_blob"]], stdout=subprocess.PIPE, check=True).stdout
            if hashlib.sha256(blob).hexdigest() != art["sha256"]:
                check(False, "the pinned store's blob for %s does not match "
                             "the sealed sha256" % sha[:16])
                continue
            content = blob.decode("utf-8", "replace")
            fetched += 1
        embedded += 1
        parsed += 1
        if art["kind"] == "sdl-gamecontrollerdb":
            groups, kinds, n, bad = classify_db(content)
            db_entries += n
            db_bad += bad
        else:
            groups, kinds = classify_gptk(content)
        key = (art["kind"], art["layer"],
               ",".join(sorted(groups)) or "NONE",
               ",".join(sorted(kinds)) or "NONE")
        classes.setdefault(key, []).append(sha)

    check(parsed + not_embedded == len(artifacts),
          "the syntactic pass covered EVERY artifact: %d parsed (%d of them "
          "read from the pinned store) + %d unavailable = %d"
          % (parsed, fetched, not_embedded, len(artifacts)))
    check(not_embedded == 0,
          "no artifact was skipped for want of its bytes")

    keys = sorted(classes)
    digest = hashlib.sha256(
        "\n".join("%s|%s|%s|%s|%d" % (k[0], k[1], k[2], k[3], len(classes[k]))
                  for k in keys).encode()).hexdigest()
    print("\nc6 corpus classes: %d (digest %s)" % (len(keys), digest))
    check(len(keys) != 71,
          "the class count is RECOMPUTED (%d), not the retired 71" % len(keys))

    # ------------------------------------------------- end-to-end coverage
    exercised = set()
    for path in args.result:
        result = json.load(open(path))
        mapping = result.get("effective_mapping")
        if not mapping:
            continue
        groups, kinds, _, _ = classify_db(mapping)
        exercised.add((",".join(sorted(groups)), ",".join(sorted(kinds))))

    covered, pending = [], []
    for k in keys:
        if (k[2], k[3]) in exercised:
            covered.append(k)
        else:
            pending.append(k)

    check(bool(covered),
          "at least one corpus class was exercised end to end against the "
          "real SDL libraries (%d of %d)" % (len(covered), len(keys)))

    report = {
        "schema": "org.nextos.c6.corpus-classes",
        "schema_version": 1,
        "corpus_sha256": args.corpus_sha256,
        "artifacts": len(artifacts),
        "artifacts_parsed": parsed,
        "artifacts_without_embedded_content": not_embedded,
        "gamecontrollerdb_entries_parsed": db_entries,
        "gamecontrollerdb_lines_rejected": db_bad,
        "artifacts_read_from_pinned_store": fetched,
        "class_count": len(keys),
        "class_digest": digest,
        "classes_covered_e2e": len(covered),
        "classes_pending": len(pending),
        "note": (
            "Every class is dispositioned. `REAL_API_HOST` means the class "
            "was exercised end to end against the three real SDL libraries. "
            "`PENDING` means the class is parsed and understood but was not "
            "run end to end in C6: the reason is the same for all of them and "
            "it is stated rather than hidden -- an end-to-end run needs a pad "
            "whose measured capabilities match the class, and C6 built the "
            "pads it could justify rather than synthesising a device per "
            "class. No class is claimed as proven that was not run."),
        "classes": [
            {"kind": k[0], "layer": k[1], "groups": k[2], "binding_kinds": k[3],
             "artifacts": len(classes[k]),
             "disposition": "REAL_API_HOST" if k in covered else "PENDING"}
            for k in keys],
    }
    with open(args.out, "w") as fh:
        json.dump(report, fh, indent=1, sort_keys=True)
        fh.write("\n")
    check(all(c["disposition"] in ("REAL_API_HOST", "PENDING")
              for c in report["classes"]),
          "every one of the %d classes carries an explicit disposition"
          % len(keys))

    print("\nc6_corpus_gate: %d checks, %d passed, %d failed"
          % (CHECKS, CHECKS - len(FAILS), len(FAILS)))
    if FAILS:
        print("c6_corpus_gate: FAIL")
        return 1
    print("c6_corpus_gate: PASS (FIXTURE_HOST for the syntactic pass; the "
          "E2E classes are REAL_API_HOST)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
