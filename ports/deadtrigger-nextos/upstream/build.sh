#!/usr/bin/env bash
# Canonical public build: always use the pinned low-glibc recipe.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
exec "$PORT_DIR/build_universal.sh" "$@"
