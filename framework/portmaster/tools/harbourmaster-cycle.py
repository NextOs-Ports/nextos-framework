#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run a PortMaster ZIP through the pinned, unmodified HarbourMaster.

The upstream code is executed offline in an exclusive temporary root. Package
launchers and ELFs are data to this gate and are never executed.
"""

import argparse
import builtins
import hashlib
import json
import os
import re
import socket
import stat
import sys
import tempfile
import warnings
import zipfile
from pathlib import Path, PurePosixPath


REPO_ROOT = Path(__file__).resolve().parents[3]
PORTMASTER_ROOT = REPO_ROOT / "framework/portmaster"
VENDOR_ROOT = PORTMASTER_ROOT / "vendor/PortMaster-GUI-8f9ddc4"
VENDOR_PROVENANCE = PORTMASTER_ROOT / "vendor/PortMaster-GUI-8f9ddc4.json"
VENDOR_MANIFEST = PORTMASTER_ROOT / "vendor/PortMaster-GUI-8f9ddc4.sha256"
METADATA_SCHEMA = PORTMASTER_ROOT / "schema/port-json-supported-v2.schema.json"
RUNTIME_NEGATIVE = (
    PORTMASTER_ROOT / "fixtures/harbourmaster/runtime-missing-v1.json"
)
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024


class CycleError(Exception):
    """Expected validation failure."""


def require(condition, message):
    if not condition:
        raise CycleError(message)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(payload):
    return hashlib.sha256(payload).hexdigest()


def load_json_strict_bytes(payload, label):
    require(not payload.startswith(b"\xef\xbb\xbf"), "%s has a UTF-8 BOM" % label)
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise CycleError("%s is not strict UTF-8" % label) from error

    def pairs_hook(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "%s has duplicate key %r" % (label, key))
            result[key] = value
        return result

    def reject_constant(value):
        raise CycleError("%s contains non-JSON constant %s" % (label, value))

    try:
        result = json.loads(
            text, object_pairs_hook=pairs_hook, parse_constant=reject_constant
        )
    except (json.JSONDecodeError, UnicodeError) as error:
        raise CycleError("%s is malformed JSON: %s" % (label, error)) from error
    require(isinstance(result, dict), "%s root is not an object" % label)
    return result


def load_json_file(path):
    require(path.is_file() and not path.is_symlink(), "missing unsafe file: %s" % path)
    return load_json_strict_bytes(path.read_bytes(), str(path.relative_to(REPO_ROOT)))


def verify_vendor_snapshot():
    provenance = load_json_file(VENDOR_PROVENANCE)
    require(
        provenance.get("commit") == "8f9ddc4b0f75dfe61eb370bd3d1b4ec9d5ef6967",
        "HarbourMaster vendor commit changed",
    )
    require(
        sha256_file(VENDOR_MANIFEST) == provenance.get("file_manifest_sha256"),
        "HarbourMaster vendor manifest hash changed",
    )
    expected = {}
    for number, line in enumerate(VENDOR_MANIFEST.read_text(encoding="utf-8").splitlines(), 1):
        match = re.fullmatch(r"([0-9a-f]{64})  [.]\/([^\0]+)", line)
        require(match is not None, "malformed vendor manifest line %d" % number)
        relative = PurePosixPath(match.group(2))
        require(".." not in relative.parts and not relative.is_absolute(), "unsafe vendor path")
        key = relative.as_posix()
        require(key not in expected, "duplicate vendor path: %s" % key)
        expected[key] = match.group(1)

    actual = {}
    for path in sorted(VENDOR_ROOT.rglob("*")):
        if path.is_dir():
            continue
        require(path.is_file() and not path.is_symlink(), "unsafe vendor member: %s" % path)
        require(path.stat().st_nlink == 1, "hard-linked vendor member: %s" % path)
        relative = path.relative_to(VENDOR_ROOT).as_posix()
        actual[relative] = sha256_file(path)
    require(set(actual) == set(expected), "HarbourMaster vendor inventory changed")
    changed = sorted(path for path in expected if actual[path] != expected[path])
    if changed:
        raise CycleError("HarbourMaster vendor bytes changed: %s" % changed[0])


def block_network():
    def blocked(*_args, **_kwargs):
        raise RuntimeError("network is blocked by the HarbourMaster release gate")

    original_socket = socket.socket

    class OfflineSocket(original_socket):
        def connect(self, *_args, **_kwargs):
            return blocked()

        def connect_ex(self, *_args, **_kwargs):
            return blocked()

    socket.socket = OfflineSocket
    socket.create_connection = blocked
    socket.getaddrinfo = blocked


def load_upstream(temp_root):
    os.environ["XDG_DATA_HOME"] = str(temp_root / "xdg")
    block_network()
    warnings.filterwarnings("ignore", category=SyntaxWarning)
    sys.path.insert(0, str(VENDOR_ROOT / "PortMaster/exlibs"))
    sys.path.insert(0, str(VENDOR_ROOT / "PortMaster/pylibs"))
    builtins.PORTMASTER_DEBUG = False

    import fastjsonschema  # noqa: PLC0415
    import harbourmaster  # noqa: PLC0415
    from loguru import logger  # noqa: PLC0415

    logger.remove()
    return harbourmaster, fastjsonschema


def safe_archive_member(info, seen, seen_casefold):
    name = info.filename
    require(name and "\0" not in name and "\\" not in name, "unsafe ZIP member name")
    path = PurePosixPath(name)
    require(not path.is_absolute() and ".." not in path.parts, "ZIP member escapes root")
    normalized = path.as_posix().rstrip("/") + ("/" if info.is_dir() else "")
    require(normalized not in seen, "duplicate ZIP member: %s" % normalized)
    require(normalized.casefold() not in seen_casefold, "case-colliding ZIP member: %s" % normalized)
    seen.add(normalized)
    seen_casefold.add(normalized.casefold())
    mode = (info.external_attr >> 16) & 0xFFFF
    kind = stat.S_IFMT(mode)
    require(kind in (0, stat.S_IFREG, stat.S_IFDIR), "special ZIP member: %s" % normalized)
    return path


def inspect_archive(archive, validator):
    require(archive.is_file() and not archive.is_symlink(), "archive is missing or linked")
    with zipfile.ZipFile(str(archive), "r") as package:
        infos = package.infolist()
        require(infos, "archive is empty")
        require(sum(info.file_size for info in infos) <= MAX_ARCHIVE_BYTES, "archive is excessive")
        seen = set()
        seen_casefold = set()
        top_dirs = set()
        launchers = set()
        port_jsons = []
        files = {}
        modes = {}
        for info in infos:
            path = safe_archive_member(info, seen, seen_casefold)
            if len(path.parts) > 1:
                top_dirs.add(path.parts[0] + "/")
            elif not info.is_dir():
                require(path.name.endswith(".sh"), "non-launcher file at ZIP root: %s" % path)
                launchers.add(path.name)
            if len(path.parts) == 2 and path.name == "port.json" and not info.is_dir():
                port_jsons.append(path.as_posix())
            if not info.is_dir():
                files[path.as_posix()] = package.read(info)
                modes[path.as_posix()] = "%04o" % (
                    (info.external_attr >> 16) & 0o777
                )

        require(len(launchers) == 1, "archive must have exactly one root launcher")
        require(len(port_jsons) == 1, "archive must have exactly one <port>/port.json")
        port_dir = PurePosixPath(port_jsons[0]).parts[0]
        require(
            "%s/INSTALLATION.md" % port_dir in files,
            "archive lacks <port>/INSTALLATION.md",
        )
        metadata = load_json_strict_bytes(files[port_jsons[0]], port_jsons[0])
        try:
            validator(metadata)
        except Exception as error:
            raise CycleError("port.json schema rejected metadata: %s" % error) from error

        required = metadata["items"]
        optional = metadata["items_opt"]
        require(not (set(required) & set(optional)), "items and items_opt overlap")
        actual_items = launchers | top_dirs
        require(set(required) <= actual_items, "required item is absent from ZIP")
        require(actual_items <= set(required) | set(optional), "ZIP has an undeclared top-level item")
        for item in required:
            require(item in actual_items, "item case/path mismatch: %s" % item)
        return {
            "metadata": metadata,
            "port_json": port_jsons[0],
            "port_dir": port_dir,
            "files": files,
            "modes": modes,
            "launchers": launchers,
        }


def generation_relative_path(value, label):
    require(isinstance(value, str), "%s is not a string" % label)
    relative = PurePosixPath(value)
    require(
        value == relative.as_posix() and not relative.is_absolute() and
        all(part not in ("", ".", "..") for part in relative.parts),
        "%s is not a normalized relative path" % label,
    )
    return relative


def generation_digest(value, label):
    require(
        isinstance(value, str) and
        re.fullmatch(r"[0-9a-f]{64}", value) is not None,
        "%s is not a canonical SHA-256" % label,
    )
    return value


def validate_generation_identity_common(identity, schema_version, label):
    common_fields = {"nxbootstrap", "nxport_sha256", "nxsplash", "schema",
                     "schema_version"}
    require(isinstance(identity, dict), "%s identity is invalid" % label)
    require(common_fields <= set(identity),
            "%s identity common fields are incomplete" % label)
    require(
        identity.get("schema") ==
        "org.nextos.nxruntime.generation-identity" and
        identity.get("schema_version") == schema_version,
        "%s identity schema is invalid" % label,
    )
    generation_digest(identity.get("nxport_sha256"),
                      "%s nxport identity" % label)
    bootstrap = identity.get("nxbootstrap")
    require(isinstance(bootstrap, dict) and set(bootstrap) == {
        "generator_sha256", "launcher_template_sha256", "version",
    }, "%s nxbootstrap identity is invalid" % label)
    generation_digest(bootstrap.get("generator_sha256"),
                      "%s generator identity" % label)
    generation_digest(bootstrap.get("launcher_template_sha256"),
                      "%s launcher-template identity" % label)
    require(isinstance(bootstrap.get("version"), str) and
            re.fullmatch(r"[0-9]+[.][0-9]+[.][0-9]+", bootstrap["version"]),
            "%s nxbootstrap version is invalid" % label)
    splash = identity.get("nxsplash")
    require(isinstance(splash, dict) and set(splash) == {
        "architecture", "sha256", "version",
    }, "%s NXSplash identity is invalid" % label)
    require(splash.get("architecture") in {
        "aarch64", "armv7", "x86_64", "i386",
    }, "%s NXSplash architecture is invalid" % label)
    generation_digest(splash.get("sha256"), "%s NXSplash identity" % label)
    require(isinstance(splash.get("version"), str) and
            re.fullmatch(r"[0-9]+[.][0-9]+[.][0-9]+", splash["version"]),
            "%s NXSplash version is invalid" % label)


def validate_generation_v1(store_files, store_modes, generation_prefix,
                           generation_id, manifest, label):
    require(len(generation_id) in (32, 64),
            "%s v1 generation id has an invalid length" % label)
    require(set(manifest) == {
        "components", "generation_id", "identity_basis", "schema",
        "schema_version",
    }, "%s v1 generation manifest fields are not canonical" % label)
    components = manifest.get("components")
    require(isinstance(components, dict) and len(components) == 2,
            "%s v1 generation does not contain two components" % label)
    expected_files = set()
    checksums = []
    launcher_count = 0
    nxport_count = 0
    launcher_payload = None
    nxport_payload = None
    for relative, record in components.items():
        path = generation_relative_path(relative, "%s v1 component path" % label)
        require(
            isinstance(record, dict) and set(record) == {"mode", "sha256"},
            "%s v1 component metadata is not canonical" % label,
        )
        mode = record.get("mode")
        digest = generation_digest(
            record.get("sha256"), "%s v1 component hash" % label
        )
        if len(path.parts) == 2 and path.parts[0] == "launcher" and \
                path.name.endswith(".sh"):
            require(mode == "0755", "%s v1 launcher mode is invalid" % label)
            launcher_count += 1
            launcher_payload = store_files.get(
                generation_prefix + "files/" + relative
            )
        elif relative == "nxport.json":
            require(mode == "0644", "%s v1 nxport mode is invalid" % label)
            nxport_count += 1
            nxport_payload = store_files.get(
                generation_prefix + "files/" + relative
            )
        else:
            raise CycleError("%s v1 component role/path is invalid" % label)
        logical = generation_prefix + "files/" + relative
        payload = store_files.get(logical)
        require(payload is not None,
                "%s v1 component file is absent" % label)
        require(sha256_bytes(payload) == digest,
                "%s v1 component bytes are stale" % label)
        require(store_modes.get(logical) == mode,
                "%s v1 component mode is stale" % label)
        expected_files.add(logical)
        checksums.append((relative, digest))
    require(launcher_count == 1 and nxport_count == 1,
            "%s v1 launcher/nxport closure is incomplete" % label)
    require(generation_id.encode("ascii") in launcher_payload,
            "%s v1 launcher does not carry its generation id" % label)
    checksum_payload = "".join(
        "%s  %s\n" % (digest, path)
        for path, digest in sorted(checksums)
    ).encode("utf-8")
    require(
        store_files.get(generation_prefix + "components.sha256") ==
        checksum_payload,
        "%s v1 components.sha256 is stale" % label,
    )
    expected_files.update({
        generation_prefix + "commit",
        generation_prefix + "components.sha256",
        generation_prefix + "manifest.json",
    })
    require(set(store_files) == expected_files,
            "%s v1 generation file closure is incomplete" % label)
    identity = manifest.get("identity_basis")
    if len(generation_id) == 32:
        require(identity == "nxport-canonical-sha256",
                "%s historical v1 identity marker is invalid" % label)
        require(generation_id == sha256_bytes(nxport_payload)[:32],
                "%s historical v1 id does not bind nxport.json" % label)
    else:
        require(isinstance(identity, dict) and set(identity) == {
            "nxbootstrap", "nxport_sha256", "nxsplash", "schema",
            "schema_version",
        }, "%s v1 identity fields are not canonical" % label)
        validate_generation_identity_common(identity, 1, "%s v1" % label)
        require(identity.get("nxport_sha256") ==
                components["nxport.json"]["sha256"],
                "%s v1 nxport identity is stale" % label)
        identity_payload = json.dumps(
            identity, sort_keys=True, separators=(",", ":"),
            ensure_ascii=False
        ).encode("utf-8")
        require(generation_id == sha256_bytes(identity_payload),
                "%s v1 generation id does not bind its identity" % label)
    for control in ("commit", "components.sha256", "manifest.json"):
        require(store_modes.get(generation_prefix + control) == "0644",
                "%s v1 control mode is stale" % label)
    canonical_manifest = (
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    require(store_files[generation_prefix + "manifest.json"] ==
            canonical_manifest,
            "%s v1 manifest bytes are not canonical" % label)
    return "legacy-control-only"


def validate_generation_v2(store_files, store_modes, generation_prefix,
                           generation_id, manifest, label):
    require(len(generation_id) == 64,
            "%s v2 generation id is not a full SHA-256" % label)
    require(set(manifest) == {
        "components", "generation_id", "identity_basis", "schema",
        "schema_version",
    }, "%s v2 generation manifest fields are not canonical" % label)
    components = manifest.get("components")
    require(isinstance(components, list) and len(components) >= 4,
            "%s v2 generation component list is incomplete" % label)
    role_rules = {
        "launcher": (0, {"0755"}),
        "nxport": (1, {"0644"}),
        "executable": (2, {"0755"}),
        "private-library": (3, {"0644", "0755"}),
        "runtime-data": (4, {"0644"}),
        "runtime-hook": (5, {"0644", "0755"}),
        "nxextract-recipe": (6, {"0644"}),
        "nxextract-engine": (7, {"0644"}),
        "nxextract-runner": (8, {"0644"}),
        "nxextract-runtime-env": (9, {"0644"}),
        "nxextract-ui": (10, {"0755"}),
        "nxextract-helper": (11, {"0644", "0755"}),
        "nxextract-spec": (12, {"0644"}),
        "nxsplash": (13, {"0755"}),
    }
    normalized = []
    store_records = []
    expected_files = set()
    seen_paths = set()
    counts = {}
    previous_key = None
    for index, record in enumerate(components):
        require(
            isinstance(record, dict) and
            set(record) == {"mode", "path", "role", "sha256"},
            "%s v2 component %d is not canonical" % (label, index),
        )
        role = record.get("role")
        path = record.get("path")
        mode = record.get("mode")
        digest = generation_digest(
            record.get("sha256"), "%s v2 component hash" % label
        )
        generation_relative_path(path, "%s v2 component path" % label)
        require(role in role_rules, "%s v2 component role is invalid" % label)
        rank, allowed_modes = role_rules[role]
        require(mode in allowed_modes, "%s v2 component mode is invalid" % label)
        order_key = (rank, path)
        require(previous_key is None or order_key > previous_key,
                "%s v2 component role order is invalid" % label)
        previous_key = order_key
        require(path not in seen_paths,
                "%s v2 repeats a component path" % label)
        seen_paths.add(path)
        counts[role] = counts.get(role, 0) + 1
        if role == "launcher":
            require(PurePosixPath(path).name == path and path.endswith(".sh"),
                    "%s v2 launcher path is invalid" % label)
            source_relative = "launcher/" + path
        elif role == "nxport":
            require(path == "nxport.json", "%s v2 nxport path is invalid" % label)
            source_relative = "nxport.json"
        else:
            if role == "nxextract-recipe":
                require(path == "extractor.json",
                        "%s v2 NXExtract recipe path is invalid" % label)
            elif role == "nxextract-engine":
                require(path == "nxextract/nxextract.py",
                        "%s v2 NXExtract engine path is invalid" % label)
            elif role == "nxextract-runner":
                require(path == "nxextract/run-extractor.sh",
                        "%s v2 NXExtract runner path is invalid" % label)
            elif role == "nxextract-runtime-env":
                require(path == "nxextract/nxextract-runtime-env.sh",
                        "%s v2 NXExtract env path is invalid" % label)
            elif role == "nxextract-ui":
                require(path == "nxextract/nxextract-ui",
                        "%s v2 NXExtract UI path is invalid" % label)
            elif role in ("nxextract-helper", "nxextract-spec"):
                require(path.startswith("nxextract/") and path not in {
                    "nxextract/nxextract.py", "nxextract/run-extractor.sh",
                    "nxextract/nxextract-runtime-env.sh", "nxextract/nxextract-ui",
                }, "%s v2 NXExtract extra path is invalid" % label)
            elif role == "nxsplash":
                require(path == "nxsplash-nextos",
                        "%s v2 NXSplash path is invalid" % label)
            source_relative = "runtime/" + path
        logical = generation_prefix + "files/" + source_relative
        payload = store_files.get(logical)
        require(payload is not None,
                "%s v2 component file is absent" % label)
        require(sha256_bytes(payload) == digest,
                "%s v2 component bytes are stale" % label)
        require(store_modes.get(logical) == mode,
                "%s v2 component mode is stale" % label)
        expected_files.add(logical)
        store_records.append((source_relative, digest))
        normalized.append({
            "mode": mode, "path": path, "role": role, "sha256": digest,
        })
    for role in ("launcher", "nxport", "executable", "nxsplash"):
        require(counts.get(role) == 1,
                "%s v2 required role %s is incomplete" % (label, role))
    nxextract_core = {
        "nxextract-recipe", "nxextract-engine", "nxextract-runner",
        "nxextract-runtime-env", "nxextract-ui",
    }
    present_nxextract = {role for role in counts if role.startswith("nxextract-")}
    require(not present_nxextract or
            (nxextract_core <= present_nxextract and
             all(counts.get(role) == 1 for role in nxextract_core)),
            "%s v2 NXExtract core closure is incomplete" % label)

    components_v2 = "".join(
        "{role}\t{mode}\t{sha256}\t{path}\n".format(**record)
        for record in normalized
    ).encode("utf-8")
    runtime_records = "".join(
        "{role}\t{mode}\t{sha256}\t{path}\n".format(**record)
        for record in normalized if record["role"] not in ("launcher", "nxport")
    ).encode("utf-8")
    checksums = "".join(
        "%s  %s\n" % (digest, path)
        for path, digest in sorted(store_records)
    ).encode("utf-8")
    identity = manifest.get("identity_basis")
    require(isinstance(identity, dict) and set(identity) == {
        "components", "launcher_preimage_sha256", "nxbootstrap",
        "nxport_sha256", "nxsplash", "runtime_records_sha256", "schema",
        "schema_version",
    }, "%s v2 identity fields are not canonical" % label)
    require(
        identity.get("schema") == "org.nextos.nxruntime.generation-identity" and
        identity.get("schema_version") == 2,
        "%s v2 identity schema is invalid" % label,
    )
    validate_generation_identity_common(identity, 2, "%s v2" % label)
    identity_components = identity.get("components")
    require(isinstance(identity_components, list) and
            len(identity_components) == len(normalized),
            "%s v2 identity component closure is incomplete" % label)
    launcher_preimage = generation_digest(
        identity.get("launcher_preimage_sha256"),
        "%s v2 launcher preimage" % label,
    )
    for index, (identity_record, actual_record) in enumerate(
            zip(identity_components, normalized)):
        require(isinstance(identity_record, dict) and
                set(identity_record) == set(actual_record),
                "%s v2 identity component %d is invalid" % (label, index))
        expected = dict(actual_record)
        if actual_record["role"] == "launcher":
            expected["sha256"] = launcher_preimage
        require(identity_record == expected,
                "%s v2 identity components are stale" % label)
    nxport_record = next(
        record for record in normalized if record["role"] == "nxport"
    )
    splash_record = next(
        record for record in normalized if record["role"] == "nxsplash"
    )
    require(identity.get("nxport_sha256") == nxport_record["sha256"],
            "%s v2 nxport identity is stale" % label)
    splash_identity = identity.get("nxsplash")
    require(splash_identity.get("sha256") == splash_record["sha256"],
            "%s v2 NXSplash identity is stale" % label)
    require(identity.get("runtime_records_sha256") == sha256_bytes(runtime_records),
            "%s v2 runtime identity is stale" % label)
    identity_payload = (
        json.dumps(identity, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")
    require(sha256_bytes(identity_payload) == generation_id,
            "%s v2 generation id does not bind identity.json" % label)
    launcher_record = next(
        record for record in normalized if record["role"] == "launcher"
    )
    launcher_payload = store_files[
        generation_prefix + "files/launcher/" + launcher_record["path"]
    ]
    generation_token = generation_id.encode("ascii")
    require(launcher_payload.count(generation_token) == 1,
            "%s v2 launcher does not carry exactly one generation id" % label)
    require(
        sha256_bytes(launcher_payload.replace(
            generation_token, b"0" * 64, 1
        )) == launcher_preimage,
        "%s v2 launcher preimage identity is stale" % label,
    )
    manifest_payload = (
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    controls = {
        "commit": (generation_id + "\n").encode("ascii"),
        "components.sha256": checksums,
        "components.v2": components_v2,
        "format": b"nxruntime-generation-v2\n",
        "identity-runtime.v2": runtime_records,
        "identity.json": identity_payload,
        "manifest.json": manifest_payload,
    }
    for name, payload in controls.items():
        require(store_files.get(generation_prefix + name) == payload,
                "%s v2 control %s is stale" % (label, name))
        require(store_modes.get(generation_prefix + name) == "0644",
                "%s v2 control %s mode is stale" % (label, name))
        expected_files.add(generation_prefix + name)
    require(set(store_files) == expected_files,
            "%s v2 generation file closure is incomplete" % label)
    return "rollback-v2"


def authenticated_generation_files(inspected, label):
    """Return the one immutable generation owned by an archive, if present.

    HarbourMaster updates directory items by overlay.  The generation-v2
    runtime deliberately relies on that property: the newly installed
    generation is tried while the previous complete generation remains as the
    rollback anchor.  That old root is not an arbitrary stale package member.
    It is allowed only when the previous archive's GENERATION.json authenticates
    its complete path/byte closure and the embedded manifest and commit marker
    agree with the generation id.
    """
    port_dir = inspected["port_dir"]
    files = inspected["files"]
    modes = inspected["modes"]
    receipt_path = "%s/GENERATION.json" % port_dir
    store_prefix = "%s/.nxruntime/generations/" % port_dir
    store_files = {
        path: payload for path, payload in files.items()
        if path.startswith(store_prefix)
    }
    store_modes = {
        path: modes[path] for path in store_files
    }
    receipt_payload = files.get(receipt_path)
    if not store_files and receipt_payload is None:
        return None
    require(
        store_files and receipt_payload is not None,
        "%s has a generation store/receipt split" % label,
    )
    receipt = load_json_strict_bytes(receipt_payload, receipt_path)
    required_receipt_fields = {
        "artifacts", "claims", "generation_id", "generator",
        "project_manifest_sha256", "schema", "schema_version",
        "source_pins",
    }
    optional_receipt_fields = {"execution_roles"}
    require(
        required_receipt_fields <= set(receipt) and
        not (set(receipt) - required_receipt_fields -
             optional_receipt_fields) and
        receipt.get("schema") == "nxgenerator-receipt-v1" and
        receipt.get("schema_version") == 1,
        "%s has an unsupported generation receipt" % label,
    )
    generator = receipt.get("generator")
    require(isinstance(generator, dict) and set(generator) == {
        "name", "version",
    } and generator.get("name") == "nxgenerator" and
            isinstance(generator.get("version"), str) and
            re.fullmatch(r"[0-9]+[.][0-9]+[.][0-9]+",
                         generator["version"]),
            "%s has an invalid generator identity" % label)
    generator_version = tuple(
        int(part) for part in generator["version"].split(".")
    )
    generation_digest(receipt.get("project_manifest_sha256"),
                      "%s project-manifest identity" % label)
    require(isinstance(receipt.get("claims"), dict) and
            isinstance(receipt.get("source_pins"), dict),
            "%s has invalid receipt claims/source pins" % label)
    generation_id = receipt.get("generation_id")
    require(isinstance(generation_id, str) and
            re.fullmatch(r"(?:[0-9a-f]{32}|[0-9a-f]{64})", generation_id),
            "%s has an invalid generation id" % label)
    generation_prefix = store_prefix + generation_id + "/"
    require(
        all(path.startswith(generation_prefix) for path in store_files),
        "%s generation store contains an unauthenticated root" % label,
    )

    artifacts = receipt.get("artifacts")
    require(isinstance(artifacts, list), "%s artifacts are not a list" % label)
    authenticated = {}
    seen_artifacts = set()
    artifact_paths = []
    for index, artifact in enumerate(artifacts):
        require(
            isinstance(artifact, dict) and
            set(artifact) == {"mode", "path", "sha256"},
            "%s artifact %d is not canonical" % (label, index),
        )
        logical = artifact.get("path")
        require(isinstance(logical, str), "%s artifact path is invalid" % label)
        generation_relative_path(logical, "%s artifact path" % label)
        require(
            artifact.get("mode") in ("0644", "0755") and
            isinstance(artifact.get("sha256"), str) and
            re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"]) is not None,
            "%s artifact metadata is invalid" % label,
        )
        require(logical not in seen_artifacts,
                "%s repeats an artifact path" % label)
        seen_artifacts.add(logical)
        artifact_paths.append(logical)
        artifact_class = (
            "generation" if logical.startswith(store_prefix) else "receipt"
        )
        payload = files.get(logical)
        require(payload is not None,
                "%s %s artifact is absent" % (label, artifact_class))
        require(sha256_bytes(payload) == artifact["sha256"],
                "%s %s artifact hash is stale" % (label, artifact_class))
        require(modes.get(logical) == artifact["mode"],
                "%s %s artifact mode is stale" % (label, artifact_class))
        if logical.startswith(store_prefix):
            authenticated[logical] = payload

    expected_artifact_order = (
        sorted(artifact_paths, key=lambda path: PurePosixPath(path).parts)
        if generator_version < (0, 2, 19) else sorted(artifact_paths)
    )
    require(artifact_paths == expected_artifact_order,
            "%s artifact inventory is not canonically ordered" % label)

    require(
        set(authenticated) == set(store_files),
        "%s generation artifact closure is incomplete" % label,
    )
    launcher_name = next(iter(inspected["launchers"]))
    live_store_pairs = (
        (launcher_name,
         generation_prefix + "files/launcher/" + launcher_name, "0755"),
        (port_dir + "/nxport.json",
         generation_prefix + "files/nxport.json", "0644"),
    )
    for live_path, stored_path, expected_mode in live_store_pairs:
        require(live_path in seen_artifacts and stored_path in seen_artifacts,
                "%s live/store pair is absent from the receipt" % label)
        require(files.get(live_path) == store_files.get(stored_path) and
                modes.get(live_path) == expected_mode and
                store_modes.get(stored_path) == expected_mode,
                "%s live/store path, mode or SHA-256 differs" % label)
    stored_nxport = load_json_strict_bytes(
        store_files[generation_prefix + "files/nxport.json"],
        generation_prefix + "files/nxport.json",
    )
    stored_roles = stored_nxport.get("execution_roles")
    if stored_roles is None:
        require("execution_roles" not in receipt,
                "%s legacy receipt invents execution_roles" % label)
    else:
        require(isinstance(stored_roles, dict) and
                receipt.get("execution_roles") == stored_roles,
                "%s execution_roles differ from stored nxport.json" % label)
    commit_path = generation_prefix + "commit"
    manifest_path = generation_prefix + "manifest.json"
    require(
        store_files.get(commit_path) == (generation_id + "\n").encode("ascii"),
        "%s generation commit marker is stale" % label,
    )
    manifest_payload = store_files.get(manifest_path)
    require(manifest_payload is not None,
            "%s generation manifest is absent" % label)
    manifest = load_json_strict_bytes(manifest_payload, manifest_path)
    schema_version = manifest.get("schema_version")
    require(
        manifest.get("generation_id") == generation_id and
        ((manifest.get("schema") == "nxruntime-generation-v1" and
          schema_version == 1) or
         (manifest.get("schema") == "nxruntime-generation-v2" and
          schema_version == 2)),
        "%s generation manifest identity is stale" % label,
    )
    if schema_version == 1:
        generation_class = validate_generation_v1(
            store_files, store_modes, generation_prefix, generation_id,
            manifest, label
        )
    else:
        generation_class = validate_generation_v2(
            store_files, store_modes, generation_prefix, generation_id,
            manifest, label
        )
    return {
        "class": generation_class,
        "generation_id": generation_id,
        "installed": {
            "ports/" + path: payload for path, payload in store_files.items()
        },
        "closure": {
            "ports/" + path: {
                "mode": store_modes[path],
                "sha256": sha256_bytes(payload),
            }
            for path, payload in store_files.items()
        },
    }


def verify_upstream_runtime_negative(harbourmaster):
    broken = load_json_file(RUNTIME_NEGATIVE)
    try:
        harbourmaster.port_info_load(broken, do_default=True)
    except KeyError as error:
        require(error.args == ("runtime",), "unexpected upstream negative: %r" % (error,))
    else:
        raise CycleError("pinned upstream no longer reproduces missing attr.runtime")


def verify_parser(harbourmaster, inspected, label):
    metadata = json.loads(json.dumps(inspected["metadata"]))
    try:
        parsed = harbourmaster.port_info_load(metadata, do_default=True)
    except Exception as error:
        raise CycleError("pinned port_info_load rejected %s: %s" % (label, error)) from error
    require(parsed.get("version") == 4, "%s did not normalize to metadata v4" % label)
    require(isinstance(parsed.get("attr", {}).get("runtime"), list), "%s runtime did not normalize to a list" % label)


def expected_installed_files(inspected):
    result = {}
    for logical, payload in inspected["files"].items():
        path = PurePosixPath(logical)
        if len(path.parts) == 1 and path.name.endswith(".sh"):
            destination = "scripts/" + path.name
        else:
            destination = "ports/" + path.as_posix()
        result[destination] = payload
    return result


def verify_installed_tree(root, inspected, phase, retained=None):
    expected = expected_installed_files(inspected)
    retained = retained or {}
    allowed = dict(retained)
    allowed.update(expected)
    actual = {}
    for prefix in ("ports", "scripts"):
        base = root / prefix
        for path in sorted(base.rglob("*")):
            if path.is_dir():
                continue
            require(path.is_file() and not path.is_symlink(), "%s installed an unsafe member" % phase)
            actual["%s/%s" % (prefix, path.relative_to(base).as_posix())] = path.read_bytes()
    missing = sorted(set(allowed) - set(actual))
    stale = sorted(set(actual) - set(allowed))
    require(not missing, "%s lost installed member: %s" % (phase, missing[0] if missing else ""))
    require(not stale, "%s left stale installed member: %s" % (phase, stale[0] if stale else ""))
    # 19/08: launchers SAIRAM do conjunto mutavel — a crise de campo provou
    # que o .sh e a peca mais critica do update. O HarbourMaster real instala
    # o launcher byte-identico ao do ZIP, com UMA excecao conhecida e inocua:
    # quando o script nao traz a linha "# PORTMASTER: <zip>, <sh>", ele a
    # INSERE apos o shebang. Nossos launchers ja shipam a linha (AB v1.1.4 e
    # v1.1.7 instalaram byte-identicos, provado por sha) — aqui toleramos
    # exatamente essa insercao e NADA mais; qualquer outra divergencia grita.
    mutable = {"ports/" + inspected["port_json"]}
    changed = []
    for path in sorted(allowed):
        if path in mutable or actual[path] == allowed[path]:
            continue
        if path.startswith("scripts/") and path.endswith(".sh"):
            got = actual[path].split(b"\n")
            if (len(got) > 1 and got[1].startswith(b"# PORTMASTER: ") and
                    b"\n".join(got[:1] + got[2:]) == allowed[path]):
                continue
        changed.append(path)
    require(not changed, "%s changed package byte: %s" % (phase, changed[0] if changed else ""))


def require_one_loaded(hm, phase):
    hm.load_ports()
    require(len(hm.installed_ports) == 1, "%s did not produce one installed port" % phase)
    key = next(iter(hm.installed_ports))
    runtime = hm.installed_ports[key].get("attr", {}).get("runtime")
    require(isinstance(runtime, list), "%s load_ports returned invalid runtime" % phase)
    return key


def run_cycle(harbourmaster, root, archive, inspected, previous, previous_inspected):
    tools = root / "tools"
    ports = root / "ports"
    scripts = root / "scripts"
    scratch = root / "scratch"
    scratch.mkdir()
    hm = harbourmaster.HarbourMaster(
        {"offline": True, "no-check": True, "quiet": True, "debug": False},
        tools_dir=tools,
        ports_dir=ports,
        scripts_dir=scripts,
        temp_dir=scratch,
    )
    require(not hm.installed_ports, "temporary root was not empty")

    # Authenticate the candidate even on a clean install.  In particular, a
    # receipt without its store (or a store without its receipt) is a corrupt
    # split state, not a plain ZIP and not a valid first adoption.  Apply the
    # same boundary to --previous before HarbourMaster overlays either tree.
    candidate_generation = authenticated_generation_files(
        inspected, "candidate ZIP"
    )
    previous_generation = (
        authenticated_generation_files(previous_inspected, "previous ZIP")
        if previous_inspected is not None else None
    )

    retained = {}
    retained_generations = 0
    if previous is not None:
        require(hm.install_port(str(previous)) == 0, "pinned HarbourMaster rejected previous ZIP")
        require_one_loaded(hm, "previous-install")
        verify_installed_tree(root, previous_inspected, "previous-install")

        expected_previous = expected_installed_files(previous_inspected)
        expected_candidate = expected_installed_files(inspected)
        installed_store_prefix = (
            "ports/%s/.nxruntime/generations/" % inspected["port_dir"]
        )
        retained_store_paths = {
            path for path in expected_previous
            if path.startswith(installed_store_prefix) and
            path not in expected_candidate
        }
        if previous_generation is not None:
            require(
                candidate_generation is not None,
                "candidate removed the authenticated generation contract",
            )
        if previous_generation is not None and candidate_generation is not None:
            if (previous_generation["generation_id"] ==
                    candidate_generation["generation_id"]):
                require(
                    previous_generation["closure"] ==
                    candidate_generation["closure"],
                    "same generation id has a different path/mode/hash closure",
                )
                require(
                    not retained_store_paths,
                    "update retained files under the candidate generation id",
                )
            else:
                retained = {
                    path: payload
                    for path, payload in previous_generation["installed"].items()
                    if path not in expected_candidate
                }
                require(
                    set(retained) == retained_store_paths,
                    "previous ZIP retained only part of its authenticated generation",
                )
                retained_generations = int(bool(retained))

    require(hm.install_port(str(archive)) == 0, "pinned HarbourMaster rejected candidate ZIP")
    key = require_one_loaded(hm, "candidate-install")
    verify_installed_tree(
        root, inspected, "candidate-install", retained=retained
    )

    require(hm.uninstall_port(key) == 0, "pinned HarbourMaster uninstall failed")
    hm.load_ports()
    require(not hm.installed_ports, "uninstall left a discovered port")
    require(not list(ports.iterdir()), "uninstall left an owned ports item")
    require(not list(scripts.iterdir()), "uninstall left an owned scripts item")

    require(hm.install_port(str(archive)) == 0, "pinned HarbourMaster reinstall failed")
    require_one_loaded(hm, "candidate-reinstall")
    verify_installed_tree(root, inspected, "candidate-reinstall")
    return retained_generations


def main(argv=None):
    parser = argparse.ArgumentParser(description="Pinned real HarbourMaster ZIP cycle")
    parser.add_argument("archive", type=Path)
    parser.add_argument("--previous", type=Path)
    arguments = parser.parse_args(argv)
    try:
        verify_vendor_snapshot()
        schema = load_json_file(METADATA_SCHEMA)
        with tempfile.TemporaryDirectory(prefix="nx-harbourmaster-cycle.") as temporary:
            root = Path(temporary)
            harbourmaster, fastjsonschema = load_upstream(root)
            validator = fastjsonschema.compile(schema)
            verify_upstream_runtime_negative(harbourmaster)
            archive = arguments.archive.resolve()
            inspected = inspect_archive(archive, validator)
            verify_parser(harbourmaster, inspected, "candidate")
            previous = None
            previous_inspected = None
            if arguments.previous is not None:
                previous = arguments.previous.resolve()
                previous_inspected = inspect_archive(previous, validator)
                verify_parser(harbourmaster, previous_inspected, "previous")
                require(
                    previous_inspected["port_dir"] == inspected["port_dir"],
                    "update ZIPs own different port directories",
                )
            retained_generations = run_cycle(
                harbourmaster,
                root,
                archive,
                inspected,
                previous,
                previous_inspected,
            )
        print(
            "HarbourMaster real cycle passed: parser=8f9ddc4 "
            "install=1 update=%d uninstall=1 reinstall=1 "
            "retained_generations=%d guest_execution=0 network=0"
            % (1 if arguments.previous is not None else 0,
               retained_generations)
        )
        return 0
    except (CycleError, OSError, ValueError, zipfile.BadZipFile) as error:
        print("HarbourMaster real cycle failed: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
