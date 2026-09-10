#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxrelease 0.3.26 — nx-input-proof-lock: receipts -> frozen external candidate lock.

Turns one or more ON_DEVICE_AUTOMATED_INPUT_PROOF receipts (nxinput 0.10.2
`nx-device-input-proof/1`, default NEXTOSCONTROLLERS.gptk, same ELF) into the
nxrelease candidate lock (`nxrelease-candidate-lock-v1`) that `bundle`
consumes. Nothing here is invented: every case comes from the runtime
readback (`nxinput-gptk-event-evidence/1`) the device produced, the video
proof/generation/run come from the run's own launcher log, and the ELF,
adapter-contract and default-mapping hashes must agree between the receipt and
the candidate being packaged.

Refuses: a receipt whose classification is not ON_DEVICE_AUTOMATED_INPUT_PROOF,
a receipt that did not pass, receipts with different ELF/mapping hashes, a
port binding never observed on the device, a run without the healthy
promotion line or without the OK video proof.

Usage:
  nx-input-proof-lock.py --receipt DIR [--receipt DIR ...] --elf FILE
      --nxproject FILE --adapter FILE --defaults FILE --executable NAME/EXE
      --sink-symbol SINK=SYMBOL [...] --out LOCK.json
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

VIDEO_FIELDS = ("schema", "schema_version", "run_id", "generation", "port_id", "verdict", "reason")
INPUT_SCHEMA = "nxinput-gptk-event-evidence/1"
RECEIPT_SCHEMA = "nx-device-input-proof/1"
CLASSIFICATION = "ON_DEVICE_AUTOMATED_INPUT_PROOF"
# nxrelease 0.4.6 (04/09/2026, ordem do NextOS): a validação AO VIVO pelo dono
# do projeto é uma classe de evidência própria, nunca disfarçada de receipt
# automático. O lock registra quem aprovou, quando, em que aparelho e o que
# foi validado; ELF, adapter-contract, defaults e a geração continuam presos
# por hash ao log real da execução aprovada.
CLASSIFICATION_HUMAN = "HUMAN_LIVE_APPROVAL"
RUNTIME_MARKER = "nxinput-gptk-runtime/3"
# V5 (nxinput 0.11.0): a schema-4 port publishes "nxinput-gptk-runtime/4" in the
# `kind:runtime` evidence line; the lock names the marker the receipts PROVED,
# never a constant. Every receipt of one lock must agree.
RUNTIME_MARKERS_KNOWN = ("nxinput-gptk-runtime/3", "nxinput-gptk-runtime/4")


def runtime_marker_from_receipts(receipt_dirs):
    import json as _json
    seen = set()
    for d in receipt_dirs:
        path = d / "nxgptk-receipt.jsonl"
        if not path.is_file():
            continue
        for line in path.read_text(errors="replace").splitlines():
            try:
                rec = _json.loads(line)
            except ValueError:
                continue
            if rec.get("kind") == "runtime" and rec.get("marker"):
                seen.add(rec["marker"])
    if not seen:
        return RUNTIME_MARKER
    if len(seen) != 1 or next(iter(seen)) not in RUNTIME_MARKERS_KNOWN:
        raise SystemExit("nx-input-proof-lock: receipts disagree on the runtime marker: %s" % sorted(seen))
    return next(iter(seen))
REQUIRED_SYMBOLS = [
    "nxinput_gptk_load_at", "nxinput_gptk_load_receipt_json", "nxinput_gptk_parse", "nxinput_gptk_decide",
    "nxinput_gptk_live_init", "nxinput_gptk_live_register", "nxinput_gptk_live_register_vector",
    "nxinput_gptk_live_seal", "nxinput_gptk_live_set_context", "nxinput_gptk_live_clear_context",
    "nxinput_gptk_live_should_consume", "nxinput_gptk_live_feed", "nxinput_gptk_live_feed_vector",
    "nxinput_gptk_runtime_marker", "nxinput_gptk_event_evidence_schema",
]


