#!/usr/bin/env python3
"""Validate the staged Huntdown AArch64 payload by technical contract.

Container names, signatures, whole-container hashes and internal payload
hashes are intentionally not part of this contract (APK-COMPAT-01: identity
of the owner-provided container never decides compatibility).  NXExtract may
obtain the files from a monolithic APK, split APK set, APKM, APKS, XAPK or a
harmlessly repacked equivalent, and an unknown compatible build must never be
rejected for missing a whitelist.

What is validated here is the technical shape every runnable Huntdown build
shares: safe regular files, little-endian AArch64 ELF64 engine libraries with
a sane program-header table, UnityFS archives, IL2CPP global-metadata, the
rebuilt Unity asset pack and the authored MP4 movies.  The private IL2CPP
bridges re-verify their exact instruction bytes at runtime before patching
and degrade per subsystem, so install time does not gate on build identity.
"""

import argparse
import json
import os
from pathlib import Path
import stat
import struct
import tempfile


MARKER = ".huntdown-build.json"

ENGINE_LIBRARIES = ("libunity.so", "libil2cpp.so", "libmain.so")

UNITYFS_ARCHIVES = ("bin/Data/data.unity3d", "bin/Data/datapack.unity3d")

GLOBAL_METADATA = "bin/Data/Managed/Metadata/global-metadata.dat"

ASSET_PACK = "UnityDataAssetPack.apk"

AUTHORED_MOVIES = ("video/CoffeeIntro.mp4", "video/EasyTriggerVignette.mp4")


def fail(message):
    raise SystemExit("huntdown validation error: %s" % message)


def checked_file(root, relative):
    path = root / relative
    try:
        mode = path.lstat().st_mode
    except OSError as error:
        fail("cannot read %s: %s" % (relative, error))
    if not stat.S_ISREG(mode) or path.is_symlink():
        fail("%s is not a safe regular file" % relative)
    try:
        path.resolve().relative_to(root)
    except (OSError, ValueError):
        fail("%s escapes the staging root" % relative)
    if path.lstat().st_size <= 0:
        fail("%s is empty" % relative)
    return path


def check_elf64_aarch64(path, relative):
    with path.open("rb") as stream:
        header = stream.read(64)
    if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01":
        fail("%s is not little-endian ELF64" % relative)
    fields = struct.unpack("<16sHHIQQQIHHHHHH", header)
    if fields[2] != 183:
        fail("%s is not AArch64" % relative)
    phnum = fields[10]
    if fields[9] < 56 or phnum <= 0 or phnum > 128:
        fail("%s has an invalid program-header table" % relative)


def check_magic(path, relative, expected, offset=0):
    with path.open("rb") as stream:
        stream.seek(offset)
        actual = stream.read(len(expected))
    if actual != expected:
        fail("%s does not carry the expected format signature" % relative)


def validate(root):
    observed = {}
    for relative in ENGINE_LIBRARIES:
        path = checked_file(root, relative)
        check_elf64_aarch64(path, relative)
        observed[relative] = path.lstat().st_size
    for relative in UNITYFS_ARCHIVES:
        path = root / relative
        if not path.exists():
            continue  # the datapack may already live inside the asset pack
        checked_file(root, relative)
        check_magic(path, relative, b"UnityFS\x00")
    metadata = checked_file(root, GLOBAL_METADATA)
    check_magic(metadata, GLOBAL_METADATA, b"\xaf\x1b\xb1\xfa")
    pack = checked_file(root, ASSET_PACK)
    check_magic(pack, ASSET_PACK, b"PK\x03\x04")
    for relative in AUTHORED_MOVIES:
        path = checked_file(root, relative)
        check_magic(path, relative, b"ftyp", offset=4)
    return observed


def write_marker(root, observed):
    document = {
        "schema": "org.nextos.huntdown.payload-profile",
        "schema_version": 2,
        "package_id": "com.coffeestain.huntdown",
        "abi": "arm64-v8a",
        "validation": "technical-contract",
        "engine": {
            name: {"bytes": size} for name, size in sorted(observed.items())
        },
    }
    payload = (json.dumps(document, sort_keys=True,
                          separators=(",", ":")) + "\n").encode()
    fd, temporary = tempfile.mkstemp(prefix=MARKER + ".", dir=str(root))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, root / MARKER)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", required=True)
    parser.add_argument("--write-marker", action="store_true")
    args = parser.parse_args()
    root = Path(args.stage).resolve()
    if not root.is_dir():
        fail("stage is not a directory")
    observed = validate(root)
    if args.write_marker:
        write_marker(root, observed)
    print(
        "Huntdown payload validated: technical contract OK "
        "(engines: %s)"
        % ", ".join("%s=%d" % (name, size)
                    for name, size in sorted(observed.items()))
    )


if __name__ == "__main__":
    main()
