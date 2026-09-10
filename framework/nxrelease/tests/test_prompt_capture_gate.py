#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V5 (M1c item 2): uma prova de prompt so vale em `nx-prompt-capture-proof/2`.

O `/1` aprovou `PRESS [10] TO BEGIN` no FP2 -- `result: PASS`, `forbidden: []`
(achado E13 da auditoria). O regex conhecia `Joystick Button N`, `Button N` e
`Axis +-N`, mas o jogo desenha o PROPRIO icone com o ordinal cru DENTRO e o OCR
de tela inteira nem enxerga o digito. Um esquema capaz de aprovar exatamente o
defeito que deveria pegar nao e prova: e recusado por IDENTIDADE.

O controle positivo obrigatorio esta no fim: o receipt `/1` que hoje vive em
`ports/fp2/proofs/` TEM de ser recusado por este gate.
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


def run(source_root):
    try:
        nxr.validate_prompt_capture_proof({"source_root": Path(source_root)})
    except nxr.ReleaseError as error:
        return str(error)
    return None


GOOD = {
    "schema": "nx-prompt-capture-proof/2",
    "label": "title",
    "port": "fixture",
    "result": "PASS",
    "captures": [{
        "name": "title.png",
        "regions": [{
            "name": "press-prompt",
            "kind": "glyph",
            "expect": "A",
            "glyph_by_ocr": True,
            "glyph_met": True,
            "raw_ordinal_tokens": [],
        }],
    }],
}


def write(root, label, document):
    directory = Path(root) / "proofs" / label
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "receipt.json").write_text(
        json.dumps(document, indent=2) + "\n", encoding="utf-8")


work = Path(tempfile.mkdtemp(prefix="nx-prompt-gate."))
try:
    port = work / "port"
    (port / "proofs").mkdir(parents=True)
    check(run(port) is None, "a port with no prompt proof is not forced to have one")

    write(port, "ok", GOOD)
    check(run(port) is None,
          "a /2 receipt with a recognised expected glyph passes")

    legacy = {"schema": "nx-prompt-capture-proof/1", "result": "PASS",
              "captures": [{"name": "x.png", "forbidden": []}]}
    write(port, "legacy", legacy)
    message = run(port)
    check(message is not None and "only nx-prompt-capture-proof/2" in message,
          "MUTANT killed: a /1 receipt is refused by identity -- it is the "
          "scheme that certified PRESS [10] TO BEGIN as PASS")
    shutil.rmtree(port / "proofs" / "legacy")

    silent = copy.deepcopy(GOOD)
    silent["captures"][0]["regions"][0].pop("expect")
    silent["captures"][0]["regions"][0].pop("glyph_met")
    write(port, "silent", silent)
    message = run(port)
    check(message is not None and "never passes by silence" in message,
          "MUTANT killed: PASS with no declared glyph expectation is refused "
          "(the /1 hole was exactly a silent PASS)")
    shutil.rmtree(port / "proofs" / "silent")

    ordinal = copy.deepcopy(GOOD)
    ordinal["captures"][0]["regions"][0]["raw_ordinal_tokens"] = ["10"]
    write(port, "ordinal", ordinal)
    message = run(port)
    check(message is not None and "shows the raw ordinal" in message,
          "MUTANT killed: PASS while a declared region shows a raw ordinal is "
          "refused, naming the token")
    shutil.rmtree(port / "proofs" / "ordinal")

    unmet = copy.deepcopy(GOOD)
    unmet["captures"][0]["regions"][0]["glyph_met"] = False
    write(port, "unmet", unmet)
    message = run(port)
    check(message is not None and "never passes by silence" in message,
          "a declared glyph that was not recognised cannot be a PASS")
    shutil.rmtree(port / "proofs" / "unmet")

    unknown = copy.deepcopy(GOOD)
    unknown["captures"][0]["regions"][0]["expect"] = "BUTTON7"
    write(port, "unknown", unknown)
    message = run(port)
    check(message is not None and "unknown glyph" in message,
          "an expectation outside the Xbox token whitelist is refused")
    shutil.rmtree(port / "proofs" / "unknown")

    flat = copy.deepcopy(GOOD)
    flat["captures"][0].pop("regions")
    write(port, "flat", flat)
    message = run(port)
    check(message is not None and "no region list" in message,
          "MUTANT killed: full-frame OCR with no declared region is refused "
          "-- that is precisely what the /1 did")
    shutil.rmtree(port / "proofs" / "flat")

    inconclusive = copy.deepcopy(GOOD)
    inconclusive["result"] = "INCONCLUSIVE"
    inconclusive["captures"][0]["regions"][0]["glyph_met"] = False
    write(port, "inconclusive", inconclusive)
    check(run(port) is None,
          "INCONCLUSIVE is a legitimate verdict: the gate refuses false PASS, "
          "not honest uncertainty")

    # --- positive control: the receipt that exists in the tree TODAY --------
    fp2 = ROOT / "ports" / "fp2"
    if (fp2 / "proofs").is_dir():
        message = run(fp2)
        check(message is not None and
              "only nx-prompt-capture-proof/2" in message,
              "positive control: the FP2 prompt receipt in the tree today is "
              "REFUSED (it is the /1 that approved the ordinal in the icon)")
    else:
        check(False, "the FP2 proofs directory was not found")
finally:
    shutil.rmtree(work, ignore_errors=True)

print("nxrelease-prompt-capture-gate: %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
