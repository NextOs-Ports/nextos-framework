#!/usr/bin/env python3
"""Prepare NXExtract-authenticated Party Hard GO owner data.

NXExtract authenticates the ARM64 payload before this hook runs.  The hook
unbundles the Play Asset Delivery pack `assets/bin/Data/datapack.unity3d`
into the seven loose serialized files the Unity engine already looks for
(`resources.assets`, `.resS`, `sharedassets1/2`, `level1`, `level2`), byte for
byte as they are stored inside the container, and then removes the container
from the stage.  Nothing is reserialized and nothing is written outside
NXExtract's disposable stage.  No APK, game asset or binary delta is
distributed.
"""

from __future__ import print_function

import argparse
import json
import os
import stat
import struct
import sys


FORMAT = 1
PACKAGE = "com.tinybuild.PartyHardGO"
PREPARATION = "partyhard-datapack-loose-layout-v1"
MARKER = ".partyhard-data.json"
DATAPACK = "assets/bin/Data/datapack.unity3d"
DATA_DIR = "assets/bin/Data"
LZ4_RELATIVE = "nxextract/lib/aarch64/liblz4.so.1"
WRITE_CHUNK_SIZE = 1024 * 1024
UNITYFS_FORMAT = 8
UNITYFS_COMPRESSION_MASK = 0x3F
UNITYFS_DIRECTORY_COMBINED = 0x40
UNITYFS_BLOCK_INFO_AT_END = 0x80
UNITYFS_BLOCK_PADDING = 0x200
UNITYFS_ENCRYPTION_FLAGS = 0x1400
MAX_METADATA_BYTES = 16 * 1024 * 1024
MAX_BLOCKS = 65536
MAX_BLOCK_BYTES = 16 * 1024 * 1024
MAX_NAME_BYTES = 4096

# The seven loose files of the approved Mali-450 port, exactly as the
# container stores them (sizes).  Their content identity is closed by the
# recipe checkpoint (exact files/bytes and the CRC tree fingerprint of the
# staged assets/bin tree), never by a hash literal in hook logic; a different
# container content is refused before a byte is published.
EXPECTED_ENTRIES = {
    "level1": 111236,
    "level2": 69124,
    "resources.assets": 40255596,
    "resources.assets.resS": 247103760,
    "sharedassets1.assets": 161676,
    "sharedassets1.assets.resS": 156016,
    "sharedassets2.assets": 1101,
}


class PreparationError(RuntimeError):
    pass


def fail(message):
    raise PreparationError("Party Hard GO preparation failed: " + message)


def regular_file(path):
    try:
        return stat.S_ISREG(os.lstat(path).st_mode)
    except OSError:
        return False


def safe_relative(value, label):
    if not isinstance(value, str) or not value or "\\" in value or "\0" in value:
        fail("invalid %s" % label)
    parts = value.split("/")
    if value.startswith("/") or any(part in ("", ".", "..") for part in parts):
        fail("unsafe %s %r" % (label, value))
    return value


def inside(root, path):
    root = os.path.realpath(root)
    path = os.path.realpath(path)
    return path == root or path.startswith(root + os.sep)


def stage_path(stage, relative):
    relative = safe_relative(relative, "stage path")
    path = os.path.abspath(os.path.join(stage, *relative.split("/")))
    if not inside(stage, path):
        fail("stage path escaped its root")
    return path


def read_exact(stream, size, label):
    if size < 0:
        fail("negative %s size" % label)
    payload = stream.read(size)
    if len(payload) != size:
        fail("truncated %s" % label)
    return payload


def read_cstring(stream, label):
    payload = bytearray()
    while len(payload) <= MAX_NAME_BYTES:
        value = stream.read(1)
        if not value:
            fail("truncated %s" % label)
        if value == b"\0":
            try:
                return payload.decode("utf-8")
            except UnicodeDecodeError:
                fail("invalid UTF-8 in %s" % label)
        payload.extend(value)
    fail("oversized %s" % label)


