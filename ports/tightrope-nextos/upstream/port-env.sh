#!/bin/bash
# Adapter-only environment. Backend selection remains owned by the firmware.
export SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0

# The published 1.0.1 launcher explicitly handed the firmware controller
# database to the loader.  Keep that proven adapter contract: some ROCKNIX,
# muOS and ArkOS SDL builds do not classify the integrated pad from their
# compiled-in database alone.  The loader applies this file before opening any
# joystick; no device/CFW name chooses the mapping.
if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ]; then
  for TR_CONTROLLER_DB in \
    "${controlfolder:-}/gamecontrollerdb.txt" \
    "${controlfolder:-}/gamecontrollerdb-SDL2.txt" \
    /opt/system/Tools/PortMaster/gamecontrollerdb.txt \
    /roms/ports/PortMaster/gamecontrollerdb.txt \
    /storage/roms/ports/PortMaster/gamecontrollerdb.txt \
    /usr/share/SDL2/gamecontrollerdb.txt \
    /storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt \
    /storage/.config/SDL-GameControllerDB/gamecontrollerdb-SDL2.txt
  do
    [ -n "$TR_CONTROLLER_DB" ] && [ -f "$TR_CONTROLLER_DB" ] && \
      [ ! -L "$TR_CONTROLLER_DB" ] && [ -r "$TR_CONTROLLER_DB" ] || continue
    export SDL_GAMECONTROLLERCONFIG_FILE="$TR_CONTROLLER_DB"
    break
  done
  unset TR_CONTROLLER_DB
fi

export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
export ALSOFT_LOGLEVEL=${ALSOFT_LOGLEVEL:-0}
export TR_RELEASE_VERSION=1.0.5-test.1
BIN="$GAMEDIR/tightrope-nextos"
BIN_PRELOAD=""
