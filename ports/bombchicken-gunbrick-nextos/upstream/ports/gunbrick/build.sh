#!/usr/bin/env bash
# Compatibility entry point: every public build uses the low-glibc recipe.
set -euo pipefail
PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$PORT_DIR/build_universal.sh" "$@"
