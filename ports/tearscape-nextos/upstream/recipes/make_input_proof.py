#!/usr/bin/env python3
"""Bind Tearscape's generated GPTK map to the real engine ACK sinks."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess


PORT_ID = "tearscape"
ENGINE = "tearscape-nextos"
RUNTIME_MARKER = "nxinput-gptk-runtime/4"
GODOT_RUNTIME_MARKER = "nxinput-godot-runtime/1"
FRAME_PROOF_MARKER = "nxgl-godot-frame-proof/2"
EVIDENCE_SCHEMA = "nxinput-gptk-event-evidence/1"
VIDEO_PROOF_FIELDS = (
    "schema", "schema_version", "run_id", "generation", "port_id",
    "verdict", "reason",
)
RUNTIME_SYMBOLS = sorted({
    "nxgl_frame_proof_consume_fatal",
    "nxgl_frame_proof_is_fatal",
    "nxinput_gptk_load_at",
    "nxinput_gptk_load_receipt_json",
    "nxinput_gptk_parse",
    "nxinput_gptk_decide",
    "nxinput_gptk_live_init",
    "nxinput_gptk_live_register",
    "nxinput_gptk_live_register_vector",
    "nxinput_gptk_live_seal",
    "nxinput_gptk_live_set_context",
    "nxinput_gptk_live_clear_context",
    "nxinput_gptk_live_clear_context_checked",
    "nxinput_gptk_live_is_fatal",
    "nxinput_gptk_live_should_consume",
    "nxinput_gptk_live_feed",
    "nxinput_gptk_live_feed_vector",
    "nxinput_gptk_runtime_marker",
    "nxinput_gptk_event_evidence_schema",
    # V5 schema 4: the owner is projected onto the live V3 dispatch by the
    # schema-4 bridge (nxinput_gptk4), which replaces the V4 PortMaster joydev
    # conversion (nxinput_pm_convert_joydev_mapping / nxinput_pm_normalize_source
    # are no longer part of the live boundary).
    "nxinput_gptk4_load_project_at",
    "nxinput_gptk4_bridge_receipt_json",
})
CONTEXT_SOURCES = {
    "gameplay": "scene:level",
    "menu": "scene:ui",
}
SINKS = {
    "adapter.system.quit": (
        "tearscape_gptk_quit_sink", ["lifecycle:quit-request"]),
    "engine.input.attack": (
        "tearscape_gptk_inputmap_sink", ["attack"]),
    "engine.input.heal": (
        "tearscape_gptk_inputmap_sink", ["heal"]),
    "engine.input.move": (
        "tearscape_gptk_inputmap_vector_sink",
        ["move_down", "move_left", "move_right", "move_up"]),
    "engine.input.open_map": (
        "tearscape_gptk_inputmap_sink", ["open_map"]),
    "engine.input.pause": (
        "tearscape_gptk_inputmap_sink", ["menu"]),
    "engine.input.roll": (
        "tearscape_gptk_inputmap_sink", ["roll"]),
    "engine.input.select_next": (
        "tearscape_gptk_inputmap_sink", ["select_next"]),
    "engine.input.select_prev": (
        "tearscape_gptk_inputmap_sink", ["select_prev"]),
    "engine.input.switch_tool": (
        "tearscape_gptk_inputmap_sink", ["switch_tool"]),
    "engine.input.use_shield": (
        "tearscape_gptk_inputmap_sink", ["use_shield"]),
    "engine.input.use_tool": (
        "tearscape_gptk_inputmap_sink", ["use_tool"]),
    "engine.input.zoom_map": (
        "tearscape_gptk_inputmap_sink", ["zoom_map"]),
    "engine.ui_accept": (
        "tearscape_gptk_inputmap_sink", ["ui_accept"]),
    "engine.ui_cancel": (
        "tearscape_gptk_inputmap_sink", ["ui_cancel"]),
    "engine.ui_direction": (
        "tearscape_gptk_inputmap_vector_sink",
        ["ui_down", "ui_left", "ui_right", "ui_up"]),
}


class ProofError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise ProofError(message)


def regular(path: Path, label: str) -> Path:
    try:
        metadata = path.lstat()
    except OSError as error:
        fail(f"{label} is unavailable: {error}")
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        fail(f"{label} must be a regular non-symlink file")
    return path


def absolute_without_following(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def parse_json_bytes(payload: bytes, label: str) -> dict:
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                fail(f"{label} repeats key {key!r}")
            value[key] = item
        return value

    try:
        value = json.loads(
            payload.decode("utf-8"), object_pairs_hook=unique,
            parse_constant=lambda item: fail(
                f"{label} contains non-JSON constant {item}"),
        )
    except (UnicodeDecodeError, ValueError) as error:
        fail(f"{label} is not strict JSON: {error}")
    if not isinstance(value, dict):
        fail(f"{label} must contain an object")
    return value


def strict_json(path: Path, label: str) -> dict:
    try:
        payload = regular(path, label).read_bytes()
    except OSError as error:
        fail(f"cannot read {label}: {error}")
    return parse_json_bytes(payload, label)


def load_pipeline_module():
    path = Path(__file__).resolve().parent / "build_public_byo.py"
    specification = importlib.util.spec_from_file_location(
        "tearscape_public_pipeline", path,
    )
    if specification is None or specification.loader is None:
        fail("cannot load the Tearscape preparation verifier")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def git_output(repository: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments], text=True,
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False,
    )
    if result.returncode:
        fail(f"git {' '.join(arguments)} failed: {result.stdout.strip()}")
    return result.stdout.strip()


def verify_port_checkout(expected_commit: str) -> Path:
    repository = Path(git_output(
        Path(__file__).resolve().parent.parent,
        "rev-parse", "--show-toplevel",
    )).resolve()
    if git_output(repository, "rev-parse", "HEAD") != expected_commit:
        fail("controls harness checkout differs from the prepared port commit")
    if git_output(repository, "status", "--porcelain", "--untracked-files=normal"):
        fail("controls harness checkout is dirty")
    return repository


def read_video_proof(
        path: Path, generation: str, prepared: Path, repository: Path,
) -> tuple[dict, str]:
    video_path = regular(
        absolute_without_following(path), "external video proof receipt",
    )
    if is_within(video_path, prepared):
        fail("video proof receipt must be external to the prepared root")
    if is_within(video_path, repository):
        fail("video proof receipt must be external to the port checkout")
    parent_metadata = video_path.parent.lstat()
    if (stat.S_ISLNK(parent_metadata.st_mode) or
            not stat.S_ISDIR(parent_metadata.st_mode) or
            parent_metadata.st_uid != os.geteuid() or
            stat.S_IMODE(parent_metadata.st_mode) != 0o700):
        fail("video proof parent must be an owned real directory at 0700")
    metadata_before = video_path.lstat()
    if (metadata_before.st_uid != os.geteuid() or
            metadata_before.st_nlink != 1 or
            stat.S_IMODE(metadata_before.st_mode) != 0o600):
        fail("video proof receipt must be owned, single-link and exactly 0600")
    try:
        payload = video_path.read_bytes()
    except OSError as error:
        fail(f"cannot read video proof receipt: {error}")
    metadata_after = video_path.lstat()
    identity = lambda item: (
        item.st_dev, item.st_ino, item.st_uid, item.st_nlink, item.st_mode,
        item.st_size, item.st_mtime_ns, item.st_ctime_ns,
    )
    if identity(metadata_before) != identity(metadata_after):
        fail("video proof receipt changed while it was read")
    proof = parse_json_bytes(payload, "external video proof receipt")
    if set(proof) != set(VIDEO_PROOF_FIELDS):
        fail("video proof receipt fields are not the C producer contract")
    if (proof.get("schema") != "org.nextos.nxruntime.video-proof" or
            type(proof.get("schema_version")) is not int or
            proof.get("schema_version") != 1 or
            not isinstance(proof.get("run_id"), str) or
            re.fullmatch(r"[A-Za-z0-9._-]{1,192}", proof["run_id"]) is None or
            proof.get("generation") != generation or
            proof.get("port_id") != PORT_ID or
            proof.get("verdict") != "OK" or
            proof.get("reason") != "non-black"):
        fail("video proof is not real OK/non-black evidence for this generation")
    producer_bytes = (json.dumps(
        {field: proof[field] for field in VIDEO_PROOF_FIELDS},
        ensure_ascii=False, separators=(",", ":"),
    ) + "\n").encode("utf-8")
    if payload != producer_bytes:
        fail("video proof bytes differ from the exact C producer line")
    return proof, hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with regular(path, path.name).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def defined_symbols(path: Path) -> set[str]:
    result = subprocess.run(
        ["readelf", "--syms", "--wide", str(path)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode:
        fail(f"readelf failed: {result.stdout.strip()}")
    symbols = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[6] not in ("UND", "Name"):
            symbols.add(fields[7].split("@", 1)[0])
    return symbols


def canonical_receipt(value: dict) -> bytes:
    return (json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
    ) + "\n").encode("utf-8")


def parse_harness_evidence(output: str) -> tuple[list[dict], list[dict], dict]:
    """Parse only cases the compiled live-boundary harness actually ran."""
    contexts = []
    cases = []
    safety_seen = {}
    for raw_line in output.splitlines():
        if not raw_line.startswith("NXGPTK_PROOF\t"):
            continue
        fields = raw_line.split("\t")
        kind = fields[1] if len(fields) > 1 else ""
        if kind == "CONTEXT" and len(fields) == 4:
            contexts.append({
                "context": fields[2], "source": fields[3],
                "observed": True,
            })
        elif kind == "CASE" and len(fields) == 9 and fields[8] == "1":
            cases.append({
                "context": fields[2], "context_source": fields[3],
                "control": fields[4], "event": fields[5],
                "decision": "ACTION", "action": fields[6],
                "sink": fields[7], "delivery_count": 1,
            })
        elif kind == "SAFETY" and len(fields) == 5 and fields[4] == "0":
            name = fields[2]
            if name in safety_seen:
                fail(f"controls harness repeated safety observation {name}")
            safety_seen[name] = fields[3]
        else:
            fail(f"controls harness emitted malformed proof line: {raw_line}")
    contexts.sort(key=lambda item: item["context"])
    cases.sort(key=lambda item: (
        item["context"], item["control"], item["action"], item["sink"],
        item["event"], item["context_source"],
    ))
    if len({(item["context"], item["source"]) for item in contexts}) != \
            len(contexts):
        fail("controls harness repeated a context observation")
    case_key = lambda item: (
        item["context"], item["context_source"], item["control"],
        item["event"], item["action"], item["sink"],
    )
    if len({case_key(item) for item in cases}) != len(cases):
        fail("controls harness repeated a delivered case")
    expected_safety = {
        "unknown_context": "PASSTHROUGH",
        "missing_sink": "PASSTHROUGH",
        "failed_ack": "FATAL",
    }
    if safety_seen != expected_safety:
        fail("controls harness did not execute the exact safety closure")
    return contexts, cases, {
        "unknown_context": {
            "result": "PASSTHROUGH", "suppressed": False,
            "delivery_count": 0,
        },
        "missing_sink": {
            "result": "PASSTHROUGH", "suppressed": False,
            "delivery_count": 0,
        },
        "failed_ack": {"result": "FATAL", "native_replay": False},
    }


def pretty_json(value: dict) -> bytes:
    return (json.dumps(
        value, ensure_ascii=False, sort_keys=True, indent=2,
    ) + "\n").encode("utf-8")


def publish_read_only(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    parent_metadata = path.parent.lstat()
    if (stat.S_ISLNK(parent_metadata.st_mode) or
            not stat.S_ISDIR(parent_metadata.st_mode) or
            parent_metadata.st_uid != os.geteuid() or
            stat.S_IMODE(parent_metadata.st_mode) != 0o700):
        fail("proof output parent must be an owned non-symlink directory at 0700")
    if path.exists() or path.is_symlink():
        fail(f"proof output already exists: {path}")
    descriptor = os.open(
        path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600,
    )
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.fchmod(descriptor, 0o444)
        metadata = os.fstat(descriptor)
        if (not stat.S_ISREG(metadata.st_mode) or
                metadata.st_uid != os.geteuid() or metadata.st_nlink != 1 or
                stat.S_IMODE(metadata.st_mode) != 0o444):
            fail("proof output did not freeze as an owned single-link 0444 file")
    finally:
        os.close(descriptor)
    parent_descriptor = os.open(
        path.parent,
        os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
    )
    try:
        os.fsync(parent_descriptor)
    finally:
        os.close(parent_descriptor)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Freeze GPTK plus real non-black evidence for one preparation",
    )
    parser.add_argument("--prepared-root", required=True, type=Path)
    parser.add_argument("--video-proof", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    prepared = absolute_without_following(args.prepared_root)
    pipeline = load_pipeline_module()
    try:
        preparation = pipeline.verify_prepared_root(prepared)
    except pipeline.PipelineError as error:
        fail(f"prepared candidate is invalid: {error}")
    if (prepared / pipeline.BUNDLE_ATTEMPT_FILE).exists():
        fail("prepared candidate was already consumed by a bundle attempt")
    repository = verify_port_checkout(preparation["port_commit"])
    scaffold = prepared / "scaffold"
    port = scaffold / PORT_ID
    engine = regular(port / ENGINE, "prepared Tearscape engine")
    engine_sha256 = sha256_file(engine)
    if engine_sha256 != preparation["engine_sha256"]:
        fail("prepared engine differs from its preparation receipt")
    engine_bytes = engine.read_bytes()
    for identity in (
            RUNTIME_MARKER, GODOT_RUNTIME_MARKER, FRAME_PROOF_MARKER,
            EVIDENCE_SCHEMA,
    ):
        if identity.encode("ascii") not in engine_bytes:
            fail(f"engine lacks {identity}")

    symbols = defined_symbols(engine)
    required_symbols = set(RUNTIME_SYMBOLS)
    required_symbols.update(symbol for symbol, _targets in SINKS.values())
    missing = sorted(required_symbols - symbols)
    if missing:
        fail("engine lacks defined input proof symbols: " + ", ".join(missing))

    project = strict_json(port / "nxproject.json", "generated nxproject")
    adapter = strict_json(
        port / "adapter/adapter-contract.json", "generated adapter contract")
    generation = strict_json(port / "GENERATION.json", "generation receipt")
    controls = project.get("controls")
    adapter_input = adapter.get("input") if isinstance(adapter, dict) else None
    profiles = controls.get("controller_profiles") if isinstance(controls, dict) else None
    adapter_profiles = adapter.get("input_controller_profiles") if isinstance(adapter, dict) else None
    if (not isinstance(controls, dict) or
            controls.get("runtime_mapping") != "nxinput-gptk" or
            not isinstance(adapter_input, dict) or
            adapter_input.get("actions") != controls.get("actions") or
            adapter_input.get("contexts") != controls.get("contexts")):
        fail("generated project and promoted adapter input differ")
    bundled_profiles = regular(port / "controllers.nxb", "controller profile bundle")
    profiles_sha256 = sha256_file(bundled_profiles)
    # Schema 4 (nxinput 0.11.0, V5): the Xbox surface is POSITIONAL, so there is
    # exactly ONE controller bundle and no modern/retro FACE_LAYOUT variant pair.
    # Authority 3 is that single controllers.nxb, pinned by SHA-256 to the exact
    # staged bytes and identical across project, promoted adapter and disk.
    if (not isinstance(profiles, dict) or
            profiles != {
                "bundle": "controllers.nxb", "enabled": True,
                "sha256": profiles_sha256,
            } or adapter_profiles != profiles):
        fail("generated authority-3 controller bundle differs across project, adapter and bytes")
    actions = controls["actions"]
    contexts = controls["contexts"]
    if set(contexts) != set(CONTEXT_SOURCES):
        fail("Tearscape proof sources do not cover the generated contexts")
    actions_by_id = {action["id"]: action for action in actions}
    expected_sinks = {
        sink for action in actions for sink in action.get("sinks", [])
    }
    if expected_sinks != set(SINKS):
        fail("port-owned sink registry differs from nxproject")

    expected_context_proofs = [
        {"context": context, "source": CONTEXT_SOURCES[context],
         "observed": True}
        for context in sorted(contexts)
    ]
    sink_proofs = [
        {"sink": sink, "symbol": SINKS[sink][0],
         "method": "runtime-action-registry",
         "targets": sorted(SINKS[sink][1]), "exists": True}
        for sink in sorted(SINKS)
    ]
    events = {"button": "press", "axis": "axis", "vector": "motion"}
    expected_cases = []
    for context in sorted(contexts):
        for control, action_id in contexts[context].items():
            if action_id in ("native", "null"):
                continue
            action = actions_by_id.get(action_id)
            if action is None or action.get("kind") not in events:
                fail(f"binding {context}.{control} lacks a declared action")
            for sink in action["sinks"]:
                expected_cases.append({
                    "context": context,
                    "context_source": CONTEXT_SOURCES[context],
                    "control": control,
                    "event": events[action["kind"]],
                    "decision": "ACTION",
                    "action": action_id,
                    "sink": sink,
                    "delivery_count": 1,
                })
    expected_cases.sort(key=lambda item: (
        item["context"], item["control"], item["action"], item["sink"],
        item["event"], item["context_source"],
    ))

    generation_id = generation.get("generation_id")
    if (not isinstance(generation_id, str) or len(generation_id) not in (32, 64)
            or any(character not in "0123456789abcdef"
                   for character in generation_id)):
        fail("generated receipt lacks a canonical generation id")
    if generation_id != preparation["generation"]:
        fail("generated receipt differs from the prepared generation")
    video_proof, video_proof_sha256 = read_video_proof(
        args.video_proof, generation_id, prepared, repository,
    )
    mapping = regular(
        port / "defaults/NEXTOSCONTROLLERS.gptk", "generated GPTK default")
    harness = regular(
        Path(__file__).resolve().parent.parent / "tests/controls/run.sh",
        "Tearscape controls harness")
    result = subprocess.run(
        ["bash", str(harness), str(mapping)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False,
    )
    print(result.stdout, end="")
    if result.returncode != 0:
        fail("exact generated GPTK runtime harness failed")
    context_proofs, cases, safety = parse_harness_evidence(result.stdout)
    if context_proofs != expected_context_proofs:
        fail("controls harness did not observe the exact generated contexts")
    if cases != expected_cases:
        fail("controls harness did not deliver the exact generated binding closure")
    try:
        if pipeline.verify_prepared_root(prepared) != preparation:
            fail("prepared receipt changed while the input proof ran")
    except pipeline.PipelineError as error:
        fail(f"prepared candidate drifted while the input proof ran: {error}")
    if verify_port_checkout(preparation["port_commit"]) != repository:
        fail("controls harness repository changed while the input proof ran")
    adapter_path = regular(
        port / "adapter/adapter-contract.json", "generated adapter contract")
    proof = {
        "schema": EVIDENCE_SCHEMA,
        "schema_version": 1,
        "run_id": f"tearscape-{generation_id[:16]}-{engine_sha256[:16]}",
        "generation": generation_id,
        "port_id": PORT_ID,
        "mapping_sha256": sha256_file(mapping),
        "adapter_contract_sha256": sha256_file(adapter_path),
        "verdict": "OK",
        "runtime": {
            "marker": RUNTIME_MARKER,
            "godot_marker": GODOT_RUNTIME_MARKER,
            "frame_proof_marker": FRAME_PROOF_MARKER,
            "evidence_schema": EVIDENCE_SCHEMA,
            "symbols": RUNTIME_SYMBOLS,
        },
        "contexts": context_proofs,
        "sinks": sink_proofs,
        "cases": cases,
        "safety": safety,
    }
    lock = {
        "executable": f"{PORT_ID}/{ENGINE}",
        "input_proof": proof,
        "input_proof_receipt_sha256": hashlib.sha256(
            canonical_receipt(proof)).hexdigest(),
        "schema": "nxrelease-candidate-lock-v1",
        "schema_version": 1,
        "sha256": engine_sha256,
        "video_proof": video_proof,
        "video_proof_receipt_sha256": video_proof_sha256,
    }
    if not args.output.name or args.output.name in (".", ".."):
        fail("candidate lock output path is invalid")
    output = args.output.parent.resolve() / args.output.name
    if is_within(output, prepared) or is_within(output, repository):
        fail("candidate lock output must be external to preparation and source")
    publish_read_only(output, pretty_json(lock))
    print(
        "TEARSCAPE INPUT PROOF: PASS "
        f"contexts={len(context_proofs)} sinks={len(sink_proofs)} "
        f"cases={len(cases)} video_run={video_proof['run_id']} lock={output}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ProofError as error:
        print(f"TEARSCAPE INPUT PROOF: FAIL: {error}", file=os.sys.stderr)
        raise SystemExit(1)
