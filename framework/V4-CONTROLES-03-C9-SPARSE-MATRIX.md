# V4-CONTROLLERS-03 / C9 — matriz esparsa de evidência

Estado da frente: `INTEGRATED_CLOSED` (fechamento V4, tag `framework-v4`, 02/09/2026)

Estado físico dos bytes integrados: `PENDING_PHYSICAL` para a prova histórica por dedo humano (não realizada, não bloqueante); prova AUTOMÁTICA no aparelho realmente obtida: `ON_DEVICE_AUTOMATED_INPUT_PROOF` 3/3 para Nameless Cat 1.2.7, Freedom Planet 2 1.1.3, Blossom Tales 1.4.0 e Tearscape 0.2.16 no dArkOS/K36S (receipts em `ports/<port>/proofs/on-device-20260902/`, locks por `nx-input-proof-lock`). RP5/ROCKNIX: nenhuma prova (`UNPROVEN`, best effort)

V4 permanece aberta: `NO` — fechada em `framework-v4`; mudanças posteriores = V5

Esta matriz contém somente pares realmente exercitados. Uma linha de host não
é suporte físico a CFW, e a captura física histórica não é promovida ao novo
HEAD integrado. Célula ausente significa ausência de prova, nunca suporte
implícito.

## Vocabulário e identidade integrada

Classes permitidas: `PHYSICAL`, `REAL_API_HOST`, `FIXTURE`, `PENDING`,
`UNPROVEN` e `N/A`. A linha P01 usa `PHYSICAL` somente para a cadeia de mapping
que foi de fato lida no aparelho histórico; seus consumers continuam
`UNPROVEN`.

Vetor canônico C18, sempre nesta ordem:

`A, B, X, Y, L1, R1, L2, R2, L3, R3, START, SELECT, UP, DOWN, LEFT, RIGHT, LEFT_STICK, RIGHT_STICK`.

Owner integrado comum:

- nxinput 0.7.0: head `cc9db69be3d98a99f34a53e9d3413b0d7495083c`,
  subtree `3e59ae088b6b10525742cc237a9a1a1c3e5f7b41`;
- nxandroid 0.5.0: head `182e3b8e08410158996af2c8b10901e6ea2c82a3`,
  subtree `098316c0f96417bb3bf9ec49d391a735c902f546`;
- nxobs 0.4.2: head `6f4d3c7a8e45f4ef55d4d7eaf96af661c0e98435`,
  subtree `c25a616326eb1378b4d979405dcfbfaed27a402c`;
- nxgenerator 0.3.2: head `746a8a260656d6c3f034b5cf40a415ea16167f1a`,
  subtree `1c8cdbaf4c9351dc995c617d0a88bb165d825b46`;
- nxrelease 0.3.2: head `384eeb28272f3fa0c0ac24f572364f4045b2640b`,
  subtree `7d7b1073388828ec37e4d504feb0ddfad0e138c9`.

## Pares realmente exercitados

