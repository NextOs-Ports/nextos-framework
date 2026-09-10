#!/usr/bin/env python3
"""Refresh the committed NXRelease v2 allowlist from reviewed port sources."""

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def digest(relative: str) -> str:
    path = ROOT / relative
    if path.is_symlink() or not path.is_file():
        raise SystemExit("missing or unsafe manifest source: " + relative)
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def plain(source: str, kind: str = "payload", mode: str = "0644", target=None):
    return {
        "source": source,
        "target": target or source,
        "kind": kind,
        "mode": mode,
        "sha256": digest(source),
    }


def linux(source: str, target: str, kind: str, provenance: str, needed):
    item = plain(source, kind, "0755", target)
    item.update({
        "architecture": "aarch64",
        "build_profile": "universal-low-glibc",
        "provenance": provenance,
        "needed": sorted(needed),
        "soname": None,
    })
    return item


launcher = plain("Hitman GO.sh", "launcher", "0755")
nxport = plain("hitmango/nxport.json", "nxbootstrap-config")
recipe = plain("hitmango/extractor.json", "nxextract-recipe")
port_json = plain("hitmango/port.json", "portmaster-metadata")
nxsplash = linux(
    "hitmango/nxsplash-nextos",
    "hitmango/nxsplash-nextos",
    "nxsplash-linux",
    "nxsplash-v0.1.2 immutable AArch64 release",
    ["libc.so.6", "libdl.so.2"],
)
binary = linux(
    "build/hitmango-nextos",
    "hitmango/bin/aarch64/hitmango-nextos",
    "project-linux",
    "build_universal.sh; playfetch-builder:buster image sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf; Debian Buster AArch64 cross-toolchain",
    [
        "libSDL2-2.0.so.0",
        "libc.so.6",
        "libdl.so.2",
        "libgcc_s.so.1",
        "libm.so.6",
        "libpthread.so.0",
        "libz.so.1",
    ],
)
nxextract_engine = plain("hitmango/nxextract/nxextract.py", "nxextract")
nxextract_runner = plain(
    "hitmango/nxextract/run-extractor.sh", "nxextract-runner"
)
nxextract_runtime = plain(
    "hitmango/nxextract/nxextract-runtime-env.sh", "nxextract-runtime-env"
)
nxextract_ui = linux(
    "hitmango/nxextract/nxextract-ui",
    "hitmango/nxextract/nxextract-ui",
    "nxextract-ui-linux",
    "NXExtract 1.2.9 canonical graphical AArch64 UI",
    ["libc.so.6", "libdl.so.2"],
)

files = [
    launcher,
    nxport,
    nxsplash,
    binary,
    plain("hitmango/nxproject.json"),
    plain("hitmango/port-env.sh", "script"),
    recipe,
    nxextract_engine,
    nxextract_runner,
    nxextract_runtime,
    nxextract_ui,
    port_json,
    plain("hitmango/README.md"),
    plain("hitmango/INSTALLATION.md"),
    plain("hitmango/CHANGELOG.md"),
    plain("hitmango/LICENSE", "license-notice"),
    plain("hitmango/NOTICE.md", "license-notice"),
    plain("hitmango/version.txt"),
    plain("hitmango/FRAMEWORK-PIN.json"),
    plain("hitmango/GENERATION.json"),
    plain("hitmango/adapter/adapter-contract.json"),
    plain("hitmango/gamedata/README.txt"),
    plain("hitmango/licenses/NXExtract-MIT.txt", "license-notice"),
    plain("hitmango/licenses/NXSplash-MIT.txt", "license-notice"),
]

dependencies = []
for soname, provider in (
    ("libSDL2-2.0.so.0", "portmaster"),
    ("libc.so.6", "glibc-base"),
    ("libdl.so.2", "glibc-base"),
    ("libgcc_s.so.1", "firmware"),
    ("libm.so.6", "glibc-base"),
    ("libpthread.so.0", "glibc-base"),
    ("libz.so.1", "firmware"),
):
    dependencies.append({
        "namespace": "linux",
        "architecture": "aarch64",
        "soname": soname,
        "provider": provider,
    })

document = {
    "schema_version": 2,
    "source_root": ".",
    "package": {
        "id": "hitmango",
        "version": "1.2.2",
        "profile": "universal-portmaster",
        "launcher": "Hitman GO.sh",
        "launcher_chain": ["Hitman GO.sh"],
        "launcher_contract": {
            "generator": "nxbootstrap",
            "version": "0.6.14",
            "config_path": "hitmango/nxport.json",
            "config_sha256": nxport["sha256"],
        },
        "port_dir": "hitmango",
        "license": {
            "spdx_id": "GPL-3.0-only",
            "source_url": "https://github.com/NextOs-Ports/hitmango-nextos",
            "file": "hitmango/LICENSE",
        },
    },
    "release": {
        "source_date_epoch": 1786752000,
        "max_glibc": "2.27",
        "compression": "deflated",
    },
    "nxextract": {
        "path": "hitmango/nxextract/nxextract.py",
        "version": "1.2.9",
        "minimum_version": "1.2.9",
        "sha256": nxextract_engine["sha256"],
        "runner_path": "hitmango/nxextract/run-extractor.sh",
        "runner_sha256": nxextract_runner["sha256"],
        "runtime_env_path": "hitmango/nxextract/nxextract-runtime-env.sh",
        "runtime_env_sha256": nxextract_runtime["sha256"],
        "ui_path": "hitmango/nxextract/nxextract-ui",
        "ui_sha256": nxextract_ui["sha256"],
        "recipe_path": "hitmango/extractor.json",
        "recipe_sha256": recipe["sha256"],
    },
    "dependencies": dependencies,
    "portmaster_metadata": {
        "port_json": {
            "path": "hitmango/port.json",
            "sha256": port_json["sha256"],
        },
        "images": [],
    },
    "files": files,
    "exceptions": [],
}

(ROOT / "nxrelease.json").write_text(
    json.dumps(document, indent=2, ensure_ascii=False) + "\n",
    encoding="utf-8",
)
print("refreshed nxrelease.json with", len(files), "files")
