#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify one prepared Spruce environment without touching device hardware."""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath


DEVICE_DIR = Path(__file__).resolve().parent
REPOSITORY = DEVICE_DIR.parents[3]
CONTRACT_PATH = DEVICE_DIR / "contract-v1.json"
PROBE_PATH = REPOSITORY / "framework/tests/probes/spruce-sdl-drivers.c"
PRIVATE_PATH = re.compile(r"(?:/home/|/Users/|[A-Za-z]:\\\\)")
PRIVATE_ADDRESS = re.compile(
    r"(?:10\.|127\.|169\.254\.|192\.168\.|"
    r"172\.(?:1[6-9]|2[0-9]|3[01])\.)"
)
TRUSTED_TOOL_PATH = os.defpath


class VerificationError(Exception):
    """The contract or prepared environment is invalid."""


def require(condition, message):
    if not condition:
        raise VerificationError(message)


def read_json(path):
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def safe_relative(root, relative):
    logical = PurePosixPath(relative)
    require(not logical.is_absolute() and ".." not in logical.parts,
            "unsafe relative path: %s" % relative)
    path = root.joinpath(*logical.parts)
    resolved_root = root.resolve()
    resolved = path.resolve(strict=False)
    require(os.path.commonpath((str(resolved_root), str(resolved))) ==
            str(resolved_root), "path escaped environment: %s" % relative)
    return path


def logical_path(environment, absolute):
    require(isinstance(absolute, str) and absolute.startswith("/") and
            ".." not in PurePosixPath(absolute).parts,
            "unsafe logical path: %r" % absolute)
    return safe_relative(environment / "root", absolute.lstrip("/"))


def elf_identity(path):
    with path.open("rb") as stream:
        header = stream.read(64)
    require(len(header) >= 20 and header[:4] == b"\x7fELF",
            "not an ELF: %s" % path)
    require(header[5] in (1, 2), "unsupported ELF byte order: %s" % path)
    endian = "<" if header[5] == 1 else ">"
    return header[4], struct.unpack(endian + "H", header[18:20])[0]


def resolve_tool(name, required=True):
    found = shutil.which(name, path=TRUSTED_TOOL_PATH)
    if found is None:
        if required:
            raise VerificationError("required tool is missing: %s" % name)
        return None
    try:
        resolved = Path(found).resolve(strict=True)
    except OSError as error:
        raise VerificationError("cannot resolve %s: %s" % (name, error))
    require(resolved.is_file() and os.access(resolved, os.X_OK),
            "tool is not executable: %s" % resolved)
    return str(resolved)


def clean_subprocess_env():
    return {"PATH": TRUSTED_TOOL_PATH, "LANG": "C", "LC_ALL": "C"}


def validate_regular(path, expected, label):
    require(path.is_file() and not path.is_symlink(),
            "%s is missing or unsafe" % label)
    require(path.stat().st_size == expected["size"],
            "%s size mismatch" % label)
    require(sha256_file(path) == expected["sha256"],
            "%s SHA-256 mismatch" % label)


def validate_contract(contract):
    require(contract.get("schema") == "nxframework-device-environment-v1" and
            contract.get("schema_version") == 1 and
            contract.get("id") == "spruce-miyoo-flip-4.3.4",
            "wrong Spruce environment contract identity")
    text = json.dumps(contract, sort_keys=True)
    require(PRIVATE_PATH.search(text) is None,
            "contract contains a personal path")
    require(PRIVATE_ADDRESS.search(text) is None,
            "contract contains a private address")
    require(contract["firmware"]["stored_in_repository"] is False and
            contract["firmware"]["redistributed_by_framework"] is False,
            "contract would redistribute firmware")

    topology = contract["topology"]
    require(topology["host_architecture"] == "aarch64" and
            topology["roles"] == {
                "nxextract": "aarch64",
                "nxsplash": "armv7",
                "game": "armv7",
            }, "mixed-ABI role topology changed")
    require(topology["default_armhf_interpreter"] == {
                "path": "/lib/ld-linux-armhf.so.3",
                "state": "absent",
            }, "default ARMHF interpreter contract changed")
    require(topology["alternate_armhf_interpreter"] ==
            "/mnt/SDCARD/spruce/flip/ld-linux-armhf.so.3",
            "alternate ARMHF interpreter changed")
    require(topology["sdl"]["compiled_video_drivers"] ==
            ["KMSDRM", "dummy"] and
            topology["sdl"]["physical_selected_video_driver"] == "KMSDRM",
            "target SDL video evidence changed")
    require("/mnt/SDCARD/spruce/flip/muOS/usr/lib" not in
            topology["armhf_library_path_order"],
            "AArch64 usr/lib contaminated the ARMHF closure")


