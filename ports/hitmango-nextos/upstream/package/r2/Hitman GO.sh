#!/bin/sh
# NextOS Elite launcher. Keep the game in foreground so the frontend resumes
# through its normal application lifecycle after Hitman GO exits.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd -P) || exit 1
ROMS_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." 2>/dev/null && pwd -P) || exit 1
exec "$ROMS_DIR/ports/hitmango/run.sh"
