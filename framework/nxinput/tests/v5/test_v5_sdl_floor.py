#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""0.11.2 -- the runtime the ports vendor (src/ + engine-glue/) must not CALL
any SDL entry point born above the universal floor (nxabi policy
`sdl_floor`, authority nx-sdl-symbol-floor/1): a post-floor API is reached
through dlsym, so a CFW whose SDL predates it keeps loading the port.
nxrelease validate enforces the same rule on the final ELF; this gate
catches it at the source, before any port revendors the runtime.

Usage: test_v5_sdl_floor.py <nxinput dir>
The test also runs its own mutant: the fixed call site of nxc6_glue.c put
back as a direct import must be reported (RED), otherwise the scanner is
blind and the test fails.
"""
import json, os, re, sys, tempfile

NX = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", ".."))
FW = os.path.dirname(NX)
TABLE = os.path.join(FW, "nxabi", "sdl2-symbol-floor.tsv")
POLICY = os.path.join(FW, "nxabi", "policy-v1.json")
CALL = re.compile(r"\b(SDL_[A-Za-z0-9_]+)\s*\(")

def version(v):
    return tuple(int(x) for x in v.split("."))

def load_table():
    births = {}
    for line in open(TABLE, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        symbol, born, _source = line.rstrip("\n").split("\t")[:3]
        births[symbol] = version(born)
    return births

def floor_version():
    policy = json.load(open(POLICY, encoding="utf-8"))
    def walk(o):
        if isinstance(o, dict):
            if "floor" in o and isinstance(o["floor"], str) and o["floor"].count(".") == 2:
                yield o["floor"]
            for v in o.values():
                for f in walk(v):
                    yield f
        elif isinstance(o, list):
            for v in o:
                for f in walk(v):
                    yield f
    floors = sorted(set(walk(policy)), key=version)
    if not floors:
        raise SystemExit("policy-v1.json declares no sdl floor")
    return version(floors[0])

def strip_comments_and_strings(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(\\.|[^"\\])*"', '""', text)
    return text

def scan_file(path, births, floor):
    code = strip_comments_and_strings(open(path, encoding="utf-8").read())
    findings = []
    for m in CALL.finditer(code):
        symbol = m.group(1)
        born = births.get(symbol)
        if born is not None and born > floor:
            line = code.count("\n", 0, m.start()) + 1
            findings.append((symbol, "%d.%d.%d" % born, line))
    return findings

def vendored_runtime_sources():
    """The src/ files nx-vendor-nxinput.py ships to every port (RUNTIME_SRC):
    the scope of the floor is exactly what a port links. SDL3-only sources
    (nxinput_sdl3_*) are never vendored and speak SDL3 names anyway."""
    tool = open(os.path.join(NX, "tools", "nx-vendor-nxinput.py"), encoding="utf-8").read()
    block = re.search(r"RUNTIME_SRC\s*=\s*\((.*?)\)", tool, re.S)
    if not block:
        raise SystemExit("nx-vendor-nxinput.py: RUNTIME_SRC not found")
    return sorted(set(re.findall(r'"([^"]+\.[ch])"', block.group(1))))

def scan_tree(root, births, floor, runtime_src=None):
    out = []
    for sub in ("src", "engine-glue"):
        d = os.path.join(root, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not (name.endswith(".c") or name.endswith(".h")):
                continue
            if sub == "src" and runtime_src is not None and name not in runtime_src:
                continue
            for symbol, born, line in scan_file(os.path.join(d, name), births, floor):
                out.append("%s/%s:%d %s (SDL %s)" % (sub, name, line, symbol, born))
    return out

def main():
    births = load_table()
    floor = floor_version()
    runtime_src = vendored_runtime_sources()
    findings = scan_tree(NX, births, floor, runtime_src)
    for f in findings:
        print("POST-FLOOR DIRECT CALL: " + f)
    # Mutant: the 0.11.1 defect (direct SDL_JoystickGetDeviceInstanceID in the
    # in-process measurement) reintroduced in a copy of the tree.
    src = open(os.path.join(NX, "engine-glue", "nxc6_glue.c"), encoding="utf-8").read()
    needle = 'dlsym(\n      RTLD_DEFAULT, "SDL_JoystickGetDeviceInstanceID")'
    if needle not in src:
        print("mutant anchor missing in nxc6_glue.c"); return 1
    mutant = src.replace(needle, "(void *)0", 1).replace("instance_for_index(i) ==", "SDL_JoystickGetDeviceInstanceID(i) ==", 1)
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "src")); os.makedirs(os.path.join(tmp, "engine-glue"))
        open(os.path.join(tmp, "engine-glue", "nxc6_glue.c"), "w", encoding="utf-8").write(mutant)
        killed = any("SDL_JoystickGetDeviceInstanceID" in f for f in scan_tree(tmp, births, floor))
    print("MUTANT direct SDL_JoystickGetDeviceInstanceID: %s" % ("killed" if killed else "SURVIVED"))
    if findings or not killed:
        print("sdl floor gate: FAIL"); return 1
    print("sdl floor gate: PASS floor=%d.%d.%d vendored_src=%d symbols=%d mutants_killed=1" % (floor + (len(runtime_src), len(births))))
    return 0

if __name__ == "__main__":
    sys.exit(main())
