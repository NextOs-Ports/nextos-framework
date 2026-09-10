#!/usr/bin/env bash
# NAMELESSCAT-INPUT-LIFECYCLE 1.2.6 — regressões estáticas das fronteiras de
# input do adapter (o que o harness puro do GPTK não enxerga).
set -euo pipefail

cd "$(dirname "$0")/../.."

gcc -std=gnu11 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-unused-parameter -Wno-unused-function \
  -I/usr/include/SDL2 -Isrc -Ivendor/nxinput/include \
  -Ivendor/nxinput/engine-glue -Ivendor/nxgl/adapters src/input.c src/input_gptk.c

python3 - <<'PY'
import re
from pathlib import Path

src = Path("src/input.c").read_text()
gptk = Path("src/input_gptk.c").read_text()
main = Path("src/main.c").read_text()
build = Path("build_universal.sh").read_text()

def forbid(pattern, why, text=src):
    hits = [m.start() for m in re.finditer(pattern, text, re.M)]
    assert not hits, f"PROIBIDO ({why}): {pattern!r}"

def require(pattern, why, text=src, count=1):
    n = len(re.findall(pattern, text, re.M))
    assert n >= count, f"OBRIGATÓRIO ({why}): {pattern!r} ({n} < {count})"

# Autoridades falsas eliminadas
for pat, why in [
    (r"SDL_JoystickOpen|SDL_JoystickGetButton|SDL_JoystickGetAxis|SDL_JoystickGetHat|raw_joystick", "joystick cru/posicional"),
    (r"swap_ab|swap_sticks|known_inverted|0x484b|click_uses_a", "segunda troca A/B, heurística VID/PID, A-clique"),
    (r"/dev/input|EVIOCG|BTN_TRIGGER_HAPPY|evdev", "varredura própria de evdev"),
    (r"X-Box 360|Xbox|InControl|Rewired|tvOS|Hitman|Stranger|Bomb Chicken|bcgp|bcshot", "identidade falsa e herança de outros ports"),
    (r"replace_body|il2cpp_base|ST_TVOS|ST_GET_JOYSTICK|0x[0-9a-f]{7}u", "RVA/hook binário de outro jogo"),
    (r"SDL_CONTROLLER_BUTTON_GUIDE\]\s*\|\||GUIDE\s*\|\|", "GUIDE no chord de saída"),
    (r"nxinput_gptk_dispatcher_", "dispatcher legado"),
    (r"SDL_SCANCODE|uinput|KEYCODE_ESCAPE|KEYCODE_ENTER|\b(27|13|66|111)\b\s*/\*\s*KEYCODE", "teclado sintético"),
]:
    forbid(pat, why)
forbid(r"ST_GPVIRT|ST_CLICK_A|ST_SWAP|ST_B_IS_BACK|ST_TRIGGER_KEYS|ST_NATIVE_CONTROLS|ST_SWIPE", "envs herdadas")

# Chord soberano: só SELECT+START lógicos, pelo nxinput, antes do GPTK
require(r"nxinput_exit_chord_update\(&exit_chord, chord_select, chord_start\)", "chord SELECT+START pelo nxinput (entradas do padset, mesmo instance)")
require(r"nxinput_padset_chord_inputs\(&padset, &chord_select, &chord_start\)", "chord só do mesmo instance (nxinput_padset)")
require(r"nxinput_padset_open_all\(&padset, padset_admit, padset_opened, NULL\)", "todos os pads admitidos abrem (padset)")
forbid(r"controller = SDL_GameControllerOpen\(i\);", "abertura de um único controller fora do padset")
require(r"nxinput_exit_chord_fold_signal", "SIGTERM converge no chord")
require(r"nxinput_exit_chord_init\(&exit_chord, 1\);", "chord instantâneo (1 poll), sem vazar START")
chord_at = src.index("nxinput_exit_chord_update(&exit_chord")
feed_at = src.index("nc_gptk_feed_button(c, control_down[c], value)")
assert chord_at < feed_at, "o chord precisa ser consultado ANTES do feed GPTK"

