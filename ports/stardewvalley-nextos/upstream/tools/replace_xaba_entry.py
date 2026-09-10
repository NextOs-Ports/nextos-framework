#!/usr/bin/env python3
"""Replace one compressed assembly in a Xamarin Android XABA v2 store.

The replacement is deliberately in-place: it may be smaller than the original
slot, but never larger.  That keeps the ELF payload and every following store
offset untouched.
"""

import argparse
import pathlib
import struct
import sys

import lz4.block


XABA_MAGIC = b"XABA"
XALZ_MAGIC = b"XALZ"
XABA_HEADER = struct.Struct("<4sIIII")
XABA_DESCRIPTOR = struct.Struct("<7I")
XALZ_HEADER = struct.Struct("<4sII")


def decode_entry(blob, store_offset, data_offset, data_size):
    entry = bytes(blob[store_offset + data_offset :
                       store_offset + data_offset + data_size])
    if entry[:4] != XALZ_MAGIC:
        return entry, None

    magic, mapping_index, uncompressed_size = XALZ_HEADER.unpack_from(entry)
    decoded = lz4.block.decompress(
        entry[XALZ_HEADER.size :], uncompressed_size=uncompressed_size
    )
    if len(decoded) != uncompressed_size:
        raise ValueError("XALZ decompressed size mismatch")
    return decoded, mapping_index


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("store", type=pathlib.Path)
    parser.add_argument("original_assembly", type=pathlib.Path)
    parser.add_argument("replacement_assembly", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    blob = bytearray(args.store.read_bytes())
    original = args.original_assembly.read_bytes()
    replacement = args.replacement_assembly.read_bytes()
    store_offset = blob.find(XABA_MAGIC)
    if store_offset < 0:
        raise ValueError("XABA store not found")

    magic, version, entry_count, index_count, index_size = XABA_HEADER.unpack_from(
        blob, store_offset
    )
    if magic != XABA_MAGIC or version != 0x80010002:
        raise ValueError("unsupported XABA store version")
    if index_size != index_count * 12:
        raise ValueError("unexpected XABA index layout")

    descriptors_offset = store_offset + XABA_HEADER.size + index_size
    match = None
    for entry_index in range(entry_count):
        descriptor_offset = descriptors_offset + entry_index * XABA_DESCRIPTOR.size
        descriptor = XABA_DESCRIPTOR.unpack_from(blob, descriptor_offset)
        mapping_index, data_offset, data_size = descriptor[:3]
        decoded, compressed_mapping = decode_entry(
            blob, store_offset, data_offset, data_size
        )
        if decoded != original:
            continue
        if match is not None:
            raise ValueError("original assembly occurs more than once")
        match = (descriptor_offset, mapping_index, data_offset, data_size,
                 compressed_mapping)

    if match is None:
        raise ValueError("original assembly not found in XABA store")

    descriptor_offset, mapping_index, data_offset, old_size, compressed_mapping = match
    if compressed_mapping is None:
        encoded = replacement
    else:
        compressed = lz4.block.compress(
            replacement,
            mode="high_compression",
            compression=12,
            store_size=False,
        )
        encoded = XALZ_HEADER.pack(
            XALZ_MAGIC, compressed_mapping, len(replacement)
        ) + compressed

    if len(encoded) > old_size:
        raise ValueError(
            "replacement does not fit XABA slot: {} > {} bytes".format(
                len(encoded), old_size
            )
        )

    absolute_data = store_offset + data_offset
    blob[absolute_data : absolute_data + len(encoded)] = encoded
    blob[absolute_data + len(encoded) : absolute_data + old_size] = bytes(
        old_size - len(encoded)
    )
    struct.pack_into("<I", blob, descriptor_offset + 8, len(encoded))

    verified, _ = decode_entry(blob, store_offset, data_offset, len(encoded))
    if verified != replacement:
        raise ValueError("replacement verification failed")

    args.output.write_bytes(blob)
    print(
        "replaced XABA entry {} (mapping {}): {} -> {} bytes".format(
            (descriptor_offset - descriptors_offset) // XABA_DESCRIPTOR.size,
            mapping_index,
            old_size,
            len(encoded),
        )
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print("error: {}".format(error), file=sys.stderr)
        sys.exit(1)
