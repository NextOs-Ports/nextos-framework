# Laboratório de shaders Vulkan → GLES2

[English](README.en.md)

Uma aula reproduzível com shaders autorais, inspirada no método documentado para Freedom Planet 2. Não contém shaders extraídos de jogos, arquivos Unity ou offsets comerciais.

## 1. Preparar as ferramentas

No host Linux, use Git, Python 3.11+, compilador C++17 e `glslangValidator`. A execução verificada usou glslang 16.4.0. O script registra a versão real usada; outra versão exige rever o resultado. As fontes SPIRV-Cross e SMOL-V são fixadas por commit em [sources.json](sources.json).

```sh
python3 examples/shader-lab/run.py --fetch   --deps work/shader-dependencies --output work/shader-results
```

`--fetch` autoriza downloads dos dois repositórios públicos declarados. Sem a opção, fontes ausentes geram erro. Diretórios existentes precisam ter o commit exato e estar limpos. O build não altera o tradutor do FP2 nem instala dependências globalmente.

## 2. Seguir a transformação

`color.vert` e `color.frag` geram SPIR-V Vulkan 1.0. O [tradutor autoral](translate.cpp) comprime em SMOL-V, descomprime e compara todos os bytes antes de traduzir para ESSL 1.00. O fragment shader preserva alpha e premultiplica RGB explicitamente. O glslang valida o texto resultante.

`unsupported.comp` é a contraprova: compute não entra na rota GLES2 da aula. O tradutor retorna erro e não cria saída. Uma falha não recebe shader genérico. `RESULT.json` registra cinco verificações, ferramentas e pins.

## 3. Distinguir os pins do FP2

O commit SMOL-V `4b52c165c13763051a18e80ffbc2ee436314ceb2` reproduz exatamente os hashes de `smolv.cpp`/`.h` exigidos pelo script histórico do FP2.

O SPIRV-Cross histórico `eb32b288ea553e938005fcfd819a2290b1c8032d` não pôde ser obtido do upstream Khronos nesta revisão (`not our ref`). A aula usa outro commit público explicitamente fixado. **Isso não substitui o pin do FP2 nem prova reconstrução idêntica do seu tradutor.** Para reconstruir o FP2 aprovado, recupere a origem exata daquele pin; não remova sua checagem. O script histórico também exige SPIRV-Tools 2026.3.1, que esta aula independente não utiliza.

## 4. Levar o método a um port

Tradução host não prova compilação pelo driver, bindings em Unity, stencil, pixels, variantes ou gameplay. Acrescente um teste GLES2 físico no alvo autorizado. O [estudo FP2](../../portando_unity/pt-BR/FP2-VULKAN-GLES2.md) descreve as fronteiras adicionais. Referências: [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), [SMOL-V](https://github.com/aras-p/smol-v).
