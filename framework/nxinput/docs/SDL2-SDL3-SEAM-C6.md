# A costura SDL2/SDL3 — contrato V4-CONTROLLERS-03 / C6

## O que este componente resolve

Um port PortMaster recebe um mapping soberano do CFW. A pergunta da C6 é
estreita e verificável: **esse mapping produz o mesmo controle canônico num
consumer SDL2 GameController, num consumer SDL3 Gamepad e num consumer
raw-evdev explicitamente registrado — sem o framework reinterpretar nada?**

A resposta não é uma opinião do framework. Ela é medida dentro de três
bibliotecas SDL reais, com a costura ligada no binário.

## Onde a costura fica, e por quê

`nxinput_sdl_seam_admit()` é chamada de dentro do backend Linux de joystick da
SDL, em `MaybeAddDevice()`, na própria thread da SDL, com o lock de joystick da
SDL, **imediatamente antes de `SDL_PrivateJoystickAdded()`**. Nesse ponto:

- nada foi anunciado — nenhum `SDL_JOYDEVICEADDED` / `SDL_EVENT_JOYSTICK_ADDED`
  e nenhum `SDL_EVENT_GAMEPAD_ADDED` foi empilhado;
- nada foi classificado — a SDL2 chama `SDL_IsGameController()` **dentro** de
  `SDL_PrivateJoystickAdded`, e a SDL3 responde `SDL_IsGamepad()` a partir da
  lista anunciada;
- nada foi aberto — `SDL_GameControllerOpen()` / `SDL_OpenGamepad()` não podem
  ter rodado para um device que a aplicação ainda não conhece.

Um pad que a costura não admite **não é anunciado**. É a única forma de uma
recusa ser honesta: o jogo não recebe um pad meio configurado, recebe pad
nenhum, e o port falha antes do gameplay exatamente como a C3 exige. A prova
disso é medida: nos cenários negativos, `SDL_NumJoysticks()`/`SDL_GetJoysticks()`
devolve zero.

## O que a costura NÃO decide

Ela não decide o mapping. A decisão é `nxinput_authority_admit()`, que é
`nxinput_sovereign_resolve()`, que é a ordem da C3, **literalmente**:

```
get_controls vivo → GUID oficial do CFW → bundle pinado → built-in →
raw declarado → falha explícita
```

Não existe uma segunda ordem local. `env`, arquivo e bundle **não** criam um
ranking paralelo: são os passos 1, 2 e 3 dessa ordem única, resolvidos por
código da C3. A costura entrega as fontes reais, as capacidades realmente
medidas e o setter/readback reais da SDL, e obedece.

Os seis passos foram exercitados **na API real**, cada um com o seu cenário:
`env-get-controls`, `cfw-db-guid`, `port-bundle`, `runtime-builtin`,
`raw-passthrough` e `fail-explicit`.

## As quatro regras de fronteira que a costura acrescenta

**1. A colisão de GUID.** A SDL guarda mappings **por GUID**; a C3 admite pads
**por instância**. Duas instâncias vivas com o mesmo GUID não podem sustentar
mappings divergentes numa única SDL. A C3 mantém as entradas independentes, e
está certa; a SDL não consegue. A costura arbitra isso explicitamente: bytes
idênticos são aceitos (dois pads iguais são o caso normal), divergentes falham
fechado. A ordem de chegada nunca decide.

**2. O staging antes do `SDL_Init`.** A SDL importa `SDL_GAMECONTROLLERCONFIG`
durante a inicialização com prioridade **USER**, acima da prioridade **API** de
qualquer mapping que a costura instale depois. Deixada no ambiente, a variável
vence em silêncio e a decisão soberana vira decoração.
`nxinput_sdl_seam_stage_before_init()` copia os bytes para armazenamento do
chamador e **remove** a variável, antes da SDL subir. Os bytes não são
descartados: continuam sendo a autoridade 1 quando a costura resolve — apenas
deixam de contornar a decisão. Staging depois de `SDL_WasInit(0) != 0` falha,
em vez de fingir: já não dá para provar que a variável não foi importada.

