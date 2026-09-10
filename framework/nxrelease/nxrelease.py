#!/usr/bin/env python3
"""Build and verify deterministic universal PortMaster release archives.

This is deliberately a host-side tool.  It never executes a port or game data.
Python 3.8+ and GNU readelf are the only non-stdlib runtime requirements.
"""

from __future__ import print_function

import argparse
import ast
import copy
import ctypes
import errno
import hashlib
import importlib.util
import json
import os
import platform
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


TOOL_VERSION = "0.4.11"
SCHEMA_VERSION = 2
RELEASE_AUTHORITY_HUMAN = "human"
RELEASE_AUTHORITY_LOCK = "candidate-lock"
RELEASE_AUTHORITIES = frozenset((
    RELEASE_AUTHORITY_HUMAN,
    RELEASE_AUTHORITY_LOCK,
))
CANDIDATE_LOCK_SCHEMA = "nxrelease-candidate-lock-v1"
CANDIDATE_LOCK_VERSION = (0, 3, 8)
HISTORICAL_AUTHORITY_SCHEMA = "nxrelease-historical-authority-v1"
HISTORICAL_TOOL_VERSIONS = frozenset((
    "0.2.40", "0.2.41", "0.2.42", "0.2.43",
    "0.3.0", "0.3.1", "0.3.2", "0.3.3", "0.3.4", "0.3.5",
    "0.3.6", "0.3.7", "0.3.8", "0.3.9", "0.3.10", "0.3.11",
    "0.3.12", "0.3.13", "0.3.14", "0.3.15",
    "0.3.16", "0.3.17", "0.3.18", "0.3.19", "0.3.20", "0.3.21",
    "0.3.22", "0.3.23", "0.3.24", "0.3.25", "0.3.26",
))
PUBLIC_FINAL_RECEIPT_SCHEMA = "nxrelease-public-final-receipt-v1"
PUBLIC_FINAL_PROVENANCE_SCHEMA = "nxrelease-build-provenance-v1"
PUBLIC_GLIBC_MAX = "2.30"
PUBLIC_GLIBCXX_MAX = "3.4.25"
PUBLIC_CXXABI_MAX = "1.3.11"
ABI_POLICY_PATH = (
    Path(__file__).resolve().parents[1] / "nxabi" / "policy-v1.json"
)
# V4-03B: immutable public SDL floor mirrored the same way as the glibc
# ceilings; the versioned symbol->version authority lives in framework/nxabi
# and is consumed through nxabi's own strict parser, so nxabi and nxrelease
# always decide from exactly the same bytes.
SDL_PUBLIC_FLOOR = "2.0.4"
SDL_CORE_SONAME = "libSDL2-2.0.so.0"
NXABI_MODULE_PATH = (
    Path(__file__).resolve().parents[1] / "nxabi" / "nxabi.py"
)
PORTMASTER_ZIP_AUDITOR_PATH = (
    Path(__file__).resolve().parents[1] / "tests" / "audit-portmaster-zip.py"
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
    "nxsplash-linux",
    "nxextract",
    "nxextract-runner",
    "nxextract-recipe",
    "nxextract-runtime-env",
    "nxextract-ui-linux",
    "nxbootstrap-config",
    "portmaster-metadata",
    "portmaster-image",
    "license-notice",
    # V3-UPDATE-01: the committed generation store (.nxruntime/generations/**)
    # ships with the port so the launcher's pending->active/health machinery
    # exists on the device from the first install.
    "nxruntime-generation",
    "nxruntime-generation-linux",
    # V4-REPACK-01: the visible, content-addressed seed the launcher rebuilds
    # the local .nxruntime cache from when a personal rezip drops every dotdir.
    "nxruntime-seed",
))
ELF_KINDS = frozenset((
    "project-linux", "third-party-linux", "nxsplash-linux",
    "nxextract-ui-linux", "nxruntime-generation-linux",
))
LINUX_ELF_KINDS = frozenset((
    "project-linux", "third-party-linux", "nxsplash-linux",
    "nxextract-ui-linux", "nxruntime-generation-linux",
))
SINGLE_FILE_KINDS = frozenset((
    "launcher", "script", "project-linux", "third-party-linux",
    "nxsplash-linux",
    "nxextract", "nxextract-runner",
    "nxextract-recipe", "nxextract-runtime-env", "nxextract-ui-linux",
    "nxbootstrap-config",
    "portmaster-metadata", "portmaster-image", "license-notice",
    "nxruntime-seed",
    "nxruntime-generation-linux",
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
# Sonames a public port may leave UNBUNDLED because the PortMaster runtime
# guarantees them on every supported CFW (verified against the SDL2 family it
# ships). Anything a port NEEDs from `portmaster` that is not here must be
# bundled (provider=package) or static-linked -- otherwise it is a status-127
# "cannot open shared object" on the CFWs that lack it.
PORTMASTER_BASELINE_SONAMES = frozenset((
    "libSDL2-2.0.so.0", "libSDL2_mixer-2.0.so.0", "libSDL2_image-2.0.so.0",
    "libSDL2_ttf-2.0.so.0", "libSDL2_net-2.0.so.0", "libSDL2_gfx-1.0.so.0",
))
# Sonames the DEVICE FIRMWARE provides on every supported CFW: the loader/GL
# stack, the C++/GCC runtime, zlib, freetype and OpenAL. Curated from the
# dependencies of every shipping-and-working port. NOTABLY ABSENT: libzip.so.5
# -- muOS/Knulli carry it in /usr/lib but plain ArkOS does not, so a port that
# NEEDs it must bundle it (Magic Rampage shipped it as provider=portmaster and
# died with `libzip.so.5: cannot open shared object` on ArkOS).
FIRMWARE_BASELINE_SONAMES = frozenset((
    "ld-linux-aarch64.so.1", "ld-linux-armhf.so.3",
    "libEGL.so", "libEGL.so.1",
    "libGLESv2.so", "libGLESv2.so.2", "libGLESv1_CM.so", "libGLESv1_CM.so.1",
    "libgcc_s.so.1", "libstdc++.so.6",
    "libz.so.1", "libfreetype.so.6", "libopenal.so.1",
))
# Function-like names that are MACROS in the public SDL2 / SDL_mixer headers:
# the real exported symbol has a different name. If one of these appears as an
# UNDEFINED dynamic symbol, the header that defined the macro was missing at
# build time, and the loader will die with `undefined symbol: <name>` on any
# device whose library exports only the real symbol. Magic Rampage shipped with
# an UND `Mix_PlayChannel` (macro -> Mix_PlayChannelTimed) and crashed on the
# first sound (spruce/Mali-G52, status 127). This gate refuses that at build
# time. Values map to the real symbol the caller should use instead.
MACRO_ONLY_DYNSYMS = {
    "Mix_LoadWAV": "Mix_LoadWAV_RW",
    "Mix_PlayChannel": "Mix_PlayChannelTimed",
    "Mix_FadeInChannel": "Mix_FadeInChannelTimed",
    "SDL_LoadBMP": "SDL_LoadBMP_RW",
    "SDL_SaveBMP": "SDL_SaveBMP_RW",
    "SDL_LoadWAV": "SDL_LoadWAV_RW",
    "SDL_BlitSurface": "SDL_UpperBlit",
    "SDL_BlitScaled": "SDL_UpperBlitScaled",
    "SDL_GameControllerAddMappingsFromFile": "SDL_GameControllerAddMappingsFromRW",
}
EXCEPTION_RULES = frozenset(("adaptive-driver", "supervised-child"))
NXPORT_SCHEMA_VERSIONS = (2, 3)
NXPORT_CAPABILITY_RE = re.compile(
    r"^(?:host|graphics|audio|input)\.[a-z0-9][a-z0-9.-]{0,62}$"
)
NXPORT_QUIRK_RE = re.compile(
    r"^(?:adapter|engine|game)\.[a-z0-9][a-z0-9._-]{0,62}$"
)
NXPORT_LANGUAGE_RE = re.compile(r"^[a-z]{2}(?:-[a-z0-9]{2,8})?$")
HOOK_CLOSURE_TEXT_SUFFIXES = frozenset((
    ".bash", ".cfg", ".ini", ".js", ".json", ".lua", ".pl", ".py",
    ".sh", ".toml", ".yaml", ".yml",
))
HOOK_CLOSURE_MAX_FILES = 256
HOOK_CLOSURE_MAX_BYTES = 16 * 1024 * 1024
HOOK_CLOSURE_JSON_SNIFF_MAX_BYTES = HOOK_CLOSURE_MAX_BYTES
HOOK_GENERIC_FALLBACK_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
HOOK_REJECTING_FALLBACK_TOKENS = frozenset((
    "abort", "error", "fail", "failure", "incompatible", "nenhum", "none",
    "reject", "rejection",
))
CAPABILITY_REGISTRY_PATH = (
    Path(__file__).resolve().parents[1] / "nxcompat" /
    "capabilities-v1.json"
)
QUIRK_REGISTRY_PATH = (
    Path(__file__).resolve().parents[1] / "nxcompat" /
    "quirk-registry-v1.json"
)
NXBOOTSTRAP_GENERATOR_PATH = (
    Path(__file__).resolve().parents[1] / "nxbootstrap" /
    "tools" / "generate-port.py"
)
NXBOOTSTRAP_REQUIRED_VERSION = "0.8.4"
NXSPLASH_RUNTIME_NAME = "nxsplash-nextos"
NXSPLASH_REQUIRED_VERSION = "0.1.2"
NXSPLASH_BOOTSTRAP_FLOOR = (0, 6, 9)
NXEXTRACT_REQUIRED_VERSION = "1.3.0"
NXEXTRACT_UI_REQUIRED_VERSION = "1.2.16"
NXGENERATOR_REQUIRED_VERSION = "0.4.5"
NXEXTRACT_ROOT = (
    Path(__file__).resolve().parents[2] /
    "suportando_outros_devices" / "extrator-universal"
)
NXEXTRACT_UI_MANIFEST_PATH = (
    NXEXTRACT_ROOT / "ui" / "release" / "manifest-v1.json"
)
# Identidades de motor aceitas num pacote. Havia exatamente UMA, cravada aqui,
# e era isso que tornava impossivel subir o NXExtract "por opt-in": no instante
# do bump, todo port que pinava a versao anterior passava a reprovar. Agora a
# lista mora num registro versionado, com a canonica marcada -- e uma entrada
# nova so' entra no momento do bump, registrando a identidade que esta' saindo.
NXEXTRACT_ENGINES_PATH = (
    Path(__file__).resolve().parent / "nxextract-engines-v1.json"
)
APKCOMPAT_PATH = (
    Path(__file__).resolve().parents[1] / "contracts" / "apkcompat" /
    "apkcompat.py"
)


def _load_apkcompat():
    spec = importlib.util.spec_from_file_location(
        "nx_apkcompat", APKCOMPAT_PATH
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load the canonical apkcompat contract")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


APKCOMPAT = _load_apkcompat()


# V4-04B adapter (nxrelease 0.3.25): the structural scanner is no longer an
# isolated foundation.  For the payload languages it actually understands
# (``.py``/``.pyi``/``.json``) nxscan decides, so a Python type annotation or a
# JSON key is judged by the grammar instead of by a byte regex.  Every other
# type -- and every payload nxscan cannot structurally decide -- keeps falling
# back to the historical fail-closed regex authority below.
NXSCAN_PATH = Path(__file__).resolve().parent / "nxscan.py"


def _load_nxscan():
    spec = importlib.util.spec_from_file_location("nx_structural_scan",
                                                  NXSCAN_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load the structural scanner nxscan")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if getattr(module, "SCAN_SCHEMA", None) != "nx-structural-scan/1":
        raise RuntimeError("nxscan has an unknown structural scan schema")
    return module


NXSCAN = _load_nxscan()



def _load_nxextract_engines():
    with NXEXTRACT_ENGINES_PATH.open("r", encoding="utf-8") as stream:
        document = json.load(stream)
    if document.get("schema") != "org.nextos.nxextract.engine-identities":
        raise RuntimeError("nxextract engine registry has an unknown schema")
    engines = document.get("engines")
    canonical = document.get("canonical")
    if not isinstance(engines, dict) or canonical not in engines:
        raise RuntimeError("nxextract engine registry is malformed")
    return canonical, engines


NXEXTRACT_CANONICAL_VERSION, NXEXTRACT_ENGINES = _load_nxextract_engines()
NXEXTRACT_SUPPORTED_VERSIONS = tuple(sorted(NXEXTRACT_ENGINES))
NXEXTRACT_ENGINE_SHA256 = (
    NXEXTRACT_ENGINES[NXEXTRACT_CANONICAL_VERSION]["engine_sha256"]
)
NXEXTRACT_RUNNER_SHA256 = (
    NXEXTRACT_ENGINES[NXEXTRACT_CANONICAL_VERSION]["runner_sha256"]
)
NXEXTRACT_RUNTIME_ENV_SHA256 = (
    NXEXTRACT_ENGINES[NXEXTRACT_CANONICAL_VERSION]["runtime_env_sha256"]
)
_NXBOOTSTRAP_GENERATOR = None


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
DOTNET_DEPS_VERSION_CONTEXT_RE = re.compile(
    rb'"(?:assemblyVersion|fileVersion)"\s*:\s*"\s*$'
)
SECRET_NAME_PATTERN = (
    rb"(?:api[_-]?key|secret|passwd|password|bearer|credential|private[_-]?key)"
)
SECRET_VALUE_PATTERN = rb"[A-Za-z0-9/_+\-]{8,}"
# Keep assignments separate from mapping-style ``key: value`` syntax.  In a
# Python source file, a colon after a name is a type annotation, not a value:
# generated Unity bindings legitimately contain declarations such as
# ``m_VCPassword: Optional[str] = None``.  An annotated assignment with a real
# literal is still rejected by the second expression below.
SECRET_LITERAL_RE = re.compile(
    rb"(?i)" + SECRET_NAME_PATTERN + rb"\s*=\s*" + SECRET_VALUE_PATTERN
)
PYTHON_ANNOTATED_SECRET_LITERAL_RE = re.compile(
    rb"(?i)" + SECRET_NAME_PATTERN +
    rb"\s*:\s*[^\r\n=]{1,256}\s*=\s*" + SECRET_VALUE_PATTERN
)
MAPPING_SECRET_LITERAL_RE = re.compile(
    rb"(?i)" + SECRET_NAME_PATTERN + rb"\s*:\s*" + SECRET_VALUE_PATTERN
)
HOST_LITERAL_RE = re.compile(
    rb"(?i)\b(?:host|hostname)\s*[:=]\s*[A-Za-z0-9][A-Za-z0-9._-]{1,127}\b"
)
ADVOCACY_RE = re.compile(
    rb"(?i)\b(?:donate|donations?|sponsor(?:ing|ship|s)?|patreon|paypal|"
    rb"ko-?fi|opencollective|liberapay|buymeacoffee|buy-me-a-coffee|funding)\b"
)
NXBUNDLE_MAGIC = b"NXBUNDLE1\n"
NXBUNDLE_MEMBER_RE = re.compile(
    rb"^M\t(0644|0755)\t([0-9a-f]{64})\t([0-9]+)\t([0-9]+)\t([^\t\r\n]+)\n$"
)
# The device parser caps the complete header at 4096 lines: four fixed lines,
# one END line and at most 4091 member records.
NXBUNDLE_MAX_MEMBERS = 4091
NXBUNDLE_MAX_HEADER_LINE = 8192
FORCED_DRIVER_RE = re.compile(
    r"^[ \t]*(?!#)(?:export[ \t]+)?(?:SDL_(?:VIDEO|AUDIO)DRIVER|ALSOFT_DRIVERS)=",
    re.MULTILINE | re.IGNORECASE,
)
DETACHED_RE = re.compile(r"(^|[; \t])(setsid|nohup)(?=[ \t]|$)")
# Regra #9b da casa: port NUNCA cria/gerencia swap nem mexe em cache de pagina.
# Handhelds de 1 GB ja tem a politica de swap do firmware; um port que a altera
# quebra o device inteiro, nao so a si mesmo.
SWAP_RE = re.compile(
    r"(^|[;&| \t])(mkswap|swapon|swapoff|zramctl)(?=[ \t]|$)"
    r"|/swapfile\b|drop_caches",
    re.MULTILINE,
)
FRONTEND_RE = re.compile(
    r"(^|[; \t])(systemctl|killall|pkill)(?=[ \t]).*(?:emustation|emulationstation)",
    re.IGNORECASE,
)
SHELL_KINDS = frozenset((
    "launcher", "script", "nxextract-runner", "nxextract-runtime-env",
))
SHELL_INTERPRETER_NAMES = frozenset((
    "sh", "bash", "dash", "ash", "ksh", "mksh", "zsh", "hush",
))
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


SETTINGS_VIDEO_KEYS = (
    "video.authority", "video.output_size", "video.aspect", "video.filter",
    "video.invalid_policy",
)
SETTINGS_VIDEO_ENUMS = {
    "video.authority": ("nextos", "engine", "synchronized"),
    "video.output_size": ("auto", "display", "640x480", "1280x720", "1920x1080"),
    "video.aspect": ("auto", "engine", "preserve", "stretch", "crop", "integer"),
    "video.filter": ("engine", "nearest", "linear"),
    "video.invalid_policy": ("fail_closed", "last_known_good", "package_default"),
}


def settings_video_value_ok(key, value):
    """Mirror of nxcompat_settings_video_value_ok (NEXTOS_SETTINGS/2)."""
    allowed = SETTINGS_VIDEO_ENUMS.get(key)
    if allowed is None:
        return False
    if value in allowed:
        return True
    if key == "video.output_size":
        match = re.fullmatch(r"([1-9][0-9]{0,3})x([1-9][0-9]{0,3})", value)
        return bool(match) and int(match.group(1)) <= 8192 and \
            int(match.group(2)) <= 8192
    return False


def validate_owner_runtime_contract(records, config):
    """V5 7A.1/FV6: owner files are SEEDS in the package, never live copies.

    Under nxport.owner_runtime="1": defaults/port-env.sh is a pinned 0644
    payload; the live <port>/port-env.sh is never packaged; port-env.sh is
    never a generation member (the healing closure); the sealed helper
    adapter-env.sh IS a generation member; OWNERSHIP.json (nx-ownership/1)
    is present and consistent; every owner-native path it declares stays out
    of the closure; no `.new` and no live typed owner file is packaged.
    Without the opt-in only the OWNERSHIP.json shape is checked when present.
    """
    port_dir = config["port_dir"]
    by_target = {item["target"]: item for item in records}
    nxport = config.get("nxport_manifest")
    if not isinstance(nxport, dict):
        return
    runtime = nxport.get("generation_runtime")
    runtime_paths = {}
    if isinstance(runtime, list):
        for member in runtime:
            if isinstance(member, dict) and isinstance(member.get("path"), str):
                runtime_paths[member["path"]] = member.get("role")
    ownership_record = by_target.get(port_dir + "/OWNERSHIP.json")
    ownership = None
    if ownership_record is not None:
        if ownership_record.get("kind") != "payload" or \
                ownership_record.get("mode") != 0o644:
            fail("OWNERSHIP.json must be a 0644 payload")
        ownership = load_json(ownership_record["actual_path"], "OWNERSHIP.json")
        if not isinstance(ownership, dict) or \
                ownership.get("schema") != "nx-ownership/1" or \
                not isinstance(ownership.get("paths"), list):
            fail("OWNERSHIP.json is not nx-ownership/1")
        for entry in ownership["paths"]:
            entry = require_object(entry, "OWNERSHIP.json path")
            path = entry.get("path")
            klass = entry.get("class")
            if klass not in ("owner-seeded", "sealed-runtime", "owner-native"):
                fail("OWNERSHIP.json class %r is unknown" % klass)
            if klass in ("owner-seeded", "owner-native"):
                if entry.get("healed") is not False:
                    fail("OWNERSHIP.json: owner path %s must declare "
                         "healed=false" % path)
                live = entry.get("live") if klass == "owner-seeded" else path
                if isinstance(live, str) and live in runtime_paths:
                    fail("owner path %s entered the generation closure "
                         "(would be healed)" % live)
                if klass == "owner-seeded" and isinstance(live, str) and \
                        port_dir + "/" + live in by_target:
                    fail("live owner file %s must not be packaged (seed only)"
                         % live)
            if klass == "sealed-runtime" and path not in runtime_paths:
                fail("OWNERSHIP.json: sealed path %s is not a generation member"
                     % path)
    for suffix in (".new",):
        for target in by_target:
            if target.startswith(port_dir + "/") and target.endswith(suffix):
                fail("package must not ship %s (owner-side artefact)" % target)
    if nxport.get("owner_runtime") != "1":
        return
    if ownership is None:
        fail("owner_runtime requires OWNERSHIP.json in the package")
    seed = by_target.get(port_dir + "/defaults/port-env.sh")
    if seed is None or seed.get("kind") != "payload" or \
            seed.get("mode") != 0o644:
        fail("owner_runtime requires defaults/port-env.sh as a 0644 payload seed")
    if port_dir + "/port-env.sh" in by_target:
        fail("owner_runtime: the live port-env.sh must not be packaged "
             "(seed lives in defaults/)")
    if "port-env.sh" in runtime_paths:
        fail("owner_runtime: port-env.sh cannot be a generation member "
             "(it would be healed)")
    if runtime_paths.get("adapter-env.sh") != "runtime-hook":
        fail("owner_runtime: adapter-env.sh must be the sealed runtime-hook "
             "generation member")
    if "port-env.sh" in (nxport.get("required_files") or []):
        fail("owner_runtime: port-env.sh cannot be a required file")
    seeded = {e.get("path"): e for e in ownership["paths"]
              if isinstance(e, dict)}
    entry = seeded.get("defaults/port-env.sh")
    if entry is None or entry.get("class") != "owner-seeded" or \
            entry.get("live") != "port-env.sh":
        fail("OWNERSHIP.json must map defaults/port-env.sh -> port-env.sh "
             "as owner-seeded")
    sealed = seeded.get("adapter-env.sh")
    if sealed is None or sealed.get("class") != "sealed-runtime":
        fail("OWNERSHIP.json must declare adapter-env.sh as sealed-runtime")
    # KOTOR lesson, applied to the seed and the sealed helper: every
    # $GAMEDIR file they test or assign must be staged.
    for hook_target in (port_dir + "/defaults/port-env.sh",
                        port_dir + "/adapter-env.sh"):
        hook = by_target.get(hook_target)
        if hook is None:
            continue
        source = hook.get("actual_path") or hook.get("source")
        text = read_small_text(source, hook_target)
        referenced = set()
        for line in active_shell_text(text).splitlines():
            if not re.search(r"(?:\[\s+-[fxe]\s|BIN(?:_PRELOAD)?=)", line):
                continue
            referenced.update(re.findall(
                r"\$(?:\{)?GAMEDIR(?:\})?/([A-Za-z0-9][A-Za-z0-9._/-]*)", line))
        for relative in sorted(referenced):
            if "*" in relative or "$" in relative:
                continue
            if port_dir + "/" + relative not in by_target and \
                    relative not in ("port-env.sh", "NEXTOSSETTINGS.txt",
                                     "NEXTOSCONTROLLERS.gptk"):
                fail("%s references %s/%s which is not staged" % (
                    hook_target, port_dir, relative))


def fail(message):
    raise ReleaseError(message)

PROMPT_CAPTURE_SCHEMA = "nx-prompt-capture-proof/2"
PROMPT_CAPTURE_GLYPH_TOKENS = frozenset({
    "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "SELECT",
    "L1", "R1", "L2", "R2",
})


def validate_prompt_capture_proof(config):
    """V5 (M1c item 2): uma prova de prompt só vale em `/2`.

    O `/1` aprovou `PRESS [10] TO BEGIN` no FP2 com `result: PASS` e
    `forbidden: []` (achado E13): o regex conhecia `Joystick Button N`,
    `Button N` e `Axis ±N`, mas o jogo desenha o PRÓPRIO ícone com o ordinal
    cru DENTRO, e o OCR de tela inteira nem enxerga o dígito. Ou seja: o
    esquema `/1` é capaz de aprovar exatamente o defeito que deveria pegar,
    então ele não é aceito como prova -- é recusado por identidade, não por
    conteúdo.

    Em `/2` o receipt precisa provar as três coisas que faltavam:
      * cada REGIÃO declarada foi lida isolada (é lá que o dígito aparece);
      * PASS exige pelo menos uma região com glyph ESPERADO reconhecido --
        sem expectativa declarada o veredito é INCONCLUSIVE, nunca PASS por
        silêncio;
      * qualquer achado `raw-ordinal-in-region` derruba o PASS.
    """
    source_root = config.get("source_root")
    if source_root is None:
        return
    proofs = source_root / "proofs"
    if not proofs.is_dir() or proofs.is_symlink():
        return
    checked = 0
    for receipt_path in sorted(proofs.glob("*/receipt.json")):
        if receipt_path.is_symlink() or not receipt_path.is_file():
            continue
        document = load_json(receipt_path, "prompt capture receipt")
        if not isinstance(document, dict):
            continue
        schema = document.get("schema")
        if not isinstance(schema, str) or \
                not schema.startswith("nx-prompt-capture-proof/"):
            continue
        label = receipt_path.parent.name
        if schema != PROMPT_CAPTURE_SCHEMA:
            fail("prompt capture proof %s is %s: only %s is accepted (the /1 "
                 "scheme approved a raw ordinal drawn inside the game's own "
                 "icon, so it cannot certify the very defect it must catch)"
                 % (label, schema, PROMPT_CAPTURE_SCHEMA))
        result = document.get("result")
        if result not in ("PASS", "NOT_PASS", "FAIL", "INCONCLUSIVE"):
            fail("prompt capture proof %s has no usable result" % label)
        captures = document.get("captures")
        if not isinstance(captures, list) or not captures:
            fail("prompt capture proof %s carries no capture" % label)
        glyph_met = False
        for capture in captures:
            if not isinstance(capture, dict):
                fail("prompt capture proof %s has a malformed capture" % label)
            regions = capture.get("regions")
            if not isinstance(regions, list):
                fail("prompt capture proof %s capture has no region list "
                     "(full-frame OCR alone is what the /1 did)" % label)
            for region in regions:
                if not isinstance(region, dict):
                    fail("prompt capture proof %s has a malformed region"
                         % label)
                raw = region.get("raw_ordinal_tokens")
                if raw and result == "PASS":
                    fail("prompt capture proof %s is PASS while region %r "
                         "shows the raw ordinal(s) %s" % (
                             label, region.get("name"), raw))
                expect = region.get("expect")
                if expect is None:
                    continue
                if not isinstance(expect, str) or \
                        expect.upper() not in PROMPT_CAPTURE_GLYPH_TOKENS:
                    fail("prompt capture proof %s declares the unknown glyph "
                         "%r" % (label, expect))
                if region.get("glyph_met") is True:
                    glyph_met = True
        if result == "PASS" and not glyph_met:
            fail("prompt capture proof %s is PASS with no recognised expected "
                 "glyph: a proof never passes by silence" % label)
        checked += 1
    config["prompt_capture_proofs_checked"] = checked


def validate_controls_closure(records, config):
    """V5 (M1c item 1.2/1.3): a closure esperada acompanha o owner gerado.

    O harness de host do Tearscape derivava a expectativa dentro do port e
    ficou no loader V1-V3 quando o owner virou schema 4: a prova toda foi a
    vermelho em `NXI1006` e o laço morreu lendo o rabo da cascata. A regra
    8.1 diz que o esperado nasce do projeto -- o nxgenerator emite
    `CONTROLS-CLOSURE.json` --, e aqui se cobra que ele exista, seja
    `nx-controls-closure/1`, nomeie o port certo e não esteja vazio quando o
    port declara controles schema 4.
    """
    port_dir = config["port_dir"]
    by_target = {item["target"]: item for item in records}
    record = by_target.get(port_dir + "/CONTROLS-CLOSURE.json")
    nxport = config.get("nxport_manifest")
    schema = None
    if isinstance(nxport, dict):
        controls = nxport.get("controls")
        if isinstance(controls, dict):
            schema = controls.get("schema")
    if record is None:
        if schema == 4:
            fail("controls schema 4 requires CONTROLS-CLOSURE.json (the host "
                 "expectation must come from the project, never from the map "
                 "under test)")
        return
    if record.get("kind") != "payload" or record.get("mode") != 0o644:
        fail("CONTROLS-CLOSURE.json must be a 0644 payload")
    document = load_json(record["actual_path"], "CONTROLS-CLOSURE.json")
    if not isinstance(document, dict) or \
            document.get("schema") != "nx-controls-closure/1":
        fail("CONTROLS-CLOSURE.json is not nx-controls-closure/1")
    if document.get("port_id") != config["package_id"] and \
            document.get("port_id") != port_dir:
        fail("CONTROLS-CLOSURE.json names port %r, not this port"
             % document.get("port_id"))
    contexts = document.get("declared_contexts")
    if not isinstance(contexts, list) or not contexts:
        fail("CONTROLS-CLOSURE.json declares no context")
    cases = document.get("cases")
    if not isinstance(cases, list):
        fail("CONTROLS-CLOSURE.json has no case list")
    if not cases:
        # 0.4.3 (review 2, F9): an empty closure proves nothing -- it must
        # name at least one delivery per declared context.
        fail("CONTROLS-CLOSURE.json has an empty case list")
    for case in cases:
        if not isinstance(case, dict) or set(case) != {
                "context", "control", "event", "decision", "action", "sink",
                "delivery_count"}:
            fail("CONTROLS-CLOSURE.json has a malformed case")
        if case["context"] not in contexts:
            fail("CONTROLS-CLOSURE.json case names the undeclared context %r"
                 % case["context"])
        if case["decision"] != "ACTION" or case["delivery_count"] != 1:
            fail("CONTROLS-CLOSURE.json case %s.%s is not one ACTION delivery"
                 % (case["context"], case["control"]))
        if case["event"] not in ("press", "axis", "motion"):
            fail("CONTROLS-CLOSURE.json case %s.%s has an unknown event"
                 % (case["context"], case["control"]))



NXRELEASE_FRAMEWORK_DIR = Path(__file__).resolve().parents[1]
# NXExtract is the one framework component that does not live under
# framework/<name> (framework_pin.py carries the same exception).
VENDOR_COMPONENT_SOURCES = {
    "nxextract": Path("suportando_outros_devices") / "extrator-universal",
}


def vendor_component_source(name):
    """Absolute path of the framework tree a vendored component copies."""
    relative = VENDOR_COMPONENT_SOURCES.get(name)
    if relative is not None:
        return NXRELEASE_FRAMEWORK_DIR.parent / relative
    return NXRELEASE_FRAMEWORK_DIR / name


_PINNED_TREE_CACHE = {}


def _framework_repo_root():
    """Top level of the git work tree that holds framework/ (worktrees have a
    `.git` FILE, so this asks git instead of looking for a directory)."""
    import subprocess
    try:
        out = subprocess.run(["git", "-C", str(NXRELEASE_FRAMEWORK_DIR),
                              "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    top = out.strip()
    return Path(top) if top else None


def pinned_tree_blobs(commit, component_relative):
    """{relative path: sha256 of the blob} of `component_relative` (e.g.
    framework/nxinput) at `commit`, read from the git object store -- the
    tree the pin DECLARES, never the checkout that happens to run the gate.
    Returns None when the commit is unknown to this repository."""
    import subprocess
    key = (commit, component_relative)
    if key in _PINNED_TREE_CACHE:
        return _PINNED_TREE_CACHE[key]
    repo = _framework_repo_root()
    if repo is None or not commit:
        _PINNED_TREE_CACHE[key] = None
        return None
    try:
        listing = subprocess.run(
            ["git", "-C", str(repo), "ls-tree", "-r", "-z",
             "%s:%s" % (commit, component_relative)],
            capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        _PINNED_TREE_CACHE[key] = None
        return None
    blobs = {}
    for record in listing.split(b"\0"):
        if not record:
            continue
        meta, _, path = record.partition(b"\t")
        parts = meta.split()
        if len(parts) < 3 or parts[1] != b"blob":
            continue
        blobs[path.decode("utf-8")] = parts[2].decode("ascii")
    # git object ids are sha1; hash the CONTENT with sha256 lazily
    _PINNED_TREE_CACHE[key] = {"repo": repo, "commit": commit,
                               "component": component_relative,
                               "oids": blobs, "sha256": {}}
    return _PINNED_TREE_CACHE[key]


def pinned_blob_sha256(tree, relative):
    """sha256 of one file of a pinned tree (None when absent)."""
    import subprocess
    if tree is None or relative not in tree["oids"]:
        return None
    if relative in tree["sha256"]:
        return tree["sha256"][relative]
    try:
        data = subprocess.run(
            ["git", "-C", str(tree["repo"]), "cat-file", "blob",
             tree["oids"][relative]],
            capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    digest = hashlib.sha256(data).hexdigest()
    tree["sha256"][relative] = digest
    return digest


def vendor_component_relative(name):
    relative = VENDOR_COMPONENT_SOURCES.get(name)
    if relative is not None:
        return relative.as_posix()
    return "framework/%s" % name


# Framework copies that live OUTSIDE `vendor/` (the Tearscape engine keeps
# the nxinput sources under src/nxinput/): any file of the port whose
# basename is a source of these components is a vendored copy and must be
# byte-identical to the pinned tree. Component trees are searched under
# these subdirectories only (tools/docs/tests never get vendored).
VENDOR_LOOSE_COMPONENTS = ("nxinput", "nxcompat", "nxgl", "nxbootstrap")
VENDOR_LOOSE_SUBDIRS = ("src", "include", "engine-glue", "adapters", "tools")
VENDOR_LOOSE_SKIP_DIRS = {"vendor", ".git", "build", "stage", "payload",
                          "gamedata", "gamedata_staging", "byo", "work",
                          ".build", "dump", "extracted", "shots"}


def validate_loose_vendor_copies(config, pinned_commits, checked):
    """0.4.3 (review 2, F5): copies of framework sources outside vendor/."""
    source_root = config.get("source_root")
    if source_root is None:
        return checked
    index = {}
    for name in VENDOR_LOOSE_COMPONENTS:
        commit = pinned_commits.get(name)
        if not commit:
            continue
        tree = pinned_tree_blobs(commit, vendor_component_relative(name))
        if tree is None:
            continue
        for relative in tree["oids"]:
            top = relative.split("/", 1)[0]
            if top not in VENDOR_LOOSE_SUBDIRS:
                continue
            base = relative.rsplit("/", 1)[-1]
            if not base.endswith((".c", ".h", ".cpp", ".hpp")):
                continue
            index.setdefault(base, []).append((name, tree, relative))
    if not index:
        return checked
    for present in sorted(source_root.rglob("*")):
        if present.is_dir() or present.is_symlink():
            continue
        rel_parts = present.relative_to(source_root).parts
        if any(part in VENDOR_LOOSE_SKIP_DIRS for part in rel_parts[:-1]):
            continue
        candidates = index.get(present.name)
        if not candidates:
            continue
        actual = sha256_file(present)
        matches = [c for c in candidates
                   if pinned_blob_sha256(c[1], c[2]) == actual]
        if matches:
            checked += 1
            continue
        fail("%s copies the framework source %s but differs from the pinned "
             "tree of %s (commit %s): re-vendor before claiming this pin"
             % (present.relative_to(source_root).as_posix(),
                present.name, candidates[0][0],
                (pinned_commits.get(candidates[0][0]) or "?")[:12]))
    return checked


# 0.4.3 (review 2, F7): the "too late to stage" predicate belongs to the seam
# (nxc6_stage_before_init keys on JOYSTICK|GAMECONTROLLER); a port that
# supplies its own SDL_WasInit(0) / SDL_INIT_EVERYTHING predicate refuses to
# stage for every engine that brings VIDEO up first -- the very defect
# nxinput 0.11.3 fixed in the glue, which a port-side copy silently keeps.
PORT_INIT_PREDICATE_PATTERN = re.compile(
    r"SDL_WasInit\s*\(\s*(0[uU]?|SDL_INIT_EVERYTHING)\s*\)")
PORT_INIT_PREDICATE_SKIP_DIRS = {"vendor", ".git", "build", "stage", "payload",
                                 "gamedata", "gamedata_staging", "byo", "work",
                                 ".build", "dump", "extracted", "shots", "tests"}


def validate_port_init_predicate(config):
    source_root = config.get("source_root")
    if source_root is None:
        return
    offenders = []
    for present in sorted(source_root.rglob("*")):
        if present.is_dir() or present.is_symlink():
            continue
        if present.suffix not in (".c", ".cc", ".cpp", ".h", ".hpp", ".patch"):
            continue
        parts = present.relative_to(source_root).parts
        if any(part in PORT_INIT_PREDICATE_SKIP_DIRS for part in parts[:-1]):
            continue
        try:
            text = present.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if PORT_INIT_PREDICATE_PATTERN.search(text):
            offenders.append(present.relative_to(source_root).as_posix())
    if offenders:
        fail("port source supplies its own SDL init predicate (SDL_WasInit(0)/"
             "SDL_INIT_EVERYTHING) in %s: the staging boundary is the seam's "
             "(nxc6_stage_before_init keys on JOYSTICK|GAMECONTROLLER); the "
             "port must call it, never carry its own copy" % ", ".join(offenders))


def validate_vendor_pin_contract(config):
    """V5 (auditoria 03/09, item E3/d): vendor BYTE A BYTE contra o pin.

    Um port que compila `vendor/<componente>/` embute uma CÓPIA do framework.
    Até aqui o único conferidor era o próprio `PINS.json`, que lista os
    digests dos arquivos vendorizados -- e é recalculado junto com a cópia.
    Ou seja: um vendor velho é internamente coerente e passa. Foi exatamente
    o que aconteceu em 03/09: FP2, Nameless Cat e Blossom declararam pin no
    commit congelado do framework carregando `nxinput/src/nxinput_provider.c`
    de ANTES do provider estático (217 linhas contra 240), e o "ELF
    byte-idêntico" comemorado era consequência de NÃO revendorizar. O pin
    mentia e nada reprovava.

    A verdade não é o PINS.json: é a ÁRVORE DO COMPONENTE. `validate` roda
    dentro do repositório no commit que o `FRAMEWORK-PIN.json` declara, então
    cada arquivo vendorizado é comparado byte a byte com
    `framework/<componente>/<caminho>` e qualquer diferença reprova, nomeando
    o arquivo. Um `vendor/<nome>/` sem `PINS.json` só é aceito quando o nome
    NÃO é de um componente do framework (SDL2 upstream, fdk-aac e afins);
    caso contrário a rota de fuga seria simplesmente apagar o PINS.json.
    """
    # A SOURCE-side contract: `vendor/` is build input and is never staged or
    # packaged. verify_stage/verify_archive audit a record set that has no
    # source tree, so they carry no source_root and this check does not apply
    # there -- `validate` and `build` (which validates the sources first) are
    # the boundaries that enforce it.
    source_root = config.get("source_root")
    if source_root is None:
        return
    vendor_root = source_root / "vendor"
    pinned_commits = {}
    pin_document = source_root / "FRAMEWORK-PIN.json"
    if pin_document.is_file() and not pin_document.is_symlink():
        document = load_json(pin_document, "FRAMEWORK-PIN.json")
        components = document.get("components") if isinstance(document, dict) \
            else None
        if isinstance(components, dict):
            for name, value in components.items():
                if isinstance(value, dict):
                    pinned_commits[name] = value.get("commit")
                else:
                    pinned_commits[name] = document.get("framework_commit")
    checked = 0
    if vendor_root.is_dir() and not vendor_root.is_symlink():
        for entry in sorted(vendor_root.iterdir()):
            if entry.is_symlink() or not entry.is_dir():
                continue
            name = entry.name
            component_source = vendor_component_source(name)
            pins_path = entry / "PINS.json"
            is_component = component_source.is_dir()
            if not pins_path.is_file() or pins_path.is_symlink():
                if is_component:
                    fail("vendor/%s copies the framework component %s but carries "
                         "no PINS.json: a vendored component is always pinned"
                         % (name, name))
                continue
            if not is_component:
                fail("vendor/%s declares component pins but %s is not a framework "
                     "component" % (name, name))
            # 0.4.3 (review 2, F5): the truth is the tree of the COMMIT the
            # FRAMEWORK-PIN.json declares, read from the git object store --
            # never the checkout that happens to run the gate (a checkout
            # ahead of the pin passed a stale vendor; a checkout behind it
            # refused a correct one). No pin, no git => fail closed.
            commit = pinned_commits.get(name)
            if not commit:
                fail("vendor/%s is vendored but FRAMEWORK-PIN.json pins no commit "
                     "for the component %s" % (name, name))
            tree = pinned_tree_blobs(commit, vendor_component_relative(name))
            if tree is None:
                fail("vendor/%s: the pinned commit %s of %s is not in this "
                     "repository (or git is unavailable); the vendor cannot be "
                     "verified against the tree it declares"
                     % (name, str(commit)[:12], name))
            pins = load_json(pins_path, "vendor/%s/PINS.json" % name)
            files = pins.get("files") if isinstance(pins, dict) else None
            if not isinstance(files, dict) or not files:
                fail("vendor/%s/PINS.json declares no files" % name)
            declared = set()
            for relative, pinned in sorted(files.items()):
                if not isinstance(relative, str) or not isinstance(pinned, str):
                    fail("vendor/%s/PINS.json has a malformed entry" % name)
                safe_relative(relative, "vendor/%s/PINS.json entry" % name)
                declared.add(relative)
                copied = entry / relative
                if copied.is_symlink() or not copied.is_file():
                    fail("vendor/%s/%s is pinned but missing" % (name, relative))
                actual = sha256_file(copied)
                if actual != pinned:
                    fail("vendor/%s/%s does not match its own PINS.json entry"
                         % (name, relative))
                upstream = pinned_blob_sha256(tree, relative)
                if upstream is None:
                    fail("vendor/%s/%s has no counterpart in the pinned "
                         "framework tree (commit %s)"
                         % (name, relative, str(commit)[:12]))
                if upstream != actual:
                    fail("vendor/%s/%s differs from the pinned framework tree "
                         "(PINS.json says framework_commit=%s, FRAMEWORK-PIN.json "
                         "says %s): re-vendor before claiming this pin"
                         % (name, relative,
                            (pins.get("framework_commit") or "?")[:12],
                            str(commit)[:12]))
                checked += 1
            for present in sorted(entry.rglob("*")):
                if present.is_dir() or present.is_symlink():
                    continue
                relative = present.relative_to(entry).as_posix()
                if relative == "PINS.json" or relative in declared:
                    continue
                fail("vendor/%s/%s is present but undeclared in PINS.json"
                     % (name, relative))
    checked = validate_loose_vendor_copies(config, pinned_commits, checked)
    config["vendor_files_checked"] = checked




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


def parse_json_strict(payload, context):
    if payload.startswith(b"\xef\xbb\xbf"):
        fail("{} cannot contain a UTF-8 BOM".format(context))
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        fail("{} is not strict UTF-8: {}".format(context, exc))

    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                fail("{} contains duplicate key {!r}".format(context, key))
            result[key] = value
        return result

    def reject_constant(value):
        fail("{} contains non-JSON constant {}".format(context, value))

    try:
        return json.loads(
            text, object_pairs_hook=unique_pairs,
            parse_constant=reject_constant,
        )
    except ReleaseError:
        raise
    except ValueError as exc:
        fail("{} is malformed JSON: {}".format(context, exc))


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


def bootstrap_requires_nxsplash(version):
    """Return whether this bootstrap contract mandates the pre-runtime helper."""
    return version_tuple(version, "nxbootstrap version") >= \
        NXSPLASH_BOOTSTRAP_FLOOR


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
    declared_sdl_floor = policy.get("sdl", {}).get("floor")
    if declared_sdl_floor != SDL_PUBLIC_FLOOR:
        fail("nxabi policy drift: sdl.floor is {!r}, nxrelease enforces {!r}"
             .format(declared_sdl_floor, SDL_PUBLIC_FLOOR))
    return "agrees"


# V4-03B: the SDL symbol->version authority is loaded through nxabi's own
# strict fail-closed parser, so both tools interpret the same bytes the same
# way.  The module and the authority are cached per process.
_NXABI_MODULE = None
_SDL_AUTHORITY = None


def _nxabi_module():
    global _NXABI_MODULE
    if _NXABI_MODULE is None:
        import importlib.util
        try:
            spec = importlib.util.spec_from_file_location(
                "nxabi_sdl_authority", str(NXABI_MODULE_PATH))
            if spec is None or spec.loader is None:
                raise ImportError("no loadable spec")
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
        except (OSError, ImportError, SyntaxError, ValueError) as error:
            fail("cannot load the nxabi SDL authority parser {}: {}".format(
                NXABI_MODULE_PATH, error))
        _NXABI_MODULE = module
    return _NXABI_MODULE


def load_sdl_symbol_authority():
    """The single SDL symbol->minimum-version authority (fail closed).

    Membership in the SONAME allowlist or in a dependency closure never
    equals meeting the floor: the decision for every direct SDL2-core import
    of a PUBLIC/universal candidate comes from this table alone, read from
    framework/nxabi through nxabi's strict parser and recorded (authority id
    plus SHA-256) in the validate receipt.
    """
    global _SDL_AUTHORITY
    if _SDL_AUTHORITY is None:
        module = _nxabi_module()
        try:
            policy = module.load_policy(ABI_POLICY_PATH)
            authority = module.load_sdl_authority_for_policy(
                policy, ABI_POLICY_PATH)
            declared = policy.get("sdl", {}).get("floor")
        except module.AbiError as error:
            fail("SDL symbol authority rejected: {}".format(error))
        if declared != SDL_PUBLIC_FLOOR:
            fail("nxabi policy drift: sdl.floor is {!r}, nxrelease enforces "
                 "{!r}".format(declared, SDL_PUBLIC_FLOOR))
        _SDL_AUTHORITY = authority
    return _SDL_AUTHORITY


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


VIDEO_PROOF_RECEIPT_FIELDS = (
    "schema", "schema_version", "run_id", "generation", "port_id",
    "verdict", "reason",
)

INPUT_PROOF_SCHEMA = "nxinput-gptk-event-evidence/1"
# /3: the nxinput 0.10.0 live runtime understands NEXTOS_CONTROLLERS/3
# (FACE_LAYOUT). A V3-capable runtime must never present /2 (sealed
# V4-CTRL-01 oracle, case N28).
INPUT_RUNTIME_MARKER = "nxinput-gptk-runtime/3"
# V5 (nxinput 0.11.0): a NEXTOS_CONTROLLERS/4 port is driven through the
# gptk4 bridge and prints the /4 marker. The expected marker follows the
# schema of the packaged default (set when the default is audited).
INPUT_RUNTIME_MARKER_V4 = "nxinput-gptk-runtime/4"
_INPUT_RUNTIME_MARKER_EXPECTED = [INPUT_RUNTIME_MARKER]


def input_runtime_marker():
    return _INPUT_RUNTIME_MARKER_EXPECTED[0]
# nxinput 0.10.2: the class of evidence behind the event->sink cases. The only
# class accepted for a port that declares controls.proof is the automated
# on-device proof (uinput clones on the real device, framework-driven). A host
# fixture is never on-device evidence and a human witness is never required.
INPUT_PROOF_EVIDENCE_CLASS_ON_DEVICE = "ON_DEVICE_AUTOMATED_INPUT_PROOF"
# 0.4.6 (04/09/2026, ordem do NextOS): a validação AO VIVO pelo dono do projeto
# é uma classe própria, explícita e nomeada no lock (`approval`: quem, quando,
# aparelho, o que foi validado, sha do log real). Nunca se apresenta como
# receipt automático.
INPUT_PROOF_EVIDENCE_CLASS_HUMAN = "HUMAN_LIVE_APPROVAL"
INPUT_PROOF_EVIDENCE_CLASSES = frozenset((INPUT_PROOF_EVIDENCE_CLASS_ON_DEVICE, INPUT_PROOF_EVIDENCE_CLASS_HUMAN))
INPUT_PROOF_ACCEPTED_FOR_CONTROLS_PROOF = frozenset((INPUT_PROOF_EVIDENCE_CLASS_ON_DEVICE, INPUT_PROOF_EVIDENCE_CLASS_HUMAN))
INPUT_GODOT_RUNTIME_MARKER = "nxinput-godot-runtime/1"
INPUT_GODOT_FRAME_PROOF_MARKER = "nxgl-godot-frame-proof/2"
INPUT_GODOT_FRAME_PROOF_REQUIRED_SYMBOLS = frozenset((
    "nxgl_frame_proof_is_fatal",
    "nxgl_frame_proof_consume_fatal",
))
INPUT_RUNTIME_CONTRACT = {
    "schema": "nxinput-gptk-live/1",
    "context_initial": "unproven",
    "unproven_policy": "native-passthrough",
    "sink_coverage": "all-actions-before-activation",
    "delivery_ack": "required",
}
INPUT_RUNTIME_REQUIRED_SYMBOLS = frozenset((
    "nxinput_gptk_load_at",
    "nxinput_gptk_load_receipt_json",
    "nxinput_gptk_parse",
    "nxinput_gptk_decide",
    "nxinput_gptk_live_init",
    "nxinput_gptk_live_register",
    "nxinput_gptk_live_seal",
    "nxinput_gptk_live_set_context",
    "nxinput_gptk_live_clear_context",
    "nxinput_gptk_live_should_consume",
    "nxinput_gptk_live_feed",
    "nxinput_gptk_runtime_marker",
    "nxinput_gptk_event_evidence_schema",
))


def input_proof_receipt_bytes(value):
    """Canonical external input-proof producer line, including final LF."""
    return (json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
    ) + "\n").encode("utf-8")


def _input_proof_string(value, context, pattern, maximum=192):
    value = require_string(value, context)
    if len(value) > maximum or not re.fullmatch(pattern, value):
        fail("{} is not canonical".format(context))
    return value


def _input_proof_string_list(value, context, pattern, maximum=192):
    if not isinstance(value, list) or not value:
        fail("{} must be a non-empty array".format(context))
    result = [
        _input_proof_string(item, "{}[{}]".format(context, index),
                            pattern, maximum)
        for index, item in enumerate(value)
    ]
    if result != sorted(set(result)):
        fail("{} must be sorted and unique".format(context))
    return result


def normalize_input_proof(value, context):
    value = require_object(value, context)
    fields = {
        "schema", "schema_version", "run_id", "generation", "port_id",
        "mapping_sha256", "adapter_contract_sha256", "verdict", "runtime",
        "contexts", "sinks", "cases", "safety",
    }
    if set(value) not in (fields, fields | {"evidence_class"}, fields | {"evidence_class", "approval"}):
        fail("{} fields are not canonical".format(context))
    evidence_class = value.get("evidence_class")
    if evidence_class is not None and \
            evidence_class not in INPUT_PROOF_EVIDENCE_CLASSES:
        fail("{}.evidence_class {!r} is not an accepted on-device class".format(
            context, evidence_class))
    approval = value.get("approval")
    if (evidence_class == INPUT_PROOF_EVIDENCE_CLASS_HUMAN) != (approval is not None):
        fail("{}: HUMAN_LIVE_APPROVAL and the approval block come together".format(context))
    if approval is not None:
        approval = require_object(approval, context + ".approval")
        if set(approval) != {"approved_by", "device", "validated", "approved_at", "device_log_sha256"}:
            fail("{}.approval fields are not canonical".format(context))
        for key in ("approved_by", "device", "validated", "approved_at"):
            _input_proof_string(approval.get(key), context + ".approval." + key, r"[ -~\u00a0-\uffff]+", 512)
        parse_sha256(approval.get("device_log_sha256"), context + ".approval.device_log_sha256")
    if (value.get("schema") != INPUT_PROOF_SCHEMA or
            type(value.get("schema_version")) is not int or
            value.get("schema_version") != 1 or value.get("verdict") != "OK"):
        fail("{} schema/verdict is unsupported".format(context))
    run_id = _input_proof_string(
        value.get("run_id"), context + ".run_id", r"[A-Za-z0-9._-]{1,192}")
    generation = _input_proof_string(
        value.get("generation"), context + ".generation",
        r"[0-9a-f]{32}|[0-9a-f]{64}", 64)
    port_id = _input_proof_string(
        value.get("port_id"), context + ".port_id",
        r"[a-z0-9][a-z0-9._-]*", 128)

    runtime = require_object(value.get("runtime"), context + ".runtime")
    base_runtime_fields = {"marker", "evidence_schema", "symbols"}
    godot_runtime_fields = base_runtime_fields | {
        "godot_marker", "frame_proof_marker",
    }
    if set(runtime) not in (base_runtime_fields, godot_runtime_fields) or \
            runtime.get("marker") not in (INPUT_RUNTIME_MARKER, INPUT_RUNTIME_MARKER_V4) or \
            runtime.get("evidence_schema") != INPUT_PROOF_SCHEMA:
        fail("{}.runtime is not the canonical live boundary".format(context))
    # The lock is loaded before the packaged defaults are read (V3 or V4): the
    # exact marker is re-checked against the defaults' schema once known
    # (see the NEXTOS_CONTROLLERS magic detection).
    godot_runtime = set(runtime) == godot_runtime_fields
    if godot_runtime and (
            runtime.get("godot_marker") != INPUT_GODOT_RUNTIME_MARKER or
            runtime.get("frame_proof_marker") !=
            INPUT_GODOT_FRAME_PROOF_MARKER):
        fail("{}.runtime Godot markers are not canonical".format(context))
    runtime_symbols = _input_proof_string_list(
        runtime.get("symbols"), context + ".runtime.symbols",
        r"[A-Za-z_][A-Za-z0-9_]*", 192)
    if not INPUT_RUNTIME_REQUIRED_SYMBOLS.issubset(runtime_symbols):
        fail("{}.runtime.symbols omits the canonical live boundary".format(
            context))
    if godot_runtime and not \
            INPUT_GODOT_FRAME_PROOF_REQUIRED_SYMBOLS.issubset(runtime_symbols):
        fail("{}.runtime.symbols omits the canonical Godot fatal boundary".format(
            context))

    contexts = value.get("contexts")
    if not isinstance(contexts, list) or not contexts:
        fail("{}.contexts must be a non-empty array".format(context))
    normalized_contexts = []
    for index, item in enumerate(contexts):
        item_context = "{}.contexts[{}]".format(context, index)
        item = require_object(item, item_context)
        if set(item) != {"context", "source", "observed"}:
            fail(item_context + " fields are not canonical")
        name = _input_proof_string(
            item.get("context"), item_context + ".context",
            r"menu|gameplay|cursor", 16)
        source = _input_proof_string(
            item.get("source"), item_context + ".source",
            r"[A-Za-z0-9_.:/-]{1,95}", 95)
        if item.get("observed") is not True:
            fail(item_context + ".observed must be true")
        normalized_contexts.append({
            "context": name, "source": source, "observed": True,
        })
    if normalized_contexts != sorted(
            normalized_contexts, key=lambda item: item["context"]):
        fail("{}.contexts must be sorted and unique".format(context))
    if len({item["context"] for item in normalized_contexts}) != \
            len(normalized_contexts):
        fail("{}.contexts contains a duplicate context".format(context))

    sinks = value.get("sinks")
    if not isinstance(sinks, list) or not sinks:
        fail("{}.sinks must be a non-empty array".format(context))
    normalized_sinks = []
    for index, item in enumerate(sinks):
        item_context = "{}.sinks[{}]".format(context, index)
        item = require_object(item, item_context)
        required = {"sink", "symbol", "method", "targets", "exists"}
        optional = {"artifact", "artifact_sha256"}
        if set(item) - required - optional or not required.issubset(item):
            fail(item_context + " fields are not canonical")
        sink = _input_proof_string(
            item.get("sink"), item_context + ".sink",
            r"[a-z][a-z0-9_-]*(?:[.][a-z][a-z0-9_-]*)+", 96)
        symbol = _input_proof_string(
            item.get("symbol"), item_context + ".symbol",
            r"[A-Za-z_][A-Za-z0-9_]*", 192)
        method = item.get("method")
        if method not in ("runtime-action-registry", "contract-artifact"):
            fail(item_context + ".method is unsupported")
        targets = _input_proof_string_list(
            item.get("targets"), item_context + ".targets",
            r"[A-Za-z0-9_./:-]{1,192}", 192)
        if item.get("exists") is not True:
            fail(item_context + ".exists must be true")
        normalized = {"sink": sink, "symbol": symbol, "method": method,
                      "targets": targets, "exists": True}
        artifact = item.get("artifact")
        artifact_sha = item.get("artifact_sha256")
        if method == "contract-artifact":
            if artifact is None or artifact_sha is None:
                fail(item_context + " contract-artifact needs path and SHA-256")
            normalized["artifact"] = safe_relative(
                artifact, item_context + ".artifact")
            normalized["artifact_sha256"] = parse_sha256(
                artifact_sha, item_context + ".artifact_sha256")
        elif artifact is not None or artifact_sha is not None:
            fail(item_context + " runtime registry cannot claim an artifact")
        normalized_sinks.append(normalized)
    if normalized_sinks != sorted(
            normalized_sinks, key=lambda item: item["sink"]):
        fail("{}.sinks must be sorted and unique".format(context))
    if len({item["sink"] for item in normalized_sinks}) != len(normalized_sinks):
        fail("{}.sinks contains a duplicate sink".format(context))

    cases = value.get("cases")
    if not isinstance(cases, list) or not cases:
        fail("{}.cases must be a non-empty array".format(context))
    normalized_cases = []
    for index, item in enumerate(cases):
        item_context = "{}.cases[{}]".format(context, index)
        item = require_object(item, item_context)
        expected = {"context", "context_source", "control", "event",
                    "decision", "action", "sink", "delivery_count"}
        if set(item) != expected:
            fail(item_context + " fields are not canonical")
        normalized = {
            "context": _input_proof_string(
                item.get("context"), item_context + ".context",
                r"menu|gameplay|cursor", 16),
            "context_source": _input_proof_string(
                item.get("context_source"), item_context + ".context_source",
                r"[A-Za-z0-9_.:/-]{1,95}", 95),
            "control": _input_proof_string(
                item.get("control"), item_context + ".control",
                r"[A-Z0-9_]{1,32}", 32),
            "event": _input_proof_string(
                item.get("event"), item_context + ".event",
                r"press|axis|motion", 16),
            "decision": item.get("decision"),
            "action": _input_proof_string(
                item.get("action"), item_context + ".action",
                r"[a-z][a-z0-9_]*(?:[.][a-z][a-z0-9_]*)+", 64),
            "sink": _input_proof_string(
                item.get("sink"), item_context + ".sink",
                r"[a-z][a-z0-9_-]*(?:[.][a-z][a-z0-9_-]*)+", 96),
            "delivery_count": item.get("delivery_count"),
        }
        if normalized["decision"] != "ACTION" or \
                type(normalized["delivery_count"]) is not int or \
                normalized["delivery_count"] != 1:
            fail(item_context + " must prove one ACTION delivery")
        normalized_cases.append(normalized)
    case_key = lambda item: (
        item["context"], item["control"], item["action"], item["sink"],
        item["event"], item["context_source"],
    )
    if normalized_cases != sorted(normalized_cases, key=case_key):
        fail("{}.cases must be canonically sorted".format(context))
    if len({case_key(item) for item in normalized_cases}) != len(normalized_cases):
        fail("{}.cases contains a duplicate case".format(context))

    safety = require_object(value.get("safety"), context + ".safety")
    if set(safety) != {"unknown_context", "missing_sink", "failed_ack"}:
        fail("{}.safety fields are not canonical".format(context))
    passthrough = {"result": "PASSTHROUGH", "suppressed": False,
                   "delivery_count": 0}
    fatal = {"result": "FATAL", "native_replay": False}
    if safety.get("unknown_context") != passthrough or \
            safety.get("missing_sink") != passthrough or \
            safety.get("failed_ack") != fatal:
        fail("{}.safety does not prove fail-safe behavior".format(context))

    normalized_runtime = {
        "marker": runtime.get("marker"),
        "evidence_schema": INPUT_PROOF_SCHEMA,
        "symbols": runtime_symbols,
    }
    if godot_runtime:
        normalized_runtime.update({
            "godot_marker": INPUT_GODOT_RUNTIME_MARKER,
            "frame_proof_marker": INPUT_GODOT_FRAME_PROOF_MARKER,
        })
    normalized = {
        "schema": INPUT_PROOF_SCHEMA, "schema_version": 1,
        "run_id": run_id, "generation": generation, "port_id": port_id,
        "mapping_sha256": parse_sha256(
            value.get("mapping_sha256"), context + ".mapping_sha256"),
        "adapter_contract_sha256": parse_sha256(
            value.get("adapter_contract_sha256"),
            context + ".adapter_contract_sha256"),
        "verdict": "OK",
        "runtime": normalized_runtime,
        "contexts": normalized_contexts, "sinks": normalized_sinks,
        "cases": normalized_cases, "safety": safety,
    }
    if evidence_class is not None:
        normalized["evidence_class"] = evidence_class
    if approval is not None:
        normalized["approval"] = {k: approval[k] for k in sorted(approval)}
    return normalized


def video_proof_receipt_bytes(value):
    """Exact compact line emitted by the C producer, including final LF."""
    ordered = {field: value[field] for field in VIDEO_PROOF_RECEIPT_FIELDS}
    return (json.dumps(
        ordered, separators=(",", ":"), ensure_ascii=False,
    ) + "\n").encode("utf-8")


def normalize_candidate_lock(value, context, document_sha256=None):
    """Validate the external, post-proof executable lock.

    The lock is deliberately not a field in nxrelease.json or nxport.json.  A
    package cannot attest that its own current executable is the one which was
    observed before packaging.  The build boundary receives a frozen external
    document and carries its identity into release metadata instead.  This is
    a procedural byte/receipt trust anchor, not independent cryptographic
    proof that a human observed a physical display.
    """
    value = require_object(value, context)
    required = {"schema", "schema_version", "executable", "sha256"}
    optional = {
        "document_sha256", "video_proof", "video_proof_receipt_sha256",
        "input_proof", "input_proof_receipt_sha256",
    }
    if set(value) - required - optional or not required.issubset(value):
        fail("{} fields are not canonical".format(context))
    if (value.get("schema") != CANDIDATE_LOCK_SCHEMA or
            type(value.get("schema_version")) is not int or
            value.get("schema_version") != 1):
        fail("{} schema is unsupported".format(context))
    executable = safe_relative(
        value.get("executable"), context + ".executable"
    )
    if (len(executable) > 512 or not re.fullmatch(
            r"[A-Za-z0-9._+-]+(?:/[A-Za-z0-9._+-]+)+", executable)):
        fail("{}.executable is not a portable packaged executable path".format(
            context))
    digest = parse_sha256(value.get("sha256"), context + ".sha256")
    video_proof = value.get("video_proof")
    video_proof_receipt_sha256 = value.get("video_proof_receipt_sha256")
    if (video_proof is None) != (video_proof_receipt_sha256 is None):
        fail("{} video proof object and receipt SHA-256 must appear together".format(
            context))
    if video_proof is not None:
        video_proof = require_object(
            video_proof, context + ".video_proof"
        )
        proof_fields = set(VIDEO_PROOF_RECEIPT_FIELDS)
        if set(video_proof) != proof_fields:
            fail("{}.video_proof fields are not canonical".format(context))
        if (video_proof.get("schema") !=
                "org.nextos.nxruntime.video-proof" or
                type(video_proof.get("schema_version")) is not int or
                video_proof.get("schema_version") != 1):
            fail("{}.video_proof schema is unsupported".format(context))
        run_id = require_string(
            video_proof.get("run_id"), context + ".video_proof.run_id"
        )
        if not re.fullmatch(r"[A-Za-z0-9._-]{1,192}", run_id):
            fail("{}.video_proof.run_id is not canonical".format(context))
        generation = require_string(
            video_proof.get("generation"),
            context + ".video_proof.generation",
        )
        if not re.fullmatch(r"[0-9a-f]{32}|[0-9a-f]{64}", generation):
            fail("{}.video_proof.generation is not a generation id".format(
                context))
        port_id = require_string(
            video_proof.get("port_id"), context + ".video_proof.port_id"
        )
        if not PACKAGE_ID_RE.fullmatch(port_id):
            fail("{}.video_proof.port_id is not portable".format(context))
        if video_proof.get("verdict") != "OK":
            fail("{}.video_proof verdict must be OK".format(context))
        if video_proof.get("reason") != "non-black":
            fail("{}.video_proof reason must be non-black".format(context))
        video_proof_receipt_sha256 = parse_sha256(
            video_proof_receipt_sha256,
            context + ".video_proof_receipt_sha256",
        )
        receipt_line = video_proof_receipt_bytes(video_proof)
        actual_receipt_sha256 = hashlib.sha256(receipt_line).hexdigest()
        if video_proof_receipt_sha256 != actual_receipt_sha256:
            fail("{} video proof receipt SHA-256 differs from the exact producer bytes".format(
                context))

    input_proof = value.get("input_proof")
    input_proof_receipt_sha256 = value.get("input_proof_receipt_sha256")
    if (input_proof is None) != (input_proof_receipt_sha256 is None):
        fail("{} input proof object and receipt SHA-256 must appear together".format(
            context))
    if input_proof is not None:
        input_proof = normalize_input_proof(
            input_proof, context + ".input_proof")
        input_proof_receipt_sha256 = parse_sha256(
            input_proof_receipt_sha256,
            context + ".input_proof_receipt_sha256")
        actual_input_receipt_sha256 = hashlib.sha256(
            input_proof_receipt_bytes(input_proof)).hexdigest()
        if input_proof_receipt_sha256 != actual_input_receipt_sha256:
            fail("{} input proof receipt SHA-256 differs from the canonical producer bytes".format(
                context))

    carried_document_sha256 = value.get("document_sha256")
    if carried_document_sha256 is not None:
        carried_document_sha256 = parse_sha256(
            carried_document_sha256, context + ".document_sha256"
        )
    if document_sha256 is not None:
        document_sha256 = parse_sha256(
            document_sha256, context + " source document SHA-256"
        )
        if (carried_document_sha256 is not None and
                carried_document_sha256 != document_sha256):
            fail("{} document identity changed".format(context))
        carried_document_sha256 = document_sha256
    if carried_document_sha256 is None:
        fail("{} lacks the external document identity".format(context))
    result = {
        "document_sha256": carried_document_sha256,
        "executable": executable,
        "schema": CANDIDATE_LOCK_SCHEMA,
        "schema_version": 1,
        "sha256": digest,
    }
    if video_proof is not None:
        result["video_proof"] = dict(video_proof)
        result["video_proof_receipt_sha256"] = video_proof_receipt_sha256
    if input_proof is not None:
        result["input_proof"] = input_proof
        result["input_proof_receipt_sha256"] = input_proof_receipt_sha256
    return result


def _authority_file_identity(file_stat):
    return (
        file_stat.st_dev, file_stat.st_ino, file_stat.st_uid,
        stat.S_IMODE(file_stat.st_mode), file_stat.st_nlink,
        file_stat.st_size, file_stat.st_mtime_ns, file_stat.st_ctime_ns,
    )


def _validate_authority_directory(directory_stat, context, direct_parent):
    if not stat.S_ISDIR(directory_stat.st_mode):
        fail("{} is not a directory".format(context))
    effective_uid = os.geteuid()
    if directory_stat.st_uid not in (0, effective_uid):
        fail("{} is owned by an untrusted uid".format(context))
    mode = stat.S_IMODE(directory_stat.st_mode)
    writable = mode & 0o022
    sticky_root = bool(
        directory_stat.st_uid == 0 and mode & stat.S_ISVTX
    )
    if writable and (direct_parent or not sticky_root):
        fail("{} is group/other writable and not a safe authority".format(
            context))
    if direct_parent and directory_stat.st_uid != effective_uid:
        fail("{} must be owned by the invoking uid".format(context))


def _open_secure_authority_parent(parent, context):
    """Open every directory component without following symlinks."""
    if not hasattr(os, "O_NOFOLLOW") or not hasattr(os, "O_DIRECTORY"):
        fail("{} requires O_NOFOLLOW and O_DIRECTORY support".format(context))
    parent = Path(os.path.abspath(os.fspath(parent)))
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    flags |= getattr(os, "O_CLOEXEC", 0)
    descriptor = os.open("/", flags)
    try:
        parts = parent.parts[1:]
        _validate_authority_directory(
            os.fstat(descriptor), context + " ancestor /", not parts
        )
        for index, component in enumerate(parts):
            next_descriptor = os.open(component, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = next_descriptor
            _validate_authority_directory(
                os.fstat(descriptor),
                "{} directory {}".format(context, parent),
                index == len(parts) - 1,
            )
        return descriptor, os.fstat(descriptor), parent
    except BaseException:
        os.close(descriptor)
        raise


def _validate_authority_file(file_stat, context):
    if not stat.S_ISREG(file_stat.st_mode):
        fail("{} is not a regular file".format(context))
    if file_stat.st_uid != os.geteuid():
        fail("{} must be owned by the invoking uid".format(context))
    if file_stat.st_nlink != 1:
        fail("{} must retain exactly one link; hardlinked/replaced authority is forbidden".format(
            context))
    if stat.S_IMODE(file_stat.st_mode) & 0o222:
        fail("{} must be frozen read-only before release".format(context))
    if file_stat.st_size > 64 * 1024:
        fail("{} is unexpectedly larger than 65536 bytes".format(context))


def _read_external_authority(path, context, forbidden_root=None):
    """Read one authority via a stable O_NOFOLLOW fd and safe parent chain."""
    candidate = Path(os.path.abspath(os.fspath(path)))
    if not candidate.name or candidate.name in (".", ".."):
        fail("{} path is invalid".format(context))
    if forbidden_root is not None and source_is_within(
            Path(forbidden_root).resolve(), candidate):
        fail("{} must be external to source_root; self-attestation is forbidden".format(
            context))
    parent_fd = None
    file_fd = None
    reopened_fd = None
    try:
        parent_fd, parent_before, parent_path = \
            _open_secure_authority_parent(candidate.parent, context)
        flags = os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0)
        file_fd = os.open(candidate.name, flags, dir_fd=parent_fd)
        file_before = os.fstat(file_fd)
        _validate_authority_file(file_before, context)
        entry_before = os.stat(
            candidate.name, dir_fd=parent_fd, follow_symlinks=False
        )
        if _authority_file_identity(entry_before) != \
                _authority_file_identity(file_before):
            fail("{} directory entry differs from the opened authority".format(
                context))
        chunks = []
        total = 0
        while True:
            chunk = os.read(file_fd, min(65537 - total, 16384))
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if total > 64 * 1024:
                fail("{} is unexpectedly larger than 65536 bytes".format(
                    context))
        file_after = os.fstat(file_fd)
        _validate_authority_file(file_after, context)
        entry_after = os.stat(
            candidate.name, dir_fd=parent_fd, follow_symlinks=False
        )
        if (_authority_file_identity(file_before) !=
                _authority_file_identity(file_after) or
                _authority_file_identity(file_before) !=
                _authority_file_identity(entry_after)):
            fail("{} changed inode/content/security state while being read".format(
                context))
        parent_after = os.fstat(parent_fd)
        if ((parent_before.st_dev, parent_before.st_ino,
             parent_before.st_uid, stat.S_IMODE(parent_before.st_mode)) !=
                (parent_after.st_dev, parent_after.st_ino,
                 parent_after.st_uid, stat.S_IMODE(parent_after.st_mode))):
            fail("{} parent authority changed while being read".format(context))
        reopened_fd, reopened_stat, reopened_path = \
            _open_secure_authority_parent(candidate.parent, context)
        if (reopened_path != parent_path or
                (reopened_stat.st_dev, reopened_stat.st_ino) !=
                (parent_before.st_dev, parent_before.st_ino)):
            fail("{} parent path was replaced while being read".format(context))
        reopened_entry = os.stat(
            candidate.name, dir_fd=reopened_fd, follow_symlinks=False
        )
        if _authority_file_identity(reopened_entry) != \
                _authority_file_identity(file_before):
            fail("{} path no longer names the opened authority".format(context))
        payload = b"".join(chunks)
        return {
            "document_sha256": hashlib.sha256(payload).hexdigest(),
            "path": candidate,
            "payload": payload,
        }
    except OSError as error:
        fail("cannot securely read {}: {}".format(context, error))
    finally:
        for descriptor in (reopened_fd, file_fd, parent_fd):
            if descriptor is not None:
                os.close(descriptor)


def load_candidate_lock(path, source_root):
    """Read a frozen post-proof lock through a stable external authority fd."""
    authority = _read_external_authority(
        path, "candidate lock", forbidden_root=source_root
    )
    document = parse_json_strict(authority["payload"], "candidate lock")
    return normalize_candidate_lock(
        document, "candidate lock", authority["document_sha256"]
    )


def normalize_historical_authority(value, context, document_sha256=None):
    value = require_object(value, context)
    expected = {
        "schema", "schema_version", "mode", "archive_sha256",
        "metadata_tool_version", "nxbootstrap_version", "quarantine",
    }
    optional = {"document_sha256"}
    if set(value) - expected - optional or not expected.issubset(value):
        fail("{} fields are not canonical".format(context))
    if (value.get("schema") != HISTORICAL_AUTHORITY_SCHEMA or
            type(value.get("schema_version")) is not int or
            value.get("schema_version") != 1 or
            value.get("mode") != "historical-read-only" or
            value.get("quarantine") is not True):
        fail("{} schema/mode is unsupported".format(context))
    tool_version = require_string(
        value.get("metadata_tool_version"),
        context + ".metadata_tool_version",
    )
    if tool_version not in HISTORICAL_TOOL_VERSIONS:
        fail("{} metadata tool version is outside the authenticated historical allowlist".format(
            context))
    bootstrap_version = require_string(
        value.get("nxbootstrap_version"), context + ".nxbootstrap_version"
    )
    version_tuple(bootstrap_version, context + ".nxbootstrap_version")
    archive_sha256 = parse_sha256(
        value.get("archive_sha256"), context + ".archive_sha256"
    )
    carried_document_sha256 = value.get("document_sha256")
    if carried_document_sha256 is not None:
        carried_document_sha256 = parse_sha256(
            carried_document_sha256, context + ".document_sha256"
        )
    if document_sha256 is not None:
        document_sha256 = parse_sha256(
            document_sha256, context + " source document SHA-256"
        )
        if (carried_document_sha256 is not None and
                carried_document_sha256 != document_sha256):
            fail("{} document identity changed".format(context))
        carried_document_sha256 = document_sha256
    if carried_document_sha256 is None:
        fail("{} lacks its external document identity".format(context))
    return {
        "archive_sha256": archive_sha256,
        "document_sha256": carried_document_sha256,
        "metadata_tool_version": tool_version,
        "mode": "historical-read-only",
        "nxbootstrap_version": bootstrap_version,
        "quarantine": True,
        "schema": HISTORICAL_AUTHORITY_SCHEMA,
        "schema_version": 1,
    }


def load_historical_authority(path, archive_path):
    authority_file = _read_external_authority(path, "historical authority")
    document = normalize_historical_authority(
        parse_json_strict(
            authority_file["payload"], "historical authority"
        ),
        "historical authority",
        authority_file["document_sha256"],
    )
    actual_archive_sha256 = sha256_file(Path(archive_path).resolve())
    if document["archive_sha256"] != actual_archive_sha256:
        fail("historical authority names different archive bytes")
    archive_parts = [
        part.casefold() for part in Path(archive_path).resolve().parts
    ]
    if not any(re.search(r"(?:^|[._-])quarantine(?:$|[._-])", part)
               for part in archive_parts):
        fail("historical archive must live in a clearly named quarantine path")
    return document


def normalize_release_authority(value, context="release authority"):
    value = require_object(value, context)
    require_keys(value, ("mode", "machine_receipts_required"), context)
    mode = require_string(value.get("mode"), context + ".mode")
    if mode not in RELEASE_AUTHORITIES:
        fail("{} mode is unsupported".format(context))
    receipts = value.get("machine_receipts_required")
    if not isinstance(receipts, bool):
        fail("{}.machine_receipts_required must be boolean".format(context))
    expected = mode == RELEASE_AUTHORITY_LOCK
    if receipts is not expected:
        fail("{} contradicts mode {}".format(context, mode))
    return {"mode": mode, "machine_receipts_required": receipts}


def resolve_release_authority(requested, candidate_lock):
    """Choose one authority without turning a human decision into a receipt.

    A supplied legacy candidate lock keeps its old fail-closed meaning.  With
    no lock, V5 defaults to the owner's explicit human decision; nxrelease
    authenticates the package bytes but does not manufacture proof of what a
    person observed on a device.
    """
    if requested is not None and requested not in RELEASE_AUTHORITIES:
        fail("release authority is unsupported")
    mode = requested
    if mode is None:
        mode = RELEASE_AUTHORITY_LOCK if candidate_lock is not None \
            else RELEASE_AUTHORITY_HUMAN
    if mode == RELEASE_AUTHORITY_LOCK and candidate_lock is None:
        fail("candidate-lock authority requires --candidate-lock")
    if mode == RELEASE_AUTHORITY_HUMAN and candidate_lock is not None:
        fail("human authority cannot also consume a candidate lock")
    return {
        "mode": mode,
        "machine_receipts_required": mode == RELEASE_AUTHORITY_LOCK,
    }


def config_release_authority(config):
    """Return the normalized mode, including old in-process test fixtures."""
    value = config.get("release_authority")
    if value is None:
        value = resolve_release_authority(None, config.get("candidate_lock"))
    return normalize_release_authority(value)


def candidate_lock_from_metadata(metadata, historical_authority=None,
                                 release_authority=None):
    """Recover an optional legacy lock under the declared authority mode."""
    raw_candidate_lock = metadata.get("candidate_lock")
    if historical_authority is not None:
        if raw_candidate_lock is not None:
            fail("historical read-only metadata must not claim a current candidate lock")
        return None
    if release_authority is None:
        release_authority = metadata.get("release_authority")
    authority = normalize_release_authority(
        release_authority, "release metadata release_authority"
    )
    if authority["mode"] == RELEASE_AUTHORITY_HUMAN:
        if raw_candidate_lock is not None:
            fail("human release metadata must not embed a candidate lock")
        return None
    if raw_candidate_lock is None:
        fail("candidate-lock release metadata lacks its candidate lock")
    return normalize_candidate_lock(
        raw_candidate_lock, "release metadata candidate_lock"
    )


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


def collect_dynamic_symbols(dyn_syms_output):
    """(undefined GLOBAL nao-weak, definidos exportados) de `--dyn-syms`."""
    undefined, defined = set(), set()
    for line in dyn_syms_output.splitlines():
        fields = line.split()
        if len(fields) < 8:
            continue
        bind, ndx, name = fields[4], fields[6], fields[7].split("@", 1)[0]
        if not name or name in ("Name",):
            continue
        if ndx == "UND":
            if bind == "GLOBAL":
                undefined.add(name)
        else:
            defined.add(name)
    return undefined, defined


_SYMBOL_FLOORS = None
# Simbolos dinamicos por logical_path da execucao corrente (tabela lateral --
# deliberadamente FORA do dict de metadata/SBOM, cujo contrato e pinado).
_DYNAMIC_SYMBOLS = {}
# Marcadores de recibo de video por logical_path (mesma disciplina lateral).
_RECEIPT_MARKERS = {}


def load_symbol_floors(directory=None):
    """Pisos de simbolos por soname: exports garantidos pela versao MAIS ANTIGA
    suportada de cada lib de CFW (intersecao AmberELEC-2023 x dArkOSRE). Um
    soname SEM arquivo de piso e um curinga (satisfaz qualquer simbolo), entao
    o gate nunca cria falso-positivo em stacks que nao mapeamos.

    V4-03B: a familia CORE da SDL2 saiu deste mecanismo. A unica autoridade
    dela e a tabela versionada framework/nxabi/sdl2-symbol-floor.tsv; uma
    lista `.syms` paralela para o core (capaz de divergir, como a que aceitou
    SDL_JoystickGetVendor/Product) falha fechado aqui. `directory` existe
    somente para fixtures de teste."""
    global _SYMBOL_FLOORS
    if directory is None and _SYMBOL_FLOORS is not None:
        return _SYMBOL_FLOORS
    floors = {}
    base = directory if directory is not None else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "symbol-floors")
    if os.path.isdir(base):
        for name in os.listdir(base):
            if not name.endswith(".syms"):
                continue
            if name == SDL_CORE_SONAME + ".syms":
                fail("legacy parallel SDL2-core symbol list {} found in {}; "
                     "the single authority is the versioned "
                     "framework/nxabi/sdl2-symbol-floor.tsv and a parallel "
                     "list capable of diverging fails closed".format(
                         name, base))
            symbols = set()
            with open(os.path.join(base, name),
                      encoding="utf-8") as handle:
                for line in handle:
                    line = line.strip()
                    if line and not line.startswith("#"):
                        symbols.add(line)
            floors[name[:-len(".syms")]] = symbols
    if directory is None:
        _SYMBOL_FLOORS = floors
    return floors


def macro_only_undefined_symbols(dyn_syms_output):
    """Names in MACRO_ONLY_DYNSYMS that appear as UND in `readelf --dyn-syms`.

    Layout of a symbol row: Num: Value Size Type Bind Vis Ndx Name
    An imported (undefined) symbol has "UND" in the Ndx column (index 6).
    Versioned names (Name@VER / Name@@VER) are compared by their base name.
    """
    found = []
    for line in dyn_syms_output.splitlines():
        fields = line.split()
        if len(fields) < 8 or fields[6] != "UND":
            continue
        imported = fields[7].split("@", 1)[0]
        if imported in MACRO_ONLY_DYNSYMS and imported not in found:
            found.append(imported)
    return found


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
    # Refuse UNDEFINED dynamic symbols that are MACROS in the public SDL headers
    # (the real symbol has a different name). Such an UND means the header was
    # missing at build time and the loader crashes with `undefined symbol` on
    # devices whose library exports only the real name.
    dyn_syms_text = run_readelf(path, ("--dyn-syms", "--wide"))
    undefined_globals, defined_exports = collect_dynamic_symbols(dyn_syms_text)
    _DYNAMIC_SYMBOLS[logical_path] = (undefined_globals, defined_exports)
    with open(path, "rb") as stream:
        elf_bytes = stream.read()
    _RECEIPT_MARKERS[logical_path] = (
        b"frame proof verdict=" in elf_bytes,
        b"VIDEO: window=" in elf_bytes)
    del elf_bytes
    for imported in macro_only_undefined_symbols(dyn_syms_text):
        fail("ELF {} imports the macro-only symbol {} as an undefined "
             "dynamic symbol; the real function is {}. A device whose "
             "library exports only the real symbol crashes with "
             "`undefined symbol: {}` (status 127). Call {} directly."
             .format(logical_path, imported, MACRO_ONLY_DYNSYMS[imported],
                     imported, MACRO_ONLY_DYNSYMS[imported]))

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
        with open(str(path), "rb") as handle:
            return parse_json_strict(handle.read(), context)
    except OSError as exc:
        fail("cannot read {} {}: {}".format(context, path, exc))


def canonical_nxextract_ui_contract(architecture):
    """Resolve the immutable NXExtract UI artifact for one package ABI.

    The release manifest, rather than a process-global AArch64 digest, owns the
    architecture mapping.  Both the canonical artifact and the packaged record
    are still independently audited as ELFs by the normal NXRelease pipeline.
    """
    if architecture not in ARCH_MACHINES:
        fail("canonical NXExtract UI architecture is unsupported: {}".format(
            architecture
        ))
    manifest_path = NXEXTRACT_UI_MANIFEST_PATH
    if manifest_path.is_symlink() or not manifest_path.is_file():
        fail("canonical NXExtract UI release manifest is missing or unsafe")
    ensure_no_symlink(
        manifest_path, NXEXTRACT_ROOT,
        "canonical NXExtract UI release manifest",
    )
    manifest = require_object(
        load_json(manifest_path, "canonical NXExtract UI release"),
        "canonical NXExtract UI release",
    )
    required_manifest_fields = {
        "artifacts", "component", "schema_version", "source_sha256",
        "toolchain", "version",
    }
    require_keys(
        manifest, required_manifest_fields,
        "canonical NXExtract UI release",
    )
    if set(manifest) != required_manifest_fields:
        fail("canonical NXExtract UI release manifest is incomplete")
    if (manifest.get("schema_version") != 1 or
            manifest.get("component") != "nxextract-ui" or
            manifest.get("version") != NXEXTRACT_UI_REQUIRED_VERSION):
        fail("canonical NXExtract UI release header is invalid")
    source_sha256 = parse_sha256(
        manifest.get("source_sha256"),
        "canonical NXExtract UI source_sha256",
    )
    source_path = NXEXTRACT_ROOT / "ui" / "nxextract_ui.c"
    if source_path.is_symlink() or not source_path.is_file():
        fail("canonical NXExtract UI source is missing or unsafe")
    ensure_no_symlink(
        source_path, NXEXTRACT_ROOT, "canonical NXExtract UI source"
    )
    if sha256_file(source_path) != source_sha256:
        fail("canonical NXExtract UI source hash differs from release manifest")
    artifacts = require_object(
        manifest.get("artifacts"), "canonical NXExtract UI artifacts"
    )
    artifact = require_object(
        artifacts.get(architecture),
        "canonical NXExtract UI artifact {}".format(architecture),
    )
    artifact_fields = {"glibc_max", "mode", "path", "sha256", "size"}
    require_keys(
        artifact, artifact_fields,
        "canonical NXExtract UI artifact {}".format(architecture),
    )
    if set(artifact) != artifact_fields:
        fail("canonical NXExtract UI artifact record is incomplete")
    expected_path = "ui/release/{}/nxextract-ui".format(architecture)
    if artifact.get("path") != expected_path:
        fail("canonical NXExtract UI artifact path is not architecture-specific")
    if artifact.get("mode") != "0755":
        fail("canonical NXExtract UI artifact mode must be 0755")
    digest = parse_sha256(
        artifact.get("sha256"),
        "canonical NXExtract UI artifact sha256",
    )
    size = artifact.get("size")
    if isinstance(size, bool) or not isinstance(size, int) or size <= 0:
        fail("canonical NXExtract UI artifact size is invalid")
    glibc_max = validate_ceiling(
        artifact.get("glibc_max"),
        "canonical NXExtract UI artifact glibc_max",
    )
    artifact_path = NXEXTRACT_ROOT / PurePosixPath(expected_path)
    if artifact_path.is_symlink() or not artifact_path.is_file():
        fail("canonical NXExtract UI artifact is missing or unsafe")
    ensure_no_symlink(
        artifact_path, NXEXTRACT_ROOT,
        "canonical NXExtract UI artifact",
    )
    if artifact_path.stat().st_size != size:
        fail("canonical NXExtract UI artifact size differs from release manifest")
    if sha256_file(artifact_path) != digest:
        fail("canonical NXExtract UI artifact hash differs from release manifest")
    if not is_elf(artifact_path):
        fail("canonical NXExtract UI artifact is not an ELF")
    header = run_readelf(artifact_path, ("-hW",))
    class_match = re.search(r"^\s*Class:\s*(\S+)", header, re.MULTILINE)
    machine_match = re.search(
        r"^\s*Machine:\s*(.+?)\s*$", header, re.MULTILINE
    )
    if (class_match is None or
            class_match.group(1) != ARCH_CLASSES[architecture]):
        fail("canonical NXExtract UI ELF class differs from architecture record")
    if (machine_match is None or
            machine_match.group(1) != ARCH_MACHINES[architecture]):
        fail("canonical NXExtract UI ELF machine differs from architecture record")
    versions = run_readelf(artifact_path, ("--version-info", "--wide"))
    numeric_versions = sorted(
        set(token[len("GLIBC_"):] for token in
            GLIBC_TOKEN_RE.findall(versions)
            if re.match(r"^GLIBC_[0-9]", token)),
        key=lambda item: version_tuple(item, "NXExtract UI GLIBC version"),
    )
    actual_glibc_max = numeric_versions[-1] if numeric_versions else "none"
    if actual_glibc_max != glibc_max:
        fail("canonical NXExtract UI GLIBC maximum differs from release manifest")
    return {
        "architecture": architecture,
        "glibc_max": glibc_max,
        "release_manifest_sha256": sha256_file(manifest_path),
        "sha256": digest,
        "source_sha256": source_sha256,
        "engine_version": NXEXTRACT_REQUIRED_VERSION,
        "version": NXEXTRACT_UI_REQUIRED_VERSION,
    }


def bind_canonical_nxextract_ui(records, config, architecture):
    """Bind the packaged UI record to the architecture-specific release row."""
    ui_record = next(
        (record for record in records
         if record.get("target") == config["nxextract"]["ui_path"]),
        None,
    )
    if ui_record is None or ui_record.get("kind") != "nxextract-ui-linux":
        fail("NXExtract UI is absent or has the wrong inventory kind")
    if ui_record.get("mode") != 0o755:
        fail("canonical NXExtract UI must be packaged with mode 0755")
    if ui_record.get("architecture") != architecture:
        fail("NXExtract UI architecture differs from nxport")
    contract = canonical_nxextract_ui_contract(architecture)
    # A UI e' pinada por identidade PROPRIA e nao acompanha o motor -- por isso
    # ela tem versao separada (NXEXTRACT_UI_REQUIRED_VERSION). Cobrar aqui que o
    # motor empacotado seja o canonico anularia o opt-in: um port que ainda
    # carrega o motor anterior seria recusado por causa da UI, que nao mudou.
    if config["nxextract"]["version"] not in NXEXTRACT_ENGINES:
        fail("packaged NXExtract engine version is not a supported engine")
    if config["nxextract"]["ui_sha256"] != contract["sha256"]:
        fail("nxextract.ui_sha256 differs from the canonical {} artifact".format(
            architecture
        ))
    actual_path = ui_record.get("actual_path")
    if actual_path is None:
        actual_path = ui_record.get("source")
    if actual_path is None or sha256_file(actual_path) != contract["sha256"]:
        fail("packaged NXExtract UI differs from the canonical release artifact")
    metadata_contract = {
        "ui_architecture": contract["architecture"],
        "ui_glibc_max": contract["glibc_max"],
        "ui_release_manifest_sha256": contract["release_manifest_sha256"],
        "ui_source_sha256": contract["source_sha256"],
        "ui_version": contract["version"],
    }
    for field, expected in metadata_contract.items():
        archived = config["nxextract"].get(field)
        if archived is not None and archived != expected:
            fail("archived NXExtract {} differs from the canonical UI release"
                 .format(field))
    config["nxextract"].update(metadata_contract)
    return contract


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
        fail("portmaster_metadata with a pinned port.json is required for a public release")
    value = require_object(value, "portmaster_metadata")
    require_keys(value, ("port_json", "gameinfo_xml", "images"), "portmaster_metadata")
    missing = sorted({"port_json", "gameinfo_xml", "images"} - set(value))
    if missing:
        fail("portmaster_metadata is missing field(s): {}".format(
            ", ".join(missing)
        ))
    result = {"gameinfo_xml": None, "images": [], "port_json": None}
    if value.get("port_json") is None:
        fail("portmaster_metadata.port_json is required for a public release")
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


def normalize_sdl3_exception(value, context, port_dir):
    if value is None:
        return None
    value = require_object(value, context)
    fields = {
        "architecture", "license_file", "license_spdx", "mode", "path",
        "reason", "sha256", "soname", "source_url", "version",
    }
    if set(value) != fields:
        fail("{} fields are not canonical".format(context))
    path = safe_relative(value.get("path"), context + ".path")
    if not path.startswith(port_dir + "/"):
        fail("{}.path must live inside package.port_dir".format(context))
    if value.get("mode") != "0644":
        fail("{}.mode must be 0644 for a private SDL3 DSO".format(context))
    architecture = require_string(
        value.get("architecture"), context + ".architecture"
    )
    if architecture not in ARCH_MACHINES:
        fail("{}.architecture must be aarch64 or armv7".format(context))
    soname = validate_soname(value.get("soname"), context + ".soname")
    if _sdl_family_from_name(soname) != 3:
        fail("{}.soname must identify SDL3".format(context))
    version = require_string(value.get("version"), context + ".version")
    if not re.fullmatch(r"3\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9._-]+)?", version):
        fail("{}.version must be an exact SDL3 release".format(context))
    source_url = require_string(
        value.get("source_url"), context + ".source_url"
    )
    if not re.fullmatch(r"https://[A-Za-z0-9.-]+(?:/[A-Za-z0-9._~:/?#\[\]@!$&'()*+,;=%-]*)?", source_url):
        fail("{}.source_url must be an HTTPS source identity".format(context))
    reject_private_literal(source_url, context + ".source_url")
    license_spdx = require_string(
        value.get("license_spdx"), context + ".license_spdx"
    )
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+-]{1,63}", license_spdx):
        fail("{}.license_spdx is not a portable SPDX identifier".format(
            context))
    license_file = safe_relative(
        value.get("license_file"), context + ".license_file"
    )
    if not license_file.startswith(port_dir + "/"):
        fail("{}.license_file must live inside package.port_dir".format(
            context))
    reason = require_string(value.get("reason"), context + ".reason")
    if len(reason) < 32:
        fail("{}.reason must document the technical necessity".format(context))
    reject_private_literal(reason, context + ".reason")
    return {
        "architecture": architecture,
        "license_file": license_file,
        "license_spdx": license_spdx,
        "mode": "0644",
        "path": path,
        "reason": reason,
        "sha256": parse_sha256(value.get("sha256"), context + ".sha256"),
        "soname": soname,
        "source_url": source_url,
        "version": version,
    }


def load_manifest(path, requested_ceiling=None, project_linux_overrides=None,
                  candidate_lock_path=None, trusted_candidate_lock=None,
                  release_authority=None):
    manifest_path = Path(path).resolve()
    try:
        manifest_bytes = manifest_path.read_bytes()
        if len(manifest_bytes) > 16 * 1024 * 1024:
            fail("manifest is unexpectedly larger than 16777216 bytes")
        data = parse_json_strict(manifest_bytes, "manifest")
    except OSError as exc:
        fail("cannot read manifest {}: {}".format(manifest_path, exc))
    data = require_object(data, "manifest")
    manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
    require_keys(
        data,
        ("schema_version", "source_root", "package", "release", "nxextract", "portmaster_metadata", "dependencies", "files", "exceptions", "sdl3_exception"),
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

    raw_overrides = project_linux_overrides or {}
    if not isinstance(raw_overrides, dict):
        fail("project-linux overrides must be a path mapping")
    normalized_overrides = {}
    for raw_relative, raw_path in raw_overrides.items():
        relative = safe_relative(
            raw_relative, "project-linux override source", allow_dot=True
        )
        if relative in normalized_overrides:
            fail("duplicate project-linux override {}".format(relative))
        candidate = Path(raw_path)
        if candidate.is_symlink() or not candidate.is_file():
            fail("project-linux override is missing, non-regular or a symlink: {}"
                 .format(relative))
        normalized_overrides[relative] = candidate.resolve()

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
    if launcher_contract_version != NXBOOTSTRAP_REQUIRED_VERSION:
        fail("package.launcher_contract.version must be canonical nxbootstrap {}"
             .format(NXBOOTSTRAP_REQUIRED_VERSION))

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
            "ui_path", "ui_sha256",
            "recipe_path", "recipe_sha256",
        ),
        "nxextract",
    )
    nx_path = safe_relative(nxextract.get("path"), "nxextract.path")
    nx_runner = safe_relative(nxextract.get("runner_path"), "nxextract.runner_path")
    nx_runtime_env = safe_relative(
        nxextract.get("runtime_env_path"), "nxextract.runtime_env_path"
    )
    nx_ui = safe_relative(nxextract.get("ui_path"), "nxextract.ui_path")
    nx_recipe = safe_relative(
        nxextract.get("recipe_path"), "nxextract.recipe_path"
    )
    expected_nx_paths = {
        "path": port_dir + "/nxextract/nxextract.py",
        "runner_path": port_dir + "/nxextract/run-extractor.sh",
        "runtime_env_path": port_dir + "/nxextract/nxextract-runtime-env.sh",
        "ui_path": port_dir + "/nxextract/nxextract-ui",
        "recipe_path": port_dir + "/extractor.json",
    }
    actual_nx_paths = {
        "path": nx_path,
        "runner_path": nx_runner,
        "runtime_env_path": nx_runtime_env,
        "ui_path": nx_ui,
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
    if nx_version not in NXEXTRACT_ENGINES:
        fail("nxextract.version {} is not a supported NXExtract engine "
             "(suportadas: {})".format(
                 nx_version, ", ".join(NXEXTRACT_SUPPORTED_VERSIONS)))
    nx_sha = parse_sha256(nxextract.get("sha256"), "nxextract.sha256")
    nx_runner_sha = parse_sha256(
        nxextract.get("runner_sha256"), "nxextract.runner_sha256"
    )
    nx_runtime_env_sha = parse_sha256(
        nxextract.get("runtime_env_sha256"), "nxextract.runtime_env_sha256"
    )
    nx_ui_sha = parse_sha256(
        nxextract.get("ui_sha256"), "nxextract.ui_sha256"
    )
    # A identidade cobrada e' a DA VERSAO DECLARADA. Aceitar a versao antiga
    # com os hashes da canonica deixaria passar um motor trocado.
    engine_identity = NXEXTRACT_ENGINES[nx_version]
    canonical_nxextract_hashes = (
        (nx_sha, engine_identity["engine_sha256"], "nxextract.sha256"),
        (nx_runner_sha, engine_identity["runner_sha256"],
         "nxextract.runner_sha256"),
        (
            nx_runtime_env_sha,
            engine_identity["runtime_env_sha256"],
            "nxextract.runtime_env_sha256",
        ),
    )
    for actual_hash, expected_hash, hash_field in canonical_nxextract_hashes:
        if actual_hash != expected_hash:
            fail("{} differs from the NXExtract {} identity".format(
                hash_field, nx_version))
    nx_recipe_sha = parse_sha256(
        nxextract.get("recipe_sha256"), "nxextract.recipe_sha256"
    )

    portmaster_metadata = validate_portmaster_metadata_manifest(
        data.get("portmaster_metadata"), port_dir
    )
    dependencies = validate_dependencies_manifest(data.get("dependencies"), port_dir)
    sdl3_exception = normalize_sdl3_exception(
        data.get("sdl3_exception"), "sdl3_exception", port_dir
    )

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
            "ui_path": nx_ui,
            "ui_sha256": nx_ui_sha,
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
        "sdl3_exception": sdl3_exception,
        "project_linux_overrides": normalized_overrides,
        "project_linux_overrides_used": set(),
    }
    if candidate_lock_path is not None and trusted_candidate_lock is not None:
        fail("candidate lock must come from exactly one external authority")
    if candidate_lock_path is not None:
        config["candidate_lock"] = load_candidate_lock(
            candidate_lock_path, source_root
        )
    elif trusted_candidate_lock is not None:
        # Used only by public-final after verify_archive authenticated the exact
        # physically tested candidate.  Ordinary manifests/commands have no
        # route to this argument.
        config["candidate_lock"] = normalize_candidate_lock(
            trusted_candidate_lock, "authenticated tested candidate lock"
        )
    else:
        config["candidate_lock"] = None
    config["release_authority"] = resolve_release_authority(
        release_authority, config["candidate_lock"]
    )
    config["candidate_lock_required"] = (
        config["release_authority"]["mode"] == RELEASE_AUTHORITY_LOCK
    )
    config["records"] = expand_inputs(file_entries, config)
    unused_overrides = set(normalized_overrides) - \
        config["project_linux_overrides_used"]
    if unused_overrides:
        fail("project-linux override does not match a manifest source: {}".format(
            ", ".join(sorted(unused_overrides))
        ))
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
    override = config.get("project_linux_overrides", {}).get(source_value)
    if override is not None:
        if kind != "project-linux":
            fail("{} override is allowed only for kind project-linux".format(
                context
            ))
        source = override
        config["project_linux_overrides_used"].add(source_value)
    else:
        source_candidate = config["source_root"] / PurePosixPath(source_value)
        ensure_no_symlink(
            source_candidate, config["source_root"], context + ".source"
        )
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
        "source_relative": source_value,
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
    installation_path = config["port_dir"] + "/INSTALLATION.md"
    installation = by_target.get(installation_path)
    if installation is None or installation["kind"] != "payload":
        fail("public package must stage {0} as payload".format(installation_path))
    if installation["mode"] != 0o644:
        fail("INSTALLATION.md must be staged with mode 0644")
    if installation.get("expected_sha") is None:
        fail("INSTALLATION.md must be pinned with sha256")
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
            # Source validation records carry ``source`` while staged/archive
            # verification records carry ``actual_path``.  The same contract
            # must run in both phases; otherwise every self-contained port
            # that ships port-env.sh crashes validation before packaging.
            port_env_source = port_env_record.get("actual_path")
            if port_env_source is None:
                port_env_source = port_env_record["source"]
            port_env_text = read_small_text(port_env_source, port_env_path)
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
    ui_record = by_target.get(nx["ui_path"])
    recipe_record = by_target.get(nx["recipe_path"])
    if nx_record is None or nx_record["kind"] != "nxextract":
        fail("nxextract.path must match one files[] entry with kind nxextract")
    if runner_record is None or runner_record["kind"] != "nxextract-runner":
        fail("nxextract.runner_path must match one files[] entry with kind nxextract-runner")
    if (runtime_env_record is None or
            runtime_env_record["kind"] != "nxextract-runtime-env"):
        fail("nxextract.runtime_env_path must match one files[] entry with kind nxextract-runtime-env")
    if ui_record is None or ui_record["kind"] != "nxextract-ui-linux":
        fail("nxextract.ui_path must match one files[] entry with kind nxextract-ui-linux")
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
    if ui_record["mode"] != 0o755:
        fail("canonical NXExtract UI must be packaged with mode 0755")
    if ui_record.get("architecture") not in ARCH_MACHINES:
        fail("canonical NXExtract UI must declare a supported architecture")
    if nx_record["sha256"] != nx["sha256"]:
        fail("nxextract.sha256 does not pin the staged NXExtract file")
    if runner_record["sha256"] != nx["runner_sha256"]:
        fail("nxextract.runner_sha256 does not pin the staged runner")
    if runtime_env_record["sha256"] != nx["runtime_env_sha256"]:
        fail("nxextract.runtime_env_sha256 does not pin the runtime helper")
    if ui_record["sha256"] != nx["ui_sha256"]:
        fail("nxextract.ui_sha256 does not pin the staged UI")
    if recipe_record["sha256"] != nx["recipe_sha256"]:
        fail("nxextract.recipe_sha256 does not pin extractor.json")

    # GAMEDATA-DIR-01: a package with NXExtract must physically materialize
    # the owner-data directory through the generated marker. The check runs
    # on every audit boundary (manifest sources, staged set, reopened ZIP)
    # because validate_package_members is executed on all three.
    gamedata_target = "{}/gamedata/README.txt".format(config["port_dir"])
    gamedata_record = by_target.get(gamedata_target)
    if gamedata_record is None:
        fail(
            "NXExtract package must ship {} so a clean installation "
            "physically creates the documented owner-data directory".format(
                gamedata_target
            )
        )
    if gamedata_record["kind"] != "payload":
        fail("gamedata/README.txt must have kind payload")
    if gamedata_record["mode"] != 0o644:
        fail("gamedata/README.txt must be packaged with mode 0644")
    if not gamedata_record.get("sha256"):
        fail("gamedata/README.txt must be pinned by sha256 (framework marker "
             "integrity; never APK compatibility)")

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
    except ValueError as exc:
        fail("cannot lex shell script {}: {}".format(logical_path, exc))
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
            token = tokens[index]
            wrapper = basename(token)
        elif wrapper == "builtin":
            index += 1
            while index < len(tokens) and tokens[index].startswith("-"):
                index += 1
            if index >= len(tokens):
                return False
            token = tokens[index]
            wrapper = basename(token)
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
            token = tokens[index]
            wrapper = basename(token)
        elif wrapper == "busybox":
            index += 1
            if index >= len(tokens):
                return False
            token = tokens[index]
            wrapper = basename(token)

        # These commands execute another literal command after their own
        # options. Their option grammars vary by implementation, so fail
        # closed if a literal stat executable appears in the same command.
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


def validate_nxextract_recipe_arrays(recipe):
    """Validate the array fields with the same defaults as NXExtract 1.3.0."""
    for field in ("extract", "commit"):
        if not isinstance(recipe.get(field), list):
            fail("NXExtract recipe {} must be an array".format(field))
    if not isinstance(recipe.get("validate", []), list):
        fail("NXExtract recipe validate must be an array")


def validate_nxextract(records, config):
    by_target = {record["target"]: record for record in records}
    nx = config["nxextract"]
    nx_record = by_target.get(nx["path"])
    runner_record = by_target.get(nx["runner_path"])
    runtime_env_record = by_target.get(nx["runtime_env_path"])
    ui_record = by_target.get(nx["ui_path"])
    recipe_record = by_target.get(nx["recipe_path"])
    if nx_record is None or nx_record.get("kind") != "nxextract":
        fail("NXExtract path is absent or has the wrong inventory kind")
    if runner_record is None or runner_record.get("kind") != "nxextract-runner":
        fail("NXExtract runner is absent or has the wrong inventory kind")
    if (runtime_env_record is None or
            runtime_env_record.get("kind") != "nxextract-runtime-env"):
        fail("NXExtract runtime helper is absent or has the wrong inventory kind")
    if ui_record is None or ui_record.get("kind") != "nxextract-ui-linux":
        fail("NXExtract UI is absent or has the wrong inventory kind")
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
    for token in (
            "mandatory setup UI graphical renderer confirmed",
            "def assert_visible(self):", "--require-ui"):
        if token not in nx_text:
            fail("NXExtract engine lost mandatory UI attestation: {}".format(
                token
            ))

    runner_text = "\n".join(shell_tokens(
        read_small_text(runner_record["actual_path"], nx["runner_path"]),
        nx["runner_path"],
    ))
    for token in (
            "nxextract.py", "nxextract-runtime-env.sh", "extractor.json",
            "--recipe", "--game-dir", "--require-ui"):
        if token not in runner_text:
            fail("NXExtract runner does not actively reference {}".format(token))
    if sha256_file(runner_record["actual_path"]) != nx["runner_sha256"]:
        fail("NXExtract runner content pin mismatch")
    if sha256_file(runtime_env_record["actual_path"]) != nx["runtime_env_sha256"]:
        fail("NXExtract runtime helper content pin mismatch")
    ui_contract = bind_canonical_nxextract_ui(
        records, config, nx.get("ui_architecture")
    )
    if (sha256_file(ui_record["actual_path"]) != nx["ui_sha256"] or
            nx["ui_sha256"] != ui_contract["sha256"]):
        fail("NXExtract UI content pin mismatch")
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
    validate_nxextract_recipe_arrays(recipe)
    validate_apk_variant_policy(recipe)
    validate_recipe_hooks_static(recipe, records, recipe_record["target"])


def _validate_hook_generic_fallback(value, context):
    """Require a machine-readable path name, never prose that means reject."""
    if not isinstance(value, str) or not HOOK_GENERIC_FALLBACK_RE.fullmatch(value):
        fail(
            "{} must be a machine-readable fallback id (letters, digits, "
            "dot, underscore or dash), not free-form prose".format(context)
        )
    tokens = {
        token for token in re.split(r"[._-]+", value.casefold()) if token
    }
    rejecting = sorted(tokens & HOOK_REJECTING_FALLBACK_TOKENS)
    if rejecting:
        fail(
            "{} names a rejecting fallback ({}); an unknown compatible build "
            "must follow a generic/symbolic path".format(
                context, ", ".join(rejecting)
            )
        )


def _validate_recipe_patch_profile_fallbacks(recipe):
    """Preserve the generic-path requirement without trusting profile hashes."""
    profiles = recipe.get("patch_profiles")
    if not isinstance(profiles, list):
        return
    for index, profile in enumerate(profiles):
        if not isinstance(profile, dict):
            continue
        _validate_hook_generic_fallback(
            profile.get("fallback"),
            "patch_profiles[{}].fallback".format(index),
        )


def _sniff_referenced_hook_json(record, target):
    """Recognize extensionless JSON data without treating binaries as text.

    This is deliberately narrower than ``read_small_text``: only a small,
    strict UTF-8 JSON object/array may make an otherwise unknown suffix part of
    a hook closure.  Declared Linux ELFs, ELF magic, NUL bytes, invalid UTF-8,
    oversized data and JSON scalars are rejected before enqueueing.
    """
    if record.get("kind") in ELF_KINDS:
        return False
    path = record.get("actual_path")
    if not isinstance(path, Path):
        path = Path(path)
    try:
        size = path.stat().st_size
        if size > HOOK_CLOSURE_JSON_SNIFF_MAX_BYTES:
            return False
        with path.open("rb") as stream:
            raw = stream.read(HOOK_CLOSURE_JSON_SNIFF_MAX_BYTES + 1)
    except OSError as exc:
        fail("cannot inspect referenced hook data {}: {}".format(target, exc))
    if (len(raw) != size or len(raw) > HOOK_CLOSURE_JSON_SNIFF_MAX_BYTES or
            raw.startswith(b"\x7fELF") or b"\x00" in raw):
        return False
    try:
        text = raw.decode("utf-8", errors="strict")
        document = json.loads(text)
    except (UnicodeDecodeError, TypeError, ValueError):
        return False
    return isinstance(document, (dict, list))


def _hook_reference_literals(text, target):
    """Extract local path/module literals without executing untrusted hooks."""
    suffix = PurePosixPath(target).suffix.casefold()
    references = set()
    if suffix == ".py":
        try:
            tree = ast.parse(text, filename=target)
        except SyntaxError as exc:
            fail("cannot parse packaged Python hook dependency {}: {}".format(
                target, exc
            ))
        for node in ast.walk(tree):
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                references.add(node.value)
            elif isinstance(node, ast.Import):
                for alias in node.names:
                    references.add(alias.name.replace(".", "/") + ".py")
            elif isinstance(node, ast.ImportFrom):
                if node.module:
                    references.add(node.module.replace(".", "/") + ".py")
                    references.add(node.module.rsplit(".", 1)[-1] + ".py")
                elif node.level:
                    for alias in node.names:
                        references.add(alias.name + ".py")
    elif suffix == ".json" or suffix not in HOOK_CLOSURE_TEXT_SUFFIXES:
        try:
            document = json.loads(text)
        except (TypeError, ValueError):
            document = None

        if (suffix != ".json" and
                not isinstance(document, (dict, list))):
            document = None

        def visit(value):
            if isinstance(value, dict):
                for child in value.values():
                    visit(child)
            elif isinstance(value, list):
                for child in value:
                    visit(child)
            elif isinstance(value, str):
                references.add(value)

        if document is not None:
            visit(document)
        elif suffix != ".json":
            for match in re.finditer(
                    r"(?P<quote>['\"])(?P<value>[^'\"\r\n]{1,2048})(?P=quote)",
                    text):
                references.add(match.group("value"))
    else:
        for match in re.finditer(
                r"(?P<quote>['\"])(?P<value>[^'\"\r\n]{1,2048})(?P=quote)",
                text):
            references.add(match.group("value"))

    # A docstring or a composed os.path.join may hold only a basename. Keep
    # filename-looking literals, but never treat arbitrary prose as a path.
    for match in re.finditer(
            r"(?<![A-Za-z0-9_.-])"
            r"[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*"
            r"(?:\.py|\.sh|\.bash|\.pl|\.lua|\.js|\.json|\.toml|\.ya?ml|\.ini|\.cfg)"
            r"(?![A-Za-z0-9_.-])",
            text):
        references.add(match.group(0))
    return references


def _packaged_hook_closure(hook, records, recipe_target):
    """Resolve the complete reachable packaged text closure of one hook."""
    by_target = {
        record.get("target"): record for record in (records or [])
        if isinstance(record, dict) and isinstance(record.get("target"), str)
    }
    if not isinstance(recipe_target, str):
        return []
    recipe_root = PurePosixPath(recipe_target).parent.as_posix()
    if recipe_root == ".":
        recipe_root = ""

    def expand_local(value, relative_root):
        if not isinstance(value, str):
            return None
        expanded = value.replace("{game_dir}", recipe_root)
        expanded = expanded.replace("{recipe_dir}", recipe_root)
        if "{" in expanded or "}" in expanded or expanded.startswith("/"):
            return None
        pure = PurePosixPath(expanded)
        if any(part in ("", ".", "..") for part in pure.parts):
            return None
        candidate = pure.as_posix()
        if candidate not in by_target and relative_root:
            candidate = (PurePosixPath(relative_root) / pure).as_posix()
        return candidate

    cwd = expand_local(hook.get("cwd", "{game_dir}"), recipe_root)
    if cwd not in by_target:
        cwd = cwd or recipe_root

    queue = []
    queued = set()
    sniffed_json = set()

    def enqueue(target, direct=False):
        record = by_target.get(target)
        if record is None or target in queued:
            return
        suffix = PurePosixPath(target).suffix.casefold()
        if not direct and record.get("kind") != "script" and \
                suffix not in HOOK_CLOSURE_TEXT_SUFFIXES:
            if not _sniff_referenced_hook_json(record, target):
                return
            sniffed_json.add(target)
        if direct and record.get("kind") != "script" and suffix not in {
                ".py", ".sh", ".bash", ".pl", ".lua", ".js"}:
            return
        queued.add(target)
        queue.append(record)

    for value in hook.get("argv", []):
        target = expand_local(value, cwd)
        if target is not None:
            enqueue(target, direct=True)

    closure = []
    total_bytes = 0
    while queue:
        if len(closure) >= HOOK_CLOSURE_MAX_FILES:
            fail("hook {} packaged closure exceeds {} files".format(
                hook.get("id", "?"), HOOK_CLOSURE_MAX_FILES
            ))
        record = queue.pop(0)
        target = record["target"]
        path = record.get("actual_path")
        if not isinstance(path, Path):
            path = Path(path)
        size = path.stat().st_size
        total_bytes += size
        if total_bytes > HOOK_CLOSURE_MAX_BYTES:
            fail("hook {} packaged closure exceeds {} bytes".format(
                hook.get("id", "?"), HOOK_CLOSURE_MAX_BYTES
            ))
        text = read_small_text(path, target, limit=HOOK_CLOSURE_MAX_BYTES)
        if target in sniffed_json:
            try:
                document = json.loads(text)
            except (TypeError, ValueError):
                document = None
            if ("\x00" in text or
                    not isinstance(document, (dict, list))):
                fail("referenced hook data {} changed after JSON sniff".format(
                    target
                ))
        closure.append((record, text))

        parent = PurePosixPath(target).parent.as_posix()
        if parent == ".":
            parent = ""
        for reference in _hook_reference_literals(text, target):
            if not isinstance(reference, str) or len(reference) > 2048:
                continue
            reference = reference.strip().replace("\\", "/")
            if (not reference or reference.startswith("/") or "\x00" in reference or
                    "{" in reference or "}" in reference):
                continue
            pure = PurePosixPath(reference)
            if any(part in ("", ".", "..") for part in pure.parts):
                continue
            candidates = [pure.as_posix()]
            for root in (parent, cwd, recipe_root):
                if root:
                    candidates.append((PurePosixPath(root) / pure).as_posix())
            for candidate in candidates:
                enqueue(candidate)
    return closure


def validate_recipe_hooks_static(recipe, records=None, recipe_target=None):
    """V3 (APK-COMPAT-01): hooks are untrusted code for release purposes.

    Static defence over the declared argv/env and the complete reachable
    packaged text closure (scripts, imported modules and data/spec files):
    equality against undeclared 64-hex literals, signing members, literal
    dotted version tokens, exact size tables and absolute offsets are flagged
    fail-closed (the Terraria dot-4 vs dot-49 lesson). The hook closure may not
    contain any literal 64-hex digest. NXExtract selects authenticated
    patch_profiles before hook execution and exposes only the selected profile
    identity; post-transform integrity belongs to canonical recipe checkpoints,
    output validation or generation_runtime, never an arbitrary source key.
    Hooks in a recipe adopting the V3 blocks must declare a
    contract, and every declared contract is validated by the canonical
    module. Static analysis complements, never replaces, the dynamic
    metamorphic fixtures in the test suites.
    """
    hooks = recipe.get("hooks")
    if not isinstance(hooks, list):
        return
    v3_recipe = any(
        key in recipe
        for key in ("reference_build", "compatibility", "patch_profiles")
    )
    _validate_recipe_patch_profile_fallbacks(recipe)

    for hook in hooks:
        if not isinstance(hook, dict):
            continue
        hook_id = hook.get("id", "?")
        blob = json.dumps(
            {"argv": hook.get("argv", []), "env": hook.get("env", {})},
            sort_keys=True,
        )
        findings = APKCOMPAT.scan_static_suspects(
            blob, "hook {}".format(hook_id)
        )
        if findings:
            fail(findings[0])
        for source_record, source_text in _packaged_hook_closure(
                hook, records, recipe_target):
            target = source_record["target"]
            findings = APKCOMPAT.scan_static_suspects(
                source_text,
                "hook {} source {}".format(hook_id, target),
            )
            if findings:
                fail(findings[0])
        contract = hook.get("contract")
        if contract is None:
            if v3_recipe:
                fail(
                    "hook {} must declare a contract "
                    "(org.nextos.apk-compat.hook-contract/1) in a V3 "
                    "recipe".format(hook_id)
                )
            continue
        _validate_hook_generic_fallback(
            contract.get("fallback"),
            "hook {} contract fallback".format(hook_id),
        )
        contract_findings = []
        APKCOMPAT.validate_hook_contract(
            contract, {hook_id}, contract_findings.append
        )
        if contract_findings:
            fail("hook {} contract: {}".format(
                hook_id, contract_findings[0]))


def strong_internal_apk_anchor(rule):
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


def container_identity_count(value, pattern, context):
    if value is None:
        return 0
    identities = [value] if isinstance(value, str) else value
    if (not isinstance(identities, list) or not identities or
            not all(isinstance(item, str) and re.fullmatch(pattern, item)
                    for item in identities)):
        fail("{} contains an invalid identity list".format(context))
    return len(set(item.lower() for item in identities))


def validate_apk_variant_policy(recipe):
    """Require APK-container compatibility to survive packaging SHA drift."""
    # V3 (APK-COMPAT-01): the canonical shared module is the single source of
    # the container rule -- sha256/crc32 in ANY quantity and exact size are
    # identity, never compatibility. reference_build/compatibility/
    # patch_profiles blocks are validated by the same module.
    canonical_findings = []
    APKCOMPAT.validate_recipe_apk_compat(recipe, canonical_findings.append)
    if canonical_findings:
        fail("; ".join(canonical_findings[:3]))
    containers = []
    content_anchors = []
    for index, rule in enumerate(recipe.get("extract", [])):
        if not isinstance(rule, dict):
            fail("NXExtract recipe extract[{}] must be an object".format(index))
        source = rule.get("source")
        validation = rule.get("validate", {})
        if not isinstance(source, dict) or not isinstance(validation, dict):
            continue
        kind = source.get("kind")
        if kind == "container":
            containers.append((index, validation))
        elif strong_internal_apk_anchor(rule):
            content_anchors.append(index)
    if not containers:
        return
    input_config = recipe.get("input")
    packages = input_config.get("packages") if isinstance(input_config, dict) else None
    if (not isinstance(packages, list) or not packages or
            not all(isinstance(item, str) and item for item in packages)):
        fail("NXExtract container recipe must declare input.packages")
    for index, validation in containers:
        context = "NXExtract container extract[{}]".format(index)
        if "size" in validation:
            fail("{} cannot pin one exact APK size".format(context))
        identity_count = 0
        for field, pattern in (
                ("sha256", r"[0-9a-fA-F]{64}"),
                ("crc32", r"[0-9a-fA-F]{8}")):
            count = container_identity_count(
                validation.get(field), pattern,
                "{} {}".format(context, field),
            )
            if count == 1:
                fail(
                    "{} {} must list at least two APK variants or be omitted "
                    "for content identity".format(context, field)
                )
            identity_count += count
        # Flexible BYO identity (mandatory house rule): every game recipe must
        # accept the owner's own bundle, and each store/version bundle differs,
        # so a fixed per-APK SHA can never be listed. The identity is then the
        # declared Android package (nxextract verifies the container manifest
        # package matches input.packages, required above) plus a magic header
        # and a bounded size. A container proving all three is valid and needs
        # no SHA list or separate content anchor.
        has_magic = any(k in validation for k in ("magic_hex", "magic_ascii"))
        has_bounded_size = "min_size" in validation and "max_size" in validation
        flexible_ok = has_magic and has_bounded_size
        if not identity_count and not content_anchors and not flexible_ok:
            fail(
                "{} needs a declared package with magic and bounded size, a "
                "strong internal SHA-256, or a structural content anchor".format(
                    context)
            )


def exception_allowed(config, used, rule, target):
    key = (rule, target)
    if key not in config["exception_map"]:
        return False
    used.add(key)
    return True


def _structural_secret_verdict(data, logical_path):
    """Ask the structural scanner first; ``None`` means "the regex decides".

    The adapter is strictly ADDITIVE.  A structural ``FAIL`` is a proof and
    rejects immediately -- that is how the grammar catches what a byte regex
    cannot see: a quoted Python literal, a bytes literal, a credential nested
    in a dict or in a JSON subtree.  Every other status returns ``None``:
    ``PASS`` never acquits a payload, because the historical regex authority
    is deliberately broader (it also rejects a bare *name* bound to a
    sensitive identifier), and ``UNSUPPORTED``/``STRUCTURAL_ERROR`` are exactly
    the fail-closed cases the caller's regex must keep owning.  A scanner
    crash degrades to the regex path, never to a pass.
    """
    if not isinstance(logical_path, str) or not logical_path:
        return None
    try:
        result = NXSCAN.scan_bytes(logical_path, bytes(data))
    except Exception:
        return None
    if isinstance(result, dict) and result.get("status") == NXSCAN.FAIL:
        return True
    return None


def _contains_secret_literal(data, logical_path):
    """Return whether *data* contains a credential-like literal.

    Python uses ``name: Type`` for annotations, so applying the generic
    mapping scanner to ``.py``/``.pyi`` sources confuses type names with secret
    values.  Plain and annotated assignments remain covered independently.
    Other text formats keep the existing ``key: value`` protection.
    """
    verdict = _structural_secret_verdict(data, logical_path)
    if verdict is not None:
        return verdict
    if SECRET_LITERAL_RE.search(data):
        return True
    lowered_path = logical_path.casefold()
    if lowered_path.endswith((".py", ".pyi")):
        return PYTHON_ANNOTATED_SECRET_LITERAL_RE.search(data) is not None
    return MAPPING_SECRET_LITERAL_RE.search(data) is not None


def _reject_private_chunk(data, logical_path, text_like):
    if PRIVATE_PATH_RE.search(data):
        fail("{} contains a private home path".format(logical_path))
    if text_like and ADVOCACY_RE.search(data):
        fail("{} advertises funding/donations forbidden in public releases".format(
            logical_path))
    if not text_like:
        return
    for match in IPV4_RE.finditer(data):
        octets = [int(part) for part in match.group(0).split(b".")]
        prefix = data[max(0, match.start() - 96):match.start()]
        dotnet_version = (
            logical_path.lower().endswith(".deps.json") and
            DOTNET_DEPS_VERSION_CONTEXT_RE.search(prefix) is not None
        )
        if all(part <= 255 for part in octets) and not dotnet_version:
            fail("{} contains an IPv4 literal".format(logical_path))
    if _contains_secret_literal(data, logical_path):
        fail("{} embeds a credential/secret literal".format(logical_path))
    if HOST_LITERAL_RE.search(data):
        fail("{} embeds a hostname literal".format(logical_path))


def _scan_private_stream(handle, logical_path, length=None):
    """Scan one independent payload and return the SHA-256 of its bytes."""
    remaining = length
    prefix_size = 4096 if remaining is None else min(4096, remaining)
    prefix = handle.read(prefix_size)
    if remaining is not None:
        remaining -= len(prefix)
    text_like = b"\0" not in prefix
    digest = hashlib.sha256()
    tail = b""
    chunk = prefix
    while chunk:
        digest.update(chunk)
        data = tail + chunk
        _reject_private_chunk(data, logical_path, text_like)
        tail = data[-512:]
        if remaining is None:
            chunk = handle.read(256 * 1024)
        else:
            chunk = handle.read(min(256 * 1024, remaining))
            remaining -= len(chunk)
    if remaining not in (None, 0):
        fail("{} is truncated".format(logical_path))
    return digest.hexdigest()


def _read_nxbundle_line(handle, logical_path):
    line = handle.readline(NXBUNDLE_MAX_HEADER_LINE + 1)
    if (not line or len(line) > NXBUNDLE_MAX_HEADER_LINE or
            not line.endswith(b"\n")):
        fail("{} has a malformed nxbundle-v1 header".format(logical_path))
    return line


def _scan_nxbundle_for_private_data(handle, logical_path):
    """Scan each authenticated nxbundle-v1 member at its own text boundary.

    A seed begins with a potentially large ASCII index followed by concatenated
    payloads.  Classifying the whole file from that index makes every embedded
    ELF look textual and turns protocol constants in binary code into private
    data.  Parse and authenticate the canonical container, then reset the text
    classifier for every member exactly as if its generation copy were scanned.
    """
    header = []
    line = _read_nxbundle_line(handle, logical_path)
    header.append(line)
    if line != NXBUNDLE_MAGIC:
        fail("{} is not an nxbundle-v1 seed".format(logical_path))

    line = _read_nxbundle_line(handle, logical_path)
    header.append(line)
    if re.fullmatch(rb"generation [0-9a-f]{64}\n", line) is None:
        fail("{} has an invalid generation id".format(logical_path))
    generation_id = line[len(b"generation "):-1].decode("ascii")

    line = _read_nxbundle_line(handle, logical_path)
    header.append(line)
    if re.fullmatch(rb"port [a-z0-9][a-z0-9._-]{0,62}\n", line) is None:
        fail("{} has an invalid port id".format(logical_path))
    port_id = line[len(b"port "):-1].decode("ascii")

    line = _read_nxbundle_line(handle, logical_path)
    header.append(line)
    count_match = re.fullmatch(rb"members ([0-9]+)\n", line)
    if count_match is None:
        fail("{} has an invalid member count".format(logical_path))
    count_raw = count_match.group(1)
    if len(count_raw) > 4:
        fail("{} has an out-of-range member count".format(logical_path))
    member_count = int(count_raw)
    if (count_raw != str(member_count).encode("ascii") or
            member_count < 1 or member_count > NXBUNDLE_MAX_MEMBERS):
        fail("{} has an out-of-range member count".format(logical_path))

    records = []
    seen = set()
    seen_folded = set()
    expected_offset = 0
    for _index in range(member_count):
        line = _read_nxbundle_line(handle, logical_path)
        header.append(line)
        match = NXBUNDLE_MEMBER_RE.fullmatch(line)
        if match is None:
            fail("{} has a malformed member record".format(logical_path))
        mode, expected_digest, size_raw, offset_raw, path_raw = match.groups()
        try:
            member_path = path_raw.decode("ascii")
        except UnicodeDecodeError:
            fail("{} has a non-ASCII member path".format(logical_path))
        parts = member_path.split("/")
        if (member_path.startswith("/") or "\\" in member_path or
                any(part in ("", ".", "..") for part in parts) or
                member_path == "commit" or member_path.endswith("/commit")):
            fail("{} has an unsafe member path".format(logical_path))
        folded = member_path.lower()
        if member_path in seen or folded in seen_folded:
            fail("{} has duplicate or case-colliding members".format(logical_path))
        seen.add(member_path)
        seen_folded.add(folded)
        if len(size_raw) > 19 or len(offset_raw) > 19:
            fail("{} has an out-of-range member number".format(logical_path))
        size = int(size_raw)
        offset = int(offset_raw)
        if (size_raw != str(size).encode("ascii") or
                offset_raw != str(offset).encode("ascii") or
                size > (1 << 63) - 1 or offset > (1 << 63) - 1):
            fail("{} has a non-canonical member number".format(logical_path))
        if offset != expected_offset:
            fail("{} has a non-contiguous member offset".format(logical_path))
        expected_offset += size
        records.append((member_path, expected_digest.decode("ascii"), size,
                        mode.decode("ascii")))

    line = _read_nxbundle_line(handle, logical_path)
    header.append(line)
    if line != b"END\n":
        fail("{} has no canonical END marker".format(logical_path))
    header_bytes = handle.tell()
    try:
        total_size = os.fstat(handle.fileno()).st_size
    except OSError as exc:
        fail("cannot size {}: {}".format(logical_path, exc))
    if total_size != header_bytes + expected_offset:
        fail("{} has a truncated or trailing payload".format(logical_path))
    _reject_private_chunk(b"".join(header), logical_path + "!header", True)

    for member_path, expected_digest, size, _mode in records:
        member_logical = "{}!{}".format(logical_path, member_path)
        actual_digest = _scan_private_stream(handle, member_logical, size)
        if actual_digest != expected_digest:
            fail("{} digest does not match its nxbundle record".format(
                member_logical))
    return {
        "generation_id": generation_id,
        "port_id": port_id,
        "members": {
            member_path: {
                "mode": mode,
                "sha256": digest,
                "size": size,
            }
            for member_path, digest, size, mode in records
        },
    }


def scan_text_for_private_data(path, logical_path, kind=None):
    """Scan payload bytes for private host data, secrets and advocacy."""
    try:
        with open(str(path), "rb") as handle:
            if kind == "nxruntime-seed":
                return _scan_nxbundle_for_private_data(handle, logical_path)
            else:
                _scan_private_stream(handle, logical_path)
    except OSError as exc:
        fail("cannot scan {}: {}".format(logical_path, exc))
    return None


def validate_runtime_seed(records, config, seed_descriptors):
    """Require one reconstructible visible seed for every schema-3 runtime.

    The renderer normally emits this member, but source manifests and stages
    are also external inputs.  Enforce the invariant in the nxrelease core so
    a hand-written/stale manifest cannot ship a hidden-only generation that
    ArkOS or a personal rezip can strand as NXU0002/NXU0009.
    """
    nxport = config.get("nxport_manifest")
    if not isinstance(nxport, dict) or nxport.get("schema_version", 1) != 3:
        return
    generation_id = config.get("generation_id")
    if not isinstance(generation_id, str) or not SHA256_RE.fullmatch(
            generation_id):
        fail("generation-v2 runtime seed requires one full generation id")

    port_dir = config["port_dir"]
    store_base = "{}/.nxruntime/generations/".format(port_dir)
    store_root = "{}{}/".format(store_base, generation_id)
    store_records = [
        record for record in records
        if record.get("target", "").startswith(store_base)
    ]
    if not store_records or any(
            not record["target"].startswith(store_root)
            for record in store_records):
        fail("generation-v2 runtime seed requires one exact generation store")

    expected_target = "{}/nxruntime-{}.nxb".format(port_dir, generation_id)
    seed_like = [
        record for record in records
        if (record.get("kind") == "nxruntime-seed" or
            re.fullmatch(
                re.escape(port_dir) + r"/nxruntime-[0-9a-f]{64}\.nxb",
                record.get("target", ""),
            ))
    ]
    if len(seed_like) != 1 or seed_like[0].get("target") != expected_target:
        fail("generation-v2 runtime seed is missing, duplicated or misnamed")
    seed = seed_like[0]
    if seed.get("kind") != "nxruntime-seed" or seed.get("mode") != 0o644:
        fail("generation-v2 runtime seed must be one pinned 0644 nxruntime-seed")
    descriptor = seed_descriptors.get(expected_target)
    if descriptor is None:
        fail("generation-v2 runtime seed was not authenticated as nxbundle-v1")
    if (descriptor.get("generation_id") != generation_id or
            descriptor.get("port_id") != config["package_id"]):
        fail("generation-v2 runtime seed header differs from its port/generation")

    expected_members = {}
    for record in store_records:
        relative = record["target"][len(store_root):]
        if relative == "commit":
            continue
        try:
            size = record["actual_path"].stat().st_size
        except OSError as error:
            fail("generation-v2 runtime seed cannot size {}: {}".format(
                record["target"], error))
        expected_members[relative] = {
            "mode": "{:04o}".format(record["mode"]),
            "sha256": record["sha256"],
            "size": size,
        }
    if descriptor.get("members") != expected_members:
        fail("generation-v2 runtime seed closure differs from the generation store")

    launcher = next(
        (record for record in records
         if record.get("target") == config["launcher"]), None
    )
    if launcher is None:
        fail("generation-v2 runtime seed cannot resolve the generated launcher")
    launcher_text = active_shell_text(read_small_text(
        launcher["actual_path"], config["launcher"]
    ))
    required_fragments = (
        'NXBOOTSTRAP_BUNDLE_NAME="nxruntime-$NXBOOTSTRAP_GENERATION_ID.nxb"',
        "nxbootstrap_bundle_materialize() {",
        'nxbootstrap_bundle_materialize "$GAMEDIR/$NXBOOTSTRAP_BUNDLE_NAME"',
    )
    if any(fragment not in launcher_text for fragment in required_fragments):
        fail("generation-v2 runtime seed launcher lacks cache reconstruction")


def audit_script(path, logical_path, config, used_exceptions):
    text = read_small_text(path, logical_path)
    active_text = "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#")
    )
    if shell_invokes_external_stat(text, logical_path):
        fail("{} calls the external stat command".format(logical_path))
    first_line = text.splitlines()[0] if text.splitlines() else ""
    shell = shell_shebang_interpreter(first_line)
    if shell is None:
        if first_line.startswith("#!"):
            fail("shell file has an unsupported shebang: {}".format(logical_path))
        shell = "sh"
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
    if SWAP_RE.search(active_text):
        fail("{} creates or manages swap/page-cache (mkswap/swapon/zram/"
             "swapfile/drop_caches); ports must never touch the firmware "
             "memory policy".format(logical_path))
    if FRONTEND_RE.search(active_text) or (
            any(token in ("systemctl", "killall", "pkill") for token in tokens) and
            any("emustation" in token.lower() or "emulationstation" in token.lower()
                for token in tokens)):
        fail("{} manages EmulationStation directly".format(logical_path))
    # The canonical capability-gated guest audio shield pins the EMBEDDED
    # Android OpenAL to the loader's own OpenSL bridge; `opensl` is the only
    # value that cannot select a host backend, so it is not a forced driver.
    # Any other ALSOFT_DRIVERS value -- and every SDL_*DRIVER assignment --
    # still requires the adaptive-driver exception.
    def is_forced_driver_token(token):
        if re.match(r"^ALSOFT_DRIVERS=opensl$", token, re.IGNORECASE):
            return False
        return re.match(r"^(?:SDL_(?:VIDEO|AUDIO)_?DRIVER|ALSOFT_DRIVERS)=",
                        token, re.IGNORECASE) is not None

    forced_driver = any(is_forced_driver_token(token) for token in tokens)
    for match in FORCED_DRIVER_RE.finditer(text):
        line_end = text.find("\n", match.start())
        line = text[match.start():line_end if line_end != -1 else len(text)]
        if re.search(r"ALSOFT_DRIVERS=opensl[ \t]*$", line, re.IGNORECASE):
            continue
        forced_driver = True
        break
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


def record_is_shell(record):
    if (record.get("kind") in SHELL_KINDS or
            str(record.get("target", "")).lower().endswith(".sh")):
        return True
    try:
        with open(str(record["actual_path"]), "rb") as handle:
            first = handle.readline(256)
    except OSError:
        return False
    try:
        first_line = first.decode("utf-8").rstrip("\r\n")
    except UnicodeDecodeError:
        return False
    return shell_shebang_interpreter(first_line) is not None


def load_canonical_nxbootstrap_generator(expected_version):
    global _NXBOOTSTRAP_GENERATOR
    if (not NXBOOTSTRAP_GENERATOR_PATH.is_file() or
            NXBOOTSTRAP_GENERATOR_PATH.is_symlink()):
        fail("canonical nxbootstrap generator is missing or unsafe")
    if _NXBOOTSTRAP_GENERATOR is None:
        spec = importlib.util.spec_from_file_location(
            "nxrelease_canonical_nxbootstrap", NXBOOTSTRAP_GENERATOR_PATH
        )
        if spec is None or spec.loader is None:
            fail("cannot load the canonical nxbootstrap generator")
        module = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(module)
        except (OSError, RuntimeError, ValueError) as error:
            fail("cannot load the canonical nxbootstrap generator: {}".format(
                error
            ))
        _NXBOOTSTRAP_GENERATOR = module
    if _NXBOOTSTRAP_GENERATOR.NXBOOTSTRAP_VERSION != expected_version:
        fail("release launcher must be regenerated by nxbootstrap {}".format(
            _NXBOOTSTRAP_GENERATOR.NXBOOTSTRAP_VERSION
        ))
    if expected_version != NXBOOTSTRAP_REQUIRED_VERSION:
        fail("NXRelease {} requires nxbootstrap {}".format(
            TOOL_VERSION, NXBOOTSTRAP_REQUIRED_VERSION
        ))
    return _NXBOOTSTRAP_GENERATOR


def canonical_nxbootstrap_launcher(nxport, expected_version):
    generator = load_canonical_nxbootstrap_generator(expected_version)
    # O manifesto chega como o JSON PUBLICO, sem normalizar. Campos internos
    # como shell_variable/shell_suffix de `options` so' nascem em validate(),
    # entao render_launcher() sobre o JSON cru estoura antes de auditar o ZIP --
    # e estourava por fora do except abaixo, como traceback cru: o
    # nxbootstrap 0.6.29 gera launcher com `options` corretamente, mas so'
    # depois da normalizacao canonica dele.
    #
    # Validar aqui e' de proposito: a normalizacao pertence ao nxbootstrap, e o
    # NXRelease nao deve reimplementar nem adivinhar os campos internos.
    try:
        normalized = generator.validate(nxport)
    except generator.ManifestError as error:
        fail("nxport manifest rejected by nxbootstrap {}: {}".format(
            expected_version, error
        ))
    except Exception as error:  # noqa: BLE001 - erro fechado, nunca traceback
        fail("nxport manifest cannot be normalized by nxbootstrap {}: {}: {}"
             .format(expected_version, type(error).__name__, error))
    try:
        return generator.render_launcher(normalized)
    except generator.ManifestError as error:
        fail("cannot render the canonical nxbootstrap launcher: {}".format(
            error
        ))
    except Exception as error:  # noqa: BLE001 - erro fechado, nunca traceback
        # Um manifesto que passa pela validacao e ainda assim derruba o render
        # e' defeito do contrato entre as duas ferramentas, e o operador precisa
        # do nome da excecao para saber onde olhar -- nao de um traceback.
        fail("cannot render the canonical nxbootstrap launcher: {}: {}".format(
            type(error).__name__, error
        ))


def canonical_nxsplash_contract(architecture, expected_bootstrap_version):
    """Resolve and verify the immutable splash artifact selected by bootstrap."""
    generator = load_canonical_nxbootstrap_generator(
        expected_bootstrap_version
    )
    try:
        _artifact, artifact_sha256 = generator.nxsplash_artifact(architecture)
        manifest_path = generator.NXSPLASH_MANIFEST_PATH
        manifest = load_json(manifest_path, "canonical nxsplash release")
    except (OSError, ValueError, generator.ManifestError) as error:
        fail("cannot resolve canonical nxsplash release: {}".format(error))
    if manifest_path.is_symlink() or not manifest_path.is_file():
        fail("canonical nxsplash release manifest is missing or unsafe")
    source_sha256 = manifest.get("source_sha256")
    if not isinstance(source_sha256, str) or not SHA256_RE.fullmatch(
            source_sha256):
        fail("canonical nxsplash source hash is invalid")
    if manifest.get("duration_ms") != 5000:
        fail("canonical nxsplash duration must remain exactly 5000 ms")
    if manifest.get("version") != NXSPLASH_REQUIRED_VERSION:
        fail("canonical nxsplash version must be {}".format(
            NXSPLASH_REQUIRED_VERSION
        ))
    return {
        "architecture": architecture,
        "duration_ms": 5000,
        "path": NXSPLASH_RUNTIME_NAME,
        "release_manifest_sha256": sha256_file(manifest_path),
        "sha256": artifact_sha256,
        "source_sha256": source_sha256,
        "version": require_string(
            manifest.get("version"), "canonical nxsplash version"
        ),
    }


def verify_self_contained_wrapper(wrapper, wrapper_path, config, nx_arch,
                                  nxport):
    """Verify the 0.6.0 single-launcher shape and its golden-port guarantees."""
    if "# PORTMASTER: {}, {}".format(
            config["package_id"], PurePosixPath(wrapper_path).name
    ) not in wrapper:
        fail("self-contained launcher lacks its PORTMASTER identity")
    if "audio.embedded-openal" in nxport["required_capabilities"]:
        if "ALSOFT_DRIVERS=opensl" not in wrapper:
            fail("nxport declares audio.embedded-openal but the launcher "
                 "does not pin ALSOFT_DRIVERS=opensl (guest audio shield "
                 "missing; regenerate with nxbootstrap 0.6.21+)")
        if "ENV RECEIPT:" not in wrapper:
            fail("nxport declares audio.embedded-openal but the launcher "
                 "lacks the one-line audio ENV RECEIPT")
    if re.search(r"@[A-Z0-9_]+@", wrapper):
        fail("self-contained launcher has unresolved template tokens")
    active = active_shell_text(wrapper)
    for token in ("control.txt", "get_controls", "pm_platform_helper",
                  "nxbootstrap_finish",
                  'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"',
                  "command ls -Lldn /proc/self/fd/9",
                  '"$NXBOOTSTRAP_LOCK_FILE" -ef /proc/self/fd/9',
                  '[ "$NXBOOTSTRAP_FINISHED" = 0 ] || return 0',
                  "nxbootstrap_abort_before_game 129",
                  "nxbootstrap_abort_before_game 130",
                  "nxbootstrap_abort_before_game 143",
                  "NXBOOTSTRAP_CHILD_STARTTIME=${20}", "flock -n 9",
                  'wait "$game_pid"', "NXBOOTSTRAP_SHUTDOWN_TICKS=10",
                  'builtin kill -KILL "$game_pid"',
                  "trap '' INT TERM HUP", "printf '\\033c'"):
        if token not in active:
            fail("self-contained launcher is missing {}".format(token))
    if ('GAMEDIR="/$directory/ports/{}"'.format(config["package_id"])
            not in active):
        fail("self-contained launcher does not derive GAMEDIR from $directory")

    tokens = shell_tokens(wrapper, wrapper_path)
    binary_assignment = "BIN=$GAMEDIR/{}".format(nxport["executable"])
    try:
        binary_index = tokens.index(binary_assignment)
    except ValueError:
        fail("self-contained launcher lacks its executable assignment")
    generator = load_canonical_nxbootstrap_generator(
        config["launcher_contract"]["version"]
    )
    executable_preflight = (
        "# The executable is itself required and is touched before the owner-data phase."
    )
    if (wrapper.count(executable_preflight) != 1 or
            wrapper.count("NXBOOTSTRAP_EXECUTABLE=") != 3):
        fail("self-contained launcher executable preflight differs from nxport.json")
    nxextract_block = generator.render_nxextract_block(nxport)
    if wrapper.count(nxextract_block) != 1:
        fail("self-contained launcher NXExtract policy differs from nxport.json")
    required_block = generator.render_required_files_block(nxport)
    if (wrapper.count(required_block) != 1 or
            wrapper.count("NXBOOTSTRAP_REQUIRED_FILES=") != 1):
        fail("self-contained launcher required_files gate differs from nxport.json")
    splash_block = None
    if bootstrap_requires_nxsplash(config["launcher_contract"]["version"]):
        splash_block = generator.render_splash_block(nxport)
        if wrapper.count(splash_block) != 1:
            fail("self-contained launcher nxsplash handoff differs from nxport.json")
        if re.search(
                r"NXSPLASH_(?:DISABLE|SKIP)|--(?:disable|skip)", wrapper):
            fail("self-contained launcher exposes a nxsplash removal switch")
    executable_preflight_index = wrapper.index(executable_preflight)
    nxextract_index = wrapper.index(nxextract_block)
    required_index = wrapper.index(required_block)
    binary_line = 'BIN="$GAMEDIR/{}"'.format(nxport["executable"])
    if wrapper.count(binary_line) != 1:
        fail("self-contained launcher executable assignment differs from nxport.json")
    binary_char_index = wrapper.index(binary_line)
    if splash_block is not None:
        splash_index = wrapper.index(splash_block)
        adapter_boundary = wrapper.index("# Optional per-port adapter")
        private_library_boundary = wrapper.index("# Host phase ends here")
        if not (executable_preflight_index < binary_char_index <
                nxextract_index < required_index < splash_index <
                adapter_boundary < private_library_boundary):
            fail("self-contained launcher BYO/payload/splash phases are out of order")
    else:
        legacy_order = (executable_preflight_index < nxextract_index <
                        required_index < binary_char_index)
        dialog_handoff_order = (
            executable_preflight_index < binary_char_index <
            nxextract_index < required_index
        )
        if not (legacy_order or dialog_handoff_order):
            fail("self-contained launcher BYO/payload phases are out of order")
    prepare_script = nxport["prepare_script"]
    if prepare_script:
        prepare_call = 'bash "$GAMEDIR/{}"'.format(prepare_script)
        if (wrapper.count(prepare_call) != 1 or
                not nxextract_index < wrapper.index(prepare_call) < required_index):
            fail("self-contained launcher prepare phase is out of order")
    adapter_indices = [
        index for index, token in enumerate(tokens)
        if token == "$GAMEDIR/port-env.sh"
    ]
    platform_indices = [
        index for index, token in enumerate(tokens[:-1])
        if token == "pm_platform_helper" and tokens[index + 1] == "$BIN"
    ]
    launch_indices = [
        index for index in range(len(tokens) - 3)
        if tokens[index:index + 4] == ["if", "[", "-n", "$BIN_PRELOAD"]
    ]
    if not launch_indices and nxport.get("sdl_provider") == "system":
        # nxbootstrap 0.7.5+ with sdl_provider=system launches through the
        # provider-owned subshell: BIN_PRELOAD is neutralized in the preamble
        # and the child env is derived from NXBOOTSTRAP_SYSTEM_SDL_*.  The
        # boundary token of that canonical block is the child-preload guard;
        # the legacy quad no longer exists anywhere in the wrapper.
        launch_indices = [
            index for index in range(len(tokens) - 3)
            if tokens[index:index + 4] ==
            ["if", "[", "-n", "$NXBOOTSTRAP_SYSTEM_SDL_CHILD_LD_PRELOAD"]
        ]
    if not adapter_indices or len(platform_indices) != 1:
        fail("self-contained launcher lacks its mutable hook boundary")
    if len(launch_indices) != 1:
        fail("self-contained launcher lacks its supervised launch boundary")
    mutable_hook_index = max(
        binary_index, max(adapter_indices), platform_indices[0]
    )
    launch_index = launch_indices[0]
    expected_exports = {
        "NXCOMPAT_PORT_ID": config["package_id"],
        # V3-UPDATE-01: the guest's lexical identity is the LOGICAL game dir
        # (the frontend's /roms/... root), not the pwd -P physical swap.
        "NXCOMPAT_GAME_DIR": "$NXBOOTSTRAP_LOGICAL_GAMEDIR",
        "NXCOMPAT_REQUIRED_CAPABILITIES": "\n".join(
            nxport["required_capabilities"]),
        "NXCOMPAT_ENABLED_QUIRKS": "\n".join(nxport["enabled_quirks"]),
        "NXCOMPAT_RUNTIME_REPORT": nxport["runtime_report"],
    }
    for name, expected in expected_exports.items():
        prefix = name + "="
        assignments = [
            (index, token[len(prefix):])
            for index, token in enumerate(tokens)
            if token.startswith(prefix)
        ]
        if len(assignments) != 1:
            fail("self-contained launcher must assign {} exactly once".format(
                name
            ))
        assignment_index, actual = assignments[0]
        if assignment_index == 0 or tokens[assignment_index - 1] != "export":
            fail("self-contained launcher does not export {}".format(name))
        if assignment_index <= mutable_hook_index:
            fail("self-contained launcher exports {} before mutable hooks".format(
                name
            ))
        if assignment_index >= launch_index:
            fail("self-contained launcher exports {} after loader launch".format(
                name
            ))
        if actual != expected:
            fail("self-contained launcher export {} differs from nxport.json".format(
                name
            ))
        if any(tokens[index:index + 2] == ["unset", name]
               for index in range(len(tokens) - 1)):
            fail("self-contained launcher unsets {}".format(name))
    canonical_wrapper = canonical_nxbootstrap_launcher(
        nxport, config["launcher_contract"]["version"]
    )
    if wrapper != canonical_wrapper:
        fail("self-contained launcher differs from the canonical nxbootstrap render")
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
    nxport_required_keys = {
        "schema_version", "id", "title", "launcher_name", "architecture",
        "executable", "argument_mode", "home_mode", "nxextract",
        "required_files", "private_library_paths", "prepare_script",
        "required_capabilities", "enabled_quirks", "runtime_report",
    }
    nxport_optional_keys = {
        "language", "options", "execution_roles", "generation_runtime",
        "sdl_provider", "video_proof", "owner_runtime",
    }
    unknown_nxport = sorted(
        set(manifest) - nxport_required_keys - nxport_optional_keys
    )
    if unknown_nxport:
        fail("nxbootstrap config has unknown field(s): {}".format(
            ", ".join(unknown_nxport)
        ))
    if not nxport_required_keys.issubset(manifest):
        missing = sorted(nxport_required_keys - set(manifest))
        fail("canonical nxbootstrap config is missing field(s): {}".format(
            ", ".join(missing)
        ))
    nxport_schema_version = manifest.get("schema_version")
    if type(nxport_schema_version) is not int or \
            nxport_schema_version not in NXPORT_SCHEMA_VERSIONS:
        fail("nxbootstrap config schema_version must be 2 or 3")
    generation_runtime = manifest.get("generation_runtime")
    if nxport_schema_version == 2 and generation_runtime is not None:
        fail("nxbootstrap schema 2 cannot declare generation_runtime")
    if nxport_schema_version == 3 and \
            (not isinstance(generation_runtime, list) or not generation_runtime):
        fail("nxbootstrap schema 3 requires non-empty generation_runtime")
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
    config["nxport_title"] = title
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
    if (nx_version not in NXEXTRACT_ENGINES or
            nx_version != config["nxextract"]["version"]):
        fail("nxbootstrap config must pin the packaged NXExtract engine "
             "(suportadas: {})".format(
                 ", ".join(NXEXTRACT_SUPPORTED_VERSIONS)))
    generator = load_canonical_nxbootstrap_generator(contract["version"])
    try:
        execution_roles = generator.validate_execution_roles(
            manifest, nx_arch, nx_executable, nx_mode
        )
    except (OSError, ValueError, generator.ManifestError) as error:
        fail("nxbootstrap config execution_roles violate the canonical contract: {}".format(
            error
        ))
    try:
        options = generator.validate_options(manifest)
    except (ValueError, generator.ManifestError) as error:
        fail("nxbootstrap config options violate the canonical contract: {}".format(
            error
        ))
    declared_options = manifest.get("options")
    if declared_options is not None:
        canonical_options = [
            {key: option[key] for key in
             ("id", "label", "values", "default", "environment")}
            for option in options
        ]
        declared_normalized = [
            dict(option, label=option.get("label", option.get("id")))
            if isinstance(option, dict) else option
            for option in declared_options
        ]
        if sorted(declared_normalized,
                  key=lambda item: item.get("id", "")) != canonical_options:
            fail("nxbootstrap config options are not canonical")
    config["options"] = options
    if "execution_roles" in manifest:
        if execution_roles is None or manifest["execution_roles"] != execution_roles:
            fail("nxbootstrap config execution_roles are not canonical")
    elif execution_roles is not None:
        fail("nxbootstrap config silently enabled execution_roles")
    config["execution_roles"] = execution_roles
    config["nxport_manifest"] = manifest
    extractor_role = (
        execution_roles.get("extractor") if execution_roles is not None else None
    )
    extractor_arch = (
        extractor_role["architecture"] if extractor_role is not None else nx_arch
    )
    bind_canonical_nxextract_ui(records, config, extractor_arch)
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
    if bootstrap_requires_nxsplash(contract["version"]):
        if (normalized_required.count(NXSPLASH_RUNTIME_NAME) != 1 or
                len(normalized_required) < 2 or
                normalized_required[1] != NXSPLASH_RUNTIME_NAME):
            fail("nxbootstrap config must pin mandatory nxsplash exactly once "
                 "after the executable")
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
    language = manifest.get("language")
    if language is not None:
        language = require_object(language, "nxbootstrap config language")
        require_keys(language, ("default", "supported"),
                     "nxbootstrap config language")
        if set(language) != {"default", "supported"}:
            fail("nxbootstrap config language requires default and supported")
        language_default = require_string(
            language.get("default"), "nxbootstrap config language.default"
        )
        language_supported = language.get("supported")
        if (not isinstance(language_supported, list) or
                not language_supported or len(language_supported) > 32 or
                any(not isinstance(item, str) or
                    not NXPORT_LANGUAGE_RE.fullmatch(item)
                    for item in language_supported)):
            fail("nxbootstrap config language.supported is invalid")
        if len(language_supported) != len(set(language_supported)):
            fail("nxbootstrap config language.supported contains duplicates")
        if (language_default != "auto" and
                language_default not in language_supported):
            fail("nxbootstrap config language.default is unsupported")
    runtime_report = manifest.get("runtime_report")
    if runtime_report not in ("log", "log-and-logo"):
        fail("nxbootstrap config runtime_report is invalid")
    sdl_provider = manifest.get("sdl_provider")
    if sdl_provider is not None and sdl_provider != "system":
        fail("nxbootstrap config sdl_provider must be system when declared")
    config["sdl_provider"] = sdl_provider
    video_proof = manifest.get("video_proof")
    if video_proof is not None and video_proof != "required":
        fail("nxbootstrap config video_proof must be required when declared")
    config["video_proof"] = video_proof
    executable_target = PurePosixPath(config["port_dir"], nx_executable).as_posix()
    executable_record = by_target.get(executable_target)
    if (executable_record is None or
            executable_record.get("kind") not in LINUX_ELF_KINDS):
        fail("nxbootstrap executable must be a packaged classified Linux ELF")
    if executable_record.get("architecture") != nx_arch:
        fail("nxbootstrap architecture does not match its executable ELF")

    if bootstrap_requires_nxsplash(contract["version"]):
        splash_target = PurePosixPath(
            config["port_dir"], NXSPLASH_RUNTIME_NAME
        ).as_posix()
        splash_records = [
            record for record in records
            if record.get("kind") == "nxsplash-linux"
        ]
        splash_record = by_target.get(splash_target)
        if len(splash_records) != 1 or splash_record is None:
            fail("release must contain exactly one classified nxsplash helper")
        if splash_record.get("kind") != "nxsplash-linux":
            fail("mandatory nxsplash helper has the wrong inventory kind")
        if splash_record.get("mode") != 0o755:
            fail("mandatory nxsplash helper must use mode 0755")
        splash_arch = (
            execution_roles["splash"]["architecture"]
            if execution_roles is not None else nx_arch
        )
        if splash_record.get("architecture") != splash_arch:
            fail("mandatory nxsplash helper architecture differs from its execution role")
        splash_contract = canonical_nxsplash_contract(
            splash_arch, contract["version"]
        )
        splash_contract["path"] = splash_target
        actual_splash_sha256 = sha256_file(splash_record["actual_path"])
        if actual_splash_sha256 != splash_contract["sha256"]:
            fail("mandatory nxsplash helper differs from the canonical release")
        expected_splash_sha256 = splash_record.get("expected_sha")
        if (expected_splash_sha256 is not None and
                expected_splash_sha256 != splash_contract["sha256"]):
            fail("mandatory nxsplash files[] pin differs from the canonical release")
        archived_splash = config.get("nxsplash")
        if archived_splash is not None and archived_splash != splash_contract:
            fail("archived nxsplash contract differs from the canonical release")
        config["nxsplash"] = splash_contract

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
            wrapper, wrapper_path, config, nx_arch, manifest
        )
        canonical_paths = [wrapper_path]
        # V3-UPDATE-01: the shipped generation store carries a byte-identical
        # copy of the canonical launcher. It is accepted ONLY as that exact
        # copy — any divergence fails closed.
        wrapper_sha = sha256_file(by_target[wrapper_path]["actual_path"])
        launcher_basename = wrapper_path.rsplit("/", 1)[-1]
        for target, record in sorted(by_target.items()):
            if record.get("kind") != "nxruntime-generation":
                continue
            if not target.endswith("/files/launcher/" + launcher_basename):
                continue
            if sha256_file(record["actual_path"]) != wrapper_sha:
                fail("generation launcher copy differs from the canonical "
                     "launcher: {}".format(target))
            canonical_paths.append(target)
        config["canonical_bootstrap_paths"] = tuple(canonical_paths)
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
        "NXPORT_SCHEMA_VERSION": str(nxport_schema_version),
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


def validate_portmaster_item_list(value, context, inventory_paths):
    if not isinstance(value, list):
        fail("{} must be an array".format(context))
    normalized_items = []
    for index, item in enumerate(value):
        item, directory_hint = safe_top_level_item(
            item, "{}[{}]".format(context, index)
        )
        is_file = item in inventory_paths
        is_directory = any(
            candidate.startswith(item + "/") for candidate in inventory_paths
        )
        if directory_hint != is_directory:
            fail("PortMaster port.json item {} has the wrong directory suffix".format(
                item
            ))
        if not is_file and not is_directory:
            fail("PortMaster port.json item {} is absent from the package".format(
                item
            ))
        if is_file and is_directory:
            fail("PortMaster package has a file/directory collision at {}".format(
                item
            ))
        normalized_items.append(item)
    if len(set(normalized_items)) != len(normalized_items):
        fail("{} contains duplicates".format(context))
    return normalized_items


def validate_portmaster_runtime(attributes):
    runtime = attributes.get("runtime", [])
    if not isinstance(runtime, list):
        fail("PortMaster port.json attr.runtime must be an array")
    if "runtime" in attributes and not runtime:
        fail("PortMaster port.json empty attr.runtime array must be omitted")
    normalized_runtime = []
    for index, value in enumerate(runtime):
        normalized_runtime.append(require_string(
            value, "PortMaster port.json attr.runtime[{}]".format(index)
        ))
    if len(normalized_runtime) != len(set(normalized_runtime)):
        fail("PortMaster port.json attr.runtime contains duplicates")
    return normalized_runtime


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
    if port_json_declaration is None:
        fail("PortMaster port.json declaration is required")
    if port_json_declaration is not None:
        record = by_target[port_json_declaration["path"]]
        port_json = require_object(
            load_json(record["actual_path"], "PortMaster port.json"),
            "PortMaster port.json",
        )
        require_keys(
            port_json,
            ("version", "name", "items", "items_opt", "attr", "status",
             "files", "source", "md5"),
            "PortMaster port.json",
        )
        missing_root = sorted(
            {"version", "name", "items", "items_opt", "attr"} - set(port_json)
        )
        if missing_root:
            fail("PortMaster port.json is missing field(s): {}".format(
                ", ".join(missing_root)
            ))
        schema_version = port_json.get("version")
        if isinstance(schema_version, bool) or schema_version != 4:
            fail("PortMaster port.json version must be 4")
        if port_json.get("name") != config["package_id"] + ".zip":
            fail("PortMaster port.json name must be {}.zip".format(config["package_id"]))
        normalized_items = validate_portmaster_item_list(
            port_json["items"], "PortMaster port.json items", inventory_paths
        )
        normalized_optional = validate_portmaster_item_list(
            port_json["items_opt"], "PortMaster port.json items_opt",
            inventory_paths,
        )
        overlap = sorted(set(normalized_items) & set(normalized_optional))
        if overlap:
            fail("PortMaster port.json items/items_opt overlap at {}".format(
                overlap[0]
            ))
        for required_item in (config["launcher"], config["port_dir"]):
            if required_item not in normalized_items:
                fail("PortMaster port.json items omit {}".format(required_item))
        actual_top_level = {
            PurePosixPath(path).parts[0] for path in inventory_paths
        }
        declared_top_level = set(normalized_items) | set(normalized_optional)
        if actual_top_level != declared_top_level:
            missing = sorted(actual_top_level - declared_top_level)
            extra = sorted(declared_top_level - actual_top_level)
            detail = missing[0] if missing else extra[0]
            fail("PortMaster port.json top-level inventory differs at {}".format(
                detail
            ))
        attributes = require_object(port_json.get("attr"), "PortMaster port.json attr")
        require_keys(
            attributes,
            ("title", "desc", "inst", "genres", "porter", "image", "rtr",
             "exp", "runtime", "min_glibc", "reqs", "arch"),
            "PortMaster port.json attr",
        )
        missing_attributes = sorted(
            {"title", "arch", "min_glibc"} - set(attributes)
        )
        if missing_attributes:
            fail("PortMaster port.json attr is missing field(s): {}".format(
                ", ".join(missing_attributes)
            ))
        title = require_string(
            attributes.get("title"), "PortMaster port.json attr.title"
        )
        if title != config.get("nxport_title"):
            fail("PortMaster port.json title differs from nxport.json")
        validate_portmaster_runtime(attributes)
        declared_arches = attributes.get("arch")
        if (not isinstance(declared_arches, list) or not declared_arches or
                any(not isinstance(item, str) or not item
                    for item in declared_arches) or
                len(declared_arches) != len(set(declared_arches))):
            fail("PortMaster port.json attr.arch must be a unique string array")
        expected_arches = set()
        for candidate in records:
            if candidate.get("kind") in LINUX_ELF_KINDS:
                expected_arches.add(
                    "armhf" if candidate.get("architecture") == "armv7" else
                    candidate.get("architecture")
                )
        if expected_arches != set(declared_arches):
            fail("PortMaster port.json attr.arch differs from packaged Linux architecture(s)")
        min_glibc = attributes.get("min_glibc")
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


def validate_installation_document(records, config):
    """Require the public, pinned bilingual installation contract.

    Detailed owner-data identities remain port-specific, but every public ZIP
    must expose the same stable path and both language sections.  This check is
    repeated against source files, the stage and bytes reopened from the ZIP.
    """
    target = config["port_dir"] + "/INSTALLATION.md"
    record = next((item for item in records if item["target"] == target), None)
    if record is None or record.get("kind") != "payload":
        fail("public package lacks mandatory {0}".format(target))
    if record.get("mode") != 0o644:
        fail("INSTALLATION.md must use mode 0644")
    text = read_small_text(record["actual_path"], target)
    if not re.search(r"(?mi)^##[ \t]+English[ \t]*$", text):
        fail("INSTALLATION.md lacks the English section")
    if not re.search(r"(?mi)^##[ \t]+Portugu(?:ê|e)s[ \t]*$", text):
        fail("INSTALLATION.md lacks the Portuguese section")
    if len(re.sub(r"\s+", "", text)) < 80:
        fail("INSTALLATION.md is too short to be an installation contract")
    # The generator's scaffold says, in so many words, that it is not a release
    # document. It shipped once because this gate only checked for two headings
    # and a minimum length, so the owner-data placeholders and the "replace me"
    # preamble reached a published archive. Refuse them by name.
    scaffold_markers = (
        "REQUIRED BEFORE RELEASE",
        "OBRIGATÓRIO ANTES DA RELEASE",
        "generated, non-release scaffold",
        "scaffold gerado e ainda não public",
    )
    for marker in scaffold_markers:
        if marker in text:
            fail("INSTALLATION.md still carries the generator scaffold "
                 "placeholder: {0!r}".format(marker))
    # A BYO-data port must tell the owner which game version was tested. The
    # extraction recipe already carries that version; the installation
    # contract has to name it, in both languages, or the reader cannot know
    # which copy of the game the port was validated against.
    recipe_version = None
    if isinstance(config.get("nxextract"), dict):
        recipe_path = config["nxextract"].get("recipe_path")
        recipe_record = next((item for item in records
                              if item["target"] == recipe_path), None)
        if recipe_record is not None:
            try:
                recipe = json.loads(read_small_text(recipe_record["actual_path"],
                                                    recipe_path))
                recipe_version = str(recipe.get("version") or "")
            except Exception:  # noqa: BLE001 - reported by the recipe gate itself
                recipe_version = None
    if recipe_version:
        # "1.4.12-universal-1" -> the game version is the leading dotted part.
        game_version = re.match(r"^v?(\d+(?:\.\d+)+)", recipe_version)
        if game_version and game_version.group(1) not in text:
            fail("INSTALLATION.md does not name the tested game version {0} "
                 "that the extraction recipe validates".format(
                     game_version.group(1)))


def validate_owner_data_contract(records, config):
    """V3 (GAMEDATA-DIR-01): three-boundary owner-data coherence.

    Runs on the manifest sources, the staged set and the bytes reopened from
    the ZIP. The marker must be real and non-empty, INSTALLATION.md must
    document the exact <port>/gamedata/ path, and the recipe search_dirs must
    look in the same place the instructions send the owner to.
    """
    nxblock = config.get("nxextract")
    if not isinstance(nxblock, dict):
        return
    port_dir = config["port_dir"]
    marker_target = port_dir + "/gamedata/README.txt"
    record = next(
        (item for item in records if item["target"] == marker_target), None
    )
    if record is None:
        fail("NXExtract package lost {} at an audit boundary".format(
            marker_target))
    marker_text = read_small_text(record["actual_path"], marker_target)
    if not marker_text.strip():
        fail("gamedata/README.txt must not be empty")
    installation_target = port_dir + "/INSTALLATION.md"
    installation = next(
        (item for item in records if item["target"] == installation_target),
        None,
    )
    if installation is not None:
        installation_text = read_small_text(
            installation["actual_path"], installation_target
        )
        if (port_dir + "/gamedata/") not in installation_text:
            fail(
                "INSTALLATION.md must document the exact owner-data path "
                "{}/gamedata/ that the package materializes".format(port_dir)
            )
    recipe_record = next(
        (item for item in records
         if item["target"] == nxblock.get("recipe_path")), None
    )
    if recipe_record is not None:
        try:
            recipe = json.loads(read_small_text(
                recipe_record["actual_path"], nxblock.get("recipe_path")
            ))
        except ValueError:
            return
        input_config = recipe.get("input")
        search_dirs = None
        if isinstance(input_config, dict):
            search_dirs = input_config.get("search_dirs")
        if search_dirs is not None and (
            not isinstance(search_dirs, list)
            or not search_dirs
            or search_dirs[0] != "gamedata"
        ):
            fail(
                "extractor.json input.search_dirs must place gamedata first; "
                "the installed instructions and the extractor must agree on "
                "the owner-data directory"
            )


def validate_video_receipt(elf_results, config):
    """D2 (E4): um loader que ja emite o frame-proof do nxgl tem que emitir
    tambem o recibo unico `VIDEO:` do nxgl 0.2.8 (motivo nomeado para tela
    preta). Escopo automatico: ELF sem adapter de frame-proof fica fora; um
    rebuild com o adapter vendorado antigo e exatamente o que deve reprovar."""
    for item in elf_results:
        markers = _RECEIPT_MARKERS.get(item["path"])
        if not markers:
            continue
        has_frame_proof, has_video = markers
        if has_frame_proof and not has_video:
            fail("ELF {} emits the nxgl frame-proof verdict but not the "
                 "single-line `VIDEO:` receipt; update the vendored nxgl "
                 "frame-proof adapter to 0.2.8+ before release".format(
                     item["path"]))


def validate_symbol_floor(elf_results, config):
    """3.1 (E4): classe de campo `undefined symbol: Mix_PlayChannel`. Para cada
    familia de biblioteca com piso conhecido (exports garantidos pela CFW mais
    antiga suportada), todo import UND nao-WEAK cujo nome pertence ao namespace
    da familia tem que existir no piso ou nos exports de um ELF EMPACOTADO. A
    checagem so vale quando o ELF declara NEEDED daquela familia e nao a embute
    -- carga por dlopen/escopo global fica de fora de proposito. Simbolos fora
    dos namespaces (glibc versionada, GL de blob) sao policiados pelos gates de
    versao e pelo recibo VIDEO:, nunca aqui: zero falso-positivo por construcao.

    V4-03B: para a familia CORE da SDL2 a decisao e por VERSAO DE NASCIMENTO,
    lida da autoridade unica framework/nxabi/sdl2-symbol-floor.tsv: todo
    import direto acima do piso universal SDL 2.0.4 (classe
    SDL_JoystickGetVendor/Product, SDL 2.0.6) reprova ja no primeiro
    preflight somente leitura, antes de stage/ZIP. Pertencer a allowlist de
    SONAME ou existir no firmware mais novo nunca equivale a cumprir o piso;
    a rota canonica para APIs pos-piso e o resolver opcional do nxcompat
    0.4.0, sem import direto. Este gate nao tem waiver."""
    floors = load_symbol_floors()
    bundled_exports = set()
    bundled_sonames = set()
    for item in elf_results:
        symbols = _DYNAMIC_SYMBOLS.get(item["path"])
        if symbols:
            bundled_exports.update(symbols[1])
        if item.get("soname"):
            bundled_sonames.add(item["soname"])
    for item in elf_results:
        symbols = _DYNAMIC_SYMBOLS.get(item["path"])
        if not symbols or not symbols[0]:
            continue
        undefined = symbols[0]
        # V4-05A (audit divergence 2): the SDL2-core floor reaches EVERY
        # applicable undefined SDL_* import, even when the SDL arrives
        # indirectly and the ELF carries no direct DT_NEEDED of the core --
        # exactly the reach nxabi already had, and the SAME shared decision
        # function, so the two consumers can never disagree. The documented
        # exclusions stay: an ELF that resolves against a bundled SDL3, an
        # ELF that packages the SDL2 core itself, and symbols exported by a
        # bundled ELF.
        sdl_undefined = sorted(
            name for name in undefined
            if name.startswith("SDL_") and name not in bundled_exports)
        sdl3_needed = any(
            needed_name.startswith("libSDL3")
            for needed_name in item.get("needed", ()))
        if (sdl_undefined and not sdl3_needed and
                SDL_CORE_SONAME not in bundled_sonames and
                item.get("soname") != SDL_CORE_SONAME):
            authority = load_sdl_symbol_authority()
            decide = _nxabi_module().decide_sdl_floor
            for name, verdict, since in decide(
                    sdl_undefined, authority["table"], SDL_PUBLIC_FLOOR):
                if verdict == "unknown":
                    fail("ELF {} imports {} which is absent from the SDL "
                         "symbol authority {} (sha256 {}); an unproven "
                         "symbol cannot meet the declared universal "
                         "floor SDL {}".format(
                             item["path"], name, authority["authority"],
                             authority["sha256"], SDL_PUBLIC_FLOOR))
                if verdict == "post-floor":
                    fail("ELF {} imports {} born in SDL {} above the "
                         "declared universal floor SDL {}; a post-floor "
                         "SDL API must be reached through the nxcompat "
                         "optional-SDL resolver, never by direct import "
                         "(authority {} sha256 {})".format(
                             item["path"], name, since,
                             SDL_PUBLIC_FLOOR, authority["authority"],
                             authority["sha256"]))
        for soname, matcher in SYMBOL_FLOOR_FAMILIES:
            if soname not in item.get("needed", ()) or soname in bundled_sonames:
                continue
            if soname == SDL_CORE_SONAME:
                # Decided above for every ELF, direct or indirect.
                continue
            floor = floors.get(soname)
            if floor is None:
                continue
            missing = sorted(
                name for name in undefined
                if matcher(name) and name not in floor
                and name not in bundled_exports)
            if missing:
                fail("ELF {} imports {} symbols missing from the {} floor "
                     "guaranteed by the oldest supported firmware: {}. These "
                     "resolve on SOME devices and die with `undefined symbol` "
                     "on older ones; use an older API, bundle the library, or "
                     "extend the floor with proof.".format(
                         item["path"], len(missing), soname,
                         ", ".join(missing[:8])))


def _zlib_symbol(name):
    return (name.startswith(("gz", "inflate", "deflate", "compress",
                             "uncompress", "crc32", "adler32", "zlib"))
            or name in ("zError", "get_crc_table", "zcalloc", "zcfree"))


def _openal_symbol(name):
    return ((name.startswith("al") or name.startswith("alc"))
            and len(name) > 2 and name[2].isupper()
            or name.startswith("alc") and len(name) > 3 and name[3].isupper())


SYMBOL_FLOOR_FAMILIES = (
    ("libSDL2-2.0.so.0", lambda name: name.startswith("SDL_")),
    ("libSDL2_mixer-2.0.so.0", lambda name: name.startswith("Mix_")),
    ("libSDL2_image-2.0.so.0", lambda name: name.startswith("IMG_")),
    ("libSDL2_ttf-2.0.so.0", lambda name: name.startswith("TTF_")),
    ("libfreetype.so.6", lambda name: name.startswith("FT_")),
    ("libopenal.so.1", _openal_symbol),
    ("libz.so.1", _zlib_symbol),
)


def validate_generation_store_elf_mirrors(records, config):
    """Bind every Linux ELF in generation-v2 storage to its live member.

    The rollback store is part of the public package, so its ELF copies need
    the normal GLIBC/ABI audit.  They are mirrors, not independent providers:
    logical path, mode, SHA-256 and all declared ELF metadata must be exactly
    those of the authenticated live generation_runtime entry.
    """
    prefix = config["port_dir"] + "/.nxruntime/generations/"
    mirror_records = [
        record for record in records
        if record["kind"] == "nxruntime-generation-linux"
    ]
    for mirror in mirror_records:
        if not mirror["target"].startswith(prefix):
            fail("nxruntime-generation-linux is allowed only inside the "
                 "authenticated generation store: {}".format(
                     mirror["target"]))
    store_records = [
        record for record in records if record["target"].startswith(prefix)
    ]
    if not store_records:
        return
    nxport = config.get("nxport_manifest")
    runtime = nxport.get("generation_runtime") \
        if isinstance(nxport, dict) else None
    runtime_by_path = {}
    if isinstance(runtime, list):
        for index, member in enumerate(runtime):
            context = "nxport generation_runtime[{}]".format(index)
            member = require_object(member, context)
            path = safe_relative(member.get("path"), context + ".path")
            mode = member.get("mode")
            if mode not in ("0644", "0755"):
                fail(context + ".mode is invalid")
            runtime_by_path[path] = {
                "mode": int(mode, 8),
                "sha256": parse_sha256(
                    member.get("sha256"), context + ".sha256"
                ),
            }
    by_target = {record["target"]: record for record in records}
    metadata_fields = (
        "architecture", "build_profile", "needed", "soname", "provenance",
    )
    for store in store_records:
        target = store["target"]
        store_is_elf = is_elf(store["actual_path"])
        expected_kind = (
            "nxruntime-generation-linux" if store_is_elf else
            "nxruntime-generation"
        )
        if store["kind"] != expected_kind:
            fail("generation store member {} must use kind {}".format(
                target, expected_kind
            ))
        relative = target[len(prefix):]
        generation_id, separator, internal = relative.partition("/")
        if (not separator or not SHA256_RE.fullmatch(generation_id) or
                not internal.startswith("files/runtime/")):
            if store["kind"] == "nxruntime-generation-linux":
                fail("generation store Linux ELF has no authenticated live "
                     "runtime path: {}".format(target))
            continue
        logical_path = internal[len("files/runtime/"):]
        live_target = config["port_dir"] + "/" + logical_path
        live = by_target.get(live_target)
        member = runtime_by_path.get(logical_path)
        if store["kind"] != "nxruntime-generation-linux":
            continue
        if live is None or member is None:
            fail("generation store Linux ELF has no live counterpart: {}"
                 .format(target))
        if live.get("kind") not in LINUX_ELF_KINDS - {
                "nxruntime-generation-linux"}:
            fail("generation store Linux ELF maps to a non-live ELF kind: {}"
                 .format(live_target))
        if (store["mode"] != member["mode"] or
                store["sha256"] != member["sha256"] or
                live["mode"] != member["mode"] or
                live["sha256"] != member["sha256"]):
            fail("generation live/store path, mode or SHA-256 differs: {}"
                 .format(logical_path))
        for field in metadata_fields:
            if store.get(field) != live.get(field):
                fail("generation store Linux ELF {} differs from live {}: {}"
                     .format(target, field, live_target))


def validate_dependency_closure(elf_results, config):
    declarations = {
        (item["namespace"], item["architecture"], item["soname"]): item
        for item in config["dependencies"]
    }
    packaged = {}
    packaged_folded = {}
    by_path = {item["path"]: item for item in elf_results}
    for item in elf_results:
        # An immutable rollback mirror is audited as Linux but never becomes a
        # second packaged provider for its live ELF's SONAME.
        if item.get("kind") == "nxruntime-generation-linux":
            continue
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
            else:
                # Not bundled: a portmaster/firmware soname the port relies on
                # the host to provide must be one the CFW baseline guarantees.
                # libzip.so.5 exists on muOS/Knulli but not on plain ArkOS, so a
                # port that NEEDs it and leaves it unbundled dies with
                # "libzip.so.5: cannot open shared object" (status 127).
                nbso = key[2]
                if declaration["provider"] == "portmaster" and \
                        nbso not in PORTMASTER_BASELINE_SONAMES:
                    fail("{soname} (NEEDed by {path}) is provider=portmaster but "
                         "is NOT in the guaranteed PortMaster runtime baseline; "
                         "bundle it (provider=package inside the port) or "
                         "static-link it -- otherwise it is status 127 "
                         "(cannot open shared object) on CFWs that lack it. "
                         "Guaranteed portmaster sonames: {baseline}".format(
                             soname=nbso, path=item["path"],
                             baseline=", ".join(sorted(PORTMASTER_BASELINE_SONAMES))))
                if declaration["provider"] == "firmware" and \
                        nbso not in FIRMWARE_BASELINE_SONAMES:
                    fail("{soname} (NEEDed by {path}) is provider=firmware but is "
                         "NOT in the guaranteed device-firmware baseline (not "
                         "universally present across CFWs -- e.g. libzip.so.5 is "
                         "on muOS/Knulli but not plain ArkOS). Bundle it "
                         "(provider=package inside the port) or static-link it, "
                         "otherwise it is status 127 on CFWs that lack it. "
                         "Guaranteed firmware sonames: {baseline}".format(
                             soname=nbso, path=item["path"],
                             baseline=", ".join(sorted(FIRMWARE_BASELINE_SONAMES))))

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


def legacy_execution_roles(config):
    """Project the historical single-ABI contract without changing its input."""
    architecture = config["nxextract"]["ui_architecture"]
    splash = config.get("nxsplash")
    if splash is not None and splash["architecture"] != architecture:
        fail("legacy package mixes NXExtract UI and nxsplash architectures")
    nxport = config["nxport_manifest"]
    return {
        "extractor": {
            "architecture": architecture,
            "executable": PurePosixPath(
                config["nxextract"]["ui_path"]
            ).relative_to(config["port_dir"]).as_posix(),
            "executor": "native",
            "interpreter": LINUX_INTERPRETERS[architecture],
            "closure": "host",
        },
        "splash": ({
            "architecture": architecture,
            "executable": NXSPLASH_RUNTIME_NAME,
            "executor": "native-or-loader",
            "interpreter": LINUX_INTERPRETERS[architecture],
            "closure": "firmware",
        } if splash is not None else None),
        "game": {
            "architecture": nxport["architecture"],
            "executable": nxport["executable"],
            "executor": "native-or-loader",
            "interpreter": LINUX_INTERPRETERS[nxport["architecture"]],
            "closure": "firmware-and-port",
        },
        "helpers": [],
    }


def validate_execution_role_elfs(elf_results, config):
    """Correlate each executable role with its actual ELF ABI and closure."""
    explicit_roles = config.get("execution_roles") is not None
    roles = (
        config["execution_roles"] if explicit_roles
        else legacy_execution_roles(config)
    )
    by_path = {item["path"]: item for item in elf_results}
    declarations = {
        (item["namespace"], item["architecture"], item["soname"]): item
        for item in config["dependencies"]
    }
    selected = []
    if roles.get("extractor") is not None:
        selected.append(("extractor", roles["extractor"], "nxextract-ui-linux"))
    if roles.get("splash") is not None:
        selected.append(("splash", roles["splash"], "nxsplash-linux"))
    selected.append(("game", roles["game"], None))
    selected.extend(
        ("helper {}".format(item["id"]), item, None)
        for item in roles.get("helpers", [])
    )

    for label, role, exact_kind in selected:
        target = PurePosixPath(
            config["port_dir"], role["executable"]
        ).as_posix()
        info = by_path.get(target)
        if info is None:
            fail("execution role {} does not resolve to a packaged Linux ELF: {}".format(
                label, target
            ))
        if exact_kind is not None and info["kind"] != exact_kind:
            fail("execution role {} must resolve to kind {}".format(
                label, exact_kind
            ))
        if (exact_kind is None and
                info["kind"] not in ("project-linux", "third-party-linux")):
            fail("execution role {} resolves to an invalid ELF kind {}".format(
                label, info["kind"]
            ))
        if info["architecture"] != role["architecture"]:
            fail("execution role {} architecture differs from its ELF".format(label))
        if info["class"] != ARCH_CLASSES[role["architecture"]]:
            fail("execution role {} ELF class differs from its architecture".format(
                label
            ))
        if info["machine"] != ARCH_MACHINES[role["architecture"]]:
            fail("execution role {} ELF machine differs from its architecture".format(
                label
            ))
        if (info["interpreter"] != role["interpreter"] and
                (explicit_roles or info["interpreter"] != "none")):
            fail("execution role {} PT_INTERP must be exactly {}; got {}".format(
                label, role["interpreter"], info["interpreter"]
            ))
        if role["closure"] in ("host", "firmware"):
            for soname in info["needed"]:
                declaration = declarations.get(
                    ("linux", role["architecture"], soname)
                )
                if declaration is None:
                    fail("execution role {} has an unresolved closure dependency {}".format(
                        label, soname
                    ))
                if declaration["provider"] == "package":
                    fail("execution role {} with {} closure depends on packaged {}".format(
                        label, role["closure"], soname
                    ))


def _graphics_version_tuple(value):
    """(major, minor) from 'MAJOR.MINOR', or None if malformed."""
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]+\.[0-9]+", value):
        return None
    major, minor = value.split(".")
    return (int(major), int(minor))


def parse_graphics_evidence(line):
    """Parse a structured GRAPHICS-EVIDENCE receipt line (from
    nxgl_graphics_contract_evidence_receipt) into its fields, or None if it is
    not a well-formed evidence line. This is the physical artifact a release
    proof MUST link to -- a hand-written verdict with no matching evidence line
    cannot pass the gate."""
    prefix = "GRAPHICS-EVIDENCE:"
    if not isinstance(line, str) or not line.startswith(prefix):
        return None
    fields = {}
    for token in line[len(prefix):].strip().split():
        if "=" not in token:
            return None
        key, value = token.split("=", 1)
        if key in fields:
            return None
        fields[key] = value
    result = {
        "run_id": fields.get("run_id"),
        "generation": fields.get("generation"),
        "commit": fields.get("commit"),
        "cfw": fields.get("cfw"),
        "build_id": fields.get("build_id"),
        "shader_probe": fields.get("shader_probe"),
        "verdict": fields.get("verdict"),
        "reason": fields.get("reason"),
        # V4-GRAPHICS-04 extension fields (absent on a pre-present receipt).
        "phase": fields.get("phase"),
        "first_present": fields.get("first_present"),
    }
    obtained = fields.get("obtained")
    if isinstance(obtained, str):
        parts = obtained.split("/")
        if len(parts) == 3:
            result["obtained"] = {
                "api": parts[0], "profile": parts[1], "version": parts[2]
            }
    drawable = fields.get("drawable")
    if isinstance(drawable, str) and "x" in drawable:
        w_text, h_text = drawable.split("x", 1)
        if w_text.isdigit() and h_text.isdigit():
            result["drawable_w"] = int(w_text)
            result["drawable_h"] = int(h_text)
    pre_drawable = fields.get("pre_drawable")
    if isinstance(pre_drawable, str) and "x" in pre_drawable:
        w_text, h_text = pre_drawable.split("x", 1)
        if w_text.isdigit() and h_text.isdigit():
            result["pre_drawable_w"] = int(w_text)
            result["pre_drawable_h"] = int(h_text)
    return result


def validate_generation_receipt(records, config):
    """Validate an optional nxgenerator receipt and its mixed-ABI opt-in."""
    config["generation_id"] = None
    target = config["port_dir"] + "/GENERATION.json"
    record = next(
        (item for item in records if item.get("target") == target), None
    )
    if record is None:
        return
    if record.get("kind") != "payload" or record.get("mode") != 0o644:
        fail("GENERATION.json must be a payload with mode 0644")
    receipt = require_object(
        load_json(record["actual_path"], "nxgenerator receipt"),
        "nxgenerator receipt",
    )
    required = {
        "schema", "schema_version", "generator", "project_manifest_sha256",
        "source_pins", "artifacts", "claims",
    }
    optional = {"execution_roles", "generation_id"}
    if set(receipt) - required - optional or not required.issubset(receipt):
        fail("GENERATION.json fields are not canonical")
    if (receipt.get("schema") != "nxgenerator-receipt-v1" or
            receipt.get("schema_version") != 1):
        fail("GENERATION.json schema is unsupported")
    generator = require_object(
        receipt.get("generator"), "GENERATION.json generator"
    )
    if set(generator) != {"name", "version"} or generator != {
            "name": "nxgenerator", "version": NXGENERATOR_REQUIRED_VERSION}:
        fail("GENERATION.json must come from nxgenerator {}".format(
            NXGENERATOR_REQUIRED_VERSION
        ))
    project_manifest_sha256 = parse_sha256(
        receipt.get("project_manifest_sha256"),
        "GENERATION.json project_manifest_sha256",
    )
    project_target = config["port_dir"] + "/nxproject.json"
    project_record = next(
        (item for item in records if item.get("target") == project_target),
        None,
    )
    if (project_record is None or project_record.get("kind") != "payload" or
            project_record.get("mode") != 0o644):
        fail(
            "GENERATION.json requires the generated nxproject.json as a "
            "0644 payload"
        )
    if project_record.get("sha256") != project_manifest_sha256:
        fail(
            "GENERATION.json project_manifest_sha256 differs from the "
            "packaged nxproject.json"
        )
    try:
        project_manifest = json.loads(read_small_text(
            project_record["actual_path"], project_target))
    except ValueError:
        fail("generated nxproject.json is not valid JSON")
    if not isinstance(project_manifest, dict):
        fail("generated nxproject.json must be a JSON object")
    config["nxproject_manifest"] = project_manifest

    raw_artifacts = receipt.get("artifacts")
    if not isinstance(raw_artifacts, list):
        fail("GENERATION.json artifacts must be an array")
    receipt_artifacts = []
    previous_artifact_path = None
    for index, raw_artifact in enumerate(raw_artifacts):
        context = "GENERATION.json artifacts[{}]".format(index)
        artifact = _public_final_exact_keys(
            raw_artifact, ("path", "mode", "sha256"), (), context
        )
        path = safe_relative(artifact.get("path"), context + ".path")
        mode = artifact.get("mode")
        if mode not in ("0644", "0755"):
            fail(context + ".mode must be 0644 or 0755")
        digest = parse_sha256(artifact.get("sha256"), context + ".sha256")
        if previous_artifact_path is not None and path <= previous_artifact_path:
            fail("GENERATION.json artifacts are not strictly ordered by path")
        if path == target:
            fail("GENERATION.json artifacts must not include GENERATION.json")
        receipt_artifacts.append({
            "path": path, "mode": mode, "sha256": digest,
        })
        previous_artifact_path = path

    packaged_artifacts = [
        {
            "path": item["target"],
            "mode": "%04o" % item["mode"],
            "sha256": item["sha256"],
        }
        for item in records
        if item.get("target") != target
    ]
    packaged_artifacts.sort(key=lambda item: item["path"])
    if receipt_artifacts != packaged_artifacts:
        receipt_paths = {item["path"] for item in receipt_artifacts}
        packaged_paths = {item["path"] for item in packaged_artifacts}
        missing = sorted(packaged_paths - receipt_paths)
        extra = sorted(receipt_paths - packaged_paths)
        detail = []
        if missing:
            detail.append("missing=" + ",".join(missing[:3]))
        if extra:
            detail.append("extra=" + ",".join(extra[:3]))
        if not detail:
            detail.append("path/mode/SHA mismatch")
        fail(
            "GENERATION.json artifact closure is stale: " + "; ".join(detail)
        )
    generation_id = receipt.get("generation_id")
    if generation_id is not None and (
        not isinstance(generation_id, str)
        or not re.fullmatch(r"[0-9a-f]{64}|[0-9a-f]{32}", generation_id)
    ):
        fail(
            "GENERATION.json generation_id must be the full 64 lowercase hex "
            "SHA-256 (32 hex accepted only as legacy migration input)"
        )
    config["generation_id"] = generation_id
    # V3-CONTROLLERS-01: a generation produced by nxgenerator 0.2.13+ (any
    # receipt carrying a generation_id) ships the immutable controls default.
    if generation_id is not None:
        defaults_target = config["port_dir"] + "/defaults/NEXTOSCONTROLLERS.gptk"
        defaults_record = next(
            (item for item in records if item.get("target") == defaults_target),
            None,
        )
        if defaults_record is None:
            fail(
                "generation ships no {}; every new port carries the "
                "immutable NEXTOS_CONTROLLERS/1 default".format(defaults_target)
            )
        if (defaults_record.get("kind") != "payload"
                or defaults_record.get("mode") != 0o644
                or not defaults_record.get("sha256")
                or ("expected_sha" in defaults_record
                    and not defaults_record.get("expected_sha"))):
            fail("defaults/NEXTOSCONTROLLERS.gptk must be a pinned 0644 payload")
        config["gptk_defaults_sha256"] = defaults_record.get("sha256")
        defaults_text = read_small_text(
            defaults_record["actual_path"], defaults_target
        )
        # V4-CONTROLLERS-03/C4: /1 and /2 are both legal; V2 is opt-in per
        # port and is held to a stricter contract below.
        gptk_schema = None
        if "format = NEXTOS_CONTROLLERS/1" in defaults_text:
            gptk_schema = 1
        elif "format = NEXTOS_CONTROLLERS/2" in defaults_text:
            gptk_schema = 2
        elif "format = NEXTOS_CONTROLLERS/3" in defaults_text:
            gptk_schema = 3
        elif "format = NEXTOS_CONTROLLERS/4" in defaults_text:
            gptk_schema = 4
        else:
            fail("defaults/NEXTOSCONTROLLERS.gptk lacks the "
                 "NEXTOS_CONTROLLERS/1, /2, /3 or /4 magic")
        _INPUT_RUNTIME_MARKER_EXPECTED[0] = (
            INPUT_RUNTIME_MARKER_V4 if gptk_schema == 4 else INPUT_RUNTIME_MARKER)
        lock_for_marker = config.get("candidate_lock")
        if isinstance(lock_for_marker, dict):
            lock_marker = (lock_for_marker.get("input_proof") or {}).get("runtime", {}).get("marker")
            if lock_marker is not None and lock_marker != _INPUT_RUNTIME_MARKER_EXPECTED[0]:
                fail("candidate lock.input_proof.runtime.marker %s does not match the packaged "
                     "NEXTOS_CONTROLLERS/%d defaults (expected %s)" % (
                         lock_marker, gptk_schema, _INPUT_RUNTIME_MARKER_EXPECTED[0]))
        # nxinput 0.10.0: V3 inherits the whole V2 contract and adds exactly
        # one mandatory lowercase FACE_LAYOUT preamble line. A V3 default
        # without it, with a duplicate, with the wrong case or with a value
        # outside auto|modern|retro fails closed; V1/V2 must not carry the
        # V3 field at all.
        gptk_face_layout = None
        face_lines = [line.strip() for line in defaults_text.splitlines()
                      if line.strip().startswith("FACE_LAYOUT")]
        if gptk_schema == 3:
            section_at = len(defaults_text)
            for section_name in ("[menu]", "[gameplay]", "[cursor]",
                                 "[camera]"):
                found_at = defaults_text.find(section_name)
                if found_at >= 0:
                    section_at = min(section_at, found_at)
            preamble = defaults_text[:section_at]
            preamble_lines = [line.strip() for line in preamble.splitlines()
                              if line.strip().startswith("FACE_LAYOUT")]
            if len(face_lines) != 1 or len(preamble_lines) != 1:
                fail("a NEXTOS_CONTROLLERS/3 default requires exactly one "
                     "FACE_LAYOUT line in the preamble")
            face_match = re.fullmatch(
                r"FACE_LAYOUT[ \t]*=[ \t]*(auto|modern|retro)",
                face_lines[0])
            if face_match is None:
                fail("FACE_LAYOUT must be exactly auto, modern or retro "
                     "(lowercase)")
            gptk_face_layout = face_match.group(1)
        elif face_lines:
            fail("FACE_LAYOUT is a NEXTOS_CONTROLLERS/3 field; schema {} "
                 "must not carry it".format(gptk_schema))
        config["gptk_face_layout"] = gptk_face_layout
        # Fuller gptk structure (blocker 4): a default that is only the magic
        # binds no control. Require at least one known section and one
        # `CONTROL = action` line so a truncated default fails closed.
        if not any(section in defaults_text
                   for section in ("[menu]", "[gameplay]",
                                   "[cursor]", "[camera]", "[base]")):
            fail("defaults/NEXTOSCONTROLLERS.gptk has no "
                 "[menu]/[gameplay]/[cursor]/[camera] section")
        if not re.search(r"(?m)^[A-Za-z0-9_]+[ \t]*=[ \t]*[A-Za-z0-9_.]+[ \t]*$",
                         defaults_text):
            fail("defaults/NEXTOSCONTROLLERS.gptk declares no "
                 "CONTROL = action binding")
        # The closure against the adapter contract runs once that contract
        # has been loaded and shape-checked, a few blocks below.
        gptk_defaults_text = defaults_text

        # defaults/NEXTOSSETTINGS.txt (blocker 4): the generation must also ship
        # the immutable settings default, satisfying the SAME contract the
        # runtime nxcompat_settings.c enforces (allowlist language/quality,
        # value [A-Za-z0-9._-]{1,32}, quality in auto|low|medium|high, no
        # whitespace stripping). Absent or malformed fails closed.
        settings_target = config["port_dir"] + "/defaults/NEXTOSSETTINGS.txt"
        settings_record = next(
            (item for item in records if item.get("target") == settings_target),
            None,
        )
        if settings_record is None:
            fail("generation ships no {}; every new port carries the "
                 "immutable NEXTOS_SETTINGS/1 default".format(settings_target))
        if (settings_record.get("kind") != "payload"
                or settings_record.get("mode") != 0o644
                or not settings_record.get("sha256")
                or ("expected_sha" in settings_record
                    and not settings_record.get("expected_sha"))):
            fail("defaults/NEXTOSSETTINGS.txt must be a pinned 0644 payload")
        settings_text = read_small_text(
            settings_record["actual_path"], settings_target
        )
        settings_magic_seen = False
        settings_schema = 0
        settings_keys_seen = set()
        settings_video = {}
        for settings_raw in settings_text.split("\n"):
            settings_line = (settings_raw[:-1]
                             if settings_raw.endswith("\r") else settings_raw)
            if all(ch in " \t" for ch in settings_line):
                continue
            if not settings_magic_seen:
                if settings_line == "# NEXTOS_SETTINGS/1":
                    settings_schema = 1
                elif settings_line == "# NEXTOS_SETTINGS/2":
                    settings_schema = 2
                else:
                    fail("defaults/NEXTOSSETTINGS.txt lacks the "
                         "# NEXTOS_SETTINGS/1 or /2 first line")
                settings_magic_seen = True
                continue
            if settings_line.startswith("#"):
                continue
            if "=" not in settings_line or settings_line[0] == "=":
                fail("defaults/NEXTOSSETTINGS.txt: not a key=value line")
            settings_key, settings_value = settings_line.split("=", 1)
            if len(settings_key) > 32 or not re.fullmatch(
                    r"[A-Za-z0-9._-]+", settings_key):
                fail("defaults/NEXTOSSETTINGS.txt: malformed key")
            if not (1 <= len(settings_value) <= 32) or not re.fullmatch(
                    r"[A-Za-z0-9._-]+", settings_value):
                fail("defaults/NEXTOSSETTINGS.txt: malformed value")
            if settings_key in SETTINGS_VIDEO_KEYS:
                if settings_schema != 2:
                    fail("defaults/NEXTOSSETTINGS.txt: video keys need "
                         "# NEXTOS_SETTINGS/2")
                if not settings_video_value_ok(settings_key, settings_value):
                    fail("defaults/NEXTOSSETTINGS.txt: %s=%s outside the "
                         "schema /2 enum" % (settings_key, settings_value))
                settings_video[settings_key] = settings_value
            elif settings_key not in ("language", "quality"):
                fail("defaults/NEXTOSSETTINGS.txt: unknown key %r "
                     "(allowlist language, quality%s)" % (
                         settings_key,
                         ", video.*" if settings_schema == 2 else ""))
            if settings_key in settings_keys_seen:
                fail("defaults/NEXTOSSETTINGS.txt: duplicate key %r"
                     % settings_key)
            settings_keys_seen.add(settings_key)
            if settings_key == "quality" and settings_value not in (
                    "auto", "low", "medium", "high"):
                fail("defaults/NEXTOSSETTINGS.txt: quality not in "
                     "auto|low|medium|high")
        if not settings_magic_seen:
            fail("defaults/NEXTOSSETTINGS.txt lacks the "
                 "# NEXTOS_SETTINGS/1 or /2 first line")
        # V5 7A.3: a project that declares `video` must seed /2 with EVERY video
        # key explicit and equal to the declared defaults; a project without it
        # must not seed video keys (nothing hidden, nothing improvised).
        project_video = (config.get("nxproject_manifest") or {}).get("video")
        if isinstance(project_video, dict):
            if settings_schema != 2:
                fail("nxproject declares video but defaults/NEXTOSSETTINGS.txt "
                     "is not NEXTOS_SETTINGS/2")
            missing = sorted(set(SETTINGS_VIDEO_KEYS) - set(settings_video))
            if missing:
                fail("defaults/NEXTOSSETTINGS.txt omits video key(s) %s (every "
                     "key is explicit under /2)" % ", ".join(missing))
            expected = {
                "video.authority": project_video.get("authority"),
                "video.aspect": project_video.get("aspect"),
                "video.output_size": project_video.get("output_size", "display"),
                "video.filter": project_video.get("filter", "engine"),
                "video.invalid_policy": project_video.get("invalid_policy"),
            }
            for key, value in expected.items():
                if value is not None and settings_video.get(key) != value:
                    fail("defaults/NEXTOSSETTINGS.txt %s=%s differs from the "
                         "declared nxproject.video default %s" % (
                             key, settings_video.get(key), value))
            policies = project_video.get("aspect_policies")
            if isinstance(policies, list) and \
                    settings_video.get("video.aspect") not in policies:
                fail("defaults/NEXTOSSETTINGS.txt video.aspect is not among the "
                     "aspect policies the port implements")
        elif settings_video:
            fail("defaults/NEXTOSSETTINGS.txt seeds video keys but nxproject "
                 "declares no video block")

        # adapter/adapter-contract.json (blockers 4/5): REQUIRED and well-formed
        # -- absent or malformed now fails closed (was silently skipped). It
        # declares language_access (how the player reaches languages), the input
        # actions and the language sinks.
        adapter_target = config["port_dir"] + "/adapter/adapter-contract.json"
        adapter_record = next(
            (item for item in records if item.get("target") == adapter_target),
            None,
        )
        if adapter_record is None:
            fail("generation ships no {}; a V3 generation declares its adapter "
                 "contract".format(adapter_target))
        try:
            adapter_contract = json.loads(read_small_text(
                adapter_record["actual_path"], adapter_target
            ))
        except ValueError:
            fail("adapter-contract.json is not valid JSON")
        if not isinstance(adapter_contract, dict):
            fail("adapter-contract.json must be a JSON object")
        config["adapter_contract"] = adapter_contract
        config["adapter_contract_sha256"] = adapter_record.get("sha256")
        access = adapter_contract.get("language_access")
        if access is None:
            fail(
                "adapter-contract language_access must not be null; "
                "every new port declares native-menu, first-run-"
                "native, adapter, single-language or none"
            )
        if not isinstance(access, dict) or access.get("mode") not in (
            "native-menu", "first-run-native", "adapter",
            "single-language", "none",
        ):
            fail("adapter-contract language_access.mode is invalid")
        supported = access.get("supported") or []
        if not isinstance(supported, list):
            fail("adapter-contract language_access.supported must be a list")
        if not isinstance(access.get("sinks"), list):
            fail("adapter-contract language_access.sinks must be a list")
        fallback = access.get("fallback")
        if access.get("mode") != "none" and fallback not in supported:
            fail("adapter-contract language_access.fallback must be "
                 "one of supported")
        adapter_input = adapter_contract.get("input")
        if not isinstance(adapter_input, dict) or not isinstance(
                adapter_input.get("actions"), list):
            fail("adapter-contract input.actions must be a list")
        # A scaffold (release_ready False) may leave actions empty; a contract
        # that claims release_ready must declare the actions it binds.
        if adapter_contract.get("release_ready") is True and not \
                adapter_input.get("actions"):
            fail("a release-ready adapter-contract must declare input.actions")
        # V4-CONTROLLERS-03/C4: the shipped controls default must close
        # against this very contract -- every action it binds has a real sink,
        # and every control appears exactly once per section.
        # The V3 opt-ins downstream (face_layout_variants closure, the
        # NXC6-DOMAIN/live-database ELF identity) key off the ACCEPTED
        # default's schema; publish it on the config or they read None and
        # either fail a legitimate V3 port or silently skip.
        config["gptk_schema"] = gptk_schema
        if gptk_face_layout is not None:
            config["gptk_face_layout"] = gptk_face_layout
        _validate_gptk_closure(gptk_defaults_text, gptk_schema, adapter_input)
        # V3-GRAPHICS-02 release gate: if the adapter declares a graphics
        # CONTEXT contract, it must be well-formed, and a release-ready port
        # with physical support proven must carry a physical graphics receipt
        # per device whose verdict is OK and whose obtained context does not
        # betray the contract (never a desktop-GL context for a GLES contract,
        # a usable drawable -- never 1x1 -- and a passing shader probe of the
        # declared dialect). The contract is the port's OWN declaration; a
        # game-specific workaround never becomes a global default here.
        # --- V4 declarative opt-ins ------------------------------------
        # nxgenerator 0.3.0 always writes these three blocks, already
        # normalized. Here they are only checked for shape and for the one
        # package-level consequence nxrelease can actually see: an EGL binding
        # exists precisely because the universal executable must NOT link EGL.
        _validate_v4_optins(adapter_contract, config, records)

        graphics = adapter_contract.get("graphics")
        if graphics is not None:
            if not isinstance(graphics, dict):
                fail("adapter-contract graphics must be an object")
            if graphics.get("api") not in ("gles", "gl"):
                fail("adapter-contract graphics.api must be gles or gl")
            if graphics.get("profile") not in ("es", "core", "compat"):
                fail("adapter-contract graphics.profile is invalid")
            if graphics.get("version_policy") not in (
                    "exact", "minimum", "range"):
                fail("adapter-contract graphics.version_policy is invalid")
            if graphics.get("shader_dialect") not in (
                    "essl100", "essl300", "essl310", "glsl-any"):
                fail("adapter-contract graphics.shader_dialect is invalid")
            if not re.fullmatch(r"[0-9]+\.[0-9]+",
                                str(graphics.get("version", ""))):
                fail("adapter-contract graphics.version must be MAJOR.MINOR")
            # V4-GRAPHICS-04: the declarative opt-in. Absence preserves the
            # previous boundary; the only valid new value is
            # post-first-present, and it is what arms the stricter proof
            # requirements below.
            evidence_boundary = graphics.get("evidence_boundary")
            if evidence_boundary is not None and \
                    evidence_boundary != "post-first-present":
                fail("adapter-contract graphics.evidence_boundary must be "
                     "post-first-present when present")
            claims = receipt.get("claims")
            release_ready = (isinstance(claims, dict) and
                             claims.get("release_ready") is True)
            physical_proven = (isinstance(claims, dict) and
                               claims.get("physical_support_proven") is True)
            # version policy coherence of the DECLARATION itself.
            contract_version = _graphics_version_tuple(graphics.get("version"))
            policy = graphics.get("version_policy")
            contract_vmax = None
            if policy == "range":
                contract_vmax = _graphics_version_tuple(
                    graphics.get("version_max"))
                if contract_vmax is None:
                    fail("adapter-contract graphics.version_policy range "
                         "requires a MAJOR.MINOR version_max")
            elif graphics.get("version_max") is not None:
                fail("adapter-contract graphics.version_max is only valid for a "
                     "range policy")
            required_devices = graphics.get("required_devices")
            if required_devices is not None and (
                    not isinstance(required_devices, list)
                    or not required_devices
                    or any(not isinstance(item, str) or not item
                           for item in required_devices)
                    or len(set(required_devices)) != len(required_devices)):
                fail("adapter-contract graphics.required_devices must be a "
                     "non-empty list of unique device ids")
            proofs = adapter_contract.get("graphics_proofs")
            if proofs is not None and not isinstance(proofs, list):
                fail("adapter-contract graphics_proofs must be a list")
            if release_ready and physical_proven:
                if not isinstance(proofs, list) or not proofs:
                    fail("a release-ready graphics port with physical support "
                         "proven must carry graphics_proofs (per device)")
                seen_devices = set()
                boundary_seen_runs = set()
                boundary_commits = set()
                for proof in proofs:
                    if not isinstance(proof, dict):
                        fail("graphics_proofs entry must be an object")
                    device = proof.get("device")
                    if not device or not isinstance(device, str):
                        fail("a graphics proof lacks a device identity")
                    if device in seen_devices:
                        fail("graphics_proofs has a duplicate proof for %r"
                             % device)
                    seen_devices.add(device)
                    # The proof MUST link to a physical, structured
                    # GRAPHICS-EVIDENCE receipt line -- a hand-written verdict
                    # decoupled from a real run cannot pass.
                    evidence_line = proof.get("evidence")
                    evidence = parse_graphics_evidence(evidence_line)
                    if evidence is None:
                        fail("graphics proof on %r lacks a well-formed "
                             "GRAPHICS-EVIDENCE receipt line" % device)
                    # Tie the receipt to THIS generation and a real measured DSO.
                    if (generation_id is not None and
                            evidence.get("generation") != generation_id):
                        fail("graphics proof on %r: evidence generation does "
                             "not match this generation_id" % device)
                    if not evidence.get("run_id") or \
                            evidence["run_id"] == "-":
                        fail("graphics proof on %r: evidence carries no run_id"
                             % device)
                    if not evidence.get("build_id") or \
                            evidence["build_id"] == "-":
                        fail("graphics proof on %r: evidence carries no provider "
                             "build-id (no DSO was measured)" % device)
                    # The summary fields cannot disagree with the evidence line.
                    obtained = evidence.get("obtained") or {}
                    draw_w = evidence.get("drawable_w")
                    draw_h = evidence.get("drawable_h")
                    if evidence.get("verdict") != proof.get("verdict") or \
                            evidence.get("reason") != proof.get("reason") or \
                            evidence.get("shader_probe") != \
                            proof.get("shader_probe") or \
                            obtained.get("api") != proof.get("obtained_api") or \
                            obtained.get("profile") != \
                            proof.get("obtained_profile") or \
                            obtained.get("version") != \
                            proof.get("obtained_version") or \
                            draw_w != proof.get("drawable_w") or \
                            draw_h != proof.get("drawable_h"):
                        fail("graphics proof on %r: summary disagrees with its "
                             "GRAPHICS-EVIDENCE line" % device)
                    # Now judge the evidence-backed verdict.
                    if proof.get("verdict") != "OK":
                        fail("graphics proof on %r is not OK (reason=%s)"
                             % (device, proof.get("reason")))
                    if evidence.get("reason") != "ok":
                        fail("graphics proof on %r: verdict OK but reason=%r"
                             % (device, evidence.get("reason")))
                    if graphics.get("api") == "gles" and \
                            obtained.get("api") != "gles":
                        fail("graphics proof on %r received a non-GLES context "
                             "for a GLES contract" % device)
                    if graphics.get("profile") == "es" and \
                            obtained.get("profile") != "es":
                        fail("graphics proof on %r: obtained profile is not es "
                             "for an ES contract" % device)
                    obtained_version = _graphics_version_tuple(
                        obtained.get("version"))
                    if obtained_version is None:
                        fail("graphics proof on %r has a malformed obtained "
                             "version" % device)
                    if policy == "exact" and obtained_version != \
                            contract_version:
                        fail("graphics proof on %r: obtained version %s is not "
                             "exactly %s" % (device, obtained.get("version"),
                                             graphics.get("version")))
                    if policy == "minimum" and obtained_version < \
                            contract_version:
                        fail("graphics proof on %r: obtained version below the "
                             "declared minimum" % device)
                    if policy == "range" and not (
                            contract_version <= obtained_version <=
                            contract_vmax):
                        fail("graphics proof on %r: obtained version out of the "
                             "declared range" % device)
                    if not (isinstance(draw_w, int) and isinstance(draw_h, int)
                            and draw_w > 1 and draw_h > 1):
                        fail("graphics proof on %r has an unusable 1x1 drawable"
                             % device)
                    if proof.get("shader_probe") != "pass":
                        fail("graphics proof on %r: shader probe of the "
                             "declared dialect failed" % device)
                    # V4-GRAPHICS-04: under the post-first-present opt-in a
                    # promotable receipt must have been emitted AFTER the
                    # guest's first real present. A pre-present receipt (no
                    # phase), a missing first present, a missing pre-present
                    # diagnosis, a duplicated receipt or a divergent
                    # commit identity never promote.
                    if evidence_boundary == "post-first-present":
                        if evidence.get("phase") != "post-first-present":
                            fail("graphics proof on %r: the declared "
                                 "evidence boundary requires a "
                                 "phase=post-first-present receipt; a "
                                 "pre-present receipt never promotes"
                                 % device)
                        if evidence.get("first_present") != "1":
                            fail("graphics proof on %r: the receipt does not "
                                 "attest first_present=1" % device)
                        if not isinstance(
                                evidence.get("pre_drawable_w"), int) or \
                                not isinstance(
                                    evidence.get("pre_drawable_h"), int):
                            fail("graphics proof on %r: the receipt lacks the "
                                 "pre-present drawable diagnosis" % device)
                        if not evidence.get("commit") or \
                                evidence["commit"] == "-":
                            fail("graphics proof on %r: evidence carries no "
                                 "framework commit identity" % device)
                        run_key = evidence.get("run_id")
                        if run_key in boundary_seen_runs:
                            fail("graphics proof on %r reuses the receipt of "
                                 "run %r: a final receipt is one-shot"
                                 % (device, run_key))
                        boundary_seen_runs.add(run_key)
                        boundary_commits.add(evidence["commit"])
                        if len(boundary_commits) > 1:
                            fail("graphics proofs disagree on the framework "
                                 "commit identity: %s"
                                 % ", ".join(sorted(boundary_commits)))
                # Coverage: every declared required device must be proven.
                if required_devices is not None:
                    missing = [item for item in required_devices
                               if item not in seen_devices]
                    if missing:
                        fail("graphics_proofs miss required device(s): %s"
                             % ", ".join(missing))
    claims = require_object(receipt.get("claims"), "GENERATION.json claims")
    if set(claims) != {
            "deterministic_scaffold", "release_ready",
            "physical_support_proven", "adapter_lifecycle_implemented"} or any(
                not isinstance(value, bool) for value in claims.values()):
        fail("GENERATION.json claims are not canonical")
    source_pins = require_object(
        receipt.get("source_pins"), "GENERATION.json source_pins"
    )
    if set(source_pins) != {"nxbootstrap", "nxsplash", "nxextract", "portmaster"}:
        fail("GENERATION.json source_pins are not canonical")
    for name, expected in (
            ("nxbootstrap", NXBOOTSTRAP_REQUIRED_VERSION),
            ("nxsplash", NXSPLASH_REQUIRED_VERSION),
            ("nxextract", NXEXTRACT_REQUIRED_VERSION)):
        pin = source_pins.get(name)
        if pin is None and name == "nxextract":
            continue
        pin = require_object(pin, "GENERATION.json source_pins." + name)
        if pin.get("version") != expected:
            fail("GENERATION.json {} pin must be {}".format(name, expected))
    roles = config.get("execution_roles")
    receipt_roles = receipt.get("execution_roles")
    if roles is None:
        if "execution_roles" in receipt:
            fail("legacy GENERATION.json must not claim execution_roles")
    elif receipt_roles != roles:
        fail("GENERATION.json execution_roles differ from nxport.json")


def _sdl_family_from_name(value):
    """Return 1/2/3 for a private SDL-looking basename or SONAME."""
    if not isinstance(value, str):
        return None
    name = PurePosixPath(value).name.casefold()
    if re.match(r"^libsdl3(?:[._+-]|$)", name):
        return 3
    if re.match(r"^libsdl2(?:[._+-]|$)", name):
        return 2
    # SDL 1.2 commonly used libSDL-1.2.so, libSDL.so and unnumbered add-ons
    # such as libSDL_image.so.  Anything in the namespace which is not SDL3
    # is therefore an SDL1/SDL2 private provider and is forbidden.
    if re.match(r"^libsdl(?:[._+-]|$)", name):
        return 1
    return None


_SDL_ADDON_EXPORT_PAIRS = (
    {"Mix_OpenAudio", "Mix_Quit"},
    {"IMG_Init", "IMG_Quit"},
    {"TTF_Init", "TTF_Quit"},
    {"SDLNet_Init", "SDLNet_Quit"},
    {"rotozoomSurface", "pixelColor"},       # SDL_gfx / SDL2_gfx
    {"GPU_Init", "GPU_Quit"},                # SDL_gpu
    {"Sound_Init", "Sound_Quit"},            # SDL_sound
    {"RTF_Init", "RTF_Quit"},                # SDL_rtf
    {"FC_CreateFont", "FC_FreeFont"},         # SDL_FontCache
)


def _sdl_family_from_symbol_set(exports):
    """Classify a provider from defined symbols; 0 means SDL add-on/unknown."""
    common = {"SDL_Init", "SDL_Quit"}
    if not common.issubset(exports):
        return 0 if any(
            pair.issubset(exports) for pair in _SDL_ADDON_EXPORT_PAIRS
        ) else None
    if {"SDL_SetVideoMode", "SDL_GetVideoSurface"} & exports:
        return 1
    if {"SDL_GameControllerOpen", "SDL_RWFromFile",
            "SDL_RenderSetLogicalSize"} & exports:
        return 2
    if {"SDL_OpenGamepad", "SDL_IOFromFile",
            "SDL_CreateWindowWithProperties"} & exports:
        return 3
    # A provider exporting the common SDL entry points but no discriminator is
    # ambiguous, never a normal SDL consumer.  Fail closed instead of allowing
    # a renamed/stripped SDL1/2 build.
    return 0


def _defined_symbols_from_full_table(record):
    """Read defined regular/hidden/static symbols when an object exposes them."""
    path = record["actual_path"]
    try:
        with open(str(path), "rb") as stream:
            magic = stream.read(8)
    except OSError as error:
        fail("cannot inspect {} for SDL provider symbols: {}".format(
            record.get("target"), error))
    if not (magic.startswith(b"\x7fELF") or magic == b"!<arch>\n"):
        return set()
    symbol_table = run_readelf(path, ("--syms", "--wide"))
    defined = set()
    for line in symbol_table.splitlines():
        fields = line.split()
        if len(fields) < 8 or fields[6] in ("UND", "Name"):
            continue
        name = fields[7].split("@", 1)[0]
        if name and name != "Name":
            defined.add(name)
    return defined


def _sdl_family_from_exports(record):
    logical_path = record["target"]
    symbols = _DYNAMIC_SYMBOLS.get(logical_path)
    exports = set(symbols[1]) if symbols is not None else set()
    exports.update(_defined_symbols_from_full_table(record))
    return _sdl_family_from_symbol_set(exports)


_SDL_PROVENANCE_RE = re.compile(
    r"(?i)(?:^|[^a-z0-9])(?:libsdl(?:1|2|3)?|sdl(?:1(?:\.2)?|2|3)?"
    r"(?:[-_+.](?:gfx|gpu|sound|image|mixer|ttf|net|rtf|fontcache))?)"
    r"(?:$|[^a-z0-9])"
)


def _sdl_family_from_provenance(record):
    """Treat a library's own SDL provenance as provider evidence."""
    provenance = record.get("provenance")
    basename = PurePosixPath(record.get("target", "")).name.casefold()
    library_container = (
        record.get("soname") is not None or basename.endswith(".a") or
        ".so" in basename
    )
    if not library_container or not isinstance(provenance, str) or not \
            _SDL_PROVENANCE_RE.search(provenance):
        return None
    lowered = provenance.casefold()
    if re.search(r"(?:libsdl|\bsdl)\s*[-_+.]?\s*3(?:\D|$)", lowered):
        return 3
    if re.search(r"(?:libsdl|\bsdl)\s*[-_+.]?\s*2(?:\D|$)", lowered):
        return 2
    return 1


def _record_is_library_container(record):
    basename = PurePosixPath(record.get("target", "")).name.casefold()
    return bool(
        record.get("soname") is not None or basename.endswith(".a") or
        re.search(r"\.so(?:[._+-]|$)", basename)
    )


def _validate_private_sdl3_elf(record, exception):
    """Bind the narrow exception to a real Linux DSO and its declared ABI."""
    logical_path = record["target"]
    path = record["actual_path"]
    if (record.get("kind") != "third-party-linux" or
            record.get("mode") != 0o644 or not is_elf(path)):
        fail("private SDL3 {} must be a real third-party-linux ELF DSO at mode 0644".format(
            logical_path))
    if (record.get("architecture") != exception["architecture"] or
            record.get("soname") != exception["soname"]):
        fail("private SDL3 {} ABI/SONAME differs from its exception".format(
            logical_path))
    header = run_readelf(path, ("-hW",))
    elf_class = re.search(r"^\s*Class:\s*(\S+)", header, re.MULTILINE)
    elf_type = re.search(r"^\s*Type:\s*(\S+)", header, re.MULTILINE)
    machine = re.search(r"^\s*Machine:\s*(.+?)\s*$", header, re.MULTILINE)
    if (not elf_class or not elf_type or not machine or
            elf_type.group(1) != "DYN" or
            elf_class.group(1) != ARCH_CLASSES[exception["architecture"]] or
            machine.group(1) != ARCH_MACHINES[exception["architecture"]]):
        fail("private SDL3 {} is not the declared Linux ET_DYN ABI".format(
            logical_path))
    dynamic = run_readelf(path, ("-dW",))
    sonames = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", dynamic)
    if sonames != [exception["soname"]]:
        fail("private SDL3 {} DT_SONAME differs from its exception".format(
            logical_path))
    symbols = _defined_symbols_from_full_table(record)
    if (_sdl_family_from_symbol_set(symbols) != 3 or
            not {"SDL_Init", "SDL_Quit", "SDL_OpenGamepad"}.issubset(symbols)):
        fail("private SDL3 {} lacks the auditable SDL3 core provider symbols".format(
            logical_path))
    provenance = record.get("provenance")
    if (not isinstance(provenance, str) or
            exception["version"] not in provenance or
            exception["source_url"] not in provenance):
        fail("private SDL3 {} provenance must bind the declared version and source URL".format(
            logical_path))


_LOCAL_SDL_LITERAL_RE = re.compile(
    rb"(?i)(?:\.\.?/|(?:lib|libs|runtime|sdl)/|/(?:roms|storage|mnt)/)"
    rb"[^\x00\r\n\"']{0,256}libsdl[^/\x00\r\n\"']*\.so"
)


def _record_has_local_sdl_dlopen(record):
    """Find a local SDL literal used by dlopen/SDL_LoadObject, streaming."""
    has_loader = False
    has_local_sdl = False
    overlap = b""
    try:
        with open(str(record["actual_path"]), "rb") as stream:
            while True:
                chunk = stream.read(256 * 1024)
                if not chunk:
                    break
                window = overlap + chunk
                lowered = window.lower()
                if b"dlopen" in lowered or b"sdl_loadobject" in lowered:
                    has_loader = True
                if _LOCAL_SDL_LITERAL_RE.search(window):
                    has_local_sdl = True
                overlap = window[-1024:]
    except OSError as error:
        fail("cannot scan {} for SDL provider redirection: {}".format(
            record.get("target"), error))
    return has_loader and has_local_sdl


def _validate_shell_sdl_resolution(record):
    if not record_is_shell(record):
        return
    logical_path = record["target"]
    text = read_small_text(record["actual_path"], logical_path)
    tokens = shell_tokens(text, logical_path)
    for token in tokens:
        assignment = token.split("=", 1)
        if len(assignment) != 2:
            continue
        name, value = assignment[0].upper(), assignment[1]
        if name == "SDL_DYNAMIC_API" and value:
            # nxbootstrap 0.7.5+ (sdl_provider=system) FORWARDS the value the
            # firmware/adapter already owns through the provider boundary
            # variables; that is restoration, not redirection.  Any other
            # non-empty assignment remains a private-provider redirect.
            if not value.startswith("$NXBOOTSTRAP_SYSTEM_SDL_"):
                fail("{} redirects SDL_DYNAMIC_API; public ports must use the system SDL provider".format(
                    logical_path))
        if name == "LD_PRELOAD" and "libsdl" in value.casefold():
            fail("{} preloads a private SDL provider".format(logical_path))
        if (name == "LD_LIBRARY_PATH" and
                re.search(r"(?:^|[/.:_$-])sdl(?:[/.:_$-]|$)", value,
                          re.IGNORECASE)):
            fail("{} adds an SDL-specific local LD_LIBRARY_PATH".format(
                logical_path))


def validate_private_sdl_policy(records, config):
    """Enforce system SDL1/2 and the one content-addressed SDL3 exception."""
    adapter_contract = config.get("adapter_contract") or {}
    sdl3_contract = adapter_contract.get("input_sdl3_portmaster")
    sdl3_enabled = bool(
        isinstance(sdl3_contract, dict) and sdl3_contract.get("enabled") is True
    )
    sdl3_digest = (
        sdl3_contract.get("private_sdl3_sha256")
        if sdl3_enabled else None
    )
    sdl_provider = config.get("sdl_provider")
    if sdl_provider is None:
        nxport = config.get("nxport_manifest") or {}
        sdl_provider = nxport.get("sdl_provider")
    if sdl_provider is not None and sdl_provider != "system":
        fail("nxport sdl_provider must be system when declared")

    private_sdl3 = []
    for record in records:
        logical_path = record["target"]
        # Basename is an independent signal.  Do not require a conventional
        # .so/.a suffix: dlopen accepts extensionless files and a renamed
        # private provider must not escape merely by dropping its suffix.
        path_family = _sdl_family_from_name(logical_path)
        soname_family = _sdl_family_from_name(record.get("soname"))
        export_family = _sdl_family_from_exports(record)
        provenance_family = _sdl_family_from_provenance(record)
        families = {item for item in (
            path_family, soname_family, export_family, provenance_family)
                    if item is not None}
        if families & {0, 1, 2}:
            fail("private SDL1/SDL2 provider is forbidden: {}".format(
                logical_path))
        if 3 in families:
            private_sdl3.append(record)
        _validate_shell_sdl_resolution(record)
        if _record_has_local_sdl_dlopen(record):
            fail("{} loads a local SDL provider with dlopen/SDL_LoadObject".format(
                logical_path))

    package_sdl3_dependencies = []
    for dependency in config.get("dependencies", ()):
        family = _sdl_family_from_name(dependency.get("soname"))
        if dependency.get("provider") != "package" or family is None:
            continue
        if family != 3:
            fail("private SDL1/SDL2 dependency provider is forbidden: {}".format(
                dependency.get("soname")))
        package_sdl3_dependencies.append(dependency)

    if not private_sdl3:
        if sdl3_enabled:
            fail("input_sdl3_portmaster is enabled but no private SDL3 exists in the package")
        if config.get("sdl3_exception") is not None:
            fail("sdl3_exception is declared but no private SDL3 exists in the package")
        if package_sdl3_dependencies:
            fail("a package SDL3 dependency exists without the declared private DSO")
        return
    if sdl_provider == "system":
        fail("sdl_provider=system contradicts a packaged private SDL3")
    if not sdl3_enabled or not isinstance(sdl3_digest, str) or not \
            SHA256_RE.fullmatch(sdl3_digest):
        fail("private SDL3 requires input_sdl3_portmaster.enabled and an exact SHA-256")
    if len(private_sdl3) != 1:
        fail("the SDL3 exception permits exactly one private provider DSO")
    exception = config.get("sdl3_exception")
    if exception is None:
        fail("private SDL3 requires a canonical sdl3_exception declaration")
    exception = normalize_sdl3_exception(
        exception, "sdl3_exception", config["port_dir"]
    )
    record = private_sdl3[0]
    if record["target"] != exception["path"]:
        fail("private SDL3 path differs from sdl3_exception.path")
    actual = sha256_file(record["actual_path"])
    if (actual != sdl3_digest or record.get("sha256") != sdl3_digest or
            exception["sha256"] != sdl3_digest):
        fail("private SDL3 {} differs from the two exact SHA-256 authorities".format(
            record["target"]))
    _validate_private_sdl3_elf(record, exception)

    matching_dependencies = [
        item for item in package_sdl3_dependencies
        if (item.get("path") == exception["path"] and
            item.get("soname") == exception["soname"] and
            item.get("architecture") == exception["architecture"] and
            item.get("namespace") == "linux")
    ]
    if len(package_sdl3_dependencies) != 1 or len(matching_dependencies) != 1:
        fail("private SDL3 requires one exact Linux package dependency declaration")
    license_records = [
        item for item in records
        if item.get("target") == exception["license_file"]
    ]
    if (len(license_records) != 1 or
            license_records[0].get("kind") != "license-notice" or
            license_records[0].get("mode") != 0o644):
        fail("private SDL3 license_file must be one packaged license-notice at mode 0644")
    try:
        license_size = license_records[0]["actual_path"].stat().st_size
    except OSError as error:
        fail("private SDL3 license_file is unreadable: {}".format(error))
    if license_size < 1 or license_size > 1024 * 1024:
        fail("private SDL3 license_file must be non-empty and at most 1 MiB")


def validate_provider_policy(records, config):
    """Apply 0.3.8 policy at every current source/stage/ZIP boundary."""
    validate_private_sdl_policy(records, config)


def _validate_input_proof_contract(records, config, executable_record, proof):
    """Close live GPTK from declared event through a real ACK-capable sink."""
    project = config.get("nxproject_manifest") or {}
    controls = project.get("controls")
    declared = isinstance(controls, dict) and \
        controls.get("runtime_mapping") == "nxinput-gptk"
    if not declared:
        if proof is not None:
            fail("candidate input proof claims an undeclared GPTK runtime")
        return
    if proof is None:
        fail("controls.runtime_mapping=nxinput-gptk needs external event-to-sink proof")
    # nxinput 0.10.2: a port that declares controls.proof is proven on the
    # device by the framework; its candidate lock must carry that class.
    if isinstance(controls.get("proof"), dict) and \
            proof.get("evidence_class") not in INPUT_PROOF_ACCEPTED_FOR_CONTROLS_PROOF:
        fail("a port with controls.proof requires %s or %s evidence in its candidate lock"
             % (INPUT_PROOF_EVIDENCE_CLASS_ON_DEVICE, INPUT_PROOF_EVIDENCE_CLASS_HUMAN))

    adapter = config.get("adapter_contract")
    adapter_input = adapter.get("input") if isinstance(adapter, dict) else None
    actions = controls.get("actions")
    contexts = controls.get("contexts")
    if (not isinstance(actions, list) or not actions or
            not isinstance(contexts, dict) or not contexts or
            not isinstance(adapter_input, dict)):
        fail("live GPTK project/adapter controls are malformed")
    if (adapter.get("status") != "implemented_release" or
            adapter.get("release_ready") is not True or
            adapter_input.get("actions") != actions or
            adapter_input.get("contexts") != contexts or
            adapter_input.get("runtime_mapping") != "nxinput-gptk" or
            adapter_input.get("runtime_contract") != INPUT_RUNTIME_CONTRACT):
        fail("live GPTK adapter is not release-ready on the fail-safe runtime contract")

    if proof.get("port_id") != config.get("package_id") or \
            proof.get("generation") != config.get("generation_id"):
        fail("candidate input proof port/generation is absent or stale")
    if proof.get("mapping_sha256") != config.get("gptk_defaults_sha256"):
        fail("candidate input proof mapping differs from packaged defaults")
    if proof.get("adapter_contract_sha256") != \
            config.get("adapter_contract_sha256"):
        fail("candidate input proof adapter contract differs from the package")

    path = executable_record["actual_path"]
    try:
        executable_bytes = path.read_bytes()
    except OSError as error:
        fail("cannot inspect live GPTK executable: {}".format(error))
    if (input_runtime_marker().encode("ascii") not in executable_bytes or
            INPUT_PROOF_SCHEMA.encode("ascii") not in executable_bytes):
        fail("live GPTK executable lacks runtime/receipt identity ({})".format(
            input_runtime_marker()))
    # nxinput 0.10.0 seam identity, required exactly when the port claims
    # the V3 layout contract: the semantic-domain receipt line and the
    # canonical live-database path are compile-time constants of the new C6
    # boundary, so a V3 default packaged with a pre-0.10.0 executable fails
    # closed here.
    if config.get("gptk_schema") in (3, 4):
        if b"NXC6-DOMAIN" not in executable_bytes:
            fail("a NEXTOS_CONTROLLERS/3 port requires the nxinput 0.10.0 "
                 "domain receipt identity (NXC6-DOMAIN) in the executable")
        if config.get("gptk_schema") == 4 and \
                b"NXC6-PROVIDER" not in executable_bytes:
            fail("a NEXTOS_CONTROLLERS/4 port requires the nxinput 0.11.0 "
                 "provider descriptor receipt (NXC6-PROVIDER) in the executable")
        if b"/usr/lib/gamecontrollerdb.txt" not in executable_bytes:
            fail("a NEXTOS_CONTROLLERS/3 port requires the nxinput 0.10.0 "
                 "live-database acquisition identity in the executable")
    # Mission 5.8: the quarantined generic-fallback family (commit 98e051f)
    # must never re-enter a packaged executable. Official mapping lines in
    # data files are fixtures; a compiled generic fallback is a defect.
    for forbidden in (b"nx_add_generic_gamepad_mappings",
                      b"Generic Xbox Fallback"):
        if forbidden in executable_bytes:
            fail("live GPTK executable carries the quarantined generic "
                 "gamepad fallback ({})".format(
                     forbidden.decode("ascii")))
    elf_has_godot_marker = \
        INPUT_GODOT_RUNTIME_MARKER.encode("ascii") in executable_bytes
    elf_has_frame_proof_marker = \
        INPUT_GODOT_FRAME_PROOF_MARKER.encode("ascii") in executable_bytes
    if elf_has_godot_marker != elf_has_frame_proof_marker:
        fail("live GPTK executable has a partial Godot runtime identity")
    runtime_proof = proof["runtime"]
    godot_marker = runtime_proof.get("godot_marker")
    frame_proof_marker = runtime_proof.get("frame_proof_marker")
    if (godot_marker is None) != (frame_proof_marker is None):
        fail("candidate input proof has a partial Godot runtime identity")
    proof_has_godot_identity = godot_marker is not None
    if proof_has_godot_identity != elf_has_godot_marker:
        fail("candidate proof and ELF differ on the Godot runtime identity")
    defined_symbols = _defined_symbols_from_full_table(executable_record)
    required_symbols = set(INPUT_RUNTIME_REQUIRED_SYMBOLS)
    if proof_has_godot_identity:
        required_symbols.update(INPUT_GODOT_FRAME_PROOF_REQUIRED_SYMBOLS)
    actions_by_id = {}
    expected_sinks = set()
    for index, action in enumerate(actions):
        if not isinstance(action, dict):
            fail("controls.actions[{}] is malformed".format(index))
        action_id = action.get("id")
        kind = action.get("kind")
        sinks = action.get("sinks")
        if (not isinstance(action_id, str) or kind not in
                ("button", "axis", "vector") or
                not isinstance(sinks, list) or not sinks):
            fail("controls.actions[{}] is malformed".format(index))
        actions_by_id[action_id] = action
        expected_sinks.update(sinks)
        if kind == "vector":
            required_symbols.update(("nxinput_gptk_live_register_vector",
                                     "nxinput_gptk_live_feed_vector"))
    runtime_symbols = set(proof["runtime"]["symbols"])
    if not required_symbols.issubset(runtime_symbols) or \
            not runtime_symbols.issubset(defined_symbols):
        fail("candidate input proof runtime symbols differ from the final ELF")
    missing_defined = sorted(required_symbols - defined_symbols)
    if missing_defined:
        fail("live GPTK executable lacks defined boundary symbol(s): {}".format(
            ", ".join(missing_defined)))

    proof_contexts = {item["context"]: item for item in proof["contexts"]}
    if set(proof_contexts) != set(contexts):
        fail("candidate input proof does not observe every declared context")

    proof_sinks = {item["sink"]: item for item in proof["sinks"]}
    if set(proof_sinks) != expected_sinks:
        fail("candidate input proof sink closure differs from adapter actions")
    records_by_target = {item.get("target"): item for item in records}
    for sink, evidence in proof_sinks.items():
        symbol = evidence["symbol"]
        if symbol.startswith("nxinput_") or symbol not in defined_symbols:
            fail("GPTK sink {} is not a defined port/engine sink".format(sink))
        if evidence["method"] == "contract-artifact":
            artifact = records_by_target.get(evidence["artifact"])
            if artifact is None or artifact.get("sha256") != \
                    evidence["artifact_sha256"]:
                fail("GPTK sink {} contract artifact is absent or stale".format(
                    sink))

    expected_cases = set()
    for context_name, bindings in contexts.items():
        if not isinstance(bindings, dict):
            fail("controls context {} is malformed".format(context_name))
        source = proof_contexts[context_name]["source"]
        for control, action_id in bindings.items():
            if action_id in ("native", "null"):
                continue
            action = actions_by_id.get(action_id)
            if action is None:
                fail("controls context references an undeclared action")
            event = {"button": "press", "axis": "axis",
                     "vector": "motion"}[action["kind"]]
            for sink in action["sinks"]:
                expected_cases.add((context_name, source, control, event,
                                    action_id, sink, 1))
    actual_cases = {
        (item["context"], item["context_source"], item["control"],
         item["event"], item["action"], item["sink"],
         item["delivery_count"])
        for item in proof["cases"]
    }
    if actual_cases != expected_cases or len(actual_cases) != len(proof["cases"]):
        missing = len(expected_cases - actual_cases)
        extra = len(actual_cases - expected_cases)
        fail("candidate input proof event->decision->sink closure differs "
             "(missing={} extra={})".format(missing, extra))


def validate_candidate_lock(records, config, required=None):
    """Bind the frozen external proof lock to the source/stage executable."""
    if required is None:
        required = bool(config.get("candidate_lock_required"))
    lock = config.get("candidate_lock")
    if lock is None:
        if required:
            fail("a frozen external candidate lock is required before packaging")
        return
    lock = normalize_candidate_lock(lock, "candidate lock")
    nxport = config.get("nxport_manifest")
    if not isinstance(nxport, dict):
        fail("candidate lock cannot resolve the canonical nxport executable")
    executable = safe_relative(
        nxport.get("executable"), "nxport executable for candidate lock"
    )
    target = PurePosixPath(config["port_dir"], executable).as_posix()
    if lock["executable"] != target:
        fail("candidate lock executable {} differs from nxport executable {}".format(
            lock["executable"], target))
    proof = lock.get("video_proof")
    if nxport.get("video_proof") == "required" and proof is None:
        fail("video_proof=required needs an OK non-black receipt in the external candidate lock")
    if proof is not None:
        package_id = config.get("package_id")
        generation_id = config.get("generation_id")
        if not package_id or proof.get("port_id") != package_id:
            fail("candidate video proof port_id differs from the packaged port")
        if not generation_id or proof.get("generation") != generation_id:
            fail("candidate video proof generation is absent or stale")
    matches = [record for record in records if record.get("target") == target]
    if len(matches) != 1:
        fail("candidate lock executable must resolve to exactly one packaged file")
    record = matches[0]
    if (record.get("kind") not in LINUX_ELF_KINDS or
            record.get("mode") != 0o755):
        fail("candidate lock executable is not the classified executable at mode 0755")
    actual = sha256_file(record["actual_path"])
    if actual != lock["sha256"] or record.get("sha256") != lock["sha256"]:
        fail("candidate lock SHA-256 differs from packaged executable {}".format(
            target))
    _validate_input_proof_contract(
        records, config, record, lock.get("input_proof"))
    config["candidate_lock"] = lock
    config["candidate_lock_verified"] = True


def validate_video_proof_contract(records, config):
    """Close the opt-in health proof against executable wiring, not JSON.

    `video_proof=required` is additive; an absent field keeps the historical
    behavior.  A required producer must call both canonical nxgl boundaries,
    consume NXBOOTSTRAP_VIDEO_FILE and carry the exact schema/receipt strings.
    Merely adding the nxport field or embedding one slogan cannot satisfy it.
    """
    nxport = config.get("nxport_manifest") or {}
    mode = nxport.get("video_proof")
    if mode is None:
        return
    if mode != "required":
        fail("nxport video_proof must be required when declared")
    executable = safe_relative(
        nxport.get("executable"), "nxport video-proof executable"
    )
    target = PurePosixPath(config["port_dir"], executable).as_posix()
    matches = [record for record in records if record.get("target") == target]
    if len(matches) != 1:
        fail("video_proof=required cannot resolve exactly one executable")
    record = matches[0]
    path = record["actual_path"]
    if record.get("kind") not in LINUX_ELF_KINDS or not is_elf(path):
        fail("video_proof=required executable is not an auditable Linux ELF")
    symbol_table = run_readelf(path, ("--syms", "--wide"))
    symbols = set()
    for line in symbol_table.splitlines():
        fields = line.split()
        if len(fields) >= 8:
            symbols.add(fields[7].split("@", 1)[0])
    required_symbols = {
        "nxgl_frame_proof_before_present",
        "nxgl_frame_proof_publish",
    }
    missing_symbols = sorted(required_symbols - symbols)
    if missing_symbols:
        fail("video_proof=required executable lacks auditable wiring symbol(s): {}".format(
            ", ".join(missing_symbols)))
    required_literals = (
        b"org.nextos.nxruntime.video-proof",
        b"VIDEO-PROOF:",
        b"NXBOOTSTRAP_VIDEO_FILE",
    )
    found = {literal: False for literal in required_literals}
    overlap = b""
    try:
        with open(str(path), "rb") as stream:
            while True:
                chunk = stream.read(256 * 1024)
                if not chunk:
                    break
                window = overlap + chunk
                for literal in required_literals:
                    found[literal] = found[literal] or literal in window
                overlap = window[-128:]
    except OSError as error:
        fail("cannot audit video-proof executable {}: {}".format(target, error))
    missing_literals = [literal.decode("ascii") for literal in required_literals
                        if not found[literal]]
    if missing_literals:
        fail("video_proof=required executable lacks runtime wiring literal(s): {}".format(
            ", ".join(missing_literals)))


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
    validate_generation_store_elf_mirrors(records, config)
    used_exceptions = set()
    elf_results = []
    seed_descriptors = {}
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
        # V3-STATE-01: NEXTOSCONTROLLERS.gptk outside defaults/ is the
        # owner's editable copy (mutable owner state). It is materialized at
        # first boot and preserved by updates; a ZIP shipping it would
        # overwrite the owner's mapping on every install.
        if (basename == "nextoscontrollers.gptk" and
                "defaults" not in parts[:-1]):
            fail(
                "owner-state NEXTOSCONTROLLERS.gptk cannot enter a release; "
                "ship only defaults/NEXTOSCONTROLLERS.gptk: {}".format(
                    logical_path)
            )
        descriptor = scan_text_for_private_data(
            path, logical_path, record["kind"]
        )
        if descriptor is not None:
            seed_descriptors[logical_path] = descriptor
        if record_is_shell(record):
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
    validate_execution_role_elfs(elf_results, config)
    validate_symbol_floor(elf_results, config)
    validate_video_receipt(elf_results, config)
    validate_nxextract(records, config)
    validate_installation_document(records, config)
    validate_owner_data_contract(records, config)
    validate_portmaster_metadata(records, config, maximum)
    # Run the immutable generator closure after semantic/content diagnostics.
    # A malformed package still reports its precise contract failure, while no
    # successful validate/stage/verify boundary can bypass a stale receipt.
    validate_generation_receipt(records, config)
    validate_owner_runtime_contract(records, config)
    validate_vendor_pin_contract(config)
    validate_port_init_predicate(config)
    validate_controls_closure(records, config)
    validate_prompt_capture_proof(config)
    validate_runtime_seed(records, config, seed_descriptors)
    validate_provider_policy(records, config)
    validate_video_proof_contract(records, config)
    validate_candidate_lock(records, config)
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


PAD_ORDINAL_DEFINITION = re.compile(
    r"\bstatic\s+(?:int|void)\s+\w*pad_ordinal_fix_apply\s*\(")
PAD_ORDINAL_SOURCE_SUFFIXES = (".h", ".hpp", ".c", ".cc", ".cpp")
PAD_ORDINAL_SKIP_DIRS = frozenset((
    ".git", "build", "tmpbuild", "gamedata", "gamedata-template", "package",
    "stage", "dist", "third_party", "__pycache__",
))


def pad_ordinal_gate_is_present(text):
    """A correcao ordinal so pode existir com o gate de barramento externo.

    O pad interno do H700 publica BTN_C/BTN_Z e casa com a assinatura antiga;
    sem o gate BUS_HOST a correcao troca os botoes de um controle que ja estava
    certo. Aceitamos o helper canonico do nxinput ou uma copia local que ainda
    decida por bustype, e recusamos qualquer variante que aceite barramento
    interno.
    """
    if "nxinput_pad_ordinal_signature" in text:
        return True
    if "bustype" not in text:
        return False
    for name in ("pad_ord_external_bus", "nxinput_pad_ordinal_bus_is_external"):
        marker = name + "(unsigned short"
        if marker not in text:
            continue
        body = text.split(marker, 1)[1]
        body = body.split("}", 1)[0]
        if "BUS_USB" not in body or "BUS_BLUETOOTH" not in body:
            return False
        for internal in ("BUS_HOST", "BUS_I2C", "BUS_SPI", "BUS_VIRTUAL"):
            if internal in body:
                return False
        return True
    return False


def validate_pad_ordinal_sources(config):
    """Recusa a assinatura ordinal herdada que nao olha o barramento."""
    root = config["source_root"]
    offenders = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        if path.suffix.lower() not in PAD_ORDINAL_SOURCE_SUFFIXES:
            continue
        try:
            relative = path.relative_to(root)
        except ValueError:
            continue
        if PAD_ORDINAL_SKIP_DIRS.intersection(relative.parts[:-1]):
            continue
        if path.stat().st_size > 1024 * 1024:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if "pad_ordinal_fix_apply" not in text:
            continue
        if not PAD_ORDINAL_DEFINITION.search(text):
            continue
        if not pad_ordinal_gate_is_present(text):
            offenders.append(relative.as_posix())
    if offenders:
        fail(
            "legacy ordinal pad fix without the external-bus gate: {}. "
            "Use framework/nxinput/include/nxinput_pad_ordinal_fix.h; an "
            "internal pad (BUS_HOST/BUS_I2C, e.g. H700) must never be "
            "remapped.".format(", ".join(offenders))
        )


def validate_sources(config):
    validate_pad_ordinal_sources(config)
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


def create_metadata(config, records, elf_results, maximum,
                    artifact_tool_version=TOOL_VERSION):
    if artifact_tool_version != TOOL_VERSION:
        fail("new release metadata must use nxrelease {}".format(TOOL_VERSION))
    inventory = []
    for record in records:
        inventory.append({
            "kind": record["kind"],
            "mode": "{:04o}".format(record["mode"]),
            "path": record["target"],
            "sha256": sha256_file(record["actual_path"]),
        })
    release_authority = config_release_authority(config)
    metadata = {
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
        "nxsplash": (
            dict(config["nxsplash"])
            if config.get("nxsplash") is not None else None
        ),
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
        "release_authority": release_authority,
        "schema_version": SCHEMA_VERSION,
        "sdl3_exception": (
            dict(config["sdl3_exception"])
            if config.get("sdl3_exception") is not None else None
        ),
        "tool": {"name": "nxrelease", "version": artifact_tool_version},
    }
    if release_authority["mode"] == RELEASE_AUTHORITY_LOCK:
        if not config.get("candidate_lock_verified"):
            fail("candidate lock was not verified against the staged executable")
        metadata["candidate_lock"] = dict(config["candidate_lock"])
    else:
        metadata["candidate_lock"] = None
    return metadata


def create_sbom(config, records, elf_results,
                artifact_tool_version=TOOL_VERSION):
    """Project a deterministic CycloneDX 1.5 BOM from the audited inventory.

    The serial number and metadata timestamp are derived from the pinned
    package identity and source_date_epoch, so the SBOM is byte-reproducible
    with the rest of the release.  The BOM is an authoritative projection of
    NXRELEASE-METADATA; verify_stage/verify_archive re-check its coverage and
    per-file hashes against the inventory.
    """
    if artifact_tool_version != TOOL_VERSION:
        fail("new release SBOM must use nxrelease {}".format(TOOL_VERSION))
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
                       "version": artifact_tool_version}],
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


def stage_release(config, destination, artifact_tool_version=TOOL_VERSION):
    artifact_tool_version = require_string(
        artifact_tool_version, "artifact nxrelease metadata version"
    )
    if artifact_tool_version != TOOL_VERSION:
        fail("new stage/package must use nxrelease {} policy".format(
            TOOL_VERSION))
    release_authority = config_release_authority(config)
    config["release_authority"] = release_authority
    if (release_authority["mode"] == RELEASE_AUTHORITY_LOCK and
            config.get("candidate_lock") is None):
        fail("candidate-lock authority requires a frozen external candidate lock")
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
        metadata = create_metadata(
            config, stage_records, elf_results, maximum,
            artifact_tool_version=artifact_tool_version,
        )
        metadata_name, checksum_name, sbom_name = internal_paths(config["port_dir"])
        metadata_target = temporary / PurePosixPath(metadata_name)
        sbom_target = temporary / PurePosixPath(sbom_name)
        checksum_target = temporary / PurePosixPath(checksum_name)
        metadata_target.parent.mkdir(parents=True, exist_ok=True)
        write_bytes(metadata_target, json_bytes(metadata), 0o644, config["epoch"])
        sbom = create_sbom(
            config, stage_records, elf_results,
            artifact_tool_version=artifact_tool_version,
        )
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


def validate_internal_metadata(metadata, requested_ceiling=None,
                               historical_authority=None):
    metadata = require_object(metadata, "release metadata")
    require_keys(
        metadata,
        ("archive", "audit_exceptions", "candidate_lock", "dependencies", "elf_audit", "input_manifest_sha256", "inventory", "nxextract", "nxsplash", "package", "portmaster_metadata", "release_authority", "schema_version", "sdl3_exception", "tool"),
        "release metadata",
    )
    if metadata.get("schema_version") != SCHEMA_VERSION:
        fail("release metadata schema_version is unsupported")
    authority = None
    if historical_authority is not None:
        authority = normalize_historical_authority(
            historical_authority, "authenticated historical authority"
        )
    tool = require_object(metadata.get("tool"), "release metadata tool")
    require_keys(tool, ("name", "version"), "release metadata tool")
    if tool.get("name") != "nxrelease":
        fail("release metadata tool name is invalid")
    tool_version = require_string(
        tool.get("version"), "release metadata tool.version"
    )
    version_tuple(tool_version, "release metadata tool.version")
    if authority is None:
        if tool_version != TOOL_VERSION:
            fail("ordinary verification requires nxrelease {} metadata; use an external historical authority for read-only legacy audit".format(
                TOOL_VERSION))
        expected_bootstrap_version = NXBOOTSTRAP_REQUIRED_VERSION
    else:
        if tool_version != authority["metadata_tool_version"]:
            fail("historical authority metadata version differs from the archive")
        expected_bootstrap_version = authority["nxbootstrap_version"]
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
    if launcher_contract["version"] != expected_bootstrap_version:
        fail("metadata launcher must pin canonical nxbootstrap {}".format(
            expected_bootstrap_version
        ))

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

    nxsplash_value = metadata.get("nxsplash")
    if bootstrap_requires_nxsplash(launcher_contract["version"]):
        nxsplash_value = require_object(
            nxsplash_value, "release metadata nxsplash"
        )
        require_keys(
            nxsplash_value,
            ("architecture", "duration_ms", "path",
             "release_manifest_sha256", "sha256", "source_sha256",
             "version"),
            "release metadata nxsplash",
        )
        nxsplash_path = safe_relative(
            nxsplash_value.get("path"), "metadata nxsplash.path"
        )
        expected_nxsplash_path = port_dir + "/" + NXSPLASH_RUNTIME_NAME
        if nxsplash_path != expected_nxsplash_path:
            fail("metadata nxsplash.path is not canonical")
        nxsplash_architecture = require_string(
            nxsplash_value.get("architecture"),
            "metadata nxsplash.architecture",
        )
        if nxsplash_architecture not in ARCH_MACHINES:
            fail("metadata nxsplash architecture is unsupported")
        nxsplash_version = require_string(
            nxsplash_value.get("version"), "metadata nxsplash.version"
        )
        version_tuple(nxsplash_version, "metadata nxsplash.version")
        if nxsplash_version != NXSPLASH_REQUIRED_VERSION:
            fail("metadata nxsplash version must be {}".format(
                NXSPLASH_REQUIRED_VERSION
            ))
        if nxsplash_value.get("duration_ms") != 5000:
            fail("metadata nxsplash duration must be exactly 5000 ms")
        nxsplash = {
            "architecture": nxsplash_architecture,
            "duration_ms": 5000,
            "path": nxsplash_path,
            "release_manifest_sha256": parse_sha256(
                nxsplash_value.get("release_manifest_sha256"),
                "metadata nxsplash.release_manifest_sha256",
            ),
            "sha256": parse_sha256(
                nxsplash_value.get("sha256"), "metadata nxsplash.sha256"
            ),
            "source_sha256": parse_sha256(
                nxsplash_value.get("source_sha256"),
                "metadata nxsplash.source_sha256",
            ),
            "version": nxsplash_version,
        }
        nxsplash_inventory = inventory_map.get(nxsplash_path)
        if (nxsplash_inventory is None or
                nxsplash_inventory["kind"] != "nxsplash-linux" or
                nxsplash_inventory["mode"] != 0o755 or
                nxsplash_inventory["sha256"] != nxsplash["sha256"]):
            fail("metadata nxsplash is absent, misclassified, unsafe or unpinned")
    else:
        if nxsplash_value is not None:
            fail("metadata nxsplash is valid only for a supporting nxbootstrap")
        nxsplash = None

    dependencies = validate_dependencies_manifest(
        metadata.get("dependencies"), port_dir
    )
    if metadata.get("dependencies") != dependencies:
        fail("release metadata dependencies are not canonical/sorted")
    sdl3_exception = normalize_sdl3_exception(
        metadata.get("sdl3_exception"),
        "release metadata sdl3_exception", port_dir,
    )

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
        "runtime_env_path", "runtime_env_sha256", "ui_path", "ui_sha256",
        "ui_architecture", "ui_glibc_max", "ui_release_manifest_sha256",
        "ui_source_sha256", "ui_version",
        "recipe_path",
        "recipe_sha256", "sha256", "version",
    ), "release metadata nxextract")
    nx_config = {
        "minimum_version": require_string(nx.get("minimum_version"), "metadata nxextract.minimum_version"),
        "path": safe_relative(nx.get("path"), "metadata nxextract.path"),
        "runner_path": safe_relative(nx.get("runner_path"), "metadata nxextract.runner_path"),
        "runner_sha256": parse_sha256(nx.get("runner_sha256"), "metadata nxextract.runner_sha256"),
        "runtime_env_path": safe_relative(nx.get("runtime_env_path"), "metadata nxextract.runtime_env_path"),
        "runtime_env_sha256": parse_sha256(nx.get("runtime_env_sha256"), "metadata nxextract.runtime_env_sha256"),
        "ui_path": safe_relative(nx.get("ui_path"), "metadata nxextract.ui_path"),
        "ui_sha256": parse_sha256(nx.get("ui_sha256"), "metadata nxextract.ui_sha256"),
        "ui_architecture": require_string(
            nx.get("ui_architecture"), "metadata nxextract.ui_architecture"),
        "ui_glibc_max": validate_ceiling(
            nx.get("ui_glibc_max"), "metadata nxextract.ui_glibc_max"),
        "ui_release_manifest_sha256": parse_sha256(
            nx.get("ui_release_manifest_sha256"),
            "metadata nxextract.ui_release_manifest_sha256"),
        "ui_source_sha256": parse_sha256(
            nx.get("ui_source_sha256"),
            "metadata nxextract.ui_source_sha256"),
        "ui_version": require_string(
            nx.get("ui_version"), "metadata nxextract.ui_version"),
        "recipe_path": safe_relative(nx.get("recipe_path"), "metadata nxextract.recipe_path"),
        "recipe_sha256": parse_sha256(nx.get("recipe_sha256"), "metadata nxextract.recipe_sha256"),
        "sha256": parse_sha256(nx.get("sha256"), "metadata nxextract.sha256"),
        "version": require_string(nx.get("version"), "metadata nxextract.version"),
    }
    version_tuple(nx_config["version"], "metadata nxextract.version")
    version_tuple(nx_config["minimum_version"], "metadata nxextract.minimum_version")
    version_tuple(nx_config["ui_version"], "metadata nxextract.ui_version")
    if version_lt(nx_config["minimum_version"], NXEXTRACT_FLOOR):
        fail("archived NXExtract minimum is below tool floor {}".format(NXEXTRACT_FLOOR))
    if version_lt(nx_config["version"], nx_config["minimum_version"]):
        fail("archived NXExtract version is below its minimum")
    if nx_config["version"] not in NXEXTRACT_ENGINES:
        fail("archived NXExtract version is not a supported engine")
    # O pacote pode legitimamente carregar um motor mais antigo -- e' o opt-in.
    # O que nao pode e' carregar a versao antiga com os bytes de outra.
    archived_identity = NXEXTRACT_ENGINES[nx_config["version"]]
    if nx_config["ui_architecture"] not in ARCH_MACHINES:
        fail("archived NXExtract UI architecture is unsupported")
    canonical_ui = canonical_nxextract_ui_contract(
        nx_config["ui_architecture"]
    )
    canonical_ui_metadata = {
        "ui_glibc_max": canonical_ui["glibc_max"],
        "ui_release_manifest_sha256": canonical_ui["release_manifest_sha256"],
        "ui_sha256": canonical_ui["sha256"],
        "ui_source_sha256": canonical_ui["source_sha256"],
        "ui_version": canonical_ui["version"],
    }
    for field, expected in canonical_ui_metadata.items():
        if nx_config[field] != expected:
            fail("archived NXExtract {} is not canonical".format(field))
    canonical_nxextract_hashes = {
        "sha256": archived_identity["engine_sha256"],
        "runner_sha256": archived_identity["runner_sha256"],
        "runtime_env_sha256": archived_identity["runtime_env_sha256"],
    }
    for hash_field, expected_hash in canonical_nxextract_hashes.items():
        if nx_config[hash_field] != expected_hash:
            fail("archived NXExtract {} differs from the {} identity".format(
                hash_field, nx_config["version"]))
    expected_nx = {
        "path": (port_dir + "/nxextract/nxextract.py", "nxextract", "sha256"),
        "runner_path": (port_dir + "/nxextract/run-extractor.sh", "nxextract-runner", "runner_sha256"),
        "runtime_env_path": (port_dir + "/nxextract/nxextract-runtime-env.sh", "nxextract-runtime-env", "runtime_env_sha256"),
        "ui_path": (port_dir + "/nxextract/nxextract-ui", "nxextract-ui-linux", "ui_sha256"),
        "recipe_path": (port_dir + "/extractor.json", "nxextract-recipe", "recipe_sha256"),
    }
    for field, (expected_path, expected_kind, hash_field) in expected_nx.items():
        if nx_config[field] != expected_path:
            fail("archived NXExtract {} is not canonical".format(field))
        inventory_item = inventory_map.get(expected_path)
        if inventory_item is None or inventory_item["kind"] != expected_kind:
            fail("archived NXExtract {} has the wrong inventory kind".format(field))
        expected_mode = 0o755 if field == "ui_path" else 0o644
        if inventory_item["mode"] != expected_mode:
            fail("archived NXExtract {} has the wrong mode".format(field))
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

    if authority is None:
        release_authority = normalize_release_authority(
            metadata.get("release_authority"),
            "release metadata release_authority",
        )
    else:
        # Historical verification predates the V5 authority field and remains
        # read-only.  It never becomes a current human-approved package.
        release_authority = {
            "mode": RELEASE_AUTHORITY_HUMAN,
            "machine_receipts_required": False,
        }
    candidate_lock = candidate_lock_from_metadata(
        metadata, authority, release_authority
    )
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
        "candidate_lock": candidate_lock,
        "candidate_lock_required": candidate_lock is not None,
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
        "nxsplash": nxsplash,
        "package_id": package_id,
        "package_version": require_string(package.get("version"), "metadata package.version"),
        "port_dir": port_dir,
        "portmaster_metadata": portmaster_metadata,
        "sdl3_exception": sdl3_exception,
        "historical_authority": authority,
        "historical_read_only": authority is not None,
        "publication_eligible": authority is None,
        "release_authority": release_authority,
        "tool_version": tool_version,
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


def verify_stage(stage, requested_ceiling=None, historical_authority=None):
    stage_input = Path(stage)
    if stage_input.is_symlink() or not stage_input.is_dir():
        fail("stage is missing, not a directory, or a symlink: {}".format(stage_input))
    stage = stage_input.resolve()
    discovered_port_dir, metadata_name, checksum_name, sbom_name = discover_stage_internal_paths(stage)
    metadata_path = stage / PurePosixPath(metadata_name)
    metadata = load_json(metadata_path, "release metadata")
    config = validate_internal_metadata(
        metadata, requested_ceiling=requested_ceiling,
        historical_authority=historical_authority,
    )
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
            # Reconstruct the same pinned record contract audited before the
            # copy.  validate_generation_receipt deliberately refuses an
            # unpinned controls/settings default; dropping this field while
            # reopening our own authenticated inventory made a valid stage
            # look unpinned and broke stage/bundle after source validation had
            # already passed.
            "sha256": item["sha256"],
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

    if config["historical_read_only"]:
        # The external authority authenticates these immutable archive bytes.
        # Historical mode is deliberately read-only: it checks exact
        # inventory/SBOM/checksums and that every recorded ELF is still an ELF,
        # but never re-certifies it under current product/provider policy.
        for target, item in sorted(config["inventory"].items()):
            classified = item["kind"] in ELF_KINDS
            recorded = target in elf_metadata
            if classified != recorded:
                fail("historical ELF classification differs at {}".format(
                    target))
            if recorded and not is_elf(stage / PurePosixPath(target)):
                fail("historical metadata claims a non-ELF at {}".format(
                    target))
        return {
            "elf_count": len(elf_metadata),
            "max_glibc": config["elf_audit"].get("max_glibc_seen"),
            "package_id": config["package_id"],
            "package_version": config["package_version"],
            "config": config,
        }

    audit_config = {
        "ceiling": config["ceiling"],
        "candidate_lock": config["candidate_lock"],
        "candidate_lock_required": config["candidate_lock_required"],
        "dependencies": config["dependencies"],
        "exception_map": config["exception_map"],
        "launcher": config["launcher"],
        "launcher_chain": config["launcher_chain"],
        "launcher_contract": config["launcher_contract"],
        "nxextract": config["nxextract"],
        "nxsplash": config["nxsplash"],
        "package_id": config["package_id"],
        "port_dir": config["port_dir"],
        "portmaster_metadata": config["portmaster_metadata"],
        "release_authority": config["release_authority"],
        "sdl3_exception": config["sdl3_exception"],
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
    if config["release_authority"]["mode"] == RELEASE_AUTHORITY_LOCK:
        if not audit_config.get("candidate_lock_verified"):
            fail("reopened stage did not verify the candidate lock")
        config["candidate_lock_verified"] = True
    return {
        "elf_count": len(elf_results),
        "max_glibc": maximum,
        "package_id": config["package_id"],
        "package_version": config["package_version"],
        "config": config,
    }


def zip_timestamp(epoch):
    value = list(time.gmtime(epoch)[:6])
    value[5] -= value[5] % 2
    return tuple(value)


def create_archive(stage, output, defer_verification=False):
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
        if not defer_verification:
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
        result = create_archive(
            stage, temporary_archive, defer_verification=True
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


def audit_public_portmaster_zip(archive_path, previous_archive=None):
    auditor = PORTMASTER_ZIP_AUDITOR_PATH
    if auditor.is_symlink() or not auditor.is_file():
        fail("canonical PortMaster ZIP auditor is missing or unsafe")
    command = [sys.executable, "-B", str(auditor), str(archive_path)]
    if previous_archive is not None:
        previous_input = Path(previous_archive)
        if previous_input.is_symlink() or not previous_input.is_file():
            fail("previous archive is missing, not regular, or a symlink")
        command.extend(["--previous", str(previous_input.resolve())])
    process = subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        timeout=300,
    )
    if process.returncode != 0:
        detail = (process.stderr or process.stdout).strip()
        fail("final PortMaster ZIP audit failed: {}".format(
            detail or "auditor exited {}".format(process.returncode)
        ))


def verify_archive(archive_path, requested_ceiling=None, checksum_path=None,
                   previous_archive=None, historical_authority_path=None):
    archive_input = Path(archive_path)
    if archive_input.is_symlink() or not archive_input.is_file():
        fail("archive is missing, not regular, or a symlink: {}".format(archive_input))
    archive_path = archive_input.resolve()
    historical_authority = None
    if historical_authority_path is not None:
        if previous_archive is not None:
            fail("historical quarantine verification cannot compare/publish an update")
        historical_authority = load_historical_authority(
            historical_authority_path, archive_path
        )
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
            metadata = parse_json_strict(
                archive.read(metadata_name), "archive release metadata"
            )
            internal = validate_internal_metadata(
                metadata, requested_ceiling=requested_ceiling,
                historical_authority=historical_authority,
            )
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
        result = verify_stage(
            temporary, requested_ceiling=requested_ceiling,
            historical_authority=historical_authority,
        )
        # A schema-3 package is not verified merely because every individual
        # ZIP member matches nxrelease.json.  Its live runtime, immutable
        # rollback store, control files and GENERATION.json must describe the
        # same closed generation.  Keep this in the ordinary ``verify`` path
        # so a custom build wrapper cannot bypass the atomic-generation gate.
        if historical_authority is None:
            _verify_schema3_generation_archive(
                archive_path, temporary, result["config"]
            )
        # GAMEDATA-DIR-01: prove on the extracted bytes that a clean
        # installation physically materializes the documented owner-data
        # directory. ZIPs carry no directory entries, so only the marker file
        # can create <port>/gamedata/ on disk.
        if (historical_authority is None and
                isinstance(result.get("config", {}).get("nxextract"), dict)):
            extracted_port = temporary / result["config"]["port_dir"]
            extracted_gamedata = extracted_port / "gamedata"
            if not extracted_gamedata.is_dir() or not (
                extracted_gamedata / "README.txt"
            ).is_file():
                fail(
                    "clean extraction of the final ZIP did not materialize "
                    "{}/gamedata/README.txt".format(result["config"]["port_dir"])
                )
        # This is the real ZIP path (temporary pre-publication or final
        # published bytes), not a reconstructed stage. Keep the independent
        # public firmware auditor in the build and verify paths permanently.
        if historical_authority is None:
            if previous_archive is None:
                audit_public_portmaster_zip(archive_path)
            else:
                audit_public_portmaster_zip(archive_path, previous_archive)
        if (historical_authority is not None and
                sha256_file(archive_path) !=
                historical_authority["archive_sha256"]):
            fail("historical archive bytes changed during read-only verification")
        return result
    finally:
        shutil.rmtree(str(temporary))


def _public_final_exact_keys(value, required, optional, context):
    value = require_object(value, context)
    required = set(required)
    optional = set(optional)
    missing = sorted(required - set(value))
    unknown = sorted(set(value) - required - optional)
    if missing or unknown:
        detail = []
        if missing:
            detail.append("missing={}".format(",".join(missing)))
        if unknown:
            detail.append("unknown={}".format(",".join(unknown)))
        fail("{} fields are not canonical ({})".format(
            context, "; ".join(detail)
        ))
    return value


def _public_final_string_list(value, context, nonempty=True):
    if (not isinstance(value, list) or
            (nonempty and not value) or
            any(not isinstance(item, str) or not item for item in value)):
        fail("{} must be {}array of non-empty strings".format(
            context, "a non-empty " if nonempty else "an "
        ))
    if value != sorted(set(value)):
        fail("{} must be sorted and unique".format(context))
    return value


def _public_final_safe_identifier(value, context):
    value = require_string(value, context)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", value):
        fail("{} contains an unsafe identifier".format(context))
    return value


def _public_final_reject_private_document(value, context):
    payload = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    if PRIVATE_PATH_RE.search(payload) or IPV4_RE.search(payload) or \
            HOST_LITERAL_RE.search(payload):
        fail("{} contains private host information".format(context))
    if _contains_secret_literal(payload, context):
        fail("{} contains a credential-like literal".format(context))


def _public_final_archive_member(archive, name, context):
    try:
        info = archive.getinfo(name)
    except KeyError:
        fail("{} is absent from the final ZIP: {}".format(context, name))
    if info.file_size > 16 * 1024 * 1024:
        fail("{} is unexpectedly large".format(context))
    return archive.read(info)


def _public_final_artifact_records(generation):
    artifacts = generation.get("artifacts")
    if not isinstance(artifacts, list):
        fail("public-final GENERATION.json artifacts must be an array")
    records = {}
    for index, item in enumerate(artifacts):
        context = "public-final GENERATION.json artifacts[{}]".format(index)
        item = _public_final_exact_keys(
            item, ("path", "mode", "sha256"), (), context
        )
        path = safe_relative(item.get("path"), context + ".path")
        mode = item.get("mode")
        if mode not in ("0644", "0755"):
            fail("{}.mode must be 0644 or 0755".format(context))
        digest = parse_sha256(item.get("sha256"), context + ".sha256")
        if path in records:
            fail("public-final GENERATION.json has duplicate artifact {}".format(
                path
            ))
        records[path] = {"mode": mode, "sha256": digest}
    return records


def _public_final_inventory_binding(config, path, mode, digest, context,
                                    kinds=None):
    """Bind one authenticated generation member to the verified ZIP index."""
    item = config["inventory"].get(path)
    if item is None:
        fail("{} is absent from the final ZIP: {}".format(context, path))
    if item.get("mode") != int(mode, 8):
        fail("{} has the wrong mode: {}".format(context, path))
    if item.get("sha256") != digest:
        fail("{} has bytes different from its generation: {}".format(
            context, path
        ))
    if kinds is not None and item.get("kind") not in kinds:
        fail("{} has the wrong inventory kind: {}".format(context, path))
    return item


def _public_final_runtime_manifest_bytes(value):
    """Canonical bytes used by nxbootstrap for generation manifest.json."""
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _public_final_generation_v2(documents, config, generation_id, nxport,
                                artifacts):
    """Prove one closed schema-3 live/store generation.

    The runtime manifest is not trusted as an inventory by itself.  Every
    record must be the exact schema-3 ``generation_runtime`` declaration, its
    live path and immutable rollback path must carry the same SHA/mode, and
    all control files are reconstructed here from their authenticated inputs.
    This deliberately treats NXExtract helpers/specs as ordinary declared
    members instead of learning game-specific names.
    """
    runtime = _public_final_exact_keys(
        documents["runtime_manifest"],
        ("schema", "schema_version", "generation_id", "identity_basis",
         "components"), (), "public-final runtime generation manifest",
    )
    if (runtime.get("schema") != "nxruntime-generation-v2" or
            type(runtime.get("schema_version")) is not int or
            runtime.get("schema_version") != 2 or
            runtime.get("generation_id") != generation_id):
        fail("embedded runtime generation-v2 does not match generation_id")

    generation_runtime = nxport.get("generation_runtime")
    if not isinstance(generation_runtime, list) or not generation_runtime:
        fail("public-final schema 3 requires non-empty generation_runtime")
    normalized_runtime = []
    for index, member in enumerate(generation_runtime):
        context = "public-final generation_runtime[{}]".format(index)
        member = _public_final_exact_keys(
            member, ("role", "path", "mode", "sha256"), (), context
        )
        role = require_string(member.get("role"), context + ".role")
        path = safe_relative(member.get("path"), context + ".path")
        mode = member.get("mode")
        if mode not in ("0644", "0755"):
            fail("{}.mode must be 0644 or 0755".format(context))
        digest = parse_sha256(member.get("sha256"), context + ".sha256")
        normalized_runtime.append({
            "role": role, "path": path, "mode": mode, "sha256": digest,
        })
    if normalized_runtime != generation_runtime:
        fail("public-final generation_runtime is not canonical")

    launcher_name = safe_relative(
        nxport.get("launcher_name"), "public-final nxport launcher_name"
    )
    if launcher_name != config["launcher"]:
        fail("public-final launcher differs from nxport launcher_name")
    launcher_bytes = documents.get("launcher_bytes")
    if not isinstance(launcher_bytes, bytes):
        fail("public-final launcher bytes are unavailable")
    generation_token = generation_id.encode("ascii")
    if launcher_bytes.count(generation_token) != 1:
        fail("public-final launcher does not carry exactly one generation_id")
    launcher_sha256 = hashlib.sha256(launcher_bytes).hexdigest()
    launcher_preimage = launcher_bytes.replace(
        generation_token, ("0" * 64).encode("ascii")
    )
    launcher_preimage_sha256 = hashlib.sha256(launcher_preimage).hexdigest()
    nxport_sha256 = hashlib.sha256(documents["nxport_bytes"]).hexdigest()

    components = runtime.get("components")
    if not isinstance(components, list):
        fail("public-final runtime generation-v2 components must be an array")
    expected_components = [
        {
            "role": "launcher", "path": launcher_name, "mode": "0755",
            "sha256": launcher_sha256,
        },
        {
            "role": "nxport", "path": "nxport.json", "mode": "0644",
            "sha256": nxport_sha256,
        },
    ] + normalized_runtime
    if components != expected_components:
        fail("runtime generation-v2 components differ from nxport/live bytes")

    identity_runtime = "".join(
        "{}\t{}\t{}\t{}\n".format(
            member["role"], member["mode"], member["sha256"], member["path"]
        )
        for member in normalized_runtime
    ).encode("utf-8")
    identity = _public_final_exact_keys(
        runtime.get("identity_basis"),
        ("schema", "schema_version", "nxport_sha256",
         "launcher_preimage_sha256", "runtime_records_sha256",
         "nxbootstrap", "components", "nxsplash"), (),
        "public-final generation-v2 identity_basis",
    )
    if (identity.get("schema") !=
            "org.nextos.nxruntime.generation-identity" or
            identity.get("schema_version") != 2 or
            identity.get("nxport_sha256") != nxport_sha256 or
            identity.get("launcher_preimage_sha256") !=
            launcher_preimage_sha256 or
            identity.get("runtime_records_sha256") !=
            hashlib.sha256(identity_runtime).hexdigest()):
        fail("generation-v2 identity basis differs from packaged runtime bytes")

    identity_components = copy.deepcopy(expected_components)
    identity_components[0]["sha256"] = launcher_preimage_sha256
    if identity.get("components") != identity_components:
        fail("generation-v2 identity components differ from the runtime closure")
    bootstrap_identity = _public_final_exact_keys(
        identity.get("nxbootstrap"),
        ("version", "generator_sha256", "launcher_template_sha256"), (),
        "public-final generation-v2 nxbootstrap identity",
    )
    expected_bootstrap_identity = {
        "version": NXBOOTSTRAP_REQUIRED_VERSION,
        "generator_sha256": sha256_file(NXBOOTSTRAP_GENERATOR_PATH),
        "launcher_template_sha256": sha256_file(
            NXBOOTSTRAP_GENERATOR_PATH.parent.parent /
            "templates" / "launcher.sh.in"
        ),
    }
    if bootstrap_identity != expected_bootstrap_identity:
        fail("generation-v2 identity does not bind the required nxbootstrap")

    splash_members = [
        member for member in normalized_runtime
        if member["role"] == "nxsplash"
    ]
    if len(splash_members) != 1:
        fail("generation-v2 requires exactly one nxsplash member")
    splash_identity = _public_final_exact_keys(
        identity.get("nxsplash"),
        ("version", "architecture", "sha256"), (),
        "public-final generation-v2 nxsplash identity",
    )
    expected_splash = config.get("nxsplash")
    if (not isinstance(expected_splash, dict) or
            splash_identity.get("version") != NXSPLASH_REQUIRED_VERSION or
            splash_identity.get("architecture") !=
            expected_splash.get("architecture") or
            splash_identity.get("sha256") != splash_members[0]["sha256"] or
            splash_identity.get("sha256") != expected_splash.get("sha256")):
        fail("generation-v2 nxsplash identity differs from the packaged splash")

    identity_bytes = json_bytes(identity)
    if hashlib.sha256(identity_bytes).hexdigest() != generation_id:
        fail("generation-v2 generation_id is not the identity.json SHA-256")
    components_v2 = "".join(
        "{}\t{}\t{}\t{}\n".format(
            member["role"], member["mode"], member["sha256"], member["path"]
        )
        for member in expected_components
    ).encode("utf-8")
    checksum_records = []
    for member in expected_components:
        if member["role"] == "launcher":
            internal = "launcher/" + member["path"]
        elif member["role"] == "nxport":
            internal = "nxport.json"
        else:
            internal = "runtime/" + member["path"]
        checksum_records.append((internal, member["sha256"]))
    components_sha256 = "".join(
        "{}  {}\n".format(digest, path)
        for path, digest in sorted(checksum_records)
    ).encode("utf-8")
    controls = documents.get("runtime_controls")
    expected_controls = {
        "commit": (generation_id + "\n").encode("ascii"),
        "components.sha256": components_sha256,
        "components.v2": components_v2,
        "format": b"nxruntime-generation-v2\n",
        "identity-runtime.v2": identity_runtime,
        "identity.json": identity_bytes,
        # nxbootstrap's stored runtime manifest intentionally uses the
        # json.dumps ensure_ascii=True default. identity.json uses real UTF-8;
        # the two canonical byte contracts must not be conflated.
        "manifest.json": _public_final_runtime_manifest_bytes(runtime),
    }
    if not isinstance(controls, dict) or set(controls) != set(expected_controls):
        fail("generation-v2 control-file set is incomplete")
    for name, expected_payload in expected_controls.items():
        if controls[name] != expected_payload:
            fail("generation-v2 control file is stale: {}".format(name))

    root = "{}/.nxruntime/generations/{}".format(
        config["port_dir"], generation_id
    )
    elf_audit = config.get("elf_audit", {}).get("files", [])
    elf_by_path = {
        item.get("path"): item for item in elf_audit
        if isinstance(item, dict) and isinstance(item.get("path"), str)
    }
    expected_store_paths = set()
    required_artifacts = {}
    for name, payload in expected_controls.items():
        path = root + "/" + name
        digest = hashlib.sha256(payload).hexdigest()
        _public_final_inventory_binding(
            config, path, "0644", digest,
            "public-final generation-v2 control",
            kinds={"nxruntime-generation"},
        )
        expected_store_paths.add(path)
        required_artifacts[path] = ("0644", digest)

    for member in expected_components:
        if member["role"] == "launcher":
            store_path = root + "/files/launcher/" + member["path"]
            live_path = config["launcher"]
            live_kinds = {"launcher"}
        elif member["role"] == "nxport":
            store_path = root + "/files/nxport.json"
            live_path = config["port_dir"] + "/nxport.json"
            live_kinds = {"nxbootstrap-config"}
        else:
            store_path = root + "/files/runtime/" + member["path"]
            live_path = config["port_dir"] + "/" + member["path"]
            live_kinds = None
        live_item = _public_final_inventory_binding(
            config, live_path, member["mode"], member["sha256"],
            "public-final live generation member", kinds=live_kinds,
        )
        store_kind = (
            "nxruntime-generation-linux"
            if live_item.get("kind") in ELF_KINDS else
            "nxruntime-generation"
        )
        _public_final_inventory_binding(
            config, store_path, member["mode"], member["sha256"],
            "public-final immutable generation member",
            kinds={store_kind},
        )
        if store_kind == "nxruntime-generation-linux":
            live_elf = elf_by_path.get(live_path)
            store_elf = elf_by_path.get(store_path)
            metadata_fields = (
                "architecture", "build_profile", "needed", "soname",
                "provenance",
            )
            if (live_elf is None or store_elf is None or
                    any(store_elf.get(field) != live_elf.get(field)
                        for field in metadata_fields)):
                fail("public-final generation store ELF metadata differs "
                     "from live: {}".format(member["path"]))
        expected_store_paths.add(store_path)
        required_artifacts[store_path] = (member["mode"], member["sha256"])
        required_artifacts[live_path] = (member["mode"], member["sha256"])

    generation_prefix = config["port_dir"] + "/.nxruntime/generations/"
    actual_generation_paths = {
        path for path in config["inventory"] if path.startswith(generation_prefix)
    }
    if actual_generation_paths != expected_store_paths:
        fail("public-final generation-v2 store is not one exact closed generation")
    for path, (mode, digest) in required_artifacts.items():
        record = artifacts.get(path)
        if record is None or record["mode"] != mode or \
                record["sha256"] != digest:
            fail("GENERATION.json does not bind generation-v2 artifact {}".format(
                path
            ))
    generation_receipt_path = config["port_dir"] + "/GENERATION.json"
    expected_artifact_paths = set(config["inventory"]) - {
        generation_receipt_path
    }
    if set(artifacts) != expected_artifact_paths:
        fail("GENERATION.json artifact inventory differs from the final package")
    for path, item in config["inventory"].items():
        if path == generation_receipt_path:
            continue
        record = artifacts[path]
        if (record["mode"] != "{:04o}".format(item["mode"]) or
                record["sha256"] != item["sha256"]):
            fail("GENERATION.json artifact differs from final inventory: {}".format(
                path
            ))

    _public_final_validate_live_nxextract(
        nxport, normalized_runtime, config
    )


def _public_final_nxextract_runtime_paths(normalized_runtime):
    """Return every generation member owned by the live NXExtract tree.

    NXExtract helper DSOs retain the generic private-library role, so role
    prefix alone is not a complete live-tree closure.
    """
    return {
        member["path"] for member in normalized_runtime
        if (member["role"].startswith("nxextract-") or
            (member["role"] == "private-library" and
             member["path"].startswith("nxextract/")))
    }


def _public_final_validate_live_nxextract(nxport, normalized_runtime, config):
    nxextract_paths = _public_final_nxextract_runtime_paths(
        normalized_runtime
    )
    if nxport.get("nxextract", {}).get("mode") in ("yes", "auto"):
        live_nxextract_paths = set()
        nxextract_prefix = config["port_dir"] + "/nxextract/"
        for path in config["inventory"]:
            if path == config["port_dir"] + "/extractor.json":
                live_nxextract_paths.add("extractor.json")
            elif path.startswith(nxextract_prefix):
                live_nxextract_paths.add(path[len(config["port_dir"]) + 1:])
        if live_nxextract_paths != nxextract_paths:
            fail("public-final live NXExtract closure differs from generation_runtime")
    elif nxextract_paths:
        fail("nxextract.mode=no cannot carry NXExtract generation roles")


def _public_final_generation_contract(documents, config):
    """Close nxproject -> adapter -> generation -> runtime-manifest.

    Publication accepts only the embedded current generation.  Historical
    bytes are authenticated exclusively by ``verify --historical-authority``
    in quarantine and cannot enter this helper through an alternate caller.
    """
    if documents.get("generation_mode") != "embedded":
        fail("public-final requires an embedded current generation; historical bytes belong only in quarantine")
    generation = _public_final_exact_keys(
        documents["generation"],
        ("schema", "schema_version", "generator",
         "project_manifest_sha256", "source_pins", "artifacts", "claims",
         "generation_id"),
        ("execution_roles",),
        "public-final GENERATION.json",
    )
    if (generation.get("schema") != "nxgenerator-receipt-v1" or
            type(generation.get("schema_version")) is not int or
            generation.get("schema_version") != 1):
        fail("public-final GENERATION.json schema is unsupported")
    generator = _public_final_exact_keys(
        generation.get("generator"), ("name", "version"), (),
        "public-final GENERATION.json generator",
    )
    if generator.get("name") != "nxgenerator":
        fail("public-final GENERATION.json was not emitted by nxgenerator")
    generator_version = require_string(
        generator.get("version"), "public-final GENERATION.json generator.version"
    )
    version_tuple(generator_version, "public-final generator version")
    if generator_version != NXGENERATOR_REQUIRED_VERSION:
        fail("embedded public-final GENERATION.json must come from "
             "nxgenerator {}".format(NXGENERATOR_REQUIRED_VERSION))
    source_pins = require_object(
        generation.get("source_pins"), "public-final GENERATION source_pins"
    )
    if set(source_pins) != {
            "nxbootstrap", "nxsplash", "nxextract", "portmaster"}:
        fail("public-final GENERATION source_pins are not canonical")
    for name, current in (
            ("nxbootstrap", NXBOOTSTRAP_REQUIRED_VERSION),
            ("nxsplash", NXSPLASH_REQUIRED_VERSION),
            ("nxextract", NXEXTRACT_REQUIRED_VERSION)):
        pin = source_pins.get(name)
        if pin is None and name == "nxextract":
            continue
        pin = require_object(
            pin, "public-final GENERATION source_pins." + name
        )
        pinned_version = require_string(
            pin.get("version"),
            "public-final GENERATION source_pins.{}.version".format(name),
        )
        version_tuple(pinned_version, "public-final {} source pin".format(name))
        if pinned_version != current:
            fail("embedded public-final {} source pin must be {}".format(
                name, current
            ))
    portmaster_pin = source_pins.get("portmaster")
    if not isinstance(portmaster_pin, dict) or not portmaster_pin:
        fail("public-final GENERATION portmaster source pin is incomplete")

    generation_id = parse_sha256(
        generation.get("generation_id"),
        "public-final GENERATION.json generation_id",
    )
    project_bytes = documents["project_bytes"]
    project_hash = hashlib.sha256(project_bytes).hexdigest()
    if parse_sha256(
            generation.get("project_manifest_sha256"),
            "public-final GENERATION.json project_manifest_sha256",
    ) != project_hash:
        fail("public-final generation does not bind the packaged nxproject.json")

    claims = _public_final_exact_keys(
        generation.get("claims"),
        ("deterministic_scaffold", "release_ready",
         "physical_support_proven", "adapter_lifecycle_implemented"),
        (), "public-final GENERATION.json claims",
    )
    if any(not isinstance(value, bool) for value in claims.values()):
        fail("public-final generation claims must be booleans")
    if claims.get("release_ready") is not True:
        fail("public-final rejects release_ready=false")
    if claims.get("physical_support_proven") is not True:
        fail("public-final rejects physical_support_proven=false")
    if claims.get("adapter_lifecycle_implemented") is not True:
        fail("public-final rejects an incomplete adapter lifecycle")

    project = require_object(documents["project"], "public-final nxproject.json")
    if (type(project.get("schema_version")) is not int or
            project.get("schema_version") != 3):
        fail("public-final requires nxproject schema_version 3")
    package_payload_raw = project.get("package_payload", [])
    if (not isinstance(package_payload_raw, list) or
            len(package_payload_raw) > 128):
        fail("public-final nxproject.package_payload is malformed")
    package_payload = []
    package_payload_paths = set()
    previous_package_payload_path = None
    for index, raw in enumerate(package_payload_raw):
        context = "public-final nxproject.package_payload[{}]".format(index)
        record = _public_final_exact_keys(
            raw, ("path", "mode", "sha256", "kind"), (), context
        )
        path = safe_relative(record.get("path"), context + ".path")
        if (previous_package_payload_path is not None and
                path <= previous_package_payload_path):
            fail("public-final package_payload is not ordered by path")
        folded = path.casefold()
        if folded in package_payload_paths:
            fail("public-final package_payload has a duplicate path")
        mode = record.get("mode")
        kind = record.get("kind")
        if mode not in ("0644", "0755") or kind not in (
                "payload", "license-notice"):
            fail(context + " kind/mode is invalid")
        if mode == "0755" and PurePosixPath(path).parts[0] != "tools":
            fail(context + " mode 0755 is allowed only below tools/")
        digest = record.get("sha256")
        if (not isinstance(digest, str) or
                not re.fullmatch(r"[0-9a-f]{64}", digest)):
            fail(context + ".sha256 must be lowercase SHA-256")
        target = config["port_dir"] + "/" + path
        packaged = config["inventory"].get(target)
        if (packaged is None or packaged.get("kind") != kind or
                packaged.get("mode") != int(mode, 8) or
                packaged.get("sha256") != digest):
            fail("public-final inventory differs from package_payload: " + path)
        package_payload_paths.add(folded)
        previous_package_payload_path = path
        package_payload.append({
            "path": path, "mode": mode, "sha256": digest, "kind": kind,
        })
    project_nxport = require_object(
        project.get("nxport"), "public-final nxproject.nxport"
    )
    if project_nxport.get("id") != config["package_id"]:
        fail("public-final nxproject id differs from the package")
    executable_name = safe_relative(
        project_nxport.get("executable"), "public-final nxproject executable"
    )
    if "/" in executable_name:
        fail("public-final executable must be directly inside package.port_dir")

    packaged_nxport = require_object(
        documents["nxport"], "public-final packaged nxport.json"
    )
    expected_nxport = copy.deepcopy(project_nxport)
    expected_required = expected_nxport.get("required_files")
    if not isinstance(expected_required, list) or not expected_required:
        fail("public-final nxproject.nxport.required_files is incomplete")
    if NXSPLASH_RUNTIME_NAME not in expected_required:
        expected_required.insert(1, NXSPLASH_RUNTIME_NAME)
    nested_schema = expected_nxport.get("schema_version")
    if nested_schema != 3:
        fail("embedded public-final requires nxport schema_version 3")
    project_runtime = expected_nxport.get("generation_runtime")
    packaged_runtime = packaged_nxport.get("generation_runtime")
    if (not isinstance(project_runtime, list) or not project_runtime or
            not isinstance(packaged_runtime, list) or not packaged_runtime):
        fail("public-final nxport schema 3 requires generation_runtime")
    if project_runtime != packaged_runtime:
        # nxbootstrap owns the canonical NXSplash and appends its exact
        # record when the source project omits it.  No other normalization
        # is accepted at this boundary.
        if (len(packaged_runtime) != len(project_runtime) + 1 or
                packaged_runtime[:-1] != project_runtime):
            fail("packaged generation_runtime differs from nxproject")
        splash = packaged_runtime[-1]
        expected_splash = config.get("nxsplash")
        if (not isinstance(splash, dict) or
                splash != {
                    "role": "nxsplash",
                    "path": NXSPLASH_RUNTIME_NAME,
                    "mode": "0755",
                    "sha256": (
                        expected_splash.get("sha256")
                        if isinstance(expected_splash, dict) else None
                    ),
                }):
            fail("nxbootstrap did not append the canonical nxsplash record")
        expected_nxport["generation_runtime"] = copy.deepcopy(
            packaged_runtime
        )
    if packaged_nxport != expected_nxport:
        fail("packaged nxport.json differs from nxproject.nxport")

    promotion = _public_final_exact_keys(
        project.get("promotion"), ("adapter_contract", "claims"), (),
        "public-final nxproject.promotion",
    )
    safe_relative(
        promotion.get("adapter_contract"),
        "public-final nxproject.promotion.adapter_contract",
    )
    promotion_claims = _public_final_exact_keys(
        promotion.get("claims"),
        ("release_ready", "physical_support_proven",
         "adapter_lifecycle_implemented"),
        (), "public-final nxproject.promotion.claims",
    )
    for key in promotion_claims:
        if promotion_claims[key] is not claims[key]:
            fail("nxproject promotion claim {} differs from GENERATION.json".format(
                key
            ))

    adapter = require_object(
        documents["adapter"], "public-final adapter-contract.json"
    )
    if (adapter.get("schema") != "nxadapter-skeleton-v1" or
            type(adapter.get("schema_version")) is not int or
            adapter.get("schema_version") != 1):
        fail("public-final adapter contract schema is unsupported")
    if adapter.get("status") == "unimplemented_nonrelease":
        fail("public-final rejects unimplemented_nonrelease")
    if adapter.get("status") != "implemented_release":
        fail("public-final requires adapter status implemented_release")
    if adapter.get("release_ready") is not True:
        fail("public-final rejects adapter release_ready=false")
    lifecycle = require_object(
        adapter.get("lifecycle"), "public-final adapter lifecycle"
    )
    sequence = lifecycle.get("sequence")
    evidence = lifecycle.get("source_evidence")
    if (not isinstance(sequence, list) or not sequence or
            any(not isinstance(item, str) or not item for item in sequence) or
            not isinstance(evidence, list) or not evidence or
            any(not isinstance(item, str) or not item for item in evidence)):
        fail("public-final adapter lifecycle sequence/evidence is incomplete")

    if adapter.get("language_access") != project.get("language_access"):
        fail("adapter language_access differs from nxproject")
    controls = require_object(
        project.get("controls"), "public-final nxproject.controls"
    )
    control_actions = controls.get("actions")
    control_contexts = controls.get("contexts")
    if (not isinstance(control_actions, list) or not control_actions or
            any(not isinstance(item, dict) for item in control_actions) or
            not isinstance(control_contexts, dict) or not control_contexts):
        fail("public-final nxproject controls are incomplete")
    adapter_input = require_object(
        adapter.get("input"), "public-final adapter input"
    )
    if (adapter_input.get("actions") != controls.get("actions") or
            adapter_input.get("contexts") != controls.get("contexts")):
        fail("adapter input actions/contexts differ from nxproject")
    runtime_mapping = controls.get("runtime_mapping")
    if runtime_mapping not in (None, "nxinput-gptk"):
        fail("public-final controls.runtime_mapping is unsupported")
    if adapter_input.get("runtime_mapping") != runtime_mapping:
        fail("adapter input runtime_mapping differs from nxproject")
    project_graphics = project.get("graphics")
    if isinstance(project_graphics, dict):
        project_graphics = dict(project_graphics)
        project_graphics.pop("adapter", None)
    adapter_graphics = adapter.get("graphics")
    if isinstance(adapter_graphics, dict):
        adapter_graphics = dict(adapter_graphics)
        adapter_graphics.pop("adapter", None)
    if adapter_graphics != project_graphics:
        fail("adapter graphics contract differs from nxproject")
    if adapter_graphics is not None:
        if (not isinstance(adapter_graphics, dict) or
                adapter_graphics.get("api") not in ("gles", "gl") or
                adapter_graphics.get("profile") not in ("es", "core", "compat") or
                adapter_graphics.get("version_policy") not in
                ("exact", "minimum", "range") or
                adapter_graphics.get("shader_dialect") not in
                ("essl100", "essl300", "essl310", "glsl-any") or
                _graphics_version_tuple(adapter_graphics.get("version")) is None):
            fail("public-final graphics contract is malformed")
        if adapter_graphics.get("version_policy") == "range":
            if _graphics_version_tuple(adapter_graphics.get("version_max")) is None:
                fail("public-final range graphics contract lacks version_max")
        elif adapter_graphics.get("version_max") is not None:
            fail("public-final non-range graphics contract declares version_max")
        required_devices = adapter_graphics.get("required_devices")
        if (not isinstance(required_devices, list) or not required_devices or
                any(not isinstance(item, str) or not item
                    for item in required_devices) or
                len(required_devices) != len(set(required_devices))):
            fail("public-final graphics contract needs unique required_devices")
    audio = require_object(adapter.get("audio"), "public-final adapter audio")
    callbacks = audio.get("callbacks")
    if (not isinstance(callbacks, list) or
            any(not isinstance(item, str) or not item for item in callbacks) or
            len(callbacks) != len(set(callbacks))):
        fail("public-final adapter audio callbacks are malformed")
    if ((audio.get("format") is None) != (not callbacks)):
        fail("public-final adapter audio format/callback declaration is incomplete")
    terminal = require_object(
        adapter.get("terminal"), "public-final adapter terminal"
    )
    if not isinstance(terminal.get("action"), str) or not terminal["action"]:
        fail("public-final adapter terminal action is incomplete")

    artifacts = _public_final_artifact_records(generation)
    for record in package_payload:
        target = config["port_dir"] + "/" + record["path"]
        artifact = artifacts.get(target)
        if (artifact is None or artifact.get("mode") != record["mode"] or
                artifact.get("sha256") != record["sha256"]):
            fail("GENERATION.json does not bind package_payload: " +
                 record["path"])
    runtime_schema = documents["runtime_manifest"].get("schema") \
        if isinstance(documents["runtime_manifest"], dict) else None
    if runtime_schema != "nxruntime-generation-v2":
        fail("embedded public-final requires one closed generation-v2 store")
    _public_final_generation_v2(
        documents, config, generation_id, packaged_nxport, artifacts
    )

    required_artifacts = {
        config["port_dir"] + "/nxproject.json": project_bytes,
        config["port_dir"] + "/adapter/adapter-contract.json":
            documents["adapter_bytes"],
        documents["runtime_member"]: documents["runtime_bytes"],
    }
    for path, payload in required_artifacts.items():
        record = artifacts.get(path)
        if record is None or record["mode"] != "0644" or \
                record["sha256"] != hashlib.sha256(payload).hexdigest():
            fail("GENERATION.json does not bind required artifact {}".format(path))

    executable_path = config["port_dir"] + "/" + executable_name
    executable_inventory = config["inventory"].get(executable_path)
    if (executable_inventory is None or
            executable_inventory.get("kind") != "project-linux" or
            executable_inventory.get("mode") != 0o755):
        fail("public-final executable is absent or not a 0755 project-linux ELF")
    return {
        "adapter": adapter,
        "claims": claims,
        "executable_path": executable_path,
        "generation_id": generation_id,
        "generation_mode": documents["generation_mode"],
        "generation_receipt_sha256": documents["generation_sha256"],
        "project": project,
    }


def _public_final_documents(archive_path, config):
    port_dir = config["port_dir"]
    generation_member = port_dir + "/GENERATION.json"
    project_member = port_dir + "/nxproject.json"
    adapter_member = port_dir + "/adapter/adapter-contract.json"
    nxport_member = port_dir + "/nxport.json"
    try:
        archive = zipfile.ZipFile(str(archive_path), "r")
    except (OSError, zipfile.BadZipFile) as exc:
        fail("cannot reopen public-final ZIP: {}".format(exc))
    with archive:
        names = set(archive.namelist())
        project_bytes = _public_final_archive_member(
            archive, project_member, "public-final nxproject.json"
        )
        adapter_bytes = _public_final_archive_member(
            archive, adapter_member, "public-final adapter contract"
        )
        nxport_bytes = _public_final_archive_member(
            archive, nxport_member, "public-final nxport.json"
        )
        if generation_member not in names:
            fail("public-final requires an embedded current GENERATION.json; legacy archives are quarantine-only")
        generation_bytes = _public_final_archive_member(
            archive, generation_member, "public-final GENERATION.json"
        )
        generation_mode = "embedded"
        inventory_item = config["inventory"].get(generation_member)
        if (inventory_item is None or inventory_item.get("kind") != "payload" or
                inventory_item.get("mode") != 0o644):
            fail("embedded public-final GENERATION.json must be a 0644 payload")
        generation = parse_json_strict(
            generation_bytes, "public-final generation receipt"
        )
        generation_id = generation.get("generation_id") \
            if isinstance(generation, dict) else None
        if not isinstance(generation_id, str) or not SHA256_RE.fullmatch(generation_id):
            fail("public-final generation_id must be a full SHA-256")
        runtime_member = "{}/.nxruntime/generations/{}/manifest.json".format(
            port_dir, generation_id
        )
        runtime_bytes = _public_final_archive_member(
            archive, runtime_member, "public-final runtime generation manifest"
        )
        runtime_manifest = parse_json_strict(
            runtime_bytes, "public-final runtime generation manifest"
        )
        launcher_bytes = _public_final_archive_member(
            archive, config["launcher"], "public-final launcher"
        )
        runtime_controls = {"manifest.json": runtime_bytes}
        if (isinstance(runtime_manifest, dict) and
                runtime_manifest.get("schema") == "nxruntime-generation-v2"):
            generation_root = "{}/.nxruntime/generations/{}".format(
                port_dir, generation_id
            )
            for name in (
                    "commit", "components.sha256", "components.v2", "format",
                    "identity-runtime.v2", "identity.json"):
                runtime_controls[name] = _public_final_archive_member(
                    archive, generation_root + "/" + name,
                    "public-final generation-v2 " + name,
                )
    documents = {
        "adapter": parse_json_strict(adapter_bytes, "public-final adapter contract"),
        "adapter_bytes": adapter_bytes,
        "generation": generation,
        "generation_mode": generation_mode,
        "generation_sha256": hashlib.sha256(generation_bytes).hexdigest(),
        "launcher_bytes": launcher_bytes,
        "nxport": parse_json_strict(nxport_bytes, "public-final nxport.json"),
        "nxport_bytes": nxport_bytes,
        "project": parse_json_strict(project_bytes, "public-final nxproject.json"),
        "project_bytes": project_bytes,
        "runtime_bytes": runtime_bytes,
        "runtime_controls": runtime_controls,
        "runtime_manifest": runtime_manifest,
        "runtime_member": runtime_member,
    }
    return documents


def _verify_schema3_generation_archive(archive_path, extracted_stage, config):
    """Authenticate the embedded generation-v2 closure during normal verify."""
    nxport_path = (Path(extracted_stage) / config["port_dir"] / "nxport.json")
    nxport = require_object(
        load_json(nxport_path, "schema-3 packaged nxport.json"),
        "schema-3 packaged nxport.json",
    )
    schema_version = nxport.get("schema_version", 1)
    if schema_version != 3:
        return

    documents = _public_final_documents(archive_path, config)
    if documents["generation_mode"] != "embedded":
        fail("nxport schema 3 requires embedded GENERATION.json")
    generation = require_object(
        documents["generation"], "schema-3 GENERATION.json"
    )
    generation_id = parse_sha256(
        generation.get("generation_id"),
        "schema-3 GENERATION.json generation_id",
    )
    runtime = require_object(
        documents["runtime_manifest"],
        "schema-3 runtime generation manifest",
    )
    if runtime.get("schema") != "nxruntime-generation-v2":
        fail("nxport schema 3 requires an embedded runtime generation-v2")
    artifacts = _public_final_artifact_records(generation)
    _public_final_generation_v2(
        documents, config, generation_id, nxport, artifacts
    )


def _public_final_elf_identity(config, executable_path):
    entries = config["elf_audit"].get("files")
    if not isinstance(entries, list):
        fail("public-final release metadata lacks ELF audit files")
    matches = [item for item in entries
               if isinstance(item, dict) and item.get("path") == executable_path]
    if len(matches) != 1:
        fail("public-final executable has no unique ELF audit identity")
    identity = matches[0]
    build_id = identity.get("build_id")
    if not isinstance(build_id, str) or not re.fullmatch(r"[0-9a-f]{8,128}", build_id):
        fail("public-final executable needs a real GNU build-id")
    return identity


def _public_final_symbols(value, context, defined_symbols):
    symbols = _public_final_string_list(value, context)
    missing = sorted(set(symbols) - set(defined_symbols))
    if missing:
        fail("{} names symbols absent from the final ELF: {}".format(
            context, ", ".join(missing)
        ))
    return symbols


def _public_final_evidence_hash(value, context):
    return parse_sha256(value, context)


def _public_final_validate_graphics(receipt, run, artifact, contract,
                                    defined_symbols, context):
    block = _public_final_exact_keys(
        receipt,
        ("contract_initialized", "evidence", "evidence_sha256", "frame_proof",
         "symbols"), (), context,
    )
    if block.get("contract_initialized") is not True:
        fail("{} did not initialize the declared graphics contract".format(context))
    symbols = _public_final_symbols(
        block.get("symbols"), context + ".symbols", defined_symbols
    )
    required_symbols = {
        "nxgl_graphics_contract_validate",
        "nxgl_graphics_contract_adapter_shader_probe",
        "nxgl_graphics_contract_evidence_receipt",
    }
    if not required_symbols.issubset(symbols):
        fail("{} lacks the canonical nxgl contract/probe/receipt symbols".format(
            context
        ))
    _public_final_evidence_hash(
        block.get("evidence_sha256"), context + ".evidence_sha256"
    )
    evidence = parse_graphics_evidence(block.get("evidence"))
    if evidence is None:
        fail("{} lacks a structured GRAPHICS-EVIDENCE line".format(context))
    if evidence.get("run_id") != run["run_id"]:
        fail("{} GRAPHICS-EVIDENCE run_id differs from the receipt".format(context))
    if evidence.get("generation") != artifact["generation_id"]:
        fail("{} GRAPHICS-EVIDENCE generation differs from the ZIP".format(context))
    if evidence.get("commit") != artifact["port_commit"]:
        fail("{} GRAPHICS-EVIDENCE commit differs from the source commit".format(
            context
        ))
    if (not evidence.get("build_id") or evidence.get("build_id") == "-" or
            evidence.get("verdict") != "OK" or evidence.get("reason") != "ok" or
            evidence.get("shader_probe") != "pass"):
        fail("{} graphics contract evidence is not a measured OK result".format(
            context
        ))
    if not (isinstance(evidence.get("drawable_w"), int) and
            isinstance(evidence.get("drawable_h"), int) and
            evidence["drawable_w"] > 1 and evidence["drawable_h"] > 1):
        fail("{} graphics evidence has an unusable 1x1 drawable".format(context))
    obtained = evidence.get("obtained") or {}
    if contract.get("api") == "gles" and obtained.get("api") != "gles":
        fail("{} accepted desktop GL for a GLES contract".format(context))
    if contract.get("profile") == "es" and obtained.get("profile") != "es":
        fail("{} obtained a non-ES profile".format(context))
    obtained_version = _graphics_version_tuple(obtained.get("version"))
    declared_version = _graphics_version_tuple(contract.get("version"))
    policy = contract.get("version_policy")
    if obtained_version is None or declared_version is None:
        fail("{} contains a malformed graphics version".format(context))
    if policy == "exact" and obtained_version != declared_version:
        fail("{} obtained graphics version differs from exact contract".format(context))
    if policy == "minimum" and obtained_version < declared_version:
        fail("{} obtained graphics version is below the contract".format(context))
    if policy == "range":
        maximum = _graphics_version_tuple(contract.get("version_max"))
        if maximum is None or not declared_version <= obtained_version <= maximum:
            fail("{} obtained graphics version is outside the contract".format(context))
    frame = _public_final_exact_keys(
        block.get("frame_proof"),
        ("verdict", "sample_count", "non_black_percent"), (),
        context + ".frame_proof",
    )
    verdict = frame.get("verdict")
    if verdict == "BLACK":
        fail("{} frame proof is conclusively BLACK".format(context))
    if verdict != "OK":
        fail("{} frame proof must be conclusively OK".format(context))
    samples = frame.get("sample_count")
    non_black = frame.get("non_black_percent")
    if (not isinstance(samples, int) or isinstance(samples, bool) or samples < 1 or
            not isinstance(non_black, (int, float)) or isinstance(non_black, bool) or
            non_black <= 0 or non_black > 100):
        fail("{} frame proof has no measured non-black pixels".format(context))


def _public_final_validate_receipts(receipts, archive_path, config, contract,
                                    source_commit):
    archive_sha = sha256_file(archive_path)
    archive_size = Path(archive_path).stat().st_size
    executable = _public_final_elf_identity(
        config, contract["executable_path"]
    )
    symbol_record = _DYNAMIC_SYMBOLS.get(contract["executable_path"])
    if symbol_record is None:
        fail("public-final has no audited symbol table for the executable")
    defined_symbols = symbol_record[1]
    if not receipts:
        fail("public-final requires at least one external physical receipt")
    summaries = []
    seen_devices = set()
    adapter = contract["adapter"]
    graphics_declared = isinstance(adapter.get("graphics"), dict) and \
        adapter["graphics"].get("uses_gl") is not False
    project_controls = (contract.get("project") or {}).get("controls") or {}
    input_declared = \
        project_controls.get("runtime_mapping") == "nxinput-gptk"
    audio_callbacks = (adapter.get("audio") or {}).get("callbacks") or []
    audio_declared = bool(audio_callbacks)
    terminal_declared = bool((adapter.get("terminal") or {}).get("action"))

    for receipt_path in receipts:
        path = Path(receipt_path)
        if path.is_symlink() or not path.is_file():
            fail("public-final receipt is missing, non-regular or a symlink: {}".format(
                path
            ))
        if path.stat().st_size > 16 * 1024 * 1024:
            fail("public-final receipt is unexpectedly large")
        payload = path.read_bytes()
        value = parse_json_strict(payload, "public-final physical receipt")
        _public_final_reject_private_document(value, "public-final physical receipt")
        value = _public_final_exact_keys(
            value,
            ("schema", "schema_version", "sanitized", "artifact", "run",
             "integrations"), (), "public-final physical receipt",
        )
        if (value.get("schema") != PUBLIC_FINAL_RECEIPT_SCHEMA or
                type(value.get("schema_version")) is not int or
                value.get("schema_version") != 1 or
                value.get("sanitized") is not True):
            fail("public-final physical receipt schema/sanitization is invalid")
        artifact = _public_final_exact_keys(
            value.get("artifact"),
            ("package_id", "package_version", "port_commit", "zip_sha256",
             "zip_size", "executable_path", "executable_sha256",
             "executable_build_id", "generation_id"), (),
            "public-final physical receipt artifact",
        )
        port_commit = require_string(
            artifact.get("port_commit"), "public-final receipt port_commit"
        )
        if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", port_commit):
            fail("public-final receipt port_commit is not a full Git object id")
        if (type(artifact.get("zip_size")) is not int or
                artifact.get("zip_size") < 1):
            fail("public-final receipt zip_size must be a positive integer")
        expected_artifact = {
            "package_id": config["package_id"],
            "package_version": config["package_version"],
            "port_commit": source_commit,
            "zip_sha256": archive_sha,
            "zip_size": archive_size,
            "executable_path": contract["executable_path"],
            "executable_sha256": executable["sha256"],
            "executable_build_id": executable["build_id"],
            "generation_id": contract["generation_id"],
        }
        if artifact != expected_artifact:
            fail("public-final receipt does not identify this exact ZIP/ELF/"
                 "generation/source commit")
        run = _public_final_exact_keys(
            value.get("run"), ("run_id", "device_id", "verdict"), (),
            "public-final physical receipt run",
        )
        _public_final_safe_identifier(run.get("run_id"), "public-final run_id")
        device = _public_final_safe_identifier(
            run.get("device_id"), "public-final device_id"
        )
        if run.get("verdict") != "PASS":
            fail("public-final physical receipt verdict is not PASS")
        if device in seen_devices:
            fail("public-final has duplicate physical receipts for {}".format(device))
        seen_devices.add(device)
        integrations = _public_final_exact_keys(
            value.get("integrations"),
            ("lifecycle", "graphics", "input", "audio", "terminal"), (),
            "public-final physical receipt integrations",
        )
        lifecycle = _public_final_exact_keys(
            integrations.get("lifecycle"),
            ("completed", "health_evidence", "symbols", "evidence_sha256"), (),
            "public-final lifecycle receipt",
        )
        if lifecycle.get("completed") is not True:
            fail("public-final lifecycle did not complete")
        expected_health = (
            "UPDATE NXU0006: generation {} proved healthy receipt_run={}".format(
                artifact["generation_id"], run["run_id"]
            )
        )
        if lifecycle.get("health_evidence") != expected_health:
            fail("public-final lifecycle lacks the exact run-bound NXU0006 "
                 "generation promotion")
        _public_final_symbols(
            lifecycle.get("symbols"), "public-final lifecycle symbols",
            defined_symbols,
        )
        _public_final_evidence_hash(
            lifecycle.get("evidence_sha256"),
            "public-final lifecycle evidence_sha256",
        )

        graphics = integrations.get("graphics")
        if graphics_declared:
            if graphics is None:
                fail("declared graphics integration has no physical receipt")
            _public_final_validate_graphics(
                graphics, run, artifact, adapter["graphics"], defined_symbols,
                "public-final graphics receipt",
            )
        elif graphics is not None:
            fail("physical receipt claims undeclared graphics integration")

        input_block = integrations.get("input")
        if input_declared:
            input_block = _public_final_exact_keys(
                input_block,
                ("gptk_loaded", "gptk_sha256", "parser", "dispatcher",
                 "delivery_count", "double_input", "ab_swap_observed",
                 "start_select_observed", "symbols", "sink_symbols",
                 "evidence_sha256"), (), "public-final input receipt",
            )
            if (input_block.get("gptk_loaded") is not True or
                    input_block.get("parser") != "nxinput_gptk" or
                    input_block.get("dispatcher") != "nxinput_gptk_dispatcher" or
                    type(input_block.get("delivery_count")) is not int or
                    input_block.get("delivery_count") != 1 or
                    input_block.get("double_input") is not False or
                    input_block.get("ab_swap_observed") is not True or
                    input_block.get("start_select_observed") is not True):
                fail("public-final input receipt does not prove GPTK -> sink, "
                     "A/B, chord and exactly-one delivery")
            parse_sha256(input_block.get("gptk_sha256"),
                         "public-final input gptk_sha256")
            input_symbols = _public_final_symbols(
                input_block.get("symbols"), "public-final input symbols",
                defined_symbols,
            )
            required_input = {
                "nxinput_gptk_load_at",
                "nxinput_gptk_load_receipt_json",
                "nxinput_gptk_parse",
                "nxinput_gptk_dispatcher_register",
                "nxinput_gptk_source_guard_init",
                "nxinput_gptk_dispatcher_set_primary_mask",
                "nxinput_gptk_dispatcher_feed_source",
            }
            if not required_input.issubset(input_symbols):
                fail("public-final input receipt lacks parser/dispatcher symbols")
            sink_symbols = _public_final_symbols(
                input_block.get("sink_symbols"),
                "public-final input sink_symbols", defined_symbols,
            )
            if any(symbol.startswith("nxinput_") for symbol in sink_symbols):
                fail("public-final input sink_symbols must be port-owned sinks")
            _public_final_evidence_hash(
                input_block.get("evidence_sha256"),
                "public-final input evidence_sha256",
            )
        elif input_block is not None:
            fail("physical receipt claims undeclared input integration")

        audio_block = integrations.get("audio")
        if audio_declared:
            audio_block = _public_final_exact_keys(
                audio_block,
                ("callback_alive", "recovery_state", "symbols",
                 "evidence_sha256"), (), "public-final audio receipt",
            )
            if (audio_block.get("callback_alive") is not True or
                    audio_block.get("recovery_state") not in
                    ("not-needed", "recovered")):
                fail("public-final audio receipt is not live/recovered")
            audio_symbols = _public_final_symbols(
                audio_block.get("symbols"), "public-final audio symbols",
                defined_symbols,
            )
            if not set(audio_callbacks).issubset(audio_symbols):
                fail("public-final audio receipt omits declared adapter callbacks")
            required_audio = {
                "nxaudio_receipt_format",
                "nxaudio_liveness_tick",
            }
            if not required_audio.issubset(audio_symbols):
                fail("public-final audio receipt lacks canonical receipt/liveness "
                     "symbols")
            if audio_block.get("recovery_state") == "recovered":
                required_recovery = {
                    "nxaudio_backend_recovery_run",
                    "nxaudio_backend_recovery_format",
                }
                if not required_recovery.issubset(audio_symbols):
                    fail("public-final recovered audio lacks canonical bounded "
                         "recovery symbols")
            _public_final_evidence_hash(
                audio_block.get("evidence_sha256"),
                "public-final audio evidence_sha256",
            )
        elif audio_block is not None:
            fail("physical receipt claims undeclared audio integration")

        terminal_block = integrations.get("terminal")
        if terminal_declared:
            terminal_block = _public_final_exact_keys(
                terminal_block,
                ("independent", "completed", "symbols", "evidence_sha256"),
                (), "public-final terminal receipt",
            )
            if (terminal_block.get("independent") is not True or
                    terminal_block.get("completed") is not True):
                fail("public-final terminal receipt does not prove independent exit")
            _public_final_symbols(
                terminal_block.get("symbols"), "public-final terminal symbols",
                defined_symbols,
            )
            _public_final_evidence_hash(
                terminal_block.get("evidence_sha256"),
                "public-final terminal evidence_sha256",
            )
        elif terminal_block is not None:
            fail("physical receipt claims undeclared terminal integration")

        summaries.append({
            "device_id": device,
            "receipt_sha256": hashlib.sha256(payload).hexdigest(),
            "run_id": run["run_id"],
        })

    required_devices = ((adapter.get("graphics") or {}).get("required_devices")
                        if graphics_declared else None)
    if required_devices:
        missing = sorted(set(required_devices) - seen_devices)
        if missing:
            fail("public-final receipts miss required device(s): {}".format(
                ", ".join(missing)
            ))
    return summaries


def _public_final_git_tree(path, label):
    original = Path(path)
    if original.is_symlink() or not original.is_dir():
        fail("{} source tree is missing, non-directory or a symlink".format(label))
    root = original.resolve()
    process = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--show-toplevel"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, universal_newlines=True,
    )
    if process.returncode != 0:
        fail("{} source tree is not a Git worktree".format(label))
    if Path(process.stdout.strip()).resolve() != root:
        fail("{} must name the Git worktree root exactly".format(label))
    status = subprocess.run(
        ["git", "-C", str(root), "status", "--porcelain=v1",
         "--untracked-files=all"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, universal_newlines=True,
    )
    if status.returncode != 0:
        fail("cannot inspect {} source tree cleanliness".format(label))
    if status.stdout:
        fail("{} source tree is not clean".format(label))
    ignored = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--others",
         "--ignored", "--exclude-standard"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if ignored.returncode != 0:
        fail("cannot inspect {} ignored build outputs".format(label))
    if ignored.stdout:
        fail("{} source tree contains ignored/untracked build output".format(
            label
        ))
    commit_process = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, universal_newlines=True,
    )
    if commit_process.returncode != 0:
        fail("cannot resolve {} source commit".format(label))
    commit = commit_process.stdout.strip()
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit):
        fail("{} source commit is not a full object id".format(label))
    return {"root": root, "commit": commit}


def _public_final_manifest(tree, relative, label):
    relative = safe_relative(relative, label + " manifest")
    candidate = tree["root"] / PurePosixPath(relative)
    ensure_no_symlink(candidate, tree["root"], label + " manifest")
    resolved = candidate.resolve()
    if not source_is_within(tree["root"], resolved) or not resolved.is_file():
        fail("{} manifest is absent or escapes its worktree".format(label))
    return resolved, relative


def _public_final_run_port_build(tree, build_script_relative, output_dir,
                                 source_date_epoch, label):
    relative = safe_relative(build_script_relative, label + " build script")
    candidate = tree["root"] / PurePosixPath(relative)
    ensure_no_symlink(candidate, tree["root"], label + " build script")
    script = candidate.resolve()
    if (not source_is_within(tree["root"], script) or not script.is_file() or
            not os.access(str(script), os.X_OK)):
        fail("{} build script is absent, escapes the worktree or is not executable".format(
            label
        ))
    environment = dict(os.environ)
    environment["NX_PUBLIC_FINAL_REPRO_BUILD"] = "1"
    environment["NX_PUBLIC_FINAL_OUTPUT_DIR"] = str(output_dir)
    environment["SOURCE_DATE_EPOCH"] = str(source_date_epoch)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["LC_ALL"] = "C"
    environment["TZ"] = "UTC"
    process = subprocess.run(
        [str(script)], cwd=str(script.parent), env=environment,
        stdin=subprocess.DEVNULL,
    )
    if process.returncode != 0:
        fail("{} port build script failed with status {}".format(
            label, process.returncode
        ))
    return relative


def _public_final_prebuild_manifest(path, label):
    """Read only the fields needed to run a clean external project build.

    The complete manifest intentionally cannot be loaded yet: a clean checkout
    may not contain ignored/prebuilt project ELFs.  This preflight has a very
    small authority surface (source root, epoch and project-linux output
    names); every other field and every resulting byte is validated later by
    ``load_manifest`` with one-for-one external overrides.
    """
    manifest_path = Path(path).resolve()
    try:
        payload = manifest_path.read_bytes()
    except OSError as exc:
        fail("cannot read {} manifest {}: {}".format(label, manifest_path, exc))
    if len(payload) > 16 * 1024 * 1024:
        fail("{} manifest is unexpectedly large".format(label))
    document = require_object(
        parse_json_strict(payload, label + " prebuild manifest"),
        label + " prebuild manifest",
    )
    require_keys(
        document,
        ("schema_version", "source_root", "package", "release", "nxextract",
         "portmaster_metadata", "dependencies", "files", "exceptions",
         "sdl3_exception"),
        label + " prebuild manifest",
    )
    if document.get("schema_version") != SCHEMA_VERSION:
        fail("{} prebuild manifest schema_version is unsupported".format(label))
    source_root_value = safe_relative(
        document.get("source_root", "."), label + " prebuild source_root",
        allow_dot=True,
    )
    source_root_candidate = manifest_path.parent / source_root_value
    if source_root_candidate.is_symlink():
        fail("{} prebuild source_root cannot be a symlink".format(label))
    source_root = source_root_candidate.resolve()
    if not source_root.is_dir():
        fail("{} prebuild source_root is missing".format(label))
    release = require_object(
        document.get("release"), label + " prebuild release"
    )
    require_keys(
        release, ("source_date_epoch", "max_glibc", "compression"),
        label + " prebuild release",
    )
    epoch = release.get("source_date_epoch")
    if isinstance(epoch, bool) or not isinstance(epoch, int) or \
            epoch < 315532800 or epoch > 4354819198:
        fail("{} prebuild source_date_epoch is invalid".format(label))
    entries = document.get("files")
    if not isinstance(entries, list) or not entries:
        fail("{} prebuild files must be a non-empty array".format(label))
    expected = set()
    for index, raw_entry in enumerate(entries):
        context = "{} prebuild files[{}]".format(label, index)
        entry = require_object(raw_entry, context)
        require_keys(
            entry,
            ("source", "target", "kind", "mode", "sha256", "architecture",
             "build_profile", "provenance", "needed", "soname"),
            context,
        )
        if entry.get("kind") != "project-linux":
            continue
        relative = safe_relative(
            entry.get("source"), context + ".source", allow_dot=True
        )
        if relative in expected:
            fail("{} has duplicate project-linux build output {}".format(
                label, relative
            ))
        expected.add(relative)
    if not expected:
        fail("{} manifest declares no project-linux ELF to rebuild".format(label))
    return {
        "epoch": epoch,
        "expected": expected,
        "source_root": source_root,
    }


def _public_final_collect_build_outputs(preflight, output_dir, label):
    """Return exact external project-linux overrides after a clean build."""
    expected = set(preflight["expected"])
    actual = set()
    for path in output_dir.rglob("*"):
        relative = path.relative_to(output_dir).as_posix()
        if path.is_symlink():
            fail("{} external build output contains symlink {}".format(
                label, relative
            ))
        if path.is_file():
            actual.add(relative)
        elif not path.is_dir():
            fail("{} external build output contains non-regular {}".format(
                label, relative
            ))
    if actual != expected:
        fail("{} NX_PUBLIC_FINAL_OUTPUT_DIR differs from project-linux sources "
             "(missing={}, extra={})".format(
                 label, sorted(expected - actual), sorted(actual - expected),
             ))
    overrides = {}
    for relative in sorted(expected):
        artifact = output_dir / PurePosixPath(relative)
        ensure_no_symlink(artifact, output_dir, label + " external build output")
        if not artifact.is_file() or not is_elf(artifact):
            fail("{} external build output is not an ELF: {}".format(
                label, relative
            ))
        overrides[relative] = artifact.resolve()
    return overrides


def _public_final_build(tree, manifest_path, build_script_relative, output,
                        requested_ceiling, label,
                        trusted_candidate_lock=None):
    output = Path(output)
    if output.suffix.lower() != ".zip":
        fail("{} output must end in .zip".format(label))
    if output.exists() or output.is_symlink() or \
            Path(str(output) + ".sha256").exists() or \
            Path(str(output) + ".sha256").is_symlink():
        fail("{} output or checksum already exists".format(label))
    output_parent = output.parent.resolve()
    if not output_parent.is_dir():
        fail("{} output parent does not exist".format(label))
    resolved_output = output_parent / output.name
    if source_is_within(tree["root"], resolved_output):
        fail("{} output must live outside the clean source worktree".format(label))
    preflight = _public_final_prebuild_manifest(manifest_path, label)
    if not source_is_within(tree["root"], preflight["source_root"]):
        fail("{} manifest source_root escapes the clean source worktree".format(label))
    temporary = Path(tempfile.mkdtemp(prefix="nxrelease-public-final-{}-".format(
        label.lower().replace(" ", "-")
    )))
    try:
        external_outputs = temporary / "artifacts"
        external_outputs.mkdir(mode=0o700)
        _public_final_run_port_build(
            tree, build_script_relative, external_outputs,
            preflight["epoch"], label
        )
        after_build = _public_final_git_tree(tree["root"], label)
        if after_build["commit"] != tree["commit"]:
            fail("{} changed commit during port build".format(label))
        overrides = _public_final_collect_build_outputs(
            preflight, external_outputs, label
        )
        config = load_manifest(
            manifest_path, requested_ceiling,
            project_linux_overrides=overrides,
            trusted_candidate_lock=trusted_candidate_lock,
        )
        if config["source_root"] != preflight["source_root"]:
            fail("{} manifest source_root changed during build".format(label))
        project_outputs = sorted(overrides)
        validate_sources(config)
        stage = temporary / "stage"
        destination = stage_release(config, stage)
        result = create_archive(destination, resolved_output)
    finally:
        shutil.rmtree(str(temporary), ignore_errors=True)
    # A build that writes generated state back into a source tree is not clean.
    after = _public_final_git_tree(tree["root"], label)
    if after["commit"] != tree["commit"]:
        fail("{} changed commit during build".format(label))
    return result, result["config"], resolved_output, project_outputs


def _public_final_config_fingerprint(config):
    inventory = [
        {"path": path, "kind": item["kind"],
         "mode": "{:04o}".format(item["mode"]), "sha256": item["sha256"]}
        for path, item in sorted(config["inventory"].items())
    ]
    elfs = sorted(config["elf_audit"].get("files") or [],
                  key=lambda item: item.get("path", ""))
    return {"inventory": inventory, "elfs": elfs}


def _public_final_write_provenance(output, value):
    output = Path(output)
    if output.name != "BUILD-PROVENANCE.json":
        fail("public-final provenance output must be named BUILD-PROVENANCE.json")
    if output.exists() or output.is_symlink():
        fail("public-final provenance output already exists")
    parent = output.parent.resolve()
    if not parent.is_dir():
        fail("public-final provenance parent does not exist")
    _public_final_reject_private_document(value, "BUILD-PROVENANCE.json")
    payload = json_bytes(value)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".nxrelease-provenance-", suffix=".json", dir=str(parent)
    )
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o644)
        stream = os.fdopen(descriptor, "wb")
        descriptor = None
        with stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        rename_noreplace(temporary, parent / output.name)
        temporary = None
        fsync_directory(parent)
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if temporary is not None and temporary.exists():
            temporary.unlink()


def _public_final_external_candidate_lock(path, source_root, tested_config):
    """Require the original frozen authority, not its metadata projection."""
    external = load_candidate_lock(path, source_root)
    carried = normalize_candidate_lock(
        tested_config.get("candidate_lock"),
        "tested archive candidate lock projection",
    )
    if external != carried:
        fail("public-final external candidate lock differs from the tested archive projection")
    if not tested_config.get("candidate_lock_verified"):
        fail("public-final tested archive did not verify its candidate lock")
    return external


def command_preflight(arguments):
    """V4-05A: delegate to the standalone read-only aggregate boundary."""
    import importlib.util as _ilu
    spec = _ilu.spec_from_file_location(
        "nxrelease_preflight", str(Path(__file__).resolve().parent /
                                   "nxpreflight.py"))
    module = _ilu.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.main([
        "--profile", arguments.profile,
        "--source", arguments.source,
        "--project", arguments.project,
        "--inputs-manifest", arguments.inputs_manifest,
    ] + (["--out", arguments.out] if arguments.out else [])
      + (["--json"] if arguments.json else []))


def command_public_final(arguments):
    """Promote exact tested bytes after one clean source compilation."""
    if (getattr(arguments, "generation_receipt", None) is not None or
            getattr(arguments, "allow_external_generation", False)):
        fail("public-final rejects legacy/external generation; historical bytes belong only in quarantine")
    # V4-05A: the aggregate preflight receipt guards the WHOLE promotion
    # path. It must be PASS, profile PUBLIC-FINAL and bound to the exact
    # commit/tree of --source, BEFORE any other input is opened or any
    # mutation happens. A DEV, FAIL, stale or malformed receipt refuses here.
    import importlib.util as _ilu
    _spec = _ilu.spec_from_file_location(
        "nxrelease_preflight_guard", str(Path(__file__).resolve().parent /
                                         "nxpreflight.py"))
    _module = _ilu.module_from_spec(_spec)
    _spec.loader.exec_module(_module)
    try:
        _module.require_preflight_receipt(
            getattr(arguments, "preflight_receipt", None),
            Path(arguments.source).resolve())
    except _module.PreflightUsage as error:
        fail(str(error))
    archive_input = Path(arguments.archive)
    if archive_input.is_symlink() or not archive_input.is_file():
        fail("public-final tested archive is missing, non-regular or a symlink")
    archive = archive_input.resolve()
    tree = _public_final_git_tree(arguments.source, "public-final build")
    build_target = Path(arguments.build).parent.resolve() / \
        Path(arguments.build).name
    provenance_target = Path(arguments.build_provenance).parent.resolve() / \
        Path(arguments.build_provenance).name
    if (archive == build_target or provenance_target in
            (archive, build_target)):
        fail("public-final tested/build/provenance outputs must be distinct")
    manifest, manifest_relative = _public_final_manifest(
        tree, arguments.manifest, "public-final build"
    )

    _DYNAMIC_SYMBOLS.clear()
    tested = verify_archive(archive, requested_ceiling=arguments.max_glibc)
    tested_config = tested["config"]
    external_candidate_lock = _public_final_external_candidate_lock(
        arguments.candidate_lock, tree["root"], tested_config
    )
    tested_fingerprint = _public_final_config_fingerprint(tested_config)
    documents = _public_final_documents(archive, tested_config)
    generation_contract = _public_final_generation_contract(
        documents, tested_config
    )
    receipts = _public_final_validate_receipts(
        arguments.receipt, archive, tested_config, generation_contract,
        tree["commit"],
    )

    rebuilt, rebuilt_config, rebuilt_archive, outputs = _public_final_build(
        tree, manifest, arguments.build_script, arguments.build,
        arguments.max_glibc, "public-final build",
        trusted_candidate_lock=external_candidate_lock,
    )
    hashes = [sha256_file(path) for path in (archive, rebuilt_archive)]
    sizes = [path.stat().st_size for path in (archive, rebuilt_archive)]
    if len(set(hashes)) != 1 or len(set(sizes)) != 1:
        fail("public-final exact-byte check failed: tested and rebuilt ZIP differ")
    rebuilt_fingerprint = _public_final_config_fingerprint(rebuilt_config)
    if tested_fingerprint != rebuilt_fingerprint:
        fail("public-final inventory/ELF comparison differs from tested bytes")
    if (rebuilt["package_id"] != tested["package_id"] or
            rebuilt["package_version"] != tested["package_version"]):
        fail("public-final rebuild produced a different package identity")

    provenance = {
        "schema": PUBLIC_FINAL_PROVENANCE_SCHEMA,
        "schema_version": 1,
        "sanitized": True,
        "tool": {"name": "nxrelease", "version": TOOL_VERSION},
        "release": {
            "package_id": tested["package_id"],
            "package_version": tested["package_version"],
            "port_commit": tree["commit"],
            "zip_sha256": hashes[0],
            "zip_size": sizes[0],
            "generation_id": generation_contract["generation_id"],
            "generation_mode": generation_contract["generation_mode"],
            "generation_receipt_sha256":
                generation_contract["generation_receipt_sha256"],
            "candidate_document_sha256":
                external_candidate_lock["document_sha256"],
            "candidate_executable": external_candidate_lock["executable"],
            "candidate_executable_sha256": external_candidate_lock["sha256"],
        },
        "sources": [
            {"label": "source", "commit": tree["commit"], "clean": True,
             "manifest": manifest_relative,
             "manifest_sha256": sha256_file(manifest),
             "build_script": arguments.build_script},
        ],
        "builds": [
            {"label": "tested", "zip_sha256": hashes[0],
             "zip_size": sizes[0]},
            {"label": "rebuilt", "zip_sha256": hashes[1],
             "zip_size": sizes[1]},
        ],
        "comparison": {
            "archives_identical": True,
            "inventories_identical": True,
            "elfs_identical": True,
            "inventory": tested_fingerprint["inventory"],
            "elfs": tested_fingerprint["elfs"],
            "external_project_outputs": outputs,
        },
        "physical_receipts": receipts,
        "verdict": "PUBLIC-FINAL-PASS",
    }
    _public_final_write_provenance(arguments.build_provenance, provenance)
    print("NXRELEASE PUBLIC-FINAL: PASS package={} version={} sha256={} "
          "generation={} commit={} receipts={} provenance={}".format(
              tested["package_id"], tested["package_version"], hashes[0],
              generation_contract["generation_id"], tree["commit"],
              len(receipts), Path(arguments.build_provenance).resolve(),
          ))


def command_validate(arguments):
    config = load_manifest(
        arguments.manifest, arguments.max_glibc,
        candidate_lock_path=arguments.candidate_lock,
        release_authority=arguments.authority,
    )
    # The single SDL symbol authority is part of the read-only preflight: a
    # missing/tampered table fails validate before any stage or ZIP exists,
    # and the receipt names the exact bytes both consumers decide from.
    sdl_authority = load_sdl_symbol_authority()
    elfs, maximum = validate_sources(config)
    print("NXRELEASE VALIDATE: PASS package={} version={} files={} elfs={} "
          "max_glibc={} ceiling={} sdl_floor={} sdl_authority={} "
          "sdl_authority_sha256={}".format(
              config["package_id"], config["package_version"],
              len(config["records"]), len(elfs), maximum, config["ceiling"],
              SDL_PUBLIC_FLOOR, sdl_authority["authority"],
              sdl_authority["sha256"]
          ))


def command_stage(arguments):
    config = load_manifest(
        arguments.manifest, arguments.max_glibc,
        candidate_lock_path=arguments.candidate_lock,
        release_authority=arguments.authority,
    )
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


def parse_display_receipt(line):
    """Parse the one-line DISPLAY receipt emitted by nxgl_display_receipt.

    A port that declares `remap_input` is claiming it moved the player's touch
    into a content rect. That claim is only believable with the receipt the
    runtime actually printed: a hand-written policy field proves nothing about
    what the adapter installed.
    """
    prefix = "DISPLAY:"
    if not isinstance(line, str) or not line.startswith(prefix):
        return None
    fields = {}
    for token in line[len(prefix):].strip().split():
        if "=" not in token:
            return None
        key, value = token.split("=", 1)
        if key in fields:
            return None
        fields[key] = value
    required = ("policy", "internal", "drawable", "content")
    if any(key not in fields for key in required):
        return None
    internal = re.fullmatch(r"([0-9]+)x([0-9]+)", fields["internal"] or "")
    drawable = re.fullmatch(r"([0-9]+)x([0-9]+)", fields["drawable"] or "")
    content = re.fullmatch(r"(-?[0-9]+),(-?[0-9]+)\+([0-9]+)x([0-9]+)",
                           fields["content"] or "")
    if not internal or not drawable or not content:
        return None
    return {
        "policy": fields["policy"],
        "internal": (int(internal.group(1)), int(internal.group(2))),
        "drawable": (int(drawable.group(1)), int(drawable.group(2))),
        "content": tuple(int(content.group(index)) for index in range(1, 5)),
        "noop": fields.get("noop"),
    }



GPTK_CONTROLS_V2 = (
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
    "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
    "LEFT_STICK", "RIGHT_STICK",
)
GPTK_TUNING_KEYS = frozenset((
    "speed", "deadzone", "response_curve", "acceleration", "smoothing_ms",
    "sensitivity_x", "sensitivity_y", "invert_x", "invert_y", "authority",
))


def _gptk_sections(text):
    """{section: [(key, value), ...]} in file order. Comments and blanks are
    dropped; the magic and `port` line live outside any section."""
    sections = {}
    order = []
    current = None
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
            if current not in sections:
                sections[current] = []
                order.append(current)
            continue
        if "=" in line and current is not None:
            key, _, value = line.partition("=")
            sections[current].append((key.strip(), value.strip()))
    return sections, order


GPTK4_DISCRETE = ("A", "B", "X", "Y", "L1", "R1", "L3", "R3", "START",
                  "SELECT", "GUIDE", "UP", "DOWN", "LEFT", "RIGHT")
GPTK4_HEADER = ("CONTROL_STANDARD", "GLYPH_STYLE", "AUTHORITY", "CONTEXT_POLICY")
GPTK4_OVERRIDE_KEYS = GPTK4_DISCRETE + (
    "stick.left.vector", "stick.right.vector",
    "stick.left.up", "stick.left.down", "stick.left.left", "stick.left.right",
    "stick.right.up", "stick.right.down", "stick.right.left", "stick.right.right",
    "trigger.left.analog", "trigger.right.analog",
    "trigger.left.digital", "trigger.right.digital")
GPTK4_BINDING_RE = re.compile(
    r"^(native|null|action:[a-z0-9][a-z0-9._-]*(@key:[A-Z0-9_+]+)?)$")


def _validate_gptk4_closure(text, adapter_input):
    """NEXTOS_CONTROLLERS/4 default: one [base] with every core control
    explicit, sparse overrides, both sticks and both triggers, typed
    bindings whose actions the adapter contract declares with sinks, no
    FACE_LAYOUT, no [cursor]/[camera] tuning (that is adapter data)."""
    sections, order = _gptk_sections(text)
    header = {}
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith("["):
            if line.startswith("["):
                break
            continue
        if "=" in line:
            key, value = (part.strip() for part in line.split("=", 1))
            header[key] = value
    for key in GPTK4_HEADER:
        if key not in header:
            fail("NEXTOS_CONTROLLERS/4 default lacks the header key %s" % key)
    if header.get("CONTROL_STANDARD") != "xbox":
        fail("NEXTOS_CONTROLLERS/4 CONTROL_STANDARD must be xbox (positional)")
    if header.get("AUTHORITY") not in ("nextos", "synchronized"):
        fail("NEXTOS_CONTROLLERS/4 shipped owner default must be nextos or "
             "synchronized (engine mode has no editable owner file)")
    if "FACE_LAYOUT" in header:
        fail("FACE_LAYOUT is a schema-3 field; schema 4 is positional")
    sinks_by_action = {}
    for action in adapter_input.get("actions") or []:
        if isinstance(action, dict) and isinstance(action.get("id"), str):
            sinks = action.get("sinks")
            if isinstance(sinks, list) and sinks:
                sinks_by_action[action["id"]] = sinks
    if not sinks_by_action:
        fail("a NEXTOS_CONTROLLERS/4 default requires an adapter contract "
             "that declares input.actions with sinks")
    required = {"base", "stick.left", "stick.right", "trigger.left", "trigger.right"}
    missing = sorted(required - set(sections))
    if missing:
        fail("NEXTOS_CONTROLLERS/4 default omits section(s) %s" % ", ".join(missing))
    for name in sections:
        if name in ("cursor", "camera", "menu", "gameplay"):
            fail("NEXTOS_CONTROLLERS/4 default carries a V3 section [%s]" % name)
        if not (name in required or name.startswith("override.") or
                name.startswith("keyboard.")):
            fail("NEXTOS_CONTROLLERS/4 default has an unknown section [%s]" % name)

    def check_binding(section, key, value):
        if not GPTK4_BINDING_RE.match(value):
            fail("NEXTOS_CONTROLLERS/4 [%s] %s = %r is not a typed binding"
                 % (section, key, value))
        if value.startswith("action:"):
            action_id = value[len("action:"):].split("@", 1)[0]
            if action_id not in sinks_by_action:
                fail("NEXTOS_CONTROLLERS/4 [%s] binds %s to %r, which the "
                     "adapter contract does not declare with a sink"
                     % (section, key, action_id))
    base = sections["base"]
    seen = set()
    for key, value in base:
        if key not in GPTK4_DISCRETE and not key.startswith("EXT."):
            fail("NEXTOS_CONTROLLERS/4 [base] names an unknown control %r" % key)
        if key in seen:
            fail("NEXTOS_CONTROLLERS/4 [base] repeats %r" % key)
        seen.add(key)
        check_binding("base", key, value)
    omitted = sorted(set(GPTK4_DISCRETE) - seen)
    if omitted:
        fail("NEXTOS_CONTROLLERS/4 [base] omits %s (omission is never a hidden "
             "null)" % ", ".join(omitted))
    for name, entries in sections.items():
        if name.startswith("override."):
            if not entries:
                fail("NEXTOS_CONTROLLERS/4 [%s] is empty" % name)
            seen = set()
            for key, value in entries:
                if key not in GPTK4_OVERRIDE_KEYS:
                    fail("NEXTOS_CONTROLLERS/4 [%s] names %r" % (name, key))
                if key in seen:
                    fail("NEXTOS_CONTROLLERS/4 [%s] repeats %r" % (name, key))
                seen.add(key)
                check_binding(name, key, value)
            if (set(GPTK4_DISCRETE) - {"GUIDE"}) <= seen:
                fail("NEXTOS_CONTROLLERS/4 [%s] duplicates the whole base "
                     "(overrides must be sparse)" % name)
    for name in ("stick.left", "stick.right"):
        entries = dict(sections[name])
        if entries.get("mode") not in ("vector", "digital", "split"):
            fail("NEXTOS_CONTROLLERS/4 [%s] mode must be vector|digital|split" % name)
        for key in ("vector", "up", "down", "left", "right"):
            if key not in entries:
                fail("NEXTOS_CONTROLLERS/4 [%s] omits %s" % (name, key))
            check_binding(name, key, entries[key])
        if entries["mode"] == "vector" and any(
                entries[d] != "null" for d in ("up", "down", "left", "right")):
            fail("NEXTOS_CONTROLLERS/4 [%s] vector mode requires null directions" % name)
    for name in ("trigger.left", "trigger.right"):
        entries = dict(sections[name])
        if entries.get("mode") not in ("analog", "digital"):
            fail("NEXTOS_CONTROLLERS/4 [%s] mode must be analog|digital" % name)
        for key in ("analog", "digital"):
            if key not in entries:
                fail("NEXTOS_CONTROLLERS/4 [%s] omits %s" % (name, key))
            check_binding(name, key, entries[key])
        if entries["mode"] == "analog" and entries["digital"] != "null":
            fail("NEXTOS_CONTROLLERS/4 [%s] analog mode requires digital = null" % name)
        if entries["mode"] == "digital" and entries["analog"] != "null":
            fail("NEXTOS_CONTROLLERS/4 [%s] digital mode requires analog = null" % name)
    del order


def _validate_gptk_closure(text, schema, adapter_input):
    if schema == 4:
        _validate_gptk4_closure(text, adapter_input)
        return
    """V4-CONTROLLERS-03 / C4.

    Every action the shipped default binds must exist in the adapter contract
    WITH at least one real sink -- a mapping that points at an action nothing
    consumes is a dead control the owner cannot tell from a working one. And
    every control appears exactly once per section, so no duplicate can make
    the order decide.

    In schema 2 the section must also be COMPLETE: all 18 controls, so the
    owner sees the whole pad. Schema 1 keeps its old, looser contract.
    """
    sections, _order = _gptk_sections(text)
    sinks_by_action = {}
    documented_shape = True
    for action in adapter_input.get("actions") or []:
        # Older/legacy contracts carry shapes this gate was never meant to
        # police. They are not closable, but they are not an error here
        # either -- the surrounding validation already accepts them.
        if not isinstance(action, dict) or not isinstance(
                action.get("id"), str) or not action.get("id"):
            documented_shape = False
            continue
        action_id = action["id"]
        sinks = action.get("sinks")
        # An action written in the DOCUMENTED shape but with no usable sink is
        # a real defect: a control bound to it would be dead.
        if not isinstance(sinks, list) or not sinks or not all(
                isinstance(sink, str) and sink for sink in sinks):
            fail("adapter-contract action {!r} declares no usable sink"
                 .format(action_id))
        sinks_by_action[action_id] = sinks

    # A SCAFFOLD contract declares no actions at all (release_ready False, or
    # a legacy schema-1/2 generation shipping the generic compatibility
    # default). There is no allowlist to close against, so the ACTION check
    # does not apply -- the STRUCTURAL rules below still do. A v2 file in that
    # position is a contradiction: v2 is opt-in on a schema-3 port, which
    # always declares its actions in the documented shape.
    closable = bool(sinks_by_action) and documented_shape
    if schema in (2, 3) and not closable:
        fail("a NEXTOS_CONTROLLERS/{} default requires an adapter contract "
             "that declares input.actions with sinks".format(schema))

    for name, entries in sections.items():
        if name == "camera":
            continue
        controls = [key for key, _ in entries if key not in GPTK_TUNING_KEYS]
        seen = set()
        for control in controls:
            if control not in GPTK_CONTROLS_V2:
                fail("defaults/NEXTOSCONTROLLERS.gptk section [{}] names an "
                     "unknown control {!r}".format(name, control))
            if control in seen:
                fail("defaults/NEXTOSCONTROLLERS.gptk section [{}] repeats "
                     "control {!r}".format(name, control))
            seen.add(control)
        if schema in (2, 3) and seen != set(GPTK_CONTROLS_V2):
            # Schema 3 inherits the V2 completeness verbatim: the owner sees
            # the whole pad, FACE_LAYOUT adds a preamble, never a control.
            missing = sorted(set(GPTK_CONTROLS_V2) - seen)
            fail("NEXTOS_CONTROLLERS/{} section [{}] omits {}"
                 .format(schema, name, ", ".join(missing)))
        for control, value in entries:
            if control in GPTK_TUNING_KEYS:
                continue
            if value in ("null", "native"):
                # The explicit tri-state belongs to schemas 2 AND 3; only a
                # schema-1 file has no words for "governed nothing".
                if schema not in (2, 3):
                    fail("defaults/NEXTOSCONTROLLERS.gptk uses {!r} but "
                         "declares NEXTOS_CONTROLLERS/1".format(value))
                continue
            if closable and value not in sinks_by_action:
                fail("defaults/NEXTOSCONTROLLERS.gptk binds [{}] {} to {!r}, "
                     "which the adapter contract does not declare with a sink"
                     .format(name, control, value))

def _validate_v4_optins(adapter_contract, config, records=None):
    """Shape and package consequences of the V4 declarative opt-ins.

    ``records`` are the location-resolved records (with ``actual_path``) of
    the boundary under audit; the controller-profiles closure reads the
    bundle's bytes through them."""
    display = adapter_contract.get("display")
    if display is not None:
        if not isinstance(display, dict):
            fail("adapter-contract display must be an object")
        policy = display.get("policy")
        if policy not in ("game", "preserve", "adaptive", "fill", "stretch"):
            fail("adapter-contract display.policy is invalid")
        if not isinstance(display.get("remap_input", False), bool):
            fail("adapter-contract display.remap_input must be a boolean")
        if policy == "game" and set(display) - {"policy", "remap_input"}:
            fail("adapter-contract display policy game declares presentation "
                 "fields")
        if policy != "game":
            for field in ("internal_width", "internal_height"):
                value = display.get(field)
                if (not isinstance(value, int) or isinstance(value, bool) or
                        value < 1 or value > 65535):
                    fail("adapter-contract display.%s is invalid" % field)
        # V4-DISPLAY-01: declaring an input remap is a claim about what the
        # player's finger does. It requires the receipt the runtime actually
        # printed, bound to the declared policy and internal size -- otherwise
        # a port could claim a remapped content rect it never installed.
        if display.get("remap_input") and policy != "game":
            proofs = adapter_contract.get("display_proofs")
            if not isinstance(proofs, list) or not proofs:
                fail("a port declaring display.remap_input must carry "
                     "display_proofs with the content-rect receipt")
            seen_devices = set()
            for proof in proofs:
                if not isinstance(proof, dict):
                    fail("display_proofs entry must be an object")
                device = proof.get("device")
                if not device or not isinstance(device, str):
                    fail("a display proof lacks a device identity")
                if device in seen_devices:
                    fail("display_proofs has a duplicate proof for %r" % device)
                seen_devices.add(device)
                receipt = parse_display_receipt(proof.get("evidence"))
                if receipt is None:
                    fail("display proof on %r lacks a well-formed DISPLAY "
                         "receipt" % device)
                if receipt["policy"] != policy:
                    fail("display proof on %r reports policy %r but the port "
                         "declares %r"
                         % (device, receipt["policy"], policy))
                if receipt["internal"] != (display["internal_width"],
                                           display["internal_height"]):
                    fail("display proof on %r reports a different internal "
                         "resolution than the port declares" % device)
                if receipt["content"][2] < 1 or receipt["content"][3] < 1:
                    fail("display proof on %r reports an empty content rect"
                         % device)

    sdl3 = adapter_contract.get("input_sdl3_portmaster")
    if sdl3 is not None:
        if not isinstance(sdl3, dict) or set(sdl3) != {
                "enabled", "private_sdl3_sha256"}:
            fail("adapter-contract input_sdl3_portmaster is malformed")
        if not isinstance(sdl3.get("enabled"), bool):
            fail("adapter-contract input_sdl3_portmaster.enabled must be a "
                 "boolean")
        digest = sdl3.get("private_sdl3_sha256")
        if sdl3["enabled"]:
            if not isinstance(digest, str) or not re.fullmatch(
                    r"[0-9a-f]{64}", digest):
                fail("an enabled input_sdl3_portmaster must pin the private "
                     "SDL3 SHA-256")
        elif digest not in ("", None):
            fail("a disabled input_sdl3_portmaster must not pin an SDL3")

    profiles = adapter_contract.get("input_controller_profiles")
    # A field regression proved that a live nxinput runtime without its
    # packaged third authority can lose the rescue step on imperfect CFW data.
    # nxgenerator refuses the project and this mirror refuses the package.
    input_contract = adapter_contract.get("input")
    if (isinstance(input_contract, dict) and
            input_contract.get("runtime_mapping") == "nxinput-gptk" and
            (not isinstance(profiles, dict) or
             profiles.get("enabled") is not True)):
        fail("input runtime_mapping nxinput-gptk requires an enabled "
             "input_controller_profiles bundle inside the package "
             "(authority 3 of the sovereign order)")
    if profiles is not None:
        if not isinstance(profiles, dict) or set(profiles) - {
                "face_layout_variants"} != {"enabled", "bundle", "sha256"}:
            fail("adapter-contract input_controller_profiles is malformed")
        if not isinstance(profiles.get("enabled"), bool):
            fail("adapter-contract input_controller_profiles.enabled must be "
                 "a boolean")
        bundle_name = profiles.get("bundle")
        digest = profiles.get("sha256")
        if not profiles["enabled"]:
            if digest not in ("", None) or bundle_name not in ("", None):
                fail("a disabled input_controller_profiles must not pin a "
                     "bundle")
        else:
            if (not isinstance(bundle_name, str) or not bundle_name or
                    "/" in bundle_name or bundle_name.startswith(".") or
                    not re.fullmatch(r"[A-Za-z0-9._-]+", bundle_name)):
                fail("input_controller_profiles.bundle must be a plain file "
                     "name inside the port")
            if bundle_name != "controllers.nxb":
                fail("input_controller_profiles.bundle must be exactly "
                     "controllers.nxb: the runtime authority-3 declaration "
                     "uses that canonical name")
            if not isinstance(digest, str) or not re.fullmatch(
                    r"[0-9a-f]{64}", digest):
                fail("an enabled input_controller_profiles must pin the "
                     "bundle SHA-256")
            # Closure: the content-addressed bundle must be IN the package,
            # byte-exact against the pin, carry the NXCONTROLLER_PROFILES/1
            # contract header and its upstream license, and smell of no
            # personal data. Nothing may point at `latest` or a runtime
            # download.
            target = config["port_dir"] + "/" + bundle_name
            located = records if records is not None \
                else config.get("records") or []
            record = next((item for item in located
                           if item.get("target") == target), None)
            if record is None:
                fail("input_controller_profiles pins %s but the package does "
                     "not carry it" % bundle_name)
            if record.get("sha256") != digest:
                fail("the packaged controller-profiles bundle does not match "
                     "the pinned SHA-256")
            bundle_path = record.get("actual_path")
            if bundle_path is None:
                bundle_path = record.get("source")
            try:
                with open(bundle_path, "rb") as stream:
                    payload = stream.read(8 * 1024 * 1024 + 1)
            except (OSError, TypeError):
                fail("the packaged controller-profiles bundle is unreadable")
            if len(payload) > 8 * 1024 * 1024:
                fail("the packaged controller-profiles bundle is oversized")
            # The pin is checked against the BYTES on disk, not only against
            # what the package record claims about them.
            if hashlib.sha256(payload).hexdigest() != digest:
                fail("the packaged controller-profiles bundle does not match "
                     "the pinned SHA-256")
            text = payload.decode("utf-8", "replace")
            if not text.startswith("NXCONTROLLER_PROFILES/1\n"):
                fail("the controller-profiles bundle lacks the "
                     "NXCONTROLLER_PROFILES/1 header")
            if "\n# license=" not in text:
                fail("the controller-profiles bundle lacks its license "
                     "header")
            if re.search(r"(/home/|/storage/|/roms/|"
                         r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b|"
                         r"hostname|latest)", text):
                fail("the controller-profiles bundle carries forbidden "
                     "content (personal data, address or a latest pointer)")

            # nxinput 0.10.0: the FACE_LAYOUT variant pair. Only a V3 GPTK
            # default may declare it; when the port's default is V3 and the
            # profiles are enabled, the COMPLETE authenticated pair is
            # mandatory. Each variant passes the same closure as the base
            # (packaged, byte-exact pin, V1 header, license, no personal
            # data), the pair holds the same identity set and diverges only
            # in the authorized a/b/x/y face bindings plus '#' provenance
            # metadata, and the mutable pair GUIDs never re-enter the
            # invariant base -- freezing one half of a user preference was
            # the 0.9.0 field defect. `auto` selects no variant by
            # construction (the runtime declares only controllers.nxb).
            variants = profiles.get("face_layout_variants")
            gptk_schema_now = config.get("gptk_schema")
            if variants is not None and gptk_schema_now != 3:
                fail("face_layout_variants requires a NEXTOS_CONTROLLERS/3 "
                     "default (V1/V2 never carry the V3 block)")
            if gptk_schema_now == 3 and variants is None:
                fail("a NEXTOS_CONTROLLERS/3 port with enabled "
                     "controller profiles must pin the complete "
                     "face_layout_variants pair")
            if variants is not None:
                if not isinstance(variants, dict) or set(variants) != {
                        "modern", "retro"}:
                    fail("face_layout_variants must declare exactly the "
                         "complete modern and retro pair")
                variant_lines = {}
                variant_digests = set()
                for layout in ("modern", "retro"):
                    entry = variants[layout]
                    expected_name = "controllers-{}.nxb".format(layout)
                    if (not isinstance(entry, dict) or
                            set(entry) != {"bundle", "sha256"} or
                            entry.get("bundle") != expected_name):
                        fail("face_layout_variants.{} must pin exactly "
                             "bundle {} and its sha256".format(
                                 layout, expected_name))
                    variant_digest = entry.get("sha256")
                    if not isinstance(variant_digest, str) or                             not re.fullmatch(r"[0-9a-f]{64}",
                                             variant_digest):
                        fail("face_layout_variants.{} must pin the variant "
                             "SHA-256".format(layout))
                    variant_digests.add(variant_digest)
                    variant_target = config["port_dir"] + "/" + expected_name
                    variant_record = next(
                        (item for item in located
                         if item.get("target") == variant_target), None)
                    if variant_record is None:
                        fail("face_layout_variants pins {} but the package "
                             "does not carry it".format(expected_name))
                    if variant_record.get("sha256") != variant_digest:
                        fail("the packaged {} does not match its pinned "
                             "SHA-256".format(expected_name))
                    variant_path = variant_record.get("actual_path")
                    if variant_path is None:
                        variant_path = variant_record.get("source")
                    try:
                        if os.path.islink(str(variant_path)):
                            fail("{} must not be a symlink".format(
                                expected_name))
                        with open(variant_path, "rb") as stream:
                            variant_payload = stream.read(
                                8 * 1024 * 1024 + 1)
                    except (OSError, TypeError):
                        fail("the packaged {} is unreadable".format(
                            expected_name))
                    if not variant_payload:
                        fail("the packaged {} is empty".format(
                            expected_name))
                    if len(variant_payload) > 8 * 1024 * 1024:
                        fail("the packaged {} is oversized".format(
                            expected_name))
                    if hashlib.sha256(variant_payload).hexdigest() !=                             variant_digest:
                        fail("the packaged {} does not match its pinned "
                             "SHA-256".format(expected_name))
                    variant_text = variant_payload.decode("utf-8", "replace")
                    if not variant_text.startswith(
                            "NXCONTROLLER_PROFILES/1\n"):
                        fail("{} lacks the NXCONTROLLER_PROFILES/1 "
                             "header".format(expected_name))
                    if "\n# license=" not in variant_text:
                        fail("{} lacks its license header".format(
                            expected_name))
                    if re.search(r"(/home/|/storage/|/roms/|"
                                 r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\."
                                 r"\d{1,3}\b|hostname|latest)",
                                 variant_text):
                        fail("{} carries forbidden content".format(
                            expected_name))
                    variant_lines[layout] = [
                        line for line in variant_text.splitlines()
                        if line and not line.startswith("#") and
                        not line.startswith("NXCONTROLLER_PROFILES/")]
                if len(variant_digests) != 2:
                    fail("face_layout_variants modern and retro must be "
                         "distinct artifacts")

                def variant_identity(lines):
                    identity = {}
                    for line in lines:
                        guid = line.split(",", 1)[0]
                        fields = sorted(
                            field for field in line.split(",")
                            if ":" in field and
                            field.split(":", 1)[0] not in
                            ("a", "b", "x", "y"))
                        identity[guid] = fields
                    return identity

                modern_identity = variant_identity(variant_lines["modern"])
                retro_identity = variant_identity(variant_lines["retro"])
                if set(modern_identity) != set(retro_identity):
                    fail("face_layout_variants must hold the same identity "
                         "set (a variant may never change which pads "
                         "exist)")
                for guid in modern_identity:
                    if modern_identity[guid] != retro_identity[guid]:
                        fail("face_layout_variants diverge outside the "
                             "authorized a/b/x/y face bindings for GUID "
                             "{}".format(guid))
                if variant_lines["modern"] == variant_lines["retro"]:
                    fail("face_layout_variants must actually differ in the "
                         "face bindings")
                base_lines = [line for line in text.splitlines()
                              if line and not line.startswith("#") and
                              not line.startswith("NXCONTROLLER_PROFILES/")]
                base_guids = {line.split(",", 1)[0] for line in base_lines}
                mutable_guids = set(modern_identity)
                frozen = sorted(base_guids & mutable_guids)
                if frozen:
                    fail("the invariant base bundle froze the mutable "
                         "layout GUID(s) {}; a user-preference face layout "
                         "never enters controllers.nxb".format(
                             ", ".join(frozen)))

    binding = adapter_contract.get("egl_binding")
    if binding is None:
        return
    if not isinstance(binding, dict) or set(binding) != {"enabled", "imports"}:
        fail("adapter-contract egl_binding is malformed")
    if not isinstance(binding.get("enabled"), bool):
        fail("adapter-contract egl_binding.enabled must be a boolean")
    imports = binding.get("imports")
    if not isinstance(imports, list) or any(
            not isinstance(name, str) or
            not re.fullmatch(r"egl[A-Za-z0-9_]+", name) for name in imports):
        fail("adapter-contract egl_binding.imports must be EGL symbol names")
    if not binding["enabled"]:
        if imports:
            fail("a disabled egl_binding must declare an empty inventory")
        return
    if not imports or imports != sorted(imports) or \
            len(set(imports)) != len(imports):
        fail("an enabled egl_binding needs a unique, ordered inventory")
    if "eglGetCurrentContext" not in imports:
        fail("an enabled egl_binding must declare eglGetCurrentContext")
    # The whole point of V4-GRAPHICS-03 is to bind EGL at runtime, per port,
    # to the provider that owns the current context. A packaged ELF that links
    # EGL directly would reintroduce the global dependency the front removes.
    for record in config.get("records", []) or []:
        needed = record.get("needed")
        if not needed:
            continue
        offenders = sorted(
            name for name in needed if str(name).startswith("libEGL")
        )
        if offenders:
            fail("egl_binding is enabled but {} declares DT_NEEDED {}".format(
                record.get("target"), ", ".join(offenders)))


# The public-final provenance schema is frozen and has a different,
# promotion-specific shape. The canonical build/bundle flow gets its own
# schema id so a validator never has to guess which document it holds.
BUILD_PROVENANCE_SCHEMA = "nxrelease-canonical-build-provenance-v1"
BUILD_PROVENANCE_NAME = "BUILD-PROVENANCE.json"


def _repro_stage_elf_digests(stage):
    """Return {staged path: sha256} for every ELF the stage authenticated.

    Comparing the ZIP alone answers "the bytes differ" and stops there. The
    canonical flow has to be able to NAME the ELF that stopped reproducing,
    because that is the difference between a fixable toolchain leak and a
    mystery.
    """
    stage = Path(stage).resolve()
    _port_dir, metadata_name, _checksum, _sbom = \
        discover_stage_internal_paths(stage)
    metadata = load_json(
        stage / PurePosixPath(metadata_name), "release metadata"
    )
    digests = {}
    for item in metadata.get("elf_audit", {}).get("files", []) or []:
        digests[item["path"]] = item["sha256"]
    return digests


def _repro_toolchain_identity():
    """Sanitized identity of the environment that produced these bytes."""
    return {
        "python": "%d.%d.%d" % sys.version_info[:3],
        "platform": platform.machine(),
        "source_date_epoch": os.environ.get("SOURCE_DATE_EPOCH"),
        "umask": "0%o" % _current_umask(),
        "tz": os.environ.get("TZ"),
        "lc_all": os.environ.get("LC_ALL"),
    }


def _current_umask():
    value = os.umask(0o022)
    os.umask(value)
    return value


def _repro_prove(config, result, stage_digests):
    """Optional diagnostic restage; never a second project compilation."""
    probe_root = tempfile.mkdtemp(prefix="nxrelease-repro.")
    try:
        probe_stage = str(Path(probe_root) / "stage")
        probe_zip = str(Path(probe_root) / "probe.zip")
        probe_destination = stage_release(config, probe_stage)
        probe = create_archive(probe_destination, probe_zip)
        probe_digests = _repro_stage_elf_digests(probe_destination)
        if set(probe_digests) != set(stage_digests):
            missing = sorted(set(stage_digests) ^ set(probe_digests))
            fail("build is not reproducible: ELF inventory differs at {}".format(
                ", ".join(missing)))
        divergent = sorted(
            target for target in stage_digests
            if stage_digests[target] != probe_digests[target]
        )
        if divergent:
            fail("build is not reproducible: ELF bytes differ at {}".format(
                ", ".join(divergent)))
        if probe["sha256"] != result["sha256"]:
            fail("build is not reproducible: archive {} != {}".format(
                result["sha256"], probe["sha256"]))
        return {
            "archive_sha256": result["sha256"],
            "elf_count": len(stage_digests),
            "elfs": [
                {"path": target, "sha256": stage_digests[target]}
                for target in sorted(stage_digests)
            ],
        }
    finally:
        shutil.rmtree(probe_root, ignore_errors=True)


def _repro_write_provenance(target, document):
    """Write the build provenance next to the artifact, no-replace.

    ``bundle`` publishes a fresh directory, so it gets the canonical
    ``BUILD-PROVENANCE.json``. ``build`` writes a bare ZIP into a directory that
    may already hold other artifacts, so its provenance is paired with the
    archive exactly like the existing ``<archive>.sha256`` is.
    """
    target = Path(target)
    if target.exists() or target.is_symlink():
        fail("{} already exists".format(target.name))
    parent = target.parent.resolve()
    if not parent.is_dir():
        fail("{} parent does not exist".format(BUILD_PROVENANCE_NAME))
    reject_private_literal(
        json_bytes(document).decode("utf-8"), BUILD_PROVENANCE_NAME
    )
    payload = json_bytes(document)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".nxrelease-provenance-", suffix=".json", dir=str(parent)
    )
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o644)
        stream = os.fdopen(descriptor, "wb")
        descriptor = None
        with stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        rename_noreplace(temporary, parent / target.name)
        temporary = None
        fsync_directory(parent)
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if temporary is not None and temporary.exists():
            temporary.unlink()


def _candidate_binding_proof(config, result, stage_digests):
    release_authority = config_release_authority(config)
    binding = {
        "archive_sha256": result["sha256"],
        "elf_count": len(stage_digests),
        "elfs": [
            {"path": target, "sha256": stage_digests[target]}
            for target in sorted(stage_digests)
        ],
        "method": "stage-archive-integrity",
        "mandatory": True,
        "release_authority": release_authority["mode"],
    }
    if release_authority["mode"] == RELEASE_AUTHORITY_LOCK:
        if not config.get("candidate_lock_verified"):
            fail("canonical provenance lacks a verified candidate lock")
        lock = normalize_candidate_lock(
            config.get("candidate_lock"),
            "canonical provenance candidate lock",
        )
        if stage_digests.get(lock["executable"]) != lock["sha256"]:
            fail("canonical provenance ELF inventory does not contain the locked executable bytes")
        binding.update({
            "candidate_document_sha256": lock["document_sha256"],
            "executable": lock["executable"],
            "executable_sha256": lock["sha256"],
            "method": "candidate-lock-stage-archive-reopen",
        })
    return binding


def _repro_provenance_document(config, result, binding, mode,
                               diagnostic=None):
    return {
        "schema": BUILD_PROVENANCE_SCHEMA,
        "schema_version": 1,
        "sanitized": True,
        "mode": mode,
        "tool": {"name": "nxrelease", "version": TOOL_VERSION},
        "release": {
            "package_id": result["package_id"],
            "package_version": result["package_version"],
            "archive_sha256": result["sha256"],
            "max_glibc": result["max_glibc"],
            "ceiling": config["ceiling"],
        },
        "toolchain": _repro_toolchain_identity(),
        "verification": binding,
        "diagnostic_restage": {
            "performed": diagnostic is not None,
            "archive_sha256": (
                diagnostic["archive_sha256"]
                if diagnostic is not None else None
            ),
        },
    }


def command_build(arguments):
    output_path = Path(arguments.output)
    if output_path.exists() or output_path.is_symlink():
        fail("archive output already exists: {}".format(output_path))
    checksum_path = Path(str(output_path) + ".sha256")
    if checksum_path.exists() or checksum_path.is_symlink():
        fail("archive checksum output already exists: {}".format(checksum_path))
    config = load_manifest(
        arguments.manifest, arguments.max_glibc,
        candidate_lock_path=arguments.candidate_lock,
        release_authority=arguments.authority,
    )
    validate_sources(config)
    destination = stage_release(config, arguments.stage)
    result = create_archive(destination, arguments.output)
    stage_digests = _repro_stage_elf_digests(destination)
    binding = _candidate_binding_proof(config, result, stage_digests)
    diagnostic = None
    if arguments.prove_deterministic:
        diagnostic = _repro_prove(config, result, stage_digests)
        print("NXRELEASE DIAGNOSTIC RESTAGE: PASS sha256={} elfs={}".format(
            result["sha256"], diagnostic["elf_count"]))
    _repro_write_provenance(
        Path(str(Path(arguments.output)) + "." + BUILD_PROVENANCE_NAME),
        _repro_provenance_document(
            config, result, binding, "build", diagnostic=diagnostic
        ),
    )
    print("NXRELEASE BUILD: PASS package={} version={} stage={} archive={} sha256={} elfs={} max_glibc={} ceiling={}".format(
        result["package_id"], result["package_version"], destination, result["archive"],
        result["sha256"], result["elf_count"], result["max_glibc"], config["ceiling"]
    ))


def command_bundle(arguments):
    bundle_path = Path(arguments.destination)
    if bundle_path.exists() or bundle_path.is_symlink():
        fail("bundle destination already exists: {}".format(bundle_path))
    config = load_manifest(
        arguments.manifest, arguments.max_glibc,
        candidate_lock_path=arguments.candidate_lock,
        release_authority=arguments.authority,
    )
    validate_sources(config)
    destination = stage_release(config, arguments.stage)
    result = create_release_bundle(
        destination, arguments.destination, arguments.archive_name
    )
    stage_digests = _repro_stage_elf_digests(destination)
    binding = _candidate_binding_proof(config, result, stage_digests)
    diagnostic = None
    if arguments.prove_deterministic:
        diagnostic = _repro_prove(config, result, stage_digests)
        print("NXRELEASE DIAGNOSTIC RESTAGE: PASS sha256={} elfs={}".format(
            result["sha256"], diagnostic["elf_count"]))
    _repro_write_provenance(
        Path(result["bundle"]) / BUILD_PROVENANCE_NAME,
        _repro_provenance_document(
            config, result, binding, "bundle", diagnostic=diagnostic
        ),
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
        previous_archive=arguments.previous_archive,
        historical_authority_path=arguments.historical_authority,
    )
    if arguments.historical_authority is not None:
        print("NXRELEASE HISTORICAL-QUARANTINE: READ-ONLY package={} version={} archive={} sha256={} publication_eligible=false".format(
            result["package_id"], result["package_version"],
            Path(arguments.archive).resolve(),
            sha256_file(Path(arguments.archive).resolve())
        ))
    else:
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
    validate.add_argument(
        "--candidate-lock",
        help="external read-only nxrelease-candidate-lock-v1 document",
    )
    validate.add_argument("--authority", choices=sorted(RELEASE_AUTHORITIES))
    validate.add_argument("--max-glibc")
    validate.set_defaults(function=command_validate)

    stage = subparsers.add_parser("stage", help="create and verify a fresh staging tree")
    stage.add_argument("--manifest", required=True)
    stage.add_argument("--candidate-lock")
    stage.add_argument("--authority", choices=sorted(RELEASE_AUTHORITIES))
    stage.add_argument("--stage", required=True)
    stage.add_argument("--max-glibc")
    stage.set_defaults(function=command_stage)

    verify_stage_parser = subparsers.add_parser("verify-stage", help="verify a staged release")
    verify_stage_parser.add_argument("--stage", required=True)
    verify_stage_parser.add_argument("--max-glibc")
    verify_stage_parser.set_defaults(function=command_verify_stage)

    build = subparsers.add_parser("build", help="stage, package and re-open/re-verify a release")
    build.add_argument("--manifest", required=True)
    build.add_argument("--candidate-lock")
    build.add_argument("--authority", choices=sorted(RELEASE_AUTHORITIES))
    build.add_argument("--stage", required=True)
    build.add_argument("--output", required=True)
    build.add_argument("--max-glibc")
    build.add_argument(
        "--prove-deterministic", action="store_true",
        help="optional diagnostic restage/repackage; never a second compilation")
    build.set_defaults(function=command_build)

    bundle = subparsers.add_parser(
        "bundle",
        help="atomically publish a new directory containing ZIP and SHA-256",
    )
    bundle.add_argument("--manifest", required=True)
    bundle.add_argument("--candidate-lock")
    bundle.add_argument("--authority", choices=sorted(RELEASE_AUTHORITIES))
    bundle.add_argument("--stage", required=True)
    bundle.add_argument("--destination", required=True)
    bundle.add_argument("--archive-name", required=True)
    bundle.add_argument("--max-glibc")
    bundle.add_argument(
        "--prove-deterministic", action="store_true",
        help="optional diagnostic restage/repackage; never a second compilation")
    bundle.set_defaults(function=command_bundle)

    verify = subparsers.add_parser("verify", help="re-open and verify a built ZIP")
    verify.add_argument("--archive", required=True)
    verify.add_argument("--sha256-file")
    verify.add_argument(
        "--historical-authority",
        help=("external read-only nxrelease-historical-authority-v1; enables "
              "legacy audit only for the exact pinned archive bytes"),
    )
    verify.add_argument(
        "--previous-archive",
        help="previous ZIP to prove a real HarbourMaster overlay update",
    )
    verify.add_argument("--max-glibc")
    verify.set_defaults(function=command_verify)

    preflight = subparsers.add_parser(
        "preflight",
        help=("READ-ONLY aggregate boundary: evaluate every release "
              "category, report every independent error and emit the "
              "org.nextos.v4.preflight-receipt/1 nxledger consumes; "
              "mutates nothing but the explicit --out receipt"),
    )
    preflight.add_argument("--profile", required=True)
    preflight.add_argument("--source", required=True)
    preflight.add_argument("--project", required=True)
    preflight.add_argument("--inputs-manifest", required=True)
    preflight.add_argument("--out")
    preflight.add_argument("--json", action="store_true")
    preflight.set_defaults(function=command_preflight)

    public_final = subparsers.add_parser(
        "public-final",
        help=("fail-closed promotion using exact physical receipts and one "
              "clean source compilation bound by the candidate lock"),
    )
    public_final.add_argument(
        "--archive", required=True,
        help="the exact already-tested ZIP to promote",
    )
    public_final.add_argument(
        "--receipt", action="append", required=True,
        help="sanitized physical receipt (repeat once per required device)",
    )
    public_final.add_argument(
        "--build-script", required=True,
        help="repository-relative executable build script",
    )
    public_final.add_argument(
        "--candidate-lock", required=True,
        help="original external read-only candidate authority",
    )
    public_final.add_argument("--source", required=True)
    public_final.add_argument("--manifest", required=True)
    public_final.add_argument("--build", required=True)
    public_final.add_argument(
        "--build-provenance", required=True,
        help="new output path whose basename is BUILD-PROVENANCE.json",
    )
    public_final.add_argument("--max-glibc")
    public_final.add_argument(
        "--preflight-receipt", required=True,
        help=("PASS org.nextos.v4.preflight-receipt/1 bound to the exact "
              "source commit/tree with profile PUBLIC-FINAL; a DEV, FAIL, "
              "stale or malformed receipt refuses before the first "
              "mutation"),
    )
    public_final.set_defaults(function=command_public_final)
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
