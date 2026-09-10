#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# nx-prepare-data — prepara os dados de UM port no DESKTOP usando o MOTOR
# CANONICO do NXExtract, com o extractor.json do proprio port.
#
# Onda v2 (AUD-36): oito ports reimplementavam a extracao de XAPK/split em
# prepare-data.sh artesanais (sem CRC, sem escrita atomica, oito md5
# diferentes). O device e o desktop passam a usar EXATAMENTE o mesmo codigo
# auditado: quando o motor ganha um conserto, a preparacao local ganha junto.
#
# Uso:
#   framework/tools/nx-prepare-data.sh --port-dir ports/<slug> \
#       [--work DIR]   # area de trabalho (default: <port>/gamedata_staging)
#
# Os arquivos do jogo (BYO) ficam onde o port ja' espera: <port>/gamedata/.
# A saida e' a MESMA arvore que o extractor produziria no device.
set -euo pipefail

usage() {
  echo "uso: nx-prepare-data.sh --port-dir DIR [--work DIR]" >&2
  exit 2
}

PORT_DIR=""
WORK_DIR=""
while [ $# -gt 0 ]; do
  case "$1" in
    --port-dir) PORT_DIR=${2:?}; shift 2 ;;
    --work) WORK_DIR=${2:?}; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$PORT_DIR" ] || usage
PORT_DIR=$(CDPATH= cd -- "$PORT_DIR" && pwd -P)
[ -f "$PORT_DIR/extractor.json" ] ||
  { echo "nx-prepare-data: sem extractor.json em $PORT_DIR" >&2; exit 1; }

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(dirname -- "$(dirname -- "$SCRIPT_DIR")")
ENGINE_ROOT="$REPO_ROOT/suportando_outros_devices/extrator-universal"
[ -f "$ENGINE_ROOT/nxextract.py" ] ||
  { echo "nx-prepare-data: motor canonico ausente em $ENGINE_ROOT" >&2; exit 1; }

WORK_DIR=${WORK_DIR:-$PORT_DIR}

# O motor roda com o game-dir do port (le gamedata/, escreve data/ etc.).
# Sem --require-ui: no desktop a UI grafica nao abre e o motor cai sozinho no
# modo headless (barra de progresso e' cosmetica; o recibo terminal e' o que
# vale), registrando ui_fallback=headless no resultado.
python3 -B "$ENGINE_ROOT/nxextract.py" install \
  --recipe "$PORT_DIR/extractor.json" \
  --game-dir "$WORK_DIR" \
  "$@"
