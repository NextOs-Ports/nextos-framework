# Stardew Valley (Android) — so-loader port — HANDOFF

**Status em 2026-08-04:** release universal v1.1.6 preparada sobre a baseline validada no
ArkOS/RK3326/Mali-G31 e na bancada original Amlogic/Mali-450. `Runtime_init`,
ativação Xamarin, lifecycle nativo, GLES, OpenAL, controles, criação de personagem,
save e gameplay funcionam. O teste atual carregou um save real, percorreu a lista
completa de Opções e voltou ao gameplay; o manifesto dos saves ficou idêntico antes
e depois.

A v1.1.2 corrige o relato RG-DS/ROCKNIX/Panfrost em duas fronteiras comprovadas. A
transação NXExtract de 394,3 MiB terminou e foi commitada; apenas sua interface visual
podia ficar invisível. O filho AArch64 agora prioriza SDL/Wayland do firmware, copiando
para `debug.log` somente o trecho novo de `ui.log`. Depois da instalação havia uma falha
separada: com contexto real `OpenGL ES 3.1 Mesa 26.1.2`, o `libGL` bloqueado deixava
handle nulo, mas `dlsym(NULL, "eglBindAPI")` encontrava a libEGL global. Mesa aceitava
`EGL_OPENGL_API`, MonoGame marcava `BoundApi=GL`, não reconhecia o FBO core de ES 3.1 e
lançava `PlatformNotSupportedException` antes do primeiro frame. O loader oculta somente
esse lookup vazado depois que a bridge GLES está pronta; o catch/fallback nativo do
MonoGame seleciona ES/libGLESv2. Um probe Mesa real confirmou ausência da string EXT,
entrypoints core presentes e FBO completo. O RG-DS ainda precisa do reteste físico da
release; o RG 40XX-H/muOS foi confirmado pela comunidade na v1.1.1.

O segundo trace comunitário, em KMSDRM/Mali proprietário, não contém outro crash:
chega a `swap #1`, faz três saves e sai pelo `_exit(0)` de SELECT+START. As mensagens
`music CopyTo ... limitado`/`seek rapido` confirmam o workaround XACT (metadados de
3436 bytes e `fseek`, sem ler fisicamente centenas de MiB); `*.XmlSerializers not found`
são probes opcionais que caem no serializer dinâmico. Como as linhas de runtime não têm
timestamp, o relato de uma pausa temporária de aproximadamente cinco minutos não pode
ser correlacionado. Não houve mudança especulativa em áudio/save/serialização; se a pausa
repetir, o próximo trace deve acrescentar tempo monotônico e amostragem de CPU/I/O.

Na v1.1.3, os arquivos visíveis em `ports/` e `ports_scripts/` viraram wrappers POSIX
mínimos e idênticos; ambos executam em foreground o único runtime
`ports/sdvnextos/run.sh`. Esse runtime resolve o diretório pela própria localização,
migra/prepara os dados com NXExtract e escolhe o loader pelo firmware. Não usa
`systemctl`, `setsid` nem perfil SDL/áudio fixo. O NextOS recebe o binário do sysroot
atual (glibc 2.43 nesta release); ArkOS e outros CFWs externos recebem o build
compatível (artefato atual GLIBC máx. 2.17). A saída continua no binário por
`SELECT+START -> _exit(0)`.

Na v1.1.4 esse runtime caiu de 305 para 141 linhas. Saíram a trava de lançamento, o
matador de instância anterior e todo o ciclo ES/TTY/`pm_finish`: no ArkOS o próprio
EmulationStation lança o port com
`sudo perfmax %GOVERNOR% %ROM%; nice -n -19 /usr/local/bin/AltSDL.sh %ROM%; sudo perfnorm`
e nunca é morto, então esse trabalho era duplicado. O handoff `pm_platform_helper`
continua sendo chamado, preservando os CFWs cujo `control.txt` faz mais que o do ArkOS.
A remoção corrige uma falha real: o antigo `stop_existing_game` matava qualquer processo
cujo executável casasse `*/ports/sdvnextos/stardewvalley*`, de modo que um segundo
lançamento derrubava com `SIGKILL` uma sessão já em gameplay — assinatura visível no log
como três `Killed` seguidos, incluindo subshells do `funcs.txt` do PortMaster.

