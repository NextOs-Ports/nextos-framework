#!/usr/bin/env python3
"""Refresh only the artifact identities in the conservative generator receipt."""

import hashlib
import json
import stat
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RECEIPT = ROOT / "GENERATION.json"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


document = json.loads(RECEIPT.read_text(encoding="utf-8"))
for record in document["artifacts"]:
    relative = record["path"]
    if relative == "Retro City Rampage DX.sh":
        path = ROOT / relative
    elif relative.startswith("rcrdx/"):
        path = ROOT / relative.removeprefix("rcrdx/")
    else:
        raise SystemExit("unexpected generated artifact: " + relative)
    if path.is_symlink() or not path.is_file():
        raise SystemExit("missing generated artifact: " + str(path))
    record["sha256"] = digest(path)
    record["mode"] = "%04o" % stat.S_IMODE(path.stat().st_mode)

project = ROOT / "nxproject.json"
document["project_manifest_sha256"] = digest(project)
document["source_pins"]["nxextract"]["recipe_sha256"] = digest(
    ROOT / "extractor.json"
)
RECEIPT.write_text(
    json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
)
print("refreshed conservative GENERATION.json inventory")
