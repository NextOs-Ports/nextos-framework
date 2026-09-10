# nxinput

`nxinput` 0.10.2 é a camada estática de controle para loaders novos do
NextOS/PortMaster. Ela
abre até quatro `SDL_GameController`, mantém cada jogador pelo
`SDL_JoystickInstanceID` e oferece um estado Xbox pequeno e previsível para a ponte
específica da engine.

Ela não conhece Mali, Mesa, framebuffer, áudio, JNI ou uma engine particular. Também
não substitui o fluxo nativo do jogo. Sua única fronteira é:

```text
mapping do PortMaster/firmware -> SDL_GameController -> nxinput -> ponte da engine
```

## Runtime Godot reutilizável (0.8.1, opt-in)

`nxinput_godot_runtime.h` contém somente a política comum que adapters Godot 3/4
precisam para não reintroduzir bugs já medidos: OR/refcount quando dois controles
apontam para a mesma ação, barreira de neutralidade na passagem do caminho nativo
para GPTK, strengths analógicos sem uma segunda deadzone e lifecycle fatal separado
do quit limpo. O header não conhece cenas, nomes de ações, co-op nem a API do jogo;
isso continua no adapter versionado do port.

Uma callback que retorna sucesso comprova que a ponte enfileirou o
`InputEventAction`, não que o consumer C#/GDScript reagiu. A release deve ligar
ações e sinks reais por evidência externa congelada. A integração é opt-in e não
migra nenhum port aprovado automaticamente.

## Mapping soberano (V4-CONTROLLERS-03/C3)

`nxinput_sovereign` implementa a ordem única de autoridade de mapping, com o
mapping funcional do PortMaster/CFW soberano: env do
`get_controls` -> banco oficial por GUID -> bundle `NXCONTROLLER_PROFILES/1`
do ZIP -> built-in do runtime -> raw declarado -> falha explícita. Cada etapa
valida sintaxe, GUID exato, capacidades medidas e readback efetivo. O rewrite
pós-load por topologia foi removido do caminho default; detecção de CFW serve
apenas para localizar fornecedor/paths, nunca para decidir A/B/L2/R2.

Desde 0.8.0, dado real bem formado segue a semântica da SDL: metadata
desconhecida é tolerada e a última entrada efetiva para um GUID vence, com o
conflito contado no receipt. GUID ou mapping malformado e readback divergente
continuam falhando fechado. Um runtime GPTK vivo também exige o bundle pinado
da terceira autoridade dentro do próprio pacote.

## Costura SDL2/SDL3 (V4-CONTROLLERS-03/C6, opt-in)

`nxinput_sdl_seam` é compilada **dentro** da SDL2 ou SDL3 pinada do port e roda
em `MaybeAddDevice()`, antes de `SDL_PrivateJoystickAdded()` — portanto antes do
anúncio, antes de `SDL_IsGameController`/`SDL_IsGamepad` e antes de qualquer
`Open`. Um pad não admitido não é anunciado e some também de `SDL_Joystick`.

Ela **não decide o mapping**: chama `nxinput_authority_admit()`, que é a ordem
soberana da C3, literalmente. O que ela acrescenta é a arbitragem da colisão de
GUID (a store da SDL é indexada por GUID; a C3 admite por instância) e o staging
de `SDL_GAMECONTROLLERCONFIG` antes do `SDL_Init`, sem o qual a SDL importa a
variável com prioridade USER e vence a decisão em silêncio.

Na SDL3 e na SDL2 (≥ 2.26), a costura também reproduz a própria regra de
identidade da SDL para GUIDs de bus: um banco PortMaster pode trazer zerado o
CRC16 de nome dos bytes 2–3, enquanto o GUID ao vivo traz esse CRC preenchido.
Somente quando os outros 14 bytes coincidem exatamente, a visão entregue ao
resolver recebe o CRC ao vivo. Não há casamento por nome/modelo/VID/PID e uma
SDL2 anterior à 2.26 entrega word zero e fica intocada. Desde a 0.8.0, quando
a fonte já traz a entrada exata do GUID vivo, a projeção de CRC nem roda e a
entrada exata vence; duplicata divergente que ainda se materialize no mesmo
store resolve pela última linha (semântica do `AddMapping` da SDL), contada
no receipt como `dup_lastwins`.

Medido nos pins da C6: SDL 2.28.5, 2.32.10 e 3.2.30 enumeram botões/eixos
identicamente entre si. A 0.9.0 acrescenta a diferença que o caso real do muOS
expôs antes dessa fronteira: certos `control.txt` enumeram todos os `EV_KEY`
em ordem joydev, incluindo teclas de volume antes dos botões. Quando o event
node exato e os bindings de volume provam esse dialeto, somente os `bN` são
projetados para o domínio evdev da SDL; mappings já nativos permanecem
byte-idênticos. A projeção de identidade do GUID continua separada. Contrato
completo, limites do claim e o que fica `UNPROVEN` em
`docs/SDL2-SDL3-SEAM-C6.md`.

