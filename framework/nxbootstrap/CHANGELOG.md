# 0.8.4 (2026-09-05, V5 — SDL herdada obsoleta do ArkOS)

- Corrige o caso físico que ainda falhava na 0.8.3: no K36S autorizado o
  PortMaster herdou `/usr/$LIB/libSDL2-2.0.so.0.3000.10`, enquanto o ArkOS
  fornecia apenas SDL `0.3200.x`. A própria `ld.so` ignorou o objeto ausente e,
  sem mapeamento para provar, a barreira conservadora 0.8.3 recusou o jogo.
- `sdl_provider=system` agora remove do ambiente efetivo do filho somente uma
  entrada SDL1/2 não resolvida com o segmento completo `$LIB`/`${LIB}`
  diretamente sob `/usr` ou `/lib`, somente quando ela veio do ambiente
  herdado e o adapter não acrescentou outro `LD_PRELOAD`. A remoção recebe
  diagnóstico nominal; o jogo usa a SDL atual pela resolução normal do
  firmware.
- O caso não vira allowlist genérica: caminho fora de `/usr`/`/lib`, componente
  adicional, basename não SDL, valor criado pelo adapter, token em outra
  variável, destino privado, ambiguidade e todo override não resolvido
  restante continuam falhando fechado. Um token herdado realmente mapeado
  preserva exatamente o literal anterior.
- Regressão no launcher real cobre `$LIB` e `${LIB}` obsoletos descartados,
  filho/helper sem contaminação, adapter obsoleto recusado e objeto não SDL
  recusado, além de toda a matriz 0.8.3.

# 0.8.3 (2026-09-05, V5 — token `$LIB` herdado do ArkOS)

- Corrige a recusa anterior ao jogo observada no ArkOS/PortMaster quando o
  frontend herda `LD_PRELOAD=/usr/$LIB/libSDL2-2.0.so.0.3000.10`. `$LIB` e
  `${LIB}` são tokens do dynamic loader, não variáveis shell não resolvidas.
- O modo `sdl_provider=system` agora admite esses tokens somente em
  `LD_PRELOAD` e somente quando a expansão exata já está carregada, de forma
  única, no processo do launcher segundo `/proc/$$/maps`. O valor literal é
  preservado para o filho; o framework continua executando seus helpers com o
  override em quarentena.
- A resolução não usa `eval`, não adivinha `lib`, `lib64` ou multiarch e não
  cria allowlist específica do ArkOS. Token não carregado, ambíguo, em outra
  variável ou que resolva para SDL privada continua falhando fechado.
- Regressão no launcher real cobre `$LIB`, `${LIB}`, o basename exato do SDL
  do ArkOS, token ausente e token package-private.

# 0.8.2 (2026-09-03, V5 revisão 2 F6: migração do `.new` sem pin)

- Um `port-env.sh.new` escrito por launcher 0.8.0 (sem `.nxruntime/<f>.new.sha256`) deixa de ser classificado como
  "editado pelo dono" (o que congelava a oferta para sempre): igual ao default anterior = era oferta nossa, é
  migrado para o default novo e pinado; bytes desconhecidos = preservados como `.new.unpinned`, e o default novo é
  ofertado com pin. Receipts nominais. Provado no launcher REAL (test-owner-e6a-real.sh, boots 5 e 6).
- Orçamento de linhas do launcher subido pelo valor MEDIDO (motivo no test-generator.sh).

# 0.8.1 (2026-09-03, V5 E6a — o `.new` do dono é real e não congela)

- **Defeito fechado (E6a).** O `.new` só era escrito quando ainda não
  existia. Depois do primeiro default novo, um dono que tinha customizado o
  `port-env.sh` ficava preso àquela oferta PARA SEMPRE: um terceiro, quarto e
  quinto default nunca chegavam a ele, e nenhuma linha de receipt dizia isso.
  Reproduzido antes da correção (default v1 → semeia; dono edita; default v2 →
  `.new` = v2; default v3 → `.new` continua v2).
  Agora existe um pin próprio da oferta (`.nxruntime/<arquivo>.new.sha256`):
  * `.new` ausente → instala e pina;
  * `.new` presente e igual ao pin (a oferta do framework, intocada) →
    ATUALIZA para o default mais novo, repina e emite
    `OWNER FILE: <arquivo>.new refreshed to the newest default`;
  * `.new` presente e diferente do pin (o dono escreveu ali) → **preserva os
    bytes** e emite `OWNER FILE: <arquivo>.new was edited; kept as it is and
    the newest default was NOT written over it`.
  O pin do default (`<arquivo>.default.sha256`) continua sem avançar nesse
  caminho, senão a oferta se perderia; o arquivo VIVO do dono nunca é tocado
  em nenhum dos três casos, e a operação segue idempotente.
- **Prova nova, no launcher REAL (`tests/test-owner-e6a-real.sh`).** O
  `test-owner-runtime.sh` prova o MODELO: extrai as funções do
  `templates/launcher.sh.in` e as executa direto. Isso nunca pegaria um
  launcher que renderiza os helpers e não os chama, chama na fase errada ou
  chama com um GAMEDIR diferente do que o jogo recebe. O gate novo renderiza
  um launcher com `tools/generate-port.py` (schema v3 + `--runtime-root`,
  bytes reais e digests reais), monta uma árvore PortMaster e faz QUATRO
  aberturas de verdade, lendo só o que um aparelho veria — o log do launcher e
  os bytes no disco: semeia uma vez (e o jogo recebe o valor semeado); dono
  edita + default novo → bytes do dono intactos, `.new` oferecido e a edição
  chega ao estado efetivo sem rebuild nem repack; terceiro default → `.new`
  ATUALIZADO; `.new` editado pelo dono → preservado e nomeado. Com o template
  anterior o gate reprova exatamente no terceiro default.

# 0.8.0 (2026-09-03, V5 7A.1/7A.2 — owner runtime: port-env.sh do dono, nunca curado)

- Opt-in `owner_runtime: "1"` no `nxport.json` (schema v3, exige o store de
  gerações V4). Separa papéis E paths: `defaults/port-env.sh` é só semente do
  pacote; `<port>/port-env.sh` é o hook vivo do dono, semeado UMA vez de forma
  atômica (`set -C`, nunca segue symlink), preservado por health/healing/update
  (default novo vira `port-env.sh.new` com receipt; cópia intocada migra com
  receipt); a lógica indispensável do adapter migra para `adapter-env.sh`,
  helper SELADO e membro normal da geração (papel `runtime-hook`).
- Sob `owner_runtime`, `port-env.sh` NUNCA entra numa geração, em
  `required_files`, em `prepare_script` nem no conjunto de hash da saúde
  (`nxbootstrap_relative_path_safe` o classifica como owner, ao lado de
  `NEXTOSCONTROLLERS.gptk`/`NEXTOSSETTINGS.txt`); o gerador recusa cada uma
  dessas tentativas com mensagem nominal. Sem o opt-in, o launcher renderiza
  como antes (V4 inalterada: hook em geração, cópia viva curada).
- Ordem observável e guardada: `NX-OWNER-RUNTIME/1
  order=heal-sealed,extract,splash,seed-owner,sealed-env,owner-env,launch`.
  O helper selado é lido antes do hook do dono. Antes de executar um hook,
  `LC_ALL=C bash -n` — hook que não parseia aborta CEDO e VISIVELMENTE com o
  diagnóstico origem:linha do próprio bash; nunca last-known-good silencioso.
  Variáveis reservadas do plano de controle (`GAMEDIR`, `PORT_ID`,
  `NXBOOTSTRAP_LAUNCHER_DIR`, `NXBOOTSTRAP_GENERATION_ID/FORMAT`,
  `NXBOOTSTRAP_OWNER_RUNTIME`, `NXBOOTSTRAP_VIDEO_REQUIRED`,
  `NXBOOTSTRAP_HEALTH_FILE`, `NXBOOTSTRAP_VIDEO_FILE`,
  `NXBOOTSTRAP_LOGICAL_GAMEDIR`, `NXBOOTSTRAP_ANALOG_STICKS_HINT`) são
  fotografadas antes e comparadas depois: mudança = falha visível citando o
  NOME da variável; os bytes do dono nunca são reescritos por nenhum caminho.
  Variáveis de jogo/runtime/vídeo pertencem ao dono. Sob `sdl_provider=system`
  o mesmo bloco guardado roda dentro da quarentena de overrides do provider.
