#!/usr/bin/env python3
"""Validate a compatible Summertime Saga APK payload and prepare its modules."""

import ast
import gzip
import hashlib
import json
import os
import re
import shutil
import stat
import sys
import tarfile


PACKAGE = "com.kompasproductions.summertimesaga"
MIN_NUMERIC_VERSION = 7723
SUPPORTED_RENPY_LINE = (8, 5)
MIN_RENPY_PATCH = 3
INDEX_EXTENSIONS = (
    ".rpy",
    ".rpyc",
    ".rpym",
    ".rpymc",
    ".rpyb",
    ".rpa",
    ".rpi",
    ".png",
    ".jpg",
    ".jpeg",
    ".webp",
    ".sdf",
    ".ogg",
    ".opus",
    ".mp3",
    ".wav",
    ".flac",
    ".mkv",
    ".webm",
    ".mp4",
    ".ttf",
    ".otf",
)
PRIVATE_RUNTIME_MAX_FILES = 20000
PRIVATE_RUNTIME_MAX_BYTES = 512 * 1024 * 1024
PRIVATE_RUNTIME_MIN_FILES = 500
PRIVATE_RUNTIME_MIN_BYTES = 5 * 1024 * 1024
PRIVATE_RUNTIME_REQUIRED = (
    "__future__.pyc",
    "encodings/__init__.pyc",
    "os.pyc",
    "site.pyc",
    "sitecustomize.pyc",
)


def fail(message):
    raise SystemExit("Summertime Saga data preparation failed: " + message)


def regular_file(path):
    try:
        mode = os.lstat(path).st_mode
    except OSError:
        return False
    return stat.S_ISREG(mode)


def contained(root, path):
    root = os.path.realpath(root)
    path = os.path.realpath(path)
    return path == root or path.startswith(root + os.sep)


def read_json(path):
    if not regular_file(path):
        fail("missing x-android.json")
    try:
        with open(path, "r", encoding="utf-8") as stream:
            value = json.load(stream)
    except (OSError, ValueError) as error:
        fail("invalid x-android.json: %s" % error)
    if not isinstance(value, dict):
        fail("x-android.json is not an object")
    return value


def read_engine_version(path):
    if not regular_file(path):
        fail("missing x-script_version.txt")
    try:
        with open(path, "r", encoding="ascii") as stream:
            value = ast.literal_eval(stream.read().strip())
    except (OSError, ValueError, SyntaxError) as error:
        fail("invalid Ren'Py version marker: %s" % error)
    if (
        not isinstance(value, tuple)
        or len(value) < 3
        or any(not isinstance(item, int) for item in value[:3])
    ):
        fail("unsupported Ren'Py version marker")
    return tuple(value[:3])


def validate_identity(assets):
    metadata = read_json(os.path.join(assets, "x-android.json"))
    if metadata.get("package") != PACKAGE:
        fail("Android package is not %s" % PACKAGE)

    version = metadata.get("version")
    numeric = metadata.get("numeric_version")
    if not isinstance(version, str) or not re.match(r"^[0-9]+(?:[.][0-9]+)+", version):
        fail("game version is missing or malformed")
    if not isinstance(numeric, int) or numeric < MIN_NUMERIC_VERSION:
        fail("game build is older than the supported preview")

    game_major = int(version.split(".", 1)[0])
    if game_major < 21:
        fail("legacy 0.20.x APKs are not compatible with this port")

    engine = read_engine_version(
        os.path.join(assets, "x-game", "x-script_version.txt")
    )
    if engine[:2] != SUPPORTED_RENPY_LINE or engine[2] < MIN_RENPY_PATCH:
        fail(
            "Ren'Py %d.%d.%d is outside the supported 8.5.x line"
            % engine
        )
    return metadata, engine


def strip_x_component(value):
    if not value.startswith("x-") or len(value) <= 2:
        fail("unexpected game-module path component: " + value)
    return value[2:]


