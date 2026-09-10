#!/usr/bin/env python3
"""Generate PF2's version-pinned XOR transformation masks.

Developer-only reproducibility tool.  It combines the encrypted ranges from
the exact v1.09.3 ARM64 split with four owner-captured Android plaintext
windows.  The output masks are unusable on their own; the release hook applies
them only after cryptographically validating the user's original libraries.
"""

from __future__ import print_function

import argparse
import hashlib
import os
import struct


WINDOWS = (
    (
        "libil2cpp.so",
        "libil2cpp.text",
        0x0118BF7C,
        0x0000C800,
        "9aace79e98b47dd5460a97cfa5810ba2bf7ceb0d7361bfc708598e5aa0c5edc5",
    ),
    (
        "libil2cpp.so",
        "libil2cpp.data",
        0x02F0E7A0,
        0x0000C7B0,
        "868e71e3a1ef63f9fecb25747c9ccae110419da79eb50d2c720d032f309d6025",
    ),
    (
        "libunity.so",
        "libunity.text",
        0x00397710,
        0x0000C800,
        "bc7dc02c875f9ebadb904abf031cfd5d141e2b583d0a0d9e42199c219702547b",
    ),
    (
        "libunity.so",
        "libunity.data",
        0x011CDBF8,
        0x00004810,
        "b364e95e0a94095a6cdae37d47cf90288135ef9a42c57d1eb8b7923e8a350f58",
    ),
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def vaddr_to_offset(image, vaddr, size):
    if image[:4] != b"\x7fELF" or image[4:6] != b"\x02\x01":
        raise RuntimeError("expected a little-endian ELF64 image")
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phentsize = struct.unpack_from("<H", image, 54)[0]
    phnum = struct.unpack_from("<H", image, 56)[0]
    for index in range(phnum):
        entry = phoff + index * phentsize
        p_type = struct.unpack_from("<I", image, entry)[0]
        if p_type != 1:
            continue
        p_offset, p_vaddr = struct.unpack_from("<QQ", image, entry + 8)
        p_filesz = struct.unpack_from("<Q", image, entry + 32)[0]
        if p_vaddr <= vaddr and vaddr + size <= p_vaddr + p_filesz:
            return p_offset + vaddr - p_vaddr
    raise RuntimeError("window is outside every file-backed PT_LOAD")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lib-dir", required=True)
    parser.add_argument("--overlay-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    os.makedirs(args.output_dir, exist_ok=True)

    for library_name, overlay_name, vaddr, size, expected_hash in WINDOWS:
        library = open(os.path.join(args.lib_dir, library_name), "rb").read()
        overlay = open(os.path.join(args.overlay_dir, overlay_name), "rb").read()
        if len(overlay) != size or sha256(overlay) != expected_hash:
            raise RuntimeError("unsupported owner overlay " + overlay_name)
        offset = vaddr_to_offset(library, vaddr, size)
        encrypted = library[offset : offset + size]
        mask = bytes(left ^ right for left, right in zip(encrypted, overlay))
        reconstructed = bytes(left ^ right for left, right in zip(encrypted, mask))
        if reconstructed != overlay:
            raise RuntimeError("internal reconstruction failure")
        path = os.path.join(args.output_dir, overlay_name + ".xormask")
        with open(path, "wb") as stream:
            stream.write(mask)
        print(
            "%s vaddr=%#x file=%#x size=%d mask_sha256=%s"
            % (overlay_name, vaddr, offset, size, sha256(mask))
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
