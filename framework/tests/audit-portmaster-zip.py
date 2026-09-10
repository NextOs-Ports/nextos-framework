#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Shell/ELF preflight plus pinned real HarbourMaster lifecycle for a ZIP."""

import argparse
import json
import os
import re
import shutil
import shlex
import stat
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath


REPOSITORY = Path(__file__).resolve().parents[2]
NXABI = REPOSITORY / "framework/nxabi/nxabi.py"
HARBOURMASTER_CYCLE = (
    REPOSITORY / "framework/portmaster/tools/harbourmaster-cycle.py"
)
MAX_MEMBERS = 8192
MAX_MEMBER_BYTES = 512 * 1024 * 1024
MAX_TOTAL_BYTES = 4 * 1024 * 1024 * 1024
SHELL_INTERPRETER_NAMES = frozenset((
    "sh", "bash", "dash", "ash", "ksh", "mksh", "zsh", "hush",
))
PUBLIC_TEXT_SUFFIXES = frozenset((".md", ".txt", ".json"))
FORBIDDEN_OWNER_SOURCE = re.compile(
    r"(?:apk[\s._+-]*(?:pure|mirror|vision|combo)|"
    r"5[\s._+-]*play|uptodown|"
    r"[a-z0-9+_.-]*(?:mod|hack)[a-z0-9+_.-]*\."
    r"(?:apk|apkm|apks|xapk))",
    re.IGNORECASE,
)


class AuditError(Exception):
    """Unsafe archive shape or failed package contract."""


def require(condition, message):
    if not condition:
        raise AuditError(message)


def has_shell_shebang(path):
    try:
        with path.open("rb") as stream:
            first = stream.readline(256)
    except OSError:
        return False
    try:
        first_line = first.decode("utf-8").rstrip("\r\n")
    except UnicodeDecodeError:
        return False
    # Use the same parser as syntax selection so extensionless ash, mksh,
    # hush and ``env busybox sh`` helpers cannot escape the ZIP audit.
    return shell_shebang_interpreter(first_line) is not None


def is_shell_path(path, extracted_path):
    name = PurePosixPath(path).name
    return name.endswith(".sh") or name in {
        "nxbootstrap", "run-extractor", "nxextract-runtime-env"
    } or has_shell_shebang(extracted_path)


def safe_member(info, seen):
    name = info.filename
    require("\\" not in name and "\0" not in name,
            "archive member has an unsafe separator or NUL")
    path = PurePosixPath(name)
    require(name and not path.is_absolute() and
            all(part not in ("", ".", "..") for part in path.parts),
            "archive member is not a normalized relative path: %s" % name)
    key = name.casefold()
    require(key not in seen, "archive has a duplicate/case collision: %s" % name)
    seen.add(key)
    mode_type = (info.external_attr >> 16) & 0o170000
    require(mode_type not in (stat.S_IFLNK, stat.S_IFCHR, stat.S_IFBLK,
                              stat.S_IFIFO, stat.S_IFSOCK),
            "archive member is not a regular file/directory: %s" % name)
    require(info.file_size <= MAX_MEMBER_BYTES,
            "archive member exceeds the size limit: %s" % name)
    require(FORBIDDEN_OWNER_SOURCE.search(name) is None,
            "archive member reveals an owner-data source: %s" % name)


def audit_public_text(path, logical_path):
    if PurePosixPath(logical_path).suffix.casefold() not in PUBLIC_TEXT_SUFFIXES:
        return False
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise AuditError(
            "public text file is not UTF-8: %s" % logical_path
        ) from error
    require(FORBIDDEN_OWNER_SOURCE.search(text) is None,
            "public text reveals an owner-data source: %s" % logical_path)
    return True


def active_shell_text(text):
    return "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#")
    )


