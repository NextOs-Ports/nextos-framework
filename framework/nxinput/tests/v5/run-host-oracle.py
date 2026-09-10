#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V5 / H2-H5 host oracle driver.

Creates DEVICE-FAITHFUL uinput clones from an incident fixture (bus/vid/pid/
version/name/keys/abs/EVIOCGABS), stages the fixture's CFW mapping exactly as
the launcher would (SDL_GAMECONTROLLERCONFIG -> NXC6_STAGED_MAPPING), runs
the pure SDL2 harness with the real C6 glue against the SDL2 THIS HOST maps,
then presses POSITIONS from the physical profile and judges the semantic the
provider delivered with nxoracle_v5 -- never with the mapping under test.

Claims: PROVED_SOFTWARE_CONSISTENCY on the host provider (pin
pin-5da9fd36-sdl2-2.32.70). It is NOT physical proof of any handheld.

usage: run-host-oracle.py FIXTURE.json HARNESS_BIN OUT_DIR [--stock DOMAIN] [--legacy-stage]

0.11.1: `--stock DOMAIN` is the review's RED case for an UNPINNED provider
(matrix M1c): the CFW line is LEFT in SDL_GAMECONTROLLERCONFIG, the harness
calls nxc6_stage_before_init (NXC6_STAGE_V5=1), the framework must report
the provider as unknown, leave the variable for the stock import, ADMIT the
pad in stock mode -- and the pad must NOT be mute: every position must
deliver exactly what STOCK SDL delivers, which the oracle computes from
DOMAIN (the host provider's real numbering, known to the oracle, unknown to
the framework). `--legacy-stage` keeps the blind staging (NXC6_STAGED_MAPPING)
under an unknown provider: the seam must reinstate the CFW's own env line.
"""
import ctypes, fcntl, json, os, re, struct, subprocess, sys, time, hashlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools"))
import nxoracle_v5 as ox

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502

class Clone:
    def __init__(self, name, keys, absl, bus=0x19, vid=1, pid=1, ver=0x100):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        for t in (0, 1, 3): fcntl.ioctl(self.fd, UI_SET_EVBIT, t)
        for k in keys: fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        for a in absl: fcntl.ioctl(self.fd, UI_SET_ABSBIT, a)
        amax = [0]*64; amin = [0]*64; fuzz = [0]*64; flat = [0]*64
        for a in absl:
            if 0x10 <= a <= 0x17: amin[a], amax[a] = -1, 1
            else: amin[a], amax[a], fuzz[a], flat[a] = -1800, 1800, 32, 32   # CubeXX DTS calibration
        p = name.encode()[:79].ljust(80, b"\0") + struct.pack("HHHH", bus, vid, pid, ver) + struct.pack("i", 0)
        p += struct.pack("64i", *amax) + struct.pack("64i", *amin) + struct.pack("64i", *fuzz) + struct.pack("64i", *flat)
        os.write(self.fd, p); fcntl.ioctl(self.fd, UI_DEV_CREATE)
    def emit(self, t, c, v): os.write(self.fd, struct.pack("llHHi", 0, 0, t, c, v))
    def key(self, code, down): self.emit(1, code, 1 if down else 0); self.emit(0, 0, 0)
    def close(self): fcntl.ioctl(self.fd, UI_DEV_DESTROY); os.close(self.fd)

def main():
    fx = json.load(open(sys.argv[1])); harness = sys.argv[2]; out = sys.argv[3]
    stock_domain = None; legacy_stage = False
    if "--stock" in sys.argv: stock_domain = sys.argv[sys.argv.index("--stock") + 1]
    if "--legacy-stage" in sys.argv: legacy_stage = True
    # 1.2: never write under the immutable V4 checkout or the frozen V4 ZIP folder.
    real = os.path.realpath(out)
    for forbidden in (os.path.expanduser("~/.codex-worktrees/framework-v4-final"), os.path.expanduser("~/Área de trabalho/V4-QUATRO-ZIPS-FINAIS")):
        if real == forbidden or real.startswith(forbidden + os.sep):
            sys.exit("refusing output under the immutable V4 path: %s" % out)
    os.makedirs(out, exist_ok=True)
    keys = [int(c, 16) for c in fx["device"]["ev_key_codes"]]
    absl = [int(c, 16) for c in fx["device"]["ev_abs_codes"]]
    phys = fx["physical_table"]["position_to_ev_key"]
    mapping = fx["source"].get("mapping") or fx["source"]["mapping_retro"]
    name = fx["device"].get("name_evidence_only", "V5 clone pad")
    env = dict(os.environ, NXC6_SEAM="1", NXC6_RECEIPT=os.path.join(out, "nxc6-receipt.log"), SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS="1")
    env.pop("SDL_GAMECONTROLLERCONFIG", None); env.pop("NXC6_STAGED_MAPPING", None)
    if stock_domain and not legacy_stage:
        # the CFW's own environment, untouched by the port: the framework stages it (or leaves it) itself
        env["SDL_GAMECONTROLLERCONFIG"] = mapping + "\n"; env["NXC6_STAGE_V5"] = "1"
    else:
        env["NXC6_STAGED_MAPPING"] = mapping + "\n"
    open(env["NXC6_RECEIPT"], "w").close()
    c0 = Clone(name, keys, absl); c1 = Clone(name + " 2", keys, absl)
    time.sleep(0.6)
    log = open(os.path.join(out, "harness.log"), "w")
    proc = subprocess.Popen([harness, "12", name], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.set_blocking(proc.stdout.fileno(), False)
    lines = []; buf = [b""]
    def pump(sec):
        t0 = time.time()
        while time.time() - t0 < sec:
            try:
                chunk = os.read(proc.stdout.fileno(), 65536)
            except BlockingIOError:
                chunk = b""
            if chunk:
                buf[0] += chunk
                while b"\n" in buf[0]:
                    line, buf[0] = buf[0].split(b"\n", 1)
                    text = line.decode(errors="replace")
                    lines.append(text); log.write(text + "\n"); log.flush()
            else:
                time.sleep(0.01)
    pump(1.5)
    opens = [l for l in lines if l.startswith("OPEN ")]
    prov = [l for l in lines if l.startswith("NXC6-PROVIDER")]
    dom = [l for l in lines if l.startswith("NXC6-DOMAIN")]
    verdicts = []
    def v(name, ok, detail): verdicts.append({"check": name, "result": "PASS" if ok else "FAIL", "detail": detail}); print("%s %-58s %s" % ("PASS" if ok else "FAIL", name, detail))
    v("two clones admitted and opened", len(opens) == 2, "%d OPEN lines" % len(opens))
    v("provider receipt emitted once per generation", len(prov) == 1, prov[0][:160] if prov else "-")
    provider_domain = re.search(r"domain=(\S+)", prov[0]).group(1) if prov else "undeclared"
    sha = re.search(r"sha256=(\S+)", prov[0]).group(1) if prov else ""
    receipt_file = open(env["NXC6_RECEIPT"]).read().splitlines() if os.path.exists(env["NXC6_RECEIPT"]) else []
    v("NXC6-PROVIDER line lands in the durable NXC6_RECEIPT file (review finding 3)", any(l.startswith("NXC6-PROVIDER") for l in receipt_file), "%d receipt lines" % len(receipt_file))
    if stock_domain:
        # the RED case: framework must NOT know this provider, must not mute the pad, must behave as stock
        v("provider is UNKNOWN to the framework (unpinned bytes)", re.search(r"method=unknown domain=undeclared", prov[0] if prov else "") is not None, sha[:16])
        stage = [l for l in lines if l.startswith("STAGE ")]
        if not legacy_stage:
            v("nxc6_stage_before_init LEFT the CFW line for the stock import (rc=1, env still set)", bool(stage) and "rc=1" in stage[0] and "env_still_set=1" in stage[0], stage[0] if stage else "-")
        ann = [l for l in receipt_file if "stage=announce" in l and "result=admit" in l]
        v("pad ADMITTED in stock mode (never mute): source=stock-passthrough, nothing translated", len(ann) >= 2 and all("source=stock-passthrough" in l and "translated=0" in l and "mapping_present=1" in l for l in ann), ann[0][:200] if ann else "-")
        if legacy_stage:
            v("legacy blind staging + unknown provider: the CFW's own env line was reinstated", all("env_reinstated=1" in l for l in ann) if ann else False, "")
        v("no NXC6-DOMAIN translation attempted under an unknown provider", len(dom) == 0, "%d domain lines" % len(dom))
        provider_domain = stock_domain  # what the oracle KNOWS the host really is; the framework did not
    else:
        v("provider sha bound and pinned/measured (not ldconfig)", re.search(r"sha_bound=[12]", prov[0] if prov else "") is not None and re.search(r"method=(pinned-elf|measured-inprocess)", prov[0] if prov else "") is not None, sha[:16])
        v("NXC6-DOMAIN v5 names source and provider domains", any("source_domain=sdl2-ascending-patched" in d and "provider_domain=%s" % provider_domain in d for d in dom), dom[0][:200] if dom else "-")
    O = ox.Oracle(phys, provider_domain, keys, absl)
    inst = [int(re.search(r"instance=(\d+)", o).group(1)) for o in opens]
    # what semantic does the CFW line assign to each POSITION? (fixture-side, provider-independent)
    _, _, fields = ox.parse_mapping(mapping)
    # the line was authored for the ascending provider; under STOCK on another
    # provider the numbering that reads it is the host's own (that is the P0
    # defect as stock SDL lives it; the oracle expects exactly that, not silence)
    reader = ox.button_table(stock_domain if stock_domain else "sdl2-ascending-patched", keys)
    pos_sem = {}
    for pos, code in phys.items():
        ordn = [i for i, c in reader.items() if c == int(code, 16)]
        sem = [k for k, val in fields.items() if ordn and val == "b%d" % ordn[0]]
        if sem: pos_sem[pos] = sem[0]
    def edges_for(instance, since):
        return [l for l in lines[since:] if l.startswith("EDGE instance=%d " % instance)]
    for pos in ("face.south", "face.east", "face.west", "face.north", "l1", "r1", "select", "start", "l2", "r2", "l3", "r3", "guide"):
        code = O.stimulus(pos); expected = pos_sem.get(pos)
        since = len(lines); c0.key(code, True); pump(0.15); c0.key(code, False); pump(0.15)
        got = [re.search(r"semantic=(\S+) pressed=(\d)", e).groups() for e in edges_for(inst[0], since)]
        # a trigger presented as a BUTTON by the pad reaches SDL as the trigger AXIS (0 -> max -> 0)
        for l in lines[since:]:
            m = re.match(r"AXIS instance=%d semantic=(\S+) value=(-?\d+)" % inst[0], l)
            if m and m.group(1) in ("lefttrigger", "righttrigger"):
                got.append((m.group(1), "1" if int(m.group(2)) > 16000 else "0"))
        delivered = [s for s, p in got if p == "1"]
        released = [s for s, p in got if p == "0"]
        if stock_domain and expected in (None, "volumedown", "volumeup", "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad"):
            # under STOCK on this provider the position lands on no controller
            # semantic (or one the harness reports as `other`): the honest
            # expectation is "nothing standard delivered", never silence as PASS
            v("position %s -> stock semantic '%s' (non-controller): no standard delivery" % (pos, expected), all(s == "other" for s in delivered) and all(s == "other" for s in released), "delivered=%s" % delivered)
        else:
            v("position %s -> CFW semantic '%s' exactly once, released" % (pos, expected), delivered == [expected] and released == [expected], "delivered=%s" % delivered)
    # chord same instance: the PHYSICAL positions whose semantic is back/start
    # under the numbering that really reads the line (pinned: the CFW's; stock:
    # the host's own -- the chord follows what the game sees, not the plastic)
    sem_pos = {v_: k_ for k_, v_ in pos_sem.items()}
    sel_code = int(phys[sem_pos.get("back", "select")], 16); sta_code = int(phys[sem_pos.get("start", "start")], 16)
    v("chord positions resolved from the reading numbering", "back" in sem_pos and "start" in sem_pos, "select=%s start=%s" % (sem_pos.get("back"), sem_pos.get("start")))
    since = len(lines); c0.key(sel_code, True); c0.key(sta_code, True); pump(0.2)
    v("SELECT+START same instance -> CHORD once", sum(1 for l in lines[since:] if l.startswith("CHORD instance=%d" % inst[0])) == 1, "")
    c0.key(sel_code, False); c0.key(sta_code, False); pump(0.2)
    # cross pad chord must not fire
    since = len(lines); c0.key(sel_code, True); c1.key(sta_code, True); pump(0.2)
    v("SELECT pad0 + START pad1 -> no CHORD", not any(l.startswith("CHORD") for l in lines[since:]), "")
    c0.key(sel_code, False); c1.key(sta_code, False); pump(0.2)
    # L2+R2 never a chord
    since = len(lines); c0.key(int(phys["l2"], 16), True); c0.key(int(phys["r2"], 16), True); pump(0.2)
    v("L2+R2 -> no CHORD", not any(l.startswith("CHORD") for l in lines[since:]), "")
    c0.key(int(phys["l2"], 16), False); c0.key(int(phys["r2"], 16), False); pump(0.2)
    # hotplug: destroy clone 1 -> REMOVED; no stuck edge
    since = len(lines); c1.close(); pump(0.6)
    v("unplug clone 1 -> REMOVED + forget", any(l.startswith("REMOVED instance=%d" % inst[1]) for l in lines[since:]), "")
    proc.wait(timeout=15); pump(0.2)
    c0.close()
    receipt = {"receipt": "nx-host-oracle-v5/1", "class": "PROVED_SOFTWARE_CONSISTENCY" if not stock_domain else "PROVED_STOCK_UNDER_UNKNOWN_PROVIDER", "mode": "stock:%s%s" % (stock_domain, " legacy-stage" if legacy_stage else "") if stock_domain else "pinned", "fixture": fx["id"], "fixture_sha256": hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest(),
               "provider": prov[0] if prov else None, "domain_lines": dom, "verdicts": verdicts, "harness_exit": proc.returncode,
               "fails": sum(1 for x in verdicts if x["result"] == "FAIL")}
    json.dump(receipt, open(os.path.join(out, "receipt.json"), "w"), indent=1)
    print("host-oracle: %s (%d fails)" % ("OK" if receipt["fails"] == 0 else "FAIL", receipt["fails"]))
    sys.exit(1 if receipt["fails"] else 0)

if __name__ == "__main__":
    main()
