# SPDX-License-Identifier: GPL-3.0-only
"""Write C6-SDL-PROVENANCE.json from the trees and binaries that were built.

This RECORDS the chain. c6_provenance_gate.py is what ENFORCES it, and the
two are deliberately separate programs: a document that checked itself would
prove nothing.
"""

import argparse, hashlib, json, os, subprocess, sys

VENDORED = {
    "nxc6_glue.c": "engine-glue/nxc6_glue.c",
    "nxc6_glue.h": "engine-glue/nxc6_glue.h",
    "nxinput_sdl_seam.c": "src/nxinput_sdl_seam.c",
    "nxinput_sdl_seam.h": "include/nxinput_sdl_seam.h",
    "nxinput_sdl.c": "src/nxinput_sdl.c",
    "nxinput_sdl.h": "include/nxinput_sdl.h",
    "nxinput_godot.c": "src/nxinput_godot.c",
    "nxinput_godot.h": "include/nxinput_godot.h",
    "nxinput_portmaster.c": "src/nxinput_portmaster.c",
    "nxinput_portmaster.h": "include/nxinput_portmaster.h",
    "nxinput_sovereign.c": "src/nxinput_sovereign.c",
    "nxinput_sovereign.h": "include/nxinput_sovereign.h",
    "nxinput_authority.c": "src/nxinput_authority.c",
    "nxinput_authority.h": "include/nxinput_authority.h",
    # 0.10.0: the bounded live-database acquisition ships inside the seam.
    "nxinput_livedb.c": "src/nxinput_livedb.c",
    "nxinput_livedb.h": "include/nxinput_livedb.h",
}


def sha256(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--framework", required=True)
    ap.add_argument("--pins", required=True)
    ap.add_argument("--tree", action="append", required=True)
    ap.add_argument("--binary", action="append", required=True)
    ap.add_argument("--build-command", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    pins = json.load(open(args.pins))
    trees = dict(s.split("=", 1) for s in args.tree)
    binaries = dict(s.split("=", 1) for s in args.binary)
    compiler = subprocess.run(["cc", "--version"], capture_output=True,
                              text=True).stdout.splitlines()[0]

    doc = {
        "schema": "org.nextos.c6.sdl-provenance",
        "schema_version": 1,
        "compiler": compiler,
        "note": (
            "Chain source -> licence -> patch -> binary -> execution for the "
            "SDL libraries the C6 seam was linked into. Reproduce: check out "
            "<upstream_commit>, copy the framework sources listed in "
            "framework_sources_vendored into src/joystick/linux/ (that "
            "directory is globbed by both majors' CMake, so nothing else has "
            "to be taught about them), apply <seam_patch>, build with "
            "<build_command>. A path handed in by the environment is never an "
            "authority: c6_provenance_gate.py must match these hashes and "
            "must recover the pristine upstream bytes by reversing the patch."),
        "sdl": {},
    }

    for name, tree in sorted(trees.items()):
        pin = pins["sdl"][name]
        target = os.path.join(tree, pin["path"])
        lic = os.path.join(tree, pin["license_path"])
        binary = binaries[name]
        vendored = {}
        for rel, fw in sorted(VENDORED.items()):
            path = os.path.join(tree, "src/joystick/linux", rel)
            vendored[rel] = {"framework_path": fw, "sha256": sha256(path)}
        with open(binary, "rb") as fh:
            blob = fh.read()
        doc["sdl"][name] = {
            "upstream": pin["upstream"],
            "upstream_commit": pin["upstream_commit"],
            "upstream_tag": pin["upstream_tag"],
            "version": pin["version"],
            "role": pin["role"],
            "license_spdx": pin["license_spdx"],
            "license_path": pin["license_path"],
            "license_sha256": sha256(lic),
            "path": pin["path"],
            "pristine_sha256": pin["sha256"],
            "patched_sha256": sha256(target),
            "seam_patch": "engine-patches/%s-nxc6-seam.patch" % name,
            "seam_patch_sha256": sha256(
                os.path.join(args.framework,
                             "engine-patches/%s-nxc6-seam.patch" % name)),
            "framework_sources_vendored": vendored,
            "binary": os.path.basename(binary),
            "binary_bytes": len(blob),
            "binary_sha256": sha256(binary),
            "build_command": args.build_command,
            "link_method": ("symbol-table" if b"nxc6_admit_before_announce"
                            in blob else "receipt-string(stripped-binary)"),
            "link_evidence": {
                "NXC6-SEAM_format_string_in_binary":
                    blob.count(b"NXC6-SEAM"),
                "seam_entry_point_symbol_visible":
                    b"nxc6_admit_before_announce" in blob,
                "runtime_proof": (
                    "the seam receipts in the attempt's evidence directory "
                    "carry this library's own pid and tid"),
            },
        }

    with open(args.out, "w") as fh:
        json.dump(doc, fh, indent=1, sort_keys=True)
        fh.write("\n")
    print("wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
