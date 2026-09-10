#!/usr/bin/env bash
# Compatibility entry point retained for existing recipes.
set -euo pipefail
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$SCRIPT_DIR/build_universal.sh" "$@"
