#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxrelease 0.4.3 gate: the ON_DEVICE_AUTOMATED_INPUT_PROOF evidence class.

Hermetic. Proves:
  * normalize_input_proof accepts a canonical input_proof with or without
    evidence_class, keeps the class, and refuses HOST_FIXTURE or any other class;
  * the live-GPTK contract requires the on-device class for a port that
    declares controls.proof (static boundary on the validator);
  * nx-input-proof-lock.py builds a lock only from a passing on-device receipt
    whose ELF/adapter/default hashes match, refuses a failed receipt, a
    foreign class and an unobserved binding, and emits the class.
"""
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOOL = ROOT / "framework/nxrelease/nx-input-proof-lock.py"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


rel = load("nxrelease", ROOT / "framework/nxrelease/nxrelease.py")
fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", what)


def sha(b):
    return hashlib.sha256(b).hexdigest()


SYMBOLS = sorted(rel.INPUT_RUNTIME_REQUIRED_SYMBOLS | {"demo_sink_keyevent"})
BASE_PROOF = {
    "schema": rel.INPUT_PROOF_SCHEMA, "schema_version": 1, "run_id": "demo-1-2-3", "generation": "a" * 64,
    "port_id": "demo", "mapping_sha256": "b" * 64, "adapter_contract_sha256": "c" * 64, "verdict": "OK",
    "runtime": {"marker": rel.INPUT_RUNTIME_MARKER, "evidence_schema": rel.INPUT_PROOF_SCHEMA, "symbols": SYMBOLS},
    "contexts": [{"context": "gameplay", "source": "stage:player-alive", "observed": True},
                 {"context": "menu", "source": "stage:no-player", "observed": True}],
    "sinks": [{"sink": "adapter.input.keyevent", "symbol": "demo_sink_keyevent", "method": "runtime-action-registry",
               "targets": ["demo/demo-nextos"], "exists": True}],
    "cases": [{"context": "gameplay", "context_source": "stage:player-alive", "control": "B", "event": "press",
               "decision": "ACTION", "action": "g.jump", "sink": "adapter.input.keyevent", "delivery_count": 1}],
    "safety": {"unknown_context": {"result": "PASSTHROUGH", "suppressed": False, "delivery_count": 0},
               "missing_sink": {"result": "PASSTHROUGH", "suppressed": False, "delivery_count": 0},
               "failed_ack": {"result": "FATAL", "native_replay": False}},
}


def normalize(doc):
    return rel.normalize_input_proof(json.loads(json.dumps(doc)), "lock.input_proof")


legacy = normalize(BASE_PROOF)
check("evidence_class" not in legacy, "a legacy proof without class stays valid and class-less")
on_device = normalize({**BASE_PROOF, "evidence_class": rel.INPUT_PROOF_EVIDENCE_CLASS_ON_DEVICE})
check(on_device.get("evidence_class") == "ON_DEVICE_AUTOMATED_INPUT_PROOF", "class carried through normalization")
for bad in ("HOST_FIXTURE", "ON_DEVICE_HUMAN_WITNESS", "", 1):
    try:
        normalize({**BASE_PROOF, "evidence_class": bad})
        check(False, "class %r must be refused" % (bad,))
    except SystemExit:
        pass
    except Exception as e:  # nxrelease fail() may raise its own error type
        check("class" in str(e).lower() or "canonical" in str(e).lower(), "refusal message for %r: %s" % (bad, e))

src = (ROOT / "framework/nxrelease/nxrelease.py").read_text()
check('controls.get("proof")' in src and "requires %s or %s evidence in its candidate lock" in src,
      "live-GPTK contract demands the on-device class OR the explicit human live approval for ports with controls.proof")
# 0.4.6: HUMAN_LIVE_APPROVAL is accepted only WITH its approval block (who/when/device/what/log sha)
check(rel.INPUT_PROOF_EVIDENCE_CLASS_HUMAN in rel.INPUT_PROOF_EVIDENCE_CLASSES, "human live approval is a named class")
good_h = dict(BASE_PROOF, evidence_class=rel.INPUT_PROOF_EVIDENCE_CLASS_HUMAN, approval={
    "approved_by": "NextOS", "device": "dArkOS .137", "validated": "controls+prompts", "approved_at": "2026-09-04T01:00:00Z",
    "device_log_sha256": "0" * 64})
normalize(good_h)
try:
    normalize({**BASE_PROOF, "evidence_class": rel.INPUT_PROOF_EVIDENCE_CLASS_HUMAN})
    check(False, "MUTANT killed: human class without the approval block must be refused")
except (SystemExit, Exception) as e:
    check("approval" in str(e).lower(), "human class without approval refused: %s" % e)
try:
    normalize({**BASE_PROOF, "approval": good_h["approval"]})
    check(False, "MUTANT killed: approval block on the automated class must be refused")
except (SystemExit, Exception) as e:
    check("approval" in str(e).lower() or "canonical" in str(e).lower(), "approval block without the human class refused")
check(rel.TOOL_VERSION == "0.4.11", "version advanced")

# ---- converter: fixture receipt directory
with tempfile.TemporaryDirectory() as td:
    td = Path(td)
    elf = td / "demo-nextos"; elf.write_bytes(b"\x7fELFdemo")
    adapter = td / "adapter-contract.json"; adapter.write_text('{"input": {}}')
    defaults = td / "NEXTOSCONTROLLERS.gptk"; defaults.write_text("format = NEXTOS_CONTROLLERS/3\n[gameplay]\nB = g.jump\n")
    nxproject = td / "nxproject.json"
    nxproject.write_text(json.dumps({"nxport": {"id": "demo"}, "controls": {
        "actions": [{"id": "g.jump", "kind": "button", "sinks": ["adapter.input.keyevent"]},
                    {"id": "m.ok", "kind": "button", "sinks": ["adapter.input.keyevent"]}],
        "contexts": {"gameplay": {"B": "g.jump", "A": "native"}, "menu": {"A": "m.ok"}}}}))

    def receipt_dir(name, all_pass=True, classification="ON_DEVICE_AUTOMATED_INPUT_PROOF", elf_sha=None, deliveries=None,
                    gptk_sha=None, promoted=True, chord_exit=True):
        d = td / name; d.mkdir()
        gsha = gptk_sha or sha(defaults.read_bytes())
        (d / "receipt.json").write_text(json.dumps({
            "schema": "nx-device-input-proof/1", "classification": classification, "all_pass": all_pass, "port_id": "demo",
            "executable": {"sha256": elf_sha or sha(elf.read_bytes())}, "adapter_contract_sha256": sha(adapter.read_bytes()),
            "gptk": {"default_sha256": gsha, "loaded_sha256": gsha[:16] + "..."}}))
        log = "VIDEO-PROOF: verdict=OK reason=non-black receipt=published\n"
        if chord_exit:
            log += "[demo/input] SELECT+START: lifecycle exit requested\n"
        log += "PHASE runtime EXIT status=0\n"
        if promoted:
            log += "UPDATE NXU0006: generation %s proved healthy clean_exit_run=demo-1-2-3\n" % ("a" * 64)
        (d / "log.txt").write_text(log)
        lines = deliveries if deliveries is not None else [
            {"context": "gameplay", "context_source": "stage:player-alive", "control": "B", "event": "press", "action": "g.jump",
             "sink": "adapter.input.keyevent", "pressed": 1},
            {"context": "menu", "context_source": "stage:no-player", "control": "A", "event": "press", "action": "m.ok",
             "sink": "adapter.input.keyevent", "pressed": 1}]
        (d / "nxgptk-receipt.jsonl").write_text("".join(json.dumps({"schema": rel.INPUT_PROOF_SCHEMA, "kind": "delivery", **l}) + "\n" for l in lines))
        return d

    def run(*dirs, out="lock.json"):
        cmd = [sys.executable, str(TOOL)] + sum([["--receipt", str(d)] for d in dirs], []) + [
            "--elf", str(elf), "--nxproject", str(nxproject), "--adapter", str(adapter), "--defaults", str(defaults),
            "--executable", "demo/demo-nextos", "--sink-symbol", "adapter.input.keyevent=demo_sink_keyevent", "--out", str(td / out)]
        return subprocess.run(cmd, capture_output=True, text=True)

    good = receipt_dir("good")
    p = run(good)
    check(p.returncode == 0, "converter builds a lock from a passing on-device receipt: %s" % p.stderr.strip()[-200:])
    lock = json.loads((td / "lock.json").read_text())
    check(lock["input_proof"]["evidence_class"] == "ON_DEVICE_AUTOMATED_INPUT_PROOF", "lock carries the class")
    check(lock["sha256"] == sha(elf.read_bytes()) and len(lock["input_proof"]["cases"]) == 2, "lock binds the ELF and covers both bindings")
    check((os.stat(td / "lock.json").st_mode & 0o777) == 0o444, "lock is frozen read-only")
    normalized = rel.normalize_candidate_lock(json.loads(json.dumps(lock)), "lock", sha((td / "lock.json").read_bytes()))
    check(normalized["input_proof"]["evidence_class"] == "ON_DEVICE_AUTOMATED_INPUT_PROOF", "nxrelease normalizes the converter's lock")
    check(run(receipt_dir("failed", all_pass=False), out="l2.json").returncode == 2, "a failed proof never becomes a lock")
    check(run(receipt_dir("host", classification="HOST_FIXTURE"), out="l3.json").returncode == 2, "HOST_FIXTURE receipt refused")
    check(run(receipt_dir("otherelf", elf_sha="0" * 64), out="l4.json").returncode == 2, "receipt of another ELF refused")
    check(run(receipt_dir("partial", deliveries=[]), out="l5.json").returncode == 2, "unobserved binding refused")
    check(run(receipt_dir("stale", promoted=False), out="l6.json").returncode == 2, "stale run without the healthy promotion refused")
    check(run(receipt_dir("gptkswap", gptk_sha="f" * 64), out="l7.json").returncode == 2, "receipt of a swapped GPTK default refused")
    check(run(receipt_dir("sigterm", chord_exit=False), out="l8.json").returncode == 2, "an exit not attributed to SELECT+START (external signal) refused")
    # renamed/symlinked receipt directory: content decides, not the name
    import os as _os
    link = td / "linked"; _os.symlink(td / "good", link)
    check(run(link, out="l9.json").returncode == 0, "a symlinked receipt directory is read by content")

if fails:
    print("nxrelease on-device input proof class gate: FAIL (%d)" % fails)
    sys.exit(1)
print("nxrelease on-device input proof class gate: PASS — class normalization, refusal of foreign classes, contract requirement, converter positive/negatives (failed, foreign class, other ELF, unobserved binding, stale run, swapped GPTK, non-chord exit)")
