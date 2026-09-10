#!/bin/bash
# PORTMASTER: sonic4ep2.zip, Sonic4EP2.sh

PORTNAME="Sonic The Hedgehog 4: Episode II"
XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"

if [ -d "/opt/system/Tools/PortMaster" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "$controlfolder/mod_${CFW_NAME:-}.txt" ] \
  && source "$controlfolder/mod_${CFW_NAME}.txt"
get_controls

directory="${directory:-roms}"
GAMEDIR="/${directory#/}/ports/sonic4ep2"
cd "$GAMEDIR" || exit 1
exec > "$GAMEDIR/log.txt" 2>&1

chmod +x "$GAMEDIR/sonic4.arm64" \
  "$GAMEDIR/tools/sonic4ep2_extract.sh" 2>/dev/null || true

# Prefer firmware libraries and use the bundled audio codecs only as fallback.
export LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib:$GAMEDIR:${LD_LIBRARY_PATH:-}:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:$GAMEDIR/libs.aarch64"
[ -n "${sdl_controllerconfig:-}" ] \
  && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Knulli/Batocera keep the working ALSA configuration outside the login home.
if [ -z "${SONIC_KEEP_HOME:-}" ]; then
  for audio_home in "$HOME" /userdata/system /storage/.config /root; do
    [ -f "$audio_home/.asoundrc" ] \
      && { export HOME="$audio_home"; break; }
  done
fi

echo "[port] $PORTNAME $(sed -n '1p' "$GAMEDIR/version.txt" 2>/dev/null)"
pm_platform_helper "$GAMEDIR/sonic4.arm64"
"$GAMEDIR/sonic4.arm64"
status=$?
pm_finish
exit "$status"
