# Pikmin — running on Mali-450 (NextOS Elite)

**Language / Idioma:** [English](#english) · [Português](#português)

---

## Community

Questions, bug reports, help getting the port running, and news about the next ones:

💬 **Discord:** [discord.gg/DHfY62eDNN](https://discord.gg/DHfY62eDNN)

## English

A native AArch64 port of **Pikmin** (GameCube, 2001) for **NextOS Elite** on
Amlogic hardware with the **Mali-450 (Utgard) GPU and OpenGL ES 2.0**.

It is built on the [projectPiki](https://github.com/projectPiki/pikmin)
decompilation and runs the reconstructed game code natively — it is **not** a
GameCube emulator. The game boots, plays, saves and exits on a fixed-function
Mali-450 with 1 GB of RAM.

> [!WARNING]
> **Target hardware: Mali-450 (Utgard) with OpenGL ES 2.0 only.**
>
> This port was written for, built for and tested on exactly one class of
> hardware: Amlogic devices running NextOS Elite with an ARM **Mali-450 MP**
> (Utgard) GPU on the **fbdev** path, rendering through **OpenGL ES 2.0**.
>
> Nothing here has been tested on any other GPU, driver, kernel, distribution or
> graphics API. The renderer targets ES 2.0 specifically — there is no ES 3.x,
> desktop GL, Vulkan or WebGPU path in this fork. Other hardware may build, may
> run, may render incorrectly, or may not start at all — none of it is supported
> or claimed to work.

> [!IMPORTANT]
> This repository contains **no game data whatsoever** — no disc image, no
> texture, no audio bank, no Nintendo asset of any kind. You must supply your
> own legally obtained copy of the game. See [Game data](#game-data).
>
> That includes the font: the decompilation embeds a 508 KB font texture taken
> from the disc, and this port does not. It reads `bigFont.bti` from your image
> at boot, so neither the tree nor the built executable carries game art.
>
> It also contains no `libMali.so`. The Mali driver is proprietary ARM/Amlogic
> code; copy it from your own device to build. See [Building](#building).

### Contents

- [Screenshots](#screenshots)
- [What works](#what-works)
- [What this port adds](#what-this-port-adds)
- [Performance](#performance)
- [Controls](#controls)
- [Game data](#game-data)
- [Building](#building)
- [Repository layout](#repository-layout)
- [Credits](#credits)
- [Legal](#legal)

### Screenshots

Captured from the framebuffer on the target hardware — Mali-450 (Utgard),
OpenGL ES 2.0, fbdev, at the panel's native 1280×720.

| | |
|:---:|:---:|
| ![Title screen](docs/screenshots/01-title.png) | ![Area select on day 2](docs/screenshots/02-area-select.png) |
| The title screen, in 16:9 | Area select — day 2, loaded from a save |
| ![The Forest of Hope](docs/screenshots/03-forest-flora.png) | ![An Onion and its Pikmin](docs/screenshots/04-onion.png) |
| The Forest of Hope, full flora | An Onion, sprouts and the HUD |

![The Impact Site](docs/screenshots/05-impact-site.png)

*The Impact Site — Olimar, Pikmin and the day's first steps.*

### What works

Tested end to end on the device:

- **Boot to gameplay, unattended.** Nintendo logo → title → attract loop → file
  select → landing site → gameplay, every transition driven by the game's own
  state machine.
- **Sound.** Music and effects, rendered in software (see below). The GameCube
  DSP does not exist here, so the 64 voices are synthesized on the CPU.
- **Saving.** The memory card is a GCI folder on disk. Day-end saves and file
  select both work; a save made on day 1 loads back on day 2.
- **16:9 widescreen**, anamorphic — the 3D frustum widens and the 2D layers are
  squeezed by the panel's real aspect, read at runtime.
- **Native gamepad**, through the frontend's controller layout. No key mapper,
  no mouse cursor.
- **Clean exit.** Hold **SELECT + START** to quit back to the frontend, and the
  display driver is released properly.

### What this port adds

The decompilation targets the GameCube: it has no PC target, no CMake, no SDL
and no renderer. Everything below is the port layer built on top of it.

- **A software JAudio renderer.** This is the piece with no shortcut. Unlike
  MusyX, JAudio ships raw DSP microcode and has no PC abstraction to fill in —
  so `src/port/jaudio_host.cpp` renders the game's 64 native voices on the CPU:
  AFC and PCM decode, resampling, the low-pass/FIR/biquad chain, the bus
  layout and the effect sends, mixed to stereo and handed to SDL3 at 32 kHz.
  The game's own sequencer, banks and oscillators are untouched — only the
  DSP boundary is replaced.
- **A cooperative scheduler for Dolphin OS threads**, giving them the console's
  one-thread-at-a-time semantics, with a retrace clock on its own thread.
- **The OpenGL ES 2.0 path on Mali-450**, through our fork of Aurora: forward-Z
  on this driver, lighting specialized per draw so the classic Mali compiler
  only sees the lights that are on, and native vertex fetch for the game's
  indexed arrays.
- **64-bit ABI fixes to the decompiled code**, where it assumes a 32-bit
  pointer — allocator unit arithmetic, animation matrix caches, texture cache
  headers, and the envelope-skinning path.
- **Anamorphic widescreen**, packaging and the NextOS launcher.

### Performance

Measured on the device, at the panel's native 1280×720:

| Scene | Frame rate |
|---|---|
| Title and menus | 59.94 fps |
| The Impact Site | 29.97 fps — the game's own gameplay cap |
| The Forest of Hope | 24–27 fps |

The Forest of Hope is CPU-bound, not GPU-bound: it issues roughly 950 draw
calls per frame against a 33 ms budget on a Cortex-A53. That is the open
performance item.

### Controls

Standard GameCube mapping through the frontend's controller layout:

| GameCube | Action |
|---|---|
| Control stick | Move Olimar |
| A | Throw / confirm |
| B | Whistle / cancel |
| C stick | Camera |
| Start | Pause |
| **SELECT + START (hold)** | **Quit the port** |

### Game data

You need your own copy of **Pikmin (USA, `GPIE01`)** as a `.rvz`, `.iso` or
`.gcm` disc image. Nothing about the game is included here and nothing is
downloaded — the port reads the image directly (no extraction step).

Place it next to the port and pass it as the first argument:

```sh
./pikmin /path/to/pikmin.rvz
```

The NextOS launcher looks for the first image in the port's `assets/`
directory.

### Building

Builds are cross-compiled from a Linux host to AArch64 with the toolchain from
the current NextOS Elite tree; `configure-nextos.sh` locates it automatically,
or select it explicitly with `NEXTOS_TOOLCHAIN_ROOT`.

You also need two things that are not in this repository:

1. **A Mali-fbdev SDL3 source tree** — the `mali` video driver build
   (`SDL_MALI=ON`), not an SDL2 shim build. Point `PIKMIN_SDL_SRC` at it.
2. **`libMali.so` from your own device.** Copy it into `build/sysroot/lib/` and
   link the GLES/EGL names to it:

```sh
cp /path/from/device/libMali.so build/sysroot/lib/
ln -s libMali.so build/sysroot/lib/libGLESv2.so
ln -s libMali.so build/sysroot/lib/libEGL.so
```

Then:

```sh
git submodule update --init --recursive

PIKMIN_SDL_SRC=/path/to/SDL3-mali \
  ./configure-nextos.sh

cmake --build build/nextos-gles2 -j8
```

There is also a host build (`./configure-host.sh`) that compiles for x86_64 and
can run headless (`PIKMIN_BACKEND=null SDL_VIDEO_DRIVER=offscreen`). It is not a
deliverable — it exists because most bugs in this port were data parsing or
state-machine bugs, and iterating on the device costs minutes per attempt.

### Repository layout

| Path | What it is |
|---|---|
| `src/`, `include/` | The projectPiki decompilation (CC0), plus our 64-bit fixes |
| `src/port/` | The port layer — scheduler, audio, input, frame pump, disc |
| `extern/aurora/` | Our Aurora fork (submodule), branch `pikmin-mali450` |
| `packaging/` | NextOS launcher and packaging |
| `docs/screenshots/` | The images above |
| `build/sysroot/` | Khronos headers; the Mali driver goes here (not shipped) |

### Credits

This port stands on other people's work. Credit where it is due:

- **The [projectPiki](https://github.com/projectPiki/pikmin) team** — the Pikmin
  decompilation this port is built on, released under **CC0 1.0**. Without their
  reconstruction of the game code there is nothing to port. Everything in `src/`
  and `include/` outside `src/port/` is theirs.
- **Luke Street (`encounter`) and the Aurora contributors** — Aurora, the
  source-level GameCube/Wii compatibility layer that models the GX pipeline, and
  nod, the disc image reader. MIT licensed.
  Upstream: [encounter/aurora](https://github.com/encounter/aurora).
- **[Brian Degenhardt (`bmdhacks`)](https://github.com/bmdhacks/aurora)** — the
  GLES/GLES3 backend for Aurora, which cut the renderer over from Dawn/WebGPU
  and built the GX-to-GLES translation this port stands on. Our ES 2.0 work is
  an extension of his, not a replacement for it.
- **The [Dusklight](https://github.com/TwilitRealm/dusklight) team, and the
  [Twilight Princess decompilation](https://github.com/zeldaret/tp) team behind
  it** — that GLES renderer was built and proven on Dusklight. Pikmin renders on
  this hardware because that groundwork already existed.
- **The SDL contributors** — SDL3, used here with the Mali fbdev video backend
  and ALSA audio.
- **NextOS Elite** — the port layer, the software JAudio renderer, the Mali-450
  ES 2.0 work, the widescreen path, the 64-bit fixes, launcher and packaging.

Every dependency keeps its own license file in its source directory.

### Legal

The decompilation is CC0; our contributions are MIT — see [LICENSE](LICENSE) and
[NOTICE.md](NOTICE.md) for exactly what that grant does and does not cover.

Pikmin, Nintendo, and related names and assets are trademarks or copyrights of
Nintendo. This is an unaffiliated community port, not endorsed by Nintendo, and
it distributes no game data.

---

## Português

### Comunidade

Dúvidas, relatos de bug, ajuda pra colocar o port pra rodar e novidades dos próximos:

💬 **Discord:** [discord.gg/DHfY62eDNN](https://discord.gg/DHfY62eDNN)

Port nativo AArch64 de **Pikmin** (GameCube, 2001) para o **NextOS Elite** em
hardware Amlogic com **GPU Mali-450 (Utgard) e OpenGL ES 2.0**.

Ele é construído sobre a decompilação do
[projectPiki](https://github.com/projectPiki/pikmin) e executa o código
reconstruído do jogo nativamente — **não** é um emulador de GameCube. O jogo
inicia, joga, salva e sai num Mali-450 de função fixa com 1 GB de RAM.

> [!WARNING]
> **Hardware alvo: Mali-450 (Utgard) com OpenGL ES 2.0, só.**
>
> Este port foi escrito, compilado e testado em exatamente uma classe de
> hardware: aparelhos Amlogic rodando NextOS Elite com GPU ARM **Mali-450 MP**
> (Utgard) no caminho **fbdev**, renderizando por **OpenGL ES 2.0**.
>
> Nada aqui foi testado em outra GPU, driver, kernel, distribuição ou API
> gráfica. O renderizador mira ES 2.0 especificamente — não há caminho ES 3.x,
> GL desktop, Vulkan ou WebGPU neste fork. Outro hardware pode compilar, pode
> rodar, pode renderizar errado ou pode não abrir — nada disso é suportado nem
> alegado como funcional.

> [!IMPORTANT]
> Este repositório **não contém nenhum dado do jogo** — nenhuma imagem de disco,
> textura, banco de áudio ou qualquer asset da Nintendo. Você precisa fornecer
> sua própria cópia legal do jogo.
>
> Isso inclui a fonte: a decompilação embarca uma textura de fonte de 508 KB
> tirada do disco, e este port não. Ele lê `bigFont.bti` da sua imagem no boot,
> então nem a árvore nem o executável carregam arte do jogo.
>
> Também não contém `libMali.so`. O driver Mali é código proprietário da
> ARM/Amlogic; copie do seu próprio aparelho para compilar.

### O que funciona

Testado de ponta a ponta no aparelho:

- **Do boot ao gameplay, sozinho**: logo da Nintendo → título → tela de atração
  → seleção de arquivo → local de pouso → gameplay, cada transição vinda da
  própria máquina de estados do jogo.
- **Som**: música e efeitos, renderizados em software. O DSP do GameCube não
  existe aqui, então as 64 vozes são sintetizadas na CPU.
- **Save**: o memory card é uma pasta GCI em disco. Salvar no fim do dia e a
  seleção de arquivo funcionam; um save do dia 1 carrega no dia 2.
- **Widescreen 16:9** anamórfico — o frustum 3D alarga e as camadas 2D encolhem
  pelo aspecto real do painel, lido em tempo de execução.
- **Controle nativo**, pelo layout do frontend. Sem mapeador de teclas, sem
  cursor de mouse.
- **Saída limpa**: segure **SELECT + START** para voltar ao frontend, liberando
  o driver de vídeo corretamente.

### O que este port acrescenta

A decompilação mira o GameCube: não tem alvo PC, CMake, SDL nem renderizador.
Tudo abaixo é a camada de port construída sobre ela.

- **Um renderizador JAudio em software.** Esta é a parte sem atalho. Diferente
  do MusyX, o JAudio embarca microcódigo cru de DSP e não tem abstração de PC
  para preencher — então `src/port/jaudio_host.cpp` renderiza as 64 vozes
  nativas do jogo na CPU: decode AFC e PCM, reamostragem, a cadeia de filtros,
  o layout de barramentos e os envios de efeito, mixados em estéreo e entregues
  ao SDL3 a 32 kHz. O sequenciador, os bancos e os osciladores do jogo ficam
  intactos — só a fronteira do DSP é substituída.
- **Um escalonador cooperativo para as threads do Dolphin OS**, dando a elas a
  semântica de uma-thread-por-vez do console, com o relógio de retrace em thread
  própria.
- **O caminho OpenGL ES 2.0 no Mali-450**, pelo nosso fork do Aurora: forward-Z
  neste driver, iluminação especializada por draw para o compilador clássico do
  Mali ver só as luzes ligadas, e busca nativa de vértices para os arrays
  indexados do jogo.
- **Correções de ABI 64 bits no código decompilado**, onde ele assume ponteiro
  de 32 bits — aritmética de unidades do alocador, caches de matriz de animação,
  cabeçalhos de cache de textura e o caminho de skinning por envelope.
- **Widescreen anamórfico**, empacotamento e o launcher do NextOS.

### Desempenho

Medido no aparelho, no 1280×720 nativo do painel:

| Cena | Taxa de quadros |
|---|---|
| Título e menus | 59,94 fps |
| The Impact Site | 29,97 fps — o teto do próprio jogo em gameplay |
| The Forest of Hope | 24–27 fps |

The Forest of Hope é limitado por CPU, não por GPU: são cerca de 950 draw calls
por quadro contra um orçamento de 33 ms num Cortex-A53. Esse é o item de
desempenho em aberto.

### Dados do jogo

Você precisa da sua própria cópia de **Pikmin (USA, `GPIE01`)** como imagem de
disco `.rvz`, `.iso` ou `.gcm`. Nada do jogo vem aqui e nada é baixado — o port
lê a imagem direto, sem etapa de extração.

### Créditos

Este port se apoia no trabalho de outras pessoas:

- **Time do [projectPiki](https://github.com/projectPiki/pikmin)** — a
  decompilação de Pikmin sobre a qual este port é construído, sob **CC0 1.0**.
  Sem a reconstrução do código do jogo feita por eles não há o que portar.
- **Luke Street (`encounter`) e os contribuidores do Aurora** — o Aurora, camada
  de compatibilidade GameCube/Wii em nível de código, e o nod, leitor de imagem
  de disco. Licença MIT.
- **[Brian Degenhardt (`bmdhacks`)](https://github.com/bmdhacks/aurora)** — o
  backend GLES/GLES3 do Aurora, que migrou o renderizador do Dawn/WebGPU e
  construiu a tradução GX→GLES sobre a qual este port se apoia. Nosso trabalho
  em ES 2.0 é uma extensão do dele, não um substituto.
- **O time do [Dusklight](https://github.com/TwilitRealm/dusklight) e o time da
  [decompilação de Twilight Princess](https://github.com/zeldaret/tp)** — aquele
  renderizador GLES foi construído e provado no Dusklight. Pikmin renderiza
  neste hardware porque essa base já existia.
- **Os contribuidores do SDL** — SDL3, aqui com o backend de vídeo Mali fbdev e
  áudio ALSA.
- **NextOS Elite** — a camada de port, o renderizador JAudio em software, o
  trabalho ES 2.0 no Mali-450, o widescreen, as correções de 64 bits, launcher e
  empacotamento.

### Legal

A decompilação é CC0; nossas contribuições são MIT — veja [LICENSE](LICENSE) e
[NOTICE.md](NOTICE.md).

Pikmin, Nintendo e nomes e assets relacionados são marcas ou direitos autorais
da Nintendo. Este é um port comunitário não afiliado, sem endosso da Nintendo, e
não distribui nenhum dado do jogo.
