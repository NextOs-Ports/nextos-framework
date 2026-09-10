# 0.3.5 (2026-09-02, fatal de vídeo sem falso positivo por contenção)

- `nxgl_frame_proof_is_fatal()` deixa de falhar fechado quando a trava do
  adapter está ocupada. Na 0.3.4 o laço de quadros (thread do engine) lia
  "fatal" enquanto a thread de render fazia a leitura legítima do quadro 30
  (glReadPixels de quadro inteiro leva dezenas de ms no Mali-450): o port
  encerrava um jogo saudável com status 72 no quadro 32 com prova de imagem
  100% não-preta no mesmo log (Nameless Cat 1.2.7, dArkOS, 02/09). A flag é um
  int escrito sob a trava e só vai de 0 para 1; a leitura é um load atômico
  acquire, sem trava e sem recusa. A escrita ganha store release.
- Efeito colateral corrigido junto: o poll do laço também ROUBAVA amostras
  legítimas (a amostra do render era recusada por "concurrent/reentrant" quando
  a trava estava com o poll). O harness novo mostra 38-39 de 40 leituras na
  0.3.4 e 40 de 40 na 0.3.5.
- Harness `tests/test_frame_proof_fatal_race.c` (gate v3): duas threads, quadros
  grandes não-pretos sob contenção = zero falsos fatais; depois três amostras
  pretas conclusivas no cronograma before-present (30/120/600) ainda armam o
  fatal uma vez, consumido uma vez e persistente.
- Aditivo: `nxgl_frame_proof_set_video_size(w,h)` (adapter) e
  `nxgl_godot_frame_proof_resize()` (glue Godot) atualizam SÓ a geometria
  registrada no recibo `VIDEO:` quando o compositor reconfigura a janela
  depois do contexto (Wayland/Sway no RP5); a prova por quadro já usava o
  tamanho vivo via before_swap. Sem tamanho positivo ou antes do contexto:
  ignorado/recusado.
- Nenhuma mudança de contrato, marcador ou receipt.

# 0.3.4 (2026-08-31, fatal de vídeo fecha Godot)

- Corrige o defeito em que BLACK/DEAD-CONTEXT conclusivo publicava receipt,
  mas a API `void` deixava o wrapper Godot retornar 0 e o engine continuava
  present e áudio indefinidamente.
- O adapter ganha fatal persistente e close one-shot. O primeiro consumo
  revoga com segurança um health receipt regular 0600 capturado no launch;
  falha de revogação não transforma o encerramento em sucesso.
- O glue Godot v2 retorna `-2` antes do present fatal, bloqueia todas as
  tentativas seguintes, expõe close one-shot, health proibido e exit 72. O
  marcador passa a `nxgl-godot-frame-proof/2` para release não aceitar o glue
  antigo que apenas logava.
- Gates dirigidos provam três BLACK sem quarto present, DEAD-CONTEXT fatal,
  health revogado uma vez e OK sem close/status alterado.

# 0.3.3 (2026-08-31, integração frame-proof para Godot)

- Adiciona glue source-only comum a Godot 3/4 que fixa a ordem launch →
  resolver → contexto → amostra imediatamente pré-swap → publish antes da
  destruição do contexto.
- Rejeita uso pré-contexto, resolver ausente, dimensão inválida, inicialização
  duplicada e chamadas depois do shutdown. O glue não cria janela, quadro ou
  present sintético.
- Documenta patches separados por major e proíbe confundir o frame proof
  before-present com a fronteira gráfica post-first-present.
- Gate dirigido GCC/Clang usa stubs e comprova `LRCPS`, sem janela ou device.
- Corrige os valores GLES de `PACK_SKIP_ROWS`/`PACK_SKIP_PIXELS`; a prova de
  frame passa a consultar exatamente os estados definidos pela especificação,
  sem trocar renderer, contexto ou caminho de pixels.

# 0.3.2 (2026-08-30, fatal video proof)

- Extended the separately linked frame-proof adapter with an atomic private
  `org.nextos.nxruntime.video-proof` receipt. It is bound to the launch's exact
  run, generation and port tuple and is therefore not reusable by another
  launch.
- A positive observation now proves the default framebuffer at an explicit
  before-present boundary. It counts only RGB-nonblack pixels with alpha
  nonzero; the Amlogic failure shape RGB!=0/A=0 is therefore BLACK. AFTER and
  legacy unspecified samples remain diagnostic and cannot authorize a machine
  verdict. BLACK and DEAD-CONTEXT both require three sequential before-present
  observations; manual `publish()` cannot promote one black sample. Remote or
  unknown black remains inconclusive. The VIDEO field layout is unchanged, but
  legacy/AFTER visible samples intentionally change from false `OK` to
  `INCONCLUSIVE/presentation-point-unproved`. Alpha is deliberately the literal
  proven boundary (`0` rejected, `1..255` nonzero), not an unmeasured claim
  about perceptual panel brightness.
- The readback preflight proves GLES state, zero draw/read FBO, zero pack PBO,
  compatible pack row/skip/alignment, bounded stride and at most 64 MiB. The
  initialized-buffer/two-sentinel/guard protocol rejects absent, partial and
  overflowing writes without ever calling `glGetError` or consuming the game's
  pending error. Mutable adapter state has a render-thread guard held across
  measurement, but entry performs one test-and-set with no wait/spin;
  concurrent/reentrant GL callbacks fail closed rather than deadlock.
- The receipt is a real owner-only regular file (`0600`, one link), written by
  exclusive temporary plus atomic rename inside the validated private runtime
  directory. Unsafe paths, tuples, modes and symlinks fail closed. A failed
  fatal replacement actively revokes a prior OK, remains retryable and never
  unlinks an exclusive-temp collision it did not create. The first valid path
  and tuple are copied into immutable process state, so later environment drift
  cannot redirect the fatal away from the original OK.
