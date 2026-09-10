# 0.4.5 (2026-09-02, diretório do jogo dos launchers 0.7.x)

- `nx-device-launch.sh`: a descoberta do diretório do jogo aceita
  `NXBOOTSTRAP_LOGICAL_GAMEDIR` (launchers do nxbootstrap 0.7.x) além do
  `GAMEDIR` antigo; sem isso o harness caía no palpite por nome e a prova
  automática de controles (nxinput 0.10.2) não encontrava o log.

# 0.4.4 (2026-09-01, receipt C6 no support bundle)

- `nx-support-bundle.py` ingere as linhas `NXC6-SEAM` e `NXC6-DOMAIN` que o
  nxinput 0.10.0 passa a emitir no log normal do port (contrato 5.9):
  autoridade vencedora, motivo de cada degrau, contagens da classificacao
  semantica de dominio, classe/alvo/retries/elapsed da aquisicao do banco
  vivo, FACE_LAYOUT, nome sanitizado e hash do mapping efetivo — sempre como
  eventos OBSERVED, jamais promovendo estado.
- Allowlist fechada de campos: pid/tid/timestamps, prosa livre e qualquer
  token nao listado sao descartados; path pessoal ou mapping cru nunca entra
  no bundle compartilhado.
- Gate novo `tests/test_v4_c6_receipt.py` registrado na matriz.

# nxobs changelog

## 0.4.3 — 2026-08-31 (diretório privado de prova visual)

- `nx-device-launch.sh` cria o diretório remoto de prova como uma operação de
  posse exclusiva: `umask 077 && mkdir`. Um caminho já existente,
  inclusive symlink, falha fechado antes de iniciar o jogo; nunca é reutilizado,
  completado nem apagado. O cleanup só alcança um diretório cuja criação esta
  invocação confirmou.
- A criação não usa `mkdir -p`, `chmod` posterior nem o comando externo `stat`.
  Isso elimina a janela 0755 observada com umask 022 e também impede que uma
  correção de permissões transforme um caminho pré-plantado em autoridade.
- Gate dirigido `tests/test_device_launch_proof_dir.py`: modo 0700 e owner real,
  recusa/preservação de diretório pré-existente e symlink, além da fronteira
  estática contra as três regressões.

## 0.4.2 — 2026-08-29 (V4-CONTROLLERS-03 / C2: preservação dos receipts de input)

- O support bundle ingere os seis receipts do `nxinput_observe`
  (`NXINPUT-LOAD`, `-CAPABILITIES`, `-BINDING`, `-CHORD`, `-EVENT`,
  `-CONSUMER`) como eventos `input/observe-*` **sempre OBSERVED** — a
  ingestão nunca promove estado: um consumer `pending/not-instrumented`
  permanece exatamente assim no bundle publicado.
- Somente chaves de uma allowlist sobrevivem; chaves reservadas/desconhecidas
  (hostname, ip, path, nome livre) são descartadas fail-closed. Antes desta
  versão o bundle não interpretava esses receipts e a observabilidade de
  input sobrevivia só no log cru.
- Gate novo `tests/test_v4_input_observe.py`: seis tipos, observed-only,
  allowlist e fim a fim pelo `build_bundle`.

## 0.4.1 — 2026-08-29 (V4-GRAPHICS-04: preservação do GRAPHICS-EVIDENCE)

- O support bundle passa a **ingerir e preservar sanitizado** o receipt final
  `GRAPHICS-EVIDENCE` do nxgl (evento `graphics/evidence`, reason 1601):
  verdict OK vira `ok`, verdict FAIL vira `failed`, e somente campos de uma
  allowlist sobrevivem (`generation`, `commit`, `build_id`, `egl_build_id`,
  `sdl`, `cfw`, `requested`, `obtained`, `drawable`, `pre_drawable`,
  `shader_probe`, `verdict`, `reason`, `phase`, `first_present`, `port_id`,
  `port_version`). Os caminhos de DSO dos providers são privados e **nunca**
  copiados. Antes desta versão o bundle não interpretava a linha e a prova
  pós-present sobrevivia só no log cru que ninguém envia.
- A linha diagnóstica `GRAPHICS-PREPRESENT-EVIDENCE` é preservada como
  `graphics/pre-present` **OBSERVED** (reason 1602): um estado
  `awaiting-first-present` nunca é representado como `ok` e nunca promove.
- Gate novo `tests/test_v4_graphics_evidence.py`: OK, FAIL, pré-present
  observed-only e sanitização dos caminhos, unitário e fim a fim pelo
  `build_bundle`.

