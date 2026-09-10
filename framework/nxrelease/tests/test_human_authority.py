#!/usr/bin/env python3
"""Directed contract for the V5 human-authority packaging path."""

import hashlib
import importlib.util
import inspect
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("nxrelease_human", ROOT / "nxrelease.py")
NX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(NX)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def expect_failure(needle, callback):
    try:
        callback()
    except NX.ReleaseError as error:
        require(needle in str(error), "unexpected failure: %s" % error)
    else:
        raise AssertionError("expected failure containing %r" % needle)


def main():
    human = NX.resolve_release_authority(None, None)
    require(human == {
        "mode": "human",
        "machine_receipts_required": False,
    }, "no-lock packaging did not default to human authority")

    lock = {"schema": NX.CANDIDATE_LOCK_SCHEMA}
    legacy = NX.resolve_release_authority(None, lock)
    require(legacy["mode"] == "candidate-lock" and
            legacy["machine_receipts_required"] is True,
            "an explicit legacy lock lost its old authority")
    expect_failure(
        "requires --candidate-lock",
        lambda: NX.resolve_release_authority("candidate-lock", None),
    )
    expect_failure(
        "cannot also consume",
        lambda: NX.resolve_release_authority("human", lock),
    )

    parser = NX.build_parser()
    for command, extra in (
            ("stage", ["--stage", "stage"]),
            ("build", ["--stage", "stage", "--output", "out.zip"]),
            ("bundle", ["--stage", "stage", "--destination", "out",
                        "--archive-name", "game.zip"])):
        parsed = parser.parse_args(
            [command, "--manifest", "release.json", "--authority", "human"] + extra
        )
        require(parsed.candidate_lock is None,
                "%s still made candidate lock syntactically mandatory" % command)

    digest = hashlib.sha256(b"elf").hexdigest()
    binding = NX._candidate_binding_proof({
        "release_authority": human,
    }, {
        "sha256": hashlib.sha256(b"zip").hexdigest(),
    }, {"game/game-nextos": digest})
    require(binding["method"] == "stage-archive-integrity" and
            binding["release_authority"] == "human" and
            "candidate_document_sha256" not in binding,
            "human provenance fabricated or retained a candidate receipt")

    archive_source = inspect.getsource(NX.create_archive)
    bundle_source = inspect.getsource(NX.create_release_bundle)
    require(archive_source.count("verify_archive(") == 1,
            "bare build must reopen the ZIP exactly once")
    require("defer_verification=True" in bundle_source and
            bundle_source.count("verify_archive(") == 1,
            "bundle must defer and reopen the final ZIP exactly once")

    print("nxrelease 0.4.11 human authority: PASS (no receipt, one ZIP reopen)")


if __name__ == "__main__":
    main()