**Regressão da v1.1.4, corrigida na v1.1.5.** Ao encurtar o launcher, a lista de
`chmod +x` foi reduzida aos dois loaders e aos entry points do NXExtract. Isso quebra o
caminho de primeira instalação: `nxextract-runtime-env.sh` executa `run-extractor.sh`
diretamente, e `run-extractor.sh` só roda a migração do assembly store quando
`tools/prepare_stardew_data.py` passa num `[ -x ... ]` — sem o bit, a migração é pulada
**em silêncio**, sem erro algum. A lista completa voltou, junto com a guarda `[ ! -L ]` do
helper carregado por `source`, a checagem de `extractor.json` e a validação de `uname -m`.
Armadilha de bancada que escondeu isso: o `/roms` do ArkOS de teste é **exfat com
`fmask=0000`**, então todo arquivo é sempre 0777 e `chmod` não tem efeito observável —
qualquer teste de permissão feito só ali dá falso positivo. O
`tests/launcher_permissions_test.sh` cobre isso em ext4 e falha contra a v1.1.4.

**Regressão de lançamento no muOS/RG 40XX-H, corrigida na v1.1.6.** Relato: v1.1.5
instalada, o port volta ao menu e **nenhum arquivo de log é gerado**. "Sem log" localiza a
falha antes do redirecionamento do `run.sh` — e havia dois pontos cegos somados. Primeiro,
o wrapper visível fino (introduzido na v1.1.3) precisa *encontrar* `ports/sdvnextos/run.sh`
numa lista fixa de raízes que cobria `/roms`, `/roms2` e `/storage/roms`, mas não os
layouts do muOS em `/mnt/mmc` e `/mnt/sdcard`; além disso derivava o diretório de `$0` sem
resolver symlink. Segundo, ao não encontrar, ele escrevia apenas em stderr — que o frontend
descarta —, então o modo de falha era silêncio absoluto. Agora o wrapper resolve o próprio
caminho real, procura relativo a si mesmo e em muito mais layouts, e **sempre grava
`stardew-launcher-error.log`** em disco. O `run.sh` passou a abrir o log como primeira ação,
com fallback para `TMPDIR`, e voltou a **supervisionar** o jogo (`"$BIN"; status=$?`) em vez
do `exec "$BIN"` da v1.1.5, conforme a seção 5 de `launcher-portmaster.md`.

Ponto importante de prova: o muOS tinha confirmação física apenas na **v1.1.1**, quando o
arquivo visível ainda era o launcher completo e não precisava procurar nada. As v1.1.2 e
v1.1.3 nunca foram validadas nesse hardware, então a regressão de layout passou três
releases sem aparecer. `tests/launcher_wrapper_test.sh` agora cobre wrapper alcançado por
symlink e falha nunca-silenciosa; ambos falham contra a v1.1.5.

**Perfil de memória medido no ArkOS (RG351MP/R36S, 639 MB, ES parado, 2026-08-04.)**
A pausa sem explicação do trace anterior foi instrumentada com relógio de parede e
amostragem por thread. Não há OOM: nenhum `Killed process` em `/var/log/dmesg*`, e
`MemAvailable` nunca ficou crítico. O jogo renderiza 60 fps cravados (600 frames a cada
10 s) e o RSS evolui assim: ~280 MB no título, ~380 MB ao iniciar o carregamento do
mundo, **pico de 534 MB** e estabilização em ~517 MB já em gameplay, com 11.100 frames
em 304 s. Ou seja, a folga real num aparelho de 640 MB é de dezenas de MB, não centenas.
Detalhe operacional: com `vm.swappiness=60` o zram de 512 MB ficou **inteiramente sem
uso** (`SwapFree` intacto) mesmo com `MemFree` em 5 MB — o kernel preferiu descartar page
cache. Quem quiser que o zram absorva o pico precisa subir `vm.swappiness` e zerar
`vm.page-cluster`. Aviso para o próximo trace: um contador de tempo que some apenas o
`sleep` do amostrador subestima o tempo real quando cada iteração varre `/proc`, e isso
faz um `timeout` do próprio teste parecer morte do jogo.

