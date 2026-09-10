# Merchant of the Skies — Unity 6000.4.2f1

[English](merchantskies.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/merchantskies-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

TextMeshPro em blocos expôs um contrato de canal: R8 convertido para LUMINANCE produzia alpha constante.

## Solução a estudar

Na rota GLES2 demonstrada, a representação LUMINANCE_ALPHA precisa preservar leituras r/a. Em contexto físico GLES3, storage R8, upload RED e swizzle devem permanecer coerentes; não converter somente um lado.

## Áudio e controles

Uma libaaudio ausente deve falhar como ausente para não selecionar mixer falso. Rewired usa gamepad normalizado; ações A/B dependem do contrato medido.

## Limites e contraprova

Não copiar tabelas de keycodes ou fallback de áudio por semelhança. Preserve pause/save antes do prazo de shutdown quando nativeDone não retorna.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [merchantskies-nextos](https://github.com/NextOs-Ports/merchantskies-nextos), commit `213a3d9875158164bb532ef031651a052ab8360e`. [Manifesto](../../ports/merchantskies-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/merchantskies-nextos/upstream/README.md)
- [src/main.c](../../ports/merchantskies-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/merchantskies-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/merchantskies-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/merchantskies-nextos/upstream/src/egl.c)
- [src/gles3.c](../../ports/merchantskies-nextos/upstream/src/gles3.c)
- [src/unity6_shader.c](../../ports/merchantskies-nextos/upstream/src/unity6_shader.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
