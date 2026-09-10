#!/usr/bin/env python3
"""Derive Tearscape's public build receipts and nxproject from real bytes.

This is a pre-freeze authoring tool.  The final package builder is read-only:
it verifies these documents, the clean port commit and the clean Framework V4
commit before it creates the only candidate ZIP.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys


PORT = Path(__file__).resolve().parent.parent
PORT_ID = "tearscape"
ENGINE = "tearscape-nextos"
# nxinput is independently released from this mixed-component Framework V4
# commit. The tag must remain an annotated tag resolving to these exact bytes.
FRAMEWORK_COMMIT = "ba1da0e7b0482c4dbad76b7e36afe04de57fa5c3"
RELEASE_FRAMEWORK_COMMIT = "275a8fb47c2f2bf9a452734dec5c33ceb4bd549f"
NXRELEASE_COMPONENT_VERSION = "0.4.9"
NXINPUT_OWNER_COMMIT = "ba1da0e7b0482c4dbad76b7e36afe04de57fa5c3"
# See build_public_byo.py: candidate-commit-pinned pins by exact commit and
# component VERSION, with NO tag and no promotion path.
FRAMEWORK_STATE = "candidate-commit-pinned"
NXINPUT_COMPONENT_VERSION = "0.11.8"
NXCOMPAT_COMPONENT_VERSION = "0.5.3"
NXINPUT_FRAMEWORK_TAG = "nxinput-v0.9.0"
NXINPUT_FRAMEWORK_TAG_OBJECT = "32ed6553e16bc4195baed988ea9a9d3849fd86b1"
NXINPUT_FRAMEWORK_TAG_OBJECT_SHA256 = \
    "2accbb023971fa47c4e73d23ee2e46472572b857f94911cabca3d9cf814c95b2"
NXINPUT_0_9_PIN_PENDING = False
NXGL_OWNER_COMMIT = "ba1da0e7b0482c4dbad76b7e36afe04de57fa5c3"
GODOT_COMMIT = "14d19694e0c88a3f9e82d899a0400f27a24c176e"
SDL_COMMIT = "f5e5f6588921eed3d7d048ce43d9eb1ff0da0ffc"
SOURCE_DATE_EPOCH = 1771254831
NUPKG_SHA256 = "051bfc73f47f7d28bf9fb4cee616df71063ffc3da8861ec4f0645b0e73476d78"
HEX64 = re.compile(r"^[0-9a-f]{64}$")
CONTROLLER_SOURCE_FILE_SHA256 = "4238023705e73ea94b468975d1d15f71cc8d63f8880b3cd205aebbe2a75f2a0f"
CONTROLLER_SOURCE_LINE_SHA256 = "da0447deece5800c7c845652cb1a73893e706cbdb08601e33a2b74c5da3daa64"
# V5 (schema 4): ONE positional bundle. The muOS modern/retro user-preference
# variants of 0.10.0 were retired with FACE_LAYOUT; the live CFW database is the
# authority for that legend and the base bundle stays invariant.
HEX40 = re.compile(r"^[0-9a-f]{40}$")
ENGINE_BUILD_RECEIPT = "BUILD-RECEIPT.json"
ENGINE_BUILD_RECEIPT_SCHEMA = "org.nextos.tearscape.engine-build-receipt"
BUILD_RECIPE_PATHS = (
    "build_low_glibc.sh",
    "recipes/audit_low_glibc.sh",
    "recipes/compiler_wrapper.sh",
    "recipes/gcc10_buster_compat.h",
    "recipes/low_glibc_container.sh",
    "recipes/vendor_sdl_3_2_30.sh",
)


def fail(message: str) -> None:
    raise SystemExit("refresh-release-inputs: " + message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def regular(path: Path, label: str) -> Path:
    try:
        metadata = path.lstat()
    except OSError as error:
        fail(f"{label} is unavailable: {error}")
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        fail(f"{label} must be a regular non-symlink file")
    return path


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode()


def strict_json(path: Path, label: str) -> dict:
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                fail(f"{label} repeats JSON key {key!r}")
            value[key] = item
        return value
    try:
        value = json.loads(
            regular(path, label).read_text(encoding="utf-8"),
            object_pairs_hook=unique,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot parse {label}: {error}")
    if not isinstance(value, dict):
        fail(f"{label} must be a JSON object")
    return value


def write_json(path: Path, value: object) -> None:
    path.write_bytes(canonical_json(value))
    path.chmod(0o644)


def publish_json(path: Path, value: object, check: bool) -> None:
    payload = canonical_json(value)
    if check:
        if regular(path, path.name).read_bytes() != payload:
            fail(f"derived release input is stale: {path.name}")
        return
    path.write_bytes(payload)
    path.chmod(0o644)


def command(*arguments: str, cwd: Path | None = None) -> str:
    result = subprocess.run(
        list(arguments), cwd=cwd, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False,
    )
    if result.returncode:
        fail(f"command failed ({arguments[0]}): {result.stdout.strip()}")
    return result.stdout.strip()


def max_glibc(path: Path) -> str:
    output = command("readelf", "--version-info", str(path))
    versions = [tuple(map(int, match.split("."))) for match in re.findall(r"GLIBC_([0-9]+(?:[.][0-9]+)+)", output)]
    if not versions:
        fail(f"ELF lacks a GLIBC version requirement: {path.name}")
    value = max(versions)
    return "GLIBC_" + ".".join(map(str, value))


def tree_identity(root: Path) -> tuple[int, str]:
    files: list[Path] = []

    def visit(directory: Path) -> None:
        try:
            entries = sorted(os.scandir(directory), key=lambda item: os.fsencode(item.name))
        except OSError as error:
            fail(f"cannot traverse source tree {directory}: {error}")
        for entry in entries:
            metadata = entry.stat(follow_symlinks=False)
            path = Path(entry.path)
            if stat.S_ISDIR(metadata.st_mode):
                visit(path)
            elif stat.S_ISREG(metadata.st_mode):
                files.append(path)
            else:
                fail(f"source tree contains a symlink or special file: {path}")

    visit(root)
    if not files:
        fail(f"empty source tree: {root}")
    return selected_files_identity(root, files)


def selected_files_identity(root: Path, files: list[Path]) -> tuple[int, str]:
    digest = hashlib.sha256()
    for path in sorted(files, key=lambda item: os.fsencode(item.relative_to(root).as_posix())):
        relative = path.relative_to(root).as_posix()
        payload = path.read_bytes()
        mode = "100755" if path.stat().st_mode & 0o111 else "100644"
        digest.update(mode.encode() + b"\0" + relative.encode() + b"\0")
        digest.update(str(len(payload)).encode() + b"\0")
        digest.update(hashlib.sha256(payload).hexdigest().encode() + b"\n")
    return len(files), digest.hexdigest()


def build_recipe_identity(port: Path) -> tuple[int, str]:
    files = []
    for relative in BUILD_RECIPE_PATHS:
        files.append(regular(port / relative, f"engine build recipe {relative}"))
    return selected_files_identity(port, files)


def verify_engine_build_receipt(
    engine_dir: Path, *, port_root: Path = PORT, verify_git: bool = True,
) -> tuple[dict, str]:
    """Bind engine bytes to the exact source/recipe snapshot used once."""
    receipt_path = regular(
        engine_dir / ENGINE_BUILD_RECEIPT, "immutable engine build receipt",
    )
    metadata = receipt_path.lstat()
    if (metadata.st_uid != os.geteuid() or metadata.st_nlink != 1 or
            stat.S_IMODE(metadata.st_mode) != 0o444):
        fail("engine build receipt must be owned, single-link and mode 0444")
    receipt = strict_json(receipt_path, "engine build receipt")
    expected_fields = {
        "build", "causal_inputs", "outputs", "port_head", "port_id",
        "prepared_source_tree", "schema", "schema_version",
        "source_date_epoch",
    }
    if set(receipt) != expected_fields:
        fail("engine build receipt fields are not canonical")
    if (receipt.get("schema") != ENGINE_BUILD_RECEIPT_SCHEMA or
            receipt.get("schema_version") != 1 or
            receipt.get("port_id") != PORT_ID or
            receipt.get("source_date_epoch") != SOURCE_DATE_EPOCH or
            not isinstance(receipt.get("port_head"), str) or
            HEX40.fullmatch(receipt["port_head"]) is None):
        fail("engine build receipt identity is not canonical")
    expected_build = {
        "builder_image_id": "sha256:57cd7c3a7a273101a6485ba99423ee568157882804b1124b4dd04266317710de",
        "clean_build_count": 1,
        "network": "none",
        "priority": "nice-10",
        "workers": receipt.get("build", {}).get("workers")
            if isinstance(receipt.get("build"), dict) else None,
    }
    if (receipt.get("build") != expected_build or
            expected_build["workers"] not in (1, 2)):
        fail("engine build receipt execution boundary differs")
    source_count, source_digest = tree_identity(port_root / "src")
    recipe_count, recipe_digest = build_recipe_identity(port_root)
    expected_causal = {
        "source_tree": {"files": source_count, "sha256": source_digest},
        "recipe": {
            "files": recipe_count,
            "paths": list(BUILD_RECIPE_PATHS),
            "sha256": recipe_digest,
        },
    }
    if receipt.get("causal_inputs") != expected_causal:
        fail("engine build receipt differs from the current source/recipe bytes")
    prepared = receipt.get("prepared_source_tree")
    if (not isinstance(prepared, dict) or set(prepared) != {"files", "sha256"} or
            type(prepared.get("files")) is not int or prepared["files"] <= 0 or
            not isinstance(prepared.get("sha256"), str) or
            HEX64.fullmatch(prepared["sha256"]) is None):
        fail("engine build receipt lacks a canonical prepared-source identity")
    expected_outputs = {}
    for relative in (ENGINE, "shimlib/libEGL.so", "shimlib/libGLESv2.so"):
        output = regular(engine_dir / relative, f"engine build output {relative}")
        expected_outputs[relative] = {
            "sha256": sha256_file(output), "size": output.stat().st_size,
        }
    if receipt.get("outputs") != expected_outputs:
        fail("engine build receipt output identity differs from the supplied engine")
    if verify_git:
        repository = Path(command("git", "rev-parse", "--show-toplevel", cwd=port_root))
        result = subprocess.run(
            ["git", "merge-base", "--is-ancestor", receipt["port_head"], "HEAD"],
            cwd=repository, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode != 0:
            fail("engine build HEAD is not an ancestor of the current port HEAD")
    return receipt, sha256_file(receipt_path)


def verify_controller_profile_source(framework: Path) -> tuple[Path, dict]:
    evidence_path = regular(
        PORT / "evidence/controller-profile-source.json",
        "controller profile source evidence",
    )
    try:
        evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as error:
        fail(f"controller profile source evidence is invalid: {error}")
    profile = evidence.get("profile") if isinstance(evidence, dict) else None
    scope = evidence.get("scope") if isinstance(evidence, dict) else None
    if (evidence.get("schema") != "org.nextos.controller-profile-source" or
            evidence.get("schema_version") != 1 or
            not isinstance(evidence.get("capture"), dict) or
            not isinstance(profile, dict) or
            not isinstance(scope, dict) or
            profile.get("bundle") != "controllers.nxb" or
            profile.get("guid") != "190000004b4800000011000000010000" or
            profile.get("source_file_sha256") != CONTROLLER_SOURCE_FILE_SHA256 or
            profile.get("source_line_number") != 1110 or
            profile.get("source_line_sha256") != CONTROLLER_SOURCE_LINE_SHA256 or
            scope.get("candidate_admission_required") is not True or
            scope.get("universal_fallback_claimed") is not False):
        fail("controller profile source evidence differs from the measured contract")
    # V5 (schema 4): the historical modern/retro variant records may remain in
    # the evidence file as history; no variant bundle exists or is verified.
    bundle = regular(PORT / "controllers.nxb", "controller profile bundle")
    lines = [line for line in bundle.read_bytes().splitlines(keepends=True)
             if line and not line.startswith(b"#") and
             not line.startswith(b"NXCONTROLLER_PROFILES/")]
    if (len(lines) != 1 or
            hashlib.sha256(lines[0]).hexdigest() != CONTROLLER_SOURCE_LINE_SHA256):
        fail("the invariant base bundle must retain ONLY the GO-Super line")
    if any(line.startswith(b"19000000010000000100000000010000,")
           for line in lines):
        fail("the mutable muOS GUID re-entered the invariant base bundle")
    return evidence_path, evidence


def pinned_tree_identity(repository: Path, selected: Path) -> tuple[int, str]:
    """Reproduce the framework pin's recursive byte-order traversal."""
    files: list[Path] = []

    def visit(directory: Path) -> None:
        for entry in sorted(os.scandir(directory), key=lambda item: os.fsencode(item.name)):
            if entry.name == "__pycache__":
                continue
            metadata = entry.stat(follow_symlinks=False)
            path = Path(entry.path)
            if stat.S_ISLNK(metadata.st_mode):
                fail(f"framework pin traverses a symlink: {path}")
            if stat.S_ISDIR(metadata.st_mode):
                visit(path)
            elif stat.S_ISREG(metadata.st_mode):
                files.append(path)
            else:
                fail(f"framework pin contains a special file: {path}")

    visit(selected)
    aggregate = hashlib.sha256()
    for path in files:
        payload = path.read_bytes()
        mode = "100755" if path.stat().st_mode & 0o111 else "100644"
        logical = path.relative_to(repository).as_posix()
        aggregate.update(mode.encode() + b"\0" + logical.encode() + b"\0")
        aggregate.update(str(len(payload)).encode() + b"\0")
        aggregate.update(hashlib.sha256(payload).hexdigest().encode() + b"\n")
    return len(files), aggregate.hexdigest()


