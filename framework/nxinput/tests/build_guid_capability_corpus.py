#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the GUID -> capabilities oracle the C5B positive control consumes.

WHAT THE 116A AUDIT REFUSED
---------------------------
The old positive control invented two generic profiles, reused them for every
GUID and then multiplied the row count by profile and by engine. It could not
fail a single line, because the capability it compared against had been chosen
to fit.

WHAT THIS DOES INSTEAD
----------------------
Capabilities are never read out of a mapping. Each selected GUID is assigned
ONE hardware SHAPE from a declared catalogue, that shape is materialised as a
real kernel device through /dev/uinput with the GUID's own bus/vendor/product/
version, and the capability finally recorded is what the KERNEL answered on
the resulting /dev/input/eventN -- EVIOCGBIT for EV_KEY and EV_ABS, EVIOCGABS
for every axis range. The mapping's bytes take no part in that.

Because the shapes differ, the oracle can and does REFUSE official lines: a
line that binds a hat cannot be satisfied on a shape with no hat, and a line
that binds `a5` cannot be satisfied on a shape with four axes. That is the
point. An oracle that passes everything proves nothing.

CLAIM CLASS: FIXTURE_HOST. A uinput node is never physical proof.
"""
import argparse
import array
import ctypes
import fcntl
import glob
import hashlib
import json
import os
import pathlib
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vpad import VirtualPad  # noqa: E402

EV_KEY, EV_ABS = 0x01, 0x03
ABS_MAX = 0x40
KEY_MAX = 0x2FF


def EVIOCGBIT(ev, length):
    return (2 << 30) | (ord("E") << 8) | (0x20 + ev) | (length << 16)


def EVIOCGABS(code):
    return (2 << 30) | (ord("E") << 8) | (0x40 + code) | (24 << 16)


def UI_GET_SYSNAME(length):
    return (2 << 30) | (ord("U") << 8) | 44 | (length << 16)


BTN = dict(A=0x130, B=0x131, C=0x132, X=0x133, Y=0x134, Z=0x135,
           TL=0x136, TR=0x137, TL2=0x138, TR2=0x139,
           SELECT=0x13A, START=0x13B, MODE=0x13C,
           THUMBL=0x13D, THUMBR=0x13E,
           DPAD_UP=0x220, DPAD_DOWN=0x221, DPAD_LEFT=0x222, DPAD_RIGHT=0x223)

STICK = (-32768, 32767, 128)
TRIGGER = (0, 255, 0)
HAT = (-1, 1, 0)

# The declared catalogue of hardware SHAPES. Each entry describes a real
# family of pads; none of them is derived from any mapping line.
SHAPES = {
    "dual-analog-hat": {
        "why": "the common USB pad: two sticks, analog triggers, one hat",
        "keys": ["A", "B", "X", "Y", "TL", "TR", "SELECT", "START", "MODE",
                 "THUMBL", "THUMBR"],
        "axes": {0: STICK, 1: STICK, 2: TRIGGER, 3: STICK, 4: STICK,
                 5: TRIGGER, 16: HAT, 17: HAT},
    },
    "dual-analog-dpad-buttons": {
        "why": "same, but the D-pad is four keys and there is no hat",
        "keys": ["A", "B", "X", "Y", "TL", "TR", "SELECT", "START", "MODE",
                 "THUMBL", "THUMBR", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT",
                 "DPAD_RIGHT"],
        "axes": {0: STICK, 1: STICK, 2: TRIGGER, 3: STICK, 4: STICK,
                 5: TRIGGER},
    },
    "dual-analog-digital-triggers": {
        "why": "two sticks, a hat, and triggers that are KEYS not axes",
        "keys": ["A", "B", "X", "Y", "TL", "TR", "TL2", "TR2", "SELECT",
                 "START", "THUMBL", "THUMBR"],
        "axes": {0: STICK, 1: STICK, 3: STICK, 4: STICK, 16: HAT, 17: HAT},
    },
    "single-analog-hat": {
        "why": "one stick and a hat: handhelds and older pads",
        "keys": ["A", "B", "X", "Y", "TL", "TR", "SELECT", "START"],
        "axes": {0: STICK, 1: STICK, 16: HAT, 17: HAT},
    },
    "digital-only-hat": {
        "why": "no stick at all: a hat and eight keys",
        "keys": ["A", "B", "X", "Y", "TL", "TR", "SELECT", "START"],
        "axes": {16: HAT, 17: HAT},
    },
    "full-analog-hat-and-dpad": {
        "why": "a rich pad: two sticks, analog triggers, a hat AND four "
               "D-pad keys, the shape most gamecontrollerdb lines assume",
        "keys": ["A", "B", "C", "X", "Y", "Z", "TL", "TR", "TL2", "TR2",
                 "SELECT", "START", "MODE", "THUMBL", "THUMBR", "DPAD_UP",
                 "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"],
        "axes": {0: STICK, 1: STICK, 2: TRIGGER, 3: STICK, 4: STICK,
                 5: TRIGGER, 16: HAT, 17: HAT},
    },
    "full-analog-no-hat": {
        "why": "the same rich pad without a hat: the D-pad is keys only",
        "keys": ["A", "B", "C", "X", "Y", "Z", "TL", "TR", "TL2", "TR2",
                 "SELECT", "START", "MODE", "THUMBL", "THUMBR", "DPAD_UP",
                 "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"],
        "axes": {0: STICK, 1: STICK, 2: TRIGGER, 3: STICK, 4: STICK,
                 5: TRIGGER},
    },
    "digital-only-dpad-buttons": {
        "why": "no stick and no hat: the D-pad is four keys",
        "keys": ["A", "B", "X", "Y", "TL", "TR", "SELECT", "START",
                 "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"],
        "axes": {},
    },
}

SHAPE_ORDER = sorted(SHAPES)

NINTENDO = re.compile(r"nintendo|switch|joy-?con|wii|gamecube|\bn64\b",
                      re.IGNORECASE)


def parse_bundle(path):
    """Return [(guid, name, line_bytes)] for every mapping in the bundle."""
    rows = []
    raw = pathlib.Path(path).read_bytes()
    for chunk in raw.split(b"\n"):
        if not chunk or chunk.startswith(b"#") or chunk.startswith(b"NXCONT"):
            continue
        fields = chunk.split(b",")
        if len(fields) < 3 or len(fields[0]) != 32:
            continue
        rows.append((fields[0].decode(), fields[1].decode("utf-8", "replace"),
                     chunk))
    return rows


def guid_hw(guid):
    """bus/vendor/product/version out of an SDL2 GUID, little-endian pairs."""
    def le16(off):
        return int(guid[off + 2:off + 4] + guid[off:off + 2], 16)
    return le16(0), le16(8), le16(16), le16(24)


def read_back(node):
    """What the KERNEL says this node is. Nothing here is our request."""
    fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
    try:
        kb = array.array("B", [0] * ((KEY_MAX + 8) // 8))
        fcntl.ioctl(fd, EVIOCGBIT(EV_KEY, len(kb)), kb, True)
        ab = array.array("B", [0] * ((ABS_MAX + 8) // 8))
        fcntl.ioctl(fd, EVIOCGBIT(EV_ABS, len(ab)), ab, True)
        keys = [c for c in range(KEY_MAX + 1) if kb[c // 8] & (1 << (c % 8))]
        axes = [c for c in range(ABS_MAX) if ab[c // 8] & (1 << (c % 8))]
        absinfo = {}
        for code in axes:
            buf = array.array("i", [0] * 6)
            fcntl.ioctl(fd, EVIOCGABS(code), buf, True)
            absinfo[str(code)] = {
                "value": buf[0], "minimum": buf[1], "maximum": buf[2],
                "fuzz": buf[3], "flat": buf[4], "resolution": buf[5]}
        return keys, axes, absinfo
    finally:
        os.close(fd)


def materialise(name, shape, hw):
    bus, vendor, product, version = hw
    keys = [BTN[k] for k in SHAPES[shape]["keys"]]
    axes = SHAPES[shape]["axes"]
    before = set(glob.glob("/dev/input/event*"))
    pad = VirtualPad(name[:79], keys=keys, axes=axes, vendor=vendor,
                     product=product, version=version,
                     bustype=bus if bus else 3)
    try:
        buf = array.array("B", [0] * 64)
        fcntl.ioctl(pad.fd, UI_GET_SYSNAME(64), buf, True)
        sysname = bytes(buf).split(b"\0")[0].decode()
        node = None
        for _ in range(40):
            fresh = sorted(set(glob.glob("/dev/input/event*")) - before)
            for candidate in fresh:
                link = "/sys/class/input/%s/device" % os.path.basename(
                    candidate)
                if os.path.realpath(link).endswith(sysname):
                    node = candidate
                    break
            if node:
                break
            time.sleep(0.1)
        if node is None:
            raise RuntimeError("the kernel node for %s never appeared"
                               % sysname)
        return node, read_back(node)
    finally:
        pad.close()
        time.sleep(0.15)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundle", required=True)
    ap.add_argument("--bundle-sha256", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--swapped", type=int, default=8,
                    help="how many a:b1,b:b0 lines to include")
    ap.add_argument("--nintendo", type=int, default=8)
    ap.add_argument("--other", type=int, default=8)
    args = ap.parse_args()

    digest = hashlib.sha256(pathlib.Path(args.bundle).read_bytes()).hexdigest()
    if digest != args.bundle_sha256:
        print("corpus build FAILED: the bundle is not the sealed one (%s)"
              % digest)
        return 1

    rows = parse_bundle(args.bundle)
    swapped = [r for r in rows if b"a:b1,b:b0" in r[2]]
    nintendo = [r for r in rows if NINTENDO.search(r[1])]
    picked, seen = [], set()

    def take(source, limit, reason):
        taken = 0
        for guid, name, line in source:
            if guid in seen or taken >= limit:
                continue
            seen.add(guid)
            picked.append((guid, name, line, reason))
            taken += 1

    take(swapped, args.swapped, "official a:b1,b:b0")
    take(nintendo, args.nintendo, "Nintendo layout")
    take(rows, args.other, "ordinary line")

    records = []
    for index, (guid, name, line, reason) in enumerate(picked):
        shape = SHAPE_ORDER[index % len(SHAPE_ORDER)]
        hw = guid_hw(guid)
        node, (keys, axes, absinfo) = materialise(name, shape, hw)
        records.append({
            "guid": guid,
            "name": name,
            "selected_because": reason,
            "shape": shape,
            "shape_why": SHAPES[shape]["why"],
            "shape_assignment":
                "declared catalogue, index %d of %d in sorted shape order; "
                "the mapping's bytes take no part in it"
                % (index % len(SHAPE_ORDER), len(SHAPE_ORDER)),
            "bustype": hw[0], "vendor": hw[1], "product": hw[2],
            "version": hw[3],
            "official_line_sha256": hashlib.sha256(line).hexdigest(),
            "official_line_bytes": len(line),
            "capabilities": {
                "evdev_node_kind": "uinput-materialised, read back from the "
                                   "kernel node the driver created",
                "kernel_node": os.path.basename(node),
                "ev_key": keys,
                "ev_abs": axes,
                "absinfo": absinfo,
            },
        })
        print("record %2d/%2d %s %-44s %s"
              % (index + 1, len(picked), guid, name[:44], shape))

    out = {
        "schema": "org.nextos.c5b.guid-capabilities",
        "schema_version": 2,
        "source_bundle_sha256": digest,
        "note": ("GUID -> capabilities for the C5B positive control. The "
                 "capability of a GUID is the answer the KERNEL gave on the "
                 "evdev node it created for a device carrying that GUID's own "
                 "bus/vendor/product/version, shaped by a declared hardware "
                 "catalogue. It is never derived from the mapping. Shapes "
                 "differ on purpose, so an official line CAN be refused."),
        "shape_catalogue": {k: SHAPES[k]["why"] for k in SHAPE_ORDER},
        "records": records,
    }
    pathlib.Path(args.out).write_text(
        json.dumps(out, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print("wrote %s with %d records over %d shapes"
          % (args.out, len(records), len(SHAPE_ORDER)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
