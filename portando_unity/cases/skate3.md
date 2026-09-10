# Skateboard Party 3 — Unity 2022.3.45f1

[English](skate3.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/skate3-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

A build conserva GLES2 e ETC1, com um conjunto residual de ETC2 que precisa de fallback seletivo.

## Solução a estudar

Confirmar a escolha efetiva do backend e o upload de cada formato antes de converter. Logs de analytics não demonstram travamento gráfico.

## Áudio e controles

FMOD/OpenSL entrega áudio à SDL; preservar controles nativos e mapping do firmware. Validar entrada, menus de idade, manobras e saída.

## Limites e contraprova

O nome do repositório é skate3-nextos, mas o jogo é Skateboard Party 3. O nome não identifica automaticamente InControl ou Rewired.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [skate3-nextos](https://github.com/NextOs-Ports/skate3-nextos), commit `18621e90ce7458c3166aa50d5cc508a13e65ee84`. [Manifesto](../../ports/skate3-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/skate3-nextos/upstream/README.md)
- [src/main.c](../../ports/skate3-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/skate3-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/skate3-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/skate3-nextos/upstream/src/egl.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