def validate_repository_boundary():
    for path in DEVICE_DIR.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        require(path.suffix not in (".img", ".sqsh", ".7z"),
                "firmware payload entered the repository: %s" % path)
        with path.open("rb") as stream:
            require(stream.read(4) != b"\x7fELF",
                    "firmware ELF entered the repository: %s" % path)


def validate_elf_directory(root, expected_class, expected_machine, label):
    count = 0
    require(root.is_dir() and not root.is_symlink(),
            "%s root is missing or unsafe" % label)
    # The dynamic loader searches this directory, not recursive plugin,
    # firmware or kernel-module subtrees beneath it.
    for path in root.iterdir():
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as stream:
            magic = stream.read(4)
        if magic != b"\x7fELF":
            continue
        require(elf_identity(path) == (expected_class, expected_machine),
                "%s contains a cross-ABI ELF: %s" % (label, path))
        count += 1
    require(count > 0, "%s contains no ELF evidence" % label)
    return count


def validate_environment(contract, environment):
    layout = contract["prepared_layout"]
    receipt_path = safe_relative(environment, layout["receipt"])
    receipt = read_json(receipt_path)
    require(receipt.get("schema") ==
            "nxframework-prepared-device-environment-v1" and
            receipt.get("schema_version") == 1 and
            receipt.get("environment_id") == contract["id"],
            "prepared receipt identity mismatch")
    require(receipt.get("contract_sha256") == sha256_file(CONTRACT_PATH),
            "prepared receipt was made from another contract")
    require(receipt.get("source_archive") == {
                "name": contract["firmware"]["archive_name"],
                "size": contract["firmware"]["archive_size"],
                "sha256": contract["firmware"]["archive_sha256"],
            }, "prepared source archive receipt mismatch")
    require(receipt.get("evidence") == {
                "kind": "prepared-host-environment",
                "hardware_ran": False,
                "device_access": False,
                "physical_runtime_claim": False,
            }, "prepared receipt crossed the PC evidence boundary")

    default_loader = logical_path(
        environment, contract["topology"]["default_armhf_interpreter"]["path"]
    )
    require(not default_loader.exists() and not default_loader.is_symlink(),
            "default ARMHF interpreter unexpectedly exists")

    alternate = logical_path(
        environment, contract["topology"]["alternate_armhf_interpreter"]
    )
    alternate_expected = next(
        item for item in contract["archive_members"]
        if item["id"] == "alternate-loader"
    )
    validate_regular(alternate, alternate_expected, "alternate ARMHF loader")
    require(elf_identity(alternate) == (1, 40),
            "alternate loader is not ARMHF")
    require(os.access(alternate, os.X_OK),
            "alternate ARMHF loader is not executable")

    roots = {
        "armhf-chroot": safe_relative(environment, layout["armhf_chroot"]),
        "muos-reduced": safe_relative(environment, layout["muos_reduced"]),
    }
    for expected in contract["runtime_files"]:
        path = safe_relative(roots[expected["root"]], expected["path"])
        validate_regular(path, expected, expected["id"])
        require(elf_identity(path) ==
                (expected["elf_class"], expected["elf_machine"]),
                "%s ELF identity mismatch" % expected["id"])

    armhf_count = 0
    armhf_count += validate_elf_directory(
        roots["armhf-chroot"] / "usr/lib", 1, 40, "Spruce ARMHF chroot"
    )
    armhf_count += validate_elf_directory(
        roots["muos-reduced"] / "usr/lib32", 1, 40, "Spruce muOS ARMHF"
    )
    aarch64_count = validate_elf_directory(
        roots["muos-reduced"] / "usr/lib", 2, 183, "Spruce muOS AArch64"
    )
    return alternate, roots, armhf_count, aarch64_count


