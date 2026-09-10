#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""INDEPENDENT reference implementation of the SDL2 mapping dialect and of
the sovereign database lookup, written from the CONTRACT in
`include/nxinput_sovereign.h` -- not from `src/nxinput_sovereign.c`.

It exists so the C3 differential proof compares the resolver under test with
something that is not the resolver under test. It shares no code, no data
structure and no file with nxinput_sovereign: given a database and a GUID it
independently decides which entry must win, with which capabilities, and what
the runtime must be holding afterwards. The gate then compares that with what
the real resolver actually pushed into the runtime consumer.

If nxinput ever swaps, drops or rewrites a binding this reference preserves,
the comparison fails. `tests/run-sovereign-corpus-host.sh` proves exactly
that by rebuilding the driver from a deliberately mutated resolver and
requiring the gate to REJECT it.
"""

import re

# The binding keys of the SDL2 game-controller dialect (header: "GUID,name,
# key:value,... with known keys").
BINDING_KEYS = frozenset((
    "a", "b", "x", "y", "back", "guide", "start",
    "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright",
    "leftx", "lefty", "rightx", "righty",
    "lefttrigger", "righttrigger",
    "misc1", "misc2", "misc3", "misc4", "misc5", "misc6",
    "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
))
# Half-axis outputs: an axis key may carry a +/- prefix.
AXIS_KEYS = frozenset(("leftx", "lefty", "rightx", "righty", "lefttrigger",
                       "righttrigger"))
# Any non-binding `key:value` field is metadata (platform, crc, hint, type,
# sdk>= and whatever a CFW invents next): validated for its key:value shape
# only, never part of the semantic binding set, never an error.

LINE_MAX = 1024
MAX_FIELDS = 64
GUID_RE = re.compile(r"^[0-9a-f]{32}$")
ORDINAL_RE = re.compile(r"^\d{1,4}$")
CAPS_RE = re.compile(r"[:+\-]([bah])(\d+)")

OK = "ok"
SYNTAX_INVALID = "syntax-invalid"
UNREACHABLE = "unreachable"
GUID_NOT_FOUND = "guid-not-found"
DUPLICATE_DIVERGENT = "duplicate-divergent"
SOURCE_EMPTY = "source-empty"


def _ordinal(text):
    if not ORDINAL_RE.match(text):
        return None
    return int(text)


def parse_value(value):
    """One binding value: bN | aN | +aN | -aN | any of those with a trailing
    '~' | hN.M. Returns (button, axis, hat) with None where unused, or None
    when the value is not of the dialect."""
    if not value:
        return None
    if value.endswith("~"):
        value = value[:-1]
        if not value:
            return None
    prefixed = value[0] in "+-"
    if prefixed:
        value = value[1:]
        if not value or value[0] != "a":
            return None
    kind, rest = value[0], value[1:]
    if kind == "b":
        if prefixed:
            return None
        number = _ordinal(rest)
        return None if number is None else (number, None, None)
    if kind == "a":
        number = _ordinal(rest)
        return None if number is None else (None, number, None)
    if kind == "h":
        if prefixed or "." not in rest:
            return None
        hat, _, mask = rest.partition(".")
        hat_number, mask_number = _ordinal(hat), _ordinal(mask)
        if hat_number is None or mask_number is None:
            return None
        return (None, None, hat_number)
    return None


def is_binding_key(key):
    if len(key) > 1 and key[0] in "+-":
        return key[1:] in AXIS_KEYS
    return key in BINDING_KEYS


def parse_line(line, caps=None):
    """Validate one mapping line and return (reason, bindings).

    `bindings` is the ordered key -> value map of EFFECTIVE bindings; the name
    and the metadata fields are excluded. A duplicated binding key resolves
    the way SDL executes it -- the LAST occurrence wins -- and a line with no
    binding at all is not a mapping.  When `caps` is given, an ordinal outside
    the measured capabilities is UNREACHABLE."""
    if line is None or not (34 <= len(line) < LINE_MAX):
        return SYNTAX_INVALID, {}
    if not GUID_RE.match(line[:32]) or line[32] != ",":
        return SYNTAX_INVALID, {}
    fields = line[33:].split(",")
    if len(fields) > MAX_FIELDS:
        return SYNTAX_INVALID, {}
    if not fields or not fields[0]:
        return SYNTAX_INVALID, {}  # the name is cosmetic but never empty
    bindings = {}
    for field in fields[1:]:
        if not field:
            continue
        if ":" not in field:
            return SYNTAX_INVALID, {}
        key, _, value = field.partition(":")
        if is_binding_key(key):
            parsed = parse_value(value)
            if parsed is None:
                return SYNTAX_INVALID, {}
            button, axis, hat = parsed
            if caps is not None:
                buttons, axes, hats = caps
                if ((button is not None and button >= buttons) or
                        (axis is not None and axis >= axes) or
                        (hat is not None and hat >= hats)):
                    return UNREACHABLE, {}
            if key not in bindings and len(bindings) >= MAX_FIELDS:
                return SYNTAX_INVALID, {}
            bindings[key] = value  # SDL semantics: the last occurrence wins
        # else: metadata -- tolerated, never semantic
    if not bindings:
        return SYNTAX_INVALID, {}  # parses, but binds nothing
    return OK, bindings


def caps_of_line(line):
    """Smallest capabilities that make every ordinal of `line` reachable."""
    buttons = axes = hats = 1
    for kind, number in CAPS_RE.findall(line):
        value = int(number) + 1
        if kind == "b":
            buttons = max(buttons, value)
        elif kind == "a":
            axes = max(axes, value)
        else:
            hats = max(hats, value)
    return buttons, axes, hats


def _trim(line):
    # The store trims only trailing CR and spaces, byte for byte.
    while line and line[-1] in "\r ":
        line = line[:-1]
    return line


def db_lookup(database, guid, skip_header=False):
    """Exact-GUID lookup. A store legally carries the same GUID more than
    once; the runtime that executes the mapping is SDL, whose AddMapping
    REPLACES an existing entry, so the LAST line wins -- exactly that, never
    a refusal. The first line is NEVER chosen on a GUID mismatch."""
    if not database:
        return SOURCE_EMPTY, None
    found = None
    for index, raw in enumerate(database.split("\n")):
        if (skip_header and index == 0) or not raw or raw.startswith("#"):
            continue
        if len(raw) > 33 and raw[:32] == guid and raw[32] == ",":
            found = _trim(raw)
    if found is None:
        return GUID_NOT_FOUND, None
    return OK, found


def expected_db_resolution(database, guid, caps):
    """What the sovereign order must produce when the CFW database is the only
    source: (source, reason, line_the_runtime_must_hold, bindings)."""
    reason, line = db_lookup(database, guid)
    if reason != OK:
        return "fail-explicit", reason, "", {}
    syntax, bindings = parse_line(line)
    if syntax != OK:
        return "fail-explicit", syntax, "", {}
    reach, bindings = parse_line(line, caps)
    if reach != OK:
        return "fail-explicit", reach, "", {}
    return "cfw-db-guid", OK, line, bindings
