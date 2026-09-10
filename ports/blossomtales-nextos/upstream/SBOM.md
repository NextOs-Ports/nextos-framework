# SBOM — Blossom Tales (port)

Inventário do que é **nosso**, do que é **de terceiros e embutido** e do que é
**do sistema**. Hashes conferidos pelo `build_buster.sh`.

## Nosso (código do port)

| Componente | Origem | Licença |
|---|---|---|
| so-loader / shims JNI / ponte EGL | `ports/blossomtales/src/` | GPL-3.0-only |
| fachada FBO MonoGame sobre o core GLES (`fbo_compat.c`) | idem | GPL-3.0-only |
| decodificador de mídia (`aac_decoder.c`, `media_player.c`) | idem | GPL-3.0-only |
| resolução de biblioteca do host (`host_libs.c`) | idem | GPL-3.0-only |

O código nxinput 0.11.8 e nxcompat 0.5.3 usado pelo executável é vendorizado
byte a byte do commit fixado em `FRAMEWORK-PIN.json`; os hashes individuais
estão em `vendor/nxinput/PINS.json` e `vendor/nxcompat/PINS.json`.

## Terceiros embutidos (ligados estaticamente no executável)

| Componente | Versão | SHA-256 da fonte | Licença |
|---|---|---|---|
| FDK-AAC (só o decodificador) | 2.0.3 | `e25671cd96b10bad896aa42ab91a695a9e573395262baed4e4a2ff178d6a3a78` | Fraunhofer FDK AAC Codec Library for Android |
| minimp4 (`minimp4.h`) | arquivo único versionado no port | `1fc8d29b8c29dbeead9710bd9e95f6aab4d2fec48a55c2cc54a59f0338190bf2` | CC0-1.0 |

## Do sistema (carregado do aparelho, nunca redistribuído)

| Componente | Como é resolvido |
|---|---|
| SDL2 | biblioteca do CFW; nunca sombreada por `LD_LIBRARY_PATH` nem por cópia privada |
| EGL / OpenGL ES 2.0 | driver do aparelho |
| OpenAL Soft | biblioteca do sistema |
| ICU | por capacidade: o nome curto `libicuuc.so` do Android é resolvido para a maior versão instalada (ver `src/host_libs.c`) |
| glibc | teto de `GLIBC_2.30`; este build exige no máximo `GLIBC_2.27` |

## Ambiente de build

| Item | Valor |
|---|---|
| Imagem | `blossomtales-builder:glibc230` (Debian 10 buster, glibc 2.28) |
| Compilador | `aarch64-linux-gnu-gcc-8` / `g++-8` |
| Receita | `Dockerfile.builder` + `build_buster.sh` + `vendor/build_fdk.sh` |
| Pins de terceiros | `vendor/PINS.txt` |

## Dados do jogo

**Não fazem parte deste pacote.** Ver `INSTALLATION.md`.
