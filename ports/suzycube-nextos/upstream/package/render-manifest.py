#!/usr/bin/env python3
"""Render Suzy Cube's NXRelease manifest from checked-in declarations/files."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "nxrelease.json"


def framework_component_version(component: str) -> str:
    """Read a component version from the framework tree, never from a literal.

    The manifest already derives every hash from the file it pins; the
    launcher-contract version was the one value still typed by hand, and it
    went stale the first time nxbootstrap was bumped.
    """
    for env in ("SC_FRAMEWORK_REPOSITORY",):
        base = os.environ.get(env)
        if base:
            candidate = Path(base) / "framework" / component / "VERSION"
            if candidate.is_file():
                return candidate.read_text(encoding="utf-8").strip()
    for env in ("NX_FRAMEWORK_ROOT", "NEXTOS_FRAMEWORK_ROOT"):
        base = os.environ.get(env)
        if base:
            candidate = Path(base) / component / "VERSION"
            if candidate.is_file():
                return candidate.read_text(encoding="utf-8").strip()
    candidate = ROOT.parent / "nextos_ports_android" / "framework" / component / "VERSION"
    if candidate.is_file():
        return candidate.read_text(encoding="utf-8").strip()
    fail(f"cannot resolve the {component} VERSION from the framework tree")
    raise SystemExit(1)
SOURCE_DATE_EPOCH = 1786752000
BUILDER_IMAGE = (
    "sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf"
)
FRAMEWORK_PIN_SHA256 = (
    "5c786a8ad3c35fb716dd1e6e0b264b94ca32f1568759592d2251abcc8a4a51b9"
)


def fail(message: str) -> None:
    raise SystemExit("render-manifest: " + message)


def digest(relative: str) -> str:
    path = ROOT / relative
    if not path.is_file() or path.is_symlink():
        fail("missing or unsafe source file: " + relative)
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_json(relative: str):
    def no_duplicates(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                fail("duplicate JSON key in %s: %s" % (relative, key))
            value[key] = item
        return value

    return json.loads((ROOT / relative).read_text(encoding="utf-8"),
                      object_pairs_hook=no_duplicates,
                      parse_constant=lambda token: fail(
                          "non-finite JSON value in %s: %s" % (relative, token)))


def file_entry(source: str, target: str, kind: str, mode: str = "0644",
               **extra):
    entry = {
        "source": source,
        "target": target,
        "kind": kind,
        "mode": mode,
        "sha256": digest(source),
    }
    entry.update(extra)
    return entry


def elf_needed(relative: str):
    result = subprocess.run(
        ("readelf", "-dW", str(ROOT / relative)), text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode:
        fail("readelf failed for %s: %s" % (relative, result.stderr.strip()))
    values = []
    for line in result.stdout.splitlines():
        if "(NEEDED)" in line and "[" in line and "]" in line:
            values.append(line.split("[", 1)[1].split("]", 1)[0])
    return sorted(values)


def render():
    project = load_json("nxproject.json")
    nxport = load_json("nxport.json")
    metadata = load_json("port.json")
    expected_nxport = dict(project.get("nxport", {}))
    declared_required = list(expected_nxport.get("required_files", []))
    if "nxsplash-nextos" not in declared_required:
        declared_required.insert(1, "nxsplash-nextos")
    expected_nxport["required_files"] = declared_required
    if expected_nxport != nxport:
        fail("nxport.json differs from nxproject.json plus generated NXSplash")
    if metadata.get("version") != 4 or metadata.get("attr", {}).get("runtime") != []:
        fail("PortMaster metadata is not canonical schema v4")
    if nxport.get("nxextract", {}).get("version") != "1.2.10":
        fail("NXExtract engine pin drift")
    version = (ROOT / "version.txt").read_text(encoding="utf-8").strip()
    if version != "1.1.12":
        fail("unexpected package version: " + version)

    executable_needed = elf_needed("build/suzycube-nextos")
    expected_needed = sorted([
        "ld-linux-aarch64.so.1", "libSDL2-2.0.so.0", "libc.so.6",
        "libdl.so.2", "libgcc_s.so.1", "libm.so.6", "libpthread.so.0",
        "libz.so.1",
    ])
    if executable_needed != expected_needed:
        fail("unexpected suzycube-nextos DT_NEEDED: %r" % executable_needed)

    files = [
        file_entry("Suzy Cube.sh", "Suzy Cube.sh", "launcher", "0755"),
        file_entry("nxport.json", "suzycube/nxport.json",
                   "nxbootstrap-config"),
        file_entry("nxproject.json", "suzycube/nxproject.json", "payload"),
        file_entry("FRAMEWORK-BUILD-PIN.json",
                   "suzycube/FRAMEWORK-BUILD-PIN.json", "payload"),
        file_entry(
            "build/suzycube-nextos", "suzycube/suzycube-nextos",
            "project-linux", "0755", architecture="aarch64",
            build_profile="universal-low-glibc",
            provenance=(
                "build_universal.sh; offline Debian Buster image %s; "
                "framework pin sha256:%s; target sysroot used for headers only"
                % (BUILDER_IMAGE, FRAMEWORK_PIN_SHA256)),
            needed=executable_needed, soname=None),
        file_entry("README.md", "suzycube/README.md", "payload"),
        file_entry("INSTALLATION.md", "suzycube/INSTALLATION.md", "payload"),
        file_entry("NOTICE.md", "suzycube/NOTICE.md", "license-notice"),
        file_entry("LICENSE", "suzycube/LICENSE", "license-notice"),
        file_entry("version.txt", "suzycube/version.txt", "payload"),
        file_entry("gamedata-template/README.txt",
                   "suzycube/gamedata/README.txt", "payload"),
        file_entry("extractor.json", "suzycube/extractor.json",
                   "nxextract-recipe"),
        file_entry("nxextract-version.txt", "suzycube/nxextract-version.txt",
                   "payload"),
        file_entry("nxextract/nxextract.py",
                   "suzycube/nxextract/nxextract.py", "nxextract"),
        file_entry("nxextract/merge-suzycube-data.py",
                   "suzycube/nxextract/merge-suzycube-data.py", "payload",
                   "0755"),
        file_entry(
            "nxextract/nxextract-ui", "suzycube/nxextract/nxextract-ui",
            "nxextract-ui-linux", "0755", architecture="aarch64",
            build_profile="universal-low-glibc",
            provenance="NXExtract UI 1.2.9 canonical AArch64 artifact",
            needed=["libc.so.6", "libdl.so.2"], soname=None),
        file_entry("nxextract/run-extractor.sh",
                   "suzycube/nxextract/run-extractor.sh",
                   "nxextract-runner"),
        file_entry("nxextract/nxextract-runtime-env.sh",
                   "suzycube/nxextract/nxextract-runtime-env.sh",
                   "nxextract-runtime-env"),
        file_entry("licenses/NXExtract-MIT.txt",
                   "suzycube/licenses/NXExtract-MIT.txt", "license-notice"),
        file_entry(
            "nxsplash-nextos", "suzycube/nxsplash-nextos",
            "nxsplash-linux", "0755", architecture="aarch64",
            build_profile="universal-low-glibc",
            provenance="NXSplash 0.1.2 canonical AArch64 artifact",
            needed=["libc.so.6", "libdl.so.2"], soname=None),
        file_entry("port.json", "suzycube/port.json",
                   "portmaster-metadata"),
        file_entry("gameinfo.xml", "suzycube/gameinfo.xml",
                   "portmaster-metadata"),
    ]

    manifest = {
        "schema_version": 2,
        "source_root": ".",
        "package": {
            "id": "suzycube",
            "version": version,
            "profile": "universal-portmaster",
            "launcher": "Suzy Cube.sh",
            "launcher_chain": ["Suzy Cube.sh"],
            "launcher_contract": {
                "generator": "nxbootstrap",
                "version": framework_component_version("nxbootstrap"),
                "config_path": "suzycube/nxport.json",
                "config_sha256": digest("nxport.json"),
            },
            "port_dir": "suzycube",
            "license": {
                "spdx_id": "GPL-3.0-only",
                "source_url":
                    "https://github.com/NextOs-Ports/suzycube-nextos",
                "file": "suzycube/LICENSE",
            },
        },
        "release": {
            "source_date_epoch": SOURCE_DATE_EPOCH,
            "max_glibc": "2.30",
            "compression": "deflated",
        },
        "nxextract": {
            "path": "suzycube/nxextract/nxextract.py",
            "version": "1.2.10",
            "minimum_version": "1.2.10",
            "sha256": digest("nxextract/nxextract.py"),
            "runner_path": "suzycube/nxextract/run-extractor.sh",
            "runner_sha256": digest("nxextract/run-extractor.sh"),
            "runtime_env_path":
                "suzycube/nxextract/nxextract-runtime-env.sh",
            "runtime_env_sha256":
                digest("nxextract/nxextract-runtime-env.sh"),
            "ui_path": "suzycube/nxextract/nxextract-ui",
            "ui_sha256": digest("nxextract/nxextract-ui"),
            "recipe_path": "suzycube/extractor.json",
            "recipe_sha256": digest("extractor.json"),
        },
        "dependencies": [
            {"namespace": "linux", "architecture": "aarch64",
             "soname": soname, "provider": provider}
            for soname, provider in (
                ("ld-linux-aarch64.so.1", "firmware"),
                ("libSDL2-2.0.so.0", "portmaster"),
                ("libc.so.6", "glibc-base"),
                ("libdl.so.2", "glibc-base"),
                ("libgcc_s.so.1", "firmware"),
                ("libm.so.6", "glibc-base"),
                ("libpthread.so.0", "glibc-base"),
                ("libz.so.1", "firmware"),
            )
        ],
        "portmaster_metadata": {
            "port_json": {
                "path": "suzycube/port.json",
                "sha256": digest("port.json"),
            },
            "gameinfo_xml": {
                "path": "suzycube/gameinfo.xml",
                "sha256": digest("gameinfo.xml"),
            },
            "images": [],
        },
        "files": files,
        "exceptions": [],
    }
    return (json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2) +
            "\n").encode("utf-8")


def write_atomic(content: bytes) -> None:
    descriptor, temporary = tempfile.mkstemp(
        prefix=".nxrelease.", suffix=".tmp", dir=str(ROOT))
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, OUTPUT)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="fail when nxrelease.json is not current")
    args = parser.parse_args()
    content = render()
    if args.check:
        if not OUTPUT.is_file() or OUTPUT.read_bytes() != content:
            fail("nxrelease.json is stale; run package/render-manifest.py")
        print("SUZY CUBE NXRELEASE MANIFEST: CURRENT")
        return 0
    write_atomic(content)
    print("SUZY CUBE NXRELEASE MANIFEST: WROTE " + str(OUTPUT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
