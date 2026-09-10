#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V5 (M1c 1.2): a CLOSURE ESPERADA nasce do projeto, nunca do mapa sob teste.

O harness de host do Tearscape carregava a própria expectativa dentro do port.
Quando o owner virou schema 4 o harness ficou no loader V1-V3 e a prova inteira
foi a vermelho em `generated default map loads` (NXI1006) -- e a falha apareceu
três cascatas depois, como "generated vector reaches the declared action sink",
o que matou o laço da rodada 3 sem contexto (E11 da auditoria).

Regra 8.1: o esperado não pode vir do mapa sob teste. O gerador, que já é dono
de `controls.contexts` e `controls.actions`, escreve `CONTROLS-CLOSURE.json` ao
lado do owner gerado, no mesmo formato que o `make_input_proof.py` calcula.
"""
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "nxgenerator", ROOT / "framework/nxgenerator/nxgenerator.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

fails = 0


def check(condition, message):
    global fails
    print(("ok   " if condition else "FAIL ") + message)
    if not condition:
        fails += 1


CONTROLS = {
    "schema": 4,
    "actions": [
        {"id": "menu.accept", "kind": "button", "sinks": ["ui.accept"]},
        {"id": "menu.navigate", "kind": "vector", "sinks": ["ui.move"]},
        {"id": "player.attack", "kind": "button",
         "sinks": ["combat.attack", "tutorial.attack"]},
        {"id": "player.aim", "kind": "axis", "sinks": ["combat.aim"]},
    ],
    "contexts": {
        "menu": {"A": "menu.accept", "B": "null", "LEFT_STICK": "menu.navigate",
                 "GUIDE": "native"},
        "gameplay": {"X": "player.attack", "L2": "player.aim",
                     "Y": "native", "START": "null"},
    },
}

closure = gen.build_controls_closure(CONTROLS, "closuretest")
cases = closure["cases"]

check(closure["schema"] == "nx-controls-closure/1" and
      closure["gptk_schema"] == 4 and closure["port_id"] == "closuretest",
      "closure is nx-controls-closure/1 and names the port and the gptk schema")
check(closure["declared_contexts"] == ["gameplay", "menu"],
      "closure lists ONLY the contexts the port declared -- a schema-4 [base] "
      "also reaches undeclared contexts and that is not an extra binding")

ids = [(c["context"], c["control"], c["action"], c["sink"], c["event"],
        c["delivery_count"]) for c in cases]
check(("menu", "A", "menu.accept", "ui.accept", "press", 1) in ids,
      "a button binding becomes one press case per sink, delivered once")
check(("menu", "LEFT_STICK", "menu.navigate", "ui.move", "motion", 1) in ids,
      "a vector binding becomes a motion case (the sink that went red in the "
      "Tearscape harness)")
check(("gameplay", "L2", "player.aim", "combat.aim", "axis", 1) in ids,
      "an axis binding becomes an axis case")
check(sum(1 for c in cases if c["action"] == "player.attack") == 2,
      "an action with two sinks yields one case per sink")
check(not any(c["control"] in ("B", "START", "GUIDE", "Y") for c in cases),
      "MUTANT killed: `null` and `native` carry NO case -- they are the "
      "absence of an action and the host gate proves them as NONE/SUPPRESS")
check(all(c["decision"] == "ACTION" for c in cases),
      "every case in the closure is an ACTION decision")
check(cases == sorted(cases, key=lambda c: (c["context"], c["control"],
                                            c["action"], c["sink"], c["event"])),
      "cases are canonically ordered (byte-stable closure)")
check(len(cases) == 5, "closure size is exactly the declared bindings (5)")

# The closure must be a pure function of the project, not of any generated map.
again = gen.build_controls_closure(json.loads(json.dumps(CONTROLS)),
                                   "closuretest")
check(gen.canonical_json(again) == gen.canonical_json(closure),
      "the closure is deterministic for the same project")

broken = json.loads(json.dumps(CONTROLS))
broken["contexts"]["menu"]["A"] = "menu.does_not_exist"
try:
    gen.build_controls_closure(broken, "closuretest")
    check(False, "a binding to an undeclared action must be refused")
except gen.ProjectError as error:
    check("undeclared action" in str(error),
          "MUTANT killed: a binding to an undeclared action is refused, named "
          "(%s)" % error)

print("nxgenerator-controls-closure: %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
