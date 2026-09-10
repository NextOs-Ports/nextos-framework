# NextOS Framework

Base V5 para criar ports Android em Linux ARM, acompanhada de fontes de **45 títulos de referência** e guias para trabalhar com ajuda de IA.

**Rascunho privado para revisão. A publicação pública depende de aprovação do mantenedor NextOS.** As referências são seleções de fontes; não são 45 novos pacotes instaláveis nem uma declaração de suporte universal. Nenhum dado comercial de jogo acompanha esta coleção.

Autoria da coleção e integração: **NextOS** · [GitHub oficial](https://github.com/NextOs-Ports).

## Comece aqui

1. [Guia para portar com IA](docs/pt-BR/PORTAR-COM-IA.md).
2. [Compilar para ARM e AArch64](docs/pt-BR/COMPILAR-ARM.md).
3. [Shims: exemplo executável e mapa de referências](examples/shims-reference/README.md).
4. [Catálogo dos jogos e códigos](catalog/README.md).
5. [Licenças, créditos e redistribuição](LICENSING.md).

Leia também o [guia de arquitetura e limites](docs/pt-BR/ARQUITETURA.md). [English overview](README.en.md).

## O que há nesta árvore

| Caminho | Conteúdo |
| --- | --- |
| `framework/` | Fontes, contratos, templates e auxiliares preservados da V5 |
| `suportando_outros_devices/extrator-universal/` | NXExtract: engine, runner e UI gráfica |
| `ports/` | 41 repositórios de referência, representando 45 títulos |
| `examples/shims-reference/` | Exemplo C compilável de resolução explícita e shims tipados |
| `catalog/ports.json` | Índice para humanos e IA, com origem, commit, plataforma e limites |
| `toolchains/` | Configuração CMake para usar um sysroot Linux escolhido explicitamente |
| `publication/` | Manifesto da exportação, verificação e pendências antes de publicar |

O núcleo provém de `framework-v5`, commit `657fb65a23b5c3b20040e76307b27e6470b1d17c`. Os bytes exportados constam em `publication/v5-export.json`. Doze testes que contêm dependências privadas não foram transportados; a suíte histórica completa não é anunciada como autossuficiente neste repositório. O código de runtime incluído foi preservado.

Cada port mantém sua origem e seus próprios pins. Estar nesta coleção não migra uma referência V3/V4 para V5. Um novo port deve nascer em diretório próprio; alterações ao núcleo compartilhado pertencem à linha futura V6.

## Contribuição e dados

Use a IA para inventariar imports, localizar soluções, escrever o adapter, compilar e executar os testes permitidos. O dono fornece sua cópia compatível do jogo e valida os resultados físicos necessários. Nunca envie APK, IPA, OBB, bibliotecas proprietárias, assets ou saves para commits, issues ou artefatos de CI.

É permitido reutilizar e redistribuir conforme a licença de cada componente, preservando os avisos de autoria e licença, inclusive NextOS e terceiros, e a fonte correspondente quando exigida. A condição não comercial desejada pelo mantenedor está documentada em `LICENSING.md`; ela não substitui as permissões GPL/MIT já existentes.
