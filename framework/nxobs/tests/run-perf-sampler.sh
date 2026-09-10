#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V3-PERF-01 / VSYNC-OBS host gate. Hermetic: the /proc sources are fixtures,
# so no device is required and the absent-field path is really exercised.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd -P)"
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/nxobs-perf-gate.XXXXXX")
trap 'rm -rf -- "$BUILD"' EXIT INT TERM
compilers=("${CC:-cc}")
if command -v clang >/dev/null 2>&1 && [ "${CC:-cc}" != clang ]; then
  compilers+=(clang)
fi
for compiler in "${compilers[@]}"; do
  "$compiler" -std=c99 -D_POSIX_C_SOURCE=200809L \
    -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes \
    -Wconversion -Wsign-conversion -Wcast-qual \
    -I"$ROOT/framework/nxobs/include" \
    "$ROOT/framework/nxobs/src/nxobs_perf.c" \
    "$ROOT/framework/nxobs/tests/test_perf_sampler.c" \
    -o "$BUILD/test-perf-sampler-$compiler"
  "$BUILD/test-perf-sampler-$compiler"
done
# The frame path must stay free of I/O and logging by construction, not by
# hope: per-frame spam fails QA and per-frame /proc reads burn the SD card.
present_body=$(sed -n '/^void nxobs_perf_present/,/^}/p' \
  "$ROOT/framework/nxobs/src/nxobs_perf.c")
for forbidden in fopen opendir snprintf 'perf->emit' malloc; do
  if grep -Fq -- "$forbidden" <<< "$present_body"; then
    printf 'nxobs_perf: the frame path calls %s\n' "$forbidden" >&2
    exit 1
  fi
done
# Observation never authorizes action.
for forbidden in 'kill(' setrlimit malloc_trim 'unlink(' 'remove(' 'nice('; do
  if grep -Fq -- "$forbidden" "$ROOT/framework/nxobs/src/nxobs_perf.c"; then
    printf 'nxobs_perf: the sampler acts instead of observing (%s)\n' \
      "$forbidden" >&2
    exit 1
  fi
done
printf 'NXOBS PERF GATE: PASS\n'
