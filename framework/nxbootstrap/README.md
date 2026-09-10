# nxbootstrap

`nxbootstrap` é a camada comum anterior ao `main` dos ports PortMaster. Ela gera um
wrapper visível curto, valida a ABI e os arquivos, prepara o ambiente do host, executa
NXExtract isoladamente, entrega a tela obrigatória do framework e supervisiona uma
única instância do loader.

O código de jogo, JNI, EGL, áudio e controle não pertence ao shell. Depois do
preflight, o comportamento multi-device fica no loader ligado a `nxcompat` e
`nxinput`.

Versão atual: `0.8.4`. `schema_version: 3` é o opt-in canônico para
`generation_runtime`; schemas 1/2 continuam legíveis sem promoção automática.

## SDL de sistema herdada e obsoleta na 0.8.4

Alguns ambientes ArkOS/PortMaster deixam no launcher um `LD_PRELOAD` SDL com
versão que já não existe no firmware. O caso físico que motivou esta revisão
herdava `/usr/$LIB/libSDL2-2.0.so.0.3000.10`, mas o aparelho oferecia somente
SDL `0.3200.x`; a própria `ld.so` ignorava a entrada antes do framework.

Com `sdl_provider: "system"`, o launcher remove essa entrada do ambiente
efetivo do jogo somente quando ela é um basename SDL1/2 diretamente sob
`/usr/$LIB`, `/usr/${LIB}`, `/lib/$LIB` ou `/lib/${LIB}`, não resolveu para um
objeto carregado, veio do ambiente herdado e o adapter não acrescentou outro
preload. O descarte é registrado nominalmente e a SDL atual continua sendo
encontrada pelo search path normal do firmware. Um token válido já mapeado
continua preservado literalmente.

Essa exceção não aceita um arquivo: ela remove uma solicitação comprovadamente
ineficaz e estreitamente classificada. Caminhos privados, basenames não SDL,
subdiretórios extras, valores criados pelo adapter e demais entradas não
resolvidas continuam falhando fechado.

## Biblioteca privada do NXExtract na 0.7.8

Um DSO necessário somente por um helper do NXExtract é declarado com o papel
existente `private-library`, modo `0644` e path estritamente abaixo de uma raiz
de `private_library_paths`. Ele pode ficar em `nxextract/` apenas quando o
NXExtract está ativo; não vira `nxextract-helper`, não ganha execução e não
pode ocupar `nxextract.py`, `run-extractor.sh`,
`nxextract-runtime-env.sh` ou `nxextract-ui`.

O membro participa da mesma geração autenticada, store imutável, heal e
allowlist de árvore dos demais componentes. NXExtract desligado, owner data,
path fora/igual à raiz e modo diferente de `0644` falham fechado. Bibliotecas
privadas fora da árvore do NXExtract preservam o contrato anterior.

## Saída do jogo também governa a promoção na 0.7.7

Um receipt `ready`, mesmo acompanhado de `VIDEO OK/non-black`, só promove a
geração se o launcher tiver persistido a fronteira `runtime EXIT` e o filho
exato tiver retornado status 0. Saída não zero continua sendo falha real,
incrementa `prehealth_failures`, preserva a pendência e nunca emite `NXU0006`.
O adapter não pode absolver um crash publicando receipts antes de sair.

## Primeiro boot sem autobloqueio na 0.7.6

Uma instalação fresca não possui geração anterior para rollback. Se o adapter
não publicar o receipt de saúde, uma saída limpa e run-bound (`status=0`) agora
promove essa primeira geração; quando vídeo é obrigatório, o receipt final
`OK/non-black` continua exigido. Atualizações A/B com uma geração saudável
anterior continuam exigindo o receipt exato do adapter.

Se um estado antigo já chegou a três falhas com `active=null` e
`previous_healthy=null`, o launcher tenta novamente a única geração completa em
vez de recusar para sempre com `NXU0009`. Nenhuma âncora é fabricada. Arquivos do
dono e arquivos extras fora da closure autenticada permanecem intocados.

## Boot fast path na 0.7.5

O boot saudável é O(1): com estado/commit/identidade coerentes valida-se só o
plano de controle e nada proporcional ao número de arquivos roda antes do
preflight. O probe de mídia chmodless acontece no máximo uma vez por
execução. O caminho profundo (recuperação) verifica a closure com sha256sum
em lote, é anunciado (NXU0014, log e console) e continua fail-closed.

## Vídeo fatal e SDL do sistema na 0.7.4

O campo opcional fechado `sdl_provider: "system"` declara que SDL1/SDL2 são do
firmware. Nesse modo, os diretórios do firmware precedem os overlays do
PortMaster e entradas herdadas de `LD_LIBRARY_PATH` cujo caminho canônico está
dentro do port são descartadas. O launcher registra o provider, a ordem e o
search path efetivos, mas nunca define `SDL_VIDEODRIVER` nem escolhe um backend.
Sem o campo, a ordem e o `LD_LIBRARY_PATH` herdado preservam o comportamento
legado. Qualquer outro valor é recusado pelo gerador.

Nesse opt-in, `BIN_PRELOAD`, `LD_PRELOAD` e `SDL_DYNAMIC_API` herdados são
capturados e removidos com builtins no primeiro ponto alcançável do launcher,
antes do primeiro subprocess/helper executado. Isso não promete desfazer o que
o dynamic loader já aplicou ao iniciar o próprio Bash; garante que os
subprocessos posteriores do framework não herdem esses valores. Comandos que o
hook executa enquanto está sendo sourced pertencem ao hook e precedem
necessariamente a fronteira de retorno. O hook conserva sua semântica top-level;
os três valores finais são copiados imediatamente depois do source, recebem
`export -n` e são removidos. Como o de-export funciona também sobre readonly, um
`readonly` do adapter é recusado já com o ambiente externo limpo e a finalização
normal preservada. Se um valor herdado já vier readonly e impedir o primeiro
`unset`, o launcher sai por uma
rota somente de builtins, deixa o `launcher-error.<pid>.log` 0600 obrigatório e
não executa helper contaminado.

