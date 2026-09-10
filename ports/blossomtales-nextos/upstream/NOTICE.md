# NOTICE — Blossom Tales: The Sleeping King (port)

Este pacote contém **apenas** o runtime do port. O jogo, seus dados e sua
trilha sonora **não** são redistribuídos: eles vêm da cópia legal do próprio
usuário, extraída no aparelho na primeira execução.

## Componentes de terceiros embutidos no executável

### FDK-AAC 2.0.3 — decodificador AAC

- Versão: 2.0.3
- SHA-256 do pacote fonte:
  `e25671cd96b10bad896aa42ab91a695a9e573395262baed4e4a2ff178d6a3a78`
- Licença: *Software License for The Fraunhofer FDK AAC Codec Library for
  Android* (arquivo `NOTICE` do próprio pacote). Permite redistribuição em
  forma de fonte e de binário mantendo o aviso de copyright. Não é GPL.
- Uso: somente o decodificador, ligado estaticamente. Substitui a chamada a
  `ffmpeg`/`ffprobe` do sistema que a versão anterior deste port fazia.

> © Copyright 1995 - 2018 Fraunhofer-Gesellschaft zur Förderung der angewandten
> Forschung e.V. All rights reserved.
>
> O uso do FDK AAC Codec pode exigir licenças de patente. Consulte o texto
> integral da licença distribuído com o código-fonte do FDK-AAC.

### minimp4 — demultiplexador MP4

- Origem: projeto `minimp4` (arquivo único `minimp4.h`)
- SHA-256 do arquivo usado:
  `1fc8d29b8c29dbeead9710bd9e95f6aab4d2fec48a55c2cc54a59f0338190bf2`
- Licença: CC0-1.0 (domínio público)
- Uso: leitura do container MP4/M4A da trilha.

## Componentes do sistema (não redistribuídos)

O port usa, sempre do próprio sistema operacional do aparelho e nunca de uma
cópia privada: SDL2, EGL/OpenGL ES, OpenAL Soft e ICU.

## Runtime do jogo

As bibliotecas MonoVM / .NET for Android / MonoGame do jogo pertencem à cópia
do usuário e são carregadas de dentro dela. Nada disso é distribuído aqui.
