#!/usr/bin/env python3
"""Reject download-source names from every public text file in the release."""

import json
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = json.loads((ROOT / "nxrelease.json").read_text(encoding="utf-8"))
FORBIDDEN = ("apkpure", "apkmirror", "apkvision", "5play", "apkcombo", "uptodown")
TEXT_SUFFIXES = {".json", ".md", ".txt"}

checked = 0
for entry in MANIFEST.get("files", []):
    source = ROOT / entry["source"]
    if source.suffix.lower() not in TEXT_SUFFIXES:
        continue
    text = source.read_text(encoding="utf-8").lower()
    for token in FORBIDDEN:
        if token in text:
            raise SystemExit(
                f"public docs gate: forbidden download source {token!r} in {source}"
            )
    checked += 1

if checked == 0:
    raise SystemExit("public docs gate: no public text files were checked")
print(f"public docs gate: PASS files={checked}")
