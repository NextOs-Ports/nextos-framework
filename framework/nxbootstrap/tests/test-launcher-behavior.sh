#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Behavioral gate for the generated 0.6.16 launcher: it LAUNCHES the launcher
# and measures what a device would see (log, exit status, lock, signals,
# physical GAMEDIR, mapping contract). String checks live in test-generator.sh;
# this file exists so those strings can never go green on their own.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_ROOT=$(cd -- "$TEST_DIR/.." && pwd -P)
VERSION=$(<"$PROJECT_ROOT/VERSION")
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxbootstrap-behavior.XXXXXX")
cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/nxbootstrap-behavior.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'launcher behavior test failed: %s\n' "$*" >&2
  if [[ -n ${LOG-} && -f $LOG ]]; then
    printf '%s\n' '--- launcher log ---' >&2
    sed -n '1,240p' "$LOG" >&2 || true
    printf '%s\n' '--- end launcher log ---' >&2
  fi
  exit 1
}

wait_for_file() {
  local file=$1 attempt
  for ((attempt = 0; attempt < 100; ++attempt)); do
    [[ -e $file ]] && return 0
    sleep 0.1
  done
  fail "timed out waiting for marker $file"
}

wait_for_process_exit() {
  local pid=$1 attempt process_stat process_rest process_state
  for ((attempt = 0; attempt < 160; ++attempt)); do
    if ! kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
    if IFS= read -r process_stat < "/proc/$pid/stat" 2>/dev/null; then
      process_rest=${process_stat##*) }
      process_state=${process_rest%% *}
      [[ $process_state == Z || $process_state == X ]] && return 0
    fi
    sleep 0.1
  done
  return 1
}

live_process_starttime() {
  local pid=$1 process_stat process_rest process_state
  IFS= read -r process_stat < "/proc/$pid/stat" 2>/dev/null || return 1
  process_rest=${process_stat##*) }
  set -- $process_rest
  [[ $# -ge 20 ]] || return 1
  process_state=$1
  [[ $process_state != Z && $process_state != X ]] || return 1
  printf '%s\n' "${20}"
}

wait_for_ignored_signals() {
  local pid=$1 attempt ignored
  for ((attempt = 0; attempt < 100; ++attempt)); do
    ignored=$(awk '/^SigIgn:/ {print $2}' "/proc/$pid/status" 2>/dev/null || true)
    if [[ $ignored =~ ^[0-9A-Fa-f]+$ ]] &&
       (( (16#$ignored & 16#4003) == 16#4003 )); then
      return 0
    fi
    sleep 0.1
  done
  fail "timed out waiting for launcher $pid to ignore HUP/INT/TERM"
}

assert_finish_once() {
  local context=$1 count=0
  [[ -f $MARKERS/pm-finish ]] && count=$(wc -l < "$MARKERS/pm-finish")
  [[ $count == 1 ]] || fail "pm_finish count for $context is $count, expected 1"
}

# The launcher prefers the /opt PortMaster roots; on a host that really has
# one installed the fixture below would never be sourced and every assertion
# would test the wrong control.txt. Mask them inside this private mount
# namespace so the fixture is ALWAYS the selected root (this also fixes the
# vacuous-fixture hole found in review).
for masked in /opt /boot /storage/roms/ports /roms/ports; do
  if [[ -d $masked ]]; then
    mount -t tmpfs -o size=64k,mode=0755 tmpfs "$masked" 2>/dev/null ||
      fail "cannot mask preferred PortMaster root $masked"
  fi
done
# A higher-priority directory without control.txt must not hide the valid XDG
# fixture below. This was the control-discovery bug seen on standalone layouts.
mkdir -p /opt/system/Tools/PortMaster
mkdir -p "/opt/system/Advanced"
: > "/boot/arkos4clone-uboot.dtb"
: > "/opt/system/Advanced/Backup dArkOS Settings.sh"

# ---------------------------------------------------------------- fixture CFW
# ROM tree reached through a SYMLINK so the physical-GAMEDIR guarantee is
# actually exercised (the /roms -> /storage/roms case caught on real NextOS).
REAL_ROOT=$TEST_ROOT/real
ln -s "$REAL_ROOT" "$TEST_ROOT/link"
mkdir -p "$REAL_ROOT/roms/ports"
# TMPDIR itself may sit behind a symlink; assertions compare physical paths.
REAL_PHYS=$(cd "$REAL_ROOT" && pwd -P)
PM_DIR=$TEST_ROOT/xdg/PortMaster
mkdir -p "$PM_DIR/libs" "$PM_DIR/libs.aarch64"
MARKERS=$TEST_ROOT/markers
mkdir -p "$MARKERS"
REAL_READLINK=$(command -v readlink)
WRAPPED_BIN=$TEST_ROOT/wrapped-bin
mkdir -p "$WRAPPED_BIN"
cat > "$WRAPPED_BIN/readlink" <<WRAPPED_READLINK
#!/bin/bash
printf 'args=%s bin_preload=%s ld_preload=%s sdl_dynamic_api=%s\n' \
  "\$*" "\${BIN_PRELOAD-__unset__}" "\${LD_PRELOAD-__unset__}" \
  "\${SDL_DYNAMIC_API-__unset__}" >> "$MARKERS/post-hook-provider-env"
exec "$REAL_READLINK" "\$@"
WRAPPED_READLINK
chmod 0755 "$WRAPPED_BIN/readlink"
RUNTIME_DIR=$TEST_ROOT/runtime
mkdir -m 0700 "$RUNTIME_DIR"
LOCK_FILE="$RUNTIME_DIR/.nxbootstrap-${UID:-$(id -u)}/nxport-behav-port.flock"
FALLBACK_LOCK_DIR="$LOCK_FILE.d"

cat > "$PM_DIR/control.txt" <<CONTROL
# behavioral fixture control.txt
directory="${TEST_ROOT#/}/link/roms"
ESUDO=""
CUR_TTY=/dev/null
CFW_NAME=behavfix
sdl_controllerconfig="\${NXBEHAV_MAPPING-}"
get_controls() {
  if [ "\${NXBEHAV_STICK_ALIAS-}" = 1 ]; then
    unset ANALOGSTICKS
    ANALOG_STICKS="\${NXBEHAV_ANALOG_STICKS-}"
  else
    unset ANALOG_STICKS
    ANALOGSTICKS="\${NXBEHAV_ANALOG_STICKS-}"
  fi
  if [ -n "\${NXBEHAV_EARLY_MARKER-}" ]; then
    : > "\$NXBEHAV_EARLY_MARKER"
    while :; do sleep 1; done
  fi
}
if [ -n "\${NXBEHAV_DIALOG_PIPE-}" ]; then
  PM_PIPE=\$NXBEHAV_DIALOG_PIPE
else
  unset PM_PIPE
fi
pm_platform_helper() {
  printf '%s\n' "\$1" >> "$MARKERS/platform-helper"
  printf '%s\n' "\${LD_LIBRARY_PATH-}" >> "$MARKERS/helper-ldpath"
  printf 'ld_preload=%s\n' "\${LD_PRELOAD-__unset__}" \
    >> "$MARKERS/helper-provider-env"
  printf 'sdl_dynamic_api=%s\n' "\${SDL_DYNAMIC_API-__unset__}" \
    >> "$MARKERS/helper-provider-env"
  if [ "\${NXBEHAV_DIALOG_MODE-}" = helper-closes ]; then
    rm -f -- "\$PM_PIPE"
  fi
}
if [ "\${NXBEHAV_DIALOG_MODE-}" != missing-api ]; then
  PortMasterDialogExit() {
    printf 'close\n' >> "$MARKERS/dialog-close"
    case "\${NXBEHAV_DIALOG_MODE-}" in
      close) rm -f -- "\$PM_PIPE" ;;
      failed-close) return 19 ;;
      persistent) return 0 ;;
      *) return 23 ;;
    esac
  }
fi
pm_finish() { printf 'finish\n' >> "$MARKERS/pm-finish"; }
CONTROL

# ---------------------------------------------------------------- fixture port
cat > "$TEST_ROOT/nxport.json" <<'JSON'
{
  "schema_version": 2,
  "id": "behav-port",
  "title": "Behav Port",
  "launcher_name": "Behav Port.sh",
  "architecture": "aarch64",
  "executable": "bin/behav-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "no", "version": "1.3.0"},
  "required_files": ["bin/behav-loader"],
  "private_library_paths": [],
  "sdl_provider": "system",
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "language": {"default": "auto", "supported": ["en", "es"]},
  "runtime_report": "log"
}
JSON
python3 -B "$PROJECT_ROOT/tools/generate-port.py" "$TEST_ROOT/nxport.json" \
  --output "$TEST_ROOT/generated" >/dev/null ||
  fail 'generator refused the behavioral fixture manifest'

PORTS_DIR=$REAL_ROOT/roms/ports
cp "$TEST_ROOT/generated/Behav Port.sh" "$PORTS_DIR/"
mkdir -p "$PORTS_DIR/behav-port/bin"
cp "$TEST_ROOT/generated/behav-port/nxport.json" "$PORTS_DIR/behav-port/"
cp "$PROJECT_ROOT/tests/splash-stub.sh" \
  "$PORTS_DIR/behav-port/nxsplash-nextos"
chmod 0755 "$PORTS_DIR/behav-port/nxsplash-nextos"
cat > "$PORTS_DIR/behav-port/port-env.sh" <<'STUB'
NXINPUT_ANALOG_STICKS_HINT=9
export NXINPUT_ANALOG_STICKS_HINT
NXBOOTSTRAP_ANALOG_STICKS_HINT=2
case "${NXBEHAV_PROVIDER_OVERRIDE-}" in
  bin)
    BIN_PRELOAD="$GAMEDIR/hidden/renamed-provider.so"
    ;;
  bin-safe)
    BIN_PRELOAD="$GAMEDIR/hidden/non-sdl-preload.so"
    export BIN_PRELOAD
    ;;
  ld)
    LD_PRELOAD="$GAMEDIR/hidden/libSDL2-2.0.so.disguised"
    export LD_PRELOAD
    ;;
  ld-safe)
    LD_PRELOAD="$GAMEDIR/hidden/non-sdl-preload.so${LD_PRELOAD:+:$LD_PRELOAD}"
    export LD_PRELOAD
    ;;
  dynamic)
    SDL_DYNAMIC_API="$GAMEDIR/hidden/arbitrary-name.so"
    export SDL_DYNAMIC_API
    ;;
  addon-*)
    BIN_PRELOAD="$GAMEDIR/hidden/renamed-${NXBEHAV_PROVIDER_OVERRIDE#addon-}.so"
    ;;
  bin-unresolved)
    BIN_PRELOAD="$GAMEDIR/hidden/missing-bin-preload.so"
    ;;
  ld-unresolved)
    LD_PRELOAD="$GAMEDIR/hidden/missing-ld-preload.so"
    export LD_PRELOAD
    ;;
  ld-stale-system)
    LD_PRELOAD='/usr/$LIB/libSDL2-2.0.so.0.3000.10'
    export LD_PRELOAD
    ;;
  dynamic-unresolved)
    SDL_DYNAMIC_API="$GAMEDIR/hidden/missing-dynamic-api.so"
    export SDL_DYNAMIC_API
    ;;
  origin-addon)
    BIN_PRELOAD='$ORIGIN/hidden/origin-provider.so'
    ;;
  readonly-provider)
    LD_PRELOAD="$GAMEDIR/hidden/non-sdl-preload.so"
    export LD_PRELOAD
    readonly LD_PRELOAD
    ;;
  bin-route)
    BIN="$GAMEDIR/hidden/non-sdl-preload.so"
    ;;
  interp-route)
    NXBOOTSTRAP_INTERP_PREFIX="$GAMEDIR/hidden/non-sdl-preload.so"
    ;;
  game-loader-route)
    NXBOOTSTRAP_GAME_LOADER="$GAMEDIR/hidden/non-sdl-preload.so"
    ;;
  game-libs-route)
    NXBOOTSTRAP_GAME_LIBS="$GAMEDIR/hidden"
    ;;
