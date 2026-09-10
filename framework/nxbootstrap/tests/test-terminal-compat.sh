#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# COMPATIBILIDADE DO TERMINAL-RESULT (crise de campo 19/08): dois testers muOS
# ficaram com launcher ANTIGO (.sh nao substituido no update) + engine NOVO, e
# o validador embutido estrito matou a fase nxextract ("invalid terminal result
# object" / "unknown terminal result schema").
#
# Este gate prova as DUAS pontas:
#  1. Os validadores LEGADOS (0.6.16 e 0.6.21, capturados byte-a-byte do git —
#     identicos aos que estao no campo) REPRODUZEM o erro contra um resultado
#     do engine 1.2.12: a classe e real e nomeada.
#  2. O validador ATUAL (0.6.26+) e forward/backward-compatible DENTRO do
#     schema_version 1: aceita membro extra desconhecido, aceita engine mais
#     novo E mais velho (sem `ui`), e continua rejeitando o que deve rejeitar
#     (schema_version 2, membro obrigatorio faltando, schema errado).
# Regra de ouro selada: o contrato de compat e schema + schema_version; a
# lista de membros e a versao do engine NUNCA quebram launcher publicado.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_ROOT=$(cd -- "$HERE/.." && pwd -P)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nx-terminal-compat.XXXXXX")
trap 'rm -rf "$TEST_ROOT"' EXIT

fail() { printf 'terminal-compat: FAIL %s\n' "$1" >&2; exit 1; }

# ---- validador ATUAL: extraido de um launcher recem-gerado (nao do fonte) --
GEN_OUT=$TEST_ROOT/generated
python3 -B "$PROJECT_ROOT/tools/generate-port.py" \
  "$PROJECT_ROOT/examples/nxport.example.json" --output "$GEN_OUT" >/dev/null
LAUNCHER=$(find "$GEN_OUT" -maxdepth 1 -name '*.sh' | head -1)
python3 - "$LAUNCHER" "$TEST_ROOT/current.py" <<'PY'
import sys
src = open(sys.argv[1], encoding="utf-8").read()
start = src.index("nxbootstrap_validate_nxextract_result() {")
blk = src[start:]
a = blk.index("<<'PY'") + len("<<'PY'") + 1
b = blk.index("\nPY\n")
open(sys.argv[2], "w", encoding="utf-8").write(blk[a:b])
PY

LEGACY_DIR=$PROJECT_ROOT/tests/fixtures/legacy-terminal-validators

# ---- fixtures de terminal-result -------------------------------------------
mk_result() { # $1=outfile  $2=python-dict-mutations
  python3 - "$TEST_ROOT/$1" "$2" <<'PY'
import json, sys
base = {
    "schema": "org.nextos.nxextract.terminal-result",
    "schema_version": 1,
    "nxextract_version": "1.2.12",
    "outcome": "success",
    "code": "NXE0000",
    "final_phase": {"index": 8, "id": "ready", "label": "Ready"},
    "recipe": {"id": "compat-fixture", "version": "1", "digest": "0" * 64},
    "package_id": "org.nextos.compat",
    "abi": "arm64-v8a",
    "container": {"kind": "existing", "identity": "1" * 64},
    "validated": {"items": 1, "bytes": 1, "critical_payloads": []},
    "logs": {"summary": "nxextract.log", "detail": "nxextract-detail.log"},
    "duration_ms": 1,
    "completed_unix": 1,
    "ui": {"mode": "visible", "renderer": "sdl", "fallback_reason": None},
    "error": None,
}
exec(sys.argv[2], {"base": base})
with open(sys.argv[1], "w", encoding="utf-8") as fh:
    json.dump(base, fh, sort_keys=True, separators=(",", ":"))
    fh.write("\n")
PY
}

mk_result current.json  "pass"
mk_result future.json   "base['nxextract_version']='1.9.9'; base['future_member']={'novo':True}"
mk_result oldstyle.json "del base['ui']; base['nxextract_version']='1.2.10'"
mk_result breaking.json "base['schema_version']=2"
mk_result broken.json   "del base['code']"

run_validator() { # $1=validator.py $2=fixture -> ecoa 'accept' ou 'reject'
  if python3 -B "$1" "$TEST_ROOT/$2" 0 >/dev/null 2>&1; then
    echo accept
  else
    echo reject
  fi
}

expect() { # $1=validator $2=fixture $3=esperado $4=descricao
  local got
  got=$(run_validator "$1" "$2")
  [[ $got == "$3" ]] || fail "$4 (esperado=$3, obtido=$got)"
}

CUR=$TEST_ROOT/current.py

# ---- 1. validador ATUAL: compat nos dois sentidos --------------------------
expect "$CUR" current.json  accept "atual rejeitou o resultado corrente com ui"
expect "$CUR" future.json   accept "atual rejeitou engine FUTURO + membro novo (classe do campo!)"
expect "$CUR" oldstyle.json accept "atual rejeitou engine ANTIGO sem ui (hibrido inverso)"
expect "$CUR" breaking.json reject "atual aceitou schema_version 2 (breaking de verdade)"
expect "$CUR" broken.json   reject "atual aceitou resultado sem membro obrigatorio"

# ---- 2. legados reproduzem o erro do campo (prova da classe) ---------------
expect "$LEGACY_DIR/launcher-0.6.16.py" current.json reject \
  "legado 0.6.16 deveria reproduzir 'invalid terminal result object'"
expect "$LEGACY_DIR/launcher-0.6.21.py" current.json reject \
  "legado 0.6.21 deveria reproduzir 'unknown terminal result schema'"
expect "$LEGACY_DIR/launcher-0.6.16.py" oldstyle.json accept \
  "legado 0.6.16 deveria aceitar o resultado da propria epoca (fixture valida)"

echo "terminal-compat: PASS atual=5 legados=3 (classe do campo reproduzida e morta)"
