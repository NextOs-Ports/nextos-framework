#!/usr/bin/env bash
# Universal PortMaster/NextOS entry point for ASM2 1.2.7d/1.2.8d.

# muOS inspects this literal marker before launching the port and exposes its
# 32-bit PipeWire/SPA runtime for the ARMHF process.
PORT_32BIT="Y"
export PORT_32BIT

XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"

if [ -d /opt/system/Tools/PortMaster ]; then
  controlfolder=/opt/system/Tools/PortMaster
elif [ -d /opt/tools/PortMaster ]; then
  controlfolder=/opt/tools/PortMaster
elif [ -d "$XDG_DATA_HOME/PortMaster" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
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

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" 2>/dev/null && pwd -P) || exit 1
if [ -n "${directory:-}" ] &&
   [ -d "/${directory#/}/ports/asm2_127" ]; then
  GAMEDIR="/${directory#/}/ports/asm2_127"
elif [ -d "$SCRIPT_DIR/asm2_127" ]; then
  GAMEDIR="$SCRIPT_DIR/asm2_127"
elif [ -d /roms/ports/asm2_127 ]; then
  GAMEDIR=/roms/ports/asm2_127
elif [ -d /roms2/ports/asm2_127 ]; then
  GAMEDIR=/roms2/ports/asm2_127
elif [ -d /storage/roms/ports/asm2_127 ]; then
  GAMEDIR=/storage/roms/ports/asm2_127
else
  printf 'ASM2: game directory not found\n' > "$CUR_TTY" 2>/dev/null
  exit 1
fi
GAMEDIR=$(cd -- "$GAMEDIR" 2>/dev/null && pwd -P) || exit 1

export ASM2_GAMEDIR="$GAMEDIR"
export ASM2_CONTROLFOLDER="$controlfolder"
export ASM2_CFW_NAME="${CFW_NAME:-}"
export ASM2_OS_NAME="${OS_NAME:-}"
export SDL_GAMECONTROLLERCONFIG="${sdl_controllerconfig:-${SDL_GAMECONTROLLERCONFIG:-}}"

finish_done=0
finish_frontend_once() {
  [ "$finish_done" -eq 0 ] || return
  finish_done=1
  ${ESUDO:-} chmod 666 "$CUR_TTY" 2>/dev/null || true
  printf '\033c' >> "$CUR_TTY" 2>/dev/null || true
  # This is the only frontend-finalization call in the complete launch flow.
  command -v pm_finish >/dev/null 2>&1 && pm_finish
}

finish_on_exit() {
  local status=$?
  trap - EXIT
  finish_frontend_once
  exit "$status"
}
trap finish_on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

${ESUDO:-} chmod +x \
  "$GAMEDIR/run.sh" \
  "$GAMEDIR/run-extractor.sh" \
  "$GAMEDIR/nxextract.py" \
  "$GAMEDIR/tools/prepare_asm2_data.py" \
  "$GAMEDIR/bin/asm2-nextos-armhf" \
  "$GAMEDIR/bin/asm2-portmaster-armhf" \
  "$GAMEDIR/asm2_127_x86_box32" \
  "$GAMEDIR/runtime/x5m/box64" \
  2>/dev/null || true
${ESUDO:-} chmod 666 "$CUR_TTY" /dev/uinput 2>/dev/null || true

"$GAMEDIR/run.sh"
exit $?
