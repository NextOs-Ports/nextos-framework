#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate a deterministic, fail-closed PortMaster project scaffold."""

import argparse
import ast
import ctypes
from decimal import Decimal
import errno
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import sys
import tempfile
import types
from xml.etree import ElementTree
from xml.sax.saxutils import escape as xml_escape


ROOT = Path(__file__).resolve().parent
REPOSITORY = ROOT.parents[1]
BOOTSTRAP_ROOT = REPOSITORY / "framework" / "nxbootstrap"
NXSPLASH_ROOT = REPOSITORY / "framework" / "nxsplash"
NXEXTRACT_ROOT = REPOSITORY / "suportando_outros_devices" / "extrator-universal"
BOOTSTRAP_GENERATOR_PATH = BOOTSTRAP_ROOT / "tools" / "generate-port.py"
BOOTSTRAP_LAUNCHER_TEMPLATE = BOOTSTRAP_ROOT / "templates" / "launcher.sh.in"
NXSPLASH_RELEASE_MANIFEST = NXSPLASH_ROOT / "release" / "manifest-v1.json"
NXSPLASH_SOURCE = NXSPLASH_ROOT / "src" / "nxsplash.c"
PORTMASTER_ROOT = REPOSITORY / "framework" / "portmaster"
PORTMASTER_CONTRACT = PORTMASTER_ROOT / "contract-v3.json"
PORTMASTER_METADATA_SCHEMA = (
    PORTMASTER_ROOT / "schema" / "port-json-supported-v2.schema.json"
)
PORTMASTER_VENDOR_PROVENANCE = (
    PORTMASTER_ROOT / "vendor" / "PortMaster-GUI-8f9ddc4.json"
)
PORTMASTER_LEGACY_RUNTIME = (
    PORTMASTER_ROOT / "fixtures" / "harbourmaster" /
    "runtime-legacy-compat-v1.json"
)
NXEXTRACT_RELEASE_MANIFEST = (
    NXEXTRACT_ROOT / "ui" / "release" / "manifest-v1.json"
)
NXEXTRACT_SOURCE = NXEXTRACT_ROOT / "ui" / "nxextract_ui.c"
# V4-04B: the NXExtract 1.3.0 engine IS the single canonical recipe
# authority.  The generator delegates the whole structural validation of
# extractor.json to this exact framework file — never to code shipped by the
# candidate — after verifying its declared VERSION on both sides.
NXEXTRACT_ENGINE = NXEXTRACT_ROOT / "nxextract.py"
NXEXTRACT_RECIPE_AUTHORITY_VERSION = "1.3.0"
# V4-04C: the engine's bytes are authenticated BEFORE any of them execute.
# This pinned identity is independent of the presented file; it changes only
# in a reviewed nxgenerator bump, together with the version pin above.
NXEXTRACT_ENGINE_SHA256 = (
    "e59a1e525ea475635c2e8f51bff3c6fff8cdaad406c158e3dad011cdd96876a7")
NXEXTRACT_ENGINE_MAX_BYTES = 4 * 1024 * 1024
NXEXTRACT_AUTHORITY_LOGICAL_NAME = "<nxextract-recipe-authority>"
NXEXTRACT_ENGINE_COMPONENTS = (
    "suportando_outros_devices", "extrator-universal", "nxextract.py")
OWNER_DATA_DIRECTORY = "gamedata"
LANGUAGE_ACCESS_MODES = (
    "native-menu", "first-run-native", "adapter", "single-language", "none",
)
# V3-GRAPHICS-02: the declared graphics context contract. These enums MUST match
# nxgl_graphics_contract.h exactly (api/profile/version_policy/shader_dialect) so
# the schema and the C validator never disagree.
GRAPHICS_API = ("gles", "gl")
GRAPHICS_PROFILE = ("es", "core", "compat")
GRAPHICS_VERSION_POLICY = ("exact", "minimum", "range")
GRAPHICS_SHADER_DIALECT = ("essl100", "essl300", "essl310", "glsl-any")
# The single adapter every GL port vendors and calls right after context
# creation; recorded in the generated contract so nxrelease can require it.
GRAPHICS_ADAPTER = "nxgl_graphics_contract_adapter"
GRAPHICS_EVIDENCE_BOUNDARY = ("post-first-present",)
GPTK_CONTEXTS = ("menu", "gameplay", "cursor")
GPTK_REQUIRED_CONTEXTS = ("menu", "gameplay")
GPTK_CONTROLS = (
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
    "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT", "LEFT_STICK",
    "RIGHT_STICK",
)
GPTK_NULL = "null"
GPTK_NATIVE = "native"
GPTK_ACTION_RE = re.compile(
    r"[a-z][a-z0-9_]*(?:[.][a-z][a-z0-9_]*)+"
)
GPTK_SINK_RE = re.compile(
    r"[a-z][a-z0-9_-]*(?:[.][a-z][a-z0-9_-]*)+"
)
GPTK_ACTION_KINDS = ("button", "axis", "vector")
GPTK_STATIC_GUIDANCE = (
    "# Mapa estático dos controles nativos. Este port NÃO carrega edições "
    "deste\n"
    "# arquivo em runtime; SDL/PortMaster permanece a fonte dos controles.\n"
    "# Static map of native controls. This port does NOT load edits from "
    "this\n"
    "# file at runtime; SDL/PortMaster remains the controls source."
)
GPTK_LIVE_GUIDANCE = (
    "# Edite as ações à direita para trocar os controles. A lista de ações\n"
    "# válidas vem do adapter-contract.json do port. Este arquivo é SEU:\n"
    "# atualizações do port nunca sobrescrevem a sua cópia editada.\n"
    "# Edit the actions on the right to remap. Valid actions come from the\n"
    "# port's adapter-contract.json. This file is YOURS: port updates never\n"
    "# overwrite your edited copy."
)
GPTK_LIVE_RUNTIME_CONTRACT = {
    "schema": "nxinput-gptk-live/1",
    "context_initial": "unproven",
    "unproven_policy": "native-passthrough",
    "sink_coverage": "all-actions-before-activation",
    "delivery_ack": "required",
}
APKCOMPAT_PATH = (
    REPOSITORY / "framework" / "contracts" / "apkcompat" / "apkcompat.py"
)


def load_apkcompat():
    spec = importlib.util.spec_from_file_location(
        "nx_apkcompat", APKCOMPAT_PATH
    )
    if spec is None or spec.loader is None:
        raise ProjectError("cannot load the canonical apkcompat contract")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_promoted_gptk_live_contract(promoted_input, controls):
    """Keep a promoted adapter on the generated fail-safe live policy."""
    if controls.get("runtime_mapping") != "nxinput-gptk":
        return
    if (not isinstance(promoted_input, dict) or
            promoted_input.get("runtime_mapping") != "nxinput-gptk" or
            promoted_input.get("runtime_contract") !=
            controls["runtime_contract"]):
        raise ProjectError(
            "promoted adapter-contract live runtime policy differs from "
            "the generated fail-safe controls contract"
        )


def validate_promoted_controller_profiles(promoted, controller_profiles):
    """A promoted contract replaces the generated skeleton verbatim, so it
    must carry exactly the same authority-3 pin declared by nxproject.  Absence
    is also exact: a promotion cannot introduce an undeclared bundle."""
    if not isinstance(promoted, dict):
        raise ProjectError("promoted adapter-contract must be an object")
    present = "input_controller_profiles" in promoted
    if controller_profiles is None:
        if present:
            raise ProjectError(
                "promoted adapter-contract controller profiles differ from "
                "nxproject controls"
            )
        return
    if (not present or
            promoted.get("input_controller_profiles") != controller_profiles):
        raise ProjectError(
            "promoted adapter-contract controller profiles differ from "
            "nxproject controls"
        )

NXEXTRACT_UI_REQUIRED_VERSION = "1.2.16"
NXEXTRACT_COMMON_FILES = (
    ("nxextract.py", "nxextract.py", 0o644),
    ("run-extractor.sh", "run-extractor.sh", 0o644),
    ("nxextract-runtime-env.sh", "nxextract-runtime-env.sh", 0o644),
)
PROJECT_KEYS = {
    "schema_version",
    "nxport",
    "runtime_root",
    "nxextract_recipe",
    "adapter",
    "portmaster",
    "license",
    "documentation",
    "owner_data",
    "language_access",
    "graphics",
    "controls",
    "display",
    "promotion",
    "package_payload",
    "video",
    "owner_runtime",
}
# V5 7A.3: NEXTOS_SETTINGS/2 video namespace (must match nxcompat_settings.h)
VIDEO_AUTHORITIES = ("nextos", "engine", "synchronized")
VIDEO_ASPECTS = ("auto", "engine", "preserve", "stretch", "crop", "integer")
VIDEO_OUTPUT_SIZES = ("auto", "display", "640x480", "1280x720", "1920x1080")
VIDEO_FILTERS = ("engine", "nearest", "linear")
VIDEO_INVALID_POLICIES = ("fail_closed", "last_known_good", "package_default")
VIDEO_AUTO_ALGORITHMS = ("stretch", "ratio-threshold", "epsilon", "none")
PACKAGE_PAYLOAD_MAX_FILES = 128
PACKAGE_PAYLOAD_MAX_FILE_SIZE = 4 * 1024 * 1024
PACKAGE_PAYLOAD_MAX_TOTAL_SIZE = 16 * 1024 * 1024
PACKAGE_PAYLOAD_KINDS = ("payload", "license-notice")
PACKAGE_PAYLOAD_RESERVED_FILES = frozenset((
    "nxport.json", "nxproject.json", "GENERATION.json", "port.json",
    "gameinfo.xml", "LICENSE", "extractor.json", "nxsplash-nextos",
    "cover.png",
))
PACKAGE_PAYLOAD_DOCUMENTATION = frozenset((
    "README.md", "INSTALLATION.md",
))
PACKAGE_PAYLOAD_RESERVED_PREFIXES = frozenset((
    "adapter", "defaults", "gamedata", "nxextract", "lib", ".nxruntime",
    ".nxrelease", "saves", "userdata",
))
PACKAGE_PAYLOAD_ARCHIVE_SUFFIXES = (
    ".apk", ".apkm", ".apks", ".xapk", ".obb", ".zip", ".jar", ".aar",
    ".7z", ".rar", ".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz",
    ".tar.bz2", ".tbz", ".tbz2", ".gz", ".bz2", ".xz", ".zst",
    ".cab", ".deb", ".rpm",
)
PACKAGE_PAYLOAD_SHA256_RE = re.compile(r"[0-9a-f]{64}")
CURSOR_TUNING_KEYS = (
    "speed", "deadzone", "response_curve", "acceleration", "smoothing_ms",
)
CAMERA_TUNING_KEYS = (
    "sensitivity_x", "sensitivity_y", "deadzone", "response_curve",
    "invert_x", "invert_y", "authority",
)
CURSOR_TUNING_RANGES = {
    "speed": (0.05, 8.0),
    "deadzone": (0.0, 0.9),
    "response_curve": (0.25, 4.0),
    "acceleration": (0.0, 4.0),
    "smoothing_ms": (0.0, 500.0),
}
CAMERA_TUNING_RANGES = {
    "sensitivity_x": (0.05, 8.0),
    "sensitivity_y": (0.05, 8.0),
    "deadzone": (0.0, 0.9),
    "response_curve": (0.25, 4.0),
}
IPV4_RE = re.compile(
    r"(?<![0-9])(?:[0-9]{1,3}[.]){3}[0-9]{1,3}(?![0-9])"
)
PERSONAL_PATH_RE = re.compile(
    r"(?:^|[ /])(?:/home/[^/ ]+|/Users/[^/ ]+|[A-Za-z]:[\\/])",
    re.IGNORECASE,
)
SPDX_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.+-]{0,63}$")
VERSION_RE = re.compile(r"^[0-9]+(?:[.][0-9]+)+$")
AT_FDCWD = -100
RENAME_NOREPLACE = 1


class ProjectError(Exception):
    """A project manifest or publication boundary is invalid."""


