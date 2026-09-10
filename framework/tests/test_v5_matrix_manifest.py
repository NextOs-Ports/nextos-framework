#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V5 I2: o manifesto FINITO e COMPLETO da matriz obrigatoria (secao 9).

Antes disto a cobertura da matriz vivia em checkboxes de um documento de
missao: um caso podia ficar em branco para sempre sem nada reclamar, "todos os
devices" nao tinha lista fechada, e a diferenca entre "nao foi feito" e "o
aparelho nao existe" era so a redacao da anotacao.

Este gate cobra o manifesto, nao a opiniao:

  * a contagem e o digest sao CONGELADOS -- acrescentar, remover ou reescrever
    um caso e ato deliberado (o commit tem de mexer nos dois);
  * todo caso tem exatamente um estado do conjunto fechado;
  * PROVED e PARTIAL nomeiam a evidencia; PARTIAL tambem nomeia o que falta;
  * BLOCKED_DEVICE nomeia o hardware ausente E o kit/fixture que fecha depois
    -- e a unica saida legitima para item que nao e de software;
  * UNPROVEN nomeia o DONO (a fatia de missao que fecha): nenhum item de
    software pode ficar sem quem o feche;
  * NOT_APPLICABLE nomeia o motivo;
  * ids unicos, ordenados e coerentes com a secao.
