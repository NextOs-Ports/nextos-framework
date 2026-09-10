# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C6 -- source -> licence -> patch -> binary -> execution.

CLASS: SOURCE_AUDIT. It ENFORCES the chain rather than recording it. A path
handed in by the environment is never an authority: every file must match a
hash the provenance document pins, and the binary must actually carry the
seam.

It also RECONSTRUCTS the pristine upstream file by reverse-applying the seam
patch to the patched source, and requires the result to hash to the upstream
pin. That is what makes the patch honest: if the tree had been edited
anywhere else, the reconstruction would not land on the upstream bytes.
"""

import argparse, hashlib, json, os, subprocess, sys

CHECKS = 0
FAILS = []
EXPECTED_VENDORED = {
    "nxc6_glue.c", "nxc6_glue.h",
    "nxinput_authority.c", "nxinput_authority.h",
    "nxinput_godot.c", "nxinput_godot.h",
    "nxinput_livedb.c", "nxinput_livedb.h",
    "nxinput_portmaster.c", "nxinput_portmaster.h",
    "nxinput_sdl.c", "nxinput_sdl.h",
    "nxinput_sdl_seam.c", "nxinput_sdl_seam.h",
    "nxinput_sovereign.c", "nxinput_sovereign.h",
}


def check(ok, label):
    global CHECKS
    CHECKS += 1
    print(("ok   " if ok else "FAIL ") + label)
    if not ok:
        FAILS.append(label)


def sha256(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--provenance", required=True)
    ap.add_argument("--pins", required=True)
    ap.add_argument("--tree", action="append", required=True,
                    help="name=path of the built SDL checkout")
    ap.add_argument("--binary", action="append", required=True,
                    help="name=path of the built libSDL")
    ap.add_argument("--framework", required=True,
                    help="the framework/nxinput directory")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    prov = json.load(open(args.provenance))
    pins = json.load(open(args.pins))
    trees = dict(s.split("=", 1) for s in args.tree)
    binaries = dict(s.split("=", 1) for s in args.binary)
    os.makedirs(args.out, exist_ok=True)

    for name, entry in sorted(prov["sdl"].items()):
        tree = trees.get(name)
        binary = binaries.get(name)
        check(tree is not None and binary is not None,
              "%s: a built tree and a binary were supplied" % name)
        if tree is None or binary is None:
            continue

        # 1. LICENCE. Vendoring a library into our build without carrying its
        # licence is not a paperwork problem, it is redistribution without
        # terms.
        lic = os.path.join(tree, entry["license_path"])
        check(os.path.exists(lic) and sha256(lic) == entry["license_sha256"],
              "%s: the %s licence file is present and is the pinned one"
              % (name, entry["license_spdx"]))

        # 2. THE FRAMEWORK SOURCES, vendored VERBATIM. Byte-for-byte, so the
        # library under test contains the same seam this repository holds and
        # not a local variant of it.
        check(set(entry["framework_sources_vendored"]) == EXPECTED_VENDORED,
              "%s: the vendored framework source set is closed and complete"
              % name)
        for rel, want in sorted(entry["framework_sources_vendored"].items()):
            vend = os.path.join(tree, "src/joystick/linux", rel)
            here = os.path.join(args.framework, want["framework_path"])
            check(os.path.exists(vend) and sha256(vend) == want["sha256"],
                  "%s: %s is the pinned framework file" % (name, rel))
            check(os.path.exists(here) and sha256(here) == want["sha256"],
                  "%s: %s is IDENTICAL to the framework copy at %s"
                  % (name, rel, want["framework_path"]))

        # 3. THE PATCH, and the pristine bytes it must reverse to.
        patch = os.path.join(args.framework, entry["seam_patch"])
        check(sha256(patch) == entry["seam_patch_sha256"],
              "%s: the seam patch is the pinned one" % name)
        target = os.path.join(tree, entry["path"])
        check(sha256(target) == entry["patched_sha256"],
              "%s: the patched source is the pinned one" % name)

        recovered = os.path.join(args.out, "%s-pristine.c" % name)
        with open(target, "rb") as fh:
            data = fh.read()
        with open(recovered, "wb") as fh:
            fh.write(data)
        rc = subprocess.run(["patch", "-R", "-s", "-p1", "--fuzz=0",
                             recovered, patch],
                            capture_output=True, text=True)
        pin = pins["sdl"][name]
        check(rc.returncode == 0,
              "%s: the seam patch reverse-applies cleanly (%s)"
              % (name, rc.stderr.strip() or "clean"))
        check(rc.returncode == 0 and sha256(recovered) == pin["sha256"],
              "%s: reversing the patch RECOVERS the pinned upstream bytes -- "
              "nothing else in that file was touched" % name)

        # 4. THE BINARY really carries the seam. A stripped library has no
        # symbol table, so the receipt format string in .rodata is the
        # evidence, exactly as C5B did for the stripped Godot 4.
        check(sha256(binary) == entry["binary_sha256"],
              "%s: the binary is the pinned one" % name)
        with open(binary, "rb") as fh:
            blob = fh.read()
        check(b"NXC6-SEAM" in blob,
              "%s: the seam's receipt format string is IN the library" % name)
        check(b"nxc6_admit_before_announce" in blob or
              entry.get("link_method") == "receipt-string(stripped-binary)",
              "%s: the seam entry point is linked in" % name)

    print("\nc6_provenance_gate: %d checks, %d passed, %d failed"
          % (CHECKS, CHECKS - len(FAILS), len(FAILS)))
    if FAILS:
        print("c6_provenance_gate: FAIL")
        return 1
    print("c6_provenance_gate: PASS (SOURCE_AUDIT, enforced not recorded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
