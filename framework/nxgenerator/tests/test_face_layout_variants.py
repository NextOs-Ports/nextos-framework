#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxgenerator 0.3.15: the controls.schema-3 FACE_LAYOUT variant pair.

The declarative opt-in of nxinput 0.10.0: under controls.schema 3 an enabled
controller_profiles block must pin the COMPLETE modern/retro pair (fixed
bundle names, one SHA-256 each, distinct artifacts), and nothing about the
pair is accepted under schema 1/2 (sealed oracle cases N10-N12). The
validation is exercised through validate_project -- the same door the
generation runs through -- never through a private re-implementation.
"""

import copy
import importlib.util
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_generator():
    spec = importlib.util.spec_from_file_location(
        "nxgenerator_flv", ROOT / "nxgenerator.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def main():
    generator = load_generator()
    base = json.loads(
        (ROOT / "tests" / "fixtures" / "c4-baseline" /
         "nxproject.json").read_text(encoding="utf-8"))
    # 0.3.15 guard: schema >= 2 refuses an undeclared D-pad cluster, and this
    # gate exercises schema-3 projects; the fixture stays a schema-1 literal.
    for name, bindings in base["controls"]["contexts"].items():
        if name != "cursor":
            for control in ("UP", "DOWN", "LEFT", "RIGHT"):
                bindings.setdefault(control, "native")

    sha_a = "a" * 64
    sha_b = "b" * 64
    sha_c = "c" * 64

    def project(mutate):
        document = copy.deepcopy(base)
        mutate(document)
        return document

    def expect_error(mutate, fragment, why):
        document = project(mutate)
        try:
            generator.validate_project(document)
        except generator.ProjectError as error:
            require(fragment in str(error),
                    "%s: error does not name %r: %s" % (why, fragment, error))
            return
        raise GateError("%s was accepted" % why)

    def with_profiles(document, variants=True, schema=3):
        controls = document["controls"]
        controls["schema"] = schema
        block = {"enabled": True, "bundle": "controllers.nxb",
                 "sha256": sha_a}
        if variants:
            block["face_layout_variants"] = {
                "modern": {"bundle": "controllers-modern.nxb",
                           "sha256": sha_b},
                "retro": {"bundle": "controllers-retro.nxb",
                          "sha256": sha_c},
            }
        controls["controller_profiles"] = block

    # Positive: schema 3 + enabled profiles + the complete pair.
    document = project(lambda d: with_profiles(d))
    config = generator.validate_project(document)
    profiles = config["controller_profiles"]
    require(profiles["face_layout_variants"]["modern"]["bundle"] ==
            "controllers-modern.nxb" and
            profiles["face_layout_variants"]["retro"]["sha256"] == sha_c,
            "the validated pair does not carry the fixed names and pins")
    require(config["controls"]["face_layout"] == "auto",
            "schema 3 must default face_layout to auto")

    # N12: a single variant is never a pair.
    def single(d):
        with_profiles(d)
        del d["controls"]["controller_profiles"][
            "face_layout_variants"]["retro"]
    expect_error(single, "complete", "a single variant")

    # Fixed names: a renamed variant bundle fails.
    def renamed(d):
        with_profiles(d)
        d["controls"]["controller_profiles"]["face_layout_variants"][
            "modern"]["bundle"] = "controllers-Modern.nxb"
    expect_error(renamed, "controllers-modern.nxb", "a renamed variant")

    # Equal pins would freeze one layout twice.
    def equal_pins(d):
        with_profiles(d)
        d["controls"]["controller_profiles"]["face_layout_variants"][
            "retro"]["sha256"] = sha_b
    expect_error(equal_pins, "distinct", "an equal-pin pair")

    # N10/N11: nothing V3 enters schema 1/2.
    expect_error(lambda d: with_profiles(d, schema=2),
                 "requires controls.schema 3", "variants under schema 2")
    expect_error(lambda d: d["controls"].__setitem__("face_layout", "auto"),
                 "requires controls.schema 3", "face_layout under schema 1")

    # Schema 3 with enabled profiles REQUIRES the pair.
    expect_error(lambda d: with_profiles(d, variants=False),
                 "face_layout_variants", "schema 3 profiles without the pair")

    # A disabled block must not smuggle variants in.
    def disabled(d):
        with_profiles(d)
        d["controls"]["controller_profiles"] = {
            "enabled": False, "bundle": "", "sha256": "",
            "face_layout_variants": {}}
    expect_error(disabled, "disabled", "variants on a disabled block")

    print("nxgenerator 0.3.15 face-layout variants: PASS positives=1 "
          "negatives=7")
    return 0


if __name__ == "__main__":
    sys.exit(main())
