#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure fail-closed validator for the M18 release closure receipt."""

import ast
import hashlib
import json
import re
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[3]
LEDGER = ROOT / "framework/nxrelease/m18-closure-v1.json"
TOOL = ROOT / "framework/nxrelease/nxrelease.py"
SCHEMA = ROOT / "framework/nxrelease/schema/nxrelease-v2.schema.json"
SUITE = ROOT / "framework/nxrelease/tests/test_nxrelease.sh"
RECOVERY = ROOT / "framework/nxrelease/RECOVERY.md"
ZIP_AUDITOR = ROOT / "framework/tests/audit-portmaster-zip.py"
ITEM_IDS = tuple("M18-%03d" % index for index in range(1, 26))
REF_RE = re.compile(r"^(?P<path>[A-Za-z0-9_./-]+):(?P<line>[1-9][0-9]*)$")
SENSITIVE = re.compile(
    r"(?:\b(?:10|127|169[.]254|192[.]168|172[.](?:1[6-9]|2\d|3[01]))"
    r"(?:[.]\d{1,3}){2,3}\b|/home/[^/]+/|/Users/[^/]+/|"
    r"(?:password|credential|token)=|ports/tasm2)",
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
    require(not SENSITIVE.search(value), "sensitive literal in %s" % context)


def validate_ref(value, context):
    validate_text(value, context)
    match = REF_RE.fullmatch(value)
    require(match is not None, "invalid evidence reference: %s" % value)
    relative = match.group("path")
    pure = PurePosixPath(relative)
    require(not pure.is_absolute() and ".." not in pure.parts and
            relative.startswith("framework/nxrelease/"),
            "evidence escaped nxrelease: %s" % relative)
    path = ROOT / relative
    require(path.is_file() and not path.is_symlink(),
            "missing/unsafe evidence file: %s" % relative)
    require(int(match.group("line")) <=
            len(path.read_text(encoding="utf-8").splitlines()),
            "evidence line out of range: %s" % value)


def validate_list(values, context, refs=False):
    require(isinstance(values, list) and values, "%s is empty" % context)
    for index, value in enumerate(values):
        if refs:
            validate_ref(value, "%s[%d]" % (context, index))
        else:
            validate_text(value, "%s[%d]" % (context, index))


def literal_assignment(tree, name):
    for node in tree.body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1 and
                isinstance(node.targets[0], ast.Name) and
                node.targets[0].id == name):
            value = node.value
            if (isinstance(value, ast.Call) and
                    isinstance(value.func, ast.Name) and
                    value.func.id in ("frozenset", "set", "tuple") and
                    len(value.args) == 1 and not value.keywords):
                value = value.args[0]
            return ast.literal_eval(value)
    raise GateError("missing literal assignment %s" % name)