- Gates novos: `nxbootstrap-owner-runtime` (extrai as funções verbatim do
  template: semear uma vez, `.new`, migração, edição chega ao estado efetivo
  sem rebuild, reserva nominal, `bash -n`, symlink ignorado, V4 intacta com o
  opt-in desligado) e `nxbootstrap-owner-runtime-generator` (mutantes:
  hook em geração/required/prepare recusados, literal só `"1"`, exige
  `generation_runtime`, combinação com `sdl_provider=system`).
  `test-generator.sh` sobe os tetos 3260→3300 / 3450→3490 pelas linhas novas.
- Consumidores: nxgenerator precisa repinar o hash do template e emitir
  `defaults/port-env.sh` + `adapter-env.sh` selado quando o port opt-in
  (própria versão/branch); nxrelease ganha os gates de owner (própria
  versão/branch). NXExtract e NXSplash não mudam: o owner só é semeado depois
  do gate dos dados e antes do adapter/jogo.

# 0.7.8 (2026-08-31, biblioteca privada do helper NXExtract)

- Permite que uma biblioteca compartilhada usada por helper do NXExtract
  permaneça abaixo de `nxextract/` sem fingir que o ELF é um helper
  executável: o papel continua sendo `private-library`, o modo é exatamente
  `0644` e o arquivo precisa ser descendente estrito de um
  `private_library_paths` declarado.
- A exceção só existe com NXExtract `yes`/`auto`. Modo `no`, owner data,
  caminho fora da raiz declarada, caminho igual à raiz, modo executável e os
  quatro paths canônicos do core continuam falhando fechado.
- A closure viva, o store imutável, o heal transacional, a limpeza de
  temporários e a detecção de extras passam a reconhecer o mesmo membro. Os
  papéis existentes não mudam: `nxextract-helper` ELF continua exigindo
  `0755`, e biblioteca privada comum fora de `nxextract/` conserva o contrato
  anterior `0644`/`0755`.

# 0.7.7 (2026-08-31, status do filho é autoridade de promoção)

- Corrige uma falha da máquina de gerações: um adapter podia publicar health
  `ready` e `VIDEO OK/non-black` e depois sair com status não zero; como a
  autoridade do receipt era avaliada antes do fallback de saída limpa, a
  geração pendente ainda era promovida e recebia `NXU0006`.
- A autoridade de health agora exige também a fronteira `runtime EXIT`
  persistida e status 0 obtido pelo `wait` do filho exato. Saída não zero
  preserva o status, conta falha pré-health e nunca promove.
- `test-generation-v2.sh` reproduz o receipt e vídeo exatos com child status 1
  e prova estado pendente, contador, `NXU0005` e ausência de `NXU0006`.

# 0.7.6 (2026-08-30, primeira geração nunca se autobloqueia)

- Corrige o caso de campo do Nameless Cat 1.2.0: o jogo executava e saía com
  status 0, mas a ausência do receipt do adapter somava três falhas; como uma
  instalação fresca não tem `previous_healthy`, o quarto boot recusava a única
  geração completa com `NXU0009`.
- Uma saída 0 do filho exato, persistida na fronteira run-bound `runtime EXIT`,
  promove como fallback somente a primeira geração sem âncora. Se
  `video_proof=required`, `OK/non-black` final continua obrigatório.
- Geração A/B com rollback disponível mantém a regra forte: somente receipt
  exato do adapter promove uma atualização pendente.
- Estado fresco antigo já preso em três falhas recebe `NXU0015`, tenta a única
  geração autenticada e se recupera sem fabricar `previous_healthy`.
- Três aberturas sucessivas sem emissor, recuperação do estado de campo e arquivo
  extra do dono são provados no gate dirigido `test-v3-generations.sh`.

# 0.7.5 (2026-08-30, boot fast path: fim do custo O(N) por abertura)

- Bug de campo (Tearscape, closure de 197 componentes/157 MB em cartao
  FAT/exFAT): cada abertura repetia a validacao/heal da closure inteira em
  shell — probes de chmodless POR ARQUIVO (touch/chmod/ls/rm), sha256sum e
  grep POR MEMBRO, multiplas passadas — somando ~8.400 subprocessos e 3-7
  minutos de TELA PRETA silenciosa antes do NXExtract. O conteudo em si custa
  poucos segundos; o custo era a cerimonia O(N).
- V4-BOOT-01: o probe de filesystem chmodless roda no maximo UMA vez por
  execucao e fica em memoria; em midia chmodless o mode-check vira builtins
  (0644 aceito; 0755 exige executavel na visao montada) sem um `ls` por
  membro, e nenhum chmod por arquivo tenta restaurar bits que o filesystem
  ignora.
- V4-BOOT-01: fast path O(1) para o boot saudavel: quando o estado persistido
  (ou a materializacao recem-concluida, ou a primeira tentativa de uma
  instalacao nova sem estado) aponta para a geracao comprometida do proprio
  launcher, valida-se apenas o plano de controle — formato, commit,
  identity.json content-addressed, nxport vivo pinado pela identidade,
  metadados da closure e entry point real/executavel — em numero constante de
  subprocessos. Sem varrer components.v2, sem heal, sem chmod/rm/ls/sha por
  membro. O health receipt run-bound continua decidindo a promocao (NXU0006)
  e qualquer falha manda o proximo boot para o caminho profundo.
- Troca de contrato documentada: o boot saudavel O(1) NAO relê o corpo do
  estore imutavel; um membro do estore rasgado com arvore viva integra so
  aparece no proximo evento profundo (update, rollback, run sem health — que
  arma o marcador one-shot `deep-verify-next` — ou o proprio marcador). O
  entry point vivo e ancorado no hash pinado pela identidade a cada boot, e
  nxport vivo idem; engine trocada por update/cirurgia nunca faz fast boot.
- Idem para a normalizacao de modos POSIX do PortMaster (chmod -R 777): ela
  acontece no proximo evento profundo, nao mais em toda abertura saudavel.
- V4-BOOT-02: o caminho profundo (recuperacao verdadeira: estado/identidade
  divergente, instalacao interrompida, adulteracao) verifica a closure com
  UMA chamada de sha256sum em lote por passada, indexa components.sha256 em
  memoria e reaproveita o lote para o heal — nunca um subprocesso por membro.
- V4-BOOT-03: o caminho profundo nunca e mais uma tela preta muda: o inicio e
  logado, publicado como NXU0014 e ecoado no console quando ha tty. A UI
  grafica canonica do NXExtract em modo "verificando/reparando" fica
  registrada como continuacao (exige modo novo no contrato visual selado do
  NXExtract; nao entra como efeito colateral desta versao).
- Gate novo `tests/test-boot-fastpath.sh`: fixture chmodless com 4205
  membros; primeira e segunda abertura rapidas (< 1 s ate o preflight no
  host) com contagem de subprocessos CONSTANTE e identica a de uma closure de
  8 membros; recuperacao adulterada obrigada a usar sha em lote (< 40 forks
  para 4205 membros) com NXU0014 visivel e heal comprovado; commit ausente e
  entry point symlink/adulterado continuam fail-closed; sem `stat` externo.
