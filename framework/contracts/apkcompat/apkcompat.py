# SPDX-License-Identifier: GPL-3.0-only
# NXCOMPAT-APK -- canonical, dependency-free APK compatibility contract (V3).
#
# This module is the SINGLE source of truth for the rule that decides how a
# recipe may describe the owner-provided container (APK/APKM/APKS/XAPK).
# It is consumed by three layers:
#   - suportando_outros_devices/extrator-universal/nxextract.py (embedded copy,
#     byte-identity enforced by a sync gate in the tests of both sides),
#   - framework/nxgenerator/nxgenerator.py (imported by file path),
#   - framework/nxrelease/nxrelease.py (imported by file path).
#
# V3 permanent rule (APK-COMPAT-01):
#   Identity of the owner-provided container (sha256, crc32, exact size, file
#   name, signature, packaging tool, member order/timestamps, exact
#   versionCode, literal version text) must NEVER decide compatibility --
#   neither alone nor in a list of any length. Those values may appear only in
#   the documentation-only `reference_build` block. Compatibility is decided
#   by runtime contracts: package family, ABI, required structure/members,
#   engine/metadata format, ELF architecture and consumed symbols/interfaces.
#   Internal payload hashes may only SELECT a patch profile that really
#   depends on those bytes, and every profile must declare a generic/symbolic
#   fallback; a compatible-but-unknown build follows the fallback and is never
#   rejected for missing a whitelist.
#
# Error code family: NXA#### (APK compatibility).

APKCOMPAT_SCHEMA = "org.nextos.apk-compat"
APKCOMPAT_SCHEMA_VERSION = 3

import fnmatch as _fnmatch
import re as _re

# 64 hex digits used as an equality predicate.
_HEX64_RE = _re.compile(r"\b[0-9a-fA-F]{64}\b")
# Dotted literal version token with at least three numeric components
# (a five-component game build number, say). Two-component tokens are too common in honest
# text (schema versions, GLIBC) to flag by default.
_DOTTED_VERSION_RE = _re.compile(r"\b\d+(?:\.\d+){2,}\b")

CONTAINER_IDENTITY_KEYS = ("sha256", "crc32", "size")

PREDICATE_CLASSES = ("reference_identity", "compatibility", "patch_selection")

REFERENCE_BUILD_KEYS = (
    "game_version",
    "version_code",
    "container_size",
    "container_sha256",
    "note",
)

COMPATIBILITY_KEYS = (
    "package_families",
    "abis",
    "required_members",
    "payload_contracts",
    "required_symbols_or_interfaces",
)

PATCH_PROFILE_KEYS = ("id", "match_internal_payload", "fallback", "note")

MAX_REQUIRED_MEMBERS = 256
MAX_PATCH_PROFILES = 128
MAX_MEMBER_PATH = 1024

# Roles a declared required member may carry. A plain string defaults to
# core_required. core_required: present in EVERY compatible build. optional: may
# be absent (documented, never gates). variant_required: required only for a
# named variant. patch_selector: names bytes that pick a patch profile, it never
# gates acceptance on its own (a build missing it follows the fallback).
MEMBER_ROLES = (
    "core_required", "optional", "variant_required", "patch_selector",
)
# Container-identity keys that must NEVER gate acceptance (cosmetic identity of
# the owner-provided copy). A re-signed, renamed or versionCode-bumped build
# that is structurally identical stays compatible.
CONTAINER_COSMETIC_IDENTITY = (
    "signature", "signing_cert", "cert_sha1", "cert_sha256",
    "filename", "name", "version_code", "versionCode",
)
# A hex SHA-1 certificate fingerprint (40 hex) used as an equality predicate.
_HEX40_RE = _re.compile(r"\b[0-9a-fA-F]{40}\b")
_SIGNING_MEMBER_TEXT_RE = _re.compile(
    r"META-INF[/\\\\][^\s'\"]*(?:\.SF|\.RSA|\.DSA|\.EC|MANIFEST\.MF)",
    _re.IGNORECASE,
)

HOOK_CONTRACT_SCHEMA = "org.nextos.apk-compat.hook-contract"
HOOK_CONTRACT_SCHEMA_VERSION = 1
HOOK_CONTRACT_KEYS = (
    "schema",
    "schema_version",
    "hook_id",
    "inputs",
    "predicates",
    "patch_profiles",
    "fallback",
    "error_codes",
)


