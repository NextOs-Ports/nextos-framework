#!/usr/bin/env bash
# Universal Prizefighters 2 runtime for PortMaster/NextOS-class ARM64 systems.
# Video, audio, display size and gamepad identity are negotiated at runtime.

PF2_RUNTIME_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" \
  2>/dev/null && pwd -P) || exit 1
PORTNAME='Prizefighters 2 (NextOS)'
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d /opt/system/Tools/PortMaster ]; then
  controlfolder=/opt/system/Tools/PortMaster
elif [ -d /opt/tools/PortMaster ]; then
  controlfolder=/opt/tools/PortMaster
elif [ -d "$XDG_DATA_HOME/PortMaster" ]; then
  controlfolder=$XDG_DATA_HOME/PortMaster
elif [ -d /roms/ports/PortMaster ]; then
  controlfolder=/roms/ports/PortMaster
elif [ -d /storage/roms/ports/PortMaster ]; then
  controlfolder=/storage/roms/ports/PortMaster
else
  controlfolder=/storage/.config/PortMaster
fi

if [ -f "$controlfolder/control.txt" ] &&
   [ ! -L "$controlfolder/control.txt" ]; then
  # shellcheck source=/dev/null
  source "$controlfolder/control.txt"
  case "${CFW_NAME:-}" in
    ''|*[!A-Za-z0-9._-]*) ;;
    *)
      if [ -f "$controlfolder/mod_${CFW_NAME}.txt" ] &&
         [ ! -L "$controlfolder/mod_${CFW_NAME}.txt" ]; then
        # shellcheck source=/dev/null
        source "$controlfolder/mod_${CFW_NAME}.txt"
      fi
      ;;
  esac
  declare -F get_controls >/dev/null 2>&1 && get_controls
fi
: "${ESUDO:=}"
: "${CUR_TTY:=/dev/tty0}"

launcher_error() {
  local message console
  message="Prizefighters 2: $*"
  printf '%s\n' "$message" >&2
  # Failures before the pf2.log redirect (missing files, lock, no game dir)
  # would otherwise be completely silent when launched from the frontend.
  printf '=== %s ===\n%s\n' \
    "$(date -Is 2>/dev/null || date 2>/dev/null || echo unknown)" "$message" \
    >> "$PF2_RUNTIME_DIR/pf2-launcher-error.log" 2>/dev/null
  for console in "${CUR_TTY:-/dev/tty0}" /dev/tty1 /dev/console; do
    [ -w "$console" ] || continue
    printf '\n%s\nSee %s\n' "$message" \
      "$PF2_RUNTIME_DIR/pf2-launcher-error.log" >> "$console" 2>/dev/null &&
      break
  done
  exit 1
}

GAMEDIR=${PF2_GAMEDIR:-$PF2_RUNTIME_DIR}
GAMEDIR=$(CDPATH= cd -- "$GAMEDIR" 2>/dev/null && pwd -P) ||
  launcher_error 'game directory is missing'
BIN=$GAMEDIR/pf2

[ -f "$GAMEDIR/extractor.json" ] &&
[ -f "$GAMEDIR/run-extractor.sh" ] &&
[ -f "$GAMEDIR/nxextract-runtime-env.sh" ] &&
[ -f "$GAMEDIR/nxextract.py" ] ||
  launcher_error 'runtime or NXExtract files are missing'

process_starttime() {
  local stat_line stat_fields
  local -a fields

  IFS= read -r stat_line < "/proc/$1/stat" 2>/dev/null || return 1
  stat_fields=${stat_line#*) }
  read -r -a fields <<< "$stat_fields"
  [ "${#fields[@]}" -ge 20 ] || return 1
  case "${fields[19]}" in
    ''|*[!0-9]*) return 1 ;;
  esac
  printf '%s\n' "${fields[19]}"
}

LOCK_FILE=
LOCK_DIR=
LOCK_KIND=
LOCK_START=

lock_cleanup() {
  local owner_pid owner_start

  if [ "$LOCK_KIND" = mkdir ] && [ -r "$LOCK_DIR/owner" ]; then
    read -r owner_pid owner_start < "$LOCK_DIR/owner" || return
    if [ "$owner_pid" = "$$" ] && [ "$owner_start" = "$LOCK_START" ]; then
      rm -f -- "$LOCK_DIR/owner"
      rmdir -- "$LOCK_DIR" 2>/dev/null || true
    fi
  fi
}