esac
if [ "${NXBEHAV_HEALTH_MUTATION-}" = 1 ]; then
  ( NXBOOTSTRAP_HEALTH_FILE="$GAMEDIR/tampered-health.json" ) || true
  ( unset NXBOOTSTRAP_HEALTH_SCHEMA ) || true
  ( NXBOOTSTRAP_HEALTH_SCHEMA_VERSION=99 ) || true
  ( unset NXBOOTSTRAP_HEALTH_RUN_ID ) || true
  ( NXBOOTSTRAP_HEALTH_GENERATION=tampered-generation ) || true
  ( unset NXBOOTSTRAP_HEALTH_PORT_ID ) || true
  {
    printf 'health_file=%s\n' "${NXBOOTSTRAP_HEALTH_FILE-__unset__}"
    printf 'health_schema=%s\n' "${NXBOOTSTRAP_HEALTH_SCHEMA-__unset__}"
    printf 'health_schema_version=%s\n' "${NXBOOTSTRAP_HEALTH_SCHEMA_VERSION-__unset__}"
    printf 'health_run_id=%s\n' "${NXBOOTSTRAP_HEALTH_RUN_ID-__unset__}"
    printf 'health_generation=%s\n' "${NXBOOTSTRAP_HEALTH_GENERATION-__unset__}"
    printf 'health_port_id=%s\n' "${NXBOOTSTRAP_HEALTH_PORT_ID-__unset__}"
  } > "$NXBEHAV_HEALTH_MUTATION_MARKER"
fi
STUB

cat > "$TEST_ROOT/reset-signals.py" <<'PY'
import os
import signal
import sys

for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
    signal.signal(signum, signal.SIG_DFL)
os.execvpe(sys.argv[1], sys.argv[1:], os.environ)
PY

NO_FLOCK_BASH_ENV="$TEST_ROOT/no-flock-bash-env.sh"
cat > "$NO_FLOCK_BASH_ENV" <<'BASH_ENV'
# Hide only the flock capability probe. Every other command keeps Bash's
# normal lookup, so the real generated launcher exercises its mkdir fallback.
command() {
  if [ "$#" -eq 2 ] && [ "$1" = -v ] && [ "$2" = flock ]; then
    return 1
  fi
  builtin command "$@"
}
BASH_ENV

cat > "$TEST_ROOT/term-stuck.py" <<PY
import pathlib
import signal

ready = pathlib.Path("$MARKERS/term-stuck-ready")
seen = pathlib.Path("$MARKERS/term-seen")

def on_term(_signum, _frame):
    seen.write_text("TERM\n", encoding="utf-8")

signal.signal(signal.SIGTERM, on_term)
ready.write_text("ready\n", encoding="utf-8")
while True:
    signal.pause()
PY

cat > "$PORTS_DIR/behav-port/bin/behav-loader" <<STUB
#!/bin/bash
{
  printf 'cwd=%s\n' "\$(pwd -P)"
  printf 'game_dir=%s\n' "\${NXCOMPAT_GAME_DIR-}"
  printf 'game_dir_physical=%s\n' "\${NXCOMPAT_GAME_DIR_PHYSICAL-}"
  printf 'port_id=%s\n' "\${NXCOMPAT_PORT_ID-}"
  printf 'mapping=%s\n' "\${SDL_GAMECONTROLLERCONFIG-__unset__}"
  printf 'analog_sticks=%s\n' "\${NXINPUT_ANALOG_STICKS_HINT-__unset__}"
  printf 'language=%s\n' "\${NXPORT_LANGUAGE-__unset__}"
  printf 'ld_library_path=%s\n' "\${LD_LIBRARY_PATH-__unset__}"
  printf 'sdl_provider=%s\n' "\${NXBOOTSTRAP_SDL_PROVIDER-__unset__}"
  printf 'sdl_videodriver=%s\n' "\${SDL_VIDEODRIVER-__unset__}"
  printf 'ld_preload=%s\n' "\${LD_PRELOAD-__unset__}"
  printf 'sdl_dynamic_api=%s\n' "\${SDL_DYNAMIC_API-__unset__}"
  printf 'health_file=%s\n' "\${NXBOOTSTRAP_HEALTH_FILE-__unset__}"
  printf 'health_schema=%s\n' "\${NXBOOTSTRAP_HEALTH_SCHEMA-__unset__}"
  printf 'health_schema_version=%s\n' "\${NXBOOTSTRAP_HEALTH_SCHEMA_VERSION-__unset__}"
  printf 'health_run_id=%s\n' "\${NXBOOTSTRAP_HEALTH_RUN_ID-__unset__}"
  printf 'health_generation=%s\n' "\${NXBOOTSTRAP_HEALTH_GENERATION-__unset__}"
  printf 'health_port_id=%s\n' "\${NXBOOTSTRAP_HEALTH_PORT_ID-__unset__}"
} > "$MARKERS/child-env"
printf 'child-stderr\n' >&2
case "\${NXBEHAV_STUB_MODE-}" in
  log-flood)
    printf 'FLOOD-HEAD\n'
    # 6 MiB over the 4 MiB budget, written as the records a real adapter
    # prints into this same stream, so the byte cut lands INSIDE one -- which
    # is the whole point: the survivor still starts with the NXEVENT marker
    # and no longer holds valid JSON.
    pad='xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'
    pad="\$pad\$pad\$pad\$pad\$pad\$pad\$pad\$pad"
    j=0
    while [ "\$j" -lt 12000 ]; do
      printf 'NXEVENT {"schema":"nx-event-v1","source":"graphics","phase":"context","status":"observed","reason_code":1600,"details":{"i":%s,"p":"%s"}}\n' \\
        "\$j" "\$pad"
      j=\$((j + 1))
    done
    printf 'FLOOD-TAIL\n'
    # The runtime adapter owns this file for the whole run, so it is bounded
    # by nobody unless the launcher bounds it. Past the 1 MiB budget.
    if [ -n "\${NXOBS_EVENTS_FILE-}" ]; then
      # Real nx-event-v1 records: the runtime adapter never writes a line
      # without a reason_code or a receipt code, and the support bundle is
      # right to refuse one that does.
      ev='{"schema":"nx-event-v1","schema_version":1,"run_id":"behav","source":"lifecycle","phase":"loop","status":"ok","reason_code":1800,"details":'
      printf '%s{"note":"EVENT-HEAD"}}\n' "\$ev" >> "\$NXOBS_EVENTS_FILE"
      i=0
      while [ "\$i" -lt 9000 ]; do
        printf '%s{"note":"pad","i":%s,"p":"%s"}}\n' \\
          "\$ev" "\$i" 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' \\
          >> "\$NXOBS_EVENTS_FILE"
        i=\$((i + 1))
      done
      printf '%s{"note":"EVENT-TAIL"}}\n' "\$ev" >> "\$NXOBS_EVENTS_FILE"
    fi
    ;;
  sleep) exec sleep 5 ;;
  term-immune)
    trap '' TERM
    : > "$MARKERS/term-immune-ready"
    sleep 2
    ;;
  term-127)
    trap '' TERM
    : > "$MARKERS/term-127-ready"
    sleep 2
    exit 127
    ;;
  term-default)
    : > "$MARKERS/term-default-ready"
    exec sleep 5
    ;;
  term-stuck) exec python3 "$TEST_ROOT/term-stuck.py" ;;
  daemon-fd)
    # A detached background helper that would inherit any leaked lock fd.
    sleep 30 >/dev/null 2>&1 </dev/null &
    printf "%s" "\$!" > "$MARKERS/daemon-pid"
    ;;
  video-black|video-black-term-immune)
    if [ "\${NXBEHAV_STUB_MODE-}" = video-black-term-immune ]; then
      trap '' TERM
      printf '%s\n' "\$\$" > "$MARKERS/video-kill-child-pid"
      : > "$MARKERS/video-term-immune-ready"
    else
      trap ': > "$MARKERS/video-term-seen"; exit 0' TERM
    fi
    : > "$MARKERS/audio-active"
    printf '%s\n' "\$NXBOOTSTRAP_HEALTH_FILE" > "$MARKERS/health-path"
    video_tmp="\$NXBOOTSTRAP_VIDEO_FILE.tmp.\$\$"
    health_tmp="\$NXBOOTSTRAP_HEALTH_FILE.tmp.\$\$"
    (umask 077; printf '%s\n' \
      "{\"schema\":\"org.nextos.nxruntime.health\",\"schema_version\":1,\"run_id\":\"\$NXBOOTSTRAP_HEALTH_RUN_ID\",\"generation\":\"\$NXBOOTSTRAP_HEALTH_GENERATION\",\"port_id\":\"\$NXBOOTSTRAP_HEALTH_PORT_ID\",\"status\":\"ready\"}" > "\$health_tmp")
    mv -f -- "\$health_tmp" "\$NXBOOTSTRAP_HEALTH_FILE"
    (umask 077; printf '%s\n' \
      "{\"schema\":\"org.nextos.nxruntime.video-proof\",\"schema_version\":1,\"run_id\":\"\$NXBOOTSTRAP_HEALTH_RUN_ID\",\"generation\":\"\$NXBOOTSTRAP_HEALTH_GENERATION\",\"port_id\":\"\$NXBOOTSTRAP_HEALTH_PORT_ID\",\"verdict\":\"BLACK\",\"reason\":\"black-streak\"}" > "\$video_tmp")
    mv -f -- "\$video_tmp" "\$NXBOOTSTRAP_VIDEO_FILE"
    while :; do printf 'audio\n' > /dev/null; sleep 1; done
    ;;
esac
exit 42
STUB
chmod 0755 "$PORTS_DIR/behav-port/bin/behav-loader"

LAUNCHER=$PORTS_DIR/Behav\ Port.sh
run_launcher() {
  env -i PATH="${NXBEHAV_PATH:-$PATH}" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
    BASH_ENV="${NXBEHAV_BASH_ENV-}" \
    XDG_DATA_HOME="$TEST_ROOT/xdg" \
    NXBEHAV_MAPPING="${NXBEHAV_MAPPING-}" \
    NXBEHAV_ANALOG_STICKS="${NXBEHAV_ANALOG_STICKS-}" \
    NXBEHAV_STICK_ALIAS="${NXBEHAV_STICK_ALIAS-}" \
    NXBEHAV_STUB_MODE="${NXBEHAV_STUB_MODE-}" \
    NXBEHAV_EARLY_MARKER="${NXBEHAV_EARLY_MARKER-}" \
    NXBEHAV_DIALOG_PIPE="${NXBEHAV_DIALOG_PIPE-}" \
    NXBEHAV_DIALOG_MODE="${NXBEHAV_DIALOG_MODE-}" \
    NXBEHAV_PROVIDER_OVERRIDE="${NXBEHAV_PROVIDER_OVERRIDE-}" \
    NXBEHAV_HEALTH_MUTATION="${NXBEHAV_HEALTH_MUTATION-}" \
    NXBEHAV_HEALTH_MUTATION_MARKER="${NXBEHAV_HEALTH_MUTATION_MARKER-}" \
    NXBEHAV_INHERITED_LD_PRELOAD="${NXBEHAV_INHERITED_LD_PRELOAD-}" \
    NXBEHAV_INHERITED_DYNAMIC_API="${NXBEHAV_INHERITED_DYNAMIC_API-}" \
    LD_PRELOAD="${NXBEHAV_INHERITED_LD_PRELOAD-}" \
    SDL_DYNAMIC_API="${NXBEHAV_INHERITED_DYNAMIC_API-}" \
    LD_LIBRARY_PATH="${NXBEHAV_LD_LIBRARY_PATH-}" \
    NXPORT_LANGUAGE="${NXBEHAV_LANGUAGE-}" \
    XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    bash "$LAUNCHER" </dev/null
}

