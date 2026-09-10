#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxinput 0.10.2 — nx-device-input-proof: ON_DEVICE_AUTOMATED_INPUT_PROOF.

The NextOS framework is automatic: the proof that a port's controls work on
the target hardware is produced by the framework itself, on the real device,
without a person pressing buttons. This tool is that proof.

Model
  1. On the real device, launched with the frontend's true environment
     (nx-device-launch), the tool captures the REAL controller's profile from
     the kernel: identity (bus/vendor/product/version/name), EV_KEY/EV_ABS
     bits, EVIOCGABS ranges, counts — plus the firmware SDL provider and the
     CFW identity.
  2. BEFORE the game's SDL_Init, an external helper (nx-input-inject-agent.py,
     never part of any port or ZIP) creates uinput pads that are device-faithful
     clones of that profile. The firmware SDL enumerates them like hardware,
     gives them the same GUID, applies the same mapping, and the port's C6 seam
     admits them through the same authority order. The receipt proves that
     admission from the port's own log (one `controller:` + `pad slot=` line
     per pad, C6 admission lines).
  3. The control->kernel-code table is derived ONLY from the mapping line the
     port admitted and the node's bitmasks (SDL2 linux numbering). Nothing is
     invented to make a test pass; a control the mapping does not bind is
     refused.
  4. A declarative roteiro (nxgenerator writes it from the port's actions,
     contexts and sinks; the port adds only navigation) is executed on the
     clones with local timing: every control, neutrality, START, SELECT,
     SELECT+START on ONE instance, negatives (L1+R1, L2+R2, cross-pad
     SELECT+START, A in a menu never selecting QUIT), press/release once.
  5. Each stimulus is tied to the whole chain — uinput on the real device ->
     firmware SDL -> C6/readback -> GPTK -> real context -> action -> real
     engine sink — through windowed verdicts on the runtime evidence
     (nxinput-gptk-event-evidence/1) and the log.
  6. The receipt fixes device/CFW, SDL provider, GUID, capabilities, mapping
     hash/readback, GPTK hash, adapter-contract, ELF, generation, run,
     contexts and sinks, and classifies itself ON_DEVICE_AUTOMATED_INPUT_PROOF:
     valid proof of the input software on the target hardware; it does not
     claim to test mechanical wear of a button (hardware QA, outside the gate).

Never: the old per-game in-port virtual pads; injection after SDL; a private
SDL; evdev-to-mapping logic inside the port; HOST_FIXTURE presented as
on-device; a human witness as a release requirement.

Usage:
  nx-device-input-proof.py --host IP --user USER --launcher "/roms/ports/X.sh"
      --roteiro roteiro.json --out DIR [--owner-gptk FILE] [--sudo]
      [--profile-cache DIR] [--controller-name NAME] [--label TEXT]
