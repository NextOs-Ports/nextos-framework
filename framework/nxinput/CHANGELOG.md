# 0.11.8 — porta axial do vetor (2026-09-04, CANDIDATE, OPEN)

- `nxinput_axis_axial(x, y, floor, &ox, &oy)`: numa (x,y) já normalizada, o componente MENOR é zerado
  enquanto fica abaixo de `floor`; o dominante passa inteiro; diagonal real (os dois >= floor) passa
  inteira. Campo Nameless Cat 1.2.8 (dArkOS K36S, 04/09, medido com `[nc/stk]`): empurrar tudo à
  ESQUERDA lê y = +0.28 neste stick (à direita, +0.13) e a engine trata "axis 2 > ~0.25" como agachar:
  o gato "parava do nada", mais à esquerda, e "não restaurava" a direção. O D-pad nunca teve componente
  menor, por isso funcionava. O sink de movimento do port passa o vetor por esta porta antes do
  MotionEvent (o cursor não). Teste + mutante em `tests/v5/test_v5_axis_calib.c`.

# 0.11.7 — corpus do default FP2 (2026-09-04, CANDIDATE, OPEN)

- `tests/v5/corpus/fp2-generated-v4.gptk` acompanha o default do FP2 1.1.4: `R3 = null` (o R3 nativo é
  KEYCODE_BUTTON_THUMBR/107, o próprio botão de pausa do jogo — pressionar R3 pausava; o único dono de 107 é
  `fp2.pause` via START). Nenhuma mudança de runtime.

# 0.11.6 — campo FP2 03/09 (noite), auditoria de universalidade (CANDIDATE, OPEN): seis correções universais + prova exige efeito da engine

- **Pre-router (F1):** o release adiado do 0.11.5 ainda saía no MESMO tick quando o press era descarregado por um
  EVENTO (duplo toque dentro da janela; "SELECT solto, depois START"): o padset chama `tick()` na mesma amostra e
  pagava a dívida na hora — o toque continuava invisível. Agora a dívida é independente de `state[]`
  (`release_deferred` 2→1→paga; `owed_release_id`), só um tick POSTERIOR ao que descarregou o press a paga, e
  `flush_edge` paga uma dívida pendente antes de encaminhar o press seguinte da mesma aresta (ordem). Testes:
  test_v5_prerouter (evento+tick na mesma amostra, casos 12/13) e test_v5_padset (duplo toque visto na união).
- **Costura (F2):** device que o glue NÃO conseguiu medir (devpath vazio = VIRTUAL; nó sem permissão, corrida de
  hotplug ou symlink recusado = UNKNOWN; `buttons=-1`) era `BLOCK_IDENTITY` — port não abria o pad = MUDO onde a
  SDL stock funcionava. Agora é admitido em MODO STOCK como o HIDAPI (1.4: UNKNOWN = stock). Nó evdev MEDIDO com
  0 botões e 0 eixos continua bloqueado. Teste: test_v5_seam 1f (2 mutantes).
- **Tradutor (F3):** D-pad publicado por gpio-keys como KEY_UP/DOWN/LEFT/RIGHT (abaixo de BTN_MISC) — a linha do
  CFW com `dpup:bN` ali era incoerente em TODO domínio → fonte cedia → builtin não sintetiza → `BLOCK_AUTHORITY`
  (mudo). Classe `SEM_DPAD` aceita as quatro setas SÓ para dpup/dpdown/dpleft/dpright; uma face em KEY_UP segue
  incoerente (mutante). Teste: test_v5_translate.
- **gptk_live (F5):** release de um controle que a ação NUNCA latchou (press saiu nativo com contexto não provado)
  devolvia DELIVERED — o adaptador descartava o release nativo e a engine ficava com o botão preso. Agora é
  PASSTHROUGH (o release pertence a quem viu o press). Teste: test_gptk_live_boundary (mutante).
- **Padset (F6):** primário = "primeiro admitido" deixava MORTOS o stick e os gatilhos de um pad com dois nós
  (adc-joystick + gpio-keys) ou do clone de prova admitido depois do pad real (medido no .137: 15 checagens de
  LEFT_STICK com 0 entregas). Agora o primário é ELEITO por atividade em `nxinput_padset_sample` (qualquer face/
  ombro/dpad em baixo ou eixo além de 1/4 do curso), com histerese: quem está ativo mantém a cadeira; um primário
  explícito nunca se move. O vetor e o gatilho continuam INTEIROS e de UMA instance (D2). Teste: test_v5_padset.
- **Bridge /4 (F4):** stick em `mode=digital|split` era projetado como NULL — o adaptador suprimia o eixo nativo e
  o stick morria em silêncio (só um contador no receipt). Sem sink V3 para as oito direções, a projeção é RECUSADA
  com motivo (`rc=-2`, `what`), e o loader trata o owner como rejeitado: a geração válida anterior/default segue
  viva. Teste: test_v5_gptk4_bridge (mutante).
- **Prova (oráculo):** `nx-device-input-proof.py` ganha `expect: context_change` — a janela precisa conter um
  receipt `kind=context` da ENGINE (contexto/source diferentes do último provado antes do estímulo). Motivo: no
  FP2 1.1.4 um hook IL2CPP na função de POLLING (`JoystickInput.getInputName` → `Input.GetButton`) matou todos os
  botões e a sessão "menu" deu ALL PASS só com receipts do adaptador. Teste: test_device_input_proof (mutante:
  delivery sem efeito). O nxgenerator 0.4.2 emite a expectativa a partir de `controls.proof.effects`.
- Higiene: `tests/test_gptk_preinit.c` tinha mkdtemp/mkdir/fopen DENTRO de `assert()` — em Release (NDEBUG) o
  teste segfaultava antes desta versão (pré-existente, não coberto pela bateria de host).
- Gates: run-v5-host 27 testes, **158 mutantes mortos / 0 sobreviventes**; run-padset-host, run-gptk-host,
  run-gptk-live-boundary PASS; ctest 53/53 (Debug e Release).

# 0.11.5 — campo FP2 03/09 (CANDIDATE, OPEN): toque curto de START/SELECT nunca mais some

- Pre-router: um toque completo dentro da janela (press+release < 180 ms) era encaminhado press+release no MESMO
  tick; o padset amostra por quadro e nunca via a borda — START não pausava, SELECT não agia (medido no FP2 1.1.4
  nos dois aparelhos: 3 toques de START no menu, 0 entregas). Agora o press sai ao fim da janela e o release só no
  tick seguinte (`release_deferred`), exatamente uma vez; um novo press durante o release adiado descarrega o
  release antes (duplo toque rápido preservado). Testes: test_v5_prerouter (traços por tick, duplo toque) e
  test_v5_padset (toque de 150 ms visto em UMA amostra). Mutantes: 150 mortos, 0 sobreviventes.

# 0.11.4 — V5 revisão 2 (2026-09-03, CANDIDATE, OPEN): medição por device, modo stock decidido uma vez, allow-list para back/start, gatilho universal

- F1 (N1): a medição em processo roda para TODO device evdev admitido e é aplicada contra o CONJUNTO de planos
  que reproduzem a tabela (`nxinput_provider_measurement_plans` + `nxinput_provider_apply_measurement_set`):
  pad reproduzido por vários planos (só BTN_*) não decide nada (fica UNDECIDED = stock); provider decidido
  (pin / fonte estática / medição anterior) é CONFIRMADO se está no conjunto e vira UNKNOWN se a tabela o exclui;
  nunca é substituído "pelo primeiro plano na ordem". Testes: test_v5_provider (2 pads, pin Knulli confirmado/contradito).
- F2 (N2): `nxinput_sdl_seam_ops.env_left_for_stock` (cauda 0.11.4): linha do CFW deixada no ambiente antes do
  init (provider indeciso) = modo stock para a corrida inteira, mesmo que a medição decida o provider depois
  (a prioridade USER da linha importada nunca é vencida pela API). O glue preenche pelo resultado do staging.
- F4: `guide`, `back` e `start` formam a classe SEM_SYSBIND, bindável em tecla de sistema (KEY_BACK/KEY_HOMEPAGE/
  KEY_MENU…); faces/ombros/dpad continuam na classe estrita (uma face em KEY_BACK segue incoerente — mutante).
  Forma estrita deixada pela sessão do FP2 (03/09) e adotada aqui.
- P2 (universal): `nxinput_padset_trigger_norm()` — gatilho do pad PRIMÁRIO em [0,1] pela calibração; junto com
  `nxinput_trigger_digital` substitui o par ENTER/EXIT e o `nxinput_padset_axis` (união por eixo) que os ports
  usavam para gatilhos. Teste em test_v5_padset (mutante: união entre pads).
- `NXC6-MEASURE` ganha `plans=`, `decided=`, `conflict=`.
- Gates: ctest 100%; run-v5-host.sh mutantes 0 sobreviventes; `run-gptk-live-boundary.sh` entra na matriz.

# 0.11.3 — V5-M3a: staging não pode confundir SUBSISTEMA de vídeo com "SDL já iniciada" (2026-09-03, CANDIDATE, V5 ABERTA)

Regressão universal medida no dArkOS/.137 (Mali-G31) ao provar o FP2 1.1.4 no aparelho: o guarda
"tarde demais para estagiar" de `nxc6_stage_before_init` (engine-glue/nxc6_glue.c) usava
`SDL_WasInit(0)` — QUALQUER subsistema. Toda engine que sobe o SDL VIDEO para o contexto GL antes de
o port abrir o pad (Unity/Godot/MonoGame) caía como "SDL já iniciada": `NXC6-STAGE result=error
provider_method=pinned-elf`, `staging failed before the joystick init (rc=-1)`, e então
`FATAL: FP2 public release requires a connected controller` — mesmo com js0..js2 presentes.

- Correção (framework, universal): o guarda passa a olhar só o subsistema que IMPORTA o
  `SDL_GAMECONTROLLERCONFIG` — `SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER` (SDL3:
  `SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD`). Vídeo/áudio já de pé não bloqueiam o staging.
- Gate novo (`tests/v5/test_v5_stage_subsystem.py`, ctest `nxinput-v5-stage-subsystem`, bateria):
  o predicado tem de casar as duas máscaras e nunca `SDL_WasInit(0)`/`EVERYTHING`; mutante (o
  defeito da 0.11.2 reintroduzido) tem de ser reportado — killed.
- Prova física: FP2 1.1.4 no .137 passa a boot ao menu (ver M3a); o velho `rc=-1` some.

Bateria: ctest 53/53; run-v5-host.sh 27 testes, mutantes 0 sobreviventes.

# 0.11.2 — V5-M3a: piso universal de símbolos SDL no runtime vendorizado (2026-09-03, CANDIDATE, V5 ABERTA)

Achado na M3a (FP2 1.1.4), ao primeiro `nxrelease validate` sobre um ELF com o nxinput 0.11.1
vendorizado: `nxc6_measure_provider_inprocess` (engine-glue) chamava `SDL_JoystickGetDeviceInstanceID`
por import direto — símbolo nascido na SDL 2.0.6, acima do piso universal 2.0.4
(`nx-sdl-symbol-floor/1`). Um CFW com SDL anterior não carregaria o port. Correção universal:

- `nxc6_glue.c`: o símbolo é resolvido por `dlsym(RTLD_DEFAULT, ...)` no ponto de uso; SDL sem
  ele = instância não enumerável daqui, nada medido (o descriptor fica como estava, modo stock).
- Gate novo na fonte (`tests/v5/test_v5_sdl_floor.py`, ctest `nxinput-v5-sdl-floor`, bateria
  `run-v5-host.sh`): nenhum `.c/.h` de `src/` ou `engine-glue/` pode CHAMAR entrada SDL nascida
  acima do piso do `nxabi/policy-v1.json`, pela mesma tabela que o nxrelease usa no ELF final.
  Mutante embutido (a chamada direta reintroduzida) tem de ser reportado — killed.