def align_offset(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


class MetadataReader(object):
    def __init__(self, payload):
        self.payload = memoryview(payload)
        self.position = 0

    def take(self, size, label):
        if size < 0 or self.position + size > len(self.payload):
            fail("truncated UnityFS %s" % label)
        start = self.position
        self.position += size
        return self.payload[start:self.position]

    def unpack(self, format_, size, label):
        value = struct.unpack(">" + format_, self.take(size, label))[0]
        return value

    def u16(self, label):
        return self.unpack("H", 2, label)

    def u32(self, label):
        return self.unpack("I", 4, label)

    def u64(self, label):
        return self.unpack("Q", 8, label)

    def cstring(self, label):
        start = self.position
        limit = min(len(self.payload), start + MAX_NAME_BYTES + 1)
        while self.position < limit:
            value = self.payload[self.position]
            self.position += 1
            if value == 0:
                try:
                    return bytes(self.payload[start:self.position - 1]).decode("utf-8")
                except UnicodeDecodeError:
                    fail("invalid UTF-8 in UnityFS %s" % label)
        fail("unterminated or oversized UnityFS %s" % label)

    def finish(self):
        trailing = self.payload[self.position:]
        if any(trailing):
            fail("unexpected trailing UnityFS metadata")


def atomic_buffer(path, payload):
    temporary = "%s.nxpart.%d" % (path, os.getpid())
    if os.path.lexists(temporary):
        fail("stale or unsafe temporary output")
    view = memoryview(payload)
    try:
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb", buffering=0) as stream:
            offset = 0
            while offset < len(view):
                written = stream.write(view[offset : offset + WRITE_CHUNK_SIZE])
                if not written:
                    fail("short write while publishing prepared owner data")
                offset += written
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        view.release()
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def atomic_bytes(path, payload):
    atomic_buffer(path, payload)


def validate_owner_input(stage):
    assets = stage_path(stage, "assets")
    if not os.path.isdir(assets) or os.path.islink(assets):
        fail("NXExtract-authenticated owner asset tree is missing or unsafe")
    library_root = stage_path(stage, "lib")
    if not os.path.isdir(library_root) or os.path.islink(library_root):
        fail("ARM64 library directory is missing or unsafe")
    actual_names = sorted(os.listdir(library_root))
    expected_names = ["libil2cpp.so", "libmain.so", "libunity.so"]
    if actual_names != expected_names:
        fail("NXExtract-authenticated ARM64 library set changed")
    for name in expected_names:
        path = os.path.join(library_root, name)
        if not regular_file(path) or os.path.islink(path):
            fail("NXExtract-authenticated owner library is missing or unsafe: " + name)
    datapack = stage_path(stage, DATAPACK)
    if not regular_file(datapack) or os.path.islink(datapack):
        fail("owner asset pack datapack.unity3d is missing or unsafe")
    for name in ("data.unity3d", "boot.config", "unity_app_guid",
                 "Managed/Metadata/global-metadata.dat"):
        path = stage_path(stage, DATA_DIR + "/" + name)
        if not regular_file(path) or os.path.islink(path):
            fail("owner Unity data is missing or unsafe: " + name)


def reject_stale_python_cache(game_dir):
    root = os.path.join(game_dir, "nxextract")
    if not os.path.isdir(root) or os.path.islink(root):
        fail("packaged Python root is missing or unsafe")
    for current, directories, files in os.walk(root, followlinks=False):
        for name in directories:
            path = os.path.join(current, name)
            if name == "__pycache__":
                fail("stale Python bytecode cache is forbidden")
            if os.path.islink(path):
                fail("packaged Python path contains a symbolic link")
        for name in files:
            if name.endswith(".pyc"):
                fail("stale Python bytecode is forbidden")


def safe_packaged_file(game_dir, relative, label):
    relative = safe_relative(relative, label + " path")
    cursor = game_dir
    parts = relative.split("/")
    for index, part in enumerate(parts):
        cursor = os.path.join(cursor, part)
        try:
            mode = os.lstat(cursor).st_mode
        except OSError:
            fail("%s is missing or unsafe" % label)
        if stat.S_ISLNK(mode):
            fail("%s path contains a symbolic link" % label)
        if index < len(parts) - 1 and not stat.S_ISDIR(mode):
            fail("%s parent is not a directory" % label)
    if not regular_file(cursor) or os.path.islink(cursor):
        fail("%s is missing or unsafe" % label)
    if not inside(game_dir, cursor):
        fail("%s must be supplied under the port directory" % label)
    return os.path.realpath(cursor)


def configure_lz4(game_dir):
    reject_stale_python_cache(game_dir)
    vendor = os.path.join(game_dir, "nxextract", "vendor", "python")
    if not os.path.isdir(vendor) or os.path.islink(vendor):
        fail("vendored Python runtime is missing or unsafe")
    lz4_path = safe_packaged_file(game_dir, LZ4_RELATIVE, "packaged LZ4 runtime")
    sys.path.insert(0, vendor)
    # The vendored ctypes LZ4 shim looks at this variable first and falls back
    # to a system liblz4 (host-side recipe checks) when the packaged AArch64
    # object cannot be loaded on the current machine.
    os.environ["PF2_LZ4_LIBRARY"] = lz4_path
    try:
        import lz4.block
    except Exception as error:
        fail("could not load the pinned LZ4 preparation runtime: %s" % error)
    return lz4.block


def progress(done, total, text):
    print("NXEXTRACT_PROGRESS %d %d %s" % (done, total, text))
    sys.stdout.flush()


def decompress_unityfs(payload, expected_size, flags, lz4_block, label):
    mode = flags & UNITYFS_COMPRESSION_MASK
    if expected_size < 0 or expected_size > MAX_BLOCK_BYTES:
        fail("unsafe uncompressed size for %s" % label)
    if mode == 0:
        if len(payload) != expected_size:
            fail("stored %s size differs from its UnityFS declaration" % label)
        return payload
    if mode not in (2, 3):
        fail("unsupported UnityFS compression %d for %s" % (mode, label))
    try:
        result = lz4_block.decompress(payload, expected_size)
    except Exception as error:
        fail("cannot decompress %s: %s" % (label, error))
    if len(result) != expected_size:
        fail("decompressed %s size differs from its UnityFS declaration" % label)
    return result


def parse_unityfs(path, lz4_block):
    actual_size = os.path.getsize(path)
    with open(path, "rb") as stream:
        if read_cstring(stream, "UnityFS signature") != "UnityFS":
            fail("datapack.unity3d is not a UnityFS container")
        version = struct.unpack(">I", read_exact(stream, 4, "UnityFS format"))[0]
        if version != UNITYFS_FORMAT:
            fail("unsupported UnityFS format %d" % version)
        read_cstring(stream, "UnityFS player version")
        read_cstring(stream, "UnityFS engine version")
        header = struct.unpack(">QIII", read_exact(stream, 20, "UnityFS header"))
        declared_size, info_compressed_size, info_size, archive_flags = header
        if declared_size != actual_size:
            fail("UnityFS declared size differs from datapack.unity3d")
        if not (archive_flags & UNITYFS_DIRECTORY_COMBINED):
            fail("UnityFS directory metadata is not combined with block metadata")
        if archive_flags & UNITYFS_ENCRYPTION_FLAGS:
            fail("encrypted UnityFS containers are unsupported")
        known_flags = (
            UNITYFS_COMPRESSION_MASK
            | UNITYFS_DIRECTORY_COMBINED
            | UNITYFS_BLOCK_INFO_AT_END
            | 0x100
            | UNITYFS_BLOCK_PADDING
        )
        if archive_flags & ~known_flags:
            fail("UnityFS archive uses unsupported flags")
        if not (0 < info_compressed_size <= MAX_METADATA_BYTES):
            fail("unsafe compressed UnityFS metadata size")
        if not (0 < info_size <= MAX_METADATA_BYTES):
            fail("unsafe UnityFS metadata size")

        first_payload_offset = align_offset(stream.tell(), 16)
        if first_payload_offset > actual_size:
            fail("UnityFS header exceeds the container")
        if archive_flags & UNITYFS_BLOCK_INFO_AT_END:
            info_offset = declared_size - info_compressed_size
        else:
            info_offset = first_payload_offset
        if info_offset < first_payload_offset or info_offset > declared_size:
            fail("unsafe UnityFS metadata offset")
        stream.seek(info_offset)
        compressed_info = read_exact(
            stream, info_compressed_size, "UnityFS block metadata"
        )
        info = decompress_unityfs(
            compressed_info,
            info_size,
            archive_flags,
            lz4_block,
            "UnityFS block metadata",
        )

    metadata = MetadataReader(info)
    metadata.take(16, "content hash")
    block_count = metadata.u32("block count")
    if not (0 < block_count <= MAX_BLOCKS):
        fail("unsafe UnityFS block count")
    blocks = []
    for index in range(block_count):
        uncompressed_size = metadata.u32("block uncompressed size")
        compressed_size = metadata.u32("block compressed size")
        flags = metadata.u16("block flags")
        if not (0 < uncompressed_size <= MAX_BLOCK_BYTES):
            fail("unsafe UnityFS block %d uncompressed size" % index)
        if not (0 < compressed_size <= MAX_BLOCK_BYTES):
            fail("unsafe UnityFS block %d compressed size" % index)
        if (flags & UNITYFS_COMPRESSION_MASK) not in (0, 2, 3):
            fail("unsupported UnityFS block %d compression" % index)
        blocks.append((uncompressed_size, compressed_size, flags))

    node_count = metadata.u32("directory count")
    if node_count != len(EXPECTED_ENTRIES):
        fail("UnityFS inner file count differs from the approved build")
    nodes = []
    for _index in range(node_count):
        offset = metadata.u64("entry offset")
        size = metadata.u64("entry size")
        flags = metadata.u32("entry flags")
        name = safe_relative(metadata.cstring("entry name"), "UnityFS entry")
        if "/" in name:
            fail("nested UnityFS entry is unsupported")
        nodes.append({"offset": offset, "size": size, "flags": flags, "name": name})
    metadata.finish()

    names = sorted(node["name"] for node in nodes)
    if names != sorted(EXPECTED_ENTRIES):
        fail("datapack.unity3d inner file set differs from the approved build: %r" % names)
    ordered = sorted(nodes, key=lambda node: node["offset"])
    logical_size = 0
    for node in ordered:
        if node["offset"] != logical_size:
            fail("UnityFS entries overlap or leave an unsupported gap")
        expected_size = EXPECTED_ENTRIES[node["name"]]
        if node["size"] != expected_size:
            fail(
                "inner file %s has %d bytes, expected %d (different build)"
                % (node["name"], node["size"], expected_size)
            )
        logical_size += node["size"]
    if sum(block[0] for block in blocks) != logical_size:
        fail("UnityFS block stream size differs from its directory")

    if archive_flags & UNITYFS_BLOCK_INFO_AT_END:
        data_offset = first_payload_offset
        data_limit = info_offset
    else:
        data_offset = info_offset + info_compressed_size
        data_limit = declared_size
    if archive_flags & UNITYFS_BLOCK_PADDING:
        data_offset = align_offset(data_offset, 16)
    if data_offset + sum(block[1] for block in blocks) != data_limit:
        fail("UnityFS compressed block stream has an unexpected boundary")
    return blocks, ordered, data_offset


def write_all(descriptor, payload):
    view = memoryview(payload)
    try:
        offset = 0
        while offset < len(view):
            written = os.write(descriptor, view[offset:])
            if not written:
                fail("short write while publishing prepared owner data")
            offset += written
    finally:
        view.release()


def stream_unityfs(path, stage, blocks, nodes, data_offset, lz4_block):
    states = []
    try:
        for node in nodes:
            name = node["name"]
            target = stage_path(stage, DATA_DIR + "/" + name)
            if os.path.lexists(target) and (
                os.path.islink(target) or not regular_file(target)
            ):
                fail("unsafe existing output %s" % name)
            temporary = "%s.nxpart.%d" % (target, os.getpid())
            if os.path.lexists(temporary):
                fail("stale or unsafe temporary output")
            descriptor = os.open(
                temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
            )
            states.append(
                {
                    "descriptor": descriptor,
                    "name": name,
                    "remaining": node["size"],
                    "target": target,
                    "temporary": temporary,
                    "written": 0,
                }
            )

        total = len(states)
        current = 0
        progress(0, total, "PREPARANDO 1/%d" % total)
        with open(path, "rb") as stream:
            stream.seek(data_offset)
            for index, block in enumerate(blocks):
                uncompressed_size, compressed_size, flags = block
                compressed = read_exact(
                    stream, compressed_size, "UnityFS data block %d" % index
                )
                payload = decompress_unityfs(
                    compressed,
                    uncompressed_size,
                    flags,
                    lz4_block,
                    "UnityFS data block %d" % index,
                )
                payload_offset = 0
                while payload_offset < len(payload):
                    if current >= total:
                        fail("UnityFS block stream exceeds its directory")
                    state = states[current]
                    count = min(
                        len(payload) - payload_offset, state["remaining"]
                    )
                    segment = memoryview(payload)[
                        payload_offset : payload_offset + count
                    ]
                    try:
                        write_all(state["descriptor"], segment)
                    finally:
                        segment.release()
                    state["remaining"] -= count
                    state["written"] += count
                    payload_offset += count
                    if state["remaining"] == 0:
                        current += 1
                        if current < total:
                            progress(
                                current,
                                total,
                                "PREPARANDO %d/%d" % (current + 1, total),
                            )

        if current != total or any(state["remaining"] for state in states):
            fail("UnityFS block stream ended before all entries were written")
        for state in states:
            if state["written"] != EXPECTED_ENTRIES[state["name"]]:
                fail("prepared owner data size changed for %s" % state["name"])
            os.fsync(state["descriptor"])
            os.close(state["descriptor"])
            state["descriptor"] = None
        for state in states:
            os.replace(state["temporary"], state["target"])
        progress(total, total, "PREPARANDO %d/%d" % (total, total))
        return total, sum(state["written"] for state in states)
    finally:
        for state in states:
            descriptor = state.get("descriptor")
            if descriptor is not None:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
            try:
                os.unlink(state["temporary"])
            except FileNotFoundError:
                pass


def unbundle(stage, lz4_block):
    datapack = stage_path(stage, DATAPACK)
    blocks, nodes, data_offset = parse_unityfs(datapack, lz4_block)
    files, written = stream_unityfs(
        datapack, stage, blocks, nodes, data_offset, lz4_block
    )
    # The container itself never reaches the device: the engine reads the loose
    # layout and the pack would only duplicate 50 MB on the card.
    os.unlink(datapack)
    return files, written


def write_marker(stage, files, written):
    document = {
        "format": FORMAT,
        "inputs": "nxextract-authenticated",
        "loose_files": files,
        "loose_bytes": written,
        "package": PACKAGE,
        "preparation": PREPARATION,
    }
    payload = (json.dumps(document, ensure_ascii=True, sort_keys=True,
                          separators=(",", ":")) + "\n").encode("ascii")
    atomic_bytes(os.path.join(stage, MARKER), payload)


def prepare(stage, game_dir):
    lz4_block = configure_lz4(game_dir)
    print("[prepare] checking NXExtract-authenticated ARM64 inputs")
    validate_owner_input(stage)
    print("[prepare] unbundling the asset pack into the loose Unity layout")
    files, written = unbundle(stage, lz4_block)
    for name in EXPECTED_ENTRIES:
        path = stage_path(stage, DATA_DIR + "/" + name)
        if not regular_file(path) or os.path.islink(path):
            fail("loose asset is missing after preparation: " + name)
        if os.path.getsize(path) != EXPECTED_ENTRIES[name]:
            fail("loose asset size changed after preparation: " + name)
    write_marker(stage, files, written)
    print("[prepare] Party Hard GO owner data ready: %d loose files, %d bytes"
          % (files, written))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--game-dir", required=True)
    args = parser.parse_args(argv)
    if os.path.islink(args.stage) or not os.path.isdir(args.stage):
        fail("NXExtract stage is missing or unsafe")
    if os.path.islink(args.game_dir) or not os.path.isdir(args.game_dir):
        fail("port directory is missing or unsafe")
    prepare(os.path.realpath(args.stage), os.path.realpath(args.game_dir))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PreparationError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
