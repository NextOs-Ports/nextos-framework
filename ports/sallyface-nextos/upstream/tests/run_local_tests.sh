#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Gates locais do port. Os modulos de runtime compilados junto ao loader estao
# vendorizados em vendor/ neste mesmo repositorio, entao os testes rodam sem
# nenhuma arvore externa.
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/sallyface-tests.XXXXXX")
trap 'rm -rf -- "$TEST_TMP"' EXIT
NXINPUT_DIR=$PORT_DIR/vendor/nxinput
NXGL_DIR=$PORT_DIR/vendor/nxgl
NXCOMPAT_DIR=$PORT_DIR/vendor/nxcompat

cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I "$PORT_DIR/src" -I "$NXCOMPAT_DIR/include" -I "$NXGL_DIR/include" \
  "$PORT_DIR/tests/test_quality.c" \
  "$PORT_DIR/src/quality.c" \
  "$NXCOMPAT_DIR/src/nxcompat_settings.c" \
  "$NXGL_DIR/src/nxgl_quality.c" \
  -o "$TEST_TMP/test_quality"
"$TEST_TMP/test_quality"

cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I "$PORT_DIR/src" \
  "$PORT_DIR/tests/test_etc1.c" "$PORT_DIR/src/etc1.c" \
  -o "$TEST_TMP/test_etc1"
"$TEST_TMP/test_etc1"

cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror \
  -I "$PORT_DIR/src" \
  "$PORT_DIR/tests/test_essl1.c" "$PORT_DIR/src/essl1.c" \
  -o "$TEST_TMP/test_essl1"
"$TEST_TMP/test_essl1" "$TEST_TMP"
if command -v glslangValidator >/dev/null 2>&1; then
  glslangValidator -S vert "$TEST_TMP/unity-alias.vert" >/dev/null
  glslangValidator -S frag "$TEST_TMP/unity-alias.frag" >/dev/null
  printf '%s\n' 'ESSL100 Unity alias fixtures: OK'
else
  printf '%s\n' 'ESSL100 Unity alias fixtures: glslangValidator unavailable (syntax check skipped)'
fi

cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I "$PORT_DIR/src" \
  "$PORT_DIR/tests/test_display_contract.c" \
  "$PORT_DIR/src/display_contract.c" \
  -o "$TEST_TMP/test_display_contract"
"$TEST_TMP/test_display_contract"

cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I "$PORT_DIR/src" -I "$NXINPUT_DIR/include" \
  "$PORT_DIR/tests/test_gptk_adapter.c" \
  "$PORT_DIR/src/gptk_adapter.c" \
  "$NXINPUT_DIR/src/nxinput_gptk.c" \
  "$NXINPUT_DIR/src/nxinput_gptk_motion.c" \
  -lm -o "$TEST_TMP/test_gptk_adapter"
"$TEST_TMP/test_gptk_adapter"

PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s "$PORT_DIR/tests" -p 'test_*.py' -v

printf 'sallyface local gates: OK\n'