- `nx-vendor-nxinput.py`: `nxinput_gptk4_preinit.c` entra em RUNTIME_SRC (o 0.11.1 mandava o port chamar
  `nxinput_gptk4_preinit_load` mas não o vendorizava; a M3a o anexara à mão).
- O gate também apanhou `nxinput_authority_sdl.c` (SDL_RWsize/SDL_RWread/SDL_RWclose, entradas reais só
  desde a 2.0.10): a leitura do arquivo de texto passa a stdio — efeito idêntico, zero decisão.

# 0.11.1 — V5-M1a: bugs da revisão de 03/09, lógica de port no framework, itens C5/C6/D5/B8/E3/E9a/F2/F3/H5/H8/I3a (2026-09-03, CANDIDATE, V5 ABERTA)

Regra do NextOS (03/09): toda correção de controle é UNIVERSAL — nasce aqui, com teste RED e
mutante, e o port só chama. Nada desta linha toca `ports/`.

**Bugs da revisão independente (`REVISAO-NXINPUT-V5-CORRECAO.md`), todos com RED + mutante:**
- (1) **Provider UNKNOWN nunca deixa o pad mudo.** A costura ganha MODO STOCK
  (`NXINPUT_SDL_SEAM_ADMIT_STOCK`): nada traduzido, nada externo no store; o mapping que o
  provider já tem (import da env, built-in, síntese HIDAPI) é lido de volta e receitado
  (`source=stock-passthrough decision=DO_NOT_MUTATE_STORE passthrough=1`). O staging passa a
  conhecer o provider (`nxinput_sdl_seam_stage_with_provider` + `nxc6_stage_before_init`):
  com UNKNOWN a `SDL_GAMECONTROLLERCONFIG` fica NO AMBIENTE para o import stock; o staging
  legado (cego) só é reparado reinstaurando a própria linha env do CFW
  (EXISTING_NATIVE_PASSTHROUGH da 1.4; bundle/arquivo nunca). Provado no host com DSO não
  pinada (`tests/v5/run-unpinned-provider-oracle.sh`, 25 checks × 2 modos) e NO APARELHO
  secundário (`tests/v5/receipts/device-oracle-*-unpinned-stock*.json`, 25/25 × 2).
- (1b) **MEASURED_INPROCESS real.** `nxinput_provider_measure_by_id()` chama os ById da DSO
  mapeada para a instância aberta e `nxinput_provider_measurement_match()` casa a tabela
  medida com os planos transcritos; a PRESENÇA dos símbolos nunca mais infere domínio
  (`exported-api` = undeclared até medir). A costura mede na admissão (abre/fecha o joystick
  por índice; dentro da libSDL fica undeclared = stock). Tabela sem plano = UNKNOWN com
  `measurement_conflict=1`. Pins de runtime por arquivo (`NXINPUT_PROVIDER_PINS`).
- (2) **HIDAPI/hidraw = stock, nunca bloqueio.** `nxinput_provider_probe_device()` classifica o
  driver por device (evdev/hidapi/virtual/unknown), preenche `driver` e o digest físico; um
  device HIDAPI é admitido em modo stock com o mapping do próprio provider
  (`source=provider-native-hidapi`). `nxinput_sdl_seam_device` ganha `driver` (layout 0.10.0
  aceito).
- (3) `NXC6-PROVIDER` (e `NXC6-DEVICE`, `NXC6-MEASURE`, `NXC6-STAGE`) vão para o
  `NXC6_RECEIPT` (path conhecido antes de resolver o provider). Provado nos dois aparelhos.
- (4) Equivalência integral alimentada por evidência real: backend por device, flags da
  linha (half-axis/inversão/trigger-botão), digest do corpus da admissão, digest físico do nó.
- (5) `statically_linked` decidido contra `/proc/self/exe` (dev/inode), não contra "o mesmo
  objeto que o nxinput" (mutante: DSO reportada como estática, morto no probe do host).
- (6) Hat de um eixo: detecção por domínio (`nxinput_sdl_hat_present`; ascending = qualquer
  metade presente é hat, p009; upstream = o par).
- (7) `accepts()`: `guide` aceita teclas de sistema (KEY_MENU/HOMEPAGE/HOME/BACK/ESC/SELECT/OK/
  POWER/ENTER/SPACE) — só `guide`, para a prova por exclusão manter os dentes.
- (8) Pre-router: chord exige as duas arestas FISICAMENTE em baixo (tap SELECT + START = dois
  presses), duplo toque encaminha os dois, descarga em ordem de press, overflow visível — e
  está LIGADO: o `nxinput_padset` retém START/SELECT na janela, uma saída por chord, geração por
  instance, relógio injetável.
- (9) Calibração/piso de neutro no framework: `nxinput_gptk_live` ganha o piso de neutro do
  gesto de vetor (padrão 1/64, por controle, ≤ 0,9; mutante piso 0 + centro +0,0039 morto) e
  `nxinput_padset_vector_norm()` normaliza uma vez (Sint16 → [-1,1] zero exato) com deadzone
  radial. Substitui `nc_gptk_set_vector_neutral_floor` e o `!= 0.0f` de FP2/Blossom.
- (10) Linha do GUID ≥ 2048 bytes: a fonte cede (receipt), nunca chega ao setter.
- (11) `nxinput_padset` confere a instance aberta contra a admitida (corrida de índice:
  fecha, conta, não adota); `nxinput_lifecycle_open_checked()` com geração de hotplug.
- Router: 9ª fonte numa saída é recusada (antes: contada sem registro = saída presa).

**Lógica genérica que estava nos ports (auditoria 03/09):**
- `nxinput_gptk4_preinit_load(gamedir, contract, out)`: o pre-init schema 4 universal (dirs
  O_NOFOLLOW, default→owner, projeção V3 byte-igual à dos 4 ports, receipt JSON, linha de
  log). `test_v5_gptk4_preinit` (mutantes de symlink mortos).
- `nx-gptk4-host-gate` (M1c): gate de host universal de closure schema 4 (contrato
  `adapter-contract.json` + owner + contextos declarados [+ `closure.tsv`]), emite as linhas
  `NXGPTK_PROOF\tCONTEXT|CASE|SAFETY`. RED: o owner gerado do Tearscape
  (`tests/v5/fixtures/tearscape-owner-schema4.gptk`) passa 15/15; o harness V3 do port dizia
  NXI1006. Mutantes: owner /1, closure contra owner, contrato com kind errado, binding extra,
  `[base]` alcançando `cursor` não declarado NÃO falha. O sink na linha CASE é o primeiro
  `sinks[]` do contrato (M1b: emitir `closure.tsv` = `context\tcontrol\taction\tsink\tpress|motion\t1`).

**Itens abertos da missão, fechados nesta linha:**
- C5: reopen recusado, quarentena definitiva (holds soltos ao quarentenar — achado do fuzz),
  geração de hotplug, `open_checked`; remap durante hold solta na geração velha.
- C6: `nxinput_route_policy` — UMA política diante de todo setter (V5 seam pela máquina 1.4;
  SDL3/PortMaster e ordinal-fix V3 = LEGADOS com opt-in explícito; Godot nativo = rota tipada
  direta). A costura Godot nativa aceita `domain=sdl2-ascending-patched` e TRADUZ por código
  físico (mutante: os mesmos bytes declarados sdl2-evdev = readback discorda = não anunciado).
  Regenerar `engine-patches/C5B-ENGINE-PROVENANCE.json` no próximo rebuild da engine (M2).
- D5: grafo físico no pre-router (`nxinput_prerouter_bind_physical`): um dono de ingestão por
  physical_id (prioridade declarada), observadores nunca duplicam saída, SELECT só certificado.
- B8: `nxinput_coexist` (sequenciador da `SDL_GAMECONTROLLERCONFIG` global por major, hint
  provider-local, recusa concorrente) + arbitragem: **sdl2-compat sobre SDL3 = núcleo
  compartilhado** detectado pelos bytes (host E aparelho principal são assim); isolamento real
  provado com SDL2 clássica 2.32.10 (tarball pinado) + SDL3 no host: stores separados, símbolos
  por handle, um dono físico. `test_v5_coexist` (77 = skip sem DSOs).
- E3: `nxinput_authority_v5` executável (nextos/engine/synchronized; ENGINE sem owner editável,
  só espelho de readback; SYNCHRONIZED exige os 4 hooks; chord suprime; geração velha suprime).
- E9a/F2/F3: `nxinput_keyboard` — teclado físico → actions pelo MESMO router (uma press/uma
  release com o gamepad), backends evento e polling idênticos, chord solta com o modificador,
  laço recusado no parser (cruzado entre seções — gap do parser fechado) e na fonte; prompts de
  teclado = `key.<chord>`, gamepad = tokens Xbox posicionais.
- H5/H8: `chain_harness` + `test_v5_chain.py` (oráculo independente: fixture + tabela pinada +
  owner; 8 mutantes mortos, inclusive provider errado numa pipeline coerente). Teclado,
  trigger digital, stick 8-way e rota touch tipada na mesma cadeia.
- I3a: `fuzz_v5_lifecycle` (ASan/UBSan, invariantes L1–L3/P1–P2/R1/K1) — achou e corrigiu a
  quarentena sem release.
- Registry: `nxinput_registry_glyph_for_sdl()` (glyph humano de ordinal SDL; mutante numérico
  morto) e regra NEG-2 `nxinput_registry_prompt_text_has_isolated_number()` (`PRESS 10 TO
  BEGIN` morto) para os 4 pontos de intercepção da M1c.
- H2a `[!]` INCONCLUSIVO POR ENQUANTO: `tools/nx-uhid-hidapi-pad.py` provou o caminho uhid no
  aparelho secundário (gamepad genérico → hidraw0 + evdev, SDL abre, harness entrega A/B); a
  classe HIDAPI (DS4 054c:05c4) precisa de um report descriptor DS4 pinado que o hid-sony aceite
  — o descriptor montado à mão foi recusado (sem START). Próxima tentativa: capturar
  `/sys/class/hidraw/*/device/report_descriptor` de um DS4 real e pinar.

Números: ctest 51/51; `tests/run-v5-host.sh` 25 testes + pins + fuzz + RED não pinado + chain +
host gate, **mutantes 140 mortos / 0 sobreviventes**; aparelhos: principal 46/46 (2 fixtures) +
coexist shared-core; secundário 46/46 + 50/50 (stock não pinado, 2 modos) + uhid controle.
Testes novos para a matriz (M1b): `nxinput-v5-gptk4-preinit`, `nxinput-v5-gptk-live-vector`,
`nxinput-v5-authority-v5`, `nxinput-v5-keyboard`, `nxinput-v5-route-policy`, `nxinput-v5-coexist`
(SKIP 77), `nxinput-v5-godot-seam-ascending`, `nxinput-v5-chain`, `nxinput-v5-fuzz-lifecycle`,
`nxinput-v5-gptk4-host-gate`; o gate `nxinput-v5-host` (`tests/run-v5-host.sh`) cobre todos.
Adapters (M2/M3): chamar `nxc6_stage_before_init()` em vez de stagear à mão;
`nxinput_gptk4_preinit_load()` em vez da cópia; `nxinput_padset_set_clock()` opcional;
`nxinput_padset_vector_norm()` + piso do `nxinput_gptk_live` em vez de `!= 0`; linkar
`nxinput_prerouter.c`, `nxinput_axis_calib.c`, `nxinput_decision.c`, `nxinput_provider.c`
onde o padset/costura entram; revendorizar byte a byte (gate da M1b).

# 0.11.0 — V5 controls: provider descriptor, physical translation, schema 4 (2026-09-03, CANDIDATE, OPEN)

Breaking for adopters: the seam ops table gains a V5 tail (`provider_domain`,
`provider_method`, `source_domain_slot`); `nxinput_sdl_api_domain()` is an upstream
presumption only and no longer decides the rewrite target.

