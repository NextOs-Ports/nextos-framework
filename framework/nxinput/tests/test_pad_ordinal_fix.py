#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Gate fail-closed do ordinal pad fix canonico do nxinput.

Compila o nucleo real do header (sem SDL, sem device) e roda as tabelas evdev
de aparelhos reais declaradas em framework/tests/fixtures/controls.
O caso H700 e a fronteira: a assinatura SEM gate de barramento casa com ele,
e apenas o gate BUS_HOST impede que um pad interno correto seja remapeado.
"""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HEADER = ROOT / "framework/nxinput/include/nxinput_pad_ordinal_fix.h"
PROBE = ROOT / "framework/nxinput/tests/pad_ordinal_probe.c"
FIXTURE = ROOT / "framework/tests/fixtures/controls/pad-ordinal-v1.json"
REQUIRED = ("id", "provenance", "bus", "evdev_keys", "evdev_abs",
            "expect_signature", "expect_signature_ungated", "expect_reason")


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_json(path):
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key: %s" % key)
            result[key] = value
        return result

    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=no_duplicates)


def compile_probe(workdir):
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    require(compiler, "host C compiler not found; the gate cannot run blind")
    binary = workdir / "pad_ordinal_probe"
    command = [
        compiler, "-std=c99", "-Wall", "-Wextra", "-Wformat=2", "-Wshadow",
        "-Wstrict-prototypes", "-Werror", "-D_POSIX_C_SOURCE=200809L",
        "-I", str(HEADER.parent), "-o", str(binary), str(PROBE),
    ]
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    require(result.returncode == 0,
            "probe failed to build:\n%s" % result.stdout.decode("utf-8", "replace"))
    return binary


def run_probe(binary, case):
    command = [str(binary), "--bus", case["bus"],
               "--layout", case.get("layout", "hid")]
    if case["evdev_keys"]:
        command += ["--keys", ",".join(case["evdev_keys"])]
    if case["evdev_abs"]:
        command += ["--abs", ",".join(case["evdev_abs"])]
    if case.get("guid"):
        command += ["--guid", case["guid"]]
    if case.get("name"):
        command += ["--name", case["name"]]
    if case.get("config") is not None:
        command += ["--config", case["config"]]
    if case.get("force") is not None:
        command += ["--force", str(case["force"])]
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    require(result.returncode == 0,
            "probe failed for %s: %s" % (case["id"],
                                         result.stderr.decode("utf-8", "replace")))
    fields = {}
    for line in result.stdout.decode("utf-8").splitlines():
        key, _, value = line.partition("=")
        fields[key] = value
    return fields


def validate_header_source():
    require(HEADER.is_file(), "canonical header is missing: %s" % HEADER)
    text = HEADER.read_text(encoding="utf-8")
    require("nxinput_pad_ordinal_bus_is_external" in text,
            "canonical header lost the bus gate")
    require("BUS_HOST" in text,
            "canonical header must document the BUS_HOST boundary")
    require("BUS_USB" in text and "BUS_BLUETOOTH" in text,
            "canonical header must name the external buses it accepts")
    gate = text.split("nxinput_pad_ordinal_bus_is_external(unsigned short bus)")[-1]
    gate = gate.split("}")[0]
    for forbidden in ("BUS_HOST", "BUS_I2C", "BUS_SPI", "BUS_VIRTUAL"):
        require(forbidden not in gate,
                "internal bus %s accepted by the gate" % forbidden)
    require("nxinput_pad_ordinal_signature" in text,
            "canonical header lost the complete signature helper")
    # Identidade (nome, firmware, VID/PID) nao decide nada aqui: a classe de
    # ordem de report e parametro do chamador.
    require("0x054c" not in text and "0x54c" not in text,
            "canonical header hard-codes a vendor id")
    require("NXINPUT_PAD_ORDINAL_LAYOUT_HID" in text and
            "NXINPUT_PAD_ORDINAL_LAYOUT_ALT" in text,
            "canonical header lost the caller-selected layout")

    # A fonte e unica: nenhuma outra copia dentro do framework.
    copies = [path for path in (ROOT / "framework").rglob("*.h")
              if path != HEADER
              and "nxinput_pad_ordinal_fix_apply" in path.read_text(
                  encoding="utf-8", errors="replace")]
    require(not copies,
            "duplicated ordinal fix inside the framework: %s" % copies)


def main():
    validate_header_source()
    fixture = load_json(FIXTURE)
    require(fixture.get("schema") == "nxframework-pad-ordinal-v1",
            "unexpected fixture schema")
    require(fixture.get("schema_version") == 1, "unexpected fixture version")
    cases = fixture.get("cases")
    require(isinstance(cases, list) and cases, "fixture has no cases")

    identifiers = set()
    internal_protected = 0
    external_fires = 0
    with tempfile.TemporaryDirectory() as tmp:
        binary = compile_probe(Path(tmp))
        for case in cases:
            for key in REQUIRED:
                require(key in case, "case %r lacks %s" % (case.get("id"), key))
            require(case["id"] not in identifiers,
                    "duplicated case id: %s" % case["id"])
            identifiers.add(case["id"])
            require(len(case["provenance"]) > 20,
                    "case %s has no real provenance" % case["id"])

            fields = run_probe(binary, case)
            expected = str(case["expect_signature"])
            require(fields.get("signature") == expected,
                    "case %s: signature=%s expected %s" % (
                        case["id"], fields.get("signature"), expected))
            expected_ungated = str(case["expect_signature_ungated"])
            require(fields.get("signature_ungated") == expected_ungated,
                    "case %s: ungated signature=%s expected %s" % (
                        case["id"], fields.get("signature_ungated"),
                        expected_ungated))
            # A/B authority (V3-CONTROLLERS-01): a complete sovereign mapping
            # must make the fix DEFER (no second swap) unless force opt-in.
            if "expect_sovereign" in case:
                require(fields.get("sovereign") ==
                        str(case["expect_sovereign"]),
                        "case %s: sovereign=%s expected %s" % (
                            case["id"], fields.get("sovereign"),
                            case["expect_sovereign"]))
            if "expect_should_apply" in case:
                require(fields.get("should_apply") ==
                        str(case["expect_should_apply"]),
                        "case %s: should_apply=%s expected %s" % (
                            case["id"], fields.get("should_apply"),
                            case["expect_should_apply"]))

            if case.get("expect_mapping"):
                require(fields.get("mapping") == case["expect_mapping"],
                        "case %s: mapping\n  got %s\n  want %s" % (
                            case["id"], fields.get("mapping"),
                            case["expect_mapping"]))
            elif case["expect_signature"] == 0 or \
                    case.get("expect_should_apply") == 0:
                require("mapping" not in fields,
                        "case %s produced a mapping while refusing the fix"
                        % case["id"])

            if case["expect_signature"] == 0 and \
                    case["expect_signature_ungated"] == 1:
                require(fields.get("external_bus") == "0",
                        "case %s: only an internal bus may be saved by the gate"
                        % case["id"])
                internal_protected += 1
            if case["expect_signature"] == 1:
                external_fires += 1

    require(internal_protected >= 1,
            "no internal-bus case proves the BUS_HOST gate (H700 required)")
    require(external_fires >= 1,
            "no external HID case proves the fix still applies")
    print("NXINPUT PAD-ORDINAL GATE: PASS cases=%d protected_by_bus_gate=%d "
          "fires=%d" % (len(cases), internal_protected, external_fires))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateError as error:
        print("NXINPUT PAD-ORDINAL GATE: FAIL %s" % error, file=sys.stderr)
        sys.exit(1)