A 0.10.0 fecha a outra metade do caso real: em CFWs onde A/B e X/Y são
PREFERÊNCIA do usuário (duas bases oficiais para o mesmo GUID, escolhidas por
um symlink recriado no boot), o bundle do ZIP jamais pode congelar uma das
metades. O GPTK ganha o formato `NEXTOS_CONTROLLERS/3` com
`FACE_LAYOUT = auto|modern|retro` (lido uma única vez, pré-init), o C6 ganha
`nxc6_declare_port_bundle_for_layout()` (três bundles: base invariante +
duas variantes autenticadas, sempre só autoridade 3), `nxinput_livedb` espera
de forma limitada o banco vivo que o boot cria em background, e a decisão de
domínio joydev↔evdev passa a ser por PROVA SEMÂNTICA contra o bitmap medido
(ambiguidade faz a fonte ceder; nunca passa em silêncio). Todo receipt C6 sai
em dois sinks: o arquivo durável e o stderr/log normal do port.

## Observabilidade completa de input (V4-CONTROLLERS-03/C2, opt-in)

`nxinput-observe` é um arquivo separado e puro: o adapter injeta fatos já
medidos e o módulo emite receipts limitados (`NXINPUT-LOAD`,
`NXINPUT-CAPABILITIES`, `NXINPUT-BINDING` com os 18 controles canônicos,
`NXINPUT-CHORD` com negativos de saída, `NXINPUT-EVENT` bounded e
`NXINPUT-CONSUMER`). Sink NULL = no-op total; telemetria nunca altera decisão
ou sequência de input. O `nxinput-doctor` mostra a matriz completa ao vivo em
modo somente leitura, sem grab e sem injeção.

## SDL3 privado + mapping PortMaster (V4, opt-in)

`nxinput-sdl3-portmaster` é um alvo separado e desligado por padrão. Ele atende
runtimes que carregam uma SDL3 privada, mas recebem de PortMaster um mapping
autorado na ordem SDL2/joydev. O contrato completo está em
[`docs/SDL3-PORTMASTER-V4.md`](docs/SDL3-PORTMASTER-V4.md).

O adapter chama `nxinput_sdl3_pm_stage_before_sdl_init()` antes de qualquer
subsistema SDL. O componente copia o mapping não vazio e remove
`SDL_GAMECONTROLLERCONFIG` do ambiente antes de `SDL_Init`, impedindo que SDL3 o
carregue com prioridade USER e recuse silenciosamente o setter de prioridade
API. Depois da enumeração, o coordenador mede o event node exato e converte por
capacidades antes de o guest chamar `SDL_IsGamepad` ou `SDL_OpenGamepad`. Tanto
o rewrite quanto um mapping já nativo são registrados e só entram no cache após
readback semântico por `SDL_GetGamepadMappingForID`. Falhas de
path/open/fstat/ioctl/registro/readback continuam retryable e registradas, mas
não bloqueiam a chamada SDL nativa; remoção por hotplug invalida somente a
instância.

Esta primeira fatia coordena até 16 instâncias compatíveis com o mesmo mapping
de entrada. Seleção genérica de vários mappings newline-separated e pads
heterogêneos continua pendente; a branch é um candidato host, não uma release
V4 nem uma alegação de suporte físico herdado.

As wrappers de enumeração, classificação, Open, remoção e receipt pertencem à
mesma thread dona do loop SDL. O adapter deve encaminhar os eventos de remoção
nessa thread antes de reutilizar um instance ID; o componente não drena eventos
nem promete coordenação com chamadas concorrentes da engine.

A biblioteca não promete que a engine recebeu um botão apenas porque SDL
classificou ou abriu o pad. O adapter registra a confirmação somente no ponto
real em que seu wrapper/callback devolve o estado ao consumidor do jogo.
Também não fornece SDL3: cada port fixa uma build privada com backend Linux
evdev e carregamento dinâmico de udev, sem `DT_NEEDED` em `libudev`, além de sua
própria procedência/hash e receipt de descoberta antes da classificação.

## Contrato

- C99 e APIs antigas do SDL2 GameController; `SDL_CONTROLLERDEVICEREMAPPED` só é
  compilado quando o header oferece SDL 2.0.4 ou superior;
- biblioteca `STATIC`, destinada a entrar no loader ARMv7/AArch64 em vez de adicionar
  mais um processo ou helper ao port;