- P0 fix (FP2 1.1.3/muOS, Blossom Tales 1.4.0/Knulli deaf pads): the C6 target domain
  comes from the PROVIDER DESCRIPTOR of the SDL the process mapped (`nxinput_provider`,
  `nxinput_provider_linux`): exported ById API > pinned sha256 > UNKNOWN (never rewrite).
- New domain `sdl2-ascending-patched` transcribed from the pinned RetroArch/Batocera
  patches and measured against the exact 2.30.12 DSO (`tests/providers`).
- `nxinput_translate`: full physical translation (bN/aN/hN, sign, inversion, half-axis),
  byte-intact when source == provider, unproven when provider UNKNOWN, rejected when
  incoherent; receipts `NXC6-PROVIDER` and `NXC6-DOMAIN` (v5 fields).
- Seam: same-GUID divergent refused BEFORE the setter (`store_mutated=0`); 0.10.0-sized
  ops accepted; `target_domain=undeclared` for UNKNOWN providers, and with a V5 ops table an
  UNKNOWN provider lets NO external ordinal line (env/CFW/bundle) reach the setter, not even
  unchanged (mission 1.4 `DO_NOT_MUTATE_STORE`): only the provider's own built-in database or
  a declared raw route remains; the bytes stay in the receipt for diagnosis.
- Third incident fixture: Nameless Cat 1.2.7/muOS (`matching=2` duplicate env lines,
  movement by hat/axes survives, faces/chord die) reproduced in `test_v5_incident_red.c`.
- Namespaces: `provider_generation` (descriptor), `mapping_generation` (router/registry),
  `context_epoch`, `modality_epoch`; no bare `generation` in the new contracts.
- `nxinput_padset`: whole vector from ONE instance (primary), overflow counted/logged.
- `nxinput_axis_calib`: EVIOCGABS → exact-zero normalization (midpoint/pinned centre,
  asymmetric halves, triggers `[0,1]`, hats, quarantine), radial deadzone, 8-way/4-way
  Schmitt directions, trigger digital edges.
- `nxinput_gptk4`: NEXTOS_CONTROLLERS/4 (docs/NEXTOS_CONTROLLERS-4.md), complete fixture,
  typed grammar, sparse overrides, keyboard source grammar, errors with line/column.
- `nxinput_route`: one primary route, refcount per (route, output, player, epoch),
  explicit same-transport companions, release-all, cooperative lease, self-source refusal.
- `nxinput_registry`: bindings/prompt/glyph bound to mapping generation, context epoch,
  modality epoch and source edge; Xbox positional pack by default; raw-ordinal text gate.
- Proof: `tools/nxoracle_v5.py`, `tests/v5/run-host-oracle.py`, `harness_sdl2_provider.c`,
  `tools/nx-provider-ordinal-probe.py`, mutation runner (V4 proof tool circularity proved,
  V5 oracle kills all mutants). Receipts: host, dArkOS K36S, NextOS Elite 4.9.
- Version identities aligned (VERSION, `NXINPUT_VERSION`, CMake) and the SDL3 discovery
  contract test now checks their agreement instead of a rotted literal.
- Round 2 (1.4 as ONE machine): `nxinput_decision` — SOURCE_TRUST × CONSUMER_KIND ×
  CONSUMER_TABLE → KEEP_EXISTING_BYTE_INTACT / TRANSLATE_TYPED / ROUTE_TYPED_DIRECT /
  DO_NOT_MUTATE_STORE, plus RESOLVED_EDGE_OWNER. INTEGRAL equivalence = backend, hat/half-axis/
  inversion/trigger flags, the COMPLETE ordinal table digest for the measured caps, corpus
  digest and physical digest; a shared label (`ascending`, major, GUID) never suffices. The C6
  glue (`nxc6_glue.c`, now requires `nxinput_decision.c`) runs every matching line through the
  machine: an unproven source or an UNKNOWN provider makes the WHOLE source yield
  (`result=source-unproven-yields`), i.e. the line never reaches `AddMapping`, not even
  byte-identical; a native line whose integral equivalence fails is emitted as the typed
  translation, never as BYTE_INTACT. Receipt `NXC6-DOMAIN` gains `decision=`.
- `nxinput_corpus` (B6): inventory of every mapping line by origin (builtin/hint-env/file/
  livedb/bundle/addmapping), `platform:` filter before election, byte-identical duplicates
  collapsed (`matching=N`), divergent duplicates refused before the store unless the later
  line has proved precedence AND both are provider-native; corpus digest feeds equivalence.
- `nxinput_prerouter` (D7, 5.6): sovereign START/SELECT pre-router outside the authority
  mode; bounded window (180 ms default, ≤1 s); same instance + same generation inside the
  window = both consumed, exactly one `system.exit`, START never pauses first; otherwise the
  individual press/release forwarded exactly once, in order; auto-repeat ignored; cross-pad,
  reused instance with a new `device_instance_generation`, focus, unplug and reload never latch.
- `nxinput_lifecycle` (D4, D6, E5a, E7): one central admit/open/remove/reconnect/focus
  lifecycle; open re-checks the admitted generation (instance race → quarantine); physical
  down-state separate from delivered state; every transition = release-all of the previous
  epoch → new `context_epoch` → NEUTRAL GATE (no owner until every source is neutral) → the
  declared fallback for unknown/loading/unproven (never immediate passthrough during a hold);
  hot reload releases the old generation before publishing `mapping_generation+1`, invalid
  owner text keeps the last valid generation. `nxinput_sync` (E13): synchronized authority as
  a CAS transaction with lease — owner_reload applies, reads back and publishes only on equal
  readback (else engine rollback, generation kept, owner untouched); engine_rebind_ui does the
  owner CAS on the digest it read (conflict = nothing written); rollback failure = restart.
- Mutation runner now aggregates the C machines (`NXINPUT_V5_BIN`): 46 killed, 0 V5 survivors,
  V4 tool still lets 2 survive (circularity). ASan/UBSan clean on the four new modules.
  `tests/v5/build-harness.sh` = reproducible harness build (host and cross); host oracle
  receipts regenerated with the new glue (3 fixtures, 21/21 each).
- E4a: `EXT.*` capability-gated extension controls in schema 4 (declared by the adapter,
  bound in `[base]`, omission = NXI4001, undeclared = NXI4004, default lists them as visible
  `null`); `nxinput_gptk4_ext()` / `nxinput_gptk4_ext_name_valid()`.
- Provider pins: muOS 2601.1 `libSDL2-2.0.so.0.2800.5` (sha256 40d0616f…, ById exports) MEASURED
  on the authorized aarch64 device = `sdl2-ascending-patched`; its 32-bit sibling (4e95cde5…) pinned
  UNDECLARED. The exact DSO ran the composed harness session for both muOS fixtures (21/21 each,
  byte-intact native). Manifest carries the image sha256, the modern/retro CFW tables and the DTS.
- `nxinput_gptk4_bridge` (pilots): loads the schema-4 owner (default → owner, bytes never
  rewritten, receipt with line/reason) and PROJECTS it onto the live V3 dispatch structure the
  shipped ports drive (`nxinput_gptk_live`): unified base in every context, sparse overrides,
  triggers/sticks by mode; marker `nxinput-gptk-runtime/4`. The schema-4 magic may follow the
  bilingual comment header. `tools/nx-gptk4-check` validates an owner file with the real parser.
- Not in this line: engine prompt hooks per pilot, pilots 1.1.4/1.4.1/1.2.8/0.2.17, HIDAPI
  /uhid claims, touch adapters, synchronized-authority CAS on a real engine (see handoff).
- 5.1 provider estático (03/09, tarde): `nxinput_provider_declare_static()` +
  `nxc6_declare_static_provider(entry, pin_id, domain)` — uma engine com SDL EMBUTIDA
  (Godot/SDL3 do Tearscape) declara a fonte pinada (tarball + patch da costura em
  `tests/providers/provider-manifest-v5.json` → `static_sources`) e o domínio medido;
  método `declared-static-source` no receipt `NXC6-PROVIDER`. Só vale quando `SDL_Init`
  mora no programa principal; DSO nunca é declarada por este caminho (fail-closed).
- Tradução física: `is_button_class` cobre `BTN_TRIGGER_HAPPY*` (K36S GO-Super: o D-pad
  do CFW usa 0x2c0+); regressão em `tests/v5`.
- Oráculo do aparelho (`tools/nx-device-input-proof.py`, modo V5): linha do CFW filtrada
  por `platform:Linux`, depois strings built-in da DSO, depois o built-in upstream pinado;
  `--gamedir` e `--frontend-stop/--frontend-start` para launchers fora da pasta do jogo.
- Lição dos pilotos (NC 1.2.8 no NextOS Elite, pad 0..255): a aresta de gesto de vetor
  dos glues NÃO pode ser `!= 0` — centro normalizado +0,0039 nascia "pressionado"; o piso
  de neutro é o mesmo deadzone da cinemática do cursor (regra do eixo assimétrico).


# 0.10.2 (2026-09-02, prova automática de controles no aparelho: ON_DEVICE_AUTOMATED_INPUT_PROOF)

- **Correção de interpretação (dono, 02/09):** o framework é automático e não
  depende de uma pessoa apertando botões. A prova de controles válida para
  compatibilidade de ports é o receipt `ON_DEVICE_AUTOMATED_INPUT_PROOF`,
  produzido no aparelho real por um clone uinput device-faithful comandado
  pela IA via SSH. Saúde do interruptor mecânico é QA de hardware, fora do gate.
- `engine-glue/nxinput_padset.{c,h}` (`nxinput-padset/1`): TODOS os pads
  admitidos pela autoridade do port (costura C6) abrem ao mesmo tempo; estado
  simbólico = união; eixo = maior deflexão; chord SELECT+START só no MESMO
  instance — SELECT num pad + START noutro é negado e registrado uma vez por
  ocorrência; hotplug compacta sem perder os outros. Vtable de SDL preenchida
  pelo chamador com a SDL do firmware: módulo puro, testado no host com
  fakes (`tests/run-padset-host.sh`). Sem ele, um clone uinput seria ignorado
  (o port abria só o índice 0) e a regra "combinações entre pads não encerram"
  não era verificável.
- `tools/nx-device-input-proof.py` + `tools/nx-input-inject-agent.py`:
  captura no aparelho (via `nx-device-launch`, env do frontend) o perfil do
  controle real no kernel (identidade, EV_KEY/EV_ABS, ranges EVIOCGABS,
  contagens), o provider SDL do firmware e o CFW; cria ANTES do SDL_Init do
  jogo N clones uinput fiéis; confirma pelo log do port que a SDL do sistema
  admitiu cada clone com o MESMO GUID/mapping (linhas `controller:` +
  `pad slot=` + C6); deriva controle→código SÓ do mapping admitido e dos
  bitmasks do nó (numeração SDL2 linux); executa um roteiro declarativo
  (`nx-device-input-proof-roteiro/1`) com vereditos por janela sobre o
  readback `nxinput-gptk-event-evidence/1`: entrega, supressão (`null`),
  passthrough, neutralidade, press/release uma vez, L1+R1 / L2+R2 / cross-pad
  não encerram, SELECT+START no mesmo instance encerra limpo; receipt
  `nx-device-input-proof/1` fixa device/CFW, SDL (path+sha), GUID,
  capabilities, mapping (sha, campos, readback), FACE_LAYOUT, GPTK
  (default/loaded/owner sha), adapter-contract sha, ELF sha, generation, run,
  contextos e sinks. Perfil reutilizável por hash (device+CFW+SDL) via
  `--profile-cache`. Nada disso entra no ELF/ZIP público.
- Gates novos na matriz: `nxinput-padset-host`, `nxinput-device-input-proof`.
- Ports: cada um fornece apenas ações/contextos/sinks (nxproject) e a
  navegação até cada contexto; Nameless Cat e FP2 são os primeiros
  consumidores. Ports antigos aprovados não migram automaticamente.

