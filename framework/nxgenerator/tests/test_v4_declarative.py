#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4 declarative opt-ins in nxproject schema 3.

Three fronts reach the port through the manifest, all default-OFF:

  * V4-DISPLAY-01  -> ``display``
  * V4-GRAPHICS-03 -> ``graphics.egl_binding``
  * V4-CONTROLLERS-02 -> ``controls.sdl3_portmaster``

The gate proves the validators are pure and fail closed, and that a manifest
that declares none of them normalizes to the exact no-op values -- so no
already approved port changes behaviour by being regenerated.
"""

import importlib.util
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[3]
TOOL = REPOSITORY / "framework" / "nxgenerator" / "nxgenerator.py"


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_tool():
    specification = importlib.util.spec_from_file_location(
        "nxgenerator_v4_under_test", TOOL)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def refuses(tool, call, message):
    try:
        call()
    except tool.ProjectError:
        return
    raise GateError(message)


def check_display(tool):
    # Absence is the no-op. This is the single most important property: a
    # regenerated V3 port must not acquire a viewport it never asked for.
    require(tool.validate_display(None, 3) is None,
            "an absent display block is not None")

    plan = tool.validate_display({"policy": "game"}, 3)
    require(plan == {"policy": "game", "remap_input": False},
            "policy game did not normalize to a pure no-op")
    refuses(tool, lambda: tool.validate_display(
        {"policy": "game", "internal_width": 640}, 3),
        "policy game accepted a presentation field")

    plan = tool.validate_display(
        {"policy": "preserve", "internal_width": 640, "internal_height": 480,
         "remap_input": True}, 3)
    require(plan["policy"] == "preserve" and plan["internal_width"] == 640 and
            plan["remap_input"] is True, "preserve did not normalize")

    plan = tool.validate_display(
        {"policy": "adaptive", "internal_width": 640, "internal_height": 480,
         "min_width": 320, "min_height": 240, "max_width": 960,
         "max_height": 540}, 3)
    require(plan["max_width"] == 960 and plan["min_height"] == 240,
            "adaptive limits were not preserved")

    for bad, why in (
        ({"policy": "mali"}, "a policy named after a GPU"),
        ({"policy": "Preserve", "internal_width": 640,
          "internal_height": 480}, "a case-variant policy"),
        ({"policy": "preserve"}, "preserve without an internal size"),
        ({"policy": "preserve", "internal_width": 0,
          "internal_height": 480}, "a zero internal width"),
        ({"policy": "preserve", "internal_width": 640,
          "internal_height": 480, "min_width": 320,
          "min_height": 240}, "limits on a non-adaptive policy"),
        ({"policy": "adaptive", "internal_width": 640,
          "internal_height": 480}, "adaptive without a declared maximum"),
        ({"policy": "adaptive", "internal_width": 640, "internal_height": 480,
          "min_width": 800, "min_height": 600, "max_width": 640,
          "max_height": 480}, "a maximum below the minimum"),
        ({"policy": "adaptive", "internal_width": 640, "internal_height": 480,
          "max_width": 960}, "a half-declared limit pair"),
        ({"policy": "preserve", "internal_width": 640, "internal_height": 480,
          "device": "rg40xx"}, "an unknown field"),
        ({"policy": "preserve", "internal_width": True,
          "internal_height": 480}, "a boolean extent"),
    ):
        refuses(tool, lambda bad=bad: tool.validate_display(bad, 3),
                "display accepted %s" % why)
    refuses(tool, lambda: tool.validate_display({"policy": "game"}, 2),
            "display was accepted on schema 2")


def check_egl_binding(tool):
    gl = {"uses_gl": True}
    require(tool.validate_egl_binding(None, 3, gl) is None,
            "an absent egl_binding is not None")
    require(tool.validate_egl_binding(
                {"enabled": False, "imports": []}, 3, None) ==
            {"enabled": False, "imports": []},
            "a disabled egl_binding did not normalize")

    imports = sorted(["eglGetCurrentContext", "eglGetCurrentDisplay",
                      "eglSwapBuffers", "eglMakeCurrent"])
    binding = tool.validate_egl_binding(
        {"enabled": True, "imports": imports}, 3, gl)
    require(binding == {"enabled": True, "imports": imports},
            "an enabled egl_binding did not normalize")

    for bad, context, why in (
        ({"enabled": True, "imports": imports}, {"uses_gl": False},
         "an enabled binding on a non-GL port"),
        ({"enabled": True, "imports": imports}, None,
         "an enabled binding with no graphics contract"),
        ({"enabled": True, "imports": []}, gl, "an empty inventory"),
        ({"enabled": True, "imports": ["eglSwapBuffers"]}, gl,
         "an inventory without the ownership probe"),
        ({"enabled": True,
          "imports": ["eglGetCurrentContext", "glClear"]}, gl,
         "a non-EGL symbol"),
        ({"enabled": True,
          "imports": ["eglSwapBuffers", "eglGetCurrentContext"]}, gl,
         "an unordered inventory"),
        ({"enabled": True, "imports": ["eglGetCurrentContext",
                                       "eglGetCurrentContext"]}, gl,
         "a duplicated import"),
        ({"enabled": False, "imports": ["eglGetCurrentContext"]}, gl,
         "a disabled binding that still pins an inventory"),
        ({"enabled": True}, gl, "a binding without an inventory"),
    ):
        refuses(tool,
                lambda bad=bad, context=context:
                    tool.validate_egl_binding(bad, 3, context),
                "egl_binding accepted %s" % why)


def check_sdl3_portmaster(tool):
    digest = "a" * 64
    require(tool.validate_sdl3_portmaster(None, 3) is None,
            "an absent sdl3_portmaster is not None")
    require(tool.validate_sdl3_portmaster(
                {"enabled": False, "private_sdl3_sha256": ""}, 3) ==
            {"enabled": False, "private_sdl3_sha256": ""},
            "a disabled sdl3_portmaster did not normalize")
    require(tool.validate_sdl3_portmaster(
                {"enabled": True, "private_sdl3_sha256": digest}, 3) ==
            {"enabled": True, "private_sdl3_sha256": digest},
            "an enabled sdl3_portmaster did not normalize")
    for bad, why in (
        ({"enabled": True, "private_sdl3_sha256": ""},
         "an enabled opt-in with no pinned SDL3"),
        ({"enabled": True, "private_sdl3_sha256": "A" * 64},
         "an uppercase digest"),
        ({"enabled": True, "private_sdl3_sha256": "a" * 63},
         "a truncated digest"),
        ({"enabled": False, "private_sdl3_sha256": "a" * 64},
         "a disabled opt-in that still pins an SDL3"),
        ({"enabled": True}, "a block without the digest field"),
    ):
        refuses(tool, lambda bad=bad: tool.validate_sdl3_portmaster(bad, 3),
                "sdl3_portmaster accepted %s" % why)


def main():
    tool = load_tool()
    check_display(tool)
    check_egl_binding(tool)
    check_sdl3_portmaster(tool)
    print("nxgenerator V4 declarative gate passed: display=1 egl_binding=1 "
          "sdl3_portmaster=1 defaults_are_noop=1")


if __name__ == "__main__":
    main()