def _is_bool(value):
    return isinstance(value, bool)


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _is_text(value):
    return isinstance(value, str)


def _is_string_list(value):
    return isinstance(value, list) and all(
        _is_text(item) and item for item in value
    )


def _normalize_member_path(value, label, fail):
    """Return one canonical APK-member path, or ``None`` after reporting it.

    The canonical contract deliberately has no pathlib dependency.  These are
    the same portable path properties enforced by NXExtract when it opens a
    ZIP: relative POSIX spelling, no empty/dot traversal components, no
    control characters and no Windows-hostile trailing dot/space.
    """
    if not _is_text(value) or not value or len(value) > MAX_MEMBER_PATH:
        fail("NXA0026 %s must be a non-empty path of at most %d characters"
             % (label, MAX_MEMBER_PATH))
        return None
    if (value.startswith("/") or "\\" in value or "\x00" in value
            or any(ord(character) < 32 for character in value)):
        fail("NXA0026 %s is not a safe relative APK-member path: %r"
             % (label, value))
        return None
    parts = value.split("/")
    if any(part in ("", ".", "..") or ":" in part
           or part.endswith((" ", ".")) for part in parts):
        fail("NXA0026 %s is not a portable APK-member path: %r"
             % (label, value))
        return None
    return "/".join(parts)


def _is_signing_member(value):
    """Whether a member names Android/JAR signature or certificate material."""
    if not _is_text(value):
        return False
    upper = value.upper()
    if not upper.startswith("META-INF/"):
        return False
    name = upper.rsplit("/", 1)[-1]
    return (
        name == "MANIFEST.MF"
        or name == "STAMP-CERT-SHA256"
        or name.endswith((".SF", ".RSA", ".DSA", ".EC"))
    )


def _is_signing_member_pattern(value):
    """Conservative declarative check for a required signature-member rule."""
    if not _is_text(value):
        return False
    upper = value.replace("\\", "/").upper()
    name = upper.rsplit("/", 1)[-1]
    signing_name = (
        name == "MANIFEST.MF"
        or name == "STAMP-CERT-SHA256"
        or name.endswith((".SF", ".RSA", ".DSA", ".EC"))
    )
    if upper.startswith("META-INF/"):
        return signing_name or any(token in name for token in "*?[")
    if "/" not in upper or not signing_name:
        return False
    directory = upper.rsplit("/", 1)[0]
    return _fnmatch.fnmatchcase("META-INF", directory)


def validate_container_rule_identity(rule_id, validate_block, fail,
                                     field="validate"):
    """NXA0001..NXA0003: forbid container-identity predicates in ANY quantity.

    `validate_block` is one source-side validation object for a source of kind
    `container`. sha256/crc32 lists of ANY length and an exact `size` are
    identity of the owner-provided copy, never compatibility. Bounded size
    (min_size/max_size), magic and elf_machine remain allowed because they
    test structure, not identity.
    """
    if validate_block is None:
        return
    if not isinstance(validate_block, dict):
        fail("NXA0001 extract %s container %s must be an object"
             % (rule_id, field))
        return
    if "sha256" in validate_block:
        fail(
            "NXA0001 extract %s %s: sha256 of the owner-provided container is "
            "identity, not compatibility; it must not gate acceptance in any "
            "quantity (document only the tested container in reference_build)"
            % (rule_id, field)
        )
    if "crc32" in validate_block:
        fail(
            "NXA0002 extract %s %s: crc32 of the owner-provided container is "
            "identity, not compatibility; it must not gate acceptance in any "
            "quantity (document only the tested container in reference_build)"
            % (rule_id, field)
        )
    if "size" in validate_block:
        fail(
            "NXA0003 extract %s %s: exact container size is identity, not "
            "compatibility; use min_size/max_size bounds or omit it"
            % (rule_id, field)
        )
    if any(key in validate_block for key in (
            "signature", "signing_cert", "cert_sha1", "cert_sha256")):
        fail(
            "NXA0004 extract %s %s: the container signature/signing certificate is "
            "identity, not compatibility; it must not gate acceptance (a "
            "re-signed but structurally identical build stays compatible)"
            % (rule_id, field)
        )
    if "filename" in validate_block or "name" in validate_block:
        fail(
            "NXA0005 extract %s %s: the container file name is identity, not "
            "compatibility; a renamed but identical build stays compatible" %
            (rule_id, field)
        )
    if "version_code" in validate_block or "versionCode" in validate_block:
        fail(
            "NXA0006 extract %s %s: the exact versionCode is identity, not "
            "compatibility; decide by structure/members/ABI (a bumped build "
            "stays compatible) and document the code in reference_build" %
            (rule_id, field)
        )


