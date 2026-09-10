#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Capture and validate nxdoctor recovery evidence without network access.

The wrapper executes exactly one explicit nxdoctor action, records canonical
before/after snapshots and seals the payload with SHA-256. Host fixtures are
always HOST_FIXTURE. This version rejects PHYSICAL because a caller-supplied
context plus its own SHA-256 is not an external trust anchor; there is
deliberately no boolean ``--physical`` switch or environment override.
"""

import argparse
import base64
import hashlib
import json
import os
import platform
import re
import secrets
import stat
import subprocess
import sys
import time

import nxdoctor as doctor


SCHEMA = "nx-doctor-recovery-evidence-v1"
SCHEMA_VERSION = 1
PHYSICAL_CONTEXT_SCHEMA = "nx-doctor-physical-context-v1"
TOOL_VERSION = "0.3.0"
MAX_JSON_BYTES = 8 * 1024 * 1024
MAX_CONTEXT_BYTES = 64 * 1024
MAX_SNAPSHOT_BYTES = 3 * 1024 * 1024
MAX_TREE_ENTRIES = 200000
ACTION_TIMEOUT_SECONDS = 60
PHYSICAL_CONTEXT_LIFETIME_SECONDS = 30 * 60

HASH64 = re.compile(r"^[0-9a-f]{64}$")
RUN_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
HEALTH_RUN_ID = re.compile(r"^[A-Za-z0-9._-]{1,192}$")
IPV4 = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")
SUPPORTED_PHYSICAL_MACHINES = {"aarch64", "arm64", "armv7l", "armv8l"}
PHYSICAL_TRUST_AVAILABLE = False
ACTIONS = {
    "restore-previous": ("--restore-previous", "generation"),
    "discard-staging": ("--discard-staging", "path"),
    "complete-gc": ("--complete-gc", "generation"),
    "clear-lock": ("--clear-lock", "pid"),
}


class EvidenceError(Exception):
    """Fail-closed evidence error carrying a public, path-free reason."""


def canonical_bytes(value):
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=True).encode("utf-8")
    except (TypeError, ValueError, RecursionError) as error:
        raise EvidenceError("value cannot be represented canonically") \
            from error


def sha256_bytes(value):
    return hashlib.sha256(value).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as error:
        raise EvidenceError("required regular file is unreadable") from error
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode):
            raise EvidenceError("required path is not a regular file")
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            digest.update(block)
        after = os.fstat(fd)
        if (before.st_dev, before.st_ino, before.st_size) != (
                after.st_dev, after.st_ino, after.st_size):
            raise EvidenceError("file changed while it was hashed")
    except OSError as error:
        raise EvidenceError("required file could not be hashed") from error
    finally:
        try:
            os.close(fd)
        except OSError as error:
            raise EvidenceError("required file could not be closed") from error
    return digest.hexdigest()


def strict_json_bytes(raw, label, maximum=MAX_JSON_BYTES):
    if len(raw) > maximum:
        raise EvidenceError("%s exceeds the size limit" % label)
    if raw.startswith(b"\xef\xbb\xbf"):
        raise EvidenceError("%s contains a UTF-8 BOM" % label)

    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise EvidenceError("%s contains a duplicate key" % label)
            result[key] = value
        return result

    def reject_constant(_value):
        raise EvidenceError("%s contains a non-JSON constant" % label)

    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=unique,
                          parse_constant=reject_constant)
    except EvidenceError:
        raise
    except (UnicodeError, ValueError, RecursionError) as error:
        raise EvidenceError("%s is malformed or truncated" % label) from error


def read_regular(path, maximum, label):
    try:
        info = os.lstat(path)
    except OSError as error:
        raise EvidenceError("%s is absent" % label) from error
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise EvidenceError("%s is not a regular non-symlink file" % label)
    if info.st_size > maximum:
        raise EvidenceError("%s exceeds the size limit" % label)
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
        try:
            opened = os.fstat(fd)
            if not stat.S_ISREG(opened.st_mode) \
                    or (info.st_dev, info.st_ino, info.st_size) != (
                        opened.st_dev, opened.st_ino, opened.st_size):
                raise EvidenceError("%s changed before it was read" % label)
            chunks = []
            total = 0
            while total <= maximum:
                block = os.read(fd, min(1 << 20, maximum + 1 - total))
                if not block:
                    break
                chunks.append(block)
                total += len(block)
            closed = os.fstat(fd)
            if (opened.st_dev, opened.st_ino, opened.st_size) != (
                    closed.st_dev, closed.st_ino, closed.st_size):
                raise EvidenceError("%s changed while it was read" % label)
        finally:
            try:
                os.close(fd)
            except OSError as error:
                raise EvidenceError("%s could not be closed" % label) \
                    from error
    except OSError as error:
        raise EvidenceError("%s is unreadable" % label) from error
    value = b"".join(chunks)
    if len(value) > maximum:
        raise EvidenceError("%s grew beyond the size limit" % label)
    return value


def decode_base64(value, label, maximum):
    if not isinstance(value, str):
        raise EvidenceError("%s is not base64 text" % label)
    try:
        raw = base64.b64decode(value.encode("ascii"), validate=True)
    except (UnicodeError, ValueError) as error:
        raise EvidenceError("%s is malformed base64" % label) from error
    if len(raw) > maximum:
        raise EvidenceError("%s exceeds the decoded size limit" % label)
    return raw


def validate_run_id(value):
    if not isinstance(value, str) or not RUN_ID.fullmatch(value) \
            or IPV4.search(value):
        raise EvidenceError("run id is invalid")
    return value


def validate_hash(value, label):
    if not isinstance(value, str) or not HASH64.fullmatch(value):
        raise EvidenceError("%s is not a complete SHA-256" % label)
    return value


def clean_relative(value, label):
    if not isinstance(value, str) or not value or value.startswith("/"):
        raise EvidenceError("%s is not a safe relative path" % label)
    if "\\" in value or "\x00" in value or "\n" in value \
            or "\r" in value or IPV4.search(value):
        raise EvidenceError("%s is not a safe relative path" % label)
    if any(part in ("", ".", "..") for part in value.split("/")):
        raise EvidenceError("%s is not a safe relative path" % label)
    return value


def validate_target(action, target):
    if action not in ACTIONS:
        raise EvidenceError("unknown action")
    field = ACTIONS[action][1]
    if not isinstance(target, dict) or set(target) != {field}:
        raise EvidenceError("action target shape is invalid")
    value = target[field]
    if field == "generation":
        if not isinstance(value, str) or not doctor.GEN_ID.fullmatch(value):
            raise EvidenceError("generation target is invalid")
    elif field == "path":
        clean_relative(value, "staging target")
        try:
            parts = doctor.safe_relpath_parts(value)
        except doctor.DoctorError as error:
            raise EvidenceError("staging target is hostile") from error
        if len(parts) < 3 or parts[0] != ".nxruntime" \
                or not parts[1].startswith("staging"):
            raise EvidenceError("staging target is outside staging")
    elif not isinstance(value, int) or isinstance(value, bool) \
            or value <= 0 or value > 2147483647:
        raise EvidenceError("pid target is invalid")
    return target


def validate_port(port):
    try:
        info = os.lstat(port)
    except OSError as error:
        raise EvidenceError("port directory is absent") from error
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise EvidenceError("port must be a non-symlink directory")


def _file_record(path, relpath, info):
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as error:
        raise EvidenceError("snapshot file is unreadable") from error
    digest = hashlib.sha256()
    try:
        opened = os.fstat(fd)
        if not stat.S_ISREG(opened.st_mode):
            raise EvidenceError("snapshot entry changed type")
        if (info.st_dev, info.st_ino, info.st_size) != (
                opened.st_dev, opened.st_ino, opened.st_size):
            raise EvidenceError("snapshot entry changed before it was read")
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            digest.update(block)
        closed = os.fstat(fd)
        if (opened.st_dev, opened.st_ino, opened.st_size) != (
                closed.st_dev, closed.st_ino, closed.st_size):
            raise EvidenceError("snapshot entry changed while read")
    except OSError as error:
        raise EvidenceError("snapshot file could not be hashed") from error
    finally:
        try:
            os.close(fd)
        except OSError as error:
            raise EvidenceError("snapshot file could not be closed") from error
    return {"path": relpath, "type": "file", "bytes": info.st_size,
            "mode": "%04o" % stat.S_IMODE(info.st_mode),
            "links": info.st_nlink, "sha256": digest.hexdigest()}


def scan_tree(root):
    """Return a canonical, non-following inventory relative to *root*."""
    try:
        root_info = os.lstat(root)
    except OSError as error:
        raise EvidenceError("snapshot root is inaccessible") from error
    if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
        raise EvidenceError("snapshot root is not a plain directory")
    records = []
    def walk_error(error):
        raise EvidenceError("snapshot directory is inaccessible") from error

    for dirpath, dirnames, filenames in os.walk(
            root, topdown=True, followlinks=False, onerror=walk_error):
        dirnames.sort()
        filenames.sort()
        for name in list(dirnames) + list(filenames):
            path = os.path.join(dirpath, name)
            relpath = os.path.relpath(path, root).replace(os.sep, "/")
            clean_relative(relpath, "snapshot entry")
            try:
                info = os.lstat(path)
            except OSError as error:
                raise EvidenceError("snapshot entry disappeared") from error
            if stat.S_ISLNK(info.st_mode):
                try:
                    target = os.readlink(path).encode("utf-8", "surrogateescape")
                except OSError as error:
                    raise EvidenceError("snapshot symlink is unreadable") from error
                records.append({"path": relpath, "type": "symlink",
                                "target_bytes": len(target),
                                "target_sha256": sha256_bytes(target)})
                if name in dirnames:
                    dirnames.remove(name)
            elif stat.S_ISDIR(info.st_mode):
                records.append({"path": relpath, "type": "dir",
                                "mode": "%04o" % stat.S_IMODE(info.st_mode)})
            elif stat.S_ISREG(info.st_mode):
                records.append(_file_record(path, relpath, info))
            else:
                raise EvidenceError("snapshot contains a special file")
            if len(records) > MAX_TREE_ENTRIES:
                raise EvidenceError("snapshot contains too many entries")
    records.sort(key=lambda item: item["path"])
    return records


def tree_section(entries):
    return {"entries": entries,
            "sha256": sha256_bytes(canonical_bytes(entries))}


def records_under(entries, prefix):
    return [item for item in entries
            if item["path"] == prefix or item["path"].startswith(prefix + "/")]


def find_entry(entries, path):
    for item in entries:
        if item["path"] == path:
            return item
    return None


STATE_BASE_KEYS = {"schema", "schema_version", "active", "pending",
                   "previous_healthy", "activation_seq",
                   "prehealth_failures"}
STATE_HEALTH_KEYS = {"failure_generation", "last_health_run_id"}


def parse_state_snapshot(raw):
    value = strict_json_bytes(raw, "state snapshot", doctor.MAX_STATE_BYTES)
    if not isinstance(value, dict):
        raise EvidenceError("state snapshot is not an object")
    schema = value.get("schema")
    expected_version = doctor.STATE_SCHEMAS.get(schema)
    if expected_version is None or value.get("schema_version") != expected_version:
        raise EvidenceError("state snapshot schema is invalid")
    keys = set(value)
    if schema == "nxruntime-state-v1":
        if keys != STATE_BASE_KEYS:
            raise EvidenceError("state-v1 fields are invalid")
    elif keys != STATE_BASE_KEYS | STATE_HEALTH_KEYS:
        raise EvidenceError("state-v2 fields are invalid")
    normalized = dict(value)
    for field in ("active", "pending", "previous_healthy"):
        item = normalized[field]
        if item in (None, "null"):
            normalized[field] = None
        elif not isinstance(item, str) or not doctor.GEN_ID.fullmatch(item):
            raise EvidenceError("state snapshot generation is invalid")
    for field in ("activation_seq", "prehealth_failures"):
        item = normalized[field]
        if not isinstance(item, int) or isinstance(item, bool) or item < 0:
            raise EvidenceError("state snapshot counter is invalid")
    if STATE_HEALTH_KEYS <= keys:
        failure = normalized["failure_generation"]
        if failure in (None, "null"):
            normalized["failure_generation"] = None
        elif not isinstance(failure, str) \
                or not doctor.GEN_ID.fullmatch(failure):
            raise EvidenceError("state failure generation is invalid")
        health_run = normalized["last_health_run_id"]
        if health_run != "null" \
                and (not isinstance(health_run, str)
                     or not HEALTH_RUN_ID.fullmatch(health_run)
                     or IPV4.search(health_run)):
            raise EvidenceError("state health run id is invalid")
    return normalized


def state_section(port, runtime_entries):
    state_entry = find_entry(runtime_entries, "state.json")
    state_path = os.path.join(port, ".nxruntime", "state.json")
    if state_entry is None:
        if os.path.lexists(state_path):
            raise EvidenceError("state appeared while snapshot was captured")
        return {"present": False, "document": None, "sha256": None,
                "raw_base64": None}
    if state_entry.get("type") != "file":
        raise EvidenceError("state snapshot is not a regular file")
    raw = read_regular(state_path, doctor.MAX_STATE_BYTES, "state snapshot")
    if sha256_bytes(raw) != state_entry["sha256"]:
        raise EvidenceError("state changed while snapshot was captured")
    document = parse_state_snapshot(raw)
    return {"present": True, "document": document,
            "sha256": state_entry["sha256"],
            "raw_base64": base64.b64encode(raw).decode("ascii")}


def generation_sections(runtime_entries):
    names = set()
    for item in runtime_entries:
        parts = item["path"].split("/")
        if len(parts) >= 2 and parts[0] == "generations":
            names.add(parts[1])
    result = {}
    for name in sorted(names):
        clean_relative(name, "generation entry")
        if not doctor.GEN_ID.fullmatch(name):
            raise EvidenceError("generation index contains an invalid id")
        prefix = "generations/" + name
        entries = records_under(runtime_entries, prefix)
        commit_entry = find_entry(runtime_entries, prefix + "/commit")
        claim_entry = find_entry(runtime_entries,
                                 prefix + "/" + doctor.GC_CLAIM_NAME)
        claim = None
        if claim_entry is not None:
            expected = (name + "\n").encode("ascii")
            if claim_entry.get("type") != "file" \
                    or claim_entry.get("links") != 1 \
                    or claim_entry.get("bytes") != len(expected) \
                    or claim_entry.get("sha256") != sha256_bytes(expected) \
                    or claim_entry.get("mode") not in ("0600", "0644",
                                                       "0777"):
                raise EvidenceError("generation gc claim is invalid")
            claim = {key: claim_entry[key] for key in (
                "bytes", "mode", "links", "sha256")}
        result[name] = {
            "tree_sha256": sha256_bytes(canonical_bytes(entries)),
            "commit": bool(commit_entry
                           and commit_entry.get("type") == "file"),
            "claim": claim,
        }
    return result


def staging_sections(runtime_entries):
    names = set()
    for item in runtime_entries:
        first = item["path"].split("/", 1)[0]
        if first.startswith("staging"):
            names.add(first)
    return {name: sha256_bytes(
                canonical_bytes(records_under(runtime_entries, name)))
            for name in sorted(names)}


def is_owner_record(record):
    return any(doctor.is_owner_data_name(part)
               for part in record["path"].split("/"))


def parse_lock_owner_snapshot(raw):
    try:
        tokens = raw.decode("ascii").split()
    except UnicodeError as error:
        raise EvidenceError("lock owner is not ASCII") from error
    if len(tokens) == 2 \
            and all(re.fullmatch(r"[0-9]+", token) for token in tokens):
        pid, recorded = (int(token) for token in tokens)
        if pid <= 0 or pid > 2147483647 \
                or recorded <= 0 or recorded > 9223372036854775807:
            raise EvidenceError("lock owner pid/starttime is invalid")
        return {"format": "pid-starttime", "pid": pid,
                "recorded_starttime": recorded, "token_sha256": None}
    if len(tokens) == 2 and tokens[0].startswith("pid=") \
            and tokens[1].startswith("token="):
        pid_text = tokens[0][len("pid="):]
        token = tokens[1][len("token="):]
        if not re.fullmatch(r"[0-9]+", pid_text) \
                or not doctor.LOCK_TOKEN.fullmatch(token) \
                or IPV4.search(token):
            raise EvidenceError("lock owner token shape is invalid")
        pid = int(pid_text)
        if pid <= 0 or pid > 2147483647:
            raise EvidenceError("lock owner pid is invalid")
        return {"format": "pid-token", "pid": pid,
                "recorded_starttime": None,
                "token_sha256": sha256_bytes(token.encode("ascii"))}
    raise EvidenceError("lock owner shape is invalid")


def lock_section(port):
    _flock, lock_dir, _name = doctor.lock_paths(port)
    try:
        info = os.lstat(lock_dir)
    except FileNotFoundError:
        return {"present": False, "type": None,
                "tree": tree_section([]), "owner": None,
                "owner_raw_base64": None}
    except OSError as error:
        raise EvidenceError("lock path is inaccessible") from error
    if stat.S_ISLNK(info.st_mode):
        target = os.readlink(lock_dir).encode("utf-8", "surrogateescape")
        entry = {"path": "lock", "type": "symlink",
                 "target_bytes": len(target),
                 "target_sha256": sha256_bytes(target)}
        return {"present": True, "type": "symlink",
                "tree": tree_section([entry]), "owner": None,
                "owner_raw_base64": None}
    if not stat.S_ISDIR(info.st_mode):
        return {"present": True, "type": "other",
                "tree": tree_section([]), "owner": None,
                "owner_raw_base64": None}
    entries = scan_tree(lock_dir)
    owner_entry = find_entry(entries, "owner")
    owner_raw = None
    parsed_owner = None
    if owner_entry is not None:
        if owner_entry.get("type") != "file":
            raise EvidenceError("lock owner is not a regular file")
        owner_raw = read_regular(os.path.join(lock_dir, "owner"), 4096,
                                 "lock owner")
        if sha256_bytes(owner_raw) != owner_entry["sha256"]:
            raise EvidenceError("lock owner changed while snapshot was captured")
        parsed_owner = parse_lock_owner_snapshot(owner_raw)
    if scan_tree(lock_dir) != entries:
        raise EvidenceError("lock changed while snapshot was captured")
    owner = None
    if parsed_owner is not None:
        owner_pid = parsed_owner["pid"]
        recorded = parsed_owner["recorded_starttime"]
        current = doctor.proc_starttime(owner_pid)
        owner = {"pid": owner_pid,
                 "owner_format": parsed_owner["format"],
                 "recorded_starttime": recorded,
                 "token_sha256": parsed_owner["token_sha256"],
                 "current_starttime": current,
                 "liveness": "dead" if current is None else
                             "alive" if recorded is None or current == recorded
                             else "reused"}
    return {"present": True, "type": "dir",
            "tree": tree_section(entries), "owner": owner,
            "owner_raw_base64": (base64.b64encode(owner_raw).decode("ascii")
                                 if owner_raw is not None else None)}


def take_snapshot(port):
    validate_port(port)
    all_entries = scan_tree(port)
    runtime_entries = []
    protected_entries = []
    for item in all_entries:
        path = item["path"]
        if path == ".nxruntime":
            continue
        if path.startswith(".nxruntime/"):
            copy = dict(item)
            copy["path"] = path[len(".nxruntime/"):]
            runtime_entries.append(copy)
        else:
            protected_entries.append(item)
    runtime_entries.sort(key=lambda item: item["path"])
    protected_entries.sort(key=lambda item: item["path"])
    owner_entries = [item for item in protected_entries
                     if is_owner_record(item)]
    return {
        "state": state_section(port, runtime_entries),
        "runtime": tree_section(runtime_entries),
        "generations": generation_sections(runtime_entries),
        "staging": staging_sections(runtime_entries),
        "lock": lock_section(port),
        "protected": tree_section(protected_entries),
        "owner_data": tree_section(owner_entries),
    }


def port_identity_sha256(port):
    validate_port(port)
    info = os.stat(port, follow_symlinks=False)
    value = "%d:%d:%s" % (info.st_dev, info.st_ino, os.path.basename(port))
    return sha256_bytes(value.encode("utf-8", "surrogateescape"))


def system_file_hash(path):
    return sha256_file(path)


def create_physical_context(port, run_id, doctor_path, evidence_path):
    if not PHYSICAL_TRUST_AVAILABLE:
        raise EvidenceError(
            "PHYSICAL evidence is PENDING an external trust anchor")
    validate_run_id(run_id)
    validate_port(port)
    machine = platform.machine().lower()
    if platform.system() != "Linux" or machine not in SUPPORTED_PHYSICAL_MACHINES:
        raise EvidenceError("physical context requires a supported ARM Linux target")
    now = int(time.time())
    return {
        "schema": PHYSICAL_CONTEXT_SCHEMA,
        "schema_version": 1,
        "run_id": run_id,
        "created_unix": now,
        "expires_unix": now + PHYSICAL_CONTEXT_LIFETIME_SECONDS,
        "machine": machine,
        "boot_id_sha256": system_file_hash("/proc/sys/kernel/random/boot_id"),
        "os_release_sha256": system_file_hash("/etc/os-release"),
        "port_identity_sha256": port_identity_sha256(port),
        "doctor_sha256": sha256_file(doctor_path),
        "evidence_tool_sha256": sha256_file(evidence_path),
        "nonce": secrets.token_hex(16),
    }


def validate_physical_context(path, expected_sha256, port, run_id,
                              doctor_path, evidence_path):
    validate_hash(expected_sha256, "physical context hash")
    raw = read_regular(path, MAX_CONTEXT_BYTES, "physical context")
    if sha256_bytes(raw) != expected_sha256:
        raise EvidenceError("physical context hash diverges")
    context = strict_json_bytes(raw, "physical context", MAX_CONTEXT_BYTES)
    required = {"schema", "schema_version", "run_id", "created_unix",
                "expires_unix", "machine", "boot_id_sha256",
                "os_release_sha256", "port_identity_sha256",
                "doctor_sha256", "evidence_tool_sha256", "nonce"}
    if not isinstance(context, dict) or set(context) != required:
        raise EvidenceError("physical context fields are incomplete")
    if context["schema"] != PHYSICAL_CONTEXT_SCHEMA \
            or type(context["schema_version"]) is not int \
            or context["schema_version"] != 1:
        raise EvidenceError("physical context schema is invalid")
    if context["run_id"] != run_id:
        raise EvidenceError("physical context is stale for this run")
    now = int(time.time())
    created = context["created_unix"]
    expires = context["expires_unix"]
    if not isinstance(created, int) or isinstance(created, bool) \
            or not isinstance(expires, int) or isinstance(expires, bool) \
            or created > now + 30 or expires < now or expires <= created \
            or expires - created > PHYSICAL_CONTEXT_LIFETIME_SECONDS:
        raise EvidenceError("physical context is stale")
    machine = platform.machine().lower()
    if platform.system() != "Linux" \
            or machine not in SUPPORTED_PHYSICAL_MACHINES \
            or context["machine"] != machine:
        raise EvidenceError("physical context machine diverges")
    checks = {
        "boot_id_sha256": system_file_hash("/proc/sys/kernel/random/boot_id"),
        "os_release_sha256": system_file_hash("/etc/os-release"),
        "port_identity_sha256": port_identity_sha256(port),
        "doctor_sha256": sha256_file(doctor_path),
        "evidence_tool_sha256": sha256_file(evidence_path),
    }
    for field, expected in checks.items():
        validate_hash(context[field], field)
        if context[field] != expected:
            raise EvidenceError("physical context %s diverges" % field)
    if not isinstance(context["nonce"], str) \
            or not re.fullmatch(r"[0-9a-f]{32}", context["nonce"]):
        raise EvidenceError("physical context nonce is invalid")
    return {
        "context_sha256": expected_sha256,
        "created_unix": created,
        "expires_unix": expires,
        "machine": machine,
        "nonce_sha256": sha256_bytes(context["nonce"].encode("ascii")),
        "boot_id_sha256": context["boot_id_sha256"],
        "os_release_sha256": context["os_release_sha256"],
        "port_identity_sha256": context["port_identity_sha256"],
        "doctor_sha256": context["doctor_sha256"],
        "evidence_tool_sha256": context["evidence_tool_sha256"],
    }


def validate_saved_physical_context(path, expected_sha256, identity):
    """Reopen the separately pinned context during offline validation."""
    validate_hash(expected_sha256, "expected physical context hash")
    raw = read_regular(path, MAX_CONTEXT_BYTES, "saved physical context")
    if sha256_bytes(raw) != expected_sha256:
        raise EvidenceError("saved physical context hash diverges")
    context = strict_json_bytes(raw, "saved physical context",
                                MAX_CONTEXT_BYTES)
    required = {"schema", "schema_version", "run_id", "created_unix",
                "expires_unix", "machine", "boot_id_sha256",
                "os_release_sha256", "port_identity_sha256",
                "doctor_sha256", "evidence_tool_sha256", "nonce"}
    if not isinstance(context, dict) or set(context) != required \
            or context["schema"] != PHYSICAL_CONTEXT_SCHEMA \
            or type(context["schema_version"]) is not int \
            or context["schema_version"] != 1:
        raise EvidenceError("saved physical context shape is invalid")
    if context["run_id"] != identity["run_id"]:
        raise EvidenceError("saved physical context is stale for this run")
    if type(context["created_unix"]) is not int \
            or type(context["expires_unix"]) is not int \
            or context["expires_unix"] <= context["created_unix"] \
            or context["expires_unix"] - context["created_unix"] > \
            PHYSICAL_CONTEXT_LIFETIME_SECONDS:
        raise EvidenceError("saved physical context timing is invalid")
    if context["machine"] not in SUPPORTED_PHYSICAL_MACHINES:
        raise EvidenceError("saved physical context machine is invalid")
    for field in ("boot_id_sha256", "os_release_sha256",
                  "port_identity_sha256", "doctor_sha256",
                  "evidence_tool_sha256"):
        validate_hash(context[field], field)
    if context["doctor_sha256"] != identity["doctor_sha256"] \
            or context["evidence_tool_sha256"] != \
            identity["evidence_tool_sha256"]:
        raise EvidenceError("saved physical context source hashes diverge")
    if not isinstance(context["nonce"], str) \
            or not re.fullmatch(r"[0-9a-f]{32}", context["nonce"]):
        raise EvidenceError("saved physical context nonce is invalid")
    projection = {
        "context_sha256": expected_sha256,
        "created_unix": context["created_unix"],
        "expires_unix": context["expires_unix"],
        "machine": context["machine"],
        "nonce_sha256": sha256_bytes(context["nonce"].encode("ascii")),
        "boot_id_sha256": context["boot_id_sha256"],
        "os_release_sha256": context["os_release_sha256"],
        "port_identity_sha256": context["port_identity_sha256"],
        "doctor_sha256": context["doctor_sha256"],
        "evidence_tool_sha256": context["evidence_tool_sha256"],
    }
    return projection


def command_for(action, target, doctor_path, port):
    flag, field = ACTIONS[action]
    value = target[field]
    command = [sys.executable, doctor_path, port, flag]
    if field == "generation":
        command.extend(["--generation", value])
    elif field == "path":
        command.extend(["--path", value])
    else:
        command.extend(["--pid", str(value)])
    return command


def parse_receipt(stdout):
    try:
        text = stdout.decode("utf-8")
    except UnicodeError:
        return None, "stdout-not-utf8", 0
    lines = [line for line in text.splitlines() if line.strip()]
    if len(lines) != 1:
        return None, "receipt-count-not-one", len(lines)
    try:
        receipt = strict_json_bytes(lines[0].encode("utf-8"),
                                    "action receipt", MAX_CONTEXT_BYTES)
    except EvidenceError:
        return None, "receipt-malformed", 1
    if not isinstance(receipt, dict):
        return None, "receipt-not-object", 1
    return receipt, "parsed", 1


def execution_section(returncode, stdout, stderr, timed_out):
    receipt, parse_status, line_count = parse_receipt(stdout)
    return {
        "returncode": returncode,
        "timed_out": timed_out,
        "stdout_bytes": len(stdout),
        "stdout_sha256": sha256_bytes(stdout),
        "stdout_nonempty_lines": line_count,
        "stderr_bytes": len(stderr),
        "stderr_sha256": sha256_bytes(stderr),
        "receipt_parse": parse_status,
        "receipt": receipt,
    }


def seal_payload(payload):
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "payload": payload,
        "integrity": {
            "algorithm": "sha256",
            "payload_sha256": sha256_bytes(canonical_bytes(payload)),
        },
    }


def capture_action(port, action, target, run_id, physical_context=None,
                   physical_context_sha256=None):
    validate_port(port)
    validate_run_id(run_id)
    validate_target(action, target)
    here = os.path.dirname(os.path.abspath(__file__))
    doctor_path = os.path.join(here, "nxdoctor.py")
    evidence_path = os.path.abspath(__file__)
    doctor_hash_before = sha256_file(doctor_path)
    evidence_hash_before = sha256_file(evidence_path)
    environment = "HOST_FIXTURE"
    context = None
    if physical_context is not None or physical_context_sha256 is not None:
        if not PHYSICAL_TRUST_AVAILABLE:
            raise EvidenceError(
                "PHYSICAL evidence is PENDING an external trust anchor")
        if not physical_context or not physical_context_sha256:
            raise EvidenceError("physical context path and hash are both required")
        context = validate_physical_context(
            physical_context, physical_context_sha256, port, run_id,
            doctor_path, evidence_path)
        environment = "PHYSICAL"

    before = take_snapshot(port)
    if len(canonical_bytes(before)) > MAX_SNAPSHOT_BYTES:
        raise EvidenceError("snapshot is too large; action was not executed")
    command = command_for(action, target, doctor_path, port)
    child_env = {"PATH": os.defpath, "LANG": "C", "LC_ALL": "C"}
    if os.environ.get("XDG_RUNTIME_DIR"):
        child_env["XDG_RUNTIME_DIR"] = os.environ["XDG_RUNTIME_DIR"]
    started_unix = int(time.time())
    started_ns = time.monotonic_ns()
    timed_out = False
    try:
        completed = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=child_env, timeout=ACTION_TIMEOUT_SECONDS, check=False)
        returncode = completed.returncode
        stdout = completed.stdout
        stderr = completed.stderr
    except subprocess.TimeoutExpired as error:
        timed_out = True
        returncode = None
        stdout = error.stdout or b""
        stderr = error.stderr or b""
    duration_ns = time.monotonic_ns() - started_ns
    capture_error = None
    try:
        after = take_snapshot(port)
        if len(canonical_bytes(after)) > MAX_SNAPSHOT_BYTES:
            after = None
            capture_error = "snapshot-after-exceeded-limit"
    except EvidenceError:
        after = None
        capture_error = "snapshot-after-failed"
    execution = execution_section(returncode, stdout, stderr, timed_out)
    if len(stdout) > MAX_CONTEXT_BYTES or len(stderr) > MAX_CONTEXT_BYTES:
        capture_error = "action-output-exceeded-limit"
    if sha256_file(doctor_path) != doctor_hash_before \
            or sha256_file(evidence_path) != evidence_hash_before:
        capture_error = "source-changed-during-action"
    payload = {
        "identity": {
            "action": action,
            "target": target,
            "run_id": run_id,
            "environment": environment,
            "tool_version": TOOL_VERSION,
            "doctor_sha256": doctor_hash_before,
            "evidence_tool_sha256": evidence_hash_before,
            "physical_context": context,
        },
        "timing": {
            "started_unix": started_unix,
            "finished_unix": int(time.time()),
            "duration_monotonic_ns": duration_ns,
        },
        "snapshot_before": before,
        "snapshot_after": after,
        "execution": execution,
        "capture_error": capture_error,
    }
    return seal_payload(payload)


def validate_tree(section, label):
    if not isinstance(section, dict) or set(section) != {"entries", "sha256"}:
        raise EvidenceError("%s tree shape is invalid" % label)
    validate_hash(section["sha256"], "%s tree hash" % label)
    entries = section["entries"]
    if not isinstance(entries, list) or len(entries) > MAX_TREE_ENTRIES:
        raise EvidenceError("%s entries are invalid" % label)
    previous = None
    for item in entries:
        if not isinstance(item, dict) or "path" not in item \
                or "type" not in item:
            raise EvidenceError("%s entry shape is invalid" % label)
        path = clean_relative(item["path"], "%s entry" % label)
        if previous is not None and path <= previous:
            raise EvidenceError("%s entries are duplicated or unsorted" % label)
        previous = path
        kind = item["type"]
        if kind == "file":
            expected = {"path", "type", "bytes", "mode", "links", "sha256"}
            if set(item) != expected \
                    or not isinstance(item["bytes"], int) \
                    or isinstance(item["bytes"], bool) or item["bytes"] < 0 \
                    or not isinstance(item["links"], int) \
                    or isinstance(item["links"], bool) or item["links"] < 1 \
                    or not isinstance(item["mode"], str) \
                    or not re.fullmatch(r"[0-7]{4}", item["mode"]):
                raise EvidenceError("%s file metadata is invalid" % label)
            validate_hash(item["sha256"], "%s file hash" % label)
        elif kind == "dir":
            if set(item) != {"path", "type", "mode"} \
                    or not isinstance(item["mode"], str) \
                    or not re.fullmatch(r"[0-7]{4}", item["mode"]):
                raise EvidenceError("%s directory entry is invalid" % label)
        elif kind == "symlink":
            if set(item) != {"path", "type", "target_bytes",
                             "target_sha256"} \
                    or not isinstance(item["target_bytes"], int) \
                    or isinstance(item["target_bytes"], bool) \
                    or item["target_bytes"] < 0:
                raise EvidenceError("%s symlink entry is invalid" % label)
            validate_hash(item["target_sha256"], "%s symlink hash" % label)
        else:
            raise EvidenceError("%s entry type is invalid" % label)
    if section["sha256"] != sha256_bytes(canonical_bytes(entries)):
        raise EvidenceError("%s tree hash diverges" % label)


def validate_snapshot(snapshot, label):
    required = {"state", "runtime", "generations", "staging", "lock",
                "protected", "owner_data"}
    if not isinstance(snapshot, dict) or set(snapshot) != required:
        raise EvidenceError("%s snapshot shape is invalid" % label)
    for name in ("runtime", "protected", "owner_data"):
        validate_tree(snapshot[name], "%s %s" % (label, name))
    runtime_entries = snapshot["runtime"]["entries"]
    protected_entries = snapshot["protected"]["entries"]
    owner_entries = [item for item in protected_entries
                     if is_owner_record(item)]
    if snapshot["owner_data"]["entries"] != owner_entries:
        raise EvidenceError("%s owner-data list diverges" % label)
    if snapshot["generations"] != generation_sections(runtime_entries):
        raise EvidenceError("%s generation index diverges" % label)
    if snapshot["staging"] != staging_sections(runtime_entries):
        raise EvidenceError("%s staging index diverges" % label)
    state_value = snapshot["state"]
    if not isinstance(state_value, dict) or set(state_value) != {
            "present", "document", "sha256", "raw_base64"}:
        raise EvidenceError("%s state shape is invalid" % label)
    state_entry = find_entry(runtime_entries, "state.json")
    expected_hash = (state_entry.get("sha256") if state_entry
                     and state_entry.get("type") == "file" else None)
    if state_value["sha256"] != expected_hash:
        raise EvidenceError("%s state hash diverges" % label)
    if state_value["present"]:
        raw_state = decode_base64(state_value["raw_base64"],
                                  "%s state bytes" % label,
                                  doctor.MAX_STATE_BYTES)
        if sha256_bytes(raw_state) != state_value["sha256"]:
            raise EvidenceError("%s state bytes do not match the tree" % label)
        if parse_state_snapshot(raw_state) != state_value["document"]:
            raise EvidenceError("%s state document is not bound to bytes" % label)
    elif state_value["document"] is not None \
            or state_value["sha256"] is not None \
            or state_value["raw_base64"] is not None \
            or state_entry is not None:
        raise EvidenceError("%s absent state carries data" % label)
    lock = snapshot["lock"]
    if not isinstance(lock, dict) or set(lock) != {
            "present", "type", "tree", "owner", "owner_raw_base64"}:
        raise EvidenceError("%s lock shape is invalid" % label)
    validate_tree(lock["tree"], "%s lock" % label)
    lock_entries = lock["tree"]["entries"]
    if not lock["present"]:
        if lock["type"] is not None or lock_entries \
                or lock["owner"] is not None \
                or lock["owner_raw_base64"] is not None:
            raise EvidenceError("%s absent lock carries data" % label)
    elif lock["type"] == "dir":
        pass
    elif lock["type"] == "symlink":
        if lock["owner"] is not None or lock["owner_raw_base64"] is not None \
                or len(lock_entries) != 1 \
                or lock_entries[0].get("path") != "lock" \
                or lock_entries[0].get("type") != "symlink":
            raise EvidenceError("%s symlink lock shape is invalid" % label)
    elif lock["type"] == "other":
        if lock_entries or lock["owner"] is not None \
                or lock["owner_raw_base64"] is not None:
            raise EvidenceError("%s non-directory lock carries data" % label)
    else:
        raise EvidenceError("%s lock type is invalid" % label)
    owner = lock["owner"]
    owner_entry = find_entry(lock_entries, "owner")
    if owner is None:
        if lock["owner_raw_base64"] is not None or owner_entry is not None:
            raise EvidenceError("%s lock owner bytes are inconsistent" % label)
        return
    if owner_entry is None or owner_entry.get("type") != "file":
        raise EvidenceError("%s lock owner file is absent" % label)
    raw_owner = decode_base64(lock["owner_raw_base64"],
                              "%s lock owner bytes" % label, 4096)
    if sha256_bytes(raw_owner) != owner_entry["sha256"]:
        raise EvidenceError("%s lock owner is not bound to the tree" % label)
    parsed_owner = parse_lock_owner_snapshot(raw_owner)
    if owner is not None:
        required_owner = {"pid", "owner_format", "recorded_starttime",
                          "token_sha256", "current_starttime", "liveness"}
        if not isinstance(owner, dict) or set(owner) != required_owner \
                or owner["liveness"] not in ("dead", "alive", "reused"):
            raise EvidenceError("%s lock owner shape is invalid" % label)
        if not isinstance(owner["pid"], int) or isinstance(owner["pid"], bool) \
                or owner["pid"] <= 0:
            raise EvidenceError("%s lock owner pid is invalid" % label)
        if owner["pid"] != parsed_owner["pid"] \
                or owner["owner_format"] != parsed_owner["format"] \
                or owner["recorded_starttime"] != \
                parsed_owner["recorded_starttime"] \
                or owner["token_sha256"] != parsed_owner["token_sha256"]:
            raise EvidenceError("%s parsed lock owner diverges" % label)
        for field in ("recorded_starttime", "current_starttime"):
            value = owner[field]
            if value is not None and (not isinstance(value, int)
                                      or isinstance(value, bool) or value < 0):
                raise EvidenceError("%s lock starttime is invalid" % label)
        if owner["owner_format"] not in ("pid-starttime", "pid-token"):
            raise EvidenceError("%s lock owner format is invalid" % label)
        if (owner["owner_format"] == "pid-starttime"
                and (owner["recorded_starttime"] is None
                     or owner["token_sha256"] is not None)) \
                or (owner["owner_format"] == "pid-token"
                    and (owner["recorded_starttime"] is not None
                         or owner["token_sha256"] is None)):
            raise EvidenceError("%s lock owner proof shape is invalid" % label)
        if owner["token_sha256"] is not None:
            validate_hash(owner["token_sha256"], "%s lock token hash" % label)
        expected_liveness = ("dead" if owner["current_starttime"] is None
                             else "alive" if
                             owner["recorded_starttime"] is None
                             or owner["current_starttime"] ==
                             owner["recorded_starttime"]
                             else "reused")
        if owner["liveness"] != expected_liveness:
            raise EvidenceError("%s lock liveness is inconsistent" % label)


def static_lock(lock):
    value = dict(lock)
    owner = value.get("owner")
    if owner is not None:
        owner = dict(owner)
        owner.pop("current_starttime", None)
        owner.pop("liveness", None)
        value["owner"] = owner
    return value


def without_prefix(entries, prefix):
    return [item for item in entries
            if item["path"] != prefix
            and not item["path"].startswith(prefix + "/")]


def validate_receipt(identity, execution):
    required_execution = {"returncode", "timed_out", "stdout_bytes",
                          "stdout_sha256", "stdout_nonempty_lines",
                          "stderr_bytes", "stderr_sha256", "receipt_parse",
                          "receipt"}
    if not isinstance(execution, dict) \
            or set(execution) != required_execution:
        raise EvidenceError("execution shape is invalid")
    validate_hash(execution["stdout_sha256"], "stdout hash")
    validate_hash(execution["stderr_sha256"], "stderr hash")
    for field in ("stdout_bytes", "stdout_nonempty_lines", "stderr_bytes"):
        if not isinstance(execution[field], int) \
                or isinstance(execution[field], bool) or execution[field] < 0:
            raise EvidenceError("execution counters are invalid")
    if execution["timed_out"] is not False \
            or execution["receipt_parse"] != "parsed" \
            or execution["stdout_nonempty_lines"] != 1 \
            or execution["stderr_bytes"] != 0:
        raise EvidenceError("action did not emit one clean receipt")
    receipt = execution["receipt"]
    allowed = {"schema", "schema_version", "tool_version", "action", "result",
               "reason", "generation", "activation_seq", "path",
               "requested_path", "status_before", "lock", "pid"}
    mandatory = {"schema", "schema_version", "tool_version", "action", "result"}
    if not isinstance(receipt, dict) or not mandatory <= set(receipt) \
            or not set(receipt) <= allowed:
        raise EvidenceError("receipt fields are invalid")
    if receipt["schema"] != doctor.ACTION_SCHEMA \
            or type(receipt["schema_version"]) is not int \
            or receipt["schema_version"] != doctor.SCHEMA_VERSION \
            or receipt["tool_version"] != TOOL_VERSION \
            or receipt["action"] != identity["action"]:
        raise EvidenceError("receipt identity diverges")
    result = receipt["result"]
    if result not in ("ok", "already-clear", "refused"):
        raise EvidenceError("receipt result is invalid")
    expected_rc = 1 if result == "refused" else 0
    if type(execution["returncode"]) is not int \
            or execution["returncode"] != expected_rc:
        raise EvidenceError("receipt result and process status diverge")
    if result == "refused" \
            and (not isinstance(receipt.get("reason"), str)
                 or not receipt.get("reason")):
        raise EvidenceError("refused receipt has no reason")
    for value in receipt.values():
        if isinstance(value, str) and ("\x00" in value or "\n" in value
                                      or "\r" in value or value.startswith("/")
                                      or IPV4.search(value)):
            raise EvidenceError("receipt contains unsanitized text")
    expected_stdout = (json.dumps(receipt, sort_keys=True) + "\n").encode("utf-8")
    if execution["stdout_bytes"] != len(expected_stdout) \
            or execution["stdout_sha256"] != sha256_bytes(expected_stdout) \
            or execution["stderr_sha256"] != sha256_bytes(b""):
        raise EvidenceError("receipt does not match captured process output")
    action = identity["action"]
    target = identity["target"]
    if action in ("restore-previous", "complete-gc"):
        if receipt.get("generation") != target["generation"]:
            raise EvidenceError("receipt generation diverges")
    elif action == "discard-staging":
        if receipt.get("requested_path") != target["path"] \
                or (result != "refused" and receipt.get("path") != target["path"]):
            raise EvidenceError("receipt path diverges")
    elif type(receipt.get("pid")) is not int \
            or receipt.get("pid") != target["pid"]:
        raise EvidenceError("receipt pid diverges")
    if "activation_seq" in receipt \
            and (type(receipt["activation_seq"]) is not int
                 or receipt["activation_seq"] < 0):
        raise EvidenceError("receipt activation sequence is invalid")
    pending_cleanup = receipt.get("cleanup_pending")
    if pending_cleanup is not None \
            and (pending_cleanup is not True or result != "ok"):
        raise EvidenceError("cleanup-pending receipt is invalid")
    if action == "discard-staging" and pending_cleanup:
        clean_relative(receipt.get("quarantine_path"),
                       "discard quarantine")
    if action == "clear-lock" and pending_cleanup:
        quarantine = receipt.get("quarantine")
        if not isinstance(quarantine, str) \
                or "/" in quarantine or not quarantine.startswith(
                    ".nxdoctor-cleared-lock-"):
            raise EvidenceError("lock quarantine receipt is invalid")
    return result, receipt


def validate_transition(identity, before, after, result, receipt):
    action = identity["action"]
    target = identity["target"]
    if before["protected"] != after["protected"] \
            or before["owner_data"] != after["owner_data"]:
        raise EvidenceError("protected owner data changed")
    if action != "clear-lock" \
            and static_lock(before["lock"]) != static_lock(after["lock"]):
        raise EvidenceError("unrelated lock changed")
    before_runtime = before["runtime"]["entries"]
    after_runtime = after["runtime"]["entries"]

    if action == "restore-previous":
        if before["generations"] != after["generations"] \
                or before["staging"] != after["staging"] \
                or without_prefix(before_runtime, "state.json") != \
                without_prefix(after_runtime, "state.json"):
            raise EvidenceError("restore changed an unrelated runtime entry")
        if result in ("already-clear", "refused"):
            if before_runtime != after_runtime:
                raise EvidenceError("non-successful restore changed state")
            if result == "already-clear":
                old_doc = before["state"].get("document") or {}
                if old_doc.get("active") != target["generation"] \
                        or old_doc.get("pending") is not None:
                    raise EvidenceError("already-clear restore is incompatible")
            return
        old = before["state"]
        new = after["state"]
        if not old["present"] or not new["present"]:
            raise EvidenceError("restore lacks valid state snapshots")
        old_doc = old["document"]
        new_doc = new["document"]
        generation = target["generation"]
        stable_old = {key: value for key, value in old_doc.items()
                      if key not in ("active", "pending", "activation_seq")}
        stable_new = {key: value for key, value in new_doc.items()
                      if key not in ("active", "pending", "activation_seq")}
        if old_doc.get("previous_healthy") != generation \
                or new_doc.get("active") != generation \
                or new_doc.get("pending") is not None \
                or new_doc.get("activation_seq") != old_doc.get("activation_seq") + 1 \
                or stable_new != stable_old:
            raise EvidenceError("restore transition is incompatible")
        if receipt.get("activation_seq") != new_doc["activation_seq"]:
            raise EvidenceError("restore receipt sequence diverges")
        if any(item["path"].startswith(".state.json.nxdoctor-tmp-")
               for item in after_runtime):
            raise EvidenceError("restore left an atomic-write temporary file")
        return

    if action == "discard-staging":
        if before["state"] != after["state"] \
                or before["generations"] != after["generations"]:
            raise EvidenceError("discard changed state or a generation")
        prefix = target["path"][len(".nxruntime/"):]
        if result == "ok":
            if find_entry(before_runtime, prefix) is None \
                    or records_under(after_runtime, prefix):
                raise EvidenceError("discard transition is incompatible")
            if receipt.get("cleanup_pending"):
                quarantine = receipt["quarantine_path"]
                if quarantine == prefix or quarantine.startswith(prefix + "/") \
                        or not records_under(after_runtime, quarantine) \
                        or any(is_owner_record(item) for item in
                               records_under(before_runtime, prefix)) \
                        or without_prefix(before_runtime, prefix) != \
                        without_prefix(without_prefix(after_runtime, prefix),
                                       quarantine):
                    raise EvidenceError("pending discard quarantine diverges")
            elif without_prefix(before_runtime, prefix) != \
                    without_prefix(after_runtime, prefix):
                raise EvidenceError("discard changed an unrelated entry")
        elif before_runtime != after_runtime:
            raise EvidenceError("non-successful discard changed runtime")
        elif result == "already-clear" \
                and records_under(before_runtime, prefix):
            raise EvidenceError("already-clear discard target still exists")
        return

    if action == "complete-gc":
        if before["state"] != after["state"] \
                or before["staging"] != after["staging"]:
            raise EvidenceError("gc changed state or staging")
        generation = target["generation"]
        prefix = "generations/" + generation
        before_gen = before["generations"].get(generation)
        after_gen = after["generations"].get(generation)
        if result == "ok":
            if not before["state"]["present"]:
                raise EvidenceError("gc success lacks a valid state snapshot")
            state_doc = before["state"]["document"]
            if generation in (state_doc.get("active"), state_doc.get("pending"),
                               state_doc.get("previous_healthy"),
                               state_doc.get("failure_generation")):
                raise EvidenceError("gc removed a referenced generation")
            if before_gen is None or after_gen is not None \
                    or without_prefix(before_runtime, prefix) != \
                    without_prefix(after_runtime, prefix):
                raise EvidenceError("gc transition is incompatible")
            status_before = receipt.get("status_before")
            if status_before not in ("complete", "corrupt", "gc-interrupted") \
                    or (status_before == "gc-interrupted"
                        and not before_gen["claim"]) \
                    or (status_before != "gc-interrupted"
                        and not before_gen["commit"]):
                raise EvidenceError("gc receipt status diverges")
        elif result == "already-clear":
            if before_gen is not None or before_runtime != after_runtime:
                raise EvidenceError("already-clear gc is incompatible")
        elif before_runtime != after_runtime:
            if before_gen is None or after_gen is None \
                    or not after_gen["claim"] or after_gen["commit"] \
                    or not (before_gen["commit"] or before_gen["claim"]) \
                    or without_prefix(before_runtime, prefix) != \
                    without_prefix(after_runtime, prefix):
                raise EvidenceError("refused gc is not safely resumable")
        return

    if before_runtime != after_runtime:
        raise EvidenceError("clear-lock changed the port runtime")
    if result == "ok":
        owner = before["lock"].get("owner")
        if not before["lock"]["present"] or after["lock"]["present"] \
                or owner is None or owner.get("pid") != target["pid"] \
                or owner.get("liveness") not in ("dead", "reused"):
            raise EvidenceError("clear-lock lacks dead/reused owner proof")
    elif result == "already-clear":
        if before["lock"]["present"] or after["lock"]["present"]:
            raise EvidenceError("already-clear lock transition is incompatible")
    elif static_lock(before["lock"]) != static_lock(after["lock"]):
        raise EvidenceError("refused clear-lock changed the lock")


def validate_evidence(document, expected_run_id, expected_doctor_sha256,
                      expected_environment, expected_action, expected_target,
                      physical_context_path=None,
                      expected_physical_context_sha256=None):
    validate_run_id(expected_run_id)
    validate_hash(expected_doctor_sha256, "expected doctor hash")
    if expected_environment not in ("HOST_FIXTURE", "PHYSICAL"):
        raise EvidenceError("expected environment is invalid")
    if expected_environment == "PHYSICAL" and not PHYSICAL_TRUST_AVAILABLE:
        raise EvidenceError(
            "PHYSICAL evidence is PENDING an external trust anchor")
    validate_target(expected_action, expected_target)
    if not isinstance(document, dict) or set(document) != {
            "schema", "schema_version", "payload", "integrity"}:
        raise EvidenceError("evidence envelope shape is invalid")
    if document["schema"] != SCHEMA or document["schema_version"] != 1:
        raise EvidenceError("evidence schema is invalid")
    integrity = document["integrity"]
    if not isinstance(integrity, dict) or set(integrity) != {
            "algorithm", "payload_sha256"} \
            or integrity["algorithm"] != "sha256":
        raise EvidenceError("evidence integrity block is invalid")
    validate_hash(integrity["payload_sha256"], "payload hash")
    payload = document["payload"]
    if integrity["payload_sha256"] != sha256_bytes(canonical_bytes(payload)):
        raise EvidenceError("evidence payload hash diverges")
    required_payload = {"identity", "timing", "snapshot_before",
                        "snapshot_after", "execution", "capture_error"}
    if not isinstance(payload, dict) or set(payload) != required_payload:
        raise EvidenceError("evidence payload shape is invalid")
    if payload["capture_error"] is not None \
            or payload["snapshot_after"] is None:
        raise EvidenceError("evidence capture did not complete")
    identity = payload["identity"]
    required_identity = {"action", "target", "run_id", "environment",
                         "tool_version", "doctor_sha256",
                         "evidence_tool_sha256", "physical_context"}
    if not isinstance(identity, dict) or set(identity) != required_identity:
        raise EvidenceError("evidence identity shape is invalid")
    validate_target(identity["action"], identity["target"])
    if identity["action"] != expected_action \
            or identity["target"] != expected_target:
        raise EvidenceError("evidence action or target diverges")
    if identity["run_id"] != expected_run_id:
        raise EvidenceError("evidence is stale for the expected run")
    if identity["environment"] != expected_environment:
        raise EvidenceError("evidence class diverges")
    if identity["tool_version"] != TOOL_VERSION \
            or identity["doctor_sha256"] != expected_doctor_sha256:
        raise EvidenceError("doctor identity diverges")
    validate_hash(identity["evidence_tool_sha256"], "evidence tool hash")
    if identity["evidence_tool_sha256"] != sha256_file(os.path.abspath(__file__)):
        raise EvidenceError("evidence tool source diverges")
    context = identity["physical_context"]
    if expected_environment == "HOST_FIXTURE":
        if context is not None:
            raise EvidenceError("host evidence carries a physical claim")
        if physical_context_path is not None \
                or expected_physical_context_sha256 is not None:
            raise EvidenceError("host validation received a physical context")
    else:
        required_context = {"context_sha256", "created_unix", "expires_unix",
                            "machine", "nonce_sha256", "boot_id_sha256",
                            "os_release_sha256", "port_identity_sha256",
                            "doctor_sha256", "evidence_tool_sha256"}
        if not isinstance(context, dict) or set(context) != required_context:
            raise EvidenceError("physical evidence lacks required context")
        for field in ("context_sha256", "nonce_sha256", "boot_id_sha256",
                      "os_release_sha256", "port_identity_sha256",
                      "doctor_sha256", "evidence_tool_sha256"):
            validate_hash(context[field], field)
        if context["machine"] not in SUPPORTED_PHYSICAL_MACHINES:
            raise EvidenceError("physical machine is unsupported")
        if physical_context_path is None \
                or expected_physical_context_sha256 is None:
            raise EvidenceError("physical validation requires the pinned context")
        validate_hash(expected_physical_context_sha256,
                      "expected physical context hash")
        if context["context_sha256"] != expected_physical_context_sha256:
            raise EvidenceError("physical context pin diverges")
        if validate_saved_physical_context(
                physical_context_path, expected_physical_context_sha256,
                identity) != context:
            raise EvidenceError("physical context projection diverges")
    timing = payload["timing"]
    if not isinstance(timing, dict) or set(timing) != {
            "started_unix", "finished_unix", "duration_monotonic_ns"} \
            or any(not isinstance(timing[key], int)
                   or isinstance(timing[key], bool) for key in timing) \
            or timing["finished_unix"] < timing["started_unix"] \
            or timing["duration_monotonic_ns"] < 0:
        raise EvidenceError("evidence timing is invalid")
    if context is not None and not (
            context["created_unix"] <= timing["started_unix"]
            <= context["expires_unix"]):
        raise EvidenceError("physical context was stale at execution")
    validate_snapshot(payload["snapshot_before"], "before")
    validate_snapshot(payload["snapshot_after"], "after")
    result, receipt = validate_receipt(identity, payload["execution"])
    validate_transition(identity, payload["snapshot_before"],
                        payload["snapshot_after"], result, receipt)
    return result


def safe_write(path, value, forbidden_root=None):
    parent = os.path.dirname(os.path.abspath(path)) or "."
    if not os.path.isdir(parent):
        raise EvidenceError("output parent is absent")
    if forbidden_root is not None:
        candidate = os.path.realpath(os.path.join(parent,
                                                  os.path.basename(path)))
        root = os.path.realpath(forbidden_root)
        try:
            inside = os.path.commonpath([candidate, root]) == root
        except ValueError:
            inside = False
        if inside:
            raise EvidenceError("evidence output must stay outside the port")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL \
        | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags, 0o600)
    except OSError as error:
        raise EvidenceError("output must be a new regular file") from error
    try:
        created = os.fstat(fd)
    except OSError as error:
        os.close(fd)
        try:
            os.unlink(path)
        except OSError:
            pass
        raise EvidenceError("new output could not be inspected") from error
    success = False
    try:
        os.fchmod(fd, 0o600)
        private = os.fstat(fd)
        if not stat.S_ISREG(private.st_mode) or private.st_nlink != 1 \
                or stat.S_IMODE(private.st_mode) != 0o600:
            raise EvidenceError("output filesystem did not enforce mode 0600")
        payload = value if isinstance(value, bytes) else value.encode("utf-8")
        written = 0
        while written < len(payload):
            count = os.write(fd, payload[written:])
            if count <= 0:
                raise EvidenceError("output write did not make progress")
            written += count
        os.fsync(fd)
        finished = os.fstat(fd)
        if finished.st_size != len(payload) \
                or stat.S_IMODE(finished.st_mode) != 0o600:
            raise EvidenceError("output file verification failed")
        success = True
    except OSError as error:
        raise EvidenceError("output filesystem operation failed") from error
    finally:
        os.close(fd)
        if not success:
            try:
                current = os.lstat(path)
                if stat.S_ISREG(current.st_mode) \
                        and (current.st_dev, current.st_ino) == (
                            created.st_dev, created.st_ino):
                    os.unlink(path)
            except OSError:
                pass


def load_evidence(path):
    raw = read_regular(path, MAX_JSON_BYTES, "evidence")
    return strict_json_bytes(raw, "evidence")


def target_from_args(args):
    field = ACTIONS[args.action][1]
    supplied = {"generation": args.generation, "path": args.path,
                "pid": args.pid}
    if supplied[field] is None:
        raise EvidenceError("the action target is required")
    for other, value in supplied.items():
        if other != field and value is not None:
            raise EvidenceError("unrelated target argument is forbidden")
    return validate_target(args.action, {field: supplied[field]})


def build_parser():
    parser = argparse.ArgumentParser(prog="nxdoctor-evidence")
    sub = parser.add_subparsers(dest="command", required=True)

    capture = sub.add_parser("capture", help="capture one explicit action")
    capture.add_argument("port")
    capture.add_argument("--action", choices=sorted(ACTIONS), required=True)
    capture.add_argument("--generation")
    capture.add_argument("--path")
    capture.add_argument("--pid", type=int)
    capture.add_argument("--run-id", required=True)
    capture.add_argument("--physical-context")
    capture.add_argument("--physical-context-sha256")
    capture.add_argument("--out")

    context = sub.add_parser(
        "physical-context", help="create a short-lived ARM target context")
    context.add_argument("port")
    context.add_argument("--run-id", required=True)
    context.add_argument("--out", required=True)

    validate = sub.add_parser("validate", help="validate saved evidence")
    validate.add_argument("evidence")
    validate.add_argument("--expected-run-id", required=True)
    validate.add_argument("--expected-doctor-sha256", required=True)
    validate.add_argument("--expected-environment",
                          choices=("HOST_FIXTURE", "PHYSICAL"), required=True)
    validate.add_argument("--action", choices=sorted(ACTIONS), required=True)
    validate.add_argument("--generation")
    validate.add_argument("--path")
    validate.add_argument("--pid", type=int)
    validate.add_argument("--physical-context")
    validate.add_argument("--expected-physical-context-sha256")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    here = os.path.dirname(os.path.abspath(__file__))
    doctor_path = os.path.join(here, "nxdoctor.py")
    evidence_path = os.path.abspath(__file__)
    try:
        if args.command == "physical-context":
            value = create_physical_context(args.port, args.run_id,
                                            doctor_path, evidence_path)
            safe_write(args.out,
                       json.dumps(value, sort_keys=True, indent=2) + "\n",
                       forbidden_root=args.port)
            print("physical context created: sha256=%s"
                  % sha256_file(args.out))
            return 0
        if args.command == "capture":
            target = target_from_args(args)
            document = capture_action(
                args.port, args.action, target, args.run_id,
                physical_context=args.physical_context,
                physical_context_sha256=args.physical_context_sha256)
            expected_environment = ("PHYSICAL" if args.physical_context
                                    else "HOST_FIXTURE")
            result = validate_evidence(
                document, args.run_id, sha256_file(doctor_path),
                expected_environment, args.action, target,
                physical_context_path=args.physical_context,
                expected_physical_context_sha256=
                args.physical_context_sha256)
            value = canonical_bytes(document).decode("ascii") + "\n"
            if len(value.encode("ascii")) > MAX_JSON_BYTES:
                raise EvidenceError("evidence exceeds the output size limit")
            if args.out:
                safe_write(args.out, value, forbidden_root=args.port)
                print("evidence captured: result=%s sha256=%s"
                      % (result, sha256_file(args.out)))
            else:
                print(value, end="")
            return 0
        document = load_evidence(args.evidence)
        target = target_from_args(args)
        result = validate_evidence(
            document, args.expected_run_id, args.expected_doctor_sha256,
            args.expected_environment, args.action, target,
            physical_context_path=args.physical_context,
            expected_physical_context_sha256=
            args.expected_physical_context_sha256)
        print("evidence valid: result=%s" % result)
        return 0
    except (EvidenceError, doctor.DoctorError) as error:
        print("nxdoctor-evidence: %s" % error, file=sys.stderr)
        return 2
    except (OSError, RecursionError) as error:
        print("nxdoctor-evidence: operation failed closed (%s)"
              % type(error).__name__, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
