#!/usr/bin/env python3
"""Focused tests for Terraria's structure-based compatibility hook."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import struct
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "prepare_terraria_data", ROOT / "tools" / "prepare_terraria_data.py"
)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def write_aarch64_elf(path: Path, payload: bytes = b"") -> None:
    header = bytearray(64)
    header[:4] = b"\x7fELF"
    header[4] = 2
    struct.pack_into("<H", header, 18, 183)
    path.write_bytes(bytes(header) + payload)


def make_stage(root: Path, data_payload: bytes, unity_version: bytes) -> Path:
    stage = root / "stage"
    data = stage / "bin/Data"
    metadata = data / "Managed/Metadata"
    metadata.mkdir(parents=True)
    (data / "boot.config").write_bytes(b"gfx-enable-gfx-jobs=1\n")
    (data / "data.unity3d").write_bytes(b"UnityFS\x00" + data_payload)
    (metadata / "global-metadata.dat").write_bytes(
        MODULE.GLOBAL_METADATA_MAGIC + b"metadata"
    )
    (data / "resources.resource").write_bytes(b"resources")
    (data / "unity default resources").write_bytes(b"defaults")
    write_aarch64_elf(stage / "libunity.so", unity_version + b"\x00")
    write_aarch64_elf(stage / "libil2cpp.so")
    write_aarch64_elf(stage / "libc++_shared.so")
    return stage


def compatible_payload_passes(data_payload: bytes, label: str) -> None:
    with tempfile.TemporaryDirectory(prefix="terraria-compat-test.") as directory:
        root = Path(directory)
        stage = make_stage(root, data_payload, b"2021.3.56f2")
        build = MODULE.prepare(stage)
        manifest = json.loads((stage / ".terraria-data.json").read_text())
        assert build.startswith("unknown-"), label
        assert manifest["source"]["known_build"] is False, label
        assert manifest["source"]["compatibility_contract"] == (
            MODULE.COMPATIBILITY_CONTRACT
        ), label


# The documented build uses an internal suffix ending in .49. It must pass,
# and the same compatible structure must also pass without any game-version
# string at all. Version text is provenance, never an acceptance gate.
compatible_payload_passes(b"internal 1.4.5.6.49 payload", "reference suffix")
compatible_payload_passes(b"no dotted game version here", "no version token")

with tempfile.TemporaryDirectory(prefix="terraria-compat-negative.") as directory:
    stage = make_stage(Path(directory), b"payload", b"2022.3.1f1")
    try:
        MODULE.prepare(stage)
    except RuntimeError as error:
        assert "unsupported Unity engine" in str(error)
    else:
        raise AssertionError("incompatible Unity engine was accepted")

assert not hasattr(MODULE, "contains_ascii_token")
print("Terraria structural-compatibility tests: PASS")
