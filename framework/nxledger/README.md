# nxledger

Versão atual: **0.2.3**.

Ferramenta host-only que deriva a identidade REAL da composição do framework
diretamente do Git e dos arquivos `VERSION`. Ela existe porque um ledger
mantido à mão diverge: em 31/08/2026 o `FRAMEWORK-V4-CURRENT.json` ainda
apontava para um HEAD antigo enquanto os sucessores reais viviam em outras
branches. Identidade se deriva; não se digita.

## O que ela lê (e nada além)

- Git local, somente leitura: `rev-parse HEAD`, `rev-parse HEAD^{tree}`,
  `symbolic-ref --short -q HEAD` (branch ou detached),
  `status --porcelain` (sujeira) e `tag --points-at HEAD`.
- Uma allowlist explícita de TODAS as autoridades `VERSION` canônicas sob
  `framework/`: os componentes `nx*`, `portmaster_contract`
  (`framework/portmaster/VERSION`), `apkcompat`
  (`framework/contracts/apkcompat/VERSION`) e `framework_tests`
  (`framework/tests/VERSION`) — mais a autoridade externa aceita
  explicitamente `nxextract`
  (`suportando_outros_devices/extrator-universal/VERSION`). O inventário é
  determinístico: qualquer `VERSION` sob `framework/` fora da allowlist
  falha fechado até ser revisado para dentro dela; a contagem esperada
  deriva da allowlist.
- A identidade do contrato declarativo
  (`framework/contracts/declarative-v1.json`): `schema_version` e
  `contract_version`.

Falha fechada, sem inventar dado ausente: componente da allowlist sem
`VERSION`, conteúdo que não é versão, `VERSION` symlink, um
`framework/nx*/VERSION` fora da allowlist, contrato ausente ou sem os campos
de identidade — tudo é erro (exit 2), nunca um campo adivinhado.

O ledger **nunca** registra tag/release, prova física, promoção, baseline ou
suporte. Essas são decisões humanas com autoridade própria; a ferramenta só
descreve o que o Git e os arquivos provam.

## Uso

```
nxledger.py [--repo PATH]                # deriva e imprime o JSON canônico
nxledger.py --check LEDGER.JSON          # confere um ledger gravado
nxledger.py --out PATH                   # também grava em arquivo NOVO
nxledger.py oneshot eligible|reserve ... # identidade one-shot (leitura/reserva)
nxledger.py oneshot finish ...           # acrescenta o resultado, append-only
```

- stdout é JSON canônico e determinístico (chaves ordenadas, separadores
  fixos, uma quebra final). Não há data/hora variável: `generated_epoch`
  só aparece quando o chamador exporta `SOURCE_DATE_EPOCH`.
- `--check` re-deriva e compara commit, tree, branch/detached, sujeira,
  componentes, contrato e tags no HEAD; qualquer divergência sai listada em
  stderr e o exit é 1. Igualdade imprime `nxledger: MATCH <commit>`.
- `--out` recusa arquivo existente (symlink incluído) e diretório-pai
  symlink, e só escreve onde o documento não invalida a si mesmo: **fora do
  repositório**, ou dentro dele num path que o Git comprovadamente ignora
  (`git check-ignore`) — assim o documento gerado passa `--check` no mesmo
  estado da árvore. A escrita é atômica via arquivo temporário + hard link;
  uma escrita interrompida não deixa arquivo parcial nem temporário.

Exit codes (não ambíguos): `0` sucesso/match/elegível · `1` divergência no
`--check` · `2` erro de contrato/uso/derivação · `3` decisão one-shot
bloqueada · `4` falha de I/O controlada (`IO-FAILURE`). Nunca traceback.

## One-shot e perfis (V4-PRE-07 / V4-PRE-06)

Desde a 0.2.1 o base/artifact ID vincula também o manifesto de inputs, cada
ELF medido e o toolchain — um host-battery PASS só autoriza um
package-candidate de bytes idênticos — e o attempt ID inclui as famílias
físicas requeridas e o SHA-256+identidade de cada receipt; a raiz de estado
precisa ser externa a qualquer árvore Git (e, desde a 0.2.2, também a
qualquer diretório Git ou repositório bare), com todos os componentes reais
e sem symlink, e os documentos do store são revalidados por recomputação de
IDs na releitura.

