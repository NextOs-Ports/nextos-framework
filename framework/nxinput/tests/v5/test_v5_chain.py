#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""0.11.1 H5/H8: the complete chain judged by an INDEPENDENT oracle.

Expectations come from the fixture (physical profile, key codes, CFW line),
from the provider table nxoracle_v5 computes for the PINNED domain, and from
the schema-4 corpus the OWNER declares -- never from the harness's own
output. Every mutant of NX_CHAIN_MUTANT must be caught.
usage: test_v5_chain.py <chain_harness> <nxinput-root>
"""
import json, os, re, subprocess, sys
harness, root = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(root, "tools")); import nxoracle_v5 as ox
fx = json.load(open(os.path.join(root, "tests/v5/fixtures/incident-fp2-muos-h700.json")))
keys = [int(c, 16) for c in fx["device"]["ev_key_codes"]]; phys = fx["physical_table"]["position_to_ev_key"]
line = fx["source"].get("mapping") or fx["source"]["mapping_retro"]
owner = os.path.join(root, "tests/v5/corpus/fp2-complete.gptk")
domain = "sdl2-ascending-patched"   # the fixture's provider (pinned, measured)
O = ox.Oracle(phys, domain, keys, [])
fails = 0
def check(ok, msg):
    global fails
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok: fails += 1
# the owner's base map (parsed independently, minimal): slot -> action
base = {}; sec = None
for l in open(owner):
    l = l.strip()
    if l.startswith("["): sec = l.strip("[]"); continue
    if sec == "base" and "=" in l:
        k, v = [x.strip() for x in l.split("=", 1)]; base[k] = v
def fnv1a32(text):
    h = 2166136261
    for ch in text.encode(): h = ((h ^ ch) * 16777619) & 0xffffffff
    return h or 1
SEM2SLOT = {"a": "A", "b": "B", "x": "X", "y": "Y", "leftshoulder": "L1", "rightshoulder": "R1", "start": "START", "back": "SELECT", "leftstick": "L3", "rightstick": "R3"}
def run(pos, mutant=None, keysym="SPACE"):
    env = dict(os.environ); env.pop("NX_CHAIN_MUTANT", None)
    if mutant: env["NX_CHAIN_MUTANT"] = mutant
    r = subprocess.run([harness, domain, ",".join("%x" % k for k in keys), line, phys[pos], owner, "", keysym], capture_output=True, text=True, env=env)
    return r.stdout
def judge(pos, out):
    """oracle expectation: semantic from the provider table; action from the owner base; one press+release; prompt of gen 5 with a glyph."""
    code = O.stimulus(pos); expected_sem = O.semantic_delivered(line, code)
    slot = SEM2SLOT.get(expected_sem); binding = base.get(slot, "")
    exp_action = binding.split(":", 1)[1].split("@")[0] if binding.startswith("action:") else None
    m_sem = re.search(r"CHAIN semantic=(\S+)", out); m_act = re.search(r"CHAIN slot=\S+ binding=(\S+) action=(\S+)", out)
    sinks = re.findall(r"CHAIN sink kind=(\d+) code=(\d+) pressed=(\d) mapping_generation=(\d+)", out)
    prompt = re.search(r"CHAIN prompt token=(\S+) text=(.*?) mapping_generation=(\d+)", out)
    done = re.search(r"CHAIN result=done presses=(\d+) releases=(\d+) held=(\d+)", out)
    problems = []
    m_prov = re.search(r"CHAIN provider=(\S+) ev_key=(\S+) ordinal=(-?\d+)", out)
    # the provider the chain ran under must be the PINNED one of this fixture,
    # and the ordinal must be the pinned table's -- a self-consistent pipeline
    # on the wrong provider is exactly the V4 defect seen from inside
    if not m_prov or m_prov.group(1) != domain or int(m_prov.group(3)) != O.provider_ordinal(code): problems.append("provider/ordinal %s != pinned %s/%s" % (m_prov.groups() if m_prov else None, domain, O.provider_ordinal(code)))
    if not m_sem or m_sem.group(1) != (expected_sem or "-"): problems.append("semantic %s != %s" % (m_sem.group(1) if m_sem else None, expected_sem))
    if exp_action:
        if not m_act or m_act.group(2) != exp_action: problems.append("action %s != %s" % (m_act.group(2) if m_act else None, exp_action))
        action_sinks = [s for s in sinks if s[0] == "0" and int(s[1]) == fnv1a32(exp_action)]
        if len(action_sinks) != 2 or action_sinks[0][2] != "1" or action_sinks[1][2] != "0": problems.append("action sink press/release != exactly one each: %s" % action_sinks)
        if any(s[0] != "0" and s[0] != "4" and int(s[1]) == fnv1a32(exp_action) for s in sinks): problems.append("the same edge reached a second route kind")
        if not prompt or prompt.group(3) != "5" or prompt.group(1) in ("", "unbound") or re.search(r"\b(Button|Axis)\s*\d", prompt.group(2)) or re.fullmatch(r"\d{1,2}", prompt.group(2).strip()): problems.append("prompt not of mapping_generation 5 or not a glyph: %r" % ((prompt.groups(),) if prompt else None))
    if not done or done.group(3) != "0": problems.append("outputs still held at the end")
    kb = re.search(r"CHAIN keyboard keysym=\S+ presses=(\d+) releases=(\d+) held=(\d+)", out)
    if kb and (kb.group(1) != "1" or kb.group(3) != "0"): problems.append("keyboard edge != one press, nothing held")
    tg = re.search(r"CHAIN trigger edges=(-?\d),(-?\d),(-?\d)", out)
    if tg and tg.groups() != ("1", "0", "-1"): problems.append("trigger hysteresis: %s" % (tg.groups(),))
    return problems
# suite normal: every face/shoulder/start/select position
for pos in ("face.south", "face.east", "face.west", "face.north", "l1", "r1", "start", "select"):
    p = judge(pos, run(pos)); check(not p, "chain %s: stimulus -> provider -> owner -> route -> sink -> prompt -> release (%s)" % (pos, "; ".join(p) if p else "clean"))
# mutants: each must be caught on face.south
for mutant in ("swap_ab", "wrong_provider", "no_release", "double_route", "stale_prompt", "ordinal_prompt", "keyboard_double", "trigger_no_hysteresis"):
    p = judge("face.south", run("face.south", mutant)); check(bool(p), "MUTANT killed: %s (%s)" % (mutant, p[0] if p else "NOT CAUGHT"))
print("v5-chain: %s" % ("FAIL" if fails else "OK")); sys.exit(1 if fails else 0)