| ID | CFW/build + engine/perfil/versão | Artefato e pin | Classe | Provider/path do mapping | GUID/capabilities sanitizados |
|---|---|---|---|---|---|
| P01 | dArkOS/Debian 13, RK3326 GO-Super + PortMaster `get_controls` instalado | captura C1; corpus `c5a2da52a428859dee97441aa174451d4554293ad159047991ecd42d6143cb68`; upstream `8d3a5db24bb0e8944b71242c6a00c56a08702fe4` | `PHYSICAL` somente mapping histórico | `/opt/system/Tools/PortMaster/control.txt` → `get_controls` → `/tmp/gamecontrollerdb.txt`; linha SHA-256 `da0447deece5800c7c845652cb1a73893e706cbdb08601e33a2b74c5da3daa64` | GUID `190000004b4800000011000000010000`; `ANALOGSTICKS=2`; mapping anuncia C18 completo; nenhum log cru/device address é guardado aqui |
| F01 | host hermético + autoridade soberana C3 / API nxinput 0.7.0 | log corrigido `dde4d2a4157a98074fcaa3b3862a7ab011a3d14bb9ccb50576f11152b2f79b19`; bundle/corpus C1 integral | `FIXTURE` | seis providers isolados: live, DB por GUID, bundle, builtin, raw declarado e recusa; bytes do vencedor preservados | GUIDs de fixture e todos os 28.875 registros do corpus; capabilities medidas por registro e digest ligadas à decisão |
| F02 | host hermético + NEXTOSCONTROLLERS v2 / GPTK API 2 | log `c8dbc27479bbf1ec53c5d823f0f3237a9af5a912e25ff06cd588438d9248a56e` | `FIXTURE` | decisão C3 congelada; arquivo do dono validado sem overwrite e default autenticado | identidade física não é necessária nesta camada; quatro slots e dois pads mantêm estado separado |
| F03 | host hermético + observabilidade de input C2 / receipt v1 | relatório `73de85a6a7f65d64629a817f89dc789f2bcd0084f3ebfcde75ebdb2f988deed2`; head nxobs acima | `FIXTURE` | replay da decisão, observado sem alterar o caminho de input | GUID, path e nome livre redigidos; capability mask, trigger kind e geração preservados de forma sanitizada |
| G03 | host sem GUI + Godot 3.5.3 `platform=server` | pins `43faf9b77d93653d37c3809ad62b65542bbb85910058eb5f10e30f1a2874f93a`; log C5B `523e38364623aeceb2668617f59224d5039803ba452ee4f5b0cfd44677e75bf1` | `REAL_API_HOST` | C3 provider allowlisted por cenário → setter/readback dentro do processo → anúncio | corpus de 24 GUIDs, 8 shapes, uma linha por GUID; capabilities lidas do nó virtual e ligadas ao digest da origem |
| G04 | host sem GUI + Godot 4.2.2 `--headless` | mesmos pins e log de G03 | `REAL_API_HOST` | igual a G03, usando API real Godot 4 antes de `joy_connection_changed` | mesmo corpus/capabilities de G03; domínio ordinal medido na fonte Godot 4 |
| S21 | host sem GUI + SDL 2.28.5 GameController/Joystick | pins `20328dd3b31f4e364933a69baaadde8c04b6ab7fd1e14760b3dc8a2590c7d2c0`; log C6 `860db62d8e0401c00e6516502c89081335140341329da3533f0add41527abfae` | `REAL_API_HOST` | C3 antes de `SDL_PrivateJoystickAdded`; DB nativo responde no passo 4 quando aplicável | GUID/capability fixtures por instância; mapping heterogêneo escolhe GUID exato; pad recusado não entra em `SDL_NumJoysticks` |
| S22 | host sem GUI + SDL 2.32.10 GameController/Joystick | mesmos pins e log de S21 | `REAL_API_HOST` | igual a S21 | mesmo domínio executado de S21; instâncias independentes, inclusive GUID duplicado |
| S30 | host sem GUI + SDL 3.2.30 Gamepad/Joystick | mesmos pins e log de S21 | `REAL_API_HOST` | C3 antes de `SDL_PrivateJoystickAdded`; banco SDL3 ainda não responde no passo 4, portanto o port precisa declarar fonte | GUID/capability fixtures por instância; divergência para o mesmo GUID fecha antes do anúncio |
| A01 | host hermético + Android strict input API v1 / GPTK bridge | header `e9b756b8c0ec220049ab578e8fee5acb29bd1d05cb4e6da2019acdd1806b98bd`; log C7 `11d1d91999cfcd0f48590491e6fe459c0758d352d4bf1226d655ae938e0f6983` | `FIXTURE` | snapshot C3/C4 congelado; uma rota declarada KeyEvent, MotionEvent, JNI, native-gamepad ou touch | tokens sanitizados de instance/device/generation; quatro pads, inclusive mesmo GUID, sem identidade publicada |
| U01 | host hermético + Unity Legacy Input profile v1 | header `fa56c9e6b59a3ec0edaec7011f31ae09f463671cf04fb841adf8db9b4f5dc84c`; ledger `430eba98debb34a2f95f405075292ee8095902b6475f44ccfe9335cfb1de924c` | `FIXTURE` | recebe somente o evento já congelado por A01; zero segunda leitura SDL/evdev/GPTK | pad/generation tokens de fixture; identidade real de plugin/port não é alegada |
| U02 | host hermético + Unity New Input System profile v1 | mesmos artefatos de U01 | `FIXTURE` | igual a U01, com assinaturas completas próprias do perfil | igual a U01 |
| U03 | host hermético + Rewired profile v1 | mesmos artefatos de U01 | `FIXTURE` | igual a U01, com producer/low-level/action receipts separados | igual a U01 |
| U04 | host hermético + InControl profile v1 | mesmos artefatos de U01 | `FIXTURE` | igual a U01, sem reutilizar ABI/assinatura de outro perfil | igual a U01 |
| U05 | host hermético + raw Android `InputDevice` profile v1 | mesmos artefatos de U01 | `FIXTURE` | igual a U01; raw é perfil declarado, não fallback universal | igual a U01 |

## Cobertura por linha

`C18=18/18` abaixo significa que os 18 grupos foram realmente exercitados na
classe daquela linha. `MAPPING=18` em P01 significa somente que a linha oficial
declara os 18; não afirma entrega no jogo.

