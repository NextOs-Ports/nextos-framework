#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Regressao do gate de ordinal pad fix sem gate de barramento.

A assinatura herdada (BTN_GAMEPAD + BTN_C/BTN_Z, sem olhar o bustype) casa com
o pad INTERNO do H700, que publica esses mesmos codigos com layout semantico
correto. Aplicar a correcao ali troca os botoes de um controle que ja estava
certo. Este teste dirige o parser puro do nxrelease e depois exercita o gate
completo sobre uma arvore de fontes temporaria.
"""

import os
import shutil
import sys
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import nxrelease as nx

ROOT = Path(HERE).resolve().parents[2]
CANONICAL = ROOT / "framework/nxinput/include/nxinput_pad_ordinal_fix.h"

LEGACY = """
static void pad_ordinal_fix_apply(int index, const char *env_prefix) {
    if (ioctl(fd, EVIOCGID, &id) == 0 && id.vendor == vid &&
        id.product == pid &&
        pad_ord_test_bit(keyb, BTN_GAMEPAD) &&
        (pad_ord_test_bit(keyb, BTN_C) || pad_ord_test_bit(keyb, BTN_Z)))
        found = 1;
}
"""

GATED_LOCAL_COPY = """
static int pad_ord_external_bus(unsigned short bus) {
  return bus == BUS_USB || bus == BUS_BLUETOOTH;
}

static int pad_ordinal_fix_apply(int index, const char *env_prefix) {
    if (ioctl(fd, EVIOCGID, &id) == 0 && id.bustype &&
        pad_ord_external_bus(id.bustype) &&
        pad_ord_test_bit(keyb, BTN_GAMEPAD))
        found = 1;
    return found;
}
"""

WIDENED_GATE = """
static int pad_ord_external_bus(unsigned short bus) {
  return bus == BUS_USB || bus == BUS_BLUETOOTH || bus == BUS_HOST;
}

static int pad_ordinal_fix_apply(int index, const char *env_prefix) {
    if (id.bustype && pad_ord_external_bus(id.bustype))
        found = 1;
    return found;
}
"""


def check(label, condition):
    if not condition:
        raise SystemExit("pad-ordinal gate test FAIL: " + label)


check("legacy signature is rejected",
      not nx.pad_ordinal_gate_is_present(LEGACY))
check("gated local copy is accepted",
      nx.pad_ordinal_gate_is_present(GATED_LOCAL_COPY))
check("a gate widened to BUS_HOST is rejected",
      not nx.pad_ordinal_gate_is_present(WIDENED_GATE))
check("the canonical header is accepted",
      nx.pad_ordinal_gate_is_present(CANONICAL.read_text(encoding="utf-8")))
check("the definition regex still matches the legacy form",
      nx.PAD_ORDINAL_DEFINITION.search(LEGACY) is not None)
check("a mere call site is not a definition",
      nx.PAD_ORDINAL_DEFINITION.search(
          "  pad_ordinal_fix_apply(index, \"PORT\");\n") is None)


def run_tree(files):
    workdir = Path(tempfile.mkdtemp(prefix="nxrelease-pad-ordinal-"))
    try:
        for name, text in files.items():
            path = workdir / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        try:
            nx.validate_pad_ordinal_sources({"source_root": workdir})
        except SystemExit as exit_error:
            return str(exit_error)
        except nx.ReleaseError as release_error:
            return str(release_error)
        return None
    finally:
        shutil.rmtree(str(workdir))


failure = run_tree({"src/pad_ordinal_fix.h": LEGACY})
check("the tree gate rejects a legacy port header", failure is not None)
check("the failure names the offending file",
      failure is not None and "src/pad_ordinal_fix.h" in failure)

check("the tree gate accepts a gated port header",
      run_tree({"src/pad_ordinal_fix.h": GATED_LOCAL_COPY}) is None)
check("the tree gate accepts the canonical header",
      run_tree({"src/nxinput_pad_ordinal_fix.h":
                CANONICAL.read_text(encoding="utf-8")}) is None)
check("a call site alone never fails the port",
      run_tree({"src/main.c": "pad_ordinal_fix_apply(i, \"PORT\");\n"}) is None)
check("build leftovers are not audited",
      run_tree({"build/copy/pad_ordinal_fix.h": LEGACY}) is None)

print("NXRELEASE PAD-ORDINAL GATE: PASS")
