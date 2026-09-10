#!/usr/bin/env python3
"""Build and verify deterministic universal PortMaster release archives.

This is deliberately a host-side tool.  It never executes a port or game data.
Python 3.8+ and GNU readelf are the only non-stdlib runtime requirements.
"""

from __future__ import print_function

import argparse
import ctypes
import errno
import hashlib
import json
import os
import re
import shutil
import shlex
import stat
import subprocess
import sys
import tempfile
import time
import unicodedata
import uuid
import zipfile
from datetime import datetime, timezone
import xml.etree.ElementTree as ElementTree
from pathlib import Path, PurePosixPath


TOOL_VERSION = "0.2.5"
SCHEMA_VERSION = 2
PUBLIC_GLIBC_MAX = "2.30"
PUBLIC_GLIBCXX_MAX = "3.4.25"
PUBLIC_CXXABI_MAX = "1.3.11"
ABI_POLICY_PATH = (
    Path(__file__).resolve().parents[1] / "nxabi" / "policy-v1.json"
)
NXEXTRACT_FLOOR = "1.2.2"
METADATA_BASENAME = "NXRELEASE-METADATA.json"
CHECKSUM_MANIFEST_BASENAME = "MANIFEST.sha256"
SBOM_BASENAME = "SBOM.cdx.json"
INTERNAL_DIRNAME = ".nxrelease"
PROFILE = "universal-portmaster"
LOW_GLIBC_PROFILE = "universal-low-glibc"
ALLOWED_KINDS = frozenset((
    "launcher",
    "script",
    "payload",
    "project-linux",
    "third-party-linux",
    "nxextract",
    "nxextract-runner",
    "nxextract-recipe",
    "nxextract-runtime-env",
    "nxbootstrap-config",
    "portmaster-metadata",
    "portmaster-image",
    "license-notice",
))
ELF_KINDS = frozenset(("project-linux", "third-party-linux"))
LINUX_ELF_KINDS = frozenset(("project-linux", "third-party-linux"))
SINGLE_FILE_KINDS = frozenset((
    "launcher", "script", "project-linux", "third-party-linux",
    "nxextract", "nxextract-runner",
    "nxextract-recipe", "nxextract-runtime-env", "nxbootstrap-config",
    "portmaster-metadata", "portmaster-image", "license-notice",
))
ARCH_MACHINES = {
    "aarch64": "AArch64",
    "armv7": "ARM",
}
ARCH_CLASSES = {
    "aarch64": "ELF64",
    "armv7": "ELF32",
}
LINUX_INTERPRETERS = {
    "aarch64": "/lib/ld-linux-aarch64.so.1",
    "armv7": "/lib/ld-linux-armhf.so.3",
}
DEPENDENCY_NAMESPACES = frozenset(("linux", "android"))
DEPENDENCY_PROVIDERS = frozenset((
    "package", "glibc-base", "firmware", "portmaster",
    "nxloader-import-registry",
))
GLIBC_BASE_SONAMES = frozenset((
    "libanl.so.1", "libBrokenLocale.so.1", "libc.so.6", "libdl.so.2",
    "libm.so.6", "libnsl.so.1", "libpthread.so.0", "libresolv.so.2",
    "librt.so.1", "libutil.so.1",
))
BIONIC_SONAMES = frozenset((
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so",
    "libjnigraphics.so", "libOpenSLES.so", "libaaudio.so",
))
EXCEPTION_RULES = frozenset(("adaptive-driver", "supervised-child"))
NXPORT_SCHEMA_VERSION = 2
NXPORT_CAPABILITY_RE = re.compile(
    r"^(?:host|graphics|audio|input)\.[a-z0-9][a-z0-9.-]{0,62}$"
)
NXPORT_QUIRK_RE = re.compile(
    r"^(?:adapter|engine|game)\.[a-z0-9][a-z0-9._-]{0,62}$"
)
CAPABILITY_REGISTRY_PATH = (
    Path(__file__).resolve().parents[1] / "nxcompat" /
    "capabilities-v1.json"
)
QUIRK_REGISTRY_PATH = (
    Path(__file__).resolve().parents[1] / "nxcompat" /
    "quirk-registry-v1.json"
)


def load_quirk_registry():
    try:
        data = json.loads(QUIRK_REGISTRY_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError("cannot load quirk registry: %s" % error)
    if not isinstance(data, dict):
        raise RuntimeError("invalid quirk registry root")
    entries = data.get("quirks")
    if (data.get("schema_version") != 1 or
            data.get("default_enabled") is not False or
            not isinstance(entries, list)):
        raise RuntimeError("invalid quirk registry header")
    identifiers = []
    for entry in entries:
        identifier = entry.get("id") if isinstance(entry, dict) else None
        if (not isinstance(identifier, str) or
                not NXPORT_QUIRK_RE.fullmatch(identifier)):
            raise RuntimeError("invalid quirk registry identifier")
        identifiers.append(identifier)
    if len(identifiers) != len(set(identifiers)):
        raise RuntimeError("duplicate quirk registry identifier")
    return tuple(identifiers)


def load_capability_registry():
    try:
        with CAPABILITY_REGISTRY_PATH.open("r", encoding="utf-8") as stream:
            data = json.load(stream)
    except (OSError, ValueError) as error:
        raise RuntimeError("cannot load capability registry: {}".format(error))
    if not isinstance(data, dict):
        raise RuntimeError("invalid capability registry root")
    entries = data.get("capabilities")
    if (data.get("schema_version") != 1 or
            data.get("default_required") is not False or
            not isinstance(entries, list)):
        raise RuntimeError("invalid capability registry header")
    identifiers = []
    for entry in entries:
        identifier = entry.get("id") if isinstance(entry, dict) else None
        if (not isinstance(identifier, str) or
                not NXPORT_CAPABILITY_RE.fullmatch(identifier)):
            raise RuntimeError("invalid capability registry identifier")
        identifiers.append(identifier)
    if len(identifiers) != len(set(identifiers)):
        raise RuntimeError("duplicate capability registry identifier")
    return tuple(identifiers)


CAPABILITY_REGISTRY_ERROR = None
try:
    NXPORT_CAPABILITY_IDENTIFIERS = load_capability_registry()
    NXPORT_QUIRK_IDENTIFIERS = load_quirk_registry()
except RuntimeError as error:
    NXPORT_CAPABILITY_IDENTIFIERS = ()
    NXPORT_QUIRK_IDENTIFIERS = ()
    CAPABILITY_REGISTRY_ERROR = str(error)
NXPORT_CAPABILITY_IDS = frozenset(NXPORT_CAPABILITY_IDENTIFIERS)
NXPORT_QUIRK_IDS = frozenset(NXPORT_QUIRK_IDENTIFIERS)
NXPORT_CAPABILITY_ORDER = {
    identifier: index
    for index, identifier in enumerate(NXPORT_CAPABILITY_IDENTIFIERS)
}
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
VERSION_RE = re.compile(r"^[0-9]+(?:\.[0-9]+)+$")
PACKAGE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
SONAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+~-]{0,126}$")
NX_VERSION_RE = re.compile(
    r"^\s*NXEXTRACT_VERSION\s*=\s*['\"]([^'\"]+)['\"]\s*$", re.MULTILINE
)
GLIBC_TOKEN_RE = re.compile(r"\bGLIBC_(?:[0-9]+(?:\.[0-9]+)+|PRIVATE|ABI_[A-Za-z0-9_]+)\b")
GLIBCXX_TOKEN_RE = re.compile(r"\bGLIBCXX_(?:[0-9]+(?:\.[0-9]+)+)\b")
CXXABI_TOKEN_RE = re.compile(r"\bCXXABI_(?:[0-9]+(?:\.[0-9]+)+)\b")
PRIVATE_PATH_RE = re.compile(rb"/(?:home|Users)/[A-Za-z0-9._-]+")
IPV4_RE = re.compile(
    rb"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])"
)
SECRET_LITERAL_RE = re.compile(
    rb"(?i)(?:api[_-]?key|secret|passwd|password|bearer|credential|private[_-]?key)"
    rb"\s*[:=]\s*[A-Za-z0-9/_+\-]{8,}"
)
HOST_LITERAL_RE = re.compile(
    rb"(?i)\b(?:host|hostname)\s*[:=]\s*[A-Za-z0-9][A-Za-z0-9._-]{1,127}\b"
)
ADVOCACY_RE = re.compile(
    rb"(?i)\b(?:donate|donations?|sponsor(?:ing|ship|s)?|patreon|paypal|"
    rb"ko-?fi|opencollective|liberapay|buymeacoffee|buy-me-a-coffee|funding)\b"
)
FORCED_DRIVER_RE = re.compile(
    r"^[ \t]*(?!#)(?:export[ \t]+)?(?:SDL_(?:VIDEO|AUDIO)DRIVER|ALSOFT_DRIVERS)=",
    re.MULTILINE | re.IGNORECASE,
)
DETACHED_RE = re.compile(r"(^|[; \t])(setsid|nohup)(?=[ \t]|$)")
FRONTEND_RE = re.compile(
    r"(^|[; \t])(systemctl|killall|pkill)(?=[ \t]).*(?:emustation|emulationstation)",
    re.IGNORECASE,
)
FORBIDDEN_SUFFIXES = (
    ".apk", ".apkm", ".apks", ".xapk", ".obb", ".jar", ".dex", ".pdb", ".log", ".pyc",
)
FORBIDDEN_BASENAMES = frozenset((
    "funding.yml", "funding.yaml", "funding", ".env", "core",
))
FORBIDDEN_PATH_PARTS = frozenset((
    "saves", "tmp", "temp", "cache", "__pycache__", ".config",
))


class ReleaseError(Exception):
    """A user-facing validation failure."""


def fail(message):
    raise ReleaseError(message)


def require_keys(value, allowed, context):
    unknown = sorted(set(value) - set(allowed))
    if unknown:
        fail("{} has unknown field(s): {}".format(context, ", ".join(unknown)))


def require_object(value, context):
    if not isinstance(value, dict):
        fail("{} must be a JSON object".format(context))
    return value


def require_string(value, context, allow_empty=False):
    if not isinstance(value, str) or (not allow_empty and not value.strip()):
        fail("{} must be a non-empty string".format(context))
    if any(ord(char) < 32 for char in value):
        fail("{} contains a control character".format(context))
    return value


def version_tuple(value, context):
    value = require_string(value, context)
    if not VERSION_RE.match(value):
        fail("{} is not a dotted numeric version: {}".format(context, value))
    return tuple(int(part) for part in value.split("."))


def version_gt(left, right):
    return version_tuple(left, "version") > version_tuple(right, "version")


def version_lt(left, right):
    return version_tuple(left, "version") < version_tuple(right, "version")


def nxbootstrap_script_name(version):
    """Return the deployment-safe bootstrap selected by a generated wrapper."""
    parsed = version_tuple(version, "nxbootstrap version")
    if parsed < (0, 5, 1):
        fail("nxbootstrap versions before 0.5.1 lack the complete deployment receipt")
    if parsed >= (0, 6, 0):
        fail("nxbootstrap 0.6.0+ launchers are self-contained and have no library")
    return "nxbootstrap-{}.sh".format(version)


def bootstrap_self_contained(version):
    """nxbootstrap 0.6.0+ emits one self-contained launcher (Limbo shape)."""
    return version_tuple(version, "nxbootstrap version") >= (0, 6, 0)


