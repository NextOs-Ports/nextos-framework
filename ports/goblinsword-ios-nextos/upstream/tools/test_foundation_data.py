#!/usr/bin/env python3
"""Compare the host Foundation data bridge against independent Python parsers.

Reads assets directly from the IPA without writing payload copies. Does not
execute the game. Expects the C++ test harness to have been compiled already.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile


def convert(value):
    if isinstance(value, bytes):
        return {"__data_hex__": value.hex()}
    if isinstance(value, list):
        return [convert(item) for item in value]
    if isinstance(value, dict):
        return {key: convert(item) for key, item in value.items()}
    return value


def xml_events(element):
    yield ["start", element.tag, element.attrib]
    if element.text:
        yield ["text", element.text]
    for child in element:
        yield from xml_events(child)
        if child.tail:
            yield ["text", child.tail]
    yield ["end", element.tag]


def trim_outside(events):
    while events and events[0][0] == "text" and not events[0][1].strip():
        events.pop(0)
    while events and events[-1][0] == "text" and not events[-1][1].strip():
        events.pop()
    return events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipa", type=Path)
    parser.add_argument("--harness", type=Path, required=True, help="Compiled test_foundation_data.cpp harness")
    parser.add_argument("--scratch-parent", type=Path, help="Existing directory for temporary defaults files (default: system temporary directory)")
    args = parser.parse_args()
    results = {"schema": "goblin-foundation-data-host-validation/1", "executes_guest_code": False,
               "ipa_sha256": hashlib.sha256(args.ipa.read_bytes()).hexdigest(),
               "plist_pass": 0, "xml_pass": 0, "failures": [], "malformed_rejected": []}
    def invoke(mode, data):
        return subprocess.run([str(args.harness), mode], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    with zipfile.ZipFile(args.ipa) as archive:
        for info in archive.infolist():
            if not info.filename.endswith((".plist", ".tmx")):
                continue
            content = archive.read(info)
            mode = "--plist" if info.filename.endswith(".plist") else "--xml"
            process = invoke(mode, content)
            if process.returncode:
                results["failures"].append({"asset": info.filename.rsplit("/", 1)[-1], "reason": process.stderr.decode(errors="replace").strip()})
                continue
            actual = json.loads(process.stdout)
            expected = convert(plistlib.loads(content)) if mode == "--plist" else list(xml_events(ET.fromstring(content)))
            if mode == "--xml":
                actual = trim_outside(actual)
            if actual != expected:
                results["failures"].append({"asset": info.filename.rsplit("/", 1)[-1], "reason": "independent parser comparison differs"})
            else:
                results["plist_pass" if mode == "--plist" else "xml_pass"] += 1
    for name, mode, data in [
        ("short_binary_plist", "--plist", b"bplist00"),
        ("invalid_binary_plist_trailer", "--plist", b"bplist00" + bytes(32)),
        ("xml_mismatched_close", "--xml", b"<root><item></root>"),
        ("xml_unknown_entity", "--xml", b"<root>&external;</root>"),
    ]:
        process = invoke(mode, data)
        if process.returncode:
            results["malformed_rejected"].append(name)
        else:
            results["failures"].append({"fixture": name, "reason": "malformed input accepted"})
    process = invoke("--strings", b"")
    if process.returncode:
        results["failures"].append({"fixture": "strings_and_ownership", "reason": process.stderr.decode(errors="replace")})
    else:
        results["other_checks"] = json.loads(process.stdout)
    with tempfile.TemporaryDirectory(prefix="foundation-defaults-", dir=args.scratch_parent) as scratch:
        process = subprocess.run([str(args.harness), "--defaults", scratch], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if process.returncode:
            results["failures"].append({"fixture": "defaults_persistence", "reason": process.stderr.decode(errors="replace")})
        else:
            results.setdefault("other_checks", {}).update(json.loads(process.stdout))
    results["status"] = "PASS" if not results["failures"] else "FAIL"
    print(json.dumps(results, ensure_ascii=False, indent=2))
    raise SystemExit(bool(results["failures"]))


if __name__ == "__main__":
    main()
