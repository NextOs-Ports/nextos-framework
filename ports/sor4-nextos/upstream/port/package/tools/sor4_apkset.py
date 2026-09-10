#!/usr/bin/env python3
"""Safe helpers for complete SOR4 APK sets and APK bundle exports."""

import binascii
import hashlib
import os
import shutil
import stat
import sys
import zipfile


MAX_APK_SOURCES = 64
MAX_BUNDLE_APKS = 64
MAX_INNER_APK_BYTES = 2_500_000_000
MAX_BUNDLE_APK_BYTES = 3_000_000_000


def _regular_file(path):
    try:
        mode = os.lstat(path).st_mode
    except OSError:
        return False
    return stat.S_ISREG(mode)


def safe_asset_relative(info):
    name = info.filename
    if not name.startswith("assets/"):
        return None
    if "\x00" in name or "\\" in name or any(ord(char) < 32 for char in name):
        raise ValueError("unsafe ZIP entry name: %r" % name)
    relative = name[len("assets/") :]
    if info.is_dir():
        if relative == "":
            return None
        if not relative.endswith("/"):
            raise ValueError("ambiguous asset directory: %r" % name)
        relative = relative[:-1]
    if not relative or relative.startswith("/"):
        raise ValueError("unsafe asset path: %r" % name)
    parts = relative.split("/")
    if any(part in ("", ".", "..") or ":" in part for part in parts):
        raise ValueError("unsafe asset path: %r" % name)
    return "/".join(parts)


class ApkSet:
    """Open APKs and expose one collision-safe virtual asset namespace."""

    def __init__(self, paths):
        if not paths:
            raise ValueError("no APK sources were supplied")
        if len(paths) > MAX_APK_SOURCES:
            raise ValueError("too many APK sources (%d; maximum %d)" %
                             (len(paths), MAX_APK_SOURCES))
        self.paths = []
        self.archives = []
        self.entries = {}
        self.assets = []
        self.fonts = []
        self._open(paths)

    def _open(self, paths):
        try:
            seen_paths = set()
            asset_destinations = {}
            flattened_fonts = {}
            for original in paths:
                path = os.path.abspath(original)
                if path in seen_paths:
                    continue
                seen_paths.add(path)
                if not _regular_file(path):
                    raise ValueError("APK source is missing, linked or not regular: " + original)
                archive = zipfile.ZipFile(path)
                source_index = len(self.archives)
                self.paths.append(path)
                self.archives.append(archive)
                local_names = set()
                for info in archive.infolist():
                    if info.filename in local_names:
                        raise ValueError("duplicate ZIP entry in %s: %s" %
                                         (os.path.basename(path), info.filename))
                    local_names.add(info.filename)

                    previous = self.entries.get(info.filename)
                    if previous is None:
                        self.entries[info.filename] = (source_index, info)
                    elif info.filename in (
                        "lib/arm64-v8a/libWwise.so",
                        "lib/arm64-v8a/libassemblies.arm64-v8a.blob.so",
                    ):
                        old_info = previous[1]
                        if (old_info.file_size, old_info.CRC) != (info.file_size, info.CRC):
                            raise ValueError("mixed APK set: conflicting " + info.filename)

                    relative = safe_asset_relative(info)
                    if relative is None or info.is_dir():
                        continue
                    key = relative.casefold()
                    previous_asset = asset_destinations.get(key)
                    if previous_asset is not None:
                        old_relative, old_info = previous_asset[2], previous_asset[1]
                        if (old_relative != relative or
                                (old_info.file_size, old_info.CRC) !=
                                (info.file_size, info.CRC)):
                            raise ValueError(
                                "mixed APK set: conflicting asset destination %s / %s"
                                % (old_relative, relative)
                            )
                        continue
                    record = (source_index, info, relative)
                    asset_destinations[key] = record
                    self.assets.append(record)
                    if relative.lower().endswith((".ttf", ".otf")):
                        basename_key = relative.rsplit("/", 1)[-1].casefold()
                        if basename_key in flattened_fonts:
                            raise ValueError("duplicate flattened font destination: " + relative)
                        flattened_fonts[basename_key] = relative
                        self.fonts.append(record)
        except Exception:
            self.close()
            raise

    def get(self, name):
        return self.entries.get(name)

    def open(self, record):
        source_index, info = record[:2]
        return self.archives[source_index].open(info)

    def close(self):
        for archive in self.archives:
            try:
                archive.close()
            except Exception:
                pass
        self.archives = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()


