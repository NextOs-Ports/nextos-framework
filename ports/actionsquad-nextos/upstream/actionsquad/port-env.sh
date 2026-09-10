#!/bin/bash
# Adapter-only environment. Preserve firmware/PortMaster providers first.
BIN="$GAMEDIR/bin/aarch64/actionsquad-nextos"
export BIN
export AS_ROOT="$GAMEDIR"
export ALSOFT_CONF="$GAMEDIR/alsoft.conf"

ACTIONSQUAD_SDL_PROVIDER=/usr/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-gbm.so
if [ -f "$ACTIONSQUAD_SDL_PROVIDER" ] && [ ! -L "$ACTIONSQUAD_SDL_PROVIDER" ]; then
  export ACTIONSQUAD_SDL_PROVIDER
else
  unset ACTIONSQUAD_SDL_PROVIDER
fi
printf '[adapter] actionsquad aarch64; inherited video/audio; PortMaster controller mapping\n'
