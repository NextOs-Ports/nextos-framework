#!/usr/bin/env python3
"""Validate the exact Sonic 4 Episode II v3 ARM64 payload and its ZIP sources."""

import hashlib
import os
import struct
import sys
import time
import zipfile
from pathlib import Path, PurePosixPath


LIBFOX_SIZE = 12_220_336
LIBFOX_SHA256 = "ca07163ad1e92d767016d43048a2c13eede7b9d6217ed4f032ca4d6d8e342a1a"
LIBFOX_BUILD_ID = "fb0e884de8487a837d5fdda6ea35f731f21cff32"
DATA_OBB_SIZE = 673_277_896
DATA_OBB_SHA256 = "a2c988a0c2b057b27a328053cef1627e16b6491f5b6700f9627cdc141f82012b"
DATA_OBB_MAGIC = b"LPK\0"

CHUNK_SIZE = 1024 * 1024
PROGRESS_MIN_INTERVAL = 0.10
BUNDLE_MEMBERS = ("split_config.arm64_v8a.apk", "split_packs.apk")
BUNDLE_MEMBER_SIZES = {
    "split_config.arm64_v8a.apk": 12_247_458,
    "split_packs.apk": 843_668_368,
}
BUNDLE_MAX_UNCOMPRESSED = 1_200_000_000


def fail(message):
    raise SystemExit("validation failed: %s" % message)


def env_int(name, default):
    try:
        return int(os.environ.get(name, str(default)))
    except ValueError:
        return default


class SetupProgressReporter:
    """Best-effort UI updates; validation never depends on this side channel."""

    def __init__(self):
        self.path = os.environ.get("SONIC_SETUP_PROGRESS_FILE", "")
        self.phase = max(0, min(8, env_int("SONIC_SETUP_PHASE", 2)))
        self.base = max(0, min(1000, env_int("SONIC_VALIDATION_BASE", 0)))
        self.span = max(
            0, min(1000 - self.base, env_int("SONIC_VALIDATION_SPAN", 1000))
        )
        self.extraction = max(
            0, min(1000, env_int("SONIC_EXTRACTION_PERMILLE", 0))
        )
        message = os.environ.get("SONIC_SETUP_MESSAGE", "VALIDATING GAME DATA")
        self.message = " ".join(
            message.replace("\r", " ").replace("\n", " ").split()
        )
        self.last_value = -1
        self.last_write = 0.0
        self.disabled = not self.path

    def update(self, done, total, force=False):
        if self.disabled:
            return
        if total <= 0:
            value = self.base + (self.span if force else 0)
        else:
            done = max(0, min(done, total))
            value = self.base + self.span * done // total
        value = max(0, min(1000, value))

        now = time.monotonic()
        if not force and value == self.last_value:
            return
        if not force and self.last_write and now - self.last_write < PROGRESS_MIN_INTERVAL:
            return

        active = self.extraction if self.phase == 4 else value
        tmp = "%s.py.%d" % (self.path, os.getpid())
        try:
            with open(tmp, "w", encoding="utf-8") as stream:
                stream.write("1 %d 1000\n" % active)
                stream.write("%s\n" % (self.message or "VALIDATING GAME DATA"))
                stream.write(
                    "SONIC_SETUP_V2 %d %d %d\n"
                    % (self.phase, value, self.extraction)
                )
            os.replace(tmp, self.path)
        except OSError:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            self.disabled = True
            return
        self.last_value = value
        self.last_write = now


def regular_members(archive):
    return [info for info in archive.infolist() if not info.is_dir()]


def stream_member(source, output, reporter, progress):
    done, total = progress
    while True:
        chunk = source.read(CHUNK_SIZE)
        if not chunk:
            break
        if output is not None:
            output.write(chunk)
        done += len(chunk)
        reporter.update(done, total)
    return done, total


def validate_archive_crc(path, announce=True):
    archive_path = Path(path)
    reporter = SetupProgressReporter()
    try:
        with zipfile.ZipFile(archive_path, "r") as archive:
            members = regular_members(archive)
            total = sum(info.file_size for info in members)
            done = 0
            reporter.update(0, total, force=True)
            for info in members:
                if info.flag_bits & 1:
                    fail("%s contains encrypted member %s" % (archive_path.name, info.filename))
                with archive.open(info, "r") as source:
                    done, _ = stream_member(source, None, reporter, (done, total))
            reporter.update(total, total, force=True)
    except (OSError, zipfile.BadZipFile, RuntimeError, NotImplementedError) as error:
        fail("%s ZIP test failed: %s" % (archive_path.name, error))
    if announce:
        print("%s ZIP CRC OK size=%d" % (archive_path.name, archive_path.stat().st_size))


