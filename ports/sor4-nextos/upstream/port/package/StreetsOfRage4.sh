#!/bin/bash
# PORTMASTER: sor4.zip, StreetsOfRage4.sh

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

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"
case "${CFW_NAME:-}" in
  ''|*[!A-Za-z0-9._-]*) ;;
  *) [ -f "$controlfolder/mod_${CFW_NAME}.txt" ] &&
       source "$controlfolder/mod_${CFW_NAME}.txt" ;;
esac
declare -F get_controls >/dev/null 2>&1 && get_controls
: "${ESUDO:=}"
# control.txt owns the console the firmware really uses; only assume tty0 without it.
: "${CUR_TTY:=/dev/tty0}"

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" 2>/dev/null && pwd -P) || exit 1
if [ -n "${directory:-}" ]; then
  GAMEDIR="/${directory#/}/ports/sor4"
else
  GAMEDIR="$SCRIPT_DIR/sor4"
fi
GAMEDIR=$(cd -- "$GAMEDIR" 2>/dev/null && pwd -P) || exit 1
PKG="$GAMEDIR/host_pkg"

cd "$GAMEDIR" || exit 1
# One session per file: the launcher owns log.txt and truncates it here, so the
# starter must never rewrite the file underneath this descriptor.
exec > "$GAMEDIR/log.txt" 2>&1

${ESUDO:-} chmod +x "$PKG/sor4host" "$GAMEDIR/tools/sor4_setup.sh" \
  "$GAMEDIR/tools/sor4_profile.sh" "$GAMEDIR/tools/sor4probe" \
  "$GAMEDIR/tools/sor4splash" 2>/dev/null || true
${ESUDO:-} chmod 666 "$CUR_TTY" /dev/uinput 2>/dev/null || true

# The frontend kills this script when the user leaves the menu.  An orphan host holding
# the framebuffer/DRM master makes the NEXT launch open black, so take our own child
# down with us -- and only ever our own child.
sor4_stop_host() {
  local attempt
  [ -n "${SOR4_HOST_PID:-}" ] || return 0
  kill -TERM "$SOR4_HOST_PID" 2>/dev/null || return 0
  for attempt in 1 2 3 4 5 6 7 8; do
    kill -0 "$SOR4_HOST_PID" 2>/dev/null || return 0
    sleep 1
  done
  kill -KILL "$SOR4_HOST_PID" 2>/dev/null || true
}
trap 'sor4_stop_host' EXIT INT TERM

# Firmware libraries first; PortMaster's own compatibility directory next; the
# self-contained bundle last.  The starter re-derives this list, but the probe and
# the setup splash run before that and need the same view.
export SOR4_CONTROLFOLDER="$controlfolder"
export LD_LIBRARY_PATH="$PKG/libs:/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$controlfolder/libs:$controlfolder/libs.aarch64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[ -n "${sdl_controllerconfig:-}" ] &&
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

command -v pm_platform_helper >/dev/null 2>&1 &&
  pm_platform_helper "$PKG/sor4host" >/dev/null 2>&1

# Started in the background on purpose: bash defers a trap until the current foreground
# command returns, so a launcher blocked on the host would only react to SIGTERM after
# the host had already exited -- exactly when the reaper is no longer needed.
"$PKG/sor4host" --starter &
SOR4_HOST_PID=$!
while :; do
  wait "$SOR4_HOST_PID"
  status=$?
  kill -0 "$SOR4_HOST_PID" 2>/dev/null || break
done
SOR4_HOST_PID=""
if [ "$status" -eq 75 ]; then
  exit 0
fi
if [ "$status" -ne 0 ]; then
  echo "SOR4: setup ou inicializacao falhou; veja sor4/log.txt. Seus dados foram preservados." \
    > "$CUR_TTY" 2>/dev/null || true
fi
${ESUDO:-} chmod 666 "$CUR_TTY" 2>/dev/null || true
printf '\033c' >> "$CUR_TTY" 2>/dev/null || true
command -v pm_finish >/dev/null 2>&1 && pm_finish
exit "$status"
