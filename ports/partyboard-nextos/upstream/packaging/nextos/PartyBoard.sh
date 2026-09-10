#!/bin/bash
# PartyBoard — NextOS / Mali-450 GLES2 launcher (native Mario Party 4 port)
# Mirrors the approved Dusklight recipe: SDL3 "mali" fbdev driver, ALSA audio env,
# runtime under the port dir, foreground launch (no setsid/nohup/watchdog).

PORTNAME="PartyBoard"

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
command -v get_controls >/dev/null 2>&1 && get_controls

if [ -n "${directory:-}" ]; then
  ROMSROOT="/$directory"
else
  ROMSROOT="/storage/roms"
fi

GAMEDIR="${PARTYBOARD_GAMEDIR:-$ROMSROOT/ports/partyboard}"
RUNTIME_DIR="$GAMEDIR/runtime"
ASSETS_DIR="$GAMEDIR/assets"
BIN="$GAMEDIR/partyboard"
SDL3_LIB="$GAMEDIR/sdl3-fallback/libSDL3.so.0"
LOGFILE="$GAMEDIR/log.txt"

mkdir -p "$RUNTIME_DIR/home" "$RUNTIME_DIR/config" "$RUNTIME_DIR/cache" "$ASSETS_DIR"
cd "$GAMEDIR" || exit 1
ulimit -c 0

# Warm the driver-specific GL program cache on a fresh install. Aurora validates
# its embedded Mali/GL fingerprint before using any binary and discards the
# cache automatically when the firmware or driver differs.
PROGRAM_CACHE_SEED="$GAMEDIR/initial_program_binary_cache.db"
PROGRAM_CACHE_DIR="$RUNTIME_DIR/Party Board"
PROGRAM_CACHE="$PROGRAM_CACHE_DIR/program_binary_cache.db"
if [ -s "$PROGRAM_CACHE_SEED" ] && [ ! -s "$PROGRAM_CACHE" ]; then
  mkdir -p "$PROGRAM_CACHE_DIR"
  cp "$PROGRAM_CACHE_SEED" "$PROGRAM_CACHE"
fi

[ ! -s "$LOGFILE" ] || mv -f "$LOGFILE" "$GAMEDIR/log.prev.txt"
: >"$LOGFILE"
exec >>"$LOGFILE" 2>&1

cleanup() { command -v pm_finish >/dev/null 2>&1 && pm_finish; }
trap cleanup EXIT INT TERM

need() { [ -e "$1" ] || { echo "[missing] $1"; return 1; }; }
need "$BIN" || exit 1
need "$SDL3_LIB" || exit 1