def framework_pin(framework: Path) -> dict:
    public = strict_json(PORT / "FRAMEWORK-PIN.json", "FRAMEWORK-PIN.json")
    private = strict_json(
        PORT / "recipes/framework-release-pin-v1.json", "framework pin",
    )
    if public != private:
        fail("public and recipe framework pins differ")
    if public.get("framework_commit") != FRAMEWORK_COMMIT:
        fail("framework pin names the wrong integration commit")
    if public.get("framework_state") != FRAMEWORK_STATE:
        fail("framework pin state differs from the recipe")
    if public.get("components", {}).get("nxinput") != NXINPUT_COMPONENT_VERSION:
        fail(f"framework pin does not name nxinput {NXINPUT_COMPONENT_VERSION}")
    if command("git", "rev-parse", "HEAD", cwd=framework) != FRAMEWORK_COMMIT:
        fail("framework checkout is not the pinned integration commit")
    if command("git", "status", "--porcelain", cwd=framework):
        fail("framework checkout is dirty")
    if FRAMEWORK_STATE == "component-tag-pinned":
        if public.get("component_tags") != {"nxinput": NXINPUT_FRAMEWORK_TAG}:
            fail("framework pin does not name the exact nxinput component tag")
        if public.get("component_tag_objects") != {
                "nxinput": NXINPUT_FRAMEWORK_TAG_OBJECT}:
            fail("framework pin does not name the exact nxinput tag object")
        if public.get("component_tag_object_sha256") != {
                "nxinput": NXINPUT_FRAMEWORK_TAG_OBJECT_SHA256}:
            fail("framework pin does not name the exact nxinput tag-object digest")
        tag_ref = f"refs/tags/{NXINPUT_FRAMEWORK_TAG}"
        if command("git", "cat-file", "-t", tag_ref, cwd=framework) != "tag":
            fail("nxinput component release is not an annotated tag")
        if command("git", "rev-parse", tag_ref, cwd=framework) != NXINPUT_FRAMEWORK_TAG_OBJECT:
            fail("nxinput component tag object differs from the immutable pin")
        tag_payload = subprocess.run(
            ["git", "cat-file", "tag", tag_ref], cwd=framework,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False,
        )
        logical_tag_object = (
            b"tag " + str(len(tag_payload.stdout)).encode() + b"\0" + tag_payload.stdout
        )
        if (tag_payload.returncode != 0 or hashlib.sha256(
                logical_tag_object).hexdigest() != NXINPUT_FRAMEWORK_TAG_OBJECT_SHA256):
            fail("nxinput component tag object differs from its SHA-256 pin")
        if command("git", "rev-parse", f"{tag_ref}^{{commit}}", cwd=framework) != FRAMEWORK_COMMIT:
            fail("nxinput component tag does not resolve to the pinned commit")
    elif FRAMEWORK_STATE == "candidate-commit-pinned":
        # NO tag exists or may be created. The candidate stays unpublishable:
        # physical_support_proven is forced False below and the packager
        # refuses to promote this state.
        if public.get("component_tags") not in (None, {}):
            fail("a candidate pin must not name component tags")
        version = command(
            "git", "show", f"{FRAMEWORK_COMMIT}:framework/nxinput/VERSION",
            cwd=framework)
        if version.strip() != NXINPUT_COMPONENT_VERSION:
            fail("candidate pin: released nxinput VERSION mismatch")
    else:
        fail(f"unknown framework pin state: {FRAMEWORK_STATE}")
    for record in public.get("roots", []):
        selected = framework / record["path"]
        if selected.is_file() and not selected.is_symlink():
            payload = selected.read_bytes()
            mode = "100755" if selected.stat().st_mode & 0o111 else "100644"
            logical = selected.relative_to(framework).as_posix()
            digest = hashlib.sha256(
                mode.encode() + b"\0" + logical.encode() + b"\0" +
                str(len(payload)).encode() + b"\0" +
                hashlib.sha256(payload).hexdigest().encode() + b"\n"
            ).hexdigest()
            count = 1
        else:
            count, digest = pinned_tree_identity(framework, selected)
        if count != record.get("files") or digest != record.get("tree_sha256"):
            fail(f"framework root differs from its pin: {record['path']}")
    return public