- Kept video proof separate from generation health: audio, PID liveness, exit
  status and GL context creation cannot create health or override a fatal
  video verdict. Directed host tests cover OK, audio-with-black, fatal
  replacement/irreversibility, dead context, no sample, stale tuple, unsafe
  mode and symlink without a window or device. The real RGBA PNG is exactly the
  first before-present sample eligible for OK and uses its own exclusive temp,
  fsync, atomic rename and directory fsync. A requested PNG failure blocks OK;
  the decoder requires RGB colour and nonzero alpha, so a log-only assertion
  cannot stand in for visible pixels.

# 0.3.1 (2026-08-29, V4-GRAPHICS-04)

- **V4-GRAPHICS-04** (`nxgl_graphics_present_gate.h`): fronteira gráfica
  aditiva **post-first-present**, para backends (Wayland) em que o drawable só
  deixa o placeholder `1x1` depois do primeiro buffer apresentado pelo guest.
  Origem: o BB2 1.0.6 no ROCKNIX — contexto ES 3.1 vivo, shader OK, drawable
  preso em `1x1` **antes** do primeiro present; o wrapper esperava o resize sem
  devolver o controle, o guest nunca apresentava, e o gate reprovava o provider
  e repetia aliases até o runtime sair com status 255. O contrato estava certo
  em recusar `1x1`; o erro era exigir prova final pós-present numa fronteira
  pré-present.
- Máquina de estados **pura** (`src/nxgl_graphics_present_gate.c`), sem SDL,
  GL, clock ou I/O — o adapter mede e injeta tudo, inclusive o tempo
  monotônico:
  `UNINITIALIZED -> REJECTED | AWAITING_FIRST_PRESENT`;
  `AWAITING_FIRST_PRESENT -> REJECTED | PROVED`; `PROVED` e `REJECTED` são
  terminais e estáveis, sem novo receipt e sem recuperação oculta.
  - Preflight valida contrato, API/profile/versão, provider vivo e shader
    probe real, **não** espera resize, não emite health/receipt final; `1x1`
    (ou dimensão já grande) pré-present é somente PENDENTE.
  - `after_present` só é chamado pelo wrapper **depois** do present real do
    guest; o deadline monotônico começa no primeiro present e nunca reinicia
    por frame; só drawable útil observado pós-present promove a `PROVED`, uma
    única vez, com receipt one-shot resistente a dois swaps concorrentes.
  - `1x1` persistente continua `drawable-stuck-1x1`; razões novas estáveis:
    `drawable-unreadable` (0x0/símbolo ausente), `drawable-absurd`,
    `gate-identity-changed` (window/context/provider trocado),
    `gate-misuse`, `clock-unavailable`. Enum estendido só por append; a API
    0.3.0 permanece literal e `nxgl_graphics_drawable_usable(1,1)` continua
    falso.
  - Receipt final = linha `GRAPHICS-EVIDENCE` clássica + campos anexados
    `phase=post-first-present first_present=1 pre_drawable=WxH port_id=…
    port_version=… egl_build_id=…` (o parser antigo ignora chaves extras).
    Linha diagnóstica pré-present separada e não promocional:
    `GRAPHICS-PREPRESENT-EVIDENCE: state=awaiting-first-present final=0 …`.
- Adapter (`nxgl_graphics_contract_adapter.{h,c}`):
  `nxgl_graphics_contract_adapter_pre_present` /
  `nxgl_graphics_contract_adapter_after_present`. A coleta de proveniência e
  identidade GL/EGL foi extraída em helpers compartilhados **sem alterar** a
  saída da API one-shot antiga. Zero `glClear`, zero desenho, zero swap
  sintético; depois de `PROVED` não há chamada de resolver por frame; mutex
  nunca é segurado através de chamada SDL/EGL/GL. Um contexto descartável de
  probe nunca autoriza o contexto real do guest.
- Testes novos: `tests/test_v4_graphics_present_gate.c` (máquina pura,
  timestamps injetados) e `tests/test_v4_graphics_lifecycle_adapter.c`
  (SDL/GL falso com lifecycle Wayland: drawable só materializa no swap real;
  spy de ordem, contadores de clear/draw/swap, SDL2 e SDL3, concorrência,
  API antiga ainda recusando `1x1`). `run-v4-host.sh` roda ambos em GCC,
  Clang, ASAN e UBSAN, além da auditoria estática de pureza e de nomes de
  marca. Ativação somente pelo opt-in declarativo
  `graphics.evidence_boundary=post-first-present` (nxgenerator/nxrelease);
  nenhum launcher ativa por autodetecção de Wayland ou nome de firmware.

# 0.3.0 (2026-08-29, V4-DISPLAY-01 e V4-GRAPHICS-03)

- **V4-DISPLAY-01** (`nxgl_display.h`): módulo puro de apresentação. Dada a
  política declarada, o aspecto interno e o drawable **medido**, devolve o
  content rect, a transformada de apresentação e a **inversa exata** para
  toque/cursor. Sem GL, sem I/O, sem ambiente.
  - Resolve a contradição da especificação V3.1: **ausência de campo é
    `game`/no-op** e preserva exatamente o comportamento V3 de todo port já
    aprovado. `preserve`, com letterbox, é opt-in e nunca default, porque muda
    pixels.
  - Políticas finitas: `game`, `preserve`, `adaptive`, `fill`, `stretch`.
    Nome desconhecido falha fechado. A decisão é sempre por medição — nunca por
    GPU, CFW, device ou nome de jogo.
  - Toque na barra de letterbox/pillarbox é **descartado**, nunca grudado na
    borda. Em apresentação 1:1 ou ampliada o round-trip painel↔conteúdo é
    exato; redução é declaradamente muitos-para-um e o contrato não finge o
    contrário.
  - Recibo `DISPLAY: policy=… internal=… drawable=… content=… letterbox=…`.
