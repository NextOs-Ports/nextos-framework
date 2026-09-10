# Sally Face — Unity 2022.3.62f3

[English](sallyface.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/sallyface-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

Chuva magenta exigiu cobrir shaders de cenas além dos globais; cenário/personagem cortados vinham de atlas reduzidos sem retângulos Sprite correspondentes.

## Solução a estudar

Traduzir o conjunto de programas usado e preservar atlas completos/recortes. Reduzir dimensão não é uma consequência obrigatória de ETC1.

## Áudio e controles

O áudio exigia executar a função da thread FMODAudioDevice.run e alimentar a fila PCM. Consultar input.c para o mapa real antes de copiar navegação.

## Limites e contraprova

As notas registram prova no Episódio 1 e não uma campanha completa de todos os episódios. Preservar esse limite.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [sallyface-nextos](https://github.com/NextOs-Ports/sallyface-nextos), commit `39f3d3bd7fed4f160ae20af0f8824ab6b71b79bf`. [Manifesto](../../ports/sallyface-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/sallyface-nextos/upstream/README.md)
- [src/main.c](../../ports/sallyface-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/sallyface-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/sallyface-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/sallyface-nextos/upstream/src/egl.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