A barreira resolve toda entrada de `BIN_PRELOAD`, `LD_PRELOAD` e
`SDL_DYNAMIC_API` contra o search path final; uma entrada que não resolve falha
fechado. Um alvo canônico dentro do port é recusado quando seu nome é SDL1/2 ou
quando um rename ainda contém fingerprints conservadores do core ou dos add-ons
image, mixer, ttf, net, gfx, gpu, sound, rtf e fontcache. Essa proibição é
package-private: depois da resolução canônica, um alvo fora do
`NXBOOTSTRAP_PHYSICAL_GAMEDIR` permanece autoridade externa/firmware, inclusive
`SDL_DYNAMIC_API` e um arquivo com nome/fingerprint SDL. Overrides externos
resolvidos só são exportados dentro do subshell que faz `exec` do jogo.
`$ORIGIN` é resolvido relativamente ao diretório físico do
executável, como no dynamic loader, e não relativamente à raiz do port.
Em `LD_PRELOAD`, `$LIB` e `${LIB}` também preservam a semântica do loader, mas
somente quando o próprio processo do launcher comprova em `/proc/$$/maps` uma
única expansão já carregada. Isso cobre o provider herdado pelo ArkOS sem
executar texto, sem adivinhar a arquitetura e sem transformar um token criado
pelo adapter em caminho confiável. O literal original chega ao jogo; expansão
ambígua, package-private ou usada em outra variável continua recusada. A única
expansão ausente removível é a entrada SDL1/2 de sistema herdada e obsoleta
descrita acima; todas as demais continuam recusadas.

O mesmo modo congela o `BIN` do manifesto e recusa substituição pelo hook do
interpreter, game loader ou library route. Essa é uma barreira de execução
deliberadamente limitada: a auditoria profunda package-wide de ELF, SONAME,
símbolos completos, dependências e proveniência dos bytes do pacote continua
obrigatória no NXRelease. Sem `sdl_provider`, a quarentena e a rota imutável não são geradas;
o comportamento legado do hook e do ambiente é preservado.

Independentemente desse campo, o launcher recusa antes do `exec` qualquer
nome `libSDL*` do namespace SDL1/SDL2 nos roots visíveis de bibliotecas do
jogo, incluindo add-ons `image`, `mixer`, `ttf`, `net`, `gfx`, `gpu`, `sound`,
`rtf` e `FontCache`. Bibliotecas SDL3 não são classificadas como SDL1/2; um ZIP
que as inclua continua dependendo da exceção formal SDL3/Godot, com hash,
validada pelo NXRelease.

Todo launcher exporta `NXBOOTSTRAP_VIDEO_FILE` num runtime privado e único por
execução. O produtor publica por rename atômica exatamente uma linha no schema
`org.nextos.nxruntime.video-proof`, versão 1, presa ao mesmo `run_id`, geração e
port do health receipt. Os pares aceitos são exclusivamente:

- `OK` / `non-black`;
- `BLACK` / `black-streak`;
- `BLACK` / `all-black`;
- `DEAD-CONTEXT` / `dead-context`.

O launcher só confia em arquivo real do owner, modo 0600, um hardlink, inode
aberto e tuple exata. `BLACK`/`DEAD-CONTEXT` conclusivo emite `NXR0004`, envia
TERM e depois KILL somente ao PID/starttime do filho, invalida qualquer health
concorrente e retorna 72. Áudio, log de render e PID vivo nunca mascaram preto.
Imediatamente antes de cada KILL, PID/estado/starttime são relidos; uma sentinela
ou PID divergente nunca é alvo. `OK/non-black` não encerra o jogo.

Com `video_proof: "required"`, o launcher exporta
`NXBOOTSTRAP_VIDEO_REQUIRED=1` e um health `ready` só promove a geração depois
do `OK/non-black` da mesma execução. Antes da promoção, o launcher reabre e
autentica o receipt final atual: um OK observado que foi removido, substituído
por JSON malformado ou revogado deixa de valer. Os paths, schema, versão e tuple
completos de health são readonly antes do hook `port-env.sh`, portanto o adapter
não pode redirecionar, alterar nem apagar essa autoridade. Ausência do campo
preserva a promoção legada; um receipt fatal continua sendo veto em ambos os
modos.

## Dados de runtime autenticados na 0.7.2

O papel aditivo `runtime-data` leva dados imutáveis de um runtime gerenciado
(assemblies, metadados e configurações) na mesma geração transacional dos ELFs.
Cada membro deve ser `0644`, viver estritamente abaixo de um
`private_library_paths` declarado e continua preso a path, modo e SHA-256.
Dados do dono, saves e caminhos fora da closure permanecem recusados.
## Rollback após crash real na 0.7.1 (V4-ROLLBACK-REAL-CRASH)

O limite continua sendo três falhas pré-health consecutivas da mesma geração
pendente. Cada tentativa pertence a um `run_id` novo: receipt atrasado de outra
execução, PID reciclado e processo concorrente nunca promovem nem absolvem a
pendência. A abertura seguinte persiste o rollback para `previous_healthy`
antes de executar seus bytes; se a âncora anterior estiver ausente, incompleta,
incompatível ou se o novo estado não puder ser publicado, a abertura falha
fechado e orienta executar o `nxdoctor`, sem conceder uma quarta tentativa à
geração defeituosa.

O gate `tests/test-v4-rollback-real-crash.sh` usa o launcher realmente gerado e
processos reais para provar `SIGABRT`, `SIGTERM`, saída não zero, duas aberturas
concorrentes entre gerações e recibos stale/late/recycled. No cenário concorrente,
A continua vivo enquanto os arquivos top-level são substituídos atomicamente
por B; o launcher B é recusado antes de criar outro runtime. Cada tentativa deixa
um recibo estruturado com PID/starttime/run-id/status, deadline monotônica derivada
de `/proc/uptime`, projeção integral do estado, hashes dos artefatos e auditoria de
lock, staging, temporários e dados do dono. Os bytes da NXSplash e o fluxo visual
não mudam nesta versão.

## Seed visível `.nxb` na 0.7.0 (V4-REPACK-01)

Todo port de schema v3 emite `<port-id>/nxruntime-<generation>.nxb`, um arquivo
regular, visível e determinístico com a closure imutável completa. `.nxruntime`
passa a ser cache local: um ZIP pessoal que descarta dotdirs continua
instalando, e apagar a cache é recuperável somente pelo seed autenticado. A
raiz de confiança, o que ela detecta e o que ela deliberadamente não promete
estão em `BUNDLE-TRUST-V4.md`.

### Códigos de update introduzidos na 0.7.0

