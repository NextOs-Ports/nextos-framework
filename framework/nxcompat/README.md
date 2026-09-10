# nxcompat

`nxcompat` é a biblioteca C99 estática de compatibilidade para loaders novos. Ela
descobre capacidades do host, produz um plano auditável de ajustes process-local,
negocia backends pela abertura real e entrega mensagens curtas para o logo e o log.

Ela não é um launcher, não resolve falhas anteriores a `main`, não implementa JNI e
não escolhe um device por nome. O loader da engine continua dono da ordem nativa, da
janela/contexto, do present, do áudio em runtime e do teardown.

Versão atual: `0.5.3`, API corrente `NXCOMPAT_API_VERSION == 2`. A versão 0.3.0
acrescentou, por opt-in, o parser estrito `NEXTOS_SETTINGS/1` e a resolução
imutável de idioma da API v2; ela não altera locale, seleciona firmware por nome
nem migra ports existentes. A versão 0.4.0 é aditiva sobre a mesma API 2:
acrescenta o resolver opcional de símbolos SDL posteriores ao piso universal
2.0.4 (`nxcompat_sdl_optional.h`), desligado até um adapter chamá-lo
explicitamente, e preserva byte a byte todo o comportamento 0.3.0. Os entry
points legados de API 1 permanecem disponíveis. As versões 0.5.x acrescentam
o contrato tipado de vídeo; a 0.5.3 soma o algoritmo total aditivo
`auto-stretch`, sem alterar os algoritmos anteriores. Requirements e receipts
pertencem ao contrato tipado da API 2.

## Modelo de uso

```text
probe -> plano inspecionável -> apply -> status inicial
                                      |
                                      v
                       negociação SDL/engine real
                                      |
                                      v
                 receipts fortes de gráfico/áudio/input
                                      |
                                      v
                 registry finito -> gates -> relatório sanitizado
```

Todos os structs públicos carregam `api_version` e `struct_size`. Inicialize-os com
zero, preencha esses dois campos e compile a biblioteca diretamente no ELF da ABI do
loader. Não distribua `libnxcompat.so` nem dependa de `LD_PRELOAD`: o objetivo é que
o contrato acompanhe o binário e não acrescente outro ponto de falha em runtime.

## Descoberta de capacidades

`nxcompat_probe()` observa, sem inicializar SDL/EGL/GL/áudio/input:

- arquitetura compilada do processo e arquitetura do kernel;
- modelo apenas para diagnóstico, ID/versão do sistema e glibc do processo;
- classe de memória e swap;
- filesystem do diretório do jogo, incluindo classes FUSE-like e rede;
- DRM, conector conectado, modo publicado pelo kernel e framebuffer;
- runtime de sessão gravável, socket Wayland, Pulse e PipeWire e presença ALSA;
- diretórios de bibliotecas ARMHF/AArch64/i386; layouts multiarch são aceitos
  diretamente, enquanto `/usr/lib`, `/lib` e variantes `lib64` genéricas só
  promovem a capacidade depois que o cabeçalho de um ELF compartilhado comprova
  a ABI (incluindo ARM hard-float);
- módulos de áudio ARMHF em userland AArch64;
- PortMaster e banco/mapping SDL de controle;
- hints SDL de vídeo/áudio já herdados.

`probe_root` permite redirecionar somente as leituras de filesystem para uma árvore
sintética absoluta e sem componentes `.`/`..`. Ele não é um chroot: kernel, processo,
limites e ambiente continuam sendo os do host vivo, portanto esse modo não é um
relatório offline autoritativo. Um relatório offline hermético deve usar o provider
custom exclusivo da API v2. Os caminhos retornados continuam no namespace do aparelho
para que o relatório não vaze o diretório da fixture.

A presença de `/dev/fb0`, DRM ou um socket não prova que a engine conseguiu abrir
um frame. Por isso o probe nunca é usado como declaração final do backend; essa prova
ocorre na negociação e, para gráficos, depois no contexto/drawable real.

`nxbootstrap` exporta `NXCOMPAT_PORTMASTER_DIR` depois de carregar o `control.txt`;
essa autoridade tem prioridade no probe. Sem ela, o probe consulta o diretório irmão
do jogo e o mesmo catálogo curto de raízes explícitas do bootstrap, sem busca recursiva.
Um `SDL_GAMECONTROLLERCONFIG_FILE` herdado é uma escolha explícita de autoridade:
um path absoluto, ou um path relativo seguro resolvido sob `game_dir`, só é
observado quando aponta para arquivo regular não vazio. Uma escolha inválida não
cai silenciosamente em outro banco. Bancos descobertos pelo probe também precisam
ser regulares e não vazios.

