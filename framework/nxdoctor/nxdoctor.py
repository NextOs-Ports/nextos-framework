#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxdoctor - read-only diagnostician for the V3 on-device port layout.

Default runs perform ZERO writes: the tool only opens files for reading and
reports generation health, space usage, owner-data presence, config syntax,
lock liveness and log inventory.  Every mutating operation requires its own
explicit action flag plus a resolved target, is idempotent, refuses symlinks
and emits a JSON receipt line.

Clock independence (V3-CLOCK-01): no decision in this tool is ever based on
mtime/ctime or on version-name ordering.  Only explicit generation ids and
the activation_seq counter are used.

Owner data (gamedata/, saves, *.apk, *.obb, NEXTOSCONTROLLERS.gptk,
NEXTOSSETTINGS.txt) is never modified or deleted, under any flag.
"""

import argparse
import errno
import fcntl
import functools
import hashlib
import json
import os
import re
import stat
import sys

TOOL_VERSION = "0.3.0"

# Written into a generation the moment its collection starts, so an
# interrupted collection is resumable and never looks like a whole tree.
GC_CLAIM_NAME = ".nxdoctor-gc-claim"
REPORT_SCHEMA = "nx-doctor-report-v1"
ACTION_SCHEMA = "nx-doctor-action-v1"
SCHEMA_VERSION = 1

# nxbootstrap has written state-v2 since 0.6.35 and generation-v2 since 0.6.36.
# Pinning v1 here made the doctor blind on every port it exists to diagnose:
# state read as "unknown schema", every generation manifest read as corrupt.
# Both schemas are accepted; the v1 pair stays for ports that never migrated.
STATE_SCHEMAS = {"nxruntime-state-v1": 1, "nxruntime-state-v2": 2}
STATE_BASE_KEYS = {"schema", "schema_version", "active", "pending",
                   "previous_healthy", "activation_seq",
                   "prehealth_failures"}
STATE_HEALTH_KEYS = {"failure_generation", "last_health_run_id"}
HEALTH_RUN_ID = re.compile(r"^[A-Za-z0-9._-]{1,192}$")
LOCK_TOKEN = re.compile(r"^[A-Za-z0-9._-]{1,192}$")
GENERATION_SCHEMAS = {
    "nxruntime-generation-v1": 1,
    "nxruntime-generation-v2": 2,
}

# A modern generation id is the COMPLETE SHA-256 (64 hex). The 32-hex form
# is only legacy migration input, so requiring it refused every real id.
GEN_ID = re.compile(r"^(?:[0-9a-f]{32}|[0-9a-f]{64})$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
# NEXTOSSETTINGS.txt: espelho EXATO de framework/nxcompat/src/nxcompat_settings.c
# (auditoria V3, ponto 8: o nxdoctor não pode aprovar o que o runtime rejeita).
# charset = nxset_value_char_ok; chaves = allowlist do runtime; quality no allowlist.
SETTINGS_CHARSET = re.compile(r"^[A-Za-z0-9._-]+$")
SETTINGS_KEYS = ("language", "quality")
SETTINGS_QUALITY = ("auto", "low", "medium", "high")
GPTK_KEY = re.compile(r"^[A-Za-z0-9_.+-]{1,64}$")
GPTK_MAGIC = re.compile(r"^format\s*=\s*NEXTOS_CONTROLLERS/1\s*$")
GPTK_SECTIONS = ("menu", "gameplay", "cursor", "camera")
SETTINGS_MAGIC = "# NEXTOS_SETTINGS/1"

MAX_MANIFEST_BYTES = 1024 * 1024
MAX_STATE_BYTES = 64 * 1024
MAX_COMMIT_BYTES = 4096
MAX_GPTK_BYTES = 64 * 1024
MAX_GPTK_LINES = 512
MAX_SETTINGS_BYTES = 4 * 1024

# Owner-data guard: file or directory names that must never be deleted or
# rewritten by any nxdoctor code path.  Checked before every unlink.
OWNER_NAME_RE = re.compile(
    r"(?:\.(?:apk|obb)$|^gamedata$|^save|^saves$|_saves?$|"
    r"^NEXTOSCONTROLLERS\.gptk$|^NEXTOSSETTINGS\.txt$)",
    re.IGNORECASE)

# Every file a port really carries, rotations included. Listing only some of
# them understates what is on the card, which is exactly what misleads someone
# diagnosing one that filled up.
LOG_NAMES = ("log.txt", "log.prev.txt", "log.prev2.txt",
             "events.jsonl", "events.prev.jsonl",
             "nxextract.log", "nxextract-detail.log",
             "nxextract-detail.log.prev", "nxphase-result.json")
PHASE_RESULT_NAME = "nxphase-result.json"
PHASE_RESULT_SCHEMA = "org.nextos.nxbootstrap.phase-result"
LAUNCHER_ERROR_RE = re.compile(r"^[A-Za-z0-9._-]+-launcher-error\.[^/]*\.log$")


class DoctorError(Exception):
    pass


class Refusal(Exception):
    """An action was refused; carries the stable reason."""


# ---------------------------------------------------------------------------
# bounded, fail-closed primitives
# ---------------------------------------------------------------------------

def is_owner_data_name(name):
    return bool(OWNER_NAME_RE.search(name))


def lstat_regular(path, maximum):
    """lstat a path; require regular non-symlink file within the bound."""
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return None
    except OSError as error:
        raise DoctorError("path could not be inspected") from error
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise DoctorError("not a regular non-symlink file")
    if info.st_size > maximum:
        raise DoctorError("exceeds the bounded size")
    return info


def read_bounded(path, maximum):
    """Read a regular non-symlink file, bounded.  Returns bytes or None."""
    info = lstat_regular(path, maximum)
    if info is None:
        return None
    fd = None
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        opened = os.fstat(fd)
        if not stat.S_ISREG(opened.st_mode) \
                or (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
            raise DoctorError("file changed while it was inspected")
        chunks = []
        total = 0
        while total <= maximum:
            block = os.read(fd, min(65536, maximum + 1 - total))
            if not block:
                break
            chunks.append(block)
            total += len(block)
        value = b"".join(chunks)
    except OSError as error:
        raise DoctorError("file could not be read") from error
    finally:
        if fd is not None:
            os.close(fd)
    if len(value) > maximum:
        raise DoctorError("input grew beyond the bounded size")
    return value


def strict_json(value, context):
    if value.startswith(b"\xef\xbb\xbf"):
        raise DoctorError("%s contains a UTF-8 BOM" % context)

    def unique(pairs):
        result = {}
        for key, item in pairs:
            if key in result:
                raise DoctorError("%s contains a duplicate key" % context)
            result[key] = item
        return result

    def constant(item):
        raise DoctorError("%s contains non-JSON constant %s" % (context, item))

    try:
        return json.loads(value.decode("utf-8"), object_pairs_hook=unique,
                          parse_constant=constant)
    except (UnicodeError, ValueError) as error:
        raise DoctorError("%s is malformed: %s" % (context, error))


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            block = stream.read(1 << 20)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def safe_relpath_parts(relpath):
    """Validate a manifest/CLI relative path.  Returns its parts."""
    if not isinstance(relpath, str) or not relpath or relpath.startswith("/"):
        raise DoctorError("unsafe relative path")
    if "\x00" in relpath or "\\" in relpath:
        raise DoctorError("unsafe relative path")
    parts = relpath.split("/")
    for part in parts:
        if part in ("", ".", ".."):
            raise DoctorError("unsafe relative path")
    return parts


def join_no_symlink(root, parts):
    """Join root/parts, refusing any symlink at every intermediate step."""
    path = root
    for part in parts:
        path = os.path.join(path, part)
        try:
            info = os.lstat(path)
        except FileNotFoundError:
            return path  # nonexistent tail is fine; caller checks presence
        except OSError as error:
            raise DoctorError("path could not be inspected") from error
        if stat.S_ISLNK(info.st_mode):
            raise DoctorError("path contains a symlink")
    return path


def inspect_plain_directory(path, label):
    """Return True for a plain directory, False only when it is absent.

    An action must never turn EACCES/EIO/ESTALE into "already clear" and
    must not traverse a symlinked runtime parent.
    """
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    except OSError as error:
        raise DoctorError("%s could not be inspected" % label) from error
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise DoctorError("%s is not a plain directory" % label)
    return True


def inspect_action_directory_chain(port, parts):
    """Validate the port and every existing action parent without symlinks.

    Returns (full_path, exists).  Once one component is genuinely absent the
    remaining tail cannot exist below it, so only that case may be interpreted
    by a cleanup action as already-clear.
    """
    path = port
    if not inspect_plain_directory(path, "port directory"):
        raise DoctorError("port directory is absent")
    for index, part in enumerate(parts):
        path = os.path.join(path, part)
        if not inspect_plain_directory(path, part + " directory"):
            if index + 1 < len(parts):
                path = os.path.join(path, *parts[index + 1:])
            return path, False
    return path, True


# ---------------------------------------------------------------------------
# state / generations
# ---------------------------------------------------------------------------

def runtime_root(port):
    return os.path.join(port, ".nxruntime")


def generations_root(port):
    return os.path.join(runtime_root(port), "generations")


def load_state(port):
    """Return (state dict or None, problem string or None)."""
    path = os.path.join(runtime_root(port), "state.json")
    try:
        value = read_bounded(path, MAX_STATE_BYTES)
    except DoctorError as error:
        return None, str(error)
    if value is None:
        return None, None  # legacy: no generation state
    try:
        state = strict_json(value, "state.json")
    except DoctorError as error:
        return None, str(error)
    if not isinstance(state, dict):
        return None, "state.json is not an object"
    expected_version = STATE_SCHEMAS.get(state.get("schema"))
    if expected_version is None:
        return None, "state.json has an unknown schema"
    if state.get("schema_version") != expected_version:
        return None, "state.json has an unsupported schema_version"
    keys = set(state)
    if state["schema"] == "nxruntime-state-v1":
        if keys != STATE_BASE_KEYS:
            return None, "state-v1 fields are invalid"
    elif keys != STATE_BASE_KEYS | STATE_HEALTH_KEYS:
        return None, "state-v2 fields are invalid"
    # The launcher writes the LITERAL STRING "null" for an unset slot (the
    # value is interpolated into a hand-built JSON line), so every real
    # state.json carries `"pending":"null"`. Reading that as invalid condemned
    # the state of every port in the field. Normalize it to absent.
    for key in ("active", "pending", "previous_healthy"):
        item = state.get(key)
        if item == "null":
            state[key] = None
            continue
        if item is not None and not (isinstance(item, str)
                                     and GEN_ID.fullmatch(item)):
            return None, "state.json field %s is invalid" % key
    seq = state.get("activation_seq")
    if not isinstance(seq, int) or isinstance(seq, bool) or seq < 0:
        return None, "state.json activation_seq is invalid"
    failures = state.get("prehealth_failures")
    if not isinstance(failures, int) or isinstance(failures, bool) \
            or failures < 0:
        return None, "state.json prehealth_failures is invalid"
    if state["schema"] == "nxruntime-state-v2":
        failure_generation = state.get("failure_generation")
        if failure_generation == "null":
            state["failure_generation"] = None
        elif failure_generation is not None \
                and (not isinstance(failure_generation, str)
                     or not GEN_ID.fullmatch(failure_generation)):
            return None, "state.json failure_generation is invalid"
        health_run = state.get("last_health_run_id")
        if health_run != "null" \
                and (not isinstance(health_run, str)
                     or not HEALTH_RUN_ID.fullmatch(health_run)):
            return None, "state.json last_health_run_id is invalid"
    return state, None


def normalize_generation_v2_components(records):
    """Map generation-v2 component records onto the v1 path->entry shape.

    v2 records name a role plus the logical path; the bytes live under
    files/<role-prefix>/<path>. Returning None marks a malformed list so the
    caller can report it instead of silently verifying nothing.
    """
    prefixes = {
        "launcher": "launcher/",
        "nxport": None,
    }
    normalized = {}
    if not records:
        return None
    for record in records:
        if not isinstance(record, dict):
            return None
        role = record.get("role")
        path = record.get("path")
        mode = record.get("mode")
        digest = record.get("sha256")
        if not isinstance(role, str) or not isinstance(path, str) \
                or not isinstance(mode, str) or not isinstance(digest, str):
            return None
        # The bytes really live under files/ in a generation-v2 store, so the
        # normalized key must be the on-disk path relative to the generation
        # root -- otherwise every component reads as absent.
        if role == "launcher":
            relative = "files/launcher/" + path
        elif role == "nxport":
            relative = "files/nxport.json"
        else:
            relative = "files/runtime/" + path
        if relative in normalized:
            return None
        normalized[relative] = {"sha256": digest, "mode": mode}
    _ = prefixes
    return normalized


def verify_generation(port, generation_id):
    """Verify one generation directory.

    Returns a dict {generation_id, status, problems, components_total,
    components_verified}.  status: complete | incomplete | corrupt | missing.
    Only explicit ids and content hashes decide -- never timestamps.
    """
    result = {"generation_id": generation_id, "status": "missing",
              "problems": [], "notes": [], "components_total": 0,
              "components_verified": 0, "commit_marker": False,
              "manifest_ok": False}
    if not GEN_ID.fullmatch(generation_id):
        result["status"] = "corrupt"
        result["problems"].append("generation id is not 32 lowercase hex")
        return result
    gen_dir = os.path.join(generations_root(port), generation_id)
    try:
        info = os.lstat(gen_dir)
    except OSError:
        result["problems"].append("generation directory is absent")
        return result
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        result["status"] = "corrupt"
        result["problems"].append("generation path is not a plain directory")
        return result

    incomplete = False
    corrupt = False

    # commit marker: exactly the generation id plus one newline
    try:
        commit = read_bounded(os.path.join(gen_dir, "commit"),
                              MAX_COMMIT_BYTES)
    except DoctorError as error:
        commit = None
        corrupt = True
        result["problems"].append("commit marker: %s" % error)
    if commit is None:
        if not corrupt:
            incomplete = True
            result["problems"].append("commit marker is absent")
    elif commit != (generation_id + "\n").encode("ascii"):
        corrupt = True
        result["problems"].append("commit marker does not match the id")
    else:
        result["commit_marker"] = True

    manifest = None
    try:
        value = read_bounded(os.path.join(gen_dir, "manifest.json"),
                             MAX_MANIFEST_BYTES)
        if value is None:
            incomplete = True
            result["problems"].append("manifest.json is absent")
        else:
            manifest = strict_json(value, "manifest.json")
    except DoctorError as error:
        corrupt = True
        result["problems"].append("manifest.json: %s" % error)

    components = None
    if manifest is not None:
        manifest_version = (
            GENERATION_SCHEMAS.get(manifest.get("schema"))
            if isinstance(manifest, dict) else None
        )
        # generation-v2 stores its components as an ordered LIST of records;
        # v1 stores a path-keyed object. The doctor reads both and normalizes
        # to the v1 shape so the rest of the verification is unchanged.
        components_value = (
            manifest.get("components") if isinstance(manifest, dict) else None
        )
        components_ok = (
            isinstance(components_value, dict) if manifest_version == 1
            else isinstance(components_value, list)
        )
        if not isinstance(manifest, dict) \
                or manifest_version is None \
                or manifest.get("schema_version") != manifest_version \
                or manifest.get("generation_id") != generation_id \
                or not components_ok:
            corrupt = True
            result["problems"].append("manifest.json fails the schema")
        else:
            result["manifest_ok"] = True
            if manifest_version == 1:
                components = manifest["components"]
            else:
                components = normalize_generation_v2_components(
                    components_value)
                if components is None:
                    corrupt = True
                    result["manifest_ok"] = False
                    result["problems"].append(
                        "generation-v2 components are malformed")

    if components is not None:
        result["components_total"] = len(components)
        for relpath in sorted(components):
            entry = components[relpath]
            try:
                parts = safe_relpath_parts(relpath)
            except DoctorError:
                corrupt = True
                result["problems"].append(
                    "component path is unsafe: %r" % relpath)
                continue
            if not isinstance(entry, dict):
                corrupt = True
                result["problems"].append(
                    "component entry is invalid: %s" % relpath)
                continue
            want_sha = entry.get("sha256")
            want_mode = entry.get("mode")
            if not (isinstance(want_sha, str) and HEX64.fullmatch(want_sha)):
                corrupt = True
                result["problems"].append(
                    "component sha256 is invalid: %s" % relpath)
                continue
            try:
                path = join_no_symlink(gen_dir, parts)
                info = os.lstat(path)
            except DoctorError as error:
                corrupt = True
                result["problems"].append(
                    "component %s: %s" % (relpath, error))
                continue
            except OSError:
                incomplete = True
                result["problems"].append(
                    "component is absent: %s" % relpath)
                continue
            if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
                corrupt = True
                result["problems"].append(
                    "component is not a regular file: %s" % relpath)
                continue
            if sha256_file(path) != want_sha:
                corrupt = True
                result["problems"].append(
                    "component sha256 mismatch: %s" % relpath)
                continue
            if want_mode is not None:
                if isinstance(want_mode, str):
                    try:
                        want_mode = int(want_mode, 8)
                    except ValueError:
                        want_mode = -1
                verdict = classify_component_mode(want_mode,
                                                  stat.S_IMODE(info.st_mode))
                if verdict == "mismatch":
                    corrupt = True
                    result["problems"].append(
                        "component mode mismatch: %s" % relpath)
                    continue
                if verdict == "not-enforced":
                    result["notes"].append(
                        "filesystem does not enforce modes: %s" % relpath)
            result["components_verified"] += 1

    if corrupt:
        result["status"] = "corrupt"
    elif incomplete:
        result["status"] = "incomplete"
    else:
        result["status"] = "complete"
    return result


def list_generation_entries(port):
    root = generations_root(port)
    try:
        entries = sorted(os.listdir(root))
    except OSError:
        return []
    return entries


# ---------------------------------------------------------------------------
# report sections (all read-only)
# ---------------------------------------------------------------------------

def tree_bytes(root):
    total = 0
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames:
            try:
                info = os.lstat(os.path.join(dirpath, name))
            except OSError:
                continue
            if stat.S_ISREG(info.st_mode):
                total += info.st_size
    return total


def report_space(port, referenced):
    vfs = os.statvfs(port)
    section = {"free_bytes": vfs.f_bavail * vfs.f_frsize,
               "seeds": [], "staging": [], "unreferenced_generations": []}
    # V4-REPACK-01: the visible seed is normally the LARGEST file in a port. A
    # space report that omits it is misleading to whoever is diagnosing a card
    # that filled up, and it is also what tells them whether the local cache
    # can be rebuilt at all.
    try:
        root_names = sorted(os.listdir(port))
    except OSError:
        root_names = []
    for name in root_names:
        if not (name.startswith("nxruntime-") and name.endswith(".nxb")):
            continue
        path = os.path.join(port, name)
        try:
            info = os.lstat(path)
        except OSError:
            continue
        if not stat.S_ISREG(info.st_mode):
            section["seeds"].append({"name": name, "bytes": 0,
                                     "regular": False})
            continue
        section["seeds"].append({
            "name": name,
            "bytes": info.st_size,
            "regular": True,
            "generation_id": name[len("nxruntime-"):-len(".nxb")],
        })
    runtime = runtime_root(port)
    try:
        names = sorted(os.listdir(runtime))
    except OSError:
        names = []
    for name in names:
        if name.startswith("staging"):
            path = os.path.join(runtime, name)
            if os.path.isdir(path) and not os.path.islink(path):
                section["staging"].append(
                    {"path": ".nxruntime/" + name,
                     "bytes": tree_bytes(path)})
    for name in list_generation_entries(port):
        if name not in referenced:
            path = os.path.join(generations_root(port), name)
            section["unreferenced_generations"].append(
                {"generation_id": name,
                 "bytes": tree_bytes(path) if os.path.isdir(path)
                 else 0})
    return section


def report_gamedata(port):
    gamedata = os.path.join(port, "gamedata")
    readme = os.path.join(gamedata, "README.txt")
    return {"gamedata_present": os.path.isdir(gamedata)
            and not os.path.islink(gamedata),
            "readme_present": os.path.isfile(readme)}


def validate_gptk(port):
    path = os.path.join(port, "NEXTOSCONTROLLERS.gptk")
    try:
        value = read_bounded(path, MAX_GPTK_BYTES)
    except DoctorError as error:
        return {"status": "invalid", "reason": str(error)}
    if value is None:
        return {"status": "absent"}
    if b"\x00" in value:
        return {"status": "invalid", "reason": "contains a NUL byte"}
    try:
        text = value.decode("utf-8", "strict")
    except UnicodeError:
        return {"status": "invalid", "reason": "not strict UTF-8"}
    lines = text.splitlines()
    if len(lines) > MAX_GPTK_LINES:
        return {"status": "invalid", "reason": "exceeds the line bound"}
    seen_magic = False
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if not seen_magic:
            if GPTK_MAGIC.fullmatch(line):
                seen_magic = True
                continue
            return {"status": "invalid",
                    "reason": "line %d: missing NEXTOS_CONTROLLERS/1 magic"
                    % number}
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            if section not in GPTK_SECTIONS:
                return {"status": "invalid",
                        "reason": "line %d: unknown section [%s]"
                        % (number, section)}
            continue
        if "=" not in line:
            return {"status": "invalid",
                    "reason": "line %d: not a KEY = value line" % number}
        key = line.split("=", 1)[0].strip()
        if not GPTK_KEY.fullmatch(key):
            return {"status": "invalid",
                    "reason": "line %d: invalid key" % number}
    if not seen_magic:
        return {"status": "invalid",
                "reason": "missing NEXTOS_CONTROLLERS/1 magic"}
    return {"status": "ok"}


def validate_settings(port):
    path = os.path.join(port, "NEXTOSSETTINGS.txt")
    try:
        value = read_bounded(path, MAX_SETTINGS_BYTES)
    except DoctorError as error:
        return {"status": "invalid", "reason": str(error)}
    if value is None:
        return {"status": "absent"}
    if b"\x00" in value:
        return {"status": "invalid", "reason": "contains a NUL byte"}
    try:
        text = value.decode("utf-8", "strict")
    except UnicodeError:
        return {"status": "invalid", "reason": "not strict UTF-8"}
    # O runtime não faz strip: espaço/tab dentro de chave/valor é fatal, e só
    # ' '/'\t' contam como linha em branco. A magic é a primeira linha não-branca.
    def blank(s):
        return all(ch in " \t" for ch in s)

    lines = text.split("\n")
    magic_seen = False
    seen = set()
    for number, raw in enumerate(lines, 1):
        line = raw[:-1] if raw.endswith("\r") else raw  # tolera CRLF
        if blank(line):
            continue
        if not magic_seen:
            if line != SETTINGS_MAGIC:
                return {"status": "invalid",
                        "reason": "missing '# NEXTOS_SETTINGS/1' first line"}
            magic_seen = True
            continue
        if line.startswith("#"):
            continue
        if "=" not in line or line[0] == "=":
            return {"status": "invalid",
                    "reason": "line %d: not a key=value line" % number}
        key, val = line.split("=", 1)
        if len(key) > 32 or not SETTINGS_CHARSET.fullmatch(key):
            return {"status": "invalid",
                    "reason": "line %d: malformed key" % number}
        if not (1 <= len(val) <= 32) or not SETTINGS_CHARSET.fullmatch(val):
            return {"status": "invalid",
                    "reason": "line %d: malformed value" % number}
        if key not in SETTINGS_KEYS:
            return {"status": "invalid",
                    "reason": "line %d: unknown key %s (allowlist: %s)"
                              % (number, key, ", ".join(SETTINGS_KEYS))}
        if key in seen:
            return {"status": "invalid",
                    "reason": "line %d: duplicate key %s" % (number, key)}
        seen.add(key)
        if key == "quality" and val not in SETTINGS_QUALITY:
            return {"status": "invalid",
                    "reason": "line %d: quality not in %s"
                              % (number, "|".join(SETTINGS_QUALITY))}
    if not magic_seen:
        return {"status": "invalid",
                "reason": "missing '# NEXTOS_SETTINGS/1' first line"}
    return {"status": "ok"}


def lock_base_dir():
    base = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    return os.path.join(base, ".nxbootstrap-%d" % os.getuid())


def lock_paths(port):
    port_id = os.path.basename(os.path.abspath(port))
    name = "nxport-%s.flock" % port_id
    base = lock_base_dir()
    return (os.path.join(base, name), os.path.join(base, name + ".d"), name)


def begin_action_guard(port, reserve_fallback=True):
    """Exclude both flock and no-flock launchers during a mutation."""
    flock_path, fallback, _name = lock_paths(port)
    parent = os.path.dirname(os.path.dirname(flock_path))
    base = os.path.dirname(flock_path)
    if not inspect_plain_directory(parent, "runtime lock parent"):
        raise DoctorError("runtime lock parent is absent")
    try:
        base_exists = inspect_plain_directory(base, "runtime lock directory")
    except DoctorError:
        raise
    if not base_exists:
        try:
            os.mkdir(base, 0o700)
        except FileExistsError:
            pass
        except OSError as error:
            raise DoctorError("runtime lock directory could not be created") \
                from error
    if not inspect_plain_directory(base, "runtime lock directory"):
        raise DoctorError("runtime lock directory is absent")
    base_info = os.lstat(base)
    if base_info.st_uid != os.getuid() \
            or stat.S_IMODE(base_info.st_mode) & 0o077:
        raise DoctorError("runtime lock directory is not private")

    flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(flock_path, flags, 0o600)
    reserved = False
    owner_value = None
    try:
        opened = os.fstat(fd)
        path_info = os.lstat(flock_path)
        if not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1 \
                or opened.st_uid != os.getuid() \
                or (opened.st_dev, opened.st_ino) != (
                    path_info.st_dev, path_info.st_ino):
            raise DoctorError("runtime flock identity is unsafe")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise Refusal("a launcher currently holds the port lock") \
                from error
        if reserve_fallback:
            try:
                os.mkdir(fallback, 0o700)
            except FileExistsError as error:
                raise Refusal("a no-flock launcher lock is present") \
                    from error
            reserved = True
            self_start = proc_starttime(os.getpid())
            owner_value = ("pid=%d token=nxdoctor-%d\n"
                           % (os.getpid(), self_start)).encode("ascii")
            owner_fd = os.open(
                os.path.join(fallback, "owner"),
                os.O_WRONLY | os.O_CREAT | os.O_EXCL
                | getattr(os, "O_NOFOLLOW", 0), 0o600)
            try:
                offset = 0
                while offset < len(owner_value):
                    count = os.write(owner_fd, owner_value[offset:])
                    if count <= 0:
                        raise OSError(errno.ENOSPC,
                                      "short lock owner write")
                    offset += count
                os.fsync(owner_fd)
                if os.fstat(owner_fd).st_size != len(owner_value):
                    raise OSError(errno.EIO,
                                  "lock owner size mismatch")
            finally:
                os.close(owner_fd)
        return {"fd": fd, "fallback": fallback,
                "reserved": reserved, "owner": owner_value}
    except BaseException:
        if reserved:
            try:
                os.unlink(os.path.join(fallback, "owner"))
            except OSError:
                pass
            try:
                os.rmdir(fallback)
            except OSError:
                pass
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd)
        raise


def end_action_guard(guard):
    if guard["reserved"]:
        owner_path = os.path.join(guard["fallback"], "owner")
        try:
            value = read_bounded(owner_path, 4096)
            if value == guard["owner"]:
                os.unlink(owner_path)
                os.rmdir(guard["fallback"])
        except (DoctorError, OSError):
            # Fail closed: an unremovable private maintenance reservation may
            # block a later launch, but is never replaced or recursively
            # deleted when its identity changed.
            pass
    try:
        fcntl.flock(guard["fd"], fcntl.LOCK_UN)
    finally:
        os.close(guard["fd"])


def guarded_action(action, reserve_fallback=True):
    """Decorator that places every mutator under the canonical lock."""
    def decorate(function):
        @functools.wraps(function)
        def wrapped(port, target):
            if action in ("restore-previous", "complete-gc"):
                extra = {"generation": target}
            elif action == "discard-staging":
                extra = {"requested_path": target}
            else:
                extra = {"pid": target}
            try:
                guard = begin_action_guard(port, reserve_fallback)
            except Refusal as error:
                return receipt(action, "refused", str(error), **extra)
            except DoctorError as error:
                return receipt(action, "refused", str(error), **extra)
            except OSError as error:
                return denied(action, "action lock could not be acquired",
                              error, **extra)
            try:
                return function(port, target)
            finally:
                end_action_guard(guard)
        return wrapped
    return decorate


def proc_starttime(pid):
    """Field 22 of /proc/<pid>/stat, or None only if provably absent."""
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        raise DoctorError("process pid is invalid")
    try:
        with open("/proc/%d/stat" % pid, "rb") as stream:
            value = stream.read(8192)
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ESRCH):
            return None
        raise DoctorError("process starttime could not be inspected") \
            from error
    # comm may contain spaces/parens: parse after the last ')'.
    tail = value.rsplit(b")", 1)
    if len(tail) != 2:
        raise DoctorError("process stat is malformed")
    fields = tail[1].split()
    # tail fields start at field 3 ("state"); starttime is field 22.
    if len(fields) < 20:
        raise DoctorError("process stat is malformed")
    try:
        starttime = int(fields[19])
    except ValueError as error:
        raise DoctorError("process starttime is malformed") from error
    if starttime <= 0:
        raise DoctorError("process starttime is invalid")
    return starttime


def read_lock_owner(lock_dir):
    """Parse exact historic starttime and current launcher token records."""
    try:
        value = read_bounded(os.path.join(lock_dir, "owner"), 4096)
    except DoctorError:
        return None
    if value is None:
        return None
    try:
        text = value.decode("ascii", "strict")
    except UnicodeError:
        return None
    tokens = text.split()
    if len(tokens) == 2 and all(token.isdigit() for token in tokens):
        pid, starttime = (int(token) for token in tokens)
        if pid <= 0 or pid > 2147483647 \
                or starttime <= 0 or starttime > 9223372036854775807:
            return None
        return {"format": "pid-starttime", "pid": pid,
                "starttime": starttime, "token_sha256": None}
    if len(tokens) == 2 and tokens[0].startswith("pid=") \
            and tokens[1].startswith("token="):
        pid_text = tokens[0][len("pid="):]
        token = tokens[1][len("token="):]
        if not pid_text.isdigit() or not LOCK_TOKEN.fullmatch(token):
            return None
        pid = int(pid_text)
        if pid <= 0 or pid > 2147483647:
            return None
        return {"format": "pid-token", "pid": pid, "starttime": None,
                "token_sha256": hashlib.sha256(
                    token.encode("ascii")).hexdigest()}
    return None


def report_locks(port):
    flock_path, lock_dir, name = lock_paths(port)
    section = {"lock_name": name,
               "flock_present": os.path.lexists(flock_path),
               "fallback_dir_present": os.path.isdir(lock_dir)
               and not os.path.islink(lock_dir)}
    if section["fallback_dir_present"]:
        parsed = read_lock_owner(lock_dir)
        if parsed is not None:
            pid = parsed["pid"]
            recorded = parsed["starttime"]
            try:
                current = proc_starttime(pid)
                inspection_problem = None
            except DoctorError as error:
                current = None
                inspection_problem = str(error)
            owner = {"pid": pid, "owner_format": parsed["format"],
                     "recorded_starttime": recorded,
                     "token_sha256": parsed["token_sha256"],
                     "current_starttime": current}
            if inspection_problem is not None:
                owner["state"] = "unknown"
                owner["problem"] = inspection_problem
            elif current is None:
                owner["state"] = "dead"
            elif recorded is not None and recorded != current:
                owner["state"] = "reused"
            else:
                owner["state"] = "alive"
            section["owner"] = owner
        else:
            section["owner"] = None
    return section


# /roms is exFAT: every regular file reads back 0777 and chmod is a no-op.
# The launcher has always known this -- it proves the property and then falls
# back to "the build is the authority for the pinned mode; runtime integrity
# comes from regular/no-symlink/HASH". The doctor did not, so on the device's
# own filesystem it called EVERY healthy generation corrupt, and would then
# have let --complete-gc collect an unreferenced healthy one as "corrupt".
#
# The doctor is read-only, so it cannot run the launcher's write probe to
# prove chmodless. It therefore reports the weaker, honest thing: a mode that
# is neither of the two pinned POSIX values is an OBSERVATION, not corruption.
# What still bites is the launcher's own executable rule -- a member pinned
# 0755 that is not executable in the mounted view is a real problem -- and,
# above all, the hash, which is what a tamper actually breaks.
POSIX_PINNED_MODES = (0o644, 0o755)


def classify_component_mode(want_mode, actual_mode):
    if not isinstance(want_mode, int) or want_mode not in POSIX_PINNED_MODES:
        return "mismatch"
    if actual_mode == want_mode:
        return "ok"
    if actual_mode in POSIX_PINNED_MODES:
        # The filesystem clearly does enforce modes: this really is wrong.
        return "mismatch"
    if want_mode == 0o755 and not actual_mode & 0o100:
        return "mismatch"
    return "not-enforced"


def report_phase(port):
    """The last phase boundary the launcher published.

    The launcher writes this atomically at every boundary, and it is the
    single most useful fact for a port that will not start: how far it got.
    The doctor was listing the file's SIZE and nothing else. Unreadable,
    foreign or malformed content is reported as such -- never guessed, and
    never a reason to fail the whole report.

    `read` is this reader's own outcome; `status` is the launcher's word for
    the boundary. Collapsing the two would make an unreadable file
    indistinguishable from a phase that failed.
    """
    path = os.path.join(port, PHASE_RESULT_NAME)
    try:
        info = os.lstat(path)
    except OSError:
        return {"read": "absent"}
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        return {"read": "not-a-regular-file"}
    try:
        with open(path, "rb") as handle:
            raw = handle.read(64 * 1024)
        document = json.loads(raw.decode("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError):
        return {"read": "unreadable"}
    if not isinstance(document, dict):
        return {"read": "unreadable"}
    if document.get("schema") != PHASE_RESULT_SCHEMA:
        return {"read": "unknown-schema",
                "schema": str(document.get("schema"))[:64]}
    section = {"read": "ok"}
    for key in ("phase", "boundary", "status", "source", "run_id"):
        value = document.get(key)
        section[key] = value if isinstance(value, str) else None
    for key in ("sequence", "reason_code", "child_status", "pid"):
        value = document.get(key)
        section[key] = value if isinstance(value, int) else None
    return section


def report_logs(port):
    logs = []
    for name in LOG_NAMES:
        path = os.path.join(port, name)
        try:
            info = os.lstat(path)
        except OSError:
            continue
        if stat.S_ISREG(info.st_mode):
            logs.append({"name": name, "bytes": info.st_size})
    try:
        names = sorted(os.listdir(port))
    except OSError:
        names = []
    for name in names:
        if LAUNCHER_ERROR_RE.fullmatch(name):
            try:
                info = os.lstat(os.path.join(port, name))
            except OSError:
                continue
            if stat.S_ISREG(info.st_mode):
                logs.append({"name": name, "bytes": info.st_size})
    return logs


def build_report(port):
    state, state_problem = load_state(port)
    report = {"schema": REPORT_SCHEMA, "schema_version": SCHEMA_VERSION,
              "tool_version": TOOL_VERSION}
    referenced = set()
    if state is None:
        report["state"] = {"mode": "legacy",
                           "note": "legacy: no generation state"}
        if state_problem:
            report["state"]["problem"] = state_problem
    else:
        report["state"] = {"mode": "v3",
                           "active": state["active"],
                           "pending": state["pending"],
                           "previous_healthy": state["previous_healthy"],
                           "activation_seq": state["activation_seq"],
                           "prehealth_failures": state["prehealth_failures"]}
        if state.get("schema") == "nxruntime-state-v2":
            report["state"]["failure_generation"] = \
                state.get("failure_generation")
            report["state"]["last_health_run_id"] = \
                state.get("last_health_run_id")
        for key in ("active", "pending", "previous_healthy",
                    "failure_generation"):
            if state.get(key):
                referenced.add(state[key])
    generations = []
    for name in list_generation_entries(port):
        generations.append(verify_generation(port, name))
    report["generations"] = generations
    report["space"] = report_space(port, referenced)
    report["gamedata"] = report_gamedata(port)
    report["controllers_gptk"] = validate_gptk(port)
    report["settings"] = validate_settings(port)
    report["locks"] = report_locks(port)
    report["logs"] = report_logs(port)
    report["phase"] = report_phase(port)
    return report


def print_human(report):
    state = report["state"]
    if state["mode"] == "legacy":
        print("state: %s" % state["note"])
    else:
        print("state: active=%s pending=%s previous_healthy=%s "
              "activation_seq=%d prehealth_failures=%d"
              % (state["active"], state["pending"],
                 state["previous_healthy"], state["activation_seq"],
                 state["prehealth_failures"]))
    for gen in report["generations"]:
        print("generation %s: %s" % (gen["generation_id"], gen["status"]))
        for problem in gen["problems"]:
            print("  - %s" % problem)
        for note in gen.get("notes", []):
            print("  . %s" % note)
    space = report["space"]
    print("space: free=%d bytes" % space["free_bytes"])
    for item in space.get("seeds", []):
        if item.get("regular"):
            print("  seed %s: %d bytes" % (item["name"], item["bytes"]))
        else:
            print("  seed %s: NOT A REGULAR FILE" % item["name"])
    for item in space["staging"]:
        print("  staging %s: %d bytes" % (item["path"], item["bytes"]))
    for item in space["unreferenced_generations"]:
        print("  unreferenced generation %s: %d bytes"
              % (item["generation_id"], item["bytes"]))
    gamedata = report["gamedata"]
    print("gamedata: present=%s readme=%s"
          % (gamedata["gamedata_present"], gamedata["readme_present"]))
    for label, key in (("NEXTOSCONTROLLERS.gptk", "controllers_gptk"),
                       ("NEXTOSSETTINGS.txt", "settings")):
        entry = report[key]
        line = "%s: %s" % (label, entry["status"])
        if entry.get("reason"):
            line += " (%s)" % entry["reason"]
        print(line)
    locks = report["locks"]
    print("lock %s: flock=%s fallback_dir=%s"
          % (locks["lock_name"], locks["flock_present"],
             locks["fallback_dir_present"]))
    owner = locks.get("owner")
    if owner:
        print("  owner pid=%d state=%s starttime=%s"
              % (owner["pid"], owner["state"], owner["current_starttime"]))
    for entry in report["logs"]:
        print("log %s: %d bytes" % (entry["name"], entry["bytes"]))
    phase = report["phase"]
    if phase["read"] != "ok":
        print("phase: %s" % phase["read"])
    else:
        print("phase: %s %s status=%s source=%s sequence=%s child_status=%s"
              % (phase["phase"], phase["boundary"], phase["status"],
                 phase["source"], phase["sequence"], phase["child_status"]))


# ---------------------------------------------------------------------------
# actions (each requires its explicit flag; all idempotent; receipts on stdout)
# ---------------------------------------------------------------------------

def receipt(action, result, reason=None, **extra):
    line = {"schema": ACTION_SCHEMA, "schema_version": SCHEMA_VERSION,
            "tool_version": TOOL_VERSION, "action": action, "result": result}
    if reason is not None:
        line["reason"] = reason
    line.update(extra)
    print(json.dumps(line, sort_keys=True))
    return 0 if result in ("ok", "already-clear") else 1


def denied(action, what, error, **extra):
    """A filesystem that says no is a receipt, never a traceback.

    Read-only media, a busy entry, a directory the owner cannot write: all
    ordinary field conditions on a handheld's card.  Every action that touches
    the filesystem funnels its OSError through here so the refusal names the
    entry that blocked and the tool still exits with a receipt on stdout.
    """
    return receipt(action, "refused",
                   "%s: %s" % (what, os.strerror(error.errno or 0)),
                   path=os.path.basename(error.filename or "") or "unknown",
                   **extra)


def write_state_atomic(port, state, expected_raw):
    """Atomically replace state.json through a no-follow runtime dirfd."""
    directory = runtime_root(port)
    payload = (json.dumps(state, sort_keys=True) + "\n").encode("utf-8")
    _path, exists = inspect_action_directory_chain(port, [".nxruntime"])
    if not exists:
        raise DoctorError(".nxruntime directory is absent")
    dir_flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) \
        | getattr(os, "O_NOFOLLOW", 0)
    directory_fd = os.open(directory, dir_flags)
    temp = ".state.json.nxdoctor-tmp-%d" % os.getpid()
    try:
        fd = os.open(temp,
                     os.O_WRONLY | os.O_CREAT | os.O_EXCL
                     | getattr(os, "O_NOFOLLOW", 0),
                     0o644, dir_fd=directory_fd)
        try:
            try:
                offset = 0
                while offset < len(payload):
                    written = os.write(fd, payload[offset:])
                    if written <= 0:
                        raise OSError(errno.ENOSPC, "short state write")
                    offset += written
                if os.fstat(fd).st_size != len(payload):
                    raise OSError(errno.EIO, "state size mismatch")
                os.fsync(fd)
            finally:
                os.close(fd)
            if read_bounded(os.path.join(directory, "state.json"),
                            MAX_STATE_BYTES) != expected_raw:
                raise DoctorError("state changed during restore")
            os.replace(temp, "state.json", src_dir_fd=directory_fd,
                       dst_dir_fd=directory_fd)
            os.fsync(directory_fd)
        except BaseException:
            try:
                os.unlink(temp, dir_fd=directory_fd)
            except OSError:
                pass
            raise
    finally:
        os.close(directory_fd)


def scan_tree_guard(root):
    """Walk a tree pre-deletion: refuse symlinks, extra hardlinks and any
    owner-data name.  Never follows symlinks."""
    def walk_error(error):
        raise Refusal("tree could not be inspected (%s)"
                      % os.path.basename(error.filename or "unknown"))

    for dirpath, dirnames, filenames in os.walk(
            root, topdown=True, followlinks=False, onerror=walk_error):
        for name in dirnames + filenames:
            if is_owner_data_name(name):
                raise Refusal("tree contains owner data (%s)" % name)
        for name in filenames:
            path = os.path.join(dirpath, name)
            try:
                info = os.lstat(path)
            except OSError as error:
                raise Refusal("tree entry could not be inspected (%s)"
                              % name) from error
            if stat.S_ISLNK(info.st_mode):
                raise Refusal("tree contains a symlink")
            if stat.S_ISREG(info.st_mode) and info.st_nlink > 1:
                raise Refusal("tree contains a multiply-linked file")
            if stat.S_ISREG(info.st_mode):
                flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
                try:
                    fd = os.open(path, flags)
                    try:
                        opened = os.fstat(fd)
                    finally:
                        os.close(fd)
                except OSError as error:
                    raise Refusal("tree file could not be inspected (%s)"
                                  % name) from error
                if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
                    raise Refusal("tree entry changed while inspected")
        for name in dirnames:
            path = os.path.join(dirpath, name)
            try:
                info = os.lstat(path)
            except OSError as error:
                raise Refusal("tree directory could not be inspected (%s)"
                              % name) from error
            if stat.S_ISLNK(info.st_mode):
                raise Refusal("tree contains a symlink")


def inspect_gc_claim(path, generation_id):
    """Return False when absent; accept only our exact resumable claim."""
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    except OSError as error:
        raise DoctorError("gc claim could not be inspected") from error
    mode = stat.S_IMODE(info.st_mode)
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 \
            or info.st_uid != os.getuid() \
            or mode not in (0o600, 0o644, 0o777):
        raise Refusal("gc claim identity is invalid")
    value = read_bounded(path, MAX_COMMIT_BYTES)
    expected = (generation_id + "\n").encode("ascii")
    if value != expected:
        raise Refusal("gc claim content is invalid")
    return True


def create_gc_claim(root, generation_id):
    """Create an authenticated resumable claim without following links."""
    path = os.path.join(root, GC_CLAIM_NAME)
    payload = (generation_id + "\n").encode("ascii")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL \
        | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags, 0o600)
    try:
        offset = 0
        while offset < len(payload):
            count = os.write(fd, payload[offset:])
            if count <= 0:
                raise OSError(errno.ENOSPC, "short gc claim write")
            offset += count
        os.fsync(fd)
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 \
                or info.st_size != len(payload):
            raise OSError(errno.EIO, "gc claim verification failed")
    except BaseException:
        try:
            current = os.lstat(path)
            opened = os.fstat(fd)
            if (current.st_dev, current.st_ino) == (
                    opened.st_dev, opened.st_ino):
                os.unlink(path)
        except OSError:
            pass
        raise
    finally:
        os.close(fd)


def quarantine_for_removal(target, purpose):
    """Atomically detach a target before destructive best-effort cleanup."""
    token = hashlib.sha256(target.encode("utf-8", "surrogateescape")) \
        .hexdigest()[:16]
    quarantine = os.path.join(
        os.path.dirname(target),
        ".nxdoctor-%s-%s-%d" % (purpose, token, os.getpid()))
    try:
        os.lstat(quarantine)
    except FileNotFoundError:
        pass
    except OSError as error:
        raise DoctorError("cleanup quarantine could not be inspected") \
            from error
    else:
        raise Refusal("cleanup quarantine already exists")
    os.rename(target, quarantine)
    return quarantine


def cleanup_quarantine(path):
    """Return None on complete cleanup, or a stable pending reason."""
    try:
        info = os.lstat(path)
        if stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode):
            remove_tree(path)
        elif stat.S_ISREG(info.st_mode) and info.st_nlink == 1:
            os.unlink(path)
        else:
            return "quarantine identity changed"
    except (Refusal, DoctorError) as error:
        return str(error)
    except OSError as error:
        return "cleanup pending: %s" % os.strerror(error.errno or 0)
    return None


def remove_tree(root):
    """Remove a pre-guarded tree without following symlinks."""
    for dirpath, dirnames, filenames in os.walk(root, topdown=False,
                                                followlinks=False):
        for name in filenames:
            if is_owner_data_name(name):
                raise Refusal("owner data reached the unlink path (%s)"
                              % name)
            if dirpath == root and name == GC_CLAIM_NAME:
                # The claim is what makes an interrupted collection resumable,
                # so it outlives every other member and goes just before the
                # root itself.
                continue
            os.unlink(os.path.join(dirpath, name))
        for name in dirnames:
            os.rmdir(os.path.join(dirpath, name))
    claim = os.path.join(root, GC_CLAIM_NAME)
    claimed = os.path.isfile(claim)
    if claimed:
        os.unlink(claim)
    try:
        os.rmdir(root)
    except OSError:
        # The root itself would not go.  Put the claim back before reporting,
        # or the caller is left with an empty directory that carries no
        # `commit` and no claim: garbage nothing would ever agree to collect.
        if claimed:
            create_gc_claim(root, os.path.basename(root))
        raise


@guarded_action("restore-previous")
def action_restore_previous(port, generation_id):
    try:
        _root, parents_exist = inspect_action_directory_chain(
            port, [".nxruntime", "generations"])
    except DoctorError as error:
        return receipt("restore-previous", "refused", str(error),
                       generation=generation_id)
    if not parents_exist:
        return receipt("restore-previous", "refused",
                       "runtime generation parents are absent",
                       generation=generation_id)
    try:
        state_raw = read_bounded(os.path.join(runtime_root(port), "state.json"),
                                 MAX_STATE_BYTES)
    except DoctorError as error:
        return receipt("restore-previous", "refused", str(error),
                       generation=generation_id)
    state, problem = load_state(port)
    if state is None:
        return receipt("restore-previous", "refused",
                       problem or "state.json is absent (legacy install)",
                       generation=generation_id)
    if not (isinstance(generation_id, str)
            and GEN_ID.fullmatch(generation_id)):
        return receipt("restore-previous", "refused",
                       "generation id is not 32 or 64 lowercase hex",
                       generation=generation_id)
    if state.get("previous_healthy") != generation_id:
        return receipt("restore-previous", "refused",
                       "generation is not previous_healthy",
                       generation=generation_id)
    verdict = verify_generation(port, generation_id)
    if verdict["status"] != "complete":
        return receipt("restore-previous", "refused",
                       "previous generation is %s, not complete"
                       % verdict["status"], generation=generation_id)
    if state.get("active") == generation_id and state.get("pending") is None:
        return receipt("restore-previous", "already-clear",
                       generation=generation_id)
    new_state = dict(state)
    new_state["active"] = generation_id
    new_state["pending"] = None
    new_state["activation_seq"] = state["activation_seq"] + 1
    try:
        write_state_atomic(port, new_state, state_raw)
    except DoctorError as error:
        return receipt("restore-previous", "refused", str(error),
                       generation=generation_id)
    except OSError as error:
        # The rollback did not happen; say so instead of dying half way and
        # leaving the caller to guess whether state moved.
        return denied("restore-previous", "state could not be written",
                      error, generation=generation_id)
    return receipt("restore-previous", "ok", generation=generation_id,
                   activation_seq=new_state["activation_seq"])


@guarded_action("discard-staging")
def action_discard_staging(port, relpath):
    try:
        parts = safe_relpath_parts(relpath)
    except DoctorError:
        return receipt("discard-staging", "refused",
                       "unsafe relative path", requested_path=relpath)
    if len(parts) < 3 or parts[0] != ".nxruntime" \
            or not parts[1].startswith("staging"):
        return receipt("discard-staging", "refused",
                       "path is outside the .nxruntime/staging area",
                       requested_path=relpath)
    try:
        _parent, parents_exist = inspect_action_directory_chain(
            port, parts[:-1])
    except DoctorError as error:
        return receipt("discard-staging", "refused", str(error),
                       requested_path=relpath)
    if not parents_exist:
        return receipt("discard-staging", "already-clear", path=relpath,
                       requested_path=relpath)
    try:
        target = join_no_symlink(port, parts)
    except DoctorError as error:
        return receipt("discard-staging", "refused", str(error),
                       requested_path=relpath)
    try:
        info = os.lstat(target)
    except FileNotFoundError:
        return receipt("discard-staging", "already-clear", path=relpath,
                       requested_path=relpath)
    except OSError as error:
        return denied("discard-staging", "staging could not be inspected",
                      error, requested_path=relpath)
    if stat.S_ISLNK(info.st_mode):
        return receipt("discard-staging", "refused", "target is a symlink",
                       requested_path=relpath)
    if is_owner_data_name(parts[-1]):
        return receipt("discard-staging", "refused",
                       "target name matches owner data",
                       requested_path=relpath)
    if stat.S_ISDIR(info.st_mode):
        # a complete generation must never be discarded as staging garbage
        manifest = os.path.join(target, "manifest.json")
        commit = os.path.join(target, "commit")
        if os.path.isfile(manifest) and os.path.isfile(commit):
            return receipt("discard-staging", "refused",
                           "target looks like a committed generation",
                           requested_path=relpath)
        try:
            scan_tree_guard(target)
            quarantine = quarantine_for_removal(target, "discard")
        except Refusal as refusal:
            return receipt("discard-staging", "refused", str(refusal),
                           requested_path=relpath)
        except DoctorError as error:
            return receipt("discard-staging", "refused", str(error),
                           requested_path=relpath)
        except OSError as error:
            return denied("discard-staging",
                          "staging could not be detached", error,
                          requested_path=relpath)
    elif stat.S_ISREG(info.st_mode):
        if info.st_nlink > 1:
            return receipt("discard-staging", "refused",
                           "target is a multiply-linked file",
                           requested_path=relpath)
        try:
            quarantine = quarantine_for_removal(target, "discard")
        except (Refusal, DoctorError) as error:
            return receipt("discard-staging", "refused", str(error),
                           requested_path=relpath)
        except OSError as error:
            return denied("discard-staging",
                          "staging could not be detached", error,
                          requested_path=relpath)
    else:
        return receipt("discard-staging", "refused",
                       "target is not a file or directory",
                       requested_path=relpath)
    pending = cleanup_quarantine(quarantine)
    extra = {"path": relpath, "requested_path": relpath}
    if pending is not None:
        extra["cleanup_pending"] = True
        extra["quarantine_path"] = os.path.relpath(quarantine,
                                                   runtime_root(port))
    return receipt("discard-staging", "ok", **extra)


@guarded_action("complete-gc")
def action_complete_gc(port, generation_id):
    if not (isinstance(generation_id, str)
            and GEN_ID.fullmatch(generation_id)):
        return receipt("complete-gc", "refused",
                       "generation id is not 32 or 64 lowercase hex",
                       generation=generation_id)
    try:
        _runtime, runtime_exists = inspect_action_directory_chain(
            port, [".nxruntime"])
    except DoctorError as error:
        return receipt("complete-gc", "refused", str(error),
                       generation=generation_id)
    if not runtime_exists:
        return receipt("complete-gc", "refused",
                       ".nxruntime directory is absent",
                       generation=generation_id)
    try:
        state_raw = read_bounded(os.path.join(runtime_root(port), "state.json"),
                                 MAX_STATE_BYTES)
    except DoctorError as error:
        return receipt("complete-gc", "refused", str(error),
                       generation=generation_id)
    state, problem = load_state(port)
    if state is None:
        return receipt("complete-gc", "refused",
                       problem or "state.json is absent (legacy install)",
                       generation=generation_id)
    referenced = set()
    for key in ("active", "pending", "previous_healthy",
                "failure_generation"):
        if state.get(key):
            referenced.add(state[key])
    if generation_id in referenced:
        return receipt("complete-gc", "refused",
                       "generation is referenced by state.json",
                       generation=generation_id)
    try:
        root, generations_exist = inspect_action_directory_chain(
            port, [".nxruntime", "generations"])
    except DoctorError as error:
        return receipt("complete-gc", "refused", str(error),
                       generation=generation_id)
    if not generations_exist:
        return receipt("complete-gc", "already-clear",
                       generation=generation_id)
    target = os.path.join(root, generation_id)
    try:
        info = os.lstat(target)
    except FileNotFoundError:
        return receipt("complete-gc", "already-clear",
                       generation=generation_id)
    except OSError as error:
        return denied("complete-gc", "generation could not be inspected",
                      error, generation=generation_id)
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        return receipt("complete-gc", "refused",
                       "generation path is not a plain directory",
                       generation=generation_id)
    # A collection this tool already started is finished on sight.  Without
    # this the recovery below would be unreachable: claiming the tree makes it
    # incomplete, and an incomplete generation is otherwise never collected.
    marker = os.path.join(target, GC_CLAIM_NAME)
    try:
        resuming = inspect_gc_claim(marker, generation_id)
    except Refusal as refusal:
        return receipt("complete-gc", "refused", str(refusal),
                       generation=generation_id)
    except DoctorError as error:
        return receipt("complete-gc", "refused", str(error),
                       generation=generation_id)
    verdict = verify_generation(port, generation_id)
    if not resuming and verdict["status"] not in ("complete", "corrupt"):
        return receipt("complete-gc", "refused",
                       "generation is %s; only complete or corrupt "
                       "unreferenced generations are collected"
                       % verdict["status"], generation=generation_id)
    try:
        if read_bounded(os.path.join(runtime_root(port), "state.json"),
                        MAX_STATE_BYTES) != state_raw:
            return receipt("complete-gc", "refused",
                           "state changed during gc preflight",
                           generation=generation_id)
    except DoctorError as error:
        return receipt("complete-gc", "refused", str(error),
                       generation=generation_id)
    try:
        scan_tree_guard(target)
        if not resuming:
            # Claim the tree BEFORE destroying anything, then drop `commit`.
            # Creation writes `commit` last, so deletion has to remove it
            # first: a delete the filesystem denies half way through then
            # leaves a tree that no reader can mistake for a whole one.
            # Removing `commit` last would leave a gutted generation still
            # wearing its completion marker.
            create_gc_claim(target, generation_id)
            commit_path = os.path.join(target, "commit")
            try:
                commit_info = os.lstat(commit_path)
            except FileNotFoundError:
                commit_info = None
            if commit_info is not None and stat.S_ISREG(commit_info.st_mode):
                if commit_info.st_nlink > 1:
                    raise Refusal("tree contains a multiply-linked file")
                os.unlink(commit_path)
        remove_tree(target)
    except Refusal as refusal:
        return receipt("complete-gc", "refused", str(refusal),
                       generation=generation_id)
    except OSError as error:
        # The claim stays in place, so a later pass finishes the collection.
        return denied("complete-gc", "generation could not be removed",
                      error, generation=generation_id)
    return receipt("complete-gc", "ok", generation=generation_id,
                   status_before="gc-interrupted" if resuming
                   else verdict["status"])


@guarded_action("clear-lock", reserve_fallback=False)
def action_clear_lock(port, pid):
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        return receipt("clear-lock", "refused",
                       "requested pid is invalid", pid=pid)
    _flock, lock_dir, name = lock_paths(port)
    try:
        base = os.path.dirname(lock_dir)
        if not inspect_plain_directory(base, "lock base directory"):
            return receipt("clear-lock", "already-clear", lock=name,
                           pid=pid)
    except DoctorError as error:
        return receipt("clear-lock", "refused", str(error), pid=pid)
    try:
        info = os.lstat(lock_dir)
    except FileNotFoundError:
        return receipt("clear-lock", "already-clear", lock=name, pid=pid)
    except OSError as error:
        return denied("clear-lock", "lock could not be inspected", error,
                      lock=name, pid=pid)
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        return receipt("clear-lock", "refused",
                       "lock fallback path is not a plain directory", pid=pid)
    owner = read_lock_owner(lock_dir)
    if owner is None:
        return receipt("clear-lock", "refused",
                       "lock owner record is absent or invalid",
                       pid=pid)
    owner_pid = owner["pid"]
    recorded = owner["starttime"]
    if owner_pid != pid:
        return receipt("clear-lock", "refused",
                       "owner pid does not match the requested pid", pid=pid)
    try:
        current = proc_starttime(pid)
    except DoctorError as error:
        return receipt("clear-lock", "refused", str(error), pid=pid)
    if current is not None:
        if recorded is None:
            return receipt("clear-lock", "refused",
                           "owner pid exists and token record cannot prove "
                           "starttime reuse", pid=pid)
        if recorded == current:
            return receipt("clear-lock", "refused",
                           "owner process is still alive", pid=pid)
        # starttime differs: the pid was reused by another process
    try:
        scan_tree_guard(lock_dir)
        quarantine = quarantine_for_removal(lock_dir, "cleared-lock")
    except Refusal as refusal:
        return receipt("clear-lock", "refused", str(refusal), pid=pid)
    except DoctorError as error:
        return receipt("clear-lock", "refused", str(error), pid=pid)
    except OSError as error:
        return denied("clear-lock", "lock could not be detached", error,
                      lock=name, pid=pid)
    pending = cleanup_quarantine(quarantine)
    extra = {"lock": name, "pid": pid}
    if pending is not None:
        extra["cleanup_pending"] = True
        extra["quarantine"] = os.path.basename(quarantine)
    return receipt("clear-lock", "ok", **extra)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="nxdoctor",
        description="Read-only diagnostician for V3 port directories.")
    parser.add_argument("port", help="port directory to inspect")
    parser.add_argument("--json", action="store_true",
                        help="emit the nx-doctor-report-v1 JSON report")
    parser.add_argument("--restore-previous", action="store_true",
                        help="reactivate the previous_healthy generation")
    parser.add_argument("--discard-staging", action="store_true",
                        help="remove one path inside .nxruntime/staging*")
    parser.add_argument("--complete-gc", action="store_true",
                        help="remove one unreferenced generation")
    parser.add_argument("--clear-lock", action="store_true",
                        help="remove the fallback lock dir of a dead owner")
    parser.add_argument("--generation", metavar="ID",
                        help="target generation id (32 or 64 lowercase hex)")
    parser.add_argument("--path", metavar="RELPATH",
                        help="target path relative to the port directory")
    parser.add_argument("--pid", type=int, metavar="PID",
                        help="owner pid recorded in the lock")
    args = parser.parse_args(argv)

    port = args.port.rstrip("/") or args.port
    if os.path.islink(port):
        print("nxdoctor: the port directory must not be a symlink",
              file=sys.stderr)
        return 2
    if not os.path.isdir(port):
        print("nxdoctor: not a directory", file=sys.stderr)
        return 2

    actions = [name for name, flag in (
        ("restore-previous", args.restore_previous),
        ("discard-staging", args.discard_staging),
        ("complete-gc", args.complete_gc),
        ("clear-lock", args.clear_lock)) if flag]
    if len(actions) > 1:
        print("nxdoctor: at most one action per invocation",
              file=sys.stderr)
        return 2

    try:
        if actions:
            action = actions[0]
            if action == "restore-previous":
                if not args.generation:
                    print("nxdoctor: --restore-previous requires "
                          "--generation", file=sys.stderr)
                    return 2
                return action_restore_previous(port, args.generation)
            if action == "discard-staging":
                if not args.path:
                    print("nxdoctor: --discard-staging requires --path",
                          file=sys.stderr)
                    return 2
                return action_discard_staging(port, args.path)
            if action == "complete-gc":
                if not args.generation:
                    print("nxdoctor: --complete-gc requires --generation",
                          file=sys.stderr)
                    return 2
                return action_complete_gc(port, args.generation)
            if action == "clear-lock":
                if args.pid is None:
                    print("nxdoctor: --clear-lock requires --pid",
                          file=sys.stderr)
                    return 2
                return action_clear_lock(port, args.pid)
        report = build_report(port)
        if args.json:
            print(json.dumps(report, sort_keys=True))
        else:
            print_human(report)
        return 0
    except DoctorError as error:
        print("nxdoctor: %s" % error, file=sys.stderr)
        return 2
    except (OSError, RecursionError) as error:
        print("nxdoctor: operation failed closed (%s)"
              % type(error).__name__, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
