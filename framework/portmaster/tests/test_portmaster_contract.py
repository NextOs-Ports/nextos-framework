#!/usr/bin/env python3
"""Static, process-free regression gate for the PortMaster contract."""

import ast
import json
import hashlib
import re
import sys
from pathlib import Path, PurePosixPath


PORTMASTER_ROOT = Path(__file__).resolve().parents[1]
FRAMEWORK_ROOT = PORTMASTER_ROOT.parent
BOOTSTRAP_ROOT = FRAMEWORK_ROOT / "nxbootstrap"
SOURCES_PATH = PORTMASTER_ROOT / "upstream-sources-v1.json"
CONTRACT_PATH = PORTMASTER_ROOT / "contract-v1.json"
FIXTURES_PATH = PORTMASTER_ROOT / "fixtures" / "contract-cases-v1.json"
CONTRACT_V2_PATH = PORTMASTER_ROOT / "contract-v2.json"
CONTRACT_V3_PATH = PORTMASTER_ROOT / "contract-v3.json"
VENDOR_PROVENANCE_PATH = (
    PORTMASTER_ROOT / "vendor/PortMaster-GUI-8f9ddc4.json"
)
VENDOR_MANIFEST_PATH = (
    PORTMASTER_ROOT / "vendor/PortMaster-GUI-8f9ddc4.sha256"
)
CHARSET_LICENSE_PATH = (
    PORTMASTER_ROOT / "vendor/LICENSE-charset-normalizer-3.0.1.txt"
)
VENDOR_ROOT = PORTMASTER_ROOT / "vendor/PortMaster-GUI-8f9ddc4"
METADATA_SCHEMA_V1_PATH = (
    PORTMASTER_ROOT / "schema/port-json-supported-v1.schema.json"
)
METADATA_SCHEMA_V2_PATH = (
    PORTMASTER_ROOT / "schema/port-json-supported-v2.schema.json"
)
RUNTIME_NEGATIVE_PATH = (
    PORTMASTER_ROOT / "fixtures/harbourmaster/runtime-missing-v1.json"
)
LEGACY_RUNTIME_PATH = (
    PORTMASTER_ROOT / "fixtures/harbourmaster/runtime-legacy-compat-v1.json"
)
REAL_CYCLE_TOOL = PORTMASTER_ROOT / "tools/harbourmaster-cycle.py"


def fail(message):
    raise AssertionError(message)


def require(condition, message):
    if not condition:
        fail(message)


