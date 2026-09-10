#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Behavioral gate for V3-UPDATE-01 / V3-ROLLBACK-01: it generates REAL ports
# with tools/generate-port.py and boots the REAL generated launcher against
# the fixture CFW, then measures what a device would see in log.txt,
# events.jsonl and .nxruntime/state.json. Covered fronts: clean boot + health
# promotion, owner-file materialization/migration, hybrid old-launcher/
# new-runtime self-heal, truncated generations, stale root files, crash-loop
# rollback, and pre-health failure counting. Selection must stay clock-free.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_ROOT=$(cd -- "$TEST_DIR/.." && pwd -P)
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

command -v sha256sum >/dev/null 2>&1 || {
  printf 'nxbootstrap v3 generations test refused: sha256sum is required on the test host\n' >&2
  exit 77
}

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxbootstrap-v3gen.XXXXXX")
cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/nxbootstrap-v3gen.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'v3 generations test failed: %s\n' "$*" >&2
  if [[ -n ${LOG-} && -f $LOG ]]; then
    printf '%s\n' '--- launcher log ---' >&2
    sed -n '1,240p' "$LOG" >&2 || true
    printf '%s\n' '--- end launcher log ---' >&2
  fi
  exit 1
}

# ------------------------------------------------ static clock-free assertion
# The V3 selection must never order or choose by mtime/ctime. Cheap greps on
# the template keep a future "-nt"/"ls -t" shortcut from going green here.
TEMPLATE=$PROJECT_ROOT/templates/launcher.sh.in
for forbidden in ' -nt ' ' -ot ' 'ls -t'; do
  ! grep -Fq -- "$forbidden" "$TEMPLATE" ||
    fail "template selects by mtime: forbidden token '$forbidden' present"
done

# The launcher prefers the /opt PortMaster roots; mask every preferred root in
# this private mount namespace so the XDG fixture below is ALWAYS selected
# (same anti-vacuous-fixture recipe as test-launcher-behavior.sh).
for masked in /opt /boot /storage/roms/ports /roms/ports; do
  if [[ -d $masked ]]; then
    mount -t tmpfs -o size=64k,mode=0755 tmpfs "$masked" 2>/dev/null ||
      fail "cannot mask preferred PortMaster root $masked"
  fi
done

# ---------------------------------------------------------------- fixture CFW
REAL_ROOT=$TEST_ROOT/real
PORTS_DIR=$REAL_ROOT/roms/ports
mkdir -p "$PORTS_DIR"
PM_DIR=$TEST_ROOT/xdg/PortMaster
mkdir -p "$PM_DIR"
MARKERS=$TEST_ROOT/markers
mkdir -p "$MARKERS"
RUNTIME_DIR=$TEST_ROOT/runtime
mkdir -m 0700 "$RUNTIME_DIR"

cat > "$PM_DIR/control.txt" <<CONTROL
# v3 generations fixture control.txt
directory="${TEST_ROOT#/}/real/roms"
ESUDO=""
CUR_TTY=/dev/null
CFW_NAME=v3fix
sdl_controllerconfig=""
pm_finish() { printf 'finish\n' >> "$MARKERS/pm-finish"; }
CONTROL

