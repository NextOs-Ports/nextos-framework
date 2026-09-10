# Guias NextOS em português

[English](../en/README.md)

[Repositório](../../README.md) · [Catálogo dos 47 títulos](../../catalog/README.md) · [Portando Unity](../../portando_unity/README.md)

Comece pelo primeiro exemplo no host; depois escolha a engine. Os guias ensinam comandos, contratos e critérios para a IA conduzir a implementação. Pré-requisitos que ainda precisam ser fornecidos, como sysroot e dados do dono, aparecem explicitamente.

| Guia | Conteúdo |
| --- | --- |
| [Primeiros passos](PRIMEIROS-PASSOS.md) | Clone, ferramentas e primeiro teste |
| [Arquitetura](ARQUITETURA.md) | Componentes, responsabilidades e pins |
| [Portar com IA](PORTAR-COM-IA.md) | Missão, inventário e implementação autônoma |
| [Compilar ARM](COMPILAR-ARM.md) | Host, AArch64, ARMv7, NDK e ELF |
| [Shims](SHIMS.md) | ABI, JNI, ownership e testes |
| [Mono Android](MONO-ANDROID.md) | Mono/.NET, MonoGame/FNA e bootstrap |
| [Godot](GODOT.md) | Engine, export, renderer, C# e input |
| [Cocos2d-x](COCOS2D-X.md) | JNI, assets, texto, render e áudio |
| [NXExtract](NXEXTRACT.md) | Receita, dados do dono e instalação limpa |
| [Testes e entrega](TESTES-E-ENTREGA.md) | Evidência, candidato e release |
| [Problemas comuns](PROBLEMAS-COMUNS.md) | Sintomas e diagnóstico dirigido |

## Trilhas especializadas

- [Unity](../../portando_unity/README.md): 15 casos com fontes públicas, dois registros comunitários e ferramentas sintéticas.
- [Freedom Planet 2 — Vulkan para GLES2](../../portando_unity/pt-BR/FP2-VULKAN-GLES2.md): conversão por programa e preservação de dados.
- [Exemplo de shims](../../examples/shims-reference/README.md): contrato pequeno compilável.
- [Licenças e créditos](../../LICENSING.md), [contribuir](../../CONTRIBUTING.md), [validação](../../publication/VALIDATION.md).

## Idiomas e fontes históricas

Toda documentação editorial desta publicação tem versão PT/EN equivalente. Links no topo trocam idioma. Identificadores, caminhos, schemas e comandos permanecem estáveis; exemplos de prompts e explicações são traduzidos.

Arquivos históricos em `framework/`, na árvore NXExtract e em `ports/*/upstream/` conservam os bytes, licenças e idiomas originais. As fichas e os guias bilíngues estão fora dessas árvores imutáveis. Não foram traduzidos comentários de código, mensagens antigas de ferramentas ou textos legais normativos como se fossem nova licença.
