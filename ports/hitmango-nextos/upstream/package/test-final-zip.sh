#!/usr/bin/env bash
# Exercise the exact archived launcher with lightweight host target fixtures.
set -euo pipefail

export LC_ALL=C
export TZ=UTC
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPOSITORY_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
ARCHIVE=${1:-"$REPOSITORY_ROOT/dist/v1.2.2/hitmango.zip"}

fail() {
  printf 'hitmango final ZIP gate: FAIL: %s\n' "$*" >&2
  exit 1
}

[[ -f $ARCHIVE && ! -L $ARCHIVE ]] || fail "unsafe or missing archive: $ARCHIVE"

privacy_summary=$(
  python3 -B - "$ARCHIVE" <<'PY'
import pathlib
import re
import sys
import zipfile


archive = pathlib.Path(sys.argv[1])
text_suffixes = {".md", ".txt", ".json"}
source_patterns = (
    ("APKPure", re.compile(r"apk[\s._/-]{0,3}pure", re.IGNORECASE)),
    ("APKMirror", re.compile(r"apk[\s._/-]{0,3}mirror", re.IGNORECASE)),
    ("APKVision", re.compile(r"apk[\s._/-]{0,3}vision", re.IGNORECASE)),
    ("5play", re.compile(r"5[\s._/-]{0,3}play", re.IGNORECASE)),
    ("APKCombo", re.compile(r"apk[\s._/-]{0,3}combo", re.IGNORECASE)),
    ("Uptodown", re.compile(r"uptodown", re.IGNORECASE)),
)
mod_source_label = re.compile(
    r"(?<![a-z0-9])mod(?:ded|ified)?[\s._-]*source(?![a-z0-9])",
    re.IGNORECASE,
)
mod_marker = re.compile(
    r"(?<![a-z0-9])mod(?:ded|ified)?(?![a-z0-9])", re.IGNORECASE
)
container_suffix = re.compile(r"\.(?:apk|apkm|apks|xapk)$", re.IGNORECASE)
filename_candidate = re.compile(
    r"""(?ix)
    (?P<quote>["'`])
    (?P<quoted>[^"'`\r\n]{1,240}\.(?:apk|apkm|apks|xapk))
    (?P=quote)
    |
    (?P<bare>[^\s"'`<>|=,:;]{1,240}\.(?:apk|apkm|apks|xapk))
    """
)
filename_label = re.compile(
    r"""(?ix)
    \b(?:reference[\s_-]*filename|file[\s_-]*name|filename|
        nome[\s_-]*(?:de[\s_-]*)?(?:refer[eê]ncia|arquivo))\b
    \s*[:=]\s*(?P<name>[^\r\n]{1,240}\.(?:apk|apkm|apks|xapk))
    """
)


def fail(message):
    raise SystemExit("public text/privacy scan: " + message)


def source_name(value):
    for label, pattern in source_patterns:
        if pattern.search(value):
            return label
    return None


def is_mod_source_filename(value):
    candidate = value.strip().split("?", 1)[0].split("#", 1)[0]
    candidate = candidate.rstrip(".)]}>,;")
    basename = candidate.replace("\\", "/").rsplit("/", 1)[-1]
    return bool(container_suffix.search(basename) and mod_marker.search(basename))


def scan_text(member, text):
    origin = source_name(text)
    if origin:
        fail("%s exposes forbidden origin %s" % (member, origin))
    if mod_source_label.search(text):
        fail("%s exposes a mod-source label" % member)
    for match in filename_candidate.finditer(text):
        candidate = match.group("quoted") or match.group("bare")
        if is_mod_source_filename(candidate):
            fail("%s exposes modified-package filename %s" %
                 (member, candidate))
    for match in filename_label.finditer(text):
        candidate = match.group("name").strip()
        if is_mod_source_filename(candidate):
            fail("%s exposes modified-package filename %s" %
                 (member, candidate))


try:
    with zipfile.ZipFile(archive) as bundle:
        text_count = 0
        member_count = 0
        total_text_bytes = 0
        for info in bundle.infolist():
            if info.is_dir():
                continue
            member_count += 1
            origin = source_name(info.filename)
            if origin:
                fail("archive member name exposes forbidden origin %s: %s" %
                     (origin, info.filename))
            if mod_source_label.search(info.filename) or \
                    is_mod_source_filename(info.filename):
                fail("archive member name exposes a mod-source filename: %s" %
                     info.filename)
            if pathlib.PurePosixPath(info.filename).suffix.lower() not in \
                    text_suffixes:
                continue
            if info.file_size > 8 * 1024 * 1024:
                fail("public text member exceeds 8 MiB: %s" % info.filename)
            payload = bundle.read(info)
            total_text_bytes += len(payload)
            if total_text_bytes > 32 * 1024 * 1024:
                fail("public text members exceed the 32 MiB scan budget")
            try:
                text = payload.decode("utf-8")
            except UnicodeDecodeError:
                fail("public text member is not UTF-8: %s" % info.filename)
            scan_text(info.filename, text)
            text_count += 1
except (OSError, zipfile.BadZipFile, RuntimeError) as error:
    fail("cannot inspect final ZIP: %s" % error)

print("text-files=%d member-names=%d" % (text_count, member_count))
PY
) || fail 'public text/privacy scan rejected the final ZIP'
printf 'hitmango final ZIP privacy gate: PASS %s\n' "$privacy_summary"

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/hitmango-final-zip.XXXXXX")
cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/hitmango-final-zip.*)
      [[ -d $TEST_ROOT ]] && rm -rf -- "$TEST_ROOT"
      ;;
    *) printf 'refusing unsafe cleanup target: %s\n' "$TEST_ROOT" >&2 ;;
  esac
}
trap cleanup EXIT INT TERM

