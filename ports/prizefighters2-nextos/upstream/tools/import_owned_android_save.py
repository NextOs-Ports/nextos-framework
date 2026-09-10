#!/usr/bin/env python3
"""Import an owner-exported Prizefighters 2 Android save into this port.

The importer copies only the game's documented save directories and merges a
real Unity PlayerPrefs XML (or an existing PF2PREF1 store) into the port's
typed SharedPreferences file.  It backs up every replaced file and never
creates, guesses or changes a premium flag, receipt or purchase token.
"""

from __future__ import print_function

import argparse
import math
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import time
import xml.etree.ElementTree as ET


MAGIC = b"PF2PREF1"
MAX_PREFS = 1024
MAX_PREF_BYTES = 4 * 1024 * 1024
MAX_SAVE_FILES = 4096
MAX_SAVE_FILE_BYTES = 64 * 1024 * 1024
MAX_SAVE_TOTAL_BYTES = 512 * 1024 * 1024

PREF_INT = 1
PREF_FLOAT = 2
PREF_STRING = 3
PREF_BOOL = 4

SAVE_DIRECTORIES = (
    "Career Saves",
    "Custom Fighters",
    "Custom Gyms",
    "Custom Leagues",
)

PREFERENCE_FILENAMES = (
    "com.koalitygame.prizefighters2.v2.playerprefs.xml",
    "com.koalitygame.prizefighters2.playerprefs.xml",
    "com.koalitygame.prizefighters2_preferences.xml",
)

# Android display/session identity must not override the handheld's native
# resolution or generate the save-dependent zoom regression seen in testing.
DEVICE_LOCAL_KEYS = {
    "unity.player_session_count",
    "unity.player_sessionid",
    "unity.cloud_userid",
}


class ImportError(RuntimeError):
    pass


def fail(message):
    raise ImportError("Prizefighters 2 save import failed: " + message)


def is_regular(path):
    try:
        return stat.S_ISREG(os.lstat(str(path)).st_mode)
    except OSError:
        return False


def is_real_directory(path):
    try:
        return stat.S_ISDIR(os.lstat(str(path)).st_mode)
    except OSError:
        return False


def is_device_local_key(key):
    if key in DEVICE_LOCAL_KEYS:
        return True
    normalised = key.replace("%20", " ").lower()
    return normalised.startswith("screenmanager resolution") or normalised.startswith(
        "screenmanager fullscreen"
    )


def decode_pf2_preferences(path):
    if not is_regular(path):
        fail("preference store is missing or not a regular file")
    size = os.path.getsize(str(path))
    if size < 12 or size > MAX_PREF_BYTES:
        fail("preference store has an unsafe size")
    data = path.read_bytes()
    if data[:8] != MAGIC:
        fail("preference store has an unsupported format")
    count = struct.unpack_from("<I", data, 8)[0]
    if count > MAX_PREFS:
        fail("preference store contains too many entries")
    entries = []
    seen = set()
    offset = 12
    for _index in range(count):
        if offset + 9 > len(data):
            fail("preference store is truncated")
        value_type = data[offset]
        key_length, value_length = struct.unpack_from("<II", data, offset + 1)
        offset += 9
        if (
            value_type < PREF_INT
            or value_type > PREF_BOOL
            or key_length == 0
            or key_length > 4096
            or value_length > 1024 * 1024
            or offset + key_length + value_length > len(data)
        ):
            fail("preference store contains an invalid entry")
        key_bytes = data[offset : offset + key_length]
        offset += key_length
        value_bytes = data[offset : offset + value_length]
        offset += value_length
        try:
            key = key_bytes.decode("utf-8")
        except UnicodeDecodeError:
            fail("preference store contains a non-UTF-8 key")
        if key in seen:
            fail("preference store contains a duplicate key")
        seen.add(key)
        if value_type == PREF_STRING:
            try:
                value = value_bytes.decode("utf-8")
            except UnicodeDecodeError:
                fail("preference store contains a non-UTF-8 string")
        elif value_length != 4:
            fail("numeric preference has an invalid size")
        elif value_type == PREF_FLOAT:
            value = struct.unpack("<f", value_bytes)[0]
            if not math.isfinite(value):
                fail("preference store contains a non-finite float")
        else:
            value = struct.unpack("<i", value_bytes)[0]
            if value_type == PREF_BOOL:
                value = bool(value)
        entries.append((key, value_type, value))
    if offset != len(data):
        fail("preference store has trailing data")
    return entries


