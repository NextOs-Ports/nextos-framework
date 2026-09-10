#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Directed gate for rendering a real nxgenerator 0.3.12 package root.

NXGENERATOR_038_ROOT lets the nxrelease version worktree exercise the
nxgenerator version branch before both commits are integrated.  The historical
NXGENERATOR_035_ROOT and NXGENERATOR_0220_ROOT aliases remain accepted.  Once
integrated, the repository-local framework is the default.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath


TESTS = Path(__file__).resolve().parent
NXRELEASE = TESTS.parent
REPOSITORY = NXRELEASE.parents[1]
RENDERER = NXRELEASE / "nx-render-manifest.py"
GENERATOR_REPOSITORY = Path(
    os.environ.get(
        "NXGENERATOR_038_ROOT",
        os.environ.get(
            "NXGENERATOR_035_ROOT",
            os.environ.get("NXGENERATOR_0220_ROOT", str(REPOSITORY)),
        ),
    )
).resolve()
GENERATOR = (
    GENERATOR_REPOSITORY / "framework" / "nxgenerator" / "nxgenerator.py"
)
FRAMEWORK = GENERATOR_REPOSITORY / "framework"
NXEXTRACT_ROOT = (
    GENERATOR_REPOSITORY / "suportando_outros_devices" / "extrator-universal"
)
AUTHORED_README = (
    b"# Generator Root Fixture\n\n"
    b"[PT-BR] Candidato autoral autenticado.\n\n"
    b"[EN] Authenticated authored candidate.\n"
)
AUTHORED_INSTALLATION = (
    b"# Instalacao / Installation\n\n"
    b"## Portugues\n\nInstale `Generator Root Fixture.sh` e a pasta "
    b"`generator-root-fixture/` juntas em `ports/`. Este fixture nao inclui "
    b"dados de jogo e nao pede downloads externos. Confira os controles antes "
    b"de iniciar, coloque os dados do dono em "
    b"`generator-root-fixture/gamedata/` e preserve a pasta durante "
    b"atualizacoes.\n\n"
    b"## English\n\nInstall `Generator Root Fixture.sh` and the "
    b"`generator-root-fixture/` directory together under `ports/`. This fixture "
    b"contains no game data and requests no external download. Review controls "
    b"before launch, place owner data under "
    b"`generator-root-fixture/gamedata/`, and preserve the directory during "
    b"updates.\n"
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(
        json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )


def freeze_candidate_lock(candidate):
    """Create the canonical read-only authority outside the package root."""
    port = candidate / "generator-root-fixture"
    executable = port / "fixture-nextos"
    project = json.loads((port / "nxproject.json").read_text(encoding="utf-8"))
    controls = project["controls"]
    generation = json.loads(
        (port / "GENERATION.json").read_text(encoding="utf-8")
    )["generation_id"]
    mapping = port / "defaults" / "NEXTOSCONTROLLERS.gptk"
    adapter = port / "adapter" / "adapter-contract.json"
    sink_symbols = {
        "adapter.menu.accept": "adapter_menu_accept",
        "adapter.menu.back": "adapter_menu_back",
        "adapter.game.pause": "adapter_game_pause",
        "adapter.cursor.pointer": "adapter_cursor_pointer",
        "adapter.cursor.click": "adapter_cursor_click",
    }
    contexts = [
        {"context": name, "source": "fixture:" + name, "observed": True}
        for name in sorted(controls["contexts"])
    ]
    actions = {item["id"]: item for item in controls["actions"]}
    cases = []
    for context_name, bindings in controls["contexts"].items():
        for control, action_id in bindings.items():
            action = actions[action_id]
            event = {"button": "press", "axis": "axis",
                     "vector": "motion"}[action["kind"]]
            for sink in action["sinks"]:
                cases.append({
                    "context": context_name,
                    "context_source": "fixture:" + context_name,
                    "control": control, "event": event,
                    "decision": "ACTION", "action": action_id,
                    "sink": sink, "delivery_count": 1,
                })
    cases.sort(key=lambda item: (
        item["context"], item["control"], item["action"], item["sink"],
        item["event"], item["context_source"],
    ))
    runtime_symbols = sorted({
        "nxinput_gptk_load_at", "nxinput_gptk_load_receipt_json",
        "nxinput_gptk_parse", "nxinput_gptk_decide",
        "nxinput_gptk_live_init", "nxinput_gptk_live_register",
        "nxinput_gptk_live_register_vector", "nxinput_gptk_live_seal",
        "nxinput_gptk_live_set_context", "nxinput_gptk_live_clear_context",
        "nxinput_gptk_live_should_consume", "nxinput_gptk_live_feed",
        "nxinput_gptk_live_feed_vector", "nxinput_gptk_runtime_marker",
        "nxinput_gptk_event_evidence_schema",
    })
    input_proof = {
        "schema": "nxinput-gptk-event-evidence/1", "schema_version": 1,
        "run_id": "generator-root-directed", "generation": generation,
        "port_id": "generator-root-fixture",
        "mapping_sha256": sha256(mapping),
        "adapter_contract_sha256": sha256(adapter), "verdict": "OK",
        "runtime": {"marker": "nxinput-gptk-runtime/3",
                    "evidence_schema": "nxinput-gptk-event-evidence/1",
                    "symbols": runtime_symbols},
        "contexts": contexts,
        "sinks": [
            {"sink": sink, "symbol": symbol,
             "method": "runtime-action-registry",
             "targets": [sink.replace(".", "_")], "exists": True}
            for sink, symbol in sorted(sink_symbols.items())
        ],
        "cases": cases,
        "safety": {
            "unknown_context": {"result": "PASSTHROUGH",
                                "suppressed": False, "delivery_count": 0},
            "missing_sink": {"result": "PASSTHROUGH",
                             "suppressed": False, "delivery_count": 0},
            "failed_ack": {"result": "FATAL", "native_replay": False},
        },
    }
    input_line = (json.dumps(
        input_proof, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ) + "\n").encode("utf-8")
    lock_path = candidate.parent / "candidate-lock.json"
    write_json(lock_path, {
        "schema": "nxrelease-candidate-lock-v1",
        "schema_version": 1,
        "executable": "generator-root-fixture/fixture-nextos",
        "sha256": sha256(executable),
        "input_proof": input_proof,
        "input_proof_receipt_sha256": hashlib.sha256(input_line).hexdigest(),
    })
    lock_path.chmod(0o444)
    return lock_path


def run(command, expected=0):
    result = subprocess.run(
        [str(part) for part in command], text=True, capture_output=True,
        check=False,
    )
    require(
        result.returncode == expected,
        "command returned %d, expected %d:\n%s\nstdout:\n%s\nstderr:\n%s"
        % (result.returncode, expected, " ".join(map(str, command)),
           result.stdout, result.stderr),
    )
    return result


def runtime_record(runtime_root, role, path, mode):
    return {
        "role": role,
        "path": path,
        "mode": mode,
        "sha256": sha256(runtime_root / PurePosixPath(path)),
    }


def package_record(source_root, path, mode="0644", kind="payload"):
    return {
        "path": path,
        "mode": mode,
        "sha256": sha256(source_root / PurePosixPath(path)),
        "kind": kind,
    }


def materialize_nxextract_runtime(source_root, runtime_root):
    """Create the exact schema-3 NXExtract closure under the runtime root."""
    recipe = (NXEXTRACT_ROOT / "examples" / "recipe-minimal.json").read_bytes()
    for target in (source_root / "extractor.json",
                   runtime_root / "extractor.json"):
        target.write_bytes(recipe)
        target.chmod(0o644)

    nxextract = runtime_root / "nxextract"
    nxextract.mkdir()
    for name in (
            "nxextract.py", "run-extractor.sh",
            "nxextract-runtime-env.sh"):
        target = nxextract / name
        shutil.copyfile(NXEXTRACT_ROOT / name, target)
        target.chmod(0o644)
    ui = nxextract / "nxextract-ui"
    shutil.copyfile(
        NXEXTRACT_ROOT / "ui" / "release" / "aarch64" / "nxextract-ui",
        ui,
    )
    ui.chmod(0o755)


def project_document(source_root, runtime_root):
    return {
        "schema_version": 3,
        "runtime_root": "runtime",
        "nxport": {
            "schema_version": 3,
            "id": "generator-root-fixture",
            "title": "Generator Root Fixture",
            "launcher_name": "Generator Root Fixture.sh",
            "architecture": "aarch64",
            "executable": "fixture-nextos",
            "argument_mode": "none",
            "home_mode": "preserve",
            "sdl_provider": "system",
            "nxextract": {"mode": "yes", "version": "1.3.0"},
            "required_files": [
                "fixture-nextos", "hooks/nested.sh",
                "nxextract/lib/aarch64/libfoo.so.1",
            ],
            "private_library_paths": ["lib", "nxextract/lib/aarch64"],
            "prepare_script": "hooks/nested.sh",
            "required_capabilities": ["host.portmaster"],
            "enabled_quirks": [],
            "runtime_report": "log-and-logo",
            "generation_runtime": [
                runtime_record(
                    runtime_root, "executable", "fixture-nextos", "0755"
                ),
                runtime_record(
                    runtime_root, "private-library",
                    "lib/libfixture-private.so", "0644"
                ),
                runtime_record(
                    runtime_root, "private-library",
                    "nxextract/lib/aarch64/libfoo.so.1", "0644"
                ),
                runtime_record(
                    runtime_root, "runtime-data",
                    "lib/runtime/System.Private.CoreLib.dll", "0644"
                ),
                runtime_record(
                    runtime_root, "runtime-hook", "hooks/nested.sh", "0644"
                ),
                runtime_record(
                    runtime_root, "nxextract-recipe", "extractor.json", "0644"
                ),
                runtime_record(
                    runtime_root, "nxextract-engine",
                    "nxextract/nxextract.py", "0644"
                ),
                runtime_record(
                    runtime_root, "nxextract-runner",
                    "nxextract/run-extractor.sh", "0644"
                ),
                runtime_record(
                    runtime_root, "nxextract-runtime-env",
                    "nxextract/nxextract-runtime-env.sh", "0644"
                ),
                runtime_record(
                    runtime_root, "nxextract-ui",
                    "nxextract/nxextract-ui", "0755"
                ),
            ],
        },
        "nxextract_recipe": "extractor.json",
        "adapter": {"skeleton": "contract-only"},
        "portmaster": {
            "metadata_version": 4,
            "min_glibc": "2.17",
            "runtime": [],
        },
        "license": {"spdx_id": "GPL-3.0-only", "source": "LICENSE"},
        "documentation": {"status": "authored", "proven_support": []},
        "language_access": {
            "mode": "none", "supported": [], "fallback": "", "sinks": [],
        },
        "controls": {
            "runtime_mapping": "nxinput-gptk",
            "controller_profiles": {
                "enabled": True,
                "bundle": "controllers.nxb",
                "sha256": sha256(source_root / "controllers.nxb"),
            },
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
                {
                    "id": "cursor.fixture.pointer", "kind": "vector",
                    "sinks": ["adapter.cursor.pointer"],
                },
                {
                    "id": "cursor.fixture.click", "kind": "button",
                    "sinks": ["adapter.cursor.click"],
                },
            ],
            "contexts": {
                "menu": {"A": "fixture.accept", "B": "fixture.back"},
                "gameplay": {"START": "fixture.pause"},
                "cursor": {
                    "RIGHT_STICK": "cursor.fixture.pointer",
                    "R3": "cursor.fixture.click",
                },
            },
            "tuning": {
                "cursor": {
                    "speed": 1.25,
                    "deadzone": 0.15,
                    "response_curve": 1.35,
                    "acceleration": 0.35,
                    "smoothing_ms": 45,
                },
                "camera": {"authority": "native"},
            },
        },
        "graphics": {
            "uses_gl": True,
            "api": "gles",
            "profile": "es",
            "version": "2.0",
            "version_policy": "minimum",
            "shader_dialect": "essl100",
            "drawable_ready_timeout_ms": 8000,
            "adopt_single_channel": True,
            "required_devices": ["mali-450"],
        },
        "package_payload": [
            package_record(source_root, "FRAMEWORK-PIN.json"),
            package_record(source_root, "INSTALLATION.md"),
            package_record(source_root, "README.md"),
            package_record(source_root, "controllers.nxb"),
            package_record(source_root, "tools/BUILD-INPUTS.json"),
            package_record(source_root, "tools/inspect", "0755"),
            package_record(
                source_root, "tools/reference.py", kind="license-notice"
            ),
            package_record(
                source_root, "tools/verify-build-inputs.py", "0755"
            ),
            package_record(source_root, "version.txt"),
        ],
    }


