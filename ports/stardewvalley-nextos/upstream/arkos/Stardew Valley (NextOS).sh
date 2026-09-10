#!/bin/sh
# Standard PortMaster entry point. The full multi-device runtime stays beside
# the game files so ports/ and ports_scripts/ share one implementation.
#
# Resolve o proprio caminho REAL antes de procurar: em alguns frontends o
# arquivo visivel e' um symlink ou copia, e `dirname $0` apontaria para o lugar
# errado. Uma lista fixa de raizes de ROM nao cobre todo CFW (muOS usa /mnt/mmc
# e /mnt/sdcard), entao a busca relativa ao script vem primeiro e sempre vale.

SELF=$0
[ -L "$SELF" ] && SELF=$(readlink -f -- "$SELF" 2>/dev/null || printf '%s' "$0")
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$SELF")" 2>/dev/null && pwd -P) ||
  exit 1

for launcher in \
  "$SCRIPT_DIR/sdvnextos/run.sh" \
  "$SCRIPT_DIR/../ports/sdvnextos/run.sh" \
  "$SCRIPT_DIR/../../ports/sdvnextos/run.sh" \
  /roms/ports/sdvnextos/run.sh \
  /roms2/ports/sdvnextos/run.sh \
  /storage/roms/ports/sdvnextos/run.sh \
  /mnt/mmc/ports/sdvnextos/run.sh \
  /mnt/mmc/roms/ports/sdvnextos/run.sh \
  /mnt/mmc/ROMS/ports/sdvnextos/run.sh \
  /mnt/sdcard/ports/sdvnextos/run.sh \
  /mnt/sdcard/roms/ports/sdvnextos/run.sh \
  /mnt/sdcard/ROMS/ports/sdvnextos/run.sh \
  /userdata/roms/ports/sdvnextos/run.sh
do
  if [ -f "$launcher" ] && [ ! -L "$launcher" ]; then
    exec bash "$launcher" "$@"
  fi
done

# Falhar em silencio aqui foi o pior modo de falha da v1.1.5: o frontend descarta
# stderr, o port voltava ao menu e NENHUM arquivo era gerado, entao nao havia o
# que reportar. Agora o erro fica gravado em disco em algum lugar legivel.
message="Stardew Valley: ports/sdvnextos/run.sh not found (script=$SELF dir=$SCRIPT_DIR)"
printf '%s\n' "$message" >&2
for spot in "$SCRIPT_DIR/stardew-launcher-error.log" \
            "${TMPDIR:-/tmp}/stardew-launcher-error.log"
do
  if printf '%s\n' "$message" > "$spot" 2>/dev/null; then
    printf 'Stardew Valley: detalhes em %s\n' "$spot" >&2
    break
  fi
done
exit 1
