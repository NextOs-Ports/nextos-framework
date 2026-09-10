#!/usr/bin/env python3
"""Prepare Stardew Valley 1.6.15.3 data for the Linux controller port.

The Android assembly store is kept intact as an ELF/XABA container.  Only the
known StardewValley.dll payload is patched, recompressed into its existing
slot, verified after the write, and atomically published.  No game data is
shipped by this project.
"""

from __future__ import print_function

import argparse
import ctypes
import ctypes.util
import hashlib
import json
import os
import stat
import struct
import sys


STORE_RELATIVE = os.path.join("libs", "libassemblies.arm64-v8a.blob.so")
DATA_MARKER = ".stardew-data.json"
PACKAGE = "com.chucklefish.stardewvalley"
GAME_VERSION = "1.6.15.3"
BUNDLED_LZ4_SHA256 = (
    "a65c53e2e7015b636e4f212449eff2016b99736cdf5798fe2cf3672818b88b8b"
)

ORIGINAL_ASSEMBLY_SHA256 = (
    "09a67870cbc4a31fa255b534012f3f55809725c5588892c2431eadfcd71cfbee"
)
LEGACY_PATCHED_ASSEMBLY_SHA256 = (
    "d04b9507f41103b2dbf2f512fe7a54c474a88d7bb830ed2d81a42d409d7e5465"
)
CONTROLLER_V1_ASSEMBLY_SHA256 = (
    "d71f7a1f44f3ad9874277f59c5ff0432c73b22d178ae966d491a622f2daf8586"
)
PREVIOUS_FINAL_ASSEMBLY_SHA256 = (
    "d7749b430a36df08c353d3acae2d532070e771caa9baf43e6df08d07f0ffe417"
)
RELEASE_116_ASSEMBLY_SHA256 = (
    "484633c3a22a72b25be9f6cff603ed67a7f7f154f9ce3b7a36df94792eaccd8b"
)
FINAL_ASSEMBLY_SHA256 = (
    "c51dd147461ff4e594ad2251491c2eb34ff0851f7db87bbeccef276d46d0b31f"
)

XABA_MAGIC = b"XABA"
XALZ_MAGIC = b"XALZ"
XABA_HEADER = struct.Struct("<4sIIII")
XABA_DESCRIPTOR = struct.Struct("<7I")
XALZ_HEADER = struct.Struct("<4sII")


