#!/usr/bin/env bash
# FP2-CONTROLS-LIVE — harness host dirigido do runtime vivo NEXTOSCONTROLLERS/3.
# Compila só o glue puro do port + o nxinput 0.10.1 vendorizado + o teste.
set -euo pipefail
cd "$(dirname "$0")/../.."   # ports/fp2

RED() { echo "GPTK-LIVE: RED — $1"; exit 1; }

[[ -f src/input_gptk.c && -f src/input_gptk.h ]] ||
  RED "src/input_gptk.c ausente: o binário não liga o GPTK ao runtime"
# Estático: zero teclado/keycode/SDL/evdev no caminho GPTK puro.
if grep -qE "fp2_jni_key_event|KEYCODE|SDL_SCANCODE|uinput|SDL_|/dev/input|evdev|libSDL" src/input_gptk.c; then
  RED "caminho GPTK menciona teclado/SDL/evdev — proibido"
fi
DEFAULT=generated/fp2/defaults/NEXTOSCONTROLLERS.gptk
[[ -f $DEFAULT ]] || RED "default gerado ausente ($DEFAULT): rode o nxgenerator"
grep -Fxq 'format = NEXTOS_CONTROLLERS/3' "$DEFAULT" || RED "default não é V3"
grep -Fxq 'FACE_LAYOUT = auto' "$DEFAULT" || RED "default sem FACE_LAYOUT = auto"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror \
  -Ivendor/nxinput/include -Isrc \
  vendor/nxinput/src/nxinput_gptk.c \
  vendor/nxinput/src/nxinput_gptk_loader.c \
  vendor/nxinput/src/nxinput_gptk_live.c \
  vendor/nxinput/src/nxinput_gptk_preinit.c \
  vendor/nxinput/src/nxinput_gptk_motion.c \
  src/input_gptk.c \
  tests/gptk-live/test_gptk_live.c \
  -lm -o "$WORK/gptk-live" || RED "glue GPTK não compila no host"
"$WORK/gptk-live" "$WORK" "$DEFAULT" || RED "casos dirigidos falharam"
echo "GPTK-LIVE: GREEN — 6 cenários dirigidos OK"
