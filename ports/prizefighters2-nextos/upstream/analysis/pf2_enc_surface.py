#!/usr/bin/env python3
"""Map every way clean code can reach PairIP's encrypted .text prefix.

PairIP encrypts a fixed 0xC800-byte prefix of each protected library's .text.
Decrypting it needs the VM's key, which is a project of its own.  But the
region only matters to us through the references that reach it, so the question
that decides the port is: how large is that entry surface, and is any of it
reachable other than by a direct call?

Four independent probes, because missing one would make a bypass silently wrong:
  1. direct BL/B from clean code into the region (with call counts)
  2. ADRP+ADD pairs in clean code that materialise an address in the region
     (a function pointer taken, i.e. a vtable or callback install)
  3. R_AARCH64_RELATIVE relocations whose addend lands in the region
     (a function pointer already baked into .data)
  4. .eh_frame_hdr's sorted FDE table, which gives the exact function
     boundaries inside the region regardless of it being ciphertext

Probe 1 alone would say "22 functions, reimplement them".  Probes 2 and 3 are
what tell us whether that is true.
"""
import struct
import sys

REGION = 0xC800


class ELF:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        e_shoff, = struct.unpack_from("<Q", d, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 0x3A)
        secs = []
        for i in range(e_shnum):
            o = e_shoff + i * e_shentsize
            name, typ, flags, addr, off, size, link, info, align, entsize = \
                struct.unpack_from("<IIQQQQIIQQ", d, o)
            secs.append(dict(name_off=name, type=typ, flags=flags, addr=addr,
                             off=off, size=size, entsize=entsize))
        stroff = secs[e_shstrndx]["off"]
        for s in secs:
            end = d.index(b"\0", stroff + s["name_off"])
            s["name"] = d[stroff + s["name_off"]:end].decode()
        self.secs = secs

    def sec(self, name):
        for s in self.secs:
            if s["name"] == name:
                return s
        return None

    def read_at_addr(self, addr, n):
        """Virtual address -> bytes, via the section that contains it."""
        for s in self.secs:
            if s["type"] != 8 and s["addr"] and s["addr"] <= addr < s["addr"] + s["size"]:
                o = s["off"] + (addr - s["addr"])
                return self.data[o:o + n]
        return b""