def validate_container_source_identity(rule_id, source, fail):
    """NXA0005: an external container name may not narrow source selection."""
    if not isinstance(source, dict):
        return
    patterns = source.get("patterns")
    if patterns is None:
        return
    if not isinstance(patterns, list) or patterns != ["*"]:
        fail(
            "NXA0005 extract %s: container source.patterns must be exactly "
            "['*']; external file names and extensions never decide "
            "compatibility (select an APK split by its manifest split value)"
            % rule_id
        )


def validate_signing_member_rule(rule_id, source, fail):
    """NXA0004: extraction may not expose signing identity to later gates."""
    if not isinstance(source, dict):
        return
    if source.get("kind") not in ("entry", "entries", "entry_or_file"):
        return
    for pattern in source.get("patterns") or []:
        if _is_signing_member_pattern(pattern):
            fail(
                "NXA0004 extract %s: Android signature/certificate "
                "member pattern %r is container identity, not compatibility"
                % (rule_id, pattern)
            )


def validate_reference_build(block, fail):
    """NXA0010..NXA0012: documentation-only identity of the tested copy."""
    if block is None:
        return
    if not isinstance(block, dict):
        fail("NXA0010 reference_build must be an object")
        return
    for key in block:
        if key not in REFERENCE_BUILD_KEYS:
            fail(
                "NXA0011 reference_build.%s is not a documented field; only "
                "%s are allowed" % (key, ", ".join(REFERENCE_BUILD_KEYS))
            )
    for key, value in block.items():
        if key in ("container_size", "version_code"):
            if not (_is_int(value) or _is_text(value)):
                fail("NXA0012 reference_build.%s must be text or integer" % key)
        elif not _is_text(value):
            fail("NXA0012 reference_build.%s must be text" % key)


def validate_compatibility(block, fail, input_packages=None, abi_order=None):
    """NXA0020..NXA0026: the only block allowed to decide acceptance."""
    if block is None:
        return
    if not isinstance(block, dict):
        fail("NXA0020 compatibility must be an object")
        return
    for key in block:
        if key not in COMPATIBILITY_KEYS:
            fail(
                "NXA0021 compatibility.%s is unknown; allowed keys: %s"
                % (key, ", ".join(COMPATIBILITY_KEYS))
            )
    families = block.get("package_families")
    if families is not None:
        if not _is_string_list(families) or not families:
            fail("NXA0022 compatibility.package_families must be a non-empty string list")
        elif input_packages is not None:
            declared = {item.casefold() for item in families}
            engine = {item.casefold() for item in input_packages}
            if declared != engine:
                fail(
                    "NXA0023 compatibility.package_families must match "
                    "input.packages exactly (one source of decision)"
                )
    abis = block.get("abis")
    if abis is not None:
        if not _is_string_list(abis) or not abis:
            fail("NXA0024 compatibility.abis must be a non-empty string list")
        elif abi_order is not None:
            declared = {item.casefold() for item in abis}
            engine = {item.casefold() for item in abi_order}
            if declared != engine:
                fail(
                    "NXA0025 compatibility.abis must match abi_order exactly "
                    "(one source of decision)"
                )
    normalize_required_members(
        block.get("required_members"), fail, abi_order=abi_order
    )
    for key in ("payload_contracts", "required_symbols_or_interfaces"):
        value = block.get(key)
        if value is not None and not (
            isinstance(value, list)
            and all(_is_text(item) and item for item in value)
        ):
            fail("NXA0026 compatibility.%s must be a string list" % key)
    for item in block.get("payload_contracts") or []:
        if _is_text(item) and _HEX64_RE.search(item):
            fail(
                "NXA0027 compatibility.payload_contracts must describe "
                "structure, not carry a hash: %r" % item
            )


