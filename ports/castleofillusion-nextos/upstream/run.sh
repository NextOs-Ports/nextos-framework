#!/usr/bin/env bash
# Castle of Illusion Starring Mickey Mouse (Disney/Sega 2013, engine "oz").
# so-loader AArch64: libViewer_GP.so + FMOD Ex, fluxo NativeActivity original.
#
# Deliberadamente enxuto: o ciclo de vida do frontend (parar/voltar o ES,
# gptokeyb, TTY) e' do CFW/PortMaster. Aqui ficam apenas: instancia unica,
# dados BYO, ambiente de execucao e o lancamento supervisionado.
#
# Sem `set -u`: control.txt do PortMaster consulta variaveis que ainda nao
# existem. Nao e' codigo nosso.

GAMEDIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" 2>/dev/null && pwd -P) ||
  exit 1

# O LOG VEM ANTES DE TUDO: qualquer falha antes do redirecionamento some sem
# deixar rastro e vira um relato "nao abre, sem log". Se o diretorio do jogo nao
# aceitar escrita (cartao FAT montado read-only, por exemplo), cai para /tmp.
if [ -s "$GAMEDIR/debug.log" ]; then
  mv -f -- "$GAMEDIR/debug.log" "$GAMEDIR/debug.prev.log" 2>/dev/null
fi
if : > "$GAMEDIR/debug.log" 2>/dev/null; then
  COI_LOG=$GAMEDIR/debug.log
else
  COI_LOG=${TMPDIR:-/tmp}/castleofillusion-debug.log
fi
exec >> "$COI_LOG" 2>&1
printf '=== Castle of Illusion (NextOS) | release %s | %s ===\n' \
  "$(tr -d '\r\n' < "$GAMEDIR/version.txt" 2>/dev/null || echo unknown)" \
  "$(date -Is 2>/dev/null || date)"
printf '[runtime] gamedir=%s log=%s shell=%s\n' \
  "$GAMEDIR" "$COI_LOG" "${BASH_VERSION:-?}"

cd "$GAMEDIR" || { printf '[runtime] could not enter %s\n' "$GAMEDIR"; exit 1; }

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

# REAFIRMA O LOG DEPOIS DO HANDOFF. O control.txt/mod do CFW e' codigo de
# terceiro que rodamos com `source`: um `exec 2>/dev/null` la dentro levaria
# junto TODO o diagnostico do port, porque o loader escreve em stderr — e o
# relato de campo chegaria cego. O PortMaster do ArkOS (verificado) nao faz
# isso; os demais CFW nao temos como verificar um a um, e reafirmar custa nada.
exec >> "$COI_LOG" 2>&1

printf '[runtime] cfw=%s controlfolder=%s esudo=%s\n' \
  "${CFW_NAME:-nenhum}" "$controlfolder" "${ESUDO:-nenhum}"

# Erro nunca-mudo: alem do log, avisa no TTY do frontend quando possivel. E
# devolve o controle ao frontend (pm_finish) em vez de deixa-lo pendurado.
runtime_error() {
  printf '[runtime] ERROR: %s\n' "$*"
  printf 'Castle of Illusion: %s\n' "$*" > "$CUR_TTY" 2>/dev/null || true
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
}

$ESUDO chmod 666 "$CUR_TTY" /dev/uinput 2>/dev/null || true
$ESUDO chmod +x "$GAMEDIR/castleofillusion" 2>/dev/null || true

# ---- Instancia unica ----------------------------------------------------
# Duas camadas. O `flock` resolve a corrida de dois launchers comecando ao mesmo
# tempo (frontend que dispara duas vezes); a varredura de /proc abaixo resolve o
# caso real de campo, que e' um JOGO de uma sessao anterior ainda vivo — esse
# nao segura lock nenhum, porque quem segurava era o launcher que ja morreu.
COI_LOCK=$GAMEDIR/.castleofillusion.lock
# CUIDADO: um `exec` que só carrega redirecionamentos aplica-os ao SHELL, para
# sempre. Escrever `exec 9>"$COI_LOCK" 2>/dev/null` mandaria o stderr do
# launcher — e o do jogo, que herda — para /dev/null pelo resto da execucao, e o
# debug.log ficaria vazio a partir daqui. O `2>` fica no `if`, nunca no `exec`.
# O lock SERIALIZA, mas nunca VETA. Um `flock -n` que aborta transforma um
# launcher travado (ou morto sem soltar o fd) num port que nao abre mais ate
# reiniciar o aparelho — foi o que aconteceu aqui: o jogo foi morto, o run.sh
# anterior ficou vivo segurando o fd, e a abertura seguinte desistiu sozinha.
# Trocamos por uma espera curta; se o lock nao vier, seguimos assim mesmo e a
# varredura de /proc abaixo — que e' a rede de verdade — encerra o jogo velho.
if command -v flock >/dev/null 2>&1 && exec 9>"$COI_LOCK"; then
  if flock -w 10 9; then
    printf '[runtime] single-instance lock acquired\n'
  else
    printf '[runtime] lock busy for 10s; falling back to the process sweep\n'
  fi
