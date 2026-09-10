#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Filesystem-only regression gate for the deterministic M19 generator."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
from xml.etree import ElementTree


REPOSITORY = Path(__file__).resolve().parents[3]
ROOT = REPOSITORY / "framework" / "nxgenerator"
TOOL = ROOT / "nxgenerator.py"
EXPECTED_GENERATOR_VERSION = (
    (ROOT / "VERSION").read_text(encoding="utf-8").strip()
)
BOOTSTRAP_ROOT = REPOSITORY / "framework" / "nxbootstrap"
EXPECTED_BOOTSTRAP_VERSION = (
    (BOOTSTRAP_ROOT / "VERSION").read_text(encoding="utf-8").strip()
)
BOOTSTRAP_GENERATOR = BOOTSTRAP_ROOT / "tools" / "generate-port.py"
BOOTSTRAP_LAUNCHER_TEMPLATE = BOOTSTRAP_ROOT / "templates" / "launcher.sh.in"
NXSPLASH_ROOT = REPOSITORY / "framework" / "nxsplash"
NXSPLASH_VERSION = (NXSPLASH_ROOT / "VERSION").read_text(
    encoding="utf-8").strip()
NXSPLASH_RELEASE_MANIFEST = NXSPLASH_ROOT / "release" / "manifest-v1.json"
NXSPLASH_SOURCE = NXSPLASH_ROOT / "src" / "nxsplash.c"
NXEXTRACT = (
    REPOSITORY / "suportando_outros_devices" / "extrator-universal"
)
NXEXTRACT_VERSION = (NXEXTRACT / "VERSION").read_text(
    encoding="utf-8").strip()
NXEXTRACT_UI_MANIFEST = NXEXTRACT / "ui" / "release" / "manifest-v1.json"
NXEXTRACT_UI_SOURCE = NXEXTRACT / "ui" / "nxextract_ui.c"
NXEXTRACT_UI_VERSION = json.loads(
    NXEXTRACT_UI_MANIFEST.read_text(encoding="utf-8")
)["version"]
PORTMASTER_ROOT = REPOSITORY / "framework" / "portmaster"
PORTMASTER_CYCLE = PORTMASTER_ROOT / "tools" / "harbourmaster-cycle.py"
PORTMASTER_CONTRACT = PORTMASTER_ROOT / "contract-v3.json"
PORTMASTER_SCHEMA = (
    PORTMASTER_ROOT / "schema" / "port-json-supported-v2.schema.json"
)
PORTMASTER_PROVENANCE = (
    PORTMASTER_ROOT / "vendor" / "PortMaster-GUI-8f9ddc4.json"
)
PORTMASTER_LEGACY_RUNTIME = (
    PORTMASTER_ROOT / "fixtures" / "harbourmaster" /
    "runtime-legacy-compat-v1.json"
)
EXAMPLES = (
    (ROOT / "examples" / "nxproject-aarch64.example.json",
     "nxexample-aarch64", "NXExample AArch64.sh", "aarch64"),
    (ROOT / "examples" / "nxproject-armv7.example.json",
     "nxexample-armv7", "NXExample ARMv7.sh", "armv7"),
)
MIXED_EXAMPLE = (
    ROOT / "examples" / "nxproject-mixed-armv7.example.json",
    "nxexample-mixed-armhf",
    "NXExample Mixed ARMHF.sh",
    "armv7",
)
SCHEMA_V3 = ROOT / "schema" / "nxproject-v3.schema.json"
APK_VARIANT_FIXTURES = (
    ROOT / "tests" / "fixtures" / "apk-variant-recipes" / "angrybirds.json",
    ROOT / "tests" / "fixtures" / "apk-variant-recipes" / "retrohighway.json",
    ROOT / "tests" / "fixtures" / "apk-variant-recipes" / "scourgebringer.json",
)
IPV4_RE = re.compile(
    r"(?<![0-9])(?:[0-9]{1,3}[.]){3}[0-9]{1,3}(?![0-9])"
)
LINK_RE = re.compile(r"\[[^]]+\]\(([^)]+)\)")
EXTERNAL_STAT_RE = re.compile(
    r"(?m)(?:^|[;&|(`])[ \t]*(?:command[ \t]+)?stat(?:[ \t]|$)"
)


class GateError(Exception):
    pass


def load_tool_module():
    specification = importlib.util.spec_from_file_location(
        "nxgenerator_under_test", TOOL
    )
    require(specification is not None and specification.loader is not None,
            "cannot load nxgenerator for focused deployment checks")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def require(condition, message):
    if not condition:
        raise GateError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def nxextract_ui_artifact(architecture):
    document = json.loads(
        NXEXTRACT_UI_MANIFEST.read_text(encoding="utf-8")
    )
    record = document["artifacts"][architecture]
    return NXEXTRACT / record["path"], record


def validate_apk_variant_regressions(tool_module):
    recipes = {
        path.stem: json.loads(path.read_text(encoding="utf-8"))
        for path in APK_VARIANT_FIXTURES
    }
    require(set(recipes) == {"angrybirds", "retrohighway", "scourgebringer"},
            "real APK variant recipe fixture set changed")
    require(recipes["angrybirds"]["input"]["packages"] ==
            ["com.rovio.angrybirds"], "Angry Birds package fixture drifted")
    require(recipes["retrohighway"]["input"]["packages"] ==
            ["com.nicolaigd.retrohighway"],
            "Retro Highway package fixture drifted")
    require(recipes["scourgebringer"]["input"]["packages"] ==
            ["com.pid.scourgebringer"],
            "ScourgeBringer package fixture drifted")
    for recipe in recipes.values():
        tool_module.validate_apk_variant_policy(recipe)

    # V3: the pre-V3 Angry Birds shape (container sha256 whitelist of two)
    # is the canonical NEGATIVE -- identity lists of ANY length are refused.
    legacy = json.loads((APK_VARIANT_FIXTURES[0].parent / "angrybirds-legacy-identity.json"
                         ).read_text(encoding="utf-8"))
    try:
        tool_module.validate_apk_variant_policy(legacy)
    except tool_module.ProjectError as error:
        require("NXA0001" in str(error),
                "legacy identity whitelist must fail with NXA0001")
    else:
        raise GateError("legacy container sha whitelist passed")

    scourge_container = recipes["scourgebringer"]["extract"][-1]
    require(scourge_container["source"] == {"kind": "container"} and
            scourge_container["validate"] == {"type": "file"},
            "ScourgeBringer transformed container fixture gained bounds/magic")

    flexible = json.loads(json.dumps(recipes["retrohighway"]))
    container = flexible["extract"][0]["validate"]
    negative_cases = (
        ("missing-package", lambda value: value["input"].pop("packages")),
        ("single-sha", lambda value: value["extract"][0]["validate"].update(
            {"sha256": "b" * 64})),
        ("single-crc", lambda value: value["extract"][0]["validate"].update(
            {"crc32": "deadbeef"})),
        ("exact-size", lambda value: value["extract"][0]["validate"].update(
            {"size": 123456})),
        ("source-validate-sha", lambda value: value["extract"][0].update(
            {"source_validate": {"sha256": "c" * 64}})),
        ("literal-container-name", lambda value: value["extract"][0]["source"].update(
            {"patterns": ["OriginalOwnerCopy.apk"]})),
        ("signing-member", lambda value: value.update(
            {"compatibility": {
                "required_members": ["META-INF/CERT.RSA"]
            }})),
    )
    require("sha256" not in container and "crc32" not in container,
            "Retro Highway fixture no longer covers hash-free container")
    for label, mutate in negative_cases:
        candidate = json.loads(json.dumps(flexible))
        mutate(candidate)
        try:
            tool_module.validate_apk_variant_policy(candidate)
        except tool_module.ProjectError:
            pass
        else:
            raise GateError("APK variant policy accepted %s" % label)

    # Flexible BYO identity (mandatory house rule): a lone container with the
    # declared package plus a magic header and bounded size is valid on its own
    # -- every game bundle differs, so a fixed SHA can never be listed. The
    # retrohighway fixture container carries exactly package+magic+bounds.
    flexible_only = json.loads(json.dumps(flexible))
    flexible_only["extract"] = [flexible_only["extract"][0]]
    require("magic_hex" in container and "min_size" in container and
            "max_size" in container,
            "Retro Highway fixture no longer covers magic+bounded container")
    tool_module.validate_apk_variant_policy(flexible_only)

    # A hash-free container that also drops its magic and bounds has no identity
    # at all and must still fail.
    unanchored = json.loads(json.dumps(flexible))
    unanchored["extract"] = [unanchored["extract"][0]]
    for field in ("magic_hex", "magic_ascii", "min_size", "max_size"):
        unanchored["extract"][0]["validate"].pop(field, None)
    try:
        tool_module.validate_apk_variant_policy(unanchored)
    except tool_module.ProjectError:
        pass
    else:
        raise GateError("container without SHA, magic or bounds passed")