# 0.10.1 (2026-09-01, descoberta por capacidade: pad sem ABS_X/ABS_Y deixa de ser invisivel)

- Fecha o defeito de campo do Tearscape 0.2.11 no RG40XX-H/muOS: video
  perfeito, ZERO controle e SELECT+START morto. Causa raiz PROVADA em host
  com as tres SDLs reais + pads uinput de kernel: o fallback sem udev
  (`SDL_EVDEV_GuessDeviceClass`) so concede a classe JOYSTICK quando o node
  tem o par ABS_X+ABS_Y. Um controlador que expoe apenas botoes de jogo
  (cluster gamepad, BTN_DPAD, faixa TRIGGER_HAPPY) e no maximo um hat --
  o "combined controller" dos firmwares, `muOS-Keys` incluso -- nunca era
  enumerado: `SDL_GetJoysticks()` vazio, a costura C6 nunca rodava, nenhum
  recibo era emitido. O mesmo defeito ja tinha invalidado o Beach Buggy
  1.0.5 no mesmo aparelho (receita BB 1.0.6: "not discoverable by SDL's
  non-udev fallback").
- Conserto por CAPACIDADE nos tres patches da seam (SDL2 2.28.5, SDL2
  2.32.10, SDL3 3.2.30): `NXC6_KeybitsCarryGameButtons()` espelha o
  `input_id` do udev -- um node cujo keybit carrega qualquer codigo em
  BTN_JOYSTICK..BTN_DIGI, BTN_DPAD_UP..BTN_DPAD_RIGHT ou
  BTN_TRIGGER_HAPPY..BTN_TRIGGER_HAPPY40 e joystick, mesmo sem ABS_X/ABS_Y.
  Nenhum nome, GUID, VID/PID ou CFW consultado; mouse, digitizer e teclado
  puro continuam de fora. A ordem soberana C3 e a admissao C6 ficam
  intocadas: o sweep so devolve a EXISTENCIA do device ao funil que ja era
  fail-closed.
- Teste de verdade na bateria C6 (vermelho provado contra os binarios
  0.10.0): cenarios `stickless_combined` (cluster gamepad + BTN_DPAD +
  teclas ESC/volume, ZERO eixos), `stickless_plain` (so cluster + hat) --
  ambos exigem enumeracao, admissao por autoridade e entrega real de
  controles -- e `keyboard_only` (node so de teclas de teclado), que exige
  invisibilidade para provar que o sweep nao sobre-admite. Nos binarios
  0.10.0 o proprio `discover_guid()` morre com "sdl assigned no guid",
  que e exatamente a falha de campo reproduzida.
- `C6-SDL-PROVENANCE.json` reselado (patch/patched/binary dos tres SDLs);
  `run-sdl-c6-host.sh` ganha os tres cenarios e passa a linkar
  `nxinput_livedb.c` no SEAM_SRC (0.10.0 referenciava
  `nxinput_livedb_path_class_name` sem o objeto).
- Nada muda em GPTK, dominios, layout authority ou contratos: releases que
  consomem 0.10.0 permanecem validas; o bump e aditivo e de descoberta.

# 0.10.0 (2026-08-31, autoridade de layout muOS: FACE_LAYOUT, banco vivo e prova semantica de dominio)

- Fecha o defeito de campo do RG40XX-H/muOS 2601.1: A/B e X/Y sao PREFERENCIA
  do usuario (`modern`/`retro`, symlink recriado no boot), nunca identidade do
  aparelho. Nenhuma regra decide por CFW, modelo, GUID, VID/PID ou nome; esses
  dados aparecem somente como fixtures/evidencia.
- `NEXTOS_CONTROLLERS/3` (GPTK V3): herda completude e tri-state do V2 e exige
  exatamente uma linha de preambulo `FACE_LAYOUT = auto|modern|retro`
  (minusculas exatas; near-miss falha com mensagem propria). V1/V2 permanecem
  byte/semantica-identicos e equivalem a `auto`. `nxinput_gptk` ganha o campo
  aditivo de cauda `face_layout` + `nxinput_gptk_face_layout_of()/name()`;
  `nxinput_gptk_upgrade` reporta a diferenca de FACE_LAYOUT no diff sem tocar
  a copia do dono. Marcador de runtime vivo sobe para
  `nxinput-gptk-runtime/3` (oraculo selado V4-CTRL-01, caso N28).
- Fronteira pre-init `nxinput_gptk_preinit_load()`: o mapa do dono/default e o
  layout sao lidos EXATAMENTE uma vez, antes de declarar bundle, staging e
  `SDL_Init`; nada rele por frame nem depois (TOCTOU zero).
- C6: `nxc6_declare_port_bundle_for_layout(gamedir, layout)` seleciona qual
  bundle pode servir de autoridade 3 (`controllers.nxb` para auto,
  `controllers-modern.nxb`/`controllers-retro.nxb` para as variantes; arquivos
  regulares, nunca symlink). `nxc6_declare_port_bundle()` segue como wrapper
  legado = auto. Retorno `0` continua benigno por contrato ("o port nao traz
  bundle"): somente `< 0` e erro. FACE_LAYOUT jamais ultrapassa as autoridades
  1/2.
- `nxinput_livedb`: aquisicao limitada do banco vivo (autoridade 2) quando o
  ambiente nao entregou mapping e nenhum `SDL_GAMECONTROLLERCONFIG_FILE` foi
  declarado. Caminhos canonicos `/usr/lib{,32}/gamecontrollerdb.txt`
  escolhidos por fato do processo (largura de ponteiro); symlink morto espera
  no maximo 20 tentativas de 25 ms sob teto absoluto de 500 ms monotonic,
  EINTR-safe; relogio/sleep/fs injetados (testes nunca dormem de verdade);
  snapshot estavel (identidade re-verificada apos a leitura; troca
  modern<->retro durante a espera nunca produz vista mista); loop de symlink,
  FIFO, diretorio, device, oversize e NUL cedem sem bloquear; path explicito
  ilegivel cede sem autorizar busca. Uma vez por admissao/hotplug, nunca no
  frame loop.
- Dominio por PROVA SEMANTICA (contrato 5.6): cada `bN` e interpretado nos
  dois dominios (joydev ascendente e evdev atual) contra o bitmap EV_KEY
  medido; classes `CURRENT_NATIVE`, `LEGACY_JOYDEV_REWRITE`,
  `IDENTICAL_IN_BOTH`, `AMBIGUOUS`, `INVALID`. Exatamente um coerente
  usa/converte; identicos preservam bytes; AMBIGUOUS/INVALID fazem a FONTE
  ceder (`NXINPUT_PM_SOURCE_YIELDS`) -- ambiguidade nunca vira
  NOT_APPLICABLE silencioso. Os marcadores de volume viram evidencia positiva
  forte, nao condicao exclusiva (cobre a linha TrimUI sem teclas de volume).
- Receipt em DOIS sinks (contrato 5.9): alem do arquivo duravel
  `NXC6_RECEIPT`, toda linha vai ao stderr do processo para o log normal do
  port/support bundle. Linhas de announce/bloqueio ganham `name=` (evidencia
  sanitizada via `nxc6_admit_before_announce_named`; patches SDL atualizados),
  `db_class/db_target/db_retries/db_elapsed_ms`, `face_layout=`,
  `effective_guid=`, `map_fnv1a64=`/`map_bytes=` e a linha `NXC6-DOMAIN` com
  as contagens de classificacao. Sem IP, hostname ou path pessoal.
- P7: a composicao existente NAO entrega cursor em aparelho zero-stick (o
  dpad->left-stick age so no estado retornado e o fallback left->right exige
  left stick medido); nasce o opt-in aditivo
  `NXINPUT_CURSOR_OPTION_DPAD_IF_NO_STICK`, somente menu, somente capability
  medida de zero sticks, com release dos latches do D-pad. Nada o habilita
  por default; Tearscape permanece com P7 desligado.
- Gate estatico ampliado: alem de nome/CFW/VID-PID, falha diante de
  `nx_add_generic_gamepad_mappings`, "Generic Xbox Fallback" e literal de
  mapping sintetizado em fonte de producao (quarentena definitiva do 98e051f).
- Estruturas publicadas continuam aditivas: ops/device do seam aceitam os
  tamanhos 0.8.1/0.9.0 explicitamente; consumidores recompilados contra este
  header ganham os campos de cauda.
- Fixture oficial nova `gamecontrollerdb-rg40xx-h-retro.txt` (sha256
  c7732e14f1c78ba1e0c0f24601c15886f9b213b7e9439ee32439afb791cc4016, byte-
  intacta da ROM muOS 2601.1 ja pinada pelo contract-v1) ao lado da modern; o
  runner dedicado `tests/muos_layout_gate.py` roda os cenarios de layout
  contra as tres SDLs reais e julga por trace do consumer + receipts.

# 0.9.0 (2026-08-31, domínio joydev do PortMaster no SDL2/SDL3 atual)

- Corrige o caso real do muOS RG40XX-H sem escolher por CFW, nome ou VID/PID.
  O `control.txt` oficial enumera `EV_KEY` no domínio joydev antigo; SDL2
  2.28.5/2.32.10 e SDL3 3.2.30 usam o domínio evdev atual.
- `nxinput_pm_normalize_source()` só projeta `bN` quando as capabilities do
  event node exato e os marcadores de volume provam positivamente a origem.
  GUID, nome, eixos, hats, metadata, autoridade e política ficam intactos.
- A projeção acontece antes dos gates de sintaxe, alcance, setter e readback.
  Falha recusa a autoridade; nenhum teclado ou fallback cru é inventado.
- O cenário dirigido usa a linha literal e as capabilities extraídas da ROM
  oficial e exige os 18 grupos corretos nas três versões SDL pinadas.

# 0.8.1 (2026-08-31, runtime Godot sobre autoridade universal 0.8.0)

- Adiciona `nxinput_godot_runtime.h`, opt-in e sem dependência de Godot/SDL:
  aliases físicos que apontam para a mesma ação viram OR/refcount, a transição
  nativo→governado espera release/centro e vetores preservam strengths crus
  para `Input.GetVector` aplicar a deadzone configurável da engine.
- Falha de entrega/release ganha lifecycle separado de quit limpo: bloqueia
  health permanentemente, solicita close uma vez e exige status não zero.
- `nxinput_gptk_live_clear_context_checked()` e `is_fatal()` expõem de forma
  aditiva a falha de ACK ocorrida durante releases da API live v1 existente.
- A camada não conhece cenas, ações, pads ou co-op de jogo. O adapter do port
  continua dono dessa política e uma callback bem-sucedida prova somente o
  enqueue na ponte; a ligação ao consumer semântico exige evidência externa.
- Inclui glue Godot 4 source-only parametrizada por descritores do port;
  nenhuma ABI C++ é prometida entre versões da engine.
- Gate dirigido compila com GCC/Clang e cobre alias sobreposto, handoff neutro,
  ausência de deadzone duplicada e fatal após consumo do pedido de close.

# 0.8.0 (2026-08-31, regressão muOS do Nameless Cat 1.2.3: dado real de CFW é admissível)

- **A régua do validador agora é a semântica executada pela SDL, não uma
  régua mais dura que ela.** No muOS, o port morreu com `FATAL: controller
  initialization failed closed` com o banco oficial do CFW SEGURANDO uma
  entrada funcional para o pad: o env do get_controls reprovava por chave
  desconhecida, o gamecontrollerdb reprovava por GUID duplicado divergente e
  o pacote não embarcava bundle — todos os degraus caíam e o fail-closed
  virava a regra em vez da exceção.
- **GUID duplicado num store = a ÚLTIMA linha vence** (o AddMapping da SDL
  substitui a entrada existente ao carregar). A tolerância é CONTADA na
  decisão (`duplicate_lastwins`, membro novo no fim da struct) e aparece no
  recibo (`dup_lastwins=N`, token aditivo). `duplicate-divergent` deixa de
  ser veredito terminal; o valor do enum permanece para compatibilidade.
- **Chave duplicada numa linha = a última ocorrência vence** (a SDL processa
  os campos em ordem e a atribuição posterior sobrescreve). O parser
  deduplica na coleta, então a comparação semântica lê pares EFETIVOS e uma
  duplicata divergente não se esconde atrás da primeira ocorrência.
- **`key:value` desconhecido é metadata**, nunca erro (platform, crc, hint,
  type, `sdk>=` e o que o próximo CFW inventar). Linha que não amarra NADA
  continua reprovada; sintaxe quebrada, ordinal inalcançável e readback
  divergente continuam fail-closed. A ordem de autoridades não muda.
- **Projeção CRC cede à entrada exata**: quando a fonte já tem a linha do
  GUID vivo, a SDL nunca consulta o alias de CRC zero — a projeção não roda
  e a entrada EXATA vence, em vez do fail-closed artificial de
  exata+alias divergentes.
- **Autoridade 3 deixou de ser opcional na prática**: `nxc6_declare_port_bundle()`
  aponta `NXCONTROLLER_PROFILES` para o `controllers.nxb` do gamedir quando o
  launcher não declarou (env já posta nunca é sobrescrita).
- Regressões dirigidas: cenário muOS completo no `test_sovereign.c` (env com
  chave desconhecida vence; banco com duplicata divergente resolve last-wins
  e é contado; env quebrado cede ao banco), `guid_divergent` da matriz C6
  passa de bloqueio a admissão dupla idêntica, e a referência independente
  (`sovereign_reference.py`) decide igual por construção própria.

# 0.7.3 (2026-08-31, fronteira fail-safe do GPTK vivo)

- Acrescenta `nxinput_gptk_live`: a fronteira de runtime começa sem contexto
  provado e deixa o caminho nativo receber o evento até que todos os ACTIONs
  tenham sinks ACK-capable e o adapter selecione um contexto por evidência real
  da engine. Foco GUI ausente, cena desconhecida ou sink faltante jamais viram
  gameplay por omissão nem autorizam supressão.
- O adapter registra sinks escalares/vetoriais, sela a cobertura e só então
  chama `nxinput_gptk_live_set_context`. `null` suprime apenas nesse estado
  pronto; `native` e contexto não provado são passthrough. Uma falha depois de
  invocar o sink é fatal e nunca reproduz o mesmo evento no caminho nativo.
- A troca/limpeza de contexto solta ações latched antes de perder a autoridade.
  O marcador sobe para `nxinput-gptk-runtime/2` e a prova externa usa
  `nxinput-gptk-event-evidence/1`.
- Gate dirigido `run-gptk-live-boundary.sh` cobre contexto desconhecido, sink
  ausente, `null`, ACTION entregue e ACK fatal com GCC e Clang. APIs públicas
  anteriores e ports sem opt-in permanecem inalterados.

# 0.7.2 (2026-08-30, correção física SDL2/dArkOS: GUID com CRC)

- **A mesma identidade, agora também na rota SDL2.** O caso físico do
  GO-Super no dArkOS (Nameless Cat) repetiu na SDL2 o que a 0.7.1 tratou na
  SDL3: GUID ao vivo `1900bb3e4b4800000011000000010000`, banco oficial
  `190000004b4800000011000000010000`. Auditado nas fontes pinadas: a SDL2
  2.28.5 e a 2.32.10 gravam o CRC16 de nome no GUID
  (`SDL_CreateJoystickGUID`) e a própria busca de mapping cai para a entrada
  sem CRC depois de a exata falhar (`SDL_PrivateGetControllerMappingForGUID`).
  A projeção do word CRC deixa de ser exclusiva da SDL3 e vale nas duas
  majors executadas; uma SDL2 anterior à 2.26 entrega word zero e continua
  intocada por construção.
- **Nada mais muda.** Mesmo predicado (bus-form, 28 dígitos idênticos, word
  do banco zero e ao vivo não zero), mesma ordem de autoridades, mesmo
  fail-closed para exata+alias divergentes, receipt `source_crc_aliases`
  inalterado. Nenhum binding é derivado, trocado ou sintetizado.
- **Regressão dirigida.** `test_sdl_seam.c` admite a linha real do dArkOS
  (`leftstick:b14`, `rightstick:b15`, `back:b12`, `start:b13`) na SDL2 e prova
  que R3/L3 existem, SELECT/START ficam, a troca a/b do dono sobrevive e a
  linha é byte-intacta após o GUID; o cenário `crc_alias` da matriz passa a
  ser admitido nas três SDLs reais. Bateria C5/C6 global não repetida.
- **Build.** O alvo `nxinput-gptk` passa a declarar `_POSIX_C_SOURCE=200809L`
  como os demais alvos: `nxinput_gptk_upgrade.c` (C4) usa `openat`/`fstatat`/
  `unlinkat`/`renameat` e `O_CLOEXEC`/`O_NOFOLLOW`, invisíveis num build
  estrito `-std=c11 -Werror` (gate `nxcompat-host`). Sem mudança de
  comportamento.

# 0.7.1 (2026-08-30, correção física SDL3/PortMaster)

- **GUID padrão SDL3 × banco PortMaster.** A validação física no dArkOS
  encontrou o GO-Super com GUID ao vivo
  `1900bb3e4b4800000011000000010000`, enquanto o banco oficial do PortMaster
  identifica o mesmo pad como `190000004b4800000011000000010000`.
  A diferença é o CRC16 de nome que a SDL3 grava nos bytes 2–3 do GUID ao
  vivo e zera antes de consultar bancos de mappings. A costura agora reproduz
  exatamente essa regra para SDL3: preenche o word zerado somente quando os
  outros 14 bytes são idênticos e o GUID tem a forma padrão da SDL. SDL2
  continua byte-exato; nome, modelo, VID/PID e diferenças fora do CRC jamais
  viram aproximação.
- **Fail-closed preservado.** A linha projetada ainda atravessa parser,
  capacidades medidas, setter e readback da C3. Uma entrada exata e outra
  zero-CRC com bindings divergentes colidem e são recusadas; o receipt declara
  `source_crc_aliases`.
- **Regressão real.** A matriz exercita a identidade PortMaster em SDL2
  2.28.5, SDL2 2.32.10 e SDL3 3.2.30. O cenário só pode ser admitido na SDL3;
  fixtures cobrem banco oficial, byte divergente e colisão exata+alias.

# 0.7.0-C6 (2026-08-30, V4-CONTROLLERS-03 / C6 — SDL2, SDL3 e consumers nativos)

> Mesma versão 0.7.0, aditivo sobre a C5B (`1ba4a19`). Nada da C5B foi
> reescrito: a costura do Godot, os patches das engines e a proveniência C5B
> continuam intactos e alcançáveis. A C6 acrescenta a costura equivalente para
> a família SDL.

## O que entrou

- **`nxinput_sdl_seam` — a costura de produção, ligada DENTRO da SDL.**
  Compilada em três bibliotecas SDL reais e chamada de `MaybeAddDevice()`, na
  thread e no lock da própria SDL, **antes de `SDL_PrivateJoystickAdded()`** —
  ou seja, antes do anúncio, antes de `SDL_IsGameController`/`SDL_IsGamepad` e
  antes de qualquer `Open`. Um pad não admitido não é anunciado e some também
  da lista de `SDL_Joystick`. Os patches estão em `engine-patches/`, com a
  cadeia completa em `C6-SDL-PROVENANCE.json`.
- **A ordem soberana da C3, sem reinterpretação.** A costura não decide o
  mapping: ela chama `nxinput_authority_admit()`, que é
  `nxinput_sovereign_resolve()`. Os seis passos foram exercitados **na API
  real**, um cenário cada: `env-get-controls`, `cfw-db-guid`, `port-bundle`,
  `runtime-builtin`, `raw-passthrough` e `fail-explicit`. `env`, arquivo e
  bundle não criam uma segunda ordem local.
- **Staging antes do `SDL_Init`.** `nxinput_sdl_seam_stage_before_init()` copia
  `SDL_GAMECONTROLLERCONFIG` e a **remove** do ambiente antes de a SDL subir,
  porque a SDL a importa com prioridade USER, acima da prioridade API de
  qualquer setter posterior. Os bytes continuam sendo a autoridade 1; deixam
  apenas de contornar a decisão. Staging tardio falha em vez de fingir.
- **`nxinput_sdl` — os domínios ordinais, medidos por pin.** `c6_domain_gate`
  extrai os laços de `ConfigJoystick()` dos quatro pins e os confronta com o que
  o módulo afirma. Resultado: SDL 2.28.5, 2.32.10 e 3.2.30 enumeram um pad
  **identicamente**, então a conversão SDL2/PortMaster → SDL3 privada continua
  estritamente opt-in e **não dispara**. Dois domínios realmente divergem e
  ganharam entrada própria: `joydev-legacy` e `sdl2-legacy-evdev` (a 2.0.10 pula
  a faixa ABS_HAT inteira, não só os pares detectados).
- **Colisão de GUID arbitrada.** A store da SDL é indexada por GUID e a C3
  admite por instância. Bytes idênticos são aceitos; divergentes falham
  fechado. A ordem de chegada nunca decide.

## Correção encontrada durante a C6

A primeira versão da costura exigia readback vivo de **toda** decisão. Isso
tornava a autoridade 5 (raw passthrough) inalcançável, porque raw passthrough
não instala mapping nenhum e portanto não tem o que ler de volta — um passo da
ordem da C3 sumiria em silêncio no dia em que um port legitimamente precisasse
dele. Agora o readback é exigido de toda decisão que **instalou** um mapping, e
o passo 5 é guardado pelo que é específico dele: a declaração do consumer, que a
costura reafirma em vez de confiar. O receipt reporta `readback_checked=0` nesse
caso, honestamente.

## Duas coisas que a C6 mediu e que um port precisa saber

- **A autoridade 4 não responde na SDL3.** Auditado nas fontes pinadas: a SDL2
  carrega o banco embutido em `SDL_JoystickInit()` **antes** do `Init()` dos
  drivers, enquanto a SDL3 inicializa o `SDL_INIT_JOYSTICK` inteiro — a
  enumeração inclusive — **antes** de `SDL_InitGamepads()`. Logo, na fronteira
  do anúncio, o banco da SDL3 ainda não existe. Um port SDL3 precisa declarar
  uma fonte (autoridade 1, 2 ou 3); sem isso a ordem chega ao passo 6 e recusa
  o pad antes do gameplay. Não foi "corrigido" fazendo a costura carregar os
  mappings mais cedo: isso substituiria o fluxo nativo em vez de interceptá-lo.
  `c6_domain_gate` fixa os dois arquivos por SHA-256 e exige a ordem literal.
- **O tipo de saída vem do controle, não do binding.** Uma entrada oficial do
  corpus liga os gatilhos a botões (`lefttrigger:b10`); a SDL continua
  reportando L2/R2 como **eixo** (0 → 32767 → 0). A referência independente
  modela isso e a bateria confere gatilho ligado a botão no caminho de eixo.

## Limites declarados

- `A/B=null` é provado nos **eventos e no polling** de GameController e Gamepad.
  Para `SDL_Joystick` e para raw evdev não declarado, a supressão **por
  controle** é **`UNPROVEN`** e está marcada assim — a costura suprime o
  *device*, não um controle isolado, nessas APIs.
- Os pads da bateria são devices `uinput` **reais** (kernel real, `EVIOCGBIT`
  real, eventos reais), mas compartilham o kernel deste host. São evidência de
  host, nunca prova física: **`PENDING_PHYSICAL`**.
- Engines nativas arbitrárias ficam fora do claim.

## Testes

`tests/run-sdl-c6-host.sh` roda a bateria inteira uma única vez, cada `gate_id`
exatamente uma vez: `provenance` e `domains` (`SOURCE_AUDIT`),
`seam_fail_closed` e `corpus` (`FIXTURE_HOST`), `matrix` (`REAL_API_HOST` —
três bibliotecas SDL reais, 20 cenários cada). `nxinput-sdl-seam` entrou no
`ctest`. As classes do corpus C1 foram **recalculadas** (o número 71 da C3 está
aposentado desde a 114A e não foi reutilizado); os 946 artefatos selados são
lidos integralmente, 17 deles do store pinado do PortMaster-New.

# 0.7.0-C5B (2026-08-29, V4-CONTROLLERS-03 / C5 — correção consolidada 116B)

> Mesma versão 0.7.0. A auditoria externa **reprovou também a C5A**
> (`babee3cd` / marker `V4_CONTROLES_C5_GODOT_REJECTED_AUDIT_116A`). Este é o
> sucessor. Os heads reprovados (`dd312ed`, `7116b323`, `babee3cd`), o marker
> falso, o relatório falso e os logs `C5-FINAL-BATTERY.log` /
> `C5A-FINAL-BATTERY.log` continuam alcançáveis como evidência **rejeitada**;
> nada disso foi reescrito. As seções `0.7.0-C5A` e `0.7.0-C5` abaixo ficam
> marcadas `REJECTED_HISTORY` e não devem ser citadas como prova.

## O que a auditoria recusou, e o que mudou

- **A costura agora está DENTRO da engine.** O que a C5A chamava de
  `REAL_API_HOST` era um consumer chamado FORA do processo, depois que a
  engine tinha saído, com as ops respondidas por um arquivo. A engine estava
  presente, mas nunca foi setter, readback nem announce.
  **`nxinput_godot_seam`** (novo) é compilado e **ligado no binário** de cada
  major e roda dentro de `JoypadLinux::open_joypad()`, no mesmo PID e TID do
  setter, do readback e da decisão, **antes** de `joy_connection_changed()`.
  Um pad não admitido não é anunciado: não chega a `Input.get_connected_
  joypads()`, a InputMap, a `_input`, ao polling nem ao jogo. Os patches das
  duas engines estão versionados em `engine-patches/`, com a proveniência
  completa em `C5B-ENGINE-PROVENANCE.json`.
- **A gramática dos bindings fechou.** A C5A classificava o que não conseguia
  ler como "outro" e **ignorava**: `a:b`, `a:bx`, `a:b3x`, `a:h0`, `a:h0.3` e
  `a:z9` chegavam ao setter intactos, e a MÁSCARA de um hat nunca era lida.
  Agora todo campo é metadado declarado ou um binding que precisa casar
  inteiro (`bN`, `[+-]aN~?`, `hN.M` com máscara em {1,2,4,8}), o hat tem de
  existir na capacidade medida, e `~`/sinal fora de um eixo bloqueiam.
- **`absinfo` medida.** `nxinput_godot_caps_set_absinfo()` carrega as
  respostas do `EVIOCGABS` do fd que pertence àquele joy id, e um eixo sem
  absinfo — ou uma meia-faixa que a faixa do kernel não tem — bloqueia. Sem
  isso a decisão de faixa do SDL não era reproduzível. O membro é aditivo:
  um chamador da API 2 continua válido (e simplesmente bloqueia eixos).
- **O controle positivo virou um oráculo que pode reprovar.** Os dois perfis
  genéricos, reusados para todo GUID e com a contagem multiplicada por perfil
  e por engine ("525/1050"), saíram. `tests/corpus/guid-capabilities.json`
  tem 24 GUIDs, **uma linha cada**, e a capacidade de cada um é o que o
  KERNEL respondeu no nó que criou para um device com aquele
  bus/vendor/product/version. Como as formas de hardware diferem, 16 linhas
  oficiais são legitimamente **recusadas** — um oráculo que não reprova nada
  não é oráculo. `a:b1,b:b0` continua byte-intacto onde é admitido.
  `godot_official_probe.c` e `godot_official_gate.py` foram removidos.
- **A cadeia do dono existe.** O owner-swap não é mais um mapping escrito à
  mão: `c5b_v2_decide` usa o parser e o dispatcher do PRÓPRIO framework sobre
  o NEXTOSCONTROLLERS V2 do dono, e `c5b_v2_to_mapping.py` projeta `null`,
  `native` e a troca de ações na linha SDL, emitindo o mapping e a declaração
  que a costura vai exigir.
- **A matriz cobra o que a engine deve.** A matriz anterior registrava linhas
  e contava; uma linha onde a engine não respondia era indistinguível de uma
  linha certa, e nove passaram assim. Agora cada linha carrega o índice
  lógico que a engine DEVE entregar, derivado do mapping e das tabelas
  `_joy_buttons[]`/`_joy_axes[]` da própria fonte fixada — que **diferem**
  entre as duas majors (`back` é 10 na 3 e 4 na 4; `lefttrigger` é botão na 3
  e eixo na 4). Silêncio reprova.
- **SIGTERM converge no chord.** `nxinput_exit_chord_fold_signal()` (novo)
  faz um sinal de término levantar o MESMO pedido pegajoso que o chord
  levanta, para o port ter uma única finalização e um único save de qualquer
  jeito — e nunca respondendo com tecla sintética.
- **A bateria é one-shot de verdade.** `tests/c5b_attempt.py` cria diretório e
  log com criação exclusiva (recusa se existirem), escreve o manifesto ANTES,
  invoca a bateria UMA vez, conta cada `gate_id` e publica hash, tamanho e o
  status verdadeiro — inclusive no FAIL. A segunda tentativa C5A truncou e
  sobrescreveu o próprio log; isso não pode se repetir por construção.
- **Godot integralmente headless.** Godot 3 usa o binário `platform=server`
  com `JoypadLinux` integrado e flush do buffer de input no loop Server, roda
  com `--no-window` e precisa reportar `OS=Server`/`can_draw=0`, sem
  dependência ELF de X11, Wayland, GTK ou SDL.
  Godot 4 usa `--headless`, `display_server=headless` e zero telas. A matriz
  remove todas as variáveis de GUI; nenhuma sessão de editor ou janela pode
  existir. A tentativa recusa prioridade normal e mais de dois workers; o
  runner continua sequencial.
- **Piloto MMW: `PENDING_RUNTIME_INTEGRATION`** (saída E2). Não está ligado,
  não é executado e não conta como consumer admitido nem como prova runtime.
- **Prova física continua `PENDING_PHYSICAL`.** Um pad de `uinput` é evidência
  de host e nunca prova de aparelho.

# 0.7.0-C5A — `REJECTED_HISTORY` (reprovada pela auditoria 116A→116B)

> **Não usar como evidência.** O que esta seção chama de
> `REAL_API_HOST` era um consumer offline. Mantida só como registro.

> Mesma versão 0.7.0. A auditoria externa **reprovou** o fechamento C5
> (`dd312ed`); este é o sucessor corrigido. O head rejeitado e o log
> `C5-FINAL-BATTERY.log` ficam preservados como evidência histórica.

- **Domínio de origem DECLARADO, nunca inferido.** A versão reprovada decidia
  o domínio olhando o NOME de cada binding e supondo um código evdev canônico
  (`a` teria de ser BTN_SOUTH…). Isso é falso: `a`/`b`/`x`/`y` são funções
  lógicas e um mapping existe justamente para dizer que este pad as coloca em
  outro botão. Um Godot 3.5.3 e um Godot 4.2.2 **realmente executados** neste
  repositório confirmam: com `a:b1,b:b0`, apertar BTN_A faz a engine reportar
  índice lógico 1 e BTN_B reportar 0. A tabela `nome → BTN_*` foi **removida**;
  agora `nxinput_godot_origin_declare()` exige domínio + provider + receipt, e
  origem ausente/desconhecida **bloqueia**.
- **Readback é da ENGINE, não nosso.** O bloco que reparseava a própria saída
  com as próprias funções passou a se chamar `internal_consistency` — é o que
  ele sempre foi. O readback que autoriza anunciar o joypad vive em
  **`nxinput_godot_consumer`** (novo): mede o pad, resolve, entrega ao setter
  real da engine, lê o estado efetivo de volta pela API real, e só então
  anuncia e emite o receipt. Qualquer discordância **bloqueia** e abre o
  doctor. Gate estático impede o adapter puro de voltar a dizer "readback".
- **Botões, eixos e hats.** A conversão deixou de olhar só `bN`: agora cobre
  `aN`/`+aN`/`-aN`/`aN~` e `hN.M`, com capacidade **EV_KEY e EV_ABS** medida.
  Domínios de eixo medidos nas fontes: SDL2 varre `[0, ABS_MAX)`, Godot
  `[0, ABS_MISC)`, ambos pulando hats.
- **Duplicata falha fechada**, inclusive `a:b0,a:b0` — a ordem decidiria.
- **Buffer seguro:** a concatenação passou a usar um `append()` limitado que
  não pode ultrapassar o buffer, em vez de somar retornos de `snprintf`.
- **Classes de prova separadas**, como a auditoria exigiu:
  `godot_domain_gate.py` é **SOURCE_AUDIT** (loops lidos de fontes **fixadas**
  por sha256/commit/licença em `tests/godot-source-pins.json`; caminho
  arbitrário do environment não é autoridade); `godot_real_gate.py` é
  **REAL_API_HOST** (processos reais de Godot 3 e 4, com pad virtual, que
  respondem `get_connected_joypads`, setter, identidade, `InputMap`,
  `_input`/`InputEventJoypad*`, polling e press/release/eixos);
  `test_godot_consumer.c` é **FIXTURE** e foi renomeado internamente para
  falar das rotas do ADAPTER, não das APIs da Godot.
- **Controle positivo sem circularidade:** o probe não reautora mais nenhuma
  linha. Os bytes oficiais são servidos como estão, pareados com perfis de
  capacidade declarados aqui a partir de layouts evdev reais; entrada sem
  oráculo vira `PENDING_NO_CAPABILITIES`. Inclui explicitamente os oficiais
  com `a:b1,b:b0`, que precisam ficar intactos.
- **Fronteira one-shot:** `run-godot-c5a-host.sh` **não** é registrado no
  ctest; a bateria única o chama uma vez e nada o repete.

# 0.7.0-C5 — `REJECTED_HISTORY` (reprovada pela auditoria 116A)

> **Não usar como evidência.** Inferia o domínio pelo NOME do binding e
> chamava de real um controle positivo em C puro. Mantida só como registro.

> Mesma versão 0.7.0 (linha ainda não lançada); continuação do head terminal
> da C4.

- **`nxinput_godot`** (novo, puro): serve o mapping oficial soberano a Godot 3
  e Godot 4 **sem troca extra**. Os domínios ordinais foram MEDIDOS nas fontes
  reais fixadas nesta máquina, não supostos:
  - SDL2 enumera `BTN_JOYSTICK..KEY_MAX` e depois **`0..BTN_JOYSTICK`**;
  - Godot 3.5.3 e Godot 4.2.2 enumeram `BTN_JOYSTICK..KEY_MAX` e depois
    **`BTN_MISC..BTN_JOYSTICK`** — e as duas versões são **idênticas**, então
    um adapter serve as duas.
  Como os dois varrem a faixa alta PRIMEIRO, um binding com código
  `>= BTN_JOYSTICK` tem o MESMO ordinal nos dois domínios: a maioria dos pads
  não diverge e o mapping fica **byte-intacto**.
- **A marca opcional deixou de decidir.** O adapter port-local anterior exigia
  `volumedown`/`volumeup` para acreditar num domínio legado e, sem eles,
  respondia `native` mesmo tendo medido `ignored_low=3`. Agora o domínio é
  decidido por **código evdev canônico e capacidade medida**; as marcas são
  contadas no recibo e nunca consultadas na decisão (gate estático).
- **Ambiguidade bloqueia.** Quando nenhum domínio explica os ordinais medidos,
  ou mais de um os explica de formas diferentes, o resultado é
  `ambiguous-blocked`: a saída é esvaziada, o joypad **não** é anunciado e o
  motivo fica no recibo para o doctor. Nunca um `native` silencioso.
- **Conversão prova readback COMPLETO** antes de anunciar o joypad: cada
  binding da linha convertida precisa resolver, no domínio da engine, para
  exatamente o código evdev que o domínio de origem nomeava. Converter duas
  vezes é no-op (a segunda passagem vê o domínio da engine).
- **Perda de controle é explícita**: um controle CORE que a engine não
  consegue enumerar bloqueia; uma tecla de fornecedor (volume, `misc`) é
  descartada e **contada** no recibo, nunca mantida apontando para outro botão.
- **Recibo novo** `NXINPUT-GODOT-MAPPING-EVIDENCE` com engine, resultado,
  domínio de origem, `keys/godot/ignored_low`, bindings, reescritos,
  descartados, inalcançáveis, readback e marcas — o recibo numérico antigo não
  bastava.
- **Contrato do consumer**: os 18 controles aparecem, capacidade física
  ausente é registrada `reachable=0` (nunca PASS fabricado), e `null` é morto
  nas **três** rotas da Godot (InputMap, `_input`/`InputEventJoypad*` e
  polling `is_joy_button_pressed`).

# 0.7.0-C4 (2026-08-29, V4-CONTROLLERS-03 / C4: NEXTOSCONTROLLERS v2 e `null` real)

> Mesma versão 0.7.0 (linha ainda não lançada); continuação da branch da C3
> corrigida pela auditoria 114A.

- **`NEXTOS_CONTROLLERS/2`**, opt-in por port. A magia sozinha escolhe o
  schema; `/1` continua literal para todo port publicado.
- **Completude**: toda seção V2 lista os 18 controles, cada um uma vez, na
  ordem estável. Campo ausente é `NXI1002` e a mensagem nomeia seção e
  controle. Em V1 a ausência mantém o significado antigo.
- **Tri-state** `ação` / `null` / `native`, com **uma única autoridade**:
  `nxinput_gptk_decide()` responde por controle e por contexto. `null` produz
  `SUPPRESS`, consumido **antes** de qualquer fallback — inclusive antes da
  fonte de fallback estreita. Quase-grafias (`NULL`, `none`, `nil`, `off`,
  `disabled`, `Native`…) falham fechadas com mensagem própria.
- **Gatilhos**: `nxinput_gptk_dispatcher_feed_trigger()` preserva o eixo
  contínuo (vector sink opt-in) e deriva uma borda digital com limiares
  distintos de entrada/saída, sem repetição por frame.
- **Fronteira de lifecycle**: `SELECT=null`/`START=null` suprimem os botões
  para o jogo e não desarmam o chord soberano de saída, que não lê o mapa.
  Gate estático impede o chord de crescer leitura de mapping, L2/R2,
  GUID/layout ou botão substituto.
- **`nxinput_gptk_upgrade`** (novo): um default diferente vira
  `NEXTOSCONTROLLERS.gptk.new` ao lado, atômico e symlink-safe sobre
  descritor de diretório, com diff semântico limitado e sem caminho pessoal.
  O arquivo do dono nunca é sobrescrito, reparado, renomeado ou apagado.
- **Corpus C1 como gate**: os 921 artefatos gptokeyb reais do PortMaster são
  todos recusados como mapa NextOS, e os 73 tokens upstream distintos são
  recusados na posição de controle de um arquivo V2 válido.
- **Receipt de carga**: membro aditivo `selected_gptk_schema` (1, 2 ou 0),
  para que a evidência registre **qual formato venceu** além do hash. O id do
  schema do receipt (`nxinput-gptk-load-evidence/1`) **não** muda — é adição
  compatível, e um leitor anterior à C4 simplesmente ignora o campo, então o
  nxobs não precisou de mudança. O JSON continua sem caminho pessoal.
- **Limite de claim**: tudo aqui é `CORE_HERMETIC`. Adapters de engine (SDL,
  Godot, Android, Unity, touch) são `PENDING_116_119`.

# 0.7.0-C3A (2026-08-29, V4-CONTROLLERS-03 / C3 — correções da auditoria 114A)

> Mesma versão 0.7.0. Esta entrada registra a correção obrigatória exigida
> pela auditoria independente da missão 114A, sobre a mesma branch.

- **A ordem soberana passou a ser o caminho de produção.** Antes, o
  `nxinput_sovereign` decidia apenas nos testes: `nxinput.c` ainda carregava
  `SDL_GAMECONTROLLERCONFIG_FILE` inteiro e aplicava cada linha de
  `SDL_GAMECONTROLLERCONFIG` com o retorno descartado, sem readback, sem
  alcançar o bundle e sem falhar quando nada servia. Esse carregador cego foi
  **removido**. Novo adaptador `nxinput_authority` (`include/nxinput_authority.h`,
  `src/nxinput_authority.c`): reúne as fontes reais, MEDE o pad, injeta o
  setter+readback reais do SDL (`src/nxinput_authority_sdl.c`) e instala a
  decisão. `nxinput.c` não aplica mapping nenhum; um pad que nenhuma
  autoridade serve **não é aberto**, e `nxinput_create()` falha quando há pads
  presentes e nenhum pode ser servido. Gate estático prova que não sobrou uma
  segunda rota.
- **Hotplug explícito.** A decisão morre com o device: desconexão invalida a
  entrada na hora, reconexão é reavaliada pelo GUID e pelas capacidades
  ATUAIS, e nada do device anterior é herdado — inclusive quando o pad novo
  não alcança o mapping do antigo. Dois pads de mesmo GUID têm entradas
  independentes.
- **Ambiguidade falha fechada.** Uma chave de binding repetida na mesma linha
  (em qualquer permutação) e um mapping sintaticamente válido porém sem
  nenhum binding efetivo agora são `syntax-invalid`. O comparador semântico
  não pode mais satisfazer duas chaves duplicadas com uma única ocorrência do
  readback nem ignorar uma binding divergente.
- **A prova diferencial deixou de ser circular.** O driver não emite mais
  veredito: ele publica só o que o consumer/runtime ficou segurando depois do
  setter+readback reais. Quem julga é `tests/sovereign_reference.py`, uma
  implementação independente escrita a partir do contrato do header. A prova
  cobre TODAS as entradas de TODOS os bancos (28.875), não três por banco, e
  o runner recompila o resolver com um binding trocado e exige que o gate o
  **rejeite**.
- **API nova (aditiva):** `nxinput_declare_raw_consumer()` (a autoridade 5 é
  declaração do consumer, nunca palpite) e `nxinput_mapping_authority()`
  (leitura das decisões em vigor). Nenhum campo foi adicionado a
  `nxinput_config`, então nenhum consumidor publicado quebra.
- Documentação corrigida: o README não descreve mais a normalização removida.

# 0.7.0-C3 (2026-08-29, V4-CONTROLLERS-03 / C3: mapping soberano e bundle por CFW)

> Mesma versão 0.7.0 (a linha ainda não foi integrada); esta entrada registra
> a segunda leva de mudanças da frente de controles na branch proprietária.

- **O rewrite pós-load foi REMOVIDO do caminho default.**
  `nxinput_normalize_device_mapping()`, o detector de topologia handheld e os
  quatro helpers exclusivos deles saíram de `nxinput.c`: o mapping herdado do
  PortMaster/CFW agora é aberto byte-intacto. A fixture vermelha da C2
  (contradição x/y contra o GO-Super físico) morre na raiz. Conversores
  opt-in de domínio comprovadamente diferente (ex.: SDL3 PortMaster manager,
  com entrada/saída/consumidor medidos e readback) permanecem.
- **`nxinput_sovereign`** (novo, puro, arquivo separado): a ordem única de
  autoridade — 1) mapping vivo do `control.txt/get_controls`; 2) entrada
  exata por GUID no banco oficial do CFW; 3) bundle `NXCONTROLLER_PROFILES/1`
  pinado no ZIP; 4) built-in do runtime (validado pelo mesmo funil);
  5) passthrough raw somente com declaração do consumer; 6) falha explícita
  antes do gameplay. Cada etapa valida sintaxe -> GUID exato -> capacidades
  medidas -> readback efetivo com identidade semântica; proibições
  garantidas por teste: nada de troca A/B por nome de layout, nada de
  GUIDE->R3 sem prova, nada de segunda normalização, nada de primeira linha
  com GUID divergente, nada de setter sem readback idêntico; duplicata
  divergente do mesmo GUID falha fechado (ordem nunca decide).
