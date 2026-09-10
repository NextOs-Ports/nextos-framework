#!/usr/bin/env bash
# Build the Sonic 4 Episode II V6 PortMaster archive from an explicit BYO-data allowlist.
set -euo pipefail

export LC_ALL=C
export TZ=UTC

fail() {
    printf 'package error: %s\n' "$*" >&2
    exit 1
}

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
STATIC_DIR="$SCRIPT_DIR/sonic4ep2"
LAUNCHER="$SCRIPT_DIR/ports/Sonic4EP2.sh"
ALLOWLIST="$SCRIPT_DIR/package-files.txt"
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1783814400}

case "$SOURCE_DATE_EPOCH" in
    ''|*[!0-9]*) fail "SOURCE_DATE_EPOCH must be a Unix timestamp" ;;
esac
(( SOURCE_DATE_EPOCH >= 315532800 )) || fail "SOURCE_DATE_EPOCH predates ZIP timestamps"
(( SOURCE_DATE_EPOCH <= 4354819198 )) || fail "SOURCE_DATE_EPOCH exceeds ZIP timestamps"
(( SOURCE_DATE_EPOCH % 2 == 0 )) || fail "SOURCE_DATE_EPOCH must use ZIP's two-second granularity"

for tool in awk basename bash cmp comm dirname find grep head install mkdir \
            mktemp mv python3 readelf rm sed sha256sum sort strings tail \
            touch unzip zip; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing host tool: $tool"
done

[[ -f "$ALLOWLIST" ]] || fail "missing allowlist: $ALLOWLIST"
[[ -f "$STATIC_DIR/version.txt" ]] || fail "missing version.txt"
[[ "$(head -n 1 "$STATIC_DIR/version.txt")" == "V6" ]] || \
    fail "version.txt first line must be exactly: V6"

OUT=${1:-"$SCRIPT_DIR/dist/sonic4ep2.zip"}
OUT_DIR=$(dirname -- "$OUT")
OUT_NAME=$(basename -- "$OUT")
mkdir -p -- "$OUT_DIR"
OUT_DIR=$(cd -- "$OUT_DIR" && pwd -P)
OUT="$OUT_DIR/$OUT_NAME"

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/sonic4ep2-v6-package.XXXXXX")
STAGE="$TMP_ROOT/stage"
TMP_ZIP="$TMP_ROOT/$OUT_NAME"
cleanup() {
    rm -rf -- "$TMP_ROOT"
}
trap cleanup EXIT INT TERM
mkdir -p -- "$STAGE"

EXPECTED="$TMP_ROOT/expected.txt"
ACTUAL="$TMP_ROOT/actual.txt"
sort -u "$ALLOWLIST" > "$EXPECTED"
cmp -s "$ALLOWLIST" "$EXPECTED" || fail "package-files.txt must be sorted and unique"

