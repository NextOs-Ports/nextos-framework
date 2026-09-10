#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free, fail-closed validator for the M15 nxinput contract."""

import hashlib
import json
import re
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[3]
LEDGER = Path("framework/nxinput/references/m15-input-contract-v1.json")
RUNTIME_RECEIPT = Path("framework/nxinput/references/m15-runtime-receipt-v1.json")
APPROVED = {
    "bully2": "ports/bully2",
    "sonic4ep2": "ports/sonic4",
    "horizonchase": "ports/horizonchase",
    "kotor": "ports/kotor",
    "asm2_127": "ports/asm2_127",
}
ITEM_IDS = tuple("M15-%03d" % n for n in range(1, 23))
ITEM_STATUS = {"contract_ready", "adapter_scoped", "closed_with_evidence",
               "closed"}
REF_RE = re.compile(r"^(?P<path>[A-Za-z0-9_./-]+):(?P<line>[1-9][0-9]*)$")
SENSITIVE = re.compile(
    r"(?:\b(?:10|127|169\.254|192\.168|172\.(?:1[6-9]|2\d|3[01]))"
    r"(?:\.\d{1,3}){2,3}\b|\broot@|/home/[^/]+/|"
    r"(?:password|credential|token)=)", re.IGNORECASE)


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
    require(not SENSITIVE.search(value), "sensitive literal in %s" % context)
    require("ports/tasm2" not in value.lower(),
            "unapproved legacy source named in %s" % context)


def validate_path(value):
    require(isinstance(value, str) and value, "empty path")
    pure = PurePosixPath(value)
    require(not pure.is_absolute() and ".." not in pure.parts,
            "unsafe path in ledger: %s" % value)
    require("\\" not in value, "backslash path in ledger: %s" % value)


def validate_ref(value, context):
    validate_text(value, context)
    match = REF_RE.fullmatch(value)
    require(match is not None, "invalid evidence reference %s" % value)
    relative = match.group("path")
    validate_path(relative)
    allowed = relative.startswith("framework/") or any(
        relative == prefix or relative.startswith(prefix + "/")
        for prefix in APPROVED.values())
    require(allowed, "evidence outside approved scope: %s" % value)
    path = ROOT / relative
    require(path.is_file() and not path.is_symlink(),
            "evidence file missing or symlinked: %s" % relative)
    require(int(match.group("line")) <=
            len(path.read_text(encoding="utf-8").splitlines()),
            "evidence line out of bounds: %s" % value)


def strings(values, context):
    require(isinstance(values, list) and values, "%s is empty" % context)
    for index, value in enumerate(values):
        validate_text(value, "%s[%d]" % (context, index))


def refs(values, context):
    require(isinstance(values, list) and values, "%s is empty" % context)
    for index, value in enumerate(values):
        validate_ref(value, "%s[%d]" % (context, index))


def validate_item(item, expected):
    require(set(item) == {"id", "status", "evidence_refs", "reusable",
                          "adapter_specific", "gaps", "tests"},
            "%s fields changed" % expected)
    require(item["id"] == expected and item["status"] in ITEM_STATUS,
            "%s id/status invalid" % expected)
    refs(item["evidence_refs"], expected + ".evidence_refs")
    for field in ("reusable", "adapter_specific", "gaps", "tests"):
        strings(item[field], expected + "." + field)


def validate_boundary(record, adapter_id, name):
    require(set(record) == {"status", "refs", "notes"},
            "%s.%s fields changed" % (adapter_id, name))
    validate_text(record["status"], "%s.%s.status" % (adapter_id, name))
    refs(record["refs"], "%s.%s.refs" % (adapter_id, name))
    strings(record["notes"], "%s.%s.notes" % (adapter_id, name))


def validate_adapter(adapter):
    require(set(adapter) == {"id", "port_path", "approved_status",
                             "identity_refs", "controls", "touch",
                             "runtime_status"},
            "adapter fields changed")
    adapter_id = adapter["id"]
    require(adapter_id in APPROVED and
            adapter["port_path"] == APPROVED[adapter_id],
            "adapter scope changed: %s" % adapter_id)
    require(adapter["approved_status"] == "approved-reference",
            "%s is not approved" % adapter_id)
    refs(adapter["identity_refs"], adapter_id + ".identity_refs")
    validate_boundary(adapter["controls"], adapter_id, "controls")
    validate_boundary(adapter["touch"], adapter_id, "touch")
    validate_text(adapter["runtime_status"], adapter_id + ".runtime_status")