- `NXU0012` — diagnósticos do seed visível: cache reconstruída a partir dele,
  seed ausente/não regular, geração declarada diferente da compilada no
  launcher, membro que falhou autenticação, closure materializada que não
  autenticou e launcher instalado fora da closure do seed.
- `NXU0013` — recusa por espaço insuficiente antes de qualquer escrita, sempre
  acompanhada do recibo `STORAGE:`. A geração ativa nunca é destruída para
  abrir espaço.
- `NXU0014` — o arquivo de eventos foi cortado no teto de 1 MiB ao fim da
  execução, guardando a cauda. O registro é ele próprio um evento válido, com
  os bytes descartados, porque quem lê `events.jsonl` lê linha a linha.

## Generation runtime v2 na 0.6.37

Um manifesto v3 declara uma lista ordenada exata de membros com `role`, `path`,
`mode` e `sha256`. O gerador acrescenta a NXSplash canônica e exige exatamente
um `executable` igual a `nxport.executable`; bibliotecas só podem estar abaixo
de `private_library_paths` e hooks somente em `port-env.sh` ou no prepare
declarado. O modo de um hook sourced pode ser `0644`; um hook executado também
pode ser `0755`.

Quando `nxextract.mode` é `yes` ou `auto`, a mesma lista precisa conter
exatamente os cinco membros canônicos: `extractor.json` (`nxextract-recipe`,
`0644`), `nxextract/nxextract.py` (`nxextract-engine`, `0644`),
`nxextract/run-extractor.sh` (`nxextract-runner`, `0644`),
`nxextract/nxextract-runtime-env.sh` (`nxextract-runtime-env`, `0644`) e
`nxextract/nxextract-ui` (`nxextract-ui`, `0755`). Módulos, scripts e ferramentas
adicionais usam `nxextract-helper` (`0644` ou `0755`); JSON, tabelas, specs e
outros dados imutáveis usam `nxextract-spec` (`0644`). Todo helper/spec fica em
path normalizado sob `nxextract/`, sem lista de nomes, extensões ou teto ligado a
um jogo. `nxextract.mode=no` recusa qualquer desses papéis.

A identidade `nxruntime-generation-v2` liga launcher, `nxport.json`, NXSplash,
ELF, bibliotecas privadas, hooks e a closure NXExtract completa. Os bytes
imutáveis ficam exclusivamente em `files/runtime/<path>` e curam o mesmo
`$GAMEDIR/<path>`; launcher e nxport conservam seus dois mapeamentos internos.
Dados do dono, saves, GPTK e settings nunca entram na closure. Antes do
preflight, cada fonte precisa ser arquivo real, sem symlink e com SHA-256 exato.
A cura resolve todos os alvos, recusa arquivos não declarados sob `nxextract/`,
stageia todos os membros divergentes e só então publica as renames. Falha no
staging não altera nenhum byte live; interrupção durante as renames nunca chega
à extração/jogo, pois a raiz inteira é relida e a abertura seguinte completa a
mesma geração imutável. Essa abertura remove somente temporários `nxheal` com
formato reservado cujo PID criador já morreu dentro da árvore NXExtract gerida
pelo framework; paths do dono fora dela nunca são alvo dessa limpeza.
Temporário malformado, symlink ou com criador vivo falha fechado.

Modos continuam exatos no build/ZIP. Se o PortMaster aplicar `chmod -R 777` em
mídia POSIX, a 0.6.37 primeiro autentica identidade, paths, tipos, SHA-256 e a
closure completa sem alterar bits; só depois restaura no store os `0644`/`0755`
declarados, repete a validação exigindo modo e cura a árvore live. Em
FAT/exFAT/FUSE que realmente ignora
`chmod`, o launcher só aceita a apresentação sintética depois de um probe no
mesmo diretório; hash e anti-symlink continuam obrigatórios, e o executável
precisa permanecer executável. Gerações v1 são `legacy-control-only` e nunca
viram `previous_healthy` de uma pendência v2. V2 sem `sha256sum` falha fechado.

`NXU0006` só é emitido depois que o rename de `state.json` persiste a transição
`pending -> active`. Falha nessa escrita emite `NXU0011`, retorna erro e não
promove a geração.

## Reconciliação e suporte seguro na 0.6.35

A 0.6.35 reúne, no mesmo sucessor, o NXExtract 1.2.21 opt-in da 0.6.34 e o
ownership fail-closed do fallback de lock criado na linha 0.6.33. Sem `flock`,
somente o processo que criou o diretório e cujo arquivo owner ainda contém seu
PID/token exatos pode removê-lo; uma segunda abertura nunca herda essa
autoridade. O registro canônico também passa a identificar o engine e o runner
1.2.21 pelos hashes reais, preservando 1.2.20 como versão suportada para ports
anteriores.

O bloco `SUPPORT` não lista mais logs crus para envio. Esses arquivos são
entradas internas do diagnóstico e podem conter dados privados. O usuário deve
compartilhar somente o bundle sanitizado gerado pelo `nxobs`, ou o hash do seu
manifesto.

## NXExtract 1.2.21 na 0.6.34

Novos manifests optam pelo NXExtract `1.2.21`, incluindo o caminho
`reuse-only` que falha antes de recuperação, UI, busca de fonte ou extração.
Ports já publicados continuam presos aos seus pins e só migram com bump e ZIP
próprios. O template do launcher, a NXSplash de cinco segundos e todas as telas
existentes permanecem inalterados.

## Gerações e health receipt na 0.6.32

O `generation_id` é um SHA-256 completo e determinístico do manifesto do port,
das fontes exatas do gerador/template e do artefato NXSplash da ABI. Uma geração
com `commit` nunca é sobrescrita: bytes diferentes sob o mesmo ID falham
fechado. IDs antigos de 32 dígitos continuam somente como leitura de migração.

Atualizações entram como `pending`; `active` só muda depois de o runtime gravar
atomicamente no caminho privado `NXBOOTSTRAP_HEALTH_FILE` a linha JSON exata:

```json
{"schema":"org.nextos.nxruntime.health","schema_version":1,"run_id":"$NXBOOTSTRAP_HEALTH_RUN_ID","generation":"$NXBOOTSTRAP_HEALTH_GENERATION","port_id":"$NXBOOTSTRAP_HEALTH_PORT_ID","status":"ready"}
```

