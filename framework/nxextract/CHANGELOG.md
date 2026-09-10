# 1.3.0 (2026-08-29, APK-COMPAT-01 com fixture legal real e cerca de recursos fail-closed)
- **Os orçamentos de log viraram limite de verdade.** `nx-budget-check`
  declarava 2 MiB para o log compacto e 8 MiB (+1 rotação) para o de detalhe, e
  **nada aplicava nenhum dos dois**. O NXExtract roda na hora da instalação, no
  mesmo cartão em que os payloads vão cair: um log sem teto disputa espaço com
  a própria extração que ele está narrando. Agora o log compacto — que é a
  narrativa de abertura (identidade da receita, descoberta, plano) — para no
  teto **dizendo na última linha que parou**, porque um corte silencioso é
  indistinguível de uma execução que morreu; e o log de detalhe, que é o fluxo
  sem perdas, **rotaciona uma vez**, então a janela mais recente sobrevive
  inteira e exatamente uma geração anterior é guardada. Uma rotação que o
  filesystem recuse não derruba o fluxo: continua escrevendo onde já estava.
  Os tetos estão fixados no teste contra os números publicados, e verificados
  por mutação: não limitar o compacto, não rotacionar o detalhe e limitar sem
  avisar reprovam o gate, um a um.


- **V3-HARDENING-01 fechado.** A cerca de recursos dos hooks de receita deixa
  de engolir erro: um `RLIMIT_CPU`/`RLIMIT_AS`/`RLIMIT_FSIZE`/`RLIMIT_NPROC`
  que não pode ser estabelecido agora **recusa o hook**. Antes, qualquer falha
  virava `pass` silencioso e o hook rodava **sem teto nenhum** — exatamente o
  estado que esses limites existem para impedir. O erro sobe pelo `preexec` e o
  processo nunca chega a existir sem cerca.
  - Regressão nova com **controle positivo**: a mesma receita roda o hook de
    verdade sem a falha injetada e não roda com ela, então o negativo não pode
    passar de forma vazia.
- **APK-COMPAT-01: a lacuna de fixture legal externa está fechada.** O novo
  gate `framework/nxextract/tests/test_legal_fixture_e2e.py` roda o **pipeline
  gerado de verdade** (não funções isoladas) contra uma cópia legal do dono e
  prova a propriedade metamórfica que o contrato realmente afirma: **a
  identidade do container nunca decide compatibilidade**.
  - Positivos: como fornecido, **renomeado**, **reempacotado** (ordem invertida,
    `stored` em vez de `deflate`, timestamps futuros, atributos DOS e extra
    fields estranhos) e em **outro formato de container suportado**.
    Todos têm de resolver **exatamente o mesmo conjunto de payloads**.
  - Negativos: package de outro jogo, payload obrigatório ausente (com a recusa
    nomeando a propriedade técnica) e ABI que a receita não declara.
  - O artefato proprietário **nunca entra no repositório** e nada é descoberto:
    os dois caminhos são fornecidos explicitamente e, sem eles, o gate sai 77
    dizendo que aquilo é **limite de claim, não aprovação**.
  - Os recibos guardados em `framework/nxextract/evidence/` trazem só
    identidade técnica — receita, ABI, contagem de payloads, tamanho e SHA-256
    da cópia de referência — nunca o nome do arquivo original, o caminho de
    origem ou a procedência.
  - Executado de verdade em duas cópias legais reais, uma `.apk` (17 payloads)
    e uma `.apkm` (525 payloads), ambas com 4 positivos e 3 negativos.
- O motor sobe para `1.3.0` e a identidade de saída `1.2.21` fica registrada em
  `framework/nxrelease/nxextract-engines-v1.json` no momento do bump, como o
  próprio arquivo exige. Runner e runtime-env permanecem byte-idênticos. Nenhum
  port publicado é migrado: ele continua válido no motor que embarca.

# Changelog

## 1.2.21

- Adds a stable content seal behind the ordinary metadata fast path, allowing
  byte-identical payloads on FAT/exFAT/FUSE to be resealed without extraction.
- Migrates only authenticated 1.2.20 markers; metadata-drifted legacy markers
  require an independent expected content seal, same-size byte corruption is
  rejected and older markers remain fail-closed.
- Adds `reuse-only`, which either validates/migrates existing data or aborts
  before transaction recovery, UI startup, source discovery and extraction.
- Keeps the NXExtract UI source, pixels and immutable low-GLIBC artifacts
  unchanged.

## 1.2.13

- Separates terminal renderer evidence from live UI process state, so normal
  private-runtime cleanup cannot rewrite a proven `sdl`/`fbdev` run as
  `ui.mode=disabled`.
- Adds focused SDL and fbdev terminal-receipt regressions while preserving the
  immutable 1.2.9 UI artifacts and pixel goldens.
- Leaves generated ports pinned to NXExtract 1.2.12 until an explicit
  nxbootstrap/nxgenerator/nxrelease opt-in promotion.

## 1.2.10

- Adds atomic terminal-result schema 1 with stable `NXE####` codes, final phase,
  sanitized container identity, package/ABI and validated payload totals.
- Separates compact milestone logging from lossless per-file/hook detail and
  provides explicit verbose opt-in.
- Preserves the NXExtract 1.2.9 UI release as an independently pinned immutable
  presentation artifact; renderer source, four low-GLIBC ELFs and both pixel
  goldens are unchanged.
- Advances the declarative component lock to 1.0.31. Existing generated ports
  remain on their exact NXExtract 1.2.9 pin until an explicit nxbootstrap,
  nxgenerator and nxrelease promotion.