acquire_launch_lock() {
  local lock_pid lock_start live_start stale_dir

  LOCK_FILE=$GAMEDIR/.pf2-launch.lock
  LOCK_DIR=$GAMEDIR/.pf2-launch.lock.d
  LOCK_START=$(process_starttime "$$")
  case "$LOCK_START" in
    ''|*[!0-9]*) launcher_error 'could not identify launcher process' ;;
  esac

  if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK_FILE" || launcher_error 'could not open launch lock'
    flock -n 9 || launcher_error 'another Prizefighters 2 launcher is active'
    LOCK_KIND=flock
  else
    while ! mkdir "$LOCK_DIR" 2>/dev/null; do
      [ -r "$LOCK_DIR/owner" ] || launcher_error 'launch lock has no owner'
      read -r lock_pid lock_start < "$LOCK_DIR/owner" ||
        launcher_error 'launch lock is invalid'
      case "$lock_pid:$lock_start" in
        *[!0-9:]*|:*|*:) launcher_error 'launch lock is invalid' ;;
      esac
      live_start=$(process_starttime "$lock_pid")
      [ "$live_start" != "$lock_start" ] ||
        launcher_error 'another Prizefighters 2 launcher is active'
      stale_dir="${LOCK_DIR}.stale.$$.$LOCK_START"
      if mv -- "$LOCK_DIR" "$stale_dir" 2>/dev/null; then
        rm -f -- "$stale_dir/owner"
        rmdir -- "$stale_dir" 2>/dev/null || true
      fi
    done
    printf '%s %s\n' "$$" "$LOCK_START" > "$LOCK_DIR/owner" ||
      launcher_error 'could not record launch lock owner'
    LOCK_KIND=mkdir
  fi
}

matching_game_pids() {
  local process pid comm argv0 executable working_directory matched

  for process in /proc/[0-9]*; do
    [ -d "$process" ] || continue
    pid=${process##*/}
    [ "$pid" = "$$" ] && continue
    [ "$pid" = "${PPID:-}" ] && continue
    comm=
    argv0=
    IFS= read -r comm < "$process/comm" 2>/dev/null || true
    IFS= read -r -d '' argv0 < "$process/cmdline" 2>/dev/null || true
    executable=$(command readlink "$process/exe" 2>/dev/null || true)
    working_directory=$(command readlink "$process/cwd" 2>/dev/null || true)
    matched=0

    case "$executable" in
      "$BIN"|"$BIN (deleted)"|\
      */ports/pf2/pf2|*/ports/pf2/pf2\ \(deleted\)|\
      */ports/pf2-*/pf2|*/ports/pf2-*/pf2\ \(deleted\)|\
      */ports/pf2nextos/pf2|*/ports/pf2nextos/pf2\ \(deleted\))
        matched=1 ;;
    esac
    if [ "$matched" -eq 0 ]; then
      case "$argv0" in
        "$BIN"|./pf2|*/ports/pf2/pf2|*/ports/pf2-*/pf2|\
        */ports/pf2nextos/pf2) matched=1 ;;
      esac
    fi
    if [ "$matched" -eq 0 ] && [ "$working_directory" = "$GAMEDIR" ]; then
      case "$comm" in
        pf2|UnityMain) matched=1 ;;
      esac
    fi
    if [ "$matched" -ne 0 ]; then
      printf '[launcher] found PF2 pid=%s comm=%s exe=%s cwd=%s argv0=%s\n' \
        "$pid" "$comm" "$executable" "$working_directory" "$argv0" >&2
      printf '%s\n' "$pid"
    fi
  done
}

stop_existing_game() {
  local old_pids pid attempt remaining

  old_pids=$(matching_game_pids)
  if [ -n "$old_pids" ]; then
    for pid in $old_pids; do
      printf '[launcher] stopping old PF2 instance pid=%s\n' "$pid"
      kill "$pid" 2>/dev/null || true
    done
    attempt=0
    remaining=$(matching_game_pids)
    while [ -n "$remaining" ] && [ "$attempt" -lt 20 ]; do
      sleep 0.5
      attempt=$((attempt + 1))
      remaining=$(matching_game_pids)
    done
    if [ -n "$remaining" ]; then
      for pid in $remaining; do
        printf '[launcher] forcing old PF2 instance pid=%s\n' "$pid"
        kill -9 "$pid" 2>/dev/null || true
      done
      sleep 1
    fi
  fi
  remaining=$(matching_game_pids)
  [ -z "$remaining" ] ||
    launcher_error "an old game instance could not be stopped: $remaining"
}

finish_done=0
finish_frontend_once() {
  [ "$finish_done" -eq 0 ] || return
  finish_done=1
  ${ESUDO:-} chmod 666 "$CUR_TTY" 2>/dev/null || true
  [ -w "$CUR_TTY" ] && printf '\033c' >> "$CUR_TTY" 2>/dev/null || true
  command -v pm_finish >/dev/null 2>&1 && pm_finish
}

finish_on_exit() {
  local status=$?
  trap - EXIT
  lock_cleanup
  finish_frontend_once
  exit "$status"
}

