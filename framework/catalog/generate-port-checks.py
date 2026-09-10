#!/usr/bin/env python3
"""Expand the evidence catalog into exactly 42 recorded decisions per entry."""

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


def build_rows(catalog: dict[str, Any], definitions: dict[str, Any]) -> list[dict[str, str]]:
    checks = definitions.get("checks")
    expected = definitions.get("checks_per_entry")
    if not isinstance(checks, list) or expected != 42 or len(checks) != expected:
        raise SystemExit("check definition must contain exactly 42 checks")

    check_ids = [check.get("id") for check in checks]
    if len(set(check_ids)) != len(check_ids) or any(not item for item in check_ids):
        raise SystemExit("check IDs must be non-empty and unique")

    entries = catalog.get("entries")
    if not isinstance(entries, list) or not entries:
        raise SystemExit("catalog has no entries")

    entry_ids = [entry.get("canonical_id") for entry in entries]
    if len(set(entry_ids)) != len(entry_ids) or any(not item for item in entry_ids):
        raise SystemExit("catalog entry IDs must be non-empty and unique")

    rows: list[dict[str, str]] = []
    missing: list[str] = []
    for entry in sorted(entries, key=lambda item: item["canonical_id"]):
        entry_id = entry["canonical_id"]
        for check in checks:
            value = lookup(entry, check["path"])
            if value is MISSING:
                missing.append(f"{entry_id}:{check['path']}")
                continue
            rows.append(
                {
                    "id": f"PORT-{entry_id}-{check['id']}",
                    "entry": entry_id,
                    "check": check["id"],
                    "category": check["category"],
                    "status": "recorded",
                    "value": compact(value),
                }
            )

    if missing:
        raise SystemExit("missing catalog fields: " + ", ".join(missing))
    expected_rows = len(entries) * 42
    if len(rows) != expected_rows:
        raise SystemExit(f"expected {expected_rows} rows, got {len(rows)}")
    return rows


def render_tsv(rows: list[dict[str, str]]) -> str:
    fields = ("id", "entry", "check", "category", "status", "value")
    lines = ["\t".join(fields)]
    lines.extend("\t".join(row[field] for field in fields) for row in rows)
    return "\n".join(lines) + "\n"


def render_jsonl(rows: list[dict[str, str]]) -> str:
    return "".join(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n" for row in rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--checks", type=Path, required=True)
    parser.add_argument("--format", choices=("tsv", "jsonl"), default="tsv")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    rows = build_rows(load_json(args.catalog), load_json(args.checks))
    rendered = render_tsv(rows) if args.format == "tsv" else render_jsonl(rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    print(f"entries={len(rows) // 42} checks_per_entry=42 rows={len(rows)} output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
