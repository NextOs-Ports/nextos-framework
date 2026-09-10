# NXExtract framework evidence

The canonical NXExtract source remains in
`suportando_outros_devices/extrator-universal`. This directory contains the
framework-level evidence ledger for milestone M07; it does not duplicate or
vendor the extractor.

`m07-audit-v1.json` preserves the historical M07 evidence. The current
1.3.0 component contract is recorded in `p06-terminal-result-contract-v1.json`,
`CHANGELOG.md` and `REGRESSION-MATRIX.md`; its gate proves structured terminal
results, compact/detailed logging, renderer-receipt persistence and exact visual
preservation. Version 1.2.21 additionally authenticates harmless filesystem
metadata drift through a stable content seal and exposes a fail-closed
`reuse-only` update path that cannot open the setup UI or extract data.

The M07 ledger maps every historical requirement to an implementation token and
a regression/gate token. `tests/test_m07_audit.py` verifies the complete ordered
set, source paths, exact 1.2.7 version, 59 synthetic cases and the current
low-glibc UI hash. Framework packaging treats that UI as mandatory whenever
NXExtract is active. APK-container recipes identify compatibility by package
and critical extracted content (or multiple explicit container identities),
never by one external APK SHA/size. The packaged runner also requires an SDL or
an approved graphical readiness proof before extraction. Historical diagnostic
TTY evidence remains separate and cannot authorize any current public run.

## APK-COMPAT-01 — fixture legal externa (1.3.0)

`tests/test_legal_fixture_e2e.py` roda o **pipeline gerado de verdade** contra
uma cópia legal fornecida pelo dono e prova a propriedade que o contrato
realmente afirma: **a identidade do container nunca decide compatibilidade**.

A mesma cópia é apresentada com outro nome, reempacotada com ordem de membros,
método de compressão, timestamps e extra fields diferentes, e em outro formato
de container suportado. Todas as variantes têm de resolver **exatamente o mesmo
conjunto de payloads**. Depois vêm os negativos: package de outro jogo, payload
obrigatório ausente e ABI que a receita não declara.

```sh
NXEXTRACT_LEGAL_APK=/caminho/para/a/copia-legal.apk \
NXEXTRACT_LEGAL_RECIPE=/caminho/para/extractor.json \
NXEXTRACT_LEGAL_RECEIPT=recibo.json \
  python3 -B framework/nxextract/tests/test_legal_fixture_e2e.py
```

Nada é descoberto: os dois caminhos são obrigatórios. Sem eles o gate sai `77`
declarando explicitamente que aquilo é **limite de claim, não aprovação**.

O artefato proprietário nunca entra no repositório. Os recibos em `evidence/`
guardam somente identidade técnica — receita, ABI, contagem de payloads,
tamanho e SHA-256 da cópia de referência — nunca o nome do arquivo original, o
caminho de origem ou a procedência.
