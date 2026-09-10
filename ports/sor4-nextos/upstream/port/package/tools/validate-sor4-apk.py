#!/usr/bin/env python3
"""Validate the exact supported SOR4 1.4.5 Android data before extraction."""

import hashlib
import struct
import sys
import zipfile

from sor4_apkset import ApkSet


EXPECTED = {
    "assets/blank.xnb": (
        97,
        "b8d4449f6895fdc5e57bd4a95e1ca63f28c7204eee8bd61517498a078213299f",
    ),
    "assets/bigfile": (
        3_928_526,
        "98489edb6bbe65c24ff7620de7e96292c65ea232f34742f13bdedac986190ccb",
    ),
    "lib/arm64-v8a/libWwise.so": (
        2_939_888,
        "4db3d430cde5525f3eeaa99af6e46c44feb554d89d76955b1391dfd5dd2cf0f2",
    ),
    "lib/arm64-v8a/libassemblies.arm64-v8a.blob.so": (
        3_370_059,
        "4c9976a0b66a689120d586d4a45ac35adaa9f4fa475b01a5a951c01b278f9c66",
    ),
}
EXPECTED_ASSET_COUNT = 25_905
EXPECTED_ASSET_BYTES = 1_836_494_230
EXPECTED_WEM_COUNT = 613
EXPECTED_ASSET_FINGERPRINT = (
    "fc6435c0dcd7026247cf60faf6f490f12cac9cbc1384d997ec018c0c83eca424"
)
ACCUMULATOR_MODULUS = 1 << 256
EXPECTED_XNB_COUNT = 25_224
EXPECTED_XNB_BYTES = 0x3CE6EF35
EXPECTED_XNB_SUM = int(
    "de8761e0aefc07d27efdfd324ced139a7a353f633a54a9188996ce2054e00060", 16
)
EXPECTED_XNB_XOR = int(
    "03020b568ea5d01355be5e414bce6f0849930ce88e0f1afaadaf7dcd7210015a", 16
)
EXPECTED_XNB_SQUARE_SUM = int(
    "e03c437c9aa65edda543ef8b42916428f28ed908be6c1fc2e448692192476e36",
    16,
)
EXPECTED_NON_XNB_COUNT = 681
EXPECTED_NON_XNB_BYTES = 0x308FBE61
EXPECTED_NON_XNB_SUM = int(
    "460fe010da29084c296d10ec75ca637c193c485f4ddb5eba21b6c419eb4eccd3", 16
)
EXPECTED_NON_XNB_XOR = int(
    "c0e33337a8a635d2ef60914fafc1d56aa91aaea5332c2600f2bbb1e0b02ca055",
    16,
)
EXPECTED_NON_XNB_SQUARE_SUM = int(
    "3c9064345a8f6c95f492b86f6df7220bef5b373d7de6193709c93d78c17c6737",
    16,
)


def digest(stream):
    value = hashlib.sha256()
    while True:
        block = stream.read(1024 * 1024)
        if not block:
            return value.hexdigest()
        value.update(block)


def asset_fingerprint(asset_entries):
    """Fingerprint the collision-safe virtual asset tree across every split."""
    fingerprint = hashlib.sha256()
    for _, entry, relative in sorted(asset_entries, key=lambda item: item[2]):
        fingerprint.update(asset_record(relative, entry))
    return fingerprint.hexdigest()


def asset_record(relative, entry):
    encoded = relative.encode("utf-8")
    record = bytearray()
    record.extend(struct.pack("<I", len(encoded)))
    record.extend(encoded)
    # Content identity independent of ZIP recompression: path + exact size +
    # the per-entry content CRC verified by ZipFile while extracting.
    record.extend(struct.pack("<QI", entry.file_size, entry.CRC))
    return bytes(record)


def asset_accumulator(asset_entries):
    """Order-independent proof used only for the one-missing-XNB compatibility set."""
    total = 0
    exclusive_or = 0
    square_sum = 0
    for _, entry, relative in asset_entries:
        token = int.from_bytes(
            hashlib.sha256(asset_record(relative, entry)).digest(), "big"
        )
        total = (total + token) % ACCUMULATOR_MODULUS
        exclusive_or ^= token
        square_sum = (square_sum + token * token) % ACCUMULATOR_MODULUS
    return total, exclusive_or, square_sum


