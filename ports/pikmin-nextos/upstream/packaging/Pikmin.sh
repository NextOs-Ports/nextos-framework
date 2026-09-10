#!/bin/bash
# Pikmin - NextOS / Mali-450 GLES2 launcher (native GameCube port)
# Follows the approved PartyBoard/Dusklight recipe: SDL3 "mali" fbdev driver,
# runtime under the port dir, foreground launch (no setsid, no watchdog).

PORTNAME="Pikmin"

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"
[ -n "${CFW_NAME:-}" ] && [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
if command -v get_controls >/dev/null 2>&1; then
  # SSH sessions normally inherit HOME=/root, while NextOS keeps the active
  # EmulationStation controller layout under /storage.  The temporary
  # assignments let PortMaster's mapper see that layout without changing the
  # private HOME used by Pikmin below.
  if [ -f "/storage/.config/emulationstation/es_input.cfg" ]; then
    HOME=/storage XDG_CONFIG_HOME=/storage/.config get_controls
  else
    get_controls
  fi
fi

if [ -n "${directory:-}" ]; then
  ROMSROOT="/$directory"
else
  ROMSROOT="/storage/roms"
fi

GAMEDIR="${PIKMIN_GAMEDIR:-$ROMSROOT/ports/pikmin}"
RUNTIME_DIR="$GAMEDIR/runtime"
ASSETS_DIR="$GAMEDIR/assets"
BIN="$GAMEDIR/pikmin"
SDL3_LIB="$GAMEDIR/libSDL3.so.0"
LOGFILE="$GAMEDIR/log.txt"

mkdir -p "$RUNTIME_DIR/home" "$RUNTIME_DIR/config" "$RUNTIME_DIR/cache" "$ASSETS_DIR"
cd "$GAMEDIR" || exit 1
ulimit -c 0

[ ! -s "$LOGFILE" ] || mv -f "$LOGFILE" "$GAMEDIR/log.prev.txt"
: >"$LOGFILE"
exec >>"$LOGFILE" 2>&1

cleanup() { command -v pm_finish >/dev/null 2>&1 && pm_finish; }
trap cleanup EXIT INT TERM

need() { [ -e "$1" ] || { echo "[missing] $1"; return 1; }; }
need "$BIN" || exit 1
need "$SDL3_LIB" || exit 1

# Locate the GameCube disc (GPIE01). Auto-pick the first image.
DVD_PATH="${PIKMIN_DISC:-}"
if [ -z "$DVD_PATH" ]; then
  for c in "$ASSETS_DIR"/*.rvz "$ASSETS_DIR"/*.RVZ "$ASSETS_DIR"/*.iso "$ASSETS_DIR"/*.ISO "$ASSETS_DIR"/*.gcm "$ASSETS_DIR"/*.GCM; do
    [ -f "$c" ] && { DVD_PATH="$c"; break; }
  done
fi
if [ -z "$DVD_PATH" ]; then
  echo "[missing] Place a supported Pikmin (GPIE01) .rvz/.iso/.gcm in $ASSETS_DIR"
  exit 1
fi

export HOME="$RUNTIME_DIR/home"
export XDG_DATA_HOME="$RUNTIME_DIR"
export XDG_CONFIG_HOME="$RUNTIME_DIR/config"
export XDG_CACHE_HOME="$RUNTIME_DIR/cache"

export LD_LIBRARY_PATH="$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_PRELOAD="$SDL3_LIB${LD_PRELOAD:+:$LD_PRELOAD}"

# NextOS Mali-450 fbdev: the bundled SDL3 carries the mali-fbdev video driver
# (opens /dev/fb0, dlopens GLESv2/EGL from libMali).  Not the SDL2-delegating
# shim - that one recurses through the firmware's SDL2 and blows the stack,
# because the SDL2 and SDL3 symbol names collide in the global scope.
export SDL_VIDEODRIVER=mali
export SDL_AUDIODRIVER=alsa
# NextOS keeps the physical ALSA device open through its system PulseAudio
# service.  Its "default" ALSA PCM routes into that server and can coexist
# with the frontend; opening plughw:0,0 directly fails with EBUSY.
export SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE="${SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE:-default}"
export SDL_AUDIO_DEVICE_SAMPLE_FRAMES="${SDL_AUDIO_DEVICE_SAMPLE_FRAMES:-2048}"
unset ALSA_CONFIG_PATH

# English, always.
export LANG=C
export LC_ALL=C

# Gamepad mapping comes from PortMaster's controls.  Preserve an explicit
# caller-provided mapping; otherwise use the current NextOS PlayGame layout as
# a direct SDL user mapping (higher priority than SDL's stale PS2-position
# default for this GUID).  The bundled db remains a file fallback.
PIKMIN_NEXTOS_PAD_MAPPING="03000000100800000100000010010000,PlayGame PS2-like Controller,a:b1,b:b2,x:b0,y:b3,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b10,rightstick:b11,leftx:a0,lefty:a1,rightx:a3,righty:a2,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,
0300605b100800000100000010010000,PlayGame PS2-like Controller,a:b1,b:b2,x:b0,y:b3,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b10,rightstick:b11,leftx:a0,lefty:a1,rightx:a3,righty:a2,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,"
if [ -z "${SDL_GAMECONTROLLERCONFIG:-}" ]; then
  if [ -n "${sdl_controllerconfig:-}" ]; then
    # Keep PortMaster's mappings for every other pad, then append the two
    # NextOS GUID variants so SDL3 does not fall back to its inverted built-in
    # Twin USB PS2 layout when PortMaster's generated file omits this device.
    export SDL_GAMECONTROLLERCONFIG="${sdl_controllerconfig}
${PIKMIN_NEXTOS_PAD_MAPPING}"
  else
    export SDL_GAMECONTROLLERCONFIG="$PIKMIN_NEXTOS_PAD_MAPPING"
  fi
fi
if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] || [ ! -r "$SDL_GAMECONTROLLERCONFIG_FILE" ]; then
  export SDL_GAMECONTROLLERCONFIG_FILE="$GAMEDIR/gamecontrollerdb.txt"
fi

# Internal resolution scale (1.0 = the game's native 640x480 EFB).
export PIKMIN_INTERNAL_SCALE="${PIKMIN_INTERNAL_SCALE:-1.0}"

echo "[pikmin] NextOS Mali-450 GLES2"
echo "[pikmin] disc=$(basename "$DVD_PATH")"
chmod +x "$BIN" 2>/dev/null || true
command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$BIN"

# Line-buffer the log. stdout is a file here, so libc would block-buffer it and
# a wedge would take the last few KB - which is exactly the part that says why.
if command -v stdbuf >/dev/null 2>&1; then
  stdbuf -oL -eL "$BIN" "$DVD_PATH"
else
  "$BIN" "$DVD_PATH"
fi
exit $?
