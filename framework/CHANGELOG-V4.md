# Framework NextOS V4 — linha FECHADA (tag `framework-v4`, 02/09/2026)

> **ESTADO: V4 FECHADA.** Fechamento documental sobre a composição terminal
> `framework/v4-composicao-quatro-ports-20260902` @ `9ade8911732b64b81be862362739b516d9d6f4a9`
> (framework executável provado em `4db2fff34ff1c6cd4019754d230f036c81834a2e`; a
> composição só acrescenta os quatro ports e a reconciliação do catálogo). Branch
> final: `framework/integration-v4-final-20260902`; tag agregada anotada
> `framework-v4` no commit final dessa branch; tags de componente anotadas nas
> versões abaixo. Nenhum merge em `master`/`release/v1`, nenhum upload, nenhum
> ZIP publicado. A V3 permanece congelada na tag `framework-v3`
> (`272165635235b9ac856e7a772930b038539a532a`); **nenhum port aprovado é
> regenerado, migrado ou retestado** por causa deste fechamento.

## Fechamento V4 — 02/09/2026 (noite)

- **Composição terminal:** `9ade891` = `4db2fff` + merges `--no-ff` de
  `port/namelesscat-1.2.6-v4-live-controls` (`b810eeb`; build `cd493e8`),
  `port/fp2-1.1.2-v4-live-controls` (`4670521`; build `7525bd4`),
  `port/blossomtales-1.4.0-v4-live-controls` (`ad0ed5d`; build `6f9fdbe`) e
  `port/tearscape-0.2.16-wayland-geometry` (`b840f23`; build `b840f23`) +
  catálogo reconciliado (`tearscape` na lista/taxonomia; `ports/INDEX.md`
  regenerado). `framework/` é byte-idêntico a `4db2fff` fora de `framework/catalog/`.
- **Bateria final (reutilizada, não repetida):** `framework/tests/run-safe-gates.sh`
  em `9ade891`: **ALL PASS count=141 skipped_external=3** `hardware_ran=0
  device_access=0`. Os três `SKIP` são limites externos declarados (guest
  initializers/JNI_OnLoad/link sintético fora do host), não falhas escondidas.
- **Quatro ZIPs congelados (autoridade final, byte a byte):** `namelesscat.zip`
  `6b346a0d272685251b0527490cde8b3a460d2cc781b3b3f5ec2328d23155711f`; `fp2.zip`
  `1826eb9ccf12c16a78db362d19bd4c71f54f4501e157e08c825eff41286b6fb8`;
  `blossomtales.zip` `79038b80938fc9396c3d2e3f5a2ba0191e99cdbb7f3485dfb6e57eb2cd44eb2f`;
  `tearscape.zip` `beb8f1d5113ee84a9532897752261e3763616cc80a9478e7f72e089df2eff950`.
  Controles provados automaticamente (`ON_DEVICE_AUTOMATED_INPUT_PROOF`) nos bytes
  finais, no dArkOS/K36S; nenhum rebuild, repack ou reteste neste fechamento.
- **Decisão do NextOS — prompts crus:** textos apresentados pelos próprios jogos
  (`Axis -2`, `Axis +2`, `Button 10`, `Joystick Button 10`) são **limitação de
  apresentação conhecida**, aceita nesta entrega; não bloqueiam a V4 e não foram
  corrigidos (glyphs derivados da ação GPTK → backlog V5).
- **Decisão do NextOS — RP5/ROCKNIX (Wayland):** a correção de geometria do
  Tearscape fica exatamente como está, classificada `BEST_EFFORT_UNVERIFIED`:
  fora da matriz de suporte garantido, sem prova física reivindicada, não
  bloqueante. Uma correção posterior exige sucessor próprio do port.
- **Suíte de testes:** `framework/tests` passa a `1.1.4` (tag
  `framework-tests-v1.1.4`), porque a tag imutável `framework-tests-v1.1.3` não
  representa a suíte que executou os 141 gates; a `v1.1.3` permanece intocada.
- **Imutabilidade:** depois de `framework-v4` verificada no `origin`, toda mudança
  de código, contrato, schema, comportamento ou versão nasce como trabalho da V5,
  a partir de `framework-v4`, em branch `framework/<componente>-<nova-versão>`.
  Ports novos consomem a tag pelo checkout detached
  `~/.codex-worktrees/framework-v4-final`; ports antigos mantêm seus pins.

## Composição

