# Final Fantasy IV 3D Remake — Android v2.0.4 (aarch64 so-loader, GLES1)

**Language / Idioma:** [English](#english) · [Português](#português)

This directory contains the loader code only. The proprietary Square Enix engine and
game data must be supplied from a legally owned APK.

Esta pasta contém apenas o código do loader. A engine proprietária e os dados da Square
Enix devem ser fornecidos a partir de um APK adquirido legalmente.

---

## English

Status: **playable on two proven device families**. The tested flow reaches the world
map, battles, field menu and inventory with video, 32 kHz stereo audio, save/reload and
a physical controller. Field rendering holds 30 FPS and battles use the game's native
15 FPS target.

Proven on real hardware, 21/08/2026:

| Family | Renderer | Frame proof |
|---|---|---|
| NextOS Retro/Elite, Mali-450 Utgard | `Mali-450 MP`, OpenGL ES-CM 1.1 | 99.8% non-black, verdict OK, 1280x720 |
| ArkOS/dArkOS R36S, Mali-G31 Bifrost | `Mali-G31`, OpenGL ES-CM 1.1 v1.r13p0 | 99.8% non-black, verdict OK, 640x480 |

Both runs install from the APK through NXExtract to the `NXE0000` terminal marker,
find a system font, draw, and close on `SELECT`+`START`. Families that were not
tested on real hardware are listed honestly in `../../docs` and in the release notes;
the port fails closed with the missing symbol named rather than opening a black screen.

### Architecture

The Android game uses Square Enix's Matrix/“cuore” engine, inherited from the Nintendo
DS version, as an arm64 Android shared object with a GLES1 fixed-function renderer.
The Linux executable:

1. opens and decodes the encrypted ARC1 OBB (8,770 entries, seed `0x19000000`);
2. loads and relocates the original `libff4.so` without emulating the ARM CPU;
3. implements the Java/JNI callbacks expected by `MainActivity`;
4. bridges OpenSL ES output to SDL2 audio;
5. preserves the Android lifecycle order: `resume`, then one `touch` call before every
   `render`, followed by the port overlays and one buffer swap.

### Fixed problems

| Problem | Resolution |
|---|---|
| encrypted game data | native ARC1 VFS with the APK's exact decode algorithm |
| unreadable menu text | UTF-16LE string repack plus GLES1 font rendering |
| repeated/stuck input | restored Android's mandatory `touch(0,0,...)` frame pump |
| menu moved but A/B did nothing | removed invasive `CPad` hooks and let the engine calculate edge/repeat/release |
| Start paused but could not resume | context-locked Start route using the game's own `assignBackButton` mode |
| touch-only UI fallback | polished right-stick arrow with radial deadzone, progressive response and R3 click |
| cursor damaged the next frame | complete save/restore of touched GLES1 state, arrays and VBO binding |
| Android OpenSL unavailable | OpenSL ES compatibility layer backed by SDL2, 32 kHz stereo |

The cursor is a fallback, not the primary control path. It exists only while the engine
marks a UI context through `assignBackButton`; it is hidden during gameplay.

### Controls

| Control | Action |
|---|---|
| D-pad / left stick | movement and native menu navigation |
| A | confirm / interact |
| B | cancel / return |
| X, Y, L1, R1 | original game actions |
| Start | field menu; pause/unpause or return according to the current game context |
| Right stick | fallback pointer in touch-oriented UI only |
| R3 | click the fallback pointer in UI; native stick button outside UI |
| Select + Start | exit the port |

### Required game data

From the arm64 APK, the runtime needs:

```text
ff4/
├── ff4
├── libff4.so                 # lib/arm64-v8a/libff4.so
├── data/
│   └── main.obb              # assets/main.obb
└── saves/                    # created/used locally; never ship a personal save
```

`res/raw/opening.mp4` is a Java MediaPlayer splash and is not consumed by this native
runtime. The playable opening sequence is rendered by the engine from the OBB.

### Build and run

```bash
cd ports/ff4
./build.sh
```

The build produces the historical NextOS executable name `ff4`. The PortMaster launcher
is `Final Fantasy IV 3D Remake.sh`; it obtains the device-specific SDL controller mapping
and runs the game in the foreground without managing EmulationStation itself.

### Source map

- `src/main.c` — Android lifecycle, SDL controller mapping, native touch pump and cursor.
- `src/jni_shim.c` — fake JNI environment and `MainActivity` callbacks.
- `src/game.c` — ARC1 VFS, timing, saves, textures, fonts and Android service shims.
- `src/audio.c` — OpenSL ES to SDL2 audio bridge.
- `src/imports.c` — bionic/libc/GLES/OpenSL symbol surface.
- `src/so_util.c` — arm64 ELF loader and relocations.
- `src/vkbd.c` — contextual virtual keyboard for name-entry screens.
- `STUDY.md` — internal APK/engine analysis and the menu-input root cause.

### Licenses

The loader code follows the repository's GPL-3.0 license. `stb_image` and
`stb_truetype` retain their upstream public-domain/MIT terms. SDL2 and zlib are system
dependencies under their respective licenses. Final Fantasy IV, `libff4.so`, the OBB,
artwork, music and all other game content remain property of Square Enix and are not
licensed or distributed by this source repository.

---

## Português

Estado: **jogável no NextOS arm64 / Mali-450**. O fluxo validado chega ao mapa-múndi,
batalhas, menu de campo e inventário com vídeo, áudio estéreo a 32 kHz, save/reload e
controle físico. O campo mantém 30 FPS e as batalhas usam o alvo nativo de 15 FPS do
jogo.

Provado em aparelho real em 21/08/2026 em duas famílias: NextOS Retro/Elite no
Mali-450 Utgard (`Mali-450 MP`, prova de imagem 99,8% a 1280x720) e ArkOS/dArkOS
no R36S com Mali-G31 Bifrost (`Mali-G31`, 99,8% a 640x480). Nos dois casos a
instalação vai do APK até o marcador `NXE0000`, a fonte do sistema é encontrada,
a imagem é medida e `SELECT`+`START` fecha o jogo.

### Arquitetura

O jogo Android usa a engine Matrix/“cuore” da Square Enix, herdada da versão Nintendo
DS, em uma biblioteca arm64 Android com renderizador fixed-function GLES1. O executável
Linux:

1. abre e decodifica o OBB ARC1 criptografado (8.770 entradas, seed `0x19000000`);
2. carrega e realoca a `libff4.so` original sem emular a CPU ARM;
3. implementa os callbacks Java/JNI esperados pela `MainActivity`;
4. liga a saída OpenSL ES ao áudio SDL2;
5. preserva a ordem do Android: `resume`, uma chamada `touch` antes de cada `render`,
   overlays do port e uma única troca de buffers.

### Problemas resolvidos

| Problema | Solução |
|---|---|
| dados criptografados | VFS ARC1 nativo com o algoritmo exato do APK |
| texto ilegível no menu | repack UTF-16LE e renderização de fontes em GLES1 |
| input repetido ou preso | restauração do pump obrigatório `touch(0,0,...)` do Android |
| menu navegava, mas A/B não respondiam | remoção dos hooks invasivos de `CPad`; edge/repeat/release voltaram à engine |
| Start pausava e não despausava | rota contextual travada no press, seguindo `assignBackButton` do jogo |
| telas originalmente touch | seta de fallback no analógico direito, deadzone radial, curva progressiva e clique no R3 |
| cursor estragava o quadro seguinte | restauração completa dos estados, arrays e VBO tocados no GLES1 |
| ausência de OpenSL no Linux | camada OpenSL ES sobre SDL2, estéreo a 32 kHz |

O cursor é apenas uma contingência. Ele aparece somente quando a engine marca uma UI
por `assignBackButton`; durante o gameplay permanece oculto e não substitui o controle
nativo.

### Controles

| Controle | Ação |
|---|---|
| D-pad / analógico esquerdo | movimento e navegação nativa |
| A | confirmar / interagir |
| B | cancelar / voltar |
| X, Y, L1, R1 | ações originais do jogo |
| Start | menu no campo; pausa/despausa ou volta conforme o contexto atual |
| Analógico direito | seta de fallback somente nas interfaces touch |
| R3 | clica a seta na UI; botão de stick nativo fora dela |
| Select + Start | encerra o port |

### Dados necessários

O runtime usa `lib/arm64-v8a/libff4.so` e `assets/main.obb` do APK arm64 adquirido
legalmente. Saves pessoais são locais e nunca devem entrar em um pacote. O vídeo
`res/raw/opening.mp4` era um splash do MediaPlayer Java e não é consumido pelo runtime;
a abertura jogável é renderizada pela própria engine com dados do OBB.

### Compilar e executar

```bash
cd ports/ff4
./build.sh
```

O build mantém o nome histórico `ff4`. O launcher PortMaster é
`Final Fantasy IV 3D Remake.sh`: ele recebe o mapeamento SDL específico do controle e
executa o jogo em foreground, sem administrar o EmulationStation.

### Mapa do código e licenças

- `main.c`: lifecycle, controle, touch nativo e cursor;
- `jni_shim.c`: ambiente JNI falso e callbacks da Activity;
- `game.c`: VFS, timing, saves, texturas e fontes;
- `audio.c`: bridge OpenSL ES para SDL2;
- `imports.c`: compatibilidade bionic/libc/GLES/OpenSL;
- `so_util.c`: loader ELF arm64;
- `vkbd.c`: teclado virtual contextual.

O loader segue a GPL-3.0 do repositório. `stb_image` e `stb_truetype` preservam os
termos public-domain/MIT upstream. SDL2 e zlib são dependências do sistema. Final
Fantasy IV, a engine, o OBB, arte e áudio pertencem à Square Enix e não são licenciados
nem distribuídos por este repositório-fonte.
