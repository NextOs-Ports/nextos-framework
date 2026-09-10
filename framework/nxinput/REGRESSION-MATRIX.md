# nxinput 0.11.1 regression matrix

> Estado 0.10.2 (fechamento V4, 02/09/2026): as linhas `PENDING_PHYSICAL` abaixo
> descrevem a prova física HISTÓRICA por dedo humano, que continua não realizada
> sobre os bytes V4. Ela foi substituída como autoridade de release pela prova
> automática no aparelho (`ON_DEVICE_AUTOMATED_INPUT_PROOF`, seção no fim deste
> arquivo), realmente obtida para os quatro ports da composição terminal. RP5/ROCKNIX
> não recebeu prova alguma (best effort, não verificado).

| Gate | Expected result | Evidence class |
|---|---|---|
| Godot aliases | B+R2 (or any aliases) emit one semantic press and release only after the final physical release | `tests/run-godot-runtime-policy.sh` |
| Godot native handoff | input already held while native stays native through its release/center; only the next gesture may be governed | `tests/run-godot-runtime-policy.sh` |
| Godot analog ownership | raw directional strengths reach the adapter; the engine remains sole owner of its configurable radial deadzone | `tests/run-godot-runtime-policy.sh` |
| Godot fatal lifecycle | delivery/release failure cannot become clean quit or health; close is one-shot and exit remains nonzero | `tests/run-godot-runtime-policy.sh` |
| GPTK checked clear | scalar release failure during context clear is observable and sticky fatal | `tests/run-gptk-live-boundary.sh` |
| Godot source glue | source-ABI template performs enqueue-before-commit and carries no game/scene/SDL policy | `tests/test_godot_runtime_template.py` |

| Gate | Expected result | Evidence class |
|---|---|---|
| C6 seam before announce (SDL2 2.28.5 / 2.32.10, SDL3 3.2.30) | a refused pad is never announced, never classified, never opened, and is absent from `SDL_NumJoysticks`/`SDL_GetJoysticks` too | `REAL_API_HOST` — `tests/c6_matrix_gate.py` |
| C6 sovereign order on the real API | all six C3 steps win in their own scenario: `env-get-controls`, `cfw-db-guid`, `port-bundle`, `runtime-builtin`, `raw-passthrough`, `fail-explicit` | `REAL_API_HOST` |
| C6 all 18 V2 groups, press and release | every group reaches the group the mapping names, in the physical press order; each pressed once and released once | `REAL_API_HOST` |
| C6 axes, hats, triggers | sticks reach both extremes and return to centre; four hat directions; L2/R2 walk a real 0..255 analogue range | `REAL_API_HOST` |
| C6 events vs polling | both paths reported separately, and they agree | `REAL_API_HOST` |
| C6 `A/B=null`, L2/R2 as actions | A and B silent on the event path AND in every polling sample; the other groups unaffected | `REAL_API_HOST` |
| C6 `A/B=null` on `SDL_Joystick` / undeclared raw evdev | per-control suppression is **not** claimed on those APIs | `UNPROVEN`, declared |
| C6 owner-swap | `a:b1,b:b0` makes physical BTN_A report B; the framework does not "correct" the sovereign mapping | `REAL_API_HOST` |
| C6 muOS joydev domain | the literal 315-byte RG40XX-H mapping plus its exact 16-key/six-axis capability set reaches all 18 canonical groups on SDL2 2.28.5, SDL2 2.32.10 and SDL3 3.2.30; the ROM-exact BUS_HOST case composes the zero-CRC alias and ordinal projection in the same admission; physical Start is START, never L1, and L2 remains L2 | `REAL_API_HOST` + `FIXTURE_HOST` |
| C6 domain projection guard | translation requires the exact event-node bitset plus the two volume-key markers; native mappings and unproved capabilities remain byte-identical; receipt counts the changed lines/bindings and both domains | `FIXTURE_HOST` |
| C6 GUID name-CRC identity (SDL3 and SDL2 ≥ 2.26) | a PortMaster zero-CRC GUID is admitted only when every other GUID byte equals the live standard bus-form GUID, on both executed majors; non-CRC differences and divergent exact+alias entries remain fail-closed; the dArkOS GO-Super line keeps `leftstick:b14`/`rightstick:b15`/`back:b12`/`start:b13` byte-intact on SDL2 | `REAL_API_HOST` + `FIXTURE_HOST` |
| C6 native (no declaration) | the seam admits nothing and blocks nothing; stock SDL behaviour is preserved | `REAL_API_HOST` |
| C6 heterogeneous GUID list | each of two pads gets ITS entry out of a list with decoys, in the wrong order | `REAL_API_HOST` |
| C6 same GUID, identical mapping | both instances admitted, independently | `REAL_API_HOST` |
| C6 same GUID, divergent mapping | SDL's last-wins store semantics are reproduced and every resolved divergence is counted; both instances receive the same effective mapping | `REAL_API_HOST` + `FIXTURE_HOST` |
| C6 hotplug | exactly the removed instance is forgotten; the reconnection is a NEW instance, resolved again and never inherited; another live pad keeps its decision | `REAL_API_HOST` + `FIXTURE_HOST` |
| C6 SELECT+START chord | fires exactly once, from the SAME pad | `REAL_API_HOST` |
| C6 L2+R2, and cross-pad SELECT/START | never fire the chord | `REAL_API_HOST` |
| C6 synthetic keyboard | zero keyboard events in every scenario | `REAL_API_HOST` |
| C6 consumer receipt | one receipt per delivery, written by the callback that actually received the state | `REAL_API_HOST` |
| C6 pre-`SDL_Init` staging | `SDL_GAMECONTROLLERCONFIG` is copied and REMOVED before SDL can import it at USER priority; staging after `SDL_WasInit` fails instead of pretending | `FIXTURE_HOST` |
| C6 seam fail-closed ladder | partial ops, unusable device record, refusing setter, hostile readback and undeclared raw all block before the announce | `FIXTURE_HOST` — `tests/test_sdl_seam.c` |
| C6 ordinal domains | the four pinned SDL sources match what `nxinput_sdl` claims; the three executed SDL pins are identical to each other, while a capability-proved joydev source is projected to their evdev domain | `SOURCE_AUDIT` + `REAL_API_HOST` |
| C6 provenance | source → licence → patch → binary, enforced; reversing the seam patch recovers the pinned upstream bytes | `SOURCE_AUDIT` — `tests/c6_provenance_gate.py` |
| C6 corpus classes | all 946 sealed C1 artifacts parsed (17 from the pinned store); classes recomputed, never the retired 71; every class dispositioned | `FIXTURE_HOST` + `REAL_API_HOST` |
| C6 authority 4 on SDL2 | `SDL_GameControllerInitMappings()` runs before the drivers' `Init()`, so SDL's own database answers at the announce boundary and wins step 4 | `REAL_API_HOST` + `SOURCE_AUDIT` |
| C6 authority 4 on SDL3 | `SDL_INIT_JOYSTICK` (enumeration included) completes before `SDL_InitGamepads()`, so SDL3's database cannot answer there; the order reaches step 6 and refuses the pad BEFORE gameplay | `REAL_API_HOST` + `SOURCE_AUDIT` |
| C6 trigger bound to a button | reported on the AXIS path (0 → 32767 → 0), because the output kind comes from the control, not from the binding | `REAL_API_HOST` |
| C6 physical validation | not attempted; the device carries no SDL3 and the uinput pads are host evidence | `PENDING_PHYSICAL` |

