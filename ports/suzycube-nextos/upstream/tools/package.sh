#!/usr/bin/env bash
# Compatibility entry point for contributors; the canonical NXRelease pipeline
# is the only code allowed to assemble a public BYO-data package.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
exec "$PORT_DIR/package/build-package.sh" "$@"
