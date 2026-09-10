# Installation — Beach Buggy Racing 2

## English

This port ships the runtime only. The game itself is **not** included and is
not distributed here: Beach Buggy Racing 2, its engine, its assets, its art and
its audio belong to Vector Unit. You must supply them from a copy you legally
own.

1. Install the port through PortMaster, or unzip it into your `ports` folder.
2. Put your Android APK into the port's `gamedata/` folder (create it if it
   does not exist) or next to the launcher. Any filename works — the extractor
   identifies the package by its contents, not by its name.
3. Launch the port once. It extracts, validates and commits the payload, then
   starts the game. Extraction runs only on the first launch.

### Reference APK

The build was tested against the release identified below. Other builds of
the same package are accepted as long as the engine and the assets validate.

| Field | Value |
|---|---|
| Package | `com.vectorunit.cobalt.googleplay` |
| App version | `2026.07.31` |
| Size | 282239181 bytes |
| SHA-256 | `d15056d8b5dde16b4b7cc61432cc0045e2834821433999f0a6ffd52ecd2b8330` |
| ABI used | `arm64-v8a` |

### Requirements

- aarch64 CFW with glibc 2.27 or newer
- zlib from the firmware (SDL3 is bundled with the port)
- OpenGL ES 2.0, from `libGLESv2` or from a unified `libmali` blob — the port
  resolves it at run time and does not require either SONAME to exist
- roughly 250 MiB free for the extracted payload

### Controls

- Left stick: steering
- `A`: confirm / use power-up (never a cursor click)
- `R1`: accelerate
- `L1`: brake
- Right stick: menu cursor when a touch menu needs it
- `R2` or `R3`: cursor click while the cursor is visible
- `START`: pause
- `SELECT` + `START`: exit the port

## Português

Este pacote traz apenas o runtime. O jogo **não** vem incluso e não é
distribuído aqui: Beach Buggy Racing 2, sua engine, os assets, a arte e o áudio
pertencem à Vector Unit. Você precisa fornecê-los a partir de uma cópia
adquirida legalmente.

1. Instale o port pelo PortMaster, ou descompacte na sua pasta `ports`.
2. Coloque o APK Android na pasta `gamedata/` do port (crie a pasta se não
   existir) ou ao lado do launcher. O nome do arquivo não importa — o extrator
   identifica o pacote pelo conteúdo, não pelo nome.
3. Abra o port uma vez. Ele extrai, valida e publica o payload, e então inicia
   o jogo. A extração acontece só na primeira abertura.

### APK de referência

O build foi testado com a versão identificada abaixo. Outros builds do mesmo
pacote são aceitos desde que a engine e os assets passem na validação.

| Campo | Valor |
|---|---|
| Pacote | `com.vectorunit.cobalt.googleplay` |
| Versão do app | `2026.07.31` |
| Tamanho | 282239181 bytes |
| SHA-256 | `d15056d8b5dde16b4b7cc61432cc0045e2834821433999f0a6ffd52ecd2b8330` |
| ABI usada | `arm64-v8a` |

### Requisitos

- CFW aarch64 com glibc 2.27 ou mais nova
- zlib vindo do firmware (a SDL3 vem incluída no port)
- OpenGL ES 2.0, seja pela `libGLESv2` ou por um blob `libmali` unificado — o
  port resolve em tempo de execução e não exige que nenhum desses SONAMEs
  exista
- cerca de 250 MiB livres para o payload extraído

### Controles

- Analógico esquerdo: direção
- `A`: confirmar / usar power-up (nunca clique de cursor)
- `R1`: acelerar
- `L1`: frear
- Analógico direito: cursor nos menus touch
- `R2` ou `R3`: clique do cursor enquanto ele estiver visível
- `START`: pausa
- `SELECT` + `START`: sai do port