| SDL3 heterogeneous list | the entry whose GUID matches this device is selected wherever it sits; comments, blank lines and CRLF are transport noise | `tests/test_sdl3_portmaster_mapping.c` |
| SDL3 list without this device | passthrough: no SDL call, no cache, the guest still classifies and opens | `tests/test_sdl3_portmaster_manager.c` |
| SDL3 same-GUID divergence in the list | fails closed with EPROTO; list order never decides the winner | both SDL3 tests |
| SDL3 malformed list entry | fails closed instead of being skipped | `tests/test_sdl3_portmaster_mapping.c` |
| Owner valid | Safe regular `NEXTOSCONTROLLERS.gptk` wins, exact SHA-256/source recorded | Hermetic host filesystem |
| Owner malformed/unknown action | Owner inode/bytes remain untouched; validated default is selected for this session only | Hermetic host filesystem |
| Owner missing | Default selected, no owner file materialized | Hermetic host filesystem |
| Owner symlink/non-regular | Target is never followed or mutated; NXI1007 and rejected-owner source recorded | Hermetic host filesystem |
| Owner FIFO | Nonblocking open reaches type check, rejects immediately and leaves FIFO intact | Hermetic host filesystem |
| Owner oversized | Read bounded at 64 KiB, NXI1004, no unsafe partial hash treated as selected | Hermetic host filesystem |
| Loader memory floor | One 65537-byte bounded heap buffer; allocation failure is terminal NXI1007 and mapping output stays empty | Source/install gate |
| Default invalid | Fail closed with source `none`; no owner mapping reaches runtime | Hermetic host filesystem |
| Receipt privacy | Schema/source/sizes/hash/error only; no host path, controller identity or mapping contents | Hermetic host |
| A/B edit | Swapped file changes the sink action once, without a second physical-label inversion | Hermetic host |
| Multi-sink | One logical edge reaches each registered sink exactly once | Hermetic host |
| PRIMARY ownership | Complete SDL2/SDL3/PortMaster control suppresses the corresponding raw fallback only | Hermetic host |
| SDL+evdev dedup | Unowned control observed by both sources emits one press and one release after both are up | Hermetic host |
| Authority/context hand-off | Active latches release before source/context state is cleared | Hermetic host |
| Godot scene hand-off | Context resolves before SDL polling; held ACTION/null is suppressed and held NONE/native passes through until release/center; a second transition preserves the first owner and masks never overlap | `tests/test_godot_runtime_policy.c` + port source gate |
| Godot stick aliases/drift | Two sticks on one vector action aggregate by per-direction maximum; neutral alias cannot release held alias; 0.02/0.05 drift releases handoff without shaping delivered vectors; button+stick edge aliases share semantic refcount | `tests/test_godot_runtime_policy.c` + port host/source gate |
| Cursor deadzone/FPS | Radial deadzone and delta-time motion remain invariant at 30/60/120 FPS within 2% | Hermetic host |
| Cursor context | Menu RIGHT_STICK moves cursor and R3 clicks; A/D-pad remain their menu actions | Hermetic host |
| Gameplay context | RIGHT_STICK returns to camera, R3 to declared gameplay action; no cursor action leaks | Hermetic host |
| Per-stick guest suppression | Only cursor/camera-owned stick is masked; other stick, A, D-pad and R3 are not stolen | Hermetic host |
| Neutral exit chord | Same-pad SELECT+START fires after bounded polls, request is sticky, no repeat during one hold | Hermetic host |
| SDL2/SDL3 boundary | Core chord exposes no SDL types; wrappers provide logical state and type-free primary authority | Compile/install + SDL2 host wrapper |
| Independent evdev fallback | Raw fallback stays active only without a complete primary SELECT+START binding | SDL2 virtual/fixture host |
| Installed API | External consumer includes installed loader/dispatcher/motion/chord headers and links only installed `libnxinput-gptk.a` | Hermetic install consumer |
| Existing nxinput API | API 1 layouts, numeric values and legacy `feed()`/SDL2 chord entry points remain unchanged | Component version/compile gates |
| PortMaster 315-byte mapping | Capability-driven conversion rewrites A/B/START/BACK exactly and preserves axes, hats, name and optional fields | Hermetic host fixture from BB1 physical incident |
| Native/already converted mapping | Conversion is a closed no-op; no second ordinal translation | Hermetic host |
| Pre-init staging / USER priority | Mapping is copied and environment hint removed before SDL init; staging after init fails closed and a USER>API false-success cannot produce a receipt | Hermetic host coordinator with priority-aware SDL mock |
| Effective mapping readback | Rewritten and intact staged mappings cache only after all semantic bindings match; name/order/metadata may differ, missing/divergent bindings fail retryably | Hermetic host coordinator |
| Pre-classification order | Every enumerated instance is prepared before the raw `SDL_IsGamepad` and confirmed again before Open | Hermetic host coordinator |
| Transient retry/cache | no-path/open/fstat/ioctl/registration/readback is never cached; verified rewrite and verified intact mapping are bounded | Hermetic host coordinator |
| Hotplug/multiple pads | Adapter-forwarded removal invalidates one instance on the SDL owner thread; independent IDs remain independent | Hermetic host coordinator |
| Same-GUID divergence | Different rewritten button maps sharing one SDL GUID are reported as collision and blocked | Hermetic host coordinator |
| Guest consumer receipt | Receipt advances only after the adapter explicitly confirms delivery at the real engine sink | Hermetic host adapter boundary |
| SDL3 discovery prerequisite | Adopter pins Linux evdev + dynamically loaded udev, forbids `libudev` `DT_NEEDED`, and proves discovery before classification; framework ships no replacement SDL binary | Static positive/negative contract + port release gate |

