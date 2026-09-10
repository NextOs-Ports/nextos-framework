#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare the optional ROCKNIX RK3566 PC environment transactionally."""

import argparse
import gzip
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from pathlib import Path, PurePosixPath


DEVICE_DIR = Path(__file__).resolve().parent
REPOSITORY = DEVICE_DIR.parents[3]
CONTRACT_PATH = DEVICE_DIR / "contract-v1.json"
STREAM_BLOCK_SIZE = 4 * 1024 * 1024
GPT_CAPTURE_SIZE = 64 * 1024
TRUSTED_TOOL_PATH = os.defpath


class PreparationError(Exception):
    """The source image or prepared environment violated its contract."""


def require(condition, message):
    if not condition:
        raise PreparationError(message)


def read_json(path):
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def inside(path, root):
    candidate = path.resolve(strict=False)
    boundary = root.resolve()
    return os.path.commonpath((str(candidate), str(boundary))) == str(boundary)


def safe_member_path(root, member):
    logical = PurePosixPath(member)
    require(not logical.is_absolute() and ".." not in logical.parts,
            "unsafe SquashFS member: %s" % member)
    return root.joinpath(*logical.parts)


def validate_regular(path, expected, label):
    require(path.is_file() and not path.is_symlink(),
            "%s is missing or not a regular file: %s" % (label, path))
    require(path.stat().st_size == expected["size"],
            "%s size mismatch" % label)
    require(sha256_file(path) == expected["sha256"],
            "%s SHA-256 mismatch" % label)


def elf_identity(path):
    with path.open("rb") as stream:
        header = stream.read(64)
    require(len(header) >= 20 and header[:4] == b"\x7fELF",
            "not an ELF: %s" % path)
    require(header[5] in (1, 2), "unsupported ELF byte order: %s" % path)
    endian = "<" if header[5] == 1 else ">"
    return header[4], struct.unpack(endian + "H", header[18:20])[0]


def resolve_tool(name):
    found = shutil.which(name, path=TRUSTED_TOOL_PATH)
    require(found is not None, "required tool is missing: %s" % name)
    try:
        resolved = Path(found).resolve(strict=True)
    except OSError as error:
        raise PreparationError("cannot resolve %s: %s" % (name, error))
    require(resolved.is_file() and os.access(resolved, os.X_OK),
            "tool is not executable: %s" % resolved)
    return str(resolved)


def validate_runtime_files(contract, root):
    checked = []
    for expected in contract["runtime_files"]:
        path = safe_member_path(root, expected["path"])
        if expected["kind"] == "symlink":
            require(path.is_symlink(), "%s symlink is missing" % expected["id"])
            require(os.readlink(path) == expected["target"],
                    "%s symlink target mismatch" % expected["id"])
            target = path.parent / expected["target"]
            require(inside(target, root),
                    "%s symlink escapes the rootfs" % expected["id"])
        else:
            validate_regular(path, expected, expected["id"])
            if "elf_class" in expected or "elf_machine" in expected:
                require(elf_identity(path) ==
                        (expected["elf_class"], expected["elf_machine"]),
                        "%s ELF identity mismatch" % expected["id"])
        checked.append(expected["id"])
    return checked


def run_checked(arguments):
    require(arguments and Path(arguments[0]).is_absolute(),
            "external executable must use an absolute path")
    result = subprocess.run(
        arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=False,
        env={"PATH": TRUSTED_TOOL_PATH, "LANG": "C", "LC_ALL": "C"},
    )
    if result.returncode != 0:
        tail = "\n".join(result.stdout.splitlines()[-30:])
        raise PreparationError(
            "command failed (%d): %s\n%s" %
            (result.returncode, " ".join(arguments), tail)
        )


def copy_overlap(block, block_start, wanted_start, wanted_size, destination):
    block_end = block_start + len(block)
    wanted_end = wanted_start + wanted_size
    start = max(block_start, wanted_start)
    end = min(block_end, wanted_end)
    if start < end:
        destination.extend(block[start - block_start:end - block_start])


