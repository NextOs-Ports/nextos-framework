#!/usr/bin/env python3
"""Prepare owner-supplied Prizefighters 2 v1.09.3 data on the handheld.

NXExtract first stages the pristine assets and ARM64 split libraries.  This
hook validates that exact input, reconstructs four small runtime windows using
version-pinned XOR transformation masks, and creates a GLES2 copy of Unity's
main asset with the vendored, Python-3.7-compatible UnityPy toolchain.

No APK, complete game library, game asset, raw overlay, save, receipt or
purchase state is distributed by this project.
"""

from __future__ import print_function

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys


PACKAGE = "com.koalitygame.prizefighters2"
GAME_VERSION = "1.09.3"
PREPARATION = "pf2-arm64-xormask-gles2-v2"
MARKER = ".pf2-data.json"

ORIGINAL_FILES = {
    "assets/bin/Data/data.unity3d": (
        3702830,
        "1907b95153585eb7b0734c02310d654825b0df665e99588b4745fc0469b74365",
    ),
    "assets/bin/Data/Managed/Metadata/global-metadata.dat": (
        8858944,
        "6cc99669c7cf5c17a4f3e30ded80296e983a54c88117c94b47e7890fefde8216",
    ),
    "lib/libmain.so": (
        6728,
        "9348cd29bcdec5b677c7124b111a2ebb285bfd6550282619c8fd178f2982eb5d",
    ),
    "lib/libil2cpp.so": (
        53135664,
        "304ad445f2eafb2ad7177e092e0aeb4cf62731940ac2a05b3ec5cff3a049b2c0",
    ),
    "lib/libunity.so": (
        19679648,
        "0f4f6cc64871d381b938bddbb853c2d9b36154a18987ed681c4f6e3bf9340b87",
    ),
    "lib/lib_burst_generated.so": (
        116848,
        "12d6e600ac93155b1aafa5247efc91c5c9e91498f1d9655061b2f8b6f5f6e770",
    ),
}

WINDOWS = {
    "libil2cpp.text": (
        "lib/libil2cpp.so",
        0x0118BF7C,
        51200,
        "9aace79e98b47dd5460a97cfa5810ba2bf7ceb0d7361bfc708598e5aa0c5edc5",
        "2a5100d0964e334a700df9f3f015298ba0fb5709faea914a97a3c0c330f5b3af",
    ),
    "libil2cpp.data": (
        "lib/libil2cpp.so",
        0x02F0E7A0,
        51120,
        "868e71e3a1ef63f9fecb25747c9ccae110419da79eb50d2c720d032f309d6025",
        "1fccf72b18c607fe7d1e1a06c01d4fb7c7df92a9239a6f018c1fe35e5924f25b",
    ),
    "libunity.text": (
        "lib/libunity.so",
        0x00397710,
        51200,
        "bc7dc02c875f9ebadb904abf031cfd5d141e2b583d0a0d9e42199c219702547b",
        "34c46bca967ef8c667805f045b0f382a382254180716f95e0dc762e7b09ac7a0",
    ),
    "libunity.data": (
        "lib/libunity.so",
        0x011CDBF8,
        18448,
        "b364e95e0a94095a6cdae37d47cf90288135ef9a42c57d1eb8b7923e8a350f58",
        "170f471d673b916cf6d3c6bfbbddf7a50edaf41731813898991ff27d8ab8ac5c",
    ),
}

BUNDLED_LZ4_SIZE = 108824
BUNDLED_LZ4_SHA256 = (
    "a65c53e2e7015b636e4f212449eff2016b99736cdf5798fe2cf3672818b88b8b"
)


class PreparationError(RuntimeError):
    pass


def fail(message):
    raise PreparationError("Prizefighters 2 data preparation failed: " + message)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def regular_file(path):
    try:
        return stat.S_ISREG(os.lstat(path).st_mode)
    except OSError:
        return False


def contained(root, path):
    root = os.path.realpath(root)
    path = os.path.realpath(path)
    return path == root or path.startswith(root + os.sep)


def validate_file(path, size, expected_hash, label):
    if not regular_file(path):
        fail(label + " is missing or not a regular file")
    actual_size = os.path.getsize(path)
    if actual_size != size:
        fail("%s has size %d, expected %d" % (label, actual_size, size))
    actual_hash = sha256_file(path)
    if actual_hash != expected_hash:
        fail("%s has unsupported SHA-256 %s" % (label, actual_hash))


def validate_originals(stage):
    for relative, expected in sorted(ORIGINAL_FILES.items()):
        validate_file(
            os.path.join(stage, *relative.split("/")),
            expected[0],
            expected[1],
            relative,
        )