Physical acceptance remains artifact- and port-scoped. These host gates prove
the reusable mechanism; they do not invent a game sink, advertise a device, or
turn an old ZIP receipt into evidence for a new ELF.

## 0.10.2 — padset e prova automática no aparelho

| Gate | Regressão rejeitada | Evidência |
|---|---|---|
| PADSET união/eixo | união de botões perdida ou eixo de um pad em repouso anulando outro | `tests/run-padset-host.sh` |
| PADSET chord por instance | SELECT num pad + START noutro encerrando; chord legítimo negado; denial em loop de log | `tests/run-padset-host.sh` |
| PADSET hotplug/cap | remoção de um instance fechando os outros; mais de NXINPUT_PADSET_MAX pads | `tests/run-padset-host.sh` |
| PADSET pureza | header SDL, env, dispositivo ou nome dentro do módulo | `tests/run-padset-host.sh` (auditoria estática) |
| PROOF numeração/tabela | bitmask do kernel mal lido; numeração SDL chutada; controle não ligado pelo mapping injetado | `tests/test_device_input_proof.py` |
| PROOF roteiro/janelas | roteiro sem `wait_exit`; movimento fantasma em janela silenciosa; press repetido; saída não-zero; processo restante | `tests/test_device_input_proof.py` |
| PROOF fronteiras | IP literal, uinput fora do agente externo, HOST_FIXTURE como on-device | `tests/test_device_input_proof.py` |

## V4-CONTROLLERS-03 / C2 (0.7.0)

| Gate | Resultado esperado | Evidência |
|---|---|---|
| ON/OFF replay | O mesmo rig de decisão com sink ligado e NULL produz traces byte-idênticos; OFF não emite nada | `tests/test_observe.c` |
| Binding completo | Os 18 controles canônicos sempre presentes; máscara de faltantes provável; L2/R2 declaram trigger_kind | `tests/test_observe.c` |
| Eventos bounded | Primeiro press/release uma vez por controle; repetes só contados; teto rígido do modo diagnóstico | `tests/test_observe.c` |
| Sticks/gatilhos | dz-exit/enter e thr-enter/exit firsts + centro/mín/máx nos sumários | `tests/test_observe.c` |
| Hotplug/2 pads | Receipts por pad nunca misturados; reset re-arma só o pad replugado | `tests/test_observe.c` |
| Chord | Par SELECT+START explícito; L2+R2, GUIDE+START e cross-pad atestados denied | `tests/test_observe.c` |
| Consumer | Não instrumentado = pending/not-instrumented, nunca delivered | `tests/test_observe.c` |
| Redação | Path, IP, hostname, nome livre e mapping cru jamais chegam a receipt | `tests/test_observe.c` + gate estático |
| Doctor read-only | Matriz completa via replay hermético; sem grab, sem escrita, sem eco de path | `tests/run-observe-host.sh` |
| Fixture vermelha | Rewrite pós-load contradiz o mapping soberano capturado (x/y GO-Super); comportamento intocado na C2 | `tests/corpus/red-portmaster-handheld-rewrite.json` |

## V4-CONTROLLERS-03 / C3 (0.7.0)

| Gate | Resultado esperado | Evidência |
|---|---|---|
| Ordem de autoridade | env > cfw-db > bundle > builtin > raw declarado > falha explícita; vencedor byte-intacto | `tests/test_sovereign.c` |
| Rewrite removido | normalize/rewrite/gap-detector ausentes do código; mapping aberto byte-intacto | gate estático em `run-sovereign-corpus-host.sh` |
| Proibições | setter sem readback idêntico recusado; inalcançável nunca vence; raw só declarado. Desde a 0.8.0, duplicata divergente de GUID no mesmo store resolve pela semântica da SDL — última linha vence, contada no receipt (`dup_lastwins`); o fail-closed permanece só quando nenhuma autoridade entrega mapping admissível | `tests/test_sovereign.c` |
| Corpus integral | 946/946 artefatos parseados/classificados/deduplicados; 25 dbs no pipeline completo; TODAS as 28.875 entradas GUID diferenciadas (26.818 admitidas + 2.057 recusadas, 494.530 bindings comparados) contra referência independente; GO-Super físico intacto | `tests/run-sovereign-corpus-host.sh` (corpus selado C1) |
| Caminho de produção | `nxinput.c` não aplica mapping algum; todo pad passa por `nxinput_authority_admit()`; bundle alcançável; recusa antes do gameplay; rota única provada por gate estático | `tests/test_authority.c` + gate estático |
| Hotplug | desconexão invalida a decisão na hora; reconexão reavalia pelo GUID/capacidades atuais; zero herança do device anterior; dois pads de mesmo GUID sem estado cruzado | `tests/test_authority.c` |
| Ambiguidade | chave de binding duplicada (nas duas permutações) e mapping sem binding efetivo falham fechados, na linha, no banco e no readback | `tests/test_sovereign.c` |
| Referência independente | `sovereign_reference.py` decide sozinha vencedor/bindings/capacidades; prova de mutante: resolver recompilado com um binding trocado **tem** de ser rejeitado | `tests/sovereign_reference.py` + `run-sovereign-corpus-host.sh` |
| Bundle | Header NXCONTROLLER_PROFILES/1 obrigatório; builder determinístico com manifesto/licença/claims; leitor serve GUID byte-intacto | `tools/nx-controller-profiles.py` + `tests/test_sovereign.c` |

