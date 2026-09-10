#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-GRAPHICS-03 ELF audit.

Reads an ELF directly (no readelf, no external helper) and reports:

* every EGL symbol the object leaves UNDEFINED, split by relocation kind
  (JUMP_SLOT / GLOB_DAT / other), which is the inventory the port must declare;
* whether the object carries ``DT_NEEDED libEGL`` - forbidden for the universal
  executable, because EGL must never be linked for every game;
* the highest ``GLIBC_x.y`` version requirement, for the public <= 2.30 gate.

Exit status is 0 when the audit matches the declared contract, 1 otherwise.
"""

import argparse
import json
import struct
import sys

R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_GLOB_DAT = 1025
R_ARM_JUMP_SLOT = 22
R_ARM_GLOB_DAT = 21
R_X86_64_JUMP_SLOT = 7
R_X86_64_GLOB_DAT = 6

JUMP_SLOT = {R_AARCH64_JUMP_SLOT, R_ARM_JUMP_SLOT, R_X86_64_JUMP_SLOT}
GLOB_DAT = {R_AARCH64_GLOB_DAT, R_ARM_GLOB_DAT, R_X86_64_GLOB_DAT}

DT_NEEDED = 1
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_REL = 17
DT_RELSZ = 18
DT_RELENT = 19
DT_JMPREL = 23
DT_PLTRELSZ = 2
DT_PLTREL = 20
DT_VERNEED = 0x6FFFFFFE
DT_VERNEEDNUM = 0x6FFFFFFF


class AuditError(Exception):
    pass


class Elf(object):
    def __init__(self, data):
        if len(data) < 64 or data[:4] != b"\x7fELF":
            raise AuditError("not an ELF object")
        self.data = data
        self.is64 = data[4] == 2
        little = data[5] == 1
        self.endian = "<" if little else ">"
        self.machine = struct.unpack_from(self.endian + "H", data, 18)[0]
        if self.is64:
            self.phoff, = struct.unpack_from(self.endian + "Q", data, 32)
            self.phentsize, self.phnum = struct.unpack_from(
                self.endian + "HH", data, 54)
        else:
            self.phoff, = struct.unpack_from(self.endian + "I", data, 28)
            self.phentsize, self.phnum = struct.unpack_from(
                self.endian + "HH", data, 42)
        self.segments = []
        for index in range(self.phnum):
            base = self.phoff + index * self.phentsize
            if self.is64:
                p_type, _flags, p_offset, p_vaddr = struct.unpack_from(
                    self.endian + "IIQQ", data, base)
                p_filesz, = struct.unpack_from(self.endian + "Q", data,
                                               base + 32)
            else:
                p_type, p_offset, p_vaddr = struct.unpack_from(
                    self.endian + "III", data, base)
                p_filesz, = struct.unpack_from(self.endian + "I", data,
                                               base + 16)
            self.segments.append((p_type, p_offset, p_vaddr, p_filesz))
        self.dynamic = self._read_dynamic()

    def vaddr_to_offset(self, vaddr):
        for p_type, p_offset, p_vaddr, p_filesz in self.segments:
            if p_type == 1 and p_vaddr <= vaddr < p_vaddr + p_filesz:
                return p_offset + (vaddr - p_vaddr)
        raise AuditError("virtual address 0x%x is outside every PT_LOAD" %
                         vaddr)

    def _read_dynamic(self):
        entries = []
        for p_type, p_offset, _p_vaddr, p_filesz in self.segments:
            if p_type != 2:
                continue
            step = 16 if self.is64 else 8
            fmt = self.endian + ("qQ" if self.is64 else "iI")
            for cursor in range(p_offset, p_offset + p_filesz, step):
                tag, value = struct.unpack_from(fmt, self.data, cursor)
                entries.append((tag, value))
                if tag == 0:
                    break
        if not entries:
            raise AuditError("the object has no PT_DYNAMIC segment")
        return entries

    def dyn_value(self, tag, default=None):
        for entry_tag, value in self.dynamic:
            if entry_tag == tag:
                return value
        return default

    def dyn_values(self, tag):
        return [value for entry_tag, value in self.dynamic if entry_tag == tag]

    def string(self, offset):
        strtab = self.vaddr_to_offset(self.dyn_value(DT_STRTAB))
        end = self.data.index(b"\x00", strtab + offset)
        return self.data[strtab + offset:end].decode("utf-8", "replace")

    def symbol(self, index):
        symtab = self.vaddr_to_offset(self.dyn_value(DT_SYMTAB))
        entsize = self.dyn_value(DT_SYMENT, 24 if self.is64 else 16)
        base = symtab + index * entsize
        name_offset, = struct.unpack_from(self.endian + "I", self.data, base)
        if self.is64:
            shndx, = struct.unpack_from(self.endian + "H", self.data, base + 6)
        else:
            shndx, = struct.unpack_from(self.endian + "H", self.data, base + 14)
        return self.string(name_offset), shndx

    def relocations(self):
        found = []
        tables = []
        rela = self.dyn_value(DT_RELA)
        if rela is not None:
            tables.append((rela, self.dyn_value(DT_RELASZ, 0),
                           self.dyn_value(DT_RELAENT,
                                          24 if self.is64 else 12), True))
        rel = self.dyn_value(DT_REL)
        if rel is not None:
            tables.append((rel, self.dyn_value(DT_RELSZ, 0),
                           self.dyn_value(DT_RELENT,
                                          16 if self.is64 else 8), False))
        jmprel = self.dyn_value(DT_JMPREL)
        if jmprel is not None:
            pltrel = self.dyn_value(DT_PLTREL, 7)
            is_rela = pltrel == 7
            entsize = (24 if self.is64 else 12) if is_rela else (
                16 if self.is64 else 8)
            tables.append((jmprel, self.dyn_value(DT_PLTRELSZ, 0), entsize,
                           is_rela))
        for address, size, entsize, _is_rela in tables:
            if not size or not entsize:
                continue
            offset = self.vaddr_to_offset(address)
            for cursor in range(offset, offset + size, entsize):
                if self.is64:
                    _r_offset, r_info = struct.unpack_from(
                        self.endian + "QQ", self.data, cursor)
                    r_type = r_info & 0xFFFFFFFF
                    r_sym = r_info >> 32
                else:
                    _r_offset, r_info = struct.unpack_from(
                        self.endian + "II", self.data, cursor)
                    r_type = r_info & 0xFF
                    r_sym = r_info >> 8
                if r_sym == 0:
                    continue
                name, shndx = self.symbol(r_sym)
                if not name or shndx != 0:
                    continue
                found.append((name, r_type))
        return found

    def needed(self):
        return [self.string(value) for value in self.dyn_values(DT_NEEDED)]

    def glibc_versions(self):
        verneed = self.dyn_value(DT_VERNEED)
        count = self.dyn_value(DT_VERNEEDNUM, 0)
        versions = []
        if verneed is None or not count:
            return versions
        cursor = self.vaddr_to_offset(verneed)
        for _index in range(count):
            _version, cnt, _file, aux, nxt = struct.unpack_from(
                self.endian + "HHIII", self.data, cursor)
            aux_cursor = cursor + aux
            for _entry in range(cnt):
                _hash, _flags, _other, name, aux_next = struct.unpack_from(
                    self.endian + "IHHII", self.data, aux_cursor)
                versions.append(self.string(name))
                if not aux_next:
                    break
                aux_cursor += aux_next
            if not nxt:
                break
            cursor += nxt
        return versions


def audit(path):
    with open(path, "rb") as stream:
        elf = Elf(stream.read())
    jump_slot = []
    glob_dat = []
    other = []
    for name, r_type in elf.relocations():
        if not name.startswith("egl"):
            continue
        if r_type in JUMP_SLOT:
            jump_slot.append(name)
        elif r_type in GLOB_DAT:
            glob_dat.append(name)
        else:
            other.append(name)
    needed = elf.needed()
    glibc = []
    for version in elf.glibc_versions():
        if version.startswith("GLIBC_"):
            parts = version[len("GLIBC_"):].split(".")
            try:
                glibc.append(tuple(int(part) for part in parts))
            except ValueError:
                continue
    return {
        "path": path,
        "egl_jump_slot": sorted(set(jump_slot)),
        "egl_glob_dat": sorted(set(glob_dat)),
        "egl_other": sorted(set(other)),
        "egl_undefined": sorted(set(jump_slot) | set(glob_dat) | set(other)),
        "needed": needed,
        "needed_libegl": sorted(
            name for name in needed if name.startswith("libEGL")),
        "max_glibc": ".".join(str(part) for part in max(glibc)) if glibc
                     else None,
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("elf")
    parser.add_argument(
        "--declared", action="append", default=[],
        help="one declared EGL import; repeat for the whole inventory")
    parser.add_argument(
        "--forbid-needed-libegl", action="store_true",
        help="fail when the object carries DT_NEEDED libEGL")
    parser.add_argument(
        "--max-glibc", default=None,
        help="highest allowed GLIBC_x.y requirement, e.g. 2.30")
    options = parser.parse_args(argv)
    try:
        report = audit(options.elf)
    except (AuditError, OSError, ValueError, struct.error) as error:
        print("nxgl-egl-audit: %s" % error, file=sys.stderr)
        return 1
    problems = []
    if options.declared:
        declared = set(options.declared)
        undeclared = sorted(set(report["egl_undefined"]) - declared)
        missing = sorted(declared - set(report["egl_undefined"]))
        if undeclared:
            problems.append("undeclared EGL imports: %s" % ", ".join(undeclared))
        if missing:
            problems.append("declared but unused EGL imports: %s"
                            % ", ".join(missing))
    if options.forbid_needed_libegl and report["needed_libegl"]:
        problems.append("DT_NEEDED %s is forbidden here"
                        % ", ".join(report["needed_libegl"]))
    if options.max_glibc and report["max_glibc"]:
        limit = tuple(int(part) for part in options.max_glibc.split("."))
        actual = tuple(int(part) for part in report["max_glibc"].split("."))
        if actual > limit:
            problems.append("GLIBC_%s exceeds the %s ceiling"
                            % (report["max_glibc"], options.max_glibc))
    report["problems"] = problems
    print(json.dumps(report, indent=2, sort_keys=True))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
