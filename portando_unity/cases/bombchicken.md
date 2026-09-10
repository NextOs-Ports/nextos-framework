# Bomb Chicken — Unity 2022.3.39f1

[English](bombchicken.en.md)

[Índice Unity](../README.md) · [Ficha do código NextOS](../../ports/bombchicken-gunbrick-nextos/README.md)

Perfil das notas selecionadas: IL2CPP/AArch64. Esta é uma adaptação editorial do estudo `portando_unity`, vinculada a fontes públicas. O texto descreve causas e soluções históricas; a inclusão não revalida o snapshot nem certifica todos os aparelhos.

## Sintoma e fronteira

Tela preta podia vir de exceção em LevelStart.Awake causada por retorno JNI incorreto; outra falha no fim da fase vinha de Progress malformado.

## Solução a estudar

Preservar GLES2/stencil8 e reparar apenas o contrato JNI/persistência demonstrado. Não trocar o renderer para tratar uma exceção de lógica.

## Áudio e controles

O mixer Unity/FMOD usa a ponte OpenSL/SDL. Chamadas IL2CPP de input precisam de overload, typedef e MethodInfo corretos para a build.

## Limites e contraprova

Esta ficha é de Bomb Chicken; o mesmo repositório também contém Gunbrick, que possui seu próprio perfil. Não transferir offsets entre eles.

Ao adaptar, recalcule assinaturas/offsets para o payload do dono. Compare antes/depois na mesma cena, preserve a referência e registre o hash do novo artefato. Um teste host ou um título visível não prova gameplay completo.

## Fontes públicas selecionadas

Origem: [bombchicken-gunbrick-nextos](https://github.com/NextOs-Ports/bombchicken-gunbrick-nextos), commit `53df6041da4ecc4905af5ecd24551034eea21175`. [Manifesto](../../ports/bombchicken-gunbrick-nextos/SOURCE-MAP.json); [proveniência da edição Unity](../SOURCE-MAP.json).

- [README.md](../../ports/bombchicken-gunbrick-nextos/upstream/README.md)
- [docs/images/README.md](../../ports/bombchicken-gunbrick-nextos/upstream/docs/images/README.md)
- [ports/bombchicken/src/main.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/bombchicken/src/main.c)
- [ports/gunbrick/src/main.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/gunbrick/src/main.c)
- [ports/bombchicken/src/jni.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/bombchicken/src/jni.c)
- [ports/gunbrick/src/jni.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/gunbrick/src/jni.c)
- [ports/bombchicken/src/input.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/bombchicken/src/input.c)
- [ports/gunbrick/src/input.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/gunbrick/src/input.c)
- [ports/bombchicken/src/egl.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/bombchicken/src/egl.c)
- [ports/gunbrick/src/egl.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/gunbrick/src/egl.c)

Autoria do port/integração: **NextOS**. Preserve licenças e créditos de terceiros; dados do jogo não acompanham as fontes.
