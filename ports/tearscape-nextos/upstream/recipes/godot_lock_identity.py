#!/usr/bin/env python3
"""Add the Godot runtime identity to an ON_DEVICE candidate lock.

`nxrelease/nx-input-proof-lock.py` (nxrelease 0.3.26) builds the
`nxrelease-candidate-lock-v1` from the on-device receipts but knows nothing
about Godot engines. `nxrelease bundle` requires a Godot ELF (one that carries
`nxinput-godot-runtime/1` + `nxgl-godot-frame-proof/2`) to be matched by a
proof whose runtime names both markers and the frame-proof symbols. Nothing
here is invented: every marker must be present in the ELF bytes AND observed
in the launcher log of every receipt run, every symbol must be defined in the
ELF, and the lock's ELF hash must be the ELF given. The result is a NEW lock
file (the input is never rewritten) with the receipt hash recomputed the same
canonical way nx-input-proof-lock does.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

GODOT_RUNTIME_MARKER = "nxinput-godot-runtime/1"
FRAME_PROOF_MARKER = "nxgl-godot-frame-proof/2"
FRAME_PROOF_SYMBOLS = ("nxgl_frame_proof_is_fatal", "nxgl_frame_proof_consume_fatal")
LOG_GODOT = re.compile(r"godot_runtime=" + re.escape(GODOT_RUNTIME_MARKER) + r"\b")
LOG_FRAME = re.compile(r"frame-proof runtime=" + re.escape(FRAME_PROOF_MARKER) + r"\b")


def fail(message):
    print("godot-lock-identity: %s" % message, file=sys.stderr)
    sys.exit(2)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def defined_symbols(elf):
    out = subprocess.run(["readelf", "-sW", str(elf)], check=True, capture_output=True, text=True).stdout
    symbols = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[6] != "UND" and parts[3] in ("FUNC", "OBJECT", "IFUNC"):
            symbols.add(parts[7].split("@")[0])
    return symbols


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--lock", required=True, help="candidate lock written by nx-input-proof-lock")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--receipt", action="append", required=True, help="receipt directory used for the lock (repeatable)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    lock = json.loads(Path(a.lock).read_text(encoding="utf-8"))
    if lock.get("schema") != "nxrelease-candidate-lock-v1":
        fail("not an nxrelease-candidate-lock-v1")
    elf = Path(a.elf)
    elf_sha = sha256_file(elf)
    if lock.get("sha256") != elf_sha:
        fail("lock ELF %s differs from %s" % (lock.get("sha256"), elf_sha))
    elf_bytes = elf.read_bytes()
    for marker in (GODOT_RUNTIME_MARKER, FRAME_PROOF_MARKER):
        if marker.encode("ascii") not in elf_bytes:
            fail("ELF lacks the %s identity; this is not a Godot runtime ELF" % marker)
    defined = defined_symbols(elf)
    for symbol in FRAME_PROOF_SYMBOLS:
        if symbol not in defined:
            fail("ELF does not define %s" % symbol)
    for receipt_dir in a.receipt:
        log = (Path(receipt_dir) / "log.txt").read_text(encoding="utf-8", errors="replace")
        if not LOG_GODOT.search(log):
            fail("%s: the run never logged godot_runtime=%s" % (receipt_dir, GODOT_RUNTIME_MARKER))
        if not LOG_FRAME.search(log):
            fail("%s: the run never logged frame-proof runtime=%s" % (receipt_dir, FRAME_PROOF_MARKER))
    proof = lock["input_proof"]
    runtime = proof["runtime"]
    if runtime.get("godot_marker") not in (None, GODOT_RUNTIME_MARKER) or \
            runtime.get("frame_proof_marker") not in (None, FRAME_PROOF_MARKER):
        fail("the lock already names a different Godot identity")
    runtime["godot_marker"] = GODOT_RUNTIME_MARKER
    runtime["frame_proof_marker"] = FRAME_PROOF_MARKER
    symbols = set(runtime.get("symbols") or [])
    missing = sorted(s for s in symbols if s not in defined)
    if missing:
        fail("lock runtime symbols not defined in the ELF: %s" % ", ".join(missing))
    symbols.update(FRAME_PROOF_SYMBOLS)
    runtime["symbols"] = sorted(symbols)
    input_line = (json.dumps(proof, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n").encode()
    lock["input_proof_receipt_sha256"] = hashlib.sha256(input_line).hexdigest()
    out = Path(a.out)
    if out.exists():
        fail("refusing to overwrite %s" % out)
    out.write_text(json.dumps(lock, indent=2, sort_keys=True, ensure_ascii=False) + "\n")
    out.chmod(0o444)
    print("godot-lock-identity: PASS elf=%s markers=%s,%s symbols=%d receipts=%d -> %s" % (
        elf_sha[:16], GODOT_RUNTIME_MARKER, FRAME_PROOF_MARKER, len(runtime["symbols"]), len(a.receipt), out))


if __name__ == "__main__":
    main()
