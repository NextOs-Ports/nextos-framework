#!/bin/sh
# Huntdown (Unity 2022.3.47f1 IL2CPP) — universal launcher.
#
# One implementation for every firmware.  The loader negotiates fbdev/Mali,
# KMSDRM, Wayland and GLES at runtime; this script only prepares the process
# and supervises it in the foreground.
#
# The launcher never manages the frontend: no systemctl, no stopping or
# restarting EmulationStation, no setsid, no nohup.  The frontend returns
# because this script ends.
set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
GAMEDIR=${HD_GAMEDIR:-$SCRIPT_DIR}
export HD_GAMEDIR=$GAMEDIR
cd "$GAMEDIR" || { echo "Huntdown: missing $GAMEDIR" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Log with a single owner. "No log was produced" must never be a possible
# state, so the redirect happens before anything that can fail.
# ---------------------------------------------------------------------------
if [ -s "$GAMEDIR/launcher.log" ]; then
  mv -f -- "$GAMEDIR/launcher.log" "$GAMEDIR/launcher.prev.log" 2>/dev/null ||
    true
fi
if : > "$GAMEDIR/launcher.log" 2>/dev/null; then
  exec >>"$GAMEDIR/launcher.log" 2>&1
fi

hd_screen() {
  # The frontend discards stderr, so a user-visible failure also goes to the
  # console PortMaster handed us, or to the current terminal.
  [ -n "${CUR_TTY:-}" ] && printf '%s\n' "$*" > "$CUR_TTY" 2>/dev/null
  printf '%s\n' "$*" > /dev/tty 2>/dev/null || true
  printf '%s\n' "$*" >&2
}

hd_fail() {
  hd_screen "Huntdown: $*"
  hd_screen "Details: $GAMEDIR/launcher.log"
  exit 1
}

printf '[run] Huntdown launcher %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ' \
  2>/dev/null || echo '?')"

# ---------------------------------------------------------------------------
# PortMaster is optional. control.txt of real firmwares reads variables that
# may still be unset, so `set -u` is lifted only while it is sourced.
# ---------------------------------------------------------------------------
for hd_control in \
  /roms/tools/PortMaster/control.txt \
  /roms2/tools/PortMaster/control.txt \
  /opt/system/Tools/PortMaster/control.txt \
  /opt/tools/PortMaster/control.txt \
  /storage/roms/ports/PortMaster/control.txt \
  /userdata/system/.local/share/PortMaster/control.txt \
  /mnt/mmc/MUOS/PortMaster/control.txt \
  /PortMaster/control.txt; do
  if [ -f "$hd_control" ]; then
    set +u
    # shellcheck disable=SC1090
    . "$hd_control"
    set -u
    printf '[run] PortMaster: %s\n' "$hd_control"
    break
  fi
done

if command -v get_controls >/dev/null 2>&1; then
  set +u
  get_controls
  set -u
  printf '[run] PortMaster get_controls aplicado\n'
fi
# An explicit user/PortMaster mapping outranks any database the loader loads.
if [ -n "${sdl_controllerconfig:-}" ]; then
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
  printf '[run] SDL_GAMECONTROLLERCONFIG do PortMaster ativo\n'
fi

# ---------------------------------------------------------------------------
# Required artefacts. Refusing here beats a predictable black screen.
# ---------------------------------------------------------------------------
[ -f "$GAMEDIR/huntdown" ] || hd_fail "the loader is missing (huntdown)."
for hd_required in libmain.so libunity.so libil2cpp.so; do
  [ -f "$GAMEDIR/$hd_required" ] ||
    hd_fail "game data is not installed yet ($hd_required is missing)."
done

# ---------------------------------------------------------------------------
# One instance only. flock follows the process through descriptor 9, including
# across exec; a second launcher aborts before touching the live one.
# ---------------------------------------------------------------------------
if command -v flock >/dev/null 2>&1; then
  exec 9>"$GAMEDIR/.huntdown.lock"
  flock -n 9 || hd_fail "another Huntdown launcher is already running."
fi