- **V4-GRAPHICS-03** (`nxgl_egl_binding.h`): API opt-in que liga os imports EGL
  de um guest Android relocado ao provider que **realmente** possui o contexto
  corrente. Origem: o crash reproduzível do OTR 1.0.3 no ROCKNIX, onde o GLVND
  mantinha `libEGL.so.1` no escopo `RTLD_LOCAL` do SDL, o resolver por
  `RTLD_DEFAULT` não achava nada e os construtores do guest rodavam mesmo
  assim.
  - Identidade pelo resolver SDL + `dladdr`; o candidato abre `RTLD_LOCAL`,
    prova **todos** os imports declarados, prova contexto corrente não nulo e
    prova que é o mesmo objeto do resolver; só então é promovido a
    `RTLD_GLOBAL` e retido. Candidato recusado nunca chega ao namespace global.
  - Tabela explícita entregue ao adapter antes de `init_array`/`JNI_OnLoad`.
  - `nxgl_egl_binding_check_inventory` recusa import EGL do guest que o port
    não declarou. Um contexto, uma surface, um swap; nada de `DT_NEEDED libEGL`
    no executável universal.
  - Recibo estruturado one-shot com path/Build ID do provider, visibilidade,
    imports esperados/resolvidos, identidade do guest e resultado.
  - Todos os efeitos são injetados (`nxgl_egl_dl_ops`), então GLVND split, Mali
    monolítico, caminho já global e todos os negativos são provados
    hermeticamente no host.
- Nova ferramenta `tools/nxgl-egl-audit.py`: lê o ELF diretamente (sem
  `readelf`), classifica os imports EGL indefinidos por `JUMP_SLOT`/`GLOB_DAT`,
  compara com o inventário declarado, recusa `DT_NEEDED libEGL` e confere o
  teto `GLIBC <= 2.30`.
- Novo gate `tests/run-v4-host.sh`: GCC e Clang com `-Werror`, os dois testes
  puros, auditoria de um ELF real construído na hora (inventário exato, import
  não declarado e `DT_NEEDED libEGL`) e auditoria estática que proíbe nome de
  device/CFW/GPU como condição.

# 0.2.17 (2026-08-27) — explicit RED coverage compatibility

- Added the opt-in `NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT=3` without changing
  either existing semantic: `PRESERVE=1` remains the API-1/default behavior and
  rejects an unrepresentable RED legacy fallback, while `ALPHA_MASK=2` keeps
  its native `(ONE,ONE,ONE,RED)` swizzle and fallback bytes `(255,R)` exactly.
- The new semantic preserves the proven Unity/TextMeshPro split: native ES3
  remains `GL_R8`/`GL_RED` with the existing alpha-mask swizzle; a measured,
  explicitly authorized legacy fallback rewrites R8/RED TexImage/TexSubImage
  to `GL_LUMINANCE_ALPHA` and expands every coverage byte as `(R,R)`. The exact
  fixture is `00 7f ff -> 00 00 7f 7f ff ff`, including tracked alignment,
  row length and skip layout.
- Added the bounded contiguous-CPU helper
  `nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous()`. The v2
  layout-aware converter uses it per selected source row. It cannot interpret
  pixel-store/PBO state and does not unquarantine or change the legacy
  `duplicate_r_to_la()` helper.
- Locked the adapter to its planner/observer role: TexStorage availability is
  measured by symbol resolution only. Effectful fake-GL tests prove that
  measurement and storage/image/subimage planning never call, wrap or replace
  `glTexStorage2D`. Selection remains capability-driven with no GPU/device key.
- Existing API/layouts, RGBA/compressed passthrough and all prior defaults stay
  unchanged. No approved port is migrated automatically by this release.

# 0.2.16 (2026-08-27) — opt-in nxloader graphics barrier

- Added the separate, build-time and runtime default-off
  `nxgl-nxloader-provider` archive for nxloader 0.7.2. It installs after the
  port's ordinary providers and before `nxloader_module_resolve`, captures the
  selected originals, and overrides exactly MakeCurrent, GetProcAddress and
  glCreateShader. A successful MakeCurrent must complete the live context,
  drawable and contract-specific shader probe and persist schema-v2 JSON before
  any guest shader is released. No nxloader source, ABI or version changed.
- Provider API 2 takes the release provenance as an explicit adapter-owned
  binding. The SHA-256 is the selected game ELF, not the generation manifest;
  only run/generation/port identity may fall back to nxbootstrap 0.6.32's
  `NXBOOTSTRAP_HEALTH_*` tuple, and a mismatch fails closed. Legacy ambient
  `NX_*` provenance cannot forge or override the JSON receipt.
- Removed the public verify-only graphics-adapter path. Runtime evidence now
  always formats JSON; malformed requested/obtained tuples fail before timeout
  or GL work; SDL3's boolean drawable API is honored; desktop core probes use
  legal core GLSL; every failed compile/link path deletes all created objects.
- Hardened the single-channel opt-in boundary. The legacy context-free planner
  remains linkable but can no longer activate rewrites. The new path requires a
  declared PRESERVE or ALPHA_MASK semantic, stores per-context capabilities,
  tracks bindings per context/thread/unit/target and objects per share-group/
  target/id, tracks UNPACK alignment/row-length/skips and PBO binding, validates
  subrect/type/all size arithmetic, and fails closed on state-table overflow.
  RED, ALPHA and LUMINANCE keep distinct sample semantics; LA CPU conversion
  emits `(255,a)` or `(l,255)` as appropriate. RGBA/compressed/unknown data is
  never rewritten.
- `run-v3-graphics-host.sh` now links the provider against the real nxloader
  registry under GCC and Clang. Named ROCKNIX/Vector Unit BBR1 and BBR2 fixtures
  cover their declared GLES2/GLES3 contracts plus desktop-GL, 1x1 and shader
  negatives. No device was contacted and no port is migrated by this release.

# 0.2.15 (2026-08-26) — V3-GRAPHICS-01

