#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
OUTPUT=${1:-"$ROOT/package/dist/forager.zip"}

if [[ -n "${NXRELEASE_TOOL:-}" ]]; then
  RELEASE_TOOL=$NXRELEASE_TOOL
elif [[ -f "$ROOT/../nextos_ports_android/framework/nxrelease/nxrelease.py" ]]; then
  RELEASE_TOOL="$ROOT/../nextos_ports_android/framework/nxrelease/nxrelease.py"
else
  printf 'package error: set NXRELEASE_TOOL to nxrelease.py 0.2.6 or newer\n' >&2
  exit 1
fi

[[ -f "$RELEASE_TOOL" ]] || {
  printf 'package error: NXRelease tool not found\n' >&2
  exit 1
}
[[ ! -e "$OUTPUT" && ! -e "$OUTPUT.sha256" ]] || {
  printf 'package error: output already exists: %s\n' "$OUTPUT" >&2
  exit 1
}

mkdir -p -- "$(dirname -- "$OUTPUT")"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/forager-release.XXXXXX")
cleanup() {
  rm -rf -- "$WORK"
}
trap cleanup EXIT INT TERM

python3 "$RELEASE_TOOL" build \
  --manifest "$ROOT/nxrelease.json" \
  --stage "$WORK/stage" \
  --output "$OUTPUT"
