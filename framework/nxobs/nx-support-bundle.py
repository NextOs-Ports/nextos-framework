#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Create a bounded, sanitized support bundle from runtime logs.

The source logs remain private.  The published bundle contains only finite
event classes, stable reason codes, selected capability facts and SHA-256
fingerprints that let a maintainer correlate the original evidence later.
"""

from __future__ import print_function

import argparse
import datetime
import errno
import hashlib
import io
import collections
import json
import os
import pathlib
from pathlib import Path
import re
import secrets
import shutil
import stat
import sys
import zipfile


TOOL_VERSION = "0.3.1"
COMPONENT_VERSION_PATH = pathlib.Path(__file__).resolve().parent / "VERSION"
try:
    NXOBS_COMPONENT_VERSION = COMPONENT_VERSION_PATH.read_text(
        encoding="utf-8").strip()
except OSError:
    NXOBS_COMPONENT_VERSION = "unknown"
SCHEMA_VERSION = 1
MAX_INPUT_BYTES = 8 * 1024 * 1024
MAX_LINE_BYTES = 65536
MAX_EVENTS = 2048
ID = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
RUN_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,95}$")
PRIVATE = re.compile(
    r"(?:\b(?:10|127|169\.254|192\.168|172\.(?:1[6-9]|2\d|3[01]))"
    r"(?:\.\d{1,3}){2,3}\b|(?:/[A-Za-z0-9._-]+){2,}|"
    r"\b(?:root|admin|user)@|(?:password|passwd|credential|token|secret)\s*[=:])",
    re.IGNORECASE)
SENSITIVE_ASSIGNMENT = re.compile(
    r"(?P<prefix>^|[\s,;?&{(\[])"
    r"[\"']?(?P<key>[A-Za-z][A-Za-z0-9_.-]{0,63})[\"']?"
    r"\s*(?:=|:)\s*",
    re.IGNORECASE)
BEARER_SCHEME = re.compile(
    r"(?:^|[^A-Za-z0-9])bearer(?:$|[^A-Za-z0-9])", re.IGNORECASE)
SENSITIVE_CLASS_WORD = re.compile(
    r"(?:^|[^A-Za-z0-9])(?P<name>authorization|proxy[-_ ]authorization|"
    r"api[-_ ]key|x[-_ ]api[-_ ]key|x[-_ ]authentication|"
    r"set[-_ ]cookie|cookie)(?:$|[^A-Za-z0-9])",
    re.IGNORECASE)
REDACTION_MARKERS = {
    "authorization": "redacted-authorization",
    "cookie": "redacted-cookie",
    "credential": "redacted-credential",
    "query": "redacted-query",
}
PRIVATE_DETAIL_FIELDS = {
    "address", "device", "devicemodel", "host", "hostname", "ip",
    "ipaddress", "path",
}
HEX64 = re.compile(r"^[0-9a-f]{64}$")
SAFE_TEXT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ._:+/()@-]{0,95}$")
EVENT_SOURCES = {
    "bootstrap", "extractor", "loader", "graphics", "audio", "input",
    "lifecycle", "diagnostic",
}
EVENT_STATUSES = {"begin", "ok", "failed", "observed", "skipped"}
# Free-form producers write NXEVENT details; these host/identity-revealing keys
# are reserved and dropped from the shared bundle. The sanitized host
# capabilities the bundle DOES keep are built by parse_nxcompat itself, never
# copied verbatim from a producer's detail blob.
RESERVED_DETAIL_KEYS = frozenset({
    "host", "hostname", "user", "username", "home", "ip", "ipv4", "ipv6",
    "mac", "serial", "uuid", "machine_id",
})
CRASH_FIELDS = {
    "schema", "schema_version", "signal", "status", "phase", "frame",
    "thread", "pc", "lr", "sp", "fault_address", "module", "build_id",
    "module_offset", "last_asset", "last_graphics_call", "provider", "maps",
}
ADDRESS = re.compile(r"^0x[0-9a-f]+$")
BUILD_ID = re.compile(r"^(?:[0-9a-f]{8,128}|unavailable)$")
CRASH_TEXT = re.compile(r"^[A-Za-z0-9._:+@-]{1,63}$")


class BundleError(Exception):
    pass


def sha256_bytes(value):
    return hashlib.sha256(value).hexdigest()


def file_bytes(path, maximum=MAX_INPUT_BYTES):
    path = Path(path)
    try:
        info = path.lstat()
    except OSError as error:
        raise BundleError("input unavailable") from error
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise BundleError("input must be a regular non-symlink file")
    if info.st_size > maximum:
        raise BundleError("input exceeds the bounded size")
    with path.open("rb") as stream:
        value = stream.read(maximum + 1)
    if len(value) > maximum:
        raise BundleError("input grew beyond the bounded size")
    return value


def bounded_lines(value):
    lines = value.splitlines()
    for line in lines:
        if len(line) > MAX_LINE_BYTES:
            raise BundleError("log line exceeds the bounded size")
    return [line.decode("utf-8", "replace") for line in lines]


def strict_json_bytes(value, context):
    if value.startswith(b"\xef\xbb\xbf"):
        raise BundleError("%s contains a UTF-8 BOM" % context)

    def unique(pairs):
        result = {}
        for key, item in pairs:
            if key in result:
                raise BundleError("%s contains a duplicate key" % context)
            result[key] = item
        return result

    def constant(item):
        raise BundleError("%s contains non-JSON constant %s" %
                          (context, item))

    try:
        return json.loads(value.decode("utf-8"), object_pairs_hook=unique,
                          parse_constant=constant)
    except (UnicodeError, ValueError) as error:
        raise BundleError("%s is malformed: %s" % (context, error))


def compact_field_name(value):
    if not isinstance(value, str):
        return ""
    return re.sub(r"[^a-z0-9]", "", value.lower())


def sensitive_field_kind(value):
    """Return a finite credential class for a structured field name."""
    compact = compact_field_name(value)
    if compact in {
            "authorization", "proxyauthorization", "xauthentication",
            "authentication", "auth", "oauth", "oauth2"}:
        return "authorization"
    if compact in {"cookie", "cookies", "setcookie"}:
        return "cookie"
    if (compact in {
            "apikey", "xapikey", "key", "keys", "token", "tokens",
            "accesstoken", "refreshtoken", "idtoken", "authtoken",
            "session", "sessions", "sessionid", "sessionkey",
            "credential", "credentials", "password", "passwd", "secret",
            "clientsecret", "privatekey"} or compact.endswith("token")):
        return "credential"
    return None


def sensitive_text_kind(value):
    """Classify credentials embedded in bounded text, JSON or query syntax."""
    if not isinstance(value, str):
        return None
    if BEARER_SCHEME.search(value):
        return "authorization"
    for match in SENSITIVE_ASSIGNMENT.finditer(value):
        kind = sensitive_field_kind(match.group("key"))
        if kind is None:
            continue
        if match.group("prefix") in ("?", "&", ";"):
            return "query"
        return kind
    class_word = SENSITIVE_CLASS_WORD.search(value)
    if class_word is not None:
        return sensitive_field_kind(class_word.group("name"))
    return None


def private_detail_field(value):
    return compact_field_name(value) in PRIVATE_DETAIL_FIELDS


def safe_detail_key(value):
    if (not isinstance(value, str) or not SAFE_TEXT.fullmatch(value) or
            PRIVATE.search(value)):
        return None
    return value


def safe_text(value, fallback="unknown"):
    sensitive = sensitive_text_kind(value)
    if sensitive is not None:
        return REDACTION_MARKERS[sensitive]
    if not isinstance(value, str) or not SAFE_TEXT.fullmatch(value):
        return fallback
    if PRIVATE.search(value):
        return fallback
    return value


def append_event(events, run_id, source, phase, status, reason_code,
                 details=None, source_time=None, monotonic_ns=None,
                 duration_ns=None):
    if len(events) >= MAX_EVENTS:
        raise BundleError("event count exceeds the bounded maximum")
    if source not in EVENT_SOURCES or status not in EVENT_STATUSES:
        raise BundleError("event enum is outside the finite schema")
    if not isinstance(reason_code, int) or reason_code < 0 or reason_code > 9999:
        raise BundleError("event reason code is invalid")
    event = {
        "schema": "nx-event-v1",
        "schema_version": 1,
        "sequence": len(events) + 1,
        "run_id": run_id,
        "source": source,
        "phase": safe_text(phase),
        "status": status,
        "reason_code": reason_code,
        "source_time": source_time,
        "monotonic_ns": monotonic_ns,
        "duration_ns": duration_ns,
        "details": details or {},
    }
    events.append(event)


# The launcher writes TWO shapes of nx-event-v1: phase records carrying a
# numeric reason_code, and the NXU/NXR receipt families carrying a string
# `code` and no reason_code at all.  The second shape has been written by
# every published port since V3 -- an install that rebuilds its cache emits
# NXU0012 -- and this reader refused it, so the support bundle failed closed on
# exactly the ports that had something worth diagnosing.  The literal code is
# kept in details; the numeric namespaces are disjoint from the phase codes so
# nothing is conflated.
EVENT_CODE_NAMESPACES = {"NXU": 7000, "NXR": 8000}


def reason_code_from_code(value):
    """Map an NXU####/NXR#### receipt code onto its numeric namespace.

    Returns None when the field is absent or not one of those families, so a
    record with neither a reason_code nor a well-formed code stays refused.
    """
    if not isinstance(value, str) or len(value) != 7:
        return None
    base = EVENT_CODE_NAMESPACES.get(value[:3])
    if base is None or not value[3:].isdigit():
        return None
    numeric = base + int(value[3:])
    if numeric > 9999:
        return None
    return numeric


class EventWindow:
    """Bounded most-recent window over produced events.

    MAX_EVENTS is a memory bound against hostile input, not a claim that a
    longer input is malformed. Both the runtime log and events.jsonl routinely
    carry more records than that -- their budgets are 4 MiB and 1 MiB -- so
    enforcing the cap by refusing took the whole bundle down on healthy long
    sessions. Losing old events is acceptable; losing the bundle is not. The
    window keeps the END, because that is where the failure being diagnosed
    is, and reports what it dropped so the truncation is visible.

    One implementation, both callers: this defect was fixed once in
    events.jsonl and left standing in the runtime log, which is the primary
    input.
    """

    def __init__(self, events):
        self.window = collections.deque(
            maxlen=max(1, MAX_EVENTS - len(events) - 1))
        self.dropped = 0

    def add(self, produced):
        for event in produced:
            if len(self.window) == self.window.maxlen:
                self.dropped += 1
            self.window.append(event)

    def flush(self, events, run_id, source):
        if self.dropped:
            append_event(events, run_id, "diagnostic", source, "observed",
                         1902, {"note": "older-events-dropped",
                                "dropped_events": self.dropped})
        for event in self.window:
            event["sequence"] = len(events) + 1
            events.append(event)


def parse_explicit_event(line, events, run_id):
    marker = "NXEVENT "
    if not line.startswith(marker):
        return False
    try:
        item = json.loads(line[len(marker):])
    except (TypeError, ValueError):
        raise BundleError("malformed NXEVENT record")
    if item.get("schema") != "nx-event-v1":
        raise BundleError("unknown NXEVENT schema")
    monotonic_ns = item.get("monotonic_ns")
    duration_ns = item.get("duration_ns")
    for name, value in (("monotonic_ns", monotonic_ns),
                        ("duration_ns", duration_ns)):
        if value is not None and (not isinstance(value, int) or value < 0):
            raise BundleError("invalid %s" % name)
    raw_details = item.get("details", {})
    if not isinstance(raw_details, dict) or len(raw_details) > 16:
        raise BundleError("NXEVENT details are not bounded")
    details = {}
    for key, value in raw_details.items():
        if private_detail_field(key):
            continue
        sensitive_key = sensitive_field_kind(key)
        if sensitive_key is not None:
            details["redacted_%s" % sensitive_key] = True
            continue
        safe_key = safe_detail_key(key)
        if safe_key is None:
            continue
        if safe_key in RESERVED_DETAIL_KEYS:
            continue
        if isinstance(value, bool) or isinstance(value, int):
            details[safe_key] = value
        elif isinstance(value, str):
            details[safe_key] = safe_text(value)
    reason_code = item.get("reason_code")
    if reason_code is None:
        reason_code = reason_code_from_code(item.get("code"))
        if reason_code is not None and "code" not in details:
            details["code"] = safe_text(item.get("code"))
    append_event(events, run_id, item.get("source"), item.get("phase"),
                 item.get("status"), reason_code, details,
                 safe_text(item.get("source_time"), None), monotonic_ns,
                 duration_ns)
    return True


def parse_events_jsonl(raw, events, run_id):
    """V3-OBS-01: native events.jsonl input (one nx-event-v1 object per line).

    Reuses the exact NXEVENT validation path. A torn final line (interrupted
    writer) is tolerated as an observed diagnostic; a malformed line anywhere
    else fails closed like any other hostile input.
    """
    lines = [line for line in bounded_lines(raw)]
    # MAX_EVENTS is a memory bound against hostile input, not a statement that
    # a longer file is malformed. The runtime's events budget is 1 MiB, which
    # holds thousands of records, so a healthy long session used to blow this
    # cap and take the WHOLE bundle down -- the same fail-closed-on-real-data
    # shape as the missing reason_code. Losing old events is acceptable;
    # losing the bundle is not. Keep a bounded window of the most recent
    # records, because the failure being diagnosed is at the end, and say how
    # many were dropped.
    window = EventWindow(events)
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped:
            continue
        scratch = []
        try:
            parse_explicit_event("NXEVENT " + stripped, scratch, run_id)
        except BundleError:
            if index == len(lines) - 1:
                scratch = []
                append_event(scratch, run_id, "diagnostic", "events-file",
                             "observed", 1901,
                             {"note": "torn-final-line"})
                window.add(scratch)
                break
            raise
        window.add(scratch)
    window.flush(events, run_id, "events-file")


def receipt_summary(receipt):
    if not isinstance(receipt, dict):
        return None
    result = {}
    for key in ("source", "generation", "proof_flags", "connected_count",
                "frequency", "channels", "samples", "backend", "gl_vendor",
                "gl_renderer", "gl_version", "egl_vendor", "egl_version"):
        value = receipt.get(key)
        if isinstance(value, bool) or isinstance(value, int):
            result[key] = value
        elif isinstance(value, str):
            result[key] = safe_text(value)
    for key in ("window", "drawable", "gles", "rgba"):
        value = receipt.get(key)
        if (isinstance(value, list) and len(value) <= 4 and
                all(isinstance(item, int) and 0 <= item <= 32768
                    for item in value)):
            result[key] = value
    return result


GRAPHICS_EVIDENCE_KEEP = (
    "generation", "commit", "build_id", "egl_build_id", "sdl", "cfw",
    "requested", "obtained", "drawable", "pre_drawable", "shader_probe",
    "verdict", "reason", "phase", "first_present", "port_id", "port_version",
)


def parse_graphics_evidence(line, events, run_id):
    """Ingest the nxgl graphics-contract receipts (V4-GRAPHICS-04).

    The final GRAPHICS-EVIDENCE line is the proof nxrelease later binds a
    physical claim to; before 0.4.1 the shared bundle dropped it on the
    floor, so a post-first-present proof survived only in a raw log nobody
    is asked to send. Only allowlisted, sanitized fields are kept -- the
    provider DSO paths are private and never copied. The pre-present
    diagnostic line is preserved as OBSERVED only: an
    awaiting-first-present state is never represented as ok."""
    marker = "GRAPHICS-PREPRESENT-EVIDENCE:"
    offset = line.find(marker)
    if offset >= 0:
        details = {}
        for token in line[offset + len(marker):].strip().split():
            if "=" not in token:
                return False
            key, value = token.split("=", 1)
            key = safe_detail_key(key)
            if key is None or key in RESERVED_DETAIL_KEYS:
                continue
            if len(details) >= 8:
                break
            details[key] = safe_text(value)
        if not details:
            return False
        append_event(events, run_id, "graphics", "pre-present", "observed",
                     1602, details)
        return True
    marker = "GRAPHICS-EVIDENCE:"
    offset = line.find(marker)
    if offset < 0:
        return False
    fields = {}
    for token in line[offset + len(marker):].strip().split():
        if "=" not in token:
            return False
        key, value = token.split("=", 1)
        if key in fields:
            return False
        fields[key] = value
    if "verdict" not in fields or "reason" not in fields:
        return False
    details = {}
    for key in GRAPHICS_EVIDENCE_KEEP:
        value = fields.get(key)
        if isinstance(value, str) and value and value != "-":
            details[key] = safe_text(value)
    status = "ok" if fields.get("verdict") == "OK" else "failed"
    append_event(events, run_id, "graphics", "evidence", status, 1601,
                 details)
    return True


NXINPUT_MARKERS = (
    ("NXINPUT-LOAD:", "load", 1620),
    ("NXINPUT-CAPABILITIES:", "capabilities", 1621),
    ("NXINPUT-BINDING:", "binding", 1622),
    ("NXINPUT-CHORD:", "chord", 1623),
    ("NXINPUT-EVENT:", "event", 1624),
    ("NXINPUT-CONSUMER:", "consumer", 1625),
)
NXINPUT_KEEP = frozenset((
    "schema", "run", "gen", "consumer", "seq", "source", "entries",
    "map_sha256", "guid_requested", "guid_selected", "priority", "result",
    "pad", "buttons", "axes", "hats", "ordinal_max", "low_keys",
    "gamepad_range", "analog_axes", "digest", "control", "physical", "kind",
    "ordinal", "semantic", "sink", "reachable", "trigger_kind", "select",
    "start", "same_instance", "state", "pair", "exit", "phase", "context",
    "first", "x", "y", "dz", "value", "thr", "never_pressed", "dropped",
    "side", "min", "max", "min_x", "max_x", "min_y", "max_y", "action",
    "delivery",
))


def parse_nxinput_observe(line, events, run_id):
    """Ingest the nxinput_observe receipts (V4-CONTROLLERS-03 / C2).

    The shared bundle preserves the sanitized input observability -- where
    the mapping came from, how each canonical control was bound, the
    SELECT+START chord attestations, bounded events and consumer deliveries
    -- as OBSERVED events only: ingestion never promotes any state, and a
    pending/not-instrumented consumer stays exactly that. Only allowlisted
    key=value fields survive; anything else (and any unparsable token) is
    dropped, so a personal path or free-form name can never ride along."""
    for marker, phase, reason in NXINPUT_MARKERS:
        offset = line.find(marker)
        if offset < 0:
            continue
        details = {}
        for token in line[offset + len(marker):].strip().split():
            if "=" not in token:
                return False
            key, value = token.split("=", 1)
            if key not in NXINPUT_KEEP or key in RESERVED_DETAIL_KEYS:
                continue
            if len(details) >= 24:
                break
            details[key] = safe_text(value)
        if not details:
            return False
        append_event(events, run_id, "input", "observe-" + phase, "observed",
                     reason, details)
        return True
    return False


NXC6_MARKERS = (
    ("NXC6-SEAM ", "seam", 1630),
    ("NXC6-DOMAIN ", "domain", 1631),
)
NXC6_KEEP = frozenset((
    "seq", "sdl", "instance", "guid", "stage", "result", "reason", "source",
    "step_env", "step_cfw", "step_bundle", "step_builtin", "step_raw",
    "readback_checked", "source_crc_aliases", "dup_lastwins", "domain_lines",
    "domain_bindings", "source_domain", "target_domain", "name", "db_class",
    "db_target", "db_retries", "db_elapsed_ms", "face_layout",
    "effective_guid", "map_fnv1a64", "map_bytes", "buttons", "axes", "hats",
    "resolutions", "admitted", "forgotten", "still_admitted", "matching",
    "rewritten", "rewritten_bindings", "native", "identical", "ambiguous",
    "invalid", "volume_markers", "other_instance", "same_guid",
    "divergent_mapping", "native_behaviour",
))


def parse_nxc6_receipt(line, events, run_id):
    """Ingest the C6 admission receipts (nxinput 0.10.0, contract 5.9).

    The seam already writes every decision to the port's normal log; the
    shared bundle preserves the sanitized subset -- which authority won,
    why each earlier step yielded, the domain classification counts, the
    live-database acquisition class/target/retries and the FACE_LAYOUT --
    as OBSERVED events only. Only allowlisted key=value fields survive;
    free-form prose, pids, timestamps and anything unlisted are dropped, so
    a personal path or raw mapping can never ride along. Ingestion promotes
    nothing: a blocked pad stays a blocked pad."""
    for marker, phase, reason in NXC6_MARKERS:
        offset = line.find(marker)
        if offset < 0:
            continue
        details = {}
        for token in line[offset + len(marker):].strip().split():
            if "=" not in token:
                continue  # bounded prose inside one detail is not a field
            key, value = token.split("=", 1)
            if key not in NXC6_KEEP or key in RESERVED_DETAIL_KEYS:
                continue
            if len(details) >= 32:
                break
            details[key] = safe_text(value)
        if not details:
            return False
        append_event(events, run_id, "input", "c6-" + phase, "observed",
                     reason, details)
        return True
    return False


def parse_nxcompat(line, events, run_id):
    marker = "NXCOMPAT_REPORT "
    offset = line.find(marker)
    if offset < 0:
        return False
    try:
        encoded = line[offset + len(marker):].lstrip()
        report, _unused_offset = json.JSONDecoder().raw_decode(encoded)
    except (TypeError, ValueError):
        append_event(events, run_id, "diagnostic", "capability-report",
                     "failed", 1950)
        return True
    phase = safe_text(report.get("phase"), "capabilities")
    details = {"report_reason_code": int(report.get("report_reason_code", 0))}
    host = report.get("host", {})
    if isinstance(host, dict):
        details["host"] = {
            key: safe_text(host.get(key))
            for key in ("process_arch", "kernel_arch", "libc", "memory_class",
                        "filesystem_class")
        }
    decisions = []
    for item in report.get("capabilities", []):
        if not isinstance(item, dict) or len(decisions) >= 64:
            continue
        identifier = safe_text(item.get("id"))
        state = safe_text(item.get("state"))
        reason = item.get("reason_code")
        if identifier != "unknown" and state != "unknown" and isinstance(reason, int):
            decisions.append({"id": identifier, "state": state,
                              "reason_code": reason})
    details["capabilities"] = decisions
    receipts = {}
    for name in ("graphics", "audio", "input"):
        value = receipt_summary(report.get("receipts", {}).get(name))
        if value:
            receipts[name] = value
    details["receipts"] = receipts
    append_event(events, run_id, "bootstrap", phase, "observed",
                 int(report.get("report_reason_code", 0)), details)
    return True


def parse_perf_receipt(line, scratch, run_id):
    """Ingest the PERF:/VSYNC: receipts the runtime sampler prints.

    These are the whole deliverable of the PERF and VSYNC debts, and the
    bundle -- the artifact a player actually shares -- dropped both on the
    floor: the numbers survived only in a raw log nobody is asked to send.
    A driver that silently ignores the requested swap interval is exactly
    what has to be visible here.
    """
    for marker, phase, reason in (("PERF: ", "boot-cost", 1810),
                                  ("VSYNC: ", "present", 1811)):
        if not line.startswith(marker):
            continue
        details = {}
        for token in line[len(marker):].strip().split():
            if "=" not in token:
                return False
            key, value = token.split("=", 1)
            key = safe_detail_key(key)
            if key is None or key in RESERVED_DETAIL_KEYS:
                continue
            if len(details) >= 16:
                break
            try:
                details[key] = int(value, 0)
            except ValueError:
                details[key] = safe_text(value)
        if not details:
            return False
        append_event(scratch, run_id, "graphics" if reason == 1811 else
                     "lifecycle", phase, "observed", reason, details)
        return True
    return False


def parse_runtime(lines, events, run_id):
    # Every event this produces goes through the bounded window: a 4 MiB
    # runtime log holds far more records than MAX_EVENTS, and refusing it
    # took the whole bundle down on healthy long sessions.
    window = EventWindow(events)
    for index, line in enumerate(lines):
        scratch = []
        try:
            if parse_explicit_event(line, scratch, run_id):
                window.add(scratch)
                continue
        except BundleError:
            # A runtime log can be truncated at the FRONT by anything that
            # bounds it -- the launcher's own budget trim, a card that filled
            # up mid-write, a copy that started late. The first surviving line
            # is then half a record that still begins with the marker. That is
            # a torn line, not hostile input, and it must not cost the whole
            # bundle. Anywhere else, a malformed record still fails closed.
            if index == 0:
                append_event(scratch, run_id, "diagnostic", "runtime-log",
                             "observed", 1903, {"note": "torn-first-line"})
                window.add(scratch)
                continue
            raise
        if parse_perf_receipt(line, scratch, run_id):
            window.add(scratch)
            continue
        if parse_graphics_evidence(line, scratch, run_id):
            window.add(scratch)
            continue
        if parse_nxinput_observe(line, scratch, run_id):
            window.add(scratch)
            continue
        if parse_nxc6_receipt(line, scratch, run_id):
            window.add(scratch)
            continue
        if parse_nxcompat(line, scratch, run_id):
            if "game exited with status 0" in line:
                append_event(scratch, run_id, "lifecycle", "shutdown", "ok",
                             1802)
            window.add(scratch)
            continue
        lower = line.lower()
        if "run_start_utc=" in line:
            append_event(scratch, run_id, "bootstrap", "run", "begin", 1000)
        elif "running NXExtract" in line:
            append_event(scratch, run_id, "extractor", "validation", "begin",
                         1400)
        elif line.startswith("NXLOADER[") and " loaded " in line:
            append_event(scratch, run_id, "loader", "mapping", "ok", 1500)
        elif line.startswith("NXLOADER prepared"):
            append_event(scratch, run_id, "loader", "relocations", "ok", 1501)
        elif "constructors -> JNI_OnLoad" in line:
            append_event(scratch, run_id, "loader", "initializers-jni",
                         "begin", 1502)
        elif "NXLOADER game READY JNI=" in line:
            append_event(scratch, run_id, "loader", "jni", "ok", 1503)
        elif line.startswith("NXGL[") or line.startswith("VIDEO "):
            append_event(scratch, run_id, "graphics", "context", "observed",
                         1600)
        elif line.startswith("AUDIO:") or "audio opened:" in line:
            append_event(scratch, run_id, "audio", "output", "ok", 1700)
        elif "Entering main loop" in line:
            append_event(scratch, run_id, "lifecycle", "loop", "ok", 1800)
        elif "nativeOnPause" in line:
            append_event(scratch, run_id, "lifecycle", "pause-save", "ok",
                         1801)
        elif "game exited with status 0" in line:
            append_event(scratch, run_id, "lifecycle", "shutdown", "ok", 1802)
        elif ("error" in lower or "failed" in lower or "fatal" in lower) and not (
                "errors=0" in lower or "failed=0" in lower):
            append_event(scratch, run_id, "diagnostic", "runtime", "failed",
                         1900)
        window.add(scratch)
    window.flush(events, run_id, "runtime-log")


def parse_extractor(lines, events, run_id):
    # Third reader, same bound, same reasoning: the compact extractor log is
    # budgeted at 2 MiB and a recipe with hundreds of payload candidates emits
    # far more than MAX_EVENTS matching lines.
    window = EventWindow(events)
    timestamp = re.compile(r"^\[([0-9]{4}-[0-9]{2}-[0-9]{2} [0-9:]{8})\] (.*)$")
    for line in lines:
        scratch = []
        match = timestamp.match(line)
        if not match:
            continue
        source_time, message = match.groups()
        lower = message.lower()
        if "=== nxextract" in lower:
            phase, status, reason = "run", "begin", 1400
        elif "fast validation marker accepted" in lower:
            phase, status, reason = "validation", "ok", 1401
        elif "adopted fully validated" in lower:
            phase, status, reason = "adoption", "ok", 1402
        elif "validated payload committed" in lower:
            phase, status, reason = "commit", "ok", 1403
        elif "installation complete" in lower:
            phase, status, reason = "install", "ok", 1404
        elif "rejected" in lower:
            phase, status, reason = "validation", "observed", 1405
        elif "failed" in lower or "error" in lower:
            phase, status, reason = "run", "failed", 1499
        else:
            continue
        append_event(scratch, run_id, "extractor", phase, status, reason,
                     source_time=source_time)
        window.add(scratch)
    window.flush(events, run_id, "extractor-log")


def parse_crash_receipt(value, events, run_id):
    lines = value.splitlines()
    if len(lines) != 1:
        raise BundleError("crash receipt must contain exactly one record")
    item = strict_json_bytes(lines[0], "crash receipt")
    if not isinstance(item, dict) or set(item) != CRASH_FIELDS:
        raise BundleError("crash receipt fields differ from nx-crash-v1")
    if item.get("schema") != "nx-crash-v1" or item.get("schema_version") != 1:
        raise BundleError("unsupported crash receipt schema")
    signal_number = item.get("signal")
    status = item.get("status")
    frame = item.get("frame")
    thread = item.get("thread")
    if (isinstance(signal_number, bool) or not isinstance(signal_number, int) or
            signal_number < 1 or signal_number > 64 or
            status != 128 + signal_number):
        raise BundleError("crash signal/status contract is invalid")
    if (isinstance(frame, bool) or not isinstance(frame, int) or frame < 0 or
            isinstance(thread, bool) or not isinstance(thread, int) or
            thread < 0):
        raise BundleError("crash frame/thread contract is invalid")
    for field in ("pc", "lr", "sp", "fault_address", "module_offset"):
        if not isinstance(item[field], str) or not ADDRESS.fullmatch(item[field]):
            raise BundleError("crash receipt %s is invalid" % field)
    if (not isinstance(item["build_id"], str) or
            not BUILD_ID.fullmatch(item["build_id"])):
        raise BundleError("crash receipt build_id is invalid")
    text_fields = (
        "phase", "module", "last_asset", "last_graphics_call", "provider",
        "maps",
    )
    details = {
        "signal": signal_number,
        "status": status,
        "frame": frame,
        "thread": thread,
    }
    for field in text_fields:
        value = item[field]
        if not isinstance(value, str):
            raise BundleError("crash receipt %s is not sanitized" % field)
        sensitive = sensitive_text_kind(value)
        if sensitive is not None:
            details[field] = REDACTION_MARKERS[sensitive]
            continue
        if (not CRASH_TEXT.fullmatch(value) or
                safe_text(value, None) != value):
            raise BundleError("crash receipt %s is not sanitized" % field)
        details[field] = value
    for field in ("pc", "lr", "sp", "fault_address", "module_offset",
                  "build_id"):
        details[field] = item[field]
    append_event(events, run_id, "diagnostic", item["phase"], "failed",
                 1910 + signal_number, details)
    return item


def write_file(path, value):
    path = Path(path)
    with path.open("xb") as stream:
        stream.write(value)
        stream.flush()
        os.fsync(stream.fileno())


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=True, sort_keys=True,
                       separators=(",", ":")) + "\n").encode("utf-8")


def artifact_components(value):
    """Read only finite component hashes from the package's own manifest."""
    wanted = {
        "chrono-universal": "loader-adapter-shims",
        "nxbootstrap.sh": "nxbootstrap",
        "nxextract.py": "nxextract",
        "nxport.json": "manifest",
    }
    result = {}
    try:
        with zipfile.ZipFile(io.BytesIO(value)) as archive:
            names = [name for name in archive.namelist()
                     if name.endswith("/PACKAGE-MANIFEST.sha256")]
            if len(names) != 1:
                return result
            info = archive.getinfo(names[0])
            if info.file_size > 65536:
                return result
            manifest = archive.read(info).decode("ascii")
    except (KeyError, UnicodeError, ValueError, zipfile.BadZipFile):
        return result
    for line in manifest.splitlines():
        fields = line.split("  ", 1)
        if len(fields) != 2 or not HEX64.fullmatch(fields[0]):
            continue
        basename = fields[1].rsplit("/", 1)[-1]
        component = wanted.get(basename)
        if component:
            result[component] = fields[0]
    return result