Os valores são variáveis exportadas, não texto literal. O produtor deve criar
um temporário exclusivo com `umask 077` e renomeá-lo sobre o destino. O launcher
confere arquivo real, owner, modo 0600, um hardlink, inode aberto, uma única linha,
run, geração e port. Log, PID vivo, splash e exit 0 nunca contam como saúde.
Três falhas pré-health da mesma pendência fazem a abertura seguinte usar
`previous_healthy`, sem segunda instância e sem tocar nos dados do dono.

## Execução mixed ABI opt-in na 0.6.27

O campo opcional `execution_roles` separa o processo host do payload-alvo. Cada
papel declara arquitetura, executável, política de executor, interpreter e
closure; o contrato completo está em
[`EXECUTION-ROLES.md`](EXECUTION-ROLES.md). Sem esse campo, o gerador conserva o
caminho legado. Com ele, o launcher:

- mantém NXExtract AArch64 no ambiente host;
- resolve NXSplash e jogo ARMHF de forma independente;
- prefere o interpreter nativo e só procura um loader alternativo para o papel
  cujo `PT_INTERP` realmente está ausente;
- verifica classe ELF e `e_machine` antes de aceitar executável, loader ou raiz
  de provider;
- monta closures separados e nunca acrescenta o `usr/lib` AArch64 comprovado
  no ambiente muOS reduzido ao closure ARMHF;
- registra um `EXECUTION RECEIPT` por papel e falha fechado sem rota coerente.

A ABI global histórica continua sendo a ABI do jogo, preservando manifests e
consumidores existentes. O novo exemplo é
[`nxport-mixed-armv7.example.json`](examples/nxport-mixed-armv7.example.json).

## Mudanças da 0.6.15

- fixa NXExtract `1.2.10` para novos manifests e valida seu
  `nxextract-result.json` estritamente antes de copiar uma única linha
  `NXEXTRACT_RESULT` sanitizada para `log.txt`; resultado ausente, antigo,
  ligado por symlink, malformado ou incompatível com o status do processo
  falha fechado;
- publica por rename atômico o último limite em `nxphase-result.json` e emite
  registros humanos `PHASE` e estruturados `NXEVENT` para preflight,
  NXExtract, NXSplash e runtime; adapters recebem o protocolo explícito para
  registrar `video-provider` e `first-frame` sem pipe, frame extra ou mudança
  de lifecycle;
- instala o trap de saída antes de resolver o launcher ou procurar PortMaster;
  falhas pré-log tentam, em ordem, game dir, launcher dir, runtime privado e
  `TMPDIR`/`/tmp`, com arquivo exclusivo por PID, `umask 077`, status verdadeiro
  e uma única finalização PortMaster;
- mantém NXSplash `0.1.2` e os artefatos/pixels da UI NXExtract `1.2.9`
  exatamente iguais; a mudança é de observabilidade e engine, não de layout,
  cor, tipografia, duração, renderer ou fallback visual.

## Mudanças da 0.6.14

- eleva somente o pin de novos manifests para NXExtract `1.2.9`, mantendo o
  package Android obrigatório e a compatibilidade do container por duas ou mais
  identidades explícitas ou por âncoras internas fortes;
- preserva byte a byte o template, o runtime legado e o fluxo do launcher da
  0.6.13; no gerador muda exclusivamente o slot `NXEXTRACT_VERSION`;
- mantém NXSplash `0.1.2`, sua tela de cinco segundos, seus artefatos e seu
  manifesto sem qualquer alteração;
- mantém o ELF AArch64 NXSplash de SHA-256 `d85d896a906a778c9af250e5617d45d085a98b18552cb0254addbbc626036c97`
  como `physical-unverified` até o mesmo ZIP Hitman GO passar nos dois aparelhos
  autorizados; pin imutável não é recibo físico;
- conserva como gates cumulativos a execução sem o comando externo `stat`, a
  identificação segura dos CFWs e a assinatura dArkOSRE de dois marcadores.

## Mudanças da 0.6.13

- adota o NXSplash `0.1.2` por pin imutável e mantém o artefato selecionado
  exclusivamente pela arquitetura declarada no manifesto; o helper tenta SDL,
  recuperação portátil de provider e o mesmo layout gráfico via framebuffer,
  deixando a representação TTY apenas para diagnóstico explícito;
- eleva novos manifests para NXExtract `1.2.8`, restaurando o orçamento de
  negociação gráfica, aceitando somente prontidão SDL/fbdev e separando os
  ELFs públicos por arquitetura;
- preserva byte a byte o template e o runtime legado da 0.6.12; no gerador muda
  somente o slot do pin NXExtract, e no launcher materializado mudam somente os
  slots declarados de versão do nxbootstrap e do NXSplash;
- torna permanente a prova da correção 0.6.8: `.OS` limitada e regular,
  assinatura dArkOSRE de dois marcadores, rejeição de nome hostil e uso da
  identidade apenas em log/mod protegido;
- continua executando em perfis sem o comando externo `stat` e não migra nem
  regenera automaticamente qualquer port já aprovado.

## Mudanças da 0.6.12

- Eleva o pin explícito dos ports recém-gerados para NXExtract `1.2.7`, cujo
  runner só inicia a varredura do APK depois de SDL ou o TTY ativo confirmar
  uma saída realmente visível.
- Mantém o splash NEXTOS/RETRO ELITE obrigatório por cinco segundos em toda
  abertura, inclusive no marker fast-path sem nova extração.
- Preserva os packages já aprovados: eles continuam presos à versão e ao hash
  antigos até migração opt-in e novo ZIP.

## Mudanças da 0.6.11

- `nxextract-ui` passa a integrar o conjunto obrigatório do NXExtract; o
  launcher falha fechado se a interface estiver ausente, vazia, ligada por
  symlink ou sem possibilidade de execução;
- a interface continua restrita à instalação/extração de dados, enquanto
  `nxsplash-nextos` permanece obrigatório por cinco segundos em toda abertura;
- a ordem canônica é `NXExtract/UI (quando necessário) → gate do payload →
  nxsplash (sempre) → adapter/bibliotecas → jogo`.

## Mudanças da 0.6.10

- adota o `nxsplash-nextos` 0.1.1 e fixa o novo artefato por arquitetura;
- mantém o handoff no mesmo ponto do fluxo nativo e permite que o helper use o
  VT ativo publicado pelo kernel quando um PortMaster antigo deixa `CUR_TTY`
  vazio;
- continua sem carregar provider gráfico específico, biblioteca privada ou
  adapter do jogo antes da tela, preservando os outros firmwares.

