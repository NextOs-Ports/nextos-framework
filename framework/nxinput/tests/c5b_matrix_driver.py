#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Drive ONE real Godot process through ONE C5B scenario and record what each
consumer route answered.

The engine is the oracle. This file only presses buttons and reads the
engine's own output; it never recomputes a logical index. What it DOES do,
and what the 116A matrix did not, is state beforehand -- from the mapping and
from the engine's own pinned name tables -- exactly which logical button or
axis every physical press owes, so a row where the engine says nothing is a
failure instead of a line in a list.

CLAIM CLASS: REAL_API_HOST for everything the engine answered in its own
process. The uinput pads are FIXTURE_HOST and are never physical proof.
"""
import argparse
import json
import os
import pathlib
import queue
import re
import signal
import subprocess
import sys
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import c5b_expectations as EX  # noqa: E402
from vpad import VirtualPad  # noqa: E402

BTN_ORDER = [0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13A, 0x13B, 0x13C,
             0x13D, 0x13E]
STICK = (-32768, 32767, 128)
TRIGGER = (0, 255, 0)
HAT = (-1, 1, 0)
FULL_AXES = {0: STICK, 1: STICK, 2: TRIGGER, 3: STICK, 4: STICK, 5: TRIGGER,
             16: HAT, 17: HAT}
SHRUNK_AXES = {0: STICK, 1: STICK, 16: HAT, 17: HAT}

EV = re.compile(r"^MX EV route=input kind=(\w+) dev=(-?\d+) "
                r"(?:idx|axis)=(-?\d+) (?:pressed|v)=(\S+)$")
POLL = re.compile(r"^MX POLL pads=(\S+) dev=(.*) act=(.*)$")

# Both engines are deliberately windowless. Godot 4's pinned seam includes
# the minimal LinuxBSD headless flush needed after JoypadLinux queues events;
# without that flush the real engine enumerates a pad but leaves its buffered
# events undelivered. No X11/Wayland fallback is allowed here.
ENGINE_ARGS = {
    "godot3": ["--no-window", "-s", "matrix.gd"],
    "godot4": ["--headless", "--script", "matrix.gd"],
}

GUI_ENVIRONMENT = (
    "DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "XAUTHORITY",
    "MIR_SOCKET", "SDL_VIDEODRIVER", "GDK_BACKEND", "QT_QPA_PLATFORM",
)

HEADLESS_RUNTIME_PREFIXES = ("MX headless_backend=",
                             "MX display_server=")
ELF_NEEDED = re.compile(r"Shared library: \[([^]]+)\]")


def elf_needed(path):
    """Read ELF dependencies without starting the engine or a loader."""
    run = subprocess.run(["readelf", "-d", path], capture_output=True,
                         text=True)
    if run.returncode != 0:
        raise RuntimeError("readelf could not audit %s: %s" %
                           (path, run.stderr.strip()))
    return sorted(set(ELF_NEEDED.findall(run.stdout)))


class Engine(object):
    """A live engine process and everything it has printed."""

    def __init__(self, binary, which, project, env):
        self.lines = []
        self.q = queue.Queue()
        self.which = which
        self.proc = subprocess.Popen(
            [binary] + ENGINE_ARGS[which], cwd=project, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            self.lines.append(line)
            self.q.put(line)

    def wait_for(self, predicate, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.q.get(timeout=max(0.05, end - time.time()))
            except queue.Empty:
                return None
            if predicate(line):
                return line
        return None

    def drain(self):
        while True:
            try:
                self.q.get_nowait()
            except queue.Empty:
                return

    def settle(self, seconds=0.55, quiet=0.20, cap=1.5):
        """Everything the engine printed during one settling window.

        It waits `seconds`, then keeps reading until the engine has been
        quiet for `quiet`, and never longer than `seconds + cap`. A fixed
        one-shot drain lost Godot 4's polling lines, which arrive a beat
        after the event line and were thrown away by the next drain, so the
        polling route looked silent when it was answering correctly. The cap
        matters as much: an axis held at maximum jitters in its last decimal,
        the polling line changes every frame, and an unbounded wait-for-quiet
        never returns.
        """
        time.sleep(seconds)
        deadline = time.time() + cap
        out = []
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            try:
                out.append(self.q.get(timeout=min(quiet, remaining)))
            except queue.Empty:
                break
        return out

    def stop(self, sig=signal.SIGTERM, timeout=20):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return self.proc.wait(timeout=10)


PAD_NAME = "NXC5 Test Pad"


def stale_pads():
    """Any pad of ours the kernel still has. A leftover node would be
    enumerated by the next engine and silently become a second joypad --
    which is exactly how a run once reported two pads for one device."""
    try:
        devices = pathlib.Path("/proc/bus/input/devices").read_text()
    except OSError:
        return []
    return [block for block in devices.split("\n\n")
            if 'Name="%s"' % PAD_NAME in block]


def make_pad(name=PAD_NAME, axes=None, vendor=0x1209, product=0xC5A1,
             version=0x0110):
    pad = VirtualPad(name, keys=list(BTN_ORDER),
                     axes=dict(FULL_AXES if axes is None else axes),
                     vendor=vendor, product=product, version=version)
    if pad.node is None:
        pad.close()
        raise SystemExit("the kernel never produced a node for the pad")
    return pad


def _unquote(text):
    return text.replace("\\", "").replace('"', "")


def observed(lines):
    """Split one settling window into the three routes."""
    events = []
    polls = []
    for line in lines:
        match = EV.match(line)
        if match:
            kind, dev, index, value = match.groups()
            events.append({"kind": kind, "dev": int(dev),
                           "index": int(index), "value": value})
            continue
        match = POLL.match(line)
        if match:
            # Godot 4 prints an Array of Strings with the quotes ESCAPED, so
            # the same reading comes out as ax["0=1"] on Godot 3 and as
            # ax[\"0=1\"] on Godot 4. Normalising here is why the polling
            # route stopped looking silent on one major and not the other.
            polls.append({"pads": match.group(1),
                          "dev": _unquote(match.group(2)),
                          "act": _unquote(match.group(3))})
    return events, polls


def poll_pressed(polls, dev, index):
    """Did the polling route ever report this logical button held down?"""
    needle = "%d:btn[" % dev
    for poll in polls:
        text = poll["dev"]
        at = text.find(needle)
        if at < 0:
            continue
        close = text.find("]", at)
        held = text[at + len(needle):close]
        parts = [p.strip() for p in held.split(",") if p.strip()]
        if str(index) in parts:
            return True
    return False


def poll_axis(polls, dev, index):
    """The largest magnitude the polling route reported for this axis."""
    best = None
    marker = "%d=" % index
    for poll in polls:
        text = poll["dev"]
        at = 0
        while True:
            at = text.find(":ax[", at)
            if at < 0:
                break
            close = text.find("]", at)
            body = text[at + 4:close]
            for entry in body.split(","):
                entry = entry.strip()
                if entry.startswith(marker):
                    try:
                        value = float(entry[len(marker):])
                    except ValueError:
                        continue
                    if best is None or abs(value) > abs(best):
                        best = value
            at = close + 1
    return best


LIVE = re.compile(r"btn\[[^\]]+\]|ax\[[^\]]+\]|act=\[[^\]]+\]")


def poll_is_live(line):
    """True only when the polling line shows something actually held.

    `MX POLL pads=[] dev=[] act=[]` is the correct SILENT state -- treating
    it as noise was what made every origin negative look like a leak.
    """
    return bool(LIVE.search(_unquote(line)))


def press_button(pad, engine, code, hold=0.30):
    engine.drain()
    pad.key(code, 1)
    down = engine.settle(hold)
    pad.key(code, 0)
    up = engine.settle(hold)
    return down, up


def move_axis(pad, engine, code, value, hold=0.45):
    engine.drain()
    pad.abs(code, value)
    return engine.settle(hold)


def move_hat(pad, engine, x, y, hold=0.45):
    engine.drain()
    pad.abs(16, x)
    pad.abs(17, y)
    return engine.settle(hold)


def run_matrix(pad, engine, expectations, rows):
    """The eighteen V2 groups, both sticks, the triggers and the hat."""
    hat_dir = {1: (0, -1), 2: (1, 0), 4: (0, 1), 8: (-1, 0)}
    for exp in expectations:
        kind = exp.physical[0]
        row = {"group": exp.group, "control": exp.control,
               "physical": list(exp.physical),
               "expect_kind": exp.output_kind,
               "expect_index": exp.output_index}
        if kind == "button":
            down, up = press_button(pad, engine, exp.physical[1])
            ev_down, poll_down = observed(down)
            ev_up, _ = observed(up)
            row["case"] = "press_release"
            row["input_press"] = ev_down
            row["input_release"] = ev_up
            row["poll_pressed"] = poll_pressed(poll_down, 0,
                                               exp.output_index)
            row["poll_axis"] = poll_axis(poll_down, 0, exp.output_index)
        elif kind == "axis":
            code = exp.physical[1]
            top = 32767 if code not in (2, 5) else 255
            low = -32768 if code not in (2, 5) else 0
            lines = move_axis(pad, engine, code, top)
            ev, polls = observed(lines)
            row["case"] = "axis_max"
            row["input_max"] = ev
            row["poll_axis_max"] = poll_axis(polls, 0, exp.output_index)
            row["poll_pressed_max"] = poll_pressed(polls, 0,
                                                   exp.output_index)
            lines = move_axis(pad, engine, code, low)
            ev, polls = observed(lines)
            row["input_min"] = ev
            row["poll_axis_min"] = poll_axis(polls, 0, exp.output_index)
            centre = 0 if code not in (2, 5) else 0
            lines = move_axis(pad, engine, code, centre)
            ev, polls = observed(lines)
            row["input_centre"] = ev
            row["poll_axis_centre"] = poll_axis(polls, 0, exp.output_index)
            lines = move_axis(pad, engine, code, 1000 if code not in (2, 5)
                              else 10)
            ev, polls = observed(lines)
            row["input_deadzone"] = ev
            row["poll_axis_deadzone"] = poll_axis(polls, 0, exp.output_index)
            move_axis(pad, engine, code, centre)
        elif kind == "hat":
            mask = exp.physical[2]
            x, y = hat_dir[mask]
            lines = move_hat(pad, engine, x, y)
            ev, polls = observed(lines)
            row["case"] = "hat"
            row["input_press"] = ev
            row["poll_pressed"] = poll_pressed(polls, 0, exp.output_index)
            lines = move_hat(pad, engine, 0, 0)
            row["input_release"] = observed(lines)[0]
        rows.append(row)
    # Diagonal, which the contract accepts as two directions at once.
    lines = move_hat(pad, engine, -1, -1)
    ev, polls = observed(lines)
    rows.append({"group": "HAT", "case": "hat_diagonal_upleft",
                 "input_press": ev, "poll_line": [p["dev"] for p in polls]})
    move_hat(pad, engine, 0, 0)


def chord_cases(pad, engine, expectations, rows, second_pad=None):
    """The exit chord and every negative, from the mapping's own bindings."""
    by_group = {e.group: e for e in expectations}
    select = by_group.get("SELECT")
    start = by_group.get("START")
    if select is None or start is None:
        return
    sel_code = select.physical[1]
    sta_code = start.physical[1]
    l2 = by_group.get("L2")
    r2 = by_group.get("R2")
    guide = by_group.get("GUIDE")

    def negative(name, actions):
        engine.drain()
        for do in actions:
            do()
        lines = engine.settle(0.9)
        fired = any("MX CHORD fired" in l for l in lines)
        for do in reversed(actions):
            pass
        rows.append({"group": "CHORD", "case": "negative_" + name,
                     "chord_fired": fired,
                     "lines": [l for l in lines if "CHORD" in l]})
        return fired

    # a single key of the chord
    negative("select_alone", [lambda: pad.key(sel_code, 1)])
    pad.key(sel_code, 0)
    engine.settle(0.3)
    negative("start_alone", [lambda: pad.key(sta_code, 1)])
    pad.key(sta_code, 0)
    engine.settle(0.3)
    # a pair that is NOT the chord
    if guide is not None:
        negative("guide_start", [lambda: pad.key(guide.physical[1], 1),
                                 lambda: pad.key(sta_code, 1)])
        pad.key(guide.physical[1], 0)
        pad.key(sta_code, 0)
        engine.settle(0.3)
    if l2 is not None and r2 is not None and l2.physical[0] == "button":
        negative("l1_r1", [lambda: pad.key(0x136, 1), lambda: pad.key(0x137, 1)])
        pad.key(0x136, 0)
        pad.key(0x137, 0)
        engine.settle(0.3)
    else:
        negative("l1_r1", [lambda: pad.key(0x136, 1),
                           lambda: pad.key(0x137, 1)])
        pad.key(0x136, 0)
        pad.key(0x137, 0)
        engine.settle(0.3)
    # release: press both, release one, the chord must not fire afterwards
    engine.drain()
    pad.key(sel_code, 1)
    pad.key(sel_code, 0)
    pad.key(sta_code, 1)
    lines = engine.settle(0.9)
    rows.append({"group": "CHORD", "case": "negative_release_between",
                 "chord_fired": any("MX CHORD fired" in l for l in lines)})
    pad.key(sta_code, 0)
    engine.settle(0.3)
    # two different pads holding one key each
    if second_pad is not None:
        engine.drain()
        pad.key(sel_code, 1)
        second_pad.key(sta_code, 1)
        lines = engine.settle(1.0)
        rows.append({"group": "CHORD", "case": "negative_mixed_pads",
                     "chord_fired": any("MX CHORD fired" in l
                                        for l in lines)})
        pad.key(sel_code, 0)
        second_pad.key(sta_code, 0)
        engine.settle(0.3)
    # and finally the real chord, in the inverted order too
    engine.drain()
    pad.key(sta_code, 1)
    pad.key(sel_code, 1)
    lines = engine.settle(1.4)
    rows.append({"group": "CHORD", "case": "select_start_same_pad",
                 "chord_fired": any("MX CHORD fired" in l for l in lines),
                 "lines": [l for l in lines if "CHORD" in l or
                           "FINALISE" in l]})
    pad.key(sta_code, 0)
    pad.key(sel_code, 0)