def build_candidate(work):
    source = work / "source"
    runtime = source / "runtime"
    runtime.mkdir(parents=True)
    # Link the real AArch64 nxinput live boundary.  The old fixture appended a
    # marker string to NXSplash and was exactly the false positive fixed by
    # nxrelease 0.3.12.
    executable = runtime / "fixture-nextos"
    live_fixture = source / "live-fixture.c"
    live_fixture.write_text(
        '#include "nxinput_gptk_live.h"\n'
        '#define SINK(name) int name(void){return 0;}\n'
        'SINK(adapter_menu_accept) SINK(adapter_menu_back)\n'
        'SINK(adapter_game_pause) SINK(adapter_cursor_pointer)\n'
        'SINK(adapter_cursor_click)\n'
        'int main(void){return nxinput_gptk_runtime_marker()[0]==0 || '
        'nxinput_gptk_event_evidence_schema()[0]==0;}\n',
        encoding="utf-8",
    )
    run([
        "aarch64-linux-gnu-gcc", "-std=c11", "-O0", "-static",
        "-D_POSIX_C_SOURCE=200809L", "-I", FRAMEWORK / "nxinput" / "include",
        live_fixture, FRAMEWORK / "nxinput" / "src" / "nxinput_gptk.c",
        FRAMEWORK / "nxinput" / "src" / "nxinput_gptk_live.c",
        FRAMEWORK / "nxinput" / "src" / "nxinput_gptk_loader.c",
        FRAMEWORK / "nxinput" / "src" / "nxinput_gptk_motion.c",
        "-lm", "-o", executable,
    ])
    executable.chmod(0o755)
    # Reuse a real AArch64 ELF so the regression reaches the renderer's
    # production readelf/GLIBC path.  Its private-library role and 0644 mode
    # are authenticated independently of the filename and immutable copy.
    private_library = runtime / "lib" / "libfixture-private.so"
    private_library.parent.mkdir()
    shutil.copyfile(
        FRAMEWORK / "nxsplash" / "release" / "aarch64" /
        "nxsplash-nextos",
        private_library,
    )
    private_library.chmod(0o644)
    runtime_data = runtime / "lib" / "runtime" / "System.Private.CoreLib.dll"
    runtime_data.parent.mkdir()
    runtime_data.write_bytes(b"managed runtime data fixture\n")
    runtime_data.chmod(0o644)
    hook = runtime / "hooks" / "nested.sh"
    hook.parent.mkdir()
    hook.write_bytes(b"#!/bin/sh\nexit 0\n")
    hook.chmod(0o644)
    materialize_nxextract_runtime(source, runtime)
    nxextract_private_library = (
        runtime / "nxextract" / "lib" / "aarch64" / "libfoo.so.1"
    )
    nxextract_private_library.parent.mkdir(parents=True)
    shutil.copyfile(
        FRAMEWORK / "nxsplash" / "release" / "aarch64" /
        "nxsplash-nextos",
        nxextract_private_library,
    )
    nxextract_private_library.chmod(0o644)
    shutil.copyfile(GENERATOR_REPOSITORY / "LICENSE", source / "LICENSE")
    (source / "README.md").write_bytes(AUTHORED_README)
    (source / "README.md").chmod(0o644)
    (source / "INSTALLATION.md").write_bytes(AUTHORED_INSTALLATION)
    (source / "INSTALLATION.md").chmod(0o644)
    (source / "FRAMEWORK-PIN.json").write_bytes(b"{\"schema_version\":1}\n")
    (source / "FRAMEWORK-PIN.json").chmod(0o644)
    (source / "version.txt").write_bytes(b"1.2.3\n")
    (source / "version.txt").chmod(0o644)
    # runtime_mapping nxinput-gptk exige o bundle da autoridade 3 pinado
    # dentro do pacote (regressão de campo de 31/08/2026).
    (source / "controllers.nxb").write_bytes(
        b"NXCONTROLLER_PROFILES/1\n"
        b"# license=Zlib (SDL_GameControllerDB dialect fixture)\n"
        b"190000004b4800000011000000010000,Fixture Pad,a:b0,b:b1,start:b9,"
        b"back:b8,leftx:a0,lefty:a1,platform:Linux,\n"
    )
    (source / "controllers.nxb").chmod(0o644)
    tools = source / "tools"
    tools.mkdir()
    (tools / "BUILD-INPUTS.json").write_bytes(
        b"{\"schema_version\":1,\"inputs\":[]}\n"
    )
    (tools / "BUILD-INPUTS.json").chmod(0o644)
    (tools / "inspect").write_bytes(b"#!/bin/sh\nexit 0\n")
    (tools / "inspect").chmod(0o755)
    (tools / "reference.py").write_bytes(
        b"# Reference notice carried as data, not an executable.\n"
    )
    (tools / "reference.py").chmod(0o644)
    (tools / "verify-build-inputs.py").write_bytes(
        b"#!/usr/bin/env python3\nraise SystemExit(0)\n"
    )
    (tools / "verify-build-inputs.py").chmod(0o755)
    project = project_document(source, runtime)
    promoted_adapter = {
        "schema": "nxadapter-skeleton-v1", "schema_version": 1,
        "status": "implemented_release", "release_ready": True,
        "lifecycle": {"sequence": ["fixture-live"], "source_evidence": []},
        "language_access": project["language_access"],
        "graphics": project["graphics"],
        "input_controller_profiles":
            project["controls"]["controller_profiles"],
        "input": {
            "actions": project["controls"]["actions"],
            "contexts": project["controls"]["contexts"],
            "runtime_mapping": "nxinput-gptk",
            "runtime_contract": {
                "schema": "nxinput-gptk-live/1",
                "context_initial": "unproven",
                "unproven_policy": "native-passthrough",
                "sink_coverage": "all-actions-before-activation",
                "delivery_ack": "required",
            },
        },
    }
    write_json(source / "promoted-adapter.json", promoted_adapter)
    project["promotion"] = {
        "adapter_contract": "promoted-adapter.json",
        "claims": {"release_ready": True, "physical_support_proven": False,
                   "adapter_lifecycle_implemented": True},
    }
    manifest = source / "nxproject-input.json"
    write_json(manifest, project)
    output = work / "candidate"
    run([
        sys.executable, GENERATOR, manifest, "--output", output,
        "--source-root", source,
    ])
    return output


