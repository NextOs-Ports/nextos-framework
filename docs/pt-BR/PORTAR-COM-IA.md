# Criar um port com a IA conduzindo o trabalho

[English](../en/AI-PORTING.md)

Antes de inventar ferramentas novas, use o [inventário Android](INVENTARIO-ANDROID.md), os [pins e fontes](PINS-E-FONTES.md) e o [diagnóstico guiado](DIAGNOSTICO-GUIADO.md). O [primeiro port](../../examples/first-port/README.md) dá à IA uma execução demonstrada para comparar cada fronteira.

A IA deve investigar, escrever código, compilar, executar verificações autorizadas e deixar resultados reproduzíveis. O dono fornece os inputs compatíveis, escolhe o alvo, relata a experiência física necessária e aprova a publicação. Não é preciso responder a uma pergunta a cada ajuste local reversível.

## 1. Entregar uma missão concreta

Copie o modelo e preencha apenas o que souber. Um campo desconhecido vira tarefa de investigação, não licença para inventar dados:

```text
Leia AGENTS.md e docs/pt-BR/README.md deste clone.
Jogo e versão: [informar]
Cópia Android local fornecida por mim: [caminho privado]
Alvo desta etapa: [sistema, CPU/GPU e ABI de userland]
Repositório/diretório novo: [destino]
Primeiro objetivo: identificar a build e alcançar o fluxo nativo no alvo.

Conduza autonomamente inventário, escolha das referências públicas,
implementação do adapter, build e testes locais pertinentes.
Prefira AArch64. Preserve a V5 e todos os ports de referência.
Se for Unity, leia portando_unity/README.md e use somente seus casos públicos.
Leia docs/pt-BR/ANDROID-RUNTIMES.md e escolha uma das oito trilhas Android.
Use somente os ports públicos já selecionados, com commit e evidência fixados.

Mantenha fontes, pins, contratos, logs privados e resultados organizados.
Não invente offsets, assinaturas, suporte, licença ou sucesso de APIs ausentes.
Implemente e teste o que puder; descreva precisamente qualquer bloqueio.
Não envie dados do jogo ao GitHub. Não acesse aparelho sem endereço autorizado
nesta tarefa. Prepare materiais revisáveis antes de pedir aprovação para publicar.
```

## 2. Fazer o inventário antes de escolher o loader

Crie um relatório privado com jogo/versão, package ID, tamanho/SHA do container de referência, ABIs, bibliotecas críticas e respectivos hashes, engine/runtime real, dependências, caminhos de assets, shaders/texturas, áudio e input. Em splits, identifique todos os containers necessários. Não confunda a versão do jogo com a versão da engine.

O comando abaixo apenas lista bibliotecas dentro de um APK informado; não extrai nem executa seu conteúdo. Package ID e versão devem vir de um leitor de manifesto Android binário apropriado; `strings` sozinho não comprova esses campos.

```sh
export NEXTOS_OWNER_APK=/private/owner-input/game.apk
python3 - <<'PY'
import os
from zipfile import ZipFile
with ZipFile(os.environ['NEXTOS_OWNER_APK']) as apk:
    for item in apk.infolist():
        if item.filename.startswith('lib/') and item.filename.endswith('.so'):
            print(item.filename, item.file_size)
PY
```

Não comite a saída bruta se ela trouxer informações privadas. Leia dados comerciais somente no ambiente autorizado; não os envie como anexos para serviços externos de IA.

## 3. Escolher a referência por contrato

Abra o catálogo e `SOURCE-MAP.json`. Registre a razão da escolha: engine/build, ABI, JNI, áudio, renderer, input e licença compatíveis. Uma mesma função pode ter assinatura diferente em outra build. Verifique se a implementação citada está nos arquivos selecionados e se a prova se refere ao mesmo artefato.

| Encontrado | Trilha |
| --- | --- |
| Unity (Mono ou IL2CPP) | [Portando Unity](../../portando_unity/README.md) |
| Mono/.NET Android, incluindo MonoGame/FNA da build Android | [Mono Android](MONO-ANDROID.md) |
| Projeto/runtime Godot | [Godot](GODOT.md) |
| Cocos2d-x e callbacks Android | [Cocos2d-x](COCOS2D-X.md) |
| GameMaker Android | [GameMaker Android](GAMEMAKER-ANDROID.md) |
| Ren’Py Android | [Ren’Py Android](RENPY-ANDROID.md) |
| Haxe/hxcpp/Lime Android | [Haxe/hxcpp/Lime Android](HAXE-LIME-ANDROID.md) |
| C/C++ Android | [C/C++ Android](NATIVE-ANDROID.md) |

## 4. Organizar entregas pequenas e verificáveis

No repositório do **novo** port, mantenha um inventário, uma tabela de contratos, fonte do adapter, receita de build, receita NXExtract e registro de testes. Nomes sugeridos: `docs/inventory.md`, `docs/contracts.md`, `src/`, `recipes/`, `docs/validation.md`. Esses nomes são organização sugerida; os schemas reais do gerador continuam soberanos.

Para cada contrato registre import/assinatura, origem do código, licença, owner da memória/thread, comportamento de erro e teste. Para cada execução, registre commit, SHA do ELF, perfil de dados, alvo e resultado. Informações privadas ficam fora da documentação publicável.

## 5. Seguir a ordem nativa e corrigir uma fronteira por vez

Primeiro bibliotecas/relocações e construtores; depois JNI/callbacks de inicialização e lifecycle; então contexto/surface, frames, áudio, input, persistência e saída. A ordem exata vem da engine analisada, não de uma lista universal de nomes.

Quando ocorrer falha, escreva hipótese, medição que a distingue, reparo mínimo e contraprova. Por exemplo: áudio vivo com tela preta exige medir a fronteira gráfica; não é sucesso parcial de vídeo. Import desconhecido exige implementar seu contrato; um `return 0` genérico só oculta o problema.

## 6. Automatizar sem apagar a evidência

A IA pode gerar inventários, comparar hashes, localizar símbolos, escrever testes dirigidos e preparar o pacote. Preserve o executável aprovado; documentar ou traduzir não autoriza reconstruí-lo. Conclua alterações antes da bateria final, sem criar um ZIP novo a cada erro de desenvolvimento.

Um bom checkpoint informa o que mudou, comando executado, resultado, o que ainda falta e o próximo passo independente. Ao retomar, a IA deve ler esse checkpoint e comparar os arquivos, evitando redescobrir correções já comprovadas.

## 7. Concluir com o alcance correto

Entregue código e receita, origem das peças, build reproduzível, diagnóstico dos contratos faltantes, [instalação limpa](NXEXTRACT.md) e [provas dos bytes finais](TESTES-E-ENTREGA.md). “Compilou”, “abriu o menu” e “gameplay completo” são resultados diferentes. Declare somente o que foi observado.

Se faltar um input ou aparelho, termine o trabalho independente e diga exatamente qual fronteira não pôde testar. A visibilidade deste repositório permanece privada até aprovação explícita de NextOS.