BAD_DECLARATIONS = {
    "unknown_domain": ("domain", "gadot"),
    "empty_domain": ("domain", ""),
    "provider_off_allowlist": ("provider", "some-random-tool"),
    "digest_mismatch": ("mapping_sha256",
                        "0" * 64),
    "foreign_guid": ("guid", "03000000ffffffffffffffffffffffff"),
}


def run_negatives(args, env, result):
    """Each bad declaration, against the REAL engine, one run at a time.

    Nothing here is injected into the seam: the engine is started normally
    with a declaration whose origin does not authenticate, and the only
    question asked is whether the pad was announced. It must not be -- no
    joypad, no event, no InputMap action, no polling, nothing for the game.
    """
    base = pathlib.Path(args.declaration).read_text()
    work = pathlib.Path(args.out).parent
    result["headless_runtime"] = []
    for name, (field, value) in sorted(BAD_DECLARATIONS.items()):
        lines = []
        for line in base.splitlines():
            key = line.split("=", 1)[0]
            lines.append("%s=%s" % (field, value) if key == field else line)
        bad = work / ("bad-decl-%s-%s.txt" % (args.which, name))
        bad.write_text("\n".join(lines) + "\n", encoding="utf-8")
        receipt = work / ("receipt-%s-neg-%s.txt" % (args.which, name))
        for stale in (receipt,):
            try:
                os.unlink(stale)
            except OSError:
                pass
        for _ in range(50):
            if not stale_pads():
                break
            time.sleep(0.2)
        pad = make_pad()
        engine = None
        try:
            run_env = dict(env)
            run_env["NXC5B_DECLARATION"] = str(bad)
            run_env["NXC5B_RECEIPT"] = str(receipt)
            run_env["NXC5B_SECONDS"] = "12"
            engine = Engine(args.engine, args.which, args.project, run_env)
            engine.wait_for(lambda l: l.startswith("MX ready"), 90)
            time.sleep(1.0)
            press_button(pad, engine, 0x130, hold=0.45)
            lines_seen = list(engine.lines)
            result["headless_runtime"].extend(
                line for line in lines_seen
                if line.startswith(HEADLESS_RUNTIME_PREFIXES))
            engine.stop()
        finally:
            if engine is not None and engine.proc.poll() is None:
                engine.stop(signal.SIGKILL, timeout=10)
            pad.close()
        pads_line = next((l for l in lines_seen if l.startswith("MX pads=")),
                         "")
        events = [l for l in lines_seen if "route=input" in l]
        polls = [l for l in lines_seen
                 if l.startswith("MX POLL") and poll_is_live(l)]
        text = receipt.read_text() if receipt.exists() else ""
        result["rows"].append({
            "group": "NEGATIVE", "case": "origin_" + name,
            "field": field, "value": value,
            "pads_line": pads_line,
            "announced": "MX pad id=" in " ".join(lines_seen),
            "events": events[:8],
            "non_empty_polls": polls[:8],
            "receipt": text,
            "blocked_reason": next(
                (l for l in text.splitlines() if "result=block" in l), ""),
        })
        print("NEGATIVE %s/%s announced=%s"
              % (args.which, name, "MX pad id=" in " ".join(lines_seen)))
    pathlib.Path(args.out).write_text(json.dumps(result, indent=1),
                                      encoding="utf-8")
    print("SCENARIO %s/negatives rows=%d" % (args.which, len(result["rows"])))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True)
    ap.add_argument("--which", required=True, choices=("godot3", "godot4"))
    ap.add_argument("--checkout", required=True)
    ap.add_argument("--project", required=True)
    ap.add_argument("--scenario", required=True,
                    choices=("matrix", "null", "ownerswap", "native",
                             "twopads", "hotplug", "sigterm", "negatives"))
    ap.add_argument("--declaration", default="")
    ap.add_argument("--mapping", default="")
    ap.add_argument("--receipt", required=True)
    ap.add_argument("--save", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=120)
    args = ap.parse_args()

    env = dict(os.environ)
    # The test must remain hermetic even on an interactive workstation: do
    # not merely ask the engine not to create a window; remove every common
    # route by which it could connect to a desktop display.
    for name in GUI_ENVIRONMENT:
        env.pop(name, None)
    env["NXC5B_RECEIPT"] = args.receipt
    env["NXC5B_SAVE"] = args.save
    env["NXC5B_SECONDS"] = str(args.seconds)
    if args.declaration:
        env["NXC5B_DECLARATION"] = args.declaration
    else:
        env.pop("NXC5B_DECLARATION", None)
    for stale in (args.save, args.receipt):
        try:
            os.unlink(stale)
        except OSError:
            pass

    for _ in range(50):
        if not stale_pads():
            break
        time.sleep(0.2)
    if stale_pads():
        raise SystemExit("a previous run left a %s node behind; refusing to "
                         "start, because the engine would enumerate it"
                         % PAD_NAME)

    tables = EX.load_tables(args.checkout, args.which)
    mapping = ""
    if args.mapping:
        mapping = pathlib.Path(args.mapping).read_text().strip()

    result = {
        "scenario": args.scenario,
        "engine": args.which,
        "mapping": mapping,
        "rows": [],
        "notes": {},
        "execution": {
            "argv": [args.engine] + ENGINE_ARGS[args.which],
            "gui_environment": {
                name: env[name] for name in GUI_ENVIRONMENT if name in env
            },
            "elf_needed": elf_needed(args.engine),
            "binary_basename": pathlib.Path(args.engine).name,
            "window_policy": ("godot3--no-window" if args.which == "godot3"
                              else "godot4--headless"),
        },
    }

    if args.scenario == "negatives":
        return run_negatives(args, env, result)
    pads = []
    created_nodes = []
    pad = None
    engine = None
    try:
        if args.scenario in ("matrix", "null", "ownerswap", "native",
                             "sigterm"):
            pad = make_pad()
            pads.append(pad)
            created_nodes.append(pad.node)
        elif args.scenario == "twopads":
            # Two devices carrying the SAME GUID: same bus/vendor/product/
            # version. They must still get independent fds, joy ids and state.
            pad = make_pad()
            second = make_pad()
            pads += [pad, second]
            created_nodes += [pad.node, second.node]

        engine = Engine(args.engine, args.which, args.project, env)
        if engine.wait_for(lambda l: l.startswith("MX ready"), 90) is None:
            result["notes"]["fatal"] = "the engine never became ready"
            raise SystemExit(1)
        boot = [l for l in engine.lines if l.startswith("MX pad") or
                l.startswith("MX pads=") or l.startswith("MX chord_binding")]
        result["notes"]["boot"] = boot
        time.sleep(0.8)

        key_codes = list(BTN_ORDER)
        abs_codes = sorted(FULL_AXES)
        expectations, unmapped = ([], [])
        if mapping:
            expectations, unmapped = EX.build(mapping, tables, key_codes,
                                              abs_codes)
        result["notes"]["unmapped_groups"] = unmapped
        result["expectations"] = [
            {"group": e.group, "control": e.control,
             "physical": list(e.physical), "kind": e.output_kind,
             "index": e.output_index} for e in expectations]

        if args.scenario in ("matrix", "ownerswap"):
            run_matrix(pad, engine, expectations, result["rows"])
            chord_cases(pad, engine, expectations, result["rows"])
        elif args.scenario == "null":
            # A and B are suppressed: nothing may appear on ANY route.
            for name, code in (("A", 0x130), ("B", 0x131)):
                down, up = press_button(pad, engine, code, hold=0.45)
                ev, polls = observed(down)
                result["rows"].append({
                    "group": name, "case": "null_no_leak",
                    "input_press": ev,
                    "poll_lines": [p["dev"] for p in polls],
                    "action_lines": [p["act"] for p in polls]})
            # a control that is NOT suppressed still has to work
            run_matrix(pad, engine, [e for e in expectations
                                     if e.group in ("X", "Y", "START")],
                       result["rows"])
        elif args.scenario == "native":
            # No declaration at all: the seam must add nothing and take
            # nothing away, so the engine's own behaviour stands.
            for name, code in (("A", 0x130), ("START", 0x13B)):
                down, up = press_button(pad, engine, code, hold=0.45)
                ev, polls = observed(down)
                result["rows"].append({
                    "group": name, "case": "native_passthrough",
                    "input_press": ev,
                    "poll_lines": [p["dev"] for p in polls]})
        elif args.scenario == "twopads":
            for index, this in enumerate(pads):
                down, up = press_button(this, engine, 0x130, hold=0.45)
                ev, polls = observed(down)
                result["rows"].append({
                    "group": "A", "case": "two_pads_pad%d" % index,
                    "input_press": ev,
                    "poll_lines": [p["dev"] for p in polls]})
            chord_cases(pad, engine, expectations, result["rows"],
                        second_pad=pads[1])
        elif args.scenario == "hotplug":
            result["notes"]["boot_pads"] = [
                l for l in engine.lines if l.startswith("MX pads=")]
            engine.drain()
            pad = make_pad()
            pads.append(pad)
            created_nodes.append(pad.node)
            time.sleep(1.6)
            lines = engine.settle(1.0)
            result["notes"]["after_plug"] = [
                l for l in lines if "POLL" in l or "pad" in l]
            down, up = press_button(pad, engine, 0x130, hold=0.45)
            ev, polls = observed(down)
            result["rows"].append({"group": "A", "case": "hotplug_press",
                                   "input_press": ev,
                                   "poll_lines": [p["dev"] for p in polls]})
            engine.drain()
            pad.close()
            pads.remove(pad)
            time.sleep(1.6)
            result["notes"]["after_unplug"] = [
                l for l in engine.settle(1.0) if "POLL" in l]
            # Reconnect with SHRUNK capabilities: the mapping still binds
            # `righttrigger:a5`, which this pad no longer has, so the seam
            # must refuse it. Nothing may be inherited from the generation
            # that was admitted a moment ago.
            engine.drain()
            shrunk = make_pad(axes=SHRUNK_AXES)
            pads.append(shrunk)
            created_nodes.append(shrunk.node)
            time.sleep(1.8)
            after = engine.settle(1.2)
            result["notes"]["after_shrunk_reconnect"] = after
            down, up = press_button(shrunk, engine, 0x130, hold=0.45)
            ev, polls = observed(down)
            result["rows"].append({"group": "A", "case": "shrunk_reconnect",
                                   "input_press": ev,
                                   "poll_lines": [p["dev"] for p in polls]})
        elif args.scenario == "sigterm":
            # The LAUNCHER receives the termination signal, not the game.
            # nxinput_exit_chord_fold_signal() is the contract: a signal
            # raises the SAME sticky request the chord raises, and the port
            # is asked to stop the way it already knows how -- on its pad.
            # Godot 3 in script mode turns no signal into a quit, so a bare
            # SIGTERM to the engine would kill it with no save at all; that
            # is precisely why the convergence has to be explicit.
            press_button(pad, engine, 0x130, hold=0.35)
            by_group = {e.group: e for e in expectations}
            select = by_group["SELECT"].physical[1]
            start = by_group["START"].physical[1]
            got_signal = {"at": None}

            def on_term(signum, frame):
                got_signal["at"] = time.time()

            previous = signal.signal(signal.SIGTERM, on_term)
            os.kill(os.getpid(), signal.SIGTERM)
            time.sleep(0.2)
            signal.signal(signal.SIGTERM, previous)
            result["notes"]["launcher_saw_sigterm"] = \
                got_signal["at"] is not None
            engine.drain()
            pad.key(select, 1)
            pad.key(start, 1)
            lines = engine.settle(1.6)
            pad.key(select, 0)
            pad.key(start, 0)
            result["notes"]["after_signal"] = [
                l for l in lines if "CHORD" in l or "FINALISE" in l]
            try:
                result["notes"]["exit_code"] = engine.proc.wait(timeout=25)
            except subprocess.TimeoutExpired:
                result["notes"]["exit_code"] = engine.stop(signal.SIGKILL,
                                                           timeout=10)

        if "exit_code" not in result["notes"]:
            result["notes"]["exit_code"] = engine.stop()
    finally:
        if engine is not None and engine.proc.poll() is None:
            engine.stop(signal.SIGKILL, timeout=10)
        for this in pads:
            try:
                this.close()
            except Exception:
                pass

    result["pads_created"] = created_nodes
    result["pad_key_codes"] = sorted(BTN_ORDER)
    result["pad_declares_keyboard"] = any(code < 0x100 for code in BTN_ORDER)
    result["all_lines"] = engine.lines if engine else []
    result["headless_runtime"] = [
        line for line in result["all_lines"]
        if line.startswith(HEADLESS_RUNTIME_PREFIXES)
    ]
    save = pathlib.Path(args.save)
    result["save_file"] = save.read_text() if save.exists() else ""
    receipt = pathlib.Path(args.receipt)
    result["seam_receipt"] = receipt.read_text() if receipt.exists() else ""
    pathlib.Path(args.out).write_text(json.dumps(result, indent=1),
                                      encoding="utf-8")
    print("SCENARIO %s/%s rows=%d exit=%s"
          % (args.which, args.scenario, len(result["rows"]),
             result["notes"].get("exit_code")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
