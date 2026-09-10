#!/bin/bash
# run_device.sh -- execucao de diagnostico NO APARELHO (nao e o launcher final).
# Mascara o ES em --runtime, confirma que nenhum jogo esta ativo, roda o loader
# com log persistente e devolve o ES no fim.
GAMEDIR=/storage/roms/ports/blossomtales
cd "$GAMEDIR" || exit 1
SECS="${SB_RUN_SECONDS:-90}"

RUN_LOCK=/tmp/blossomtales-run.lock
exec 9>"$RUN_LOCK" || exit 1
if ! flock -n 9; then
  echo "[run] outro run ja esta ativo; abortando" >&2
  exit 73
fi

systemctl mask --runtime emustation >/dev/null 2>&1
systemctl stop emustation >/dev/null 2>&1

# Nunca lancar sobre outro jogo: varre /proc/*/exe e mata qualquer binario que
# esteja dentro de /storage/roms/ports (regra da casa), depois confirma zero.
kill_ports() {
  local sig="$1" found=0 pid exe
  for pid in /proc/[0-9]*; do
    exe=$(readlink "$pid/exe" 2>/dev/null) || continue
    case "$exe" in
      /storage/roms/ports/*)
        [ "${pid#/proc/}" = "$$" ] && continue
        echo "[run] matando ${pid#/proc/} ($exe)"
        kill "-$sig" "${pid#/proc/}" 2>/dev/null
        found=1
        ;;
    esac
  done
  return $found
}
kill_ports TERM; sleep 2; kill_ports KILL; sleep 1
STILL=""
for pid in /proc/[0-9]*; do
  exe=$(readlink "$pid/exe" 2>/dev/null) || continue
  case "$exe" in /storage/roms/ports/*) STILL="$STILL ${pid#/proc/}";; esac
done
if [ -n "$STILL" ]; then
  echo "[run] ERRO: ainda ha jogo ativo:$STILL; launch CANCELADO"
  systemctl unmask --runtime emustation >/dev/null 2>&1
  systemctl start emustation >/dev/null 2>&1
  exit 73
fi
echo "[run] confirmado: zero jogos ativos"

[ -f debug.log ] && mv -f debug.log debug.prev.log
echo 3 > /proc/sys/vm/drop_caches

for socket in /var/run/pulse/native /run/pulse/native; do
  [ -S "$socket" ] && export PULSE_SERVER="unix:$socket" && break
done
mkdir -p /tmp/sb-run && export XDG_RUNTIME_DIR=/tmp/sb-run
mkdir -p "$GAMEDIR/home"

# Captura da tela feita PELO APARELHO: o fb0 do Amlogic tem dois buffers
# empilhados e so a metade apontada por `pan` esta na tela. Fazer isso aqui
# evita depender do tempo de ida e volta do SSH.
if [ -n "$SB_SHOT_AT" ]; then
  rm -f "$GAMEDIR"/shot_*.raw
  (
    exec 9>&-
    last=0
    for at in $SB_SHOT_AT; do
      sleep $((at - last)); last=$at
      Y=$(cat /sys/class/graphics/fb0/pan 2>/dev/null); Y=${Y#*,}
      S=$(cat /sys/class/graphics/fb0/stride)
      dd if=/dev/fb0 of="$GAMEDIR/shot_$at.raw" bs="$S" skip="$Y" count=720 2>/dev/null
      echo "[run] shot em ${at}s" >> "$GAMEDIR/debug.log"
    done
  ) &
fi

# Roteiro de teclas para teste sem as maos: "segundos:keycode[:ms] ..."
# (keycodes do Android; 96=A, 97=B, 99=X, 100=Y, 108=START, 19..22=DPAD).
if [ -n "$SB_KEYS" ]; then
  (
    exec 9>&-
    last=0
    for step in $SB_KEYS; do
      at=${step%%:*}; rest=${step#*:}
      code=${rest%%:*}; ms=${rest#*:}
      [ "$ms" = "$rest" ] && ms=120
      sleep $((at - last)); last=$at
      echo "press $code $ms" > /tmp/sb-input.cmd
      echo "[run] tecla $code (${ms}ms) em ${at}s" >> "$GAMEDIR/debug.log"
    done
  ) &
fi

# Amostra de memoria: o Mono no Mali-450 e classe de OOM conhecida e o teto
# combinado de swap e 350 MiB.
(
  exec 9>&-
  while sleep 30; do
    pid=$(pgrep -x blossomtales 2>/dev/null | head -1)
    [ -z "$pid" ] && break
    rss=$(awk '/^VmRSS/{print $2}' /proc/$pid/status 2>/dev/null)
    swap=$(awk '/^VmSwap/{print $2}' /proc/$pid/status 2>/dev/null)
    free=$(awk '/^MemAvailable/{print $2}' /proc/meminfo)
    echo "[mem] rss=${rss}kB swap=${swap}kB disponivel=${free}kB" >> "$GAMEDIR/debug.log"
  done
) &

echo "[run] lancando blossomtales ($SECS s) $(date +%H:%M:%S)"
HOME="$GAMEDIR/home" \
MALLOC_ARENA_MAX=2 \
SB_RUNTIME_INIT=1 SB_START_ACTIVITY=1 SB_HOLD=1 \
SB_LIBDIR="$GAMEDIR/libs" \
SB_ASSET_DIR="$GAMEDIR/assets" \
SB_APK="$GAMEDIR/game.apk" \
SB_PRESENT_FBO=1 SB_FBO_TRACK=1 SB_RIGHT_CURSOR=0 \
SB_JNI_VERBOSE="${SB_JNI_VERBOSE:-0}" SB_FPS_TRACE=1 \
SB_INPUT_COMMANDS=/tmp/sb-input.cmd \
SB_WIDTH=1280 SB_HEIGHT=720 \
LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$GAMEDIR/libs" \
env ${SB_EXTRA_ENV} nice -n 19 timeout -k 5 "$SECS" ./blossomtales >> debug.log 2>&1
echo "[run] EXIT=$? $(date +%H:%M:%S)" >> debug.log

systemctl unmask --runtime emustation >/dev/null 2>&1
systemctl start emustation >/dev/null 2>&1
tail -n "${SB_TAIL:-120}" debug.log