def nxbootstrap_deployment_id(port_id, launcher, version, bootstrap_sha256,
                              nxport_sha256):
    material = {
        "bootstrap_filename": nxbootstrap_script_name(version),
        "bootstrap_sha256": bootstrap_sha256,
        "bootstrap_version": version,
        "launcher_name": launcher,
        "nxport_sha256": nxport_sha256,
        "port_id": port_id,
        "schema_version": 1,
    }
    encoded = json.dumps(
        material, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def highest_version(tokens, prefix_length):
    """Return the highest dotted version in a list of VERSION_ prefixed tokens."""
    values = {token[prefix_length:] for token in tokens}
    if not values:
        return None
    return sorted(values, key=lambda item: version_tuple(item, "ABI version"))[-1]


def assert_abi_policy_agrees():
    """Fail loudly if framework/nxabi/policy-v1.json drifts from the ceilings.

    The ceilings above stay hardcoded on purpose: they are the immutable law of
    the public profile.  The policy file is what every other checker reads, so
    the two must never disagree silently (M17-003).
    """
    try:
        with ABI_POLICY_PATH.open("r", encoding="utf-8") as stream:
            policy = json.load(stream)
    except (OSError, ValueError):
        return "unreadable"
    ceilings = policy.get("ceilings")
    if not isinstance(ceilings, dict):
        return "malformed"
    expected = {
        "glibc_max": PUBLIC_GLIBC_MAX,
        "glibcxx_max": PUBLIC_GLIBCXX_MAX,
        "cxxabi_max": PUBLIC_CXXABI_MAX,
    }
    for key, value in expected.items():
        if ceilings.get(key) != value:
            fail("nxabi policy drift: ceilings.{} is {!r}, nxrelease enforces {!r}"
                 .format(key, ceilings.get(key), value))
    return "agrees"


def minimum_version(left, right):
    if version_tuple(left, "version") <= version_tuple(right, "version"):
        return left
    return right


def validate_ceiling(value, context):
    version_tuple(value, context)
    if version_gt(value, PUBLIC_GLIBC_MAX):
        fail("{} {} exceeds the immutable public ceiling {}".format(
            context, value, PUBLIC_GLIBC_MAX
        ))
    return value


def safe_relative(value, context, allow_dot=False, allow_internal=False):
    value = require_string(value, context)
    if "\\" in value:
        fail("{} must use '/' separators".format(context))
    pure = PurePosixPath(value)
    if pure.is_absolute() or any(part in ("", "..") for part in pure.parts):
        fail("{} is not a safe relative path: {}".format(context, value))
    if not allow_dot and value in (".", ""):
        fail("{} cannot be '.'".format(context))
    normalized = pure.as_posix()
    if normalized.startswith("./"):
        normalized = normalized[2:]
    if value != "." and normalized != value:
        fail("{} is not in canonical relative form: {}".format(context, value))
    if not allow_internal and INTERNAL_DIRNAME in PurePosixPath(normalized).parts:
        fail("{} uses reserved release directory {}".format(
            context, INTERNAL_DIRNAME
        ))
    return normalized


def safe_top_level_item(value, context):
    """Normalize PortMaster's conventional one trailing slash for directories."""
    value = require_string(value, context)
    if value.endswith("//"):
        fail("{} has more than one trailing '/'".format(context))
    directory_hint = value.endswith("/")
    candidate = value[:-1] if directory_hint else value
    normalized = safe_relative(candidate, context)
    if "/" in normalized:
        fail("{} must name one top-level member".format(context))
    return normalized, directory_hint


def validate_soname(value, context):
    value = require_string(value, context)
    if not SONAME_RE.match(value) or value in (".", ".."):
        fail("{} is not a portable ELF library basename: {}".format(
            context, value
        ))
    return value


def dependency_namespace(kind):
    """Every ELF admitted to a public NXRelease package is Linux-native."""
    if kind not in LINUX_ELF_KINDS:
        fail("unsupported ELF namespace kind {}".format(kind))
    return "linux"


def internal_paths(port_dir):
    base = PurePosixPath(port_dir, INTERNAL_DIRNAME)
    return (
        PurePosixPath(base, METADATA_BASENAME).as_posix(),
        PurePosixPath(base, CHECKSUM_MANIFEST_BASENAME).as_posix(),
        PurePosixPath(base, SBOM_BASENAME).as_posix(),
    )


def portable_path_key(value):
    """Collision key for case-insensitive, normalization-prone removable media."""
    return unicodedata.normalize("NFC", value).casefold()


def validate_public_shell_layout(paths, launcher, port_dir, context):
    """Keep one public launcher and reject the retired secondary run.sh hop."""
    forbidden_run_key = portable_path_key(
        PurePosixPath(port_dir, "run.sh").as_posix()
    )
    top_level_shells = []
    for path in paths:
        portable_key = portable_path_key(path)
        if portable_key == forbidden_run_key:
            fail("{} contains forbidden secondary <port>/run.sh: {}".format(
                context, path
            ))
        parts = PurePosixPath(path).parts
        if len(parts) == 1 and portable_key.endswith(".sh"):
            top_level_shells.append(path)

    if len(top_level_shells) != 1:
        fail("{} must contain exactly one top-level .sh file; found {}".format(
            context, len(top_level_shells)
        ))
    if top_level_shells[0] != launcher:
        fail("{} top-level .sh file must be package.launcher".format(context))


def sha256_file(path):
    digest = hashlib.sha256()
    with open(str(path), "rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def parse_sha256(value, context):
    value = require_string(value, context).lower()
    if not SHA256_RE.match(value):
        fail("{} must be 64 lowercase hexadecimal characters".format(context))
    return value


def reject_private_literal(value, context):
    encoded = value.encode("utf-8")
    if (PRIVATE_PATH_RE.search(encoded) or IPV4_RE.search(encoded) or
            HOST_LITERAL_RE.search(encoded)):
        fail("{} contains private host information".format(context))


def source_is_within(root, path):
    try:
        return os.path.commonpath((str(root), str(path))) == str(root)
    except ValueError:
        return False


def ensure_no_symlink(path, root, context):
    current = path
    while True:
        if current.is_symlink():
            fail("{} traverses a symlink: {}".format(context, current))
        if current == root:
            break
        if not source_is_within(root, current):
            fail("{} escapes source_root".format(context))
        current = current.parent


def normalized_mode(path, explicit, context):
    if explicit is not None:
        if not isinstance(explicit, str) or explicit not in ("0644", "0755"):
            fail("{} mode must be either '0644' or '0755'".format(context))
        return int(explicit, 8)
    return 0o755 if os.access(str(path), os.X_OK) else 0o644


def is_elf(path):
    try:
        with open(str(path), "rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError as exc:
        fail("cannot read {}: {}".format(path, exc))


def run_readelf(path, arguments):
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    process = subprocess.run(
        ["readelf"] + list(arguments) + [str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        env=environment,
    )
    if process.returncode != 0:
        detail = (process.stderr or process.stdout).strip()
        fail("readelf {} rejected {}: {}".format(
            " ".join(arguments), path, detail
        ))
    return process.stdout + process.stderr


def elf_information(path, logical_path, kind, expected_arch, ceiling, build_profile,
                    expected_needed, expected_soname, provenance=None):
    header = run_readelf(path, ("-hW",))
    class_match = re.search(r"^\s*Class:\s*(\S+)", header, re.MULTILINE)
    data_match = re.search(r"^\s*Data:\s*(.+?)\s*$", header, re.MULTILINE)
    header_version_match = re.search(
        r"^\s*Version:\s*([0-9]+)(?:\s|$)", header, re.MULTILINE
    )
    type_match = re.search(r"^\s*Type:\s*(\S+)", header, re.MULTILINE)
    machine_match = re.search(r"^\s*Machine:\s*(.+?)\s*$", header, re.MULTILINE)
    flags_match = re.search(r"^\s*Flags:\s*(.+?)\s*$", header, re.MULTILINE)
    if (not class_match or not data_match or not header_version_match or
            not type_match or not machine_match):
        fail("ELF {} has an incomplete header".format(logical_path))
    elf_class = class_match.group(1)
    data_encoding = data_match.group(1)
    elf_type = type_match.group(1)
    machine = machine_match.group(1)

    if "little endian" not in data_encoding:
        fail("ELF {} is not little-endian".format(logical_path))
    if header_version_match.group(1) != "1":
        fail("ELF {} has an unsupported header version".format(logical_path))
    if elf_type not in ("EXEC", "DYN"):
        fail("ELF {} has non-loadable type {}; expected ET_EXEC/ET_DYN".format(
            logical_path, elf_type
        ))

    if expected_arch:
        expected_machine = ARCH_MACHINES[expected_arch]
        if machine != expected_machine:
            fail("ELF {} is {}, manifest says {} ({})".format(
                logical_path, machine, expected_arch, expected_machine
            ))
        expected_class = ARCH_CLASSES[expected_arch]
        if elf_class != expected_class:
            fail("ELF {} class {} disagrees with {} ({})".format(
                logical_path, elf_class, expected_arch, expected_class
            ))

    if expected_arch == "armv7":
        flags = flags_match.group(1) if flags_match else ""
        if "Version5 EABI" not in flags:
            fail("ELF {} is not ARM EABI5".format(logical_path))
        if kind in LINUX_ELF_KINDS and "hard-float ABI" not in flags:
            fail("ELF {} is ARM soft-float/unknown; armv7 Linux must be hard-float".format(
                logical_path
            ))

    versions = run_readelf(path, ("--version-info", "--wide"))
    tokens = sorted(set(GLIBC_TOKEN_RE.findall(versions)))
    cxx_maximum = None
    cxxabi_maximum = None
    if kind in LINUX_ELF_KINDS:
        # M17-010: libstdc++ carries its own version namespaces and a C++ port
        # can breach the device floor without ever touching a new GLIBC_ token.
        cxx_maximum = highest_version(
            GLIBCXX_TOKEN_RE.findall(versions), len("GLIBCXX_")
        )
        cxxabi_maximum = highest_version(
            CXXABI_TOKEN_RE.findall(versions), len("CXXABI_")
        )
        if cxx_maximum is not None and version_gt(cxx_maximum, PUBLIC_GLIBCXX_MAX):
            fail("ELF {} requires GLIBCXX_{} (> GLIBCXX_{})".format(
                logical_path, cxx_maximum, PUBLIC_GLIBCXX_MAX
            ))
        if cxxabi_maximum is not None and version_gt(cxxabi_maximum, PUBLIC_CXXABI_MAX):
            fail("ELF {} requires CXXABI_{} (> CXXABI_{})".format(
                logical_path, cxxabi_maximum, PUBLIC_CXXABI_MAX
            ))
    forbidden_abi = [token for token in tokens if not re.match(r"^GLIBC_[0-9]", token)]
    if forbidden_abi and kind in LINUX_ELF_KINDS:
        fail("ELF {} requires unsupported private GLIBC ABI: {}".format(
            logical_path, ", ".join(forbidden_abi)
        ))
    numeric_versions = sorted(
        set(token[len("GLIBC_"):] for token in tokens if re.match(r"^GLIBC_[0-9]", token)),
        key=lambda item: version_tuple(item, "ELF GLIBC version"),
    )
    maximum = numeric_versions[-1] if numeric_versions else "none"
    for required_version in numeric_versions:
        if version_gt(required_version, ceiling):
            fail("ELF {} requires GLIBC_{} (> GLIBC_{})".format(
                logical_path, required_version, ceiling
            ))

    program_headers = run_readelf(path, ("-lW",))
    if not re.search(r"^\s*LOAD\s", program_headers, re.MULTILINE):
        fail("ELF {} has no PT_LOAD segment".format(logical_path))
    interpreter_matches = re.findall(
        r"Requesting program interpreter:\s*([^\]]+)\]", program_headers
    )
    if len(interpreter_matches) > 1:
        fail("ELF {} has multiple PT_INTERP segments".format(logical_path))
    interpreter = interpreter_matches[0].strip() if interpreter_matches else "none"
    if interpreter != "none" and not interpreter.startswith("/"):
        fail("ELF {} has non-absolute PT_INTERP {}".format(logical_path, interpreter))
    if interpreter.startswith("/home/") or interpreter.startswith("/Users/"):
        fail("ELF {} embeds private PT_INTERP {}".format(logical_path, interpreter))

    dynamic = run_readelf(path, ("-dW",))
    needed_raw = re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic)
    if len(needed_raw) != len(set(needed_raw)):
        fail("ELF {} repeats a DT_NEEDED entry".format(logical_path))
    needed = sorted(
        validate_soname(item, "ELF {} DT_NEEDED".format(logical_path))
        for item in needed_raw
    )
    bionic_dependencies = sorted(set(needed) & BIONIC_SONAMES)
    if bionic_dependencies:
        fail("ELF {} imports Android/Bionic dependencies and cannot be packaged "
             "as Linux: {}".format(logical_path, ", ".join(bionic_dependencies)))
    if len({portable_path_key(item) for item in needed}) != len(needed):
        fail("ELF {} has a portable-name collision in DT_NEEDED".format(
            logical_path
        ))
    sonames = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", dynamic)
    if len(sonames) > 1:
        fail("ELF {} declares multiple DT_SONAME values".format(logical_path))
    soname = (
        validate_soname(sonames[0], "ELF {} DT_SONAME".format(logical_path))
        if sonames else None
    )
    if needed != expected_needed:
        fail("ELF {} DT_NEEDED differs from manifest: expected {}, got {}".format(
            logical_path, expected_needed, needed
        ))
    if soname != expected_soname:
        fail("ELF {} DT_SONAME differs from manifest: expected {}, got {}".format(
            logical_path, expected_soname, soname
        ))
    search_paths = re.findall(r"\((?:RPATH|RUNPATH)\).*?\[([^\]]*)\]", dynamic)
    if search_paths:
        fail("ELF {} embeds RPATH/RUNPATH; universal packages require none".format(
            logical_path
        ))

    if interpreter.startswith("/system/bin/linker"):
        fail("ELF {} is tagged Linux but has Android PT_INTERP {}".format(
            logical_path, interpreter
        ))
    if build_profile != LOW_GLIBC_PROFILE:
        fail("ELF {} uses forbidden build_profile {}; universal packages require {}".format(
            logical_path, build_profile or "missing", LOW_GLIBC_PROFILE
        ))
    expected_interpreter = LINUX_INTERPRETERS[expected_arch]
    if interpreter != "none" and interpreter != expected_interpreter:
        fail("ELF {} PT_INTERP must be exactly {}; got {}".format(
            logical_path, expected_interpreter, interpreter
        ))

    # M17-018: provenance that survives strip.  The build-id is the only
    # identifier that ties a packaged artifact back to a build tree, and
    # .note.nx.toolchain names the toolchain that produced it.
    notes = run_readelf(path, ("-nW",))
    build_id_match = re.search(r"Build ID:\s*([0-9a-f]+)", notes)

    return {
        "architecture": expected_arch,
        "build_id": build_id_match.group(1) if build_id_match else None,
        "build_profile": build_profile,
        "cxxabi_max": cxxabi_maximum,
        "glibcxx_max": cxx_maximum,
        "class": elf_class,
        "data": "little-endian",
        "elf_type": elf_type,
        "flags": flags_match.group(1) if flags_match else "",
        "glibc_max": maximum,
        "interpreter": interpreter,
        "kind": kind,
        "machine": machine,
        "namespace": "linux",
        "needed": needed,
        "path": logical_path,
        "provenance": provenance,
        "sha256": sha256_file(path),
        "soname": soname,
    }


def load_json(path, context):
    try:
        with open(str(path), "r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError) as exc:
        fail("cannot read {} {}: {}".format(context, path, exc))


def validate_metadata_pin(value, context, port_dir, expected_suffix=None):
    value = require_object(value, context)
    require_keys(value, ("path", "sha256"), context)
    path = safe_relative(value.get("path"), context + ".path")
    if not path.startswith(port_dir + "/"):
        fail("{}.path must live inside package.port_dir".format(context))
    if (expected_suffix is not None and
            PurePosixPath(path).name.lower() != expected_suffix):
        fail("{}.path must name {}".format(context, expected_suffix))
    return {
        "path": path,
        "sha256": parse_sha256(value.get("sha256"), context + ".sha256"),
    }


def validate_portmaster_metadata_manifest(value, port_dir):
    if value is None:
        return {"gameinfo_xml": None, "images": [], "port_json": None}
    value = require_object(value, "portmaster_metadata")
    require_keys(value, ("port_json", "gameinfo_xml", "images"), "portmaster_metadata")
    result = {"gameinfo_xml": None, "images": [], "port_json": None}
    if value.get("port_json") is not None:
        result["port_json"] = validate_metadata_pin(
            value["port_json"], "portmaster_metadata.port_json", port_dir, "port.json"
        )
    if value.get("gameinfo_xml") is not None:
        result["gameinfo_xml"] = validate_metadata_pin(
            value["gameinfo_xml"], "portmaster_metadata.gameinfo_xml", port_dir,
            "gameinfo.xml",
        )
    images = value.get("images", [])
    if not isinstance(images, list):
        fail("portmaster_metadata.images must be an array")
    seen_paths = set()
    seen_roles = set()
    for index, image in enumerate(images):
        context = "portmaster_metadata.images[{}]".format(index)
        image = require_object(image, context)
        require_keys(image, ("path", "role", "sha256"), context)
        path = safe_relative(image.get("path"), context + ".path")
        if not path.startswith(port_dir + "/"):
            fail("{}.path must live inside package.port_dir".format(context))
        if not path.lower().endswith((".png", ".jpg", ".jpeg", ".webp")):
            fail("{}.path must be PNG, JPEG or WebP".format(context))
        role = require_string(image.get("role"), context + ".role")
        if role not in ("box", "cover", "screenshot", "splash", "thumbnail"):
            fail("{}.role is unsupported: {}".format(context, role))
        if path in seen_paths or role in seen_roles:
            fail("portmaster_metadata.images has duplicate path or role")
        seen_paths.add(path)
        seen_roles.add(role)
        result["images"].append({
            "path": path,
            "role": role,
            "sha256": parse_sha256(image.get("sha256"), context + ".sha256"),
        })
    result["images"].sort(key=lambda item: (item["role"], item["path"]))
    return result


def validate_dependencies_manifest(value, port_dir):
    if not isinstance(value, list):
        fail("dependencies must be a JSON array")
    result = []
    seen = {}
    folded = {}
    for index, declaration in enumerate(value):
        context = "dependencies[{}]".format(index)
        declaration = require_object(declaration, context)
        require_keys(
            declaration,
            ("namespace", "architecture", "soname", "provider", "path"),
            context,
        )
        namespace = require_string(
            declaration.get("namespace"), context + ".namespace"
        )
        if namespace not in DEPENDENCY_NAMESPACES:
            fail("{}.namespace must be linux or android".format(context))
        architecture = require_string(
            declaration.get("architecture"), context + ".architecture"
        )
        if architecture not in ARCH_MACHINES:
            fail("{}.architecture must be aarch64 or armv7".format(context))
        soname = validate_soname(declaration.get("soname"), context + ".soname")
        provider = require_string(
            declaration.get("provider"), context + ".provider"
        )
        if provider not in DEPENDENCY_PROVIDERS:
            fail("{}.provider is unsupported: {}".format(context, provider))
        path = declaration.get("path")
        if provider == "package":
            path = safe_relative(path, context + ".path")
            if not path.startswith(port_dir + "/"):
                fail("{}.path must live inside package.port_dir".format(context))
        elif path is not None:
            fail("{}.path is valid only for provider=package".format(context))
        if namespace == "android" and provider != "nxloader-import-registry" and provider != "package":
            fail("{} Android dependencies must use package or nxloader-import-registry".format(
                context
            ))
        if namespace == "linux" and provider == "nxloader-import-registry":
            fail("{} Linux dependencies cannot use nxloader-import-registry".format(
                context
            ))
        if provider == "glibc-base" and soname not in GLIBC_BASE_SONAMES:
            fail("{} cannot label {} as glibc-base".format(context, soname))
        key = (namespace, architecture, soname)
        portable_key = (namespace, architecture, portable_path_key(soname))
        if key in seen:
            fail("duplicate dependency provider for {}/{}/{}".format(*key))
        if portable_key in folded:
            fail("portable dependency-name collision: {} and {}".format(
                folded[portable_key], soname
            ))
        seen[key] = context
        folded[portable_key] = soname
        normalized = {
            "architecture": architecture,
            "namespace": namespace,
            "provider": provider,
            "soname": soname,
        }
        if path is not None:
            normalized["path"] = path
        result.append(normalized)
    return sorted(
        result,
        key=lambda item: (item["namespace"], item["architecture"], item["soname"]),
    )


def load_manifest(path, requested_ceiling=None):
    manifest_path = Path(path).resolve()
    try:
        manifest_bytes = manifest_path.read_bytes()
        if len(manifest_bytes) > 16 * 1024 * 1024:
            fail("manifest is unexpectedly larger than 16777216 bytes")
        data = json.loads(manifest_bytes.decode("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        fail("cannot read manifest {}: {}".format(manifest_path, exc))
    data = require_object(data, "manifest")
    manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
    require_keys(
        data,
        ("schema_version", "source_root", "package", "release", "nxextract", "portmaster_metadata", "dependencies", "files", "exceptions"),
        "manifest",
    )
    if data.get("schema_version") != SCHEMA_VERSION:
        fail("manifest schema_version must be {}; v1 did not close dependencies or pin the complete NXExtract runtime".format(
            SCHEMA_VERSION
        ))

    source_root_value = safe_relative(data.get("source_root", "."), "source_root", allow_dot=True)
    source_root = (manifest_path.parent / source_root_value).resolve()
    if not source_root.is_dir():
        fail("source_root is not a directory: {}".format(source_root))
    if (manifest_path.parent / source_root_value).is_symlink():
        fail("source_root cannot be a symlink")

    package = require_object(data.get("package"), "package")
    require_keys(package, ("id", "version", "profile", "launcher", "launcher_chain", "launcher_contract", "port_dir", "license"), "package")
    package_id = require_string(package.get("id"), "package.id")
    if not PACKAGE_ID_RE.match(package_id):
        fail("package.id must match {}".format(PACKAGE_ID_RE.pattern))
    package_version = require_string(package.get("version"), "package.version")
    if len(package_version) > 80:
        fail("package.version is too long")
    if package.get("profile") != PROFILE:
        fail("package.profile must be {}".format(PROFILE))
    launcher = safe_relative(package.get("launcher"), "package.launcher")
    if "/" in launcher or not launcher.lower().endswith(".sh"):
        fail("package.launcher must be one top-level .sh file")
    port_dir = safe_relative(package.get("port_dir"), "package.port_dir")
    if "/" in port_dir:
        fail("package.port_dir must be one top-level directory name")
    if port_dir != package_id:
        fail("package.port_dir must equal package.id for the canonical PortMaster layout")
    if launcher.casefold() == port_dir.casefold():
        fail("package.launcher and package.port_dir collide")
    package_license = package.get("license")
    if package_license is None:
        fail("package.license is required for a public release")
    package_license = require_object(package_license, "package.license")
    require_keys(
        package_license, ("spdx_id", "source_url", "file"), "package.license")
    license_spdx = require_string(
        package_license.get("spdx_id"), "package.license.spdx_id")
    license_source = require_string(
        package_license.get("source_url"), "package.license.source_url")
    license_file = safe_relative(
        package_license.get("file"), "package.license.file")
    if not license_file.startswith(port_dir + "/"):
        fail("package.license.file must live inside package.port_dir")
    reject_private_literal(license_source, "package.license.source_url")
    reject_private_literal(license_spdx, "package.license.spdx_id")
    package_license = {
        "file": license_file,
        "source_url": license_source,
        "spdx_id": license_spdx,
    }

    launcher_contract = require_object(
        package.get("launcher_contract"), "package.launcher_contract"
    )
    require_keys(
        launcher_contract,
        ("generator", "version", "config_path", "config_sha256"),
        "package.launcher_contract",
    )
    if launcher_contract.get("generator") != "nxbootstrap":
        fail("package.launcher_contract.generator must be nxbootstrap")
    launcher_contract_version = require_string(
        launcher_contract.get("version"), "package.launcher_contract.version"
    )
    version_tuple(launcher_contract_version, "package.launcher_contract.version")

    launcher_chain = package.get("launcher_chain")
    if not isinstance(launcher_chain, list) or len(launcher_chain) not in (1, 2):
        fail("package.launcher_chain must contain launcher and nxbootstrap only")
    normalized_chain = []
    for index, chain_path in enumerate(launcher_chain):
        normalized_chain.append(safe_relative(
            chain_path, "package.launcher_chain[{}]".format(index)
        ))
    if normalized_chain[0] != launcher:
        fail("package.launcher_chain must start with package.launcher")
    if len(set(normalized_chain)) != len(normalized_chain):
        fail("package.launcher_chain contains duplicates")
    for chain_path in normalized_chain[1:]:
        if not chain_path.startswith(port_dir + "/"):
            fail("launcher chain scripts after the wrapper must live inside package.port_dir")
    if bootstrap_self_contained(launcher_contract_version):
        expected_chain = [launcher]
    else:
        expected_chain = [
            launcher,
            port_dir + "/" + nxbootstrap_script_name(launcher_contract_version),
        ]
    if normalized_chain != expected_chain:
        fail("package.launcher_chain must select the canonical bootstrap for launcher_contract.version")
    launcher_config_path = safe_relative(
        launcher_contract.get("config_path"),
        "package.launcher_contract.config_path",
    )
    if launcher_config_path != port_dir + "/nxport.json":
        fail("package.launcher_contract.config_path must be {}/nxport.json".format(
            port_dir
        ))
    launcher_config_sha = parse_sha256(
        launcher_contract.get("config_sha256"),
        "package.launcher_contract.config_sha256",
    )

    release = require_object(data.get("release"), "release")
    require_keys(release, ("source_date_epoch", "max_glibc", "compression"), "release")
    epoch = release.get("source_date_epoch")
    if isinstance(epoch, bool) or not isinstance(epoch, int):
        fail("release.source_date_epoch must be an integer")
    minimum_epoch = 315532800  # 1980-01-01 UTC, the ZIP timestamp floor.
    maximum_epoch = 4354819198  # 2107-12-31 23:59:58 UTC.
    if epoch < minimum_epoch or epoch > maximum_epoch:
        fail("release.source_date_epoch must fit the ZIP 1980..2107 range")
    manifest_ceiling = validate_ceiling(
        release.get("max_glibc", PUBLIC_GLIBC_MAX), "release.max_glibc"
    )
    if requested_ceiling is not None:
        requested_ceiling = validate_ceiling(requested_ceiling, "--max-glibc")
        ceiling = minimum_version(manifest_ceiling, requested_ceiling)
    else:
        ceiling = manifest_ceiling
    compression = release.get("compression", "deflated")
    if compression not in ("deflated", "stored"):
        fail("release.compression must be 'deflated' or 'stored'")

    nxextract = require_object(data.get("nxextract"), "nxextract")
    require_keys(
        nxextract,
        (
            "path", "version", "minimum_version", "sha256",
            "runner_path", "runner_sha256",
            "runtime_env_path", "runtime_env_sha256",
            "recipe_path", "recipe_sha256",
        ),
        "nxextract",
    )
    nx_path = safe_relative(nxextract.get("path"), "nxextract.path")
    nx_runner = safe_relative(nxextract.get("runner_path"), "nxextract.runner_path")
    nx_runtime_env = safe_relative(
        nxextract.get("runtime_env_path"), "nxextract.runtime_env_path"
    )
    nx_recipe = safe_relative(
        nxextract.get("recipe_path"), "nxextract.recipe_path"
    )
    expected_nx_paths = {
        "path": port_dir + "/nxextract/nxextract.py",
        "runner_path": port_dir + "/nxextract/run-extractor.sh",
        "runtime_env_path": port_dir + "/nxextract/nxextract-runtime-env.sh",
        "recipe_path": port_dir + "/extractor.json",
    }
    actual_nx_paths = {
        "path": nx_path,
        "runner_path": nx_runner,
        "runtime_env_path": nx_runtime_env,
        "recipe_path": nx_recipe,
    }
    for field, expected_path in expected_nx_paths.items():
        if actual_nx_paths[field] != expected_path:
            fail("nxextract.{} must be the canonical path {}".format(
                field, expected_path
            ))
    nx_version = require_string(nxextract.get("version"), "nxextract.version")
    nx_minimum = require_string(nxextract.get("minimum_version"), "nxextract.minimum_version")
    version_tuple(nx_version, "nxextract.version")
    version_tuple(nx_minimum, "nxextract.minimum_version")
    if version_lt(nx_minimum, NXEXTRACT_FLOOR):
        fail("nxextract.minimum_version {} is below tool floor {}".format(
            nx_minimum, NXEXTRACT_FLOOR
        ))
    if version_lt(nx_version, nx_minimum):
        fail("nxextract.version {} is below manifest minimum {}".format(
            nx_version, nx_minimum
        ))
    nx_sha = parse_sha256(nxextract.get("sha256"), "nxextract.sha256")
    nx_runner_sha = parse_sha256(
        nxextract.get("runner_sha256"), "nxextract.runner_sha256"
    )
    nx_runtime_env_sha = parse_sha256(
        nxextract.get("runtime_env_sha256"), "nxextract.runtime_env_sha256"
    )
    nx_recipe_sha = parse_sha256(
        nxextract.get("recipe_sha256"), "nxextract.recipe_sha256"
    )

    portmaster_metadata = validate_portmaster_metadata_manifest(
        data.get("portmaster_metadata"), port_dir
    )
    dependencies = validate_dependencies_manifest(data.get("dependencies"), port_dir)

    file_entries = data.get("files")
    if not isinstance(file_entries, list) or not file_entries:
        fail("files must be a non-empty JSON array")

    exceptions = data.get("exceptions", [])
    if not isinstance(exceptions, list):
        fail("exceptions must be a JSON array")
    exception_map = {}
    for index, exception in enumerate(exceptions):
        context = "exceptions[{}]".format(index)
        exception = require_object(exception, context)
        require_keys(exception, ("rule", "path", "reason"), context)
        rule = require_string(exception.get("rule"), context + ".rule")
        if rule not in EXCEPTION_RULES:
            fail("{} has unsupported rule {}".format(context, rule))
        target = safe_relative(exception.get("path"), context + ".path")
        reason = require_string(exception.get("reason"), context + ".reason")
        if len(reason) < 16:
            fail("{}.reason must contain concrete evidence".format(context))
        reject_private_literal(reason, context + ".reason")
        key = (rule, target)
        if key in exception_map:
            fail("duplicate exception {} for {}".format(rule, target))
        exception_map[key] = reason

    config = {
        "ceiling": ceiling,
        "compression": compression,
        "data": data,
        "dependencies": dependencies,
        "epoch": epoch,
        "exception_map": exception_map,
        "launcher": launcher,
        "launcher_chain": normalized_chain,
        "launcher_contract": {
            "config_path": launcher_config_path,
            "config_sha256": launcher_config_sha,
            "generator": "nxbootstrap",
            "version": launcher_contract_version,
        },
        "manifest_ceiling": manifest_ceiling,
        "manifest_path": manifest_path,
        "manifest_sha256": manifest_sha256,
        "nxextract": {
            "minimum_version": nx_minimum,
            "path": nx_path,
            "runner_path": nx_runner,
            "runner_sha256": nx_runner_sha,
            "runtime_env_path": nx_runtime_env,
            "runtime_env_sha256": nx_runtime_env_sha,
            "recipe_path": nx_recipe,
            "recipe_sha256": nx_recipe_sha,
            "sha256": nx_sha,
            "version": nx_version,
        },
        "package_id": package_id,
        "package_version": package_version,
        "portmaster_metadata": portmaster_metadata,
        "port_dir": port_dir,
        "license": package_license,
        "source_root": source_root,
    }
    config["records"] = expand_inputs(file_entries, config)
    validate_package_members(config)
    return config


def validate_entry(entry, index, config):
    context = "files[{}]".format(index)
    entry = require_object(entry, context)
    require_keys(
        entry,
        ("source", "target", "kind", "mode", "sha256", "architecture", "build_profile", "provenance", "needed", "soname"),
        context,
    )
    source_value = safe_relative(entry.get("source"), context + ".source", allow_dot=True)
    target = safe_relative(entry.get("target"), context + ".target")
    kind = require_string(entry.get("kind"), context + ".kind")
    if kind not in ALLOWED_KINDS:
        fail("{}.kind is unsupported: {}".format(context, kind))
    if kind in ("launcher", "script") and not target.lower().endswith(".sh"):
        fail("{}.kind={} requires a .sh target so it cannot bypass shell audit".format(
            context, kind
        ))
    source_candidate = config["source_root"] / PurePosixPath(source_value)
    ensure_no_symlink(source_candidate, config["source_root"], context + ".source")
    source = source_candidate.resolve()
    if not source_is_within(config["source_root"], source):
        fail("{}.source escapes source_root".format(context))
    if not source.exists():
        fail("{}.source does not exist: {}".format(context, source_value))
    ensure_no_symlink(source, config["source_root"], context + ".source")

    expected_sha = None
    if "sha256" in entry:
        expected_sha = parse_sha256(entry["sha256"], context + ".sha256")
    architecture = entry.get("architecture")
    build_profile = entry.get("build_profile")
    provenance = entry.get("provenance")
    needed = entry.get("needed")
    soname = entry.get("soname")

    if kind in ELF_KINDS:
        if expected_sha is None:
            fail("{}.sha256 is required for every classified ELF".format(context))
        if "soname" not in entry:
            fail("{}.soname must explicitly be a string or null".format(context))
        if not isinstance(needed, list):
            fail("{}.needed must be an exact DT_NEEDED array".format(context))
        normalized_needed = []
        for needed_index, library in enumerate(needed):
            library = require_string(
                library, "{}.needed[{}]".format(context, needed_index)
            )
            normalized_needed.append(validate_soname(
                library, "{}.needed[{}]".format(context, needed_index)
            ))
        if normalized_needed != sorted(set(normalized_needed)):
            fail("{}.needed must be sorted and unique".format(context))
        if len({portable_path_key(item) for item in normalized_needed}) != len(normalized_needed):
            fail("{}.needed contains a portable-name collision".format(context))
        needed = normalized_needed
        if soname is not None:
            soname = validate_soname(soname, context + ".soname")

    if kind in LINUX_ELF_KINDS:
        if architecture not in ARCH_MACHINES:
            fail("{}.architecture must be aarch64 or armv7".format(context))
        if build_profile != LOW_GLIBC_PROFILE:
            fail("{}.build_profile must be {}; current-host variants cannot enter a universal package".format(
                context, LOW_GLIBC_PROFILE
            ))
        provenance = require_string(provenance, context + ".provenance")
    else:
        if any(field in entry for field in (
                "architecture", "build_profile", "provenance", "needed", "soname")):
            fail("{} uses ELF-only metadata on kind {}".format(context, kind))

    if provenance is not None:
        reject_private_literal(provenance, context + ".provenance")

    if source.is_dir():
        if kind in SINGLE_FILE_KINDS:
            fail("{} kind {} requires a regular file source".format(context, kind))
        if "mode" in entry or expected_sha is not None:
            fail("{} directory inputs cannot set mode or sha256".format(context))
    elif not source.is_file():
        fail("{}.source is not a regular file or directory".format(context))
    elif kind in SINGLE_FILE_KINDS and expected_sha is None:
        fail("{}.sha256 is required for kind {}".format(context, kind))

    return {
        "architecture": architecture,
        "build_profile": build_profile,
        "entry": entry,
        "expected_sha": expected_sha,
        "kind": kind,
        "needed": needed,
        "provenance": provenance,
        "soname": soname,
        "source": source,
        "target": target,
    }


def expand_inputs(entries, config):
    records = []
    targets = {}
    target_parent_dirs = set()
    folded_target_parent_dirs = set()
    folded_targets = {}
    for index, raw_entry in enumerate(entries):
        entry = validate_entry(raw_entry, index, config)
        source = entry["source"]
        expanded = []
        if source.is_file():
            mode = normalized_mode(source, raw_entry.get("mode"), "files[{}]".format(index))
            expanded.append((source, entry["target"], mode))
        else:
            for directory, directory_names, file_names in os.walk(str(source), followlinks=False):
                directory_path = Path(directory)
                for name in list(directory_names):
                    candidate = directory_path / name
                    if candidate.is_symlink():
                        fail("files[{}] directory contains symlink {}".format(index, candidate))
                for name in sorted(file_names):
                    candidate = directory_path / name
                    if candidate.is_symlink() or not candidate.is_file():
                        fail("files[{}] directory contains non-regular file {}".format(index, candidate))
                    relative = candidate.relative_to(source).as_posix()
                    target = PurePosixPath(entry["target"], relative).as_posix()
                    mode = normalized_mode(candidate, None, "files[{}]".format(index))
                    expanded.append((candidate, target, mode))
            if not expanded:
                fail("files[{}] directory source is empty".format(index))

        for source_file, target, mode in sorted(expanded, key=lambda item: item[1]):
            target = safe_relative(
                target, "expanded files[{}].target".format(index)
            )
            target_parts = PurePosixPath(target).parts
            parents = [
                PurePosixPath(*target_parts[:cut]).as_posix()
                for cut in range(1, len(target_parts))
            ]
            conflicting_parent = next((item for item in parents if item in targets), None)
            folded_parents = [portable_path_key(item) for item in parents]
            folded_conflicting_parent = next(
                (item for item in folded_parents if item in folded_targets), None
            )
            folded_target = portable_path_key(target)
            if conflicting_parent is not None or target in target_parent_dirs:
                fail("file/directory target collision at {}{}".format(
                    target,
                    " (parent file {})".format(conflicting_parent)
                    if conflicting_parent else "",
                ))
            if (folded_conflicting_parent is not None or
                    folded_target in folded_target_parent_dirs):
                fail("portable file/directory target collision at {}".format(target))
            folded = folded_target
            if target in targets:
                fail("duplicate target {}".format(target))
            if folded in folded_targets:
                fail("case-insensitive target collision: {} and {}".format(
                    folded_targets[folded], target
                ))
            targets[target] = True
            target_parent_dirs.update(parents)
            folded_target_parent_dirs.update(folded_parents)
            folded_targets[folded] = target
            actual_sha = sha256_file(source_file)
            if entry["expected_sha"] is not None and actual_sha != entry["expected_sha"]:
                fail("source hash mismatch for {}: expected {}, got {}".format(
                    target, entry["expected_sha"], actual_sha
                ))
            record = dict(entry)
            record.update({
                "mode": mode,
                "sha256": actual_sha,
                "source": source_file,
                "target": target,
            })
            record.pop("entry", None)
            records.append(record)
    return sorted(records, key=lambda item: item["target"])


def validate_package_members(config):
    by_target = {record["target"]: record for record in config["records"]}
    validate_public_shell_layout(
        by_target, config["launcher"], config["port_dir"], "package manifest"
    )
    launcher = by_target.get(config["launcher"])
    if launcher is None or launcher["kind"] != "launcher":
        fail("package.launcher must match one files[] entry with kind launcher")
    if launcher["mode"] != 0o755:
        fail("package.launcher must be staged with mode 0755")
    if not any(path.startswith(config["port_dir"] + "/") for path in by_target):
        fail("package.port_dir has no staged files")
    for index, chain_path in enumerate(config["launcher_chain"]):
        chain_record = by_target.get(chain_path)
        expected_kind = "launcher" if index == 0 else "script"
        if chain_record is None or chain_record["kind"] != expected_kind:
            fail("launcher chain path {} must use kind {}".format(
                chain_path, expected_kind
            ))
        if chain_record.get("expected_sha") is None:
            fail("launcher chain path {} must be pinned with sha256".format(chain_path))

    bootstrap_path = config["launcher_chain"][-1]
    compatibility_path = config["port_dir"] + "/nxbootstrap.sh"
    if bootstrap_self_contained(config["launcher_contract"]["version"]):
        for retired in (compatibility_path,
                        config["port_dir"] + "/nxdeployment.json"):
            if retired in by_target:
                fail("self-contained launcher must not ship retired artifact {}".format(
                    retired
                ))
        for path in by_target:
            if (path.startswith(config["port_dir"] + "/nxbootstrap-") and
                    path.endswith(".sh")):
                fail("self-contained launcher must not ship a bootstrap library")
        # KOTOR 1.1.x field lesson: port-env.sh selected a fallback runtime
        # that the package did not contain, so low-glibc devices silently
        # lost their working binary.  Every $GAMEDIR file that port-env.sh
        # tests or assigns must exist in the staged port directory.
        port_env_path = config["port_dir"] + "/port-env.sh"
        port_env_record = by_target.get(port_env_path)
        if port_env_record is not None:
            port_env_text = read_small_text(
                port_env_record["actual_path"], port_env_path
            )
            referenced = set()
            for line in active_shell_text(port_env_text).splitlines():
                if not re.search(r"(?:\[\s+-[fxe]\s|BIN(?:_PRELOAD)?=)", line):
                    continue
                referenced.update(re.findall(
                    r"\$(?:\{)?GAMEDIR(?:\})?/([A-Za-z0-9][A-Za-z0-9._/-]*)",
                    line,
                ))
            for relative in sorted(referenced):
                if "*" in relative or "$" in relative:
                    continue
                target = config["port_dir"] + "/" + relative
                if target not in by_target:
                    fail("port-env.sh references {} which is not staged".format(
                        target
                    ))
    elif bootstrap_path != compatibility_path:
        compatibility = by_target.get(compatibility_path)
        bootstrap = by_target[bootstrap_path]
        if compatibility is None or compatibility["kind"] != "script":
            fail("versioned nxbootstrap requires a canonical nxbootstrap.sh compatibility copy")
        if compatibility["mode"] != 0o644:
            fail("canonical nxbootstrap.sh compatibility copy must use mode 0644")
        if compatibility.get("expected_sha") is None:
            fail("canonical nxbootstrap.sh compatibility copy must be pinned with sha256")
        if compatibility["sha256"] != bootstrap["sha256"]:
            fail("canonical and versioned nxbootstrap copies must be byte-identical")
        deployment_path = config["port_dir"] + "/nxdeployment.json"
        deployment = by_target.get(deployment_path)
        if deployment is None or deployment["kind"] != "payload":
            fail("versioned nxbootstrap requires a classified nxdeployment.json receipt")
        if deployment["mode"] != 0o644:
            fail("nxdeployment.json must use mode 0644")
        if deployment.get("expected_sha") is None:
            fail("nxdeployment.json must be pinned with sha256")

    launcher_contract = config["launcher_contract"]
    launcher_config_record = by_target.get(launcher_contract["config_path"])
    if (launcher_config_record is None or
            launcher_config_record["kind"] != "nxbootstrap-config"):
        fail("launcher_contract.config_path must match an nxbootstrap-config file entry")
    if launcher_config_record["sha256"] != launcher_contract["config_sha256"]:
        fail("launcher_contract.config_sha256 does not pin nxport.json")

    nx = config["nxextract"]
    nx_record = by_target.get(nx["path"])
    runner_record = by_target.get(nx["runner_path"])
    runtime_env_record = by_target.get(nx["runtime_env_path"])
    recipe_record = by_target.get(nx["recipe_path"])
    if nx_record is None or nx_record["kind"] != "nxextract":
        fail("nxextract.path must match one files[] entry with kind nxextract")
    if runner_record is None or runner_record["kind"] != "nxextract-runner":
        fail("nxextract.runner_path must match one files[] entry with kind nxextract-runner")
    if (runtime_env_record is None or
            runtime_env_record["kind"] != "nxextract-runtime-env"):
        fail("nxextract.runtime_env_path must match one files[] entry with kind nxextract-runtime-env")
    if recipe_record is None or recipe_record["kind"] != "nxextract-recipe":
        fail("nxextract.recipe_path must match one files[] entry with kind nxextract-recipe")
    for record, label in (
            (nx_record, "nxextract.py"), (runner_record, "run-extractor.sh"),
            (runtime_env_record, "nxextract-runtime-env.sh"),
            (recipe_record, "extractor.json")):
        if record["mode"] != 0o644:
            fail("canonical NXExtract {} must be packaged with mode 0644".format(
                label
            ))
    if nx_record["sha256"] != nx["sha256"]:
        fail("nxextract.sha256 does not pin the staged NXExtract file")
    if runner_record["sha256"] != nx["runner_sha256"]:
        fail("nxextract.runner_sha256 does not pin the staged runner")
    if runtime_env_record["sha256"] != nx["runtime_env_sha256"]:
        fail("nxextract.runtime_env_sha256 does not pin the runtime helper")
    if recipe_record["sha256"] != nx["recipe_sha256"]:
        fail("nxextract.recipe_sha256 does not pin extractor.json")

    record_targets = set(by_target)
    for declaration in config["dependencies"]:
        if declaration["provider"] == "package" and declaration["path"] not in record_targets:
            fail("package dependency provider path is absent: {}".format(
                declaration["path"]
            ))

    license_declaration = config.get("license")
    if license_declaration is not None:
        license_record = by_target.get(license_declaration["file"])
        if license_record is None or license_record["kind"] != "license-notice":
            fail("package.license.file must match a license-notice file entry: {}".format(
                license_declaration["file"]))
        if license_record.get("mode") != 0o644:
            fail("package.license.file must be staged with mode 0644")

    metadata = config["portmaster_metadata"]
    for field in ("port_json", "gameinfo_xml"):
        declaration = metadata[field]
        if declaration is None:
            continue
        record = by_target.get(declaration["path"])
        if record is None or record["kind"] != "portmaster-metadata":
            fail("{} must match a portmaster-metadata file entry".format(
                declaration["path"]
            ))
        if record["sha256"] != declaration["sha256"]:
            fail("PortMaster metadata pin mismatch for {}".format(declaration["path"]))
    for declaration in metadata["images"]:
        record = by_target.get(declaration["path"])
        if record is None or record["kind"] != "portmaster-image":
            fail("{} must match a portmaster-image file entry".format(
                declaration["path"]
            ))
        if record["sha256"] != declaration["sha256"]:
            fail("PortMaster image pin mismatch for {}".format(declaration["path"]))


def read_small_text(path, context, limit=16 * 1024 * 1024):
    if path.stat().st_size > limit:
        fail("{} is unexpectedly larger than {} bytes".format(context, limit))
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        fail("cannot read {} as UTF-8: {}".format(context, exc))


def shell_tokens(text, logical_path):
    try:
        lexer = shlex.shlex(text, posix=True, punctuation_chars=";&|()<>")
        lexer.whitespace_split = True
        lexer.commenters = "#"
        return list(lexer)
    except ValueError as exc:
        fail("cannot lex shell script {}: {}".format(logical_path, exc))


def validate_nxextract(records, config):
    by_target = {record["target"]: record for record in records}
    nx = config["nxextract"]
    nx_record = by_target.get(nx["path"])
    runner_record = by_target.get(nx["runner_path"])
    runtime_env_record = by_target.get(nx["runtime_env_path"])
    recipe_record = by_target.get(nx["recipe_path"])
    if nx_record is None or nx_record.get("kind") != "nxextract":
        fail("NXExtract path is absent or has the wrong inventory kind")
    if runner_record is None or runner_record.get("kind") != "nxextract-runner":
        fail("NXExtract runner is absent or has the wrong inventory kind")
    if (runtime_env_record is None or
            runtime_env_record.get("kind") != "nxextract-runtime-env"):
        fail("NXExtract runtime helper is absent or has the wrong inventory kind")
    if recipe_record is None or recipe_record.get("kind") != "nxextract-recipe":
        fail("NXExtract recipe is absent or has the wrong inventory kind")
    nx_text = read_small_text(nx_record["actual_path"], nx["path"])
    versions = NX_VERSION_RE.findall(nx_text)
    if len(versions) != 1:
        fail("{} must declare exactly one NXEXTRACT_VERSION".format(nx["path"]))
    actual_version = versions[0]
    if actual_version != nx["version"]:
        fail("NXExtract version mismatch: manifest {}, file {}".format(
            nx["version"], actual_version
        ))
    if version_lt(actual_version, nx["minimum_version"]):
        fail("NXExtract {} is below required {}".format(actual_version, nx["minimum_version"]))
    if sha256_file(nx_record["actual_path"]) != nx["sha256"]:
        fail("NXExtract content pin mismatch")

    runner_text = "\n".join(shell_tokens(
        read_small_text(runner_record["actual_path"], nx["runner_path"]),
        nx["runner_path"],
    ))
    for token in (
            "nxextract.py", "nxextract-runtime-env.sh", "extractor.json",
            "--recipe", "--game-dir"):
        if token not in runner_text:
            fail("NXExtract runner does not actively reference {}".format(token))
    if sha256_file(runner_record["actual_path"]) != nx["runner_sha256"]:
        fail("NXExtract runner content pin mismatch")
    if sha256_file(runtime_env_record["actual_path"]) != nx["runtime_env_sha256"]:
        fail("NXExtract runtime helper content pin mismatch")
    runtime_tokens = shell_tokens(read_small_text(
        runtime_env_record["actual_path"], nx["runtime_env_path"]
    ), nx["runtime_env_path"])
    runtime_text = "\n".join(runtime_tokens)
    has_exec_argv = any(
        runtime_tokens[index:index + 2] == ["exec", "$@"]
        for index in range(max(0, len(runtime_tokens) - 1))
    )
    if "NXEXTRACT_RUNTIME_ENV_ACTIVE" not in runtime_text or not has_exec_argv:
        fail("NXExtract runtime helper lacks its isolated re-entry contract")
    if sha256_file(recipe_record["actual_path"]) != nx["recipe_sha256"]:
        fail("NXExtract recipe content pin mismatch")
    recipe = require_object(
        load_json(recipe_record["actual_path"], "NXExtract recipe"),
        "NXExtract recipe",
    )
    if recipe.get("schema") != 1:
        fail("NXExtract recipe schema must be 1")
    require_string(recipe.get("id"), "NXExtract recipe id")
    for field in ("extract", "validate", "commit"):
        if not isinstance(recipe.get(field), list):
            fail("NXExtract recipe {} must be an array".format(field))


def exception_allowed(config, used, rule, target):
    key = (rule, target)
    if key not in config["exception_map"]:
        return False
    used.add(key)
    return True


def scan_text_for_private_data(path, logical_path):
    """Scan payload bytes for private host data, embedded secrets and funding advocacy."""
    try:
        with open(str(path), "rb") as handle:
            prefix = handle.read(4096)
            text_like = b"\0" not in prefix
            tail = b""
            chunk = prefix
            while chunk:
                data = tail + chunk
                if PRIVATE_PATH_RE.search(data):
                    fail("{} contains a private home path".format(logical_path))
                if text_like and ADVOCACY_RE.search(data):
                    fail("{} advertises funding/donations forbidden in public releases".format(
                        logical_path))
                if text_like:
                    match = IPV4_RE.search(data)
                    if match is not None:
                        octets = [int(part) for part in match.group(0).split(b".")]
                        if all(part <= 255 for part in octets):
                            fail("{} contains an IPv4 literal".format(logical_path))
                    if SECRET_LITERAL_RE.search(data):
                        fail("{} embeds a credential/secret literal".format(logical_path))
                    if HOST_LITERAL_RE.search(data):
                        fail("{} embeds a hostname literal".format(logical_path))
                tail = data[-512:]
                chunk = handle.read(256 * 1024)
    except OSError as exc:
        fail("cannot scan {}: {}".format(logical_path, exc))


def audit_script(path, logical_path, config, used_exceptions):
    text = read_small_text(path, logical_path)
    active_text = "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#")
    )
    shell = "bash" if text.splitlines() and "bash" in text.splitlines()[0] else "sh"
    process = subprocess.run(
        [shell, "-n", str(path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    if process.returncode != 0:
        fail("shell syntax error in {}: {}".format(logical_path, process.stderr.strip()))
    tokens = shell_tokens(text, logical_path)
    if DETACHED_RE.search(active_text) or any(
            token in ("setsid", "nohup") for token in tokens):
        fail("{} launches a detached process with setsid/nohup".format(logical_path))
    if FRONTEND_RE.search(active_text) or (
            any(token in ("systemctl", "killall", "pkill") for token in tokens) and
            any("emustation" in token.lower() or "emulationstation" in token.lower()
                for token in tokens)):
        fail("{} manages EmulationStation directly".format(logical_path))
    forced_driver = FORCED_DRIVER_RE.search(text) or any(
        re.match(r"^(?:SDL_(?:VIDEO|AUDIO)_?DRIVER|ALSOFT_DRIVERS)=", token,
                 re.IGNORECASE)
        for token in tokens
    )
    if forced_driver and not exception_allowed(
            config, used_exceptions, "adaptive-driver", logical_path):
        fail("{} forces a video/audio driver without an adaptive-driver exception".format(
            logical_path
        ))
    background = "&" in tokens
    if background:
        if (logical_path in config.get("canonical_bootstrap_paths", ()) and
                config.get("canonical_launcher_verified")):
            return
        if not exception_allowed(config, used_exceptions, "supervised-child", logical_path):
            fail("{} backgrounds a child without a supervised-child exception".format(
                logical_path
            ))
        supervision_contract = {
            "$!": any(token == "$!" or token.endswith("=$!") for token in tokens),
            "trap": "trap" in tokens,
            "wait": "wait" in tokens,
        }
        for supervision_token, present in supervision_contract.items():
            if not present:
                fail("{} supervised child contract lacks {}".format(
                    logical_path, supervision_token
                ))


def active_shell_text(text):
    return "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#")
    )


def verify_self_contained_wrapper(wrapper, wrapper_path, config, nx_arch):
    """Verify the 0.6.0 single-launcher shape and its golden-port guarantees."""
    if "# PORTMASTER: {}, {}".format(
            config["package_id"], PurePosixPath(wrapper_path).name
    ) not in wrapper:
        fail("self-contained launcher lacks its PORTMASTER identity")
    if re.search(r"@[A-Z0-9_]+@", wrapper):
        fail("self-contained launcher has unresolved template tokens")
    active = active_shell_text(wrapper)
    for token in ("control.txt", "get_controls", "pm_platform_helper",
                  "pm_finish", "flock -n 9", 'wait "$game_pid"',
                  "trap - INT TERM HUP", "printf '\\033c'"):
        if token not in active:
            fail("self-contained launcher is missing {}".format(token))
    if ('GAMEDIR="/$directory/ports/{}"'.format(config["package_id"])
            not in active):
        fail("self-contained launcher does not derive GAMEDIR from $directory")
    if re.search(r"(?:export\s+)?(?:SDL_VIDEODRIVER|SDL_AUDIODRIVER)\s*=",
                 active):
        fail("self-contained launcher forces an SDL backend")
    if nx_arch == "armv7":
        if 'PORT_32BIT="Y"' not in active:
            fail("armv7 launcher lacks literal PORT_32BIT")
    elif "PORT_32BIT" in active:
        fail("aarch64 launcher must not carry PORT_32BIT")


def verify_generated_launcher(records, config):
    by_target = {record["target"]: record for record in records}
    contract = config["launcher_contract"]
    config_record = by_target.get(contract["config_path"])
    if config_record is None or config_record.get("kind") != "nxbootstrap-config":
        fail("canonical nxbootstrap config is absent")
    if sha256_file(config_record["actual_path"]) != contract["config_sha256"]:
        fail("canonical nxbootstrap config content pin mismatch")

    manifest = require_object(
        load_json(config_record["actual_path"], "nxbootstrap config"),
        "nxbootstrap config",
    )
    nxport_keys = {
        "schema_version", "id", "title", "launcher_name", "architecture",
        "executable", "argument_mode", "home_mode", "nxextract",
        "required_files", "private_library_paths", "prepare_script",
        "required_capabilities", "enabled_quirks", "runtime_report",
    }
    unknown_nxport = sorted(set(manifest) - nxport_keys)
    if unknown_nxport:
        fail("nxbootstrap config has unknown field(s): {}".format(
            ", ".join(unknown_nxport)
        ))
    if set(manifest) != nxport_keys:
        missing = sorted(nxport_keys - set(manifest))
        fail("canonical nxbootstrap config is missing field(s): {}".format(
            ", ".join(missing)
        ))
    if manifest.get("schema_version") != NXPORT_SCHEMA_VERSION:
        fail("nxbootstrap config schema_version must be {}; regenerate legacy input".format(
            NXPORT_SCHEMA_VERSION
        ))
    if manifest.get("id") != config["package_id"]:
        fail("nxbootstrap config id does not match package.id")
    if manifest.get("launcher_name") != config["launcher"]:
        fail("nxbootstrap config launcher_name does not match package.launcher")
    nx_arch = manifest.get("architecture")
    if nx_arch not in ARCH_MACHINES:
        fail("nxbootstrap config architecture must be aarch64 or armv7 for a universal release")
    nx_executable = manifest.get("executable")
    if not isinstance(nx_executable, str):
        fail("nxbootstrap config executable is invalid")
    nx_executable = safe_relative(nx_executable, "nxbootstrap executable")
    title = require_string(manifest.get("title"), "nxbootstrap config title")
    argument_mode = manifest.get("argument_mode", "game-dir-and-passthrough")
    if argument_mode not in (
            "none", "passthrough", "game-dir", "game-dir-and-passthrough"):
        fail("nxbootstrap config argument_mode is invalid")
    home_mode = manifest.get("home_mode", "preserve")
    if home_mode not in ("preserve", "port"):
        fail("nxbootstrap config home_mode is invalid")
    nxextract_contract = require_object(
        manifest.get("nxextract"), "nxbootstrap config nxextract"
    )
    require_keys(nxextract_contract, ("mode", "version"),
                 "nxbootstrap config nxextract")
    if set(nxextract_contract) != {"mode", "version"}:
        fail("nxbootstrap config nxextract requires mode and version")
    nx_mode = nxextract_contract.get("mode")
    if nx_mode not in ("auto", "yes", "no"):
        fail("nxbootstrap config nxextract.mode is invalid")
    nx_version = require_string(
        nxextract_contract.get("version"),
        "nxbootstrap config nxextract.version",
    )
    if nx_version != "1.2.6" or nx_version != config["nxextract"]["version"]:
        fail("nxbootstrap config must pin the packaged NXExtract 1.2.6 set")
    required_files = manifest.get("required_files", [])
    if (not isinstance(required_files, list) or
            any(not isinstance(item, str) for item in required_files)):
        fail("nxbootstrap config required_files must be a string array")
    normalized_required = [
        safe_relative(item, "nxbootstrap required_files")
        for item in required_files
    ]
    if len(set(normalized_required)) != len(normalized_required):
        fail("nxbootstrap config required_files contains duplicates")
    if nx_executable not in normalized_required:
        fail("nxbootstrap config required_files omits its executable")
    private_paths = manifest.get("private_library_paths")
    if (not isinstance(private_paths, list) or
            any(not isinstance(item, str) for item in private_paths)):
        fail("nxbootstrap config private_library_paths must be a string array")
    normalized_private = [
        safe_relative(item, "nxbootstrap private_library_paths")
        for item in private_paths
    ]
    if len(set(normalized_private)) != len(normalized_private):
        fail("nxbootstrap config private_library_paths contains duplicates")
    prepare_script = manifest.get("prepare_script")
    if not isinstance(prepare_script, str):
        fail("nxbootstrap config prepare_script must be a string")
    if prepare_script:
        prepare_script = safe_relative(
            prepare_script, "nxbootstrap prepare_script"
        )
    if CAPABILITY_REGISTRY_ERROR is not None:
        fail(CAPABILITY_REGISTRY_ERROR)
    required_capabilities = manifest.get("required_capabilities")
    enabled_quirks = manifest.get("enabled_quirks")
    for values, label, pattern in (
            (required_capabilities, "required_capabilities", NXPORT_CAPABILITY_RE),
            (enabled_quirks, "enabled_quirks", NXPORT_QUIRK_RE)):
        if (not isinstance(values, list) or
                any(not isinstance(item, str) for item in values)):
            fail("nxbootstrap config {} must be a string array".format(label))
        if len(values) != len(set(values)):
            fail("nxbootstrap config {} contains duplicates".format(label))
        for value in values:
            if not pattern.fullmatch(value):
                fail("nxbootstrap config {} has an invalid name: {}".format(
                    label, value
                ))
            if ".device." in ".{}.".format(value):
                fail("nxbootstrap config {} selects a device by name".format(label))
            if (label == "required_capabilities" and
                    value not in NXPORT_CAPABILITY_IDS):
                fail("nxbootstrap config required_capabilities has an "
                     "unknown name: {}".format(value))
            if (label == "enabled_quirks" and
                    value not in NXPORT_QUIRK_IDS):
                fail("nxbootstrap config enabled_quirks has an "
                     "unknown name: {}".format(value))
    if required_capabilities != sorted(
            required_capabilities, key=NXPORT_CAPABILITY_ORDER.__getitem__):
        fail("nxbootstrap config required_capabilities is not in canonical "
             "registry order")
    runtime_report = manifest.get("runtime_report")
    if runtime_report not in ("log", "log-and-logo"):
        fail("nxbootstrap config runtime_report is invalid")
    executable_target = PurePosixPath(config["port_dir"], nx_executable).as_posix()
    executable_record = by_target.get(executable_target)
    if (executable_record is None or
            executable_record.get("kind") not in LINUX_ELF_KINDS):
        fail("nxbootstrap executable must be a packaged classified Linux ELF")
    if executable_record.get("architecture") != nx_arch:
        fail("nxbootstrap architecture does not match its executable ELF")

    expected_paths = list(config["launcher_chain"]) + [contract["config_path"]]
    expected_modes = [0o755] + [0o644] * (len(expected_paths) - 1)
    for logical_path, expected_mode in zip(expected_paths, expected_modes):
        record = by_target.get(logical_path)
        if record is None or record["mode"] != expected_mode:
            fail("{} mode must be {:04o} in the pinned launcher chain".format(
                logical_path, expected_mode
            ))

    if bootstrap_self_contained(contract["version"]):
        wrapper_path = config["launcher_chain"][0]
        wrapper = read_small_text(
            by_target[wrapper_path]["actual_path"], wrapper_path
        )
        verify_self_contained_wrapper(
            wrapper, wrapper_path, config, nx_arch
        )
        config["canonical_bootstrap_paths"] = (wrapper_path,)
        config["canonical_launcher_verified"] = True
        return

    wrapper_path, bootstrap_path = config["launcher_chain"]
    wrapper = read_small_text(by_target[wrapper_path]["actual_path"], wrapper_path)
    bootstrap = read_small_text(
        by_target[bootstrap_path]["actual_path"], bootstrap_path
    )
    canonical_bootstrap_paths = [bootstrap_path]
    compatibility_path = config["port_dir"] + "/nxbootstrap.sh"
    if bootstrap_path != compatibility_path:
        compatibility_record = by_target.get(compatibility_path)
        if (compatibility_record is None or
                compatibility_record.get("kind") != "script" or
                compatibility_record.get("mode") != 0o644):
            fail("canonical nxbootstrap.sh compatibility copy is absent or misclassified")
        if (sha256_file(compatibility_record["actual_path"]) !=
                sha256_file(by_target[bootstrap_path]["actual_path"])):
            fail("canonical and versioned nxbootstrap copies differ")
        canonical_bootstrap_paths.append(compatibility_path)
    config["canonical_bootstrap_paths"] = tuple(canonical_bootstrap_paths)

    deployment_id = None
    if bootstrap_path != compatibility_path:
        deployment_path = config["port_dir"] + "/nxdeployment.json"
        deployment_record = by_target.get(deployment_path)
        if (deployment_record is None or
                deployment_record.get("kind") != "payload" or
                deployment_record.get("mode") != 0o644):
            fail("nxdeployment.json receipt is absent or misclassified")
        deployment = require_object(
            load_json(deployment_record["actual_path"], "nxdeployment receipt"),
            "nxdeployment receipt",
        )
        if set(deployment) != {
                "bootstrap", "deployment_id", "launcher_name",
                "nxport_sha256", "port_id", "schema_version"}:
            fail("nxdeployment receipt fields are not canonical")
        bootstrap_receipt = require_object(
            deployment.get("bootstrap"), "nxdeployment bootstrap"
        )
        if set(bootstrap_receipt) != {"filename", "sha256", "version"}:
            fail("nxdeployment bootstrap fields are not canonical")
        bootstrap_sha256 = sha256_file(
            by_target[bootstrap_path]["actual_path"])
        expected_deployment_id = nxbootstrap_deployment_id(
            config["package_id"], config["launcher"], contract["version"],
            bootstrap_sha256, contract["config_sha256"],
        )
        expected_deployment = {
            "bootstrap": {
                "filename": PurePosixPath(bootstrap_path).name,
                "sha256": bootstrap_sha256,
                "version": contract["version"],
            },
            "deployment_id": expected_deployment_id,
            "launcher_name": config["launcher"],
            "nxport_sha256": contract["config_sha256"],
            "port_id": config["package_id"],
            "schema_version": 1,
        }
        if deployment != expected_deployment:
            fail("nxdeployment receipt does not match the launcher/bootstrap/config bytes")
        deployment_id = expected_deployment_id
        config["deployment_id"] = deployment_id

    def static_assignment(text, name, logical_path):
        prefix = name + "="
        matches = [token[len(prefix):] for token in shell_tokens(text, logical_path)
                   if token.startswith(prefix)]
        if len(matches) != 1:
            fail("{} must assign {} exactly once".format(logical_path, name))
        return matches[0]

    if ("# Generated by nxbootstrap." not in wrapper or
            "# PORTMASTER: {}, {}".format(
                config["package_id"], config["launcher"]
            ) not in wrapper):
        fail("top-level wrapper lacks its generated nxbootstrap identity")
    wrapper_tokens = shell_tokens(wrapper, wrapper_path)
    if not any(
            token == "try_port" and
            index + 1 < len(wrapper_tokens) and
            wrapper_tokens[index + 1].endswith("/$PORT_ID")
            for index, token in enumerate(wrapper_tokens)
    ):
        fail("top-level launcher does not discover the $PORT_ID directory")
    if any(token in wrapper for token in (
            "control.txt", "get_controls", "pm_platform_helper", "pm_finish")):
        fail("top-level wrapper is not thin")
    if static_assignment(wrapper, "PORT_ID", wrapper_path) != config["package_id"]:
        fail("top-level wrapper PORT_ID differs from nxport.json")
    if static_assignment(wrapper, "PORT_TITLE", wrapper_path) != title:
        fail("top-level wrapper PORT_TITLE differs from nxport.json")

    required_launcher_sequences = (
        ["source", "$NXPORT_GAME_DIR/$NXPORT_BOOTSTRAP_LIBRARY"],
        ["nxbootstrap_main", "$@"],
    )
    for sequence in required_launcher_sequences:
        if not any(
                wrapper_tokens[index:index + len(sequence)] == sequence
                for index in range(len(wrapper_tokens))):
            fail("launcher is missing canonical command: {}".format(
                " ".join(sequence)
            ))
    expected_launcher_assignments = {
        "NXPORT_ID": config["package_id"],
        "NXPORT_TITLE": title,
        "NXPORT_SCHEMA_VERSION": str(NXPORT_SCHEMA_VERSION),
        "NXPORT_ARCH": nx_arch,
        "NXPORT_EXECUTABLE": nx_executable,
        "NXPORT_ARGUMENT_MODE": argument_mode,
        "NXPORT_HOME_MODE": home_mode,
        "NXPORT_NXEXTRACT": nx_mode,
        "NXPORT_NXEXTRACT_VERSION": nx_version,
        "NXPORT_REQUIRED_FILES": "\n".join(normalized_required),
        "NXPORT_PRIVATE_LIBRARY_PATHS": "\n".join(normalized_private),
        "NXPORT_PREPARE_SCRIPT": prepare_script,
        "NXPORT_REQUIRED_CAPABILITIES": "\n".join(required_capabilities),
        "NXPORT_ENABLED_QUIRKS": "\n".join(enabled_quirks),
        "NXPORT_RUNTIME_REPORT": runtime_report,
    }
    if bootstrap_path != compatibility_path:
        expected_launcher_assignments.update({
            "NXPORT_BOOTSTRAP_LIBRARY": PurePosixPath(bootstrap_path).name,
            "NXPORT_BOOTSTRAP_SHA256": sha256_file(
                by_target[bootstrap_path]["actual_path"]),
            "NXPORT_BOOTSTRAP_VERSION": contract["version"],
            "NXPORT_MANIFEST_SHA256": contract["config_sha256"],
            "NXPORT_DEPLOYMENT_ID": deployment_id,
            "NXPORT_DEPLOYMENT_RECEIPT": read_small_text(
                deployment_record["actual_path"], deployment_path
            ),
        })
    for name, expected in expected_launcher_assignments.items():
        if static_assignment(wrapper, name, wrapper_path) != expected:
            fail("launcher {} differs from nxport.json".format(name))
    if nx_arch == "armv7" and "PORT_32BIT=Y" not in wrapper_tokens:
        fail("armv7 launcher lacks literal PORT_32BIT=Y")

    def function_body(name):
        match = re.search(
            r"^" + re.escape(name) + r"\(\) \{\n(.*?)^\}",
            bootstrap, re.MULTILINE | re.DOTALL,
        )
        if not match:
            fail("nxbootstrap lacks callable function {}".format(name))
        return active_shell_text(match.group(1))

    if "Shared pre-main lifecycle for PortMaster ports" not in bootstrap:
        fail("nxbootstrap lacks its generated library identity")
    version_matches = re.findall(
        r"^[ \t]*NXBOOTSTRAP_VERSION=([0-9]+(?:\.[0-9]+)+)[ \t]*$",
        bootstrap, re.MULTILINE,
    )
    if version_matches != [contract["version"]]:
        fail("nxbootstrap version does not match launcher_contract.version")
    load_body = function_body("nxbootstrap_load_portmaster")
    finish_body = function_body("nxbootstrap_finish_once")
    platform_body = function_body("nxbootstrap_platform_prepare")
    main_body = function_body("nxbootstrap_main")
    launch_body = function_body("nxbootstrap_launch")
    load_tokens = shell_tokens(load_body, bootstrap_path + ":nxbootstrap_load_portmaster")
    finish_tokens = shell_tokens(finish_body, bootstrap_path + ":nxbootstrap_finish_once")
    platform_tokens = shell_tokens(platform_body, bootstrap_path + ":nxbootstrap_platform_prepare")
    main_tokens = shell_tokens(main_body, bootstrap_path + ":nxbootstrap_main")
    launch_tokens = shell_tokens(launch_body, bootstrap_path + ":nxbootstrap_launch")
    if (not any(token.endswith("/control.txt") for token in load_tokens) or
            "get_controls" not in load_tokens):
        fail("nxbootstrap_load_portmaster lacks active PortMaster integration")
    if "pm_finish" not in finish_tokens:
        fail("nxbootstrap_finish_once lacks active pm_finish")
    if "pm_platform_helper" not in platform_tokens:
        fail("nxbootstrap_platform_prepare lacks active pm_platform_helper")
    for call in (
            "nxbootstrap_load_portmaster", "nxbootstrap_platform_prepare",
            "nxbootstrap_run_extractor", "nxbootstrap_launch"):
        if call not in main_tokens:
            fail("nxbootstrap_main does not reach {}".format(call))
    supervision_checks = {
        "$!": any(token == "$!" or token.endswith("=$!") for token in launch_tokens),
        "wait": "wait" in launch_tokens,
        "nxbootstrap_finish_once": "nxbootstrap_finish_once" in launch_tokens,
    }
    for supervision, present in supervision_checks.items():
        if not present:
            fail("nxbootstrap_launch lacks supervised child token {}".format(
                supervision
            ))
    config["canonical_launcher_verified"] = True


def validate_launcher_chain(records, config):
    by_target = {record["target"]: record for record in records}
    verify_generated_launcher(records, config)
    combined = []
    for index, chain_path in enumerate(config["launcher_chain"]):
        record = by_target.get(chain_path)
        expected_kind = "launcher" if index == 0 else "script"
        if record is None or record.get("kind") != expected_kind:
            fail("declared launcher chain path {} is absent or has wrong kind".format(
                chain_path
            ))
        text = active_shell_text(read_small_text(record["actual_path"], chain_path))
        combined.append(text)
        if index + 1 < len(config["launcher_chain"]):
            next_name = PurePosixPath(config["launcher_chain"][index + 1]).name
            if next_name not in text:
                fail("launcher chain {} does not reference next script {}".format(
                    chain_path, next_name
                ))
    if not bootstrap_self_contained(config["launcher_contract"]["version"]):
        if ("nxbootstrap_main" not in combined[0] or
                "source \"$NXPORT_GAME_DIR/$NXPORT_BOOTSTRAP_LIBRARY\"" not in
                combined[0]):
            fail("top-level PortMaster launcher must source nxbootstrap directly")
    all_active = "\n".join(combined)
    for token in ("control.txt", "get_controls", "pm_platform_helper", "pm_finish"):
        if token not in all_active:
            fail("declared PortMaster launcher chain is missing {}".format(token))


def collect_image_strings(value):
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        result = []
        for item in value:
            result.extend(collect_image_strings(item))
        return result
    if isinstance(value, dict):
        result = []
        for item in value.values():
            result.extend(collect_image_strings(item))
        return result
    return []


def validate_image_magic(path, logical_path):
    with open(str(path), "rb") as handle:
        prefix = handle.read(16)
    lower = logical_path.lower()
    if lower.endswith(".png") and not prefix.startswith(b"\x89PNG\r\n\x1a\n"):
        fail("PortMaster image {} is not a PNG".format(logical_path))
    if lower.endswith((".jpg", ".jpeg")) and not prefix.startswith(b"\xff\xd8"):
        fail("PortMaster image {} is not a JPEG".format(logical_path))
    if lower.endswith(".webp") and not (
            prefix.startswith(b"RIFF") and prefix[8:12] == b"WEBP"):
        fail("PortMaster image {} is not a WebP".format(logical_path))


def validate_portmaster_metadata(records, config, maximum_glibc):
    declarations = config["portmaster_metadata"]
    by_target = {record["target"]: record for record in records}
    inventory_paths = set(by_target)
    for field in ("port_json", "gameinfo_xml"):
        declaration = declarations[field]
        if declaration is None:
            continue
        record = by_target.get(declaration["path"])
        if record is None or record.get("kind") != "portmaster-metadata":
            fail("declared PortMaster metadata {} is absent or misclassified".format(
                declaration["path"]
            ))
        if sha256_file(record["actual_path"]) != declaration["sha256"]:
            fail("PortMaster metadata content pin mismatch for {}".format(
                declaration["path"]
            ))
    for declaration in declarations["images"]:
        record = by_target.get(declaration["path"])
        if record is None or record.get("kind") != "portmaster-image":
            fail("declared PortMaster image {} is absent or misclassified".format(
                declaration["path"]
            ))
        if sha256_file(record["actual_path"]) != declaration["sha256"]:
            fail("PortMaster image content pin mismatch for {}".format(
                declaration["path"]
            ))
        validate_image_magic(record["actual_path"], declaration["path"])

    port_json_declaration = declarations["port_json"]
    if port_json_declaration is not None:
        record = by_target[port_json_declaration["path"]]
        port_json = require_object(
            load_json(record["actual_path"], "PortMaster port.json"),
            "PortMaster port.json",
        )
        schema_version = port_json.get("version")
        if isinstance(schema_version, bool) or not isinstance(schema_version, int) or schema_version < 1:
            fail("PortMaster port.json has an invalid version")
        if port_json.get("name") != config["package_id"] + ".zip":
            fail("PortMaster port.json name must be {}.zip".format(config["package_id"]))
        items = port_json.get("items")
        if not isinstance(items, list):
            fail("PortMaster port.json items must be an array")
        normalized_items = []
        for index, item in enumerate(items):
            item, directory_hint = safe_top_level_item(
                item, "PortMaster port.json items[{}]".format(index)
            )
            is_file = item in inventory_paths
            is_directory = any(
                candidate.startswith(item + "/") for candidate in inventory_paths
            )
            if directory_hint and not is_directory:
                fail("PortMaster port.json item {} uses '/' but is not a directory".format(
                    item
                ))
            if not is_file and not is_directory:
                fail("PortMaster port.json item {} is absent from the package".format(item))
            if is_file and is_directory:
                fail("PortMaster package has a file/directory collision at {}".format(item))
            normalized_items.append(item)
        if len(set(normalized_items)) != len(normalized_items):
            fail("PortMaster port.json items contain duplicates")
        for required_item in (config["launcher"], config["port_dir"]):
            if required_item not in normalized_items:
                fail("PortMaster port.json items omit {}".format(required_item))
        attributes = require_object(port_json.get("attr"), "PortMaster port.json attr")
        require_string(attributes.get("title"), "PortMaster port.json attr.title")
        declared_arches = attributes.get("arch")
        if not isinstance(declared_arches, list):
            fail("PortMaster port.json attr.arch must be an array")
        expected_arches = set()
        for candidate in records:
            if candidate.get("kind") in LINUX_ELF_KINDS:
                expected_arches.add(
                    "armhf" if candidate.get("architecture") == "armv7" else
                    candidate.get("architecture")
                )
        if not expected_arches.issubset(set(declared_arches)):
            fail("PortMaster port.json attr.arch omits packaged Linux architecture(s)")
        min_glibc = attributes.get("min_glibc")
        if min_glibc not in (None, ""):
            validate_ceiling(min_glibc, "PortMaster port.json attr.min_glibc")
            if version_gt(min_glibc, config["ceiling"]):
                fail("PortMaster port.json min_glibc exceeds release ceiling")
            if maximum_glibc != "none" and version_lt(min_glibc, maximum_glibc):
                fail("PortMaster port.json min_glibc understates packaged ELF requirements")
        image_parent = PurePosixPath(port_json_declaration["path"]).parent
        for image_value in collect_image_strings(attributes.get("image")):
            if image_value.startswith(("http://", "https://")):
                continue
            if image_value.startswith("./"):
                image_value = image_value[2:]
            image_value = safe_relative(image_value, "PortMaster port.json image")
            image_path = PurePosixPath(image_parent, image_value).as_posix()
            if image_path not in inventory_paths:
                fail("PortMaster port.json references missing image {}".format(image_path))

    gameinfo_declaration = declarations["gameinfo_xml"]
    if gameinfo_declaration is not None:
        record = by_target[gameinfo_declaration["path"]]
        text = read_small_text(record["actual_path"], "PortMaster gameinfo.xml")
        if "<!DOCTYPE" in text.upper() or "<!ENTITY" in text.upper():
            fail("PortMaster gameinfo.xml cannot contain DTD/entities")
        try:
            root = ElementTree.fromstring(text)
        except ElementTree.ParseError as exc:
            fail("PortMaster gameinfo.xml is malformed: {}".format(exc))
        games = root.findall("game")
        if root.tag != "gameList" or len(games) != 1:
            fail("PortMaster gameinfo.xml must contain exactly one gameList/game")
        game = games[0]
        if game.findtext("path") != "./" + config["launcher"]:
            fail("PortMaster gameinfo.xml path does not match the launcher")
        require_string(game.findtext("name"), "PortMaster gameinfo.xml game/name")
        image_text = game.findtext("image")
        if image_text:
            image_value = image_text[2:] if image_text.startswith("./") else image_text
            image_path = safe_relative(image_value, "PortMaster gameinfo.xml image")
            if image_path not in inventory_paths:
                fail("PortMaster gameinfo.xml references missing image {}".format(image_path))


def validate_dependency_closure(elf_results, config):
    declarations = {
        (item["namespace"], item["architecture"], item["soname"]): item
        for item in config["dependencies"]
    }
    packaged = {}
    packaged_folded = {}
    by_path = {item["path"]: item for item in elf_results}
    for item in elf_results:
        if item["soname"] is None:
            continue
        key = (item["namespace"], item["architecture"], item["soname"])
        folded_key = (
            item["namespace"], item["architecture"],
            portable_path_key(item["soname"]),
        )
        if key in packaged:
            fail("duplicate packaged ELF provider for {}/{}/{}: {} and {}".format(
                key[0], key[1], key[2], packaged[key]["path"], item["path"]
            ))
        if folded_key in packaged_folded:
            fail("portable packaged SONAME collision: {} and {}".format(
                packaged_folded[folded_key]["soname"], item["soname"]
            ))
        packaged[key] = item
        packaged_folded[folded_key] = item

    used = set()
    for item in elf_results:
        for soname in item["needed"]:
            key = (item["namespace"], item["architecture"], soname)
            declaration = declarations.get(key)
            if declaration is None:
                fail("unresolved DT_NEEDED {} for {} ({}/{})".format(
                    soname, item["path"], item["namespace"], item["architecture"]
                ))
            used.add(key)
            package_provider = packaged.get(key)
            if declaration["provider"] == "package":
                if package_provider is None:
                    fail("dependency {}/{}/{} declares package provider but no packaged ELF has that SONAME".format(
                        *key
                    ))
                if declaration["path"] != package_provider["path"]:
                    fail("dependency {}/{}/{} package provider path must be {}".format(
                        key[0], key[1], key[2], package_provider["path"]
                    ))
            elif package_provider is not None:
                fail("dependency {}/{}/{} has duplicate providers: package {} and {}".format(
                    key[0], key[1], key[2], package_provider["path"],
                    declaration["provider"],
                ))

    for key, declaration in declarations.items():
        if declaration["provider"] == "package":
            provider = by_path.get(declaration["path"])
            if provider is None:
                fail("package dependency provider is not a classified ELF: {}".format(
                    declaration["path"]
                ))
            actual_key = (
                provider["namespace"], provider["architecture"], provider["soname"]
            )
            if actual_key != key:
                fail("package dependency provider {} does not define {}/{}/{}".format(
                    declaration["path"], key[0], key[1], key[2]
                ))
        if key not in used:
            fail("unused dependency provider declaration: {}/{}/{}".format(*key))


def audit_record_set(records, config):
    if shutil.which("readelf") is None:
        fail("GNU readelf is required")
    for record in records:
        elf = is_elf(record["actual_path"])
        if elf and record["kind"] not in ELF_KINDS:
            fail("ELF {} is unclassified (kind={})".format(
                record["target"], record["kind"]
            ))
        if not elf and record["kind"] in ELF_KINDS:
            fail("{} is classified as {} but is not an ELF".format(
                record["target"], record["kind"]
            ))
    validate_launcher_chain(records, config)
    used_exceptions = set()
    elf_results = []
    maximum = "none"
    for record in records:
        path = record["actual_path"]
        logical_path = record["target"]
        lower = logical_path.lower()
        parts = tuple(PurePosixPath(lower).parts)
        basename = parts[-1] if parts else lower
        if lower.endswith(FORBIDDEN_SUFFIXES):
            fail("forbidden release data (proprietary/temp/log suffix): {}".format(logical_path))
        if basename.startswith("core.") or basename in FORBIDDEN_BASENAMES:
            fail("forbidden release artifact: {}".format(logical_path))
        if any(part in FORBIDDEN_PATH_PARTS for part in parts):
            fail("private/temp/cache data cannot enter a release: {}".format(logical_path))
        scan_text_for_private_data(path, logical_path)
        if lower.endswith(".sh"):
            audit_script(path, logical_path, config, used_exceptions)

        elf = is_elf(path)
        if elf and record["kind"] not in ELF_KINDS:
            fail("ELF {} is unclassified (kind={})".format(logical_path, record["kind"]))
        if not elf and record["kind"] in ELF_KINDS:
            fail("{} is classified as {} but is not an ELF".format(
                logical_path, record["kind"]
            ))
        if not elf:
            continue
        info = elf_information(
            path,
            logical_path,
            record["kind"],
            record.get("architecture"),
            config["ceiling"],
            record.get("build_profile"),
            record.get("needed"),
            record.get("soname"),
            record.get("provenance"),
        )
        elf_results.append(info)
        if info["glibc_max"] != "none" and (
                maximum == "none" or version_gt(info["glibc_max"], maximum)):
            maximum = info["glibc_max"]

    unused = sorted(set(config["exception_map"]) - used_exceptions)
    if unused:
        fail("unused audit exception(s): {}".format(
            ", ".join("{}:{}".format(rule, path) for rule, path in unused)
        ))

    validate_dependency_closure(elf_results, config)
    validate_nxextract(records, config)
    validate_portmaster_metadata(records, config, maximum)
    return sorted(elf_results, key=lambda item: item["path"]), maximum


def records_at_sources(config):
    result = []
    for record in config["records"]:
        copied = dict(record)
        copied["actual_path"] = record["source"]
        result.append(copied)
    return result


def records_at_stage(config, stage):
    result = []
    for record in config["records"]:
        copied = dict(record)
        copied["actual_path"] = stage / PurePosixPath(record["target"])
        result.append(copied)
    return result


def validate_sources(config):
    results, maximum = audit_record_set(records_at_sources(config), config)
    return results, maximum


def json_bytes(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def write_bytes(path, data, mode, epoch):
    with open(str(path), "wb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())
    os.chmod(str(path), mode)
    os.utime(str(path), (epoch, epoch))


def rename_noreplace(source, destination):
    """Linux atomic rename with the no-replace guarantee the release gate needs."""
    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None:
        fail("host libc lacks renameat2; cannot guarantee no-overwrite staging")
    renameat2.argtypes = (
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
        ctypes.c_uint,
    )
    renameat2.restype = ctypes.c_int
    at_fdcwd = -100
    rename_noreplace_flag = 1
    result = renameat2(
        at_fdcwd, os.fsencode(str(source)), at_fdcwd,
        os.fsencode(str(destination)), rename_noreplace_flag,
    )
    if result == 0:
        return
    error = ctypes.get_errno()
    if error == errno.EEXIST:
        fail("destination appeared concurrently; refusing to overwrite: {}".format(
            destination
        ))
    if error in (errno.ENOSYS, errno.EINVAL, errno.EOPNOTSUPP):
        fail("host filesystem cannot guarantee atomic no-replace rename for {}".format(
            destination
        ))
    raise OSError(error, os.strerror(error), str(destination))


def unlink_if_same(path, reference_stat):
    try:
        current = os.stat(str(path), follow_symlinks=False)
    except FileNotFoundError:
        return
    if (current.st_dev, current.st_ino) == (reference_stat.st_dev, reference_stat.st_ino):
        os.unlink(str(path))


def fsync_directory(path):
    descriptor = os.open(str(path), os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def publish_archive_pair(archive_temp, checksum_temp, output, checksum_output):
    """Publish a pre-verified ZIP/checksum pair without ever replacing a path.

    The checksum link is installed first and rolled back if the ZIP name loses a
    race. Consequently a public ZIP is never visible without its matching hash.
    A per-output O_EXCL lock serializes nxrelease publishers; hard-link creation
    itself protects both final names against non-cooperating concurrent writers.
    """
    parent = output.parent
    checksum_stat = os.stat(str(checksum_temp), follow_symlinks=False)
    archive_stat = os.stat(str(archive_temp), follow_symlinks=False)
    lock = parent / ("." + output.name + ".nxrelease-publish.lock")
    try:
        lock_fd = os.open(str(lock), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        fail("publication lock already exists: {}".format(lock))
    lock_stat = os.fstat(lock_fd)
    checksum_published = False
    archive_published = False
    try:
        os.close(lock_fd)
        if output.exists() or output.is_symlink():
            fail("archive output appeared concurrently: {}".format(output))
        if checksum_output.exists() or checksum_output.is_symlink():
            fail("archive checksum output appeared concurrently: {}".format(
                checksum_output
            ))
        try:
            os.link(str(checksum_temp), str(checksum_output))
            checksum_published = True
            os.link(str(archive_temp), str(output))
            archive_published = True
            fsync_directory(parent)
        except FileExistsError:
            fail("release destination appeared concurrently; nothing was overwritten")
        except OSError:
            raise
    except BaseException:
        if archive_published:
            unlink_if_same(output, archive_stat)
        if checksum_published:
            unlink_if_same(checksum_output, checksum_stat)
        fsync_directory(parent)
        raise
    finally:
        unlink_if_same(lock, lock_stat)


def manifest_lines(stage, checksum_path):
    paths = []
    for path in stage.rglob("*"):
        if path.is_symlink():
            fail("stage contains symlink {}".format(path.relative_to(stage)))
        if path.is_file():
            relative = path.relative_to(stage).as_posix()
            if relative != checksum_path:
                paths.append(relative)
        elif not path.is_dir():
            fail("stage contains non-regular path {}".format(path.relative_to(stage)))
    return ["{}  {}\n".format(sha256_file(stage / PurePosixPath(path)), path) for path in sorted(paths)]


def create_metadata(config, records, elf_results, maximum):
    inventory = []
    for record in records:
        inventory.append({
            "kind": record["kind"],
            "mode": "{:04o}".format(record["mode"]),
            "path": record["target"],
            "sha256": sha256_file(record["actual_path"]),
        })
    return {
        "audit_exceptions": [
            {"path": path, "reason": reason, "rule": rule}
            for (rule, path), reason in sorted(config["exception_map"].items())
        ],
        "archive": {
            "compression": config["compression"],
            "source_date_epoch": config["epoch"],
        },
        "dependencies": list(config["dependencies"]),
        "elf_audit": {
            "count": len(elf_results),
            "files": elf_results,
            "max_glibc_seen": maximum,
            "public_ceiling": config["ceiling"],
        },
        "input_manifest_sha256": config["manifest_sha256"],
        "inventory": inventory,
        "nxextract": dict(config["nxextract"]),
        "package": {
            "id": config["package_id"],
            "launcher": config["launcher"],
            "launcher_chain": list(config["launcher_chain"]),
            "launcher_contract": dict(config["launcher_contract"]),
            "port_dir": config["port_dir"],
            "profile": PROFILE,
            "version": config["package_version"],
            "license": config.get("license"),
        },
        "portmaster_metadata": config["portmaster_metadata"],
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": "nxrelease", "version": TOOL_VERSION},
}


def create_sbom(config, records, elf_results):
    """Project a deterministic CycloneDX 1.5 BOM from the audited inventory.

    The serial number and metadata timestamp are derived from the pinned
    package identity and source_date_epoch, so the SBOM is byte-reproducible
    with the rest of the release.  The BOM is an authoritative projection of
    NXRELEASE-METADATA; verify_stage/verify_archive re-check its coverage and
    per-file hashes against the inventory.
    """
    package_id = config["package_id"]
    version = config["package_version"]
    application_ref = "pkg:portmaster/{}@{}".format(package_id, version)
    elf_by_path = {item["path"]: item for item in elf_results}
    components = []
    depends_on = []
    for record in records:
        target = record["target"]
        digest = sha256_file(record["actual_path"])
        component = {
            "type": "application" if record["kind"] == "launcher" else "file",
            "bom-ref": "file:" + target,
            "name": PurePosixPath(target).name,
            "hashes": [{"alg": "SHA-256", "content": digest}],
            "properties": [
                {"name": "nxrelease:path", "value": target},
                {"name": "nxrelease:kind", "value": record["kind"]},
                {"name": "nxrelease:mode", "value": "{:04o}".format(record["mode"])},
            ],
        }
        elf = elf_by_path.get(target)
        if elf is not None:
            component["purl"] = "pkg:generic/{}@{}".format(
                PurePosixPath(target).name, version)
            for prop_name, prop_value in (
                    ("nxrelease:architecture", elf["architecture"]),
                    ("nxrelease:build_profile", elf["build_profile"] or ""),
                    ("nxrelease:glibc_max", elf["glibc_max"]),
                    ("nxrelease:interpreter", elf["interpreter"]),
                    ("nxrelease:soname", elf["soname"] or ""),
                    ("nxrelease:provenance", elf["provenance"] or "")):
                component["properties"].append(
                    {"name": prop_name, "value": prop_value})
        components.append(component)
        depends_on.append("file:" + target)
    components.sort(key=lambda item: item["bom-ref"])
    depends_on.sort()
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "serialNumber": "urn:uuid:" + str(
            uuid.uuid5(uuid.NAMESPACE_URL, "nxrelease/" + application_ref)),
        "metadata": {
            "timestamp": datetime.fromtimestamp(
                config["epoch"], timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "tools": [{"vendor": "NextOS", "name": "nxrelease",
                       "version": TOOL_VERSION}],
            "component": {
                "type": "application",
                "bom-ref": application_ref,
                "name": package_id,
                "version": version,
                "purl": application_ref,
            },
        },
        "components": components,
        "dependencies": [{"ref": application_ref, "dependsOn": depends_on}],
    }


def validate_sbom(value, inventory, context="release SBOM"):
    value = require_object(value, context)
    require_keys(
        value,
        ("bomFormat", "specVersion", "version", "serialNumber", "metadata",
         "components", "dependencies"),
        context,
    )
    if value.get("bomFormat") != "CycloneDX":
        fail("{} bomFormat must be CycloneDX".format(context))
    if value.get("specVersion") != "1.5":
        fail("{} specVersion must be 1.5".format(context))
    if value.get("version") != 1:
        fail("{} version must be 1".format(context))
    serial = require_string(value.get("serialNumber"), context + ".serialNumber")
    if not serial.startswith("urn:uuid:"):
        fail("{} serialNumber must be a urn:uuid reference".format(context))
    metadata = require_object(value.get("metadata"), context + ".metadata")
    require_keys(metadata, ("timestamp", "tools", "component"), context + ".metadata")
    root_component = require_object(
        metadata.get("component"), context + ".metadata.component")
    require_keys(root_component, ("type", "bom-ref", "name", "version", "purl"),
                 context + ".metadata.component")
    if root_component.get("type") != "application":
        fail("{} root component must be an application".format(context))
    components = value.get("components")
    if not isinstance(components, list):
        fail("{} components must be an array".format(context))
    covered = {}
    for index, entry in enumerate(components):
        entry_context = "{}.components[{}]".format(context, index)
        entry = require_object(entry, entry_context)
        require_keys(
            entry, ("type", "bom-ref", "name", "hashes", "properties", "purl"),
            entry_context,
        )
        bom_ref = require_string(entry.get("bom-ref"), entry_context + ".bom-ref")
        if not bom_ref.startswith("file:"):
            continue
        path = safe_relative(bom_ref[len("file:"):], entry_context + ".bom-ref path")
        hashes = entry.get("hashes")
        if not isinstance(hashes, list) or len(hashes) != 1:
            fail("{} must carry exactly one hash".format(entry_context))
        digest_entry = require_object(hashes[0], entry_context + ".hashes[0]")
        if digest_entry.get("alg") != "SHA-256":
            fail("{} hash algorithm must be SHA-256".format(entry_context))
        digest = parse_sha256(
            digest_entry.get("content"), entry_context + ".hash content")
        if path in covered:
            fail("{} covers {} more than once".format(context, path))
        covered[path] = digest
    if set(covered) != set(inventory):
        missing = sorted(set(inventory) - set(covered))
        extra = sorted(set(covered) - set(inventory))
        fail("{} coverage differs from inventory (missing={}, extra={})".format(
            context, missing, extra))
    for path, item in inventory.items():
        if covered[path] != item["sha256"]:
            fail("{} hash mismatch for {}".format(context, path))


def copy_record_to_stage(record, target, epoch):
    """Copy one already-audited input and close the validate-to-copy race."""
    shutil.copyfile(str(record["source"]), str(target))
    copied_hash = sha256_file(target)
    if copied_hash != record["sha256"]:
        fail("source changed between validation and staging: {}".format(
            record["target"]
        ))
    source_hash = sha256_file(record["source"])
    if source_hash != record["sha256"]:
        fail("source changed while staging: {}".format(record["target"]))
    if (record.get("expected_sha") is not None and
            copied_hash != record["expected_sha"]):
        fail("staged content no longer matches manifest pin: {}".format(
            record["target"]
        ))
    os.chmod(str(target), record["mode"])
    os.utime(str(target), (epoch, epoch))


def stage_release(config, destination):
    if sha256_file(config["manifest_path"]) != config["manifest_sha256"]:
        fail("input manifest changed between validation and staging")
    destination_input = Path(destination)
    if destination_input.exists() or destination_input.is_symlink():
        fail("stage destination already exists: {}".format(destination_input))
    destination = destination_input.resolve()
    parent = destination.parent
    if not parent.is_dir():
        fail("stage parent does not exist: {}".format(parent))
    temporary = Path(tempfile.mkdtemp(prefix=".nxrelease-stage-", dir=str(parent)))
    try:
        for record in config["records"]:
            target = temporary / PurePosixPath(record["target"])
            target.parent.mkdir(parents=True, exist_ok=True)
            copy_record_to_stage(record, target, config["epoch"])
        stage_records = records_at_stage(config, temporary)
        elf_results, maximum = audit_record_set(stage_records, config)
        metadata = create_metadata(config, stage_records, elf_results, maximum)
        metadata_name, checksum_name, sbom_name = internal_paths(config["port_dir"])
        metadata_target = temporary / PurePosixPath(metadata_name)
        sbom_target = temporary / PurePosixPath(sbom_name)
        checksum_target = temporary / PurePosixPath(checksum_name)
        metadata_target.parent.mkdir(parents=True, exist_ok=True)
        write_bytes(metadata_target, json_bytes(metadata), 0o644, config["epoch"])
        sbom = create_sbom(config, stage_records, elf_results)
        write_bytes(sbom_target, json_bytes(sbom), 0o644, config["epoch"])
        checksum_data = "".join(
            manifest_lines(temporary, checksum_name)
        ).encode("utf-8")
        write_bytes(checksum_target, checksum_data, 0o644, config["epoch"])
        verify_stage(temporary, requested_ceiling=config["ceiling"])
        rename_noreplace(temporary, destination)
        fsync_directory(parent)
        temporary = None
    finally:
        if temporary is not None and temporary.exists():
            shutil.rmtree(str(temporary))
    return destination


def validate_internal_metadata(metadata, requested_ceiling=None):
    metadata = require_object(metadata, "release metadata")
    require_keys(
        metadata,
        ("archive", "audit_exceptions", "dependencies", "elf_audit", "input_manifest_sha256", "inventory", "nxextract", "package", "portmaster_metadata", "schema_version", "tool"),
        "release metadata",
    )
    if metadata.get("schema_version") != SCHEMA_VERSION:
        fail("release metadata schema_version is unsupported")
    package = require_object(metadata.get("package"), "release metadata package")
    require_keys(package, ("id", "launcher", "launcher_chain", "launcher_contract", "port_dir", "profile", "version", "license"), "release metadata package")
    if package.get("profile") != PROFILE:
        fail("archive is not a universal PortMaster profile")
    parse_sha256(metadata.get("input_manifest_sha256"), "metadata input_manifest_sha256")
    launcher = safe_relative(package.get("launcher"), "metadata package.launcher")
    port_dir = safe_relative(package.get("port_dir"), "metadata package.port_dir")
    package_id = require_string(package.get("id"), "metadata package.id")
    if not PACKAGE_ID_RE.match(package_id):
        fail("metadata package.id is not portable")
    if "/" in launcher or "/" in port_dir:
        fail("metadata package layout is invalid")
    if package_id != port_dir:
        fail("metadata package.port_dir must equal package.id")

    launcher_contract_value = require_object(
        package.get("launcher_contract"), "metadata package.launcher_contract"
    )
    require_keys(
        launcher_contract_value,
        ("config_path", "config_sha256", "generator", "version"),
        "metadata package.launcher_contract",
    )
    if launcher_contract_value.get("generator") != "nxbootstrap":
        fail("metadata launcher generator must be nxbootstrap")
    launcher_contract = {
        "config_path": safe_relative(
            launcher_contract_value.get("config_path"),
            "metadata launcher config_path",
        ),
        "config_sha256": parse_sha256(
            launcher_contract_value.get("config_sha256"),
            "metadata launcher config_sha256",
        ),
        "generator": "nxbootstrap",
        "version": require_string(
            launcher_contract_value.get("version"),
            "metadata launcher version",
        ),
    }
    version_tuple(launcher_contract["version"], "metadata launcher version")

    launcher_chain = package.get("launcher_chain")
    if not isinstance(launcher_chain, list) or len(launcher_chain) not in (1, 2):
        fail("metadata launcher_chain must contain launcher and nxbootstrap")
    normalized_chain = [
        safe_relative(path, "metadata launcher_chain[{}]".format(index))
        for index, path in enumerate(launcher_chain)
    ]
    if bootstrap_self_contained(launcher_contract["version"]):
        expected_chain = [launcher]
    else:
        expected_chain = [
            launcher,
            port_dir + "/" + nxbootstrap_script_name(
                launcher_contract["version"]),
        ]
    if normalized_chain != expected_chain:
        fail("metadata launcher_chain is not canonical")
    if launcher_contract["config_path"] != port_dir + "/nxport.json":
        fail("metadata launcher config_path is not canonical")
    metadata_license = package.get("license")
    if metadata_license is None:
        fail("metadata package.license is required for a public release")
    metadata_license = require_object(
        metadata_license, "metadata package.license")
    require_keys(
        metadata_license, ("spdx_id", "source_url", "file"),
        "metadata package.license")
    license_spdx = require_string(
        metadata_license.get("spdx_id"), "metadata package.license.spdx_id")
    license_source = require_string(
        metadata_license.get("source_url"),
        "metadata package.license.source_url")
    license_file = safe_relative(
        metadata_license.get("file"), "metadata package.license.file")
    if not license_file.startswith(port_dir + "/"):
        fail("metadata package.license.file must live inside package.port_dir")
    reject_private_literal(license_source, "metadata package.license.source_url")
    reject_private_literal(license_spdx, "metadata package.license.spdx_id")
    metadata_license = {
        "file": license_file,
        "source_url": license_source,
        "spdx_id": license_spdx,
    }

    archive = require_object(metadata.get("archive"), "release metadata archive")
    require_keys(archive, ("compression", "source_date_epoch"), "release metadata archive")
    compression = archive.get("compression")
    if compression not in ("deflated", "stored"):
        fail("metadata archive compression is invalid")
    epoch = archive.get("source_date_epoch")
    if isinstance(epoch, bool) or not isinstance(epoch, int):
        fail("metadata source_date_epoch is invalid")

    elf_audit = require_object(metadata.get("elf_audit"), "release metadata elf_audit")
    require_keys(elf_audit, ("count", "files", "max_glibc_seen", "public_ceiling"), "release metadata elf_audit")
    ceiling = validate_ceiling(elf_audit.get("public_ceiling"), "metadata GLIBC ceiling")
    if requested_ceiling is not None:
        ceiling = minimum_version(ceiling, validate_ceiling(requested_ceiling, "--max-glibc"))

    inventory = metadata.get("inventory")
    if not isinstance(inventory, list) or not inventory:
        fail("release metadata inventory must be non-empty")
    inventory_map = {}
    folded = {}
    inventory_parent_dirs = set()
    folded_inventory_parent_dirs = set()
    for index, item in enumerate(inventory):
        context = "metadata inventory[{}]".format(index)
        item = require_object(item, context)
        require_keys(item, ("kind", "mode", "path", "sha256"), context)
        target = safe_relative(item.get("path"), context + ".path")
        kind = require_string(item.get("kind"), context + ".kind")
        if kind not in ALLOWED_KINDS:
            fail("{} has invalid kind".format(context))
        mode = item.get("mode")
        if mode not in ("0644", "0755"):
            fail("{} has invalid mode".format(context))
        digest = parse_sha256(item.get("sha256"), context + ".sha256")
        portable_key = portable_path_key(target)
        if target in inventory_map or portable_key in folded:
            fail("metadata inventory has duplicate/case collision at {}".format(target))
        parts = PurePosixPath(target).parts
        parents = {
            PurePosixPath(*parts[:cut]).as_posix()
            for cut in range(1, len(parts))
        }
        folded_parents = {portable_path_key(parent) for parent in parents}
        if any(parent in inventory_map for parent in parents) or target in inventory_parent_dirs:
            fail("metadata inventory has a file/directory collision at {}".format(target))
        if (any(parent in folded for parent in folded_parents) or
                portable_key in folded_inventory_parent_dirs):
            fail("metadata inventory has a portable file/directory collision at {}".format(
                target
            ))
        inventory_map[target] = {"kind": kind, "mode": int(mode, 8), "sha256": digest}
        inventory_parent_dirs.update(parents)
        folded_inventory_parent_dirs.update(folded_parents)
        folded[portable_key] = target

    validate_public_shell_layout(
        inventory_map, launcher, port_dir, "release metadata inventory"
    )

    dependencies = validate_dependencies_manifest(
        metadata.get("dependencies"), port_dir
    )
    if metadata.get("dependencies") != dependencies:
        fail("release metadata dependencies are not canonical/sorted")

    exceptions = metadata.get("audit_exceptions")
    if not isinstance(exceptions, list):
        fail("release metadata audit_exceptions must be an array")
    exception_map = {}
    for index, exception in enumerate(exceptions):
        context = "metadata audit_exceptions[{}]".format(index)
        exception = require_object(exception, context)
        require_keys(exception, ("path", "reason", "rule"), context)
        rule = require_string(exception.get("rule"), context + ".rule")
        if rule not in EXCEPTION_RULES:
            fail("{} has unsupported rule".format(context))
        target = safe_relative(exception.get("path"), context + ".path")
        reason = require_string(exception.get("reason"), context + ".reason")
        if len(reason) < 16:
            fail("{} reason lacks concrete evidence".format(context))
        reject_private_literal(reason, context + ".reason")
        key = (rule, target)
        if key in exception_map:
            fail("duplicate metadata audit exception for {}".format(target))
        exception_map[key] = reason

    nx = require_object(metadata.get("nxextract"), "release metadata nxextract")
    require_keys(nx, (
        "minimum_version", "path", "runner_path", "runner_sha256",
        "runtime_env_path", "runtime_env_sha256", "recipe_path",
        "recipe_sha256", "sha256", "version",
    ), "release metadata nxextract")
    nx_config = {
        "minimum_version": require_string(nx.get("minimum_version"), "metadata nxextract.minimum_version"),
        "path": safe_relative(nx.get("path"), "metadata nxextract.path"),
        "runner_path": safe_relative(nx.get("runner_path"), "metadata nxextract.runner_path"),
        "runner_sha256": parse_sha256(nx.get("runner_sha256"), "metadata nxextract.runner_sha256"),
        "runtime_env_path": safe_relative(nx.get("runtime_env_path"), "metadata nxextract.runtime_env_path"),
        "runtime_env_sha256": parse_sha256(nx.get("runtime_env_sha256"), "metadata nxextract.runtime_env_sha256"),
        "recipe_path": safe_relative(nx.get("recipe_path"), "metadata nxextract.recipe_path"),
        "recipe_sha256": parse_sha256(nx.get("recipe_sha256"), "metadata nxextract.recipe_sha256"),
        "sha256": parse_sha256(nx.get("sha256"), "metadata nxextract.sha256"),
        "version": require_string(nx.get("version"), "metadata nxextract.version"),
    }
    version_tuple(nx_config["version"], "metadata nxextract.version")
    version_tuple(nx_config["minimum_version"], "metadata nxextract.minimum_version")
    if version_lt(nx_config["minimum_version"], NXEXTRACT_FLOOR):
        fail("archived NXExtract minimum is below tool floor {}".format(NXEXTRACT_FLOOR))
    if version_lt(nx_config["version"], nx_config["minimum_version"]):
        fail("archived NXExtract version is below its minimum")
    expected_nx = {
        "path": (port_dir + "/nxextract/nxextract.py", "nxextract", "sha256"),
        "runner_path": (port_dir + "/nxextract/run-extractor.sh", "nxextract-runner", "runner_sha256"),
        "runtime_env_path": (port_dir + "/nxextract/nxextract-runtime-env.sh", "nxextract-runtime-env", "runtime_env_sha256"),
        "recipe_path": (port_dir + "/extractor.json", "nxextract-recipe", "recipe_sha256"),
    }
    for field, (expected_path, expected_kind, hash_field) in expected_nx.items():
        if nx_config[field] != expected_path:
            fail("archived NXExtract {} is not canonical".format(field))
        inventory_item = inventory_map.get(expected_path)
        if inventory_item is None or inventory_item["kind"] != expected_kind:
            fail("archived NXExtract {} has the wrong inventory kind".format(field))
        if inventory_item["mode"] != 0o644:
            fail("archived NXExtract {} must have mode 0644".format(field))
        if inventory_item["sha256"] != nx_config[hash_field]:
            fail("archived NXExtract {} pin differs from inventory".format(field))

    launcher_config_item = inventory_map.get(launcher_contract["config_path"])
    if (launcher_config_item is None or
            launcher_config_item["kind"] != "nxbootstrap-config" or
            launcher_config_item["sha256"] != launcher_contract["config_sha256"]):
        fail("archived nxbootstrap config is absent, misclassified or unpinned")

    if not launcher.lower().endswith(".sh"):
        fail("metadata launcher is not a .sh file")
    if launcher == port_dir:
        fail("metadata launcher collides with port_dir")
    if epoch < 315532800 or epoch > 4354819198:
        fail("metadata source_date_epoch is outside the ZIP range")
    inventory_paths = [item.get("path") for item in inventory]
    if inventory_paths != sorted(inventory_paths):
        fail("release metadata inventory is not sorted")

    tool = require_object(metadata.get("tool"), "release metadata tool")
    require_keys(tool, ("name", "version"), "release metadata tool")
    if tool.get("name") != "nxrelease":
        fail("release metadata tool name is invalid")
    require_string(tool.get("version"), "release metadata tool.version")
    portmaster_metadata = validate_portmaster_metadata_manifest(
        metadata.get("portmaster_metadata"), port_dir
    )

    if license_file is not None:
        license_item = inventory_map.get(license_file)
        if license_item is None or license_item["kind"] != "license-notice":
            fail("metadata package.license.file is absent or not a license-notice: {}".format(
                license_file))

    return {
        "ceiling": ceiling,
        "compression": compression,
        "dependencies": dependencies,
        "elf_audit": elf_audit,
        "epoch": epoch,
        "exception_map": exception_map,
        "inventory": inventory_map,
        "license": metadata_license,
        "launcher": launcher,
        "launcher_chain": normalized_chain,
        "launcher_contract": launcher_contract,
        "nxextract": nx_config,
        "package_id": package_id,
        "package_version": require_string(package.get("version"), "metadata package.version"),
        "port_dir": port_dir,
        "portmaster_metadata": portmaster_metadata,
    }


def discover_stage_internal_paths(stage):
    candidates = [
        path for path in stage.glob(
            "*/{}/{}".format(INTERNAL_DIRNAME, METADATA_BASENAME)
        ) if path.is_file() and not path.is_symlink()
    ]
    if len(candidates) != 1:
        fail("stage must contain exactly one <port>/{}/{}".format(
            INTERNAL_DIRNAME, METADATA_BASENAME
        ))
    metadata_path = candidates[0]
    ensure_no_symlink(metadata_path, stage, "release metadata path")
    relative = metadata_path.relative_to(stage)
    port_dir = relative.parts[0]
    metadata_name, checksum_name, sbom_name = internal_paths(port_dir)
    checksum_path = stage / PurePosixPath(checksum_name)
    if checksum_path.is_symlink() or not checksum_path.is_file():
        fail("stage lacks {}".format(checksum_name))
    ensure_no_symlink(checksum_path, stage, "release checksum path")
    sbom_path = stage / PurePosixPath(sbom_name)
    if sbom_path.is_symlink() or not sbom_path.is_file():
        fail("stage lacks {}".format(sbom_name))
    ensure_no_symlink(sbom_path, stage, "release SBOM path")
    return port_dir, metadata_name, checksum_name, sbom_name


def parse_checksum_manifest(stage, checksum_name):
    path = stage / PurePosixPath(checksum_name)
    text = read_small_text(path, checksum_name)
    parsed = {}
    lines = text.splitlines()
    if not lines:
        fail("{} is empty".format(checksum_name))
    for line in lines:
        match = re.match(r"^([0-9a-f]{64})  (.+)$", line)
        if not match:
            fail("{} contains a malformed line".format(checksum_name))
        digest, target = match.groups()
        target = safe_relative(
            target, checksum_name + " path", allow_internal=True
        )
        if target == checksum_name:
            fail("{} cannot hash itself".format(checksum_name))
        if target in parsed:
            fail("{} contains duplicate path {}".format(checksum_name, target))
        parsed[target] = digest
    if list(parsed) != sorted(parsed):
        fail("{} is not sorted".format(checksum_name))
    return parsed


def verify_stage(stage, requested_ceiling=None):
    stage_input = Path(stage)
    if stage_input.is_symlink() or not stage_input.is_dir():
        fail("stage is missing, not a directory, or a symlink: {}".format(stage_input))
    stage = stage_input.resolve()
    discovered_port_dir, metadata_name, checksum_name, sbom_name = discover_stage_internal_paths(stage)
    metadata_path = stage / PurePosixPath(metadata_name)
    metadata = load_json(metadata_path, "release metadata")
    config = validate_internal_metadata(metadata, requested_ceiling=requested_ceiling)
    if config["port_dir"] != discovered_port_dir:
        fail("release metadata port_dir does not own its .nxrelease directory")
    sbom = load_json(stage / PurePosixPath(sbom_name), "release SBOM")
    validate_sbom(sbom, config["inventory"])
    sbom_mode = stat.S_IMODE((stage / PurePosixPath(sbom_name)).stat().st_mode)
    if sbom_mode != 0o644:
        fail("{} must have mode 0644".format(sbom_name))
    checksums = parse_checksum_manifest(stage, checksum_name)

    actual_paths = []
    folded = {}
    for path in stage.rglob("*"):
        relative = path.relative_to(stage).as_posix()
        if path.is_symlink():
            fail("stage contains symlink {}".format(relative))
        if path.is_file():
            portable_key = portable_path_key(relative)
            if portable_key in folded:
                fail("stage has case-insensitive collision: {} and {}".format(
                    folded[portable_key], relative
                ))
            folded[portable_key] = relative
            actual_paths.append(relative)
        elif not path.is_dir():
            fail("stage contains non-regular path {}".format(relative))
    expected_hashed = sorted(path for path in actual_paths if path != checksum_name)
    if sorted(checksums) != expected_hashed:
        fail("{} does not cover the stage exactly".format(checksum_name))
    for target in expected_hashed:
        actual = sha256_file(stage / PurePosixPath(target))
        if actual != checksums[target]:
            fail("{} verification failed for {}".format(checksum_name, target))
    internal_names = frozenset((metadata_name, checksum_name, sbom_name))
    for internal_name in internal_names:
        internal_mode = stat.S_IMODE((stage / internal_name).stat().st_mode)
        if internal_mode != 0o644:
            fail("{} must have mode 0644".format(internal_name))

    payload_paths = set(actual_paths) - internal_names
    if payload_paths != set(config["inventory"]):
        fail("release metadata inventory does not match staged payload")
    records = []
    elf_metadata = {}
    files = config["elf_audit"].get("files")
    if not isinstance(files, list):
        fail("metadata elf_audit.files must be an array")
    for item in files:
        item = require_object(item, "metadata ELF entry")
        require_keys(item, ("architecture", "build_id", "build_profile", "class", "cxxabi_max", "data", "elf_type", "flags", "glibc_max", "glibcxx_max", "interpreter", "kind", "machine", "namespace", "needed", "path", "provenance", "sha256", "soname"), "metadata ELF entry")
        target = safe_relative(item.get("path"), "metadata ELF path")
        kind = require_string(item.get("kind"), "metadata ELF kind")
        if kind not in ELF_KINDS:
            fail("metadata ELF {} has an invalid kind".format(target))
        architecture = require_string(
            item.get("architecture"), "metadata ELF architecture"
        )
        if architecture not in ARCH_MACHINES:
            fail("metadata ELF {} has an invalid architecture".format(target))
        if item.get("namespace") != dependency_namespace(kind):
            fail("metadata ELF {} has an invalid dependency namespace".format(target))
        needed = item.get("needed")
        if not isinstance(needed, list):
            fail("metadata ELF {} needed must be an array".format(target))
        normalized_needed = [
            validate_soname(value, "metadata ELF {} needed".format(target))
            for value in needed
        ]
        if normalized_needed != sorted(set(normalized_needed)):
            fail("metadata ELF {} needed is not sorted/unique".format(target))
        if len({portable_path_key(value) for value in normalized_needed}) != len(normalized_needed):
            fail("metadata ELF {} needed has a portable-name collision".format(target))
        if item.get("soname") is not None:
            validate_soname(item.get("soname"), "metadata ELF {} soname".format(target))
        parse_sha256(item.get("sha256"), "metadata ELF {} sha256".format(target))
        provenance = require_string(
            item.get("provenance"), "metadata ELF {} provenance".format(target)
        )
        reject_private_literal(provenance, "metadata ELF {} provenance".format(target))
        if target in elf_metadata:
            fail("duplicate metadata ELF path {}".format(target))
        elf_metadata[target] = item
    if list(elf_metadata) != sorted(elf_metadata):
        fail("metadata ELF audit entries are not sorted")
    if config["elf_audit"].get("count") != len(elf_metadata):
        fail("metadata ELF count is inconsistent")

    for target, item in sorted(config["inventory"].items()):
        path = stage / PurePosixPath(target)
        if sha256_file(path) != item["sha256"]:
            fail("metadata inventory hash mismatch for {}".format(target))
        actual_mode = stat.S_IMODE(path.stat().st_mode)
        if actual_mode != item["mode"]:
            fail("metadata inventory mode mismatch for {}".format(target))
        elf_item = elf_metadata.get(target)
        record = {
            "actual_path": path,
            "architecture": elf_item.get("architecture") if elf_item else None,
            "build_profile": elf_item.get("build_profile") if elf_item else None,
            "kind": item["kind"],
            "mode": item["mode"],
            "needed": elf_item.get("needed") if elf_item else None,
            "provenance": elf_item.get("provenance") if elf_item else None,
            "soname": elf_item.get("soname") if elf_item else None,
            "target": target,
        }
        records.append(record)

    if config["launcher"] not in config["inventory"]:
        fail("metadata launcher is absent from inventory")
    if config["inventory"][config["launcher"]]["kind"] != "launcher":
        fail("metadata launcher kind is invalid")
    if config["inventory"][config["launcher"]]["mode"] != 0o755:
        fail("metadata launcher is not executable")
    if not any(path.startswith(config["port_dir"] + "/") for path in payload_paths):
        fail("metadata port_dir has no payload")

    audit_config = {
        "ceiling": config["ceiling"],
        "dependencies": config["dependencies"],
        "exception_map": config["exception_map"],
        "launcher": config["launcher"],
        "launcher_chain": config["launcher_chain"],
        "launcher_contract": config["launcher_contract"],
        "nxextract": config["nxextract"],
        "package_id": config["package_id"],
        "port_dir": config["port_dir"],
        "portmaster_metadata": config["portmaster_metadata"],
    }
    elf_results, maximum = audit_record_set(records, audit_config)
    current = {item["path"]: item for item in elf_results}
    if set(current) != set(elf_metadata):
        fail("ELF inventory does not match actual staged ELFs")
    for target in sorted(current):
        if current[target] != elf_metadata[target]:
            fail("ELF audit metadata changed for {}".format(target))
    if maximum != config["elf_audit"].get("max_glibc_seen"):
        fail("metadata maximum GLIBC is inconsistent")
    return {
        "elf_count": len(elf_results),
        "max_glibc": maximum,
        "package_id": config["package_id"],
        "package_version": config["package_version"],
    }


def zip_timestamp(epoch):
    value = list(time.gmtime(epoch)[:6])
    value[5] -= value[5] % 2
    return tuple(value)


def create_archive(stage, output):
    stage = Path(stage).resolve()
    output_input = Path(output)
    output_name = require_string(output_input.name, "archive output filename")
    if not output_name.lower().endswith(".zip"):
        fail("archive output filename must end in .zip")
    if output_input.exists() or output_input.is_symlink():
        fail("archive output already exists: {}".format(output_input))
    output = output_input.resolve()
    checksum_output = Path(str(output) + ".sha256")
    if checksum_output.exists() or checksum_output.is_symlink():
        fail("archive checksum output already exists: {}".format(checksum_output))
    if not output.parent.is_dir():
        fail("archive output parent does not exist: {}".format(output.parent))
    if source_is_within(stage, output):
        fail("archive output cannot be inside the stage")

    result = verify_stage(stage)
    _, metadata_name, _, _ = discover_stage_internal_paths(stage)
    metadata = load_json(
        stage / PurePosixPath(metadata_name), "release metadata"
    )
    internal = validate_internal_metadata(metadata)
    compression = zipfile.ZIP_DEFLATED if internal["compression"] == "deflated" else zipfile.ZIP_STORED
    file_paths = sorted(
        (path for path in stage.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(stage).as_posix(),
    )
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".nxrelease-archive-", suffix=".zip", dir=str(output.parent)
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    checksum_temp = None
    try:
        with zipfile.ZipFile(
                str(temporary), "w", compression=compression, compresslevel=9,
                allowZip64=True) as archive:
            archive.comment = b""
            for path in file_paths:
                relative = path.relative_to(stage).as_posix()
                mode = stat.S_IMODE(path.stat().st_mode)
                info = zipfile.ZipInfo(relative, date_time=zip_timestamp(internal["epoch"]))
                info.create_system = 3
                info.compress_type = compression
                info.external_attr = (stat.S_IFREG | mode) << 16
                info.flag_bits |= 0x800
                info.file_size = path.stat().st_size
                with open(str(path), "rb") as source_handle:
                    with archive.open(
                            info, "w", force_zip64=info.file_size >= zipfile.ZIP64_LIMIT
                    ) as output_handle:
                        shutil.copyfileobj(source_handle, output_handle, 1024 * 1024)
        os.chmod(str(temporary), 0o644)
        os.utime(str(temporary), (internal["epoch"], internal["epoch"]))
        with open(str(temporary), "rb") as archive_handle:
            os.fsync(archive_handle.fileno())
        verify_archive(temporary)
        digest = sha256_file(temporary)
        checksum_line = "{}  {}\n".format(digest, output.name).encode("utf-8")
        checksum_descriptor, checksum_temp_name = tempfile.mkstemp(
            prefix=".nxrelease-checksum-", suffix=".sha256",
            dir=str(output.parent),
        )
        os.close(checksum_descriptor)
        checksum_temp = Path(checksum_temp_name)
        write_bytes(checksum_temp, checksum_line, 0o644, internal["epoch"])
        publish_archive_pair(
            temporary, checksum_temp, output, checksum_output
        )
        temporary.unlink()
        temporary = None
        checksum_temp.unlink()
        checksum_temp = None
        verify_archive(output, checksum_path=checksum_output)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
        if checksum_temp is not None and checksum_temp.exists():
            checksum_temp.unlink()
    result.update({"archive": str(output), "sha256": sha256_file(output)})
    return result


def create_release_bundle(stage, destination, archive_name):
    """Atomically publish one directory containing both ZIP and SHA-256.

    A directory rename is the only portable POSIX operation that makes two
    directory entries visible as one transaction. Direct `build` remains a
    coordinated no-overwrite pair; public automation should use `bundle` when
    crash-atomic joint visibility is required.
    """
    archive_name = require_string(archive_name, "bundle archive name")
    if PurePosixPath(archive_name).name != archive_name or not archive_name.lower().endswith(".zip"):
        fail("bundle archive name must be a .zip basename")
    destination_input = Path(destination)
    if destination_input.exists() or destination_input.is_symlink():
        fail("bundle destination already exists: {}".format(destination_input))
    destination = destination_input.resolve()
    parent = destination.parent
    if not parent.is_dir():
        fail("bundle parent does not exist: {}".format(parent))
    temporary = Path(tempfile.mkdtemp(
        prefix=".nxrelease-bundle-", dir=str(parent)
    ))
    try:
        temporary_archive = temporary / archive_name
        result = create_archive(stage, temporary_archive)
        verify_archive(
            temporary_archive,
            checksum_path=Path(str(temporary_archive) + ".sha256"),
        )
        os.chmod(str(temporary), 0o755)
        fsync_directory(temporary)
        rename_noreplace(temporary, destination)
        fsync_directory(parent)
        temporary = None
    finally:
        if temporary is not None and temporary.exists():
            shutil.rmtree(str(temporary))
    final_archive = destination / archive_name
    final_checksum = Path(str(final_archive) + ".sha256")
    verify_archive(final_archive, checksum_path=final_checksum)
    result.update({
        "archive": str(final_archive),
        "bundle": str(destination),
        "sha256": sha256_file(final_archive),
    })
    return result


def safe_zip_member(name):
    if name.endswith("/"):
        fail("archive contains an explicit directory entry: {}".format(name))
    return safe_relative(name, "ZIP member", allow_internal=True)


def verify_archive(archive_path, requested_ceiling=None, checksum_path=None):
    archive_input = Path(archive_path)
    if archive_input.is_symlink() or not archive_input.is_file():
        fail("archive is missing, not regular, or a symlink: {}".format(archive_input))
    archive_path = archive_input.resolve()
    if checksum_path is not None:
        checksum_input = Path(checksum_path)
        if checksum_input.is_symlink() or not checksum_input.is_file():
            fail("archive checksum is missing, not regular, or a symlink")
        checksum_file = checksum_input.resolve()
        text = read_small_text(checksum_file, "archive checksum")
        match = re.match(r"^([0-9a-f]{64})  ([^\n]+)\n?$", text)
        if not match or match.group(2) != archive_path.name:
            fail("archive checksum file is malformed or names another archive")
        if match.group(1) != sha256_file(archive_path):
            fail("archive SHA-256 verification failed")

    temporary = Path(tempfile.mkdtemp(prefix="nxrelease-verify-"))
    try:
        try:
            archive = zipfile.ZipFile(str(archive_path), "r")
        except (OSError, zipfile.BadZipFile) as exc:
            fail("cannot open ZIP {}: {}".format(archive_path, exc))
        with archive:
            if archive.comment:
                fail("archive comment must be empty")
            infos = archive.infolist()
            names = []
            folded = {}
            seen_names = set()
            for info in infos:
                name = safe_zip_member(info.filename)
                if name in seen_names:
                    fail("archive contains duplicate member {}".format(name))
                portable_key = portable_path_key(name)
                if portable_key in folded:
                    fail("archive contains case-insensitive collision: {} and {}".format(
                        folded[portable_key], name
                    ))
                folded[portable_key] = name
                seen_names.add(name)
                names.append(name)
                mode_type = (info.external_attr >> 16) & 0o170000
                if mode_type == stat.S_IFLNK:
                    fail("archive contains symlink {}".format(name))
                if mode_type != stat.S_IFREG:
                    fail("archive member is not a regular file: {}".format(name))
            if names != sorted(names):
                fail("archive members are not sorted deterministically")
            metadata_candidates = [
                name for name in names
                if (len(PurePosixPath(name).parts) == 3 and
                    PurePosixPath(name).parts[1:] == (
                        INTERNAL_DIRNAME, METADATA_BASENAME
                    ))
            ]
            if len(metadata_candidates) != 1:
                fail("archive must contain exactly one <port>/{}/{}".format(
                    INTERNAL_DIRNAME, METADATA_BASENAME
                ))
            metadata_name = metadata_candidates[0]
            discovered_port_dir = PurePosixPath(metadata_name).parts[0]
            _, checksum_name, sbom_name = internal_paths(discovered_port_dir)
            if checksum_name not in names:
                fail("archive lacks {}".format(checksum_name))
            if sbom_name not in names:
                fail("archive lacks {}".format(sbom_name))
            bad_member = archive.testzip()
            if bad_member is not None:
                fail("ZIP CRC verification failed at {}".format(bad_member))
            if archive.getinfo(metadata_name).file_size > 16 * 1024 * 1024:
                fail("release metadata is unreasonably large")
            try:
                metadata = json.loads(archive.read(metadata_name).decode("utf-8"))
            except (UnicodeDecodeError, ValueError) as exc:
                fail("archive release metadata is invalid: {}".format(exc))
            internal = validate_internal_metadata(metadata, requested_ceiling=requested_ceiling)
            if internal["port_dir"] != discovered_port_dir:
                fail("archive metadata is stored outside its declared port_dir")
            expected_timestamp = zip_timestamp(internal["epoch"])
            expected_compression = zipfile.ZIP_DEFLATED if internal["compression"] == "deflated" else zipfile.ZIP_STORED
            expected_modes = {
                path: item["mode"] for path, item in internal["inventory"].items()
            }
            expected_modes[metadata_name] = 0o644
            expected_modes[checksum_name] = 0o644
            expected_modes[sbom_name] = 0o644
            if set(names) != set(expected_modes):
                fail("ZIP members do not match metadata inventory")
            for info in infos:
                if info.date_time != expected_timestamp:
                    fail("ZIP timestamp is not deterministic for {}".format(info.filename))
                if info.compress_type != expected_compression:
                    fail("ZIP compression differs for {}".format(info.filename))
                if info.create_system != 3:
                    fail("ZIP member is not recorded with Unix mode: {}".format(info.filename))
                mode = (info.external_attr >> 16) & 0o7777
                if mode != expected_modes[info.filename]:
                    fail("ZIP mode mismatch for {}".format(info.filename))
                target = temporary / PurePosixPath(info.filename)
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info, "r") as source_handle:
                    with open(str(target), "wb") as target_handle:
                        shutil.copyfileobj(source_handle, target_handle, 1024 * 1024)
                os.chmod(str(target), mode)
                os.utime(str(target), (internal["epoch"], internal["epoch"]))
        return verify_stage(temporary, requested_ceiling=requested_ceiling)
    finally:
        shutil.rmtree(str(temporary))


def command_validate(arguments):
    config = load_manifest(arguments.manifest, arguments.max_glibc)
    elfs, maximum = validate_sources(config)
    print("NXRELEASE VALIDATE: PASS package={} version={} files={} elfs={} max_glibc={} ceiling={}".format(
        config["package_id"], config["package_version"], len(config["records"]),
        len(elfs), maximum, config["ceiling"]
    ))


def command_stage(arguments):
    config = load_manifest(arguments.manifest, arguments.max_glibc)
    validate_sources(config)
    destination = stage_release(config, arguments.stage)
    result = verify_stage(destination, requested_ceiling=config["ceiling"])
    print("NXRELEASE STAGE: PASS stage={} package={} files={} elfs={} max_glibc={} ceiling={}".format(
        destination, result["package_id"], len(config["records"]), result["elf_count"],
        result["max_glibc"], config["ceiling"]
    ))


def command_verify_stage(arguments):
    result = verify_stage(arguments.stage, requested_ceiling=arguments.max_glibc)
    print("NXRELEASE VERIFY-STAGE: PASS package={} version={} elfs={} max_glibc={}".format(
        result["package_id"], result["package_version"], result["elf_count"],
        result["max_glibc"]
    ))


def command_build(arguments):
    output_path = Path(arguments.output)
    if output_path.exists() or output_path.is_symlink():
        fail("archive output already exists: {}".format(output_path))
    checksum_path = Path(str(output_path) + ".sha256")
    if checksum_path.exists() or checksum_path.is_symlink():
        fail("archive checksum output already exists: {}".format(checksum_path))
    config = load_manifest(arguments.manifest, arguments.max_glibc)
    validate_sources(config)
    destination = stage_release(config, arguments.stage)
    result = create_archive(destination, arguments.output)
    print("NXRELEASE BUILD: PASS package={} version={} stage={} archive={} sha256={} elfs={} max_glibc={} ceiling={}".format(
        result["package_id"], result["package_version"], destination, result["archive"],
        result["sha256"], result["elf_count"], result["max_glibc"], config["ceiling"]
    ))


def command_bundle(arguments):
    bundle_path = Path(arguments.destination)
    if bundle_path.exists() or bundle_path.is_symlink():
        fail("bundle destination already exists: {}".format(bundle_path))
    config = load_manifest(arguments.manifest, arguments.max_glibc)
    validate_sources(config)
    destination = stage_release(config, arguments.stage)
    result = create_release_bundle(
        destination, arguments.destination, arguments.archive_name
    )
    print("NXRELEASE BUNDLE: PASS package={} version={} stage={} bundle={} archive={} sha256={} elfs={} max_glibc={} ceiling={}".format(
        result["package_id"], result["package_version"], destination,
        result["bundle"], result["archive"], result["sha256"],
        result["elf_count"], result["max_glibc"], config["ceiling"]
    ))


def command_verify(arguments):
    result = verify_archive(
        arguments.archive, requested_ceiling=arguments.max_glibc,
        checksum_path=arguments.sha256_file,
    )
    print("NXRELEASE VERIFY: PASS package={} version={} archive={} sha256={} elfs={} max_glibc={}".format(
        result["package_id"], result["package_version"], Path(arguments.archive).resolve(),
        sha256_file(Path(arguments.archive).resolve()), result["elf_count"], result["max_glibc"]
    ))


def build_parser():
    parser = argparse.ArgumentParser(
        description="Stage, audit, package and re-verify a universal PortMaster release"
    )
    parser.add_argument("--version", action="version", version="nxrelease {}".format(TOOL_VERSION))
    subparsers = parser.add_subparsers(dest="command")
    subparsers.required = True

    validate = subparsers.add_parser("validate", help="validate manifest, source pins and source ELFs")
    validate.add_argument("--manifest", required=True)
    validate.add_argument("--max-glibc")
    validate.set_defaults(function=command_validate)

    stage = subparsers.add_parser("stage", help="create and verify a fresh staging tree")
    stage.add_argument("--manifest", required=True)
    stage.add_argument("--stage", required=True)
    stage.add_argument("--max-glibc")
    stage.set_defaults(function=command_stage)

    verify_stage_parser = subparsers.add_parser("verify-stage", help="verify a staged release")
    verify_stage_parser.add_argument("--stage", required=True)
    verify_stage_parser.add_argument("--max-glibc")
    verify_stage_parser.set_defaults(function=command_verify_stage)

    build = subparsers.add_parser("build", help="stage, package and re-open/re-verify a release")
    build.add_argument("--manifest", required=True)
    build.add_argument("--stage", required=True)
    build.add_argument("--output", required=True)
    build.add_argument("--max-glibc")
    build.set_defaults(function=command_build)

    bundle = subparsers.add_parser(
        "bundle",
        help="atomically publish a new directory containing ZIP and SHA-256",
    )
    bundle.add_argument("--manifest", required=True)
    bundle.add_argument("--stage", required=True)
    bundle.add_argument("--destination", required=True)
    bundle.add_argument("--archive-name", required=True)
    bundle.add_argument("--max-glibc")
    bundle.set_defaults(function=command_bundle)

    verify = subparsers.add_parser("verify", help="re-open and verify a built ZIP")
    verify.add_argument("--archive", required=True)
    verify.add_argument("--sha256-file")
    verify.add_argument("--max-glibc")
    verify.set_defaults(function=command_verify)
    return parser


def main(argv=None):
    parser = build_parser()
    arguments = parser.parse_args(argv)
    try:
        assert_abi_policy_agrees()
        arguments.function(arguments)
    except ReleaseError as exc:
        print("NXRELEASE FAIL: {}".format(exc), file=sys.stderr)
        return 1
    except (OSError, RuntimeError, subprocess.SubprocessError, zipfile.BadZipFile) as exc:
        print("NXRELEASE FAIL: host operation failed: {}".format(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("NXRELEASE FAIL: interrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
