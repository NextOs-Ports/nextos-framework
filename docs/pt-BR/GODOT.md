# Portar jogos Godot do Android para Linux ARM

[English](../en/GODOT.md)

O primeiro objetivo é identificar a versão, o formato do projeto e as extensões que o runtime precisa. Um APK Godot pode permitir uma rota com engine Linux compatível, mas um PCK isolado não garante que o jogo dispense plugins Android, C# ou bibliotecas nativas.

## 1. Fazer o inventário

Registre Godot major/minor/patch e se é build modificada; GDScript ou C#; PCK/projeto exportado; recursos importados; plugins Android; GDExtension/GDNative; renderer; dependências de áudio, vídeo e controle. Confirme a versão pelos dados/runtime, não pelo nome da pasta.

| Achado | Consequência |
| --- | --- |
| Godot 3 com GLES2 | Investigar template compatível dessa geração |
| Godot 4 Compatibility | Não presumir GLES2 físico no driver padrão |
| Forward+/Mobile | Recursos podem exceder a rota GL do alvo |
| C# | Pin da engine, assemblies e runtime .NET compatíveis |
| Plugin Android | Implementar/adaptar contrato ou registrar bloqueio |
| Extensão nativa | Precisa de build Linux/ABI compatível ou solução equivalente autorizada |

Godot diferencia método de render e driver gráfico; Compatibility usa a família OpenGL, enquanto Forward+/Mobile usam RenderingDevice. Trocar o método pode alterar recursos e aparência. [Visão oficial dos renderers](https://docs.godotengine.org/en/stable/tutorials/rendering/renderers.html).

## 2. Escolher a rota sem perder compatibilidade

Quando você possui o projeto-fonte, prepare uma exportação Linux para a arquitetura correta, com template e recursos correspondentes. O sistema de exportação combina o projeto em PCK com um executável; a arquitetura deve ser escolhida explicitamente. [Exportação Linux do Godot](https://docs.godotengine.org/en/stable/tutorials/export/exporting_for_linux.html).

Quando dispõe somente da cópia Android do dono, inventarie o conteúdo exportado e determine se uma engine Linux exata pode consumi-lo. Não invente fontes, chaves, plugins ou compatibilidade de formato. GDScript compilado, importações de textura e assemblies podem prender a build a uma versão específica.

## 3. Ler a referência pública

[Tearscape](../../ports/tearscape-nextos/README.md) contém fontes de integração Godot 4.6.1/.NET, providers de vídeo, controles e preparação de dados. Leia o README do snapshot, `build_low_glibc.sh`, `src/shim/` e os arquivos listados no manifesto antes de aproveitar um trecho.

O snapshot registra pendências físicas em revisões recentes. Sua presença aqui não o promove a baseline universal aprovado. Use a arquitetura como estudo e preserve qualquer artefato já aprovado no port que está sendo trabalhado. Uma rota GLES2 fisicamente comprovada deve entrar no build oficial e seus pins; não deixá-la numa pasta experimental que o empacotador ignora.

## 4. Preparar engine e build

Separe engine Linux, bibliotecas Linux, runtime .NET quando necessário e dados do dono. Fixe commit da engine e patches, SDK/sysroot, configurações do build e hashes das dependências. Confira arquitetura e GLIBC de todos os ELFs Linux com o [guia ARM](COMPILAR-ARM.md).

Não execute o build de Tearscape apenas para experimentar: sua receita tem entradas congeladas e regra de build único. No novo port, leia primeiro as opções do sistema de build da versão exata e crie uma receita independente. A coleção não contém um SDK Godot ARM universal pronto nem um PCK de demonstração comercial.

Para um **projeto autoral** já preparado e uma instalação compatível do editor, esta é a forma de consultar e usar a CLI; `Linux ARM64` precisa existir como preset e ter o template correto instalado:

```sh
export NEXTOS_GODOT=/opt/godot/godot
"$NEXTOS_GODOT" --version
"$NEXTOS_GODOT" --help
mkdir -p work/godot-export
"$NEXTOS_GODOT" --headless --path "$PWD/work/godot-project" \
  --export-release "Linux ARM64" "$PWD/work/godot-export/demo-nextos"
```

Esse comando é condicionado ao projeto/preset: não roda a partir de um APK sem preparação. Exportar por CLI é diferente de compilar o engine. [Referência oficial de linha de comando](https://docs.godotengine.org/en/stable/tutorials/editor/command_line_tutorial.html).

## 5. Preservar janela, viewport e imagem

Meça tamanho do painel/drawable, janela, viewport lógico e retângulo de conteúdo separadamente. Alterar a altura lógica pode alterar o zoom da câmera do jogo. Letterbox ou stretch deve respeitar o comportamento aprovado e a opção explícita do usuário.

Em Mali-450, mantenha GLES2 físico. Uma fachada de tradução exige cobertura real de shaders, texturas, FBO, blend e operações de cópia; mudar uma string GL não implementa o renderer. Compare título, gameplay, transições, transparências e efeitos. Tela preta com áudio é falha gráfica terminal.

## 6. Input, áudio, C# e saves

As ações de controle chegam aos sinks reais de `InputMap`. Se um mapa se apresenta como editável, prove que editar uma ação muda o comportamento em runtime; não basta embarcar o arquivo. Preserve identidade de controles, co-op e troca de contexto.

Teste o provider de áudio e o shutdown. Em C#, valide versão .NET, BCL, assemblies e P/Invoke; veja [Mono/.NET](MONO-ANDROID.md) para distinguir runtimes. Preserve `user://` e migrações de save; dados preparados e saves têm ciclos diferentes.

## 7. Entrega verificável

Crie [receita NXExtract](NXEXTRACT.md) para o projeto exportado, assemblies e transformações estritamente necessárias. Teste instalação limpa com input completo, depois cena inicial, progressão, transição, áudio, controle, save/reload e saída. Registre versão da engine, SHA dos executáveis, perfil de dados e dispositivos efetivamente testados.

## Missão para a IA

```text
Identifique Godot e o formato exportado, renderer, C#/extensões e plugins
Android. Escolha uma engine Linux compatível e fixe fonte, patches e runtime.
Implemente o provider/adaptações no novo port preservando a rota GLES2 aprovada,
viewport e InputMap. Documente dados exigidos, build, NXExtract e provas reais.
```