def stream_system_payload(archive, contract, destination):
    firmware = contract["firmware"]
    payload = contract["system_payload"]
    partitions = {item["number"]: item for item in contract["disk"]["partitions"]}
    system_partition = partitions[payload["partition_number"]]
    storage_partition = next(item for item in partitions.values()
                             if item["filesystem"] == "ext4")
    captures = {
        "gpt": (0, GPT_CAPTURE_SIZE, bytearray()),
        "fat": (system_partition["offset"], 512, bytearray()),
        "ext4": (storage_partition["offset"], 4096, bytearray()),
    }
    written = 0
    position = 0

    with gzip.open(archive, "rb") as source, destination.open("xb") as output:
        while True:
            block = source.read(STREAM_BLOCK_SIZE)
            if not block:
                break
            for wanted_start, wanted_size, capture in captures.values():
                copy_overlap(block, position, wanted_start, wanted_size, capture)
            block_end = position + len(block)
            payload_start = payload["raw_offset"]
            payload_end = payload_start + payload["size"]
            start = max(position, payload_start)
            end = min(block_end, payload_end)
            if start < end:
                piece = block[start - position:end - position]
                output.write(piece)
                written += len(piece)
            position = block_end

    require(position == firmware["uncompressed_size"],
            "uncompressed image size mismatch")
    require(written == payload["size"], "SYSTEM extraction was incomplete")
    return {name: bytes(value[2]) for name, value in captures.items()}


def validate_gpt(contract, data):
    disk = contract["disk"]
    sector = disk["logical_sector_size"]
    require(len(data) >= 34 * sector, "GPT capture is incomplete")
    require(data[510:512] == b"\x55\xaa", "protective MBR signature mismatch")
    header = data[sector:2 * sector]
    fields = struct.unpack("<8sIIIIQQQQ16sQIII", header[:92])
    (signature, revision, header_size, stored_crc, reserved, current_lba,
     backup_lba, first_usable, last_usable, disk_guid, entries_lba,
     entry_count, entry_size, entries_crc) = fields
    require(signature == b"EFI PART" and revision == 0x00010000,
            "GPT header identity mismatch")
    require(header_size >= 92 and header_size <= sector and reserved == 0,
            "GPT header layout is invalid")
    crc_header = bytearray(header[:header_size])
    crc_header[16:20] = b"\0\0\0\0"
    require((zlib.crc32(crc_header) & 0xffffffff) == stored_crc,
            "GPT header CRC mismatch")
    entries_start = entries_lba * sector
    entries_end = entries_start + entry_count * entry_size
    require(entries_end <= len(data), "GPT entry array capture is incomplete")
    entries = data[entries_start:entries_end]
    require((zlib.crc32(entries) & 0xffffffff) == entries_crc,
            "GPT entry-array CRC mismatch")
    require(current_lba == 1 and
            backup_lba == disk["total_logical_sectors"] - 1,
            "GPT primary/backup LBA mismatch")
    require(str(uuid.UUID(bytes_le=disk_guid)) == disk["disk_guid"],
            "GPT disk GUID mismatch")
    require(disk["crc_state"] == "valid", "contract does not require valid GPT CRCs")

    observed = []
    for index in range(entry_count):
        entry = entries[index * entry_size:(index + 1) * entry_size]
        if entry[:16] == b"\0" * 16:
            continue
        start_lba, end_lba = struct.unpack("<QQ", entry[32:48])
        observed.append({
            "number": index + 1,
            "name": entry[56:128].decode("utf-16le").rstrip("\0"),
            "start_lba": start_lba,
            "end_lba": end_lba,
            "offset": start_lba * sector,
            "size": (end_lba - start_lba + 1) * sector,
            "type_guid": str(uuid.UUID(bytes_le=entry[:16])),
        })
    require(len(observed) == len(disk["partitions"]),
            "GPT active partition count mismatch")
    for actual, expected in zip(observed, disk["partitions"]):
        for key in ("number", "name", "start_lba", "end_lba", "offset",
                    "size", "type_guid"):
            require(actual[key] == expected[key],
                    "GPT partition %d %s mismatch" % (expected["number"], key))
    require(first_usable <= observed[0]["start_lba"] and
            last_usable >= observed[-1]["end_lba"],
            "GPT usable range excludes a declared partition")


