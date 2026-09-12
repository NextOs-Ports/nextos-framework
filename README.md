# NextOS Framework

[English](README.en.md)

Ports, estudos e exemplos de iOS estão fora do escopo desta coleção.

Framework V5 para criar ports Android em Linux ARM, com **guias ampliados em português e inglês**, fontes selecionadas de **44 títulos** e mais **2 ports no catálogo comunitário**. A IA pode conduzir a maior parte da investigação, código, build e verificações.

**Repositório privado para revisão. Tornar público exige aprovação explícita de NextOS.** A coleção não inclui dados comerciais de jogos e não representa 46 pacotes instaláveis certificados.

Autoria da coleção/integração: **NextOS** · [GitHub oficial](https://github.com/NextOs-Ports).

## Comece aqui

1. [Primeiros passos](docs/pt-BR/PRIMEIROS-PASSOS.md) e [índice de todos os guias](docs/pt-BR/README.md).
2. [Deixar a IA conduzir o port](docs/pt-BR/PORTAR-COM-IA.md).
3. [SDK público ARM](toolchains/sdk/README.md) e [primeiro port integrado](examples/first-port/README.md).
4. [Shims](docs/pt-BR/SHIMS.md), [NXExtract](docs/pt-BR/NXEXTRACT.md) e [testes](docs/pt-BR/TESTES-E-ENTREGA.md).

## Guias por engine

[Escolher runtime Android](docs/pt-BR/ANDROID-RUNTIMES.md). As referências de jogos são somente os ports públicos já selecionados na coleção.

| Trilha | O que ensina |
| --- | --- |
| [Unity](portando_unity/README.md) | Triagem, lifecycle, GLES2, ETC1/dual, input, áudio e 15 casos públicos |
| [Mono Android](docs/pt-BR/MONO-ANDROID.md) | Mono/.NET, MonoGame/FNA da build Android, assemblies e bootstrap |
| [Godot](docs/pt-BR/GODOT.md) | Engine/export, renderer, C#, viewport e InputMap |
| [Cocos2d-x](docs/pt-BR/COCOS2D-X.md) | Biblioteca C++, JNI, assets, texto, áudio e loop nativo |
| [GameMaker Android](docs/pt-BR/GAMEMAKER-ANDROID.md) | Runner YoYo, VM/YYC, ABI e dados |
| [Ren’Py Android](docs/pt-BR/RENPY-ANDROID.md) | Python, SDL/JNI, assets e persistência |
| [Haxe/hxcpp/Lime Android](docs/pt-BR/HAXE-LIME-ANDROID.md) | Bootstrap, threads, TLS/GC e limites |
| [C/C++ Android](docs/pt-BR/NATIVE-ANDROID.md) | JNI, NativeActivity, SDL Android e Bionic |
| [Freedom Planet 2: Vulkan → GLES2](portando_unity/pt-BR/FP2-VULKAN-GLES2.md) | SMOL-V/SPIR-V, tradução de programas, escrita cirúrgica e stencil/alpha |

## Primeiro exemplo

```sh
cmake -S examples/shims-reference -B work/host -DCMAKE_BUILD_TYPE=Release
cmake --build work/host --parallel 2
ctest --test-dir work/host --output-on-failure
```

Esse é o teste introdutório C no computador. Em seguida, o [minijogo autoral integrado](examples/first-port/README.md) ensina a compilar ARM64, carregar um guest Android, instalar pelo NXExtract e executar 11 verificações em CPU emulada. O [SDK público](toolchains/sdk/README.md) fornece seu ambiente de build; imagem, som e controles físicos continuam exigindo teste no aparelho.

Para trabalhar em outro jogo, use o [inventário executável](docs/pt-BR/INVENTARIO-ANDROID.md), os [pins e fontes](docs/pt-BR/PINS-E-FONTES.md), os [exercícios por engine](examples/engines/README.md) e o [laboratório de shaders](examples/shader-lab/README.md).

## Catálogo e fontes

[46 títulos NextOS](catalog/README.md): 44 com seleção de fontes de 40 repositórios públicos e 2 fichas comunitárias — Stranger Things 3 e AVGN I & II Deluxe, ainda sem código importado. Freedom Planet 2 já integra os 44. Cada referência possui status, origem e limites próprios.

| Diretório | Conteúdo |
| --- | --- |
| `framework/` | Componentes/templates V5 preservados |
| `suportando_outros_devices/extrator-universal/` | NXExtract pinado: engine, runner e UI |
| `ports/` | Snapshots de código, fichas PT/EN e manifestos |
| `portando_unity/` | Edição selecionada bilíngue, casos e ferramentas genéricas |
| `docs/pt-BR/`, `docs/en/` | Guias ampliados e correspondentes |
| `examples/`, `toolchains/` | Exemplos integrados, engines, shaders e SDK público |
| `publication/` | Integridade, validação, idiomas e pendências |

V5: `framework-v5` @ `657fb65a23b5c3b20040e76307b27e6470b1d17c`. [Hashes exportados](publication/v5-export.json). Doze testes históricos com dependências privadas foram omitidos; os bytes do runtime foram preservados. Ports antigos conservam pins V3/V4/V5/V6; nenhuma migração foi feita.

## Contribuir e redistribuir

Leia [AGENTS.md](AGENTS.md), [contribuição](CONTRIBUTING.md) e [licenças/créditos](LICENSING.md). Use somente a cópia do dono para preparar dados; nunca envie APK, IPA, OBB, bibliotecas originais, assets ou saves ao GitHub/CI. ZIPs obedecem às licenças de cada componente e preservam créditos NextOS/terceiros. A proposta não comercial continua em revisão e não substitui permissões GPL/MIT existentes.

Os guias editoriais são bilíngues; fontes históricas, comentários e licenças normativas preservam o idioma original. [Atualização e testes](publication/ONBOARDING-UPDATE.md) · [Pendências antes de publicar](publication/REVIEW.md).