- no máximo quatro pads independentes, nunca fundidos no Player 1;
- **o mapping é decidido por uma única autoridade** (V4-CONTROLLERS-03/C3):
  todo pad passa por `nxinput_authority_admit()`, que roda
  `nxinput_sovereign_resolve()` na ordem `get_controls vivo` >
  `GUID exato no banco do CFW` (`SDL_GAMECONTROLLERCONFIG_FILE`) >
  `bundle NXCONTROLLER_PROFILES/1 do ZIP` (`NXCONTROLLER_PROFILES`) >
  `banco embutido do runtime` > `raw declarado pelo consumer` >
  **falha explícita antes do gameplay**. Cada etapa valida sintaxe, GUID
  exato, capacidades MEDIDAS do pad e um readback efetivo; um setter cujo
  readback não é semanticamente idêntico nunca vence. A entrada
  PortMaster/CFW é soberana: suas escolhas semânticas permanecem intactas; a
  costura só pode projetar ordinais joydev para evdev quando as capabilities
  do event node e os marcadores de volume provam o domínio de origem;
- **não existe segunda rota**: `nxinput.c` não aplica mapping nenhum por conta
  própria (o gate estático falha se `SDL_GameControllerAddMapping` reaparecer
  ali), e um pad que nenhuma autoridade consegue servir **não é aberto**.
  Quando há pads presentes e nenhum deles pode ser servido,
  `nxinput_create()` falha em vez de devolver um contexto sem controle;
- a normalização pós-carga que existia aqui (`face b0..b3` → contrato
  PortMaster com R3) foi **removida na raiz** na C3: ela reescrevia
  `a/b/x/y/guide` depois do mapping oficial e contradizia a captura física do
  GO-Super. Só conversores opt-in com entrada, saída e consumer medidos
  (o coordenador SDL3/PortMaster) podem transformar um mapping;
- uma linha que repete uma chave de binding, ou que não declara binding
  nenhum, é ambígua e **falha fechada** — a ordem nunca decide;
- as capabilities de D-pad, sticks e gatilhos são calculadas por pad a partir
  dos bindings do `SDL_GameController` e validadas contra a quantidade real de
  eixos, botões e hats do `SDL_Joystick` aberto. Um mapping que declara `a2/a3`
  em hardware com apenas dois eixos não fabrica um segundo stick; nome, GUID e
  a contagem global de sticks do host não substituem essa prova;
- essa validação também separa bug de port de bug de firmware: se o kernel
  omite os eixos de um stick físico (caso comprovado com `skip-absr` no DTB do
  dArkOSRE), `nxinput` não inventa movimento que nunca chegou à SDL. A correção
  pertence ao firmware/device tree; o framework apenas registra a topologia
  real e mantém os bindings fantasmas inativos;
- `nxinput_get_pad()` mantém a leitura original. Duplicar um D-pad completo no
  stick esquerdo ausente exige opt-in por leitura e nunca sobrescreve um eixo
  físico mapeado, mesmo quando ele está centralizado;
- hotplug por evento, com rescan periódico como recuperação caso o loop perca um evento;
- desconexão e perda de foco zeram botões, sticks, gatilhos e movimento do cursor. Os
  releases correspondentes ficam consumíveis, e presses ainda pendentes são descartados
  para não reaparecerem como ações fantasmas;
- deadzone radial com rescale e histerese separada para entrar/sair do neutro;
- todo tap observado por evento deixa um latch de press, mesmo se o botão já estiver
  solto quando a engine fizer seu próximo poll;
- `BACK/SELECT + START` apenas cria um pedido sticky de quit. O port decide quando
  executar pause/save/flush/teardown e nunca é encerrado pela biblioteca;
- cursor opcional, normalizado em `0..1`, progressivo, suavizado e baseado em tempo por
  frame. Por padrão ele usa exclusivamente stick direito + R3 e somente no
  contexto `MENU`. Um adapter pode pedir por chamada o stick esquerdo apenas se
  o pad aberto provar que o direito não existe; o clique continua em R3. D-pad
  e A nunca fazem parte do cursor.

O core `nxinput` não abre `/dev/input/eventN`, `jsN`, `gptokeyb` ou `uinput`.
O header opt-in de chord mantém seu fallback evdev estreito e separado. O core fica
silencioso no fluxo normal; quando uma autoridade é recusada, o motivo estável
(`syntax-invalid`, `unreachable`, `readback-mismatch`, `duplicate-divergent`,
…) fica registrado em `step_reason[]` da decisão, legível por
`nxinput_mapping_authority()`.

## GPTK editável no runtime (0.7.3)

