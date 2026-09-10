#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxscan — structural secret/credential scanner (V4-04B foundation, WIRED).

STATUS: WIRED since nxrelease 0.3.25.  ``nxrelease.py`` loads this module by
``spec_from_file_location`` (same pattern as the apkcompat contract),
validates ``SCAN_SCHEMA`` on load, and calls ``scan_bytes`` from
``_structural_secret_verdict()`` BEFORE the textual fallback for the payload
types this module actually parses.

WHY: a global regex cannot tell a Python type annotation
(``m_VCPassword: Optional[str] = None``) from a real assignment, nor a JSON
key from prose.  This module classifies by TYPE with real parsers (``ast``
for Python, structural walk for JSON) and returns a stable, sanitized
finding list — the secret VALUE is never echoed.

ADAPTER CONTRACT (as actually wired in nxrelease 0.3.25):
  1. ``FAIL`` is a proof and rejects immediately: it catches what the byte
     regex cannot see (a quoted Python literal, a ``bytes`` literal, a
     credential nested in a dict or in a JSON subtree);
  2. ``PASS`` NEVER acquits.  The historical regex authority is deliberately
     broader — it also rejects a bare *name* bound to a sensitive identifier
     (``password=abcdefgh``) — so it keeps deciding.  The adapter is
     strictly ADDITIVE and no existing rejection was weakened;
  3. ``UNSUPPORTED`` (a type not parsed here) and ``STRUCTURAL_ERROR``
     (invalid syntax, BOM, duplicate key, non-finite constant, excessive
     nesting, oversize) both fall back to the CURRENT fail-closed regex
     scanner; neither ever becomes a PASS;
  4. an exception raised here degrades to the regex path, never to approval;
  5. the private-path, hostname, IPv4 and advocacy scanners are untouched;
  6. the sensitive-name pattern below must stay byte-identical to
     ``SECRET_NAME_PATTERN`` in nxrelease.py (the parity test in
     tests/test_nxscan.py enforces it).

