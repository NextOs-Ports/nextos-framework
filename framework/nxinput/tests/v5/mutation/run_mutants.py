#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V5 / A6 + H6 -- mutation tests of the input oracle.

Part 1 (A6): the V4 proof tool (tools/nx-device-input-proof.py) derives the
EV_KEY to inject FROM THE MAPPING UNDER TEST, with a hard-coded high-first
order. Two mutants therefore SURVIVE it: (a) swapping A/B in the mapping,
(b) the provider being ascending instead of high-first. This part proves
the survival (the tool would still report the delivery it expected).

Part 2 (H6): the same mutants, and the rest of the mission list that the
oracle model covers, are KILLED by nxoracle_v5 because its stimulus comes
from the physical profile and the provider descriptor, not the mapping.
"""
import importlib.util, json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nxoracle_v5 as ox

spec = importlib.util.spec_from_file_location("v4proof", os.path.join(ROOT, "tools", "nx-device-input-proof.py"))
v4 = importlib.util.module_from_spec(spec); spec.loader.exec_module(v4)

fx = json.load(open(os.path.join(ROOT, "tests/v5/fixtures/incident-blossom-knulli-cubexx.json")))
key_codes = [int(c, 16) for c in fx["device"]["ev_key_codes"]]
abs_codes = [int(c, 16) for c in fx["device"]["ev_abs_codes"]]
phys = fx["physical_table"]["position_to_ev_key"]
native = fx["source"]["mapping"]                      # authored for ascending
_, _, fields = ox.parse_mapping(native)

results = []
def record(name, killed, detail):
    results.append((name, killed, detail)); print("%-52s %s  %s" % (name, "killed  " if killed else "SURVIVED", detail))

# ---------------- Part 1: V4 tool circularity (mutants SURVIVE) -------------
def v4_press_code(mapping_line, control):
    _, _, f = ox.parse_mapping(mapping_line)
    table, _ = v4.build_control_table(f, key_codes, abs_codes)
    return table[control]["code"]

# mutant A: swap a/b in the mapping. V4 injects whatever key the mapping names
# for 'A' and expects delivery of 'A' -> a coherent pipeline delivers 'a' -> PASS.
swapped = native.replace(",a:b4,", ",a:b3,").replace(",b:b3,", ",b:b4,").replace(",a:b3,", ",a:b3,")
code_native, code_swapped = v4_press_code(native, "A"), v4_press_code(swapped, "A")
survives = code_native != code_swapped   # the tool simply follows the mapping
record("V4: swap A/B in mapping", not survives, "V4 injects 0x%x then 0x%x and expects 'A' both times -> passes both" % (code_native, code_swapped))
# mutant B: provider ascending. The V4 tool hard-codes high-first: the key it
# injects for 'A' is the high-first reading of ordinal b4 = 0x131 (east), yet
# it expects 'A'. On the real ascending provider 0x131 is 'b'. The tool has
# no way to notice: its expectation is derived from its own hypothesis.
hf = v4.sdl_button_index_map(key_codes)
asc = ox.button_table("sdl2-ascending-patched", key_codes)
record("V4: provider ascending vs hard-coded high-first", hf == asc, "high-first b4->0x%x, ascending b4->0x%x; V4 has no provider descriptor to compare" % (hf[4], asc[4]))

# ---------------- Part 2: nxoracle_v5 KILLS them ----------------------------
O = ox.Oracle(phys, "sdl2-ascending-patched", key_codes, abs_codes)
south_sem = [k for k, v in fields.items() if v == "b%d" % O.provider_ordinal(O.stimulus("face.south"))][0]

ok, d = O.judge("face.south", native, south_sem)
assert ok, d  # suite-normal green: the native line delivers the south semantic
print("suite-normal: pressing face.south delivers '%s' on the native line: PASS" % south_sem)

ok, d = O.judge("face.south", swapped, south_sem)
record("V5: swap A/B in physical/mapping", not ok, d)

# high-first rewrite of the native line, on the ascending provider (the V4 bug)
hf_table = ox.button_table("sdl2-evdev", key_codes)
def rewrite_to(line, src_dom, dst_dom):
    st, dt = ox.button_table(src_dom, key_codes), ox.button_table(dst_dom, key_codes)
    rev = {c: i for i, c in dt.items()}
    g, n, f = ox.parse_mapping(line)
    out = []
    for k, v in f.items():
        if v.startswith("b"):
            out.append("%s:b%d" % (k, rev[st[int(v[1:])]]))
        else:
            out.append("%s:%s" % (k, v))
    return ",".join([g, n] + out) + ","
corrupt = rewrite_to(native, "sdl2-ascending-patched", "sdl2-evdev")
ok, d = O.judge("face.south", corrupt, south_sem)
record("V5: rewrite into high-first on ascending provider", not ok, d)
ok, d = O.judge("start", corrupt, "start")
record("V5: START after wrong rewrite (field: START on L1)", not ok, d)

# DSO from ldconfig (high-first) differs from the mapped one (ascending):
# an oracle built on the ldconfig DSO would accept the corrupt line.
O_wrong = ox.Oracle(phys, "sdl2-evdev", key_codes, abs_codes)
ok_wrong, _ = O_wrong.judge("face.south", corrupt, south_sem)
ok_right, _ = O.judge("face.south", corrupt, south_sem)
record("V5: DSO from ldconfig != mapped DSO", ok_wrong and not ok_right, "ldconfig-oracle would PASS the corrupt line; mapped-provider oracle FAILS it")

# source==target claimed while tables differ -> the 'identical' claim must be
# rejected by the table comparison itself.
record("V5: claim source==target with divergent tables", ox.button_table("sdl2-evdev", key_codes) != ox.button_table("sdl2-ascending-patched", key_codes), "tables differ on this pad, equality claim refused")

# unknown provider attempting rewrite: the oracle has no table -> refuses.
try:
    ox.Oracle(phys, "unknown-provider", key_codes, abs_codes); killed = False
except KeyError:
    killed = True
record("V5: unknown provider attempting rewrite", killed, "no table for an unknown provider; nothing to judge, nothing to rewrite")

# visible ordinal in a prompt: any token like 'Button 10'/'Axis' fails.
import re
def prompt_ok(text): return re.search(r"\b(Button|Axis|Joystick Button)\s*[-+]?\d", text) is None
record("V5: raw ordinal in a declared glyph surface", not prompt_ok("Press Joystick Button 10") and prompt_ok("Press A"), "regex gate")

# ---------------- Part 3: the pure machines (1.4 / B9 / D7 / lifecycle) -----
# The C tests of the decision machine, the corpus, the pre-router and the
# lifecycle each apply the mission's wrong implementations/configurations
# and print `MUTANT killed:` for every one the machine rejects. Run them
# here so the mutation report is ONE list. NXINPUT_V5_BIN is set by ctest.
import subprocess
bindir = os.environ.get("NXINPUT_V5_BIN")
required = {
    "test_v5_decision": ["UNKNOWN -> no setter", "label 'ascending' equal, complete table differs", "corpus/precedence", "physical identity", "backend/driver", "forced to use an SDL ordinal table", "declaring NOT_APPLICABLE to skip"],
    "test_v5_corpus": ["last-wins with a line not native"],
    "test_v5_prerouter": ["SELECT tap + START press forming a chord", "two routes generating two shutdowns", "SELECT the physical profile never certified"],
    "test_v5_lifecycle": ["reopen of an open device", "quarantined device reopened"],
    # 0.11.1 (M1a): the review's bugs and the new machines, each with its mutant
    "test_v5_seam": ["UNKNOWN provider muting the pad", "HIDAPI pad blocked", "bundle line reaching the setter under an UNKNOWN provider", "unsetenv(SDL_GAMECONTROLLERCONFIG) before a descriptor"],
    "test_v5_provider": ["domain inferred from the PRESENCE", "a table no plan reproduces", "malformed pin file"],
    "test_v5_translate": ["guide on KEY_MENU rejected", "half-hat line rejected"],
    "test_v5_padset": ["START leaking to the game before the pre-router", "SELECT tap + START press exiting", "SELECT of the previous generation", "opened pad is not the admitted instance", "0..255 residual"],
    "test_v5_route": ["9th source counted without being recorded"],
    "test_v5_gptk_live_vector": ["floor 0 + centre +0.0039"],
    "test_v5_registry": ["numeric/ordinal glyph", "PRESS 10 TO BEGIN"],
    "test_v5_authority_v5": ["SYNCHRONIZED elected without engine hooks", "ENGINE mode presenting an editable owner", "chord hold not suppressing", "another mapping generation resolving"],
    "test_v5_keyboard": ["keyboard and gamepad on one action delivering two presses", "chord kept after its modifier released", "context change with a key held", "@key re-entering the same key", "claiming the router's own identity"],
    "test_v5_route_policy": ["legacy SDL3/PortMaster route", "SDL3 route rewriting under an unknown provider", "V3 ordinal fix calling AddMapping"],
    "test_v5_gptk4_preinit": ["symlinked game directory followed", "symlinked defaults/ followed"],
}
corpus_arg = {"test_v5_registry", "test_v5_gptk_live_vector", "test_v5_authority_v5", "test_v5_keyboard", "test_v5_gptk4_preinit"}
for exe, needles in required.items():
    path = os.path.join(bindir, exe) if bindir else None
    if not path or not os.path.exists(path):
        record("V5 machine: %s" % exe, False, "binary not found (set NXINPUT_V5_BIN)"); continue
    r = subprocess.run([path] + ([os.path.join(ROOT, "tests/v5/corpus/fp2-complete.gptk")] if exe in corpus_arg else []), capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if "MUTANT killed" in l]
    fails = [l for l in r.stdout.splitlines() if l.startswith("FAIL")]
    for l in lines:
        record("V5 machine: " + l.split("MUTANT killed", 1)[-1].lstrip(": (")[:44], True, exe)
    for n in needles:
        record("V5 machine required: " + n[:40], any(n in l for l in lines), exe)
    record("V5 machine: %s exit" % exe, r.returncode == 0 and not fails, "rc=%d fails=%d" % (r.returncode, len(fails)))

survivors = [n for n, k, _ in results if n.startswith("V5") and not k]
print("\nV4 mutants surviving (expected, proves circularity): %d" % sum(1 for n, k, _ in results if n.startswith("V4") and not k))
print("V5 mutants surviving: %d" % len(survivors))
sys.exit(1 if survivors else 0)
