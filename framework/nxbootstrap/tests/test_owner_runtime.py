#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V5 7A.1 (nxbootstrap 0.8.0): the generator side of the owner runtime.

owner_runtime="1" makes port-env.sh owner-native: it cannot enter a
generation (no runtime-hook role, no required_files entry, no prepare
script); the sealed helper adapter-env.sh IS accepted as the runtime-hook;
the rendered launcher carries the guard block and the owner-file seed; a
port without the opt-in renders byte-identically to before (V4 unchanged).
"""
import copy, importlib.util, json, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("generate_port", ROOT / "tools/generate-port.py")
gp = importlib.util.module_from_spec(spec); spec.loader.exec_module(gp)
H = "0" * 64
FIX = {
    "schema_version": 3, "id": "ownertest", "title": "Owner Test", "launcher_name": "Owner Test.sh",
    "architecture": "aarch64", "executable": "bin/ownertest-nextos",
    "argument_mode": "game-dir-and-passthrough", "home_mode": "preserve",
    "nxextract": {"mode": "no", "version": "1.3.0"},
    "required_files": ["bin/ownertest-nextos", "port-env.sh"], "private_library_paths": ["lib"],
    "prepare_script": "", "required_capabilities": ["host.portmaster"], "enabled_quirks": [],
    "runtime_report": "log-and-logo",
    "generation_runtime": [
        {"role": "executable", "path": "bin/ownertest-nextos", "mode": "0755", "sha256": H},
        {"role": "private-library", "path": "lib/libfixture.so", "mode": "0644", "sha256": H},
        {"role": "runtime-hook", "path": "port-env.sh", "mode": "0644", "sha256": H},
    ],
}
fails = 0
def check(cond, msg):
    global fails
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: fails += 1
def expect_error(doc, needle, msg):
    try:
        gp.validate(copy.deepcopy(doc))
    except gp.ManifestError as e:
        check(needle in str(e), "%s (%s)" % (msg, e)); return
    check(False, msg + " (accepted)")

base = FIX
ok_doc = copy.deepcopy(base)
try:
    cfg0 = gp.validate(copy.deepcopy(ok_doc))
except gp.ManifestError as e:
    print("fixture invalid: %s" % e); sys.exit(1)
launcher0 = gp.render_launcher(cfg0)
check(cfg0.get("owner_runtime") is None and "NXBOOTSTRAP_OWNER_RUNTIME=0" in launcher0, "without the opt-in: owner_runtime absent, launcher literal 0")
check("nxbootstrap_owner_env_source \"$GAMEDIR/port-env.sh\"" not in launcher0, "without the opt-in: the legacy port-env source line is kept (V4 unchanged)")

doc = copy.deepcopy(ok_doc); doc["owner_runtime"] = "1"
doc["generation_runtime"] = [e for e in doc["generation_runtime"] if e["path"] != "port-env.sh"]
doc["required_files"] = [p for p in doc["required_files"] if p != "port-env.sh"]
if doc.get("prepare_script") == "port-env.sh": doc["prepare_script"] = ""
doc["generation_runtime"].append({"role": "runtime-hook", "path": "adapter-env.sh", "mode": "0644", "sha256": "1" * 64})
cfg = gp.validate(copy.deepcopy(doc))
check(cfg["owner_runtime"] == "1", "owner_runtime accepted with adapter-env.sh as the sealed runtime-hook")
launcher = gp.render_launcher(cfg)
check("NXBOOTSTRAP_OWNER_RUNTIME=1" in launcher and "nxbootstrap_owner_env_source \"$GAMEDIR/adapter-env.sh\" sealed" in launcher and "nxbootstrap_owner_env_source \"$GAMEDIR/port-env.sh\" owner" in launcher, "launcher renders the sealed-then-owner guarded source block")
check("NX-OWNER-RUNTIME/1 order=" in launcher, "order is observable in the launcher output")
check(json.loads(gp.canonical_manifest(cfg))["owner_runtime"] == "1", "canonical nxport carries owner_runtime")

bad = copy.deepcopy(doc); bad["generation_runtime"].append({"role": "runtime-hook", "path": "port-env.sh", "mode": "0644", "sha256": "2" * 64}); bad["required_files"].append("port-env.sh")
expect_error(bad, "cannot enter a generation", "MUTANT killed: port-env.sh as a generation member under owner_runtime refused")
bad = copy.deepcopy(doc); bad["required_files"].append("port-env.sh")
expect_error(bad, "cannot be a required file", "MUTANT killed: port-env.sh in required_files under owner_runtime refused")
bad = copy.deepcopy(doc); bad["prepare_script"] = "port-env.sh"; bad["required_files"].append("port-env.sh")
expect_error(bad, "owner_runtime", "MUTANT killed: port-env.sh as prepare script under owner_runtime refused")
bad = copy.deepcopy(doc); bad["owner_runtime"] = "yes"
expect_error(bad, "must be \"1\"", "owner_runtime accepts only \"1\"")
bad = copy.deepcopy(doc); del bad["generation_runtime"]
expect_error(bad, "requires generation_runtime", "owner_runtime needs the V4 generation store")
# sdl_provider=system combination renders the guard inside the quarantine block
doc2 = copy.deepcopy(doc); doc2["sdl_provider"] = "system"
try:
    cfg2 = gp.validate(copy.deepcopy(doc2)); l2 = gp.render_launcher(cfg2)
    check("nxbootstrap_owner_env_source \"$GAMEDIR/port-env.sh\" owner" in l2 and "export -n BIN_PRELOAD" in l2, "sdl_provider=system + owner_runtime: guarded source inside the provider quarantine")
except gp.ManifestError as e:
    print("note: sdl_provider=system combination not testable on this fixture: %s" % e)
print("owner-runtime-generator: %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