Everything here is read-only and deterministic: no stage, marker, cache or
file is ever created.
"""

import ast
import hashlib
import json
import re

SCAN_SCHEMA = "nx-structural-scan/1"

# Must remain byte-identical to nxrelease.py SECRET_NAME_PATTERN (decoded);
# the parity test compares both so the foundation and the live regex scanner
# never disagree about what a sensitive NAME is.
SENSITIVE_NAME_PATTERN = (
    r"(?:api[_-]?key|secret|passwd|password|bearer|credential|private[_-]?key)"
)
SENSITIVE_NAME_RE = re.compile(r"(?i)" + SENSITIVE_NAME_PATTERN)
# A "real credential literal" mirrors nxrelease's SECRET_VALUE_PATTERN shape.
CREDENTIAL_VALUE_RE = re.compile(r"[A-Za-z0-9/_+\-]{8,}\Z")

MAX_SCAN_BYTES = 16 * 1024 * 1024
MAX_JSON_DEPTH = 64

PASS = "PASS"
FAIL = "FAIL"
UNSUPPORTED = "UNSUPPORTED"
STRUCTURAL_ERROR = "STRUCTURAL_ERROR"


def _finding(code, path, message):
    """A finding never contains the secret value; only names and paths."""
    return {"code": code, "path": path, "message": message}


def _name_is_sensitive(name):
    return bool(name) and SENSITIVE_NAME_RE.search(name) is not None


def _literal_is_credential(value):
    if isinstance(value, bytes):
        try:
            value = value.decode("utf-8")
        except UnicodeDecodeError:
            return True  # opaque bytes under a sensitive key: fail closed
    if not isinstance(value, str):
        return False
    return bool(CREDENTIAL_VALUE_RE.match(value.strip()))


def _result(logical_path, language, status, findings, contract_hash):
    return {
        "schema": SCAN_SCHEMA,
        "logical_path": logical_path,
        "language": language,
        "status": status,
        "findings": findings,
        "scanner_sha256": contract_hash,
    }


def _self_hash():
    try:
        with open(__file__, "rb") as handle:
            return hashlib.sha256(handle.read()).hexdigest()
    except OSError:
        return "unavailable"


# ------------------------------------------------------------------- python


def _python_target_names(node):
    if isinstance(node, ast.Name):
        return [node.id]
    if isinstance(node, ast.Attribute):
        return [node.attr]
    if isinstance(node, (ast.Tuple, ast.List)):
        names = []
        for element in node.elts:
            names.extend(_python_target_names(element))
        return names
    return []


def _python_constant_secret(value_node):
    """A provable literal credential: a str/bytes Constant, directly or one
    simple nesting level down (tuple/list/dict literal)."""
    if isinstance(value_node, ast.Constant):
        if isinstance(value_node.value, (str, bytes)):
            return _literal_is_credential(value_node.value)
        return False
    if isinstance(value_node, (ast.Tuple, ast.List, ast.Set)):
        return any(_python_constant_secret(item) for item in value_node.elts)
    return False


def scan_python(logical_path, data):
    findings = []
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return [_finding("PY-ENCODING", logical_path,
                         "python source is not valid UTF-8")], STRUCTURAL_ERROR
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError, RecursionError):
        # Invalid Python is never declared clean: a structural finding, not a
        # traceback and not a PASS.
        return [_finding("PY-SYNTAX", logical_path,
                         "python source does not parse; it cannot be "
                         "declared clean")], STRUCTURAL_ERROR
    for node in ast.walk(tree):
        if isinstance(node, ast.AnnAssign):
            targets = _python_target_names(node.target)
            value = node.value
            if value is None:
                continue  # bare annotation: no literal at all
            if (isinstance(value, ast.Constant) and value.value is None):
                continue  # annotated None default is legitimate
            if any(_name_is_sensitive(name) for name in targets) and \
                    _python_constant_secret(value):
                findings.append(_finding(
                    "PY-SECRET-ANNASSIGN", logical_path,
                    "annotated assignment binds a sensitive name to a real "
                    "string/bytes literal"))
        elif isinstance(node, ast.Assign):
            targets = []
            for target in node.targets:
                targets.extend(_python_target_names(target))
            if any(_name_is_sensitive(name) for name in targets) and \
                    _python_constant_secret(node.value):
                findings.append(_finding(
                    "PY-SECRET-ASSIGN", logical_path,
                    "assignment binds a sensitive name to a real "
                    "string/bytes literal"))
        elif isinstance(node, ast.Dict):
            for key, value in zip(node.keys, node.values):
                if (isinstance(key, ast.Constant) and
                        isinstance(key.value, str) and
                        _name_is_sensitive(key.value) and
                        _python_constant_secret(value)):
                    findings.append(_finding(
                        "PY-SECRET-DICT", logical_path,
                        "dict literal binds a sensitive key to a real "
                        "string/bytes literal"))
    return findings, (FAIL if findings else PASS)


# --------------------------------------------------------------------- json


def _json_pairs_hook(pairs):
    seen = set()
    for key, _value in pairs:
        if key in seen:
            raise ValueError("duplicate JSON key: %r" % key)
        seen.add(key)
    return dict(pairs)


def _json_reject_constant(token):
    raise ValueError("non-finite JSON constant: %s" % token)


def _walk_json(value, path, findings, logical_path, depth=0):
    if depth > MAX_JSON_DEPTH:
        raise RecursionError("json nesting exceeds the scan depth ceiling")
    if isinstance(value, dict):
        for key, item in value.items():
            child = path + "." + key if path else key
            if (_name_is_sensitive(key) and isinstance(item, str) and
                    _literal_is_credential(item)):
                findings.append(_finding(
                    "JSON-SECRET", logical_path,
                    "sensitive key %r holds a real credential literal at %s"
                    % (key, child)))
            _walk_json(item, child, findings, logical_path, depth + 1)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            _walk_json(item, "%s[%d]" % (path, index), findings,
                       logical_path, depth + 1)


def scan_json(logical_path, data):
    if data[:3] == b"\xef\xbb\xbf":
        return [_finding("JSON-BOM", logical_path,
                         "unexpected UTF-8 BOM")], STRUCTURAL_ERROR
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return [_finding("JSON-ENCODING", logical_path,
                         "json is not valid UTF-8")], STRUCTURAL_ERROR
    try:
        document = json.loads(
            text, object_pairs_hook=_json_pairs_hook,
            parse_constant=_json_reject_constant)
    except (ValueError, RecursionError):
        # Invalid JSON or an ambiguous duplicate is never a PASS.
        return [_finding("JSON-STRUCTURE", logical_path,
                         "json does not parse strictly (syntax, duplicate "
                         "key or non-finite constant)")], STRUCTURAL_ERROR
    findings = []
    try:
        _walk_json(document, "", findings, logical_path)
    except RecursionError:
        return [_finding("JSON-DEPTH", logical_path,
                         "json nesting exceeds the scan depth "
                         "ceiling")], STRUCTURAL_ERROR
    return findings, (FAIL if findings else PASS)


# -------------------------------------------------------------------- entry


def scan_bytes(logical_path, data):
    """Classify one payload by type and scan it structurally.

    ``logical_path`` decides the language; the host path never does.  The
    result status is exactly one of PASS, FAIL, UNSUPPORTED or
    STRUCTURAL_ERROR — an UNSUPPORTED type must keep falling back to the
    caller's existing fail-closed scanner until the adapter is wired.
    """
    contract_hash = _self_hash()
    if not isinstance(logical_path, str) or not logical_path:
        return _result(str(logical_path), "unknown", STRUCTURAL_ERROR,
                       [_finding("SCAN-PATH", str(logical_path),
                                 "logical path must be a non-empty string")],
                       contract_hash)
    if not isinstance(data, (bytes, bytearray)):
        return _result(logical_path, "unknown", STRUCTURAL_ERROR,
                       [_finding("SCAN-INPUT", logical_path,
                                 "payload must be bytes")], contract_hash)
    data = bytes(data)
    if len(data) > MAX_SCAN_BYTES:
        return _result(logical_path, "unknown", STRUCTURAL_ERROR,
                       [_finding("SCAN-SIZE", logical_path,
                                 "payload exceeds the scan size ceiling")],
                       contract_hash)
    lowered = logical_path.lower()
    if lowered.endswith((".py", ".pyi")):
        findings, status = scan_python(logical_path, data)
        return _result(logical_path, "python", status, findings,
                       contract_hash)
    if lowered.endswith(".json"):
        findings, status = scan_json(logical_path, data)
        return _result(logical_path, "json", status, findings, contract_hash)
    return _result(logical_path, "unknown", UNSUPPORTED, [], contract_hash)