PORTS_ROOT="$TEST_ROOT/roms/ports"
XDG_ROOT="$TEST_ROOT/xdg"
PM_ROOT="$XDG_ROOT/PortMaster"
RUNTIME_ROOT="$TEST_ROOT/runtime"
MARKERS="$TEST_ROOT/markers"
NO_STAT="$TEST_ROOT/no-stat"
mkdir -p -- "$PORTS_ROOT" "$PM_ROOT" "$RUNTIME_ROOT" "$MARKERS" "$NO_STAT"
chmod 0700 "$RUNTIME_ROOT"
unzip -q "$ARCHIVE" -d "$PORTS_ROOT"

[[ -f $PORTS_ROOT/hitmango/INSTALLATION.md ]] ||
  fail 'INSTALLATION.md is absent from the final ZIP'
[[ -x $PORTS_ROOT/Hitman\ GO.sh ]] || fail 'launcher mode was lost'
[[ -x $PORTS_ROOT/hitmango/nxsplash-nextos ]] || fail 'nxsplash mode was lost'
[[ -x $PORTS_ROOT/hitmango/nxextract/nxextract-ui ]] ||
  fail 'NXExtract UI mode was lost'
[[ $(<"$PORTS_ROOT/hitmango/version.txt") == 1.2.2 ]] ||
  fail 'port version drifted from 1.2.2'
grep -aFq '[hgo/video] retrying portable EGL/GLES provider names' \
  "$PORTS_ROOT/hitmango/bin/aarch64/hitmango-nextos" ||
  fail 'ArkOS/KMSDRM provider recovery is absent from the packaged binary'
control_contract=$(env -i PATH="$PATH" GAMEDIR="$PORTS_ROOT/hitmango" \
  bash -c '. "$1" >/dev/null; printf "%s:%s:%s:%s" "$HGO_CURSOR" \
    "$HGO_SWAP_STICKS" "$HGO_CLICK_A" "$HGO_SWIPE_MOVE"' \
  bash "$PORTS_ROOT/hitmango/port-env.sh")
[[ $control_contract == 1:1:1:1 ]] ||
  fail "v1.2.0 default control contract drifted ($control_contract)"
control_contract=$(env -i PATH="$PATH" GAMEDIR="$PORTS_ROOT/hitmango" \
  HGO_SWAP_STICKS=0 HGO_CLICK_A=0 \
  bash -c '. "$1" >/dev/null; printf "%s:%s" "$HGO_SWAP_STICKS" \
    "$HGO_CLICK_A"' bash "$PORTS_ROOT/hitmango/port-env.sh")
[[ $control_contract == 0:0 ]] ||
  fail 'right-stick/R3 alternative is not explicit opt-in'
[[ $(sha256sum -- "$PORTS_ROOT/hitmango/nxextract/nxextract-ui" | awk '{print $1}') == \
   7ca901d8515ab9a084be81e05888e1fd03cec80fb03896df6331c1c95698ef56 ]] ||
  fail 'NXExtract UI identity drifted in the final ZIP'