# --------------------------------------- 0. pre-runtime failure is never mute
# The visible entry exists and Bash executes it, but neither the PortMaster
# directory nor the relative fallback can resolve the game tree. This failure
# happens before log.txt can be opened and must leave one owner-only diagnostic
# beside the visible launcher.
EARLY_VISIBLE=$TEST_ROOT/early-visible
EARLY_XDG=$TEST_ROOT/early-xdg
mkdir -p "$EARLY_VISIBLE" "$EARLY_XDG/PortMaster"
cp "$LAUNCHER" "$EARLY_VISIBLE/Behav Port.sh"
cat > "$EARLY_XDG/PortMaster/control.txt" <<EARLY_CONTROL
directory="${TEST_ROOT#/}/missing-rom-root"
ESUDO=""
CUR_TTY=/dev/null
CFW_NAME=earlyfix
pm_finish() { printf 'finish\n' >> "$MARKERS/early-pm-finish"; }
EARLY_CONTROL
status=0
env -i PATH="$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
  XDG_DATA_HOME="$EARLY_XDG" \
  bash "$EARLY_VISIBLE/Behav Port.sh" </dev/null >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] || fail "pre-runtime discovery failure returned $status"
shopt -s nullglob
early_logs=("$EARLY_VISIBLE"/behav-port-launcher-error.*.log)
shopt -u nullglob
[[ ${#early_logs[@]} == 1 ]] ||
  fail "pre-runtime failure produced ${#early_logs[@]} diagnostics, expected 1"
grep -Fq "nxbootstrap $VERSION | pre-runtime failure" "${early_logs[0]}" ||
  fail 'pre-runtime diagnostic lacks the bootstrap version and phase'
grep -Fq 'status=1 ' "${early_logs[0]}" ||
  fail 'pre-runtime diagnostic lacks the truthful status'
[[ $(stat -c '%a' "${early_logs[0]}") == 600 ]] ||
  fail 'pre-runtime diagnostic is not owner-only'
[[ ! -e $EARLY_VISIBLE/behav-port/log.txt ]] ||
  fail 'pre-runtime test unexpectedly reached the runtime log'
[[ $(wc -l < "$MARKERS/early-pm-finish") == 1 ]] ||
  fail 'pm_finish did not run exactly once on the pre-runtime failure'

# No PortMaster candidate at all: the trap was already live before discovery,
# so a missing fallback game tree still leaves its exclusive diagnostic.
EARLY_NONE_VISIBLE=$TEST_ROOT/early-none-visible
EARLY_NONE_XDG=$TEST_ROOT/early-none-xdg
mkdir -p "$EARLY_NONE_VISIBLE" "$EARLY_NONE_XDG"
cp "$LAUNCHER" "$EARLY_NONE_VISIBLE/Behav Port.sh"
status=0
env -i PATH="$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
  XDG_DATA_HOME="$EARLY_NONE_XDG" \
  bash "$EARLY_NONE_VISIBLE/Behav Port.sh" \
  </dev/null >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] || fail "pre-PortMaster failure returned $status"
shopt -s nullglob
early_none_logs=("$EARLY_NONE_VISIBLE"/behav-port-launcher-error.*.log)
shopt -u nullglob
[[ ${#early_none_logs[@]} == 1 ]] ||
  fail 'failure without PortMaster did not leave exactly one diagnostic'
grep -Fq "game_dir=$EARLY_NONE_VISIBLE/behav-port" "${early_none_logs[0]}" ||
  fail 'failure without PortMaster did not record its attempted game directory'
[[ $(stat -c '%a' "${early_none_logs[0]}") == 600 ]] ||
  fail 'failure without PortMaster did not preserve umask 077'

# Both game and launcher destinations are unwritable.  A read-only bind mount
# makes the condition real even under the namespace's mapped root; the trap
# must advance to the private runtime directory and retain the true status.
EARLY_RO_VISIBLE=$TEST_ROOT/early-ro-visible
EARLY_RO_XDG=$TEST_ROOT/early-ro-xdg
EARLY_RO_RUNTIME=$TEST_ROOT/early-ro-runtime
mkdir -p "$EARLY_RO_VISIBLE" "$EARLY_RO_XDG/PortMaster" "$EARLY_RO_RUNTIME"
chmod 0700 "$EARLY_RO_RUNTIME"
cp "$LAUNCHER" "$EARLY_RO_VISIBLE/Behav Port.sh"
cat > "$EARLY_RO_XDG/PortMaster/control.txt" <<EARLY_RO_CONTROL
directory="${TEST_ROOT#/}/missing-readonly-rom-root"
ESUDO=""
CUR_TTY=/dev/null
CFW_NAME=readonlyfix
pm_finish() { printf 'finish\n' >> "$MARKERS/early-ro-pm-finish"; }
EARLY_RO_CONTROL
mount --bind "$EARLY_RO_VISIBLE" "$EARLY_RO_VISIBLE" ||
  fail 'could not bind the read-only launcher fixture'
mount -o remount,bind,ro "$EARLY_RO_VISIBLE" ||
  fail 'could not make the launcher fixture read-only'
status=0
env -i PATH="$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
  XDG_DATA_HOME="$EARLY_RO_XDG" XDG_RUNTIME_DIR="$EARLY_RO_RUNTIME" \
  bash "$EARLY_RO_VISIBLE/Behav Port.sh" \
  </dev/null >/dev/null 2>&1 || status=$?
umount "$EARLY_RO_VISIBLE" || fail 'could not release read-only launcher fixture'
[[ $status == 1 ]] || fail "read-only pre-runtime failure returned $status"
shopt -s nullglob
early_ro_logs=("$EARLY_RO_RUNTIME"/behav-port-launcher-error.*.log)
shopt -u nullglob
[[ ${#early_ro_logs[@]} == 1 ]] ||
  fail 'read-only paths did not fall back to one runtime diagnostic'
grep -Fq 'status=1 ' "${early_ro_logs[0]}" ||
  fail 'runtime fallback diagnostic lost the truthful status'
[[ $(stat -c '%a' "${early_ro_logs[0]}") == 600 ]] ||
  fail 'runtime fallback diagnostic is not owner-only'
[[ $(wc -l < "$MARKERS/early-ro-pm-finish") == 1 ]] ||
  fail 'runtime fallback did not finalize PortMaster exactly once'

# ------------------------------------------------- 1. normal run, real values
NXBEHAV_MAPPING='behavguid,Behav Pad,a:b0,start:b7,back:b6'
NXBEHAV_ANALOG_STICKS=0
NXBEHAV_STICK_ALIAS=1
export NXBEHAV_MAPPING
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "true exit status was not propagated (got $status)"
LOG=$PORTS_DIR/behav-port/log.txt
[[ -f $LOG ]] || fail 'durable log.txt was not written'
grep -q 'cfw=behavfix' "$LOG" ||
  fail 'launcher did not source the fixture control.txt (vacuous run)'
grep -q 'end (status 42)' "$LOG" || fail 'log does not record the real status'
grep -q '^child-stderr$' "$LOG" || fail 'child stderr did not reach log.txt'
grep -qx "cwd=$REAL_PHYS/roms/ports/behav-port" "$MARKERS/child-env" ||
  fail 'child cwd is not the physical game directory'
# V3-UPDATE-01: the guest's lexical identity is the LOGICAL root the frontend
# invoked (the symlinked /link/roms), never the pwd -P swap. The physical root
# is delivered separately for framework-side I/O.
grep -qx "game_dir=$TEST_ROOT/link/roms/ports/behav-port" "$MARKERS/child-env" ||
  fail 'NXCOMPAT_GAME_DIR is not the LOGICAL game directory (pwd -P swap regressed)'
grep -qx "game_dir_physical=$REAL_PHYS/roms/ports/behav-port" "$MARKERS/child-env" ||
  fail 'NXCOMPAT_GAME_DIR_PHYSICAL is not the physical game directory'
grep -qx 'port_id=behav-port' "$MARKERS/child-env" ||
  fail 'child did not receive NXCOMPAT_PORT_ID'
grep -qx "mapping=$NXBEHAV_MAPPING" "$MARKERS/child-env" ||
  fail 'CFW mapping did not reach the child'
grep -qx 'analog_sticks=0' "$MARKERS/child-env" ||
  fail 'validated zero-stick PortMaster hint did not reach the child'
grep -qx 'language=auto' "$MARKERS/child-env" ||
  fail 'default opt-in language did not reach the child'
grep -qx 'sdl_provider=system' "$MARKERS/child-env" ||
  fail 'declared system SDL provider did not reach the child'
grep -qx 'sdl_videodriver=__unset__' "$MARKERS/child-env" ||
  fail 'system SDL policy forced SDL_VIDEODRIVER'
[[ $(wc -l < "$MARKERS/pm-finish") == 1 ]] ||
  fail 'pm_finish did not run exactly once on the normal path'
grep -q "behav-loader" "$MARKERS/platform-helper" ||
  fail 'pm_platform_helper did not receive the real executable'
! grep -q "behav-port" "$MARKERS/helper-ldpath" ||
  fail 'game/private library paths contaminated pm_platform_helper'

# --------------------------------- 2. empty CFW mapping must not clobber env
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
NXBEHAV_MAPPING=''
NXBEHAV_ANALOG_STICKS=invalid
unset NXBEHAV_STICK_ALIAS
NXBEHAV_LANGUAGE=es
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "empty-mapping run broke the launcher ($status)"
grep -qx 'mapping=__unset__' "$MARKERS/child-env" ||
  fail 'empty CFW mapping clobbered SDL_GAMECONTROLLERCONFIG'
grep -qx 'analog_sticks=__unset__' "$MARKERS/child-env" ||
  fail 'invalid PortMaster stick hint reached the child'
grep -qx 'language=es' "$MARKERS/child-env" ||
  fail 'valid language override did not reach the child'
assert_finish_once empty-mapping
unset NXBEHAV_LANGUAGE
unset NXBEHAV_ANALOG_STICKS

# muOS can omit stat completely.  Put a failing sentinel first in PATH: the
# launcher must validate fd/path identity with Bash -ef and never execute it.
mkdir -p "$TEST_ROOT/no-stat"
cat > "$TEST_ROOT/no-stat/stat" <<STAT_STUB
#!/bin/bash
: > "$MARKERS/stat-called"
exit 127
STAT_STUB
chmod 0755 "$TEST_ROOT/no-stat/stat"
rm -f "$MARKERS/child-env" "$MARKERS/stat-called"
: > "$MARKERS/pm-finish"
NXBEHAV_PATH="$TEST_ROOT/no-stat:$PATH"
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_PATH
[[ $status == 42 ]] ||
  fail "muOS-style environment without stat broke the launcher ($status)"
[[ ! -e $MARKERS/stat-called && -e $MARKERS/child-env ]] ||
  fail 'launcher called stat or did not launch the child'
assert_finish_once no-stat

# ---------------------- 2b. real generated launcher owns SDL/provider safety
mkdir -p "$PORTS_DIR/behav-port/lib" "$TEST_ROOT/inherited-provider"
mkdir -p "$PORTS_DIR/behav-port/hidden"
printf '%s\n' SDL_Init SDL_PollEvent SDL_CreateWindow SDL_GetVersion > \
  "$PORTS_DIR/behav-port/hidden/renamed-provider.so"
printf 'not an SDL payload\n' > \
  "$PORTS_DIR/behav-port/hidden/arbitrary-name.so"
printf 'ordinary local preload payload\n' > \
  "$PORTS_DIR/behav-port/hidden/non-sdl-preload.so"
printf 'not loaded: canonical name is enough\n' > \
  "$PORTS_DIR/behav-port/hidden/libSDL2-2.0.so.disguised"
while read -r addon_label addon_first addon_second; do
  printf '%s\n%s\n' "$addon_first" "$addon_second" > \
    "$PORTS_DIR/behav-port/hidden/renamed-$addon_label.so"
done <<'SDL_ADDON_SYMBOLS'
image IMG_Init IMG_Quit
mixer Mix_OpenAudio Mix_Quit
ttf TTF_Init TTF_Quit
net SDLNet_Init SDLNet_Quit
gfx rotozoomSurface pixelColor
gpu GPU_Init GPU_Quit
sound Sound_Init Sound_Quit
rtf RTF_Init RTF_Quit
fontcache FC_CreateFont FC_FreeFont
SDL_ADDON_SYMBOLS

# A real preload is required here: the 0.8.3 contract trusts a dynamic-loader
# token only when the current launcher process proves its unique expansion in
# /proc/$$/maps. Populate the common glibc $LIB expansions with one tiny DSO;
# the test still fails closed if this host expands the token some other way.
LDSO_TOKEN_NAME=libSDL2-2.0.so.0.3000.10
LDSO_TOKEN_EXTERNAL=$TEST_ROOT/ldso-token-external
LDSO_TOKEN_PRIVATE=$PORTS_DIR/behav-port/ldso-token-private
LDSO_TOKEN_OBJECT=$TEST_ROOT/$LDSO_TOKEN_NAME
CC_BIN=${CC:-cc}
command -v "$CC_BIN" >/dev/null 2>&1 ||
  fail 'a C compiler is required for the real loader-token fixture'
printf '%s\n' 'void nxbootstrap_ldso_token_fixture(void) {}' > \
  "$TEST_ROOT/ldso-token.c"
"$CC_BIN" -shared -fPIC -Wl,-soname,"$LDSO_TOKEN_NAME" \
  "$TEST_ROOT/ldso-token.c" -o "$LDSO_TOKEN_OBJECT" ||
  fail 'could not build the real loader-token fixture'
ldso_token_dirs=(lib lib64 lib32)
ldso_multiarch=$("$CC_BIN" -print-multiarch 2>/dev/null || true)
if [[ -n $ldso_multiarch ]]; then
  ldso_token_dirs+=("lib/$ldso_multiarch")
fi
ldso_machine=$(uname -m)
[[ -n $ldso_machine ]] && ldso_token_dirs+=("lib/$ldso_machine-linux-gnu")
for ldso_root in "$LDSO_TOKEN_EXTERNAL" "$LDSO_TOKEN_PRIVATE"; do
  for ldso_dir in "${ldso_token_dirs[@]}"; do
    mkdir -p "$ldso_root/$ldso_dir"
    cp "$LDSO_TOKEN_OBJECT" "$ldso_root/$ldso_dir/$LDSO_TOKEN_NAME"
  done
done
mkdir -p "$LDSO_TOKEN_EXTERNAL/ambiguous"
cp "$LDSO_TOKEN_OBJECT" \
  "$LDSO_TOKEN_EXTERNAL/ambiguous/$LDSO_TOKEN_NAME"

# BASH_ENV can mark an inherited value readonly before the launcher begins.
# The earliest builtin-only boundary must refuse it before defining/running any
# framework helper rather than silently continuing with a contaminated env.
cat > "$TEST_ROOT/readonly-bash-env.sh" <<READONLY_BASH_ENV
export BIN_PRELOAD="$PORTS_DIR/behav-port/hidden/non-sdl-preload.so"
readonly BIN_PRELOAD
READONLY_BASH_ENV
rm -f "$MARKERS/child-env"
rm -f "$PORTS_DIR"/behav-port-launcher-error.*.log
NXBEHAV_BASH_ENV="$TEST_ROOT/readonly-bash-env.sh"
status=0; run_launcher >"$TEST_ROOT/early-readonly.out" 2>&1 || status=$?
unset NXBEHAV_BASH_ENV
[[ $status == 125 && ! -e $MARKERS/child-env ]] ||
  fail "readonly inherited override escaped early quarantine ($status)"
grep -Fq 'cannot quarantine readonly inherited overrides' \
  "$TEST_ROOT/early-readonly.out" ||
  fail 'readonly inherited refusal lacks the builtin diagnostic'
readonly_logs=("$PORTS_DIR"/behav-port-launcher-error.*.log)
[[ ${#readonly_logs[@]} == 1 && -f ${readonly_logs[0]} && \
   ! -L ${readonly_logs[0]} ]] ||
  fail 'readonly inherited refusal did not leave one regular launcher-error log'
[[ $(stat -c '%a' "${readonly_logs[0]}") == 600 ]] ||
  fail 'readonly inherited launcher-error log is not 0600'
grep -Fq 'status=125 ' "${readonly_logs[0]}" ||
  fail 'readonly inherited launcher-error log lost the exact status'
unset readonly_logs
printf 'private SDL2 sentinel\n' > \
  "$PORTS_DIR/behav-port/lib/libSDL2-2.0.so.0"
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] ||
  fail "private SDL2 was not refused before exec (status $status)"
[[ ! -e $MARKERS/child-env ]] ||
  fail 'private SDL2 reached the child exec boundary'
grep -Fq 'private SDL1/SDL2 provider is forbidden' "$LOG" ||
  fail 'private SDL2 refusal lacks a precise diagnostic'
assert_finish_once private-sdl2
rm -f "$PORTS_DIR/behav-port/lib/libSDL2-2.0.so.0"

# Every SDL1/SDL2 add-on namespace covered by the NXRelease provider policy is
# also refused by the bounded runtime name gate. SDL3 remains separate below.
for addon_name in \
  libSDL_image.so libSDL2_mixer.so libSDL2_ttf.so libSDL2_net.so \
  libSDL2_gfx.so libSDL2_gpu.so libSDL2_sound.so libSDL2_rtf.so \
  libSDL2_FontCache.so LiBsDl2_image.SO libSDL2; do
  printf 'private SDL add-on sentinel\n' > \
    "$PORTS_DIR/behav-port/lib/$addon_name"
  rm -f "$MARKERS/child-env"
  : > "$MARKERS/pm-finish"
  status=0; run_launcher >/dev/null 2>&1 || status=$?
  [[ $status == 1 && ! -e $MARKERS/child-env ]] ||
    fail "$addon_name bypassed the runtime SDL namespace gate ($status)"
  grep -Fq 'private SDL1/SDL2 provider is forbidden' "$LOG" ||
    fail "$addon_name refusal lacks a precise diagnostic"
  assert_finish_once "private-addon-$addon_name"
  rm -f "$PORTS_DIR/behav-port/lib/$addon_name"
done

# $ORIGIN has the loader's semantics: it is relative to the physical
# executable directory, not the package root. A benign root decoy must not
# hide the renamed add-on beside a nested executable.
mkdir -p "$PORTS_DIR/behav-port/bin/hidden"
printf 'ordinary root decoy\n' > \
  "$PORTS_DIR/behav-port/hidden/origin-provider.so"
printf 'IMG_Init\nIMG_Quit\n' > \
  "$PORTS_DIR/behav-port/bin/hidden/origin-provider.so"
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
NXBEHAV_PROVIDER_OVERRIDE=origin-addon
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_PROVIDER_OVERRIDE
[[ $status == 1 && ! -e $MARKERS/child-env ]] ||
  fail "ORIGIN resolved away from the nested executable ($status)"
grep -Fq 'rejects private SDL override variable=BIN_PRELOAD' "$LOG" ||
  fail 'ORIGIN add-on refusal lacks the exact diagnostic'
assert_finish_once origin-addon

# SDL3 is intentionally outside this generic SDL1/2 prohibition. Its formal
# Godot/native-SDL3 exception and pinned hash are owned by the release gate.
printf 'private SDL3 sentinel\n' > \
  "$PORTS_DIR/behav-port/lib/libSDL3.so.0"
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 && -e $MARKERS/child-env ]] ||
  fail "eligible SDL3 was rejected by the SDL1/2 gate (status $status)"
assert_finish_once private-sdl3-eligible
rm -f "$PORTS_DIR/behav-port/lib/libSDL3.so.0"

# A frontend may inherit the previous port's private path. It must be removed,
# while a real external provider path survives and firmware precedes PM SDL.
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
NXBEHAV_LD_LIBRARY_PATH="$PORTS_DIR/behav-port/lib:$TEST_ROOT/inherited-provider"
export NXBEHAV_LD_LIBRARY_PATH
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_LD_LIBRARY_PATH
[[ $status == 42 ]] || fail "filtered inherited-path run returned $status"
child_ld=$(sed -n 's/^ld_library_path=//p' "$MARKERS/child-env")
case ":$child_ld:" in *:"$PORTS_DIR/behav-port/lib":*)
  fail 'inherited port-local library path reached the child' ;;
esac
case ":$child_ld:" in *:"$TEST_ROOT/inherited-provider":*) ;; *)
  fail 'external inherited library path was discarded' ;;
esac
python3 - "$child_ld" "$PM_DIR" <<'PY' || fail 'system SDL policy did not keep firmware ahead of PortMaster overlays'
import sys
parts = sys.argv[1].split(":")
pm = sys.argv[2]
assert parts.index("/usr/lib") < parts.index(pm + "/libs")
assert parts.index("/usr/lib") < parts.index(pm + "/libs.aarch64")
PY
assert_finish_once inherited-path-filter

# system SDL is a provider boundary, not only LD_LIBRARY_PATH ordering. The
# adapter cannot tunnel a private provider through BIN_PRELOAD, LD_PRELOAD or
# SDL_DYNAMIC_API, including a subdirectory and a content-detectable rename.
for provider_override in bin ld dynamic; do
  rm -f "$MARKERS/child-env"
  : > "$MARKERS/pm-finish"
  NXBEHAV_PROVIDER_OVERRIDE=$provider_override
  status=0; run_launcher >/dev/null 2>&1 || status=$?
  unset NXBEHAV_PROVIDER_OVERRIDE
  [[ $status == 1 ]] ||
    fail "$provider_override private SDL override returned $status"
  [[ ! -e $MARKERS/child-env ]] ||
    fail "$provider_override private SDL override reached child exec"
  case $provider_override in
    bin) expected_override=BIN_PRELOAD ;;
    ld) expected_override=LD_PRELOAD ;;
    dynamic) expected_override=SDL_DYNAMIC_API ;;
  esac
  grep -Fq "rejects private SDL override variable=$expected_override" "$LOG" ||
    fail "$provider_override private SDL override lacks exact refusal"
  ! grep -Fq 'SDL PROVIDER: declared=system' "$LOG" ||
    fail "$provider_override refusal falsely claimed a completed provider barrier"
  assert_finish_once "provider-override-$provider_override"
