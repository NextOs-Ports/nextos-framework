#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate the invariant PortMaster wrapper/runtime from nxport.json."""

import argparse
import hashlib
import json
import os
import posixpath
import re
import shlex
import stat
import sys
import tempfile
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]
NXSPLASH_ROOT = ROOT.parent / "nxsplash"
NXSPLASH_MANIFEST_PATH = NXSPLASH_ROOT / "release" / "manifest-v1.json"
NXSPLASH_RUNTIME_NAME = "nxsplash-nextos"
CAPABILITY_REGISTRY_PATH = (
    ROOT.parent / "nxcompat" / "capabilities-v1.json"
)
QUIRK_REGISTRY_PATH = (
    ROOT.parent / "nxcompat" / "quirk-registry-v1.json"
)
CURRENT_SCHEMA_VERSION = 3
LEGACY_SCHEMA_VERSIONS = (1, 2)
# A versao CANONICA -- a que a geracao nova escreve no manifesto.
NXEXTRACT_VERSION = "1.3.0"
# E o conjunto que um manifesto pode DECLARAR. Enquanto isto era um valor unico,
# subir o NXExtract obrigava todo port publicado a migrar no mesmo instante, o
# que contradiz a regra de promover por opt-in. 23/08/2026 (onda v2): o
# conjunto e' LIDO do registro canonico de identidades
# (framework/nxrelease/nxextract-engines-v1.json) em vez de cravado aqui --
# a copia local ja' apodreceu uma vez (aceitava so' 1.2.14 com o registro
# suportando ate' 1.2.17). O gerador nao verifica hash de motor; hash e'
# assunto do nxrelease.
_ENGINE_REGISTRY = json.loads(
    (ROOT.parent / "nxrelease/nxextract-engines-v1.json").read_text(
        encoding="utf-8"))
if _ENGINE_REGISTRY.get("canonical") != NXEXTRACT_VERSION:
    raise RuntimeError(
        "nxextract canonical drifted: generator=%s registry=%s"
        % (NXEXTRACT_VERSION, _ENGINE_REGISTRY.get("canonical")))
NXEXTRACT_SUPPORTED_VERSIONS = tuple(sorted(_ENGINE_REGISTRY["engines"]))
NXBOOTSTRAP_VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", NXBOOTSTRAP_VERSION):
    raise RuntimeError("invalid nxbootstrap VERSION")
NXSPLASH_VERSION = (NXSPLASH_ROOT / "VERSION").read_text(
    encoding="utf-8").strip()
if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", NXSPLASH_VERSION):
    raise RuntimeError("invalid nxsplash VERSION")
