#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
GAME_DIR="${NXEXTRACT_GAME_DIR:-$SCRIPT_DIR}"
RECIPE="${NXEXTRACT_RECIPE:-$GAME_DIR/extractor.json}"
PYTHON_BIN="${NXEXTRACT_PYTHON:-python3}"
RUNTIME_HELPER="$SCRIPT_DIR/nxextract-runtime-env.sh"

if [ "${NXEXTRACT_RUNTIME_ENV_ACTIVE:-0}" != 1 ]; then
  [ -x "$RUNTIME_HELPER" ] || {
    printf 'NXExtract: runtime helper is missing or not executable: %s\n' \
      "$RUNTIME_HELPER" >&2
    exit 1
  }
  export NXEXTRACT_GAME_DIR="$GAME_DIR"
  exec "$RUNTIME_HELPER" "$SCRIPT_DIR/run-extractor.sh" "$@"
fi

exec "$PYTHON_BIN" "$SCRIPT_DIR/nxextract.py" install \
  --recipe "$RECIPE" \
  --game-dir "$GAME_DIR" \
  "$@"
