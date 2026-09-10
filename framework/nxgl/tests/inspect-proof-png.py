#!/usr/bin/env python3
"""Inspect nxgl's real pre-present PNG without Pillow or a display server."""

import binascii
import struct
import sys
import zlib
from pathlib import Path


def fail(message):
    raise SystemExit("proof PNG inspection FAIL: " + message)


if len(sys.argv) != 3 or sys.argv[2] not in ("non-black", "black"):
    fail("usage: inspect-proof-png.py IMAGE non-black|black")

payload = Path(sys.argv[1]).read_bytes()
if payload[:8] != b"\x89PNG\r\n\x1a\n":
    fail("bad signature")

offset = 8
width = height = None
compressed = bytearray()
seen_end = False
while offset < len(payload):
    if offset + 12 > len(payload):
        fail("truncated chunk")
    length = struct.unpack(">I", payload[offset:offset + 4])[0]
    kind = payload[offset + 4:offset + 8]
    end = offset + 12 + length
    if end > len(payload):
        fail("chunk exceeds file")
    data = payload[offset + 8:offset + 8 + length]
    expected_crc = struct.unpack(">I", payload[offset + 8 + length:end])[0]
    actual_crc = binascii.crc32(kind + data) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        fail("chunk CRC mismatch")
    if kind == b"IHDR":
        if length != 13:
            fail("bad IHDR size")
        width, height, depth, colour, compression, filtering, interlace = \
            struct.unpack(">IIBBBBB", data)
        if (depth, colour, compression, filtering, interlace) != (8, 6, 0, 0, 0):
            fail("unexpected PNG format")
    elif kind == b"IDAT":
        compressed.extend(data)
    elif kind == b"IEND":
        seen_end = True
        if end != len(payload):
            fail("bytes after IEND")
    offset = end

if not seen_end or width is None or height is None or not compressed:
    fail("missing required chunks")
if (width, height) != (640, 480):
    fail("unexpected dimensions {}x{}".format(width, height))

raw = zlib.decompress(bytes(compressed))
row_size = 1 + width * 4
if len(raw) != row_size * height:
    fail("unexpected decompressed size")
rgb_coloured = 0
visible = 0
alpha_zero = 0
for y in range(height):
    row = raw[y * row_size:(y + 1) * row_size]
    if row[0] != 0:
        fail("unexpected PNG row filter")
    pixels = row[1:]
    for x in range(0, len(pixels), 4):
        rgb = bool(pixels[x] or pixels[x + 1] or pixels[x + 2])
        rgb_coloured += int(rgb)
        visible += int(rgb and pixels[x + 3] != 0)
        alpha_zero += int(pixels[x + 3] == 0)

ratio = visible * 100.0 / (width * height)
if sys.argv[2] == "non-black" and ratio < 0.5:
    fail("expected visible pixels, measured {:.1f}%".format(ratio))
if sys.argv[2] == "black" and visible != 0:
    fail("expected black capture, measured {:.1f}% visible".format(ratio))
print("proof PNG inspection: PASS expected={} visible={:.1f}% rgb={:.1f}% alpha0={:.1f}%".format(
    sys.argv[2], ratio, rgb_coloured * 100.0 / (width * height),
    alpha_zero * 100.0 / (width * height)
))
