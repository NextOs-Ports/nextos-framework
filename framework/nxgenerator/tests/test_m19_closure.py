#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free closure, evidence and documentation gate for M19."""

import ast
import json
from pathlib import Path, PurePosixPath
import re
import sys


REPOSITORY = Path(__file__).resolve().parents[3]
ROOT = REPOSITORY / "framework" / "nxgenerator"
CLOSURE = ROOT / "m19-closure-v1.json"
TOOL = ROOT / "nxgenerator.py"
SCHEMA = ROOT / "schema" / "nxproject-v1.schema.json"
SCHEMA_V2 = ROOT / "schema" / "nxproject-v2.schema.json"
TEMPLATE = ROOT / "templates" / "README.md.in"
INSTALLATION_TEMPLATE = ROOT / "templates" / "INSTALLATION.md.in"
README = ROOT / "README.md"
GENERATOR_TEST = ROOT / "tests" / "test_nxgenerator.py"
PIN_TOOL = ROOT / "framework_pin.py"
PIN_SCHEMA = ROOT / "schema" / "framework-build-pin-v1.schema.json"
PIN_TEST = ROOT / "tests" / "test_framework_pin.py"
ITEM_IDS = tuple("M19-%03d" % index for index in range(1, 23))
REFERENCE_ROOTS = (
    "framework/",
    "suportando_outros_devices/",
    "publicando_ports/",
)
PRESENTATION_REFERENCE = "ports/gtasa/README.md"
EVIDENCE_RE = re.compile(r"^([^:]+):([1-9][0-9]*)$")
LINK_RE = re.compile(r"\[[^]]+\]\(([^)]+)\)")
IPV4_RE = re.compile(
    r"(?<![0-9])(?:[0-9]{1,3}[.]){3}[0-9]{1,3}(?![0-9])"
)


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def read(path):
    require(path.is_file() and not path.is_symlink(),
            "missing or unsafe file: %s" % path.relative_to(REPOSITORY))
    return path.read_text(encoding="utf-8")


def validate_evidence(reference):
    require(isinstance(reference, str), "evidence reference is not a string")
    matched = EVIDENCE_RE.fullmatch(reference)
    require(matched is not None, "evidence lacks an exact line: %s" % reference)
    relative, raw_line = matched.groups()
    require(relative == PRESENTATION_REFERENCE or
            relative.startswith(REFERENCE_ROOTS),
            "evidence escaped the approved M19 roots: %s" % relative)
    logical = PurePosixPath(relative)
    require(not logical.is_absolute() and ".." not in logical.parts,
            "unsafe evidence path: %s" % relative)
    path = REPOSITORY.joinpath(*logical.parts)
    text = read(path)
    line = int(raw_line)
    require(line <= len(text.splitlines()),
            "evidence line is outside the file: %s" % reference)


def validate_links(path):
    source = read(path)
    for target in LINK_RE.findall(source):
        if target.startswith(("#", "http://", "https://")):
            continue
        target = target.split("#", 1)[0]
        logical = PurePosixPath(target)
        require(not logical.is_absolute(),
                "unsafe documentation link in %s: %s" % (path.name, target))
        candidate = (path.parent / target).resolve()
        try:
            candidate.relative_to(REPOSITORY)
        except ValueError as error:
            raise GateError(
                "documentation link escaped the repository in %s: %s" %
                (path.name, target)
            ) from error
        require(candidate.exists(),
                "broken documentation link in %s: %s" % (path.name, target))


