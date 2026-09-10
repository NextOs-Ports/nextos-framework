# Terraria — Unity 2021.3.56f2

[English](terraria.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/terraria-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

A engine podia emitir zero GL quando eglGetProcAddress não roteava funções EGL e a configuração solicitada era recusada no alvo.

## Solução a estudar

Preservar identidade EGL e uma configuração válida do contexto SDL, com nativeRender dirigido pelo host. Medir o GfxDevice e primeiro present.

## Áudio e controles

A taxa informada por fmodGetInfo e o tamanho do DirectByteBuffer precisam corresponder aos frames realmente produzidos. InControl deve receber uma única rota de gamepad, neutralizada durante o teclado de texto quando necessário.

## Limites e contraprova

Não promover workarounds históricos de jobs/GC ao framework. A classificação desta ficha vem da build Android examinada, não de outras edições do jogo.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [terraria-nextos](https://github.com/NextOs-Ports/terraria-nextos), commit `e04f6fe7d3be9591e659f6ecd42355d1b5d2caf6`. [Manifesto](../../ports/terraria-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/terraria-nextos/upstream/README.md)
- [package/universal/README.md](../../ports/terraria-nextos/upstream/package/universal/README.md)
- [src/main.c](../../ports/terraria-nextos/upstream/src/main.c)
- [src/jni_shim.c](../../ports/terraria-nextos/upstream/src/jni_shim.c)
- [src/egl_shim.c](../../ports/terraria-nextos/upstream/src/egl_shim.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