| ID | C18, press/release e analógicos | Mapping load/readback | GPTK `action` / `null` / `native` | Consumer realmente provado |
|---|---|---|---|---|
| P01 | `MAPPING=18`; press/release, centro/mín/máx/deadzone e consumer `UNPROVEN` | load físico por `get_controls`; readback do runtime `UNPROVEN` | `N/A` | somente cadeia PortMaster até `SDL_GAMECONTROLLERCONFIG_FILE` |
| F01 | bindings/capabilities completos por cenário; não é sink de gameplay | seis passos, setter/readback hostil e recusa antes do anúncio | `N/A` | autoridade/admissão nxinput |
| F02 | `C18=18/18`; press/release; sticks null sem dupla leitura; gatilhos contínuos e bordas 0,60/0,40 | parser/dispatcher v2 e owner/default; readback SDL `N/A` | três estados provados, inclusive null sem latch | core GPTK v2 |
| F03 | `C18=18/18`; primeiras bordas, centro/mín/máx, deadzone e limiares | replay ON/OFF byte-idêntico; não altera mapping | observado, nunca autoriza ação | receipt/doctor read-only |
| G03 | `C18=18/18`; press/release; hats; sticks min/centro/max/deadzone; L2/R2 analógicos | setter + readback + announce, no mesmo PID/TID da engine | três estados nas rotas InputMap, `_input` e polling | Godot 3 real, headless |
| G04 | igual a G03 | igual a G03 | igual a G03 | Godot 4 real, headless |
| S21 | `C18=18/18`; eventos e polling separados; sticks extremos/centro; hats; triggers 0..255 | antes do anúncio, readback semântico e fail-closed | três estados em GameController; `null` por controle em SDL_Joystick é `UNPROVEN` | SDL 2.28.5 real |
| S22 | igual a S21 | igual a S21 | igual a S21 | SDL 2.32.10 real |
| S30 | igual a S21 | igual a S21, exceto autoridade 4 indisponível por ordem nativa SDL3 | três estados em Gamepad; `null` por controle em SDL_Joystick/raw não declarado é `UNPROVEN` | SDL 3.2.30 real |
| A01 | `C18=18/18` por perfil host; down/up; hats; sticks/deadzone; gatilhos contínuos | autoridade congelada uma vez; readback pertence a C3, não a esta camada | action em uma rota, null em zero, native em uma rota | callbacks strict Android host, não VM/aparelho |
| U01 | `C18=18/18`; parte dos 90/90 controles dos cinco perfis; press/release e analógicos | evento congelado A01; mapping/readback `N/A` nesta camada | null zero chamadas; native uma rota | Legacy fixture + retornos producer/low-level/action |
| U02 | igual a U01 | igual a U01 | igual a U01 | New Input System fixture |
| U03 | igual a U01 | igual a U01 | igual a U01 | Rewired fixture |
| U04 | igual a U01 | igual a U01 | igual a U01 | InControl fixture |
| U05 | igual a U01 | igual a U01 | igual a U01 | raw Android fixture |

## Lifecycle, multipad e chord

| ID | SELECT+START e negativos | Hotplug/reopen/dois pads | Limite explícito |
|---|---|---|---|
| P01 | `UNPROVEN` | `UNPROVEN` | não promove prova física para o HEAD V4 |
| F01 | `N/A` | remoção invalida só a instância; reconnect redecide; dois pads de mesmo GUID independentes | fixture de autoridade, sem gameplay |
| F02 | mesmo pad dispara; L2+R2, GUIDE+START e cross-pad recusados | dois pads/contextos independentes; release no contexto antigo | host core |
| F03 | receipt do mesmo pad e três negativos | receipt por pad e geração; replug rearma só o alvo | observed-only |
| G03 | dispara uma vez; START só, GUIDE+START, L1+R1, release no meio e cross-pad recusados; SIGTERM usa o mesmo lifecycle | dois pads/GUID duplicado; hotplug; reconnect encolhido recusado | física `PENDING` |
| G04 | igual a G03 | igual a G03 | física `PENDING` |
| S21 | dispara uma vez; L2+R2 e cross-pad recusados; zero teclado sintético | dois pads/GUID duplicado; remove só alvo; reopen redecide | supressão SDL_Joystick por controle `UNPROVEN` |
| S22 | igual a S21 | igual a S21 | igual a S21 |
| S30 | igual a S21 | igual a S21; GUID duplicado divergente fecha | port SDL3 deve declarar fonte |
| A01 | mesmo pad apenas; L2+R2, GUIDE+START e cross-pad recusados, fora das rotas Android | quatro pads, mesmo GUID, geração nova, focus/pause/cancel sem ressuscitar estado | VM, guest e device não executados |
| U01 | mesmo pad apenas; zero ação Unity/teclado | dois pads, mesmo GUID, unplug/reconnect e lifecycle completo | perfil real `PENDING` |
| U02 | igual a U01 | igual a U01 | perfil real `UNPROVEN` |
| U03 | igual a U01 | igual a U01 | perfil real `PENDING` |
| U04 | igual a U01 | igual a U01 | perfil real `PENDING` |
| U05 | igual a U01 | igual a U01 | perfil real `UNPROVEN` |

## Inventário que não vira linha de sucesso

- Novos bytes V4 em qualquer CFW/aparelho: `PENDING_PHYSICAL`.
- Blossom Tales, Off The Road e Geometry Dash SubZero preservam apenas a prova
  histórica de seus artefatos; adoção C7: `PENDING`, não exercitada.
- Os 14 candidatos Unity do corte dos últimos 30 objetos foram classificados,
  mas nenhum perfil real foi executado: Legacy/Rewired/InControl `PENDING`, New
  Input System/raw Android `UNPROVEN`, todos `reachable=0` para os bytes novos.
- A/B `null` em SDL_Joystick e raw evdev não declarado: `UNPROVEN`.
- A matriz não concede suporte universal a ArkOS, dArkOS, Knulli, muOS,
  ROCKNIX, NextOS ou qualquer outro CFW.