`nxinput-gptk` é SDL-free e serve igualmente a adapters SDL2 e SDL3. O fluxo
canônico é `nxinput_gptk_load_at()` → parser/allowlist → dispatcher → sinks reais
do adapter. O loader recebe descritores já abertos do diretório do dono e de
`defaults/`, lê somente o basename fixo `NEXTOSCONTROLLERS.gptk` com
`O_NOFOLLOW`, aceita apenas arquivo regular e limita a leitura a 64 KiB.

O default imutável é validado primeiro. Um owner válido vence; owner ausente ou
inválido é preservado e o default vale somente naquela sessão. Nenhum arquivo é
criado, corrigido ou removido pelo loader. O receipt
`nxinput-gptk-load-evidence/1` registra `source`, tamanhos, códigos NXI e SHA-256
do default/selecionado (e do owner quando ele pôde ser lido seguramente), sem
paths, identidade do controle ou conteúdo do mapping.

O loader não registra nem inventa sinks. Cada adapter continua obrigado a
registrar callbacks reais para suas ações. Para impedir SDL+evdev duplicados,
um `nxinput_gptk_source_guard` separado preserva a ABI do dispatcher;
`nxinput_gptk_dispatcher_set_primary_mask()` declara por controle o mapping
completo SDL/PortMaster e `nxinput_gptk_dispatcher_feed_source()` faz a
deduplicação PRIMARY/FALLBACK: um press físico gera uma entrega lógica, e a
release só sai quando todas as observações daquele controle estão soltas.

`nxinput_gptk_live` fecha a fronteira que o parser/dispatcher isolado não pode
provar. Ela nasce **UNPROVEN**: enquanto a engine não fornecer um contexto com
fonte de evidência e todas as ações não tiverem sink real capaz de confirmar a
entrega, `nxinput_gptk_live_should_consume()` é falso e o evento segue uma vez
pelo caminho nativo. Não existe fallback implícito de foco ausente para
gameplay. Depois de `register`/`register_vector` e `seal`, o adapter pode ativar
um contexto por `set_context`; somente então ACTION ou `null` podem impedir o
caminho nativo. Falha de ACK após chamada do sink retorna `FATAL`, invalida a
sessão e nunca duplica a ação por replay nativo.

O executável opt-in publica `nxinput-gptk-runtime/3`; a release exige ainda a
prova externa `nxinput-gptk-event-evidence/1`, ligada ao mapping, contrato,
geração e ELF exatos. Marker ou log de load isolado não comprovam dispatch.

O chord principal também é SDL-version-neutral: `nxinput_exit_chord_poll()`
recebe um callback que devolve SELECT/START lógicos por pad. Um wrapper SDL2 usa
GameController; um wrapper SDL3 usa Gamepad. A máquina sticky não consulta GPTK
nem depende do loop do jogo receber a ação. `nx_evdev_chord_set_primary_active()`
permite aos dois wrappers manter o evdev independente ativo somente quando a
fonte primária não possui os dois binds.

## Receipt M15

O contrato verificável dos 22 itens do M15 está em
`references/m15-input-contract-v1.json`. O gate puro pode ser executado com:

```sh
python3 -B framework/nxinput/tests/test_m15_input_contract.py
```

Ele aceita como evidência positiva somente Bully 2, Sonic 4 EP2, Horizon Chase,
KOTOR e ASM2 1.2.7, mantém mapping, offsets, keycodes, touch, callbacks e
shutdown específicos no adapter e rejeita dados privados e fontes não aprovadas.
O estado é `closed_for_framework`: os 22 itens têm contrato, teste e fronteira
adapter-specific explícitos.

`references/m15-runtime-receipt-v1.json` separa três níveis que não podem ser
confundidos: a aceitação humana anterior dos cinco ports finalizados, observações
físicas no GO-Super e observações virtuais de hotplug/desconexão. O recibo não
afirma que input virtual é controle físico e não atribui ao agente uma nova
validação manual de gameplay. Uma combinação nova de aparelho/firmware continua
precisando de aceitação própria antes de virar promessa pública; isso é gate da
release, não pendência do contrato M15.

## Receipt forte para nxcompat

`nxinput_nxcompat_publish_context()` recebe somente um `nxinput_context` opaco já
criado. A existência desse contexto representa o contrato concluído de subsistemas
GameController ativos, mappings herdados aplicados, scan inicial executado e event
watch habilitado. A ponte ainda consulta `nxinput_connected_count()` e os quatro
slots com `nxinput_get_pad()`, rejeitando divergência entre a contagem agregada e os
slots.

O receipt publicado contém apenas flags finitas, quantidade conectada e uma geração
de topologia derivada das gerações dos quatro slots. Nome do controle, GUID,
`instance_id`, botões e eixos não são copiados. Um pad realmente aberto comprova que
há mapping SDL em runtime; sem pad, a ponte não inventa mapping e preserva uma
observação independente que o probe já tenha recebido do PortMaster/banco.

