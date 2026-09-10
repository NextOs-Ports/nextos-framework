#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C4: nxrelease side of NEXTOSCONTROLLERS v2.

Two claims, checked against the adapter contract that ships in the same
package:

  * every action the default binds exists in the contract WITH at least one
    real sink -- a control bound to an action nothing consumes is a dead
    control the owner cannot tell from a working one;
  * every control appears exactly ONCE per section, and in schema 2 every
    section lists all 18, so the owner sees the whole pad and no duplicate
    can let file order decide.

Counts printed below are derived from what actually ran.
"""

import importlib.util
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[3]
TOOL = REPOSITORY / "framework" / "nxrelease" / "nxrelease.py"

CONTROLS = ("A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
            "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
            "LEFT_STICK", "RIGHT_STICK")

COUNTS = {"accepts": 0, "refuses": 0}


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_tool():
    specification = importlib.util.spec_from_file_location(
        "nxrelease_c4_gptk_under_test", TOOL)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


ADAPTER = {
    "actions": [
        {"id": "ui.confirm", "kind": "button", "sinks": ["adapter.ui.ok"]},
        {"id": "game.cancel", "kind": "button", "sinks": ["adapter.cancel"]},
        {"id": "camera.look", "kind": "vector", "sinks": ["adapter.look"]},
    ]
}


def v2_file(gameplay=None, menu=None, omit=(), extra=""):
    gameplay = gameplay or {}
    menu = menu or {}
    lines = ["format = NEXTOS_CONTROLLERS/2", "port = fixture", "", "[menu]"]
    for control in CONTROLS:
        if control in omit:
            continue
        lines.append("%s = %s" % (control, menu.get(control, "null")))
    lines += ["", "[gameplay]"]
    for control in CONTROLS:
        lines.append("%s = %s" % (control, gameplay.get(control, "null")))
    return "\n".join(lines) + "\n" + extra


def accepts(tool, text, schema, why):
    try:
        tool._validate_gptk_closure(text, schema, ADAPTER)
    except tool.ReleaseError as error:
        raise GateError("%s: %s" % (why, error))
    COUNTS["accepts"] += 1


def refuses(tool, text, schema, fragment, why):
    try:
        tool._validate_gptk_closure(text, schema, ADAPTER)
    except tool.ReleaseError as error:
        require(fragment in str(error),
                "%s: refusal did not mention %r (%s)" % (why, fragment, error))
        COUNTS["refuses"] += 1
        return
    raise GateError("nxrelease accepted %s" % why)


def main():
    tool = load_tool()

    # --- positives ------------------------------------------------------
    accepts(tool, v2_file(gameplay={"A": "game.cancel", "X": "native",
                                    "RIGHT_STICK": "camera.look"}), 2,
            "a complete v2 default whose actions all have sinks")
    accepts(tool,
            "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n"
            "[gameplay]\nA = game.cancel\n", 1,
            "a v1 default with two bound controls")
    # Tuning keys live beside the controls and are not controls.
    accepts(tool, v2_file(gameplay={"RIGHT_STICK": "camera.look"},
                          extra="[camera]\nsensitivity_x = 1.5\n"), 2,
            "a [camera] tuning section is not held to the control contract")

    # --- negatives ------------------------------------------------------
    refuses(tool, v2_file(gameplay={"A": "game.orphan"}), 2,
            "does not declare with a sink",
            "an action the adapter contract never declares")
    refuses(tool,
            "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n"
            "A = game.cancel\n", 1,
            "repeats control", "a duplicated control in one section")
    refuses(tool, v2_file(omit=("R3",)), 2, "omits R3",
            "a v2 section that omits a control")
    refuses(tool,
            "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = null\n", 1,
            "declares NEXTOS_CONTROLLERS/1",
            "null in a file that declares v1")
    refuses(tool,
            "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = native\n", 1,
            "declares NEXTOS_CONTROLLERS/1",
            "native in a file that declares v1")
    refuses(tool,
            v2_file() + "HOME = game.cancel\n", 2,
            "unknown control", "an unknown control name")

    # A SCAFFOLD contract (no actions at all) keeps working: a legacy or
    # not-yet-release-ready generation ships the generic v1 default and there
    # is no allowlist to close against. The structural rules still apply.
    scaffold = {"actions": []}
    tool._validate_gptk_closure(
        "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n"
        "[gameplay]\nA = player.primary\n", 1, scaffold)
    COUNTS["accepts"] += 1
    try:
        tool._validate_gptk_closure(
            "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n"
            "A = ui.cancel\n", 1, scaffold)
    except tool.ReleaseError as error:
        require("repeats control" in str(error),
                "a scaffold still must not repeat a control: %s" % error)
        COUNTS["refuses"] += 1
    else:
        raise GateError("a scaffold was allowed to repeat a control")
    # But a v2 default with no declared actions is a contradiction: v2 is
    # opt-in on a schema-3 port, which always declares its actions.
    try:
        tool._validate_gptk_closure(v2_file(), 2, scaffold)
    except tool.ReleaseError as error:
        require("declares input.actions with sinks" in str(error),
                "a v2 scaffold was refused for the wrong reason: %s" % error)
        COUNTS["refuses"] += 1
    else:
        raise GateError("nxrelease accepted a v2 default with no actions")

    # A legacy contract shape is not closable, and not an error either: the
    # structural rules still run, the action check simply does not.
    legacy = {"actions": ["ui.confirm", "player.primary"]}
    tool._validate_gptk_closure(
        "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n", 1,
        legacy)
    COUNTS["accepts"] += 1
    try:
        tool._validate_gptk_closure(v2_file(), 2, legacy)
    except tool.ReleaseError as error:
        require("declares input.actions with sinks" in str(error),
                "a v2 file over a legacy shape was refused wrongly: %s"
                % error)
        COUNTS["refuses"] += 1
    else:
        raise GateError("nxrelease accepted a v2 default over a legacy shape")

    # An action declared WITHOUT a usable sink poisons the whole contract.
    starved = {"actions": [{"id": "game.cancel", "kind": "button",
                            "sinks": []}]}
    try:
        tool._validate_gptk_closure(v2_file(gameplay={"A": "game.cancel"}),
                                    2, starved)
    except tool.ReleaseError as error:
        require("declares no usable sink" in str(error),
                "a sinkless action was refused for the wrong reason: %s"
                % error)
        COUNTS["refuses"] += 1
    else:
        raise GateError("nxrelease accepted an action with no sink at all")

    print("nxrelease C4 gptk closure gate passed: accepts=%d refuses=%d "
          "controls=%d" % (COUNTS["accepts"], COUNTS["refuses"],
                           len(CONTROLS)))


if __name__ == "__main__":
    main()