fi

# O binario pode aparecer como "(deleted)" depois de uma atualizacao, e o comm
# muda quando a engine renomeia a thread principal. Por isso a deteccao olha
# exe, cmdline, cwd e comm — nunca so' o nome.
coi_matches() {
  coi_pid=${1##*/}
  [ "$coi_pid" != "$$" ] || return 1
  coi_exe=$(readlink "$1/exe" 2>/dev/null || true)
  coi_comm=$(cat "$1/comm" 2>/dev/null || true)
  coi_cwd=$(readlink "$1/cwd" 2>/dev/null || true)
  coi_cmd=$(tr '\000' ' ' < "$1/cmdline" 2>/dev/null || true)
  case "$coi_exe" in
    "$GAMEDIR/castleofillusion"|"$GAMEDIR/castleofillusion (deleted)") return 0 ;;
  esac
  case "$coi_cmd" in
    *"$GAMEDIR/castleofillusion"*|*"./castleofillusion"*)
      [ "$coi_cwd" = "$GAMEDIR" ] && return 0 ;;
  esac
  if [ "$coi_cwd" = "$GAMEDIR" ]; then
    case "$coi_comm" in castleofillusion|Main) return 0 ;; esac
  fi
  return 1
}

coi_pids() {
  for coi_proc in /proc/[0-9]*; do
    [ -d "$coi_proc" ] || continue
    coi_matches "$coi_proc" && printf '%s\n' "${coi_proc##*/}"
  done
}

coi_old=$(coi_pids)
if [ -n "$coi_old" ]; then
  for coi_pid in $coi_old; do
    printf '[runtime] previous instance pid=%s comm=%s exe=%s\n' \
      "$coi_pid" "$(cat /proc/$coi_pid/comm 2>/dev/null)" \
      "$(readlink /proc/$coi_pid/exe 2>/dev/null)"
  done
  # TERM primeiro: o loader trata SIGTERM no MESMO caminho do SELECT+START
  # (pause/save/sair), entao o save da instancia velha nao e' perdido.
  kill -TERM $coi_old 2>/dev/null || true
  coi_wait=0
  while [ -n "$(coi_pids)" ] && [ "$coi_wait" -lt 16 ]; do
    sleep 0.5
    coi_wait=$((coi_wait + 1))
  done
fi
coi_old=$(coi_pids)
if [ -n "$coi_old" ]; then
  printf '[runtime] clean shutdown timed out; killing pid(s): %s\n' "$coi_old"
  kill -KILL $coi_old 2>/dev/null || true
  sleep 1
fi
coi_old=$(coi_pids)
[ -z "$coi_old" ] ||
  runtime_error "another Castle of Illusion loader is still alive: $coi_old"

# ---- NXExtract: dados BYO validados/instalados de forma transacional ----
# A UI usa SDL/EGL/GLES do firmware (nxextract-runtime-env.sh cuida do escopo);
# o arquivo legal do usuario nunca e' apagado; dados antigos validos sao
# adotados por hash sem pedir o APK/OBB de novo.
chmod +x "$GAMEDIR/run-extractor.sh" "$GAMEDIR/nxextract-runtime-env.sh" \
  "$GAMEDIR/nxextract.py" "$GAMEDIR/nxextract-ui" 2>/dev/null || true