PATCH_REGIONS = (
    (
        0x2DAFEC,
        bytes.fromhex(
            "172a" + "00" * 66
        ),
        {
            ORIGINAL_ASSEMBLY_SHA256: bytes.fromhex(
                "7eaf1500047234e20a70176f234d000628cc2f00066f6f3500066fa9370006"
                "7e7f150004166f983c00067e7f1500046f973c00067eaf1500047234e20a70"
                "6f244d00062a"
            ),
            LEGACY_PATCHED_ASSEMBLY_SHA256: bytes.fromhex(
                "172a" + "00" * 66
            ),
            CONTROLLER_V1_ASSEMBLY_SHA256: bytes.fromhex(
                "172a" + "00" * 66
            ),
            PREVIOUS_FINAL_ASSEMBLY_SHA256: bytes.fromhex(
                "172a" + "00" * 66
            ),
        },
    ),
    (
        0x4D4BA2,
        bytes.fromhex(
            "7e473600042c012a02032894610006281e3000067ba51e00042c0403152e0403"
            "173306031f645a1001027b3b390004036f425e00062a"
        ),
        {
            ORIGINAL_ASSEMBLY_SHA256: bytes.fromhex(
                "7e473600042c012a02032894610006281e3000067ba51e00042c0403152e0403"
                "173306031f645a1001027b3b390004036f425e00062a"
            ),
            LEGACY_PATCHED_ASSEMBLY_SHA256: bytes.fromhex(
                "7e473600042c012a02032894610006281e3000067ba51e00042c0403152e0403"
                "173306031f645a1001027b3b390004036f425e00062a"
            ),
            CONTROLLER_V1_ASSEMBLY_SHA256: bytes.fromhex(
                "031f262e18031f282e1703172e0f03152e0f031f642f0e031f9c31092a1f64"
                "2b021f9c1001027b3b390004036f425e00062a00000000"
            ),
            PREVIOUS_FINAL_ASSEMBLY_SHA256: bytes.fromhex(
                "7e473600042c012a02032894610006281e3000067ba51e00042c0403152e0403"
                "173306031f645a1001027b3b390004036f425e00062a"
            ),
        },
    ),
    (
        0x4F9AD4,
        bytes.fromhex(
            "02285965000602285b650006282803000a2c0b027282030070285a6500060228"
            "6d3000062a00000000000000000000000000"
        ),
        {
            ORIGINAL_ASSEMBLY_SHA256: bytes.fromhex(
                "7282030070728203007002285965000616280120000a0a0602fe067165000673"
                "5921000a6f8202002b26021628706500062a"
            ),
            LEGACY_PATCHED_ASSEMBLY_SHA256: bytes.fromhex(
                "02286d3000062a" + "00" * 43
            ),
            CONTROLLER_V1_ASSEMBLY_SHA256: bytes.fromhex(
                "02285965000602285b650006282803000a2c0b027282030070285a6500060228"
                "6d3000062a00000000000000000000000000"
            ),
            PREVIOUS_FINAL_ASSEMBLY_SHA256: bytes.fromhex(
                "02285965000602285b650006282803000a2c0b027282030070285a6500060228"
                "6d3000062a00000000000000000000000000"
            ),
        },
    ),
    (
        0x6128E2,
        bytes.fromhex("9f694d00"),
        {
            ORIGINAL_ASSEMBLY_SHA256: bytes.fromhex("9f694d00"),
            LEGACY_PATCHED_ASSEMBLY_SHA256: bytes.fromhex("9f694d00"),
            CONTROLLER_V1_ASSEMBLY_SHA256: bytes.fromhex("a1694d00"),
            PREVIOUS_FINAL_ASSEMBLY_SHA256: bytes.fromhex("9f694d00"),
        },
    ),
    (
        0x610194,
        bytes.fromhex("a4d78d00"),
        {
            ORIGINAL_ASSEMBLY_SHA256: bytes.fromhex("61ae4a00"),
            LEGACY_PATCHED_ASSEMBLY_SHA256: bytes.fromhex("61ae4a00"),
            CONTROLLER_V1_ASSEMBLY_SHA256: bytes.fromhex("61ae4a00"),
            PREVIOUS_FINAL_ASSEMBLY_SHA256: bytes.fromhex("a4d78d00"),
        },
    ),
    (
        0x6103E6,
        bytes.fromhex("61ae4a00"),
        {
            ORIGINAL_ASSEMBLY_SHA256: bytes.fromhex("3dc24a00"),
            LEGACY_PATCHED_ASSEMBLY_SHA256: bytes.fromhex("3dc24a00"),
            CONTROLLER_V1_ASSEMBLY_SHA256: bytes.fromhex("3dc24a00"),
            PREVIOUS_FINAL_ASSEMBLY_SHA256: bytes.fromhex("61ae4a00"),
        },
    ),
    (
        0x8DB9A4,
        bytes.fromhex(
            "b6027535070002252c0b03172e1003182e14262b0126020328946100062a1f64"
            "6fa36300062a1f9c6fa36300062a"
        ),
        {
            ORIGINAL_ASSEMBLY_SHA256: bytes(46),
            LEGACY_PATCHED_ASSEMBLY_SHA256: bytes(46),
            CONTROLLER_V1_ASSEMBLY_SHA256: bytes(46),
            PREVIOUS_FINAL_ASSEMBLY_SHA256: bytes.fromhex(
                "b6027535070002252c0b03172e1003182e14262b0126020328946100062a1f64"
                "6fa36300062a1f9c6fa36300062a"
            ),
        },
    ),
    (
        0x301ABF,
        bytes.fromhex("220000403f7dc41e0004"),
        {
            ORIGINAL_ASSEMBLY_SHA256: bytes.fromhex("220000803f7dc41e0004"),
            LEGACY_PATCHED_ASSEMBLY_SHA256: bytes.fromhex("220000803f7dc41e0004"),
            CONTROLLER_V1_ASSEMBLY_SHA256: bytes.fromhex("220000803f7dc41e0004"),
            PREVIOUS_FINAL_ASSEMBLY_SHA256: bytes.fromhex("220000803f7dc41e0004"),
        },
    ),
    (
        0x3021D5,
        bytes.fromhex("220000403f7dc31e0004"),
        {
            ORIGINAL_ASSEMBLY_SHA256: bytes.fromhex("220000803f7dc31e0004"),
            LEGACY_PATCHED_ASSEMBLY_SHA256: bytes.fromhex("220000803f7dc31e0004"),
            CONTROLLER_V1_ASSEMBLY_SHA256: bytes.fromhex("220000803f7dc31e0004"),
            PREVIOUS_FINAL_ASSEMBLY_SHA256: bytes.fromhex("220000803f7dc31e0004"),
        },
    ),
    # Game1.UpdateControlInput, mobile hold-to-repeat: the gamepad X-held
    # branch re-fires pressUseToolButton every tick with no time gate, so an
    # empty hand near furniture toggles pick-up/place ("turbo"). The guard
    # only skipped MeleeWeapon; brfalse.s on CurrentTool arms the repeat for
    # equipped tools only.  75d5040002 2d03 (isinst MeleeWeapon; brtrue.s)
    # -> 2c08 (brfalse.s) + 5 nops; ldc.i4.1/stloc.s untouched.
    (
        0x2289A8,
        bytes.fromhex("2c080000000000"),
        {
            ORIGINAL_ASSEMBLY_SHA256: bytes.fromhex("75d50400022d03"),
            LEGACY_PATCHED_ASSEMBLY_SHA256: bytes.fromhex("75d50400022d03"),
            CONTROLLER_V1_ASSEMBLY_SHA256: bytes.fromhex("75d50400022d03"),
            PREVIOUS_FINAL_ASSEMBLY_SHA256: bytes.fromhex("75d50400022d03"),
            RELEASE_116_ASSEMBLY_SHA256: bytes.fromhex("75d50400022d03"),
        },
    ),
)