def bundle_member_map(archive):
    found = {}
    for info in regular_members(archive):
        base = PurePosixPath(info.filename).name
        if base not in BUNDLE_MEMBERS:
            continue
        if base in found:
            fail("bundle contains duplicate %s" % base)
        found[base] = info
    missing = [name for name in BUNDLE_MEMBERS if name not in found]
    if missing:
        fail("bundle is missing %s" % ", ".join(missing))
    for name, expected_size in BUNDLE_MEMBER_SIZES.items():
        actual_size = found[name].file_size
        if actual_size != expected_size:
            fail(
                "%s size %d (expected %d)"
                % (name, actual_size, expected_size)
            )
    return found


def validate_and_expand_bundle(path, output_dir):
    archive_path = Path(path)
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    reporter = SetupProgressReporter()
    partials = {}
    try:
        with zipfile.ZipFile(archive_path, "r") as archive:
            selected = bundle_member_map(archive)
            members = regular_members(archive)
            total = sum(info.file_size for info in members)
            if total > BUNDLE_MAX_UNCOMPRESSED:
                fail(
                    "bundle uncompressed size %d exceeds safety limit %d"
                    % (total, BUNDLE_MAX_UNCOMPRESSED)
                )
            done = 0
            reporter.update(0, total, force=True)
            for info in members:
                if info.flag_bits & 1:
                    fail("bundle contains encrypted member %s" % info.filename)
                base = PurePosixPath(info.filename).name
                target = None
                stream = None
                if base in selected and selected[base] is info:
                    target = out / base
                    partial = out / (base + ".part.%d" % os.getpid())
                    partials[base] = partial
                    stream = partial.open("wb")
                try:
                    with archive.open(info, "r") as source:
                        done, _ = stream_member(source, stream, reporter, (done, total))
                    if stream is not None:
                        stream.flush()
                        os.fsync(stream.fileno())
                finally:
                    if stream is not None:
                        stream.close()
                if target is not None:
                    os.replace(partials.pop(base), target)
            reporter.update(total, total, force=True)
    except (OSError, zipfile.BadZipFile, RuntimeError, NotImplementedError) as error:
        for partial in partials.values():
            try:
                partial.unlink()
            except OSError:
                pass
        fail("%s bundle extraction failed: %s" % (archive_path.name, error))
    print("%s bundle CRC OK and splits extracted" % archive_path.name)


def entry_info(path, entry):
    try:
        with zipfile.ZipFile(path, "r") as archive:
            info = archive.getinfo(entry)
    except (OSError, KeyError, zipfile.BadZipFile) as error:
        fail("cannot read %s from %s: %s" % (entry, Path(path).name, error))
    print("%d %08x" % (info.file_size, info.CRC))


def find_entry(path, basename):
    try:
        with zipfile.ZipFile(path, "r") as archive:
            matches = [
                info.filename
                for info in regular_members(archive)
                if PurePosixPath(info.filename).name == basename
            ]
    except (OSError, zipfile.BadZipFile) as error:
        fail("cannot inspect %s: %s" % (Path(path).name, error))
    if len(matches) != 1:
        fail("expected one %s in %s, found %d" % (basename, Path(path).name, len(matches)))
    print(matches[0])


def has_entry(path, entry):
    try:
        with zipfile.ZipFile(path, "r") as archive:
            archive.getinfo(entry)
    except (OSError, KeyError, zipfile.BadZipFile):
        raise SystemExit(1)


def hash_file(path, reporter, done, total):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while True:
            chunk = stream.read(CHUNK_SIZE)
            if not chunk:
                break
            digest.update(chunk)
            done += len(chunk)
            reporter.update(done, total)
    return digest.hexdigest(), done