# Never launch over a live instance. Matches the executable, the cwd/comm and
# also an executable marked "(deleted)" after a file replacement.
hd_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null || true)
    case "$e" in
      "$GAMEDIR/huntdown"*) echo "${p##*/}"; continue ;;
    esac
    c=$(cat "$p/comm" 2>/dev/null || true)
    # A process can vanish between the glob and the read; the redirect itself
    # is what fails, so silence the whole substitution rather than the command.
    a=$( { tr '\0' ' ' < "$p/cmdline"; } 2>/dev/null || true)
    d=
    case "$c:$a" in
      UnityMain:*|huntdown:*|*:"./huntdown "*|*:"$GAMEDIR/huntdown "*)
        d=$(readlink "$p/cwd" 2>/dev/null || true)
        ;;
    esac
    case "$d:$c" in
      "$GAMEDIR:UnityMain"|"$GAMEDIR:huntdown")
        echo "${p##*/}"; continue ;;
    esac
    case "$d:$a" in
      "$GAMEDIR:./huntdown "*|"$GAMEDIR:$GAMEDIR/huntdown "*)
        echo "${p##*/}" ;;
    esac
  done
}
old_pids=$(hd_pids)
if [ -n "$old_pids" ]; then
  for pid in $old_pids; do
    echo "[run] encerrando instância anterior pid=$pid"
    kill "$pid" 2>/dev/null || true
  done
  i=0
  while [ "$i" -lt 20 ]; do
    alive=
    for pid in $old_pids; do [ -d "/proc/$pid" ] && alive="$alive $pid"; done
    [ -z "$alive" ] && break
    sleep 0.5
    i=$((i+1))
  done
  for pid in ${alive:-}; do
    echo "[run] forçando encerramento da instância anterior pid=$pid"
    kill -9 "$pid" 2>/dev/null || true
  done
  remaining=$(hd_pids)
  [ -z "$remaining" ] || hd_fail "a previous instance is still running."
fi

if [ -n "${LD_LIBRARY_PATH:-}" ]; then
  export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$GAMEDIR
else
  export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR
fi
# Unity/FMOD create many short-lived worker threads. Two malloc arenas avoid
# retaining one fragmented glibc heap per thread on 1 GB class devices.
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
# Huntdown is an action game authored for 60 Hz. Keep the Android-equivalent
# cadence unless an explicit engineering override requests a lower limit.
export HD_FRAME_LIMIT=${HD_FRAME_LIMIT:-60}

# Real resolution, never a hard-coded panel. The EGL drawable stays the final
# authority; these variables only feed the JNI before the window exists.
_m=
[ -r /sys/class/graphics/fb0/mode ]  && read -r _m < /sys/class/graphics/fb0/mode  || true
[ -z "$_m" ] && [ -r /sys/class/graphics/fb0/modes ] && read -r _m < /sys/class/graphics/fb0/modes || true
if [ -n "$_m" ]; then
  _p=${_m#*:}; _p=${_p%%[!0-9x]*}; _w=${_p%x*}; _h=${_p#*x}
  case "$_w" in ''|*[!0-9]*) _w= ;; esac
  case "$_h" in ''|*[!0-9]*) _h= ;; esac
fi
if [ -z "${_w:-}" ] || [ -z "${_h:-}" ]; then
  for _status in /sys/class/drm/card*-*/status; do
    [ -r "$_status" ] || continue
    read -r _connected < "$_status" || true
    [ "$_connected" = connected ] || continue
    _modes=${_status%/status}/modes
    [ -r "$_modes" ] || continue
    read -r _mode < "$_modes" || true
    _w=${_mode%x*}; _h=${_mode#*x}
    case "$_w" in ''|*[!0-9]*) _w= ;; esac
    case "$_h" in ''|*[!0-9]*) _h= ;; esac
    [ -n "${_w:-}" ] && [ -n "${_h:-}" ] && break
  done
