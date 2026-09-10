# Party Hard GO — Unity 6000.3.10f1

[English](partyhard.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/partyhard-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

Layout incorreto dos dados impedia cenas; depois, D-pad entregue por KeyEvent e HAT simultaneamente causava movimento preso.

## Solução a estudar

Preservar assets/bin/Data e lifecycle. Usar uma autoridade direcional e soltar ações ao mudar contexto/foco ou desconectar. SharedPreferences.apply pode agrupar escrita mantendo commit/pause/saída duráveis.

## Áudio e controles

InControl governa gameplay; cursor no analógico direito/R3 atende o diálogo de controles. FMOD/AudioTrack mantém mixer e contagem de frames.

## Limites e contraprova

A nota distingue o fechamento público 1.0.6/V5 de builds anteriores. Conferir a revisão do snapshot antes de atribuir essa prova ao HEAD observado.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [partyhard-nextos](https://github.com/NextOs-Ports/partyhard-nextos), commit `0a03b8483ac9c84d3be63c8dd898cf5acd305576`. [Manifesto](../../ports/partyhard-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/partyhard-nextos/upstream/README.md)
- [src/main.c](../../ports/partyhard-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/partyhard-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/partyhard-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/partyhard-nextos/upstream/src/egl.c)
- [src/gles3.c](../../ports/partyhard-nextos/upstream/src/gles3.c)
- [src/unity6_shader.c](../../ports/partyhard-nextos/upstream/src/unity6_shader.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