- **`NXCONTROLLER_PROFILES/1`** (docs/NXCONTROLLER-PROFILES-V1.md): contrato
  do bundle content-addressed por CFW dentro do ZIP; CFW/versão são apenas
  chave de fornecedor; linhas SDL byte-intactas dedupe por GUID/domínio; zero
  dado pessoal; sem `latest` e sem download em runtime. Builder determinístico
  `tools/nx-controller-profiles.py` (manifesto com fonte, commit oficial,
  licença, cobertura, conflitos excluídos e limites de claim).
- **Gate de corpus integral** (`tests/run-sovereign-corpus-host.sh` +
  driver + orquestrador): todos os 946 artefatos selados da C1 passam por
  parsing/classificação/deduplicação; os 25 gamecontrollerdb rodam o pipeline
  completo com prova diferencial (linha oficial PortMaster = consumer
  positivo × decisão nxinput com readback: byte-intacta E semanticamente
  idêntica); a cadeia física GO-Super fecha o gate. Typos reais do upstream
  são catalogados com teto congelado, nunca escondidos. Unit gates em GCC,
  Clang, ASAN e UBSAN.

# 0.7.0 (2026-08-29, V4-CONTROLLERS-03 / C2: observabilidade completa de input)

- **`nxinput_observe`** (novo, arquivo separado `nxinput::observe`, opt-in por
  link explícito): observador PURO e limitado da cadeia de input. Com sink
  NULL toda chamada é no-op; nenhuma função devolve dado em que uma decisão
  possa se ramificar — telemetria ligada ou desligada produz exatamente as
  mesmas decisões e a mesma sequência de input (provado por replay A/B no
  gate).
