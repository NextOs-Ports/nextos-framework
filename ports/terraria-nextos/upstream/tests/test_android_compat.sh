#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/terraria-android-compat.XXXXXX")
trap 'rm -rf -- "$BUILD"' EXIT INT TERM

${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/src/android_compat.c" "$ROOT/tests/test_android_compat.c" \
  -o "$BUILD/test-android-compat"
"$BUILD/test-android-compat"