- Regra permanente derivada (backlog V4 corrigido): boot saudavel nunca pode
  ter custo proporcional a quantidade de arquivos, e operacao demorada nunca
  pode ser tela preta silenciosa. A explicacao anterior de que os minutos
  eram custo inevitavel do SHA estava errada e foi removida.

# 0.7.4 (2026-08-30, provider SDL de sistema + falha fatal de vídeo)

- O launcher gerado recusa providers privados SDL1/SDL2 antes de executar o
  jogo, independentemente do manifesto. SDL3 não é confundido com SDL1/2; sua
  exceção privada continua pertencendo ao contrato formal e ao hash verificado
  pelo NXRelease.
- O novo opt-in fechado `sdl_provider: "system"` preserva a SDL do firmware à
  frente dos overlays PortMaster, remove do `LD_LIBRARY_PATH` herdado qualquer
  diretório resolvido dentro do port e registra provider, ordem e search path.
  Não define `SDL_VIDEODRIVER`. Campo ausente conserva a ordem e o caminho
  herdado legados; qualquer outro valor falha no gerador.
- A revisão sucessora fecha os caminhos que contornavam essa ordem: no opt-in
  `system`, `BIN_PRELOAD`, `LD_PRELOAD` e `SDL_DYNAMIC_API` herdados são capturados e
  removidos somente com builtins no primeiro ponto alcançável do script, antes
  do primeiro subprocess/helper executado. O Bash já foi iniciado pelo dynamic
  loader nesse ponto e um preload herdado não pode ser descarregado
  retroativamente; a garantia é para todos os subprocessos posteriores do
  framework.
- `port-env.sh` conserva a semântica top-level; os três valores finais são
  copiados imediatamente depois do source, recebem `export -n` e são removidos.
  Comandos que o próprio hook executa enquanto é sourced continuam
  responsabilidade do adapter. Como o de-export funciona sobre readonly, um
  readonly do hook é recusado com o ambiente externo limpo e cleanup normal.
  `BIN_PRELOAD`, os
  preloads aceitos e `SDL_DYNAMIC_API` só são aplicados dentro do subshell que
  faz `exec` do filho exato. Se readonly herdado impedir o primeiro `unset`, a
  abertura falha por builtins, grava o `launcher-error.<pid>.log` 0600 e não
  inicia helper contaminado.
- Cada entrada explícita de `BIN_PRELOAD`, `LD_PRELOAD` e `SDL_DYNAMIC_API` é
  resolvida contra o search path final; entrada não resolvida falha fechado.
  Alvos locais com nome SDL1/2 ou fingerprint de core/add-on são recusados;
  `$ORIGIN` usa o diretório físico do executável. Alvo canônico fora do
  `NXBOOTSTRAP_PHYSICAL_GAMEDIR` preserva a autoridade externa/firmware,
  inclusive `SDL_DYNAMIC_API` e arquivo com fingerprint SDL, sempre child-only.
  Os pares de add-on ficam alinhados ao NXRelease: image, mixer, ttf, net, gfx,
  gpu, sound, rtf e fontcache. O gate comum de nomes cobre os mesmos namespaces.
- No opt-in `system`, o hook não pode substituir `BIN`, interpreter, game
  loader ou library route. O runtime prova essa rota e os overrides explícitos;
  a inspeção profunda package-wide de ELF/SONAME/símbolos/proveniência continua
  pertencendo ao NXRelease. Sem opt-in, ordem, hook e ambiente legados não
  recebem a quarentena nem a imutabilidade nova.
- Todo launcher exporta um caminho privado e único
  `NXBOOTSTRAP_VIDEO_FILE` para o recibo run-bound de vídeo. `BLACK` e
  `DEAD-CONTEXT` conclusivos, autenticados por schema, tuple, owner, modo 0600,
  um hardlink e inode aberto, encerram o filho exato com TERM/KILL, emitem
  `NXR0004`, invalidam health e retornam 72; áudio ou PID vivo não mascaram a
  falha. `OK/non-black` continua a execução.
- O opt-in fechado `video_proof: "required"` exporta
  `NXBOOTSTRAP_VIDEO_REQUIRED=1` e impede promoção por health enquanto o recibo
  `OK/non-black` da mesma execução não existir. A promoção relê o inode final:
  `OK` observado e depois removido, malformado ou revogado não permanece válido.
  Campo ausente conserva a regra histórica de promoção.
- Os seis campos da tuple de health ficam readonly antes do `port-env.sh`; uma
  tentativa de assign/unset não redireciona nem altera o receipt. Cada KILL do
  filho reexecuta `nxbootstrap_child_alive` imediatamente antes do sinal,
  vinculando novamente PID e starttime. NXSplash, NXExtract e interfaces
  visuais não mudam.

# 0.7.3 (2026-08-30, integração V4 aberta: runtime-data + rollback real-crash)

- Composição das duas linhas paralelas da 0.7.0: o papel `runtime-data` da
  0.7.2 e o rollback fail-closed após crash real da 0.7.1. Nenhum dos dois
  comportamentos muda; o schema `phase-result` passa a pinar `0.7.3`.
- NXSplash, NXExtract e o fluxo visual permanecem byte a byte.

# 0.7.2 (2026-08-30, dados de runtime gerenciado na geração V2)

- Adiciona o papel `runtime-data` à closure transacional do schema v3 para
  assemblies e metadados imutáveis que não são ELF nem hooks.
- Exige modo `0644` e path estritamente abaixo de um
  `private_library_paths`; owner data e saves continuam proibidos.
- O parser do launcher reconhece o novo papel sem alterar telas, ordem de
  abertura ou fluxo físico da 0.7.0.

# 0.7.1 (2026-08-30, V4-ROLLBACK-REAL-CRASH)

- Faz o crash-loop falhar fechado depois de três falhas pré-health da mesma
  pendência quando `previous_healthy` está ausente, incompleta ou incompatível;
  antes, essa condição apenas pulava o rollback e permitia tentativas ilimitadas
  dos mesmos bytes defeituosos. Falha ao persistir o rollback continua recusando
  o launch com `NXU0011`; âncora inválida usa `NXU0009` e orienta `nxdoctor`.
- Adiciona o gate real `test-v4-rollback-real-crash.sh`: `SIGABRT` 134,
  `SIGTERM` 143 e saída 42 consecutivos, rollback A/B na abertura seguinte,
  concorrência sem segundo runtime e negativos de `previous_healthy` e
  persistência somente-leitura.
- O gate vincula toda decisão a PID + `/proc/<pid>/stat` starttime + run-id,
  usa deadlines monotônicas derivadas de `/proc/uptime`, mata somente identidades
  exatas e reabre o launcher depois de receipts stale, late e de PID reciclado.
  A concorrência mantém A vivo enquanto publica atomicamente B e prova que o
  launcher B não cria um segundo runtime. Receipts por tentativa registram status
  do launcher/filho, projeção integral do estado, hashes e higiene de
  locks/staging/temporários; dados do dono são verificados por manifesto SHA-256.
- NXSplash, NXExtract, launcher congelado distribuído e demais interfaces
  visuais permanecem byte a byte; a alteração comportamental está restrita ao
  template gerador no caso fail-closed acima.

# 0.7.0 (2026-08-29, V4-REPACK-01: seed visível e `.nxruntime` como cache)

