#!/bin/sh
# Standard PortMaster entry point.  The full universal runtime stays beside the
# game files so ports/ and ports_scripts/ share one implementation.
#
# Resolve o proprio caminho REAL antes de procurar: em alguns frontends o
# arquivo visivel e' um symlink ou copia, e `dirname $0` apontaria para o lugar
# errado. Uma lista fixa de raizes de ROM nao cobre todo CFW (muOS usa /mnt/mmc
# e /mnt/sdcard), entao a busca relativa ao script vem primeiro e sempre vale.

SELF=$0
[ -L "$SELF" ] && SELF=$(readlink -f -- "$SELF" 2>/dev/null || printf '%s' "$0")
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$SELF")" 2>/dev/null && pwd -P) ||
  SCRIPT_DIR=.

for launcher in \
  "$SCRIPT_DIR/pf2/pf2-nextos.sh" \
  "$SCRIPT_DIR/../ports/pf2/pf2-nextos.sh" \
  "$SCRIPT_DIR/../../ports/pf2/pf2-nextos.sh" \
  /roms/ports/pf2/pf2-nextos.sh \
  /roms2/ports/pf2/pf2-nextos.sh \
  /storage/roms/ports/pf2/pf2-nextos.sh \
  /mnt/mmc/ports/pf2/pf2-nextos.sh \
  /mnt/mmc/roms/ports/pf2/pf2-nextos.sh \
  /mnt/mmc/ROMS/ports/pf2/pf2-nextos.sh \
  /mnt/sdcard/ports/pf2/pf2-nextos.sh \
  /mnt/sdcard/roms/ports/pf2/pf2-nextos.sh \
  /mnt/sdcard/ROMS/ports/pf2/pf2-nextos.sh \
  /mnt/union/roms/ports/pf2/pf2-nextos.sh \
  /mnt/SDCARD/roms/ports/pf2/pf2-nextos.sh \
  /userdata/roms/ports/pf2/pf2-nextos.sh
do
  if [ -f "$launcher" ] && [ ! -L "$launcher" ]; then
    exec bash "$launcher" "$@"
  fi
done

# Falhar em silencio aqui foi o modo de falha reportado no muOS/RG40XXH: o
# frontend descarta stderr, o port voltava ao menu e NENHUM arquivo era gerado,
# entao nao havia o que reportar. Agora o erro fica gravado em disco.
message="Prizefighters 2: ports/pf2/pf2-nextos.sh not found. Put the folder \
\"pf2\" in the same ports folder as this script (roms/ports/pf2 on \
muOS/ArkOS/ROCKNIX). script=$SELF dir=$SCRIPT_DIR"
printf '%s\n' "$message" >&2
for spot in "$SCRIPT_DIR/pf2-launcher-error.log" \
            "${TMPDIR:-/tmp}/pf2-launcher-error.log"
do
  if printf '%s\n' "$message" > "$spot" 2>/dev/null; then
    printf 'Prizefighters 2: detalhes em %s\n' "$spot" >&2
    break
  fi
done
for console in "${CUR_TTY:-/dev/tty0}" /dev/tty1 /dev/console; do
  [ -w "$console" ] || continue
  printf '\n%s\n' "$message" >> "$console" 2>/dev/null && break
done
exit 1
