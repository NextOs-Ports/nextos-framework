#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write or verify the immutable NXExtract UI multi-architecture release."""

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
from pathlib import Path


UI_DIR = Path(__file__).resolve().parents[1]
SOURCE = UI_DIR / "nxextract_ui.c"
MANIFEST = UI_DIR / "release" / "manifest-v1.json"
ARCHES = ("aarch64", "armv7", "x86_64", "i386")
EXPECTED = {
    "aarch64": ("ELF64", "AArch64", "/lib/ld-linux-aarch64.so.1"),
    "armv7": ("ELF32", "ARM", "/lib/ld-linux-armhf.so.3"),
    "x86_64": (
        "ELF64",
        "Advanced Micro Devices X86-64",
        "/lib64/ld-linux-x86-64.so.2",
    ),
    "i386": ("ELF32", "Intel 80386", "/lib/ld-linux.so.2"),
}
GLIBC_CEILING = (2, 17)
UI_RELEASE_VERSION = "1.2.16"


class ReleaseError(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command(*arguments):
    try:
        return subprocess.check_output(
            arguments, text=True, stderr=subprocess.STDOUT
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise ReleaseError("command failed: %s" % " ".join(arguments)) from error


def version_tuple(value):
    return tuple(int(item) for item in value.split("."))


def inspect_elf(path, architecture):
    header = command("readelf", "-hW", str(path))
    program = command("readelf", "-lW", str(path))
    versions = command("readelf", "--version-info", str(path))
    expected_class, expected_machine, expected_interpreter = EXPECTED[architecture]
    class_match = re.search(r"^\s*Class:\s+(\S+)", header, re.MULTILINE)
    type_match = re.search(r"^\s*Type:\s+(\S+)", header, re.MULTILINE)
    machine_match = re.search(r"^\s*Machine:\s+(.+?)\s*$", header, re.MULTILINE)
    interpreter_match = re.search(
        r"Requesting program interpreter:\s*([^\]]+)", program
    )
    if not class_match or class_match.group(1) != expected_class:
        raise ReleaseError("%s has wrong ELF class" % path)
    if not type_match or type_match.group(1) != "DYN":
        raise ReleaseError("%s is not a PIE executable" % path)
    if not machine_match or machine_match.group(1) != expected_machine:
        raise ReleaseError("%s has wrong ELF machine" % path)
    if not interpreter_match or interpreter_match.group(1) != expected_interpreter:
        raise ReleaseError("%s has wrong interpreter" % path)
    if architecture == "armv7" and "hard-float ABI" not in header:
        raise ReleaseError("%s is not ARM hard-float" % path)
    found = sorted(
        set(re.findall(r"GLIBC_([0-9]+\.[0-9]+)", versions)), key=version_tuple
    )
    if not found:
        raise ReleaseError("%s has no auditable GLIBC version" % path)
    maximum = found[-1]
    if version_tuple(maximum) > GLIBC_CEILING:
        raise ReleaseError("%s requires GLIBC_%s" % (path, maximum))
    dynamic = command("readelf", "-dW", str(path))
    needed = sorted(re.findall(r"Shared library: \[([^\]]+)\]", dynamic))
    unexpected = sorted(set(needed) - {"libc.so.6", "libdl.so.2"})
    if unexpected:
        raise ReleaseError(
            "%s has unexpected DT_NEEDED: %s" % (path, ", ".join(unexpected))
        )
    return maximum


def expected_manifest():
    artifacts = {}
    for architecture in ARCHES:
        path = UI_DIR / "release" / architecture / "nxextract-ui"
        if path.is_symlink() or not path.is_file():
            raise ReleaseError("missing release ELF: %s" % path)
        mode = stat.S_IMODE(path.stat().st_mode)
        if mode != 0o755:
            raise ReleaseError("release ELF mode is not 0755: %s" % path)
        maximum = inspect_elf(path, architecture)
        artifacts[architecture] = {
            "glibc_max": maximum,
            "mode": "0755",
            "path": "ui/release/%s/nxextract-ui" % architecture,
            "sha256": sha256(path),
            "size": path.stat().st_size,
        }
    return {
        "artifacts": artifacts,
        "component": "nxextract-ui",
        "schema_version": 1,
        "source_sha256": sha256(SOURCE),
        "toolchain": {
            "archive_sha256": "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00",
            "name": "zig",
            "version": "0.16.0",
        },
        # Presentation is an independently pinned immutable artifact. Engine
        # releases may advance without relabeling or rebuilding these bytes.
        "version": UI_RELEASE_VERSION,
    }


def canonical(data):
    return json.dumps(data, indent=2, sort_keys=True) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--verify", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        expected = expected_manifest()
        rendered = canonical(expected)
        if arguments.write:
            MANIFEST.parent.mkdir(parents=True, exist_ok=True)
            temporary = MANIFEST.with_name(".%s.%d" % (MANIFEST.name, os.getpid()))
            temporary.write_text(rendered, encoding="utf-8")
            os.chmod(temporary, 0o644)
            os.replace(temporary, MANIFEST)
        else:
            actual = MANIFEST.read_text(encoding="utf-8")
            if actual != rendered:
                raise ReleaseError("release manifest differs from audited artifacts")
    except (OSError, ValueError, ReleaseError) as error:
        print("nxextract-ui release: %s" % error, file=sys.stderr)
        return 1
    print(
        "nxextract-ui release: PASS mode=%s arches=%s"
        % ("write" if arguments.write else "verify", ",".join(ARCHES))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