"""
import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path

VERSION = "0.10.2"
CLASSIFICATION = "ON_DEVICE_AUTOMATED_INPUT_PROOF"
RECEIPT_SCHEMA = "nx-device-input-proof/1"
ROTEIRO_SCHEMA = "nx-device-input-proof-roteiro/1"
PLAN_SCHEMA = "nx-device-input-proof-plan/1"
PROFILE_SCHEMA = "nx-device-input-proof-profile/1"
GPTK_EVIDENCE_SCHEMA = "nxinput-gptk-event-evidence/1"

HERE = Path(__file__).resolve().parent
AGENT = HERE / "nx-input-inject-agent.py"
DEVICE_LAUNCH = HERE.parent.parent / "nxobs" / "nx-device-launch.sh"

BTN_JOYSTICK, KEY_MAX = 0x120, 0x2FF
ABS_HAT0X, ABS_HAT3Y = 0x10, 0x17

CONTROL_TO_SDL = {
    "A": "a", "B": "b", "X": "x", "Y": "y",
    "L1": "leftshoulder", "R1": "rightshoulder",
    "L2": "lefttrigger", "R2": "righttrigger",
    "L3": "leftstick", "R3": "rightstick",
    "START": "start", "SELECT": "back", "GUIDE": "guide",
    "UP": "dpup", "DOWN": "dpdown", "LEFT": "dpleft", "RIGHT": "dpright",
}
STICK_TO_SDL = {"LEFT_STICK": ("leftx", "lefty"), "RIGHT_STICK": ("rightx", "righty")}
EXPECT_KINDS = {"delivery", "suppressed", "quiet", "no_delivery", "log", "no_log",
                "exit_status", "process_gone", "count", "context_change"}
OPS = {"wait_log", "press", "hold", "chord", "chord_cross", "sleep_ms", "expect", "mark", "wait_exit", "unplug", "replug"}


class ProofError(Exception):
    pass


# ---------------------------------------------------------------------------
# Pure logic (host gate: tests/test_device_input_proof.py)
# ---------------------------------------------------------------------------
def parse_bitmask(text):
    words = [int(w, 16) for w in text.split()]
    bits = []
    for i, word in enumerate(reversed(words)):
        for b in range(64):
            if word >> b & 1:
                bits.append(i * 64 + b)
    return sorted(bits)


def parse_proc_input_devices(text):
    devices, cur = [], None
    for line in text.splitlines():
        if line.startswith("I:"):
            cur = {"name": "", "handlers": [], "event_node": None, "key_codes": [], "abs_codes": []}
            for k, v in re.findall(r"(\w+)=([0-9a-fA-F]+)", line):
                cur[k.lower()] = v.lower()
            devices.append(cur)
        elif cur is None:
            continue
        elif line.startswith("N:"):
            m = re.search(r'Name="(.*)"', line)
            cur["name"] = m.group(1) if m else ""
        elif line.startswith("H:"):
            cur["handlers"] = line.split("=", 1)[1].split()
            for h in cur["handlers"]:
                if re.fullmatch(r"event\d+", h):
                    cur["event_node"] = "/dev/input/" + h
        elif line.startswith("B: KEY="):
            cur["key_codes"] = parse_bitmask(line.split("=", 1)[1])
        elif line.startswith("B: ABS="):
            cur["abs_codes"] = parse_bitmask(line.split("=", 1)[1])
    return devices


def pick_real_controller(devices, wanted_name=None):
    """The physical pad: a device with a joystick handler (js*) and gamepad
    keys. With more than one, the caller must name it — never guessed."""
    cands = [d for d in devices if any(h.startswith("js") for h in d["handlers"])
             and any(k >= BTN_JOYSTICK for k in d["key_codes"]) and d["event_node"]]
    if wanted_name:
        cands = [d for d in cands if d["name"] == wanted_name]
    if len(cands) != 1:
        raise ProofError("expected exactly one real controller%s, found %d: %s" % (
            " named %r" % wanted_name if wanted_name else "", len(cands), [d["name"] for d in cands]))
    return cands[0]


def sdl_button_index_map(key_codes):
    codes = set(key_codes)
    ordered = [c for c in range(BTN_JOYSTICK, KEY_MAX + 1) if c in codes]
    ordered += [c for c in range(0, BTN_JOYSTICK) if c in codes]
    return {i: c for i, c in enumerate(ordered)}


def sdl_axis_index_map(abs_codes):
    axes, hats = {}, {}
    for c in sorted(abs_codes):
        if ABS_HAT0X <= c <= ABS_HAT3Y:
            hat = (c - ABS_HAT0X) // 2
            hats.setdefault(hat, {})["x" if (c - ABS_HAT0X) % 2 == 0 else "y"] = c
        else:
            axes[len(axes)] = c
    return axes, hats


def parse_mapping_line(line):
    parts = line.strip().rstrip(",").split(",")
    if len(parts) < 3:
        raise ProofError("mapping line too short: %r" % line)
    fields = {}
    for p in parts[2:]:
        if ":" in p:
            k, v = p.split(":", 1)
            fields[k.strip()] = v.strip()
    return parts[0], parts[1], fields


def resolve_target(target, button_map, axis_map, hat_map):
    m = re.fullmatch(r"([+-]?)a(\d+)(~?)", target)
    if m:
        idx = int(m.group(2))
        if idx not in axis_map:
            raise ProofError("mapping uses axis a%d absent from the kernel node" % idx)
        return {"type": "axis", "code": axis_map[idx], "sign": -1.0 if m.group(1) == "-" else 1.0,
                "inverted": m.group(3) == "~"}
    m = re.fullmatch(r"b(\d+)", target)
    if m:
        idx = int(m.group(1))
        if idx not in button_map:
            raise ProofError("mapping uses button b%d absent from the kernel node" % idx)
        return {"type": "key", "code": button_map[idx]}
    m = re.fullmatch(r"h(\d+)\.(\d+)", target)
    if m:
        hat, mask = int(m.group(1)), int(m.group(2))
        if hat not in hat_map:
            raise ProofError("mapping uses hat h%d absent from the kernel node" % hat)
        axis = "y" if mask in (1, 4) else "x"
        return {"type": "hat", "code": hat_map[hat][axis], "value": -1.0 if mask in (1, 8) else 1.0}
    raise ProofError("unsupported mapping target %r" % target)


# ---------------------------------------------------------------- V5 oracle
# Mission 3.2/8.1 (nxinput 0.11.0): the V4 tool derived the EV_KEY to inject
# from the mapping the PORT had already normalized and from a hard-coded
# high-first order -- a circular oracle. In V5 mode the stimulus table comes
# from two sources INDEPENDENT of the port process: (1) the CFW's own mapping
# line for the pad's GUID, read from the firmware/PortMaster database files
# on the device (never from the port log), and (2) the ordinal table of the
# provider DSO the device ships, looked up by its sha256 in the pinned
# provider manifest (measured tables; an UNDECLARED provider refuses to guess).
def sdl_guid_from_profile(profile):
    """SDL2 Linux joystick GUID: bustype, 0, vendor, 0, product, 0, version, 0 (u16 LE each)."""
    def le16(v): return "%02x%02x" % (v & 0xff, (v >> 8) & 0xff)
    return le16(profile["bustype"]) + "0000" + le16(profile["vendor"]) + "0000" + le16(profile["product"]) + "0000" + le16(profile["version"]) + "0000"


def v5_provider_for_sha(manifest_path, sha):
    man = json.load(open(manifest_path))
    for row in man.get("providers", []):
        if row.get("sha256") == sha:
            if row.get("domain") in (None, "", "undeclared"):
                raise ProofError("provider %s is pinned UNDECLARED (%s): refusing to guess its ordinal table" % (sha[:12], row.get("id")))
            return row["id"], row["domain"]
    raise ProofError("provider sha256 %s is not pinned in %s: measure it first (nx-provider-ordinal-probe.py)" % (sha[:12], manifest_path))


def v5_fetch_cfw_mapping(dev, guid, extra_paths, sdl_path=None, builtin_db=None):
    """The CFW-authored line for this GUID, from database files on the device."""
    candidates = list(extra_paths or []) + [
        "/usr/lib/gamecontrollerdb.txt", "/usr/lib32/gamecontrollerdb.txt",
        "/usr/share/sdl2/gamecontrollerdb.txt", "/roms/tools/PortMaster/gamecontrollerdb.txt",
        "/roms2/tools/PortMaster/gamecontrollerdb.txt", "/storage/roms/ports/PortMaster/gamecontrollerdb.txt",
        "/storage/roms/tools/PortMaster/gamecontrollerdb.txt", "/opt/muos/device/control/gamecontrollerdb.txt",
        "/usr/lib/gamecontrollerdb-nextos.txt", "/storage/.config/gamecontrollerdb.txt",
    ]
    for path in candidates:
        # every line of this GUID; the Linux platform line (or a line without a
        # platform field) is the one the Linux provider consumes -- a Windows/macOS
        # line for the same VID:PID has other ordinals (corpus rule: platform is
        # filtered BEFORE election).
        lines = dev.ssh("grep '^%s,' %s 2>/dev/null || true" % (guid, shlex.quote(path)), check=False).stdout.splitlines()
        for line in lines:
            line = line.strip()
            m = re.search(r"platform:([^,]+)", line)
            if line and (m is None or m.group(1).strip().lower() == "linux"):
                return line, path
    env_line = dev.ssh("printf '%s' \"${SDL_GAMECONTROLLERCONFIG:-}\" | grep -m1 '^%s,' || true" % guid, check=False).stdout.strip()
    if env_line:
        return env_line, "SDL_GAMECONTROLLERCONFIG (login environment)"
    # Authority 4: the provider's own built-in database, read from the BYTES of
    # the mapped DSO (never from the port). SDL >= 2.26 keys its GUID with a
    # name CRC in bytes 2-3 and the version word; the built-in lines carry
    # zeros there, so match the CRC-less form.
    if sdl_path:
        zero = guid[:4] + "0000" + guid[8:]
        lines = dev.ssh("strings -n 40 %s 2>/dev/null | grep '^%s,' || true" % (shlex.quote(sdl_path), zero), check=False).stdout.splitlines()
        for line in lines:
            line = line.strip()
            m = re.search(r"platform:([^,]+)", line)
            if line and (m is None or m.group(1).strip().lower() == "linux"):
                return line, "built-in database of the mapped provider DSO (%s)" % os.path.basename(sdl_path)
    if builtin_db:
        zero = guid[:4] + "0000" + guid[8:]
        for line in open(builtin_db, encoding="utf-8", errors="replace"):
            line = line.strip()
            if line.startswith(zero + ","):
                m = re.search(r"platform:([^,]+)", line)
                if m is None or m.group(1).strip().lower() == "linux":
                    return line, "pinned upstream built-in database %s" % os.path.basename(builtin_db)
    raise ProofError("no CFW-authored mapping for GUID %s in any database file on the device nor in the provider's built-in database; V5 refuses the port's own line" % guid)


def build_control_table_v5(mapping_fields, key_codes, abs_codes, domain):
    sys.path.insert(0, str(HERE))
    import nxoracle_v5 as ox
    bmap = ox.button_table(domain, key_codes)
    amap, hmap_axes = ox.axis_table(domain, abs_codes)
    _, hmap = sdl_axis_index_map(abs_codes)
    table = {}
    for control, field in CONTROL_TO_SDL.items():
        if field in mapping_fields:
            table[control] = resolve_target(mapping_fields[field], bmap, amap, hmap)
    for stick, (fx, fy) in STICK_TO_SDL.items():
        if fx in mapping_fields and fy in mapping_fields:
            table[stick] = {"type": "stick", "x": resolve_target(mapping_fields[fx], bmap, amap, hmap),
                            "y": resolve_target(mapping_fields[fy], bmap, amap, hmap)}
    return table, {"button_index_to_code": bmap, "axis_index_to_code": amap, "hats": hmap, "domain": domain}


def build_control_table(mapping_fields, key_codes, abs_codes):
    bmap = sdl_button_index_map(key_codes)
    amap, hmap = sdl_axis_index_map(abs_codes)
    table = {}
    for control, field in CONTROL_TO_SDL.items():
        if field in mapping_fields:
            table[control] = resolve_target(mapping_fields[field], bmap, amap, hmap)
    for stick, (fx, fy) in STICK_TO_SDL.items():
        if fx in mapping_fields and fy in mapping_fields:
            table[stick] = {"type": "stick", "x": resolve_target(mapping_fields[fx], bmap, amap, hmap),
                            "y": resolve_target(mapping_fields[fy], bmap, amap, hmap)}
    return table, {"button_index_to_code": bmap, "axis_index_to_code": amap, "hats": hmap}


def validate_roteiro(roteiro):
    if roteiro.get("schema") != ROTEIRO_SCHEMA:
        raise ProofError("roteiro schema must be %s" % ROTEIRO_SCHEMA)
    if not re.fullmatch(r"[a-z0-9_-]+", roteiro.get("port_id", "")):
        raise ProofError("roteiro.port_id missing or invalid")
    steps = roteiro.get("steps")
    if not isinstance(steps, list) or not steps:
        raise ProofError("roteiro.steps must be a non-empty list")
    saw_exit = False
    for i, s in enumerate(steps):
        keys = set(s) & OPS
        if len(keys) != 1:
            raise ProofError("step %d must name exactly one operation" % i)
        (op,) = keys
        if op == "expect" and s["expect"] not in EXPECT_KINDS:
            raise ProofError("step %d: unknown expect kind %r" % (i, s["expect"]))
        if op == "expect" and s["expect"] in ("log", "no_log") and "regex" not in s:
            raise ProofError("step %d: expect %s needs regex" % (i, s["expect"]))
        if op == "expect" and s["expect"] in ("count", "exit_status") and "value" not in s:
            raise ProofError("step %d: expect %s needs value" % (i, s["expect"]))
        if op == "expect" and s["expect"] == "context_change" and not isinstance(s.get("context"), str):
            raise ProofError("step %d: expect context_change needs the context the engine must reach" % i)
        if op == "wait_log" and not isinstance(s["wait_log"], str):
            raise ProofError("step %d: wait_log must be a regex string" % i)
        if op in ("chord", "chord_cross") and (not isinstance(s[op], list) or len(s[op]) < 2):
            raise ProofError("step %d: %s needs at least two controls" % (i, op))
        if op in ("unplug", "replug") and (type(s[op]) is not int or s[op] < 1):
            raise ProofError("step %d: %s needs a clone index >= 1 (never the primary stimulus pad)" % (i, op))
        if op == "wait_exit":
            saw_exit = True
    if not saw_exit:
        raise ProofError("roteiro must end the game itself (wait_exit) — a run killed from outside proves nothing")
    return steps


def clones_needed(roteiro):
    n = int(roteiro.get("clones", 1))
    if any("chord_cross" in s or "unplug" in s or "replug" in s for s in roteiro["steps"]):
        n = max(n, 2)
    return max(1, min(n, 3))


def compile_plan(roteiro, table, log_path, receipt_path, port_dir, profile=None):
    def spec_for(control):
        spec = table.get(control)
        if spec is None:
            raise ProofError("control %s is not bound by the admitted mapping; refusing to guess" % control)
        return spec

    def press_step(control, ms, pad):
        spec = spec_for(control)
        if spec["type"] == "key":
            return {"op": "press", "codes": [spec["code"]], "ms": ms, "control": control, "pad": pad}
        if spec["type"] == "axis":
            return {"op": "hold_axis", "axes": [[spec["code"], spec["sign"] * (-1.0 if spec["inverted"] else 1.0)]],
                    "ms": ms, "control": control, "pad": pad}
        if spec["type"] == "hat":
            return {"op": "hold_axis", "axes": [[spec["code"], spec["value"]]], "ms": ms, "control": control, "pad": pad}
        raise ProofError("control %s is a stick on this device; use hold with x/y" % control)

    def key_of(control):
        spec = spec_for(control)
        if spec["type"] != "key":
            raise ProofError("chord control %s is not a key on this device" % control)
        return spec["code"]

    steps = []
    for s in roteiro["steps"]:
        pad = int(s.get("pad", 0))
        if "press" in s:
            steps.append(press_step(s["press"], int(s.get("ms", 120)), pad))
        elif "chord" in s:
            steps.append({"op": "chord", "keys": [[pad, key_of(c)] for c in s["chord"]],
                          "ms": int(s.get("ms", 250)), "controls": s["chord"]})
        elif "chord_cross" in s:
            # control i goes to clone i: SELECT on pad 0, START on pad 1.
            steps.append({"op": "chord", "keys": [[i, key_of(c)] for i, c in enumerate(s["chord_cross"])],
                          "ms": int(s.get("ms", 250)), "controls": s["chord_cross"], "cross_pad": True})
        elif "hold" in s:
            spec = table.get(s["hold"])
            if spec is None or spec["type"] != "stick":
                raise ProofError("hold needs a stick control bound by the mapping: %r" % s.get("hold"))
            axes = []
            for axis_name, unit in (("x", float(s.get("x", 0.0))), ("y", float(s.get("y", 0.0)))):
                a = spec[axis_name]
                if a["type"] != "axis":
                    raise ProofError("stick %s/%s is not an axis on this device" % (s["hold"], axis_name))
                axes.append([a["code"], unit * a["sign"] * (-1.0 if a["inverted"] else 1.0)])
            steps.append({"op": "hold_axis", "axes": axes, "ms": int(s.get("ms", 500)), "control": s["hold"], "pad": pad})
        elif "sleep_ms" in s:
            steps.append({"op": "sleep", "ms": int(s["sleep_ms"])})
        elif "unplug" in s:
            steps.append({"op": "unplug", "pad": int(s["unplug"])})
        elif "replug" in s:
            steps.append({"op": "replug", "pad": int(s["replug"])})
        elif "wait_log" in s:
            steps.append({"op": "wait_log", "regex": s["wait_log"], "timeout_s": int(s.get("timeout_s", 120)),
                          "abort_regex": s.get("abort_regex", r"FATAL|launcher-error|runtime EXIT status=[1-9]"),
                          "required": bool(s.get("required", True))})
        elif "wait_exit" in s:
            steps.append({"op": "wait_exit", "timeout_s": int(s.get("timeout_s", 60))})
        else:
            steps.append({"op": "mark", "expect": s.get("expect"), "spec": s})
    plan = {"schema": PLAN_SCHEMA, "log_path": log_path, "receipt_path": receipt_path,
            "port_dir": port_dir, "clones": clones_needed(roteiro), "steps": steps}
    if profile is not None:
        plan["profile"] = profile  # lets the agent recreate a clone after an unplug step
    return plan


def parse_receipt(lines):
    out = []
    for line in lines:
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        if d.get("schema") == GPTK_EVIDENCE_SCHEMA:
            out.append(d)
    return out


def evaluate(roteiro_steps, step_records, receipt_lines, log_lines):
    receipts = []
    for l in receipt_lines:
        try:
            receipts.append(json.loads(l) if l.strip().startswith("{") else None)
        except json.JSONDecodeError:
            receipts.append(None)
    verdicts = []
    # Window semantics: the evidence a stimulus group produced. A group starts
    # at the first stimulus step after an expect/mark and ends at the expect
    # that reads it; consecutive expects read the same window, a `mark` closes
    # a group without judging it (so `mark, sleep, expect quiet` is a pure
    # neutrality window).
    win_from = {"receipt_lines": 0, "log_lines": 0}
    in_group = False
    by_index = {r["index"]: r for r in step_records if "index" in r}
    exit_status, procs_alive = None, None
    for r in step_records:
        if r.get("step", {}).get("op") == "wait_exit":
            exit_status, procs_alive = r.get("exit_status"), r.get("processes_alive")

    def match(d, **want):
        return all(d.get(k) == v for k, v in want.items())

    for i, s in enumerate(roteiro_steps):
        rec = by_index.get(i)
        if rec is None:
            verdicts.append({"step": i, "spec": s, "result": "FAIL", "why": "step never ran (agent stopped early)"})
            continue
        if "expect" not in s and "mark" not in s:
            if not in_group:
                win_from = dict(rec["before"])
                in_group = True
            continue
        end = rec["after"]
        win_r = [receipts[k] for k in range(win_from["receipt_lines"], min(end["receipt_lines"], len(receipts)))
                 if receipts[k] and receipts[k].get("schema") == GPTK_EVIDENCE_SCHEMA]
        win_l = log_lines[win_from["log_lines"]:end["log_lines"]]
        in_group = False
        if "mark" in s:
            win_from = dict(end)
            continue
        kind, result, why = s["expect"], "PASS", ""
        if kind == "delivery":
            want = {k: s[k] for k in ("context", "control", "action", "sink") if k in s}
            n = sum(1 for d in win_r if d.get("kind") == "delivery" and d.get("pressed", 1) != 0 and match(d, **want))
            if n < int(s.get("min", 1)):
                result, why = "FAIL", "delivery %s seen %d time(s), wanted >= %d" % (want, n, int(s.get("min", 1)))
        elif kind == "suppressed":
            want = {k: s[k] for k in ("context", "control") if k in s}
            n = sum(1 for d in win_r if d.get("kind") == "suppressed" and match(d, **want))
            if n < int(s.get("min", 1)):
                result, why = "FAIL", "suppression %s seen %d time(s)" % (want, n)
        elif kind == "quiet":
            noisy = [d for d in win_r if d.get("kind") in ("delivery", "suppressed")]
            if noisy:
                result, why = "FAIL", "%d input event(s) in a window that had to be silent: %s" % (
                    len(noisy), sorted({str(d.get("control")) for d in noisy}))
        elif kind == "no_delivery":
            want = {k: s[k] for k in ("context", "control", "action") if k in s}
            n = sum(1 for d in win_r if d.get("kind") == "delivery" and d.get("pressed", 1) != 0 and match(d, **want))
            if n:
                result, why = "FAIL", "unexpected delivery %s x%d" % (want, n)
        elif kind == "count":
            want = {k: s[k] for k in ("context", "control", "action", "kind") if k in s}
            pressed = s.get("pressed")
            n = sum(1 for d in win_r if match(d, **want) and (pressed is None or d.get("pressed", 1) == pressed))
            if n != int(s["value"]):
                result, why = "FAIL", "%s%s counted %d, wanted %d" % (want, "" if pressed is None else " pressed=%s" % pressed, n, int(s["value"]))
        elif kind == "log":
            pat = re.compile(s["regex"])
            if not any(pat.search(l) for l in win_l):
                result, why = "FAIL", "no log line matched %r in the window" % s["regex"]
        elif kind == "no_log":
            pat = re.compile(s["regex"])
            hits = [l.strip() for l in win_l if pat.search(l)]
            if hits:
                result, why = "FAIL", "forbidden log line: %s" % hits[0][:160]
        elif kind == "exit_status":
            if exit_status != int(s["value"]):
                result, why = "FAIL", "exit status %r, wanted %d" % (exit_status, int(s["value"]))
        elif kind == "process_gone":
            if procs_alive != 0:
                result, why = "FAIL", "%r process(es) still alive under the port directory" % procs_alive
        elif kind == "context_change":
            # 0.11.6 (FP2 03/09): a delivery is the ADAPTER's word; the engine
            # may still ignore it (a hook broke Input.GetButton and every
            # "delivery" kept passing). The only receipt the adapter writes
            # FROM the engine is the context proof (kind=context, source =
            # the engine state the adapter read). This expectation requires
            # the window to contain a context receipt naming the context the
            # stimulus must produce, with a (context, source) different from
            # the last one proven before the window -- the engine moved.
            before = [receipts[k] for k in range(0, min(win_from["receipt_lines"], len(receipts)))
                      if receipts[k] and receipts[k].get("schema") == GPTK_EVIDENCE_SCHEMA and receipts[k].get("kind") == "context"]
            last = (before[-1].get("context"), before[-1].get("source")) if before else None
            pat = re.compile(s["source_regex"]) if s.get("source_regex") else None
            moved = [d for d in win_r if d.get("kind") == "context" and d.get("context") == s["context"]
                     and (pat is None or pat.search(str(d.get("source", ""))))
                     and (d.get("context"), d.get("source")) != last]
            if not moved:
                result, why = "FAIL", "the engine never proved context %r%s after the stimulus (last proven before the window: %r) -- delivery without effect" % (
                    s["context"], " matching %r" % s["source_regex"] if s.get("source_regex") else "", last)
        verdicts.append({"step": i, "spec": s, "result": result, "why": why,
                         "window": {"receipt_lines": len(win_r), "log_lines": len(win_l)}})
    for r in step_records:
        if r.get("step", {}).get("op") == "wait_log" and r.get("timed_out"):
            verdicts.append({"step": r["index"], "spec": r["step"], "result": "FAIL",
                             "why": "wait_log timed out (%s)" % r.get("note", "")})
    all_pass = bool(verdicts) and all(v["result"] == "PASS" for v in verdicts)
    return verdicts, all_pass


def profile_key(profile, cfw, sdl):
    """Reuse key: the device + firmware + SDL identity. A mapping change or a
    different SDL binary makes a new profile."""
    body = {k: profile.get(k) for k in ("name", "bustype", "vendor", "product", "version", "key_codes", "abs")}
    body["cfw"] = cfw
    body["sdl"] = sdl
    return hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Device orchestration
# ---------------------------------------------------------------------------
class Device:
    def __init__(self, host, user, sudo):
        if not re.fullmatch(r"[0-9.]+", host):
            raise ProofError("refusing a non-literal host: pass the current device IP")
        self.host, self.user, self.sudo = host, user, "sudo -n" if sudo else ""
        self.opts = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                     "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=8"]

    def ssh(self, cmd, check=True, timeout=120):
        # -n: no stdin, so a detached remote process can never hold the session open.
        p = subprocess.run(["ssh", "-n"] + self.opts + ["%s@%s" % (self.user, self.host), cmd],
                           capture_output=True, text=True, timeout=timeout)
        if check and p.returncode != 0:
            raise ProofError("ssh failed (%d): %s\n%s" % (p.returncode, cmd[:120], p.stderr.strip()[-400:]))
        return p

    def put(self, local, remote):
        subprocess.run(["scp", "-q"] + self.opts + [str(local), "%s@%s:%s" % (self.user, self.host, remote)],
                       check=True, timeout=120)

    def get(self, remote, local):
        subprocess.run(["scp", "-q"] + self.opts + ["%s@%s:%s" % (self.user, self.host, remote), str(local)],
                       check=False, timeout=300)


def kill_under(dev, gamedir):
    dev.ssh("%s ls -l /proc/[0-9]*/exe 2>/dev/null | grep %s/ | sed -E 's#.*/proc/([0-9]+)/exe.*#\\1#' | "
            "while read -r pid; do %s kill -TERM \"$pid\"; done" % (dev.sudo, shlex.quote(gamedir), dev.sudo), check=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--user", required=True)
    ap.add_argument("--launcher", required=True)
    ap.add_argument("--roteiro", required=True)
    ap.add_argument("--out", required=True, help="local output directory (must not exist)")
    ap.add_argument("--owner-gptk", help="local NEXTOSCONTROLLERS.gptk installed as the owner copy for this run")
    ap.add_argument("--sudo", action="store_true")
    ap.add_argument("--profile-cache", help="directory of reusable device profiles (keyed by device+CFW+SDL hash)")
    ap.add_argument("--controller-name", help="kernel name of the real pad when more than one is present")
    ap.add_argument("--frontend-stop")
    ap.add_argument("--frontend-start")
    ap.add_argument("--max-seconds", type=int, default=1500)
    ap.add_argument("--mapping-timeout", type=int, default=900)
    ap.add_argument("--label", default="")
    ap.add_argument("--v5-provider-manifest", help="V5 oracle: pinned provider manifest (tests/providers/provider-manifest-v5.json); the stimulus table then comes from the CFW database line + the pinned provider table, never from the port log")
    ap.add_argument("--cfw-db", action="append", default=[], help="extra CFW mapping database path(s) on the device to consult first (V5 mode)")
    ap.add_argument("--builtin-db", help="V5 mode last resort: pinned upstream built-in database (Linux lines) for a provider whose DSO keeps its database out of plain strings; must be the release the DSO derives from")
    ap.add_argument("--gamedir", help="game directory on the device when it is not <launcher dir>/<port-id> (e.g. firmwares that keep launchers in ports_scripts/)")
    a = ap.parse_args()

    out = Path(a.out)
    if out.exists():
        raise ProofError("--out must not exist: %s" % out)
    roteiro = json.load(open(a.roteiro))
    steps = validate_roteiro(roteiro)
    n_clones = clones_needed(roteiro)
    dev = Device(a.host, a.user, a.sudo)

    port_root = os.path.dirname(a.launcher)
    launcher_text = dev.ssh("cat %s" % shlex.quote(a.launcher)).stdout
    m = re.search(r'^(?:NXBOOTSTRAP_LOGICAL_GAMEDIR|GAMEDIR)="/\$directory/ports/([A-Za-z0-9._-]+)"', launcher_text, re.M)
    if not m:
        raise ProofError("cannot read the game directory from the launcher; is it an nxgenerator launcher?")
    gamedir = a.gamedir.rstrip("/") if a.gamedir else "%s/%s" % (port_root, m.group(1))
    if a.gamedir and os.path.basename(gamedir) != m.group(1):
        raise ProofError("--gamedir %s does not name the launcher's port directory %s" % (gamedir, m.group(1)))
    log_path, receipt_path = gamedir + "/log.txt", gamedir + "/nxgptk-receipt.jsonl"

    # Preconditions, fail closed.
    live = dev.ssh("%s ls -l /proc/[0-9]*/exe 2>/dev/null | grep -c %s/ || true" % (dev.sudo, shlex.quote(gamedir))).stdout.strip()
    if live not in ("", "0"):
        raise ProofError("something is already running under %s" % gamedir)
    pre = dev.ssh("command -v python3 >/dev/null && echo py=ok || echo py=missing; test -w /dev/uinput && echo uinput=ok || echo uinput=missing").stdout
    if "py=ok" not in pre or "uinput=ok" not in pre:
        raise ProofError("device lacks python3 or a writable /dev/uinput: %s" % pre.strip())

    proc_devices = dev.ssh("cat /proc/bus/input/devices").stdout
    real = pick_real_controller(parse_proc_input_devices(proc_devices), a.controller_name)
    cfw = dev.ssh("cat /etc/os-release 2>/dev/null | grep -E '^(ID|PRETTY_NAME|VERSION_ID|VERSION)=' | tr '\\n' ';'", check=False).stdout.strip()
    sdl_path = dev.ssh("ldconfig -p 2>/dev/null | grep -m1 -oE '/[^ ]*libSDL2-2\\.0\\.so\\.0[^ ]*' || true", check=False).stdout.strip()
    if not sdl_path:
        # devices without ldconfig (dArkOS/ArkOS, EmuELEC): the canonical system paths
        sdl_path = dev.ssh("for p in /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0 /usr/lib/libSDL2-2.0.so.0 /usr/lib64/libSDL2-2.0.so.0 /lib/aarch64-linux-gnu/libSDL2-2.0.so.0; do [ -e \"$p\" ] && { echo \"$p\"; break; }; done", check=False).stdout.strip()
    sdl = {"soname": "libSDL2-2.0.so.0", "path": sdl_path or None,
           "sha256": dev.ssh("sha256sum %s | cut -c1-64" % shlex.quote(sdl_path), check=False).stdout.strip() if sdl_path else None,
           "realpath": dev.ssh("readlink -f %s" % shlex.quote(sdl_path), check=False).stdout.strip() if sdl_path else None}

    out.mkdir(parents=True)
    (out / "proc-bus-input-devices.txt").write_text(proc_devices)
    remote_dir = "/tmp/nx-input-proof-%d" % os.getpid()
    dev.ssh("umask 077 && mkdir %s" % remote_dir)
    dev.put(AGENT, remote_dir + "/agent.py")

    # Profile of the REAL pad (kernel truth), reusable while device+CFW+SDL hashes hold.
    prof_json = dev.ssh("python3 %s/agent.py profile %s" % (remote_dir, real["event_node"])).stdout
    profile = json.loads(prof_json)
    if profile.get("schema") != PROFILE_SCHEMA:
        raise ProofError("agent returned an unexpected profile")
    pkey = profile_key(profile, cfw, sdl)
    profile_reused = False
    if a.profile_cache:
        cache = Path(a.profile_cache)
        cache.mkdir(parents=True, exist_ok=True)
        cached = cache / ("%s.json" % pkey)
        if cached.exists():
            profile_reused = True
        else:
            cached.write_text(json.dumps({"key": pkey, "cfw": cfw, "sdl": sdl, "profile": profile}, indent=2) + "\n")
    (out / "device-profile.json").write_text(json.dumps({"key": pkey, "reused": profile_reused, "cfw": cfw, "sdl": sdl, "profile": profile}, indent=2) + "\n")

    # A previous run's log/readback would satisfy every wait immediately and
    # poison the windows: set them aside (kept in --out) before launching.
    for rel, keep in (("log.txt", "previous-log.txt"), ("nxgptk-receipt.jsonl", "previous-nxgptk-receipt.jsonl")):
        remote = "%s/%s" % (gamedir, rel)
        if dev.ssh("test -f %s && echo yes || echo no" % shlex.quote(remote), check=False).stdout.strip() == "yes":
            dev.get(remote, out / keep)
            dev.ssh("rm -f %s" % shlex.quote(remote), check=False)

    owner_installed = False
    launcher = None
    agent_rc = None
    try:
        if a.owner_gptk:
            dev.put(a.owner_gptk, gamedir + "/NEXTOSCONTROLLERS.gptk")
            owner_installed = True
        # Clones are born BEFORE the game's SDL_Init.
        dev.ssh("printf '%%s' '%s' > %s/profile.json" % (prof_json.replace("'", "'\\''"), remote_dir))
        # Detached: no stdin, all output to a file, so ssh returns at once.
        # One backgrounded command with every descriptor redirected: an
        # `A && B &` list would background a subshell that still holds the
        # session's stdout and keep ssh waiting for the agent to finish.
        dev.ssh("nohup sh -c 'cd %s && echo $$ > agent.pid && exec python3 agent.py run profile.json %s %d' "
                "</dev/null >%s/agent.out 2>&1 &" % (remote_dir, remote_dir, n_clones, remote_dir), timeout=30)
        deadline = time.time() + 30
        while time.time() < deadline:
            if dev.ssh("test -f %s/clones.json && echo yes || echo no" % remote_dir).stdout.strip() == "yes":
                break
            time.sleep(1)
        else:
            raise ProofError("the agent did not create the uinput clones in time")
        clones_info = json.loads(dev.ssh("cat %s/clones.json" % remote_dir).stdout)

        launch_cmd = [str(DEVICE_LAUNCH), "--host", a.host, "--user", a.user, "--launcher", a.launcher,
                      "--seconds", str(a.max_seconds)]
        if a.frontend_stop and a.frontend_start:
            launch_cmd += ["--frontend-stop", a.frontend_stop, "--frontend-start", a.frontend_start]
        launcher = subprocess.Popen(launch_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        # Wait until the port admitted the real pad AND every clone (same GUID, same mapping).
        wanted_pads = 1 + n_clones
        mapping_line = None
        deadline = time.time() + a.mapping_timeout
        while time.time() < deadline:
            p = dev.ssh("grep -E 'controller: .* mapping=|pad slot=' %s 2>/dev/null || true" % shlex.quote(log_path), check=False)
            lines = p.stdout.splitlines()
            ml = [l for l in lines if " mapping=" in l]
            slots = [l for l in lines if "pad slot=" in l]
            if ml and len(slots) >= wanted_pads:
                mapping_line = ml
                break
            if launcher.poll() is not None:
                raise ProofError("the launcher ended before the port admitted the pads")
            time.sleep(2)
        if mapping_line is None:
            raise ProofError("the port did not admit %d pads (real + %d clone(s)) in time" % (wanted_pads, n_clones))
        # The mapping line carries the pad NAME with spaces: take the rest of the line.
        parsed = [re.search(r"controller: (.+?) \(([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\) mapping=(\S.*?)\s*$", l) for l in mapping_line]
        parsed = [q for q in parsed if q]
        guids = {parse_mapping_line(q.group(4))[0] for q in parsed}
        if len(guids) != 1:
            raise ProofError("the admitted pads do not share one GUID/mapping: %s" % sorted(guids))
        name, vendor, product, line = parsed[0].group(1), parsed[0].group(2).lower(), parsed[0].group(3).lower(), parsed[0].group(4)
        port_guid, port_map_name, port_fields = parse_mapping_line(line)
        oracle = {"mode": "v4-port-line", "independent": False}
        if a.v5_provider_manifest:
            # V5: the port's line is evidence only; stimulus from independent sources.
            if not sdl.get("sha256"):
                raise ProofError("V5 oracle needs the sha256 of the device SDL")
            pin_id, domain = v5_provider_for_sha(a.v5_provider_manifest, sdl["sha256"])
            guid = sdl_guid_from_profile(profile)
            cfw_line, cfw_source = v5_fetch_cfw_mapping(dev, guid, a.cfw_db, sdl.get("realpath") or sdl.get("path"), a.builtin_db)
            guid2, map_name, fields = parse_mapping_line(cfw_line)
            if guid2 != guid and guid2 != guid[:4] + "0000" + guid[8:]:
                raise ProofError("CFW line GUID differs from the pad GUID")
            table, index_maps = build_control_table_v5(fields, profile["key_codes"], [int(c) for c in profile["abs"]], domain)
            line = cfw_line
            oracle = {"mode": "v5-independent", "independent": True, "provider_pin": pin_id, "provider_domain": domain,
                      "mapping_source": cfw_source, "port_line_sha256": hashlib.sha256(parsed[0].group(4).encode()).hexdigest(),
                      "port_line_equals_cfw_line": parsed[0].group(4).strip().rstrip(",") == cfw_line.strip().rstrip(","),
                      "note": "stimulus EV_KEY/EV_ABS chosen from the CFW-authored line interpreted with the pinned provider table; the port's normalized line never produced the expectation"}
        else:
            guid, map_name, fields = port_guid, port_map_name, port_fields
            table, index_maps = build_control_table(fields, profile["key_codes"], [int(c) for c in profile["abs"]])
        plan = compile_plan(roteiro, table, log_path, receipt_path, gamedir, profile)

        with tempfile.TemporaryDirectory() as td:
            plan_path = Path(td) / "plan.json"
            plan_path.write_text(json.dumps(plan))
            dev.put(plan_path, remote_dir + "/plan.json.tmp")
            dev.ssh("mv %s/plan.json.tmp %s/plan.json" % (remote_dir, remote_dir))
        # The agent executes with local timing; wait for it (bounded by max-seconds).
        deadline = time.time() + a.max_seconds + 120
        while time.time() < deadline:
            alive = dev.ssh("kill -0 $(cat %s/agent.pid 2>/dev/null) 2>/dev/null && echo yes || echo no" % remote_dir, check=False).stdout.strip()
            if alive != "yes":
                break
            time.sleep(3)
        agent_rc_text = dev.ssh("tail -1 %s/agent.out 2>/dev/null; echo; cat %s/steps.jsonl 2>/dev/null | wc -l" % (remote_dir, remote_dir), check=False).stdout
        dev.get(remote_dir + "/steps.jsonl", out / "steps.jsonl")
        dev.get(remote_dir + "/agent.out", out / "agent.out")
        try:
            launcher_out, _ = launcher.communicate(timeout=240)
        except subprocess.TimeoutExpired:
            kill_under(dev, gamedir)
            launcher_out, _ = launcher.communicate(timeout=120)
        (out / "device-launch.txt").write_text(launcher_out or "")
        agent_rc = 0 if (out / "steps.jsonl").exists() else 4
    except Exception:
        kill_under(dev, gamedir)
        dev.ssh("touch %s/abort" % remote_dir, check=False)
        if launcher is not None:
            try:
                launcher.communicate(timeout=180)
            except subprocess.TimeoutExpired:
                launcher.kill()
        raise
    finally:
        for rel in ("log.txt", "nxgptk-receipt.jsonl", "nxc6-receipt.log", "nxport.json", "adapter/adapter-contract.json",
                    "defaults/NEXTOSCONTROLLERS.gptk", ".nxruntime/state.json"):
            dev.get("%s/%s" % (gamedir, rel), out / rel.replace("/", "__"))
        if owner_installed:
            dev.ssh("rm -f %s" % shlex.quote(gamedir + "/NEXTOSCONTROLLERS.gptk"), check=False)
        dev.ssh("rm -rf %s" % remote_dir, check=False)

    steps_records = [json.loads(l) for l in (out / "steps.jsonl").read_text().splitlines() if l.strip()] if (out / "steps.jsonl").exists() else []
    receipt_lines = (out / "nxgptk-receipt.jsonl").read_text(errors="replace").splitlines() if (out / "nxgptk-receipt.jsonl").exists() else []
    log_lines = (out / "log.txt").read_text(errors="replace").splitlines() if (out / "log.txt").exists() else []
    verdicts, all_pass = evaluate(steps, steps_records, receipt_lines, log_lines)
    if agent_rc != 0:
        all_pass = False
        verdicts.append({"step": -1, "spec": {"agent": "rc"}, "result": "FAIL", "why": "device agent produced no step log"})

    # Identity of what was proven.
    nxport = json.loads((out / "nxport.json").read_text()) if (out / "nxport.json").exists() else {}
    executable = (nxport.get("runtime") or {}).get("executable") or nxport.get("executable")
    elf_sha = dev.ssh("sha256sum %s 2>/dev/null | cut -c1-64" % shlex.quote("%s/%s" % (gamedir, executable)), check=False).stdout.strip() if executable else None
    gptk_loaded = next((re.search(r"sha256=([0-9a-f]+)", l).group(1) for l in log_lines if "preinit:" in l and "sha256=" in l), None)
    run_id = next((re.search(r"(?:receipt_run|clean_exit_run|run_id|run)=([A-Za-z0-9._-]+)", l).group(1) for l in log_lines if "NXU0006" in l), None)
    generation = next((re.search(r"generation ([0-9a-f]{64})", l).group(1) for l in log_lines if "NXU000" in l and "generation" in l), None)
    contexts = sorted({m.group(0) for l in log_lines for m in [re.search(r"context=\w+ source=[\w:.-]+", l)] if m})
    c6_lines = [re.sub(r"pid=\d+ tid=\d+", "pid=N tid=N", l.strip()) for l in log_lines if "NXC6-DOMAIN" in l or "NXC6-SEAM" in l or "NXC6 seam" in l]
    pad_lines = [l.strip() for l in log_lines if "pad slot=" in l or (" mapping=" in l and "controller:" in l)]
    sinks = sorted({d.get("sink") for d in parse_receipt(receipt_lines) if d.get("sink")})

    receipt = {
        "schema": RECEIPT_SCHEMA, "schema_version": 1, "classification": CLASSIFICATION,
        "claim": ("valid proof of the input software on the target hardware, produced automatically by the "
                  "framework through device-faithful uinput clones on the real device; does not claim to test "
                  "mechanical wear of a physical button (hardware QA, outside this gate)"),
        "tool": {"name": "nx-device-input-proof", "version": VERSION}, "label": a.label,
        "port_id": roteiro["port_id"], "launcher": os.path.basename(a.launcher),
        "device": {"cfw": cfw, "controller": {"name": profile["name"], "bustype": profile["bustype"], "vendor": "%04x" % profile["vendor"],
                   "product": "%04x" % profile["product"], "version": profile["version"], "node": real["event_node"],
                   "handlers": real["handlers"], "key_codes": profile["key_codes"], "abs": profile["abs"],
                   "buttons": len(profile["key_codes"]), "axes": len([c for c in profile["abs"] if not ABS_HAT0X <= int(c) <= ABS_HAT3Y]),
                   "hats": len([c for c in profile["abs"] if ABS_HAT0X <= int(c) <= ABS_HAT3Y]) // 2},
                   "profile_key": pkey, "profile_reused": profile_reused},
        "sdl": sdl,
        "injection": {"provider": "uinput-clone-device-faithful", "clones": clones_info, "born_before_game_sdl_init": True,
                      "helper": "nx-input-inject-agent.py (external to the port and to every ZIP)"},
        "oracle": oracle,
        "mapping": {"guid": guid, "name": map_name, "fields": fields, "line_sha256": hashlib.sha256(line.encode()).hexdigest(),
                    "sdl_index_maps": index_maps, "control_table": table, "pads_admitted": pad_lines,
                    "face_layout": next((re.search(r"layout=(\w+)", l).group(1) for l in log_lines if "preinit:" in l and "layout=" in l), None)},
        "c6_admission": c6_lines[:40],
        "gptk": {"loaded_sha256": gptk_loaded, "default_sha256": sha256_file(out / "defaults__NEXTOSCONTROLLERS.gptk") if (out / "defaults__NEXTOSCONTROLLERS.gptk").exists() else None,
                 "owner_sha256": sha256_file(a.owner_gptk) if a.owner_gptk else None},
        "adapter_contract_sha256": sha256_file(out / "adapter__adapter-contract.json") if (out / "adapter__adapter-contract.json").exists() else None,
        "executable": {"path": executable, "sha256": elf_sha or None},
        "generation": generation, "run_id": run_id,
        "readback": {"schema": GPTK_EVIDENCE_SCHEMA, "lines": len(parse_receipt(receipt_lines)),
                     "sha256": sha256_file(out / "nxgptk-receipt.jsonl") if (out / "nxgptk-receipt.jsonl").exists() else None},
        "contexts_seen": contexts, "sinks_seen": sinks,
        "consumer": {"exit_status": next((r.get("exit_status") for r in steps_records if r.get("step", {}).get("op") == "wait_exit"), None)},
        "extraction": next((re.search(r"(NXE\d{4})", l).group(1) for l in log_lines if "TERMINAL SUCCESS NXE" in l), None),
        "verdicts": verdicts, "all_pass": all_pass,
    }
    (out / "receipt.json").write_text(json.dumps(receipt, indent=2, ensure_ascii=False) + "\n")
    sums = ["%s  %s" % (sha256_file(f), f.name) for f in sorted(out.iterdir()) if f.is_file() and f.name != "SHA256SUMS"]
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    for v in verdicts:
        print("%-4s step %2d %s %s" % (v["result"], v["step"], json.dumps(v["spec"], ensure_ascii=False)[:90], v["why"]))
    print("nx-device-input-proof: %s %s port=%s pads=%d guid=%s receipt=%s" % (
        "ALL PASS" if all_pass else "FAIL", CLASSIFICATION, roteiro["port_id"], 1 + n_clones, guid[:16], out / "receipt.json"))
    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    try:
        main()
    except ProofError as e:
        print("nx-device-input-proof: %s" % e, file=sys.stderr)
        sys.exit(2)
