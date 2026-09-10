#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V3-PERF-01: boot/log/write budget receipt for a port directory.

Baseline ceilings (starting points, tuned by baseline — NEVER used to kill a
game or cap saves; a framework component simply stops emitting detail at the
ceiling, keeping reserve for the terminal cause):
  runtime log 4 MiB (+2 rotations), NXExtract compact 2 MiB,
  NXExtract detail 8 MiB (+1 rotation), hook line 64 KiB, hook output 4 MiB,
  events.jsonl 1 MiB (+1 rotation).

The runtime log ceiling is not only reported here: since nxbootstrap 0.7.0 the
launcher trims log.txt back to it once the game is gone, keeping the tail.
Rotation alone bounded the log only BETWEEN runs, and a chatty engine writes
for hours inside one.
Emits an nx-budget-receipt-v1 JSON and exits 1 only with --strict when a
framework-owned artifact exceeds its ceiling.
"""

import argparse
import json
import pathlib
import sys

TOOL_VERSION = "0.1.0"
BUDGETS = {
    "log.txt": 4 * 1024 * 1024,
    "log.prev.txt": 4 * 1024 * 1024,
    "log.prev2.txt": 4 * 1024 * 1024,
    "nxextract.log": 2 * 1024 * 1024,
    "nxextract-detail.log": 8 * 1024 * 1024,
    "events.jsonl": 1024 * 1024,
    "events.prev.jsonl": 1024 * 1024,
}


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("port_dir", type=pathlib.Path)
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    if not args.port_dir.is_dir():
        print("nx-budget-check: not a directory", file=sys.stderr)
        return 2
    rows = []
    over = 0
    for name, ceiling in sorted(BUDGETS.items()):
        target = args.port_dir / name
        if target.is_symlink() or not target.is_file():
            continue
        size = target.stat().st_size
        exceeded = size > ceiling
        over += 1 if exceeded else 0
        rows.append({"file": name, "bytes": size, "ceiling": ceiling,
                     "exceeded": exceeded})
    receipt = {"schema": "nx-budget-receipt-v1", "schema_version": 1,
               "tool_version": TOOL_VERSION, "entries": rows,
               "exceeded_count": over}
    if args.json:
        print(json.dumps(receipt, sort_keys=True))
    else:
        for row in rows:
            print("BUDGET %s: %d/%d bytes%s" % (
                row["file"], row["bytes"], row["ceiling"],
                " EXCEEDED" if row["exceeded"] else ""))
        print("nx-budget-check: entries=%d exceeded=%d" % (len(rows), over))
    if args.strict and over:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