def verify_nxinput_vendor(framework: Path) -> None:
    sources = {
        "nxinput_authority.c": "src/nxinput_authority.c",
        "nxinput_authority.h": "include/nxinput_authority.h",
        "nxinput_godot.c": "src/nxinput_godot.c",
        "nxinput_godot.h": "include/nxinput_godot.h",
        "nxinput_godot_runtime.h": "include/nxinput_godot_runtime.h",
        "nxinput_gptk.c": "src/nxinput_gptk.c",
        "nxinput_gptk.h": "include/nxinput_gptk.h",
        "nxinput_gptk_live.c": "src/nxinput_gptk_live.c",
        "nxinput_gptk_live.h": "include/nxinput_gptk_live.h",
        "nxinput_gptk_loader.c": "src/nxinput_gptk_loader.c",
        "nxinput_gptk_loader.h": "include/nxinput_gptk_loader.h",
        "nxinput_gptk_motion.c": "src/nxinput_gptk_motion.c",
        "nxinput_gptk_motion.h": "include/nxinput_gptk_motion.h",
        "nxinput_portmaster.c": "src/nxinput_portmaster.c",
        "nxinput_portmaster.h": "include/nxinput_portmaster.h",
        "nxinput_sdl.c": "src/nxinput_sdl.c",
        "nxinput_sdl.h": "include/nxinput_sdl.h",
        "nxinput_sdl_seam.c": "src/nxinput_sdl_seam.c",
        "nxinput_sdl_seam.h": "include/nxinput_sdl_seam.h",
        "nxinput_sovereign.c": "src/nxinput_sovereign.c",
        "nxinput_sovereign.h": "include/nxinput_sovereign.h",
        "nxc6_glue.c": "engine-glue/nxc6_glue.c",
        "nxc6_glue.h": "engine-glue/nxc6_glue.h",
        "sdl3-3.2.30-nxc6-seam.patch": "engine-patches/sdl3-3.2.30-nxc6-seam.patch",
        "nxinput_gptk_preinit.c": "src/nxinput_gptk_preinit.c",
        "nxinput_gptk_preinit.h": "include/nxinput_gptk_preinit.h",
        "nxinput_livedb.c": "src/nxinput_livedb.c",
        "nxinput_livedb.h": "include/nxinput_livedb.h",
        "nxinput_provider.c": "src/nxinput_provider.c",
        "nxinput_provider.h": "include/nxinput_provider.h",
        "nxinput_provider_linux.c": "src/nxinput_provider_linux.c",
        "nxinput_provider_linux.h": "include/nxinput_provider_linux.h",
        "nxinput_sha256.c": "src/nxinput_sha256.c",
        "nxinput_sha256.h": "include/nxinput_sha256.h",
        "nxinput_translate.c": "src/nxinput_translate.c",
        "nxinput_translate.h": "include/nxinput_translate.h",
        "nxinput_decision.c": "src/nxinput_decision.c",
        "nxinput_decision.h": "include/nxinput_decision.h",
        "nxinput_gptk4.c": "src/nxinput_gptk4.c",
        "nxinput_gptk4.h": "include/nxinput_gptk4.h",
        "nxinput_gptk4_bridge.c": "src/nxinput_gptk4_bridge.c",
        "nxinput_gptk4_bridge.h": "include/nxinput_gptk4_bridge.h",
        "nxinput_gptk4_preinit.c": "src/nxinput_gptk4_preinit.c",
        "nxinput_gptk4_preinit.h": "include/nxinput_gptk4_preinit.h",
        "nx-gptk4-host-gate.c": "tools/nx-gptk4-host-gate.c",
    }
    owner = framework / "framework/nxinput"
    if regular(owner / "VERSION", "framework nxinput version").read_text(
            encoding="utf-8").strip() != NXINPUT_COMPONENT_VERSION:
        fail(f"pinned framework nxinput version is not {NXINPUT_COMPONENT_VERSION}")
    vendor = PORT / "src/nxinput"
    for local, upstream in sources.items():
        local_path = regular(vendor / local, f"vendored nxinput {local}")
        upstream_path = regular(owner / upstream, f"framework nxinput {upstream}")
        if local_path.read_bytes() != upstream_path.read_bytes():
            fail(f"vendored nxinput differs from pinned framework: {local}")


