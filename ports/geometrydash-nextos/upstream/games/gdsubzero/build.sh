#!/usr/bin/env bash
# Single-game build wrapper for nx-ship-port: build gdsubzero from the repo's
# universal builder and place the loader next to this launcher as `gdsubzero`.
set -euo pipefail
PORT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_DIR=$(CDPATH= cd -- "$PORT_DIR/../.." && pwd -P)
( cd "$REPO_DIR" && ./build_universal.sh gdsubzero )
cp -f "$REPO_DIR/gdsubzero-universal" "$PORT_DIR/gdsubzero"
chmod +x "$PORT_DIR/gdsubzero"
printf 'GDSUBZERO BUILD OK -> %s/gdsubzero\n' "$PORT_DIR"
