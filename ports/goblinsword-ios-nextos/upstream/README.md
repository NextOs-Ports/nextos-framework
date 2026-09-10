# Goblin Sword iOS → NextOS

## Capturas reais / Real screenshots

Capturas do NextOS/Mali-450, com a referência de gameplay `6bf61d62`.
Real NextOS/Mali-450 captures from gameplay reference `6bf61d62`.
[Identidade das imagens / Image provenance](docs/screenshots/captures.json).

**Menu original / Original menu**

![Menu original do Goblin Sword no NextOS / Original Goblin Sword menu on NextOS](docs/screenshots/menu.png)

**Primeira fase: personagem, HUD e tutorial / First stage: hero, HUD and tutorial**

![Primeira fase do Goblin Sword no NextOS / Goblin Sword first stage on NextOS](docs/screenshots/first-stage.png)

**Movimento do personagem e da câmera pela vila / Hero and camera movement through the village**

![Personagem adiante na vila após movimento com controle / Hero farther into the village after controller input](docs/screenshots/village-movement.png)

## Português

Protótipo de compatibilidade para executar o Mach-O ARM64 original de
Goblin Sword 2.6.9 em Linux AArch64. Implementa os contratos de Objective-C,
Foundation, UIKit/EAGL e outros serviços que este jogo utiliza. O alvo físico
testado é NextOS, Amlogic-old, Mali-450/fbdev e GLES2.

Há prova física de menu, introdução concluída com save nativo e movimento
na primeira fase. Foram observados cerca de **40 FPS no menu e na introdução**
e **13–15 FPS na primeira fase**. O usuário aprovou a experiência inicial;
fase completa, combate, áudio confirmado por escuta e estabilidade prolongada
continuam fora dessa validação.

A referência de gameplay é o executável identificado pelo prefixo SHA-256
`6bf61d62`. O sucessor `c9b02720`, representado pelo código funcional publicado,
acrescentou saída por Select+Start: passou em 18 casos host, abriu o menu a
40 FPS e saiu fisicamente com status 0, restaurando o timer do console.
O gameplay da referência não foi reapresentado como um novo teste do sucessor.
Detalhes em [validação](docs/validation.md).

Este repositório contém código e ferramentas de estudo. Não contém IPA,
executável do jogo, assets, binários Linux ou registros privados.
É necessário fornecer uma cópia própria compatível. Não há instalador,
pacote universal ou release de jogo pronta.

### Preparar e executar

Use um toolchain AArch64 e sysroot compatíveis com o NextOS alvo. O build
precisa das bibliotecas e headers do sistema: EGL/GLES2, SDL2, libpng, zlib,
FreeType, FFmpeg e OpenAL. A execução usa a implementação Mali/EGL/GLES2
e as bibliotecas SDL2, FFmpeg e OpenAL do próprio aparelho.

```sh
make -C prototype TOOLCHAIN=/path/to/nextos/toolchain -j4
python3 tools/prepare_owner_data.py /path/to/owned.ipa --output data
```

A ferramenta prepara `data/goblin-sword.macho` e `data/assets/` localmente,
validando a identidade da versão suportada e o executável ARM64 não
criptografado. Esses dados não devem ser adicionados ao Git.

No aparelho AArch64, com o executável construído e os dados preparados:

```sh
GOBLIN_DIAGNOSTIC_SECONDS=0 ./prototype/build/goblinsword-ios-nextos data/goblin-sword.macho data/assets
```

O programa recebe exatamente dois argumentos: executável Mach-O e diretório
de recursos. `GOBLIN_DIAGNOSTIC_SECONDS=0` desativa o limite de tempo;
use, por exemplo, `=120` para uma sessão limitada a 120 segundos. Se a variável
for omitida, o limite padrão é **30 segundos**.

É necessário acesso ao framebuffer e ao VT ativo. Deixe o frontend e outra
instância do jogo fora da execução, para não disputarem vídeo, áudio e entrada.
O diretório de recursos precisa permitir o save local em `.ipa-study/`.
Consulte [arquitetura e requisitos](docs/architecture.md) e
[testes reproduzíveis](docs/validation.md). As instruções detalhadas estão
em inglês nesses documentos.

## English

An experimental compatibility runtime for the original Goblin Sword 2.6.9
ARM64 Mach-O on AArch64 Linux. It implements the Objective-C, Foundation,
UIKit/EAGL and related contracts used by this game. The physically tested
target is NextOS on Amlogic-old, Mali-450/fbdev and GLES2.

Physical testing reached the menu, completed the original intro with a native
save, and showed movement in the first stage. Observed performance was about
**40 FPS in the menu/intro and 13–15 FPS in the first stage**. The owner approved
the initial experience. Full-stage completion, combat, listening-confirmed
audio and prolonged stability remain unvalidated.

The preserved gameplay reference has executable SHA-256 prefix `6bf61d62`.
The source successor `c9b02720` adds Select+Start exit. Its 18 host cases
passed, and physical testing confirmed a 40 FPS menu and exit status 0 with
the console timer restored. The earlier gameplay evidence is not a fresh
gameplay test of this successor. See [validation](docs/validation.md).

This repository contains source and study tools. It does not include the IPA,
game executable, assets, Linux binaries or private run records.
Supply your own compatible data. There is no installer, universal package
or ready-to-play game release.

### Build and run

Use an AArch64 toolchain/sysroot compatible with your NextOS target, providing
EGL/GLES2, SDL2, libpng, zlib, FreeType, FFmpeg and OpenAL headers/libraries.
The runtime uses the device's system libraries; it does not bundle private
SDL, OpenAL or Mali copies.

```sh
make -C prototype TOOLCHAIN=/path/to/nextos/toolchain -j4
python3 tools/prepare_owner_data.py /path/to/owned.ipa --output data
```

The data tool locally produces `data/goblin-sword.macho` and `data/assets/`
after validating the supported identity and an unencrypted ARM64 executable.
Keep these files out of Git. On the AArch64 target, run:

```sh
GOBLIN_DIAGNOSTIC_SECONDS=0 ./prototype/build/goblinsword-ios-nextos data/goblin-sword.macho data/assets
```

The two positional arguments are the Mach-O executable and resource directory.
Zero disables the diagnostic time limit; use an explicit value such as `120`
for a bounded run. Without the environment variable, the default is
**30 seconds**.

The process needs access to the framebuffer and active Linux VT. Stop the
frontend and any previous game instance before starting, so they do not
compete for display, audio and input. The resource directory must permit
local save files under `.ipa-study/`. See [architecture](docs/architecture.md)
and [validation](docs/validation.md) for the scope and test requirements.

## Controles / Controls

Names are logical controller labels; no automatic PlayStation/PS2 symbol
translation is implied. / Os nomes são botões lógicos, sem tradução automática
para símbolos de PlayStation/PS2.

| Entrada / Input | Ação / Action |
| --- | --- |
| Direções / Directions | Navegar e mover / Navigate and move |
| A | Confirmar no menu; pular / Confirm in menus; jump |
| Y | Pular / Jump |
| B ou/or X | Atacar / Attack |
| R1 | Dash |
| Start | Pausar / Pause |
| Select + Start | Sair, no mesmo controle / Quit, on the same controller |

## Licença / License

**Licença ainda não selecionada.** A licença do código será informada quando
escolhida. As dependências e os dados originais mantêm suas próprias licenças
e direitos.

**License not yet selected.** Source licensing will be stated once selected.
Dependencies and original game data retain their respective licenses and rights.
