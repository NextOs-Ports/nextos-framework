# Freedom Planet 2 — Unity 2018.4.36f1

[English](fp2.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/fp2-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

A build usa programas Vulkan SMOL-V/SPIR-V. A rota pública prepara programas GLES2/ESSL 1.00 na instalação, preservando a lógica visual necessária do jogo.

## Solução a estudar

Usar tradução elegível exata e escrita cirúrgica nos serializados. StencilDraw/StencilInvert exigem correção estreita do swizzle ARGB→RGBA e do alpha, com identidade por SHA.

## Áudio e controles

FMOD entrega PCM pela SDL do firmware; controles seguem o contrato Android do jogo. Conferir título, número 2, tutorial, carregamento seguinte, HUD e saída.

## Limites e contraprova

Não é um driver Vulkan universal. Variantes não representáveis têm política explícita; não trocar todo shader por um template genérico. Veja o estudo aprofundado de Vulkan nesta edição.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

Leia também [FP2: Vulkan para GLES2](../pt-BR/FP2-VULKAN-GLES2.md).

## Fontes públicas selecionadas

Origem: [fp2-nextos](https://github.com/NextOs-Ports/fp2-nextos), commit `f8caab9d946c39041fa5cc25cb52621590f82adc`. [Manifesto](../../ports/fp2-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/fp2-nextos/upstream/README.md)
- [src/main.c](../../ports/fp2-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/fp2-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/fp2-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/fp2-nextos/upstream/src/egl.c)
- [src/gles3.c](../../ports/fp2-nextos/upstream/src/gles3.c)
- [src/unity6_shader.c](../../ports/fp2-nextos/upstream/src/unity6_shader.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