Tudo aditivo sobre a onda v2 (descoberta por medicao, prova de vida GLES1,
frame proof e recibo VIDEO ficam como estavam; linhas historicas intactas,
campos novos so' APENSADOS).

- `nxgl_graphics_contract` (header + fonte novos, V3-GRAPHICS-02, step 1): o
  contrato de CONTEXTO gráfico declarado pelo adapter e validado pelo framework,
  nunca por nome de GPU/CFW/jogo. Um port declara api/profile/version/policy
  (exact|minimum|range)/shader_dialect; o validador puro `nxgl_graphics_
  contract_validate` compara com o CONTEXTO REALMENTE OBTIDO e devolve um reason
  code estável -- o caso Beach Buggy (contrato GLES recebeu contexto desktop GL
  "3.1 Mesa") falha em `desktop-gl-for-gles-contract` ANTES de qualquer shader.
  `liveness=ok` (funções existem, glGetString responde) NÃO prova GLES, dialeto,
  drawable útil nem shader. `nxgl_graphics_drawable_usable` recusa 1x1 (nunca é
  prova de vídeo); `nxgl_shader_source_matches_dialect` confere o `#version` do
  dialeto declarado; `nxgl_graphics_contract_receipt` emite a linha
  `GRAPHICS: requested=.../obtained=.../drawable=.../verdict=.../reason=...`.
  Puro (sem EGL/GL/I/O); o adapter mede e alimenta. Gate host v3-graphics
  `graphics_contract=PASS` (gcc+clang -Werror strict, asan/ubsan). PRÓXIMO:
  receipt runtime real (EGL/GL), gate no nxrelease, fixtures Beach Buggy/FF4,
  camada single-channel e provas físicas Mali-G31+Mali-450.
- `nxgl_graphics_contract_adapter` (adapters/, V3-GRAPHICS-02, step 2): a metade
  que TOCA SDL/EGL/GL, vendorizada no port como o frame-proof adapter (GL
  resolvido em runtime). Mede o contexto REALMENTE obtido -- autoritativo por
  `glGetString(GL_VERSION)`: uma string sem "OpenGL ES" é desktop GL mesmo que o
  port tenha pedido ES (o caso "3.1 Mesa") -- com `SDL_GL_GetAttribute`
  (profile mask) e as client APIs EGL como corroboração, nunca a única chamada.
  Lê o drawable por `SDL_GL_GetDrawableSize` e recusa 1x1. `..._verify()` mede,
  roda o validador PURO e emite o receipt de uma linha; um reason≠OK obriga o
  port a parar ANTES dos shaders. Gate host v3-graphics `graphics_adapter=PASS`
  (fakes de SDL/GL: ES2→OK, "3.1 Mesa"→desktop-gl-for-gles-contract,
  1x1→drawable-stuck-1x1, sem contexto→provider-nominal-only, GLES3 minimum→OK).
- `nxgl_single_channel` (header + fonte novos, V3-GRAPHICS-02, step 5): a
  política de textura single-channel (Alpha8/R8) decidida POR CAPABILITY e POR
  TEXTURA, módulo separado que registra no MESMO receipt gráfico. Rota NATIVA
  (`glTexStorage2D(GL_R8)` + upload GL_RED + swizzle da engine) só quando ES3
  físico, TexStorage, swizzle E GL_RED estão TODOS vivos e medidos; qualquer um
  faltando cai no fallback ESTREITO `GL_LUMINANCE_ALPHA` com o byte duplicado
  (nunca toca RGBA/sampler/shader global). `nxgl_single_channel_coherent`:
  store e upload da MESMA textura têm de casar -- store R8 imutável + upload LA
  = INCOERENTE (o bug do atlas de menu vazio), falha no gate em vez de atlas
  silenciosamente branco. Decisão por medição, nunca por nome de GPU/CFW/jogo.
  Puro (sem GL/I/O); default preservado, adoção opt-in. Gate host v3-graphics
  `single_channel=PASS`. Na 0.2.15 ainda faltava a adoção por um port; no estado
  atual, a prova de release continua exigindo o mesmo artefato final em
  Mali-G31 e Mali-450, sem registrar endereços de aparelho neste changelog.
- `nxgl_quality` (header + fonte novos, auditoria V3 ponto 9): o ciclo
  `quality=low|medium|high` como estado puro e ADAPTER-OWNED. `nxgl_quality_
  parse()` (allowlist do runtime, valor corrupto cai SEGURO em auto),
  `nxgl_quality_resolve()` (auto -> tier recomendado PELO adapter, nunca um
  default global; piso definido em medium), e a maquina de estados
  resolve -> apply -> ready (ordem imposta: sem ready antes de apply) com o
  recibo de uma linha `QUALITY: stage=... requested=... resolved=... applied=...
  ready=...`. Nada aqui interpreta os knobs do engine nem toca EGL/I/O -- o
  adapter aplica o tier e o frame proof do device confirma o `ready`. Gate host
  `run-v3-graphics-host.sh` (gcc+clang -Werror strict, asan/ubsan).
  Escopo fechado da V3: contrato e gate host, sem claim físico/universal de
  aplicação no engine. Adoção e frame proof de tiers ficam por port e não
  bloqueiam a release do módulo opt-in.
- `nxgl_config_request` (header + fonte novos): requisitos de EGLConfig
  declarados POR ADAPTER, nunca globais. `nxgl_config_request_default()` e'
  todo "don't care" — o RGBA8888 do caso Huntdown e' declaracao daquele
  adapter e um teste estatico trava o default para sempre. Matcher puro
  `nxgl_config_satisfies()` (semantica EGL "at least") + recibo de uma linha
  `EGLCONFIG: requested=... observed=... verdict=...` que nomeia o primeiro
  atributo violado.
- `nxgl_retry_contract` (header + fonte novos): o contrato de UMA repeticao
  limpa como maquina de estados PURA — a primeira falha real de KMSDRM/EGL
  autoriza no maximo UM retry; a segunda falha e' TERMINAL (maquina acabada
  recusa qualquer evento; sucesso depois de terminal nao ressuscita).
