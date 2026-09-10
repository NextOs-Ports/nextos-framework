#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxrelease 0.4.3 / V5 FV6: owner-runtime and NEXTOS_SETTINGS/2 gates, pure.

Drives `validate_owner_runtime_contract` and the settings block of
`validate_generation_receipt` with synthetic records so every mission
mutant is exercised without a full stage: live owner packaged, port-env.sh
in the closure, missing seed, `.new` shipped, owner-native path healed,
video keys under /1, seed diverging from the declared defaults.
"""
import importlib.util, json, os, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nxrelease_owner_test", ROOT / "nxrelease.py")
nx = importlib.util.module_from_spec(spec); spec.loader.exec_module(nx)
fails = 0
def check(c, m):
    global fails
    print(("ok   " if c else "FAIL ") + m)
    if not c: fails += 1
PORT = "fx"
work = Path(tempfile.mkdtemp(prefix="nxrelease-owner-"))
def payload(rel, text, mode=0o644):
    p = work / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text); os.chmod(p, mode)
    return {"target": PORT + "/" + rel, "kind": "payload", "mode": mode, "actual_path": p, "sha256": "x"}
def gate(records, nxport, expect=None, label=""):
    config = {"port_dir": PORT, "nxport_manifest": nxport}
    try:
        nx.validate_owner_runtime_contract(records, config)
    except (SystemExit, nx.ReleaseError) as e:
        msg = str(e)
        check(expect is not None and expect.lower() in msg.lower(), "%s -> refused: %s" % (label, msg)); return
    check(expect is None, "%s -> accepted" % label)
ownership = {"schema": "nx-ownership/1", "paths": [
    {"path": "defaults/NEXTOSSETTINGS.txt", "class": "owner-seeded", "live": "NEXTOSSETTINGS.txt", "healed": False},
    {"path": "defaults/port-env.sh", "class": "owner-seeded", "live": "port-env.sh", "healed": False},
    {"path": "adapter-env.sh", "class": "sealed-runtime", "live": None, "healed": True},
    {"path": "game/override.cfg", "class": "owner-native", "live": "game/override.cfg", "healed": False, "authority": "engine"},
]}
def base_records(own=ownership):
    return [payload("defaults/port-env.sh", "export NX_VIDEO_ASPECT=auto\n"),
            payload("adapter-env.sh", "[ -f \"$GAMEDIR/lib/libx.so\" ] && export LIBX=1\n"),
            payload("lib/libx.so", "elf"),
            payload("OWNERSHIP.json", json.dumps(own))]
nxport_v5 = {"owner_runtime": "1", "required_files": ["bin/x"], "generation_runtime": [
    {"role": "executable", "path": "bin/x", "mode": "0755", "sha256": "0" * 64},
    {"role": "runtime-hook", "path": "adapter-env.sh", "mode": "0644", "sha256": "0" * 64}]}
nxport_v4 = {"required_files": ["bin/x", "port-env.sh"], "generation_runtime": [
    {"role": "executable", "path": "bin/x", "mode": "0755", "sha256": "0" * 64},
    {"role": "runtime-hook", "path": "port-env.sh", "mode": "0644", "sha256": "0" * 64}]}

gate(base_records(), nxport_v5, None, "V5 opt-in, seed + sealed helper + ownership")
gate([payload("port-env.sh", "x")], nxport_v4, None, "V4 port (no opt-in, live hook in closure) unchanged")
gate(base_records() + [payload("port-env.sh", "live")], nxport_v5, "must not be packaged", "MUTANT: live owner hook packaged")
r = base_records(); nxport_bad = json.loads(json.dumps(nxport_v5)); nxport_bad["generation_runtime"].append({"role": "runtime-hook", "path": "port-env.sh", "mode": "0644", "sha256": "0" * 64})
gate(r, nxport_bad, "would be healed", "MUTANT: port-env.sh inside the healing closure")
gate([x for x in base_records() if not x["target"].endswith("defaults/port-env.sh")], nxport_v5, "defaults/port-env.sh", "MUTANT: seed missing")
gate(base_records() + [payload("port-env.sh.new", "n")], nxport_v5, ".new", "MUTANT: .new shipped in the package")
nxport_noseal = json.loads(json.dumps(nxport_v5)); nxport_noseal["generation_runtime"] = nxport_noseal["generation_runtime"][:1]
gate(base_records(), nxport_noseal, "adapter-env.sh", "MUTANT: sealed helper not a generation member")
own2 = json.loads(json.dumps(ownership)); nxport_native = json.loads(json.dumps(nxport_v5)); nxport_native["generation_runtime"].append({"role": "runtime-data", "path": "game/override.cfg", "mode": "0644", "sha256": "0" * 64})
gate(base_records(own2), nxport_native, "generation closure", "MUTANT: owner-native config healed (in the closure)")
gate([x for x in base_records() if not x["target"].endswith("OWNERSHIP.json")], nxport_v5, "OWNERSHIP.json", "MUTANT: ownership manifest missing")
gate(base_records() + [payload("NEXTOSSETTINGS.txt", "# NEXTOS_SETTINGS/2\n")], nxport_v5, "must not be packaged", "MUTANT: live typed owner settings packaged")
r = base_records(); r[1] = payload("adapter-env.sh", "[ -f \"$GAMEDIR/lib/missing.so\" ] && export LIBX=1\n")
gate(r, nxport_v5, "not staged", "sealed helper references an unstaged file (KOTOR lesson)")
own3 = json.loads(json.dumps(ownership)); own3["paths"][1]["healed"] = True
gate(base_records(own3), nxport_v5, "healed=false", "MUTANT: owner path declared healed")

# settings /2 mirror
check(nx.settings_video_value_ok("video.aspect", "preserve") and not nx.settings_video_value_ok("video.aspect", "keep") and nx.settings_video_value_ok("video.output_size", "800x600") and not nx.settings_video_value_ok("video.output_size", "0x600") and not nx.settings_video_value_ok("video.gamma", "1"), "settings /2 enum mirror agrees with nxcompat")
print("nxrelease-owner-runtime: %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
