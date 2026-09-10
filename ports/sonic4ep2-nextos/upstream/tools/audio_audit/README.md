# Auditoria do mapa de audio (sfx_map.tsv) via AudioDataTbl do DEX

Reproduz o cruzamento que acha cues MUDAS (faltando no sfx_map.tsv).

1. `unzip -o sonic-the-hedgehog-4-episode-ii-2.0.0.apk 'classes*.dex'`
   (a `com/mineloader/fox/AudioDataTbl` esta em `classes2.dex`).
2. `python3 dexparse.py classes2.dex > dexmap.tsv`
   - parser DEX proprio (sem ferramenta externa): le os `GetCueMap_*` e
     extrai os const-string em ordem -> pares (cue, arquivo). Validado:
     bate 574/574 com o sfx_map verificado a mao, 0 divergencia.
3. Manifesto de OGGs do OBB (LPK, nao-zip):
   `strings -n6 main.22.*.obb | grep -i '\.ogg$' | sort -u > obb_oggs.txt`
4. `cp <repo>/package/ports/sonic4ep2/sfx_map.tsv cur_sfx.tsv`
5. `python3 audit.py` -> missing.tsv (cues mudas), mismatched.tsv, unresolved.tsv
6. novo sfx_map = `cat cur_sfx.tsv missing.tsv` (preserva antigo, so adiciona).

Resultado 2026-06-29: 78 cues mudas achadas (Metal Sonic/boss3, zone3/zone4),
mapeadas -> sfx_map.tsv 652->730 linhas, 0 regressao (antigo preservado).