## V4-CONTROLLERS-03 / C4 (0.7.0) — NEXTOSCONTROLLERS v2

| Gate | Resultado esperado | Evidência |
|---|---|---|
| Completude V2 | toda seção lista os 18 controles uma vez; omissão nomeia seção e controle | `tests/test_gptk_v2.c` |
| Tri-state | `ação`/`null`/`native` por uma única autoridade; quase-grafias falham fechadas | `tests/test_gptk_v2.c` |
| `null` = SUPPRESS | consumido antes de qualquer fallback, inclusive da fonte de fallback estreita; nada latcha | `tests/test_gptk_v2.c` |
| Aceitação do dono | A/B `null` inertes; L2/R2 disparam e soltam uma vez; L2+R2 nunca saem; rebind sem recompilar; log por NOME | `tests/test_gptk_v2.c` |
| Gatilhos | eixo contínuo para vector sink + borda digital com limiar de entrada/saída distinto, sem repetição | `tests/test_gptk_v2.c` |
| Lifecycle out-of-band | `SELECT=null`/`START=null` não desarmam o chord; L2/R2 nunca entram nele | `tests/test_gptk_v2.c` + gate estático |
| Contexto / dois pads | release no contexto ANTIGO; pads independentes; `null` não latcha em nenhum | `tests/test_gptk_v2.c` |
| Dupla leitura | stick `null` não é entregue nem reivindicado pelo framework | `tests/test_gptk_v2.c` |
| Arquivo do dono | nunca sobrescrito; `.new` atômico e symlink-safe; diff sem caminho | `tests/test_gptk_upgrade.c` |
| Corpus C1 | 921/921 artefatos gptokeyb recusados; 73 tokens upstream recusados na posição de controle | `tests/gptk_v2_corpus_gate.py` |
| V1 intacto | `/1` continua literal; ausência decide NONE, nunca SUPPRESS | `tests/test_gptk.c` + `tests/test_gptk_v2.c` |

## V4-CONTROLLERS-03 / C5 (0.7.0) — adapter Godot 3/4 — `REJECTED_HISTORY`

> **`REJECTED_HISTORY`.** Esta seção descreve a entrega C5, **reprovada** pela
> auditoria externa (marker `V4_CONTROLES_C5_GODOT_REJECTED_AUDIT_116A`).
> As classes abaixo estão **erradas** e ficam aqui só como registro: o
> "controle positivo" nunca foi `REAL_API_HOST` (era um programa em C puro
> sobre perfis genéricos, com a contagem multiplicada por perfil e engine) e
> `godot_domain_gate.py` é `SOURCE_AUDIT`, não engine executada. Não usar
> nenhuma linha desta seção como evidência. A entrega válida é a C5B.

| Gate | Resultado esperado | Evidência | Classe |
|---|---|---|---|
| Domínios reais | loops de enumeração re-extraídos de Godot 3.5.3, Godot 4.2.2 e SDL2 batem com o comportamento medido do adapter | `tests/godot_domain_gate.py` | REAL_API_HOST |
| Godot 3 == Godot 4 | as duas versões enumeram igual; um adapter serve as duas | `tests/godot_domain_gate.py` | REAL_API_HOST |
| Mapping nativo | domínio já é o da engine ⇒ byte-intacto, zero reescrita | `tests/test_godot_mapping.c` | FIXTURE |
| Domínio divergente | converte UMA vez, com readback completo; converter de novo é no-op | `tests/test_godot_mapping.c` | FIXTURE |
| `keys=17/godot=14/ignored_low=3` | decisão idêntica COM e SEM as marcas opcionais | `tests/test_godot_mapping.c` | FIXTURE |
| Ambiguidade | bloqueia, esvazia a saída e registra motivo; nunca `native` silencioso | `tests/test_godot_mapping.c` | FIXTURE |
| Controle perdido | CORE inalcançável bloqueia; tecla de fornecedor é descartada e contada | `tests/test_godot_mapping.c` | FIXTURE |
| Controle positivo | 525 mappings oficiais × 2 engines servidos byte-intactos | `tests/godot_official_gate.py` | REAL_API_HOST |
| Contrato de 18 | todos presentes; ausente = `reachable=0`, nunca PASS fabricado | `tests/test_godot_consumer.c` | FIXTURE |
| Vazamento raw | `null` morto em InputMap, `_input` e polling | `tests/test_godot_consumer.c` | FIXTURE |
| A/B → L2/R2 | ações migram sem rebuild; `native` continua chegando | `tests/test_godot_consumer.c` | FIXTURE |
| Dois pads / GUID duplicado / hotplug | independentes, sem estado cruzado nem latch velho | `tests/test_godot_consumer.c` | FIXTURE |
| Chord | SELECT+START do mesmo pad encerra; L2+R2, START sozinho e pads misturados não | `tests/test_godot_consumer.c` | FIXTURE |
| Comparação em aparelho | PortMaster × NextOS no mesmo CFW e mesmos bytes | — | **PENDING** |

## V4-CONTROLLERS-03 / C5A (0.7.0) — Godot real — `REJECTED_HISTORY`

> **`REJECTED_HISTORY`.** A correção 116A também foi **reprovada**. O que ela
> chamava de `REAL_API_HOST` era um consumer chamado FORA do processo da
> engine, depois que a engine já tinha saído, com ops respondidas por arquivo;
> a engine real estava presente mas nunca foi setter, readback nem announce.
> `tests/godot_real_gate.py`, `tests/godot_real_tool.c` e
> `tests/godot_official_*` foram removidos por isso. Nada aqui é evidência.