[[ $(sha256sum -- "$PORTS_ROOT/hitmango/nxsplash-nextos" | awk '{print $1}') == \
   d85d896a906a778c9af250e5617d45d085a98b18552cb0254addbbc626036c97 ]] ||
  fail 'NXSplash identity drifted in the final ZIP'
[[ ! -e $PORTS_ROOT/hitmango/run.sh ]] ||
  fail 'retired intermediate run.sh entered the final ZIP'

# Keep launcher bytes exact. Only target-architecture programs and the
# already-pinned extractor runner are replaced inside this disposable fixture;
# canonical NXExtract behavior has its own framework suite.
cat > "$PORTS_ROOT/hitmango/nxextract/nxextract-ui" <<'UI'
#!/bin/bash
printf 'ui\n' >> "$NXZIP_MARKERS/events"
printf 'visible=host-fixture\n' > "$NXZIP_MARKERS/ui"
exit 0
UI
cat > "$PORTS_ROOT/hitmango/nxextract/run-extractor.sh" <<'EXTRACT'
#!/bin/bash
set -e
game=${NXEXTRACT_GAME_DIR:?}
if [ ! -e "$game/.host-fixture-installed" ]; then
  "$game/nxextract/nxextract-ui"
  mkdir -p \
    "$game/lib" \
    "$game/assets/bin/Data/Managed/Metadata"
  for file in \
    "$game/lib/libmain.so" \
    "$game/lib/libunity.so" \
    "$game/lib/libil2cpp.so" \
    "$game/lib/libFirebaseCppApp-12_10_1.so" \
    "$game/assets/bin/Data/boot.config" \
    "$game/assets/bin/Data/globalgamemanagers" \
    "$game/assets/bin/Data/Managed/Metadata/global-metadata.dat"; do
    printf 'fixture\n' > "$file"
  done
  : > "$game/.host-fixture-installed"
  printf '%s\n' \
    'setup UI started with host fixture' \
    'mandatory setup UI graphical renderer confirmed: host-fixture' > "$game/nxextract.log"
else
  printf '%s\n' \
    'fast validation marker accepted; no source scan needed' > "$game/nxextract.log"
fi
EXTRACT
cat > "$PORTS_ROOT/hitmango/nxsplash-nextos" <<'SPLASH'
#!/bin/bash
printf 'splash\n' >> "$NXZIP_MARKERS/events"
printf 'splash=%s\n' "$1" >> "$NXZIP_MARKERS/splash"
exit 0
SPLASH
cat > "$PORTS_ROOT/hitmango/bin/aarch64/hitmango-nextos" <<'GAME'
#!/bin/bash
printf 'game\n' >> "$NXZIP_MARKERS/events"
printf 'game_dir=%s\n' "$1" >> "$NXZIP_MARKERS/child"
exit 42
GAME
chmod 0755 \
  "$PORTS_ROOT/hitmango/nxextract/nxextract-ui" \
  "$PORTS_ROOT/hitmango/nxextract/run-extractor.sh" \
  "$PORTS_ROOT/hitmango/nxsplash-nextos" \
  "$PORTS_ROOT/hitmango/bin/aarch64/hitmango-nextos"

cat > "$NO_STAT/stat" <<NO_STAT_SENTINEL
#!/bin/bash
: > "$MARKERS/stat-called"
exit 127
NO_STAT_SENTINEL
chmod 0755 "$NO_STAT/stat"

cat > "$PM_ROOT/control.txt" <<CONTROL
directory="${TEST_ROOT#/}/roms"
controlfolder="$PM_ROOT"
CFW_NAME=hostfixture
ESUDO=""
CUR_TTY=/dev/null
sdl_controllerconfig="fixture-guid,Hitman GO Test,a:b0"
get_controls() { ANALOGSTICKS=2; }
pm_platform_helper() { printf 'helper\n' >> "$MARKERS/platform-helper"; }
pm_finish() { printf 'finish\n' >> "$MARKERS/pm-finish"; }
CONTROL

status=0
env -i PATH="$NO_STAT:$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
  XDG_DATA_HOME="$XDG_ROOT" XDG_RUNTIME_DIR="$RUNTIME_ROOT" \
  NXZIP_MARKERS="$MARKERS" \
  bash "$PORTS_ROOT/Hitman GO.sh" </dev/null || status=$?