def load_bootstrap_generator():
    spec = importlib.util.spec_from_file_location(
        "nxbootstrap_generate_port", BOOTSTRAP_GENERATOR_PATH
    )
    if spec is None or spec.loader is None:
        raise ProjectError("cannot load the pinned nxbootstrap generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BOOTSTRAP_GENERATOR = load_bootstrap_generator()


def sha256_bytes(payload):
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json(value):
    return (json.dumps(
        value, ensure_ascii=False, indent=2, sort_keys=True
    ) + "\n").encode("utf-8")


def require_object(value, context, keys):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise ProjectError(
            "%s must contain exactly: %s" %
            (context, ", ".join(sorted(keys)))
        )
    return value


def require_string(value, context, pattern=None):
    if not isinstance(value, str) or not value or len(value) > 512:
        raise ProjectError("%s must be a bounded non-empty string" % context)
    if any(ord(character) < 0x20 or 0x7f <= ord(character) <= 0x9f
           for character in value):
        raise ProjectError("%s contains a control character" % context)
    if pattern is not None and not pattern.fullmatch(value):
        raise ProjectError("%s has an invalid value" % context)
    return value


def require_unique_strings(value, context):
    if not isinstance(value, list):
        raise ProjectError("%s must be an array" % context)
    result = []
    for index, item in enumerate(value):
        result.append(require_string(
            item, "%s[%d]" % (context, index)
        ))
    if len(result) != len(set(result)):
        raise ProjectError("%s contains duplicates" % context)
    return result


def reject_public_literal(value, context):
    if IPV4_RE.search(value) or PERSONAL_PATH_RE.search(value):
        raise ProjectError("%s contains a private host literal" % context)


def safe_source_root(value):
    root = Path(os.path.abspath(os.fspath(value)))
    if root.is_symlink() or not root.is_dir():
        raise ProjectError("source root must be a real existing directory")
    cursor = Path(root.anchor)
    for part in root.parts[1:]:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ProjectError("source root traverses a symlink")
    return root


def safe_repository_file(value, context, source_root=REPOSITORY):
    value = require_string(value, context)
    logical = PurePosixPath(value)
    if (logical.is_absolute() or not logical.parts or
            any(part in ("", ".", "..") for part in logical.parts)):
        raise ProjectError("%s must be a source-root-relative path" % context)
    cursor = safe_source_root(source_root)
    for part in logical.parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ProjectError("%s traverses a symlink" % context)
    if not cursor.is_file():
        raise ProjectError("%s is not a regular file" % context)
    return cursor


def safe_repository_directory(value, context, source_root=REPOSITORY):
    """Resolve a source-root-relative directory without following symlinks.

    ``runtime_root`` is a build input, not a path embedded into the generated
    launcher.  Keeping it relative to the already validated source root makes
    standalone projects reproducible while preventing a manifest from
    selecting an arbitrary host directory.  ``.`` deliberately denotes the
    source root itself; every other spelling must be canonical.
    """
    value = require_string(value, context)
    root = safe_source_root(source_root)
    if value == ".":
        return root
    logical = PurePosixPath(value)
    if (logical.is_absolute() or not logical.parts or
            any(part in ("", ".", "..") for part in logical.parts)):
        raise ProjectError("%s must be a source-root-relative directory" % context)
    cursor = root
    for part in logical.parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ProjectError("%s traverses a symlink" % context)
    if not cursor.is_dir():
        raise ProjectError("%s is not a real existing directory" % context)
    return cursor


def _path_collision(left, right):
    """Return whether two canonical relative paths overlap as files/parents."""
    left = left.casefold()
    right = right.casefold()
    return (left == right or left.startswith(right + "/") or
            right.startswith(left + "/"))


def _package_payload_path(value, context):
    value = require_string(value, context)
    if "\\" in value:
        raise ProjectError("%s must use forward slashes" % context)
    logical = PurePosixPath(value)
    if (logical.is_absolute() or not logical.parts or
            logical.as_posix() != value or
            any(part in ("", ".", "..") or part.startswith(".")
                for part in logical.parts)):
        raise ProjectError(
            "%s must be a canonical non-hidden source-root-relative path" %
            context
        )
    return value, logical


def _package_payload_forbidden_type(path, payload):
    basename = PurePosixPath(path).name.casefold()
    if (basename.endswith(".so") or ".so." in basename or
            any(basename.endswith(suffix)
                for suffix in PACKAGE_PAYLOAD_ARCHIVE_SUFFIXES)):
        return True
    if payload.startswith(b"\x7fELF"):
        return True
    archive_magics = (
        b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08", b"\x1f\x8b",
        b"BZh", b"\xfd7zXZ\x00", b"7z\xbc\xaf'\x1c", b"Rar!\x1a\x07",
        b"\x28\xb5\x2f\xfd", b"!<arch>\n",
    )
    return (any(payload.startswith(magic) for magic in archive_magics) or
            (len(payload) >= 262 and payload[257:262] == b"ustar"))


def _read_package_payload_source(source_root, logical, mode, expected_sha256,
                                 context):
    """Read and retain one author payload after a single descriptor audit."""
    parent = source_root
    for part in logical.parts[:-1]:
        parent = parent / part
        if parent.is_symlink() or not parent.is_dir():
            raise ProjectError("%s traverses an unsafe directory" % context)
    source = parent / logical.parts[-1]
    try:
        listed = os.lstat(source)
    except OSError as error:
        raise ProjectError("%s is not a readable non-symlink file: %s" %
                           (context, error))
    if stat.S_ISLNK(listed.st_mode) or not stat.S_ISREG(listed.st_mode):
        raise ProjectError("%s is not a readable non-symlink file" % context)
    flags = os.O_RDONLY
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(source, flags)
    except OSError as error:
        raise ProjectError("%s is not a readable non-symlink file: %s" %
                           (context, error))
    try:
        before = os.fstat(descriptor)
        if ((listed.st_dev, listed.st_ino) != (before.st_dev, before.st_ino) or
                not stat.S_ISREG(before.st_mode) or before.st_nlink != 1):
            raise ProjectError(
                "%s must be a single-link regular non-symlink file" % context
            )
        if stat.S_IMODE(before.st_mode) != mode:
            raise ProjectError("%s mode differs from its declaration" % context)
        if before.st_size > PACKAGE_PAYLOAD_MAX_FILE_SIZE:
            raise ProjectError("%s exceeds the 4 MiB file limit" % context)
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            payload = stream.read(PACKAGE_PAYLOAD_MAX_FILE_SIZE + 1)
        after = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    if (len(payload) != before.st_size or len(payload) >
            PACKAGE_PAYLOAD_MAX_FILE_SIZE or
            (before.st_dev, before.st_ino, before.st_size, before.st_mode) !=
            (after.st_dev, after.st_ino, after.st_size, after.st_mode) or
            after.st_nlink != 1):
        raise ProjectError("%s changed while it was being validated" % context)
    if sha256_bytes(payload) != expected_sha256:
        raise ProjectError("%s SHA-256 differs from its declaration" % context)
    if _package_payload_forbidden_type(logical.as_posix(), payload):
        raise ProjectError("%s is a forbidden ELF/game/archive payload" % context)
    return payload


def _runtime_member_paths(nxport):
    result = set(nxport.get("required_files", []))
    result.add(nxport["executable"])
    if nxport.get("prepare_script"):
        result.add(nxport["prepare_script"])
    for record in nxport.get("generation_runtime") or []:
        result.add(record["path"])
    roles = nxport.get("execution_roles") or {}
    for role_name in ("extractor", "splash", "game"):
        role = roles.get(role_name)
        if role is not None:
            result.add(role["executable"])
    for helper in roles.get("helpers", []):
        result.add(helper["executable"])
    return result


def validate_package_payload(raw_payload, project_schema, source_root, nxport,
                             documentation):
    if project_schema < 3:
        raise ProjectError("package_payload requires nxproject schema_version 3")
    if not isinstance(raw_payload, list):
        raise ProjectError("package_payload must be an array")
    if len(raw_payload) > PACKAGE_PAYLOAD_MAX_FILES:
        raise ProjectError("package_payload exceeds the 128-file limit")

    records = []
    paths = []
    for index, raw_record in enumerate(raw_payload):
        context = "package_payload[%d]" % index
        record = require_object(
            raw_record, context, {"path", "mode", "sha256", "kind"}
        )
        path, logical = _package_payload_path(record["path"], context + ".path")
        mode_text = record["mode"]
        if mode_text not in ("0644", "0755"):
            raise ProjectError("%s.mode must be exactly 0644 or 0755" % context)
        if mode_text == "0755" and not (
                len(logical.parts) > 1 and logical.parts[0] == "tools"):
            raise ProjectError("%s mode 0755 is allowed only below tools/" % context)
        expected_sha256 = require_string(
            record["sha256"], context + ".sha256", PACKAGE_PAYLOAD_SHA256_RE
        )
        kind = record["kind"]
        if kind not in PACKAGE_PAYLOAD_KINDS:
            raise ProjectError(
                "%s.kind must be payload or license-notice" % context
            )
        records.append({
            "path": path,
            "logical": logical,
            "mode": int(mode_text, 8),
            "mode_text": mode_text,
            "sha256": expected_sha256,
            "kind": kind,
        })
        paths.append(path)

    if paths != sorted(paths):
        raise ProjectError("package_payload paths must be lexicographically ordered")
    casefolded = [path.casefold() for path in paths]
    if len(casefolded) != len(set(casefolded)):
        raise ProjectError("package_payload paths must be casefold-unique")
    for index, path in enumerate(paths):
        for other in paths[index + 1:]:
            if _path_collision(path, other):
                raise ProjectError("package_payload paths collide: %s and %s" %
                                   (path, other))

    doc_paths = set(paths) & set(PACKAGE_PAYLOAD_DOCUMENTATION)
    for record in records:
        if (record["path"] in PACKAGE_PAYLOAD_DOCUMENTATION and
                record["kind"] != "payload"):
            raise ProjectError(
                "package_payload documentation member %s must use kind payload" %
                record["path"]
            )
    if documentation["status"] == "authored":
        missing = set(PACKAGE_PAYLOAD_DOCUMENTATION) - set(paths)
        if missing:
            raise ProjectError(
                "authored documentation requires package_payload paths: %s" %
                ", ".join(sorted(missing))
            )
    elif doc_paths:
        raise ProjectError(
            "scaffold documentation must not replace README.md or INSTALLATION.md"
        )

    reserved_files = {item.casefold() for item in PACKAGE_PAYLOAD_RESERVED_FILES}
    reserved_prefixes = {
        item.casefold() for item in PACKAGE_PAYLOAD_RESERVED_PREFIXES
    }
    dynamic_paths = _runtime_member_paths(nxport)
    dynamic_paths.add(nxport["launcher_name"])
    generated_paths = set(PACKAGE_PAYLOAD_RESERVED_FILES)
    generated_paths.update(PACKAGE_PAYLOAD_DOCUMENTATION)
    for record in records:
        path = record["path"]
        logical = record["logical"]
        folded = path.casefold()
        if (logical.parts[0].casefold() in reserved_prefixes or
                (folded in reserved_files and
                 path not in PACKAGE_PAYLOAD_DOCUMENTATION)):
            raise ProjectError("package_payload path is reserved: %s" % path)
        if path not in PACKAGE_PAYLOAD_DOCUMENTATION and any(
                _path_collision(path, generated) for generated in generated_paths):
            raise ProjectError("package_payload path collides with generation: %s" %
                               path)
        if any(_path_collision(path, runtime) for runtime in dynamic_paths):
            raise ProjectError("package_payload path collides with runtime: %s" %
                               path)

    total_size = 0
    for index, record in enumerate(records):
        context = "package_payload[%d]" % index
        payload = _read_package_payload_source(
            source_root, record["logical"], record["mode"],
            record["sha256"], context,
        )
        total_size += len(payload)
        if total_size > PACKAGE_PAYLOAD_MAX_TOTAL_SIZE:
            raise ProjectError("package_payload exceeds the 16 MiB total limit")
        record["payload"] = payload
    return records


def read_version(path, component):
    try:
        value = path.read_text(encoding="utf-8").strip()
    except OSError as error:
        raise ProjectError("cannot read %s version: %s" % (component, error))
    if not VERSION_RE.fullmatch(value):
        raise ProjectError("%s has an invalid version" % component)
    return value


def require_regular_file(path, context):
    if path.is_symlink() or not path.is_file():
        raise ProjectError("%s must be a regular non-symlink file" % context)
    return path


def bootstrap_source_state():
    # nxbootstrap 0.7.5: the product is one self-contained launcher plus
    # nxport.json and the splash-role-matched nxsplash helper. There is no
    # bash runtime library or deployment receipt; the pins cover the generator
    # and its template instead.
    version = read_version(BOOTSTRAP_ROOT / "VERSION", "nxbootstrap")
    if getattr(BOOTSTRAP_GENERATOR, "NXBOOTSTRAP_VERSION", None) != version:
        raise ProjectError(
            "nxbootstrap generator version does not match VERSION"
        )

    generator_path = require_regular_file(
        BOOTSTRAP_GENERATOR_PATH, "nxbootstrap generator"
    )
    launcher_template = require_regular_file(
        BOOTSTRAP_LAUNCHER_TEMPLATE, "nxbootstrap launcher template"
    )
    template_bytes = launcher_template.read_bytes()
    if b"@NXBOOTSTRAP_VERSION@" not in template_bytes:
        raise ProjectError(
            "nxbootstrap launcher template lacks its version slot"
        )
    return {
        "version": version,
        "source_files": {
            "templates/launcher.sh.in": sha256_bytes(template_bytes),
            "tools/generate-port.py": sha256_file(generator_path),
        },
    }


def nxsplash_source_state(architecture):
    version = read_version(NXSPLASH_ROOT / "VERSION", "nxsplash")
    release_manifest = require_regular_file(
        NXSPLASH_RELEASE_MANIFEST, "nxsplash release manifest"
    )
    source = require_regular_file(NXSPLASH_SOURCE, "nxsplash source")
    try:
        artifact, artifact_sha256 = BOOTSTRAP_GENERATOR.nxsplash_artifact(
            architecture
        )
    except BOOTSTRAP_GENERATOR.ManifestError as error:
        raise ProjectError("invalid nxsplash release: %s" % error)
    return {
        "version": version,
        "release_manifest_sha256": sha256_file(release_manifest),
        "source_sha256": sha256_file(source),
        "artifact": {
            "architecture": architecture,
            "path": artifact.relative_to(NXSPLASH_ROOT).as_posix(),
            "mode": "%04o" % stat.S_IMODE(artifact.stat().st_mode),
            "sha256": artifact_sha256,
        },
    }


def execution_role_architecture(nxport, role):
    """Return the artifact ABI owned by a validated execution role."""
    roles = nxport.get("execution_roles")
    if roles is None:
        return nxport["architecture"]
    selected = roles[role]
    if selected is None:
        return None
    return selected["architecture"]


_NXEXTRACT_AUTHORITY = None
_NXEXTRACT_AUTHORITY_SHA256 = None


def _authority_error(message):
    """Deterministic, sanitized refusal: no pathname, no traceback."""
    return ProjectError("NXExtract recipe authority: " + message)


def _read_authenticated_engine():
    """Authenticate the canonical engine bytes before ANY of them run.

    The path is derived only from the fixed repository-relative components;
    candidate, source root or environment never select the module.  Every
    component is lstat'd (real directory, never a symlink), the file is
    opened no-follow, validated by fstat (regular, bounded size, same
    inode/device as the lstat), read exactly once, and the SHA-256 of the
    presented bytes must equal the pinned independent identity."""
    path = REPOSITORY.joinpath(*NXEXTRACT_ENGINE_COMPONENTS)
    if path != NXEXTRACT_ENGINE:
        raise _authority_error("canonical engine path drifted")
    probe = REPOSITORY
    info = None
    for index, part in enumerate(NXEXTRACT_ENGINE_COMPONENTS):
        probe = probe / part
        try:
            info = os.lstat(probe)
        except FileNotFoundError:
            raise _authority_error("a canonical engine component is missing")
        except OSError:
            raise _authority_error(
                "a canonical engine component is unreadable")
        if stat.S_ISLNK(info.st_mode):
            raise _authority_error(
                "a canonical engine component is a symlink")
        last = index == len(NXEXTRACT_ENGINE_COMPONENTS) - 1
        if not last and not stat.S_ISDIR(info.st_mode):
            raise _authority_error(
                "a canonical engine component is not a directory")
    if not stat.S_ISREG(info.st_mode):
        raise _authority_error("the canonical engine is not a regular file")
    if info.st_size > NXEXTRACT_ENGINE_MAX_BYTES:
        raise _authority_error("the canonical engine exceeds the size ceiling")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(str(path), flags)
    except OSError:
        raise _authority_error("cannot open the canonical engine")
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode) or \
                opened.st_size > NXEXTRACT_ENGINE_MAX_BYTES:
            raise _authority_error(
                "the canonical engine changed shape while opening")
        if (opened.st_ino, opened.st_dev) != (info.st_ino, info.st_dev):
            raise _authority_error(
                "the canonical engine was swapped while opening")
        chunks = []
        remaining = NXEXTRACT_ENGINE_MAX_BYTES + 1
        while remaining > 0:
            try:
                chunk = os.read(descriptor, min(remaining, 1 << 20))
            except OSError:
                raise _authority_error("cannot read the canonical engine")
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
    finally:
        os.close(descriptor)
    if len(data) != opened.st_size:
        raise _authority_error(
            "the canonical engine changed while being read")
    digest = hashlib.sha256(data).hexdigest()
    if digest != NXEXTRACT_ENGINE_SHA256:
        raise _authority_error(
            "the presented engine bytes do not match the pinned independent "
            "identity; refusing to execute them")
    return data, digest


def _authority_version_from_source(source_bytes):
    """Extract the single literal NXEXTRACT_VERSION from VERIFIED bytes."""
    try:
        tree = ast.parse(source_bytes.decode("utf-8"))
    except (SyntaxError, UnicodeDecodeError, ValueError):
        raise _authority_error("the engine source does not parse")
    declarations = []
    for node in tree.body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and \
                        target.id == "NXEXTRACT_VERSION":
                    declarations.append(node.value)
        elif isinstance(node, ast.AnnAssign) and \
                isinstance(node.target, ast.Name) and \
                node.target.id == "NXEXTRACT_VERSION":
            declarations.append(node.value)
    if len(declarations) != 1:
        raise _authority_error(
            "the engine must declare NXEXTRACT_VERSION exactly once at top "
            "level (found %d declarations)" % len(declarations))
    value = declarations[0]
    if not isinstance(value, ast.Constant) or \
            not isinstance(value.value, str):
        raise _authority_error(
            "NXEXTRACT_VERSION must be a single literal string, never an "
            "expression")
    return value.value


def _read_canonical_engine_version_file():
    path = NXEXTRACT_ROOT / "VERSION"
    try:
        info = os.lstat(path)
    except OSError:
        raise _authority_error("the canonical VERSION file is unreadable")
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise _authority_error(
            "the canonical VERSION file must be a regular non-symlink file")
    if info.st_size > 4096:
        raise _authority_error("the canonical VERSION file is too large")
    return read_version(path, "NXExtract")


def nxextract_recipe_authority():
    """The canonical NXExtract Recipe authority, authenticated before use.

    V4-04B: the generator never re-implements the recipe grammar.  V4-04C:
    not a single byte of the presented engine executes before its identity
    is proven — canonical fixed path only, component-wise symlink refusal,
    no-follow open validated by fstat, one bounded read, SHA-256 equal to
    the pinned independent identity, and the version extracted by AST from
    the verified bytes.  Only the authenticated snapshot is compiled, under
    a stable logical name; the pathname is never reopened for execution.
    The authority is cached only after the Recipe/NXError interface is
    proven; any failure leaves the cache empty."""
    global _NXEXTRACT_AUTHORITY, _NXEXTRACT_AUTHORITY_SHA256
    if _NXEXTRACT_AUTHORITY is not None:
        return _NXEXTRACT_AUTHORITY
    data, digest = _read_authenticated_engine()
    declared = _authority_version_from_source(data)
    recorded = _read_canonical_engine_version_file()
    if declared != NXEXTRACT_RECIPE_AUTHORITY_VERSION or (
            recorded != NXEXTRACT_RECIPE_AUTHORITY_VERSION):
        raise _authority_error(
            "version drifted: engine %r, VERSION file %r, nxgenerator "
            "requires %s"
            % (declared, recorded, NXEXTRACT_RECIPE_AUTHORITY_VERSION))
    module = types.ModuleType("nxextract_recipe_authority")
    module.__file__ = NXEXTRACT_AUTHORITY_LOGICAL_NAME
    try:
        code = compile(data, NXEXTRACT_AUTHORITY_LOGICAL_NAME, "exec",
                       dont_inherit=True)
        exec(code, module.__dict__)
    except Exception as error:
        raise _authority_error(
            "the verified engine failed to initialize (%s)"
            % type(error).__name__)
    recipe_class = getattr(module, "Recipe", None)
    error_class = getattr(module, "NXError", None)
    if not callable(recipe_class) or not (
            isinstance(error_class, type) and
            issubclass(error_class, BaseException)):
        raise _authority_error(
            "the verified engine does not expose the Recipe/NXError "
            "interface")
    _NXEXTRACT_AUTHORITY = module
    _NXEXTRACT_AUTHORITY_SHA256 = digest
    return _NXEXTRACT_AUTHORITY


def validate_recipe_with_nxextract_authority(recipe_source, logical_value):
    """Validate one extractor.json through the NXExtract Recipe authority.

    Returns the authority-validated document.  Read-only: no stage, marker,
    cache or file is created.  Error messages replace the host path with the
    manifest's logical value so no personal path leaks into reports."""
    authority = nxextract_recipe_authority()
    try:
        validated = authority.Recipe(str(recipe_source))
    except RecursionError:
        raise ProjectError(
            "NXExtract recipe refused by the canonical authority: "
            "JSON nesting exceeds the safe depth (%s)" % logical_value
        )
    except authority.NXError as error:
        message = str(error)
        for leak in {
            str(recipe_source),
            os.path.realpath(str(recipe_source)),
        }:
            message = message.replace(leak, str(logical_value))
        raise ProjectError(
            "NXExtract recipe refused by the canonical %s authority: %s"
            % (NXEXTRACT_RECIPE_AUTHORITY_VERSION, message)
        )
    return validated.data


def nxextract_source_state(architecture):
    if architecture not in ("aarch64", "armv7"):
        raise ProjectError("NXExtract UI supports only public ARM architectures")
    version = read_version(NXEXTRACT_ROOT / "VERSION", "NXExtract")
    release_manifest = require_regular_file(
        NXEXTRACT_RELEASE_MANIFEST, "NXExtract UI release manifest"
    )
    source = require_regular_file(NXEXTRACT_SOURCE, "NXExtract UI source")
    try:
        document = json.loads(release_manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as error:
        raise ProjectError("cannot read NXExtract UI release: %s" % error)
    if not isinstance(document, dict) or set(document) != {
        "artifacts", "component", "schema_version", "source_sha256",
        "toolchain", "version",
    }:
        raise ProjectError("invalid NXExtract UI release manifest")
    if (
        document["schema_version"] != 1
        or document["component"] != "nxextract-ui"
        or document["version"] != NXEXTRACT_UI_REQUIRED_VERSION
        or not isinstance(document["artifacts"], dict)
        or set(document["artifacts"]) != {
            "aarch64", "armv7", "i386", "x86_64"
        }
        or architecture not in document["artifacts"]
        or not isinstance(document["toolchain"], dict)
        or set(document["toolchain"]) != {
            "archive_sha256", "name", "version"
        }
        or document["toolchain"] != {
            "archive_sha256": (
                "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00"
            ),
            "name": "zig",
            "version": "0.16.0",
        }
    ):
        raise ProjectError("invalid NXExtract UI release manifest")
    if (
        not isinstance(document["source_sha256"], str)
        or not re.fullmatch(r"[0-9a-f]{64}", document["source_sha256"])
        or document["source_sha256"] != sha256_file(source)
    ):
        raise ProjectError("NXExtract UI source differs from its release manifest")
    record = document["artifacts"][architecture]
    if not isinstance(record, dict) or set(record) != {
        "glibc_max", "mode", "path", "sha256", "size"
    }:
        raise ProjectError("invalid NXExtract UI artifact record")
    artifact_value = require_string(
        record["path"], "NXExtract UI artifact path"
    )
    artifact_logical = PurePosixPath(artifact_value)
    if (
        artifact_logical.is_absolute()
        or not artifact_logical.parts
        or any(part in ("", ".", "..") for part in artifact_logical.parts)
    ):
        raise ProjectError("NXExtract UI artifact path is unsafe")
    artifact = NXEXTRACT_ROOT
    for part in artifact_logical.parts:
        artifact = artifact / part
        if artifact.is_symlink():
            raise ProjectError("NXExtract UI artifact path traverses a symlink")
    require_regular_file(artifact, "NXExtract UI artifact")
    expected_mode = "%04o" % stat.S_IMODE(artifact.stat().st_mode)
    expected_path = "ui/release/%s/nxextract-ui" % architecture
    payload = artifact.read_bytes()
    expected_class = 2 if architecture == "aarch64" else 1
    expected_machine = 183 if architecture == "aarch64" else 40
    expected_flags = None
    if architecture == "armv7" and len(payload) >= 40:
        expected_flags = int.from_bytes(payload[36:40], "little")
    if (
        record["path"] != expected_path
        or record["mode"] != expected_mode
        or record["mode"] != "0755"
        or not isinstance(record["sha256"], str)
        or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])
        or record["sha256"] != sha256_file(artifact)
        or isinstance(record["size"], bool)
        or not isinstance(record["size"], int)
        or record["size"] != artifact.stat().st_size
        or not isinstance(record["glibc_max"], str)
        or not re.fullmatch(r"[0-9]+[.][0-9]+", record["glibc_max"])
        or version_tuple(record["glibc_max"]) > (2, 30)
        or len(payload) < 20
        or payload[:4] != b"\x7fELF"
        or payload[4] != expected_class
        or payload[5] != 1
        or int.from_bytes(payload[16:18], "little") != 3
        or int.from_bytes(payload[18:20], "little") != expected_machine
        or (architecture == "armv7" and (
            expected_flags is None
            or expected_flags & 0x05000000 != 0x05000000
            or expected_flags & 0x00000400 == 0
        ))
    ):
        raise ProjectError("NXExtract UI artifact differs from its release manifest")
    return {
        "version": version,
        "ui_version": document["version"],
        "release_manifest_sha256": sha256_file(release_manifest),
        "source_sha256": sha256_file(source),
        "artifact": {
            "architecture": architecture,
            "path": artifact.relative_to(NXEXTRACT_ROOT).as_posix(),
            "mode": expected_mode,
            "sha256": record["sha256"],
        },
    }


def version_tuple(value):
    return tuple(int(part) for part in value.split("."))


def portmaster_source_state():
    contract_path = require_regular_file(
        PORTMASTER_CONTRACT, "PortMaster contract v3"
    )
    schema_path = require_regular_file(
        PORTMASTER_METADATA_SCHEMA, "PortMaster metadata schema"
    )
    provenance_path = require_regular_file(
        PORTMASTER_VENDOR_PROVENANCE, "PortMaster vendor provenance"
    )
    legacy_path = require_regular_file(
        PORTMASTER_LEGACY_RUNTIME, "PortMaster legacy runtime provenance"
    )
    try:
        contract = json.loads(contract_path.read_text(encoding="utf-8"))
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
        legacy = json.loads(legacy_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as error:
        raise ProjectError("cannot read the pinned PortMaster contract: %s" % error)
    if (
        contract.get("schema_version") != 3
        or contract.get("metadata_schema") !=
            "schema/port-json-supported-v2.schema.json"
        or contract.get("upstream_source") != "portmaster-gui-main-8f9ddc4"
        or contract.get("legacy_upstream_source") !=
            "portmaster-gui-2024.03.10-0841-7471d54"
        or provenance.get("commit") !=
            "8f9ddc4b0f75dfe61eb370bd3d1b4ec9d5ef6967"
        or legacy.get("commit") !=
            "7471d54c7c6ca57c16dee1b77cdd57d4226a1b86"
        or schema.get("properties", {}).get("attr", {}).get("required") !=
            ["title", "arch", "min_glibc"]
    ):
        raise ProjectError("the pinned PortMaster contract is incompatible")
    return {
        "contract": "v3",
        "contract_sha256": sha256_file(contract_path),
        "legacy_parser_commit": legacy["commit"],
        "legacy_runtime_fixture_sha256": sha256_file(legacy_path),
        "metadata_schema": "port-json-supported-v2",
        "metadata_schema_sha256": sha256_file(schema_path),
        "parser_commit": provenance["commit"],
        "vendor_provenance_sha256": sha256_file(provenance_path),
    }


def _version_tuple_str(value, label):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]+\.[0-9]+", value):
        raise ProjectError("%s must be MAJOR.MINOR" % label)
    major, minor = value.split(".")
    return (int(major), int(minor))