- Receipts, todos com `schema=nx-input-observe/1`, run/generation/consumer e
  sequência monotônica:
  - `NXINPUT-LOAD`: fonte (`portmaster-env`/`cfw-file`/`port-bundle`/
    `sdl-builtin`), entradas, hash do mapping (o conteúdo nunca entra),
    GUID pedido/selecionado, prioridade e resultado;
  - `NXINPUT-CAPABILITIES`: contagens/digest de botões, eixos, hats, domínio
    ordinal, teclas baixas, faixa gamepad e eixos analógicos — nunca nome de
    device;
  - `NXINPUT-BINDING`: uma linha para CADA um dos 18 controles canônicos
    (A B X Y L1 R1 L2 R2 L3 R3 START SELECT UP DOWN LEFT RIGHT LEFT_STICK
    RIGHT_STICK), com fonte física simbólica, kind, ordinal, estado
    semântico (`action`/`null`/`native`/`legacy-unmanaged` — a C2 só relata o
    que existe: ausência no GPTK V1 é `legacy-unmanaged`), sink, `reachable` e
    `trigger_kind` obrigatório para L2/R2 (`axis`/`button`/`both`);
    completude provável por `nxinput_observe_binding_missing`;
  - `NXINPUT-CHORD`: par exato SELECT+START (mesma instância) + atestados
    negativos `L2+R2`, `GUIDE+START` e cross-pad = `exit=denied`;
  - `NXINPUT-EVENT`: primeiro press E release por controle por run (bounded),
    dz-exit/dz-enter e extremos por stick, thr-enter/thr-exit e min/max por
    gatilho analógico, sumários; modo diagnóstico com teto rígido
    (`NXINPUT_OBSERVE_DIAG_BUDGET`), excedente só contado — nunca log
    infinito por frame;
  - `NXINPUT-CONSUMER`: nasce SOMENTE no callback/readback que entrega à
    engine; adapter não instrumentado emite `pending/not-instrumented`,
    nunca `delivered` (Godot/SDL/Android/Unity reais ligam nas categorias
    116–119).