trap finish_on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

cd "$GAMEDIR" || launcher_error 'could not enter game directory'
acquire_launch_lock
if [ -s "$GAMEDIR/pf2.log" ]; then
  mv -f -- "$GAMEDIR/pf2.log" "$GAMEDIR/pf2.prev.log"
fi
exec > "$GAMEDIR/pf2.log" 2>&1
release_version=$(command tr -d '\r\n' < "$GAMEDIR/version.txt" \
  2>/dev/null || true)
printf '=== %s | release %s | %s ===\n' \
  "$PORTNAME" "${release_version:-unknown}" "$(date -Is 2>/dev/null || date)"

${ESUDO:-} chmod +x \
  "$BIN" \
  "$GAMEDIR/pf2-nextos.sh" \
  "$GAMEDIR/run-extractor.sh" \
  "$GAMEDIR/nxextract-runtime-env.sh" \
  "$GAMEDIR/nxextract.py" \
  "$GAMEDIR/nxextract-ui" \
  "$GAMEDIR/tools/prepare_pf2_data.py" \
  2>/dev/null || true

stop_existing_game

nx_firmware_libraries=
for directory in "$controlfolder/libs" "$controlfolder/libs.aarch64"; do
  if [ -d "$directory" ]; then
    nx_firmware_libraries=${nx_firmware_libraries:+$nx_firmware_libraries:}$directory
  fi
done

NXEXTRACT_GAME_DIR=$GAMEDIR \
NXEXTRACT_FIRMWARE_LIBRARY_PATH=$nx_firmware_libraries \
  "$GAMEDIR/run-extractor.sh" || {
  status=$?
  launcher_error "game-data preparation failed ($status)"
}
if [ "${PF2_EXTRACTOR_ONLY:-0}" = 1 ]; then
  printf '[launcher] extractor-only validation completed\n'
  exit 0
fi

[ -x "$BIN" ] || launcher_error 'runtime loader is missing or not executable'

# Host libraries stay first so SDL, EGL, Mali and audio always match the
# running kernel/firmware. Android game DSOs are opened explicitly by PF2.
host_libraries='/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib'
if [ -n "$nx_firmware_libraries" ]; then
  host_libraries=$host_libraries:$nx_firmware_libraries
fi
export LD_LIBRARY_PATH="$host_libraries:$GAMEDIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PF2_GAMEDIR=$GAMEDIR
export PF2_CONTROLFOLDER=$controlfolder
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
export SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0

# Menu pointer speed in the port's 1280x720 design space (pixels/second).
# Edit this default or export PF2_CURSOR_SPEED before launch to tune it.
: "${PF2_CURSOR_SPEED:=1400}"
export PF2_CURSOR_SPEED

[ -n "${sdl_controllerconfig:-}" ] &&
  export SDL_GAMECONTROLLERCONFIG=$sdl_controllerconfig
if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ]; then
  for database in \
    "$controlfolder/gamecontrollerdb.txt" \
    "$controlfolder/gamecontrollerdb-SDL2.txt" \
    /storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt \
    /storage/.config/SDL-GameControllerDB/gamecontrollerdb-SDL2.txt; do
    if [ -r "$database" ] && [ ! -L "$database" ]; then
      export SDL_GAMECONTROLLERCONFIG_FILE=$database
      break
    fi
  done
fi

for pulse_socket in /var/run/pulse/native /run/pulse/native; do
  if [ -z "${PULSE_SERVER:-}" ] && [ -S "$pulse_socket" ]; then
    export PULSE_SERVER=unix:$pulse_socket
    break
  fi
done

memory_kib=$(awk '/^MemTotal:/ {print $2; exit}' /proc/meminfo \
  2>/dev/null || true)
case "$memory_kib" in
  ''|*[!0-9]*) memory_kib=0 ;;
esac
if [ "$memory_kib" -gt 0 ] && [ "$memory_kib" -lt 1250000 ]; then
  export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
  export MALLOC_TRIM_THRESHOLD_=${MALLOC_TRIM_THRESHOLD_:-131072}
  export MALLOC_MMAP_THRESHOLD_=${MALLOC_MMAP_THRESHOLD_:-65536}
fi

printf '[launcher] binary=%s video=%s audio=%s controller=%s\n' \
  "$(basename "$BIN")" \
  "${SDL_VIDEODRIVER:-firmware-auto}" \
  "${PF2_AUDIO_DRIVER:-${SDL_AUDIODRIVER:-firmware-auto}}" \
  "${SDL_GAMECONTROLLERCONFIG:+PortMaster mapping}"
if command -v pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$BIN" >/dev/null ||
    launcher_error 'PortMaster could not prepare frontend lifecycle'
fi

"$BIN" "$GAMEDIR"
exit $?
