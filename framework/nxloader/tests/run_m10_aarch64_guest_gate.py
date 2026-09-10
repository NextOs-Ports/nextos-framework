#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional, local-only M10 gate for the five approved AArch64 guests.

The gate never discovers proprietary data. Every input path and the evidence
directory must be supplied explicitly by an argument or its documented
environment variable. Guest files are opened read-only and are never copied.
The only guest-processing subprocesses permitted are:

    nxloader_inspect GUEST --relocate
    nxloader_inspect GUEST --exports

Those modes map and apply local relocations; ``--exports`` additionally adds
the module's eligible symbols to a temporary registry. Both then destroy all
temporary state. Neither resolves imports, finalizes executable permissions,
runs DT_INIT/INIT_ARRAY, nor calls JNI_OnLoad.
"""

import argparse
import collections
import datetime
import errno
import hashlib
import json
import math
import mmap
import os
import re
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


EXPECTED_PATH = Path(__file__).with_name(
    "m10-approved-aarch64-guests-v1.json")
LOADER_ROOT = Path(__file__).resolve().parents[1]

SOURCE_SNAPSHOT_PATHS = (
    "VERSION",
    "CMakeLists.txt",
    "include/nxloader.h",
    "src/nxloader_internal.h",
    "src/nxloader.c",
    "src/nxloader_elf32.c",
    "src/nxloader_elf64.c",
    "src/nxloader_hooks.c",
    "src/nxloader_protect.c",
    "src/nxloader_registry.c",
    "tools/nxloader_inspect.c",
)
SOURCE_SNAPSHOT_DOMAIN = b"NXLOADER_SOURCE_SNAPSHOT_V1\0"

DEFAULT_TIMEOUT_SECONDS = 120.0
DEFAULT_OUTPUT_LIMIT_BYTES = 256 * 1024
MAX_TIMEOUT_SECONDS = 3600.0
MAX_OUTPUT_LIMIT_BYTES = 16 * 1024 * 1024
TERM_GRACE_SECONDS = 2.0
LIMIT_ENV = {
    "timeout_seconds": "NXLOADER_M10_TIMEOUT_SECONDS",
    "output_limit_bytes": "NXLOADER_M10_OUTPUT_LIMIT_BYTES",
}

ENV_BY_ARGUMENT = {
    "bully2": "NXLOADER_M10_BULLY2",
    "sonic4": "NXLOADER_M10_SONIC4",
    "horizon_unity": "NXLOADER_M10_HORIZON_UNITY",
    "horizon_il2cpp": "NXLOADER_M10_HORIZON_IL2CPP",
    "horizon_main": "NXLOADER_M10_HORIZON_MAIN",
    "inspector": "NXLOADER_M10_INSPECTOR",
    "output_dir": "NXLOADER_M10_OUTPUT_DIR",
}

ELF_MAGIC = b"\x7fELF"
ELFCLASS64 = 2
ELFDATA2LSB = 1
EV_CURRENT = 1
ELFOSABI_SYSV = 0
ET_DYN = 3
EM_AARCH64 = 183

PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_TLS = 7
PF_X = 1
PF_W = 2

DT_NULL = 0
DT_NEEDED = 1
DT_PLTRELSZ = 2
DT_HASH = 4
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_INIT = 12
DT_RPATH = 15
DT_REL = 17
DT_RELSZ = 18
DT_RELENT = 19
DT_PLTREL = 20
DT_TEXTREL = 22
DT_JMPREL = 23
DT_INIT_ARRAY = 25
DT_INIT_ARRAYSZ = 27
DT_RUNPATH = 29
DT_FLAGS = 30
DT_PREINIT_ARRAY = 32
DT_PREINIT_ARRAYSZ = 33
DT_RELRSZ = 35
DT_RELR = 36
DT_RELRENT = 37
DT_GNU_HASH = 0x6FFFFEF5
DT_TLSDESC_PLT = 0x6FFFFEF6
DT_TLSDESC_GOT = 0x6FFFFEF7
DT_FLAGS_1 = 0x6FFFFFFB
DT_ANDROID_REL = 0x6000000F
DT_ANDROID_RELSZ = 0x60000010
DT_ANDROID_RELA = 0x60000011
DT_ANDROID_RELASZ = 0x60000012
DT_ANDROID_RELR = 0x6FFFE000
DT_ANDROID_RELRSZ = 0x6FFFE001
DT_ANDROID_RELRENT = 0x6FFFE003

DF_TEXTREL = 0x4
DF_1_PIE = 0x08000000

STT_TLS = 6
STT_GNU_IFUNC = 10

STB_GLOBAL = 1
STB_WEAK = 2

STT_NOTYPE = 0
STT_OBJECT = 1
STT_FUNC = 2

STV_INTERNAL = 1
STV_HIDDEN = 2

SHN_UNDEF = 0
MAX_DYNAMIC_NAME_LENGTH = 4096

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027

RELOCATION_NAMES = {
    R_AARCH64_ABS64: "R_AARCH64_ABS64",
    R_AARCH64_GLOB_DAT: "R_AARCH64_GLOB_DAT",
    R_AARCH64_JUMP_SLOT: "R_AARCH64_JUMP_SLOT",
    R_AARCH64_RELATIVE: "R_AARCH64_RELATIVE",
}

REGISTRY_EXPORT_FIELDS = (
    "eligible",
    "added",
    "equivalent",
    "replaced_lower_priority",
    "ignored_lower_priority",
    "collisions",
)

FORBIDDEN_DYNAMIC_TAGS = {
    DT_RPATH: "DT_RPATH",
    DT_REL: "DT_REL",
    DT_RELSZ: "DT_RELSZ",
    DT_RELENT: "DT_RELENT",
    DT_TEXTREL: "DT_TEXTREL",
    DT_RUNPATH: "DT_RUNPATH",
    DT_PREINIT_ARRAY: "DT_PREINIT_ARRAY",
    DT_PREINIT_ARRAYSZ: "DT_PREINIT_ARRAYSZ",
    DT_RELRSZ: "DT_RELRSZ",
    DT_RELR: "DT_RELR",
    DT_RELRENT: "DT_RELRENT",
    DT_TLSDESC_PLT: "DT_TLSDESC_PLT",
    DT_TLSDESC_GOT: "DT_TLSDESC_GOT",
    DT_ANDROID_REL: "DT_ANDROID_REL",
    DT_ANDROID_RELSZ: "DT_ANDROID_RELSZ",
    DT_ANDROID_RELA: "DT_ANDROID_RELA",
    DT_ANDROID_RELASZ: "DT_ANDROID_RELASZ",
    DT_ANDROID_RELR: "DT_ANDROID_RELR",
    DT_ANDROID_RELRSZ: "DT_ANDROID_RELRSZ",
    DT_ANDROID_RELRENT: "DT_ANDROID_RELRENT",
}

SAFE_PREFIX = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
ABSOLUTE_PATH = re.compile(r"(?<![A-Za-z0-9_.-])(?:/[A-Za-z0-9_.+@%=-]+){2,}")
INSPECT_HEADER = re.compile(r"(?:^|\s)([a-z_]+)=([^\s]+)")
EXPORT_REPORT = re.compile(
    r"^exports=success added=([0-9]+) equivalent=([0-9]+)$")


class GateError(Exception):
    """A deterministic gate failure whose message is safe to persist."""


def require(condition, message):
    if not condition:
        raise GateError(message)


def checked_add(left, right, limit, message):
    require(left >= 0 and right >= 0 and left <= limit - right, message)
    return left + right


def inventory_registry_exports(exports_by_name):
    """Simulate a same-priority provider add into an empty registry."""
    inventory = {
        "eligible": sum(len(items) for items in exports_by_name.values()),
        "added": 0,
        "equivalent": 0,
        "replaced_lower_priority": 0,
        "ignored_lower_priority": 0,
        "collisions": 0,
    }
    for candidates in exports_by_name.values():
        require(candidates, "registry export candidate group is empty")
        winner = candidates[0]
        inventory["added"] += 1
        for candidate in candidates[1:]:
            new_strength = 0 if candidate["weak"] else 1
            old_strength = 0 if winner["weak"] else 1
            if new_strength == old_strength:
                if candidate["address"] == winner["address"]:
                    inventory["equivalent"] += 1
                else:
                    inventory["collisions"] += 1
            elif new_strength < old_strength:
                inventory["ignored_lower_priority"] += 1
            else:
                winner = candidate
                inventory["replaced_lower_priority"] += 1
    return inventory


def sha256_file(path):
    digest = hashlib.sha256()
    try:
        with open(path, "rb", buffering=0) as stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError as error:
        raise GateError("input cannot be read (errno=%s)" % error.errno)
    return digest.hexdigest()


def build_source_snapshot(loader_root=LOADER_ROOT):
    """Hash the exact ordered source set used to build nxloader_inspect."""
    aggregate = hashlib.sha256()
    aggregate.update(SOURCE_SNAPSHOT_DOMAIN)
    files = []
    root = Path(loader_root)
    for relative_name in SOURCE_SNAPSHOT_PATHS:
        path = root / relative_name
        try:
            before = path.stat()
            require(stat.S_ISREG(before.st_mode) and not path.is_symlink(),
                    "source snapshot member is not a regular file: %s" %
                    relative_name)
            content = path.read_bytes()
            after = path.stat()
        except GateError:
            raise
        except OSError as error:
            raise GateError(
                "source snapshot member cannot be read: %s (errno=%s)" %
                (relative_name, error.errno))
        require(before.st_dev == after.st_dev and
                before.st_ino == after.st_ino and
                before.st_size == after.st_size and
                before.st_mtime_ns == after.st_mtime_ns and
                len(content) == before.st_size,
                "source snapshot member changed while hashing: %s" %
                relative_name)
        path_bytes = relative_name.encode("utf-8")
        aggregate.update(struct.pack(">I", len(path_bytes)))
        aggregate.update(path_bytes)
        aggregate.update(struct.pack(">Q", len(content)))
        aggregate.update(content)
        files.append({
            "path": relative_name,
            "size": len(content),
            "sha256": hashlib.sha256(content).hexdigest(),
        })
    return {
        "schema_version": 1,
        "algorithm": "sha256-domain-u32be-path-u64be-content-v1",
        "ordered": True,
        "file_count": len(files),
        "sha256": aggregate.hexdigest(),
        "files": files,
    }


def load_json_no_duplicates(path):
    def no_duplicates(pairs):
        value = {}
        for key, item in pairs:
            require(key not in value, "expected manifest has a duplicate key")
            value[key] = item
        return value

    try:
        return json.loads(path.read_text(encoding="utf-8"),
                          object_pairs_hook=no_duplicates)
    except OSError as error:
        raise GateError("expected manifest cannot be read (errno=%s)" %
                        error.errno)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise GateError("expected manifest is invalid JSON (%s)" %
                        error.__class__.__name__)


def sanitize_text(value, private_values=()):
    text = str(value)
    replacements = set()
    for private in private_values:
        if not private:
            continue
        replacements.add(str(private))
        try:
            replacements.add(str(Path(private).resolve(strict=False)))
        except OSError:
            pass
    for private in sorted(replacements, key=len, reverse=True):
        text = text.replace(private, "<redacted-path>")
    text = ABSOLUTE_PATH.sub("<redacted-path>", text)
    text = "".join(character if character == "\t" or ord(character) >= 0x20
                   else "?" for character in text)
    return text[:1000]


class EvidenceWriter:
    def __init__(self, output_dir, prefix):
        require(SAFE_PREFIX.fullmatch(prefix) is not None,
                "output prefix is not a safe filename")
        try:
            directory = Path(output_dir)
            directory.mkdir(parents=True, exist_ok=True)
            require(directory.is_dir(), "evidence destination is not a directory")
            self.log_path = directory / (prefix + ".log")
            self.manifest_path = directory / (prefix + ".json")
            require(not self.log_path.exists() and
                    not self.manifest_path.exists(),
                    "evidence filename already exists")
            self.stream = self.log_path.open("x", encoding="utf-8")
        except GateError:
            raise
        except OSError as error:
            raise GateError("evidence destination cannot be created (errno=%s)" %
                            error.errno)

    def log(self, message):
        safe = sanitize_text(message)
        print(safe)
        self.stream.write(safe + "\n")
        self.stream.flush()
        os.fsync(self.stream.fileno())

    def write_manifest(self, manifest):
        temporary = None
        try:
            temporary = tempfile.NamedTemporaryFile(
                mode="x", encoding="utf-8", dir=self.manifest_path.parent,
                prefix=".m10-manifest.", suffix=".tmp", delete=False)
            json.dump(manifest, temporary, indent=2, sort_keys=True)
            temporary.write("\n")
            temporary.flush()
            os.fsync(temporary.fileno())
            temporary.close()
            os.replace(temporary.name, self.manifest_path)
            temporary = None
        except OSError as error:
            raise GateError("evidence manifest cannot be written (errno=%s)" %
                            error.errno)
        finally:
            if temporary is not None:
                name = temporary.name
                temporary.close()
                try:
                    os.unlink(name)
                except OSError:
                    pass

    def close(self):
        self.stream.close()


class Elf64View:
    ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
    PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
    DYNAMIC_ENTRY = struct.Struct("<qQ")
    RELA_ENTRY = struct.Struct("<QQq")
    SYMBOL_ENTRY = struct.Struct("<IBBHQQ")

    def __init__(self, image, logical_id):
        self.image = image
        self.size = len(image)
        self.logical_id = logical_id
        self.program_headers = []
        self.loads = []
        self.dynamic = collections.defaultdict(list)
        self.relocations = []

    def fail(self, message):
        raise GateError("guest=%s %s" % (self.logical_id, message))

    def unpack(self, layout, offset, message):
        if offset < 0 or offset > self.size - layout.size:
            self.fail(message)
        return layout.unpack_from(self.image, offset)

    def one_tag(self, tag, required=False):
        values = self.dynamic.get(tag, [])
        if required and len(values) != 1:
            self.fail("dynamic tag %d must occur exactly once" % tag)
        if len(values) > 1:
            self.fail("dynamic tag %d is duplicated" % tag)
        return values[0] if values else None

    def vaddr_offset(self, address, length, message):
        for header in self.loads:
            delta = address - header[3]
            if delta < 0 or delta > header[5]:
                continue
            if length <= header[5] - delta:
                offset = checked_add(header[2], delta, self.size, message)
                if length <= self.size - offset:
                    return offset
        self.fail(message)

    def writable_vaddr(self, address, length):
        for header in self.loads:
            if not (header[1] & PF_W):
                continue
            delta = address - header[3]
            if delta >= 0 and delta <= header[6] and length <= header[6] - delta:
                return True
        return False

    def parse_header_and_programs(self, abi):
        fields = self.unpack(self.ELF_HEADER, 0, "ELF header is truncated")
        ident = fields[0]
        require(ident[:4] == ELF_MAGIC,
                "guest=%s ELF magic mismatch" % self.logical_id)
        observed_abi = {
            "elf_class": ident[4],
            "elf_data": ident[5],
            "elf_version": ident[6],
            "osabi": ident[7],
            "abi_version": ident[8],
            "elf_type": fields[1],
            "machine": fields[2],
            "flags": fields[7],
        }
        require(observed_abi == abi,
                "guest=%s ELF64 AArch64 ABI mismatch" % self.logical_id)
        require(fields[3] == EV_CURRENT and fields[8] == 64 and
                fields[9] == self.PROGRAM_HEADER.size,
                "guest=%s ELF/program header layout mismatch" %
                self.logical_id)
        program_offset = fields[5]
        program_count = fields[10]
        require(program_count > 0,
                "guest=%s has no program headers" % self.logical_id)
        table_size = program_count * self.PROGRAM_HEADER.size
        require(program_offset <= self.size and
                table_size <= self.size - program_offset,
                "guest=%s program table is out of bounds" % self.logical_id)
        for index in range(program_count):
            header = self.unpack(
                self.PROGRAM_HEADER,
                program_offset + index * self.PROGRAM_HEADER.size,
                "program header is truncated")
            p_type, p_flags, p_offset, _, _, p_filesz, p_memsz, _ = header
            if p_filesz:
                require(p_offset <= self.size and
                        p_filesz <= self.size - p_offset,
                        "guest=%s segment is outside the file" %
                        self.logical_id)
            require(p_memsz >= p_filesz,
                    "guest=%s segment filesz exceeds memsz" %
                    self.logical_id)
            require(not (p_flags & PF_W and p_flags & PF_X),
                    "guest=%s contains a writable-executable segment" %
                    self.logical_id)
            self.program_headers.append(header)
            if p_type == PT_LOAD:
                self.loads.append(header)
        require(not any(header[0] == PT_INTERP
                        for header in self.program_headers),
                "guest=%s contains forbidden PT_INTERP" % self.logical_id)
        require(not any(header[0] == PT_TLS
                        for header in self.program_headers),
                "guest=%s contains forbidden PT_TLS" % self.logical_id)
        return observed_abi

    def parse_dynamic(self):
        dynamic_headers = [header for header in self.program_headers
                           if header[0] == PT_DYNAMIC]
        require(len(dynamic_headers) == 1,
                "guest=%s must contain exactly one PT_DYNAMIC" %
                self.logical_id)
        header = dynamic_headers[0]
        require(header[5] % self.DYNAMIC_ENTRY.size == 0,
                "guest=%s PT_DYNAMIC size is misaligned" % self.logical_id)
        found_null = False
        for offset in range(header[2], header[2] + header[5],
                            self.DYNAMIC_ENTRY.size):
            tag, value = self.unpack(self.DYNAMIC_ENTRY, offset,
                                     "PT_DYNAMIC is truncated")
            if tag == DT_NULL:
                found_null = True
                break
            self.dynamic[tag].append(value)
        require(found_null,
                "guest=%s PT_DYNAMIC lacks DT_NULL" % self.logical_id)
        for tag, name in FORBIDDEN_DYNAMIC_TAGS.items():
            require(tag not in self.dynamic,
                    "guest=%s contains forbidden %s" %
                    (self.logical_id, name))
        flags = self.one_tag(DT_FLAGS) or 0
        flags_1 = self.one_tag(DT_FLAGS_1) or 0
        require(not (flags & DF_TEXTREL),
                "guest=%s has forbidden DF_TEXTREL" % self.logical_id)
        require(not (flags_1 & DF_1_PIE),
                "guest=%s has forbidden DF_1_PIE" % self.logical_id)

    def derive_symbol_count(self):
        counts = []
        hash_address = self.one_tag(DT_HASH)
        if hash_address is not None:
            offset = self.vaddr_offset(hash_address, 8,
                                       "DT_HASH is out of bounds")
            _, chains = struct.unpack_from("<II", self.image, offset)
            counts.append(chains)
        gnu_address = self.one_tag(DT_GNU_HASH)
        if gnu_address is not None:
            offset = self.vaddr_offset(gnu_address, 16,
                                       "DT_GNU_HASH is out of bounds")
            buckets, symbol_offset, bloom_count, _ = struct.unpack_from(
                "<IIII", self.image, offset)
            require(buckets > 0 and bloom_count > 0,
                    "guest=%s malformed DT_GNU_HASH" % self.logical_id)
            bucket_offset = offset + 16 + bloom_count * 8
            require(bucket_offset <= self.size and
                    buckets * 4 <= self.size - bucket_offset,
                    "guest=%s GNU hash buckets are out of bounds" %
                    self.logical_id)
            bucket_values = struct.unpack_from(
                "<%dI" % buckets, self.image, bucket_offset)
            chain_base = bucket_offset + buckets * 4
            maximum = symbol_offset
            for first_symbol in bucket_values:
                if first_symbol == 0:
                    continue
                require(first_symbol >= symbol_offset,
                        "guest=%s GNU hash bucket precedes symbol offset" %
                        self.logical_id)
                symbol = first_symbol
                while True:
                    chain_index = symbol - symbol_offset
                    chain_position = chain_base + chain_index * 4
                    require(chain_position <= self.size - 4,
                            "guest=%s GNU hash chain is out of bounds" %
                            self.logical_id)
                    chain = struct.unpack_from("<I", self.image,
                                               chain_position)[0]
                    symbol += 1
                    maximum = max(maximum, symbol)
                    if chain & 1:
                        break
            counts.append(maximum)
        require(counts and all(count == counts[0] for count in counts),
                "guest=%s dynamic hash symbol counts disagree" %
                self.logical_id)
        return counts[0]

    def parse_symbols_and_strings(self, expected_count):
        string_address = self.one_tag(DT_STRTAB, required=True)
        string_size = self.one_tag(DT_STRSZ, required=True)
        symbol_address = self.one_tag(DT_SYMTAB, required=True)
        symbol_entry_size = self.one_tag(DT_SYMENT, required=True)
        require(string_size > 0 and symbol_entry_size == self.SYMBOL_ENTRY.size,
                "guest=%s dynamic string/symbol layout mismatch" %
                self.logical_id)
        string_offset = self.vaddr_offset(string_address, string_size,
                                          "DT_STRTAB is out of bounds")
        symbol_count = self.derive_symbol_count()
        require(symbol_count == expected_count,
                "guest=%s dynamic symbol count mismatch" % self.logical_id)
        symbol_size = symbol_count * self.SYMBOL_ENTRY.size
        symbol_offset = self.vaddr_offset(symbol_address, symbol_size,
                                          "DT_SYMTAB is out of bounds")
        strings = self.image[string_offset:string_offset + string_size]

        def dynamic_string(offset):
            require(offset < len(strings),
                    "guest=%s dynamic string offset is out of bounds" %
                    self.logical_id)
            bounded_end = min(len(strings), offset +
                              MAX_DYNAMIC_NAME_LENGTH + 1)
            end = strings.find(b"\0", offset, bounded_end)
            require(end >= 0,
                    "guest=%s dynamic string is unterminated or too long" %
                    self.logical_id)
            try:
                return strings[offset:end].decode("utf-8")
            except UnicodeDecodeError:
                self.fail("dynamic string is not UTF-8")

        forbidden_types = collections.Counter()
        exports_by_name = collections.defaultdict(list)
        eligible = 0
        for index in range(symbol_count):
            symbol = self.unpack(
                self.SYMBOL_ENTRY,
                symbol_offset + index * self.SYMBOL_ENTRY.size,
                "dynamic symbol table is truncated")
            require(symbol[0] < string_size,
                    "guest=%s dynamic symbol name is out of bounds" %
                    self.logical_id)
            symbol_type = symbol[1] & 0x0F
            if symbol_type in (STT_TLS, STT_GNU_IFUNC):
                forbidden_types[symbol_type] += 1
            binding = symbol[1] >> 4
            visibility = symbol[2] & 0x03
            if (symbol[3] != SHN_UNDEF and symbol[0] != 0 and
                    symbol[4] != 0 and
                    binding in (STB_GLOBAL, STB_WEAK) and
                    symbol_type in (STT_NOTYPE, STT_OBJECT, STT_FUNC) and
                    visibility not in (STV_HIDDEN, STV_INTERNAL)):
                name = dynamic_string(symbol[0])
                require(name != "",
                        "guest=%s eligible export has an empty name" %
                        self.logical_id)
                exports_by_name[name].append({
                    "address": symbol[4],
                    "weak": binding == STB_WEAK,
                })
                eligible += 1
        require(not forbidden_types,
                "guest=%s contains forbidden TLS/IFUNC symbols" %
                self.logical_id)

        registry_exports = inventory_registry_exports(exports_by_name)
        require(registry_exports["eligible"] == eligible,
                "guest=%s registry export inventory lost candidates" %
                self.logical_id)

        needed = [dynamic_string(offset)
                  for offset in self.dynamic.get(DT_NEEDED, [])]
        return symbol_count, needed, registry_exports

    def parse_relocations(self):
        ranges = []

        def append_table(address, size, label):
            require(size % self.RELA_ENTRY.size == 0,
                    "guest=%s %s size is misaligned" %
                    (self.logical_id, label))
            offset = self.vaddr_offset(address, size,
                                       "%s is out of bounds" % label)
            end = offset + size
            for existing_start, existing_end in ranges:
                require(end <= existing_start or offset >= existing_end,
                        "guest=%s relocation tables overlap" %
                        self.logical_id)
            ranges.append((offset, end))
            for position in range(offset, end, self.RELA_ENTRY.size):
                self.relocations.append(self.unpack(
                    self.RELA_ENTRY, position,
                    "%s entry is truncated" % label))

        rela_address = self.one_tag(DT_RELA, required=True)
        rela_size = self.one_tag(DT_RELASZ, required=True)
        require(self.one_tag(DT_RELAENT, required=True) ==
                self.RELA_ENTRY.size,
                "guest=%s DT_RELAENT mismatch" % self.logical_id)
        append_table(rela_address, rela_size, "DT_RELA")
        jump_address = self.one_tag(DT_JMPREL, required=True)
        jump_size = self.one_tag(DT_PLTRELSZ, required=True)
        require(self.one_tag(DT_PLTREL, required=True) == DT_RELA,
                "guest=%s PLT relocations are not RELA" % self.logical_id)
        append_table(jump_address, jump_size, "DT_JMPREL")

        counts = collections.Counter()
        for relocation_offset, relocation_info, _ in self.relocations:
            relocation_type = relocation_info & 0xFFFFFFFF
            symbol_index = relocation_info >> 32
            require(relocation_type in RELOCATION_NAMES,
                    "guest=%s unsupported relocation type=%d" %
                    (self.logical_id, relocation_type))
            if relocation_type == R_AARCH64_RELATIVE:
                require(symbol_index == 0,
                        "guest=%s RELATIVE has a nonzero symbol index" %
                        self.logical_id)
            require(self.writable_vaddr(relocation_offset, 8),
                    "guest=%s relocation target is not writable" %
                    self.logical_id)
            counts[RELOCATION_NAMES[relocation_type]] += 1
        return dict(sorted(counts.items()))

    def inspect_initializers(self):
        require(DT_INIT not in self.dynamic,
                "guest=%s contains unexpected DT_INIT" % self.logical_id)
        addresses = self.dynamic.get(DT_INIT_ARRAY, [])
        sizes = self.dynamic.get(DT_INIT_ARRAYSZ, [])
        require(len(addresses) == len(sizes) and len(addresses) <= 1,
                "guest=%s INIT_ARRAY metadata is inconsistent" %
                self.logical_id)
        if not addresses:
            return 0, {}
        require(sizes[0] % 8 == 0,
                "guest=%s INIT_ARRAY size is misaligned" % self.logical_id)
        self.vaddr_offset(addresses[0], sizes[0],
                          "INIT_ARRAY is out of bounds")
        relocation_counts = collections.Counter()
        start = addresses[0]
        end = start + sizes[0]
        for relocation_offset, relocation_info, _ in self.relocations:
            if start <= relocation_offset < end:
                require((relocation_offset - start) % 8 == 0,
                        "guest=%s INIT_ARRAY relocation is misaligned" %
                        self.logical_id)
                relocation_type = relocation_info & 0xFFFFFFFF
                relocation_counts[RELOCATION_NAMES[relocation_type]] += 1
        require(sum(relocation_counts.values()) == sizes[0] // 8,
                "guest=%s INIT_ARRAY slots are not fully relocated" %
                self.logical_id)
        return sizes[0] // 8, dict(sorted(relocation_counts.items()))


def validate_guest(path, expected, required_abi):
    logical_id = expected["id"]
    started = time.monotonic_ns()
    try:
        file_stat = os.stat(path, follow_symlinks=True)
    except OSError as error:
        raise GateError("guest=%s input unavailable (errno=%s)" %
                        (logical_id, error.errno))
    require(stat.S_ISREG(file_stat.st_mode),
            "guest=%s input is not a regular file" % logical_id)
    require(file_stat.st_size == expected["size"],
            "guest=%s file size mismatch" % logical_id)
    digest = sha256_file(path)
    require(digest == expected["sha256"],
            "guest=%s SHA-256 mismatch" % logical_id)
    try:
        with open(path, "rb", buffering=0) as stream:
            opened_stat = os.fstat(stream.fileno())
            require(opened_stat.st_dev == file_stat.st_dev and
                    opened_stat.st_ino == file_stat.st_ino and
                    opened_stat.st_size == file_stat.st_size,
                    "guest=%s input changed before static inspection" %
                    logical_id)
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
                elf = Elf64View(image, logical_id)
                abi = elf.parse_header_and_programs(required_abi)
                load_flags = [header[1] for header in elf.loads]
                require(load_flags == expected["load_segment_flags"],
                        "guest=%s PT_LOAD layout mismatch" % logical_id)
                elf.parse_dynamic()
                symbol_count, needed, registry_exports = (
                    elf.parse_symbols_and_strings(
                        expected["dynamic_symbol_count"]))
                require(needed == expected["needed"],
                        "guest=%s DT_NEEDED list mismatch" % logical_id)
                require(registry_exports == expected["registry_exports"],
                        "guest=%s registry export inventory mismatch" %
                        logical_id)
                relocations = elf.parse_relocations()
                require(relocations == expected["relocations"],
                        "guest=%s RELA counts mismatch" % logical_id)
                init_entries, init_relocations = elf.inspect_initializers()
                require(init_entries == expected["init_array_entries"] and
                        init_relocations == expected["init_array_relocations"],
                        "guest=%s INIT_ARRAY inventory mismatch" % logical_id)
    except GateError:
        raise
    except OSError as error:
        raise GateError("guest=%s static read failed (errno=%s)" %
                        (logical_id, error.errno))
    report = {
        "id": logical_id,
        "sha256": digest,
        "size": file_stat.st_size,
        "abi": abi,
        "pt_load_count": len(load_flags),
        "pt_load_flags": load_flags,
        "dynamic_symbol_count": symbol_count,
        "registry_exports": registry_exports,
        "relocations": relocations,
        "relocation_count": sum(relocations.values()),
        "init_array_entries": init_entries,
        "init_array_relocations": init_relocations,
        "needed": needed,
        "prohibitions": {
            "pt_interp": 0,
            "pt_tls": 0,
            "writable_executable_segments": 0,
            "textrel": 0,
            "packed_or_relr": 0,
            "tls_or_ifunc_symbols": 0
        },
        "static_validation_elapsed_ns": time.monotonic_ns() - started,
        "static_validation": "PASS",
    }
    return report


def _condition_wait_until(condition, predicate, deadline_ns):
    """Wait on a kernel-backed condition until predicate or monotonic deadline."""
    with condition:
        while not predicate():
            remaining_ns = deadline_ns - time.monotonic_ns()
            if remaining_ns <= 0:
                return False
            condition.wait(remaining_ns / 1_000_000_000)
        return True


def run_child(argv, timeout_seconds=DEFAULT_TIMEOUT_SECONDS,
              output_limit_bytes=DEFAULT_OUTPUT_LIMIT_BYTES,
              term_grace_seconds=TERM_GRACE_SECONDS):
    """Run one child with bounded output and PID-specific safe termination."""
    require(sys.platform.startswith("linux") and hasattr(os, "wait4") and
            hasattr(os, "pidfd_open") and
            hasattr(signal, "pidfd_send_signal"),
            "safe inspector supervision requires Linux pidfd and wait4")
    require(isinstance(timeout_seconds, (int, float)) and
            math.isfinite(timeout_seconds) and
            0 < timeout_seconds <= MAX_TIMEOUT_SECONDS,
            "subprocess timeout must be finite, positive and bounded")
    require(isinstance(term_grace_seconds, (int, float)) and
            math.isfinite(term_grace_seconds) and term_grace_seconds >= 0,
            "TERM grace must be finite and nonnegative")
    require(isinstance(output_limit_bytes, int) and
            0 < output_limit_bytes <= MAX_OUTPUT_LIMIT_BYTES,
            "subprocess output limit must be positive and bounded")
    environment = {
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
    }
    started = time.monotonic_ns()
    timeout_ns = max(1, int(timeout_seconds * 1_000_000_000))
    deadline_ns = started + timeout_ns
    condition = threading.Condition()
    state = {
        "exited": False,
        "wait_status": None,
        "usage": None,
        "wait_error": None,
        "overflow": set(),
        "reader_error": None,
    }
    buffers = {"stdout": bytearray(), "stderr": bytearray()}
    byte_counts = {"stdout": 0, "stderr": 0}
    process = None
    pidfd = None
    reader_threads = []
    waiter_thread = None
    termination_signal = None
    timed_out = False

    def read_stream(name, stream):
        try:
            while True:
                chunk = stream.read(64 * 1024)
                if not chunk:
                    break
                byte_counts[name] += len(chunk)
                remaining = output_limit_bytes - len(buffers[name])
                if remaining > 0:
                    buffers[name].extend(chunk[:remaining])
                if byte_counts[name] > output_limit_bytes:
                    with condition:
                        state["overflow"].add(name)
                        condition.notify_all()
        except OSError as error:
            with condition:
                state["reader_error"] = error.errno
                condition.notify_all()
        finally:
            try:
                stream.close()
            except OSError:
                pass

    def wait_child():
        try:
            while True:
                try:
                    _, wait_status, usage = os.wait4(process.pid, 0)
                    break
                except InterruptedError:
                    continue
            process.returncode = os.waitstatus_to_exitcode(wait_status)
            with condition:
                state["wait_status"] = wait_status
                state["usage"] = usage
                state["exited"] = True
                condition.notify_all()
        except OSError as error:
            with condition:
                state["wait_error"] = error.errno
                condition.notify_all()

    def send_child_signal(selected_signal):
        try:
            signal.pidfd_send_signal(pidfd, selected_signal)
            return True
        except ProcessLookupError:
            return False
        except OSError as error:
            if error.errno == errno.ESRCH:
                return False
            raise GateError("cannot signal inspector child (errno=%s)" %
                            error.errno)

    try:
        process = subprocess.Popen(
            argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, close_fds=True, env=environment,
            bufsize=0)
        try:
            pidfd = os.pidfd_open(process.pid, 0)
        except OSError as error:
            process.kill()
            process.wait()
            raise GateError("cannot acquire inspector pidfd (errno=%s)" %
                            error.errno)

        for name, stream in (("stdout", process.stdout),
                             ("stderr", process.stderr)):
            thread = threading.Thread(
                target=read_stream, args=(name, stream),
                name="m10-inspector-%s" % name)
            thread.start()
            reader_threads.append(thread)
        waiter_thread = threading.Thread(
            target=wait_child, name="m10-inspector-wait")
        waiter_thread.start()

        def child_or_fault():
            return (state["exited"] or bool(state["overflow"]) or
                    state["reader_error"] is not None or
                    state["wait_error"] is not None)

        observed = _condition_wait_until(
            condition, child_or_fault, deadline_ns)
        timed_out = not observed
        with condition:
            needs_termination = (
                timed_out or bool(state["overflow"]) or
                state["reader_error"] is not None or
                state["wait_error"] is not None)
            already_exited = state["exited"]

        if needs_termination and not already_exited:
            if send_child_signal(signal.SIGTERM):
                termination_signal = "SIGTERM"
            term_deadline_ns = time.monotonic_ns() + max(
                1, int(term_grace_seconds * 1_000_000_000))
            exited_after_term = _condition_wait_until(
                condition, lambda: state["exited"] or
                state["wait_error"] is not None, term_deadline_ns)
            with condition:
                still_running = not state["exited"]
            if not exited_after_term or still_running:
                if send_child_signal(signal.SIGKILL):
                    termination_signal = "SIGKILL"
                with condition:
                    while (not state["exited"] and
                           state["wait_error"] is None):
                        condition.wait()

        if waiter_thread is not None:
            waiter_thread.join()
        with condition:
            wait_error = state["wait_error"]
        if wait_error is not None:
            # pidfd keeps the identity stable; ensure no live child is leaked.
            send_child_signal(signal.SIGKILL)
            try:
                while True:
                    try:
                        _, wait_status, usage = os.wait4(process.pid, 0)
                        break
                    except InterruptedError:
                        continue
                process.returncode = os.waitstatus_to_exitcode(wait_status)
                with condition:
                    state["wait_status"] = wait_status
                    state["usage"] = usage
                    state["exited"] = True
            except ChildProcessError:
                pass
            raise GateError("cannot wait for inspector child (errno=%s)" %
                            wait_error)

        for thread in reader_threads:
            thread.join()
        with condition:
            reader_error = state["reader_error"]
            usage = state["usage"]
            output_streams = sorted(state["overflow"])
        require(reader_error is None,
                "cannot read inspector output (errno=%s)" % reader_error)
        require(usage is not None and process.returncode is not None,
                "inspector child was not collected")
    except GateError:
        raise
    except OSError as error:
        if process is not None and pidfd is not None:
            try:
                signal.pidfd_send_signal(pidfd, signal.SIGKILL)
            except OSError:
                pass
            if waiter_thread is not None:
                waiter_thread.join()
        raise GateError("inspector subprocess failed (errno=%s)" % error.errno)
    except Exception as error:
        raise GateError("inspector supervisor failed (%s)" %
                        error.__class__.__name__)
    finally:
        if process is not None and waiter_thread is not None:
            with condition:
                child_live = (not state["exited"] and
                              state["wait_error"] is None)
            if child_live and pidfd is not None:
                try:
                    signal.pidfd_send_signal(pidfd, signal.SIGKILL)
                except OSError:
                    pass
            waiter_thread.join()
            for thread in reader_threads:
                thread.join()
        elif process is not None and process.returncode is None:
            process.kill()
            process.wait()
        for thread in reader_threads:
            thread.join()
        if pidfd is not None:
            os.close(pidfd)

    return {
        "returncode": process.returncode,
        "stdout": bytes(buffers["stdout"]).decode("utf-8", "replace"),
        "stderr": bytes(buffers["stderr"]).decode("utf-8", "replace"),
        "stdout_bytes": byte_counts["stdout"],
        "stderr_bytes": byte_counts["stderr"],
        "stdout_truncated": byte_counts["stdout"] > output_limit_bytes,
        "stderr_truncated": byte_counts["stderr"] > output_limit_bytes,
        "output_limit_bytes": output_limit_bytes,
        "output_limit_exceeded": bool(output_streams),
        "output_limit_streams": output_streams,
        "timed_out": timed_out,
        "timeout_seconds": timeout_seconds,
        "termination_signal": termination_signal,
        "elapsed_ns": time.monotonic_ns() - started,
        "max_rss_kib": int(usage.ru_maxrss),
        "max_rss_source": "wait4-ru_maxrss",
    }


def require_child_success(result, purpose):
    require(not result["timed_out"],
            "%s timed out after %s seconds" %
            (purpose, result["timeout_seconds"]))
    require(not result["output_limit_exceeded"],
            "%s exceeded output limit on %s" %
            (purpose, ",".join(result["output_limit_streams"])))


def verify_inspector_support(inspector, timeout_seconds,
                             output_limit_bytes):
    result = run_child([inspector], timeout_seconds, output_limit_bytes)
    require_child_success(result, "inspector preflight")
    combined = result["stdout"] + "\n" + result["stderr"]
    require(result["returncode"] == 2 and "--relocate" in combined and
            "--exports" in combined,
            "inspector does not advertise required --relocate/--exports modes")
    return {
        "sha256": sha256_file(inspector),
        "relocate_supported": True,
        "exports_supported": True,
        "preflight_elapsed_ns": result["elapsed_ns"],
        "preflight_max_rss_kib": result["max_rss_kib"],
        "preflight_max_rss_source": result["max_rss_source"],
        "preflight_stdout_bytes": result["stdout_bytes"],
        "preflight_stderr_bytes": result["stderr_bytes"],
        "preflight_timed_out": result["timed_out"],
        "preflight_output_limit_exceeded": result["output_limit_exceeded"],
        "preflight_termination_signal": result["termination_signal"],
        "timeout_seconds": timeout_seconds,
        "output_limit_bytes_per_stream": output_limit_bytes,
    }


def expected_inspector_header(static_report):
    return {
        "arch": "AArch64/ELF64",
        "flags": "0x0",
        "arm_float_abi": "not-applicable",
        "segments": str(static_report["pt_load_count"]),
        "symbols": str(static_report["dynamic_symbol_count"]),
        "relocations": str(static_report["relocation_count"]),
        "needed": str(len(static_report["needed"])),
    }


def parse_inspector_common(stdout, static_report):
    logical_id = static_report["id"]
    headers = [line for line in stdout.splitlines()
               if line.startswith("arch=")]
    relocations = [line for line in stdout.splitlines()
                   if line.startswith("relocate=")]
    require(len(headers) == 1 and relocations == ["relocate=success"],
            "guest=%s inspector output requires one relocate=success" %
            logical_id)
    header_pairs = INSPECT_HEADER.findall(headers[0])
    require(len({key for key, _ in header_pairs}) == len(header_pairs),
            "guest=%s inspector header contains duplicate fields" %
            logical_id)
    header = dict(header_pairs)
    expected_header = expected_inspector_header(static_report)
    for field, value in expected_header.items():
        require(header.get(field) == value,
                "guest=%s inspector %s mismatch" % (logical_id, field))
    return expected_header


def parse_export_report(stdout, logical_id):
    export_lines = [line for line in stdout.splitlines()
                    if line.startswith("exports=")]
    require(len(export_lines) == 1,
            "guest=%s inspector output requires one exports report" %
            logical_id)
    match = EXPORT_REPORT.fullmatch(export_lines[0])
    require(match is not None,
            "guest=%s inspector exports report is malformed" % logical_id)
    return {
        "added": int(match.group(1), 10),
        "equivalent": int(match.group(2), 10),
    }


def inspect_relocate(inspector, guest_path, static_report, private_values,
                     timeout_seconds, output_limit_bytes):
    result = run_child([inspector, guest_path, "--relocate"],
                       timeout_seconds, output_limit_bytes)
    require_child_success(result, "guest=%s inspector relocate" %
                          static_report["id"])
    stdout_digest = hashlib.sha256(result["stdout"].encode("utf-8")).hexdigest()
    stderr_digest = hashlib.sha256(result["stderr"].encode("utf-8")).hexdigest()
    if result["returncode"] != 0:
        diagnostic = sanitize_text(result["stderr"], private_values).splitlines()
        detail = diagnostic[0] if diagnostic else "no diagnostic"
        raise GateError("guest=%s inspector relocate failed rc=%d detail=%s" %
                        (static_report["id"], result["returncode"], detail))
    expected_header = parse_inspector_common(result["stdout"], static_report)
    return {
        "mode": "--relocate",
        "status": "PASS",
        "elapsed_ns": result["elapsed_ns"],
        "max_rss_kib": result["max_rss_kib"],
        "max_rss_source": result["max_rss_source"],
        "stdout_bytes": result["stdout_bytes"],
        "stderr_bytes": result["stderr_bytes"],
        "output_limit_bytes_per_stream": output_limit_bytes,
        "timeout_seconds": timeout_seconds,
        "timed_out": False,
        "output_limit_exceeded": False,
        "termination_signal": result["termination_signal"],
        "stdout_truncated": result["stdout_truncated"],
        "stderr_truncated": result["stderr_truncated"],
        "stdout_sha256": stdout_digest,
        "stderr_sha256": stderr_digest,
        "reported": expected_header,
        "resolve_executed": 0,
        "finalize_executed": 0,
        "guest_initializers_executed": 0,
        "guest_jni_onload_executed": 0,
    }


def inspect_exports(inspector, guest_path, static_report, private_values,
                    timeout_seconds, output_limit_bytes):
    result = run_child([inspector, guest_path, "--exports"],
                       timeout_seconds, output_limit_bytes)
    purpose = "guest=%s inspector exports" % static_report["id"]
    require_child_success(result, purpose)
    stdout_digest = hashlib.sha256(result["stdout"].encode("utf-8")).hexdigest()
    stderr_digest = hashlib.sha256(result["stderr"].encode("utf-8")).hexdigest()
    if result["returncode"] != 0:
        diagnostic = sanitize_text(result["stderr"], private_values).splitlines()
        detail = diagnostic[0] if diagnostic else "no diagnostic"
        raise GateError("guest=%s inspector exports failed rc=%d detail=%s" %
                        (static_report["id"], result["returncode"], detail))
    expected_header = parse_inspector_common(result["stdout"], static_report)
    reported_exports = parse_export_report(result["stdout"],
                                           static_report["id"])
    static_inventory = static_report["registry_exports"]
    require(reported_exports == {
        "added": static_inventory["added"],
        "equivalent": static_inventory["equivalent"],
    }, "guest=%s inspector registry export counts mismatch" %
        static_report["id"])
    return {
        "mode": "--exports",
        "status": "PASS",
        "eligible": static_inventory["eligible"],
        "added": reported_exports["added"],
        "equivalent": reported_exports["equivalent"],
        "static_inventory": dict(static_inventory),
        "elapsed_ns": result["elapsed_ns"],
        "max_rss_kib": result["max_rss_kib"],
        "max_rss_source": result["max_rss_source"],
        "stdout_bytes": result["stdout_bytes"],
        "stderr_bytes": result["stderr_bytes"],
        "output_limit_bytes_per_stream": output_limit_bytes,
        "timeout_seconds": timeout_seconds,
        "timed_out": False,
        "output_limit_exceeded": False,
        "termination_signal": result["termination_signal"],
        "stdout_truncated": result["stdout_truncated"],
        "stderr_truncated": result["stderr_truncated"],
        "stdout_sha256": stdout_digest,
        "stderr_sha256": stderr_digest,
        "reported": expected_header,
        "reported_exports": reported_exports,
        "relocate_executed": 1,
        "registry_create_executed": 1,
        "registry_add_module_executed": 1,
        "resolve_executed": 0,
        "finalize_executed": 0,
        "guest_initializers_executed": 0,
        "guest_jni_onload_executed": 0,
        "device_access": 0,
        "guest_files_copied": 0,
    }


def validate_expected_manifest(expected):
    require(expected.get("schema_version") == 1 and
            expected.get("milestone") == "M10" and
            expected.get("scope") ==
            "hash-pinned-local-read-only-aarch64-reference-gate",
            "expected manifest header mismatch")
    abi = expected.get("required_abi")
    require(abi == {
        "elf_class": ELFCLASS64,
        "elf_data": ELFDATA2LSB,
        "elf_version": EV_CURRENT,
        "osabi": ELFOSABI_SYSV,
        "abi_version": 0,
        "elf_type": ET_DYN,
        "machine": EM_AARCH64,
        "flags": 0,
    }, "expected manifest ABI policy mismatch")
    safety = expected.get("safety")
    require(safety == {
        "inspector_modes": ["--relocate", "--exports"],
        "resolve": False,
        "finalize": False,
        "guest_initializers_executed": False,
        "guest_jni_onload_executed": False,
        "device_access": False,
        "guest_files_copied": False,
    }, "expected manifest safety policy mismatch")
    guests = expected.get("guests")
    require(isinstance(guests, list) and len(guests) == 5,
            "expected manifest must contain exactly five guests")
    require([guest.get("argument") for guest in guests] == [
        "bully2", "sonic4", "horizon_unity", "horizon_il2cpp",
        "horizon_main"], "expected manifest guest order mismatch")
    require(len({guest.get("id") for guest in guests}) == 5 and
            len({guest.get("sha256") for guest in guests}) == 5,
            "expected manifest identities are not unique")
    for guest in guests:
        require(re.fullmatch(r"[0-9a-f]{64}", guest.get("sha256", "")) is
                not None, "expected manifest contains an invalid SHA-256")
        require(set(guest.get("relocations", {})).issubset(
                set(RELOCATION_NAMES.values())),
                "expected manifest contains an unsupported relocation")
        registry_exports = guest.get("registry_exports")
        require(isinstance(registry_exports, dict) and
                set(registry_exports) == set(REGISTRY_EXPORT_FIELDS),
                "expected manifest registry export fields mismatch")
        require(all(type(registry_exports[field]) is int and
                    registry_exports[field] >= 0
                    for field in REGISTRY_EXPORT_FIELDS),
                "expected manifest registry export counts are invalid")
        require(registry_exports["eligible"] ==
                registry_exports["added"] +
                registry_exports["equivalent"] +
                registry_exports["replaced_lower_priority"] +
                registry_exports["ignored_lower_priority"] +
                registry_exports["collisions"] and
                registry_exports["collisions"] == 0,
                "expected manifest registry export inventory is inconsistent")
    return guests, abi


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).replace(
        microsecond=0).isoformat().replace("+00:00", "Z")


def positive_finite_float(value):
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        raise argparse.ArgumentTypeError("must be a number")
    if (not math.isfinite(parsed) or parsed <= 0 or
            parsed > MAX_TIMEOUT_SECONDS):
        raise argparse.ArgumentTypeError(
            "must be finite, positive and at most %d" %
            MAX_TIMEOUT_SECONDS)
    return parsed


def positive_integer(value):
    try:
        parsed = int(value, 10)
    except (TypeError, ValueError):
        raise argparse.ArgumentTypeError("must be a base-10 integer")
    if parsed <= 0 or parsed > MAX_OUTPUT_LIMIT_BYTES:
        raise argparse.ArgumentTypeError(
            "must be positive and at most %d" % MAX_OUTPUT_LIMIT_BYTES)
    return parsed


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="optional read-only M10 gate for five approved guests")
    parser.add_argument("--bully2", help="Bully2 libGame.so path")
    parser.add_argument("--sonic4", help="Sonic4 EP2 libfox.so path")
    parser.add_argument("--horizon-unity", dest="horizon_unity",
                        help="Horizon Chase libunity.so path")
    parser.add_argument("--horizon-il2cpp", dest="horizon_il2cpp",
                        help="Horizon Chase libil2cpp.so path")
    parser.add_argument("--horizon-main", dest="horizon_main",
                        help="Horizon Chase libmain.so path")
    parser.add_argument("--inspector", help="nxloader_inspect executable")
    parser.add_argument("--output-dir", dest="output_dir",
                        help="directory for sanitized durable evidence")
    parser.add_argument("--output-prefix", default="m10-aarch64-guests-v1",
                        help="safe evidence filename prefix")
    parser.add_argument(
        "--timeout-seconds", type=positive_finite_float,
        help="monotonic timeout per inspector process (default: 120)")
    parser.add_argument(
        "--output-limit-bytes", type=positive_integer,
        help="captured byte limit per stdout/stderr stream (default: 262144)")
    return parser.parse_args()


def configured_values(arguments):
    values = {}
    for name, environment_name in ENV_BY_ARGUMENT.items():
        argument_value = getattr(arguments, name)
        environment_value = os.environ.get(environment_name)
        values[name] = argument_value if argument_value is not None \
            else environment_value
        if values[name] == "":
            values[name] = None
    return values


def configured_limits(arguments):
    timeout_value = arguments.timeout_seconds
    if timeout_value is None:
        environment_value = os.environ.get(LIMIT_ENV["timeout_seconds"])
        timeout_value = (DEFAULT_TIMEOUT_SECONDS if environment_value is None
                         else positive_finite_float(environment_value))
    output_value = arguments.output_limit_bytes
    if output_value is None:
        environment_value = os.environ.get(LIMIT_ENV["output_limit_bytes"])
        output_value = (DEFAULT_OUTPUT_LIMIT_BYTES if environment_value is None
                        else positive_integer(environment_value))
    return {
        "timeout_seconds": timeout_value,
        "output_limit_bytes_per_stream": output_value,
        "term_grace_seconds": TERM_GRACE_SECONDS,
    }


def emit_skip(values, prefix, missing, limits):
    line = ("m10-aarch64-guest-gate: SKIP missing=%s "
            "guest_initializers_executed=0 guest_jni_onload_executed=0 "
            "device_access=0 guest_files_copied=0" % ",".join(missing))
    output_dir = values.get("output_dir")
    if output_dir is None:
        print(line)
        return 0
    writer = EvidenceWriter(output_dir, prefix)
    manifest = {
        "schema_version": 1,
        "milestone": "M10",
        "status": "SKIP",
        "reason": "missing_explicit_inputs",
        "missing": missing,
        "created_utc": utc_now(),
        "limits": limits,
        "guests": [],
        "safety": {
            "device_access": 0,
            "guest_files_copied": 0,
            "guest_initializers_executed": 0,
            "guest_jni_onload_executed": 0,
            "resolve_executed": 0,
            "finalize_executed": 0,
        },
    }
    try:
        writer.log(line)
        writer.write_manifest(manifest)
        writer.log("evidence_log=%s evidence_manifest=%s" %
                   (writer.log_path.name, writer.manifest_path.name))
    finally:
        writer.close()
    return 0


def run_gate(arguments, values, limits):
    expected = load_json_no_duplicates(EXPECTED_PATH)
    guests, required_abi = validate_expected_manifest(expected)
    missing = [name for name in ENV_BY_ARGUMENT if not values.get(name)]
    if missing:
        return emit_skip(values, arguments.output_prefix, missing, limits)

    private_values = list(values.values())
    writer = EvidenceWriter(values["output_dir"], arguments.output_prefix)
    manifest = {
        "schema_version": 1,
        "milestone": "M10",
        "scope": "hash-pinned-local-read-only-aarch64-reference-gate",
        "status": "RUNNING",
        "created_utc": utc_now(),
        "expected_manifest_sha256": sha256_file(EXPECTED_PATH),
        "source_snapshot": {},
        "limits": limits,
        "inspector": {},
        "guests": [],
        "safety": {
            "device_access": 0,
            "guest_files_copied": 0,
            "guest_initializers_executed": 0,
            "guest_jni_onload_executed": 0,
            "resolve_executed": 0,
            "finalize_executed": 0,
        },
    }
    started = time.monotonic_ns()
    try:
        manifest["source_snapshot"] = build_source_snapshot()
        writer.log(
            "m10-aarch64-guest-gate: START guests=5 "
            "modes=--relocate,--exports "
            "timeout_seconds=%s output_limit_bytes_per_stream=%d" %
            (limits["timeout_seconds"],
             limits["output_limit_bytes_per_stream"]))
        writer.log("source_snapshot_sha256=%s files=%d" %
                   (manifest["source_snapshot"]["sha256"],
                    manifest["source_snapshot"]["file_count"]))
        inspector_stat = os.stat(values["inspector"], follow_symlinks=True)
        require(stat.S_ISREG(inspector_stat.st_mode) and
                os.access(values["inspector"], os.X_OK),
                "inspector is not an executable regular file")
        manifest["inspector"] = verify_inspector_support(
            values["inspector"], limits["timeout_seconds"],
            limits["output_limit_bytes_per_stream"])
        manifest["inspector"]["source_snapshot_sha256"] = (
            manifest["source_snapshot"]["sha256"])
        writer.log("inspector_support=PASS modes=--relocate,--exports "
                   "sha256=%s" %
                   manifest["inspector"]["sha256"])

        for expected_guest in guests:
            guest_path = values[expected_guest["argument"]]
            guest_started = time.monotonic_ns()
            report = validate_guest(guest_path, expected_guest, required_abi)
            writer.log(
                "guest=%s static=PASS sha256=%s pt_load=%d symbols=%d "
                "relocations=%d init_array=%d registry_eligible=%d" %
                (report["id"], report["sha256"], report["pt_load_count"],
                 report["dynamic_symbol_count"], report["relocation_count"],
                 report["init_array_entries"],
                 report["registry_exports"]["eligible"]))
            report["inspector"] = inspect_relocate(
                values["inspector"], guest_path, report, private_values,
                limits["timeout_seconds"],
                limits["output_limit_bytes_per_stream"])
            require(sha256_file(guest_path) == expected_guest["sha256"],
                    "guest=%s changed during relocate run" % report["id"])
            writer.log(
                "guest=%s relocate=PASS monotonic_elapsed_ns=%d "
                "max_rss_kib=%d guest_initializers_executed=0 "
                "guest_jni_onload_executed=0" %
                (report["id"], report["inspector"]["elapsed_ns"],
                 report["inspector"]["max_rss_kib"]))
            report["registry"] = inspect_exports(
                values["inspector"], guest_path, report, private_values,
                limits["timeout_seconds"],
                limits["output_limit_bytes_per_stream"])
            require(sha256_file(guest_path) == expected_guest["sha256"],
                    "guest=%s changed during exports run" % report["id"])
            report["total_elapsed_ns"] = time.monotonic_ns() - guest_started
            manifest["guests"].append(report)
            writer.log(
                "guest=%s exports=PASS added=%d equivalent=%d "
                "monotonic_elapsed_ns=%d max_rss_kib=%d "
                "guest_initializers_executed=0 "
                "guest_jni_onload_executed=0" %
                (report["id"], report["registry"]["added"],
                 report["registry"]["equivalent"],
                 report["registry"]["elapsed_ns"],
                 report["registry"]["max_rss_kib"]))

        manifest["status"] = "PASS"
        manifest["completed_utc"] = utc_now()
        manifest["total_elapsed_ns"] = time.monotonic_ns() - started
        writer.write_manifest(manifest)
        writer.log(
            "m10-aarch64-guest-gate: PASS guests=5 "
            "guest_initializers_executed=0 guest_jni_onload_executed=0 "
            "device_access=0 guest_files_copied=0")
        writer.log("evidence_log=%s evidence_manifest=%s" %
                   (writer.log_path.name, writer.manifest_path.name))
        return 0
    except (GateError, OSError, ValueError, struct.error) as error:
        safe_error = sanitize_text(error, private_values)
        manifest["status"] = "FAIL"
        manifest["completed_utc"] = utc_now()
        manifest["total_elapsed_ns"] = time.monotonic_ns() - started
        manifest["error"] = safe_error
        try:
            writer.write_manifest(manifest)
        except GateError as manifest_error:
            safe_error += "; " + sanitize_text(manifest_error, private_values)
        writer.log(
            "m10-aarch64-guest-gate: FAIL error=%s "
            "guest_initializers_executed=0 guest_jni_onload_executed=0 "
            "device_access=0 guest_files_copied=0" % safe_error)
        return 1
    finally:
        writer.close()


def main():
    arguments = parse_arguments()
    values = configured_values(arguments)
    try:
        limits = configured_limits(arguments)
        return run_gate(arguments, values, limits)
    except (GateError, argparse.ArgumentTypeError) as error:
        print("m10-aarch64-guest-gate: FAIL error=%s "
              "guest_initializers_executed=0 guest_jni_onload_executed=0 "
              "device_access=0 guest_files_copied=0" % sanitize_text(error))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