**3. O word CRC de nome no GUID de bus da SDL3.** A SDL3 preenche os bytes
2–3 do GUID ao vivo com um CRC16 do nome do device, mas a própria busca de
mapping limpa esse word antes de comparar um banco. Bancos PortMaster/SDL2
podem, portanto, trazer `0000` ali para a mesma identidade. Isso apareceu no
GO-Super físico: o runtime entregou
`1900bb3e4b4800000011000000010000` e o banco oficial trouxe
`190000004b4800000011000000010000`.

A costura projeta somente essa igualdade nativa da SDL antes da C3: a linha
precisa ter `0000` no word CRC, o GUID ao vivo precisa ter valor não zero, os
outros 28 dígitos hexadecimais precisam coincidir byte a byte e o bus precisa
ter a forma de bus aceita pela SDL. O resultado passa normalmente por sintaxe,
capacidade medida, setter e readback; não existe autoridade nova. Desde a
0.7.2 a regra vale também na SDL2: as duas SDL2 executadas (2.28.5 e 2.32.10)
gravam o mesmo CRC16 de nome no GUID ao vivo e caem para a entrada sem CRC na
busca de mapping — foi exatamente isso que o GO-Super repetiu na rota SDL2 do
dArkOS. Uma SDL2 anterior à 2.26 entrega word zero e fica intocada por
construção. Nome, modelo, VID/PID ou qualquer diferença fora do word CRC
nunca aproximam identidades. Desde a 0.8.0, quando a fonte já contém a linha
exata do GUID vivo, a projeção de CRC nem roda — a entrada exata vence, como
no lookup da própria SDL. Duplicata divergente que ainda se materialize no
mesmo store resolve pela última linha (semântica do `AddMapping`), contada no
receipt como `dup_lastwins`; não é mais veredito terminal.

**4. O domínio joydev legado provado pelas capabilities.** A ROM oficial do
muOS 2601.1 para RG40XX-H publica uma linha `Deeplay-keys` cujos `bN` não são
os ordinais evdev da SDL2 presente no próprio sistema. Eles percorrem todos os
`EV_KEY` em ordem crescente: `KEY_ESC`, volume down, volume up e depois
`BTN_GAMEPAD...`. A SDL2/SDL3 atual percorre primeiro a faixa de gamepad e só
depois as teclas baixas. Aplicar os 315 bytes literalmente faz botões físicos
sumirem ou chegarem como outro controle — em particular, Start físico vira L1
e o Start lógico fica no L2 físico.

A 0.9.0 projeta somente a representação ordinal. O predicado não contém nome
de device, CFW, board, VID ou PID: exige o bitset `EV_KEY` do event node exato,
`BTN_GAMEPAD`, teclas abaixo de `BTN_JOYSTICK` e os dois bindings de metadata
`volumedown`/`volumeup` apontando exatamente para `KEY_VOLUMEDOWN` e
`KEY_VOLUMEUP` no domínio crescente. Sem essa prova, o source é no-op. Com
ela, cada `bN` é convertido pelo código evdev intermediário para o ordinal da
SDL ativa; GUID, nome, eixos, hats, metadata, autoridade e escolha semântica
permanecem intactos. O resultado ainda precisa passar pelo parser soberano,
alcance medido, setter real e readback efetivo. O receipt torna a projeção
auditável por `domain_lines`, `domain_bindings`, `source_domain` e
`target_domain`.

## Os domínios ordinais, medidos e não presumidos

Um mapping é uma lista de ordinais. Se a fonte e o consumer enumeram igual, o
mapping atravessa a fronteira **byte-intacto** e qualquer conversão o
corromperia; se divergem, a representação precisa ser projetada sem alterar a
semântica. Presumir qualquer um dos dois lados é bug, então
`tests/c6_domain_gate.py` mede os consumers por pin e o adapter joydev exige a
capability proof do source em cada event node.

Resultado medido nos pins da C6:

