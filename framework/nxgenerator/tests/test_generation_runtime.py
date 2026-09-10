#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Focused nxproject/nxport schema-v3 generation-runtime integration gate."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import zipfile
import sys
import subprocess
import tempfile
from xml.etree import ElementTree


REPOSITORY = Path(__file__).resolve().parents[3]
ROOT = REPOSITORY / "framework" / "nxgenerator"
TOOL = ROOT / "nxgenerator.py"
SCHEMA_V3 = ROOT / "schema" / "nxproject-v3.schema.json"
VIDEO_PROVIDER_GATE = ROOT / "tests" / "test_video_provider_contract.py"
EXPECTED_GENERATOR_VERSION = "0.4.5"


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_tool():
    specification = importlib.util.spec_from_file_location(
        "nxgenerator_generation_runtime_under_test", TOOL
    )
    require(specification is not None and specification.loader is not None,
            "cannot load nxgenerator")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def run_video_provider_gate():
    """Run the required sibling gate without changing this gate's stdout."""
    specification = importlib.util.spec_from_file_location(
        "nxgenerator_video_provider_required_gate", VIDEO_PROVIDER_GATE
    )
    require(specification is not None and specification.loader is not None,
            "cannot load required video/provider gate")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    captured = io.StringIO()
    try:
        with contextlib.redirect_stdout(captured), \
                contextlib.redirect_stderr(captured):
            result = module.main()
    except (module.GateError, OSError, ValueError, KeyError) as error:
        raise GateError("required video/provider gate failed: %s" % error) \
            from error
    require(result is None, "required video/provider gate returned a status")
    require(captured.getvalue() == (
        "nxgenerator 0.4.5 video/provider gate passed: "
        "nxbootstrap=0.8.4 omitted=2 roundtrip=2 launcher=2 "
        "unrelated_bytes=15 negatives=8 no_device=1\n"
    ), "required video/provider gate emitted an unexpected result")


def sha256(payload):
    return hashlib.sha256(payload).hexdigest()


def write_runtime(root):
    payloads = {
        "bin/fixture-nextos": (b"#!/bin/sh\nexit 0\n", 0o755),
        "lib/libfixture.so": (b"fixture-private-library\n", 0o644),
        "lib/runtime/System.Private.CoreLib.dll": (
            b"fixture-managed-runtime-data\n", 0o644),
        "port-env.sh": (b"#!/bin/sh\nexport FIXTURE_RUNTIME=1\n", 0o644),
    }
    for relative, (payload, mode) in payloads.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        os.chmod(target, mode)
    return payloads


def add_nxextract_runtime(tool, runtime_root, document):
    """Turn the generation fixture into the real schema3+NXExtract shape."""
    recipe_source = tool.NXEXTRACT_ROOT / "examples" / "recipe-minimal.json"
    recipe_payload = recipe_source.read_bytes()
    (runtime_root / "extractor.json").write_bytes(recipe_payload)
    os.chmod(runtime_root / "extractor.json", 0o644)

    role_by_target = {
        "nxextract.py": "nxextract-engine",
        "run-extractor.sh": "nxextract-runner",
        "nxextract-runtime-env.sh": "nxextract-runtime-env",
    }
    records = [{
        "role": "nxextract-recipe",
        "path": "extractor.json",
        "mode": "0644",
        "sha256": sha256(recipe_payload),
    }]
    for source_name, target_name, mode in tool.NXEXTRACT_COMMON_FILES:
        payload = (tool.NXEXTRACT_ROOT / source_name).read_bytes()
        target = runtime_root / "nxextract" / target_name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        os.chmod(target, mode)
        records.append({
            "role": role_by_target[target_name],
            "path": "nxextract/" + target_name,
            "mode": "%04o" % mode,
            "sha256": sha256(payload),
        })

    ui_state = tool.nxextract_source_state("aarch64")
    ui_payload = (
        tool.NXEXTRACT_ROOT / ui_state["artifact"]["path"]
    ).read_bytes()
    ui_target = runtime_root / "nxextract" / "nxextract-ui"
    ui_target.write_bytes(ui_payload)
    os.chmod(ui_target, 0o755)
    records.append({
        "role": "nxextract-ui",
        "path": "nxextract/nxextract-ui",
        "mode": "0755",
        "sha256": sha256(ui_payload),
    })

    document["nxextract_recipe"] = "runtime/extractor.json"
    document["nxport"]["nxextract"]["mode"] = "yes"
    document["nxport"]["generation_runtime"].extend(records)
    return records


