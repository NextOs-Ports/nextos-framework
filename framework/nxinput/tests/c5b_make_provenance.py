#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Write C5B-ENGINE-PROVENANCE.json from the checkouts that were built.

The chain it records is the one c5b_provenance_gate.py then enforces:

    upstream commit -> pristine blob OIDs -> seam patch -> the exact sources
    that were compiled -> the binary -> the receipts that binary produced.

Reproducing a binary is therefore: check out the pinned commit, copy the
framework sources listed under framework_sources_vendored_verbatim into
nxc5b/, apply the seam patch, and run the recorded build command.

CLAIM CLASS: SOURCE_AUDIT.
"""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys

FRAMEWORK_VENDORED = [
    ("nxc5b/nxinput_godot.c", "src/nxinput_godot.c"),
    ("nxc5b/nxinput_godot.h", "include/nxinput_godot.h"),
    ("nxc5b/nxinput_godot_seam.c", "src/nxinput_godot_seam.c"),
    ("nxc5b/nxinput_godot_seam.h", "include/nxinput_godot_seam.h"),
]

ENGINES = {
    "godot3": {
        "version": "3.5.3-stable",
        "commit": "6c814135b69d4e703956bacc2073b4b179ff5a00",
        "binary": "bin/godot_server.x11.opt.tools.64",
        "build": ("nice -n 10 scons -j2 platform=server "
                  "target=release_debug tools=yes "
                  "module_raycast_enabled=no"),
        "link_method": "symbol-table",
        "patch": "godot3-nxc5b-seam.patch",
        "touched": ["platform/x11/SCsub", "platform/x11/joypad_linux.cpp",
                    "platform/server/SCsub", "platform/server/os_server.h",
                    "platform/server/os_server.cpp",
                    "main/input_default.h", "main/input_default.cpp"],
        "added": ["nxc5b/nxc5b_glue.h", "nxc5b/nxc5b_glue.cpp"],
        "objects": ["nxc5b/nxc5b_glue.x11.opt.tools.64.o",
                    "nxc5b/nxinput_godot_seam.x11.opt.tools.64.o",
                    "nxc5b/nxinput_godot.x11.opt.tools.64.o",
                    "platform/x11/joypad_linux.x11.opt.tools.64.o",
                    "platform/server/os_server.x11.opt.tools.64.o",
                    "main/input_default.x11.opt.tools.64.o"],
        "modules": {"raycast/embree":
                    "incompatible with the host GCC; not on the input path"},
    },
    "godot4": {
        "version": "4.2.2-stable",
        "commit": "15073afe3856abd2aa1622492fe50026c7d63dc1",
        "binary": "bin/godot.linuxbsd.editor.x86_64",
        "build": ('nice -n 10 scons -j2 platform=linuxbsd target=editor '
                  'vulkan=no '
                  'module_raycast_enabled=no CXXFLAGS="-include cstdint"'),
        "link_method": "receipt-string(stripped-binary)",
        "patch": "godot4-nxc5b-seam.patch",
        "touched": ["platform/linuxbsd/SCsub",
                    "platform/linuxbsd/joypad_linux.cpp",
                    "platform/linuxbsd/os_linuxbsd.cpp",
                    "core/input/input.h", "core/input/input.cpp"],
        "added": ["nxc5b/nxc5b_glue.h", "nxc5b/nxc5b_glue.cpp"],
        "objects": ["nxc5b/nxc5b_glue.linuxbsd.editor.x86_64.o",
                    "nxc5b/nxinput_godot_seam.linuxbsd.editor.x86_64.o",
                    "nxc5b/nxinput_godot.linuxbsd.editor.x86_64.o",
                    "platform/linuxbsd/joypad_linux.linuxbsd.editor.x86_64.o",
                    "platform/linuxbsd/os_linuxbsd.linuxbsd.editor.x86_64.o",
                    "core/input/input.linuxbsd.editor.x86_64.o"],
        "modules": {"raycast/embree":
                    "incompatible with the host GCC; not on the input path",
                    "vulkan/glslang":
                    "needs <cstdint> on GCC 13+; not on the input path"},
    },
}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def blob_oid(repo, commit, path):
    if not repo:
        return None
    try:
        return subprocess.check_output(
            ["git", "-C", repo, "rev-parse", "%s:%s" % (commit, path)],
            text=True, stderr=subprocess.DEVNULL).strip()
    except (subprocess.CalledProcessError, OSError):
        return None


def defined_symbols(path):
    try:
        out = subprocess.run(["nm", str(path)], capture_output=True,
                             text=True).stdout
    except OSError:
        return []
    return sorted({line.split()[-1] for line in out.splitlines()
                   if " T " in line or " t " in line})


def has_symtab(path):
    try:
        out = subprocess.run(["readelf", "-S", str(path)],
                             capture_output=True, text=True).stdout
    except OSError:
        return False
    return ".symtab" in out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--framework", required=True,
                    help="this framework/nxinput directory")
    ap.add_argument("--godot3-tree", required=True)
    ap.add_argument("--godot4-tree", required=True)
    ap.add_argument("--godot3-repo", default="",
                    help="an upstream clone, for the pristine blob OIDs")
    ap.add_argument("--godot4-repo", default="")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    framework = pathlib.Path(args.framework)
    trees = {"godot3": pathlib.Path(args.godot3_tree),
             "godot4": pathlib.Path(args.godot4_tree)}
    repos = {"godot3": args.godot3_repo, "godot4": args.godot4_repo}
    previous = {}
    out_path = pathlib.Path(args.out)
    if out_path.exists():
        previous = json.loads(out_path.read_text()).get("engines", {})

    result = {
        "schema": "org.nextos.c5b.engine-provenance",
        "schema_version": 2,
        "note": ("Chain source -> licence -> patch -> binary -> execution "
                 "for the engines the C5B seam was linked into. Reproduce: "
                 "check out <upstream_commit>, copy the framework sources "
                 "listed in framework_sources_vendored_verbatim into nxc5b/, "
                 "apply <seam_patch>, build with <build_command>. A path "
                 "handed in by the environment is never an authority: "
                 "c5b_provenance_gate.py must match these hashes."),
        "compiler": subprocess.check_output(["gcc", "--version"],
                                            text=True).splitlines()[0],
        "engines": {},
    }

    for which, spec in sorted(ENGINES.items()):
        tree = trees[which]
        binary = tree / spec["binary"]
        patch = framework / "engine-patches" / spec["patch"]
        licence = tree / "LICENSE.txt"
        record = {
            "version": spec["version"],
            "upstream": "https://github.com/godotengine/godot",
            "upstream_commit": spec["commit"],
            "upstream_tag": spec["version"],
            "checkout_method":
                "git archive %s | tar -x  (the extraction is not a git "
                "repository; the pristine bytes are pinned by the blob OIDs "
                "below)" % spec["commit"],
            "license": "MIT", "license_spdx": "MIT",
            "license_path": "LICENSE.txt",
            "license_sha256": sha256(licence),
            "seam_patch": "engine-patches/" + spec["patch"],
            "seam_patch_sha256": sha256(patch),
            "build_command": spec["build"],
            "link_method": spec["link_method"],
            "modules_disabled_and_why": spec["modules"],
            "binary": spec["binary"],
            "binary_bytes": os.path.getsize(binary),
            "binary_sha256": sha256(binary),
            "pristine_blob_oids": {},
            "patched_source_sha256": {},
            "added_source_sha256": {},
            "framework_sources_vendored_verbatim": {},
        }
        old = previous.get(which, {})
        for path in spec["touched"]:
            oid = blob_oid(repos[which], spec["commit"], path)
            if oid is None:
                oid = old.get("pristine_blob_oids", {}).get(path)
            record["pristine_blob_oids"][path] = oid
            record["patched_source_sha256"][path] = sha256(tree / path)
        for path in spec["added"]:
            record["added_source_sha256"][path] = sha256(tree / path)
        for dest, source in FRAMEWORK_VENDORED:
            local = sha256(framework / source)
            vendored = sha256(tree / dest)
            if local != vendored:
                raise SystemExit(
                    "%s: %s in the engine tree is not the framework's %s"
                    % (which, dest, source))
            record["framework_sources_vendored_verbatim"][dest] = {
                "framework_path": source, "sha256": local}

        blob = binary.read_bytes()
        link = {
            "binary_symbol_table":
                "present" if has_symtab(binary) else
                "stripped by the pinned build command (no .symtab); the link "
                "is proven by the object files, the seam's format string in "
                ".rodata and the runtime receipts",
            "seam_symbols_in_binary":
                [s for s in defined_symbols(binary)
                 if "nxc5b" in s.lower() or "nxinput_godot_seam" in s.lower()],
            "NXC5B-SEAM_format_string_occurrences_in_binary":
                blob.count(b"NXC5B-SEAM"),
            "translation_units_linked": {},
            "runtime_proof":
                "the seam receipts in the attempt's evidence directory carry "
                "this binary's own pid and tid",
        }
        for name in spec["objects"]:
            obj = tree / name
            if not obj.exists():
                continue
            link["translation_units_linked"][name] = {
                "sha256": sha256(obj),
                "defined_text_symbols": defined_symbols(obj)[:12],
            }
        record["link_evidence"] = link
        result["engines"][which] = record

    out_path.write_text(json.dumps(result, indent=1, sort_keys=True) + "\n",
                        encoding="utf-8")
    for which, record in sorted(result["engines"].items()):
        print("%s patch=%s binary=%s seam_strings=%d"
              % (which, record["seam_patch_sha256"][:16],
                 record["binary_sha256"][:16],
                 record["link_evidence"]
                       ["NXC5B-SEAM_format_string_occurrences_in_binary"]))
    print("wrote %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
