#!/usr/bin/env python3
"""Decode an .IAP program's entry state the way libpairipcore's vm_entry does.

Transcribed from the arm64 disassembly of libpairipcore+0x4d164 (vm_entry) and
its prologue block at +0x4d820.  The point of doing it offline is that every
input is a pure function of the file and its length, so if the port feeds the VM
correctly then these numbers are exactly what the interpreter sees -- and a
nonsense value here means the container model is wrong, not the environment.

The operand masking is the part worth stating plainly: the interpreter reads a
32-bit word and unmasks it as ~(word ^ len), then reduces it mod len.  len is a
constant for the whole run, so this is masking, not encryption -- and because
XOR with a constant preserves entropy, the payload's 7.92 bits/byte never was
evidence of a cipher.
"""
import struct
import sys

U32 = 0xFFFFFFFF


def u32(x):
    return x & U32


def s32(x):
    x &= U32
    return x - (1 << 32) if x & 0x80000000 else x


class Program:
    def __init__(self, path):
        self.buf = open(path, "rb").read()
        self.len = len(self.buf)
        self.path = path

    def w(self, off):
        """32-bit load, as the interpreter does it: unaligned is fine."""
        return struct.unpack_from("<I", self.buf, off)[0]

    def h(self, off):
        return struct.unpack_from("<H", self.buf, off)[0]

    def sh(self, off):
        return struct.unpack_from("<h", self.buf, off)[0]

    def unmask(self, word):
        """eon(word, len) == ~(word ^ len)"""
        return u32(~(word ^ self.len))

    def operand(self, off):
        """The interpreter's standard fetch: unmask, then reduce mod len."""
        return self.unmask(self.w(off)) % self.len


def entry_state(p):
    """vm_entry+0x6bc: derive the initial PC and the first operands.

    Every constant below is straight out of the disassembly; the arithmetic is
    obfuscated but it is all a function of w9, the version word at offset 4.
    """
    w9 = p.w(4)                       # ldr w9, [x8, #4]   -- the version field
    w12, w13, w14 = 0x23, 0x46, u32(-34)

    w10 = u32(w9 & w12)               # and  w10, w9, w12
    w11 = u32(w9 << 1)                # lsl  w11, w9, #1
    w12 = u32(w9 | w12)               # orr  w12, w9, w12
    w10 = u32(w10 + (w10 << 1))       # add  w10, w10, w10, lsl #1
    w14 = u32(w9 | w14)               # orr  w14, w9, w14
    w13 = u32(w11 ^ w13)              # eor  w13, w11, w13
    w10 = u32(w10 - w12)              # sub  w10, w10, w12
    w10 = u32(w10 + w13)              # add  w10, w10, w13
    off_a = w10                       # ldr  w10, [x8, w10, uxtw]

    w13 = 0x21
    w13 = u32(w9 | w13)               # orr  w13, w9, w13
    w24 = p.operand(off_a)            # eon + udiv/msub  -> mod len
    w12b = 0x57
    w10b = u32(-44)
    w11 = u32(w11 | w12b)             # orr  w11, w11, w12
    w12 = u32(w13 + w14)              # add  w12, w13, w14
    w10b = u32(w9 ^ w10b)             # eor  w10, w9, w10
    w9b = u32(w9 + 0x27)              # add  w9,  w9,  #0x27
    w25 = u32(w12 + 0x22)             # add  w25, w12, #0x22
    w20 = u32(w11 + w10b)             # add  w20, w11, w10   -> the initial PC

    return dict(version=w9, pc=w20,
                off_a=off_a, w24=w24,
                w29_off=w9b, w29=p.w(w9b),
                w28_off=w25, w28=p.h(w25),
                w19_off=w24, w19=p.h(w24))


def report(path):
    p = Program(path)
    st = entry_state(p)
    print(f"=== {path.split('/')[-1]}  len={p.len} ({p.len:#x}) ===")
    hdr = p.buf[:12]
    print(f"  header      {hdr.hex(' ')}   magic={hdr[:4]!r} version={st['version']}")
    print(f"  mask ~len   {u32(~p.len):#010x}")
    print(f"  initial PC  {st['pc']}  ({st['pc']:#x})   "
          f"{'IN RANGE' if st['pc'] < p.len else 'OUT OF RANGE'}")
    print(f"  operand@{st['off_a']:<3d} raw={p.w(st['off_a']):#010x} -> "
          f"w24={st['w24']} ({st['w24']:#x}) "
          f"{'in range' if st['w24'] < p.len else 'OUT OF RANGE'}")
    print(f"  w19 = u16@{st['w19_off']:<7d} = {st['w19']}")
    print(f"  w28 = u16@{st['w28_off']:<7d} = {st['w28']}")
    print(f"  w29 = u32@{st['w29_off']:<7d} = {st['w29']:#010x}")

    # The crash was a runaway loop whose trip count came from a *signed*
    # halfword operand.  If well-formed programs never present a negative one at
    # a plausible instruction stride, a negative here means we drifted off the
    # instruction grid rather than that the program is malformed.
    neg = sum(1 for off in range(0, p.len - 2) if p.sh(off) < 0)
    print(f"  signed halfwords that are negative: {neg}/{p.len - 2} "
          f"({100.0 * neg / (p.len - 2):.1f}%)")
    print()


for a in sys.argv[1:]:
    report(a)