## Plano de ambiente

O contrato recomendado usa `nxcompat_plan_environment_v2()` para criar a lista
tipada e `nxcompat_apply_environment_v2()` para aplicá-la. Valores herdados ou
alterados entre plan/apply são preservados; se uma escrita posterior falhar, as
anteriores são revertidas e verificadas. Estados, final reason, contagens de apply/
rollback e `env_restored` tornam o resultado auditável sem interpretar texto livre.
As funções sem sufixo permanecem apenas para compatibilidade API 1.

`NXCOMPAT_POLICY_AUTOMATIC_SAFE` inclui:

| Política | Condição | Efeito possível |
| --- | --- | --- |
| session runtime | diretório existente, gravável e com socket de sessão | `XDG_RUNTIME_DIR` |
| Pulse | socket real encontrado e nenhum servidor herdado | `PULSE_SERVER=unix:...` |
| áudio ARMHF | processo ARMv7 em kernel AArch64 e diretórios correspondentes | paths PipeWire/SPA/ALSA |
| controller DB | nenhum mapping/config existente e banco legível encontrado | arquivo SDL controller DB |
| semântica Xbox | firmware ainda não definiu labels | posições Xbox para `nxinput` |

O ajuste de `MALLOC_ARENA_MAX` é deliberadamente excluído da política automática.
Ele só entra quando o port habilita `NXCOMPAT_POLICY_LOW_MEMORY_ARENAS` e o probe
mediu a classe curta. Isso evita transformar uma otimização de um jogo em regra para
todos.

Nenhuma política define `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER`, resolução, card,
versão GLES ou override Mesa.

## Negociação de backend

O contrato recomendado é `nxcompat_negotiate_backend_v2()`. Ele executa uma
máquina de estados pequena e finita:

1. salva os hints declarados pelo adapter e tenta usá-los sem alteração;
2. executa `cleanup` exatamente uma vez depois da tentativa, inclusive no sucesso;
3. somente uma falha declarada `RETRYABLE` permite remover os hints e fazer uma única
   tentativa de autodetecção normal do runtime;
4. se houve retry, executa também o segundo `cleanup`; em todo caminho terminal tenta
   restaurar o ambiente herdado byte a byte e falha fechado se a verificação não passar.

O report da tentativa só pode declarar sucesso depois que a saída necessária está
realmente utilizável e deve registrar o backend que abriu, nunca o texto pedido por
variável. `accept_name` rejeita saídas falsas. API 2 não possui enumeração, callback
`discover`, lista de nomes, nem terceira tentativa.

`nxcompat_negotiate_backend()` e seu callback `discover` permanecem somente para ABI
legada de API 1. Eles não são o modelo de integração de ports novos e nunca produzem
receipt forte de API 2.

### Adapter SDL2

O target opcional `nxcompat-sdl2` é um probe transitório. Ele fecha antes de retornar
somente as referências de subsistemas que ele próprio adquiriu; nunca fecha SDL
pertencente à engine. Um retorno de sucesso garante o ambiente herdado restaurado; o
resultado informa o backend que realmente abriu sem fixá-lo por nome para a abertura
posterior da engine.

- vídeo não deve anteceder `nxgl` no fluxo padrão. `nxgl` é o único dono da abertura
  real janela/EGL/GLES/drawable e da recuperação de hint de vídeo;

- áudio usa `nxcompat_sdl2_negotiate_audio_v2()`: abre o output default, exige device
  ID não zero e spec válido, fecha o device e verifica cleanup antes de produzir o
  receipt `OPENED_THEN_CLOSED`. Em falha retryable existe somente uma autodetecção
  normal; não há loop de drivers;
- `NXCOMPAT_SDL2_ALLOW_NONDISPLAY` existe apenas para ferramenta headless;
- não existe modo `SKIP_AUDIO_DEVICE_PROBE`: inicializar subsistema ou obter um nome
  de driver nunca substitui a abertura real.

O adapter recusa retry se o subsistema primário já pertencia ao caller e a validação
falhou, pois não pode reiniciar estado externo com segurança. `additional_sdl_init_flags`
aceita apenas subsistemas adicionais; `SDL_INIT_VIDEO` e `SDL_INIT_AUDIO` são rejeitados.

## Resolver opcional de símbolos SDL (V4-PRE-02A)

