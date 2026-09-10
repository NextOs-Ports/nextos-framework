#!/usr/bin/env bash
# Hitman GO Unity/IL2CPP runtime. SDL chooses the firmware backend; the
# Mali-450 path keeps ownership of the vendor fbdev EGL context.
#
# Deliberadamente enxuto: o ciclo de vida do frontend (parar/voltar o ES,
# gptokeyb, TTY) e' do CFW/PortMaster. Aqui ficam apenas: validacao dos dados,
# ambiente de execucao e o lancamento supervisionado.
#
# Sem `set -u`: control.txt do PortMaster consulta variaveis que ainda nao
# existem. Nao e' codigo nosso.

GAMEDIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" 2>/dev/null && pwd -P) ||
  exit 1

# O LOG VEM ANTES DE TUDO (licao Stardew v1.1.6): qualquer falha antes do
# redirecionamento some sem deixar rastro e vira um relato "nao abre, sem log".
# Se o diretorio do jogo nao aceitar escrita, cai para /tmp em vez de morrer.
if [ -s "$GAMEDIR/debug.log" ]; then
  mv -f -- "$GAMEDIR/debug.log" "$GAMEDIR/debug.prev.log" 2>/dev/null
fi
if : > "$GAMEDIR/debug.log" 2>/dev/null; then
  HGO_LOG=$GAMEDIR/debug.log
else
  HGO_LOG=${TMPDIR:-/tmp}/hitmango-debug.log
fi
exec >> "$HGO_LOG" 2>&1
printf '=== Hitman GO (NextOS) | release %s | %s ===\n' \
  "$(tr -d '\r\n' < "$GAMEDIR/version.txt" 2>/dev/null || echo unknown)" \
  "$(date -Is 2>/dev/null || date)"
printf '[runtime] gamedir=%s log=%s shell=%s\n' \
  "$GAMEDIR" "$HGO_LOG" "${BASH_VERSION:-?}"

export HGO_GAMEDIR="$GAMEDIR"
cd "$GAMEDIR" || { printf '[runtime] nao consegui entrar em %s\n' "$GAMEDIR"; exit 1; }

# Handoff PortMaster/CFW: identifica o firmware e carrega o mapeamento de
# controle nativo. Em NextOS/EmuELEC nada disso existe e tudo e' pulado.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
for _cf in /opt/system/Tools/PortMaster /opt/tools/PortMaster \
           "$XDG_DATA_HOME/PortMaster" /roms/ports/PortMaster \
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

# Erro nunca-mudo: alem do log, avisa no TTY do frontend quando possivel.
runtime_error() {
  printf '[runtime] ERRO: %s\n' "$*"
  printf 'Hitman GO: %s\n' "$*" > "$CUR_TTY" 2>/dev/null || true
  exit 1
}

$ESUDO chmod 666 "$CUR_TTY" /dev/uinput 2>/dev/null || true
$ESUDO chmod +x "$GAMEDIR/hitmango" 2>/dev/null || true

hgo_matches() {
  hgo_pid=${1##*/}
  [ "$hgo_pid" != "$$" ] || return 1
  hgo_exe=$(readlink "$1/exe" 2>/dev/null || true)
  hgo_comm=$(cat "$1/comm" 2>/dev/null || true)
  hgo_cwd=$(readlink "$1/cwd" 2>/dev/null || true)
  hgo_cmd=$(tr '\000' ' ' < "$1/cmdline" 2>/dev/null || true)
  case "$hgo_exe" in
    "$GAMEDIR/hitmango"|"$GAMEDIR/hitmango (deleted)") return 0 ;;
  esac
  case "$hgo_cmd" in
    *"$GAMEDIR/hitmango"*|*"./hitmango"*)
      [ "$hgo_cwd" = "$GAMEDIR" ] && return 0 ;;
  esac
  if [ "$hgo_cwd" = "$GAMEDIR" ]; then
    case "$hgo_comm" in hitmango|UnityMain) return 0 ;; esac
  fi
  return 1
}

hgo_pids() {
  for hgo_proc in /proc/[0-9]*; do
    [ -d "$hgo_proc" ] || continue
    hgo_matches "$hgo_proc" && printf '%s\n' "${hgo_proc##*/}"
  done
}

hgo_describe() {
  for hgo_pid in $(hgo_pids); do
    hgo_proc=/proc/$hgo_pid
    hgo_exe=$(readlink "$hgo_proc/exe" 2>/dev/null || true)
    hgo_comm=$(cat "$hgo_proc/comm" 2>/dev/null || true)
    hgo_cmd=$(tr '\000' ' ' < "$hgo_proc/cmdline" 2>/dev/null || true)
    printf '[runtime] old pid=%s comm=%s exe=%s cmd=%s\n' \
      "$hgo_pid" "$hgo_comm" "$hgo_exe" "$hgo_cmd"
  done
}

hgo_old=$(hgo_pids)
if [ -n "$hgo_old" ]; then
  hgo_describe
  kill -TERM $hgo_old 2>/dev/null || true
  hgo_wait=0
  while [ -n "$(hgo_pids)" ] && [ "$hgo_wait" -lt 10 ]; do
    sleep 0.5
    hgo_wait=$((hgo_wait + 1))
  done
