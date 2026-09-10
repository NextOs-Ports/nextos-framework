#!/bin/sh
# NextOS/PortMaster entry point. O launcher interno e' quem manda no ciclo de
# vida; este arquivo so' encontra o run.sh e sai da frente.
#
# Resolve o proprio caminho REAL antes de procurar: em alguns frontends o
# arquivo visivel e' symlink ou copia (muOS faz isso), e `dirname $0` apontaria
# para o lugar errado. A busca relativa ao script vem primeiro e sempre vale; a
# lista fixa cobre os layouts de ROM conhecidos por CFW.

SELF=$0
[ -L "$SELF" ] && SELF=$(readlink -f -- "$SELF" 2>/dev/null || printf '%s' "$0")
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$SELF")" 2>/dev/null && pwd -P) ||
  exit 1

for launcher in \
  "$SCRIPT_DIR/castleofillusion/run.sh" \
  "$SCRIPT_DIR/../ports/castleofillusion/run.sh" \
  "$SCRIPT_DIR/../../ports/castleofillusion/run.sh" \
  /roms/ports/castleofillusion/run.sh \
  /roms2/ports/castleofillusion/run.sh \
  /storage/roms/ports/castleofillusion/run.sh \
  /mnt/mmc/ports/castleofillusion/run.sh \
  /mnt/mmc/roms/ports/castleofillusion/run.sh \
  /mnt/mmc/ROMS/ports/castleofillusion/run.sh \
  /mnt/sdcard/ports/castleofillusion/run.sh \
  /mnt/sdcard/roms/ports/castleofillusion/run.sh \
  /mnt/sdcard/ROMS/ports/castleofillusion/run.sh \
  /userdata/roms/ports/castleofillusion/run.sh
do
  if [ -f "$launcher" ] && [ ! -L "$launcher" ]; then
    exec bash "$launcher" "$@"
  fi
done

# Falhar em silencio e' o pior modo de falha: o frontend descarta stderr, o port
# volta ao menu e NENHUM arquivo e' gerado. O erro fica gravado em disco.
message="Castle of Illusion: castleofillusion/run.sh not found (script=$SELF dir=$SCRIPT_DIR)"
printf '%s\n' "$message" >&2
for spot in "$SCRIPT_DIR/castleofillusion-launcher-error.log" \
            "${TMPDIR:-/tmp}/castleofillusion-launcher-error.log"
do
  if printf '%s\n' "$message" > "$spot" 2>/dev/null; then
    printf 'Castle of Illusion: details in %s\n' "$spot" >&2
    break
  fi
done
exit 1
