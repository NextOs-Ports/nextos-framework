#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Executable P06 gate: every declared boundary is truthful, ordered and durable.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_ROOT=$(cd -- "$TEST_DIR/.." && pwd -P)
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxbootstrap-phases.XXXXXX")
cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/nxbootstrap-phases.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'nxbootstrap phase gate failed: %s\n' "$*" >&2
  [[ -n ${LOG-} && -f $LOG ]] && sed -n '1,260p' "$LOG" >&2 || true
  exit 1
}

wait_for_file() {
  local path=$1 attempt
  for ((attempt = 0; attempt < 100; ++attempt)); do
    [[ -e $path ]] && return 0
    sleep 0.1
  done
  fail "timed out waiting for $path"
}

# Seal higher-priority host PortMaster roots inside the already-private mount
# namespace so this test can never source a developer workstation integration.
if [[ -d /opt ]]; then
  mount -t tmpfs -o size=64k,mode=0755 tmpfs /opt 2>/dev/null ||
    fail 'could not isolate /opt'
fi

ROM_ROOT=$TEST_ROOT/roms
PORTS=$ROM_ROOT/ports
XDG=$TEST_ROOT/xdg
MARKERS=$TEST_ROOT/markers
mkdir -p "$PORTS" "$XDG/PortMaster" "$MARKERS"
cat > "$XDG/PortMaster/control.txt" <<CONTROL
directory="${ROM_ROOT#/}"
ESUDO=""
CUR_TTY=/dev/null
CFW_NAME=phasefix
sdl_controllerconfig=""
pm_finish() { printf 'finish\n' >> "$MARKERS/pm-finish"; }
CONTROL

cat > "$TEST_ROOT/nxport.json" <<'JSON'
{
  "schema_version": 2,
  "id": "phase-port",
  "title": "Phase Port",
  "launcher_name": "Phase Port.sh",
  "architecture": "aarch64",
  "executable": "phase-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "yes", "version": "1.3.0"},
  "required_files": ["phase-loader", "data/ready.bin"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log"
}
JSON
python3 -B "$PROJECT_ROOT/tools/generate-port.py" "$TEST_ROOT/nxport.json" \
  --output "$PORTS" >/dev/null
GAME=$PORTS/phase-port
LAUNCHER=$PORTS/Phase\ Port.sh
LOG=$GAME/log.txt
cp "$TEST_DIR/splash-stub.sh" "$GAME/nxsplash-nextos"
chmod 0755 "$GAME/nxsplash-nextos"
mkdir -p "$GAME/nxextract"
printf '{"schema":1}\n' > "$GAME/extractor.json"
printf '# fixture core\n' > "$GAME/nxextract/nxextract.py"
cat > "$GAME/nxextract/nxextract-runtime-env.sh" <<'SH'
#!/usr/bin/env bash
exec "$@"
SH
cat > "$GAME/nxextract/nxextract-ui" <<'SH'
#!/usr/bin/env bash
exit 0
SH
cat > "$GAME/nxextract/run-extractor.sh" <<'SH'
#!/usr/bin/env bash
set -eu
mode=${NXPHASE_EXTRACT_MODE:-success}
case $mode in
  missing) exit 0 ;;
  malformed)
    printf '{"schema":' > "$NXEXTRACT_GAME_DIR/nxextract-result.json"
    exit 0
    ;;
  duplicate)
    printf '{"schema":"org.nextos.nxextract.terminal-result","schema":"duplicate"}\n' \
      > "$NXEXTRACT_GAME_DIR/nxextract-result.json"
    exit 0
    ;;
esac
rm -f "$NXEXTRACT_GAME_DIR/nxextract-result.alias.json" \
  "$NXEXTRACT_GAME_DIR/nxextract-result.outside.json"
mkdir -p "$NXEXTRACT_GAME_DIR/data"
printf 'ready\n' > "$NXEXTRACT_GAME_DIR/data/ready.bin"
python3 -B - "$NXEXTRACT_GAME_DIR/nxextract-result.json" "$mode" <<'PY'
import json
import sys