def project_document(payloads):
    runtime = [
        {
            "role": "executable",
            "path": "bin/fixture-nextos",
            "mode": "0755",
            "sha256": sha256(payloads["bin/fixture-nextos"][0]),
        },
        {
            "role": "private-library",
            "path": "lib/libfixture.so",
            "mode": "0644",
            "sha256": sha256(payloads["lib/libfixture.so"][0]),
        },
        {
            "role": "runtime-data",
            "path": "lib/runtime/System.Private.CoreLib.dll",
            "mode": "0644",
            "sha256": sha256(
                payloads["lib/runtime/System.Private.CoreLib.dll"][0]
            ),
        },
        {
            "role": "runtime-hook",
            "path": "port-env.sh",
            "mode": "0644",
            "sha256": sha256(payloads["port-env.sh"][0]),
        },
    ]
    return {
        "schema_version": 3,
        "runtime_root": "runtime",
        "nxport": {
            "schema_version": 3,
            "id": "nxgenerator-runtime-fixture",
            "title": "NXGenerator Runtime & Fixture",
            "launcher_name": "NXGenerator Runtime Fixture.sh",
            "architecture": "aarch64",
            "executable": "bin/fixture-nextos",
            "argument_mode": "game-dir-and-passthrough",
            "home_mode": "preserve",
            "nxextract": {"mode": "no", "version": "1.3.0"},
            "required_files": ["bin/fixture-nextos", "port-env.sh"],
            "private_library_paths": ["lib"],
            "prepare_script": "",
            "required_capabilities": ["host.portmaster"],
            "enabled_quirks": [],
            "runtime_report": "log-and-logo",
            "generation_runtime": runtime,
        },
        "nxextract_recipe": None,
        "adapter": {"skeleton": "contract-only"},
        "portmaster": {
            "metadata_version": 4,
            "min_glibc": "2.17",
            "runtime": [],
        },
        "license": {"spdx_id": "GPL-3.0-only", "source": "LICENSE"},
        "documentation": {"status": "scaffold", "proven_support": []},
        "language_access": {
            "mode": "none", "supported": [], "fallback": "", "sinks": [],
        },
        "controls": {
            "actions": [
                {
                    "id": "fixture.accept", "kind": "button",
                    "sinks": ["adapter.menu.accept"],
                },
                {
                    "id": "fixture.back", "kind": "button",
                    "sinks": ["adapter.menu.back"],
                },
                {
                    "id": "fixture.pause", "kind": "button",
                    "sinks": ["adapter.game.pause"],
                },
            ],
            "contexts": {
                "menu": {"A": "fixture.accept", "B": "fixture.back"},
                "gameplay": {"START": "fixture.pause"},
            },
        },
        "graphics": {"uses_gl": False},
    }


def expect_project_error(tool, document, output, source_root, token):
    try:
        tool.generate_project(document, output, source_root)
    except tool.ProjectError as error:
        require(token in str(error),
                "unexpected error for %s: %s" % (token, error))
        require(not output.exists(),
                "failed generation published a partial output")
        return
    raise GateError("invalid project passed: %s" % token)


def assert_artifact_inventory_order(tool, work):
    stage = work / "artifact-order"
    payloads = {
        "fixture/Z-last-uppercase.txt": (b"uppercase\n", 0o644),
        "fixture/a-first-lowercase.txt": (b"lowercase\n", 0o644),
        "fixture/nxextract/run-extractor.sh": (b"#!/bin/sh\n", 0o755),
        "fixture/nxextract-version.txt": (b"1.3.0\n", 0o644),
        "fixture/prefix/member.txt": (b"slash\n", 0o644),
        "fixture/prefix-file.txt": (b"hyphen\n", 0o644),
        "fixture/prefix.member.txt": (b"dot\n", 0o644),
        "fixture/prefix0.txt": (b"digit\n", 0o644),
    }
    for relative, (payload, mode) in payloads.items():
        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        os.chmod(target, mode)
    receipt = stage / "fixture" / "GENERATION.json"
    receipt.write_text("{}\n", encoding="utf-8")
    os.chmod(receipt, 0o644)

    records = tool.artifact_inventory(stage)
    expected = [
        {
            "path": relative,
            "mode": "%04o" % payloads[relative][1],
            "sha256": sha256(payloads[relative][0]),
        }
        for relative in sorted(payloads)
    ]
    require(records == expected,
            "artifact inventory is not ordered by its POSIX logical path")
    paths = [record["path"] for record in records]
    require(paths.index("fixture/nxextract-version.txt") <
            paths.index("fixture/nxextract/run-extractor.sh"),
            "hyphen/slash prefix boundary regressed")
    require(paths.index("fixture/prefix-file.txt") <
            paths.index("fixture/prefix.member.txt") <
            paths.index("fixture/prefix/member.txt") <
            paths.index("fixture/prefix0.txt"),
            "hyphen/dot/slash/digit ordering boundary regressed")


