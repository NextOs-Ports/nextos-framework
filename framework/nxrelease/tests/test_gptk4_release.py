#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxrelease 0.4.3 / V5: NEXTOS_CONTROLLERS/4 default closure and the /4
runtime marker. Positive = the nxgenerator-rendered FP2 default pinned in
the nxinput corpus; mutants: omitted control, undeclared action, whole-base
override, V3 section, FACE_LAYOUT, engine authority, untyped binding,
vector mode with a live direction, analog trigger with a digital binding."""
import importlib.util, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nxrelease_gptk4_test", ROOT / "nxrelease.py")
nx = importlib.util.module_from_spec(spec); spec.loader.exec_module(nx)
fails = 0
def check(c, m):
    global fails
    print(("ok   " if c else "FAIL ") + m)
    if not c: fails += 1
ADAPTER = {"actions": [{"id": a, "sinks": ["adapter.input.android-keyevent"]} for a in ("fp2.attack", "fp2.cancel", "fp2.confirm", "fp2.guard", "fp2.jump", "fp2.pause", "fp2.special")] + [{"id": "fp2.move", "sinks": ["adapter.input.android-motion"]}]}
corpus = (ROOT.parents[1] / "framework" / "nxinput" / "tests" / "v5" / "corpus" / "fp2-generated-v4.gptk").read_text(encoding="utf-8")
def closure(text, expect=None, label=""):
    try:
        nx._validate_gptk_closure(text, 4, ADAPTER)
    except (SystemExit, nx.ReleaseError) as e:
        check(expect is not None and expect.lower() in str(e).lower(), "%s -> refused: %s" % (label, e)); return
    check(expect is None, "%s -> accepted" % label)
closure(corpus, None, "generated FP2 schema-4 default")
closure(corpus.replace("GUIDE = native\n", ""), "omits GUIDE", "MUTANT: core control omitted")
closure(corpus.replace("A = action:fp2.attack", "A = action:fp2.nope"), "does not declare", "MUTANT: action without sink")
closure(corpus.replace("A = action:fp2.attack", "A = fp2.attack"), "typed binding", "MUTANT: untyped legacy binding")
closure(corpus.replace("[override.menu]\n", "[override.menu]\n" + "".join("%s = native\n" % k for k in ("L1", "R1", "L3", "R3", "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT"))), "sparse", "MUTANT: override duplicating the whole base")
closure(corpus + "\n[cursor]\nA = null\n", "V3 section", "MUTANT: V3 [cursor] section in a /4 owner")
closure(corpus.replace("CONTEXT_POLICY = unified", "CONTEXT_POLICY = unified\nFACE_LAYOUT = auto"), "FACE_LAYOUT", "MUTANT: FACE_LAYOUT in schema 4")
closure(corpus.replace("AUTHORITY = nextos", "AUTHORITY = engine"), "engine mode", "MUTANT: engine authority shipped as an editable owner")
closure(corpus.replace("[stick.left]\nmode = vector\nvector = action:fp2.move\nup = null", "[stick.left]\nmode = vector\nvector = action:fp2.move\nup = action:fp2.jump"), "null directions", "MUTANT: vector mode with a live direction")
closure(corpus.replace("[trigger.left]\nmode = analog\nanalog = native\ndigital = null", "[trigger.left]\nmode = analog\nanalog = native\ndigital = action:fp2.jump"), "digital = null", "MUTANT: analog trigger carrying a digital binding")
closure(corpus.replace("[trigger.right]\nmode = analog\nanalog = native\ndigital = null\n", ""), "omits section", "MUTANT: trigger section missing")
check(nx.input_runtime_marker() == nx.INPUT_RUNTIME_MARKER, "default expected marker is /3")
nx._INPUT_RUNTIME_MARKER_EXPECTED[0] = nx.INPUT_RUNTIME_MARKER_V4
check(nx.input_runtime_marker() == "nxinput-gptk-runtime/4", "schema-4 packages expect the /4 marker")
print("nxrelease-gptk4: %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
