#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Symbolize one sanitized NXObs crash receipt against private symbols."""

from __future__ import print_function

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import subprocess
import sys


BUILD_ID = re.compile(r"Build ID:\s*([0-9a-fA-F]{8,128})")
ADDRESS = re.compile(r"^0x[0-9a-f]+$")
SAFE = re.compile(r"^[A-Za-z0-9._:+@-]{1,127}$")


class SymbolizeError(Exception):
    pass


def strict_json_bytes(value, context):
    if value.startswith(b"\xef\xbb\xbf"):
        raise SymbolizeError("%s contains a UTF-8 BOM" % context)

    def unique(pairs):
        result = {}
        for key, item in pairs:
            if key in result:
                raise SymbolizeError("%s contains a duplicate key" % context)
            result[key] = item
        return result

    def constant(value):
        raise SymbolizeError("%s contains %s" % (context, value))

    try:
        return json.loads(value.decode("utf-8"), object_pairs_hook=unique,
                          parse_constant=constant)
    except (UnicodeError, ValueError) as error:
        raise SymbolizeError("%s is malformed: %s" % (context, error))


def regular_bytes(path, maximum, context):
    path = Path(path)
    try:
        info = path.lstat()
    except OSError as error:
        raise SymbolizeError("%s is unavailable: %s" % (context, error))
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise SymbolizeError("%s must be a regular non-symlink file" % context)
    if info.st_size <= 0 or info.st_size > maximum:
        raise SymbolizeError("%s size is outside the boundary" % context)
    return path.resolve(), path.read_bytes()


def binary_build_id(path):
    try:
        env = dict(os.environ, LC_ALL="C")
        output = subprocess.check_output(
            ["readelf", "-nW", str(path)], stderr=subprocess.STDOUT,
            universal_newlines=True, timeout=30, env=env,
        )
    except (OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        raise SymbolizeError("cannot read symbol binary build-id: %s" % error)
    values = {item.lower() for item in BUILD_ID.findall(output)}
    if len(values) != 1:
        raise SymbolizeError("symbol binary must expose exactly one build-id")
    return next(iter(values))


def safe_name(value, fallback="unknown"):
    value = value.strip()
    return value if SAFE.fullmatch(value) else fallback


def symbolize(receipt_path, binary_path, output_path):
    receipt_file, receipt_bytes = regular_bytes(
        receipt_path, 64 * 1024, "crash receipt"
    )
    lines = receipt_bytes.splitlines()
    if len(lines) != 1:
        raise SymbolizeError("crash receipt must contain exactly one record")
    receipt = strict_json_bytes(lines[0], "crash receipt")
    if (not isinstance(receipt, dict) or
            receipt.get("schema") != "nx-crash-v1" or
            receipt.get("schema_version") != 1):
        raise SymbolizeError("unsupported crash receipt schema")
    binary_file, binary_bytes = regular_bytes(
        binary_path, 1024 * 1024 * 1024, "symbol binary"
    )
    expected_module = receipt.get("module")
    if expected_module != binary_file.name:
        raise SymbolizeError("symbol binary basename differs from crash module")
    archived_build_id = receipt.get("build_id")
    if (not isinstance(archived_build_id, str) or
            archived_build_id == "unavailable"):
        raise SymbolizeError("crash receipt has no usable build-id")
    actual_build_id = binary_build_id(binary_file)
    if actual_build_id != archived_build_id:
        raise SymbolizeError("symbol binary build-id differs from crash receipt")
    offset = receipt.get("module_offset")
    if not isinstance(offset, str) or not ADDRESS.fullmatch(offset):
        raise SymbolizeError("crash receipt module_offset is invalid")
    try:
        env = dict(os.environ, LC_ALL="C")
        raw = subprocess.check_output(
            ["addr2line", "-f", "-C", "-e", str(binary_file), offset],
            stderr=subprocess.STDOUT, universal_newlines=True, timeout=30,
            env=env,
        ).splitlines()
    except (OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        raise SymbolizeError("addr2line failed: %s" % error)
    function = safe_name(raw[0] if raw else "unknown")
    location = raw[1] if len(raw) > 1 else "unknown:0"
    location_name, separator, line_text = location.rpartition(":")
    source_name = safe_name(Path(location_name).name if separator else
                            "unknown")
    try:
        source_line = int(line_text) if separator else 0
    except ValueError:
        source_line = 0
    if source_line < 0:
        source_line = 0
    report = {
        "schema": "nx-symbolization-v1",
        "schema_version": 1,
        "receipt_sha256": hashlib.sha256(receipt_bytes).hexdigest(),
        "binary_sha256": hashlib.sha256(binary_bytes).hexdigest(),
        "module": expected_module,
        "build_id": actual_build_id,
        "module_offset": offset,
        "function": function,
        "source_basename": source_name,
        "source_line": source_line,
        "paths_sanitized": True,
    }
    output = Path(output_path)
    if not output.is_absolute() or output.exists() or output.is_symlink():
        raise SymbolizeError("output must be a new absolute path")
    parent = output.parent
    parent_info = parent.lstat()
    if stat.S_ISLNK(parent_info.st_mode) or not stat.S_ISDIR(parent_info.st_mode):
        raise SymbolizeError("output parent must be a real directory")
    payload = (json.dumps(report, sort_keys=True, separators=(",", ":")) +
               "\n").encode("ascii")
    temporary = parent / (".%s.tmp.%d.%s" % (
        output.name, os.getpid(), secrets.token_hex(4)
    ))
    descriptor = os.open(str(temporary),
                         os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(str(temporary), str(output))
        directory_fd = os.open(
            str(parent), os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        )
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
    return report


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--receipt", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--output", required=True)
    arguments = parser.parse_args(argv)
    try:
        result = symbolize(arguments.receipt, arguments.binary,
                           arguments.output)
    except (OSError, SymbolizeError) as error:
        print("nx-symbolize-crash: %s" % error, file=sys.stderr)
        return 1
    print("nx_symbolize=PASS module=%s build_id=%s offset=%s" % (
        result["module"], result["build_id"], result["module_offset"]
    ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