def render(candidate, expected=0):
    return run([
        sys.executable, RENDERER, "--generator-root", candidate,
        "--framework-root", FRAMEWORK,
        "--source-url", "https://example.invalid/generator-root",
    ], expected=expected)


def assert_positive(candidate):
    pid = "generator-root-fixture"
    launcher = "Generator Root Fixture.sh"
    port = candidate / pid
    require((candidate / launcher).is_file(),
            "nxgenerator did not put the launcher at package root")
    require(not (port / launcher).exists(),
            "nxgenerator duplicated the launcher inside the game directory")
    require((port / "gameinfo.xml").is_file(),
            "real nxgenerator 0.3.12 candidate lacks gameinfo.xml")
    require((port / "GENERATION.json").is_file(),
            "real candidate lacks GENERATION.json")

    render(candidate)
    release_path = candidate / "nxrelease.json"
    release = json.loads(release_path.read_text(encoding="utf-8"))
    candidate_lock = freeze_candidate_lock(candidate)
    stage = candidate.parent / "validated-stage"
    run([
        sys.executable, NXRELEASE / "nxrelease.py", "validate",
        "--manifest", release_path, "--candidate-lock", candidate_lock,
    ])
    run([
        sys.executable, NXRELEASE / "nxrelease.py", "stage",
        "--manifest", release_path, "--candidate-lock", candidate_lock,
        "--stage", stage,
    ])
    run([
        sys.executable, NXRELEASE / "nxrelease.py", "verify-stage",
        "--stage", stage,
    ])
    require(release["source_root"] == ".",
            "generator-root manifest source_root is not package-shaped '.'")
    require(release["package"]["version"] == "1.2.3",
            "authored version.txt did not define the package version")
    require({path.name for path in candidate.iterdir()} == {
        launcher, pid, "nxrelease.json",
    }, "renderer changed or accepted a non-package-shaped root")

    records = release["files"]
    require(records and all(not Path(record["source"]).is_absolute()
                            for record in records),
            "manifest contains an absolute package source")
    sources = {record["source"]: record for record in records}
    require(sources[launcher]["target"] == launcher,
            "root launcher source/target is not package-shaped")
    require(sum(record["source"] == launcher for record in records) == 1,
            "launcher is not the sole root file source")
    require(all(
        record["source"] == launcher or record["source"].startswith(pid + "/")
        for record in records
    ), "a game file source escaped the generated <port-id>/ directory")

    expected_game_sources = {
        pid + "/nxport.json",
        pid + "/nxproject.json",
        pid + "/fixture-nextos",
        pid + "/lib/libfixture-private.so",
        pid + "/nxextract/lib/aarch64/libfoo.so.1",
        pid + "/lib/runtime/System.Private.CoreLib.dll",
        pid + "/hooks/nested.sh",
        pid + "/README.md",
        pid + "/INSTALLATION.md",
        pid + "/LICENSE",
        pid + "/port.json",
        pid + "/gameinfo.xml",
        pid + "/nxsplash-nextos",
        pid + "/extractor.json",
        pid + "/nxextract/nxextract.py",
        pid + "/nxextract/run-extractor.sh",
        pid + "/nxextract/nxextract-runtime-env.sh",
        pid + "/nxextract/nxextract-ui",
        pid + "/gamedata/README.txt",
        pid + "/GENERATION.json",
        pid + "/FRAMEWORK-PIN.json",
        pid + "/tools/BUILD-INPUTS.json",
        pid + "/tools/inspect",
        pid + "/tools/reference.py",
        pid + "/tools/verify-build-inputs.py",
        pid + "/version.txt",
    }
    require(expected_game_sources <= set(sources),
            "renderer omitted package-shaped generated game files: %r" %
            sorted(expected_game_sources - set(sources)))

    generation_roots = list(
        (port / ".nxruntime" / "generations").iterdir()
    )
    require(len(generation_roots) == 1 and generation_roots[0].is_dir(),
            "fixture did not produce one immutable generation-v2 store")
    generation_id = generation_roots[0].name
    seed_source = pid + "/nxruntime-" + generation_id + ".nxb"
    seed = sources.get(seed_source)
    require(seed is not None,
            "renderer omitted the visible generation-v2 runtime seed")
    require(seed.get("source") == seed_source and
            seed.get("target") == seed_source and
            seed.get("kind") == "nxruntime-seed" and
            seed.get("mode") == "0644" and
            seed.get("sha256") == sha256(candidate / seed_source),
            "generation-v2 runtime seed lost its exact path/kind/mode/SHA")
    launcher_text = (candidate / launcher).read_text(encoding="utf-8")
    for token in (
            'NXBOOTSTRAP_BUNDLE_NAME="nxruntime-$NXBOOTSTRAP_GENERATION_ID.nxb"',
            "nxbootstrap_bundle_materialize()",
            "runtime cache rebuilt from seed"):
        require(token in launcher_text,
                "canonical launcher is not runtime-seed capable: " + token)
    generation_prefix = (
        pid + "/" + generation_roots[0].relative_to(port).as_posix()
    )
    linux_pairs = {
        pid + "/fixture-nextos":
            generation_prefix + "/files/runtime/fixture-nextos",
        pid + "/lib/libfixture-private.so":
            generation_prefix +
            "/files/runtime/lib/libfixture-private.so",
        pid + "/nxextract/lib/aarch64/libfoo.so.1":
            generation_prefix +
            "/files/runtime/nxextract/lib/aarch64/libfoo.so.1",
        pid + "/nxsplash-nextos":
            generation_prefix + "/files/runtime/nxsplash-nextos",
        pid + "/nxextract/nxextract-ui":
            generation_prefix + "/files/runtime/nxextract/nxextract-ui",
    }
    required_linux_fields = {
        "source", "target", "kind", "mode", "sha256", "architecture",
        "build_profile", "provenance", "needed", "soname",
    }
    generation_linux_sources = set()
    for live_source, store_source in linux_pairs.items():
        live = sources[live_source]
        store = sources.get(store_source)
        require(store is not None,
                "generation-v2 omitted duplicated Linux ELF: " + store_source)
        require(store.get("kind") == "nxruntime-generation-linux",
                "generation-v2 ELF lacks its own Linux inventory class: " +
                store_source)
        require(set(store) == required_linux_fields,
                "generation-v2 ELF lacks complete Linux metadata: " +
                store_source)
        require(store["source"] == store["target"] == store_source,
                "generation-v2 ELF logical source/target drifted: " +
                store_source)
        require(store["mode"] == live["mode"] and
                store["sha256"] == live["sha256"],
                "generation-v2 ELF mode/SHA differs from its live copy: " +
                store_source)
        for field in (
                "architecture", "build_profile", "provenance", "needed",
                "soname"):
            require(store[field] == live[field],
                    "generation-v2 ELF metadata differs from live %s: %s" %
                    (field, store_source))
        generation_linux_sources.add(store_source)
    require({
        record["source"] for record in records
        if record.get("kind") == "nxruntime-generation-linux"
    } == generation_linux_sources,
            "generation-v2 Linux class includes a non-ELF or misses an ELF")
    require(sources[pid + "/lib/libfixture-private.so"]["mode"] == "0644",
            "authenticated private-library mode was rewritten")
    require(
        sources[pid + "/nxextract/lib/aarch64/libfoo.so.1"]["kind"] ==
        "third-party-linux" and
        sources[pid + "/nxextract/lib/aarch64/libfoo.so.1"]["mode"] ==
        "0644",
        "NXExtract private library lost third-party class or mode",
    )
    runtime_data_source = pid + "/lib/runtime/System.Private.CoreLib.dll"
    runtime_data_store = (
        generation_prefix +
        "/files/runtime/lib/runtime/System.Private.CoreLib.dll"
    )
    runtime_data_live = sources[runtime_data_source]
    runtime_data_immutable = sources.get(runtime_data_store)
    require(runtime_data_live.get("kind") == "payload" and
            runtime_data_live.get("mode") == "0644",
            "runtime-data live member was reclassified as an ELF")
    require(runtime_data_immutable is not None and
            runtime_data_immutable.get("kind") == "nxruntime-generation" and
            runtime_data_immutable.get("mode") == "0644" and
            runtime_data_immutable.get("sha256") ==
            runtime_data_live.get("sha256"),
            "runtime-data immutable copy differs from its live member")
    require({
        "architecture", "build_profile", "provenance", "needed", "soname",
    }.isdisjoint(runtime_data_immutable),
            "runtime-data immutable copy gained Linux metadata")

    generation_controls = (
        "commit", "components.sha256", "components.v2", "format",
        "identity-runtime.v2", "identity.json", "manifest.json",
    )
    for name in generation_controls:
        logical = generation_prefix + "/" + name
        control = sources.get(logical)
        require(control is not None and
                control.get("kind") == "nxruntime-generation",
                "generation-v2 non-ELF control lost its generic class: " +
                logical)
        require(set(control) == {
            "source", "target", "kind", "mode", "sha256",
        }, "generation-v2 control gained Linux metadata: " + logical)
        require((candidate / PurePosixPath(logical)).read_bytes()[:4] !=
                b"\x7fELF",
                "generation-v2 control fixture unexpectedly became an ELF")
    require(
        sources[pid + "/tools/inspect"]["mode"] == "0755" and
        sources[pid + "/tools/reference.py"]["mode"] == "0644" and
        sources[pid + "/tools/reference.py"]["kind"] == "license-notice",
        "renderer inferred authored tool mode/kind instead of honoring it",
    )
    generation = sources[pid + "/GENERATION.json"]
    require(generation["target"] == pid + "/GENERATION.json" and
            generation["kind"] == "payload",
            "GENERATION.json is not included as authenticated game payload")
    require(any(
        record["source"].startswith(pid + "/.nxruntime/generations/")
        for record in records
    ), "generation-v2 store is absent from package sources")
    require((port / "README.md").read_bytes() == AUTHORED_README and
            (port / "INSTALLATION.md").read_bytes() == AUTHORED_INSTALLATION,
            "nxgenerator did not compose exact authored documentation")
    gptk = (port / "defaults" / "NEXTOSCONTROLLERS.gptk").read_text(
        encoding="utf-8"
    )
    for token in (
            "speed = 1.25", "deadzone = 0.15", "response_curve = 1.35",
            "acceleration = 0.35", "smoothing_ms = 45", "[camera]",
            "authority = native"):
        require(token in gptk, "generated GPTK lost tuning token: " + token)
    generation_document = json.loads(
        (port / "GENERATION.json").read_text(encoding="utf-8")
    )
    artifacts = {item["path"]: item for item in generation_document["artifacts"]}
    for path in (
            "FRAMEWORK-PIN.json", "INSTALLATION.md", "README.md",
            "tools/BUILD-INPUTS.json", "tools/inspect", "tools/reference.py",
            "tools/verify-build-inputs.py", "version.txt"):
        record = artifacts.get(pid + "/" + path)
        require(record is not None and record["sha256"] == sha256(port / path),
                "GENERATION.json did not bind authored payload: " + path)
    executable_record = next(
        item for item in records if item["target"] == pid + "/fixture-nextos"
    )
    require("FRAMEWORK-PIN.json" in executable_record["provenance"] and
            "tools/BUILD-INPUTS.json" in executable_record["provenance"],
            "project ELF provenance omitted authored build pins")
    return candidate_lock

