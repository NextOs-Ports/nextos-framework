#!/usr/bin/env bash
# Geometry Dash (Cocos2d-x 2.2.x) -- runtime unico multi-device.
#
# Contrato: suportando_outros_devices/launcher-portmaster.md. O launcher NAO
# administra o frontend: nao para, nao reinicia e nao mascara o
# EmulationStation, nao usa systemctl, nohup nem setsid. Ele valida os dados,
# monta o ambiente e supervisiona o jogo em foreground.
#
# Sem `set -u`: o control.txt do PortMaster consulta variaveis que ainda nao
# existem. Nao e' codigo nosso.

GD_NAME="Geometry Dash"
GD_BIN_NAME=geometrydash
GD_PACKAGE=com.robtopx.geometryjump

GAMEDIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" 2>/dev/null && pwd -P) ||
  exit 1

# O LOG VEM ANTES DE TUDO: falha antes do redirecionamento vira "nao abre, sem
# log". Cartao somente-leitura cai no /tmp.
if [ -s "$GAMEDIR/debug.log" ]; then
  mv -f -- "$GAMEDIR/debug.log" "$GAMEDIR/debug.prev.log" 2>/dev/null
fi
if : > "$GAMEDIR/debug.log" 2>/dev/null; then
  GD_LOG=$GAMEDIR/debug.log
else
  GD_LOG=${TMPDIR:-/tmp}/$GD_BIN_NAME-debug.log
fi
exec >> "$GD_LOG" 2>&1
printf '=== %s (NextOS) | release %s | %s ===\n' "$GD_NAME" \
  "$(tr -d '\r\n' < "$GAMEDIR/version.txt" 2>/dev/null || echo unknown)" \
  "$(date -Is 2>/dev/null || date)"
printf '[runtime] gamedir=%s log=%s shell=%s\n' \
  "$GAMEDIR" "$GD_LOG" "${BASH_VERSION:-?}"

cd "$GAMEDIR" || { printf '[runtime] nao consegui entrar em %s\n' "$GAMEDIR"; exit 1; }

GD_BIN=$GAMEDIR/$GD_BIN_NAME

# ---- PortMaster opcional -----------------------------------------------------
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
for _cf in /opt/system/Tools/PortMaster /opt/tools/PortMaster \
           "$XDG_DATA_HOME/PortMaster" /roms/ports/PortMaster \
           /roms2/ports/PortMaster /userdata/roms/ports/PortMaster \
           /storage/.config/PortMaster; do
  [ -d "$_cf" ] && { controlfolder=$_cf; break; }
done
: "${controlfolder:=/storage/.config/PortMaster}"
if [ -f "$controlfolder/control.txt" ]; then
  # shellcheck disable=SC1091
  source "$controlfolder/control.txt"
  case "${CFW_NAME:-}" in
    ''|*[!A-Za-z0-9._-]*) ;;
    *) [ -f "$controlfolder/mod_${CFW_NAME}.txt" ] &&
         source "$controlfolder/mod_${CFW_NAME}.txt" ;;
  esac
  declare -F get_controls >/dev/null 2>&1 && get_controls
fi
: "${ESUDO:=}"
: "${CUR_TTY:=/dev/tty0}"
printf '[runtime] cfw=%s controlfolder=%s esudo=%s\n' \
  "${CFW_NAME:-nenhum}" "$controlfolder" "${ESUDO:-nenhum}"

# Erro NUNCA mudo: log + aviso no TTY do frontend quando existir.
runtime_error() {
  printf '[runtime] ERRO: %s\n' "$*"
  printf '%s: %s\n' "$GD_NAME" "$*" > "$CUR_TTY" 2>/dev/null || true
  command -v pm_finish >/dev/null 2>&1 && pm_finish 2>/dev/null
  exit 1
}

$ESUDO chmod 666 "$CUR_TTY" /dev/uinput 2>/dev/null || true
# Cartao FAT/exFAT come o bit +x; o binario e' invocado direto, entao repor o
# bit e' util, mas nenhuma etapa CRITICA depende de `[ -x ... ]`.
$ESUDO chmod +x "$GD_BIN" 2>/dev/null || true

# ---- ABI: este port e' AArch64-only (as libs do jogo so' existem em arm64) ----
gd_arch=$(uname -m 2>/dev/null)
case "$gd_arch" in
  aarch64|arm64) ;;
  *) runtime_error "este port precisa de userland AArch64 (detectado: $gd_arch); as bibliotecas originais do jogo existem apenas em arm64-v8a" ;;
esac
[ -e /lib/ld-linux-aarch64.so.1 ] || [ -e /lib64/ld-linux-aarch64.so.1 ] ||
  runtime_error "userland AArch64 nao encontrado (/lib/ld-linux-aarch64.so.1 ausente)"
[ -s "$GD_BIN" ] || runtime_error "runtime ausente: $GD_BIN_NAME"

