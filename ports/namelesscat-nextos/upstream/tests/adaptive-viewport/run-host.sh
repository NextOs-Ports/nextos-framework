#!/usr/bin/env bash
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT INT TERM

cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I "$PORT_DIR/src" \
  "$PORT_DIR/tests/adaptive-viewport/test_policy.c" \
  "$PORT_DIR/src/viewport_policy.c" -lm \
  -o "$WORK/test-policy"
"$WORK/test-policy"

grep -Fq 'st_input_set_screen_size(screen_width, screen_height);' \
  "$PORT_DIR/src/egl_sdl.c"
grep -Fq 'st_input_set_screen_size(width, height);' \
  "$PORT_DIR/src/egl.c"
grep -Fq 'ADAPTIVE-VIEWPORT/1 OK' "$PORT_DIR/src/input.c"
grep -Fq 'find_managed_class("", "LevelManager")' "$PORT_DIR/src/input.c"
grep -Fq 'viewport_level_class, "_instance")' "$PORT_DIR/src/input.c"
grep -Fq 'il2cpp_field_static_get_value_p(viewport_level_instance_field, &instance)' \
  "$PORT_DIR/src/input.c"
if grep -Fq 'st_find_instance(viewport_level_class)' "$PORT_DIR/src/input.c"; then
  printf 'adaptive viewport FAIL: inactive singleton hidden by FindObjectOfType\n' >&2
  exit 1
fi
grep -Fq 'find_managed_class("UnityEngine.UI", "CanvasScaler")' \
  "$PORT_DIR/src/input.c"
grep -Fq 'nc_viewport_should_probe(frame, 0)' \
  "$PORT_DIR/src/input.c"
grep -Fq 'if (st_input_init() != 0)' "$PORT_DIR/src/main.c"
if sed -n '/int st_input_init(void)/,/void st_input_poll/p' \
     "$PORT_DIR/src/input.c" | grep -Fq 'viewport_resolve_contract()'; then
  printf 'adaptive viewport FAIL: managed contract resolved before frame zero\n' >&2
  exit 1
fi

printf 'ADAPTIVE VIEWPORT WIRING PASS\n'
