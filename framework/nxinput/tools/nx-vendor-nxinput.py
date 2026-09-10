#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Vendor the nxinput runtime sources into a port (`<port>/vendor/nxinput`)
and pin every file byte-for-byte (PINS.json, `<port>-nxinput-vendor-pins/1`).

    nx-vendor-nxinput.py <port-dir> [--patch sdl2-2.32.10-nxc6-seam.patch ...]

Copies include/, src/ (runtime only: no probe/bench tools), engine-glue/ and
the named engine patches + C6-SDL-PROVENANCE.json. The port's build must
verify PINS.json before compiling (build_universal.sh does). Records the
framework commit and branch so nxrelease/FRAMEWORK-PIN can be reconciled.
"""
import hashlib, json, os, shutil, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    port = Path(sys.argv[1]).resolve()
    patches = []; excludes = set(); note = None
    args = sys.argv[2:]
    while args:
        a = args.pop(0)
        if a == "--patch": patches.append(args.pop(0))
        elif a == "--exclude": excludes.add(args.pop(0))
        elif a == "--note": note = args.pop(0)
        else: raise SystemExit("unknown argument %s" % a)
    vendor = port / "vendor" / "nxinput"
    if vendor.exists():
        shutil.rmtree(vendor)
    files = {}
    # The SDL2 port runtime set: the V4 seam/GPTK/live files plus the V5
    # provider descriptor, physical translation, 1.4 decision machine,
    # corpus, pre-router, lifecycle, schema 4 + bridge, router, registry and
    # axis calibration. Godot/nxcompat/observe/core consumers stay out (they
    # need other headers and are not part of an SDL2 port).
    RUNTIME_SRC = (
        "nxinput_authority.c", "nxinput_authority_sdl.c", "nxinput_authority_sdl.h",
        "nxinput_exit_chord.c", "nxinput_godot.c", "nxinput_gptk.c", "nxinput_gptk_live.c",
        "nxinput_gptk_loader.c", "nxinput_gptk_motion.c", "nxinput_gptk_preinit.c",
        "nxinput_livedb.c", "nxinput_portmaster.c", "nxinput_sdl.c", "nxinput_sdl_seam.c",
        "nxinput_sovereign.c",
        "nxinput_provider.c", "nxinput_provider_linux.c", "nxinput_sha256.c",
        "nxinput_translate.c", "nxinput_decision.c", "nxinput_corpus.c",
        "nxinput_prerouter.c", "nxinput_lifecycle.c", "nxinput_gptk4.c",
        "nxinput_gptk4_bridge.c", "nxinput_route.c", "nxinput_registry.c",
        "nxinput_gptk4_preinit.c",
        "nxinput_axis_calib.c",
    )
    for sub in ("include", "src", "engine-glue"):
        src = ROOT / sub
        (vendor / sub).mkdir(parents=True)
        for f in sorted(src.iterdir()):
            if not f.is_file() or f.suffix not in (".c", ".h", ".cpp"):
                continue
            if sub == "src" and (f.name not in RUNTIME_SRC or f.name in excludes):
                continue
            if sub == "engine-glue" and f.suffix == ".cpp":
                continue
            shutil.copy2(f, vendor / sub / f.name)
            files["%s/%s" % (sub, f.name)] = sha(f)
    (vendor / "engine-patches").mkdir()
    for name in patches + ["C6-SDL-PROVENANCE.json"]:
        src = ROOT / "engine-patches" / name
        if not src.is_file():
            raise SystemExit("engine patch not found: %s" % src)
        shutil.copy2(src, vendor / "engine-patches" / name)
        files["engine-patches/%s" % name] = sha(src)
    commit = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    branch = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--abbrev-ref", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    pins = {
        "schema": "%s-nxinput-vendor-pins/1" % port.name,
        "nxinput_version": (ROOT / "VERSION").read_text().strip(),
        "framework_commit": commit,
        "framework_branch": branch,
        **({"note": note} if note else {}),
        "files": dict(sorted(files.items())),
    }
    (vendor / "PINS.json").write_text(json.dumps(pins, indent=2, sort_keys=False) + "\n")
    print("vendored nxinput %s @ %s into %s: %d files" % (pins["nxinput_version"], commit[:12], vendor, len(files)))
    return 0
if __name__ == "__main__":
    sys.exit(main())
