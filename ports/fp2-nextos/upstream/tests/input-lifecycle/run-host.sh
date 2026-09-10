#!/usr/bin/env bash
# FP2-INPUT-LIFECYCLE — gate estático das fronteiras de input (1.1.2).
set -euo pipefail
cd "$(dirname "$0")/../.."

RED() { echo "INPUT-LIFECYCLE: RED — $1"; exit 1; }

gcc -std=gnu11 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-unused-parameter -Wno-unused-function \
  -I/usr/include/SDL2 -Isrc -Ivendor/nxinput/include \
  -Ivendor/nxinput/engine-glue src/input.c src/input_gptk.c ||
  RED "input.c/input_gptk.c não compilam no host"
gcc -std=gnu11 -fsyntax-only -DFP2_RELEASE_BUILD=1 -Wall -Wextra -Werror \
  -Wno-unused-parameter -Wno-unused-function \
  -I/usr/include/SDL2 -Isrc -Ivendor/nxinput/include \
  -Ivendor/nxinput/engine-glue src/input.c ||
  RED "input.c não compila em modo release"

# Autoridades falsas eliminadas.
for pat in raw_joystick SDL_JoystickOpen SDL_JoystickGetButton SDL_JoystickGetAxis \
           SDL_JoystickGetHat /dev/input TRIGGER_HAPPY evdev_key_rank Hitman \
           Stranger 'Bomb Chicken' X-Box FP2_CLICK_A FP2_SWAP_STICKS FP2_B_IS_BACK \
           FP2_GPVIRT FP2_CURSOR FP2_RESUME_XY 'FP2_SHOT"' il2cpp_string_ascii \
           'linux/input.h' SDL_CONTROLLER_BUTTON_GUIDE; do
  if grep -qF -- "$pat" src/input.c; then
    RED "src/input.c ainda contém autoridade proibida: $pat"
  fi
done
if grep -qE '\b(st|nc)_[a-z]' src/input.c src/input_gptk.c src/input_gptk.h; then
  RED "prefixos de outro port (st_/nc_) sobraram"
fi
# Chord soberano pelo framework, sem GUIDE.
grep -qF 'nxinput_exit_chord_update(&exit_chord,' src/input.c ||
  RED "chord de saída não usa nxinput_exit_chord_update"
grep -qF 'nxinput_exit_chord_fold_signal' src/input.c ||
  RED "SIGTERM não converge no chord"
grep -qF 'nxinput_padset_chord_inputs(&padset, &chord_select, &chord_start)' src/input.c ||
  RED "chord não vem do nxinput_padset (mesmo instance)"
grep -qF 'nxinput_exit_chord_update(&exit_chord, chord_select, chord_start)' src/input.c ||
  RED "chord de saída não consome as entradas do padset"
grep -qF 'nxinput_padset_open_all(&padset, padset_admit, padset_opened, NULL)' src/input.c ||
  RED "pads admitidos não abrem pelo nxinput_padset"
if grep -qE 'controller = SDL_GameControllerOpen\(i\);' src/input.c; then
  RED "abertura de um único controller fora do padset"
fi
# Caminho nativo dirigido por estado.
grep -qF 'static uint8_t key_down_state[256]' src/input.c ||
  RED "tabela key_down_state ausente"
grep -qF 'release_all_keys()' src/input.c || RED "release_all_keys ausente"
grep -qF 'sink_key_pressed[keycode]' src/input.c || RED "guarda de keycode de sink ausente"
# Pré-init antes do vídeo e status fatal no main.
grep -qF 'fp2_input_preinit()' src/main.c || RED "main.c não chama fp2_input_preinit"
grep -qF '_exit(fp2_video_fatal ? 72 : fp2_input_fatal() ? 70 : 0)' src/main.c || RED "main.c ignora fatal do runtime/vídeo"
grep -qF 'nxgl_frame_proof_consume_fatal()' src/main.c || RED "main.c não consome o fatal de vídeo"
python3 - <<'PY'
from pathlib import Path
m = Path("src/main.c").read_text()
assert m.index("fp2_input_preinit()") < m.index("fp2_egl_init();"), "preinit depois do vídeo"
i = Path("src/input.c").read_text()
assert "radial_deadzone(&lx, &ly)" in i and "radial_deadzone(&rx, &ry)" in i
assert "hy = (float)(dn - up)" in i and "ax = clampf" not in i, "D-pad não pode entrar em X/Y no FP2"
assert "fp2_gptk_should_consume(NXINPUT_GPTK_L2)" in i, "gatilho nativo não consulta a decisão"
assert "il2_load()" in i and "frame == 0" in i, "contexto de cena sem guarda de frame 0"
assert "SDL_WINDOWEVENT_FOCUS_LOST" in i, "perda de foco não solta"
import re
envs = set(re.findall(r'getenv\("(FP2_[A-Z_]+)"\)', i))
assert envs <= {"FP2_INPUT_DIAG", "FP2_VPAD", "FP2_VPAD_FILE"}, envs
assert "vpad_poll();" in i and "(controller || vpad_enabled)" in i
print("INPUT-LIFECYCLE: GREEN — autoridade única, chord soberano, estado de teclas, pré-init")
PY

grep -qF "nxinput_exit_chord_init(&exit_chord, 1);" src/input.c || RED "chord precisa disparar no primeiro poll"
