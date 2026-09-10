#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Gate do nx-add-capability.
#
# Uma capability nova toca nove arquivos e o gate reclama de um por vez. O que
# este teste protege nao e' a conveniencia: e' que os nove fiquem COERENTES
# entre si. Dois deles falham do pior jeito se ficarem para tras --
# a tabela de nomes de test_nxcompat_registry.c faz o teste SEGFALTAR em vez de
# dar mensagem, e a allowlist do shell compara por ORDEM, nao por conjunto.
#
# Roda numa copia do framework em diretorio temporario; a arvore real nao e'
# tocada.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd -- "$HERE/../../.." && pwd -P)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/framework"
cp -a "$REPO_ROOT/framework/." "$WORK/framework/"

TOOL="$WORK/framework/tools/nx-add-capability.py"
NEW_ID=graphics.gate-probe

fail() { echo "nx_add_capability=FAIL $*" >&2; exit 1; }

before=$(python3 -c "
import json;print(len(json.load(open('$WORK/framework/nxcompat/capabilities-v1.json'))['capabilities']))")

# 1. dry-run nao escreve nada
python3 "$TOOL" --root "$WORK" --id "$NEW_ID" --phase graphics --source nxgl \
  --evidence opened --role optional-enhancement --dry-run >/dev/null
after=$(python3 -c "
import json;print(len(json.load(open('$WORK/framework/nxcompat/capabilities-v1.json'))['capabilities']))")
[ "$before" = "$after" ] || fail "dry-run escreveu no registry"

# 2. vocabulario invalido e' recusado, um campo de cada vez
for bad in "--phase inexistente" "--role inexistente" "--source inexistente"; do
  if python3 "$TOOL" --root "$WORK" --id x.y --phase graphics --source nxgl \
       --evidence opened --role optional-enhancement $bad >/dev/null 2>&1; then
    fail "aceitou $bad"
  fi
done

# 3. minimum_evidence absent/lost nao faz sentido e e' recusado
if python3 "$TOOL" --root "$WORK" --id x.y --phase graphics --source nxgl \
     --evidence absent --role optional-enhancement >/dev/null 2>&1; then
  fail "aceitou minimum_evidence=absent"
fi

# 4. a execucao de verdade
python3 "$TOOL" --root "$WORK" --id "$NEW_ID" --phase graphics --source nxgl \
  --evidence opened --role optional-enhancement --no-reseal >/dev/null

python3 - "$WORK" "$NEW_ID" <<'PY'
import hashlib, json, re, sys
root, new_id = sys.argv[1], sys.argv[2]
def read(rel): return open(root + "/" + rel, encoding="utf-8").read()
def bad(msg): print("nx_add_capability=FAIL " + msg, file=sys.stderr); raise SystemExit(1)

registry = json.loads(read("framework/nxcompat/capabilities-v1.json"))
ids = [c["id"] for c in registry["capabilities"]]
count = len(ids)
suffix = new_id.replace(".", "_").replace("-", "_").upper()

# entra no FIM: renumerar ids ja' gravados em recibos quebraria a leitura deles
if ids[-1] != new_id:
    bad("a capability nova nao ficou no fim do registry")

header = read("framework/nxcompat/include/nxcompat.h")
if "NXCOMPAT_CAPABILITY_%s = %d" % (suffix, count - 1) not in header:
    bad("enum de id ausente ou com numero errado")
if "#define NXCOMPAT_CAPABILITY_COUNT %du" % count not in header:
    bad("NXCOMPAT_CAPABILITY_COUNT nao acompanhou")

registry_c = read("framework/nxcompat/src/nxcompat_registry.c")
if '{%du, "%s"' % (count - 1, new_id) not in registry_c:
    bad("entrada ausente na tabela C")

# esquecer aqui faz o teste SEGFALTAR, nao falhar com mensagem
names = read("framework/nxcompat/tests/test_nxcompat_registry.c")
if '"%s"};' % new_id not in names:
    bad("tabela de nomes do teste do registry nao recebeu o nome")

m12 = read("framework/nxcompat/tests/test_m12_audit.py")
for token in ('("%s", ' % new_id, "len(capabilities) == %d" % count,
              "tuple(range(%d))" % count, '"%s",' % suffix,
              "NXCOMPAT_CAPABILITY_COUNT %du" % count):
    if token not in m12:
        bad("auditoria m12 sem: %s" % token)

contract = read("framework/nxbootstrap/tests/test-manifest-contract.py")
if "len(identifiers) == %d" % count not in contract:
    bad("contrato do manifesto com total errado")
for prefix in ("host", "graphics", "audio", "input"):
    total = sum(i.startswith(prefix + ".") for i in ids)
    if '"%s": %d' % (prefix, total) not in contract:
        bad("contagem do namespace %s errada" % prefix)

schema = json.loads(read("framework/nxbootstrap/schema/nxport-v2.schema.json"))
# o gate compara por ORDEM, nao por conjunto
if schema["properties"]["required_capabilities"]["items"]["enum"] != ids:
    bad("enum do schema fora da ordem do registry")

shell = read("framework/nxbootstrap/nxbootstrap.sh")
allow = re.search(r"nxbootstrap_capability_known\(\) \{(.*?)\n\}", shell, re.S)
found = re.findall(r"(?:host|graphics|audio|input)\.[a-z0-9][a-z0-9.-]{0,62}",
                   allow.group(1))
if found != ids:
    bad("allowlist do shell fora da ordem do registry")

pin = re.search(r'"([0-9a-f]{64})"',
                read("framework/nxbootstrap/tests/test-068-preservation.py"))
digest = hashlib.sha256(
    open(root + "/framework/nxbootstrap/nxbootstrap.sh", "rb").read()).hexdigest()
if pin.group(1) != digest:
    bad("o pin do runtime aposentado nao foi reselado junto")
PY

# 5. duplicata e' recusada
if python3 "$TOOL" --root "$WORK" --id "$NEW_ID" --phase graphics \
     --source nxgl --evidence opened --role optional-enhancement \
     >/dev/null 2>&1; then
  fail "aceitou uma capability que ja' existia"
fi

echo "nx_add_capability=PASS 5 cenarios, 9 arquivos conferidos"
