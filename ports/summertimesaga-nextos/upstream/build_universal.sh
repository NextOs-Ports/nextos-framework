#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
"$SCRIPT_DIR/build.sh"
"$SCRIPT_DIR/build_r36s.sh"
