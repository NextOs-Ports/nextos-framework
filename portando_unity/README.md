# Portando Unity — edição selecionada NextOS

[English](README.en.md)

Esta edição bilíngue adapta o estudo `portando_unity` para **15 casos com repositório público**, com duas ferramentas genéricas e fixtures sintéticas. Dois ports adicionais têm fichas de distribuição comunitária explicitamente autorizadas. As fontes e receitas dos demais jogos locais não foram incluídas.

## Ordem de leitura

1. [Triagem da build](pt-BR/TRIAGEM.md): identidade, runtime, contratos e escolha de referência.
2. [Gráficos e texturas](pt-BR/GRAFICOS-E-TEXTURAS.md): GLES2, ETC1/dual, alpha, escala e memória.
3. [Input e áudio](pt-BR/INPUT-E-AUDIO.md): consumidores reais, toque curto e lifecycle.
4. [Diagnóstico por fronteira](pt-BR/DIAGNOSTICO.md): sintoma, medição, causa e contraprova.
5. [Freedom Planet 2: Vulkan para GLES2](pt-BR/FP2-VULKAN-GLES2.md): estudo aprofundado com fontes públicas.

## Casos com fontes públicas

| Port NextOS | Unity nas notas selecionadas | Leitura |
| --- | --- | --- |
| Suzy Cube | 2017.4.40f1 | [Caso](cases/suzycube.md) |
| Freedom Planet 2 | 2018.4.36f1 | [Caso](cases/fp2.md) |
| Dead Trigger | 2019.4 (patch a confirmar / patch to confirm) | [Caso](cases/deadtrigger.md) |
| Terraria | 2021.3.56f2 | [Caso](cases/terraria.md) |
| Horizon Chase | 2022.3.33f1 | [Caso](cases/horizonchase.md) |
| Bomb Chicken | 2022.3.39f1 | [Caso](cases/bombchicken.md) |
| Prizefighters 2 | 2022.3.62f2 | [Caso](cases/pf2.md) |
| Oceanhorn: Chronos Dungeon | 2022.3.61f1 | [Caso](cases/oceanhorn.md) |
| Hitman GO | 2022.3.67f2 | [Caso](cases/hitmango.md) |
| Huntdown | 2022.3.47f1 / 6000.2.6f2 | [Caso](cases/huntdown.md) |
| Skateboard Party 3 | 2022.3.45f1 | [Caso](cases/skate3.md) |
| Sally Face | 2022.3.62f3 | [Caso](cases/sallyface.md) |
| Merchant of the Skies | 6000.4.2f1 | [Caso](cases/merchantskies.md) |
| Nameless Cat | 6000.3.11f1 | [Caso](cases/namelesscat.md) |
| Party Hard GO | 6000.3.10f1 | [Caso](cases/partyhard.md) |

Cada ficha aponta para o commit público selecionado no catálogo. Os resultados históricos da nota não certificam automaticamente esse HEAD. Huntdown mantém os perfis 2022 e 6000.x separados; título/menu do segundo não vira gameplay aprovado.

## Casos autorizados por distribuição comunitária

- [Stranger Things 3: The Game](../catalog/community/strangerthings3.md): Unity 6000.2.6f2, ficha técnica sem snapshot de código.
- [AVGN I & II Deluxe](../catalog/community/avgn12deluxe.md): Unity 2019.4.16f1 e referência V6 BYO-data; não altera a V5.

Não há outros títulos importados por analogia ou por estarem em um arquivo local. Todos são ports NextOS; componentes de terceiros mantêm créditos e licenças próprios.

## Ferramentas e limites

[Planejamento ETC1](diagnostico/texturas/README.md) e [toque curto](diagnostico/toque/README.md) são ferramentas offline. Não conectam aparelhos, não executam jogos e não geram aprovação física. Seus códigos e fixtures preservam hashes; os guias têm versões completas PT/EN.

[SOURCE-MAP.json](SOURCE-MAP.json) registra origem/hash das notas selecionadas, correspondência com código público e arquivos genéricos copiados. Guias transversais foram adaptados para retirar referências a jogos fora da seleção. Galerias, dados, logs e provas privadas não acompanham esta edição.

Para iniciar um port, leia [o guia de IA](../docs/pt-BR/PORTAR-COM-IA.md) e [o build ARM](../docs/pt-BR/COMPILAR-ARM.md). Preserve a V5, SDL do firmware, fluxo nativo e as interfaces NXExtract/NXSplash. Mudança de comportamento compartilhado pertence à linha V6, não a esta edição de documentação.
