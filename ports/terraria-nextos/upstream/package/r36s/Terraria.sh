#!/usr/bin/env bash
# Universal PortMaster entry point for Terraria.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [[ -d /opt/system/Tools/PortMaster ]]; then
  controlfolder=/opt/system/Tools/PortMaster
elif [[ -d /opt/tools/PortMaster ]]; then
  controlfolder=/opt/tools/PortMaster
elif [[ -d "$XDG_DATA_HOME/PortMaster" ]]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [[ -d /roms/ports/PortMaster ]]; then
  controlfolder=/roms/ports/PortMaster
else
  controlfolder=/storage/.config/PortMaster
fi

[[ -f "$controlfolder/control.txt" ]] && source "$controlfolder/control.txt"
case "${CFW_NAME:-}" in
  ''|*[!A-Za-z0-9._-]*) ;;
  *) [[ -f "$controlfolder/mod_${CFW_NAME}.txt" ]] &&
       source "$controlfolder/mod_${CFW_NAME}.txt" ;;
esac
declare -F get_controls >/dev/null 2>&1 && get_controls
: "${ESUDO:=}"
: "${CUR_TTY:=/dev/tty0}"

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" 2>/dev/null && pwd -P) || exit 1
if [[ -n "${TERRARIA_GAME_DIR:-}" ]]; then
  GAMEDIR=$TERRARIA_GAME_DIR
elif [[ -n "${directory:-}" ]]; then
  GAMEDIR="/${directory#/}/ports/terraria"
else
  GAMEDIR="$SCRIPT_DIR/terraria"
fi
GAMEDIR=$(cd -- "$GAMEDIR" 2>/dev/null && pwd -P) || {
  printf 'Terraria: game directory is missing: %s\n' "$GAMEDIR" > "$CUR_TTY" 2>/dev/null
  exit 1
}
export TER_GAMEDIR="$GAMEDIR"

firmware_libs="/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$controlfolder/libs:$controlfolder/libs.aarch64"
export TERRARIA_FIRMWARE_LIBRARY_PATH="$firmware_libs"
export NXEXTRACT_FIRMWARE_LIBRARY_PATH="$firmware_libs"
export LD_LIBRARY_PATH="$firmware_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[[ -n "${sdl_controllerconfig:-}" ]] &&
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

cd "$GAMEDIR" || exit 1
exec > "$GAMEDIR/run.log" 2>&1
printf '=== Terraria NextOS universal %s ===\n' \
  "$(tr -d '\r\n' < "$GAMEDIR/version.txt" 2>/dev/null || printf unknown)"
printf '[launcher] game=%s backend=%s audio=%s\n' \
  "$GAMEDIR" "${SDL_VIDEODRIVER:-auto}" "${SDL_AUDIODRIVER:-auto}"

for executable in terraria run.sh run-extractor.sh nxextract-runtime-env.sh nxextract-ui; do
  [[ -f "$GAMEDIR/$executable" ]] || continue
  $ESUDO chmod +x "$GAMEDIR/$executable" 2>/dev/null ||
    chmod +x "$GAMEDIR/$executable" 2>/dev/null || true
done
$ESUDO chmod 666 "$CUR_TTY" /dev/uinput 2>/dev/null || true

if [[ ! -f "$GAMEDIR/run-extractor.sh" || ! -r "$GAMEDIR/extractor.json" ]]; then
  printf '[launcher] ERROR: NXExtract runtime or recipe is missing\n'
  exit 72
fi
if ! command -v python3 >/dev/null 2>&1; then
  printf '[launcher] ERROR: python3 is required by NXExtract\n'
  exit 72
fi

kill_stray_setup_ui() {
  # Kill only a setup UI that belongs to THIS game directory (matched by its
  # cwd, never by bare process name), so a failed run can never leave a
  # fullscreen UI holding the display, the input grabs and the VT behind us.
  local proc cwd pid
  for proc in /proc/[0-9]*; do
    [[ -d $proc ]] || continue
    cwd=$(readlink "$proc/cwd" 2>/dev/null) || continue
    [[ $cwd == "$GAMEDIR"/.nxextract/* ]] || continue
    [[ $(cat "$proc/comm" 2>/dev/null) == nxextract-ui ]] || continue
    pid=${proc##*/}
    printf '[launcher] terminating stray setup UI pid=%s\n' "$pid"
    kill -TERM "$pid" 2>/dev/null || true
    sleep 1
    kill -KILL "$pid" 2>/dev/null || true
  done
}

printf '[launcher] validating owner-supplied Android data with NXExtract 1.2.2\n'
bash "$GAMEDIR/run-extractor.sh" || {
  status=$?
  printf '[launcher] ERROR: owner-data setup failed (status=%d)\n' "$status"
  kill_stray_setup_ui
  $ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
  printf '\033c' >> "$CUR_TTY" 2>/dev/null || true
  declare -F pm_finish >/dev/null 2>&1 && pm_finish
  exit "$status"
}
kill_stray_setup_ui

missing=()
for relative in libunity.so libil2cpp.so libc++_shared.so \
  bin/Data/boot.config bin/Data/data.unity3d \
  bin/Data/Managed/Metadata/global-metadata.dat .terraria-data.json; do
  [[ -s "$GAMEDIR/$relative" ]] || missing+=("$relative")
done
if (( ${#missing[@]} )); then
  printf '[launcher] ERROR: validated payload is incomplete: %s\n' "${missing[*]}"
  exit 73
fi

"$GAMEDIR/run.sh"
status=$?

$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
printf '\033c' >> "$CUR_TTY" 2>/dev/null || true
declare -F pm_finish >/dev/null 2>&1 && pm_finish
exit "$status"
