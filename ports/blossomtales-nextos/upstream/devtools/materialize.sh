#!/bin/bash
# materialize.sh -- gera o projeto V4 e traz os artefatos GERADOS para a pasta
# do port, que e' a raiz de origem do nxrelease.
#
# Ordem obrigatoria: assemble_runtime.sh (pina generation_runtime) -> nxgenerator
# -> copia -> make_nxrelease.py. Pular um passo faz o manifesto descrever bytes
# que nao existem mais.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${1:-/tmp/bt-gen}"

cd "$REPO"
"$HERE/devtools/assemble_runtime.sh"
rm -rf "$OUT"
python3 framework/nxgenerator/nxgenerator.py \
  --output "$OUT" --source-root ports/blossomtales ports/blossomtales/nxproject.json

cd "$HERE"
rm -rf .nxruntime defaults adapter GENERATION.json gameinfo.xml nxport.json \
       port.json nxsplash-nextos nxruntime-*.nxb "Blossom Tales.sh"
cp -a "$OUT/blossomtales/." .
cp -a "$OUT/Blossom Tales.sh" .
chmod 0755 "Blossom Tales.sh" blossomtales-nextos nxsplash-nextos \
           nxextract/nxextract-ui

python3 devtools/make_nxrelease.py "$OUT" nxrelease.json
echo "materializado em $HERE"
