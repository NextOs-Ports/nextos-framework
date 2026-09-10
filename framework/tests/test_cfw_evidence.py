#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fail-closed honesty gate for the V3 CFW evidence catalog.

Validates framework/evidence/CFW-EVIDENCE.json against the semantics of
framework/evidence/CFW-EVIDENCE.schema.json with hand-rolled checks (no
external jsonschema dependency), plus honesty rules the schema alone cannot
express: no level promotion without a receipt or an explicit receipt gap, no
private literals, no claim-level conflicts and no inheritance between claims.
"""

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath


REPOSITORY = Path(__file__).resolve().parents[2]
EVIDENCE_ROOT = REPOSITORY / "framework/evidence"
CATALOG_PATH = EVIDENCE_ROOT / "CFW-EVIDENCE.json"
SCHEMA_PATH = EVIDENCE_ROOT / "CFW-EVIDENCE.schema.json"

CLAIM_KEYS = {
    "id", "cfw", "hardware", "runtime", "frontend", "framework_ref",
    "level", "scope", "scenarios", "receipt", "gaps",
}
CFW_KEYS = {"name", "version_or_build", "origin"}
CFW_ORIGINS = ("official-image", "contract", "release-notes", "none")
HARDWARE_KEYS = {
    "vendor_model", "soc", "gpu_driver", "display_compositor",
    "resolution", "orientation",
}
RUNTIME_KEYS = {
    "host_abi", "guest_abi", "libc", "filesystem_semantics", "ram_free",
}
FRONTEND_KEYS = {"name", "portmaster_version", "control_mapping_source"}
FRAMEWORK_REF_KEYS = {"tag_or_commit", "components_note"}
RECEIPT_KEYS = {"present", "sanitized_ref", "private_manifest_sha256"}
PHYSICAL_RECEIPT_KEYS = {
    "schema", "schema_version", "claim_id", "evidence_level",
    "evidence_index_ref", "evidence_index_sha256", "evidence_path_prefix",
    "framework", "port",
    "artifact", "artifact_proofs", "stack", "scenarios", "claim_bindings",
    "single_channel", "owner_state", "gaps", "private_manifest_sha256",
    "sanitized",
}
PHYSICAL_FRAMEWORK_KEYS = {
    "tested_commit", "tested_tag", "aggregate_release_tag",
    "components_sha256", "nxrelease_manifest_file_sha256",
    "component_commits", "composition",
}
PHYSICAL_PORT_KEYS = {"id", "version", "commit"}
PHYSICAL_ARTIFACT_KEYS = {
    "zip_sha256", "zip_size", "executable_member", "executable_sha256",
    "executable_build_id", "generation_id",
}
PHYSICAL_STACK_KEYS = {
    "cfw", "hardware_class", "renderer", "graphics_api", "drawable",
}
PHYSICAL_SCENARIO_KEYS = {"passed", "evidence_ref", "evidence_sha256"}
PHYSICAL_SINGLE_CHANNEL_KEYS = {"route", "evidence_ref", "evidence_sha256"}
PHYSICAL_OWNER_STATE_KEYS = {
    "before_evidence", "after_evidence", "compared", "byte_identical",
}
PHYSICAL_ARTIFACT_PROOF_KEYS = {"candidate_manifest", "freeze"}
PHYSICAL_EVIDENCE_ENTRY_KEYS = {"evidence_ref", "evidence_sha256"}
PHYSICAL_OWNER_COMPARISON_KEYS = {
    "home", "gamedata", "gptk", "settings",
}
PHYSICAL_OWNER_HASH_KEYS = {"before_sha256", "after_sha256"}
# Enum exato, na ordem do schema. Nao adicionar nivel sem mexer no schema.
LEVELS = (
    "design-only", "synthetic", "image-backed", "community",
    "physical-subsystem", "physical-full",
)
KEBAB_ID = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
SHA256_HEX = re.compile(r"^[0-9a-f]{64}$")
COMMIT_HEX = re.compile(r"^[0-9a-f]{40}$")
BUILD_ID_HEX = re.compile(r"^[0-9a-f]{16,64}$")
SCENARIO_ID = re.compile(r"^[a-z0-9]+(?:_[a-z0-9]+)*$")
TAG_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]*$")
MANIFEST_LINE = re.compile(r"^([0-9a-f]{64})  (.+)$")
# Literais privados proibidos em QUALQUER lugar do catalogo.
IPV4_LITERAL = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
PRIVATE_TOKENS = ("/home/", "/Users/", "root@", ".local")
# Nao-heranca: cada claim prova sozinho o que afirma.
INHERITANCE_WORDS = ("herda", "inherited from", "same as")
# Autoverificacao: as nove familias de perfil V2 precisam aparecer em ids.
V2_PROFILE_FAMILIES = (
    "muos", "rocknix", "amberelec", "knulli", "arkos", "darkosre",
    "nextos", "trimui", "spruce",
)


class EvidenceError(Exception):
    """Malformed, dishonest or unsafe evidence catalog."""


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


def read_json(path):
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result,
                    "duplicate JSON key in %s: %s" % (path, key))
            result[key] = value
        return result

    require(path.is_file() and not path.is_symlink(),
            "missing or unsafe JSON: %s" % path)
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=no_duplicates)


def require_string_object(mapping, expected_keys, context):
    require(isinstance(mapping, dict) and set(mapping) == expected_keys,
            "%s fields changed" % context)
    for key in sorted(expected_keys):
        value = mapping[key]
        require(isinstance(value, str) and value.strip(),
                "%s.%s must be a non-empty string" % (context, key))


def validate_schema_identity(schema):
    require(isinstance(schema, dict) and
            schema.get("$schema") ==
            "https://json-schema.org/draft/2020-12/schema",
            "evidence schema is not draft 2020-12")
    properties = schema.get("properties", {})
    require(properties.get("schema", {}).get("const") ==
            "nx-cfw-evidence-v1" and
            properties.get("schema_version", {}).get("const") == 1,
            "evidence schema identity changed")
    level_enum = (schema.get("$defs", {}).get("claim", {})
                  .get("properties", {}).get("level", {}).get("enum"))
    require(tuple(level_enum or ()) == LEVELS,
            "evidence schema level enum drifted from the gate")


def validate_string_lists(value, context, allow_empty):
    require(isinstance(value, list),
            "%s must be an array" % context)
    require(allow_empty or value,
            "%s may not be empty" % context)
    for item in value:
        require(isinstance(item, str) and item.strip(),
                "%s entries must be non-empty strings" % context)


def validate_receipt(receipt, context):
    require(isinstance(receipt, dict) and set(receipt) == RECEIPT_KEYS,
            "%s receipt fields changed" % context)
    require(isinstance(receipt["present"], bool),
            "%s receipt.present must be a boolean" % context)
    reference = receipt["sanitized_ref"]
    require(reference is None or
            (isinstance(reference, str) and reference.strip()),
            "%s receipt.sanitized_ref must be a string or null" % context)
    digest = receipt["private_manifest_sha256"]
    require(digest is None or
            (isinstance(digest, str) and SHA256_HEX.fullmatch(digest)),
            "%s receipt.private_manifest_sha256 must be sha256 hex or null" %
            context)
    if receipt["present"]:
        require(reference is not None and digest is not None,
                "%s receipt.present requires reference and private digest" %
                context)
    else:
        require(reference is None and digest is None,
                "%s absent receipt must keep reference and digest null" %
                context)


def require_sha256(value, context):
    require(isinstance(value, str) and SHA256_HEX.fullmatch(value),
            "%s must be sha256 hex" % context)


def require_safe_relative_path(value, context):
    require(isinstance(value, str) and value.strip(),
            "%s must be a non-empty path" % context)
    candidate = PurePosixPath(value)
    require(not candidate.is_absolute() and
            all(part not in ("", ".", "..") for part in candidate.parts) and
            candidate.as_posix() == value,
            "%s must be a normalized safe relative POSIX path" % context)
    return candidate


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def resolve_safe_evidence_file(value, context, under_receipts=False):
    """Resolve a versioned evidence file without following any symlink."""
    relative = require_safe_relative_path(value, context)
    if under_receipts:
        require(relative.parts and relative.parts[0] == "receipts",
                "%s must live under framework/evidence/receipts" % context)

    root = EVIDENCE_ROOT.resolve()
    candidate = EVIDENCE_ROOT
    for part in relative.parts:
        candidate = candidate / part
        require(not candidate.is_symlink(),
                "%s contains a symlink: %s" % (context, value))
    try:
        candidate.resolve().relative_to(root)
    except ValueError:
        raise EvidenceError("%s escapes the evidence root" % context)
    require(candidate.is_file(),
            "%s is missing: %s" % (context, value))
    return candidate


def read_evidence_index(document, context):
    index_ref = document["evidence_index_ref"]
    index_path = resolve_safe_evidence_file(
        index_ref, context + ".evidence_index_ref", under_receipts=True)
    require(index_path.suffix == ".sha256",
            "%s evidence index must be a .sha256 file" % context)
    require_sha256(document["evidence_index_sha256"],
                   context + ".evidence_index_sha256")
    require(sha256_file(index_path) == document["evidence_index_sha256"],
            "%s evidence index digest mismatch" % context)

    raw_text = index_path.read_text(encoding="utf-8")
    validate_private_literals(raw_text)
    entries = {}
    for line_number, line in enumerate(raw_text.splitlines(), start=1):
        require(line.strip() == line and line,
                "%s evidence index has a blank or padded line %d" %
                (context, line_number))
        match = MANIFEST_LINE.fullmatch(line)
        require(match is not None,
                "%s evidence index line %d is malformed" %
                (context, line_number))
        digest, reference = match.groups()
        require_safe_relative_path(
            reference, "%s evidence index line %d path" %
            (context, line_number))
        require(reference not in entries,
                "%s evidence index repeats %s" % (context, reference))
        entries[reference] = digest
    require(entries, "%s evidence index is empty" % context)
    return entries


def validate_evidence_entry(entry, index, context, prefix=None):
    require(isinstance(entry, dict) and
            set(entry) == PHYSICAL_EVIDENCE_ENTRY_KEYS,
            "%s fields changed" % context)
    relative = require_safe_relative_path(entry["evidence_ref"],
                                          context + ".evidence_ref")
    require_sha256(entry["evidence_sha256"],
                   context + ".evidence_sha256")
    require(index.get(entry["evidence_ref"]) == entry["evidence_sha256"],
            "%s is not bound to the sanitized evidence index" % context)
    if prefix is not None:
        require(relative.parts and
                (relative.parts[0] == prefix or
                 relative.parts[0].startswith(prefix + "-")),
                "%s points outside evidence prefix %s" %
                (context, prefix))


def resolve_tag_commit(tag, context):
    require(isinstance(tag, str) and TAG_NAME.fullmatch(tag) and
            ".." not in tag and "@{" not in tag and not tag.endswith("/"),
            "%s tag name is unsafe: %r" % (context, tag))
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet",
         "refs/tags/%s^{commit}" % tag],
        cwd=REPOSITORY, check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    require(result.returncode == 0 and result.stdout.strip(),
            "%s tag is missing from the repository: %s" % (context, tag))
    return result.stdout.strip()


def require_repository_commit(commit, context):
    result = subprocess.run(
        ["git", "cat-file", "-e", commit + "^{commit}"],
        cwd=REPOSITORY, check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    require(result.returncode == 0,
            "%s commit is missing from the repository: %s" %
            (context, commit))


def mali_model(value):
    match = re.search(r"mali[ -]*([a-z]?\d+)", value.lower())
    return None if match is None else "mali" + match.group(1)


def validate_physical_receipt(claim, seen_references):
    context = "claim %s physical receipt" % claim["id"]
    reference = claim["receipt"]["sanitized_ref"]
    require(reference not in seen_references,
            "%s reuses receipt reference %s" % (context, reference))
    seen_references.add(reference)

    receipt_path = resolve_safe_evidence_file(
        reference, context + ".reference", under_receipts=True)

    raw_text = receipt_path.read_text(encoding="utf-8")
    validate_private_literals(raw_text)
    document = read_json(receipt_path)
    require(isinstance(document, dict) and
            set(document) == PHYSICAL_RECEIPT_KEYS,
            "%s root fields changed" % context)
    require(document["schema"] == "nx-v3-physical-receipt-v1" and
            document["schema_version"] == 1,
            "%s schema identity changed" % context)
    require(document["claim_id"] == claim["id"],
            "%s claim_id does not match the catalog" % context)
    require(isinstance(document["evidence_level"], str) and
            document["evidence_level"].removesuffix("-scoped") ==
            claim["level"],
            "%s evidence level differs from the catalog" % context)
    require(document["sanitized"] is True,
            "%s must declare sanitized=true" % context)
    require(document["private_manifest_sha256"] ==
            claim["receipt"]["private_manifest_sha256"],
            "%s private manifest digest differs from the catalog" % context)
    evidence_prefix = document["evidence_path_prefix"]
    require(isinstance(evidence_prefix, str) and
            KEBAB_ID.fullmatch(evidence_prefix),
            "%s evidence_path_prefix must be kebab-case" % context)
    evidence_index = read_evidence_index(document, context)

    framework = document["framework"]
    require(isinstance(framework, dict) and
            set(framework) == PHYSICAL_FRAMEWORK_KEYS,
            "%s framework fields changed" % context)
    require(isinstance(framework["tested_commit"], str) and
            COMMIT_HEX.fullmatch(framework["tested_commit"]),
            "%s framework.tested_commit must be 40 hex" % context)
    require_repository_commit(framework["tested_commit"],
                              context + ".framework.tested_commit")
    require(isinstance(framework["tested_tag"], str) and
            framework["tested_tag"].strip(),
            "%s framework.tested_tag must be non-empty" % context)
    require(resolve_tag_commit(framework["tested_tag"], context) ==
            framework["tested_commit"],
            "%s tested_tag does not resolve to tested_commit" % context)
    aggregate_tag = framework["aggregate_release_tag"]
    require(aggregate_tag is None or
            (isinstance(aggregate_tag, str) and
             TAG_NAME.fullmatch(aggregate_tag) and
             ".." not in aggregate_tag and "@{" not in aggregate_tag and
             not aggregate_tag.endswith("/") and
             aggregate_tag.upper() != "PENDING"),
            "%s aggregate_release_tag must be null or an immutable tag" %
            context)
    if aggregate_tag is not None:
        resolve_tag_commit(aggregate_tag,
                           context + ".framework.aggregate_release_tag")
    require(framework["composition"] == "base-tag-plus-pinned-components",
            "%s framework.composition is unsupported" % context)
    require_sha256(framework["components_sha256"],
                   context + ".framework.components_sha256")
    require_sha256(framework["nxrelease_manifest_file_sha256"],
                   context + ".framework.nxrelease_manifest_file_sha256")
    commits = framework["component_commits"]
    require(isinstance(commits, dict) and commits,
            "%s framework.component_commits must be non-empty" % context)
    for component, commit in sorted(commits.items()):
        require(isinstance(component, str) and KEBAB_ID.fullmatch(component),
                "%s has an invalid component name: %r" %
                (context, component))
        require(isinstance(commit, str) and COMMIT_HEX.fullmatch(commit),
                "%s component %s commit must be 40 hex" %
                (context, component))
        require_repository_commit(commit,
                                  "%s component %s" % (context, component))

    port = document["port"]
    require(isinstance(port, dict) and set(port) == PHYSICAL_PORT_KEYS,
            "%s port fields changed" % context)
    require(isinstance(port["id"], str) and KEBAB_ID.fullmatch(port["id"]),
            "%s port.id is invalid" % context)
    require(isinstance(port["version"], str) and port["version"].strip(),
            "%s port.version must be non-empty" % context)
    require(isinstance(port["commit"], str) and
            COMMIT_HEX.fullmatch(port["commit"]),
            "%s port.commit must be 40 hex" % context)

    artifact = document["artifact"]
    require(isinstance(artifact, dict) and
            set(artifact) == PHYSICAL_ARTIFACT_KEYS,
            "%s artifact fields changed" % context)
    require_sha256(artifact["zip_sha256"], context + ".artifact.zip_sha256")
    require(isinstance(artifact["zip_size"], int) and
            not isinstance(artifact["zip_size"], bool) and
            artifact["zip_size"] > 0,
            "%s artifact.zip_size must be positive" % context)
    require_safe_relative_path(artifact["executable_member"],
                               context + ".artifact.executable_member")
    require_sha256(artifact["executable_sha256"],
                   context + ".artifact.executable_sha256")
    require(isinstance(artifact["executable_build_id"], str) and
            BUILD_ID_HEX.fullmatch(artifact["executable_build_id"]),
            "%s artifact.executable_build_id is invalid" % context)
    require_sha256(artifact["generation_id"],
                   context + ".artifact.generation_id")

    artifact_proofs = document["artifact_proofs"]
    require(isinstance(artifact_proofs, dict) and
            set(artifact_proofs) == PHYSICAL_ARTIFACT_PROOF_KEYS,
            "%s artifact_proofs fields changed" % context)
    for proof_name, proof in sorted(artifact_proofs.items()):
        validate_evidence_entry(
            proof, evidence_index,
            "%s.artifact_proofs.%s" % (context, proof_name),
            prefix="candidate",
        )

    stack = document["stack"]
    require_string_object(stack, PHYSICAL_STACK_KEYS, context + ".stack")
    catalog_cfw = re.sub(r"[^a-z0-9]", "", claim["cfw"]["name"].lower())
    receipt_cfw = re.sub(r"[^a-z0-9]", "", stack["cfw"].lower())
    require(catalog_cfw in receipt_cfw,
            "%s stack.cfw does not match the catalog" % context)
    resolution = re.search(r"(\d+x\d+)",
                           claim["hardware"]["resolution"].lower())
    require(resolution is not None and
            stack["drawable"].lower() == resolution.group(1),
            "%s stack.drawable does not match catalog resolution" % context)
    catalog_mali = mali_model(claim["hardware"]["gpu_driver"])
    receipt_mali = mali_model(stack["renderer"])
    require(catalog_mali is not None and catalog_mali == receipt_mali,
            "%s stack.renderer does not match catalog GPU" % context)

    scenarios = document["scenarios"]
    require(isinstance(scenarios, dict) and scenarios,
            "%s scenarios must be a non-empty object" % context)
    for scenario_id, result in sorted(scenarios.items()):
        require(isinstance(scenario_id, str) and
                SCENARIO_ID.fullmatch(scenario_id),
                "%s has an invalid scenario id: %r" %
                (context, scenario_id))
        require(isinstance(result, dict) and
                set(result) == PHYSICAL_SCENARIO_KEYS,
                "%s scenario %s fields changed" % (context, scenario_id))
        require(result["passed"] is True,
                "%s scenario %s is not a passing proof" %
                (context, scenario_id))
        validate_evidence_entry(
            {key: result[key] for key in PHYSICAL_EVIDENCE_ENTRY_KEYS},
            evidence_index,
            "%s scenario %s evidence" % (context, scenario_id),
            prefix=evidence_prefix,
        )

    claim_bindings = document["claim_bindings"]
    require(isinstance(claim_bindings, dict) and
            set(claim_bindings) == set(claim["scenarios"]),
            "%s claim_bindings differ from catalog scenarios" % context)
    bound_scenarios = set()
    for claim_scenario, receipt_scenarios in claim_bindings.items():
        validate_string_lists(
            receipt_scenarios,
            "%s.claim_bindings.%s" % (context, claim_scenario),
            allow_empty=False,
        )
        require(len(receipt_scenarios) == len(set(receipt_scenarios)),
                "%s claim binding repeats a receipt scenario" % context)
        for scenario_id in receipt_scenarios:
            require(scenario_id in scenarios,
                    "%s claim binding references unknown scenario %s" %
                    (context, scenario_id))
            bound_scenarios.add(scenario_id)
    require(bound_scenarios == set(scenarios),
            "%s receipt scenarios are not all bound to catalog claims" %
            context)

    single_channel = document["single_channel"]
    require(isinstance(single_channel, dict) and
            set(single_channel) == PHYSICAL_SINGLE_CHANNEL_KEYS,
            "%s single_channel fields changed" % context)
    require(isinstance(single_channel["route"], str) and
            single_channel["route"].strip(),
            "%s single_channel.route must be non-empty" % context)
    validate_evidence_entry(
        {key: single_channel[key]
         for key in PHYSICAL_EVIDENCE_ENTRY_KEYS},
        evidence_index, context + ".single_channel",
        prefix=evidence_prefix,
    )

    owner_state = document["owner_state"]
    require(isinstance(owner_state, dict) and
            set(owner_state) == PHYSICAL_OWNER_STATE_KEYS,
            "%s owner_state fields changed" % context)
    for state_name in ("before_evidence", "after_evidence"):
        validate_evidence_entry(
            owner_state[state_name], evidence_index,
            "%s.owner_state.%s" % (context, state_name),
            prefix=evidence_prefix,
        )
    compared = owner_state["compared"]
    require(isinstance(compared, dict) and
            set(compared) == PHYSICAL_OWNER_COMPARISON_KEYS,
            "%s owner_state.compared fields changed" % context)
    for state_name, hashes in sorted(compared.items()):
        require(isinstance(hashes, dict) and
                set(hashes) == PHYSICAL_OWNER_HASH_KEYS,
                "%s owner_state.compared.%s fields changed" %
                (context, state_name))
        require_sha256(hashes["before_sha256"],
                       "%s owner %s before hash" % (context, state_name))
        require_sha256(hashes["after_sha256"],
                       "%s owner %s after hash" % (context, state_name))
        require(hashes["before_sha256"] == hashes["after_sha256"],
                "%s owner %s changed across the test" %
                (context, state_name))
    require(owner_state["byte_identical"] is True,
            "%s owner_state must be byte-identical" % context)
    validate_string_lists(document["gaps"], context + ".gaps",
                          allow_empty=True)

    framework_text = (claim["framework_ref"]["tag_or_commit"] + " " +
                      claim["framework_ref"]["components_note"])
    require(framework["tested_commit"][:8] in framework_text,
            "%s tested framework commit is not bound by the catalog" %
            context)
    for commit in commits.values():
        require(commit[:8] in framework_text,
                "%s component commit is not bound by the catalog: %s" %
                (context, commit[:8]))
    require(port["commit"][:8] in framework_text and
            artifact["zip_sha256"] in framework_text and
            artifact["generation_id"] in framework_text,
            "%s artifact identity is not bound by the catalog" % context)
    return document


def validate_claim(claim):
    require(isinstance(claim, dict) and set(claim) == CLAIM_KEYS,
            "claim fields changed: %r" % claim.get("id"))
    claim_id = claim["id"]
    require(isinstance(claim_id, str) and KEBAB_ID.fullmatch(claim_id),
            "claim id is not kebab-case: %r" % claim_id)
    context = "claim %s" % claim_id

    cfw = claim["cfw"]
    require_string_object(cfw, CFW_KEYS, context + ".cfw")
    require(cfw["origin"] in CFW_ORIGINS,
            context + " has an unapproved cfw.origin: %r" % cfw["origin"])
    require_string_object(claim["hardware"], HARDWARE_KEYS,
                          context + ".hardware")
    require_string_object(claim["runtime"], RUNTIME_KEYS,
                          context + ".runtime")
    require_string_object(claim["frontend"], FRONTEND_KEYS,
                          context + ".frontend")
    require_string_object(claim["framework_ref"], FRAMEWORK_REF_KEYS,
                          context + ".framework_ref")

    level = claim["level"]
    require(level in LEVELS, context + " has unknown level: %r" % level)
    scope = claim["scope"]
    require(isinstance(scope, str) and scope.strip(),
            context + " scope must be a non-empty string")
    lowered_scope = scope.lower()
    for word in INHERITANCE_WORDS:
        require(word not in lowered_scope,
                context + " scope inherits evidence (%r); every claim "
                "must stand alone" % word)

    validate_string_lists(claim["scenarios"], context + ".scenarios",
                          allow_empty=(level == "design-only"))
    validate_receipt(claim["receipt"], context)
    validate_string_lists(claim["gaps"], context + ".gaps", allow_empty=True)

    # Honestidade: nivel fisico sem receipt desta tag exige o buraco escrito.
    if level.startswith("physical-"):
        receipt_gap = any("receipt" in gap.lower() for gap in claim["gaps"])
        require(claim["receipt"]["present"] is True or receipt_gap,
                context + " claims %s without a receipt and without a gap "
                "naming the missing receipt" % level)
    if claim["receipt"]["present"]:
        require(claim["receipt"]["sanitized_ref"] is not None,
                context + " receipt.present without a sanitized_ref")
    return claim_id, level


def validate_catalog(document):
    require(isinstance(document, dict) and set(document) == {
                "schema", "schema_version", "generated_note", "claims",
            }, "evidence catalog root fields changed")
    require(document["schema"] == "nx-cfw-evidence-v1" and
            document["schema_version"] == 1,
            "evidence catalog identity changed")
    require(isinstance(document["generated_note"], str) and
            document["generated_note"].strip(),
            "evidence catalog generated_note is missing")
    claims = document["claims"]
    require(isinstance(claims, list) and claims,
            "evidence catalog has no claims")

    seen_ids = set()
    seen_receipt_references = set()
    physical_receipts = []
    conflict_index = {}
    for claim in claims:
        claim_id, level = validate_claim(claim)
        require(claim_id not in seen_ids,
                "duplicate claim id: %s" % claim_id)
        seen_ids.add(claim_id)
        conflict_key = (claim["cfw"]["name"], claim["cfw"]["version_or_build"],
                        claim["hardware"]["vendor_model"])
        previous = conflict_index.setdefault(conflict_key, (claim_id, level))
        require(previous[1] == level,
                "conflicting levels for the same CFW build and device: "
                "%s=%s vs %s=%s" % (previous[0], previous[1],
                                    claim_id, level))
        if claim["receipt"]["present"]:
            physical_receipts.append(
                validate_physical_receipt(claim, seen_receipt_references))

    receipt_groups = {}
    for receipt in physical_receipts:
        # O manifesto privado e a ancora externa comum da rodada. Nao incluir
        # campos mutaveis do proprio receipt na chave: isso permitiria que um
        # receipt divergente simplesmente escapasse do cohort.
        group_key = receipt["private_manifest_sha256"]
        receipt_groups.setdefault(group_key, []).append(receipt)
    shared_artifact_groups = 0
    for grouped_receipts in receipt_groups.values():
        if len(grouped_receipts) < 2:
            continue
        shared_artifact_groups += 1
        baseline = grouped_receipts[0]
        for receipt in grouped_receipts[1:]:
            for field in ("evidence_index_ref", "framework", "port",
                          "artifact", "artifact_proofs"):
                require(receipt[field] == baseline[field],
                        "receipts from one evidence bundle disagree on %s" %
                        field)

    for family in V2_PROFILE_FAMILIES:
        require(any(family in claim_id for claim_id in seen_ids),
                "V2 profile family has no claim in the catalog: %s" % family)
    return (len(claims), len(seen_receipt_references),
            shared_artifact_groups)


def validate_private_literals(raw_text):
    require(IPV4_LITERAL.search(raw_text) is None,
            "evidence catalog contains an IPv4 literal")
    for token in PRIVATE_TOKENS:
        require(token not in raw_text,
                "evidence catalog contains a private literal: %s" % token)


def main():
    raw_text = CATALOG_PATH.read_text(encoding="utf-8")
    validate_private_literals(raw_text)
    document = read_json(CATALOG_PATH)
    validate_schema_identity(read_json(SCHEMA_PATH))
    claim_count, receipt_count, shared_artifact_groups = validate_catalog(
        document)
    print("CFW evidence gate passed: claims=%d levels_ok=1 no_promotion=1 "
          "receipts=%d receipt_index_binding=1 shared_artifact_groups=%d "
          "no_private_literals=1 conflicts=0" %
          (claim_count, receipt_count, shared_artifact_groups))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (EvidenceError, KeyError, TypeError, ValueError, OSError,
            json.JSONDecodeError) as error:
        print("CFW evidence gate failed: %s" % error, file=sys.stderr)
        raise SystemExit(1)
