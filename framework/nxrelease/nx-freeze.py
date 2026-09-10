#!/usr/bin/env python3
"""Freeze the framework combination and derive the affected ports.

FRAMEWORK-LOCK.json and AFFECTED-PORTS.json exist so a release can say exactly
which component versions and which port packages it stands on. Both were being
written by hand, and both drifted the same day: the lock still named versions
that had been bumped hours earlier, and the affected list carried package hashes
that no longer matched any file. A freeze that is typed is not a freeze.

This derives both. Component versions come from each VERSION file, hashes from
the files themselves, and the affected-port list from the packages that were
actually built and, when a proof directory is given, the device proofs that
were actually measured. Nothing here is invented.

Usage:
  nx-freeze.py --framework-root DIR --out DIR
               [--port NAME=ARCHIVE]... [--proof-json FILE]
"""

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_version(root: Path, component: str) -> str | None:
    p = root / component / "VERSION"
    return p.read_text(encoding="utf-8").strip() if p.is_file() else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--framework-root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", action="append", default=[],
                    help="NAME=path/to/archive.zip (repeatable)")
    ap.add_argument("--proof-json",
                    help="TEST-RUNS.jsonl with measured device proofs")
    args = ap.parse_args()

    root = Path(args.framework_root).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    now = datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")

    # Every component that carries a VERSION file is part of the combination,
    # with the files a release actually depends on pinned by hash.
    pinned_files = {
        "nxbootstrap": ["tools/generate-port.py", "templates/launcher.sh.in"],
        "nxrelease": ["nxrelease.py", "nx-refresh-pins.py", "nx-ship-port.sh"],
        "nxobs": ["nx-device-launch.sh"],
        "nxgl": ["include/nxgl.h", "adapters/nxgl_frame_proof_adapter.c"],
        "nxsplash": [],
    }
    components = {}
    for comp_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        version = read_version(root, comp_dir.name)
        if version is None:
            continue
        entry = {"version": version,
                 "version_sha256": sha256_of(comp_dir / "VERSION")}
        for rel in pinned_files.get(comp_dir.name, []):
            f = comp_dir / rel
            if f.is_file():
                entry[rel.replace("/", "_").replace(".", "_") + "_sha256"] = \
                    sha256_of(f)
        components[comp_dir.name] = entry

    contract = root / "contracts" / "declarative-v1.json"
    lock = {
        "schema_version": 2,
        "timestamp": now,
        "framework_version": f"{components.get('nxbootstrap', {}).get('version', '?')}-definitivo",
        "components": components,
        "declarative_contract_sha256": sha256_of(contract) if contract.is_file() else None,
    }
    (out / "FRAMEWORK-LOCK.json").write_text(
        json.dumps(lock, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    # Affected ports: only what was built, with the hash of the file as it is.
    proofs = {}
    if args.proof_json and Path(args.proof_json).is_file():
        for line in Path(args.proof_json).read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("frame_proof") == "OK" and rec.get("conclusive"):
                proofs.setdefault(rec.get("port"), []).append({
                    "device": rec.get("device"),
                    "best_non_black_pct": rec.get("best_non_black_pct"),
                })

    ports = []
    for spec in args.port:
        name, _, archive = spec.partition("=")
        p = Path(archive)
        if not p.is_file():
            print(f"nx-freeze: archive missing for {name}: {archive}",
                  file=sys.stderr)
            return 1
        ports.append({
            "id": name,
            "package": p.name,
            "bytes": p.stat().st_size,
            "sha256": sha256_of(p),
            "device_proofs": proofs.get(name, []),
            "proven": bool(proofs.get(name)),
        })
    affected = {"schema_version": 2, "timestamp": now,
                "derived_from": "packages built by nx-ship-port and proofs in TEST-RUNS.jsonl",
                "ports": ports}
    (out / "AFFECTED-PORTS.json").write_text(
        json.dumps(affected, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(f"nx-freeze: {len(components)} components locked, "
          f"{len(ports)} ports listed, "
          f"{sum(1 for p in ports if p['proven'])} proven")
    return 0


if __name__ == "__main__":
    sys.exit(main())