- Frame proof: registrado ONDE cada amostra foi lida —
  `nxgl_frame_proof_sample_at()` + enum {NXGL_PROOF_BEFORE_PRESENT,
  NXGL_PROOF_AFTER_PRESENT}; `before_present()` registra before-present, o
  `sample()` historico vira "unspecified". O recibo `VIDEO:` ganha o campo
  APENSADO `sample_point=before-present|after-present|unspecified|mixed|none`;
  todos os campos anteriores byte-identicos (gate prova).
- Recibo GLES1 ganha `candidates[]` (campo novo no FIM da struct): rastro
  "nome:razao" NA ORDEM de medicao (live/dead/incomplete/absent). O rastro
  termina no escolhido — candidato posterior ausente do rastro = degrau
  nunca invocado.
- Gate novo `tests/run-v3-graphics-host.sh` (hermetico): cenarios espelhados
  de candidatos (morto ANTES do vivo nos dois sentidos, vencedor por
  medicao, razao de cada perdedor no recibo, saudavel-primeiro sem degrau
  extra); provedor que aceita chamadas sem desenhar reprovado pelo frame
  proof; transicoes do retry contract (varredura exaustiva ate 4 eventos);
  default do config request todo don't-care; e gate estatico de nome de
  device/marca — arquivos novos do V3 sem nenhum token, arvore inteira
  limitada a baseline nao-comentario congelada (candidatos de dlopen por
  NOME DE ARQUIVO provados por medicao, tupla medida do bridge gate e quirk
  declarado pelo chamador; crescer = FAIL).
- CMakeLists: fontes novas no alvo `nxgl`, headers novos instalados, e
  removido o bloco DUPLICADO do alvo `nxgl-gles1` (erro rigido latente de
  CMake em host com GLES/gl.h presente).

## V3-GRAPHICS-02 item 4 — recibo de contexto REAL (aditivo, 2026-08-27)

Tudo aditivo sobre o que já estava em 0.2.15; comportamento e linhas históricas
intactos, adoção opt-in.

- Puro (`nxgl_graphics_contract`): `nxgl_shader_probe_source` gera o VS/FS
  mínimo do dialeto declarado (o texto que o adapter compila de fato — `#version`
  correto + qualificador de precisão); `nxgl_shader_probe_result` nomeia
  pass/compile-failed/link-failed/skipped (SKIPPED NUNCA é pass). Nova evidência
  estruturada `nxgl_graphics_evidence` + `nxgl_graphics_contract_evidence_receipt`
  emitem `GRAPHICS-EVIDENCE: run_id=... generation=... commit=... cfw=... sdl=N
  provider_egl=... provider_gles=... build_id=... requested=.../obtained=...
  drawable=WxH shader_probe=... verdict=... reason=...` — campos MEDIDOS são a
  decisão; procedência é REGISTRADA (texto opaco, nunca decide); vazio imprime "-".
- Adapter (toca GL/SDL): `..._drawable_wait` espera o drawable sair do 1x1 num
  prazo MONOTÔNICO real (`CLOCK_MONOTONIC` + nanosleep + pump), distinguindo o
  driver que reporta 1x1 nos primeiros frames do travado; `..._sdl_major`
  distingue SDL2/SDL3 por qual símbolo de drawable resolve (SDL3 renomeou
  `SDL_GL_GetDrawableSize`→`SDL_GetWindowSizeInPixels`), nunca chamando
  `SDL_GetVersion` com assinatura adivinhada; `..._shader_probe` compila VS+FS
  contra o CONTEXTO VIVO, linka e lê `GL_COMPILE_STATUS`/`GL_LINK_STATUS`;
  `..._evidence` faz a passada completa (mede contexto → espera drawable → probe
  → SDL → procedência do ambiente + build-id do DSO que exporta `glGetString`
  via dladdr→dl_iterate_phdr) e formata o recibo. Contexto contrato PRIMEIRO; um
  contrato GLES cujo shader não linka vira `shader-probe-failed`, nunca OK
  silencioso.
- Gate host v3-graphics: `graphics_contract` e `graphics_adapter` cobrem
  probe-source/evidence/sdl-major/shader-probe/drawable-wait (gcc+clang -Werror
  strict); `brand_name_gate=PASS`.

## V3-GRAPHICS-02 — endurecimento do contrato/adapter (aditivo, 2026-08-27)

Seção 1 da onda de fechamento gráfico; comportamento preservado, testes host verdes.

- Validador puro `nxgl_contract_well_formed` endurecido: todos os enums
  (api/profile/version_policy/shader_dialect) limitados; coerência api↔profile
  (gles⇒es; gl⇒core|compat) e api↔dialeto (gles⇒ESSL; gl⇒glsl-any); RANGE exige
  version_max≥version; timeout 0..60000. Contrato malformado = CONTRACT_INVALID
  ANTES de comparar o contexto obtido.
- Adapter: relógio monotônico agora `int64_t` com overflow verificado e **falha
  imediata se `clock_gettime` falhar** (nunca tratar relógio quebrado como t=0);
  drawable lido por SDL2 (`SDL_GL_GetDrawableSize`) **e** SDL3
  (`SDL_GetWindowSizeInPixels`), com fixture SDL3-only (sdl_major=3); probe de
  shader **SKIPPED nunca é sucesso** — num contexto já casado, SKIPPED vira
  `shader-probe-failed`, só compile+link PASS mantém OK.
- Cobertura host nova: rejeições do validador (8 casos), SDL3-only, SKIPPED-fail.
- **Receipt JSON versionado** (`nxgl_graphics_contract_evidence_json`, schema
  `nx-graphics-evidence`/2): objeto com run_id, generation (espaço p/ SHA-256
  completo), commit, cfw, device, port{id,version}, artifact_sha256, sdl_major,
  provider{egl,egl_build_id,gles,gles_build_id} (build-ids SEPARADOS de EGL e
  GLES), gl{renderer,version,glsl,egl_version}, requested/obtained, drawable,
  shader_probe, verdict, reason. Strings escapadas em JSON (aspas/controle);
  campos vazios viram "". O adapter mede renderer/GL_VERSION/GLSL
  (glGetString) e EGL_VERSION+build-id do EGL (eglQueryString + dladdr em
  eglMakeCurrent). No provider API 2, device/versão/commit/CFW e o SHA-256 do
  ELF selecionado vêm do binding explícito do adapter; apenas run/generation/
  port podem vir do tuple health do nxbootstrap. É o artefato que o nxrelease
  reparseia, RECALCULA o verdict e liga ao ELF imutável. Teste host cobre
  escaping, campos e falha-fechada.