A ponte não chama `nxinput_create()`, não drena eventos, não faz poll, não abre um
device e não simula hotplug. O loader a chama depois da criação e novamente após uma
mudança de topologia observada no seu fluxo normal. Publicar a mesma geração outra
vez é rejeitado como stale; falha nunca altera o registry anterior.

`input.hotplug` exige simultaneamente event watch **e** rescan ativo. O contexto
opaco atual prova o watch, mas não expõe se `rescan_interval_ms` foi desabilitado;
por isso esta bridge fica fail-closed e não anuncia `RESCAN_ACTIVE` nem satisfaz
`input.hotplug`. O publisher genérico aceita o receipt completo de um adapter que
tenha ambas as provas. Um accessor futuro do estado interno poderá elevar a bridge
sem transformar a configuração default em suposição universal.

## PortMaster e mapping

O launcher continua carregando `control.txt`/`get_controls` quando disponíveis e só
exporta valores reais:

```sh
[ -n "${sdl_controllerconfig:-}" ] &&
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
```

Quando a integração PortMaster deixa `ANALOGSTICKS` (ou o alias legado
`ANALOG_STICKS`) disponível após `get_controls`, o launcher aceita exatamente
`0`, `1` ou `2` e exporta o valor
sanitizado em `NXINPUT_ANALOG_STICKS_HINT`. Valor ausente ou inválido fica
unset. Essa informação descreve o controle integrado do host para diagnóstico;
ela não identifica um pad USB/hotplug, não cria bindings e não habilita fallback.

Isso precisa ocorrer antes do loader iniciar. Não grave GUID, índice, nome de
device ou mapping escolhido por firmware dentro de `nxinput`. A única recuperação
comum aceita em 0.3.0 exige a assinatura completa e contraditória documentada
acima; qualquer perfil parcial, identidade específica ou ordem não comprovada
continua pertencendo ao adapter do port.

## Integração no loop

```c
nxinput_config input_config;
nxinput_context *input;

nxinput_config_init(&input_config);
input = nxinput_create(&input_config);
if (!input) {
    fprintf(stderr, "input: %s\n", SDL_GetError());
    return 1;
}

while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        nxinput_observe_event(input, &event);

        /* Obrigatório: nxinput só observa. O port ainda entrega o MESMO evento
         * à janela, lifecycle e handlers nativos da engine. */
        game_observe_event(&event);
        if (event.type == SDL_QUIT)
            request_safe_shutdown();
    }

    nxinput_poll(input);
    if (nxinput_quit_requested(input))
        request_safe_shutdown();

    /* A ponte deste jogo lê nxinput_get_pad() e converte o contrato Xbox para
     * callbacks JNI, HID, keycodes Android ou estruturas internas reais. */
    update_game_input(input);
}

nxinput_destroy(input);
```

Não chame `SDL_PollEvent` dentro de outra camada de input: só o loop dono da aplicação
drena a fila. Para uma engine que gerencia focus fora dos eventos SDL, chame
`nxinput_set_focus(input, 0/1)` no ponto equivalente do lifecycle.

### Slots, hotplug e multiplayer

`nxinput_get_pad(input, slot, &state)` expõe `connected`, `instance_id` e
`generation`. O slot é armazenamento local; `instance_id` identifica o pad naquela
conexão. A geração muda em connect/disconnect e quando um remap altera a topologia
de bindings, permitindo que a ponte anuncie o lifecycle no momento nativo correto
da engine.

Em multiplayer, percorra os quatro slots e preserve cada identidade. Em jogo
single-player, `nxinput_first_connected()` encontra o primeiro slot vivo. Nunca reutilize
um índice antigo recebido em `SDL_CONTROLLERDEVICEADDED`: naquele evento o campo é um
índice de abertura; depois de aberto, a identidade é o instance ID.

### Polling e latches

O estado atual fica em `state.buttons` e nos seis floats normalizados. Eixos de stick
usam `-1..1`, com Y no sentido SDL (positivo para baixo); gatilhos usam `0..1`. Inverta Y
somente na ponte cuja engine realmente exigir.

Para não perder um toque de 10 ms numa engine a 30 Hz:

```c
uint32_t press = nxinput_consume_pressed(input, slot,
                                         NXINPUT_BUTTON_MASK_ALL);
uint32_t release = nxinput_consume_released(input, slot,
                                            NXINPUT_BUTTON_MASK_ALL);
dispatch_edges_to_engine(press, release);
```

O consumo é seletivo por máscara e não muda `state.buttons`. Down/up repetidos são
deduplicados. Num boundary de focus/hotplug, releases são preservados e presses pendentes
são limpos por segurança.