- Sanitização fail-closed: caminho, IPv4, hostname e nome livre de aparelho
  nunca chegam a um receipt (`nxinput_observe_sanitize`); só nomes simbólicos
  BTN_/SDL_/ordinais.
- **`nxinput-doctor`** (`tests/nxinput_doctor.c`): matriz 18-controles
  read-only ao vivo (`--evdev`, O_RDONLY|O_NONBLOCK, JAMAIS grab/injeção/
  root obrigatório) e modo hermético `--replay` usado pelo gate; mostra
  never-pressed, press/release, centro/mín/máx dos sticks e L2/R2/L3/R3.
- **Fixture vermelha registrada, comportamento intocado**:
  `tests/corpus/red-portmaster-handheld-rewrite.json` — o rewrite
  `nxinput_rewrite_portmaster_handheld_mapping` contradiz em x/y o mapping
  soberano do PortMaster capturado fisicamente na C1 (GO-Super: `x:b2,y:b3`
  reais × `x:b3,y:b2` do rewrite). A correção pertence às categorias 3/5.
- Gate `tests/run-observe-host.sh`: GCC, Clang, ASAN, UBSAN + goldens +
  auditoria estática de pureza e no-grab. Nenhuma decisão de mapping, formato
  GPTK ou adapter de engine mudou nesta versão.