| pin | papel | domínio | scan de eixos |
|---|---|---|---|
| SDL 2.0.10 | somente auditoria de fonte | `sdl2-legacy-evdev` | pula **toda** a faixa ABS_HAT |
| SDL 2.28.5 | **mínima suportada**, executada | `sdl2-evdev` | pula só os pares detectados |
| SDL 2.32.10 | atual, executada | `sdl2-evdev` | pula só os pares detectados |
| SDL 3.2.30 | **SDL3 privada pinada**, executada | `sdl3-evdev` | pula só os pares detectados |

As três executadas enumeram botões/eixos **identicamente entre si**. Logo não
há conversão SDL2 atual → SDL3 atual. Isso não torna toda fonte PortMaster
evdev: o dialeto `joydev-legacy` do muOS percorre os códigos em ordem crescente
simples e diverge das três. A outra divergência medida é a SDL 2.0.10, cujo
scan de eixos derruba a faixa ABS_HAT inteira — o que muda a numeração num pad
com um eixo ABS_HAT sem par. A projeção do word CRC é uma regra de identidade
separada de ambas.

## A autoridade 4 não responde na SDL3 — e isso é estrutural

Esta é a descoberta mais consequente da C6 para quem adota SDL3, e ela foi
**auditada nas fontes pinadas**, não deduzida de um run vermelho:

- **SDL2** — `SDL_JoystickInit()` chama `SDL_GameControllerInitMappings()`
  **antes** do laço `SDL_joystick_drivers[i]->Init()`. Quando `MaybeAddDevice()`
  roda, o banco embutido já está carregado: a autoridade 4 **responde**.
- **SDL3** — `SDL_Init(SDL_INIT_GAMEPAD)` inicializa **por inteiro** o
  `SDL_INIT_JOYSTICK` primeiro — que é justamente onde `MaybeAddDevice()` e
  portanto a costura rodam — e só depois chama `SDL_InitGamepads()`. Quando a
  costura pergunta, o banco de mappings da SDL3 ainda não existe: a autoridade 4
  **não responde**.

`tests/c6_domain_gate.py` fixa os dois arquivos por SHA-256 e exige a ordem
literal, então isso é gate, não observação.

**Consequência prática para um port SDL3:** ele precisa **declarar** uma fonte
(autoridade 1, 2 ou 3). Não dá para contar com o banco da própria SDL3 nessa
fronteira. Se nada acima responder, a ordem chega ao passo 6 e o pad é
**recusado antes do gameplay** — fail-closed e correto, mas o port tem de saber
disso antes de descobrir no aparelho.

Não "consertamos" isso fazendo a costura carregar os mappings da SDL3 mais cedo.
Isso seria **substituir** o fluxo nativo da engine em vez de interceptá-lo, que
é exatamente o que a regra da casa proíbe. Um port que precise da autoridade 4
na SDL3 deve chamar `SDL_Init(SDL_INIT_GAMEPAD)` e só então declarar; o que a C6
garante é que a recusa é explícita e auditável, nunca um pad meio configurado.

## O tipo de saída é do controle, não do binding

Uma entrada oficial do corpus (OpenSimHardware OSH PB Controller) liga os dois
gatilhos a **botões**: `lefttrigger:b10`, `righttrigger:b11`. A SDL continua
reportando L2/R2 pelo caminho de **eixo** (0 → 32767 → 0), porque
`SDL_CONTROLLER_AXIS_TRIGGERLEFT` é um eixo — o tipo de saída vem do controle,
nunca do que o mapping ligou nele. O mesmo vale para os dois sticks. A referência
independente da C6 modela isso em `AXIS_OUTPUT_GROUPS`, e a bateria confere L2/R2
ligados a botão no caminho de eixo, com press e release.

## Limites do claim, escritos de propósito