while IFS= read -r rel; do
    [[ -n "$rel" ]] || fail "blank path in package-files.txt"
    case "$rel" in
        /*|../*|*/../*|*/..|*/./*|./*) fail "unsafe allowlist path: $rel" ;;
    esac
done < "$ALLOWLIST"

put() {
    local mode=$1 source=$2 destination=$3
    [[ -f "$source" ]] || fail "missing package source: $source"
    install -D -m "$mode" -- "$source" "$STAGE/$destination"
}

# Runtime files come from the canonical port and never from package/ports/sonic4ep2.
put 0755 "$LAUNCHER"                                  "Sonic4EP2.sh"
put 0755 "$PORT_DIR/sonic4.arm64"                    "sonic4ep2/sonic4.arm64"
put 0755 "$STATIC_DIR/tools/sonic4ep2_extract.sh"    "sonic4ep2/tools/sonic4ep2_extract.sh"
put 0644 "$STATIC_DIR/tools/validate-sonic4-data.py" "sonic4ep2/tools/validate-sonic4-data.py"

for rel in LICENSE.md README-pt-BR.md README.md box.png cover.png gameinfo.xml \
           port.json screenshot.png sfx_map.tsv splash.png version.txt \
           libs.aarch64/libmpg123.so.0 libs.aarch64/libogg.so.0 \
           libs.aarch64/libvorbis.so.0 libs.aarch64/libvorbisfile.so.3 \
           licenses/Apache-2.0-NOTICE.txt licenses/Apache-2.0.txt \
           licenses/LGPL-2.1-or-later.txt licenses/LICENSE.md \
           licenses/SDL2-zlib.txt licenses/Xiph-Ogg-Vorbis-BSD.txt \
           licenses/game-data-notice.txt licenses/mpg123-LGPL-NOTICE.txt; do
    put 0644 "$STATIC_DIR/$rel" "sonic4ep2/$rel"
done

python3 - "$STAGE/sonic4ep2/port.json" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, encoding="utf-8") as stream:
    data = json.load(stream)

if data.get("version") != 4:
    raise SystemExit("port.json schema version must be 4 for current HarbourMaster")
if data.get("name") != "sonic4ep2.zip":
    raise SystemExit("port.json name must be the stable identifier sonic4ep2.zip")
if data.get("items") != ["Sonic4EP2.sh", "sonic4ep2"]:
    raise SystemExit("port.json items do not match the archive layout")
arch = data.get("attr", {}).get("arch", [])
if arch != ["aarch64"]:
    raise SystemExit("port.json must declare only aarch64")
joined = json.dumps(data, ensure_ascii=True)
if "3.0.0-109" not in joined:
    raise SystemExit("port.json must identify Android data version 3.0.0-109")
if "2.0.0" in joined or "armhf" in joined or "armeabi-v7a" in joined:
    raise SystemExit("port.json contains obsolete armv7/v2 data references")
PY

python3 - "$STAGE/sonic4ep2/gameinfo.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
game = root.find("game")
if game is None or game.findtext("path") != "./Sonic4EP2.sh":
    raise SystemExit("gameinfo.xml does not point to ./Sonic4EP2.sh")
if game.findtext("image") != "./sonic4ep2/cover.png":
    raise SystemExit("gameinfo.xml does not point to ./sonic4ep2/cover.png")
if "3.0.0-109" not in (game.findtext("desc") or ""):
    raise SystemExit("gameinfo.xml must identify Android data version 3.0.0-109")
PY

python3 - "$STAGE/sonic4ep2" <<'PY'
import pathlib
import struct
import sys

root = pathlib.Path(sys.argv[1])
expected = {
    "box.png": (915, 1481),
    "cover.png": (1080, 1080),
    "screenshot.png": (640, 480),
    "splash.png": (1280, 720),
}
for name, dimensions in expected.items():
    header = (root / name).read_bytes()[:24]
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{name} is not a valid PNG")
    actual = struct.unpack(">II", header[16:24])
    if actual != dimensions:
        raise SystemExit(
            f"{name} dimensions are {actual[0]}x{actual[1]}, "
            f"expected {dimensions[0]}x{dimensions[1]}"
        )
PY

# The embedded first-run loader and extractor must agree on this exact path.
grep -Fq 'validate-sonic4-data.py' \
    "$STAGE/sonic4ep2/tools/sonic4ep2_extract.sh" || \
    fail "extractor does not reference validate-sonic4-data.py"
strings "$STAGE/sonic4ep2/sonic4.arm64" | \
    grep -Fx 'tools/sonic4ep2_extract.sh' >/dev/null || \
    fail "loader does not reference tools/sonic4ep2_extract.sh"
if find "$STAGE" -type f -name '*.src' -print -quit | grep . >/dev/null; then
    fail "obsolete .src extractor found in package"
fi

while IFS= read -r script; do
    bash -n "$script"
done < <(find "$STAGE" -type f -name '*.sh' -print | sort)

while IFS= read -r script; do
    PYTHONPYCACHEPREFIX="$TMP_ROOT/pycache" python3 -m py_compile "$script"
done < <(find "$STAGE" -type f -name '*.py' -print | sort)

if find "$STAGE" \( -type d -name '__pycache__' -o -type f \
        \( -name '*.pyc' -o -name '*.pyo' \) \) -print -quit | grep . >/dev/null; then
    fail "Python bytecode/cache must not enter the release stage"
fi

check_aarch64_glibc() {
    local elf=$1 max_allowed=$2 machine newest highest
    machine=$(readelf -h "$elf" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
    [[ "$machine" == "AArch64" ]] || fail "$elf is not AArch64 (found: $machine)"

    newest=$(readelf --version-info "$elf" 2>/dev/null |
        sed -n 's/.*Name: GLIBC_\([0-9][0-9.]*\).*/\1/p' | sort -Vu | tail -n 1)
    [[ -n "$newest" ]] || return 0
    highest=$(printf '%s\n%s\n' "$max_allowed" "$newest" | sort -V | tail -n 1)
    [[ "$highest" == "$max_allowed" ]] || \
        fail "$elf requires GLIBC_$newest (maximum allowed is GLIBC_$max_allowed)"
}

check_soname() {
    local elf=$1 expected=$2
    readelf -d "$elf" | grep -F "Library soname: [$expected]" >/dev/null || \
        fail "$elf does not declare SONAME $expected"
}

check_aarch64_glibc "$STAGE/sonic4ep2/sonic4.arm64" 2.30
for soname in libmpg123.so.0 libogg.so.0 libvorbis.so.0 libvorbisfile.so.3; do
    check_aarch64_glibc "$STAGE/sonic4ep2/libs.aarch64/$soname" 2.30
    check_soname "$STAGE/sonic4ep2/libs.aarch64/$soname" "$soname"