def elf_build_id(path):
    with Path(path).open("rb") as stream:
        header = stream.read(64)
        if len(header) < 64 or header[:4] != b"\x7fELF":
            fail("libfox.so is not an ELF file")
        if header[4] != 2 or header[5] != 1:
            fail("libfox.so is not little-endian ELF64")
        if struct.unpack_from("<H", header, 18)[0] != 183:
            fail("libfox.so is not AArch64")

        phoff = struct.unpack_from("<Q", header, 32)[0]
        phentsize = struct.unpack_from("<H", header, 54)[0]
        phnum = struct.unpack_from("<H", header, 56)[0]
        if phentsize < 56 or phnum > 1024:
            fail("invalid ELF program-header table")

        for index in range(phnum):
            stream.seek(phoff + index * phentsize)
            phdr = stream.read(56)
            if len(phdr) != 56:
                fail("truncated ELF program-header table")
            p_type, _flags, p_offset, _vaddr, _paddr, p_filesz, _memsz, _align = (
                struct.unpack("<IIQQQQQQ", phdr)
            )
            if p_type != 4:
                continue
            if p_filesz > 16 * 1024 * 1024:
                fail("unreasonable ELF note segment")
            stream.seek(p_offset)
            data = stream.read(p_filesz)
            if len(data) != p_filesz:
                fail("truncated ELF note segment")
            note = 0
            while note + 12 <= len(data):
                namesz, descsz, note_type = struct.unpack_from("<III", data, note)
                note += 12
                name_end = note + namesz
                desc_start = (name_end + 3) & ~3
                desc_end = desc_start + descsz
                next_note = (desc_end + 3) & ~3
                if name_end > len(data) or desc_end > len(data) or next_note > len(data):
                    fail("truncated ELF note")
                name = data[note:name_end].rstrip(b"\0")
                if name == b"GNU" and note_type == 3:
                    return data[desc_start:desc_end].hex()
                note = next_note
    fail("GNU BuildID not found in libfox.so")


def validate_payload(libfox_path, obb_path):
    libfox = Path(libfox_path)
    obb = Path(obb_path)
    try:
        lib_size = libfox.stat().st_size
        obb_size = obb.stat().st_size
    except OSError as error:
        fail("payload stat failed: %s" % error)
    if lib_size != LIBFOX_SIZE:
        fail("libfox.so size %d (expected %d)" % (lib_size, LIBFOX_SIZE))
    if obb_size != DATA_OBB_SIZE:
        fail("data.obb size %d (expected %d)" % (obb_size, DATA_OBB_SIZE))

    try:
        with obb.open("rb") as stream:
            magic = stream.read(4)
    except OSError as error:
        fail("data.obb read failed: %s" % error)
    if magic != DATA_OBB_MAGIC:
        fail("data.obb has invalid LPK magic %s" % magic.hex())

    reporter = SetupProgressReporter()
    total = lib_size + obb_size
    reporter.update(0, total, force=True)
    try:
        lib_digest, done = hash_file(libfox, reporter, 0, total)
        if lib_digest != LIBFOX_SHA256:
            fail("libfox.so SHA256 %s (expected %s)" % (lib_digest, LIBFOX_SHA256))
        build_id = elf_build_id(libfox)
        if build_id != LIBFOX_BUILD_ID:
            fail("libfox.so BuildID %s (expected %s)" % (build_id, LIBFOX_BUILD_ID))
        obb_digest, done = hash_file(obb, reporter, done, total)
    except OSError as error:
        fail("payload hash failed: %s" % error)
    if obb_digest != DATA_OBB_SHA256:
        fail("data.obb SHA256 %s (expected %s)" % (obb_digest, DATA_OBB_SHA256))
    reporter.update(total, total, force=True)
    print("libfox.so OK sha256=%s buildid=%s" % (lib_digest, build_id))
    print("data.obb OK sha256=%s magic=%s" % (obb_digest, magic.hex()))


def usage():
    fail(
        "usage: validate-sonic4-data.py archive ZIP | bundle ZIP OUTDIR | "
        "payload LIBFOX OBB | has ZIP ENTRY | entry-info ZIP ENTRY | "
        "find-entry ZIP BASENAME"
    )


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "archive":
        validate_archive_crc(sys.argv[2])
        return
    if len(sys.argv) == 4 and sys.argv[1] == "bundle":
        validate_and_expand_bundle(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) == 4 and sys.argv[1] == "payload":
        validate_payload(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) == 4 and sys.argv[1] == "has":
        has_entry(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) == 4 and sys.argv[1] == "entry-info":
        entry_info(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) == 4 and sys.argv[1] == "find-entry":
        find_entry(sys.argv[2], sys.argv[3])
        return
    usage()


if __name__ == "__main__":
    main()
