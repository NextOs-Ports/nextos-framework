# Huntdown — Unity 2022.3.47f1 / 6000.2.6f2

[English](huntdown.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/huntdown-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

Há dois perfis de dados: 200023/Unity 2022 e 200036/Unity 6. A prova de gameplay do primeiro não se transfere ao segundo.

## Solução a estudar

No perfil 2022, medir capacidades de timestamp EGL antes do fallback Swappy e validar assinaturas antes do patch. No perfil 6, JNI proxy, callbacks tipados e fachada/ESSL têm prova limitada de título/menu nas notas selecionadas.

## Áudio e controles

Input segue métodos GamePad específicos do perfil e mapping do firmware. Áudio também varia entre FMOD/OpenSL e a rota AAudio; suporte declarado a dois slots não prova co-op físico.

## Limites e contraprova

Não usar um patch de Swappy 2022 em Unity 6 nem tratar configurações corrigidas por logs como aparelhos fisicamente aprovados.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [huntdown-nextos](https://github.com/NextOs-Ports/huntdown-nextos), commit `7eef080e69a66a09efc005bf3472035eef09248b`. [Manifesto](../../ports/huntdown-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/huntdown-nextos/upstream/README.md)
- [huntdown/README.md](../../ports/huntdown-nextos/upstream/huntdown/README.md)
- [src/main.c](../../ports/huntdown-nextos/upstream/src/main.c)
- [src/jni_shim.c](../../ports/huntdown-nextos/upstream/src/jni_shim.c)
- [src/egl_shim.c](../../ports/huntdown-nextos/upstream/src/egl_shim.c)
- [src/gles3.c](../../ports/huntdown-nextos/upstream/src/gles3.c)
- [src/unity6_shader.c](../../ports/huntdown-nextos/upstream/src/unity6_shader.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
