#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""0.11.3 -- the "too late to stage" guard of nxc6_stage_before_init must key
on the subsystem that IMPORTS SDL_GAMECONTROLLERCONFIG: JOYSTICK/GAMECONTROLLER
(SDL3: JOYSTICK/GAMEPAD). Keying on SDL_WasInit(0) (ANY subsystem) makes
staging refuse for every engine that brings SDL VIDEO up for its GL context
before the port opens the pad (Unity/Godot/MonoGame) -- the port then finds no
controller and the public build aborts. Measured on dArkOS/.137 (Mali-G31):
before the fix, "NXC6-STAGE result=error ... provider_method=pinned-elf" and
"FATAL: FP2 public release requires a connected controller" even with js0..2
present. The gate reads the predicate and runs its own mutant (the SDL_WasInit(0)
form put back must be reported).

Usage: test_v5_stage_subsystem.py <nxinput dir>
"""
import os, re, sys

NX = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", ".."))
GLUE = os.path.join(NX, "engine-glue", "nxc6_glue.c")

def predicate_body(text):
    m = re.search(r"static int nxc6_env_was_init\(void \*u\)\s*\{(.*?)\n\}", text, re.S)
    if not m:
        raise SystemExit("nxc6_env_was_init not found")
    return m.group(1)

def bad(body):
    # any SDL_WasInit that asks for "everything": explicit 0, or SDL_INIT_EVERYTHING.
    for call in re.findall(r"SDL_WasInit\s*\(([^)]*)\)", body):
        arg = call.strip()
        if arg in ("0", "0u", "0U") or "EVERYTHING" in arg:
            return "SDL_WasInit(%s)" % arg
    return None

def good(body):
    # SDL2 arm must gate on JOYSTICK|GAMECONTROLLER; SDL3 arm on JOYSTICK|GAMEPAD.
    sdl2 = re.search(r"SDL_WasInit\([^)]*SDL_INIT_JOYSTICK[^)]*SDL_INIT_GAMECONTROLLER[^)]*\)", body)
    sdl3 = re.search(r"SDL_WasInit\([^)]*SDL_INIT_JOYSTICK[^)]*SDL_INIT_GAMEPAD[^)]*\)", body)
    return bool(sdl2) and bool(sdl3)

def main():
    text = open(GLUE, encoding="utf-8").read()
    body = predicate_body(text)
    b = bad(body)
    if b:
        print("STAGE SUBSYSTEM GATE: FAIL -- predicate asks for %s (any subsystem)" % b); return 1
    if not good(body):
        print("STAGE SUBSYSTEM GATE: FAIL -- predicate does not gate on JOYSTICK|GAMECONTROLLER (SDL2) and JOYSTICK|GAMEPAD (SDL3)"); return 1
    # Mutant: reintroduce the 0.11.2 defect and confirm the gate reports it.
    mutant = body.replace("SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER", "0").replace("SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD", "0")
    if mutant == body:
        print("STAGE SUBSYSTEM GATE: FAIL -- mutant anchor missing"); return 1
    killed = bad(mutant) is not None
    print("MUTANT nxc6_env_was_init -> SDL_WasInit(0): %s" % ("killed" if killed else "SURVIVED"))
    if not killed:
        print("STAGE SUBSYSTEM GATE: FAIL"); return 1
    print("STAGE SUBSYSTEM GATE: PASS predicate=joystick+gamecontroller/gamepad mutants_killed=1")
    return 0

if __name__ == "__main__":
    sys.exit(main())