def assert_generation_v2(tool, output, payloads, source_root):
    port_id = "nxgenerator-runtime-fixture"
    port = output / port_id
    nxport = json.loads((port / "nxport.json").read_text(encoding="utf-8"))
    require(nxport["schema_version"] == 3 and
            len(nxport["generation_runtime"]) == 5,
            "schema-v3 nxport did not retain the complete runtime closure")
    require(nxport["generation_runtime"][-1]["role"] == "nxsplash",
            "canonical nxsplash was not appended to generation_runtime")
    generation_id = tool.BOOTSTRAP_GENERATOR.generation_identity(nxport)
    generation = port / ".nxruntime" / "generations" / generation_id
    require((generation / "format").read_bytes() ==
            b"nxruntime-generation-v2\n",
            "schema-v3 output is not a generation-v2 closure")
    require((generation / "commit").read_text(encoding="ascii") ==
            generation_id + "\n", "generation-v2 commit is stale")
    require((generation / "identity-runtime.v2").read_text(encoding="utf-8") ==
            tool.BOOTSTRAP_GENERATOR.generation_runtime_records(nxport),
            "generation-v2 identity records differ from nxport")
    for relative, (payload, mode) in payloads.items():
        for candidate in (
            port / relative,
            generation / "files" / "runtime" / relative,
        ):
            require(candidate.is_file() and not candidate.is_symlink(),
                    "runtime member is absent: %s" % candidate)
            require(candidate.read_bytes() == payload and
                    stat.S_IMODE(candidate.stat().st_mode) == mode,
                    "runtime member bytes/mode changed: %s" % candidate)
    launcher = output / "NXGenerator Runtime Fixture.sh"
    require("NXBOOTSTRAP_GENERATION_FORMAT=2" in
            launcher.read_text(encoding="utf-8"),
            "launcher did not opt into generation-v2")
    receipt = json.loads((port / "GENERATION.json").read_text(encoding="utf-8"))
    require(receipt["generator"] == {
        "name": "nxgenerator", "version": EXPECTED_GENERATOR_VERSION
    }, "generation receipt did not pin nxgenerator " +
        EXPECTED_GENERATOR_VERSION)
    require(receipt["generation_id"] == generation_id,
            "nxgenerator and nxbootstrap generation identities diverged")
    gameinfo_path = port / "gameinfo.xml"
    expected_gameinfo = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<gameList>\n'
        '  <game>\n'
        '    <path>./NXGenerator Runtime Fixture.sh</path>\n'
        '    <name>NXGenerator Runtime &amp; Fixture</name>\n'
        '  </game>\n'
        '</gameList>\n'
    ).encode("utf-8")
    require(gameinfo_path.read_bytes() == expected_gameinfo,
            "gameinfo.xml bytes are not canonical")
    gameinfo = ElementTree.fromstring(expected_gameinfo)
    require(gameinfo.tag == "gameList" and len(gameinfo) == 1 and
            gameinfo[0].findtext("path") ==
            "./NXGenerator Runtime Fixture.sh" and
            gameinfo[0].findtext("name") ==
            "NXGenerator Runtime & Fixture",
            "gameinfo.xml XML does not bind launcher/title")
    artifact = next(
        (item for item in receipt["artifacts"]
         if item["path"] == port.name + "/gameinfo.xml"), None
    )
    require(artifact == {
        "path": port.name + "/gameinfo.xml",
        "mode": "0644",
        "sha256": sha256(expected_gameinfo),
    }, "GENERATION.json does not bind canonical gameinfo.xml")
    all_bytes = b"".join(
        path.read_bytes() for path in sorted(output.rglob("*")) if path.is_file()
    )
    require(str(source_root).encode("utf-8") not in all_bytes,
            "host runtime_root leaked into generated bytes")


