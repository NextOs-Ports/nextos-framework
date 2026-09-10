#!/bin/sh
# Summertime Saga Ren'Py Android loader — ArkOS/R36S and current NextOS.
set -u

GAMEDIR=${SUMMERTIME_GAMEDIR:-/storage/roms/ports/summertimesaga}
GAMEDIR=$(CDPATH= cd -- "$GAMEDIR" 2>/dev/null && pwd -P) ||
  { echo "Summertime Saga: diretório inválido"; exit 1; }
cd "$GAMEDIR" || exit 1

# Keep the lock attached to this shell for the whole run. The explicit process
# scan below also catches an orphan whose executable was replaced and now ends
# in " (deleted)".
if command -v flock >/dev/null 2>&1; then
  exec 9>"$GAMEDIR/.summertimesaga.lock"
  flock -n 9 ||
    { echo "Summertime Saga: outro launcher já está ativo"; exit 1; }
fi

summertime_pids() {
  for process in /proc/[0-9]*; do
    [ -d "$process" ] || continue
    pid=${process##*/}
    executable=$(readlink "$process/exe" 2>/dev/null || true)
    case "$executable" in
      "$GAMEDIR/summertimesaga"*)
        echo "$pid"
        continue
        ;;
    esac

    comm=$(cat "$process/comm" 2>/dev/null || true)
    command_line=$(tr '\0' ' ' < "$process/cmdline" 2>/dev/null || true)
    working_dir=$(readlink "$process/cwd" 2>/dev/null || true)
    case "$working_dir:$comm:$command_line" in
      "$GAMEDIR":summertimesaga:*|\
      "$GAMEDIR":summertimesaga-*:*|\
      "$GAMEDIR":*:"./summertimesaga "*|\
      "$GAMEDIR":*:"./summertimesaga-"*)
        echo "$pid"
        ;;
    esac
  done
}

old_pids=$(summertime_pids)
if [ -n "$old_pids" ]; then
  for pid in $old_pids; do
    echo "[run] encerrando instância anterior pid=$pid"
    kill "$pid" 2>/dev/null || true
  done
  attempt=0
  while [ "$attempt" -lt 10 ]; do
    alive=
    for pid in $old_pids; do
      [ -d "/proc/$pid" ] && alive="$alive $pid"
    done
    [ -z "$alive" ] && break
    sleep 1
    attempt=$((attempt + 1))
  done
  for pid in ${alive:-}; do
    echo "[run] forçando encerramento da instância anterior pid=$pid"
    kill -9 "$pid" 2>/dev/null || true
  done
  remaining=$(summertime_pids)
  [ -z "$remaining" ] ||
    { echo "Summertime Saga: instância ainda viva ($remaining)"; exit 1; }
fi

if grep -qE '^ID="?nextos"?$' /etc/os-release 2>/dev/null; then
  BINARY=$GAMEDIR/summertimesaga-nextos
  BUILD_KIND=nextos
else
  BINARY=$GAMEDIR/summertimesaga-r36s
  BUILD_KIND=compat
fi
if [ -n "${SUMMERTIME_BINARY:-}" ]; then
  BINARY=$SUMMERTIME_BINARY
  BUILD_KIND=override
fi
[ -x "$BINARY" ] ||
  { echo "Summertime Saga: loader ausente: $BINARY"; exit 1; }
echo "[run] loader=$BUILD_KIND ($(basename "$BINARY"))"

mkdir -p "$GAMEDIR/logs" "$GAMEDIR/game" "$GAMEDIR/saves/cache"

export XDG_DATA_HOME=$GAMEDIR/saves
export XDG_CONFIG_HOME=$GAMEDIR/saves
export XDG_CACHE_HOME=$GAMEDIR/saves/cache
export LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

for pulse_socket in /var/run/pulse/native /run/pulse/native; do
  if [ -S "$pulse_socket" ]; then
    export PULSE_SERVER=unix:$pulse_socket
    break
  fi
done

export SUMMERTIME_LOG=1
export SUMMERTIME_ASSETS=$GAMEDIR/assets
export SUMMERTIME_GAME_DIR=$GAMEDIR/game
export SUMMERTIME_COMMON_DIR=$GAMEDIR/renpy/common
export SUMMERTIME_LOG_DIR=$GAMEDIR/logs
SUMMERTIME_GAME_VERSION=$(
  sed -n 's/^[[:space:]]*"game_version":[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$GAMEDIR/.summertime-data.json" 2>/dev/null | head -1
)
export SUMMERTIME_GAME_VERSION=${SUMMERTIME_GAME_VERSION:-21-compatible}
export SUMMERTIME_AUTO_GATE=1
# Perfil de GPU/texturas (MAX_TEX, TEXDS_MAX, ETC1, TEX16, PREMUL_PATH,
# GC_EVERY, CURSOR, RENPY_CURSOR): decidido pelo BINARIO depois de medir o
# contexto GLES real e a memoria fisica (ss_apply_runtime_profile, padrao
# SOR4/Horizon Chase). Exportar qualquer uma dessas variaveis aqui ou no
# ambiente vira override de engenharia e desliga so aquele default.

# The OSD2 hardware cursor is specific to the validated Mali-450 framebuffer
# stack. R36S and newer Mali GPUs always use the Ren'Py cursor fallback.
gpu_name=
for gpu_file in \
  /sys/class/misc/mali0/device/gpuinfo \
  /sys/kernel/debug/mali0/gpuinfo \
  /proc/mali/version; do
  [ -r "$gpu_file" ] && gpu_name="$gpu_name $(tr '\n' ' ' < "$gpu_file")"