def qemu_sdl_probe(contract, alternate, roots):
    clang = resolve_tool("clang", required=False)
    qemu = resolve_tool("qemu-arm-static", required=False)
    require(clang is not None, "clang is required for the ARMHF SDL probe")
    require(qemu is not None, "qemu-arm-static is required for the ARMHF SDL probe")
    require(PROBE_PATH.is_file() and not PROBE_PATH.is_symlink(),
            "ARMHF SDL probe source is missing")

    chroot = roots["armhf-chroot"]
    muos = roots["muos-reduced"]
    sdl = chroot / "usr/lib/libSDL2-2.0.so.0.22.0"
    library_paths = [
        chroot / "usr/lib32",
        chroot / "usr/lib",
        chroot / "lib",
        muos / "usr/lib32",
        muos / "lib32",
    ]

    with tempfile.TemporaryDirectory(prefix="spruce-sdl-probe.") as temporary:
        probe = Path(temporary) / "spruce-sdl-drivers"
        compile_result = subprocess.run([
            clang,
            "--target=armv7a-linux-gnueabihf",
            "-march=armv7-a",
            "-mfloat-abi=hard",
            "-O2",
            "-fuse-ld=lld",
            "-nostdlib",
            "-fno-builtin",
            "-fno-stack-protector",
            "-Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3",
            "-Wl,-e,_start",
            "-Wl,--no-as-needed",
            "-Wl,--allow-shlib-undefined",
            str(PROBE_PATH),
            "-L%s" % sdl.parent,
            "-l:%s" % sdl.name,
            "-o",
            str(probe),
        ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
           check=False, env=clean_subprocess_env())
        require(compile_result.returncode == 0,
                "ARMHF probe compilation failed:\n%s" % compile_result.stdout)
        require(elf_identity(probe) == (1, 40),
                "compiled SDL probe is not ARMHF")

        run_result = subprocess.run([
            qemu,
            str(alternate),
            "--library-path",
            ":".join(str(path) for path in library_paths),
            str(probe),
        ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
           check=False, env=clean_subprocess_env())
        require(run_result.returncode == 0,
                "ARMHF SDL probe failed (%d):\n%s" %
                (run_result.returncode, run_result.stdout))
        expected = "sdl=%s\ndrivers=%s\n" % (
            contract["topology"]["sdl"]["version"],
            ",".join(contract["topology"]["sdl"]["compiled_video_drivers"]),
        )
        require(run_result.stdout == expected,
                "target SDL report changed: %r" % run_result.stdout)
        return run_result.stdout.strip().replace("\n", " ")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Verify one prepared Spruce PC environment"
    )
    parser.add_argument("--environment", required=True, type=Path)
    parser.add_argument(
        "--static-only", action="store_true",
        help="skip the clang/qemu SDL query",
    )
    args = parser.parse_args(argv)
    try:
        environment = args.environment.resolve()
        require(environment.is_dir() and not environment.is_symlink(),
                "prepared environment is missing or unsafe")
        contract = read_json(CONTRACT_PATH)
        validate_contract(contract)
        validate_repository_boundary()
        alternate, roots, armhf_count, aarch64_count = validate_environment(
            contract, environment
        )
        qemu_result = "skipped"
        if not args.static_only:
            qemu_result = qemu_sdl_probe(contract, alternate, roots)
    except (OSError, StopIteration, VerificationError) as error:
        print("spruce environment gate failed: %s" % error, file=sys.stderr)
        return 1

    print(
        "spruce environment gate passed: armhf_elfs=%d aarch64_elfs=%d "
        "qemu=%s hardware_ran=0 device_access=0" %
        (armhf_count, aarch64_count, qemu_result)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