def run_generator(manifest, output, expected=0, source_root=None):
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    command = [sys.executable, "-B", str(TOOL), str(manifest),
               "--output", str(output)]
    if source_root is not None:
        command.extend(("--source-root", str(source_root)))
    result = subprocess.run(
        command,
        cwd=str(REPOSITORY),
        env=environment,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    require(result.returncode == expected,
            "generator status %d != %d: %s" %
            (result.returncode, expected, result.stderr.strip()))
    return result


def snapshot(root):
    result = {}
    for path in sorted(root.rglob("*")):
        require(not path.is_symlink(), "generated tree contains a symlink")
        if path.is_file():
            result[path.relative_to(root).as_posix()] = (
                stat.S_IMODE(path.stat().st_mode), path.read_bytes()
            )
    return result


def validate_real_portmaster_cycle(output, archive_path):
    with zipfile.ZipFile(str(archive_path), "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(item for item in output.rglob("*") if item.is_file()):
            relative = path.relative_to(output).as_posix()
            info = zipfile.ZipInfo(relative)
            info.external_attr = (
                (stat.S_IFREG | stat.S_IMODE(path.stat().st_mode)) << 16
            )
            archive.writestr(info, path.read_bytes())
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    process = subprocess.run(
        [sys.executable, "-B", str(PORTMASTER_CYCLE), str(archive_path)],
        cwd=str(REPOSITORY), env=environment, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
    )
    require(process.returncode == 0,
            "generated tree failed the real HarbourMaster cycle: %s" %
            (process.stderr or process.stdout).strip())
    require("parser=8f9ddc4" in process.stdout and
            "install=1" in process.stdout and "reinstall=1" in process.stdout,
            "real HarbourMaster cycle produced no terminal proof")


def validate_links(readme):
    text = readme.read_text(encoding="utf-8")
    for target in LINK_RE.findall(text):
        if target.startswith(("#", "http://", "https://")):
            continue
        require((readme.parent / target).is_file(),
                "generated README link is missing: %s" % target)


def validate_generation(output, port_id, launcher_name, architecture):
    port = output / port_id
    launcher_path = output / launcher_name
    nxport = json.loads(
        (port / "nxport.json").read_text(encoding="utf-8")
    )
    require(nxport["architecture"] == architecture,
            "generated game architecture differs from the project")
    execution_roles = nxport.get("execution_roles")
    splash_architecture = architecture
    extractor_architecture = architecture
    if execution_roles is not None:
        splash_architecture = execution_roles["splash"]["architecture"]
        extractor_role = execution_roles["extractor"]
        extractor_architecture = (
            extractor_role["architecture"] if extractor_role is not None
            else None
        )
    splash_path = port / "nxsplash-nextos"
    extract_ui_path = port / "nxextract" / "nxextract-ui"
    require(launcher_path.is_file() and not launcher_path.is_symlink(),
            "wrapper is absent or unsafe")
    for retired in ("run.sh", "nxbootstrap.sh",
                    "nxbootstrap-%s.sh" % EXPECTED_BOOTSTRAP_VERSION,
                    "nxdeployment.json"):
        require(not (port / retired).exists() and
                not (output / retired).exists(),
                "generator retained the retired artifact %s" % retired)
    require(stat.S_IMODE(launcher_path.stat().st_mode) == 0o755,
            "wrapper mode changed")
    require(splash_path.is_file() and not splash_path.is_symlink() and
            stat.S_IMODE(splash_path.stat().st_mode) == 0o755,
            "nxsplash helper is absent or unsafe")
    splash_release = json.loads(
        NXSPLASH_RELEASE_MANIFEST.read_text(encoding="utf-8")
    )
    splash_artifact = splash_release["artifacts"][splash_architecture]
    nxextract_ui = None
    nxextract_ui_record = None
    if nxport["nxextract"]["mode"] != "no":
        require(extractor_architecture is not None,
                "active NXExtract project lacks an extractor role")
        nxextract_ui, nxextract_ui_record = nxextract_ui_artifact(
            extractor_architecture
        )
    require(sha256(splash_path) == splash_artifact["sha256"] and
            nxport["required_files"][1] == "nxsplash-nextos" and
            nxport["required_files"].count("nxsplash-nextos") == 1,
            "generated nxsplash bytes or required-file identity changed")
    if nxport["nxextract"]["mode"] == "no":
        require(not extract_ui_path.exists(),
                "disabled NXExtract project retained the UI")
    else:
        require(extract_ui_path.is_file() and
                not extract_ui_path.is_symlink() and
                stat.S_IMODE(extract_ui_path.stat().st_mode) == 0o755 and
                sha256(extract_ui_path) == nxextract_ui_record["sha256"] ==
                sha256(nxextract_ui),
                "mandatory NXExtract UI is absent, unsafe or unpinned")

    scripts = [launcher_path]
    if nxport["nxextract"]["mode"] != "no":
        scripts.extend((
            port / "nxextract" / "run-extractor.sh",
            port / "nxextract" / "nxextract-runtime-env.sh",
        ))
    for script in scripts:
        script_text = script.read_text(encoding="utf-8")
        require(not EXTERNAL_STAT_RE.search(script_text),
                "generated public shell path calls external stat: %s" %
                script.name)
        checked = subprocess.run(
            ["bash", "-n", str(script)], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        require(checked.returncode == 0,
                "generated shell syntax failed: %s" % script.name)

    wrapper = launcher_path.read_text(encoding="utf-8")
    require("nxbootstrap %s" % EXPECTED_BOOTSTRAP_VERSION in wrapper,
            "launcher does not record its generator version")
    require(not re.search(r"@[A-Z0-9_]+@", wrapper),
            "launcher has unresolved template tokens")
    if execution_roles is None:
        require("EXECUTION RECEIPT:" not in wrapper,
                "legacy scaffold gained the mixed-ABI resolver")
    else:
        for token in (
            "nxbootstrap_elf_identity()",
            "EXECUTION RECEIPT: role=$role",
            "NXBOOTSTRAP_EXTRACTOR_EXECUTOR",
            "NXBOOTSTRAP_SPLASH_LOADER",
            "NXBOOTSTRAP_GAME_LOADER",
        ):
            require(token in wrapper,
                    "mixed scaffold lacks execution contract: %s" % token)
    expected_runtime_exports = (
        "export NXCOMPAT_PORT_ID=%s" % port_id,
        'export NXCOMPAT_GAME_DIR="$NXBOOTSTRAP_LOGICAL_GAMEDIR"',
        "export NXCOMPAT_REQUIRED_CAPABILITIES=",
        "export NXCOMPAT_ENABLED_QUIRKS=",
        "export NXCOMPAT_RUNTIME_REPORT=%s" % nxport["runtime_report"],
    )
    for runtime_export in expected_runtime_exports:
        require(runtime_export in wrapper,
                "launcher lacks runtime export: %s" % runtime_export)
    language = nxport.get("language")
    if language is None:
        require("GAME_LANGUAGE=" not in wrapper and
                "NXPORT_LANGUAGE" not in wrapper,
                "launcher exposes language without adapter opt-in")
    else:
        generator = load_tool_module().BOOTSTRAP_GENERATOR
        require(wrapper.count(generator.render_language_block(nxport)) == 1 and
                wrapper.count(
                    generator.render_language_reassert_block(nxport)
                ) == 1,
                "launcher language contract differs from nxport")
    required_assignment = "NXBOOTSTRAP_REQUIRED_FILES=%s" % \
        load_tool_module().BOOTSTRAP_GENERATOR.shell_join(
            nxport["required_files"]
        )
    require(wrapper.count("NXBOOTSTRAP_REQUIRED_FILES=") == 1 and
            wrapper.count(required_assignment) == 1,
            "launcher required-files assignment differs from nxport")
    bin_assignment = 'BIN="$GAMEDIR/%s"' % nxport["executable"]
    nxextract_block = \
        load_tool_module().BOOTSTRAP_GENERATOR.render_nxextract_block(nxport)
    required_block = \
        load_tool_module().BOOTSTRAP_GENERATOR.render_required_files_block(
            nxport
        )
    splash_block = \
        load_tool_module().BOOTSTRAP_GENERATOR.render_splash_block(nxport)
    require(wrapper.count(splash_block) == 1 and
            not re.search(
                r"NXSPLASH_(?:DISABLE|SKIP)|--(?:disable|skip)", wrapper
            ), "launcher nxsplash handoff became optional or divergent")
    require(wrapper.index(bin_assignment) < wrapper.index(nxextract_block) <
            wrapper.index(required_block) < wrapper.index(splash_block) <
            wrapper.index("# Optional per-port adapter") <
            wrapper.index("# Host phase ends here"),
            "launcher payload/splash gates are outside canonical order")
    requested_assignments = re.findall(
        r"(?m)^[ \t]*NXEXTRACT_REQUESTED=([01])$", wrapper
    )
    expected_requested = {
        "no": [], "yes": ["1"], "auto": ["0", "1"],
    }[nxport["nxextract"]["mode"]]
    require(requested_assignments == expected_requested,
            "launcher NXEXTRACT_REQUESTED mode semantics differ")
    for guarantee in ('exec 9>>"$NXBOOTSTRAP_LOCK_FILE"',
                      "command ls -Lldn /proc/self/fd/9",
                      '"$NXBOOTSTRAP_LOCK_FILE" -ef /proc/self/fd/9',
                      '[ "$NXBOOTSTRAP_FINISHED" = 0 ] || return 0',
                      "nxbootstrap_abort_before_game 129",
                      "nxbootstrap_abort_before_game 130",
                      "nxbootstrap_abort_before_game 143",
                      "NXBOOTSTRAP_CHILD_STARTTIME=${20}",
                      "flock -n 9",
                      'wait "$game_pid"',
                      "NXBOOTSTRAP_SHUTDOWN_TICKS=10",
                      'builtin kill -KILL "$game_pid"',
                      "trap '' INT TERM HUP",
                      "printf '\\033c'",
                      "nxbootstrap_finish"):
        require(guarantee in wrapper,
                "launcher lacks golden-port guarantee: %s" % guarantee)
    if architecture == "armv7":
        require('PORT_32BIT="Y"' in wrapper and
                "export PORT_32BIT" in wrapper,
                "ARMHF wrapper lost its literal")
    else:
        require('PORT_32BIT="Y"' not in wrapper,
                "AArch64 wrapper gained ARMHF metadata")

    if nxport["nxextract"]["mode"] == "no":
        require(not (port / "nxextract").exists() and
                not (port / "extractor.json").exists(),
                "disabled NXExtract project still vendors an integration")
        require(not (port / "gamedata").exists(),
                "disabled NXExtract project must not promise gamedata/")
    else:
        # GAMEDATA-DIR-01: the owner-data directory must exist physically
        # through a real generated marker, never an empty ZIP entry.
        marker = port / "gamedata" / "README.txt"
        require(marker.is_file() and not marker.is_symlink(),
                "gamedata/README.txt marker is absent or not regular")
        require((marker.stat().st_mode & 0o7777) == 0o644,
                "gamedata marker mode must be 0644")
        marker_text = marker.read_text(encoding="utf-8")
        require(marker_text.strip() != "", "gamedata marker is empty")
        for token in ("[PT-BR]", "[EN]", "../INSTALLATION.md",
                      "gamedata/", "legalmente", "lawfully"):
            require(token in marker_text,
                    "gamedata marker lacks: %s" % token)
        require("http" not in marker_text.lower() and
                ".apk\n" not in marker_text.lower(),
                "gamedata marker leaks an origin or filename")
        installation_v3 = (port / "INSTALLATION.md").read_text(
            encoding="utf-8"
        )
        require("%s/gamedata/" % port_id in installation_v3,
                "INSTALLATION.md does not document the gamedata path")
        for source_name, target_name in (
                ("nxextract.py", "nxextract.py"),
                ("run-extractor.sh", "run-extractor.sh"),
                ("nxextract-runtime-env.sh", "nxextract-runtime-env.sh")):
            require((port / "nxextract" / target_name).read_bytes() ==
                    (NXEXTRACT / source_name).read_bytes(),
                    "vendored NXExtract file differs: %s" % target_name)
        require((port / "nxextract" / "nxextract-ui").read_bytes() ==
                nxextract_ui.read_bytes(),
                "vendored NXExtract UI differs from its architecture release")
        require((port / "extractor.json").read_bytes() ==
                (NXEXTRACT / "examples" / "recipe-minimal.json").read_bytes(),
                "NXExtract recipe is not byte-pinned")

    adapter = json.loads(
        (port / "adapter" / "adapter-contract.json").read_text(encoding="utf-8")
    )
    require(adapter["status"] == "unimplemented_nonrelease" and
            adapter["release_ready"] is False,
            "adapter skeleton overclaims readiness")
    require(adapter["lifecycle"] == {"sequence": [], "source_evidence": []},
            "adapter skeleton invented lifecycle")
    adapter_input = adapter["input"]
    if adapter_input.get("mapping_authority") is None:
        require(adapter_input.get("actions") == [] and
                adapter_input.get("contexts") == {},
                "legacy adapter skeleton invented control actions")
    else:
        require(isinstance(adapter_input.get("actions"), list) and
                adapter_input["actions"] and
                isinstance(adapter_input.get("contexts"), dict) and
                set(("menu", "gameplay")) <= set(adapter_input["contexts"]),
                "V3 adapter skeleton lost action/context declarations")
    require("language_access" in adapter,
            "adapter skeleton lost the V3 language declaration")
    require(adapter["jni"]["callbacks"] == [] and
            adapter["persistence"]["callbacks"] == [] and
            adapter["terminal"]["action"] is None,
            "adapter skeleton invented callbacks or terminal behavior")

    metadata = json.loads((port / "port.json").read_text(encoding="utf-8"))
    expected_arch = "armhf" if architecture == "armv7" else "aarch64"
    require(metadata == {
        "version": 4,
        "name": port_id + ".zip",
        "items": [launcher_name, port_id + "/"],
        "items_opt": [],
        "attr": {
            "title": nxport["title"],
            "arch": [expected_arch],
            "min_glibc": "2.17",
        },
    }, "generated PortMaster metadata changed")

    gameinfo_path = port / "gameinfo.xml"
    gameinfo_bytes = gameinfo_path.read_bytes()
    expected_gameinfo = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<gameList>\n'
        '  <game>\n'
        '    <path>./%s</path>\n'
        '    <name>%s</name>\n'
        '  </game>\n'
        '</gameList>\n' % (launcher_name, nxport["title"])
    ).encode("utf-8")
    gameinfo = ElementTree.fromstring(gameinfo_bytes)
    require(gameinfo.tag == "gameList" and len(gameinfo) == 1 and
            [child.tag for child in gameinfo[0]] == ["path", "name"] and
            gameinfo[0].findtext("path") == "./" + launcher_name and
            gameinfo[0].findtext("name") == nxport["title"] and
            gameinfo_bytes == expected_gameinfo,
            "generated gameinfo.xml is not canonical")

    installation = port / "INSTALLATION.md"
    installation_text = installation.read_text(encoding="utf-8")
    require(installation.is_file() and not installation.is_symlink() and
            "## English" in installation_text and
            "## Português" in installation_text and
            "REQUIRED BEFORE RELEASE" in installation_text and
            "OBRIGATÓRIO ANTES DA RELEASE" in installation_text and
            launcher_name in installation_text and port_id in installation_text,
            "generated INSTALLATION.md is absent or incomplete")

    readme = port / "README.md"
    readme_text = readme.read_text(encoding="utf-8")
    for token in (
        "## English", "## Português", "### Architecture",
        "### Arquitetura", "### Solved problems",
        "### Problemas resolvidos", "### Controls", "### Controles",
        "### Game data", "### Dados do jogo", "### Build and run",
        "### Compilar e executar", "### Source map", "### Mapa de fontes",
        "### Licenses", "### Licenças", "Mali-450/GLES2",
        "same ZIP and SHA-256", "mesmo ZIP público exato",
        "development-only", "somente ao desenvolvimento",
    ):
        require(token in readme_text, "generated README lacks: %s" % token)
    require(not IPV4_RE.search(readme_text) and "/home/" not in readme_text and
            "/Users/" not in readme_text,
            "generated README contains a private literal")
    validate_links(readme)

    # V3-CONTROLLERS-01: every new port ships the immutable default map.
    controllers_default = port / "defaults" / "NEXTOSCONTROLLERS.gptk"
    require(controllers_default.is_file() and
            not controllers_default.is_symlink(),
            "defaults/NEXTOSCONTROLLERS.gptk is missing")
    controllers_text = controllers_default.read_text(encoding="utf-8")
    require("format = NEXTOS_CONTROLLERS/1" in controllers_text and
            "[menu]" in controllers_text and
            "[gameplay]" in controllers_text and
            ("port = %s" % port_id) in controllers_text,
            "controllers default lacks the NEXTOS_CONTROLLERS/1 contract")
    require("gptokeyb" not in controllers_text.lower().replace(
        "não é gptokeyb", "").replace("not gptokeyb", ""),
        "controllers default must not reference gptokeyb semantics")
    settings_default = port / "defaults" / "NEXTOSSETTINGS.txt"
    require(settings_default.is_file() and not settings_default.is_symlink(),
            "defaults/NEXTOSSETTINGS.txt is missing")
    settings_text = settings_default.read_text(encoding="utf-8")
    require(settings_text.startswith("# NEXTOS_SETTINGS/1") and
            "language=auto" in settings_text,
            "settings default lacks the NEXTOS_SETTINGS/1 contract")

    receipt_path = port / "GENERATION.json"
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    require(receipt["generator"] == {
        "name": "nxgenerator", "version": EXPECTED_GENERATOR_VERSION
    }, "generator version is not pinned")
    require(isinstance(receipt.get("generation_id"), str) and
            len(receipt["generation_id"]) == 64 and
            all(c in "0123456789abcdef" for c in receipt["generation_id"]),
            "GENERATION.json lacks a well-formed generation_id")
    expected_bootstrap_pin = {
        "version": EXPECTED_BOOTSTRAP_VERSION,
        "source_files": {
            "templates/launcher.sh.in": sha256(BOOTSTRAP_LAUNCHER_TEMPLATE),
            "tools/generate-port.py": sha256(BOOTSTRAP_GENERATOR),
        },
    }
    expected_splash_pin = {
        "artifact": {
            "architecture": splash_architecture,
            "mode": "0755",
            "path": "release/%s/nxsplash-nextos" % splash_architecture,
            "sha256": splash_artifact["sha256"],
        },
        "release_manifest_sha256": sha256(NXSPLASH_RELEASE_MANIFEST),
        "source_sha256": sha256(NXSPLASH_SOURCE),
        "version": NXSPLASH_VERSION,
    }
    expected_nxextract_pin = receipt["source_pins"]["nxextract"]
    if nxport["nxextract"]["mode"] == "no":
        require(expected_nxextract_pin is None,
                "disabled NXExtract project retained a source pin")
    else:
        require(expected_nxextract_pin["version"] == NXEXTRACT_VERSION,
                "vendored NXExtract version pin is not exact")
        require(expected_nxextract_pin["ui_version"] == NXEXTRACT_UI_VERSION,
                "vendored NXExtract UI version pin is not exact")
        require(expected_nxextract_pin["files"]["nxextract-ui"] ==
                nxextract_ui_record["sha256"],
                "vendored NXExtract UI source pin is not exact")
        require(expected_nxextract_pin["ui_release_manifest_sha256"] ==
                sha256(NXEXTRACT_UI_MANIFEST) and
                expected_nxextract_pin["ui_source_sha256"] ==
                sha256(NXEXTRACT_UI_SOURCE) and
                expected_nxextract_pin["ui_artifact"] == {
                    "architecture": extractor_architecture,
                    "mode": "0755",
                    "path": nxextract_ui_record["path"],
                    "sha256": nxextract_ui_record["sha256"],
                }, "vendored NXExtract architecture pin is incomplete")
    if execution_roles is None:
        require("execution_roles" not in receipt,
                "legacy receipt gained mixed-ABI semantics")
    else:
        require(receipt.get("execution_roles") == execution_roles,
                "generation receipt lost the canonical execution roles")
    require(receipt["source_pins"]["nxbootstrap"] == expected_bootstrap_pin,
            "vendored nxbootstrap source pins are not exact")
    require(receipt["source_pins"]["nxsplash"] == expected_splash_pin,
            "vendored nxsplash source/artifact pins are not exact")
    require(receipt["source_pins"]["portmaster"] == {
        "contract": "v3",
        "contract_sha256": sha256(PORTMASTER_CONTRACT),
        "legacy_parser_commit":
            "7471d54c7c6ca57c16dee1b77cdd57d4226a1b86",
        "legacy_runtime_fixture_sha256": sha256(PORTMASTER_LEGACY_RUNTIME),
        "metadata_schema": "port-json-supported-v2",
        "metadata_schema_sha256": sha256(PORTMASTER_SCHEMA),
        "parser_commit": "8f9ddc4b0f75dfe61eb370bd3d1b4ec9d5ef6967",
        "vendor_provenance_sha256": sha256(PORTMASTER_PROVENANCE),
    }, "PortMaster parser/schema source pins are not exact")
    require(receipt["claims"] == {
        "deterministic_scaffold": True,
        "release_ready": False,
        "physical_support_proven": False,
        "adapter_lifecycle_implemented": False,
    }, "generation receipt overclaims completion")
    expected_records = {}
    for path in sorted(output.rglob("*")):
        if path.is_file() and path != receipt_path:
            expected_records[path.relative_to(output).as_posix()] = {
                "path": path.relative_to(output).as_posix(),
                "mode": "%04o" % stat.S_IMODE(path.stat().st_mode),
                "sha256": sha256(path),
            }
    actual_records = {entry["path"]: entry for entry in receipt["artifacts"]}
    require(actual_records == expected_records,
            "generation artifact inventory is incomplete or stale")
    deployment_artifacts = {
        launcher_name,
        "%s/nxport.json" % port_id,
        "%s/nxsplash-nextos" % port_id,
    }
    require(deployment_artifacts <= set(actual_records),
            "generation inventory omits the launcher deployment set")
    require(actual_records[launcher_name]["sha256"] == sha256(launcher_path) and
            actual_records["%s/nxport.json" % port_id]["sha256"] ==
            sha256(port / "nxport.json") and
            actual_records["%s/nxsplash-nextos" % port_id]["sha256"] ==
            sha256(splash_path),
            "generation inventory contains stale deployment hashes")
    require((port / "LICENSE").read_bytes() ==
            (REPOSITORY / "LICENSE").read_bytes(),
            "license was not copied exactly")


def validate_deployment_tamper_rejection(clean, work, port_id, launcher_name):
    tool_module = load_tool_module()
    source_state = tool_module.bootstrap_source_state()
    nxport = json.loads(
        (clean / port_id / "nxport.json").read_text(encoding="utf-8")
    )
    splash_state = tool_module.nxsplash_source_state(
        tool_module.execution_role_architecture(nxport, "splash")
    )

    def expect_rejected(label, mutate):
        target = work / ("tampered-" + label)
        shutil.copytree(clean, target)
        mutate(target)
        try:
            tool_module.validate_bootstrap_deployment(
                target, nxport, source_state, splash_state
            )
        except tool_module.ProjectError:
            return
        raise GateError(
            "nxgenerator accepted a tampered deployment: %s" % label
        )

    def resurrect_library(root):
        (root / port_id / "nxbootstrap.sh").write_text(
            "#!/bin/bash\n", encoding="utf-8"
        )

    def resurrect_receipt(root):
        (root / port_id / "nxdeployment.json").write_text(
            "{}\n", encoding="utf-8"
        )

    def strip_instance_lock(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        require("flock -n 9" in contents,
                "fixture launcher lacks the instance lock")
        path.write_text(
            contents.replace("flock -n 9", "true", 1), encoding="utf-8"
        )

    def remove_manifest(root):
        (root / port_id / "nxport.json").unlink()

    def remove_splash(root):
        (root / port_id / "nxsplash-nextos").unlink()

    def alter_splash(root):
        (root / port_id / "nxsplash-nextos").write_bytes(b"tampered\n")

    def strip_runtime_contract(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        needle = "export NXCOMPAT_RUNTIME_REPORT="
        require(needle in contents,
                "fixture launcher lacks the runtime contract")
        path.write_text(
            contents.replace(needle, "export NXCOMPAT_REPORT=", 1),
            encoding="utf-8",
        )

    def duplicate_runtime_contract(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        marker = 'if [ -n "$BIN_PRELOAD" ]; then'
        require(contents.count(marker) == 1,
                "fixture launcher lacks its launch boundary")
        path.write_text(
            contents.replace(
                marker,
                "NXCOMPAT_RUNTIME_REPORT=divergent\n" + marker,
                1,
            ),
            encoding="utf-8",
        )

    def diverge_required_files(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        assignment = "NXBOOTSTRAP_REQUIRED_FILES=%s" % \
            tool_module.BOOTSTRAP_GENERATOR.shell_join(
                nxport["required_files"]
            )
        require(contents.count(assignment) == 1,
                "fixture launcher lacks the required-files assignment")
        path.write_text(
            contents.replace(
                assignment, "NXBOOTSTRAP_REQUIRED_FILES=wrong-file", 1
            ),
            encoding="utf-8",
        )

    def strip_required_guard(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        guard = '[ ! -s "$required_path" ]'
        require(contents.count(guard) == 1,
                "fixture launcher lacks the non-empty required-file guard")
        path.write_text(
            contents.replace(guard, "false", 1), encoding="utf-8"
        )

    def move_required_gate_before_nxextract(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        block = tool_module.BOOTSTRAP_GENERATOR.render_required_files_block(
            nxport
        )
        nxextract_block = tool_module.BOOTSTRAP_GENERATOR.render_nxextract_block(
            nxport
        )
        require(contents.count(block) == 1 and
                contents.count(nxextract_block) == 1,
                "fixture launcher lacks the ordered required-files gate")
        contents = contents.replace(block, "", 1)
        path.write_text(
            contents.replace(nxextract_block, block + "\n" + nxextract_block,
                             1),
            encoding="utf-8",
        )

    def strip_splash_handoff(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        block = tool_module.BOOTSTRAP_GENERATOR.render_splash_block(nxport)
        require(contents.count(block) == 1,
                "fixture launcher lacks the mandatory nxsplash handoff")
        path.write_text(
            contents.replace(block, "# nxsplash stripped\ntrue", 1),
            encoding="utf-8",
        )

    def move_splash_before_required(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        splash = tool_module.BOOTSTRAP_GENERATOR.render_splash_block(nxport)
        required = tool_module.BOOTSTRAP_GENERATOR.render_required_files_block(
            nxport
        )
        require(contents.count(splash) == 1 and contents.count(required) == 1,
                "fixture launcher lacks ordered payload/splash gates")
        contents = contents.replace(splash, "", 1)
        path.write_text(
            contents.replace(required, splash + "\n" + required, 1),
            encoding="utf-8",
        )

    def disable_required_nxextract(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        assignment = "NXEXTRACT_REQUESTED=1"
        require(nxport["nxextract"]["mode"] == "yes" and
                contents.count(assignment) == 1,
                "fixture launcher is not a mandatory NXExtract port")
        path.write_text(
            contents.replace(assignment, "NXEXTRACT_REQUESTED=0", 1),
            encoding="utf-8",
        )

    def strip_executable_symlink_guard(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        guard = '[ -L "$GAMEDIR/%s" ]' % nxport["executable"]
        require(contents.count(guard) == 1,
                "fixture launcher lacks the executable symlink guard")
        path.write_text(
            contents.replace(
                guard, '[ -L "$NXBOOTSTRAP_EXECUTABLE" ]', 1
            ),
            encoding="utf-8",
        )

    def hide_payload_gates_in_dead_code(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        nxextract_block = \
            tool_module.BOOTSTRAP_GENERATOR.render_nxextract_block(nxport)
        required_block = \
            tool_module.BOOTSTRAP_GENERATOR.render_required_files_block(nxport)
        require(contents.count(nxextract_block) == 1 and
                contents.count(required_block) == 1,
                "fixture launcher lacks its payload gates")
        contents = contents.replace(
            nxextract_block, "if false; then\n" + nxextract_block, 1
        )
        path.write_text(
            contents.replace(required_block, required_block + "\nfi", 1),
            encoding="utf-8",
        )

    def restore_inode_bound_lock(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        stable = 'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"'
        require(contents.count(stable) == 1,
                "fixture launcher lacks its stable lock open")
        path.write_text(
            contents.replace(
                stable, 'exec 9<"$NXBOOTSTRAP_EXECUTABLE"', 1
            ),
            encoding="utf-8",
        )

    def strip_forced_termination(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        forced = 'builtin kill -KILL "$game_pid"'
        require(contents.count(forced) == 1,
                "fixture launcher lacks bounded forced termination")
        path.write_text(
            contents.replace(forced, 'builtin kill -TERM "$game_pid"', 1),
            encoding="utf-8",
        )

    def strip_finish_guard(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        guard = '[ "$NXBOOTSTRAP_FINISHED" = 0 ] || return 0'
        require(contents.count(guard) == 1,
                "fixture launcher lacks the pm_finish exactly-once guard")
        path.write_text(
            contents.replace(guard, "true", 1), encoding="utf-8"
        )

    expect_rejected("resurrected-library", resurrect_library)
    expect_rejected("resurrected-receipt", resurrect_receipt)
    expect_rejected("missing-splash", remove_splash)
    expect_rejected("altered-splash", alter_splash)
    expect_rejected("stripped-instance-lock", strip_instance_lock)
    expect_rejected("stripped-runtime-contract", strip_runtime_contract)
    expect_rejected("duplicate-runtime-contract", duplicate_runtime_contract)
    expect_rejected("divergent-required-files", diverge_required_files)
    expect_rejected("stripped-required-guard", strip_required_guard)
    expect_rejected("required-before-nxextract",
                    move_required_gate_before_nxextract)
    expect_rejected("stripped-splash", strip_splash_handoff)
    expect_rejected("splash-before-required", move_splash_before_required)
    expect_rejected("disabled-required-nxextract", disable_required_nxextract)
    expect_rejected("stripped-executable-preflight",
                    strip_executable_symlink_guard)
    expect_rejected("dead-payload-gates", hide_payload_gates_in_dead_code)
    expect_rejected("inode-bound-lock", restore_inode_bound_lock)
    expect_rejected("unbounded-termination", strip_forced_termination)
    expect_rejected("unguarded-finish", strip_finish_guard)
    expect_rejected("missing-manifest", remove_manifest)


def validate_nxextract_release_tamper_rejection(tool_module, work):
    clean = work / "nxextract-release-clean"
    shutil.copytree(NXEXTRACT, clean)
    original_root = tool_module.NXEXTRACT_ROOT
    original_manifest = tool_module.NXEXTRACT_RELEASE_MANIFEST
    original_source = tool_module.NXEXTRACT_SOURCE

    def write_manifest(root, document):
        (root / "ui" / "release" / "manifest-v1.json").write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def expect_rejected(label, mutate):
        target = work / ("nxextract-release-tampered-" + label)
        shutil.copytree(clean, target)
        mutate(target)
        tool_module.NXEXTRACT_ROOT = target
        tool_module.NXEXTRACT_RELEASE_MANIFEST = (
            target / "ui" / "release" / "manifest-v1.json"
        )
        tool_module.NXEXTRACT_SOURCE = target / "ui" / "nxextract_ui.c"
        try:
            tool_module.nxextract_source_state("aarch64")
        except tool_module.ProjectError:
            return
        raise GateError(
            "nxgenerator accepted a tampered NXExtract release: %s" % label
        )

    def cross_arch_path(root):
        path = root / "ui" / "release" / "manifest-v1.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        document["artifacts"]["aarch64"]["path"] = (
            "ui/release/armv7/nxextract-ui"
        )
        write_manifest(root, document)

    def cross_arch_bytes(root):
        path = root / "ui" / "release" / "manifest-v1.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        destination = root / document["artifacts"]["aarch64"]["path"]
        source = root / document["artifacts"]["armv7"]["path"]
        destination.write_bytes(source.read_bytes())
        os.chmod(destination, 0o755)
        record = document["artifacts"]["aarch64"]
        record["sha256"] = sha256(destination)
        record["size"] = destination.stat().st_size
        write_manifest(root, document)

    def alter_artifact(root):
        path = root / "ui" / "release" / "manifest-v1.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        artifact = root / document["artifacts"]["aarch64"]["path"]
        artifact.write_bytes(artifact.read_bytes() + b"tampered")

    def alter_source(root):
        source = root / "ui" / "nxextract_ui.c"
        source.write_bytes(source.read_bytes() + b"\n/* tampered */\n")

    def raise_glibc(root):
        path = root / "ui" / "release" / "manifest-v1.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        document["artifacts"]["aarch64"]["glibc_max"] = "2.31"
        write_manifest(root, document)

    def alter_toolchain(root):
        path = root / "ui" / "release" / "manifest-v1.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        document["toolchain"]["version"] = "latest"
        write_manifest(root, document)

    def alter_ui_version(root):
        path = root / "ui" / "release" / "manifest-v1.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        document["version"] = "1.2.10"
        write_manifest(root, document)

    try:
        expect_rejected("cross-arch-path", cross_arch_path)
        expect_rejected("cross-arch-bytes", cross_arch_bytes)
        expect_rejected("artifact-hash", alter_artifact)
        expect_rejected("source-hash", alter_source)
        expect_rejected("glibc-ceiling", raise_glibc)
        expect_rejected("toolchain", alter_toolchain)
        expect_rejected("ui-version", alter_ui_version)
    finally:
        tool_module.NXEXTRACT_ROOT = original_root
        tool_module.NXEXTRACT_RELEASE_MANIFEST = original_manifest
        tool_module.NXEXTRACT_SOURCE = original_source


def main():
    tool_module = load_tool_module()
    schema_v3 = json.loads(SCHEMA_V3.read_text(encoding="utf-8"))
    promotion_schema = schema_v3["properties"].get("promotion")
    require(isinstance(promotion_schema, dict) and
            promotion_schema.get("additionalProperties") is False and
            promotion_schema.get("required") ==
            ["adapter_contract", "claims"],
            "nxproject v3 schema lacks the closed promotion contract")
    promotion_claims = promotion_schema["properties"]["claims"]
    require(promotion_claims.get("additionalProperties") is False and
            promotion_claims["properties"]["release_ready"] ==
            {"const": True} and
            promotion_claims["properties"]
            ["adapter_lifecycle_implemented"] == {"const": True} and
            promotion_claims["properties"]["physical_support_proven"] ==
            {"type": "boolean"},
            "nxproject v3 promotion claims schema drifted")
    nxextract_states = {
        architecture: tool_module.nxextract_source_state(architecture)
        for architecture in ("aarch64", "armv7")
    }
    require(
        nxextract_states["aarch64"]["artifact"]["path"] !=
        nxextract_states["armv7"]["artifact"]["path"] and
        nxextract_states["aarch64"]["artifact"]["sha256"] !=
        nxextract_states["armv7"]["artifact"]["sha256"],
        "NXExtract release maps both public architectures to one UI",
    )
    flexible_recipe = {
        "input": {"packages": ["com.example.game"]},
        "extract": [
            {
                "source": {"kind": "container"},
                "validate": {
                    "min_size": 1024, "max_size": 4096,
                    "magic_hex": "504b0304",
                },
            },
            {
                "source": {
                    "kind": "entry",
                    "patterns": ["lib/{abi}/libgame.so"],
                },
                "validate": {"sha256": "a" * 64},
            },
        ],
    }
    tool_module.validate_apk_variant_policy(flexible_recipe)
    exact_recipe = json.loads(json.dumps(flexible_recipe))
    exact_recipe["extract"][0]["validate"].update({
        "size": 2048, "sha256": "b" * 64,
    })
    try:
        tool_module.validate_apk_variant_policy(exact_recipe)
    except tool_module.ProjectError:
        pass
    else:
        raise GateError("single-container APK pin was not rejected")
    validate_apk_variant_regressions(tool_module)

    with tempfile.TemporaryDirectory(prefix="nxgenerator-m19-") as temporary:
        work = Path(temporary)
        validate_nxextract_release_tamper_rejection(tool_module, work)
        external = work / "standalone-source"
        external.mkdir()
        external_manifest = external / "nxproject.json"
        external_document = json.loads(
            (ROOT / "examples" / "nxproject-aarch64.example.json").read_text(
                encoding="utf-8"
            )
        )
        external_document["nxextract_recipe"] = "extractor.json"
        external_manifest.write_text(
            json.dumps(external_document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        (external / "LICENSE").write_bytes((REPOSITORY / "LICENSE").read_bytes())
        (external / "extractor.json").write_bytes(
            (NXEXTRACT / "examples" / "recipe-minimal.json").read_bytes()
        )
        external_output = work / "standalone-generated"
        run_generator(external_manifest, external_output,
                      source_root=external)
        validate_generation(external_output, "nxexample-aarch64",
                            "NXExample AArch64.sh", "aarch64")
        generated_bytes = b"\n".join(
            path.read_bytes() for path in sorted(external_output.rglob("*"))
            if path.is_file()
        )
        require(str(external).encode() not in generated_bytes,
                "standalone source root leaked into generated bytes")
        source_link = work / "standalone-source-link"
        source_link.symlink_to(external, target_is_directory=True)
        run_generator(external_manifest, work / "symlink-root-output",
                      expected=1, source_root=source_link)
        for manifest, port_id, launcher, architecture in EXAMPLES:
            first = work / (port_id + "-first")
            second = work / (port_id + "-second")
            run_generator(manifest, first)
            run_generator(manifest, second)
            validate_generation(first, port_id, launcher, architecture)
            validate_real_portmaster_cycle(
                first, work / (port_id + "-real-portmaster.zip")
            )
            require(snapshot(first) == snapshot(second),
                    "two clean generations differ for %s" % architecture)
            if architecture == "aarch64":
                validate_deployment_tamper_rejection(
                    first, work, port_id, launcher
                )
            run_generator(manifest, first, expected=1)

        mixed_manifest, mixed_id, mixed_launcher, mixed_architecture = \
            MIXED_EXAMPLE
        mixed_output = work / "nxexample-mixed-armhf"
        run_generator(mixed_manifest, mixed_output)
        validate_generation(
            mixed_output, mixed_id, mixed_launcher, mixed_architecture
        )

        runtime_base = json.loads(
            EXAMPLES[0][0].read_text(encoding="utf-8")
        )
        runtime_negatives = {
            "missing": lambda value: value["portmaster"].pop("runtime"),
            "null": lambda value: value["portmaster"].update({"runtime": None}),
            "string": lambda value: value["portmaster"].update(
                {"runtime": "mono.squashfs"}),
            "duplicate": lambda value: value["portmaster"].update(
                {"runtime": ["mono.squashfs", "mono.squashfs"]}),
        }
        for label, mutate in runtime_negatives.items():
            candidate = json.loads(json.dumps(runtime_base))
            mutate(candidate)
            candidate_path = work / ("runtime-%s.json" % label)
            candidate_path.write_text(
                json.dumps(candidate, ensure_ascii=False), encoding="utf-8"
            )
            run_generator(
                candidate_path, work / ("runtime-%s-output" % label),
                expected=1,
            )

        legacy = json.loads(json.dumps(runtime_base))
        legacy["schema_version"] = 1
        legacy["portmaster"].pop("runtime")
        legacy_path = work / "legacy-v1.json"
        legacy_path.write_text(
            json.dumps(legacy, ensure_ascii=False), encoding="utf-8"
        )
        legacy_output = work / "legacy-v1-output"
        run_generator(legacy_path, legacy_output)
        legacy_metadata = json.loads(
            (legacy_output / "nxexample-aarch64" / "port.json").read_text(
                encoding="utf-8"
            )
        )
        require("runtime" not in legacy_metadata["attr"],
                "legacy nxproject v1 emitted an incompatible empty runtime")

        nonempty = json.loads(json.dumps(runtime_base))
        nonempty["portmaster"]["runtime"] = ["mono.squashfs"]
        nonempty_path = work / "runtime-nonempty.json"
        nonempty_path.write_text(
            json.dumps(nonempty, ensure_ascii=False), encoding="utf-8"
        )
        nonempty_output = work / "runtime-nonempty-output"
        run_generator(nonempty_path, nonempty_output)
        nonempty_metadata = json.loads(
            (nonempty_output / "nxexample-aarch64" / "port.json").read_text(
                encoding="utf-8"
            )
        )
        require(nonempty_metadata["attr"]["runtime"] == ["mono.squashfs"],
                "nonempty PortMaster runtime was not preserved")

        legacy_mixed = json.loads(
            MIXED_EXAMPLE[0].read_text(encoding="utf-8")
        )
        legacy_mixed["schema_version"] = 1
        legacy_mixed["portmaster"].pop("runtime")
        legacy_mixed_path = work / "legacy-v1-mixed.json"
        legacy_mixed_path.write_text(
            json.dumps(legacy_mixed, ensure_ascii=False), encoding="utf-8"
        )
        run_generator(
            legacy_mixed_path, work / "legacy-v1-mixed-output", expected=1
        )

        strict_text = json.dumps(runtime_base, ensure_ascii=False)
        strict_negatives = {
            "duplicate-key": strict_text.replace(
                '"schema_version": 2',
                '"schema_version": 2, "schema_version": 2', 1),
            "nan": strict_text.replace('"schema_version": 2',
                                       '"schema_version": NaN', 1),
            "trailing-comma": strict_text[:-1] + ",}",
            "truncated": strict_text[:-1],
            "bom": "\ufeff" + strict_text,
        }
        for label, payload in strict_negatives.items():
            strict_path = work / ("strict-%s.json" % label)
            strict_path.write_text(payload, encoding="utf-8")
            run_generator(
                strict_path, work / ("strict-%s-output" % label),
                expected=1,
            )

        # The production deployment gate is mode-sensitive even though the
        # two public examples deliberately use mandatory extraction.
        for nxextract_mode in ("auto", "no"):
            mode_project = json.loads(
                EXAMPLES[0][0].read_text(encoding="utf-8")
            )
            mode_project["nxport"]["nxextract"]["mode"] = nxextract_mode
            if nxextract_mode == "no":
                mode_project["nxextract_recipe"] = None
            mode_manifest = work / ("mode-%s.json" % nxextract_mode)
            mode_manifest.write_text(
                json.dumps(mode_project, ensure_ascii=False), encoding="utf-8"
            )
            mode_output = work / ("mode-%s-output" % nxextract_mode)
            run_generator(mode_manifest, mode_output)
            validate_generation(
                mode_output, "nxexample-aarch64",
                "NXExample AArch64.sh", "aarch64"
            )

        victim = work / "victim"
        victim.mkdir()
        symlink_output = work / "output-link"
        symlink_output.symlink_to(victim, target_is_directory=True)
        run_generator(EXAMPLES[0][0], symlink_output, expected=1)
        require(list(victim.iterdir()) == [],
                "generator wrote through an output symlink")

        hostile = json.loads(EXAMPLES[0][0].read_text(encoding="utf-8"))
        hostile["nxport"]["title"] = "Private 192.0.2.10"
        hostile_path = work / "hostile.json"
        hostile_path.write_text(
            json.dumps(hostile, ensure_ascii=False), encoding="utf-8"
        )
        run_generator(hostile_path, work / "hostile-output", expected=1)

        missing_recipe = json.loads(
            EXAMPLES[0][0].read_text(encoding="utf-8")
        )
        missing_recipe["nxextract_recipe"] = None
        missing_path = work / "missing-recipe.json"
        missing_path.write_text(
            json.dumps(missing_recipe), encoding="utf-8"
        )
        run_generator(missing_path, work / "missing-output", expected=1)

        # V3 owner_data: schema 3 with the canonical block generates and
        # customizes the marker; a non-canonical directory and a recipe whose
        # search_dirs demote gamedata both fail closed.
        v3_project = json.loads(EXAMPLES[0][0].read_text(encoding="utf-8"))
        v3_project["schema_version"] = 3
        v3_project["owner_data"] = {
            "directory": "gamedata",
            "formats": ["APK", "XAPK"],
            "reference": {"version": "9.9.9-test"},
        }
        v3_project["language_access"] = {
            "mode": "adapter",
            "supported": ["en", "pt-BR"],
            "fallback": "en",
            "sinks": ["unity-i2loc"],
        }
        v3_project["controls"] = {
            "actions": [
                {"id": "example.accept", "kind": "button",
                 "sinks": ["adapter.menu.accept"]},
                {"id": "example.back", "kind": "button",
                 "sinks": ["adapter.menu.back"]},
                {"id": "example.drive", "kind": "vector",
                 "sinks": ["adapter.game.drive"]},
                {"id": "example.pause", "kind": "button",
                 "sinks": ["adapter.game.pause"]},
            ],
            "contexts": {
                "menu": {
                    "A": "example.accept", "B": "example.back",
                },
                "gameplay": {
                    "LEFT_STICK": "example.drive",
                    "START": "example.pause",
                },
            },
        }
        v3_project["graphics"] = {"uses_gl": False}
        v3_path = work / "v3-owner-data.json"
        v3_path.write_text(
            json.dumps(v3_project, ensure_ascii=False), encoding="utf-8"
        )
        v3_output = work / "v3-owner-data-output"
        run_generator(v3_path, v3_output)
        validate_generation(
            v3_output, "nxexample-aarch64", "NXExample AArch64.sh", "aarch64"
        )
        v3_marker = (
            v3_output / "nxexample-aarch64" / "gamedata" / "README.txt"
        ).read_text(encoding="utf-8")
        require("APK/XAPK" in v3_marker and "9.9.9-test" in v3_marker,
                "owner_data block did not customize the marker")
        # V3-SETTINGS-01 (blocker 5): the generated adapter-contract carries
        # the declared language_access, never null.
        v3_adapter = json.loads((
            v3_output / "nxexample-aarch64" / "adapter" /
            "adapter-contract.json"
        ).read_text(encoding="utf-8"))
        require(v3_adapter.get("language_access") == v3_project["language_access"],
                "adapter-contract lost the declared language_access")
        require(v3_adapter.get("input", {}).get("actions") ==
                v3_project["controls"]["actions"],
                "adapter-contract lost the action/sink contract")
        require(v3_adapter.get("input", {}).get("contexts") ==
                v3_project["controls"]["contexts"],
                "adapter-contract lost the context bindings")
        require(v3_adapter.get("input", {}).get("mapping_authority") ==
                "sdl-portmaster-complete-first",
                "adapter-contract lost SDL mapping authority")
        # V3-PROMOTION-01: opt-in. Sem `promotion`, o scaffold e o default
        # (release_ready False). Com `promotion`, o gerador COPIA o
        # adapter-contract real do port e emite os claims promovidos, de forma
        # reproduzivel; sem o bloco, nada muda.
        default_gen = json.loads((
            v3_output / "nxexample-aarch64" / "GENERATION.json"
        ).read_text(encoding="utf-8"))
        require(default_gen["claims"]["release_ready"] is False and
                v3_adapter.get("release_ready") is not True,
                "default (sem promotion) deixou de ser scaffold")
        promo_project = dict(v3_project)
        promo_project["promotion"] = {
            "adapter_contract":
                "framework/nxgenerator/examples/promoted-adapter.example.json",
            "claims": {"release_ready": True,
                       "physical_support_proven": False,
                       "adapter_lifecycle_implemented": True},
        }
        promo_path = work / "v3-promoted.json"
        promo_path.write_text(
            json.dumps(promo_project, ensure_ascii=False), encoding="utf-8"
        )
        promo_out = work / "v3-promoted-output"
        run_generator(promo_path, promo_out)
        promo_adapter = json.loads((
            promo_out / "nxexample-aarch64" / "adapter" /
            "adapter-contract.json").read_text(encoding="utf-8"))
        require(promo_adapter.get("release_ready") is True and
                promo_adapter.get("status") == "implemented_release",
                "promotion nao copiou o adapter-contract real")
        promo_gen = json.loads((
            promo_out / "nxexample-aarch64" / "GENERATION.json"
        ).read_text(encoding="utf-8"))
        require(promo_gen["claims"]["release_ready"] is True and
                promo_gen["claims"]["physical_support_proven"] is False and
                promo_gen["claims"]["adapter_lifecycle_implemented"] is True,
                "promotion nao emitiu os claims promovidos")
        # Reprodutivel: regenerar de novo produz o mesmo adapter.
        promo_out2 = work / "v3-promoted-output-2"
        run_generator(promo_path, promo_out2)
        require((promo_out / "nxexample-aarch64" / "adapter" /
                 "adapter-contract.json").read_bytes() ==
                (promo_out2 / "nxexample-aarch64" / "adapter" /
                 "adapter-contract.json").read_bytes(),
                "promotion nao e reproduzivel")
        promotion_negatives = {}

        old_schema_promotion = json.loads(json.dumps(promo_project))
        old_schema_promotion["schema_version"] = 2
        old_schema_promotion.pop("language_access")
        old_schema_promotion.pop("controls")
        old_schema_promotion.pop("graphics")
        promotion_negatives["old-schema"] = old_schema_promotion

        unsafe_path_promotion = json.loads(json.dumps(promo_project))
        unsafe_path_promotion["promotion"]["adapter_contract"] = \
            "../promoted-adapter.json"
        promotion_negatives["unsafe-path"] = unsafe_path_promotion

        contradictory_claim = json.loads(json.dumps(promo_project))
        contradictory_claim["promotion"]["claims"]["release_ready"] = False
        promotion_negatives["contradictory-claim"] = contradictory_claim

        mismatched_contract = json.loads(json.dumps(promo_project))
        mismatched_contract["controls"]["contexts"]["menu"]["A"] = \
            "example.back"
        promotion_negatives["contract-mismatch"] = mismatched_contract

        for label, candidate in promotion_negatives.items():
            candidate_path = work / ("v3-promotion-%s.json" % label)
            candidate_path.write_text(
                json.dumps(candidate, ensure_ascii=False), encoding="utf-8"
            )
            run_generator(
                candidate_path,
                work / ("v3-promotion-%s-output" % label),
                expected=1,
            )
        v3_gptk = (v3_output / "nxexample-aarch64" / "defaults" /
                   "NEXTOSCONTROLLERS.gptk").read_text(encoding="utf-8")
        require("A = example.accept" in v3_gptk and
                "LEFT_STICK = example.drive" in v3_gptk and
                "player.primary" not in v3_gptk,
                "V3 GPTK was not rendered only from declared actions")

        wrong_dir = json.loads(v3_path.read_text(encoding="utf-8"))
        wrong_dir["owner_data"]["directory"] = "meusdados"
        wrong_dir_path = work / "v3-wrong-dir.json"
        wrong_dir_path.write_text(json.dumps(wrong_dir), encoding="utf-8")
        run_generator(wrong_dir_path, work / "v3-wrong-dir-output", expected=1)

        # V3 project WITHOUT language_access must fail closed.
        no_lang = json.loads(v3_path.read_text(encoding="utf-8"))
        no_lang.pop("language_access")
        no_lang_path = work / "v3-no-language.json"
        no_lang_path.write_text(json.dumps(no_lang), encoding="utf-8")
        run_generator(no_lang_path, work / "v3-no-language-output", expected=1)

        # V3-CONTROLLERS-01: silence, unknown actions, sinkless actions,
        # duplicate action ids and invalid cursor ownership all fail closed.
        no_controls = json.loads(v3_path.read_text(encoding="utf-8"))
        no_controls.pop("controls")
        no_controls_path = work / "v3-no-controls.json"
        no_controls_path.write_text(json.dumps(no_controls), encoding="utf-8")
        run_generator(no_controls_path, work / "v3-no-controls-output",
                      expected=1)
        control_negatives = []
        candidate = json.loads(v3_path.read_text(encoding="utf-8"))
        candidate["controls"]["contexts"]["menu"]["A"] = "unknown.action"
        control_negatives.append(candidate)
        candidate = json.loads(v3_path.read_text(encoding="utf-8"))
        candidate["controls"]["actions"][0]["sinks"] = []
        control_negatives.append(candidate)
        candidate = json.loads(v3_path.read_text(encoding="utf-8"))
        candidate["controls"]["actions"].append(
            dict(candidate["controls"]["actions"][0]))
        control_negatives.append(candidate)
        candidate = json.loads(v3_path.read_text(encoding="utf-8"))
        candidate["controls"]["contexts"]["cursor"] = {
            "A": "example.accept", "RIGHT_STICK": "example.drive",
            "R3": "example.back",
        }
        control_negatives.append(candidate)
        candidate = json.loads(v3_path.read_text(encoding="utf-8"))
        candidate["controls"]["contexts"]["gameplay"]["A"] = "example.drive"
        control_negatives.append(candidate)
        for index, candidate in enumerate(control_negatives):
            candidate_path = work / ("v3-controls-negative-%d.json" % index)
            candidate_path.write_text(json.dumps(candidate), encoding="utf-8")
            run_generator(candidate_path,
                          work / ("v3-controls-negative-%d-output" % index),
                          expected=1)

        # The five modes each generate; incoherent fallback/sinks fail.
        for mode, supported, fallback, sinks, expected in (
            ("native-menu", ["en"], "en", [], 0),
            ("first-run-native", ["en", "pt-BR"], "en", ["unity"], 0),
            ("adapter", ["en", "pt-BR"], "en", ["unity"], 0),
            ("single-language", ["pt-BR"], "pt-BR", [], 0),
            ("none", [], "", [], 0),
            ("adapter", ["en"], "ru", ["unity"], 1),      # fallback not in supported
            ("adapter", ["en"], "en", [], 1),             # adapter needs a sink
        ):
            cand = json.loads(v3_path.read_text(encoding="utf-8"))
            cand["language_access"] = {
                "mode": mode, "supported": supported,
                "fallback": fallback, "sinks": sinks,
            }
            cp = work / ("v3-lang-%s-%d.json" % (mode, expected))
            cp.write_text(json.dumps(cand), encoding="utf-8")
            run_generator(cp, work / ("v3-lang-%s-%d-out" % (mode, expected)),
                          expected=expected)

        # V3-GRAPHICS-02 (item 3): the optional graphics context contract
        # round-trips into adapter-contract.json (with the vendored adapter
        # recorded), and every incoherent declaration fails closed.
        gfx_ok = json.loads(v3_path.read_text(encoding="utf-8"))
        gfx_ok["graphics"] = {
            "uses_gl": True,
            "api": "gles",
            "profile": "es",
            "version": "2.0",
            "version_policy": "minimum",
            "shader_dialect": "essl100",
            "drawable_ready_timeout_ms": 5000,
            "adopt_single_channel": True,
        }
        gfx_ok_path = work / "v3-graphics-ok.json"
        gfx_ok_path.write_text(json.dumps(gfx_ok, ensure_ascii=False),
                               encoding="utf-8")
        gfx_out = work / "v3-graphics-ok-output"
        run_generator(gfx_ok_path, gfx_out)
        validate_generation(
            gfx_out, "nxexample-aarch64", "NXExample AArch64.sh", "aarch64"
        )
        gfx_adapter = json.loads((
            gfx_out / "nxexample-aarch64" / "adapter" / "adapter-contract.json"
        ).read_text(encoding="utf-8"))
        gfx_block = gfx_adapter.get("graphics")
        require(isinstance(gfx_block, dict) and gfx_block.get("uses_gl") is True,
                "adapter-contract lost the declared graphics contract")
        require(gfx_block.get("api") == "gles" and
                gfx_block.get("shader_dialect") == "essl100" and
                gfx_block.get("version_policy") == "minimum" and
                gfx_block.get("adapter") == "nxgl_graphics_contract_adapter",
                "graphics contract did not normalise with the vendored adapter")
        # V4-GRAPHICS-04: absence of the opt-in never invents the field.
        require("evidence_boundary" not in gfx_block,
                "an undeclared evidence_boundary leaked into adapter-contract")

        # V4-GRAPHICS-04: the declared post-first-present boundary
        # round-trips into adapter-contract.json, exactly as declared.
        gfx_pfp = json.loads(gfx_ok_path.read_text(encoding="utf-8"))
        gfx_pfp["graphics"]["evidence_boundary"] = "post-first-present"
        gfx_pfp_path = work / "v4-graphics-pfp.json"
        gfx_pfp_path.write_text(json.dumps(gfx_pfp, ensure_ascii=False),
                                encoding="utf-8")
        gfx_pfp_out = work / "v4-graphics-pfp-output"
        run_generator(gfx_pfp_path, gfx_pfp_out)
        pfp_adapter = json.loads((
            gfx_pfp_out / "nxexample-aarch64" / "adapter" /
            "adapter-contract.json"
        ).read_text(encoding="utf-8"))
        require(pfp_adapter.get("graphics", {}).get("evidence_boundary") ==
                "post-first-present",
                "the declared post-first-present boundary was lost")

        # V4-CONTROLLERS-03/C3: the controller_profiles opt-in round-trips
        # into adapter-contract.json; absence normalizes to disabled.
        cp_prj = json.loads(v3_path.read_text(encoding="utf-8"))
        cp_prj.setdefault("controls", {})
        cp_prj["controls"]["controller_profiles"] = {
            "enabled": True, "bundle": "controllers.nxb",
            "sha256": "ab" * 32}
        cp_path = work / "v4-controller-profiles.json"
        cp_path.write_text(json.dumps(cp_prj, ensure_ascii=False),
                           encoding="utf-8")
        cp_out = work / "v4-controller-profiles-output"
        run_generator(cp_path, cp_out)
        cp_adapter = json.loads((
            cp_out / "nxexample-aarch64" / "adapter" / "adapter-contract.json"
        ).read_text(encoding="utf-8"))
        require(cp_adapter.get("input_controller_profiles") == {
                    "enabled": True, "bundle": "controllers.nxb",
                    "sha256": "ab" * 32},
                "the controller_profiles pin was lost or rewritten")
        # Mission 114A: WITHOUT a declaration the field must not exist at
        # all -- appending a disabled block to every regenerated contract
        # would break the byte preservation promised to published ports.
        # tests/test_c3_bytes_preserved.py proves the literal equality
        # against a real pre-C3 golden.
        require("input_controller_profiles" not in gfx_adapter,
                "absence of controller_profiles must leave no field behind")
        for bad_block, why in (
            ({"enabled": True, "bundle": "", "sha256": "ab" * 32},
             "enabled without a bundle name"),
            ({"enabled": True, "bundle": "sub/dir.nxb", "sha256": "ab" * 32},
             "a bundle with a path separator"),
            ({"enabled": True, "bundle": "controllers.nxb", "sha256": ""},
             "enabled without a pin"),
            ({"enabled": False, "bundle": "controllers.nxb",
              "sha256": "ab" * 32}, "disabled with a pin"),
            ({"enabled": True, "bundle": "controllers.nxb"},
             "a missing digest field"),
        ):
            cand = json.loads(v3_path.read_text(encoding="utf-8"))
            cand.setdefault("controls", {})
            cand["controls"]["controller_profiles"] = bad_block
            cand_path = work / ("v4-cp-neg-%s.json" % abs(hash(why)))
            cand_path.write_text(json.dumps(cand), encoding="utf-8")
            run_generator(cand_path,
                          work / ("v4-cp-neg-%s-out" % abs(hash(why))),
                          expected=1)

        # A non-GL port declares uses_gl false and nothing else.
        gfx_nogl = json.loads(v3_path.read_text(encoding="utf-8"))
        gfx_nogl["graphics"] = {"uses_gl": False}
        gfx_nogl_path = work / "v3-graphics-nogl.json"
        gfx_nogl_path.write_text(json.dumps(gfx_nogl), encoding="utf-8")
        run_generator(gfx_nogl_path, work / "v3-graphics-nogl-output")

        # Every incoherent graphics declaration fails closed.
        gfx_negatives = (
            {"uses_gl": True},  # GL port with no contract fields
            {"uses_gl": False, "api": "gles"},  # non-GL must be bare
            {"uses_gl": True, "api": "gles", "profile": "core",
             "version": "2.0", "version_policy": "exact",
             "shader_dialect": "essl100"},  # gles must be profile es
            {"uses_gl": True, "api": "gles", "profile": "es",
             "version": "3.0", "version_policy": "exact",
             "shader_dialect": "glsl-any"},  # gles needs an ESSL dialect
            {"uses_gl": True, "api": "gl", "profile": "compat",
             "version": "3.1", "version_policy": "exact",
             "shader_dialect": "essl300"},  # desktop GL needs glsl-any
            {"uses_gl": True, "api": "gles", "profile": "es",
             "version": "3.0", "version_policy": "range",
             "shader_dialect": "essl300"},  # range without version_max
            {"uses_gl": True, "api": "gles", "profile": "es",
             "version": "3.0", "version_policy": "range", "version_max": "2.0",
             "shader_dialect": "essl300"},  # version_max < version
            {"uses_gl": True, "api": "gles", "profile": "es",
             "version": "3.0", "version_policy": "exact", "version_max": "3.1",
             "shader_dialect": "essl300"},  # version_max on non-range
            {"uses_gl": True, "api": "zzz", "profile": "es",
             "version": "2.0", "version_policy": "exact",
             "shader_dialect": "essl100"},  # unknown api
            {"uses_gl": True, "api": "gles", "profile": "es",
             "version": "2", "version_policy": "exact",
             "shader_dialect": "essl100"},  # malformed version
            {"uses_gl": True, "api": "gles", "profile": "es",
             "version": "2.0", "version_policy": "exact",
             "shader_dialect": "essl100", "bogus": 1},  # unknown field
            {"uses_gl": True, "api": "gles", "profile": "es",
             "version": "2.0", "version_policy": "exact",
             "shader_dialect": "essl100",
             "evidence_boundary": "pre-present"},  # unknown boundary value
            {"uses_gl": False,
             "evidence_boundary": "post-first-present"},  # non-GL must be bare
        )
        for idx, block in enumerate(gfx_negatives):
            cand = json.loads(v3_path.read_text(encoding="utf-8"))
            cand["graphics"] = block
            cp = work / ("v3-graphics-neg-%d.json" % idx)
            cp.write_text(json.dumps(cand), encoding="utf-8")
            run_generator(cp, work / ("v3-graphics-neg-%d-out" % idx),
                          expected=1)

        # graphics on a schema_version 2 project fails closed.
        gfx_v2 = json.loads(v3_path.read_text(encoding="utf-8"))
        gfx_v2["schema_version"] = 2
        gfx_v2.pop("language_access", None)
        gfx_v2.pop("owner_data", None)
        gfx_v2["graphics"] = {"uses_gl": False}
        gfx_v2_path = work / "v3-graphics-v2.json"
        gfx_v2_path.write_text(json.dumps(gfx_v2), encoding="utf-8")
        run_generator(gfx_v2_path, work / "v3-graphics-v2-output", expected=1)

        v2_with_owner = json.loads(v3_path.read_text(encoding="utf-8"))
        v2_with_owner["schema_version"] = 2
        v2_with_owner.pop("language_access", None)
        v2_owner_path = work / "v2-owner-data.json"
        v2_owner_path.write_text(json.dumps(v2_with_owner), encoding="utf-8")
        run_generator(v2_owner_path, work / "v2-owner-output", expected=1)

        recipe_bytes = json.loads(
            (REPOSITORY / "suportando_outros_devices" / "extrator-universal" /
             "examples" / "recipe-minimal.json").read_text(encoding="utf-8")
        )
        recipe_bytes.setdefault("input", {})["search_dirs"] = [".", "gamedata"]
        divergent_root = work / "divergent-root"
        divergent_root.mkdir()
        (divergent_root / "extractor.json").write_text(
            json.dumps(recipe_bytes), encoding="utf-8"
        )
        shutil.copyfile(
            REPOSITORY / "LICENSE", divergent_root / "LICENSE"
        )
        divergent_project = json.loads(
            EXAMPLES[0][0].read_text(encoding="utf-8")
        )
        divergent_project["nxextract_recipe"] = "extractor.json"
        divergent_project["license"]["source"] = "LICENSE"
        divergent_path = work / "divergent-search-dirs.json"
        divergent_path.write_text(
            json.dumps(divergent_project), encoding="utf-8"
        )
        run_generator(
            divergent_path, work / "divergent-output", expected=1,
            source_root=divergent_root,
        )

    print("M19 nxgenerator tests passed: abis=2 mixed_roles=1 deterministic=1 "
          "nxbootstrap=%s deployment=1 tamper_rejections=19 "
          "nxextract_release_tamper_rejections=7 no_external_stat=1 "
          "standalone_source_root=1 "
          "nxextract_modes=3 metadata=1 runtime_empty_omitted=1 "
          "runtime_nonempty_preserved=1 adapter_unimplemented=1 "
          "promotion=1 promotion_negatives=4 promotion_fixture_physical=0 "
          "portmaster_real_cycles=2 runtime_negatives=4 legacy_v1=1 "
          "legacy_mixed_rejected=1 "
          "strict_json_negatives=5 "
          "support_claims=0 apk_variant_policy=package-abi-structure "
          "real_recipe_regressions=3"
          % EXPECTED_BOOTSTRAP_VERSION)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError, ValueError, KeyError) as error:
        print("M19 nxgenerator tests failed: %s" % error, file=sys.stderr)
        raise SystemExit(1)
