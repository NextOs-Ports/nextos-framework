#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Directed regressions for credential scanning in annotated Python sources."""

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "nxrelease.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("nxrelease_secret_scan", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def accepted(module, label, payload, logical_path):
    try:
        module._reject_private_chunk(payload, logical_path, True)
    except module.ReleaseError as error:
        raise AssertionError("{} was rejected: {}".format(label, error))


def rejected(module, label, payload, logical_path):
    try:
        module._reject_private_chunk(payload, logical_path, True)
    except module.ReleaseError as error:
        require("credential/secret literal" in str(error),
                "{} failed for the wrong reason: {}".format(label, error))
        return
    raise AssertionError(label + " unexpectedly passed")


def main():
    module = load_tool()
    require(module.TOOL_VERSION == "0.4.11",
            "NXRelease version authority is not 0.4.11")

    accepted(
        module, "UnityPy Optional annotation",
        b"m_VCPassword: Optional[str] = None\n", "UnityPy/generated.py")
    accepted(
        module, "plain Python annotation",
        b"password: CredentialType\n", "types.pyi")
    accepted(
        module, "case-insensitive Python suffix",
        b"api_key: list[str] = None\n", "GENERATED.PY")

    rejected(
        module, "plain assignment",
        b"password=abcdefgh\n", "settings.py")
    rejected(
        module, "spaced assignment",
        b"api_key = abcdefgh\n", "settings.py")
    rejected(
        module, "annotated assignment",
        b"m_VCPassword: Optional[str] = abcdefgh\n", "generated.py")
    rejected(
        module, "mapping syntax outside Python",
        b"password: abcdefgh\n", "settings.yaml")

    print(
        "nxrelease 0.4.11 secret literal scan: PASS "
        "python_annotations=3 real_literals_rejected=4")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
