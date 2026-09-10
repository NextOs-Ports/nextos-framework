#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Tearscape 0.2.16 — kit de prova WAYLAND_GEOMETRY_PROOF em UM comando, fora do
# ZIP público. Para um aparelho ROCKNIX/Sway (RP5) acessível por SSH:
#
#   recipes/device-kit/rp5-wayland-geometry-kit.sh --host IP --user root \
#       --tree tearscape-0216-public.tar --out ./rp5-proof-bundle
#
# O que faz, na ordem, sem pedir dedo a ninguém:
#   1. instala a árvore gerada (mesmos bytes do candidato) em /roms/ports (ou
#      $PORTS_ROOT), preservando gamedata/ do dono;
#   2. abre o jogo pelo ambiente do frontend (nxobs/nx-device-launch.sh), que lê
#      a unit real do frontend em vez de presumir variáveis;
#   3. durante a tela do jogo captura `swaymsg -t get_outputs -r`,
#      `swaymsg -t get_tree -r`, o recibo nx-geometry-proof/1
#      (nxgeometry-receipt.jsonl), o log e o recibo de vídeo do MESMO run;
#   4. roda o consumidor make_geometry_proof.py (falha em portrait/cortado,
#      app_id vazio, timeout de configure, drawable stale, tupla de run errada);
#   5. entrega um bundle SANITIZADO (sem IP, sem caminhos pessoais, sem dados do
#      jogo) em --out, com SHA256SUMS.
# O gate de controles (nx-device-input-proof.py) roda separado, na mesma árvore.
set -euo pipefail
HOST="" USER_NAME="root" TREE="" OUT="" PORTS_ROOT="/roms/ports" SECONDS_TO_RUN=75
KIT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PORT_DIR=$(cd -- "$KIT_DIR/../.." && pwd -P)
REPO_ROOT=$(cd -- "$PORT_DIR/../.." && pwd -P)
LAUNCH="$REPO_ROOT/framework/nxobs/nx-device-launch.sh"
fail() { printf 'rp5-kit: %s\n' "$*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case $1 in
    --host) HOST=${2:-}; shift 2 ;;
    --user) USER_NAME=${2:-}; shift 2 ;;
    --tree) TREE=${2:-}; shift 2 ;;
    --out) OUT=${2:-}; shift 2 ;;
    --ports-root) PORTS_ROOT=${2:-}; shift 2 ;;
    --seconds) SECONDS_TO_RUN=${2:-}; shift 2 ;;
    *) fail "argumento desconhecido: $1" ;;
  esac
done
[ -n "$HOST" ] && [ -n "$TREE" ] && [ -n "$OUT" ] || fail "uso: --host IP --user USER --tree ARVORE.tar --out DIR"
[ -f "$TREE" ] || fail "árvore não encontrada: $TREE"
[ -x "$LAUNCH" ] || fail "nx-device-launch.sh ausente em $LAUNCH"
[ ! -e "$OUT" ] || fail "--out já existe: $OUT"
SSH=(ssh -o BatchMode=yes "$USER_NAME@$HOST")
mkdir -p "$OUT"
# 1. instalar preservando gamedata
scp -q "$TREE" "$USER_NAME@$HOST:/tmp/tearscape-kit.tar"
"${SSH[@]}" "set -e; cd '$PORTS_ROOT'; if pgrep -f '[t]earscape-nextos' >/dev/null; then echo 'jogo já em execução' >&2; exit 2; fi; \
  [ -d tearscape/gamedata ] && mv tearscape/gamedata /tmp/tearscape-gamedata-keep || true; \
  rm -rf tearscape Tearscape.sh; tar -xf /tmp/tearscape-kit.tar; rm -rf tearscape/gamedata; \
  [ -d /tmp/tearscape-gamedata-keep ] && mv /tmp/tearscape-gamedata-keep tearscape/gamedata || true; chmod +x Tearscape.sh; \
  rm -f tearscape/log.txt tearscape/nxgeometry-receipt.jsonl; sha256sum tearscape/tearscape-nextos"
# 2. abrir pelo frontend e 3. capturar durante a tela do jogo (em paralelo)
( sleep 35; "${SSH[@]}" "swaymsg -t get_outputs -r 2>/dev/null || echo '{\"error\":\"swaymsg unavailable\"}'" > "$OUT/sway-outputs.json"; \
  "${SSH[@]}" "swaymsg -t get_tree -r 2>/dev/null || echo '{\"error\":\"swaymsg unavailable\"}'" > "$OUT/sway-tree.json" ) &
CAP=$!
"$LAUNCH" --host "$HOST" --user "$USER_NAME" --launcher "$PORTS_ROOT/Tearscape.sh" --seconds "$SECONDS_TO_RUN" > "$OUT/launch.log" 2>&1 || true
wait "$CAP" || true
scp -q "$USER_NAME@$HOST:$PORTS_ROOT/tearscape/log.txt" "$OUT/log.txt" || true
scp -q "$USER_NAME@$HOST:$PORTS_ROOT/tearscape/nxgeometry-receipt.jsonl" "$OUT/nxgeometry-receipt.jsonl" || true
# 4. consumidor
RUN_ID=$(grep -oE 'NXOBS_RUN_ID=[^ ]+|receipt_run=[A-Za-z0-9._-]+' "$OUT/log.txt" | head -1 | cut -d= -f2 || true)
GEN=$(grep -oE 'generation ([0-9a-f]{64})' "$OUT/log.txt" | head -1 | awk '{print $2}' || true)
if [ -s "$OUT/nxgeometry-receipt.jsonl" ]; then
  python3 "$PORT_DIR/recipes/make_geometry_proof.py" "$OUT/nxgeometry-receipt.jsonl" ${RUN_ID:+--run-id "$RUN_ID"} ${GEN:+--generation "$GEN"} > "$OUT/geometry-verdict.txt" 2>&1 || true
else
  echo "VERDICT: FAIL no nxgeometry-receipt.jsonl produced" > "$OUT/geometry-verdict.txt"
fi
# 5. sanitizar: nunca IP, nunca caminho pessoal
for f in "$OUT"/*; do [ -f "$f" ] && sed -i -E 's#[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}#<device>#g; s#/home/[^/ ]+#/home/<user>#g' "$f"; done
( cd "$OUT" && sha256sum -- * > SHA256SUMS )
echo "rp5-kit: bundle em $OUT"; cat "$OUT/geometry-verdict.txt" | tail -3
