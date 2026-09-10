#!/usr/bin/env bash
# NAMELESSCAT-CONTROLS-LIVE 1.2.6 — harness host dirigido do runtime vivo.
# Um processo por cenário (o módulo é one-shot por processo, como no jogo).
# Não é bateria completa: compila só o glue + gptk vendorizado + o teste.
set -euo pipefail
cd "$(dirname "$0")/../.."   # ports/namelesscat

RED() { echo "GPTK-LIVE: RED — $1"; exit 1; }

[[ -f src/input_gptk.c && -f src/input_gptk.h ]] ||
  RED "src/input_gptk.c ausente: o binário não liga o GPTK ao runtime"

# Estático: zero teclado sintético, SDL, evdev ou nome de aparelho no glue.
if grep -qE "st_jni_key_event|KEYCODE|SDL_SCANCODE|uinput" src/input_gptk.c; then
  RED "caminho GPTK menciona teclado/keycode — proibido"
fi
if grep -qE "SDL_|/dev/input|evdev|libSDL|VID|PID|Deeplay|X-Box" src/input_gptk.c; then
  RED "caminho GPTK depende de SDL/evdev/identidade — deve ser puro nxinput"
fi
# O runtime vivo canônico é a fronteira (não o dispatcher antigo).
grep -q "nxinput_gptk_live_seal" src/input_gptk.c || RED "runtime vivo não selado no glue"
grep -q "nxinput_gptk_preinit_load" src/input_gptk.c || RED "pré-init canônico ausente"
grep -q "nxinput_gptk_runtime_marker()" src/input_gptk.c || RED "marcador por código vivo ausente"
grep -q "nxinput_gptk_dispatcher_" src/input_gptk.c && RED "dispatcher legado não pode coexistir"

DEFAULT=generated/namelesscat/defaults/NEXTOSCONTROLLERS.gptk
[[ -f $DEFAULT ]] || RED "default gerado ausente ($DEFAULT): rode o nxgenerator"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

gcc -std=c11 -O1 -g -Wall -Wextra -Werror \
  -Ivendor/nxinput/include -Isrc \
  vendor/nxinput/src/nxinput_gptk.c \
  vendor/nxinput/src/nxinput_gptk_loader.c \
  vendor/nxinput/src/nxinput_gptk_motion.c \
  vendor/nxinput/src/nxinput_gptk_preinit.c \
  vendor/nxinput/src/nxinput_gptk_live.c \
  src/input_gptk.c \
  tests/gptk-live/test_gptk_live.c \
  -lm -o "$WORK/gptk-live" ||
  RED "glue GPTK não compila no host"

CASES=(default owner-remap owner-invalid start-latch unknown-context missing-sink ack-fatal evidence no-map)
for c in "${CASES[@]}"; do
  mkdir -p "$WORK/$c"
  "$WORK/gptk-live" "$c" "$WORK/$c" "$DEFAULT" || RED "cenário $c falhou"
done

echo "GPTK-LIVE: GREEN — ${#CASES[@]} cenários dirigidos OK"
