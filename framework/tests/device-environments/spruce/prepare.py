#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare the external Spruce 4.3.4 PC environment transactionally."""

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath


DEVICE_DIR = Path(__file__).resolve().parent
REPOSITORY = DEVICE_DIR.parents[3]
CONTRACT_PATH = DEVICE_DIR / "contract-v1.json"
TRUSTED_TOOL_PATH = os.defpath


class PreparationError(Exception):
    """The supplied source or prepared environment violated its contract."""


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


def safe_member_path(root, member):
    logical = PurePosixPath(member)
    require(not logical.is_absolute() and ".." not in logical.parts,
            "unsafe archive member: %s" % member)
    return root.joinpath(*logical.parts)


def validate_file(path, expected, label):
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


def run_checked(arguments):
    require(arguments and Path(arguments[0]).is_absolute(),
            "external executable must use an absolute path")
    result = subprocess.run(
        arguments,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
        env={"PATH": TRUSTED_TOOL_PATH, "LANG": "C", "LC_ALL": "C"},
    )
    if result.returncode != 0:
        tail = "\n".join(result.stdout.splitlines()[-20:])
        raise PreparationError(
            "command failed (%d): %s\n%s" %
            (result.returncode, " ".join(arguments), tail)
        )


def validate_runtime_files(contract, stage):
    layout = contract["prepared_layout"]
    roots = {
        "armhf-chroot": stage / layout["armhf_chroot"],
        "muos-reduced": stage / layout["muos_reduced"],
    }
    checked = []
    for expected in contract["runtime_files"]:
        path = safe_member_path(roots[expected["root"]], expected["path"])
        validate_file(path, expected, expected["id"])
        require(elf_identity(path) ==
                (expected["elf_class"], expected["elf_machine"]),
                "%s ELF identity mismatch" % expected["id"])
        checked.append(expected["id"])
    return checked


def prepare(archive, output):
    contract = read_json(CONTRACT_PATH)
    firmware = contract["firmware"]

    require(archive.is_absolute(), "--archive must be an absolute path")
    require(output.is_absolute(), "--output must be an absolute path")
    require(archive.is_file() and not archive.is_symlink(),
            "firmware archive is missing or unsafe")
    require(not output.exists() and not output.is_symlink(),
            "output already exists; refusing to overwrite it")
    require(REPOSITORY not in output.parents and output != REPOSITORY,
            "firmware environments must stay outside the repository")
    require(archive.stat().st_size == firmware["archive_size"],
            "firmware archive size mismatch")
    require(sha256_file(archive) == firmware["archive_sha256"],
            "firmware archive SHA-256 mismatch")

    tools = {name: resolve_tool(name) for name in ("7z", "unsquashfs")}

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(
        prefix=".spruce-environment.", dir=str(output.parent)
    ))
    source = temporary / "source"
    stage = temporary / "stage"
    source.mkdir()
    stage.mkdir()

    try:
        members = [item["path"] for item in contract["archive_members"]]
        run_checked([
            tools["7z"], "x", "-y", "-o%s" % source, str(archive), *members,
        ])

        by_id = {}
        for expected in contract["archive_members"]:
            path = safe_member_path(source, expected["path"])
            validate_file(path, expected, expected["id"])
            by_id[expected["id"]] = path

        layout = contract["prepared_layout"]
        alternate_loader = stage / layout["alternate_loader"]
        chroot = stage / layout["armhf_chroot"]
        muos = stage / layout["muos_reduced"]

        alternate_loader.parent.mkdir(parents=True, exist_ok=True)
        chroot.parent.mkdir(parents=True, exist_ok=True)
        muos.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(by_id["alternate-loader"], alternate_loader)
        alternate_loader.chmod(0o755)

        run_checked([
            tools["unsquashfs"], "-f", "-d", str(chroot),
            str(by_id["armhf-chroot-image"]),
        ])
        run_checked([
            tools["unsquashfs"], "-f", "-d", str(muos),
            str(by_id["muos-reduced-image"]),
            "lib32",
            "usr/lib32",
            "usr/lib/libSDL2-2.0.so.0",
            "usr/lib/libSDL2-2.0.so.0.2800.5",
            "usr/lib/libSDL2.so",
            "usr/lib/libEGL.so",
            "usr/lib/libEGL.so.1",
            "usr/lib/libGLESv2.so",
            "usr/lib/libGLESv2.so.2",
            "usr/lib/libmali.so",
            "usr/lib/libasound.so",
            "usr/lib/libasound.so.2",
            "usr/lib/libasound.so.2.0.0",
        ])

        validate_file(alternate_loader, by_id_contract(
            contract, "alternate-loader"
        ), "prepared alternate loader")
        require(elf_identity(alternate_loader) == (1, 40),
                "prepared alternate loader is not ARMHF ELF32")
        checked = validate_runtime_files(contract, stage)

        receipt = {
            "schema": "nxframework-prepared-device-environment-v1",
            "schema_version": 1,
            "environment_id": contract["id"],
            "contract_sha256": sha256_file(CONTRACT_PATH),
            "source_archive": {
                "name": firmware["archive_name"],
                "size": firmware["archive_size"],
                "sha256": firmware["archive_sha256"],
            },
            "layout": layout,
            "checks": {
                "source_archive": True,
                "archive_members": True,
                "runtime_files": checked,
                "alternate_loader_armhf": True,
                "repository_contains_firmware": False,
            },
            "evidence": {
                "kind": "prepared-host-environment",
                "hardware_ran": False,
                "device_access": False,
                "physical_runtime_claim": False,
            },
        }
        receipt_path = stage / layout["receipt"]
        with receipt_path.open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2, sort_keys=True)
            stream.write("\n")

        os.replace(stage, output)
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def by_id_contract(contract, artifact_id):
    for expected in contract["archive_members"]:
        if expected["id"] == artifact_id:
            return expected
    raise PreparationError("contract lacks archive member: %s" % artifact_id)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Prepare the verified Spruce 4.3.4 PC environment"
    )
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        prepare(args.archive.resolve(), args.output.resolve())
    except (OSError, PreparationError) as error:
        print("spruce environment preparation failed: %s" % error,
              file=sys.stderr)
        return 1
    print("spruce environment prepared: %s" % args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
