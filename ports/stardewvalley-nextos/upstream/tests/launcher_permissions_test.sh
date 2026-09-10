#!/usr/bin/env bash
# Regressao v1.1.4: o launcher precisa restaurar o bit +x de TODO arquivo que o
# caminho de primeira instalacao executa.
#
# Por que isto existe: `nxextract-runtime-env.sh` executa `run-extractor.sh`
# diretamente, e `run-extractor.sh` so' roda a migracao do assembly store se
# `tools/prepare_stardew_data.py` passar num `[ -x ... ]`. Sem o bit, a migracao
# e' pulada EM SILENCIO — nada falha, o usuario so' fica sem os dados migrados.
#
# Nao da' para pegar isso no aparelho de bancada: la' o /roms e' exfat com
# fmask=0000, entao tudo e' sempre 0777 e o chmod nao tem efeito observavel. Em
# ROM partition ext4, ou quando o ZIP e' extraido por uma ferramenta que descarta
# os modos, o bit some de verdade.
set -euo pipefail

REPO=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/sdv-perms-test.XXXXXX")
cleanup() { rm -rf -- "$TMP_ROOT"; }
trap cleanup EXIT INT TERM

fail() {
  printf 'launcher permissions test failed: %s\n' "$*" >&2
  exit 1
}

GAMEDIR="$TMP_ROOT/ports/sdvnextos"
mkdir -p "$GAMEDIR/tools"

# Tudo chega sem bit de execucao, como numa extracao que perdeu os modos.
for relative in stardewvalley stardewvalley.multi run-extractor.sh \
                nxextract.py nxextract-ui tools/prepare_stardew_data.py; do
  printf '#!/bin/sh\nexit 0\n' > "$GAMEDIR/$relative"
  chmod 0644 "$GAMEDIR/$relative"
done
printf '1.1.5\n' > "$GAMEDIR/version.txt"
printf '{}\n' > "$GAMEDIR/extractor.json"
install -m 0755 "$REPO/run.sh" "$GAMEDIR/run.sh"

# Stub do helper: valida a integracao sem extrair 420 MB de dados reais.
cat > "$GAMEDIR/nxextract-runtime-env.sh" <<'SH'
sdv_run_extractor() { return 0; }
SH
chmod 0644 "$GAMEDIR/nxextract-runtime-env.sh"

# SDV_EXTRACTOR_ONLY=1 encerra logo apos a fase de dados, que e' o trecho testado.
( cd "$GAMEDIR" && SDV_EXTRACTOR_ONLY=1 HOME="$TMP_ROOT" \
    bash "$GAMEDIR/run.sh" >/dev/null 2>&1 ) ||
  fail "launcher exited non-zero during the data phase"

for relative in stardewvalley stardewvalley.multi run-extractor.sh \
                nxextract.py nxextract-ui tools/prepare_stardew_data.py; do
  [ -x "$GAMEDIR/$relative" ] ||
    fail "$relative is still not executable after the launcher ran"
done

# A guarda de symlink protege um `source`: um link plantado no lugar do helper
# executaria codigo arbitrario com os privilegios do jogo.
ln -sf /dev/null "$GAMEDIR/nxextract-runtime-env.sh"
if ( cd "$GAMEDIR" && SDV_EXTRACTOR_ONLY=1 HOME="$TMP_ROOT" \
       bash "$GAMEDIR/run.sh" >/dev/null 2>&1 ); then
  fail "launcher sourced a symlinked NXExtract helper"
fi

printf 'launcher permissions tests: PASS\n'