| Gate | Resultado esperado | Evidência | Classe |
|---|---|---|---|
| Origem declarada | domínio vem de provider+receipt; ausente/desconhecida bloqueia; nunca inferida do nome | `tests/test_godot_mapping.c` | FIXTURE |
| Layout preservado | `a:b1,b:b0` servido byte-intacto nas duas engines | `tests/test_godot_mapping.c` | FIXTURE |
| Botões/eixos/hats | conversão e bloqueio cobrem as três classes | `tests/test_godot_mapping.c` | FIXTURE |
| Duplicata | `a:b0,a:b0` e `a:b0,a:b1` falham fechados | `tests/test_godot_mapping.c` | FIXTURE |
| Buffer | saída pequena recusa em vez de truncar | `tests/test_godot_mapping.c` | FIXTURE |
| Domínios | loops das fontes **fixadas** batem com o comportamento medido | `tests/godot_domain_gate.py` | SOURCE_AUDIT |
| Godot 3 real | processo real responde identidade, setter, InputMap, `_input`, polling, press/release e eixos | `tests/godot_real_gate.py` | REAL_API_HOST |
| Godot 4 real | idem, com o enum lógico próprio da 4 | `tests/godot_real_gate.py` | REAL_API_HOST |
| Readback da engine | admissão só anuncia depois de a engine concordar; discordância bloqueia | `tests/godot_real_gate.py` | REAL_API_HOST |
| `null` na engine | controle não ligado não produz evento, ação nem polling na engine real | `tests/godot_real_gate.py` | REAL_API_HOST |
| Controle positivo | bytes oficiais sem reautoria, perfis independentes, `a:b1,b:b0` intactos | `tests/godot_official_gate.py` | REAL_API_HOST |
| Fan-out do adapter | `null` morto nas três rotas do adapter | `tests/test_godot_consumer.c` | FIXTURE |
| Prova física | comparação PortMaster×NextOS, log físico e 18 controles no aparelho | — | **PENDING_PHYSICAL** |

## V4-CONTROLLERS-03 / C5B (0.7.0) — a costura dentro da engine

Esta é a entrega válida. O callsite está **compilado e ligado dentro do
binário** de cada major e roda em `JoypadLinux::open_joypad()`, no mesmo
PID/TID do setter, do readback e da decisão, **antes** de
`joy_connection_changed`. Um pad não admitido não é anunciado, não chega a
InputMap, `_input` nem polling, e não chega ao jogo.

| Gate | Resultado esperado | Evidência | Classe |
|---|---|---|---|
| Proveniência | fonte→licença→patch→binário casam por hash; binário diferente do pin reprova | `tests/c5b_provenance_gate.py` | SOURCE_AUDIT |
| Fonte pristina | o upstream é recuperado revertendo o patch e bate com o pin | `tests/c5b_reconstruct_pristine.py` | SOURCE_AUDIT |
| Domínios | os loops das fontes fixadas batem com o adapter; Godot 3 == Godot 4 | `tests/godot_domain_gate.py` | SOURCE_AUDIT |
| Origem declarada | domínio vem da decisão C3 (provider da allowlist, geração, GUID, SHA dos bytes); vazio/typo/desconhecido bloqueia | `tests/test_godot_mapping.c` | FIXTURE_HOST |
| Gramática fechada | `b`, `bx`, `b3x`, `b3~`, `+b3`, `~a1`, `h0`, `h0.3`, `h0.16`, chave desconhecida e campo vazio **bloqueiam** | `tests/test_godot_parser_negatives.c` | FIXTURE_HOST |
| Hats de verdade | máscara validada e o hat tem de existir na capacidade medida | `tests/test_godot_parser_negatives.c` | FIXTURE_HOST |
| `absinfo` | eixo sem absinfo medida bloqueia; meia-faixa inexistente bloqueia | `tests/test_godot_parser_negatives.c` | FIXTURE_HOST |
| Controle positivo | 24 GUIDs, **uma linha cada**, capacidade lida do nó do kernel; `a:b1,b:b0` intactos; 16 linhas oficiais recusadas pela capacidade real | `tests/godot_corpus_gate.py` | FIXTURE_HOST |
| Cadeia do dono | launcher → NEXTOSCONTROLLERS V2 → mapping+origem, pelo parser e dispatcher do próprio framework | `tests/c5b_v2_decide.c` + `tests/c5b_v2_to_mapping.py` | FIXTURE_HOST |
| 18 grupos, duas engines | press/release, min/centro/max/deadzone, gatilhos, hat em 4 direções e diagonal, nas três rotas reais | `tests/godot_c5b_matrix_gate.py` | REAL_API_HOST |
| Headless real | Godot 3 `platform=server`, `--no-window`, `OS=Server`, `can_draw=0` e ELF sem GUI; Godot 4 com `--headless`, `display_server=headless` e zero telas; ambiente sem GUI | `tests/c5b_matrix_driver.py` + `tests/godot_c5b_matrix_gate.py` | REAL_API_HOST |
| Ordem e processo | `origin→resolve→setter→readback→announce`, sequência monotônica, um único PID/TID, igual ao PID da engine | `tests/godot_c5b_matrix_gate.py` | REAL_API_HOST |
| `null` | controle suprimido não vaza por `_input`, polling nem InputMap; os outros continuam funcionando | `tests/godot_c5b_matrix_gate.py` | REAL_API_HOST |
| Owner-swap | A/B trocam com L2/R2 pela configuração do dono e a engine entrega os índices trocados | `tests/godot_c5b_matrix_gate.py` | REAL_API_HOST |
| `native` | sem declaração a costura não tira nem acrescenta nada | `tests/godot_c5b_matrix_gate.py` | REAL_API_HOST |
| Dois pads / GUID duplicado | ids e estado separados; nenhum nó anunciado duas vezes | `tests/godot_c5b_matrix_gate.py` | REAL_API_HOST |
| Hotplug / reconnect encolhido | pad depois do boot é admitido; reconexão com capacidade menor é **recusada**, sem herdar a geração anterior | `tests/godot_c5b_matrix_gate.py` | REAL_API_HOST |
| Chord | SELECT+START do mesmo pad encerra; sozinho, GUIDE+START, L1+R1, release no meio e pads misturados **não** | `tests/godot_c5b_matrix_gate.py` | REAL_API_HOST |
| SIGTERM | mesmo save/lifecycle do chord, uma única finalização, zero Escape/Enter sintético | `tests/godot_c5b_matrix_gate.py` | REAL_API_HOST |
| Bateria one-shot | diretório e log exclusivos, cada `gate_id` exatamente uma vez, hash publicado inclusive no FAIL | `tests/c5b_attempt.py` | — |
| Piloto MMW | `PENDING_RUNTIME_INTEGRATION` (saída E2): não ligado, não executado, não é consumer admitido | `ports/mmw/v4-input/pilot-gate.sh` | PENDING_RUNTIME_INTEGRATION |
| Prova física | 18 controles no aparelho, comparação PortMaster×NextOS, log físico | — | **PENDING_PHYSICAL** |

