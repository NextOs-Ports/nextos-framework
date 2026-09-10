#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepara no PC o ambiente muOS a partir da imagem OFICIAL, sem montar nada.

Molde estrutural igual ao ambiente Spruce -- contrato versionado, preparação
transacional fora do repositório e recibo de proveniência -- mas nenhuma
decisão do outro aparelho é copiada: caminhos, ABIs, interpretador e
provedores de renderer saem da própria imagem.

A imagem NUNCA entra no Git e NUNCA é redistribuída. A leitura é feita com
`debugfs` em modo somente leitura, com deslocamento em bytes da partição
rootfs: sem montar, sem loop device, sem root e sem escrever um byte no
arquivo de origem.

Uso:

    python3 framework/tests/device-environments/muos/prepare.py \
      --image /caminho/para/MustardOS_...img \
      --output /caminho/para/firmware-environments/muos-2601.1
"""

import argparse
import hashlib
import json
import os
import posixpath
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[3].resolve()
CONTRACT = HERE / "contract-v1.json"
CHUNK = 1 << 22
# Um closure de biblioteca real cabe com folga aqui; o teto existe para que um
# erro de resolução vire falha e não um despejo infinito.
MAX_LIBRARIES = 512
TRUSTED_TOOL_PATH = os.defpath


class PrepareError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise PrepareError(message)


def outside_repository(path, label):
    """Fail closed after resolving existing parents and symlinks."""
    resolved = Path(path).resolve(strict=False)
    try:
        resolved.relative_to(REPOSITORY)
    except ValueError:
        return resolved
    raise PrepareError("%s must stay outside the repository: %s" %
                       (label, resolved))


def safe_logical_path(root, logical):
    require(isinstance(logical, str) and logical.startswith("/") and
            not logical.startswith("//") and "\x00" not in logical and
            "\n" not in logical and "\r" not in logical,
            "unsafe image path: %r" % logical)
    normalized = posixpath.normpath(logical)
    require(normalized == logical and ".." not in PurePosixPath(logical).parts,
            "non-canonical image path: %r" % logical)
    local = root.joinpath(*PurePosixPath(logical).parts[1:])
    resolved_root = root.resolve(strict=False)
    resolved = local.resolve(strict=False)
    try:
        resolved.relative_to(resolved_root)
    except ValueError:
        raise PrepareError("image path escaped the prepared root: %s" % logical)
    return local


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def clean_subprocess_env():
    return {
        "PATH": TRUSTED_TOOL_PATH,
        "LANG": "C",
        "LC_ALL": "C",
    }


def resolve_tool(name):
    """Resolve once so the subprocess never looks the command up in PATH."""
    found = shutil.which(name, path=TRUSTED_TOOL_PATH)
    require(found is not None, "required tool is missing: %s" % name)
    try:
        resolved = Path(found).resolve(strict=True)
    except OSError as error:
        raise PrepareError("cannot resolve %s: %s" % (name, error))
    require(resolved.is_file() and os.access(resolved, os.X_OK),
            "tool is not executable: %s" % resolved)
    return str(resolved)


def load_contract():
    with CONTRACT.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def gpt_offset(image, expected_name):
    """Confere o deslocamento declarado contra a tabela GPT real da imagem."""
    with image.open("rb") as stream:
        stream.seek(512)
        header = stream.read(92)
        require(header[:8] == b"EFI PART", "image is not GPT")
        entry_lba = struct.unpack("<Q", header[72:80])[0]
        count = struct.unpack("<I", header[80:84])[0]
        size = struct.unpack("<I", header[84:88])[0]
        require(0 < count <= 256 and 128 <= size <= 4096,
                "GPT partition array is implausible")
        stream.seek(entry_lba * 512)
        table = stream.read(count * size)
    for index in range(count):
        entry = table[index * size:(index + 1) * size]
        if entry[:16] == b"\x00" * 16:
            continue
        name = entry[56:128].decode("utf-16-le").rstrip("\x00")
        if name == expected_name:
            return struct.unpack("<Q", entry[32:40])[0] * 512
    raise PrepareError("partition %r not found in the GPT" % expected_name)


class ImageReader:
    """Leitor somente leitura de uma partição ext4 dentro da imagem."""

    def __init__(self, image, offset):
        self.spec = "%s?offset=%d" % (image, offset)
        self.debugfs = resolve_tool("debugfs")

    def _run(self, command):
        try:
            return subprocess.run(
                [self.debugfs, "-R", command, self.spec],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
                env=clean_subprocess_env(), timeout=60)
        except subprocess.TimeoutExpired:
            raise PrepareError("debugfs timed out while reading the image")

    def exists(self, path):
        result = self._run('stat "%s"' % path)
        return result.returncode == 0 and b"Inode:" in result.stdout

    def dump(self, path, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        # O parser do debugfs separa por espaco: os dois caminhos vao entre
        # aspas para que um destino com espaco continue valido.
        result = self._run('dump -p "%s" "%s"' % (path, destination))
        require(result.returncode == 0 and destination.is_file(),
                "cannot read %s from the image: %s"
                % (path, result.stderr.decode("utf-8", "replace").strip()))
        require(destination.stat().st_size > 0, "%s came out empty" % path)

    def resolve(self, path):
        """Segue links simbólicos dentro da própria imagem."""
        seen = set()
        current = posixpath.normpath(path)
        require(current.startswith("/") and not current.startswith("//"),
                "unsafe image path: %r" % path)
        for _ in range(16):
            require(not any(mark in current for mark in
                            ('"', "\x00", "\n", "\r")),
                    "unsafe image path: %r" % current)
            result = self._run('stat "%s"' % current)
            require(result.returncode == 0, "missing in the image: %s" % path)
            text = result.stdout.decode("utf-8", "replace")
            if "Type: symlink" not in text:
                return current
            target = text.split("Fast link dest: ")[-1].split("\n")[0].strip().strip('"')
            require(target, "unreadable symlink: %s" % current)
            current = posixpath.normpath(
                target if target.startswith("/") else
                posixpath.join(posixpath.dirname(current), target))
            require(current.startswith("/") and not current.startswith("//"),
                    "symlink escaped image root: %s" % path)
            require(current not in seen, "symlink loop at %s" % path)
            seen.add(current)
        raise PrepareError("symlink chain too deep: %s" % path)


def elf_identity(path):
    with Path(path).open("rb") as stream:
        header = stream.read(64)
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return None
    return header[4], struct.unpack_from("<H", header, 18)[0]


def elf_dynamic(path):
    """DT_NEEDED e DT_SONAME de um ELF, sem depender de biblioteca externa."""
    data = Path(path).read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return [], None
    is64 = data[4] == 2
    if is64:
        phoff = struct.unpack_from("<Q", data, 32)[0]
        phentsize = struct.unpack_from("<H", data, 54)[0]
        phnum = struct.unpack_from("<H", data, 56)[0]
    else:
        phoff = struct.unpack_from("<I", data, 28)[0]
        phentsize = struct.unpack_from("<H", data, 42)[0]
        phnum = struct.unpack_from("<H", data, 44)[0]
    dynamic = None
    for index in range(phnum):
        base = phoff + index * phentsize
        if base + phentsize > len(data):
            return [], None
        ptype = struct.unpack_from("<I", data, base)[0]
        if ptype != 2:  # PT_DYNAMIC
            continue
        if is64:
            # ELF64 Phdr: p_offset em +8, p_filesz em +32.
            dynamic = (struct.unpack_from("<Q", data, base + 8)[0],
                       struct.unpack_from("<Q", data, base + 32)[0])
        else:
            dynamic = (struct.unpack_from("<I", data, base + 4)[0],
                       struct.unpack_from("<I", data, base + 16)[0])
        break
    if dynamic is None:
        return [], None
    offset, size = dynamic
    step = 16 if is64 else 8
    needed_offsets = []
    soname_offset = None
    strtab = None
    for position in range(offset, min(offset + size, len(data)), step):
        if is64:
            tag = struct.unpack_from("<q", data, position)[0]
            value = struct.unpack_from("<Q", data, position + 8)[0]
        else:
            tag = struct.unpack_from("<i", data, position)[0]
            value = struct.unpack_from("<I", data, position + 4)[0]
        if tag == 0:
            break
        if tag == 1:
            needed_offsets.append(value)
        elif tag == 5:
            strtab = value
        elif tag == 14:  # DT_SONAME
            soname_offset = value
    if strtab is None:
        return [], None
    # DT_STRTAB é endereço virtual: converte pelo PT_LOAD que o contém.
    file_offset = None
    for index in range(phnum):
        base = phoff + index * phentsize
        ptype = struct.unpack_from("<I", data, base)[0]
        if ptype != 1:  # PT_LOAD
            continue
        if is64:
            p_offset = struct.unpack_from("<Q", data, base + 8)[0]
            p_vaddr = struct.unpack_from("<Q", data, base + 16)[0]
            p_filesz = struct.unpack_from("<Q", data, base + 32)[0]
        else:
            p_offset = struct.unpack_from("<I", data, base + 4)[0]
            p_vaddr = struct.unpack_from("<I", data, base + 8)[0]
            p_filesz = struct.unpack_from("<I", data, base + 16)[0]
        if p_vaddr <= strtab < p_vaddr + p_filesz:
            file_offset = strtab - p_vaddr + p_offset
            break
    if file_offset is None:
        return [], None

    def read_string(entry):
        start = file_offset + entry
        end = data.find(b"\x00", start)
        if start >= len(data) or end <= start:
            return None
        return data[start:end].decode("ascii", "replace")

    names = [name for name in (read_string(entry) for entry in needed_offsets)
             if name]
    soname = read_string(soname_offset) if soname_offset is not None else None
    return names, soname


def prepare(arguments):
    contract = load_contract()
    raw_image = Path(arguments.image)
    require(raw_image.is_absolute(), "--image must be an absolute path")
    require(not any(mark in str(raw_image) for mark in
                    ('"', "\x00", "\n", "\r")),
            "--image contains unsafe characters")
    require(raw_image.is_file() and not raw_image.is_symlink(),
            "official image not found or is a symlink: %s" % raw_image)
    image = outside_repository(raw_image.resolve(strict=True), "official image")

    # Valida o destino antes do hash multi-gigabyte, inclusive symlink
    # pendente. O staging usa este parent canônico e externo ao repositório.
    raw_output = Path(arguments.output)
    require(raw_output.is_absolute(), "--output must be an absolute path")
    require(not any(mark in str(raw_output) for mark in
                    ('"', "\x00", "\n", "\r")),
            "--output contains unsafe characters")
    require(not raw_output.exists() and not raw_output.is_symlink(),
            "output already exists or is a symlink: %s" % raw_output)
    require(raw_output.parent.is_dir() and
            not raw_output.parent.is_symlink(),
            "output parent is missing or is a symlink: %s" %
            raw_output.parent)
    output_parent = outside_repository(
        raw_output.parent.resolve(strict=True), "output parent")
    output = outside_repository(output_parent / raw_output.name, "output")

    firmware = contract["firmware"]
    size = image.stat().st_size
    if image.name.endswith(".gz"):
        raise PrepareError(
            "decompress the official archive first; this preparer reads the "
            "raw image and never writes next to the source")
    require(size == firmware["image_size"],
            "image size differs from the contract: %d" % size)
    print("muos prepare: hashing the official image (%d bytes)" % size)
    digest = sha256_file(image)
    require(digest == firmware["image_sha256"],
            "image SHA-256 differs from the contract: %s" % digest)

    layout = contract["image_layout"]
    offset = gpt_offset(image, layout["rootfs_partition_name"])
    require(offset == layout["rootfs_offset"],
            "rootfs offset differs from the contract: %d" % offset)

    staging = Path(tempfile.mkdtemp(prefix="muos-environment.",
                                    dir=str(output_parent)))
    try:
        reader = ImageReader(image, offset)
        root = staging / "root"
        inventory = {"aarch64": [], "armv7": []}
        search = contract["topology"]["roots"]

        for abi, seeds in contract["seeds"].items():
            pending = list(seeds)
            done = set()
            while pending:
                require(len(done) < MAX_LIBRARIES,
                        "library closure exceeded %d files" % MAX_LIBRARIES)
                path = pending.pop(0)
                if path in done:
                    continue
                done.add(path)
                real = reader.resolve(path)
                destination = safe_logical_path(root, path)
                require(not destination.exists() and not destination.is_symlink(),
                        "duplicate prepared path: %s" % path)
                reader.dump(real, destination)
                identity = elf_identity(destination)
                expected_identity = ((2, 183) if abi == "aarch64" else
                                     (1, 40))
                require(identity == expected_identity,
                        "%s has wrong ELF identity for %s: %r" %
                        (path, abi, identity))
                needed, soname = elf_dynamic(destination)
                aliases = []
                if soname:
                    require("/" not in soname and soname not in (".", "..") and
                            not any(mark in soname for mark in
                                    ('"', "\x00", "\n", "\r")),
                            "unsafe DT_SONAME %r in %s" % (soname, path))
                if soname and soname != destination.name:
                    alias_path = posixpath.join(posixpath.dirname(path), soname)
                    safe_logical_path(root, alias_path)
                    aliases.append(alias_path)
                inventory[abi].append({
                    "path": path,
                    "abi": abi,
                    "size": destination.stat().st_size,
                    "sha256": sha256_file(destination),
                    "elf": {"class": identity[0], "machine": identity[1]},
                    "soname": soname,
                    "needed": needed,
                    "aliases": aliases,
                })
                for name in needed:
                    require("/" not in name and name not in ("", ".", "..") and
                            not any(mark in name for mark in
                                    ('"', "\x00", "\n", "\r")),
                            "unsafe DT_NEEDED %r in %s" % (name, path))
                    resolved = False
                    for directory in search[abi]:
                        candidate = posixpath.join(directory, name)
                        if candidate in done:
                            resolved = True
                            break
                        if reader.exists(candidate):
                            pending.append(candidate)
                            resolved = True
                            break
                    require(resolved, "unresolved DT_NEEDED %s required by %s "
                            "in %s closure" % (name, path, abi))

        # Recria somente aliases declarados, depois que todos os arquivos
        # regulares do closure já estão fixados.
        for entries in inventory.values():
            entries.sort(key=lambda entry: entry["path"])
        regular_paths = {entry["path"] for entries in inventory.values()
                         for entry in entries}
        regular_records = {entry["path"]: entry
                           for entries in inventory.values()
                           for entry in entries}
        alias_targets = {}
        for abi, entries in inventory.items():
            for entry in entries:
                kept = []
                for alias_path in entry["aliases"]:
                    if alias_path in regular_paths:
                        continue
                    previous = alias_targets.get(alias_path)
                    if previous is not None:
                        require(regular_records[previous]["sha256"] ==
                                entry["sha256"],
                                "conflicting SONAME alias: %s" % alias_path)
                        # A imagem muOS publica EGL/GLES/libmali com o mesmo
                        # SONAME e bytes idênticos. Um único alias determinista
                        # basta; o recibo continua fixando todos os regulares.
                        continue
                    if previous is None:
                        alias = safe_logical_path(root, alias_path)
                        require(not alias.exists() and not alias.is_symlink(),
                                "alias collides with prepared path: %s" %
                                alias_path)
                        alias.symlink_to(Path(entry["path"]).name)
                        alias_targets[alias_path] = entry["path"]
                    kept.append(alias_path)
                entry["aliases"] = sorted(kept)

        aliases_by_abi = {
            abi: sorted(alias for entry in entries for alias in entry["aliases"])
            for abi, entries in inventory.items()
        }

        receipt = {
            "schema": "nxframework-prepared-device-environment-v1",
            "schema_version": 1,
            "environment_id": contract["id"],
            "contract_sha256": sha256_file(CONTRACT),
            "source_image": {
                "name": firmware["image_name"],
                "size": firmware["image_size"],
                "sha256": firmware["image_sha256"],
                "rootfs_offset": offset,
                "read_method": layout["read_method"],
            },
            "evidence": {
                "kind": "prepared-host-environment",
                "hardware_ran": False,
                "device_access": False,
                "physical_runtime_claim": False,
            },
            "layout": {"root": "root", "receipt": "environment-receipt.json"},
            # `inventory` mantém o formato v1 para leitores já existentes;
            # `files` acrescenta o recibo forte sem quebrar compatibilidade.
            "inventory": {
                abi: [entry["path"] for entry in entries]
                for abi, entries in inventory.items()},
            "files": inventory,
            "soname_aliases": aliases_by_abi,
            "counts": {abi: len(paths) for abi, paths in inventory.items()},
        }
        (staging / "environment-receipt.json").write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        require(not output.exists() and not output.is_symlink(),
                "output appeared during preparation: %s" % output)
        os.rename(str(staging), str(output))
    except BaseException:
        shutil.rmtree(str(staging), ignore_errors=True)
        raise

    print("muos prepare: ready environment=%s aarch64=%d armv7=%d "
          "hardware_ran=0 device_access=0"
          % (output, receipt["counts"]["aarch64"], receipt["counts"]["armv7"]))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True,
                        help="official raw muOS image (never stored in Git)")
    parser.add_argument("--output", required=True,
                        help="destination directory, outside the repository")
    return prepare(parser.parse_args())


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PrepareError as error:
        print("muos prepare failed: %s" % error, file=sys.stderr)
        sys.exit(1)
