#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Create, materialize and verify immutable framework source snapshots."""

import argparse
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent
PIN_SCHEMA = "nextos-framework-build-pin-v1"
RECEIPT_SCHEMA = "nextos-framework-build-receipt-v1"
RECEIPT_NAME = "FRAMEWORK-SOURCE.json"
TREE_DIGEST = "nxgenerator-component-tree-sha256-v1"
TREE_DOMAIN = (TREE_DIGEST + "\0").encode("ascii")
NORMALIZED_MTIME = 946684800
NORMALIZED_MTIME_NS = NORMALIZED_MTIME * 1_000_000_000
MAX_JSON_BYTES = 1024 * 1024
MAX_GIT_OUTPUT_BYTES = 16 * 1024 * 1024
MAX_BLOB_BYTES = 8 * 1024 * 1024
MAX_COMPONENT_BYTES = 64 * 1024 * 1024
MAX_COMPONENT_FILES = 20000
MAX_COMPONENT_TREES = 20000
MAX_COMPONENT_TREE_BYTES = 16 * 1024 * 1024
GIT_TIMEOUT_SECONDS = 120
GIT_SEARCH_PATH = "/usr/local/bin:/usr/bin:/bin"
# Every framework component lives under framework/<name> in the repository and
# lands at the same place in a snapshot. NXExtract is the one exception: it is
# a sibling tree, and nxrelease resolves it relative to its own file as
# ../../suportando_outros_devices/extrator-universal, so a snapshot has to put
# it exactly there or a port built from the snapshot cannot run nxrelease.
COMPONENT_SOURCE_PATHS = {
    "nxextract": ("suportando_outros_devices", "extrator-universal"),
}
COMPONENTS = frozenset({
    "nxextract",
    # framework/tests holds the canonical PortMaster ZIP auditor and the
    # firmware profiles nxrelease shells out to; without it a snapshot can
    # render and validate but never bundle.
    "tests",
    # framework/portmaster holds the pinned HarbourMaster real-cycle gate the
    # ZIP auditor shells out to.
    "portmaster",
    "nxabi",
    "nxandroid",
    "nxaudio",
    "nxbootstrap",
    "nxcompat",
    "nxgenerator",
    "nxgl",
    "nxinput",
    "nxloader",
    "nxobs",
    "nxrelease",
    "nxsplash",
})
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
VERSION_RE = re.compile(
    r"^(?:0|[1-9][0-9]*)(?:[.](?:0|[1-9][0-9]*)){2}$"
)
REF_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/@+-]{0,255}$")
AT_FDCWD = -100
RENAME_NOREPLACE = 1


class PinError(Exception):
    """The source pin, Git object set or published snapshot is invalid."""


def canonical_json(value):
    return (json.dumps(
        value, ensure_ascii=False, indent=2, sort_keys=True
    ) + "\n").encode("utf-8")


def sha256_bytes(value):
    return hashlib.sha256(value).hexdigest()


def read_tool_version():
    try:
        raw_value = (ROOT / "VERSION").read_text(encoding="ascii")
    except OSError as error:
        raise PinError("cannot read nxgenerator VERSION: %s" % error)
    if not raw_value.endswith("\n") or raw_value.count("\n") != 1:
        raise PinError("nxgenerator VERSION is not canonical")
    value = raw_value[:-1]
    if not VERSION_RE.fullmatch(value):
        raise PinError("nxgenerator VERSION is invalid")
    return value


def require_string(value, context, pattern=None):
    if not isinstance(value, str) or not value or len(value) > 512:
        raise PinError("%s must be a bounded non-empty string" % context)
    if any(ord(character) < 0x20 or 0x7f <= ord(character) <= 0x9f
           for character in value):
        raise PinError("%s contains a control character" % context)
    if pattern is not None and pattern.fullmatch(value) is None:
        raise PinError("%s has an invalid value" % context)
    return value


def require_exact_object(value, context, keys):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise PinError(
            "%s must contain exactly: %s" %
            (context, ", ".join(sorted(keys)))
        )
    return value