### Topologia e fallback digital opt-in

Consulte a topologia resolvida do slot antes de adaptar um jogo que exige eixo:

```c
uint32_t capabilities;
nxinput_pad_state state;

if (nxinput_get_pad_capabilities(input, slot, &capabilities) &&
    !(capabilities & NXINPUT_PAD_CAP_LEFT_STICK)) {
    nxinput_get_pad_with_options(
        input, slot, NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING, &state);
} else {
    nxinput_get_pad(input, slot, &state);
}
```

O opt-in só produz `left_x/left_y` quando o mapping possui as quatro direções do
D-pad e não possui nenhum eixo do stick esquerdo. Diagonais são normalizadas, os
bits do D-pad continuam disponíveis e o estado armazenado permanece cru. Um GUID
que o SDL não reconhece como `SDL_GameController` continua exigindo correção no
banco/mapping do PortMaster ou firmware; `nxinput` não adivinha um joystick raw.

### Cursor contextual

O port deve habilitá-lo por estado real da UI, nunca por temporizador:

```c
nxinput_cursor_state cursor;

nxinput_set_cursor_context(input, menu_open ? NXINPUT_CURSOR_MENU
                                             : NXINPUT_CURSOR_GAMEPLAY);
nxinput_cursor_update_with_options(
    input, slot, frame_seconds,
    NXINPUT_CURSOR_OPTION_LEFT_STICK_IF_RIGHT_MISSING, &cursor);
if (cursor.active) {
    draw_polished_arrow(cursor.x, cursor.y); /* asset/seta do port */
    if (nxinput_cursor_consume_click(input, slot))
        send_paired_touch_at(cursor.x, cursor.y);
}
```

Sem o opt-in acima, `nxinput_cursor_update()` preserva literalmente o contrato
anterior de stick direito. Mesmo com opt-in, o stick esquerdo só move a seta no
contexto `MENU` e quando ambos os bindings direitos são inalcançáveis no joystick
real. O latch do clique do cursor é separado do latch normal de R3: observar/consumir
um não rouba o outro. Em gameplay, a API de cursor para completamente e a ponte
continua entregando os controles à câmera ou ação original. O desenho da seta e o
touch DOWN/UP pareado pertencem ao port, pois dependem do drawable e da API real da
engine.

## Build e teste

Standalone:

```sh
cmake -S framework/nxinput -B build/nxinput \
  -DNXINPUT_BUILD_TESTS=ON \
  -DNXINPUT_BUILD_NATIVE_TESTS=OFF \
  -DNXINPUT_WITH_NXCOMPAT=ON
cmake --build build/nxinput
ctest --test-dir build/nxinput --output-on-failure
```

Como subdiretório do loader:

```cmake
add_subdirectory(path/to/framework/nxinput)
target_link_libraries(game-loader PRIVATE nxinput)
```

Esse comando é o gate hermético M12: executa o gate estático e o teste separado de
`nxinput_nxcompat`, com contexto e quatro slots inteiramente fake. Ele valida
conexão/desconexão, geração monotônica, hotplug fail-closed sem prova de rescan e
agregação sem identificadores. Não chama `nxinput_create()`, não enumera SDL nem acessa
`/dev`, rede, sessão ou controle físico.

`NXINPUT_BUILD_NATIVE_TESTS=ON` habilita uma bateria separada/manual que liga o core
ao subsistema SDL nativo e pode enumerar GameController. Ela cobre extremos dos eixos,
gatilho, deadzone, latches, lifecycle, cursor e, quando suportado, GameControllers
virtuais com zero, um e dois sticks para provar o limite do fallback, além de
mapping/poll/hot-unplug. Essa suíte native/hardware-facing não pertence ao
gate hermético M12 e só deve rodar intencionalmente. Validação física de input/hotplug
no aparelho continua sendo um gate posterior, nunca uma consequência do teste host.

Um gate estático (`nxinput-static-gate`) complementa a bateria: ele falha se o
fonte de `nxinput` citar nome de CFW, device, firmware ou VID/PID.

A bateria da autoridade soberana é `tests/run-sovereign-corpus-host.sh`
(ctest `nxinput-sovereign-corpus`, SKIP 77 sem o corpus selado): unidade
(`test_sovereign`) e adaptador de produção (`test_authority`, com hotplug,
dois pads de mesmo GUID, setter hostil e recusa antes do gameplay) em GCC,
Clang, ASAN e UBSAN; auditoria estática da rota única; a prova diferencial de
TODAS as entradas de TODOS os bancos do corpus contra uma referência
independente (`tests/sovereign_reference.py`); e a prova de mutante — o
resolver é recompilado com um binding trocado e o gate **tem** de rejeitá-lo.

## O que continua específico do jogo

