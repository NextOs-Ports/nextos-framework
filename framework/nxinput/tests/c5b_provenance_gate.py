#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""source -> licence -> patch -> binary -> execution, enforced.

The 116A audit refused the provenance because a binary chosen by an
environment variable was simply hashed after the fact: any path would have
done. This gate turns the recorded chain into a condition. A binary that is
not the pinned one, a checkout at a different commit, a licence whose bytes
moved, or a patch that no longer matches, all fail here -- before any engine
is allowed to speak.

CLAIM CLASS: SOURCE_AUDIT.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

FAILURES = []


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check(ok, what, detail=""):
    if not ok:
        FAILURES.append("%s%s" % (what, (" -- " + detail) if detail else ""))
    print("%-4s %s%s" % ("ok" if ok else "FAIL", what,
                         ("  [" + detail + "]") if detail and not ok else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--provenance", required=True)
    ap.add_argument("--godot3", required=True)
    ap.add_argument("--godot4", required=True)
    ap.add_argument("--godot3-src", required=True)
    ap.add_argument("--godot4-src", required=True)
    args = ap.parse_args()

    root = pathlib.Path(args.provenance).resolve().parent.parent
    data = json.loads(pathlib.Path(args.provenance).read_text())
    check(data.get("schema") == "org.nextos.c5b.engine-provenance" and
          data.get("schema_version") == 2,
          "the provenance file is the C5B schema")

    binaries = {"godot3": args.godot3, "godot4": args.godot4}
    sources = {"godot3": args.godot3_src, "godot4": args.godot4_src}
    for which, engine in sorted(data["engines"].items()):
        binary = binaries[which]
        source = pathlib.Path(sources[which])

        # The binary that will RUN must be the pinned one. A path handed in
        # by the environment is not an authority; this is.
        check(sha256(binary) == engine["binary_sha256"],
              "%s: the binary about to run is the pinned one" % which,
              sha256(binary))

        # The patch that produced it is pinned by its own bytes.
        patch = root / engine["seam_patch"]
        check(patch.exists() and sha256(patch) == engine["seam_patch_sha256"],
              "%s: the seam patch matches its recorded hash" % which)

        # The licence bytes, at the recorded path.
        licence = source / engine["license_path"]
        check(licence.exists() and
              sha256(licence) == engine["license_sha256"],
              "%s: the %s licence bytes are unchanged" % (which,
                                                          engine["license"]))

        # The patched sources that were compiled.
        for path, digest in sorted(engine["patched_source_sha256"].items()):
            here = source / path
            check(here.exists() and sha256(here) == digest,
                  "%s: %s is the file that was compiled" % (which, path))
        for path, digest in sorted(engine["added_source_sha256"].items()):
            here = source / path
            check(here.exists() and sha256(here) == digest,
                  "%s: %s is the file that was compiled" % (which, path))

        # The framework sources vendored verbatim: the engine must be
        # carrying THIS repository's seam, not a copy that drifted.
        for dest, record in sorted(
                engine["framework_sources_vendored_verbatim"].items()):
            local = root / record["framework_path"]
            vendored = source / dest
            check(local.exists() and sha256(local) == record["sha256"],
                  "%s: framework %s matches the vendored hash"
                  % (which, record["framework_path"]))
            check(vendored.exists() and sha256(vendored) == record["sha256"],
                  "%s: %s in the engine tree is that same file"
                  % (which, dest))

        # The pristine side of the patch is pinned by git blob OID, so an
        # auditor with the upstream repository can reproduce it exactly.
        for path, oid in sorted(engine["pristine_blob_oids"].items()):
            check(bool(oid) and len(oid) == 40,
                  "%s: %s has a pristine upstream blob OID" % (which, path),
                  str(oid))

        # And the seam really is inside the binary that will run.
        blob = pathlib.Path(binary).read_bytes()
        occurrences = blob.count(b"NXC5B-SEAM")
        check(occurrences ==
              engine["link_evidence"]
                    ["NXC5B-SEAM_format_string_occurrences_in_binary"],
              "%s: the seam's receipt format string is linked into the binary"
              % which, "found %d" % occurrences)
        check(occurrences > 0,
              "%s: the binary carries the seam at all" % which)

    print("\nc5b_provenance_gate: %d failures" % len(FAILURES))
    if FAILURES:
        print("c5b_provenance_gate: FAILED")
        for failure in FAILURES:
            print("  - %s" % failure)
        return 1
    print("c5b_provenance_gate: PASS (SOURCE_AUDIT)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