def normalize_required_members(members, fail, abi_order=None):
    """NXA0026..NXA0029: required_members may be plain member paths (each an
    implicit core_required) OR role-tagged objects {member, role[, variant]}.

    Roles (MEMBER_ROLES): core_required (in every compatible build), optional
    (may be absent, never gates), variant_required (required for a named
    variant), patch_selector (names bytes that pick a patch profile, never
    gates acceptance -- a build without it follows the fallback). A member path
    is structure, never a hash. Returns normalized dictionaries so the runtime
    consumes the exact same role interpretation that generator/release validate.
    """
    normalized = []
    if members is None:
        return normalized
    if not isinstance(members, list) or len(members) > MAX_REQUIRED_MEMBERS:
        fail("NXA0026 compatibility.required_members must be a list")
        return normalized
    variants = None
    if abi_order is not None:
        variants = {
            value.casefold(): value for value in abi_order
            if _is_text(value) and value
        }
    seen = set()
    for index, entry in enumerate(members):
        label = "compatibility.required_members[%d].member" % index
        role = "core_required"
        variant = None
        if _is_text(entry):
            member = entry
        elif isinstance(entry, dict):
            extra = set(entry) - {"member", "role", "variant"}
            if extra:
                fail(
                    "NXA0028 compatibility.required_members entry has unknown "
                    "key(s): %s" % ", ".join(sorted(extra))
                )
            member = entry.get("member")
            role = entry.get("role", "core_required")
            if role not in MEMBER_ROLES:
                fail(
                    "NXA0028 compatibility.required_members role must be one "
                    "of %s" % ", ".join(MEMBER_ROLES)
                )
            if role == "variant_required":
                variant = entry.get("variant")
                if not (_is_text(variant) and variant):
                    fail(
                        "NXA0029 compatibility.required_members variant_required "
                        "entry must name its variant"
                    )
                elif variants is None:
                    fail(
                        "NXA0029 compatibility.required_members "
                        "variant_required needs an explicit abi_order"
                    )
                elif variant.casefold() not in variants:
                    fail(
                        "NXA0029 compatibility.required_members variant %r "
                        "is not declared in abi_order" % variant
                    )
                else:
                    variant = variants[variant.casefold()]
            elif "variant" in entry:
                fail(
                    "NXA0029 compatibility.required_members variant is only "
                    "valid for role variant_required"
                )
        else:
            fail(
                "NXA0026 compatibility.required_members entry must be a member "
                "path or a {member, role} object"
            )
            continue
        member = _normalize_member_path(member, label, fail)
        if member is None:
            continue
        if _HEX64_RE.search(member):
            fail(
                "NXA0027 compatibility.required_members must describe "
                "structure, not carry a hash: %r" % member
            )
        if _is_signing_member(member):
            fail(
                "NXA0004 compatibility.required_members must not use Android "
                "signature/certificate member %r as a compatibility anchor"
                % member
            )
        key = member.casefold()
        if key in seen:
            fail(
                "NXA0028 compatibility.required_members contains duplicate or "
                "case-colliding member %r" % member
            )
            continue
        seen.add(key)
        record = {"member": member, "role": role}
        if role == "variant_required" and variant is not None:
            record["variant"] = variant
        normalized.append(record)
    return normalized


