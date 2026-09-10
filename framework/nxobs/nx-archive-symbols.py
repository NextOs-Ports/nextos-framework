#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Atomically archive project symbols in a private tree outside public ZIPs."""

from __future__ import print_function

import argparse
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys


BUILD_ID = re.compile(r"Build ID:\s*([0-9a-fA-F]{8,128})")
COMMIT = re.compile(r"^[0-9a-f]{7,64}$")


class ArchiveError(Exception):
    pass


def fsync_directory(path):
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open(str(path), flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def rename_noreplace(source, destination):
    library = ctypes.CDLL(None, use_errno=True)
    function = getattr(library, "renameat2", None)
    if function is None:
        raise ArchiveError("renameat2(RENAME_NOREPLACE) is required")
    function.argtypes = [ctypes.c_int, ctypes.c_char_p,
                         ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    function.restype = ctypes.c_int
    result = function(
        -100, os.fsencode(str(source)), -100, os.fsencode(str(destination)), 1
    )
    if result == 0:
        return
    value = ctypes.get_errno()
    if value == errno.EEXIST:
        raise ArchiveError("output appeared concurrently")
    raise ArchiveError("renameat2(RENAME_NOREPLACE) failed: %s" %
                       os.strerror(value))


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def build_id(path):
    try:
        env = dict(os.environ, LC_ALL="C")
        output = subprocess.check_output(
            ["readelf", "-nW", str(path)], stderr=subprocess.STDOUT,
            universal_newlines=True, timeout=30, env=env,
        )
    except (OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        raise ArchiveError("cannot read build-id for %s: %s" %
                           (path.name, error))
    values = {value.lower() for value in BUILD_ID.findall(output)}
    if len(values) != 1:
        raise ArchiveError("%s must expose exactly one build-id" % path.name)
    return next(iter(values))


def validate_private_root(path):
    root = Path(path)
    if not root.is_absolute():
        raise ArchiveError("private root must be absolute")
    info = root.lstat()
    if (stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode) or
            info.st_uid != os.geteuid() or info.st_mode & 0o077):
        raise ArchiveError("private root must be real, owned and mode 0700-class")
    return root.resolve()


def regular_binary(path):
    candidate = Path(path)
    info = candidate.lstat()
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise ArchiveError("symbol input must be a regular non-symlink file")
    if info.st_size <= 0 or info.st_size > 1024 * 1024 * 1024:
        raise ArchiveError("symbol input size is outside the boundary")
    return candidate.resolve()


def archive_symbols(private_root, output, binaries, source_commit):
    root = validate_private_root(private_root)
    destination = Path(output)
    if (not destination.is_absolute() or ".." in destination.parts or
            destination.exists() or destination.is_symlink()):
        raise ArchiveError("output must be a new absolute path")
    requested_parent = destination.parent.lstat()
    if (stat.S_ISLNK(requested_parent.st_mode) or
            not stat.S_ISDIR(requested_parent.st_mode)):
        raise ArchiveError("output parent must be a real directory")
    destination = destination.parent.resolve() / destination.name
    try:
        destination.relative_to(root)
    except ValueError:
        raise ArchiveError("output must live below the private root")
    if not COMMIT.fullmatch(source_commit):
        raise ArchiveError("source commit must be a lowercase hexadecimal object ID")
    inputs = [regular_binary(value) for value in binaries]
    if not inputs:
        raise ArchiveError("at least one symbol binary is required")
    entries = []
    seen = set()
    for binary in inputs:
        identifier = build_id(binary)
        key = (identifier, binary.name)
        if key in seen:
            raise ArchiveError("duplicate symbol input")
        seen.add(key)
        entries.append({
            "source": binary,
            "name": binary.name,
            "build_id": identifier,
            "sha256": digest(binary),
            "bytes": binary.stat().st_size,
        })
    parent = destination.parent
    parent_info = parent.lstat()
    if stat.S_ISLNK(parent_info.st_mode) or not stat.S_ISDIR(parent_info.st_mode):
        raise ArchiveError("output parent must be a real directory")
    temporary = parent / (".%s.tmp.%d.%s" % (
        destination.name, os.getpid(), secrets.token_hex(4)
    ))
    temporary.mkdir(mode=0o700)
    try:
        manifest_entries = []
        for entry in sorted(entries, key=lambda item: (item["build_id"], item["name"])):
            relative = Path(entry["build_id"]) / entry["name"]
            target = temporary / relative
            target.parent.mkdir(mode=0o700)
            with entry["source"].open("rb") as source:
                descriptor = os.open(
                    str(target), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
                )
                with os.fdopen(descriptor, "wb") as output_stream:
                    shutil.copyfileobj(source, output_stream, 1024 * 1024)
                    output_stream.flush()
                    os.fsync(output_stream.fileno())
            if digest(target) != entry["sha256"]:
                raise ArchiveError("symbol copy hash mismatch")
            manifest_entries.append({
                "path": relative.as_posix(),
                "name": entry["name"],
                "build_id": entry["build_id"],
                "sha256": entry["sha256"],
                "bytes": entry["bytes"],
            })
        manifest = {
            "schema": "nx-symbol-archive-v1",
            "schema_version": 1,
            "source_commit": source_commit,
            "public_zip_member": False,
            "symbols": manifest_entries,
        }
        manifest_bytes = (json.dumps(
            manifest, sort_keys=True, separators=(",", ":")
        ) + "\n").encode("ascii")
        manifest_path = temporary / "SYMBOLS-MANIFEST.json"
        descriptor = os.open(
            str(manifest_path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
        )
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(manifest_bytes)
            stream.flush()
            os.fsync(stream.fileno())
        checksum = hashlib.sha256(manifest_bytes).hexdigest()
        checksum_path = temporary / "MANIFEST.sha256"
        descriptor = os.open(
            str(checksum_path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
        )
        with os.fdopen(descriptor, "wb") as stream:
            stream.write((checksum + "  SYMBOLS-MANIFEST.json\n").encode("ascii"))
            stream.flush()
            os.fsync(stream.fileno())
        for directory in sorted(
                (path for path in temporary.iterdir() if path.is_dir()),
                key=lambda path: path.name):
            fsync_directory(directory)
        fsync_directory(temporary)
        rename_noreplace(temporary, destination)
        fsync_directory(parent)
    except Exception:
        if temporary.exists():
            shutil.rmtree(str(temporary))
        raise
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--private-root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--binary", action="append", required=True)
    parser.add_argument("--source-commit", required=True)
    arguments = parser.parse_args(argv)
    try:
        result = archive_symbols(
            arguments.private_root, arguments.output, arguments.binary,
            arguments.source_commit,
        )
    except (ArchiveError, OSError) as error:
        print("nx-archive-symbols: %s" % error, file=sys.stderr)
        return 1
    print("nx_symbol_archive=PASS symbols=%d public_zip_member=0" %
          len(result["symbols"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