done
case "$gpu_name" in
  *Mali-450*)
    if [ ! -e /dev/fb1 ] ||
       [ ! -e /sys/class/graphics/fb1/color_key ] ||
       [ ! -e /sys/class/graphics/fb1/enable_key ]; then
      export SUMMERTIME_NO_FB1=1
    fi
    ;;
  *) export SUMMERTIME_NO_FB1=1 ;;
esac

screen_mode=
[ -r /sys/class/graphics/fb0/mode ] &&
  read -r screen_mode < /sys/class/graphics/fb0/mode || true
[ -z "$screen_mode" ] && [ -r /sys/class/graphics/fb0/modes ] &&
  read -r screen_mode < /sys/class/graphics/fb0/modes || true
if [ -n "$screen_mode" ]; then
  screen_mode=${screen_mode#*:}
  screen_mode=${screen_mode%%[!0-9x]*}
  screen_width=${screen_mode%x*}
  screen_height=${screen_mode#*x}
  case "$screen_width:$screen_height" in
    *[!0-9x:]*|:|*:x*|*x*:) screen_width=; screen_height= ;;
  esac
fi
if [ -z "${screen_width:-}" ] || [ -z "${screen_height:-}" ]; then
  for status_file in /sys/class/drm/card*-*/status; do
    [ -r "$status_file" ] || continue
    read -r connector_status < "$status_file" || true
    [ "$connector_status" = connected ] || continue
    modes_file=${status_file%/status}/modes
    [ -r "$modes_file" ] || continue
    read -r screen_mode < "$modes_file" || true
    screen_width=${screen_mode%x*}
    screen_height=${screen_mode#*x}
    case "$screen_width:$screen_height" in
      *[!0-9x:]*|:|*:x*|*x*:) screen_width=; screen_height= ;;
    esac
    [ -n "${screen_width:-}" ] && [ -n "${screen_height:-}" ] && break
  done
fi
export SUMMERTIME_SCREEN_WIDTH=${SUMMERTIME_SCREEN_WIDTH:-${screen_width:-1280}}
export SUMMERTIME_SCREEN_HEIGHT=${SUMMERTIME_SCREEN_HEIGHT:-${screen_height:-720}}
export SUMMERTIME_RES=${SUMMERTIME_RES:-${SUMMERTIME_SCREEN_WIDTH}x${SUMMERTIME_SCREEN_HEIGHT}}

# The game is authored at 16:9. Exact 4:3 panels default to a full-panel
# non-uniform scale because 640x480 handheld users explicitly prefer the
# larger image. SUMMERTIME_DISPLAY_MODE=fit restores aspect-correct letterbox.
if [ -z "${SUMMERTIME_DISPLAY_MODE:-}" ]; then
  case "$SUMMERTIME_SCREEN_WIDTH:$SUMMERTIME_SCREEN_HEIGHT" in
    *[!0-9:]*|:*|*:)
      SUMMERTIME_DISPLAY_MODE=fit
      ;;
    *)
      if [ $((SUMMERTIME_SCREEN_WIDTH * 3)) -eq \
           $((SUMMERTIME_SCREEN_HEIGHT * 4)) ]; then
        SUMMERTIME_DISPLAY_MODE=stretch
      else
        SUMMERTIME_DISPLAY_MODE=fit
      fi
      ;;
  esac
fi
case "$SUMMERTIME_DISPLAY_MODE" in
  fit|stretch|fill) ;;
  *)
    echo "[run] modo de tela inválido '$SUMMERTIME_DISPLAY_MODE'; usando fit"
    SUMMERTIME_DISPLAY_MODE=fit
    ;;
esac
export SUMMERTIME_DISPLAY_MODE

# Cursor movement is normalized to screen size. A modest reduction on small
# 480p panels keeps the D-pad precise without changing the approved 720p feel.
if [ -z "${SUMMERTIME_CURSOR_SCALE:-}" ]; then
  case "$SUMMERTIME_SCREEN_WIDTH:$SUMMERTIME_SCREEN_HEIGHT" in
    *[!0-9:]*|:*|*:) SUMMERTIME_CURSOR_SCALE=1.00 ;;
    *)
      if [ "$SUMMERTIME_SCREEN_WIDTH" -le 640 ] &&
         [ "$SUMMERTIME_SCREEN_HEIGHT" -le 480 ]; then
        SUMMERTIME_CURSOR_SCALE=0.80
      else
        SUMMERTIME_CURSOR_SCALE=1.00
      fi
      ;;
  esac
fi
export SUMMERTIME_CURSOR_SCALE
echo "[run] tela=$SUMMERTIME_RES modo=$SUMMERTIME_DISPLAY_MODE cursor=$SUMMERTIME_CURSOR_SCALE versão=$SUMMERTIME_GAME_VERSION"

export ANDROID_PRIVATE=$GAMEDIR
export ANDROID_PUBLIC=$GAMEDIR
export ANDROID_OLD_PUBLIC=$GAMEDIR
export ANDROID_ARGUMENT=$GAMEDIR
export ANDROID_APP_PATH=$GAMEDIR
export PYTHONDONTWRITEBYTECODE=1
export PYTHONNOUSERSITE=1
export PYTHONUTF8=1
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
export MALLOC_TRIM_THRESHOLD_=${MALLOC_TRIM_THRESHOLD_:-524288}
export RENPY_NO_REDIRECT_STDIO=1
export RENPY_PLATFORM=android

cleanup() {
  if [ -w /sys/class/graphics/fb1/blank ]; then
    echo 1 > /sys/class/graphics/fb1/blank 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

"$BINARY"
status=$?

remaining=$(summertime_pids)
if [ -n "$remaining" ]; then
  echo "[run] limpando processo residual: $remaining"
  for pid in $remaining; do
    kill -9 "$pid" 2>/dev/null || true
  done
fi
exit "$status"
