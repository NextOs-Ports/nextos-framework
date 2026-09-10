#!/usr/bin/env python3
"""Name every PLT/GOT slot of a PairIP-protected library from a post-DT_INIT dump.

PairIP leaves 200 of libunity's 336 `.got.plt` slots and 199 of libil2cpp's 569
without any relocation, so their names cannot be read from the tables the file
ships.  A dump of the live `.got.plt` taken after the protector's `DT_INIT` ran
holds the *resolved addresses*, and `/proc/<pid>/maps` says which library each
address belongs to.  Resolving address -> (library, symbol) therefore names the
slots exactly, with no guessing from call-site counts or argument shapes.

Inputs (all produced by the capture step, none of them in Git):
  --got     dumped `.got.plt` (8 bytes per slot, first three reserved)
  --maps    `/proc/<pid>/maps` of the same process
  --syslib  directory with the platform libraries pulled from that system
  --gamelib directory with the application's own libraries

Output: JSON mapping slot index -> [soname, symbol].
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

# ARM/AArch64 mapping symbols and other labels that carry no callable identity.
_JUNK_PREFIX = ("$x", "$d", "$a", "$t")

STT_FUNC = 2
STT_GNU_IFUNC = 10
STB_LOCAL = 0


def _elf_sections(blob: bytes):
    if blob[:4] != b"\x7fELF":
        return []
    e_shoff = struct.unpack_from("<Q", blob, 0x28)[0]
    shentsize, shnum, _shstrndx = struct.unpack_from("<HHH", blob, 0x3A)
    out = []
    for i in range(shnum):
        o = e_shoff + i * shentsize
        sh_type = struct.unpack_from("<I", blob, o + 4)[0]
        sh_offset, sh_size = struct.unpack_from("<QQ", blob, o + 24)[0:2]
        sh_link = struct.unpack_from("<I", blob, o + 40)[0]
        sh_entsize = struct.unpack_from("<Q", blob, o + 56)[0]
        out.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))
    return out


def symbol_table(path: str) -> dict[int, str]:
    """Map st_value -> best name for every defined symbol in an ELF."""
    blob = open(path, "rb").read()
    table: dict[int, tuple[int, str]] = {}
    sections = _elf_sections(blob)
    for sh_type, sh_offset, sh_size, sh_link, sh_entsize in sections:
        if sh_type not in (2, 11) or sh_entsize == 0:  # SYMTAB, DYNSYM
            continue
        if sh_link >= len(sections):
            continue
        stroff = sections[sh_link][1]
        for k in range(sh_size // sh_entsize):
            so = sh_offset + k * sh_entsize
            st_name, st_info = struct.unpack_from("<IB", blob, so)
            st_value = struct.unpack_from("<Q", blob, so + 8)[0]
            if st_name == 0 or st_value == 0:
                continue
            end = blob.index(b"\0", stroff + st_name)
            name = blob[stroff + st_name : end].decode("latin1")
            if not name or name.startswith(_JUNK_PREFIX):
                continue
            # Prefer global FUNC symbols: they are what a PLT slot can point at.
            bind = st_info >> 4
            stype = st_info & 0xF
            score = (0 if bind == STB_LOCAL else 2) + (
                1 if stype in (STT_FUNC, STT_GNU_IFUNC) else 0
            )
            prev = table.get(st_value)
            if prev is None or score > prev[0]:
                table[st_value] = (score, name)
    return {addr: name for addr, (_score, name) in table.items()}


def load_bases(maps_path: str) -> dict[str, int]:
    """Lowest (start - file_offset) per mapped path: the ELF load base."""
    bases: dict[str, int] = {}
    pattern = re.compile(
        r"^([0-9a-f]+)-([0-9a-f]+)\s+\S+\s+([0-9a-f]+)\s+\S+\s+\S+\s+(/\S+)$"
    )
    for line in open(maps_path):
        m = pattern.match(line.strip())
        if not m:
            continue
        start = int(m.group(1), 16)
        offset = int(m.group(3), 16)
        path = m.group(4)
        base = start - offset
        if path not in bases or base < bases[path]:
            bases[path] = base
    return bases


def find_local(path: str, syslib: str | None, gamelib: str | None) -> str | None:
    name = os.path.basename(path)
    for directory in (gamelib, syslib):
        if not directory:
            continue
        candidate = os.path.join(directory, name)
        if os.path.exists(candidate):
            return candidate
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--got", required=True)
    ap.add_argument("--maps", required=True)
    ap.add_argument("--syslib")
    ap.add_argument("--gamelib")
    ap.add_argument("--out")
    ap.add_argument("--tolerance", type=lambda s: int(s, 0), default=0,
                    help="accept a symbol this many bytes below the address")
    args = ap.parse_args()

    bases = load_bases(args.maps)
    resolved: dict[str, str] = {}
    cache: dict[str, dict[int, str]] = {}
    unknown: list[tuple[int, int, str]] = []

    got = open(args.got, "rb").read()
    nslots = len(got) // 8 - 3

    # Longest base first so the innermost containing mapping wins.
    ordered = sorted(bases.items(), key=lambda kv: -kv[1])

    for slot in range(nslots):
        addr = struct.unpack_from("<Q", got, (slot + 3) * 8)[0]
        if addr == 0:
            continue
        for path, base in ordered:
            if addr < base:
                continue
            local = find_local(path, args.syslib, args.gamelib)
            if local is None:
                continue
            if local not in cache:
                cache[local] = symbol_table(local)
            table = cache[local]
            off = addr - base
            if off in table:
                resolved[str(slot)] = [os.path.basename(path), table[off]]
                break
            if args.tolerance:
                below = [v for v in table if v <= off and off - v <= args.tolerance]
                if below:
                    near = max(below)
                    resolved[str(slot)] = [
                        os.path.basename(path),
                        f"{table[near]}+{off - near}",
                    ]
                    break
        else:
            owner = next(
                (p for p, b in ordered if addr >= b and addr - b < (1 << 32)), "?"
            )
            unknown.append((slot, addr, owner))

    print(f"{nslots} slots: {len(resolved)} named, {len(unknown)} unnamed")
    by_lib: dict[str, int] = {}
    for soname, _sym in resolved.values():
        by_lib[soname] = by_lib.get(soname, 0) + 1
    for soname, count in sorted(by_lib.items(), key=lambda kv: -kv[1]):
        print(f"   {count:4d}  {soname}")
    for slot, addr, owner in unknown[:20]:
        print(f"   unnamed slot {slot}: {addr:#x} in {owner}")

    if args.out:
        json.dump(resolved, open(args.out, "w"), indent=0, sort_keys=True)
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
