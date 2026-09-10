#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V3 coverage gate binding the V2 firmware matrix to the CFW evidence catalog.

Asserts three read-only facts:
1. every V2 matrix profile family is still covered by at least one V3 claim;
2. the V3 catalog disaggregates the families the V2 matrix grouped
   (knulli-batocera, amberelec-vs-p4elec, nextos mali450-vs-x5, rocknix);
3. no V2 profile records a stronger evidence level than what the V3 catalog
   honestly records for that family (evidence binding, never promotion).
"""

import json
import sys
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent
REPOSITORY = TEST_ROOT.parents[1]
CATALOG_PATH = REPOSITORY / "framework/evidence/CFW-EVIDENCE.json"
V2_RUNNER_PATH = TEST_ROOT / "run-firmware-matrix-v2.sh"

sys.path.insert(0, str(TEST_ROOT))
import test_firmware_matrix_v2 as matrix_v2  # noqa: E402

EXPECTED_V2_PROFILES = (
    "muos", "rocknix-panfrost", "amberelec", "knulli-batocera", "arkos",
    "darkosre", "nextos-mali450", "trimui", "spruce",
)
# Cada perfil V2 precisa de pelo menos um claim V3 destas familias. O arkos
# exige exclusao de "darkos" porque "darkosre" contem "arkos" como substring.
PROFILE_FAMILY_TOKENS = {
    "muos": ("muos",),
    "rocknix-panfrost": ("rocknix",),
    "amberelec": ("amberelec",),
    "knulli-batocera": ("knulli", "batocera"),
    "arkos": ("arkos",),
    "darkosre": ("darkosre",),
    "nextos-mali450": ("nextos",),
    "trimui": ("trimui",),
    "spruce": ("spruce",),
}
CATALOG_LEVELS = (
    "design-only", "synthetic", "image-backed", "community",
    "physical-subsystem", "physical-full",
)
# Vocabulario V2 -> vocabulario do catalogo V1 (nx-cfw-evidence-v1), sempre
# para o nivel mais fraco compativel: mapear para cima seria promocao.
V2_LEVEL_TO_CATALOG = {
    "design-only": "design-only",
    "field-observation": "community",
    "release-fix": "community",
    "physical-runtime-ui-unverified": "physical-subsystem",
    "physical-subsystem": "physical-subsystem",
    "physical-full": "physical-full",
}


class CoverageError(Exception):
    """V2/V3 coverage, disaggregation or evidence-binding failure."""


def require(condition, message):
    if not condition:
        raise CoverageError(message)


def read_catalog():
    require(CATALOG_PATH.is_file() and not CATALOG_PATH.is_symlink(),
            "missing or unsafe CFW evidence catalog: %s" % CATALOG_PATH)
    with CATALOG_PATH.open("r", encoding="utf-8") as stream:
        document = json.load(stream)
    require(document.get("schema") == "nx-cfw-evidence-v1" and
            document.get("schema_version") == 1,
            "CFW evidence catalog identity changed")
    claims = document.get("claims")
    require(isinstance(claims, list) and claims,
            "CFW evidence catalog has no claims")
    for claim in claims:
        require(isinstance(claim, dict) and
                isinstance(claim.get("id"), str) and
                claim.get("level") in CATALOG_LEVELS,
                "malformed claim in CFW evidence catalog: %r" %
                claim.get("id"))
    return claims


def claims_for_profile(claims, profile_id):
    tokens = PROFILE_FAMILY_TOKENS[profile_id]
    matched = []
    for claim in claims:
        claim_id = claim["id"]
        for token in tokens:
            if token in claim_id:
                if token == "arkos" and "darkos" in claim_id:
                    continue
                matched.append(claim)
                break
    return matched


def validate_v2_profiles_still_covered(claims):
    require(tuple(matrix_v2.PROFILE_IDS) == EXPECTED_V2_PROFILES,
            "V2 matrix profile set changed; update the V3 coverage gate "
            "deliberately")
    runner_text = V2_RUNNER_PATH.read_text(encoding="utf-8")
    require("firmware-profiles-v2.json" in runner_text and
            "test_firmware_matrix_v2.py" in runner_text,
            "V2 runner no longer drives the V2 matrix contract")
    for profile_id in EXPECTED_V2_PROFILES:
        require(claims_for_profile(claims, profile_id),
                "V2 profile has no V3 evidence claim: %s" % profile_id)
    return len(EXPECTED_V2_PROFILES)


def validate_disaggregation(claims):
    ids = [claim["id"] for claim in claims]
    require(any("knulli" in i and "batocera" not in i for i in ids),
            "no standalone Knulli claim: knulli-batocera is still grouped")
    require(any("batocera" in i and "knulli" not in i for i in ids),
            "no standalone Batocera claim: knulli-batocera is still grouped")
    require(any("amberelec" in i for i in ids) and
            any("p4elec" in i for i in ids),
            "AmberELEC and P4ELEC must exist as separate claims")
    require(not any("amberelec" in i and "p4elec" in i for i in ids),
            "AmberELEC and P4ELEC were merged into one claim")
    require(any("nextos" in i and "mali450" in i for i in ids),
            "no NextOS Mali-450 claim")
    require(any("nextos" in i and "x5" in i for i in ids),
            "no NextOS X5/X5M claim separate from Mali-450")
    rocknix_lines = {
        (claim["hardware"]["soc"], claim["hardware"]["vendor_model"])
        for claim in claims if "rocknix" in claim["id"]
    }
    require(len(rocknix_lines) >= 3,
            "ROCKNIX needs at least 3 distinct hardware lines, found %d" %
            len(rocknix_lines))


def validate_evidence_binding(claims):
    rank = {level: index for index, level in enumerate(CATALOG_LEVELS)}
    for profile_id, v2_level in matrix_v2.EVIDENCE_LEVELS.items():
        require(v2_level in V2_LEVEL_TO_CATALOG,
                "unknown V2 evidence level for %s: %s" %
                (profile_id, v2_level))
        v2_rank = rank[V2_LEVEL_TO_CATALOG[v2_level]]
        family = claims_for_profile(claims, profile_id)
        catalog_rank = max(rank[claim["level"]] for claim in family)
        require(v2_rank <= catalog_rank,
                "V2 profile %s claims %s, above what the CFW evidence "
                "catalog records for its family" % (profile_id, v2_level))


def main():
    claims = read_catalog()
    profile_count = validate_v2_profiles_still_covered(claims)
    validate_disaggregation(claims)
    validate_evidence_binding(claims)
    print("firmware matrix v3 gate passed: v2_profiles=%d disaggregated=1 "
          "evidence_bound=1" % profile_count)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CoverageError, KeyError, TypeError, ValueError, OSError,
            json.JSONDecodeError) as error:
        print("firmware matrix v3 gate failed: %s" % error, file=sys.stderr)
        raise SystemExit(1)