def validate_filesystem_headers(contract, fat, ext4):
    partitions = {item["filesystem"]: item
                  for item in contract["disk"]["partitions"]}
    expected_fat = partitions["fat32"]
    require(len(fat) == 512 and fat[510:512] == b"\x55\xaa",
            "FAT32 boot sector is incomplete")
    require(struct.unpack("<H", fat[11:13])[0] ==
            contract["disk"]["logical_sector_size"],
            "FAT32 logical sector size mismatch")
    require(fat[82:90].rstrip() == b"FAT32", "FAT32 signature mismatch")
    require(fat[71:82].decode("ascii").rstrip() == expected_fat["label"],
            "FAT32 label mismatch")
    serial = "%08X" % struct.unpack("<I", fat[67:71])[0]
    require(serial[:4] + "-" + serial[4:] == expected_fat["uuid"],
            "FAT32 UUID mismatch")

    expected_ext4 = partitions["ext4"]
    require(len(ext4) == 4096, "ext4 header capture is incomplete")
    superblock = ext4[1024:2048]
    require(struct.unpack("<H", superblock[56:58])[0] == 0xef53,
            "ext4 superblock signature mismatch")
    require(str(uuid.UUID(bytes=superblock[104:120])) == expected_ext4["uuid"],
            "ext4 UUID mismatch")
    label = superblock[120:136].split(b"\0", 1)[0].decode("ascii")
    require(label == expected_ext4["label"], "ext4 label mismatch")


def validate_squashfs(contract, path):
    expected = contract["system_payload"]["filesystem"]
    with path.open("rb") as stream:
        header = stream.read(96)
    require(len(header) == 96 and header[:4] == b"hsqs",
            "SYSTEM is not little-endian SquashFS")
    inode_count = struct.unpack("<I", header[4:8])[0]
    block_size = struct.unpack("<I", header[12:16])[0]
    compression, major = struct.unpack("<H6xH", header[20:30])
    bytes_used = struct.unpack("<Q", header[40:48])[0]
    require(expected["type"] == "squashfs" and major == expected["version"],
            "SYSTEM SquashFS version mismatch")
    require(expected["compression"] == "zstd" and compression == 6,
            "SYSTEM SquashFS compression mismatch")
    require(inode_count == expected["inode_count"] and
            block_size == expected["block_size"] and
            bytes_used == expected["bytes_used"],
            "SYSTEM SquashFS geometry mismatch")


def validate_contract(contract):
    require(contract.get("schema") == "nxframework-device-environment-v1" and
            contract.get("schema_version") == 1 and
            contract.get("id") == "rocknix-rk3566-specific-20260801",
            "wrong ROCKNIX environment contract identity")
    firmware = contract["firmware"]
    require(firmware["compression"] == "gzip" and
            firmware["stored_in_repository"] is False and
            firmware["redistributed_by_framework"] is False,
            "firmware ownership boundary changed")
    payload = contract["system_payload"]
    require(payload["contiguous"] is True and
            payload["raw_offset"] ==
            payload["raw_start_sector"] * contract["disk"]["logical_sector_size"] and
            payload["size"] ==
            payload["raw_sector_count"] * contract["disk"]["logical_sector_size"],
            "SYSTEM stream locator is inconsistent")


