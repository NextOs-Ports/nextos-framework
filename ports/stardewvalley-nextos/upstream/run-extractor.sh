#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
GAME_DIR="${NXEXTRACT_GAME_DIR:-$SCRIPT_DIR}"
RECIPE="${NXEXTRACT_RECIPE:-$GAME_DIR/extractor.json}"
PYTHON_BIN="${NXEXTRACT_PYTHON:-python3}"

# NXExtract normally adopts an already-valid installation before looking for
# source packages.  Older Stardew packages predate its marker, so prepare and
# verify that active store first; this migrates the controller patch without
# touching assets or saves and lets NXExtract adopt it without requiring the
# user's APK again.  A rejected legacy store falls through to normal source
# discovery so a valid APK can repair it transactionally.
if [ -f "$GAME_DIR/libs/libassemblies.arm64-v8a.blob.so" ] &&
   [ -d "$GAME_DIR/assets/Content" ] &&
   [ -x "$GAME_DIR/tools/prepare_stardew_data.py" ]; then
  if ! "$PYTHON_BIN" "$GAME_DIR/tools/prepare_stardew_data.py" \
      --stage "$GAME_DIR"; then
    printf '%s\n' \
      'Stardew Valley: existing data needs a supported 1.6.15.3 APK.' >&2
  fi
fi

exec "$PYTHON_BIN" "$SCRIPT_DIR/nxextract.py" install \
  --recipe "$RECIPE" \
  --game-dir "$GAME_DIR" \
  "$@"
