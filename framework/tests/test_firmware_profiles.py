#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free honesty and schema gate for firmware contract profiles."""

import json
import os
import re
import sys
from pathlib import Path, PurePosixPath


REPOSITORY = Path(__file__).resolve().parents[2]
TEST_ROOT = Path(__file__).resolve().parent
PROFILES_PATH = TEST_ROOT / "firmware-profiles-v1.json"
MATRIX_PATH = TEST_ROOT / "test-matrix-v1.json"
RUNNER_PATH = TEST_ROOT / "run-safe-gates.sh"
PORTMASTER_CONTRACT_PATH = (
    REPOSITORY / "framework/portmaster/contract-v1.json"
)
PORTMASTER_CASES_PATH = (
    REPOSITORY / "framework/portmaster/fixtures/contract-cases-v1.json"
)
DEVICES_PATH = REPOSITORY / "framework/catalog/devices-v1.json"
PROFILE_IDS = (
    "arkos", "muos", "rocknix", "darkos", "nextos",
    "knulli-batocera",
)
OFFICIAL_CASES = {
    "arkos": ("official-generic-arkos-family",),
    "muos": ("official-muos-split-root",),
    "rocknix": ("official-rocknix-helper",),
    "darkos": (),
    "nextos": ("nextos-portmaster-control-bridge",),
    "knulli-batocera": ("official-knulli-exfat",),
}
PROFILE_SCOPES = {
    "arkos": "official-contract-plus-synthetic",
    "muos": "official-contract-plus-synthetic",
    "rocknix": "official-contract-plus-synthetic",
    "darkos": "field-observation-not-static-proof",
    "nextos": "local-contract-plus-synthetic",
    "knulli-batocera": "official-contract-plus-synthetic",
}
DEVICE_VARIANTS = {
    "nextos-mali450-fbdev": (
        "nextos-mali450-fbdev", "nextos", ("aarch64", "armv7"),
        "physical-full", "imported-physical-history-not-host-proof",
    ),
    "r36s-arkos-g31-kmsdrm": (
        "r36s-arkos-g31-kmsdrm", "arkos", ("aarch64", "armv7"),
        "physical-full", "imported-physical-history-not-host-proof",
    ),
    "nextos-x5-g310-kmsdrm": (
        "nextos-x5-g310-kmsdrm", "nextos", ("aarch64", "i386"),
        "physical-full", "imported-physical-history-not-host-proof",
    ),
    "rg40xx-muos": (
        "rg40xx-muos", "muos", ("aarch64", "armv7"),
        "community-confirmed", "imported-community-history-not-host-proof",
    ),
    "rgds-rocknix-panfrost-wayland": (
        "rgds-rocknix-panfrost-wayland", "rocknix", ("aarch64",),
        "fix-published-retest-pending",
        "imported-fix-history-retest-pending-not-host-proof",
    ),
    "knulli-batocera-amberelec-trimui": (
        "knulli-batocera-amberelec-trimui", "knulli-batocera",
        ("aarch64",), "design-compatible", "designed-route-not-host-proof",
    ),
    "darkos-rk3326-g31-kmsdrm": (
        None, "darkos", ("aarch64",), "field-observation",
        "field-observation-not-static-proof",
    ),
}
SHARED_GATES = {
    "nxcompat-host": ("capability-probe", "input-fake-receipts"),
    "nxgl-m13-host": ("graphics-fake-provider",),
    "nxaudio-m14-host": ("audio-fake-provider",),
}
ROOT_KEYS = {
    "schema_version",
    "contract_id",
    "purpose",
    "evidence_boundary",
    "shared_synthetic_gates",
    "device_variants",
    "profiles",
}
BOUNDARY_KEYS = {
    "universal_evidence",
    "hardware_proof",
    "hardware_ran",
    "device_access",
    "firmware_images_used",
    "result",
    "meaning",
}
PROFILE_KEYS = {
    "id",
    "label",
    "cfw_name",
    "evidence_scope",
    "evidence_refs",
    "portmaster_case_ids",
    "candidate_roots",
    "launcher_architecture",
    "port_32bit",
    "arch_library_suffix",
    "simulated_environment",
    "universal_evidence",
    "hardware_proof",
}
ENVIRONMENT_KEYS = {
    "shell",
    "external_stat",
    "split_launcher_and_data_roots",
    "graphics",
    "audio",
    "input",
}
DEVICE_VARIANT_KEYS = {
    "id",
    "catalog_family_id",
    "firmware_profile_id",
    "arch_routes",
    "evidence_level",
    "evidence_scope",
    "evidence_refs",
    "hardware_proof_from_this_gate",
}
PRIVATE_IP = re.compile(
    r"(?:10\.|127\.|169\.254\.|192\.168\.|"
    r"172\.(?:1[6-9]|2[0-9]|3[01])\.)"
)
CFW_NAME = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


