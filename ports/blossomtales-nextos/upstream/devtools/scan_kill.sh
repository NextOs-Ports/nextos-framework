#!/bin/bash
# scan_kill.sh -- varre e mata tudo que pertenca ao diretorio do port.
# Roda como root, numa unica invocacao. Ver o cabecalho de run137.sh.
G="${NX_PORTDIR:?}"

ignore=" $$ ${PPID:-} "
pid="${NX_SELF:-}"
while [ -n "$pid" ] && [ "$pid" != 0 ] && [ "$pid" != 1 ]; do
  ignore="$ignore$pid "
  pid=$(awk '{print $4}' "/proc/$pid/stat" 2>/dev/null)
done

scan() {
  for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    case "$ignore" in *" $pid "*) continue;; esac
    exe=$(readlink "$p/exe" 2>/dev/null)
    exe=${exe% (deleted)}
    case "$exe" in "$G"/*) echo "$pid"; continue;; esac
    comm=$(cat "$p/comm" 2>/dev/null)
    case "$comm" in blossomtales*) echo "$pid"; continue;; esac
    cwd=$(readlink "$p/cwd" 2>/dev/null)
    cl=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$cwd" in
      "$G"|"$G"/*) case "$cl" in *blossomtales-nextos*) echo "$pid";; esac;;
    esac
  done
}

for sig in TERM KILL; do
  pids=$(scan)
  [ -z "$pids" ] && break
  echo "  sinal $sig -> $(echo $pids | tr '\n' ' ')"
  kill "-$sig" $pids 2>/dev/null
  sleep 2
done
left=$(scan | wc -l)
echo "instancias do jogo vivas: $left"
[ "$left" -eq 0 ]