# Locate the GameCube disc (GMPE01 Rev.0/Rev.1). Auto-pick the first image.
DVD_PATH="${PARTYBOARD_DISC:-}"
if [ -z "$DVD_PATH" ]; then
  for c in "$ASSETS_DIR"/*.rvz "$ASSETS_DIR"/*.RVZ "$ASSETS_DIR"/*.iso "$ASSETS_DIR"/*.ISO "$ASSETS_DIR"/*.gcm "$ASSETS_DIR"/*.GCM; do
    [ -f "$c" ] && { DVD_PATH="$c"; break; }
  done
fi
if [ -z "$DVD_PATH" ]; then
  echo "[missing] Place a supported Mario Party 4 (GMPE01) .rvz/.iso/.gcm in $ASSETS_DIR"
  exit 1
fi

# Seed config.json so PartyBoard boots straight into the game (skip disc-select UI).
PARTYBOARD_INTERNAL_SCALE="${PARTYBOARD_INTERNAL_SCALE:-0.50}"
PARTYBOARD_SHADOW_DIVISOR="${PARTYBOARD_SHADOW_DIVISOR:-3}"
PARTYBOARD_HEAVY_MINIGAME_SHADOW_DIVISOR="${PARTYBOARD_HEAVY_MINIGAME_SHADOW_DIVISOR:-8}"
PARTYBOARD_AVALANCHE_PARTICLE_DIVISOR="${PARTYBOARD_AVALANCHE_PARTICLE_DIVISOR:-4}"
PARTYBOARD_BLIZZARD_PARTICLE_DIVISOR="${PARTYBOARD_BLIZZARD_PARTICLE_DIVISOR:-2}"
PARTYBOARD_HEAVY_MINIGAME_REFLECTIONS="${PARTYBOARD_HEAVY_MINIGAME_REFLECTIONS:-0}"
PARTYBOARD_EXPERIMENTAL_AUDIO="${PARTYBOARD_EXPERIMENTAL_AUDIO:-1}"
export PARTYBOARD_SHADOW_DIVISOR
export PARTYBOARD_HEAVY_MINIGAME_SHADOW_DIVISOR
export PARTYBOARD_AVALANCHE_PARTICLE_DIVISOR
export PARTYBOARD_BLIZZARD_PARTICLE_DIVISOR
export PARTYBOARD_HEAVY_MINIGAME_REFLECTIONS
export PARTYBOARD_EXPERIMENTAL_AUDIO
export HOME="$RUNTIME_DIR/home"
export XDG_DATA_HOME="$RUNTIME_DIR"
export XDG_CONFIG_HOME="$RUNTIME_DIR/config"
export XDG_CACHE_HOME="$RUNTIME_DIR/cache"
# SDL_GetPrefPath("MarioPartyRD","Party Board") -> $XDG_DATA_HOME/MarioPartyRD/Party Board/
CFG="$RUNTIME_DIR/MarioPartyRD/Party Board/config.json"
mkdir -p "$(dirname "$CFG")"
cat >"$CFG" <<EOF
{
    "backend.isoPath": "$DVD_PATH",
    "backend.skipPreLaunchUI": true,
    "backend.isoVerification": 1,
    "video.enableFullscreen": true,
    "video.enableVsync": false,
    "game.internalResolutionScale": $PARTYBOARD_INTERNAL_SCALE
}
EOF

# SDL3 selection. The firmware may already ship an SDL3 with a video driver it
# validated for its own hardware; prefer it when it is complete enough to run
# us, and keep the bundled copy as the fallback. The bundled library lives in
# its own directory precisely so it can be left off the search path here.
#
# The check is the dynamic linker itself, in the mode `ldd -r` uses:
# LD_TRACE_LOADED_OBJECTS makes ld.so resolve the link map and exit without ever
# running main, and LD_WARN + LD_BIND_NOW make it report every symbol it could
# not bind. So this costs milliseconds, never reaches SDL, and never touches the
# screen. Only a symbol that fails to resolve sends us to the bundled copy.
SDL3_FALLBACK_DIR="$GAMEDIR/sdl3-fallback"
SDL3_SOURCE="bundled"
if [ -z "${PARTYBOARD_FORCE_BUNDLED_SDL3:-}" ] && [ -e /usr/lib/libSDL3.so.0 ]; then
  SDL3_PROBE="$(LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=yes \
                LD_LIBRARY_PATH="$GAMEDIR" "$BIN" 2>&1)"
  if [ -n "$SDL3_PROBE" ] && ! printf '%s' "$SDL3_PROBE" | grep -qiE "undefined symbol|not found"; then
    SDL3_SOURCE="system"
  fi
fi

if [ "$SDL3_SOURCE" = "system" ]; then
  export LD_LIBRARY_PATH="$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="$SDL3_FALLBACK_DIR:$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

# NextOS Mali-450 fbdev: both SDL3 copies carry the mali video backend.
export SDL_VIDEODRIVER=mali
export SDL_AUDIODRIVER=alsa
export SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE="${SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE:-plughw:0,0}"
export SDL_AUDIO_DEVICE_SAMPLE_FRAMES="${SDL_AUDIO_DEVICE_SAMPLE_FRAMES:-2048}"
unset ALSA_CONFIG_PATH

# Gamepad mapping: prefer PortMaster's, then the bundled Twin USB (PS2 adapter,
# 0810:0001) default so Start/face buttons map before the user configures it.
if [ -n "${sdl_controllerconfig:-}" ]; then
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
elif [ -z "${SDL_GAMECONTROLLERCONFIG:-}" ]; then
  export SDL_GAMECONTROLLERCONFIG_FILE="$GAMEDIR/gamecontrollerdb.txt"
fi

echo "[partyboard] NextOS Mali-450 GLES2"
echo "[partyboard] disc=$(basename "$DVD_PATH") scale=$PARTYBOARD_INTERNAL_SCALE shadows=1/$PARTYBOARD_SHADOW_DIVISOR heavy=1/$PARTYBOARD_HEAVY_MINIGAME_SHADOW_DIVISOR"
echo "[partyboard] heavy particles: Avalanche=1/$PARTYBOARD_AVALANCHE_PARTICLE_DIVISOR Blizzard=1/$PARTYBOARD_BLIZZARD_PARTICLE_DIVISOR reflections=$PARTYBOARD_HEAVY_MINIGAME_REFLECTIONS"
echo "[partyboard] audio buffer=$SDL_AUDIO_DEVICE_SAMPLE_FRAMES frames"
echo "[partyboard] SDL3: $SDL3_SOURCE"
chmod +x "$BIN" 2>/dev/null || true
command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$BIN"

"$BIN"
exit $?
