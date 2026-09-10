#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Compatibility entry point. The universal, reproducible recipe is the only
# supported build path for the promoted public port.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$PORT_DIR/build_universal.sh" "$@"