def fail(message):
    raise RuntimeError("Stardew Valley data preparation failed: " + message)


def sha256_bytes(value):
    return hashlib.sha256(value).hexdigest()


def regular_file(path):
    try:
        return stat.S_ISREG(os.lstat(path).st_mode)
    except OSError:
        return False


def contained(root, path):
    root = os.path.realpath(root)
    path = os.path.realpath(path)
    return path == root or path.startswith(root + os.sep)


class Lz4(object):
    def __init__(self):
        candidates = []
        bundled = os.path.join(
            os.path.dirname(os.path.realpath(__file__)), "liblz4.so.1"
        )
        if regular_file(bundled):
            with open(bundled, "rb") as stream:
                bundled_hash = sha256_bytes(stream.read())
            if bundled_hash != BUNDLED_LZ4_SHA256:
                fail("bundled liblz4 SHA-256 mismatch: " + bundled_hash)
            candidates.append(bundled)
        discovered = ctypes.util.find_library("lz4")
        if discovered:
            candidates.append(discovered)
        candidates.extend(("liblz4.so.1", "liblz4.so"))
        self.library = None
        errors = []
        for candidate in candidates:
            try:
                self.library = ctypes.CDLL(candidate)
                break
            except OSError as error:
                errors.append(str(error))
        if self.library is None:
            fail("liblz4 is unavailable (%s)" % "; ".join(errors))

        self.library.LZ4_decompress_safe.argtypes = (
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
        )
        self.library.LZ4_decompress_safe.restype = ctypes.c_int
        self.library.LZ4_compressBound.argtypes = (ctypes.c_int,)
        self.library.LZ4_compressBound.restype = ctypes.c_int
        try:
            compressor = self.library.LZ4_compress_HC
            compressor.argtypes = (
                ctypes.c_void_p,
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_int,
            )
            compressor.restype = ctypes.c_int
            self.compress_hc = compressor
        except AttributeError:
            fail("liblz4 has no high-compression API")

    def decompress(self, source, output_size):
        source_buffer = ctypes.create_string_buffer(source, len(source))
        output = ctypes.create_string_buffer(output_size)
        result = self.library.LZ4_decompress_safe(
            source_buffer, output, len(source), output_size
        )
        if result != output_size:
            fail("XALZ decompression size mismatch")
        return output.raw[:output_size]

    def compress(self, source):
        bound = self.library.LZ4_compressBound(len(source))
        if bound <= 0:
            fail("liblz4 rejected the assembly size")
        source_buffer = ctypes.create_string_buffer(source, len(source))
        output = ctypes.create_string_buffer(bound)
        result = self.compress_hc(
            source_buffer, output, len(source), bound, 12
        )
        if result <= 0:
            fail("liblz4 could not recompress the assembly")
        return output.raw[:result]


def rva_to_file_offset(image, rva):
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    section_count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    section = pe + 24 + optional_size
    for _index in range(section_count):
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", image, section + 8
        )
        span = max(virtual_size, raw_size)
        if virtual_address <= rva < virtual_address + span:
            return raw_offset + rva - virtual_address
        section += 40
    fail("PE RVA is outside every section")


