#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxinput 0.10.2 — device-side agent of nx-device-input-proof.

Runs ON the device (python3 only, no third-party modules). Two commands:

  profile NODE
      Read the REAL controller node: identity (bus/vendor/product/version/
      name), EV_KEY / EV_ABS bitmasks and the EVIOCGABS ranges of every axis.
      Prints JSON. Never writes to the node.

  run PROFILE.json WORKDIR [clones]
      Create `clones` (default 1) uinput pads that are DEVICE-FAITHFUL copies
      of the profile (same name, bustype, vendor, product, version, key bits,
      abs bits and ranges) BEFORE the game starts, then wait for WORKDIR/plan.json
      (the host writes it once the port has logged the mapping it admitted),
      execute the plan with local timing, append one record per step to
      WORKDIR/steps.jsonl, destroy the clones and exit. The clones are REAL
      kernel devices: SDL of the firmware enumerates them exactly like the
      hardware pad, applies the same mapping (same GUID) and the C6 seam admits
      them through the same authority order. Nothing is injected inside the
      port and the real node is never written.

The agent measures and stimulates; it never judges.
"""
import fcntl
import json
import os
import re
import struct
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
EV_MAX, KEY_MAX, ABS_MAX = 0x1F, 0x2FF, 0x3F
# EVIOCGBIT(ev, len) = _IOC(_IOC_READ, 'E', 0x20 + ev, len); EVIOCGABS(abs) = _IOR('E', 0x40 + abs, 24)
def EVIOCGBIT(ev, length):
    return (2 << 30) | (length << 16) | (0x45 << 8) | (0x20 + ev)
def EVIOCGABS(code):
    return (2 << 30) | (24 << 16) | (0x45 << 8) | (0x40 + code)
EVIOCGID = (2 << 30) | (8 << 16) | (0x45 << 8) | 0x02
EVIOCGNAME = (2 << 30) | (256 << 16) | (0x45 << 8) | 0x06
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
INPUT_EVENT = struct.Struct("qqHHi")


def bits(buf):
    out = []
    for i, byte in enumerate(buf):
        for b in range(8):
            if byte >> b & 1:
                out.append(i * 8 + b)
    return out


def profile(node):
    fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
    try:
        ident = bytearray(8)
        fcntl.ioctl(fd, EVIOCGID, ident)
        bustype, vendor, product, version = struct.unpack("HHHH", ident)
        name = bytearray(256)
        fcntl.ioctl(fd, EVIOCGNAME, name)
        name = name.split(b"\0", 1)[0].decode("utf-8", "replace")
        evbits = bytearray((EV_MAX + 8) // 8)
        fcntl.ioctl(fd, EVIOCGBIT(0, len(evbits)), evbits)
        keys, absinfo = [], {}
        if EV_KEY in bits(evbits):
            kb = bytearray((KEY_MAX + 8) // 8)
            fcntl.ioctl(fd, EVIOCGBIT(EV_KEY, len(kb)), kb)
            keys = bits(kb)
        if EV_ABS in bits(evbits):
            ab = bytearray((ABS_MAX + 8) // 8)
            fcntl.ioctl(fd, EVIOCGBIT(EV_ABS, len(ab)), ab)
            for code in bits(ab):
                buf = bytearray(24)
                fcntl.ioctl(fd, EVIOCGABS(code), buf)
                value, lo, hi, fuzz, flat, res = struct.unpack("iiiiii", buf)
                absinfo[str(code)] = {"min": lo, "max": hi, "fuzz": fuzz, "flat": flat, "resolution": res, "value": value}
        return {"schema": "nx-device-input-proof-profile/1", "node": node, "name": name, "bustype": bustype,
                "vendor": vendor, "product": product, "version": version, "ev_bits": bits(evbits),
                "key_codes": keys, "abs": absinfo}
    finally:
        os.close(fd)


class Clone:
    """One uinput pad, device-faithful to the profile."""

    def __init__(self, prof):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_SYN)
        if prof["key_codes"]:
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
            for k in prof["key_codes"]:
                fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        if prof["abs"]:
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
            for code in prof["abs"]:
                fcntl.ioctl(self.fd, UI_SET_ABSBIT, int(code))
        absmax, absmin, absfuzz, absflat = [0] * 64, [0] * 64, [0] * 64, [0] * 64
        for code, a in prof["abs"].items():
            c = int(code)
            absmin[c], absmax[c], absfuzz[c], absflat[c] = a["min"], a["max"], a["fuzz"], a["flat"]
        payload = prof["name"].encode("utf-8")[:79].ljust(80, b"\0")
        payload += struct.pack("HHHH", prof["bustype"], prof["vendor"], prof["product"], prof["version"])
        payload += struct.pack("i", 0)
        payload += struct.pack("64i", *absmax) + struct.pack("64i", *absmin)
        payload += struct.pack("64i", *absfuzz) + struct.pack("64i", *absflat)
        os.write(self.fd, payload)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        self.abs = {int(k): (v["min"], v["max"]) for k, v in prof["abs"].items()}
        # The real pad's RESTING values (EVIOCGABS at profile time). A fresh uinput
        # node reports 0 for every axis until its first event: on a 0..255 pad that
        # is "fully left/up", so the game would see a deflected stick (and open a
        # stick gesture) before any stimulus. Publish the measured rest first.
        self.rest_values = {int(k): int(v.get("value", (v["min"] + v["max"]) // 2))
                            for k, v in prof["abs"].items()}
        time.sleep(0.4)
        self.rest()
        time.sleep(0.1)

    def emit(self, etype, code, value):
        now = time.time()
        os.write(self.fd, INPUT_EVENT.pack(int(now), int((now % 1) * 1e6), etype, code, value))

    def key(self, code, down):
        self.emit(EV_KEY, code, 1 if down else 0)
        self.emit(EV_SYN, SYN_REPORT, 0)

    def axis(self, code, unit):
        lo, hi = self.abs.get(code, (-32768, 32767))
        unit = max(-1.0, min(1.0, float(unit)))
        mid = (lo + hi) / 2.0
        self.emit(EV_ABS, code, int(round(mid + unit * ((hi - lo) / 2.0))))
        self.emit(EV_SYN, SYN_REPORT, 0)

    def rest(self):
        # Neutral = the value the REAL device reports at rest, never a computed
        # midpoint (an asymmetric range would otherwise leave a residual deflection).
        for code in self.abs:
            self.emit(EV_ABS, code, self.rest_values.get(code, int(round(sum(self.abs[code]) / 2.0))))
        self.emit(EV_SYN, SYN_REPORT, 0)

    def close(self):
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        except OSError:
            pass
        os.close(self.fd)


def count_lines(path):
    try:
        with open(path, "rb") as f:
            return sum(1 for _ in f)
    except OSError:
        return 0


def log_match(path, regex, since=0):
    pat = re.compile(regex)
    try:
        with open(path, "r", errors="replace") as f:
            for i, line in enumerate(f):
                if i >= since and pat.search(line):
                    return line.rstrip("\n")
    except OSError:
        return None
    return None


def processes_alive(port_dir):
    n = 0
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                if os.readlink("/proc/%s/exe" % pid).startswith(port_dir.rstrip("/") + "/"):
                    n += 1
            except OSError:
                pass
    return n


def run(prof, workdir, clones_wanted):
    clones = [Clone(prof) for _ in range(clones_wanted)]
    nodes = []
    # Which event nodes did the kernel give the clones? (for the receipt)
    try:
        with open("/proc/bus/input/devices") as f:
            blocks = f.read().split("\n\n")
        for b in blocks:
            if ('Name="%s"' % prof["name"]) in b and prof["node"].split("/")[-1] not in b:
                m = re.search(r"(event\d+)", b)
                if m:
                    nodes.append("/dev/input/" + m.group(1))
    except OSError:
        pass
    with open(os.path.join(workdir, "clones.json"), "w") as f:
        json.dump({"count": len(clones), "nodes": nodes, "created_unix": time.time()}, f)
    plan_path = os.path.join(workdir, "plan.json")
    deadline = time.time() + 1500
    plan = None
    while time.time() < deadline:
        if os.path.exists(plan_path):
            try:
                with open(plan_path) as f:
                    plan = json.load(f)
                break
            except (OSError, ValueError):
                time.sleep(0.5)
        if os.path.exists(os.path.join(workdir, "abort")):
            break
        time.sleep(0.5)
    rc = 4
    out = open(os.path.join(workdir, "steps.jsonl"), "w")
    try:
        if plan is None:
            out.write(json.dumps({"index": -1, "note": "no plan arrived; clones destroyed"}) + "\n")
            return rc
        rc = execute(plan, clones, out)
    finally:
        out.close()
        for c in clones:
            if c is None:
                continue
            try:
                c.rest()
            except OSError:
                pass
            c.close()
    return rc


def execute(plan, clones, out):
    log_path, receipt_path, port_dir = plan["log_path"], plan["receipt_path"], plan["port_dir"]

    def counters():
        return {"receipt_lines": count_lines(receipt_path), "log_lines": count_lines(log_path)}

    for index, step in enumerate(plan["steps"]):
        rec = {"index": index, "step": step, "t_start": time.time(), "before": counters(), "note": ""}
        op = step["op"]
        pad = clones[min(int(step.get("pad", 0)), len(clones) - 1)]
        if pad is None and op in ("press", "hold_axis"):
            rec["note"] = "pad destroyed; stimulus skipped"
            rec["after"] = counters()
            rec["t_end"] = time.time()
            out.write(json.dumps(rec) + "\n")
            out.flush()
            continue
        if op == "press":
            for code in step["codes"]:
                pad.key(code, True)
            time.sleep(step.get("ms", 120) / 1000.0)
            for code in reversed(step["codes"]):
                pad.key(code, False)
        elif op == "hold_axis":
            for code, unit in step["axes"]:
                pad.axis(code, unit)
            time.sleep(step.get("ms", 500) / 1000.0)
            for code, _u in step["axes"]:
                pad.axis(code, 0.0)
        elif op == "chord":
            # each entry: [pad_index, code]
            for pi, code in step["keys"]:
                c = clones[min(pi, len(clones) - 1)]
                if c is not None:
                    c.key(code, True)
            time.sleep(step.get("ms", 250) / 1000.0)
            for pi, code in reversed(step["keys"]):
                c = clones[min(pi, len(clones) - 1)]
                if c is not None:
                    c.key(code, False)
        elif op == "sleep":
            time.sleep(step.get("ms", 1000) / 1000.0)
        elif op == "unplug":
            # Hotplug removal of one clone: the kernel deletes the node, SDL
            # sees SDL_CONTROLLERDEVICEREMOVED, the port must release every
            # latch of that instance without disturbing the others.
            idx = min(int(step.get("pad", 1)), len(clones) - 1)
            try:
                clones[idx].rest()
            except OSError:
                pass
            clones[idx].close()
            clones[idx] = None
            rec["note"] = "clone %d destroyed" % idx
        elif op == "replug":
            idx = min(int(step.get("pad", 1)), len(clones) - 1)
            if clones[idx] is None:
                clones[idx] = Clone(plan["profile"]) if plan.get("profile") else None
                rec["note"] = "clone %d recreated" % idx if clones[idx] else "no profile in plan"
        elif op == "wait_log":
            deadline = time.time() + step.get("timeout_s", 60)
            hit = None
            while time.time() < deadline:
                hit = log_match(log_path, step["regex"])
                if hit:
                    break
                if step.get("abort_regex") and log_match(log_path, step["abort_regex"]):
                    rec["note"] = "abort_regex matched"
                    break
                time.sleep(0.5)
            rec["matched"] = hit
            rec["timed_out"] = hit is None
            if hit is None and step.get("required", True):
                rec["after"] = counters()
                rec["t_end"] = time.time()
                out.write(json.dumps(rec) + "\n")
                out.flush()
                return 3
        elif op == "wait_exit":
            deadline = time.time() + step.get("timeout_s", 60)
            status = None
            while time.time() < deadline:
                hit = log_match(log_path, r"runtime EXIT status=(\d+)")
                if hit:
                    status = int(re.search(r"status=(\d+)", hit).group(1))
                    break
                time.sleep(0.5)
            settle = time.time() + 15
            while time.time() < settle and processes_alive(port_dir):
                time.sleep(0.5)
            rec["exit_status"] = status
            rec["processes_alive"] = processes_alive(port_dir)
        elif op == "mark":
            pass
        else:
            rec["note"] = "unknown op"
        rec["after"] = counters()
        rec["t_end"] = time.time()
        out.write(json.dumps(rec) + "\n")
        out.flush()
    return 0


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "profile":
        print(json.dumps(profile(sys.argv[2])))
        return 0
    if len(sys.argv) >= 4 and sys.argv[1] == "run":
        with open(sys.argv[2]) as f:
            prof = json.load(f)
        clones = int(sys.argv[4]) if len(sys.argv) > 4 else 1
        return run(prof, sys.argv[3], max(1, min(clones, 3)))
    sys.exit("usage: nx-input-inject-agent.py profile NODE | run PROFILE.json WORKDIR [clones]")


if __name__ == "__main__":
    sys.exit(main())