def expect_negative(base, work, label, needle, mutate):
    candidate = work / label
    shutil.copytree(base, candidate)
    mutate(candidate)
    result = render(candidate, expected=1)
    message = result.stdout + result.stderr
    require(needle in message,
            "%s reported %r, expected %r" % (label, message, needle))


def expect_stale_generation(base, work):
    candidate = work / "coordinated-authored-overlay"
    shutil.copytree(base, candidate)
    port = candidate / "generator-root-fixture"
    readme = port / "README.md"
    readme.write_text("coordinated overlay after generation\n", encoding="utf-8")
    project_path = port / "nxproject.json"
    project = json.loads(project_path.read_text(encoding="utf-8"))
    next(
        record for record in project["package_payload"]
        if record["path"] == "README.md"
    )["sha256"] = sha256(readme)
    write_json(project_path, project)
    receipt_path = port / "GENERATION.json"
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    receipt["project_manifest_sha256"] = sha256(project_path)
    write_json(receipt_path, receipt)

    # Source, declaration and even the receipt's project hash now agree; its
    # immutable artifact closure still describes the pre-overlay bytes.
    result = render(candidate, expected=1)
    require("GENERATION.json artifact closure is stale" in
            result.stdout + result.stderr,
            "renderer accepted a coordinated overlay after generation")