def strip_debug_directory(image, source_hash):
    known_data_offset = 0x8DB9A4
    known_data_size = 0x8B
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", image, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    directory = directories + 6 * 8
    debug_rva, debug_size = struct.unpack_from("<II", image, directory)
    if not debug_rva:
        expected = bytes(known_data_size)
        if source_hash in (
            PREVIOUS_FINAL_ASSEMBLY_SHA256,
            RELEASE_116_ASSEMBLY_SHA256,
        ):
            controller_code = bytes.fromhex(
                "b6027535070002252c0b03172e1003182e14262b0126020328946100062a1f64"
                "6fa36300062a1f9c6fa36300062a"
            )
            expected = controller_code + bytes(
                known_data_size - len(controller_code)
            )
        if image[known_data_offset : known_data_offset + known_data_size] != expected:
            fail("stripped CodeView code cave is not clean")
        return
    if debug_size < 28:
        fail("expected CodeView debug directory is invalid")
    debug = rva_to_file_offset(image, debug_rva)
    found = False
    for entry in range(debug, debug + debug_size, 28):
        if entry + 28 > debug + debug_size:
            break
        debug_type = struct.unpack_from("<I", image, entry + 12)[0]
        size = struct.unpack_from("<I", image, entry + 16)[0]
        data = struct.unpack_from("<I", image, entry + 24)[0]
        if (
            debug_type == 2
            and size >= 24
            and data + size <= len(image)
            and image[data : data + 4] == b"RSDS"
        ):
            image[data : data + size] = b"\0" * size
            found = True
    if not found:
        fail("expected CodeView record is missing")
    image[debug : debug + debug_size] = b"\0" * debug_size
    image[directory : directory + 8] = b"\0" * 8


def patch_assembly(assembly):
    source_hash = sha256_bytes(assembly)
    if source_hash == FINAL_ASSEMBLY_SHA256:
        return assembly, False
    if source_hash not in (
        ORIGINAL_ASSEMBLY_SHA256,
        LEGACY_PATCHED_ASSEMBLY_SHA256,
        CONTROLLER_V1_ASSEMBLY_SHA256,
        PREVIOUS_FINAL_ASSEMBLY_SHA256,
        RELEASE_116_ASSEMBLY_SHA256,
    ):
        fail("unsupported StardewValley.dll SHA-256 " + source_hash)

    output = bytearray(assembly)
    strip_debug_directory(output, source_hash)
    for offset, replacement, expected_by_hash in PATCH_REGIONS:
        # A source state missing from the map already carries the final bytes
        # for that region (RELEASE_116 shipped every earlier patch verbatim).
        expected = expected_by_hash.get(source_hash, replacement)
        if output[offset : offset + len(expected)] != expected:
            fail("assembly patch precondition failed at 0x%x" % offset)
        if len(replacement) != len(expected):
            fail("internal patch size mismatch at 0x%x" % offset)
        output[offset : offset + len(replacement)] = replacement
    result = bytes(output)
    actual = sha256_bytes(result)
    if actual != FINAL_ASSEMBLY_SHA256:
        fail("prepared assembly SHA-256 mismatch: " + actual)
    return result, True


def decode_entry(blob, store_offset, data_offset, data_size, lz4):
    start = store_offset + data_offset
    entry = bytes(blob[start : start + data_size])
    if entry[:4] != XALZ_MAGIC:
        return entry, None
    _magic, mapping, unpacked_size = XALZ_HEADER.unpack_from(entry)
    return lz4.decompress(entry[XALZ_HEADER.size :], unpacked_size), mapping


