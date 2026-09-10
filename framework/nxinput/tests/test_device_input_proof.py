#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxinput 0.10.2 — host gate for nx-device-input-proof (no device, no network).

ON_DEVICE_AUTOMATED_INPUT_PROOF: the framework proves controls on the real
device through device-faithful uinput clones; no human witness is required.

Proves the pure logic the device proof depends on:
  * kernel bitmask parsing (/proc/bus/input/devices, MSW first);
  * SDL2 linux joystick numbering of buttons (BTN_JOYSTICK.. first, then 0..)
    and axes (ABS ascending, hats excluded);
  * mapping-line -> kernel-code table derived ONLY from the admitted mapping
    and the node's bitmasks (a control the mapping does not bind is refused);
  * roteiro validation (must end the game itself; unknown expects refused);
  * plan compilation (press on key/axis/hat, chord, stick hold with sign);
  * windowed evaluation against fixture receipts: delivery, suppressed,
    quiet (neutrality), no_delivery, log, exit_status, process_gone, and a
    step the agent never ran.
  * static boundaries: no IP literal, no uinput, no in-port injection env.
"""
import importlib.util
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOOL = ROOT / "framework/nxinput/tools/nx-device-input-proof.py"
AGENT = ROOT / "framework/nxinput/tools/nx-input-inject-agent.py"

spec = importlib.util.spec_from_file_location("nxproof", TOOL)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", what)


# GO-Super Gamepad as the kernel reports it (dArkOS fixture, 16 keys, 4 axes).
PROC = """I: Bus=0019 Vendor=484b Product=1100 Version=0001
N: Name="GO-Super Gamepad"
P: Phys=gpio-keys/input0
H: Handlers=js0 event2 dmcfreq
B: PROP=0
B: EV=b
B: KEY=1f 0 0 f00000000 0 0 0 3db000000000000 0 0 0 0
B: ABS=1b
I: Bus=0019 Vendor=0001 Product=0001 Version=0100
N: Name="rk29-keypad"
H: Handlers=event3
B: EV=100003
B: KEY=8000 c000000000000 0
"""
devs = m.parse_proc_input_devices(PROC)
check(len(devs) == 2 and devs[0]["name"] == "GO-Super Gamepad", "proc parser: devices")
pad = devs[0]
check(m.pick_real_controller(devs)["name"] == "GO-Super Gamepad", "real controller = the js-handled gamepad, keypad ignored")
try:
    m.pick_real_controller(devs + [dict(devs[0], name="Other Pad")])
    check(False, "two candidate pads must require --controller-name")
except m.ProofError:
    pass
check(m.pick_real_controller(devs + [dict(devs[0], name="Other Pad")], "Other Pad")["name"] == "Other Pad", "named pick")
check(pad["event_node"] == "/dev/input/event2" and pad["vendor"] == "484b" and pad["product"] == "1100", "proc parser: node/ids")
# KEY bitmask: words MSW first. '3db000000000000' is word index 4 from the right:
# bits 0x120.. => BTN_SOUTH(0x130)... check a few known codes are present.
keys = set(pad["key_codes"])
check(0x130 in keys and 0x131 in keys and 0x133 in keys and 0x134 in keys, "bitmask: face buttons BTN_SOUTH/EAST/NORTH/WEST")
check(len(keys) == 17, "bitmask: 17 keys (8 face+shoulder, 4 D-pad, 5 TRIGGER_HAPPY), got %d" % len(keys))
check(pad["abs_codes"] == [0, 1, 3, 4], "ABS bitmask 0x1b -> X Y Z RX, got %r" % pad["abs_codes"])

bmap = m.sdl_button_index_map(pad["key_codes"])
check(bmap[0] == min(k for k in keys if k >= 0x120), "SDL button 0 = lowest code >= BTN_JOYSTICK")
check(list(bmap.values()) == sorted(k for k in keys if k >= 0x120) + sorted(k for k in keys if k < 0x120), "SDL button order")
amap, hats = m.sdl_axis_index_map([0, 1, 3, 4, 0x10, 0x11])
check(amap == {0: 0, 1: 1, 2: 3, 3: 4} and hats == {0: {"x": 0x10, "y": 0x11}}, "SDL axis/hat numbering")

LINE = ("1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,x:b3,y:b2,back:b12,start:b13,"
        "leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b14,rightstick:b15,"
        "dpup:b8,dpdown:b9,dpleft:b10,dpright:b11,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,")
guid, name, fields = m.parse_mapping_line(LINE)
check(guid.startswith("1900bb3e") and name == "GO-Super Gamepad" and fields["a"] == "b1", "mapping line parse")
table, maps = m.build_control_table(fields, pad["key_codes"], pad["abs_codes"])
check(table["A"] == {"type": "key", "code": bmap[1]}, "A -> SDL b1 -> kernel code")
check(table["B"] == {"type": "key", "code": bmap[0]}, "B -> SDL b0")
check(table["LEFT_STICK"]["x"]["code"] == 0 and table["LEFT_STICK"]["y"]["code"] == 1, "left stick axes")
check(table["RIGHT_STICK"]["x"]["code"] == 3 and table["RIGHT_STICK"]["y"]["code"] == 4, "right stick axes a2/a3 -> ABS_Z/RX")
check("GUIDE" not in table, "unbound control is absent, never guessed")
# hats and inverted/half axes
t2, _ = m.build_control_table({"dpup": "h0.1", "dpdown": "h0.4", "lefttrigger": "+a2", "lefty": "a1~", "leftx": "a0"},
                              [0x130], [0, 1, 3, 0x10, 0x11])
check(t2["UP"] == {"type": "hat", "code": 0x11, "value": -1.0} and t2["DOWN"]["value"] == 1.0, "hat targets")
check(t2["L2"]["type"] == "axis" and t2["L2"]["code"] == 3 and t2["L2"]["sign"] == 1.0, "half-axis trigger")
check(t2["LEFT_STICK"]["y"]["inverted"] is True, "inverted axis flag")
try:
    m.resolve_target("b9", {0: 0x130}, {}, {})
    check(False, "button beyond node capabilities must be refused")
except m.ProofError:
    pass

# roteiro validation
good = {"schema": m.ROTEIRO_SCHEMA, "port_id": "fp2", "steps": [
    {"wait_log": "context=menu", "timeout_s": 5},
    {"press": "START", "ms": 100}, {"expect": "delivery", "context": "menu", "control": "START", "action": "fp2.pause"},
    {"sleep_ms": 100}, {"expect": "quiet"},
    {"hold": "LEFT_STICK", "x": 1.0, "y": 0.0, "ms": 300}, {"expect": "delivery", "control": "LEFT_STICK"},
    {"press": "A"}, {"expect": "suppressed", "control": "A"},
    {"press": "L2"}, {"expect": "no_delivery", "control": "L2"},
    {"chord": ["SELECT", "START"], "ms": 250}, {"wait_exit": True, "timeout_s": 10},
    {"expect": "exit_status", "value": 0}, {"expect": "process_gone"}]}
steps = m.validate_roteiro(good)
check(len(steps) == 15, "roteiro validates")
for bad, why in [
    ({**good, "steps": good["steps"][:-4]}, "roteiro without wait_exit must be refused"),
    ({**good, "steps": [{"expect": "teleport"}, {"wait_exit": True}]}, "unknown expect refused"),
    ({**good, "steps": [{"press": "A", "expect": "quiet"}, {"wait_exit": True}]}, "two ops in one step refused"),
    ({**good, "schema": "other"}, "schema refused"),
]:
    try:
        m.validate_roteiro(bad)
        check(False, why)
    except m.ProofError:
        pass

plan = m.compile_plan(good, table, "/g/log.txt", "/g/nxgptk-receipt.jsonl", "/g")
ops = [s["op"] for s in plan["steps"]]
check(ops[0] == "wait_log" and ops[1] == "press" and ops[2] == "mark", "plan op sequence")
check(plan["steps"][5]["op"] == "hold_axis" and plan["steps"][5]["axes"] == [[0, 1.0], [1, 0.0]], "stick hold compiled to ABS codes")
check(plan["steps"][9]["op"] == "press" and plan["steps"][9]["codes"] == [bmap[6]], "L2 press on a key-bound trigger")
chord = [s for s in plan["steps"] if s["op"] == "chord"][0]
check(chord["keys"] == [[0, bmap[12]], [0, bmap[13]]] and not chord.get("cross_pad"), "chord keys SELECT+START on pad 0")
check(plan["clones"] == 1, "one clone when no cross-pad step")
cross = {**good, "steps": [{"chord_cross": ["SELECT", "START"]}, {"expect": "no_log", "regex": "EXIT"}, {"wait_exit": True}]}
cp = m.compile_plan(cross, table, "l", "r", "/g")
check(cp["clones"] == 2 and cp["steps"][0]["keys"] == [[0, bmap[12]], [1, bmap[13]]] and cp["steps"][0]["cross_pad"], "cross-pad chord uses two clones")
check(m.clones_needed(cross) == 2 and m.clones_needed({**good, "clones": 3}) == 3 and m.clones_needed({**good, "clones": 9}) == 3, "clone count derived and capped")
try:
    m.compile_plan({**good, "steps": [{"press": "GUIDE"}, {"wait_exit": True}]}, table, "l", "r", "/g")
    check(False, "press of an unbound control must fail closed")
except m.ProofError:
    pass

# windowed evaluation with fixture receipts
def ev(kind, context, control, action=None, pressed=1):
    d = {"schema": m.GPTK_EVIDENCE_SCHEMA, "kind": kind, "context": context, "context_source": "x",
         "control": control, "event": "press", "pressed": pressed}
    if action:
        d["action"] = action
        d["sink"] = "adapter.input.android-keyevent"
    return json.dumps(d)

receipt = [ev("delivery", "menu", "START", "fp2.pause"), ev("delivery", "menu", "START", "fp2.pause", 0),
           # quiet window: nothing
           ev("delivery", "gameplay", "LEFT_STICK", "fp2.move"),
           ev("suppressed", "gameplay", "A"),
           # L2 null: nothing
           ]
log = ["[x] context=menu source=stage:no-player", "PHASE runtime EXIT status=0"]
# agent records: counters after each step (receipt_lines, log_lines)
def rec(i, op, r, l, **extra):
    before = extra.pop("before", None)
    d = {"index": i, "step": {"op": op}, "before": before if before is not None else {"receipt_lines": r, "log_lines": l},
         "after": {"receipt_lines": r, "log_lines": l}}
    d.update(extra)
    return d
B = lambda r, l: {"receipt_lines": r, "log_lines": l}
records = [rec(0, "wait_log", 0, 1, before=B(0, 0)), rec(1, "press", 2, 1, before=B(0, 1)), rec(2, "mark", 2, 1), rec(3, "sleep", 2, 1, before=B(2, 1)), rec(4, "mark", 2, 1),
           rec(5, "hold_axis", 3, 1, before=B(2, 1)), rec(6, "mark", 3, 1), rec(7, "press", 4, 1, before=B(3, 1)), rec(8, "mark", 4, 1),
           rec(9, "press", 4, 1, before=B(4, 1)), rec(10, "mark", 4, 1), rec(11, "chord", 4, 1, before=B(4, 1)),
           rec(12, "wait_exit", 4, 2, exit_status=0, processes_alive=0, before=B(4, 1)), rec(13, "mark", 4, 2), rec(14, "mark", 4, 2)]
verdicts, ok = m.evaluate(steps, records, receipt, log)
check(ok, "fixture roteiro passes: %s" % [v for v in verdicts if v["result"] != "PASS"])
check(len([v for v in verdicts if v["result"] == "PASS"]) == 7, "seven expectations evaluated")
# neutrality failure: a phantom motion inside the quiet window
receipt_bad = receipt[:2] + [ev("delivery", "menu", "LEFT_STICK", "fp2.move")] + receipt[2:]
records_bad = [dict(r, after=dict(r["after"], receipt_lines=r["after"]["receipt_lines"] + (1 if r["index"] >= 3 else 0))) for r in records]
verdicts, ok = m.evaluate(steps, records_bad, receipt_bad, log)
check(not ok and any("silent" in v["why"] for v in verdicts), "phantom motion in a quiet window fails")
# latch: START delivered again where nothing was pressed -> no_delivery/quiet catches; exit status wrong
records_exit = [dict(r, exit_status=70) if r["step"]["op"] == "wait_exit" else r for r in records]
verdicts, ok = m.evaluate(steps, records_exit, receipt, log)
check(not ok and any("exit status" in v["why"] for v in verdicts), "non-zero exit fails")
# a step the agent never reached
verdicts, ok = m.evaluate(steps, records[:6], receipt, log)
check(not ok and any("never ran" in v["why"] for v in verdicts), "missing step records fail closed")

# press/release once: count with pressed filter
cnt_steps = [{"press": "B"}, {"expect": "count", "kind": "delivery", "control": "B", "pressed": 1, "value": 1},
             {"expect": "count", "kind": "delivery", "control": "B", "pressed": 0, "value": 1}, {"wait_exit": True},
             {"expect": "exit_status", "value": 0}]
cnt_receipt = [ev("delivery", "gameplay", "B", "fp2.jump", 1), ev("delivery", "gameplay", "B", "fp2.jump", 0)]
cnt_records = [rec(0, "press", 2, 0, before=B(0, 0)), rec(1, "mark", 2, 0), rec(2, "mark", 2, 0), rec(3, "wait_exit", 2, 1, exit_status=0, processes_alive=0, before=B(2, 0)), rec(4, "mark", 2, 1)]
verdicts, ok = m.evaluate(cnt_steps, cnt_records, cnt_receipt, ["PHASE runtime EXIT status=0"])
check(ok, "exactly one press and one release pass: %s" % [v for v in verdicts if v["result"] != "PASS"])
cnt_records_3 = [rec(0, "press", 3, 0, before=B(0, 0)), rec(1, "mark", 3, 0), rec(2, "mark", 3, 0), rec(3, "wait_exit", 3, 1, exit_status=0, processes_alive=0, before=B(3, 0)), rec(4, "mark", 3, 1)]
verdicts, ok = m.evaluate(cnt_steps, cnt_records_3, cnt_receipt + [ev("delivery", "gameplay", "B", "fp2.jump", 1)],
                          ["PHASE runtime EXIT status=0"])
check(not ok, "a repeated press (latch/double delivery) fails")
check(m.CLASSIFICATION == "ON_DEVICE_AUTOMATED_INPUT_PROOF", "classification constant")

# 0.11.6: context_change -- the engine's own word, not the adapter's delivery
def ctxev(context, source):
    return json.dumps({"schema": m.GPTK_EVIDENCE_SCHEMA, "kind": "context", "context": context, "source": source, "observed": True})
cc_steps = [{"wait_log": "context=gameplay"}, {"press": "START"}, {"sleep_ms": 3000},
            {"expect": "delivery", "context": "gameplay", "control": "START", "action": "fp2.pause"},
            {"expect": "context_change", "context": "menu", "source_regex": "stage:paused"},
            {"wait_exit": True}, {"expect": "exit_status", "value": 0}]
m.validate_roteiro({"schema": m.ROTEIRO_SCHEMA, "port_id": "fp2", "steps": cc_steps, "classification": m.CLASSIFICATION, "clones": 1})
cc_receipt_ok = [ctxev("gameplay", "stage:player-alive"), ev("delivery", "gameplay", "START", "fp2.pause", 1),
                 ev("delivery", "gameplay", "START", "fp2.pause", 0), ctxev("menu", "stage:paused")]
cc_records = [rec(0, "wait_log", 1, 1, before=B(0, 0)), rec(1, "press", 3, 1, before=B(1, 1)), rec(2, "sleep", 4, 1, before=B(3, 1)),
              rec(3, "mark", 4, 1), rec(4, "mark", 4, 1), rec(5, "wait_exit", 4, 2, exit_status=0, processes_alive=0, before=B(4, 1)), rec(6, "mark", 4, 2)]
verdicts, ok = m.evaluate(cc_steps, cc_records, cc_receipt_ok, ["ctx", "PHASE runtime EXIT status=0"])
check(ok, "delivery followed by the engine proving the paused context passes: %s" % [v for v in verdicts if v["result"] != "PASS"])
# MUTANT: the adapter delivered START but the engine never moved (the getInputName hook of FP2 1.1.4)
cc_receipt_dead = cc_receipt_ok[:3] + [ctxev("gameplay", "stage:player-alive")]
verdicts, ok = m.evaluate(cc_steps, cc_records, cc_receipt_dead, ["ctx", "PHASE runtime EXIT status=0"])
check(not ok and any("delivery without effect" in v["why"] for v in verdicts), "MUTANT killed: delivery counted while the engine stayed in the same context (dead input passes on receipts alone)")
# a re-proof of the SAME context is not a change
cc_receipt_same = cc_receipt_ok[:3] + [ctxev("menu", "stage:no-player")]
cc_records_same = [dict(r) for r in cc_records]
verdicts, ok = m.evaluate(cc_steps, cc_records_same, [ctxev("menu", "stage:no-player")] + cc_receipt_same[1:], ["ctx", "PHASE runtime EXIT status=0"])
check(not ok, "a context receipt identical to the one proven before the window is not a change")
try:
    m.validate_roteiro({"schema": m.ROTEIRO_SCHEMA, "port_id": "fp2", "steps": [{"expect": "context_change"}, {"wait_exit": True}], "classification": m.CLASSIFICATION, "clones": 1})
    check(False, "context_change without a context must be refused")
except m.ProofError:
    check(True, "context_change without a context refused")

# static boundaries
src = TOOL.read_text() + AGENT.read_text()
check(not re.search(r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b", src), "no IP literal in the tool")
check("UI_DEV_CREATE" in AGENT.read_text() and "UI_DEV_CREATE" not in TOOL.read_text(), "only the external agent creates uinput devices")
check("HOST_FIXTURE" in TOOL.read_text() and "ON_DEVICE_AUTOMATED_INPUT_PROOF" in TOOL.read_text(), "classification named and HOST_FIXTURE excluded")
check("NC_VPAD" not in src and "FP2_VPAD" not in src, "tool never uses in-port injection envs")
check("uinput-clone-device-faithful" in src and "born_before_game_sdl_init" in src, "receipt names its injection provider and the pre-SDL birth")
check("evaluate(" in src and "quiet" in src and "process_gone" in src, "verdict kinds present")

if fails:
    print("nxinput device-input-proof gate: FAIL (%d)" % fails)
    sys.exit(1)
print("nxinput device-input-proof gate: PASS — bitmask/SDL numbering, mapping-derived table, roteiro, plan, windowed verdicts, static boundaries")
