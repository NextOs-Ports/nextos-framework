#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Directed gates for public SDL providers and the external candidate lock."""

import hashlib
import importlib.util
import inspect
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "nxrelease.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("nxrelease_provider_lock", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def refuses(module, label, fragment, callback):
    try:
        callback()
    except module.ReleaseError as error:
        require(fragment.casefold() in str(error).casefold(),
                "%s reported %r, expected %r" %
                (label, str(error), fragment))
        return
    raise AssertionError(label + " unexpectedly passed")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def record(path, target, kind="payload", soname=None, mode=0o644,
           architecture=None, provenance=None):
    value = {
        "actual_path": Path(path),
        "kind": kind,
        "mode": mode,
        "sha256": digest(path),
        "soname": soname,
        "target": target,
    }
    if architecture is not None:
        value["architecture"] = architecture
    if provenance is not None:
        value["provenance"] = provenance
    return value


def sdl_config(**overrides):
    value = {
        "adapter_contract": {
            "input_sdl3_portmaster": {
                "enabled": False,
                "private_sdl3_sha256": "",
            },
        },
        "dependencies": [],
        "port_dir": "fixture",
    }
    value.update(overrides)
    return value


def exercise_sdl_policy(module, root):
    plain = root / "plain.bin"
    plain.write_bytes(b"ordinary payload\n")
    module.validate_private_sdl_policy(
        [record(plain, "fixture/plain.bin")], sdl_config())

    # A system SDL2 consumer/provider declaration is the canonical public path.
    system = sdl_config(dependencies=[{
        "architecture": "aarch64", "namespace": "linux",
        "provider": "portmaster", "soname": "libSDL2-2.0.so.0",
    }])
    module.validate_private_sdl_policy(
        [record(plain, "fixture/plain.bin")], system)

    private_sdl2 = root / "libSDL2-2.0.so.0"
    private_sdl2.write_bytes(b"even a non-ELF basename is forbidden\n")
    private_sdl2_record = record(
        private_sdl2, "fixture/libSDL2-2.0.so.0")
    refuses(module, "SDL2 basename", "SDL1/SDL2",
            lambda: module.validate_provider_policy(
                [private_sdl2_record], sdl_config()))
    extensionless_sdl2 = root / "libSDL2"
    extensionless_sdl2.write_bytes(b"extensionless private provider\n")
    refuses(module, "extensionless SDL2 basename", "SDL1/SDL2",
            lambda: module.validate_provider_policy([
                record(extensionless_sdl2, "fixture/libSDL2")
            ], sdl_config()))
    refuses(module, "metadata cannot disable SDL policy", "SDL1/SDL2",
            lambda: module.validate_provider_policy(
                [private_sdl2_record],
                sdl_config(provider_policy_required=False)))

    renamed = root / "renamed.so"
    shutil.copyfile("/bin/true", renamed)
    refuses(module, "renamed SDL2 SONAME", "SDL1/SDL2",
            lambda: module.validate_private_sdl_policy(
                [record(renamed, "fixture/renamed.so", "third-party-linux",
                        "libSDL2-2.0.so.0")], sdl_config()))

    identity = record(renamed, "fixture/provider.so", "third-party-linux")
    module._DYNAMIC_SYMBOLS[identity["target"]] = (
        set(), {"SDL_Init", "SDL_Quit", "SDL_RWFromFile"})
    refuses(module, "renamed SDL2 ELF identity", "SDL1/SDL2",
            lambda: module.validate_private_sdl_policy(
                [identity], sdl_config()))
    module._DYNAMIC_SYMBOLS.clear()

    for label, symbols in (
            ("gfx", {"rotozoomSurface", "pixelColor"}),
            ("gpu", {"GPU_Init", "GPU_Quit"}),
            ("sound", {"Sound_Init", "Sound_Quit"})):
        addon = record(renamed, "fixture/renamed-{}.so".format(label),
                       "third-party-linux")
        module._DYNAMIC_SYMBOLS[addon["target"]] = (set(), symbols)
        refuses(module, "renamed SDL {} add-on".format(label), "SDL1/SDL2",
                lambda item=addon: module.validate_private_sdl_policy(
                    [item], sdl_config()))
        module._DYNAMIC_SYMBOLS.clear()

    static_source = root / "renamed-static.c"
    static_object = root / "renamed-static.o"
    static_archive = root / "renamed-static.a"
    static_source.write_text(
        "__attribute__((visibility(\"hidden\"))) int Mix_OpenAudio(void) { return 0; }\n"
        "__attribute__((visibility(\"hidden\"))) void Mix_Quit(void) {}\n",
        encoding="utf-8")
    subprocess.run([
        "aarch64-linux-gnu-gcc", "-c", "-fvisibility=hidden",
        str(static_source), "-o", str(static_object),
    ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    subprocess.run([
        "aarch64-linux-gnu-ar", "rcs", str(static_archive),
        str(static_object),
    ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    refuses(module, "renamed hidden/static SDL add-on", "SDL1/SDL2",
            lambda: module.validate_private_sdl_policy([
                record(static_archive, "fixture/renamed-static.a")
            ], sdl_config()))

    script = root / "redirect.sh"
    script.write_text(
        "#!/bin/sh\nexport SDL_DYNAMIC_API=$GAMEDIR/lib/libSDL2.so\n",
        encoding="utf-8")
    refuses(module, "SDL_DYNAMIC_API redirect", "SDL_DYNAMIC_API",
            lambda: module.validate_private_sdl_policy(
                [record(script, "fixture/redirect.sh", "script", mode=0o755)],
                sdl_config()))

    # The canonical system-provider launcher restores the already-audited
    # firmware/adapter value through its private boundary variable.  This is
    # propagation, not a redirect to a packaged SDL.
    forwarded = root / "system-forward.sh"
    forwarded.write_text(
        "#!/bin/sh\n"
        "export SDL_DYNAMIC_API=$NXBOOTSTRAP_SYSTEM_SDL_DYNAMIC_API\n",
        encoding="utf-8")
    module.validate_private_sdl_policy(
        [record(forwarded, "fixture/system-forward.sh", "script", mode=0o755)],
        sdl_config(sdl_provider="system"))

    local_dlopen = root / "local-loader.bin"
    local_dlopen.write_bytes(b'dlopen\0./lib/libSDL2.so.0\0')
    refuses(module, "local SDL dlopen", "dlopen",
            lambda: module.validate_private_sdl_policy(
                [record(local_dlopen, "fixture/local-loader.bin")],
                sdl_config()))

    license_file = root / "SDL3-LICENSE.txt"
    license_file.write_text("Zlib license fixture for directed audit.\n",
                            encoding="utf-8")
    source_url = "https://github.com/libsdl-org/SDL/releases/tag/release-3.2.0"
    version = "3.2.0"
    private_sdl3_source = root / "sdl3-provider.c"
    private_sdl3 = root / "libSDL3.so.0"
    private_sdl3_source.write_text(
        "int SDL_Init(unsigned flags) { return (int)flags; }\n"
        "void SDL_Quit(void) {}\n"
        "void *SDL_OpenGamepad(int index) { return (void *)(long)(index + 1); }\n",
        encoding="utf-8")
    subprocess.run([
        "aarch64-linux-gnu-gcc", "-shared", "-fPIC", "-nostdlib",
        "-Wl,--build-id", "-Wl,-soname,libSDL3.so.0",
        str(private_sdl3_source), "-o", str(private_sdl3),
    ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    sdl3_record = record(
        private_sdl3, "fixture/libSDL3.so.0", "third-party-linux",
        "libSDL3.so.0", architecture="aarch64",
        provenance="SDL {} from {}".format(version, source_url))
    sdl3_sha = digest(private_sdl3)
    exception = {
        "architecture": "aarch64",
        "license_file": "fixture/SDL3-LICENSE.txt",
        "license_spdx": "Zlib",
        "mode": "0644",
        "path": "fixture/libSDL3.so.0",
        "reason": ("Native SDL3 game requires APIs absent from every declared "
                   "system SDL provider."),
        "sha256": sdl3_sha,
        "soname": "libSDL3.so.0",
        "source_url": source_url,
        "version": version,
    }
    dependency = {
        "architecture": "aarch64", "namespace": "linux",
        "provider": "package", "soname": "libSDL3.so.0",
        "path": "fixture/libSDL3.so.0",
    }
    allowed = sdl_config(
        adapter_contract={"input_sdl3_portmaster": {
            "enabled": True, "private_sdl3_sha256": sdl3_sha,
        }}, dependencies=[dependency], sdl3_exception=exception)
    license_record = record(
        license_file, "fixture/SDL3-LICENSE.txt", "license-notice")
    module.validate_private_sdl_policy(
        [sdl3_record, license_record], allowed)

    fake = root / "libSDL3-fake.so.0"
    fake.write_bytes(b"SDL_Init SDL_Quit SDL_OpenGamepad text only\n")
    fake_sha = digest(fake)
    fake_exception = dict(
        exception, path="fixture/libSDL3-fake.so.0", sha256=fake_sha)
    fake_dependency = dict(dependency, path="fixture/libSDL3-fake.so.0")
    fake_record = record(
        fake, "fixture/libSDL3-fake.so.0", "third-party-linux",
        "libSDL3.so.0", architecture="aarch64",
        provenance="SDL {} from {}".format(version, source_url))
    fake_config = sdl_config(
        adapter_contract={"input_sdl3_portmaster": {
            "enabled": True, "private_sdl3_sha256": fake_sha,
        }}, dependencies=[fake_dependency], sdl3_exception=fake_exception)
    refuses(module, "text fake SDL3 exception", "real", lambda:
            module.validate_private_sdl_policy(
                [fake_record, license_record], fake_config))

    bad_pin = dict(allowed, adapter_contract={
        "input_sdl3_portmaster": {
            "enabled": True, "private_sdl3_sha256": "a" * 64,
        }})
    refuses(module, "SDL3 wrong pin", "differs",
            lambda: module.validate_private_sdl_policy(
                [sdl3_record, license_record], bad_pin))
    refuses(module, "SDL3 without opt-in", "requires",
            lambda: module.validate_private_sdl_policy(
                [sdl3_record, license_record],
                sdl_config(dependencies=[dependency],
                           sdl3_exception=exception)))
    system_sdl3 = dict(allowed, sdl_provider="system")
    refuses(module, "system/private SDL3 contradiction", "contradicts",
            lambda: module.validate_private_sdl_policy(
                [sdl3_record, license_record], system_sdl3))
    missing_license = dict(allowed)
    refuses(module, "SDL3 missing license", "license_file",
            lambda: module.validate_private_sdl_policy(
                [sdl3_record], missing_license))
    bad_abi = dict(allowed, sdl3_exception=dict(exception,
                                                architecture="armv7"),
                   dependencies=[dict(dependency, architecture="armv7")])
    refuses(module, "SDL3 wrong declared ABI", "ABI/SONAME",
            lambda: module.validate_private_sdl_policy(
                [sdl3_record, license_record], bad_abi))
    refuses(module, "empty enabled SDL3 opt-in", "no private SDL3",
            lambda: module.validate_private_sdl_policy(
                [record(plain, "fixture/plain.bin")], allowed))


def lock_document(executable, sha256):
    return {
        "schema": "nxrelease-candidate-lock-v1",
        "schema_version": 1,
        "executable": executable,
        "sha256": sha256,
    }


def exercise_external_lock(module, root):
    source = root / "source"
    source.mkdir()
    executable = source / "fixture-nextos"
    shutil.copyfile("/bin/true", executable)
    os.chmod(executable, 0o755)
    executable_sha = digest(executable)

    lock_path = root / "candidate-lock.json"
    lock_path.write_text(json.dumps(
        lock_document("fixture/fixture-nextos", executable_sha),
        sort_keys=True, indent=2) + "\n", encoding="utf-8")
    os.chmod(lock_path, 0o444)
    lock = module.load_candidate_lock(lock_path, source.resolve())
    require(lock["sha256"] == executable_sha and
            lock["document_sha256"] == digest(lock_path),
            "external lock did not retain executable/document identities")

    hardlink = root / "candidate-lock-hardlink.json"
    os.link(lock_path, hardlink)
    refuses(module, "hardlinked authority", "exactly one link",
            lambda: module.load_candidate_lock(hardlink, source.resolve()))
    hardlink.unlink()

    symlink = root / "candidate-lock-symlink.json"
    symlink.symlink_to(lock_path.name)
    refuses(module, "symlinked authority", "securely read",
            lambda: module.load_candidate_lock(symlink, source.resolve()))

    unsafe_parent = root / "unsafe-authority"
    unsafe_parent.mkdir(mode=0o777)
    os.chmod(unsafe_parent, 0o777)
    unsafe_lock = unsafe_parent / "candidate-lock.json"
    unsafe_lock.write_text(json.dumps(
        lock_document("fixture/fixture-nextos", executable_sha)),
        encoding="utf-8")
    os.chmod(unsafe_lock, 0o444)
    refuses(module, "unsafe authority parent", "group/other writable",
            lambda: module.load_candidate_lock(
                unsafe_lock, source.resolve()))

    race_lock = root / "candidate-lock-race.json"
    race_replacement = root / "candidate-lock-race-replacement.json"
    shutil.copyfile(lock_path, race_lock)
    shutil.copyfile(lock_path, race_replacement)
    os.chmod(race_lock, 0o444)
    os.chmod(race_replacement, 0o444)
    original_read = module.os.read
    race_triggered = [False]

    def racing_read(descriptor, count):
        payload = original_read(descriptor, count)
        if not race_triggered[0]:
            race_triggered[0] = True
            os.replace(race_replacement, race_lock)
        return payload

    try:
        module.os.read = racing_read
        refuses(module, "authority TOCTOU replacement", "replaced",
                lambda: module.load_candidate_lock(
                    race_lock, source.resolve()))
    finally:
        module.os.read = original_read

    inside = source / "self-attested-lock.json"
    inside.write_text(json.dumps(
        lock_document("fixture/fixture-nextos", executable_sha)),
        encoding="utf-8")
    os.chmod(inside, 0o444)
    refuses(module, "self-attested lock", "self-attestation",
            lambda: module.load_candidate_lock(inside, source.resolve()))

    writable = root / "writable-lock.json"
    writable.write_text(json.dumps(
        lock_document("fixture/fixture-nextos", executable_sha)),
        encoding="utf-8")
    refuses(module, "writable lock", "read-only",
            lambda: module.load_candidate_lock(writable, source.resolve()))
    refuses(module, "boolean candidate schema version", "schema",
            lambda: module.normalize_candidate_lock(dict(
                lock_document("fixture/fixture-nextos", executable_sha),
                schema_version=True,
                document_sha256="f" * 64), "boolean lock"))
    refuses(module, "nonportable candidate executable", "portable",
            lambda: module.normalize_candidate_lock(dict(
                lock_document("fixture/jogó-nextos", executable_sha),
                document_sha256="f" * 64), "nonportable lock"))

    source_record = record(
        executable, "fixture/fixture-nextos", "project-linux", mode=0o755)
    config = {
        "candidate_lock": lock,
        "candidate_lock_required": True,
        "nxport_manifest": {"executable": "fixture-nextos"},
        "port_dir": "fixture",
    }
    module.validate_candidate_lock([source_record], config)

    # Repeat against independent staged bytes: this is the same call wired into
    # verify_stage's audit_record_set, not trust in source record metadata.
    stage = root / "stage"
    stage.mkdir()
    staged_executable = stage / "fixture-nextos"
    shutil.copyfile(executable, staged_executable)
    os.chmod(staged_executable, 0o755)
    module.validate_candidate_lock([
        record(staged_executable, "fixture/fixture-nextos", "project-linux",
               mode=0o755)
    ], dict(config))

    rebuilt = root / "rebuilt-nextos"
    rebuilt.write_bytes(executable.read_bytes() + b"post-proof rebuild\n")
    os.chmod(rebuilt, 0o755)
    refuses(module, "post-proof executable replacement", "differs",
            lambda: module.validate_candidate_lock([
                record(rebuilt, "fixture/fixture-nextos", "project-linux",
                       mode=0o755)
            ], dict(config)))
    refuses(module, "missing mandatory lock", "required",
            lambda: module.validate_candidate_lock(
                [source_record], {
                    "candidate_lock": None,
                    "candidate_lock_required": True,
                }))

    metadata_config = {
        "candidate_lock": lock,
        "candidate_lock_verified": True,
        "ceiling": "2.30",
        "compression": "deflated",
        "dependencies": [],
        "epoch": 1786492800,
        "exception_map": {},
        "launcher": "Fixture.sh",
        "launcher_chain": ["Fixture.sh"],
        "launcher_contract": {},
        "license": None,
        "manifest_sha256": "b" * 64,
        "nxextract": {},
        "nxsplash": None,
        "package_id": "fixture",
        "package_version": "1.0.0",
        "port_dir": "fixture",
        "portmaster_metadata": {},
        "sdl3_exception": None,
    }
    current = module.create_metadata(
        metadata_config, [source_record], [], "none")
    require(current["candidate_lock"] == lock,
            "0.3.8 metadata did not carry the authenticated lock")
    refuses(module, "new build cannot self-select legacy metadata", "must use",
            lambda: module.create_metadata(
                metadata_config, [source_record], [], "none", "0.3.7"))
    require(module.candidate_lock_from_metadata(current) == lock,
            "verify-stage metadata boundary did not recover the lock")
    refuses(module, "0.3.8 ZIP metadata without authority", "release_authority",
            lambda: module.candidate_lock_from_metadata({
                "tool": {"name": "nxrelease", "version": "0.3.8"}}))
    refuses(module, "legacy metadata cannot omit current authority", "release_authority",
            lambda: module.candidate_lock_from_metadata({
                "tool": {"name": "nxrelease", "version": "0.2.40"}}))

    quarantine = root / "historical-quarantine"
    quarantine.mkdir()
    archive = quarantine / "historical.zip"
    archive.write_bytes(b"authenticated historical archive fixture\n")
    for index, (tool_version, bootstrap_version) in enumerate((
            ("0.2.40", "0.6.37"), ("0.3.7", "0.7.3"),
            ("0.3.13", "0.7.6"), ("0.3.14", "0.7.7"),
            ("0.3.15", "0.7.7"), ("0.3.16", "0.7.8"), ("0.3.17", "0.7.8"),
            ("0.3.17", "0.7.8"), ("0.3.18", "0.7.8"),
            ("0.3.19", "0.7.8"), ("0.3.20", "0.7.8"))):
        authority_path = root / "historical-authority-{}.json".format(index)
        authority_path.write_text(json.dumps({
            "schema": "nxrelease-historical-authority-v1",
            "schema_version": 1,
            "mode": "historical-read-only",
            "quarantine": True,
            "archive_sha256": digest(archive),
            "metadata_tool_version": tool_version,
            "nxbootstrap_version": bootstrap_version,
        }, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        os.chmod(authority_path, 0o444)
        authority = module.load_historical_authority(
            authority_path, archive)
        require(authority["metadata_tool_version"] == tool_version and
                authority["nxbootstrap_version"] == bootstrap_version and
                authority["document_sha256"] == digest(authority_path),
                "historical external authority lost an exact identity")
        require(module.candidate_lock_from_metadata({}, authority) is None,
                "authenticated historical read-only mode required a new lock")
        refuses(module, "historical metadata cannot self-claim current lock",
                "must not claim", lambda value=authority:
                module.candidate_lock_from_metadata(
                    {"candidate_lock": lock}, value))

    public_named_copy = root / "historical.zip"
    shutil.copyfile(archive, public_named_copy)
    refuses(module, "historical archive outside quarantine", "quarantine path",
            lambda: module.load_historical_authority(
                authority_path, public_named_copy))

    misleading = root / "notquarantine"
    misleading.mkdir()
    misleading_copy = misleading / "historical.zip"
    shutil.copyfile(archive, misleading_copy)
    refuses(module, "misleading historical archive path", "quarantine path",
            lambda: module.load_historical_authority(
                authority_path, misleading_copy))

    wrong_authority = root / "wrong-historical-authority.json"
    wrong_authority.write_text(json.dumps({
        "schema": "nxrelease-historical-authority-v1",
        "schema_version": 1,
        "mode": "historical-read-only",
        "quarantine": True,
        "archive_sha256": "0" * 64,
        "metadata_tool_version": "0.3.7",
        "nxbootstrap_version": "0.7.3",
    }) + "\n", encoding="utf-8")
    os.chmod(wrong_authority, 0o444)
    refuses(module, "historical authority wrong archive", "different archive",
            lambda: module.load_historical_authority(
                wrong_authority, archive))

    # Lock the actual call chain: source/stage/ZIP all converge on the same
    # gate.  A future refactor that leaves the test helper green but removes a
    # release boundary must fail this directed regression.
    audit_source = inspect.getsource(module.audit_record_set)
    require("validate_provider_policy(records, config)" in audit_source,
            "audit_record_set no longer enforces the SDL provider policy")
    require("validate_video_proof_contract(records, config)" in audit_source,
            "audit_record_set no longer enforces video-proof wiring")
    require("validate_candidate_lock(records, config)" in audit_source,
            "audit_record_set no longer enforces the candidate lock")
    require("audit_record_set(records, audit_config)" in
            inspect.getsource(module.verify_stage),
            "verify_stage no longer re-audits the candidate lock")
    require("verify_stage(stage)" in inspect.getsource(module.create_archive),
            "create_archive no longer verifies its locked stage")
    require("verify_stage(" in inspect.getsource(module.verify_archive),
            "verify_archive no longer reopens through verify_stage")

    os.chmod(lock_path, 0o644)
    os.chmod(inside, 0o644)


def exercise_video_proof(module, root):
    executable = root / "proof-nextos"
    executable.write_bytes(
        b"\x7fELF\0org.nextos.nxruntime.video-proof\0VIDEO-PROOF:\0"
        b"NXBOOTSTRAP_VIDEO_FILE\0")
    os.chmod(executable, 0o755)
    proof_record = record(
        executable, "fixture/proof-nextos", "project-linux", mode=0o755)
    config = {
        "nxport_manifest": {
            "executable": "proof-nextos", "video_proof": "required",
        },
        "port_dir": "fixture",
    }
    old_is_elf = module.is_elf
    old_readelf = module.run_readelf
    symbol_table = (
        "  1: 00000000 0 FUNC GLOBAL DEFAULT 1 "
        "nxgl_frame_proof_before_present\n"
        "  2: 00000000 0 FUNC GLOBAL DEFAULT 1 "
        "nxgl_frame_proof_publish\n"
    )
    try:
        module.is_elf = lambda _path: True
        module.run_readelf = lambda _path, _arguments: symbol_table
        module.validate_video_proof_contract([proof_record], config)

        generation = "c" * 64
        receipt = {
            "schema": "org.nextos.nxruntime.video-proof",
            "schema_version": 1,
            "run_id": "physical-run-01",
            "generation": generation,
            "port_id": "fixture",
            "verdict": "OK",
            "reason": "non-black",
        }
        receipt_bytes = module.video_proof_receipt_bytes(receipt)
        require(receipt_bytes == (
            b'{"schema":"org.nextos.nxruntime.video-proof",'
            b'"schema_version":1,"run_id":"physical-run-01",'
            b'"generation":"' + generation.encode("ascii") +
            b'","port_id":"fixture","verdict":"OK",'
            b'"reason":"non-black"}\n'),
            "video receipt bytes differ from the fixed C producer order")
        raw_lock = lock_document("fixture/proof-nextos", digest(executable))
        raw_lock.update({
            "document_sha256": "d" * 64,
            "video_proof": receipt,
            "video_proof_receipt_sha256": hashlib.sha256(
                receipt_bytes).hexdigest(),
        })
        proof_lock = module.normalize_candidate_lock(
            raw_lock, "video candidate lock")
        proof_config = dict(config,
                            candidate_lock=proof_lock,
                            candidate_lock_required=True,
                            generation_id=generation,
                            package_id="fixture")
        module.validate_candidate_lock([proof_record], proof_config)

        no_receipt = module.normalize_candidate_lock(dict(
            lock_document("fixture/proof-nextos", digest(executable)),
            document_sha256="e" * 64), "no-receipt lock")
        refuses(module, "required proof missing from candidate lock", "receipt",
                lambda: module.validate_candidate_lock(
                    [proof_record], dict(proof_config,
                                         candidate_lock=no_receipt)))
        stale = dict(proof_lock)
        stale["video_proof"] = dict(receipt, generation="f" * 64)
        stale_line = module.video_proof_receipt_bytes(stale["video_proof"])
        stale["video_proof_receipt_sha256"] = hashlib.sha256(
            stale_line).hexdigest()
        refuses(module, "stale proof generation", "stale",
                lambda: module.validate_candidate_lock(
                    [proof_record], dict(proof_config,
                                         candidate_lock=stale)))
        black = dict(raw_lock)
        black["video_proof"] = dict(receipt, verdict="BLACK",
                                    reason="all-black")
        black_line = module.video_proof_receipt_bytes(black["video_proof"])
        black["video_proof_receipt_sha256"] = hashlib.sha256(
            black_line).hexdigest()
        refuses(module, "BLACK candidate receipt", "verdict must be OK",
                lambda: module.normalize_candidate_lock(
                    black, "black candidate lock"))
        bad_receipt_hash = dict(raw_lock,
                                video_proof_receipt_sha256="0" * 64)
        refuses(module, "invented receipt hash", "exact producer bytes",
                lambda: module.normalize_candidate_lock(
                    bad_receipt_hash, "invented candidate lock"))
        bad_run_id = dict(raw_lock)
        bad_run_id["video_proof"] = dict(receipt, run_id="device:run")
        bad_run_id["video_proof_receipt_sha256"] = hashlib.sha256(
            module.video_proof_receipt_bytes(
                bad_run_id["video_proof"])).hexdigest()
        refuses(module, "producer-incompatible receipt run_id", "run_id",
                lambda: module.normalize_candidate_lock(
                    bad_run_id, "bad run-id candidate lock"))
        # The absent opt-in is the exact legacy behavior.
        module.validate_video_proof_contract(
            [proof_record], {"nxport_manifest": {
                "executable": "proof-nextos"}, "port_dir": "fixture"})

        module.run_readelf = lambda _path, _arguments: symbol_table.splitlines()[0]
        refuses(module, "video proof without publish wiring", "symbol",
                lambda: module.validate_video_proof_contract(
                    [proof_record], config))
        module.run_readelf = lambda _path, _arguments: symbol_table
        executable.write_bytes(
            b"\x7fELF\0org.nextos.nxruntime.video-proof\0VIDEO-PROOF:\0")
        proof_record["sha256"] = digest(executable)
        refuses(module, "video proof without output-file wiring", "literal",
                lambda: module.validate_video_proof_contract(
                    [proof_record], config))
        refuses(module, "invalid video proof mode", "must be required",
                lambda: module.validate_video_proof_contract(
                    [proof_record], {
                        "nxport_manifest": {
                            "executable": "proof-nextos",
                            "video_proof": "claimed",
                        },
                        "port_dir": "fixture",
                    }))
    finally:
        module.is_elf = old_is_elf
        module.run_readelf = old_readelf


def main():
    module = load_tool()
    with tempfile.TemporaryDirectory(prefix="nxrelease-provider-lock-") as raw:
        root = Path(raw)
        exercise_sdl_policy(module, root)
        exercise_external_lock(module, root)
        exercise_video_proof(module, root)
    print("nxrelease provider/lock regression passed: "
          "system-sdl=2 private-sdl-negatives=17 real-sdl3=1 "
          "candidate-boundaries=23 historical-authority=10 video-proof=10")


if __name__ == "__main__":
    main()
