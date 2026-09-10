#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Completeness gate for the additive NXExtract P06 slice."""

import hashlib
import json
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
AUDIT_PATH = REPO_ROOT / "framework/nxextract/p06-terminal-result-contract-v1.json"
NX_ROOT = REPO_ROOT / "suportando_outros_devices/extrator-universal"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_json(path):
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate key %s in %s" % (key, path))
            result[key] = value
        return result

    return json.loads(
        path.read_text(encoding="utf-8"), object_pairs_hook=no_duplicates
    )


def main():
    audit = load_json(AUDIT_PATH)
    require(
        set(audit)
        == {
            "schema_version",
            "milestone",
            "component",
            "scope",
            "implemented_requirements",
            "deferred_requirements",
            "visual_identity",
        },
        "P06 NXExtract audit has unknown fields",
    )
    require(audit["schema_version"] == 1, "wrong audit schema")
    require(audit["milestone"] == "P06-NXExtract", "wrong milestone")
    require(
        audit["component"].get("id") == "nxextract"
        and set(audit["component"]) == {"id", "version"}
        and audit["component"]["version"]
        == (NX_ROOT / "VERSION").read_text(encoding="utf-8").strip(),
        "wrong component identity",
    )
    expected_ids = [
        "OBS-%03d" % number
        for number in list(range(1, 12)) + list(range(13, 21))
    ] + ["OBS-041"]
    requirements = audit["implemented_requirements"]
    require(
        [item.get("id") for item in requirements] == expected_ids,
        "implemented requirement set is incomplete or reordered",
    )
    for requirement in requirements:
        require(
            set(requirement) == {"id", "implementation", "tests"},
            "%s has unknown fields" % requirement.get("id"),
        )
        for evidence_kind in ("implementation", "tests"):
            evidence = requirement[evidence_kind]
            require(evidence, "%s lacks %s" % (requirement["id"], evidence_kind))
            for reference in evidence:
                require(
                    set(reference) == {"path", "token"},
                    "%s has malformed evidence" % requirement["id"],
                )
                relative = Path(reference["path"])
                require(
                    not relative.is_absolute() and ".." not in relative.parts,
                    "%s evidence escapes the repository" % requirement["id"],
                )
                target = REPO_ROOT / relative
                require(
                    target.is_file() and not target.is_symlink(),
                    "%s evidence missing or linked: %s"
                    % (requirement["id"], relative),
                )
                require(
                    reference["token"] in target.read_text(encoding="utf-8"),
                    "%s token absent from %s"
                    % (requirement["id"], relative),
                )

    require(
        audit["deferred_requirements"]
        == [
            {
                "id": "OBS-012",
                "owner": "nxbootstrap",
                "reason": "launcher must strictly copy the completed result into log.txt",
            },
            {
                "id": "OBS-021..040",
                "owner": "nxbootstrap",
                "reason": "cross-phase persistence and pre-log launcher evidence",
            },
        ],
        "launcher-owned P06 work was hidden or misclassified",
    )

    version = (NX_ROOT / "VERSION").read_text(encoding="utf-8").strip()
    engine = (NX_ROOT / "nxextract.py").read_text(encoding="utf-8")
    # The P06 contract is carried by the engine, not by one patch number. The
    # pin follows the declared component version and still fails closed when
    # VERSION and the engine constant disagree.
    require(version == audit["component"]["version"],
            "VERSION is not the contract engine version %s"
            % audit["component"]["version"])
    require('NXEXTRACT_VERSION = "%s"' % version in engine,
            "the engine constant disagrees with VERSION (%s)" % version)
    schema = load_json(NX_ROOT / "docs/terminal-result-schema-v1.json")
    require(schema.get("additionalProperties") is False, "result schema is open")
    require(
        schema["properties"]["schema_version"].get("const") == 1,
        "result schema version drifted",
    )
    require(
        schema["properties"]["nxextract_version"].get("const") == "1.2.10",
        "result schema engine pin drifted",
    )

    suite = unittest.defaultTestLoader.discover(
        str(NX_ROOT / "tests"), pattern="test_nxextract.py"
    )
    # Closed at 106 when P06 was audited. The pin exists to catch tests being
    # REMOVED, which is what would quietly shrink the audited surface; adding
    # tests is the direction this work is supposed to go, and an equality pin
    # punished exactly that.
    require(
        suite.countTestCases() >= 106,
        "NXExtract Python case count fell below the audited 106",
    )
    visual = audit["visual_identity"]
    require(visual["ui_changed"] is False, "audit claims a UI change")
    manifest = load_json(NX_ROOT / "ui/release/manifest-v1.json")
    require(
        manifest["version"] == visual["ui_release_version"] == "1.2.16",
        "immutable UI release was relabeled",
    )
    require(
        set(record["sha256"] for record in manifest["artifacts"].values())
        == {
            "8e4a68ae0a611096d23b04628b4f2e8b5cf34755fe9b71d134bc0d7ea6ccf987",
            "e846c5cbf0d17ec6aed2fd70066649b2283cdbbfb092d9c1dde59c50bc443860",
            "1bd67035d0fabba8fc57132e7ee83541db217c26974566c2764a3715ca248e72",
            "909c41c5dba4d3f0f4237c3db7ff149f477ae2f5f49a36fb9a3ba48ac35fec95",
        },
        "immutable multi-architecture UI hashes drifted",
    )
    audit_text = AUDIT_PATH.read_text(encoding="utf-8")
    require(
        re.search(
            r"(?:^|[^0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?:[^0-9]|$)",
            audit_text,
        )
        is None,
        "P06 audit contains a device address",
    )
    require(
        hashlib.sha256(
            (NX_ROOT / "ui/nxextract_ui.c").read_bytes()
        ).hexdigest()
        == manifest["source_sha256"],
        "UI source differs from its immutable manifest",
    )
    contract = load_json(REPO_ROOT / "framework/contracts/declarative-v1.json")
    component = {
        item["id"]: item for item in contract["components"]
    }["nxextract"]
    require(
        contract["contract_version"] in (
            "1.0.35", "1.0.36", "1.0.37", "1.0.38", "1.0.39", "1.0.41",
            "1.0.40", "1.0.42", "1.0.43", "1.0.44", "1.0.45", "1.0.46", "1.0.47", "1.0.48", "1.0.49", "1.0.50", "1.0.51", "1.0.52", "1.0.53", "1.0.54", "1.0.55", "1.0.56", "1.0.57", "1.0.58", "1.0.59", "1.0.60", "1.0.61", "1.0.62", "1.0.63", "1.0.64", "1.0.65", "1.0.66", "1.0.67", "1.0.68"),
        "shared lock drifted",
    )
    require(component["current_version"] == version, "component lock drifted")
    require(
        contract["nxport"]["nxextract_version"] == version,
        "current generated ports lost the explicit NXExtract opt-in pin",
    )

    print(
        "P06 NXExtract gate passed: requirements=20 tests=%d (floor 106) "
        "schema=1 visual_diff=0 deferred=launcher"
        % suite.countTestCases()
    )


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, json.JSONDecodeError) as error:
        print("P06 NXExtract gate failed: %s" % error)
        raise SystemExit(1)