def assert_schema2_regression(tool, document, output, source_root):
    legacy = json.loads(json.dumps(document))
    legacy["schema_version"] = 2
    legacy.pop("runtime_root")
    legacy.pop("language_access")
    legacy.pop("controls")
    legacy.pop("graphics")
    nxport = legacy["nxport"]
    nxport["schema_version"] = 2
    nxport.pop("generation_runtime")
    nxport["required_files"] = [nxport["executable"]]
    nxport["private_library_paths"] = []
    tool.generate_project(legacy, output, source_root)
    port = output / nxport["id"]
    emitted = json.loads((port / "nxport.json").read_text(encoding="utf-8"))
    require(emitted["schema_version"] == 2 and
            "generation_runtime" not in emitted,
            "schema-v2 project silently opted into generation_runtime")
    generation_id = tool.BOOTSTRAP_GENERATOR.generation_identity(emitted)
    generation = port / ".nxruntime" / "generations" / generation_id
    manifest = json.loads(
        (generation / "manifest.json").read_text(encoding="utf-8")
    )
    require(manifest["schema"] == "nxruntime-generation-v1" and
            manifest["schema_version"] == 1,
            "schema-v2 regression lost generation-v1 semantics")
    for v2_only in ("format", "components.v2", "identity.json",
                    "identity-runtime.v2"):
        require(not (generation / v2_only).exists(),
                "schema-v2 output gained generation-v2 metadata")


PORTMASTER_CYCLE = (REPOSITORY / "framework" / "portmaster" / "tools" /
                    "harbourmaster-cycle.py")


