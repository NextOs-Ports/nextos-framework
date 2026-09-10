#!/usr/bin/env python3
"""Build the NXRelease v2 allowlist from reviewed RCRDX sources."""

import hashlib
import os
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def digest(relative: str) -> str:
    path = ROOT / relative
    if path.is_symlink() or not path.is_file():
        raise SystemExit("missing or unsafe manifest source: " + relative)
    return hashlib.sha256(path.read_bytes()).hexdigest()


def plain(source: str, kind="payload", mode="0644", target=None):
    return {
        "source": source,
        "target": target or "rcrdx/" + source,
        "kind": kind,
        "mode": mode,
        "sha256": digest(source),
    }


def linux(source: str, kind: str, provenance: str, needed):
    item = plain(source, kind, "0755")
    item.update({
        "architecture": "aarch64",
        "build_profile": "universal-low-glibc",
        "provenance": provenance,
        "needed": sorted(needed),
        "soname": None,
    })
    return item


launcher = plain(
    "Retro City Rampage DX.sh", "launcher", "0755", "Retro City Rampage DX.sh"
)
nxport = plain("nxport.json", "nxbootstrap-config")
recipe = plain("extractor.json", "nxextract-recipe")
port_json = plain("port.json", "portmaster-metadata")
gameinfo_xml = plain("gameinfo.xml", "portmaster-metadata")
nxsplash = linux(
    "nxsplash-nextos",
    "nxsplash-linux",
    "NXSplash 0.1.2 canonical AArch64 release",
    ["libc.so.6", "libdl.so.2"],
)
binary = linux(
    "rcrdx-nextos",
    "project-linux",
    "build_universal.sh; pinned Debian Buster AArch64 builder sha256:58ce6f1271ae1c8a2006ff7d3e54e9874d839f573d8009c20154ad0f2fb0a225; API headers and static zlib from read-only NextOS sysroot",
    [
        "libSDL2-2.0.so.0",
        "libEGL.so.1",
        "libGLESv2.so.2",
        "libc.so.6",
        "libdl.so.2",
        "libm.so.6",
        "libpthread.so.0",
    ],
)
engine = plain("nxextract/nxextract.py", "nxextract")
runner = plain("nxextract/run-extractor.sh", "nxextract-runner")
runtime = plain("nxextract/nxextract-runtime-env.sh", "nxextract-runtime-env")
ui = linux(
    "nxextract/nxextract-ui",
    "nxextract-ui-linux",
    "NXExtract 1.2.10 canonical graphical AArch64 UI",
    ["libc.so.6", "libdl.so.2"],
)

files = [
    launcher,
    nxport,
    nxsplash,
    binary,
    plain("nxproject.json"),
    recipe,
    engine,
    runner,
    runtime,
    ui,
    port_json,
    gameinfo_xml,
    plain("README.md"),
    plain("INSTALLATION.md"),
    plain("LICENSE", "license-notice"),
    plain("NOTICE.md", "license-notice"),
    plain("version.txt"),
    plain("GENERATION.json"),
    plain("adapter/adapter-contract.json"),
    plain("gamedata/COLOQUE-O-APK-AQUI.txt"),
    plain("nxextract-version.txt"),
    plain("licenses/NXExtract-MIT.txt", "license-notice"),
    plain("licenses/nxsplash-MIT.txt", "license-notice"),
]

dependencies = [
    {
        "namespace": "linux",
        "architecture": "aarch64",
        "soname": soname,
        "provider": provider,
    }
    for soname, provider in (
        ("libEGL.so.1", "firmware"),
        ("libGLESv2.so.2", "firmware"),
        ("libSDL2-2.0.so.0", "portmaster"),
        ("libc.so.6", "glibc-base"),
        ("libdl.so.2", "glibc-base"),
        ("libm.so.6", "glibc-base"),
        ("libpthread.so.0", "glibc-base"),
    )
]

document = {
    "schema_version": 2,
    "source_root": ".",
    "package": {
        "id": "rcrdx",
        "version": "1.0.0",
        "profile": "universal-portmaster",
        "launcher": "Retro City Rampage DX.sh",
        "launcher_chain": ["Retro City Rampage DX.sh"],
        "launcher_contract": {
            "generator": "nxbootstrap",
            "version": (
                # Read from the framework, never a literal: this is the pin
                # that let the port fall behind while the others advanced.
                (Path(os.environ.get("NEXTOS_FRAMEWORK_ROOT")
                              or os.environ.get("NX_FRAMEWORK_ROOT")
                              or ROOT.parent.parent / "framework") / "nxbootstrap" / "VERSION"
                 ).read_text(encoding="utf-8").strip()
            ),
            "config_path": "rcrdx/nxport.json",
            "config_sha256": nxport["sha256"],
        },
        "port_dir": "rcrdx",
        "license": {
            "spdx_id": "GPL-3.0-only",
            "source_url": "https://github.com/NextOs-Ports/nextos_ports_android",
            "file": "rcrdx/LICENSE",
        },
    },
    "release": {
        "source_date_epoch": 1786752000,
        "max_glibc": "2.27",
        "compression": "deflated",
    },
    "nxextract": {
        "path": "rcrdx/nxextract/nxextract.py",
        "version": "1.2.10",
        "minimum_version": "1.2.10",
        "sha256": engine["sha256"],
        "runner_path": "rcrdx/nxextract/run-extractor.sh",
        "runner_sha256": runner["sha256"],
        "runtime_env_path": "rcrdx/nxextract/nxextract-runtime-env.sh",
        "runtime_env_sha256": runtime["sha256"],
        "ui_path": "rcrdx/nxextract/nxextract-ui",
        "ui_sha256": ui["sha256"],
        "recipe_path": "rcrdx/extractor.json",
        "recipe_sha256": recipe["sha256"],
    },
    "dependencies": dependencies,
    "portmaster_metadata": {
        "port_json": {"path": "rcrdx/port.json", "sha256": port_json["sha256"]},
        "gameinfo_xml": {"path": "rcrdx/gameinfo.xml", "sha256": gameinfo_xml["sha256"]},
        "images": [],
    },
    "files": files,
    "exceptions": [],
}

(ROOT / "nxrelease.json").write_text(
    json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
)
print("refreshed nxrelease.json with", len(files), "files")
