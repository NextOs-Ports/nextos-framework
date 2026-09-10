#!/usr/bin/env python3
"""Map the part of a PairIP-protected library's .text that ships encrypted.

PairIP does not only hide imports: in this build it also leaves a block at the
start of libunity.so's .text as ciphertext, and the VM program its DT_INIT asks
for is what decrypts it.  Finding that block matters because it decides whether
a port can get by with the offline import reconstruction or has to run the VM.

The test is an encoding one, not entropy.  In AArch64 the top-level group is
bits 28:25; values 0b0000..0b0011 are reserved, SVE and SME, and a compiler
targeting armv8-a never emits them.  Real code scores 0.00 on that measure,
ciphertext scores around 0.25 because a quarter of random encodings land there.
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

class Text:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        S = sections(path)
        self.va, self.off, self.size = S['.text']

    def word(self, va):
        return struct.unpack_from('<I', self.d, self.off + (va - self.va))[0]

    def noncode(self, va, words=24):
        bad = 0
        for i in range(words):
            if ((self.word(va + i * 4) >> 25) & 0xF) <= 0x3:
                bad += 1
        return bad / words

    def blocks(self, step=256, window=64, thresh=0.08):
        """Contiguous runs that do not decode as code."""
        runs, start, prev = [], None, None
        va = self.va
        end = self.va + self.size - window * 4
        while va < end:
            if self.noncode(va, window) > thresh:
                if start is None:
                    start = va
                prev = va + step
            elif start is not None:
                runs.append((start, prev))
                start = None
            va += step
        if start is not None:
            runs.append((start, prev))
        return runs

    def call_targets(self):
        """Every direct BL target inside .text, with its call sites."""
        out = {}
        for i in range(0, self.size - 3, 4):
            x = struct.unpack_from('<I', self.d, self.off + i)[0]
            if (x & 0xFC000000) != 0x94000000:
                continue
            imm = x & 0x03FFFFFF
            if imm & 0x02000000:
                imm -= 0x04000000
            t = self.va + i + imm * 4
            if self.va <= t < self.va + self.size - 128:
                out.setdefault(t, []).append(self.va + i)
        return out

if __name__ == '__main__':
    t = Text(sys.argv[1])
    runs = t.blocks()
    total = sum(b - a for a, b in runs)
    print('.text %#x..%#x (%d KB)' % (t.va, t.va + t.size, t.size // 1024))
    print('blocks that do not decode as code: %d, %d KB total' % (len(runs), total // 1024))
    for a, b in runs:
        print('   %#010x..%#010x  %d KB' % (a, b, (b - a) // 1024))
    tg = t.call_targets()
    enc = sorted(x for x in tg if t.noncode(x) > 0.10)
    calls = sum(len(tg[x]) for x in enc)
    allc = sum(len(v) for v in tg.values())
    print('\ndirect call targets: %d ; encrypted: %d (%.2f%%)'
          % (len(tg), len(enc), 100.0 * len(enc) / max(len(tg), 1)))
    print('calls into encrypted code: %d / %d (%.2f%%)'
          % (calls, allc, 100.0 * calls / max(allc, 1)))
    for x in enc:
        sites = tg[x]
        print('   %#010x  %4d calls   first caller %#x' % (x, len(sites), sites[0]))
    json.dump({'blocks': [[a, b] for a, b in runs],
               'encrypted_targets': {hex(x): len(tg[x]) for x in enc}},
              open(os.path.basename(sys.argv[1]) + '.encrypted.json', 'w'), indent=1)