| Componente | V3 | V4 (fechada) |
|---|---|---|
| nxbootstrap | 0.6.37 | **0.7.8** (closure privada do NXExtract sem promover DSO a helper executável) |
| nxgl | 0.2.17 | **0.3.5** (frame proof fatal persistente antes do present, sem trocar GLES; adapters `nxgl_frame_proof_adapter` com PINS nos ports; 0.3.5: `nxgl_frame_proof_is_fatal()` sem trava — a 0.3.4 devolvia FATAL sob contenção e fechava jogo saudável com status 72 no quadro 32 — e `set_video_size`/`nxgl_godot_frame_proof_resize` para o recibo VIDEO seguir a reconfiguração do compositor) |
| nxloader | 0.7.2 | **0.9.0** (AArch64 icache attestation) |
| nxinput | 0.5.1 | **0.10.2** (runtime GPTK V3 vivo `nxinput-gptk-runtime/3`, costura C6 in-process, udev dinâmico + varredura por capacidade; `nxinput_padset` — todos os pads admitidos, chord só no mesmo instance; `nx-device-input-proof` — prova automática de controles no aparelho real por clones uinput, receipt `ON_DEVICE_AUTOMATED_INPUT_PROOF`) |
| nxandroid | — | **0.5.0** |
| nxaudio | 0.3.1 | **0.4.0** (runtime API 2) |
| nxgenerator | 0.2.20 | **0.3.17** (schema 3 de controles: `face_layout`, `controller_profiles` com par `face_layout_variants`, promoção do adapter-contract; `controls.proof` → roteiros da prova automática gerados ao lado do pacote, uma sessão por contexto; 0.3.17: START verificado antes do hotplug e dos negativos) |
| nxrelease | 0.2.43 | **0.3.26** (`bundle` = validate → stage → verify-stage → bundle numa bateria; candidate-lock externo somente leitura; evidência `nxinput-gptk-event-evidence/1` com `evidence_class` = `ON_DEVICE_AUTOMATED_INPUT_PROOF` obrigatória para ports com `controls.proof`; `nx-input-proof-lock` converte receipts em lock; base `controllers.nxb` não pode congelar GUID de layout mutável; nxscan estrutural ligado) |
| nxobs | 0.3.1 | **0.4.5** (diretório físico de prova privado e exclusivo; `nx-device-launch` reproduz o env do unit do frontend e aceita `NXBOOTSTRAP_LOGICAL_GAMEDIR`) |
| NXExtract | 1.2.21 | **1.3.0** |
| nxdoctor | 0.1.0 | **0.3.0** (evidência de recuperação) |
| nxabi | — | **0.2.3** (autoridade única de símbolo SDL, parser fail-closed) |
| contrato PortMaster | 2.0.0 | **2.1.1** |
| nxsplash | 0.1.2 | 0.1.2 (somente gate/changelog; artefato byte-idêntico) |
| nxcompat | — | **0.4.0** (resolver opcional de símbolos SDL pós-piso 2.0.4, aditivo e desligado por padrão) |

## Prova automática de controles no aparelho — 02/09/2026 (tarde)

Correção de interpretação do NextOS após a auditoria externa rejeitar o
primeiro status terminal da cascata: **o framework é automático e não depende
de uma pessoa apertando botões**. A prova válida de controles para
compatibilidade é o receipt `ON_DEVICE_AUTOMATED_INPUT_PROOF`, produzido no
aparelho real por clones uinput device-faithful comandados pela IA. Saúde do
interruptor mecânico é QA de hardware, fora do gate; testemunha humana não é
requisito de release.

Branch `framework/nxinput-0.10.2-on-device-automated-proof` (sobre a composição
de pré-fechamento):

- nxinput 0.10.2: `engine-glue/nxinput_padset` (união dos pads admitidos, chord
  SELECT+START só no mesmo instance, cross-pad negado e registrado, hotplug);
  `tools/nx-device-input-proof.py` + `tools/nx-input-inject-agent.py` (perfil do
  controle real no kernel, clones antes do SDL_Init do jogo, admissão provada
  pelo log do port, tabela controle→código só do mapping admitido, roteiro
  declarativo com vereditos por janela, receipt com device/CFW/SDL/GUID/
  capabilities/mapping/GPTK/adapter/ELF/generation/run, perfil reutilizável
  por hash). Gates `nxinput-padset-host`, `nxinput-device-input-proof`.
- nxgenerator 0.3.16: `controls.proof` (navigation, quit_guard, owner_remap,
  clones) → `<output>-proof/` com `input-proof-default.json`,
  `input-proof-owner-remap.json` e a cópia do dono; fora do adapter e do ZIP.
  Gate `nxgenerator-input-proof-roteiro`.
- nxrelease 0.3.26: `input_proof.evidence_class` (só
  `ON_DEVICE_AUTOMATED_INPUT_PROOF`; HOST_FIXTURE recusado; obrigatório para
  ports com `controls.proof`); `nx-input-proof-lock.py`. Gate
  `nxrelease-on-device-input-proof`.
- nxobs 0.4.5: `nx-device-launch` lê `NXBOOTSTRAP_LOGICAL_GAMEDIR`.
- Proibições mantidas: nenhum injetor de bancada em ELF/ZIP público (NC_VPAD/
  FP2_VPAD saíram das builds públicas; variantes de bancada compiladas à parte),
  nada injetado depois da SDL, nenhuma SDL privada, nenhuma conversão evdev→
  mapping dentro do port, HOST_FIXTURE nunca como on-device.
- Primeiros consumidores: Nameless Cat 1.2.7 e FP2 1.1.3 (candidatos 1.2.6 e
  1.1.2 invalidados). Próximos ports V4 herdam a prova declarando
  `controls.proof`; ports antigos aprovados não migram automaticamente.

## Cascata de 02/09/2026 — Nameless Cat 1.2.6, Freedom Planet 2 1.1.2, Blossom Tales

Branch `framework/v4-pre-close-20260902` (de `bcb375d`): merges rastreáveis
`3fe6317` (blossomtales `d424ad3`), `e835687` (namelesscat `f0fad66`),
`ceb69b1` (fp2 `1cd42d6`). Nenhum componente do framework mudou nesta
cascata; o que mudou foram os dois ports Unity, que passaram a consumir o
runtime GPTK V3 vivo de verdade, e a integração do candidato aprovado do
Blossom Tales. **V4 continua ABERTA**: sem tag, push, merge em `master`,
upload ou publicação.