Os dados proprietários são BYO-data. NXExtract 1.1.2 aceita APK/APKM/APKS/XAPK/ZIP,
valida a árvore exata e confirma `libs`/`assets` transacionalmente. O hook portátil
Python 3.7+ migra tanto o store original quanto os patches públicos anteriores para
o hash final, sem tocar nos saves. Desde v1.1.1, o hook carrega primeiro a
`tools/liblz4.so.1` AArch64 auditada do Debian Buster (SHA-256
`a65c53e2e7015b636e4f212449eff2016b99736cdf5798fe2cf3672818b88b8b`, GLIBC máx.
2.17), eliminando a dependência opcional ausente no ROCKNIX/RG-DS. O pacote público
é montado por allowlist, reprodutível e auditado contra dados de jogo, logs,
caminhos pessoais e procedência de build inválida.

O restante deste documento preserva o histórico do bootstrap e deve ser lido como
diagnóstico antigo, não como descrição do estado atual.

### Destravamentos posteriores ao handoff histórico

- AssetManager/InputStream fake lê XNB/XACT diretamente de `assets/`.
- SDL cria o contexto ES2 real; EGL10 fake apenas apresenta esse contexto ao
  MonoGame. O backbuffer recebe alpha 1 antes do swap para o compositor Amlogic.
- `Game1.SetRenderTarget` era silenciosamente ignorado porque o fake
  `Looper.myLooper()` fazia `SyncContext.Send` executar na task background. A
  worker é promovida para foreground no próprio `myLooper`, restaurando a
  semântica da UI thread e habilitando os FBOs.
- Wrappers temporários de trace GL corrompiam o Mali após cerca de 1500 frames.
  O dispatch de produção agora devolve diretamente os entrypoints do contexto;
  o teste passou de 14 mil frames.
- `libopenal32.so` do MonoGame é redirecionado para o `libopenal.so.1` do NextOS.
- O banco `music.xwb` de 258.574.094 bytes não é mais copiado para um
  `MemoryStream`: o fake Asset InputStream expõe 3436 bytes de metadados ao
  construtor e converte o descarte da worker em `fseek`, mantendo a leitura real
  do payload. Isso removeu o pico de ~520 MiB e o atraso por faixa.
- `SDL_GL_SetSwapInterval(1)` é aplicado com o contexto atual; o driver confirmou
  sucesso (`result=0`) e eliminou o tearing percebido como pequenos flickers.
- `TextBox.ShowAndroidKeyboard` chama o `TextEntryMenu` interno do jogo. Antes de
  abrir, limpa somente quando `Text == TitleText`; assim os placeholders vermelhos
  de personagem/fazenda/favorito somem como no Android e nomes reais sobrevivem a
  reaberturas.
- `IClickableMenu.receiveGamePadButton` tem um wrapper mínimo no code cave já
  existente: quando a página atual é `OptionsPage`, D-pad Cima/Baixo chama o scroll
  limitado da própria página. Todos os demais botões e menus preservam byte por
  byte a lógica original. O caminho foi confirmado no runtime Android/MonoGame,
  que entrega `Buttons.DPadUp/DPadDown` e não eventos `Keys`.
- `Options.singlePlayerBaseZoomLevel` é serializado por save. Por isso o save antigo
  testado mantinha o enquadramento bom enquanto uma fazenda nova começava em 1,0. O
  patch v1.1.1 muda somente os defaults de um objeto novo para 0,75 (mínimo suportado
  pelo jogo); preferências carregadas continuam sobrescrevendo o default.
- No port offline, `NewDaySynchronizer.readyForSave` confirma apenas a barreira
  de rede que ficava presa sem backend. O fake `Java.IO.File.get_UsableSpace`
  devolve o `statvfs` real; antes retornava zero e simulava disco cheio. O fluxo
  normal chegou a `Save in Progress`, escreveu os dois XMLs e entrou na fazenda.
- O analógico direito controla uma seta pixel-art nativa separada do foco do jogo. Ela
  usa deadzone radial/aceleração, some após 2 s e R3 envia `MotionEvent`
  `DOWN/MOVE/UP` ao `AndroidTouchEventManager` usando a ponta superior esquerda como
  hotspot; R3 continua sendo o botão nativo quando o cursor está oculto.
  `SDV_RIGHT_CURSOR=0` desliga o recurso.
- Limpeza pós-gameplay: logs por tecla ficaram opt-in (`SDV_INPUT_TRACE=1`),
  `debugPrintf` não duplica mais gravações, `sem_timedwait` converte corretamente
  deadline absoluto para futex relativo e o loader serializa cargas Bionic,
  libera mmaps de falha e nunca entrega handles custom ao `dlsym/dlclose` glibc.

