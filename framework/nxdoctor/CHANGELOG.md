## 0.3.0 — 2026-08-30

- Adiciona o coletor e validador stdlib-only
  `nx-doctor-recovery-evidence-v1` para as quatro ações explícitas. O formato
  fixa hashes de fonte/payload, recibo/status real e snapshots canônicos de
  state, gerações, staging, lock, dados do dono e superfície protegida.
- Valida transições exatas, recusas sem mutação e desanexação atômica com
  quarentena em falha tardia; GC interrompido usa claim autenticada
  (conteúdo/hash/nlink), PID morto/reciclado, integridade, run id e privacidade. Evidência
  duplicada, truncada, stale, hostil ou divergente falha fechado.
- Todo receipt ecoa o alvo pedido. GC recusa state ausente/inválido e
  clear-lock recusa owner ausente/inválido antes de qualquer mutação, evitando
  uma rejeição tardia do kit depois de a ação já ter ocorrido.
- Isola `HOST_FIXTURE` de `PHYSICAL`: sem âncora externa, esta versão recusa a
  criação/captura/validação física como `PENDING`, em vez de autenticar um
  arquivo e SHA autocriados pelo mesmo chamador.
- Todas as mutações usam exclusão mútua nos namespaces flock/fallback; restore
  compara state antes do replace, GC revalida state e protege também o
  `failure_generation`. Pais symlink e erros EACCES/EIO/ESTALE falham fechados.
- Escrita de state e claims usa write-all, tamanho conferido, fsync, no-follow
  e criação exclusiva; owner de lock aceita estritamente `PID starttime` e o
  formato real `pid=PID token=RUN_ID` (token só em hash). PID token-only vivo
  nunca é classificado como reciclado.
- Mantém a CLI e o JSON de diagnóstico existentes. Corrige a documentação para
  state/generation v1+v2, ids 32+64, literal `"null"`, `files/`, chmodless e
  seed visível.

# 0.2.0 (2026-08-29, o doutor volta a enxergar um port real)

O NXDoctor estava preso ao formato **v1** e a ids de geração de **32 hex**,
enquanto o nxbootstrap escreve `nxruntime-state-v2` desde a 0.6.35,
`nxruntime-generation-v2` desde a 0.6.36 e o **SHA-256 completo (64 hex)** como
id. Em qualquer port V3 ou V4 de verdade — exatamente os ports que ele existe
para diagnosticar — o resultado era:

- `state.json` lido como *unknown schema*, então nenhum estado;
- todo manifesto de geração lido como corrompido;
- `--complete-gc` recusando **todo** id real com "not 32 lowercase hex", o que
  deixava a ação morta e a metade de GC do débito de STORAGE sem executor.

Correções:

- aceita os pares de schema v1 **e** v2, para estado e para geração;
- aceita id de 64 hex (o de 32 continua aceito como entrada legada);
- lê os `components` de uma geração-v2, que são uma **lista ordenada** de
  registros por papel, e os normaliza para o formato path→entrada usado na
  verificação; os bytes são procurados sob `files/`, que é onde eles realmente
  ficam;
- normaliza a **string literal `"null"`** que o launcher grava para um slot
  vazio. O `state.json` real traz `"pending":"null"`, e tratar isso como valor
  inválido condenava o estado de todo port em campo.

**Na midia do aparelho, o doutor chamava de corrompida TODA geracao saudavel.**
Medido no aparelho autorizado (handheld ArkOS, AArch64): `/roms` e' exFAT montado com `fmask=0000`, entao
todo arquivo regular le' de volta `0777` e `chmod` e' no-op. O doutor comparava
o modo gravado no manifesto com o modo real e reportava "component mode
mismatch" em CADA membro — num port que o launcher acabara de rodar com
sucesso (`phase: runtime EXIT status=ok child_status=0`). Pior: `--complete-gc`
teria entao coletado uma geracao saudavel nao referenciada como "corrupt".

O launcher sempre soube disso: ele PROVA a propriedade com uma sonda de escrita
e entao trata o build como autoridade do modo, porque "integridade de runtime
vem de regular/sem-symlink/HASH". O doutor e' somente-leitura e nao pode rodar
essa sonda, entao reporta a coisa mais fraca e honesta: um modo que nao e'
nenhum dos dois valores POSIX fixados vira OBSERVACAO, nao corrupcao. O que
continua mordendo e' a regra de executavel do launcher (membro fixado 0755 que
nao e' executavel na visao montada), o modo trocado num filesystem que
CLARAMENTE aplica modos, e sobretudo o HASH — que e' o que uma adulteracao
realmente quebra.

Provado nos dois lados: no aparelho as duas geracoes passaram de `corrupt` para
`complete`, 13/13 membros verificados por hash, com a propriedade declarada em
13 notas; no host, regressao com modos 0777 sinteticos mais adulteracao de byte
que continua sendo pega. Verificado por mutacao: voltar a comparacao estrita
reprova; excusar tudo reprova; largar a regra de executavel reprova.

