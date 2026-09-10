#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Directed gate for NXExtract's optional top-level validate array."""

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "nxrelease.py"


def load_tool():
    spec = importlib.util.spec_from_file_location(
        "nxrelease_recipe_validate", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def accepted(module, label, recipe):
    try:
        module.validate_nxextract_recipe_arrays(recipe)
    except module.ReleaseError as error:
        raise AssertionError("{} was rejected: {}".format(label, error))


def rejected(module, label, recipe, fragment):
    try:
        module.validate_nxextract_recipe_arrays(recipe)
    except module.ReleaseError as error:
        if fragment.casefold() not in str(error).casefold():
            raise AssertionError(
                "{} failed for the wrong reason: {}".format(label, error))
        return
    raise AssertionError(label + " unexpectedly passed")


def main():
    module = load_tool()
    if module.TOOL_VERSION != "0.4.11":
        raise AssertionError("NXRelease version authority is not 0.4.11")

    accepted(module, "missing validate", {"extract": [], "commit": []})
    accepted(
        module, "explicit empty validate",
        {"extract": [], "validate": [], "commit": []})
    rejected(
        module, "object validate",
        {"extract": [], "validate": {}, "commit": []},
        "validate must be an array")

    # The compatibility change is intentionally limited to validate.
    rejected(module, "missing extract", {"commit": []},
             "extract must be an array")
    rejected(module, "missing commit", {"extract": []},
             "commit must be an array")

    print(
        "nxrelease 0.4.11 NXExtract recipe validate: PASS "
        "missing=1 explicit_empty=1 non_array_rejected=1 roots_required=2")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