APK: `com.chucklefish.stardewvalley_1.6.15.3` — app **.NET 8 / Mono-Android AOT**
(`libaot-StardewValley.dll.so`, `libmonosgen-2.0.so`, `libmonodroid.so`,
`libxamarin-app.so`, MonoGame.Framework). NÃO é jogo C/C++ nativo — é o 1º port
Mono-Android do repo (sem precedente).

## O que FUNCIONA (milestone — 1ª vez no repo)

O ELF loader custom (linhagem max_arm64, copiado do `gtalcs2`) carrega a cadeia
Bionic de módulos e roda o `JNI_OnLoad` do Xamarin com sucesso:

1. `libmonosgen-2.0.so` — carrega, reloc, init_array OK, **1322 símbolos mono** exportados (snapshot).
2. `libxamarin-app.so` — carrega, 28 símbolos.
3. `libmonodroid.so` — carrega, 5 init_array OK.
4. `JNI_OnLoad(fake_vm)` → chama `Util.initialize`, `GetEnv`, `OSBridge::initialize_on_onLoad`. **Retorna `0x10006` (JNI_VERSION_1_6).** OSBridge inicializado.

A fake JavaVM/JNIEnv (offsets de vtable Bionic arm64 LP64, verificados no gtalcs2)
é suficiente pro `JNI_OnLoad`. Log confirmado: `FindClass`, `GetMethodID`,
`CallStaticObjectMethodV` todos despacham sem crash.

## O MURO (duas vias, mesma causa raiz)

O runtime Mono é compilado contra **Bionic** (libc do Android). Rodando em
**glibc** via symbol-shimming, o estado de baixo nível do runtime corrompe
(TLS, sinais, tamanhos de allocator). Não é shimmável no nível de símbolo —
precisa de Bionic real (libhybris/container) ou um Mono build glibc.

- **Via A — `Java_mono_android_Runtime_init` (Xamarin):** executa, Mono lê
  props (`debug.mono.log`, `debug.mono.max_grefc`), processa TimeZone/lang,
  loga "Failed to create directory for TMPDIR", depois **SIGSEGV em glibc
  (`libc+0x95488`, SIMD strlen) chamado de `libc+0x3a2b4` (região
  setcontext/__tfind)**. `x0=x1=0`, `x2=0x000f0000f0000000` (constante em
  todo run — dado lido de TLS/global Bionic). Imune a TODOS os shims
  (string-ops, mkdir, property, JNIEnv não-NULL, syslog/vfprintf) — porque é
  glibc chamando glibc internamente (não passa pelo GOT que shimmamos).
- **Via B — `mono_jit_init_version` direto (bypass):** runtime começa a init,
  mas trava em asserção concreta do Mono:
  `mono/utils/lock-free-alloc.c:608: condition (block_size & (block_size-1))==0 not met`
  (`block_size` não é potência de 2 — valor runtime corrompido pela ABI).

## Arquitetura do loader (como funciona)

- `main.c` — cadeia: `load_module(libmonosgen)` → `so_snapshot_symbols` (mono_*) →
  `load_module(libxamarin-app)` → snapshot → `load_module(libmonodroid, tabela combinada)`.
  Depois `jni_build_env()` (fake VM/env) e chama `JNI_OnLoad`.
- `jni_shim.c` — fake JNIEnv/JavaVM. Vtable plana nos offsets Bionic
  (`FindClass@0x30`, `GetMethodID@0x108`, `GetEnv@0x30` da JavaVM, etc.).
  Dispatch genérico: métodos desconhecidos recebem ID válido + default
  (sentinel não-NULL pra evitar NULL→strlen crash).
- `imports.gen.c` — shims Bionic→glibc: `__errno`, `__android_log_*`,
  `__system_property_get` (log + valores reais p/ TMPDIR/sdk/abi),
  `android_dlopen_ext`, `my_sigaction` (struct sigaction 32B→152B),
  string-ops NULL-safe, `mkdir`/`strerror`/`syslog`/`vfprintf` NULL-safe.
- `bionic_shims.c` / `pthread_bridge.c` — copiados do gtalcs2 (pthread_t Bionic→glibc,
  FORTIFY `_chk`, etc.).

## Build / run / deploy