def validate_runtime_receipt(receipt):
    require(receipt.get("schema") == "nxinput-m15-runtime-receipt-v1",
            "wrong M15 runtime receipt schema")
    require(receipt.get("schema_version") == 1 and
            receipt.get("milestone") == "M15",
            "wrong M15 runtime receipt version")
    validate_text(receipt["scope"], "runtime_receipt.scope")
    validate_text(receipt["method"], "runtime_receipt.method")
    require(receipt.get("session") == {
        "physical_profile_observed": "go_super",
        "virtual_profile_observed": "xbox_compatible_uinput",
        "runtime_stack": "arkos_aarch64_armhf_640x480",
        "frontend_final": "inactive_disabled",
        "game_processes_final": 0,
        "approved_runtime_adapters": ["sonic4ep2", "horizonchase", "kotor",
                                      "asm2_127"],
        "approved_acceptance_adapters": list(APPROVED),
        "manual_gameplay_revalidation_claimed": False,
        "virtual_input_claimed_as_physical": False,
        "unapproved_sources_used": False,
    }, "M15 runtime session boundary changed")
    adapters = receipt.get("adapters")
    expected_ids = list(APPROVED)
    require(isinstance(adapters, list) and
            [adapter.get("id") for adapter in adapters] == expected_ids,
            "M15 runtime adapter scope changed")
    fields = {"id", "basis", "mapping", "controls", "touch", "terminal",
              "status", "gaps"}
    for adapter in adapters:
        require(set(adapter) == fields, "M15 runtime adapter fields changed")
        for field in fields - {"gaps"}:
            validate_text(adapter[field], "runtime_receipt.%s.%s" %
                         (adapter["id"], field))
        strings(adapter["gaps"], "runtime_receipt.%s.gaps" % adapter["id"])
        require(adapter["status"] == "accepted_reference",
                "%s lost approved acceptance" % adapter["id"])
    require(receipt.get("closure") == {
        "m15_003": "closed_by_deterministic_core_test_and_runtime_hotplug_boundary",
        "m15_016": "closed_by_approved_adapter_receipts_and_runtime_sdl_evdev_observation",
        "m15_018": "closed_by_separate_go_super_observation_and_approved_twin_record",
        "m15_019": "closed_by_disconnect_release_tests_and_runtime_disconnect_reconnect_boundary",
        "m15_020": "closed_framework_matrix_with_five_approved_adapter_rows",
        "m15_022": "closed_for_framework",
        "scope_note": "Closure does not advertise an untested device or turn any physical mapping into a universal default",
    }, "M15 runtime closure changed")