def prepare(archive, output):
    contract = read_json(CONTRACT_PATH)
    validate_contract(contract)
    firmware = contract["firmware"]

    require(archive.is_absolute(), "--archive must be an absolute path")
    require(output.is_absolute(), "--output must be an absolute path")
    require(archive.is_file() and not archive.is_symlink(),
            "firmware archive is missing or unsafe")
    require(not inside(archive, REPOSITORY),
            "firmware archive must remain outside the repository")
    require(not output.exists() and not output.is_symlink(),
            "output already exists; refusing to overwrite it")
    require(not inside(output, REPOSITORY),
            "prepared environments must stay outside the repository")
    require(archive.stat().st_size == firmware["archive_size"],
            "firmware archive size mismatch")
    require(sha256_file(archive) == firmware["archive_sha256"],
            "firmware archive SHA-256 mismatch")
    with archive.open("rb") as stream:
        require(stream.read(2) == b"\x1f\x8b", "firmware archive is not gzip")
    unsquashfs = resolve_tool("unsquashfs")

    output.parent.mkdir(parents=True, exist_ok=True)
    require(output.parent.is_dir() and not inside(output.parent, REPOSITORY),
            "output parent is unsafe")
    temporary = Path(tempfile.mkdtemp(
        prefix=".rocknix-environment.", dir=str(output.parent)
    ))
    stage = temporary / "stage"
    stage.mkdir()

    try:
        layout = contract["prepared_layout"]
        system_image = safe_member_path(stage, layout["system_image"])
        rootfs = safe_member_path(stage, layout["rootfs"])
        system_image.parent.mkdir(parents=True)
        rootfs.mkdir(parents=True)

        captures = stream_system_payload(archive, contract, system_image)
        validate_gpt(contract, captures["gpt"])
        validate_filesystem_headers(contract, captures["fat"], captures["ext4"])
        validate_regular(system_image, contract["system_payload"], "SYSTEM")
        validate_squashfs(contract, system_image)

        members = [item["path"] for item in contract["runtime_files"]]
        run_checked([
            unsquashfs, "-quiet", "-no-progress", "-f",
            "-d", str(rootfs), str(system_image), *members,
        ])
        checked = validate_runtime_files(contract, rootfs)

        receipt = {
            "schema": "nxframework-prepared-device-environment-v1",
            "schema_version": 1,
            "environment_id": contract["id"],
            "contract_sha256": sha256_file(CONTRACT_PATH),
            "source_archive": {
                "name": firmware["archive_name"],
                "size": firmware["archive_size"],
                "sha256": firmware["archive_sha256"],
                "compression": firmware["compression"],
                "uncompressed_size": firmware["uncompressed_size"],
            },
            "system_payload": {
                "path": layout["system_image"],
                "size": contract["system_payload"]["size"],
                "sha256": contract["system_payload"]["sha256"],
            },
            "layout": layout,
            "checks": {
                "source_archive": True,
                "uncompressed_size": True,
                "gpt_crc_and_layout": True,
                "filesystem_headers": True,
                "system_payload": True,
                "runtime_files": checked,
                "repository_contains_firmware": False,
            },
            "evidence": {
                "kind": "prepared-host-environment",
                "hardware_ran": False,
                "device_access": False,
                "physical_graphics_claim": False,
            },
        }
        receipt_path = safe_member_path(stage, layout["receipt"])
        with receipt_path.open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2, sort_keys=True)
            stream.write("\n")

        os.replace(stage, output)
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Prepare the opt-in ROCKNIX RK3566 PC environment"
    )
    parser.add_argument("--archive", required=True, type=Path,
                        help="absolute path to the owner-supplied official image")
    parser.add_argument("--output", required=True, type=Path,
                        help="new environment directory outside this repository")
    args = parser.parse_args(argv)
    archive = Path(os.path.abspath(os.fspath(args.archive)))
    output = Path(os.path.abspath(os.fspath(args.output)))
    try:
        prepare(archive, output)
    except (EOFError, gzip.BadGzipFile, OSError, PreparationError) as error:
        print("ROCKNIX environment preparation failed: %s" % error,
              file=sys.stderr)
        return 1
    print("ROCKNIX environment prepared: %s" % output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
