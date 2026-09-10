#!/usr/bin/env python3
"""Look for AES in a native library.

The cipher over the encrypted .text was characterised as ECB with a 16-byte
block.  That is AES's shape, so before reversing the VM it is worth asking the
cheap question: does the decryptor use the ARMv8 crypto extension, or a software
AES with the usual tables?  Either one hands us the key schedule directly.

Two independent probes:
  1. AArch64 AESE/AESD/AESMC/AESIMC opcodes (and PMULL, which would mean GCM).
  2. The AES S-box / inverse S-box / T-tables as data.
"""
import sys, struct

# AArch64 crypto-AES encodings: 0100 1110 0010 1000 01ss 11nn nnnd dddd
AES_OPS = {
    0x4E284800: "aese",
    0x4E285800: "aesd",
    0x4E286800: "aesmc",
    0x4E287800: "aesimc",
}
SHA_OPS = {
    0x5E000000: "sha1c-family",
}
PMULL = 0x0EE0E000  # pmull/pmull2 (GCM)

SBOX = bytes([
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76])
INV_SBOX = bytes([
0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb])
RCON = bytes([0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36])

def scan(path):
    data = open(path, "rb").read()
    print(f"=== {path}  ({len(data)} bytes) ===")

    # probe 1: crypto extension opcodes
    hits = {}
    for off in range(0, len(data) - 4, 4):
        w = struct.unpack_from("<I", data, off)[0]
        base = w & 0xFFFFFC00
        if base in AES_OPS:
            hits.setdefault(AES_OPS[base], []).append(off)
        elif (w & 0xFFE0FC00) == PMULL:
            hits.setdefault("pmull", []).append(off)
    if hits:
        for k, v in sorted(hits.items()):
            print(f"  ARMv8 crypto {k:8s}: {len(v):5d} sites, first at {v[0]:#x}")
    else:
        print("  ARMv8 crypto AES opcodes: none")

    # probe 2: tables as data
    for name, pat in (("AES S-box", SBOX), ("inverse S-box", INV_SBOX),
                      ("rcon", RCON)):
        at, i = [], data.find(pat)
        while i >= 0 and len(at) < 8:
            at.append(i)
            i = data.find(pat, i + 1)
        print(f"  {name:14s}: {'found at ' + ', '.join(hex(x) for x in at) if at else 'not found'}")

for p in sys.argv[1:]:
    scan(p)
