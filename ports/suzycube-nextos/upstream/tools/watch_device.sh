#!/bin/bash
# Roda NO DEVICE: vigia o processo do Suzy Cube, anota RSS e memoria livre e
# guarda uma copia do debug.log em tmpfs.  O log do jogo mora em VFAT: quando o
# processo morre de forma abrupta o dirent nunca e' escrito e o arquivo SOME --
# por isso a copia em /tmp, que sobrevive a queda e conta o que aconteceu.
#
#   watch_device.sh <dir-do-jogo> <voltas> <intervalo>
set -u
DST=${1:-/storage/roms/ports/suzycube}
LOOPS=${2:-40}
STEP=${3:-10}
OUT=/tmp/sc_watch.log
COPY=/tmp/sc_debug.log
: > "$OUT"

game_pid() {
  for q in /proc/[0-9]*; do
    e=$(readlink "$q/exe" 2>/dev/null)
    case "$e" in "$DST/suzycube-nextos"*) echo "${q##*/}"; return 0 ;; esac
  done
  return 1
}

i=0
visto=0
while [ "$i" -lt "$LOOPS" ]; do
  i=$((i + 1))
  sleep "$STEP"
  pid=$(game_pid) || pid=""
  if [ -z "$pid" ]; then
    # As primeiras voltas ainda pegam o launcher subindo: ausencia so' conta
    # como fim depois que o processo ja' tinha aparecido pelo menos uma vez.
    if [ "$visto" -eq 0 ]; then
      echo "[$i] ainda subindo" >> "$OUT"
      continue
    fi
    echo "[$i] PROCESSO AUSENTE" >> "$OUT"
    break
  fi
  visto=1
  rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)
  avail=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
  echo "[$i] pid=$pid rss=${rss}kB disponivel=${avail}kB" >> "$OUT"
  cp -f "$DST/log.txt" "$COPY" 2>/dev/null
done
echo "[fim] laco terminou apos $i voltas (teto $LOOPS)" >> "$OUT"
