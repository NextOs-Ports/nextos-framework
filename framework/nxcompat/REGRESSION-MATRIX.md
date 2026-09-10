# nxcompat 0.5.0 — matriz de regressão

| Gate canônico | Contrato comprovado | Classe de evidência |
|---|---|---|
| `nxcompat-host` | Probe, plano/apply/rollback, negociação com uma única retry, registry e receipts tipados, redaction, bridges NXGL/NXInput, instalação e link; GCC/Clang com ASan/UBSan/LSan e analisadores | Host hermético; GPU, áudio, controle, guest, rede e device reais não executados |
| `nxcompat-system-font` | Prioridade explícita/env/PortMaster/raiz declarada, assinatura mínima de fonte e rejeição de arquivo vazio ou falso | Filesystem temporário host; oito cenários |
| `nxcompat-m12-audit` | Ledger M12 completo, pins SHA-256, API/capabilities/reasons finitos, política sem nomes de device e escopo físico declarado como ausente | Auditoria estática/process-free |
| `nxcompat-settings-language` | Parser estrito `NEXTOS_SETTINGS/1` **e `/2`** (namespace `video.*` tipado, erro com linha), defaults fail-closed, idioma V2 puro, geometria de vídeo `nx-video/1` (preserve/stretch/crop/integer/engine, `auto` total por algoritmo declarado, inversa touch, receipt) e corpus adversarial determinístico (14 arquivos) | Host hermético; GCC e Clang; sem mutação de ambiente ou filesystem no parser |
| `nxcompat-settings-language` (parte `test_video_owner`) | **Autoridade ÚNICA de aspect** (0.5.1): eleição atômica `nextos`/`engine`/`synchronized` com sobreposição fora da eleição falhando antes do init; precedência `port-env` > settings `/2` > native config EDITADO > `auto` declarado > default do pacote; `auto` sem algoritmo total rejeitado; receipt `NX-VIDEO/1` com a fonte vencedora; readback `NX-VIDEO-READBACK/1` em que readback vazio é mismatch; matriz 9.4 (640×480, 720×720, 1280×720, 1920×1080, portrait 720×1280) resolvida através do framework | Host hermético; GCC e Clang; funções puras, sem I/O, sem nome de device nem vocabulário de engine |
| `nxcompat-settings-language` (parte `test_settings` 0.5.1) | **`video.invalid_policy` executada**: `fail_closed` recusa; `last_known_good` exige estado aplicado sob o MESMO contrato e sem ele cai para fail-closed nomeando o motivo; `package_default` aplica a semente sem copiá-la sobre o dono; bytes do dono nunca reescritos; recuperação recusada num parse bem-sucedido | Host hermético; função pura, sem I/O |
| `nxcompat-installed-consumer` | `configure -> build -> install -> compile -> link -> run` de consumidor que usa somente headers e `libnxcompat.a` instalados | Host hermético; prova do artefato instalado |
| `nxcompat-sdl-optional` | Resolver opcional de SDL com providers modelados: 2.0.4 sem Vendor/Product boota com metadata zero, 2.0.6+ entrega os valores exatos, um só símbolo, resolver/provider inválido falha fechado, misuse de structs, 1000 repetições e 8 threads | Host hermético; nenhuma SDL linkada ou carregada |
| `nxcompat-sdl-optional-floor204` / `-sdl206` / `-partial` | Resolver default sobre providers realmente carregados (shared objects de fixture); o cenário floor204 carrega junto uma biblioteca estranha com Vendor/Product envenenados que jamais pode ser selecionada | Host hermético; provider real via `dlsym(RTLD_DEFAULT)` + identidade `dladdr`/`dli_fbase`, zero `dlopen` no módulo de produção |
| `nxcompat-sdl-optional-provider` | SDL2 do sistema com imports baseline apenas; Vendor/Product alcançados só pelo resolver, identidade de módulo por `dladdr` e prova por valor num joystick virtual com VID/PID conhecidos | Host com SDL2 real; subsistema joystick sem device físico |
| `nxcompat-sdl-optional-audit` | `readelf`: zero import direto de `SDL_JoystickGetVendor`/`SDL_JoystickGetProduct` em todas as fixtures; gate estático: zero `dlopen`/`dlmopen`/`dlclose`/`SDL_LoadObject` no fonte do módulo de produção e zero import ELF de `dlopen` na fixture AArch64 sem sanitizers; `nxabi audit --sdl-floor 2.0.4` na fixture cross AArch64: zero achados `sdl-floor`; controle negativo com import direto precisa ser sinalizado | Auditoria estática fail-closed; exige `aarch64-linux-gnu-gcc` no host |

Os cinco gates históricos são registrados em `framework/tests/test-matrix-v1.json`
e executados por `framework/tests/run-safe-gates.sh`. Na RC7 funcional
`cad8a5b28a474a2feb63d1657f189e262bfa019b`, eles integraram a bateria canônica
selada com resultado `ALL PASS 103/103` e um checkpoint verde. Os gates
`nxcompat-sdl-optional*` da 0.4.0 rodam dentro do `ctest` do componente (e,
portanto, dentro de `nxcompat-host`); o registro em linhas próprias da matriz
compartilhada pertence à composição canônica da V4 (V4-PRE-01), não a esta
frente.

## Fronteiras que continuam físicas e por port

- abertura e desenho reais no GPU/display;
- áudio audível, controle conectado/hotplug e ausência de duplo input;
- saves, saída limpa e ciclo completo do jogo;
- compatibilidade do mesmo ZIP final nas classes de firmware declaradas.

Nenhum resultado host acima autoriza claim universal de aparelho ou CFW.