def generation_store_runtime(candidate, relative):
    port = candidate / "generator-root-fixture"
    roots = list((port / ".nxruntime" / "generations").iterdir())
    require(len(roots) == 1, "negative fixture lost its single generation")
    return roots[0] / "files" / "runtime" / PurePosixPath(relative)


def seed_closure(candidate):
    """Return the coordinated release/receipt records for the visible seed."""
    manifest_path = candidate / "nxrelease.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    seed_records = [
        record for record in manifest["files"]
        if record.get("kind") == "nxruntime-seed"
    ]
    require(len(seed_records) == 1,
            "negative fixture does not carry exactly one runtime seed")
    seed_record = seed_records[0]
    seed_target = seed_record["target"]
    require(seed_record["source"] == seed_target,
            "negative fixture seed source/target differ")
    seed_path = candidate / PurePosixPath(seed_record["source"])

    receipt_target = "generator-root-fixture/GENERATION.json"
    receipt_record = next(
        record for record in manifest["files"]
        if record.get("target") == receipt_target
    )
    receipt_path = candidate / PurePosixPath(receipt_target)
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    seed_artifacts = [
        artifact for artifact in receipt["artifacts"]
        if artifact.get("path") == seed_target
    ]
    require(len(seed_artifacts) == 1,
            "GENERATION.json does not bind exactly one runtime seed")
    return {
        "artifact": seed_artifacts[0],
        "manifest": manifest,
        "manifest_path": manifest_path,
        "receipt": receipt,
        "receipt_path": receipt_path,
        "receipt_record": receipt_record,
        "record": seed_record,
        "seed_path": seed_path,
        "target": seed_target,
    }