# ---- instancia unica ---------------------------------------------------------
# A trava definitiva e' o flock que o PROPRIO BINARIO adquire; isto aqui limpa
# instancia antiga comprovada por /proc, casando pelo DIRETORIO e nao pelo nome.
gd_matches() {
  gd_pid=${1##*/}
  [ "$gd_pid" != "$$" ] || return 1
  gd_exe=$(readlink "$1/exe" 2>/dev/null || true)
  gd_cwd=$(readlink "$1/cwd" 2>/dev/null || true)
  gd_cmd=$(tr '\000' ' ' < "$1/cmdline" 2>/dev/null || true)
  case "$gd_exe" in
    "$GD_BIN"|"$GD_BIN (deleted)") return 0 ;;
  esac
  case "$gd_cmd" in
    *"$GD_BIN"*|*"./$GD_BIN_NAME"*)
      [ "$gd_cwd" = "$GAMEDIR" ] && return 0 ;;
  esac
  return 1
}
gd_pids() {
  for gd_proc in /proc/[0-9]*; do
    [ -d "$gd_proc" ] || continue
    gd_matches "$gd_proc" && printf '%s\n' "${gd_proc##*/}"
  done
}
gd_old=$(gd_pids)
if [ -n "$gd_old" ]; then
  printf '[runtime] instancia antiga: %s\n' "$gd_old"
  kill -TERM $gd_old 2>/dev/null || true
  gd_wait=0
  while [ -n "$(gd_pids)" ] && [ "$gd_wait" -lt 16 ]; do
    sleep 0.5; gd_wait=$((gd_wait + 1))
  done
  gd_old=$(gd_pids)
  if [ -n "$gd_old" ]; then
    printf '[runtime] prazo esgotado; encerrando: %s\n' "$gd_old"
    kill -KILL $gd_old 2>/dev/null || true
    sleep 1
  fi
fi
gd_old=$(gd_pids)
[ -z "$gd_old" ] || runtime_error "outra instancia continua viva: $gd_old"

# ---- NXExtract: dados do usuario, validados e instalados transacionalmente ----
# A UI do extrator usa SDL/EGL/GLES do FIRMWARE. O APK do usuario nunca e'
# apagado nem modificado.
chmod +x "$GAMEDIR/run-extractor.sh" "$GAMEDIR/nxextract-runtime-env.sh" \
  "$GAMEDIR/nxextract.py" "$GAMEDIR/nxextract-ui" 2>/dev/null || true
if [ -f "$GAMEDIR/extractor.json" ] && [ -f "$GAMEDIR/run-extractor.sh" ]; then
  command -v python3 >/dev/null 2>&1 ||
    runtime_error "este firmware nao tem python3; o instalador de dados nao pode rodar"
  if ! NXEXTRACT_GAME_DIR=$GAMEDIR \
       NXEXTRACT_FIRMWARE_LIBRARY_PATH=/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib \
       bash "$GAMEDIR/run-extractor.sh"; then
    # Diagnostico honesto: se o pacote e' do jogo certo e mesmo assim reprovou,
    # dizer isso em vez de "nao encontrei nada".
    if grep -q "$GD_PACKAGE" "$GAMEDIR/nxextract.log" 2>/dev/null; then
      runtime_error "o arquivo em gamedata/ e' do $GD_NAME, mas o conteudo nao bate com o que esta receita aceita; detalhes em nxextract.log"
    fi
    runtime_error "dados do jogo ausentes. Coloque em gamedata/ o APK do $GD_NAME que voce possui (arm64) e abra de novo; detalhes em nxextract.log"
  fi
fi

# Gate de artefatos: recusar iniciar com payload incompleto, nunca pular calado.
for gd_required in game.apk libcocos2dcpp.so libfmod.so; do
  [ -s "$GAMEDIR/$gd_required" ] ||
    runtime_error "dado obrigatorio ausente: $gd_required (veja gamedata/README.txt)"
done

mkdir -p "$GAMEDIR/userdata"

# ---- ambiente do runtime -----------------------------------------------------
# As libs do FIRMWARE vem primeiro; as libs Android do jogo sao abertas pelo
# proprio so-loader e nunca substituem SDL/EGL/GLES/libc do host.
export HOME="$GAMEDIR"
gd_system_libs=/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib
export LD_LIBRARY_PATH="$gd_system_libs:$controlfolder/libs:$controlfolder/libs.aarch64:$GAMEDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}

# NUNCA fixar SDL_VIDEODRIVER/SDL_AUDIODRIVER: quem escolhe e' o firmware.
[ -n "${SDL_VIDEODRIVER:-}" ] &&
  printf '[runtime] SDL_VIDEODRIVER herdado=%s (preservado)\n' "$SDL_VIDEODRIVER"
[ -n "${SDL_AUDIODRIVER:-}" ] &&
  printf '[runtime] SDL_AUDIODRIVER herdado=%s (preservado)\n' "$SDL_AUDIODRIVER"

