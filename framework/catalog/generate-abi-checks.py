#!/usr/bin/env python3
"""Expand each explicitly applicable ABI variant into exactly 28 decisions."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


MISSING = object()


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def lookup(value: Any, dotted_path: str) -> Any:
    current = value
    for component in dotted_path.split("."):
        if not isinstance(current, dict) or component not in current:
            return MISSING
        current = current[component]
    return current


def compact(value: Any) -> str:
    if isinstance(value, str):
        return value.replace("\t", " ").replace("\r", " ").replace("\n", " ")
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def build_rows(variants: dict[str, Any], definitions: dict[str, Any]) -> list[dict[str, str]]:
    checks = definitions.get("checks")
    expected = definitions.get("checks_per_variant")
    if not isinstance(checks, list) or expected != 28 or len(checks) != expected:
        raise SystemExit("ABI check definition must contain exactly 28 checks")
    check_ids = [check.get("id") for check in checks]
    if len(set(check_ids)) != len(check_ids) or any(not item for item in check_ids):
        raise SystemExit("ABI check IDs must be non-empty and unique")

    entries = variants.get("variants")
    if not isinstance(entries, list) or not entries:
        raise SystemExit("ABI variant catalog has no entries")
    variant_ids = [entry.get("variant_id") for entry in entries]
    if len(set(variant_ids)) != len(variant_ids) or any(not item for item in variant_ids):
        raise SystemExit("ABI variant IDs must be non-empty and unique")

    rows: list[dict[str, str]] = []
    missing: list[str] = []
    for entry in sorted(entries, key=lambda item: item["variant_id"]):
        port = entry.get("canonical_port")
        abi = entry.get("abi")
        if not isinstance(port, str) or not port or abi not in ("armv7", "aarch64"):
            raise SystemExit("ABI variant has invalid canonical_port or abi")
        for check in checks:
            value = lookup(entry, check["path"])
            if value is MISSING:
                missing.append(f"{entry['variant_id']}:{check['path']}")
                continue
            rows.append({
                "id": f"ABI-{port}-{abi}-{check['id']}",
                "port": port,
                "abi": abi,
                "check": check["id"],
                "category": check["category"],
                "status": "recorded",
                "value": compact(value),
            })

    if missing:
        raise SystemExit("missing ABI fields: " + ", ".join(missing))
    expected_rows = len(entries) * 28
    if len(rows) != expected_rows:
        raise SystemExit(f"expected {expected_rows} ABI rows, got {len(rows)}")
    return rows


def render_tsv(rows: list[dict[str, str]]) -> str:
    fields = ("id", "port", "abi", "check", "category", "status", "value")
    lines = ["\t".join(fields)]
    lines.extend("\t".join(row[field] for field in fields) for row in rows)
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--variants", type=Path, required=True)
    parser.add_argument("--checks", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = build_rows(load_json(args.variants), load_json(args.checks))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render_tsv(rows), encoding="utf-8")
    print(f"variants={len(rows) // 28} checks_per_variant=28 rows={len(rows)} output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
