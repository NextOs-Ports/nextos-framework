#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C3 (mission 114A): bytes preserved WITHOUT an opt-in.

The C3 head that the independent audit rejected always appended a disabled
``input_controller_profiles`` block to every regenerated adapter contract.
That contradicts the promise made to already published ports: a port that
declares nothing must regenerate to the SAME bytes it had before C3.

This gate is LITERAL, not normalized. ``fixtures/c3-baseline/`` carries

  * ``nxproject.json``            -- a schema-3 manifest that declares no
                                    ``controls.controller_profiles``;
  * ``adapter-contract.golden.json`` -- the adapter contract that the REAL
                                    pre-C3 generator (framework/nxgenerator
                                    at commit 1c83799, "nxgenerator 0.3.1")
                                    produced from that exact manifest.

The current generator must reproduce that file byte for byte. It also proves
the opt-in is the only thing that can introduce the field, and that the
field then round-trips verbatim.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parents[1]
TOOL = ROOT / "nxgenerator.py"
FIXTURE = ROOT / "tests" / "fixtures" / "c3-baseline"
MANIFEST = FIXTURE / "nxproject.json"
GOLDEN = FIXTURE / "adapter-contract.golden.json"
PROJECT = "nxexample-aarch64"


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def generate(manifest, output):
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        [sys.executable, "-B", str(TOOL), str(manifest), "--output",
         str(output)],
        cwd=str(REPOSITORY), env=environment, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        check=False)
    require(result.returncode == 0,
            "the generator failed on the C3 baseline fixture: %s"
            % result.stderr.strip())
    return output / PROJECT / "adapter" / "adapter-contract.json"


def main():
    work = Path(tempfile.mkdtemp(prefix="nxgen-c3-bytes."))
    try:
        # 1. No opt-in declared: LITERALLY the pre-C3 bytes.
        produced = generate(MANIFEST, work / "baseline")
        golden_bytes = GOLDEN.read_bytes()
        produced_bytes = produced.read_bytes()
        require(produced_bytes == golden_bytes,
                "the regenerated contract is NOT byte-identical to the "
                "pre-C3 golden (%d vs %d bytes)"
                % (len(produced_bytes), len(golden_bytes)))
        contract = json.loads(golden_bytes.decode("utf-8"))
        require("input_controller_profiles" not in contract,
                "the pre-C3 golden already carried the C3 field; the "
                "baseline is not a baseline")

        # 2. The field EXISTS only by declaration, and round-trips verbatim.
        declared = json.loads(MANIFEST.read_text(encoding="utf-8"))
        pin = {"enabled": True, "bundle": "controllers.nxb",
               "sha256": "ab" * 32}
        declared.setdefault("controls", {})["controller_profiles"] = pin
        declared_path = work / "declared.json"
        declared_path.write_text(json.dumps(declared, ensure_ascii=False),
                                 encoding="utf-8")
        opted_in = json.loads(
            generate(declared_path, work / "declared-out")
            .read_text(encoding="utf-8"))
        require(opted_in.get("input_controller_profiles") == pin,
                "the declared controller_profiles pin was lost or rewritten")

        # 3. And the ONLY difference the opt-in makes is that field: every
        # other byte of the contract stays exactly as the baseline.
        without = dict(opted_in)
        without.pop("input_controller_profiles")
        require(without == contract,
                "the controller_profiles opt-in changed something else in "
                "the adapter contract")

        # 4. A disabled declaration is still a DECLARATION: it is written,
        # because the port asked for it -- but it never appears on its own.
        disabled = json.loads(MANIFEST.read_text(encoding="utf-8"))
        disabled.setdefault("controls", {})["controller_profiles"] = {
            "enabled": False, "bundle": "", "sha256": ""}
        disabled_path = work / "disabled.json"
        disabled_path.write_text(json.dumps(disabled, ensure_ascii=False),
                                 encoding="utf-8")
        disabled_contract = json.loads(
            generate(disabled_path, work / "disabled-out")
            .read_text(encoding="utf-8"))
        require(disabled_contract.get("input_controller_profiles") ==
                {"enabled": False, "bundle": "", "sha256": ""},
                "an explicitly disabled declaration was dropped")

        print("nxgenerator C3 byte-preservation gate passed: "
              "golden=%s literal_bytes=%d optin_roundtrip=1 "
              "no_other_drift=1 explicit_disabled=1"
              % (GOLDEN.name, len(golden_bytes)))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