def prepare_store(store_bytes):
    blob = bytearray(store_bytes)
    store_offset = blob.find(XABA_MAGIC)
    if store_offset < 0 or blob.find(XABA_MAGIC, store_offset + 1) >= 0:
        fail("expected one XABA store")
    magic, version, entry_count, index_count, index_size = XABA_HEADER.unpack_from(
        blob, store_offset
    )
    if magic != XABA_MAGIC or version != 0x80010002:
        fail("unsupported XABA store version")
    if entry_count <= 0 or entry_count > 1024 or index_size != index_count * 12:
        fail("unexpected XABA index layout")

    lz4 = Lz4()
    descriptors = store_offset + XABA_HEADER.size + index_size
    recognized = {
        ORIGINAL_ASSEMBLY_SHA256,
        LEGACY_PATCHED_ASSEMBLY_SHA256,
        CONTROLLER_V1_ASSEMBLY_SHA256,
        PREVIOUS_FINAL_ASSEMBLY_SHA256,
        RELEASE_116_ASSEMBLY_SHA256,
        FINAL_ASSEMBLY_SHA256,
    }
    match = None
    entry_offsets = []
    for index in range(entry_count):
        descriptor = descriptors + index * XABA_DESCRIPTOR.size
        mapping, data_offset, data_size = XABA_DESCRIPTOR.unpack_from(
            blob, descriptor
        )[:3]
        entry_offsets.append((data_offset, data_size))
        decoded, compressed_mapping = decode_entry(
            blob, store_offset, data_offset, data_size, lz4
        )
        digest = sha256_bytes(decoded)
        if digest not in recognized:
            continue
        if match is not None:
            fail("StardewValley.dll appears more than once in the store")
        match = (
            index,
            descriptor,
            mapping,
            data_offset,
            data_size,
            compressed_mapping,
            decoded,
        )
    if match is None:
        fail("supported StardewValley.dll was not found in the XABA store")

    (
        index,
        descriptor,
        mapping,
        data_offset,
        old_size,
        compressed_mapping,
        assembly,
    ) = match
    prepared, changed = patch_assembly(assembly)
    if not changed:
        return bytes(blob), False, index

    # The descriptor stores the current compressed length, not the physical
    # capacity reserved before the next XABA entry. Earlier controller patches
    # correctly shortened that length and zeroed the remaining slot. Reuse the
    # content-addressed padding when migrating such an install instead of
    # rejecting a valid recompression that is only a few bytes larger.
    following_offsets = [
        offset for offset, _size in entry_offsets if offset > data_offset
    ]
    if not following_offsets:
        fail("StardewValley.dll XABA entry has no bounded following slot")
    slot_capacity = min(following_offsets) - data_offset
    if slot_capacity < old_size or slot_capacity - old_size > 1048576:
        fail("StardewValley.dll XABA slot capacity is invalid")

    if compressed_mapping is None:
        encoded = prepared
    else:
        encoded = XALZ_HEADER.pack(
            XALZ_MAGIC, compressed_mapping, len(prepared)
        ) + lz4.compress(prepared)
    if len(encoded) > slot_capacity:
        fail(
            "prepared assembly does not fit its XABA slot (%d > %d)"
            % (len(encoded), slot_capacity)
        )

    start = store_offset + data_offset
    blob[start : start + len(encoded)] = encoded
    blob[start + len(encoded) : start + slot_capacity] = b"\0" * (
        slot_capacity - len(encoded)
    )
    struct.pack_into("<I", blob, descriptor + 8, len(encoded))
    verified, _mapping = decode_entry(
        blob, store_offset, data_offset, len(encoded), lz4
    )
    if sha256_bytes(verified) != FINAL_ASSEMBLY_SHA256:
        fail("in-memory XABA verification failed")
    return bytes(blob), True, index


def atomic_write(path, data, mode):
    temporary = path + ".nxpart"
    try:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
        directory = os.open(os.path.dirname(path), os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def marker_bytes():
    value = {
        "assembly_sha256": FINAL_ASSEMBLY_SHA256,
        "format": 1,
        "package": PACKAGE,
        "store": STORE_RELATIVE.replace(os.sep, "/"),
        "version": GAME_VERSION,
    }
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode(
        "utf-8"
    )


def prepare_root(root):
    root = os.path.realpath(root)
    if not os.path.isdir(root) or os.path.islink(root):
        fail("stage/game directory is missing or linked")
    store = os.path.join(root, STORE_RELATIVE)
    marker = os.path.join(root, DATA_MARKER)
    if not contained(root, store) or not regular_file(store):
        fail("assembly store is missing or unsafe")

    with open(store, "rb") as stream:
        original = stream.read()
    prepared, changed, index = prepare_store(original)
    if changed:
        mode = stat.S_IMODE(os.stat(store).st_mode)
        atomic_write(store, prepared, mode)
    with open(store, "rb") as stream:
        verified_store = stream.read()
    _unchanged, second_change, verified_index = prepare_store(verified_store)
    if second_change or verified_index != index:
        fail("published XABA store did not verify")
    atomic_write(marker, marker_bytes(), 0o644)
    print(
        "NXEXTRACT_PROGRESS 1000 1000 STARDEW DATA READY ENTRY %d" % index,
        flush=True,
    )
    print(
        "Stardew Valley data ready: assembly %s%s"
        % (FINAL_ASSEMBLY_SHA256, " (updated)" if changed else ""),
        flush=True,
    )


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--stage",
        default=os.environ.get("NXEXTRACT_STAGE"),
        help="NXExtract stage or an existing game directory",
    )
    args = parser.parse_args(argv)
    if not args.stage:
        parser.error("--stage or NXEXTRACT_STAGE is required")
    prepare_root(args.stage)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, struct.error) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
