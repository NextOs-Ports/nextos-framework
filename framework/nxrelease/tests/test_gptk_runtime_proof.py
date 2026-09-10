#!/usr/bin/env python3
"""Directed host gate for GPTK event -> decision -> real sink evidence."""

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
RELEASE = ROOT / "framework" / "nxrelease" / "nxrelease.py"
RENDERER = ROOT / "framework" / "nxrelease" / "nx-render-manifest.py"
NXINPUT = ROOT / "framework" / "nxinput"
CANDIDATE_LOCK_SCHEMA = (
    ROOT / "framework" / "nxrelease" / "schema" /
    "candidate-lock-v1.schema.json"
)


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load " + str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expect_failure(fn, needle):
    try:
        fn()
    except (SystemExit, Exception) as error:
        if needle not in str(error):
            raise AssertionError("wrong failure: " + str(error))
    else:
        raise AssertionError("negative passed: " + needle)


def validate_runtime_schema(runtime):
    """Exercise the closed runtime fragment without a third-party validator."""
    schema = json.loads(CANDIDATE_LOCK_SCHEMA.read_text(encoding="utf-8"))
    fragment = schema["properties"]["input_proof"]["properties"]["runtime"]
    allowed = set(fragment["properties"])
    keys = set(runtime)
    if fragment.get("additionalProperties") is False and keys - allowed:
        raise ValueError("runtime has additional properties")
    missing = set(fragment["required"]) - keys
    if missing:
        raise ValueError("runtime lacks required properties")
    for key, dependencies in fragment.get("dependentRequired", {}).items():
        if key in runtime and not set(dependencies).issubset(keys):
            raise ValueError("runtime lacks dependent property")
    for key, rule in fragment["properties"].items():
        if key in runtime and "const" in rule and runtime[key] != rule["const"]:
            raise ValueError("runtime const differs")