`nxinput` resolve aquisição, identidade e normalização. A ponte final não pode ser
universalizada sem conhecer a engine: assinatura JNI, ordem de callbacks, keycodes,
estrutura interna, tela touch-only e o momento de anunciar conexão continuam sendo
confirmados no binário de cada jogo. Essa separação evita que um workaround de um port
altere o fluxo nativo de todos os outros.

## Chord de saída SELECT+START (`nxinput_exit_chord.h`, 0.5.1)

A política principal é `nxinput_exit_chord`: máquina de estado C99 sem tipos
SDL, três polls por padrão, SELECT e START no mesmo pad, pedido sticky consumido
uma vez e nenhum refire durante o mesmo hold. SDL2/SDL3 apenas adaptam suas APIs
de estado ao callback lógico. Perda de foco/hot-unplug chama
`nxinput_exit_chord_reset_hold()`; um pedido já pendente nunca é apagado.

### Wrapper SDL2 + fallback (`nxinput_evdev_chord.h`)

Header único, opt-in, com a política **canônica** de saída dos ports (todo port
novo usa este header; cópias vendorizadas divergentes são proibidas):

1. **SDL é a autoridade** quando há `SDL_GameController` aberto: o chord é
   `SDL_GameControllerGetButton(BACK) && (START)` lido por **estado** em todo
   pad aberto, mais o botão **cru** do joystick nos índices que o próprio mapping
   declara para back/start. Dispara na borda após 3 polls (~50 ms) — sem hold
   longo (hold de 1 s foi relatado como "não sai"). É o mesmo caminho dos ports
   públicos que saem normal em todo CFW.
2. **evdev cru é só fallback sem pad SDL** (pad sem mapping): heurística
   `TRIGGER_HAPPY1/2` → `BTN_SELECT/START` → `BTN_BASE3/4`, os dois no MESMO
   device.
3. **Nunca** vigiar `BTN_SELECT/START` literais com pad SDL aberto: na família
   H700 o driver emite esses códigos para **L2/R2** ("L2+R2 sai").
4. **Nunca** derivar código evdev do índice SDL para o chord: SDL2 vanilla
   enumera `BTN_JOYSTICK..KEY_MAX` e depois `0..BTN_JOYSTICK-1`; a SDL2 dos CFWs
   Batocera-like (patch `sdl2_input_as_retroarch_udev`) enumera `0..KEY_MAX`
   crescente. Mesmo device + mesmo mapping = tabela índice→código diferente. A
   versão anterior derivava com a tabela vanilla e virou SELECT+START em L3+L2
   nesses CFWs (Forager 1.0.1/1.0.3). As duas tabelas ficam só para diagnóstico.
5. **Diagnóstico obrigatório**: `nx_exit_chord_log_controller()` imprime nome,
   GUID, mapping e binds do pad SDL e, para cada `/dev/input/event*` de gamepad,
   nome, ids e a lista completa de códigos de tecla, mais as duas tabelas
   índice→código. Um log de qualquer device explica bug de chord sem hardware.

Uso por frame: `nx_exit_chord_update(pads, n)` (bind do 1º pad + SDL primário
|| fallback evdev). Teste de host com fixtures reais (H700 Knulli/muOS,
RK3326 GO-Super): `tests/test_evdev_chord_map.c`.

## Ordinal pad fix (`nxinput_pad_ordinal_fix.h`, 0.4.4)

Header único, opt-in, com a **fonte única** da correção "ordinal" — antes
copiada, com oito conteúdos diferentes, em 22 ports.

Kernel antigo (3.14, Amlogic-old) sem driver HID específico entrega o controle
externo pelo `hid-generic` e enumera os botões pela **ordem do report**,
publicando `BTN_C`/`BTN_Z`. Um mapping autorado em kernel moderno — ou por
etiqueta física — passa a apontar para outras posições e A/B, X/Y saem
trocados. O header registra um mapping SDL pela ordem **física** da classe do
pad: `NXINPUT_PAD_ORDINAL_LAYOUT_HID` (padrão) e
`NXINPUT_PAD_ORDINAL_LAYOUT_ALT`, a segunda classe conhecida de ordem de
report. **Qual delas vale para um pad é fato de identidade (VID/PID), que
pertence ao port ou ao nxcompat — nunca a este módulo**: por isso o layout é
parâmetro do chamador, e `<PREFIX>_PAD_ORDINAL_LAYOUT=hid|alt` sobrescreve em
campo.

A troca só acontece com a assinatura **completa**:

1. o pad exposto pela SDL tem VID/PID (GUID com CRC não basta);
2. existe um evdev com o mesmo VID/PID;
3. esse evdev está num **barramento externo** (`BUS_USB` ou `BUS_BLUETOOTH`);
4. o bitmap `EV_KEY` tem `BTN_GAMEPAD` e `BTN_C` ou `BTN_Z`.

