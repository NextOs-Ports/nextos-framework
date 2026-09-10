#!/usr/bin/env python3
"""Prepare private Goblin Sword 2.6.9 data from an owner-supplied IPA.

No downloading, code decryption, execution, or game-data redistribution.
The reference executable is matched inside the container; the IPA container
itself may be renamed or legitimately repackaged.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import re
import stat
import zipfile
from inspect_ipa import MachO, require

BUNDLE = "com.gelatogames.goblinsword"
VERSION = "2.6.9"
MACHO_SHA256 = "f0b5bef0596f23a1e04270426966ff24b296ac9572d6d1b2c593ceab564c62f1"
RESOURCE_SUFFIXES = {".png", ".plist", ".tmx", ".caf", ".fnt", ".txt"}


def prepare(ipa, output):
    require(not output.exists() and not output.is_symlink(), "Output already exists; choose a new directory")
    with zipfile.ZipFile(ipa) as archive:
        entries = archive.infolist()
        names = [e.filename for e in entries]
        require(len(entries) <= 10000 and len(names) == len(set(names)), "Invalid entry count or duplicate names")
        require(sum(e.file_size for e in entries) <= 256 * 1024**2, "Expanded IPA exceeds study limit")
        for entry in entries:
            name = PurePosixPath(entry.filename)
            require(not name.is_absolute() and ".." not in name.parts and "\\" not in entry.filename,
                    "Unsafe ZIP pathname")
            require(not stat.S_ISLNK(entry.external_attr >> 16), "ZIP symlinks are unsupported")
            require(not entry.flag_bits & 1 and entry.file_size <= 64 * 1024**2, "Encrypted or oversized ZIP member")
        infos = [n for n in names if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", n)]
        require(len(infos) == 1, "Expected one top-level app")
        info = plistlib.loads(archive.read(infos[0]))
        require(info.get("CFBundleIdentifier") == BUNDLE, "Wrong bundle identifier")
        require(info.get("CFBundleShortVersionString") == VERSION, "Unsupported game version")
        executable = info.get("CFBundleExecutable")
        require(isinstance(executable, str) and executable not in ("", ".", "..")
                and "/" not in executable and "\\" not in executable, "Invalid executable name")
        prefix = infos[0].rsplit("/", 1)[0] + "/"
        binary = archive.read(prefix + executable)
        macho = MachO(binary)
        require(macho.header["file_type"] == 2, "Expected MH_EXECUTE")
        require(not any(e["cryptid"] for e in macho.encryption), "Encrypted executable is unsupported")
        require(hashlib.sha256(binary).hexdigest() == MACHO_SHA256,
                "Unvalidated internal executable; inspect a new build separately")
        resources = []
        for entry in entries:
            if entry.is_dir() or not entry.filename.startswith(prefix):
                continue
            relative = PurePosixPath(entry.filename[len(prefix):])
            # The tested game keeps its runtime resources directly in the app.
            if len(relative.parts) == 1 and relative.suffix.lower() in RESOURCE_SUFFIXES:
                resources.append((entry, relative.name))
        require(len(resources) == 637, "Runtime resource layout differs from the tested build")
        output.mkdir(parents=True, mode=0o700)
        (output / "assets").mkdir(mode=0o700)
        (output / "goblin-sword.macho").write_bytes(binary)
        manifest = []
        for entry, name in resources:
            data = archive.read(entry)
            (output / "assets" / name).write_bytes(data)
            manifest.append({"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        receipt = {"game": "Goblin Sword", "version": VERSION, "bundle_id": BUNDLE,
                   "macho_sha256": MACHO_SHA256, "resource_count": len(resources),
                   "scope": "private owner data; resource hashes are inventory, not a new gameplay approval",
                   "resources": manifest}
        (output / "owner-data.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Prepared {len(resources)} resources and reference ARM64 executable in {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipa", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare(args.ipa, args.output)
    except (ValueError, KeyError, OSError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Owner data preparation failed: {error}\n")
