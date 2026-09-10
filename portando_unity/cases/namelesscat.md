# Nameless Cat — Unity 6000.3.11f1

[English](namelesscat.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/namelesscat-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

Uma SDL privada alterou a integração de vídeo e produziu tela preta apesar do contexto GL vivo.

## Solução a estudar

Usar a SDL do firmware e admitir/configurar o controle na fronteira canônica antes de SDL_Init. Preservar a fachada gráfica e o provider comprovado.

## Áudio e controles

GameController usa GUID/mapping reais; pausa e cursor continuam contextuais. Manter o backend FMOD/AudioTrack/OpenSL do perfil pinado.

## Limites e contraprova

A nota local não liga toda fonte histórica a um único commit aprovado. O snapshot público é uma referência identificada, não prova automática daquele binário.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [namelesscat-nextos](https://github.com/NextOs-Ports/namelesscat-nextos), commit `fb489c406495e30822d50c8250c76810845414d5`. [Manifesto](../../ports/namelesscat-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/namelesscat-nextos/upstream/README.md)
- [generated/namelesscat/README.md](../../ports/namelesscat-nextos/upstream/generated/namelesscat/README.md)
- [src/main.c](../../ports/namelesscat-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/namelesscat-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/namelesscat-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/namelesscat-nextos/upstream/src/egl.c)
- [src/gles3.c](../../ports/namelesscat-nextos/upstream/src/gles3.c)
- [src/unity6_shader.c](../../ports/namelesscat-nextos/upstream/src/unity6_shader.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
