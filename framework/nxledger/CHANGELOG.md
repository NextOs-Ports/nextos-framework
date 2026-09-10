# 0.2.3 (2026-08-31, oneshot-result/2 real)

- **Corrige uma incompatibilidade real de schema, não só documentação.** O
  formato do resultado mudou em relação ao 0.2.1 (ganhou `base_id`,
  `profile`, `attempt_type`, `family`, `reservation_sha256` e o
  `result_id` recomputado), mas o `RESULT_SCHEMA_VERSION` do 0.2.2 tinha
  ficado em 1 enquanto README, CHANGELOG, docstring e kit final já
  declaravam `oneshot-result/2`. Agora `RESULT_SCHEMA_VERSION = 2` e o
  documento emitido é genuinamente `org.nextos.v4.oneshot-result/2`.
- **`oneshot-result/1` falha fechado.** Como o validador exige igualdade
  exata de `schema_version`, qualquer result que se apresente como `/1` —
  inclusive um result verdadeiro produzido pelo nxledger 0.2.1, que também
  não tem o conjunto de chaves selado — vira `tampered` e nunca autoriza um
  package (regressão dirigida provando os dois caminhos).
- **Nenhum outro comportamento muda.** Todas as contraprovas do 0.2.2
  permanecem: flip simples, flip+recompute, campo extra/ausente, bool-como-
  int, chave duplicada, NaN/Infinity/BOM/UTF-8/nesting, família cruzada,
  claims DEV recomputados e store hostil por mode/owner/symlink continuam
  fechando. `result/2` é produzido, relido, selado por `result_id` e aceito
  no fluxo `host-authority`/package.
- README "versão atual" corrigido (tinha ficado em 0.2.1), contrato
  declarativo e matriz atualizados; M11–M14 recarimbados só onde afetados.

# 0.2.2 (2026-08-31, autoridade do resultado e família física)

Fecha a classe inteira apontada pela auditoria 05B2 (`NÃO APTO`: result.json
aceitava `FAIL`→`PASS`) e pelas três variações da auditoria adversarial
paralela (claims fora do hash, proof sem família, README citando
`physical-authority/1`):

1. **Resultado selado** (`oneshot-result/2`, migração fail-closed): o
   documento carrega attempt/base ID, perfil, tipo, família, resultado,
   SHA-256 da evidência, âncora SHA-256 da reservation exata e `result_id`
   recomputado do JSON canônico integral (sem autoatestado circular); schema
   FECHADO — campo extra, ausente, trocado ou um único byte alterado
   (inclusive `FAIL`→`PASS`) vira `tampered`, nunca `finished`. A evidência
   (`oneshot-evidence/2`) nomeia o mesmo attempt E o mesmo resultado da CLI.
2. **Autoridade host explícita.** `package-candidate` PUBLIC-FINAL não
   procura mais "qualquer PASS" por base ID: exige `--host-authority`
   (`host-authority/1`, schema fechado, attempt ID + result ID), que entra
   na tupla do package e é validada como host-battery PASS PUBLIC-FINAL da
   MESMA base (manifesto, ELFs, toolchain, repo); o `finish` do package
   revalida autoridade host e provas físicas — adulteração antes ou depois
   da reserva bloqueia o consumo (exit 3), inclusive um flip com result_id
   recomputado (o pin da tupla deixa de bater).
3. **Claims derivados.** Reservation com schema FECHADO: `allowed_effects` é
   recomputado de perfil/tipo, DEV exige exatamente
   `physical_support_proven=false` e `public_candidate_authorized=false`;
   `false`→`true`, texto editado, lista esvaziada ou campo extra viram
   `tampered`; bool jamais conta como int de schema/version em nenhum
   documento.
4. **Prova física presa a UMA família.** `physical-proof` reserva com
   `--family` única e canônica (recusado nos outros tipos); a família entra
   no attempt ID e no resultado selado; o receipt sucessor
   (`physical-authority/3`, único schema físico vivo — /1 e /2 são recusados)
   nomeia family, commit/tree, base ID, proof attempt ID e proof result ID, e
   a família tem de ser exatamente a da reservation da prova: um proof de
   `mali450` jamais serve `muos-h700` e o mesmo proof nunca autoriza duas
   famílias (dois proofs corretos, um por família, autorizam).
5. **JSON estrito único e derivação sem híbrido.** Manifests, receipts,
   evidência, autoridade host, store e o próprio ledger passam pelo mesmo
   carregador (BOM, UTF-8 frouxo, chave duplicada, NaN/Infinity, nesting e
   tamanho excessivos falham fechado); manifesto e cada ELF são revalidados
   por identidade de inode/tamanho/mtime durante a medição e a identidade do
   repositório é relida ao final da derivação — fonte que mudou no meio
   recusa em vez de combinar gerações. Raiz de estado dentro de diretório
   Git ou repositório bare também é recusada; leitura de VERSION, walk do
   inventário e o subprocess da política `--out` viram IO-FAILURE/exit 4.
   **Limite honesto:** o selo não é assinatura criptográfica; o dono do
   store controla todos os bytes — o que ele garante é que adulteração
   posterior nunca fabrica PASS dentro do fluxo canônico, e a autoridade
   exata usada fica presa na tupla do package.