def write_seed_closure(context):
    """Reseal outer declarations so only the semantic seed gate can refuse."""
    seed_digest = sha256(context["seed_path"])
    context["record"]["sha256"] = seed_digest
    context["artifact"]["sha256"] = seed_digest
    write_json(context["receipt_path"], context["receipt"])
    context["receipt_record"]["sha256"] = sha256(context["receipt_path"])
    write_json(context["manifest_path"], context["manifest"])


def omit_seed_declarations(candidate):
    """Remove the seed coherently from both manifest and generator receipt."""
    context = seed_closure(candidate)
    context["manifest"]["files"] = [
        record for record in context["manifest"]["files"]
        if record is not context["record"]
    ]
    context["receipt"]["artifacts"] = [
        artifact for artifact in context["receipt"]["artifacts"]
        if artifact is not context["artifact"]
    ]
    write_json(context["receipt_path"], context["receipt"])
    context["receipt_record"]["sha256"] = sha256(context["receipt_path"])
    write_json(context["manifest_path"], context["manifest"])


def foreign_seed_generation(candidate):
    """Keep a valid NXB container but make its declared generation foreign."""
    context = seed_closure(candidate)
    payload = context["seed_path"].read_bytes()
    own_generation = context["target"].removeprefix(
        "generator-root-fixture/nxruntime-"
    ).removesuffix(".nxb")
    foreign_generation = "f" * 64
    if own_generation == foreign_generation:
        foreign_generation = "e" * 64
    old = ("generation " + own_generation + "\n").encode("ascii")
    new = ("generation " + foreign_generation + "\n").encode("ascii")
    require(payload.count(old) == 1,
            "negative fixture seed has no unique generation header")
    context["seed_path"].write_bytes(payload.replace(old, new, 1))
    write_seed_closure(context)


