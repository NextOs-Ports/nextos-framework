# Bomb Chicken → Mali-450 so-loader — HANDOFF / diário vivo

> **ÂNCORA DE MEMÓRIA. Atualizar a CADA iteração.** Se o contexto resetar, reler isto +
> `STUDY.md` e continuar daqui. **A recon JÁ ESTÁ FEITA — não refazer.**

## 🚨 REGRAS DESTE PORT
1. **Resolver TUDO, sem atalhos.** Manter o máximo do original.
2. **Entregar IMAGEM + CONTROLE + ÁUDIO + INGLÊS.** Começar e **NÃO PARAR** até o gameplay.
3. Globais do `MEMORY.md`: só master (sem branch), **sem co-autor Claude**, matar+confirmar
   0 instâncias **pelo diretório** com `sudo ps` antes de lançar, nunca `setsid` no Mali-450,
   nunca forçar `SDL_VIDEODRIVER`/`AUDIODRIVER`, sem swap extra, **jamais cravar resolução**,
   dado de jogo **nunca** vai pro repo (`.gitignore` feito).

## ALVO / ACESSO

O aparelho usado na validação de 07/08/2026 foi autorizado somente naquela sessão.
**Nunca reutilizar endereço, hostname ou credencial deste diário; usar apenas o alvo
explicitamente autorizado na sessão atual e nunca varrer a rede.**

Confirmado ao vivo em 07/08: **NextOS-Retro-Elite-Edition 4.8.2-Nexus_devel_20260728164919**,
**aarch64**, **`/dev/mali` presente** (Mali-450), **glibc 2.43**, **916 MiB RAM** (~530
disponíveis), unit do ES **`emustation`** ativa.

- Deploy: `/storage/roms/ports/bombchicken/` · launcher em `/storage/roms/ports_scripts/`.
- ⚠️ `pkill -f <padrão>` inline mata a própria sessão ssh — usar `[c]olchete`.
- ℹ️ A validação ocorreu em aparelho dedicado àquela sessão; não misturar evidências
  ou alvos de outros ports.
- 💡 Dá para adiantar **P1–P3 sem aparelho**: build, carga dos módulos, `JNI_OnLoad`,
  IL2CPP e até o C# do jogo rodam com **EGL/GLES nulo (`nullgl.c`) + `qemu-aarch64`** —
  foi assim que o Shadow Fight 2 venceu o Choreographer. Não prova pixel, prova o resto.

- **Toolchain histórico:** SDK local NextOS com
  `bin/aarch64-libreelec-linux-gnu-gcc` e sysroot
  `aarch64-libreelec-linux-gnu/sysroot` (glibc **2.43**). Caminhos locais e
  diretório temporário são deliberadamente omitidos; o build público atual usa a
  receita offline, pinada e de baixa glibc descrita abaixo.

## O QUE JÁ ESTÁ NA PASTA
```
ports/bombchicken/
├── STUDY.md            ← RECON COMPLETA + as 14 armadilhas de Unity 2022. LER ANTES DE TUDO.
├── HANDOFF.md          ← este arquivo
├── libil2cpp.so        ← arm64 39,4 MiB  (⚠️ 4 PT_LOAD — mapear por IMAGEM)
├── libunity.so         ← arm64 16,8 MiB
├── libmain.so          ← 6,7 KiB
├── gamedata/game.apk   ← APK inteiro (a Unity lê assets de dentro do ZIP)
├── src/                ← scaffold do new-port.sh
└── .gitignore
```

## ENGINE — resumo (detalhe em STUDY.md)
- **Unity 2022.3.39f1 IL2CPP, arm64-only.** Sem PairIP, sem packer.
- 🟢 **GLES2 EXISTE NOS DADOS**: 67 dos 72 shaders têm variante **`#version 100`**.
  `GfxDeviceGLES` presente, **`GfxDeviceVulkan` ausente**, `force-gles20` no binário.
  (Foi isso que faltou no Pikuniku → aquele é NO-GO, este não.)
- **Texturas**: 3528, só **10 em ETC2** (0,3%); ~97 MiB nativos no total. Sem ASTC.
- Áudio **FMOD built-in → OpenSL ES**; `libmediandk` no NEEDED.
- Input **Rewired** → ponte `InputDevice`. **Não é touch-first: sem cursor.**

## PRÓXIMOS PASSOS (ordem exata)