**A âncora do launcher não sobrevivia a uma instalação real do PortMaster.** Ele
reescreve a linha 2 do launcher instalado com o nome do zip de origem; a
comparação byte a byte então recusava o port com `NXU0012 installed launcher
does not match the runtime seed closure` — no único caminho de instalação que
os usuários usam. A prova física anterior passava só porque eu instalava o ZIP
à mão. Agora a comparação instalado × closure é feita sobre a forma canônica
(essa linha normalizada dos dois lados), sem mexer em nenhum hash gravado.
Provado no aparelho: `harbourmaster install` real reescreveu o marcador para
`# PORTMASTER: v4seed-fresh.zip, V4 Seed.sh`, o launcher materializou a cache
do seed (`NXU0012: runtime cache rebuilt from seed …`) e o NXDoctor leu a
geração como `complete`, 13/13 verificados por hash. Regressão no host simula a
reescrita; um byte alterado em qualquer outro lugar continua sendo recusa.


O **orçamento de 4 MiB do log de runtime virou um limite de verdade**. A
rotação por execução só limitava o log ENTRE execuções: dentro de uma, o jogo
herda o descritor e uma engine tagarela escreve por horas, até encher o cartão
— e cartão cheio, pela própria medição de ENOSPC desta versão, para o port. Ao
fim da execução o log é cortado de volta ao teto **guardando a cauda**, porque
é no fim que o defeito está, com uma linha dizendo quantos bytes saíram da
cabeça. Nada é feito a jogo em execução e nenhum save é limitado; a saída
posterior é reaberta sobre o arquivo cortado, nunca sobre um inode órfão.
Verificado por mutação: não cortar, ou guardar a cabeça, reprova o gate.

O corte de texto passou a **descartar a primeira linha parcial**, como o de
JSONL já fazia. O log de runtime não é lido só por gente: o support bundle
extrai registros `NXEVENT {...}` dele, e um corte caindo dentro de um deixa uma
linha que ainda começa com o marcador e não é mais JSON válido. Provado ligando
o escritor ao leitor de verdade — o gate alimenta o `nx-support-bundle.py` com
o log que o launcher realmente escreveu.

O **mesmo defeito existia em `events.jsonl`** e foi varrido junto: o orçamento
de 1 MiB também era só um número num arquivo, e quem escreve ali durante a
execução é o adapter de runtime, dentro do processo do jogo — ninguém que possa
se limitar sozinho. A diferença é que um corte por bytes cai no meio de uma
linha, o que é inofensivo num log de texto e **fatal em JSONL**, porque o
support bundle lê linha a linha: a primeira linha parcial é descartada e o
próprio registro do corte é um evento válido (`NXU0014`) com os bytes
descartados. Verificado por mutação: não limitar deixa 1.350.456 bytes, e
manter a linha parcial produz JSONL inválido.


Um membro que colide por **caixa** com outro é recusado no cabeçalho do seed.
`/roms` é exFAT: `files/lib/Game.so` e `files/lib/game.so` são dois registros
distintos e um arquivo só no cartão. Sem a recusa, um sobrescrevia o outro em
silêncio e o defeito reaparecia depois como divergência de hash num membro
inocente — ou desaparecia, quando os dois tinham o mesmo conteúdo. Uma passagem
só sobre a lista de caminhos, com diagnóstico próprio em vez do genérico
`NXU0009: closure is absent`. Verificado por mutação.

A recusa existe nos **dois lados**, de propósito e por motivos diferentes: o
launcher recusa porque um seed pode ter sido adulterado; o **gerador** recusa
porque um seed que nós mesmos construímos jamais deveria chegar ao cartão nesse
estado e ser descoberto pelo jogador. Um par que difere só na caixa em qualquer
segmento do caminho — inclusive num diretório — reprova a construção.


- Cada port de schema v3 passa a emitir `<port-id>/nxruntime-<generation>.nxb`:
  arquivo **regular, visível, 0644, determinístico e content-addressed** com a
  closure imutável completa (metadados da geração e todos os `files/`). O
  `commit` nunca é transportado; ele continua sendo escrito por último, no
  aparelho, como recibo transacional.
- `.nxruntime` vira **cache local reconstruível**. Um ZIP pessoal que descarta
  todos os dotfiles/dotdirs — comportamento normal de compactador gráfico —
  volta a instalar e abrir, sem enfraquecer o selo da V3.
- Materialização transacional no primeiro launch: staging privado
  `.nxruntime/staging.<sessão>/<generation>`, verificação de path/tipo/modo/
  tamanho/SHA-256 membro a membro, autenticação pela **mesma** função fechada
  usada por uma geração instalada, `commit` por último e promoção por um único
  `mv` atômico.
- Raiz de confiança e modelo de ameaça escritos antes do código em
  `BUNDLE-TRUST-V4.md`. A âncora é o id de geração compilado no launcher;
  `sha256(identity.json)` tem de igualá-lo. O launcher instalado é comparado ao
  `files/launcher/<nome>` do seed. Não há self-hash circular e não se afirma
  resistência contra administrador local.
- Geração existente **nunca** é reparada no lugar e `commit` ausente nunca é
  fabricado: a reconstrução só roda quando o diretório da geração está ausente.
- V3-STORAGE-01: preflight de espaço antes da materialização, com recibo
  `STORAGE: bundle=… active=… pending=… previous=… required=… available=…` e
  recusa `NXU0013` sem destruir a geração ativa. A recusa é **testada com
  espaço injetado de verdade** (tmpfs pequeno, não valor simulado), com
  controle positivo no mesmo tamanho suficiente, e prova três coisas: instalar
  sem espaço recusa antes de escrever; um update sem espaço é recusado e a
  geração que o dono já usava continua **abrindo o jogo**, voltando a
  completar-se quando há espaço; e uma promoção que não pode ser publicada não
  deixa geração parcial nem staging. O store somente-leitura é injetado por
  bind mount e não por `chmod`, porque a suíte roda root-mapped no próprio
  namespace e root atravessaria os bits de permissão — a injeção por `chmod`
  não provaria nada. Novo código `NXU0012` para os
  diagnósticos do seed.
- `namespace-watchdog.py` passa a falhar fechado quando qualquer `setrlimit`
  exigido não pode ser estabelecido (V3-HARDENING-01).
- **O cabeçalho do seed é relido com `head` limitado, não com `sed` sobre o
  arquivo inteiro.** O seed carrega todo o payload do port — centenas de MB num
  jogo real — e reparsear o cabeçalho com `sed -n "5,Np"` fazia o launcher
  puxar tudo isso do cartão a cada reconstrução, para nada. Medido num seed de
  300 MB: 91 ms contra 1 ms, e isso já em cache; num cartão frio é a leitura
  inteira. Além do custo, `sed` sobre binário com NUL embutido é
  implementation-defined numa CFW BusyBox. O gate trava isso estaticamente.
- Também documenta `NXU0012` (diagnósticos do seed) e `NXU0013` (recusa por
  espaço) na tabela de códigos.
- Novo gate `tests/test-v4-bundle.sh`: forma e determinismo do seed, instalação
  por rezip pessoal real (Info-ZIP, 7-Zip e um ZIP DOS/stored/embaralhado sem
  modos Unix), remoção total da cache com reconstrução, update oficial sobre
  ZIP pessoal preservando `gamedata` byte a byte, e negativos de seed ausente/
  truncado/adulterado/cabeçalho reescrito/geração estrangeira/launcher fora da
  closure/traversal/path absoluto/duplicata/membro extra/membro ausente/
  contagem errada/offset deslocado/modo inválido/FIFO/symlink e staging
  interrompido.

# 0.6.37 (2026-08-28, normalização POSIX autenticada)

- Corrige instalações limpas feitas pelo PortMaster em mídia POSIX, onde o
  `chmod -R 777` posterior à extração tornava uma generation-v2 íntegra
  aparentemente truncada ou stale no primeiro boot.
- A normalização acontece em duas passagens fechadas: primeiro valida
  integralmente identidade, paths, tipos, SHA-256 e closure sem alterar modo;
  somente então restaura no store os modos `0644`/`0755` autenticados e repete
  a validação completa exigindo igualdade. A cura da árvore live continua
  posterior a essa fronteira.