def file_window_for_vaddr(path, vaddr, size):
    with open(path, "rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01":
            fail(os.path.basename(path) + " is not a little-endian ELF64 image")
        phoff = struct.unpack_from("<Q", header, 32)[0]
        phentsize = struct.unpack_from("<H", header, 54)[0]
        phnum = struct.unpack_from("<H", header, 56)[0]
        if phentsize < 56 or phnum > 256:
            fail(os.path.basename(path) + " has an unsafe program-header table")
        stream.seek(phoff)
        table = stream.read(phentsize * phnum)
        if len(table) != phentsize * phnum:
            fail(os.path.basename(path) + " has a truncated program-header table")
        for index in range(phnum):
            entry = index * phentsize
            if struct.unpack_from("<I", table, entry)[0] != 1:
                continue
            file_offset, segment_vaddr = struct.unpack_from(
                "<QQ", table, entry + 8
            )
            file_size = struct.unpack_from("<Q", table, entry + 32)[0]
            if (
                segment_vaddr <= vaddr
                and vaddr + size <= segment_vaddr + file_size
            ):
                offset = file_offset + vaddr - segment_vaddr
                stream.seek(offset)
                payload = stream.read(size)
                if len(payload) != size:
                    fail(os.path.basename(path) + " has a truncated data window")
                return payload
    fail(os.path.basename(path) + " does not contain the expected data window")


def prepare_runtime_windows(stage, game_dir):
    patch_dir = os.path.join(game_dir, "tools", "patches")
    if not os.path.isdir(patch_dir) or os.path.islink(patch_dir):
        fail("the version-pinned transformation masks are missing or unsafe")

    print("[prepare] reconstructing version-pinned runtime windows")
    destination_dir = os.path.join(stage, "lib")
    for name, details in sorted(WINDOWS.items()):
        source_relative, vaddr, size, output_hash, mask_hash = details
        source = os.path.join(stage, *source_relative.split("/"))
        mask_path = os.path.join(patch_dir, name + ".xormask")
        validate_file(mask_path, size, mask_hash, "transformation mask " + name)
        encrypted = file_window_for_vaddr(source, vaddr, size)
        with open(mask_path, "rb") as stream:
            mask = stream.read()
        reconstructed = bytes(left ^ right for left, right in zip(encrypted, mask))
        if hashlib.sha256(reconstructed).hexdigest() != output_hash:
            fail("runtime window %s did not match the supported APK" % name)

        destination = os.path.join(destination_dir, name)
        temporary = destination + ".nxpart"
        if os.path.lexists(temporary):
            if os.path.islink(temporary) or not contained(stage, temporary):
                fail("unsafe temporary runtime-window path")
            os.unlink(temporary)
        with open(temporary, "wb") as stream:
            stream.write(reconstructed)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
        validate_file(destination, size, output_hash, "prepared " + name)


def prepare_shaders(stage, game_dir):
    source = os.path.join(stage, "assets", "bin", "Data", "data.unity3d")
    expected = ORIGINAL_FILES["assets/bin/Data/data.unity3d"]
    validate_file(source, expected[0], expected[1], "pristine data.unity3d")

    tools_dir = os.path.join(game_dir, "tools")
    vendor_dir = os.path.join(tools_dir, "vendor", "python")
    lz4_path = os.path.join(tools_dir, "liblz4.so.1")
    validate_file(
        lz4_path,
        BUNDLED_LZ4_SIZE,
        BUNDLED_LZ4_SHA256,
        "bundled liblz4.so.1",
    )
    if not os.path.isdir(vendor_dir) or os.path.islink(vendor_dir):
        fail("the vendored Python runtime is missing or unsafe")

    sys.path.insert(0, vendor_dir)
    sys.path.insert(0, tools_dir)
    os.environ["PF2_LZ4_LIBRARY"] = lz4_path
    try:
        import pf2_transpile_shaders as transpiler
    except Exception as error:
        fail("could not load the GLES2 transpiler: %s" % error)

    temporary = source + ".nxpart"
    if os.path.lexists(temporary):
        if os.path.islink(temporary):
            fail("unsafe temporary shader path")
        os.unlink(temporary)
    print("[prepare] creating GLES2 shader variants from owner data")
    try:
        stats = transpiler.process(Path(source), Path(temporary))
        if (
            stats.shaders_seen != 45
            or stats.shaders_patched != 35
            or stats.programs_converted != 193
            or stats.variants_added != 534
            or stats.passes_enabled != 78
        ):
            fail(
                "unexpected shader result %d/%d shaders, %d programs, "
                "%d variants, %d passes"
                % (
                    stats.shaders_patched,
                    stats.shaders_seen,
                    stats.programs_converted,
                    stats.variants_added,
                    stats.passes_enabled,
                )
            )
        transpiler.verify_output(Path(temporary))
    except PreparationError:
        raise
    except Exception as error:
        fail("GLES2 shader preparation failed: %s" % error)
    if not regular_file(temporary):
        fail("the GLES2 transpiler did not create its output")
    output_size = os.path.getsize(temporary)
    if not 3700000 <= output_size <= 4100000:
        fail("prepared data.unity3d has unexpected size %d" % output_size)
    os.replace(temporary, source)
    return output_size, sha256_file(source)


def write_marker(stage, shader_size, shader_hash):
    overlays = {name: details[3] for name, details in sorted(WINDOWS.items())}
    document = {
        "format": 1,
        "game_version": GAME_VERSION,
        "package": PACKAGE,
        "preparation": PREPARATION,
        "shader_sha256": shader_hash,
        "shader_size": shader_size,
        "overlays": overlays,
    }
    target = os.path.join(stage, MARKER)
    temporary = target + ".nxpart"
    with open(temporary, "w", encoding="utf-8") as stream:
        json.dump(document, stream, sort_keys=True, separators=(",", ":"))
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--game-dir", required=True)
    args = parser.parse_args()

    stage = os.path.realpath(args.stage)
    game_dir = os.path.realpath(args.game_dir)
    if not os.path.isdir(stage) or os.path.islink(args.stage):
        fail("the NXExtract stage is missing or unsafe")
    if not os.path.isdir(game_dir):
        fail("the port directory is missing")

    validate_originals(stage)
    prepare_runtime_windows(stage, game_dir)
    shader_size, shader_hash = prepare_shaders(stage, game_dir)
    write_marker(stage, shader_size, shader_hash)
    print(
        "[prepare] PF2 owner data ready: shader=%s size=%d"
        % (shader_hash, shader_size)
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PreparationError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