6. **Controle negativo versionado.** A bateria extrai o nxledger 0.2.1 do
   próprio Git e prova que ele aceitava o flip `FAIL`→`PASS` (store
   `finished`/PASS) e um proof servindo família alheia; o 0.2.2 responde
   `tampered` ao mesmo store e recusa o receipt — documento antigo nunca é
   interpretado como novo.

# 0.2.1 (2026-08-31, vínculo de artefato, raiz externa e store hostil)

Sete correções de vínculo e durabilidade sobre o 0.2.0, sem mudar a
interface de derivação:

1. **Mesmos artefatos entre host e pacote.** O base/artifact ID
   (`org.nextos.v4.oneshot-artifact/1`) passa a vincular o SHA-256 do
   manifesto de inputs, o mapa canônico de cada ELF medido (path lógico,
   tamanho, SHA-256, Build ID, classe/máquina, PT_INTERP, maior GLIBC), o
   toolchain declarado, commit/tree, as 18 autoridades, o contrato e o
   perfil. Trocar binário, manifesto ou toolchain cria outra identidade e
   nunca herda o host-battery PASS anterior.
2. **Raiz one-shot externa e privada de verdade.** Recusa raiz dentro de
   qualquer work tree Git; valida por lstat cada componente do caminho
   (diretório real, sem symlink) e exige owner+0700 na raiz, em
   `attempts/`, no diretório da tentativa e nos documentos; revalida
   imediatamente antes de cada operação sensível (troca tardia por symlink
   é recusada).
3. **Receipts entram na tupla.** O attempt ID inclui famílias físicas
   requeridas ordenadas (duplicata é erro, nunca última-vence) e o
   SHA-256+identidade do preflight e de cada physical receipt
   (`org.nextos.v4.physical-authority/2`), que agora vincula family,
   commit/tree, base_id do artefato e o attempt da prova física
   correspondente; receipt de outro artefato, perfil, família ou tentativa
   não autoriza pacote.
4. **Store hostil falha fechado.** Reservation/result são revalidados na
   releitura: schema/versão/perfil/tipo, attempt e base ID RECALCULADOS
   dos bytes, arquivo regular/owner/0600/teto de tamanho/UTF-8
   estrito/sem chave duplicada; um byte alterado vira estado `tampered`
   com finding explícito — nunca fabrica reserva nem PASS (um host PASS
   adulterado deixa de autorizar pacote).
5. **I/O controlado integralmente.** git/exec, lstat/listdir/mkdir/link/
   fsync (de arquivo e diretório), parser ELF truncado (struct/Value/
   Index) e falha de cleanup viram `IO-FAILURE`/exit 4 sem traceback;
   cleanup falho numa escrita publicada é erro, nunca sucesso.
6. **Durabilidade e corrida.** fsync no arquivo e no diretório-pai após
   criação/publicação; criação concorrente de `attempts/` é idempotente e
   duas reservas simultâneas dão exatamente [0, 3], nunca 4 por corrida
   esperada; crash em qualquer fronteira consome a tupla sem documento
   parcial aceito.
7. **Reseal verdadeiro.** M11/M12/M13/M14 recarimbados no mesmo commit
   contra o contrato, matriz, run-safe-gates e test_infrastructure reais
   (o 0.2.0 não havia recarimbado); `nx-reseal --check` zero stale no
   commit final.

# 0.2.0 (2026-08-31, autoridades completas, one-shot e perfis DEV/PUBLIC)

- **Parte A — identidade completa.** A allowlist passa a cobrir TODA
  autoridade `VERSION` sob `framework/` — inclusive `portmaster_contract`
  (`framework/portmaster/VERSION`, o 2.1.1 da autoridade histórica),
  `apkcompat` (`framework/contracts/apkcompat/VERSION`) e `framework_tests`
  (`framework/tests/VERSION`) — mais o externo `nxextract`. O inventário é
  determinístico por varredura completa: um domínio versionado desconhecido
  falha fechado até entrar na allowlist; a contagem esperada deriva da
  allowlist e é testada. Schema do ledger avança para 2.
- `--out` agora escreve somente onde o documento não invalida a si mesmo:
  fora do repositório, ou dentro dele num path comprovadamente git-ignored;
  o documento gerado passa `--check` no mesmo estado da árvore.
