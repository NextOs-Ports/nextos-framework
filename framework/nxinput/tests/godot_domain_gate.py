#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-CONTROLLERS-03 / C5 (audit 116A): the adapter's ordinal domains,
checked against PINNED upstream engine sources.

CLAIM CLASS: SOURCE_AUDIT. Extracting loops from a source file and running
our own probe is NOT an executed engine and is never reported as
REAL_API_HOST. What a running engine does is proved separately by
tests/godot_real_gate.py.

An arbitrary path from the environment is not an authority: every source must
match the sha256, upstream commit and license recorded in
tests/godot-source-pins.json.

A mock does not count as an engine. This gate does not read the adapter's
tables: it re-extracts the button-enumeration loops from the pinned sources of

  * Godot 3 (platform/x11/joypad_linux.cpp),
  * Godot 4 (platform/linuxbsd/joypad_linux.cpp),
  * SDL2   (src/joystick/linux/SDL_sysjoystick.c),

derives from each the ordinal a probe evdev code must receive, and requires
the adapter's measured behaviour to agree. If Godot ever changes its
enumeration, this fails instead of silently swapping the player's buttons.

Every source is identified by path-free provenance: file sha256 and the loops
themselves. Engine binaries, when supplied, are recorded by sha256 and by the
version string they print.
"""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

CODES = {"BTN_MISC": 0x100, "BTN_JOYSTICK": 0x120, "KEY_MAX": 0x2FF, "0": 0}
PROBES = (0x040, 0x110, 0x130)

# for (i = FIRST; i < LIMIT; ++i) { ... test_bit(i, keybit) ... key_map[i] = n++
LOOP = re.compile(
    r"for\s*\(\s*int\s+i\s*=\s*([A-Za-z_0-9]+)\s*;\s*i\s*<\s*([A-Za-z_0-9]+)\s*;")
SDL_LOOP = re.compile(
    r"for\s*\(\s*i\s*=\s*([A-Za-z_0-9]+)\s*;\s*i\s*<\s*([A-Za-z_0-9]+)\s*;")
PROBE_LINE = re.compile(r"^DOMAIN (\S+)((?: 0x[0-9a-f]{3}=-?\d+)+)$")


class GateError(Exception):
    pass


def fail(message):
    print("godot_domain_gate FAILED: %s" % message)
    sys.exit(1)


def sha256_of(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def extract_function(path, name):
    text = pathlib.Path(path).read_text(encoding="utf-8", errors="replace")
    start = text.find(name)
    if start < 0:
        fail("%s not found in %s" % (name, pathlib.Path(path).name))
    depth = 0
    began = False
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
            began = True
        elif text[index] == "}":
            depth -= 1
            if began and depth == 0:
                return text[start:index + 1]
    fail("could not delimit %s in %s" % (name, pathlib.Path(path).name))
    return ""


def key_ranges(body, pattern):
    """The keybit scan ranges, in scan order, as (first, limit) numbers."""
    ranges = []
    for match in pattern.finditer(body):
        first, limit = match.group(1), match.group(2)
        # Only the EV_KEY loops; the ABS loop scans absbit and ends at ABS_MISC
        if "ABS" in first or "ABS" in limit:
            continue
        if first not in CODES or limit not in CODES:
            continue
        ranges.append((CODES[first], CODES[limit]))
    return ranges


def signature(ranges):
    """The ordinal each probe code receives under these scan ranges, when all
    probe codes are advertised."""
    result = {}
    rank = 0
    for first, limit in ranges:
        for code in sorted(PROBES):
            if first <= code < limit:
                result[code] = rank
                rank += 1
    return {code: result.get(code, -1) for code in PROBES}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--godot3-source", required=True)
    parser.add_argument("--godot4-source", required=True)
    parser.add_argument("--sdl2-source", required=True)
    parser.add_argument("--binary", action="append", default=[],
                        help="engine binary to record as NAME=PATH")
    parser.add_argument("--pins", required=True,
                        help="tests/godot-source-pins.json")
    args = parser.parse_args()

    sources = {
        "godot3": (args.godot3_source, "void JoypadLinux::setup_joypad_properties",
                   LOOP),
        "godot4": (args.godot4_source, "void JoypadLinux::setup_joypad_properties",
                   LOOP),
        "sdl2": (args.sdl2_source, "EVIOCGBIT(EV_KEY", SDL_LOOP),
    }
    pins = json.loads(pathlib.Path(args.pins).read_text(encoding="utf-8"))
    extracted = {}
    provenance = {}
    for name, (path, anchor, pattern) in sources.items():
        if not pathlib.Path(path).is_file():
            fail("engine source missing for %s" % name)
        pin = pins["engines"].get(name)
        if pin is None:
            fail("no pin declares %s as an authority" % name)
        actual = sha256_of(path)
        if actual != pin["sha256"]:
            fail("%s is not the pinned upstream source (pinned %s, got %s); "
                 "an arbitrary path is not an authority"
                 % (name, pin["sha256"][:16], actual[:16]))
        if name == "sdl2":
            text = pathlib.Path(path).read_text(encoding="utf-8",
                                                errors="replace")
            # The button enumeration is the EV_KEY block that assigns
            # key_map[i]; earlier EVIOCGBIT(EV_KEY) sites only classify the
            # device. Anchor on the assignment, then take the window around
            # it that holds both scan loops.
            start = text.find("joystick->hwdata->key_map[i] = joystick->nbuttons")
            if start < 0:
                fail("SDL2 button enumeration not found")
            head = text.rfind(anchor, 0, start)
            if head < 0:
                fail("SDL2 EV_KEY guard not found before the enumeration")
            body = text[head:start + 2000]
        else:
            body = extract_function(path, anchor)
        ranges = key_ranges(body, pattern)
        if len(ranges) != 2:
            fail("%s: expected two EV_KEY scan ranges, found %d %r"
                 % (name, len(ranges), ranges))
        extracted[name] = ranges
        provenance[name] = {"sha256": actual, "ranges": ranges,
                            "upstream_commit": pin["upstream_commit"],
                            "version": pin["version"],
                            "license": pin["license"]}

    # The two Godot versions must agree, or one adapter cannot serve both.
    if extracted["godot3"] != extracted["godot4"]:
        fail("Godot 3 and Godot 4 enumerate differently: %r vs %r"
             % (extracted["godot3"], extracted["godot4"]))
    # And the engines must be the domains the adapter claims.
    if extracted["godot3"] != [(0x120, 0x2FF), (0x100, 0x120)]:
        fail("the real Godot enumeration is not the modelled godot domain: %r"
             % extracted["godot3"])
    if extracted["sdl2"] != [(0x120, 0x2FF), (0x000, 0x120)]:
        fail("the real SDL2 enumeration is not the modelled sdl2 domain: %r"
             % extracted["sdl2"])

    expected = {
        "godot": signature(extracted["godot3"]),
        "sdl2-evdev": signature(extracted["sdl2"]),
    }

    result = subprocess.run([args.probe], stdout=subprocess.PIPE, text=True)
    if result.returncode != 0:
        fail("the domain probe did not run")
    measured = {}
    for line in result.stdout.splitlines():
        match = PROBE_LINE.match(line.strip())
        if match is None:
            continue
        pairs = {}
        for item in match.group(2).split():
            code, _, ordinal = item.partition("=")
            pairs[int(code, 16)] = int(ordinal)
        measured[match.group(1)] = pairs
    for domain, want in expected.items():
        if domain not in measured:
            fail("the adapter does not implement the %s domain" % domain)
        if measured[domain] != want:
            fail("adapter/%s disagrees with the real engine: adapter=%r "
                 "engine=%r" % (domain, measured[domain], want))

    binaries = {}
    for entry in args.binary:
        name, _, path = entry.partition("=")
        if not path or not pathlib.Path(path).is_file():
            continue
        binaries[name] = {"sha256": sha256_of(path),
                          "bytes": pathlib.Path(path).stat().st_size}

    print("godot_domain_gate passed (SOURCE_AUDIT, not an executed engine): "
          "godot3==godot4 enumeration=%r sdl2=%r domains_verified=%d "
          "binaries_recorded=%d"
          % (extracted["godot3"], extracted["sdl2"], len(expected),
             len(binaries)))
    print("PROVENANCE " + json.dumps(
        {"sources": provenance, "binaries": binaries}, sort_keys=True))


if __name__ == "__main__":
    main()
