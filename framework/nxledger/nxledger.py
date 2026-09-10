#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxledger -- derived composition identity, one-shot attempts and profiles.

The V4 line produced one silent divergence (a hand-maintained ledger pointing
at an old HEAD) and one silent omission (versioned authorities like the
PortMaster contract living outside the derived identity).  nxledger removes
the hand from that loop: identity is DERIVED, never typed, and every
versioned authority of the composition is either in the explicit allowlist or
the derivation fails closed.

0.2.0 contract, on top of the 0.1.x derivation:

  * the authority allowlist covers EVERY canonical ``VERSION`` file under
    ``framework/`` (components, ``portmaster_contract``, ``apkcompat``,
    ``framework_tests``) plus the explicitly accepted external authority
    ``nxextract``; a ``VERSION`` file under ``framework/`` that is not in the
    allowlist fails closed until it is reviewed in;
  * ``--out`` writes only where the document cannot invalidate itself:
    outside the repository, or inside it on a path Git provably ignores; the
    write is atomic and an interrupted write leaves no partial file;
  * controlled failures: divergence is exit 1, contract/usage/derivation
    errors are exit 2, a blocked one-shot decision is exit 3 and an I/O
    failure is exit 4 -- never a traceback, never a reused meaning;
  * the ``oneshot`` sub-commands implement the V4-PRE-07 exact-identity
    enforcement and the V4-PRE-06 execution profiles (see README).

0.2.2 seals the outcome and pins the exact authority: results are closed
documents (oneshot-result/2) recomputed to a result_id and anchored to the
exact reservation; reservations are closed with policy-derived claims;
PUBLIC-FINAL package-candidate pins one explicit host authority
(host-authority/1) revalidated again at finish; physical-proof binds one
canonical family end to end (physical-authority/3 is the only live physical
schema); every consumed document passes one strict JSON loader and the
derivation refuses sources that move mid-read.  0.2.3 finishes that change:
RESULT_SCHEMA_VERSION is 2, so the result documents are genuinely
oneshot-result/2 (the 0.2.2 constant had lagged at 1 while the format had
already diverged from 0.2.1); an oneshot-result/1 document — including a real
result produced by nxledger 0.2.1, which also lacks the sealed key set — now
fails closed as tampered and never authorizes a package. The seal detects store
inconsistency and tampering in the canonical flow; it is NOT a
cryptographic signature -- the same-uid owner controls every byte, so
external evidence and the exact receipts remain required.