O piso SDL universal é 2.0.4: um ELF público nunca pode importar diretamente um
símbolo introduzido depois dele — `SDL_JoystickGetVendor` e
`SDL_JoystickGetProduct` (SDL 2.0.6) derrubariam o boot num firmware de piso.
`nxcompat_sdl_optional.h` fornece o caminho canônico para usar essas APIs
quando existirem, sem elevar o piso:

```c
nxcompat_sdl_optional_options options = {0};
nxcompat_sdl_optional_joystick_api api;
nxcompat_sdl_joystick_ids ids;

options.api_version = NXCOMPAT_API_VERSION_V2;
options.struct_size = sizeof(options);
/* Símbolo baseline (<= 2.0.4) que o ELF já importa diretamente. */
options.provider_anchor = (const void *)SDL_GetVersion;

nxcompat_sdl_optional_resolve_joystick_api(&options, &api);
nxcompat_sdl_optional_joystick_ids(&api, joystick, &ids);
/* ids.vendor_known/product_known dizem se o dado existe; ausência = 0. */
```

Contrato:

- resolve somente no namespace **já carregado** pelo processo: o candidato vem
  de `dlsym(RTLD_DEFAULT, ...)` e só é aceito quando `dladdr`/`dli_fbase`
  prova que ele vive no exato módulo que fornece o `provider_anchor`. O módulo
  de produção não chama `dlopen`: nada é aberto, buscado ou carregado por
  nome/path, sem SDL privada nem escolha de outra implementação — identidade
  ausente, diferente ou ambígua (uma biblioteca estranha exportando ou
  sombreando o mesmo nome, ou uma âncora de outro módulo) vira ausência
  segura;
- ausência do símbolo não é erro: numa SDL 2.0.4 o resolve retorna `NXCOMPAT_OK`
  com ponteiros NULL e as queries reportam metadata desconhecida/zero, sem
  jamais inventar VID/PID e sem impedir o boot;
- nenhuma seleção por jogo, CFW, device, nome, VID ou PID; sem estado global,
  sem cache, thread-safe e idempotente;
- `nxcompat_sdl_optional_symbol()` resolve qualquer outro símbolo `SDL_*`
  posterior ao piso sob o mesmo contrato (fixtures usam
  `SDL_JoystickAttachVirtualEx` como prova);
- um `resolver` custom permite fixtures herméticas modelarem SDL 2.0.4,
  2.0.6+ e providers parciais sem carregar SDL;
- nada no nxcompat chama esta API sozinho: ela fica desligada até o adapter
  optar por ela explicitamente, e nenhum port existente é migrado.

O gate `nxcompat-sdl-optional-audit` cruza a prova estática: `readelf` confirma
que as fixtures não importam os dois símbolos diretamente e o `nxabi audit
--sdl-floor 2.0.4` (sobre a fixture cross-compilada AArch64) confirma zero
achados `sdl-floor`, enquanto um controle negativo com o import direto precisa
ser sinalizado — se o auditor cegar, o gate falha. O mesmo gate recusa
`dlopen`/`dlmopen`/`dlclose`/`SDL_LoadObject` no fonte do módulo de produção
(comentários e strings removidos antes do scan) e qualquer import ELF de
`dlopen`/`dlmopen`/`dlclose` na fixture AArch64 sem sanitizers.

## Baseline gráfico

Mali-450 + GLES2 é o piso do contrato. Depois de criar o contexto no fluxo nativo,
o loader entrega para `nxcompat_capture_graphics()` os valores reais de driver,
`GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`, GLSL, extensões, tamanho e atributos do
drawable.

A captura classifica famílias e capabilities como GLES2/GLES3, ETC1/ETC2, ASTC e
NPOT completo. Ela não altera estado GL, formato, sampler, wrap, alpha, contexto ou
present. A engine consulta os bits antes de habilitar caminhos opcionais; ela nunca
deduz GLES3 porque o renderer parece moderno e nunca anuncia GLES 3.2 ao Mali-450.

`nxcompat_capture_graphics()` e qualquer conversão a partir de relatório livre são
somente diagnóstico legado. Eles nunca publicam no registry e, portanto, nunca
satisfazem `graphics.*` em `required_capabilities`.

## Registry, requirements e receipts API 2

[`capabilities-v1.json`](capabilities-v1.json) é o source of truth único. A tabela C
imutável expõe os mesmos 31 IDs, na mesma ordem numérica, com `phase`, `source`,
`minimum_evidence` e `role`. Não existe API para criar capability em runtime e nenhum
texto de log vira evidência.