def shell_shebang_interpreter(first_line):
    if not first_line.startswith("#!"):
        return None
    try:
        words = shlex.split(first_line[2:], posix=True)
    except ValueError:
        return None
    if not words:
        return None
    command = PurePosixPath(words.pop(0)).name
    if command == "env":
        while words and (words[0].startswith("-") or
                         re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=.*", words[0])):
            words.pop(0)
        if not words:
            return None
        command = PurePosixPath(words.pop(0)).name
    if command == "busybox":
        if not words:
            return None
        command = PurePosixPath(words.pop(0)).name
    if command not in SHELL_INTERPRETER_NAMES:
        return None
    return "bash" if command == "bash" else "sh"


def shell_command_tokens(text, logical_path):
    try:
        lexer = shlex.shlex(
            text, posix=True, punctuation_chars=";&|()<>\n"
        )
        lexer.whitespace = " \t\r"
        lexer.whitespace_split = True
        lexer.commenters = "#"
        raw = list(lexer)
    except ValueError as error:
        raise AuditError(
            "cannot lex shell file: %s" % logical_path
        ) from error
    tokens = []
    for token in raw:
        if token and all(character in ";&|()<>\n" for character in token):
            tokens.extend(token)
        else:
            tokens.append(token)
    return tokens


def shell_invokes_external_stat(text, logical_path, _depth=0):
    tokens = shell_command_tokens(text, logical_path)
    boundaries = frozenset((";", "&", "|", "(", ")", "\n"))
    command_prefixes = frozenset((
        "if", "then", "elif", "else", "while", "until", "do", "!", "{",
        "time",
    ))
    assignment = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=.*$")

    def basename(value):
        return value.rsplit("/", 1)[-1]

    def segment_contains_stat(start):
        while start < len(tokens) and tokens[start] not in boundaries:
            if basename(tokens[start]) == "stat":
                return True
            start += 1
        return False

    expecting = True
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in boundaries:
            expecting = True
            index += 1
            continue
        if not expecting:
            index += 1
            continue
        if token in command_prefixes or assignment.fullmatch(token):
            index += 1
            continue
        if token in ("<", ">"):
            index += 2
            continue

        wrapper = basename(token)
        if wrapper == "command":
            index += 1
            query_only = False
            while index < len(tokens) and tokens[index].startswith("-"):
                if tokens[index] in ("-v", "-V"):
                    query_only = True
                index += 1
            if query_only:
                expecting = False
                continue
            if index >= len(tokens):
                return False
            wrapper = basename(tokens[index])
        elif wrapper == "builtin":
            index += 1
            while index < len(tokens) and tokens[index].startswith("-"):
                index += 1
            if index >= len(tokens):
                return False
            wrapper = basename(tokens[index])
        elif wrapper == "env":
            index += 1
            while index < len(tokens):
                candidate = tokens[index]
                if (candidate.startswith("-") or
                        assignment.fullmatch(candidate)):
                    index += 1
                    continue
                break
            if index >= len(tokens):
                return False
            wrapper = basename(tokens[index])
        elif wrapper == "busybox":
            index += 1
            if index >= len(tokens):
                return False
            wrapper = basename(tokens[index])

        if wrapper in {
                "exec", "nohup", "sudo", "nice", "ionice", "timeout",
                "xargs", "setsid", "stdbuf", "chroot"}:
            if segment_contains_stat(index + 1):
                return True
        if wrapper == "find":
            cursor = index + 1
            while cursor + 1 < len(tokens) and tokens[cursor] not in boundaries:
                if (tokens[cursor] in ("-exec", "-execdir") and
                        basename(tokens[cursor + 1]) == "stat"):
                    return True
                cursor += 1
        if (wrapper in SHELL_INTERPRETER_NAMES and _depth < 8):
            cursor = index + 1
            while cursor + 1 < len(tokens) and tokens[cursor] not in boundaries:
                if tokens[cursor] == "-c" and shell_invokes_external_stat(
                        tokens[cursor + 1], logical_path, _depth + 1):
                    return True
                cursor += 1

        if wrapper == "stat":
            if index + 1 < len(tokens) and tokens[index + 1] == "(":
                expecting = False
                index += 1
                continue
            return True
        expecting = False
        index += 1
    return False


def shell_name(text, logical_path):
    first = text.splitlines()[0] if text.splitlines() else ""
    shell = shell_shebang_interpreter(first)
    if shell is not None:
        return shell
    require(not first.startswith("#!"),
            "unsupported shell interpreter in %s" % logical_path)
    require(PurePosixPath(logical_path).name.endswith(".sh"),
            "extensionless shell file lacks a shebang: %s" % logical_path)
    return "sh"


def audit_shell(path, logical_path):
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise AuditError("shell file is not UTF-8: %s" % logical_path) from error
    shell = shell_name(text, logical_path)
    result = subprocess.run(
        [shell, "-n", str(path)], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        check=False,
    )
    require(result.returncode == 0,
            "shell syntax error in %s: %s" %
            (logical_path, result.stderr.strip()))
    require(not shell_invokes_external_stat(text, logical_path),
            "shell calls the external stat command: %s" % logical_path)
    return shell


def audit_archive(archive_path):
    require(archive_path.is_file() and not archive_path.is_symlink(),
            "archive must be a regular non-symlink file")
    require(shutil.which("readelf") is not None,
            "GNU readelf is required for ELF audit")
    with zipfile.ZipFile(str(archive_path), "r") as archive:
        infos = archive.infolist()
        require(0 < len(infos) <= MAX_MEMBERS,
                "archive member count is empty or excessive")
        require(sum(info.file_size for info in infos) <= MAX_TOTAL_BYTES,
                "archive uncompressed size exceeds the limit")
        seen = set()
        for info in infos:
            safe_member(info, seen)
            require(PurePosixPath(info.filename).name != "nxbootstrap.sh",
                    "archive contains retired legacy nxbootstrap.sh")

        with tempfile.TemporaryDirectory(prefix="firmware-zip-audit.") as root:
            stage = Path(root)
            shell_files = []
            top_level_shells = []
            elf_files = []
            public_text_files = 0
            for info in infos:
                if info.is_dir():
                    continue
                logical = PurePosixPath(info.filename)
                target = stage.joinpath(*logical.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info, "r") as source, \
                        target.open("xb") as destination:
                    shutil.copyfileobj(source, destination, 1024 * 1024)
                public_text_files += int(
                    audit_public_text(target, info.filename)
                )
                if is_shell_path(info.filename, target):
                    shell_files.append((target, info.filename))
                    if len(logical.parts) == 1 and logical.name.endswith(".sh"):
                        top_level_shells.append(info.filename)
                with target.open("rb") as stream:
                    if stream.read(4) == b"\x7fELF":
                        elf_files.append(target)

            require(len(top_level_shells) == 1,
                    "archive must have exactly one top-level launcher .sh")
            require(shell_files, "archive contains no auditable shell files")
            shells = {"bash": 0, "sh": 0}
            for path, logical_path in shell_files:
                shells[audit_shell(path, logical_path)] += 1

            if elf_files:
                report = stage / "nxabi-report.json"
                command = [
                    sys.executable, "-B", str(NXABI), "audit",
                    *map(str, elf_files), "--json", str(report), "--quiet",
                ]
                result = subprocess.run(
                    command, stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                    check=False,
                )
                require(result.returncode == 0,
                        "nxabi rejected packaged ELF(s): %s" %
                        (result.stderr or result.stdout).strip())
                with report.open("r", encoding="utf-8") as stream:
                    nxabi = json.load(stream)
                require(nxabi.get("counts", {}).get("elves") == len(elf_files),
                        "nxabi ELF inventory is incomplete")

    print("PortMaster ZIP contract audit passed: launcher=1 shells=%d "
          "elfs=%d public_texts=%d result=profile-contract-pass hardware_ran=0 "
          "device_access=0 firmware_images_used=0" %
          (len(shell_files), len(elf_files), public_text_files))


def audit_harbourmaster_cycle(archive_path, previous_path=None):
    require(HARBOURMASTER_CYCLE.is_file() and not HARBOURMASTER_CYCLE.is_symlink(),
            "pinned HarbourMaster cycle gate is missing or linked")
    command = [sys.executable, "-B", str(HARBOURMASTER_CYCLE), str(archive_path)]
    if previous_path is not None:
        require(previous_path.is_file() and not previous_path.is_symlink(),
                "previous archive must be a regular non-symlink file")
        command.extend(["--previous", str(previous_path)])
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    process = subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
        check=False,
    )
    require(process.returncode == 0,
            "real HarbourMaster lifecycle rejected the ZIP: %s" %
            (process.stderr or process.stdout).strip())
    summary = [line for line in process.stdout.splitlines()
               if line.startswith("HarbourMaster real cycle passed:")]
    require(len(summary) == 1,
            "real HarbourMaster lifecycle produced no unique terminal result")
    print(summary[0])


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="PortMaster ZIP static audit and real offline lifecycle"
    )
    parser.add_argument("archive", type=Path)
    parser.add_argument("--previous", type=Path,
                        help="previous package to exercise an overlay update")
    arguments = parser.parse_args(argv)
    try:
        archive = arguments.archive.resolve()
        previous = (arguments.previous.resolve()
                    if arguments.previous is not None else None)
        audit_archive(archive)
        audit_harbourmaster_cycle(archive, previous)
    except (AuditError, OSError, ValueError, zipfile.BadZipFile) as error:
        print("PortMaster ZIP contract audit failed: %s" % error,
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