## 0.4.0 — 2026-08-29 (V3-PERF-01 e VSYNC/OBS)
- **O support bundle passou a INGERIR os recibos PERF e VSYNC.** Eles são o
  entregável inteiro de dois débitos e o bundle — que é o artefato que o
  jogador realmente compartilha — deixava os dois cair no chão: os números
  sobreviviam só num log bruto que ninguém pede para enviar. O `VSYNC:` existe
  para tornar visível o driver que ignora o intervalo pedido, e é justamente
  `requested` contra `effective` que agora chega ao bundle separado, com
  `honored` junto.
  Provado contra o EMISSOR C DE VERDADE, compilado e executado no gate, não
  contra uma string digitada: uma deriva de formato seria invisível de outro
  jeito. Verificado por mutação: o bundle voltar a ignorar reprova; o emissor
  colapsar `requested` em `effective` reprova; renomear um campo do PERF
  reprova.

- **Terceiro leitor, mesmo teto: o log do NXExtract.** O orçamento do log
  compacto é 2 MiB e uma receita com centenas de candidatos de payload emite
  muito mais que `MAX_EVENTS` linhas que casam. Este defeito foi corrigido três
  vezes — `events.jsonl`, log de runtime, log do extractor — porque eram três
  cópias que se afastaram. Agora os três leitores usam a mesma `EventWindow`, e
  um **guarda estrutural** no gate exige que qualquer leitor de linhas passe por
  ela: a próxima cópia não nasce. Verificado por mutação: tirar a janela de
  qualquer um dos três reprova.

- **E o mesmo teto derrubava o LOG DE RUNTIME, que é a entrada principal.**
  Consertar só o `events.jsonl` deixou o defeito de pé no arquivo que o bundle
  sempre lê: um `log.txt` de 4 MiB carrega milhares de registros `NXEVENT` e
  estourava `MAX_EVENTS` do mesmo jeito. Agora existe **uma** implementação de
  janela limitada (`EventWindow`) usada pelos dois leitores — era a divergência
  entre duas cópias que tinha permitido a correção parar no meio.
- **Linha rasgada no INÍCIO do log de runtime deixou de custar o bundle.** Um
  log pode ser truncado na frente por qualquer coisa que o limite: o corte de
  orçamento do próprio launcher, um cartão que encheu no meio da escrita, uma
  cópia que começou tarde. A primeira linha sobrevivente é então meia
  `NXEVENT {...}` — ainda começa com o marcador e já não é JSON válido. Isso é
  linha rasgada, não entrada hostil, e vira diagnóstico (`1903`) em vez de
  reprovar tudo. No meio do arquivo, registro malformado continua reprovando.
  Verificado por mutação: voltar ao teto rígido reprova; recusar a linha
  rasgada reprova.

- **E um andar abaixo, o mesmo defeito: 2048 eventos derrubavam o bundle.**
  `MAX_EVENTS` é um limite de memória contra entrada hostil, não uma afirmação
  de que um arquivo maior é malformado — mas era tratado como se fosse. O
  orçamento de eventos do runtime é 1 MiB, que comporta milhares de registros,
  então **uma sessão longa e saudável estourava o teto e levava o bundle
  inteiro junto**. Perder evento velho é aceitável; perder o bundle não. Agora
  o leitor guarda uma janela limitada dos registros **mais recentes** — é no
  fim que está a falha sendo diagnosticada — renumera a sequência e emite um
  diagnóstico dizendo quantos foram descartados, para que a truncagem seja
  visível em vez de parecer uma sessão que fez pouco. Entrada hostil no meio do
  arquivo continua reprovando, e a última linha rasgada continua tolerada.
  Verificado por mutação: voltar a estourar reprova; truncar em silêncio
  reprova; guardar a cabeça em vez da cauda reprova; sequência esparsa reprova.
  A ligação escritor→leitor passou a ser testada com os BYTES QUE O LAUNCHER
  REALMENTE ESCREVE, e foi essa ligação que expôs o defeito.


