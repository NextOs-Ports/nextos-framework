#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

export LC_ALL=C
MAX_MAJOR=2
MAX_MINOR=30

if [ "$#" -eq 0 ]; then
  echo "usage: check-glibc.sh ELF [...]" >&2
  exit 2
fi

command -v readelf >/dev/null 2>&1 || {
  echo "GLIBC GATE FAILED: readelf is unavailable" >&2
  exit 1
}
READELF=readelf

failed=0
for binary in "$@"; do
  if [ -L "$binary" ]; then
    echo "GLIBC GATE FAILED: symbolic link is not a release ELF: $binary" >&2
    failed=1
    continue
  fi
  if [ ! -f "$binary" ]; then
    echo "GLIBC GATE FAILED: missing regular file: $binary" >&2
    failed=1
    continue
  fi
  if [ ! -s "$binary" ]; then
    echo "GLIBC GATE FAILED: empty file: $binary" >&2
    failed=1
    continue
  fi
  link_count=$(stat -c '%h' -- "$binary" 2>/dev/null || printf 'unknown')
  if [ "$link_count" != 1 ]; then
    echo "GLIBC GATE FAILED: hard-linked or uninspectable file ($link_count links): $binary" >&2
    failed=1
    continue
  fi
  if ! header="$("$READELF" -h "$binary" 2>/dev/null)"; then
    echo "GLIBC GATE FAILED: not an ELF file: $binary" >&2
    failed=1
    continue
  fi

  versions="$(
    "$READELF" --version-info "$binary" 2>/dev/null |
      grep -oE 'GLIBC_[0-9]+\.[0-9]+' |
      sed 's/GLIBC_//' |
      sort -Vu || true
  )"
  maximum="$(printf '%s\n' "$versions" | sed '/^$/d' | tail -n 1)"
  if [ -z "$maximum" ]; then
    echo "GLIBC GATE OK: $binary has no dynamic GLIBC requirement"
    continue
  fi

  major="${maximum%%.*}"
  minor="${maximum#*.}"
  minor="${minor%%.*}"
  if [ "$major" -gt "$MAX_MAJOR" ] ||
     { [ "$major" -eq "$MAX_MAJOR" ] && [ "$minor" -gt "$MAX_MINOR" ]; }; then
    echo "GLIBC GATE FAILED: $binary needs GLIBC_$maximum (> 2.30)" >&2
    failed=1
  else
    echo "GLIBC GATE OK: $binary needs GLIBC_$maximum (<= 2.30)"
  fi
done

exit "$failed"