# ---- audio herdado: sanidade, nunca escolha -----------------------------------
# `SDL_AUDIODRIVER` continua intocado. O que se conserta aqui e' UM caso
# concreto: `PULSE_SERVER` herdado apontando para um socket que nao existe
# mais. Nesses firmwares o SDL tenta o Pulse, falha, e o jogo abre mudo -- ou
# pior, o SDL_Init inteiro cai. E o socket do pipewire-pulse (ROCKNIX) mora no
# runtime dir da SESSAO, nao em /run/pulse, entao a sonda tem que olhar la'.
gd_pulse_socket=""
for _sock in "${XDG_RUNTIME_DIR:-/run/user/$(id -u 2>/dev/null)}/pulse/native" \
             "/run/user/$(id -u 2>/dev/null)/pulse/native" \
             "/run/pulse/native" \
             "/var/run/pulse/native"; do
  [ -S "$_sock" ] && { gd_pulse_socket=$_sock; break; }
done
if [ -n "${PULSE_SERVER:-}" ]; then
  gd_pulse_path=${PULSE_SERVER#unix:}
  if [ -S "$gd_pulse_path" ]; then
    printf '[runtime] PULSE_SERVER herdado vivo: %s\n' "$PULSE_SERVER"
  elif [ -n "$gd_pulse_socket" ]; then
    printf '[runtime] PULSE_SERVER herdado morto (%s); usando o socket real %s\n' \
      "$PULSE_SERVER" "$gd_pulse_socket"
    export PULSE_SERVER="unix:$gd_pulse_socket"
  else
    printf '[runtime] PULSE_SERVER herdado morto (%s); sem socket Pulse vivo, a escolha volta para a autodeteccao da SDL\n' \
      "$PULSE_SERVER"
    unset PULSE_SERVER
  fi
elif [ -n "$gd_pulse_socket" ]; then
  printf '[runtime] socket Pulse vivo em %s\n' "$gd_pulse_socket"
fi

# Controle NATIVO: o mapping do usuario/PortMaster vence a base do firmware, e
# o binario ainda completa SELECT/START/L3/R3 lendo o evdev (varios portateis
# entregam esses quatro como TRIGGER_HAPPY, fora de qualquer mapping da SDL).
export SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0
[ -n "${sdl_controllerconfig:-}" ] &&
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && [ -n "${controlfolder:-}" ]; then
  for _db in "$controlfolder/gamecontrollerdb.txt" \
             "$controlfolder/gamecontrollerdb-SDL2.txt"; do
    [ -r "$_db" ] && [ ! -L "$_db" ] &&
      { export SDL_GAMECONTROLLERCONFIG_FILE=$_db; break; }
  done
fi

# Fonte de sistema para os poucos labels que a engine nao desenha por BMFont.
# Cada firmware guarda a sua num lugar; o PortMaster carrega uma, e essa e' a
# unica presente em CFW enxuto (ROCKNIX, muOS). Sem nenhuma, esses labels saem
# vazios -- entao a busca e' feita aqui, onde `controlfolder` e' conhecido.
if [ -z "${GD_FONT:-}" ]; then
  for _font in "$controlfolder/resources/DejaVuSans.ttf" \
               "$controlfolder/pylibs/resources/DejaVuSans.ttf" \
               /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
               /usr/share/fonts/dejavu/DejaVuSans.ttf \
               /usr/share/fonts/TTF/DejaVuSans.ttf; do
    [ -r "$_font" ] && { export GD_FONT=$_font; break; }
  done
fi
[ -n "${GD_FONT:-}" ] && printf '[runtime] fonte de sistema: %s\n' "$GD_FONT"

# Os menus sao de toque, entao o analogico DIREITO move uma seta e o R3 toca
# onde ela esta'; a seta some sozinha depois de alguns segundos parada.
export GD_CURSOR=${GD_CURSOR:-1}

printf '[runtime] controle nativo; A/B/X/Y/L/R pulam, R3 toca, START pausa, SELECT+START sai\n'
printf '[runtime] nos niveis de plataforma (Torre): D-pad e analogico esquerdo andam\n'
printf '[runtime] MemTotal=%s kB\n' \
  "$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null)"

# Fecha o splash do PortMaster e entrega o display. NAO mexe no frontend.
if command -v pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$GD_BIN" >/dev/null 2>&1 || true
fi

# ---- supervisao foreground ---------------------------------------------------
# O launcher continua vivo, dono do PID exato, e encaminha TERM ao filho.
child_pid=
stop_child() {
  [ -n "${child_pid:-}" ] || return 0
  kill -TERM "$child_pid" 2>/dev/null || return 0
  for _attempt in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$child_pid" 2>/dev/null || return 0
    sleep 1
  done
  kill -KILL "$child_pid" 2>/dev/null || true
}
trap stop_child EXIT INT TERM

"$GD_BIN" &
child_pid=$!
while :; do
  wait "$child_pid"
  status=$?
  kill -0 "$child_pid" 2>/dev/null || break
done
child_pid=
trap - EXIT INT TERM

printf '[runtime] jogo terminou com status %s\n' "$status"
printf '\033c' >> "$CUR_TTY" 2>/dev/null || true
command -v pm_finish >/dev/null 2>&1 && pm_finish 2>/dev/null
exit "$status"