def main():
    path = ROOT / LEDGER
    document = load_json(path)
    require(document.get("schema") == "nxinput-m15-input-contract-v1",
            "wrong M15 schema")
    require(document.get("schema_version") == 1 and
            document.get("milestone") == "M15" and
            document.get("contract_version") == "1.4.0",
            "wrong M15 version")
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
        "device_access": False,
        "network_access": False,
        "manual_gameplay_revalidation_claimed": False,
        "virtual_input_claimed_as_physical": False,
        "runtime_acceptance_items": ["M15-003", "M15-016", "M15-018",
                                     "M15-019", "M15-020", "M15-022"],
    }, "M15 safety boundary changed")

    universal = document.get("universal_core")
    require(isinstance(universal, dict), "universal core missing")
    for field in ("reusable", "adapter_only", "forbidden_defaults"):
        strings(universal.get(field), "universal_core." + field)
    forbidden = " ".join(universal["forbidden_defaults"]).lower()
    for token in ("guid", "device index", "cursor", "touch", "r3", "save",
                  "shut" + "down"):
        require(token in forbidden, "forbidden default lost %s" % token)

    reusable = " ".join(universal["reusable"]).lower()
    for token in ("per-pad d-pad", "host-wide portmaster stick-count hint",
                  "per-read d-pad to left-stick fallback"):
        require(token in reusable,
                "M15 1.3 topology contract lost %s" % token)
    for token in ("host-wide stick-count hint used as a per-pad",
                  "d-pad copied to a mapped physical left stick"):
        require(token in forbidden,
                "M15 1.3 fail-closed default lost %s" % token)

    implementation_tokens = {
        "framework/nxinput/include/nxinput.h": (
            "NXINPUT_PAD_CAP_LEFT_STICK",
            "NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING",
            "nxinput_get_pad_capabilities",
            "nxinput_host_analog_sticks_hint",
        ),
        "framework/nxinput/src/nxinput.c": (
            "SDL_GameControllerGetBindForButton",
            "SDL_GameControllerGetBindForAxis",
            "left_stick_binding_present",
            "NXINPUT_ANALOG_STICKS_HINT",
        ),
        "framework/nxinput/tests/test_nxinput.c": (
            "test_zero_stick_capabilities_and_opt_in",
            "test_one_stick_is_never_overridden",
            "nxinput partial stick",
            "capabilities == NXINPUT_PAD_CAP_MASK_ALL",
        ),
        "framework/nxbootstrap/templates/launcher.sh.in": (
            "ANALOGSTICKS:-${ANALOG_STICKS:-}",
            "NXBOOTSTRAP_ANALOG_STICKS_HINT",
            "NXINPUT_ANALOG_STICKS_HINT",
        ),
        "framework/nxinput/README.md": (
            "Topologia e fallback digital opt-in",
            "não adivinha um joystick raw",
        ),
        "framework/nxinput/include/nxinput_gptk_loader.h": (
            "nxinput_gptk_load_at",
            "nxinput-gptk-load-evidence/1",
            "DEFAULT_OWNER_REJECTED",
        ),
        "framework/nxinput/src/nxinput_gptk_loader.c": (
            "O_NOFOLLOW | O_NONBLOCK",
            "NXINPUT_GPTK_MAX_BYTES + 1u",
            "nxinput_gptk_validate_actions",
        ),
        "framework/nxinput/include/nxinput_gptk.h": (
            "nxinput_gptk_dispatcher_set_primary_mask",
            "nxinput_gptk_dispatcher_feed_source",
            "NXINPUT_GPTK_SOURCE_FALLBACK",
        ),
        "framework/nxinput/include/nxinput_exit_chord.h": (
            "nxinput_exit_chord_state_fn",
            "nxinput_exit_chord_poll",
            "nxinput_exit_chord_consume",
        ),
        "framework/nxinput/tests/test_gptk_loader.c": (
            "S_ISFIFO",
            "S_ISLNK",
            "DEFAULT_OWNER_MISSING",
        ),
    }
    for relative, tokens in implementation_tokens.items():
        source_path = ROOT / relative
        require(source_path.is_file() and not source_path.is_symlink(),
                "M15 1.3 source missing or symlinked: %s" % relative)
        source = source_path.read_text(encoding="utf-8")
        for token in tokens:
            require(token in source,
                    "M15 1.3 source %s lost token %s" % (relative, token))

    items = document.get("m15_items")
    require(isinstance(items, list) and len(items) == len(ITEM_IDS),
            "M15 item count changed")
    for item, expected in zip(items, ITEM_IDS):
        validate_item(item, expected)
    require(items[-1]["status"] == "closed", "M15 is not closed")
    require(not any("pending" in item["status"] for item in items),
            "M15 still contains a pending item")

    adapters = document.get("adapters")
    require(isinstance(adapters, list) and
            [entry.get("id") for entry in adapters] == list(APPROVED),
            "adapter profile order/scope changed")
    for adapter in adapters:
        validate_adapter(adapter)
    validate_runtime_receipt(load_json(ROOT / RUNTIME_RECEIPT))

    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    print("M15 input contract gate passed: items=22 adapters=5 "
          "closure=closed_for_framework sha256=%s" % digest)


if __name__ == "__main__":
    try:
        main()
    except (GateError, OSError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit("M15 input contract gate failed: %s" % error)
