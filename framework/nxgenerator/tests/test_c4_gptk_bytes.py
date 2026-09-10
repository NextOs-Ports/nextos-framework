#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C4: NEXTOSCONTROLLERS v2 is opt-in, and a static map
does not make a false editable-runtime claim.

LITERAL, not normalized: `fixtures/c4-baseline/` carries a schema-3 manifest
that declares no `controls.schema` or live runtime, plus the canonical static
`defaults/NEXTOSCONTROLLERS.gptk`. The current generator must reproduce that
file byte for byte and must not tell the owner that edits reach the engine.

The same fixture, with `controls.schema = 2` added, must then produce a
complete V2 file: the magic, every one of the 18 controls in every declared
section exactly once, `null` for what the port does not use, and the explicit
`native` where the port declared it.
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
FIXTURE = ROOT / "tests" / "fixtures" / "c4-baseline"
MANIFEST = FIXTURE / "nxproject.json"
GOLDEN = FIXTURE / "NEXTOSCONTROLLERS.gptk.golden"
PROJECT = "nxexample-aarch64"

CONTROLS = ("A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
            "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
            "LEFT_STICK", "RIGHT_STICK")


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def generate(manifest, output, expected=0):
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        [sys.executable, "-B", str(TOOL), str(manifest), "--output",
         str(output)],
        cwd=str(REPOSITORY), env=environment, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        check=False)
    require(result.returncode == expected,
            "generator status %d != %d: %s"
            % (result.returncode, expected, result.stderr.strip()))
    if expected != 0:
        return result.stderr
    return output / PROJECT / "defaults" / "NEXTOSCONTROLLERS.gptk"


def sections_of(text):
    """{context: [(control, value), ...]} in file order."""
    sections = {}
    current = None
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
            sections[current] = []
        elif "=" in line and current is not None:
            key, _, value = line.partition("=")
            sections[current].append((key.strip(), value.strip()))
    return sections



def declare_dpad(document):
    """The 0.3.15 guard: schema >= 2 refuses an undeclared D-pad cluster."""
    for name, bindings in document["controls"]["contexts"].items():
        if name == "cursor":
            continue
        for control in ("UP", "DOWN", "LEFT", "RIGHT"):
            bindings.setdefault(control, "native")

