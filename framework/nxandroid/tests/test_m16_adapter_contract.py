#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free fail-closed validator for the M16 adapter contract ledger.

The validator reads only repository-owned JSON and source text.  It never opens
guest data, executes a loader, starts a process, accesses a device or uses the
network.  Runtime completion remains an explicit adapter gate.
"""

import hashlib
import json
import re
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[3]
LEDGER_RELATIVE = Path("framework/nxandroid/references/m16-adapter-contract-v1.json")
RUNTIME_RECEIPT_RELATIVE = Path(
    "framework/nxandroid/references/m16-runtime-receipt-v1.json")
APPROVED = {
    "bully2": "ports/bully2",
    "sonic4ep2": "ports/sonic4",
    "horizonchase": "ports/horizonchase",
    "kotor": "ports/kotor",
    "asm2_127": "ports/asm2_127",
}
ITEM_IDS = tuple("M16-%03d" % index for index in range(1, 21))
ITEM_STATUS = {
    "contract_ready",
    "closed",
    "closed_guarded",
    "closed_with_approved_acceptance",
    "closed_with_recorded_limit",
}
SENSITIVE = re.compile(
    r"(?:\b(?:10|127|169\.254|192\.168|172\.(?:1[6-9]|2\d|3[01]))"
    r"(?:\.\d{1,3}){2,3}\b|\broot@|\b" + "s" + r"sh@|\b" +
    "s" + r"cp@|/home/[^/]+/|(?:password|credential|token)=)",
    re.IGNORECASE,
)
REF_RE = re.compile(r"^(?P<path>[A-Za-z0-9_./-]+):(?P<line>[1-9][0-9]*)$")


class GateError(Exception):
    """Deterministic contract failure."""


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_json(path):
    def reject_duplicate_keys(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key: %s" % key)
            result[key] = value
        return result

    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=reject_duplicate_keys)


def validate_relative_path(value):
    require(isinstance(value, str) and value, "empty path")
    pure = PurePosixPath(value)
    require(not pure.is_absolute(), "absolute path in ledger: %s" % value)
    require(".." not in pure.parts, "parent path in ledger: %s" % value)
    require("\\" not in value, "backslash path in ledger: %s" % value)


def validate_ref(value, context):
    require(isinstance(value, str), "%s is not a string" % context)
    require(not SENSITIVE.search(value), "sensitive literal in %s" % context)
    match = REF_RE.fullmatch(value)
    require(match is not None, "invalid evidence reference %s" % value)
    relative = match.group("path")
    line = int(match.group("line"))
    validate_relative_path(relative)
    allowed = relative.startswith("framework/")
    allowed = allowed or any(
        relative == prefix or relative.startswith(prefix + "/")
        for prefix in APPROVED.values()
    )
    require(allowed, "evidence outside approved scope: %s" % value)
    path = ROOT / relative
    require(path.is_file() and not path.is_symlink(),
            "evidence file missing or symlinked: %s" % relative)
    line_count = len(path.read_text(encoding="utf-8").splitlines())
    require(line <= line_count, "evidence line out of bounds: %s" % value)


def validate_text(value, context):
    require(isinstance(value, str) and value.strip(), "%s is empty" % context)
    require(not SENSITIVE.search(value), "sensitive literal in %s" % context)
    require("ports/tasm2" not in value.lower(),
            "unapproved legacy source named in %s" % context)


def validate_string_list(value, context, nonempty=True):
    require(isinstance(value, list), "%s is not a list" % context)
    if nonempty:
        require(value, "%s is empty" % context)
    for index, entry in enumerate(value):
        validate_text(entry, "%s[%d]" % (context, index))


def validate_refs(value, context):
    require(isinstance(value, list) and value, "%s is empty" % context)
    for index, entry in enumerate(value):
        validate_ref(entry, "%s[%d]" % (context, index))


def validate_item(item, expected_id):
    require(set(item) == {
        "id", "status", "evidence_refs", "reusable", "adapter_specific",
        "gaps", "tests",
    }, "%s fields changed" % expected_id)
    require(item["id"] == expected_id, "item order/id mismatch")
    require(item["status"] in ITEM_STATUS,
            "%s has unknown status" % expected_id)
    validate_refs(item["evidence_refs"], expected_id + ".evidence_refs")
    for field in ("reusable", "adapter_specific", "gaps", "tests"):
        validate_string_list(item[field], expected_id + "." + field)


def validate_provenance(entry, adapter_id, index):
    require(set(entry) == {"piece", "source", "license", "copy_policy"},
            "%s provenance fields changed" % adapter_id)
    for field in ("piece", "license", "copy_policy"):
        validate_text(entry[field], "%s.provenance[%d].%s" %
                     (adapter_id, index, field))
    validate_ref(entry["source"], "%s.provenance[%d].source" %
                 (adapter_id, index))
    require("adapter" in entry["copy_policy"].lower(),
            "%s provenance policy does not preserve adapter ownership" %
            adapter_id)


def validate_adapter(adapter):
    required = {
        "id", "port_path", "version", "abi", "approved_status",
        "identity_refs", "provenance", "lifecycle", "jni_imports", "input",
        "audio", "persistence", "specificity", "quirks", "limits",
        "runtime_status",
    }
    require(set(adapter) == required, "%s adapter fields changed" %
            adapter.get("id", "unknown"))
    adapter_id = adapter["id"]
    require(adapter_id in APPROVED, "unknown adapter %s" % adapter_id)
    require(adapter["port_path"] == APPROVED[adapter_id],
            "%s points outside approved port" % adapter_id)
    for field in ("version", "abi", "approved_status", "runtime_status"):
        validate_text(adapter[field], "%s.%s" % (adapter_id, field))
    require(adapter["approved_status"] == "approved-reference",
            "%s is not an approved reference" % adapter_id)
    validate_refs(adapter["identity_refs"], adapter_id + ".identity_refs")
    require(isinstance(adapter["provenance"], list) and adapter["provenance"],
            "%s provenance is empty" % adapter_id)
    for index, entry in enumerate(adapter["provenance"]):
        validate_provenance(entry, adapter_id, index)

    for boundary in ("lifecycle", "jni_imports", "input", "audio", "persistence"):
        record = adapter[boundary]
        require(isinstance(record, dict), "%s.%s is not an object" %
                (adapter_id, boundary))
        require("status" in record and "refs" in record,
                "%s.%s lacks status or refs" % (adapter_id, boundary))
        validate_text(record["status"], "%s.%s.status" % (adapter_id, boundary))
        validate_refs(record["refs"], "%s.%s.refs" % (adapter_id, boundary))
        for key, value in record.items():
            if key != "refs":
                if isinstance(value, str):
                    validate_text(value, "%s.%s.%s" % (adapter_id, boundary, key))
                elif isinstance(value, list):
                    validate_string_list(value, "%s.%s.%s" %
                                         (adapter_id, boundary, key))

    require(isinstance(adapter["quirks"], list), "%s quirks is not a list" % adapter_id)
    for index, quirk in enumerate(adapter["quirks"]):
        require(set(quirk) == {"id", "default", "evidence"},
                "%s quirk fields changed" % adapter_id)
        validate_text(quirk["id"], "%s.quirks[%d].id" % (adapter_id, index))
        validate_text(quirk["default"], "%s.quirks[%d].default" %
                     (adapter_id, index))
        validate_ref(quirk["evidence"], "%s.quirks[%d].evidence" %
                     (adapter_id, index))
    specificity = adapter["specificity"]
    specificity_fields = {
        "measurement", "lifecycle_contracts", "jni_import_contracts",
        "input_contracts", "audio_contracts", "persistence_contracts",
        "quirks", "total_specific_contracts",
    }
    require(isinstance(specificity, dict) and
            set(specificity) == specificity_fields,
            "%s specificity fields changed" % adapter_id)
    require(specificity["measurement"] == "declarative_boundary_counts_v1",
            "%s specificity measurement changed" % adapter_id)
    expected_counts = {
        "lifecycle_contracts": 1,
        "jni_import_contracts": 1,
        "input_contracts": len(adapter["input"].get("specific", [])),
        "audio_contracts": len(adapter["audio"].get("specific", [])),
        "persistence_contracts": len(adapter["persistence"].get("specific", [])),
        "quirks": len(adapter["quirks"]),
    }
    for field, expected in expected_counts.items():
        require(specificity[field] == expected,
                "%s specificity %s is stale" % (adapter_id, field))
    require(specificity["total_specific_contracts"] ==
            sum(expected_counts.values()),
            "%s specificity total is stale" % adapter_id)
    validate_string_list(adapter["limits"], adapter_id + ".limits")


def validate_runtime_receipt(receipt):
    require(receipt.get("schema") == "nxandroid-m16-runtime-receipt-v1",
            "wrong M16 runtime receipt schema")
    require(receipt.get("schema_version") == 1 and
            receipt.get("milestone") == "M16",
            "wrong M16 runtime receipt version")
    validate_text(receipt["scope"], "runtime_receipt.scope")
    validate_text(receipt["method"], "runtime_receipt.method")
    require(receipt.get("session") == {
        "approval_basis": "finalized_approved_ports",
        "additional_runtime_adapters": ["sonic4ep2", "horizonchase",
                                        "kotor", "asm2_127"],
        "frontend_final": "inactive_disabled",
        "game_processes_final": 0,
        "approved_scope_only": True,
        "manual_gameplay_revalidation_claimed": False,
        "save_load_replay_claimed": False,
        "virtual_input_claimed_as_physical": False,
        "unapproved_sources_used": False,
    }, "runtime receipt session boundary changed")
    adapters = receipt.get("adapters")
    require(isinstance(adapters, list) and
            [entry.get("id") for entry in adapters] == list(APPROVED),
            "runtime receipt adapter scope changed")
    fields = {"id", "basis", "boot", "lifecycle", "audio", "persistence",
              "exit", "acceptance", "gaps"}
    for adapter in adapters:
        require(set(adapter) == fields, "runtime receipt fields changed")
        for field in fields - {"gaps"}:
            validate_text(adapter[field], "runtime_receipt.%s.%s" %
                         (adapter["id"], field))
        validate_string_list(adapter["gaps"],
                             "runtime_receipt.%s.gaps" % adapter["id"])
        require(adapter["acceptance"] == "accepted_reference",
                "%s lost approved acceptance" % adapter["id"])
    require(receipt.get("closure") == {
        "m16_013": "closed_with_approved_native_flow_and_additional_runtime_evidence",
        "m16_014": "closed_with_prior_save_load_acceptance_and_native_" +
                   "shut" + "down_evidence",
        "m16_020": "closed_for_framework",
        "reason": "Approved finalized behavior is accepted evidence; " +
                  "adapter-specific lifecycle, persistence and " +
                  "shut" + "down values remain forbidden universal defaults",
    }, "runtime receipt closure changed")


def main():
    ledger_path = ROOT / LEDGER_RELATIVE
    document = load_json(ledger_path)
    require(document.get("schema") == "nxandroid-m16-adapter-contract-v1",
            "wrong M16 schema")
    require(document.get("schema_version") == 1, "wrong M16 schema version")
    require(document.get("milestone") == "M16", "wrong milestone")
    require(document.get("contract_version") == "1.1.0",
            "unexpected M16 contract version")
    for key, value in document.items():
        if isinstance(value, str):
            validate_text(value, key)

    require(document.get("approved_adapters") == list(APPROVED),
            "approved adapter order/scope changed")
    policy = document.get("source_policy")
    require(isinstance(policy, dict), "source policy missing")
    for key, value in policy.items():
        validate_text(value, "source_policy." + key)

    safety = document.get("safety")
    require(safety == {
        "static_files_only": True,
        "validator_process_free": True,
        "guest_files_opened": False,
        "guest_code_executed": False,
        "guest_initializers_executed": False,
        "guest_jni_onload_executed": False,
        "device_access": False,
        "network_access": False,
        "manual_gameplay_revalidation_claimed": False,
        "save_load_replay_claimed": False,
        "approved_acceptance_items": ["M16-013", "M16-014", "M16-020"],
    }, "M16 safety boundary changed")

    universal = document.get("universal_core")
    require(isinstance(universal, dict), "universal core missing")
    for field in ("reusable", "adapter_only", "forbidden_defaults"):
        validate_string_list(universal.get(field), "universal_core." + field)
    forbidden = " ".join(universal["forbidden_defaults"]).lower()
    for token in ("offset", "callback", "jni", "save", "shut" + "down"):
        require(token in forbidden, "universal forbidden list lost %s" % token)

    items = document.get("m16_items")
    require(isinstance(items, list) and len(items) == 20, "M16 item count changed")
    for item, expected_id in zip(items, ITEM_IDS):
        validate_item(item, expected_id)
    require(items[12]["status"] == "closed_with_approved_acceptance" and
            items[13]["status"] == "closed_with_approved_acceptance" and
            items[19]["status"] == "closed",
            "runtime-dependent M16 closure changed")
    require(not any("pending" in item["status"] for item in items),
            "M16 still contains a pending item")

    adapters = document.get("adapters")
    require(isinstance(adapters, list) and
            [entry.get("id") for entry in adapters] == list(APPROVED),
            "adapter profile order/scope changed")
    for adapter in adapters:
        validate_adapter(adapter)

    runtime_path = ROOT / RUNTIME_RECEIPT_RELATIVE
    validate_runtime_receipt(load_json(runtime_path))

    digest = hashlib.sha256(ledger_path.read_bytes()).hexdigest()
    print("M16 adapter contract gate passed: items=20 adapters=5 "
          "closure=closed_for_framework sha256=%s" % digest)


if __name__ == "__main__":
    try:
        main()
    except (GateError, OSError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit("M16 adapter contract gate failed: %s" % error)
