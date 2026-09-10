#!/usr/bin/env python3
"""Rebuild the original import table of the PairIP-protected libunity.so.

PairIP shrinks .dynsym, rewrites the head of .rela.plt and prefills the GOT
slots of the imports it hides so that its own VM program patches them at
runtime.  What survives untouched: the original SysV .hash, the tail of the
original .dynstr and the tail of the original .rela.plt.  Together those are
enough to name most hidden PLT slots without running the VM.

Analysis only: it reads a BYO library and prints/writes a symbol table.
"""
import struct, subprocess, os, sys, json

def sections(path):
    out = subprocess.run(['readelf', '-S', '-W', path], capture_output=True,
                         text=True, env={**os.environ, 'LC_ALL': 'C'}).stdout
    S = {}
    for line in out.splitlines():
        p = line.split()
        for i, tok in enumerate(p):
            if tok.startswith('.') and i + 4 < len(p):
                try:
                    S[tok] = (int(p[i + 2], 16), int(p[i + 3], 16), int(p[i + 4], 16))
                except ValueError:
                    pass
                break
    return S

def elf_hash(s):
    h = 0
    for c in s.encode():
        h = (h * 16 + c) & 0xffffffff
        g = h & 0xf0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xffffffff
    return h

class Recovered:
    def __init__(self, path, is64):
        self.path, self.is64 = path, is64
        self.d = open(path, 'rb').read()
        S = sections(path)
        self.stro, self.strsz = S['.dynstr'][1], S['.dynstr'][2]
        self.gpa, self.gpsz = S['.got.plt'][0], S['.got.plt'][2]
        rel = S['.rela.plt'] if is64 else S['.rel.plt']
        self.rpa, self.rpo, self.rpsz = rel
        self.dsa, self.dso, self.dssz = S['.dynsym']
        self.ent = 24 if is64 else 16
        self.rent = 24 if is64 else 8
        self.W = 8 if is64 else 4
        self.nslots = self.gpsz // self.W
        self._find_boundary()
        self._find_hash()
        self._find_base()
        self._tables()

    def nm(self, o):
        if o >= self.strsz:
            return None
        e = self.d.index(b'\0', self.stro + o)
        try:
            return self.d[self.stro + o:e].decode('ascii')
        except UnicodeDecodeError:
            return None

    def _find_boundary(self):
        """PairIP rewrites dynstr[0:boundary]; the original tail survives after."""
        raw = self.d[self.stro:self.stro + self.strsz]
        seen, pos, bnd = {}, 0, None
        while pos < len(raw):
            e = raw.index(b'\0', pos)
            n = raw[pos:e].decode('utf8', 'replace')
            if n in seen and len(n) > 8:
                bnd = pos
                break
            seen[n] = pos
            pos = e + 1
        self.boundary = bnd

    def _find_hash(self):
        """Original SysV .hash: nbucket == nchain == nsyms, ends at .dynstr."""
        hi = self.stro
        for off in range(hi - 4000, hi, 4):
            nb, nc = struct.unpack_from('<II', self.d, off)
            if nb == nc and 200 <= nb <= 800 and off + 8 + (nb + nc) * 4 == hi:
                self.hb, self.nsyms = off, nc
                self.buckets = list(struct.unpack_from('<%dI' % nb, self.d, off + 8))
                self.chain = list(struct.unpack_from('<%dI' % nc, self.d, off + 8 + nb * 4))
                self.nb = nb
                return
        raise RuntimeError('original .hash not found')

    def chainof(self, name):
        i, out, seen = self.buckets[elf_hash(name) % self.nb], [], set()
        while i and i < self.nsyms and i not in seen:
            seen.add(i)
            out.append(i)
            i = self.chain[i]
        return out

    def _find_base(self):
        best = None
        for base in range(0x100, self.hb - self.nsyms * self.ent, 4):
            hit = tot = 0
            for i in range(1, self.nsyms):
                o = struct.unpack_from('<I', self.d, base + i * self.ent)[0]
                if o < self.boundary or o >= self.strsz:
                    continue
                n = self.nm(o)
                if not n:
                    continue
                tot += 1
                if i in self.chainof(n):
                    hit += 1
            sc = hit * 4 - (tot - hit) * 3
            if tot > 20 and (best is None or sc > best[0]):
                best = (sc, base, hit, tot)
        self.symbase, self.base_hits, self.base_tot = best[1], best[2], best[3]

    def _tables(self):
        d, W, gpa = self.d, self.W, self.gpa
        nsym = [self.nm(struct.unpack_from('<I', d, self.dso + i * self.ent)[0])
                for i in range(self.dssz // self.ent)]
        self.survivors = {}
        for i in range(self.rpsz // self.rent):
            if self.is64:
                o, info, _ = struct.unpack_from('<QQq', d, self.rpo + i * self.rent)
                si = info >> 32
            else:
                o, info = struct.unpack_from('<II', d, self.rpo + i * self.rent)
                si = info >> 8
            if gpa <= o < gpa + self.gpsz:
                self.survivors[(o - gpa) // W] = nsym[si]
        self.leftover = {}
        off, end = self.rpo + self.rpsz, self.rpo + (self.nslots - 3) * self.rent
        while off + self.rent <= end:
            if self.is64:
                o, info, _ = struct.unpack_from('<QQq', d, off)
                si = info >> 32
            else:
                o, info = struct.unpack_from('<II', d, off)
                si = info >> 8
            if gpa <= o < gpa + self.gpsz:
                self.leftover[(o - gpa) // W] = si
            off += self.rent
        self.symname = {}
        for i in range(1, self.nsyms):
            o = struct.unpack_from('<I', d, self.symbase + i * self.ent)[0]
            if self.boundary <= o < self.strsz:
                n = self.nm(o)
                if n and i in self.chainof(n):
                    self.symname[i] = n
        self.hidden = [s for s in range(3, self.nslots) if s not in self.survivors]
        self.named_hidden = {s: self.symname[self.leftover[s]] for s in self.hidden
                             if s in self.leftover and self.leftover[s] in self.symname}

    def report(self):
        return dict(path=os.path.basename(self.path), nsyms=self.nsyms,
                    symbase=hex(self.symbase), hash=hex(self.hb),
                    dynstr_boundary=self.boundary,
                    base_validated=f'{self.base_hits}/{self.base_tot}',
                    plt_slots=self.nslots - 3, survivors=len(self.survivors),
                    hidden=len(self.hidden), leftover=len(self.leftover),
                    named_hidden=len(self.named_hidden))

if __name__ == '__main__':
    for path, is64 in [(sys.argv[1], sys.argv[2] == '64')]:
        r = Recovered(path, is64)
        print(json.dumps(r.report(), indent=1))
