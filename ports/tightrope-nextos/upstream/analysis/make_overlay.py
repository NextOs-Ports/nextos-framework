#!/usr/bin/env python3
"""Build the plaintext overlay a NextOS port needs from a donor Android process.

PairIP does two things at load time that an ordinary ELF loader does not:
it decrypts a window of each object and it fills the PLT slots whose
relocations were removed.  This tool captures the first of those.

It reads each mapped segment of a protected object out of a running donor
process, diffs it against the library on disk, and keeps every differing byte
that is NOT explained by the ordinary loader:

  - bytes covered by a relocation are dropped: our own loader computes those,
    and the donor's values are the donor's addresses;
  - eight-byte words whose donor value looks like a donor address are dropped
    for the same reason, even when no relocation names them;
  - everything else is static content the interpreter produced, and is kept.

Output is one .overlay file per object: "NXOV", u32 count, then
{u64 vaddr, u32 len, bytes} records, plus the FNV-1a the port pins.
"""
import os, struct, sys

def phdrs(path):
    d = open(path, 'rb').read()
    e_phoff, = struct.unpack_from('<Q', d, 0x20)
    e_phentsize, e_phnum = struct.unpack_from('<HH', d, 0x36)
    loads, dyn = [], None
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        t, f, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, o)
        if t == 1:
            loads.append((off, va, fs, f))
        elif t == 2:
            dyn = (off, fs)
    return d, loads, dyn

def reloc_offsets(d, dyn):
    off, sz = dyn
    tags = {}
    for k in range(sz // 16):
        t, v = struct.unpack_from('<QQ', d, off + k * 16)
        if t == 0:
            break
        tags[t] = v
    out = set()
    for base, size in ((tags.get(7), tags.get(8, 0)),
                       (tags.get(23), tags.get(2, 0))):
        if not base:
            continue
        for k in range(size // 24):
            r_off, r_info, r_add = struct.unpack_from('<QQq', d, base + k * 24)
            out.add(r_off)
    return out

def looks_like_donor_address(v):
    return 0x400000000000 <= v < 0x400200000000 or 0x7f0000000000 <= v < 0x800000000000

def fnv1a(data):
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def main(pid, path, base, out_path):
    d, loads, dyn = phdrs(path)
    relocs = reloc_offsets(d, dyn)
    mem = os.open('/proc/%d/mem' % pid, os.O_RDONLY)
    keep = bytearray()          # 1 per byte of interest, indexed by vaddr
    records = []
    kept_bytes = dropped_reloc = dropped_ptr = 0

    for off, va, fs, flags in loads:
        if not fs:
            continue
        os.lseek(mem, base + va, 0)
        live = b''
        while len(live) < fs:
            chunk = os.read(mem, min(1 << 20, fs - len(live)))
            if not chunk:
                break
            live += chunk
        orig = d[off:off + fs]
        n = min(len(live), len(orig))

        writable = bool(flags & 2)
        take = bytearray(n)
        i = 0
        while i < n:
            if live[i] == orig[i]:
                i += 1
                continue
            # A read-only or executable segment holds no runtime state, so
            # every byte that differs there is decrypted content.  Only the
            # writable segment needs the two filters.
            if writable:
                word = (va + i) & ~7
                widx = word - va
                if any((word + k) in relocs for k in range(0, 8, 4)):
                    dropped_reloc += 1
                    i += 1
                    continue
                if widx >= 0 and widx + 8 <= n:
                    v, = struct.unpack_from('<Q', live, widx)
                    if looks_like_donor_address(v):
                        dropped_ptr += 1
                        i += 1
                        continue
            take[i] = 1
            i += 1

        i = 0
        while i < n:
            if not take[i]:
                i += 1
                continue
            start = i
            while i < n and take[i]:
                i += 1
            records.append((va + start, live[start:i]))
            kept_bytes += i - start

    os.close(mem)
    blob = bytearray(b'NXOV')
    blob += struct.pack('<I', len(records))
    for vaddr, data in records:
        blob += struct.pack('<QI', vaddr, len(data)) + data
    open(out_path, 'wb').write(blob)
    print('%s: %d ranges, %d bytes kept '
          '(%d reloc bytes and %d pointer bytes dropped)'
          % (os.path.basename(out_path), len(records), kept_bytes,
             dropped_reloc, dropped_ptr))
    print('  FNV-1a %#018x' % fnv1a(blob))
    for vaddr, data in records[:12]:
        print('    %#010x  %6d bytes' % (vaddr, len(data)))
    if len(records) > 12:
        print('    ... %d more' % (len(records) - 12))

main(int(sys.argv[1]), sys.argv[2], int(sys.argv[3], 0), sys.argv[4])