path, mode = sys.argv[1:]
success = mode in ("success", "success-error", "hardlink", "symlink")
result = {
    "schema": "org.nextos.nxextract.terminal-result",
    "schema_version": 1,
    "nxextract_version": "1.2.12",
    "outcome": "success" if success else "error",
    "code": "NXE0000" if success else "NXE3001",
    "final_phase": ({"index": 8, "id": "ready", "label": "Ready"}
                    if success else
                    {"index": 3, "id": "selecting", "label": "Selecting"}),
    "recipe": {"id": "phase-port", "version": "fixture-1",
               "digest": "0" * 64},
    "package_id": "org.nextos.phase",
    "abi": "arm64-v8a",
    "container": {"kind": "existing", "identity": "1" * 64},
    "validated": {"items": 1 if success else 0,
                  "bytes": 6 if success else 0,
                  "critical_payloads": []},
    "logs": {"summary": "nxextract.log", "detail": "nxextract-detail.log"},
    "duration_ms": 1,
    "completed_unix": 1,
    "ui": {"mode": "visible", "renderer": "sdl", "fallback_reason": None},
    "error": None if success else
             {"class": "FixtureError", "message": "controlled failure"},
}
with open(path, "w", encoding="utf-8") as stream:
    json.dump(result, stream, sort_keys=True, separators=(",", ":"))
    stream.write("\n")
PY
case $mode in
  symlink)
    mv "$NXEXTRACT_GAME_DIR/nxextract-result.json" \
      "$NXEXTRACT_GAME_DIR/nxextract-result.outside.json"
    ln -s nxextract-result.outside.json \
      "$NXEXTRACT_GAME_DIR/nxextract-result.json"
    ;;
  hardlink)
    ln "$NXEXTRACT_GAME_DIR/nxextract-result.json" \
      "$NXEXTRACT_GAME_DIR/nxextract-result.alias.json"
    ;;
esac
case $mode in error|success-error) exit 33 ;; esac
exit 0
SH
chmod 0755 "$GAME/nxextract/run-extractor.sh" \
  "$GAME/nxextract/nxextract-runtime-env.sh" "$GAME/nxextract/nxextract-ui"

cat > "$GAME/phase-loader" <<'SH'
#!/usr/bin/env bash
set -eu

publish_observation() {
  local phase=$1 reason=$2 sequence=$3
  python3 -B - "$NXOBS_PHASE_FILE" "$NXOBS_BOOTSTRAP_VERSION" \
    "$NXOBS_RUN_ID" "$phase" "$reason" "$sequence" <<'PY'
import json
import os
import sys

path, version, run_id, phase, reason, sequence = sys.argv[1:]
payload = {
    "schema": "org.nextos.nxbootstrap.phase-result",
    "schema_version": 1,
    "nxbootstrap_version": version,
    "sequence": int(sequence),
    "run_id": run_id,
    "source": "graphics",
    "phase": phase,
    "boundary": "OBSERVED",
    "status": "observed",
    "reason_code": int(reason),
    "child_status": None,
    "pid": os.getpid(),
}
temporary = path + ".adapter.%s" % os.getpid()
descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(descriptor, "w", encoding="ascii") as stream:
    json.dump(payload, stream, sort_keys=True, separators=(",", ":"))
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())
os.replace(temporary, path)
PY
  printf 'PHASE %s OBSERVED\n' "$phase"
  printf 'NXEVENT {"schema":"nx-event-v1","source":"graphics","phase":"%s","status":"observed","reason_code":%s,"details":{}}\n' \
    "$phase" "$reason"
}

mode=${NXPHASE_RUNTIME_MODE:-full}
case $mode in
  pre-video) exit 41 ;;