if [ -f "$GAMEDIR/extractor.json" ] && [ -f "$GAMEDIR/run-extractor.sh" ]; then
  command -v python3 >/dev/null 2>&1 ||
    runtime_error "this firmware has no python3; the data installer cannot run"
  # O helper de runtime do NXExtract exige o bit +x (contrato upstream). Num
  # cartao FAT/exFAT montado sem permissao de execucao o chmod acima nao pega,
  # e o erro que chega ao usuario seria criptico. Diz o motivo real.
  [ -x "$GAMEDIR/nxextract-runtime-env.sh" ] ||
    runtime_error "nxextract-runtime-env.sh is not executable — the card seems to be mounted without exec (FAT/exFAT). Reinstall on a partition that preserves permissions"
  NXEXTRACT_GAME_DIR=$GAMEDIR \
    NXEXTRACT_FIRMWARE_LIBRARY_PATH=/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib \
    bash "$GAMEDIR/run-extractor.sh" ||
    runtime_error "game data missing or invalid. Put the APK (1.4.5, arm64) and the main.154 OBB from YOUR Castle of Illusion in ports/castleofillusion/gamedata/ and launch again"
fi

# Gate de dados: quem decide se o loader pode iniciar e' esta checagem, nunca o
# resultado da faxina do extrator.
for coi_required in \
  castleofillusion \
  lib/libViewer_GP.so \
  lib/libfmodex.so \
  obb/main.154.obb
do
  [ -s "$GAMEDIR/$coi_required" ] ||
    runtime_error "required file missing: $coi_required (see gamedata/README.txt)"
done

# userdata/ = save e config do jogo (sem a pasta o save nunca persiste e o
# titulo abre sempre em NEW GAME). gamedata/ e' a caixa de ENTRADA do usuario e
# fica separada de proposito: a documentacao manda apagar o APK/OBB de la depois
# da instalacao, e save de jogador nao pode estar no meio disso.
mkdir -p "$GAMEDIR/userdata" "$GAMEDIR/gamedata" "$GAMEDIR/home"
export HOME="$GAMEDIR/home"

# O loader resolve o OBB a partir do gamedir REAL (nunca /storage/roms cravado).
export COI_GAMEDIR="$GAMEDIR"

# ---- Ambiente de execucao do jogo --------------------------------------
# Libs do firmware primeiro; as libs privadas do jogo (Android) entram por
# ultimo e nunca substituem SDL, EGL, Mali, libc ou audio do host. As libs do
# PortMaster entram depois do sistema para fornecer SDL2 recente onde o CFW nao
# tem.
coi_system_libs=/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib
export LD_LIBRARY_PATH="$coi_system_libs:$controlfolder/libs:$controlfolder/libs.aarch64${CASTLEOFILLUSION_FIRMWARE_LIBRARY_PATH:+:$CASTLEOFILLUSION_FIRMWARE_LIBRARY_PATH}:$GAMEDIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# REGRA: NAO forcar SDL_VIDEODRIVER/SDL_AUDIODRIVER. O video (Mali fbdev,
# KMSDRM, Wayland...) e o audio vem do SDL do proprio firmware; o loader tem
# escada de config GL e rejeita contexto desktop-GL sozinho.
export SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0
[ -n "${sdl_controllerconfig:-}" ] &&
  export SDL_GAMECONTROLLERCONFIG=$sdl_controllerconfig
# Base de mapeamentos do CFW. Sem ela, num firmware cujo pad nao esta na base
# embutida do SDL (relato muOS/RG40XX-H), o controle nao e' reconhecido como
# GameController e o jogo fica sem navegacao. O loader ainda tem o caminho de
# pad CRU para o combo de saida, mas jogar exige o mapping.
if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && [ -n "${controlfolder:-}" ]; then
  for _db in "$controlfolder/gamecontrollerdb.txt" \
             "$controlfolder/gamecontrollerdb-SDL2.txt" \
             /storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt; do
    [ -r "$_db" ] && [ ! -L "$_db" ] &&
      { export SDL_GAMECONTROLLERCONFIG_FILE=$_db; break; }
  done
fi
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}

# ---- Idioma / Language ------------------------------------------------------
# Escolha UM: en fr it de es   (padrao en; japones nao e' oferecido).
# Pick ONE:   en fr it de es   (default en).
# Edite so' esta linha; NXPORT_TEXT ja' exportado no ambiente vence.
GAME_LANGUAGE="en"
case "$GAME_LANGUAGE" in en|fr|it|de|es) ;; *) GAME_LANGUAGE=en ;; esac
export NXPORT_LANGUAGE="${NXPORT_LANGUAGE:-$GAME_LANGUAGE}"
case "$NXPORT_LANGUAGE" in en|fr|it|de|es) ;; *) NXPORT_LANGUAGE="$GAME_LANGUAGE"; export NXPORT_LANGUAGE ;; esac
printf '[runtime] idioma=%s\n' "$NXPORT_LANGUAGE"

