#!/usr/bin/env python3
"""Find every range PairIP rewrites, by diffing a donor process against the file.

Reads each mapped PT_LOAD of the protected objects out of a running Android
process and compares it byte for byte with the library on disk.  Anything that
differs was produced by the interpreter -- either a decrypted window or an
address the Android loader relocated.  Ranges that no relocation covers are the
ones this port has to restore.
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
            loads.append((off, va, fs, ms, f))
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
    for base, size in ((tags.get(7), tags.get(8, 0)), (tags.get(23), tags.get(2, 0))):
        if not base:
            continue
        for k in range(size // 24):
            r_off, r_info, r_add = struct.unpack_from('<QQq', d, base + k * 24)
            out.add(r_off)
    return out

def main(pid, path, base):
    d, loads, dyn = phdrs(path)
    relocs = reloc_offsets(d, dyn)
    mem = os.open('/proc/%d/mem' % pid, os.O_RDONLY)
    for off, va, fs, ms, flags in loads:
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
        i = 0
        while i < n:
            if live[i] == orig[i]:
                i += 1
                continue
            start = i
            gap = 0
            while i < n and (live[i] != orig[i] or gap < 64):
                gap = gap + 1 if live[i] == orig[i] else 0
                i += 1
            end = i - gap
            covered = sum(1 for r in relocs if va + start <= r < va + end)
            print('%s  seg%s%s vaddr %#010x..%#010x  %6d bytes  '
                  'relocations inside: %d'
                  % (os.path.basename(path), '' if flags & 1 else '',
                     ' (W)' if flags & 2 else '', va + start, va + end,
                     end - start, covered))
    os.close(mem)

main(int(sys.argv[1]), sys.argv[2], int(sys.argv[3], 0))