## Mudanças da 0.6.9

- instala automaticamente o `nxsplash-nextos` 0.1.0 correspondente à arquitetura
  do manifesto e fixa seus bytes pelo release manifest do componente;
- inclui o helper implicitamente em `required_files`, sem campo ou opção para
  desligar/pular a tela no port gerado;
- executa a tela bilíngue de cinco segundos somente depois de NXExtract, prepare
  e validação completa do payload, mas antes de `HOME`, `port-env.sh`, bibliotecas
  privadas e qualquer entrada nativa do jogo;
- mantém a seleção de backend do firmware como primeira tentativa e deixa o
  próprio helper fazer fallback fail-open de renderer, sem contaminar o jogo.

## Mudanças da 0.6.8

- mantém uma identidade de CFW segura mesmo quando integrações antigas do
  PortMaster não conseguem preencher `CFW_NAME`: lê apenas um
  `~/.config/.OS` regular/não-symlink e reconhece dArkOSRE somente quando dois
  artefatos independentes do firmware também são regulares/não-symlink;
- limita essa identidade a observabilidade e à seleção já protegida de
  `mod_${CFW_NAME}.txt`; nomes de CFW/dispositivo continuam proibidos como
  condição suficiente para qualquer workaround;
- preserva integralmente os pisos das versões anteriores: lock sem o comando
  externo `stat`, diagnóstico pré-runtime 0600, handoff seguro de `PM_PIPE` e
  `pm_finish` exatamente uma vez.

## Mudanças da 0.6.7

- restaura no launcher autocontido o handoff de diálogo já exigido pelo contrato:
  depois de `pm_platform_helper`, um `PM_PIPE` ainda ativo só é aceito como FIFO
  vivo, não-symlink, e é fechado exclusivamente por `PortMasterDialogExit`;
- falha fechada e registra o motivo no `log.txt` quando o pipe é inseguro, a API
  está ausente/falha ou o pipe permanece depois do pedido de fechamento;
- mantém cumulativamente o lock sem o comando externo `stat`, o diagnóstico
  pré-runtime da 0.6.6 e a finalização única do PortMaster.

Esses cenários são pisos permanentes de release: toda versão posterior precisa
manter verdes tanto a execução com `stat` ausente quanto o handoff completo de
`PM_PIPE`. O gate estático falha se qualquer uma dessas provas executáveis for
retirada da suíte.

## Limite pré-main

Antes que uma biblioteca estática dentro do loader possa executar, o sistema precisa
resolver:

- arquitetura do kernel/userland e `PT_INTERP`;
- loader dinâmico ARMHF ou AArch64;
- paths de bibliotecas do firmware, PortMaster e do port;
- `PORT_32BIT` que alguns firmwares detectam no wrapper;
- dados do dono que ainda precisam ser extraídos;
- lock, supervisão do filho direto, sinais e retorno ao frontend.

Essas responsabilidades são de `nxbootstrap`. Detecção gráfica, seleção real de
backend e quirks da engine continuam dentro do ELF do jogo. O ELF separado do
nxsplash apenas apresenta a identidade do framework, encerra o próprio estado
SDL e sai antes do runtime nativo.

## Fases de execução

### Histórico — biblioteca 0.5.1 aposentada

A biblioteca de runtime aposentada (`nxbootstrap.sh`, mantida na árvore apenas
como evidência de contrato; nunca mais gerada) tinha `nxbootstrap_main` nesta ordem:

1. valida toda a configuração, retira paths privados herdados e abre/rotaciona o
   log por descritor verificado;
2. entra no diretório real do port;
3. carrega `control.txt`, o `mod_${CFW_NAME}.txt` seguro e `get_controls`;
4. descobre um runtime de sessão existente e gravável;
5. valida loader, arquitetura e interpretador da ABI;
6. adquire lock fora do filesystem do jogo e falha fechado se ele estiver ocupado;
7. prepara o ambiente de bibliotecas do host e o mapping do PortMaster;
8. chama `pm_platform_helper` antes de qualquer UI/extractor;
9. executa NXExtract em primeiro plano e, depois, o prepare declarado;
10. verifica todos os arquivos obrigatórios;
11. acrescenta bibliotecas privadas e aplica `HOME` somente se solicitado;
12. inicia um único filho, repassa sinais, espera o status e chama `pm_finish` uma
    única vez.

Separar ambiente do host e ambiente privado é intencional. `pm_platform_helper` e
NXExtract precisam enxergar primeiro as bibliotecas do firmware/PortMaster; as `.so`
do jogo só entram depois, para não contaminar ferramentas de setup.

`nxbootstrap` não usa `setsid` e não solta o jogo sem supervisão. O shell permanece
como pai, mesmo que o loader seja iniciado em background internamente para permitir
traps e `wait` corretos.

## PortMaster

No produto final, PortMaster é obrigatório. Quando `control.txt` é encontrado,
`nxbootstrap`:

- carrega a integração do firmware;
- aceita a `controlfolder` canônica que o controle publicar;
- chama `get_controls` quando disponível e exporta apenas o mapping real recebido;
- aceita da integração PortMaster, depois de `get_controls`, somente
  `ANALOGSTICKS=0|1|2` (com
  `ANALOG_STICKS` como alias), exportando a dica sanitizada
  `NXINPUT_ANALOG_STICKS_HINT`; valor ausente ou inválido fica unset e a dica
  nunca substitui os bindings por controle;
- passa o executável verdadeiro para `pm_platform_helper`, registrando uma falha
  opcional sem impedir o jogo;
- aceita `PM_PIPE` como sinal de handoff somente quando ele é um FIFO vivo e não um
  symlink; tipo inseguro, API ausente/falha ou FIFO persistente abortam antes do jogo;
- chama `pm_finish` exatamente uma vez em sucesso, falha ou sinal.

A procura do launcher gerado aceita raízes ArkOS, ROCKNIX, muOS,
Knulli/Batocera, TrimUI, NextOS e a raiz XDG, além de manter candidatos de
descoberta para MIYOO_EX e RetroDECK. Esses dois candidatos não têm fixture nem
runtime comprovado e permanecem `unsupported/unverified`; encontrá-los não é
claim de suporte. A procura usa o
primeiro `control.txt` regular de verdade, portanto um diretório vazio em uma
raiz de maior prioridade não esconde a integração válida seguinte. Arquivo de
controle e mod não podem ser symlinks. Um nome `CFW_NAME` não seguro é ignorado.
Quando o controle antigo não publica um nome, o fallback da 0.6.8 observa um
`.OS` seguro ou, para dArkOSRE, exige o par de marcadores documentado no gate.
Essa observação não ativa quirks nem escolhe providers.
O diretório canônico selecionado é exportado para `nxcompat`, e o mapping real
continua vindo exclusivamente de `get_controls`.