def verify_nxgl_vendor(framework: Path) -> None:
    """Require the exact reviewed nxgl 0.3.5 fatal frame-proof plus Godot glue."""
    root = framework / "framework/nxgl"
    vendor = (PORT / "src/godot_engine/v4-universal/platform/linuxbsd/fbdev")
    version = regular(framework / "framework/nxgl/VERSION", "framework nxgl version")
    if version.read_text(encoding="utf-8").strip() != "0.3.5":
        fail("pinned framework nxgl version is not 0.3.5")
    sources = {
        "nxgl_frame_proof_adapter.c": "adapters/nxgl_frame_proof_adapter.c",
        "nxgl_frame_proof_adapter.h": "adapters/nxgl_frame_proof_adapter.h",
        "nxgl_godot_frame_proof.h": "engine-glue/nxgl_godot_frame_proof.h",
    }
    for name, upstream in sources.items():
        local_path = regular(vendor / name, f"vendored nxgl {name}")
        upstream_path = regular(root / upstream, f"framework nxgl {name}")
        if local_path.read_bytes() != upstream_path.read_bytes():
            fail(f"vendored nxgl differs from pinned framework: {name}")


def verify_nxcompat_vendor(framework: Path) -> None:
    """V5: the aspect decision is nxcompat's, so its bytes are pinned here too.

    Until 0.2.17 this port carried `nx_aspect_policy.h` with its OWN copy of
    the ratio rule that `nxcompat_video` already implemented. Two authorities
    for one decision: the next Godot port would have copied it again. The
    policy now comes from the framework and only the engine translation stays
    in the port, so the framework bytes have to be byte-for-byte pinned.
    """
    root = framework / "framework/nxcompat"
    vendor = (PORT / "src/godot_engine/v4-universal/platform/linuxbsd/fbdev")
    version = regular(root / "VERSION", "framework nxcompat version")
    if version.read_text(encoding="utf-8").strip() != NXCOMPAT_COMPONENT_VERSION:
        fail(f"pinned framework nxcompat version is not {NXCOMPAT_COMPONENT_VERSION}")
    sources = {
        "nxcompat_video.c": "src/nxcompat_video.c",
        "nxcompat_video.h": "include/nxcompat_video.h",
    }
    for name, upstream in sources.items():
        local_path = regular(vendor / name, f"vendored nxcompat {name}")
        upstream_path = regular(root / upstream, f"framework nxcompat {name}")
        if local_path.read_bytes() != upstream_path.read_bytes():
            fail(f"vendored nxcompat differs from pinned framework: {name}")


