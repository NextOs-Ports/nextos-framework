#!/bin/bash
set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

python3 -B "$ROOT/tests/test_public_docs.py"

${CC:-cc} -std=c99 -Wall -Wextra -Werror \
  -I"$ROOT/src" \
  "$ROOT/src/input_adapter.c" "$ROOT/tests/test_input_adapter.c" \
  -o "$BUILD_DIR/test-input-adapter"
"$BUILD_DIR/test-input-adapter"

python3 - "$ROOT" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
source = (root / "src/input.c").read_text(encoding="utf-8")
bionic = (root / "src/bionic.c").read_text(encoding="utf-8")
port_env = (root / "port-env.sh").read_text(encoding="utf-8")

assert "cursor_menu_mode" not in source, "heuristic cursor suppression returned"
assert 'getenv("SDL_GAMECONTROLLERCONFIG_FILE")' in source
assert "SDL_GameControllerAddMappingsFromFile" in source
assert "SDL_INIT_JOYSTICK" in source and "SDL_INIT_EVENTS" in source
assert "SDL_JOYDEVICEADDED" in source
assert "system SDL sees %d joystick(s)" in source
assert "SDL_GameControllerHasButton(" not in source, "SDL floor regressed above 2.0.4"
assert "tr_input_build_raw_mapping" in source
assert "tr_input_pick_exit_codes" in source
assert "merged[SDL_CONTROLLER_BUTTON_GUIDE]" in source
assert source.index("exit_fallback_pressed()") < source.index("if (!pad_n || !registered)")
assert "SDL_GAMECONTROLLERCONFIG_FILE" in port_env
for path in (
    "/opt/system/Tools/PortMaster/gamecontrollerdb.txt",
    "/roms/ports/PortMaster/gamecontrollerdb.txt",
    "/usr/share/SDL2/gamecontrollerdb.txt",
):
    assert path in port_env, f"approved controller DB path missing: {path}"
assert "CFW_NAME" not in port_env and "DEVICE" not in port_env
assert "E(reallocarray)" not in bionic, "public ELF would import glibc reallocarray"
print("input integration contract: OK")
PY
