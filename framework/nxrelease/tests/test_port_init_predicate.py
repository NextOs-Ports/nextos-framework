#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxrelease 0.4.3 (review 2, F7): a port must not carry its own SDL init
predicate. The staging boundary belongs to the seam (nxc6_stage_before_init,
JOYSTICK|GAMECONTROLLER); a port-side SDL_WasInit(0) refuses to stage for
every engine that brings VIDEO up first (the 0.11.3 defect kept alive in a
copy the framework gate cannot see). Mutant inside: the Tearscape shape."""
import importlib.util, shutil, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("nxrelease", ROOT / "framework/nxrelease/nxrelease.py")
nxr = importlib.util.module_from_spec(spec); spec.loader.exec_module(nxr)
fails = 0
def check(c, m):
    global fails
    print(("ok   " if c else "FAIL ") + m)
    if not c: fails += 1
def run(root):
    try:
        nxr.validate_port_init_predicate({"source_root": Path(root)})
    except nxr.ReleaseError as e:
        return str(e)
    return None
work = Path(tempfile.mkdtemp(prefix="nx-init-pred."))
try:
    src = work / "port" / "src"; src.mkdir(parents=True)
    (src / "input.c").write_text("int f(void){ return nxc6_stage_before_init(0,0,0); }\n")
    check(run(work / "port") is None, "a port that calls the seam's staging boundary passes")
    (src / "joypad_sdl.cpp").write_text("static bool nx_sdl_was_initialized() { return SDL_WasInit(0) != 0; }\n")
    m = run(work / "port")
    check(m is not None and "joypad_sdl.cpp" in m, "MUTANT killed: a port-side SDL_WasInit(0) predicate (Tearscape shape) passed")
    (src / "joypad_sdl.cpp").write_text("static bool nx() { return SDL_WasInit(SDL_INIT_EVERYTHING) != 0; }\n")
    check(run(work / "port") is not None, "MUTANT killed: SDL_INIT_EVERYTHING form passed")
    (src / "joypad_sdl.cpp").write_text("static bool nx() { return SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0; }\n")
    check(run(work / "port") is None, "the subsystem-keyed form is not an offence")
    v = work / "port" / "vendor" / "sdl2"; v.mkdir(parents=True)
    (v / "SDL_joystick.c").write_text("if (SDL_WasInit(0)) {}\n")
    check(run(work / "port") is None, "vendored third-party sources are not scanned")
finally:
    shutil.rmtree(work, ignore_errors=True)
print("nxrelease-port-init-predicate: %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