def assert_real_portmaster_cycle_with_seed(output, port_id, work):
    """REPACK-01 gate 3/7: the REAL HarbourMaster cycle over a SEEDED port.

    The two existing real cycles run on schema-2 examples, which carry no
    generation_runtime and therefore no `.nxb` seed at all. A V4 port is a
    different shape: it ships a visible seed next to nxport.json. Autoinstall,
    discovery after a restart, uninstall and reinstall had never seen it, so a
    cycle that rejected the seed as an unexpected member would have shipped
    unnoticed.
    """
    port = output / port_id
    launchers = sorted(item for item in output.iterdir()
                       if item.is_file() and item.suffix == ".sh")
    require(len(launchers) == 1,
            "the generated tree does not carry exactly one launcher")
    seeds = sorted(port.glob("nxruntime-*.nxb"))
    require(len(seeds) == 1,
            "the schema-3 port does not carry exactly one visible seed")
    seed_bytes = seeds[0].read_bytes()

    archive = work / (port_id + "-seeded-portmaster.zip")
    with zipfile.ZipFile(str(archive), "w", zipfile.ZIP_DEFLATED) as bundle:
        bundle.write(str(launchers[0]), launchers[0].name)
        for item in sorted(port.rglob("*")):
            if item.is_dir():
                continue
            bundle.write(str(item), port_id + "/" +
                         item.relative_to(port).as_posix())
    completed = subprocess.run(
        [sys.executable, "-B", str(PORTMASTER_CYCLE), str(archive)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        timeout=900,
    )
    require(completed.returncode == 0,
            "the real HarbourMaster cycle refused a seeded V4 port: %s"
            % (completed.stdout.strip().splitlines()[-1:] or ["no output"])[0])
    require("install=1" in completed.stdout and
            "uninstall=1" in completed.stdout and
            "reinstall=1" in completed.stdout,
            "the seeded cycle did not complete install/uninstall/reinstall")
    require(seeds[0].read_bytes() == seed_bytes,
            "the HarbourMaster cycle mutated the visible seed")


def main():
    run_video_provider_gate()
    tool = load_tool()
    schema = json.loads(SCHEMA_V3.read_text(encoding="utf-8"))
    require("runtime_root" in schema["properties"] and schema.get("allOf"),
            "nxproject v3 schema lacks conditional runtime_root")

    with tempfile.TemporaryDirectory(
            prefix="nxgenerator-generation-runtime-") as temporary:
        work = Path(temporary)
        source_root = work / "source"
        runtime_root = source_root / "runtime"
        runtime_root.mkdir(parents=True)
        payloads = write_runtime(runtime_root)
        (source_root / "LICENSE").write_bytes(
            (REPOSITORY / "LICENSE").read_bytes()
        )
        document = project_document(payloads)
        assert_artifact_inventory_order(tool, work)

        output = work / "schema3-output"
        tool.generate_project(document, output, source_root)
        assert_generation_v2(tool, output, payloads, source_root)
        assert_real_portmaster_cycle_with_seed(
            output, "nxgenerator-runtime-fixture", work)

        nxextract_document = json.loads(json.dumps(document))
        nxextract_records = add_nxextract_runtime(
            tool, runtime_root, nxextract_document
        )
        nxextract_output = work / "schema3-nxextract-output"
        tool.generate_project(nxextract_document, nxextract_output, source_root)
        nxextract_port = nxextract_output / "nxgenerator-runtime-fixture"
        for record in nxextract_records:
            target = nxextract_port / record["path"]
            require(target.is_file() and not target.is_symlink() and
                    "%04o" % stat.S_IMODE(target.stat().st_mode) ==
                    record["mode"] and sha256(target.read_bytes()) ==
                    record["sha256"],
                    "schema3+NXExtract changed a pinned runtime member")

        noncanonical_nxextract = json.loads(json.dumps(nxextract_document))
        engine = runtime_root / "nxextract" / "nxextract.py"
        canonical_engine = engine.read_bytes()
        engine.write_bytes(b"noncanonical NXExtract engine\n")
        os.chmod(engine, 0o644)
        engine_record = next(
            item for item in noncanonical_nxextract["nxport"][
                "generation_runtime"
            ] if item["role"] == "nxextract-engine"
        )
        engine_record["sha256"] = sha256(engine.read_bytes())
        expect_project_error(
            tool, noncanonical_nxextract,
            work / "noncanonical-nxextract-output", source_root,
            "differs from the canonical NXExtract runtime",
        )
        engine.write_bytes(canonical_engine)
        os.chmod(engine, 0o644)

        missing_field = json.loads(json.dumps(document))
        missing_field.pop("runtime_root")
        expect_project_error(
            tool, missing_field, work / "missing-field-output", source_root,
            "requires runtime_root",
        )

        missing_directory = json.loads(json.dumps(document))
        missing_directory["runtime_root"] = "absent-runtime"
        expect_project_error(
            tool, missing_directory, work / "missing-directory-output",
            source_root, "is not a real existing directory",
        )

        original_render_gameinfo = tool.render_gameinfo
        tool.render_gameinfo = lambda _nxport: (
            b'<?xml version="1.0" encoding="utf-8"?>\n'
            b'<gameList><game><path>./wrong.sh</path>'
            b'<name>NXGenerator Runtime &amp; Fixture</name></game></gameList>\n'
        )
        try:
            expect_project_error(
                tool, document, work / "tampered-gameinfo-output",
                source_root, "path differs from launcher_name",
            )
        finally:
            tool.render_gameinfo = original_render_gameinfo

        old_project_new_nxport = json.loads(json.dumps(document))
        old_project_new_nxport["schema_version"] = 2
        old_project_new_nxport.pop("language_access")
        old_project_new_nxport.pop("controls")
        old_project_new_nxport.pop("graphics")
        expect_project_error(
            tool, old_project_new_nxport,
            work / "old-project-new-nxport-output", source_root,
            "requires nxproject schema_version 3",
        )

        stale_hash = json.loads(json.dumps(document))
        (runtime_root / "bin" / "fixture-nextos").write_bytes(b"changed\n")
        os.chmod(runtime_root / "bin" / "fixture-nextos", 0o755)
        expect_project_error(
            tool, stale_hash, work / "stale-hash-output", source_root,
            "runtime member hash differs",
        )
        write_runtime(runtime_root)

        assert_schema2_regression(
            tool, document, work / "schema2-output", source_root
        )

        stray_runtime_root = json.loads(json.dumps(document))
        stray_runtime_root["schema_version"] = 2
        stray_runtime_root.pop("language_access")
        stray_runtime_root.pop("controls")
        stray_runtime_root.pop("graphics")
        stray_runtime_root["nxport"]["schema_version"] = 2
        stray_runtime_root["nxport"].pop("generation_runtime")
        expect_project_error(
            tool, stray_runtime_root, work / "stray-runtime-root-output",
            source_root, "runtime_root is only valid",
        )

    print(
        "nxgenerator generation-runtime tests passed: "
        "schema3_generation_v2=1 runtime_data=1 schema3_nxextract=1 "
        "noncanonical_nxextract=1 missing_runtime_root=2 "
        "stale_runtime_hash=1 schema2_regression=1 stray_runtime_root=1"
        " gameinfo_canonical=1 gameinfo_tamper=1 artifact_posix_order=1 portmaster_seeded_real_cycle=1"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError, ValueError, KeyError) as error:
        print("nxgenerator generation-runtime tests failed: %s" % error)
        raise SystemExit(1)
