#!/usr/bin/env bash
# Suzy Cube -- monta a arvore assets/bin/Data a partir do APK + OBB embutido.
#
# O APK traz Data fatiado em .splitN e o OBB (assets/sc.txt -> zip -> .obb)
# traz o resto, tambem fatiado.  O merge e' em ORDEM NUMERICA: `ls` coloca
# split10 antes de split9 e corrompe o arquivo silenciosamente.
#
# Dados NUNCA entram no repo (regra #14b): tudo vive no staging fora dele.
set -euo pipefail

APK=${1:?uso: stage_data.sh <apk> <staging-dir>}
STG=${2:?uso: stage_data.sh <apk> <staging-dir>}

WORK=$STG/work
OUT=$STG/stage
rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT/lib" "$OUT/assets/bin/Data"

echo "[stage] extraindo APK"
unzip -q -o "$APK" 'assets/*' 'lib/arm64-v8a/*' classes.dex -d "$WORK"
cp "$WORK"/lib/arm64-v8a/*.so "$OUT/lib/"

echo "[stage] extraindo OBB embutido (assets/sc.txt)"
unzip -q -o "$WORK/assets/sc.txt" -d "$WORK/obbzip"
OBB=$(find "$WORK/obbzip" -name '*.obb' -type f | head -1)
[ -n "$OBB" ] || { echo "[stage] OBB nao encontrado dentro de sc.txt" >&2; exit 1; }
unzip -q -o "$OBB" -d "$WORK/obb"

# Une as duas arvores Data numa so; o OBB nunca sobrepoe o APK (nao ha colisao,
# mas se houver o do APK e' o canonico).
merge_tree() {
  local src=$1
  [ -d "$src" ] || return 0
  (cd "$src" && find . -type f -print0) |
    while IFS= read -r -d '' rel; do
      mkdir -p "$OUT/assets/bin/Data/$(dirname "$rel")"
      cp -n "$src/$rel" "$OUT/assets/bin/Data/$rel"
    done
}
echo "[stage] copiando Data do APK"
merge_tree "$WORK/assets/bin/Data"
echo "[stage] copiando Data do OBB"
merge_tree "$WORK/obb/assets/bin/Data"

echo "[stage] juntando os .splitN em ordem NUMERICA"
python3 - "$OUT/assets/bin/Data" <<'PY'
import os, re, sys
root = sys.argv[1]
pat = re.compile(r'^(.*)\.split(\d+)$')
groups = {}
for dirpath, _dirs, files in os.walk(root):
    for f in files:
        m = pat.match(f)
        if m:
            groups.setdefault(os.path.join(dirpath, m.group(1)), []).append(
                (int(m.group(2)), os.path.join(dirpath, f)))
n = 0
for base, parts in sorted(groups.items()):
    parts.sort(key=lambda p: p[0])           # ORDEM NUMERICA, nao lexicografica
    idx = [p[0] for p in parts]
    if idx != list(range(len(idx))):
        sys.exit(f"[stage] split faltando em {base}: {idx}")
    with open(base, 'wb') as out:
        for _, p in parts:
            with open(p, 'rb') as fh:
                while True:
                    chunk = fh.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
    for _, p in parts:
        os.remove(p)
    n += 1
print(f"[stage] {n} arquivos remontados")
PY

# boot.config do Android pede o splash nativo/debugger; deixamos o do jogo.
echo "[stage] arvore final:"
find "$OUT/assets/bin/Data" -maxdepth 1 -type f | wc -l
du -sh "$OUT"