- Preserva a fixture FAT/exFAT/FUSE sem semântica de `chmod`: modos sintéticos
  continuam aceitos apenas depois do probe chmodless existente.
- `test-generation-v2.sh` reproduz também o `chmod -R 777` real do PortMaster
  em filesystem POSIX e exige store/live restaurados aos modos do manifesto.

# 0.6.36 (2026-08-28, generation runtime v2)

- Adiciona o schema nxport v3 opt-in com `generation_runtime` ordenado e exato:
  um executável, bibliotecas somente sob raízes privadas, hooks declarados em
  `0644`/`0755` e NXSplash canônica automática; schemas 1/2 continuam no fluxo
  histórico sem ganhar runtime closure silenciosamente.
- A identidade v2 liga launcher, nxport, splash, ELF, bibliotecas e hooks por
  papel/path/modo/SHA-256. Os bytes ficam em `files/runtime`; gamedata, saves,
  GPTK e settings são recusados.
- Fecha também a closure NXExtract da mesma geração: receita, engine, runner,
  runtime-env e UI têm papéis/path/modos canônicos obrigatórios quando a
  extração está ativa; todo helper/módulo e spec/dado imutável adicional entra
  explicitamente como `nxextract-helper` ou `nxextract-spec`, sem nomes ou
  quantidade codificados para um jogo. Arquivo não declarado sob `nxextract/`
  falha antes da extração.
- A cura de instalação é all-or-nothing: fonte e alvo precisam ser regulares,
  não-symlink e hash-exatos; todos os membros divergentes são stageados antes da
  primeira rename e falha nessa fase deixa a raiz live intacta. Depois das
  renames, todos os membros são revalidados antes do preflight. Rollback para
  outra v2 reinicia pelo launcher curado para não continuar com configuração
  baked de outra geração. Um relançamento remove somente staging `nxheal` com
  formato reservado e PID criador morto dentro da árvore NXExtract gerida;
  paths do dono fora dela não são limpos. Symlink, nome malformado ou criador
  vivo falha fechado.
- Gerações v1 permanecem legíveis como `legacy-control-only`, mas nunca são
  âncora `previous_healthy` de uma pendência v2. V2 sem `sha256sum` falha fechado.
- Em mídia FAT/exFAT/FUSE, modos sintéticos só são aceitos após probe real de
  filesystem chmodless no mesmo diretório; hash, arquivo real, anti-symlink e
  executabilidade não são relaxados.
- Corrige a promoção: `NXU0006` é emitido somente após persistir com sucesso o
  estado `pending -> active`; falha de write/rename produz `NXU0011`, status 70
  e nenhuma promoção.
- `tests/test-generation-v2.sh` cobre A/B, update NXExtract deliberadamente
  híbrido, helper importado/spec, arquivo extra, falha tardia de staging sem
  publicação parcial, torn, symlink, v1/v2, ausência de SHA, falha de state e
  mídia 0777/chmod ignorado no launcher real gerado.

# 0.6.35 (2026-08-27, reconciliação e suporte sanitizado)

- Reconcilia a promoção opt-in do NXExtract 1.2.21 da 0.6.34 com o ownership
  fail-closed do lock sem `flock` desenvolvido na linha 0.6.33; nenhum dos dois
  caminhos é perdido e ports já aprovados continuam presos aos seus bytes.
- Completa o bump do NXExtract 1.2.21 no registro canônico de motores, com os
  hashes reais do engine, runner e runtime-env; gerador e release voltam a
  concordar e a entrada 1.2.20 permanece suportada para ports já pinados.
- O bloco final de suporte deixa de pedir logs crus e orienta o envio exclusivo
  do bundle sanitizado produzido pelo `nxobs`; `log.txt`, históricos,
  `nxextract.log` e `events.jsonl` permanecem entradas internas.
- A NXSplash de cinco segundos, a identidade visual do NXExtract e o fluxo
  nativo do jogo permanecem inalterados.

# 0.6.34 (2026-08-27, NXExtract 1.2.21 opt-in)

- Promove somente novos manifests para o NXExtract 1.2.21 e seu selo forte de
  conteúdo/reuso fechado; ports existentes mantêm o pin anterior.
- Preserva byte a byte o template do launcher, a NXSplash 0.1.2, sua duração e
  identidade visual e o fluxo nativo já aprovado.

# 0.6.32 (2026-08-27, V3-UPDATE/ROLLBACK closure)

- Novas gerações usam o SHA-256 canônico completo (64 hex), ligado ao manifesto
  do port, versão e fontes exatas do gerador/template e ao artefato NXSplash da
  ABI. Assim, mudar o framework sem mudar o manifesto não colide nem sobrescreve
  uma geração já comitada. Gerações antigas de 32 hex continuam legíveis.
- Uma geração comitada é reutilizada somente quando todos os bytes esperados
  são idênticos; colisão de identidade falha fechada. A lista de componentes
  aceita apenas o launcher canônico e `nxport.json`, sem symlink, duplicata ou
  path traversal, e o self-heal restaura seus modos 0755/0644 explicitamente.
- Uma geração nova permanece em `pending`; `active` só muda depois de um
  receipt de saúde estruturado, exclusivo desta execução e da mesma geração.
- O receipt vive no diretório privado de runtime validado, nunca no filesystem
  do jogo. Owner, modo 0600, inode, número de links e JSON canônico são
  conferidos pelo descritor aberto antes da promoção.
- Strings soltas no log (`NXHEALTH`, `READY` ou `Entering main loop`), PID vivo
  e exit 0 não promovem mais uma atualização.
- Três falhas consecutivas ficam ligadas à geração pendente e fazem o próximo
  boot voltar à última geração saudável, sem penalizar uma geração ativa por
  falha não relacionada.
- Um launcher de geração nova realmente instalado substitui uma tentativa
  pendente antiga; a pendência stale nunca impede a correção posterior, e a
  geração ativa continua como âncora até o novo receipt de saúde.
- Correção pré-release: a varredura de processos obsoletos roda somente DEPOIS
  de o lock estável por-port ser nosso — antes, o sweep matava um jogo saudável
  da geração corrente antes de o `flock -n` recusar a duplicata. Um processo do
  mesmo diretório cuja `NXBOOTSTRAP_HEALTH_GENERATION` é a geração selecionada
  é instância viva: emite `NXR0003`, nunca é morto e a segunda abertura sai
  com erro em vez de duelar pela GPU.

# 0.6.31a (2026-08-27, V3-UPDATE-01 item 7)

Aditivo sobre 0.6.31; toda geração 0.6.31 continua válida.

- **NXR#### deixou de ser engolido.** Os eventos de RUNTIME (single-instance/
  reaper) `NXR0001` (processo obsoleto varrido) e `NXR0002` (instância anterior
  não terminou) eram passados ao helper `nxbootstrap_update_event`, que só aceita
  `NXU####` e retornava 2 SEM emitir nada — o evento sumia. Novo helper
  `nxbootstrap_runtime_event` (família `NXR####`, phase `runtime`, prefixo
  `RUNTIME`) emite a linha e o JSONL; NXR0001/0002 agora aparecem.
- **Self-heal atômico e anti-symlink.** (1) O componente FONTE na geração precisa
  ser arquivo real, nunca symlink (um membro symlink poderia apontar para fora do
  port). (2) Um alvo que virou symlink é CURADO (trocado pelo arquivo real), nunca
  lido/escrito ATRAVÉS do link. (3) A cura só renomeia depois de VERIFICAR que o
  hash do temporário bate com o esperado — a cadeia antiga `cat && chmod || chmod`
  podia dar `mv` num temporário TRUNCADO quando o `cat` falhava, instalando um
  arquivo cortado sobre um componente bom.

