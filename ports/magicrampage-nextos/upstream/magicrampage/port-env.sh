#!/bin/bash
# Adapter-only environment. Never force SDL video/audio drivers here.
BIN="$GAMEDIR/bin/aarch64/magicrampage-nextos"
export BIN
MAGICRAMPAGE_SDL_PROVIDER=/usr/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-gbm.so
if [ -f "$MAGICRAMPAGE_SDL_PROVIDER" ] && [ ! -L "$MAGICRAMPAGE_SDL_PROVIDER" ]; then
  export MAGICRAMPAGE_SDL_PROVIDER
else
  unset MAGICRAMPAGE_SDL_PROVIDER
fi
printf '[adapter] magicrampage aarch64; inherited video/audio; PortMaster controller mapping\n'
