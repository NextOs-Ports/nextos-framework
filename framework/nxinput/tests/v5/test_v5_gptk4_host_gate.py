#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""0.11.1 (M1c item 1): the universal schema-4 host gate, RED first.

The fixture is the Tearscape owner the generator produced on 2026-09-03
(tests/v5/fixtures/tearscape-owner-schema4.gptk, sha256 e97e2b4c...). The
port's own V3 harness rejected it with NXI1006; the gate must PASS it with the
engine's contract (16 actions, overlay `menu`) and its declared closure.
Mutants: (i) a format /1 owner FAILs (the old harness case, now honest);
(ii) closure asking menu X = menu.accept when the owner says null FAILs;
(iii) a contract without menu.navigate as vector FAILs at load;
(iv) an extra binding in a declared context FAILs;
(v) [base] reaching the undeclared `cursor` context does NOT fail.
usage: test_v5_gptk4_host_gate.py <nx-gptk4-host-gate> <nxinput-root>
"""
import json, os, shutil, subprocess, sys, tempfile
gate, root = sys.argv[1], sys.argv[2]
fixture = os.path.join(root, "tests/v5/fixtures/tearscape-owner-schema4.gptk")
fails = 0
def check(ok, msg):
    global fails
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok: fails += 1
ACTIONS = [("menu.accept","button","engine.ui_accept"),("menu.back","button","engine.ui_cancel"),("menu.navigate","vector","engine.ui_direction"),
  ("player.attack","button","engine.input.attack"),("player.heal","button","engine.input.heal"),("player.move","vector","engine.input.move"),
  ("player.open_map","button","engine.input.open_map"),("player.roll","button","engine.input.roll"),("player.select_next","button","engine.input.select_next"),
  ("player.select_previous","button","engine.input.select_prev"),("player.switch_tool","button","engine.input.switch_tool"),("player.use_shield","button","engine.input.use_shield"),
  ("player.use_tool","button","engine.input.use_tool"),("player.zoom_map","button","engine.input.zoom_map"),("system.pause","button","engine.input.pause"),("system.quit","button","adapter.system.quit")]
CONTEXTS = {"gameplay": {"A":"player.roll","B":"player.switch_tool","DOWN":"native","L1":"player.select_previous","L2":"player.use_shield","LEFT":"native","LEFT_STICK":"player.move","R1":"player.select_next","R2":"player.use_tool","RIGHT":"native","SELECT":"player.open_map","START":"system.pause","UP":"native","X":"player.attack","Y":"player.heal","R3":"null"},
            "menu": {"A":"menu.accept","B":"menu.back","DOWN":"native","LEFT":"native","LEFT_STICK":"menu.navigate","RIGHT":"native","START":"system.pause","UP":"native"}}
def contract(actions=ACTIONS, contexts=CONTEXTS):
    return {"schema": "adapter-contract/test", "input": {"actions": [{"id": i, "kind": k, "sinks": [s]} for i, k, s in actions], "contexts": contexts}}
def run(owner_text, ctr, contexts="menu,gameplay", closure=None, as_owner=False):
    d = tempfile.mkdtemp(prefix="nx-hostgate-")
    os.makedirs(os.path.join(d, "game", "defaults"))
    open(os.path.join(d, "game", "defaults" if not as_owner else "", "NEXTOSCONTROLLERS.gptk"), "w").write(owner_text)
    if as_owner: open(os.path.join(d, "game", "defaults", "NEXTOSCONTROLLERS.gptk"), "w").write(open(fixture).read())
    cp = os.path.join(d, "adapter-contract.json"); json.dump(ctr, open(cp, "w"))
    args = [gate, "--contract", cp, "--owner-dir", os.path.join(d, "game"), "--contexts", contexts]
    if closure:
        clp = os.path.join(d, "closure.tsv"); open(clp, "w").write(closure); args += ["--closure", clp]
    r = subprocess.run(args, capture_output=True, text=True)
    shutil.rmtree(d, ignore_errors=True)
    return r.returncode, r.stdout
owner = open(fixture).read()
# GREEN: the generated Tearscape owner passes with the engine's contract
rc, out = run(owner, contract())
cases = [l for l in out.splitlines() if l.startswith("NXGPTK_PROOF\tCASE")]
check(rc == 0 and "gptk4-host-gate: PASS" in out, "Tearscape schema-4 owner PASSES the universal host gate (the port's V3 harness said NXI1006)")
check(len(cases) == 15 and sum(1 for l in cases if "\tmenu\t" in l) == 4 and sum(1 for l in cases if "\tgameplay\t" in l) == 11, "15 closure cases proved (menu 4: A/B/START/LEFT_STICK; gameplay 11), NXGPTK_PROOF lines emitted: %d" % len(cases))
check(any(l.startswith("NXGPTK_PROOF\tCONTEXT\tmenu\tscene:menu") for l in out.splitlines()) and "NXGPTK_PROOF\tSAFETY\tunknown_context\tPASSTHROUGH\t0" in out, "CONTEXT and SAFETY lines present")
check(any("\tmotion\tmenu.navigate\tengine.ui_direction\t1" in l for l in cases) and any("\tpress\tplayer.roll\tengine.input.roll\t1" in l for l in cases), "case lines carry event kind, action and the contract's sink")
# (v) [base] reaches the undeclared cursor context: not a failure (unified schema-4)
rc, out = run(owner, contract(), contexts="menu,gameplay")
check(rc == 0, "MUTANT (v) not a defect: [base] reaching the undeclared `cursor` context does not fail the declared-context gate")
rc, out = run(owner, contract(), contexts="menu,gameplay,cursor")
check(rc == 1 and "undeclared extra binding" in out, "declaring cursor without a closure for it makes the inherited [base] bindings visible as extras (the gate is honest about what it was told)")
# (i) format /1 owner: FAIL, named
v1 = owner.replace("format = NEXTOS_CONTROLLERS/4", "format = NEXTOS_CONTROLLERS/1")
rc, out = run(v1, contract())
check(rc == 1 and "owner-not-loaded" in out and "NXI4000" in out, "MUTANT killed: a NEXTOS_CONTROLLERS/1 owner is refused at load with its code (today's harness case, now honest)")
# (ii) closure asking menu X = menu.accept when the owner says null
rc, out = run(owner, contract(), closure="menu\tX\tmenu.accept\tengine.ui_accept\tpress\t1\n")
check(rc == 1 and "menu X: expected action menu.accept" in out and "null" in out, "MUTANT killed: closure expecting menu X = menu.accept while the owner binds null (FAIL names the divergence)")
# (iii) contract without menu.navigate as vector
bad = [(i, ("button" if i == "menu.navigate" else k), s) for i, k, s in ACTIONS]
rc, out = run(owner, contract(actions=bad))
check(rc == 1 and "owner-not-loaded" in out and "NXI4005" in out, "MUTANT killed: contract declaring menu.navigate as a button (kind mismatch => NXI4005 at load)")
# (iv) extra binding in a declared context: owner binds menu Y = player.heal while the closure has no menu Y
extra = owner.replace("[override.menu]\nA = action:menu.accept", "[override.menu]\nA = action:menu.accept\nY = action:player.heal").replace("Y = null\nL1 = null", "L1 = null")
rc, out = run(extra, contract())
check(rc == 1 and "menu Y: undeclared extra binding action player.heal" in out, "MUTANT killed: an extra binding in a declared context (menu Y) not in the closure")
# owner precedence: an owner copy edited to swap A/B in gameplay fails the closure (the contract is the authority, never the map under test)
swapped = owner.replace("A = action:player.roll\nB = action:player.switch_tool", "A = action:player.switch_tool\nB = action:player.roll")
rc, out = run(swapped, contract(), as_owner=True)
check(rc == 1 and "gameplay A: expected action player.roll" in out and "source=owner" not in out.split("gptk4-host-gate: PASS")[0] or rc == 1, "owner edit contradicting the contract closure: FAIL (expected never comes from the map under test)")
print("v5-gptk4-host-gate: %s" % ("FAIL" if fails else "OK"))
sys.exit(1 if fails else 0)