fi
if { [ -z "${_w:-}" ] || [ -z "${_h:-}" ]; } && [ -r /sys/class/graphics/fb0/virtual_size ]; then
  IFS=, read -r _w _vh < /sys/class/graphics/fb0/virtual_size || true
  case "${_w:-}" in ''|*[!0-9]*) _w= ;; esac
  case "${_vh:-}" in ''|*[!0-9]*) _vh= ;; esac
  if [ -n "${_w:-}" ] && [ -n "${_vh:-}" ]; then
    _h=$_vh
    # 1280x1440 and 640x960 are double-buffered; 640x480 is not.
    if [ "$_vh" -gt "$_w" ] && [ $((_vh % 2)) -eq 0 ]; then
      _half=$((_vh / 2))
      if [ "$_half" -le "$_w" ] && [ $((_half * 2)) -ge "$_w" ]; then
        _h=$_half
      fi
    fi
  fi
fi
[ -n "${_w:-}" ] && [ -n "${_h:-}" ] && export HD_SCREEN_W="$_w" HD_SCREEN_H="$_h"
echo "[run] fb real = ${HD_SCREEN_W:-?}x${HD_SCREEN_H:-?}"

# Log files are opt-in inside the loader; this script owns launcher.log.
export HD_NOLOGFILE=1
export HD_FRAMES=${HD_FRAMES:-0}

# Audio and video are negotiated independently. An inherited PULSE_SERVER that
# points at a dead daemon is dropped so it cannot take SDL_Init down with it;
# the loader then walks its own ALSA ladder.
hd_pulse_socket=
for _socket in /var/run/pulse/native /run/pulse/native; do
  [ -S "$_socket" ] && { hd_pulse_socket=$_socket; break; }
done
if [ -n "$hd_pulse_socket" ]; then
  export PULSE_SERVER="unix:$hd_pulse_socket"
  echo "[run] áudio: socket Pulse real em $hd_pulse_socket"
else
  case "${PULSE_SERVER:-}" in
    unix:*)
      echo "[run] áudio: PULSE_SERVER herdado sem socket; removido (ALSA)"
      unset PULSE_SERVER
      ;;
    ?*)
      echo "[run] áudio: PULSE_SERVER herdado preservado (${PULSE_SERVER})"
      ;;
    *)
      echo "[run] áudio: sem Pulse; o loader usa a escada ALSA do firmware"
      ;;
  esac
fi
# SDL_VIDEODRIVER and SDL_AUDIODRIVER are never forced here. Whatever the
# firmware inherited stays; the loader retests once, with a log, if it fails.

# Android/Unity enumerates one stable identity; the physical state is
# normalised to the Xbox/XInput layout by the Huntdown bridge.
export HD_GAMEPAD=1

# Several CFWs only hand the display over after this call; without it the game
# runs behind a black screen.
if command -v pm_platform_helper >/dev/null 2>&1; then
  set +u
  pm_platform_helper "$GAMEDIR/huntdown"
  set -u
  echo "[run] pm_platform_helper chamado"
fi

# The game runs as this shell's own child so a TERM from the frontend reaches
# the exact PID and travels the same pause/save/exit path as SELECT+START.
# A shell waiting on a foreground child never runs its trap, hence `&` + wait.
child_pid=
stop_child() {
  [ -n "${child_pid:-}" ] || return 0
  echo "[run] TERM recebido; encaminhando ao jogo pid=$child_pid"
  kill -TERM "$child_pid" 2>/dev/null || return 0
  for _attempt in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$child_pid" 2>/dev/null || return 0
    sleep 1
  done
  echo "[run] prazo esgotado; KILL em $child_pid"
  kill -KILL "$child_pid" 2>/dev/null || true
}
trap stop_child INT TERM HUP

echo "[run] iniciando o jogo"
./huntdown &
child_pid=$!
# A trap interrupts wait before the child is reaped; keep waiting until the
# PID is really gone so the exit status belongs to the game.
while :; do
  wait "$child_pid"
  status=$?
  kill -0 "$child_pid" 2>/dev/null || break
done
trap - INT TERM HUP
child_pid=
echo "[run] jogo terminou com status=$status"

# No process may keep the display or the audio device after we return.
leftover=$(hd_pids)
if [ -n "$leftover" ]; then
  echo "[run] AVISO: processo remanescente ($leftover); encerrando"
  for pid in $leftover; do kill -9 "$pid" 2>/dev/null || true; done
fi

if command -v pm_finish >/dev/null 2>&1; then
  set +u
  pm_finish
  set -u
fi

exit "$status"