```sh
bash ports/stardewvalley/build.sh   # host toolchain
# device (qualquer Mali-450):
scp .../stardewvalley root@<device-ip>:/storage/roms/ports/stardewvalley/
ssh root@<device-ip> "cd /storage/roms/ports/stardewvalley && \
  nice -n 19 timeout 90 ./stardewvalley"            # milestone JNI_OnLoad
# SDV_RUNTIME_INIT=1  -> tenta Runtime_init (Via A, crasha)
# SDV_MONO_JIT=1      -> tenta mono_jit_init_version (Via B, assertion)
```
Device: NextOS 4.8.2 EmuELEC, amlogic Gxbb (Mali-450 old), glibc 2.43, aarch64.
Libs do APK staged em `ports/stardewvalley/libs/` (78 .so).

## Caminhos pra frente (decisão a tomar)

1. **Bionic real (libhybris-style):** trazer `libc.so`/`linker64` do Android e
   carregar o Mono no namespace Bionic (TLS/sinais/pthread nativos). É o que
   faltou — shimming de símbolo não chega no nível que o Mono precisa. Esforço
   grande de infra, mas é o caminho "correto" pra Mono-Android no so-loader.
2. **Container Android (waydroid/halberd):** rodar o APK inteiro num userland
   Android. Pesado, fora do padrão so-loader.
3. **Mono build glibc:** usar um runtime Mono compilado p/ glibc + carregar os
   assemblies do blob (AOT images amarrados à versão Bionic — frágil).
4. **Mainline (PC Windows via .NET + gl4es):** JÁ STAGED no device
   (`/storage/roms/ports/stardewvalleymainline`, 740MB, + fix NextOS em
   `SDV-FIX-2026-05-26/libsv_fs.so`). Caminho comprovado (PortMaster) p/ gameplay,
   mas NÃO é o Android so-loader que o usuário pediu.

## Progresso ITERATIVO (2026-07-10 — quebrando muros, fluxo nativo Runtime_init)

O bootstrap avançou muito além do "muro fundamental". Mono loga verboso
(`SDV_MONO_LOG=all` via `debug.mono.log`), cria dirs XDG. Muros derrubados:

1. **`strdup/strlen(NULL)` no path TMPDIR** — Mono deriva dirs do Context Android
   (NULL aqui) → `setenv("TMPDIR", NULL)` → glibc `strlen(NULL)` interno. **Fix:**
   shim `setenv`/`putenv` NULL-safe (`imports.gen.c`). → Mono passou p/ HOME, XDG.
2. **`wrapper.env=NULL` no dir XDG** (@0x3730c `ldr x8,[x0]`, x0=wrapper.env) —
   patch `SDV_PATCH_RT` faz `b` always-skip em 0x37300. → Mono cria dirs XDG.

**Muro ATUAL:** `determine_primary_override_dir` (@0x48788) — MESMO padrão
`wrapper.env=NULL` (`ldp x8,x1,[wrapper]; ldr x9,[x8]` com x8=NULL). É SISTEMÂMICO:
todo wrapper de dir tem `env=NULL`.

**CAUSA RAIZ (próximo muro a derrubar):** Mono guarda o JNIEnv por thread numa
TLS (pthread_key), setada em `AttachCurrentThread`. A thread main NÃO está
"attached" → `get_jnienv()` devolve NULL → wrappers com `env=NULL`. No Android
real, a main thread é attached no boot do processo. **Fix候选:** achar a
função `get_jnienv`/attach do Mono (TLS pthread_key em libmonodroid/monosgen —
`axt @ pthread_getspecific` voltou vazio, precisa busca manual no .text) e
ou (a) chamá-la p/ attach da main thread com fake_env, ou (b) hooká-la p/
devolver `fake_env`. Isso conserta TODOS os wrappers de uma vez.

Flags de run: `SDV_RUNTIME_INIT=1 SDV_MONO_LOG=all SDV_PATCH_RT=1`
(+ optional `SDV_MONO_JIT=1 SDV_PATCH_ASSERT=1` p/ bypass via mono_jit_init,
que bate em assertion `lock-free-alloc.c:608` / `sgen-internal.c:299` —
size-classes do GC SGen corrompidas, mesmo família de causa).

## Próximos passos sugeridos (se seguir pela Via so-loader)

- Resolver a asserção `lock-free-alloc.c:608`: decompilar `mono_lock_free_allocator_alloc`
  em `libmonosgen-2.0.so` e achar de onde vem `block_size` (provável: page/cache-line
  size ou sizeof de struct cujo layout difere Bionic↔glibc). Se for leitura de TLS
  Bionic, confirmará que precisa de Bionic real.
