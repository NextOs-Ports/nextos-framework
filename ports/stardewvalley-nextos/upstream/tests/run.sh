#!/usr/bin/env bash
set -euo pipefail

REPO=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/sdv-tests.XXXXXX")
cleanup() {
  rm -rf -- "$TMP_ROOT"
}
trap cleanup EXIT INT TERM

sh -n "$REPO/arkos/Stardew Valley (NextOS).sh"
bash -n "$REPO/run.sh"
bash -n "$REPO/nxextract-runtime-env.sh"
bash "$REPO/tests/launcher_wrapper_test.sh"
bash "$REPO/tests/launcher_permissions_test.sh"
bash "$REPO/tests/runtime_environment_test.sh"
${CC:-cc} -std=c99 -Wall -Wextra -Werror -I "$REPO/src" \
  "$REPO/tests/monogame_gl_policy_test.c" \
  -o "$TMP_ROOT/monogame-gl-policy-test"
"$TMP_ROOT/monogame-gl-policy-test"
printf 'MonoGame GL policy tests: PASS\n'
printf 'all Stardew host tests: PASS\n'