def validate_patch_profiles(block, fail):
    """NXA0030..NXA0035: authenticated internal-payload patch selection."""
    if block is None:
        return
    if not isinstance(block, list) or len(block) > MAX_PATCH_PROFILES:
        fail("NXA0030 patch_profiles must be a list")
        return
    seen = set()
    seen_matches = set()
    for index, profile in enumerate(block):
        label = "patch_profiles[%d]" % index
        if not isinstance(profile, dict):
            fail("NXA0030 %s must be an object" % label)
            continue
        for key in profile:
            if key not in PATCH_PROFILE_KEYS:
                fail("NXA0031 %s.%s is unknown" % (label, key))
        identifier = profile.get("id")
        if not _is_text(identifier) or not _re.fullmatch(
            r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", identifier or ""
        ):
            fail("NXA0032 %s.id must be 1-64 safe characters" % label)
        elif identifier.casefold() in seen:
            fail("NXA0032 duplicate patch profile id: %s" % identifier)
        else:
            seen.add(identifier.casefold())
        match = profile.get("match_internal_payload")
        if not isinstance(match, dict) or not match:
            fail(
                "NXA0033 %s.match_internal_payload must identify the internal "
                "payload bytes the patch depends on" % label
            )
        else:
            for key, value in match.items():
                if key not in ("path", "sha256", "magic_hex", "magic_ascii",
                               "magic_offset"):
                    fail("NXA0033 %s.match_internal_payload.%s is unknown" % (label, key))
            path = _normalize_member_path(
                match.get("path"), "%s.match_internal_payload.path" % label,
                fail,
            )
            if path is not None and _is_signing_member(path):
                fail(
                    "NXA0004 %s.match_internal_payload.path must not select "
                    "Android signature/certificate material" % label
                )
            sha = match.get("sha256")
            if not _is_text(sha) or not _re.fullmatch(r"[0-9a-fA-F]{64}", sha or ""):
                fail(
                    "NXA0034 %s.match_internal_payload.sha256 is mandatory "
                    "and must be 64 hex; a patch profile is authenticated by "
                    "the internal payload bytes it depends on" % label
                )
            magic_hex = match.get("magic_hex")
            magic_ascii = match.get("magic_ascii")
            if magic_hex is not None and magic_ascii is not None:
                fail("NXA0034 %s may not combine magic_hex and magic_ascii" % label)
            if magic_hex is not None and (
                    not _is_text(magic_hex)
                    or not magic_hex
                    or len(magic_hex) % 2
                    or len(magic_hex) > 8192
                    or _re.fullmatch(r"[0-9a-fA-F]+", magic_hex) is None):
                fail("NXA0034 %s.match_internal_payload.magic_hex is invalid" % label)
            if magic_ascii is not None and (
                    not _is_text(magic_ascii)
                    or not magic_ascii
                    or len(magic_ascii) > 4096
                    or any(ord(character) > 127 for character in magic_ascii)):
                fail("NXA0034 %s.match_internal_payload.magic_ascii is invalid" % label)
            offset = match.get("magic_offset", 0)
            if (not _is_int(offset) or offset < 0 or offset > (1 << 40)):
                fail("NXA0034 %s.match_internal_payload.magic_offset is invalid" % label)
            if path is not None and _is_text(sha) and _re.fullmatch(
                    r"[0-9a-fA-F]{64}", sha):
                match_key = (
                    path.casefold(), sha.casefold(), magic_hex, magic_ascii,
                    offset,
                )
                if match_key in seen_matches:
                    fail("NXA0034 duplicate authenticated patch match in %s" % label)
                seen_matches.add(match_key)
        fallback = profile.get("fallback")
        if (not _is_text(fallback) or not fallback.strip()
                or len(fallback) > 256):
            fail(
                "NXA0035 %s.fallback is mandatory: a build that does not "
                "match the internal payload must follow the generic/symbolic "
                "path, never be rejected" % label
            )


def validate_patch_profile_links(compatibility, profiles, abi_order, fail):
    """NXA0036..NXA0038: selectors and profiles form one unambiguous graph."""
    if not isinstance(compatibility, dict):
        compatibility = {}
    members = normalize_required_members(
        compatibility.get("required_members"), fail, abi_order=abi_order
    )
    selectors = {
        item["member"].casefold(): item["member"]
        for item in members if item.get("role") == "patch_selector"
    }
    by_selector = {}
    if isinstance(profiles, list):
        for profile in profiles:
            if not isinstance(profile, dict):
                continue
            match = profile.get("match_internal_payload")
            if not isinstance(match, dict):
                continue
            path = match.get("path")
            if not _is_text(path):
                continue
            key = path.casefold()
            by_selector.setdefault(key, []).append(profile)
            if key not in selectors:
                fail(
                    "NXA0036 patch profile %r path %r has no matching "
                    "compatibility.required_members patch_selector"
                    % (profile.get("id"), path)
                )
    for key, member in selectors.items():
        linked = by_selector.get(key, [])
        if not linked:
            fail(
                "NXA0036 patch_selector %r must have at least one authenticated "
                "patch_profiles entry" % member
            )
            continue
        fallbacks = {
            profile.get("fallback") for profile in linked
            if _is_text(profile.get("fallback"))
        }
        if len(fallbacks) != 1:
            fail(
                "NXA0037 patch_selector %r profiles must declare one common "
                "generic/symbolic fallback" % member
            )