- Ao investigar, usar `rizin` com `e io.cache=true; pd N @ addr` (não `aaa` —
  libmonosgen é 3MB, análise completa é lenta/falha).
- O crash da Via A é opaco (glibc interno); a Via B dá mensagem concreta —
  preferir investigar pela Via B.

## GRANDE MARCO (2026-07-10 sessão 2) — Runtime_init INICIA de verdade

Derrubados 3 muros (além dos 2 anteriores). Resultado: o fluxo nativo Via A
(`SDV_RUNTIME_INIT=1 SDV_MONO_LOG=all SDV_PATCH_RT=1`) agora chega a:

```
[alog] Enabling AOT mode in Mono
Mono Warning: --llvm not enabled in this runtime.
[alog] [0/7] Runtime.init: Mono runtime init   <- MONO RUNTIME INIT INICIOU
[alog] Registering assemblies from the filesystem
[alog] Looking for assemblies in '(null)'        <- muro ATUAL
```

Antes travava em `determine_primary_override_dir` (env=NULL); agora chega ao
registro de assemblies — muito alem do que a secao "Muro" acima documentava.

Muros derrubados nesta sessao:

1. `wrapper.env=NULL` sistemico em `determine_primary_override_dir` (0x48778) -
   todo consumidor de `jstring_wrapper` faz lazy `c_str()` lendo `wrapper.env`, e
   `determine_primary_override_dir` e o UNICO sem `cbz env,skip` graceful.
   Fix: `patch4(0x48778, 0x1400000a)` (b 0x487a0) - igual ao patch XDG.
   (Apos o fix #3 - libFolders nao-vazio - o branch A propaga env e este patch
   deixa de ser estritamente necessario, mas mantido por seguranca.)

2. `__sF@LIBC` mismatch stdio Bionic<->glibc (CHAVE - dava crash opaco em libc) -
   Mono referencia `__sF` (array de FILE stdin/stdout/stderr com LAYOUT Bionic,
   sizeof(FILE_bionic)=0x98, stderr em `__sF+0x130`). GOT `0x2f3020` em
   libmonosgen. glibc NAO exporta `__sF` -> GOT fica NULL -> Mono usa `__sF+0x130`
   como stream -> fwrite/fprintf(NULL+0x130) crasha em libc (addr=0x8). Confunde
   com "crash glibc interno". Fix: shim `__sF` = buffer proprio (`g_sF[0x200]` em
   `imports.gen.c`) + interceptar fwrite/fprintf/fputs/fputc/vfprintf (PLT) que
   remapeiam stream na regiao `g_sF` -> stdin/stdout/stderr da glibc real
   (`sdv_stream_remap`: off<0x98->stdin, 0x98..0x130->stdout, >=0x130->stderr).
   Descoberta reutilizavel p/ qualquer port Mono-Android.

3. `libFolders` vazio em Runtime_init (arg 6) - passavamos `empty_arr`
   (GetArrayLength=0) -> branch B em 0x37284 (nao propaga env ao wrapper) E app
   data dir/assembly path = NULL. No Android real libFolders traz os dirs de
   libs do app. Fix: passar token 0x4001 (1 elemento = path real
   /storage/roms/ports/stardewvalley/libs via SDV_LIBDIR); jni_shim trata
   GetArrayLength(0x4001)->1 e GetObjectArrayElement(0x4001,0)->path. Branch A
   tomado, env propagado, libs/ lido. Deploy OBRIGATORIO do libs/ completo
   (78 .so, 52MB - libassemblies.arm64-v8a.blob.so + libaot-*.so) no device.

Correcao de hipotese anterior: `get_jnienv()` @0x33dac (via
`OSBridge::ensure_jnienv` -> `osbridge->vm->GetEnv` = nosso fake_vm) FUNCIONA e
devolve `fake_env`. O "main thread nao attached ao TLS" NAO era a raiz - a raiz
era `libFolders` vazio. get_jnienv so tem 2 callers (network code).

MURO ATUAL: `app_lib_directories` (global AndroidSystem::app_lib_directories
@0x6b5c0) esta NULL -> register_from_filesystem loga "Looking for assemblies in
'(null)'" -> excecao -> mono_debugger_agent_unhandled_exception (@0x1cf3d8 em
libmonosgen) crasha ao reportar. Populado por
`AndroidSystem::setup_app_library_directories` @0x48218 a partir de paths do
Context Android (dataDir/nativeLibraryDir via JNI) -> NULL. Proximo: fazer
setup_app_library_directories (ou injetar em app_lib_directories) o path libs/
onde estao libassemblies.arm64-v8a.blob.so + libaot-*.so.

