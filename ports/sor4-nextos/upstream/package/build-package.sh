#!/usr/bin/env bash
# Build the Streets of Rage 4 PortMaster archive from a strict BYO-data allowlist.
set -euo pipefail

export LC_ALL=C
export TZ=UTC

fail() {
    printf 'package error: %s\n' "$*" >&2
    exit 1
}

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
PACKAGE_SOURCE="$PORT_DIR/port/package"
STATIC_DIR="$PACKAGE_SOURCE/sor4"
TOOLS_SOURCE="$PACKAGE_SOURCE/tools"
HOST_SOURCE="$PORT_DIR/build/host_pkg"
BUILD_DIR="$PORT_DIR/build"
ALLOWLIST="$SCRIPT_DIR/package-files.txt"
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1783900800}

case "$SOURCE_DATE_EPOCH" in
    ''|*[!0-9]*) fail "SOURCE_DATE_EPOCH must be a Unix timestamp" ;;
esac
(( SOURCE_DATE_EPOCH >= 315532800 )) || fail "SOURCE_DATE_EPOCH predates ZIP timestamps"
(( SOURCE_DATE_EPOCH <= 4354819198 )) || fail "SOURCE_DATE_EPOCH exceeds ZIP timestamps"
(( SOURCE_DATE_EPOCH % 2 == 0 )) || fail "SOURCE_DATE_EPOCH must use ZIP's two-second granularity"

for tool in awk basename bash cmp comm dirname find grep head install mkdir \
            mktemp mv python3 readelf rm sed sha256sum sort stat strings tail \
            touch unzip zip; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing host tool: $tool"
done

[[ -f "$ALLOWLIST" ]] || fail "missing allowlist: $ALLOWLIST"
[[ -d "$STATIC_DIR" ]] || fail "missing canonical package metadata: $STATIC_DIR"
[[ -d "$TOOLS_SOURCE" ]] || fail "missing canonical setup tools: $TOOLS_SOURCE"
[[ -d "$HOST_SOURCE" ]] || fail "missing self-contained host publish: $HOST_SOURCE"

OUT=${1:-"$SCRIPT_DIR/dist/sor4.zip"}
OUT_DIR=$(dirname -- "$OUT")
OUT_NAME=$(basename -- "$OUT")
mkdir -p -- "$OUT_DIR"
OUT_DIR=$(cd -- "$OUT_DIR" && pwd -P)
OUT="$OUT_DIR/$OUT_NAME"

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/sor4-package.XXXXXX")
STAGE="$TMP_ROOT/stage"
ZIP_A="$TMP_ROOT/repro-a.zip"
ZIP_B="$TMP_ROOT/repro-b.zip"
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
    [[ -f "$source" && ! -L "$source" ]] || fail "missing or unsafe package source: $source"
    install -D -m "$mode" -- "$source" "$STAGE/$destination"
}

mode_for() {
    case "$1" in
        StreetsOfRage4.sh|sor4/host_pkg/sor4host|sor4/host_pkg/createdump|\
        sor4/tools/sor4_profile.sh|sor4/tools/sor4_setup.sh|\
        sor4/tools/sor4probe|sor4/tools/sor4splash)
            printf '0755\n'
            ;;
        *)
            printf '0644\n'
            ;;
    esac
}

