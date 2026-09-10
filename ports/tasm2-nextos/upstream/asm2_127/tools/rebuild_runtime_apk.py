#!/usr/bin/env python3
"""Build a valid runtime ZIP/APK from the user-supplied ASM2 1.2.7d APK.

The distributed APK has damaged ZIP bookkeeping and one raw-DEFLATE stream
with a missing byte.  ``extract_apk.py`` can recover every entry by locating
its physical local header and validating the uncompressed result.  This tool
uses that recovery path to write a separate, standards-compliant ZIP for the
native runtime.  Recovered members are stored without compression so the
result is byte-identical across the different zlib versions shipped by the
supported firmware.  It never modifies or resigns the source APK.

The generated file contains proprietary game data and must stay outside Git.
The port's ``.gitignore`` excludes ``*.apk`` and ``gamefiles/`` for that reason.
"""

from __future__ import annotations

import argparse
import binascii
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import zipfile
import zlib

from extract_apk import physical_headers, unpack_entry


SUPPORTED_METHODS = {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}


@dataclass(frozen=True)
class RecoveredEntry:
    name: str
    compress_type: int
    size: int
    crc: int
    sha256: str


def output_info(source: zipfile.ZipInfo) -> zipfile.ZipInfo:
    """Copy metadata that is meaningful after rebuilding local headers."""

    result = zipfile.ZipInfo(source.filename, source.date_time)
    result.compress_type = zipfile.ZIP_STORED
    result.comment = source.comment
    result.extra = source.extra
    result.create_system = source.create_system
    result.create_version = source.create_version
    result.extract_version = source.extract_version
    result.internal_attr = source.internal_attr
    result.external_attr = source.external_attr
    result.volume = source.volume
    return result


def recover(
    blob: bytes,
    info: zipfile.ZipInfo,
    headers: dict[str, int],
) -> tuple[bytes, int, tuple[int, int] | None]:
    offset = headers.get(info.filename)
    if offset is None:
        raise ValueError(f"physical local header not found: {info.filename}")
    if info.compress_type not in SUPPORTED_METHODS:
        raise ValueError(
            f"unsupported compression method {info.compress_type}: {info.filename}"
        )
    return unpack_entry(blob, info, offset)


def build(source: Path, temporary: Path) -> list[RecoveredEntry]:
    blob = source.read_bytes()
    headers = physical_headers(blob)
    recovered: list[RecoveredEntry] = []

    with zipfile.ZipFile(source, "r") as original:
        infos = original.infolist()
        names = [info.filename for info in infos]
        if len(names) != len(set(names)):
            raise ValueError("duplicate entry names are unsupported")

        with zipfile.ZipFile(
            temporary,
            "w",
            allowZip64=True,
        ) as runtime:
            runtime.comment = original.comment
            for number, info in enumerate(infos, start=1):
                data, actual_crc, repair = recover(blob, info, headers)
                digest = hashlib.sha256(data).hexdigest()
                recovered.append(
                    RecoveredEntry(
                        info.filename,
                        zipfile.ZIP_STORED,
                        len(data),
                        actual_crc,
                        digest,
                    )
                )

                notes: list[str] = []
                if actual_crc != info.CRC:
                    notes.append(
                        f"central CRC {info.CRC:08x} replaced by actual {actual_crc:08x}"
                    )
                if repair is not None:
                    repair_offset, repair_value = repair
                    notes.append(
                        f"restored DEFLATE byte {repair_value:02x} at {repair_offset}"
                    )
                if notes:
                    print(f"RECOVER {info.filename}: {'; '.join(notes)}")

                runtime.writestr(
                    output_info(info),
                    data,
                    compress_type=zipfile.ZIP_STORED,
                )
                if number % 100 == 0 or number == len(infos):
                    print(f"built {number}/{len(infos)} entries")

    with temporary.open("rb") as stream:
        os.fsync(stream.fileno())
    return recovered


def verify(runtime_apk: Path, expected: list[RecoveredEntry]) -> None:
    with zipfile.ZipFile(runtime_apk, "r") as archive:
        bad_member = archive.testzip()
        if bad_member is not None:
            raise ValueError(f"standard ZIP CRC test failed: {bad_member}")

        infos = archive.infolist()
        if len(infos) != len(expected):
            raise ValueError(
                f"entry count mismatch: got {len(infos)}, expected {len(expected)}"
            )

        for number, (info, reference) in enumerate(
            zip(infos, expected), start=1
        ):
            if info.filename != reference.name:
                raise ValueError(
                    f"entry order/name mismatch at {number}: "
                    f"got {info.filename!r}, expected {reference.name!r}"
                )
            if info.compress_type != reference.compress_type:
                raise ValueError(
                    f"compression method mismatch for {info.filename}: "
                    f"got {info.compress_type}, expected {reference.compress_type}"
                )
            if info.file_size != reference.size:
                raise ValueError(
                    f"size mismatch for {info.filename}: "
                    f"got {info.file_size}, expected {reference.size}"
                )
            if info.CRC != reference.crc:
                raise ValueError(
                    f"CRC mismatch for {info.filename}: "
                    f"got {info.CRC:08x}, expected {reference.crc:08x}"
                )

            data = archive.read(info)
            actual_crc = binascii.crc32(data) & 0xFFFFFFFF
            actual_sha256 = hashlib.sha256(data).hexdigest()
            if actual_crc != reference.crc or actual_sha256 != reference.sha256:
                raise ValueError(f"byte comparison failed for {info.filename}")

            if number % 100 == 0 or number == len(infos):
                print(f"verified {number}/{len(infos)} entries")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="rebuild a valid, local-only runtime APK from ASM2 1.2.7d"
    )
    parser.add_argument("source_apk", type=Path)
    parser.add_argument("runtime_apk", type=Path)
    parser.add_argument(
        "--force",
        action="store_true",
        help="atomically replace an existing runtime APK (never the source)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        source = args.source_apk.resolve(strict=True)
    except FileNotFoundError:
        print(f"ERROR: source APK not found: {args.source_apk}", file=sys.stderr)
        return 2

    destination = args.runtime_apk.resolve(strict=False)
    if source == destination:
        print("ERROR: runtime APK must not be the source APK", file=sys.stderr)
        return 2
    if destination.suffix.lower() != ".apk":
        print("ERROR: runtime output must use the .apk suffix", file=sys.stderr)
        return 2
    if destination.exists() and not args.force:
        print(
            f"ERROR: destination exists (use --force): {destination}",
            file=sys.stderr,
        )
        return 2

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=f".{destination.name}.",
            suffix=".new.apk",
            dir=destination.parent,
            delete=False,
        ) as stream:
            temporary_path = Path(stream.name)

        expected = build(source, temporary_path)
        verify(temporary_path, expected)
        temporary_path.replace(destination)
        temporary_path = None

        directory_fd = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)

        digest = hashlib.sha256(destination.read_bytes()).hexdigest()
        print(
            f"OK: {len(expected)} entries, sha256={digest}, "
            f"runtime_apk={destination}"
        )
        return 0
    except (OSError, ValueError, zipfile.BadZipFile, zlib.error) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