done

# Renaming an add-on does not erase the bounded symbol-pair fingerprints kept
# in lockstep with NXRelease (which performs the deeper defined-symbol audit).
for addon_label in image mixer ttf net gfx gpu sound rtf fontcache; do
  rm -f "$MARKERS/child-env"
  : > "$MARKERS/pm-finish"
  NXBEHAV_PROVIDER_OVERRIDE=addon-$addon_label
  status=0; run_launcher >/dev/null 2>&1 || status=$?
  unset NXBEHAV_PROVIDER_OVERRIDE
  [[ $status == 1 && ! -e $MARKERS/child-env ]] ||
    fail "renamed $addon_label add-on reached child exec ($status)"
  grep -Fq 'rejects private SDL override variable=BIN_PRELOAD' "$LOG" ||
    fail "renamed $addon_label add-on lacks an exact refusal"
  assert_finish_once "renamed-addon-$addon_label"
done

# An explicit value which the final game search path cannot resolve is not an
# unchecked maybe-provider: every supported override variable fails closed.
for provider_override in bin-unresolved ld-unresolved dynamic-unresolved; do
  rm -f "$MARKERS/child-env"
  : > "$MARKERS/pm-finish"
  NXBEHAV_PROVIDER_OVERRIDE=$provider_override
  status=0; run_launcher >/dev/null 2>&1 || status=$?
  unset NXBEHAV_PROVIDER_OVERRIDE
  [[ $status == 1 && ! -e $MARKERS/child-env ]] ||
    fail "$provider_override unresolved value reached child exec ($status)"
  case $provider_override in
    bin-*) expected_override=BIN_PRELOAD ;;
    ld-*) expected_override=LD_PRELOAD ;;
    dynamic-*) expected_override=SDL_DYNAMIC_API ;;
  esac
  grep -Fq "rejects unresolved override variable=$expected_override" "$LOG" ||
    fail "$provider_override lacks fail-closed unresolved diagnostic"
  assert_finish_once "$provider_override"
done