O contrato completo e as fontes oficiais fixadas estão em
[`../portmaster`](../portmaster/README.md).

Se o PortMaster não existir, o bootstrap registra “standalone mode” para permitir
testes e diagnósticos. Isso não autoriza publicar um port sem testar o fluxo real do
PortMaster, controles e retorno ao frontend.

## Manifesto e gerador

O arquivo declarativo `nxport.json` elimina cópias manuais divergentes. Exemplo:

```json
{
  "schema_version": 2,
  "id": "jogo",
  "title": "Jogo",
  "launcher_name": "Jogo.sh",
  "architecture": "aarch64",
  "executable": "jogo-loader",
  "argument_mode": "game-dir-and-passthrough",
  "home_mode": "preserve",
  "nxextract": {"mode": "auto", "version": "1.2.10"},
  "required_files": ["jogo-loader"],
  "private_library_paths": ["libs"],
  "prepare_script": "",
  "required_capabilities": [
    "host.portmaster",
    "graphics.gles2",
    "input.controller-mapping"
  ],
  "enabled_quirks": [],
  "language": {
    "default": "auto",
    "supported": ["en", "es", "pt-br"]
  },
  "runtime_report": "log-and-logo"
}
```

Campos:

| Campo | Contrato |
| --- | --- |
| `id` | identificador minúsculo e seguro, até 63 caracteres |
| `launcher_name` | basename terminado em `.sh` |
| `architecture` | `armv7`, `aarch64`, `i386` ou `x86_64` |
| `executable` | caminho relativo normalizado dentro do port |
| `argument_mode` | nenhum, passthrough, game-dir, ou ambos |
| `home_mode` | `preserve` por padrão; `port` somente opt-in |
| `nxextract` | objeto com modo `auto`/`yes`/`no` e versão exata `1.2.10` |
| `required_files` | arquivos não vazios exigidos antes do launch |
| `private_library_paths` | diretórios privados relativos, somente na fase do jogo |
| `sdl_provider` | opt-in fechado `system`: firmware antes de PortMaster, inherited paths internos ao port removidos, overrides resolvidos/fail-closed e rota do executável presa ao manifesto; nenhum backend SDL é forçado; ausente preserva a resolução legada |
| `video_proof` | opt-in fechado `required`: health só promove após receipt `OK/non-black` da mesma execução; ausente preserva a promoção legada |
| `prepare_script` | fase Bash relativa, opcional e específica do jogo |
| `required_capabilities` | nomes exatos do registry finito; cada um só é satisfeito no nível de evidência declarado |
| `enabled_quirks` | quirks `adapter.*`, `engine.*` ou `game.*`, vazios por padrão |
| `language` | opt-in somente para jogos que realmente trocam idioma; define `GAME_LANGUAGE` visível e exporta `NXPORT_LANGUAGE` ao adapter |
| `runtime_report` | `log` ou `log-and-logo`; nunca pode desativar o diagnóstico |

O gerador rejeita campos desconhecidos, controles, paths absolutos/`..`, duplicatas,
paths pessoais, capability ausente do
[`capabilities-v1.json`](../nxcompat/capabilities-v1.json), seletores de device,
arquiteturas e modos inválidos. Nenhuma capability é obrigatória por padrão; nome de
firmware/aparelho e presença de arquivo não substituem o receipt exigido. O executável
e `nxsplash-nextos` entram automaticamente em `required_files`. Escritas são atômicas e arquivos
existentes não são sobrescritos sem `--force`. Nem `--force` autoriza uma raiz de
saída ou diretório do port que seja symlink; um target symlink é substituído como
entrada de diretório, nunca seguido.

`language` não existe por padrão. Quando o port declara idiomas comprovadamente
suportados, o launcher ganha no topo uma linha editável `GAME_LANGUAGE=auto` e
valida o valor contra a lista declarada antes de exportar `NXPORT_LANGUAGE`. O
adapter do jogo traduz esse código para o formato nativo. O framework nunca altera
o `LANG`/locale global do Linux, portanto ferramentas, firmware e NXExtract não são
afetados. Ports sem troca de idioma não exibem nem exportam essa opção.

Entrada legada `schema_version: 1` continua aceita apenas pelo gerador, que a atualiza
deterministicamente e grava sempre v2. Um release público aceita somente a saída v2
canônica; não existe downgrade silencioso. Os schemas estão em
[`schema`](schema/nxport-v2.schema.json), e o lock semântico das três camadas e dos
dez componentes está em
[`../contracts/declarative-v1.json`](../contracts/declarative-v1.json).

```sh
python3 framework/nxbootstrap/tools/generate-port.py \
  caminho/nxport.json --output caminho/do/pacote
```

O resultado é:

```text
Jogo.sh                        launcher Bash único e autocontido, modo 0755
jogo/nxport.json               manifesto canônico, modo 0644
jogo/nxsplash-nextos           helper ELF fixado para a arquitetura, modo 0755
```

Não existe biblioteca de runtime nem receipt: a 0.6.0 aposentou
`nxbootstrap-*.sh` e `nxdeployment.json` do produto gerado (forma Limbo — a
biblioteca bash gigante foi um modo de falha real em campo). Toda a lógica de
host/plano/negociação vive no loader C (`nxcompat`/`nxgl`/`nxinput`); o shell
faz apenas o contrato PortMaster e as garantias dos ports de ouro:

- cadeia de descoberta das raízes suportadas do `controlfolder` → `control.txt` →
  `mod_${CFW_NAME}.txt` → `get_controls`, tudo opcional e fail-open;
- `GAMEDIR` derivado de `$directory` com fallback relativo por `readlink -f`;
- falha não-zero anterior ao log principal gera um
  `<port>-launcher-error.<pid>.log` exclusivo, modo 0600, primeiro no diretório
  do jogo quando já resolvido, depois ao lado do launcher e por fim em `/tmp`;
