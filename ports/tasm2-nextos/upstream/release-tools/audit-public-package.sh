#!/usr/bin/env bash
# Audit the public BYO-data ZIP and its source tree without extracting game data.
set -euo pipefail

PACKAGE_SOURCE=
ZIP_FILE=
REPORT=

fail() {
  printf 'public audit error: %s\n' "$*" >&2
  exit 2
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --package-source)
      [ "$#" -ge 2 ] || fail "--package-source requires a value"
      PACKAGE_SOURCE=$2
      shift 2
      ;;
    --zip)
      [ "$#" -ge 2 ] || fail "--zip requires a value"
      ZIP_FILE=$2
      shift 2
      ;;
    --report)
      [ "$#" -ge 2 ] || fail "--report requires a value"
      REPORT=$2
      shift 2
      ;;
    --help|-h)
      printf '%s\n' \
        'usage: audit-public-package.sh --package-source DIR --zip FILE --report FILE'
      exit 0
      ;;
    *) fail "unknown option: $1" ;;
  esac
done

[ -d "$PACKAGE_SOURCE" ] || fail "package source directory is missing"
[ -f "$PACKAGE_SOURCE/build-package.sh" ] ||
  fail "build-package.sh is missing"
[ -f "$ZIP_FILE" ] || fail "public ZIP is missing"
[ -n "$REPORT" ] || fail "--report is required"

mkdir -p -- "$(dirname -- "$REPORT")"
python3 - "$PACKAGE_SOURCE" "$ZIP_FILE" "$REPORT" <<'PY'
from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

source = Path(sys.argv[1]).resolve()
zip_path = Path(sys.argv[2]).resolve()
report_path = Path(sys.argv[3]).resolve()
blockers: list[str] = []
notes: list[str] = []
elf_abi_rows: list[tuple[str, str]] = []

EXPECTED_SCREENSHOT_DIMENSIONS = (640, 480)
EXPECTED_SCREENSHOT_SHA256 = (
    "5935351d03b43bd472dece3e610c0c5ce93decc1eba6bb2a2579e8ac1fe7888e"
)
EXPECTED_IMAGE_METADATA = {
    "screenshot": "screenshot.png",
    "covers": [],
    "thumbnail": "screenshot.png",
    "video": None,
}
PROHIBITED_NAMES = {
    "libtasm2.so",
    "libtasm2-x86.so",
    "IapLocalData_Google.json",
    "IapStoreItems_Offline.json",
}
PROHIBITED_SUFFIXES = {
    ".apk", ".apks", ".apkm", ".xapk", ".obb", ".dex", ".log",
}