The ledger and the one-shot store never record tag/release, physical proof,
promotion, baseline or support claims on their own authority: receipts are
validated inputs, and absence is always a block, never a pass.
"""

import argparse
import errno
import hashlib
import json
import os
import re
import stat as stat_module
import struct
import subprocess
import sys
import tempfile

SCHEMA = "org.nextos.v4.ledger"
SCHEMA_VERSION = 2
TOOL_NAME = "nxledger"

ONESHOT_SCHEMA = "org.nextos.v4.oneshot-attempt"
ONESHOT_SCHEMA_VERSION = 2
INPUTS_SCHEMA = "org.nextos.v4.oneshot-inputs"
INPUTS_SCHEMA_VERSION = 1
EVIDENCE_SCHEMA = "org.nextos.v4.oneshot-evidence"
EVIDENCE_SCHEMA_VERSION = 2
PREFLIGHT_SCHEMA = "org.nextos.v4.preflight-receipt"
PREFLIGHT_SCHEMA_VERSION = 1
PHYSICAL_SCHEMA = "org.nextos.v4.physical-authority"
PHYSICAL_SCHEMA_VERSION = 3
HOST_AUTHORITY_SCHEMA = "org.nextos.v4.host-authority"
HOST_AUTHORITY_SCHEMA_VERSION = 1

PROFILES = ("DEV/device-matrix", "PUBLIC-FINAL")
ATTEMPT_TYPES = ("host-battery", "package-candidate", "physical-proof")
RESULTS = ("PASS", "FAIL", "INCONCLUSIVE")

# The canonical authority allowlist.  Every VERSION file under framework/
# must appear here, and the external authorities are accepted explicitly.
# Adding an authority is a reviewed framework change, exactly like adding a
# component to the declarative contract.
AUTHORITY_VERSION_FILES = (
    ("apkcompat", "framework/contracts/apkcompat/VERSION"),
    ("framework_tests", "framework/tests/VERSION"),
    ("nxabi", "framework/nxabi/VERSION"),
    ("nxandroid", "framework/nxandroid/VERSION"),
    ("nxaudio", "framework/nxaudio/VERSION"),
    ("nxbootstrap", "framework/nxbootstrap/VERSION"),
    ("nxcompat", "framework/nxcompat/VERSION"),
    ("nxdoctor", "framework/nxdoctor/VERSION"),
    ("nxextract", "suportando_outros_devices/extrator-universal/VERSION"),
    ("nxgenerator", "framework/nxgenerator/VERSION"),
    ("nxgl", "framework/nxgl/VERSION"),
    ("nxinput", "framework/nxinput/VERSION"),
    ("nxledger", "framework/nxledger/VERSION"),
    ("nxloader", "framework/nxloader/VERSION"),
    ("nxobs", "framework/nxobs/VERSION"),
    ("nxrelease", "framework/nxrelease/VERSION"),
    ("nxsplash", "framework/nxsplash/VERSION"),
    ("portmaster_contract", "framework/portmaster/VERSION"),
)
EXTERNAL_AUTHORITY_PATHS = frozenset((
    "suportando_outros_devices/extrator-universal/VERSION",
))
EXPECTED_AUTHORITY_COUNT = len(AUTHORITY_VERSION_FILES)

DECLARATIVE_CONTRACT_PATH = "framework/contracts/declarative-v1.json"

_VERSION_RE = re.compile(r"^[0-9]+(?:\.[0-9]+)+$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_FAMILY_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")


def _is_schema_version(value, expected):
    """Booleans never count as schema/version integers."""
    return isinstance(value, int) and not isinstance(value, bool) and \
        value == expected

CHECK_FIELDS = (
    ("repo.head_commit", ("repo", "head_commit")),
    ("repo.tree", ("repo", "tree")),
    ("repo.branch", ("repo", "branch")),
    ("repo.detached", ("repo", "detached")),
    ("repo.dirty", ("repo", "dirty")),
    ("components", ("components",)),
    ("declarative_contract.schema_version",
     ("declarative_contract", "schema_version")),
    ("declarative_contract.contract_version",
     ("declarative_contract", "contract_version")),
    ("tags_at_head", ("tags_at_head",)),
)

# Injectable only so the directed interruption cases can prove that a failure
# at any write boundary leaves neither a partial document nor a false success.
_fsync = os.fsync
_link = os.link


class LedgerError(Exception):
    """Contract, usage or derivation error (exit 2)."""


class BlockedError(Exception):
    """A one-shot decision is blocked, never partially approved (exit 3)."""


class IOFailure(Exception):
    """Controlled I/O failure (exit 4); never a traceback, never exit 1."""


# --------------------------------------------------------------------------
# derivation (Part A)

def _git(repo, *args):
    try:
        proc = subprocess.run(
            ("git", "-C", repo) + args,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as exc:
        raise IOFailure("cannot run git: %s" % exc)
    if proc.returncode != 0:
        raise LedgerError(
            "git %s failed: %s" %
            (" ".join(args), proc.stderr.decode("utf-8", "replace").strip()))
    return proc.stdout.decode("utf-8", "replace")


def repo_root(path):
    try:
        top = _git(path, "rev-parse", "--show-toplevel").strip()
    except LedgerError as exc:
        raise LedgerError("not a git repository: %s (%s)" % (path, exc))
    if not top:
        raise LedgerError("git did not report a worktree root for %s" % path)
    return top


def read_repo_identity(root):
    head = _git(root, "rev-parse", "HEAD").strip()
    if not re.match(r"^[0-9a-f]{40}$", head):
        raise LedgerError("HEAD did not resolve to a commit: %r" % head)
    tree = _git(root, "rev-parse", "HEAD^{tree}").strip()
    try:
        proc = subprocess.run(
            ("git", "-C", root, "symbolic-ref", "--short", "-q", "HEAD"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as exc:
        raise IOFailure("cannot run git: %s" % exc)
    if proc.returncode == 0:
        branch = proc.stdout.decode("utf-8", "replace").strip()
        detached = False
    elif proc.returncode == 1:
        branch = None
        detached = True
    else:
        raise LedgerError("git symbolic-ref failed: %s" %
                          proc.stderr.decode("utf-8", "replace").strip())
    dirty = bool(_git(root, "status", "--porcelain").strip())
    tags = sorted(t for t in
                  _git(root, "tag", "--points-at", "HEAD").splitlines() if t)
    return head, tree, branch, detached, dirty, tags


def _read_version_file(root, name, rel):
    path = os.path.join(root, rel)
    if os.path.islink(path):
        raise LedgerError("authority %s: VERSION is a symlink: %s" %
                          (name, rel))
    if not os.path.isfile(path):
        raise LedgerError("authority %s: VERSION missing: %s" % (name, rel))
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        raise IOFailure("authority %s: cannot read VERSION %s: %s" %
                        (name, rel, exc))
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError:
        raise LedgerError("authority %s: VERSION is not ASCII: %s" %
                          (name, rel))
    value = text[:-1] if text.endswith("\n") else text
    if not _VERSION_RE.match(value):
        raise LedgerError(
            "authority %s: VERSION content is not a version: %r" %
            (name, value))
    return value


def read_authority_versions(root):
    versions = {}
    for name, rel in AUTHORITY_VERSION_FILES:
        versions[name] = _read_version_file(root, name, rel)
    # Deterministic inventory: EVERY VERSION file under framework/ must be an
    # allowlisted authority; a new versioned domain fails closed until it is
    # reviewed into the allowlist.
    allow = {rel for _, rel in AUTHORITY_VERSION_FILES}
    found = []
    fw = os.path.join(root, "framework")

    def _walk_failed(exc):
        raise IOFailure("cannot inventory framework authorities: %s" % exc)

    for dirpath, dirnames, filenames in os.walk(fw, onerror=_walk_failed):
        dirnames.sort()
        if "VERSION" in filenames or "VERSION" in dirnames:
            candidate = os.path.join(dirpath, "VERSION")
            if os.path.lexists(candidate):
                found.append(os.path.relpath(candidate, root))
    for rel in sorted(found):
        if rel.replace(os.sep, "/") not in allow:
            raise LedgerError(
                "unlisted versioned authority (extend the reviewed "
                "allowlist first): %s" % rel.replace(os.sep, "/"))
    return versions


def read_contract_identity(root, with_sha256=False):
    path = os.path.join(root, DECLARATIVE_CONTRACT_PATH)
    if os.path.islink(path):
        raise LedgerError("declarative contract is a symlink: %s" %
                          DECLARATIVE_CONTRACT_PATH)
    if not os.path.isfile(path):
        raise LedgerError("declarative contract missing: %s" %
                          DECLARATIVE_CONTRACT_PATH)
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        raise IOFailure("declarative contract unreadable: %s" % exc)
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise LedgerError("declarative contract unparseable: %s" % exc)
    if not isinstance(data, dict):
        raise LedgerError("declarative contract is not a JSON object")
    schema_version = data.get("schema_version")
    contract_version = data.get("contract_version")
    if not isinstance(schema_version, int):
        raise LedgerError(
            "declarative contract has no integer schema_version; "
            "refusing to invent it")
    if not isinstance(contract_version, str) or not contract_version:
        raise LedgerError(
            "declarative contract has no contract_version string; "
            "refusing to invent it")
    identity = {"path": DECLARATIVE_CONTRACT_PATH,
                "schema_version": schema_version,
                "contract_version": contract_version}
    if with_sha256:
        identity["sha256"] = hashlib.sha256(raw).hexdigest()
    return identity


def derive(root):
    head, tree, branch, detached, dirty, tags = read_repo_identity(root)
    identity = (head, tree, branch, detached, dirty, tags)
    ledger = {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": TOOL_NAME, "version": tool_version()},
        "repo": {
            "head_commit": head,
            "tree": tree,
            "branch": branch,
            "detached": detached,
            "dirty": dirty,
        },
        "components": read_authority_versions(root),
        "declarative_contract": read_contract_identity(root),
        "tags_at_head": tags,
    }
    if read_repo_identity(root) != identity:
        raise LedgerError(
            "repository identity changed during derivation; refusing a "
            "hybrid ledger")
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch is not None:
        if not re.match(r"^[0-9]+$", epoch):
            raise LedgerError("SOURCE_DATE_EPOCH is not a decimal integer")
        ledger["generated_epoch"] = int(epoch)
    return ledger


def tool_version():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "VERSION")
    try:
        with open(path, "r", encoding="ascii") as fh:
            value = fh.read().strip()
    except OSError as exc:
        raise IOFailure("cannot read the nxledger VERSION file: %s" % exc)
    except UnicodeDecodeError:
        raise LedgerError("nxledger VERSION file is not ASCII")
    if not _VERSION_RE.match(value):
        raise LedgerError("nxledger VERSION file is invalid: %r" % value)
    return value


def canonical_bytes(document):
    return (json.dumps(document, sort_keys=True, ensure_ascii=True,
                       separators=(",", ": "), indent=1) + "\n").encode("ascii")


def _lookup(document, path):
    node = document
    for key in path:
        if not isinstance(node, dict) or key not in node:
            return ("<absent>",)
        node = node[key]
    return node


def check(root, ledger_path):
    if os.path.islink(ledger_path):
        raise LedgerError("refusing symlinked ledger: %s" % ledger_path)
    try:
        with open(ledger_path, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        raise IOFailure("ledger unreadable: %s" % exc)
    recorded = _strict_json_object(raw, "ledger")
    if recorded.get("schema") != SCHEMA or \
            not _is_schema_version(recorded.get("schema_version"),
                                   SCHEMA_VERSION):
        raise LedgerError(
            "ledger schema is not %s/%d" % (SCHEMA, SCHEMA_VERSION))
    derived = derive(root)
    differences = []
    for label, path in CHECK_FIELDS:
        want = _lookup(recorded, path)
        have = _lookup(derived, path)
        if want != have:
            differences.append((label, want, have))
    if differences:
        for label, want, have in differences:
            sys.stderr.write("nxledger: DIVERGENT %s: ledger=%s derived=%s\n" %
                             (label, json.dumps(want, sort_keys=True),
                              json.dumps(have, sort_keys=True)))
        return False
    sys.stdout.write("nxledger: MATCH %s\n" % derived["repo"]["head_commit"])
    return True


def _out_location_allowed(target_abs, root):
    """--out may not invalidate the document it writes.

    A path outside the repository can never make the tree dirty.  A path
    inside the repository is allowed only when Git provably ignores it, so
    the derived identity (including ``dirty``) is byte-identical before and
    after the write.
    """
    root_real = os.path.realpath(root)
    parent_real = os.path.realpath(os.path.dirname(target_abs) or ".")
    inside = parent_real == root_real or \
        parent_real.startswith(root_real + os.sep)
    if not inside:
        return
    rel = os.path.join(os.path.relpath(parent_real, root_real),
                       os.path.basename(target_abs))
    rel = os.path.normpath(rel)
    try:
        proc = subprocess.run(
            ("git", "-C", root, "check-ignore", "-q", "--", rel),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as exc:
        raise IOFailure("cannot run git check-ignore: %s" % exc)
    if proc.returncode != 0:
        raise LedgerError(
            "output inside the repository must be git-ignored, or the "
            "document would dirty the identity it records: %s" % rel)


def write_atomic(data, target, root):
    target_abs = os.path.abspath(target)
    if os.path.lexists(target_abs):
        raise LedgerError(
            "refusing existing path (symlinks included): %s" % target)
    parent = os.path.dirname(target_abs) or "."
    if os.path.islink(parent):
        raise LedgerError("refusing symlinked parent directory: %s" % parent)
    if not os.path.isdir(parent):
        raise LedgerError("parent directory does not exist: %s" % parent)
    _out_location_allowed(target_abs, root)
    _write_new_file(data, target_abs, parent)


def _write_new_file(data, target_abs, parent):
    """Atomic create-new write: temp file + hard link, controlled failures."""
    try:
        fd, tmp = tempfile.mkstemp(prefix=".nxledger.", dir=parent)
    except OSError as exc:
        raise IOFailure("cannot create temporary file in %s: %s" %
                        (parent, exc))
    published = False
    try:
        try:
            with os.fdopen(fd, "wb") as fh:
                fh.write(data)
                fh.flush()
                _fsync(fh.fileno())
            _link(tmp, target_abs)  # atomic: fails if the target appeared
            dir_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                _fsync(dir_fd)
            finally:
                os.close(dir_fd)
            published = True
        except OSError as exc:
            if exc.errno == errno.EEXIST:
                raise IOFailure("target appeared concurrently: %s" %
                                target_abs)
            raise IOFailure("write failed for %s: %s" % (target_abs, exc))
    finally:
        try:
            os.unlink(tmp)
        except OSError as exc:
            # A cleanup failure is never ignored: on a published write it is
            # the failure; on a failed write the primary error already
            # propagates, so the outcome is never turned into success.
            if published:
                raise IOFailure(
                    "temporary cleanup failed for %s: %s" % (tmp, exc))


# --------------------------------------------------------------------------
# ELF inspection (Part B): the tuple binds every declared project ELF.

def _u(fmt, blob, offset):
    return struct.unpack_from(fmt, blob, offset)


def inspect_elf(path):
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except OSError as exc:
        raise IOFailure("cannot read ELF %s: %s" % (path, exc))
    if len(blob) < 64 or blob[:4] != b"\x7fELF":
        raise LedgerError("not an ELF file: %s" % path)
    try:
        return _inspect_elf_blob(blob)
    except (struct.error, ValueError, IndexError) as exc:
        raise IOFailure("truncated or malformed ELF %s: %s" % (path, exc))


def _inspect_elf_blob(blob):
    ei_class, ei_data = blob[4], blob[5]
    if ei_class not in (1, 2) or ei_data not in (1, 2):
        raise LedgerError("unsupported ELF ident")
    is64 = ei_class == 2
    endian = "<" if ei_data == 1 else ">"
    if is64:
        e_machine, = _u(endian + "H", blob, 18)
        e_phoff, = _u(endian + "Q", blob, 32)
        e_shoff, = _u(endian + "Q", blob, 40)
        e_phentsize, e_phnum = _u(endian + "HH", blob, 54)
        e_shentsize, e_shnum = _u(endian + "HH", blob, 58)
    else:
        e_machine, = _u(endian + "H", blob, 18)
        e_phoff, = _u(endian + "I", blob, 28)
        e_shoff, = _u(endian + "I", blob, 32)
        e_phentsize, e_phnum = _u(endian + "HH", blob, 42)
        e_shentsize, e_shnum = _u(endian + "HH", blob, 46)
    interp = None
    build_id = None
    for index in range(e_phnum):
        base = e_phoff + index * e_phentsize
        p_type, = _u(endian + "I", blob, base)
        if is64:
            p_offset, = _u(endian + "Q", blob, base + 8)
            p_filesz, = _u(endian + "Q", blob, base + 32)
        else:
            p_offset, = _u(endian + "I", blob, base + 4)
            p_filesz, = _u(endian + "I", blob, base + 16)
        segment = blob[p_offset:p_offset + p_filesz]
        if p_type == 3:  # PT_INTERP
            interp = segment.rstrip(b"\x00").decode("utf-8", "replace")
        elif p_type == 4 and build_id is None:  # PT_NOTE
            build_id = _note_build_id(segment, endian)
    max_glibc = _max_glibc(blob, endian, is64,
                           e_shoff, e_shentsize, e_shnum)
    return {
        "elf_class": 64 if is64 else 32,
        "machine": e_machine,
        "pt_interp": interp,
        "build_id": build_id,
        "max_glibc": max_glibc,
        "size": len(blob),
        "sha256": hashlib.sha256(blob).hexdigest(),
    }


def _note_build_id(segment, endian):
    offset = 0
    while offset + 12 <= len(segment):
        namesz, descsz, note_type = _u(endian + "III", segment, offset)
        offset += 12
        name = segment[offset:offset + namesz].rstrip(b"\x00")
        offset += (namesz + 3) & ~3
        desc = segment[offset:offset + descsz]
        offset += (descsz + 3) & ~3
        if name == b"GNU" and note_type == 3:
            return desc.hex()
    return None


def _max_glibc(blob, endian, is64, shoff, shentsize, shnum):
    best = None
    for index in range(shnum):
        base = shoff + index * shentsize
        sh_type, = _u(endian + "I", blob, base + 4)
        if sh_type != 0x6ffffffe:  # SHT_GNU_verneed
            continue
        if is64:
            sh_offset, = _u(endian + "Q", blob, base + 24)
            sh_link, = _u(endian + "I", blob, base + 40)
        else:
            sh_offset, = _u(endian + "I", blob, base + 16)
            sh_link, = _u(endian + "I", blob, base + 24)
        strtab_base = shoff + sh_link * shentsize
        if is64:
            str_off, = _u(endian + "Q", blob, strtab_base + 24)
        else:
            str_off, = _u(endian + "I", blob, strtab_base + 16)
        cursor = sh_offset
        while True:
            _vers, vn_cnt = _u(endian + "HH", blob, cursor)
            vn_aux, vn_next = _u(endian + "II", blob, cursor + 8)
            aux = cursor + vn_aux
            for _ in range(vn_cnt):
                vna_name, = _u(endian + "I", blob, aux + 8)
                vna_next, = _u(endian + "I", blob, aux + 12)
                end = blob.index(b"\x00", str_off + vna_name)
                name = blob[str_off + vna_name:end].decode("ascii", "replace")
                if name.startswith("GLIBC_"):
                    parts = tuple(int(p) for p in name[6:].split(".")
                                  if p.isdigit())
                    if best is None or parts > best[0]:
                        best = (parts, name)
                if vna_next == 0:
                    break
                aux += vna_next
            if vn_next == 0:
                break
            cursor += vn_next
    return best[1] if best else None


# --------------------------------------------------------------------------
# one-shot attempts (Parts B and C; 0.2.1 artifact binding)

MAX_STORE_DOCUMENT_BYTES = 1024 * 1024
ARTIFACT_SCHEMA = "org.nextos.v4.oneshot-artifact"
ARTIFACT_SCHEMA_VERSION = 1
RESULT_SCHEMA = "org.nextos.v4.oneshot-result"
RESULT_SCHEMA_VERSION = 2

# Keys the reservation document adds on top of the derived attempt tuple.
# Everything else must recompute to the attempt id, or the store is hostile.
RESERVATION_EXTRA_KEYS = frozenset((
    "attempt_id", "base_id", "allowed_effects",
    "physical_support_proven", "public_candidate_authorized",
))
ARTIFACT_KEYS = (
    "schema", "schema_version", "profile", "repo", "authorities",
    "declarative_contract", "tags_at_head", "inputs_manifest_sha256",
    "elves", "toolchain",
)


MAX_JSON_DEPTH = 64


def _json_no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise LedgerError("duplicate JSON key in store document: %s" % key)
        result[key] = value
    return result


def _max_json_depth(text):
    depth = 0
    deepest = 0
    in_string = False
    escaped = False
    for character in text:
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character in "{[":
            depth += 1
            if depth > deepest:
                deepest = depth
        elif character in "}]" and depth:
            depth -= 1
    return deepest


def _strict_json_object(raw, what):
    """The single strict JSON loader for every document nxledger consumes.

    Manifests, receipts, evidence, host authorities and store documents all
    pass through here: duplicate keys, NaN/Infinity, a UTF-8 BOM, loose
    encoding, excessive nesting and oversized payloads fail closed with a
    contract error naming the document, never a traceback.
    """
    if len(raw) > MAX_STORE_DOCUMENT_BYTES:
        raise LedgerError("%s exceeds the document size ceiling" % what)
    if raw.startswith(b"\xef\xbb\xbf"):
        raise LedgerError("%s must not carry a UTF-8 BOM" % what)
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise LedgerError("%s is not strict UTF-8" % what)
    if _max_json_depth(text) > MAX_JSON_DEPTH:
        raise LedgerError("%s exceeds the JSON nesting ceiling" % what)

    def _constant(_value):
        raise LedgerError("%s contains a non-JSON constant (NaN/Infinity)" %
                          what)

    try:
        document = json.loads(text, object_pairs_hook=_json_no_duplicates,
                              parse_constant=_constant)
    except LedgerError:
        raise
    except (ValueError, RecursionError):
        raise LedgerError("%s is not valid JSON" % what)
    if not isinstance(document, dict):
        raise LedgerError("%s is not a JSON object" % what)
    return document


def _regular_file_bytes(path, what):
    if os.path.islink(path):
        raise LedgerError("%s is a symlink: %s" % (what, path))
    if not os.path.isfile(path):
        raise LedgerError("%s is not a regular file: %s" % (what, path))
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        raise IOFailure("cannot read %s %s: %s" % (what, path, exc))


def _lstat_identity(path, what):
    info = _lstat(path, what)
    if info is None:
        raise LedgerError("%s disappeared during derivation: %s" %
                          (what, path))
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)


def load_inputs_manifest(path, root, repo):
    manifest_identity = _lstat_identity(path, "inputs manifest")
    raw = _regular_file_bytes(path, "inputs manifest")
    data = _strict_json_object(raw, "inputs manifest")
    if data.get("schema") != INPUTS_SCHEMA or \
            not _is_schema_version(data.get("schema_version"),
                                   INPUTS_SCHEMA_VERSION):
        raise LedgerError("inputs manifest schema is not %s/%d" %
                          (INPUTS_SCHEMA, INPUTS_SCHEMA_VERSION))
    for key in ("commit", "tree"):
        declared = data.get(key)
        if declared is not None and declared != repo[
                "head_commit" if key == "commit" else "tree"]:
            raise LedgerError(
                "inputs manifest is stale: declared %s %s does not match "
                "the derived repository" % (key, declared))
    elves = data.get("elves", [])
    if not isinstance(elves, list):
        raise LedgerError("inputs manifest 'elves' must be a list")
    records = []
    seen = set()
    for entry in elves:
        if not isinstance(entry, dict):
            raise LedgerError("inputs manifest ELF entry is not an object")
        logical = entry.get("logical_path")
        file_path = entry.get("file")
        if not isinstance(logical, str) or not logical or \
                not isinstance(file_path, str) or not file_path:
            raise LedgerError(
                "inputs manifest ELF entry needs logical_path and file")
        if logical in seen:
            raise LedgerError("duplicate ELF logical_path: %s" % logical)
        seen.add(logical)
        if os.path.isabs(logical) or ".." in logical.split("/"):
            raise LedgerError("unsafe ELF logical_path: %s" % logical)
        resolved = file_path if os.path.isabs(file_path) else \
            os.path.join(root, file_path)
        if os.path.islink(resolved) or not os.path.isfile(resolved):
            raise LedgerError(
                "ELF file must be a regular non-symlink file: %s" % file_path)
        elf_identity = _lstat_identity(resolved, "ELF file")
        record = inspect_elf(resolved)
        if _lstat_identity(resolved, "ELF file") != elf_identity:
            raise LedgerError(
                "ELF file changed while it was being measured; refusing a "
                "hybrid identity: %s" % file_path)
        for declared_key in ("sha256", "build_id", "size"):
            declared = entry.get(declared_key)
            if declared is not None and declared != record[declared_key]:
                raise LedgerError(
                    "ELF %s divergence for %s: declared %s, measured %s" %
                    (declared_key, logical, declared, record[declared_key]))
        record["logical_path"] = logical
        records.append(record)
    records.sort(key=lambda r: r["logical_path"])
    toolchain = data.get("toolchain")
    if toolchain is not None and not isinstance(toolchain, dict):
        raise LedgerError("inputs manifest 'toolchain' must be an object")
    if _lstat_identity(path, "inputs manifest") != manifest_identity:
        raise LedgerError(
            "inputs manifest changed while it was being read; refusing a "
            "hybrid identity")
    return {
        "manifest_sha256": hashlib.sha256(raw).hexdigest(),
        "elves": records,
        "toolchain": toolchain,
    }


def _load_receipt_document(path, schema, schema_version, what):
    """Structural receipt validation; malformed receipts never enter a tuple."""
    raw = _regular_file_bytes(path, what)
    data = _strict_json_object(raw, what)
    if data.get("schema") != schema or \
            not _is_schema_version(data.get("schema_version"), schema_version):
        raise LedgerError("%s schema is not %s/%d" %
                          (what, schema, schema_version))
    return data, hashlib.sha256(raw).hexdigest()


def _load_host_authority(path):
    """Closed schema: exactly the attempt and result ids being pinned."""
    data, digest = _load_receipt_document(
        path, HOST_AUTHORITY_SCHEMA, HOST_AUTHORITY_SCHEMA_VERSION,
        "host authority receipt")
    expected_keys = {"schema", "schema_version", "attempt_id", "result_id"}
    if set(data) != expected_keys:
        raise LedgerError(
            "host authority receipt carries an unexpected or missing field; "
            "the schema is closed")
    for key in ("attempt_id", "result_id"):
        value = data.get(key)
        if not isinstance(value, str) or not _SHA256_RE.match(value):
            raise LedgerError(
                "host authority receipt %s must be a 64-hex SHA-256" % key)
    return {"sha256": digest,
            "attempt_id": data["attempt_id"],
            "result_id": data["result_id"]}


def build_attempt_tuple(root, profile, attempt_type, manifest, options=None):
    """Derive the artifact base and the authorizing attempt tuple.

    The base binds the EXACT artifact inputs (manifest hash, every measured
    ELF, declared toolchain) together with profile, repo state, authorities,
    contract identity and tags; a host-battery PASS therefore authorizes only
    a package candidate built from byte-identical inputs.  The attempt id
    additionally binds the attempt type, the ordered required physical
    families and the sha256+identity of every supplied receipt.
    """
    options = options or {}
    if profile not in PROFILES:
        raise LedgerError("profile must be one of %s" % (PROFILES,))
    if attempt_type not in ATTEMPT_TYPES:
        raise LedgerError("attempt-type must be one of %s" % (ATTEMPT_TYPES,))
    family = options.get("family")
    if attempt_type == "physical-proof":
        if not isinstance(family, str) or not _FAMILY_RE.match(family):
            raise LedgerError(
                "physical-proof requires exactly one canonical --family "
                "(lowercase letters, digits, dot, underscore or dash)")
    elif family is not None:
        raise LedgerError(
            "--family is only valid for physical-proof; %s never carries "
            "one" % attempt_type)
    if options.get("host_authority") is not None and \
            attempt_type != "package-candidate":
        raise LedgerError(
            "--host-authority is only valid for package-candidate")
    head, tree, branch, detached, dirty, tags = read_repo_identity(root)
    repo = {"head_commit": head, "tree": tree, "branch": branch,
            "detached": detached, "dirty": dirty}
    inputs = load_inputs_manifest(manifest, root, repo)
    base = {
        "schema": ARTIFACT_SCHEMA,
        "schema_version": ARTIFACT_SCHEMA_VERSION,
        "profile": profile,
        "repo": repo,
        "authorities": read_authority_versions(root),
        "declarative_contract": read_contract_identity(root,
                                                       with_sha256=True),
        "tags_at_head": tags,
        "inputs_manifest_sha256": inputs["manifest_sha256"],
        "elves": inputs["elves"],
        "toolchain": inputs["toolchain"],
    }
    base_id = hashlib.sha256(canonical_bytes(base)).hexdigest()
    families = list(options.get("require_physical") or [])
    if len(set(families)) != len(families):
        raise LedgerError(
            "duplicate required physical family; last-wins never applies "
            "to physical authority")
    preflight = None
    if options.get("preflight_receipt") is not None:
        data, digest = _load_receipt_document(
            options["preflight_receipt"], PREFLIGHT_SCHEMA,
            PREFLIGHT_SCHEMA_VERSION, "preflight receipt")
        preflight = {"sha256": digest,
                     "result": data.get("result"),
                     "commit": data.get("commit"),
                     "tree": data.get("tree")}
    physical = []
    seen_families = set()
    for receipt_path in options.get("physical_authorities") or []:
        data, digest = _load_receipt_document(
            receipt_path, PHYSICAL_SCHEMA, PHYSICAL_SCHEMA_VERSION,
            "physical authority receipt")
        receipt_family = data.get("family")
        if not isinstance(receipt_family, str) or \
                not _FAMILY_RE.match(receipt_family):
            raise LedgerError(
                "physical authority receipt has no canonical family")
        if receipt_family in seen_families:
            raise LedgerError(
                "duplicate physical authority for family %s; last-wins "
                "never applies" % receipt_family)
        seen_families.add(receipt_family)
        physical.append({
            "sha256": digest,
            "family": receipt_family,
            "result": data.get("result"),
            "commit": data.get("commit"),
            "tree": data.get("tree"),
            "base_id": data.get("base_id"),
            "attempt_id": data.get("attempt_id"),
            "result_id": data.get("result_id"),
        })
    physical.sort(key=lambda r: r["family"])
    host_authority = None
    if options.get("host_authority") is not None:
        host_authority = _load_host_authority(options["host_authority"])
    attempt = dict(base)
    attempt["schema"] = ONESHOT_SCHEMA
    attempt["schema_version"] = ONESHOT_SCHEMA_VERSION
    attempt["attempt_type"] = attempt_type
    attempt["base_id"] = base_id
    attempt["family"] = family
    attempt["host_authority"] = host_authority
    attempt["required_physical_families"] = sorted(families)
    attempt["preflight_receipt"] = preflight
    attempt["physical_receipts"] = physical
    attempt_id = hashlib.sha256(canonical_bytes(attempt)).hexdigest()
    # Final snapshot revalidation: if the repository moved while manifests,
    # receipts and ELFs were being measured, the tuple would mix two
    # generations. Refuse the hybrid instead of publishing it.
    head2, tree2, branch2, detached2, dirty2, tags2 = read_repo_identity(root)
    if (head2, tree2, branch2, detached2, dirty2, tags2) != \
            (head, tree, branch, detached, dirty, tags):
        raise LedgerError(
            "repository identity changed during derivation; refusing a "
            "hybrid attempt tuple")
    return attempt, attempt_id, base_id


# ---- private, external, symlink-free state (fix 2) ----

def _lstat(path, what):
    try:
        return os.lstat(path)
    except FileNotFoundError:
        return None
    except OSError as exc:
        raise IOFailure("cannot lstat %s %s: %s" % (what, path, exc))


def _require_private_dir(path, what):
    info = _lstat(path, what)
    if info is None:
        raise LedgerError("%s does not exist: %s" % (what, path))
    if stat_module.S_ISLNK(info.st_mode):
        raise LedgerError("%s is a symlink: %s" % (what, path))
    if not stat_module.S_ISDIR(info.st_mode):
        raise LedgerError("%s is not a directory: %s" % (what, path))
    if info.st_uid != os.geteuid():
        raise LedgerError("%s is not owned by the caller: %s" % (what, path))
    if stat_module.S_IMODE(info.st_mode) & 0o077:
        raise LedgerError(
            "%s must be private (no group/other access): %s" % (what, path))


def validate_state_root(path):
    if not isinstance(path, str) or not os.path.isabs(path):
        raise LedgerError("one-shot state root must be an absolute path")
    normalized = os.path.normpath(path)
    parts = normalized.split(os.sep)
    ancestor = os.sep
    for part in parts[1:]:
        ancestor = os.path.join(ancestor, part)
        info = _lstat(ancestor, "state root component")
        if info is None:
            raise LedgerError(
                "one-shot state root component does not exist: %s" % ancestor)
        if stat_module.S_ISLNK(info.st_mode):
            raise LedgerError(
                "one-shot state root has a symlinked component: %s" % ancestor)
        if not stat_module.S_ISDIR(info.st_mode):
            raise LedgerError(
                "one-shot state root component is not a directory: %s" %
                ancestor)
    _require_private_dir(normalized, "one-shot state root")
    for probe, refusal in (
            ("--is-inside-work-tree",
             "one-shot state root must live OUTSIDE any Git work tree"),
            ("--is-inside-git-dir",
             "one-shot state root must live OUTSIDE any Git directory"),
            ("--is-bare-repository",
             "one-shot state root must live OUTSIDE any bare Git "
             "repository")):
        try:
            proc = subprocess.run(
                ("git", "-C", normalized, "rev-parse", probe),
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        except OSError as exc:
            raise IOFailure("cannot run git: %s" % exc)
        if proc.returncode == 0 and proc.stdout.strip() == b"true":
            raise LedgerError(refusal)
    return normalized


def _revalidate_store_chain(state_root, attempt_id=None):
    """Immediately before every sensitive operation (fix 2): no late swap."""
    _require_private_dir(state_root, "one-shot state root")
    attempts = os.path.join(state_root, "attempts")
    if _lstat(attempts, "attempts directory") is not None:
        _require_private_dir(attempts, "attempts directory")
    if attempt_id is not None:
        directory = os.path.join(attempts, attempt_id)
        if _lstat(directory, "attempt directory") is not None:
            _require_private_dir(directory, "attempt directory")


def _attempt_dir(state_root, attempt_id):
    return os.path.join(state_root, "attempts", attempt_id)


def _fsync_dir(path):
    try:
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError as exc:
        raise IOFailure("cannot open directory for fsync %s: %s" %
                        (path, exc))
    try:
        _fsync(fd)
    except OSError as exc:
        raise IOFailure("cannot fsync directory %s: %s" % (path, exc))
    finally:
        os.close(fd)


# ---- hostile-store validation (fix 4) ----

def _read_store_document(path, what):
    """A store document is trusted only after full structural validation."""
    info = _lstat(path, what)
    if info is None:
        return None, None
    if stat_module.S_ISLNK(info.st_mode):
        return None, "%s is a symlink" % what
    if not stat_module.S_ISREG(info.st_mode):
        return None, "%s is not a regular file" % what
    if info.st_uid != os.geteuid():
        return None, "%s has a foreign owner" % what
    if stat_module.S_IMODE(info.st_mode) & 0o077:
        return None, "%s is not private" % what
    if info.st_size > MAX_STORE_DOCUMENT_BYTES:
        return None, "%s exceeds the document size ceiling" % what
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        raise IOFailure("cannot read %s %s: %s" % (what, path, exc))
    try:
        document = _strict_json_object(raw, what)
    except LedgerError as exc:
        return None, "%s is not strict JSON: %s" % (what, exc)
    return document, None


def _allowed_effects(profile, attempt_type):
    """The effects are POLICY, derived from profile/type alone.

    The reservation stores them for the human record, and revalidation
    recomputes them from this same function: an edited effect line is a
    tampered store, never a new permission."""
    effects = ["append-only reservation under the private state root"]
    if attempt_type == "host-battery":
        effects.append("authorizes exactly one host battery for these bytes")
    elif attempt_type == "physical-proof":
        effects.append("authorizes exactly one physical-proof round for "
                       "these bytes on explicitly authorized devices")
    elif attempt_type == "package-candidate" and profile == "PUBLIC-FINAL":
        effects.append("authorizes exactly one frozen package candidate")
    if profile == "DEV/device-matrix":
        effects.append("DEV receipt records unproven physical claims as "
                       "false and authorizes no public candidate")
    return effects


ATTEMPT_TUPLE_KEYS = frozenset(ARTIFACT_KEYS) | frozenset((
    "attempt_type", "base_id", "family", "host_authority",
    "required_physical_families", "preflight_receipt", "physical_receipts",
))


def _reservation_expected_keys(profile):
    expected = set(ATTEMPT_TUPLE_KEYS) | {"attempt_id", "allowed_effects"}
    if profile == "DEV/device-matrix":
        expected |= {"physical_support_proven", "public_candidate_authorized"}
    return expected


def _validate_reservation(document, attempt_id):
    if document.get("schema") != ONESHOT_SCHEMA or \
            not _is_schema_version(document.get("schema_version"),
                                   ONESHOT_SCHEMA_VERSION):
        return "reservation schema mismatch"
    profile = document.get("profile")
    if profile not in PROFILES:
        return "reservation profile is not canonical"
    attempt_type = document.get("attempt_type")
    if attempt_type not in ATTEMPT_TYPES:
        return "reservation attempt type is not canonical"
    if document.get("attempt_id") != attempt_id:
        return "reservation names a different attempt id"
    if set(document) != _reservation_expected_keys(profile):
        return "reservation key set is not canonical (extra or missing field)"
    family = document.get("family")
    if attempt_type == "physical-proof":
        if not isinstance(family, str) or not _FAMILY_RE.match(family):
            return "physical-proof reservation carries no canonical family"
    elif family is not None:
        return "reservation family is only valid for physical-proof"
    if attempt_type != "package-candidate" and \
            document.get("host_authority") is not None:
        return "reservation host authority is only valid for " \
               "package-candidate"
    tuple_view = {key: value for key, value in document.items()
                  if key not in RESERVATION_EXTRA_KEYS}
    tuple_view["base_id"] = document.get("base_id")
    recomputed = hashlib.sha256(canonical_bytes(tuple_view)).hexdigest()
    if recomputed != attempt_id:
        return "reservation bytes do not recompute to the attempt id"
    base_view = {key: document.get(key) for key in ARTIFACT_KEYS}
    base_view["schema"] = ARTIFACT_SCHEMA
    base_view["schema_version"] = ARTIFACT_SCHEMA_VERSION
    if hashlib.sha256(canonical_bytes(base_view)).hexdigest() != \
            document.get("base_id"):
        return "reservation bytes do not recompute to the base id"
    if document.get("allowed_effects") != \
            _allowed_effects(profile, attempt_type):
        return "reservation allowed_effects do not recompute from " \
               "profile and attempt type"
    if profile == "DEV/device-matrix":
        if document.get("physical_support_proven") is not False:
            return "DEV reservation physical_support_proven must be " \
                   "exactly false"
        if document.get("public_candidate_authorized") is not False:
            return "DEV reservation public_candidate_authorized must be " \
                   "exactly false"
    return None


RESULT_KEYS = (
    "schema", "schema_version", "attempt_id", "base_id", "profile",
    "attempt_type", "family", "result", "evidence_sha256",
    "reservation_sha256", "result_id",
)


def _result_id(document):
    core = {key: document[key] for key in RESULT_KEYS if key != "result_id"}
    return hashlib.sha256(canonical_bytes(core)).hexdigest()


def _validate_result(document, attempt_id, reservation):
    if document.get("schema") != RESULT_SCHEMA or \
            not _is_schema_version(document.get("schema_version"),
                                   RESULT_SCHEMA_VERSION):
        return "result schema mismatch"
    if set(document) != set(RESULT_KEYS):
        return "result key set is not canonical (extra or missing field)"
    if document.get("attempt_id") != attempt_id:
        return "result names a different attempt id"
    for key in ("base_id", "profile", "attempt_type", "family"):
        if document.get(key) != reservation.get(key):
            return "result %s does not match the reservation" % key
    if document.get("result") not in RESULTS:
        return "result value is not canonical"
    evidence = document.get("evidence_sha256")
    if not isinstance(evidence, str) or not _SHA256_RE.match(evidence):
        return "result evidence hash is invalid"
    anchor = document.get("reservation_sha256")
    reservation_digest = hashlib.sha256(
        canonical_bytes(reservation)).hexdigest()
    if anchor != reservation_digest:
        return "result reservation anchor does not match the stored " \
               "reservation"
    declared_id = document.get("result_id")
    if not isinstance(declared_id, str) or \
            not _SHA256_RE.match(declared_id) or \
            declared_id != _result_id(document):
        return "result bytes do not recompute to the result id"
    return None


def store_lookup(state_root, attempt_id):
    """absent, reserved, finished, interrupted or tampered -- never a guess."""
    _revalidate_store_chain(state_root, attempt_id)
    directory = _attempt_dir(state_root, attempt_id)
    info = _lstat(directory, "attempt directory")
    if info is None:
        return {"state": "absent"}
    if not stat_module.S_ISDIR(info.st_mode) or \
            stat_module.S_ISLNK(info.st_mode):
        return {"state": "tampered",
                "finding": "attempt directory is not a private real directory"}
    reservation, finding = _read_store_document(
        os.path.join(directory, "reservation.json"), "reservation")
    if finding:
        return {"state": "tampered", "finding": finding}
    result, result_finding = _read_store_document(
        os.path.join(directory, "result.json"), "result")
    if result_finding:
        return {"state": "tampered", "finding": result_finding}
    if reservation is None:
        if result is not None:
            return {"state": "tampered",
                    "finding": "result exists without a reservation"}
        return {"state": "interrupted"}
    finding = _validate_reservation(reservation, attempt_id)
    if finding:
        return {"state": "tampered", "finding": finding}
    if result is None:
        return {"state": "reserved", "reservation": reservation}
    finding = _validate_result(result, attempt_id, reservation)
    if finding:
        return {"state": "tampered", "finding": finding}
    return {"state": "finished", "reservation": reservation,
            "result": result}


def _iter_finished(state_root):
    attempts = os.path.join(state_root, "attempts")
    if _lstat(attempts, "attempts directory") is None:
        return
    _require_private_dir(attempts, "attempts directory")
    try:
        entries = sorted(os.listdir(attempts))
    except OSError as exc:
        raise IOFailure("cannot list attempts: %s" % exc)
    for attempt_id in entries:
        if not _SHA256_RE.match(attempt_id):
            continue
        view = store_lookup(state_root, attempt_id)
        if view["state"] == "finished":
            yield attempt_id, view


def evaluate_attempt(state_root, attempt, attempt_id, base_id, options):
    """Return (allowed_effects, block_reasons); never partially approve."""
    reasons = []
    repo = attempt["repo"]
    if repo["dirty"]:
        reasons.append("working tree is dirty; an attempt binds exact bytes")
    if repo["detached"] and not options.get("detached_ok"):
        reasons.append("HEAD is detached; pass --detached-ok to declare it")
    if not repo["detached"] and options.get("detached_ok"):
        reasons.append("--detached-ok declared but HEAD is on a branch")
    view = store_lookup(state_root, attempt_id)
    if view["state"] == "reserved":
        reasons.append("identity already reserved; a tuple is consumed by "
                       "reservation and never recycled")
    elif view["state"] == "finished":
        reasons.append(
            "identity already consumed with result %s; a new attempt "
            "requires a genuinely different identity" %
            view["result"].get("result"))
    elif view["state"] == "interrupted":
        reasons.append("identity carries inconclusive evidence from an "
                       "interrupted attempt; it stays consumed")
    elif view["state"] == "tampered":
        reasons.append("store evidence for this identity is invalid: %s; "
                       "the tuple stays consumed and nothing is fabricated" %
                       view.get("finding"))
    profile = attempt["profile"]
    attempt_type = attempt["attempt_type"]
    effects = _allowed_effects(profile, attempt_type)
    if attempt_type == "package-candidate":
        if profile == "DEV/device-matrix":
            reasons.append("DEV/device-matrix never authorizes a public "
                           "package candidate; PUBLIC-FINAL requires its own "
                           "explicit tuple")
        else:
            reasons.extend(_check_host_authority(
                state_root, attempt.get("host_authority"), base_id, profile))
            preflight = attempt.get("preflight_receipt")
            if preflight is None:
                reasons.append("preflight receipt missing")
            else:
                if preflight.get("result") != "PASS":
                    reasons.append("preflight receipt result is not PASS")
                if preflight.get("commit") != repo["head_commit"] or \
                        preflight.get("tree") != repo["tree"]:
                    reasons.append("preflight receipt is stale: it is not "
                                   "bound to this exact commit/tree")
            reasons.extend(_check_physical_backing(
                state_root, attempt, base_id, profile, repo))
    return effects, reasons


def _check_host_authority(state_root, authority, base_id, profile):
    """The exact host-battery attempt AND result must be pinned and intact."""
    if authority is None:
        return ["host authority missing: package-candidate requires the "
                "exact host-battery attempt id and result id"]
    view = store_lookup(state_root, authority.get("attempt_id"))
    if view["state"] != "finished":
        return ["host authority does not name a finished attempt "
                "(state: %s)" % view["state"]]
    reservation = view["reservation"]
    result = view["result"]
    reasons = []
    if reservation.get("attempt_type") != "host-battery":
        reasons.append("host authority is not a host-battery attempt")
    if reservation.get("profile") != profile:
        reasons.append("host authority was recorded under another profile")
    if reservation.get("base_id") != base_id:
        reasons.append("host authority binds another artifact identity "
                       "(manifest, ELves, toolchain or repo differ)")
    if result.get("result") != "PASS":
        reasons.append("host authority result is not PASS")
    if result.get("result_id") != authority.get("result_id"):
        reasons.append("host authority result id does not match the sealed "
                       "result; the authority is stale or the store was "
                       "altered")
    return reasons


def _check_physical_backing(state_root, attempt, base_id, profile, repo):
    """Per-family physical receipts and their backing proof attempts."""
    reasons = []
    families = attempt.get("required_physical_families") or []
    if not families:
        reasons.append("PUBLIC-FINAL requires the explicit list of "
                       "physical families; none was declared")
    provided = {r["family"]: r
                for r in attempt.get("physical_receipts") or ()}
    for family in provided:
        if family not in families:
            reasons.append("physical authority for undeclared "
                           "family: %s" % family)
    for family in families:
        receipt = provided.get(family)
        if receipt is None:
            reasons.append("physical authority missing: %s" % family)
            continue
        if receipt.get("result") != "PASS":
            reasons.append(
                "physical authority is not PASS: %s" % family)
            continue
        if receipt.get("commit") != repo["head_commit"] or \
                receipt.get("tree") != repo["tree"]:
            reasons.append(
                "physical authority is stale: %s" % family)
            continue
        if receipt.get("base_id") != base_id:
            reasons.append(
                "physical authority binds another artifact: %s" %
                family)
            continue
        proof_id = receipt.get("attempt_id")
        if not isinstance(proof_id, str) or \
                not _SHA256_RE.match(proof_id):
            reasons.append(
                "physical authority names no valid physical "
                "attempt: %s" % family)
            continue
        proof = store_lookup(state_root, proof_id)
        if proof["state"] != "finished" or \
                proof["result"].get("result") != "PASS" or \
                proof["reservation"].get("attempt_type") != \
                "physical-proof" or \
                proof["reservation"].get("base_id") != base_id or \
                proof["reservation"].get("profile") != profile:
            reasons.append(
                "physical authority is not backed by a finished "
                "physical-proof PASS of this artifact and profile: "
                "%s" % family)
            continue
        if proof["reservation"].get("family") != family:
            reasons.append(
                "physical authority family does not match the proof "
                "reservation family; one proof never serves two "
                "families: %s" % family)
            continue
        if proof["result"].get("result_id") != receipt.get("result_id"):
            reasons.append(
                "physical authority result id does not match the sealed "
                "proof result: %s" % family)
    return reasons


def _attempt_report(attempt, attempt_id, base_id, effects, reasons):
    return {
        "attempt_id": attempt_id,
        "attempt_type": attempt["attempt_type"],
        "base_id": base_id,
        "blocked": bool(reasons),
        "block_reasons": list(reasons),
        "allowed_effects": list(effects),
        "profile": attempt["profile"],
    }


def _print_report(report, as_json):
    if as_json:
        sys.stdout.write(canonical_bytes(report).decode("ascii"))
        return
    sys.stdout.write("nxledger oneshot: profile=%s type=%s\n" %
                     (report["profile"], report["attempt_type"]))
    sys.stdout.write("nxledger oneshot: id=%s\n" % report["attempt_id"])
    sys.stdout.write("nxledger oneshot: base=%s\n" % report["base_id"])
    for effect in report["allowed_effects"]:
        sys.stdout.write("nxledger oneshot: effect: %s\n" % effect)
    for reason in report["block_reasons"]:
        sys.stdout.write("nxledger oneshot: BLOCKED: %s\n" % reason)
    if not report["blocked"]:
        sys.stdout.write("nxledger oneshot: ELIGIBLE\n")


def oneshot_reserve(state_root, attempt, attempt_id, base_id, effects):
    _revalidate_store_chain(state_root)
    attempts_dir = os.path.join(state_root, "attempts")
    try:
        os.mkdir(attempts_dir, 0o700)
        _fsync_dir(state_root)
    except FileExistsError:
        pass  # concurrent first creation is idempotent by design (fix 6)
    except OSError as exc:
        raise IOFailure("cannot create attempts directory: %s" % exc)
    _require_private_dir(attempts_dir, "attempts directory")
    directory = _attempt_dir(state_root, attempt_id)
    try:
        os.mkdir(directory, 0o700)
    except FileExistsError:
        raise BlockedError(
            "identity already reserved or consumed; a tuple is never "
            "recycled: %s" % attempt_id)
    except OSError as exc:
        raise IOFailure("cannot create reservation directory: %s" % exc)
    _fsync_dir(attempts_dir)
    reservation = dict(attempt)
    reservation["attempt_id"] = attempt_id
    reservation["allowed_effects"] = list(effects)
    if attempt["profile"] == "DEV/device-matrix":
        reservation["physical_support_proven"] = False
        reservation["public_candidate_authorized"] = False
    _require_private_dir(directory, "attempt directory")
    _write_new_file(canonical_bytes(reservation),
                    os.path.join(directory, "reservation.json"), directory)


def oneshot_finish(state_root, attempt_id, result, evidence_path):
    if result not in RESULTS:
        raise LedgerError("result must be one of %s" % (RESULTS,))
    if not _SHA256_RE.match(attempt_id or ""):
        raise LedgerError("attempt id must be a 64-hex SHA-256")
    view = store_lookup(state_root, attempt_id)
    if view["state"] == "absent":
        raise BlockedError("no reservation exists for %s; a result never "
                           "exists without a valid reservation" % attempt_id)
    if view["state"] == "interrupted":
        raise BlockedError("reservation for %s is inconclusive; the tuple "
                           "stays consumed" % attempt_id)
    if view["state"] == "tampered":
        raise BlockedError("store evidence for %s is invalid (%s); nothing "
                           "is fabricated and the tuple stays consumed" %
                           (attempt_id, view.get("finding")))
    if view["state"] == "finished":
        raise BlockedError("result already recorded for %s; results are "
                           "append-only and never change" % attempt_id)
    reservation = view["reservation"]
    evidence_raw = _regular_file_bytes(evidence_path, "evidence manifest")
    evidence = _strict_json_object(evidence_raw, "evidence manifest")
    if evidence.get("schema") != EVIDENCE_SCHEMA or \
            not _is_schema_version(evidence.get("schema_version"),
                                   EVIDENCE_SCHEMA_VERSION):
        raise LedgerError("evidence manifest schema is not %s/%d" %
                          (EVIDENCE_SCHEMA, EVIDENCE_SCHEMA_VERSION))
    if evidence.get("attempt_id") != attempt_id:
        raise LedgerError("evidence manifest is stale: it names attempt %s" %
                          evidence.get("attempt_id"))
    if evidence.get("result") != result:
        raise LedgerError(
            "evidence manifest declares result %r but the command declares "
            "%r; they must agree" % (evidence.get("result"), result))
    if reservation.get("attempt_type") == "package-candidate" and \
            reservation.get("profile") == "PUBLIC-FINAL":
        # The authorities were checked at reservation time, but a store that
        # changed since then must never be consumed into a package result.
        repo_view = reservation.get("repo") or {}
        stale = _check_host_authority(
            state_root, reservation.get("host_authority"),
            reservation.get("base_id"), reservation.get("profile"))
        stale += _check_physical_backing(
            state_root, reservation, reservation.get("base_id"),
            reservation.get("profile"), repo_view)
        if stale:
            raise BlockedError(
                "package authorities are no longer valid at finish time: " +
                "; ".join(stale))
    record = {
        "schema": RESULT_SCHEMA,
        "schema_version": RESULT_SCHEMA_VERSION,
        "attempt_id": attempt_id,
        "base_id": reservation.get("base_id"),
        "profile": reservation.get("profile"),
        "attempt_type": reservation.get("attempt_type"),
        "family": reservation.get("family"),
        "result": result,
        "evidence_sha256": hashlib.sha256(evidence_raw).hexdigest(),
        "reservation_sha256": hashlib.sha256(
            canonical_bytes(reservation)).hexdigest(),
    }
    record["result_id"] = _result_id(record)
    _revalidate_store_chain(state_root, attempt_id)
    directory = _attempt_dir(state_root, attempt_id)
    _require_private_dir(directory, "attempt directory")
    _write_new_file(canonical_bytes(record),
                    os.path.join(directory, "result.json"), directory)
    sys.stdout.write("nxledger oneshot: FINISHED %s result=%s\n" %
                     (attempt_id, result))
    sys.stdout.write("nxledger oneshot: result-id=%s\n" %
                     record["result_id"])


# --------------------------------------------------------------------------
# CLI

def _add_oneshot_common(parser):
    parser.add_argument("--repo", default=".")
    parser.add_argument("--state-root", required=True,
                        help="absolute, private, symlink-free directory "
                             "outside the Git tree")
    parser.add_argument("--profile", choices=PROFILES,
                        help="explicit execution profile; there is no "
                             "default")
    parser.add_argument("--attempt-type", choices=ATTEMPT_TYPES)
    parser.add_argument("--inputs-manifest")
    parser.add_argument("--detached-ok", action="store_true")
    parser.add_argument("--preflight-receipt")
    parser.add_argument("--require-physical", action="append", default=[])
    parser.add_argument("--physical-authority", action="append", default=[])
    parser.add_argument("--family",
                        help="canonical device family; required for "
                             "physical-proof, refused otherwise")
    parser.add_argument("--host-authority",
                        help="receipt pinning the exact host-battery "
                             "attempt and sealed result id; "
                             "package-candidate only")
    parser.add_argument("--json", action="store_true")


def _oneshot_prepare(args):
    state_root = validate_state_root(args.state_root)
    if not args.profile:
        raise LedgerError("an explicit --profile is required; there is no "
                          "silent default")
    if not args.attempt_type:
        raise LedgerError("an explicit --attempt-type is required")
    if not args.inputs_manifest:
        raise LedgerError("--inputs-manifest is required")
    root = repo_root(args.repo)
    options = {
        "detached_ok": args.detached_ok,
        "preflight_receipt": args.preflight_receipt,
        "require_physical": args.require_physical,
        "physical_authorities": args.physical_authority,
        "family": args.family,
        "host_authority": args.host_authority,
    }
    attempt, attempt_id, base_id = build_attempt_tuple(
        root, args.profile, args.attempt_type, args.inputs_manifest, options)
    effects, reasons = evaluate_attempt(
        state_root, attempt, attempt_id, base_id, options)
    report = _attempt_report(attempt, attempt_id, base_id, effects, reasons)
    return state_root, attempt, report


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog=TOOL_NAME,
        description="derived composition identity, one-shot attempts and "
                    "execution profiles (read-only by default)")
    parser.add_argument("--repo", default=".")
    parser.add_argument("--check", metavar="LEDGER.JSON")
    parser.add_argument("--out", metavar="PATH")
    sub = parser.add_subparsers(dest="command")
    oneshot = sub.add_parser("oneshot")
    oneshot_sub = oneshot.add_subparsers(dest="oneshot_command",
                                         required=True)
    for name in ("eligible", "reserve"):
        _add_oneshot_common(oneshot_sub.add_parser(name))
    finish = oneshot_sub.add_parser("finish")
    finish.add_argument("--state-root", required=True)
    finish.add_argument("--id", required=True)
    finish.add_argument("--result", required=True)
    finish.add_argument("--evidence-manifest", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "oneshot":
            if args.oneshot_command == "finish":
                oneshot_finish(validate_state_root(args.state_root),
                               args.id, args.result, args.evidence_manifest)
                return 0
            state_root, attempt, report = _oneshot_prepare(args)
            _print_report(report, args.json)
            if report["blocked"]:
                return 3
            if args.oneshot_command == "reserve":
                oneshot_reserve(state_root, attempt, report["attempt_id"],
                                report["base_id"],
                                report["allowed_effects"])
                sys.stdout.write("nxledger oneshot: RESERVED %s\n" %
                                 report["attempt_id"])
            return 0
        root = repo_root(args.repo)
        if args.check is not None:
            return 0 if check(root, args.check) else 1
        ledger = derive(root)
        data = canonical_bytes(ledger)
        if args.out is not None:
            write_atomic(data, args.out, root)
        sys.stdout.write(data.decode("ascii"))
        return 0
    except LedgerError as exc:
        sys.stderr.write("nxledger: ERROR %s\n" % exc)
        return 2
    except BlockedError as exc:
        sys.stderr.write("nxledger: BLOCKED %s\n" % exc)
        return 3
    except IOFailure as exc:
        sys.stderr.write("nxledger: IO-FAILURE %s\n" % exc)
        return 4


if __name__ == "__main__":
    sys.exit(main())