| Gate | Expected result | Evidence class |
|---|---|---|
| V3 FACE_LAYOUT parser | `NEXTOS_CONTROLLERS/3` requires exactly one lowercase `FACE_LAYOUT = auto\|modern\|retro`; N01-N09/N26/N27 of the sealed V4-CTRL-01 oracle fail closed; V1/V2 stay byte/semantics-identical and mean auto | `FIXTURE_HOST` — `tests/test_gptk_v3.c` |
| GPTK pre-init boundary | owner/default map and layout read EXACTLY once before bundle declare/staging/SDL_Init; rejected owner falls back to the default AND its layout | `FIXTURE_HOST` — `tests/test_gptk_preinit.c` |
| Live-database bounded wait | dead canonical link waits at most 20x25 ms under a 500 ms monotonic ceiling with injected clock (no real sleep); declared-path misses yield without search; unsafe paths yield with zero sleeps; snapshots are identity-stable | `FIXTURE_HOST` — `tests/test_livedb.c` |
| Seam live-db integration | an acquired snapshot enters the order as authority 2 only; a live env mapping skips the acquisition; a yielded acquisition leaves the ladder without step 2 and the receipt carries db_class/db_target/db_retries/db_elapsed_ms/face_layout | `FIXTURE_HOST` — `tests/test_sdl_seam.c` |
| Semantic domain classification | CURRENT_NATIVE/LEGACY_JOYDEV_REWRITE/IDENTICAL_IN_BOTH byte-rules hold; AMBIGUOUS/INVALID make the SOURCE yield (never a silent pass); the volume-less legacy line converts by proof | `FIXTURE_HOST` — `tests/test_portmaster_domain.c` |
| muOS layout authority (real SDLs) | same uinput pad + GUID follows the modern/retro symlink in two runs; env beats db and bundle; live db beats an opposite variant; FACE_LAYOUT variants serve only as authority 3; auto + empty ladder is a receipted fail-closed block, and the announce receipt carries the 0.10.0 evidence fields | `REAL_API_HOST` — `tests/muos_layout_gate.py` |
| P7 zero-stick cursor opt-in | `CURSOR_DPAD_IF_NO_STICK` engages only with the option bit + menu context + measured D-pad + zero sticks; any stick or gameplay disables it; opt-in off preserves prior behaviour bit for bit | `FIXTURE_HOST` — `tests/test_cursor_dpad_p7.c` |
| Generic-fallback quarantine | production sources carry no `nx_add_generic_gamepad_mappings`, no "Generic Xbox Fallback" string and no synthesized mapping literal | `SOURCE_AUDIT` — `tests/static_no_device_name_fallback.sh` |

| Gate | Expected result | Evidence class |
|---|---|---|
| ON_DEVICE_AUTOMATED_INPUT_PROOF — Nameless Cat 1.2.7 (`b8c8be2d…`, geração `27e8958c…`) | menu, gameplay e owner-remap (A=null, R2=player.jump): 3/3 `all_pass`, 3 clones uinput criados antes do SDL do jogo, hotplug, chord cross-pad negado, SELECT+START status 0; lock `nx-input-proof-lock` (9 casos) | `ON_DEVICE_AUTOMATED_INPUT_PROOF` — dArkOS K36S |
| ON_DEVICE_AUTOMATED_INPUT_PROOF — Freedom Planet 2 1.1.3 (`5af66726…`, geração `61dd42aa…`) | idem (owner A=null, R2=fp2.attack); 9 casos | `ON_DEVICE_AUTOMATED_INPUT_PROOF` — dArkOS K36S |
| ON_DEVICE_AUTOMATED_INPUT_PROOF — Blossom Tales 1.4.0 (`49e52b36…`, geração `c5ce3e3b…`) | idem (owner A=null, R2=player.attack); 11 casos | `ON_DEVICE_AUTOMATED_INPUT_PROOF` — dArkOS K36S |
| ON_DEVICE_AUTOMATED_INPUT_PROOF — Tearscape 0.2.16 (`7b1254cd…`, geração `eaa12f42…`) | idem (owner A=null, R3=player.roll; LEFT_STICK por borda nas 4 direções + diagonal); 15 casos | `ON_DEVICE_AUTOMATED_INPUT_PROOF` — dArkOS K36S (fbdev) |
| Prova física por dedo humano sobre os bytes V4 | não realizada; não é requisito de release desde a decisão do NextOS de 02/09/2026 | `PENDING_PHYSICAL` (histórico, não bloqueante) |
| RP5/ROCKNIX (Wayland) | nenhuma prova; implementação preservada como `BEST_EFFORT_UNVERIFIED` | `UNPROVEN`, declarado |

## V5 (0.11.0) — controls line, host + ON_DEVICE (authorized devices only)