def load_json_file(path, context):
    path = Path(path)
    reject_symlink_ancestors(path, context)

    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise PinError("duplicate JSON key in %s: %s" % (context, key))
            result[key] = value
        return result

    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(str(path), flags)
        try:
            information = os.fstat(descriptor)
            if (not stat.S_ISREG(information.st_mode) or
                    information.st_nlink != 1):
                raise PinError(
                    "%s must be a regular, uniquely linked file" % context
                )
            if information.st_size > MAX_JSON_BYTES:
                raise PinError("%s exceeds the size limit" % context)
            chunks = []
            remaining = information.st_size
            while remaining:
                chunk = os.read(descriptor, min(remaining, 65536))
                if not chunk:
                    raise PinError("%s was truncated while reading" % context)
                chunks.append(chunk)
                remaining -= len(chunk)
            if os.read(descriptor, 1):
                raise PinError("%s changed size while reading" % context)
            payload = b"".join(chunks)
        finally:
            os.close(descriptor)
        document = json.loads(
            payload.decode("utf-8"), object_pairs_hook=no_duplicates
        )
    except PinError:
        raise
    except (OSError, UnicodeDecodeError, ValueError) as error:
        raise PinError("cannot read %s: %s" % (context, error))
    return document, payload


def reject_symlink_ancestors(value, context, allow_missing_leaf=False):
    """Reject path traversal through symlinks in the local trusted workspace."""
    path = Path(os.path.abspath(os.fspath(value)))
    current = Path(path.anchor)
    parts = path.parts[1:] if path.anchor else path.parts
    for index, part in enumerate(parts):
        current = current / part
        try:
            information = current.lstat()
        except FileNotFoundError:
            if allow_missing_leaf and index == len(parts) - 1:
                return path
            raise PinError("%s path does not exist: %s" % (context, current))
        if stat.S_ISLNK(information.st_mode):
            raise PinError("%s path traverses a symlink: %s" % (context, current))
    return path


def validate_pin(document):
    require_exact_object(
        document, "framework source pin",
        {"schema", "schema_version", "tree_digest", "components"},
    )
    if document["schema"] != PIN_SCHEMA or document["schema_version"] != 1:
        raise PinError("unsupported framework source pin schema")
    if document["tree_digest"] != TREE_DIGEST:
        raise PinError("unsupported framework component tree digest")
    components = document["components"]
    if not isinstance(components, dict) or not components:
        raise PinError("framework source pin components must be non-empty")
    unknown = sorted(set(components) - COMPONENTS)
    if unknown:
        raise PinError("unknown framework component(s): %s" % ", ".join(unknown))
    normalized = {}
    for name in sorted(components):
        declaration = require_exact_object(
            components[name], "component %s" % name,
            {"version", "commit", "tree_sha256"},
        )
        normalized[name] = {
            "version": require_string(
                declaration["version"], "%s.version" % name, VERSION_RE
            ),
            "commit": require_string(
                declaration["commit"], "%s.commit" % name, COMMIT_RE
            ),
            "tree_sha256": require_string(
                declaration["tree_sha256"], "%s.tree_sha256" % name,
                SHA256_RE,
            ),
        }
    return {
        "schema": PIN_SCHEMA,
        "schema_version": 1,
        "tree_digest": TREE_DIGEST,
        "components": normalized,
    }


def validate_repository(value):
    repository = reject_symlink_ancestors(value, "repository")
    if not repository.is_dir():
        raise PinError("repository must be a real existing directory")
    result = run_git(repository, ["rev-parse", "--is-inside-work-tree"])
    if result.strip() != b"true":
        raise PinError("repository is not a Git worktree")
    object_format = run_git(
        repository, ["rev-parse", "--show-object-format"],
        max_output_bytes=64,
    ).strip()
    if object_format != b"sha1":
        raise PinError("only SHA-1 Git repositories are supported by pin v1")
    reject_promisor_repository(repository)
    return repository


