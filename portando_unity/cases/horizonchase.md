# Horizon Chase — Unity 2022.3.33f1

[English](horizonchase.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/horizonchase-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

Sprites e scanout exigem distinguir alpha da textura, precisão de UV e alpha final do backbuffer.

## Solução a estudar

Preservar ETC1 RGB/Alpha8 existentes. Corrigir o material/precisão afetado; quando RGB está correto e alpha final invalida o scanout, alterar somente o alpha antes do swap e restaurar estado.

## Áudio e controles

FMOD mantém música/efeitos; controles usam mapping do firmware. Stream→sample é correção condicionada à falha específica, não default preventivo.

## Limites e contraprova

KMS/SDL deve manter um dono de contexto/surface. Provas de um provider não certificam todos os backends.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [horizonchase-nextos](https://github.com/NextOs-Ports/horizonchase-nextos), commit `59d2d38dc496ae0a71726181d3ccc80923e4144d`. [Manifesto](../../ports/horizonchase-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/horizonchase-nextos/upstream/README.md)
- [third_party/NXExtract/README.md](../../ports/horizonchase-nextos/upstream/third_party/NXExtract/README.md)
- [src/main.c](../../ports/horizonchase-nextos/upstream/src/main.c)
- [src/jni_shim.c](../../ports/horizonchase-nextos/upstream/src/jni_shim.c)
- [src/egl_shim.c](../../ports/horizonchase-nextos/upstream/src/egl_shim.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