def unpack_private_library(private_archive, destination):
    """Safely unpack only lib/ from Ren'Py's private.mp3 tar.gz."""
    if not regular_file(private_archive):
        fail("missing private.mp3 runtime archive")
    if os.path.lexists(destination):
        if os.path.islink(destination) or not os.path.isdir(destination):
            fail("staged private runtime destination is unsafe")
        shutil.rmtree(destination)
    os.makedirs(destination)

    files = 0
    total = 0
    seen = set()
    try:
        with gzip.open(private_archive, "rb") as compressed:
            with tarfile.open(fileobj=compressed, mode="r|") as archive:
                for member in archive:
                    name = member.name.rstrip("/")
                    if name == "lib":
                        if not member.isdir():
                            fail("private runtime lib entry is not a directory")
                        continue
                    if not name.startswith("lib/"):
                        continue
                    if "\\" in name or "\x00" in name:
                        fail("private runtime contains an unsafe path")
                    components = name.split("/")
                    if (
                        len(components) < 2
                        or components[0] != "lib"
                        or any(item in ("", ".", "..") for item in components)
                    ):
                        fail("private runtime contains an unsafe path")

                    relative = os.path.join(*components[1:])
                    if relative in seen:
                        fail("private runtime contains a duplicate path")
                    seen.add(relative)
                    target = os.path.join(destination, relative)
                    if not contained(destination, target):
                        fail("private runtime path escapes its destination")

                    if member.isdir():
                        os.makedirs(target, exist_ok=True)
                        continue
                    if not member.isfile():
                        fail("private runtime contains a linked or special entry")

                    files += 1
                    total += member.size
                    if files > PRIVATE_RUNTIME_MAX_FILES:
                        fail("private runtime contains too many files")
                    if total > PRIVATE_RUNTIME_MAX_BYTES:
                        fail("private runtime is unexpectedly large")

                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    source = archive.extractfile(member)
                    if source is None:
                        fail("private runtime member cannot be read")
                    temporary = target + ".nxpart"
                    written = 0
                    try:
                        with source, open(temporary, "xb") as output:
                            while True:
                                block = source.read(1024 * 1024)
                                if not block:
                                    break
                                output.write(block)
                                written += len(block)
                            output.flush()
                            os.fsync(output.fileno())
                        if written != member.size:
                            fail("private runtime member was truncated")
                        os.chmod(temporary, 0o644)
                        os.replace(temporary, target)
                    finally:
                        try:
                            os.unlink(temporary)
                        except FileNotFoundError:
                            pass

                    if files % 100 == 0:
                        print(
                            "NXEXTRACT_PROGRESS %d 1000 "
                            "PREPARING PYTHON RUNTIME %d"
                            % (min(200, files // 5), files),
                            flush=True,
                        )
    except (OSError, tarfile.TarError, EOFError) as error:
        fail("invalid private.mp3 runtime archive: %s" % error)
    return files, total


def prepare_private_runtime(stage, assets):
    destination = os.path.join(stage, "lib")
    if not contained(stage, destination):
        fail("unsafe private runtime destination")
    files, total = unpack_private_library(
        os.path.join(assets, "private.mp3"),
        destination,
    )
    if files < PRIVATE_RUNTIME_MIN_FILES or total < PRIVATE_RUNTIME_MIN_BYTES:
        fail("private Python runtime is unexpectedly small")

    python_roots = sorted(
        name
        for name in os.listdir(destination)
        if re.match(r"^python[0-9]+[.][0-9]+$", name)
        and os.path.isdir(os.path.join(destination, name))
        and not os.path.islink(os.path.join(destination, name))
    )
    if python_roots != ["python3.12"]:
        fail(
            "private Python runtime is not the supported Python 3.12 layout"
        )
    python_root = os.path.join(destination, python_roots[0])
    for relative in PRIVATE_RUNTIME_REQUIRED:
        if not regular_file(os.path.join(python_root, relative)):
            fail("private Python runtime is missing " + relative)
    return files, total, python_roots[0]


def rebuild_saga(stage, game_dir, assets):
    source = os.path.join(assets, "x-game", "x-saga")
    destination = os.path.join(stage, "saga")
    if not os.path.isdir(source) or os.path.islink(source):
        fail("missing or linked x-saga module tree")
    if not contained(stage, destination):
        fail("unsafe saga destination")
    if os.path.lexists(destination):
        if os.path.islink(destination):
            fail("staged saga destination is linked")
        shutil.rmtree(destination)
    os.makedirs(destination)

    copied = 0
    for current, directories, files in os.walk(source, topdown=True):
        directories.sort()
        files.sort()
        if os.path.islink(current):
            fail("x-saga contains a linked directory")
        relative = os.path.relpath(current, source)
        components = [] if relative == "." else relative.split(os.sep)
        mapped_components = [strip_x_component(item) for item in components]
        mapped_dir = os.path.join(destination, *mapped_components)
        os.makedirs(mapped_dir, exist_ok=True)
        for name in files:
            original = os.path.join(current, name)
            if not regular_file(original):
                fail("x-saga contains a non-regular file")
            mapped = strip_x_component(name)
            shutil.copyfile(original, os.path.join(mapped_dir, mapped))
            copied += 1
            if copied % 50 == 0:
                print(
                    "NXEXTRACT_PROGRESS %d 500 PREPARING GAME MODULES %d"
                    % (min(copied, 500), copied),
                    flush=True,
                )

    overlay = os.path.join(game_dir, "runtime-overrides", "saga")
    allowed = {
        os.path.join("init", "logic.py"),
        os.path.join("init", "sound.py"),
    }
    for relative in sorted(allowed):
        original = os.path.join(overlay, relative)
        if not regular_file(original):
            fail("missing runtime override " + relative)
        target = os.path.join(destination, relative)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(original, target)

    if copied < 300:
        fail("x-saga tree is unexpectedly small")
    return copied + len(allowed)


def rebuild_index(assets):
    game_root = os.path.join(assets, "x-game")
    index_path = os.path.join(game_root, ".summertime-index.txt")
    entries = []
    for current, directories, files in os.walk(game_root, topdown=True):
        directories[:] = sorted(
            item
            for item in directories
            if not item.startswith(".")
            and not os.path.islink(os.path.join(current, item))
        )
        for name in sorted(files):
            path = os.path.join(current, name)
            if path == index_path or name.startswith("."):
                continue
            if not regular_file(path):
                fail("x-game contains a non-regular file")
            if name.lower().endswith(INDEX_EXTENSIONS):
                entries.append(os.path.relpath(path, game_root).replace(os.sep, "/"))

    entries.sort()
    temporary = index_path + ".nxpart"
    with open(temporary, "w", encoding="utf-8", newline="\n") as stream:
        for entry in entries:
            stream.write(entry + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, index_path)
    if len(entries) < 20000:
        fail("x-game index is unexpectedly small")
    print(
        "NXEXTRACT_PROGRESS 900 1000 INDEXED %d GAME FILES" % len(entries),
        flush=True,
    )
    return len(entries)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def write_runtime_info(
    stage,
    metadata,
    engine,
    saga_files,
    index_files,
    runtime_files,
    runtime_bytes,
    python_runtime,
):
    library = os.path.join(stage, "librenpython.so")
    if not regular_file(library):
        fail("missing librenpython.so")
    result = {
        "schema": 1,
        "package": PACKAGE,
        "game_version": metadata["version"],
        "numeric_version": metadata["numeric_version"],
        "renpy_version": list(engine),
        "abi": os.environ.get("NXEXTRACT_ABI", "arm64-v8a"),
        "librenpython_sha256": sha256_file(library),
        "python_runtime": python_runtime,
        "python_runtime_bytes": runtime_bytes,
        "python_runtime_files": runtime_files,
        "saga_files": saga_files,
        "indexed_game_files": index_files,
    }
    path = os.path.join(stage, ".summertime-data.json")
    temporary = path + ".nxpart"
    with open(temporary, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def main():
    stage = os.environ.get("NXEXTRACT_STAGE")
    game_dir = os.environ.get("NXEXTRACT_GAME_DIR")
    if not stage or not game_dir:
        fail("NXExtract environment is incomplete")
    stage = os.path.realpath(stage)
    game_dir = os.path.realpath(game_dir)
    assets = os.path.join(stage, "assets")
    if not contained(stage, assets):
        fail("unsafe assets path")

    metadata, engine = validate_identity(assets)
    runtime_files, runtime_bytes, python_runtime = prepare_private_runtime(
        stage,
        assets,
    )
    saga_files = rebuild_saga(stage, game_dir, assets)
    index_files = rebuild_index(assets)
    write_runtime_info(
        stage,
        metadata,
        engine,
        saga_files,
        index_files,
        runtime_files,
        runtime_bytes,
        python_runtime,
    )
    print(
        "NXEXTRACT_PROGRESS 1000 1000 SUMMERTIME SAGA %s READY"
        % metadata["version"],
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