## V3-GRAPHICS-02 item 6 — máquina de estado single-channel (aditivo, 2026-08-27)

O `nxgl_single_channel` deixou de só comparar enums e virou uma máquina de estado
pura, testável e por-textura:

- `nxgl_single_channel_classify(internalformat, format)` classifica RED (GL_R8/
  GL_RED), ALPHA, LUMINANCE ou RGBA; se QUALQUER lado for multi-canal, é RGBA e
  NUNCA é reescrito (textura RGBA/sampler/shader global intocados).
- `nxgl_single_channel_plan(route, kind)` diz o que o adapter faz: rota NATIVA →
  internalformat `GL_R8`, format `GL_RED`, sem duplicação, swizzle A←RED; rota
  FALLBACK → `GL_LUMINANCE_ALPHA` com o byte duplicado em luminância E alfa, sem
  swizzle; RGBA → `handled=0` (passa direto).
- `nxgl_sc_tracker` rastreia por texture-ID a rota do STORE e do UPLOAD;
  `..._note` devolve COHERENT enquanto batem, MIXED no instante em que divergem
  (store R8 imutável + upload LA = o bug do atlas vazio — falha de gate, nunca
  atlas silenciosamente em branco), e OVERFLOW à prova de falha quando a tabela
  fixa enche. `..._forget` limpa o id em delete/reuso. Puro (sem GL/I/O); o
  adapter intercepta glTexStorage2D/glTexImage2D/glTexSubImage2D e alimenta.
- Gate host `single_channel=PASS` cobre classify/plan/tracker (mix, forget/reuse,
  overflow, NULLs) em gcc+clang -Werror strict. Adoção opt-in (default LA).

### Adapter de runtime opt-in (seção 2, 2026-08-27)

`nxgl_single_channel_adapter` (novo, adapters/): a metade de RUNTIME de opt-in.
**Desabilitado por padrão** — todo plano reporta `handled=0` e o chamador faz a
chamada GL original, então adotar o adapter nunca muda uma porta existente até
ela pedir. `..._measure` mede capability real (glGetString GL_VERSION p/ ES3
físico, resolvibilidade de glTexStorage2D, GL_EXTENSIONS p/ swizzle e RED/RG) e
decide a rota uma vez. `..._on_bind`/`..._on_delete` rastreiam o id ligado por
target e limpam id deletado. `..._plan_storage`/`..._plan_upload` classificam o
formato, aplicam o plano puro e marcam a coerência por textura (`coherent=0` no
mix store/upload = bug do atlas vazio); **RGBA/ETC2/ASTC nunca são tocados**.
`..._duplicate_r_to_la` expande R8→LUMINANCE_ALPHA (byte em lum E alfa) com
verificação de overflow. Gate host `single_channel_adapter=PASS` (fake GL,
gcc+clang -Werror -O1): disabled/measure/native/fallback/rgba/mixed/reuse/
duplicate. Ainda precisa do hook de runtime (nxloader/so-loader) para ser
chamado por uma porta — essa integração é a próxima peça.

# 0.2.14 (2026-08-23)

- `nxgl_gles1`: selecao de provedor por CONJUNTO com prova de vida -- o
  `glGetString(GL_RENDERER)` do PROPRIO candidato, com o contexto corrente --
  no lugar da ordem por nome. Caso de campo: no ROCKNIX RK3566 (imagem
  oficial aberta) `libmali.so` e' um blob kbase ORFAO e a Mesa/Panfrost e' a
  viva -- o espelho exato do dArkOS; a cadeia por nome resolvia 47/47 do
  provedor morto e o jogo inteiro virava no-op (tela preta com som).
- `nxgl_gles1_set_primary_resolver()`: o port injeta `SDL_GL_GetProcAddress`
  depois de criar o contexto; a fonte coerente por construcao e' tentada
  antes de qualquer dlopen. Aditivo e opcional.
- Recibo GLES1 ganha `liveness=ok|dead|mixed` e `rejected-dead=N`;
  `nxgl_gles1_liveness()` novo. Sem nenhum candidato vivo, o comportamento
  v1 (primeiro conjunto completo) e' preservado com o recibo DENUNCIANDO
  `liveness=dead` em vez de posar de saudavel.
- Fim da mistura de origens por simbolo no caminho normal: um provedor por
  conjunto. A montagem mista v1 permanece apenas como ultimo recurso quando
  nenhuma fonte tem o conjunto completo.
- Gate hermetico com 7 cenarios, incluindo os espelhos dArkOS (blob vivo tem
  de vencer) e ROCKNIX (a Mesa viva tem de vencer o blob orfao).

# 0.2.13 (2026-08-22)

- Field case, both FF4 ports, dArkOSRE launched FROM THE MENU: the frontend
  unit exports `SDL_VIDEO_EGL_DRIVER=libEGL.so`, the context created under
  that hint reports a NULL renderer (crossed driverless Mesa), and the
  reactive repair detected it and was then refused by the inherited-hint
  guard — black screen with sound on every menu launch. SSH launches (no
  frontend environment) never showed it; the eyes-on menu test did.
- In the REACTIVE path only, a hint that has just been DISPROVEN by live
  measurement — the dead renderer came from the very context that hint
  produced in this process — no longer outranks the repair: the adapter
  saves the hinted values, unbinds them, records the override in the receipt
  and proceeds with the coherent-pair re-exec; if the exec fails the hinted
  values are restored for the post-mortem. The PRE-CONTEXT path is untouched:
  with no live criterion, the CFW's inherited hint remains sovereign
  (scenario 10 still guards it). New scenario 14 proves override + restore.

