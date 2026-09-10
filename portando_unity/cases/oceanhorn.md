# Oceanhorn: Chronos Dungeon — Unity 2022.3.61f1

[English](oceanhorn.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/oceanhorn-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

Boot e sprites dependiam de distinguir ponteiro nativo JNIBridge de GCHandle e da fonte correta de alpha do atlas.

## Solução a estudar

Preservar variantes GLES2 nativas e o framing do framebuffer. Não ativar a redução experimental de render como default desta referência.

## Áudio e controles

Rewired consome KeyEvent/MotionEvent. O fallback de FMOD para carregamento residente deve depender da falha do worker assíncrono, com memória e áudio medidos.

## Limites e contraprova

Este título é Chronos Dungeon. Não confundir versões e soluções de outros jogos da mesma franquia.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [oceanhorn-nextos](https://github.com/NextOs-Ports/oceanhorn-nextos), commit `7cb05cca86afbb4b8a6e76aa251c8f0f8397bc9e`. [Manifesto](../../ports/oceanhorn-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/oceanhorn-nextos/upstream/README.md)
- [src/main.c](../../ports/oceanhorn-nextos/upstream/src/main.c)
- [src/jni_shim.c](../../ports/oceanhorn-nextos/upstream/src/jni_shim.c)
- [src/egl_shim.c](../../ports/oceanhorn-nextos/upstream/src/egl_shim.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