def divergent_seed_member(candidate):
    """Authenticate altered member bytes while the generation store stays exact."""
    context = seed_closure(candidate)
    payload = context["seed_path"].read_bytes()
    header_end = payload.index(b"\nEND\n") + len(b"\nEND\n")
    header = payload[:header_end]
    body = bytearray(payload[header_end:])
    records = []
    for line in header.splitlines(keepends=True):
        if not line.startswith(b"M\t"):
            continue
        _magic, mode, digest, size, offset, path = line.rstrip(b"\n").split(
            b"\t", 5
        )
        records.append((line, mode, digest, int(size), int(offset), path))
    target = next(
        record for record in records
        if record[5] ==
        b"files/runtime/lib/runtime/System.Private.CoreLib.dll"
    )
    old_line, mode, _digest, size, offset, path = target
    require(size > 0, "negative fixture selected an empty seed member")
    body[offset] ^= 0x01
    member_digest = hashlib.sha256(body[offset:offset + size]).hexdigest()
    new_line = b"\t".join((
        b"M", mode, member_digest.encode("ascii"), str(size).encode("ascii"),
        str(offset).encode("ascii"), path,
    )) + b"\n"
    require(len(new_line) == len(old_line) and header.count(old_line) == 1,
            "negative fixture cannot rewrite the seed member canonically")
    context["seed_path"].write_bytes(
        header.replace(old_line, new_line, 1) + bytes(body)
    )
    write_seed_closure(context)


