#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxoracle_v5 -- the INDEPENDENT input oracle model (V5, mission 8).

The expected result of a stimulus NEVER comes from the mapping under test.
It comes from:
  * a physical profile   (position -> EV_KEY, trust root: vendor DTS/es_input,
                          or a certified physical capture);
  * a provider descriptor (DSO sha256 -> ordinal table, trust root: pinned
                          source/patch + probe), see tests/providers;
  * the capability bits of the exact event node.

Given those, the oracle derives:
  stimulus(control)       = the EV_KEY/EV_ABS to inject for a POSITION
  provider_ordinal(code)  = what the loaded provider numbers that code
and judges what the port DELIVERED against the position that was pressed.

A mapping that swaps A/B, a rewrite into the wrong provider order, or a
DSO other than the mapped one can no longer "teach" the test which wrong
button to press: the press is chosen from the physical table alone.
"""
import json, hashlib

KEY_MAX = 0x2FF
BTN_JOYSTICK = 0x120
ABS_HAT0X, ABS_HAT3Y, ABS_MAX, ABS_MISC = 0x10, 0x17, 0x3F, 0x28

DOMAINS = {
    # name: (button sweeps, axis limit)
    "sdl2-evdev": ([(BTN_JOYSTICK, KEY_MAX), (0, BTN_JOYSTICK)], ABS_MAX),
    "sdl3-evdev": ([(BTN_JOYSTICK, KEY_MAX), (0, BTN_JOYSTICK)], ABS_MAX),
    "sdl2-ascending-patched": ([(0, KEY_MAX)], ABS_MISC),
    "sdl2-legacy-evdev": ([(BTN_JOYSTICK, KEY_MAX), (0, BTN_JOYSTICK)], ABS_MAX),
}


def button_table(domain, key_codes):
    """ordinal -> EV_KEY for a provider domain, from capability bits alone."""
    sweeps, _ = DOMAINS[domain]
    codes = set(key_codes)
    ordered = []
    for lo, hi in sweeps:
        ordered += [c for c in range(lo, hi) if c in codes]
    return {i: c for i, c in enumerate(ordered)}


def axis_table(domain, abs_codes):
    _, limit = DOMAINS[domain]
    codes = set(abs_codes)
    hats = [h for h in range(ABS_HAT0X, ABS_HAT3Y, 2) if h in codes and h + 1 in codes]
    skip = set()
    for h in hats:
        skip.update((h, h + 1))
    ordered = [c for c in range(0, limit) if c in codes and c not in skip]
    return {i: c for i, c in enumerate(ordered)}, {i: h for i, h in enumerate(hats)}


def parse_mapping(line):
    parts = line.strip().rstrip(",").split(",")
    fields = {}
    for kv in parts[2:]:
        if ":" in kv:
            k, v = kv.split(":", 1)
            fields[k] = v
    return parts[0], parts[1], fields


class Oracle:
    def __init__(self, physical_profile, provider_domain, key_codes, abs_codes):
        self.phys = {k: int(v, 16) if isinstance(v, str) else v for k, v in physical_profile.items()}
        self.domain = provider_domain
        self.btable = button_table(provider_domain, key_codes)
        self.rev = {c: i for i, c in self.btable.items()}

    def stimulus(self, position):
        """EV_KEY for a POSITION token (face.south ...). Independent of any mapping."""
        return self.phys[position]

    def provider_ordinal(self, code):
        return self.rev.get(code)

    def semantic_delivered(self, mapping_line, code):
        """What the game's SDL controller layer would call the EV_KEY `code`
        under the loaded provider with `mapping_line` installed."""
        ordinal = self.provider_ordinal(code)
        if ordinal is None:
            return None
        _, _, fields = parse_mapping(mapping_line)
        for sem, target in fields.items():
            if target == "b%d" % ordinal:
                return sem
        return None

    def judge(self, position, mapping_line, expected_semantic):
        code = self.stimulus(position)
        got = self.semantic_delivered(mapping_line, code)
        return got == expected_semantic, {"position": position, "ev_key": hex(code),
                                          "provider_ordinal": self.provider_ordinal(code),
                                          "delivered": got, "expected": expected_semantic}


def content_address(obj):
    return hashlib.sha256(json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
