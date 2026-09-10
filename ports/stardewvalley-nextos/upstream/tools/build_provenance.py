#!/usr/bin/env python3
"""Record and verify reproducible loader build provenance."""

from __future__ import print_function

import argparse
import glob
import hashlib
import json
import os
import re
import subprocess
import sys


def fail(message):
    raise SystemExit("build provenance error: " + message)


def digest_file(path):
    value = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            value.update(block)
    return value.hexdigest()


def source_files(root):
    values = []
    for pattern in ("src/*.c", "src/*.h"):
        values.extend(glob.glob(os.path.join(root, pattern)))
    for relative in ("build.sh", "build_buster.sh", "buster_compat.h"):
        values.append(os.path.join(root, relative))
    values = sorted(set(values))
    for path in values:
        if not os.path.isfile(path):
            fail("missing source input " + path)
    return values


def source_digest(root):
    manifest = []
    for path in source_files(root):
        relative = os.path.relpath(path, root).replace(os.sep, "/")
        manifest.append("%s  %s\n" % (digest_file(path), relative))
    return hashlib.sha256("".join(manifest).encode("utf-8")).hexdigest()


def readelf(path, *arguments):
    try:
        return subprocess.check_output(
            [os.environ.get("READELF", "readelf")] + list(arguments) + [path],
            stderr=subprocess.STDOUT,
            universal_newlines=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        fail("readelf failed for %s: %s" % (path, error))


def elf_details(path):
    header = readelf(path, "-h")
    machine = ""
    for line in header.splitlines():
        if "Machine:" in line:
            machine = line.split("Machine:", 1)[1].strip()
            break
    if machine != "AArch64":
        fail("%s is not AArch64 (%s)" % (path, machine or "unknown"))
    versions = re.findall(r"GLIBC_([0-9]+(?:[.][0-9]+)+)", readelf(path, "--version-info"))
    newest = max(versions, key=lambda value: tuple(int(x) for x in value.split("."))) if versions else None
    return machine, newest


def glibc_release(libc):
    try:
        output = subprocess.check_output(
            ["strings", libc], universal_newlines=True, errors="replace"
        )
    except (OSError, subprocess.CalledProcessError) as error:
        fail("cannot inspect sysroot libc: %s" % error)
    match = re.search(r"GNU C Library .* stable release version ([0-9.]+)[.]", output)
    if not match:
        fail("sysroot libc release string was not found")
    return match.group(1)


def version_tuple(value):
    return tuple(int(item) for item in value.split("."))


def write_json(path, value):
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write("\n")
    os.replace(temporary, path)


def make_record(args):
    root = os.path.realpath(args.root)
    binary = os.path.realpath(args.binary)
    if not os.path.isfile(binary):
        fail("binary is missing: " + binary)
    machine, maximum = elf_details(binary)
    record = {
        "binary": os.path.basename(binary),
        "binary_bytes": os.path.getsize(binary),
        "binary_sha256": digest_file(binary),
        "compiler": args.compiler.strip(),
        "elf_machine": machine,
        "elf_required_glibc_max": maximum,
        "format": 1,
        "profile": args.profile,
        "source_digest": source_digest(root),
    }
    if args.sysroot_libc:
        libc = os.path.realpath(args.sysroot_libc)
        if not os.path.isfile(libc):
            fail("sysroot libc is missing")
        record.update(
            {
                "sysroot_glibc": glibc_release(libc),
                "sysroot_libc_sha256": digest_file(libc),
                "target": "NextOS/NextOS Elite current sysroot",
            }
        )
    else:
        if not args.glibc_ceiling:
            fail("compat records require --glibc-ceiling")
        if maximum and version_tuple(maximum) > version_tuple(args.glibc_ceiling):
            fail(
                "compat binary requires GLIBC_%s (ceiling %s)"
                % (maximum, args.glibc_ceiling)
            )
        record.update(
            {
                "builder": args.builder,
                "glibc_ceiling": args.glibc_ceiling,
                "target": "external AArch64 CFW compatibility",
            }
        )
    write_json(args.output, record)
    print("recorded %s -> %s" % (args.profile, args.output))


def verify_record(root, record_path, binary):
    with open(record_path, encoding="utf-8") as stream:
        record = json.load(stream)
    if record.get("format") != 1:
        fail("unsupported record: " + record_path)
    if record.get("source_digest") != source_digest(root):
        fail("source changed after build: " + record_path)
    if record.get("binary_sha256") != digest_file(binary):
        fail("binary changed after build: " + binary)
    if record.get("binary_bytes") != os.path.getsize(binary):
        fail("binary size changed after build: " + binary)
    _machine, maximum = elf_details(binary)
    if record.get("elf_required_glibc_max") != maximum:
        fail("ELF version record changed: " + binary)
    if record.get("profile") == "nextos-current":
        release = record.get("sysroot_glibc")
        if not isinstance(release, str) or not release:
            fail("NextOS record has no sysroot glibc release")
    elif record.get("profile") == "external-compat":
        ceiling = record.get("glibc_ceiling")
        if maximum and version_tuple(maximum) > version_tuple(ceiling):
            fail("compat GLIBC ceiling is no longer satisfied")
    else:
        fail("unknown build profile in " + record_path)
    return record


def verify(args):
    root = os.path.realpath(args.root)
    record = verify_record(root, args.record, os.path.realpath(args.binary))
    if args.output:
        write_json(args.output, record)
    print("verified %s" % record["profile"])


def combine(args):
    root = os.path.realpath(args.root)
    current = verify_record(root, args.current_record, os.path.realpath(args.current_binary))
    compat = verify_record(root, args.compat_record, os.path.realpath(args.compat_binary))
    write_json(
        args.output,
        {
            "format": 1,
            "loaders": [current, compat],
            "port": "stardewvalley-nextos",
            "release": args.release,
        },
    )
    print("combined build provenance -> " + args.output)


def main(argv=None):
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command")

    record = commands.add_parser("record")
    record.add_argument("--root", required=True)
    record.add_argument("--profile", required=True)
    record.add_argument("--binary", required=True)
    record.add_argument("--compiler", required=True)
    record.add_argument("--output", required=True)
    record.add_argument("--sysroot-libc")
    record.add_argument("--glibc-ceiling")
    record.add_argument("--builder", default="unspecified")

    check = commands.add_parser("verify")
    check.add_argument("--root", required=True)
    check.add_argument("--record", required=True)
    check.add_argument("--binary", required=True)
    check.add_argument("--output")

    merged = commands.add_parser("combine")
    merged.add_argument("--root", required=True)
    merged.add_argument("--current-record", required=True)
    merged.add_argument("--current-binary", required=True)
    merged.add_argument("--compat-record", required=True)
    merged.add_argument("--compat-binary", required=True)
    merged.add_argument("--release", required=True)
    merged.add_argument("--output", required=True)

    args = parser.parse_args(argv)
    if args.command == "record":
        make_record(args)
    elif args.command == "verify":
        verify(args)
    elif args.command == "combine":
        combine(args)
    else:
        parser.error("a command is required")
    return 0


if __name__ == "__main__":
    sys.exit(main())
