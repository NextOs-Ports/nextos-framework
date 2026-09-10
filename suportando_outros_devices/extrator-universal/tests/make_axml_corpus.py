#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deterministic generator for the AXML/APK adversarial corpus.

V3-HARDENING-01: binary fixtures are hard to review, so the corpus is not
checked in as opaque blobs -- this script IS the reviewable artifact.  It
writes the exact same bytes on every run (fixed strings, fixed ZIP
timestamps, no randomness, no environment input), so the replay gate
(test_corpus_replay.py) regenerates the corpus into a tempdir and replays it
through the real nxextract parsers.

Layout under the output directory (argv[1], default: tests/corpus):

  manifest/ok-*.xml|.axml   -> parse_android_manifest must ACCEPT
  manifest/bad-*.axml       -> must REJECT with a controlled error
  zip/ok-*.zip              -> zip_classification must say "apk"
  zip/bad-*.zip             -> must never classify "apk", never crash/hang
"""
import io
import os
import struct
import sys
import zipfile

ZIP_EPOCH = (1980, 1, 1, 0, 0, 0)  # fixed date_time => byte-stable zips


# --------------------------------------------------------------- manifests

def plain_manifest(package, split=""):
    split_attribute = ' split="%s"' % split if split else ""
    return (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<manifest package="%s"%s></manifest>' % (package, split_attribute)
    ).encode("utf-8")


def _utf8_string(value):
    encoded = value.encode("utf-8")
    assert len(value) < 128 and len(encoded) < 128
    return bytes((len(value), len(encoded))) + encoded + b"\0"


def _string_pool_utf8(strings):
    payload = b""
    offsets = []
    for value in strings:
        offsets.append(len(payload))
        payload += _utf8_string(value)
    header_size = 28
    strings_start = header_size + len(strings) * 4
    pool_size = strings_start + len(payload)
    pool = struct.pack(
        "<HHIIIIII",
        0x0001,          # RES_STRING_POOL_TYPE
        header_size,
        pool_size,
        len(strings),
        0,               # style count
        0x100,           # UTF8_FLAG
        strings_start,
        0,               # styles start
    )
    return pool + struct.pack("<%dI" % len(offsets), *offsets) + payload


def binary_manifest(package, split=""):
    """Well-formed binary AXML carrying <manifest package= split=>."""
    pool = _string_pool_utf8(["manifest", "package", "split", package, split])
    attributes = []
    for name_index, value_index in ((1, 3), (2, 4)):
        attributes.append(
            struct.pack(
                "<IIIHBBI",
                0xFFFFFFFF,   # namespace
                name_index,
                value_index,  # raw value
                8,            # value size
                0,            # res0
                0x03,         # TYPE_STRING
                value_index,
            )
        )
    node_size = 16 + 20 + sum(len(value) for value in attributes)
    node = struct.pack("<HHIII", 0x0102, 16, node_size, 1, 0xFFFFFFFF)
    node += struct.pack("<IIHHHHHH", 0xFFFFFFFF, 0, 20, 20, len(attributes), 0, 0, 0)
    node += b"".join(attributes)
    total = 8 + len(pool) + len(node)
    return struct.pack("<HHI", 0x0003, 8, total) + pool + node


def truncated_axml():
    """Valid document cut mid string pool (document size lies about it)."""
    good = binary_manifest("com.nextos.corpus")
    # keep the AXML doc header + the pool chunk header, drop the pool body
    return good[: 8 + 28]


def stringpool_count_overflow():
    """String pool whose count field claims ~2 billion entries."""
    good = bytearray(binary_manifest("com.nextos.corpus"))
    # pool chunk starts at offset 8; its count field lives at pool+8
    struct.pack_into("<I", good, 8 + 8, 0x7FFFFFFF)
    return bytes(good)


def offset_loop_axml():
    """Chunk whose declared size is 0: a naive walker never advances."""
    pool = _string_pool_utf8(["manifest"])
    loop_chunk = struct.pack("<HHI", 0x0102, 8, 0)  # chunk_size == 0
    total = 8 + len(pool) + len(loop_chunk) + 64
    return struct.pack("<HHI", 0x0003, 8, total) + pool + loop_chunk + b"\0" * 64


def utf16_oob_axml():
    """UTF-16 string pool whose char count runs past the chunk end."""
    header_size = 28
    strings_start = header_size + 4          # one offset entry
    payload = struct.pack("<H", 0x00FF) + b"AB"  # claims 255 chars, has 1
    pool_size = strings_start + len(payload)
    pool = struct.pack(
        "<HHIIIIII", 0x0001, header_size, pool_size, 1, 0, 0, strings_start, 0
    )
    pool += struct.pack("<I", 0) + payload
    total = 8 + len(pool)
    return struct.pack("<HHI", 0x0003, 8, total) + pool


# -------------------------------------------------------------------- zips

def zip_bytes(entries):
    """Byte-stable in-memory zip: fixed order, fixed timestamps."""
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=ZIP_EPOCH)
            info.external_attr = 0o644 << 16
            archive.writestr(info, data)
    return stream.getvalue()


def ok_apk_min():
    return zip_bytes(
        [
            ("AndroidManifest.xml", plain_manifest("com.nextos.corpus")),
            ("classes.dex", b"dex\n035\0corpus"),
        ]
    )


def eocd_truncated_zip():
    return ok_apk_min()[:-12]  # chops through the end-of-central-directory


def nested_bomb_zip():
    """Zip whose .apk member nests three levels of further .apk zips."""
    level = zip_bytes([("AndroidManifest.xml", plain_manifest("com.nextos.deep"))])
    for depth in range(3):
        level = zip_bytes([("nested-%d.apk" % depth, level)])
    return level


def member_count_zip():
    return zip_bytes(
        [("members/%04d.bin" % index, b"x") for index in range(2000)]
    )


def member_traversal_zip():
    return zip_bytes(
        [
            ("ok.txt", b"fine"),
            ("../../../evil.txt", b"escape attempt"),
        ]
    )


# ------------------------------------------------------------------ driver

CORPUS = {
    "manifest/ok-plain-manifest.xml": lambda: plain_manifest("com.nextos.corpus"),
    "manifest/ok-binary-manifest.axml": lambda: binary_manifest(
        "com.nextos.corpus", "config.arm64_v8a"
    ),
    "manifest/bad-truncated-axml.axml": truncated_axml,
    "manifest/bad-stringpool-count-overflow.axml": stringpool_count_overflow,
    "manifest/bad-offset-loop.axml": offset_loop_axml,
    "manifest/bad-utf16-oob.axml": utf16_oob_axml,
    "manifest/bad-empty.axml": lambda: b"",
    "zip/ok-apk-min.zip": ok_apk_min,
    "zip/bad-zip-eocd-truncated.zip": eocd_truncated_zip,
    "zip/bad-nested-bomb.zip": nested_bomb_zip,
    "zip/bad-member-count.zip": member_count_zip,
    "zip/bad-member-traversal.zip": member_traversal_zip,
}


def write_corpus(output_dir):
    written = []
    for relative in sorted(CORPUS):
        path = os.path.join(output_dir, *relative.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(CORPUS[relative]())
        written.append(path)
    return written


def main(argv):
    default = os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus")
    output_dir = argv[1] if len(argv) > 1 else default
    written = write_corpus(output_dir)
    for path in written:
        print(path)
    print("corpus files: %d" % len(written))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