Flags de run (Via A): SDV_RUNTIME_INIT=1 SDV_MONO_LOG=all SDV_PATCH_RT=1
(libFolders automatico; SDV_LIBDIR opcional).

## MURO FUNDAMENTAL CONFIRMADO (2026-07-10 sessão 2, apos count=3 + __sF)

Apos popular app_lib_directories (count=3 libFolders), Mono carrega 65
assemblies + libarc.bin (runtimeconfig) + libassemblies.blob. Entao bate em
ASSERTIONS DO ALLOCATOR SGen/GC — a familia fundamental Bionic<->glibc:

1. lock-free-alloc.c:608 — `(block_size & (block_size-1)) == 0` (em
   mono_lock_free_allocator_init_size_class @0x1e7934, chamada pelo loop de
   size-classes @0x2bc820; block_size=w2 deriva de mono_pagesize()). NOP @0x1e7954
   (SDV_PATCH_ASSERT=1) pula.
2. sgen-internal.c:299 — `allocator_sizes[index_for_size(max_size)] == max_size`
   (SGen GC size-class table). Aparece logo apos NOPar o #1.

Ambas as vias convergem aqui (Via A Runtime_init e Via B mono_jit_init). O
SDV_PATCH_ASSERT revela o #2 logo apos o #1 — whack-a-mole da MESMA raiz:
size-classes do allocator computadas/corrompidas pelo mismatch de ABI
Bionic<->glibc (sizeof de structs, page-size). NAO eh shim'avel no nivel de
simbolo — confirma a Licao NAO-OBVIOSA: Mono-Android no so-loader precisa de
Bionic real (libhybris/container) pra ter o allocator/SGen corretos.

CONCLUSAO: o so-loader (Via A) chegou MUITO longe (carrega 65 assemblies,
runtimeconfig) mas o limite fundamental eh o allocator SGen/GC. Gameplay neste
caminho exigiria Bionic real. Alternativa comprovada: mainline PC
(.NET+gl4es) ja staged no device.

Flags Via A: SDV_RUNTIME_INIT=1 SDV_MONO_LOG=all SDV_PATCH_RT=1
[SDV_PATCH_ASSERT=1 pula lock-free-alloc:608 e revela sgen-internal:299].

## MARCO: EXECUTANDO .NET GERENCIADO (2026-07-10 sessao 3)

Shims adicionais (todos em imports.gen.c/jni_shim.c):
- sysconf: map _SC Bionic{39,40,96,97,98,99}->valores reais. RAIZ do
  lock-free-alloc.c:608 + sgen-internal.c:299 (mono_pagesize voltava 1000, nao
  4096, pois _SC_PAGESIZE Bionic=39 != glibc). NAO era muro fundamental.
- sem_* (init/wait/post/trywait/timedwait/destroy) com futex: sem_t Bionic(16B)
  != glibc(32B); glibc sem_wait SIGBUS no semaforo global do mono.
- GetStaticObjectField/GetObjectField devolvem token de classe nao-nulo (Mono le
  campos Class Java p/ GCUserPeer; NULL -> abort).
- sdv_so_dlopen/sdv_so_dlsym (main.c): carrega .so Bionic via so_util contra a
  tabela combinada + snapshot p/ dlsym. Shim dlopen/dlsym/android_dlopen_ext
  (fallback so-loader apos glibc falhar). Componentes mono (marshal-ilgen) e
  imagens AOT carregam assim.
- sdv_so_dlopen RECUSA libaot-*.dll.so -> mono JIT-a (libaot tem PLT/IRELATIVE
  que o so-loader nao trata; lazy-PLT crash no ld-linux).

RESULTADO: Mono carrega 65 assemblies + componentes, mapeia Mono.Android/
Java.Interop/System.Private.CoreLib, e EXECUTA codigo C# gerenciado com stack
trace. Chega em Java.Interop.JniRuntime / Android.Runtime.AndroidRuntime /
JNIEnvInit.Initialize. Exception handling do mono funciona (FATAL UNHANDLED
EXCEPTION com managed stack).

