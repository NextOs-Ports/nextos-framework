#!/usr/bin/env python3
"""Pure fail-closed validator for the additive nxloader 0.9.0 closure."""

import json
import re
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[3]
LOADER = ROOT / "framework/nxloader"
LEDGER = LOADER / "release-0.9.0-closure-v1.json"
SENSITIVE = re.compile(
    r"(?:\b(?:10|127|169[.]254|192[.]168|172[.](?:1[6-9]|2\d|3[01]))"
    r"(?:[.]\d{1,3}){2,3}\b|/home/[^/]+/|/mnt/[^/]+/|"
    r"(?:password|credential|access[_-]?token)=)", re.IGNORECASE)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key %s" % key)
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=unique)


def validate_reference(reference, requirement_id):
    require(isinstance(reference, dict) and set(reference) == {"path", "token"},
            "%s has malformed evidence" % requirement_id)
    relative = reference.get("path")
    token = reference.get("token")
    require(isinstance(relative, str) and isinstance(token, str) and token,
            "%s has empty evidence" % requirement_id)
    pure = PurePosixPath(relative)
    require(not pure.is_absolute() and ".." not in pure.parts and
            relative.startswith("framework/nxloader/"),
            "%s evidence escapes nxloader" % requirement_id)
    path = ROOT / pure
    require(path.is_file() and not path.is_symlink(),
            "%s evidence is missing or linked: %s" %
            (requirement_id, relative))
    require(token in path.read_text(encoding="utf-8"),
            "%s token absent from %s: %r" %
            (requirement_id, relative, token))


def validate_legacy_api(header):
    stable = (
        "NXLOADER_API_VERSION_MAJOR 1u", "NXLOADER_API_VERSION_MINOR 3u",
        "NXLOADER_EREENTRANT = -15", "NXLOADER_STATE_EMPTY = 0",
        "NXLOADER_STATE_LOADED = 1", "NXLOADER_STATE_RELOCATED = 2",
        "NXLOADER_STATE_RESOLVED = 3", "NXLOADER_STATE_FINALIZED = 4",
        "NXLOADER_STATE_INITIALIZED = 5", "NXLOADER_STATE_ERROR = 6",
        "NXLOADER_STATE_INITIALIZING = 7",
        "NXLOADER_STATE_JNI_LOADING = 8", "NXLOADER_STATE_READY = 9",
    )
    require(all(token in header for token in stable),
            "0.9.0 renumbered or removed a 0.8.0 API value")
    require("NXLOADER_STATE_PATCHING = 10" in header and
            "nxloader_module_begin_patch" in header,
            "0.9.0 additive patch API is absent")


def main():
    raw = LEDGER.read_text(encoding="utf-8")
    require(SENSITIVE.search(raw) is None,
            "closure contains a private address, path or credential")
    document = load_json(LEDGER)
    require(document.get("schema") == "nxloader-0.9.0-closure-v1" and
            document.get("schema_version") == 1 and
            document.get("version") == "0.9.0" and
            document.get("api") == {"major": 1, "minor": 3},
            "wrong nxloader 0.9.0 closure header")
    require(document.get("safety") == {
        "device_access": False,
        "hardware_ran": False,
        "guest_initializers_executed": False,
        "ports_migrated": False,
        "api_080_preserved": True,
    }, "nxloader 0.9.0 safety boundary changed")
    requirements = document.get("requirements")
    expected = ["NXL-090-%03d" % index for index in range(1, 7)]
    require(isinstance(requirements, list) and
            [item.get("id") for item in requirements] == expected,
            "nxloader 0.9.0 requirements are incomplete or reordered")
    for item in requirements:
        require(set(item) == {"id", "guarantee", "implementation", "tests"},
                "%s fields changed" % item.get("id"))
        require(isinstance(item.get("guarantee"), str) and
                item["guarantee"].strip(),
                "%s guarantee is empty" % item.get("id"))
        for group in ("implementation", "tests"):
            references = item.get(group)
            require(isinstance(references, list) and references,
                    "%s lacks %s evidence" % (item.get("id"), group))
            for reference in references:
                validate_reference(reference, item["id"])
    require((LOADER / "VERSION").read_text(encoding="ascii").strip() ==
            "0.9.0", "VERSION is not 0.9.0")
    validate_legacy_api(
        (LOADER / "include/nxloader.h").read_text(encoding="utf-8"))
    print("nxloader 0.9.0 closure: PASS requirements=6 api=1.3 "
          "same_mapping_patch=1 hardware_ran=0 device_access=0")


if __name__ == "__main__":
    main()
