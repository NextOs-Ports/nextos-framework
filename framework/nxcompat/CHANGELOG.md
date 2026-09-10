# nxcompat changelog

## 0.5.3 — 2026-09-04 (V5 vídeo: `auto-stretch` aditivo)

- Adiciona o algoritmo total `nxcompat_video_auto_stretch()`: `auto` ocupa o
  drawable inteiro em qualquer proporção, preservando o comportamento já
  aprovado de ports que historicamente esticam a imagem. Letterbox continua
  disponível somente por escolha explícita do dono (`video.aspect=preserve`).
- Os algoritmos existentes `ratio-threshold` e `epsilon`, suas APIs e seus
  resultados permanecem byte a byte inalterados. Não há seleção por aparelho,
  CFW, resolução nominal ou nome de jogo.
- O gate direcionado prova que o novo algoritmo nunca devolve `auto` nem
  converte silenciosamente a preferência explícita `preserve`.

## 0.5.2 — 2026-09-03 (V5 revisão 2: readback recusa decisão recusada)

- `nxcompat_video_readback_check` devolve `mismatch=decision` quando a eleição do dono falhou (`failed=1`):
  uma decisão zerada nunca "bate" com um readback zerado. Teste com mutante em test_video_owner.c.


## 0.5.1 — 2026-09-03 (V5 FV3 / 7A.2: `nxcompat_video` é a autoridade ÚNICA de aspect)

- Antes da 0.5.1 cada port re-derivava "quem decide o aspect": o Tearscape
  carregava um `nx_aspect_policy.h` próprio com a MESMA regra de ratio
  (≤ 1,2 → letterbox) e zero referência a `nxcompat_video`; duas autoridades
  para a mesma decisão e o próximo jogo Godot copiaria de novo (auditoria de
  03/09, item E5). A decisão passa a ser UMA, aqui. No port sobra só a
  tradução da policy efetiva para o vocabulário da engine (Godot:
  `preserve` → `keep`, `stretch` → `ignore`) — cinco linhas, nenhuma regra de
  ratio nem de precedência.
- `nxcompat_video_resolve_owner(in, out)` (`nx-video-owner/1`) faz eleição e
  resolução numa chamada:
  * **Eleição atômica** (7A.2) do namespace `video.*`: `nextos` (o arquivo
    tipado e só os exports de vídeo allowlisted do `port-env.sh` são fontes
    DESTE resolver; o native config é sink projetado + readback), `engine`
    (o native config/UI é o dono da chave; asserção na camada de settings é
    **sobreposição fora da eleição** e falha ANTES do init — nunca
    last-writer-wins) e `synchronized` (só por transação CAS: sem a
    `video_config_generation` lida, ou sem o native config declarado
    presente, recusa). Token de authority ausente = `nextos` documentado.
  * **Precedência**: export do `port-env.sh` > `NEXTOSSETTINGS.txt` (/2) >
    native config **EDITADO pelo dono** > algoritmo `auto` total declarado >
    default do pacote. As duas fontes da camada de settings seguem a ordem
    observável do launcher (`NX-OWNER-RUNTIME/1`: o hook do dono é lido por
    último, logo é a última palavra dele). Native config INTOCADO é default de
    pacote e portanto fica ABAIXO do `auto`; uma edição do dono fica ACIMA —
    exatamente a regra que o Tearscape provou, agora herdada por todo port.
  * `auto` nunca é heurística: port sem algoritmo total declarado REJEITA o
    token (`auto_algorithm_declared=0`) em vez de "preservar" em silêncio.
  * Valor de native config chega já traduzido para o schema; token nativo de
    engine (`keep`/`ignore`) é recusado com motivo — a tradução no port é
    obrigatória e visível.
- `nxcompat_video_owner_receipt()`: o `NX-VIDEO/1` passa a nomear a authority
  ELEITA e a **fonte vencedora da decisão** (`port-env`/`settings`/
  `native-config`/`auto`/`package-default`), mais `via_auto`,
  `native_config_apply` e `cas_generation` — não mais rótulos escolhidos pelo
  chamador.
- **FV3 readback como API**: `nxcompat_video_readback_check(want, got, out, cap)`
  compara o que foi MEDIDO depois do primeiro present contra a decisão eleita e
  emite `NX-VIDEO-READBACK/1 ... match=/mismatch=<campo>`. Um readback vazio
  (`api_version=0`, nada medido) é mismatch: "o arquivo foi lido" nunca
  aprova (7A.2). Divergência de drawable, policy efetiva, content rect ou
  geração é reportada, nunca aprovada.
