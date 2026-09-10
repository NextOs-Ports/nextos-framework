# Installation — Final Fantasy IV 3D Remake

## English

This port ships the runtime only. The game itself is **not** included and is
not distributed here: Final Fantasy IV 3D Remake, its engine, its OBB, its art and its audio
belong to Square Enix. You must supply them from a copy you legally own.

1. Install the port through PortMaster, or unzip it into your `ports` folder.
2. Put your Android APK into the port's `gamedata/` folder. Any filename works
   — the extractor identifies the package by its contents, not by its name.
3. Launch the port once. It extracts, validates and commits the payload, then
   starts the game. Extraction runs only on the first launch.

### Reference APK

The build was tested against the release identified below. Other builds of
the same package are accepted as long as the engine and the OBB validate.

| Field | Value |
|---|---|
| Package | `com.square_enix.android_googleplay.FFIV_GP` |
| App version | `2.0.4` |
| Size | 576871164 bytes |
| SHA-256 | `dfc3796ed07d099b5c0979c3c0e85435f4448b18c6b17a4588ea3e9dcdb17c5d` |
| ABI used | `arm64-v8a` |

The Japanese package is **not** accepted by this port.

### Requirements

- aarch64 CFW with glibc 2.27 or newer
- SDL2 and zlib from the firmware or from PortMaster
- OpenGL ES 1.1, from `libGLESv1_CM` or from a unified `libmali` blob —
  the port resolves it at run time and does not require either SONAME to exist
- roughly 460 MiB free for the extracted payload

### Exit

`SELECT` + `START` closes the game.

## Português

Este pacote traz apenas o runtime. O jogo **não** vem incluso e não é
distribuído aqui: Final Fantasy IV 3D Remake, sua engine, o OBB, a arte e o áudio pertencem à
Square Enix. Você precisa fornecê-los a partir de uma cópia adquirida
legalmente.

1. Instale o port pelo PortMaster, ou descompacte na sua pasta `ports`.
2. Coloque o APK Android na pasta `gamedata/` do port. O nome do arquivo não
   importa — o extrator identifica o pacote pelo conteúdo, não pelo nome.
3. Abra o port uma vez. Ele extrai, valida e publica o payload, e então inicia
   o jogo. A extração acontece só na primeira abertura.

### APK de referência

O build foi testado com a versão identificada abaixo. Outros builds do mesmo
pacote são aceitos desde que a engine e o OBB passem na validação.

| Campo | Valor |
|---|---|
| Pacote | `com.square_enix.android_googleplay.FFIV_GP` |
| Versão do app | `2.0.4` |
| Tamanho | 576871164 bytes |
| SHA-256 | `dfc3796ed07d099b5c0979c3c0e85435f4448b18c6b17a4588ea3e9dcdb17c5d` |
| ABI usada | `arm64-v8a` |

O pacote japonês **não** é aceito por este port.

### Requisitos

- CFW aarch64 com glibc 2.27 ou mais nova
- SDL2 e zlib vindos do firmware ou do PortMaster
- OpenGL ES 1.1, seja pela `libGLESv1_CM` ou por um blob `libmali` unificado —
  o port resolve em tempo de execução e não exige que nenhum desses SONAMEs
  exista
- cerca de 460 MiB livres para o payload extraído

### Saída

`SELECT` + `START` fecha o jogo.
