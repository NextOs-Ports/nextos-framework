#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd -P)"
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/nxobs-mem-gate.XXXXXX")
trap 'rm -rf -- "$BUILD"' EXIT INT TERM
cc -std=c99 -Wall -Wextra -Werror -Wconversion -Wshadow \
  -I"$ROOT/framework/nxobs/include" \
  "$ROOT/framework/nxobs/src/nxobs_mem.c" \
  "$ROOT/framework/nxobs/tests/test_mem_sampler.c" \
  -o "$BUILD/test-mem-sampler"
"$BUILD/test-mem-sampler"
