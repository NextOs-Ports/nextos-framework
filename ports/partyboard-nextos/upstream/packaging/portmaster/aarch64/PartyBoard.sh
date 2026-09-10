#!/bin/bash
# PartyBoard — PortMaster launcher (SDL2-shim variant).
#
# Mirrors the Dusklight PortMaster recipe: our SDL3 runs its "sdl2" video
# driver, which dlopens the firmware's libSDL2 and hands aurora the borrowed
# EGL display/context. Two things differ from the NextOS launcher and both
# matter:
#   * no LD_PRELOAD of libSDL3 — preloading it puts SDL3 ahead of everything in
#     the global scope, so the dlopen'd libSDL2's own internal SDL_* calls bind
#     back into SDL3's shim and recurse until the stack blows.
#   * no forced SDL_AUDIODRIVER — audio is delegated to the firmware SDL2,
#     which already knows this device's output.

PORTNAME="PartyBoard"

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/mnt/mmc/MUOS/PortMaster/" ]; then
  controlfolder="/mnt/mmc/MUOS/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"
[ -n "${CFW_NAME:-}" ] && [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
command -v get_controls >/dev/null 2>&1 && get_controls

directory=${directory:-roms}
DEVICE_ARCH="${DEVICE_ARCH:-aarch64}"

GAMEDIR="/$directory/ports/partyboard"
RUNTIME_DIR="$GAMEDIR/runtime"
ASSETS_DIR="$GAMEDIR/assets"
BIN="$GAMEDIR/partyboard.${DEVICE_ARCH}"
LOGFILE="$GAMEDIR/log.txt"

mkdir -p "$RUNTIME_DIR/home" "$RUNTIME_DIR/config" "$RUNTIME_DIR/cache" "$ASSETS_DIR"
cd "$GAMEDIR" || exit 1
ulimit -c 0

[ ! -s "$LOGFILE" ] || mv -f "$LOGFILE" "$GAMEDIR/log.prev.txt"
: >"$LOGFILE"
exec >>"$LOGFILE" 2>&1

GPTOKEYB_PID=""
cleanup() {
  if [ -n "$GPTOKEYB_PID" ]; then
    kill "$GPTOKEYB_PID" 2>/dev/null || true
    wait "$GPTOKEYB_PID" 2>/dev/null || true
  fi
  command -v pm_finish >/dev/null 2>&1 && pm_finish
}
trap cleanup EXIT INT TERM

need() { [ -e "$1" ] || { echo "[missing] $1"; return 1; }; }
need "$BIN" || exit 1
need "$GAMEDIR/lib.${DEVICE_ARCH}/libSDL3.so.0" || exit 1

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

export LD_LIBRARY_PATH="$GAMEDIR/lib.${DEVICE_ARCH}:$GAMEDIR/libs.${DEVICE_ARCH}:$GAMEDIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_VIDEODRIVER=sdl2
export SDL3SHIM_SDL2_LIB="${SDL3SHIM_SDL2_LIB:-libSDL2-2.0.so.0}"

if [ -n "${sdl_controllerconfig:-}" ]; then
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
elif [ -z "${SDL_GAMECONTROLLERCONFIG:-}" ]; then
  export SDL_GAMECONTROLLERCONFIG_FILE="$GAMEDIR/gamecontrollerdb.txt"
fi

# Native SDL gamepad input stays authoritative; gptokeyb2 only provides
# PortMaster's emergency quit combo (the mapping file has no gameplay keys).
if [ -x "$controlfolder/gptokeyb2" ]; then
  env LD_PRELOAD="$controlfolder/libinterpose.${DEVICE_ARCH}.so" \
    "$controlfolder/gptokeyb2" "partyboard.${DEVICE_ARCH}" -c "$GAMEDIR/partyboard.gptk" -Z -H select &
  GPTOKEYB_PID=$!
elif [ -x "$controlfolder/gptokeyb" ]; then
  "$controlfolder/gptokeyb" "${ESUDOKILL:--1}" "partyboard.${DEVICE_ARCH}" -c "$GAMEDIR/partyboard.gptk" &
  GPTOKEYB_PID=$!
fi

echo "[partyboard] PortMaster / SDL2-shim (GLES2)"
echo "[partyboard] disc=$(basename "$DVD_PATH") scale=$PARTYBOARD_INTERNAL_SCALE"
echo "[partyboard] SDL_VIDEODRIVER=$SDL_VIDEODRIVER SDL3SHIM_SDL2_LIB=$SDL3SHIM_SDL2_LIB"
chmod +x "$BIN" 2>/dev/null || true
command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$BIN"

"$BIN"
exit $?
