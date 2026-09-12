# Guias NextOS em português

[English](../en/README.md)

[Repositório](../../README.md) · [Catálogo dos 46 títulos](../../catalog/README.md) · [Portando Unity](../../portando_unity/README.md)

Comece pelo exemplo C no host e avance ao [primeiro port integrado](../../examples/first-port/README.md), usando o [SDK público](../../toolchains/sdk/README.md). Depois escolha a engine. Os guias ensinam comandos, contratos e critérios para a IA conduzir a implementação; os dados compatíveis do dono permanecem privados.

| Guia | Conteúdo |
| --- | --- |
| [Primeiros passos](PRIMEIROS-PASSOS.md) | Clone, ferramentas e primeiro teste |
| [Arquitetura](ARQUITETURA.md) | Componentes, responsabilidades e pins |
| [Portar com IA](PORTAR-COM-IA.md) | Missão, inventário e implementação autônoma |
| [Compilar ARM](COMPILAR-ARM.md) | Host, AArch64, ARMv7, NDK e ELF |
| [Pins e fontes](PINS-E-FONTES.md) | Composição exportada e recuperação de arquivos públicos |
| [Inventário Android](INVENTARIO-ANDROID.md) | Manifesto, imports, ABI, bloqueios e busca de referências |
| [Diagnóstico guiado](DIAGNOSTICO-GUIADO.md) | Logs bons/ruins e contraprovas executáveis |
| [Escolher runtime Android](ANDROID-RUNTIMES.md) | Oito trilhas, evidências e busca por família |
| [Shims](SHIMS.md) | ABI, JNI, ownership e testes |
| [Mono Android](MONO-ANDROID.md) | Mono/.NET, MonoGame/FNA e bootstrap |
| [Godot](GODOT.md) | Engine, export, renderer, C# e input |
| [Cocos2d-x](COCOS2D-X.md) | JNI, assets, texto, render e áudio |
| [GameMaker Android](GAMEMAKER-ANDROID.md) | Runner YoYo, VM/YYC, ABI e dados |
| [Ren’Py Android](RENPY-ANDROID.md) | Python, SDL/JNI, assets e persistência |
| [Haxe/hxcpp/Lime Android](HAXE-LIME-ANDROID.md) | Bootstrap, threads, TLS/GC e limites |
| [C/C++ Android](NATIVE-ANDROID.md) | JNI, NativeActivity, SDL Android e Bionic |
| [NXExtract](NXEXTRACT.md) | Receita, dados do dono e instalação limpa |
| [Testes e entrega](TESTES-E-ENTREGA.md) | Evidência, candidato e release |
| [Problemas comuns](PROBLEMAS-COMUNS.md) | Sintomas e diagnóstico dirigido |

## Trilhas especializadas

- [Unity](../../portando_unity/README.md): 15 casos com fontes públicas, dois registros comunitários e ferramentas sintéticas.
- [Freedom Planet 2 — Vulkan para GLES2](../../portando_unity/pt-BR/FP2-VULKAN-GLES2.md): conversão por programa e preservação de dados.
- [Exercícios por engine](../../examples/engines/README.md): Mono, Godot, Unity e Cocos2d-x, com status de build explícito.
- [Laboratório de shaders](../../examples/shader-lab/README.md): shaders autorais, pins públicos e contraprova compute.
- [Exemplo de shims](../../examples/shims-reference/README.md): contrato pequeno compilável.
- [Licenças e créditos](../../LICENSING.md), [contribuir](../../CONTRIBUTING.md), [validação](../../publication/VALIDATION.md).

## Idiomas e fontes históricas

Toda documentação editorial desta publicação tem versão PT/EN equivalente. Links no topo trocam idioma. Identificadores, caminhos, schemas e comandos permanecem estáveis; exemplos de prompts e explicações são traduzidos.

Arquivos históricos em `framework/`, na árvore NXExtract e em `ports/*/upstream/` conservam os bytes, licenças e idiomas originais. As fichas e os guias bilíngues estão fora dessas árvores imutáveis. Não foram traduzidos comentários de código, mensagens antigas de ferramentas ou textos legais normativos como se fossem nova licença.