done

for needed in libmpg123.so.0 libvorbisfile.so.3; do
    readelf -d "$STAGE/sonic4ep2/sonic4.arm64" | \
        grep -F "Shared library: [$needed]" >/dev/null || \
        fail "loader does not declare required dependency $needed"
done

# Hash every payload file. The manifest intentionally excludes its own hash.
(
    cd -- "$STAGE"
    while IFS= read -r rel; do
        case "$rel" in
            Sonic4EP2.sh|sonic4ep2/port.json|sonic4ep2/PACKAGE-MANIFEST.sha256)
                # HarbourMaster adds its signature to the launcher and merges
                # install status/files into port.json. Keep the installed
                # manifest limited to immutable package payloads.
                continue
                ;;
        esac
        sha256sum -- "$rel"
    done < "$ALLOWLIST"
) > "$STAGE/sonic4ep2/PACKAGE-MANIFEST.sha256"

find "$STAGE" -type f -printf '%P\n' | sort > "$ACTUAL"
if ! cmp -s "$EXPECTED" "$ACTUAL"; then
    printf '%s\n' 'Unexpected package contents:' >&2
    comm -3 "$EXPECTED" "$ACTUAL" >&2
    fail "staged files differ from package-files.txt"
fi

while IFS= read -r rel; do
    lower=${rel,,}
    case "$lower" in
        *.apk|*.apks|*.apkm|*.xapk|*.obb|*.aab|*.dex|*.idx|*.f2f|*.pyc|*.pyo|\
        */libfox.so|*/data.obb|*/split_packs.apk|*/split_config.*.apk|\
        sonic4ep2/data/*|sonic4ep2/lib/*)
            fail "proprietary game-data path rejected: $rel"
            ;;
    esac
done < "$ACTUAL"

if grep -IRnE '/home/|192\.168\.|/storage/roms/ports/sonic4(\.dev|/)' \
        "$STAGE" --include='*.md' --include='*.txt' --include='*.json' \
        --include='*.xml' --include='*.sh'; then
    fail "release metadata contains a local path, test address or development path"
fi

find "$STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +

(
    cd -- "$STAGE"
    zip -X -9 -q "$TMP_ZIP" -@ < "$ALLOWLIST"
)

unzip -tq "$TMP_ZIP" >/dev/null
unzip -Z1 "$TMP_ZIP" > "$TMP_ROOT/archive.txt"
cmp -s "$ALLOWLIST" "$TMP_ROOT/archive.txt" || \
    fail "ZIP entries or ordering differ from package-files.txt"

python3 - "$TMP_ZIP" "$SOURCE_DATE_EPOCH" <<'PY'
import datetime
import stat
import sys
import zipfile

archive = sys.argv[1]
epoch = int(sys.argv[2])
expected_time = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
expected_tuple = (expected_time.year, expected_time.month, expected_time.day,
                  expected_time.hour, expected_time.minute, expected_time.second)
executables = {
    "Sonic4EP2.sh",
    "sonic4ep2/sonic4.arm64",
    "sonic4ep2/tools/sonic4ep2_extract.sh",
}

with zipfile.ZipFile(archive) as bundle:
    names = bundle.namelist()
    if names.count("sonic4ep2/tools/sonic4ep2_extract.sh") != 1:
        raise SystemExit("archive must contain exactly one canonical extractor")
    for info in bundle.infolist():
        mode = (info.external_attr >> 16) & 0o777
        wanted = 0o755 if info.filename in executables else 0o644
        if mode != wanted:
            raise SystemExit(f"bad mode for {info.filename}: {mode:o}, expected {wanted:o}")
        if info.date_time != expected_tuple:
            raise SystemExit(f"non-deterministic timestamp for {info.filename}: {info.date_time}")
        if stat.S_ISLNK(info.external_attr >> 16):
            raise SystemExit(f"symlink is not allowed in package: {info.filename}")
PY

VERIFY="$TMP_ROOT/verify"
mkdir -p -- "$VERIFY"
unzip -q "$TMP_ZIP" -d "$VERIFY"
(
    cd -- "$VERIFY"
    sha256sum -c "sonic4ep2/PACKAGE-MANIFEST.sha256" >/dev/null
)

mv -f -- "$TMP_ZIP" "$OUT"
HASH=$(sha256sum -- "$OUT" | awk '{print $1}')
printf '%s  %s\n' "$HASH" "$OUT_NAME" > "$OUT.sha256"

printf 'OK: %s\n' "$OUT"
printf 'SHA256: %s\n' "$HASH"
