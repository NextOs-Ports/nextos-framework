#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Recover the PRISTINE upstream file out of the seam patch.

The `a/` side of a unified diff is the upstream file, so reversing the seam
patch over the patched tree reproduces exactly what upstream shipped -- with
no clone of the upstream repository needed at gate time. The domain gate then
refuses the result unless its SHA-256 matches the pin, which is what makes
this safe: if the patch or the tree had drifted, the reconstruction would not
hash to the pinned value.

CLAIM CLASS: SOURCE_AUDIT.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

TARGET = {
    "godot3": "platform/x11/joypad_linux.cpp",
    "godot4": "platform/linuxbsd/joypad_linux.cpp",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--patches", required=True, help="engine-patches/")
    ap.add_argument("--godot3-tree", required=True)
    ap.add_argument("--godot4-tree", required=True)
    ap.add_argument("--pins", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    patches = pathlib.Path(args.patches)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    pins = json.loads(pathlib.Path(args.pins).read_text())
    trees = {"godot3": pathlib.Path(args.godot3_tree),
             "godot4": pathlib.Path(args.godot4_tree)}

    failures = []
    for which, path in sorted(TARGET.items()):
        patch = patches / ("%s-nxc5b-seam.patch" % which)
        blocks = re.split(r"(?m)^--- ", patch.read_text())
        picked = [b for b in blocks if b.startswith("a/%s" % path)]
        if not picked:
            failures.append("%s: %s is not in %s" % (which, path, patch))
            continue
        only = out / ("%s-only.patch" % which)
        only.write_text("--- " + picked[0])
        dest = out / ("%s-joypad_linux.cpp" % which)
        dest.write_bytes((trees[which] / path).read_bytes())
        run = subprocess.run(["patch", "-R", "-s", "-p1", "-i", str(only),
                              str(dest)], capture_output=True, text=True)
        if run.returncode != 0:
            failures.append("%s: reversing the seam patch failed: %s"
                            % (which, run.stdout + run.stderr))
            continue
        digest = hashlib.sha256(dest.read_bytes()).hexdigest()
        pinned = pins["engines"][which]["sha256"]
        if digest != pinned:
            failures.append("%s: the reconstructed source is %s, not the "
                            "pinned %s" % (which, digest[:16], pinned[:16]))
            continue
        print("ok   %s: pristine %s recovered from the seam patch (%s)"
              % (which, path, digest[:16]))

    if failures:
        print("c5b_reconstruct_pristine: FAILED")
        for failure in failures:
            print("  - %s" % failure)
        return 1
    print("c5b_reconstruct_pristine: PASS (SOURCE_AUDIT)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