def load_json(path):
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_v2_contract(sources):
    contract = load_json(CONTRACT_V2_PATH)
    provenance = load_json(VENDOR_PROVENANCE_PATH)
    schema = load_json(METADATA_SCHEMA_V1_PATH)
    negative = load_json(RUNTIME_NEGATIVE_PATH)
    require(contract.get("schema_version") == 2 and
            contract.get("extends") == "contract-v1.json",
            "PortMaster v2 contract does not extend v1 additively")
    require(contract.get("upstream_source") ==
            "portmaster-gui-main-8f9ddc4",
            "PortMaster v2 contract lost its exact upstream source")
    require(contract.get("required_cycles") == [
                "strict-json-and-schema",
                "port_info_load-do_default",
                "HarbourMaster-load_ports",
                "install",
                "discovery",
                "update",
                "uninstall",
                "reinstall",
            ], "PortMaster real lifecycle is incomplete")
    boundaries = contract.get("boundaries", {})
    require(boundaries.get("network") == "blocked" and
            boundaries.get("guest_execution") is False and
            boundaries.get("launcher_execution") is False and
            boundaries.get("archive_mutation") is False and
            boundaries.get("update_stale_members") == "fatal",
            "PortMaster real lifecycle boundary was weakened")
    required_negatives = set(contract.get("required_negative_fixtures", []))
    require(required_negatives == {
                "missing-runtime", "wrong-runtime-type",
                "duplicate-json-key", "truncated-json",
                "case-mismatched-item", "missing-item", "path-traversal",
                "stale-file-after-update",
            }, "PortMaster negative lifecycle matrix changed")

    upstream = next(item for item in sources["sources"]
                    if item["id"] == "portmaster-gui-main-8f9ddc4")
    require(provenance.get("commit") == upstream.get("commit"),
            "vendored HarbourMaster commit disagrees with source manifest")
    require(sha256(VENDOR_MANIFEST_PATH) ==
            provenance.get("file_manifest_sha256"),
            "vendored HarbourMaster file manifest changed")
    charset_license = provenance.get("additional_licenses", [])
    require(charset_license == [{
                "component": "charset-normalizer",
                "version": "3.0.1",
                "license": "MIT",
                "path": "LICENSE-charset-normalizer-3.0.1.txt",
                "source": "https://github.com/jawah/charset_normalizer/blob/3.0.1/LICENSE",
            }], "charset-normalizer license provenance changed")
    require(CHARSET_LICENSE_PATH.read_text(encoding="utf-8").startswith(
                "MIT License\n\nCopyright (c) 2019 TAHRI Ahmed R.\n"),
            "charset-normalizer 3.0.1 license text changed")
    manifest = {}
    for line in VENDOR_MANIFEST_PATH.read_text(encoding="utf-8").splitlines():
        digest, relative = line.split("  ./", 1)
        require(relative not in manifest,
                "duplicate HarbourMaster vendor manifest path")
        manifest[relative] = digest
    require(len(manifest) >= 160,
            "HarbourMaster vendor snapshot is unexpectedly incomplete")
    pinned_files = {item["path"]: item["sha256"]
                    for item in upstream["files"]}
    for relative in (
            "PortMaster/pylibs/harbourmaster/config.py",
            "PortMaster/pylibs/harbourmaster/info.py",
            "PortMaster/pylibs/harbourmaster/captain.py",
            "PortMaster/pylibs/harbourmaster/harbour.py",
            "PortMaster/pylibs/harbourmaster/platform.py"):
        require(manifest.get(relative) == pinned_files.get(relative),
                "vendored official parser hash changed: %s" % relative)
        require(sha256(VENDOR_ROOT / relative) == manifest[relative],
                "vendored official parser bytes changed: %s" % relative)
    require("__builtins__.PORTMASTER_DEBUG = False" in
            (VENDOR_ROOT / "PortMaster/harbourmaster").read_text(
                encoding="utf-8"),
            "vendor lost the official HarbourMaster bootstrap global")

    required = schema.get("required", [])
    attr_required = schema["properties"]["attr"].get("required", [])
    require("attr" in required and "runtime" in attr_required,
            "supported port.json schema no longer requires attr.runtime")
    require("runtime" not in negative.get("attr", {}),
            "missing-runtime fixture was accidentally repaired")
    tool = REAL_CYCLE_TOOL.read_text(encoding="utf-8")
    for token in (
            "verify_vendor_snapshot()", "block_network()",
            "builtins.PORTMASTER_DEBUG = False", "port_info_load",
            "HarbourMaster(", "hm.load_ports()", "hm.install_port",
            "hm.uninstall_port", "guest_execution=0 network=0"):
        require(token in tool,
                "real HarbourMaster cycle gate lacks %s" % token)


