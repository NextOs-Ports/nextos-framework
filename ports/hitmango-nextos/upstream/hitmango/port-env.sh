#!/bin/bash
# Hitman GO adapter environment. Video and audio backends stay firmware-owned.
BIN="$GAMEDIR/bin/aarch64/hitmango-nextos"
export BIN

export HGO_GAMEDIR="$GAMEDIR"
export HGO_CURSOR="${HGO_CURSOR:-1}"
export HGO_CLICK_A="${HGO_CLICK_A:-1}"
export HGO_SWAP_STICKS="${HGO_SWAP_STICKS:-1}"
export HGO_SWIPE_MOVE="${HGO_SWIPE_MOVE:-1}"
export SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0

# Some CFWs publish a database file instead of one SDL mapping string.
if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && \
   [ -n "${controlfolder:-}" ]; then
  for hgo_mapping_db in \
    "$controlfolder/gamecontrollerdb.txt" \
    "$controlfolder/gamecontrollerdb-SDL2.txt"; do
    if [ -f "$hgo_mapping_db" ] && [ ! -L "$hgo_mapping_db" ] && \
       [ -r "$hgo_mapping_db" ]; then
      export SDL_GAMECONTROLLERCONFIG_FILE="$hgo_mapping_db"
      break
    fi
  done
  unset hgo_mapping_db
fi

printf '[adapter] hitmango aarch64; cursor=left/A movement=right+dpad touch-owner=single\n'