def validate_recipe_apk_compat(recipe, fail):
    """Entry point over a parsed recipe dict.

    Applies the container-identity ban to every extract rule of kind
    `container` and validates the optional V3 blocks. Purely additive:
    recipes without the V3 blocks stay valid as long as no container rule
    carries identity predicates.
    """
    if not isinstance(recipe, dict):
        fail("NXA0000 recipe must be an object")
        return
    rules = recipe.get("extract") or []
    container_destinations = {}
    if isinstance(rules, list):
        for rule in rules:
            if not isinstance(rule, dict):
                continue
            source = rule.get("source")
            if isinstance(source, dict) and source.get("kind") == "container":
                destination = rule.get("destination")
                if _is_text(destination):
                    if "{basename}" in destination:
                        fail(
                            "NXA0005 extract %s: a container destination may "
                            "not inherit the external basename; use one fixed "
                            "package-relative path" % rule.get("id", "?")
                        )
                    else:
                        container_destinations[destination.casefold()] = rule.get(
                            "id", "?"
                        )
                validate_container_source_identity(
                    rule.get("id", "?"), source, fail
                )
                validate_container_rule_identity(
                    rule.get("id", "?"), rule.get("validate"), fail,
                    "validate",
                )
                for phase_field in ("source_validate", "output_validate"):
                    if phase_field not in rule:
                        continue
                    validate_container_rule_identity(
                        rule.get("id", "?"), rule.get(phase_field), fail,
                        phase_field,
                    )
            validate_signing_member_rule(
                rule.get("id", "?"), source, fail,
            )

    def _validate_container_output_checks(checks, field):
        if not isinstance(checks, list):
            return
        for check in checks:
            if not isinstance(check, dict) or not _is_text(check.get("path")):
                continue
            rule_id = container_destinations.get(check["path"].casefold())
            if rule_id is not None:
                validate_container_rule_identity(rule_id, check, fail, field)

    _validate_container_output_checks(recipe.get("validate"), "validate output")
    hooks = recipe.get("hooks")
    if isinstance(hooks, list):
        for hook in hooks:
            if isinstance(hook, dict):
                _validate_container_output_checks(
                    hook.get("checkpoint"),
                    "hook %s checkpoint" % hook.get("id", "?"),
                )
    input_config = recipe.get("input")
    packages = None
    if isinstance(input_config, dict):
        value = input_config.get("packages")
        if _is_string_list(value):
            packages = value
    abi_order = recipe.get("abi_order")
    if not _is_string_list(abi_order):
        abi_order = None
    profiles = recipe.get("patch_profiles")
    validate_reference_build(recipe.get("reference_build"), fail)
    validate_compatibility(
        recipe.get("compatibility"), fail,
        input_packages=packages, abi_order=abi_order,
    )
    validate_patch_profiles(profiles, fail)
    validate_patch_profile_links(
        recipe.get("compatibility"), profiles, abi_order, fail
    )