| Port | Commit | Executável (SHA-256) | ZIP candidato (SHA-256) | Prova física |
|---|---|---|---|---|
| namelesscat 1.2.6 | `f0fad66` (congelado em `c9c1b9d`) | `bb2ded01f615a41ea018b0979965f485acb39dd66da1b7e179889fc271e6d52f` | `bfba1b94b4958e02babd6cebeafabe3598c22caf53e2f1f9c4f1c60ce7fb9973` | dArkOS/K36S: default + owner `A=null`/`R2=player.jump` (pré-ZIP) e abertura única do ZIP do zero (`NXE0000`, splash 5 s, vídeo OK, GPTK vivo, saída limpa, `NXU0006`) |
| fp2 1.1.2 | `1cd42d6` (ELF congelado em `b5d60cf`) | `0755f8cebcfb40f883f617a69826d22dbfabae6f877c184bff779bb7daeeaee3` | `6b8737aeb61e9f30c94a476136c4a6e3cc6fbd7b5ed2dc41e348cb11082c85e9` | dArkOS/K36S: 3 aberturas pré-ZIP do mesmo ELF (default, cobertura de menu, owner `A=null`/`R2=fp2.attack`), todas com saída limpa; abertura do ZIP do zero registrada no relatório da cascata |
| blossomtales | `d424ad3` (byte-idêntico) | `acdfbf00777db651…` | `45ac52ba100a8cd302c047fda77db5a32e9b1a7943c4a1f81044a1cd889c0199` (nxrelease **0.3.24**, não regenerado) | dArkOS/K36S: abertura única do ZIP exato (`NXE0000`, splash, vídeo OK, GO-Super admitido, MediaPlayer) + 2ª sessão com saída limpa e `NXU0006` |

Limites ditos com honestidade:

- `192.168.31.01` não respondeu como aparelho EmuELEC/NextOS nesta sessão
  (o endereço resolve para o roteador, sem SSH): a coluna `.01` da matriz
  física é **BLOCKED_DEVICE**, não sucesso implícito. Nenhum outro IP foi
  tentado.
