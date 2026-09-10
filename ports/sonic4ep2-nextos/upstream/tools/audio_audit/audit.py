#!/usr/bin/env python3
# Auditoria completa do mapa de audio: cruza a AudioDataTbl (DEX) com o manifesto
# real do OBB e com o sfx_map.tsv atual -> lista cues FALTANDO (mudas).
import sys

# manifesto OBB: upper(nome) -> nome real
obb = {}
for line in open("obb_oggs.txt"):
    f = line.strip()
    if f: obb[f.upper()] = f

# dexmap: (bank, cue, basename)
dex = []
for line in open("dexmap.tsv"):
    if line.startswith("#") or not line.strip(): continue
    b, c, base = line.rstrip("\n").split("\t")
    dex.append((b, c, base))

def resolve(base):
    key = (base + ".OGG").upper()
    return obb.get(key)

# sfx_map atual: set de (bank,cue) e (bank,cue,file)
cur_pairs = set(); cur_files = {}
for line in open("cur_sfx.tsv"):
    if not line.strip(): continue
    parts = line.rstrip("\n").split("\t")
    if len(parts) < 3: continue
    b, c, f = parts[0], parts[1], parts[2]
    cur_pairs.add((b, c)); cur_files[(b, c)] = f

# resolver DEX e classificar
resolved = []; unresolved = []
for b, c, base in dex:
    real = resolve(base)
    if real: resolved.append((b, c, real, base))
    else: unresolved.append((b, c, base))

# DEDUP do DEX (uma cue pode repetir identica)
dex_map = {}
for b, c, real, base in resolved:
    dex_map[(b, c)] = real

missing = [(b, c, real) for (b, c), real in dex_map.items() if (b, c) not in cur_pairs]
# tambem: cues presentes mas com arquivo DIFERENTE (possivel som errado)
mismatched = [(b, c, cur_files[(b,c)], real) for (b,c),real in dex_map.items()
              if (b,c) in cur_pairs and cur_files[(b,c)].upper() != real.upper()]

print(f"=== RESUMO ===")
print(f"DEX pares resolvidos no OBB : {len(dex_map)} (unicos bank+cue)")
print(f"DEX nao-resolviveis no OBB  : {len(unresolved)}")
print(f"sfx_map atual (bank,cue)    : {len(cur_pairs)}")
print(f"FALTANDO (mudas)            : {len(missing)}")
print(f"DIVERGENTES (arquivo difere): {len(mismatched)}")

# foco Metal Sonic / boss
def is_metal(c):
    cl=c.lower(); return ("boss3" in cl) or ("metal" in cl) or c.startswith("MS_") or ("metalunit" in cl)
mm = [(b,c,r) for (b,c,r) in missing if is_metal(c)]
print(f"\n=== FALTANDO Metal Sonic/boss3 ({len(mm)}) ===")
for b,c,r in sorted(mm): print(f"  {b}\t{c}\t{r}")

print(f"\n=== TODAS as faltantes por banco ===")
from collections import Counter
cnt = Counter(b for b,c,r in missing)
for b,n in sorted(cnt.items()): print(f"  {b}: {n}")

# dump das faltantes p/ aplicar
with open("missing.tsv","w") as o:
    for b,c,r in sorted(missing):
        o.write(f"{b}\t{c}\t{r}\n")
# dump dos divergentes p/ revisar
with open("mismatched.tsv","w") as o:
    for b,c,cf,r in sorted(mismatched):
        o.write(f"{b}\t{c}\tatual={cf}\tDEX={r}\n")
# dump nao-resolviveis
with open("unresolved.tsv","w") as o:
    for b,c,base in sorted(set(unresolved)):
        o.write(f"{b}\t{c}\t{base}\n")
print("\n(missing.tsv, mismatched.tsv, unresolved.tsv gravados)")