def run_git(repository, arguments, max_output_bytes=MAX_GIT_OUTPUT_BYTES,
            allowed_returncodes=(0,)):
    # Git's environment can redirect even `git -C <repository>` to a different
    # object database or worktree.  Keep only host process lookup/temporary
    # settings and install the exact non-mutating Git policy below.
    git_binary = shutil.which("git", path=GIT_SEARCH_PATH)
    if git_binary is None:
        raise PinError("Git is unavailable in the trusted system path")
    environment = {
        "PATH": GIT_SEARCH_PATH,
        "LC_ALL": "C",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_NO_REPLACE_OBJECTS": "1",
        "GIT_NO_LAZY_FETCH": "1",
        "GIT_OPTIONAL_LOCKS": "0",
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_ASKPASS": os.devnull,
        "SSH_ASKPASS": os.devnull,
    }
    with tempfile.TemporaryFile() as standard_output, tempfile.TemporaryFile() as standard_error:
        try:
            result = subprocess.run(
                [git_binary, "-C", str(repository)] + list(arguments),
                stdin=subprocess.DEVNULL,
                stdout=standard_output,
                stderr=standard_error,
                env=environment,
                check=False,
                timeout=GIT_TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired as error:
            raise PinError(
                "Git object query exceeded %d seconds" % GIT_TIMEOUT_SECONDS
            ) from error
        standard_output.seek(0, os.SEEK_END)
        output_size = standard_output.tell()
        standard_error.seek(0, os.SEEK_END)
        error_size = standard_error.tell()
        if output_size > max_output_bytes:
            raise PinError("Git object query exceeded its output size limit")
        if error_size > MAX_JSON_BYTES:
            raise PinError("Git object query exceeded its diagnostic size limit")
        standard_output.seek(0)
        standard_error.seek(0)
        output = standard_output.read()
        diagnostic = standard_error.read()
    if result.returncode not in allowed_returncodes:
        detail = diagnostic.decode("utf-8", "replace").strip()
        if not detail:
            detail = output.decode("utf-8", "replace").strip()
        if len(detail) > 512:
            detail = detail[:512] + "..."
        raise PinError(
            "Git object query failed: %s" %
            (detail or "status %d" % result.returncode)
        )
    return output


def reject_promisor_repository(repository):
    declarations = run_git(
        repository,
        [
            "config", "--includes", "--local", "--get-regexp",
            r"^(extensions[.]partialclone|remote[.].*[.]promisor)$",
        ],
        max_output_bytes=MAX_JSON_BYTES,
        allowed_returncodes=(0, 1),
    )
    if declarations.strip():
        raise PinError("partial/promisor Git repositories are not accepted")
    common_dir = run_git(
        repository, ["rev-parse", "--git-common-dir"],
        max_output_bytes=4096,
    ).decode("utf-8", "strict").strip()
    common_path = Path(common_dir)
    if not common_path.is_absolute():
        common_path = repository / common_path
    common_path = reject_symlink_ancestors(common_path, "Git common directory")
    pack_directory = common_path / "objects" / "pack"
    if pack_directory.exists():
        reject_symlink_ancestors(pack_directory, "Git pack directory")
        if any(pack_directory.glob("*.promisor")):
            raise PinError("partial/promisor Git repositories are not accepted")


def verified_git_object(repository, object_id, expected_type, size_limit):
    object_id = require_string(object_id, "Git object id", COMMIT_RE)
    object_type = run_git(
        repository, ["cat-file", "-t", object_id], max_output_bytes=32
    ).decode("ascii", "strict").strip()
    if object_type != expected_type:
        raise PinError(
            "Git object %s is %s, expected %s" %
            (object_id, object_type, expected_type)
        )
    raw_size = run_git(
        repository, ["cat-file", "-s", object_id], max_output_bytes=64
    ).decode("ascii", "strict").strip()
    if not raw_size.isdigit():
        raise PinError("Git object has an invalid size: %s" % object_id)
    object_size = int(raw_size)
    if object_size > size_limit:
        raise PinError("Git %s object exceeds its size limit" % expected_type)
    payload = run_git(
        repository, ["cat-file", expected_type, object_id],
        max_output_bytes=object_size,
    )
    if len(payload) != object_size:
        raise PinError("Git object size changed while reading: %s" % object_id)
    actual_id = hashlib.sha1(
        expected_type.encode("ascii") + b" " +
        str(object_size).encode("ascii") + b"\0" + payload
    ).hexdigest()
    if actual_id != object_id:
        raise PinError("Git %s object hash mismatch: %s" % (expected_type, object_id))
    return payload


def commit_tree_id(repository, commit):
    payload = verified_git_object(repository, commit, "commit", MAX_JSON_BYTES)
    tree_lines = [
        line for line in payload.split(b"\n", 32)
        if line.startswith(b"tree ")
    ]
    if len(tree_lines) != 1:
        raise PinError("Git commit lacks exactly one root tree")
    object_id = tree_lines[0][5:]
    if re.fullmatch(rb"[0-9a-f]{40}", object_id) is None:
        raise PinError("Git commit root tree id is invalid")
    return object_id.decode("ascii")


def verified_tree_entries(repository, object_id):
    payload = verified_git_object(
        repository, object_id, "tree", MAX_GIT_OUTPUT_BYTES
    )
    entries = []
    cursor = 0
    names = set()
    while cursor < len(payload):
        separator = payload.find(b" ", cursor)
        terminator = payload.find(b"\0", separator + 1)
        if separator <= cursor or terminator < 0 or terminator + 21 > len(payload):
            raise PinError("Git tree object is malformed: %s" % object_id)
        mode = payload[cursor:separator]
        name = payload[separator + 1:terminator]
        raw_id = payload[terminator + 1:terminator + 21]
        cursor = terminator + 21
        if (not name or name in (b".", b"..") or b"/" in name or
                name in names):
            raise PinError("Git tree contains an unsafe or duplicate name")
        if mode not in (b"40000", b"100644", b"100755", b"120000", b"160000"):
            raise PinError("Git tree contains an unsupported mode")
        names.add(name)
        entries.append((mode, name, raw_id.hex()))
    return entries


def child_tree_id(repository, parent_id, name, context):
    matches = [
        entry for entry in verified_tree_entries(repository, parent_id)
        if entry[1] == name
    ]
    if len(matches) != 1 or matches[0][0] != b"40000":
        raise PinError("%s tree is absent or not canonical" % context)
    return matches[0][2]


def resolve_commit(repository, value):
    reference = require_string(value, "Git reference", REF_RE)
    resolved = run_git(
        repository, ["rev-parse", "--verify", reference + "^{commit}"]
    ).decode("ascii", "strict").strip()
    if COMMIT_RE.fullmatch(resolved) is None:
        raise PinError("Git reference did not resolve to a full SHA-1 commit")
    return resolved


def validate_relative_path(value, component):
    try:
        text = value.decode("utf-8", "strict")
    except UnicodeDecodeError as error:
        raise PinError("%s contains a non-UTF-8 Git path: %s" % (component, error))
    if (not text or len(value) > 4096 or "\\" in text or
            any(ord(character) < 0x20 or 0x7f <= ord(character) <= 0x9f
                for character in text)):
        raise PinError("%s contains an unsafe Git path" % component)
    logical = PurePosixPath(text)
    if (logical.is_absolute() or any(part in ("", ".", "..")
                                    for part in logical.parts) or
            logical.as_posix() != text):
        raise PinError("%s contains a noncanonical Git path" % component)
    if any(len(part.encode("utf-8")) > 255 for part in logical.parts):
        raise PinError("%s contains an oversized Git path component" % component)
    return text


def component_entries(repository, name, commit):
    if name not in COMPONENTS:
        raise PinError("unknown framework component: %s" % name)
    commit = require_string(commit, "%s.commit" % name, COMMIT_RE)
    resolved = resolve_commit(repository, commit)
    if resolved != commit:
        raise PinError("component commit is not canonical: %s" % name)
    root_tree = commit_tree_id(repository, commit)
    source_path = COMPONENT_SOURCE_PATHS.get(name, ("framework", name))
    component_tree = root_tree
    walked = []
    for segment in source_path:
        walked.append(segment)
        component_tree = child_tree_id(
            repository, component_tree, segment.encode("ascii"), "/".join(walked)
        )
    records = []
    total_size = 0
    tree_count = 0
    tree_bytes = 0

    pending_trees = [(component_tree, b"")]
    while pending_trees:
        tree_id, prefix = pending_trees.pop()
        tree_count += 1
        if tree_count > MAX_COMPONENT_TREES:
            raise PinError("component %s exceeds the tree-count limit" % name)
        entries = verified_tree_entries(repository, tree_id)
        tree_bytes += sum(
            len(mode) + 1 + len(raw_name) + 1 + 20
            for mode, raw_name, _ in entries
        )
        if tree_bytes > MAX_COMPONENT_TREE_BYTES:
            raise PinError("component %s exceeds the tree-size limit" % name)
        if not entries:
            raise PinError(
                "component %s contains an empty Git directory" % name
            )
        for mode, raw_name, object_id in entries:
            raw_path = raw_name if not prefix else prefix + b"/" + raw_name
            relative = validate_relative_path(raw_path, name)
            if mode == b"40000":
                pending_trees.append((object_id, raw_path))
                continue
            if mode not in (b"100644", b"100755"):
                raise PinError(
                    "%s contains unsupported mode/type %s" %
                    (name, mode.decode("ascii", "replace"))
                )
            if len(records) >= MAX_COMPONENT_FILES:
                raise PinError("component %s exceeds the file-count limit" % name)
            blob = verified_git_object(
                repository, object_id, "blob", MAX_BLOB_BYTES
            )
            total_size += len(blob)
            if total_size > MAX_COMPONENT_BYTES:
                raise PinError("component %s exceeds the byte-size limit" % name)
            records.append({
                "path": relative,
                "git_mode": mode.decode("ascii"),
                "mode": int(mode[-3:], 8),
                "blob": blob,
            })

    if not records:
        raise PinError("component is absent or empty at commit: %s" % name)
    records.sort(key=lambda item: item["path"].encode("utf-8"))
    if len({record["path"] for record in records}) != len(records):
        raise PinError("component contains duplicate paths: %s" % name)
    return records


def tree_digest(records):
    digest = hashlib.sha256()
    digest.update(TREE_DOMAIN)
    for record in records:
        blob = record["blob"]
        git_mode = record.get("git_mode")
        if git_mode is None:
            if record["mode"] not in (0o644, 0o755):
                raise PinError("component record has an unsupported mode")
            git_mode = "100%03o" % record["mode"]
        if git_mode not in ("100644", "100755"):
            raise PinError("component record has an unsupported Git mode")
        digest.update(git_mode.encode("ascii"))
        digest.update(b"\0")
        digest.update(record["path"].encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(len(blob)).encode("ascii"))
        digest.update(b"\0")
        digest.update(hashlib.sha256(blob).hexdigest().encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def component_version(records, name):
    matches = [record for record in records if record["path"] == "VERSION"]
    if len(matches) != 1:
        raise PinError("component %s lacks exactly one VERSION file" % name)
    try:
        raw_value = matches[0]["blob"].decode("ascii", "strict")
    except UnicodeDecodeError:
        raise PinError("component %s VERSION is not ASCII" % name)
    if not raw_value.endswith("\n") or raw_value.count("\n") != 1:
        raise PinError("component %s VERSION is not canonical" % name)
    value = raw_value[:-1]
    if VERSION_RE.fullmatch(value) is None:
        raise PinError("component %s VERSION is invalid" % name)
    return value


def load_pinned_components(repository, pin):
    result = {}
    for name, declaration in pin["components"].items():
        records = component_entries(repository, name, declaration["commit"])
        version = component_version(records, name)
        digest = tree_digest(records)
        if version != declaration["version"]:
            raise PinError(
                "%s VERSION mismatch: pin=%s source=%s" %
                (name, declaration["version"], version)
            )
        if digest != declaration["tree_sha256"]:
            raise PinError(
                "%s source tree SHA-256 mismatch: pin=%s source=%s" %
                (name, declaration["tree_sha256"], digest)
            )
        result[name] = {
            "version": version,
            "commit": declaration["commit"],
            "tree_sha256": digest,
            "records": records,
        }
    return result


def ensure_new_destination(value, context):
    destination = Path(os.path.abspath(os.fspath(value)))
    parent = destination.parent
    if destination.name in ("", ".", ".."):
        raise PinError("%s name is invalid" % context)
    reject_symlink_ancestors(parent, "%s parent" % context)
    if not parent.is_dir():
        raise PinError("%s parent must be a real existing directory" % context)
    if destination.exists() or destination.is_symlink():
        raise PinError("refusing to overwrite %s: %s" % (context, destination))
    return destination


def write_owned_file(path, payload, mode):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(str(path), flags, mode)
    try:
        view = memoryview(payload)
        while view:
            count = os.write(descriptor, view)
            if count <= 0:
                raise OSError("short write")
            view = view[count:]
        os.fsync(descriptor)
        os.fchmod(descriptor, mode)
    finally:
        os.close(descriptor)


def rename_noreplace(source, destination):
    try:
        renameat2 = ctypes.CDLL(None, use_errno=True).renameat2
    except AttributeError as error:
        raise PinError(
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
        raise PinError("refusing to overwrite snapshot: %s" % destination)
    raise PinError(
        "cannot publish snapshot without replacement: %s" %
        os.strerror(selected_errno)
    )


def expected_receipt(pin_payload, components):
    return {
        "schema": RECEIPT_SCHEMA,
        "schema_version": 1,
        "generator": {
            "name": "nxgenerator",
            "receipt_contract": RECEIPT_SCHEMA,
        },
        "pin_sha256": sha256_bytes(pin_payload),
        "tree_digest": TREE_DIGEST,
        "normalized_mtime": NORMALIZED_MTIME,
        "components": {
            name: {
                "version": state["version"],
                "commit": state["commit"],
                "tree_sha256": state["tree_sha256"],
                "file_count": len(state["records"]),
            }
            for name, state in sorted(components.items())
        },
        "claims": {
            "git_objects_only": True,
            "component_working_trees_ignored": True,
            "destination_no_overwrite": True,
            "symlinks_and_submodules_rejected": True,
        },
    }


def write_snapshot(stage, components, pin_payload):
    os.chmod(stage, 0o755)
    framework = stage / "framework"
    framework.mkdir(mode=0o755)
    for name, state in sorted(components.items()):
        source_path = COMPONENT_SOURCE_PATHS.get(name)
        if source_path:
            component = stage.joinpath(*source_path)
            component.mkdir(mode=0o755, parents=True)
        else:
            component = framework / name
            component.mkdir(mode=0o755)
        for record in state["records"]:
            destination = component / PurePosixPath(record["path"])
            destination.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
            write_owned_file(destination, record["blob"], record["mode"])
    write_owned_file(
        stage / RECEIPT_NAME,
        canonical_json(expected_receipt(pin_payload, components)),
        0o644,
    )
    for path in sorted(stage.rglob("*"), reverse=True):
        if path.is_dir():
            os.chmod(str(path), 0o755)
        os.utime(
            str(path), ns=(NORMALIZED_MTIME_NS, NORMALIZED_MTIME_NS),
            follow_symlinks=False,
        )
    os.chmod(str(stage), 0o755)
    os.utime(
        str(stage), ns=(NORMALIZED_MTIME_NS, NORMALIZED_MTIME_NS),
        follow_symlinks=False,
    )


def filesystem_component_records(component, name):
    if component.is_symlink() or not component.is_dir():
        raise PinError("snapshot component is absent or unsafe: %s" % name)
    records = []
    total_size = 0
    path_count = 0
    # Order by the POSIX path string, exactly as Git orders a tree: sorting
    # Path objects compares segment tuples, and the two disagree whenever a
    # file and a directory share a prefix ("x.json" sorts after "x/" as a
    # tuple but before it as a string). The digest is order-sensitive, so a
    # component with that shape -- framework/portmaster's vendored GUI has
    # one -- verified as a mismatch against its own pin.
    for path in sorted(component.rglob("*"),
                       key=lambda p: p.relative_to(component).as_posix()):
        path_count += 1
        if path_count > MAX_COMPONENT_FILES + MAX_COMPONENT_TREES:
            raise PinError("snapshot component exceeds the path-count limit: %s" % name)
        information = path.lstat()
        if stat.S_ISLNK(information.st_mode):
            raise PinError("snapshot contains a symlink: %s" % name)
        relative = path.relative_to(component).as_posix()
        validate_relative_path(relative.encode("utf-8"), name)
        mode = stat.S_IMODE(information.st_mode)
        if stat.S_ISDIR(information.st_mode):
            if mode != 0o755:
                raise PinError(
                    "snapshot directory mode differs: %s/%s" %
                    (name, relative)
                )
            if information.st_mtime_ns != NORMALIZED_MTIME_NS:
                raise PinError("snapshot mtime differs: %s/%s" % (name, relative))
            continue
        if (not stat.S_ISREG(information.st_mode) or
                mode not in (0o644, 0o755)):
            raise PinError(
                "snapshot file type/mode differs: %s/%s" % (name, relative)
            )
        if information.st_nlink != 1:
            raise PinError("snapshot contains a hard link: %s/%s" % (name, relative))
        if information.st_mtime_ns != NORMALIZED_MTIME_NS:
            raise PinError("snapshot mtime differs: %s/%s" % (name, relative))
        if len(records) >= MAX_COMPONENT_FILES:
            raise PinError("snapshot component exceeds the file-count limit: %s" % name)
        if information.st_size > MAX_BLOB_BYTES:
            raise PinError("snapshot component contains an oversized file: %s" % name)
        total_size += information.st_size
        if total_size > MAX_COMPONENT_BYTES:
            raise PinError("snapshot component exceeds the byte-size limit: %s" % name)
        flags = os.O_RDONLY
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        descriptor = os.open(str(path), flags)
        try:
            opened = os.fstat(descriptor)
            if ((opened.st_dev, opened.st_ino) !=
                    (information.st_dev, information.st_ino)):
                raise PinError("snapshot file changed while opening: %s/%s" % (name, relative))
            blob = b""
            remaining = opened.st_size
            chunks = []
            while remaining:
                chunk = os.read(descriptor, min(remaining, 65536))
                if not chunk:
                    raise PinError("snapshot file was truncated: %s/%s" % (name, relative))
                chunks.append(chunk)
                remaining -= len(chunk)
            if os.read(descriptor, 1):
                raise PinError("snapshot file changed size: %s/%s" % (name, relative))
            blob = b"".join(chunks)
        finally:
            os.close(descriptor)
        records.append({
            "path": relative,
            "git_mode": "100%03o" % mode,
            "mode": mode,
            "blob": blob,
        })
    if not records:
        raise PinError("snapshot component is empty: %s" % name)
    return records


def verify_snapshot(snapshot, pin, pin_payload):
    snapshot = reject_symlink_ancestors(snapshot, "snapshot")
    if not snapshot.is_dir():
        raise PinError("snapshot must be a real existing directory")
    if (stat.S_IMODE(snapshot.stat().st_mode) != 0o755 or
            snapshot.stat().st_mtime_ns != NORMALIZED_MTIME_NS):
        raise PinError("snapshot root mode or mtime differs")
    root_members = {path.name for path in snapshot.iterdir()}
    expected_root = {"framework", RECEIPT_NAME}
    for name in pin["components"]:
        source_path = COMPONENT_SOURCE_PATHS.get(name)
        if source_path:
            expected_root.add(source_path[0])
    if root_members != expected_root:
        raise PinError("snapshot root inventory differs from the contract")
    framework = snapshot / "framework"
    if framework.is_symlink() or not framework.is_dir():
        raise PinError("snapshot framework directory is absent or unsafe")
    if (stat.S_IMODE(framework.stat().st_mode) != 0o755 or
            framework.stat().st_mtime_ns != NORMALIZED_MTIME_NS):
        raise PinError("snapshot framework mode or mtime differs")
    actual_names = {path.name for path in framework.iterdir()}
    framework_components = {
        name for name in pin["components"] if name not in COMPONENT_SOURCE_PATHS
    }
    if actual_names != framework_components:
        raise PinError("snapshot component inventory differs from the pin")

    components = {}
    for name, declaration in pin["components"].items():
        source_path = COMPONENT_SOURCE_PATHS.get(name)
        component_dir = (snapshot.joinpath(*source_path) if source_path
                         else framework / name)
        records = filesystem_component_records(component_dir, name)
        if (stat.S_IMODE(component_dir.stat().st_mode) != 0o755 or
                component_dir.stat().st_mtime_ns != NORMALIZED_MTIME_NS):
            raise PinError("snapshot component mode or mtime differs: %s" % name)
        version = component_version(records, name)
        digest = tree_digest(records)
        if version != declaration["version"]:
            raise PinError("snapshot VERSION mismatch: %s" % name)
        if digest != declaration["tree_sha256"]:
            raise PinError("snapshot source tree SHA-256 mismatch: %s" % name)
        components[name] = {
            "version": version,
            "commit": declaration["commit"],
            "tree_sha256": digest,
            "records": records,
        }

    receipt_path = snapshot / RECEIPT_NAME
    receipt, receipt_payload = load_json_file(receipt_path, "framework source receipt")
    require_exact_object(
        receipt, "framework source receipt",
        {"schema", "schema_version", "generator", "pin_sha256",
         "tree_digest", "normalized_mtime", "components", "claims"},
    )
    if receipt["schema"] != RECEIPT_SCHEMA or receipt["schema_version"] != 1:
        raise PinError("unsupported framework source receipt schema")
    generator = require_exact_object(
        receipt["generator"], "framework source receipt generator",
        {"name", "receipt_contract"},
    )
    if (generator["name"] != "nxgenerator" or
            generator["receipt_contract"] != RECEIPT_SCHEMA):
        raise PinError("framework source receipt generator is invalid")
    expected = expected_receipt(pin_payload, components)
    if receipt != expected or receipt_payload != canonical_json(expected):
        raise PinError("framework source receipt differs from verified snapshot")
    if stat.S_IMODE(receipt_path.stat().st_mode) != 0o644:
        raise PinError("framework source receipt mode differs")
    if receipt_path.stat().st_mtime_ns != NORMALIZED_MTIME_NS:
        raise PinError("framework source receipt mtime differs")
    return components


def parse_component_specs(repository, values, default_reference):
    if not values:
        raise PinError("at least one --component is required")
    default_commit = None
    if default_reference is not None:
        default_commit = resolve_commit(repository, default_reference)
    result = {}
    for raw in values:
        value = require_string(raw, "--component")
        if "=" in value:
            name, reference = value.split("=", 1)
            reference = require_string(reference, "%s reference" % name, REF_RE)
            commit = resolve_commit(repository, reference)
        else:
            name = value
            if default_commit is None:
                raise PinError(
                    "--component %s needs =REF or a shared --commit" % name
                )
            commit = default_commit
        if name not in COMPONENTS:
            raise PinError("unknown framework component: %s" % name)
        if name in result:
            raise PinError("duplicate framework component: %s" % name)
        result[name] = commit
    return result


def publish_new_file(path, payload):
    destination = ensure_new_destination(path, "pin output")
    temporary = destination.parent / (
        ".%s.nxgenerator-%d-%s.tmp" %
        (destination.name, os.getpid(), secrets.token_hex(8))
    )
    published = False
    try:
        write_owned_file(temporary, payload, 0o644)
        rename_noreplace(temporary, destination)
        published = True
        parent_descriptor = os.open(
            str(destination.parent), os.O_RDONLY | os.O_DIRECTORY
        )
        try:
            os.fsync(parent_descriptor)
        finally:
            os.close(parent_descriptor)
    finally:
        if not published:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
    return destination


def command_create(arguments):
    repository = validate_repository(arguments.repository)
    specs = parse_component_specs(
        repository, arguments.component, arguments.commit
    )
    components = {}
    for name, commit in sorted(specs.items()):
        records = component_entries(repository, name, commit)
        components[name] = {
            "version": component_version(records, name),
            "commit": commit,
            "tree_sha256": tree_digest(records),
        }
    pin = {
        "schema": PIN_SCHEMA,
        "schema_version": 1,
        "tree_digest": TREE_DIGEST,
        "components": components,
    }
    payload = canonical_json(pin)
    output = publish_new_file(arguments.output, payload)
    print(
        "NXGENERATOR FRAMEWORK-PIN CREATE: PASS components=%d pin=%s sha256=%s" %
        (len(components), output, sha256_bytes(payload))
    )


def command_materialize(arguments):
    repository = validate_repository(arguments.repository)
    document, pin_payload = load_json_file(arguments.pin, "framework source pin")
    pin = validate_pin(document)
    if pin_payload != canonical_json(pin):
        raise PinError("framework source pin must use canonical JSON encoding")
    components = load_pinned_components(repository, pin)
    destination = ensure_new_destination(arguments.destination, "snapshot")
    stage = Path(tempfile.mkdtemp(
        prefix=".nxgenerator-framework-source-", dir=str(destination.parent)
    ))
    published = False
    try:
        write_snapshot(stage, components, pin_payload)
        verify_snapshot(stage, pin, pin_payload)
        rename_noreplace(stage, destination)
        published = True
        parent_descriptor = os.open(
            str(destination.parent), os.O_RDONLY | os.O_DIRECTORY
        )
        try:
            os.fsync(parent_descriptor)
        finally:
            os.close(parent_descriptor)
    finally:
        if not published and stage.exists():
            shutil.rmtree(stage)
    print(
        "NXGENERATOR FRAMEWORK-PIN MATERIALIZE: PASS components=%d snapshot=%s pin_sha256=%s" %
        (len(components), destination, sha256_bytes(pin_payload))
    )


def command_verify(arguments):
    document, pin_payload = load_json_file(arguments.pin, "framework source pin")
    pin = validate_pin(document)
    if pin_payload != canonical_json(pin):
        raise PinError("framework source pin must use canonical JSON encoding")
    components = verify_snapshot(arguments.snapshot, pin, pin_payload)
    print(
        "NXGENERATOR FRAMEWORK-PIN VERIFY: PASS components=%d snapshot=%s pin_sha256=%s" %
        (len(components), Path(arguments.snapshot).resolve(),
         sha256_bytes(pin_payload))
    )


def build_parser():
    parser = argparse.ArgumentParser(
        description="Pin and materialize framework sources from immutable Git objects"
    )
    parser.add_argument(
        "--version", action="version",
        version="nxgenerator framework-pin %s" % read_tool_version(),
    )
    commands = parser.add_subparsers(dest="command")
    commands.required = True

    create = commands.add_parser(
        "create", help="create a new canonical framework source pin"
    )
    create.add_argument("--repository", required=True)
    create.add_argument("--commit")
    create.add_argument(
        "--component", action="append", required=True,
        help="component name (uses --commit) or name=REF; repeat as needed",
    )
    create.add_argument("--output", required=True)
    create.set_defaults(function=command_create)

    materialize = commands.add_parser(
        "materialize", help="publish a new verified source snapshot from a pin"
    )
    materialize.add_argument("--repository", required=True)
    materialize.add_argument("--pin", required=True)
    materialize.add_argument("--destination", required=True)
    materialize.set_defaults(function=command_materialize)

    verify = commands.add_parser(
        "verify", help="re-audit an existing materialized source snapshot"
    )
    verify.add_argument("--pin", required=True)
    verify.add_argument("--snapshot", required=True)
    verify.set_defaults(function=command_verify)
    return parser


def main(argv=None):
    try:
        arguments = build_parser().parse_args(argv)
        arguments.function(arguments)
    except PinError as error:
        print("nxgenerator framework-pin: %s" % error, file=sys.stderr)
        return 1
    except (OSError, subprocess.SubprocessError) as error:
        print(
            "nxgenerator framework-pin: host operation failed: %s" % error,
            file=sys.stderr,
        )
        return 1
    except KeyboardInterrupt:
        print("nxgenerator framework-pin: interrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
