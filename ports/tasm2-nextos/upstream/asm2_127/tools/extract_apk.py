#!/usr/bin/env python3
"""Extract essential files from the user-supplied 1.2.7d APK.

The APK has several central-directory offsets shifted by one byte, which makes
common ZIP tools reject the ARM libraries. This extractor locates each physical
local header by its own filename, decompresses the payload, and validates size
and CRC without rewriting or resigning the APK.
"""

from __future__ import annotations

import argparse
import binascii
import os
from pathlib import Path, PurePosixPath
import struct
import sys
import zipfile
import zlib


LOCAL_MAGIC = b"PK\x03\x04"
LOCAL_HEADER = struct.Struct("<IHHHHHIIIHH")


def physical_headers(blob: bytes) -> dict[str, int]:
    result: dict[str, int] = {}
    offset = 0
    while True:
        offset = blob.find(LOCAL_MAGIC, offset)
        if offset < 0:
            break
        if offset + LOCAL_HEADER.size <= len(blob):
            fields = LOCAL_HEADER.unpack_from(blob, offset)
            name_length, extra_length = fields[-2:]
            name_start = offset + LOCAL_HEADER.size
            name_end = name_start + name_length
            extra_end = name_end + extra_length
            if extra_end <= len(blob):
                try:
                    name = blob[name_start:name_end].decode("utf-8")
                except UnicodeDecodeError:
                    name = ""
                if name:
                    result.setdefault(name, offset)
        offset += len(LOCAL_MAGIC)
    return result


def safe_output(root: Path, archive_name: str) -> Path:
    rel = PurePosixPath(archive_name)
    if rel.is_absolute() or ".." in rel.parts:
        raise ValueError(f"unsafe archive path: {archive_name!r}")
    return root.joinpath(*rel.parts)


def repair_single_missing_deflate_byte(
    compressed: bytes, expected_size: int, expected_crc: int
) -> tuple[bytes, int, int]:
    """Recover a raw DEFLATE stream with one deliberately omitted byte.

    The search is bounded to 512 bytes before zlib's first parser error. A
    candidate is accepted only if both the ZIP uncompressed size and CRC match.
    """

    probe = zlib.decompressobj(-zlib.MAX_WBITS)
    safe = 0
    while safe + 65536 <= len(compressed):
        try:
            probe.decompress(compressed[safe : safe + 65536])
        except zlib.error:
            break
        safe += 65536

    probe = zlib.decompressobj(-zlib.MAX_WBITS)
    probe.decompress(compressed[:safe])
    first_error = None
    for position in range(safe, len(compressed)):
        try:
            probe.decompress(compressed[position : position + 1])
        except zlib.error:
            first_error = position
            break
    if first_error is None:
        raise ValueError("DEFLATE failed but no parser-error position was found")

    search_start = max(0, first_error - 512)
    probe_end = min(len(compressed), first_error + 131072)
    state = zlib.decompressobj(-zlib.MAX_WBITS)
    state.decompress(compressed[:search_start])
    survivors: list[tuple[int, int]] = []

    for position in range(search_start, first_error + 1):
        before = state.copy()
        tail = compressed[position:probe_end]
        for value in range(256):
            candidate = before.copy()
            try:
                candidate.decompress(bytes((value,)) + tail)
            except zlib.error:
                continue
            survivors.append((position, value))
        try:
            state.decompress(compressed[position : position + 1])
        except zlib.error:
            break

    for position, value in survivors:
        repaired = compressed[:position] + bytes((value,)) + compressed[position:]
        try:
            data = zlib.decompress(repaired, -zlib.MAX_WBITS)
        except zlib.error:
            continue
        crc = binascii.crc32(data) & 0xFFFFFFFF
        if len(data) == expected_size and crc == expected_crc:
            return data, position, value

    raise ValueError(
        f"single-byte DEFLATE repair failed near compressed offset {first_error}"
    )


def unpack_entry(
    blob: bytes, info: zipfile.ZipInfo, offset: int
) -> tuple[bytes, int, tuple[int, int] | None]:
    fields = LOCAL_HEADER.unpack_from(blob, offset)
    magic, _extract, flags, method, _time, _date, _crc, _csize, _usize, nlen, xlen = fields
    if magic != 0x04034B50:
        raise ValueError("invalid local header")
    if flags & 0x1:
        raise ValueError("encrypted entries are unsupported")

    payload_start = offset + LOCAL_HEADER.size + nlen + xlen
    payload_end = payload_start + info.compress_size
    compressed = blob[payload_start:payload_end]
    if len(compressed) != info.compress_size:
        raise ValueError("truncated compressed payload")

    if method == zipfile.ZIP_STORED:
        data = compressed
        repair = None
    elif method == zipfile.ZIP_DEFLATED:
        try:
            data = zlib.decompress(compressed, -zlib.MAX_WBITS)
            repair = None
        except zlib.error:
            data, repair_offset, repair_value = repair_single_missing_deflate_byte(
                compressed, info.file_size, info.CRC
            )
            repair = (repair_offset, repair_value)
    else:
        raise ValueError(f"unsupported compression method {method}")

    if len(data) != info.file_size:
        raise ValueError(f"size mismatch: got {len(data)}, expected {info.file_size}")
    return data, binascii.crc32(data) & 0xFFFFFFFF, repair


def wanted(name: str, extract_all: bool) -> bool:
    if extract_all:
        return True
    return (
        name == "AndroidManifest.xml"
        or name == "classes.dex"
        or name.startswith("assets/")
        or name.startswith("lib/armeabi-v7a/")
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("apk", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--all", action="store_true", help="extract every APK entry")
    args = parser.parse_args()

    blob = args.apk.read_bytes()
    headers = physical_headers(blob)
    args.output.mkdir(parents=True, exist_ok=True)

    extracted = 0
    warnings = 0
    with zipfile.ZipFile(args.apk) as archive:
        for info in archive.infolist():
            if info.is_dir() or not wanted(info.filename, args.all):
                continue
            offset = headers.get(info.filename)
            if offset is None:
                print(f"ERROR: physical header not found: {info.filename}", file=sys.stderr)
                return 2
            try:
                data, actual_crc, repair = unpack_entry(blob, info, offset)
            except Exception as exc:
                print(f"ERROR: {info.filename}: {exc}", file=sys.stderr)
                return 2

            if actual_crc != info.CRC:
                warnings += 1
                print(
                    f"WARN: {info.filename}: central CRC {info.CRC:08x}, "
                    f"physical payload CRC {actual_crc:08x}",
                    file=sys.stderr,
                )
            if repair is not None:
                warnings += 1
                repair_offset, repair_value = repair
                print(
                    f"WARN: {info.filename}: restored missing DEFLATE byte "
                    f"{repair_value:02x} at compressed offset {repair_offset}",
                    file=sys.stderr,
                )

            destination = safe_output(args.output, info.filename)
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_name(destination.name + ".new")
            with temporary.open("wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            temporary.replace(destination)
            extracted += 1
            print(f"{info.filename}\t{len(data)}\tcrc={actual_crc:08x}")

    print(f"extracted={extracted} warnings={warnings}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