# ---------------------------------------------------------------- fixture port
PORT_ID=v3gen-port
LAUNCHER_NAME="V3 Port.sh"
write_manifest() {
  # $1 = manifest path, $2 = title (the canonical-manifest field that changes
  # the generation identity between GA and GB).
  cat > "$1" <<JSON
{
  "schema_version": 2,
  "id": "$PORT_ID",
  "title": "$2",
  "launcher_name": "$LAUNCHER_NAME",
  "architecture": "aarch64",
  "executable": "v3-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "no", "version": "1.3.0"},
  "required_files": ["v3-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "language": {"default": "auto", "supported": ["en", "es"]},
  "runtime_report": "log"
}
JSON
}

GEN_A=$TEST_ROOT/genA
GEN_B=$TEST_ROOT/genB
GEN_C=$TEST_ROOT/genC
write_manifest "$TEST_ROOT/nxport-a.json" "V3 Port A"
write_manifest "$TEST_ROOT/nxport-b.json" "V3 Port B"
write_manifest "$TEST_ROOT/nxport-c.json" "V3 Port C"
python3 -B "$PROJECT_ROOT/tools/generate-port.py" "$TEST_ROOT/nxport-a.json" \
  --output "$GEN_A" >/dev/null || fail 'generator refused manifest GA'
python3 -B "$PROJECT_ROOT/tools/generate-port.py" "$TEST_ROOT/nxport-b.json" \
  --output "$GEN_B" >/dev/null || fail 'generator refused manifest GB'
python3 -B "$PROJECT_ROOT/tools/generate-port.py" "$TEST_ROOT/nxport-c.json" \
  --output "$GEN_C" >/dev/null || fail 'generator refused manifest GC'

read_generation_id() {
  local commits=("$1/$PORT_ID/.nxruntime/generations"/*/commit)
  [[ ${#commits[@]} == 1 && -f ${commits[0]} ]] ||
    fail "generated tree $1 does not carry exactly one committed generation"
  cat "${commits[0]}"
}
GA_ID=$(read_generation_id "$GEN_A")
GB_ID=$(read_generation_id "$GEN_B")
GC_ID=$(read_generation_id "$GEN_C")
[[ -n $GA_ID && -n $GB_ID && -n $GC_ID &&
   $GA_ID != "$GB_ID" && $GA_ID != "$GC_ID" && $GB_ID != "$GC_ID" ]] ||
  fail "generation identity ignores the manifest title (GA=$GA_ID GB=$GB_ID GC=$GC_ID)"
[[ $GA_ID =~ ^[0-9a-f]{64}$ && $GB_ID =~ ^[0-9a-f]{64}$ &&
   $GC_ID =~ ^[0-9a-f]{64}$ ]] ||
  fail "new generation identities are not complete SHA-256 values"
python3 -B - "$PROJECT_ROOT/tools/generate-port.py" \
  "$TEST_ROOT/nxport-a.json" "$GA_ID" <<'PY' ||
import importlib.util
import json
import sys

generator_path, manifest_path, generated_id = sys.argv[1:]
spec = importlib.util.spec_from_file_location("v3_generation_identity", generator_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with open(manifest_path, "r", encoding="utf-8") as stream:
    config = module.validate(json.load(stream))
assert module.generation_identity(config) == generated_id
saved = module.NXBOOTSTRAP_VERSION
module.NXBOOTSTRAP_VERSION = "99.99.99"
try:
    assert module.generation_identity(config) != generated_id
finally:
    module.NXBOOTSTRAP_VERSION = saved
PY
  fail 'generation identity is not bound to the framework version'

# Re-emitting the same bytes may reuse a committed generation, but a complete
# generation carrying different content under the same id is never overwritten.
GEN_IMMUTABLE=$TEST_ROOT/gen-immutable
cp -a "$GEN_A" "$GEN_IMMUTABLE"
python3 -B "$PROJECT_ROOT/tools/generate-port.py" "$TEST_ROOT/nxport-a.json" \
  --output "$GEN_IMMUTABLE" --force >/dev/null ||
  fail 'byte-identical committed generation could not be reused'
immutable_launcher="$GEN_IMMUTABLE/$PORT_ID/.nxruntime/generations/$GA_ID/files/launcher/$LAUNCHER_NAME"
printf '%s\n' collision > "$immutable_launcher"
if python3 -B "$PROJECT_ROOT/tools/generate-port.py" \
     "$TEST_ROOT/nxport-a.json" --output "$GEN_IMMUTABLE" --force \
     >/dev/null 2>&1; then
  fail 'committed generation identity collision was overwritten'
fi
[[ $(<"$immutable_launcher") == collision ]] ||
  fail 'collision refusal still modified the committed generation'

PORT_DIR=$PORTS_DIR/$PORT_ID
LAUNCHER=$PORTS_DIR/$LAUNCHER_NAME
LOG=$PORT_DIR/log.txt
EVENTS=$PORT_DIR/events.jsonl
STATE=$PORT_DIR/.nxruntime/state.json

install_game_stub() {
  cat > "$PORT_DIR/v3-loader" <<'STUB'
#!/bin/bash
if [ "${NXV3_FAIL-}" = 1 ]; then
  # These legacy strings used to promote a generation by grep. They are now
  # deliberately emitted on a failed run to prove they carry no authority.
  echo "NXHEALTH ready"
  echo "NXLOADER game READY JNI=0x10006"
  echo "Entering main loop"
  exit 1
fi
health_run=$NXBOOTSTRAP_HEALTH_RUN_ID
health_generation=$NXBOOTSTRAP_HEALTH_GENERATION
case "${NXV3_HEALTH_MODE-valid}" in
  none) exit 0 ;;
  wrong-run) health_run=replayed-run ;;
  wrong-generation) health_generation=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ;;
esac
health_tmp="$NXBOOTSTRAP_HEALTH_FILE.tmp.$$"
(umask 077; set -C; printf '%s\n' \
  "{\"schema\":\"$NXBOOTSTRAP_HEALTH_SCHEMA\",\"schema_version\":$NXBOOTSTRAP_HEALTH_SCHEMA_VERSION,\"run_id\":\"$health_run\",\"generation\":\"$health_generation\",\"port_id\":\"$NXBOOTSTRAP_HEALTH_PORT_ID\",\"status\":\"ready\"}" \
  > "$health_tmp") || exit 2
if [ "${NXV3_HEALTH_MODE-valid}" = two-lines ]; then
  printf '%s\n' extra >> "$health_tmp"
fi
mv -f "$health_tmp" "$NXBOOTSTRAP_HEALTH_FILE" || exit 2
if [ "${NXV3_HEALTH_MODE-valid}" = insecure-mode ]; then
  chmod 0644 "$NXBOOTSTRAP_HEALTH_FILE" || exit 2
fi
exit 0
STUB
  chmod 0755 "$PORT_DIR/v3-loader"
}

install_port() {
  # Fresh install of one generated tree (rule #41 style: wipe, full copy).
  local source_tree=$1
  rm -rf -- "$PORT_DIR" "$LAUNCHER"
  mkdir -p "$PORT_DIR"
  cp -a "$source_tree/$PORT_ID/." "$PORT_DIR/"
  cp -a "$source_tree/$LAUNCHER_NAME" "$LAUNCHER"
  # The generated nxsplash is a device binary; the behavior fixture stub is
  # the proven substitute (it is not a hashed generation component).
  cp "$TEST_DIR/splash-stub.sh" "$PORT_DIR/nxsplash-nextos"
  chmod 0755 "$PORT_DIR/nxsplash-nextos"
  install_game_stub
}

run_launcher() {
  : > "$MARKERS/pm-finish"
  env -i PATH="$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
    XDG_DATA_HOME="$TEST_ROOT/xdg" XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    NXV3_FAIL="${NXV3_FAIL-}" \
    NXV3_HEALTH_MODE="${NXV3_HEALTH_MODE-valid}" \
    bash "$LAUNCHER" </dev/null >/dev/null 2>&1
}

assert_finish_once() {
  local context=$1 count=0
  [[ -f $MARKERS/pm-finish ]] && count=$(wc -l < "$MARKERS/pm-finish")
  [[ $count == 1 ]] || fail "pm_finish count for $context is $count, expected 1"
}

events_run_id() {
  # Every line of the given events file must carry ONE identical run_id.
  local events_file=$1 total with_id unique
  [[ -s $events_file ]] || fail "events file $events_file is empty or absent"
  total=$(wc -l < "$events_file")
  with_id=$(grep -c '"run_id":"' "$events_file" || true)
  [[ $total == "$with_id" ]] ||
    fail "$events_file has $total lines but only $with_id carry a run_id"
  unique=$(grep -o '"run_id":"[^"]*"' "$events_file" | sort -u)
  [[ $(printf '%s\n' "$unique" | wc -l) == 1 ]] ||
    fail "$events_file mixes run ids: $unique"
  printf '%s' "${unique#*:\"}" | tr -d '"'
}

assert_no_error_updates() {
  ! grep -Eq 'UPDATE NXU000[1-5]' "$LOG" ||
    fail "$1: log carries an unexpected NXU error/heal receipt"
}

# ========== scenario 0: fresh install never bricks when an adapter omits health
install_port "$GEN_A"
mkdir -p "$PORT_DIR/gamedata"
printf 'owner extra file survives\n' > "$PORT_DIR/gamedata/extra-owner-file.txt"
cp "$PORT_DIR/gamedata/extra-owner-file.txt" "$TEST_ROOT/extra-owner-file.bytes"
NXV3_HEALTH_MODE=none
for attempt in 1 2 3; do
  status=0; run_launcher || status=$?
  [[ $status == 0 ]] || fail "scenario 0 clean boot $attempt exited $status"
  ! grep -Fq 'UPDATE NXU0009:' "$LOG" ||
    fail "scenario 0 clean boot $attempt was refused with NXU0009"
  grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
    fail "scenario 0 clean boot $attempt did not keep the only generation active"
  grep -Fq '"prehealth_failures":0' "$STATE" ||
    fail "scenario 0 clean boot $attempt retained a pre-health failure"
  cmp -s "$TEST_ROOT/extra-owner-file.bytes" \
    "$PORT_DIR/gamedata/extra-owner-file.txt" ||
    fail "scenario 0 clean boot $attempt touched an extra owner file"
done
grep -Fq "UPDATE NXU0006: generation $GA_ID proved healthy clean_exit_run=" "$LOG" ||
  fail 'scenario 0 stable clean-exit fallback lacked NXU0006 evidence'
[[ ! -e $PORT_DIR/.nxruntime/deep-verify-next ]] ||
  fail 'scenario 0 clean active boot armed an unnecessary deep verification'

# Reproduce the exact field state left by the broken 1.2.0: three failures,
# pending only, and no previous generation that could possibly be an anchor.
install_port "$GEN_A"
printf '%s\n' \
  "{\"schema\":\"nxruntime-state-v2\",\"schema_version\":2,\"active\":\"null\",\"pending\":\"$GA_ID\",\"previous_healthy\":\"null\",\"activation_seq\":1,\"prehealth_failures\":3,\"failure_generation\":\"$GA_ID\",\"last_health_run_id\":\"null\"}" \
  > "$STATE"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 0 bricked-state recovery exited $status"
grep -Fq 'UPDATE NXU0015: fresh install has no rollback anchor' "$LOG" ||
  fail 'scenario 0 bricked-state recovery lacked NXU0015 evidence'
! grep -Fq 'UPDATE NXU0009:' "$LOG" ||
  fail 'scenario 0 bricked-state recovery still emitted NXU0009'
grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
  fail 'scenario 0 bricked-state recovery did not promote the clean runtime'
NXV3_HEALTH_MODE=valid

# ================================ scenario 1+2: clean boot, health, owner files
install_port "$GEN_A"
mkdir -p "$PORT_DIR/defaults"
cat > "$PORT_DIR/defaults/NEXTOSCONTROLLERS.gptk" <<'GPTK'
format = NEXTOS_CONTROLLERS/1
[menu]
back = "SELECT"
confirm = "START"
[gameplay]
jump = "A"
attack = "B"
GPTK
cat > "$PORT_DIR/defaults/NEXTOSSETTINGS.txt" <<'SETTINGS'
# NEXTOS_SETTINGS/1
language=auto
SETTINGS

status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 1 boot 1 exited $status, expected 0"
[[ -f $LOG ]] || fail 'scenario 1: log.txt was not written'
grep -q 'cfw=v3fix' "$LOG" ||
  fail 'scenario 1: fixture control.txt was not sourced (vacuous run)'
assert_no_error_updates 'scenario 1 boot 1'
grep -Fq "UPDATE NXU0006: generation $GA_ID proved healthy" "$LOG" ||
  fail 'scenario 1: healthy generation was not promoted (NXU0006 missing)'
[[ -f $STATE ]] || fail 'scenario 1: state.json was not written'
grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
  fail "scenario 1: state.json active is not the GA generation ($(cat "$STATE"))"
grep -Fq '"prehealth_failures":0' "$STATE" ||
  fail 'scenario 1: prehealth_failures is not 0 after a healthy boot'
run_id_boot1=$(events_run_id "$EVENTS")
assert_finish_once 'scenario 1 boot 1'

# Owner materialization: both editable copies exist, 0644, identical bytes.
for owner_file in NEXTOSCONTROLLERS.gptk NEXTOSSETTINGS.txt; do
  [[ -f $PORT_DIR/$owner_file ]] ||
    fail "scenario 2: $owner_file was not materialized"
  [[ $(stat -c '%a' "$PORT_DIR/$owner_file") == 644 ]] ||
    fail "scenario 2: $owner_file is not 0644"
  cmp -s "$PORT_DIR/defaults/$owner_file" "$PORT_DIR/$owner_file" ||
    fail "scenario 2: materialized $owner_file differs from its default"
done
grep -Fq 'OWNER FILE: materialized NEXTOSCONTROLLERS.gptk' "$LOG" ||
  fail 'scenario 2: gptk materialization receipt missing'

# Second boot: non-reusable run id.
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 1 boot 2 exited $status"
run_id_boot2=$(events_run_id "$EVENTS")
run_id_prev=$(events_run_id "$PORT_DIR/events.prev.jsonl")
[[ $run_id_prev == "$run_id_boot1" ]] ||
  fail 'scenario 1: rotated events.prev.jsonl lost the first boot run id'
[[ $run_id_boot1 != "$run_id_boot2" ]] ||
  fail "scenario 1: run id was reused across boots ($run_id_boot1)"

# Edit the owner's gptk copy (swap the two action names): never overwritten.
sed -i 's/^jump = "A"$/jump = "B"/; s/^attack = "B"$/attack = "A"/' \
  "$PORT_DIR/NEXTOSCONTROLLERS.gptk"
cp "$PORT_DIR/NEXTOSCONTROLLERS.gptk" "$TEST_ROOT/gptk-edited.bytes"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 2 boot after edit exited $status"
cmp -s "$TEST_ROOT/gptk-edited.bytes" "$PORT_DIR/NEXTOSCONTROLLERS.gptk" ||
  fail 'scenario 2: the edited gptk copy was overwritten'

# Change BOTH defaults: customized gptk gets a side-by-side .new candidate,
# the untouched settings copy migrates automatically with a receipt.
printf 'menu_open = "L1+R1"\n' >> "$PORT_DIR/defaults/NEXTOSCONTROLLERS.gptk"
cat > "$PORT_DIR/defaults/NEXTOSSETTINGS.txt" <<'SETTINGS'
# NEXTOS_SETTINGS/1
language=auto
vibration=on
SETTINGS
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 2 default-change boot exited $status"
[[ -f $PORT_DIR/NEXTOSCONTROLLERS.gptk.new ]] ||
  fail 'scenario 2: customized gptk did not receive a .new candidate'
cmp -s "$PORT_DIR/defaults/NEXTOSCONTROLLERS.gptk" \
  "$PORT_DIR/NEXTOSCONTROLLERS.gptk.new" ||
  fail 'scenario 2: gptk .new candidate is not the new default bytes'
cmp -s "$TEST_ROOT/gptk-edited.bytes" "$PORT_DIR/NEXTOSCONTROLLERS.gptk" ||
  fail 'scenario 2: default change clobbered the edited gptk copy'
grep -Fq 'NEXTOSSETTINGS.txt was untouched; migrated to the new default' "$LOG" ||
  fail 'scenario 2: untouched settings copy did not log its migration'
cmp -s "$PORT_DIR/defaults/NEXTOSSETTINGS.txt" "$PORT_DIR/NEXTOSSETTINGS.txt" ||
  fail 'scenario 2: untouched settings copy did not migrate to the new default'

# ==================== scenario 3: hybrid old launcher / new runtime, self-heal
# Field shape: the owner re-extracted the NEW port folder but the OLD visible
# .sh survived (FAT copy refused, or they kept the old entry). Owner data was
# already in place and must come out untouched.
mkdir -p "$PORT_DIR/gamedata"
printf 'owner save data sentinel\n' > "$PORT_DIR/gamedata/sentinel.txt"
cp "$PORT_DIR/gamedata/sentinel.txt" "$TEST_ROOT/sentinel.bytes"
# Member-by-member install of GB's <port-id>/ over the live port dir: each
# member the update SHIPS replaces the installed one; members it does not ship
# (gamedata, defaults, owner copies, the game binary) are left alone.
for member in "$GEN_B/$PORT_ID"/.[!.]* "$GEN_B/$PORT_ID"/*; do
  [[ -e $member ]] || continue
  rm -rf -- "$PORT_DIR/${member##*/}"
  cp -a "$member" "$PORT_DIR/"
done
cp "$TEST_DIR/splash-stub.sh" "$PORT_DIR/nxsplash-nextos"
chmod 0755 "$PORT_DIR/nxsplash-nextos"
cmp -s "$LAUNCHER" "$GEN_A/$LAUNCHER_NAME" ||
  fail 'scenario 3 setup: the stale top-level launcher is not GA'
# V3-UPDATE-01 anti-symlink: a root component that was replaced by a SYMLINK
# (e.g. pointing outside the port) must be healed by REPLACING it with the real
# file, never by reading/writing THROUGH the link.
rm -f "$PORT_DIR/nxport.json"
ln -s /dev/null "$PORT_DIR/nxport.json"

status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 3 hybrid run exited $status"
grep -Fq "UPDATE NXU0001: mixed installation: launcher carries $GA_ID, runtime generation is $GB_ID" "$LOG" ||
  fail 'scenario 3: mixed installation was a silent menu-return (no NXU0001)'
grep -Eq "UPDATE NXU0003: healing launcher/" "$LOG" ||
  fail 'scenario 3: stale launcher was not healed (no NXU0003 line)'
cmp -s "$LAUNCHER" "$GEN_B/$LAUNCHER_NAME" ||
  fail 'scenario 3: top-level launcher was not healed to GB for the next boot'
[[ ! -L "$PORT_DIR/nxport.json" ]] ||
  fail 'scenario 3: symlinked nxport.json was followed, not replaced'
cmp -s "$PORT_DIR/nxport.json" "$GEN_B/$PORT_ID/nxport.json" ||
  fail 'scenario 3: installed nxport.json is not the GB manifest'
cmp -s "$TEST_ROOT/sentinel.bytes" "$PORT_DIR/gamedata/sentinel.txt" ||
  fail 'scenario 3: gamedata sentinel was touched by the self-heal'
cmp -s "$TEST_ROOT/gptk-edited.bytes" "$PORT_DIR/NEXTOSCONTROLLERS.gptk" ||
  fail 'scenario 3: edited controllers copy was touched by the self-heal'
assert_finish_once 'scenario 3 hybrid run'

# Second (healed) run boots clean.
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 3 healed run exited $status"
! grep -q 'UPDATE NXU0001' "$LOG" ||
  fail 'scenario 3: healed installation still reports a mixed installation'
grep -Fq "\"active\":\"$GB_ID\"" "$STATE" ||
  fail 'scenario 3: GB generation is not active after the healed run'

# ============================== scenario 4: interrupted/truncated generation
# The GA launcher comes back (an interrupted update wrote the entry point but
# its own generation lost the commit marker); state still points at complete GB.
cp -a "$GEN_A/$PORT_ID/.nxruntime/generations/$GA_ID" \
  "$PORT_DIR/.nxruntime/generations/$GA_ID"
cp "$GEN_A/$LAUNCHER_NAME" "$LAUNCHER"
GA_GEN_DIR=$PORT_DIR/.nxruntime/generations/$GA_ID
cp "$GA_GEN_DIR/commit" "$TEST_ROOT/ga-commit.saved"
rm "$GA_GEN_DIR/commit"
grep -Fq "\"active\":\"$GB_ID\"" "$STATE" ||
  fail 'scenario 4 setup: state active is not the complete GB generation'

status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 4 truncated-generation run exited $status"
grep -Fq "UPDATE NXU0002: own generation $GA_ID incomplete; running last good $GB_ID" "$LOG" ||
  fail 'scenario 4: truncated generation was not named in an NXU0002 receipt'
[[ -d $GA_GEN_DIR && -f $GA_GEN_DIR/components.sha256 &&
   -d $GA_GEN_DIR/files ]] ||
  fail 'scenario 4: the truncated generation was deleted or gutted'
grep -Fq "UPDATE NXU0006: generation $GB_ID proved healthy" "$LOG" ||
  fail 'scenario 4: the complete generation did not run to health'
# Repair the interrupted generation for the rollback scenario below.
cp "$TEST_ROOT/ga-commit.saved" "$GA_GEN_DIR/commit"

# ==================================== scenario 5: stale isolated root file
printf '\n{"stale": "garbage appended by a broken copy"}\n' >> "$PORT_DIR/nxport.json"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 5 stale-root run exited $status"
grep -Fq 'UPDATE NXU0003: healing nxport.json' "$LOG" ||
  fail 'scenario 5: corrupted nxport.json was not healed (no NXU0003)'
cmp -s "$PORT_DIR/nxport.json" \
  "$PORT_DIR/.nxruntime/generations/$GB_ID/files/nxport.json" ||
  fail 'scenario 5: nxport.json is not byte-identical to the generation copy'

# ======================================== scenario 6: crash-loop rollback
# Two complete generations; GA remains active while GB is pending with three
# consecutive pre-health failures: the NEXT boot must select GA.
cmp -s "$LAUNCHER" "$GEN_B/$LAUNCHER_NAME" ||
  fail 'scenario 6 setup: top-level launcher is not GB'
printf '%s\n' \
  "{\"schema\":\"nxruntime-state-v2\",\"schema_version\":2,\"active\":\"$GA_ID\",\"pending\":\"$GB_ID\",\"previous_healthy\":\"$GA_ID\",\"activation_seq\":7,\"prehealth_failures\":3,\"failure_generation\":\"$GB_ID\",\"last_health_run_id\":\"prior-run\"}" \
  > "$STATE"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 6 rollback run exited $status"
grep -Fq "UPDATE NXU0004: crash-loop: previous healthy generation $GA_ID selected" "$LOG" ||
  fail 'scenario 6: crash-loop rollback did not select the previous healthy generation'
assert_finish_once 'scenario 6 rollback run'
grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
  fail 'scenario 6: rolled-back generation was not recorded as active'

# =============== scenario 7: run-bound receipt + pre-health failure counting
grep -Fq '"prehealth_failures":0' "$STATE" ||
  fail 'scenario 7 setup: state is not healthy before the failure run'
# Reinstall only GB's visible launcher: GA stays active and GB begins a new
# pending trial. The failing game prints every old readiness string, proving
# that log grep can no longer promote it.
cp "$GEN_B/$LAUNCHER_NAME" "$LAUNCHER"
NXV3_FAIL=1
status=0; run_launcher || status=$?
NXV3_FAIL=''
[[ $status == 1 ]] || fail "scenario 7 failing stub returned $status, expected 1"
grep -Eq 'UPDATE NXU0005: no valid run-bound health receipt .*prehealth_failures=1' "$LOG" ||
  fail 'scenario 7: pre-health failure was not counted in an NXU0005 receipt'
grep -Fq '"prehealth_failures":1' "$STATE" ||
  fail 'scenario 7: state.json did not increment prehealth_failures'

# A well-formed receipt copied from another run and a world-readable receipt
# are both rejected even when the child exits zero.
NXV3_HEALTH_MODE=wrong-run
status=0; run_launcher || status=$?
NXV3_HEALTH_MODE=valid
[[ $status == 0 ]] || fail "scenario 7 wrong-run receipt exited $status"
grep -Fq '"prehealth_failures":2' "$STATE" ||
  fail 'scenario 7: replayed run receipt was accepted'
NXV3_HEALTH_MODE=insecure-mode
status=0; run_launcher || status=$?
NXV3_HEALTH_MODE=valid
[[ $status == 0 ]] || fail "scenario 7 insecure receipt exited $status"
grep -Fq '"prehealth_failures":3' "$STATE" ||
  fail 'scenario 7: insecure-mode receipt was accepted'

# The next boot rolls back to GA before the pending GB child can run.
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 7 rollback run exited $status"
grep -Fq "UPDATE NXU0004: crash-loop: previous healthy generation $GA_ID selected" "$LOG" ||
  fail 'scenario 7: three pending failures did not trigger rollback'
grep -Fq "\"active\":\"$GA_ID\"" "$STATE" ||
  fail 'scenario 7: rollback did not preserve GA as active'

# A fresh GB trial with the exact current receipt promotes normally.
cp "$GEN_B/$LAUNCHER_NAME" "$LAUNCHER"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 7 recovered GB trial exited $status"
grep -q 'UPDATE NXU0006: ' "$LOG" ||
  fail 'scenario 7: valid current receipt did not promote the pending build'
grep -Fq "\"active\":\"$GB_ID\"" "$STATE" ||
  fail 'scenario 7: valid GB receipt did not make GB active'
grep -Fq '"prehealth_failures":0' "$STATE" ||
  fail 'scenario 7: healthy run did not reset prehealth_failures to 0'

# ================= scenario 8: a newer explicit install supersedes old pending
# An interrupted GB trial must not trap a subsequently installed GC behind the
# stale pending id. The launcher actually invoked is the authoritative install
# intent, while the healthy GA remains the rollback anchor until GC proves.
cp -a "$GEN_C/$PORT_ID/.nxruntime/generations/$GC_ID" \
  "$PORT_DIR/.nxruntime/generations/$GC_ID"
cp "$GEN_C/$LAUNCHER_NAME" "$LAUNCHER"
printf '%s\n' \
  "{\"schema\":\"nxruntime-state-v2\",\"schema_version\":2,\"active\":\"$GA_ID\",\"pending\":\"$GB_ID\",\"previous_healthy\":\"$GA_ID\",\"activation_seq\":9,\"prehealth_failures\":1,\"failure_generation\":\"$GB_ID\",\"last_health_run_id\":\"old-trial\"}" \
  > "$STATE"
status=0; run_launcher || status=$?
[[ $status == 0 ]] || fail "scenario 8 newer generation run exited $status"
grep -Fq "\"active\":\"$GC_ID\"" "$STATE" ||
  fail 'scenario 8: stale pending generation overrode the newly installed launcher'
grep -Fq "\"previous_healthy\":\"$GA_ID\"" "$STATE" ||
  fail 'scenario 8: newer generation lost the healthy rollback anchor'

printf 'nxbootstrap v3 generations gate passed: scenarios=8 hybrid_self_heal=1 rollback=1 owner_preserved=1 clock_free=1 run_bound_health=1 full_sha256=1 framework_bound=1 immutable_collision=1\n'
