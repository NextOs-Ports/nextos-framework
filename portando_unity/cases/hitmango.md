# Hitman GO — Unity 2022.3.67f2

[English](hitmango.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/hitmango-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

O caminho GLES2 nativo exige tratamento seletivo de formatos não suportados e alpha final compatível com o compositor.

## Solução a estudar

Preservar ETC1 suportado; decodificar ETC2/EAC somente quando necessário. Confirmar pixels RGB e alpha antes de aplicar a correção de backbuffer.

## Áudio e controles

Uma thread nativa reproduz FMODAudioDevice/AudioTrack sem substituir o mixer. InControl governa movimento no tabuleiro; cursor e atalhos são contextuais.

## Limites e contraprova

Não transformar esta rota de gamepad em touch global. Revalidar capítulos, saves e saída no artefato novo.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [hitmango-nextos](https://github.com/NextOs-Ports/hitmango-nextos), commit `3f735165348e17d4f9aa20f1d227a9b1ecb4efb8`. [Manifesto](../../ports/hitmango-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/hitmango-nextos/upstream/README.md)
- [hitmango/README.md](../../ports/hitmango-nextos/upstream/hitmango/README.md)
- [src/main.c](../../ports/hitmango-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/hitmango-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/hitmango-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/hitmango-nextos/upstream/src/egl.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
