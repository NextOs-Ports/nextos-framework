#!/bin/sh
# Ponto de entrada NextOS/PortMaster. O run.sh do port cuida do ciclo de vida;
# nada aqui toca no EmulationStation.
#
# Resolve o proprio caminho REAL antes de procurar: em alguns frontends o
# arquivo visivel e' um symlink ou copia, e `dirname $0` apontaria pro lugar
# errado. A busca relativa ao script vem primeiro e sempre vale; a lista fixa
# cobre os layouts de ROM conhecidos por CFW.
SELF=$0
[ -L "$SELF" ] && SELF=$(readlink -f -- "$SELF" 2>/dev/null || printf '%s' "$0")
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$SELF")" 2>/dev/null && pwd -P) ||
  exit 1

for launcher in \
  "$SCRIPT_DIR/geometrydash/run.sh" \
  "$SCRIPT_DIR/../ports/geometrydash/run.sh" \
  "$SCRIPT_DIR/../../ports/geometrydash/run.sh" \
  /roms/ports/geometrydash/run.sh \
  /roms2/ports/geometrydash/run.sh \
  /storage/roms/ports/geometrydash/run.sh \
  /mnt/mmc/ports/geometrydash/run.sh \
  /mnt/mmc/roms/ports/geometrydash/run.sh \
  /mnt/mmc/ROMS/ports/geometrydash/run.sh \
  /mnt/sdcard/ports/geometrydash/run.sh \
  /mnt/sdcard/roms/ports/geometrydash/run.sh \
  /mnt/sdcard/ROMS/ports/geometrydash/run.sh \
  /userdata/roms/ports/geometrydash/run.sh
do
  if [ -f "$launcher" ] && [ ! -L "$launcher" ]; then
    # bash, nao sh: o runtime usa BASH_SOURCE e `source` do control.txt
    exec bash "$launcher" "$@"
  fi
done

# Falhar em silencio e' o pior modo de falha: o frontend descarta stderr e o
# port volta ao menu sem deixar rastro. O erro fica gravado em disco.
message="Geometry Dash: geometrydash/run.sh nao encontrado (script=$SELF dir=$SCRIPT_DIR)"
printf '%s\n' "$message" >&2
for spot in "$SCRIPT_DIR/geometrydash-launcher-error.log" \
            "${TMPDIR:-/tmp}/geometrydash-launcher-error.log"
do
  printf '%s\n' "$message" > "$spot" 2>/dev/null && break
done
exit 72