def check_v3_contract(sources):
    contract = load_json(CONTRACT_V3_PATH)
    schema = load_json(METADATA_SCHEMA_V2_PATH)
    legacy = load_json(LEGACY_RUNTIME_PATH)
    require(contract.get("schema_version") == 3 and
            contract.get("extends") == "contract-v2.json",
            "PortMaster v3 contract does not extend v2 additively")
    require(contract.get("metadata_schema") ==
            "schema/port-json-supported-v2.schema.json",
            "PortMaster v3 contract does not pin metadata schema v2")
    require(contract.get("legacy_upstream_source") == legacy.get("source_id"),
            "PortMaster v3 contract lost its legacy source")
    runtime_contract = contract.get("runtime_compatibility", {})
    require(runtime_contract.get("empty_project_runtime") ==
            "omit attr.runtime from version 4 metadata" and
            runtime_contract.get("modern_parser_default") == [] and
            runtime_contract.get("legacy_parser_default") is None,
            "PortMaster v3 empty-runtime rule changed")

    attr_required = schema["properties"]["attr"].get("required", [])
    attr_properties = schema["properties"]["attr"].get("properties", {})
    require(attr_required == ["title", "arch", "min_glibc"] and
            "runtime" in attr_properties,
            "PortMaster schema v2 does not make only attr.runtime optional")

    source = next(item for item in sources["sources"]
                  if item["id"] == legacy["source_id"])
    require(source.get("tag") == legacy.get("tag") and
            source.get("commit") == legacy.get("commit"),
            "legacy HarbourMaster tag/commit provenance changed")
    source_hashes = {item["path"]: item["sha256"]
                     for item in source.get("files", [])}
    require(source_hashes == legacy.get("source_hashes"),
            "legacy HarbourMaster source hashes changed")
    observed = legacy.get("observed_contract", {})
    require(observed.get("missing_runtime_default") is None and
            observed.get("install_condition") == "attr.runtime is not None" and
            observed.get("runtime_nicename_requires") == "string" and
            observed.get("missing_runtime_result") ==
                "skip-runtime-and-succeed" and
            observed.get("empty_array_result") ==
                "AttributeError:list-has-no-attribute-startswith",
            "legacy HarbourMaster runtime observation changed")

    def legacy_install_probe(attributes):
        runtime = attributes.get("runtime", None)
        if runtime is None:
            return "skip-runtime-and-succeed"
        runtime.startswith("frt")
        return "runtime-path"

    require(legacy_install_probe({}) == "skip-runtime-and-succeed",
            "legacy parser model no longer accepts omitted runtime")
    try:
        legacy_install_probe({"runtime": []})
    except AttributeError:
        pass
    else:
        fail("legacy parser model no longer reproduces empty-array failure")

    tool = REAL_CYCLE_TOOL.read_text(encoding="utf-8")
    require("port-json-supported-v2.schema.json" in tool,
            "real HarbourMaster cycle does not use metadata schema v2")


def shell_function(source, name):
    match = re.search(r"^%s\(\) \{\n" % re.escape(name), source, re.MULTILINE)
    require(match is not None, "missing shell function: %s" % name)
    next_match = re.search(r"^[a-z][a-z0-9_]*\(\) \{\n", source[match.end():],
                           re.MULTILINE)
    end = match.end() + next_match.start() if next_match else len(source)
    return source[match.start():end]


def assert_order(text, markers, label):
    position = -1
    for marker in markers:
        found = text.find(marker, position + 1)
        require(found >= 0, "%s is missing marker: %s" % (label, marker))
        require(found > position, "%s has an invalid order near: %s" %
                (label, marker))
        position = found


INLINE_GAME_ROUTES = (
    re.compile(
        r'(?:LD_PRELOAD="\$BIN_PRELOAD\$\{LD_PRELOAD:\+:\$LD_PRELOAD\}"\s+)?'
        r'\$NXBOOTSTRAP_INTERP_PREFIX "\$BIN"@RUN_ARGS@ 9>&- &$'),
    re.compile(
        r'(?:LD_PRELOAD="\$BIN_PRELOAD\$\{LD_PRELOAD:\+:\$LD_PRELOAD\}"\s+)?'
        r'"\$NXBOOTSTRAP_GAME_LOADER" --library-path '
        r'"\$NXBOOTSTRAP_GAME_LIBS"\s+"\$BIN"@RUN_ARGS@ 9>&- &$'),
    re.compile(
        r'(?:LD_PRELOAD="\$BIN_PRELOAD\$\{LD_PRELOAD:\+:\$LD_PRELOAD\}"\s+)?'
        r'"\$BIN"@RUN_ARGS@ 9>&- &$'),
)
SUBSHELL_EXEC_ROUTES = (
    re.compile(r'exec \$NXBOOTSTRAP_INTERP_PREFIX "\$BIN"@RUN_ARGS@$'),
    re.compile(
        r'exec "\$NXBOOTSTRAP_GAME_LOADER" --library-path '
        r'"\$NXBOOTSTRAP_GAME_LIBS"\s+"\$BIN"@RUN_ARGS@$'),
    re.compile(r'exec "\$BIN"@RUN_ARGS@$'),
)


def matches_route(line, patterns):
    return any(pattern.fullmatch(line) is not None for pattern in patterns)