- **`SDL_Joystick` e raw direto.** A costura age antes do anúncio, então um pad
  recusado some **também** da lista de joysticks — isso é medido, não
  argumentado. O que ela **não** dá é supressão por controle nessa API: um `a`
  sem binding é um conceito de GameController/Gamepad, e um botão de joystick
  cru não tem essa noção. O caso `A/B=null` é provado nos eventos **e** no
  polling de GameController/Gamepad; para `SDL_Joystick` e para um jogo que abre
  `/dev/input/event*` por conta própria, a supressão por controle fica
  **`UNPROVEN`**. Um consumer raw só entra pela autoridade 5, e só quando o port
  **declara** que ele entende pad cru.
- **Físico.** Os pads da bateria são devices `uinput` reais: o kernel os cria,
  responde `EVIOCGBIT`/`EVIOCGABS` por eles e entrega seus eventos pelo mesmo
  caminho de um pad físico, e a SDL não os distingue de hardware. Ainda assim
  **não são prova física**: compartilham o kernel deste host, não o do aparelho.
  A validação física continua **`PENDING_PHYSICAL`**.
- **Engines nativas arbitrárias** ficam fora do claim. A C6 fala de SDL2, SDL3 e
  do consumer raw declarado, e de mais nada.

## Adoção

O alvo fica desligado por padrão: sem `NXC6_SEAM` a costura é inerte e a SDL se
comporta exatamente como upstream. Cada port adota por branch própria, bump de
versão e ZIP novo, pinando a SDL por versão e SHA-256 exatos — nunca `latest` e
nunca a SDL do sistema. `engine-patches/C6-SDL-PROVENANCE.json` guarda a cadeia
fonte → licença → patch → binário; `tests/c6_provenance_gate.py` a **impõe**,
inclusive reconstruindo os bytes upstream ao reverter o patch.

## 0.10.0 — banco vivo, FACE_LAYOUT e evidência de admissão

- **Banco vivo com espera limitada** (`nxinput_livedb`, autoridade 2): quando
  a autoridade 1 está vazia e nenhum `SDL_GAMECONTROLLERCONFIG_FILE` foi
  declarado, o glue adquire uma vez por admissão o banco canônico de runtime
  (`/usr/lib{,32}/gamecontrollerdb.txt`, escolhido pela largura de ponteiro
  do processo). Symlink morto: até 20 tentativas de 25 ms sob teto absoluto
  de 500 ms monotonic, EINTR-safe; snapshot estável com identidade
  re-verificada; loop/FIFO/diretório/device/oversize/NUL cedem sem bloquear.
  Um caminho declarado ilegível cede sem autorizar busca. O snapshot entra na
  ordem exatamente como autoridade 2 — env viva continua vencendo e o bundle
  continuando abaixo.
- **`nxc6_declare_port_bundle_for_layout(gamedir, layout)`**: `auto` declara
  `controllers.nxb`; `modern`/`retro` declaram a variante autenticada. O
  retorno `0` segue benigno (o port não traz bundle); somente `< 0` é erro.
  O wrapper legado `nxc6_declare_port_bundle()` significa `auto`.
- **Receipt em dois sinks**: cada linha vai ao arquivo durável
  (`NXC6_RECEIPT`) e ao stderr do processo (log normal/support bundle). As
  linhas de decisão carregam agora `name=` (evidência sanitizada, via
  `nxc6_admit_before_announce_named` — os três patches SDL passam
  `item->name`), `db_class/db_target/db_retries/db_elapsed_ms`,
  `face_layout=`, `effective_guid=`, `map_fnv1a64=`/`map_bytes=` e a linha
  `NXC6-DOMAIN` com as contagens da classificação semântica de domínio
  (native/identical/ambiguous/invalid). Nunca IP, hostname, path pessoal ou
  conteúdo hostil cru.
- **Domínio por prova semântica**: a projeção joydev→evdev não depende mais
  exclusivamente dos marcadores de volume; cada `bN` é interpretado nos dois
  domínios contra o bitmap `EV_KEY` medido e uma fonte ambígua/ inválida faz
  a FONTE ceder (`NXC6-DOMAIN ... result=source-yields`), nunca passar em
  silêncio no domínio errado.