# system provider mode freezes the manifest executable and its resolved loader
# route. Only the explicitly audited preload surfaces remain configurable.
for provider_override in bin-route interp-route game-loader-route \
    game-libs-route; do
  rm -f "$MARKERS/child-env"
  : > "$MARKERS/pm-finish"
  NXBEHAV_PROVIDER_OVERRIDE=$provider_override
  status=0; run_launcher >/dev/null 2>&1 || status=$?
  unset NXBEHAV_PROVIDER_OVERRIDE
  [[ $status == 1 && ! -e $MARKERS/child-env ]] ||
    fail "$provider_override changed the executable route ($status)"
  grep -Fq 'sdl_provider=system rejects adapter' "$LOG" ||
    fail "$provider_override lacks an executable-route refusal"
  assert_finish_once "$provider_override"
done

# The closed provider boundary must not erase unrelated adapter behavior.
# Both preload inputs survive validation and reach only the exact game child.
for provider_override in bin-safe ld-safe; do
  rm -f "$MARKERS/child-env"
  : > "$MARKERS/pm-finish"
  NXBEHAV_PROVIDER_OVERRIDE=$provider_override
  status=0; run_launcher >/dev/null 2>&1 || status=$?
  unset NXBEHAV_PROVIDER_OVERRIDE
  [[ $status == 42 ]] ||
    fail "$provider_override non-SDL preload returned $status"
  grep -Fxq \
    "ld_preload=$PORTS_DIR/behav-port/hidden/non-sdl-preload.so" \
    "$MARKERS/child-env" ||
    fail "$provider_override non-SDL preload did not reach the child"
  grep -Fq 'SDL PROVIDER: declared=system' "$LOG" ||
    fail "$provider_override non-SDL preload did not complete provider guard"
  ! grep -Fq 'rejects private SDL override' "$LOG" ||
    fail "$provider_override non-SDL preload was falsely rejected"
  assert_finish_once "provider-override-$provider_override"
done

# BIN_PRELOAD exported by the adapter is configuration for the exact game,
# never ambient state for readlink/grep used by the post-hook guard.
rm -f "$MARKERS/child-env" "$MARKERS/post-hook-provider-env"
: > "$MARKERS/pm-finish"
NXBEHAV_PROVIDER_OVERRIDE=bin-safe
NXBEHAV_PATH="$WRAPPED_BIN:$PATH"
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_PROVIDER_OVERRIDE NXBEHAV_PATH
[[ $status == 42 ]] || fail "exported BIN_PRELOAD quarantine returned $status"
grep -Fq \
  "args=-f -- $PORTS_DIR/behav-port/hidden/non-sdl-preload.so bin_preload=__unset__ ld_preload=__unset__ sdl_dynamic_api=__unset__" \
  "$MARKERS/post-hook-provider-env" ||
  fail 'post-hook resolver inherited an adapter provider override'
assert_finish_once exported-bin-preload-capture

# Inherited loader overrides are captured by builtins before the first helper;
# adapter LD_PRELOAD is recaptured immediately after the hook. Both accepted
# values reach only the exact child, adapter first, while SDL_DYNAMIC_API uses
# the adapter value when present and otherwise the inherited external value.
printf 'ordinary inherited preload payload\n' > \
  "$TEST_ROOT/inherited-provider/non-sdl-inherited.so"
printf 'external dynamic API payload\n' > \
  "$TEST_ROOT/inherited-provider/external-dynamic-api.so"
printf 'IMG_Init\nIMG_Quit\n' > \
  "$TEST_ROOT/inherited-provider/external-renamed-sdl.so"
rm -f "$MARKERS/child-env" "$MARKERS/helper-provider-env"
: > "$MARKERS/pm-finish"
NXBEHAV_INHERITED_LD_PRELOAD="$TEST_ROOT/inherited-provider/non-sdl-inherited.so"
NXBEHAV_PROVIDER_OVERRIDE=ld-safe
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_INHERITED_LD_PRELOAD NXBEHAV_PROVIDER_OVERRIDE
[[ $status == 42 ]] || fail "captured inherited/adapter overrides returned $status"
grep -Fxq \
  "ld_preload=$PORTS_DIR/behav-port/hidden/non-sdl-preload.so:$TEST_ROOT/inherited-provider/non-sdl-inherited.so" \
  "$MARKERS/child-env" || fail 'child did not receive accepted preloads in canonical order'
grep -Fxq 'sdl_dynamic_api=__unset__' "$MARKERS/child-env" ||
  fail 'child received an undeclared SDL_DYNAMIC_API'
grep -Fxq 'ld_preload=__unset__' "$MARKERS/helper-provider-env" ||
  fail 'framework helper inherited LD_PRELOAD after the early capture'
grep -Fxq 'sdl_dynamic_api=__unset__' "$MARKERS/helper-provider-env" ||
  fail 'framework helper inherited SDL_DYNAMIC_API after the early capture'
assert_finish_once inherited-adapter-capture

# A resolved override outside the package belongs to firmware/external policy.
# It remains valid but reaches only the exact child, never framework helpers.
rm -f "$MARKERS/child-env" "$MARKERS/helper-provider-env"
: > "$MARKERS/pm-finish"
NXBEHAV_INHERITED_DYNAMIC_API="$TEST_ROOT/inherited-provider/external-dynamic-api.so"
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_INHERITED_DYNAMIC_API
[[ $status == 42 && -e $MARKERS/child-env ]] ||
  fail "external SDL_DYNAMIC_API did not reach the exact child ($status)"
grep -Fxq \
  "sdl_dynamic_api=$TEST_ROOT/inherited-provider/external-dynamic-api.so" \
  "$MARKERS/child-env" ||
  fail 'child lost the resolved external SDL_DYNAMIC_API'
grep -Fxq 'ld_preload=__unset__' "$MARKERS/helper-provider-env" ||
  fail 'helper inherited LD_PRELOAD during external dynamic API run'
grep -Fxq 'sdl_dynamic_api=__unset__' "$MARKERS/helper-provider-env" ||
  fail 'helper inherited external SDL_DYNAMIC_API'
assert_finish_once external-dynamic-api

# Even an SDL-looking resolved external LD_PRELOAD stays external authority;
# the runtime package-private gate does not relabel firmware bytes as bundled.
rm -f "$MARKERS/child-env" "$MARKERS/helper-provider-env"
: > "$MARKERS/pm-finish"
NXBEHAV_INHERITED_LD_PRELOAD="$TEST_ROOT/inherited-provider/external-renamed-sdl.so"
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_INHERITED_LD_PRELOAD
[[ $status == 42 && -e $MARKERS/child-env ]] ||
  fail "external renamed SDL did not reach the exact child ($status)"
grep -Fxq \
  "ld_preload=$TEST_ROOT/inherited-provider/external-renamed-sdl.so" \
  "$MARKERS/child-env" ||
  fail 'child lost the resolved external SDL-looking preload'
grep -Fxq 'ld_preload=__unset__' "$MARKERS/helper-provider-env" ||
  fail 'helper inherited external SDL-looking preload'
grep -Fxq 'sdl_dynamic_api=__unset__' "$MARKERS/helper-provider-env" ||
  fail 'helper gained SDL_DYNAMIC_API during external preload run'
assert_finish_once external-renamed-sdl

# ArkOS/PortMaster inherits this exact LD_PRELOAD shape. $LIB and ${LIB} are
# glibc dynamic-string tokens, not unresolved shell variables. The launcher
# must prove the already loaded external expansion, keep helpers quarantined,
# and pass the original literal (not a guessed lib/lib64 path) to the game.
for ldso_token in '$LIB' '${LIB}'; do
  ldso_token_entry="$LDSO_TOKEN_EXTERNAL/$ldso_token/$LDSO_TOKEN_NAME"
  rm -f "$MARKERS/child-env" "$MARKERS/helper-provider-env"
  : > "$MARKERS/pm-finish"
  NXBEHAV_INHERITED_LD_PRELOAD=$ldso_token_entry
  status=0; run_launcher >"$TEST_ROOT/ldso-token.out" 2>&1 || status=$?
  unset NXBEHAV_INHERITED_LD_PRELOAD
  [[ $status == 42 && -e $MARKERS/child-env ]] ||
    fail "inherited $ldso_token loader token was not admitted ($status)"
  grep -Fxq "ld_preload=$ldso_token_entry" "$MARKERS/child-env" ||
    fail "child lost the literal $ldso_token loader token"
  grep -Fxq 'ld_preload=__unset__' "$MARKERS/helper-provider-env" ||
    fail "helper inherited the $ldso_token loader token"
  ! grep -Fq 'rejects unresolved override variable=LD_PRELOAD' "$LOG" ||
    fail "mapped $ldso_token loader token was treated as unresolved"
  assert_finish_once "ldso-token-$ldso_token"
done

# Field regression, ArkOS/dArkOSRE K36S (2026-09-05): PortMaster exported the
# old 0.3000.10 filename while the firmware provided only 0.3200.x. ld.so
# ignored it, so the launcher had no mapping to prove. The system-provider
# boundary must discard this exact class of stale inherited SDL preload and
# run the child without LD_PRELOAD. BASH_ENV places the value after Bash has
# started, deterministically reproducing the observed "not mapped" state
# without depending on which SDL version the host has installed.
for stale_token in '$LIB' '${LIB}'; do
  stale_system_entry="/usr/$stale_token/$LDSO_TOKEN_NAME"
  printf "export LD_PRELOAD='%s'\nunset BASH_ENV\n" "$stale_system_entry" > \
    "$TEST_ROOT/stale-system-sdl-bash-env.sh"
  rm -f "$MARKERS/child-env" "$MARKERS/helper-provider-env"
  : > "$MARKERS/pm-finish"
  NXBEHAV_BASH_ENV="$TEST_ROOT/stale-system-sdl-bash-env.sh"
  status=0; run_launcher >"$TEST_ROOT/stale-system-$stale_token.out" 2>&1 || status=$?
  unset NXBEHAV_BASH_ENV
  [[ $status == 42 && -e $MARKERS/child-env ]] ||
    fail "stale inherited system SDL $stale_token was not discarded ($status)"
  grep -Fxq 'ld_preload=__unset__' "$MARKERS/child-env" ||
    fail "stale inherited system SDL $stale_token reached the child"
  grep -Fxq 'ld_preload=__unset__' "$MARKERS/helper-provider-env" ||
    fail "helper inherited stale system SDL $stale_token"
  grep -Fq \
    "discarded stale inherited system SDL preload entry=$stale_system_entry" \
    "$LOG" || fail "stale system SDL $stale_token lacks its discard receipt"
  ! grep -Fq 'rejects unresolved override variable=LD_PRELOAD' "$LOG" ||
    fail "stale inherited system SDL $stale_token was still rejected"
  assert_finish_once "stale-system-sdl-$stale_token"
done

# The exception belongs only to ambient inherited state. An adapter-authored
# stale system path remains an unresolved explicit override and fails closed.
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
NXBEHAV_PROVIDER_OVERRIDE=ld-stale-system
status=0; run_launcher >"$TEST_ROOT/stale-system-adapter.out" 2>&1 || status=$?
unset NXBEHAV_PROVIDER_OVERRIDE
[[ $status == 1 && ! -e $MARKERS/child-env ]] ||
  fail "adapter-authored stale system SDL reached child exec ($status)"
grep -Fq 'rejects unresolved override variable=LD_PRELOAD' "$LOG" ||
  fail 'adapter-authored stale system SDL lacks fail-closed diagnostics'
assert_finish_once stale-system-sdl-adapter

# A missing non-SDL object below the same system token is not eligible for
# sanitation. This prevents the field exception becoming a generic allowlist.
printf "%s\n%s\n" \
  "export LD_PRELOAD='/usr/\$LIB/libnxbootstrap-missing.so'" \
  'unset BASH_ENV' > \
  "$TEST_ROOT/stale-nonsdl-bash-env.sh"
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
NXBEHAV_BASH_ENV="$TEST_ROOT/stale-nonsdl-bash-env.sh"
status=0; run_launcher >"$TEST_ROOT/stale-nonsdl.out" 2>&1 || status=$?
unset NXBEHAV_BASH_ENV
[[ $status == 1 && ! -e $MARKERS/child-env ]] ||
  fail "missing inherited non-SDL system object reached child exec ($status)"
grep -Fq 'rejects unresolved override variable=LD_PRELOAD' "$LOG" ||
  fail 'missing inherited non-SDL system object lacks exact refusal'
