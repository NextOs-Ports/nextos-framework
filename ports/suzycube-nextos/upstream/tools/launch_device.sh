#!/bin/bash
# Roda NO DEVICE: abre o Suzy Cube pela entrada do EmulationStation, com o
# ambiente de teste passado por argumentos.  Existe como ARQUIVO justamente
# para nao depender de aspas aninhadas num comando ssh -- foi o que antes fez
# o lancamento sair errado e o veredito culpar o jogo.
#
#   launch_device.sh <roteiro-de-input> <quadros-de-print>
set -u
DST=/storage/roms/ports/suzycube
SCRIPT=${1:-}
SHOTS=${2:-}

matching_pids() {
  for proc in /proc/[0-9]*; do
    pid=${proc##*/}
    comm=$(cat "$proc/comm" 2>/dev/null || true)
    exe=$(readlink "$proc/exe" 2>/dev/null || true)
    cmdline=$(tr '\000' ' ' < "$proc/cmdline" 2>/dev/null || true)
    case "$comm|$exe|$cmdline" in
      *suzycube-nextos*) printf '%s\n' "$pid" ;;
    esac
  done
}

frontend_active=0
for proc in /proc/[0-9]*; do
  comm=$(cat "$proc/comm" 2>/dev/null || true)
  cmdline=$(tr '\000' ' ' < "$proc/cmdline" 2>/dev/null || true)
  case "$comm|$cmdline" in
    *emulationstation*|*EmulationStation*) frontend_active=1 ;;
  esac
done
[ "$frontend_active" -eq 0 ] || {
  echo "refusing remote launch while EmulationStation is active" >&2
  exit 8
}

old=$(matching_pids)
if [ -n "$old" ]; then
  printf '%s\n' "$old" | while IFS= read -r pid; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  loops=0
  while [ "$loops" -lt 20 ] && [ -n "$(matching_pids)" ]; do
    sleep 0.25
    loops=$((loops + 1))
  done
  old=$(matching_pids)
  if [ -n "$old" ]; then
    printf '%s\n' "$old" | while IFS= read -r pid; do
      kill -KILL "$pid" 2>/dev/null || true
    done
  fi
  [ -z "$(matching_pids)" ] || {
    echo "old Suzy Cube instance is still alive" >&2
    exit 8
  }
fi

cd "$DST" || exit 9
rm -f verify_shot.*.ppm

export SC_LOGCAT=1
export SC_FPS=1
export SC_AUDIO_TRACE=1
[ -n "$SCRIPT" ] && export SC_INPUTTEST="$SCRIPT"
if [ -n "$SHOTS" ]; then
  export SC_SCREENSHOT="$DST/verify_shot"
  export SC_SCREENSHOT_FRAME="$SHOTS"
fi

# Jogo aberto a pedido: sem timeout e sobrevivendo ao fim do ssh.
nohup bash "/storage/roms/ports/Suzy Cube.sh" < /dev/null > /dev/null 2>&1 &
echo "lancado pid=$!"