- Gate novo `tests/test_video_owner.c` (no `run-settings-host.sh`, gcc e
  clang): 33 checks, incluindo a matriz 9.4 de drawables 640×480, 720×720,
  1280×720, 1920×1080 e portrait 720×1280 resolvida ATRAVÉS do framework, e
  mutantes que morrem por observável (troca preserve/stretch move o content
  rect; ignorar a edição do dono muda o rect; readback vazio/efetivo
  errado/rect cheio/drawable pedido/geração velha reprovam).
- **`video.invalid_policy` EXECUTADA, no framework (7A.2).** A 0.5.0 tipou a
  chave e o parser passou a devolver linha/motivo, mas quem DECIDIA o que
  fazer com um arquivo inválido era cada adapter — ou seja, cada jogo.
  `nxcompat_settings_recover()` (`NX-SETTINGS-RECOVERY/1`) decide:
  `fail_closed` recusa a abertura; `last_known_good` só vale se existir estado
  já aplicado SOB O MESMO CONTRATO e, sem ele, cai para fail-closed dizendo que
  caiu; `package_default` aplica a semente imutável SEM copiá-la por cima do
  arquivo do dono. Em nenhum caminho os bytes do dono são reescritos
  (`owner_bytes_rewritten` é 0 e o receipt afirma isso), e chamar a recuperação
  com um parse BEM-SUCEDIDO é recusado — a política de erro nunca vira rota
  para descartar uma configuração válida. Token ausente não é defaultado: o
  port tem de declarar um. Sete checks novos em `tests/test_settings.c`, com
  três mutantes mortos (LKG aceito sob outro contrato, LKG ausente virando
  package_default, política aceitando parse OK).
- Sem mudança de comportamento em `nxcompat_video_content_rect`, nos dois
  algoritmos `auto` versionados, na inversa de touch/cursor ou no parser de
  settings: a 0.5.1 é aditiva.

## 0.5.0 — 2026-09-03 (V5 7A.3 / FV3: NEXTOS_SETTINGS/2 e geometria de vídeo)

- `nxcompat_settings` API 2 (aditiva): aceita `# NEXTOS_SETTINGS/2` além do
  `/1`, com a MESMA gramática (`chave=valor`, `[A-Za-z0-9._-]`, magic primeiro,
  chave desconhecida fatal, duplicata fatal) e o namespace tipado de vídeo
  decidido pelo NextOS em 03/09 (mora DENTRO do settings, sem
  `NEXTOSVIDEO.cfg`): `video.authority=nextos|engine|synchronized`,
  `video.output_size=auto|display|640x480|1280x720|1920x1080|<WxH 1..8192>`,
  `video.aspect=auto|engine|preserve|stretch|crop|integer`,
  `video.filter=engine|nearest|linear`,
  `video.invalid_policy=fail_closed|last_known_good|package_default`. Num
  `/1` toda chave `video.*` continua desconhecida (fatal, com diagnóstico
  "precisa de /2"); num `/2` cada chave de vídeo é opcional e lê `""`
  quando ausente (o adapter aplica o default do pacote; nunca valor oculto).
  Campos novos no struct (`schema`, `video_*`), `nxcompat_settings_parse2()`
  com erro tipado (código + linha + motivo, para o launcher imprimir
  `NEXTOSSETTINGS.txt:<linha>: <motivo>` sem tocar nos bytes do dono) e
  `nxcompat_settings_video_value_ok()`. A entrada API 1 continua idêntica.
- `nxcompat_video` (novo, puro, `nx-video/1`): content rect para
  `preserve` (scale=min, maior retângulo centralizado, rounding documentado:
  16:9 em 720×720 = (0,157,720,405) barras 157/158; em 640×480 = (0,60,640,360)),
  `stretch` (drawable inteiro, `distorted` reportado), `crop` (scale=max,
  clipping central), `integer` (maior fator ≥1 ou `unsupported` explícito),
  `engine` (inherit, nada afirmado); `auto` NÃO é policy: duas funções
  totais versionadas — `nxcompat_video_auto_ratio_threshold` (regra 2.2C do
  Tearscape: Rd ≤ 1,20 → preserve, senão stretch) e
  `nxcompat_video_auto_epsilon` (Blossom: |Rd−Rs| ≤ ε → engine, senão
  preserve). Inversa do MESMO retângulo para touch/cursor
  (`drawable_to_source`/`source_to_drawable`, fora do rect reportado) e
  receipt sanitizado `NX-VIDEO/1 requested= effective= src= drawable=
  content= bars=`. Barras opacas e content rect separados para o frame proof
  (letterbox legítimo nunca vira BLACK).
- Gate `nxcompat-settings-language` ganha `test_video.c` (tests=3) e corpus
  `/2` (ok-schema2-video, bad-schema1-video-key, bad-schema2-video-enum,
  bad-schema2-wxh; `bad-magic` passa a `/3`). Mutantes mortos:
  preserve↔stretch mesmo rect; rect pelo tamanho solicitado em vez do
  drawable; touch fora da transformação do present; letterbox como BLACK.