assert_finish_once stale-system-nonsdl

# Mapping proves identity, not package authority. The same basename reached
# through a $LIB expansion below GAMEDIR remains a forbidden private SDL.
ldso_private_entry="$LDSO_TOKEN_PRIVATE/\$LIB/$LDSO_TOKEN_NAME"
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
NXBEHAV_INHERITED_LD_PRELOAD=$ldso_private_entry
status=0; run_launcher >"$TEST_ROOT/ldso-private.out" 2>&1 || status=$?
unset NXBEHAV_INHERITED_LD_PRELOAD
[[ $status == 1 && ! -e $MARKERS/child-env ]] ||
  fail "package-private loader token reached child exec ($status)"
grep -Fq 'rejects private SDL override variable=LD_PRELOAD' "$LOG" ||
  fail 'package-private loader token lacks the exact refusal'
assert_finish_once ldso-token-private

# A token which the loader did not map is still unresolved. Two distinct
# mapped objects matching one token are ambiguous and fail the same boundary.
for ldso_negative in missing ambiguous; do
  case $ldso_negative in
    missing)
      ldso_negative_entry="$TEST_ROOT/ldso-token-missing/\$LIB/$LDSO_TOKEN_NAME"
      ;;
    ambiguous)
      ldso_negative_entry="$LDSO_TOKEN_EXTERNAL/\$LIB/$LDSO_TOKEN_NAME:$LDSO_TOKEN_EXTERNAL/ambiguous/$LDSO_TOKEN_NAME"
      ;;
  esac
  rm -f "$MARKERS/child-env"
  : > "$MARKERS/pm-finish"
  NXBEHAV_INHERITED_LD_PRELOAD=$ldso_negative_entry
  status=0; run_launcher >"$TEST_ROOT/ldso-$ldso_negative.out" 2>&1 || status=$?
  unset NXBEHAV_INHERITED_LD_PRELOAD
  [[ $status == 1 && ! -e $MARKERS/child-env ]] ||
    fail "$ldso_negative loader token reached child exec ($status)"
  grep -Fq 'rejects unresolved override variable=LD_PRELOAD' "$LOG" ||
    fail "$ldso_negative loader token lacks fail-closed diagnostics"
  assert_finish_once "ldso-token-$ldso_negative"
done

# An adapter cannot defeat quarantine by making an exported override readonly.
# De-export removes the environment attribute even though readonly keeps the
# shell variable, so normal finish/lock cleanup remains uncontaminated.
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
NXBEHAV_BASH_ENV=$NO_FLOCK_BASH_ENV
NXBEHAV_PROVIDER_OVERRIDE=readonly-provider
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_PROVIDER_OVERRIDE
[[ $status == 1 && ! -e $MARKERS/child-env ]] ||
  fail "readonly provider override escaped fail-closed quarantine ($status)"
grep -Fq 'rejects readonly adapter override' "$LOG" ||
  fail 'readonly provider refusal lacks the exact diagnostic'
assert_finish_once readonly-provider
[[ ! -e $FALLBACK_LOCK_DIR ]] ||
  fail 'readonly provider refusal orphaned the no-flock fallback lock'

# The immediately following run must acquire/release that same fallback lock.
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_BASH_ENV
[[ $status == 42 && -e $MARKERS/child-env ]] ||
  fail "fallback lock was not reusable after readonly refusal ($status)"
[[ ! -e $FALLBACK_LOCK_DIR ]] ||
  fail 'recovery run did not release the no-flock fallback lock'
assert_finish_once readonly-provider-recovery

# All health tuple authorities are immutable before the mutable port-env hook.
# Assign and unset attempts must fail while the exact values reach the child.
rm -f "$MARKERS/child-env" "$MARKERS/health-after-mutation"
: > "$MARKERS/pm-finish"
NXBEHAV_HEALTH_MUTATION=1
NXBEHAV_HEALTH_MUTATION_MARKER="$MARKERS/health-after-mutation"
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_HEALTH_MUTATION NXBEHAV_HEALTH_MUTATION_MARKER
[[ $status == 42 ]] || fail "readonly health mutation run returned $status"
grep '^health_' "$MARKERS/child-env" > "$MARKERS/health-child"
cmp -s "$MARKERS/health-after-mutation" "$MARKERS/health-child" ||
  fail 'port-env changed a readonly health tuple member'
! grep -Eq 'tampered|__unset__|schema_version=99' \
  "$MARKERS/health-after-mutation" ||
  fail 'readonly health tuple accepted assign/unset mutation'
grep -Fq 'health_schema=org.nextos.nxruntime.health' \
  "$MARKERS/health-after-mutation" || fail 'health schema was not preserved'
grep -Fq 'health_port_id=behav-port' "$MARKERS/health-after-mutation" ||
  fail 'health port id was not preserved'
grep -Fq 'readonly variable' "$LOG" ||
  fail 'health assignment mutation did not fail as readonly'
grep -Fq 'cannot unset: readonly variable' "$LOG" ||
  fail 'health unset mutation did not fail as readonly'
assert_finish_once readonly-health-tuple

# A conclusive run-bound BLACK receipt is fatal even while the child remains
# alive and produces audio. A simultaneously published health-ready receipt
# must be removed before promotion can observe it.
rm -f "$MARKERS/child-env" "$MARKERS/audio-active" \
  "$MARKERS/video-term-seen" "$MARKERS/health-path"
: > "$MARKERS/pm-finish"
NXBEHAV_STUB_MODE=video-black
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_STUB_MODE
[[ $status == 72 ]] || fail "conclusive BLACK returned $status, expected 72"
[[ -e $MARKERS/audio-active && -e $MARKERS/video-term-seen ]] ||
  fail 'live/audio-producing black child was not terminated by exact PID'
health_path=$(<"$MARKERS/health-path")
[[ ! -e $health_path && ! -L $health_path ]] ||
  fail 'BLACK receipt left a competing health-ready receipt'
grep -Fq 'RUNTIME NXR0004: conclusive video failure verdict=BLACK reason=black-streak' "$LOG" ||
  fail 'BLACK termination lacks its run-bound runtime event'
grep -Fq 'PHASE runtime EXIT status=72' "$LOG" ||
  fail 'BLACK termination did not publish the fatal runtime status'
assert_finish_once video-black-fatal

# A BLACK child that ignores TERM must reach the bounded KILL path. Every KILL
# revalidates PID/starttime immediately; an unrelated live sentinel must keep
# the same identity and the exact game child must be reaped.
sleep 30 &
video_sentinel_pid=$!
video_sentinel_start=$(live_process_starttime "$video_sentinel_pid") ||
  fail 'video KILL sentinel did not start'
rm -f "$MARKERS/child-env" "$MARKERS/video-kill-child-pid" \
  "$MARKERS/video-term-immune-ready"
: > "$MARKERS/pm-finish"
NXBEHAV_STUB_MODE=video-black-term-immune
status=0; run_launcher >/dev/null 2>&1 || status=$?
unset NXBEHAV_STUB_MODE
[[ $status == 72 ]] ||
  fail "TERM-immune BLACK returned $status, expected 72"
video_killed_pid=$(<"$MARKERS/video-kill-child-pid")
[[ ! -d /proc/$video_killed_pid ]] ||
  fail 'TERM-immune BLACK child survived exact KILL'
[[ $(live_process_starttime "$video_sentinel_pid") == \
   "$video_sentinel_start" ]] ||
  fail 'BLACK KILL touched or replaced the unrelated sentinel'
kill -TERM "$video_sentinel_pid" 2>/dev/null || true
wait "$video_sentinel_pid" 2>/dev/null || true
grep -Fq 'termination deadline expired; sending KILL' "$LOG" ||
  fail 'TERM-immune BLACK did not exercise the KILL boundary'
grep -Fq 'PHASE runtime EXIT status=72' "$LOG" ||
  fail 'TERM-immune BLACK lost fatal status 72'
assert_finish_once video-black-exact-kill

# ------------------- 3. live PortMaster dialog handoff is state-based and safe
# Some CFW modules replace pm_platform_helper without closing pugwash. Prove the
# generated launcher closes only PortMaster's exact live FIFO, launches only
# after disappearance, and fails closed on every unprovable ownership/result.
DIALOG_PIPE=$TEST_ROOT/portmaster-dialog.fifo
: > "$MARKERS/pm-finish"
: > "$MARKERS/platform-helper"
rm -f "$MARKERS/dialog-close" "$MARKERS/child-env" "$DIALOG_PIPE"
mkfifo "$DIALOG_PIPE"
NXBEHAV_DIALOG_PIPE=$DIALOG_PIPE
NXBEHAV_DIALOG_MODE=close
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "valid PortMaster dialog handoff returned $status"
[[ ! -e $DIALOG_PIPE && -e $MARKERS/child-env ]] ||
  fail 'valid PortMaster dialog was not closed before the child launched'
[[ $(wc -l < "$MARKERS/platform-helper") == 1 ]] ||
  fail 'dialog handoff called pm_platform_helper more than once'
[[ $(wc -l < "$MARKERS/dialog-close") == 1 ]] ||
  fail 'dialog handoff did not call PortMasterDialogExit exactly once'
grep -Fq 'PortMaster dialog closed after platform helper' "$LOG" ||
  fail 'successful dialog handoff was not recorded'
assert_finish_once dialog-success

for dialog_failure in unsafe-pipe missing-api failed-close persistent-pipe; do
  rm -f "$DIALOG_PIPE" "$MARKERS/dialog-close" "$MARKERS/child-env"
  : > "$MARKERS/pm-finish"
  if [[ $dialog_failure == unsafe-pipe ]]; then
    : > "$DIALOG_PIPE"
    NXBEHAV_DIALOG_MODE=close
    expected_dialog_error='PM_PIPE is not a live non-symlink FIFO'
  else
    mkfifo "$DIALOG_PIPE"
    case $dialog_failure in
      missing-api)
        NXBEHAV_DIALOG_MODE=missing-api
        expected_dialog_error='close API unavailable while PM_PIPE is active'
        ;;
      failed-close)
        NXBEHAV_DIALOG_MODE=failed-close
        expected_dialog_error='close API returned status 19'
        ;;
      persistent-pipe)
        NXBEHAV_DIALOG_MODE=persistent
        expected_dialog_error='PM_PIPE remained after close request'
        ;;
    esac
  fi
  status=0; run_launcher >/dev/null 2>&1 || status=$?
  [[ $status == 1 ]] ||
    fail "$dialog_failure dialog handoff returned $status, expected 1"
  [[ ! -e $MARKERS/child-env ]] ||
    fail "$dialog_failure dialog handoff launched the child"
  grep -Fq "PortMaster dialog handoff failed: $expected_dialog_error" "$LOG" ||
    fail "$dialog_failure dialog handoff did not log its exact reason"
  assert_finish_once "dialog-$dialog_failure"
done
rm -f "$DIALOG_PIPE"
unset NXBEHAV_DIALOG_PIPE NXBEHAV_DIALOG_MODE

# Background cases must signal the LAUNCHER itself, not a function subshell:
# env execs bash, so $! from this spawn is the launcher process.
spawn_launcher() {
  (
    trap - INT HUP TERM
    exec env -i PATH="$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
      BASH_ENV="${NXBEHAV_BASH_ENV-}" \
      XDG_DATA_HOME="$TEST_ROOT/xdg" XDG_RUNTIME_DIR="$RUNTIME_DIR" \
      NXBEHAV_MAPPING="${NXBEHAV_MAPPING-}" \
      NXBEHAV_ANALOG_STICKS="${NXBEHAV_ANALOG_STICKS-}" \
      NXBEHAV_STICK_ALIAS="${NXBEHAV_STICK_ALIAS-}" \
      NXBEHAV_STUB_MODE="$1" \
      NXBEHAV_EARLY_MARKER="${NXBEHAV_EARLY_MARKER-}" \
      NXBEHAV_DIALOG_PIPE="${NXBEHAV_DIALOG_PIPE-}" \
      NXBEHAV_DIALOG_MODE="${NXBEHAV_DIALOG_MODE-}" \
      NXBEHAV_PROVIDER_OVERRIDE="${NXBEHAV_PROVIDER_OVERRIDE-}" \
      NXBEHAV_HEALTH_MUTATION="${NXBEHAV_HEALTH_MUTATION-}" \
      NXBEHAV_HEALTH_MUTATION_MARKER="${NXBEHAV_HEALTH_MUTATION_MARKER-}" \
      LD_PRELOAD="${NXBEHAV_INHERITED_LD_PRELOAD-}" \
      SDL_DYNAMIC_API="${NXBEHAV_INHERITED_DYNAMIC_API-}" \
      NXPORT_LANGUAGE="${NXBEHAV_LANGUAGE-}" \
      python3 "$TEST_ROOT/reset-signals.py" bash "$LAUNCHER" \
      </dev/null >/dev/null 2>&1
  ) &
  spawned=$!
}

