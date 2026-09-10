#!/usr/bin/env bash
# Import a user-owned Android PF2 save placed in gamedata/owned-android-save.
set -euo pipefail

GAME_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" \
  2>/dev/null && pwd -P) || exit 1
SOURCE=${1:-$GAME_DIR/gamedata/owned-android-save}

exec python3 "$GAME_DIR/tools/import_owned_android_save.py" \
  --source "$SOURCE" \
  --game-dir "$GAME_DIR"
