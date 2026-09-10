#!/usr/bin/env bash
# Universal PortMaster/NextOS entry point.
#
# Do not enable `set -u` here: PortMaster control.txt and its device modules
# intentionally populate some variables lazily and reference them before every
# firmware defines them.  Strict modes remain enabled in the port's own tools.

XDG_DATA_HOME=${XDG_DATA_HOME:-"$HOME/.local/share"}

if [ -d /opt/system/Tools/PortMaster ]; then
  controlfolder=/opt/system/Tools/PortMaster
elif [ -d /opt/tools/PortMaster ]; then
  controlfolder=/opt/tools/PortMaster
elif [ -d "$XDG_DATA_HOME/PortMaster" ]; then
  controlfolder=$XDG_DATA_HOME/PortMaster
elif [ -d /roms/ports/PortMaster ]; then
  controlfolder=/roms/ports/PortMaster
else
  controlfolder=/storage/.config/PortMaster
fi

[ -f "$controlfolder/control.txt" ] &&
  source "$controlfolder/control.txt"
case "${CFW_NAME:-}" in
  ''|*[!A-Za-z0-9._-]*) ;;
  *) [ -f "$controlfolder/mod_${CFW_NAME}.txt" ] &&
       source "$controlfolder/mod_${CFW_NAME}.txt" ;;
esac
declare -F get_controls >/dev/null 2>&1 && get_controls
: "${ESUDO:=}"
: "${CUR_TTY:=/dev/tty0}"

summertime_clear_tty() {
  [ -w "$CUR_TTY" ] || return 0
  printf '\033c' >> "$CUR_TTY" 2>/dev/null || true
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd -P) ||
  exit 1
GAMEDIR=
case "$SCRIPT_DIR" in
  */summertimesaga) GAMEDIR=$SCRIPT_DIR ;;
esac
if [ -z "$GAMEDIR" ] && [ -n "${directory:-}" ]; then
  candidate="/${directory#/}/ports/summertimesaga"
  [ -d "$candidate" ] && GAMEDIR=$candidate
fi
if [ -z "$GAMEDIR" ] && [ -d "$SCRIPT_DIR/summertimesaga" ]; then
  GAMEDIR=$SCRIPT_DIR/summertimesaga
fi
if [ -z "$GAMEDIR" ]; then
  for candidate in \
    /storage/roms/ports/summertimesaga \
    /roms/ports/summertimesaga; do
    if [ -d "$candidate" ]; then
      GAMEDIR=$candidate
      break
    fi
  done
fi
GAMEDIR=$(CDPATH= cd -- "$GAMEDIR" 2>/dev/null && pwd -P) || {
  printf 'Summertime Saga: diretório do port não encontrado\n' \
    > "$CUR_TTY" 2>/dev/null
  exit 1
}

export SUMMERTIME_GAMEDIR=$GAMEDIR
export LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$controlfolder/libs:$controlfolder/libs.aarch64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[ -n "${sdl_controllerconfig:-}" ] &&
  export SDL_GAMECONTROLLERCONFIG=$sdl_controllerconfig

cd "$GAMEDIR" || exit 1
mkdir -p "$GAMEDIR/logs"
exec > "$GAMEDIR/debug.log" 2>&1

${ESUDO:-} chmod +x \
  "$GAMEDIR/run.sh" \
  "$GAMEDIR/run-extractor.sh" \
  "$GAMEDIR/nxextract-runtime-env.sh" \
  "$GAMEDIR/summertimesaga-nextos" \
  "$GAMEDIR/summertimesaga-r36s" \
  "$GAMEDIR/tools/prepare_summertime_data.py" \
  "$GAMEDIR/tools/update-game-data.sh" \
  2>/dev/null || true
${ESUDO:-} chmod 666 "$CUR_TTY" /dev/uinput /dev/input/event* \
  2>/dev/null || true

if [ -x "$GAMEDIR/run-extractor.sh" ] &&
   [ -f "$GAMEDIR/extractor.json" ]; then
  extractor_env=$GAMEDIR/nxextract-runtime-env.sh
  if [ ! -f "$extractor_env" ] || [ -L "$extractor_env" ]; then
    printf 'Summertime Saga: helper do NXExtract ausente ou inválido\n'
    command -v pm_finish >/dev/null 2>&1 && pm_finish
    exit 1
  fi
  # shellcheck source=nxextract-runtime-env.sh
  source "$extractor_env"
  declare -F summertime_run_extractor >/dev/null 2>&1 || {
    printf 'Summertime Saga: helper do NXExtract incompleto\n'
    command -v pm_finish >/dev/null 2>&1 && pm_finish
    exit 1
  }
  machine=$(uname -m 2>/dev/null || true)
  summertime_run_extractor "$GAMEDIR" "$controlfolder" / "$machine" || {
    status=$?
    printf 'Summertime Saga: preparo de dados falhou (%d)\n' "$status"
    summertime_clear_tty
    command -v pm_finish >/dev/null 2>&1 && pm_finish
    exit "$status"
  }
fi

"$GAMEDIR/run.sh"
status=$?

${ESUDO:-} chmod 666 "$CUR_TTY" 2>/dev/null || true
summertime_clear_tty
command -v pm_finish >/dev/null 2>&1 && pm_finish
exit "$status"
