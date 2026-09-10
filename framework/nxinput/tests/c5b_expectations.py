#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""What each engine MUST answer, derived from its own pinned source.

The 116A matrix recorded what the engine printed and counted the rows. That
cannot fail: a row where the engine says nothing looks the same as a row it
got right. This module turns the mapping into a PREDICTION -- for every
physical evdev code, the exact logical button or axis the engine has to
report -- so a silent row is a failure and a wrong index is a failure.

The prediction is not invented here. `_joy_buttons[]` and `_joy_axes[]` are
read out of the pinned engine sources, and the two majors genuinely differ:

  Godot 3  buttons a,b,x,y,leftshoulder,rightshoulder,lefttrigger,...
           axes    leftx,lefty,rightx,righty            (four; the triggers
                                                         are BUTTON outputs)
  Godot 4  buttons a,b,x,y,back,guide,start,leftstick,...
           axes    leftx,lefty,rightx,righty,lefttrigger,righttrigger

so `back` is 10 on Godot 3 and 4 on Godot 4, and `lefttrigger` is a button on
one and an axis on the other. A single table would have hidden that.

CLAIM CLASS: SOURCE_AUDIT for the tables, and the running engine is what
confirms or refuses the prediction they produce.
"""
import pathlib
import re

# Where each major keeps the tables, relative to its checkout root.
TABLE_SOURCE = {
    "godot3": "main/input_default.cpp",
    "godot4": "core/input/input.cpp",
}

ARRAY = {
    "buttons": "_joy_buttons",
    "axes": "_joy_axes",
}


def _read_array(text, name):
    """Pull the string list out of `static const char *<name>[...] = {...};`"""
    match = re.search(r"static const char \*%s\[[^\]]*\]\s*=\s*\{(.*?)\};"
                      % re.escape(name), text, re.S)
    if not match:
        raise RuntimeError("%s is not in this source" % name)
    return [m.group(1) for m in re.finditer(r'"([^"]*)"', match.group(1))]


def load_tables(checkout, which):
    """Return {"buttons": [...], "axes": [...]} for one pinned checkout."""
    path = pathlib.Path(checkout) / TABLE_SOURCE[which]
    text = path.read_text(encoding="utf-8", errors="replace")
    return {key: _read_array(text, array) for key, array in ARRAY.items()}


class Expectation(object):
    """One physical input and the logical thing the engine owes for it."""

    def __init__(self, group, physical, output_kind, output_index, control):
        self.group = group              # the V2 control group, e.g. "A"
        self.physical = physical        # ("button", evdev_code) / ("axis", n)
                                        # / ("hat", (ordinal, mask))
        self.output_kind = output_kind  # "button" or "axis"
        self.output_index = output_index
        self.control = control          # the SDL control name in the mapping

    def __repr__(self):
        return "<%s %s -> %s %d>" % (self.group, self.physical,
                                     self.output_kind, self.output_index)


# The V2 control groups, and which SDL control name carries each one.
GROUPS = {
    "A": ["a"], "B": ["b"], "X": ["x"], "Y": ["y"],
    "L1": ["leftshoulder"], "R1": ["rightshoulder"],
    "L2": ["lefttrigger"], "R2": ["righttrigger"],
    "L3": ["leftstick"], "R3": ["rightstick"],
    "START": ["start"], "SELECT": ["back"],
    "DPAD_UP": ["dpup"], "DPAD_DOWN": ["dpdown"],
    "DPAD_LEFT": ["dpleft"], "DPAD_RIGHT": ["dpright"],
    "LEFT_STICK": ["leftx", "lefty"],
    "RIGHT_STICK": ["rightx", "righty"],
}
GUIDE_GROUP = "GUIDE"


def parse_mapping(mapping):
    """{control name: raw binding value} for the line's real bindings."""
    out = {}
    for field in mapping.split(",")[2:]:
        if ":" not in field or not field:
            continue
        key, value = field.split(":", 1)
        if key in ("platform", "hint", "crc", "type", "sdk", "hidapi"):
            continue
        out[key] = value
    return out


def sdl_button_code(ordinal, key_codes):
    """SDL2's button ordinal -> evdev code, over the pad's real key bits.

    SDL scans [BTN_JOYSTICK, KEY_MAX) first and then [0, BTN_JOYSTICK); the
    pad this harness builds only has codes in the first range, so the order
    is simply ascending. `key_codes` is what the KERNEL reported.
    """
    high = sorted(c for c in key_codes if 0x120 <= c <= 0x2FF)
    low = sorted(c for c in key_codes if c < 0x120)
    order = high + low
    if ordinal >= len(order):
        return None
    return order[ordinal]


def sdl_axis_code(ordinal, abs_codes):
    """SDL2's axis ordinal -> evdev ABS code, hats excluded."""
    order = sorted(c for c in abs_codes if not 0x10 <= c <= 0x17)
    if ordinal >= len(order):
        return None
    return order[ordinal]


def build(mapping, tables, key_codes, abs_codes):
    """Predict, for one mapping and one real pad, what the engine owes.

    Returns (expectations, unmapped_groups). A group with no binding is NOT
    an expectation: it is a group the engine must stay silent about, which
    the null case relies on.
    """
    bindings = parse_mapping(mapping)
    buttons = tables["buttons"]
    axes = tables["axes"]
    expectations = []
    covered = set()

    for group, controls in list(GROUPS.items()) + [(GUIDE_GROUP, ["guide"])]:
        for control in controls:
            value = bindings.get(control)
            if value is None:
                continue
            if control in buttons:
                kind, index = "button", buttons.index(control)
            elif control in axes:
                kind, index = "axis", axes.index(control)
            else:
                # This major has no output for that name at all; the engine
                # will print "Unrecognized output string" and bind nothing.
                continue
            raw = value
            invert = raw.endswith("~")
            if invert:
                raw = raw[:-1]
            half = ""
            if raw[:1] in "+-":
                half, raw = raw[0], raw[1:]
            if raw.startswith("b"):
                code = sdl_button_code(int(raw[1:]), key_codes)
                if code is None:
                    continue
                physical = ("button", code)
            elif raw.startswith("a"):
                code = sdl_axis_code(int(raw[1:]), abs_codes)
                if code is None:
                    continue
                physical = ("axis", code, half, invert)
            elif raw.startswith("h"):
                ordinal, mask = raw[1:].split(".")
                physical = ("hat", int(ordinal), int(mask))
            else:
                continue
            expectations.append(
                Expectation(group, physical, kind, index, control))
            covered.add(group)

    unmapped = [g for g in GROUPS if g not in covered]
    return expectations, sorted(unmapped)


if __name__ == "__main__":
    import argparse
    import json

    ap = argparse.ArgumentParser()
    ap.add_argument("--checkout", required=True)
    ap.add_argument("--which", required=True, choices=("godot3", "godot4"))
    ap.add_argument("--mapping", required=True)
    args = ap.parse_args()
    tables = load_tables(args.checkout, args.which)
    print(json.dumps(tables, indent=1))