def release_artifact(base: Path, manifest_relative: str) -> Path:
    manifest = json.loads((base / manifest_relative).read_text(encoding="utf-8"))
    return regular(base / manifest["artifacts"]["aarch64"]["path"], "framework release artifact")


def runtime_records(
    engine_dir: Path, framework: Path, runtime_manifest: dict,
) -> list[dict]:
    nxextract = framework / "suportando_outros_devices/extrator-universal"
    values = [
        ("executable", ENGINE, "0755", engine_dir / ENGINE, None),
        ("private-library", "lib/libEGL.so", "0755", engine_dir / "shimlib/libEGL.so", None),
        ("private-library", "lib/libGLESv2.so", "0755", engine_dir / "shimlib/libGLESv2.so", None),
    ]
    runtime_prefix = "game/.godot/mono/publish/arm64/"
    seen_destinations = set()
    for index, item in enumerate(runtime_manifest.get("files", [])):
        if not isinstance(item, dict):
            fail(f".NET runtime manifest file {index} is malformed")
        destination = item.get("destination")
        logical = PurePosixPath(destination) if isinstance(destination, str) else None
        if (logical is None or logical.is_absolute() or ".." in logical.parts or
                logical.as_posix() != destination or destination in seen_destinations):
            fail(f".NET runtime destination {index} is unsafe or duplicated")
        kind = item.get("kind")
        if kind not in ("managed", "native-linux"):
            fail(f".NET runtime member has unsupported kind: {destination}")
        if item.get("mode") != "0644":
            fail(f".NET runtime member has an unsupported mode: {destination}")
        digest = item.get("sha256")
        if not isinstance(digest, str) or not HEX64.fullmatch(digest):
            fail(f".NET runtime member lacks a valid SHA-256: {destination}")
        seen_destinations.add(destination)
        values.append((
            "private-library" if kind == "native-linux" else "runtime-data",
            runtime_prefix + destination, "0644", None, digest,
        ))
    values.extend([
        ("runtime-data", runtime_prefix + "Tearscape.deps.json", "0644",
         PORT / "runtime/Tearscape.deps.json", None),
        ("runtime-hook", "adapter-env.sh", "0644", PORT / "adapter-env.sh", None),
        ("nxextract-recipe", "extractor.json", "0644", PORT / "extractor.json", None),
        ("nxextract-helper", "nxextract/patch-camera-touch-thresholds.py", "0644",
         PORT / "nxextract/patch-camera-touch-thresholds.py", None),
        ("nxextract-engine", "nxextract/nxextract.py", "0644", nxextract / "nxextract.py", None),
        ("nxextract-runner", "nxextract/run-extractor.sh", "0644", nxextract / "run-extractor.sh", None),
        ("nxextract-runtime-env", "nxextract/nxextract-runtime-env.sh", "0644", nxextract / "nxextract-runtime-env.sh", None),
        ("nxextract-ui", "nxextract/nxextract-ui", "0755", release_artifact(nxextract, "ui/release/manifest-v1.json"), None),
        ("nxsplash", "nxsplash-nextos", "0755", release_artifact(framework / "framework/nxsplash", "release/manifest-v1.json"), None),
    ])
    role_order = {
        role: index for index, role in enumerate((
            "executable", "private-library", "runtime-data", "runtime-hook",
            "nxextract-recipe", "nxextract-engine", "nxextract-runner",
            "nxextract-runtime-env", "nxextract-ui", "nxextract-helper",
            "nxextract-spec", "nxsplash",
        ))
    }
    records = []
    for role, path, mode, source, pinned_digest in values:
        digest = pinned_digest or sha256_file(regular(source, path))
        records.append({
            "role": role, "path": path, "mode": mode, "sha256": digest,
        })
    return sorted(records, key=lambda record: (role_order[record["role"]], record["path"]))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--framework-root", required=True, type=Path)
    parser.add_argument("--engine-dir", required=True, type=Path)
    parser.add_argument("--nupkg", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if NXINPUT_0_9_PIN_PENDING:
        fail(
            "nxinput 0.9.0 awaits immutable FRAMEWORK_COMMIT and "
            "NXINPUT_OWNER_COMMIT pins"
        )
    framework = args.framework_root.resolve()
    engine_dir = args.engine_dir.resolve()
    nupkg = regular(args.nupkg.resolve(), ".NET runtime nupkg")
    pin = framework_pin(framework)
    verify_nxinput_vendor(framework)
    verify_nxgl_vendor(framework)
    verify_nxcompat_vendor(framework)
    controller_evidence_path, _controller_evidence = \
        verify_controller_profile_source(framework)
    if sha256_file(nupkg) != NUPKG_SHA256:
        fail(".NET runtime nupkg differs from the pinned official package")
    build_receipt, build_receipt_sha256 = verify_engine_build_receipt(engine_dir)

    outputs = {}
    for name, path in (
        (ENGINE, engine_dir / ENGINE),
        ("libEGL.so", engine_dir / "shimlib/libEGL.so"),
        ("libGLESv2.so", engine_dir / "shimlib/libGLESv2.so"),
    ):
        path = regular(path, name)
        outputs[name] = {
            "max_glibc": max_glibc(path), "sha256": sha256_file(path),
            "size": path.stat().st_size,
        }
    if any(tuple(map(int, value["max_glibc"][6:].split("."))) > (2, 30) for value in outputs.values()):
        fail("a project output exceeds GLIBC 2.30")

    source_count, source_digest = tree_identity(PORT / "src")
    helper_names = (
        "audit_low_glibc.sh", "compiler_wrapper.sh", "gcc10_buster_compat.h",
        "low_glibc_container.sh", "make_input_proof.py",
        "materialize_dotnet_runtime.py",
        "test_runtime_contract.py", "vendor_sdl_3_2_30.sh",
    )
    helpers = [
        {"path": f"recipes/{name}", "sha256": sha256_file(PORT / "recipes" / name)}
        for name in helper_names
    ]
    runtime_manifest = json.loads((PORT / "runtime/dotnet-runtime-manifest.json").read_text())
    if runtime_manifest.get("nupkg_sha256") != NUPKG_SHA256 or len(runtime_manifest.get("files", [])) != 184:
        fail(".NET runtime manifest differs from the closed 184-file execution contract")

    provenance = {
        "schema": "org.nextos.tearscape.build-provenance",
        "schema_version": 1,
        "status": "final",
        "source": {
            "godot": {"version": "4.6.1-stable", "commit": GODOT_COMMIT,
                      "archive_sha256": "b29af30d344afa50e44773e5347bdd3ae3151527d871f7dcc373ba8f3b52f893"},
            "sdl": {"version": "3.2.30", "commit": SDL_COMMIT,
                    "archive_sha256": "7f80bce51b7179bab472e75b90ce8ac06b162b3da8e19feadecee5acea07dbfb"},
            "nxinput": {
                "version": NXINPUT_COMPONENT_VERSION,
                "owner_commit": NXINPUT_OWNER_COMMIT,
                "tag": NXINPUT_FRAMEWORK_TAG,
                "tag_object": NXINPUT_FRAMEWORK_TAG_OBJECT,
                "tag_object_sha256": NXINPUT_FRAMEWORK_TAG_OBJECT_SHA256,
            },
            "nxgl": {"version": "0.3.5", "owner_commit": NXGL_OWNER_COMMIT},
            "controller_profiles": {
                "bundle": "controllers.nxb",
                "sha256": sha256_file(PORT / "controllers.nxb"),
                "source": "physical PortMaster gamecontrollerdb on AISLPC R36T/K36S",
                "source_file_sha256": CONTROLLER_SOURCE_FILE_SHA256,
                "source_line_number": 1110,
                "source_line_sha256": CONTROLLER_SOURCE_LINE_SHA256,
                "evidence_path": "evidence/controller-profile-source.json",
                "evidence_sha256": sha256_file(controller_evidence_path),
                "coverage": "one physically measured GO-Super source in the invariant positional base (schema 4); the live CFW database governs every other pad; exact candidate admission required; no universal fallback claim",
            },
            "port_source_tree": {"files": source_count, "sha256": source_digest},
            "build_recipe": {"path": "build_low_glibc.sh", "sha256": sha256_file(PORT / "build_low_glibc.sh")},
            "package_builder": {
                "path": "recipes/build_public_byo.py",
                "sha256": sha256_file(PORT / "recipes/build_public_byo.py"),
                "nxrelease_version": NXRELEASE_COMPONENT_VERSION,
                "release_framework_commit": RELEASE_FRAMEWORK_COMMIT,
            },
            "helpers": helpers,
        },
        "build": {
            "architecture": "aarch64",
            "workers": build_receipt["build"]["workers"],
            "priority": "nice-10",
            "network": "none", "clean_build_count": 1,
            "independent_fresh_input_tree_count": 1,
            "reproducibility": "single frozen-input deterministic build",
            "compiler": "Arm GNU Toolchain 10.3-2021.07 aarch64-none-linux-gnu",
            "toolchain_archive_sha256": "1e33d53dea59c8de823bbdfe0798280bdcd138636c7060da9d77a97ded095a84",
            "sysroot": "Debian Buster AArch64, glibc 2.28",
            "sysroot_archive_sha256": "484c85086c5e03067e633d98e1617cd5b6b21a60a596a7cdcf6e42524d363e04",
            "scons_wheel_sha256": "a4c3b434330e2d7d975002fd6783284ba348bf394db94c8f83fdc5bf69cdb8d7",
            "builder_image": "python@sha256:57cd7c3a7a273101a6485ba99423ee568157882804b1124b4dd04266317710de",
            "causal_receipt": {
                "port_head": build_receipt["port_head"],
                "prepared_source_tree": build_receipt["prepared_source_tree"],
                "sha256": build_receipt_sha256,
            },
            "source_date_epoch": SOURCE_DATE_EPOCH,
        },
        "outputs": outputs,
        "dotnet_runtime": {
            "package": "Microsoft.NETCore.App.Runtime.linux-arm64",
            "version": "10.0.3", "nupkg_sha256": NUPKG_SHA256,
            "manifest_sha256": sha256_file(PORT / "runtime/dotnet-runtime-manifest.json"),
            "files": 184, "bytes": sum(item["size"] for item in runtime_manifest["files"]),
            "native_elf_count": sum(item["kind"] == "native-linux" for item in runtime_manifest["files"]),
            "excluded_optional_assets": [{
                "name": "libmscordbi.so",
                "reason": "debugger-only library embeds RUNPATH=$ORIGIN and is outside the game execution closure",
                "sha256": "872c6e58b3dbc4e8e862f131b61475fd25db5b9d85e26b984886422f297e4fed",
            }, {
                "name": "libcoreclrtraceptprovider.so",
                "reason": "optional LTTng tracing provider depends on non-universal liblttng-ust.so.0",
                "sha256": "48b2611d91ee9b9c97dd8794b210f435ee8bb37527d0126de2781e95de72606a",
            }],
            "public_glibc_ceiling": "2.30",
        },
    }
    publish_json(PORT / "BUILD-PROVENANCE.json", provenance, args.check)
    project_pin = {
        "schema": "org.nextos.tearscape.project-build-pin",
        "schema_version": 1, "status": "final", "port_id": PORT_ID,
        "architecture": "aarch64", "outputs": outputs,
        "build_provenance_sha256": sha256_file(PORT / "BUILD-PROVENANCE.json"),
        "framework_commit": FRAMEWORK_COMMIT,
        "framework_pin_sha256": sha256_file(PORT / "FRAMEWORK-PIN.json"),
        "dotnet_runtime_manifest_sha256": sha256_file(PORT / "runtime/dotnet-runtime-manifest.json"),
        "dotnet_runtime_nupkg_sha256": NUPKG_SHA256,
        "engine_build_receipt_sha256": build_receipt_sha256,
    }
    publish_json(PORT / "PROJECT-BUILD-PIN.json", project_pin, args.check)

    payload_names = [
        "BUILD-PROVENANCE.json", "FRAMEWORK-PIN.json", "INSTALLATION.md",
        "NOTICE.md", "PROJECT-BUILD-PIN.json", "README.md", "controllers.nxb",
        "version.txt",
    ]
    payload = [
        {"path": name, "mode": "0644", "sha256": sha256_file(PORT / name), "kind": "payload"}
        for name in payload_names
    ]
    payload += [
        {"path": f"licenses/{path.name}", "mode": "0644", "sha256": sha256_file(path), "kind": "license-notice"}
        for path in sorted((PORT / "licenses").iterdir()) if path.is_file() and not path.is_symlink()
    ]
    payload.append({
        "path": "game/override.cfg", "mode": "0644",
        "sha256": sha256_file(PORT / "defaults/override.cfg"),
        "kind": "payload",
    })
    payload.sort(key=lambda item: item["path"])

    actions = [
        ("menu.accept", "button", "engine.ui_accept"),
        ("menu.back", "button", "engine.ui_cancel"),
        ("menu.navigate", "vector", "engine.ui_direction"),
        ("player.attack", "button", "engine.input.attack"),
        ("player.heal", "button", "engine.input.heal"),
        ("player.move", "vector", "engine.input.move"),
        ("player.open_map", "button", "engine.input.open_map"),
        ("player.roll", "button", "engine.input.roll"),
        ("player.select_next", "button", "engine.input.select_next"),
        ("player.select_previous", "button", "engine.input.select_prev"),
        ("player.switch_tool", "button", "engine.input.switch_tool"),
        ("player.use_shield", "button", "engine.input.use_shield"),
        ("player.use_tool", "button", "engine.input.use_tool"),
        ("player.zoom_map", "button", "engine.input.zoom_map"),
        ("system.pause", "button", "engine.input.pause"),
        ("system.quit", "button", "adapter.system.quit"),
    ]
    project = {
        "schema_version": 3, "runtime_root": ".",
        "nxport": {
            "schema_version": 3, "id": PORT_ID, "title": "Tearscape",
            "launcher_name": "Tearscape.sh", "architecture": "aarch64",
            "executable": ENGINE, "argument_mode": "none", "home_mode": "preserve",
            "nxextract": {"mode": "yes", "version": "1.3.0"},
            "required_files": [ENGINE, "adapter-env.sh"],
            "private_library_paths": ["lib", "game/.godot/mono/publish/arm64"],
            "prepare_script": "", "required_capabilities": [
                "host.portmaster", "host.aarch64-libs", "graphics.window",
                "graphics.gles2", "graphics.egl", "graphics.egl-config",
                "graphics.drawable", "audio.output-open",
                "input.controller-mapping", "input.controller-api",
            ],
            "enabled_quirks": [], "runtime_report": "log-and-logo",
            "video_proof": "required",
            # V5 owner runtime (nxbootstrap 0.8.0): sealed adapter-env.sh + the
            # owner's port-env.sh seeded once from defaults/, never healed.
            "owner_runtime": "1",
            "generation_runtime": runtime_records(engine_dir, framework, runtime_manifest),
        },
        "owner_runtime": {"hook_seed": "defaults-src/port-env.sh"},
        # NEXTOS_SETTINGS/2 video: NextOS elects the owner's typed setting and
        # this Godot adapter applies it; crop/integer are not offered because the
        # 640x360 viewport and Camera::InitZoom cannot honour them.
        "video": {
            "authority": "nextos", "aspect": "auto",
            "aspect_policies": ["auto", "engine", "preserve", "stretch"],
            "auto_algorithm": "stretch", "output_size": "display",
            "filter": "engine", "invalid_policy": "package_default",
            # The owner-native Godot file the engine reads (display/window/
            # stretch/aspect); `auto` respects an owner-edited value there.
            "native_config": "game/override.cfg",
        },
        "nxextract_recipe": "extractor.json",
        "adapter": {"skeleton": "contract-only"},
        "portmaster": {"metadata_version": 4, "min_glibc": "2.28", "runtime": []},
        "license": {"spdx_id": "GPL-3.0-only", "source": "LICENSE"},
        "documentation": {"status": "authored", "proven_support": []},
        "package_payload": payload,
        "language_access": {"mode": "native-menu", "supported": ["en"], "fallback": "en", "sinks": ["engine.locale"]},
        "controls": {
            # TEARSCAPE-CONTROLS-LIVE: the editable NEXTOSCONTROLLERS.gptk is
            # loaded by the engine glue (nxinput_gptk_godot.cpp) through the
            # canonical nxinput runtime; nxrelease >= 0.3.10 verifies the
            # marker in the packaged executable.
            "runtime_mapping": "nxinput-gptk",
            "controller_profiles": {
                "bundle": "controllers.nxb",
                "enabled": True,
                "sha256": sha256_file(PORT / "controllers.nxb"),
            },
            "schema": 4,
            # ON_DEVICE_AUTOMATED_INPUT_PROOF (nxgenerator >= 0.3.16): the port
            # supplies only what the framework cannot know -- how to reach each
            # declared context, the menu control that must never select QUIT and
            # the owner remap (A -> null, its action moved to R3, native there).
            # Context lines come from nxinput_gptk_godot.cpp ("context proven=").
            "proof": {
                "clones": 2,
                "navigation": {
                    "menu": [{"wait_log": "nx/gptk: context proven=menu", "timeout_s": 900}, {"sleep_ms": 6000}],
                    # MainMenu focuses LoadGame first: A opens StartGameScreen
                    # (save slots); A there = StartGame(). Scene transitions
                    # briefly unprove the context, so each press waits for the
                    # next screen before the following press.
                    # After StartGame the level opens behind an intro/tutorial
                    # UI screen (tree paused = menu context); further A presses
                    # dismiss it — in gameplay an extra A is only a roll.
                    "gameplay": [
                        {"press": "A", "ms": 150}, {"sleep_ms": 6000},
                        {"press": "A", "ms": 150}, {"sleep_ms": 10000},
                        {"press": "A", "ms": 150}, {"sleep_ms": 5000},
                        {"press": "A", "ms": 150}, {"sleep_ms": 5000},
                        {"press": "A", "ms": 150}, {"sleep_ms": 5000},
                        {"press": "A", "ms": 150}, {"sleep_ms": 5000},
                        {"press": "A", "ms": 150}, {"sleep_ms": 5000},
                        {"press": "A", "ms": 150}, {"sleep_ms": 5000},
                        {"wait_log": "nx/gptk: context proven=gameplay", "timeout_s": 120}, {"sleep_ms": 3000},
                    ],
                },
                # quit_guard = B: on MainMenu the Cancel action has no handler, so
                # the guard proves "back never exits" without leaving the menu.
                # A here would open StartGameScreen and the accept probe (a
                # second A) would start the game: the menu session then ran on
                # IntroScreen/level (proved on device, engine 0216f, 02/09).
                "quit_guard": {"context": "menu", "control": "B"},
                "owner_remap": {"context": "gameplay", "null": "A", "move_to": "R3"},
            },
            "actions": [{"id": action, "kind": kind, "sinks": [sink]} for action, kind, sink in actions],
            "contexts": {
                # The D-pad is DECLARED native in both contexts: schema 3
                # writes an explicit null for every omitted control, and a
                # suppressed D-pad is the field regression (menu navigable
                # only by stick). Native restores the engine's own discrete
                # one-press-one-step handling, exactly the schema-1 behavior.
                "menu": {"LEFT_STICK": "menu.navigate", "A": "menu.accept", "B": "menu.back", "START": "system.pause", "UP": "native", "DOWN": "native", "LEFT": "native", "RIGHT": "native"},
                "gameplay": {"LEFT_STICK": "player.move", "A": "player.roll", "B": "player.switch_tool", "X": "player.attack", "Y": "player.heal", "L1": "player.select_previous", "R1": "player.select_next", "L2": "player.use_shield", "R2": "player.use_tool", "SELECT": "player.open_map", "START": "system.pause", "UP": "native", "DOWN": "native", "LEFT": "native", "RIGHT": "native", "R3": "null"},
            },
        },
        "graphics": {"uses_gl": True, "api": "gles", "profile": "es", "version": "2.0", "version_policy": "minimum", "shader_dialect": "essl100", "drawable_ready_timeout_ms": 15000},
        "promotion": {
            "adapter_contract": "adapter/adapter-contract.json",
            "claims": {
                "adapter_lifecycle_implemented": True,
                "physical_support_proven": False,
                "release_ready": True,
            },
        },
    }
    publish_json(PORT / "nxproject.json", project, args.check)
    print(f"refresh-release-inputs: PASS framework={pin['framework_commit']} engine={outputs[ENGINE]['sha256']} runtime=184")
    return 0


if __name__ == "__main__":
    sys.exit(main())