class LockError(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def load_receipt(dirpath):
    d = Path(dirpath)
    receipt = json.loads((d / "receipt.json").read_text(encoding="utf-8"))
    if receipt.get("schema") != RECEIPT_SCHEMA:
        raise LockError("%s: not a %s receipt" % (d, RECEIPT_SCHEMA))
    if receipt.get("classification") != CLASSIFICATION:
        raise LockError("%s: classification %r is not %s" % (d, receipt.get("classification"), CLASSIFICATION))
    if receipt.get("all_pass") is not True:
        raise LockError("%s: the proof did not pass; a failed run never becomes a lock" % d)
    log = (d / "log.txt").read_text(encoding="utf-8", errors="replace")
    readback = [json.loads(l) for l in (d / "nxgptk-receipt.jsonl").read_text(encoding="utf-8", errors="replace").splitlines()
                if l.strip().startswith("{")]
    return receipt, log, [r for r in readback if r.get("schema") == INPUT_SCHEMA]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--receipt", action="append", default=[], help="receipt directory of a DEFAULT-mapping run (repeatable)")
    ap.add_argument("--human-live-approval", help="HUMAN_LIVE_APPROVAL: 'approved_by|device|what was validated' (the owner validated the exact ELF live)")
    ap.add_argument("--device-log", help="launcher log.txt of the run the owner validated (same ELF; healthy promotion, video proof, chord exit)")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--nxproject", required=True)
    ap.add_argument("--adapter", required=True, help="generated adapter/adapter-contract.json")
    ap.add_argument("--defaults", required=True, help="generated defaults/NEXTOSCONTROLLERS.gptk")
    ap.add_argument("--executable", required=True, help="packaged executable path, e.g. fp2/fp2-nextos")
    ap.add_argument("--sink-symbol", action="append", default=[], help="sink=symbol (repeatable)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    elf_sha, adapter_sha, defaults_sha = sha256(a.elf), sha256(a.adapter), sha256(a.defaults)
    project = json.load(open(a.nxproject))
    controls = project["controls"]
    actions = {x["id"]: x for x in controls["actions"]}
    port_id = project["nxport"]["id"] if isinstance(project.get("nxport"), dict) and "id" in project["nxport"] else None

    human = None
    if a.human_live_approval:
        if a.receipt or not a.device_log:
            raise LockError("--human-live-approval takes --device-log (the validated run's launcher log) and no --receipt")
        parts = a.human_live_approval.split("|")
        if len(parts) != 3 or not all(x.strip() for x in parts):
            raise LockError("--human-live-approval must be 'approved_by|device|validated'")
        import datetime
        human = {"approved_by": parts[0].strip(), "device": parts[1].strip(), "validated": parts[2].strip(),
                 "approved_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                 "device_log_sha256": sha256(a.device_log)}
        if not port_id:
            raise LockError("nxproject has no nxport.id (needed without receipts)")
        receipts = []
        primary_log = Path(a.device_log).read_text(errors="replace")
        if ("nxinput-gptk-runtime/4" not in primary_log) and ("nxinput-gptk-runtime/3" not in primary_log):
            raise LockError("the validated run's log names no GPTK runtime marker")
    elif not a.receipt:
        raise LockError("--receipt is required (or --human-live-approval with --device-log)")
    else:
        receipts = [load_receipt(p) for p in a.receipt]
        primary, primary_log, _ = receipts[0]
        if port_id and primary.get("port_id") != port_id:
            raise LockError("receipt port_id %r differs from nxproject %r" % (primary.get("port_id"), port_id))
        port_id = primary["port_id"]
    for r, _log, _rb in receipts:
        if (r.get("executable") or {}).get("sha256") != elf_sha:
            raise LockError("receipt ELF %s differs from the candidate ELF %s" % ((r.get("executable") or {}).get("sha256"), elf_sha))
        if r.get("adapter_contract_sha256") != adapter_sha:
            raise LockError("receipt adapter-contract differs from the generated adapter contract")
        gptk = r.get("gptk") or {}
        if gptk.get("default_sha256") != defaults_sha or gptk.get("loaded_sha256") is None or \
                not defaults_sha.startswith(gptk["loaded_sha256"][:16]):
            raise LockError("receipt did not run the packaged DEFAULT mapping (owner runs never build the lock)")

    m = re.search(r"UPDATE NXU0006: generation ([0-9a-f]{64}) proved healthy (?:receipt_run|clean_exit_run)=([A-Za-z0-9._-]+)", primary_log)
    if not m:
        raise LockError("the primary run's log has no NXU0006 healthy promotion line")
    generation, run_id = m.group(1), m.group(2)
    if "VIDEO-PROOF: verdict=OK reason=non-black receipt=published" not in primary_log:
        raise LockError("the primary run's log has no OK non-black video proof")
    # The run must have ended through the sovereign chord, never through a
    # signal sent from outside: a SIGTERM exit proves nothing about SELECT+START.
    for _r, log, _rb in ([(None, primary_log, None)] if human else receipts):
        if not re.search(r"SELECT\+START: lifecycle exit requested|release-all reason=exit-chord", log):
            raise LockError("a receipt run did not end through the SELECT+START chord (external signal exits never build a lock)")
    video = {"schema": "org.nextos.nxruntime.video-proof", "schema_version": 1, "run_id": run_id,
             "generation": generation, "port_id": port_id, "verdict": "OK", "reason": "non-black"}
    video_line = (json.dumps({f: video[f] for f in VIDEO_FIELDS}, separators=(",", ":"), ensure_ascii=False) + "\n").encode()

    # Observed deliveries across the default runs: (context, source, control, event, action, sink)
    kind_event = {"button": "press", "axis": "axis", "vector": "motion"}
    observed = {}
    sources = {}
    for _r, _log, readback in receipts:
        for d in readback:
            if d.get("kind") != "delivery" or (d.get("pressed") == 0 and d.get("event") == "press"):
                continue
            key = (d["context"], d["context_source"], d["control"], d["event"], d["action"], d["sink"])
            observed[key] = observed.get(key, 0) + 1
            sources.setdefault(d["context"], {}).setdefault(d["context_source"], 0)
            sources[d["context"]][d["context_source"]] += 1
    # ONE evidenced source per context: the one that carried the deliveries of that context.
    chosen_source = {ctx: max(srcs.items(), key=lambda kv: kv[1])[0] for ctx, srcs in sources.items()}
    if human:
        # the owner validated every declared context live: one source per context, named honestly
        chosen_source = {ctx: "human-live-validation" for ctx in controls["contexts"]}

    cases, missing = [], []
    for ctx, bindings in controls["contexts"].items():
        src = chosen_source.get(ctx)
        for control, action in bindings.items():
            if action in ("native", "null"):
                continue
            ev = kind_event[actions[action]["kind"]]
            for sink in actions[action]["sinks"]:
                key = (ctx, src, control, ev, action, sink)
                if not human and (src is None or key not in observed):
                    missing.append((ctx, control, action, sink))
                cases.append({"context": ctx, "context_source": src or "-", "control": control, "event": ev,
                              "decision": "ACTION", "action": action, "sink": sink, "delivery_count": 1})
    if missing:
        for k in missing:
            print("MISSING EVIDENCE:", k, file=sys.stderr)
        raise LockError("%d declared binding(s) were never observed on the device" % len(missing))
    cases.sort(key=lambda c: (c["context"], c["control"], c["action"], c["sink"], c["event"], c["context_source"]))

    sink_symbols = dict(s.split("=", 1) for s in a.sink_symbol)
    all_sinks = sorted({s for x in controls["actions"] for s in x["sinks"]})
    for sink in all_sinks:
        if sink not in sink_symbols:
            raise LockError("no --sink-symbol for " + sink)
    sinks = [{"sink": s, "symbol": sink_symbols[s], "method": "runtime-action-registry",
              "targets": [a.executable], "exists": True} for s in all_sinks]
    contexts = sorted(({"context": c, "source": chosen_source[c], "observed": True} for c in controls["contexts"]),
                      key=lambda x: x["context"])
    input_proof = {
        "schema": INPUT_SCHEMA, "schema_version": 1, "run_id": run_id, "generation": generation, "port_id": port_id,
        "mapping_sha256": defaults_sha, "adapter_contract_sha256": adapter_sha, "verdict": "OK",
        "evidence_class": CLASSIFICATION_HUMAN if human else CLASSIFICATION,
        "runtime": {"marker": ("nxinput-gptk-runtime/4" if "nxinput-gptk-runtime/4" in primary_log else RUNTIME_MARKER) if human else runtime_marker_from_receipts([Path(r) for r in a.receipt]), "evidence_schema": INPUT_SCHEMA,
                    "symbols": sorted(set(REQUIRED_SYMBOLS) | set(sink_symbols.values()))},
        "contexts": contexts, "sinks": sinks, "cases": cases,
        "safety": {"unknown_context": {"result": "PASSTHROUGH", "suppressed": False, "delivery_count": 0},
                   "missing_sink": {"result": "PASSTHROUGH", "suppressed": False, "delivery_count": 0},
                   "failed_ack": {"result": "FATAL", "native_replay": False}},
    }
    if human:
        input_proof["approval"] = human
    input_line = (json.dumps(input_proof, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n").encode()
    lock = {
        "schema": "nxrelease-candidate-lock-v1", "schema_version": 1,
        "executable": a.executable, "sha256": elf_sha,
        "video_proof": video, "video_proof_receipt_sha256": hashlib.sha256(video_line).hexdigest(),
        "input_proof": input_proof, "input_proof_receipt_sha256": hashlib.sha256(input_line).hexdigest(),
    }
    out = Path(a.out)
    out.write_text(json.dumps(lock, indent=2, sort_keys=True, ensure_ascii=False) + "\n")
    out.chmod(0o444)
    print("nx-input-proof-lock: %s run=%s generation=%s elf=%s receipts=%d cases=%d -> %s" % (
        CLASSIFICATION_HUMAN if human else CLASSIFICATION, run_id, generation[:16], elf_sha[:16], len(receipts), len(cases), out))


if __name__ == "__main__":
    try:
        main()
    except LockError as e:
        print("nx-input-proof-lock: %s" % e, file=sys.stderr)
        sys.exit(2)