# 0.2.12 (2026-08-22)

- The 0.2.11 pre-re-exec EGL probe is removed: it refused the GOOD dArkOS
  `-gbm` blob (SIGSEGV regression caught in the physical gate before any
  publication) because gbm-platform blobs return a null display for
  `eglGetDisplay(EGL_DEFAULT_DISPLAY)` even in a clean process — measured on
  the device. The live criterion is now the REAL attempt, per the house rule:
  the re-exec proceeds as in 0.2.10, and the new
  `nxgl_provider_precontext_rollback()` lets the re-executed process unbind
  the applied pair once when SDL's window creation fails again, so the port
  retries through the firmware's normal stack and the final error is the true
  one (FF4 3D/ROCKNIX field case: the orphan blob's "No mali devices found"
  no longer masks the original diagnosis). Without the repair marker the
  rollback never touches the environment, so an inherited CFW hint can never
  be removed by mistake. Scenarios 11–13 cover unbind-once, marker-guard and
  missing-pair; the dead-blob fixture from 0.2.11 is gone with the probe.

# 0.2.11 (2026-08-22)

- The pre-context provider repair now demands a LIVE proof before the re-exec,
  promoted from Swordigo's field-approved `glfix`: after the video teardown,
  the candidate must answer a real `eglGetDisplay` + `eglInitialize` against
  the running kernel. Exporting every symbol is not enough — the field case
  (FF4 3D on ROCKNIX) had a complete orphan `libmali.so.1.9.0` on a Panfrost
  kernel: it passed the dlsym probe, was authorized, and the re-exec traded
  SDL's original error for "No mali devices found" plus exit 1 before the
  game. A dead blob now fails the live proof the same way it would fail the
  game, the repair refuses (`NO_CANDIDATE`), no re-exec happens and SDL's
  original diagnosis is preserved. The matching blob initializes and the
  repair proceeds exactly as before — the dArkOS/NextOS behaviour proven
  physically this week is unchanged, and the reactive (live-renderer) path is
  untouched. New negative fixture `fake_dead_provider.c` and scenario 11 in
  `test_provider_discovery.sh`; the unified fake now carries real EGL entry
  signatures because the adapter actually calls them.

# 0.2.10 (2026-08-21)

- Added `nxgl_gles1`, a runtime resolver for the 47 OpenGL ES 1.1
  fixed-function entry points used by the Square Enix / Matrix Software
  family of ports. It exists because the `libGLESv1_CM` SONAMEs are not
  trustworthy. Measured on an R36S running dArkOS (Mali-Bifrost G31), they are
  crossed: `libGLESv1_CM.so.1` resolves to a 198 KB driverless Mesa, while the
  real 40 MB Mali blob sits behind the *unversioned* `libGLESv1_CM.so` and
  behind `libmali.so`. A binary that binds the versioned SONAME gets a context
  that accepts every call, answers `glGetString(GL_RENDERER)` with NULL and
  draws nothing, while audio and input keep working — a black panel with no
  error. Mali-450 (Utgard) on NextOS/EmuELEC has no such crossing, so the
  failure only appears when a GLES1 port is universalized.
- The resolver removes that `DT_NEEDED`. It resolves from the most coherent
  source first — `RTLD_DEFAULT` (the very objects SDL loaded to create the
  context), then `eglGetProcAddress`, then `dlopen` with the unified-blob names
  ahead of the versioned ones. The order is not cosmetic: field evidence
  gathered for Swordigo (`glfix.c`) shows several dArkOS clones cross their
  SONAMEs, so `libGLESv1_CM.so.1` resolves to a driverless Mesa that accepts
  every call and draws nothing while the real Mali blob sits behind the
  unversioned name. A chain preferring the versioned name would pick the dead
  library and produce a black screen with working audio and input.
- The resolver reports a bounded receipt naming the provider and the first
  unresolved symbol, and never partially succeeds: an incomplete resolution
  returns an error instead of leaving null pointers behind.
- `nxgl_gles1_lookup()` serves the same addresses to a so-loader import
  table, so the Android engine and the port host share one provider.
- Shipped as its own archive (`nxgl::gles1`), with no SDL dependency, so the
  core `nxgl` target is byte-identical for ports that do not use GLES1.
- Added the hermetic gate `framework/nxgl/tests/run_gles1.sh`, covering the
  classic provider, the unified-blob provider and the no-provider failure.
- Registered the `graphics.gles1` capability in the nxcompat registry
  (baseline-graphics, phase `graphics`, source `nxgl`); it was absent, so
  GLES1 ports had no honest capability to declare.

# 0.2.9 (2026-08-20)

- Added a default-off target-ABI SDL video-hint sanitizer for mixed-ABI
  launchers. It must run in the game process before `SDL_INIT_VIDEO`, queries
  the drivers compiled into that process's loaded SDL, preserves a supported
  inherited `SDL_VIDEODRIVER`, removes only an unsupported inherited value,
  and leaves an absent hint to SDL autodetection.
- Added a bounded receipt with the inherited hint, compiled target-SDL driver
  list, action and backend selected after successful SDL initialization. The
  helper never selects KMSDRM, fbdev, Wayland or another backend, never infers
  a device/CFW name, and does not change `SDL_VIDEO_DRIVER`,
  `SDL_DYNAMIC_API`, EGL/GLES provider variables or `LD_PRELOAD`.
- Added hermetic GCC/Clang sanitizer, analyzer, archive-symbol and installed
  link gates for supported, unsupported, absent, malformed, late and busy
  paths. Existing ports remain disabled until their adapter explicitly opts
  in; NXExtract and NXSplash code, renderer, layout and flow are unchanged.

# 0.2.8 (2026-08-18)

- E2: recibo unico `VIDEO:` no frame-proof adapter (aditivo; as linhas
  historicas `gl: frame proof verdict=` e o NXEVENT ficam intactas):
  `window=WxH driver= renderer= gles= frame_proof=% verdict= reason=`.