- [x] **P1 — Loader compila e carrega os 3 módulos.** `libmain` → `libunity` → `libil2cpp`.
      **Mapear por IMAGEM** (o `libil2cpp` tem 4 `PT_LOAD`; `mmap` precisa de `PROT_EXEC`).
      **Interceptar `dlopen`/`dlsym`** — a Unity carrega o `libil2cpp` sozinha e, se falhar,
      sobe o diálogo *"Failed to load Il2CPP."* e trava esperando um botão inexistente.
      Shims Bionic: `__sF` (**array de STRUCTS**, `stdout` = `&__sF[1]`), `sysconf`
      (`_SC_NPROCESSORS_ONLN` = `0x61` no Bionic vs `84` no glibc — errar dá
      `ArgumentOutOfRangeException: concurrencyLevel must be positive`), `sigaction`
      traduzido (senão o SIGPWR do GC do IL2CPP mata o processo: *"Falha de energia"*),
      `dl_iterate_phdr` próprio (senão toda exceção vira `Il2CppExceptionWrapper`).
- [x] **P2 — Capturar o `RegisterNatives` do `JNI_OnLoad`.** A `libunity` **não exporta**
      `Java_com_unity3d_player_UnityPlayer_*` — registra ~44 nativos ali dentro. Sem esse
      array não existe `initJni` nem `nativeRender`. `initJni` é **static e recebe o
      Context**: `(JNIEnv*, jclass, jobject)`.
- [x] **P3 — Handshake do Choreographer (o muro conhecido).** Objeto C++ com **herança
      múltipla**; `nativeProxyInvoke` por nome devolve **null calado**. A chamada certa é o
      **slot da vtable secundária**: `+8` handleMessage, `+0x10` doFrame. Sem isso a main
      dorme para sempre em `pthread_cond_wait`.
      Também: `ReflectionHelper` não é opcional (jmethodID pelo **slot 7** do JNIEnv,
      `FromReflectedMethod`); jmethodID é por **(nome, ASSINATURA)**; a Unity
      **desreferencia o `jobject`**.