# Caminho nativo dirigido por estado: todo DOWN recebe UP
require(r"static uint8_t key_down_state\[256\]", "tabela de teclas entregues")
require(r"if \(\(key_down_state\[keycode\] != 0\) == \(down != 0\)\)", "deliver_key deduplica por estado")
require(r"int desired = control_down\[c\] && !nc_gptk_should_consume\(c\);", "desejado = pressionado && não consumido")
require(r"release_all_keys\(\)", "release de todas as teclas", count=5)
require(r"SDL_WINDOWEVENT_FOCUS_LOST", "perda de foco solta tudo")
require(r"SDL_CONTROLLERDEVICEREMOVED", "hotplug solta tudo")
# Sink segurando keycode compartilhado nunca é solto pelo nativo
require(r"if \(!desired && sink_key_pressed\[keycode\]\)\s*\n\s*continue;", "guarda de keycode compartilhado")

# Eixos: deadzone radial com reescala; gatilhos nunca como eixo; cursor nunca vaza
require(r"static void radial_deadzone\(float \*x, float \*y\)", "deadzone radial")
require(r"st_jni_motion_event\(ax, ay, az, arz, 0\.0f, 0\.0f,\s*\n?\s*hx, hy\)", "gatilhos zerados no MotionEvent")
require(r"if \(!right_consumed\) \{\s*\n\s*az = rx_dz;", "RIGHT_STICK só chega cru quando não consumido")
require(r"hy = \(float\)\(dn - up\);", "hat simétrico (UP e DOWN com a mesma regra)")
for d in ("UP", "DOWN", "LEFT", "RIGHT"):
    require(rf"control_down\[NXINPUT_GPTK_{d}\] &&\s*\n\s*!nc_gptk_should_consume\(NXINPUT_GPTK_{d}\)", f"D-pad {d} respeita a decisão")

# Cursor: só RIGHT_STICK+R3 via cursor.* do arquivo; seta escondida não clica
require(r'strncmp\(stick_action, "cursor\.", 7\) == 0', "cursor só quando o arquivo entrega o stick a cursor.*")
require(r"if \(down && cursor_idle_hidden\(\) && !cursor_drag_active\)", "clique com seta escondida só revela")
require(r"nxinput_gptk_cursor_step\(&cursor_tuning", "cinemática canônica do nxinput")
forbid(r"cursor_click_now =[^;]*A", "A nunca clica")

# Contexto provado pela engine
for s in ("ui:modal", "player:allow-control", "scene:touch-ui", "player:control-locked", "player-contract-unavailable", "frame0"):
    require(re.escape(s), f"fonte de contexto {s}")
require(r"op_Implicit", "objeto destruído não conta como player vivo")
require(r'nc_gptk_set_context\(NC_GPTK_CONTEXT_MENU, "player:control-locked"\)', "controle travado = touch-only (menu), nunca gameplay presumido")
forbid(r"grounded|jumpCount", "telemetria não confiável removida")

# Pause: nunca UIManager e KeyEvent na mesma pressão
pause = src[src.index("int nc_sink_engine_input_pause"):src.index("int nc_sink_adapter_system_quit")]
assert "nc_pause_toggle_native()" in pause and "pause_key_sent = 1" in pause
assert pause.index("nc_pause_toggle_native()") < pause.index("sink_key(AKEY_BUTTON_START, 1)")

# Sinks exportados (o nxrelease liga sink-id -> símbolo definido)
for sym in ("nc_sink_engine_input_jump", "nc_sink_engine_input_interact", "nc_sink_engine_input_down",
            "nc_sink_engine_ui_accept", "nc_sink_engine_input_pause", "nc_sink_adapter_system_quit",
            "nc_sink_adapter_pointer_click", "nc_sink_engine_input_move", "nc_sink_adapter_pointer_move"):
    require(rf"^int {sym}\(", f"sink exportado {sym}", count=1)
    assert not re.search(rf"static int {sym}\(", src), f"{sym} não pode ser static"

# main.c: pré-init antes do vídeo, status não-zero em fatal
assert main.index("st_input_preinit()") < main.index("st_egl_init();"), "pré-init antes do SDL de vídeo"
assert "_exit(st_video_fatal ? 72 : st_input_fatal() ? 70 : 0)" in main
assert "nxgl_frame_proof_is_fatal()" in main and "nxgl_frame_proof_consume_fatal()" in main

# build: gates de identidade do runtime vivo
for s in ("nxinput-gptk-runtime/3", "nxinput-gptk-event-evidence/1", "NXC6-DOMAIN", "nxinput-gptk-runtime/2"):
    assert s in build, f"build_universal.sh não gateia {s}"

print("INPUT-LIFECYCLE: GREEN — autoridade única, chord soberano, estado sem latch, cursor contido")
PY
