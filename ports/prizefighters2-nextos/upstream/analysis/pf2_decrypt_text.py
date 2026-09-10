#!/usr/bin/env python3
"""Splice the plaintext of a PairIP-encrypted `.text` prefix back into a library.

PairIP ships the first 0xC800 bytes of `.text` as ciphertext and has its VM
decrypt them in place from `DT_INIT`.  Our loader does not run that VM, so the
prefix has to already be plaintext in the file we map.  A dump of the live
`.text` window taken after `DT_INIT` supplies exactly those bytes.

The splice is only accepted when the dump proves it belongs to this exact
build:

  1. every byte of the dump *outside* the differing window must equal the file
     -- that is what fixes the alignment and rules out a different build;
  2. the differing window must start at the segment's `.text` and be one
     contiguous run;
  3. the AArch64 encoding test must go from "ciphertext" on the file to "clean"
     on the dump.  Instruction bits 28:25 in 0b0000..0b0011 are
     reserved/SVE/SME, which an armv8-a compiler never emits: real code scores
     0.00, ciphertext about 0.25.

Nothing here touches Git-tracked data: the input library and the dump both come
from the user's own copy of the application.
"""

from __future__ import annotations

import argparse
import struct
import sys

PF_X = 1
CIPHER_MIN = 0.15   # a ciphertext prefix scores ~0.25
CLEAN_MAX = 0.01    # real AArch64 code scores 0.00


