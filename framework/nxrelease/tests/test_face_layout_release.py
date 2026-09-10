#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxrelease 0.3.25 directed gate: the FACE_LAYOUT release contract.

Drives the REAL _validate_v4_optins closure and the REAL defaults-GPTK
acceptance with synthetic packages. Covers the mission classes: the valid
trio; V3 defaults requiring exactly one lowercase FACE_LAYOUT; variants
bound to V3 only; single variant; renamed variant; divergent hash; symlink;
empty; oversize is exercised structurally by the 8 MiB ceiling shared with
the base; pair divergence outside a/b/x/y; equal pins; the mutable GUID
frozen into the invariant base; and the marker/identity requirements
(runtime /3, NXC6-DOMAIN + live-database identity for V3, quarantined
generic fallback)."""

import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import sys
import tempfile

COMPONENT = pathlib.Path(__file__).resolve().parent.parent

MUTABLE = "19000000010000000100000000010000"
MODERN_LINE = (MUTABLE + ",Deeplay-keys,a:b4,b:b3,x:b5,y:b6,start:b10,"
               "back:b9,platform:Linux,")
RETRO_LINE = (MUTABLE + ",Deeplay-keys,a:b3,b:b4,x:b6,y:b5,start:b10,"
              "back:b9,platform:Linux,")
GO_LINE = ("190000004b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,"
           "start:b13,back:b12,platform:Linux,")


def require(condition, message):
    if not condition:
        raise SystemExit("face-layout release gate FAILED: %s" % message)


def load_module():
    spec = importlib.util.spec_from_file_location(
        "nxrelease_flr", COMPONENT / "nxrelease.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["nxrelease_flr"] = module
    spec.loader.exec_module(module)
    return module


def bundle_text(*lines):
    return "NXCONTROLLER_PROFILES/1\n# license=Zlib\n" + \
        "".join(line + "\n" for line in lines)


class Fixture:
    def __init__(self, module, work):
        self.module = module
        self.work = pathlib.Path(work)
        self.port_dir = "example"
        (self.work / self.port_dir).mkdir()
        self.records = []
        self.write("controllers.nxb", bundle_text(GO_LINE))
        self.write("controllers-modern.nxb", bundle_text(MODERN_LINE))
        self.write("controllers-retro.nxb", bundle_text(RETRO_LINE))

    def write(self, name, text):
        path = self.work / self.port_dir / name
        path.write_text(text)
        self.records = [item for item in self.records
                        if item["target"] != self.port_dir + "/" + name]
        self.records.append({
            "target": self.port_dir + "/" + name,
            "actual_path": path,
            "sha256": hashlib.sha256(text.encode()).hexdigest(),
        })
        return hashlib.sha256(text.encode()).hexdigest()

    def sha(self, name):
        return next(item["sha256"] for item in self.records
                    if item["target"] == self.port_dir + "/" + name)

    def contract(self, variants=True, base_sha=None, modern_sha=None,
                 retro_sha=None, modern_name="controllers-modern.nxb",
                 retro_name="controllers-retro.nxb", drop=None):
        profiles = {
            "enabled": True,
            "bundle": "controllers.nxb",
            "sha256": base_sha or self.sha("controllers.nxb"),
        }
        if variants:
            entry = {
                "modern": {"bundle": modern_name,
                           "sha256": modern_sha or
                           self.sha("controllers-modern.nxb")},
                "retro": {"bundle": retro_name,
                          "sha256": retro_sha or
                          self.sha("controllers-retro.nxb")},
            }
            if drop:
                entry.pop(drop)
            profiles["face_layout_variants"] = entry
        return {"input_controller_profiles": profiles}

    def run(self, adapter_contract, gptk_schema=3):
        config = {"port_dir": self.port_dir, "gptk_schema": gptk_schema,
                  "records": self.records}
        self.module._validate_v4_optins(
            adapter_contract, config, records=self.records)

    def expect(self, adapter_contract, fragment, why, gptk_schema=3):
        try:
            self.run(adapter_contract, gptk_schema=gptk_schema)
        except self.module.ReleaseError as error:
            require(fragment in str(error),
                    "%s: error does not name %r: %s" % (why, fragment, error))
            return
        raise SystemExit("face-layout release gate FAILED: %s was accepted"
                         % why)


def main():
    module = load_module()
    work = tempfile.mkdtemp(prefix="nxrel-face-layout.")
    try:
        fixture = Fixture(module, work)

        # Positive: the valid trio under a V3 default.
        fixture.run(fixture.contract())
        print("ok   the valid trio passes under V3")

        # Variants demand V3.
        fixture.expect(fixture.contract(), "NEXTOS_CONTROLLERS/3",
                       "variants under a V2 default", gptk_schema=2)
        # V3 + enabled profiles demand the pair.
        fixture.expect(fixture.contract(variants=False),
                       "face_layout_variants",
                       "a V3 port without the pair")
        # Single variant.
        fixture.expect(fixture.contract(drop="retro"), "complete",
                       "a single variant")
        # Renamed variant bundle.
        fixture.expect(fixture.contract(modern_name="controllers-m.nxb"),
                       "controllers-modern.nxb", "a renamed variant")
        # Hash divergence.
        fixture.expect(fixture.contract(modern_sha="0" * 64),
                       "SHA-256", "a divergent variant hash")
        # Equal pins.
        fixture.write("controllers-retro.nxb", bundle_text(MODERN_LINE))
        fixture.expect(fixture.contract(), "distinct",
                       "equal modern/retro artifacts")
        fixture.write("controllers-retro.nxb", bundle_text(RETRO_LINE))
        # Pair diverging outside a/b/x/y.
        fixture.write("controllers-retro.nxb", bundle_text(
            RETRO_LINE.replace("start:b10", "start:b11")))
        fixture.expect(fixture.contract(), "a/b/x/y",
                       "a pair diverging outside the face")
        fixture.write("controllers-retro.nxb", bundle_text(RETRO_LINE))
        # Different identity sets.
        fixture.write("controllers-retro.nxb", bundle_text(
            RETRO_LINE, GO_LINE))
        fixture.expect(fixture.contract(), "identity set",
                       "variants with different identity sets")
        fixture.write("controllers-retro.nxb", bundle_text(RETRO_LINE))
        # The mutable GUID frozen into the base (the 0.9.0 defect).
        fixture.write("controllers.nxb", bundle_text(GO_LINE, MODERN_LINE))
        fixture.expect(fixture.contract(), "invariant base",
                       "the mutable GUID inside controllers.nxb")
        fixture.write("controllers.nxb", bundle_text(GO_LINE))
        # A variant absent from the package.
        records_backup = list(fixture.records)
        fixture.records = [item for item in fixture.records
                           if not item["target"].endswith(
                               "controllers-retro.nxb")]
        fixture.expect(fixture.contract(
            retro_sha=hashlib.sha256(
                bundle_text(RETRO_LINE).encode()).hexdigest()),
            "does not carry", "a variant missing from the package")
        fixture.records = records_backup
        # A symlinked variant.
        real = fixture.work / fixture.port_dir / "controllers-retro.nxb"
        link = fixture.work / fixture.port_dir / "retro-link.nxb"
        os.symlink(real, link)
        for item in fixture.records:
            if item["target"].endswith("controllers-retro.nxb"):
                item["actual_path"] = link
        fixture.expect(fixture.contract(), "symlink", "a symlinked variant")
        for item in fixture.records:
            if item["target"].endswith("controllers-retro.nxb"):
                item["actual_path"] = real
        # An empty variant.
        digest_before = fixture.sha("controllers-retro.nxb")
        fixture.write("controllers-retro.nxb", "")
        fixture.expect(fixture.contract(
            retro_sha=hashlib.sha256(b"").hexdigest()),
            "empty", "an empty variant")
        fixture.write("controllers-retro.nxb", bundle_text(RETRO_LINE))
        require(fixture.sha("controllers-retro.nxb") == digest_before,
                "fixture self-check")
        print("ok   every variant negative fails closed naming its cause")

        # V3 defaults: exactly one lowercase FACE_LAYOUT line.
        source = (COMPONENT / "nxrelease.py").read_text()
        for token in (
                'format = NEXTOS_CONTROLLERS/3',
                'requires exactly one',
                'FACE_LAYOUT must be exactly auto, modern or retro',
                'must not carry it'):
            require(token in source,
                    "the V3 defaults acceptance lost %r" % token)
        # Marker and 0.10.0 identity.
        require('INPUT_RUNTIME_MARKER = "nxinput-gptk-runtime/3"' in source,
                "the live runtime marker is not /3")
        for token in ('NXC6-DOMAIN', '/usr/lib/gamecontrollerdb.txt',
                      'nx_add_generic_gamepad_mappings',
                      'Generic Xbox Fallback'):
            require(token in source,
                    "the executable identity/quarantine lost %r" % token)
        print("ok   V3 defaults, /3 marker, 0.10.0 identity and quarantine "
              "are enforced in source")

        # The generated-defaults CLOSURE with a real V3 body: the full
        # explicit tri-state (every unbound control `null`) must be accepted,
        # incompleteness and undeclared actions must still fail.
        controls = ("A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
                    "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
                    "LEFT_STICK", "RIGHT_STICK")
        adapter_input = {"actions": [
            {"id": "menu.accept", "sinks": ["engine.ui_accept"]},
            {"id": "player.jump", "sinks": ["engine.input.jump"]},
        ]}
        def v3_body(bind_menu="menu.accept", complete=True, extra=None):
            lines = ["format = NEXTOS_CONTROLLERS/3", "port = example",
                     "FACE_LAYOUT = auto", "[menu]"]
            names = controls if complete else controls[:-1]
            for name in names:
                lines.append("%s = %s" % (
                    name, bind_menu if name == "A" else "null"))
            lines.append("[gameplay]")
            for name in names:
                lines.append("%s = %s" % (
                    name, (extra or "player.jump") if name == "A" else "null"))
            return "\n".join(lines) + "\n"
        module._validate_gptk_closure(v3_body(), 3, adapter_input)
        for body, why, fragment in (
                (v3_body(complete=False), "an incomplete V3 section",
                 "omits"),
                (v3_body(extra="player.fly"), "an undeclared V3 action",
                 "does not declare"),
        ):
            try:
                module._validate_gptk_closure(body, 3, adapter_input)
            except module.ReleaseError as error:
                require(fragment in str(error),
                        "%s: error does not name %r: %s"
                        % (why, fragment, error))
            else:
                raise SystemExit(
                    "face-layout release gate FAILED: %s was accepted" % why)
        try:
            module._validate_gptk_closure(v3_body(), 3, {"actions": []})
        except module.ReleaseError as error:
            require("adapter contract" in str(error),
                    "closable V3 without actions: wrong error: %s" % error)
        else:
            raise SystemExit("face-layout release gate FAILED: a V3 default "
                             "without a closable contract was accepted")
        print("ok   the V3 tri-state closure accepts null and stays closed")

        print("nxrelease 0.3.25 face-layout release gate: PASS positive=1 "
              "negatives=11 closure=4 source_contract=9")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