def expect_seed_core_negative(base, work, label, candidate_lock, mutate):
    """Exercise nxrelease validate, bypassing renderer-only seed checks."""
    candidate = work / label
    shutil.copytree(base, candidate)
    mutate(candidate)
    result = run([
        sys.executable, NXRELEASE / "nxrelease.py", "validate",
        "--manifest", candidate / "nxrelease.json",
        "--candidate-lock", candidate_lock,
    ], expected=1)
    message = result.stdout + result.stderr
    require("generation-v2 runtime seed" in message,
            "%s reported %r without the semantic runtime-seed refusal" %
            (label, message))


def drift_store_path(candidate):
    source = generation_store_runtime(candidate, "fixture-nextos")
    source.rename(source.with_name("fixture-nextos-drifted"))


def drift_executable_store_mode(candidate):
    generation_store_runtime(candidate, "fixture-nextos").chmod(0o644)


def drift_private_library_store_mode(candidate):
    generation_store_runtime(
        candidate, "lib/libfixture-private.so"
    ).chmod(0o755)


def drift_store_sha(candidate):
    source = generation_store_runtime(candidate, "fixture-nextos")
    source.write_bytes(source.read_bytes() + b"generation-sha-drift")


def main():
    require(GENERATOR.is_file(),
            "nxgenerator 0.3.12 tool is missing: %s" % GENERATOR)
    require((FRAMEWORK / "nxgenerator" / "VERSION").read_text(
        encoding="utf-8").strip() == "0.4.5",
        "test requires the real nxgenerator 0.4.5 tree")
    require((FRAMEWORK / "nxbootstrap" / "VERSION").read_text(
        encoding="utf-8").strip() == "0.8.4",
        "current nxgenerator candidate must use nxbootstrap 0.8.4")

    with tempfile.TemporaryDirectory(
            prefix="nxrelease-generator-root-") as temporary:
        work = Path(temporary)
        candidate = build_candidate(work)
        candidate_lock = assert_positive(candidate)
        negatives = work / "negative"
        negatives.mkdir()
        expect_seed_core_negative(
            candidate, negatives, "generation-v2-seed-omitted",
            candidate_lock, omit_seed_declarations,
        )
        expect_seed_core_negative(
            candidate, negatives, "generation-v2-seed-foreign",
            candidate_lock, foreign_seed_generation,
        )
        expect_seed_core_negative(
            candidate, negatives, "generation-v2-seed-member-divergent",
            candidate_lock, divergent_seed_member,
        )
        expect_negative(
            candidate, negatives, "extra-root-entry",
            "generator root contains unexpected entries",
            lambda root: (root / "stray.txt").write_text(
                "not part of the generated package\n", encoding="utf-8"
            ),
        )
        expect_stale_generation(candidate, negatives)
        expect_negative(
            candidate, negatives, "missing-root-launcher",
            "generated launcher is missing or unsafe",
            lambda root: (root / "Generator Root Fixture.sh").unlink(),
        )
        expect_negative(
            candidate, negatives, "tampered-authored-readme",
            "package_payload bytes differ: README.md",
            lambda root: (root / "generator-root-fixture" / "README.md").write_text(
                "tampered after generation\n", encoding="utf-8"
            ),
        )
        expect_negative(
            candidate, negatives, "generation-store-path-drift",
            "generation-v2 components[2].file is missing or unreadable",
            drift_store_path,
        )
        expect_negative(
            candidate, negatives, "generation-store-mode-drift",
            "mode is 0644, expected 0755",
            drift_executable_store_mode,
        )
        expect_negative(
            candidate, negatives, "private-library-store-mode-drift",
            "mode is 0755, expected 0644",
            drift_private_library_store_mode,
        )
        expect_negative(
            candidate, negatives, "generation-store-sha-drift",
            "component bytes differ: files/runtime/fixture-nextos",
            drift_store_sha,
        )

    print(
        "nxrelease generator-root tests passed: real_nxgenerator_0312=1 "
        "package_sources=1 authored_payload=1 gptk_tuning=1 "
        "renderer_receipt_closure=1 generation_receipt=1 source_root_dot=1 "
        "launcher_root=1 nested_runtime_hook=1 game_files_under_id=1 unexpected_root=1 "
        "runtime_seed=1 seed_capable_launcher=1 seed_omitted=1 "
        "seed_foreign=1 seed_member_divergent=1 "
        "missing_launcher=1 authored_tamper=1 coordinated_overlay=1 "
        "generation_linux_elves=5 private_library_0644=1 "
        "nxextract_private_library=1 runtime_data=1 "
        "generation_nonelf_controls=7 generation_path_drift=1 "
        "generation_mode_drift=1 private_library_mode_drift=1 "
        "generation_sha_drift=1 "
        "nxrelease_validate=1 nxrelease_stage=1 nxrelease_verify_stage=1"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError, KeyError) as error:
        print("nxrelease generator-root tests failed: %s" % error)
        raise SystemExit(1)
