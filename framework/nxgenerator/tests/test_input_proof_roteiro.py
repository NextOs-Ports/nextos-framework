#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxgenerator 0.3.17 gate: controls.proof validation and the generated
ON_DEVICE_AUTOMATED_INPUT_PROOF roteiros (nxinput 0.10.2 schema).

Hermetic: builds a schema-3 controls block in memory, renders the roteiros,
validates them with the nxinput tool's own validator, and proves the coverage
rules the mission requires. Also proves controls.proof never enters the
adapter contract."""
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gen = load("nxgen", ROOT / "framework/nxgenerator/nxgenerator.py")
proof_tool = load("nxproof", ROOT / "framework/nxinput/tools/nx-device-input-proof.py")

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", what)


def expect_error(fn, what):
    try:
        fn()
        check(False, what)
    except gen.ProjectError:
        pass


CONTROLS = {
    "schema": 3, "runtime_mapping": "nxinput-gptk", "face_layout": "auto",
    "actions": [
        {"id": "g.jump", "kind": "button", "sinks": ["adapter.input.keyevent"]},
        {"id": "g.attack", "kind": "button", "sinks": ["adapter.input.keyevent"]},
        {"id": "g.move", "kind": "vector", "sinks": ["adapter.input.motion"]},
        {"id": "g.pause", "kind": "button", "sinks": ["adapter.input.keyevent"]},
        {"id": "m.confirm", "kind": "button", "sinks": ["adapter.input.keyevent"]},
    ],
    "contexts": {
        "menu": {c: "native" for c in gen.GPTK_CONTROLS} | {"A": "m.confirm", "START": "g.pause", "L2": "null", "R2": "null"},
        "gameplay": {c: "native" for c in gen.GPTK_CONTROLS} | {"A": "g.attack", "B": "g.jump", "LEFT_STICK": "g.move",
                                                                 "START": "g.pause", "L2": "null", "R2": "null"},
    },
    "proof": {
        "navigation": {"menu": [{"wait_log": "context=menu", "timeout_s": 300}, {"sleep_ms": 3000}],
                       "gameplay": [{"press": "A"}, {"sleep_ms": 5000}, {"wait_log": "context=gameplay", "timeout_s": 60}]},
        "quit_guard": {"context": "menu", "control": "A"},
        "owner_remap": {"context": "gameplay", "null": "A", "move_to": "R2"},
        "clones": 2,
        "effects": {"g.pause": {"context": "menu", "source_regex": "stage:paused", "contexts": ["gameplay"]}},
    },
}

# validate_controls accepts proof and keeps it OUT of the adapter result
result = gen.validate_controls(CONTROLS, 3)
check("proof" not in result, "controls.proof must not enter the adapter contract result")
check(result["contexts"]["gameplay"]["A"] == "g.attack", "contexts still validated")

# invalid proof blocks
bad = json.loads(json.dumps(CONTROLS))
del bad["proof"]["navigation"]["gameplay"]
expect_error(lambda: gen.validate_controls(bad, 3), "navigation missing a declared context must be refused")
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["navigation"]["menu"].append({"press": "Z"})
expect_error(lambda: gen.validate_controls(bad, 3), "non-canonical control in navigation refused")
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["owner_remap"]["null"] = "L2"
expect_error(lambda: gen.validate_controls(bad, 3), "owner_remap.null must be a control bound to an action")
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["owner_remap"]["move_to"] = "B"
expect_error(lambda: gen.validate_controls(bad, 3), "owner_remap.move_to must be null/native in that context")
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["clones"] = 9
expect_error(lambda: gen.validate_controls(bad, 3), "clones out of range refused")
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["extra"] = 1
expect_error(lambda: gen.validate_controls(bad, 3), "unknown proof member refused")
# 0.4.2: effects -- the engine's proof after a delivery
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["effects"] = {"g.nope": {"context": "menu"}}
expect_error(lambda: gen.validate_controls(bad, 3), "effect on an undeclared action refused")
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["effects"] = {"g.pause": {"context": "nowhere"}}
expect_error(lambda: gen.validate_controls(bad, 3), "effect naming an undeclared context refused")
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["effects"] = {"g.pause": {"context": "menu", "source_regex": "("}}
expect_error(lambda: gen.validate_controls(bad, 3), "effect with an invalid regex refused")
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["effects"] = {"g.pause": {"context": "menu", "extra": 1}}
expect_error(lambda: gen.validate_controls(bad, 3), "effect with an unknown member refused")

files = gen.render_input_proof_roteiros(CONTROLS, "demo")
check(set(files) == {"input-proof-default-menu.json", "input-proof-default-gameplay.json", "input-proof-owner-remap.json", "owner-remap-NEXTOSCONTROLLERS.gptk"}, "one session per context + owner + gptk")
default = files["input-proof-default-gameplay.json"]
menu_session = files["input-proof-default-menu.json"]
owner = files["input-proof-owner-remap.json"]
for r in (default, menu_session, owner):
    steps = proof_tool.validate_roteiro(r)
    check(r["classification"] == "ON_DEVICE_AUTOMATED_INPUT_PROOF", "classification carried by the roteiro")
check(proof_tool.clones_needed(default) == 2 and proof_tool.clones_needed(menu_session) == 2, "default sessions ask for two clones (cross-pad, hotplug)")

def has(step_list, **kv):
    return any(all(s.get(k) == v for k, v in kv.items()) for s in step_list)

ds = default["steps"]
check(has(ds, expect="delivery", context="gameplay", control="B", action="g.jump", sink="adapter.input.keyevent"), "bound button -> delivery with real sink")
check(has(ds, expect="count", control="B", pressed=1, value=1) and has(ds, expect="count", control="B", pressed=0, value=1), "press/release exactly once")
check(has(ds, expect="suppressed", context="gameplay", control="L2") and has(ds, expect="no_delivery", control="L2"), "null -> suppressed, never delivered")
check(has(ds, expect="no_delivery", control="X"), "native -> no delivery")
check(has(ds, expect="delivery", context="gameplay", control="LEFT_STICK", action="g.move"), "stick -> vector delivery")
check(has(ds, expect="count", control="LEFT_STICK", pressed=0, value=1), "stick returns to neutral exactly once (edge evidence)")
check(has(ds, expect="quiet"), "neutrality windows present")
check(any(s.get("chord") == ["L1", "R1"] for s in ds) and any(s.get("chord") == ["L2", "R2"] for s in ds), "L1+R1 and L2+R2 negatives")
check(any(s.get("chord_cross") == ["SELECT", "START"] for s in ds) and has(ds, expect="log", regex="chord denied: SELECT and START on different pads"), "cross-pad denial expected")
check(has(ds, expect="no_log", regex="runtime EXIT|lifecycle exit requested"), "exit negatives")
_i_start = next(i for i, s in enumerate(ds) if s.get("press") == "START")
_i_unplug = next(i for i, s in enumerate(ds) if s.get("unplug") == 1)
_i_cross = next(i for i, s in enumerate(ds) if s.get("chord_cross") == ["SELECT", "START"])
check(_i_start < _i_unplug < _i_cross, "0.3.17: START verified before hotplug and before the negatives")
_i_stick = next(i for i, s in enumerate(ds) if s.get("hold") == "LEFT_STICK")
check(_i_start < _i_stick, "MUTANT killed (0.4.3): START tested after the stick gestures -- moving the player starts scripted dialogue whose skip eats the pause press")
check(sum(1 for s in ds if s.get("press") == "START") >= 2, "0.3.17: a START bound to an action is pressed again to undo a pause before the negatives")
# 0.4.2: the declared effect follows the START delivery -- the ENGINE must prove the paused context
_i_start_del = next(i for i, s in enumerate(ds) if s.get("expect") == "delivery" and s.get("control") == "START")
check(has(ds[_i_start_del:_i_start_del + 4], expect="context_change", context="menu", source_regex="stage:paused"), "MUTANT killed: START delivery accepted without the engine proving the pause (context_change emitted right after the delivery)")
check(not has(ds, expect="context_change", context="gameplay") and not any(s.get("expect") == "context_change" and s.get("control") for s in ds), "no effect is invented for actions the port did not declare")
check(not has(menu_session["steps"], expect="context_change"), "effect restricted to [gameplay]: the menu session (title/main menu, where START does not pause) demands none")
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["effects"]["g.pause"]["contexts"] = ["nowhere"]
expect_error(lambda: gen.validate_controls(bad, 3), "effect.contexts naming an undeclared context refused")
bad = json.loads(json.dumps(CONTROLS)); bad["proof"]["effects"]["g.pause"]["contexts"] = []
expect_error(lambda: gen.validate_controls(bad, 3), "empty effect.contexts refused")
check(not has(owner["steps"], expect="context_change"), "owner remap moves g.attack, which declares no effect: nothing invented")
check(ds[-5:-3] == [{"chord": ["SELECT", "START"], "ms": 300}, {"wait_exit": True, "timeout_s": 60}] and ds[-3]["expect"] == "log" and "lifecycle exit requested" in ds[-3]["regex"] and ds[-2:] == [{"expect": "exit_status", "value": 0}, {"expect": "process_gone"}], "ends with SELECT+START on one instance, chord-attributed clean exit, no process")
ms = menu_session["steps"]
qi = next(i for i, s in enumerate(ms) if s.get("wait_log") == "context=menu")
check(has(ms[qi:qi + 8], press="A") and has(ms[qi:qi + 10], expect="no_log"), "quit guard: A in menu never selects QUIT (menu session)")
check(has(ms, expect="delivery", context="menu", control="A", action="m.confirm"), "menu binding covered in the menu session")
check(not has(ms, expect="delivery", context="gameplay", control="B", action="g.jump"), "menu session does not claim gameplay")
check(has(ds, wait_log="context=gameplay") and has(ds, wait_log="context=menu"), "gameplay session navigates through the menu first")
check(any(s.get("unplug") == 1 for s in ds) and any(s.get("replug") == 1 for s in ds) and has(ds, expect="log", regex="controller-removed"), "hotplug of the second clone with release proof")
check(sum(1 for s in ds if s.get("hold") == "LEFT_STICK") == 5, "stick: +x, -x, +y, -y and a diagonal")
# owner remap
os_ = owner["steps"]
check(has(os_, expect="suppressed", context="gameplay", control="A") and has(os_, expect="no_delivery", control="A"), "owner: A suppressed")
check(has(os_, expect="delivery", context="gameplay", control="R2", action="g.attack"), "owner: R2 delivers the moved action")
gptk = files["owner-remap-NEXTOSCONTROLLERS.gptk"]
check("A = null" in gptk and "R2 = g.attack" in gptk, "owner GPTK carries the two edits")
check("NEXTOS_CONTROLLERS/3" in gptk and "FACE_LAYOUT = auto" in gptk, "owner GPTK has the real header (magic + FACE_LAYOUT), loadable by the runtime")
check("A = g.attack" not in gptk.split("[gameplay]")[1], "owner GPTK gameplay section no longer binds A to the action")
# deterministic
check(json.dumps(files) == json.dumps(gen.render_input_proof_roteiros(CONTROLS, "demo")), "rendering is deterministic")
# a port without proof renders no roteiro and is unaffected
plain = json.loads(json.dumps(CONTROLS)); del plain["proof"]
check(gen.validate_controls(plain, 3)["contexts"] == result["contexts"], "ports without proof unchanged")

if fails:
    print("nxgenerator input-proof roteiro gate: FAIL (%d)" % fails)
    sys.exit(1)
print("nxgenerator input-proof roteiro gate: PASS — validation, coverage, negatives, owner remap, adapter untouched, deterministic")