def validate_graphics(graphics, project_schema):
    """Validate the optional V3-GRAPHICS-02 context contract.

    The enums MUST match nxgl_graphics_contract.h so the schema and the C
    validator never disagree. Returns a normalised dict (with the adapter the
    port must vendor and call recorded), or None when absent. A non-GL port
    declares {"uses_gl": false} and NOTHING else; a GL port declares the full
    contract. Never decides by device/CFW/game name -- it only records what the
    port needs so the adapter can MEASURE the obtained context at runtime.
    """
    if graphics is None:
        return None
    if project_schema < 3:
        raise ProjectError("graphics requires nxproject schema_version 3")
    if not isinstance(graphics, dict):
        raise ProjectError("graphics must be an object")
    allowed = {"uses_gl", "api", "profile", "version", "version_policy",
               "version_max", "shader_dialect", "drawable_ready_timeout_ms",
               "adopt_single_channel", "required_devices", "egl_binding",
               "evidence_boundary"}
    unknown = set(graphics) - allowed
    if unknown:
        raise ProjectError(
            "graphics has unknown field(s): %s" % ", ".join(sorted(unknown))
        )
    if "uses_gl" not in graphics:
        raise ProjectError("graphics must declare uses_gl")
    uses_gl = graphics["uses_gl"]
    if not isinstance(uses_gl, bool):
        raise ProjectError("graphics.uses_gl must be a boolean")

    contract_keys = {"api", "profile", "version", "version_policy",
                     "shader_dialect"}
    # V4-GRAPHICS-03 is validated by validate_egl_binding, not by the context
    # contract: it is a loader concern, not an EGLConfig requirement.
    graphics = {key: value for key, value in graphics.items()
                if key != "egl_binding"}
    optional_keys = {"version_max", "drawable_ready_timeout_ms",
                     "adopt_single_channel", "required_devices",
                     "evidence_boundary"}
    present = set(graphics) - {"uses_gl"}
    if not uses_gl:
        if present:
            raise ProjectError(
                "graphics.uses_gl false must declare no context fields"
            )
        return {"uses_gl": False}

    missing = contract_keys - present
    if missing:
        raise ProjectError(
            "graphics.uses_gl true must declare %s"
            % ", ".join(sorted(contract_keys))
        )
    stray = present - contract_keys - optional_keys
    if stray:
        raise ProjectError(
            "graphics has unknown field(s): %s" % ", ".join(sorted(stray))
        )

    if graphics["api"] not in GRAPHICS_API:
        raise ProjectError(
            "graphics.api must be one of %s" % ", ".join(GRAPHICS_API)
        )
    if graphics["profile"] not in GRAPHICS_PROFILE:
        raise ProjectError(
            "graphics.profile must be one of %s" % ", ".join(GRAPHICS_PROFILE)
        )
    if graphics["version_policy"] not in GRAPHICS_VERSION_POLICY:
        raise ProjectError(
            "graphics.version_policy must be one of %s"
            % ", ".join(GRAPHICS_VERSION_POLICY)
        )
    if graphics["shader_dialect"] not in GRAPHICS_SHADER_DIALECT:
        raise ProjectError(
            "graphics.shader_dialect must be one of %s"
            % ", ".join(GRAPHICS_SHADER_DIALECT)
        )
    # API/profile coherence mirrors the C contract: a GLES context is profile
    # es; a desktop GL context is core or compat.
    api = graphics["api"]
    profile = graphics["profile"]
    if api == "gles" and profile != "es":
        raise ProjectError("graphics api gles requires profile es")
    if api == "gl" and profile == "es":
        raise ProjectError("graphics api gl requires profile core or compat")
    # A GLES contract must carry an ESSL dialect; a desktop GL contract cannot.
    dialect = graphics["shader_dialect"]
    if api == "gles" and dialect == "glsl-any":
        raise ProjectError("graphics api gles requires an ESSL shader_dialect")
    if api == "gl" and dialect != "glsl-any":
        raise ProjectError("graphics api gl requires shader_dialect glsl-any")

    version = _version_tuple_str(graphics["version"], "graphics.version")
    policy = graphics["version_policy"]
    if policy == "range":
        if "version_max" not in graphics:
            raise ProjectError(
                "graphics.version_policy range requires version_max"
            )
        vmax = _version_tuple_str(graphics["version_max"], "graphics.version_max")
        if vmax < version:
            raise ProjectError(
                "graphics.version_max must be >= version for a range policy"
            )
    elif "version_max" in graphics:
        raise ProjectError(
            "graphics.version_max is only valid for a range policy"
        )

    timeout = graphics.get("drawable_ready_timeout_ms", 5000)
    if isinstance(timeout, bool) or not isinstance(timeout, int) or \
            timeout < 0 or timeout > 60000:
        raise ProjectError(
            "graphics.drawable_ready_timeout_ms must be 0..60000"
        )
    adopt_single_channel = graphics.get("adopt_single_channel", False)
    if not isinstance(adopt_single_channel, bool):
        raise ProjectError("graphics.adopt_single_channel must be a boolean")
    # V4-GRAPHICS-04: the post-first-present boundary is a declarative opt-in.
    # ABSENCE preserves the previous boundary and the exact bytes of every
    # existing port; the generator only normalizes/emits the field when the
    # port declared it. Never enabled by CFW or Wayland autodetection.
    evidence_boundary = graphics.get("evidence_boundary")
    if evidence_boundary is not None and \
            evidence_boundary not in GRAPHICS_EVIDENCE_BOUNDARY:
        raise ProjectError(
            "graphics.evidence_boundary must be one of %s"
            % ", ".join(GRAPHICS_EVIDENCE_BOUNDARY)
        )
    required_devices = graphics.get("required_devices")
    if required_devices is not None:
        required_devices = require_unique_strings(
            required_devices, "graphics.required_devices"
        )
        if not required_devices:
            raise ProjectError(
                "graphics.required_devices must not be empty when present"
            )

    normalised = {
        "uses_gl": True,
        "api": api,
        "profile": profile,
        "version": graphics["version"],
        "version_policy": policy,
        "shader_dialect": dialect,
        "drawable_ready_timeout_ms": timeout,
        "adopt_single_channel": adopt_single_channel,
        # The adapter the port must vendor and call after context creation; the
        # runtime GRAPHICS-EVIDENCE receipt is what a release proof links to.
        "adapter": GRAPHICS_ADAPTER,
    }
    if policy == "range":
        normalised["version_max"] = graphics["version_max"]
    if required_devices is not None:
        normalised["required_devices"] = required_devices
    if evidence_boundary is not None:
        normalised["evidence_boundary"] = evidence_boundary
    return normalised


DISPLAY_POLICIES = ("game", "preserve", "adaptive", "fill", "stretch")
DISPLAY_MAX_EXTENT = 65535
EGL_BINDING_MAX_IMPORTS = 64


def validate_video(video, project_schema):
    """Validate the V5 7A.3 owner video contract (NEXTOS_SETTINGS/2).

    ABSENCE IS NO-OP: no video keys are emitted and the /1 settings seed is
    kept byte-identical. With the block, defaults/NEXTOSSETTINGS.txt is
    written in schema /2 with EXPLICIT video keys (the owner sees every one
    of them), the port declares which aspect policies it really implements
    (a policy it does not implement is refused, never faked), and `auto` is
    only accepted together with a declared TOTAL algorithm. Nothing here may
    be decided by device, CFW or model: the runtime measures the drawable.
    """
    if video is None:
        return None
    if project_schema < 3:
        raise ProjectError("video requires nxproject schema_version 3")
    if not isinstance(video, dict):
        raise ProjectError("video must be an object")
    allowed = {"authority", "aspect", "aspect_policies", "auto_algorithm",
               "output_size", "filter", "invalid_policy", "native_config"}
    unknown = set(video) - allowed
    if unknown:
        raise ProjectError(
            "video has unknown field(s): %s" % ", ".join(sorted(unknown)))
    for key in ("authority", "aspect_policies", "invalid_policy"):
        if key not in video:
            raise ProjectError("video.%s is required" % key)
    authority = video["authority"]
    if authority not in VIDEO_AUTHORITIES:
        raise ProjectError(
            "video.authority must be one of %s" % ", ".join(VIDEO_AUTHORITIES))
    policies = video["aspect_policies"]
    if (not isinstance(policies, list) or not policies or
            len(set(policies)) != len(policies) or
            any(p not in VIDEO_ASPECTS for p in policies)):
        raise ProjectError(
            "video.aspect_policies must be a non-empty unique list drawn from %s"
            % ", ".join(VIDEO_ASPECTS))
    algorithm = video.get("auto_algorithm", "none")
    if algorithm not in VIDEO_AUTO_ALGORITHMS:
        raise ProjectError(
            "video.auto_algorithm must be one of %s"
            % ", ".join(VIDEO_AUTO_ALGORITHMS))
    if "auto" in policies and algorithm == "none":
        raise ProjectError(
            "video.aspect_policies includes auto but no total auto_algorithm "
            "is declared (stretch|ratio-threshold|epsilon)")
    if "auto" not in policies and algorithm != "none":
        raise ProjectError(
            "video.auto_algorithm declared without auto in aspect_policies")
    aspect = video.get("aspect", policies[0])
    if aspect not in policies:
        raise ProjectError(
            "video.aspect default %r is not among the declared aspect_policies"
            % aspect)
    output_size = video.get("output_size", "display")
    if output_size not in VIDEO_OUTPUT_SIZES:
        if not re.fullmatch(r"[1-9][0-9]{0,3}x[1-9][0-9]{0,3}", output_size or ""):
            raise ProjectError(
                "video.output_size must be one of %s or WxH"
                % ", ".join(VIDEO_OUTPUT_SIZES))
        width, height = (int(part) for part in output_size.split("x"))
        if width > 8192 or height > 8192:
            raise ProjectError("video.output_size exceeds 8192")
    filter_name = video.get("filter", "engine")
    if filter_name not in VIDEO_FILTERS:
        raise ProjectError(
            "video.filter must be one of %s" % ", ".join(VIDEO_FILTERS))
    invalid_policy = video["invalid_policy"]
    if invalid_policy not in VIDEO_INVALID_POLICIES:
        raise ProjectError(
            "video.invalid_policy must be one of %s"
            % ", ".join(VIDEO_INVALID_POLICIES))
    native_config = video.get("native_config")
    if authority in ("engine", "synchronized") and not native_config:
        raise ProjectError(
            "video.authority %s requires video.native_config (the engine's "
            "owner-native file)" % authority)
    if native_config is not None:
        if (not isinstance(native_config, str) or not native_config or
                native_config.startswith("/") or ".." in native_config.split("/")):
            raise ProjectError("video.native_config must be a relative path")
    return {
        "authority": authority,
        "aspect": aspect,
        "aspect_policies": list(policies),
        "auto_algorithm": algorithm,
        "output_size": output_size,
        "filter": filter_name,
        "invalid_policy": invalid_policy,
        "native_config": native_config,
    }


def validate_owner_runtime(owner_runtime, nxport, project_schema, source_root):
    """Validate the V5 7A.1 owner-runtime opt-in at the project level.

    The nxport side (`owner_runtime: "1"`) is validated by nxbootstrap; here
    the project must supply the SEED of the live hook (copied to
    defaults/port-env.sh, never to the live path) and both sides must agree.
    """
    declared = nxport.get("owner_runtime") == "1"
    if owner_runtime is None:
        if declared:
            raise ProjectError(
                "nxport.owner_runtime is declared but the project has no "
                "owner_runtime block (hook_seed)")
        return None
    if project_schema < 3:
        raise ProjectError("owner_runtime requires nxproject schema_version 3")
    if not declared:
        raise ProjectError(
            "owner_runtime block requires nxport.owner_runtime \"1\"")
    block = require_object(owner_runtime, "owner_runtime", {"hook_seed"})
    seed = safe_repository_file(
        block["hook_seed"], "owner_runtime.hook_seed", source_root)
    return {"hook_seed": seed}


def validate_display(display, project_schema):
    """Validate the optional V4-DISPLAY-01 presentation contract.

    The enums MUST match nxgl_display.h. ABSENCE OF THE BLOCK IS `game`: the
    framework installs nothing and every already approved port keeps
    byte-identical behaviour. `preserve` letterboxes, which changes pixels, so
    it is an explicit opt-in and never a default. Nothing here may be decided
    by device, CFW, GPU or game name; the runtime measures the real drawable.
    """
    if display is None:
        return None
    if project_schema < 3:
        raise ProjectError("display requires nxproject schema_version 3")
    if not isinstance(display, dict):
        raise ProjectError("display must be an object")
    allowed = {"policy", "internal_width", "internal_height", "min_width",
               "min_height", "max_width", "max_height", "remap_input"}
    unknown = set(display) - allowed
    if unknown:
        raise ProjectError(
            "display has unknown field(s): %s" % ", ".join(sorted(unknown)))
    policy = display.get("policy")
    if policy not in DISPLAY_POLICIES:
        raise ProjectError(
            "display.policy must be one of %s" % ", ".join(DISPLAY_POLICIES))
    remap_input = display.get("remap_input", False)
    if not isinstance(remap_input, bool):
        raise ProjectError("display.remap_input must be a boolean")

    if policy == "game":
        stray = set(display) - {"policy"}
        if stray:
            raise ProjectError(
                "display.policy game must declare no other field")
        return {"policy": "game", "remap_input": False}

    def extent(name, required):
        value = display.get(name)
        if value is None:
            if required:
                raise ProjectError("display.%s is required for policy %s"
                                   % (name, policy))
            return 0
        if not isinstance(value, int) or isinstance(value, bool):
            raise ProjectError("display.%s must be an integer" % name)
        if value < 1 or value > DISPLAY_MAX_EXTENT:
            raise ProjectError("display.%s is outside 1..%d"
                               % (name, DISPLAY_MAX_EXTENT))
        return value

    normalized = {
        "policy": policy,
        "internal_width": extent("internal_width", True),
        "internal_height": extent("internal_height", True),
        "min_width": extent("min_width", False),
        "min_height": extent("min_height", False),
        "max_width": extent("max_width", False),
        "max_height": extent("max_height", False),
        "remap_input": remap_input,
    }
    for low, high in (("min_width", "min_height"), ("max_width", "max_height")):
        if bool(normalized[low]) != bool(normalized[high]):
            raise ProjectError(
                "display %s and %s must be declared together" % (low, high))
    if policy != "adaptive" and (normalized["min_width"] or
                                 normalized["max_width"]):
        raise ProjectError("display limits only apply to policy adaptive")
    if policy == "adaptive" and not normalized["max_width"]:
        raise ProjectError("display.policy adaptive must declare max_width "
                           "and max_height")
    if (normalized["min_width"] and normalized["max_width"] and
            (normalized["max_width"] < normalized["min_width"] or
             normalized["max_height"] < normalized["min_height"])):
        raise ProjectError("display max extents are below the min extents")
    return normalized


def validate_egl_binding(binding, project_schema, graphics):
    """Validate the optional V4-GRAPHICS-03 EGL binding declaration.

    Absence is disabled. When enabled, the port must declare the EXACT EGL
    import inventory of its guest; nxrelease audits the shipped ELF against
    this list and `eglGetCurrentContext` is mandatory because it is the symbol
    that proves ownership of the current context.
    """
    if binding is None:
        return None
    if project_schema < 3:
        raise ProjectError("egl_binding requires nxproject schema_version 3")
    if not isinstance(binding, dict):
        raise ProjectError("egl_binding must be an object")
    if set(binding) != {"enabled", "imports"}:
        raise ProjectError("egl_binding must declare exactly enabled and imports")
    enabled = binding["enabled"]
    if not isinstance(enabled, bool):
        raise ProjectError("egl_binding.enabled must be a boolean")
    imports = binding["imports"]
    if not isinstance(imports, list):
        raise ProjectError("egl_binding.imports must be an array")
    if not enabled:
        if imports:
            raise ProjectError(
                "egl_binding disabled must declare an empty import inventory")
        return {"enabled": False, "imports": []}
    if graphics is None or not graphics.get("uses_gl"):
        raise ProjectError("egl_binding requires a graphics contract with "
                           "uses_gl true")
    if not imports or len(imports) > EGL_BINDING_MAX_IMPORTS:
        raise ProjectError("egl_binding.imports must hold 1..%d names"
                           % EGL_BINDING_MAX_IMPORTS)
    seen = set()
    for name in imports:
        if not isinstance(name, str) or not re.fullmatch(r"egl[A-Za-z0-9_]+",
                                                         name):
            raise ProjectError(
                "egl_binding.imports entries must be EGL symbol names")
        if name in seen:
            raise ProjectError("egl_binding.imports has a duplicate: %s" % name)
        seen.add(name)
    if imports != sorted(imports):
        raise ProjectError("egl_binding.imports must be lexicographically "
                           "ordered")
    if "eglGetCurrentContext" not in seen:
        raise ProjectError("egl_binding.imports must include "
                           "eglGetCurrentContext")
    return {"enabled": True, "imports": list(imports)}


def validate_sdl3_portmaster(block, project_schema):
    """Validate the optional V4-CONTROLLERS-02 opt-in. Absence is disabled."""
    if block is None:
        return None
    if project_schema < 3:
        raise ProjectError(
            "sdl3_portmaster requires nxproject schema_version 3")
    if not isinstance(block, dict):
        raise ProjectError("sdl3_portmaster must be an object")
    if set(block) != {"enabled", "private_sdl3_sha256"}:
        raise ProjectError("sdl3_portmaster must declare exactly enabled and "
                           "private_sdl3_sha256")
    enabled = block["enabled"]
    if not isinstance(enabled, bool):
        raise ProjectError("sdl3_portmaster.enabled must be a boolean")
    digest = block["private_sdl3_sha256"]
    if not enabled:
        if digest not in ("", None):
            raise ProjectError("sdl3_portmaster disabled must not pin an SDL3")
        return {"enabled": False, "private_sdl3_sha256": ""}
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ProjectError(
            "sdl3_portmaster enabled must pin the private SDL3 SHA-256")
    return {"enabled": True, "private_sdl3_sha256": digest}