KNOWN_KEYS_V1 = {
    "schema_version",
    "id",
    "title",
    "launcher_name",
    "architecture",
    "executable",
    "argument_mode",
    "home_mode",
    "nxextract",
    "required_files",
    "extra_library_paths",
    "prepare_script",
}
KNOWN_KEYS_V2 = {
    "schema_version",
    "id",
    "title",
    "launcher_name",
    "architecture",
    "executable",
    "argument_mode",
    "home_mode",
    "nxextract",
    "required_files",
    "private_library_paths",
    "prepare_script",
    "required_capabilities",
    "enabled_quirks",
    "language",
    "options",
    "runtime_report",
    "execution_roles",
    "sdl_provider",
    "video_proof",
}
KNOWN_KEYS_V3 = KNOWN_KEYS_V2 | {"generation_runtime", "owner_runtime"}
PUBLIC_PATH_RE = re.compile(r"(?:^|/)(?:home|Users)/[^/]+", re.IGNORECASE)
WINDOWS_PATH_RE = re.compile(r"^[A-Za-z]:[\\/]")
CAPABILITY_RE = re.compile(
    r"^(?:host|graphics|audio|input)\.[a-z0-9][a-z0-9.-]{0,62}$"
)
QUIRK_RE = re.compile(
    r"^(?:adapter|engine|game)\.[a-z0-9][a-z0-9._-]{0,62}$"
)
LANGUAGE_CODE_RE = re.compile(r"^[a-z]{2}(?:-[a-z0-9]{2,8})?$")
ROLE_ID_RE = re.compile(r"^[a-z][a-z0-9-]{0,31}$")
OPTION_ID_RE = re.compile(r"^[a-z][a-z0-9-]{0,31}$")
OPTION_VALUE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,31}$")
OPTION_ENVIRONMENT_RE = re.compile(r"^[A-Z][A-Z0-9_]{2,63}$")
OPTION_LABEL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 /()+.,-]{0,63}$")
# Nomes que o launcher, o framework ou o runtime ja possuem. Uma opcao
# declarativa nunca pode redefinir um deles.
OPTION_RESERVED_PREFIXES = (
    "NXPORT_", "NXBOOTSTRAP_", "NXEXTRACT_", "NXSPLASH_", "SDL_", "LD_",
    "XDG_", "PM_", "GL_", "MESA_", "EGL_", "ALSA_", "PIPEWIRE_", "SPA_",
    "PULSE_", "PORT_", "TZ", "PATH", "HOME",
)
OPTION_RESERVED_NAMES = frozenset((
    "GAME_LANGUAGE", "GAMEDIR", "HOME", "PATH", "TZ", "TMPDIR", "USER",
    "SHELL", "IFS", "CFW_NAME", "ESUDO", "controlfolder",
))
ARCHITECTURES = ("aarch64", "armv7", "x86_64", "i386")
ARCH_ELF_IDENTITIES = {
    "aarch64": (2, 183),
    "armv7": (1, 40),
    "x86_64": (2, 62),
    "i386": (1, 3),
}
ARCH_INTERPRETERS = {
    "aarch64": "/lib/ld-linux-aarch64.so.1",
    "armv7": "/lib/ld-linux-armhf.so.3",
    "x86_64": "/lib64/ld-linux-x86-64.so.2",
    "i386": "/lib/ld-linux.so.2",
}
EXECUTION_ROLE_KEYS = {
    "architecture", "executable", "executor", "interpreter", "closure",
}
GENERATION_RUNTIME_ROLES = (
    "executable", "private-library", "runtime-data", "runtime-hook",
    "nxextract-recipe", "nxextract-engine", "nxextract-runner",
    "nxextract-runtime-env", "nxextract-ui", "nxextract-helper",
    "nxextract-spec", "nxsplash",
)
GENERATION_RUNTIME_ROLE_ORDER = {
    role: index for index, role in enumerate(GENERATION_RUNTIME_ROLES)
}
NXEXTRACT_GENERATION_CORE = (
    ("nxextract-recipe", "extractor.json", "0644"),
    ("nxextract-engine", "nxextract/nxextract.py", "0644"),
    ("nxextract-runner", "nxextract/run-extractor.sh", "0644"),
    ("nxextract-runtime-env", "nxextract/nxextract-runtime-env.sh", "0644"),
    ("nxextract-ui", "nxextract/nxextract-ui", "0755"),
)
NXEXTRACT_GENERATION_CORE_BY_ROLE = {
    role: (path, mode) for role, path, mode in NXEXTRACT_GENERATION_CORE
}
NXEXTRACT_GENERATION_CORE_PATHS = frozenset(
    path for _role, path, _mode in NXEXTRACT_GENERATION_CORE
)
NXEXTRACT_GENERATION_EXTRA_ROLES = frozenset((
    "nxextract-helper", "nxextract-spec",
))
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def load_capability_registry():
    try:
        data = json.loads(CAPABILITY_REGISTRY_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError("cannot load capability registry: %s" % error)
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
        if not isinstance(identifier, str) or not CAPABILITY_RE.fullmatch(identifier):
            raise RuntimeError("invalid capability registry identifier")
        identifiers.append(identifier)
    if len(identifiers) != len(set(identifiers)):
        raise RuntimeError("duplicate capability registry identifier")
    return tuple(identifiers)


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
        if not isinstance(identifier, str) or not QUIRK_RE.fullmatch(identifier):
            raise RuntimeError("invalid quirk registry identifier")
        if not isinstance(entry.get("condition"), str) or \
                not isinstance(entry.get("effect"), str) or \
                not isinstance(entry.get("evidence"), list):
            raise RuntimeError(
                "quirk registry entry lacks condition/effect/evidence")
        identifiers.append(identifier)
    if len(identifiers) != len(set(identifiers)):
        raise RuntimeError("duplicate quirk registry identifier")
    return tuple(identifiers)


CAPABILITY_REGISTRY_ERROR = None
try:
    CAPABILITY_IDENTIFIERS = load_capability_registry()
    QUIRK_IDENTIFIERS = load_quirk_registry()
except RuntimeError as error:
    CAPABILITY_IDENTIFIERS = ()
    QUIRK_IDENTIFIERS = ()
    CAPABILITY_REGISTRY_ERROR = str(error)
CAPABILITY_IDS = frozenset(CAPABILITY_IDENTIFIERS)
QUIRK_IDS = frozenset(QUIRK_IDENTIFIERS)
CAPABILITY_ORDER = {
    identifier: index
    for index, identifier in enumerate(CAPABILITY_IDENTIFIERS)
}


class ManifestError(Exception):
    pass


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def nxsplash_artifact(architecture):
    try:
        release = json.loads(
            NXSPLASH_MANIFEST_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ManifestError("cannot load nxsplash release: %s" % error)
    if (not isinstance(release, dict) or release.get("schema_version") != 1 or
            release.get("component") != "nxsplash" or
            release.get("version") != NXSPLASH_VERSION or
            release.get("duration_ms") != 5000):
        raise ManifestError("invalid nxsplash release header")
    artifacts = release.get("artifacts")
    artifact = artifacts.get(architecture) if isinstance(artifacts, dict) else None
    if not isinstance(artifact, dict):
        raise ManifestError(
            "nxsplash has no artifact for architecture %s" % architecture)
    expected_path = "release/%s/%s" % (architecture, NXSPLASH_RUNTIME_NAME)
    if artifact.get("path") != expected_path:
        raise ManifestError("nxsplash artifact path is not canonical")
    expected_sha = artifact.get("sha256")
    if not isinstance(expected_sha, str) or not re.fullmatch(
            r"[0-9a-f]{64}", expected_sha):
        raise ManifestError("nxsplash artifact hash is invalid")
    source = NXSPLASH_ROOT / PurePosixPath(expected_path)
    if source.is_symlink() or not source.is_file():
        raise ManifestError("nxsplash artifact is missing or unsafe")
    if stat.S_IMODE(source.stat().st_mode) != 0o755:
        raise ManifestError("nxsplash artifact mode must be 0755")
    if source.stat().st_size != artifact.get("size"):
        raise ManifestError("nxsplash artifact size differs from release manifest")
    if sha256_file(source) != expected_sha:
        raise ManifestError("nxsplash artifact hash differs from release manifest")
    return source, expected_sha


def require_string(data, key, allow_empty=False):
    value = data.get(key)
    if not isinstance(value, str) or (not allow_empty and not value):
        raise ManifestError("%s must be a%s string" %
                            (key, "n optionally empty" if allow_empty else " non-empty"))
    if any(ord(character) < 0x20 or 0x7f <= ord(character) <= 0x9f
           for character in value):
        raise ManifestError("%s contains a control character" % key)
    return value


def require_object(data, key):
    value = data.get(key)
    if not isinstance(value, dict):
        raise ManifestError("%s must be an object" % key)
    return value


def reject_personal_path(value, key):
    if PUBLIC_PATH_RE.search(value) or WINDOWS_PATH_RE.match(value):
        raise ManifestError("%s contains a personal or host path" % key)


def require_relative(value, key, allow_empty=False):
    if allow_empty and value == "":
        return value
    path = PurePosixPath(value)
    if len(value) > 512:
        raise ManifestError("%s is longer than 512 characters" % key)
    if path.is_absolute() or not path.parts or any(part in ("", ".", "..")
                                                   for part in path.parts):
        raise ManifestError("%s must be a normalized relative path" % key)
    reject_personal_path(value, key)
    return value


def string_list(data, key, paths=False):
    value = data.get(key, [])
    if not isinstance(value, list) or any(not isinstance(item, str) or not item
                                          for item in value):
        raise ManifestError("%s must be a list of non-empty strings" % key)
    if len(value) != len(set(value)):
        raise ManifestError("%s contains duplicates" % key)
    for item in value:
        if any(ord(character) < 0x20 or 0x7f <= ord(character) <= 0x9f
               for character in item):
            raise ManifestError("%s contains a control character" % key)
        if paths:
            require_relative(item, key)
    return value


def named_list(data, key, pattern, allowed=None):
    values = string_list(data, key)
    for value in values:
        if not pattern.fullmatch(value):
            raise ManifestError("%s contains an invalid name: %s" % (key, value))
        if ".device." in ".%s." % value or value.startswith("device."):
            raise ManifestError("%s cannot select a device by name" % key)
        if allowed is not None and value not in allowed:
            raise ManifestError("%s contains an unknown name: %s" %
                                (key, value))
    return values


def normalize_legacy_v1(data):
    unknown = sorted(set(data) - KNOWN_KEYS_V1)
    if unknown:
        raise ManifestError("unknown field(s): %s" % ", ".join(unknown))
    normalized = dict(data)
    # Schema v1 has always upgraded to v2.  Keep that exact compatibility
    # boundary: schema v3 is an explicit opt-in for generation_runtime, never
    # a silent migration of an old scaffold.
    normalized["schema_version"] = 2
    normalized["nxextract"] = {
        "mode": data.get("nxextract", "auto"),
        "version": NXEXTRACT_VERSION,
    }
    normalized["private_library_paths"] = data.get(
        "extra_library_paths", []
    )
    normalized.pop("extra_library_paths", None)
    normalized["required_capabilities"] = []
    normalized["enabled_quirks"] = []
    normalized["language"] = None
    normalized["options"] = []
    normalized["runtime_report"] = "log-and-logo"
    return normalized


def _path_below(path, directory):
    """Return true only for a strict descendant of a declared directory."""
    path_parts = PurePosixPath(path).parts
    directory_parts = PurePosixPath(directory).parts
    return (len(path_parts) > len(directory_parts) and
            path_parts[:len(directory_parts)] == directory_parts)


def validate_generation_runtime(data, executable, required, libraries,
                                prepare, splash_architecture,
                                nxextract_mode):
    """Validate and normalize the opt-in runtime rollback closure.

    The hashes live in nxport itself so generation identity/rendering remain
    pure.  Runtime bytes are checked later against --runtime-root before any
    generation is committed.  NXSplash is framework-owned and is inserted
    canonically when the author did not repeat it in the input list.
    """
    if "generation_runtime" not in data:
        return None
    entries = data["generation_runtime"]
    if not isinstance(entries, list) or not entries:
        raise ManifestError("generation_runtime must be a non-empty list")
    normalized = []
    seen_paths = set()
    executable_count = 0
    splash_count = 0
    nxextract_counts = {
        role: 0 for role, _path, _mode in NXEXTRACT_GENERATION_CORE
    }
    nxextract_extra_count = 0
    previous_key = None
    for index, entry in enumerate(entries):
        label = "generation_runtime[%d]" % index
        if not isinstance(entry, dict) or set(entry) != {
                "role", "path", "mode", "sha256"}:
            raise ManifestError(
                "%s requires exactly role, path, mode and sha256" % label)
        role = require_string(entry, "role")
        if role not in GENERATION_RUNTIME_ROLE_ORDER:
            raise ManifestError("%s.role is invalid" % label)
        path = require_relative(require_string(entry, "path"),
                                "%s.path" % label)
        if path in seen_paths:
            raise ManifestError("generation_runtime contains duplicate paths")
        seen_paths.add(path)
        if (path in ("NEXTOSCONTROLLERS.gptk", "NEXTOSSETTINGS.txt") or
                path == "gamedata" or path.startswith("gamedata/") or
                path == "saves" or path.startswith("saves/")):
            raise ManifestError(
                "%s.path is owner data and cannot enter a generation" % label)
        mode = require_string(entry, "mode")
        if mode not in ("0644", "0755"):
            raise ManifestError("%s.mode must be 0644 or 0755" % label)
        digest = require_string(entry, "sha256")
        if not SHA256_RE.fullmatch(digest):
            raise ManifestError("%s.sha256 must be lowercase SHA-256" % label)
        order_key = (GENERATION_RUNTIME_ROLE_ORDER[role], path)
        if previous_key is not None and order_key <= previous_key:
            raise ManifestError(
                "generation_runtime must be in canonical role/path order")
        previous_key = order_key
        nxextract_tree_path = path.startswith("nxextract/")
        nxextract_private_library = (
            role == "private-library" and nxextract_tree_path
        )
        if (path == "extractor.json" or path == "nxextract" or
                nxextract_tree_path) and not (
                    role.startswith("nxextract-") or
                    nxextract_private_library):
            raise ManifestError(
                "%s.path is reserved for an NXExtract generation role" % label)
        if role.startswith("nxextract-") and not (
                path == "extractor.json" or path.startswith("nxextract/")):
            raise ManifestError(
                "%s.path must belong to the NXExtract runtime tree" % label)
        if role == "executable":
            executable_count += 1
            if path != executable or mode != "0755":
                raise ManifestError(
                    "generation executable must match nxport.executable at 0755")
        elif role == "private-library":
            if not any(_path_below(path, root) for root in libraries):
                raise ManifestError(
                    "generation private-library must be below private_library_paths")
            if nxextract_private_library:
                if mode != "0644":
                    raise ManifestError(
                        "NXExtract private-library mode must be 0644")
                if nxextract_mode == "no":
                    raise ManifestError(
                        "nxextract.mode=no forbids private libraries below nxextract")
                if path in NXEXTRACT_GENERATION_CORE_PATHS:
                    raise ManifestError(
                        "generation private-library cannot replace a canonical "
                        "NXExtract component")
        elif role == "runtime-data":
            if mode != "0644":
                raise ManifestError("generation runtime-data mode must be 0644")
            if not any(_path_below(path, root) for root in libraries):
                raise ManifestError(
                    "generation runtime-data must be below private_library_paths")
        elif role == "runtime-hook":
            declared_hook = (
                (path == "port-env.sh" and path in required) or
                (path == "adapter-env.sh" and
                 data.get("owner_runtime") == "1") or
                (prepare and path == prepare and path in required)
            )
            if not declared_hook:
                raise ManifestError(
                    "generation runtime-hook must be declared port-env/prepare")
        elif role in NXEXTRACT_GENERATION_CORE_BY_ROLE:
            expected_path, expected_mode = \
                NXEXTRACT_GENERATION_CORE_BY_ROLE[role]
            nxextract_counts[role] += 1
            if path != expected_path or mode != expected_mode:
                raise ManifestError(
                    "%s must be %s at mode %s" %
                    (role, expected_path, expected_mode))
        elif role in NXEXTRACT_GENERATION_EXTRA_ROLES:
            nxextract_extra_count += 1
            if path in NXEXTRACT_GENERATION_CORE_PATHS:
                raise ManifestError(
                    "%s cannot replace a canonical NXExtract component" % role)
            if role == "nxextract-spec" and mode != "0644":
                raise ManifestError("generation nxextract-spec mode must be 0644")
        else:
            splash_count += 1
        normalized.append({
            "role": role, "path": path, "mode": mode, "sha256": digest,
        })
    if executable_count != 1:
        raise ManifestError(
            "generation_runtime requires exactly one executable")
    nxextract_core_present = sum(nxextract_counts.values())
    if nxextract_mode == "no":
        if nxextract_core_present or nxextract_extra_count:
            raise ManifestError(
                "nxextract.mode=no forbids NXExtract generation members")
    else:
        missing = [
            role for role, count in nxextract_counts.items() if count != 1
        ]
        if missing:
            raise ManifestError(
                "NXExtract generation closure requires exactly one of: %s" %
                ", ".join(missing))

    _splash_source, splash_sha256 = nxsplash_artifact(splash_architecture)
    canonical_splash = {
        "role": "nxsplash",
        "path": NXSPLASH_RUNTIME_NAME,
        "mode": "0755",
        "sha256": splash_sha256,
    }
    if splash_count == 0:
        normalized.append(canonical_splash)
    elif splash_count != 1 or normalized[-1] != canonical_splash:
        raise ManifestError(
            "generation nxsplash member must match the canonical release artifact")
    return normalized


def validate_language(data):
    if "language" not in data or data["language"] is None:
        return None
    language = require_object(data, "language")
    unknown = sorted(set(language) - {"default", "supported"})
    if unknown:
        raise ManifestError("language has unknown field(s): %s" %
                            ", ".join(unknown))
    if set(language) != {"default", "supported"}:
        raise ManifestError("language requires default and supported")
    default = require_string(language, "default")
    supported = string_list(language, "supported")
    if not supported:
        raise ManifestError("language.supported must not be empty")
    if len(supported) > 32:
        raise ManifestError("language.supported has more than 32 entries")
    for code in supported:
        if not LANGUAGE_CODE_RE.fullmatch(code):
            raise ManifestError("language.supported contains an invalid code: %s" %
                                code)
    if default != "auto" and default not in supported:
        raise ManifestError(
            "language.default must be auto or one of language.supported"
        )
    return {"default": default, "supported": supported}


def validate_options(data):
    """Opcoes declarativas do launcher: id, valores, padrao e variavel.

    Contrato ADITIVO. Um manifesto sem `options` gera exatamente o launcher
    de antes, byte a byte. O campo `language` continua existindo com a sua
    propria forma e semantica -- opcoes nao o substituem nem o duplicam.
    """
    if "options" not in data or data["options"] is None:
        return []
    value = data["options"]
    if not isinstance(value, list):
        raise ManifestError("options must be a list")
    if len(value) > 16:
        raise ManifestError("options must contain at most 16 entries")
    options = []
    identifiers = set()
    environments = set()
    for index, entry in enumerate(value):
        label_context = "options[%d]" % index
        if not isinstance(entry, dict):
            raise ManifestError("%s must be an object" % label_context)
        allowed = {"id", "values", "default", "environment", "label"}
        unknown = sorted(set(entry) - allowed)
        if unknown:
            raise ManifestError("%s has unknown field(s): %s" %
                                (label_context, ", ".join(unknown)))
        if not {"id", "values", "default", "environment"} <= set(entry):
            raise ManifestError(
                "%s requires id, values, default and environment"
                % label_context
            )
        option_id = require_string(entry, "id")
        if not OPTION_ID_RE.fullmatch(option_id):
            raise ManifestError("%s.id is invalid: %s" %
                                (label_context, option_id))
        if option_id in identifiers:
            raise ManifestError("duplicated option id: %s" % option_id)
        identifiers.add(option_id)

        values = string_list(entry, "values")
        if len(values) < 2:
            raise ManifestError("%s.values needs at least two values"
                                % label_context)
        if len(values) > 32:
            raise ManifestError("%s.values has more than 32 entries"
                                % label_context)
        if len(set(values)) != len(values):
            raise ManifestError("%s.values repeats a value" % label_context)
        for item in values:
            if not OPTION_VALUE_RE.fullmatch(item):
                raise ManifestError("%s.values has an unsafe value: %s" %
                                    (label_context, item))
        default = require_string(entry, "default")
        if default not in values:
            raise ManifestError("%s.default must be one of values"
                                % label_context)

        environment = require_string(entry, "environment")
        if not OPTION_ENVIRONMENT_RE.fullmatch(environment):
            raise ManifestError("%s.environment is invalid: %s" %
                                (label_context, environment))
        if environment in OPTION_RESERVED_NAMES or environment.startswith(
                OPTION_RESERVED_PREFIXES):
            raise ManifestError(
                "%s.environment is reserved by the framework: %s"
                % (label_context, environment)
            )
        if environment in environments:
            raise ManifestError("duplicated option environment: %s"
                                % environment)
        environments.add(environment)

        label = entry.get("label", option_id)
        label = require_string({"label": label}, "label")
        if not OPTION_LABEL_RE.fullmatch(label):
            raise ManifestError("%s.label is invalid" % label_context)

        suffix = option_id.upper().replace("-", "_")
        options.append({
            "id": option_id,
            "label": label,
            "values": values,
            "default": default,
            "environment": environment,
            "shell_suffix": suffix,
            "shell_variable": "GAME_OPTION_" + suffix,
        })
    options.sort(key=lambda option: option["id"])
    return options


def validate_execution_role(value, label):
    if not isinstance(value, dict) or set(value) != EXECUTION_ROLE_KEYS:
        raise ManifestError(
            "%s requires architecture, executable, executor, interpreter "
            "and closure" % label
        )
    architecture = require_string(value, "architecture")
    if architecture not in ARCHITECTURES:
        raise ManifestError("%s has an unsupported architecture" % label)
    executable = require_relative(
        require_string(value, "executable"), "%s.executable" % label
    )
    executor = require_string(value, "executor")
    if executor not in ("native", "native-or-loader"):
        raise ManifestError(
            "%s.executor must be native or native-or-loader" % label
        )
    interpreter = require_string(value, "interpreter")
    if interpreter != ARCH_INTERPRETERS[architecture]:
        raise ManifestError(
            "%s.interpreter must be exactly %s for %s"
            % (label, ARCH_INTERPRETERS[architecture], architecture)
        )
    closure = require_string(value, "closure")
    if closure not in ("host", "firmware", "firmware-and-port"):
        raise ManifestError("%s has an invalid closure" % label)
    return {
        "architecture": architecture,
        "executable": executable,
        "executor": executor,
        "interpreter": interpreter,
        "closure": closure,
    }


def validate_execution_roles(data, architecture, executable, nxextract_mode):
    if "execution_roles" not in data:
        return None
    roles = require_object(data, "execution_roles")
    if set(roles) != {"extractor", "splash", "game", "helpers"}:
        raise ManifestError(
            "execution_roles requires extractor, splash, game and helpers"
        )
    if nxextract_mode == "auto":
        raise ManifestError(
            "execution_roles requires an explicit nxextract mode (yes or no)"
        )
    extractor_value = roles["extractor"]
    if nxextract_mode == "yes":
        extractor = validate_execution_role(
            extractor_value, "execution_roles.extractor"
        )
        if (
            extractor["executable"]
            not in ("nxextract/nxextract-ui", "nxextract-ui")
            or extractor["executor"] != "native"
            or extractor["closure"] != "host"
        ):
            raise ManifestError(
                "extractor role must describe the native host NXExtract UI"
            )
    else:
        if extractor_value is not None:
            raise ManifestError(
                "execution_roles.extractor must be null when NXExtract is disabled"
            )
        extractor = None

    splash = validate_execution_role(
        roles["splash"], "execution_roles.splash"
    )
    if (
        splash["executable"] != NXSPLASH_RUNTIME_NAME
        or splash["closure"] != "firmware"
    ):
        raise ManifestError(
            "splash role must use nxsplash-nextos with firmware closure"
        )

    game = validate_execution_role(roles["game"], "execution_roles.game")
    if (
        game["architecture"] != architecture
        or game["executable"] != executable
        or game["closure"] != "firmware-and-port"
    ):
        raise ManifestError(
            "game role must match architecture/executable and use "
            "firmware-and-port closure"
        )

    helpers_value = roles["helpers"]
    if not isinstance(helpers_value, list) or len(helpers_value) > 16:
        raise ManifestError("execution_roles.helpers must contain at most 16 roles")
    helpers = []
    helper_ids = set()
    executables = {
        executable,
        splash["executable"],
    }
    if extractor is not None:
        executables.add(extractor["executable"])
    for index, helper_value in enumerate(helpers_value):
        if not isinstance(helper_value, dict) or set(helper_value) != (
            EXECUTION_ROLE_KEYS | {"id"}
        ):
            raise ManifestError(
                "execution_roles.helpers[%d] has an invalid shape" % index
            )
        helper_id = require_string(helper_value, "id")
        if not ROLE_ID_RE.fullmatch(helper_id) or helper_id in helper_ids:
            raise ManifestError("execution helper id is invalid or duplicated")
        helper = validate_execution_role(
            {key: helper_value[key] for key in EXECUTION_ROLE_KEYS},
            "execution_roles.helpers[%d]" % index,
        )
        if helper["closure"] == "host":
            raise ManifestError("additional helpers cannot use the host closure")
        if helper["executable"] in executables:
            raise ManifestError("execution role executables must be unique")
        helper["id"] = helper_id
        helpers.append(helper)
        helper_ids.add(helper_id)
        executables.add(helper["executable"])
    helpers.sort(key=lambda item: item["id"])
    return {
        "extractor": extractor,
        "splash": splash,
        "game": game,
        "helpers": helpers,
    }


def validate(data):
    if CAPABILITY_REGISTRY_ERROR is not None:
        raise ManifestError(CAPABILITY_REGISTRY_ERROR)
    if not isinstance(data, dict):
        raise ManifestError("manifest root must be an object")
    input_schema_version = data.get("schema_version")
    if input_schema_version == 1:
        data = normalize_legacy_v1(data)
    elif input_schema_version not in (2, CURRENT_SCHEMA_VERSION):
        raise ManifestError(
            "schema_version must be %s (legacy input supported: %s)" %
            (CURRENT_SCHEMA_VERSION, ", ".join(
                str(item) for item in LEGACY_SCHEMA_VERSIONS))
        )
    normalized_schema_version = data["schema_version"]
    if normalized_schema_version == 3 and "generation_runtime" not in data:
        raise ManifestError(
            "schema_version 3 requires generation_runtime")
    known_keys = (KNOWN_KEYS_V3 if normalized_schema_version == 3
                  else KNOWN_KEYS_V2)
    unknown = sorted(set(data) - known_keys)
    if unknown:
        raise ManifestError("unknown field(s): %s" % ", ".join(unknown))
    port_id = require_string(data, "id")
    if not re.match(r"^[a-z0-9][a-z0-9._-]{0,62}$", port_id):
        raise ManifestError("id must be a lowercase filesystem-safe identifier")
    title = require_string(data, "title")
    if len(title) > 128:
        raise ManifestError("title is longer than 128 characters")
    reject_personal_path(title, "title")
    launcher = require_string(data, "launcher_name")
    if len(launcher) > 160:
        raise ManifestError("launcher_name is longer than 160 characters")
    if Path(launcher).name != launcher or not launcher.endswith(".sh"):
        raise ManifestError("launcher_name must be a basename ending in .sh")
    architecture = require_string(data, "architecture")
    if architecture not in ARCHITECTURES:
        raise ManifestError("unsupported architecture: %s" % architecture)
    executable = require_relative(require_string(data, "executable"), "executable")
    if executable == NXSPLASH_RUNTIME_NAME:
        raise ManifestError("executable collides with the framework splash helper")
    argument_mode = data.get("argument_mode", "game-dir-and-passthrough")
    if argument_mode not in ("none", "passthrough", "game-dir",
                             "game-dir-and-passthrough"):
        raise ManifestError("invalid argument_mode")
    nxextract_config = require_object(data, "nxextract")
    unknown_nxextract = sorted(set(nxextract_config) - {"mode", "version"})
    if unknown_nxextract:
        raise ManifestError("nxextract has unknown field(s): %s" %
                            ", ".join(unknown_nxextract))
    if set(nxextract_config) != {"mode", "version"}:
        raise ManifestError("nxextract requires mode and version")
    nxextract = require_string(nxextract_config, "mode")
    if nxextract not in ("auto", "yes", "no"):
        raise ManifestError("nxextract.mode must be auto, yes or no")
    nxextract_version = require_string(nxextract_config, "version")
    if nxextract_version not in NXEXTRACT_SUPPORTED_VERSIONS:
        raise ManifestError(
            "nxextract.version %s is not supported (canonical %s; supported: %s)"
            % (nxextract_version, NXEXTRACT_VERSION,
               ", ".join(NXEXTRACT_SUPPORTED_VERSIONS)))
    home_mode = data.get("home_mode", "preserve")
    if home_mode not in ("preserve", "port"):
        raise ManifestError("home_mode must be preserve or port")
    required = string_list(data, "required_files", paths=True)
    if executable not in required:
        required.insert(0, executable)
    required = [item for item in required if item != NXSPLASH_RUNTIME_NAME]
    required.insert(1, NXSPLASH_RUNTIME_NAME)
    libraries = string_list(data, "private_library_paths", paths=True)
    prepare = (require_string(data, "prepare_script", allow_empty=True)
               if "prepare_script" in data else "")
    require_relative(prepare, "prepare_script", allow_empty=True)
    capabilities = named_list(data, "required_capabilities", CAPABILITY_RE,
                              CAPABILITY_IDS)
    capabilities.sort(key=CAPABILITY_ORDER.__getitem__)
    quirks = named_list(data, "enabled_quirks", QUIRK_RE, QUIRK_IDS)
    language = validate_language(data)
    options = validate_options(data)
    runtime_report = data.get("runtime_report", "log-and-logo")
    if runtime_report not in ("log", "log-and-logo"):
        raise ManifestError("runtime_report must be log or log-and-logo")
    sdl_provider = data.get("sdl_provider")
    if "sdl_provider" in data and sdl_provider != "system":
        raise ManifestError("sdl_provider must be system when declared")
    video_proof = data.get("video_proof")
    if "video_proof" in data and video_proof != "required":
        raise ManifestError("video_proof must be required when declared")
    owner_runtime = data.get("owner_runtime")
    if "owner_runtime" in data and owner_runtime != "1":
        raise ManifestError("owner_runtime must be \"1\" when declared")
    if owner_runtime == "1" and "generation_runtime" not in data:
        raise ManifestError("owner_runtime requires generation_runtime (V4 store)")
    if owner_runtime == "1" and prepare == "port-env.sh":
        raise ManifestError("owner_runtime: port-env.sh is owner-native and cannot be the prepare script")
    execution_roles = validate_execution_roles(
        data, architecture, executable, nxextract
    )
    if execution_roles is not None:
        for helper in execution_roles["helpers"]:
            if helper["executable"] not in required:
                required.append(helper["executable"])
    splash_architecture = (
        execution_roles["splash"]["architecture"]
        if execution_roles is not None else architecture
    )
    generation_runtime = validate_generation_runtime(
        data, executable, required, libraries, prepare, splash_architecture,
        nxextract
    )
    if owner_runtime == "1":
        for entry in generation_runtime or []:
            if entry["path"] == "port-env.sh":
                raise ManifestError(
                    "owner_runtime: port-env.sh is an owner-native live file and "
                    "cannot enter a generation (seal adapter logic in adapter-env.sh)")
        if "port-env.sh" in required:
            raise ManifestError(
                "owner_runtime: port-env.sh is owner-native and cannot be a required file")
    return {
        "schema_version": normalized_schema_version,
        "input_schema_version": input_schema_version,
        "id": port_id,
        "title": title,
        "launcher_name": launcher,
        "architecture": architecture,
        "executable": executable,
        "argument_mode": argument_mode,
        "nxextract": {
            "mode": nxextract,
            "version": nxextract_version,
        },
        "home_mode": home_mode,
        "required_files": required,
        "private_library_paths": libraries,
        "prepare_script": prepare,
        "required_capabilities": capabilities,
        "enabled_quirks": quirks,
        "language": language,
        "options": options,
        "runtime_report": runtime_report,
        "sdl_provider": sdl_provider,
        "video_proof": video_proof,
        "owner_runtime": owner_runtime,
        "execution_roles": execution_roles,
        "generation_runtime": generation_runtime,
    }


def render_nxextract_validator(config):
    if config["nxextract"]["mode"] == "no":
        return ""
    return r'''
nxbootstrap_validate_nxextract_result() {
  python3 -B - "$1" "$2" <<'PY'
import json
import os
import re
import stat
import sys

sys.tracebacklimit = 0
path, process_status_text = sys.argv[1:]
process_status = int(process_status_text)
# Members born WITH schema_version 1 are required. Members added LATER in the
# same schema_version (ui, and any future addition) are validated only when
# present -- BOTH directions of a partially updated install keep working:
# old launcher + new engine (extra member ignored there) and new launcher +
# old engine (missing member tolerated here).
top_keys = {
    "schema", "schema_version", "nxextract_version", "outcome", "code",
    "final_phase", "recipe", "package_id", "abi", "container",
    "validated", "logs", "duration_ms", "completed_unix", "error",
}
phase_ids = (
    "preparing", "scanning", "validating-packages", "selecting",
    "extracting", "processing", "validating-data", "installing", "ready",
)
sha256_re = re.compile(r"^[0-9a-f]{64}$")
safe_id_re = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")

def reject(message):
    raise ValueError(message)

def pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            reject("duplicate JSON member")
        result[key] = value
    return result

def finite_constant(_value):
    reject("non-finite JSON number")

def exact_object(value, keys, label):
    if not isinstance(value, dict) or set(value) != set(keys):
        reject("invalid " + label + " object")


def require_members(value, keys, label):
    # 0.6.26: FORWARD-COMPATIBLE. Every known member must be present and is
    # validated below, but EXTRA members added by a newer engine are ignored
    # instead of rejected -- adding a field is a compatible change. The real
    # compatibility contract is schema + schema_version, checked right after.
    if not isinstance(value, dict) or not set(keys) <= set(value):
        reject("invalid " + label + " object")

def integer(value, label):
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        reject("invalid " + label)

def string(value, minimum, maximum, label):
    if not isinstance(value, str) or not minimum <= len(value) <= maximum:
        reject("invalid " + label)
    if any(ord(char) < 32 for char in value):
        reject("control byte in " + label)

def relative_path(value, label):
    string(value, 1, 512, label)
    if value.startswith("/") or "\\" in value or ".." in value.split("/"):
        reject("invalid " + label)

flags = (os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) |
         getattr(os, "O_NOFOLLOW", 0))
fd = os.open(path, flags)
try:
    opened = os.fstat(fd)
    linked = os.lstat(path)
    if (not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1 or
            (opened.st_dev, opened.st_ino) != (linked.st_dev, linked.st_ino)):
        reject("unsafe result identity")
    chunks = []
    total = 0
    while True:
        block = os.read(fd, 65536)
        if not block:
            break
        total += len(block)
        if total > 1048576:
            reject("result exceeds size limit")
        chunks.append(block)
finally:
    os.close(fd)

document = json.loads(
    b"".join(chunks).decode("utf-8", "strict"),
    object_pairs_hook=pairs,
    parse_constant=finite_constant,
)
require_members(document, top_keys, "terminal result")
# The compatibility contract is the schema id + schema_version ONLY. A newer
# engine (any nxextract_version) that keeps schema_version 1 stays readable by
# this launcher -- so a partially-updated install (new engine under an older
# launcher .sh) no longer dies with "unknown terminal result schema". A real
# breaking change would bump schema_version, which this gate still rejects.
if (document["schema"] != "org.nextos.nxextract.terminal-result" or
        isinstance(document["schema_version"], bool) or
        document["schema_version"] != 1 or
        not isinstance(document["nxextract_version"], str) or
        re.fullmatch(r"[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}",
                     document["nxextract_version"]) is None):
    reject("unknown terminal result schema")
if document["outcome"] not in ("success", "error"):
    reject("invalid terminal outcome")
if (process_status == 0) != (document["outcome"] == "success"):
    reject("process status and terminal outcome disagree")
if (not isinstance(document["code"], str) or
        not re.fullmatch(r"^NXE[0-9]{4}$", document["code"])):
    reject("invalid terminal code")

phase = document["final_phase"]
exact_object(phase, ("index", "id", "label"), "final phase")
integer(phase["index"], "phase index")
if phase["index"] > 8 or phase["id"] != phase_ids[phase["index"]]:
    reject("invalid final phase")
string(phase["label"], 1, 64, "phase label")

recipe = document["recipe"]
exact_object(recipe, ("id", "version", "digest"), "recipe")
if (not isinstance(recipe["id"], str) or
        not safe_id_re.fullmatch(recipe["id"])):
    reject("invalid recipe id")
string(recipe["version"], 1, 128, "recipe version")
if (not isinstance(recipe["digest"], str) or
        not sha256_re.fullmatch(recipe["digest"])):
    reject("invalid recipe digest")

if document["package_id"] is not None:
    string(document["package_id"], 1, 255, "package id")
abi = document["abi"]
if (abi is not None and
        (not isinstance(abi, str) or
         not re.fullmatch(r"^[A-Za-z0-9._-]{1,64}$", abi))):
    reject("invalid ABI")
container = document["container"]
exact_object(container, ("kind", "identity"), "container")
if container["kind"] not in (
        "apk-set", "bundle", "companion", "existing", None):
    reject("invalid container kind")
identity = container["identity"]
if (identity is not None and
        (not isinstance(identity, str) or not sha256_re.fullmatch(identity))):
    reject("invalid container identity")

validated = document["validated"]
exact_object(validated, ("items", "bytes", "critical_payloads"), "validated")
integer(validated["items"], "validated item count")
integer(validated["bytes"], "validated byte count")
critical = validated["critical_payloads"]
if not isinstance(critical, list) or len(critical) > 256:
    reject("invalid critical payload list")
for item in critical:
    exact_object(item, ("id", "items", "bytes"), "critical payload")
    if (not isinstance(item["id"], str) or
            not safe_id_re.fullmatch(item["id"])):
        reject("invalid critical payload id")
    integer(item["items"], "critical payload item count")
    integer(item["bytes"], "critical payload byte count")

logs = document["logs"]
exact_object(logs, ("summary", "detail"), "logs")
relative_path(logs["summary"], "summary log")
relative_path(logs["detail"], "detail log")
integer(document["duration_ms"], "duration")
integer(document["completed_unix"], "completion time")

if "ui" in document:
    ui = document["ui"]
    exact_object(ui, ("mode", "renderer", "fallback_reason"), "ui")
    if ui["mode"] not in ("visible", "headless-fallback", "disabled"):
        reject("invalid ui mode")
    if ui["renderer"] is not None:
        string(ui["renderer"], 1, 32, "ui renderer")
    if ui["fallback_reason"] is not None:
        string(ui["fallback_reason"], 1, 512, "ui fallback reason")
    if (ui["mode"] == "headless-fallback") != (
            ui["fallback_reason"] is not None):
        reject("ui mode and fallback reason disagree")

error = document["error"]
if document["outcome"] == "success":
    if error is not None:
        reject("success result contains an error")
else:
    exact_object(error, ("class", "message"), "error")
    if (not isinstance(error["class"], str) or
            not re.fullmatch(r"^[A-Za-z][A-Za-z0-9_]{0,127}$",
                             error["class"])):
        reject("invalid error class")
    string(error["message"], 1, 512, "error message")

print(json.dumps(document, ensure_ascii=True, sort_keys=True,
                 separators=(",", ":")))
PY
}
'''


def render_nxextract_block(config):
    if config["nxextract"]["mode"] == "no":
        block = "# NXExtract: disabled for this port (nxextract.mode=no)."
    else:
        requested = "1" if config["nxextract"]["mode"] == "yes" else "0"
        auto_probe = ""
        if config["nxextract"]["mode"] == "auto":
            auto_probe = """
for nxprobe in "$GAMEDIR/extractor.json" \\
  "$GAMEDIR/nxextract/run-extractor.sh" "$GAMEDIR/nxextract/nxextract.py" \\
  "$GAMEDIR/nxextract/nxextract-runtime-env.sh" \\
  "$GAMEDIR/nxextract/nxextract-ui" \\
  "$GAMEDIR/run-extractor.sh" "$GAMEDIR/nxextract.py" \\
  "$GAMEDIR/nxextract-runtime-env.sh" "$GAMEDIR/nxextract-ui"; do
  [ -e "$nxprobe" ] || [ -L "$nxprobe" ] || continue
  NXEXTRACT_REQUESTED=1
  break
done
unset nxprobe"""
        block = """\

# NXExtract owner-data phase (BYO data): runs before the game, other process.
NXDIR=""
[ -f "$GAMEDIR/nxextract/run-extractor.sh" ] && NXDIR="$GAMEDIR/nxextract"
[ -z "$NXDIR" ] && [ -f "$GAMEDIR/run-extractor.sh" ] && NXDIR="$GAMEDIR"
NXEXTRACT_REQUESTED=%s%s
if [ "$NXEXTRACT_REQUESTED" = 1 ]; then
  nxbootstrap_phase_event nxextract START bootstrap 6200 || {
    nxbootstrap_finish
    exit 1
  }
  NXEXTRACT_INCOMPLETE=0
  [ -n "$NXDIR" ] && [ ! -L "$NXDIR" ] || NXEXTRACT_INCOMPLETE=1
  for nxfile in "$GAMEDIR/extractor.json" "$NXDIR/run-extractor.sh" \\
    "$NXDIR/nxextract.py" "$NXDIR/nxextract-runtime-env.sh" \\
    "$NXDIR/nxextract-ui"; do
    [ -f "$nxfile" ] && [ -s "$nxfile" ] && [ ! -L "$nxfile" ] || NXEXTRACT_INCOMPLETE=1
  done
  if [ "$NXEXTRACT_INCOMPLETE" = 1 ]; then
    echo "ERROR: incomplete NXExtract integration"
    nxbootstrap_phase_event nxextract ERROR bootstrap 6202 || true
    nxbootstrap_finish
    exit 1
  fi
  $ESUDO chmod +x "$NXDIR/nxextract-ui" 2>/dev/null || true
  if [ ! -x "$NXDIR/nxextract-ui" ]; then
    echo "ERROR: mandatory NXExtract UI is not executable"
    nxbootstrap_phase_event nxextract ERROR bootstrap 6202 || true
    nxbootstrap_finish
    exit 1
  fi
  NXEXTRACT_RESULT_FILE="$GAMEDIR/nxextract-result.json"
  if [ -e "$NXEXTRACT_RESULT_FILE" ] || [ -L "$NXEXTRACT_RESULT_FILE" ]; then
    if [ ! -f "$NXEXTRACT_RESULT_FILE" ] || [ -L "$NXEXTRACT_RESULT_FILE" ]; then
      echo "ERROR: stale NXExtract result path is unsafe"
      nxbootstrap_phase_event nxextract ERROR bootstrap 6202 || true
      nxbootstrap_finish
      exit 1
    fi
    mv -f "$NXEXTRACT_RESULT_FILE" \\
      "$GAMEDIR/nxextract-result.prev.json" || {
        echo "ERROR: could not rotate the previous NXExtract result"
        nxbootstrap_phase_event nxextract ERROR bootstrap 6202 || true
        nxbootstrap_finish
        exit 1
      }
  fi
  # The firmware python3 must start: a corrupted stdlib bytecode cache (muOS,
  # "bad marshal data" in encodings) kills it before NXExtract sees the APK.
  # Retry with a private cache, then fail loudly instead of "result malformed".
  NXEXTRACT_PY_PROBE='import encodings, json, zipfile, hashlib'
  if ! python3 -B -c "$NXEXTRACT_PY_PROBE" >/dev/null 2>&1; then
    if PYTHONPYCACHEPREFIX="$GAMEDIR/.nxpycache" python3 -B -c "$NXEXTRACT_PY_PROBE" >/dev/null 2>&1; then
      export PYTHONPYCACHEPREFIX="$GAMEDIR/.nxpycache"
      echo "WARN: system python bytecode cache is broken; using $GAMEDIR/.nxpycache"
    else
      echo "ERROR: the firmware's python3 cannot start (corrupted stdlib/bytecode);"
      echo "ERROR: NXExtract needs a working python3 -- reinstall/update the CFW."
      nxbootstrap_phase_event nxextract ERROR bootstrap 6209 || true
      nxbootstrap_finish
      exit 1
    fi
  fi
  NXEXTRACT_STATUS=0
  NXEXTRACT_GAME_DIR="$GAMEDIR" \\
    bash "$NXDIR/nxextract-runtime-env.sh" \\
    bash "$NXDIR/run-extractor.sh" || NXEXTRACT_STATUS=$?
  NXEXTRACT_RESULT_STATUS=0
  NXEXTRACT_SUMMARY=""
  if [ -f "$NXEXTRACT_RESULT_FILE" ] && [ ! -L "$NXEXTRACT_RESULT_FILE" ]; then
    if NXEXTRACT_SUMMARY=$(nxbootstrap_validate_nxextract_result \\
      "$NXEXTRACT_RESULT_FILE" "$NXEXTRACT_STATUS"); then
      :
    else
      NXEXTRACT_RESULT_STATUS=$?
    fi
  else
    NXEXTRACT_RESULT_STATUS=1
  fi
  if [ "$NXEXTRACT_RESULT_STATUS" -ne 0 ]; then
    echo "ERROR: NXExtract terminal result is missing, unsafe or malformed"
    nxbootstrap_phase_event nxextract ERROR bootstrap 6202 \\
      "$NXEXTRACT_STATUS" || true
    nxbootstrap_finish
    if [ "$NXEXTRACT_STATUS" -ne 0 ]; then exit "$NXEXTRACT_STATUS"; fi
    exit 1
  fi
  printf 'NXEXTRACT_RESULT %%s\\n' "$NXEXTRACT_SUMMARY"
  if [ "$NXEXTRACT_STATUS" -ne 0 ]; then
    echo "ERROR: game data installation did not complete"
    nxbootstrap_phase_event nxextract ERROR extractor 6203 \\
      "$NXEXTRACT_STATUS" || true
    nxbootstrap_finish
    exit "$NXEXTRACT_STATUS"
  fi
  nxbootstrap_phase_event nxextract OK extractor 6201 || {
    nxbootstrap_finish
    exit 1
  }
fi
unset NXDIR NXEXTRACT_REQUESTED NXEXTRACT_INCOMPLETE nxfile \\
  NXEXTRACT_RESULT_FILE NXEXTRACT_STATUS NXEXTRACT_RESULT_STATUS \\
  NXEXTRACT_SUMMARY""" % (
            requested, auto_probe)
    if config["prepare_script"]:
        block += """

bash "$GAMEDIR/%s" || {
  echo "ERROR: prepare script failed"
  nxbootstrap_finish
  exit 1
}""" % config["prepare_script"]
    return block


def render_required_files_block(config):
    return """\

# Manifest-owned payload gate: extraction/prepare must finish before launch.
NXBOOTSTRAP_REQUIRED_FILES=%s
while IFS= read -r required_file; do
  [ -n "$required_file" ] || continue
  required_path=$(readlink -f "$GAMEDIR/$required_file" 2>/dev/null) || required_path=""
  case "$required_path" in
    "$GAMEDIR"/*) ;;
    *) required_path="" ;;
  esac
  if [ -z "$required_path" ] || [ ! -f "$required_path" ] || \\
     [ ! -s "$required_path" ] || [ -L "$GAMEDIR/$required_file" ]; then
    echo "ERROR: required file is missing or unsafe: $required_file"
    nxbootstrap_finish
    exit 1
  fi
done <<< "$NXBOOTSTRAP_REQUIRED_FILES"
unset NXBOOTSTRAP_REQUIRED_FILES required_file required_path""" % shell_join(
        config["required_files"])


def render_home_block(config):
    if config["home_mode"] == "port":
        return ('# Saves and configuration stay inside the port directory.\n'
                'export HOME="$GAMEDIR"')
    return "# HOME preserved (home_mode=preserve)."


def shell_game_path(path):
    return '"$GAMEDIR"/%s' % shlex.quote(path)


def render_interpreter_routing_block(config):
    if config.get("execution_roles") is not None:
        return "# Per-role resolver below owns every interpreter decision.\nNXBOOTSTRAP_INTERP_PREFIX=\"\""
    return r'''# Dynamic-loader (PT_INTERP) preflight: an ELF whose interpreter is absent
# exec()s with ENOENT, which the shell reports as "No such file or directory"
# for the binary and a bare status 127 -- a black screen that looks like a
# broken port but is a missing device runtime. Textbook case: a 32-bit ARMHF
# port on a 64-bit-only CFW with no 32-bit ARM runtime (spruce/Miyoo Flip:
# /lib/ld-linux-armhf.so.3 absent). `grep -a` reads the early .interp; a static
# or unreadable ELF yields nothing and skips the check (never a false block).
# Prefix that runs the game THROUGH an explicit loader (empty for every normal
# port -- the run line then executes "$BIN" byte-identically). Only set below,
# for the spruce/Miyoo-Flip case where the 32-bit loader lives off the default
# path.
NXBOOTSTRAP_INTERP_PREFIX=""
NXBOOTSTRAP_INTERP=$(LC_ALL=C grep -a -o -m1 -E '/lib(32|64)?/ld-[A-Za-z0-9._-]*\.so[A-Za-z0-9._-]*' "$NXBOOTSTRAP_EXECUTABLE" 2>/dev/null | head -n1)
if [ -n "$NXBOOTSTRAP_INTERP" ] && [ ! -e "$NXBOOTSTRAP_INTERP" ]; then
  # The interpreter is absent at the exact path baked into the ELF. Before
  # giving up, look for the SAME loader elsewhere: some CFWs mount their 32-bit
  # runtime off the default path -- spruceOS/Miyoo Flip assembles a 32-bit chroot
  # at boot and copies ld-linux-armhf.so.3 under /usr/lib and /usr/lib32. If we
  # find it, run the game THROUGH that loader with an explicit --library-path
  # (additive: this branch is only reached when the game would otherwise abort).
  NXBOOTSTRAP_INTERP_BASE=$(basename "$NXBOOTSTRAP_INTERP")
  NXBOOTSTRAP_INTERP_ALT=""
  for cand in \
    "/usr/lib/$NXBOOTSTRAP_INTERP_BASE" \
    "/usr/lib32/$NXBOOTSTRAP_INTERP_BASE" \
    "/lib32/$NXBOOTSTRAP_INTERP_BASE" \
    "/mnt/SDCARD/spruce/flip/$NXBOOTSTRAP_INTERP_BASE" \
    "/mnt/SDCARD/spruce/flip/muOS/usr/lib/$NXBOOTSTRAP_INTERP_BASE" \
    "/mnt/SDCARD/Persistent/.32bit_chroot/usr/lib/$NXBOOTSTRAP_INTERP_BASE" \
    "/mnt/SDCARD/Persistent/.32bit_chroot/lib/$NXBOOTSTRAP_INTERP_BASE"; do
    [ -e "$cand" ] && { NXBOOTSTRAP_INTERP_ALT="$cand"; break; }
  done
  if [ -n "$NXBOOTSTRAP_INTERP_ALT" ]; then
    # Diretorios confirmados nas IMAGENS oficiais (spruce v4.3.4): o muOS
    # reduzido tem SDL2/EGL/GLES ARMHF em usr/lib (nao so lib32) e o chroot
    # 32-bit monta em /mnt/SDCARD/Persistent/.32bit_chroot.
    NXBOOTSTRAP_INTERP_LIBS="$GAMEDIR:$GAMEDIR/lib:/usr/lib32:/lib32:/mnt/SDCARD/spruce/flip/muOS/usr/lib:/mnt/SDCARD/spruce/flip/muOS/lib:/mnt/SDCARD/spruce/flip/muOS/lib32:/mnt/SDCARD/spruce/flip/muOS/usr/lib32:/mnt/SDCARD/Persistent/.32bit_chroot/usr/lib:/mnt/SDCARD/Persistent/.32bit_chroot/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    NXBOOTSTRAP_INTERP_PREFIX="$NXBOOTSTRAP_INTERP_ALT --library-path $NXBOOTSTRAP_INTERP_LIBS"
    echo "NOTE: interpreter $NXBOOTSTRAP_INTERP is absent at its default path, but $NXBOOTSTRAP_INTERP_ALT exists; launching %s through it."
  else
    echo "ERROR: %s cannot run here: dynamic loader $NXBOOTSTRAP_INTERP is missing."
    case "$NXBOOTSTRAP_INTERP" in
      *armhf*|*ld-linux.so.3) echo "  32-bit ARM (ARMHF) port, but this CFW has no 32-bit ARM runtime on any known path; use a CFW with 32-bit multiarch (Knulli/ROCKNIX), fix the 32-bit mount (spruce/Miyoo Flip), or use an AArch64 build." ;;
      *aarch64*|*ld-linux-aarch64*) echo "  64-bit ARM (AArch64) port, but this device has no 64-bit ARM runtime." ;;
      *) echo "  this CFW does not provide the runtime this port was built for." ;;
    esac
    nxbootstrap_phase_event preflight ERROR bootstrap 6198 || true
    nxbootstrap_finish
    exit 1
  fi
fi''' % (config["executable"], config["executable"])


def render_execution_roles_block(config):
    roles = config.get("execution_roles")
    if roles is None:
        return ""
    extractor = roles["extractor"]
    splash = roles["splash"]
    game = roles["game"]

    def resolve_call(role_name, role):
        elf_class, machine = ARCH_ELF_IDENTITIES[role["architecture"]]
        return (
            "nxbootstrap_resolve_execution_role %s %s %s %d %d %s %s %s"
            % (
                shlex.quote(role_name),
                shell_game_path(role["executable"]),
                shlex.quote(role["architecture"]),
                elf_class,
                machine,
                shlex.quote(role["interpreter"]),
                shlex.quote(role["executor"]),
                shlex.quote(role["closure"]),
            )
        )

    extractor_call = ""
    if extractor is not None:
        extractor_call = """
%s || {
  nxbootstrap_phase_event preflight ERROR bootstrap 6198 || true
  nxbootstrap_finish
  exit 1
}
NXBOOTSTRAP_EXTRACTOR_EXECUTOR=$NXBOOTSTRAP_ROUTE_EXECUTOR
NXBOOTSTRAP_EXTRACTOR_LOADER=$NXBOOTSTRAP_ROUTE_LOADER
NXBOOTSTRAP_EXTRACTOR_LIBS=$NXBOOTSTRAP_ROUTE_LIBS
""" % resolve_call("extractor", extractor)

    return r'''

# 0.6.27 opt-in execution roles. Live process state stays separate per role:
# an AArch64 extractor never inherits the ARMHF game closure, and splash/game
# resolve their own interpreter instead of borrowing one global prefix.
nxbootstrap_elf_identity() {
  # Status: 0 = ELF identificado (imprime classe:machine); 3 = o arquivo NAO e
  # ELF (script do GNU ld, texto, etc.); 1 = ELF ilegivel/truncado, ou host sem
  # od. Quem chama decide o que fazer com cada caso -- ignorar um script nao e
  # a mesma coisa que aceitar um ELF quebrado.
  local path=$1 bytes machine
  command -v od >/dev/null 2>&1 || return 1
  bytes=$(LC_ALL=C od -An -tu1 -N20 -- "$path" 2>/dev/null) || return 1
  set -- $bytes
  [ "$#" -ge 4 ] || return 3
  [ "$1" = 127 ] && [ "$2" = 69 ] && [ "$3" = 76 ] && [ "$4" = 70 ] || return 3
  [ "$#" -ge 20 ] || return 1
  case "$5:$6" in 1:1|2:1) ;; *) return 1 ;; esac
  machine=$(( ${19} + (${20} * 256) ))
  echo "$5:$machine"
}
nxbootstrap_check_execution_probe() {
  # 0 = biblioteca de runtime valida da ABI esperada (conta como raiz vista);
  # 3 = arquivo que nao e biblioteca de runtime (ignorar);
  # 2 = raiz contaminada: ELF de ABI errada ou ELF quebrado.
  local role=$1 root=$2 probe=$3 expected_class=$4 expected_machine=$5
  local identity status
  identity=$(nxbootstrap_elf_identity "$probe")
  status=$?
  if [ "$status" = 3 ]; then
    # Script de linker (GNU ld) e artefato de BUILD, nao de runtime. No
    # ArkOS/dArkOS e no Debian multiarch, /usr/lib/arm-linux-gnueabihf/libc.so
    # e exatamente isso, ao lado da libc.so.6 de verdade. Tratar o script como
    # ELF invalido descartava a closure ARMHF inteira e o port nem abria.
    echo "EXECUTION CANDIDATE SKIPPED: role=$role file=$probe reason=not-an-elf"
    return 3
  fi
  if [ "$status" != 0 ]; then
    echo "EXECUTION CANDIDATE REJECTED: role=$role root=$root reason=invalid-elf"
    return 2
  fi
  if [ "$identity" != "$expected_class:$expected_machine" ]; then
    echo "EXECUTION CANDIDATE REJECTED: role=$role root=$root reason=wrong-abi found=$identity"
    return 2
  fi
  return 0
}
nxbootstrap_add_execution_root() {
  local role=$1 root=$2 expected_class=$3 expected_machine=$4 policy=$5
  local probe status seen=0
  [ -d "$root" ] || return 1
  if [ "$policy" = port ]; then
    for probe in "$root"/*.so "$root"/*.so.*; do
      [ -f "$probe" ] || continue
      nxbootstrap_check_execution_probe "$role" "$root" "$probe" \
        "$expected_class" "$expected_machine"
      status=$?
      [ "$status" = 2 ] && return 2
      [ "$status" = 3 ] && continue
      seen=1
    done
  else
    for probe in "$root"/ld-linux*.so* "$root"/libc.so* \
      "$root"/libSDL2*.so* "$root"/libEGL.so* "$root"/libGLES*.so* \
      "$root"/libasound.so* "$root"/libpulse.so*; do
      [ -f "$probe" ] || continue
      nxbootstrap_check_execution_probe "$role" "$root" "$probe" \
        "$expected_class" "$expected_machine"
      status=$?
      [ "$status" = 2 ] && return 2
      [ "$status" = 3 ] && continue
      seen=1
    done
  fi
  [ "$policy" = port ] || [ "$seen" = 1 ] || return 1
  case ":$NXBOOTSTRAP_ROLE_LIBS:" in
    *:"$root":*) return 0 ;;
  esac
  NXBOOTSTRAP_ROLE_LIBS="${NXBOOTSTRAP_ROLE_LIBS:+$NXBOOTSTRAP_ROLE_LIBS:}$root"
}
nxbootstrap_build_execution_closure() {
  local role=$1 architecture=$2 expected_class=$3 expected_machine=$4
  local d
  NXBOOTSTRAP_ROLE_LIBS=""
  case "$architecture" in
    armv7)
      # The official Spruce profile proves these ARMHF roots and explicitly
      # excludes muOS/usr/lib, which is AArch64 in that image.
      for d in \
        /mnt/SDCARD/Persistent/.32bit_chroot/usr/lib32 \
        /mnt/SDCARD/Persistent/.32bit_chroot/usr/lib \
        /mnt/SDCARD/Persistent/.32bit_chroot/lib \
        /mnt/SDCARD/spruce/flip/muOS/usr/lib32 \
        /mnt/SDCARD/spruce/flip/muOS/lib32 \
        /usr/local/lib/arm-linux-gnueabihf /usr/local/lib32 \
        /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf \
        /usr/lib32 /lib32; do
        nxbootstrap_add_execution_root "$role" "$d" \
          "$expected_class" "$expected_machine" firmware || true
      done
      ;;
    aarch64)
      for d in /usr/local/lib/aarch64-linux-gnu /usr/local/lib64 \
        /usr/local/lib /usr/lib/aarch64-linux-gnu \
        /lib/aarch64-linux-gnu /usr/lib64 /lib64 /usr/lib /lib; do
        nxbootstrap_add_execution_root "$role" "$d" \
          "$expected_class" "$expected_machine" firmware || true
      done
      ;;
    i386)
      for d in /usr/local/lib/i386-linux-gnu /usr/local/lib32 \
        /usr/lib/i386-linux-gnu /lib/i386-linux-gnu /usr/lib32 /lib32; do
        nxbootstrap_add_execution_root "$role" "$d" \
          "$expected_class" "$expected_machine" firmware || true
      done
      ;;
    x86_64)
      for d in /usr/local/lib/x86_64-linux-gnu /usr/local/lib64 \
        /usr/local/lib /usr/lib/x86_64-linux-gnu \
        /lib/x86_64-linux-gnu /usr/lib64 /lib64 /usr/lib /lib; do
        nxbootstrap_add_execution_root "$role" "$d" \
          "$expected_class" "$expected_machine" firmware || true
      done
      ;;
    *) return 1 ;;
  esac
  if [ -n "${controlfolder:-}" ]; then
    case "$architecture" in
      armv7) d="$controlfolder/libs.armhf" ;;
      aarch64) d="$controlfolder/libs.aarch64" ;;
      i386) d="$controlfolder/libs.x86" ;;
      x86_64) d="$controlfolder/libs.x86_64" ;;
    esac
    nxbootstrap_add_execution_root "$role" "$d" \
      "$expected_class" "$expected_machine" firmware || true
    nxbootstrap_add_execution_root "$role" "$controlfolder/libs" \
      "$expected_class" "$expected_machine" firmware || true
  fi
  [ -n "$NXBOOTSTRAP_ROLE_LIBS" ]
}
nxbootstrap_select_execution_loader() {
  local role=$1 expected_class=$2 expected_machine=$3 candidate identity
  shift 3
  NXBOOTSTRAP_ROUTE_LOADER=""
  for candidate in "$@"; do
    [ -f "$candidate" ] && [ -s "$candidate" ] && [ -x "$candidate" ] || continue
    identity=$(nxbootstrap_elf_identity "$candidate") || {
      echo "EXECUTION CANDIDATE REJECTED: role=$role loader=$candidate reason=invalid-elf"
      continue
    }
    if [ "$identity" != "$expected_class:$expected_machine" ]; then
      echo "EXECUTION CANDIDATE REJECTED: role=$role loader=$candidate reason=wrong-abi found=$identity"
      continue
    fi
    NXBOOTSTRAP_ROUTE_LOADER=$candidate
    return 0
  done
  return 1
}
nxbootstrap_resolve_execution_role() {
  local role=$1 executable=$2 architecture=$3 expected_class=$4
  local expected_machine=$5 expected_interp=$6 policy=$7 closure=$8
  local identity actual_interp interp_base candidate reason executor
  NXBOOTSTRAP_ROUTE_EXECUTOR=""
  NXBOOTSTRAP_ROUTE_LOADER=""
  NXBOOTSTRAP_ROUTE_LIBS=""
  if [ ! -f "$executable" ] || [ ! -s "$executable" ] || [ -L "$executable" ]; then
    echo "ERROR: execution role $role has a missing or unsafe executable"
    return 1
  fi
  identity=$(nxbootstrap_elf_identity "$executable") || {
    echo "ERROR: execution role $role is not a supported little-endian ELF"
    return 1
  }
  if [ "$identity" != "$expected_class:$expected_machine" ]; then
    echo "ERROR: execution role $role has wrong ABI: $identity"
    return 1
  fi
  actual_interp=$(LC_ALL=C grep -a -o -m1 -E \
    '/lib(32|64)?/ld-[A-Za-z0-9._-]*\.so[A-Za-z0-9._-]*' \
    "$executable" 2>/dev/null | head -n1)
  if [ "$actual_interp" != "$expected_interp" ]; then
    echo "ERROR: execution role $role interpreter differs from its contract"
    return 1
  fi
  if [ "$closure" = host ]; then
    NXBOOTSTRAP_ROUTE_LIBS=${LD_LIBRARY_PATH:-}
  else
    nxbootstrap_build_execution_closure "$role" "$architecture" \
      "$expected_class" "$expected_machine" || {
        echo "ERROR: execution role $role has no coherent $architecture closure"
        return 1
      }
    NXBOOTSTRAP_ROUTE_LIBS=$NXBOOTSTRAP_ROLE_LIBS
  fi
  if [ -e "$expected_interp" ]; then
    executor=native
    reason=interpreter-present
  else
    if [ "$policy" = native ]; then
      echo "ERROR: execution role $role requires native interpreter $expected_interp"
      return 1
    fi
    interp_base=${expected_interp##*/}
    case "$architecture" in
      armv7)
        nxbootstrap_select_execution_loader "$role" \
          "$expected_class" "$expected_machine" \
          "/usr/lib/$interp_base" "/usr/lib32/$interp_base" \
          "/lib32/$interp_base" \
          "/mnt/SDCARD/spruce/flip/$interp_base" \
          "/mnt/SDCARD/spruce/flip/muOS/usr/lib32/$interp_base" \
          "/mnt/SDCARD/spruce/flip/muOS/lib32/$interp_base" \
          "/mnt/SDCARD/Persistent/.32bit_chroot/usr/lib/$interp_base" \
          "/mnt/SDCARD/Persistent/.32bit_chroot/lib/$interp_base" || true
        ;;
      aarch64|i386|x86_64)
        nxbootstrap_select_execution_loader "$role" \
          "$expected_class" "$expected_machine" \
          "/usr/lib/$interp_base" "/usr/lib32/$interp_base" \
          "/usr/lib64/$interp_base" "/lib32/$interp_base" \
          "/lib64/$interp_base" || true
        ;;
    esac
    if [ -z "$NXBOOTSTRAP_ROUTE_LOADER" ]; then
      echo "ERROR: execution role $role cannot resolve $expected_interp"
      return 1
    fi
    executor=alternate-loader
    reason=default-interpreter-absent
  fi
  NXBOOTSTRAP_ROUTE_EXECUTOR=$executor
  echo "EXECUTION RECEIPT: role=$role abi=$architecture executor=$executor interpreter=$expected_interp loader=${NXBOOTSTRAP_ROUTE_LOADER:-default} closure=$closure roots=${NXBOOTSTRAP_ROUTE_LIBS:-system-default} reason=$reason"
}
%s
%s || {
  nxbootstrap_phase_event preflight ERROR bootstrap 6198 || true
  nxbootstrap_finish
  exit 1
}
NXBOOTSTRAP_SPLASH_EXECUTOR=$NXBOOTSTRAP_ROUTE_EXECUTOR
NXBOOTSTRAP_SPLASH_LOADER=$NXBOOTSTRAP_ROUTE_LOADER
NXBOOTSTRAP_SPLASH_LIBS=$NXBOOTSTRAP_ROUTE_LIBS
%s || {
  nxbootstrap_phase_event preflight ERROR bootstrap 6198 || true
  nxbootstrap_finish
  exit 1
}
NXBOOTSTRAP_GAME_EXECUTOR=$NXBOOTSTRAP_ROUTE_EXECUTOR
NXBOOTSTRAP_GAME_LOADER=$NXBOOTSTRAP_ROUTE_LOADER
NXBOOTSTRAP_GAME_LIBS=$NXBOOTSTRAP_ROUTE_LIBS
''' % (
        extractor_call,
        resolve_call("splash", splash),
        resolve_call("game", game),
    )


def render_helper_execution_block(config):
    roles = config.get("execution_roles")
    if roles is None or not roles["helpers"]:
        return ""
    blocks = []
    for helper in roles["helpers"]:
        elf_class, machine = ARCH_ELF_IDENTITIES[helper["architecture"]]
        blocks.append(
            """
nxbootstrap_resolve_execution_role %s %s %s %d %d %s %s %s || {
  echo "ERROR: additional helper execution contract failed: %s"
  nxbootstrap_finish
  exit 1
}"""
            % (
                shlex.quote("helper-" + helper["id"]),
                shell_game_path(helper["executable"]),
                shlex.quote(helper["architecture"]),
                elf_class,
                machine,
                shlex.quote(helper["interpreter"]),
                shlex.quote(helper["executor"]),
                shlex.quote(helper["closure"]),
                helper["id"],
            )
        )
    return "\n# Additional helpers are validated after owner data is complete." + "".join(
        blocks
    )


def render_system_sdl_early_capture_block(config):
    if config.get("sdl_provider") != "system":
        return ""
    return r'''
# The dynamic loader has already started this Bash process, so an inherited
# preload cannot be unloaded from Bash itself.  These builtin-only statements
# are nevertheless the earliest reachable boundary: every later framework
# subprocess starts without inherited BIN_PRELOAD, LD_PRELOAD or
# SDL_DYNAMIC_API.
nxbootstrap_system_sdl_builtin_fatal() {
  local reason=$1 status=125 launcher_path launcher_dir log_dir log_path
  launcher_path=${BASH_SOURCE[0]:-$0}
  case "$launcher_path" in
    /*) launcher_dir=${launcher_path%/*} ;;
    */*) launcher_dir=$PWD/${launcher_path%/*} ;;
    *) launcher_dir=$PWD ;;
  esac
  for log_dir in "$launcher_dir" "${TMPDIR:-/tmp}"; do
    [[ -n $log_dir && -d $log_dir && ! -L $log_dir ]] || continue
    log_path=$log_dir/''' + config["id"] + r'''-launcher-error.$$.log
    if (builtin umask 077; builtin set -C; builtin printf '%s\n' \
        '== nxbootstrap ''' + NXBOOTSTRAP_VERSION + r''' | pre-runtime failure ==' \
        "status=$status pid=$$" "launcher=$launcher_path" \
        'gamedir=unresolved' 'cfw=unknown' "reason=$reason" \
        > "$log_path") 2>/dev/null; then
      break
    fi
  done
  builtin printf 'ERROR: %s\n' "$reason" >&2
  builtin trap - EXIT INT TERM HUP
  builtin exit "$status"
}
NXBOOTSTRAP_SYSTEM_SDL_INHERITED_BIN_PRELOAD=${BIN_PRELOAD-}
NXBOOTSTRAP_SYSTEM_SDL_INHERITED_LD_PRELOAD=${LD_PRELOAD-}
NXBOOTSTRAP_SYSTEM_SDL_INHERITED_DYNAMIC_API=${SDL_DYNAMIC_API-}
if ! builtin unset BIN_PRELOAD LD_PRELOAD SDL_DYNAMIC_API || \
   [[ ${BIN_PRELOAD+x} == x || ${LD_PRELOAD+x} == x || \
      ${SDL_DYNAMIC_API+x} == x ]]; then
  nxbootstrap_system_sdl_builtin_fatal \
    'sdl_provider=system cannot quarantine readonly inherited overrides'
fi
readonly NXBOOTSTRAP_SYSTEM_SDL_INHERITED_BIN_PRELOAD \
  NXBOOTSTRAP_SYSTEM_SDL_INHERITED_LD_PRELOAD \
  NXBOOTSTRAP_SYSTEM_SDL_INHERITED_DYNAMIC_API'''


def render_system_sdl_route_snapshot_block(config):
    if config.get("sdl_provider") != "system":
        return ""
    return r'''
# Snapshot the manifest-owned executable and resolved loader route before the
# mutable adapter hook.  system provider mode permits preload configuration,
# not replacement of the executable/interpreter boundary itself.
NXBOOTSTRAP_SYSTEM_SDL_MANIFEST_BIN=$BIN
NXBOOTSTRAP_SYSTEM_SDL_MANIFEST_BIN_PHYSICAL=$NXBOOTSTRAP_EXECUTABLE
NXBOOTSTRAP_SYSTEM_SDL_INTERP_PREFIX=${NXBOOTSTRAP_INTERP_PREFIX-}
NXBOOTSTRAP_SYSTEM_SDL_GAME_LOADER=${NXBOOTSTRAP_GAME_LOADER-}
NXBOOTSTRAP_SYSTEM_SDL_GAME_LIBS=${NXBOOTSTRAP_GAME_LIBS-}
readonly NXBOOTSTRAP_SYSTEM_SDL_MANIFEST_BIN \
  NXBOOTSTRAP_SYSTEM_SDL_MANIFEST_BIN_PHYSICAL \
  NXBOOTSTRAP_SYSTEM_SDL_INTERP_PREFIX \
  NXBOOTSTRAP_SYSTEM_SDL_GAME_LOADER NXBOOTSTRAP_SYSTEM_SDL_GAME_LIBS'''


OWNER_RUNTIME_SOURCE = r'''# V5 owner runtime: sealed helper first (generation member), then the live
# owner hook under the reserved-variable guard. Failure is early and visible;
# the owner's bytes are never touched.
if ! nxbootstrap_owner_env_source "$GAMEDIR/adapter-env.sh" sealed; then
  nxbootstrap_finish; exit 1
fi
if ! nxbootstrap_owner_env_source "$GAMEDIR/port-env.sh" owner; then
  nxbootstrap_finish; exit 1
fi
echo "OWNER RUNTIME: NX-OWNER-RUNTIME/1 order=heal-sealed,extract,splash,seed-owner,sealed-env,owner-env,launch sealed=adapter-env.sh owner=port-env.sh reserved_ok=1"'''


def render_port_env_source_block(config):
    owner = config.get("owner_runtime") == "1"
    if config.get("sdl_provider") != "system":
        if owner:
            return OWNER_RUNTIME_SOURCE
        return ('[ -f "$GAMEDIR/port-env.sh" ] && '
                '[ ! -L "$GAMEDIR/port-env.sh" ] && '
                '. "$GAMEDIR/port-env.sh"')
    source_line = (OWNER_RUNTIME_SOURCE if owner else
                   'if [ -f "$GAMEDIR/port-env.sh" ] && [ ! -L "$GAMEDIR/port-env.sh" ]; then\n'
                   '  . "$GAMEDIR/port-env.sh"\nfi')
    return r'''# Source the mutable adapter with its historical top-level semantics. The
# first statements after it returns copy provider values, remove their export
# attribute (which works even for readonly variables), then unset them before
# the next framework command. Commands the hook itself executes remain
# adapter-owned and necessarily precede this return boundary.
NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_BIN_PRELOAD=$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_BIN_PRELOAD
NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_LD_PRELOAD=$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_LD_PRELOAD
NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_DYNAMIC_API=$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_DYNAMIC_API
@@SOURCE_LINE@@
NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_BIN_PRELOAD=${BIN_PRELOAD-}
NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_LD_PRELOAD=${LD_PRELOAD-}
NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_DYNAMIC_API=${SDL_DYNAMIC_API-}
if ! builtin export -n BIN_PRELOAD LD_PRELOAD SDL_DYNAMIC_API; then
  nxbootstrap_system_sdl_builtin_fatal \
    'sdl_provider=system cannot de-export adapter overrides'
fi
NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_UNSET_FAILED=0
builtin unset BIN_PRELOAD LD_PRELOAD SDL_DYNAMIC_API || \
  NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_UNSET_FAILED=1
if [ "$NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_UNSET_FAILED" = 1 ] || \
   [[ ${BIN_PRELOAD+x} == x || ${LD_PRELOAD+x} == x || \
      ${SDL_DYNAMIC_API+x} == x ]]; then
  echo "ERROR: sdl_provider=system rejects readonly adapter override"
  nxbootstrap_finish
  exit 1
fi
NXBOOTSTRAP_SYSTEM_SDL_BIN_PRELOAD=$NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_BIN_PRELOAD
if [ -n "$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_BIN_PRELOAD" ] && \
   [ "$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_BIN_PRELOAD" != \
     "$NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_BIN_PRELOAD" ]; then
  NXBOOTSTRAP_SYSTEM_SDL_BIN_PRELOAD="${NXBOOTSTRAP_SYSTEM_SDL_BIN_PRELOAD:+$NXBOOTSTRAP_SYSTEM_SDL_BIN_PRELOAD:}$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_BIN_PRELOAD"
fi
NXBOOTSTRAP_SYSTEM_SDL_LD_PRELOAD=$NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_LD_PRELOAD
if [ -n "$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_LD_PRELOAD" ] && \
   [ "$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_LD_PRELOAD" != \
     "$NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_LD_PRELOAD" ]; then
  NXBOOTSTRAP_SYSTEM_SDL_LD_PRELOAD="${NXBOOTSTRAP_SYSTEM_SDL_LD_PRELOAD:+$NXBOOTSTRAP_SYSTEM_SDL_LD_PRELOAD:}$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_LD_PRELOAD"
fi
NXBOOTSTRAP_SYSTEM_SDL_DYNAMIC_API=$NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_DYNAMIC_API
[ -n "$NXBOOTSTRAP_SYSTEM_SDL_DYNAMIC_API" ] || \
  NXBOOTSTRAP_SYSTEM_SDL_DYNAMIC_API=$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_DYNAMIC_API
readonly NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_BIN_PRELOAD \
  NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_LD_PRELOAD \
  NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_DYNAMIC_API \
  NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_UNSET_FAILED \
  NXBOOTSTRAP_SYSTEM_SDL_BIN_PRELOAD NXBOOTSTRAP_SYSTEM_SDL_LD_PRELOAD \
  NXBOOTSTRAP_SYSTEM_SDL_DYNAMIC_API

if [ "$BIN" != "$NXBOOTSTRAP_SYSTEM_SDL_MANIFEST_BIN" ]; then
  echo "ERROR: sdl_provider=system rejects adapter BIN replacement"
  nxbootstrap_finish
  exit 1
fi
if [ "${NXBOOTSTRAP_INTERP_PREFIX-}" != \
     "$NXBOOTSTRAP_SYSTEM_SDL_INTERP_PREFIX" ] || \
   [ "${NXBOOTSTRAP_GAME_LOADER-}" != \
     "$NXBOOTSTRAP_SYSTEM_SDL_GAME_LOADER" ] || \
   [ "${NXBOOTSTRAP_GAME_LIBS-}" != "$NXBOOTSTRAP_SYSTEM_SDL_GAME_LIBS" ]; then
  echo "ERROR: sdl_provider=system rejects adapter executable-route replacement"
  nxbootstrap_finish
  exit 1
fi
NXBOOTSTRAP_EXE=$NXBOOTSTRAP_SYSTEM_SDL_MANIFEST_BIN_PHYSICAL
export NXBOOTSTRAP_EXE
readonly BIN NXBOOTSTRAP_EXE'''.replace('@@SOURCE_LINE@@', source_line)


def render_game_launch_block(config):
    if config.get("sdl_provider") == "system":
        if config.get("execution_roles") is None:
            return """\
(
  NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD=$NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_BIN_PRELOAD
  if [ -n "$NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_LD_PRELOAD" ]; then
    NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD="${NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD:+$NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD:}$NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_LD_PRELOAD"
  fi
  if [ -n "$NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD" ]; then
    export LD_PRELOAD=$NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD
  else
    unset LD_PRELOAD
  fi
  if [ -n "$NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_DYNAMIC_API" ]; then
    export SDL_DYNAMIC_API=$NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_DYNAMIC_API
  else
    unset SDL_DYNAMIC_API
  fi
  exec $NXBOOTSTRAP_INTERP_PREFIX "$BIN"@RUN_ARGS@
) 9>&- &"""
        return """\
(
  NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD=$NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_BIN_PRELOAD
  if [ -n "$NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_LD_PRELOAD" ]; then
    NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD="${NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD:+$NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD:}$NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_LD_PRELOAD"
  fi
  if [ -n "$NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD" ]; then
    export LD_PRELOAD=$NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD
  else
    unset LD_PRELOAD
  fi
  if [ -n "$NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_DYNAMIC_API" ]; then
    export SDL_DYNAMIC_API=$NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_DYNAMIC_API
  else
    unset SDL_DYNAMIC_API
  fi
  if [ -n "$NXBOOTSTRAP_GAME_LOADER" ]; then
    exec "$NXBOOTSTRAP_GAME_LOADER" --library-path "$NXBOOTSTRAP_GAME_LIBS" \
      "$BIN"@RUN_ARGS@
  fi
  exec "$BIN"@RUN_ARGS@
) 9>&- &"""
    if config.get("execution_roles") is None:
        return """\
if [ -n "$BIN_PRELOAD" ]; then
  LD_PRELOAD="$BIN_PRELOAD${LD_PRELOAD:+:$LD_PRELOAD}" $NXBOOTSTRAP_INTERP_PREFIX "$BIN"@RUN_ARGS@ 9>&- &
else
  $NXBOOTSTRAP_INTERP_PREFIX "$BIN"@RUN_ARGS@ 9>&- &
fi"""
    return """\
if [ -n "$NXBOOTSTRAP_GAME_LOADER" ]; then
  if [ -n "$BIN_PRELOAD" ]; then
    LD_PRELOAD="$BIN_PRELOAD${LD_PRELOAD:+:$LD_PRELOAD}" \\
      "$NXBOOTSTRAP_GAME_LOADER" --library-path "$NXBOOTSTRAP_GAME_LIBS" \\
      "$BIN"@RUN_ARGS@ 9>&- &
  else
    "$NXBOOTSTRAP_GAME_LOADER" --library-path "$NXBOOTSTRAP_GAME_LIBS" \\
      "$BIN"@RUN_ARGS@ 9>&- &
  fi
elif [ -n "$BIN_PRELOAD" ]; then
  LD_PRELOAD="$BIN_PRELOAD${LD_PRELOAD:+:$LD_PRELOAD}" "$BIN"@RUN_ARGS@ 9>&- &
else
  "$BIN"@RUN_ARGS@ 9>&- &
fi"""


def render_splash_block(config):
    roles = config.get("execution_roles")
    splash_architecture = (
        roles["splash"]["architecture"] if roles else config["architecture"]
    )
    if splash_architecture == "armv7":
        directories = """\
"$controlfolder/libs" "$controlfolder/libs.armhf" \\
  /usr/local/lib/arm-linux-gnueabihf /usr/local/lib32 \\
  /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf /usr/lib32 /usr/lib"""
    elif splash_architecture == "aarch64":
        directories = """\
"$controlfolder/libs" "$controlfolder/libs.aarch64" \\
  /usr/local/lib/aarch64-linux-gnu /usr/local/lib64 /usr/local/lib \\
  /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu \\
  /usr/lib64 /lib64 /usr/lib /lib"""
    elif splash_architecture == "i386":
        directories = """\
"$controlfolder/libs" "$controlfolder/libs.x86" \\
  /usr/local/lib/i386-linux-gnu /usr/local/lib32 \\
  /usr/lib/i386-linux-gnu /lib/i386-linux-gnu /usr/lib32 /lib32 /usr/lib"""
    else:
        directories = """\
"$controlfolder/libs" "$controlfolder/libs.x86_64" \\
  /usr/local/lib/x86_64-linux-gnu /usr/local/lib64 /usr/local/lib \\
  /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu \\
  /usr/lib64 /lib64 /usr/lib /lib"""
    if roles is not None:
        return """\

# Mandatory framework identity handoff: data is complete, native lifecycle has
# not started, and no game-private library path has been added by nxbootstrap.
NXBOOTSTRAP_SPLASH="$GAMEDIR/%s"
echo "nxsplash %s: mandatory handoff begin"
nxbootstrap_phase_event nxsplash START bootstrap 6204 || {
  nxbootstrap_finish
  exit 1
}
if [ -n "$NXBOOTSTRAP_SPLASH_LOADER" ]; then
  NX_SPLASH_TTY="${CUR_TTY:-}" \\
    LD_LIBRARY_PATH="$NXBOOTSTRAP_SPLASH_LIBS" \\
    "$NXBOOTSTRAP_SPLASH_LOADER" --library-path \\
    "$NXBOOTSTRAP_SPLASH_LIBS" "$NXBOOTSTRAP_SPLASH" %s
else
  NX_SPLASH_TTY="${CUR_TTY:-}" \\
    LD_LIBRARY_PATH="$NXBOOTSTRAP_SPLASH_LIBS" \\
    "$NXBOOTSTRAP_SPLASH" %s
fi
NXBOOTSTRAP_SPLASH_STATUS=$?
if [ "$NXBOOTSTRAP_SPLASH_STATUS" -eq 0 ]; then
  echo "nxsplash %s: mandatory handoff complete"
  nxbootstrap_phase_event nxsplash OK bootstrap 6205 || {
    nxbootstrap_finish
    exit 1
  }
else
  echo "ERROR: mandatory nxsplash failed status=$NXBOOTSTRAP_SPLASH_STATUS"
  nxbootstrap_phase_event nxsplash ERROR bootstrap 6206 \\
    "$NXBOOTSTRAP_SPLASH_STATUS" || true
  nxbootstrap_finish
  exit "$NXBOOTSTRAP_SPLASH_STATUS"
fi
unset NXBOOTSTRAP_SPLASH NXBOOTSTRAP_SPLASH_STATUS""" % (
            NXSPLASH_RUNTIME_NAME,
            NXSPLASH_VERSION,
            shlex.quote(config["title"]),
            shlex.quote(config["title"]),
            NXSPLASH_VERSION,
        )
    return """\

# Mandatory framework identity handoff: data is complete, native lifecycle has
# not started, and no game-private library path has been added by nxbootstrap.
NXBOOTSTRAP_SPLASH="$GAMEDIR/%s"
NXBOOTSTRAP_SPLASH_LIBS=""
for d in %s; do
  [ -n "$d" ] && [ -d "$d" ] && \\
    NXBOOTSTRAP_SPLASH_LIBS="${NXBOOTSTRAP_SPLASH_LIBS:+$NXBOOTSTRAP_SPLASH_LIBS:}$d"
done
echo "nxsplash %s: mandatory handoff begin"
nxbootstrap_phase_event nxsplash START bootstrap 6204 || {
  nxbootstrap_finish
  exit 1
}
# 0.6.23: em CFW 64-bit-only com o loader ARMHF fora do caminho, TODO ELF
# empacotado precisa do mesmo prefixo que o jogo — o splash executado direto
# morria com o falso "No such file or directory" (status 127) que o proprio
# preflight explica (caso de campo: spruce/Miyoo Flip).
NXBOOTSTRAP_SPLASH_PREFIX=""
if [ -n "${NXBOOTSTRAP_INTERP_PREFIX:-}" ]; then
  NXBOOTSTRAP_SPLASH_INTERP=$(LC_ALL=C grep -a -o -m1 -E '/lib(32|64)?/ld-[A-Za-z0-9._-]*\\.so[A-Za-z0-9._-]*' "$NXBOOTSTRAP_SPLASH" 2>/dev/null | head -n1)
  if [ -n "$NXBOOTSTRAP_SPLASH_INTERP" ] && [ ! -e "$NXBOOTSTRAP_SPLASH_INTERP" ]; then
    NXBOOTSTRAP_SPLASH_PREFIX="$NXBOOTSTRAP_INTERP_PREFIX"
    echo "NOTE: nxsplash also runs through the alternate dynamic loader."
  fi
fi
if NX_SPLASH_TTY="${CUR_TTY:-}" \\
   LD_LIBRARY_PATH="$NXBOOTSTRAP_SPLASH_LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \\
   $NXBOOTSTRAP_SPLASH_PREFIX "$NXBOOTSTRAP_SPLASH" %s; then
  echo "nxsplash %s: mandatory handoff complete"
  nxbootstrap_phase_event nxsplash OK bootstrap 6205 || {
    nxbootstrap_finish
    exit 1
  }
else
  NXBOOTSTRAP_SPLASH_STATUS=$?
  echo "ERROR: mandatory nxsplash failed status=$NXBOOTSTRAP_SPLASH_STATUS"
  nxbootstrap_phase_event nxsplash ERROR bootstrap 6206 \\
    "$NXBOOTSTRAP_SPLASH_STATUS" || true
  nxbootstrap_finish
  exit "$NXBOOTSTRAP_SPLASH_STATUS"
fi
unset NXBOOTSTRAP_SPLASH NXBOOTSTRAP_SPLASH_LIBS NXBOOTSTRAP_SPLASH_STATUS \\
  NXBOOTSTRAP_SPLASH_PREFIX NXBOOTSTRAP_SPLASH_INTERP d""" % (
        NXSPLASH_RUNTIME_NAME,
        directories,
        NXSPLASH_VERSION,
        shlex.quote(config["title"]),
        NXSPLASH_VERSION,
    )


def render_language_block(config):
    language = config.get("language")
    if language is None:
        return ""
    allowed = ["auto"] + [
        code for code in language["supported"] if code != "auto"
    ]
    return """
# Language / Idioma (supported / suportados: %s).
# Edit only this value; adapters opt in through nxport.json.
GAME_LANGUAGE="%s"
case "$GAME_LANGUAGE" in
  %s) ;;
  *) GAME_LANGUAGE="auto" ;;
esac
NXPORT_LANGUAGE=${NXPORT_LANGUAGE:-$GAME_LANGUAGE}
case "$NXPORT_LANGUAGE" in
  %s) ;;
  *) NXPORT_LANGUAGE=$GAME_LANGUAGE ;;
esac
NXBOOTSTRAP_LANGUAGE=$NXPORT_LANGUAGE
export NXPORT_LANGUAGE
""" % (
        ", ".join(language["supported"]),
        language["default"],
        "|".join(allowed),
        "|".join(allowed),
    )


def render_options_block(config):
    """Bloco editavel das opcoes declarativas, no mesmo formato do idioma.

    Cada opcao vira UM valor editavel no topo do launcher, validado contra a
    lista declarada, e e exportada na variavel que o manifesto nomeou. Um
    valor fora da lista -- editado a mao ou herdado do ambiente -- volta ao
    padrao declarado em vez de chegar cru ao jogo.
    """
    options = config.get("options") or []
    if not options:
        return ""
    blocks = []
    for option in options:
        allowed = "|".join(option["values"])
        blocks.append("""
# %s (%s).
# Edit only this value; adapters opt in through nxport.json.
%s="%s"
case "$%s" in
  %s) ;;
  *) %s="%s" ;;
esac
%s=${%s:-$%s}
case "$%s" in
  %s) ;;
  *) %s=$%s ;;
esac
NXBOOTSTRAP_OPTION_%s=$%s
export %s
""" % (
            option["label"], ", ".join(option["values"]),
            option["shell_variable"], option["default"],
            option["shell_variable"], allowed,
            option["shell_variable"], option["default"],
            option["environment"], option["environment"],
            option["shell_variable"],
            option["environment"], allowed,
            option["environment"], option["shell_variable"],
            option["shell_suffix"], option["environment"],
            option["environment"],
        ))
    return "".join(blocks)


def render_options_reassert_block(config):
    options = config.get("options") or []
    if not options:
        return ""
    lines = ["""
# Reassert after the mutable adapter hook; adapters consume, never redefine."""]
    for option in options:
        lines.append("\n%s=$NXBOOTSTRAP_OPTION_%s\nexport %s" % (
            option["environment"], option["shell_suffix"],
            option["environment"],
        ))
    lines.append("\n")
    return "".join(lines)


def render_language_reassert_block(config):
    if config.get("language") is None:
        return ""
    return """
# Reassert after the mutable adapter hook; adapters consume, never redefine it.
NXPORT_LANGUAGE=$NXBOOTSTRAP_LANGUAGE
export NXPORT_LANGUAGE
"""


def render_library_block(config):
    sdl_provider = config.get("sdl_provider")
    private = "".join(':$GAMEDIR/%s' % path
                      for path in config["private_library_paths"])
    private_roots = ['"$GAMEDIR"', '"$GAMEDIR"/lib']
    for path in config["private_library_paths"]:
        expression = shell_game_path(path)
        if expression not in private_roots:
            private_roots.append(expression)
    private_root_list = " ".join(private_roots)
    system_sdl_guard = ""
    if sdl_provider == "system":
        system_sdl_guard = r'''# sdl_provider=system is a real provider boundary, not a path-order claim.
# Resolve every explicit override against the final game search path. Failure
# to resolve is itself a refusal: an unchecked value never reaches the child.
# glibc expands $LIB/${LIB} inside LD_PRELOAD before Bash starts. Preserve an
# inherited form when the exact expansion is already and uniquely mapped in
# this launcher process. ArkOS/PortMaster can also leave a stale system SDL
# filename behind: when the loader did not map it, discard only the narrow
# inherited /usr/$LIB or /lib/$LIB SDL1/2 form and only when the adapter left
# LD_PRELOAD untouched. Never execute text or guess lib/lib64/multiarch.
# Local SDL_DYNAMIC_API is authoritative by definition; BIN_PRELOAD and
# LD_PRELOAD reject SDL1/2 names plus conservative core/add-on symbol pairs.
# Package-wide ELF/SONAME/provenance analysis remains the NXRelease boundary.
nxbootstrap_resolve_inherited_ldso_lib_token() {
  local entry=$1 prefix suffix address permissions offset device inode
  local mapped extra canonical resolved=""
  case "$entry" in
    *'/$LIB/'*)
      prefix=${entry%%\$LIB*}
      suffix=${entry#*\$LIB}
      ;;
    *'/${LIB}/'*)
      prefix=${entry%%\$\{LIB\}*}
      suffix=${entry#*\$\{LIB\}}
      ;;
    *) return 1 ;;
  esac
  case "$prefix" in /*/) ;; *) return 1 ;; esac
  case "$suffix" in /*) ;; *) return 1 ;; esac
  case "$prefix$suffix" in *'$'*|*'`'*|*$'\n'*|*$'\r'*) return 1 ;; esac
  [ -r "/proc/$$/maps" ] || return 1
  while IFS=' ' read -r address permissions offset device inode mapped extra; do
    [ -n "$mapped" ] && [ -z "$extra" ] || continue
    case "$prefix" in
      /) case "$mapped" in /*"$suffix") ;; *) continue ;; esac ;;
      *) case "$mapped" in "${prefix%/}"/*"$suffix") ;; *) continue ;; esac ;;
    esac
    canonical=$(readlink -f -- "$mapped" 2>/dev/null) || continue
    [ -f "$canonical" ] || continue
    if [ -n "$resolved" ] && [ "$resolved" != "$canonical" ]; then
      return 1
    fi
    resolved=$canonical
  done < "/proc/$$/maps"
  [ -n "$resolved" ] || return 1
  NXBOOTSTRAP_SDL_OVERRIDE_RESOLVED=$resolved
}
nxbootstrap_stale_inherited_system_sdl_preload_is_discardable() {
  local variable=$1 entry=$2 remainder
  [ "$variable" = LD_PRELOAD ] || return 1
  [ -z "$NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_LD_PRELOAD" ] || \
    [ "$NXBOOTSTRAP_SYSTEM_SDL_ADAPTER_LD_PRELOAD" = \
      "$NXBOOTSTRAP_SYSTEM_SDL_INHERITED_LD_PRELOAD" ] || return 1
  case "$entry" in
    '/usr/$LIB/'*|'/lib/$LIB/'*) remainder=${entry#*/\$LIB/} ;;
    '/usr/${LIB}/'*|'/lib/${LIB}/'*) remainder=${entry#*/\$\{LIB\}/} ;;
    *) return 1 ;;
  esac
  [ -n "$remainder" ] || return 1
  case "$remainder" in
    */*|*'$'*|*'`'*|*$'\n'*|*$'\r'*) return 1 ;;
  esac
  nxbootstrap_sdl_name_is_sdl12 "$remainder"
}
nxbootstrap_resolve_sdl_override_entry() {
  local variable=$1 entry=$2 candidate="" directory canonical origin
  NXBOOTSTRAP_SDL_OVERRIDE_RESOLVED=""
  [ -n "$entry" ] || return 1
  case "$entry" in
    '$ORIGIN'/*)
      origin=${NXBOOTSTRAP_SYSTEM_SDL_MANIFEST_BIN_PHYSICAL%/*}
      [ -n "$origin" ] || origin=/
      candidate="$origin/${entry#\$ORIGIN/}"
      ;;
    '${ORIGIN}'/*)
      origin=${NXBOOTSTRAP_SYSTEM_SDL_MANIFEST_BIN_PHYSICAL%/*}
      [ -n "$origin" ] || origin=/
      candidate="$origin/${entry#\$\{ORIGIN\}/}"
      ;;
    *'/$LIB/'*|*'/${LIB}/'*)
      [ "$variable" = LD_PRELOAD ] || return 1
      nxbootstrap_resolve_inherited_ldso_lib_token "$entry"
      return $?
      ;;
    /*) candidate=$entry ;;
    */*) candidate="$GAMEDIR/$entry" ;;
    *)
      while IFS= read -r directory; do
        [ -n "$directory" ] || continue
        [ -e "$directory/$entry" ] || [ -L "$directory/$entry" ] || continue
        candidate="$directory/$entry"
        break
      done < <(printf '%s\n' "${LD_LIBRARY_PATH//:/$'\n'}")
      ;;
  esac
  [ -n "$candidate" ] || return 1
  canonical=$(readlink -f -- "$candidate" 2>/dev/null) || return 1
  [ -f "$canonical" ] || return 1
  NXBOOTSTRAP_SDL_OVERRIDE_RESOLVED=$canonical
}
nxbootstrap_private_file_looks_sdl12() {
  nxbootstrap_file_looks_sdl12 "$1"
}
nxbootstrap_reject_private_sdl_override() {
  local variable=$1 value=$2 entry resolved discarded=0
  local -a entries=()
  NXBOOTSTRAP_SDL_OVERRIDE_FILTERED=""
  [ -n "$value" ] || return 0
  case "$value" in *$'\n'*|*$'\r'*)
    echo "ERROR: sdl_provider=system rejects malformed override variable=$variable"
    return 1
    ;;
  esac
  IFS=' :' read -r -a entries <<< "$value"
  [ "${#entries[@]}" -gt 0 ] || {
    echo "ERROR: sdl_provider=system rejects empty override variable=$variable"
    return 1
  }
  for entry in "${entries[@]}"; do
    [ -n "$entry" ] || continue
    if ! nxbootstrap_resolve_sdl_override_entry "$variable" "$entry"; then
      if nxbootstrap_stale_inherited_system_sdl_preload_is_discardable \
          "$variable" "$entry"; then
        echo "SDL PROVIDER GUARD: discarded stale inherited system SDL preload entry=$entry"
        discarded=1
        continue
      fi
      echo "ERROR: sdl_provider=system rejects unresolved override variable=$variable entry=$entry"
      return 1
    fi
    resolved=$NXBOOTSTRAP_SDL_OVERRIDE_RESOLVED
    case "$resolved" in
      "$NXBOOTSTRAP_PHYSICAL_GAMEDIR"/*) ;;
      *) continue ;;
    esac
    if [ "$variable" = SDL_DYNAMIC_API ] || \
       nxbootstrap_private_file_looks_sdl12 "$resolved"; then
      echo "ERROR: sdl_provider=system rejects private SDL override variable=$variable path=$resolved"
      return 1
    fi
    NXBOOTSTRAP_SDL_OVERRIDE_FILTERED="${NXBOOTSTRAP_SDL_OVERRIDE_FILTERED:+$NXBOOTSTRAP_SDL_OVERRIDE_FILTERED:}$entry"
  done
  [ "$discarded" = 1 ] || NXBOOTSTRAP_SDL_OVERRIDE_FILTERED=$value
  return 0
}
nxbootstrap_enforce_system_sdl_overrides() {
  nxbootstrap_reject_private_sdl_override \
    BIN_PRELOAD "$NXBOOTSTRAP_SYSTEM_SDL_BIN_PRELOAD" || return 1
  NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_BIN_PRELOAD=$NXBOOTSTRAP_SDL_OVERRIDE_FILTERED
  nxbootstrap_reject_private_sdl_override \
    LD_PRELOAD "$NXBOOTSTRAP_SYSTEM_SDL_LD_PRELOAD" || return 1
  NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_LD_PRELOAD=$NXBOOTSTRAP_SDL_OVERRIDE_FILTERED
  nxbootstrap_reject_private_sdl_override \
    SDL_DYNAMIC_API "$NXBOOTSTRAP_SYSTEM_SDL_DYNAMIC_API" || return 1
  NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_DYNAMIC_API=$NXBOOTSTRAP_SDL_OVERRIDE_FILTERED
  readonly NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_BIN_PRELOAD \
    NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_LD_PRELOAD \
    NXBOOTSTRAP_SYSTEM_SDL_EFFECTIVE_DYNAMIC_API
}
nxbootstrap_enforce_system_sdl_overrides || {
  nxbootstrap_finish
  exit 1
}
unset NXBOOTSTRAP_SDL_OVERRIDE_FILTERED
echo "SDL PROVIDER GUARD: manifest BIN fixed; explicit overrides resolved; stale inherited system SDL removed; package-wide deep audit=nxrelease"
'''
    private_sdl_gate = r'''# SDL1/SDL2 belong to the CFW/PortMaster integration. A local provider in
# any game-visible library root would shadow it before the runtime can repair
# input or video. SDL3 stays eligible only for the separately declared
# Godot/native-SDL3 exception and is deliberately not matched here.
nxbootstrap_sdl_name_is_sdl12() {
  local base=${1##*/}
  base=${base,,}
  case "$base" in
    libsdl3|libsdl3.*|libsdl3_*|libsdl3+*|libsdl3-*) return 1 ;;
    libsdl2|libsdl2.*|libsdl2_*|libsdl2+*|libsdl2-*|\
    libsdl|libsdl.*|libsdl_*|libsdl+*|libsdl-*) return 0 ;;
  esac
  return 1
}
nxbootstrap_sdl_file_has_pair() {
  local candidate=$1 first=$2 second=$3
  LD_PRELOAD= SDL_DYNAMIC_API= LC_ALL=C command grep -a -F -q "$first" \
    "$candidate" 2>/dev/null && \
  LD_PRELOAD= SDL_DYNAMIC_API= LC_ALL=C command grep -a -F -q "$second" \
    "$candidate" 2>/dev/null
}
nxbootstrap_file_looks_sdl12() {
  local candidate=$1 base=${1##*/} lowered
  lowered=${base,,}
  case "$lowered" in
    libsdl3|libsdl3.*|libsdl3_*|libsdl3+*|libsdl3-*) return 1 ;;
  esac
  nxbootstrap_sdl_name_is_sdl12 "$base" && return 0
  nxbootstrap_sdl_file_has_pair "$candidate" Mix_OpenAudio Mix_Quit ||
    nxbootstrap_sdl_file_has_pair "$candidate" IMG_Init IMG_Quit ||
    nxbootstrap_sdl_file_has_pair "$candidate" TTF_Init TTF_Quit ||
    nxbootstrap_sdl_file_has_pair "$candidate" SDLNet_Init SDLNet_Quit ||
    nxbootstrap_sdl_file_has_pair "$candidate" rotozoomSurface pixelColor ||
    nxbootstrap_sdl_file_has_pair "$candidate" GPU_Init GPU_Quit ||
    nxbootstrap_sdl_file_has_pair "$candidate" Sound_Init Sound_Quit ||
    nxbootstrap_sdl_file_has_pair "$candidate" RTF_Init RTF_Quit ||
    nxbootstrap_sdl_file_has_pair "$candidate" FC_CreateFont FC_FreeFont ||
    {
      LD_PRELOAD= SDL_DYNAMIC_API= LC_ALL=C command grep -a -F -q \
        SDL_Init "$candidate" 2>/dev/null &&
      LD_PRELOAD= SDL_DYNAMIC_API= LC_ALL=C command grep -a -F -q \
        SDL_PollEvent "$candidate" 2>/dev/null &&
      { nxbootstrap_sdl_file_has_pair "$candidate" \
          SDL_CreateWindow SDL_GetVersion ||
        nxbootstrap_sdl_file_has_pair "$candidate" \
          SDL_SetVideoMode SDL_Linked_Version; }
    }
}
nxbootstrap_reject_private_sdl12() {
  local root=$1 candidate
  [ -d "$root" ] || return 0
  for candidate in "$root"/[lL][iI][bB][sS][dD][lL]*; do
    [ -e "$candidate" ] || [ -L "$candidate" ] || continue
    nxbootstrap_sdl_name_is_sdl12 "$candidate" || continue
    echo "ERROR: private SDL1/SDL2 provider is forbidden: $candidate"
    return 1
  done
}
for nxbootstrap_sdl_root in %s; do
  nxbootstrap_reject_private_sdl12 "$nxbootstrap_sdl_root" || {
    nxbootstrap_finish
    exit 1
  }
done
unset nxbootstrap_sdl_root

nxbootstrap_append_inherited_library_paths() {
  local inherited=$1 directory canonical
  NXBOOTSTRAP_FILTERED_INHERITED=""
  while IFS= read -r directory; do
    [ -n "$directory" ] && [ "${directory#/}" != "$directory" ] || continue
    canonical=$(readlink -f -- "$directory" 2>/dev/null) || continue
    [ -d "$canonical" ] || continue
    case "$canonical" in
      "$NXBOOTSTRAP_PHYSICAL_GAMEDIR"|"$NXBOOTSTRAP_PHYSICAL_GAMEDIR"/*)
        echo "LIBRARY PATH: removed inherited port-local root"
        continue
        ;;
    esac
    case ":$NXBOOTSTRAP_FILTERED_INHERITED:" in
      *:"$canonical":*) ;;
      *) NXBOOTSTRAP_FILTERED_INHERITED="${NXBOOTSTRAP_FILTERED_INHERITED:+$NXBOOTSTRAP_FILTERED_INHERITED:}$canonical" ;;
    esac
  done < <(printf '%%s\n' "${inherited//:/$'\n'}")
}
''' % private_root_list
    roles = config.get("execution_roles")
    if roles is not None:
        game = roles["game"]
        roots_shell_items = ['"$GAMEDIR"', '"$GAMEDIR/lib"']
        for path in config["private_library_paths"]:
            expression = shell_game_path(path)
            if expression not in roots_shell_items:
                roots_shell_items.append(expression)
        roots_shell = " ".join(roots_shell_items)
        audio_block = ""
        if game["architecture"] == "armv7":
            audio_block = """

# Resolve target-ABI audio modules only inside the isolated game closure.
while IFS= read -r d; do
  [ -n "$d" ] || continue
  [ -d "$d/pipewire-0.3" ] && export PIPEWIRE_MODULE_DIR="$d/pipewire-0.3"
  [ -d "$d/spa-0.2" ] && export SPA_PLUGIN_DIR="$d/spa-0.2"
  [ -f "$d/alsa-lib/libasound_module_pcm_pipewire.so" ] && export ALSA_PLUGIN_DIR="$d/alsa-lib"
done < <(printf '%%s\n' "${NXBOOTSTRAP_GAME_LIBS//:/$'\n'}")"""
        provider = sdl_provider or "legacy"
        overlay = "firmware-first" if sdl_provider == "system" else "legacy"
        return private_sdl_gate + """\
# Opt-in mixed-ABI closure: firmware roots were selected before NXExtract;
# owner/game roots enter only after extraction and are class-checked here.
NXBOOTSTRAP_ROLE_LIBS="$NXBOOTSTRAP_GAME_LIBS"
for d in %s; do
  [ -d "$d" ] || continue
  if ! nxbootstrap_add_execution_root game "$d" %d %d port; then
    echo "ERROR: game closure contains an ELF from another ABI: $d"
    nxbootstrap_finish
    exit 1
  fi
done
NXBOOTSTRAP_GAME_LIBS="$NXBOOTSTRAP_ROLE_LIBS"
[ -n "$NXBOOTSTRAP_GAME_LIBS" ] || {
  echo "ERROR: no coherent game library closure was resolved"
  nxbootstrap_finish
  exit 1
}
export LD_LIBRARY_PATH="$NXBOOTSTRAP_GAME_LIBS"%s""" % (
            roots_shell,
            ARCH_ELF_IDENTITIES[game["architecture"]][0],
            ARCH_ELF_IDENTITIES[game["architecture"]][1],
            audio_block,
        ) + system_sdl_guard + """
NXBOOTSTRAP_SDL_PROVIDER=%s
NXBOOTSTRAP_SDL_RESOLUTION_PATH=$NXBOOTSTRAP_GAME_LIBS
export NXBOOTSTRAP_SDL_PROVIDER NXBOOTSTRAP_SDL_RESOLUTION_PATH
echo "SDL PROVIDER: declared=$NXBOOTSTRAP_SDL_PROVIDER order=%s backend=unforced search=$NXBOOTSTRAP_SDL_RESOLUTION_PATH"
""" % (provider, overlay)
    if config["architecture"] == "armv7":
        if sdl_provider == "system":
            directories = (
                "/usr/local/lib/arm-linux-gnueabihf /usr/local/lib32 \\\n"
                "         /usr/lib/arm-linux-gnueabihf "
                "/lib/arm-linux-gnueabihf /usr/lib32 /usr/lib \\\n"
                "         \"$controlfolder/libs\" "
                "\"$controlfolder/libs.armhf\""
            )
            provider = "system"
            overlay = "firmware-before-portmaster"
            inherited_prepare = (
                'nxbootstrap_append_inherited_library_paths '
                '"${LD_LIBRARY_PATH:-}"'
            )
            inherited_suffix = (
                '${NXBOOTSTRAP_FILTERED_INHERITED:+'
                ':$NXBOOTSTRAP_FILTERED_INHERITED}'
            )
            inherited_cleanup = "unset NXBOOTSTRAP_FILTERED_INHERITED"
        else:
            directories = (
                "/usr/local/lib/arm-linux-gnueabihf /usr/local/lib32 \\\n"
                "         /usr/lib/arm-linux-gnueabihf "
                "/lib/arm-linux-gnueabihf /usr/lib32 \\\n"
                "         \"$controlfolder/libs\" "
                "\"$controlfolder/libs.armhf\" /usr/lib"
            )
            provider = "legacy"
            overlay = "legacy"
            inherited_prepare = ":"
            inherited_suffix = '${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}'
            inherited_cleanup = ":"
        return private_sdl_gate + """\
# Firmware first; the device's 32-bit world (lib32 on muOS,
# arm-linux-gnueabihf on ArkOS/ROCKNIX) before our own; /usr/lib last (a
# 64-bit CFW's ld.so skips the wrong ELF class and keeps searching).
LIBS=""
for d in %s; do
  [ -n "$d" ] && [ -d "$d" ] && LIBS="${LIBS:+$LIBS:}$d"
done
%s
export LD_LIBRARY_PATH="${LIBS}:$GAMEDIR%s%s"
%s
NXBOOTSTRAP_SDL_PROVIDER=%s
NXBOOTSTRAP_SDL_RESOLUTION_PATH=$LD_LIBRARY_PATH
export NXBOOTSTRAP_SDL_PROVIDER NXBOOTSTRAP_SDL_RESOLUTION_PATH
echo "SDL PROVIDER: declared=$NXBOOTSTRAP_SDL_PROVIDER order=%s backend=unforced search=$NXBOOTSTRAP_SDL_RESOLUTION_PATH"
%s

# AArch64 CFWs route the ALSA default through PipeWire; the 32-bit process
# needs the 32-bit ALSA/PipeWire/SPA modules or SDL audio dies (TASM2).
for d in /usr/lib32 /usr/lib/arm-linux-gnueabihf /usr/local/lib/arm-linux-gnueabihf; do
  [ -d "$d/pipewire-0.3" ] && export PIPEWIRE_MODULE_DIR="$d/pipewire-0.3"
  [ -d "$d/spa-0.2" ] && export SPA_PLUGIN_DIR="$d/spa-0.2"
  [ -f "$d/alsa-lib/libasound_module_pcm_pipewire.so" ] && export ALSA_PLUGIN_DIR="$d/alsa-lib"
done""" % (directories, inherited_prepare, private, inherited_suffix,
             system_sdl_guard, provider, overlay, inherited_cleanup)
    if sdl_provider == "system":
        directories = (
            "/usr/local/lib/aarch64-linux-gnu /usr/local/lib64 "
            "/usr/local/lib \\\n"
            "         /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu \\\n"
            "         /usr/lib64 /lib64 /usr/lib /lib \\\n"
            "         \"$controlfolder/libs\" "
            "\"$controlfolder/libs.aarch64\""
        )
        provider = "system"
        overlay = "firmware-before-portmaster"
        inherited_prepare = (
            'nxbootstrap_append_inherited_library_paths '
            '"${LD_LIBRARY_PATH:-}"'
        )
        inherited_suffix = (
            '${NXBOOTSTRAP_FILTERED_INHERITED:+'
            ':$NXBOOTSTRAP_FILTERED_INHERITED}'
        )
        inherited_cleanup = "unset NXBOOTSTRAP_FILTERED_INHERITED"
    else:
        directories = (
            "\"$controlfolder/libs\" \"$controlfolder/libs.aarch64\" \\\n"
            "         /usr/local/lib/aarch64-linux-gnu /usr/local/lib64 "
            "/usr/local/lib \\\n"
            "         /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu \\\n"
            "         /usr/lib64 /lib64 /usr/lib /lib"
        )
        provider = "legacy"
        overlay = "portmaster-before-firmware"
        inherited_prepare = ":"
        inherited_suffix = '${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}'
        inherited_cleanup = ":"
    return private_sdl_gate + """\
# Firmware/PortMaster overlays first; preserve distro-local providers before
# the system defaults, then append the game's own directory.
LIBS=""
for d in %s; do
  [ -n "$d" ] && [ -d "$d" ] && LIBS="${LIBS:+$LIBS:}$d"
done
%s
export LD_LIBRARY_PATH="${LIBS}:$GAMEDIR%s%s"
%s
NXBOOTSTRAP_SDL_PROVIDER=%s
NXBOOTSTRAP_SDL_RESOLUTION_PATH=$LD_LIBRARY_PATH
export NXBOOTSTRAP_SDL_PROVIDER NXBOOTSTRAP_SDL_RESOLUTION_PATH
echo "SDL PROVIDER: declared=$NXBOOTSTRAP_SDL_PROVIDER order=%s backend=unforced search=$NXBOOTSTRAP_SDL_RESOLUTION_PATH"
%s
""" % (directories, inherited_prepare, private, inherited_suffix,
         system_sdl_guard, provider, overlay, inherited_cleanup)


def render(template_name, replacements):
    text = (ROOT / "templates" / template_name).read_text(encoding="utf-8")
    for name, value in replacements.items():
        text = text.replace("@%s@" % name, value)
    leftovers = sorted(set(re.findall(r"@[A-Z0-9_]+@", text)))
    if leftovers:
        raise ManifestError("unresolved template token(s): %s" % ", ".join(leftovers))
    return text


def atomic_write(path, content, mode, force):
    if (path.exists() or path.is_symlink()) and not force:
        raise ManifestError("refusing to overwrite %s (use --force)" % path)
    if path.parent.is_symlink() or not path.parent.is_dir():
        raise ManifestError("unsafe output parent: %s" % path.parent)
    descriptor, temporary = tempfile.mkstemp(prefix=".%s." % path.name,
                                              dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, str(path))
        _fsync_directory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def atomic_write_bytes(path, payload, mode, force):
    if (path.exists() or path.is_symlink()) and not force:
        raise ManifestError("refusing to overwrite %s (use --force)" % path)
    if path.parent.is_symlink() or not path.parent.is_dir():
        raise ManifestError("unsafe output parent: %s" % path.parent)
    descriptor, temporary = tempfile.mkstemp(prefix=".%s." % path.name,
                                              dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, str(path))
        _fsync_directory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def _fsync_directory(directory):
    # Onda v2 (AUD-26): sem o fsync do DIRETORIO o rename pode nao estar
    # durado num SD FAT quando a energia cai -- launcher de tamanho zero.
    try:
        descriptor = os.open(str(directory), os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(descriptor)
    except OSError:
        pass
    finally:
        os.close(descriptor)


def atomic_copy(source, path, mode, force):
    if (path.exists() or path.is_symlink()) and not force:
        raise ManifestError("refusing to overwrite %s (use --force)" % path)
    if path.parent.is_symlink() or not path.parent.is_dir():
        raise ManifestError("unsafe output parent: %s" % path.parent)
    descriptor, temporary = tempfile.mkstemp(prefix=".%s." % path.name,
                                              dir=str(path.parent))
    try:
        with source.open("rb") as input_stream, os.fdopen(
                descriptor, "wb") as output_stream:
            for block in iter(lambda: input_stream.read(1024 * 1024), b""):
                output_stream.write(block)
            output_stream.flush()
            os.fsync(output_stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, str(path))
        _fsync_directory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def safe_runtime_root(value):
    root = Path(os.path.abspath(os.fspath(value)))
    if root.is_symlink() or not root.is_dir():
        raise ManifestError("runtime root must be a real existing directory")
    cursor = Path(root.anchor)
    for part in root.parts[1:]:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ManifestError("runtime root traverses a symlink")
    return root


def runtime_member_source(runtime_root, member):
    cursor = runtime_root
    for part in PurePosixPath(member["path"]).parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ManifestError(
                "runtime member traverses a symlink: %s" % member["path"])
    if not cursor.is_file():
        raise ManifestError(
            "runtime member is missing or not regular: %s" % member["path"])
    actual_mode = "%04o" % stat.S_IMODE(cursor.stat().st_mode)
    if actual_mode != member["mode"]:
        raise ManifestError(
            "runtime member mode differs: %s expected=%s actual=%s" %
            (member["path"], member["mode"], actual_mode))
    actual_sha256 = sha256_file(cursor)
    if actual_sha256 != member["sha256"]:
        raise ManifestError(
            "runtime member hash differs: %s" % member["path"])
    return cursor


def ensure_relative_parent(root, relative):
    """Create/check every parent without ever traversing a symlink."""
    cursor = root
    for part in PurePosixPath(relative).parts[:-1]:
        cursor = cursor / part
        if cursor.exists() or cursor.is_symlink():
            if cursor.is_symlink() or not cursor.is_dir():
                raise ManifestError("unsafe output parent: %s" % cursor)
        else:
            cursor.mkdir(mode=0o755)
    return cursor


def shell_join(values):
    return shlex.quote("\n".join(values))


def canonical_manifest(config):
    """Return the deterministic canonical nxport.json content."""
    canonical = {
        "schema_version": config["schema_version"],
        "id": config["id"],
        "title": config["title"],
        "launcher_name": config["launcher_name"],
        "architecture": config["architecture"],
        "executable": config["executable"],
        "argument_mode": config["argument_mode"],
        "home_mode": config["home_mode"],
        "nxextract": config["nxextract"],
        "required_files": config["required_files"],
        "private_library_paths": config["private_library_paths"],
        "prepare_script": config["prepare_script"],
        "required_capabilities": config["required_capabilities"],
        "enabled_quirks": config["enabled_quirks"],
        "runtime_report": config["runtime_report"],
    }
    if config.get("sdl_provider") is not None:
        canonical["sdl_provider"] = config["sdl_provider"]
    if config.get("video_proof") is not None:
        canonical["video_proof"] = config["video_proof"]
    if config.get("owner_runtime") is not None:
        canonical["owner_runtime"] = config["owner_runtime"]
    if config.get("language") is not None:
        canonical["language"] = config["language"]
    if config.get("options"):
        canonical["options"] = [
            {key: option[key] for key in
             ("id", "label", "values", "default", "environment")}
            for option in config["options"]
        ]
    if config.get("execution_roles") is not None:
        canonical["execution_roles"] = config["execution_roles"]
    if config.get("generation_runtime") is not None:
        canonical["generation_runtime"] = config["generation_runtime"]
    return json.dumps(
        canonical, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


NXBUNDLE_MAGIC = "NXBUNDLE1"
NXBUNDLE_SUFFIX = ".nxb"


def bundle_name_for(generation_id):
    """V4-REPACK-01: the visible, content-addressed seed of one generation."""
    return "nxruntime-%s%s" % (generation_id, NXBUNDLE_SUFFIX)


def build_bundle_bytes(generation_id, port_id, members):
    """Return the deterministic bytes of one nxbundle-v1 seed.

    ``members`` is an ordered list of ``(relative_path, payload, mode)``.
    The relative paths are the generation-root paths of every authenticated
    file EXCEPT ``commit``, which the device writes last as its own receipt.

    The bundle is a plain regular file: an ASCII header terminated by ``END``
    followed by the payloads concatenated in header order.  Nothing in it
    depends on order of extraction, timestamps, modes reported by the
    transport, or the SHA-256 of any outer ZIP.
    """
    if not re.fullmatch(r"[0-9a-f]{64}", generation_id):
        raise ManifestError("bundle generation id must be a full SHA-256")
    seen = set()
    # The device this lands on stores /roms on exFAT, where two paths that
    # differ only in case are ONE file. The launcher refuses such a seed --
    # it has to, because a seed can be tampered with -- but a seed we build
    # ourselves must never reach a card in that state and be discovered by the
    # player. Refuse it here, at the only point that can still fix it.
    seen_folded = {}
    records = []
    offset = 0
    for relative, payload, mode in members:
        if relative in seen:
            raise ManifestError("bundle member appears twice: %s" % relative)
        seen.add(relative)
        folded = relative.lower()
        if folded in seen_folded:
            raise ManifestError(
                "bundle members collide on case-insensitive media: "
                "%s and %s" % (seen_folded[folded], relative))
        seen_folded[folded] = relative
        if (not relative or relative.startswith("/") or
                relative != posixpath.normpath(relative) or
                relative == "commit" or
                any(character in relative for character in "\t\n\r\\") or
                not all(part not in ("", ".", "..") for part in
                        relative.split("/"))):
            raise ManifestError("unsafe bundle member path: %s" % relative)
        if mode not in (0o644, 0o755):
            raise ManifestError("unsupported bundle member mode: %s" % relative)
        records.append(
            "M\t0%o\t%s\t%d\t%d\t%s\n" % (
                mode, hashlib.sha256(payload).hexdigest(), len(payload),
                offset, relative,
            )
        )
        offset += len(payload)
    header = "".join([
        NXBUNDLE_MAGIC + "\n",
        "generation %s\n" % generation_id,
        "port %s\n" % port_id,
        "members %d\n" % len(records),
    ] + records + ["END\n"])
    return header.encode("ascii") + b"".join(
        payload for _relative, payload, _mode in members
    )


def generation_identity_basis(config, nxsplash_sha256=None):
    """Return the canonical, clock-free preimage of a runtime generation.

    The nxport manifest alone is insufficient: regenerating the same port with
    a new bootstrap/template would otherwise reuse and overwrite an already
    committed generation directory.  Include the exact framework sources that
    determine the launcher plus the architecture-matched splash artifact.
    """
    roles = config.get("execution_roles")
    splash_architecture = (
        roles["splash"]["architecture"] if roles else config["architecture"]
    )
    if config.get("generation_runtime") is not None:
        splash_member = next(
            member for member in config["generation_runtime"]
            if member["role"] == "nxsplash"
        )
        if (nxsplash_sha256 is not None and
                nxsplash_sha256 != splash_member["sha256"]):
            raise ManifestError("nxsplash identity hash differs from nxport")
        nxport_sha256 = hashlib.sha256(
            canonical_manifest(config).encode("utf-8")
        ).hexdigest()
        # The final launcher embeds generation_id, so hashing its final bytes
        # into that same identity would be circular.  Bind the exact rendered
        # launcher with the sole self-reference replaced by 64 zeroes; the
        # generation manifest separately records the final launcher hash.
        launcher_preimage = render_launcher(config, "0" * 64).encode("utf-8")
        identity_components = [
            {
                "role": "launcher",
                "path": config["launcher_name"],
                "mode": "0755",
                "sha256": hashlib.sha256(launcher_preimage).hexdigest(),
            },
            {
                "role": "nxport",
                "path": "nxport.json",
                "mode": "0644",
                "sha256": nxport_sha256,
            },
        ] + list(config["generation_runtime"])
        runtime_records = generation_runtime_records(config)
        return {
            "schema": "org.nextos.nxruntime.generation-identity",
            "schema_version": 2,
            "nxport_sha256": nxport_sha256,
            "launcher_preimage_sha256": hashlib.sha256(
                launcher_preimage).hexdigest(),
            "runtime_records_sha256": hashlib.sha256(
                runtime_records.encode("utf-8")).hexdigest(),
            "nxbootstrap": {
                "version": NXBOOTSTRAP_VERSION,
                "generator_sha256": sha256_file(Path(__file__).resolve()),
                "launcher_template_sha256": sha256_file(
                    ROOT / "templates" / "launcher.sh.in"
                ),
            },
            "components": identity_components,
            "nxsplash": {
                "version": NXSPLASH_VERSION,
                "architecture": splash_architecture,
                "sha256": splash_member["sha256"],
            },
        }
    if nxsplash_sha256 is None:
        _source, nxsplash_sha256 = nxsplash_artifact(splash_architecture)
    return {
        "schema": "org.nextos.nxruntime.generation-identity",
        "schema_version": 1,
        "nxport_sha256": hashlib.sha256(
            canonical_manifest(config).encode("utf-8")
        ).hexdigest(),
        "nxbootstrap": {
            "version": NXBOOTSTRAP_VERSION,
            "generator_sha256": sha256_file(Path(__file__).resolve()),
            "launcher_template_sha256": sha256_file(
                ROOT / "templates" / "launcher.sh.in"
            ),
        },
        "nxsplash": {
            "version": NXSPLASH_VERSION,
            "architecture": splash_architecture,
            "sha256": nxsplash_sha256,
        },
    }


def generation_identity(config, nxsplash_sha256=None):
    basis = generation_identity_basis(config, nxsplash_sha256)
    if config.get("generation_runtime") is not None:
        canonical = generation_identity_text(basis).encode("utf-8")
    else:
        canonical = json.dumps(
            basis, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def generation_runtime_records(config):
    """Canonical ordered runtime-only identity records for schema v3."""
    return "".join(
        "%s\t%s\t%s\t%s\n" % (
            member["role"], member["mode"], member["sha256"], member["path"]
        )
        for member in config["generation_runtime"]
    )


def generation_identity_text(basis):
    """Stored v2 identity bytes; their SHA-256 is the generation id."""
    return json.dumps(
        basis, indent=2, sort_keys=True, ensure_ascii=False
    ) + "\n"


def render_launcher(config, generation_id=None):
    port32 = ("PORT_32BIT=\"Y\"\nexport PORT_32BIT"
              if config["architecture"] == "armv7" else "")
    # Titles are embedded inside double-quoted shell strings and comments, and
    # inside the single-quoted early-log and runtime banners as well, so an
    # apostrophe -- "Baldur's Gate" -- ends that quote and the generated
    # launcher dies with "unexpected end of file". The title is only ever
    # printed, never parsed; strip it with the other shell metacharacters.
    safe_title = re.sub(r"[`\"$\\']", "", config["title"])
    replacements = {
        "PORT_ID": config["id"],
        "PORT_ID_SHELL": shlex.quote(config["id"]),
        "REQUIRED_CAPABILITIES_SHELL": shell_join(
            config["required_capabilities"]),
        "ENABLED_QUIRKS_SHELL": shell_join(config["enabled_quirks"]),
        "RUNTIME_REPORT_SHELL": shlex.quote(config["runtime_report"]),
        "PORT_TITLE": safe_title,
        "PORT_TITLE_SHELL": shlex.quote(config["title"]),
        "LAUNCHER_NAME": config["launcher_name"],
        "PORT_EXECUTABLE": config["executable"],
        "NXBOOTSTRAP_VERSION": NXBOOTSTRAP_VERSION,
        "GENERATION_ID": generation_id or generation_identity(config),
        "GENERATION_FORMAT": (
            "2" if config.get("generation_runtime") is not None else "1"
        ),
        "OWNER_RUNTIME": "1" if config.get("owner_runtime") == "1" else "0",
        "GENERATION_NXEXTRACT_REQUIRED": (
            "1" if (config.get("generation_runtime") is not None and
                    config["nxextract"]["mode"] != "no") else "0"
        ),
        "PORT_32BIT_LITERAL": port32,
        "PORT_LANGUAGE_BLOCK": render_language_block(config),
        "PORT_LANGUAGE_REASSERT_BLOCK": render_language_reassert_block(config),
        "PORT_OPTIONS_BLOCK": render_options_block(config),
        "PORT_OPTIONS_REASSERT_BLOCK": render_options_reassert_block(config),
        "SYSTEM_SDL_EARLY_CAPTURE_BLOCK":
            render_system_sdl_early_capture_block(config),
        "SYSTEM_SDL_ROUTE_SNAPSHOT_BLOCK":
            render_system_sdl_route_snapshot_block(config),
        "PORT_ENV_SOURCE_BLOCK": render_port_env_source_block(config),
        "NXEXTRACT_VALIDATOR": render_nxextract_validator(config),
        "NXEXTRACT_BLOCK": render_nxextract_block(config),
        "REQUIRED_FILES_BLOCK": render_required_files_block(config),
        "NXSPLASH_BLOCK": render_splash_block(config),
        "VIDEO_PROOF_BLOCK": (
            "NXBOOTSTRAP_VIDEO_REQUIRED=1\n"
            "export NXBOOTSTRAP_VIDEO_REQUIRED"
            if config.get("video_proof") == "required" else ""
        ),
        "HOME_BLOCK": render_home_block(config),
        "LIBRARY_BLOCK": render_library_block(config),
        "INTERPRETER_ROUTING_BLOCK": render_interpreter_routing_block(config),
        "EXECUTION_ROLES_BLOCK": render_execution_roles_block(config),
        "HELPER_EXECUTION_BLOCK": render_helper_execution_block(config),
        "GAME_LAUNCH_BLOCK": render_game_launch_block(config),
        "RUN_ARGS": {
            "none": "",
            "passthrough": ' "$@"',
            "game-dir": ' "$GAMEDIR"',
            "game-dir-and-passthrough": ' "$GAMEDIR" "$@"',
        }[config["argument_mode"]],
    }
    return render("launcher.sh.in", replacements)


def generate(manifest_path, output, force, runtime_root=None):
    with manifest_path.open("r", encoding="utf-8") as stream:
        data = json.load(stream)
    config = validate(data)
    roles = config.get("execution_roles")
    splash_architecture = (
        roles["splash"]["architecture"] if roles else config["architecture"]
    )
    nxsplash_source, nxsplash_sha256 = nxsplash_artifact(splash_architecture)
    canonical_text = canonical_manifest(config)
    generation_v2 = config.get("generation_runtime") is not None
    runtime_sources = {}
    if generation_v2:
        if runtime_root is None:
            raise ManifestError(
                "schema v3 generation_runtime requires --runtime-root")
        runtime_root = safe_runtime_root(runtime_root)
        for member in config["generation_runtime"]:
            if member["role"] == "nxsplash":
                runtime_sources[member["path"]] = nxsplash_source
            else:
                runtime_sources[member["path"]] = runtime_member_source(
                    runtime_root, member
                )
    elif runtime_root is not None:
        raise ManifestError(
            "--runtime-root is only valid with generation_runtime")
    output.mkdir(parents=True, exist_ok=True)
    if output.is_symlink() or not output.is_dir():
        raise ManifestError("output must be a real directory: %s" % output)
    port_dir = output / config["id"]
    if port_dir.exists() or port_dir.is_symlink():
        if port_dir.is_symlink() or not port_dir.is_dir():
            raise ManifestError("port output must be a real directory: %s" %
                                port_dir)
    else:
        port_dir.mkdir(mode=0o755)
    launcher_target = output / config["launcher_name"]
    manifest_target = port_dir / "nxport.json"
    nxsplash_target = port_dir / NXSPLASH_RUNTIME_NAME
    runtime_targets = []
    if generation_v2:
        runtime_targets = [
            port_dir / member["path"]
            for member in config["generation_runtime"]
            if member["role"] != "nxsplash"
        ]
    targets = [launcher_target, manifest_target, nxsplash_target] + runtime_targets
    if not force:
        existing = [str(path) for path in targets if path.exists() or path.is_symlink()]
        if existing:
            raise ManifestError("refusing to overwrite: %s (use --force)" %
                                ", ".join(existing))
    # Onda v2 (AUD-26): publicar na ordem de DEPENDENCIA -- splash e manifesto
    # primeiro, o launcher (a porta de entrada) por ULTIMO. Interrompido no
    # meio, o conjunto antigo continua coerente: nunca um launcher novo
    # apontando para um splash/manifesto que ainda nao existe.
    generation_id = generation_identity(config, nxsplash_sha256)
    launcher_text = render_launcher(config, generation_id)
    # V3-UPDATE-01: publish the immutable generation FIRST (heal source must
    # exist before the entry point), then splash/manifest, launcher LAST.
    generation_root = port_dir / ".nxruntime" / "generations" / generation_id
    generation_files = generation_root / "files"
    launcher_bytes = launcher_text.encode("utf-8")
    manifest_bytes = canonical_text.encode("utf-8")
    components = {
        "launcher/" + config["launcher_name"]: (launcher_bytes, 0o755),
        "nxport.json": (manifest_bytes, 0o644),
    }
    component_records = [
        {
            "role": "launcher", "path": config["launcher_name"],
            "mode": "0755", "sha256": hashlib.sha256(
                launcher_bytes).hexdigest(),
        },
        {
            "role": "nxport", "path": "nxport.json", "mode": "0644",
            "sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        },
    ]
    if generation_v2:
        for member in config["generation_runtime"]:
            payload = runtime_sources[member["path"]].read_bytes()
            relative = "runtime/" + member["path"]
            components[relative] = (payload, int(member["mode"], 8))
            component_records.append(dict(member))
        manifest_document = {
            "schema": "nxruntime-generation-v2",
            "schema_version": 2,
            "generation_id": generation_id,
            "identity_basis": generation_identity_basis(
                config, nxsplash_sha256
            ),
            "components": component_records,
        }
    else:
        manifest_document = {
            "schema": "nxruntime-generation-v1",
            "schema_version": 1,
            "generation_id": generation_id,
            "identity_basis": generation_identity_basis(
                config, nxsplash_sha256
            ),
            "components": {
                relative: {
                    "sha256": hashlib.sha256(payload).hexdigest(),
                    "mode": "0%o" % mode,
                }
                for relative, (payload, mode) in components.items()
            },
        }
    generation_manifest_text = (
        json.dumps(manifest_document, indent=2, sort_keys=True) + "\n"
    )
    components_text = "".join(
        "%s  %s\n" % (hashlib.sha256(payload).hexdigest(), relative)
        for relative, (payload, _mode) in sorted(components.items())
    )
    components_v2_text = None
    identity_text = None
    identity_runtime_text = None
    if generation_v2:
        identity_basis = generation_identity_basis(config, nxsplash_sha256)
        identity_text = generation_identity_text(identity_basis)
        identity_runtime_text = generation_runtime_records(config)
        components_v2_text = "".join(
            "%s\t%s\t%s\t%s\n" % (
                record["role"], record["mode"], record["sha256"],
                record["path"],
            )
            for record in component_records
        )
    commit_target = generation_root / "commit"
    if commit_target.exists() or commit_target.is_symlink():
        # A committed generation is immutable. Reusing byte-identical content
        # is harmless; any collision is a release error and must never be
        # repaired by overwriting the previous rollback source.
        expected_files = {
            generation_root / "manifest.json": generation_manifest_text.encode(
                "utf-8"
            ),
            generation_root / "components.sha256": components_text.encode(
                "utf-8"
            ),
            commit_target: (generation_id + "\n").encode("ascii"),
        }
        if generation_v2:
            expected_files[generation_root / "format"] = (
                b"nxruntime-generation-v2\n"
            )
            expected_files[generation_root / "components.v2"] = (
                components_v2_text.encode("utf-8")
            )
            expected_files[generation_root / "identity.json"] = (
                identity_text.encode("utf-8")
            )
            expected_files[generation_root / "identity-runtime.v2"] = (
                identity_runtime_text.encode("utf-8")
            )
        expected_files.update({
            generation_files / relative: payload
            for relative, (payload, _mode) in components.items()
        })
        identical = not generation_root.is_symlink()
        for existing_path, expected_payload in expected_files.items():
            if (not identical or existing_path.is_symlink() or
                    not existing_path.is_file() or
                    existing_path.read_bytes() != expected_payload):
                identical = False
                break
        if identical:
            for relative, (_payload, expected_mode) in components.items():
                existing_path = generation_files / relative
                if stat.S_IMODE(existing_path.stat().st_mode) != expected_mode:
                    identical = False
                    break
        if not identical:
            raise ManifestError(
                "committed generation identity collision: %s" % generation_id
            )
    else:
        (generation_files / "launcher").mkdir(parents=True, exist_ok=True)
        for relative, (payload, mode) in components.items():
            target = generation_files / relative
            ensure_relative_parent(generation_files, relative)
            atomic_write_bytes(target, payload, mode, True)
        atomic_write(
            generation_root / "manifest.json",
            generation_manifest_text,
            0o644, True,
        )
        atomic_write(
            generation_root / "components.sha256",
            components_text,
            0o644, True,
        )
        if generation_v2:
            atomic_write(
                generation_root / "format",
                "nxruntime-generation-v2\n", 0o644, True,
            )
            atomic_write(
                generation_root / "components.v2",
                components_v2_text, 0o644, True,
            )
            atomic_write(
                generation_root / "identity.json",
                identity_text, 0o644, True,
            )
            atomic_write(
                generation_root / "identity-runtime.v2",
                identity_runtime_text, 0o644, True,
            )
        # The commit marker is published LAST: its presence attests completeness.
        atomic_write(commit_target, generation_id + "\n", 0o644, True)
    if generation_v2:
        # V4-REPACK-01: the visible seed. It carries the whole authenticated
        # closure so a personal rezip that drops every dotdir still installs.
        bundle_members = [
            ("format", b"nxruntime-generation-v2\n", 0o644),
            ("manifest.json", generation_manifest_text.encode("utf-8"), 0o644),
            ("components.sha256", components_text.encode("utf-8"), 0o644),
            ("components.v2", components_v2_text.encode("utf-8"), 0o644),
            ("identity.json", identity_text.encode("utf-8"), 0o644),
            ("identity-runtime.v2", identity_runtime_text.encode("utf-8"),
             0o644),
        ] + [
            ("files/" + relative, payload, mode)
            for relative, (payload, mode) in sorted(components.items())
        ]
        bundle_bytes = build_bundle_bytes(
            generation_id, config["id"], bundle_members
        )
        bundle_path = port_dir / bundle_name_for(generation_id)
        if bundle_path.is_symlink():
            raise ManifestError("bundle path is a symlink: %s" % bundle_path)
        if bundle_path.exists() and bundle_path.read_bytes() != bundle_bytes:
            raise ManifestError(
                "committed bundle identity collision: %s" % generation_id
            )
        atomic_write_bytes(bundle_path, bundle_bytes, 0o644, True)
        for member in config["generation_runtime"]:
            if member["role"] == "nxsplash":
                continue
            ensure_relative_parent(port_dir, member["path"])
            atomic_copy(
                runtime_sources[member["path"]], port_dir / member["path"],
                int(member["mode"], 8), force,
            )
    atomic_copy(nxsplash_source, nxsplash_target, 0o755, force)
    atomic_write(manifest_target, canonical_text, 0o644, force)
    atomic_write(launcher_target, launcher_text, 0o755, force)
    return config


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--runtime-root", type=Path,
        help=("root containing exact executable/library/hook paths pinned by "
              "generation_runtime"),
    )
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.output.is_symlink():
            raise ManifestError("output cannot be a symlink: %s" % args.output)
        resolved_output = args.output.resolve()
        config = generate(
            args.manifest.resolve(), resolved_output, args.force,
            args.runtime_root,
        )
    except (OSError, ValueError, json.JSONDecodeError, ManifestError) as error:
        print("nxbootstrap generator: %s" % error, file=sys.stderr)
        return 1
    upgrade = ""
    if config["input_schema_version"] != config["schema_version"]:
        upgrade = " (upgraded schema v%s -> v%s)" % (
            config["input_schema_version"], config["schema_version"])
    print("generated %s (%s) in %s%s" %
          (config["id"], config["architecture"], resolved_output,
           upgrade))
    return 0


if __name__ == "__main__":
    sys.exit(main())
