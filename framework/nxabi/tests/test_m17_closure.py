#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure fail-closed validator for the M17 closure receipt."""

import hashlib
import json
import re
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[3]
LEDGER = ROOT / "framework/nxabi/m17-closure-v1.json"
POLICY = ROOT / "framework/nxabi/policy-v1.json"
PIN = ROOT / "framework/nxabi/TOOLCHAIN-PIN.json"
REPORT = ROOT / "framework/nxabi/m17-reference-audit-v1.json"
GATE = ROOT / "framework/nxabi/tools/nx-abi-gate.sh"
NXABI = ROOT / "framework/nxabi/nxabi.py"
ITEM_IDS = tuple("M17-%03d" % index for index in range(1, 21))
APPROVED_PREFIXES = (
    "ports/bully2/",
    "ports/sonic4/",
    "ports/horizonchase/",
    "ports/kotor/",
    "ports/asm2_127/",
)
REFERENCE_ELFS = {
    "ports/kotor/kotor-universal",
    "ports/kotor/libkotor_input-universal.so",
    "ports/kotor/nxextract-ui",
    "ports/asm2_127/asm2_127-universal",
    "ports/sonic4/sonic4.arm64",
    "ports/bully2/bully.glibc230",
    "ports/horizonchase/horizonchase-universal",
}
REF_RE = re.compile(r"^(?P<path>[A-Za-z0-9_./-]+):(?P<line>[1-9][0-9]*)$")
SENSITIVE = re.compile(
    r"(?:\b(?:10|127|169[.]254|192[.]168|172[.](?:1[6-9]|2\d|3[01]))"
    r"(?:[.]\d{1,3}){2,3}\b|/home/[^/]+/|/Users/[^/]+/|"
    r"(?:password|credential|token)=|ports/tasm2|ports/sor4)",
    re.IGNORECASE,
)


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


def validate_text(value, context):
    require(isinstance(value, str) and value.strip(), "%s is empty" % context)
    require(not SENSITIVE.search(value), "sensitive/out-of-scope text in %s" % context)


def validate_ref(value, context):
    validate_text(value, context)
    match = REF_RE.fullmatch(value)
    require(match is not None, "invalid evidence reference: %s" % value)
    relative = match.group("path")
    pure = PurePosixPath(relative)
    require(not pure.is_absolute() and ".." not in pure.parts,
            "unsafe evidence path: %s" % relative)
    require(relative.startswith("framework/nxabi/") or
            relative.startswith("framework/nxrelease/"),
            "M17 evidence escaped framework scope: %s" % relative)
    path = ROOT / relative
    require(path.is_file() and not path.is_symlink(),
            "missing/unsafe evidence file: %s" % relative)
    line = int(match.group("line"))
    require(line <= len(path.read_text(encoding="utf-8").splitlines()),
            "evidence line out of range: %s" % value)


def validate_list(values, context, refs=False):
    require(isinstance(values, list) and values, "%s is empty" % context)
    for index, value in enumerate(values):
        if refs:
            validate_ref(value, "%s[%d]" % (context, index))
        else:
            validate_text(value, "%s[%d]" % (context, index))


def main():
    document = load_json(LEDGER)
    require(document.get("schema") == "nxabi-m17-closure-v1" and
            document.get("schema_version") == 1 and
            document.get("milestone") == "M17",
            "wrong M17 closure schema")
    require(document.get("status") ==
            "closed_for_framework_and_new_public_artifacts",
            "M17 is not closed")
    validate_text(document.get("scope"), "scope")
    validate_text(document.get("method"), "method")
    require(document.get("safety") == {
        "inspected_elf_execution": False,
        "guest_code_execution": False,
        "device_access": False,
        "network_access": False,
        "reference_ports_modified": False,
    }, "M17 safety boundary changed")

    items = document.get("items")
    require(isinstance(items, list) and len(items) == 20,
            "M17 must contain exactly 20 items")
    for item, expected in zip(items, ITEM_IDS):
        require(set(item) == {"id", "status", "evidence_refs", "guarantees",
                              "residual_scope", "tests"},
                "%s fields changed" % expected)
        require(item["id"] == expected and item["status"] == "closed",
                "%s is not closed or is out of order" % expected)
        validate_list(item["evidence_refs"], expected + ".evidence_refs", refs=True)
        for field in ("guarantees", "residual_scope", "tests"):
            validate_list(item[field], expected + "." + field)

    policy = load_json(POLICY)
    require(policy.get("ceilings") == {
        "glibc_max": "2.30",
        "glibc_preferred": "2.17",
        "glibcxx_max": "3.4.25",
        "cxxabi_max": "1.3.11",
    }, "M17 ABI ceilings changed")
    require(policy.get("build_profiles", {}).get("nextos-current", {}).get("public")
            is False, "NextOS-current became a public profile")

    pin = load_json(PIN)
    architectures = {
        entry.get("architecture")
        for entry in pin.get("toolchains", {}).values()
    }
    require(architectures == {"armv7", "aarch64"},
            "toolchain pin lost an architecture")
    require(set(pin.get("non_hermetic_recipes", {}).get("paths", [])) == {
        "ports/kotor/build_universal.sh",
        "ports/asm2_127/build_buster_arkos.sh",
        "ports/bully2/build-glibc230.sh",
    }, "historical recipe debt escaped the approved scope")

    report = load_json(REPORT)
    require(report.get("counts") == {"elves": 7, "errors": 1, "warnings": 9},
            "reference audit counts changed")
    finding_paths = {finding.get("path") for finding in report.get("findings", [])}
    require(finding_paths <= REFERENCE_ELFS,
            "reference report contains an unknown/out-of-scope artifact")
    for path in finding_paths:
        pure = PurePosixPath(path)
        require(not pure.is_absolute() and ".." not in pure.parts and
                path.startswith(APPROVED_PREFIXES),
                "unsafe reference report path: %s" % path)
    errors = [
        (finding.get("path"), finding.get("check"))
        for finding in report.get("findings", [])
        if finding.get("level") == "error"
    ]
    require(errors == [
        ("ports/horizonchase/horizonchase-universal", "search-path")
    ], "documented historical reference error changed")

    gate_text = GATE.read_text(encoding="utf-8")
    require('--json "$WORK/m17-reference-audit-v1.json"' in gate_text and
            '--json "$REPO_ROOT/framework/nxabi/m17-reference-audit-v1.json"'
            not in gate_text,
            "M17 gate writes a host-specific report into the repository")
    # 22/08/2026: o passo report-only aceita arvore SEM os loaders de
    # referencia (o gitignore barra binario compilado no repo), mas so' de
    # forma HONESTA: ausencia e' relatada, alvo presente segue rigoroso, erro
    # nao documentado reprova e o recibo byte a byte continua exigido quando o
    # conjunto esta' completo. O guard agora prova essas quatro travas -- um
    # pulo silencioso continua impossivel.
    require('[[ -x $ARMHF_GCC ]] || fail' in gate_text and
            "reference absent in this tree" in gate_text and
            "undocumented" in gate_text and
            "receipt comparison requires the full reference set" in gate_text and
            "checked-in reference audit is stale" in gate_text,
            "M17 gate lost the honest partial-reference contract")
    require('if status != "ok":' in NXABI.read_text(encoding="utf-8"),
            "toolchain command no longer fails on an absent pin")

    digest = hashlib.sha256(LEDGER.read_bytes()).hexdigest()
    print("M17 closure gate passed: items=20 status=closed "
          "historical_reference_errors=1 sha256=%s" % digest)


if __name__ == "__main__":
    try:
        main()
    except (GateError, OSError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit("M17 closure gate failed: %s" % error)