# Every staged source is selected by the destination allowlist. Nothing is copied
# recursively, so a dirty build directory cannot leak into the community archive.
while IFS= read -r rel; do
    [[ "$rel" == "sor4/PACKAGE-MANIFEST.sha256" ]] && continue
    mode=$(mode_for "$rel")
    case "$rel" in
        StreetsOfRage4.sh)
            source="$PACKAGE_SOURCE/StreetsOfRage4.sh"
            ;;
        sor4/host_pkg/sdl3compat/libSDL2-2.0.so.0)
            # The generic host publish still contains a workstation-linked copy.
            # Package only the audited Debian-Buster AArch64 build (GLIBC 2.17).
            source="$BUILD_DIR/sdl2-compat/build-buster/libSDL2-2.0.so.0.3200.71"
            ;;
        sor4/host_pkg/*)
            source="$HOST_SOURCE/${rel#sor4/host_pkg/}"
            ;;
        sor4/tools/sor4probe)
            source="$BUILD_DIR/sor4probe"
            ;;
        sor4/tools/sor4splash)
            source="$BUILD_DIR/sor4splash"
            ;;
        sor4/tools/sor4bake.rgba)
            source="$BUILD_DIR/placards/sor4bake.rgba"
            ;;
        sor4/tools/*)
            source="$TOOLS_SOURCE/${rel#sor4/tools/}"
            ;;
        sor4/sor4.gptk)
            source="$PACKAGE_SOURCE/sor4.gptk"
            ;;
        sor4/*)
            source="$STATIC_DIR/${rel#sor4/}"
            ;;
        *)
            fail "allowlist path has no canonical source mapping: $rel"
            ;;
    esac
    put "$mode" "$source" "$rel"
done < "$ALLOWLIST"

python3 - "$STAGE/sor4/port.json" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, encoding="utf-8") as stream:
    data = json.load(stream)

if data.get("version") != 4:
    raise SystemExit("port.json schema version must be 4 for current HarbourMaster")
if data.get("name") != "sor4.zip":
    raise SystemExit("port.json name must be the stable identifier sor4.zip")
if data.get("items") != ["StreetsOfRage4.sh", "sor4"]:
    raise SystemExit("port.json items do not match the archive layout")
if data.get("attr", {}).get("arch") != ["aarch64"]:
    raise SystemExit("port.json must declare only aarch64")
joined = json.dumps(data, ensure_ascii=True).lower()
for marker in ("1.4.5", "1gb", "2gb", "astc", "byo-data"):
    if marker not in joined:
        raise SystemExit(f"port.json is missing release contract marker: {marker}")
PY

python3 - "$STAGE/sor4/gameinfo.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
game = root.find("game")
if game is None or game.findtext("path") != "./StreetsOfRage4.sh":
    raise SystemExit("gameinfo.xml does not point to ./StreetsOfRage4.sh")
if game.findtext("image") != "./sor4/cover.png":
    raise SystemExit("gameinfo.xml does not point to ./sor4/cover.png")
if "1.4.5" not in (game.findtext("desc") or ""):
    raise SystemExit("gameinfo.xml must identify Android data version 1.4.5")
PY

python3 - "$STAGE/sor4/sor4.cfg.default" <<'PY'
import sys

expected = {
    "profile": "auto",
    "texture_streaming": "auto",
    "texture_budget_mb": "auto",
    "texture_quality": "auto",
    "streaming_log": "auto",
}
values = {}
with open(sys.argv[1], encoding="utf-8") as stream:
    for number, line in enumerate(stream, 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.count("=") != 1:
            raise SystemExit(f"sor4.cfg line {number} is not one key=value")
        key, value = (part.strip() for part in line.split("=", 1))
        if key in values:
            raise SystemExit(f"duplicate sor4.cfg key: {key}")
        values[key] = value
if values != expected:
    raise SystemExit(f"unexpected sor4.cfg defaults: {values!r}")
PY

python3 - "$STAGE/sor4" <<'PY'
import pathlib
import struct
import sys

root = pathlib.Path(sys.argv[1])
expected = {
    "cover.png": (1280, 485),
    "screenshot.png": (1280, 720),
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

[[ "$(stat -c %s "$STAGE/sor4/tools/sor4bake.rgba")" == 1228800 ]] || \
    fail "sor4bake.rgba must be an exact 640x480 RGBA frame"
grep -Fq -- '## 2.0.2' "$STAGE/sor4/CHANGELOG.md" || \
    fail "CHANGELOG.md is missing the 2.0.2 release entry"

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

LAUNCHER="$STAGE/StreetsOfRage4.sh"
SETUP="$STAGE/sor4/tools/sor4_setup.sh"
PROFILE="$STAGE/sor4/tools/sor4_profile.sh"

for marker in sor4_profile.sh sor4_setup.sh '$PKG/sor4host' '--starter' \
              pm_platform_helper pm_finish LD_LIBRARY_PATH; do
    grep -Fq -- "$marker" "$LAUNCHER" || fail "launcher is missing required contract: $marker"
done
# "Thin" means the launcher only negotiates the PortMaster contract and hands over to
# the starter: no setup, profile, texture or audio logic. The budget covers that plus
# the orphan reaper, which has to live in the shell because it must still fire when the
# frontend kills the launcher itself.
[[ "$(wc -l < "$LAUNCHER")" -le 95 ]] ||
    fail "PortMaster launcher is no longer thin (maximum 95 lines)"
for marker in --starter --game sor4_profile.sh sor4_setup.sh \
              libharfbuzz.so.0 libfreetype.so.6 libopenal.so.1 \
              SDL_NO_SIGNAL_HANDLERS SOR4_ASSETS SOR4_AUDIO SOR4_BANKDIR \
              ALSOFT_CONF PULSE_SERVER LD_LIBRARY_PATH; do
    strings -el "$STAGE/sor4/host_pkg/sor4host.dll" | grep -Fq -- "$marker" ||
        fail "compiled starter is missing runtime contract: $marker"
done
if grep -IRnE '^[[:space:]]*(export[[:space:]]+)?SDL_(VIDEO|AUDIO)DRIVER=' \
        "$STAGE" --include='*.sh'; then
    fail "package shell code must not force an SDL video or audio backend"
fi
if grep -IRnE '(^|[[:space:]])(setsid|systemctl[[:space:]]+(stop|mask))[[:space:]]' \
        "$STAGE" --include='*.sh'; then
    fail "package shell code contains a forbidden lifecycle command"
fi

for marker in sor4_apkset.py validate-sor4-apk.py sor4_apkextract.py wwise_extract.py \
              sor4texconv.dll patchgam.dll noopm.dll skipcall.dll \
              fixplatform.dll skipvideo.dll verstub.dll rettrue.dll \
              coopsplit.dll sor4splash; do
    grep -Fq -- "$marker" "$SETUP" || fail "setup is missing required contract: $marker"
done
for marker in asset_count missing_xnb_token one-missing-xnb; do
    grep -Fq -- "$marker" "$SETUP" ||
        fail "setup is missing one-XNB compatibility contract: $marker"
done
for marker in EXPECTED_XNB_SQUARE_SUM EXPECTED_NON_XNB_SQUARE_SUM \
              one-missing-XNB missing_xnb_token assets/blank.xnb; do
    grep -Fq -- "$marker" "$STAGE/sor4/tools/validate-sor4-apk.py" ||
        fail "validator is missing one-XNB compatibility proof: $marker"
done
for marker in 'asset_count=25904' 'missing_xnb_token=' '[asset COMPAT]' 'blank.xnb'; do
    strings -el "$STAGE/sor4/host_pkg/SOR4Bridge.dll" | grep -Fq -- "$marker" ||
        fail "runtime bridge is missing one-XNB fallback contract: $marker"
done
grep -Fq -- 'sor4_sdl_compat_dir "$GAMEDIR"' "$SETUP" ||
    fail "setup splash does not share the SDL3 compatibility policy"
grep -Fq -- 'LD_LIBRARY_PATH="$splash_ld"' "$SETUP" ||
    fail "setup splash does not receive its selected SDL compatibility path"
grep -Eq '^[[:space:]]*drivers[[:space:]]*=[[:space:]]*pulse,alsa[[:space:]]*$' \
    "$STAGE/sor4/alsoft.conf" || fail "alsoft.conf does not match bundled audio backends"
grep -Fq -- 'Arm astcenc 5.0.0' "$STAGE/sor4/licenses/LICENSE.md" ||
    fail "license inventory does not credit the bundled ASTC decoder"
grep -Fq -- 'StbImageSharp / StbImageWriteSharp' "$STAGE/sor4/licenses/LICENSE.md" ||
    fail "license inventory does not credit bundled StbSharp assemblies"
grep -Fq -- 'sor4probe' "$PROFILE" || fail "profile selector does not call the GLES probe"
grep -Fq -- 'MemTotal' "$PROFILE" || fail "profile selector does not use physical RAM"
grep -Eq '^sor4_sdl3_available[[:space:]]*\(\)' "$PROFILE" ||
    fail "profile helper does not define the shared SDL3 policy"
grep -Eq '^sor4_sdl_compat_dir[[:space:]]*\(\)' "$PROFILE" ||
    fail "profile helper does not define the shared SDL compatibility path"

check_release_hash() {
    local file=$1 expected=$2 actual
    actual=$(sha256sum "$file" | awk '{print $1}')
    [[ "$actual" == "$expected" ]] || \
        fail "$file is not the approved reproducible release build (found: $actual)"
}
check_release_hash "$STAGE/sor4/host_pkg/MonoGame.Framework.dll" \
    c4bd42330260009651f7149d5a6943b58709b0a05e93fa9738e864e781c2a6f3
check_release_hash "$STAGE/sor4/host_pkg/sor4host.dll" \
    e9876740e972a6d73140f56b12e95366811ebd1c9080bea675fb35fb7e479070
check_release_hash "$STAGE/sor4/host_pkg/libs/fallback/libfreetype.so.6" \
    9fc67a9721d5e51df6c23953d7e9525340a5c4bcc6f593fc160c5cd4cf1e375d
check_release_hash "$STAGE/sor4/host_pkg/libs/fallback/libharfbuzz.so.0" \
    41a5544bb4d43aca8badae1dec1474f3c896c1e8694ad0c0e4f91c8df900c236
check_release_hash "$STAGE/sor4/host_pkg/libs/libWwise.so" \
    0ad50b0fa0625f42f5692ec846606ef0996ea4833e57af583647241a9bc18460
check_release_hash "$STAGE/sor4/host_pkg/libs/libogg.so.0" \
    44490dd63c36d86797631e4a0475a6a4b1e82293f020063df82bed00a138537d
check_release_hash "$STAGE/sor4/host_pkg/libs/libopenal.so.1" \
    36a116cc1b4a100876b0759d2b954760932db492c98be500e4308cb9c31b508b
check_release_hash "$STAGE/sor4/host_pkg/libs/libopus.so.0" \
    4c4e877b2b39f975ee051cd31bace72fde3cd676bc2de81f440deb0e087f894c
check_release_hash "$STAGE/sor4/host_pkg/libs/libopusfile.so.0" \
    5355cc8d3a21d62de494b58858bc341128ee998da5a7e4321486275b46f78117
check_release_hash "$STAGE/sor4/tools/sor4texconv.dll" \
    a3755e2bc82ccb37916ef2d97ed01a7e1df92bbe5ded677cb115b4e493cfba9c

python3 - "$STAGE/sor4/host_pkg/sor4host.deps.json" "$STAGE/sor4/host_pkg" <<'PY'
import json
import pathlib
import sys

deps_path = pathlib.Path(sys.argv[1])
host = pathlib.Path(sys.argv[2])
data = json.loads(deps_path.read_text(encoding="utf-8"))
target_name = data.get("runtimeTarget", {}).get("name", "")
if target_name != ".NETCoreApp,Version=v9.0/linux-arm64":
    raise SystemExit(f"unexpected host runtime target: {target_name}")
target = data.get("targets", {}).get(target_name)
if not isinstance(target, dict):
    raise SystemExit("host dependency target is missing")
for description in target.values():
    for group in ("runtime", "native"):
        for asset in description.get(group, {}):
            name = pathlib.PurePosixPath(asset).name
            if not (host / name).is_file():
                raise SystemExit(f"self-contained host dependency is missing: {name}")
PY

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

# Validate every ELF, including CoreCLR, native bridges and setup helpers. Managed
# PE assemblies are ignored by readelf and validated through the dependency graph.
while IFS= read -r -d '' candidate; do
    if readelf -h "$candidate" >/dev/null 2>&1; then
        check_aarch64_glibc "$candidate" 2.27
    fi
done < <(find "$STAGE" -type f -print0)

check_soname() {
    local elf=$1 expected=$2
    readelf -d "$elf" | grep -F "Library soname: [$expected]" >/dev/null || \
        fail "$elf does not declare SONAME $expected"
}

check_needed() {
    local elf=$1 expected=$2
    readelf -d "$elf" | grep -F "Shared library: [$expected]" >/dev/null || \
        fail "$elf does not declare required dependency $expected"
}

check_soname "$STAGE/sor4/host_pkg/sdl3compat/libSDL2-2.0.so.0" libSDL2-2.0.so.0
check_soname "$STAGE/sor4/host_pkg/libs/libopenal.so.1" libopenal.so.1
check_soname "$STAGE/sor4/host_pkg/libs/libopusfile.so.0" libopusfile.so.0
check_soname "$STAGE/sor4/host_pkg/libs/libopus.so.0" libopus.so.0
check_soname "$STAGE/sor4/host_pkg/libs/libogg.so.0" libogg.so.0
check_needed "$STAGE/sor4/host_pkg/libs/libopusfile.so.0" libopus.so.0
check_needed "$STAGE/sor4/host_pkg/libs/libopusfile.so.0" libogg.so.0
SDL2COMPAT_HASH=$(sha256sum "$STAGE/sor4/host_pkg/sdl3compat/libSDL2-2.0.so.0" | awk '{print $1}')
[[ "$SDL2COMPAT_HASH" == 59bacc86a3b54e94669caa39d034add9b469cf247db703a7f853c8a01f8130c4 ]] || \
    fail "sdl2-compat is not the audited GLIBC 2.17 build"

REAL_WWISE_HASH=4db3d430cde5525f3eeaa99af6e46c44feb554d89d76955b1391dfd5dd2cf0f2
WRAPPER_HASH=$(sha256sum "$STAGE/sor4/host_pkg/libs/libWwise.so" | awk '{print $1}')
[[ "$WRAPPER_HASH" != "$REAL_WWISE_HASH" ]] || fail "proprietary libWwise.real.so entered the package"

# Hash immutable payloads. HarbourMaster may sign the launcher and merge install
# state into port.json, so those two files are intentionally not in the manifest.
(
    cd -- "$STAGE"
    while IFS= read -r rel; do
        case "$rel" in
            StreetsOfRage4.sh|sor4/port.json|sor4/PACKAGE-MANIFEST.sha256)
                continue
                ;;
        esac
        sha256sum -- "$rel"
    done < "$ALLOWLIST"
) > "$STAGE/sor4/PACKAGE-MANIFEST.sha256"

find "$STAGE" -type f -printf '%P\n' | sort > "$ACTUAL"
if ! cmp -s "$EXPECTED" "$ACTUAL"; then
    printf '%s\n' 'Unexpected package contents:' >&2
    comm -3 "$EXPECTED" "$ACTUAL" >&2
    fail "staged files differ from package-files.txt"
fi

while IFS= read -r rel; do
    lower=${rel,,}
    case "$lower" in
        *.apk|*.apks|*.apkm|*.xapk|*.obb|*.aab|*.dex|*.pdb|*.pyc|*.pyo|\
        *.log|*.cache|*.idx|*.src|*.ttf|*.otf|*/sor4.dll|\
        */libwwise.real.so|*/eossdk.android.dll|*/helpshiftsdkx.android.dll|\
        */standalonetypemodel.android.retail.dll|*/sharpfont*.dll|\
        */xamarin*.dll|*/_microsoft*.dll|*/progressor|\
        sor4/assets/*|sor4/gameassets/*|sor4/audioout/*|\
        sor4/host_pkg/assets/*)
            fail "proprietary, generated or diagnostic path rejected: $rel"
            ;;
        *.xml)
            [[ "$lower" == "sor4/gameinfo.xml" ]] || \
                fail "runtime XML/documentation path rejected: $rel"
            ;;
    esac
done < "$ACTUAL"

if grep -IRnE '/home/|/mnt/|192\.168\.|sshpass|ark@|root@|felipe@|sor4-test|sor4\.dev' \
        "$STAGE" --include='*.md' --include='*.txt' --include='*.json' \
        --include='*.xml' --include='*.sh' --include='*.py' \
        --include='*.cfg' --include='*.gptk'; then
    fail "release metadata contains a local path, test address or credential"
fi

# Managed PE files retain a CodeView/PDB source path even when the .pdb itself is
# omitted. Scan the complete payload so a personal build path cannot hide inside
# an otherwise clean DLL or ELF.
LOCAL_LEAKS="$TMP_ROOT/local-info.txt"
while IFS= read -r -d '' candidate; do
    if match=$(strings "$candidate" 2>/dev/null | \
            grep -E -m 1 '/home/|/root/|/Users/|/mnt/|[A-Za-z]:\\Users\\|192\.168\.|sshpass|ark@|root@|felipe@|sor4-test|sor4\.dev'); then
        printf '%s: %s\n' "${candidate#"$STAGE/"}" "$match"
    fi
done < <(find "$STAGE" -type f -print0) > "$LOCAL_LEAKS"
if [[ -s "$LOCAL_LEAKS" ]]; then
    sed -n '1,20p' "$LOCAL_LEAKS" >&2
    fail "release payload contains an embedded local path, test address or credential"
fi

find "$STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +

make_zip() {
    local destination=$1
    (
        cd -- "$STAGE"
        zip -X -9 -q "$destination" -@ < "$ALLOWLIST"
    )
}

make_zip "$ZIP_A"
make_zip "$ZIP_B"
cmp -s "$ZIP_A" "$ZIP_B" || fail "two deterministic ZIP passes produced different bytes"

unzip -tq "$ZIP_A" >/dev/null
unzip -Z1 "$ZIP_A" > "$TMP_ROOT/archive.txt"
cmp -s "$ALLOWLIST" "$TMP_ROOT/archive.txt" || \
    fail "ZIP entries or ordering differ from package-files.txt"

python3 - "$ZIP_A" "$SOURCE_DATE_EPOCH" <<'PY'
import datetime
import stat
import sys
import zipfile

archive = sys.argv[1]
epoch = int(sys.argv[2])
expected_time = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
expected_tuple = (
    expected_time.year,
    expected_time.month,
    expected_time.day,
    expected_time.hour,
    expected_time.minute,
    expected_time.second,
)
executables = {
    "StreetsOfRage4.sh",
    "sor4/host_pkg/createdump",
    "sor4/host_pkg/sor4host",
    "sor4/tools/sor4_profile.sh",
    "sor4/tools/sor4_setup.sh",
    "sor4/tools/sor4probe",
    "sor4/tools/sor4splash",
}

with zipfile.ZipFile(archive) as bundle:
    names = bundle.namelist()
    if len(names) != len(set(names)):
        raise SystemExit("archive contains duplicate entries")
    for info in bundle.infolist():
        mode = (info.external_attr >> 16) & 0o777
        wanted = 0o755 if info.filename in executables else 0o644
        if mode != wanted:
            raise SystemExit(
                f"bad mode for {info.filename}: {mode:o}, expected {wanted:o}"
            )
        if info.date_time != expected_tuple:
            raise SystemExit(
                f"non-deterministic timestamp for {info.filename}: {info.date_time}"
            )
        if stat.S_ISLNK(info.external_attr >> 16):
            raise SystemExit(f"symlink is not allowed in package: {info.filename}")
PY

VERIFY="$TMP_ROOT/verify"
mkdir -p -- "$VERIFY"
unzip -q "$ZIP_A" -d "$VERIFY"
(
    cd -- "$VERIFY"
    sha256sum -c "sor4/PACKAGE-MANIFEST.sha256" >/dev/null
)

mv -f -- "$ZIP_A" "$OUT"
HASH=$(sha256sum -- "$OUT" | awk '{print $1}')
printf '%s  %s\n' "$HASH" "$OUT_NAME" > "$OUT.sha256"

printf 'OK: %s\n' "$OUT"
printf 'SHA256: %s\n' "$HASH"