def main():
    document = json.loads(read(CLOSURE))
    require(set(document) == {
        "schema", "schema_version", "milestone", "status", "scope",
        "method", "safety", "items",
    }, "M19 closure schema changed")
    require(document["schema"] == "nxgenerator-m19-closure-v1" and
            document["schema_version"] == 1 and
            document["milestone"] == "M19" and
            document["status"] == "closed_for_generator_and_documentation",
            "M19 closure identity changed")
    require(document["safety"] == {
        "game_execution": False,
        "guest_code_execution": False,
        "device_access": False,
        "network_access": False,
        "physical_support_claimed": False,
        "approved_port_modified": False,
        "master_plan_modified": False,
    }, "M19 safety claims changed")

    items = document["items"]
    require(isinstance(items, list) and len(items) == 22,
            "M19 must account for exactly twenty-two items")
    require(tuple(item.get("id") for item in items) == ITEM_IDS,
            "M19 IDs or ordering changed")
    for item in items:
        require(set(item) == {
            "id", "status", "evidence_refs", "guarantees",
            "limitations", "tests",
        }, "%s has malformed fields" % item.get("id"))
        require(item["status"] == "closed",
                "%s is not closed" % item["id"])
        for field in ("evidence_refs", "guarantees", "limitations", "tests"):
            require(isinstance(item[field], list) and item[field],
                    "%s lacks %s" % (item["id"], field))
        for reference in item["evidence_refs"]:
            validate_evidence(reference)

    tool_source = read(TOOL)
    ast.parse(tool_source, filename=str(TOOL))
    json.loads(read(SCHEMA))
    schema_v2 = json.loads(read(SCHEMA_V2))
    require(schema_v2["properties"]["portmaster"]["required"] ==
            ["metadata_version", "min_glibc", "runtime"],
            "nxproject v2 lost explicit PortMaster runtime")
    template = read(TEMPLATE)
    installation_template = read(INSTALLATION_TEMPLATE)
    test_source = read(GENERATOR_TEST)
    pin_source = read(PIN_TOOL)
    ast.parse(pin_source, filename=str(PIN_TOOL))
    pin_schema = json.loads(read(PIN_SCHEMA))
    pin_test_source = read(PIN_TEST)
    require(pin_schema["properties"]["schema"]["const"] ==
            "nextos-framework-build-pin-v1",
            "M19 framework build pin schema changed")
    for token in (
        "BOOTSTRAP_GENERATOR.generate", "NXEXTRACT_COMMON_FILES",
        "unimplemented_nonrelease", "rename_noreplace",
        "artifact_inventory", '"physical_support_proven": False',
        "nxsplash_source_state", "generated nxsplash helper",
        "validate_apk_variant_policy", "NXEXTRACT_RELEASE_MANIFEST",
        "portmaster_source_state", 'if config["portmaster_runtime"]:',
        'render_template("INSTALLATION.md.in"',
        "execution_role_architecture", 'generation["execution_roles"]',
    ):
        require(token in tool_source, "M19 generator lacks: %s" % token)
    for token in (
        "## English", "## Português", "### Architecture",
        "### Arquitetura", "### Controls", "### Controles",
        "### Game data", "### Dados do jogo", "### Licenses",
        "### Licenças", "same ZIP and SHA-256",
        "mesmo ZIP público exato", "development-only",
        "somente ao desenvolvimento",
    ):
        require(token in template, "M19 README template lacks: %s" % token)
    for token in (
        "two clean generations differ", "retired artifact",
        "golden-port guarantee", "does not record its generator version",
        "vendored NXExtract file differs", "adapter skeleton invented lifecycle",
        "generated PortMaster metadata changed", "validate_links(readme)",
        "missing-splash", "altered-splash", "stripped-splash",
        "splash-before-required", "single-container APK pin was not rejected",
        "mandatory NXExtract UI is absent, unsafe or unpinned",
        "nxextract_release_tamper_rejections=7", "no_external_stat=1",
        "standalone_source_root=1",
        "real_recipe_regressions=3",
        "portmaster_real_cycles=2", "runtime_negatives=4", "legacy_v1=1",
        "runtime_empty_omitted=1", "runtime_nonempty_preserved=1",
        "strict_json_negatives=5", "mixed_roles=1",
        "legacy_mixed_rejected=1",
    ):
        require(token in test_source, "M19 regression gate lacks: %s" % token)
    for token in (
        "GIT_NO_REPLACE_OBJECTS", "GIT_NO_LAZY_FETCH", "TREE_DIGEST",
        "verified_git_object", "rename_noreplace", "FRAMEWORK-SOURCE.json",
        "NORMALIZED_MTIME_NS", "component_working_trees_ignored",
    ):
        require(token in pin_source, "M19 framework pin helper lacks: %s" % token)
    for token in (
        "dirty_checkout", "GIT_DIR", "object_integrity=1",
        "replace_refs=blocked", "lazy_fetch=blocked",
        "filters=not_executed", "no_overwrite=1",
    ):
        require(token in pin_test_source,
                "M19 framework pin regression lacks: %s" % token)

    for path in (README, REPOSITORY / "framework" / "README.md",
                 REPOSITORY / "suportando_outros_devices" /
                 "padrao-universal.md",
                 REPOSITORY / "publicando_ports" / "README.md"):
        validate_links(path)
    for token in (
        "## English", "## Português", "REQUIRED BEFORE RELEASE",
        "OBRIGATÓRIO ANTES DA RELEASE", "package ID", "SHA-256",
    ):
        require(token in installation_template,
                "M19 INSTALLATION template lacks: %s" % token)

    for path in (README, TEMPLATE, INSTALLATION_TEMPLATE):
        text = read(path)
        require(not IPV4_RE.search(text) and "/home/" not in text and
                "/Users/" not in text,
                "M19 public documentation contains a private literal")

    print("M19 closure gate passed: items=22 status=closed "
          "abis=2 mixed_roles=1 physical_support_claimed=0")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError, ValueError, KeyError, SyntaxError) as error:
        print("M19 closure gate failed: %s" % error, file=sys.stderr)
        raise SystemExit(1)
