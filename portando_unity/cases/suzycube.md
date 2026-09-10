# Suzy Cube — Unity 2017.4.40f1

[English](suzycube.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/suzycube-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

Os dados desta build já incluem GLES2 e ETC1. O bloqueio anterior à imagem estava em contratos de boot/JNI, incluindo String.length real e __stack_chk_guard como objeto.

## Solução a estudar

Preservar o carregamento de libil2cpp pela Unity durante initJni. A sequência registrada usa nativeRecreateGfxState, foco, resume e nativeRender; não impor os callbacks de uma Unity 2022.

## Áudio e controles

FMOD mantém o mixer; InControl recebe o gamepad normalizado. Conferir eixos já transformados pela SDL, foco e hotplug sem direção retida.

## Limites e contraprova

A prova histórica desta build não cobre toda variante de dados. Não iniciar conversão de shader/textura quando o caminho nativo já fornece os formatos necessários.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [suzycube-nextos](https://github.com/NextOs-Ports/suzycube-nextos), commit `c3db13c749173e08d33d57839aca961d0a2ab319`. [Manifesto](../../ports/suzycube-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/suzycube-nextos/upstream/README.md)
- [src/main.c](../../ports/suzycube-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/suzycube-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/suzycube-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/suzycube-nextos/upstream/src/egl.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