- **O support bundle recusava o `events.jsonl` de um port real.** O launcher
  escreve DUAS formas de `nx-event-v1`: registros de fase com `reason_code`
  numérico e as famílias de recibo NXU/NXR com um `code` em texto e **nenhum**
  `reason_code`. A segunda forma é escrita por todo port publicado desde a V3 —
  uma instalação V4 que reconstrói a cache emite `NXU0012` — e o leitor
  derrubava o bundle inteiro com "event reason code is invalid". Ou seja: o
  bundle que existe para diagnosticar um port que se comportou mal falhava
  fechado exatamente nos ports em que algo aconteceu.
  O código de recibo agora é mapeado para um espaço numérico próprio e
  **disjunto** dos códigos de fase (NXU→7000+, NXR→8000+), com o código
  literal preservado em `details`. Um registro sem `reason_code` E sem `code`
  bem formado continua sendo entrada hostil e continua reprovando.
  A correção é do LEITOR de propósito: os ports já publicados escrevem a forma
  antiga e vão continuar escrevendo, então mudar só o escritor não removeria a
  exigência. Verificado por mutação: sem o mapeamento volta o erro original;
  com os espaços colapsados NXU e NXR viram o mesmo número; aceitando qualquer
  texto como código, o registro sem código passa.

- Novo módulo `nxobs_perf` (`include/nxobs_perf.h`, `src/nxobs_perf.c`),
  passivo, somente leitura e limitado por construção, fechando as métricas que
  a auditoria V3 apontou como ausentes: **threads, descritores abertos, CPU
  (utime/stime) e `/proc/self/io`**, além de RSS/HWM.
  - **Observar nunca autoriza agir.** O módulo não mata processo, não limita
    save, não estrangula frame e não toca no orçamento do jogo.
  - Nada é decidido por nome de aparelho, CFW ou firmware.
  - Campo ausente vira máscara de completude. Um kernel sem
    `TASK_IO_ACCOUNTING` produz um recibo **menor**, nunca um recibo
    inventado; um `/proc/self/io` truncado é incompleto, não parcialmente
    confiável.
  - Os marcos são exatamente os quatro intervalos especificados: launcher →
    NXExtract, fim do gate de dados → splash, adapter → primeiro frame e
    primeiro frame → `menu-ready`. A **primeira** marcação é a medição; uma
    repetição é ruído e é ignorada. Marco não atingido é reportado como
    ausente, nunca como zero.
  - O caminho de frame é O(1): sem I/O, sem alocação e sem log. Spam por frame
    reprova QA e leitura de `/proc` por frame desgasta o cartão; o gate prova
    isso lendo a própria função.
  - `nxobs_perf_tick` respeita um piso de 1 segundo por construção,
    independentemente da frequência com que o adapter chame.
- **VSYNC/OBS (AUD-22)**: o recibo registra `requested` e `effective`
  **separadamente**, mais `honored`, a contagem de frames e o FPS medido. Um
  driver que ignora o intervalo pedido em silêncio é exatamente o que este
  recibo existe para tornar visível.
- Aditivo e opt-in por construção: nada roda enquanto o adapter não chamar.
  Nenhum port aprovado (inclusive FF4) é migrado ou regerado.
- Novo gate `tests/run-perf-sampler.sh` (GCC e Clang, `-Werror`), hermético por
  fixtures de `/proc`, com negativos de fonte ausente e truncada, marcador
  hostil, limite de taxa e auditoria estática de que o caminho de frame não faz
  I/O nem log e de que o amostrador não age.

## 0.3.1 — 2026-08-27

- Hardens `nx-support-bundle.py` so structured Authorization, Bearer/API key,
  X-Authentication, Cookie/Set-Cookie, token, key, session and auth data never
  reaches a public bundle. Values embedded in bounded text, serialized JSON or
  query strings use the same fail-closed classifier.
- Preserves useful diagnosis with finite `redacted-*` markers while retaining
  event source, phase, status, reason code and safe details. Private host fields
  are dropped and credential-like public IDs/metadata are refused.
- Extends the M12A and native-events gates with synthetic-only negative
  fixtures proving every fake value is absent from every generated bundle
  file. No raw field value from reported logs is copied into a fixture.
- Makes `tools/nx-logs` sanitized-by-default. Raw inputs live only in private
  staging, the TAR allowlist/type/duplicates are checked before extraction,
  sanitizer failure publishes nothing, and staging is removed. Historical
  `--bundle` is an alias of the default; preserving raw evidence requires the
  explicit, prominently marked `--raw-internal` maintenance mode.
- The `nx-support-bundle` CLI, schema versions, limits, event enums, crash API,
  visual behavior and native game flow remain unchanged.

## 0.3.0 — 2026-08-26 (V3-OBS-01 / V3-PERF-01)

- events.jsonl nativo: `nx-support-bundle --events-file` consome o arquivo de
  eventos `nx-event-v1` do launcher/extractor pela MESMA validação do caminho
  NXEVENT; linha final rasgada (escritor interrompido) vira diagnóstico
  `torn-final-line`, linha malformada no meio falha fechado. O run_id único
  atravessa launcher → NXExtract → loader → jogo no bundle.