`NXCOMPAT_REQUIRED_CAPABILITIES` contém nomes exatos separados por newline, gerados
pelo `nxbootstrap` a partir do manifesto. `nxcompat_requirements_parse_runtime()`
rejeita nomes desconhecidos, duplicados, linhas vazias e caracteres fora do formato.
O texto público inteiro tem limite fixo `NXCOMPAT_REQUIREMENTS_TEXT_MAX`; lookup e
parser nunca fazem busca ilimitada por terminador. As variantes
`nxcompat_requirements_parse_ex()` e `parse_runtime_ex()` devolvem reason code finito,
incluindo `REQUIREMENT_UNKNOWN` e `REQUIREMENT_DUPLICATE`, sem alterar o output em
erro.
`nxcompat_requirements_evaluate()` mantém um requirement como `pending` antes da fase
dele, marca `satisfied` somente no nível mínimo registrado e passa a `missing` quando
a fase chegou sem prova ou quando a prova ficou `lost`.

Publicar um receipt e fechar o gate global são operações diferentes. Um adapter não
deve rejeitar um receipt válido de input ou áudio só porque gráficos ainda não
publicaram (nem desfazer um contexto gráfico porque outro domínio está pendente).
Cada publisher decide apenas se sua própria prova pôde ser registrada. A decisão
cross-system usa a fase `READY`, uma única vez depois que o fluxo nativo teve a
oportunidade de criar contexto/drawable e iniciar os demais subsistemas.

O registry é caller-owned e sem estado global mutável. As publicações tipadas são:

| Receipt | Prova mínima real | Capabilities principais |
| --- | --- | --- |
| graphics | somente bridge NXGL/engine forte: janela/contexto atuais, `glGetString`, drawable positivo e EGL display/context/config consultados | `graphics.window`, GLES2/3, EGL/config/drawable e formatos opcionais |
| audio | output default realmente aberto e fechado, device ID não zero, spec obtido e cleanup verificado; engine adapter pode provar lifetime ainda ativo | `audio.output-open` |
| input | subsistema/scan, controlador realmente aberto quando conectado e, para hotplug, event watch **mais** rescan ativos | mapping, controller API, connected e hotplug |

Para input, material de banco ou `SDL_GAMECONTROLLERCONFIG` observado prova apenas
que existe uma fonte candidata: `input.controller-mapping` fica em
`OBSERVED/PROBE` (ou `OBSERVED/NXINPUT` para mapping material reportado pela ponte).
Quando uma ponte runtime/engine declara mapping resolvido para um
`SDL_GameController` conectado e aberto, a mesma capability sobe para `OPENED`
com a source tipada do receipt; a ponte `nxinput` produz `OPENED/NXINPUT`. O
reason é `input-controller-mapping-resolved`. Ao desconectar o último controle, ela retorna
à observação do probe se o banco ainda existe; sem fonte restante, torna-se `LOST`.

Cada domínio usa geração monotônica própria. Geração de receipt/topologia e contador
interno do registry são namespaces independentes: só se compara uma nova publicação
com a anterior do mesmo domínio. O validador monta uma cópia staged e só publica
depois de validar todo o receipt: geração stale, dependência de proof flags quebrada,
backend falso (`dummy`, `disk`, `offscreen`), dimensões/config inválidas ou string sem
terminador deixam evidências e o último receipt byte-idênticos. Uma publicação nova
que comprova perda converte a evidência anterior em `lost`; capacidades independentes
continuam intactas.

Cada publisher também possui variante `_ex(..., nxcompat_reason_code *reason)`.
`CAPABILITY_STALE`, ABI incompatível, backend falso e contrato de provider quebrado
ficam distinguíveis no log; o reason é output separado e um erro continua sem mutar
o registry. O contador interno recusa overflow antes do commit.

## Status e privacidade

`nxcompat_emit_startup_status()` permanece o formatter curto de compatibilidade API 1
(ou pode emitir só a linha de device com plan nulo). Integrações API 2 consomem os
estados/reasons finitos de plan/backend e o runtime report sanitizado; a UI decide como
desenhá-los. A captura diagnóstica ainda pode formatar renderer/GLES/drawable, mas não
vira evidence.

O callback é independente de UI. Ele pode escrever no log e guardar as mensagens
até o loader ter um logo desenhável. Falha de vídeo normalmente é fatal; falha de
áudio pode virar modo mudo com aviso, se a engine suportar isso corretamente.

`nxcompat_format_json()` permanece o formato legado. Para receipts/gates, use
`nxcompat_registry_runtime_report()`: ele agrega labels já sanitizados e remove o
texto bruto de extensões depois que as capabilities finitas foram derivadas.
`nxcompat_format_runtime_json()` serializa esse estado em IDs/enums finitos, números
e labels sanitizados. Paths, sockets, GUIDs, instance IDs, nomes de controle,
valores/variáveis de ambiente e motivos livres do plano nunca entram. Padrões de path
pessoal, IPv4, e-mail e credencial são substituídos por `[redacted]`.