## 0.6.0 — 2026-08-29 (Framework V4 SDL3/PortMaster opt-in)

- Added a separate, default-off SDL3/PortMaster mapping coordinator. Its
  mandatory pre-init staging API copies and removes
  `SDL_GAMECONTROLLERCONFIG` before any SDL subsystem can import it at USER
  priority. It converts button ordinals only when evdev capabilities prove the
  BB1 legacy ordering and installs the measured rewrite before the guest's first
  `SDL_IsGamepad`/`SDL_OpenGamepad` boundary.
- A native, already converted or unsupported mapping is installed intact from
  staged storage. Rewrite and intact results are cacheable only after semantic
  `SDL_GetGamepadMappingForID` readback. This rejects SDL3's false-success case
  where a higher-priority USER mapping remains active after a successful API
  return.
- Added bounded state for multiple live instances compatible with the same
  PortMaster mapping, retryable transient failures, hotplug invalidation and
  collision reporting for capability-derived rewrites sharing one SDL GUID.
  SDL3 stores an API rewrite by GUID, not by instance ID.
- Heterogeneous, newline-separated `SDL_GAMECONTROLLERCONFIG` lists are now
  supported by `nxinput_sdl3_pm_select_mapping`. A real CFW list carries one
  entry per known device, in several dialects, with blank lines and `#`
  comments, and nothing guarantees the entry this device needs comes first.
  The rule is deliberately conservative, because installing the wrong entry is
  worse than installing none:
  - exactly one entry: used, exactly as before;
  - several entries and one matches this GUID: that entry is used, wherever it
    sits in the list;
  - several entries and none matches: **passthrough**. No SDL call, no cache,
    the guest's own flow untouched. Picking by position is precisely the false
    success this front exists to stop;
  - the same GUID twice with identical bytes: accepted;
  - the same GUID twice with divergent bytes: fails closed (`EPROTO`), because
    the SDL3 store is keyed by GUID and order must never decide the winner;
  - a malformed entry fails closed instead of being skipped: a truncated line
    could be the entry for this very device.
  The receipt gained `staged_entry_count`, so a log states how many entries the
  list really carried.
- Added a consumer-receipt boundary that only the adapter's real guest/engine
  sink may complete. Parser, dispatcher, classification and Open counters do
  not claim gameplay delivery.
- The literal 315-byte muOS/H700 fixture proves A/B/START/BACK
  `b4/b3/b10/b9 -> b1/b0/b7/b6`, preserves axes/hats, rejects double
  conversion and covers transient retry and hotplug.
- This is opt-in framework code. Existing V3 ports and ZIPs remain unchanged;
  every adopter must pin a private SDL3 with Linux evdev and dynamic udev
  discovery, with no `libudev` `DT_NEEDED`, and produce its own discovery
  receipt before classification.
- Preparation is best-effort exactly like the physically approved BB1: a probe
  or registration error is recorded and retried, but never prevents
  the guest's native `SDL_IsGamepad`/`SDL_OpenGamepad` call.

## 0.5.1 — 2026-08-27 (editable controls runtime closure)

- Added the SDL-free, bounded `nxinput_gptk_load_at()` owner/default loader.
  It opens only the fixed `NEXTOSCONTROLLERS.gptk` basename through
  caller-owned directory descriptors, rejects symlinks and non-regular files,
  validates the immutable default before the editable owner, preserves every
  rejected owner byte and uses the default only for the current session. Its
  path-free receipt records source, exact selected/default SHA-256, owner hash
  when safely readable, sizes and stable NXI errors. It never creates a sink.
  The 64 KiB buffer is a single bounded heap allocation (OOM fails NXI1007),
  never port-stack storage; `O_NONBLOCK` makes FIFO/non-regular rejection
  fail-closed before `fstat` without a launch hang.
- Added per-control PRIMARY/FALLBACK source authority to the existing GPTK
  dispatcher. Complete SDL2/SDL3/PortMaster controls mute raw fallback; an
  unowned control observed through both sources is OR-deduplicated, so one
  press produces one delivery and releases only when both observations are up.
  Authority/context changes release latches before clearing source state.
  The new source guard is a separate additive struct; the published dispatcher
  layout and every existing API-1 struct size/offset remain unchanged.
- Added the SDL-version-neutral `nxinput_exit_chord` state/callback API. SDL2
  GameController and SDL3 Gamepad wrappers normalize SELECT/START into the
  same sticky state machine; the raw evdev path remains an independent narrow
  fallback and now exposes a type-free authority hand-off.
- Strengthened the host/install gates for editable fallback, symlink refusal,
  A/B behavior at sinks, delivery_count=1, source deduplication, SELECT+START,
  RIGHT_STICK/R3 cursor behavior, radial deadzone/FPS response, and menu versus
  gameplay ownership without stealing A, D-pad or the camera.

## 0.5.0 — 2026-08-26 (V3-CONTROLLERS-01)

- NEXTOSCONTROLLERS.gptk: formato próprio NextOS (`NEXTOS_CONTROLLERS/1`,
  nunca gptokeyb, nunca vira teclado): parser estrito fail-closed
  (`nxinput_gptk.h/.c` — limites 64KiB/512 linhas, UTF-8 estrito, controles
  simbólicos apenas — código evdev numérico rejeitado, a regressão do Chrono;
  NXI1001..NXI1006) com contextos menu/gameplay/cursor, validação contra a
  allowlist do adapter e dispatcher multi-sink com latches liberados na troca
  de contexto (regressão Action Squad: uma ação lógica alcança todos os
  sinks). Fixture A/B trocados prova a troca chegando ao sink final.
- Tuning universal de cursor/câmera nas seções `[cursor]`/`[camera]`
  (velocidade por altura de drawable/segundo, deadzone radial, curva,
  aceleração por ganho, smoothing por ms, sensibilidade/inversão/authority):
  parser decimal próprio sem NaN/Inf/hex, limites por chave, e cinemática
  delta-time invariante a FPS (30/60/120 dentro de 2%) em
  `nxinput_gptk_motion.h/.c`; `authority=native` deixa o jogo governar.
- Autoridade de A/B: o mapping SDL/PortMaster completo e' SOBERANO (V3-
  CONTROLLERS-01). `nxinput_pad_ordinal_fix_apply` deixava de checar o mapping
  vivo e reescrevia A/B mesmo com um mapping completo exportado em
  `SDL_GAMECONTROLLERCONFIG` -- uma SEGUNDA troca. Agora, com a assinatura
  ordinal casada, o fix DEFERE quando o config exportado ja tem um mapping
  completo para o GUID (funcoes puras novas `nxinput_pad_ordinal_mapping_is_
  complete`, `_config_has_complete_mapping`, `_should_apply`); so' o opt-in
  `<PREFIX>_ORDINAL_FIX=force` sobrepoe. Um mapping built-in nao e' autoritativo.
  Fixture GO-Super `484b:1100` com as DUAS variantes (A=b0/B=b1 e A=b1/B=b0):
  cada variante soberana fica byte-intacta; sem config, a normalizacao legitima
  aplica; com force, sobrepoe. Log da decisao (defer/apply/force) com vid/pid/
  guid. O gate do componente é host; a prova A/B física pertence ao receipt do
  executável final do port e não é herdada de um teste antigo.
- Guarda de dupla leitura POR CONTROLE (auditoria V3, ponto 7):
  `nxinput_gptk_dispatcher_suppressed_mask()` devolve um bitmask com um bit por
  stick que o contexto vivo entrega a `cursor.*`/`camera.*`, e
  `nxinput_gptk_dispatcher_control_suppressed(d, control)` consulta um controle
  só. O adapter suprime EXATAMENTE esses bits, então um jogo que mapeia apenas
  o stick direito à câmera mantém seu stick esquerdo nativo — nada de roubo em
  bloco. O `physical_suppressed()` antigo vira conveniência (`mask != 0`). Prova
  no host (`test_gptk_motion.c`: stick esquerdo NÃO suprimido) e no consumidor
  externo da lib instalada (`consumer_installed_gptk.c`).
- Gate `tests/run-gptk-host.sh` (gcc+clang -Werror) + docs
  `docs/NEXTOSCONTROLLERS.md` bilíngue.

# nxinput changelog

## 0.4.4 — 2026-08-20

- **`nxinput_pad_ordinal_fix.h`: fonte única do ordinal pad fix.** Kernel
  antigo sem driver HID específico enumera os botões pela ordem do report e
  publica `BTN_C`/`BTN_Z`; um mapping autorado em kernel moderno passa a
  apontar para outras posições. O header canônico substitui as 22 cópias
  divergentes espalhadas pelos ports e só troca o mapping com a assinatura
  completa: VID/PID no evdev, **barramento externo** (`BUS_USB` ou
  `BUS_BLUETOOTH`), `BTN_GAMEPAD` e `BTN_C`/`BTN_Z`.
- **Gate `BUS_HOST`.** Pad embutido (`BUS_HOST`, `BUS_I2C`, `BUS_SPI`,
  `BUS_VIRTUAL`) nunca é remapeado. O pad interno do H700 (Miyoo Flip,
  RG40XX-H/RG35XX/RG34XX) publica `BTN_C`/`BTN_Z` e casaria com a assinatura
  antiga — a fixture `framework/tests/fixtures/controls/pad-ordinal-v1.json`
  registra exatamente isso: `signature_ungated=1`, `signature=0`.
- **Identidade fica fora do módulo.** A classe de ordem de report é
  parâmetro do chamador (`NXINPUT_PAD_ORDINAL_LAYOUT_HID` por padrão,
  `..._ALT` para a segunda classe conhecida), com override de campo por
  `<PREFIX>_PAD_ORDINAL_LAYOUT`. Nenhum VID/PID, nome de aparelho ou rótulo
  de firmware entra no nxinput — esse fato pertence ao port ou ao nxcompat.
- O núcleo de decisão não chama SDL nem abre device e é testado no PC pelo
  gate `framework/nxinput/tests/test_pad_ordinal_fix.py` com as tabelas
  evdev reais de cada aparelho.
- Nenhum port é migrado automaticamente; cada um adota no próximo rebuild.