class ContractError(Exception):
    """A malformed profile or an overstated evidence boundary."""


def require(condition, message):
    if not condition:
        raise ContractError(message)


def read_json(path):
    require(path.is_file() and not path.is_symlink(),
            "missing or unsafe JSON: %s" % path.relative_to(REPOSITORY))
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def regular_repository_file(relative):
    logical = PurePosixPath(relative)
    require(not logical.is_absolute() and ".." not in logical.parts,
            "evidence reference is not repository-relative: %s" % relative)
    path = REPOSITORY.joinpath(*logical.parts)
    require(path.is_file() and not path.is_symlink(),
            "evidence reference is missing or unsafe: %s" % relative)
    require(os.path.commonpath((str(REPOSITORY), str(path.resolve()))) ==
            str(REPOSITORY),
            "evidence reference escaped the repository: %s" % relative)


def check_shared_gates(document, matrix, runner_source):
    declarations = document.get("shared_synthetic_gates")
    require(isinstance(declarations, list) and len(declarations) == 3,
            "shared synthetic gates must contain exactly three entries")
    actual = {}
    for entry in declarations:
        require(isinstance(entry, dict) and set(entry) ==
                {"id", "covers", "runs_per_matrix"},
                "malformed shared synthetic gate")
        gate_id = entry.get("id")
        covers = entry.get("covers")
        require(gate_id in SHARED_GATES and gate_id not in actual,
                "unknown or duplicate shared synthetic gate: %r" % gate_id)
        require(isinstance(covers, list) and tuple(covers) ==
                SHARED_GATES[gate_id],
                "shared gate coverage drifted for %s" % gate_id)
        require(entry.get("runs_per_matrix") == 1,
                "shared subsystem gate is repeated per profile: %s" % gate_id)
        actual[gate_id] = entry
    require(set(actual) == set(SHARED_GATES),
            "shared synthetic gate set is incomplete")

    matrix_by_id = {gate.get("id"): gate for gate in matrix.get("gates", [])}
    for gate_id in SHARED_GATES:
        gate = matrix_by_id.get(gate_id)
        require(gate is not None and gate.get("automatic") is True and
                gate.get("class") == "filesystem" and
                gate.get("signals") == [],
                "shared subsystem gate is not an automatic synthetic gate: %s" %
                gate_id)
        require(runner_source.count("run_gate %s " % gate_id) == 1,
                "shared subsystem gate is not executed exactly once: %s" % gate_id)

    output_sources = {
        "nxcompat-host": REPOSITORY / "framework/nxcompat/tests/run-host.sh",
        "nxgl-m13-host": REPOSITORY / "framework/nxgl/tests/run-m13-host.sh",
        "nxaudio-m14-host": REPOSITORY / "framework/nxaudio/tests/run-host.sh",
    }
    for gate_id, path in output_sources.items():
        source = path.read_text(encoding="utf-8")
        require("hardware_ran=0" in source and "device_access=0" in source,
                "shared gate lacks its zero-hardware receipt: %s" % gate_id)