def main():
    document = load_json(LEDGER)
    require(document.get("schema") == "nxrelease-m18-closure-v1" and
            document.get("schema_version") == 1 and
            document.get("milestone") == "M18",
            "wrong M18 closure schema")
    require(document.get("status") == "closed", "M18 is not closed")
    validate_text(document.get("scope"), "scope")
    validate_text(document.get("method"), "method")
    require(document.get("safety") == {
        "game_execution": False,
        "guest_code_execution": False,
        "device_access": False,
        "network_access": False,
        "existing_release_overwrite": False,
    }, "M18 safety boundary changed")

    items = document.get("items")
    require(isinstance(items, list) and len(items) == 25,
            "M18 must contain exactly 25 items")
    for item, expected in zip(items, ITEM_IDS):
        require(set(item) == {"id", "status", "evidence_refs", "guarantees",
                              "residual_scope", "tests"},
                "%s fields changed" % expected)
        require(item["id"] == expected and item["status"] == "closed",
                "%s is not closed or is out of order" % expected)
        validate_list(item["evidence_refs"], expected + ".evidence_refs", refs=True)
        for field in ("guarantees", "residual_scope", "tests"):
            validate_list(item[field], expected + "." + field)

    tool_text = TOOL.read_text(encoding="utf-8")
    tree = ast.parse(tool_text, filename=str(TOOL))
    allowed = set(literal_assignment(tree, "ALLOWED_KINDS"))
    require(allowed == {
        "launcher", "script", "payload", "project-linux",
        "third-party-linux", "nxsplash-linux", "nxextract", "nxextract-runner",
        "nxextract-recipe", "nxextract-runtime-env", "nxextract-ui-linux",
        "nxbootstrap-config",
        "portmaster-metadata", "portmaster-image", "license-notice",
        "nxruntime-generation", "nxruntime-generation-linux",
        # V4-REPACK-01: the visible seed the launcher rebuilds the local cache
        # from. It is a sealed package member like any other.
        "nxruntime-seed",
    }, "release allowlist changed")
    require("android-upstream" not in tool_text,
            "Android/BYO ELF regained a public package path")
    suffixes = set(literal_assignment(tree, "FORBIDDEN_SUFFIXES"))
    require({".apk", ".apkm", ".apks", ".xapk", ".obb", ".jar", ".dex"}
            <= suffixes, "proprietary Android suffix barrier weakened")
    bionic = set(literal_assignment(tree, "BIONIC_SONAMES"))
    require({"libc.so", "libm.so", "libdl.so", "liblog.so",
             "libandroid.so", "libOpenSLES.so"} <= bionic,
            "Bionic masquerade barrier weakened")
    for token in (
            "package.license is required for a public release",
            "metadata package.license is required for a public release",
            "HOST_LITERAL_RE", "portable_path_key", "rename_noreplace",
            "publish_archive_pair", "verify_archive(final_archive",
            "archive contains symlink", "archive contains case-insensitive collision",
            "PORTMASTER_ZIP_AUDITOR_PATH", "audit_public_portmaster_zip",
            "shell_invokes_external_stat", "record_is_shell",
            "parse_json_strict", "validate_portmaster_item_list",
            "validate_installation_document", "previous_archive",
            "portmaster_metadata.port_json is required for a public release"):
        require(token in tool_text, "nxrelease lost invariant: %s" % token)

    schema = load_json(SCHEMA)
    package = schema["properties"]["package"]
    require("license" in package.get("required", []),
            "schema no longer requires package.license")
    require("portmaster_metadata" in schema.get("required", []),
            "schema no longer requires portmaster_metadata")
    portmaster = schema["properties"]["portmaster_metadata"]
    require("port_json" in portmaster.get("required", []),
            "schema no longer requires portmaster_metadata.port_json")
    kinds = set(schema["$defs"]["file"]["properties"]["kind"]["enum"])
    require(kinds == allowed, "schema and runtime allowlists diverged")

    suite = SUITE.read_text(encoding="utf-8")
    for token in (
            "manifest-traversal", "source-symlink", "zip-traversal",
            "zip-symlink", "zip-unicode", "android-byo",
            "android-mislabelled", "license-missing",
            "forbidden-hostname", "for android_suffix in apk obb dex",
            "loader-sectionless", "race-archive",
            "race-checksum", "race-bundle", "no-overwrite",
            "missing-splash", "relabelled-splash", "altered-splash",
            "missing-extract-ui", "relabelled-extract-ui", "exact-apk",
            "cross-arch-extract-ui", "self-pinned-extract-runner",
            "nxport-splash-handoff", "nxport-splash-order",
            "nxport-splash-disable", "path-without-stat",
            "portmaster-metadata-missing", "port-json-missing",
            "installation-missing", "installation-portuguese-missing",
            "archive-metadata-duplicate", "archive-metadata-nan",
            "archive-metadata-bom", "--previous-archive",
            "nxrelease shell stat gate passed",
            "nxrelease independent real-ZIP audit passed",
            "nxrelease APK variant regressions passed",
            "nxrelease mixed-ABI gate passed",
            "metadata=10 strict_json=5 publication_contract=4",
            "nxrelease archive strict JSON regressions passed: cases=3"):
        require(token in suite, "M18 adversarial suite lacks: %s" % token)
    auditor = ZIP_AUDITOR.read_text(encoding="utf-8")
    for token in ("shell_shebang_interpreter", "shell_invokes_external_stat",
                  "retired legacy nxbootstrap.sh", "audit_shell"):
        require(token in auditor,
                "independent ZIP auditor lacks: %s" % token)
    recovery = RECOVERY.read_text(encoding="utf-8")
    for token in ("RENAME_NOREPLACE", "rollback", "Reauditoria independente",
                  "release anterior"):
        require(token in recovery, "recovery guide lacks: %s" % token)

    digest = hashlib.sha256(LEDGER.read_bytes()).hexdigest()
    print("M18 closure gate passed: items=25 status=closed "
          "android_byo_packaging=forbidden sha256=%s" % digest)


if __name__ == "__main__":
    try:
        main()
    except (GateError, OSError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit("M18 closure gate failed: %s" % error)