- `log.txt` durável com rotação para `log.prev.txt`;
- **instância única** por `flock` persistente por port ID num diretório de runtime
  0700 fora do jogo, com identidade path/FD e link count validados: trocar o ELF
  por outro inode não abre uma segunda instância;
- handoff de display por capacidade imediatamente depois do lock e antes de
  NXExtract: `pm_platform_helper` recebe o executável real e um `PM_PIPE` ainda
  ativo é fechado somente pela API oficial, com pós-condição comprovada;
- NXExtract em foreground e `required_files` fail-closed antes do jogo,
  abortando com `pm_finish` em falha;
- nxsplash obrigatório depois do gate de dados e antes de qualquer extensão ou
  biblioteca privada; ausência/falha do helper aborta, enquanto indisponibilidade
  de renderer é absorvida no helper sem impedir um jogo compatível;
- `port-env.sh` opcional e não-symlink como único ponto de extensão por-port;
- paths de firmware em ordem estável: overlays do PortMaster, provedores locais
  (`/usr/local`) e só então defaults de `/usr`; isso preserva o blob que o próprio
  CFW escolheu para seu kernel sem condicionar por nome de aparelho;
- jogo como filho direto supervisionado por PID + starttime: trap envia TERM,
  aguarda prazo finito, revalida a identidade antes de KILL e re-espera para
  devolver o status real; HUP/INT/TERM antes do jogo retornam 129/130/143 e
  `pm_finish` é protegido para executar exatamente uma vez;
- reset do console (`printf '\033c'` no `$CUR_TTY`) antes do `pm_finish`,
  cobrindo jogos que saem com o keymap em K_OFF.

## Diagnóstico

1. Sem `<port>-launcher-error.<pid>.log` e sem `log.txt` novo, não há prova de
   que o trap precoce foi alcançado: investigue entrada do frontend, `/bin/bash`,
   erro de parse, storage sem escrita, SIGKILL e perda de energia.
2. `<port>-launcher-error.<pid>.log` prova que Bash entrou no launcher e houve
   falha não-zero antes de abrir o log principal; ele registra versão, status,
   PID, launcher, diretório do jogo quando conhecido e CFW.
3. `log.txt` presente: o cabeçalho registra gerador/versão e `cfw=`; cada
   limite seguinte aparece como `PHASE` e `NXEVENT`, o resumo terminal do
   extrator aparece como `NXEXTRACT_RESULT`, e o status do filho direto fecha
   a sequência.
4. `nxphase-result.json` contém o último limite publicado integralmente. O
   schema `nxbootstrap-phase-v1` é um estado durável substituído por rename,
   não um token de segurança; adapters podem publicar `video-provider` e
   `first-frame` no mesmo caminho antes do launcher registrar `runtime EXIT`.
5. Loader iniciado: o runtime C assume o relato (probe/plano/backend do
   nxcompat), com os próprios marcadores.

## ARMHF literal e ABI

Para `architecture: armv7`, o wrapper visível gerado contém exatamente:

```sh
PORT_32BIT="Y"
export PORT_32BIT
```

O mesmo launcher exporta a variável usada pelo bootstrap. O bootstrap aceita kernel
ARMv7/ARMv8l ou AArch64, mas exige encontrar um interpretador ARMHF real. AArch64 exige userland/kernel
compatível e seu interpretador. Antes do launch, o bootstrap lê o header e os program
headers do ELF, confere class/little-endian/machine e exige exatamente o `PT_INTERP`
Linux canônico da ABI. Encontrar esse contrato correto ainda não prova que todas as
`DT_NEEDED` existem; a auditoria ELF integral do NXRelease continua obrigatória.

ARMv7 e AArch64 usam artefatos separados. Todo ELF Linux de um release público deve
ficar em `GLIBC_2.30` ou inferior, idealmente `GLIBC_2.17`.

## Bibliotecas e HOME

O `LD_LIBRARY_PATH` é montado sem duplicatas:

1. apenas na fase do jogo, paths privados declarados;
2. bibliotecas do PortMaster e da arquitetura correspondente;
3. diretórios do firmware correspondentes à ABI;
4. ambiente herdado, removendo qualquer path dentro do port;
5. diretórios genéricos do sistema.

Na fase de host/setup, o primeiro item é omitido desde antes de carregar o
`control.txt`. Diretórios privados são aceitos somente quando declarados e são
rejeitados se contiverem providers EGL, GL, GLES, GBM, DRM, Mali ou SDL.

`home_mode: preserve` é o padrão e deixa a sessão do firmware intacta.
`home_mode: port` exporta `HOME` para o diretório do jogo e deve ser escolhido
somente depois de confirmar onde a engine procura config/save. Save e cache permanecem
nos caminhos nativos da engine; qualquer redirecionamento adicional pertence ao adapter,
precisa ficar contido e ter prova própria. O manifesto genérico nunca escolhe save/cache
por firmware. O núcleo não inventa `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER`, resolução ou
override Mesa.

## Lock e supervisão exata

O lock fica num namespace 0700 dentro do runtime de sessão ou num diretório privado
sob o tmp local, nunca no cartão/FAT/exFAT do jogo. A identidade do arquivo aberto
é comparada pelo builtin `-ef` do Bash depois de validar diretório 0700, dono,
arquivo regular e ausência de symlink. A contagem de hardlinks vem do formato longo
padronizado de `ls`, disponível em POSIX, BusyBox e Toybox. O launcher não depende do
comando externo `stat`, que não existe em algumas imagens muOS. `flock -n` é usado
quando existe,
inclusive na forma limitada do BusyBox. Se `flock` não existe, o fallback `mkdir`
grava PID e starttime de `/proc`. Esse fallback nunca recupera automaticamente um
diretório existente: shell não oferece compare-and-swap seguro para distinguir um
lock órfão de um criador ainda entre `mkdir` e a gravação do owner. `HUP`, `INT`, `TERM`
e `EXIT` limpam normalmente; depois de um término não capturável, ele falha fechado até
uma manutenção explícita provar que o owner morreu. Nunca abre um segundo namespace.

Lock ocupado não autoriza o bootstrap a procurar ou encerrar a instância anterior. Ele
registra o conflito e aborta. Cwd, `comm`, cmdline, executável e ambiente são úteis para
diagnóstico operacional, mas não provam ownership suficiente para alimentar um sinal.