def signed(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def eh_frame_starts(elf):
    """Function start addresses, from .eh_frame_hdr's binary-search table.

    Easier and more reliable than walking .eh_frame: the header's table is
    already sorted and every entry is datarel|sdata4.
    """
    h = elf.sec(".eh_frame_hdr")
    if not h:
        return []
    d = elf.data
    o = h["off"]
    ver, eh_ptr_enc, cnt_enc, tbl_enc = d[o:o + 4]
    if ver != 1 or cnt_enc != 0x03 or tbl_enc != 0x3B:
        print(f"  (unexpected .eh_frame_hdr encodings "
              f"{eh_ptr_enc:#x}/{cnt_enc:#x}/{tbl_enc:#x}; skipping FDE table)")
        return []
    p = o + 4
    p += 4                                   # eh_frame_ptr, pcrel|sdata4
    count, = struct.unpack_from("<I", d, p)
    p += 4
    base = h["addr"]
    out = []
    for i in range(count):
        loc, = struct.unpack_from("<i", d, p + i * 8)
        out.append(base + loc)
    return sorted(out)


def analyse(path):
    elf = ELF(path)
    text = elf.sec(".text")
    lo = text["addr"]
    hi = lo + min(REGION, text["size"])
    print(f"=== {path.split('/')[-1]} ===")
    print(f"  .text {lo:#x}..{lo + text['size']:#x} "
          f"({text['size']} bytes); encrypted prefix {lo:#x}..{hi:#x}")

    d = elf.data
    toff = text["off"]
    tsize = text["size"]

    # ---- probe 1: direct branches from clean code into the region
    calls = {}
    branch_b = {}
    for off in range(0, tsize - 4, 4):
        addr = lo + off
        w, = struct.unpack_from("<I", d, toff + off)
        op = w >> 26
        if op not in (0x25, 0x05):            # BL, B
            continue
        imm = signed(w & 0x03FFFFFF, 26) * 4
        tgt = addr + imm
        if not (lo <= tgt < hi):
            continue
        if lo <= addr < hi:
            continue                          # inside the region: not our problem
        (calls if op == 0x25 else branch_b).setdefault(tgt, 0)
        (calls if op == 0x25 else branch_b)[tgt] += 1

    print(f"  [1] direct BL from clean code: {sum(calls.values())} calls to "
          f"{len(calls)} targets")
    for t, n in sorted(calls.items(), key=lambda kv: -kv[1]):
        print(f"        {t:#x}  {n} calls")
    if branch_b:
        print(f"      plain B into the region: {sum(branch_b.values())} to "
              f"{len(branch_b)} targets (tail calls)")
        for t, n in sorted(branch_b.items(), key=lambda kv: -kv[1]):
            print(f"        {t:#x}  {n}")

    # ---- probe 2: ADRP+ADD address-taken in clean code
    #
    # Track the most recent ADRP per destination register and pair it with an
    # ADD on the same register.  Compilers emit them adjacently in practice.
    adrp_page = {}
    taken = {}
    for off in range(0, tsize - 4, 4):
        addr = lo + off
        w, = struct.unpack_from("<I", d, toff + off)
        if (w & 0x9F000000) == 0x90000000:                     # ADRP
            rd = w & 0x1F
            immlo = (w >> 29) & 0x3
            immhi = (w >> 5) & 0x7FFFF
            imm = signed((immhi << 2) | immlo, 21) << 12
            adrp_page[rd] = ((addr & ~0xFFF) + imm, addr)
            continue
        if (w & 0xFFC00000) == 0x91000000:                     # ADD Xd,Xn,#imm
            rn = (w >> 5) & 0x1F
            rd = w & 0x1F
            if rn in adrp_page:
                page, at = adrp_page[rn]
                tgt = page + ((w >> 10) & 0xFFF)
                if lo <= tgt < hi and not (lo <= at < hi):
                    taken.setdefault(tgt, 0)
                    taken[tgt] += 1
                if rd != rn:
                    adrp_page.pop(rd, None)
            continue
        rd = w & 0x1F
        adrp_page.pop(rd, None)

    print(f"  [2] ADRP+ADD address-taken into the region: "
          f"{sum(taken.values())} sites, {len(taken)} distinct addresses")
    for t, n in sorted(taken.items(), key=lambda kv: -kv[1])[:20]:
        print(f"        {t:#x}  {n} sites")

    # ---- probe 3: relative relocations with an addend in the region
    rel = elf.sec(".rela.dyn")
    relhits = {}
    if rel:
        n = rel["size"] // 24
        for i in range(n):
            o = rel["off"] + i * 24
            r_off, r_info, r_add = struct.unpack_from("<QQq", d, o)
            if (r_info & 0xFFFFFFFF) != 1027:      # R_AARCH64_RELATIVE
                continue
            if lo <= r_add < hi:
                relhits.setdefault(r_add, []).append(r_off)
    print(f"  [3] R_AARCH64_RELATIVE pointers into the region: "
          f"{sum(len(v) for v in relhits.values())} from "
          f"{len(relhits)} distinct addresses")
    for t, v in sorted(relhits.items(), key=lambda kv: -len(kv[1]))[:20]:
        print(f"        {t:#x}  {len(v)} slots, first at {v[0]:#x}")

    # ---- probe 4: function boundaries from .eh_frame_hdr
    starts = eh_frame_starts(elf)
    inside = [a for a in starts if lo <= a < hi]
    print(f"  [4] .eh_frame FDEs: {len(starts)} total, {len(inside)} inside "
          f"the encrypted region")
    if inside:
        bounds = []
        for i, a in enumerate(inside):
            nxt = inside[i + 1] if i + 1 < len(inside) else hi
            bounds.append((a, nxt - a))
        print(f"      first 12 (addr, size): " +
              ", ".join(f"{a:#x}/{s}" for a, s in bounds[:12]))
        entries = sorted(set(list(calls) + list(branch_b) + list(taken) +
                             list(relhits)))
        named = [(a, s) for a, s in bounds if a in entries]
        print(f"      of those, {len(named)} are reachable from clean code:")
        for a, s in named:
            hits = (f"{calls.get(a, 0)} BL, {branch_b.get(a, 0)} B, "
                    f"{taken.get(a, 0)} taken, {len(relhits.get(a, []))} reloc")
            print(f"        {a:#x}  size {s:5d}  ({hits})")
        orphan = [a for a in entries if a not in dict(bounds)]
        if orphan:
            print(f"      !! {len(orphan)} reachable addresses are NOT an FDE "
                  f"start (mid-function entry): " +
                  ", ".join(hex(a) for a in orphan[:10]))
    print()


for p in sys.argv[1:]:
    analyse(p)