Nota histórica: o redesenho citado abaixo foi concluído na versão 0.6.32; a
prova física permanece separada dos gates host desta versão.

# 0.6.31 (2026-08-26, onda v3)

V3-UPDATE/ROLLBACK/CLOCK/OBS/RUNTIME — aditivo sobre 0.6.30; toda geração
0.6.30 do launcher continua válida.

- Gerações imutáveis com auto-reparo: o launcher materializa a geração como
  árvore imutável e reconstrói o que faltar antes de abrir, sem sobrescrever a
  última configuração válida.
- Rollback com prova de saúde: uma geração nova só vira a corrente depois de um
  health proof; se falhar, volta à anterior íntegra.
- `events.jsonl`: trilha de eventos append-only do run (materialização,
  rollback, saída), uma linha por evento.
- Varredura de processos no encerramento pelo funil único de EXIT.
- `templates/launcher.sh.in`, `tools/generate-port.py` e o gate novo
  `tests/test-v3-generations.sh` cobrem o fluxo.
- Materialização anti-symlink sem TOCTOU (auditoria V3, ponto 6):
  `nxbootstrap_atomic_install` deixa de criar o temp vazio e reabrir com
  `cat > tmp` (janela em que um symlink plantado era seguido); passa a criar E
  preencher o temp num único open exclusivo `( set -C; cat "$src" > "$tmp" )`
  (O_EXCL recusa qualquer path pré-existente, symlink pendurado incluído). O
  gate `tests/test-owner-materialize-atomic.sh` ganha a colisão real no temp
  path (via `$RANDOM` semeado) e a guarda estrutural anti-reopen.
- Entrada registrada em atraso: o VERSION já era 0.6.31 desde o commit 822db39;
  o gate `component-versions` passa a impedir esse tipo de defasagem.
- Raiz lógica e raiz física separadas (V3-UPDATE-01): o `pwd -P` deixava
  `GAMEDIR` só físico e o guest recebia `/storage/roms/...` no lugar de
  `/roms/...`. Agora o launcher mantém `NXBOOTSTRAP_LOGICAL_GAMEDIR` (a raiz que
  o frontend invocou -- identidade lexical entregue ao guest via
  `NXCOMPAT_GAME_DIR`) e `NXBOOTSTRAP_PHYSICAL_GAMEDIR` (`pwd -P`, para
  containment/I/O do framework, também exposto em `NXCOMPAT_GAME_DIR_PHYSICAL`).
  Duas árvores distintas com o mesmo port (frontend vs launcher invocado)
  emitem `NXU0007` e preferem a árvore do launcher realmente invocado (nunca
  uma cópia stale). Recibo sanitizado loga lógico/físico e a razão da escolha.
  Gate novo `tests/test-gamedir-logical-physical.sh` (symlink, árvores
  distintas, alias com espaços); `test-launcher-behavior` deixa de consagrar o
  swap físico.

# 0.6.30 (2026-08-23, onda v2)

- Lock de instancia: sonda `command -v flock`; em BusyBox sem o applet o
  launcher caia em 127 e culpava uma "outra instancia" fantasma para sempre.
  Fallback por mkdir com dono carimbado (receita da lib 0.5.1), dono morto
  recuperado, liberacao no funil unico de EXIT.
- Raizes PortMaster: tabela COMPLETA (espelho da biblioteca) + a pasta do
  proprio launcher; Miyoo/spruce, RetroDECK e cartoes com `Ports` maiusculo
  deixam de cair no fallback sem controles. Gate estatico trava a igualdade.
- `SDL_APP_ID`/`SDL_VIDEO_WAYLAND_WMCLASS` = id do port (valor herdado
  vence): o helper de fullscreen de firmwares Wayland (ROCKNIX/sway) passa a
  achar a janela em vez de tentar para sempre.
- Log: duas geracoes anteriores (`log.prev.txt`, `log.prev2.txt`) e bloco
  `== SUPPORT ==` no fim de todo run -- port, versoes, cfw, terminal-result
  e a lista exata de arquivos a enviar. O log.txt vira o pacote de suporte
  de um arquivo so'.
- NXExtract canonico: 1.2.18.

# Changelog do nxbootstrap

## 0.6.29 — 2026-08-20

- **Regressão de campo corrigida (ArkOS/dArkOS).** O glob `libc.so*` da
  closure por papel casava `/usr/lib/arm-linux-gnueabihf/libc.so`, que no
  Debian multiarch é um **script do GNU ld**, não uma biblioteca. O launcher
  lia o script como ELF inválido e **descartava a raiz ARMHF inteira**:

  ```
  EXECUTION CANDIDATE REJECTED: role=splash root=/usr/lib/arm-linux-gnueabihf reason=invalid-elf
  ERROR: execution role splash has no coherent armv7 closure
  ```

  O preflight morria antes de o NXExtract desenhar qualquer coisa — o ZIP
  1.0.8 do Titan Souls não abria no aparelho, enquanto o 1.0.7 (sem papéis de
  execução) chegava ao título normalmente.
- `nxbootstrap_elf_identity` passa a distinguir três casos: ELF identificado,
  **não é ELF** e ELF ilegível/truncado. Arquivo que não é ELF é **ignorado**
  (com linha `EXECUTION CANDIDATE SKIPPED ... reason=not-an-elf`) e não conta
  como raiz vista; ELF de ABI errada continua derrubando a raiz inteira, que é
  a propriedade que realmente protege o closure isolado.
- Fixture ArkOS/Debian no gate mixed-ABI, com o script real medido no
  aparelho, a `libc.so.6` ELF32 ARM, o link `/lib/arm-linux-gnueabihf` para a
  árvore de `/usr` e a prova de que um ELF AArch64 no meio continua recusado.

## 0.6.28 — 2026-08-20

- Adiciona `options`: contrato **aditivo** e opt-in de opções declarativas do
  launcher (id, valores permitidos, padrão, variável de ambiente e rótulo
  opcional). Manifesto sem `options` gera o launcher anterior byte a byte,
  incluindo os ports já publicados.
- `language` permanece intocado, com forma e semântica próprias.
- Valor, id e variável passam por regex fechada mais uma lista de nomes
  reservados do framework; valor fora da lista — editado à mão ou herdado do
  ambiente — volta ao padrão declarado em vez de chegar cru ao shell.
- Cada opção é reafirmada depois do hook mutável: adapters consomem, nunca
  redefinem. Detalhes em `OPTIONS.md`.

## 0.6.27 — 2026-08-20

- Adiciona `execution_roles` como contrato estritamente opt-in para declarar
  arquitetura, executável, política de executor, `PT_INTERP` e closure de
  extractor, NXSplash, jogo e helpers adicionais.
- Valida classe ELF e `e_machine` tanto do executável quanto do loader
  alternativo. Execução nativa continua preferencial; candidato ausente,
  inválido ou de ABI errada falha fechado com receipt por papel.
- Isola `LD_LIBRARY_PATH` por ABI no caminho novo. O closure ARMHF comprovado
  pelo perfil Spruce usa chroot/`usr/lib32` e exclui o `muOS/usr/lib` AArch64;
  bibliotecas privadas só entram depois do NXExtract e também são verificadas.
- Adota NXExtract 1.2.13. NXSplash permanece em 0.1.2 com os mesmos bytes,
  duração e identidade visual. Manifestos sem `execution_roles` continuam no
  render legado, sem migração automática.

## 0.6.26 — 2026-08-19 🚨 CORREÇÃO DE COMPATIBILIDADE DE CAMPO

