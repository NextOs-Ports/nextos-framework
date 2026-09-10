#!/usr/bin/env python3
"""Directed static boundary checks for the two field regressions."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

port = Path(sys.argv[1]).resolve()
repo = Path(sys.argv[2]).resolve()


def check(condition, message):
    if not condition:
        raise SystemExit("BLOSSOM V5 INTEGRATION: FAIL -- " + message)


input_source = (port / "src/bt_input.c").read_text(encoding="utf-8")
imports = (port / "src/imports.gen.c").read_text(encoding="utf-8")
build = (port / "build_buster.sh").read_text(encoding="utf-8")
present = (port / "src/present_fbo.c").read_text(encoding="utf-8")

manifest_tool_path = port / "devtools" / "make_nxrelease.py"
spec = importlib.util.spec_from_file_location("bt_make_nxrelease",
                                              manifest_tool_path)
manifest_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest_tool)

check("nxc6_stage_before_init(" in input_source,
      "port does not use provider-aware staging")
check("nxinput_sdl_seam_stage_before_init(" not in input_source,
      "legacy blind staging survived")
check("SDL_WasInit(0)" not in input_source,
      "port still carries the any-subsystem predicate")
check("bt_fbo_compat_extensions(" in imports and
      "bt_fbo_compat_resolve(" in imports,
      "FBO capability and symbol boundaries are not both wired")
check("sb_present_fbo_known_set_ready(count)" in present and
      "if (count < 4) return" not in present,
      "three-FBO KMSDRM compositor is still gated on a fourth FBO")
check("sdv_glTexImage2D_track" in imports and
      "sb_fbo_tracking_enabled()" in imports and
      "sb_rawscale_min() > 0 ||" in imports,
      "FBO texture geometry is still coupled to optional raw downscaling")
for required in ("src/fbo_compat.c", "nxinput_prerouter.c",
                 "nxinput_axis_calib.c", "BT-FBO-COMPAT/1"):
    check(required in build, "build omits %s" % required)
check(manifest_tool.classify("blossomtales/controllers.nxb") ==
      ("payload", "0644"),
      "controller profiles were confused with the nxruntime seed")
check(manifest_tool.classify("blossomtales/nxruntime-deadbeef.nxb") ==
      ("nxruntime-seed", "0644"),
      "nxruntime seed classification regressed")

for component, expected in (("nxinput", "0.11.8"),
                            ("nxcompat", "0.5.3")):
    pins_path = port / "vendor" / component / "PINS.json"
    pins = json.loads(pins_path.read_text(encoding="utf-8"))
    check(pins.get(component + "_version") == expected,
          "%s vendor is not %s" % (component, expected))
    for relative, digest in pins["files"].items():
        copied = port / "vendor" / component / relative
        upstream = repo / "framework" / component / relative
        check(copied.is_file() and upstream.is_file(),
              "%s/%s is missing" % (component, relative))
        actual = hashlib.sha256(copied.read_bytes()).hexdigest()
        source = hashlib.sha256(upstream.read_bytes()).hexdigest()
        check(actual == digest == source,
              "%s/%s is not byte-exact" % (component, relative))

print("BLOSSOM V5 INTEGRATION: PASS staging=provider-aware fbo=closure-gated vendors=exact")