def validate_controller_profiles(block, project_schema):
    """Validate the optional V4-CONTROLLERS-03/C3 opt-in: the
    NXCONTROLLER_PROFILES/1 bundle pinned inside the port ZIP (authority 3 of
    the sovereign mapping order). Absence is disabled and preserves the
    previous bytes and behavior; nothing here ever points at `latest` or a
    runtime download -- only a content-addressed file inside the package."""
    if block is None:
        return None
    if project_schema < 3:
        raise ProjectError(
            "controller_profiles requires nxproject schema_version 3")
    if not isinstance(block, dict):
        raise ProjectError("controller_profiles must be an object")
    allowed = {"enabled", "bundle", "sha256"}
    has_variants = "face_layout_variants" in block
    if set(block) - {"face_layout_variants"} != allowed:
        raise ProjectError("controller_profiles must declare exactly "
                           "enabled, bundle and sha256 (plus the optional "
                           "face_layout_variants under controls.schema 3)")
    enabled = block["enabled"]
    if not isinstance(enabled, bool):
        raise ProjectError("controller_profiles.enabled must be a boolean")
    bundle = block["bundle"]
    digest = block["sha256"]
    if not enabled:
        if digest not in ("", None) or bundle not in ("", None) or has_variants:
            raise ProjectError(
                "controller_profiles disabled must not pin a bundle")
        return {"enabled": False, "bundle": "", "sha256": ""}
    if (not isinstance(bundle, str) or not bundle or len(bundle) > 128 or
            not re.fullmatch(r"[A-Za-z0-9._-]+", bundle) or
            bundle.startswith(".")):
        raise ProjectError(
            "controller_profiles.bundle must be a plain file name inside "
            "the port (no path separators)")
    if bundle != "controllers.nxb":
        raise ProjectError(
            "controller_profiles.bundle must be exactly controllers.nxb: "
            "the runtime authority-3 declaration uses that canonical name")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}",
                                                       digest):
        raise ProjectError(
            "controller_profiles enabled must pin the bundle SHA-256")
    result = {"enabled": True, "bundle": bundle, "sha256": digest}
    if has_variants:
        # nxinput 0.10.0: the FACE_LAYOUT variant pair. The base bundle stays
        # the invariant authority-3 declaration for `auto`; the two variants
        # are complete NXCONTROLLER_PROFILES/1 files of their own, pinned by
        # SHA-256 and consulted ONLY when the port's FACE_LAYOUT selects
        # them. The pair is mandatory and the names are fixed -- a single
        # variant, a renamed file or a missing pin fails closed.
        variants = block["face_layout_variants"]
        if not isinstance(variants, dict) or set(variants) != {"modern",
                                                               "retro"}:
            raise ProjectError(
                "face_layout_variants must declare exactly the complete "
                "modern and retro pair")
        checked = {}
        for name in ("modern", "retro"):
            entry = variants[name]
            expected_bundle = "controllers-%s.nxb" % name
            if (not isinstance(entry, dict) or
                    set(entry) != {"bundle", "sha256"}):
                raise ProjectError(
                    "face_layout_variants.%s must declare exactly bundle "
                    "and sha256" % name)
            if entry["bundle"] != expected_bundle:
                raise ProjectError(
                    "face_layout_variants.%s.bundle must be exactly %s"
                    % (name, expected_bundle))
            if (not isinstance(entry["sha256"], str) or
                    not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"])):
                raise ProjectError(
                    "face_layout_variants.%s must pin the variant SHA-256"
                    % name)
            checked[name] = {"bundle": expected_bundle,
                             "sha256": entry["sha256"]}
        if checked["modern"]["sha256"] == checked["retro"]["sha256"]:
            raise ProjectError(
                "face_layout_variants modern and retro must be distinct "
                "artifacts (equal pins would freeze one face layout twice)")
        result["face_layout_variants"] = checked
    return result


def validate_runtime_mapping_authority_bundle(controls, controller_profiles):
    """Require the packaged authority whenever live nxinput owns mappings."""
    if (controls is not None and
            controls.get("runtime_mapping") == "nxinput-gptk" and
            (controller_profiles is None or
             not controller_profiles["enabled"])):
        raise ProjectError(
            "controls.runtime_mapping nxinput-gptk requires an enabled "
            "controls.controller_profiles bundle: authority 3 of the "
            "sovereign order must ship inside the ZIP")


def validate_controls(controls, project_schema):
    """Validate the semantic controller contract used to render GPTK.

    Schema-v3 ports cannot inherit a generic player.primary template: every
    action is named by the port, carries at least one real adapter sink, and
    every default binding references that allowlist.  The SDL/PortMaster
    mapping remains the sole physical-button authority; this block only maps
    canonical logical controls to semantic actions.
    """
    if controls is None:
        if project_schema >= 3:
            raise ProjectError(
                "nxproject schema_version 3 must declare controls actions, "
                "contexts and sinks"
            )
        return None
    if project_schema < 3:
        raise ProjectError("controls requires nxproject schema_version 3")
    if not isinstance(controls, dict):
        raise ProjectError("controls must be an object")
    allowed_control_keys = {"actions", "contexts", "tuning",
                            "sdl3_portmaster", "controller_profiles",
                            "schema", "runtime_mapping", "face_layout",
                            "proof"}
    if (not {"actions", "contexts"} <= set(controls) or
            not set(controls) <= allowed_control_keys):
        raise ProjectError(
            "controls must contain actions and contexts, with optional tuning, "
            "sdl3_portmaster, controller_profiles, schema, face_layout and proof"
        )
    # V4-CONTROLLERS-03/C4: NEXTOSCONTROLLERS v2 is OPT-IN per port. Absence
    # means format 1 and retains its binding body; from 0.3.12 the separate
    # static/live header truthfully describes whether edits reach the engine.
    # V2 is never inferred and never migrates an approved ZIP on its own.
    gptk_schema = controls.get("schema", 1)
    if gptk_schema not in (1, 2, 3, 4) or isinstance(gptk_schema, bool):
        raise ProjectError("controls.schema must be 1, 2, 3 or 4")
    # V5 (nxinput 0.11.0): NEXTOS_CONTROLLERS/4 is the positional Xbox
    # surface with ONE unified base map and sparse overrides. It is live-only
    # (the owner's edit must reach the engine) and carries no FACE_LAYOUT:
    # A/B/X/Y are positions, the plastic legend is evidence only.
    if gptk_schema == 4:
        if "face_layout" in controls:
            raise ProjectError(
                "controls.face_layout is a schema-3 field; schema 4 is "
                "positional (Xbox) and never selects by the plastic legend")
        if controls.get("runtime_mapping") != "nxinput-gptk":
            raise ProjectError(
                "controls.schema 4 requires runtime_mapping nxinput-gptk "
                "(the owner map is live by definition)")
        if "gameplay" not in controls.get("contexts", {}):
            raise ProjectError(
                "controls.schema 4 requires a gameplay context: it is the "
                "unified [base] map; menu/cursor become sparse overrides")
    # nxinput 0.10.0: NEXTOS_CONTROLLERS/3 inherits V2 completeness and
    # tri-state and adds the mandatory FACE_LAYOUT preamble line. The value
    # here is the DEFAULT the port ships (normally auto); the layout never
    # outranks the live mapping authorities at runtime.
    face_layout = controls.get("face_layout")
    if face_layout is not None and gptk_schema != 3:
        raise ProjectError(
            "controls.face_layout requires controls.schema 3")
    if gptk_schema == 3:
        if face_layout is None:
            face_layout = "auto"
        if face_layout not in ("auto", "modern", "retro"):
            raise ProjectError(
                "controls.face_layout must be auto, modern or retro "
                "(lowercase, exact)")
    # TEARSCAPE-CONTROLS-LIVE: the LIVE runtime is an explicit opt-in. Only a
    # port that declares controls.runtime_mapping = "nxinput-gptk" may claim
    # that NEXTOSCONTROLLERS.gptk is editable/functional; the nxrelease gate
    # enforces the claim against the packaged executable. Absence selects the
    # static header and never invents a live-runtime contract.
    runtime_mapping = controls.get("runtime_mapping")
    if "runtime_mapping" in controls and runtime_mapping != "nxinput-gptk":
        raise ProjectError(
            "controls.runtime_mapping must be nxinput-gptk when declared"
        )

    raw_actions = controls["actions"]
    if not isinstance(raw_actions, list) or not raw_actions:
        raise ProjectError("controls.actions must be a non-empty array")
    if len(raw_actions) > 64:
        raise ProjectError("controls.actions exceeds the 64-action limit")
    actions = []
    action_names = set()
    sink_pairs = set()
    for index, raw_action in enumerate(raw_actions):
        context = "controls.actions[%d]" % index
        action = require_object(
            raw_action, context, {"id", "kind", "sinks"}
        )
        action_id = require_string(
            action["id"], context + ".id", GPTK_ACTION_RE
        )
        if len(action_id) > 64:
            raise ProjectError("%s.id exceeds 64 characters" % context)
        if action_id in action_names:
            raise ProjectError("controls.actions contains duplicate id %r" % action_id)
        action_names.add(action_id)
        kind = action["kind"]
        if kind not in GPTK_ACTION_KINDS:
            raise ProjectError(
                "%s.kind must be button, axis or vector" % context
            )
        sinks = require_unique_strings(action["sinks"], context + ".sinks")
        if not sinks:
            raise ProjectError("%s.sinks must not be empty" % context)
        if len(sinks) > 8:
            raise ProjectError("%s.sinks exceeds the 8-sink limit" % context)
        for sink in sinks:
            if len(sink) > 96 or not GPTK_SINK_RE.fullmatch(sink):
                raise ProjectError("%s has an invalid sink %r" % (context, sink))
            pair = (action_id, sink)
            if pair in sink_pairs:
                raise ProjectError(
                    "controls declares duplicate action/sink %r -> %r"
                    % pair
                )
            sink_pairs.add(pair)
        actions.append({"id": action_id, "kind": kind, "sinks": sinks})

    raw_contexts = controls["contexts"]
    if not isinstance(raw_contexts, dict):
        raise ProjectError("controls.contexts must be an object")
    unknown_contexts = set(raw_contexts) - set(GPTK_CONTEXTS)
    missing_contexts = set(GPTK_REQUIRED_CONTEXTS) - set(raw_contexts)
    if unknown_contexts:
        raise ProjectError(
            "controls.contexts has unknown context(s): %s"
            % ", ".join(sorted(unknown_contexts))
        )
    if missing_contexts:
        raise ProjectError(
            "controls.contexts must declare menu and gameplay"
        )
    contexts = {}
    used_actions = set()
    for context_name in GPTK_CONTEXTS:
        if context_name not in raw_contexts:
            continue
        bindings = raw_contexts[context_name]
        if not isinstance(bindings, dict) or not bindings:
            raise ProjectError(
                "controls.contexts.%s must be a non-empty object" % context_name
            )
        if len(bindings) > len(GPTK_CONTROLS):
            raise ProjectError(
                "controls.contexts.%s has too many bindings" % context_name
            )
        normalised_bindings = {}
        for control, action_id in bindings.items():
            if control not in GPTK_CONTROLS:
                raise ProjectError(
                    "controls.contexts.%s has unknown control %r"
                    % (context_name, control)
                )
            # C4: `native` is a DECLARED passthrough, only in schema 2. It
            # has no sink by construction -- the adapter reads the control
            # itself -- so it is never checked against the action allowlist.
            if action_id == GPTK_NATIVE:
                if gptk_schema < 2:
                    raise ProjectError(
                        "controls.contexts.%s.%s uses native, which requires "
                        "controls.schema 2" % (context_name, control)
                    )
                normalised_bindings[control] = GPTK_NATIVE
                continue
            if action_id == GPTK_NULL:
                # Declared suppression. In schema >= 2 spelling null is how a
                # port states ON PURPOSE that a control delivers nothing --
                # required for the D-pad guard below. Schema 1 has no words
                # for governed nothing and keeps refusing it.
                if gptk_schema < 2:
                    raise ProjectError(
                        "controls.contexts.%s.%s uses null, which requires "
                        "controls.schema 2" % (context_name, control)
                    )
                normalised_bindings[control] = GPTK_NULL
                continue
            require_string(
                action_id,
                "controls.contexts.%s.%s" % (context_name, control),
                GPTK_ACTION_RE,
            )
            if action_id not in action_names:
                raise ProjectError(
                    "controls.contexts.%s.%s references action %r without a sink"
                    % (context_name, control, action_id)
                )
            normalised_bindings[control] = action_id
            used_actions.add(action_id)
        # FIELD REGRESSION GUARD (seen twice: a schema-1 port migrates, the
        # D-pad is not spelled, the complete tri-state writes null for it and
        # the whole navigation cluster goes silently dead while the stick
        # still works). In schema >= 2 the D-pad intent is never implicit:
        # every context declares all four of UP/DOWN/LEFT/RIGHT, as an
        # action, the native passthrough, or an explicit null.
        if gptk_schema >= 2 and context_name != "cursor":
            undeclared_dpad = [
                control for control in ("UP", "DOWN", "LEFT", "RIGHT")
                if control not in normalised_bindings
            ]
            if undeclared_dpad:
                raise ProjectError(
                    "controls.contexts.%s leaves the D-pad undeclared (%s): "
                    "schema %d suppresses omitted controls, so declare each "
                    "of UP/DOWN/LEFT/RIGHT as an action, \"native\" or an "
                    "explicit \"null\""
                    % (context_name, ", ".join(undeclared_dpad), gptk_schema)
                )
        contexts[context_name] = normalised_bindings

    # A declared action may be an owner-remapping alternative not present in
    # the immutable default.  It still has a sink and is therefore safe.  But
    # every default action must be backed by exactly the declared action entry.
    if "cursor" in contexts:
        cursor = contexts["cursor"]
        if (not str(cursor.get("RIGHT_STICK", "")).startswith("cursor.") or
                "R3" not in cursor):
            raise ProjectError(
                "a cursor context must bind RIGHT_STICK to cursor.* and bind R3"
            )
        for stolen in ("A", "UP", "DOWN", "LEFT", "RIGHT"):
            if cursor.get(stolen, "").startswith("cursor."):
                raise ProjectError(
                    "cursor context must not steal A or D-pad for cursor control"
                )

    by_id = {item["id"]: item for item in actions}
    for context_name, bindings in contexts.items():
        for control, action_id in bindings.items():
            if action_id in (GPTK_NATIVE, GPTK_NULL):
                # A declared passthrough has no kind to check, and a declared
                # suppression delivers nothing at all.
                continue
            kind = by_id[action_id]["kind"]
            if control in ("LEFT_STICK", "RIGHT_STICK"):
                if kind != "vector":
                    raise ProjectError(
                        "%s.%s requires a vector action" % (context_name, control)
                    )
            elif kind == "vector":
                raise ProjectError(
                    "%s.%s cannot bind a vector action" % (context_name, control)
                )
    if "cursor" in contexts:
        cursor = contexts["cursor"]
        if (by_id[cursor["RIGHT_STICK"]]["kind"] != "vector" or
                by_id[cursor["R3"]]["kind"] != "button"):
            raise ProjectError(
                "cursor RIGHT_STICK must be vector and R3 must be button"
            )

    result = {"actions": actions, "contexts": contexts,
              "runtime_mapping": runtime_mapping,
              "schema": gptk_schema}
    if gptk_schema == 3:
        result["face_layout"] = face_layout
    if runtime_mapping == "nxinput-gptk":
        # Generated policy, not a port-authored slogan.  A live mapping may
        # consume only after the adapter proves a context and complete sink
        # coverage; nxrelease closes the same contract against the ELF and
        # the external event evidence.
        result["runtime_contract"] = dict(GPTK_LIVE_RUNTIME_CONTRACT)
    if "tuning" in controls:
        result["tuning"] = validate_controls_tuning(
            controls["tuning"], contexts, by_id
        )
    # nxinput 0.10.2: controls.proof is validated here but never enters the
    # adapter contract; it only feeds the generated on-device proof roteiros.
    if "proof" in controls:
        validate_controls_proof(controls["proof"], contexts, by_id)
    return result


def _bounded_tuning_number(value, context, minimum, maximum):
    if (isinstance(value, bool) or not isinstance(value, (int, float)) or
            value < minimum or value > maximum or not math.isfinite(value)):
        raise ProjectError(
            "%s must be a finite number in the range %s..%s" %
            (context, minimum, maximum)
        )
    return value


PROOF_STEP_OPS = ("press", "hold", "sleep_ms", "wait_log", "mark", "expect")
PROOF_EXPECT_KINDS = ("log", "no_log", "quiet")


def nxgenerator_version():
    return read_version(ROOT / "VERSION", "nxgenerator")


def validate_controls_proof(proof, contexts, actions_by_id):
    """nxinput 0.10.2 / ON_DEVICE_AUTOMATED_INPUT_PROOF.

    The port supplies ONLY what the framework cannot know: how to reach each
    declared context on the real game (navigation steps), which control in
    the menu context must never select QUIT, and which gameplay control the
    owner-remap round moves. Everything else — every bound control, every
    `null`, every `native`, neutrality, press/release once, the negative
    chords, the cross-pad denial and SELECT+START — is generated from the
    actions/contexts/sinks the port already declares.
    """
    if not isinstance(proof, dict) or not proof:
        raise ProjectError("controls.proof must be a non-empty object")
    allowed = {"navigation", "quit_guard", "owner_remap", "clones", "effects"}
    if not set(proof) <= allowed or "navigation" not in proof:
        raise ProjectError(
            "controls.proof must contain navigation, with optional quit_guard, "
            "owner_remap, clones and effects")
    # nxgenerator 0.4.2 / nxinput 0.11.6: the EFFECT the engine must show for
    # an action. A delivery receipt is the adapter's word; the engine may
    # still ignore the input (FP2 1.1.4: a hook on the polling name killed
    # every button while every delivery kept passing). The port declares,
    # per action, the context proof the engine publishes after it (the
    # adapter's kind=context receipt), and the roteiro demands it right
    # after the delivery. Declare it only for actions whose effect leaves
    # the session where the generated coverage expects it (START toggling a
    # pause is pressed again by the roteiro; a confirm that changes screen
    # is NOT self-reverting and must stay undeclared).
    effects = proof.get("effects", {})
    if not isinstance(effects, dict):
        raise ProjectError("controls.proof.effects must be an object: action -> {context, source_regex?}")
    for action_id, effect in effects.items():
        where = "controls.proof.effects.%s" % action_id
        if action_id not in actions_by_id:
            raise ProjectError(where + " names an undeclared action")
        if (not isinstance(effect, dict) or not {"context"} <= set(effect) <= {"context", "source_regex", "contexts"} or
                effect["context"] not in contexts):
            raise ProjectError(where + " must be {context: <declared context>, source_regex?: <regex>, contexts?: [<declared context>...]}")
        if "contexts" in effect:
            # the contexts IN WHICH the stimulus produces this effect (FP2:
            # START pauses inside a stage, never on the title/main menu)
            if (not isinstance(effect["contexts"], list) or not effect["contexts"] or
                    any(c not in contexts for c in effect["contexts"])):
                raise ProjectError(where + ".contexts must be a non-empty list of declared contexts")
        if "source_regex" in effect:
            if not isinstance(effect["source_regex"], str) or not effect["source_regex"]:
                raise ProjectError(where + ".source_regex must be a non-empty regex string")
            try:
                re.compile(effect["source_regex"])
            except re.error as error:
                raise ProjectError(where + ".source_regex is not a valid regex: %s" % error)
    navigation = proof["navigation"]
    if not isinstance(navigation, dict) or set(navigation) != set(contexts):
        raise ProjectError(
            "controls.proof.navigation must have one step list per declared "
            "context: %s" % ", ".join(sorted(contexts)))
    for context_name, steps in navigation.items():
        if not isinstance(steps, list):
            raise ProjectError(
                "controls.proof.navigation.%s must be a list" % context_name)
        for index, step in enumerate(steps):
            where = "controls.proof.navigation.%s[%d]" % (context_name, index)
            if not isinstance(step, dict) or len(set(step) & set(PROOF_STEP_OPS)) != 1:
                raise ProjectError(where + " must name exactly one of " + ", ".join(PROOF_STEP_OPS))
            if "press" in step and step["press"] not in GPTK_CONTROLS:
                raise ProjectError(where + ".press must be a canonical control")
            if "hold" in step and step["hold"] not in ("LEFT_STICK", "RIGHT_STICK"):
                raise ProjectError(where + ".hold must be LEFT_STICK or RIGHT_STICK")
            if "expect" in step and step["expect"] not in PROOF_EXPECT_KINDS:
                raise ProjectError(where + ".expect must be log, no_log or quiet")
            if "wait_log" in step and not isinstance(step["wait_log"], str):
                raise ProjectError(where + ".wait_log must be a regex string")
    quit_guard = proof.get("quit_guard")
    if quit_guard is not None:
        if (not isinstance(quit_guard, dict) or set(quit_guard) != {"context", "control"} or
                quit_guard["context"] not in contexts or quit_guard["control"] not in GPTK_CONTROLS):
            raise ProjectError("controls.proof.quit_guard needs a declared context and a canonical control")
    owner_remap = proof.get("owner_remap")
    if owner_remap is not None:
        if (not isinstance(owner_remap, dict) or set(owner_remap) != {"context", "null", "move_to"} or
                owner_remap["context"] not in contexts or owner_remap["null"] not in GPTK_CONTROLS or
                owner_remap["move_to"] not in GPTK_CONTROLS or owner_remap["null"] == owner_remap["move_to"]):
            raise ProjectError("controls.proof.owner_remap needs context, null and a different move_to control")
        binding = contexts[owner_remap["context"]].get(owner_remap["null"])
        if binding in (None, GPTK_NULL, GPTK_NATIVE):
            raise ProjectError("controls.proof.owner_remap.null must name a control bound to an action")
        if contexts[owner_remap["context"]].get(owner_remap["move_to"]) not in (GPTK_NULL, GPTK_NATIVE):
            raise ProjectError("controls.proof.owner_remap.move_to must be null or native in that context")
    clones = proof.get("clones", 2)
    if type(clones) is not int or not 1 <= clones <= 3:
        raise ProjectError("controls.proof.clones must be 1..3")
    return proof


def render_input_proof_roteiros(controls, port_id):
    """Generate the ON_DEVICE_AUTOMATED_INPUT_PROOF roteiros from the port's
    declared actions/contexts/sinks plus the navigation it supplied in
    controls.proof. ONE SESSION PER CONTEXT: every session boots the game,
    navigates to its context, covers it and ends through SELECT+START on one
    instance, so the coverage of one context can never leave the game in a
    state the next context's navigation does not expect. Returns a dict of
    file name -> JSON-serialisable content (roteiros) or text (owner GPTK)."""
    proof = controls["proof"]
    contexts = controls["contexts"]
    actions_by_id = {a["id"]: a for a in controls["actions"]}
    schema = "nx-device-input-proof-roteiro/1"
    exit_neg = {"expect": "no_log", "regex": "runtime EXIT|lifecycle exit requested"}
    order = [c for c in ("menu", "gameplay", "cursor") if c in contexts]

    def press(control, ms=150):
        return {"press": control, "ms": ms}

    def sleep(ms):
        return {"sleep_ms": ms}

    effects = proof.get("effects", {})

    def delivery(context, control, action):
        return {"expect": "delivery", "context": context, "control": control,
                "action": action, "sink": actions_by_id[action]["sinks"][0]}

    def effect_of(action, context_name):
        """0.4.2: the engine's own proof after the delivery (see
        validate_controls_proof); [] when the port declared none, or none
        for the context the stimulus happens in."""
        effect = effects.get(action)
        if not effect or ("contexts" in effect and context_name not in effect["contexts"]):
            return []
        step = {"expect": "context_change", "context": effect["context"]}
        if effect.get("source_regex"):
            step["source_regex"] = effect["source_regex"]
        return [step]

    def once(control):
        return [{"expect": "count", "kind": "delivery", "control": control, "pressed": 1, "value": 1},
                {"expect": "count", "kind": "delivery", "control": control, "pressed": 0, "value": 1}]

    def stick_gestures(context_name, control, binding):
        """Positive/negative edges on both axes and one diagonal; each gesture
        is one start line and one return-to-neutral line, then silence."""
        steps = []
        for x, y in ((1.0, 0.0), (-1.0, 0.0), (0.0, 1.0), (0.0, -1.0), (0.7, 0.7)):
            steps += [{"mark": True}, {"hold": control, "x": x, "y": y, "ms": 700}, sleep(1200)]
            if binding not in (GPTK_NULL, GPTK_NATIVE):
                steps += [delivery(context_name, control, binding)] + once(control)
            else:
                steps += [{"expect": "no_delivery", "control": control}]
        steps += [{"mark": True}, sleep(2000), {"expect": "quiet"}]
        return steps

    def start_block(context_name, bindings):
        """START alone, isolated from the chord. 0.4.3: it runs right after
        the discrete buttons and BEFORE the stick gestures -- moving the
        player can start scripted dialogue/cutscenes (FP2 tutorial), whose
        skip consumes the next pause press and the declared effect can no
        longer be observed in its window."""
        start_binding = bindings.get("START", GPTK_NULL)
        steps = [{"mark": True}, press("START"), sleep(3000), exit_neg]
        if start_binding not in (GPTK_NULL, GPTK_NATIVE):
            steps.append(delivery(context_name, "START", start_binding))
            if actions_by_id[start_binding]["kind"] == "button":
                steps += once("START")
            steps += effect_of(start_binding, context_name)
            # a START that reaches the engine may have toggled a pause menu:
            # press it again so the rest of the coverage runs in the context it covers
            steps += [{"mark": True}, press("START"), sleep(3000), exit_neg]
        elif start_binding == GPTK_NULL:
            steps.append({"expect": "suppressed", "context": context_name, "control": "START"})
        return steps

    def cover_context(context_name, bindings):
        steps = [{"mark": True}, sleep(2500), {"expect": "quiet"}]
        sticks_done = False
        for control in GPTK_CONTROLS:
            binding = bindings.get(control, GPTK_NULL)
            if control in ("SELECT", "START"):
                continue  # START isolated before the sticks, SELECT only in the negatives
            if control in ("LEFT_STICK", "RIGHT_STICK"):
                if not sticks_done:
                    steps += start_block(context_name, bindings)
                    sticks_done = True
                steps += stick_gestures(context_name, control, binding)
                continue
            if binding == GPTK_NULL:
                steps += [{"mark": True}, press(control), sleep(1200),
                          {"expect": "suppressed", "context": context_name, "control": control},
                          {"expect": "no_delivery", "control": control}]
            elif binding == GPTK_NATIVE:
                steps += [{"mark": True}, press(control), sleep(1200), {"expect": "no_delivery", "control": control}, exit_neg]
            else:
                steps += [{"mark": True}, press(control), sleep(1500), delivery(context_name, control, binding)]
                if actions_by_id[binding]["kind"] == "button":
                    steps += once(control)
                steps += effect_of(binding, context_name)
        if not sticks_done:
            steps += start_block(context_name, bindings)  # no stick in the vocabulary: still before hotplug/negatives
        # hotplug: the second clone leaves and comes back; nothing may stay latched
        steps += [{"mark": True}, {"unplug": 1}, sleep(2500), {"expect": "log", "regex": "controller-removed"},
                  {"expect": "quiet"}, {"replug": 1}, sleep(3000), {"mark": True}, sleep(1500), {"expect": "quiet"}]
        # negatives that must never end the game (SELECT and START alone last)
        steps += [{"mark": True}, {"chord": ["L1", "R1"], "ms": 400}, sleep(1500), exit_neg,
                  {"mark": True}, {"chord": ["L2", "R2"], "ms": 400}, sleep(1500), exit_neg,
                  {"mark": True}, {"chord_cross": ["SELECT", "START"], "ms": 500}, sleep(2000), exit_neg,
                  {"expect": "log", "regex": "chord denied: SELECT and START on different pads"},
                  {"mark": True}, press("SELECT"), sleep(1500), exit_neg]
        return steps

    def navigation_to(context_name):
        steps = []
        for c in order:
            steps += list(proof["navigation"][c])
            if c == context_name:
                break
        return steps

    def clean_exit():
        return [{"mark": True}, {"chord": ["SELECT", "START"], "ms": 300}, {"wait_exit": True, "timeout_s": 60},
                {"expect": "log", "regex": "SELECT\\+START: lifecycle exit requested|exit-chord"},
                {"expect": "exit_status", "value": 0}, {"expect": "process_gone"}]

    files = {}
    for context_name in order:
        steps = navigation_to(context_name)
        if proof.get("quit_guard", {}).get("context") == context_name:
            steps += [{"mark": True}, press(proof["quit_guard"]["control"]), sleep(3000), exit_neg]
        steps += cover_context(context_name, contexts[context_name]) + clean_exit()
        files["input-proof-default-%s.json" % context_name] = {
            "schema": schema, "port_id": port_id, "generated_by": "nxgenerator %s" % nxgenerator_version(),
            "classification": "ON_DEVICE_AUTOMATED_INPUT_PROOF", "session": context_name,
            "purpose": "default NEXTOSCONTROLLERS.gptk, context [%s]: every declared control, stick edges/diagonal with "
                       "return to neutral, press/release once, hotplug, negatives, SELECT+START on one instance" % context_name,
            "clones": max(2, int(proof.get("clones", 2))), "steps": steps,
        }
    owner = proof.get("owner_remap")
    if owner is not None:
        ctx, null_control, move_to = owner["context"], owner["null"], owner["move_to"]
        moved_action = contexts[ctx][null_control]
        owner_steps = navigation_to(ctx) + [
            {"mark": True}, press(null_control), sleep(1500),
            {"expect": "suppressed", "context": ctx, "control": null_control},
            {"expect": "no_delivery", "control": null_control},
            {"mark": True}, press(move_to), sleep(1500), delivery(ctx, move_to, moved_action),
            {"expect": "count", "kind": "delivery", "control": move_to, "pressed": 1, "value": 1}] + effect_of(moved_action, ctx) + clean_exit()
        files["input-proof-owner-remap.json"] = {
            "schema": schema, "port_id": port_id, "generated_by": "nxgenerator %s" % nxgenerator_version(),
            "classification": "ON_DEVICE_AUTOMATED_INPUT_PROOF", "session": "owner-remap",
            "purpose": "owner copy with %s = null and %s = %s in [%s]: same ELF, edited file reaches the engine" % (
                null_control, move_to, moved_action, ctx),
            "clones": 1, "steps": owner_steps,
        }
        edited = dict((k, dict(v)) for k, v in contexts.items())
        edited[ctx][null_control] = GPTK_NULL
        edited[ctx][move_to] = moved_action
        rendered = render_template("NEXTOSCONTROLLERS.gptk.in", {
            "PORT_ID": port_id,
            "GPTK_SCHEMA": str(controls.get("schema", 1)),
            "GPTK_GUIDANCE": (GPTK_LIVE_GUIDANCE if controls.get("runtime_mapping") == "nxinput-gptk"
                              else GPTK_STATIC_GUIDANCE),
            "CONTROL_SECTIONS": render_controls_sections(dict(controls, contexts=edited)).rstrip(),
        })
        files["owner-remap-NEXTOSCONTROLLERS.gptk"] = (
            rendered.decode("utf-8") if isinstance(rendered, bytes) else rendered)
    return files


def write_input_proof_roteiros(raw_controls, port_id, output):
    """Beside the package (never inside it): <output>-proof/ with the
    generated ON_DEVICE_AUTOMATED_INPUT_PROOF roteiros and the owner copy."""
    proof_dir = Path(str(output) + "-proof")
    if proof_dir.exists() or proof_dir.is_symlink():
        raise ProjectError("proof output already exists: %s" % proof_dir)
    proof_dir.mkdir(mode=0o755)
    for name, content in sorted(render_input_proof_roteiros(raw_controls, port_id).items()):
        if isinstance(content, str):
            payload = content.encode("utf-8")
        else:
            payload = (json.dumps(content, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
        atomic_write_bytes(proof_dir / name, payload, 0o644)
    return proof_dir


def validate_controls_tuning(tuning, contexts, actions_by_id):
    if (not isinstance(tuning, dict) or not tuning or
            not set(tuning) <= {"cursor", "camera"}):
        raise ProjectError(
            "controls.tuning must be a non-empty object containing only "
            "cursor and/or camera"
        )
    normalised = {}
    for section_name, allowed_keys, numeric_ranges in (
            ("cursor", CURSOR_TUNING_KEYS, CURSOR_TUNING_RANGES),
            ("camera", CAMERA_TUNING_KEYS, CAMERA_TUNING_RANGES)):
        if section_name not in tuning:
            continue
        section = tuning[section_name]
        context = "controls.tuning.%s" % section_name
        if (not isinstance(section, dict) or not section or
                not set(section) <= set(allowed_keys)):
            raise ProjectError(
                "%s must be a non-empty object with only known tuning keys" %
                context
            )
        if section_name == "cursor":
            cursor_vector_bound = any(
                action_id.startswith("cursor.") and
                actions_by_id[action_id]["kind"] == "vector"
                for bindings in contexts.values()
                for action_id in bindings.values()
            )
            if not cursor_vector_bound:
                raise ProjectError(
                    "controls.tuning.cursor requires a bound cursor.* "
                    "vector action"
                )
        selected = {}
        for key in allowed_keys:
            if key not in section:
                continue
            value = section[key]
            if key in numeric_ranges:
                lower, upper = numeric_ranges[key]
                selected[key] = _bounded_tuning_number(
                    value, "%s.%s" % (context, key), lower, upper
                )
            elif key in ("invert_x", "invert_y"):
                if not isinstance(value, bool):
                    raise ProjectError("%s.%s must be a boolean" %
                                       (context, key))
                selected[key] = value
            elif key == "authority":
                if value not in ("nextos", "native"):
                    raise ProjectError(
                        "%s.authority must be nextos or native" % context
                    )
                selected[key] = value
        normalised[section_name] = selected
    return normalised


def render_tuning_value(value, bounds=None):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return value
    if bounds is not None:
        minimum, maximum = bounds
        float32 = struct.unpack("=f", struct.pack("=f", value))[0]
        # nxinput exposes its public bounds as C float constants and promotes
        # them to double during parsing. At the two inexact decimal edges
        # (0.05f and 0.9f), spelling the mathematical decimal literally would
        # land just outside that promoted interval. Emit the effective float32
        # edge so every value accepted here is accepted by the canonical parser.
        if ((value == minimum and float32 > value) or
                (value == maximum and float32 < value)):
            value = float32
            # Nine fixed fractional digits stay on the accepted side of the
            # promoted C-float boundary while rounding back to the same float.
            rendered = format(value, ".9f")
        else:
            rendered = (str(value) if isinstance(value, int) else
                        format(Decimal(str(value)), "f"))
    else:
        rendered = (str(value) if isinstance(value, int) else
                    format(Decimal(str(value)), "f"))
    # Decimal fixed-point removes the one syntax the GPTK parser deliberately
    # refuses (an exponent) without losing ordinary small fractional values.
    if isinstance(value, int):
        rendered = format(value, "f")
    if "." in rendered:
        rendered = rendered.rstrip("0").rstrip(".")
    return "0" if rendered in ("-0", "") else rendered


GPTK4_DISCRETE = ("A", "B", "X", "Y", "L1", "R1", "L3", "R3", "START",
                  "SELECT", "GUIDE", "UP", "DOWN", "LEFT", "RIGHT")


CONTROLS_CLOSURE_EVENTS = {"button": "press", "axis": "axis",
                           "vector": "motion"}


def build_controls_closure(controls, port_id):
    """V5 (M1c item 1.2): the EXPECTED host closure, born from the project.

    The Tearscape host harness was written inside the port and grew its own
    copy of the expectation; when the owner moved to schema 4 the harness
    stayed on the V3 loader and the whole proof went red at
    `generated default map loads` (NXI1006), with the failure surfacing three
    cascades later as "generated vector reaches the declared action sink".

    Mission rule 8.1: the expectation must never come from the map under
    test. So the generator -- which already owns `controls.contexts` and
    `controls.actions` -- writes the closure next to the owner it generated,
    in the same shape `make_input_proof.py` computes today. A port harness
    then only READS it.

    `native` and `null` bindings carry no case on purpose: they are the
    absence of an action, and the host gate proves them as NONE/SUPPRESS.
    """
    actions_by_id = {a["id"]: a for a in controls.get("actions", [])}
    contexts = controls["contexts"]
    cases = []
    for context in sorted(contexts):
        for control, action_id in sorted(contexts[context].items()):
            if action_id in (None, "native", "null"):
                continue
            action = actions_by_id.get(action_id)
            if action is None:
                raise ProjectError(
                    "controls.contexts.%s.%s binds undeclared action %r"
                    % (context, control, action_id))
            event = CONTROLS_CLOSURE_EVENTS.get(action["kind"])
            if event is None:
                raise ProjectError(
                    "controls.actions %r has no closure event for kind %r"
                    % (action_id, action["kind"]))
            for sink in action["sinks"]:
                cases.append({
                    "context": context,
                    "control": control,
                    "event": event,
                    "decision": "ACTION",
                    "action": action_id,
                    "sink": sink,
                    "delivery_count": 1,
                })
    cases.sort(key=lambda item: (item["context"], item["control"],
                                 item["action"], item["sink"], item["event"]))
    return {
        "schema": "nx-controls-closure/1",
        "schema_version": 1,
        "port_id": port_id,
        "generated_by": "nxgenerator %s" % read_version(
            ROOT / "VERSION", "nxgenerator"),
        "gptk_schema": controls.get("schema", 1),
        # Only the contexts the PORT declares are provable; a schema-4 [base]
        # also reaches contexts the port never declared (the bridge is unified
        # by contract) and a binding landing there is NOT an extra binding.
        "declared_contexts": sorted(contexts),
        "actions": [{"id": a["id"], "kind": a["kind"], "sinks": list(a["sinks"])}
                    for a in sorted(controls.get("actions", []),
                                    key=lambda a: a["id"])],
        "cases": cases,
    }


def _gptk4_value(binding):
    if binding in (None, "null"):
        return "null"
    if binding == "native":
        return "native"
    return "action:%s" % binding


def render_controls_sections_v4(controls):
    """NEXTOS_CONTROLLERS/4: unified [base] from the gameplay context, sparse
    [override.<ctx>] with only the controls that really diverge, sticks and
    triggers in their normative sections. Every core control is explicit
    (omission is a parser error, never a hidden null). Tuning keys of the
    V3 [cursor] section do not exist in the owner file: they belong to the
    adapter contract."""
    contexts = controls["contexts"]
    kinds = {a["id"]: a["kind"] for a in controls.get("actions", [])}
    base = contexts["gameplay"]
    # 0.4.4 (Nameless Cat 1.2.8, 04/09): the trigger channel is decided by
    # EVERY context, not by the base alone. The schema-4 bridge resolves the
    # slot the [trigger.*] mode declares, so a trigger that is null in the
    # base (gameplay) and a button action in an override (menu: R2 =
    # cursor.click) must declare `mode = digital` in the base section and
    # write the override on the digital channel -- `trigger.right.analog =
    # action:<button>` is a kind mismatch the parser rejects (NXINPUT_GPTK4_ERR_KIND).
    # Contexts that bind the same trigger to a button AND to an analog action
    # are contradictory and fail here, never silently.
    trigger_channel = {}
    for trigger in ("L2", "R2"):
        seen = set()
        for context_name, bindings in contexts.items():
            kind = kinds.get((bindings or {}).get(trigger))
            if kind:
                seen.add("digital" if kind == "button" else "analog")
        if len(seen) > 1:
            raise ProjectError(
                "controls.contexts: trigger %s is bound to a button action in one "
                "context and to an analog action in another; one trigger has one channel"
                % trigger)
        trigger_channel[trigger] = seen.pop() if seen else "analog"
    chunks = ["CONTROL_STANDARD = xbox", "GLYPH_STYLE = xbox",
              "AUTHORITY = nextos", "CONTEXT_POLICY = unified", "",
              "# Positional Xbox surface: A = south, B = east, X = west, Y = north.",
              "# One base map for title/menu/gameplay; [override.*] only where the game proved a screen diverges.",
              "[base]"]
    for control in GPTK4_DISCRETE:
        chunks.append("%s = %s" % (control, _gptk4_value(base.get(control, "native" if control == "GUIDE" else None))))
    chunks.append("")
    for context_name in GPTK_CONTEXTS:
        if context_name == "gameplay":
            continue
        bindings = contexts.get(context_name)
        if bindings is None:
            continue
        diff = []
        for control in GPTK4_DISCRETE:
            if control == "GUIDE":
                continue
            if _gptk4_value(bindings.get(control)) != _gptk4_value(base.get(control)):
                diff.append("%s = %s" % (control, _gptk4_value(bindings.get(control))))
        for stick, key in (("LEFT_STICK", "stick.left.vector"), ("RIGHT_STICK", "stick.right.vector")):
            if _gptk4_value(bindings.get(stick)) != _gptk4_value(base.get(stick)):
                diff.append("%s = %s" % (key, _gptk4_value(bindings.get(stick))))
        for trigger, key in (("L2", "trigger.left"), ("R2", "trigger.right")):
            if _gptk4_value(bindings.get(trigger)) != _gptk4_value(base.get(trigger)):
                # The trigger channel (analog vs digital) is fixed by the BASE
                # mode, because the schema-4 bridge resolves whichever slot the
                # base trigger declares. A sparse override only changes the
                # binding on that same channel, so the channel must be picked
                # from the base kind, never from the (often null) override
                # binding -- otherwise a suppressed digital trigger writes
                # `trigger.left.analog = null`, the bridge keeps resolving the
                # digital slot and the base action leaks into the context.
                diff.append("%s.%s = %s" % (key, trigger_channel[trigger], _gptk4_value(bindings.get(trigger))))
        if diff:
            chunks.append("[override.%s]" % context_name)
            chunks.extend(diff)
            chunks.append("")
    for stick, name in (("LEFT_STICK", "left"), ("RIGHT_STICK", "right")):
        chunks.extend(["[stick.%s]" % name, "mode = vector",
                       "vector = %s" % _gptk4_value(base.get(stick)),
                       "up = null", "down = null", "left = null", "right = null",
                       "enter_threshold = 0.55", "exit_threshold = 0.40",
                       "diagonal = 8way", "tie_break = horizontal", ""])
    for trigger, name in (("L2", "left"), ("R2", "right")):
        binding = base.get(trigger)
        if trigger_channel[trigger] == "digital":
            chunks.extend(["[trigger.%s]" % name, "mode = digital", "analog = null",
                           "digital = %s" % _gptk4_value(binding)])
        else:
            chunks.extend(["[trigger.%s]" % name, "mode = analog",
                           "analog = %s" % _gptk4_value(binding), "digital = null"])
        chunks.extend(["enter_threshold = 0.55", "exit_threshold = 0.40", ""])
    return "\n".join(chunks).rstrip() + "\n"


def render_controls_sections(controls):
    """Render deterministic GPTK sections from a validated contract.

    In schema 2 every section shows ALL 18 controls in the stable order:
    a control the port uses carries its action (or the explicit `native`),
    and a control the port does not use carries `null`. The owner therefore
    sees the whole pad and can remap anything without guessing what exists.
    In schema 1 only the declared controls are written, byte for byte as
    before C4.
    """
    chunks = []
    tuning = controls.get("tuning", {})
    schema = controls.get("schema", 1)
    if schema == 4:
        return render_controls_sections_v4(controls)
    if schema == 3:
        # The one mandatory V3 preamble line, rendered exactly once, before
        # any section. V1/V2 outputs keep their bytes untouched.
        chunks.append("FACE_LAYOUT = %s" % controls.get("face_layout",
                                                        "auto"))
        chunks.append("")
    for context_name in GPTK_CONTEXTS:
        bindings = controls["contexts"].get(context_name)
        if bindings is None:
            continue
        chunks.append("[%s]" % context_name)
        for control in GPTK_CONTROLS:
            if control in bindings:
                chunks.append("%s = %s" % (control, bindings[control]))
            elif schema in (2, 3):
                chunks.append("%s = %s" % (control, GPTK_NULL))
        if context_name == "cursor":
            for key in CURSOR_TUNING_KEYS:
                if key in tuning.get("cursor", {}):
                    chunks.append("%s = %s" % (
                        key, render_tuning_value(
                            tuning["cursor"][key], CURSOR_TUNING_RANGES[key]
                        )
                    ))
        chunks.append("")
    if "cursor" in tuning and "cursor" not in controls["contexts"]:
        chunks.append("[cursor]")
        for key in CURSOR_TUNING_KEYS:
            if key in tuning["cursor"]:
                chunks.append("%s = %s" % (
                    key, render_tuning_value(
                        tuning["cursor"][key], CURSOR_TUNING_RANGES[key]
                    )
                ))
        chunks.append("")
    if "camera" in tuning:
        chunks.append("[camera]")
        for key in CAMERA_TUNING_KEYS:
            if key in tuning["camera"]:
                bounds = CAMERA_TUNING_RANGES.get(key)
                chunks.append("%s = %s" % (
                    key, render_tuning_value(tuning["camera"][key], bounds)
                ))
        chunks.append("")
    return "\n".join(chunks).rstrip() + "\n"


def validate_project(document, source_root=REPOSITORY):
    source_root = safe_source_root(source_root)
    if not isinstance(document, dict):
        raise ProjectError("project manifest must be an object")
    required_keys = PROJECT_KEYS - {
        "runtime_root", "owner_data", "language_access", "graphics",
        "controls", "display", "promotion", "package_payload",
        "video", "owner_runtime"
    }
    present = set(document)
    if not required_keys <= present or not present <= PROJECT_KEYS:
        raise ProjectError(
            "project manifest must contain exactly %s (owner_data, "
            "language_access, graphics, controls, display, promotion and "
            "package_payload are "
            "schema-dependent)"
            % ", ".join(sorted(required_keys))
        )
    project_schema = document["schema_version"]
    if isinstance(project_schema, bool) or project_schema not in (1, 2, 3):
        raise ProjectError("project schema_version must be 1, 2 or 3")

    try:
        nxport = BOOTSTRAP_GENERATOR.validate(document["nxport"])
    except BOOTSTRAP_GENERATOR.ManifestError as error:
        raise ProjectError("invalid nxport: %s" % error)
    if nxport["architecture"] not in ("armv7", "aarch64"):
        raise ProjectError("public project scaffold supports ARMv7 or AArch64")
    if nxport.get("execution_roles") is not None and project_schema < 2:
        raise ProjectError(
            "execution_roles requires nxproject schema_version 2 or newer"
        )
    generation_runtime = nxport.get("generation_runtime")
    if generation_runtime is not None:
        if project_schema < 3:
            raise ProjectError(
                "nxport schema_version 3 generation_runtime requires "
                "nxproject schema_version 3"
            )
        if "runtime_root" not in document:
            raise ProjectError(
                "nxport schema_version 3 generation_runtime requires "
                "runtime_root"
            )
        runtime_root = safe_repository_directory(
            document["runtime_root"], "runtime_root", source_root
        )
    else:
        if "runtime_root" in document:
            raise ProjectError(
                "runtime_root is only valid with nxport schema_version 3 "
                "generation_runtime"
            )
        runtime_root = None
    reject_public_literal(nxport["title"], "nxport.title")
    if any(character in nxport["title"] for character in "[]<>`@"):
        raise ProjectError("nxport.title contains unsafe documentation markup")

    adapter = require_object(
        document["adapter"], "adapter", {"skeleton"}
    )
    if adapter["skeleton"] != "contract-only":
        raise ProjectError("adapter.skeleton must be contract-only")

    portmaster_keys = {"metadata_version", "min_glibc"}
    if project_schema >= 2:
        portmaster_keys.add("runtime")
    portmaster = require_object(
        document["portmaster"], "portmaster", portmaster_keys
    )
    if (isinstance(portmaster["metadata_version"], bool) or
            not isinstance(portmaster["metadata_version"], int) or
            portmaster["metadata_version"] != 4):
        raise ProjectError("portmaster.metadata_version must be 4")
    min_glibc = require_string(
        portmaster["min_glibc"], "portmaster.min_glibc", VERSION_RE
    )
    if version_tuple(min_glibc) > (2, 30):
        raise ProjectError("portmaster.min_glibc exceeds the public ceiling")
    runtime = []
    if project_schema >= 2:
        runtime = require_unique_strings(
            portmaster["runtime"], "portmaster.runtime"
        )

    license_config = require_object(
        document["license"], "license", {"spdx_id", "source"}
    )
    spdx_id = require_string(
        license_config["spdx_id"], "license.spdx_id", SPDX_RE
    )
    license_source = safe_repository_file(
        license_config["source"], "license.source", source_root
    )

    documentation = require_object(
        document["documentation"], "documentation",
        {"status", "proven_support"},
    )
    if documentation["status"] not in ("scaffold", "authored"):
        raise ProjectError("documentation.status must be scaffold or authored")
    if documentation["proven_support"] != []:
        raise ProjectError(
            "the generator cannot invent physical support declarations"
        )
    if "package_payload" in document:
        package_payload = validate_package_payload(
            document["package_payload"], project_schema, source_root, nxport,
            documentation,
        )
    else:
        package_payload = []
        if documentation["status"] == "authored":
            raise ProjectError(
                "authored documentation requires package_payload paths: "
                "INSTALLATION.md, README.md"
            )

    recipe_value = document["nxextract_recipe"]
    if nxport["nxextract"]["mode"] == "no":
        if recipe_value is not None:
            raise ProjectError(
                "nxextract_recipe must be null when NXExtract is disabled"
            )
        recipe_source = None
    else:
        recipe_source = safe_repository_file(
            recipe_value, "nxextract_recipe", source_root
        )
        # V4-04B: the WHOLE structural validation (schema, id, version,
        # input, extract, commit, validate-as-list default and every other
        # grammar rule) is the NXExtract 1.3.0 authority's decision, taken by
        # the exact framework engine the device will run.  The generator adds
        # only its own additive policies below (APK-variant flexibility and
        # owner-data search order); it never widens nor narrows the grammar.
        recipe = validate_recipe_with_nxextract_authority(
            recipe_source, recipe_value
        )
        validate_apk_variant_policy(recipe)
        search_dirs = (recipe.get("input") or {}).get("search_dirs")
        if search_dirs is not None and (
            not isinstance(search_dirs, list)
            or not search_dirs
            or search_dirs[0] != OWNER_DATA_DIRECTORY
        ):
            raise ProjectError(
                "recipe input.search_dirs must place the canonical owner-data "
                "directory %r first; the installed instructions and the "
                "extractor must look in the same place" % OWNER_DATA_DIRECTORY
            )

    owner_data = document.get("owner_data")
    if owner_data is not None:
        if project_schema < 3:
            raise ProjectError("owner_data requires nxproject schema_version 3")
        owner_data = require_object(
            owner_data, "owner_data", {"directory", "formats", "reference"}
        )
        if owner_data["directory"] != OWNER_DATA_DIRECTORY:
            raise ProjectError(
                "owner_data.directory must be the canonical %r"
                % OWNER_DATA_DIRECTORY
            )
        require_unique_strings(owner_data["formats"], "owner_data.formats")
        if not owner_data["formats"]:
            raise ProjectError("owner_data.formats must not be empty")
        for item in owner_data["formats"]:
            if not re.fullmatch(r"[A-Z0-9]{2,8}", item):
                raise ProjectError(
                    "owner_data.formats entries must be short upper-case "
                    "format names (never original filenames): %r" % item
                )
        reference = require_object(
            owner_data["reference"], "owner_data.reference", {"version"}
        )
        require_string(
            reference["version"], "owner_data.reference.version",
            re.compile(r"[A-Za-z0-9][A-Za-z0-9. _-]{0,63}"),
        )
    if owner_data is None and recipe_source is not None:
        formats = ["APK", "APKM", "APKS", "XAPK"]
        version_value = str(recipe.get("version", "")).strip()
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9. _-]{0,63}", version_value or "x"):
            version_value = ""
        owner_data = {
            "directory": OWNER_DATA_DIRECTORY,
            "formats": formats,
            "reference": {"version": version_value},
        }

    # V3-SETTINGS-01 (blocker 5): every new port (schema_version 3) must
    # DECLARE how the player reaches languages. Silence is an error.
    language_access = document.get("language_access")
    if language_access is not None:
        if project_schema < 3:
            raise ProjectError(
                "language_access requires nxproject schema_version 3"
            )
        language_access = require_object(
            language_access, "language_access",
            {"mode", "supported", "fallback", "sinks"},
        )
        if language_access["mode"] not in LANGUAGE_ACCESS_MODES:
            raise ProjectError(
                "language_access.mode must be one of %s"
                % ", ".join(LANGUAGE_ACCESS_MODES)
            )
        supported = require_unique_strings(
            language_access["supported"], "language_access.supported"
        )
        for tag in supported:
            if not re.fullmatch(r"[A-Za-z]{2,3}([-_][A-Za-z0-9]{2,8})*", tag):
                raise ProjectError(
                    "language_access.supported has an invalid tag: %r" % tag
                )
        sinks = require_unique_strings(
            language_access["sinks"], "language_access.sinks"
        )
        mode = language_access["mode"]
        fallback = language_access["fallback"]
        if mode == "none":
            if supported or sinks or fallback:
                raise ProjectError(
                    "language_access mode 'none' declares no supported/"
                    "fallback/sinks"
                )
        else:
            if not supported:
                raise ProjectError(
                    "language_access mode %r must list supported languages"
                    % mode
                )
            if fallback not in supported:
                raise ProjectError(
                    "language_access.fallback must be one of supported"
                )
            if mode in ("adapter", "first-run-native") and not sinks:
                raise ProjectError(
                    "language_access mode %r must declare at least one sink"
                    % mode
                )
    elif project_schema >= 3:
        raise ProjectError(
            "nxproject schema_version 3 must declare language_access "
            "(native-menu|first-run-native|adapter|single-language|none)"
        )

    graphics = validate_graphics(document.get("graphics"), project_schema)
    display = validate_display(document.get("display"), project_schema)
    video = validate_video(document.get("video"), project_schema)
    owner_runtime = validate_owner_runtime(
        document.get("owner_runtime"), nxport, project_schema, source_root)
    sdl3_portmaster = validate_sdl3_portmaster(
        (document.get("controls") or {}).get("sdl3_portmaster")
        if isinstance(document.get("controls"), dict) else None,
        project_schema)
    controller_profiles = validate_controller_profiles(
        (document.get("controls") or {}).get("controller_profiles")
        if isinstance(document.get("controls"), dict) else None,
        project_schema)
    egl_binding = validate_egl_binding(
        (document.get("graphics") or {}).get("egl_binding")
        if isinstance(document.get("graphics"), dict) else None,
        project_schema, graphics)
    controls = validate_controls(document.get("controls"), project_schema)
    # A field regression proved that a live nxinput runtime without its
    # packaged third authority can reject otherwise usable CFW data. Make the
    # rescue bundle an input invariant instead of a release-time convention.
    validate_runtime_mapping_authority_bundle(controls, controller_profiles)
    # nxinput 0.10.0: the FACE_LAYOUT variant pair is bound to GPTK V3.
    gptk_schema_declared = (controls or {}).get("schema", 1) if controls else 1
    profiles_variants = (controller_profiles or {}).get(
        "face_layout_variants") if controller_profiles else None
    if profiles_variants is not None and gptk_schema_declared != 3:
        raise ProjectError(
            "face_layout_variants requires controls.schema 3")
    if (gptk_schema_declared == 3 and controller_profiles is not None and
            controller_profiles.get("enabled") and profiles_variants is None):
        raise ProjectError(
            "controls.schema 3 with controller_profiles enabled requires "
            "the complete face_layout_variants pair")

    # V3-PROMOTION-01 (opt-in, exercitado SO por um port que ja provou o
    # adapter fisicamente): quando `promotion` esta presente, o gerador copia o
    # adapter-contract REAL fornecido pelo port (nunca inventa) e emite os
    # claims promovidos, de forma reproduzivel. Ausente => scaffold, default
    # 100%% inalterado.
    promotion = document.get("promotion")
    promotion_contract = None
    if promotion is not None:
        if project_schema < 3:
            raise ProjectError("promotion requires nxproject schema_version 3")
        promotion = require_object(
            promotion, "promotion", {"adapter_contract", "claims"}
        )
        promotion_contract = safe_repository_file(
            promotion["adapter_contract"], "promotion.adapter_contract",
            source_root,
        )
        try:
            promoted = json.loads(
                promotion_contract.read_text(encoding="utf-8")
            )
        except (OSError, UnicodeDecodeError, ValueError) as error:
            raise ProjectError(
                "promotion.adapter_contract is not valid JSON: %s" % error
            )
        if (not isinstance(promoted, dict) or
                promoted.get("status") != "implemented_release" or
                promoted.get("release_ready") is not True):
            raise ProjectError(
                "a promoted adapter-contract must be implemented_release "
                "and set release_ready true"
            )
        reject_public_literal(
            canonical_json(promoted).decode("utf-8"),
            "promotion.adapter_contract",
        )
        if promoted.get("language_access") != language_access:
            raise ProjectError(
                "promoted adapter-contract language_access differs from "
                "nxproject"
            )
        promoted_input = promoted.get("input")
        if (not isinstance(promoted_input, dict) or
                promoted_input.get("actions") != controls["actions"] or
                promoted_input.get("contexts") != controls["contexts"]):
            raise ProjectError(
                "promoted adapter-contract actions/contexts differ from "
                "nxproject controls"
            )
        validate_promoted_gptk_live_contract(promoted_input, controls)
        validate_promoted_controller_profiles(promoted, controller_profiles)
        expected_graphics = None if graphics is None else dict(graphics)
        if expected_graphics is not None:
            expected_graphics.pop("adapter", None)
        promoted_graphics = promoted.get("graphics")
        if isinstance(promoted_graphics, dict):
            promoted_graphics = dict(promoted_graphics)
            promoted_graphics.pop("adapter", None)
        if promoted_graphics != expected_graphics:
            raise ProjectError(
                "promoted adapter-contract graphics differs from nxproject"
            )
        claims = require_object(
            promotion["claims"], "promotion.claims",
            {"release_ready", "physical_support_proven",
             "adapter_lifecycle_implemented"},
        )
        for key in ("release_ready", "physical_support_proven",
                    "adapter_lifecycle_implemented"):
            if not isinstance(claims[key], bool):
                raise ProjectError("promotion.claims.%s must be boolean" % key)
        if (claims["release_ready"] is not True or
                claims["adapter_lifecycle_implemented"] is not True):
            raise ProjectError(
                "promotion requires release_ready and "
                "adapter_lifecycle_implemented true"
            )
        lifecycle = promoted.get("lifecycle")
        if (not isinstance(lifecycle, dict) or
                not isinstance(lifecycle.get("sequence"), list) or
                not lifecycle["sequence"]):
            raise ProjectError(
                "a promoted adapter-contract must declare a lifecycle sequence"
            )
        promotion = {"contract": promoted, "claims": claims,
                     "source": promotion["adapter_contract"]}

    return {
        "documentation": documentation,
        "package_payload": package_payload,
        "owner_data": owner_data,
        "language_access": language_access,
        "graphics": graphics,
        "display": display,
        "video": video,
        "owner_runtime": owner_runtime,
        "egl_binding": egl_binding,
        "sdl3_portmaster": sdl3_portmaster,
        "controller_profiles": controller_profiles,
        "controls": controls,
        "promotion": promotion,
        "nxport": nxport,
        "runtime_root": runtime_root,
        "project": document,
        "recipe_source": recipe_source,
        "license_source": license_source,
        "license_spdx": spdx_id,
        "min_glibc": min_glibc,
        "metadata_version": portmaster["metadata_version"],
        "portmaster_runtime": runtime,
    }


def _strong_internal_apk_anchor(rule):
    """Return whether a required archive member proves compatible content."""
    source = rule.get("source")
    validation = rule.get("validate", {})
    if (not isinstance(source, dict) or not isinstance(validation, dict) or
            rule.get("required", True) is False or
            source.get("kind") not in ("entry", "entries")):
        return False
    patterns = source.get("patterns")
    if (not isinstance(patterns, list) or not patterns or
            not all(isinstance(item, str) and item for item in patterns)):
        return False

    sha256 = validation.get("sha256")
    hashes = [sha256] if isinstance(sha256, str) else sha256
    if (isinstance(hashes, list) and hashes and
            all(isinstance(item, str) and
                re.fullmatch(r"[0-9a-fA-F]{64}", item)
                for item in hashes)):
        return True

    if source.get("kind") != "entries":
        return False
    required_paths = validation.get("required_paths")
    if (not isinstance(required_paths, list) or not required_paths or
            not all(isinstance(item, str) and item for item in required_paths)):
        return False
    numeric = ("min_files", "max_files", "min_bytes", "max_bytes")
    if not all(isinstance(validation.get(field), int) and
               not isinstance(validation.get(field), bool)
               for field in numeric):
        return False
    return (
        0 < validation["min_files"] <= validation["max_files"] and
        0 < validation["min_bytes"] <= validation["max_bytes"]
    )


def _container_identity_count(value, pattern, context):
    if value is None:
        return 0
    identities = [value] if isinstance(value, str) else value
    if (not isinstance(identities, list) or not identities or
            not all(isinstance(item, str) and re.fullmatch(pattern, item)
                    for item in identities)):
        raise ProjectError("%s contains an invalid identity list" % context)
    return len(set(item.lower() for item in identities))


APKCOMPAT = None


def validate_apk_variant_policy(recipe):
    global APKCOMPAT
    if APKCOMPAT is None:
        APKCOMPAT = load_apkcompat()
    # V3 (APK-COMPAT-01): canonical shared rule -- container identity never
    # decides compatibility, in any quantity.
    canonical_findings = []
    APKCOMPAT.validate_recipe_apk_compat(recipe, canonical_findings.append)
    if canonical_findings:
        raise ProjectError("; ".join(canonical_findings[:3]))
    """Reject external APK locks while preserving strong content recipes."""
    rules = recipe.get("extract")
    if not isinstance(rules, list):
        raise ProjectError("NXExtract recipe extract must be an array")
    containers = []
    content_anchors = []
    for index, rule in enumerate(rules):
        if not isinstance(rule, dict):
            raise ProjectError(
                "NXExtract recipe extract[%d] must be an object" % index
            )
        source = rule.get("source")
        validation = rule.get("validate", {})
        if not isinstance(source, dict) or not isinstance(validation, dict):
            continue
        kind = source.get("kind")
        if kind == "container":
            containers.append((index, validation))
        elif _strong_internal_apk_anchor(rule):
            content_anchors.append(index)
    if not containers:
        return
    input_config = recipe.get("input")
    packages = input_config.get("packages") if isinstance(input_config, dict) else None
    if (not isinstance(packages, list) or not packages or
            not all(isinstance(item, str) and item for item in packages)):
        raise ProjectError(
            "container recipes must declare input.packages for APK identity"
        )
    for index, validation in containers:
        context = "NXExtract recipe container extract[%d]" % index
        if "size" in validation:
            raise ProjectError(
                "%s cannot pin one exact APK size; use bounded min_size/max_size" %
                context
            )
        identity_count = 0
        for field, pattern in (
                ("sha256", r"[0-9a-fA-F]{64}"),
                ("crc32", r"[0-9a-fA-F]{8}")):
            count = _container_identity_count(
                validation.get(field), pattern, "%s %s" % (context, field)
            )
            if count == 1:
                raise ProjectError(
                    "%s %s must list at least two explicit APK variants or be "
                    "omitted in favor of content anchors" % (context, field)
                )
            identity_count += count
        # Flexible BYO identity (mandatory house rule): every game recipe must
        # accept the owner's own bundle, and each store/version bundle differs,
        # so a fixed per-APK SHA can never be listed. A declared package (checked
        # against the container manifest at extract time) plus a magic header and
        # a bounded size is a valid flexible container.
        has_magic = any(k in validation for k in ("magic_hex", "magic_ascii"))
        has_bounded_size = "min_size" in validation and "max_size" in validation
        flexible_ok = has_magic and has_bounded_size
        if not identity_count and not content_anchors and not flexible_ok:
            raise ProjectError(
                "%s needs a declared package with magic and bounded size, a "
                "strong internal SHA-256, or a structural content anchor" % context
            )


def atomic_write_bytes(path, payload, mode):
    descriptor, temporary = tempfile.mkstemp(
        prefix=".%s." % path.name, dir=str(path.parent)
    )
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def materialize_package_payload(port_dir, records, documentation_status):
    """Compose retained author bytes without reading their sources again."""
    for record in records:
        relative = record["logical"]
        target = port_dir.joinpath(*relative.parts)
        replace_documentation = record["path"] in PACKAGE_PAYLOAD_DOCUMENTATION
        if replace_documentation:
            if documentation_status != "authored":
                raise ProjectError(
                    "only authored documentation may replace generated docs"
                )
            if target.is_symlink() or not target.is_file():
                raise ProjectError(
                    "authored documentation target is not a generated file: %s" %
                    record["path"]
                )
        else:
            if target.exists() or target.is_symlink():
                raise ProjectError(
                    "package_payload target already exists: %s" % record["path"]
                )
            parent = port_dir
            for part in relative.parts[:-1]:
                parent = parent / part
                if parent.exists() or parent.is_symlink():
                    if parent.is_symlink() or not parent.is_dir():
                        raise ProjectError(
                            "package_payload parent collides with generation: %s" %
                            record["path"]
                        )
                else:
                    parent.mkdir(mode=0o755)
                    os.chmod(parent, 0o755)
        atomic_write_bytes(target, record["payload"], record["mode"])


def require_canonical_runtime_file(path, payload, mode, context):
    """Accept a bootstrap-materialized member only when it is canonical.

    Schema-3 generation v2 deliberately materializes the complete runtime
    closure before nxgenerator adds the remaining project metadata. NXExtract
    core files therefore already exist at this point. Rewriting them would
    hide a disagreement between the declared closure and the canonical engine;
    refusing anything except the same regular file, bytes and mode keeps the
    bootstrap and project generators on one immutable identity.
    """
    if path.is_symlink() or not path.is_file():
        raise ProjectError(
            "%s was not materialized as a regular runtime file" % context
        )
    actual_mode = stat.S_IMODE(path.stat().st_mode)
    if actual_mode != mode or path.read_bytes() != payload:
        raise ProjectError(
            "%s differs from the canonical NXExtract runtime" % context
        )


def materialize_nxextract_file(path, payload, mode, generation_v2, context):
    if generation_v2:
        require_canonical_runtime_file(path, payload, mode, context)
    else:
        atomic_write_bytes(path, payload, mode)


def render_template(name, replacements):
    text = (ROOT / "templates" / name).read_text(encoding="utf-8")
    for key, value in replacements.items():
        text = text.replace("@%s@" % key, value)
    leftovers = sorted(set(re.findall(r"@[A-Z0-9_]+@", text)))
    if leftovers:
        raise ProjectError(
            "unresolved documentation token(s): %s" % ", ".join(leftovers)
        )
    return text.encode("utf-8")


def _canonical_gameinfo_bytes(nxport):
    launcher = xml_escape(nxport["launcher_name"])
    title = xml_escape(nxport["title"])
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<gameList>\n'
        '  <game>\n'
        '    <path>./%s</path>\n'
        '    <name>%s</name>\n'
        '  </game>\n'
        '</gameList>\n' % (launcher, title)
    ).encode("utf-8")


def render_gameinfo(nxport):
    """Render the minimal canonical PortMaster gameinfo.xml document."""
    return _canonical_gameinfo_bytes(nxport)


def validate_gameinfo(payload, nxport):
    """Fail closed if generated PortMaster metadata drifts from nxport."""
    if not isinstance(payload, bytes) or not payload or len(payload) > 16384:
        raise ProjectError("generated gameinfo.xml has invalid bounded bytes")
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ProjectError("generated gameinfo.xml is not UTF-8: %s" % error)
    if "<!DOCTYPE" in text.upper() or "<!ENTITY" in text.upper():
        raise ProjectError("generated gameinfo.xml contains a DTD or entity")
    try:
        root = ElementTree.fromstring(payload)
    except ElementTree.ParseError as error:
        raise ProjectError("generated gameinfo.xml is malformed: %s" % error)
    if root.tag != "gameList" or root.attrib or len(root) != 1:
        raise ProjectError(
            "generated gameinfo.xml must contain exactly one gameList/game"
        )
    game = root[0]
    if (game.tag != "game" or game.attrib or
            [child.tag for child in game] != ["path", "name"] or
            any(child.attrib or len(child) for child in game)):
        raise ProjectError("generated gameinfo.xml shape is not canonical")
    if game.findtext("path") != "./" + nxport["launcher_name"]:
        raise ProjectError(
            "generated gameinfo.xml path differs from launcher_name"
        )
    if game.findtext("name") != nxport["title"]:
        raise ProjectError("generated gameinfo.xml name differs from title")
    if payload != _canonical_gameinfo_bytes(nxport):
        raise ProjectError("generated gameinfo.xml bytes are not canonical")


def artifact_inventory(stage):
    records = []
    candidates = (
        (path.relative_to(stage).as_posix(), path)
        for path in stage.rglob("*")
    )
    for relative, path in sorted(candidates, key=lambda item: item[0]):
        if path.is_symlink():
            raise ProjectError("generated tree contains a symlink")
        if not path.is_file():
            continue
        if relative.endswith("/GENERATION.json"):
            continue
        mode = stat.S_IMODE(path.stat().st_mode)
        records.append({
            "path": relative,
            "mode": "%04o" % mode,
            "sha256": sha256_file(path),
        })
    return records


def validate_bootstrap_deployment(stage, nxport, source_state, splash_state):
    # Legacy schemas keep the control-only generation-v1 contract. nxport v3
    # opts into generation-v2: every declared runtime member is present both
    # at the live root and below the immutable generation, with identical
    # path/role/mode/hash identity.
    port_dir = stage / nxport["id"]
    launcher_path = require_regular_file(
        stage / nxport["launcher_name"], "generated launcher"
    )
    manifest_path = require_regular_file(
        port_dir / "nxport.json", "generated nxport manifest"
    )
    splash_path = require_regular_file(
        port_dir / "nxsplash-nextos", "generated nxsplash helper"
    )
    expected_modes = {
        launcher_path: 0o755,
        manifest_path: 0o644,
        splash_path: 0o755,
    }
    for path, expected_mode in expected_modes.items():
        if stat.S_IMODE(path.stat().st_mode) != expected_mode:
            raise ProjectError(
                "generated deployment has an unsafe mode: %s" % path.name
            )

    if sha256_file(splash_path) != splash_state["artifact"]["sha256"]:
        raise ProjectError(
            "generated nxsplash differs from the pinned architecture artifact"
        )
    if (nxport["required_files"].count("nxsplash-nextos") != 1 or
            nxport["required_files"][1] != "nxsplash-nextos"):
        raise ProjectError(
            "generated nxport lacks the canonical mandatory nxsplash payload"
        )

    generation_runtime = nxport.get("generation_runtime")
    try:
        generation_id = BOOTSTRAP_GENERATOR.generation_identity(
            nxport, splash_state["artifact"]["sha256"]
        )
    except BOOTSTRAP_GENERATOR.ManifestError as error:
        raise ProjectError("invalid generated runtime identity: %s" % error)
    generation_root = (
        port_dir / ".nxruntime" / "generations" / generation_id
    )
    generation_manifest_path = require_regular_file(
        generation_root / "manifest.json", "generated runtime manifest"
    )
    commit_path = require_regular_file(
        generation_root / "commit", "generated runtime commit"
    )
    if commit_path.read_text(encoding="ascii") != generation_id + "\n":
        raise ProjectError("generated runtime commit differs from its identity")
    try:
        generation_manifest = json.loads(
            generation_manifest_path.read_text(encoding="utf-8")
        )
    except (UnicodeDecodeError, ValueError) as error:
        raise ProjectError("generated runtime manifest is invalid: %s" % error)

    if generation_runtime is None:
        if (generation_manifest.get("schema") != "nxruntime-generation-v1" or
                generation_manifest.get("schema_version") != 1 or
                generation_manifest.get("generation_id") != generation_id):
            raise ProjectError("legacy nxport lost generation-v1 semantics")
        for v2_only in (
            "format", "components.v2", "identity.json", "identity-runtime.v2"
        ):
            if (generation_root / v2_only).exists() or \
                    (generation_root / v2_only).is_symlink():
                raise ProjectError(
                    "legacy nxport unexpectedly emitted generation-v2 metadata"
                )
    else:
        if nxport.get("schema_version") != 3:
            raise ProjectError(
                "generation_runtime was emitted outside nxport schema_version 3"
            )
        format_path = require_regular_file(
            generation_root / "format", "generated runtime format"
        )
        components_v2_path = require_regular_file(
            generation_root / "components.v2", "generated runtime components"
        )
        identity_path = require_regular_file(
            generation_root / "identity.json", "generated runtime identity"
        )
        identity_runtime_path = require_regular_file(
            generation_root / "identity-runtime.v2",
            "generated runtime identity records",
        )
        if format_path.read_bytes() != b"nxruntime-generation-v2\n":
            raise ProjectError("generated runtime format is not generation-v2")
        if (generation_manifest.get("schema") != "nxruntime-generation-v2" or
                generation_manifest.get("schema_version") != 2 or
                generation_manifest.get("generation_id") != generation_id):
            raise ProjectError("nxport schema_version 3 lost generation-v2 semantics")
        expected_runtime_records = \
            BOOTSTRAP_GENERATOR.generation_runtime_records(nxport)
        if identity_runtime_path.read_text(encoding="utf-8") != \
                expected_runtime_records:
            raise ProjectError("generated runtime identity records differ from nxport")
        expected_components = [
            {
                "role": "launcher",
                "path": nxport["launcher_name"],
                "mode": "0755",
                "sha256": sha256_file(launcher_path),
            },
            {
                "role": "nxport",
                "path": "nxport.json",
                "mode": "0644",
                "sha256": sha256_file(manifest_path),
            },
        ] + generation_runtime
        if generation_manifest.get("components") != expected_components:
            raise ProjectError("generated runtime manifest closure differs from nxport")
        expected_components_text = "".join(
            "%s\t%s\t%s\t%s\n" % (
                member["role"], member["mode"], member["sha256"],
                member["path"],
            )
            for member in expected_components
        )
        if components_v2_path.read_text(encoding="utf-8") != \
                expected_components_text:
            raise ProjectError("generated runtime component records are stale")
        if sha256_file(identity_path) != generation_id:
            raise ProjectError("generated runtime identity bytes do not match generation_id")
        for member in generation_runtime:
            live_path = port_dir / member["path"]
            immutable_path = (
                generation_root / "files" / "runtime" / member["path"]
            )
            for candidate, label in (
                (live_path, "live runtime member"),
                (immutable_path, "immutable runtime member"),
            ):
                require_regular_file(candidate, label + " " + member["path"])
                actual_mode = "%04o" % stat.S_IMODE(candidate.stat().st_mode)
                if (actual_mode != member["mode"] or
                        sha256_file(candidate) != member["sha256"]):
                    raise ProjectError(
                        "%s differs from generation_runtime: %s" %
                        (label, member["path"])
                    )

    for retired in ("nxbootstrap.sh",
                    "nxbootstrap-%s.sh" % source_state["version"],
                    "nxdeployment.json",
                    "run.sh"):
        if (port_dir / retired).exists() or (stage / retired).exists():
            raise ProjectError(
                "generated deployment contains retired artifact: %s" % retired
            )

    try:
        launcher = launcher_path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise ProjectError(
            "generated launcher is not UTF-8: %s" % error
        )
    if "nxbootstrap %s" % source_state["version"] not in launcher:
        raise ProjectError(
            "generated launcher does not record its generator version"
        )
    if re.search(r"@[A-Z0-9_]+@", launcher):
        raise ProjectError("generated launcher has unresolved tokens")
    if launcher != BOOTSTRAP_GENERATOR.render_launcher(nxport):
        raise ProjectError(
            "generated launcher differs from the canonical nxbootstrap render"
        )
    expected_runtime_exports = (
        ("NXCOMPAT_PORT_ID",
         BOOTSTRAP_GENERATOR.shell_join([nxport["id"]])),
        # V3-UPDATE-01: the guest's lexical identity is the LOGICAL game dir
        # (the frontend's /roms/... root), not the pwd -P physical swap.
        ("NXCOMPAT_GAME_DIR", '"$NXBOOTSTRAP_LOGICAL_GAMEDIR"'),
        ("NXCOMPAT_REQUIRED_CAPABILITIES",
         BOOTSTRAP_GENERATOR.shell_join(nxport["required_capabilities"])),
        ("NXCOMPAT_ENABLED_QUIRKS",
         BOOTSTRAP_GENERATOR.shell_join(nxport["enabled_quirks"])),
        ("NXCOMPAT_RUNTIME_REPORT",
         BOOTSTRAP_GENERATOR.shell_join([nxport["runtime_report"]])),
    )
    for name, value in expected_runtime_exports:
        assignment = "export {}={}".format(name, value)
        if (launcher.count(name + "=") != 1 or
                launcher.count(assignment) != 1):
            raise ProjectError(
                "generated launcher runtime export differs: %s" % name
            )

    language = nxport.get("language")
    expected_language = BOOTSTRAP_GENERATOR.render_language_block(nxport)
    expected_language_reassert = \
        BOOTSTRAP_GENERATOR.render_language_reassert_block(nxport)
    if language is None:
        if "GAME_LANGUAGE=" in launcher or "NXPORT_LANGUAGE" in launcher:
            raise ProjectError(
                "generated launcher exposes language without adapter opt-in"
            )
    elif (launcher.count(expected_language) != 1 or
          launcher.count(expected_language_reassert) != 1):
        raise ProjectError(
            "generated launcher language contract differs from nxport"
        )

    options = nxport.get("options") or []
    expected_options = BOOTSTRAP_GENERATOR.render_options_block(nxport)
    expected_options_reassert = \
        BOOTSTRAP_GENERATOR.render_options_reassert_block(nxport)
    if not options:
        if "GAME_OPTION_" in launcher or "NXBOOTSTRAP_OPTION_" in launcher:
            raise ProjectError(
                "generated launcher exposes options without adapter opt-in"
            )
    elif (launcher.count(expected_options) != 1 or
          launcher.count(expected_options_reassert) != 1):
        raise ProjectError(
            "generated launcher option contract differs from nxport"
        )

    expected_nxextract = BOOTSTRAP_GENERATOR.render_nxextract_block(nxport)
    expected_required = BOOTSTRAP_GENERATOR.render_required_files_block(nxport)
    expected_splash = BOOTSTRAP_GENERATOR.render_splash_block(nxport)
    if launcher.count(expected_nxextract) != 1:
        raise ProjectError(
            "generated launcher NXExtract contract differs from nxport"
        )
    if launcher.count(expected_required) != 1:
        raise ProjectError(
            "generated launcher required-files gate differs from nxport"
        )
    if launcher.count(expected_splash) != 1:
        raise ProjectError(
            "generated launcher nxsplash handoff differs from nxport"
        )
    if re.search(r"NXSPLASH_(?:DISABLE|SKIP)|--(?:disable|skip)", launcher):
        raise ProjectError("generated launcher exposes a nxsplash removal switch")

    required_assignment = "NXBOOTSTRAP_REQUIRED_FILES={}".format(
        BOOTSTRAP_GENERATOR.shell_join(nxport["required_files"])
    )
    required_assignments = re.findall(
        r"(?m)^NXBOOTSTRAP_REQUIRED_FILES=", launcher
    )
    if (len(required_assignments) != 1 or
            launcher.count(required_assignment) != 1):
        raise ProjectError(
            "generated launcher required-files assignment differs from nxport"
        )

    requested_assignments = re.findall(
        r"(?m)^[ \t]*NXEXTRACT_REQUESTED=([01])$", launcher
    )
    nxextract_mode = nxport["nxextract"]["mode"]
    expected_requested = {
        "no": [],
        "yes": ["1"],
        "auto": ["0", "1"],
    }[nxextract_mode]
    if requested_assignments != expected_requested:
        raise ProjectError(
            "generated launcher NXEXTRACT_REQUESTED differs for mode %s" %
            nxextract_mode
        )
    runner_call = 'bash "$NXDIR/run-extractor.sh"'
    if ((nxextract_mode == "no" and runner_call in launcher) or
            (nxextract_mode != "no" and launcher.count(runner_call) != 1)):
        raise ProjectError(
            "generated launcher NXExtract invocation differs for mode %s" %
            nxextract_mode
        )

    bin_assignment = 'BIN="$GAMEDIR/{}"'.format(nxport["executable"])
    if launcher.count(bin_assignment) != 1:
        raise ProjectError("generated launcher executable assignment differs")
    nxextract_index = launcher.index(expected_nxextract)
    required_index = launcher.index(expected_required)
    splash_index = launcher.index(expected_splash)
    adapter_index = launcher.index("# Optional per-port adapter")
    library_index = launcher.index("# Host phase ends here")
    bin_index = launcher.index(bin_assignment)
    # BIN is assigned before the PortMaster handoff. The mandatory splash lives
    # strictly after owner-data/payload gates and before mutable hooks/private
    # libraries; the native child launch remains later still.
    if not (bin_index < nxextract_index < required_index < splash_index <
            adapter_index < library_index):
        raise ProjectError(
            "generated launcher payload/splash gates are outside canonical order"
        )

    executable_path = '"$GAMEDIR/{}"'.format(nxport["executable"])
    executable_preflight = (
        "NXBOOTSTRAP_EXECUTABLE=$(readlink -f {} 2>/dev/null) || "
        "NXBOOTSTRAP_EXECUTABLE=\"\"".format(executable_path),
        'case "$NXBOOTSTRAP_EXECUTABLE" in',
        '"$GAMEDIR"/*) ;;',
        '*) NXBOOTSTRAP_EXECUTABLE="" ;;',
        ('if [ -z "$NXBOOTSTRAP_EXECUTABLE" ] || '
         '[ ! -f "$NXBOOTSTRAP_EXECUTABLE" ] || \\'),
        ('[ ! -s "$NXBOOTSTRAP_EXECUTABLE" ] || [ -L {} ]; then'.format(
            executable_path
        )),
        '$ESUDO chmod +x "$NXBOOTSTRAP_EXECUTABLE"',
    )
    preflight = launcher[:nxextract_index]
    cursor = -1
    for marker in executable_preflight:
        marker_index = preflight.find(marker, cursor + 1)
        if marker_index < 0:
            raise ProjectError(
                "generated launcher lacks safe executable preflight: %s" %
                marker
            )
        cursor = marker_index

    stable_lock = (
        'NXBOOTSTRAP_LOCK_DIR="$NXBOOTSTRAP_LOCK_ROOT/.nxbootstrap-',
        ('NXBOOTSTRAP_LOCK_FILE="$NXBOOTSTRAP_LOCK_DIR/nxport-'
         '{}.flock"'.format(nxport["id"])),
        'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"',
        "command ls -Lldn /proc/self/fd/9",
        '"$NXBOOTSTRAP_LOCK_FILE" -ef /proc/self/fd/9',
        "flock -n 9",
    )
    for marker in stable_lock:
        marker_index = preflight.find(marker, cursor + 1)
        if marker_index < 0:
            raise ProjectError(
                "generated launcher lacks stable instance lock: %s" % marker
            )
        cursor = marker_index

    required_guard_markers = (
        "while IFS= read -r required_file; do",
        ('required_path=$(readlink -f "$GAMEDIR/$required_file" '
         '2>/dev/null) || required_path=""'),
        'case "$required_path" in',
        '"$GAMEDIR"/*) ;;',
        ('if [ -z "$required_path" ] || [ ! -f "$required_path" ] || \\\n'
         '     [ ! -s "$required_path" ] || '
         '[ -L "$GAMEDIR/$required_file" ]; then'),
        'done <<< "$NXBOOTSTRAP_REQUIRED_FILES"',
        "unset NXBOOTSTRAP_REQUIRED_FILES required_file required_path",
    )
    required_cursor = -1
    for marker in required_guard_markers:
        marker_index = expected_required.find(marker, required_cursor + 1)
        if marker_index < 0:
            raise ProjectError(
                "generated launcher lacks required-files guard: %s" % marker
            )
        required_cursor = marker_index
    for guarantee in ("flock -n 9",
                      'wait "$game_pid"',
                      "trap '' INT TERM HUP",
                      "nxbootstrap_finish",
                      '[ "$NXBOOTSTRAP_FINISHED" = 0 ] || return 0',
                      "nxbootstrap_abort_before_game 129",
                      "nxbootstrap_abort_before_game 130",
                      "nxbootstrap_abort_before_game 143",
                      "NXBOOTSTRAP_CHILD_STARTTIME=${20}",
                      "NXBOOTSTRAP_SHUTDOWN_TICKS=10",
                      'builtin kill -KILL "$game_pid"'):
        if guarantee not in launcher:
            raise ProjectError(
                "generated launcher lacks golden-port guarantee: %s" %
                guarantee
            )

    return {
        "launcher_sha256": sha256_file(launcher_path),
        "nxport_sha256": sha256_file(manifest_path),
        "nxsplash_sha256": sha256_file(splash_path),
    }


def rename_noreplace(source, destination):
    try:
        renameat2 = ctypes.CDLL(None, use_errno=True).renameat2
    except AttributeError as error:
        raise ProjectError(
            "host lacks renameat2; no-overwrite publication is unavailable"
        ) from error
    renameat2.argtypes = (
        ctypes.c_int, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_char_p,
        ctypes.c_uint,
    )
    renameat2.restype = ctypes.c_int
    result = renameat2(
        AT_FDCWD, os.fsencode(source),
        AT_FDCWD, os.fsencode(destination),
        RENAME_NOREPLACE,
    )
    if result == 0:
        return
    selected_errno = ctypes.get_errno()
    if selected_errno in (errno.EEXIST, errno.ENOTEMPTY):
        raise ProjectError("refusing to overwrite output: %s" % destination)
    raise ProjectError(
        "cannot publish output without replacement: %s" %
        os.strerror(selected_errno)
    )


def generate_project(document, output, source_root=REPOSITORY):
    config = validate_project(document, source_root)
    output = Path(os.path.abspath(os.fspath(output)))
    parent = output.parent
    if output.name in ("", ".", ".."):
        raise ProjectError("invalid output name")
    if parent.is_symlink() or not parent.is_dir():
        raise ProjectError("output parent must be a real existing directory")
    if output.exists() or output.is_symlink():
        raise ProjectError("refusing to overwrite output: %s" % output)

    stage = Path(tempfile.mkdtemp(prefix=".nxgenerator.", dir=str(parent)))
    published = False
    try:
        bootstrap_source = bootstrap_source_state()
        portmaster_source = portmaster_source_state()
        splash_source = nxsplash_source_state(
            execution_role_architecture(config["nxport"], "splash")
        )
        nxextract_source = None
        if config["recipe_source"] is not None:
            nxextract_source = nxextract_source_state(
                execution_role_architecture(config["nxport"], "extractor")
            )
        nxport_input = stage / ".nxport-input.json"
        atomic_write_bytes(
            nxport_input, canonical_json(document["nxport"]), 0o600
        )
        try:
            BOOTSTRAP_GENERATOR.generate(
                nxport_input, stage, False,
                runtime_root=config["runtime_root"],
            )
        except (OSError, ValueError, BOOTSTRAP_GENERATOR.ManifestError) as error:
            raise ProjectError("nxbootstrap generation failed: %s" % error)
        nxport_input.unlink()
        validate_bootstrap_deployment(
            stage, config["nxport"], bootstrap_source, splash_source
        )

        port_id = config["nxport"]["id"]
        port_dir = stage / port_id
        adapter_dir = port_dir / "adapter"
        adapter_dir.mkdir(mode=0o755)

        if config["recipe_source"] is not None:
            generation_v2 = config["runtime_root"] is not None
            nxextract_dir = port_dir / "nxextract"
            if generation_v2:
                if nxextract_dir.is_symlink() or not nxextract_dir.is_dir():
                    raise ProjectError(
                        "generation-v2 NXExtract directory was not materialized"
                    )
            else:
                nxextract_dir.mkdir(mode=0o755)
            for source_name, target_name, mode in NXEXTRACT_COMMON_FILES:
                source = NXEXTRACT_ROOT / source_name
                if source.is_symlink() or not source.is_file():
                    raise ProjectError(
                        "canonical NXExtract file is unsafe: %s" % source_name
                    )
                materialize_nxextract_file(
                    nxextract_dir / target_name, source.read_bytes(), mode,
                    generation_v2, "NXExtract core %s" % target_name,
                )
            ui_source = NXEXTRACT_ROOT / nxextract_source["artifact"]["path"]
            ui_payload = ui_source.read_bytes()
            if (sha256_bytes(ui_payload) !=
                    nxextract_source["artifact"]["sha256"]):
                raise ProjectError(
                    "NXExtract UI artifact changed after release validation"
                )
            materialize_nxextract_file(
                nxextract_dir / "nxextract-ui", ui_payload, 0o755,
                generation_v2, "NXExtract UI",
            )
            materialize_nxextract_file(
                port_dir / "extractor.json",
                config["recipe_source"].read_bytes(), 0o644,
                generation_v2, "NXExtract recipe",
            )

            # GAMEDATA-DIR-01: materialize the owner-data directory through a
            # real, bilingual, generated marker file. ZIPs built from files
            # only cannot carry an empty directory, so the marker is the
            # physical guarantee that <port-id>/gamedata/ exists after a
            # clean installation. Never an empty ZIP directory entry.
            owner_data = config["owner_data"]
            gamedata_dir = port_dir / owner_data["directory"]
            gamedata_dir.mkdir(mode=0o755)
            reference_version = owner_data["reference"]["version"] or "-"
            marker = render_template("GAMEDATA-README.txt.in", {
                "TITLE": config["nxport"]["title"],
                "ACCEPTED_FORMATS": "/".join(owner_data["formats"]),
                "REFERENCE_VERSION": reference_version,
            })
            atomic_write_bytes(
                gamedata_dir / "README.txt", marker, 0o644
            )

        # V3-CONTROLLERS-01: every new port ships an immutable default
        # controls map; the bootstrap materializes the owner's editable copy
        # at first boot and never overwrites it afterwards.
        defaults_dir = port_dir / "defaults"
        defaults_dir.mkdir(mode=0o755)
        if config["controls"] is None:
            # Legacy schema v1/v2 output remains byte-compatible. New V3 ports
            # can never reach this generic compatibility template.
            controls_sections = (
                "[menu]\nA = ui.confirm\nB = ui.cancel\n"
                "UP = ui.up\nDOWN = ui.down\nLEFT = ui.left\n"
                "RIGHT = ui.right\nSTART = ui.start\nSELECT = ui.select\n\n"
                "[gameplay]\nA = player.primary\nB = player.secondary\n"
                "X = player.tertiary\nY = player.quaternary\n"
                "UP = player.up\nDOWN = player.down\nLEFT = player.left\n"
                "RIGHT = player.right\nSTART = ui.start\nSELECT = ui.select\n"
            )
        else:
            controls_sections = render_controls_sections(config["controls"])
        controllers_default = render_template("NEXTOSCONTROLLERS.gptk.in", {
            "PORT_ID": port_id,
            # Schema 1 unless the port opted in: the rendered magic is the
            # ONLY thing that selects the format, and a v1 port keeps the
            # exact bytes it had before C4.
            "GPTK_SCHEMA": str(
                1 if config["controls"] is None
                else config["controls"].get("schema", 1)
            ),
            # TEARSCAPE-CONTROLS-LIVE: absence of the explicit live-runtime
            # opt-in is a static description, not a promise that owner edits
            # reach the engine.  The opt-in retains the established editable
            # header byte-for-byte.
            "GPTK_GUIDANCE": (
                GPTK_LIVE_GUIDANCE
                if (config["controls"] is not None and
                    config["controls"].get("runtime_mapping") ==
                    "nxinput-gptk")
                else GPTK_STATIC_GUIDANCE
            ),
            "CONTROL_SECTIONS": controls_sections.rstrip(),
        })
        atomic_write_bytes(
            defaults_dir / "NEXTOSCONTROLLERS.gptk", controllers_default, 0o644
        )
        if config["video"] is None:
            settings_default = render_template("NEXTOSSETTINGS.txt.in", {
                "TITLE": config["nxport"]["title"],
            })
        else:
            video = config["video"]
            settings_default = render_template("NEXTOSSETTINGS-2.txt.in", {
                "TITLE": config["nxport"]["title"],
                "VIDEO_AUTHORITY": video["authority"],
                "VIDEO_OUTPUT_SIZE": video["output_size"],
                "VIDEO_ASPECT": video["aspect"],
                "VIDEO_ASPECT_POLICIES": "|".join(video["aspect_policies"]),
                "VIDEO_FILTER": video["filter"],
                "VIDEO_INVALID_POLICY": video["invalid_policy"],
            })
        atomic_write_bytes(
            defaults_dir / "NEXTOSSETTINGS.txt", settings_default, 0o644
        )
        # V5 7A.1: the live hook seed and the seed->live ownership manifest.
        ownership = {
            "schema": "nx-ownership/1",
            "generated_by": "nxgenerator %s" % read_version(ROOT / "VERSION", "nxgenerator"),
            "order": ["heal-sealed", "extract", "splash", "seed-owner",
                      "sealed-env", "owner-env", "launch"],
            "paths": [
                {"path": "defaults/NEXTOSCONTROLLERS.gptk", "class": "owner-seeded",
                 "live": "NEXTOSCONTROLLERS.gptk", "healed": False},
                {"path": "defaults/NEXTOSSETTINGS.txt", "class": "owner-seeded",
                 "live": "NEXTOSSETTINGS.txt", "healed": False,
                 "schema": "NEXTOS_SETTINGS/2" if config["video"] else
                           "NEXTOS_SETTINGS/1"},
            ],
        }
        if config["owner_runtime"] is not None:
            seed_bytes = config["owner_runtime"]["hook_seed"].read_bytes()
            atomic_write_bytes(defaults_dir / "port-env.sh", seed_bytes, 0o644)
            ownership["paths"].append(
                {"path": "defaults/port-env.sh", "class": "owner-seeded",
                 "live": "port-env.sh", "healed": False})
            ownership["paths"].append(
                {"path": "adapter-env.sh", "class": "sealed-runtime",
                 "live": None, "healed": True})
        if config["video"] is not None and config["video"]["native_config"]:
            ownership["paths"].append(
                {"path": config["video"]["native_config"],
                 "class": "owner-native", "live": config["video"]["native_config"],
                 "healed": False, "authority": config["video"]["authority"]})
        atomic_write_bytes(
            port_dir / "OWNERSHIP.json", canonical_json(ownership), 0o644
        )
        # V5 (M1c 1.2): the expected host closure, next to the owner it
        # describes, so a host gate never derives the expectation from the
        # map under test.
        if config["controls"] is not None:
            atomic_write_bytes(
                port_dir / "CONTROLS-CLOSURE.json",
                canonical_json(build_controls_closure(
                    config["controls"], config["nxport"]["id"])),
                0o644,
            )

        adapter_contract = {
            "schema": "nxadapter-skeleton-v1",
            "schema_version": 1,
            "status": "unimplemented_nonrelease",
            "release_ready": False,
            "lifecycle": {"sequence": [], "source_evidence": []},
            "jni": {"classes": [], "methods": [], "callbacks": []},
            "imports": [],
            "audio": {"format": None, "callbacks": []},
            "input": {
                "mapping": None,
                "touch": None,
                "actions": (
                    [] if config["controls"] is None
                    else config["controls"]["actions"]
                ),
                "contexts": (
                    {} if config["controls"] is None
                    else config["controls"]["contexts"]
                ),
                "mapping_authority": (
                    None if config["controls"] is None
                    else "sdl-portmaster-complete-first"
                ),
                **(
                    {
                        "runtime_mapping":
                            config["controls"]["runtime_mapping"],
                        "runtime_contract":
                            config["controls"]["runtime_contract"],
                    }
                    if config["controls"] is not None and
                    config["controls"].get("runtime_mapping")
                    else {}
                ),
            },
            "language_access": config["language_access"],
            "graphics": config["graphics"],
            # V4 opt-ins. They are ALWAYS present and ALWAYS explicit, so a
            # reader never has to guess whether a port declared nothing or the
            # generator dropped a field. Absence in the project manifest
            # normalizes to the disabled/no-op value here.
            "display": (
                config["display"] if config["display"] is not None
                else {"policy": "game", "remap_input": False}
            ),
            "egl_binding": (
                config["egl_binding"] if config["egl_binding"] is not None
                else {"enabled": False, "imports": []}
            ),
            "input_sdl3_portmaster": (
                config["sdl3_portmaster"]
                if config["sdl3_portmaster"] is not None
                else {"enabled": False, "private_sdl3_sha256": ""}
            ),
            "persistence": {"paths": [], "callbacks": []},
            "terminal": {"action": None, "evidence": []},
            "quirks": [],
            "prohibitions": [
                "do not invent lifecycle order",
                "do not promote offsets or callbacks to defaults",
                "do not invent save or terminal behavior",
            ],
        }
        # V4-CONTROLLERS-03/C3 (missao 114A): o campo so' EXISTE quando o port
        # o declarou. Sem opt-in a saida fica byte a byte igual ao baseline
        # anterior a' C3 -- e' o que o gate golden prova. Acrescentar um campo
        # desabilitado a todo contrato regenerado contradiria a preservacao de
        # bytes prometida aos ports ja publicados.
        if config["controller_profiles"] is not None:
            adapter_contract["input_controller_profiles"] = \
                config["controller_profiles"]
        # Promocao opt-in: substitui o skeleton pelo adapter-contract REAL do
        # port, verbatim. Reproduzivel (mesma fonte => mesmo resultado) e nunca
        # volta ao scaffold enquanto o bloco promotion existir.
        if config.get("promotion") is not None:
            adapter_contract = config["promotion"]["contract"]
        atomic_write_bytes(
            adapter_dir / "adapter-contract.json",
            canonical_json(adapter_contract), 0o644,
        )

        architecture = config["nxport"]["architecture"]
        portmaster_arch = "armhf" if architecture == "armv7" else architecture
        attributes = {
            "title": config["nxport"]["title"],
            "arch": [portmaster_arch],
            "min_glibc": config["min_glibc"],
        }
        if config["portmaster_runtime"]:
            attributes["runtime"] = config["portmaster_runtime"]
        metadata = {
            "version": config["metadata_version"],
            "name": port_id + ".zip",
            "items": [config["nxport"]["launcher_name"], port_id + "/"],
            "items_opt": [],
            "attr": attributes,
        }
        atomic_write_bytes(
            port_dir / "port.json", canonical_json(metadata), 0o644
        )
        gameinfo = render_gameinfo(config["nxport"])
        validate_gameinfo(gameinfo, config["nxport"])
        atomic_write_bytes(port_dir / "gameinfo.xml", gameinfo, 0o644)

        project_canonical = canonical_json(document)
        atomic_write_bytes(
            port_dir / "nxproject.json", project_canonical, 0o644
        )
        atomic_write_bytes(
            port_dir / "LICENSE", config["license_source"].read_bytes(), 0o644
        )

        readme = render_template("README.md.in", {
            "TITLE": config["nxport"]["title"],
            "PORT_ID": port_id,
            "ARCHITECTURE": architecture,
            "PORTMASTER_ARCH": portmaster_arch,
            "LICENSE_SPDX": config["license_spdx"],
        })
        atomic_write_bytes(port_dir / "README.md", readme, 0o644)
        owner_block_en = ""
        owner_block_pt = ""
        if config["recipe_source"] is not None:
            owner_dir = config["owner_data"]["directory"]
            owner_formats = "/".join(config["owner_data"]["formats"])
            owner_block_en = (
                "Owner data goes in exactly this folder (created by the "
                "installation):\n\n```text\nports/%s/%s/\n```\n\n"
                "Accepted formats: %s. See `%s/README.txt` inside the "
                "installed port.\n\n" % (
                    port_id, owner_dir, owner_formats, owner_dir,
                )
            )
            owner_block_pt = (
                "Os dados do dono vão exatamente nesta pasta (criada pela "
                "instalação):\n\n```text\nports/%s/%s/\n```\n\n"
                "Formatos aceitos: %s. Veja `%s/README.txt` dentro do port "
                "instalado.\n\n" % (
                    port_id, owner_dir, owner_formats, owner_dir,
                )
            )
        installation = render_template("INSTALLATION.md.in", {
            "TITLE": config["nxport"]["title"],
            "PORT_ID": port_id,
            "LAUNCHER_NAME": config["nxport"]["launcher_name"],
            "ARCHITECTURE": architecture,
            "OWNER_DATA_BLOCK_EN": owner_block_en,
            "OWNER_DATA_BLOCK_PT": owner_block_pt,
        })
        atomic_write_bytes(
            port_dir / "INSTALLATION.md", installation, 0o644
        )

        # Author composition is deliberately last among scaffold producers:
        # README/INSTALLATION may be replaced only under the authored contract,
        # while every other member must still be absent. The retained source
        # bytes are then visible to the artifact inventory and receipt below.
        materialize_package_payload(
            port_dir, config["package_payload"],
            config["documentation"]["status"],
        )

        nxgenerator_version = read_version(ROOT / "VERSION", "nxgenerator")
        nxextract_version = read_version(
            NXEXTRACT_ROOT / "VERSION", "NXExtract"
        )
        source_pins = {
            "nxbootstrap": {
                "version": bootstrap_source["version"],
                "source_files": bootstrap_source["source_files"],
            },
            "nxsplash": splash_source,
            "nxextract": None,
            "portmaster": portmaster_source,
        }
        if config["recipe_source"] is not None:
            source_pins["nxextract"] = {
                "version": nxextract_version,
                "ui_version": nxextract_source["ui_version"],
                "files": {
                    target_name: sha256_file(NXEXTRACT_ROOT / source_name)
                    for source_name, target_name, _mode in NXEXTRACT_COMMON_FILES
                },
                "ui_release_manifest_sha256": nxextract_source[
                    "release_manifest_sha256"
                ],
                "ui_source_sha256": nxextract_source["source_sha256"],
                "ui_artifact": nxextract_source["artifact"],
                "recipe_sha256": sha256_file(config["recipe_source"]),
            }
            source_pins["nxextract"]["files"]["nxextract-ui"] = (
                nxextract_source["artifact"]["sha256"]
            )
        artifacts = artifact_inventory(stage)
        # V3-UPDATE-01/V3-CLOCK-01: ONE generation identity shared by the
        # launcher (@GENERATION_ID@), nxport.json, the runtime generation dir
        # and this receipt. The canonical identity preimage binds the nxport,
        # bootstrap generator/template and architecture-matched splash; it is
        # never derived from wall clock, mtime or a textual version ordering.
        generation_id = BOOTSTRAP_GENERATOR.generation_identity(
            config["nxport"]
        )
        generation = {
            "schema": "nxgenerator-receipt-v1",
            "schema_version": 1,
            "generation_id": generation_id,
            "generator": {
                "name": "nxgenerator",
                "version": nxgenerator_version,
            },
            "project_manifest_sha256": sha256_bytes(project_canonical),
            "source_pins": source_pins,
            "artifacts": artifacts,
            "claims": (
                {
                    "deterministic_scaffold": True,
                    "release_ready": False,
                    "physical_support_proven": False,
                    "adapter_lifecycle_implemented": False,
                }
                if config.get("promotion") is None
                else {
                    "deterministic_scaffold": True,
                    "release_ready":
                        config["promotion"]["claims"]["release_ready"],
                    "physical_support_proven":
                        config["promotion"]["claims"]
                        ["physical_support_proven"],
                    "adapter_lifecycle_implemented":
                        config["promotion"]["claims"]
                        ["adapter_lifecycle_implemented"],
                }
            ),
        }
        if config["nxport"].get("execution_roles") is not None:
            generation["execution_roles"] = config["nxport"][
                "execution_roles"
            ]
        atomic_write_bytes(
            port_dir / "GENERATION.json", canonical_json(generation), 0o644
        )

        rename_noreplace(stage, output)
        published = True
        raw_controls = document.get("controls")
        if isinstance(raw_controls, dict) and "proof" in raw_controls:
            write_input_proof_roteiros(raw_controls, port_id, output)
        return output, config
    finally:
        if not published and stage.exists():
            shutil.rmtree(stage)


def load_project(path):
    if path.is_symlink() or not path.is_file():
        raise ProjectError("project manifest must be a regular file")
    try:
        payload = path.read_bytes()
        if payload.startswith(b"\xef\xbb\xbf"):
            raise ProjectError("project manifest cannot contain a UTF-8 BOM")
        text = payload.decode("utf-8")

        def unique_pairs(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ProjectError(
                        "project manifest contains duplicate key %r" % key
                    )
                result[key] = value
            return result

        def reject_constant(value):
            raise ProjectError(
                "project manifest contains non-JSON constant %s" % value
            )

        return json.loads(
            text, object_pairs_hook=unique_pairs,
            parse_constant=reject_constant,
        )
    except ProjectError:
        raise
    except (OSError, UnicodeDecodeError, ValueError) as error:
        raise ProjectError("cannot read project manifest: %s" % error)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate a deterministic PortMaster project scaffold"
    )
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--source-root", type=Path, default=REPOSITORY,
        help="root for license and recipe paths (defaults to the framework repository)",
    )
    options = parser.parse_args(argv)
    try:
        output, config = generate_project(
            load_project(options.manifest), options.output,
            options.source_root,
        )
    except ProjectError as error:
        print("nxgenerator: %s" % error, file=sys.stderr)
        return 1
    print("generated project %s (%s) in %s" % (
        config["nxport"]["id"],
        config["nxport"]["architecture"],
        output,
    ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