MURO ATUAL: "No JavaVM registered with handle 0x0" (managed). O args.javaVm
passado a JNIEnvInit.Initialize (Android.Runtime) vem 0x0 — o nativo (mono_runtime_init
@0x2b594 ou Runtime_register @0x31748) nao esta setando args.javaVm=fake_vm (ou le
OSBridge::java_vm num global que ficou 0). Java.Interop precisa do JavaVM
registrado (java_interop_register_jvm, exposto via xamarin_app_init que so guarda
um callback em libxamarin-app). Investigaçao: achar onde args.javaVm eh setado e
garantir fake_vm.

Device de teste: Mali-450 old (estavel, uptime de dias).
Flags: SDV_RUNTIME_INIT=1 SDV_MONO_LOG=all SDV_PATCH_RT=1 (sysconf/sem/dlopen/force-JIT automaticos).

## MARCO DEFINITIVO (2026-07-10 sessao 4) — Runtime.init COMPLETA e RETORNA (exit=0)

`Java_mono_android_Runtime_init` agora **termina limpo** e retorna. Log final:
```
[0/8] Runtime.init: end native-to-managed transition
.NET for Android version: 13.2.99 (ARM64; Release); MonoVM version: 9.0.8.0
[0/11] Runtime.init: end, total time
=== Runtime_init RETORNOU ===
```
A inicializacao inteira do runtime .NET-for-Android completou. O "muro
fundamental do allocator" da sessao 2 estava ERRADO — nao era Bionic-real, era
so um `sysconf(_SC_PAGESIZE)` mal-mapeado (ja resolvido na sessao 3).

4 muros derrubados nesta sessao (todos em `jni_shim.c`, vtable do JNIEnv):

1. **`GetJavaVM` slot 0x6D8 (idx 219)** — sem ele, `args.javaVm`=0 no managed ->
   "No JavaVM registered with handle 0x0". Devolve `fake_vm`.
2. **Offsets ERRADOS de excecao** (o mais sutil): `ExceptionOccurred` estava em
   0x90 (=FatalError, idx 18) e `ExceptionCheck` em 0x98 (=PushLocalFrame,
   idx 19). Os slots REAIS (idx 15=0x78, idx 228=0x720) caiam no default
   `ret_obj` (0xC1A501 nao-NULL) -> o managed .NET achava que SEMPRE havia
   excecao pendente apos cada FindClass -> `GetExceptionForThrowable` -> tentava
   embrulhar um `Throwable` -> typemap managed->java falhava
   ("Could not determine Java type ... Java.Lang.Throwable"). O Throwable era
   SINTOMA, nao a raiz. Fix: mover pros offsets certos; adicionar
   PushLocalFrame(0x98)->0 / PopLocalFrame(0xA0).
3. **UTF-16 strings** `GetStringLength`(0x520)/`GetStringChars`(0x528)/
   `ReleaseStringChars`(0x530). `Java.Interop.Strings.ToString` usa a via UTF-16
   (nao UTF-8); sem os slots, caia no default -> `new string((jchar*)0xC1A501)`
   -> SIGSEGV em Memmove/memcpy. Nossas jstring sao char* ASCII -> byte zero-ext.
   (De passagem: `GetStringUTFLength` estava desalinhado em 0x53C -> 0x540.)
4. **`RegisterNatives` retorno** — era `static void` -> managed lia lixo em x0
   (=2=count) -> "JNIEnv::RegisterNatives() returned 2" -> InvalidOperationException.
   Agora `int` retornando 0 (JNI_OK).

Warnings benignos remanescentes: "asked if a class System.NullReferenceException
is a bridge before we inited java.lang.Object" (ordem de init do GC bridge).

**MURO ATUAL / PROXIMA FASE:** o runtime esta UP mas `main.c` so faz `exit(0)`
apos Runtime_init. Falta **invocar a entrada do jogo** — no Android real o
sistema instancia `StardewValley.MainActivity` (extends AndroidGameActivity) e
chama `onCreate` -> managed `OnCreate` -> cria o `Game` -> `Game.Run()` (loop
MonoGame/SDL). No so-loader nao ha Activity system. Proximo: achar/invocar a
entrada gerenciada via embedding Mono (mono_runtime_invoke) ou dirigir o
`n_onCreate` registrado (capturamos os natives em g_natives:
construct/registerNativeMembers). Este e o salto "runtime up" -> "jogo na tela".