def _safe_bundle_member(info):
    name = info.filename
    if "\x00" in name or "\\" in name or any(ord(char) < 32 for char in name):
        raise ValueError("unsafe bundle member name: %r" % name)
    if name.startswith("/"):
        raise ValueError("unsafe bundle member path: %r" % name)
    parts = name.split("/")
    if any(part in ("", ".", "..") or ":" in part for part in parts):
        raise ValueError("unsafe bundle member path: %r" % name)
    return parts[-1]


def inspect_bundle(path):
    if not _regular_file(path):
        raise ValueError("bundle is missing, linked or not regular: " + path)
    members = []
    with zipfile.ZipFile(path) as archive:
        names = set()
        for info in archive.infolist():
            if info.filename in names:
                raise ValueError("duplicate bundle member: " + info.filename)
            names.add(info.filename)
            if info.is_dir():
                continue
            basename = _safe_bundle_member(info)
            if not basename.lower().endswith(".apk"):
                continue
            if info.file_size <= 0 or info.file_size > MAX_INNER_APK_BYTES:
                raise ValueError("inner APK has unsafe size: " + info.filename)
            members.append(info)
    if not members:
        raise ValueError("bundle contains no APK members")
    if len(members) > MAX_BUNDLE_APKS:
        raise ValueError("bundle contains too many APK members")
    total = sum(info.file_size for info in members)
    if total > MAX_BUNDLE_APK_BYTES:
        raise ValueError("bundle APK payload exceeds the 3 GB safety limit")
    return members, total


def _crc32_file(path):
    value = 0
    with open(path, "rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                return value & 0xFFFFFFFF
            value = binascii.crc32(block, value)


def expand_bundles(output_dir, bundles):
    if not bundles:
        return []
    if os.path.lexists(output_dir) and (os.path.islink(output_dir) or not os.path.isdir(output_dir)):
        raise ValueError("bundle staging path is linked or not a directory")
    os.makedirs(output_dir, exist_ok=True)
    output_dir = os.path.realpath(output_dir)
    outputs = []
    for bundle_index, bundle in enumerate(bundles):
        members, _ = inspect_bundle(bundle)
        with zipfile.ZipFile(bundle) as archive:
            for member_index, info in enumerate(members):
                token = hashlib.sha256(info.filename.encode("utf-8")).hexdigest()[:12]
                name = "bundle-%02d-%02d-%s.apk" % (bundle_index, member_index, token)
                destination = os.path.abspath(os.path.join(output_dir, name))
                if os.path.commonpath((output_dir, destination)) != output_dir:
                    raise ValueError("bundle output escapes staging")
                valid_cached = False
                if _regular_file(destination):
                    try:
                        valid_cached = (os.path.getsize(destination) == info.file_size and
                                        _crc32_file(destination) == info.CRC)
                    except OSError:
                        pass
                if not valid_cached:
                    temporary = destination + ".sor4-part"
                    try:
                        if os.path.lexists(destination):
                            if os.path.isdir(destination) and not os.path.islink(destination):
                                raise ValueError("bundle cache target is a directory: " + name)
                            os.unlink(destination)
                        try:
                            os.unlink(temporary)
                        except FileNotFoundError:
                            pass
                        with archive.open(info) as source, open(temporary, "xb") as output:
                            shutil.copyfileobj(source, output, 1024 * 1024)
                            output.flush()
                            os.fsync(output.fileno())
                        if os.path.getsize(temporary) != info.file_size:
                            raise IOError("short bundle extraction for " + info.filename)
                        os.replace(temporary, destination)
                    finally:
                        try:
                            os.unlink(temporary)
                        except FileNotFoundError:
                            pass
                outputs.append(destination)
    keep = set(outputs)
    for name in os.listdir(output_dir):
        candidate = os.path.join(output_dir, name)
        if name.endswith(".sor4-part"):
            try:
                os.unlink(candidate)
            except OSError:
                pass
        elif name.startswith("bundle-") and name.endswith(".apk") and candidate not in keep:
            try:
                os.unlink(candidate)
            except OSError:
                pass
    return outputs


def main(argv):
    if len(argv) < 2 or argv[1] not in ("bundle-bytes", "expand-bundles"):
        print("usage: sor4_apkset.py bundle-bytes BUNDLE... | "
              "expand-bundles OUTPUT_DIR BUNDLE...", file=sys.stderr)
        return 2
    try:
        if argv[1] == "bundle-bytes":
            if len(argv) < 3:
                raise ValueError("no bundle was supplied")
            total = sum(inspect_bundle(path)[1] for path in argv[2:])
            print(total)
            return 0
        if len(argv) < 4:
            raise ValueError("output directory and bundle are required")
        for output in expand_bundles(argv[2], argv[3:]):
            print(output)
        return 0
    except (OSError, zipfile.BadZipFile, ValueError, RuntimeError, NotImplementedError) as error:
        print("SOR4 APK bundle preparation failed: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