O item 3 é o **gate `BUS_HOST`**, e não é detalhe: o pad interno do H700
(família RG40XX-H/RG35XX/RG34XX e o Flip) publica `BTN_C`/`BTN_Z` com layout
semântico **correto** e casaria com a assinatura antiga. Sem o gate, a
"correção" trocaria os botões de um controle que já estava certo. Controle
embutido (`BUS_HOST`, `BUS_I2C`, `BUS_SPI`, `BUS_VIRTUAL`) fica intocado, e
driver semântico moderno também não casa, porque não publica `BTN_C`/`BTN_Z`.

Uso, depois dos includes de SDL2:

```c
#include "nxinput_pad_ordinal_fix.h"
...
nxinput_pad_ordinal_fix_apply(index, "TITANSOULS",
                              NXINPUT_PAD_ORDINAL_LAYOUT_HID);
if (!SDL_IsGameController(index)) ...  /* ANTES de SDL_IsGameController */
```

Env: `<PREFIX>_ORDINAL_FIX=0|off` desliga; `<PREFIX>_PAD_MAP` (mapping manual)
tem prioridade e suprime a correção. Retorno `1` = mapping trocado.

O núcleo de decisão (barramento, assinatura, mapping) não chama SDL nem abre
device: roda no PC com as tabelas evdev **reais** de
`framework/tests/fixtures/controls/pad-ordinal-v1.json`, pelo gate
`tests/test_pad_ordinal_fix.py`. A fixture do H700 registra a fronteira em
número: `signature_ungated=1`, `signature=0`.

O nxrelease 0.2.27 recusa, em build-time, uma árvore de fontes que defina a
correção **sem** esse gate. Nenhum port é migrado automaticamente: cada um
adota o header canônico no seu próximo rebuild individual.

## 0.10.2 — nxinput_padset e a prova automática no aparelho

**Interpretação corrigida (02/09/2026):** o framework é automático; a prova de
controles de um port não depende de uma pessoa apertando botões. A evidência
válida para compatibilidade é o receipt `ON_DEVICE_AUTOMATED_INPUT_PROOF`.

- `engine-glue/nxinput_padset.{c,h}`: todos os pads admitidos pela autoridade
  do port abrem ao mesmo tempo; união de botões; eixo = maior deflexão;
  SELECT+START só vale no MESMO instance (cross-pad é negado e registrado);
  hotplug compacta. Vtable sobre a SDL do firmware — o módulo não conhece SDL,
  env, dispositivo nem nome. Gate: `tests/run-padset-host.sh`.
- `tools/nx-device-input-proof.py` + `tools/nx-input-inject-agent.py`: no
  aparelho real, pelo `nx-device-launch`, captura o perfil do controle no
  kernel, cria ANTES do SDL_Init do jogo clones uinput device-faithful,
  confirma pelo log do port que a SDL do sistema os admitiu com o mesmo
  GUID/mapping, deriva controle→código só do mapping admitido e executa o
  roteiro declarativo gerado pelo nxgenerator (`controls.proof`). Receipt
  `nx-device-input-proof/1` com vereditos por janela e todos os hashes
  (device/CFW, SDL, GUID, capabilities, mapping, GPTK, adapter, ELF,
  generation, run). `nxrelease/nx-input-proof-lock.py` converte os receipts no
  candidate-lock externo. Gate: `tests/test_device_input_proof.py`.
- O helper nunca entra em ELF/ZIP; ports antigos aprovados não migram.

## V5 (0.11.0) — controls line

The rewrite target of the C6 seam is the PROVIDER DESCRIPTOR of the SDL the process really
mapped, never the major's upstream presumption: see `docs/CONTROLS-V5-PROVIDER-GRAPH.md`.
The owner file moves to `NEXTOS_CONTROLLERS/4`: see `docs/NEXTOS_CONTROLLERS-4.md`.
Directed V5 gates: `ctest --test-dir <build> -R nxinput-v5`.


## 0.11.1 (V5-M1a)
Universal pieces that used to live in ports: `nxinput_gptk4_preinit_load()` (schema-4 pre-init),
the vector gesture neutral floor (`nxinput_gptk_live`), the pre-router wired into
`nxinput_padset` with calibrated vectors, `nxinput_keyboard`, `nxinput_authority_v5`,
`nxinput_route_policy`, `nxinput_coexist`, `tools/nx-gptk4-host-gate` and the STOCK MODE of the
seam for unknown providers (never a mute pad). See `CHANGELOG.md` and
`docs/CONTROLS-V5-PROVIDER-GRAPH.md`.