- **Validador do terminal-result compatível nos DOIS sentidos** (dentro do
  schema_version 1). Incidente de campo (dois testers muOS): updates que não
  substituem o `.sh` deixam launcher ANTIGO + engine NOVO, e o validador
  estrito matava a fase nxextract (`invalid terminal result object` /
  `unknown terminal result schema`). Agora: membros extras desconhecidos são
  ignorados; membros nascidos depois do schema (ui) validam só se presentes;
  `nxextract_version` valida por FORMATO, não valor exato. O gate duro segue
  sendo `schema` + `schema_version` — breaking de verdade (v2) continua
  rejeitado. **REGRA DE OURO: dentro de um schema_version, lista de membros e
  versão de engine NUNCA quebram launcher publicado.** Selado por
  `tests/test-terminal-compat.sh` com os validadores LEGADOS 0.6.16/0.6.21
  capturados byte-a-byte do git reproduzindo o erro do campo. Teto 1020→1040.
- **Gate `test-muos-device-faithful.sh`** (matrix `muos-device-faithful`):
  launcher INTEIRO rodando em sandbox com os artefatos REAIS do muOS 2601.1
  (/opt/muos + PortMaster/control.txt/device_info/funcs reais + pad RG40XX-H).
  0.6.26 + engine futuro (membro novo) → instala e abre; launcher 0.6.21
  regenerado do commit de época → reproduz o erro do campo.
- **Investigação do híbrido encerrada**: HarbourMaster real TROCA o `.sh` no
  update (provado por sha; única mutação legítima = inserir a linha
  `# PORTMASTER:` quando falta, agora tolerada com exatidão no ciclo, que
  passou a byte-verificar launchers). Archive Manager do muOS nem extrai zip
  comum e derruba arquivos soltos da raiz no `.muxzip` → híbrido do campo é
  atualização MANUAL; releases passam a instruir clean-install.


## 0.6.25 — 2026-08-19

- **`--library-path` do loader alternativo com os diretórios REAIS das
  imagens oficiais.** Auditando o spruceOS v4.3.4 de verdade (squashfs):
  o muOS reduzido guarda SDL2/EGL/GLES ARMHF em `usr/lib` (não só `lib32`) e
  o chroot 32-bit monta em `/mnt/SDCARD/Persistent/.32bit_chroot` — nenhum
  dos dois estava na lista, o que seria a próxima camada de campo (SDL2 não
  encontrado). Os mounts entram na lista de libs E como candidatos de
  interpretador; a matriz (perfil spruce) prova que o `--library-path`
  entregue carrega o `usr/lib` do muOS. Firmware comum segue byte-idêntico.
  Teto 1015→1020.


## 0.6.24 — 2026-08-19

- **`NXBOOTSTRAP_EXE`: fonte canônica de auto-localização do guest.** Sob o
  loader dinâmico alternativo (CFW 64-bit-only), o `/proc/self/exe` do jogo
  aponta pro **ld.so** — um loader que se localizava por ele perdia a própria
  pasta (caso de campo no Miyoo Flip: gamedir virou o mount do loader e o
  preflight universal recusou). O launcher agora exporta `NXBOOTSTRAP_EXE`
  com o caminho REAL do executável em todo lançamento; a matriz (perfil
  spruce, caso alt-loader) prova que o filho recebe o valor certo. Loaders
  devem preferir `NXCOMPAT_GAME_DIR`/`NXBOOTSTRAP_EXE` ao `/proc/self/exe`.
  Teto do launcher 1010→1015.


## 0.6.23 — 2026-08-19

- **Loader alternativo para TODO ELF empacotado.** Em CFW 64-bit-only com o
  runtime ARMHF fora do caminho (spruce/Miyoo Flip), o 0.6.19 ensinava só o
  JOGO a rodar pelo `ld-linux-armhf` alternativo — o **nxsplash** executado
  direto morria com o falso "No such file or directory" (status 127) e, por
  ser obrigatório, abortava o launch inteiro (caso de campo: Titan Souls no
  Flip; instalação OK, tela preta no splash). Agora o splash passa pelo mesmo
  prefixo quando o interpretador embutido dele não existe, com NOTE no log;
  splash com interpretador presente segue byte-idêntico. Selado test-first na
  matriz (perfil spruce, splash ELF ARMHF real). Teto do launcher 995→1010.


## 0.6.21 — 2026-08-18

- **Escudo de áudio do guest (capability `audio.embedded-openal`).** CFWs como
  AmberELEC/P4ELEC trazem `/etc/openal/alsoft.conf` com `drivers=alsa`, que o
  OpenAL-soft EMBUTIDO no jogo Android lê — resultado: mudo (caso de campo do
  Tightrope). Ports que declaram a capability ganham `ALSOFT_DRIVERS=opensl`
  exportado pelo launcher (a ponte do próprio loader), neutralizando o
  alsoft.conf do host. A lista `NXCOMPAT_REQUIRED_CAPABILITIES` é separada por
  NEWLINE e o casamento é por word-splitting (selado por
  `tests/test-audio-shield.sh`). Todo launcher agora imprime um recibo de
  ambiente de áudio em uma linha (`ENV RECEIPT: ...`). Ports sem a capability
  geram byte-idêntico.

## 0.6.20 — 2026-08-18

- **RUNTIME DIAGNOSIS no fim do launcher.** Bloco único no final do log com o
  resumo do run (fase alcançada, saída do jogo, recibos) para o relato de campo
  apontar a causa sem segunda viagem.

## 0.6.19 — 2026-08-18

- **ARMHF via loader alternativo (spruce/Miyoo Flip).** Em CFW 64-bit-only sem
  `/lib/ld-linux-armhf.so.3`, o launcher roda o binário 32-bit invocando um
  dynamic loader ARMHF empacotado, em vez de morrer no preflight do 0.6.18.

## 0.6.18 — 2026-08-18

- **Preflight do interpretador (PT_INTERP) antes do exec.** Um ELF cujo carregador
  dinâmico não existe no device `exec()`a com ENOENT, que o shell mostra como
  "No such file or directory" para o próprio binário + status 127 — tela preta que
  parece port quebrado mas é runtime ausente no aparelho. Caso clássico: port 32-bit
  (ARMHF) num CFW 64-bit-only sem runtime ARM 32-bit (spruce/Miyoo Flip:
  `/lib/ld-linux-armhf.so.3` ausente; Titan Souls no Miyoo Flip). O launcher agora lê
  o interpretador embutido no ELF (`grep -a` no `.interp`) e, se ele não existir,
  emite mensagem CLARA (32-bit sem multiarch → use Knulli/ROCKNIX ou build AArch64;
  ou 64-bit sem runtime) + NXEVENT `preflight failed` e sai limpo, em vez do 127 seco.
  ELF estático/ilegível → pula o check (nunca bloqueia à toa). Teto do launcher 900→915.


## 0.6.17 — 2026-08-17

- Identidade da CFW: além de `~/.config/.OS` e do par de artefatos do
  dArkOSRE, o launcher lê o `title=` do `/usr/share/plymouth/themes/text.plymouth`
  (ArkOS/TheRA/RetroOZ — a mesma fonte do `device_info.txt` do PortMaster) e,
  por fim, `OS_NAME`/`NAME` do `/etc/os-release` (ELEC/JELOS/ROCKNIX/muOS/
  Knulli), sempre saneado para `[A-Za-z0-9._-]`. Relato real: Dan the Man no
  ArkOS abria com `cfw=none`. Continua sendo só diagnóstico e seleção de mod.
- Fase NXExtract: antes de chamar o extrator o launcher prova que o `python3`
  do firmware consegue iniciar (`import encodings, json, zipfile, hashlib`).
  Se a cache de bytecode da stdlib estiver corrompida (muOS: `bad marshal
  data` em `encodings/__init__`), tenta uma cache privada
  (`PYTHONPYCACHEPREFIX=$GAMEDIR/.nxpycache`) e, se ainda assim não iniciar,
  encerra com mensagem clara (reason `6209 nxextract-host-python-unusable`)
  em vez de "terminal result is missing, unsafe or malformed".