def validate_hook_contract(document, hook_ids, fail):
    """NXA0040..NXA0047: hook-contract.json declared by custom hooks."""
    if not isinstance(document, dict):
        fail("NXA0040 hook contract must be a JSON object")
        return
    if document.get("schema") != HOOK_CONTRACT_SCHEMA:
        fail("NXA0041 hook contract schema must be %s" % HOOK_CONTRACT_SCHEMA)
    if document.get("schema_version") != HOOK_CONTRACT_SCHEMA_VERSION:
        fail(
            "NXA0041 hook contract schema_version must be %d"
            % HOOK_CONTRACT_SCHEMA_VERSION
        )
    for key in document:
        if key not in HOOK_CONTRACT_KEYS:
            fail("NXA0042 hook contract key %s is unknown" % key)
    hook_id = document.get("hook_id")
    if not _is_text(hook_id) or (hook_ids is not None and hook_id not in hook_ids):
        fail("NXA0043 hook contract hook_id must name a declared recipe hook")
    inputs = document.get("inputs")
    if not _is_string_list(inputs) or not inputs:
        fail("NXA0044 hook contract inputs must list the payload paths it reads")
    predicates = document.get("predicates")
    if not isinstance(predicates, list) or not predicates:
        fail("NXA0045 hook contract must declare at least one predicate")
        predicates = []
    for index, predicate in enumerate(predicates):
        label = "predicates[%d]" % index
        if not isinstance(predicate, dict):
            fail("NXA0045 %s must be an object" % label)
            continue
        klass = predicate.get("class")
        if klass not in PREDICATE_CLASSES:
            fail(
                "NXA0046 %s.class must be one of %s"
                % (label, ", ".join(PREDICATE_CLASSES))
            )
        if klass == "reference_identity":
            fail(
                "NXA0046 %s: reference_identity may be reported, never "
                "verified by a hook as an acceptance predicate" % label
            )
        description = predicate.get("checks")
        if not _is_text(description) or not description.strip():
            fail("NXA0046 %s.checks must describe the technical property" % label)
    validate_patch_profiles(document.get("patch_profiles"), fail)
    fallback = document.get("fallback")
    if not _is_text(fallback) or not fallback.strip():
        fail(
            "NXA0047 hook contract fallback is mandatory: unknown compatible "
            "builds follow the generic path"
        )
    codes = document.get("error_codes")
    if not _is_string_list(codes) or not codes:
        fail("NXA0047 hook contract error_codes must list stable codes")


def scan_static_suspects(text, label, allowed_hex64=(), allowed_versions=(),
                         allowed_sha1=()):
    """NXA0050..NXA0055: static defence over hook/validator source text.

    Returns a list of finding strings; the caller decides whether findings
    are fatal. Flags: equality against 64-hex literals, literal dotted
    version tokens, byte-size tables, absolute offsets without fallback, and
    40-hex SHA-1 certificate fingerprints. Exceptions must be explicit, narrow
    and justified by the caller through allowed_hex64/allowed_versions/
    allowed_sha1.
    """
    findings = []
    if not _is_text(text):
        return findings
    allowed_hex = {item.casefold() for item in allowed_hex64}
    for match in _HEX64_RE.finditer(text):
        if match.group(0).casefold() in allowed_hex:
            continue
        findings.append(
            "NXA0050 %s: 64-hex literal used by custom logic (%s...); "
            "container identity must not gate compatibility and internal "
            "hashes belong in patch_profiles with fallback"
            % (label, match.group(0)[:12])
        )
    allowed_sha1_hex = {item.casefold() for item in allowed_sha1}
    for match in _HEX40_RE.finditer(text):
        token = match.group(0)
        if token.casefold() in allowed_sha1_hex:
            continue
        # A 64-hex literal contains 40-hex substrings; do not double-flag one.
        if _HEX64_RE.search(text[max(0, match.start() - 24):match.end() + 24]):
            continue
        findings.append(
            "NXA0054 %s: 40-hex literal used by custom logic (%s...); a SHA-1 "
            "signing-certificate fingerprint is identity, not compatibility, "
            "and must never gate acceptance"
            % (label, token[:12])
        )
    signing_member = _SIGNING_MEMBER_TEXT_RE.search(text)
    if signing_member:
        findings.append(
            "NXA0055 %s: Android signature/certificate member referenced by "
            "custom logic (%s); signing identity may be documented but never "
            "gate compatibility" % (label, signing_member.group(0)[:80])
        )
    allowed_version_tokens = set(allowed_versions)
    for match in _DOTTED_VERSION_RE.finditer(text):
        token = match.group(0)
        if token in allowed_version_tokens:
            continue
        findings.append(
            "NXA0051 %s: literal dotted version token %r; version text "
            "inside assets is identity, not compatibility (the Terraria "
            "dot-4 vs dot-49 regression)" % (label, token)
        )
    for pattern, code, description in (
        (r"\bexpected_sizes?\s*[=:]\s*[\[{(]", "NXA0052",
         "table of exact sizes"),
        (r"\b(?:seek|offset)\s*[(=:]\s*0x[0-9a-fA-F]{4,}", "NXA0053",
         "absolute offset without a declared patch profile"),
    ):
        if _re.search(pattern, text):
            findings.append(
                "%s %s: %s used by custom logic; exact numbers of the "
                "reference copy require a patch profile with fallback"
                % (code, label, description)
            )
    return findings
