# Dead Trigger — Unity 2019.4 (patch a confirmar / patch to confirm)

[English](deadtrigger.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/deadtrigger-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

Uma tela preta após missão vinha de exceção num popup de controle obsoleto, interrompendo a atualização de UI.

## Solução a estudar

O reparo é restrito aos corpos Init/Show identificados e precisa rejeitar endereços compartilhados por folding IL2CPP. Preservar contexto GLES2 e lifecycle.

## Áudio e controles

OpenSL/BufferQueue deve manter objetos, vtables e PCM esperados por FMOD. Conferir primeira recarga, menus, movimento, áudio e persistência.

## Limites e contraprova

A nota local identifica Unity 2019.4 sem patch completo. Não inventar o patch nem confundir o baseline histórico com revisões posteriores do repositório.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [deadtrigger-nextos](https://github.com/NextOs-Ports/deadtrigger-nextos), commit `8fe021e4188db177a074547473d7723e060748fe`. [Manifesto](../../ports/deadtrigger-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/deadtrigger-nextos/upstream/README.md)
- [src/main.c](../../ports/deadtrigger-nextos/upstream/src/main.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
