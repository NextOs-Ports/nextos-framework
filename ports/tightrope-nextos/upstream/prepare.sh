#!/bin/bash
# Tightrope-only preparation after NXExtract and before the framework splash.
set -e

GAMEDIR=${NXCOMPAT_GAME_DIR:-${NXEXTRACT_GAME_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)}}
mkdir -p "$GAMEDIR/home" "$GAMEDIR/gamedata" "$GAMEDIR/lib"
