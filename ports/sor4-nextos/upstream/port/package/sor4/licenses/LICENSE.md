# Licencas — Streets of Rage 4 (port)

Este pacote NAO contem nenhum dado do jogo. O usuario fornece a sua propria copia
legal do APK (Streets of Rage 4 1.4.5) e o launcher extrai os dados dele na 1a
execucao (BYO-data). Abaixo, a procedencia e licenca de cada componente.

## Jogo (NAO incluso — BYO-data)
"Streets of Rage 4" (c) DotEmu / Guard Crush Games / Lizardcube / SEGA. Todas as
marcas, texturas, audio (.wem/.bnk), fontes, assemblies gerenciadas e o codigo do
jogo (`SOR4.dll`) sao propriedade dos respectivos donos e NAO sao redistribuidos
aqui. Eles vem do APK do proprio usuario; o instalador preserva o APK original.

## Codigo do port (NextOs-Ports) — Apache-2.0
O host .NET (`sor4host`), os patches do MonoGame (GLES2), o reimpl de audio
(`libWwise.so` = wrapper + OpenAL/opusfile), o glue e encoder ETC1 de textura em
`libsor4astc.so` e as ferramentas Cecil de patch sao do porter (NextOs-Ports),
liberados sob Apache-2.0, derivados do framework nextos_ports_android.

## Arm astcenc 5.0.0 (`libsor4astc.so`) — Apache-2.0
O decoder ASTC incorporado em `libsor4astc.so` usa Arm astcenc 5.0.0 em modo
decompress-only. Copyright Arm Limited e contribuidores, sob Apache-2.0.
https://github.com/ARM-software/astc-encoder (texto em `Apache-2.0.txt`).

## Wwise (NAO incluso — BYO, proprietario)
`libWwise.real.so` e o Audiokinetic Wwise runtime, (c) Audiokinetic Inc.,
PROPRIETARIO. Nao e redistribuido: e extraido do APK do usuario. A nossa
`libWwise.so` apenas o carrega para a logica e toca o som via OpenAL/opusfile.

## Runtime .NET e BCL bundlados — MIT
O runtime .NET 9 CoreCLR e a biblioteca base (`System.*.dll`, `Microsoft.*.dll`,
`lib*System*.so`, `libcoreclr*` etc.) sao (c) .NET Foundation and Contributors, sob
a licenca MIT. https://github.com/dotnet/runtime/blob/main/LICENSE.TXT
(texto completo em `MIT.txt`).

## MonoGame.Framework.dll — Ms-PL + MIT
(c) The MonoGame Team. Licenca Microsoft Public License (Ms-PL); partes derivadas do
XNA/Microsoft sob MIT. https://github.com/MonoGame/MonoGame/blob/develop/LICENSE.txt
(texto completo em `MonoGame-Ms-PL.txt`).

## Mono.Cecil.dll — MIT
(c) Jb Evain e contribuidores. MIT. https://github.com/jbevain/cecil

## StbImageSharp / StbImageWriteSharp — Public Domain
Portes gerenciados de stb_image/stb_image_write mantidos pelo projeto StbSharp e
contribuidores. Os repositórios dos pacotes declaram licença Public Domain; o stb
original é oferecido em domínio público ou MIT, à escolha do usuário.
https://github.com/StbSharp/StbImageSharp
https://github.com/StbSharp/StbImageWriteSharp

## SharpFont.Core.dll — MIT
(c) Robert Rouhani. MIT. https://github.com/Robmaister/SharpFont
(faz P/Invoke na FreeType e HarfBuzz. O launcher prefere SEMPRE as bibliotecas do
proprio CFW, resolve tambem os SONAMEs versionados e cria aliases fora do filesystem
da ROM; o par bundlado abaixo so entra se o firmware nao tiver nenhuma.)

## FreeType 2.10.4 (`host_pkg/libs/fallback/libfreetype.so.6`) — FTL
Este software e baseado em parte no trabalho do FreeType Team. O pacote inclui uma
biblioteca AArch64 de ULTIMO RECURSO, usada somente em firmwares que nao fornecem
nenhuma FreeType; ela e compilada sem zlib, png, bzip2, brotli e harfbuzz, e pode ser
substituida pelo usuario. https://www.freetype.org (texto em `freetype-FTL.txt`).

## HarfBuzz 2.6.4 (`host_pkg/libs/fallback/libharfbuzz.so.0`) — Old MIT
(c) Google, Inc., Behdad Esfahbod e contribuidores. Mesma regra de ultimo recurso da
FreeType acima; compilada sem glib, graphite2, icu, cairo e fontconfig, mantendo
apenas a integracao com FreeType que os bindings do jogo exigem
(`hb_ft_font_create_referenced`). https://harfbuzz.github.io
(texto em `harfbuzz-Old-MIT.txt`).

## SDL2 (`libSDL2-2.0.so*`) — zlib
(c) Sam Lantinga. Licenca zlib. https://www.libsdl.org/
(texto completo em `SDL2-zlib.txt`.) Em alguns devices o launcher usa a libSDL2 do
proprio CFW; nesse caso a libSDL2 e a do sistema.

## OpenAL Soft 1.21.1 (`libopenal.so.1`) — LGPL-2.1-or-later
(c) OpenAL Soft contributors. O pacote inclui uma biblioteca AArch64 dinamica e
substituivel, com backends PulseAudio/ALSA. Veja `openal-soft-LGPL-NOTICE.txt` e o
texto completo em `LGPL-2.1-or-later.txt`.

## opusfile 0.9 / Opus 1.3 / Ogg 1.3.2 — BSD-3-Clause
(c) Xiph.Org e contribuidores. O pacote inclui fallbacks AArch64 de Debian Buster
sob seus SONAMEs (`libopusfile.so.0`, `libopus.so.0`, `libogg.so.0`). Veja
`Xiph-Opus-Ogg-BSD.txt`.

## LZ4 (setup e streaming de XNB) — BSD-2-Clause
Codec LZ4 (lz4net) (c) Yann Collet / Milosz Krajewski, usado pelo conversor de
texturas e pela releitura limitada dos XNB. O extrator do assembly-store inclui um
decoder independente em Python. https://github.com/lz4/lz4

## Ferramentas de extracao (`tools/`)
- `sor4probe` e `sor4splash`: utilitarios nativos do port; carregam o SDL2 do CFW
  dinamicamente e seguem Apache-2.0 junto com o restante do codigo do port.
- O instalador, validador e extrator em shell/Python seguem Apache-2.0.
- gptokeyb (opcional, `SOR4_USE_GPTK=1`): GPL-2.0, fornecido pelo CFW (nao bundlado).