| gate | class | what it proves | status |
|---|---|---|---|
| `tests/v5/test_v5_seam.c` (0.11.1) | FIXTURE_HOST | UNKNOWN provider = STOCK admit (never mute), HIDAPI admitted native, bundle never under UNKNOWN, staging left for stock, legacy staging reinstates the CFW env line | PASS |
| `tests/v5/test_v5_provider.c` (0.11.1) | FIXTURE_HOST + host probe | ById presence never infers a domain; MEASURED_INPROCESS match/conflict; runtime pin file; DSO never static; sdl2-compat detected | PASS |
| `tests/v5/test_v5_translate.c` (0.11.1) | FIXTURE_HOST | guide on KEY_MENU coherent; lone-half hat per domain (p009) | PASS |
| `tests/v5/test_v5_prerouter.c` (0.11.1) | FIXTURE_HOST | no chord from a released tap; double tap; press-time flush order; overflow visible; physical graph owner/observer; uncertified SELECT | PASS |
| `tests/v5/test_v5_padset.c` + `tests/test_padset.c` (0.11.1) | FIXTURE_HOST | pre-router wired (START retained, one exit, generation), instance race, calibrated vector | PASS |
| `tests/v5/test_v5_lifecycle.c` (0.11.1 C5) | FIXTURE_HOST | reopen refused, quarantine definitive, hotplug generation, remap during hold | PASS |
| `tests/v5/test_v5_gptk_live_vector.c` | FIXTURE_HOST | vector gesture edge with neutral floor; floor 0 + centre +0.0039 reopens the bug (killed) | PASS |
| `tests/v5/test_v5_gptk4_preinit.c` | FIXTURE_HOST | universal schema-4 pre-init; symlinked dirs refused | PASS |
| `tests/v5/test_v5_authority_v5.c` | FIXTURE_HOST | nextos/engine/synchronized executable; SYNCHRONIZED without hooks refused; ENGINE has no editable owner | PASS |
| `tests/v5/test_v5_keyboard.c` | FIXTURE_HOST | keyboard → actions via the shared router; event == polling; chord/modifier; loop refused (parser + source) | PASS |
| `tests/v5/test_v5_route_policy.c` | FIXTURE_HOST | one policy before every setter; legacy routes opt-in only | PASS |
| `tests/v5/test_v5_godot_seam_ascending.c` | FIXTURE_HOST | Godot native seam translates a declared ascending line; V4 import (as high-first) blocked by readback | PASS |
| `tests/v5/test_v5_coexist.c` | REAL_DSO_HOST | sdl2-compat = shared core (host); classic SDL2 2.32.10 + SDL3 = separate stores, per-handle symbols, sequenced corpus, one physical owner | PASS (both cases) |
| `tests/v5/test_v5_chain.py` + `chain_harness` | ORACLE_HOST | stimulus → provider → owner → route → sink → prompt → release; 8 mutants killed | PASS |
| `tests/v5/fuzz_v5_lifecycle.c` | SANITIZER_FUZZ | lifecycle/pre-router/router/keyboard invariants under ASan/UBSan, 0 hits (found: quarantine without release) | PASS |
| `tests/v5/test_v5_gptk4_host_gate.py` + `nx-gptk4-host-gate` | FIXTURE_HOST (M1c) | Tearscape schema-4 owner 15/15; /1 owner, closure/owner divergence, kind mismatch, extra binding killed | PASS |
| `tests/v5/run-unpinned-provider-oracle.sh` (host) | PROVED_STOCK_UNDER_UNKNOWN_PROVIDER | unpinned libSDL2 (bytes appended): provider unknown, left for stock / legacy reinstated, pad delivers stock semantics, 25/25 × 2 | PASS |
| same, device secondary (dArkOS K36S, `d3b29f57` unpinned copy) | ON_DEVICE PROVED_STOCK_UNDER_UNKNOWN_PROVIDER | `receipts/device-oracle-darkos-k36s-0.11.1-unpinned-stock*.json` 25/25 × 2 | PASS |
| pinned providers, 0.11.1 glue: primary (1ac99b5c) + secondary (4fd539cd) | ON_DEVICE / PROVED_PROVIDER | both fixtures 23/23 each per device; `NXC6-PROVIDER` in the durable receipt | PASS |
| SDL2+SDL3 on the primary device | ON_DEVICE | `receipts/device-coexist-nextos-elite-4.9-0.11.1.log`: the device's SDL2 2.32.71 is sdl2-compat over SDL3 3.5.0 → shared core (isolation there is not a two-provider case) | PASS (shared-core) |
| HIDAPI by uhid (H2a) | INCONCLUSIVE FOR NOW `[!]` | uhid path proven on the secondary device (generic HID pad → hidraw0 + evdev, SDL opens, A/B delivered); DS4-class needs a pinned real report descriptor (hand-made one refused by hid-sony) | OPEN |
| `tests/v5/test_v5_incident_red.c` | FIXTURE_HOST | V4 corrupts the muOS and Knulli lines (15/2, 13/0; START→L1, back→Y) | REPRODUCED |
| `tests/v5/test_v5_provider.c` | FIXTURE_HOST + host probe | evidence order, UNKNOWN safe, majors separate, sha bound to mapped object, KATs | PASS |
| `tests/v5/test_v5_provider_pins.py` | SOURCE_AUDIT | compiled pins == manifest; patches byte-pinned | PASS |
| `tests/v5/test_v5_translate.c` | FIXTURE_HOST | byte-intact on ascending provider, rewritten on upstream, round trip, UNKNOWN untouched, ABS_MISC, signs | PASS |
| `tests/v5/test_v5_seam.c` | FIXTURE_HOST | UNKNOWN admits native, receipt domains, same-GUID refused pre-setter, hostile readback refused | PASS |
| `tests/v5/test_v5_padset.c` | FIXTURE_HOST | vector from one instance; overflow visible | PASS |
| `tests/v5/test_v5_axis_calib.c` | FIXTURE_HOST | matrix 5.5 + 6.5 directions/triggers | PASS |
| `tests/v5/test_v5_gptk4.c` | FIXTURE_HOST | schema 4 completeness, grammar, modes, keyboard, remap A→R2 | PASS |
| `tests/v5/test_v5_route.c` | FIXTURE_HOST | single route, refcount, companions, lease, release-all | PASS |
| `tests/v5/test_v5_registry.c` | FIXTURE_HOST | Xbox default, epochs, remap prompt, modality debounce, raw-ordinal gate | PASS |
| `tests/v5/test_v5_decision.c` | FIXTURE_HOST | 1.4 machine: BYTE_INTACT only under integral equivalence (7 divergent fields killed), UNKNOWN → no setter/store, NOT_APPLICABLE → ROUTE_TYPED_DIRECT, forced SDL table refused, edge owner | PASS |
| `tests/v5/test_v5_corpus.c` | FIXTURE_HOST | corpus precedence, matching=2 collapse, platform filter, divergent duplicate refused pre-store | PASS |
| `tests/v5/test_v5_prerouter.c` | FIXTURE_HOST | START/SELECT pre-router: chord both orders/simultaneous, taps/holds once, cross-pad, stale generation, focus/unplug, auto-repeat | PASS |
| `tests/v5/test_v5_lifecycle.c` | FIXTURE_HOST | admit/open race, neutral gate, unknown/loading during hold, focus/unplug held, reconnect generation, hot reload, synchronized CAS/conflict/rollback | PASS |
| `tests/v5/mutation/run_mutants.py` | ORACLE_MUTATION | V4 tool: 2 survivors (circularity); V5 oracle + C machines: 46 killed, 0 survivors | PASS |
| `tests/v5/run-host-oracle.py` (host 2.32.70) | PROVED_SOFTWARE_CONSISTENCY | both incident fixtures, 21 checks each, real SDL2 + glue + uinput | PASS |
| same, dArkOS K36S (4fd539cd) | ON_DEVICE / PROVED_PROVIDER | high-first provider identified by bytes; both fixtures 21/21 | PASS |
| same, NextOS Elite 4.9 (1ac99b5c) | ON_DEVICE / PROVED_PROVIDER | private fork MEASURED high-first; both fixtures 21/21 | PASS |
| same, EXACT Knulli 20250813 DSO (8c4dc956, 2.30.12 patched) on the authorized aarch64 device | PROVED_PROVIDER_BY_FIXTURE | Blossom/Knulli fixture 21/21, `native=1` byte-intact, rewritten=0 (image re-downloaded, sha256 87f886e0 verified) | PASS |
| same, EXACT muOS 2601.1 DSO (40d0616f, 2.28.5 patched) on the authorized aarch64 device | PROVED_PROVIDER_BY_FIXTURE | FP2/muOS and Nameless Cat/muOS fixtures 21/21 each, `native=1`/`native=2` byte-intact, rewritten=0 (mission §10 fixture path; DSO measured ascending + ById) | PASS |
| Knulli / muOS physical handheld | INCONCLUSIVE FOR NOW (no device) | kit `V5-CONTROLES-KIT-EXTERNO-KNULLI-MUOS`; providers now PROVED by fixture (Knulli 8c4dc956, muOS 40d0616f) | OPEN |
| HIDAPI/hidraw, touch, engine prompt hooks, pilots | NOT CLAIMED | see handoff | OPEN |
| Pilot FP2 1.1.4 (Unity/IL2CPP, schema 4, owner runtime) — dArkOS K36S 4fd539cd | ON_DEVICE_AUTOMATED_INPUT_PROOF (V5 independent oracle) | menu 60/60, gameplay 71/71, owner-remap ALL PASS | PASS |
| Pilot FP2 1.1.4 — NextOS Elite 4.9 1ac99b5c | ON_DEVICE_AUTOMATED_INPUT_PROOF | menu + owner-remap ALL PASS (gameplay reached); default-gameplay roteiro raced the loading screen once (rerun pending) | PASS/OPEN |
| Pilot Nameless Cat 1.2.8 (raw axis cursor, padset) — dArkOS K36S | ON_DEVICE_AUTOMATED_INPUT_PROOF | menu/gameplay/owner-remap ALL PASS (navigation calibrated from the corner, resolution-independent) | PASS |
| Pilot Nameless Cat 1.2.8 — NextOS Elite (0..255 "USB Gamepad") | ON_DEVICE_AUTOMATED_INPUT_PROOF | menu + gameplay ALL PASS after the clone publishes the pad's resting values and the glue uses a neutral floor for vector gestures | PASS |
| Pilot Blossom Tales 1.4.1 (MonoGame, video /2 content rect) — dArkOS K36S + NextOS Elite | ON_DEVICE_AUTOMATED_INPUT_PROOF | menu/gameplay/owner-remap ALL PASS on both devices | PASS |
| Pilot Tearscape 0.2.17 (Godot + built-in SDL3 declared static, 5.1) | port migrated to schema 4 / owner runtime / settings /2; engine rebuilt one-shot | device sessions pending the final composition pin | OPEN |
| Static provider path (`nxinput_provider_declare_static`) | FIXTURE_HOST (`tests/v5/test_v5_provider.c`) | main-program entry only; DSO refused; manifest `static_sources` pin | PASS |