- Falhas de I/O viram erro controlado com exit 4 próprio (`IO-FAILURE`),
  nunca traceback e nunca o exit 1 reservado à divergência. Exit codes:
  0 sucesso/match/elegível · 1 divergência do `--check` · 2 erro de
  contrato/uso/derivação · 3 decisão one-shot bloqueada · 4 falha de I/O.
- Zero `ResourceWarning`: o runner promove ResourceWarning a erro e as
  fixtures fecham todos os descritores.
- **Parte B — one-shot exato (V4-PRE-07).** `oneshot eligible|reserve|finish`
  sobre um estado externo privado e append-only: a tupla vincula schema,
  perfil, commit/tree/branch/detached/dirty=false, autoridades completas,
  contrato declarativo (com SHA-256), tags, SHA-256 do manifesto de inputs e
  cada ELF declarado (path lógico, tamanho, SHA-256, Build ID, classe/máquina,
  PT_INTERP, maior GLIBC), toolchain declarado e tipo de tentativa. O ID é
  SHA-256 do JSON canônico; reserva é exclusiva e race-free (mkdir atômico);
  reserva/resultado são documentos criados exclusivamente, com fsync de
  arquivo e diretório; nada é sobrescrito, apagado ou reciclado; crash deixa
  evidência inconclusiva que consome a tupla; não existe force/reset/retry.
- **Parte C — perfis (V4-PRE-06).** Perfil explícito obrigatório
  (`DEV/device-matrix` ou `PUBLIC-FINAL`), sem default; a CLI imprime perfil,
  tipo, ID, efeitos permitidos e motivos de bloqueio antes de reservar, com
  `--json` estável. DEV registra claims físicos não comprovados como falsos e
  jamais autoriza `package-candidate` público. PUBLIC-FINAL package-candidate
  exige host-battery PASS da mesma identidade base, receipt de preflight
  exato (interface tipada `org.nextos.v4.preflight-receipt/1`; o nxrelease
  ainda não está ligado) e autoridade física PASS por família explicitamente
  requerida (`org.nextos.v4.physical-authority/1`); ausência, SKIP, arquivo
  ilegível ou receipt malformado bloqueiam, nunca aprovam.
- Controle negativo registrado: o 0.1.1 não detectava bump isolado de
  `framework/portmaster/VERSION`; o 0.2.0 detecta (teste dirigido).

# 0.1.1 (2026-08-31, registro na matriz oficial de infraestrutura)

- Os casos dirigidos passam a chamar-se `tests/test_nxledger.py` e entram na
  matriz oficial (`framework/tests/test-matrix-v1.json`, gate
  `nxledger-host`, classe filesystem, automático), cumprindo o gate de
  infraestrutura que exige toda fonte `test*` classificada.
- Nenhuma mudança de comportamento da ferramenta: interface, JSON canônico,
  `--check`, `--out` e todos os fail-closed do 0.1.0 permanecem literais.
  A 0.1.0 não é reapresentada com bytes diferentes; esta é a versão aditiva
  que carrega o rename e o registro.

# 0.1.0 (2026-08-31, ledger canônico derivado, host-only)

- Nasce o `nxledger`: a identidade da composição V4 passa a ser DERIVADA do
  Git e dos arquivos `VERSION`, nunca digitada. Motivo: o ledger manual
  `FRAMEWORK-V4-CURRENT.json` ficou apontando para um HEAD antigo enquanto os
  sucessores reais viviam em outras branches (estado verificado em
  31/08/2026).
- Read-only por padrão: somente `rev-parse`, `symbolic-ref`,
  `status --porcelain` e `tag --points-at`, todos locais. Zero rede, zero
  mutação de Git, zero criação de tag, zero edição do ledger conferido.
- Allowlist explícita de componentes canônicos; componente ausente, `VERSION`
  inválido, symlink ou `framework/nx*/VERSION` fora da lista falham fechados.
  A identidade do contrato declarativo é lida de
  `framework/contracts/declarative-v1.json`; campo ausente nunca é inventado.
- JSON canônico e determinístico em stdout; `generated_epoch` só existe com
  `SOURCE_DATE_EPOCH` explícito.
- `--check <ledger.json>` falha (exit 1) listando toda divergência de commit,
  tree, branch/detached, sujeira, componentes, contrato e tags no HEAD.
- `--out <path>` grava os mesmos bytes canônicos apenas em arquivo NOVO,
  dentro da raiz do repositório, por escrita atômica (temp + hard link);
  escrita interrompida não deixa arquivo parcial nem temporário.
- O ledger nunca registra tag/release, prova física, promoção, baseline ou
  suporte: isso é decisão, não derivação. Esta versão não integra a V4, não
  atualiza o ledger oficial e não entra na matriz de testes; o registro na
  matriz acontece na futura missão de integração.
