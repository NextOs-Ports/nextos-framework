#!/usr/bin/env python3
"""Turn a plaintext overlay into a transformation mask (the shippable form).

An overlay holds the PLAINTEXT that PairIP's interpreter produces in memory.
That is the game's own code, so it cannot be redistributed.  A mask holds
`plaintext XOR ciphertext` instead: on its own it is a difference and reveals
nothing, and it only becomes anything when XORed against the very bytes the
user's own copy of the library already contains.

This is the same shape Prizefighters 2 ships (`tools/patches/*.xormask`): the
port reads the encrypted window out of the user's library, XORs the mask over
it, and checks the result's SHA-256 against the value pinned for the supported
build -- which both recovers the plaintext and proves the APK is the right one.

Record layout mirrors the overlay so the loader keeps one code path:

    "NXMK", u32 count, then { u64 vaddr, u32 len, bytes }

Usage:
    make_xormask.py <overlay> <pristine .so from the APK> <out .xormask>
"""
import hashlib
import struct
import sys


def load_records(path, magic):
    blob = open(path, "rb").read()
    if blob[:4] != magic:
        sys.exit("%s is not a %s file" % (path, magic.decode()))
    count, = struct.unpack_from("<I", blob, 4)
    at = 8
    records = []
    for _ in range(count):
        vaddr, length = struct.unpack_from("<QI", blob, at)
        at += 12
        records.append((vaddr, blob[at:at + length]))
        at += length
    return records


def segments(path):
    """(vaddr, file_offset, filesz) for every PT_LOAD, to map vaddr -> file."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        sys.exit("%s is not an ELF" % path)
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    out = []
    for i in range(phnum):
        base = phoff + i * phentsize
        p_type, = struct.unpack_from("<I", data, base)
        if p_type != 1:          # PT_LOAD
            continue
        p_offset, p_vaddr = struct.unpack_from("<QQ", data, base + 0x08)
        p_filesz, = struct.unpack_from("<Q", data, base + 0x20)
        out.append((p_vaddr, p_offset, p_filesz))
    return data, out


def file_bytes(data, segs, vaddr, length):
    for seg_vaddr, seg_off, seg_filesz in segs:
        if seg_vaddr <= vaddr and vaddr + length <= seg_vaddr + seg_filesz:
            start = seg_off + vaddr - seg_vaddr
            return data[start:start + length]
    sys.exit("vaddr 0x%x (+%d) is outside every PT_LOAD" % (vaddr, length))


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    overlay_path, library_path, out_path = sys.argv[1:4]

    records = load_records(overlay_path, b"NXOV")
    data, segs = segments(library_path)

    out = bytearray(b"NXMK")
    out += struct.pack("<I", len(records))
    covered = 0
    for vaddr, plain in records:
        cipher = file_bytes(data, segs, vaddr, len(plain))
        mask = bytes(a ^ b for a, b in zip(plain, cipher))
        out += struct.pack("<QI", vaddr, len(mask)) + mask
        covered += len(mask)

    open(out_path, "wb").write(bytes(out))
    print("%s: %d records, %d bytes covered" % (out_path, len(records), covered))
    print("  mask   sha256 %s" % hashlib.sha256(bytes(out)).hexdigest())
    # O que o loader confere depois de aplicar: o texto claro reconstruido.
    plain_all = b"".join(p for _, p in records)
    print("  plain  sha256 %s" % hashlib.sha256(plain_all).hexdigest())
    h = 0xcbf29ce484222325
    for byte in plain_all:
        h = ((h ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    print("  plain  fnv1a  0x%016x   <- o que o loader confere" % h)


main()