| 0.11.6 pre-router: press flushed by an EVENT owes its release to a LATER tick (double tap, SELECT tap + START) | the padset union shows the first tap down in one sample; release on the next | `tests/v5/test_v5_prerouter.c`, `tests/v5/test_v5_padset.c` (mutants killed) |
| 0.11.6 seam: VIRTUAL/UNKNOWN driver (unmeasurable node) | ADMIT_STOCK, never BLOCK_IDENTITY; measured 0/0 node still blocked | `tests/v5/test_v5_seam.c` 1f |
| 0.11.6 translator: dpad on KEY_UP/DOWN/LEFT/RIGHT (gpio-keys) | line coherent, byte-intact; a face on an arrow key stays incoherent | `tests/v5/test_v5_translate.c` |
| 0.11.6 gptk_live: release of a never-latched control | PASSTHROUGH (native owns the release), never DELIVERED | `tests/run-gptk-live-boundary.sh` |
| 0.11.6 padset: primary elected by activity, whole vector from ONE pad | the pad the user moves owns the vector/trigger; hysteresis; explicit primary wins | `tests/v5/test_v5_padset.c` |
| 0.11.6 bridge /4: stick mode digital/split without a V3 sink | projection refused with reason, owner rejected, default stays live (never a dead stick) | `tests/v5/test_v5_gptk4_bridge.c` |
| 0.11.6 proof: `expect: context_change` | a delivery must be followed by the ENGINE proving a different context; delivery-only passes are killed | `tests/test_device_input_proof.py` |