# ------------------------------------- 4. early HUP/INT/TERM status contract
for signal_case in HUP:129 INT:130 TERM:143; do
  signal_name=${signal_case%%:*}
  expected_status=${signal_case#*:}
  NXBEHAV_EARLY_MARKER="$MARKERS/early-$signal_name"
  rm -f "$NXBEHAV_EARLY_MARKER" "$MARKERS/child-env"
  : > "$MARKERS/pm-finish"
  spawn_launcher ''
  launcher_pid=$spawned
  wait_for_file "$NXBEHAV_EARLY_MARKER"
  kill -"$signal_name" "$launcher_pid"
  status=0; wait "$launcher_pid" || status=$?
  [[ $status == "$expected_status" ]] ||
    fail "early $signal_name returned $status, expected $expected_status"
  [[ ! -e $MARKERS/child-env ]] ||
    fail "loader ran after early $signal_name"
  assert_finish_once "early-$signal_name"
done
unset NXBEHAV_EARLY_MARKER

# ---------------------- 4. lock survives atomic replacement of the executable
cat > "$PORTS_DIR/behav-port/bin/behav-loader.next" <<STUB
#!/bin/bash
: > "$MARKERS/replacement-ran"
printf 'run\n' >> "$MARKERS/loader-runs"
{
  printf 'cwd=%s\n' "\$(pwd -P)"
  printf 'game_dir=%s\n' "\${NXCOMPAT_GAME_DIR-}"
  printf 'game_dir_physical=%s\n' "\${NXCOMPAT_GAME_DIR_PHYSICAL-}"
  printf 'port_id=%s\n' "\${NXCOMPAT_PORT_ID-}"
  printf 'mapping=%s\n' "\${SDL_GAMECONTROLLERCONFIG-__unset__}"
} > "$MARKERS/child-env"
printf 'child-stderr\n' >&2
case "\${NXBEHAV_STUB_MODE-}" in
  log-flood)
    printf 'FLOOD-HEAD\n'
    # 6 MiB over the 4 MiB budget, written as the records a real adapter
    # prints into this same stream, so the byte cut lands INSIDE one -- which
    # is the whole point: the survivor still starts with the NXEVENT marker
    # and no longer holds valid JSON.
    pad='xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'
    pad="\$pad\$pad\$pad\$pad\$pad\$pad\$pad\$pad"
    j=0
    while [ "\$j" -lt 12000 ]; do
      printf 'NXEVENT {"schema":"nx-event-v1","source":"graphics","phase":"context","status":"observed","reason_code":1600,"details":{"i":%s,"p":"%s"}}\n' \\
        "\$j" "\$pad"
      j=\$((j + 1))
    done
    printf 'FLOOD-TAIL\n'
    # The runtime adapter owns this file for the whole run, so it is bounded
    # by nobody unless the launcher bounds it. Past the 1 MiB budget.
    if [ -n "\${NXOBS_EVENTS_FILE-}" ]; then
      # Real nx-event-v1 records: the runtime adapter never writes a line
      # without a reason_code or a receipt code, and the support bundle is
      # right to refuse one that does.
      ev='{"schema":"nx-event-v1","schema_version":1,"run_id":"behav","source":"lifecycle","phase":"loop","status":"ok","reason_code":1800,"details":'
      printf '%s{"note":"EVENT-HEAD"}}\n' "\$ev" >> "\$NXOBS_EVENTS_FILE"
      i=0
      while [ "\$i" -lt 9000 ]; do
        printf '%s{"note":"pad","i":%s,"p":"%s"}}\n' \\
          "\$ev" "\$i" 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' \\
          >> "\$NXOBS_EVENTS_FILE"
        i=\$((i + 1))
      done
      printf '%s{"note":"EVENT-TAIL"}}\n' "\$ev" >> "\$NXOBS_EVENTS_FILE"
    fi
    ;;
  sleep) exec sleep 5 ;;
  term-immune)
    trap '' TERM
    : > "$MARKERS/term-immune-ready"
    sleep 2
    ;;
  term-127)
    trap '' TERM
    : > "$MARKERS/term-127-ready"
    sleep 2
    exit 127
    ;;
  term-default)
    : > "$MARKERS/term-default-ready"
    exec sleep 5
    ;;
  term-stuck) exec python3 "$TEST_ROOT/term-stuck.py" ;;
  daemon-fd)
    # A detached background helper that would inherit any leaked lock fd.
    sleep 30 >/dev/null 2>&1 </dev/null &
    printf "%s" "\$!" > "$MARKERS/daemon-pid"
    ;;
esac
exit 42
STUB
chmod 0755 "$PORTS_DIR/behav-port/bin/behav-loader.next"
: > "$MARKERS/pm-finish"
rm -f "$MARKERS/child-env" "$MARKERS/replacement-ran"
spawn_launcher sleep
first=$spawned
wait_for_file "$MARKERS/child-env"
old_loader_id=$(stat -c '%d:%i' "$PORTS_DIR/behav-port/bin/behav-loader")
mv -f "$PORTS_DIR/behav-port/bin/behav-loader.next" \
  "$PORTS_DIR/behav-port/bin/behav-loader"
new_loader_id=$(stat -c '%d:%i' "$PORTS_DIR/behav-port/bin/behav-loader")
[[ $old_loader_id != "$new_loader_id" ]] ||
  fail 'atomic executable replacement did not change inode'
NXBEHAV_STUB_MODE=''
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] || fail "second instance was not refused (got $status)"
grep -q 'another instance holds the lock' "$LOG" ||
  fail 'lock refusal was not logged'
[[ ! -e $MARKERS/replacement-ran ]] ||
  fail 'replacement loader ran while the old inode was active'
assert_finish_once lock-refusal
kill -TERM "$first" 2>/dev/null || true
wait "$first" 2>/dev/null || true
[[ $(wc -l < "$MARKERS/pm-finish") == 2 ]] ||
  fail 'pm_finish was not exactly once for each contending launcher'
lock_released=0
for _ in $(seq 1 60); do
  if flock -n "$LOCK_FILE" -c true 2>/dev/null; then
    lock_released=1
    break
  fi
  sleep 0.2
done
[[ $lock_released == 1 ]] || fail 'stable port lock was not released'
: > "$MARKERS/pm-finish"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "replacement loader did not run after release ($status)"
[[ -e $MARKERS/replacement-ran ]] || fail 'replacement loader never ran'
assert_finish_once replacement-after-release

# ------------------ 4b. no-flock contention never releases another owner
# The fallback is exercised through the generated launcher itself. The first
# launcher wins mkdir and remains alive; the second sees the stable directory,
# exits 1 and must leave both owner bytes and the first child untouched. No
# timeout or process-group signal participates in this proof.
rm -f "$MARKERS/child-env" "$MARKERS/loader-runs"
: > "$MARKERS/pm-finish"
NXBEHAV_BASH_ENV=$NO_FLOCK_BASH_ENV
export NXBEHAV_BASH_ENV
spawn_launcher sleep
fallback_first=$spawned
wait_for_file "$MARKERS/child-env"
wait_for_file "$FALLBACK_LOCK_DIR/owner"
fallback_first_starttime=$(live_process_starttime "$fallback_first") ||
  fail 'first no-flock launcher was not live after acquiring the lock'
fallback_owner_before=$(<"$FALLBACK_LOCK_DIR/owner")
[[ $fallback_owner_before == \
   "pid=$fallback_first token=behav-port-$fallback_first-"* ]] ||
  fail "fallback owner is not bound to first launcher: $fallback_owner_before"
[[ $(wc -l < "$MARKERS/loader-runs") == 1 ]] ||
  fail 'first no-flock launcher did not start exactly one child'
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] ||
  fail "second no-flock instance was not refused (got $status)"
grep -q 'not double-launching (no flock on this firmware)' "$LOG" ||
  fail 'no-flock contention refusal was not logged'
[[ -f $FALLBACK_LOCK_DIR/owner ]] ||
  fail 'second no-flock instance deleted the first lock'
[[ $(<"$FALLBACK_LOCK_DIR/owner") == "$fallback_owner_before" ]] ||
  fail 'second no-flock instance changed the first owner token'
[[ $(wc -l < "$MARKERS/loader-runs") == 1 ]] ||
  fail 'second no-flock instance launched a child'
[[ $(live_process_starttime "$fallback_first") == \
   "$fallback_first_starttime" ]] ||
  fail 'first no-flock launcher identity changed during contention'
assert_finish_once no-flock-contention-refusal
status=0; wait "$fallback_first" || status=$?
[[ $status == 0 ]] ||
  fail "first no-flock launcher did not exit truthfully (got $status)"
[[ ! -e $FALLBACK_LOCK_DIR ]] ||
  fail 'matching first owner did not release its fallback lock'
[[ $(wc -l < "$MARKERS/pm-finish") == 2 ]] ||
  fail 'pm_finish was not exactly once for each no-flock contender'

# Even an actual acquirer loses cleanup authority if its stamped owner changes.
# This simulates same-UID corruption and proves EXIT fails closed on PID/token
# mismatch rather than deleting a path by name.
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
spawn_launcher sleep
fallback_tampered=$spawned
wait_for_file "$MARKERS/child-env"
wait_for_file "$FALLBACK_LOCK_DIR/owner"
printf 'pid=%s token=tampered\n' "$fallback_tampered" > \
  "$FALLBACK_LOCK_DIR/owner"
status=0; wait "$fallback_tampered" || status=$?
[[ $status == 0 ]] ||
  fail "tampered-owner launcher did not preserve child status (got $status)"
[[ -d $FALLBACK_LOCK_DIR && -f $FALLBACK_LOCK_DIR/owner ]] ||
  fail 'fallback lock with a changed owner was released'
grep -q 'owner does not match this launcher; refusing to release it' \
  "$LOG" ||
  fail 'changed fallback owner was not diagnosed'
assert_finish_once no-flock-owner-mismatch
rm -f -- "$FALLBACK_LOCK_DIR/owner"
rmdir "$FALLBACK_LOCK_DIR"
unset NXBEHAV_BASH_ENV

# Lock identity checks must reject attacker-controlled aliases without running
# the loader or modifying the symlink target.
mv "$LOCK_FILE" "$LOCK_FILE.saved"
printf 'victim\n' > "$MARKERS/lock-victim"
ln -s "$MARKERS/lock-victim" "$LOCK_FILE"
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] || fail "symlink lock was accepted (status $status)"
[[ $(<"$MARKERS/lock-victim") == victim && ! -e $MARKERS/child-env ]] ||
  fail 'symlink lock touched its target or launched the child'
assert_finish_once symlink-lock-rejection
rm "$LOCK_FILE"
mv "$LOCK_FILE.saved" "$LOCK_FILE"

ln "$LOCK_FILE" "$LOCK_FILE.alias"
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] || fail "hardlinked lock was accepted (status $status)"
[[ ! -e $MARKERS/child-env ]] || fail 'hardlinked lock launched the child'
assert_finish_once hardlink-lock-rejection
rm "$LOCK_FILE.alias"

