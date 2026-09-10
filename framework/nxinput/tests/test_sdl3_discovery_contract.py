#!/usr/bin/env python3
"""Static/receipt gate for the SDL3 discovery prerequisite used by V4."""

import argparse
import json
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "references" / "v4-sdl3-discovery-bb1-v1.json"


def validate_runtime(runtime: dict) -> None:
    """Require Linux evdev with runtime-loaded udev, never a hard dependency."""
    assert runtime["linux_evdev"] is True
    assert runtime["libudev"] == "dynamic"
    assert runtime["libudev_dt_needed"] is False


def validate_adopter_receipt(path: pathlib.Path) -> None:
    receipt = json.loads(path.read_text(encoding="utf-8"))
    assert receipt["schema"] == "nxinput-v4-sdl3-adoption-receipt/1"
    validate_runtime(receipt["private_sdl3"])
    assert receipt["discovery_before_classification"] is True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--adopter-receipt",
        type=pathlib.Path,
        help="also validate one port's machine-readable SDL3 discovery receipt",
    )
    arguments = parser.parse_args()

    # The component's public compile-time identity must move with VERSION and
    # CMake. A V4 adopter must never report itself as the V3 0.5.1 core.
    # V5 (0.11.0): the three identities must agree with each other, whatever
    # the current number is (a literal here rotted at 0.10.0 while VERSION
    # had moved to 0.10.2 in the V4 tag).
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    public_header = (ROOT / "include" / "nxinput.h").read_text(encoding="utf-8")
    assert '#define NXINPUT_VERSION "%s"' % version in public_header, version
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "project(nxinput VERSION %s LANGUAGES C)" % version in cmake, version

    document = json.loads(REFERENCE.read_text(encoding="utf-8"))
    assert document["schema"] == "nxinput-v4-sdl3-discovery-reference/1"
    assert document["status"] == "positive-physical-reference"

    source = document["source"]
    assert source["port_id"] == "beachbuggy"
    assert source["port_version"] == "1.0.6"
    assert source["commit"] == "5d965512550c29c2c47a7fb69341fe75349e08fb"
    assert source["artifact_sha256"] == (
        "e8368c6bca921032b7476cbd247d42f2fdd2ff8b4bfd0213a12d6a97b71e86b3"
    )

    runtime = document["private_sdl3"]
    assert runtime["sha256"] == (
        "82445879f4e44084ffcf81266eacec7b02f82b7e29376b7ed8a559bf2aa9c173"
    )
    assert runtime["build_id"] == "fe21c660206f0866956a3277f3362195e2321973"
    assert runtime["source_commit"] == (
        "4751e3794d28fe26b6ce9c2fa6b45a7ce6ec50fd"
    )
    assert runtime["max_glibc"] == "2.27"
    validate_runtime(runtime)

    # Keep the prerequisite executable, not documentary: each way an adopter
    # could lose dynamic discovery must be rejected by the same validator used
    # for a port receipt.
    for key, invalid in (
        ("linux_evdev", False),
        ("libudev", "linked"),
        ("libudev_dt_needed", True),
    ):
        negative = dict(runtime)
        negative[key] = invalid
        try:
            validate_runtime(negative)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"invalid SDL3 discovery runtime accepted: {key}")

    contract = document["adoption_contract"]
    assert contract == {
        "opt_in": True,
        "port_pins_own_sdl3": True,
        "discovery_receipt_required": True,
        "linux_evdev_required": True,
        "dynamic_udev_required": True,
        "libudev_dt_needed_forbidden": True,
        "discovery_before_classification_required": True,
        "preinit_environment_staging_required": True,
        "effective_mapping_readback_required": True,
        "guest_native_flow_preserved": True,
        "physical_acceptance_not_inherited": True,
    }
    scope = document["framework_scope"]
    assert scope == {
        "state": "host-candidate-partial",
        "bundles_sdl3": False,
        "heterogeneous_mapping_selection": False,
    }

    public_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (REFERENCE, ROOT / "README.md", ROOT / "CHANGELOG.md")
    ).lower()
    assert "bbracing2" not in public_text
    assert "bb2" not in public_text
    if arguments.adopter_receipt is not None:
        validate_adopter_receipt(arguments.adopter_receipt)
    print("nxinput SDL3 discovery contract: PASS (BB1 positive source only)")


if __name__ == "__main__":
    main()