## Integração C/CMake

```cmake
set(NXCOMPAT_BUILD_TESTS OFF CACHE BOOL "")
set(NXCOMPAT_BUILD_TOOLS OFF CACHE BOOL "")
set(NXCOMPAT_WITH_SDL2 ON CACHE BOOL "")
add_subdirectory(path/to/framework/nxcompat)

target_link_libraries(game-loader PRIVATE nxcompat nxcompat-sdl2)
```

Esqueleto no loader:

```c
nxcompat_probe_options probe;
nxcompat_plan_options policy;
nxcompat_host host;
nxcompat_plan_v2 plan;

memset(&probe, 0, sizeof(probe));
probe.api_version = NXCOMPAT_API_VERSION;
probe.struct_size = sizeof(probe);
probe.port_id = getenv("NXCOMPAT_PORT_ID");
probe.game_dir = getenv("NXCOMPAT_GAME_DIR");

if (nxcompat_probe(&probe, &host) != 0)
    return fatal_startup("capability probe failed");

memset(&policy, 0, sizeof(policy));
policy.api_version = NXCOMPAT_API_VERSION;
policy.struct_size = sizeof(policy);
policy.runtime_arch = NXCOMPAT_ARCH_UNKNOWN; /* arquitetura deste ELF */
policy.policy_flags = NXCOMPAT_POLICY_AUTOMATIC_SAFE;

if (nxcompat_plan_environment_v2(&host, &policy, &plan) != NXCOMPAT_OK ||
    nxcompat_apply_environment_v2(&plan) != NXCOMPAT_OK)
    queue_startup_warning("compatibility environment incomplete");
```

Depois disso, `nxcompat_sdl2_negotiate_audio_v2()` pode produzir a prova transitória
de áudio antes de threads concorrentes usarem SDL. Para vídeo forte, use exclusivamente
o contexto real aberto por `nxgl`/adapter da engine e publique
`nxgl_nxcompat_publish_context()` depois de janela, GLES, EGLConfig e drawable existirem.
O probe SDL de vídeo nunca satisfaz `graphics.*`. Não pule `init_array`, `JNI_OnLoad`
ou callbacks originais para chegar mais rápido à tela.

ARMv7 e AArch64 devem compilar a mesma API em builds separados. Num release público,
audite todos os ELFs gerados e rejeite qualquer requisito acima de `GLIBC_2.30`,
preferindo `GLIBC_2.17` quando viável.

## Ferramenta e testes

Build standalone do core:

```sh
cmake -S framework/nxcompat -B build/nxcompat \
  -DNXCOMPAT_BUILD_TESTS=ON \
  -DNXCOMPAT_BUILD_TOOLS=ON
cmake --build build/nxcompat
ctest --test-dir build/nxcompat --output-on-failure
```

Com SDL2 disponível, acrescente `-DNXCOMPAT_WITH_SDL2=ON`. O diagnóstico pode ser
executado sem aplicar mudanças:

```sh
build/nxcompat/nxcompat-probe --game-dir /caminho/do/port --port-id exemplo
```

Use `--apply` somente num processo descartável ou no loader que realmente consumirá
o plano. `--json` gera o relatório estruturado.

Os testes atuais cobrem parser de modo, captura GLES2/Mali-450 e GLES3/Panfrost,
ordem e restauração da negociação API 2, rejeição de saída falsa, limite de uma única
autodetecção sem enumeração, ordem exata do registry, parsing/avaliação por fase,
atomicidade de receipts,
loss/stale, independência entre domínios e redaction adversarial do runtime JSON,
probe em filesystem sintético, sessão, áudio ARMHF, PortMaster, plano/apply e JSON
sem vazar a raiz da fixture. O resolver opcional de SDL tem gate hermético
(providers modelados 2.0.4/2.0.6+/parcial, misuse, threads), providers reais
carregados como shared objects de fixture (incluindo rejeição de biblioteca
estranha com o mesmo nome), prova por valor num joystick virtual da SDL do
sistema e a auditoria `readelf`/`nxabi` com controle negativo
(`nxcompat-sdl-optional-audit`, que exige `aarch64-linux-gnu-gcc` no host).

Eles não provam compatibilidade física. Antes de publicar, valide o mesmo loader no
Mali-450/GLES2 e nas demais classes declaradas pelo release, incluindo vídeo real,
áudio audível, controles, saves e saída limpa.