def supervised_game_launch_template(template):
    """Accept only the two canonical direct-child supervision shapes."""
    logical = re.sub(r"\\\n\s*", " ", template)
    lines = [line.strip() for line in logical.splitlines() if line.strip()]
    route_lines = [line for line in lines if "@RUN_ARGS@" in line]
    routes = template.count("@RUN_ARGS@")
    if routes == 0 or len(route_lines) != routes or any(
            line.count("@RUN_ARGS@") != 1 for line in route_lines):
        return False

    if logical.startswith("(\n"):
        if not logical.endswith("\n) 9>&- &"):
            return False
        exec_lines = [line for line in lines if re.match(r"^exec\b", line)]
        # Every exec is a known game route and every game route is that exec.
        # This rejects an arbitrary exec hidden beside an otherwise valid one.
        return (len(exec_lines) == routes and
                exec_lines == route_lines and
                all(matches_route(line, SUBSHELL_EXEC_ROUTES)
                    for line in exec_lines))

    if any(re.match(r"^exec\b", line) for line in lines):
        return False
    return all(matches_route(line, INLINE_GAME_ROUTES)
               for line in route_lines)


def walk_key_values(value):
    if isinstance(value, dict):
        for key, child in value.items():
            yield key, child
            yield from walk_key_values(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk_key_values(child)


def check_source_manifest(sources):
    require(sources.get("schema_version") == 1,
            "source manifest schema must be version 1")
    source_entries = sources.get("sources")
    history = sources.get("funcs_history")
    require(isinstance(source_entries, list) and source_entries,
            "source manifest has no sources")
    require(isinstance(history, list) and history,
            "source manifest has no funcs history")

    all_entries = source_entries + history
    ids = [entry.get("id") for entry in all_entries]
    require(all(isinstance(item, str) and item for item in ids),
            "every source and funcs snapshot needs an id")
    require(len(ids) == len(set(ids)), "source ids are not unique")

    sha_pattern = re.compile(r"^[0-9a-f]{64}$")
    commit_pattern = re.compile(r"^[0-9a-f]{40}$")
    for key, value in walk_key_values(sources):
        if key.endswith("sha256"):
            require(isinstance(value, str) and sha_pattern.fullmatch(value),
                    "invalid SHA-256 in source manifest: %r" % value)
        if key == "commit":
            require(isinstance(value, str) and commit_pattern.fullmatch(value),
                    "invalid full commit id in source manifest: %r" % value)

    versions = [entry.get("version") for entry in history]
    require(versions == [1, 2, 3],
            "official funcs history must pin versions 1, 2 and 3 in order")

    for entry in source_entries:
        if entry.get("kind") == "local-firmware-integration":
            require(entry.get("universal_evidence") is False,
                    "local firmware integration was marked universal")

    negatives = sources.get("negative_evidence", [])
    require(negatives, "negative evidence list is empty")
    for entry in negatives:
        require(entry.get("scope") == "negative-only",
                "negative evidence lost its negative-only scope")
        require(entry.get("reuse_forbidden") is True,
                "negative global hook is no longer reuse-forbidden")

    policy = sources.get("reference_policy", {})
    require(policy.get("game_ports_used_as_m03_fixtures") == [],
            "M03 unexpectedly uses a game port as a fixture")
    require(policy.get("wip_sources_allowed") is False,
            "WIP sources became allowed")
    expected_limited = {
        "Pikmin", "PartyBoard", "GTA ports", "Bully", "Dysmantle",
        "LIMBO", "Chrono Trigger",
    }
    require(set(policy.get("limited_references_never_universal", [])) ==
            expected_limited,
            "limited game references changed or were generalized")
    return set(ids)


def check_contract(contract, sources):
    require(contract.get("schema_version") == 1,
            "PortMaster contract schema must be version 1")
    require(contract.get("source_manifest") == SOURCES_PATH.name,
            "contract does not point at its pinned source manifest")
    require(contract.get("principle") ==
            "detect first, correct second, never force by default",
            "the capability-first principle changed")

    funcs_contract = contract["api"]["funcs_txt"]["versions"]
    require([entry["version"] for entry in funcs_contract] == [1, 2, 3],
            "contract does not cover funcs versions 1, 2 and 3")
    history_by_version = {
        item["version"]: item for item in sources["funcs_history"]
    }
    require(set(history_by_version) == {1, 2, 3},
            "contract funcs versions have no matching source snapshots")

    control_api = contract["api"]["control_txt"]
    published = control_api.get("may_publish", [])
    require("ANALOGSTICKS" in published and "ANALOG_STICKS" in published,
            "PortMaster stick-count variables are not contracted")
    stick_hint = contract["api"]["get_controls"].get(
        "integrated_stick_hint", {})
    require(stick_hint.get("published_names") ==
            ["ANALOGSTICKS", "ANALOG_STICKS"] and
            stick_hint.get("accepted_values") == [0, 1, 2] and
            stick_hint.get("exported_name") ==
            "NXINPUT_ANALOG_STICKS_HINT",
            "PortMaster integrated-stick hint contract changed")
    hint_policy = " ".join((stick_hint.get("scope", ""),
                            stick_hint.get("invalid_or_absent_policy", ""),
                            stick_hint.get("activation_policy", ""))).lower()
    for token in ("host-wide", "never per-pad", "unset", "never create",
                  "automatically"):
        require(token in hint_policy,
                "PortMaster stick hint lost fail-closed policy: %s" % token)

    package = contract["package_installation"]
    require(package.get("current_port_json_version") == 4,
            "current port.json version is not pinned to 4")
    require(package.get("accepted_legacy_versions") == [1, 2, 3],
            "legacy port.json upgrade set changed")
    require("trailing slash" in package.get("items_semantics", ""),
            "directory ownership no longer requires a trailing slash")
    require("top-level .sh" in package.get("split_root_semantics", ""),
            "HarbourMaster split-root install rule is missing")
    overlay = package.get("overlay_update_semantics", "")
    require("self-contained" in overlay and
            "generator version" in overlay,
            "overlay-safe launcher deployment rule is missing")
    security = " ".join(package.get("security", [])).lower()
    require("absolute" in security and "parent traversal" in security,
            "archive traversal protections are missing")

    filesystem = contract.get("filesystem_portability", {})
    filesystem_text = json.dumps(filesystem, ensure_ascii=False).lower()
    for fact in ("fat", "exfat", "executable", "symlink", "flock",
                 "bind_files", "capability"):
        require(fact in filesystem_text,
                "filesystem portability contract is missing %s" % fact)

    platform_observations = contract.get("platform_observations", [])
    require(platform_observations, "platform observations are empty")
    require(all(item.get("universal_evidence") is False
                for item in platform_observations),
            "a platform observation was promoted to universal evidence")
    discovery_text = json.dumps({
        "roots": contract["portmaster_discovery"],
        "observations": platform_observations,
    }, ensure_ascii=False)
    for token in ("MIYOO_EX", "RetroDECK", "unsupported/unverified",
                  "no fixture", "no fixture/runtime"):
        require(token in discovery_text,
                "discovery-only support boundary lacks %s" % token)

    generalizations = " ".join(contract.get("prohibited_generalizations", []))
    for token in ("Pikmin", "PartyBoard", "GTA", "Mali-450", "Panfrost",
                  "WIP"):
        require(token in generalizations,
                "missing prohibited generalization for %s" % token)

    frontend = " ".join(
        contract["frontend_and_signals"]["forbidden_framework_actions"]
    ).lower()
    for action in ("service", "desktop", "log out", "suspend", "reboot",
                   "power off", "setsid"):
        require(action in frontend,
                "frontend/session safety contract is missing %s" % action)


def check_fixtures(fixtures, valid_source_ids, contract):
    require(fixtures.get("schema_version") == 1,
            "fixture schema must be version 1")
    require(fixtures.get("contract") == CONTRACT_PATH.name,
            "fixtures point at the wrong contract")
    cases = fixtures.get("cases")
    require(isinstance(cases, list) and cases,
            "PortMaster fixtures are empty")
    ids = [case.get("id") for case in cases]
    require(len(ids) == len(set(ids)) and all(ids),
            "fixture ids are missing or duplicated")

    fixture_text = json.dumps(fixtures, ensure_ascii=False).lower()
    for forbidden_game in ("pikmin", "partyboard", "gta", "bully",
                           "dysmantle", "limbo", "chrono"):
        require(forbidden_game not in fixture_text,
                "game reference leaked into an M03 fixture: %s" %
                forbidden_game)
    require("wip" not in fixture_text,
            "a WIP source leaked into an M03 fixture")

    allowed_scopes = {
        "official-api-contract",
        "official-platform-integration",
        "local-firmware-integration",
        "negative-only",
    }
    for case in cases:
        require(case.get("universal_evidence") is False,
                "fixture %s was marked universal" % case.get("id"))
        require(case.get("evidence_scope") in allowed_scopes,
                "fixture %s has an unknown evidence scope" % case.get("id"))
        source_ids = case.get("source_ids", [])
        require(source_ids and set(source_ids) <= valid_source_ids,
                "fixture %s has an unpinned source" % case.get("id"))
        for path_text in case.get("source_paths", []):
            path = PurePosixPath(path_text)
            require(not path.is_absolute() and ".." not in path.parts,
                    "fixture source path escapes its source: %s" % path_text)
            require(not path.parts or path.parts[0].lower() != "ports",
                    "game port path used as M03 evidence: %s" % path_text)
        if case.get("evidence_scope") == "negative-only":
            require(case.get("expected", {}).get("reuse_forbidden") is True,
                    "negative fixture is not explicitly reuse-forbidden")

    funcs_by_version = {
        item["version"]: item for item in contract["api"]["funcs_txt"]["versions"]
    }
    funcs_cases = [case for case in cases
                   if case["id"].startswith("official-funcs-v")]
    require(len(funcs_cases) == 3, "fixtures do not cover all three funcs APIs")
    for case in funcs_cases:
        expected = case["expected"]
        version = expected["pm_funcs_version"]
        api = funcs_by_version[version]
        require(expected["required_functions"] == api["baseline_functions"],
                "funcs v%s fixture disagrees with the contract" % version)
        require(expected["optional_or_missing_functions"] ==
                api["not_guaranteed"],
                "funcs v%s optional capability list disagrees" % version)

    required_cases = {
        "official-generic-arkos-family",
        "official-rocknix-helper",
        "official-muos-split-root",
        "official-knulli-exfat",
        "official-harbourmaster-install",
        "nextos-portmaster-control-bridge",
        "nextos-global-rewrite-negative",
    }
    require(required_cases <= set(ids),
            "one or more required platform/installer fixtures are missing")


def strip_shell_comments(text):
    return "\n".join(line for line in text.splitlines()
                     if not line.lstrip().startswith("#"))


def check_implementation(contract):
    bootstrap_path = BOOTSTRAP_ROOT / "nxbootstrap.sh"
    source = bootstrap_path.read_text(encoding="utf-8")
    launcher = (BOOTSTRAP_ROOT / "templates" / "launcher.sh.in").read_text(
        encoding="utf-8")
    generator = (BOOTSTRAP_ROOT / "tools" / "generate-port.py").read_text(
        encoding="utf-8")

    main = shell_function(source, "nxbootstrap_main")
    assert_order(main, [
        "export PORT_32BIT=Y",
        "nxbootstrap_load_portmaster",
        "nxbootstrap_check_arch",
        "nxbootstrap_build_host_environment",
        "nxbootstrap_platform_prepare",
        "nxbootstrap_run_extractor",
        "nxbootstrap_run_prepare",
        "nxbootstrap_check_required_files",
        "nxbootstrap_build_runtime_environment",
        "nxbootstrap_launch",
    ], "nxbootstrap_main")

    loader = shell_function(source, "nxbootstrap_load_portmaster")
    assert_order(loader, [
        'source "$candidate/control.txt"',
        "controlfolder=$candidate",
        'mod_file=$candidate/mod_${CFW_NAME}.txt',
        'source "$mod_file"',
        "declare -F get_controls",
        "get_controls",
        "nxbootstrap_install_traps",
        "export NXCOMPAT_PORTMASTER_DIR=$candidate",
    ], "nxbootstrap_load_portmaster")
    require("! -L $controlfolder/control.txt" in loader and
            "! -L $candidate_root/control.txt" in loader,
            "one of the control.txt discovery routes follows symlinks")
    require(loader.count(
            'nxbootstrap_canonical_directory "$controlfolder"') >= 2,
            "ambient/published controlfolder is not physically canonicalized")
    require("! -L $mod_file" in loader,
            "platform mod discovery follows symlinks")
    require("*[!A-Za-z0-9._-]*" in loader,
            "CFW_NAME is no longer sanitized")
    require("PortMaster loaded (root=$candidate " in loader,
            "selected PortMaster root is not recorded in the runtime log")

    for root in contract["portmaster_discovery"]["known_roots"]:
        require(root in source,
                "nxbootstrap discovery is missing known root %s" % root)

    helper = shell_function(source, "nxbootstrap_platform_prepare")
    require("NXBOOTSTRAP_PLATFORM_PREPARED == 0" in helper,
            "platform helper lacks its exactly-once guard")
    require('pm_platform_helper "$NXBOOTSTRAP_BIN"' in helper,
            "platform helper does not receive the real executable")
    require("returned status $status; continuing" in helper,
            "platform helper failure is not logged as best effort")
    for marker in (
            "PM_PIPE is not a live non-symlink FIFO",
            "close API unavailable while PM_PIPE is active",
            "close API returned status $status",
            "PM_PIPE remained after close request"):
        require(marker in helper,
                "dialog handoff is not fail-closed for: %s" % marker)
    require("PortMaster dialog closed after platform helper" in helper,
            "successful dialog handoff is not recorded")

    finish = shell_function(source, "nxbootstrap_finish_once")
    require("NXBOOTSTRAP_FINISHED == 0" in finish and
            "NXBOOTSTRAP_FINISHED=1" in finish,
            "pm_finish lacks an exactly-once guard")
    require(source.count("if pm_finish; then") == 1,
            "pm_finish has more than one direct invocation")
    require("WARNING: pm_finish returned status" in finish,
            "pm_finish failure is not logged")

    library_builder = shell_function(source, "nxbootstrap_build_library_path")
    assert_order(library_builder, [
        "nxbootstrap_validate_private_library_dir",
        'nxbootstrap_add_library_dir "$controlfolder/libs"',
        "for directory in /usr/local",
        'IFS=: read -r -a nxbootstrap_old_dirs <<< "$old_path"',
        "nxbootstrap_add_library_dir /usr/lib",
    ], "nxbootstrap_build_library_path")
    private_validator = shell_function(
        source, "nxbootstrap_validate_private_library_dir")
    for provider in ("libEGL", "libGLES", "libGL", "libOpenGL", "libgbm",
                     "libdrm", "libmali", "libSDL", "libSDL2"):
        require(provider in private_validator,
                "private graphics-provider gate omits %s" % provider)

    require('PORT_32BIT=\\"Y\\"' in generator,
            "generator lost the literal ARMHF declaration")
    require("@PORT_32BIT_LITERAL@" in launcher,
            "visible launcher lost the ARMHF declaration slot")
    require(launcher.index("@PORT_32BIT_LITERAL@") <
            launcher.index('/opt/system/Tools/PortMaster'),
            "ARMHF declaration is not before launcher discovery")
    # nxbootstrap 0.6.27 moved the launch line out of the template and into the
    # generator, so the ABI role decides which route starts the game. The
    # supervision property is unchanged and is still proven here: every route
    # backgrounds the game as a direct child, and the launcher waits on it.
    require("@GAME_LAUNCH_BLOCK@" in launcher,
            "visible launcher lost the game-launch slot")
    assert_order(launcher, ["@GAME_LAUNCH_BLOCK@", "game_pid=$!",
                            'wait "$game_pid"'],
                 "visible launcher does not supervise the game as a direct child")
    generator_ast = ast.parse(generator)
    launch_functions = [
        node for node in generator_ast.body
        if isinstance(node, ast.FunctionDef) and
        node.name == "render_game_launch_block"
    ]
    require(len(launch_functions) == 1,
            "generator lost the unique render_game_launch_block")
    launch_templates = [
        node.value.value for node in ast.walk(launch_functions[0])
        if isinstance(node, ast.Return) and
        isinstance(node.value, ast.Constant) and
        isinstance(node.value.value, str)
    ]
    routes = sum(template.count("@RUN_ARGS@")
                 for template in launch_templates)
    require(routes >= 2,
            "generator no longer renders both the single-ABI and the "
            "role-aware launch routes")
    require(launch_templates and
            all(supervised_game_launch_template(template)
                for template in launch_templates),
            "a launch route runs the game in the foreground or leaks the "
            "instance-lock fd into the game child")
    require(any(template.startswith("(\n") for template in launch_templates)
            and any(not template.startswith("(\n")
                    for template in launch_templates),
            "generator no longer covers inline and supervised-subshell routes")
    for hostile_template in (
            '(\n  exec /tmp/foreign "$BIN"@RUN_ARGS@\n) 9>&- &',
            '(\n  "$BIN"@RUN_ARGS@\n) 9>&- &',
            '(\n  exec /tmp/foreign\n  exec "$BIN"@RUN_ARGS@\n) 9>&- &',
            'exec "$BIN"@RUN_ARGS@ 9>&- &'):
        require(not supervised_game_launch_template(hostile_template),
                "supervision validator accepted an arbitrary/unsupervised exec")
    require(not re.search(
                r"nxbootstrap(?:-[0-9]+(?:[.][0-9]+)*)?[.]sh", launcher),
            "visible launcher still references the retired bootstrap library")
    for marker in ('NXBOOTSTRAP_LOCK_FILE="$NXBOOTSTRAP_LOCK_DIR/nxport-',
                   'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"',
                   'command ls -Lldn /proc/self/fd/9',
                   '"$NXBOOTSTRAP_LOCK_FILE" -ef /proc/self/fd/9',
                   'NXBOOTSTRAP_ANALOG_STICKS_HINT',
                   'NXINPUT_ANALOG_STICKS_HINT',
                   'NXBOOTSTRAP_FINISHED=1',
                   "nxbootstrap_abort_before_game 129",
                   "nxbootstrap_abort_before_game 130",
                   "nxbootstrap_abort_before_game 143",
                   "NXBOOTSTRAP_CHILD_STARTTIME=${20}",
                   'builtin kill -TERM "$game_pid"',
                   "NXBOOTSTRAP_SHUTDOWN_TICKS=10",
                   'builtin kill -KILL "$game_pid"',
                   "trap '' INT TERM HUP",
                   "printf '\\033c'"):
        require(marker in launcher,
                "visible launcher lacks golden-port guarantee %s" % marker)
    require('${ANALOGSTICKS:-${ANALOG_STICKS:-}}' in launcher and
            '0|1|2)' in launcher and
            'readonly NXBOOTSTRAP_ANALOG_STICKS_HINT' in launcher and
            launcher.count('export NXINPUT_ANALOG_STICKS_HINT=') >= 2 and
            launcher.count('unset NXINPUT_ANALOG_STICKS_HINT') >= 2,
            "visible launcher does not sanitize and reassert the stick hint")
    require(launcher.rfind('export NXINPUT_ANALOG_STICKS_HINT=') >
            launcher.index('@PORT_LANGUAGE_REASSERT_BLOCK@'),
            "port-env can override the sanitized stick hint")
    require(launcher.index("flock -n 9") <
            launcher.index("@NXEXTRACT_BLOCK@"),
            "instance lock must precede the extraction phase")
    require("run.sh" not in launcher and 'port_dir / "run.sh"' not in generator,
            "generator retained the forbidden public run.sh layer")

    production_text = "\n".join((source, launcher, generator))
    production_commands = strip_shell_comments(production_text)
    forced_backend = re.compile(
        r"(?:export\s+)?(?:SDL_VIDEODRIVER|SDL_AUDIODRIVER|"
        r"SDL_VIDEO_GL_DRIVER|SDL_VIDEO_EGL_DRIVER)\s*=")
    require(not forced_backend.search(production_commands),
            "production bootstrap forces an SDL/video provider")
    forbidden_command = re.compile(
        r"(?<![A-Za-z0-9_-])(?:systemctl|loginctl|qdbus|dbus-send|"
        r"shutdown|reboot|poweroff|setsid|pgrep|pkill|killall)"
        r"(?![A-Za-z0-9_-])")
    match = forbidden_command.search(production_commands)
    require(match is None,
            "production bootstrap contains forbidden host command %s" %
            (match.group(0) if match else "unknown"))
    require("builtin kill -\"$signal\" \"$pid\"" in source,
            "exact-child signal helper is missing")
    require("nxbootstrap_matching_processes" not in source and
            "nxbootstrap_sweep" not in source,
            "host-wide process sweeping returned")


def main():
    sources = load_json(SOURCES_PATH)
    contract = load_json(CONTRACT_PATH)
    fixtures = load_json(FIXTURES_PATH)
    valid_source_ids = check_source_manifest(sources)
    check_contract(contract, sources)
    check_v2_contract(sources)
    check_v3_contract(sources)
    check_fixtures(fixtures, valid_source_ids, contract)
    check_implementation(contract)
    print("PortMaster contract v3 gate passed: %d sources, %d fixtures, funcs v1-v3" %
          (len(valid_source_ids), len(fixtures["cases"])))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, KeyError, TypeError, ValueError) as error:
        print("PortMaster contract gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