# ----------------------- 5. cooperative child keeps its truthful exit status
: > "$MARKERS/pm-finish"
rm -f "$MARKERS/child-env" "$MARKERS/term-immune-ready"
spawn_launcher term-immune
launcher_pid=$spawned
wait_for_file "$MARKERS/child-env"
wait_for_file "$MARKERS/term-immune-ready"
kill -TERM "$launcher_pid" 2>/dev/null || true
kill -HUP "$launcher_pid" 2>/dev/null || true
status=0; wait "$launcher_pid" || status=$?
[[ $status == 42 ]] ||
  fail "TERM-immune child's real status was not reported (got $status)"
grep -q 'end (status 42)' "$LOG" ||
  fail 'log does not record the true status after a trapped signal'
assert_finish_once cooperative-termination

# --------------------------- 6. exit 127 remains a truthful child exit status
: > "$MARKERS/pm-finish"
rm -f "$MARKERS/child-env" "$MARKERS/term-127-ready"
spawn_launcher term-127
launcher_pid=$spawned
wait_for_file "$MARKERS/term-127-ready"
kill -TERM "$launcher_pid" 2>/dev/null || true
kill -HUP "$launcher_pid" 2>/dev/null || true
status=0; wait "$launcher_pid" || status=$?
[[ $status == 127 ]] || fail "real child status 127 became $status"
grep -q 'end (status 127)' "$LOG" || fail 'real status 127 was not recorded'
assert_finish_once status-127

# ----------------------- 7. a child terminated by TERM truthfully returns 143
: > "$MARKERS/pm-finish"
rm -f "$MARKERS/child-env" "$MARKERS/term-default-ready"
spawn_launcher term-default
launcher_pid=$spawned
wait_for_file "$MARKERS/term-default-ready"
kill -TERM "$launcher_pid" 2>/dev/null || true
status=0; wait "$launcher_pid" || status=$?
[[ $status == 143 ]] || fail "real child status 143 became $status"
grep -q 'end (status 143)' "$LOG" || fail 'real status 143 was not recorded'
assert_finish_once status-143

# --------------------------- 8. TERM deadline forces a stuck direct child out
rm -f "$MARKERS/term-stuck-ready" "$MARKERS/term-seen" "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
spawn_launcher term-stuck
launcher_pid=$spawned
wait_for_file "$MARKERS/term-stuck-ready"
kill -TERM "$launcher_pid" 2>/dev/null || true
# Prove a later frontend signal cannot interrupt grace/re-wait only after the
# direct child confirms TERM and the launcher publishes ignored dispositions.
wait_for_file "$MARKERS/term-seen"
wait_for_ignored_signals "$launcher_pid"
kill -INT "$launcher_pid" 2>/dev/null || true
if ! wait_for_process_exit "$launcher_pid"; then
  kill -KILL "$launcher_pid" 2>/dev/null || true
  wait "$launcher_pid" 2>/dev/null || true
  fail 'launcher exceeded the bounded termination deadline'
fi
status=0; wait "$launcher_pid" || status=$?
[[ $status == 137 ]] || fail "forced termination returned $status, expected 137"
[[ -e $MARKERS/term-seen ]] || fail 'stuck child never observed TERM'
grep -q 'termination deadline expired; sending KILL' "$LOG" ||
  fail 'forced KILL was not recorded'
grep -q 'end (status 137)' "$LOG" || fail 'forced status 137 was not recorded'
assert_finish_once forced-termination

# -------- 9. symlinked control is ignored; safe dArkOSRE identity still works
mv "$PM_DIR/control.txt" "$PM_DIR/control.real"
ln -s "$PM_DIR/control.real" "$PM_DIR/control.txt"
NXBEHAV_STUB_MODE=''
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "symlinked control.txt broke the launcher ($status)"
grep -q 'cfw=dArkOSRE' "$LOG" ||
  fail 'launcher did not recognize the two-marker dArkOSRE fallback'

# One marker alone is deliberately insufficient, and the linked control file
# must still never be sourced.
mv "/boot/arkos4clone-uboot.dtb" "/boot/arkos4clone-uboot.dtb.saved"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "single-marker CFW run broke the launcher ($status)"
grep -q 'cfw=none' "$LOG" ||
  fail 'single firmware marker fabricated a CFW identity or sourced control'
mv "/boot/arkos4clone-uboot.dtb.saved" "/boot/arkos4clone-uboot.dtb"
rm "$PM_DIR/control.txt"
mv "$PM_DIR/control.real" "$PM_DIR/control.txt"

# ---------------- 10. hostile CFW_NAME cannot traverse into a mod source
cat > "$TEST_ROOT/xdg/pwn.txt" <<PWN
printf 'pwned\n' >> "$MARKERS/pwn"
PWN
sed -i 's|^CFW_NAME=behavfix$|CFW_NAME="x/../../pwn"|' "$PM_DIR/control.txt"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "hostile CFW_NAME broke the launcher ($status)"
[[ ! -e $MARKERS/pwn ]] ||
  fail 'CFW_NAME path traversal sourced an attacker-controlled mod file'
sed -i 's|^CFW_NAME="x/../../pwn"$|CFW_NAME=behavfix|' "$PM_DIR/control.txt"

# -------- 11. lock fd must not leak to a detached child (close-on-exec sense)
# The stub forks a background helper and exits 42; the launcher exits too. If
# the lock fd had leaked into the child, the orphaned helper would still hold
# the port lock and the next launch would be refused.
: > "$MARKERS/pm-finish"
rm -f "$MARKERS/daemon-pid"
NXBEHAV_STUB_MODE=daemon-fd
status=0; run_launcher >/dev/null 2>&1 || status=$?
NXBEHAV_STUB_MODE=''
[[ $status == 42 ]] || fail "daemon-fd run broke the launcher ($status)"
[[ -f $MARKERS/daemon-pid ]] || fail 'daemon helper was not spawned'
daemon_pid=$(cat "$MARKERS/daemon-pid")
# The detached helper is still alive here, but the launcher has exited.
kill -0 "$daemon_pid" 2>/dev/null || fail 'daemon helper died too early to test the leak'
if ! flock -n "$LOCK_FILE" -c true 2>/dev/null; then
  kill -KILL "$daemon_pid" 2>/dev/null || true
  fail 'lock fd leaked into the detached child; port lock held after launcher exit'
fi
kill -KILL "$daemon_pid" 2>/dev/null || true
wait "$daemon_pid" 2>/dev/null || true

# ---------------------------------------- runtime log budget is really a bound
# Rotation alone only bounds the log BETWEEN runs; the run itself inherits the
# descriptor.  A control run first: an ordinary log is never touched.
[[ -f $LOG ]] || fail 'log budget check has no log to look at'
! grep -q '== log trimmed' "$LOG" || fail 'an ordinary run was trimmed'
: > "$MARKERS/pm-finish"
NXBEHAV_STUB_MODE=log-flood
status=0; run_launcher >/dev/null 2>&1 || status=$?
NXBEHAV_STUB_MODE=''
[[ $status == 42 ]] || fail "log-flood run broke the launcher ($status)"
flood_bytes=$(wc -c < "$LOG")
[[ $flood_bytes -le 4194304 ]] ||
  fail "runtime log ran past its 4 MiB budget ($flood_bytes bytes)"
grep -q '== log trimmed' "$LOG" || fail 'the trimmed log does not say so'
# The tail is what a crash lives in, so the tail is what survives.
grep -q 'FLOOD-TAIL' "$LOG" || fail 'the trim dropped the tail instead of the head'
! grep -q 'FLOOD-HEAD' "$LOG" || fail 'the trim kept the head'
grep -q 'end (status 42)' "$LOG" ||
  fail 'the launcher end receipt did not survive the trim'
SUPPORT_BUNDLE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../nxobs" && pwd -P)/nx-support-bundle.py
[[ -f $SUPPORT_BUNDLE ]] || fail 'nx-support-bundle.py is not where this test expects it'
# Writer against reader for the TEXT log too. The support bundle parses
# NXEVENT records straight out of this file, so a cut landing inside one used
# to cost the whole bundle -- and the trim is what makes the cut.
python3 - "$LOG" <<'PYEOF' || fail 'the trim left a partial record as the first surviving line'
import json, sys
with open(sys.argv[1], encoding="utf-8", errors="replace") as handle:
    handle.readline()                      # the trim marker
    line = handle.readline().rstrip("\n")
# The cut has to have landed inside a record for this case to mean anything,
# and what survives has to be a WHOLE one.
if not line.startswith("NXEVENT "):
    raise SystemExit(1)
json.loads(line[len("NXEVENT "):])
PYEOF
python3 - "$LOG" "$SUPPORT_BUNDLE" <<'PYEOF' || fail 'the support bundle refused the trimmed runtime log'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("nxobs_support", sys.argv[2])
support = importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
events = []
support.parse_runtime(lines, events, "behav")
if not events:
    raise SystemExit(1)
# The trim dropped the partial line, so nothing should have been salvaged as
# a torn first line either.
if any(event["details"].get("note") == "torn-first-line" for event in events):
    raise SystemExit(1)
PYEOF
# The events file has the identical defect and the identical fix, except a byte
# cut in JSONL is fatal where it is harmless in a text log: the support bundle
# parses it line by line.
EVENTS=$PORTS_DIR/behav-port/events.jsonl
[[ -f $EVENTS ]] || fail 'the flood run wrote no events file'
event_bytes=$(wc -c < "$EVENTS")
[[ $event_bytes -le 1048576 ]] ||
  fail "events.jsonl ran past its 1 MiB budget ($event_bytes bytes)"
python3 - "$EVENTS" <<'PYEOF' || fail 'the trimmed events file is not valid JSONL'
import json, sys
lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
if not lines:
    raise SystemExit(1)
first = json.loads(lines[0])
if first.get("code") != "NXU0014" or first["details"]["dropped_bytes"] <= 0:
    raise SystemExit(1)
for line in lines:
    json.loads(line)
notes = [json.loads(line).get("details", {}).get("note") for line in lines]
if "EVENT-TAIL" not in notes or "EVENT-HEAD" in notes:
    raise SystemExit(1)
PYEOF
# Writer against reader, with the bytes the launcher really wrote. The trim
# record and the reader that has to ingest it were built in the same breath;
# nothing so far proved they agree, and a support bundle that chokes on the
# trim would break diagnosis on exactly the long sessions the trim exists for.
python3 - "$EVENTS" "$SUPPORT_BUNDLE" <<'PYEOF' || fail 'the support bundle refused the trimmed events file'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("nxobs_support", sys.argv[2])
support = importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)
raw = open(sys.argv[1], "rb").read()
events = []
support.parse_events_jsonl(raw, events, "behav")
if not events:
    raise SystemExit(1)
# Accepted as real events, not swallowed as the tolerated torn-final-line
# diagnostic, and never more than the reader's bounded window.
if any(event["details"].get("note") == "torn-final-line" for event in events):
    raise SystemExit(1)
if len(events) > support.MAX_EVENTS:
    raise SystemExit(1)
# Truncation has to be VISIBLE: either the launcher's own trim record survived
# the window, or the reader said it dropped older events. A silently short
# event list would read as a session that simply did little.
codes = {event["reason_code"] for event in events}
if support.reason_code_from_code("NXU0014") not in codes and 1902 not in codes:
    raise SystemExit(1)
# The tail is what the window must keep: that is where the failure is. The
# very last records are the launcher's own closing phase events, written after
# the game was gone, so EVENT-TAIL is near the end rather than at it.
notes = [event["details"].get("note") for event in events]
if "EVENT-TAIL" not in notes or "EVENT-HEAD" in notes:
    raise SystemExit(1)
PYEOF
log_budget_checks=12

printf 'nxbootstrap launcher behavior gate passed: runs=74 pre-runtime-log=3 pre-portmaster=1 readonly-fallback=1 lock=7 no-flock-contention=1 owner-token=1 early-signals=3 bounded-termination=4 finish-once=68 physical-gamedir=1 mapping-contract=1 portmaster-hardening=2 dialog-handoff=5 no-stat=1 cfw-fallback=2 lock-fd-no-leak=1 sdl-override-reject=25 sdl-addons=22 sdl-capture=3 sdl-preload-preserve=6 ldso-token=9 stale-system-sdl-drop=2 health-readonly=1 video-exact-kill=1 log-budget=%s\n' "$log_budget_checks"
