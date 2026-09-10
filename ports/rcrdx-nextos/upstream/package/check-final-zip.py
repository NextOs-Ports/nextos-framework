#!/usr/bin/env python3
"""Prove that a final ZIP contains the current reviewed source bytes."""

import hashlib
import json
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


if len(sys.argv) != 2:
    raise SystemExit("usage: check-final-zip.py ARCHIVE")

archive = Path(sys.argv[1]).resolve()
manifest = json.loads((ROOT / "nxrelease.json").read_text(encoding="utf-8"))
with zipfile.ZipFile(archive) as package:
    names = set(package.namelist())
    for record in manifest["files"]:
        source = ROOT / record["source"]
        target = record["target"]
        if target not in names:
            raise SystemExit(f"final ZIP is missing current target: {target}")
        source_hash = sha256(source.read_bytes())
        archive_hash = sha256(package.read(target))
        if source_hash != record["sha256"]:
            raise SystemExit(f"stale source pin in nxrelease.json: {record['source']}")
        if archive_hash != source_hash:
            raise SystemExit(f"final ZIP contains stale bytes: {target}")

    runtime = package.read("rcrdx/rcrdx-nextos")
    for witness in (
        b"retrying portable EGL/GLES provider names",
        b"recovered with portable EGL/GLES provider names",
        b"the game has exited with status %d",
    ):
        if witness not in runtime:
            raise SystemExit("final runtime lacks required witness: " + witness.decode())

print(f"RCRDX exact-source ZIP gate: PASS files={len(manifest['files'])}")
