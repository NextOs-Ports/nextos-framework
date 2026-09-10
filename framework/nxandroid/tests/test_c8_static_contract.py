#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Static ownership and claim boundary for nxandroid 0.5.0 / C8."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def die(message: str) -> None:
    raise SystemExit(f"c8-static: {message}")


def require(text: str, token: str, label: str) -> None:
    if token not in text:
        die(f"{label} missing {token!r}")


def main() -> int:
    header = (ROOT / "include/nxandroid_unity_input.h").read_text()
    public_header = (ROOT / "include/nxandroid.h").read_text()
    source = (ROOT / "src/nxandroid_unity_input.c").read_text()
    cmake = (ROOT / "CMakeLists.txt").read_text()
    readme = (ROOT / "README.md").read_text()
    contract = (ROOT / "CONTRACT.md").read_text()
    matrix = (ROOT / "REGRESSION-MATRIX.md").read_text()
    changelog = (ROOT / "CHANGELOG.md").read_text()

    if (ROOT / "VERSION").read_text().strip() != "0.5.0":
        die("VERSION is not 0.5.0")
    require(cmake, "project(nxandroid VERSION 0.5.0", "CMake")
    require(public_header, '#define NXANDROID_VERSION "0.5.0"',
            "public header")
    require(cmake, "src/nxandroid_unity_input.c", "CMake")
    require(cmake, "tests/test_unity_input.c", "CMake")
    require(cmake, "include/nxandroid_unity_input.h", "CMake install")

    for token in (
        "NXANDROID_UNITY_LEGACY_INPUT",
        "NXANDROID_UNITY_NEW_INPUT_SYSTEM",
        "NXANDROID_UNITY_REWIRED",
        "NXANDROID_UNITY_INCONTROL",
        "NXANDROID_UNITY_RAW_ANDROID",
    ):
        if header.count(token) != 1:
            die(f"profile enum is missing or duplicated: {token}")
    require(source, "NXANDROID_ANDROID_CONTROL_COUNT == 18u", "source")
    require(source, "context->ops.producer", "producer boundary")
    require(source, "returned->api_returned != 1", "consumer return boundary")
    require(source, "pending->low_level_returned", "low-level receipt")
    require(source, "pending->action_returned", "action receipt")

    forbidden_source = (
        "SDL_", "/dev/input", "EVIOC", "dlsym(", "dlopen(", "pthread_",
        "socket(", "gptokey", "KEYCODE_", "signal(",
    )
    for token in forbidden_source:
        if token.lower() in source.lower():
            die(f"Unity owner reads or synthesizes an external route: {token}")

    profile_match = re.search(
        r"typedef struct nxandroid_unity_profile \{(.*?)\n\} "
        r"nxandroid_unity_profile;",
        header,
        flags=re.DOTALL,
    )
    if not profile_match:
        die("profile structure not found")
    profile_body = profile_match.group(1).lower()
    for forbidden_field in ("rva", "offset", "address", "patch"):
        if forbidden_field in profile_body:
            die(f"profile exposes forbidden address mechanism: {forbidden_field}")

    producer_call = source.index("context->ops.producer")
    pending_publish = source.index("pending->valid = 1u", producer_call)
    producer_receipt = source.index("context->producer_returns++", pending_publish)
    if not producer_call < pending_publish < producer_receipt:
        die("producer state advances before callback return")
    api_return_gate = source.index("returned->api_returned != 1")
    low_count = source.index("context->low_level_returns++", api_return_gate)
    action_count = source.index("context->action_returns++", low_count)
    complete_count = source.index("context->complete_receipts++", action_count)
    if not api_return_gate < low_count < action_count < complete_count:
        die("consumer receipt order drifted")

    for text, label, tokens in (
        (readme, "README", ("Strict Unity consumer boundary (C8)", "90-control", "FIXTURE")),
        (contract, "contract", ("C8 Unity consumer ownership", "low-level", "action API")),
        (matrix, "matrix", ("nxandroid 0.5.0", "pending profile", "same-pad SELECT+START")),
        (changelog, "changelog", ("0.5.0", "Legacy Input", "PENDING")),
    ):
        for token in tokens:
            require(text, token, label)

    print(
        "c8 static contract: PASS profiles=5 controls=18 owner_reads=0 "
        "rva_fields=0 keyboard_fallbacks=0"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