O doutor passa a **dizer ate' onde o port chegou**. O launcher publica
`nxphase-result.json` atomicamente em cada fronteira de fase, e esse e' o fato
mais util que existe para um port que nao abre — e o relatorio listava o
TAMANHO do arquivo e mais nada. Agora a ultima fronteira publicada e' lida,
com `read` (o resultado desta leitura) separado de `status` (a palavra do
launcher sobre a fronteira): colapsar os dois tornaria um arquivo ilegivel
indistinguivel de uma fase que falhou. Conteudo ilegivel, de schema estrangeiro
ou de tipo errado e' reportado como tal, nunca adivinhado, e nunca derruba o
relatorio.
O registro do teste e' montado a partir do `printf` do PROPRIO TEMPLATE do
launcher, entao mudanca de formato do lado que escreve reprova aqui em vez de
passar despercebida — verificado por mutacao: trocar o schema no launcher
reprova o gate do nxdoctor.

O inventario de logs tambem estava incompleto: faltavam `events.prev.jsonl`,
`nxextract-detail.log` e a rotacao `.prev` dele. Listar so' uma parte subestima
o que esta' no cartao, que e' exatamente o que engana quem diagnostica um
cartao cheio.

Uma coleta que o filesystem nega **deixou de ser um traceback**. Cartão em
somente-leitura, entrada ocupada, diretório sem permissão de escrita para o
dono: isso é condição normal de campo, não defeito de programa. Agora sai um
recibo `refused` que **nomeia a entrada** que travou, e a coleta fica
retomável.

Retomável exige inverter a ordem da criação. A geração nasce com `commit`
escrito **por último**; a remoção então tem de tirar `commit` **primeiro**, ou
uma deleção negada no meio deixa uma árvore esvaziada ainda vestindo sua marca
de completude — exatamente a coisa que o launcher aceitaria rodar. Antes de
qualquer destruição a coleta grava sua reivindicação
(`.nxdoctor-gc-claim`), que sobrevive a todos os outros membros e é recolocada
se o `rmdir` da raiz for negado. Sem ela o passe seguinte encontraria um
diretório sem `commit` e sem reivindicação: lixo que nada mais aceitaria
coletar, porque geração incompleta nunca é recolhida.

Regressão com negação injetada em dois pontos distintos — fundo da árvore e
raiz da geração —, injetada em processo porque a suíte pode rodar
*root-mapped* e o root atravessa `chmod`. A ferramenta publicada não carrega
gancho de falha nenhum. Verificado por mutação: não tirar `commit` primeiro,
não recolocar a reivindicação, não retomar e não capturar o `OSError` reprovam
o gate, um a um.


A correção anterior parou num lugar só. `--complete-gc` ganhou o recibo de
negação e as **outras três ações que mexem no filesystem continuaram
morrendo**: `--discard-staging` (tanto no diretório quanto no arquivo solto),
`--clear-lock` e a escrita de estado de `--restore-previous`. São exatamente as
mesmas condições de campo — cartão em somente-leitura, entrada ocupada,
diretório sem escrita — e o alvo de `--clear-lock` costuma ser um jogador cujo
port se recusa a abrir. Agora as quatro passam pelo mesmo helper `denied()`,
que nomeia a entrada que travou; um rollback que não consegue persistir **diz
que não aconteceu** em vez de morrer no meio e deixar quem chamou adivinhando
se o estado se moveu. Verificado por mutação nos quatro pontos.

Varrido junto o texto de recusa de `--restore-previous`, que também ainda dizia
"32 lowercase hex".

O relatório de espaço passa a listar o **seed visível** (`nxruntime-<id>.nxb`),
que normalmente é o maior arquivo de um port V4. Omiti-lo enganava quem estava
diagnosticando um cartão cheio, e é justamente ele que diz se a cache local
ainda pode ser reconstruída. Um seed que não seja arquivo regular é nomeado
como tal, nunca dimensionado.

Regressão nova com um port no formato que o aparelho realmente escreve: estado
legível, gerações v2 completas, byte adulterado dentro de uma geração-v2 ainda
detectado como `corrupt` (a normalização não pode virar um no-op), `complete-gc`
coletando uma geração não referenciada e recusando a ativa e a anterior. As três
correções foram verificadas por mutação: reverter qualquer uma reprova o gate.

# Changelog — nxdoctor

## 0.1.0 — 2026-08-26

- Initial release (front V3-DOCTOR-01).
- Read-only report: state, per-generation verification
  (commit marker + strict manifest + sha256/mode of every component),
  space/staging/unreferenced generations, gamedata presence,
  `NEXTOSCONTROLLERS.gptk` / `NEXTOSSETTINGS.txt` syntax validation,
  fallback-lock owner liveness (starttime, field 22), log inventory.
- `NEXTOSSETTINGS.txt` validation mirrors the runtime parser
  `framework/nxcompat/src/nxcompat_settings.c` byte for byte (auditoria V3,
  ponto 8): key allowlist `{language, quality}`, value charset
  `[A-Za-z0-9._-]{1,32}`, `quality` ∈ `{auto,low,medium,high}`, no whitespace
  stripping — so the doctor can never report "ok" for a file the runtime
  rejects fail-closed.
- Sanitized JSON output `nx-doctor-report-v1` (relative paths only).
- Actions with explicit flags, idempotency, symlink refusal and JSON
  receipts: `--restore-previous`, `--discard-staging`, `--complete-gc`,
  `--clear-lock`.
- Enforced prohibitions: no network, owner-data guard before every unlink,
  configs never rewritten, no mtime decisions, no version-name ordering.
- Script gate `tests/test_nxdoctor.py` (readonly proof by whole-tree hash,
  inverted-mtime clock-independence proof, 24 guard refusals, interrupted
  atomic-write simulation).