fi
hgo_old=$(hgo_pids)
if [ -n "$hgo_old" ]; then
  printf '[runtime] graceful stop timed out; terminating stale pid(s): %s\n' \
    "$hgo_old"
  kill -KILL $hgo_old 2>/dev/null || true
  sleep 1
fi
hgo_old=$(hgo_pids)
[ -z "$hgo_old" ] ||
  runtime_error "outro loader do Hitman GO ainda esta vivo: $hgo_old"

# ---- NXExtract: dados BYO validados/instalados de forma transacional ----
# A UI usa SDL/EGL/GLES do firmware (nxextract-runtime-env.sh cuida do escopo);
# o arquivo legal do usuario nunca e' apagado; dados antigos validos sao
# adotados por hash sem pedir o APK de novo.
chmod +x "$GAMEDIR/run-extractor.sh" "$GAMEDIR/nxextract-runtime-env.sh" \
  "$GAMEDIR/nxextract.py" "$GAMEDIR/nxextract-ui" 2>/dev/null || true
if [ -f "$GAMEDIR/extractor.json" ] && [ -x "$GAMEDIR/run-extractor.sh" ]; then
  command -v python3 >/dev/null 2>&1 ||
    runtime_error "este firmware nao tem python3; o instalador de dados nao pode rodar"
  NXEXTRACT_GAME_DIR=$GAMEDIR \
    NXEXTRACT_FIRMWARE_LIBRARY_PATH=/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib \
    "$GAMEDIR/run-extractor.sh" ||
    runtime_error "dados do jogo ausentes ou invalidos. Coloque seu APK do Hitman GO (1.18.1, arm64) em ports/hitmango/gamedata/ e abra de novo"
fi

for hgo_required in \
  hitmango \
  lib/libmain.so \
  lib/libunity.so \
  lib/libil2cpp.so \
  lib/libFirebaseCppApp-12_10_1.so \
  assets/bin/Data/boot.config \
  assets/bin/Data/globalgamemanagers \
  assets/bin/Data/Managed/Metadata/global-metadata.dat
do
  [ -s "$GAMEDIR/$hgo_required" ] ||
    runtime_error "dado obrigatorio ausente: $hgo_required (veja lib/README.txt e assets/README.txt)"
done

mkdir -p "$GAMEDIR/home"

# Firmware libraries stay ahead of Android guest libraries. The loader maps
# the original game DSOs explicitly and never lets them replace host SDL,
# EGL, Mali, libc or audio components. As libs do PortMaster entram depois do
# sistema para fornecer SDL2 recente onde o CFW nao tem.
hgo_system_libs=/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib
export LD_LIBRARY_PATH="$hgo_system_libs:$controlfolder/libs:$controlfolder/libs.aarch64${HITMANGO_FIRMWARE_LIBRARY_PATH:+:$HITMANGO_FIRMWARE_LIBRARY_PATH}:$GAMEDIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0
[ -n "${sdl_controllerconfig:-}" ] &&
  export SDL_GAMECONTROLLERCONFIG=$sdl_controllerconfig
# Base de mapeamentos do CFW. Sem ela, num firmware cujo pad não está na base
# embutida do SDL (relato do muOS/RG40XX-H), o controle não é reconhecido como
# GameController e o jogo fica SEM navegação nenhuma.
if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && [ -n "${controlfolder:-}" ]; then
  for _db in "$controlfolder/gamecontrollerdb.txt" \
             "$controlfolder/gamecontrollerdb-SDL2.txt"; do
    [ -r "$_db" ] && [ ! -L "$_db" ] &&
      { export SDL_GAMECONTROLLERCONFIG_FILE=$_db; break; }
  done
fi
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
# The right-stick cursor remains available in menus, level selection and on the
# board.  During a turn, D-pad/left stick also feed Hitman GO's own
# InputManager_tvOS.OnSwipe handler; that resolves the adjacent Node and calls
# the original LevelState.OnNodeClicked path.  Set to 0 only as a diagnostic
# fallback to the stock Android touch implementation.
# Andar por padrao = swipe sintetico sobre o gerenciador de TOQUE do jogo:
# com o InputManager_tvOS selecionado o LevelState ignora toques nos nos e a
# pedra (mira por toque) nunca sai.  HGO_NATIVE_CONTROLS=1 religa o caminho
# tvOS antigo apenas para diagnostico/comparacao.
export HGO_CURSOR=${HGO_CURSOR:-1}

printf '[runtime] backend=%s audio=%s game=%s\n' \
  "${SDL_VIDEODRIVER:-auto}" "${SDL_AUDIODRIVER:-auto}" "$GAMEDIR"
printf '[runtime] SELECT+START exits through focus loss and nativePause\n'

# Fecha o dialogo/splash do PortMaster antes do jogo aparecer. NAO mexe no
# frontend (nao para nem reinicia o ES).
if command -v pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$GAMEDIR/hitmango" >/dev/null 2>&1 || true
fi

# Foreground significa OWNERSHIP: o launcher continua vivo, dono do PID exato,
# e so' retorna depois do jogo (padrao do projeto; nada de `exec`).
"$GAMEDIR/hitmango" "$GAMEDIR"
status=$?
printf '[runtime] jogo terminou com status %s\n' "$status"
exit "$status"