- SELECT+START com o dedo do NextOS (#40) continua pendente nos três ports;
  o chord saiu limpo pelo estímulo simbólico e por SIGTERM convergido no
  chord.
- Contexto do FP2 é provado pela engine (`FPStage.playerInstance_FPPlayer`
  viva; `FPStage.state == PAUSED`/timeScale 0 = pause) — o menu principal do
  jogo também é um `FPStage`, por isso `currentStage` sozinho não serve.
- Nameless Cat e FP2 pinam o framework por commit (`bcb375d`) e SHA-256
  (`vendor/*/PINS.json`; `ports/fp2/FRAMEWORK-PIN.json` recriado pelo
  `framework_pin.py create` a partir de `bcb375d`, substituindo o snapshot
  `f01636e`). O ZIP `6b8737ae…` foi empacotado ANTES dessa troca e ainda
  embarca o snapshot `f01636e` em `fp2/FRAMEWORK-PIN.json` — arquivo
  informativo da materialização pública, fora do candidate-lock; a verdade do
  runtime embarcado é `vendor/nxinput/PINS.json` (nxinput 0.10.1 de
  `bcb375d`). Um re-bundle futuro carregará o pin novo; o ZIP atual não foi
  invalidado por isso.
- Os prompts do FP2 imprimem o nome do próprio binding ("10", "Axis 1"): não
  há fronteira de identidade que o jogo consulte para glyphs.

## Composição Godot universal de 31/08/2026

Godot passa a ter uma integração reutilizável, mas estritamente opt-in. O
nxinput 0.9.0 preserva os aliases e o runtime Godot 0.8.1, somando
normalização de domínio PortMaster/joydev antes da SDL. O runtime oferece
dispatch sem teclado, troca de contexto com
handoff de press/release e teardown de lifecycle; decisões de cena e sinks
continuam pertencendo ao adapter do jogo. O nxgl 0.3.4 mede o framebuffer
padrão imediatamente antes do present pela fachada já aprovada e restaura o
estado de pack, sem escolher GLES3, criar contexto ou alterar o caminho GLES2.
BLACK/DEAD-CONTEXT conclusivo trava o frame loop antes de outro present,
revoga health seguro, pede fechamento uma vez e preserva status não zero.

O nxgenerator 0.3.12 e o nxrelease 0.3.20 fecham a composição: GPTK vivo não
existe sem o bundle pinado da terceira autoridade, e um candidato Godot precisa
conter o par inseparável `nxinput-godot-runtime/1` e
`nxgl-godot-frame-proof/2`, inclusive os símbolos de fatal/consume, no ELF e
na prova externa. Uma promoção de adapter também precisa preservar o
`input_controller_profiles` exato; substituir o skeleton não pode apagar o
pin `controllers.nxb`. O nxbootstrap 0.7.8 só promove uma geração se o filho
supervisionado também terminou com status 0. O nxobs 0.4.3 cria a
evidência física em diretório privado adquirido atomicamente. Tudo continua
sem opt-in automático, sem migração de port aprovado e com prova física dos
novos bytes ainda `PENDING_PHYSICAL`.

O nxrelease 0.3.21 fixa a composição corrente no nxinput 0.9.0: o mesmo
conjunto de gates do 0.3.20 passa a exigir a normalização joydev comprovada
por capabilities na camada de autoridade, sem relaxar bundle da autoridade 3,
GPTK vivo ou marcador Godot. Nenhum gate muda de semântica; apenas a identidade
de composição avança.

O nxrelease 0.3.20 alinha a receita ao NXExtract 1.3.0: omitir o campo
top-level `validate` equivale ao array vazio que o engine já usa por default.
`extract`/`commit` continuam arrays obrigatórios e qualquer `validate` presente
com tipo diferente de array falha fechado. A composição e os bytes gerados não
mudam; nenhum port é regenerado ou migrado automaticamente.

O nxrelease 0.3.19 distingue a gramática de anotação Python `name: Type` de
um literal textual `key: value`: fontes geradas como
`m_VCPassword: Optional[str] = None` não são mais falso positivo. Atribuições
reais, com ou sem anotação, continuam fail-closed, assim como mappings em
formatos não Python. A correção não muda composição, interface, geração nem
bytes de port; nenhum port aprovado migra automaticamente.

O nxbootstrap 0.7.8 permite que um DSO usado por helper do NXExtract permaneça
no papel existente `private-library`, exclusivamente em `0644`, abaixo de uma
raiz privada declarada e com NXExtract ativo. A closure/store/heal e a
allowlist viva reconhecem o mesmo membro; modo `no`, owner data, path fora ou
igual à raiz, modo executável e paths canônicos reservados falham fechado. O
nxrelease 0.3.16 preserva a classificação existente `third-party-linux`, modo
e hash, e inclui o membro na comparação public-final da árvore NXExtract.
Helper ELF continua sendo `nxextract-helper` 0755 e `project-linux`.

O nxgenerator 0.3.12 separa no cabeçalho do
`NEXTOSCONTROLLERS.gptk` o mapa nativo estático do runtime GPTK vivo. Sem
`controls.runtime_mapping`, o arquivo declara que edições não chegam à engine;
com `nxinput-gptk`, preserva byte a byte o cabeçalho editável e todas as provas
de loader, dispatcher, sinks, ACK e autoridade 3. O nxrelease 0.3.18 fixa essa
composição sem relaxar o gate: uma promessa editável sem runtime continua
falhando fechada. Nenhum port aprovado é regenerado ou migrado.

O nxrelease 0.3.17 fecha a provenance da closure estática de hooks. Nenhum
digest literal de 64 hexadecimais pode viajar em fonte ou spec alcançável, nem
sob nome de saída, nem por coincidir com um `patch_profile`: o NXExtract
seleciona o perfil antes e o hook consome apenas seu id/ambiente. JSON pequeno,
UTF-8 estrito e objeto/array é seguido também sob sufixo desconhecido; ELF,
NUL, binário e conteúdo não-JSON não viram texto. Integridade transformada
permanece nos checkpoints canônicos da receita e na generation_runtime.

O nxrelease 0.3.15 acrescentou duas correções fail-closed sem mudar essa
composição nem qualquer interface visual. Um ELF declarado pelo port como
`nxextract-helper` deixa de ser payload e passa pela classe `project-linux`,
incluindo ABI/GLIBC, dependências e rebuild limpo; os demais papéis genéricos
continuam proibidos de carregar ELF. Recibos físicos GPTK passam a seguir o
opt-in real `controls.runtime_mapping = nxinput-gptk`: passthrough nativo com
ações descritivas exige `input = null`, enquanto uma alegação GPTK indevida
falha e todo o fechamento GPTK anterior permanece literal quando declarado.

## nxinput 0.9.0 — domínio joydev real do muOS

O `control.txt` do muOS 2601.1 no RG40XX-H usa ordinais joydev que incluem
`KEY_ESC` e volume antes da faixa de gamepad. SDL2/SDL3 atuais usam a
enumeração evdev moderna. Aplicar a linha sem tradução deslocava os botões:
A/B podiam desaparecer, Start físico chegava como L1 e o Start lógico ficava
no L2 físico.

A projeção ocorre na fronteira comum de autoridade e só é ativada quando o
bitset do event node exato e os bindings de volume provam o domínio antigo.
Não escolhe por CFW, nome, placa ou VID/PID; preserva GUID, eixos, hats,
metadados, ordem de autoridade e as decisões semânticas do CFW. O mesmo gate
cobre SDL2 e SDL3, e a integração Godot oficial deve compilar o módulo em seus
inputs congelados para que ele não volte a ficar fora do executável.

## nxcompat 0.4.0 — resolver canônico de símbolos SDL opcionais (V4-PRE-02A)

O FP2 expôs a incoerência do piso SDL universal: o nxabi aplica piso 2.0.4,
mas a closure aceitava nomes como `SDL_JoystickGetVendor` e
`SDL_JoystickGetProduct` (SDL 2.0.6), e um import direto deles derruba o boot
num firmware de piso. O nxcompat 0.4.0 entrega o lado construtivo dessa
frente: `nxcompat_sdl_optional.h` resolve símbolos pós-piso somente na SDL já
carregada pelo processo — o candidato vem de `dlsym(RTLD_DEFAULT, ...)` e só
é aceito quando `dladdr`/`dli_fbase` prova o exato módulo do símbolo-âncora
baseline; o módulo de produção não contém `dlopen` (gate estático recusa a
chamada no fonte e o import no ELF). Sem abrir/buscar SDL por nome/path, sem
SDL privada,
sem escolher outra implementação, sem seleção por jogo/CFW/device/nome/VID/
PID. Ausência do símbolo é metadata desconhecida/zero com boot preservado;
nada chama a API até um adapter optar explicitamente, e o 0.3.0 permanece
byte-compatível.

Gates: providers modelados 2.0.4/2.0.6+/parcial, providers reais carregados
com biblioteca estranha envenenada rejeitada, prova por valor em joystick
virtual da SDL do sistema e auditoria `readelf`/`nxabi --sdl-floor 2.0.4`
sobre fixture cross AArch64 com controle negativo obrigatório. A tabela única
de autorização por símbolo entre nxabi e nxrelease e o preflight antecipado
(restante do V4-PRE-02) e a integração desta versão na composição canônica
continuam pendentes e fora desta frente; o ZIP DEV do FP2 não foi alterado.

## nxabi 0.2.2 + nxrelease 0.3.22 — política única de piso SDL (V4-03B)

Fecha o restante do V4-PRE-02. O nxabi conhecia a versão de nascimento de
cada símbolo SDL, mas a closure do nxrelease decidia por uma lista paralela
(`symbol-floors/libSDL2-2.0.so.0.syms`, interseção de firmwares reais mais
novos que o piso) — foi assim que `SDL_JoystickGetVendor`/`Product` (SDL
2.0.6) passaram pela closure do FP2 apesar do piso universal 2.0.4. Agora a
autoridade é uma só: `framework/nxabi/sdl2-symbol-floor.tsv`, versionada
(`#% authority: nx-sdl-symbol-floor/1`) e consumida pelos MESMOS bytes e
pelo MESMO parser estrito nos dois lados; ambos os recibos registram id e
SHA-256, e um teste de consistência compara o mapa integral. Tabela ausente,
symlink, malformada, duplicata ambígua ou id errado falham fechado — nunca
viram mapa vazio que aprova tudo. Import direto acima do piso reprova na
primeira fronteira somente leitura do nxrelease (`validate`), antes de
stage/cópia/ZIP, nomeando ELF, símbolo, versão exigida, piso e hash da
autoridade, sem waiver; a lista paralela do core foi removida e reaparecer é
falha fechada. A rota canônica para APIs pós-piso segue o resolver opcional
do nxcompat 0.4.0. O piso universal não subiu, nenhuma seleção por
jogo/CFW/device/nome/GUID/VID/PID foi criada e a composição 03A (nxinput
0.9.0/nxledger) permanece fora desta branch.

## Falha fechada para tela preta com áudio

Uma execução com PID vivo, áudio, entrada, contexto gráfico ou status zero não
prova imagem. O nxgl 0.3.2 mede pixels reais no framebuffer padrão imediatamente
antes do present, sem consumir `glGetError`, e publica um receipt privado,
atômico e ligado à execução. O preflight fecha draw/read FBO, pack PBO e estado
de pack; duas sentinelas recusam leitura ausente/parcial, a aritmética fica
limitada a 64 MiB e RGB colorido com alpha zero não conta como imagem. Somente
`OK/non-black` before-present prova vídeo; três observações sequenciais
conclusivas geram `BLACK` ou `DEAD-CONTEXT`, que não podem ser sobrescritos por
um frame posterior. Quando solicitada, a captura RGBA vem exatamente da
primeira amostra elegível e uma falha de escrita impede o `OK`.

O nxbootstrap 0.7.8 supervisiona esse receipt. No opt-in
`video_proof: required`, nenhuma saúde promove a geração sem imagem; uma falha
conclusiva remove qualquer receipt de saúde concorrente, encerra o filho exato
com prazo limitado e devolve erro. Mesmo receipts exatos de health e vídeo não
promovem um filho que saiu com status diferente de zero. O opt-in
`sdl_provider: system` mantém o
provider do firmware à frente dos overlays, não força backend gráfico e recusa
SDL1/SDL2 privada nos caminhos do port. `LD_PRELOAD` e `SDL_DYNAMIC_API`
herdados são capturados com builtins antes do primeiro helper; valores do
`port-env.sh` são recapturados imediatamente após o hook e só chegam ao
subshell do `exec` depois do gate. Entrada não resolvida, core/add-on SDL
package-private renomeado ou troca de BIN/interpreter/loader/rota falha
fechado; override canônico fora do `GAMEDIR` preserva a autoridade do
firmware e continua child-only. O Bash já iniciou antes da primeira captura e
comandos executados dentro do próprio hook
continuam responsabilidade do adapter; a inspeção package-wide profunda fica
no nxrelease. Manifests antigos preservam ambiente, hook e ordem legados, sem
opt-in automático.

O nxgenerator 0.3.7 transporta os dois campos sem inferi-los e fixa os bytes do
nxbootstrap 0.7.4. O nxrelease 0.3.8 fecha a fronteira restante: recusa SDL
privada por nome, SONAME, provider, símbolos core/add-on, `dlopen` e shell. A
exceção SDL3 exige DSO Linux real, ABI, SONAME, modo, path, símbolos, versão,
fonte, licença, razão e SHA-256 exatos. Para vídeo obrigatório, um candidate
lock externo, congelado e somente leitura liga o receipt `OK/non-black`, a
identidade da execução e o SHA-256 do executável. Ele é uma âncora procedural,
não uma alegação autônoma de observação física. Source, stage e ZIP reaberto
precisam conter exatamente os mesmos bytes; rebuild ou retirada da rota visual
invalida o candidato antes do ZIP. O fluxo normal compila uma vez: não existe
build-B automático; a repetição opcional só refaz stage/package dos mesmos
inputs. Candidatos 0.2.40--0.3.7 ficam verificáveis apenas em quarentena
read-only, com autoridade externa e `publication_eligible=false`.

Os gates desta integração são host e dirigidos; nenhum aparelho foi acessado.
A prova física dos novos bytes permanece `PENDING_PHYSICAL`, sem tag, release,
push, ZIP ou migração automática de ports.

## Integração "pronta para uso, aberta" — 30/08/2026

Decisão do NextOS: a V4 **não fecha**; fica pronta para uso como linha de
melhoria contínua, **sem tag, sem release, sem merge em `master`** e sem
regenerar port aprovado. Nesta integração entraram os cinco owners que
estavam prontos e fora da branch (nxaudio 0.4.0, nxdoctor 0.3.0, nxbootstrap
real-crash, nxloader 0.9.0, nxgenerator catálogo) e o nxinput 0.7.2 nascido da
validação física do Nameless Cat no dArkOS. Composições de linhas paralelas
viraram superset com bump legítimo (nxbootstrap 0.7.3, nxgenerator 0.3.6,
nxrelease 0.3.7); nenhum bump foi revertido.

Dívidas encontradas no HEAD anterior (`ed97bc3`) e fechadas aqui: lock
`nxinput` 0.7.0 × VERSION 0.7.1; pins m11/m12/m13/m14 do declarative stale
desde a onda .NET; 27 fontes de teste das frentes C1–C9 e dos owners fora da
matriz; `expect_fail` do nxrelease preso ao nxbootstrap 0.7.0; gate C8 preso
aos bytes de um ledger externo mutável. Prova física de todos os bytes novos
continua `PHYSICAL=PENDING`; o registro corrente fica em
`FRAMEWORK-DEFINITIVO-TESTES/REGISTROS/V4-MAPA-INTEGRACAO-PRONTA-PARA-USO-20260830.md`.

## Compatibilidade de runtime vazio do PortMaster pós-MMW

O segundo candidato V4 do MMW passou toda a bateria host, mas foi invalidado no
aparelho dArkOS durante a instalação. O PortMaster oficial
`2024.03.10-0841`, commit
`7471d54c7c6ca57c16dee1b77cdd57d4226a1b86`, extraiu o pacote e depois tentou
formatar `attr.runtime: []` como string, levantando `AttributeError`. Os hashes
de `info.py`, `harbour.py` e `util.py` no aparelho eram byte-idênticos à tag
oficial; não era uma modificação local do firmware.

O contrato PortMaster 2.1.0 fixa essa proveniência e demonstra a interseção
compatível: em metadado v4, campo ausente vira `None` no parser legado e `[]`
no atual, e os dois pulam corretamente a instalação de runtime. O nxgenerator
0.3.4 continua exigindo `portmaster.runtime` como lista no projeto, mas omite o
campo somente na saída quando essa lista é vazia; lista não vazia permanece
preservada. O nxrelease 0.3.4 aceita a omissão e recusa `runtime: []`, impedindo
que um pacote manual reintroduza a falha.

Owners terminais: PortMaster `7baa5458f2afd62a50f8445b5b9e375836010e4c`
(subtree `cf3c785f050f6af0ad1341c2a263980288070569`), nxgenerator
`21d4bb27690c2e785112331c9f42f34d612b1202` (subtree
`ff9e09e02010f20944e83b5aeafd42712444c147`) e nxrelease
`eaf21ec53428f67439db7091ffc03eb08b842bc9` (subtree
`aa71d37a575072cdfe8ec19cff674bbf21a4e09f`). Os três gates dirigidos passaram
e as mesmas subtrees estão integradas. O ELF Godot aprovado não mudou e não é
recompilado; o candidato físico anterior permanece invalidado e imutável.

## Correção de empacotamento pós-C9 (`45bec25`, `14b3a5a`)

O primeiro candidato V4 do MMW revelou um defeito geral no NXRelease: o seed
visível `NXBUNDLE1` começa por um índice ASCII e carrega depois os membros
concatenados. O scanner 0.3.2 classificava o contêiner inteiro pelos primeiros
4 KiB e, por isso, tratava o ELF Godot interno como texto; o endereço multicast
público SSDP `239.255.255.250` virava falsamente um IP privado do pacote.

O nxrelease 0.3.3 valida o header, os offsets, o tamanho total e o SHA-256 de
cada membro, reiniciando a classificação texto/binário em cada fronteira.
Assim, constantes binárias de protocolo deixam de ser falsos positivos sem
relaxar privacidade: IP em membro textual, path privado mesmo em ELF, segredo,
hostname, truncamento, digest divergente e colisão de paths continuam falhando
fechados. Candidatos 0.3.2 já congelados permanecem reproduzíveis.

Essa é uma correção aditiva posterior à closure C9; não reinterpreta nem repete
as baterias C1–C9, não altera controles, interfaces visuais ou runtime e mantém
a V4 aberta.

## Correções pós-integração (`5d37f4f`, `c4e80e9`, `b29e809`)

**`5d37f4f` — NXDoctor em mídia chmodless.** Medido no aparelho autorizado:
`/roms` é exFAT com `fmask=0000`, todo arquivo regular lê de volta `0777` e
`chmod` é no-op. O doutor comparava o modo gravado com o modo real e reportava
"component mode mismatch" em CADA membro de um port **saudável**, que o launcher
acabara de rodar. A identidade de modo passou a ser observação quando o
filesystem demonstravelmente não a aplica; o hash — que é o que uma adulteração
quebra — continua decidindo, e a prova física leu duas gerações como `complete`,
13/13 por hash.

**`c4e80e9` — a âncora do launcher e o PortMaster.** Um `harbourmaster install`
real reescreve a linha 2 do launcher instalado com o nome do zip de origem. A
comparação byte a byte recusava o port com `NXU0012 installed launcher does not
match the runtime seed closure` **no único caminho de instalação que os usuários
usam**; a prova anterior passava só porque o ZIP era instalado à mão. A relação
instalado × closure passou a ser tomada sobre a forma canônica dessa única linha
de comentário, sem enfraquecer nenhum hash gravado. Qualquer outro byte alterado
continua falhando fechado.

**Falha subsequente preservada como diagnóstico.** O primeiro candidato
instalado por esse caminho não chegou à NXSplash nem ao runtime: parou em
`PHASE nxextract ERROR: NXExtract terminal result is missing, unsafe or
malformed`. A causa não estava no parser. Os stubs privados da fixture usavam
`set -u`, mas dependiam de `NXV2_NXEXTRACT_MARKER` e `NXV2_RUN_MARKER`, duas
variáveis fornecidas somente pelo shell do teste host. O commit `3d65997`
adiciona fallbacks autossuficientes e `4e39e38` prova explicitamente o launcher
gerado com ambas as variáveis ausentes; `b29e809` integra essa regressão.

**E2E físico fechado no candidato exato de `b29e809`.** O ZIP privado SHA-256
`5a948993a82ba5dd6bfd876b62f0c3f856d074ebfa9737884b7020f8ecce74ac`,
com seed SHA-256
`9f696f9b31a4916692bac1cbc3d49d4628b6c92f683ae1bdf60308eceb7f139b`,
foi instalado pelo HarbourMaster real e aberto sem variável manual. A mesma
execução provou, em ordem: rebuild `NXU0012`, geração `complete` 13/13,
NXExtract 1.3.0 gráfico SDL com terminal `NXE0000`, NXSplash 0.1.2 em
SDL/KMSDRM por 5000 ms, runtime status 0 e saúde `NXU0006`. O owner-data ficou
byte-idêntico. Capturas físicas 640×480 do mesmo candidato deram `AE=0` contra
os goldens determinísticos do NXExtract READY e dos estados observados da
NXSplash. A fixture foi desinstalada e limpa ao final.

Esse resultado fecha somente o caminho NXExtract/PortMaster que estava aberto.
A V4 continua aberta, sem tag, release, publicação, push ou merge em `master`.

## O que a V4 fechou

### V4-CONTROLLERS-03 — autoridade soberana e consumers opt-in (C1–C9)

A frente de controles foi integrada sem fechar a V4. O nxinput 0.7.0 preserva
o mapping soberano do CFW byte a byte, resolve a autoridade na ordem literal
`get_controls → banco do CFW por GUID → bundle do port → builtin do runtime →
raw declarado → falha explícita` e só anuncia o pad depois de validar sintaxe,
GUID, capacidades medidas e readback. O contrato NEXTOSCONTROLLERS v2 decide,
por controle e contexto, entre `action`, `null` e `native`; `null` termina antes
de qualquer fallback, enquanto o chord SELECT+START permanece fora da banda do
gameplay e recusa L2+R2, GUIDE+START e combinações entre pads.

As costuras reais de Godot 3.5.3, Godot 4.2.2, SDL 2.28.5, SDL 2.32.10 e SDL
3.2.30 foram exercitadas em host integralmente headless, antes do anúncio e do
Open. Elas cobrem os 18 grupos, press/release, hats, sticks, gatilhos, dois pads,
GUID duplicado, hotplug/reopen, `null`, owner-swap, `native`, chord e negativos.
A supressão por controle na API SDL_Joystick e no evdev raw não declarado
permanece explicitamente `UNPROVEN`; SDL3 também exige uma fonte declarada,
pois seu banco nativo só é inicializado depois da enumeração.

O nxandroid 0.5.0 adiciona duas fronteiras independentes e default-off. A C7
entrega um evento GPTK v2 congelado a exatamente uma rota Android declarada,
com identidade de instância/geração, ack real, lifecycle, touch/cursor,
multipad e hotplug. A C8 acrescenta perfis estritos e separados para Unity
Legacy Input, New Input System, Rewired, InControl e raw Android, exigindo
identidade exata e retornos reais do producer, da API de baixo nível e da ação.
Os consumers Android/Unity foram provados somente como `FIXTURE`: nenhum dos
ports históricos foi migrado e nenhum perfil Unity real foi promovido.

O nxobs 0.4.2, nxgenerator 0.3.2 e nxrelease 0.3.2 fecham respectivamente os
receipts sanitizados/observed-only, a geração declarativa e os gates de
empacotamento da frente. A matriz esparsa agregada separa prova histórica,
`REAL_API_HOST`, `FIXTURE`, `PENDING` e `UNPROVEN`; não transforma ausência de
célula em suporte universal. Prova dos novos bytes em aparelho permanece
`PENDING_PHYSICAL`, toda adoção é explícita por port e a V4 continua aberta,
sem tag, release, publicação ou migração automática.

### V4-GRAPHICS-04 — fronteira post-first-present (nxgl 0.3.1, nxgenerator 0.3.1, nxrelease 0.3.1, nxobs 0.4.1)

O BB2 1.0.6 no ROCKNIX provou a lacuna: contexto ES 3.1 vivo, shader OK e
drawable preso em `1x1` **antes** do primeiro present. A API one-shot esperava
o resize sem devolver o controle; o guest nunca apresentava; o gate reprovava o
provider e repetia aliases até o runtime sair com status 255. O contrato estava
certo em recusar `1x1` — o erro era exigir prova pós-present numa fronteira
pré-present.

O nxgl 0.3.1 torna a fronteira explícita e aditiva: máquina pura
`UNINITIALIZED -> REJECTED | AWAITING_FIRST_PRESENT -> REJECTED | PROVED`
(terminais estáveis, one-shot), preflight que valida contrato/provider/shader
sem esperar resize e sem emitir health, e `after_present` chamado pelo wrapper
somente **depois** do present real do guest, com deadline monotônico iniciado
no primeiro present e nunca reiniciado por frame. `1x1` persistente continua
`drawable-stuck-1x1`; zero clear/draw/swap sintético; nenhuma decisão por nome
de CFW/GPU/jogo; a API 0.3.0 permanece literal. Provas host: máquina pura e
adapter (SDL/GL falso com lifecycle Wayland) em GCC/Clang/ASAN/UBSAN, e a
fixture Wayland hermética `run-v4-wayland-host.sh` (compositor headless
isolado, guest SDL2/GLES real): preflight pendente, prova só após o primeiro
commit, e janela que nunca apresenta segura fail-closed.

A ativação é somente declarativa: `graphics.evidence_boundary =
"post-first-present"` (nxgenerator 0.3.1; ausência preserva bytes antigos e
nenhum launcher ativa por autodetecção). O nxrelease 0.3.1 exige, sob o
opt-in, receipt `phase=post-first-present` + `first_present=1` +
`pre_drawable` + identidade de commit consistente, one-shot — receipt
pré-present, `1x1`, duplicata ou divergência nunca promovem. O nxobs 0.4.1
preserva o receipt final sanitizado no support bundle (sem paths de provider)
e registra o estado pré-present como observado, nunca como `ok`.

O BB2 permanece inalterado e **não** está corrigido por esta entrega: a
migração do consumidor é uma tarefa port-local separada e opt-in.

### V4-REPACK-01 — seed visível, `.nxruntime` como cache

Todo port de schema 3 emite `<port-id>/nxruntime-<generation>.nxb`: arquivo
**regular, visível, `0644`, determinístico e content-addressed** com a closure
imutável completa. O `commit` nunca é transportado — ele é escrito por último,
no aparelho, como recibo transacional.

`.nxruntime` vira **cache local reconstruível**. Um ZIP pessoal que descarta
todos os dotdirs — comportamento normal de compactador gráfico — volta a
instalar e abrir, **sem enfraquecer o selo da V3**.

A raiz de confiança e o modelo de ameaça foram escritos **antes** do código, em
`nxbootstrap/BUNDLE-TRUST-V4.md`. A âncora é o id de geração compilado no
launcher; `sha256(identity.json)` tem de igualá-lo, e o launcher instalado é
comparado ao `files/launcher/<nome>` do seed. Não há self-hash circular, e não
se afirma resistência contra um administrador local do próprio aparelho — isso
está escrito, não escondido.

Geração existente **nunca** é reparada no lugar e `commit` ausente **nunca** é
fabricado por cura.

### V4-CONTROLLERS-02 — mapping heterogêneo do PortMaster

Um `SDL_GAMECONTROLLERCONFIG` real é uma lista com uma entrada por aparelho
conhecido, em dialetos diferentes. A seleção agora acontece **antes** de
qualquer conversão e é conservadora de propósito: entrada única é usada como
antes; GUID correspondente é usado esteja onde estiver; **lista sem entrada
deste aparelho vira passthrough**, sem nenhuma chamada SDL; duplicata idêntica
passa; duas entradas divergentes com o mesmo GUID falham fechado, porque a
store da SDL3 é indexada por GUID e a ordem nunca pode decidir; entrada
malformada falha fechado em vez de ser ignorada.

### V4-GRAPHICS-03 — imports EGL ligados ao dono do contexto

`nxgl_egl_binding` liga os imports EGL de um guest Android relocado ao provider
que **realmente** possui o contexto corrente. O candidato abre `RTLD_LOCAL`,
prova todos os imports declarados, prova contexto não nulo e prova que é o
mesmo objeto do resolver do SDL; só então é promovido a `RTLD_GLOBAL`.
Candidato recusado nunca chega ao namespace global.

No nxloader, `call_initializers` passa a recusar com `NXLOADER_EUNRESOLVED`
enquanto restar import forte indefinido. Esse era exatamente o estado do
incidente OTR 1.0.3 no ROCKNIX: o resolver imprimia `UNRESOLVED`, os
construtores rodavam mesmo assim e o primeiro `eglGetCurrentContext@plt`
saltava pelo valor link-time cru do GOT.

### V4-DISPLAY-01 — apresentação e content rect

`nxgl_display` é puro: política declarada + aspecto interno + drawable
**medido** → content rect e a inversa exata para toque. A contradição da
especificação V3.1 foi resolvida na direção segura: **ausência de campo é
`game`/no-op**, então todo port já aprovado continua byte-idêntico ao ser
regerado. `preserve` faz letterbox, muda pixels, e por isso é opt-in explícito
e nunca default. Nada é decidido por device, CFW, GPU ou nome de jogo.

### Os seis débitos herdados

1. **APK-COMPAT** — o pipeline gerado roda contra uma **cópia legal externa
   real**, em variantes renomeada, reempacotada e de outro formato, com
   negativos de outro package, payload obrigatório ausente e ABI não declarada.
   Executado de verdade em `.apk` e `.apkm`. O artefato proprietário nunca
   entra no repositório e os recibos guardam só identidade técnica.
2. **STORAGE** — preflight com custo de active+pending+previous+staging+margem,
   recibo `STORAGE:` e recusa `NXU0013` antes de qualquer escrita, sem destruir
   a geração ativa.
3. **HARDENING/RLIMIT** — cerca de recursos falha fechada em duas camadas:
   `namespace-watchdog.py` e os hooks de receita do NXExtract. Antes, qualquer
   falha de `setrlimit` era engolida e o hook rodava **sem teto nenhum**.
4. **REPRO** — a dupla construção deixa de ser flag e passa a ser obrigatória
   em `build`/`bundle`; todo ELF autenticado é comparado além do ZIP e a falha
   **nomeia o ELF**; `BUILD-PROVENANCE.json` sanitizado é sempre emitido.
5. **PERF** — `nxobs_perf` fecha threads, FDs, CPU e `/proc/self/io`, mais os
   quatro intervalos de boot. Observar nunca autoriza agir; o caminho de frame
   é O(1), sem I/O e sem log.
6. **VSYNC/OBS (AUD-22)** — intervalo **pedido** e **efetivo** registrados
   separadamente, mais `honored`, frames e FPS medido.

## Limites de claim, ditos com honestidade

- Os gates de promoção que exigem ferramentas reais de Windows, macOS e Android
  e uma segunda família de firmware/filesystem **não foram executados**. As
  variações herméticas equivalentes rodaram no host (Info-ZIP, 7-Zip e um ZIP
  DOS/stored/embaralhado sem modos Unix). Isso é limite de claim, não sucesso.
- A prova física desta missão foi feita **exclusivamente** em
  o aparelho autorizado (dArkOSRE, Debian 13, AArch64, `/roms` exFAT). Uma
  família não vira claim universal.
- O MMX Regenesis é o primeiro consumidor opt-in **em nível de fonte** do
  nxinput V4. O campo declarativo `controls.sdl3_portmaster` exige migrar
  aquele port para nxproject schema 3, o que é um opt-in separado e não foi
  feito como efeito colateral.
- Nada aqui recebeu tag, release, promoção, publicação ou merge em `master`.
