#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Gate for the V3-PERF-01 budget receipt tool."""

import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOL = ROOT / "nx-budget-check.py"


def run(*argv):
    return subprocess.run(
        [sys.executable, "-B", str(TOOL), *argv],
        capture_output=True, text=True, timeout=60,
    )


def main():
    with tempfile.TemporaryDirectory(prefix="nxobs-budget-") as raw:
        port = pathlib.Path(raw)
        (port / "log.txt").write_bytes(b"x" * 1024)
        (port / "events.jsonl").write_bytes(b"{}\n")
        result = run(str(port), "--json", "--strict")
        assert result.returncode == 0, result.stderr
        receipt = json.loads(result.stdout)
        assert receipt["schema"] == "nx-budget-receipt-v1"
        assert receipt["exceeded_count"] == 0
        (port / "nxextract.log").write_bytes(b"y" * (2 * 1024 * 1024 + 1))
        result = run(str(port), "--strict")
        assert result.returncode == 1, "exceeded budget must fail --strict"
        assert "EXCEEDED" in result.stdout
        result = run(str(port))
        assert result.returncode == 0, "non-strict never fails on size"
        # games/saves are never in the budget table
        (port / "save.dat").write_bytes(b"z" * (64 * 1024 * 1024))
        result = run(str(port), "--strict")
        assert "save.dat" not in result.stdout
    print("nxobs budget gate passed: receipts=1 strict=1 saves_exempt=1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