esac
publish_observation video-provider 6210 100
if [[ $mode == hold-provider ]]; then
  : > "${NXPHASE_MARKER:?}/provider-ready"
  while [[ ! -e ${NXPHASE_MARKER}/release ]]; do sleep 0.05; done
  exit 42
fi
publish_observation first-frame 6211 101
if [[ $mode == hold-first-frame ]]; then
  : > "${NXPHASE_MARKER:?}/first-frame-ready"
  while [[ ! -e ${NXPHASE_MARKER}/release ]]; do sleep 0.05; done
  exit 43
fi
# A full run proves health exactly like a real port: the run-bound receipt
# promotes the pending generation to active.  Without it every later
# deliberate cut below would count as a pre-health failure of a generation
# that never proved itself, and nxbootstrap 0.7.1+ correctly refuses the
# fourth attempt of such a generation when no rollback anchor exists.
health_tmp="$NXBOOTSTRAP_HEALTH_FILE.tmp.$$"
(umask 077; set -C; printf '%s\n' \
  "{\"schema\":\"$NXBOOTSTRAP_HEALTH_SCHEMA\",\"schema_version\":$NXBOOTSTRAP_HEALTH_SCHEMA_VERSION,\"run_id\":\"$NXBOOTSTRAP_HEALTH_RUN_ID\",\"generation\":\"$NXBOOTSTRAP_HEALTH_GENERATION\",\"port_id\":\"$NXBOOTSTRAP_HEALTH_PORT_ID\",\"status\":\"ready\"}" \
  > "$health_tmp") || exit 44
mv -f -- "$health_tmp" "$NXBOOTSTRAP_HEALTH_FILE" || exit 44
exit 0
SH
chmod 0755 "$GAME/phase-loader"

run_launcher() {
  env -i PATH="$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
    XDG_DATA_HOME="$XDG" XDG_RUNTIME_DIR="$TEST_ROOT/runtime" \
    NXPHASE_EXTRACT_MODE="${NXPHASE_EXTRACT_MODE:-success}" \
    NXPHASE_RUNTIME_MODE="${NXPHASE_RUNTIME_MODE:-full}" \
    NXPHASE_MARKER="$MARKERS" \
    NXBOOTSTRAP_TEST_SPLASH_STATUS="${NXPHASE_SPLASH_STATUS:-0}" \
    bash "$LAUNCHER" </dev/null >/dev/null 2>&1
}
mkdir -m 0700 "$TEST_ROOT/runtime"

assert_latest() {
  local phase=$1 boundary=$2 child=${3:-null}
  python3 - "$GAME/nxphase-result.json" "$phase" "$boundary" "$child" <<'PY' ||
import json
import sys

path, phase, boundary, child = sys.argv[1:]
event = json.load(open(path, encoding="utf-8"))
assert event["schema"] == "org.nextos.nxbootstrap.phase-result"
assert event["schema_version"] == 1
assert event["phase"] == phase
assert event["boundary"] == boundary
assert event["child_status"] == (None if child == "null" else int(child))
PY
    fail "latest phase is not $phase/$boundary/$child"
}

# Full sequence: summary copy and every boundary remain in native order.
NXPHASE_EXTRACT_MODE=success NXPHASE_RUNTIME_MODE=full NXPHASE_SPLASH_STATUS=0
run_launcher || fail 'full phase sequence failed'
python3 - "$LOG" <<'PY' || fail 'phase boundaries are absent or reordered'
import sys

lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
needles = (
    "PHASE nxextract START",
    "NXEXTRACT_RESULT ",
    "PHASE nxextract OK",
    "PHASE nxsplash START",
    "PHASE nxsplash OK",
    "PHASE runtime START",
    "PHASE video-provider OBSERVED",
    "PHASE first-frame OBSERVED",
    "PHASE runtime EXIT status=0",
)
positions = []
for needle in needles:
    positions.append(next(index for index, line in enumerate(lines)
                          if line.startswith(needle)))