## 0.6.16 — 2026-08-16

- O launcher gerado agora exporta `NXLAUNCH_FRONTEND=1` quando nenhuma variável
  `SSH_*` está presente, registrando se a corrida podia colocar imagem no painel.
  Um port aberto por shell remoto pode não abrir janela por motivos que nada
  dizem sobre o jogo — no RK3326/ArkOS o mesmo binário que fica preto por SSH
  se repara e desenha quando aberto pelo frontend. Sem esse recibo, lendo só o
  log, a falha do arnês é indistinguível de regressão real, e foi perseguida
  como tal por horas em 16/08/2026.
- O adapter consome isso junto de `nxgl_frame_proof_is_conclusive_v2`: quadro
  desenhado prova que o port desenha em qualquer lançamento, quadro vazio só
  acusa o port quando o lançamento podia ter gerado imagem.

## Próxima versão

- O registro finito aceita a quirk opt-in do adapter Bomb Chicken v44 que
  separa as ABIs dos dois overloads IL2CPP de `PlayerPrefs.GetString`; ela
  continua desligada por padrão e não altera nenhum outro port.

## 0.6.15 — 2026-08-16

- Fixa NXExtract 1.2.10, valida estritamente seu resultado terminal atômico e
  copia o resumo canônico sanitizado para `log.txt`, preservando o status real
  de erro do extrator.
- Introduz `nxbootstrap-phase-v1` em `nxphase-result.json` e os limites
  `PHASE`/`NXEVENT` de preflight, NXExtract, NXSplash e runtime; provider de
  vídeo e primeiro frame permanecem observações explícitas do adapter.
- Move a instalação do trap de saída para antes de qualquer descoberta de
  path/PortMaster e comprova fallback 0600 para runtime privado quando game dir
  e launcher são não graváveis, sem usar o comando externo `stat`.
- Adiciona cortes executáveis entre todos os pares de fases e mantém os bytes
  e pixels aprovados de NXSplash 0.1.2 e da UI NXExtract 1.2.9 inalterados.

## 0.6.14 — 2026-08-15

- Adota por opt-in o NXExtract 1.2.9, com UI gráfica multiarch e validação
  flexível do container APK baseada em package e identidade interna forte.
- Mantém o NXSplash 0.1.2 e preserva byte a byte template/runtime; somente o
  slot de versão do NXExtract muda no gerador.
- Reexecuta os gates permanentes de CFW seguro, dArkOSRE com dois marcadores,
  erro pré-runtime 0600 e ambiente sem o comando externo `stat`.

## 0.6.13 — 2026-08-15

- Atualiza o pin do helper obrigatório para NXSplash 0.1.2, mantendo seleção
  por arquitetura, modo e bytes conferidos pelo manifesto imutável do
  componente; a recuperação gráfica fica no processo separado do helper, com
  SDL/provider portátil, framebuffer true-color e TTY somente diagnóstico.
- Eleva o pin dos novos manifests para NXExtract 1.2.8, cuja UI obrigatória
  aceita somente prova gráfica SDL/fbdev e possui artefatos imutáveis por
  arquitetura; o NXExtract 1.2.7 permanece apenas nos ports já fixados nele.
- Não altera o template nem o runtime legado; no gerador muda apenas o pin
  NXExtract. O handoff continua no mesmo slot, depois de NXExtract/payload e
  antes de adapter, bibliotecas privadas e lifecycle nativo.
- Adiciona um gate cumulativo que fixa os bytes do core 0.6.12, permite somente
  o slot de pin NXExtract no gerador, compara os launchers ARMv7/AArch64 depois
  de normalizar somente os slots de versão e exige as provas executáveis da
  observação segura de CFW introduzida em 0.6.8.
- Mantém a prova de execução sem o comando externo `stat`; ports publicados
  continuam presos aos pins antigos até migração opt-in e novo ZIP.

## 0.6.12 — 2026-08-14

- Faz opt-in explícito no NXExtract 1.2.7 para novos manifests v2, sem alterar
  ports já publicados ou reinterpretar pins antigos.
- Exige o novo contrato de renderer atestado do extrator antes da instalação;
  a UI obrigatória continua separada do lifecycle nativo do jogo.
- Conserva a ordem `NXExtract/UI → payload → nxsplash de 5000 ms → adapter →
  jogo`, inclusive no marker fast-path e em todos os perfis de CFW.

## 0.6.11 — 2026-08-14

- Torna `nxextract-ui` parte obrigatória do conjunto NXExtract sempre que a
  fase BYO está ativa; ausência, symlink, arquivo vazio ou helper não executável
  agora falham antes da extração e do jogo.
- Inclui a UI na detecção de integração parcial do modo `auto`, impedindo que
  um pacote incompleto recaia silenciosamente para extração headless.
- Preserva a ordem `NXExtract/UI → payload → nxsplash → adapter → jogo`; a logo
  NEXTOS continua obrigatória por cinco segundos em toda abertura.

## 0.6.10 — 2026-08-14

- Atualiza o helper obrigatório para nxsplash 0.1.1, mantendo seu hash, modo e
  arquitetura presos ao manifesto imutável do componente.
- Preserva a ordem NXExtract → gate do payload → tela → adapter/bibliotecas →
  lifecycle e passa `CUR_TTY` apenas como dica explícita; se ela estiver vazia,
  o helper resolve de forma segura o VT ativo publicado pelo kernel.
- Adiciona regressão do novo identificador do handoff sem alterar ports antigos,
  que continuam presos ao nxbootstrap/nxsplash anterior até opt-in.

## 0.6.9 — 2026-08-14

- O handoff fixo do nxsplash 0.1.0 passa a ser obrigatório em todo launcher
  recém-gerado, depois de NXExtract/preparo/gate do payload e antes dos hooks,
  bibliotecas privadas e lifecycle nativo do jogo.
- O gerador instala atomicamente o ELF `nxsplash-nextos` da arquitetura correta
  ao lado do `nxport.json`; ele entra implicitamente nos arquivos obrigatórios
  e mantém modo 0755 e bytes conferidos contra o manifesto imutável.
- Falha de renderer continua fail-open dentro do helper; helper ausente,
  inseguro ou não executável identifica pacote malformado e impede o runtime.
- A descoberta gráfica recebe somente caminhos host/PortMaster; nxbootstrap não
  adiciona biblioteca privada do jogo antes da tela.
- Os gates passam a provar ordem, bytes, ausência de opção pública de remoção,
  execução sem o comando externo `stat` e compatibilidade acumulada.

## 0.6.8 — 2026-08-12

- A identidade do firmware agora continua observável quando um `control.txt`
  antigo depende de `~/.config/.OS` ausente: primeiro é aceito um `.OS` regular
  e seguro; dArkOSRE exige simultaneamente dois artefatos regulares próprios.
- A identidade detectada serve apenas para log e seleção segura de
  `mod_${CFW_NAME}.txt`; nunca escolhe correção de vídeo, áudio ou controle.
- Permanecem obrigatórios e cobertos cumulativamente o lock sem `stat`, o log
  pré-runtime 0600, o handoff de `PM_PIPE` e `pm_finish` exatamente uma vez.

## 0.6.7 — 2026-08-12

- Restaurado no launcher gerado o handoff de diálogo do PortMaster que já existia
  no runtime 0.5.1 aposentado e permaneceu obrigatório no contrato.
- Um `PM_PIPE` ativo agora precisa ser um FIFO vivo e não-symlink; o launcher usa
  apenas `PortMasterDialogExit` e comprova que o caminho desapareceu antes de
  iniciar o jogo.
- Mantidos como gates cumulativos o funcionamento sem o comando externo `stat`,
  o diagnóstico pré-runtime 0600 e `pm_finish` exatamente uma vez.
