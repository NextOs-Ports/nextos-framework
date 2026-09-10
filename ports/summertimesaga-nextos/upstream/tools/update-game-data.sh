#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
GAME_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
XDG_DATA_HOME=${XDG_DATA_HOME:-"$HOME/.local/share"}

if [ -d /opt/system/Tools/PortMaster ]; then
  controlfolder=/opt/system/Tools/PortMaster
elif [ -d /opt/tools/PortMaster ]; then
  controlfolder=/opt/tools/PortMaster
elif [ -d "$XDG_DATA_HOME/PortMaster" ]; then
  controlfolder=$XDG_DATA_HOME/PortMaster
elif [ -d /roms/ports/PortMaster ]; then
  controlfolder=/roms/ports/PortMaster
else
  controlfolder=/storage/.config/PortMaster
fi

extractor_env=$GAME_DIR/nxextract-runtime-env.sh
[ -f "$extractor_env" ] && [ ! -L "$extractor_env" ] || {
  printf 'Summertime Saga: NXExtract runtime helper is missing\n' >&2
  exit 1
}
# shellcheck source=../nxextract-runtime-env.sh
source "$extractor_env"
declare -F summertime_run_extractor >/dev/null 2>&1 || {
  printf 'Summertime Saga: NXExtract runtime helper is incomplete\n' >&2
  exit 1
}

if [ "$#" -eq 1 ]; then
  case "$1" in
    -*) ;;
    *) set -- --input "$1" ;;
  esac
fi

machine=$(uname -m 2>/dev/null || true)
summertime_run_extractor "$GAME_DIR" "$controlfolder" / "$machine" \
  --force-source "$@"