def build_live_elf(work, godot=False):
    source = work / ("main-godot.c" if godot else "main.c")
    marker_source = ""
    marker_check = ""
    if godot:
        marker_source = (
            'static volatile const char godot_runtime[] = '
            '"nxinput-godot-runtime/1";\n'
            'static volatile const char godot_frame_proof[] = '
            '"nxgl-godot-frame-proof/2";\n'
            'int nxgl_frame_proof_is_fatal(void) { return 0; }\n'
            'int nxgl_frame_proof_consume_fatal(void) { return 0; }\n'
        )
        marker_check = (
            ' || godot_runtime[0] == 0 || godot_frame_proof[0] == 0'
        )
    source.write_text(
        '#include "nxinput_gptk_live.h"\n'
        + marker_source +
        'int engine_accept_sink(void *u, const char *a, int p, float v) '
        '{ (void)u; (void)a; (void)p; (void)v; return 0; }\n'
        'int main(void) { return nxinput_gptk_runtime_marker()[0] == 0 || '
        'nxinput_gptk_event_evidence_schema()[0] == 0' + marker_check +
        '; }\n',
        encoding="utf-8",
    )
    target = work / ("demo-godot-nextos" if godot else "demo-nextos")
    command = [
        shutil.which("cc") or "cc", "-std=c11", "-O0", "-g",
        "-D_POSIX_C_SOURCE=200809L", "-I", str(NXINPUT / "include"),
        str(source), str(NXINPUT / "src" / "nxinput_gptk.c"),
        str(NXINPUT / "src" / "nxinput_gptk_live.c"),
        str(NXINPUT / "src" / "nxinput_gptk_loader.c"),
        str(NXINPUT / "src" / "nxinput_gptk_motion.c"),
        "-lm", "-o", str(target),
    ]
    subprocess.run(command, check=True, stdin=subprocess.DEVNULL,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return target


def main():
    nxrelease = load(RELEASE, "nxrelease_gptk_proof")
    renderer = load(RENDERER, "nxrender_gptk_proof")
    generation = "a" * 64
    controls = {
        "schema": 2,
        "runtime_mapping": "nxinput-gptk",
        "actions": [{"id": "menu.accept", "kind": "button",
                     "sinks": ["engine.accept"]}],
        "contexts": {
            "menu": {"A": "menu.accept"},
            "gameplay": {"A": "menu.accept"},
        },
    }
    with tempfile.TemporaryDirectory(prefix="nxrelease-gptk-proof-") as raw:
        work = Path(raw)
        port = work / "demo"
        (port / "defaults").mkdir(parents=True)
        (port / "adapter").mkdir()
        mapping = port / "defaults" / "NEXTOSCONTROLLERS.gptk"
        mapping.write_text(
            "format = NEXTOS_CONTROLLERS/2\nport = demo\n\n"
            "[menu]\nA = menu.accept\n\n"
            "[gameplay]\nA = menu.accept\n", encoding="utf-8")
        adapter = {
            "schema": "nxadapter-skeleton-v1", "schema_version": 1,
            "status": "implemented_release", "release_ready": True,
            "input": {
                "actions": controls["actions"],
                "contexts": controls["contexts"],
                "runtime_mapping": "nxinput-gptk",
                "runtime_contract": nxrelease.INPUT_RUNTIME_CONTRACT,
            },
        }
        adapter_path = port / "adapter" / "adapter-contract.json"
        adapter_path.write_text(json.dumps(adapter, sort_keys=True) + "\n",
                                encoding="utf-8")
        executable = build_live_elf(work)
        shutil.copy2(executable, port / "demo-nextos")
        project = {"nxport": {"executable": "demo-nextos"},
                   "controls": controls}
        renderer._validate_controls_runtime(project, port)

        fake = port / "fake-nextos"
        fake.write_bytes(Path("/bin/true").read_bytes() +
                         b"nxinput-gptk-runtime/3\x00" +
                         b"nxinput-gptk-event-evidence/1\x00")
        fake_project = {"nxport": {"executable": "fake-nextos"},
                        "controls": controls}
        expect_failure(
            lambda: renderer._validate_controls_runtime(fake_project, port),
            "lacks defined boundary symbol",
        )

        runtime_symbols = sorted(nxrelease.INPUT_RUNTIME_REQUIRED_SYMBOLS)
        proof = {
            "schema": nxrelease.INPUT_PROOF_SCHEMA, "schema_version": 1,
            "run_id": "directed-host-1", "generation": generation,
            "port_id": "demo", "mapping_sha256": sha(mapping),
            "adapter_contract_sha256": sha(adapter_path), "verdict": "OK",
            "runtime": {"marker": nxrelease.INPUT_RUNTIME_MARKER,
                        "evidence_schema": nxrelease.INPUT_PROOF_SCHEMA,
                        "symbols": runtime_symbols},
            "contexts": [
                {"context": "gameplay", "source": "scene:gameplay",
                 "observed": True},
                {"context": "menu", "source": "scene:main_menu",
                 "observed": True},
            ],
            "sinks": [{"sink": "engine.accept",
                       "symbol": "engine_accept_sink",
                       "method": "runtime-action-registry",
                       "targets": ["ui_accept"], "exists": True}],
            "cases": [
                {"context": "gameplay", "context_source": "scene:gameplay",
                 "control": "A", "event": "press", "decision": "ACTION",
                 "action": "menu.accept", "sink": "engine.accept",
                 "delivery_count": 1},
                {"context": "menu", "context_source": "scene:main_menu",
                 "control": "A", "event": "press", "decision": "ACTION",
                 "action": "menu.accept", "sink": "engine.accept",
                 "delivery_count": 1},
            ],
            "safety": {
                "unknown_context": {"result": "PASSTHROUGH",
                                    "suppressed": False, "delivery_count": 0},
                "missing_sink": {"result": "PASSTHROUGH",
                                 "suppressed": False, "delivery_count": 0},
                "failed_ack": {"result": "FATAL", "native_replay": False},
            },
        }
        normalized = nxrelease.normalize_input_proof(proof, "fixture")
        validate_runtime_schema(proof["runtime"])
        executable_record = {
            "actual_path": port / "demo-nextos", "target": "demo/demo-nextos",
            "kind": "project-linux", "mode": 0o755,
            "sha256": sha(port / "demo-nextos"),
        }
        config = {
            "nxproject_manifest": {"controls": controls},
            "adapter_contract": adapter,
            "adapter_contract_sha256": sha(adapter_path),
            "gptk_defaults_sha256": sha(mapping), "package_id": "demo",
            "generation_id": generation,
        }
        nxrelease._validate_input_proof_contract(
            [executable_record], config, executable_record, normalized)

        godot_proof = json.loads(json.dumps(proof))
        godot_proof["runtime"].update({
            "godot_marker": nxrelease.INPUT_GODOT_RUNTIME_MARKER,
            "frame_proof_marker": nxrelease.INPUT_GODOT_FRAME_PROOF_MARKER,
        })
        godot_proof["runtime"]["symbols"] = sorted(
            set(godot_proof["runtime"]["symbols"]) |
            nxrelease.INPUT_GODOT_FRAME_PROOF_REQUIRED_SYMBOLS)
        normalized_godot = nxrelease.normalize_input_proof(
            godot_proof, "godot-fixture")
        validate_runtime_schema(godot_proof["runtime"])
        if normalized_godot["runtime"].get("godot_marker") != \
                nxrelease.INPUT_GODOT_RUNTIME_MARKER:
            raise AssertionError("Godot runtime marker was not normalized")
        godot_executable = build_live_elf(work, godot=True)
        godot_record = dict(executable_record)
        godot_record.update({
            "actual_path": godot_executable,
            "sha256": sha(godot_executable),
        })
        nxrelease._validate_input_proof_contract(
            [godot_record], config, godot_record, normalized_godot)
        expect_failure(
            lambda: nxrelease._validate_input_proof_contract(
                [godot_record], config, godot_record, normalized),
            "proof and ELF differ on the Godot runtime identity",
        )

        partial_godot = json.loads(json.dumps(proof))
        partial_godot["runtime"]["godot_marker"] = \
            nxrelease.INPUT_GODOT_RUNTIME_MARKER
        expect_failure(
            lambda: validate_runtime_schema(partial_godot["runtime"]),
            "dependent property",
        )
        expect_failure(
            lambda: nxrelease.normalize_input_proof(
                partial_godot, "partial-godot"),
            "canonical live boundary",
        )

        missing_fatal_boundary = json.loads(json.dumps(godot_proof))
        missing_fatal_boundary["runtime"]["symbols"].remove(
            "nxgl_frame_proof_consume_fatal")
        expect_failure(
            lambda: nxrelease.normalize_input_proof(
                missing_fatal_boundary, "missing-fatal-boundary"),
            "omits the canonical Godot fatal boundary",
        )

        missing_marker = work / "demo-nextos-missing-godot"
        missing_bytes = godot_executable.read_bytes().replace(
            nxrelease.INPUT_GODOT_RUNTIME_MARKER.encode("ascii"),
            b"xxinput-godot-runtime/1",
        )
        if missing_bytes == godot_executable.read_bytes():
            raise AssertionError("Godot marker fixture was not embedded")
        missing_marker.write_bytes(missing_bytes)
        missing_record = dict(godot_record)
        missing_record.update({
            "actual_path": missing_marker,
            "sha256": sha(missing_marker),
        })
        expect_failure(
            lambda: nxrelease._validate_input_proof_contract(
                [missing_record], config, missing_record, normalized_godot),
            "partial Godot runtime identity",
        )

        missing_fatal_symbol = work / "demo-nextos-missing-fatal-symbol"
        missing_fatal_bytes = godot_executable.read_bytes().replace(
            b"nxgl_frame_proof_consume_fatal",
            b"xxgl_frame_proof_consume_fatal",
        )
        if missing_fatal_bytes == godot_executable.read_bytes():
            raise AssertionError("Godot fatal symbol fixture was not embedded")
        missing_fatal_symbol.write_bytes(missing_fatal_bytes)
        missing_fatal_record = dict(godot_record)
        missing_fatal_record.update({
            "actual_path": missing_fatal_symbol,
            "sha256": sha(missing_fatal_symbol),
        })
        expect_failure(
            lambda: nxrelease._validate_input_proof_contract(
                [missing_fatal_record], config, missing_fatal_record,
                normalized_godot),
            "runtime symbols differ from the final ELF",
        )

        no_dispatch = json.loads(json.dumps(proof))
        no_dispatch["cases"] = no_dispatch["cases"][:1]
        normalized_bad = nxrelease.normalize_input_proof(
            no_dispatch, "no-dispatch")
        expect_failure(
            lambda: nxrelease._validate_input_proof_contract(
                [executable_record], config, executable_record, normalized_bad),
            "event->decision->sink closure differs",
        )
        bad_sink = json.loads(json.dumps(proof))
        bad_sink["sinks"][0]["symbol"] = "missing_engine_sink"
        normalized_bad_sink = nxrelease.normalize_input_proof(
            bad_sink, "bad-sink")
        expect_failure(
            lambda: nxrelease._validate_input_proof_contract(
                [executable_record], config, executable_record,
                normalized_bad_sink),
            "not a defined port/engine sink",
        )

        lock = {
            "schema": nxrelease.CANDIDATE_LOCK_SCHEMA, "schema_version": 1,
            "executable": "demo/demo-nextos", "sha256": executable_record["sha256"],
            "input_proof": proof,
            "input_proof_receipt_sha256": hashlib.sha256(
                nxrelease.input_proof_receipt_bytes(normalized)).hexdigest(),
            "document_sha256": "b" * 64,
        }
        normalized_lock = nxrelease.normalize_candidate_lock(lock, "candidate")
        config.update({
            "candidate_lock": normalized_lock,
            "candidate_lock_required": True,
            "nxport_manifest": {"executable": "demo-nextos"},
            "port_dir": "demo",
        })
        nxrelease.validate_candidate_lock(
            [executable_record], config, required=True)
        if config.get("candidate_lock_verified") is not True:
            raise AssertionError("full candidate-lock boundary did not close")

    print("nxrelease 0.3.25 GPTK runtime proof: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
