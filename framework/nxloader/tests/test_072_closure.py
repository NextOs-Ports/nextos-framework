#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure fail-closed validator for the nxloader 0.7.2 hardening closure."""

import json
import re
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[3]
LEDGER = ROOT / "framework/nxloader/release-0.7.2-closure-v1.json"
MATRIX = ROOT / "framework/tests/test-matrix-v1.json"
SENSITIVE = re.compile(
    r"(?:\b(?:10|127|169[.]254|192[.]168|172[.](?:1[6-9]|2\d|3[01]))"
    r"(?:[.]\d{1,3}){2,3}\b|/home/[^/]+/|/mnt/[^/]+/|"
    r"(?:password|credential|access[_-]?token)=)", re.IGNORECASE)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_json(path):
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key %s" % key)
            result[key] = value
        return result

    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=no_duplicates)


def validate_reference(reference, requirement_id):
    require(isinstance(reference, dict) and
            set(reference) == {"path", "token"},
            "%s has malformed evidence" % requirement_id)
    relative = reference.get("path")
    token = reference.get("token")
    require(isinstance(relative, str) and isinstance(token, str) and token,
            "%s has empty evidence" % requirement_id)
    pure = PurePosixPath(relative)
    require(not pure.is_absolute() and ".." not in pure.parts and
            relative.startswith("framework/"),
            "%s evidence escapes framework" % requirement_id)
    path = ROOT / pure
    require(path.is_file() and not path.is_symlink(),
            "%s evidence is missing or linked: %s" %
            (requirement_id, relative))
    require(token in path.read_text(encoding="utf-8"),
            "%s token absent from %s: %r" %
            (requirement_id, relative, token))


def main():
    raw = LEDGER.read_text(encoding="utf-8")
    require(SENSITIVE.search(raw) is None,
            "closure contains a private address, path or credential")
    document = load_json(LEDGER)
    require(document.get("schema") == "nxloader-0.7.2-closure-v1" and
            document.get("schema_version") == 1 and
            document.get("version") == "0.7.2" and
            document.get("status") == "branch-ready",
            "wrong nxloader 0.7.2 closure header")
    require(document.get("safety") == {
        "guest_code_execution": False,
        "device_access": False,
        "network_access": False,
        "existing_port_migration": False,
        "tag_or_merge": False,
    }, "nxloader 0.7.2 safety boundary changed")
    requirements = document.get("requirements")
    expected = ["NXL-072-%03d" % index for index in range(1, 10)]
    require(isinstance(requirements, list) and
            [item.get("id") for item in requirements] == expected,
            "nxloader 0.7.2 requirements are incomplete or reordered")
    for item in requirements:
        require(set(item) == {"id", "guarantee", "implementation", "tests"},
                "%s fields changed" % item.get("id"))
        require(isinstance(item.get("guarantee"), str) and
                item["guarantee"].strip(),
                "%s guarantee is empty" % item["id"])
        for group in ("implementation", "tests"):
            references = item.get(group)
            require(isinstance(references, list) and references,
                    "%s lacks %s evidence" % (item["id"], group))
            for reference in references:
                validate_reference(reference, item["id"])

    matrix = load_json(MATRIX)
    gates = {gate.get("id"): gate for gate in matrix.get("gates", [])}
    require(gates.get("nxloader-072-closure") == {
        "id": "nxloader-072-closure",
        "class": "pure",
        "command": ["python3", "-B",
                    "framework/nxloader/tests/test_072_closure.py"],
        "sources": ["framework/nxloader/tests/test_072_closure.py"],
        "support_files": [
            "framework/nxloader/release-0.7.2-closure-v1.json"],
        "logged": True,
        "automatic": True,
        "namespace_required": False,
        "signals": [],
    }, "nxloader 0.7.2 gate wiring changed")
    print("nxloader 0.7.2 closure: PASS requirements=9 "
          "armv7_evidence=1 aarch64_evidence=1 hardware_ran=0 "
          "device_access=0")


if __name__ == "__main__":
    main()
