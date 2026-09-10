# Prizefighters 2 — Unity 2022.3.62f2

[English](pf2.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/prizefighters2-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

A build precisa de variantes de shader GLES2 compatíveis e possui menus que consomem Mouse managed.

## Solução a estudar

Traduzir os programas necessários e preservar Built-in/Gamma. No cursor, converter posição Y e delta Y uma única vez para o espaço real do sink; não fixar 720 em um port de resolução variável.

## Áudio e controles

O Input System usa StateEvent/GamepadState específicos; o mouse é contextual. fmodGetInfo/fmodProcess e a fila PCM devem preservar o mixer.

## Limites e contraprova

A solução não é uma receita URP.renderScale. Identidade e hooks do payload exigem validação própria.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [prizefighters2-nextos](https://github.com/NextOs-Ports/prizefighters2-nextos), commit `53371b37dc5d01e0a8c09d683536ee1c48f35d6c`. [Manifesto](../../ports/prizefighters2-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/prizefighters2-nextos/upstream/README.md)
- [src/main.c](../../ports/prizefighters2-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/prizefighters2-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/prizefighters2-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/prizefighters2-nextos/upstream/src/egl.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