def main():
    work = Path(tempfile.mkdtemp(prefix="nxgen-c4-bytes."))
    try:
        # 1. No live opt-in: LITERALLY the canonical static bytes.
        produced = generate(MANIFEST, work / "baseline")
        golden_bytes = GOLDEN.read_bytes()
        produced_bytes = produced.read_bytes()
        require(produced_bytes == golden_bytes,
                "the regenerated NEXTOSCONTROLLERS.gptk is NOT byte-identical "
                "to the static golden (%d vs %d bytes)"
                % (len(produced_bytes), len(golden_bytes)))
        golden_text = golden_bytes.decode("utf-8")
        require("format = NEXTOS_CONTROLLERS/1" in golden_text,
                "the static golden is not a v1 file; it is not a baseline")
        require("= null" not in golden_text,
                "the static golden already carries null; not a baseline")
        require("This file is YOURS" not in golden_text and
                "Este arquivo é SEU" not in golden_text,
                "the static golden still promises editable runtime controls")
        require("does NOT load edits" in golden_text and
                "NÃO carrega edições" in golden_text,
                "the static golden does not explain that edits are inert")

        # 2. The opt-in produces a COMPLETE v2 file.
        declared = json.loads(MANIFEST.read_text(encoding="utf-8"))
        declared["controls"]["schema"] = 2
        declared["controls"]["contexts"]["gameplay"]["X"] = "native"
        declare_dpad(declared)
        opted_path = work / "v2.json"
        opted_path.write_text(json.dumps(declared, ensure_ascii=False),
                              encoding="utf-8")
        v2_text = generate(opted_path, work / "v2-out").read_text(
            encoding="utf-8")
        require("format = NEXTOS_CONTROLLERS/2" in v2_text,
                "the opt-in did not switch the magic to /2")
        sections = sections_of(v2_text)
        require(set(sections) >= {"menu", "gameplay"},
                "the v2 output lost a required section")
        for name, entries in sections.items():
            if name == "camera":
                continue
            controls = [key for key, _ in entries if key in CONTROLS]
            require(controls == list(CONTROLS),
                    "section [%s] does not list the 18 controls in the "
                    "stable order: %s" % (name, controls))
            require(len(set(controls)) == len(controls),
                    "section [%s] repeats a control" % name)
        gameplay = dict(sections["gameplay"])
        require(gameplay["X"] == "native",
                "the declared native passthrough was lost")
        require(gameplay["L3"] == "null",
                "an unused control was not written as null")
        used = [value for value in gameplay.values()
                if value not in ("null", "native")]
        require(used, "the v2 output bound no action at all")

        # 2b (0.3.15). The V3 opt-in renders FACE_LAYOUT exactly once, in the
        # preamble, and keeps the whole V2 completeness.
        declared = json.loads(MANIFEST.read_text(encoding="utf-8"))
        declared["controls"]["schema"] = 3
        declared["controls"]["contexts"]["gameplay"]["X"] = "native"
        declare_dpad(declared)
        v3_path = work / "v3.json"
        v3_path.write_text(json.dumps(declared, ensure_ascii=False),
                           encoding="utf-8")
        v3_text = generate(v3_path, work / "v3-out").read_text(
            encoding="utf-8")
        require("format = NEXTOS_CONTROLLERS/3" in v3_text,
                "the V3 opt-in did not switch the magic to /3")
        require(v3_text.count("FACE_LAYOUT") == 1,
                "FACE_LAYOUT must be rendered exactly once")
        preamble = v3_text.split("[", 1)[0]
        require("FACE_LAYOUT = auto" in preamble,
                "the default FACE_LAYOUT auto must sit in the preamble")
        v3_sections = sections_of(v3_text)
        for name, entries in v3_sections.items():
            if name == "camera":
                continue
            v3_controls = [key for key, _ in entries if key in CONTROLS]
            require(v3_controls == list(CONTROLS),
                    "V3 section [%s] does not keep the 18-control "
                    "completeness" % name)
        declared["controls"]["face_layout"] = "retro"
        v3_path.write_text(json.dumps(declared, ensure_ascii=False),
                           encoding="utf-8")
        v3_retro = generate(v3_path, work / "v3-retro-out").read_text(
            encoding="utf-8")
        require("FACE_LAYOUT = retro" in v3_retro.split("[", 1)[0],
                "an explicit face_layout must be rendered verbatim")

        # 3. Negatives: an unknown schema, `native` without the opt-in, and a
        # manifest that tries to spell `null` itself.
        for mutation, fragment, why in (
            # 0.3.15: schema 3 is now the FACE_LAYOUT format, so the unknown
            # schema moved to 4 and V3 gained its own negatives.
            (lambda d: d["controls"].__setitem__("schema", 4),
             "controls.schema", "an unknown gptk schema"),
            (lambda d: (d["controls"].__setitem__("schema", 3),
                        d["controls"].__setitem__("face_layout", "Modern")),
             "auto, modern or retro", "a wrong-case face layout"),
            (lambda d: d["controls"].__setitem__("face_layout", "auto"),
             "requires controls.schema 3", "face_layout without schema 3"),
            (lambda d: (d["controls"].__setitem__("schema", 1),
                        d["controls"]["contexts"]["gameplay"].__setitem__(
                            "X", "native")),
             "requires controls.schema 2", "native without the opt-in"),
            (lambda d: d["controls"]["contexts"]["gameplay"].__setitem__(
                "L3", "null"),
             "requires controls.schema 2", "null without the opt-in"),
            (lambda d: d["controls"].__setitem__("schema", 2),
             "leaves the D-pad undeclared", "an undeclared D-pad cluster"),
        ):
            candidate = json.loads(MANIFEST.read_text(encoding="utf-8"))
            mutation(candidate)
            path = work / ("neg-%s.json" % abs(hash(why)))
            path.write_text(json.dumps(candidate), encoding="utf-8")
            stderr = generate(path, work / ("neg-%s" % abs(hash(why))),
                              expected=1)
            require(fragment in stderr,
                    "%s was refused without naming %r: %s"
                    % (why, fragment, stderr.strip()[:200]))

        print("nxgenerator C4 gptk gate passed: golden=%s literal_bytes=%d "
              "v2_controls_per_section=18 v2_sections=%d native=1 null=1 "
              "negatives=5"
              % (GOLDEN.name, len(golden_bytes),
                 len([s for s in sections if s != "camera"])))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
