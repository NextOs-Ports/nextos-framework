#!/usr/bin/env bash
# Launcher PortMaster do Stardew Valley (Android 1.6.15.3, AArch64 nativo).
#
# Deliberadamente enxuto: o ciclo de vida do frontend (parar/voltar o ES, matar
# instancia anterior, gptokeyb, TTY) e' do PortMaster — runemu.sh / "run app" ja'
# fazem isso antes e depois deste script. Repetir aqui so' cria corrida com o
# proprio PortMaster. Aqui ficam apenas: dados do jogo, escolha do binario e
# ambiente de execucao.
#
# Nao forcamos backend de video nem de audio: a negociacao nativa de SDL/OpenAL
# do firmware continua valendo em todo device suportado.
#
# Sem `set -u`: control.txt e nxextract-runtime-env.sh sao carregados com
# `source` e consultam variaveis que ainda nao existem (o device_info.txt do
# PortMaster aborta em DEVICE_INFO_VERSION). Nao sao codigo nosso.

GAMEDIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" 2>/dev/null && pwd -P) ||
  exit 1

# O LOG VEM ANTES DE TUDO. Ate' a v1.1.5 o redirecionamento acontecia depois de
# carregar o control.txt do CFW, entao qualquer falha nesse trecho — ou dentro do
# proprio PortMaster — sumia sem deixar rastro: o port voltava ao menu sem gerar
# arquivo nenhum, que foi exatamente o relato do muOS/RG 40XX-H na v1.1.5.
# Se o diretorio do jogo nao aceitar escrita, cai para /tmp em vez de morrer:
# um log no lugar errado ainda e' infinitamente melhor que nenhum log.
if [ -s "$GAMEDIR/debug.log" ]; then
  mv -f -- "$GAMEDIR/debug.log" "$GAMEDIR/debug.prev.log" 2>/dev/null
fi
if : > "$GAMEDIR/debug.log" 2>/dev/null; then
  SDV_LOG=$GAMEDIR/debug.log
else
  SDV_LOG=${TMPDIR:-/tmp}/stardewvalley-debug.log
fi
exec >> "$SDV_LOG" 2>&1
printf '=== Stardew Valley (NextOS) | release %s | %s ===\n' \
  "$(tr -d '\r\n' < "$GAMEDIR/version.txt" 2>/dev/null || echo unknown)" \
  "$(date -Is 2>/dev/null || date)"
printf '[launcher] gamedir=%s log=%s shell=%s\n' \
  "$GAMEDIR" "$SDV_LOG" "${BASH_VERSION:-?}"

cd "$GAMEDIR" || { printf '[launcher] nao consegui entrar em %s\n' "$GAMEDIR"; exit 1; }

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
printf '[launcher] cfw=%s controlfolder=%s esudo=%s\n' \
  "${CFW_NAME:-nenhum}" "$controlfolder" "${ESUDO:-nenhum}"

launcher_error() { printf 'Stardew Valley: %s\n' "$*" >&2; exit 1; }

# A lista tem de ser COMPLETA (regressao corrigida na v1.1.5). O
# `nxextract-runtime-env.sh` executa `run-extractor.sh` diretamente, e o proprio
# `run-extractor.sh` so' roda a migracao do assembly store se
# `tools/prepare_stardew_data.py` passar num `[ -x ... ]` — sem o bit, a migracao
# e' pulada EM SILENCIO. Cartao FAT/exFAT esconde o problema (tudo la' e' 0777);
# ROM partition ext4, ou um ZIP extraido sem preservar modos, nao.
$ESUDO chmod +x \
  "$GAMEDIR/stardewvalley" \
  "$GAMEDIR/stardewvalley.multi" \
  "$GAMEDIR/run.sh" \
  "$GAMEDIR/run-extractor.sh" \
  "$GAMEDIR/nxextract.py" \
  "$GAMEDIR/nxextract-ui" \
  "$GAMEDIR/nxextract-runtime-env.sh" \
  "$GAMEDIR/tools/prepare_stardew_data.py" \
  2>/dev/null || true
# O dialogo do PortMaster escreve no TTY do frontend. Isto NAO para nem
# reinicia o EmulationStation.
$ESUDO chmod 666 "$CUR_TTY" /dev/uinput 2>/dev/null || true

# Preparacao BYO-data (NXExtract): extrai/valida os assets a partir do APK do
# usuario. E' o unico passo que nao pode faltar.
[ -f "$GAMEDIR/extractor.json" ] ||
  launcher_error "receita do extrator ausente no diretorio do jogo"