def check_profiles(document, contract, fixtures):
    profiles = document.get("profiles")
    require(isinstance(profiles, list) and
            tuple(item.get("id") for item in profiles) == PROFILE_IDS,
            "profile set or ordering changed")
    known_roots = set(contract["portmaster_discovery"]["known_roots"])
    cases = {item["id"]: item for item in fixtures["cases"]}

    for profile in profiles:
        profile_id = profile.get("id")
        require(set(profile) == PROFILE_KEYS,
                "profile %s has malformed fields" % profile_id)
        require(profile.get("universal_evidence") is False and
                profile.get("hardware_proof") is False,
                "profile %s crossed the evidence boundary" % profile_id)
        require(isinstance(profile.get("label"), str) and
                profile["label"].endswith("fixture"),
                "profile %s lacks a fixture-only label" % profile_id)
        require(isinstance(profile.get("cfw_name"), str) and
                CFW_NAME.fullmatch(profile["cfw_name"]),
                "profile %s has an unsafe CFW name" % profile_id)

        expected_cases = OFFICIAL_CASES[profile_id]
        require(tuple(profile.get("portmaster_case_ids", ())) == expected_cases,
                "profile %s has the wrong PortMaster evidence cases" % profile_id)
        candidate_roots = profile.get("candidate_roots")
        require(isinstance(candidate_roots, list) and candidate_roots and
                len(candidate_roots) == len(set(candidate_roots)),
                "profile %s has invalid candidate roots" % profile_id)
        require(set(candidate_roots) <= known_roots,
                "profile %s names a root outside the PortMaster contract" %
                profile_id)
        if expected_cases:
            expected_roots = []
            for case_id in expected_cases:
                case = cases.get(case_id)
                require(case is not None and
                        case.get("universal_evidence") is False,
                        "profile %s refers to unsafe PortMaster evidence" %
                        profile_id)
                expected_roots.extend(
                    case.get("expected", {}).get("candidate_roots", ()))
            if expected_roots:
                require(candidate_roots == expected_roots,
                        "profile %s roots drifted from its PortMaster fixture" %
                        profile_id)
        else:
            require(profile_id == "darkos",
                    "a field observation was presented as an official fixture")
        require(profile.get("evidence_scope") == PROFILE_SCOPES[profile_id],
                "profile %s understates or overstates its source scope" %
                profile_id)

        references = profile.get("evidence_refs")
        require(isinstance(references, list) and references and
                len(references) == len(set(references)),
                "profile %s has invalid evidence references" % profile_id)
        for reference in references:
            require(isinstance(reference, str) and reference,
                    "profile %s has a non-string evidence reference" % profile_id)
            regular_repository_file(reference)

        architecture = profile.get("launcher_architecture")
        require(architecture in ("aarch64", "armv7"),
                "profile %s has an unknown launcher architecture" % profile_id)
        is_armv7 = architecture == "armv7"
        require(profile.get("port_32bit") is is_armv7,
                "profile %s PORT_32BIT contract disagrees with its ABI" %
                profile_id)
        expected_suffix = "libs.armhf" if is_armv7 else "libs.aarch64"
        require(profile.get("arch_library_suffix") == expected_suffix,
                "profile %s has the wrong PortMaster library suffix" % profile_id)

        environment = profile.get("simulated_environment")
        require(isinstance(environment, dict) and
                set(environment) == ENVIRONMENT_KEYS,
                "profile %s has malformed simulated environment" % profile_id)
        require(environment == {
                    "shell": "bash",
                    "external_stat": "absent-by-test",
                    "split_launcher_and_data_roots": True,
                    "graphics": "shared-fake-provider",
                    "audio": "shared-fake-provider",
                    "input": "shared-fake-receipt",
                }, "profile %s weakened the synthetic boundary" % profile_id)