def encode_pf2_preferences(entries):
    output = bytearray(MAGIC)
    output.extend(struct.pack("<I", len(entries)))
    for key, value_type, value in entries:
        key_bytes = key.encode("utf-8")
        if value_type == PREF_STRING:
            value_bytes = value.encode("utf-8")
        elif value_type == PREF_FLOAT:
            value_bytes = struct.pack("<f", float(value))
        else:
            value_bytes = struct.pack("<i", int(bool(value)) if value_type == PREF_BOOL else int(value))
        output.extend(
            struct.pack("<BII", value_type, len(key_bytes), len(value_bytes))
        )
        output.extend(key_bytes)
        output.extend(value_bytes)
    if len(output) > MAX_PREF_BYTES:
        fail("merged preference store is too large")
    return bytes(output)


def parse_android_preferences(path):
    if not is_regular(path) or os.path.getsize(str(path)) > MAX_PREF_BYTES:
        fail("Android PlayerPrefs XML is missing, linked or too large")
    try:
        root = ET.fromstring(path.read_bytes())
    except (ET.ParseError, OSError) as error:
        fail("could not parse Android PlayerPrefs XML: %s" % error)
    if root.tag != "map":
        fail("Android PlayerPrefs XML root is not <map>")

    entries = []
    seen = set()
    for child in root:
        tag = child.tag.rsplit("}", 1)[-1]
        key = child.attrib.get("name")
        if not key or key in seen:
            fail("Android PlayerPrefs XML has a missing or duplicate key")
        seen.add(key)
        try:
            if tag == "string":
                value_type, value = PREF_STRING, child.text or ""
            elif tag in ("int", "long"):
                value_type = PREF_INT
                value = int(child.attrib["value"], 10)
                if value < -2147483648 or value > 2147483647:
                    fail("Android integer is outside PF2's 32-bit range")
            elif tag == "float":
                value_type, value = PREF_FLOAT, float(child.attrib["value"])
                if not math.isfinite(value):
                    fail("Android PlayerPrefs contains a non-finite float")
            elif tag == "boolean":
                raw = child.attrib["value"].strip().lower()
                if raw not in ("true", "false"):
                    fail("Android PlayerPrefs contains an invalid boolean")
                value_type, value = PREF_BOOL, raw == "true"
            else:
                fail("unsupported Android PlayerPrefs entry <%s>" % tag)
        except (KeyError, ValueError) as error:
            fail("invalid Android PlayerPrefs value for %s: %s" % (key, error))
        entries.append((key, value_type, value))
    if len(entries) > MAX_PREFS:
        fail("Android PlayerPrefs contains too many entries")
    return entries


def walk_real(root, max_depth=8):
    root = root.resolve()
    for current, directories, files in os.walk(str(root), topdown=True, followlinks=False):
        current_path = Path(current)
        depth = len(current_path.relative_to(root).parts)
        safe_directories = []
        if depth < max_depth:
            for name in sorted(directories):
                child = current_path / name
                if is_real_directory(child):
                    safe_directories.append(name)
        directories[:] = safe_directories
        yield current_path, sorted(files)


def locate_preferences(source):
    if is_regular(source):
        if source.suffix.lower() == ".xml":
            return source, "android-xml"
        return source, "pf2-bin"
    if not is_real_directory(source):
        fail("source is not a regular file or real directory")

    named = {name: [] for name in PREFERENCE_FILENAMES}
    fallback = []
    for current, files in walk_real(source):
        for name in files:
            candidate = current / name
            if not is_regular(candidate):
                continue
            if name in named:
                named[name].append(candidate)
            elif name.lower().endswith("playerprefs.xml"):
                fallback.append(candidate)
    for name in PREFERENCE_FILENAMES:
        if named[name]:
            if len(named[name]) != 1:
                fail("more than one %s was found" % name)
            return named[name][0], "android-xml"
    if len(fallback) == 1:
        return fallback[0], "android-xml"
    if len(fallback) > 1:
        fail("more than one PlayerPrefs XML was found")
    return None, None


def locate_save_directories(source):
    if not is_real_directory(source):
        return {}
    matches = {name: [] for name in SAVE_DIRECTORIES}
    for current, _files in walk_real(source):
        for name in SAVE_DIRECTORIES:
            candidate = current / name
            if is_real_directory(candidate):
                matches[name].append(candidate)
    result = {}
    for name, candidates in matches.items():
        unique = []
        seen = set()
        for candidate in candidates:
            resolved = str(candidate.resolve())
            if resolved not in seen:
                seen.add(resolved)
                unique.append(candidate)
        if len(unique) > 1:
            fail("more than one '%s' directory was found" % name)
        if unique:
            result[name] = unique[0]
    return result


def collect_save_files(save_directories):
    files = []
    total = 0
    for directory_name, source_dir in sorted(save_directories.items()):
        for current, names in walk_real(source_dir, max_depth=16):
            for name in names:
                source = current / name
                if not is_regular(source):
                    fail("save tree contains a link or special file")
                relative = source.relative_to(source_dir)
                if any(part in ("", ".", "..") for part in relative.parts):
                    fail("save tree contains an unsafe path")
                size = os.path.getsize(str(source))
                if size > MAX_SAVE_FILE_BYTES:
                    fail("save file is larger than the safety limit")
                total += size
                files.append((source, Path(directory_name) / relative))
                if len(files) > MAX_SAVE_FILES or total > MAX_SAVE_TOTAL_BYTES:
                    fail("save export exceeds the safety limit")
    return files, total