- Novo `nxgl_frame_proof_set_video_context()` (opcional; sem chamada, campos "?").
- O probe agora drena `glGetError` (um blit 0x506 vira `reason=gl-error-0x506`)
  e registra alpha==0 (`reason=alpha-zero`) e drawable invalido
  (`reason=window-invalid`). Teste: tests/test-video-receipt.sh (5 cenarios).

# nxgl changelog

## 0.2.7 — 2026-08-16

- Added `nxgl_classify_client_array_bridge_v2`. The ROCKNIX g24p0 Wayland blob
  on Mali-G52 loses the VBO association held by a GLES1 client-array descriptor
  and reads the descriptor's byte offset as a CPU address: the game boots
  cleanly, opens audio and input, and dies in the first drawn frame with
  SIGSEGV fault=0x3e inside libmali and status 139. The workaround mirrors
  buffer objects in CPU memory, which is a permanent cost, so the decision is
  gated on the whole driver tuple instead of the vendor.
  Covered by the tuples measured on real hardware: the failing ROCKNIX one
  enables it, while Mali-G31 on KMSDRM, Mali-450 on the mali driver and
  Mali-G310 on KMSDRM all stay on the direct path. The Mali-G310 case matters
  because its version string contains "wayland" while the transport is KMSDRM,
  so matching the version alone would enable the mirror on a healthy stack.
  Physical proof on ROCKNIX hardware is still pending: no such device is
  available here, and the classifier is what can be verified without one.

- Added `nxgl_classify_launch_context_v2` and
  `nxgl_frame_proof_is_conclusive_v2`, so an empty frame only accuses the port
  when the launch could have produced an image. On RK3326/ArkOS the SDL window
  never opens over SSH -- the provider repair takes its pre-context branch and
  every probe reports `eglInitialize failed on this kernel` -- while the exact
  same build launched from the device frontend repairs itself, reaches an
  `OpenGL ES-CM 1.1` context and renders. `ssh -tt`, foreground and a stopped
  frontend do not change that, and `nxsplash` draws either way, which makes the
  harness failure look convincingly like a port defect.
  The verdict is deliberately asymmetric: a drawn frame proves the port draws
  no matter how it was launched; an empty frame from a remote or unknown launch
  is `inconclusive` and demands a re-test from the frontend.

- Added `nxgl_classify_frame_proof_v2`, a pure verdict over frames the caller
  read back: `OK`, `BLACK`, or `UNKNOWN` when no sample exists. A run that
  draws nothing is indistinguishable from a healthy run in every signal a
  launcher has -- the loop ticks, audio plays, input arrives and the process
  exits 0 -- so a black screen was only ever caught by a human looking at the
  panel. Measured on the Swordigo loader after wiring it: NextOS Mali-450
  reports `OK` at 78.6% non-black on an ES-CM 1.1 context; an RK3326 Mali-G31
  handheld reports `BLACK` at 0.0% on an ES 3.2 context, where the fixed
  function pipeline the engine needs does not exist.
- `UNKNOWN` is deliberately not a pass, and the classifier requires more than
  one sample from the caller: a title card can legitimately be black at the
  first reading, and a single sample turns that into a false verdict.
- nxgl still never touches GL state. The caller owns the context and the
  readback; only the decision lives here, matching the existing silhouette and
  provider-recovery classifiers.

## 0.2.6 — 2026-08-13

- Added the separately linked, default-off `nxgl-provider-recovery` adapter
  archive. It consolidates the reusable half of the proven Swordigo/Bomb
  Chicken repair without changing ordinary nxgl startup.
- Added an explicit-candidate probe that requires EGL plus every
  adapter-declared engine GLES symbol to originate from one canonical regular
  DSO. The narrower pre-context mode also requires a real
  `eglInitialize(EGL_DEFAULT_DISPLAY)` and clean `eglTerminate`, and refuses
  to perform either until the adapter attests complete video teardown. A
  terminate failure yields an unusable receipt and retains the provider handle
  rather than unloading code that may still own a live display; it permanently
  blocks every later otherwise-valid enabled probe and authorized re-exec in
  that process.
- Added a fail-closed one-shot re-exec transaction. It accepts only a positive
  existing nxgl authorization plan and an unchanged probe receipt, requires
  completed video teardown, refuses inherited SDL provider overrides, binds
  `SDL_VIDEO_EGL_DRIVER` and `SDL_VIDEO_GL_DRIVER` to the same absolute object,
  and restores all introduced variables if exec fails.
- Kept discovery, engine symbol selection, native lifecycle teardown and the
  decision to opt in adapter-owned. The helper never scans by device/CFW name,
  changes `LD_PRELOAD`, selects a video backend or migrates existing ports.

## 0.2.5 — 2026-08-13

- Extend the pure pre-context recovery authorization to the exact exhausted
  `WINDOW_CREATE/WINDOW_FAILED` pair. Some SDL KMSDRM builds load EGL/GLES
  while creating the window rather than while creating the context.
- Continue to reject mixed stages/reasons, inherited provider overrides,
  non-exhausted ladders, different objects, missing symbols and incompatible
  transports. No environment mutation or retry was added to nxgl.

## 0.2.4 — 2026-08-13

- Make `nxgl_nxcompat_publish_context()` consume the frozen SDL window and
  context from a successful API-v2 SDL/EGL report. API-v1 behavior is
  unchanged; raw-EGL v2 contexts still require an adapter-owned receipt.
- Retain the sanitized backend that successfully started in an API-v2 failure
  report, including failures before a context becomes current.
- Add the pure, fail-closed
  `nxgl_plan_sdl_precontext_recovery_v2()` authorization helper. It never
  scans libraries, loads a provider, mutates the environment, or retries.
- Preserve all API-v1 layouts and numeric values. API v2 remains additive.

## 0.2.3

- Added the pure coherent SDL EGL/GLES provider-pair plan for a real output
  that reached a current context and positive drawable without a renderer.