- [x] **P4 — GLES2 + primeira imagem.** Resolução **lida do framebuffer** (regra #25).
      **Conferir que a engine escolheu os shaders `#version 100`** e não os `300 es`.
      Se `m_MTRendering` atrapalhar, é **patch de bytes** no `globalgamemanagers` —
      reescrever com UnityPy **perde objetos** (1,0 MB vira 55 KB).
      Testemunha de vídeo = `nx_frameprobe` (`NX_FRAMEPROBE=1`, desligar no release);
      captura do fb0 sozinha **nunca** declara preto. Medir RSS aqui.
- [x] **P5 — As 10 texturas ETC2.** Hook em `glCompressedTexImage2D` decodificando para
      RGBA8888 (~9 MiB no total). Só essas 10 — o resto é RGBA32/RGB24/Alpha8.
- [x] **P6 — Áudio.** FMOD built-in → OpenSL (`opensles_shim`). `libmediandk` pode
      precisar de shim para áudio comprimido.
- [x] **P7 — Input Rewired.** Ponte `InputDevice` do Java —
      `reference_rewired_android_ponte_inputdevice_completa`. Pad por
      `$sdl_controllerconfig` (a linha do device, **nunca** o `gamecontrollerdb.txt`
      inteiro) + **hotplug**. Select/Start **também** como `TRIGGER_HAPPY1/2` (regra #29).
      **Sem cursor** — não é jogo de toque.
- [x] **P8 — `tools/nx-verify <ip> bombchicken --game <padrão>` VERDE**
      (`verify/latest.json`, `self_test.ok=true`), launcher limpo padrão PortMaster
      (**jamais** parar/religar o ES no launcher — regra #30), commit+push no master.

## REFERÊNCIAS (só port que FUNCIONA — regra #16)
- **Estrutural, PUBLICADO:** `ports/hitmango` (v1.2.0, Unity so-loader).
- **Lições:** `reference_unity2022_soloader_armadilhas` (17 itens),
  `reference_rewired_android_ponte_inputdevice_completa`,
  `reference_il2cpp_metadata_offline_portao`.
- ⚠️ `ports/shadowfight2` e `ports/amongus` são Unity 2022.3 mas **NÃO estão terminados** —
  valem como **lição**, nunca como código a copiar.

## MURO ATUAL
**Nenhum.** O Choreographer nunca virou muro: o handshake por slot da vtable
secundária já vinha resolvido na base estrutural do `ports/hitmango` (publicado)
e funcionou de primeira — o log mostra `UnityChoreographer first doFrame
delivered` no frame 3.

**O muro real era outro, e foi aberto:** `GamesSignInClient.isAuthenticated()`
devolvia `null` pelo shim JNI, o GooglePlayGames estourava
`NullReferenceException` **dentro de `LevelStart.Awake()`** e o resto do Awake
nunca rodava — o jogo ficava para sempre numa tela preta com a engine rodando,
áudio e 3500+ draw calls por segundo. Consertado com uma regra genérica no
`dispatch()` do `jni.c`: método sem handler cujo tipo de retorno declarado é
objeto devolve um objeto vivo daquela classe (e `""` para `String`), nunca
`null`. `BC_JNI_STRICT_NULL=1` volta ao comportamento antigo para diagnóstico.

## STATUS / LOG
- **s0 (2026-08-07) — RECON COMPLETA, VEREDITO VERDE.** APK analisado: Unity 2022.3.39f1,
  **67/72 shaders com variante GLES2**, sem Vulkan, sem PairIP, 10/3528 texturas em ETC2,
  ~97 MiB de textura nativa, Rewired no input. Pasta criada, 3 `.so` + `game.apk` staged,
  scaffold arm64 gerado, STUDY + HANDOFF escritos, memória criada, comando
  `bombchicken-nonstop` gerado. **Device ainda não informado pelo NextOS.**
  **Nenhuma linha de loader escrita — começar em P1** (P1–P3 dão para adiantar no host
  com `nullgl` + `qemu-aarch64`, sem aparelho).

- **s1 (2026-08-07) — JOGÁVEL. `nx-verify` VERDE no aparelho autorizado.**
  Loader escrito sobre a base estrutural do `ports/hitmango` (publicado, mesmo
  trio `libmain -> libunity -> libil2cpp`), renomeado `hgo_`→`bc_` e adaptado.
  - **P1–P3 passaram de primeira.** Módulos mapeados por imagem, `dlopen`/
    `dlsym` interceptados, `RegisterNatives` capturado, `initJni` OK e
    `UnityChoreographer first doFrame delivered` **no frame 3**. O muro
    esperado nunca apareceu.
  - **P4 — o muro real: tela preta com a engine VIVA.** 5000+ draw calls/s,
    áudio tocando, shaders `#version 100` compilando e linkando, e mesmo assim
    o FBO offscreen E o framebuffer padrão saíam pretos. Causa:
    `GamesSignInClient.isAuthenticated()` devolvia `null` pelo shim JNI, o
    GooglePlayGames estourava `NullReferenceException` **dentro de
    `LevelStart.Awake()`** e o resto do Awake — inclusive a transição de cena —
    nunca rodava. *Quem deveria postar a próxima transição* era o próprio
    `Awake` abortado. Conserto genérico no `dispatch()` do `jni.c`: método sem
    handler cujo retorno declarado é objeto devolve um objeto vivo daquela
    classe (`""` para `String`), nunca `null`. `BC_JNI_STRICT_NULL=1` volta ao
    comportamento antigo. Depois disso: **título, menu e gameplay**, 909030 de
    921600 pixels acesos no `glReadPixels` antes do swap.
  - **P5 — ETC2:** as 10 texturas caem no decodificador RGBA por software que
    já vinha na base (`etc2_decode.c`); o Mali reporta só ETC1.
  - **P6 — áudio:** FMOD da própria Unity → thread nativa → SDL/pulseaudio,
    24000 Hz estéreo, pico de PCM medido até 25872 em gameplay.
  - **P7 — controle:** pad do CFW → `KeyEvent`/`MotionEvent` →
    `nativeInjectEvent` (`consumed=1`); o Rewired do jogo responde. Sem cursor,
    sem toque sintético. SELECT+START sai pelo caminho nativo
    (`nativeFocusChanged(false)` + `nativePause`, **status 0, sem crash no
    teardown**). Hooks de InControl por RVA do Hitman GO **desarmados** —
    são offsets de outro `libil2cpp`.
  - **P8 — `nx-verify` VERDE** (`self_test.ok=true`, vídeo=IMAGEM,
    áudio=SOM, processo=VIVO), com o jogo aberto **pelo `run.sh`**, e dump
    direto do `/dev/fb0` mostrando o nível (`verify/fb2.png`).
  - **Números:** ~59,7 fps em menu/hub, **43–46 fps em gameplay** (21,9 ms/quadro
    — não é degrau de vsync, é carga real), RSS ~410 MiB de 916.
  - **Ajustes de qualidade:** pacing pelo tempo que sobra do quadro (o sleep
    fixo de 16,67 ms somado ao trabalho derrubava um jogo de ação pela metade);
    `getenv` do caminho quente lido uma vez; trava de instância única com
    `flock` no **binário** (`/proc/self/exe`), não só no script.

- **s2 (2026-08-07) — PAUSE RESOLVIDO (reclamação do NextOS: "nada despausa").**
  Diagnóstico: o jogo **não pausa por tecla nenhuma** — `KEYCODE_BUTTON_START`
  é ignorado em gameplay e o pause "de verdade" era o `KEYCODE_BACK` (por isso
  o X pausava quando ainda era BACK). O menu de pause (PauseMenu) é touch-only,
  então nada fechava. A tela "com os diamantes e a vida" É o estado pausado
  (HUD de coração+gemas); rodando mostra o botãozinho ‖ no canto.
  - **Conserto (cravado no código):** START chama
    **`LevelStart.PausePressed()`** via IL2CPP (`il2cpp_runtime_invoke` na
    thread do render) — o toggle oficial do jogo, abre E fecha. Estado
    conferido por `LevelStart.get_IsPaused` antes/depois; se não mudou
    (título/intro, onde PausePressed é no-op), o START segue como tecla normal
    (começa o jogo no título). `BC_PAUSE_NATIVE=0` desliga para diagnóstico.
    Provado 3x seguidas no device: `IsPaused 0->1`, `1->0`, `0->1`.
  - `PausePressed` às vezes lança `ArgumentOutOfRangeException` TARDIA
    (analytics/ads) que **não desfaz o toggle** — logada e tolerada.
  - Exports do il2cpp agora resolvidos fora do gate `BC_IL2CPP_HOOKS` (lookup
    é inofensivo; o gate continua valendo para patch de RVA de outro jogo).
  - **Menu principal (▶ Play + engrenagem): o ▶ responde ao A** (KEYCODE_
    BUTTON_A) nativamente; a engrenagem/OPTIONS é touch-only → cursor do port.
  - Ferramenta de bring-up: token `tap:X,Y` no `/tmp/bcgp` (design 1280x720,
    só com `BC_GPVIRT=1`) injeta toque; provou que o toque funciona (abriu o
    OPTIONS pela engrenagem).

- **s3 (2026-08-07) — BOMBA ("pulo") NO A (pedido do NextOS).**
  Descoberta: a ação de botar bomba do jogo responde ao **KEYCODE_BUTTON_B
  (97)** — funcionava desde sempre, mas ficou invisível porque até hoje o B
  do pad mandava BACK (pausava). Provado no device: B → bomba embaixo da
  galinha (screenshot), explosão, gema coletada.
  - **A agora também bota bomba**: quando há um **`Player` vivo**
    (FindObjectOfType da classe global `Player` — só existe em nível), o A
    injeta 96 + 97 juntos; nos menus (sem Player) segue só o 96 (confirmar).
    Gate provado: menu ▶ sem "A -> bomba" no log, nível com.
  - **Game over responde ao A** (reinicia o nível) — validado.
  - Tokens de bring-up novos no `/tmp/bcgp` (só com `BC_GPVIRT=1`):
    `key:N` (keycode arbitrário) e `tap:X,Y` (toque em design 1280x720).
  - Mapa de controles final: dpad anda · **A = bomba / confirmar** ·
    **B = bomba** · **START = pausa/despausa** (via `PausePressed`) ·
    cursor (analógico direito + A) para telas touch (OPTIONS/engrenagem) ·
    SELECT+START sai.

- **s4 (2026-08-07) — TELA PRETA AO TERMINAR A FASE: RESOLVIDA.**
  Relato do NextOS: ao progredir/terminar a fase a tela ficava preta e não saía
  mais dali (só o HUD de coração e diamantes aparecia). **Não era menu faltando
  nem vídeo** — era exceção matando a corrotina do fim de fase.
  - **Diagnóstico (log da própria Unity, `BC_LOGCAT=1`):**
    `Teleporter.TeleportToCheckpoint` → `LevelStart.GoToCheckpoint` →
    `CheckpointUnlocked` → **`UpdateLevelCompletion` estourava
    `IndexOutOfRangeException`**. A corrotina morria no meio do teleporte: a
    fase seguinte nunca carregava, e a `FollowCam` passava a estourar
    `NullReferenceException` **todo quadro** (6300+ no log) porque o alvo dela
    tinha sido destruído e nunca substituído. Daí a tela preta com HUD.
  - **Causa:** `UpdateLevelCompletion` lê a chave `Progress` do PlayerPrefs,
    quebra por vírgula e cada linha em `mundo-grupo-completude` por traço, e
    **indexa a linha sem conferir o formato**. A string termina em vírgula, o
    que deixa uma **linha final VAZIA**; quando o grupo procurado não aparece
    antes dela, `linha[1]` estoura. O save também tinha uma linha `0---0`.
  - **Conserto (cravado):** o corpo de `UpdateLevelCompletion` (RVA
    **`0x107CE0C`**, lido do dump **deste** jogo com Il2CppDumper — nunca
    herdado de outro port) é substituído por `bc_update_level_completion` em
    `input.c`, que faz o mesmo trabalho em C sobre o **nosso** PlayerPrefs
    (`bc_prefs_get_string`/`set_string` novos em `jni.c`): guarda o maior valor
    de completude do grupo e **descarta as linhas malformadas na volta**, de
    modo que o parser do jogo nunca mais as veja. Não tem como estourar.
    `BC_PROGRESS_FIX=0` desliga para diagnóstico.
  - **Prova no device:** token de bring-up **`chk`** (`/tmp/bcgp`, só com
    `BC_GPVIRT=1`) invoca `LevelStart.GoToCheckpoint(true)` — a MESMA chamada da
    corrotina do Teleporter. Antes: exceção + preto para sempre. Depois:
    `[bc/save] fim de fase: grupo 0 -> 1`, `GoToCheckpoint(1) -> ok`,
    `Loading group R1G1`, **sala do checkpoint renderizada** (estátua, portas,
    contador de gemas) e **0 exceções em toda a sessão**.
  - Sonda `cam` mostra as referências da `FollowCam` ao vivo (target/camera/
    level/Player) — foi ela que separou "câmera órfã" de "nível não carregou".

- **s5 (2026-08-09) — MIGRAÇÃO UNIVERSAL HOST-ONLY, SEM INVENTAR SUPORTE.**
  A base positiva continua sendo o gameplay físico comprovado acima; nenhum
  port WIP foi usado como receita. A integração pré-runtime foi migrada para
  `nxport.json` schema v2, launcher único gerado pelo nxbootstrap 0.5.1,
  receipt estático `nxdeployment.json` e quirks exatos declarados. O antigo
  `run.sh`, os helpers de mapeamento locais, branches por CFW e a varredura
  global de `/proc` foram removidos. A trava do próprio executável permanece
  em `/proc/self/exe` e não enumera/mata nenhum processo alheio.
  - **BYO exato:** `extractor.json` aceita somente Bomb Chicken v44/build 45,
    pacote `com.nitrome.bombchicken`, ABI arm64-v8a, hashes exatos das três
    bibliotecas e fingerprint completo da árvore de 11 assets. NXExtract 1.2.6
    foi vendorizado sem patches e tem hashes registrados.
  - **Build público:** imagem Debian Buster offline e pinada; sysroot do alvo
    só para headers, saída AArch64 PIE com teto observado `GLIBC_2.27`, sem
    RPATH/RUNPATH nem caminho privado. O gate recompila duas vezes e compara os
    bytes, além de auditar todos os ELFs do pacote.
  - **Runtime preservado:** a ordem construtores/`JNI_OnLoad`/
    `NativeLoader.load`/surface/focus/resume/render/focus-lost/pause não mudou.
    As correções v44 (cursor, pause, JNI Play Games, alpha-one, progresso e
    stencil) agora nascem desligadas e só ligam pelo token exato do manifesto.
    A seleção SDL-GLES2→raw-EGL passou a depender do sucesso real da abertura,
    não do nome `mali`/CFW.
  - **Upgrade overlay 1.1.1:** `migrate-legacy-overlay.sh` roda somente como
    `prepare_script` depois da validação do launcher/bootstrap e põe em
    quarentena 0644 apenas os hashes exatos de `run.sh`, `es_map.sh`,
    `es2sdl.awk` e do loader `bombchicken` 1.0. Ausência é idempotente;
    symlink/tipo/hash desconhecido fica intacto e bloqueia o launch. O gate
    semeia a árvore antiga, preserva separadamente `assets`, `lib`, `home` e
    `gamedata`, e prova zero mutação numa raiz inativa. Em FAT/exFAT, o modo
    0700 do diretório é best-effort; se o arquivo exato continuar executável
    após chmod, ele é removido com segurança em vez de permanecer chamável.
  - **Limite honesto:** o framework universal controla contrato, extração e
    pré-runtime; o adaptador Unity compilado continua específico deste jogo.
    A migração 1.1 ainda não foi executada em aparelho e não é prova de suporte
    multi-CFW. Os dados locais ignorados (`gamedata/`, `stage/`, libs e saves)
    foram apenas lidos/hashados para formular a receita e não foram alterados.

- **s6 (2026-08-13) — NEW GAME PRETO APENAS EM SAVE LIMPO: CAUSA E
  REGRESSÃO CRAVADAS.** O render continuava vivo e o mesmo build funcionava
  com save existente, portanto não era NXExtract, EGL nem uma falha genérica
  do framework. Era ABI incorreta no adapter do Bomb Chicken.
  - O dump deste jogo possui dois wrappers distintos:
    `0x219225C = PlayerPrefs.GetString(string key, string defaultValue)` e
    `0x21922A0 = PlayerPrefs.GetString(string key)`.
  - A correção 1.1.6 substituía `0x21922A0`, mas declarava o hook com três
    ponteiros, como se aquele RVA fosse o overload com `defaultValue`. No ABI
    IL2CPP de método estático, os argumentos C# vêm primeiro e o
    `MethodInfo*` oculto vem por último. Assim, numa chave ausente, o hook
    devolvia o próprio `MethodInfo*` como se fosse `System.String`. Saves já
    preenchidos escondiam o erro; `New Game` em instalação limpa o ativava.
  - A 1.1.7 separa os dois RVAs e assinaturas. O overload com default preserva
    exatamente o objeto default; o overload de um argumento fabrica uma
    string managed vazia quando a chave ainda não existe. A lógica isolada em
    `playerprefs_fix.c` é exercitada por GCC e Clang com ASan/UBSan, incluindo
    chave inexistente, ponteiro-sentinela de `MethodInfo*`, valor persistido,
    URL decode e chave histórica com espaço.
  - **Regra permanente:** nunca escolher um patch IL2CPP apenas pelo nome do
    método. Registrar RVA + assinatura completa de cada overload, incluindo o
    `MethodInfo*` oculto, e obrigar teste do estado inicial/ausente — não apenas
    do save já populado. Esta é uma quirk opt-in do adapter v44; não promover
    seu comportamento a default do framework.
  - **Prova física 1.1.7:** no aparelho AArch64 Mali-G31/dArkOSRE autorizado, o
    mesmo loader abriu primeiro um save existente até a fase 3. O save foi
    então movido para backup recuperável e a abertura limpa registrou
    `STARTED 1ST`, `STARTING GROUP:0`, criou um novo save, terminou o tutorial,
    atualizou `grupo 0 -> 1` e carregou `R1G1`/fase 2 sem tela preta,
    `Exception` ou `NullReference`. SELECT+START também saiu pelo lifecycle
    normal. Uma tentativa anterior com música/input travados tinha duas
    instâncias concorrentes; depois de zerar os processos e abrir exatamente
    uma instância, a saída foi repetida com sucesso. Nunca lançar a segunda.

## O QUE FALTA

1. [x] nxbootstrap 0.6.8 autocontido, sem `stat` externo, launcher único;
   NXRelease 0.2.6 e manifesto schema v2 fixados.
2. [x] Gate host 1.1.7: GCC+Clang ASan/UBSan para os dois overloads, inclusive
   save inexistente; dois loaders idênticos (`77c37956...`, `GLIBC_2.27`),
   extração exata em cópia temporária e auditoria dos dois ELFs do pacote.
3. [x] Runtime 1.1.7 validado fisicamente com save existente, save limpo,
   transição para fase 2, novo save e saída. A evidência é restrita às famílias
   já nomeadas; não ampliar para “todos os devices”.
4. Opcional e posterior à paridade: investigar os 43–46 fps de gameplay por
   orçamento de textura/`glGet*`, sem atalhos de shader ou lifecycle.