O bootstrap guarda somente o PID retornado por `$!` e seu starttime. Em
`HUP`/`INT`/`TERM`,
confirma ambos imediatamente antes de sinalizar esse filho direto, aguarda um prazo e
repete a confirmação antes do fallback terminal. Ele não enumera o namespace PID e não
tenta capturar descendentes por `/proc`. Uma fase ou engine que crie outros processos deve
supervisioná-los dentro do seu próprio adapter, continuar em foreground e só retornar quando
eles acabarem.

As suítes que criam processos também falham fechado. `tests/test-nxbootstrap.sh` e
`tests/test-generator.sh` recusam execução direta com status 77. O único entry point é
`tests/run-isolated.sh`, que exige `unshare` de user, PID e mount namespace com `/proc`
privado; se o kernel/firmware não oferecer isso, a suíte é pulada e nunca cai no host.
Descritores abertos dos três namespaces originais impedem liberar o teste apenas forjando
variáveis de ambiente. O processo interno precisa ser PID 1 e um watchdog limita CPU,
memória, arquivos, número de processos e tempo de parede. Ele registra PID+starttime do
único filho e só pode enviar TERM/KILL a esse filho depois de revalidar ambos. Quando o PID
1 privado termina, `--kill-child=KILL` contém qualquer resto no próprio namespace.
`tests/test-safety-static.sh` não cria processos do port e pode rodar diretamente.

Sinais `HUP`/`INT`/`TERM` param o filho supervisionado. O status do jogo é preservado e
`pm_finish`/lock são finalizados uma vez. O pedido `SELECT/BACK + START` de `nxinput`
deve entrar no shutdown seguro da engine; ele não deve chamar `kill` por fora do fluxo
nativo.

## NXExtract 1.2.10

NXExtract continua separado porque sua responsabilidade é preparar dados do dono,
não adaptar a engine. Para `nxextract.mode: yes` ou para `auto` com receita presente,
o port precisa vendorizar juntos:

- `extractor.json` regular e não-symlink;
- `nxextract/run-extractor.sh` regular e não-symlink;
- `nxextract/nxextract.py` e `nxextract/nxextract-runtime-env.sh` do conjunto
  canônico `1.2.10`;
- `nxextract/nxextract-ui` correspondente à arquitetura e ao manifesto
  imutável de apresentação `1.2.9`.

O runner é chamado explicitamente com `bash`, portanto funciona mesmo se ZIP/FAT
perder o bit executável. Ele roda em primeiro plano, com o diretório do jogo e o path
de bibliotecas do firmware informados, antes do prepare e do loader. Uma falha aborta
o port e aponta para o diagnóstico do extractor; a engine nunca deve concorrer com
cópia, backup ou rollback dos dados. Depois do processo, o launcher aceita
somente o schema terminal v1 completo, regular, não-symlink, com um hardlink,
versão `1.2.10` e outcome coerente com o status; a linha canônica resultante
vai para o log principal antes de qualquer próxima fase.

NXExtract não deve ser ligado dentro de `nxcompat`, e o loader não deve reimplementar
seu journal/rollback.

## Integração com o loader

O bootstrap exporta:

- `NXCOMPAT_PORT_ID`;
- `NXCOMPAT_GAME_DIR`;
- `NXCOMPAT_REQUIRED_CAPABILITIES`;
- `NXCOMPAT_ENABLED_QUIRKS`;
- `NXCOMPAT_RUNTIME_REPORT`;
- `NXOBS_PHASE_PROTOCOL=nxbootstrap-phase-v1`, `NXOBS_PHASE_FILE`,
  `NXOBS_EVENT_PROTOCOL=nx-event-v1` e `NXOBS_EVENT_SINK=stdout` para o adapter
  publicar somente observações reais;
- `SDL_GAMECONTROLLERCONFIG` somente quando `get_controls` devolveu valor real;
- `PORT_32BIT=Y` no fluxo ARMHF.

O loader usa os dois primeiros no `nxcompat_probe`, deixa `nxcompat` negociar vídeo e
áudio por abertura real e cria `nxinput` depois que o mapping herdado está visível.
O shell não precisa crescer quando uma engine exige um quirk: esse código pertence à
ponte compilada e deve ser protegido pela capacidade/evidência correspondente.

## Testes

```sh
bash framework/tests/run-safe-gates.sh \
  --log-root /caminho/absoluto/para/logs
```

A bateria do gerador verifica validação, não-overwrite, determinismo, output/target
symlink, sintaxe, relocação por symlink, argumentos, log exclusivo e o literal ARMHF.
A bateria do lifecycle usa um PortMaster e loader sintéticos para verificar ordem de
fases, mapping, `HOME` opt-in, NXExtract sem bit executável, prepare, hardlinks,
HUP, helper, `pm_finish`, status do filho e lock fora do filesystem do jogo.
`test-phase-observability.sh` injeta sete cortes, valida o resumo do NXExtract,
inspeciona provider/primeiro frame enquanto o filho ainda vive e comprova a
publicação atômica. O checkpoint
[`p06-phase-observability-v1.json`](p06-phase-observability-v1.json) liga
OBS-012 e OBS-021..040 a essas provas. O ledger
[`m06-audit-v1.json`](m06-audit-v1.json) liga os 30 requisitos M06 a
seus tokens de implementação e ataques. O teste de supervisão usa `/proc` sintético e um sink de sinal; o único
sinal real da infraestrutura é exercitado contra um filho exato dentro do namespace
selado. A matriz e os detalhes estão em [`../tests`](../tests/README.md).

Esses testes não substituem o gate no aparelho. Antes do release, confirme via
PortMaster que não existe instância anterior, o logo mostra capacidades/backends,
vídeo e som entram automaticamente, controles e hotplug funcionam, save/reload
persistem e a saída devolve o frontend sem processo residual.

## Biblioteca 0.5.1 (`nxbootstrap.sh`) — CONGELADA como evidência

`nxbootstrap.sh` (0.5.1) NÃO é a implementação corrente: o launcher publicado
é GERADO por `templates/launcher.sh.in` + `tools/generate-port.py`. O arquivo
permanece BYTE-EXATO como evidência aposentada (o gate de preservação trava o
hash — por isso o aviso mora aqui no README, nunca dentro do arquivo). Não
consultar como referência de comportamento: dois achados da auditoria de
23/08/2026 nasceram de lê-lo como se fosse o atual. Os comportamentos
superiores dele (fallback de flock, tabela completa de raízes PortMaster)
foram migrados ao template na onda v2.
