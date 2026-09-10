# Gunbrick Reloaded — NextOS / PortMaster

**Language / Idioma:** [English](#english) · [Português](#português)

Independent AArch64 Linux compatibility port for the Android release of
**Gunbrick Reloaded** (Unity 2022.3 IL2CPP). The release is BYO-data: it
contains the open-source loader and integration only, never the APK, Android
libraries, assets, videos or saves.

## English

### Status

Version 0.2.1 was validated end to end with the supported v10 ARM64 payload on
three supported hardware/firmware families:

- Amlogic Mali-450 using the native `mali`/framebuffer video path;
- X5M using KMSDRM and a Mali-G310;
- Ark using KMSDRM and a Mali-G31.

On every target, NXExtract displayed its clean setup UI, transactionally
installed the owner payload, and the game reached rendered gameplay with
audible SDL audio and a working controller. The tested APK was user supplied
and is not redistributed or claimed as an original store artifact; the exact
payload files are independently pinned by size, SHA-256 and asset-tree
fingerprint. Version 0.2.1 also validated the original raw two-axis Android
joystick path on the Ark/Mali-G31 target.

Version 0.2.5 is the physically validated transition-memory and input release.
Version 0.2.4 bounded fake-JNI and FMOD ownership and stopped the observed
post-skip audio growth, but a physical Ark run still reached the same
bonus/phase-3 transition peak and the kernel killed it after both RAM and zram
were exhausted. The game starts its next asynchronous scene load without
waiting for `Resources.UnloadUnusedAssets()` to finish, while this native Linux
host had no Android Activity to deliver Unity's low-memory signal.

The adapter now restores Android's reference lifecycle with refcounted local
and global ownership, one root local frame around each host-to-guest callback,
safe object-array elements and a locked global reference for the
Choreographer callback shared with its thread. FMOD processing has the same
bounded scope. The stable semaphore bridge from 0.2.3 is unchanged, including
its intentionally non-destructive guest `sem_destroy`; the earlier destructive
variant that caused a black screen is still rejected by the release gate.
Host regressions exercise 4,096 simultaneous guest semaphores, 100,000 repeated
post/wait operations and 100,000 JNI local scopes returning to zero live local
objects. Under measured pressure, the Unity main thread now sends the native
Android `nativeLowMemory` callback before the next render and trims the host
heap with a cooldown. Read-only guest code is mapped from the original files,
so roughly 56 MiB of clean executable pages can be reclaimed instead of
occupying anonymous memory. A monitored Ark/Mali-G31 run of 0.2.5 crossed the
bonus/phase-3 transition that previously exhausted RAM and zram, remained
alive after Unity received the pressure callback, and preserved both the raw
two-axis left stick in 3D bonus gameplay and the independent D-pad path. The
same run confirmed video, audio, controls and continued gameplay end to end.

### Architecture

`Gunbrick.sh` is a generated, self-contained nxbootstrap 0.6.3 launcher. It
locates PortMaster, runs NXExtract before touching guest data, verifies the
manifest-owned runtime files, isolates game library paths from host setup,
publishes the nxcompat contract and returns cleanly to the frontend.

The adapter preserves the Android lifecycle:

1. map `libmain.so`, `libunity.so` and `libil2cpp.so` in dependency order;
2. relocate locally, resolve imports strictly and finalize guest mappings;
3. execute initializer arrays once and require JNI 1.6 from `JNI_OnLoad`;
4. let Unity `NativeLoader` establish readiness, then call `initJni`;
5. deliver surface creation/change, focus and resume before rendering;
6. deliver focus loss and pause before input/audio shutdown.

The framework is capability driven. SDL GLES2 is preferred; raw EGL is used
only after a real SDL window failure. nxcompat publishes measured EGL, GLES,
drawable, audio and controller receipts before `READY`. There are no device-IP
branches, forced firmware names, global texture-wrap changes, alpha hacks or
fixed-RVA gameplay patches.

### Controls

| Input | Action |
| --- | --- |
| D-pad / left stick | Move and orient the Gunbrick |
| Face buttons | Native in-game actions and menu confirmation/back |
| Right stick + R3 | Polished menu pointer and click where touch UI requires it |
| `SELECT + START` | Graceful exit |

The loader forwards the standard Android controller buttons, both raw stick
axes, triggers and D-pad through Unity's native input API. The left stick
deliberately keeps both simultaneous axes: the 3D bonus mode builds its
movement from the four analog quadrants, so a host-side cardinal snap would
make that mode ignore the stick. The D-pad continues through its independent
Android key/hat path. The optional directional-swipe compatibility quirk
remains disabled for this payload; no duplicate direction path is installed.

### Owner data

Place a compatible Gunbrick Reloaded v10 ARM64 APK in `gunbrick/gamedata/` and
launch the port. NXExtract 1.2.6 checks package identity
`com.nitrome.Gunbrick`, selects `arm64-v8a`, verifies exactly 13 asset files
and all three Android ELFs, then publishes `assets/` and `lib/` atomically. A
wrong, partial or different-version payload is rejected without replacing a
working installation. The source APK is preserved.

### Build and verification

```bash
./build_universal.sh
./tests/run-host.sh
./package/build-package.sh
```

The public binary is built in a pinned offline Debian Buster environment and
must stay at or below GLIBC 2.30 (currently GLIBC 2.27). Gates reject RPATH,
RUNPATH, RWE load segments, unexpected dependencies, missing framework
symbols, owner data and private machine paths. NXRelease creates a
deterministic allowlisted ZIP, audits every included Linux ELF and reopens the
archive for verification.

### Source map and licenses

- `src/main.c`: Android/Unity lifecycle and graceful termination;
- `src/nx_elf.c`: strict guest ELF loading;
- `src/jni.c`, `src/jni_refs.c`, `src/android.c`, `src/bionic.c`: Android ABI
  bridge and bounded JNI ownership;
- `src/pthread_bridge.c`: stable Bionic synchronization-object translation;
- `src/egl_sdl.c`, `src/egl.c`: SDL GLES2 and raw-EGL fallback;
- `src/input.c`, `src/audio.c`: controller and SDL audio;
- `src/framework_bridge.c`: nxcompat runtime receipts;
- `extractor.json`, `nxextract/`: transactional BYO-data installation.

Project code is GPL-3.0-only (`LICENSE`). NXExtract is MIT
(`licenses/NXExtract-MIT.txt`). Gunbrick Reloaded and its data remain property
of Nitrome or their respective rightsholders and are not distributed. See
`NOTICE.md`.

## Português

### Estado

Esta é uma camada de compatibilidade Linux AArch64 independente para a versão
Android de **Gunbrick Reloaded**. A versão 0.2.1 foi validada do começo ao fim
com o payload ARM64 v10 compatível em três famílias de hardware/firmware: Mali-450 por
`mali`/framebuffer, X5M por KMSDRM/Mali-G310 e Ark por KMSDRM/Mali-G31.

Nos três casos a interface limpa do NXExtract apareceu, os dados foram
instalados transacionalmente e o jogo chegou ao gameplay renderizado com áudio
audível e controle reconhecido. O APK usado no teste foi fornecido pelo usuário
e não é redistribuído nem apresentado como artefato original da loja; os
arquivos úteis são validados por tamanho, SHA-256 e fingerprint da árvore. A
v0.2.1 também validou no Ark o caminho Android original com os dois eixos crus
do analógico.

A versão 0.2.5 é a release de memória de transição e input validada fisicamente.
A 0.2.4 limitou o ownership do JNI falso e do FMOD e eliminou o crescimento de
áudio observado depois do skip, mas um teste físico no Ark ainda chegou ao
mesmo pico da transição bônus/fase 3 e o kernel o encerrou após esgotar RAM e
zram. O jogo começa a carregar a cena seguinte sem esperar o
`Resources.UnloadUnusedAssets()` assíncrono terminar, enquanto este host Linux
nativo não possuía uma Activity Android para avisar a Unity da pouca memória.

O adapter agora reproduz o ciclo Android com ownership local/global por
refcount, um frame local em cada entrada host→guest, elementos de arrays com
ownership correto e uma referência global protegida para o callback do
Choreographer compartilhado com outra thread. O processamento FMOD também é
limitado por escopo. A ponte estável de semáforos da 0.2.3 não mudou, inclusive
o `sem_destroy` propositalmente não destrutivo; a variante destrutiva que causou
tela preta continua rejeitada pelo gate. Os testes cobrem 4.096 semáforos,
100.000 operações post/wait e 100.000 escopos JNI voltando a zero objetos locais
vivos. Sob pressão medida, a UnityMain agora entrega `nativeLowMemory` antes do
próximo render e libera o heap do host com cooldown. O texto executável do
guest fica respaldado pelos arquivos originais, permitindo ao kernel reclamar
cerca de 56 MiB de páginas limpas. Um teste monitorado da 0.2.5 no
Ark/Mali-G31 atravessou a transição bônus/fase 3 que antes esgotava RAM e zram,
continuou vivo depois do callback de pressão e preservou tanto os dois eixos
crus do analógico na fase bônus 3D quanto o caminho independente do D-pad. O
mesmo teste confirmou vídeo, áudio, controles e continuidade do gameplay.

### Arquitetura e framework

`Gunbrick.sh` é um launcher enxuto de uso e autossuficiente, gerado pelo
nxbootstrap 0.6.3. Ele prepara PortMaster e o extrator, valida o payload exigido,
isola as bibliotecas do jogo e devolve a tela ao frontend na saída.

O adapter conserva a sequência Android real: bibliotecas, relocação e imports,
initializers, `JNI_OnLoad`, `NativeLoader`, `initJni`, surface, foco, resume e
render. Na saída executa perda de foco e pause. SDL GLES2 é a primeira opção e
EGL bruto só entra depois de falha real. O nxcompat exige evidência medida de
vídeo, áudio e input; não escolhe solução por IP ou nome de aparelho.

### Controles e dados

| Entrada | Ação |
| --- | --- |
| D-pad / analógico esquerdo | Mover e orientar o Gunbrick |
| Botões frontais | Ações nativas e confirmar/voltar nos menus |
| Analógico direito + R3 | Mover a seta e clicar quando a interface touch exigir |
| `SELECT + START` | Saída limpa |

Botões, D-pad, os dois eixos crus dos analógicos e gatilhos seguem a API Android
nativa do Unity. A fase bônus 3D depende dos quadrantes formados por X e Y ao
mesmo tempo, portanto não há snap cardinal no host; o D-pad preserva seu caminho
Android independente. `SELECT + START` pede saída limpa. O ZIP é BYO e não
contém APK, bibliotecas Android, assets, vídeos nem saves.

Coloque um APK ARM64 compatível do Gunbrick Reloaded v10 em
`gunbrick/gamedata/` e abra o port. O NXExtract 1.2.6 verifica o pacote
`com.nitrome.Gunbrick`, exatamente 13 arquivos de assets e as três bibliotecas,
depois publica `assets/` e `lib/` de forma atômica. Payload errado ou incompleto
é recusado sem destruir uma instalação válida; o APK de origem permanece no
local.

### Build, fontes e licenças

Use os três comandos da seção inglesa. O ELF público exige no máximo GLIBC
2.30 (atualmente 2.27), não aceita RPATH/RWX e o ZIP determinístico é reaberto
e auditado. O mapa de fontes também está acima.

O código do projeto é GPL-3.0-only e o NXExtract é MIT. Gunbrick Reloaded e
todos os dados do jogo continuam proprietários da Nitrome ou de seus titulares
e não fazem parte desta distribuição. Consulte `NOTICE.md`.
