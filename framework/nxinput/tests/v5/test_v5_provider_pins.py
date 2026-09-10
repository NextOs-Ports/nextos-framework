#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V5 gate: the compiled provider pin table (src/nxinput_provider.c) must equal
tests/providers/provider-manifest-v5.json, and every patch the manifest cites
must be present byte-exact under tests/providers/patches/."""
import hashlib, json, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
man = json.load(open(os.path.join(ROOT, "tests/providers/provider-manifest-v5.json")))
src = open(os.path.join(ROOT, "src/nxinput_provider.c")).read()
rows = re.findall(r'\{"([0-9a-f]{64})",\s*"([^"]+)",\s*NXINPUT_SDL_API_(\d),\s*NXINPUT_SDL_DOMAIN_([A-Z0-9_]+)\}', src)
dom = {"SDL2_EVDEV": "sdl2-evdev", "SDL2_ASCENDING_PATCHED": "sdl2-ascending-patched", "SDL3_EVDEV": "sdl3-evdev", "UNDECLARED": "undeclared"}
compiled = {sha: (pid, "sdl%s" % api, dom[d]) for sha, pid, api, d in rows}
fails = 0
for p in man["providers"]:
    want = (p["id"], p["api"], p["domain"])
    got = compiled.get(p["sha256"])
    if got != want:
        print("FAIL pin mismatch %s: manifest=%s compiled=%s" % (p["sha256"][:12], want, got)); fails += 1
    else:
        print("ok   pin %s %s %s" % (p["id"], p["api"], p["domain"]))
    ds = p.get("domain_source", {})
    for name, sha in ds.get("patches", {}).items():
        path = os.path.join(ROOT, "tests/providers/patches", name)
        h = hashlib.sha256(open(path, "rb").read()).hexdigest() if os.path.exists(path) else None
        if h != sha:
            print("FAIL patch %s sha %s != %s" % (name, h, sha)); fails += 1
        else:
            print("ok   patch %s pinned" % name)
    if p["domain"] != "undeclared" and ds.get("kind") == "none":
        print("FAIL %s claims a domain without a source" % p["id"]); fails += 1
extra = set(compiled) - {p["sha256"] for p in man["providers"]}
if extra:
    print("FAIL compiled rows absent from manifest: %s" % extra); fails += 1
print("v5-provider-pins: %s" % ("FAIL" if fails else "OK"))
sys.exit(1 if fails else 0)