def build_bundle(runtime_log, extractor_log, output, stack_id,
                 firmware_context, artifact=None, run_id=None,
                 created_utc=None, payload_abi="unknown",
                 rom_root_kind="unknown", writer=write_file,
                 crash_receipts=None, events_file=None):
    public_ids = (stack_id, firmware_context, payload_abi, rom_root_kind)
    if (any(not isinstance(value, str) or not ID.fullmatch(value) or
            safe_text(value, None) != value for value in public_ids)):
        raise BundleError("stack/context ID is outside the finite grammar")
    if run_id is None:
        run_id = "support-%s-%s" % (
            datetime.datetime.utcnow().strftime("%Y%m%dT%H%M%SZ"),
            secrets.token_hex(4))
    if (not isinstance(run_id, str) or not RUN_ID.fullmatch(run_id) or
            safe_text(run_id, None) != run_id):
        raise BundleError("run ID is outside the finite grammar")
    if created_utc is None:
        created_utc = datetime.datetime.utcnow().replace(
            microsecond=0).isoformat() + "Z"
    elif safe_text(created_utc, None) != created_utc:
        raise BundleError("created UTC is outside the finite grammar")
    runtime = file_bytes(runtime_log)
    extractor = file_bytes(extractor_log)
    events = []
    if events_file is not None:
        parse_events_jsonl(file_bytes(events_file), events, run_id)
    parse_extractor(bounded_lines(extractor), events, run_id)
    # NXExtract is an isolated foreground phase before the loader runtime.
    parse_runtime(bounded_lines(runtime), events, run_id)
    crash_sources = []
    crash_receipts = list(crash_receipts or [])
    if len(crash_receipts) > 8:
        raise BundleError("crash receipt count exceeds the bounded maximum")
    for crash_path in crash_receipts:
        crash_value = file_bytes(crash_path, maximum=64 * 1024)
        parse_crash_receipt(crash_value, events, run_id)
        crash_sources.append({
            "sha256": sha256_bytes(crash_value),
            "bytes": len(crash_value),
        })
    if not events:
        raise BundleError("no recognized bounded events")
    artifact_sha = None
    # V3-PERF-01: the component version and the tool's own version are
    # different facts; conflating them looked like a hybrid installation.
    components = {"nxobs": NXOBS_COMPONENT_VERSION,
                  "nx-support-bundle": TOOL_VERSION}
    if artifact is not None:
        artifact_value = file_bytes(artifact, maximum=64 * 1024 * 1024)
        artifact_sha = sha256_bytes(artifact_value)
        components.update(artifact_components(artifact_value))
    timed = sum(1 for item in events if item["monotonic_ns"] is not None)
    categories = sorted({item["source"] for item in events})
    last = events[-1]
    terminal = [item for item in events
                if item["source"] == "lifecycle" and
                item["phase"] == "shutdown" and item["status"] == "ok"]
    failures = [item for item in events if item["status"] == "failed"]
    last_completed = failures[-1] if failures else (
        terminal[-1] if terminal else last
    )
    observed_host = {}
    for event in events:
        host = event.get("details", {}).get("host")
        if isinstance(host, dict):
            observed_host = host
    report = {
        "schema": "nx-support-bundle-v1",
        "schema_version": 1,
        "run_id": run_id,
        "created_utc": created_utc,
        "stack_id": stack_id,
        "firmware_context": firmware_context,
        "payload_abi": payload_abi,
        "rom_root_kind": rom_root_kind,
        "artifact_sha256": artifact_sha,
        "components": components,
        "observed_host": observed_host,
        "path_classes": {
            "home": "port-relative", "save": "port-relative-userdata",
            "cache": "port-relative-cache",
        },
        "library_path_origins": ["private", "portmaster", "firmware"],
        "sources": {
            "runtime": {"sha256": sha256_bytes(runtime), "bytes": len(runtime),
                        "lines": len(runtime.splitlines())},
            "extractor": {"sha256": sha256_bytes(extractor),
                          "bytes": len(extractor),
                          "lines": len(extractor.splitlines())},
            "crash_receipts": crash_sources,
        },
        "event_count": len(events),
        "timed_event_count": timed,
        "categories": categories,
        "last_completed": {
            "sequence": last_completed["sequence"],
            "source": last_completed["source"],
            "phase": last_completed["phase"],
            "status": last_completed["status"],
            "reason_code": last_completed["reason_code"],
        },
        "privacy": {
            "raw_logs_included": False,
            "paths_included": False,
            "addresses_included": False,
            "hostnames_included": False,
            "credentials_included": False,
            "save_data_included": False,
            "credential_redaction": "fail-closed",
        },
        "limits": {"maximum_input_bytes": MAX_INPUT_BYTES,
                   "maximum_line_bytes": MAX_LINE_BYTES,
                   "maximum_events": MAX_EVENTS},
    }
    schema = {
        "schema": "nx-observability-schema-v1", "schema_version": 1,
        "event_schema": "nx-event-v1", "bundle_schema": "nx-support-bundle-v1",
        "crash_schema": "nx-crash-v1",
        "maximum_input_bytes": MAX_INPUT_BYTES,
        "maximum_line_bytes": MAX_LINE_BYTES, "maximum_events": MAX_EVENTS,
        "credential_redaction": {
            "mode": "fail-closed",
            "text_markers": [
                REDACTION_MARKERS["authorization"],
                REDACTION_MARKERS["cookie"],
                REDACTION_MARKERS["credential"],
                REDACTION_MARKERS["query"],
            ],
            "raw_value_retained": False,
        },
    }
    events_value = b"".join(json_bytes(item) for item in events)
    summary = (
        "NX support bundle\nrun_id=%s\nstack=%s\nevents=%d\n"
        "last=%s/%s/%s\nraw_logs_included=0\n" %
        (run_id, stack_id, len(events), last_completed["source"],
         last_completed["phase"], last_completed["status"])).encode("ascii")
    files = {
        "events.jsonl": events_value,
        "report.json": json_bytes(report),
        "schema.json": json_bytes(schema),
        "SUMMARY.txt": summary,
    }
    output = Path(output)
    parent = output.parent
    if not output.is_absolute() or output.exists() or output.is_symlink():
        raise BundleError("output must be a new absolute path")
    parent_info = parent.lstat()
    if stat.S_ISLNK(parent_info.st_mode) or not stat.S_ISDIR(parent_info.st_mode):
        raise BundleError("output parent must be a real directory")
    temporary = parent / (".%s.tmp.%d.%s" %
                          (output.name, os.getpid(), secrets.token_hex(4)))
    temporary.mkdir(mode=0o700)
    try:
        for name in sorted(files):
            writer(temporary / name, files[name])
        manifest = b"".join(
            ("%s  %s\n" % (sha256_bytes(files[name]), name)).encode("ascii")
            for name in sorted(files))
        writer(temporary / "MANIFEST.sha256", manifest)
        os.replace(str(temporary), str(output))
    except Exception:
        if temporary.exists():
            shutil.rmtree(str(temporary))
        raise
    return report


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-log", required=True)
    parser.add_argument("--extractor-log", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--stack-id", required=True)
    parser.add_argument("--firmware-context", required=True)
    parser.add_argument("--artifact")
    parser.add_argument("--run-id")
    parser.add_argument("--created-utc")
    parser.add_argument("--payload-abi", default="unknown")
    parser.add_argument("--rom-root-kind", default="unknown")
    parser.add_argument("--crash-receipt", action="append", default=[])
    parser.add_argument("--events-file")
    args = parser.parse_args(argv)
    try:
        report = build_bundle(
            args.runtime_log, args.extractor_log, args.output, args.stack_id,
            args.firmware_context, args.artifact, args.run_id, args.created_utc,
            args.payload_abi, args.rom_root_kind,
            crash_receipts=args.crash_receipt,
            events_file=args.events_file)
    except (BundleError, OSError, ValueError) as error:
        print("nx-support-bundle: %s" % error, file=sys.stderr)
        return 1
    print("nx_support_bundle=PASS run_id=%s events=%d raw_logs=0 "
          "addresses=0 credentials=0" %
          (report["run_id"], report["event_count"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