extractor_env=$GAMEDIR/nxextract-runtime-env.sh
# `[ ! -L ]`: o helper e' carregado com `source`, entao um symlink plantado no
# lugar dele executaria codigo arbitrario como o jogo. Guarda herdada da v1.1.3.
[ -f "$extractor_env" ] && [ ! -L "$extractor_env" ] ||
  launcher_error "NXExtract runtime helper ausente ou inseguro"
# shellcheck source=nxextract-runtime-env.sh
source "$extractor_env"
declare -F sdv_run_extractor >/dev/null 2>&1 ||
  launcher_error "NXExtract runtime helper incompleto"
machine=$(uname -m 2>/dev/null) ||
  launcher_error "nao foi possivel identificar a arquitetura"
sdv_run_extractor "$GAMEDIR" "$controlfolder" / "$machine" || {
  status=$?
  launcher_error "preparacao dos dados falhou ($status)"
}
[ "${SDV_EXTRACTOR_ONLY:-0}" = 1 ] && exit 0

# NextOS usa a glibc atual do sistema; nos demais CFW vale o binario compat.
if [ -r /etc/os-release ] &&
   LC_ALL=C grep -Eiq 'nextos|retro[[:space:]_-]*elite' /etc/os-release; then
  BIN=$GAMEDIR/stardewvalley
else
  BIN=$GAMEDIR/stardewvalley.multi
fi
[ -x "$BIN" ] || launcher_error "loader ausente: $(basename "$BIN")"

# Bibliotecas do firmware primeiro: SDL, EGL, Mali e OpenAL tem de casar com o
# kernel/CFW em execucao. As DSO Android ficam por ultimo e sao abertas
# explicitamente pelo nosso loader, nunca interpostas sobre a libc do host.
export LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib:$controlfolder/libs:$controlfolder/libs.aarch64:$GAMEDIR:$GAMEDIR/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDV_LIBDIR="$GAMEDIR/libs"
[ -f "$GAMEDIR/alsoft.conf" ] && export ALSOFT_CONF="$GAMEDIR/alsoft.conf"
for _sock in /var/run/pulse/native /run/pulse/native; do
  [ -z "${PULSE_SERVER:-}" ] && [ -S "$_sock" ] &&
    export PULSE_SERVER="unix:$_sock" && break
done
case "${AUDIO_DRIVER:-}" in
  '') ;;
  pipewire|pulse|alsa) export ALSOFT_DRIVERS=$AUDIO_DRIVER ;;
  *) launcher_error "AUDIO_DRIVER deve ser pipewire, pulse ou alsa" ;;
esac

export SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0
[ -n "${sdl_controllerconfig:-}" ] &&
  export SDL_GAMECONTROLLERCONFIG=$sdl_controllerconfig

# Devices de ~640 MB (RG351/R36S): arenas de malloc menores e devolucao agressiva
# ao SO evitam que a fragmentacao do Mono vire pressao de memoria.
_mem=$(awk '/MemTotal/{print $2; exit}' /proc/meminfo 2>/dev/null || echo 0)
case "$_mem" in ''|*[!0-9]*) _mem=0 ;; esac
if [ "$_mem" -gt 0 ] && [ "$_mem" -lt 1200000 ]; then
  export MALLOC_ARENA_MAX=2 MALLOC_TRIM_THRESHOLD_=131072 \
         MALLOC_MMAP_THRESHOLD_=65536
fi

printf '[launcher] binario=%s ram=%skB\n' "$(basename "$BIN")" "$_mem"

# Handoff do PortMaster: fecha o dialogo/splash dele antes do jogo aparecer. NAO
# mexe no frontend (nao para nem reinicia o ES) — em CFW onde o control.txt faz
# mais que no ArkOS, e' aqui que isso acontece. Manter preserva a compatibilidade
# da v1.1.3 em muOS/ROCKNIX/Knulli.
if command -v pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$BIN" >/dev/null 2>&1 || true
fi

# Foreground significa OWNERSHIP: o launcher continua vivo, dono do PID exato, e
# so' retorna depois do jogo. A v1.1.5 usava `exec "$BIN"`, que substitui o shell
# — o frontend perde o dono do processo e nada pode ser registrado depois da
# saida. Este e' o padrao aprovado do projeto (launcher-portmaster.md secao 5).
"$BIN"
status=$?
printf '[launcher] jogo terminou com status %s\n' "$status"
exit "$status"