[[ $status == 42 ]] || fail "launcher returned $status instead of child status 42"
[[ ! -e $MARKERS/stat-called ]] || fail 'an executable shell path called external stat'
[[ -s $MARKERS/ui && -s $MARKERS/child && -s $MARKERS/splash ]] ||
  fail 'setup UI, splash or child did not execute'
[[ $(paste -sd, "$MARKERS/events") == ui,splash,game ]] ||
  fail 'first-launch order differs from UI -> splash -> game'
[[ $(wc -l < "$MARKERS/pm-finish") == 1 ]] ||
  fail 'pm_finish did not run exactly once'
grep -Fq 'mandatory setup UI graphical renderer confirmed: host-fixture' \
  "$PORTS_ROOT/hitmango/nxextract.log" || fail 'setup UI proof is absent'
grep -Fq 'nxsplash 0.1.2: mandatory handoff complete' \
  "$PORTS_ROOT/hitmango/log.txt" || fail 'runtime log lacks splash handoff'
grep -Fq '== end (status 42) ==' "$PORTS_ROOT/hitmango/log.txt" ||
  fail 'runtime log lacks truthful child status'

: > "$MARKERS/events"
status=0
env -i PATH="$NO_STAT:$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
  XDG_DATA_HOME="$XDG_ROOT" XDG_RUNTIME_DIR="$RUNTIME_ROOT" \
  NXZIP_MARKERS="$MARKERS" \
  bash "$PORTS_ROOT/Hitman GO.sh" </dev/null || status=$?
[[ $status == 42 ]] || fail "second launcher run returned $status"
[[ $(paste -sd, "$MARKERS/events") == splash,game ]] ||
  fail 'second-launch order differs from splash -> game'
[[ $(wc -l < "$MARKERS/splash") == 2 ]] ||
  fail 'mandatory splash did not execute on both launches'
[[ $(wc -l < "$MARKERS/pm-finish") == 2 ]] ||
  fail 'pm_finish did not run exactly once on each normal launch'
grep -Fq 'fast validation marker accepted; no source scan needed' \
  "$PORTS_ROOT/hitmango/nxextract.log" || fail 'fast marker path was not used'

# Fail before the normal log opens and prove exclusive 0600 evidence.
EARLY_ROOT="$TEST_ROOT/early"
EARLY_XDG="$TEST_ROOT/early-xdg"
mkdir -p -- "$EARLY_ROOT" "$EARLY_XDG/PortMaster"
cp -- "$PORTS_ROOT/Hitman GO.sh" "$EARLY_ROOT/Hitman GO.sh"
cat > "$EARLY_XDG/PortMaster/control.txt" <<EARLY_CONTROL
directory="${TEST_ROOT#/}/missing"
CFW_NAME=earlyfixture
ESUDO=""
CUR_TTY=/dev/null
pm_finish() { printf 'finish\n' >> "$MARKERS/early-pm-finish"; }
EARLY_CONTROL
status=0
env -i PATH="$NO_STAT:$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
  XDG_DATA_HOME="$EARLY_XDG" \
  bash "$EARLY_ROOT/Hitman GO.sh" </dev/null >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] || fail "pre-runtime failure returned $status"
python3 -B - "$EARLY_ROOT" <<'PY'
import pathlib
import stat
import sys

root = pathlib.Path(sys.argv[1])
logs = list(root.glob("hitmango-launcher-error.*.log"))
if len(logs) != 1:
    raise SystemExit("pre-runtime proof count differs from one")
if stat.S_IMODE(logs[0].stat().st_mode) != 0o600:
    raise SystemExit("pre-runtime proof mode differs from 0600")
text = logs[0].read_text(encoding="utf-8")
if "nxbootstrap 0.6.14 | pre-runtime failure" not in text or "status=1 " not in text:
    raise SystemExit("pre-runtime proof lacks version or truthful status")
PY
[[ $(wc -l < "$MARKERS/early-pm-finish") == 1 ]] ||
  fail 'pre-runtime pm_finish did not run exactly once'
[[ ! -e $MARKERS/stat-called ]] || fail 'pre-runtime path called external stat'

printf '%s\n' \
  'hitmango final ZIP gate: PASS exact-launcher=1 no-stat=1 ui-order=1 splash-every-launch=2 early-log-0600=1 finish-once=3'
