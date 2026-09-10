#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nx-controller-profiles -- build an NXCONTROLLER_PROFILES/1 bundle.

Deterministic: the same official source database produces byte-identical
output. Lines are taken BYTE-INTACT from the source, filtered to the declared
platform domain, deduplicated by GUID; a divergent duplicate of the same
GUID/domain is EXCLUDED and recorded in the manifest (order must never
decide). The bundle carries zero ROMs, executables, addresses, hostnames or
personal data -- the builder fails closed if the source smells of any.

Usage:
  nx-controller-profiles.py --source gamecontrollerdb.txt \
      --supplier portmaster-gui --supplier-commit <sha> \
      --license "MIT (upstream gamecontrollerdb)" \
      --platform Linux --out controllers.nxb
"""

import argparse
import hashlib
import json
import pathlib
import re
import sys

GUID_LINE = re.compile(r"^[0-9a-f]{32},")
FORBIDDEN = re.compile(
    r"(/home/|/roms/|/storage/|\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b|"
    r"hostname|password|token)", re.IGNORECASE)


def fail(message):
    print("nx-controller-profiles: %s" % message, file=sys.stderr)
    sys.exit(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--supplier", required=True)
    parser.add_argument("--supplier-commit", required=True)
    parser.add_argument("--license", required=True)
    parser.add_argument("--platform", default="Linux")
    parser.add_argument("--claim", default="entries limited to the pinned "
                        "official source; never a universal claim")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    source_path = pathlib.Path(args.source)
    raw = source_path.read_bytes()
    source_sha = hashlib.sha256(raw).hexdigest()
    text = raw.decode("utf-8", "replace")

    platform_token = "platform:%s" % args.platform
    entries = {}
    conflicts = []
    foreign = 0
    for line in text.splitlines():
        line = line.rstrip("\r ")
        if not line or line.startswith("#"):
            continue
        if not GUID_LINE.match(line):
            foreign += 1
            continue
        if platform_token not in line:
            continue
        if FORBIDDEN.search(line):
            fail("the source line for %s carries forbidden content"
                 % line[:32])
        guid = line[:32]
        if guid in entries:
            if entries[guid] != line:
                conflicts.append(guid)
            continue
        entries[guid] = line

    for guid in conflicts:
        entries.pop(guid, None)

    body_lines = [entries[guid] for guid in sorted(entries)]
    header = [
        "NXCONTROLLER_PROFILES/1",
        "# supplier=%s" % args.supplier,
        "# supplier_commit=%s" % args.supplier_commit,
        "# source_sha256=%s" % source_sha,
        "# dialect=sdl2-gamecontrollerdb",
        "# platform=%s" % args.platform,
        "# license=%s" % getattr(args, "license"),
        "# coverage=%d guids" % len(body_lines),
        "# claim=%s" % args.claim,
    ]
    payload = "\n".join(header + body_lines) + "\n"
    if FORBIDDEN.search("\n".join(header)):
        fail("a header value carries forbidden content")

    out = pathlib.Path(args.out)
    out.write_text(payload, encoding="utf-8")
    digest = hashlib.sha256(payload.encode("utf-8")).hexdigest()
    (out.parent / (out.name + ".sha256")).write_text(
        "%s  %s\n" % (digest, out.name), encoding="utf-8")
    manifest = {
        "schema": "nxcontroller-profiles-manifest",
        "schema_version": 1,
        "contract": "NXCONTROLLER_PROFILES/1",
        "supplier": args.supplier,
        "supplier_commit": args.supplier_commit,
        "source_sha256": source_sha,
        "bundle_sha256": digest,
        "platform": args.platform,
        "license": getattr(args, "license"),
        "entries": len(body_lines),
        "conflicts_excluded": sorted(set(conflicts)),
        "foreign_entries_skipped": foreign,
        "claim": args.claim,
    }
    (out.parent / (out.name + ".manifest.json")).write_text(
        json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=1) +
        "\n", encoding="utf-8")
    print("bundle=%s entries=%d conflicts_excluded=%d sha256=%s"
          % (out.name, len(body_lines), len(set(conflicts)), digest))


if __name__ == "__main__":
    main()
