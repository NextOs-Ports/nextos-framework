#!/usr/bin/env python3
"""Regression: a sparse [override.<ctx>] that suppresses or remaps a trigger
must target the channel (analog/digital) fixed by the BASE trigger mode, not
the override binding's kind.

Root cause (found on Tearscape 0.2.17, dArkOS K36S, menu session):
`render_controls_sections_v4` picked the channel from `kinds.get(override_binding)`
which is None for a suppressed trigger, so it always emitted
`trigger.left.analog = null`. The schema-4 bridge resolves whichever slot the
base trigger declares (digital, when the base action is a button), never finds
the override on the digital slot and inherits the base action -- so L2/R2
leaked player.use_shield/use_tool into the menu context and the ON_DEVICE proof
failed `suppressed`/`no_delivery`.
"""
import importlib.util
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nxg", ROOT / "nxgenerator.py")
nxg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nxg)


def render(controls):
    return nxg.render_controls_sections_v4(controls)


def section(text, name):
    m = re.search(r"\[%s\](.*?)(\n\[|\Z)" % re.escape(name), text, re.S)
    return m.group(1) if m else ""


def test_digital_trigger_suppressed_in_override_uses_digital_channel():
    controls = {
        "actions": [
            {"id": "menu.accept", "kind": "button"},
            {"id": "player.roll", "kind": "button"},
            {"id": "player.use_shield", "kind": "button"},
            {"id": "player.use_tool", "kind": "button"},
        ],
        "contexts": {
            "gameplay": {"A": "player.roll", "L2": "player.use_shield",
                         "R2": "player.use_tool"},
            "menu": {"A": "menu.accept"},  # L2/R2 absent -> suppressed
        },
    }
    out = render(controls)
    ov = section(out, "override.menu")
    assert "trigger.left.digital = null" in ov, ov
    assert "trigger.right.digital = null" in ov, ov
    assert "trigger.left.analog" not in ov, ov
    assert "trigger.right.analog" not in ov, ov
    # base declares the digital channel
    assert "mode = digital" in section(out, "trigger.left")


def test_analog_trigger_remap_in_override_uses_analog_channel():
    controls = {
        "actions": [
            {"id": "menu.accept", "kind": "button"},
            {"id": "player.brake", "kind": "analog"},
            {"id": "player.creep", "kind": "analog"},
        ],
        "contexts": {
            "gameplay": {"A": "menu.accept", "L2": "player.brake"},
            "menu": {"A": "menu.accept", "L2": "player.creep"},  # remap on analog
        },
    }
    out = render(controls)
    ov = section(out, "override.menu")
    assert "trigger.left.analog = action:player.creep" in ov, ov
    assert "trigger.left.digital" not in ov, ov
    assert "mode = analog" in section(out, "trigger.left")


def test_trigger_null_in_base_button_in_override_declares_digital_channel():
    """0.4.4 (Nameless Cat 1.2.8): R2 null in gameplay, cursor.click in menu.
    Mutant (0.4.3 logic: channel from the BASE binding only) writes
    `trigger.right.analog = action:cursor.click` -- a kind mismatch the
    schema-4 parser rejects -- and `mode = analog` in the base section."""
    controls = {
        "actions": [
            {"id": "menu.accept", "kind": "button"},
            {"id": "cursor.click", "kind": "button"},
        ],
        "contexts": {
            "gameplay": {"A": "menu.accept", "R2": "null"},
            "menu": {"A": "menu.accept", "R2": "cursor.click"},
        },
    }
    out = render(controls)
    ov = section(out, "override.menu")
    assert "trigger.right.digital = action:cursor.click" in ov, ov
    assert "trigger.right.analog" not in ov, ov
    tr = section(out, "trigger.right")
    assert "mode = digital" in tr, tr
    assert "digital = null" in tr, tr
    assert "analog = null" in tr, tr
    # the untouched trigger stays analog/null
    assert "mode = analog" in section(out, "trigger.left")


def test_trigger_bound_to_button_and_analog_across_contexts_is_refused():
    controls = {
        "actions": [
            {"id": "cursor.click", "kind": "button"},
            {"id": "player.brake", "kind": "analog"},
        ],
        "contexts": {
            "gameplay": {"R2": "player.brake"},
            "menu": {"R2": "cursor.click"},
        },
    }
    try:
        render(controls)
    except nxg.ProjectError as exc:
        assert "R2" in str(exc), exc
    else:
        raise AssertionError("contradictory trigger channel was accepted")


if __name__ == "__main__":
    failed = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print("PASS", name)
            except AssertionError as exc:
                failed += 1
                print("FAIL", name, exc)
    sys.exit(1 if failed else 0)