def validate_one_missing_xnb(asset_entries):
    """Prove this is the exact 1.4.5 tree with one XNB texture record absent."""
    xnb_entries = [
        record for record in asset_entries if record[2].lower().endswith(".xnb")
    ]
    non_xnb_entries = [
        record for record in asset_entries if not record[2].lower().endswith(".xnb")
    ]
    if len(xnb_entries) != EXPECTED_XNB_COUNT - 1:
        raise ValueError(
            "the single missing asset is not an XNB texture "
            "(%d XNB files; expected %d)"
            % (len(xnb_entries), EXPECTED_XNB_COUNT - 1)
        )
    if len(non_xnb_entries) != EXPECTED_NON_XNB_COUNT:
        raise ValueError(
            "non-texture Android data is incomplete "
            "(%d files; expected %d)"
            % (len(non_xnb_entries), EXPECTED_NON_XNB_COUNT)
        )

    non_xnb_bytes = sum(entry.file_size for _, entry, _ in non_xnb_entries)
    if non_xnb_bytes != EXPECTED_NON_XNB_BYTES:
        raise ValueError("non-texture Android data size does not match version 1.4.5")
    if asset_accumulator(non_xnb_entries) != (
        EXPECTED_NON_XNB_SUM,
        EXPECTED_NON_XNB_XOR,
        EXPECTED_NON_XNB_SQUARE_SUM,
    ):
        raise ValueError("non-texture Android data does not match version 1.4.5")

    xnb_bytes = sum(entry.file_size for _, entry, _ in xnb_entries)
    if not 0 < EXPECTED_XNB_BYTES - xnb_bytes < EXPECTED_XNB_BYTES:
        raise ValueError("the missing XNB texture has an invalid size")
    actual_sum, actual_xor, actual_square_sum = asset_accumulator(xnb_entries)
    missing_token = (EXPECTED_XNB_SUM - actual_sum) % ACCUMULATOR_MODULUS
    if missing_token == 0:
        raise ValueError("the missing XNB texture identity is invalid")
    if (EXPECTED_XNB_XOR ^ actual_xor) != missing_token:
        raise ValueError("the XNB tree is not an exact one-file-short 1.4.5 set")
    expected_missing_square = (
        EXPECTED_XNB_SQUARE_SUM - actual_square_sum
    ) % ACCUMULATOR_MODULUS
    if expected_missing_square != (missing_token * missing_token) % ACCUMULATOR_MODULUS:
        raise ValueError("the XNB tree is not an exact one-file-short 1.4.5 set")
    return "%064x" % missing_token


def main():
    if len(sys.argv) < 2:
        print("usage: validate-sor4-apk.py GAME.apk [SPLIT.apk ...]", file=sys.stderr)
        return 2

    try:
        with ApkSet(sys.argv[1:]) as apk_set:
            manifest_fingerprint = asset_fingerprint(apk_set.assets)
            missing_xnb_token = None
            for name, (expected_size, expected_hash) in EXPECTED.items():
                record = apk_set.get(name)
                if record is None:
                    raise ValueError("missing required entry: " + name)
                entry = record[1]
                if entry.file_size != expected_size:
                    raise ValueError(
                        "%s has size %d; expected %d"
                        % (name, entry.file_size, expected_size)
                    )
                with apk_set.open(record) as stream:
                    actual_hash = digest(stream)
                if actual_hash != expected_hash:
                    raise ValueError(name + " does not match SOR4 Android 1.4.5")

            asset_entries = [entry for _, entry, _ in apk_set.assets]
            assets = len(asset_entries)
            if assets not in (EXPECTED_ASSET_COUNT, EXPECTED_ASSET_COUNT - 1):
                raise ValueError(
                    "incomplete or unexpected assets directory "
                    "(%d files; expected 25905 or the compatible 25904)"
                    % assets
                )
            asset_bytes = sum(entry.file_size for entry in asset_entries)
            wem_files = sum(
                1
                for _, _, relative in apk_set.assets
                if relative.lower().endswith(".wem")
            )
            if wem_files != EXPECTED_WEM_COUNT:
                raise ValueError(
                    "incomplete streamed audio (%d WEM files; expected 613)"
                    % wem_files
                )
            if assets == EXPECTED_ASSET_COUNT:
                if asset_bytes != EXPECTED_ASSET_BYTES:
                    raise ValueError(
                        "unexpected uncompressed asset size (%d bytes; expected %d)"
                        % (asset_bytes, EXPECTED_ASSET_BYTES)
                    )
                if manifest_fingerprint != EXPECTED_ASSET_FINGERPRINT:
                    raise ValueError("asset manifest does not match SOR4 Android 1.4.5")
            else:
                missing_xnb_token = validate_one_missing_xnb(apk_set.assets)
    except (OSError, zipfile.BadZipFile, ValueError, RuntimeError, NotImplementedError) as error:
        print("SOR4 APK validation failed: %s" % error, file=sys.stderr)
        return 1

    if missing_xnb_token is None:
        print(
            "SOR4 Android 1.4.5 data validated (%d assets, %d WEM)"
            % (assets, wem_files)
        )
    else:
        print(
            "SOR4 Android 1.4.5 data validated "
            "(%d assets, %d WEM; one missing XNB uses blank.xnb fallback)"
            % (assets, wem_files)
        )
    print("apk_sources=%d" % len(apk_set.paths))
    print("asset_count=%d" % assets)
    if missing_xnb_token is not None:
        print("compatibility=one-missing-xnb")
        print("missing_xnb_token=" + missing_xnb_token)
    print("asset_fingerprint=" + manifest_fingerprint)
    return 0


if __name__ == "__main__":
    sys.exit(main())