"""
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "framework/tests/v5-matrix-manifest-v1.json"
STATUSES = {"PROVED", "PARTIAL", "UNPROVEN", "BLOCKED_DEVICE",
            "NOT_APPLICABLE"}
ID = re.compile(r"^(9\.[1-4])\.([0-9]{2})$")


def require(condition, message):
    if not condition:
        print("v5_matrix_manifest=FAIL " + message)
        sys.exit(1)


require(MANIFEST.is_file() and not MANIFEST.is_symlink(),
        "manifest is missing")
document = json.loads(MANIFEST.read_text(encoding="utf-8"))
require(document.get("schema") == "nextos-v5-matrix/1" and
        document.get("schema_version") == 1, "schema is unsupported")
cases = document.get("cases")
require(isinstance(cases, list) and cases, "cases is not a list")

seen = set()
previous = ""
for case in cases:
    require(isinstance(case, dict), "case is not an object")
    identifier = case.get("id")
    match = ID.match(identifier or "")
    require(match is not None, "case id is malformed: %r" % identifier)
    require(identifier not in seen, "duplicate case id: %s" % identifier)
    seen.add(identifier)
    require(identifier > previous, "cases are not in order at %s" % identifier)
    previous = identifier
    require(case.get("section") == match.group(1),
            "%s: section does not match the id" % identifier)
    require(isinstance(case.get("case"), str) and case["case"].strip(),
            "%s: empty case text" % identifier)
    status = case.get("status")
    require(status in STATUSES, "%s: unknown status %r" % (identifier, status))
    unknown = set(case) - {"id", "section", "case", "status", "evidence",
                           "missing", "blocked_by", "closes_with", "owner"}
    require(not unknown, "%s: unexpected fields %s" % (identifier,
                                                       sorted(unknown)))

    def named(field):
        value = case.get(field)
        return isinstance(value, str) and value.strip()

    if status in ("PROVED", "PARTIAL"):
        require(named("evidence"),
                "%s is %s but names no evidence" % (identifier, status))
    if status == "PARTIAL":
        require(named("missing"),
                "%s is PARTIAL but does not say what is missing" % identifier)
        require(named("owner"),
                "%s is PARTIAL but names no owner for the rest" % identifier)
    if status == "BLOCKED_DEVICE":
        require(named("blocked_by"),
                "%s is BLOCKED_DEVICE but does not name the absent hardware"
                % identifier)
        require(named("closes_with"),
                "%s is BLOCKED_DEVICE but names no kit/fixture that closes it"
                % identifier)
        require(not named("owner") or True, "")
    if status == "UNPROVEN":
        require(named("owner"),
                "%s is UNPROVEN (software) but names no owner: an item with "
                "no one to close it is how a matrix rots" % identifier)
        require(not named("blocked_by"),
                "%s is UNPROVEN but claims absent hardware: use "
                "BLOCKED_DEVICE" % identifier)
    if status == "NOT_APPLICABLE":
        require(named("evidence") or named("missing"),
                "%s is NOT_APPLICABLE with no reason" % identifier)

# ---- device classes: "todos os devices" as a CLOSED list -------------------
# A matrix of cases is not enough: the mission's phrase "todos os devices" only
# means something when the device/provider classes are enumerated, each one
# saying what is MEASURED, what is expected after the provider fix, and HOW to
# measure what is missing. UNMEASURED is never presumed equivalence.
DEVICE_STATUSES = {"MEASURED", "MEASURED_BY_DSO", "MEASURED_BY_SOURCE",
                   "MEASURED_BYTES_ONLY", "UNMEASURED", "BLOCKED_DEVICE"}
devices = document.get("device_classes")
require(isinstance(devices, dict), "device_classes is missing")
entries = devices.get("classes")
require(isinstance(entries, list) and entries, "device_classes has no class")
require(devices.get("frozen_count") == len(entries),
        "device_classes frozen_count %r does not match %d classes"
        % (devices.get("frozen_count"), len(entries)))
seen_devices = set()
for entry in entries:
    require(isinstance(entry, dict), "a device class is not an object")
    identifier = entry.get("id")
    require(isinstance(identifier, str) and identifier and
            identifier not in seen_devices,
            "device class id is missing or duplicated: %r" % identifier)
    seen_devices.add(identifier)
    status = entry.get("status")
    require(status in DEVICE_STATUSES,
            "%s: unknown device status %r" % (identifier, status))
    for field in ("cfw", "sdl", "today", "expected_after_provider_fix",
                  "how_to_measure"):
        value = entry.get(field)
        require(isinstance(value, str) and value.strip(),
                "%s: device class field %r is empty" % (identifier, field))
    require(isinstance(entry.get("device_on_hand"), bool),
            "%s: device_on_hand must be a boolean" % identifier)
    if status == "UNMEASURED":
        require(entry.get("device_on_hand") is False or
                "fixture" in entry["how_to_measure"],
                "%s is UNMEASURED with the device on hand and no fixture "
                "route: an unmeasured class that could be measured today is "
                "a gap, not a limit" % identifier)

require(document.get("frozen_count") == len(cases),
        "frozen_count %r does not match %d cases"
        % (document.get("frozen_count"), len(cases)))
digest = hashlib.sha256()
for case in cases:
    digest.update(("%s\0%s\0%s\n" % (case["id"], case["case"],
                                     case["status"])).encode("utf-8"))
for entry in entries:
    digest.update(("class\0%s\0%s\n" % (entry["id"],
                                          entry["status"])).encode("utf-8"))
require(document.get("frozen_digest") == digest.hexdigest(),
        "frozen_digest is stale: the case list or a status moved without the "
        "manifest being re-frozen (recorded %s, measured %s)"
        % (str(document.get("frozen_digest"))[:16], digest.hexdigest()[:16]))

counts = {}
for case in cases:
    counts[case["status"]] = counts.get(case["status"], 0) + 1
require(document.get("counts") == counts,
        "counts %r do not match the cases %r" % (document.get("counts"),
                                                 counts))

blocked = sorted(c["id"] for c in cases if c["status"] == "BLOCKED_DEVICE")
unproven = sorted(c["id"] for c in cases if c["status"] == "UNPROVEN")
print("v5_matrix_manifest=PASS cases=%d proved=%d partial=%d unproven=%d "
      "blocked_device=%d not_applicable=%d" % (
          len(cases), counts.get("PROVED", 0), counts.get("PARTIAL", 0),
          counts.get("UNPROVEN", 0), counts.get("BLOCKED_DEVICE", 0),
          counts.get("NOT_APPLICABLE", 0)))
print("v5_matrix_manifest blocked_device=%s" % ",".join(blocked))
print("v5_matrix_manifest unproven=%s" % ",".join(unproven))
device_counts = {}
for entry in entries:
    device_counts[entry["status"]] = device_counts.get(entry["status"], 0) + 1
print("v5_matrix_manifest device_classes=%d %s" % (
    len(entries), " ".join("%s=%d" % kv for kv in sorted(device_counts.items()))))