- Nenhuma interface visual, probe, plano, registry ou receipt anterior mudou.

## 0.4.0 — 2026-08-31 (V4-PRE-02A)

- Adiciona `nxcompat_sdl_optional.h`: resolver canônico e aditivo para
  símbolos SDL posteriores ao piso universal 2.0.4, com a API tipada de
  joystick para `SDL_JoystickGetVendor`/`SDL_JoystickGetProduct` (SDL 2.0.6)
  e o resolve genérico `nxcompat_sdl_optional_symbol()`.
- A resolução acontece exclusivamente no namespace já carregado pelo
  processo: o candidato vem de `dlsym(RTLD_DEFAULT, ...)` e só é aceito
  quando `dladdr`/`dli_fbase` prova que ele vive no exato módulo do
  `provider_anchor` (um símbolo baseline importado diretamente). O módulo de
  produção não contém nenhuma chamada a `dlopen`: nada é aberto, buscado ou
  carregado por nome/path, sem SDL privada e sem seleção de outra
  implementação — identidade ausente, diferente ou ambígua (inclusive uma
  biblioteca estranha exportando ou sombreando o mesmo nome) vira ausência
  segura com metadata zero.
- Ausência do símbolo retorna `NXCOMPAT_OK` com ponteiro NULL e metadata
  desconhecida/zero: numa SDL 2.0.4 o boot prossegue e nenhum VID/PID é
  inventado. Nenhuma seleção por jogo, CFW, device, nome, VID ou PID; sem
  estado global, thread-safe e idempotente.
- A API fica desligada até um adapter chamá-la explicitamente; nada no core,
  no adapter SDL2 ou em qualquer port passa a usá-la nesta versão, e o
  comportamento 0.3.0 é preservado integralmente (API 2 inalterada, apenas
  funções novas).
- Gates novos: hermético com providers modelados (2.0.4 sem Vendor/Product,
  2.0.6+ com ambos, apenas um símbolo, resolver/provider inválido, misuse,
  chamadas repetidas e 8 threads), fixtures reais em shared objects incluindo
  a rejeição de biblioteca estranha envenenada, prova por valor num joystick
  virtual da SDL do sistema (entry points pós-piso resolvidos pela própria
  API), âncora e candidato de módulos diferentes lendo como ausência segura,
  e `nxcompat-sdl-optional-audit`: `readelf` prova zero import direto
  dos dois símbolos, `nxabi audit --sdl-floor 2.0.4` prova zero achados
  `sdl-floor` na fixture cross AArch64, um controle negativo com o import
  direto precisa ser sinalizado pelo auditor, e um gate estático recusa
  `dlopen`/`dlmopen`/`dlclose`/`SDL_LoadObject` no fonte do módulo de
  produção e import ELF de `dlopen` na fixture AArch64 sem sanitizers.
- A biblioteca passa a linkar `${CMAKE_DL_LIBS}` (dladdr/dlsym sobre módulos
  já carregados). Nenhuma interface visual, probe, plano, negociação,
  registry ou receipt mudou.

### Escopo da release

Esta frente não altera nxrelease, nxgenerator, nxinput, o FP2 ou qualquer
port/ZIP; a tabela única de autorização por símbolo entre nxabi e nxrelease
(V4-PRE-02) e a integração na composição V4 são atos posteriores e separados.

## 0.3.0 — 2026-08-26

- Adiciona, por opt-in, o parser estrito `NEXTOS_SETTINGS/1`. Ele aceita
  somente `language` e `quality`, limita tamanho e caracteres, rejeita UTF-8
  inválido, NUL, duplicatas e chaves desconhecidas e restaura os defaults
  seguros em toda falha.
- Adiciona o resolvedor puro de idioma V2. A ordem é sessão, settings,
  preferência SDL/firmware, locale POSIX somente leitura e fallback declarado;
  o resultado é um snapshot imutável para o adapter aplicar no ponto correto
  do ciclo nativo.
- Preserva integralmente a API legada de idioma e não altera `LANG`, `LC_ALL`,
  `setlocale`, arquivos, ambiente ou interfaces visuais.
- Instala os headers V2 e as implementações reais na biblioteca canônica. Um
  gate de consumo externo configura, compila, instala e liga um consumidor
  usando apenas os artefatos instalados.
- Acrescenta corpus adversarial determinístico para o parser e cobre as funções
  puras com GCC e Clang, sem device, rede, SDL ou filesystem dentro do parser.

### Escopo da release

`nxcompat` continua sendo uma biblioteca estática, caller-owned e sem seleção
por nome de jogo, aparelho ou CFW. A versão 0.3.0 não migra ports existentes e
não transforma os gates host em prova física. A adoção, os sinks de idioma e a
validação do artefato final continuam pertencendo ao adapter e ao release de
cada port.