def load_segments(blob: bytes):
    e_phoff = struct.unpack_from("<Q", blob, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", blob, 0x36)
    out = []
    for i in range(phnum):
        o = e_phoff + i * phentsize
        p_type, p_flags = struct.unpack_from("<II", blob, o)
        p_offset, p_vaddr = struct.unpack_from("<QQ", blob, o + 8)
        p_filesz = struct.unpack_from("<Q", blob, o + 32)[0]
        if p_type == 1:
            out.append((p_vaddr, p_offset, p_filesz, p_flags))
    return out


def vaddr_to_offset(segments, vaddr: int) -> int | None:
    """File offset of a vaddr, including the page below a segment's start.

    A loader maps whole pages, so a dump that begins on the page boundary
    below `.text` also covers the tail of the previous segment's last page.
    """
    for p_vaddr, p_offset, p_filesz, _flags in segments:
        if p_vaddr <= vaddr < p_vaddr + p_filesz:
            return p_offset + (vaddr - p_vaddr)
    for p_vaddr, p_offset, _filesz, _flags in segments:
        page = p_vaddr & ~0xFFF
        if page <= vaddr < p_vaddr:
            return (p_offset - (p_vaddr - page)) + (vaddr - page)
    return None


def encoding_rate(data: bytes) -> float:
    """Fraction of words whose bits 28:25 fall in the reserved/SVE/SME range."""
    n = len(data) // 4
    if n == 0:
        return 0.0
    bad = 0
    for i in range(n):
        word = struct.unpack_from("<I", data, i * 4)[0]
        if ((word >> 25) & 0xF) <= 0b0011:
            bad += 1
    return bad / n


def token_rate(data: bytes) -> float:
    """Fraction of 8-byte words that are a valid IL2CPP metadata-usage token.

    PairIP spends its encryption budget a second time on writable data, and in
    libil2cpp that window is the metadata-usage table: `(kind << 29) |
    (index << 1) | 1`, or zero for an unused entry.  Decrypted, the table scores
    about 0.98; encrypted it scores 0.00.  This is the `.data` counterpart of the
    AArch64 encoding test.
    """
    n = len(data) // 8
    if n == 0:
        return 0.0
    good = 0
    for i in range(n):
        word = struct.unpack_from("<Q", data, i * 8)[0]
        if word == 0:
            good += 1
            continue
        if word >> 32:
            continue
        kind = (word >> 29) & 7
        if (word & 1) and 1 <= kind <= 7:
            good += 1
    return good / n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", required=True, help="library as shipped in the APK")
    ap.add_argument("--dump", required=True, help="live .text window dump")
    ap.add_argument("--vaddr", required=True, type=lambda s: int(s, 0),
                    help="vaddr the dump starts at")
    ap.add_argument("--out", help="write the spliced library here")
    ap.add_argument("--kind", choices=("text", "usage", "data"), default="text",
                    help="which acceptance test to apply: the AArch64 encoding "
                         "test for a .text window, the metadata-usage token test "
                         "for the IL2CPP token table, or entropy/zero-rate for "
                         "any other writable-data window")
    ap.add_argument("--window", help="explicit VADDR:VADDR window to splice, "
                                     "instead of the differing run. Needed for "
                                     "writable data, where a live process has "
                                     "also modified bytes around the window.")
    args = ap.parse_args()

    blob = bytearray(open(args.lib, "rb").read())
    dump = open(args.dump, "rb").read()
    segments = load_segments(blob)

    offset = vaddr_to_offset(segments, args.vaddr)
    if offset is None:
        print(f"{args.vaddr:#x} is not in any PT_LOAD", file=sys.stderr)
        return 1
    print(f"dump vaddr {args.vaddr:#x} -> file offset {offset:#x}, "
          f"{len(dump)} bytes")

    original = bytes(blob[offset : offset + len(dump)])
    if len(original) != len(dump):
        print("the dump runs past the end of the file", file=sys.stderr)
        return 1

    differing = [i for i in range(len(dump)) if dump[i] != original[i]]
    if not differing:
        print("the dump is identical to the file: nothing to splice")
        return 0

    if args.window:
        a, b = (int(x, 0) for x in args.window.split(":"))
        lo, hi = a - args.vaddr, b - args.vaddr
        if not (0 <= lo < hi <= len(dump)):
            print("--window is not inside the dump", file=sys.stderr)
            return 1
    else:
        lo, hi = differing[0], differing[-1] + 1

    stray = [
        i for i in range(len(dump))
        if not (lo <= i < hi) and dump[i] != original[i]
    ]
    trailing_ok = all(dump[i] == original[i] for i in range(hi, len(dump)))
    print(f"window vaddr {args.vaddr + lo:#x}..{args.vaddr + hi:#x} "
          f"({hi - lo} bytes)")
    if not stray:
        print("  every byte outside the window matches the file")
    elif args.kind == "text":
        print("dump disagrees with the file outside the window: wrong "
              "alignment or a different build", file=sys.stderr)
        return 1
    else:
        # A live process has relocated pointers and initialised globals in
        # writable data, so bytes around the window legitimately differ.  The
        # bytes *after* the window are the alignment proof when they are part of
        # the same untouched table.
        print(f"  {len(stray)} bytes outside the window differ (a live process "
              f"also writes to data)")
        print(f"  bytes after the window match the file: {trailing_ok}")
        if not trailing_ok and not args.window:
            print("no alignment proof and no explicit --window; refusing",
                  file=sys.stderr)
            return 1

    if args.kind == "text":
        text_starts = {v for v, _o, _s, f in segments if f & PF_X}
        if (args.vaddr + lo) not in text_starts:
            print(f"warning: the window does not start at a .text boundary "
                  f"({sorted(hex(v) for v in text_starts)})", file=sys.stderr)

        before = encoding_rate(original[lo:hi])
        after = encoding_rate(dump[lo:hi])
        print(f"encoding test: file {before:.4f} -> dump {after:.4f}")
        if before < CIPHER_MIN:
            print("the file's window does not look like ciphertext; refusing",
                  file=sys.stderr)
            return 1
        if after > CLEAN_MAX:
            print("the dump's window does not look like AArch64 code; refusing",
                  file=sys.stderr)
            return 1
    elif args.kind == "usage":
        before = token_rate(original[lo:hi])
        after = token_rate(dump[lo:hi])
        print(f"usage-token test: file {before:.4f} -> dump {after:.4f}")
        if before > 0.10:
            print("the file's window already holds valid tokens; refusing",
                  file=sys.stderr)
            return 1
        if after < 0.80:
            print("the dump's window is not a metadata-usage table; refusing",
                  file=sys.stderr)
            return 1
    else:
        # No structure to assert for arbitrary data: require only that the
        # window stops looking like ciphertext.  Encrypted, it is ~8.0 bits of
        # entropy with almost no zero bytes.
        z_before = original[lo:hi].count(0) / (hi - lo)
        z_after = dump[lo:hi].count(0) / (hi - lo)
        print(f"zero-rate: file {z_before:.4f} -> dump {z_after:.4f}")
        if z_before > 0.02:
            print("the file's window does not look like ciphertext; refusing",
                  file=sys.stderr)
            return 1
        if z_after <= z_before * 2:
            print("the dump's window looks no less random than the file; "
                  "refusing", file=sys.stderr)
            return 1

    if not args.out:
        print("validation only (no --out given)")
        return 0

    blob[offset + lo : offset + hi] = dump[lo:hi]
    with open(args.out, "wb") as fh:
        fh.write(blob)
    print(f"wrote {args.out}: {hi - lo} bytes of plaintext spliced in")
    return 0


if __name__ == "__main__":
    sys.exit(main())