def check_device_variants(document, catalog):
    variants = document.get("device_variants")
    require(isinstance(variants, list) and
            tuple(item.get("id") for item in variants) ==
            tuple(DEVICE_VARIANTS),
            "device variant set or ordering changed")
    catalog_families = {
        item.get("id"): item for item in catalog.get("families", [])
        if isinstance(item, dict)
    }
    catalog_ids_seen = set()
    for variant in variants:
        variant_id = variant.get("id")
        require(set(variant) == DEVICE_VARIANT_KEYS,
                "device variant %s has malformed fields" % variant_id)
        expected = DEVICE_VARIANTS[variant_id]
        catalog_id, profile_id, routes, evidence, scope = expected
        require(variant.get("catalog_family_id") == catalog_id and
                variant.get("firmware_profile_id") == profile_id and
                tuple(variant.get("arch_routes", ())) == routes and
                variant.get("evidence_level") == evidence and
                variant.get("evidence_scope") == scope,
                "device variant %s drifted from its sanitized evidence" %
                variant_id)
        require(profile_id in PROFILE_IDS,
                "device variant %s refers to an unknown profile" % variant_id)
        require(variant.get("hardware_proof_from_this_gate") is False,
                "device variant %s turned host simulation into hardware proof" %
                variant_id)
        if catalog_id is not None:
            family = catalog_families.get(catalog_id)
            require(family is not None and family.get("evidence") == evidence,
                    "device variant %s disagrees with the device catalog" %
                    variant_id)
            catalog_ids_seen.add(catalog_id)
        else:
            require(variant_id == "darkos-rk3326-g31-kmsdrm" and
                    evidence == "field-observation",
                    "uncatalogued device variant is not fail-closed")
        references = variant.get("evidence_refs")
        require(isinstance(references, list) and references and
                len(references) == len(set(references)),
                "device variant %s has invalid evidence references" % variant_id)
        for reference in references:
            require(isinstance(reference, str) and reference,
                    "device variant %s has a non-string evidence reference" %
                    variant_id)
            regular_repository_file(reference)
    require(catalog_ids_seen == set(catalog_families),
            "device variants do not cover every sanitized catalog family")


def main():
    document = read_json(PROFILES_PATH)
    matrix = read_json(MATRIX_PATH)
    contract = read_json(PORTMASTER_CONTRACT_PATH)
    fixtures = read_json(PORTMASTER_CASES_PATH)
    catalog = read_json(DEVICES_PATH)
    require(set(document) == ROOT_KEYS and document.get("schema_version") == 1,
            "firmware profile root schema changed")
    require(document.get("contract_id") ==
            "nxframework-firmware-contract-simulation-v1",
            "firmware profile contract identity changed")
    boundary = document.get("evidence_boundary")
    require(isinstance(boundary, dict) and set(boundary) == BOUNDARY_KEYS,
            "firmware evidence boundary is malformed")
    require(boundary.get("universal_evidence") is False and
            boundary.get("hardware_proof") is False and
            boundary.get("hardware_ran") is False and
            boundary.get("device_access") is False and
            boundary.get("firmware_images_used") is False and
            boundary.get("result") == "profile-contract-pass",
            "firmware evidence boundary was weakened")

    encoded = json.dumps(document, ensure_ascii=False)
    require(PRIVATE_IP.search(encoded) is None,
            "firmware profiles contain a private IP literal")
    require("/home/" not in encoded and "/Users/" not in encoded,
            "firmware profiles contain a personal path")
    for overclaim in ("supported", "hardware-validated", "firmware-compatible"):
        require(overclaim not in encoded.lower(),
                "firmware profiles contain an overclaim term: %s" % overclaim)

    runner_source = RUNNER_PATH.read_text(encoding="utf-8")
    check_shared_gates(document, matrix, runner_source)
    check_profiles(document, contract, fixtures)
    check_device_variants(document, catalog)
    print("firmware profile contract gate passed: profiles=6 variants=7 "
          "catalog_families=6 "
          "result=profile-contract-pass shared_subsystem_runs=3 "
          "hardware_ran=0 device_access=0 firmware_images_used=0")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ContractError, KeyError, TypeError, ValueError, OSError) as error:
        print("firmware profile contract gate failed: %s" % error,
              file=sys.stderr)
        raise SystemExit(1)
