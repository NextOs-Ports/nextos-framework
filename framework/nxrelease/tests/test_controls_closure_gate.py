#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V5 (M1c 1.2/1.3): o `validate` cobra a closure esperada do port schema 4.

O harness de host do Tearscape derivava a expectativa DENTRO do port e ficou no
loader V1-V3 quando o owner virou schema 4: a prova toda foi a vermelho em
`NXI1006` e o laco da rodada 3 morreu lendo o rabo da cascata (E11). Regra 8.1:
o esperado nasce do projeto -- o nxgenerator emite `CONTROLS-CLOSURE.json` e
aqui se cobra que ele exista e seja coerente.
"""
import copy
import importlib.util
import json
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "nxrelease", ROOT / "framework/nxrelease/nxrelease.py")
nxr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nxr)

fails = 0


def check(condition, message):
    global fails
    print(("ok   " if condition else "FAIL ") + message)
    if not condition:
        fails += 1


CLOSURE = {
    "schema": "nx-controls-closure/1",
    "schema_version": 1,
    "port_id": "fixture",
    "gptk_schema": 4,
    "declared_contexts": ["gameplay", "menu"],
    "actions": [],
    "cases": [{
        "context": "menu", "control": "A", "event": "press",
        "decision": "ACTION", "action": "menu.accept", "sink": "ui.accept",
        "delivery_count": 1,
    }],
}

work = Path(tempfile.mkdtemp(prefix="nx-closure-gate."))


def run(document, schema=4, present=True):
    path = work / "CONTROLS-CLOSURE.json"
    if document is not None:
        path.write_text(json.dumps(document, indent=2) + "\n",
                        encoding="utf-8")
    records = []
    if present:
        records.append({"target": "fixture/CONTROLS-CLOSURE.json",
                        "kind": "payload", "mode": 0o644,
                        "actual_path": str(path)})
    config = {"port_dir": "fixture", "package_id": "fixture",
              "nxport_manifest": {"controls": {"schema": schema}}}
    try:
        nxr.validate_controls_closure(records, config)
    except nxr.ReleaseError as error:
        return str(error)
    return None


try:
    check(run(CLOSURE) is None, "a coherent closure passes")
    check(run(None, schema=4, present=False) is not None and
          "requires CONTROLS-CLOSURE.json" in run(None, schema=4, present=False),
          "MUTANT killed: a schema-4 port with NO closure is refused -- "
          "otherwise the expectation goes back to living in the port harness")
    check(run(None, schema=3, present=False) is None,
          "a port that is not schema 4 is not forced to carry a closure")

    bad = copy.deepcopy(CLOSURE)
    bad["schema"] = "nx-controls-closure/0"
    check("not nx-controls-closure/1" in (run(bad) or ""),
          "an unknown closure schema is refused")

    bad = copy.deepcopy(CLOSURE)
    bad["port_id"] = "another-port"
    check("names port" in (run(bad) or ""),
          "a closure belonging to another port is refused")

    bad = copy.deepcopy(CLOSURE)
    bad["cases"][0]["context"] = "cursor"
    check("undeclared context" in (run(bad) or ""),
          "MUTANT killed: a case in a context the port never declared is "
          "refused (the exact confusion that made the Tearscape harness "
          "demand CURSOR)")

    bad = copy.deepcopy(CLOSURE)
    bad["cases"] = []
    check("empty case list" in (run(bad) or ""),
          "MUTANT killed (0.4.3, F9): an EMPTY closure passed as proof")

    bad = copy.deepcopy(CLOSURE)
    bad["cases"][0]["delivery_count"] = 2
    check("not one ACTION delivery" in (run(bad) or ""),
          "MUTANT killed: a case that expects double delivery is refused")

    bad = copy.deepcopy(CLOSURE)
    bad["cases"][0]["event"] = "click"
    check("unknown event" in (run(bad) or ""),
          "an event outside press/axis/motion is refused")

    bad = copy.deepcopy(CLOSURE)
    bad["declared_contexts"] = []
    check("declares no context" in (run(bad) or ""),
          "a closure with no declared context is refused")
finally:
    shutil.rmtree(work, ignore_errors=True)

print("nxrelease-controls-closure-gate: %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
