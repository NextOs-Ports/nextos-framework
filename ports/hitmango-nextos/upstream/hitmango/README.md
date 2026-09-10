# Hitman GO 1.18.1 — native AArch64 compatibility port

[![Release](https://img.shields.io/github/v/release/NextOs-Ports/hitmango-nextos)](https://github.com/NextOs-Ports/hitmango-nextos/releases/latest)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

**Idioma / Language:** [Português](#português) · [English](#english)

This repository and its public ZIP contain only the open compatibility loader.
They do not contain the APK, Unity libraries, artwork, music, saves or any
other proprietary Hitman GO data.

[Pacote mais recente / Latest package](https://github.com/NextOs-Ports/hitmango-nextos/releases/latest)
· [Comunidade / Community](https://discord.gg/DHfY62eDNN)

| | | |
|---|---|---|
| ![Title on ArkOS](docs/images/title-r36t.png) | ![Chapter selection](docs/images/chapters-elite.png) | ![Gameplay](docs/images/gameplay-r36t.png) |

## Português

### Visão geral

Este port executa o Hitman GO Android 1.18.1 (Unity 2022.3.67f2/IL2CPP)
diretamente em Linux AArch64. Ele carrega as bibliotecas originais do jogo por
uma camada Bionic/glibc e reproduz o ciclo Android na ordem original; não emula
Android e não reimplementa as regras do jogo.

A versão 1.2.2 preserva a correção de arrasto da 1.2.1 e corrige a abertura no
ArkOS/KMSDRM quando o SDL encontra o backend, mas o provider EGL versionado do
firmware não cria a janela GLES2. O adapter tenta primeiro a configuração
normal e só depois da falha reinicializa o vídeo uma vez com os nomes portáveis
`libEGL.so` e `libGLESv2.so`; qualquer provider definido pelo usuário continua
intocado.

O controle padrão permanece exatamente no contrato publicado da v1.2.0:
analógico esquerdo move a seta, A inicia e segura o toque para clicar/arrastar,
e soltar A publica o `UP`; analógico direito e D-pad movimentam o tabuleiro.

O launcher atual é gerado pelo nxbootstrap 0.6.14. A primeira instalação usa o
NXExtract 1.2.9 com interface gráfica preservada; todas as aberturas mostram o handoff
obrigatório do NXSplash 0.1.2 antes do jogo. Não existe `run.sh` intermediário
no ZIP público.

### Arquitetura e problemas resolvidos

- segue construtores, `JNI_OnLoad`, `initJni`, criação/alteração da superfície,
  foco, resume, render, perda de foco e pause na ordem nativa;
- mapeia `libmain.so`, `libunity.so`, `libil2cpp.so` e Firebase pelo loader ELF
  Bionic/glibc do próprio port;
- mantém Unity no controle de cenas, regras, movimento, animação e saves;
- converte somente texturas ETC2/EAC que realmente precisam de fallback;
- preserva o alpha opaco do framebuffer em Mali/Amlogic;
- recupera o provider EGL/GLES em KMSDRM somente após falha comprovada, sem
  alterar o caminho EGL/fbdev do Mali-450;
- entrega o áudio FMOD original ao SDL sem forçar driver do firmware;
- usa um único dono para toque e timestamps Android estáveis;
- carrega o mapeamento SDL publicado pelo PortMaster/CFW e mantém fallback de
  joystick cru para controles ausentes da base SDL;
- instala dados BYO de forma transacional e aceita APKs reempacotados quando o
  package ID e o conteúdo crítico da build 1.18.1 são idênticos.

### Controles

| Controle | Ação |
|---|---|
| D-pad / analógico direito | Move o Agente 47 ao nó vizinho |
| Analógico esquerdo | Move a seta do cursor |
| A / Cross | Clica; segure A e mova o analógico esquerdo para arrastar |
| B / Circle | Voltar / Android Back |
| X / Square | Abrir dica durante a fase |
| Y / Triangle | Reiniciar a fase |
| Start | Abrir ou fechar objetivos/pause |
| Select + Start | Sair pelo fluxo limpo de pause/save |

O layout alternativo direita/R3 continua disponível somente por opt-in: use
`HGO_SWAP_STICKS=0` e `HGO_CLICK_A=0` para cursor no analógico direito, clique
no R3 e movimento no analógico esquerdo.

### Dados do dono e instalação

Use sua cópia legal ARM64 do Hitman GO 1.18.1, package
`com.squareenixmontreal.hitmango`. Coloque o APK/APKM/APKS/XAPK em
`hitmango/gamedata/` e abra o port. Veja a identidade exata, compatibilidade de
APKs alternativos e o layout em [INSTALLATION.md](INSTALLATION.md).

O ZIP público tem esta estrutura:

```text
Hitman GO.sh
hitmango/
  bin/aarch64/hitmango-nextos
  nxport.json
  nxsplash-nextos
  nxextract/
  extractor.json
  port-env.sh
  gamedata/
```

Após a instalação, NXExtract cria `assets/` e `lib/`; saves e preferências
ficam em `hitmango/home/` e sobrevivem à atualização do port.

### Build, teste e pacote

```sh
make                 # build de desenvolvimento
make universal       # build público AArch64, GLIBC <= 2.30
make package         # ZIP BYO reproduzível e .sha256
./tests/run-host-tests.sh
```

O executável público se chama `hitmango-nextos`. O gate audita arquitetura,
dependências, limite de glibc, RPATH/RUNPATH, receita do NXExtract, arquivos do
framework, ausência do comando externo `stat`, falha pré-runtime e o ZIP final
desempacotado.

### Evidência de suporte

O baseline do jogo foi validado fisicamente em ArkOS/Mali-G31 e NextOS
Elite/Mali-450. A 1.2.2 mantém o caminho Mali-450 e acrescenta a recuperação
observada como necessária no KMSDRM; a correção de arrasto da 1.2.1 continua
voltada ao relato do muOS/RG40XX-H. Suporte a uma família só é promovido quando
o mesmo ZIP e SHA são exercitados fisicamente nela.

### Mapa de fontes e licenças

- `src/main.c` — ciclo Unity/Android e teardown;
- `src/nx_elf.*`, `src/bionic.c`, `src/pthread_bridge.c` — ABI Android;
- `src/jni.c`, `src/motion_stream.*` — JNI e eventos imutáveis;
- `src/input.c`, `src/input_layout.*`, `src/touch_arbiter.*` — controles,
  cursor, swipe e propriedade exclusiva do toque;
- `src/egl*`, `src/etc2_decode.*` — EGL/GLES, cursor e texturas;
- `src/audio.c` — FMOD/AudioTrack para SDL;
- `project/` — entrada do nxgenerator;
- `package/` — gates e empacotamento.

O loader é GPL-3.0. NXExtract e NXSplash são MIT. SDL2, EGL, GLES, zlib e
bibliotecas do firmware mantêm suas próprias licenças. Todo conteúdo do jogo
continua proprietário e separado deste projeto. Veja [NOTICE.md](NOTICE.md).

## English

### Overview

This port runs the Android 1.18.1 release of Hitman GO (Unity
2022.3.67f2/IL2CPP) directly on AArch64 Linux. It maps the original game
libraries through a Bionic/glibc layer and follows Android's native lifecycle;
it neither emulates Android nor reimplements game rules.

Version 1.2.2 preserves the v1.2.1 drag fix and restores startup on ArkOS/KMSDRM
when SDL finds the backend but the firmware's versioned EGL provider cannot
create a GLES2 window. The adapter tries the normal configuration first and,
only after failure, reinitializes video once with the portable `libEGL.so` and
`libGLESv2.so` names. Any user-selected provider remains untouched.

The default controls remain exactly on the published v1.2.0 contract: the left
stick moves the arrow, A starts and holds touch for click/drag, releasing A
publishes `UP`, and the right stick plus D-pad move across the board.

The public launcher is generated by nxbootstrap 0.6.14. First installation uses
the preserved graphical NXExtract 1.2.9 UI, and every launch shows the mandatory NXSplash
0.1.2 handoff before the game. The public ZIP has no intermediate `run.sh`.

### Architecture and solved gaps

- preserves constructors, `JNI_OnLoad`, `initJni`, surface, focus, resume,
  render, focus-loss and pause ordering;
- maps the original Unity/IL2CPP/Firebase DSOs with the port's ELF bridge;
- leaves scenes, rules, movement, animation and saves under original IL2CPP;
- decodes only ETC2/EAC formats that require a software fallback;
- keeps final framebuffer alpha opaque on Mali/Amlogic;
- recovers the EGL/GLES provider only after a proven KMSDRM failure, without
  changing the Mali-450 EGL/fbdev path;
- carries original FMOD output into SDL without forcing a firmware driver;
- uses stable Android gesture timestamps and one touch owner;
- consumes PortMaster/CFW SDL mappings with a raw-joystick fallback;
- installs owner data transactionally and accepts repackaged APKs only when
  their package ID and critical 1.18.1 payload are identical.

### Controls

| Control | Action |
|---|---|
| D-pad / right stick | Move Agent 47 to an adjacent node |
| Left stick | Move the arrow cursor |
| A / Cross | Click; hold A and move the left stick to drag |
| B / Circle | Back / Android Back |
| X / Square | Open the in-level hint |
| Y / Triangle | Restart the level |
| Start | Open or close objectives/pause |
| Select + Start | Exit through the clean pause/save path |

The right-stick/R3 alternative remains opt-in only: set `HGO_SWAP_STICKS=0`
and `HGO_CLICK_A=0` for right-stick cursor, R3 click and left-stick movement.

### Owner data and installation

Use your legitimate ARM64 Hitman GO 1.18.1 copy, package
`com.squareenixmontreal.hitmango`. Put the APK/APKM/APKS/XAPK in
`hitmango/gamedata/` and launch the port. Exact identity, alternate-APK rules
and file layout are in [INSTALLATION.md](INSTALLATION.md).

The public ZIP contains:

```text
Hitman GO.sh
hitmango/
  bin/aarch64/hitmango-nextos
  nxport.json
  nxsplash-nextos
  nxextract/
  extractor.json
  port-env.sh
  gamedata/
```

NXExtract creates `assets/` and `lib/`; saves/preferences live in
`hitmango/home/` and survive port updates.

### Build, test and package

```sh
make
make universal
make package
./tests/run-host-tests.sh
```

The public executable is `hitmango-nextos`. Release gates audit architecture,
dependencies, the glibc ceiling, RPATH/RUNPATH, NXExtract identity, framework
pins, absence of external `stat`, pre-runtime failure evidence and the unpacked
final ZIP.

### Support evidence

The game baseline was physically validated on ArkOS/Mali-G31 and NextOS
Elite/Mali-450. Version 1.2.2 keeps the Mali-450 path and adds the recovery
observed as necessary on KMSDRM; the v1.2.1 drag fix continues to target the
muOS/RG40XX-H report. A device family is promoted only after the exact same
ZIP/SHA has been exercised on it.

### Source map and licenses

- `src/main.c` — Unity/Android lifecycle and teardown;
- `src/nx_elf.*`, `src/bionic.c`, `src/pthread_bridge.c` — Android ABI;
- `src/jni.c`, `src/motion_stream.*` — JNI and immutable input events;
- `src/input.c`, `src/input_layout.*`, `src/touch_arbiter.*` — controls,
  cursor, swipe and exclusive touch ownership;
- `src/egl*`, `src/etc2_decode.*` — EGL/GLES, cursor and textures;
- `src/audio.c` — FMOD/AudioTrack to SDL;
- `project/` — nxgenerator input;
- `package/` — release gates and packaging.

The loader is GPL-3.0. NXExtract and NXSplash are MIT. SDL2, EGL, GLES, zlib
and firmware libraries retain their own licenses. All game content remains
proprietary and separate. See [NOTICE.md](NOTICE.md).
