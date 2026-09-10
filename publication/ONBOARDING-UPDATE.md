# Atualização do caminho para o primeiro port

[English](ONBOARDING-UPDATE.en.md)

Data: 10/09/2026. Esta atualização implementa o caminho de treino apontado pela [simulação anterior](ONBOARDING-SIMULATION.md). O repositório permanece privado; nenhuma release de jogo ou mudança de visibilidade foi feita.

## O que foi acrescentado

- [SDK público ARM](../toolchains/sdk/README.md), construído a partir de fontes públicas fixadas, com compiladores Linux, SDL2/GLES2 de desenvolvimento, Clang e QEMU.
- [Primeiro port integrado](../examples/first-port/README.md): minijogo autoral, guest Android AArch64, nxloader V5, dois shims, JNI/lifecycle, renderer GLES2, PCM, controle e save. A receita NXExtract prepara seu input autoral do zero.
- [Pins e recuperação de fontes](../docs/pt-BR/PINS-E-FONTES.md): composição dos 15 componentes admitidos pelo helper, procedência histórica preservada e exportação separada dos contratos requeridos pelas ferramentas.
- [Inventário Android](../docs/pt-BR/INVENTARIO-ANDROID.md), [41 perfis pesquisáveis](../catalog/profiles.json) e [diagnóstico com contraprovas](../docs/pt-BR/DIAGNOSTICO-GUIADO.md).
- [Exercícios Mono, Godot, Unity e Cocos2d-x](../examples/engines/README.md), com limites de cada família, e [laboratório SMOL-V/SPIR-V → ESSL 1.00](../examples/shader-lab/README.md), sem shaders comerciais.
- [Workflow manual de CI](../.github/workflows/onboarding.yml) para a coleção e o exemplo autoral. Não publica artefatos nem executa jogos comerciais; ainda não foi executado no GitHub nesta revisão.

Todos os novos guias possuem versões PT/EN. Nenhum arquivo congelado de runtime V5, NXExtract ou snapshot upstream foi alterado. Os 47 títulos continuam sendo referências de catálogo, não 47 novos ports testados.

## Verificações realizadas

| Etapa | Resultado | Limite |
| --- | --- | --- |
| Build do SDK público | PASS | Host Docker linux/amd64; dependências públicas |
| Exemplo C cross AArch64 / ARMv7 | PASS, GLIBC 2.17 / 2.4 | Build/auditoria, sem prova Android ARMv7 |
| Pins da coleção | Create/materialize/verify dos 15 componentes PASS | Seleção exportada; testes históricos omitidos continuam omitidos |
| Primeiro port | 11 verificações integradas PASS | Extração host e execução de CPU AArch64 em QEMU |
| Ferramentas de inventário/catálogo | 10 testes PASS | Inclui imports ELF longos, identidade, caminhos e tipos de símbolos |
| Laboratório de shaders | 5 verificações PASS | Tradução/validação host, sem driver GLES2 físico |
| Mono | P/Invoke, layout, callback e erro PASS | Host Linux gerenciado; não bootstrap Mono Android |
| Godot 3.5.3 | Lógica headless PASS | Movimento/coleta; sem renderização ou export ARM |
| Unity / Cocos2d-x | Fontes de exercício adicionadas | Editor Unity e integração Cocos ainda não compilados |

As 11 verificações integradas cobrem instalação limpa, hook real, saídas verificadas, execução do guest, import ausente antes do construtor, reempacotamento compatível, package/payload/ABI incompatíveis, payload ausente e rollback de hook. O pipeline registra logs e resultado em uma área nova. O teste usa UI desativada explicitamente no host; não substitui a instalação gráfica canônica.

As etapas foram verificadas durante o desenvolvimento. As correções de manifests/geração preservaram os bytes nativos já compilados; não se reconstruiu o executável apenas para atualizar documentação. Nenhum teste descrito aqui certifica imagem, som ou controles no aparelho.

## Identidade dos artefatos de desenvolvimento

| Artefato | Identidade |
| --- | --- |
| Imagem SDK local | `sha256:041f653673aab41aa0c53adc557f31a295a8ac32585357ebfea926383b831094` |
| Pin da composição exportada | `f411e522a861e92486fb307dcfb87d0facbad8c0fde1ebf65c491ffe8cc41832` |
| Executável didático AArch64 | `a6c9142a393cd4d05624840cdc92f186f9354fdc4958e7432bdb6ed3de8aca62` |
| Guest autoral Android AArch64 | `ddab9a2004d1b51f8d7cb86e6e9a1c57f5f643badebf926bfd011a929a5fd35a` |
| Input autoral de treino | `53b00ecaeadf32058f40848874751efc776aa714fb504ead931085102138cf63` |

Esses hashes identificam a execução local registrada. A receita fixa suas fontes/dependências, mas uma imagem reconstruída pode ter outro digest por timestamps de build. Registre os próprios resultados; não copie estes hashes para afirmar que testou seus bytes. Builds, inputs e logs permanecem na área ignorada `work/`.

## O que continua aberto

O launcher/NXExtract/NXSplash gerados foram preparados, mas não testados fisicamente nesta tarefa. O adapter de geração permanece marcado como scaffold/não release. A prova de extração gráfica, pixels, áudio, input, persistência e saída precisa ser feita no alvo explicitamente autorizado antes de qualquer entrega de port.

O SMOL-V público recuperado coincide com os hashes exigidos pelo FP2. O commit SPIRV-Cross histórico do FP2 não pôde ser obtido do upstream Khronos nesta revisão; sua origem exata continua pendente. O laboratório usa outro pin público declarado e não certifica reconstrução idêntica do tradutor aprovado.

Unity e Cocos precisam dos builds de editor/engine indicados; referências históricas ainda podem exigir arquivos externos e adaptações próprias. Não existe alegação de shims cobrindo a maioria dos jogos. Licenças, revisão editorial e aprovação para tornar público continuam em [REVIEW.md](REVIEW.md).