assert positions == sorted(positions) and len(set(positions)) == len(positions)
PY
assert_latest runtime EXIT 0
shopt -s nullglob
phase_temps=("$GAME"/.nxphase-result.json.tmp.*)
shopt -u nullglob
[[ ${#phase_temps[@]} == 0 ]] || fail 'atomic phase temporaries remain'

# Cut inside NXExtract: a valid terminal error preserves status and blocks UI.
NXPHASE_EXTRACT_MODE=error NXPHASE_RUNTIME_MODE=full
status=0; run_launcher || status=$?
[[ $status == 33 ]] || fail "NXExtract error status became $status"
assert_latest nxextract ERROR 33
! grep -Fq 'PHASE nxsplash START' "$LOG" ||
  fail 'nxsplash ran after NXExtract failed'

# Missing, malformed, duplicate, status-mismatched and unsafe terminal evidence
# fails closed; a nonzero real runner status remains authoritative.
for NXPHASE_EXTRACT_MODE in missing malformed duplicate error-zero hardlink symlink; do
  status=0; run_launcher || status=$?
  [[ $status == 1 ]] || fail "$NXPHASE_EXTRACT_MODE result returned $status"
  assert_latest nxextract ERROR 0
  rm -f "$GAME/nxextract-result.json" "$GAME/nxextract-result.alias.json" \
    "$GAME/nxextract-result.outside.json"
done
NXPHASE_EXTRACT_MODE=success-error
status=0; run_launcher || status=$?
[[ $status == 33 ]] || fail "status-mismatched nonzero result returned $status"
assert_latest nxextract ERROR 33

# Cut inside NXSplash: its truthful status is terminal and runtime never starts.
NXPHASE_EXTRACT_MODE=success NXPHASE_SPLASH_STATUS=37
status=0; run_launcher || status=$?
[[ $status == 37 ]] || fail "nxsplash status became $status"
assert_latest nxsplash ERROR 37
! grep -Fq 'PHASE runtime START' "$LOG" ||
  fail 'runtime started after nxsplash failed'
NXPHASE_SPLASH_STATUS=0

# Cut after runtime START but before provider selection.
NXPHASE_RUNTIME_MODE=pre-video
status=0; run_launcher || status=$?
[[ $status == 41 ]] || fail "pre-provider runtime status became $status"
assert_latest runtime EXIT 41
! grep -Fq 'PHASE video-provider OBSERVED' "$LOG" ||
  fail 'runtime fabricated a video provider'

# Cut between provider and first frame. Inspect the atomically published child
# boundary while the child is still alive, then allow the truthful exit.
rm -f "$MARKERS/provider-ready" "$MARKERS/release"
NXPHASE_RUNTIME_MODE=hold-provider run_launcher & launcher_pid=$!
wait_for_file "$MARKERS/provider-ready"
assert_latest video-provider OBSERVED
: > "$MARKERS/release"
status=0; wait "$launcher_pid" || status=$?
[[ $status == 42 ]] || fail "post-provider runtime status became $status"
assert_latest runtime EXIT 42
! grep -Fq 'PHASE first-frame OBSERVED' "$LOG" ||
  fail 'runtime fabricated a first frame'

# Cut after first frame proves that boundary before the launcher records EXIT.
rm -f "$MARKERS/first-frame-ready" "$MARKERS/release"
NXPHASE_RUNTIME_MODE=hold-first-frame run_launcher & launcher_pid=$!
wait_for_file "$MARKERS/first-frame-ready"
assert_latest first-frame OBSERVED
: > "$MARKERS/release"
status=0; wait "$launcher_pid" || status=$?
[[ $status == 43 ]] || fail "post-first-frame runtime status became $status"
assert_latest runtime EXIT 43

printf 'nxbootstrap phase observability gate passed: schema=1 atomic=1 summary-copy=1 boundaries=9 cuts=7 result-negatives=8 truthful-status=all visual-flow-unchanged=1\n'