# ---- Perfil de desempenho / memoria (1.0.2) ---------------------------------
# As texturas do jogo ja' sao ETC1 (nada a comprimir) e a engine trava em 30fps,
# entao o ganho REAL em device apertado e' de RAM: impedir o kernel de matar o
# jogo por falta de memoria (o "trava/fecha" em <=1GB) e devolver paginas mortas.
# Tudo ligado aqui e' SEGURO: nao reduz resolucao nem toca a imagem.
#
#   COI_PAGE           = rede anti-OOM: despeja SO' texturas frias sob pressao.
#   COI_PAGE_FLOOR_MB  = piso de RAM livre; abaixo dele o despejo dispara.
#   COI_NO_STACK_SHRINK= (nao setado) => o encolhimento de stack morto fica ON.
#
# Automatico por RAM total: aparelhos <=1.25GiB ganham a rede anti-OOM; acima
# disso o comportamento fica igual ao 1.0.1. COI_PROFILE_MEM=off desliga;
# =on forca ligado. Variavel ja' exportada no ambiente sempre vence.
coi_mem_total_kb=$(awk '/^MemTotal:/{print $2; exit}' /proc/meminfo 2>/dev/null)
case "${COI_PROFILE_MEM:-auto}" in
  off) coi_mem_on=0 ;;
  on)  coi_mem_on=1 ;;
  *)   coi_mem_on=0; [ -n "$coi_mem_total_kb" ] && [ "$coi_mem_total_kb" -le 1310720 ] && coi_mem_on=1 ;;
esac
if [ "$coi_mem_on" = 1 ]; then
  # Diretorio de despejo (texturas frias viram <id>.tx no cartao). Limpo a cada
  # boot para nao acumular entre sessoes; se o cartao recusar escrita, o paging
  # simplesmente nao liga (coi_paging exige o swapdir).
  coi_swap="$GAMEDIR/.texswap"
  if rm -rf "$coi_swap" 2>/dev/null && mkdir -p "$coi_swap" 2>/dev/null; then
    [ -n "${COI_PAGE+x}" ]          || export COI_PAGE=1
    [ -n "${COI_PAGE_SWAP+x}" ]     || export COI_PAGE_SWAP="$coi_swap"
    [ -n "${COI_PAGE_FLOOR_MB+x}" ] || export COI_PAGE_FLOOR_MB=96
    [ -n "${COI_PAGE_CAP_MB+x}" ]   || export COI_PAGE_CAP_MB=420
    printf '[runtime] perfil-memoria: rede anti-OOM LIGADA (RAM=%sMB floor=%sMB cap=%sMB swap=%s)\n' \
      "$(( ${coi_mem_total_kb:-0} / 1024 ))" "$COI_PAGE_FLOOR_MB" "$COI_PAGE_CAP_MB" "$coi_swap"
  else
    printf '[runtime] perfil-memoria: swapdir sem escrita, paging OFF (RAM=%sMB)\n' \
      "$(( ${coi_mem_total_kb:-0} / 1024 ))"
  fi
else
  printf '[runtime] perfil-memoria: comportamento 1.0.1 (RAM total=%sMB)\n' \
    "$(( ${coi_mem_total_kb:-0} / 1024 ))"
fi

printf '[runtime] video=%s audio=%s controllerdb=%s\n' \
  "${SDL_VIDEODRIVER:-auto}" "${SDL_AUDIODRIVER:-auto}" \
  "${SDL_GAMECONTROLLERCONFIG_FILE:-nenhum}"
printf '[runtime] SELECT+START and SIGTERM exit through the same path (pause/save)\n'

# Fecha o dialogo/splash do PortMaster antes do jogo aparecer. NAO mexe no
# frontend (nao para nem reinicia o ES) — isso e' automatico do CFW.
if command -v pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$GAMEDIR/castleofillusion" >/dev/null 2>&1 || true
fi

# Foreground significa OWNERSHIP: o launcher continua vivo, dono do PID exato,
# e so' retorna depois do jogo. Nada de exec, setsid ou nohup.
"$GAMEDIR/castleofillusion"
status=$?
printf '[runtime] game exited status=%s\n' "$status"

if command -v pm_finish >/dev/null 2>&1; then
  pm_finish
fi
exit "$status"