Desde a 0.2.2 o RESULTADO também é selado
(`org.nextos.v4.oneshot-result/2`): o documento inclui attempt/base ID,
perfil, tipo, família, resultado, SHA-256 da evidência, âncora SHA-256 da
reservation exata e um `result_id` recomputado do JSON canônico; qualquer
byte ou campo alterado — inclusive `FAIL`→`PASS`, claims decorativos e
`allowed_effects` — vira `tampered` na releitura, porque a reservation tem
schema FECHADO e os claims são recomputados da política
(`physical_support_proven`/`public_candidate_authorized` DEV são exatamente
`false`; bool jamais conta como int de schema/version). **Limite honesto de
confiança:** o selo detecta inconsistência e adulteração do store dentro do
fluxo canônico; ele NÃO é assinatura criptográfica — o mesmo usuário que
controla todos os bytes do store não vira autoridade. A evidência externa e
o receipt exato continuam obrigatórios.

A tentativa é identificada por uma tupla derivada — nunca por nome escolhido
pelo chamador: schema da política, perfil explícito, commit/tree/branch/
detached e `dirty=false`, mapa completo das autoridades, identidade+SHA-256
do contrato declarativo, tags no HEAD, SHA-256 do manifesto de inputs
(`org.nextos.v4.oneshot-inputs/1`, arquivo regular sem symlink) e, para cada
ELF declarado, caminho lógico, tamanho, SHA-256, Build ID, classe/máquina,
PT_INTERP e maior GLIBC medidos no próprio ELF (declaração divergente falha
antes de reservar), mais o toolchain declarado e o tipo de tentativa
(`host-battery`, `package-candidate`, `physical-proof`). O ID é o SHA-256 do
JSON canônico.

O estado vive fora da árvore Git, numa raiz absoluta, do usuário, privada
(0700) e sem symlink em nenhum componente. `reserve` é exclusivo e race-free
(mkdir atômico): uma tupla reservada, aprovada, reprovada ou interrompida
está consumida para sempre — não existe `--force`, `--reset`, `--retry` nem
limpeza. `eligible` é somente leitura. `finish` apenas acrescenta `PASS`,
`FAIL` ou `INCONCLUSIVE`, preso ao ID e a um manifesto de evidência
(`org.nextos.v4.oneshot-evidence/2`) que nomeia o mesmo attempt e o MESMO
resultado da linha de comando (evidência e opção precisam concordar).

Perfis canônicos: `DEV/device-matrix` e `PUBLIC-FINAL`; o perfil é sempre
explícito (pedido textual "universal", nome de ZIP ou metadata de port nunca
escolhem perfil). O receipt DEV registra `physical_support_proven=false` e
`public_candidate_authorized=false`, e DEV jamais autoriza
`package-candidate` público. `package-candidate` PUBLIC-FINAL exige uma
autoridade host EXPLÍCITA (`--host-authority` +
`org.nextos.v4.host-authority/1`, schema fechado com attempt ID e result ID
do host-battery PASS exato — nunca "qualquer PASS" procurado por base ID),
receipt de preflight exato (`org.nextos.v4.preflight-receipt/1`, PASS,
ligado ao commit/tree) e autoridade física PASS por família explicitamente
requerida (`--require-physical` + `org.nextos.v4.physical-authority/3`, que
nomeia family, commit/tree, base ID, o attempt da prova física E o result ID
selado da prova); a família do receipt tem de ser exatamente a da
reservation da prova (`physical-proof` reserva com `--family` única e
canônica; um proof de `mali450` jamais serve `muos-h700`, e o mesmo proof ID
nunca autoriza duas famílias). A autoridade host e as provas físicas são
revalidadas de novo no `finish` do package — adulteração antes ou depois da
reserva bloqueia o consumo. Ausência, SKIP, arquivo ilegível ou receipt
malformado bloqueiam a decisão inteira. A
CLI imprime perfil, tipo, ID, efeitos permitidos e motivos de bloqueio antes
de reservar (`--json` para saída estável). Trocar de perfil gera outra tupla
e nunca converte evidência DEV em pública.

O nxrelease ainda não consome estas interfaces; a ligação (gerar o receipt
de preflight agregado e exigir `reserve` antes da bateria/candidato) é da
composição posterior.

## Testes dirigidos

`tests/run-nxledger-host.sh` executa `tests/test_nxledger.py` (unittest)
sobre repositórios Git temporários: HEAD limpo em branch e detached, árvore
suja, commit/tree/versão/contrato/tag stale no `--check`, componente ausente,
`VERSION` inválido, symlink, componente extra, determinismo em duas
execuções, e as recusas do `--out` (existente, symlink, fora da raiz) mais a
escrita interrompida sem arquivo parcial.

Desde a 0.1.1 os casos estão registrados na matriz oficial
(`framework/tests/test-matrix-v1.json`, gate `nxledger-host`); a bateria
roda também de forma avulsa pelo runner acima.
