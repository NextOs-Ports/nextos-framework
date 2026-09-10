#!/bin/bash
set -euo pipefail

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PORT_DIR=$(CDPATH= cd -- "$TEST_DIR/../.." && pwd -P)
WORK_DIR=$(mktemp -d)
trap 'rm -rf -- "$WORK_DIR"' EXIT

GAMEDIR="$WORK_DIR/tearscape"
export GAMEDIR
mkdir -p -- "$GAMEDIR/lib" "$GAMEDIR/game/addons/crt"
: > "$GAMEDIR/lib/libEGL.so"
: > "$GAMEDIR/lib/libGLESv2.so"
printf '%s\n' \
  'shader_type canvas_item;' \
  'uniform sampler2D SCREEN_TEXTURE: hint_screen_texture;' \
  'void fragment() { COLOR = texture(SCREEN_TEXTURE, UV); }' \
  > "$GAMEDIR/game/addons/crt/crt.gdshader"

# Model an owner save whose menu switch remains ON. Port-env must preserve
# settings while making the extracted CRT material inert.
mkdir -p -- "$GAMEDIR/.local/share/Tearscape"
printf '%s\n' 'IsCrtEnabled=true' > "$GAMEDIR/.local/share/Tearscape/settings.fixture"
(
  . "$PORT_DIR/adapter-env.sh"
  [ "$NX_TEARSCAPE_SCREEN_MIP_EMULATION" = 0 ]
  [ "$(pwd -P)" = "$GAMEDIR/game" ]
)

expected=$'// Tearscape/NextOS: CRT intentionally disabled.\nshader_type canvas_item;\n\nvoid fragment() {\n    discard;\n}'
[ "$(<"$GAMEDIR/game/addons/crt/crt.gdshader")" = "$expected" ]
[ "$(<"$GAMEDIR/.local/share/Tearscape/settings.fixture")" = 'IsCrtEnabled=true' ]
if grep -Eq 'hint_screen_texture|SCREEN_TEXTURE|texture[[:space:]]*\(' \
     "$GAMEDIR/game/addons/crt/crt.gdshader"; then
  printf 'TEARSCAPE CRT DISABLED: FAIL: screen-texture path survived\n' >&2
  exit 1
fi
printf 'TEARSCAPE CRT DISABLED: PASS menu=on effect=no-op screen-mip=off\n'