def digest(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def version_key(value: str) -> tuple[int, ...]:
    return tuple(int(part) for part in value.split("."))


def audit_elf_abi(name: str, payload: bytes) -> None:
    if not payload.startswith(b"\x7fELF"):
        return
    if shutil.which("readelf") is None:
        blockers.append("readelf is unavailable for the global ELF ABI audit")
        return
    with tempfile.NamedTemporaryFile() as stream:
        stream.write(payload)
        stream.flush()
        result = subprocess.run(
            ["readelf", "--version-info", stream.name],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
            text=True,
        )
    versions = sorted(
        set(re.findall(r"GLIBC_([0-9]+(?:[.][0-9]+)*)", result.stdout)),
        key=version_key,
    )
    newest = versions[-1] if versions else "none"
    elf_abi_rows.append((name, newest))
    if versions and version_key(newest) > version_key("2.30"):
        blockers.append(
            f"bundled ELF exceeds GLIBC_2.30: {name} requires GLIBC_{newest}"
        )


def png_dimensions(payload: bytes) -> tuple[int, int]:
    if len(payload) < 24 or payload[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    return struct.unpack(">II", payload[16:24])


def private_markers(payload: bytes) -> list[str]:
    needles = {
        "private IPv4 prefix": b"192" + b".168" + b".",
        "personal home": b"/home/" + b"felipe",
        "internal archive": b"/mnt/" + b"ARQUIVOS",
        "agent temporary path": b"/tmp/" + b"claude",
        "test root login": b"root" + b"@",
        "test ark login": b"ark" + b"@",
    }
    return [label for label, needle in needles.items() if needle in payload]


def release_placeholders(payload: bytes) -> list[str]:
    patterns = {
        "runtime hash placeholder": re.compile(
            rb"__ASM2_[A-Z0-9_]+__"
        ),
        "provenance replacement placeholder": re.compile(
            rb"REPLACE_WITH_[A-Z0-9_]+"
        ),
        "documentation final-value placeholder": re.compile(
            rb"`FINAL_[A-Z0-9_]+`"
        ),
    }
    return [
        label for label, pattern in patterns.items() if pattern.search(payload)
    ]


def validate_image_metadata(
    label: str, port_json_payload: bytes, gameinfo_payload: bytes
) -> None:
    try:
        metadata = json.loads(port_json_payload.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        blockers.append(f"{label} port.json is invalid: {error}")
    else:
        actual = metadata.get("attr", {}).get("image")
        if actual != EXPECTED_IMAGE_METADATA:
            blockers.append(
                f"{label} port.json must use screenshot.png for screenshot "
                "and thumbnail, with an empty covers list"
            )

    try:
        game = ET.fromstring(gameinfo_payload).find("game")
    except (UnicodeError, ET.ParseError) as error:
        blockers.append(f"{label} gameinfo.xml is invalid: {error}")
    else:
        if (
            game is None
            or game.findtext("image") != "./asm2_127/screenshot.png"
        ):
            blockers.append(
                f"{label} gameinfo.xml must use "
                "./asm2_127/screenshot.png"
            )


art_rows = []
provenance_path = source / "sources" / "ART-PROVENANCE.json"
art_provenance = None
if provenance_path.is_file():
    try:
        art_provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        blockers.append(f"ART-PROVENANCE.json is invalid: {error}")
else:
    blockers.append(
        "public screenshot provenance is absent; capture origin and release "
        "basis cannot be audited"
    )

cover_path = source / "sources" / "cover.png"
if cover_path.exists():
    blockers.append(
        "cover.png must be omitted; the selected release art is the verified "
        "R36S gameplay screenshot"
    )
    if cover_path.is_file():
        cover_payload = cover_path.read_bytes()
        try:
            cover_dimensions = png_dimensions(cover_payload)
            cover_dimensions_text = (
                f"{cover_dimensions[0]}x{cover_dimensions[1]}"
            )
        except ValueError:
            cover_dimensions_text = "invalid"
        art_rows.append(
            (
                "cover.png",
                cover_dimensions_text,
                digest(cover_payload),
                "forbidden",
            )
        )

if isinstance(art_provenance, dict):
    if art_provenance.get("schema") != 1:
        blockers.append("ART-PROVENANCE.json schema must be 1")
    if "cover.png" in art_provenance:
        blockers.append(
            "ART-PROVENANCE.json must not retain a cover.png record"
        )

screenshot_path = source / "sources" / "screenshot.png"
source_screenshot_sha = None
if not screenshot_path.is_file():
    blockers.append("screenshot.png is absent from package sources")
else:
    screenshot_payload = screenshot_path.read_bytes()
    source_screenshot_sha = digest(screenshot_payload)
    try:
        screenshot_dimensions = png_dimensions(screenshot_payload)
    except ValueError as error:
        blockers.append(f"screenshot.png: {error}")
        screenshot_dimensions_text = "invalid"
    else:
        screenshot_dimensions_text = (
            f"{screenshot_dimensions[0]}x{screenshot_dimensions[1]}"
        )
        if screenshot_dimensions != EXPECTED_SCREENSHOT_DIMENSIONS:
            blockers.append(
                "screenshot.png dimensions are "
                f"{screenshot_dimensions_text}, expected 640x480"
            )
    if source_screenshot_sha != EXPECTED_SCREENSHOT_SHA256:
        blockers.append(
            "screenshot.png is not the approved final R36S gameplay capture"
        )

    provenance_state = "missing"
    if isinstance(art_provenance, dict):
        record = art_provenance.get("screenshot.png")
        if not isinstance(record, dict):
            blockers.append("screenshot.png has no provenance record")
        else:
            required = {
                "sha256",
                "origin",
                "capture_context",
                "capture_date",
                "rights_basis",
                "release_allowed",
            }
            missing = sorted(required - record.keys())
            if missing:
                blockers.append(
                    "screenshot.png provenance lacks: "
                    f"{', '.join(missing)}"
                )
            elif record.get("sha256") != source_screenshot_sha:
                blockers.append("screenshot.png provenance hash is stale")
            elif record.get("release_allowed") is not True:
                blockers.append(
                    "screenshot.png provenance does not authorize release"
                )
            else:
                provenance_state = "recorded"
    art_rows.append(
        (
            "screenshot.png",
            screenshot_dimensions_text,
            source_screenshot_sha,
            provenance_state,
        )
    )

source_port_json = source / "sources" / "port.json"
source_gameinfo = source / "sources" / "gameinfo.xml"
if not source_port_json.is_file():
    blockers.append("port.json is absent from package sources")
if not source_gameinfo.is_file():
    blockers.append("gameinfo.xml is absent from package sources")
if source_port_json.is_file() and source_gameinfo.is_file():
    validate_image_metadata(
        "package source",
        source_port_json.read_bytes(),
        source_gameinfo.read_bytes(),
    )

text_suffixes = {".sh", ".py", ".md", ".txt", ".json", ".xml"}
for path in sorted(source.rglob("*")):
    if not path.is_file():
        continue
    relative = path.relative_to(source).as_posix()
    if (
        path.name in PROHIBITED_NAMES
        or path.suffix.lower() in PROHIBITED_SUFFIXES
    ):
        blockers.append(
            f"proprietary/generated package-source artifact: {relative}"
        )
    if "__pycache__" in path.parts or path.suffix in {".pyc", ".pyo", ".log"}:
        blockers.append(f"generated/log source artifact: {relative}")
    if path.suffix.lower() in text_suffixes:
        payload = path.read_bytes()
        for marker in private_markers(payload):
            # build-package.sh owns the scanner signatures and is not staged.
            if relative == "build-package.sh":
                continue
            blockers.append(f"private marker in source {relative}: {marker}")
        for marker in release_placeholders(payload):
            blockers.append(
                f"unresolved release value in source {relative}: {marker}"
            )

builder = (source / "build-package.sh").read_text(
    encoding="utf-8", errors="replace"
)
if "PACKAGE-MANIFEST.sha256" not in builder or "sha256sum -c" not in builder:
    blockers.append("package builder does not generate and verify its manifest")
else:
    notes.append("package builder contains automatic manifest generation and verification")
if "ART-PROVENANCE.json" not in builder:
    blockers.append("package builder does not stage ART-PROVENANCE.json")

allowlist_path = source / "package-files.txt"
allowlist_lines: list[str] = []
allowlist: set[str] = set()
if not allowlist_path.is_file():
    blockers.append("package-files.txt is absent")
else:
    allowlist_lines = [
        line
        for line in allowlist_path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines()
        if line
    ]
    allowlist = set(allowlist_lines)
    if allowlist_lines != sorted(allowlist):
        blockers.append("package-files.txt must be sorted and unique")
    if "asm2_127/cover.png" in allowlist:
        blockers.append("package allowlist still contains cover.png")
    if "asm2_127/ART-PROVENANCE.json" not in allowlist:
        blockers.append("package allowlist omits ART-PROVENANCE.json")
    if "asm2_127/screenshot.png" not in allowlist:
        blockers.append("package allowlist omits screenshot.png")

with zipfile.ZipFile(zip_path, "r") as archive:
    names = archive.namelist()
    if len(names) != len(set(names)):
        blockers.append("ZIP contains duplicate member names")
    regular_names = []
    for name in names:
        member = PurePosixPath(name)
        if member.is_absolute() or ".." in member.parts:
            blockers.append(f"unsafe ZIP member: {name}")
            continue
        if name.endswith("/"):
            continue
        regular_names.append(name)
        payload = archive.read(name)
        audit_elf_abi(name, payload)
        basename = member.name
        if (
            basename in PROHIBITED_NAMES
            or member.suffix.lower() in PROHIBITED_SUFFIXES
        ):
            blockers.append(f"proprietary/generated member entered ZIP: {name}")
        if "__pycache__" in member.parts or basename in {"debug.log", "nxextract.log"}:
            blockers.append(f"cache/log member entered ZIP: {name}")
        if member.suffix.lower() in text_suffixes:
            for marker in private_markers(payload):
                blockers.append(f"private marker in ZIP {name}: {marker}")
            for marker in release_placeholders(payload):
                blockers.append(
                    f"unresolved release value in ZIP {name}: {marker}"
                )

    if allowlist_lines and regular_names != allowlist_lines:
        for missing in sorted(allowlist - set(regular_names)):
            blockers.append(f"ZIP omits allowlisted member: {missing}")
        for extra in sorted(set(regular_names) - allowlist):
            blockers.append(f"ZIP contains non-allowlisted member: {extra}")
        if set(regular_names) == allowlist:
            blockers.append("ZIP member ordering differs from package-files.txt")

    for name in allowlist_lines:
        if name == "asm2_127/PACKAGE-MANIFEST.sha256":
            continue
        if name == "The Amazing Spider-Man 2.sh":
            source_member = source / "sources" / name
        elif name.startswith("asm2_127/"):
            source_member = source / "sources" / name.removeprefix("asm2_127/")
        else:
            blockers.append(f"allowlisted member has no source mapping: {name}")
            continue
        if not source_member.is_file() or source_member.is_symlink():
            blockers.append(f"allowlisted source file is absent or linked: {name}")
        elif name in regular_names and archive.read(name) != source_member.read_bytes():
            blockers.append(f"ZIP member differs from package source: {name}")

    if "asm2_127/cover.png" in regular_names:
        blockers.append("ZIP still contains the omitted cover.png")

    zip_screenshot = "asm2_127/screenshot.png"
    if zip_screenshot not in regular_names:
        blockers.append(f"ZIP omits public metadata image: {zip_screenshot}")
    else:
        zip_screenshot_sha = digest(archive.read(zip_screenshot))
        if zip_screenshot_sha != EXPECTED_SCREENSHOT_SHA256:
            blockers.append(
                "ZIP screenshot.png is not the approved final R36S "
                "gameplay capture"
            )
        if (
            source_screenshot_sha is not None
            and zip_screenshot_sha != source_screenshot_sha
        ):
            blockers.append(
                "ZIP screenshot.png differs from package source"
            )

    zip_provenance = "asm2_127/ART-PROVENANCE.json"
    if zip_provenance not in regular_names:
        blockers.append("ZIP omits ART-PROVENANCE.json")
    elif not provenance_path.is_file():
        blockers.append(
            "ZIP ART-PROVENANCE.json has no package-source counterpart"
        )
    elif archive.read(zip_provenance) != provenance_path.read_bytes():
        blockers.append(
            "ZIP ART-PROVENANCE.json differs from package source"
        )

    zip_port_json = "asm2_127/port.json"
    zip_gameinfo = "asm2_127/gameinfo.xml"
    if zip_port_json not in regular_names:
        blockers.append(f"ZIP omits metadata file: {zip_port_json}")
    if zip_gameinfo not in regular_names:
        blockers.append(f"ZIP omits metadata file: {zip_gameinfo}")
    if zip_port_json in regular_names and zip_gameinfo in regular_names:
        validate_image_metadata(
            "ZIP",
            archive.read(zip_port_json),
            archive.read(zip_gameinfo),
        )

    manifest_name = "asm2_127/PACKAGE-MANIFEST.sha256"
    if manifest_name not in regular_names:
        blockers.append("ZIP has no PACKAGE-MANIFEST.sha256")
    else:
        manifest_payload = archive.read(manifest_name).decode(
            "utf-8", errors="strict"
        )
        manifest_entries: dict[str, str] = {}
        for line_number, line in enumerate(manifest_payload.splitlines(), 1):
            match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
            if not match:
                blockers.append(f"invalid manifest line {line_number}")
                continue
            expected_hash, name = match.groups()
            if name in manifest_entries:
                blockers.append(f"duplicate manifest entry: {name}")
                continue
            manifest_entries[name] = expected_hash
            if name not in regular_names:
                blockers.append(f"manifest names absent ZIP member: {name}")
            elif digest(archive.read(name)) != expected_hash:
                blockers.append(f"manifest hash mismatch: {name}")

        expected_manifest_names = set(regular_names) - {manifest_name}
        actual_manifest_names = set(manifest_entries)
        for missing in sorted(expected_manifest_names - actual_manifest_names):
            blockers.append(f"manifest omits public member: {missing}")
        for extra in sorted(actual_manifest_names - expected_manifest_names):
            blockers.append(f"manifest has unexpected member: {extra}")

lines = [
    "# ASM2 public package audit",
    "",
    f"Verdict: {'FAIL' if blockers else 'PASS'}",
    "",
    "## Public art",
    "",
    "| File | Dimensions | SHA-256 | Provenance |",
    "| --- | ---: | --- | --- |",
]
for name, dimensions, sha, state in art_rows:
    lines.append(f"| `{name}` | {dimensions} | `{sha}` | {state} |")

lines.extend(
    [
        "",
        "## Bundled ELF ABI",
        "",
        "| File | Newest GLIBC |",
        "| --- | ---: |",
    ]
)
for name, newest in elf_abi_rows:
    rendered = f"GLIBC_{newest}" if newest != "none" else "none/static"
    lines.append(f"| `{name}` | {rendered} |")

lines.extend(["", "## Manifest and privacy", ""])
for note in notes:
    lines.append(f"- PASS: {note}")
if blockers:
    for blocker in blockers:
        lines.append(f"- BLOCKER: {blocker}")
else:
    lines.append("- PASS: no package blocker found")

lines.extend(
    [
        "",
        "The audit records no local input paths. It checks the final ZIP "
        "directly and requires the manifest to cover every public member "
        "except the manifest itself.",
        "",
    ]
)
report_path.write_text("\n".join(lines), encoding="utf-8")
print(f"public package audit: {'FAIL' if blockers else 'PASS'}")
print(f"blockers: {len(blockers)}")
raise SystemExit(1 if blockers else 0)
PY