- Famílias de código registradas no schema: NXU (update/gerações), NXA
  (compatibilidade de APK), NXG (gráficos), NXI (input), NXO
  (observabilidade), NXR (runtime), NXE (legado).
- V3-PERF-01: versão do componente nxobs e versão da ferramenta
  nx-support-bundle são fatos separados no relatório (fim do falso "híbrido");
  novo `nx-budget-check.py` (nx-budget-receipt-v1) mede logs do port contra os
  tetos iniciais (runtime 4MiB+2rot, compacto 2MiB, detail 8MiB+1rot,
  events 1MiB+1rot) sem jamais tocar jogo ou saves; `--strict` só reprova
  artefato do framework.
- Gates novos: tests/test_events_jsonl.py e tests/test_budget_check.py.

## 0.2.2 — 2026-08-22

- Added `nxobs_mem` (`include/nxobs_mem.h`, `src/nxobs_mem.c`): passive
  memory-pressure telemetry for adapters (P5). A read-only sampler of
  `/proc/self/status` and `/proc/meminfo` with a wall-clock interval floor of
  one second, adapter-opaque heap callbacks, a sanitized per-sample marker and
  completeness masks — a kernel that predates `RssAnon`/`RssFile` (both
  handheld field kernels, 3.14 and 4.4, do) yields `status=incompleto`
  instead of silent zeros. Observation never authorizes action: the module
  performs no GC, unload, texture eviction or `malloc_trim`, knows no game
  internals and never branches on device or firmware names. Any memory ACTION
  remains port-specific, experimental and gated by the Wave-20 (1 GB) plus
  2 GB-regression physical proof before promotion.
- New hermetic gate `nxobs-mem-sampler`
  (`tests/run-mem-sampler.sh` + `tests/test_mem_sampler.c`): synthetic /proc
  fixtures, interval floor and rate suppression, marker sanitization,
  completeness masks and adapter heap callbacks. Physical receipts (NextOS
  Mali-450 kernel 3.14, ~1 GB; dArkOSRE kernel 4.4, ~655 MiB visible) live in
  the internal evidence store; menu-open acceptance for the passive telemetry
  was decided by NextOS on 2026-08-22.

## 0.2.1 — 2026-08-16

- Added `nx-device-launch.sh`, a device harness that starts a port the way the
  firmware's own frontend starts it and reports a verdict instead of a guess.
  It discovers the frontend unit on the device, reads the `Environment=` lines
  that unit declares and reproduces them under `env -i`, so the run carries the
  frontend's environment and nothing an SSH login leaks into it.
- This exists because launching over SSH on ArkOS/dArkOS silently produces a
  black screen: the frontend unit carries
  `Environment="SDL_VIDEO_EGL_DRIVER=libEGL.so"` because the firmware's
  versioned SONAMEs resolve to driverless stubs while the unversioned names are
  the real Mali blob. Without it `SDL_CreateWindow` fails, the GL provider
  repair takes its pre-context branch, every candidate reports `eglInitialize
  failed on this kernel`, and the run is indistinguishable from a broken port.
  Three separate builds, including a published release, were investigated as
  regressions on 16/08/2026 before that unit was read.
- The variable is never hardcoded: the harness reproduces whatever the unit
  declares, so it keeps working on firmware that declares something else, and
  it does not amount to choosing an SDL driver on the firmware's behalf.
- Exit status is the verdict: `0` drew a real frame, `1` measured black on a
  launch that could draw, `2` proved nothing. Inconclusive is never reported as
  a defect. The frontend is restored on every exit path, including interrupts.

## 0.2.0 — 2026-08-16

- Adds the opt-in `nx-crash-v1` native observer for AArch64, ARMv7 and x86_64.
- Preserves the original fatal signal while recording phase, frame, registers,
  fault address, module/build-id/offset, asset, graphics call and provider.
- Captures a sanitized module map without filesystem paths; all runtime
  evidence uses exclusive owner-only files in a validated private directory.
- Adds strict crash-receipt ingestion to support bundles with an eight-record
  bound and terminal-failure precedence.
- Adds build-id-locked offline symbolization with source-path sanitization.
- Adds atomic private symbol archives explicitly marked as outside public ZIPs.
- Adds nxobs to declarative contract 1.0.30 as the eleventh version-locked
  component; the isolated gate rejects VERSION/API drift.
- Keeps the 0.1.0 bundle/event CLI compatible and changes no visual interface,
  renderer, game lifecycle or draw order.

## 0.1.0 — 2026-08-09

- Introduced bounded sanitized support bundles and `nx-event-v1` ingestion.
