#!/usr/bin/env python3
"""Materialize Tearscape's pinned Linux arm64 .NET runtime without NuGet.

Only the exact native and managed closure recorded in the port manifest is
accepted.  The owner APK is deliberately not an input to this tool.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import zipfile


SCHEMA = "tearscape-dotnet-runtime-v1"
MAX_ARCHIVE_MEMBERS = 4096
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024


class RuntimeError_(RuntimeError):
    pass


def fail(message: str) -> None:
    raise RuntimeError_(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def regular_file(path: Path, label: str) -> Path:
    try:
        metadata = path.lstat()
    except OSError as error:
        fail(f"{label} is unavailable: {error}")
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        fail(f"{label} must be a regular non-symlink file")
    return path


def strict_json(path: Path) -> dict:
    def reject_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                fail(f"runtime manifest has duplicate key {key!r}")
            result[key] = value
        return result

    try:
        with regular_file(path, "runtime manifest").open(
            "r", encoding="utf-8"
        ) as stream:
            value = json.load(stream, object_pairs_hook=reject_duplicates)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot parse runtime manifest: {error}")
    if not isinstance(value, dict):
        fail("runtime manifest must be an object")
    return value


def safe_member(name: str) -> PurePosixPath:
    path = PurePosixPath(name)
    if (
        not name
        or "\\" in name
        or path.is_absolute()
        or not path.parts
        or any(part in ("", ".", "..") for part in path.parts)
    ):
        fail(f"unsafe nupkg member {name!r}")
    return path


def parse_manifest(path: Path) -> tuple[str, list[dict]]:
    value = strict_json(path)
    expected_keys = {"schema", "package", "version", "nupkg_sha256", "files"}
    if set(value) != expected_keys or value.get("schema") != SCHEMA:
        fail("runtime manifest schema/keys differ from the closed contract")
    digest = value.get("nupkg_sha256")
    if not isinstance(digest, str) or len(digest) != 64:
        fail("runtime manifest has an invalid nupkg digest")
    records = value.get("files")
    if not isinstance(records, list) or len(records) != 184:
        fail("runtime manifest must contain the exact 184-file execution closure")

    members: set[str] = set()
    destinations: set[str] = set()
    previous = b""
    parsed = []
    for index, record in enumerate(records):
        if not isinstance(record, dict) or set(record) != {
            "destination", "kind", "member", "mode", "sha256", "size"
        }:
            fail(f"invalid runtime file record {index}")
        destination = record.get("destination")
        member = record.get("member")
        kind = record.get("kind")
        mode = record.get("mode")
        file_digest = record.get("sha256")
        size = record.get("size")
        if (
            not isinstance(destination, str)
            or PurePosixPath(destination).name != destination
            or destination.startswith(".")
            or destination.casefold() in destinations
        ):
            fail(f"unsafe/duplicate runtime destination {destination!r}")
        if destination.encode() <= previous:
            fail("runtime records are not in canonical destination order")
        previous = destination.encode()
        destinations.add(destination.casefold())
        safe_member(member if isinstance(member, str) else "")
        if member in members:
            fail(f"duplicate runtime member {member!r}")
        members.add(member)
        if kind not in ("managed", "native-linux") or mode != "0644":
            fail(f"invalid runtime kind/mode for {destination}")
        if kind == "managed" and not destination.endswith(".dll"):
            fail(f"managed runtime destination is not a DLL: {destination}")
        if kind == "native-linux" and not destination.endswith(".so"):
            fail(f"native runtime destination is not an SO: {destination}")
        if not isinstance(file_digest, str) or len(file_digest) != 64:
            fail(f"invalid digest for {destination}")
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
            fail(f"invalid size for {destination}")
        parsed.append(record)
    return digest, parsed


def require_empty_directory(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        fail("output must be a real directory")
    if any(path.iterdir()):
        fail("output directory must be empty")
    return path


def materialize(nupkg: Path, manifest: Path, output: Path) -> None:
    expected_archive, records = parse_manifest(manifest)
    regular_file(nupkg, "nupkg")
    if sha256_file(nupkg) != expected_archive:
        fail("nupkg SHA-256 differs from the pinned official runtime")
    output = require_empty_directory(output)

    try:
        archive = zipfile.ZipFile(nupkg, "r")
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"cannot open nupkg: {error}")
    with archive:
        infos = archive.infolist()
        if len(infos) > MAX_ARCHIVE_MEMBERS:
            fail("nupkg has too many members")
        if sum(item.file_size for item in infos) > MAX_ARCHIVE_BYTES:
            fail("nupkg expanded-size ceiling exceeded")
        by_name: dict[str, zipfile.ZipInfo] = {}
        casefolded: set[str] = set()
        for info in infos:
            safe_member(info.filename)
            folded = info.filename.casefold()
            if info.filename in by_name or folded in casefolded:
                fail("nupkg contains duplicate/case-colliding members")
            by_name[info.filename] = info
            casefolded.add(folded)
            unix_mode = (info.external_attr >> 16) & 0xFFFF
            if stat.S_ISLNK(unix_mode):
                fail(f"nupkg contains a symlink: {info.filename}")

        for record in records:
            info = by_name.get(record["member"])
            if info is None or info.is_dir() or info.file_size != record["size"]:
                fail(f"runtime member missing/size mismatch: {record['member']}")
            data = archive.read(info)
            if len(data) != record["size"] or sha256_bytes(data) != record["sha256"]:
                fail(f"runtime member digest mismatch: {record['member']}")
            target = output / record["destination"]
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
            descriptor = os.open(target, flags, 0o600)
            try:
                with os.fdopen(descriptor, "wb", closefd=True) as stream:
                    stream.write(data)
                    stream.flush()
                    os.fsync(stream.fileno())
            except Exception:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
                raise
            target.chmod(0o644)

    print(
        "TEARSCAPE DOTNET RUNTIME: PASS files={} nupkg_sha256={}".format(
            len(records), expected_archive
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nupkg", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    materialize(arguments.nupkg, arguments.manifest, arguments.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError_ as error:
        print(f"TEARSCAPE DOTNET RUNTIME: FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
