#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Write BUILD-PROVENANCE.json for nx-repro-build.sh (V3-REPRO-01).

Schema nx-build-provenance-v1.  Records: HEAD commit, describe/tags,
the controlled environment used, tool versions, the generator identity
(VERSION + sha256 of the generator sources), the sha256+mode manifests of
both generated output trees, the comparison verdict, and sanitized commands.

Sanitization contract: never include owner data, IPs, hostnames or $HOME
paths.  Commands are recorded relative to the worktree roots as <wt1>/...;
the finished document is scanned and the tool FAILS if a /home/ literal,
the current $HOME, the hostname, or an IPv4 address slipped in.
"""

import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import sys

SCHEMA = "nx-build-provenance-v1"
SCHEMA_VERSION = 1

GENERATOR_FILES = {
    "nxgenerator_py": "framework/nxgenerator/nxgenerator.py",
    "nxbootstrap_generate_port_py": "framework/nxbootstrap/tools/generate-port.py",
    "nxbootstrap_launcher_sh_in": "framework/nxbootstrap/templates/launcher.sh.in",
}

TOOLCHAIN_PROBES = ["cc", "gcc", "clang", "aarch64-linux-gnu-gcc"]

IPV4_RE = re.compile(r"\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b")


def run(argv, cwd=None):
    try:
        proc = subprocess.run(
            argv, cwd=cwd, capture_output=True, text=True, timeout=30
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout.strip()


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(path):
    """Parse 'mode sha|dir relpath' lines into a sorted path->entry map."""
    entries = {}
    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            raw = raw.rstrip("\n")
            if not raw:
                continue
            mode, digest, rel = raw.split(" ", 2)
            if digest == "dir":
                entries[rel] = {"type": "dir", "mode": mode}
            else:
                entries[rel] = {"type": "file", "mode": mode, "sha256": digest}
    return {key: entries[key] for key in sorted(entries)}


def first_line(text):
    return text.splitlines()[0] if text else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--head-commit", required=True)
    parser.add_argument("--verdict", required=True,
                        choices=["identical", "divergent"])
    parser.add_argument("--source-date-epoch", required=True, type=int)
    parser.add_argument("--umask", required=True)
    parser.add_argument("--lc-all", required=True)
    parser.add_argument("--lang", required=True)
    parser.add_argument("--tz", required=True)
    parser.add_argument("--manifest-a", required=True)
    parser.add_argument("--manifest-b", required=True)
    parser.add_argument("--command", action="append", default=[],
                        dest="commands",
                        help="sanitized command line (<wt1>/... style paths)")
    args = parser.parse_args()

    repo = args.repo_root
    describe = run(["git", "-C", repo, "describe", "--tags", "--always"])
    tags = run(["git", "-C", repo, "tag", "--points-at", args.head_commit])
    tag_list = tags.splitlines() if tags else []

    toolchains = {}
    for tool in TOOLCHAIN_PROBES:
        version = run([tool, "--version"])
        if version:
            toolchains[tool] = first_line(version)

    version_path = os.path.join(repo, "framework/nxgenerator/VERSION")
    generator = {"version": None, "hashes": {}}
    if os.path.isfile(version_path):
        with open(version_path, "r", encoding="utf-8") as handle:
            generator["version"] = handle.read().strip()
    for key, rel in GENERATOR_FILES.items():
        full = os.path.join(repo, rel)
        generator["hashes"][key] = {
            "path": rel,
            "sha256": sha256_of(full) if os.path.isfile(full) else None,
        }

    document = {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "head_commit": args.head_commit,
        "git_describe": describe,
        "git_tags_at_head": tag_list,
        "environment": {
            "source_date_epoch": args.source_date_epoch,
            "lc_all": args.lc_all,
            "lang": args.lang,
            "tz": args.tz,
            "umask": args.umask,
            "pythonhashseed": "0",
        },
        "tools": {
            "python3": "%d.%d.%d" % sys.version_info[:3],
            "toolchains": toolchains,
        },
        "generator": generator,
        "outputs": {
            "tree_a": load_manifest(args.manifest_a),
            "tree_b": load_manifest(args.manifest_b),
        },
        "verdict": args.verdict,
        "commands": args.commands,
    }

    rendered = json.dumps(document, indent=2, sort_keys=True,
                          ensure_ascii=True) + "\n"

    # Sanitization gate: refuse to write owner data.
    forbidden = ["/home/", "/root/"]
    home = os.environ.get("HOME")
    if home and home not in ("/", ""):
        forbidden.append(home)
    hostname = socket.gethostname()
    if hostname:
        forbidden.append(hostname)
    for needle in forbidden:
        if needle and needle in rendered:
            print("nx-build-provenance: FAIL forbidden literal %r would leak "
                  "into the provenance" % needle, file=sys.stderr)
            return 1
    if IPV4_RE.search(rendered):
        print("nx-build-provenance: FAIL an IPv4-looking literal would leak "
              "into the provenance", file=sys.stderr)
        return 1

    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(rendered)
    print("nx-build-provenance: wrote %s (verdict=%s, %d+%d entries)"
          % (os.path.basename(args.out), args.verdict,
             len(document["outputs"]["tree_a"]),
             len(document["outputs"]["tree_b"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