def merge_preferences(existing, imported):
    merged = list(existing)
    positions = {entry[0]: index for index, entry in enumerate(merged)}
    imported_count = 0
    skipped_count = 0
    for entry in imported:
        key = entry[0]
        if is_device_local_key(key):
            skipped_count += 1
            continue
        if key in positions:
            merged[positions[key]] = entry
        else:
            positions[key] = len(merged)
            merged.append(entry)
        imported_count += 1
    if len(merged) > MAX_PREFS:
        fail("merged preference store contains too many entries")
    return merged, imported_count, skipped_count


def ensure_directory_chain(root, directory):
    root = root.resolve()
    try:
        relative = directory.relative_to(root)
    except ValueError:
        fail("import destination escaped the home directory")
    current = root
    for part in relative.parts:
        current = current / part
        if os.path.lexists(str(current)):
            if not is_real_directory(current):
                fail("import destination contains a link or non-directory")
        else:
            os.mkdir(str(current), 0o755)


def atomic_write(root, path, payload):
    ensure_directory_chain(root, path.parent)
    temporary = path.with_name(path.name + ".importing.%d" % os.getpid())
    if os.path.lexists(str(temporary)):
        fail("temporary import path already exists")
    with open(str(temporary), "xb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(str(temporary), str(path))


def copy_atomic(root, source, target):
    ensure_directory_chain(root, target.parent)
    temporary = target.with_name(target.name + ".importing.%d" % os.getpid())
    if os.path.lexists(str(temporary)):
        fail("temporary save-import path already exists")
    with open(str(source), "rb") as input_stream, open(str(temporary), "xb") as output:
        shutil.copyfileobj(input_stream, output, 1024 * 1024)
        output.flush()
        os.fsync(output.fileno())
    os.replace(str(temporary), str(target))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, help="Android export directory, PlayerPrefs XML or PF2PREF1 file")
    parser.add_argument("--game-dir", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    source = Path(args.source).resolve()
    game_dir = Path(args.game_dir).resolve()
    if not is_real_directory(game_dir):
        fail("game directory is missing or linked")
    home = game_dir / "home"
    if os.path.commonpath((str(source), str(home))) == str(home):
        fail("source must be outside the active home directory")
    if os.path.lexists(str(home)) and not is_real_directory(home):
        fail("home is not a real directory")

    preference_source, preference_kind = locate_preferences(source)
    save_directories = locate_save_directories(source)
    save_files, save_bytes = collect_save_files(save_directories)
    if preference_source is None and not save_files:
        fail("no supported PlayerPrefs or save directories were found")

    existing_path = home / "shared-preferences.bin"
    existing = decode_pf2_preferences(existing_path) if is_regular(existing_path) else []
    imported = []
    if preference_source is not None:
        imported = (
            parse_android_preferences(preference_source)
            if preference_kind == "android-xml"
            else decode_pf2_preferences(preference_source)
        )
    merged, preference_count, skipped_count = merge_preferences(existing, imported)

    print("source preferences: %s" % (preference_source or "none"))
    print("preferences to merge: %d (device-local skipped: %d)" % (preference_count, skipped_count))
    print("save files to copy: %d (%d bytes)" % (len(save_files), save_bytes))
    print("entitlement policy: import owner data only; no premium state is fabricated")
    if args.dry_run:
        print("dry run complete; no files changed")
        return 0

    home.mkdir(parents=True, exist_ok=True)
    timestamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    backup = home / "import-backups" / (timestamp + "-%d" % os.getpid())
    affected = []
    if preference_source is not None:
        affected.append(Path("shared-preferences.bin"))
    affected.extend(relative for _source, relative in save_files)
    for relative in affected:
        target = home / relative
        if os.path.lexists(str(target)) and not is_regular(target):
            fail("an import target is linked or not a regular file: %s" % relative)
    existing_affected = [relative for relative in affected if is_regular(home / relative)]
    if existing_affected:
        ensure_directory_chain(home, backup.parent)
        os.mkdir(str(backup), 0o755)
        for relative in existing_affected:
            destination = backup / relative
            ensure_directory_chain(home, destination.parent)
            shutil.copyfile(str(home / relative), str(destination))

    if preference_source is not None:
        atomic_write(home, existing_path, encode_pf2_preferences(merged))
    for save_source, relative in save_files:
        copy_atomic(home, save_source, home / relative)

    print("import complete")
    if existing_affected:
        print("backup: %s" % backup)
    print("premium remains subject to the original game's saved entitlement checks")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ImportError, OSError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
