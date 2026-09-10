#!/bin/bash
# Teste auto-contido do Stardew (so-loader) no R36S. Para o ES, roda, mede e
# RESTAURA o EmulationStation no fim.
#   uso: sdv_test.sh <segundos>
SECS=${1:-120}
GAMEDIR=/roms/ports/sdvnextos

kill_sdv() {
  for p in /proc/[0-9]*/exe; do
    t=$(readlink "$p" 2>/dev/null) || continue
    case "$t" in */ports/sdvnextos/stardewvalley*) kill -9 "$(basename "$(dirname "$p")")" 2>/dev/null ;; esac
  done
}

sudo systemctl stop emulationstation 2>/dev/null
kill_sdv
sleep 1
: > "$GAMEDIR/mem.txt"

cd /roms/ports || exit 1
SDV_GL_TRACE=1 nice -n 19 timeout -s KILL "$SECS" bash "Stardew Valley (NextOS).sh" > "$GAMEDIR/run.out" 2>&1 &
RUNPID=$!

for s in 30 60 90 120 180 240; do
  [ "$s" -lt "$SECS" ] || continue
  ( sleep "$s"
    for p in /proc/[0-9]*/exe; do
      t=$(readlink "$p" 2>/dev/null) || continue
      case "$t" in */ports/sdvnextos/stardewvalley*)
        pid=$(basename "$(dirname "$p")")
        echo "t=${s}s rss=$(awk '/VmRSS/{print $2}' /proc/$pid/status)kB swap=$(awk '/VmSwap/{print $2}' /proc/$pid/status)kB cpu=$(awk '{print $14+$15}' /proc/$pid/stat)" >> "$GAMEDIR/mem.txt" ;;
      esac
    done ) &
done

wait $RUNPID 2>/dev/null
kill_sdv
sleep 1
sudo systemctl start emulationstation 2>/dev/null
echo "=== FIM (${SECS}s) ==="
grep -E "\[egl_shim\]|\[sdv-egl\]|swap #|\[libs\]|\[sdv\]|Runtime|error|Error|FALHOU" "$GAMEDIR/debug.log" 2>/dev/null | tail -30
cat "$GAMEDIR/mem.txt" 2>/dev/null
