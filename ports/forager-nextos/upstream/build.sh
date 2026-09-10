#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec "$ROOT/source/build-release.sh" "${1:-$ROOT/forager-nextos}"
