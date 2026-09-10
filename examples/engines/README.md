# Exercícios autorais por engine

[English](README.en.md)

Exemplos pequenos para investigar uma fronteira de cada família. São projetos de treino, não ports comerciais nem substitutos do [exemplo integrado ARM64](../first-port/README.md). Copie projetos de editor para `work/` antes de abri-los, para manter caches e builds fora das fontes.

## Mono: layout, callback e P/Invoke

[Program.cs](mono/Program.cs) chama a biblioteca autoral [native.c](mono/native.c), com estrutura sequencial, Cdecl, callback mantido vivo e propagação de erro. Prepare Mono/mcs e compilador C no host:

```sh
mkdir -p work/mono-training
cc -shared -fPIC -O2 examples/engines/mono/native.c   -o work/mono-training/libnextos_training.so
mcs -out:work/mono-training/Training.exe examples/engines/mono/Program.cs
mono work/mono-training/Training.exe
```

Resultado observado: `PASS: managed/native layout, callback lifetime and error propagation`. Este exercício é um host Linux gerenciado. **Não implementa Mono Android/Xamarin, assembly store, Bionic ou Activity.** Essas etapas estão no [guia Mono Android](../../docs/pt-BR/MONO-ANDROID.md); a distinção entre as duas rotas é parte da aula.

## Godot: projeto autoral e lógica testável

Use [Godot 3.5.3 oficial](https://godotengine.org/download/archive/3.5.3-stable/). Defina `NEXTOS_GODOT` como caminho para seu executável. A versão headless testa a lógica; o editor/runner gráfico abre o projeto com GLES2.

```sh
mkdir -p work/engine-training
cp -R examples/engines/godot work/engine-training/godot
"$NEXTOS_GODOT" --path work/engine-training/godot --script test_model.gd
```

Resultado observado: PASS da lógica de movimento, limite de delta e coleta. Para jogar, abra a cópia pelo editor Godot 3.5.3 ou execute o runner gráfico com `--path` e sem `--script`. Setas movem; Escape salva/sai. Não use o binário headless para alegar imagem. O arquivo [project.godot](godot/project.godot) fixa GLES2; a exportação Linux/ARM exige templates correspondentes e os testes descritos no [guia Godot](../../docs/pt-BR/GODOT.md).

## Unity: criar o próprio input Android

Instale e licencie Unity 2019.4.40f1 com módulos Android/IL2CPP e suas ferramentas suportadas. Defina `NEXTOS_UNITY_EDITOR` como caminho do editor. O projeto contém somente scripts autorais:

```sh
mkdir -p work/engine-training
cp -R examples/engines/unity work/engine-training/unity
"$NEXTOS_UNITY_EDITOR" -batchmode -quit   -projectPath "$PWD/work/engine-training/unity"   -executeMethod TrainingBuild.Android -logFile -
```

[TrainingBuild.cs](unity/Assets/Editor/TrainingBuild.cs) cria a cena, seleciona IL2CPP/AArch64/GLES2 e gera `Build/training.apk` na cópia de trabalho. [Training.cs](unity/Assets/Training.cs) desenha o jogo e salva via PlayerPrefs. **Esse build de editor não foi executado nesta revisão.** A instalação do editor/módulos/licença não faz parte do SDK C. Portar esse input continua exigindo investigar Unity e implementar seu adapter; ele não é o guest freestanding da primeira aula.

## Cocos2d-x: cena para integrar ao template

[TrainingScene.cpp](cocos/TrainingScene.cpp) e [header](cocos/TrainingScene.h) implementam desenho, teclado, coleta e UserDefault. Use o template C++ upstream de Cocos2d-x 3.17.2, commit `1528ea01d2749b4ef65e97f79cffa6135fe13c4d`, em um projeto novo com suas dependências. A [receita CMake upstream](https://github.com/cocos2d/cocos2d-x/blob/cocos2d-x-3.17.2/templates/cpp-template-default/CMakeLists.txt) identifica os arquivos que entram no alvo.

Copie os dois arquivos para `Classes/`; acrescente `Classes/TrainingScene.cpp` a `GAME_SOURCE` e o header a `GAME_HEADER`. Em `AppDelegate.cpp`, inclua `TrainingScene.h` e substitua a criação da cena inicial por `TrainingScene::create()`. Use design resolution 640×480 e preserve a criação de GLView/lifecycle do template. Construa o projeto pelo CMake do template; Android também exige o projeto Gradle/NDK da versão da engine.

**A integração Cocos não foi compilada nesta revisão.** Ela precisa do checkout/dependências upstream e de teste gráfico próprio. O [guia Cocos2d-x](../../docs/pt-BR/COCOS2D-X.md) cobre a adaptação Android, JNI, texto e áudio que uma cena autoral isolada não demonstra.
