# Haxe, hxcpp e Lime no Android

[English](../en/HAXE-LIME-ANDROID.md)

Esta trilha trata de bibliotecas produzidas para Android e executadas por um adapter Linux ARM. A fonte selecionada de [Tightrope Theatre](../../ports/tightrope-nextos/README.md) permite identificar a família e estudar seus limites. Consulte também a [seleção de runtimes](ANDROID-RUNTIMES.md).

## 1. Reconhecer a composição real

```sh
python3 tools/inventory_apk.py /private/owner-input/game.apk --output work/haxe-inventory.json
python3 tools/find_reference.py --runtime haxe-lime --abi arm64-v8a
```

Procure `liblime.so` e `libApplicationMain.so` na mesma ABI, imports, entrypoints e bootstrap Java. A detecção por nomes é uma hipótese. Confirme hxcpp e registre versões de Haxe, hxcpp, Lime e SDL somente quando houver evidência; `unknown` é melhor que uma versão inventada. OpenFL pode fazer parte da aplicação, mas Lime isolado não o comprova. Esta trilha não é uma receita para HashLink, Neko ou um executável desktop.

O [manual Haxe/C++](https://haxe.org/manual/target-cpp-getting-started.html) descreve o alvo C++ e hxcpp. Isso ajuda a separar código nativo gerado do runtime Java/Android que o inicializa; não garante que outra versão da biblioteca seja intercambiável.

## 2. Respeitar o estado da referência

O [SOURCE-MAP](../../ports/tightrope-nextos/SOURCE-MAP.json) fixa `75eb5da65994995e38057c758440faafd63d87e8`. O [README](../../ports/tightrope-nextos/upstream/README.md) identifica **1.0.5-test.1**, candidata de input ROCKNIX com aceitação física exata pendente. Ele registra provas anteriores de 1.0.4, que não aprovam automaticamente os bytes selecionados.

Use esse snapshot para inventário e diagnóstico. Comportamentos sob teste são referências negativas, não uma implementação aprovada para copiar. Reuso de código exige recuperar e conferir uma referência pública aprovada da mesma fronteira, com commit, hash e evidência correspondentes. O [guia de fontes](PINS-E-FONTES.md) explica a recuperação sem substituir o snapshot. Leia [NOTICE](../../ports/tightrope-nextos/upstream/NOTICE.md) e [LICENSE](../../ports/tightrope-nextos/upstream/LICENSE).

## 3. Mapear callbacks e threads

O [src/main.c](../../ports/tightrope-nextos/upstream/src/main.c) documenta a cadeia Android observada: Application/Activity; carga de Lime e ApplicationMain; configuração JNI de SDL, áudio e controle; criação/redimensionamento da superfície; `nativeRunMain` na thread SDLMain, que conduz a `hxcpp_main`. A thread de UI entrega eventos separadamente.

Use essa cadeia como pergunta ao analisar a nova build: quais callbacks, assinaturas, argumentos e threads ela realmente exige? Não chame `hxcpp_main` diretamente antes das etapas requeridas, não force cena/fase e não invente um caminho alternativo para iniciar o jogo. O inventário da fonte não é uma autorização para copiar transformações específicas ou mecanismos de uma build protegida.

## 4. Provar TLS, GC e fronteiras SDL

O [manual de threads/stacks hxcpp](https://haxe.org/manual/target-cpp-ThreadsAndStacks.html) descreve a relação entre threads, stacks e coleta de lixo. Confira registro de threads, raízes, bloqueios e retomada para a versão real. Uma chave TLS que sempre retorna `NULL` ou um mutex vazio pode quebrar o runtime após a inicialização.

Na seleção, [bionic.c](../../ports/tightrope-nextos/upstream/src/bionic.c) e [pthread_bridge.c](../../ports/tightrope-nextos/upstream/src/pthread_bridge.c) ajudam a localizar esses contratos; [sdl_java.c](../../ports/tightrope-nextos/upstream/src/sdl_java.c) mostra a fronteira Java/SDL. Distinga SDL contida no guest Android da SDL Linux do firmware. Estruturas, eventos e callbacks não podem atravessar versões/ABIs apenas porque têm nomes parecidos.

Defina quem possui janela, contexto, surface e present. Mantenha o fluxo e as threads que a engine exige; não crie uma segunda janela para mascarar um callback ausente. Imports não suportados devem produzir diagnóstico verificável.

## 5. Preparar build e diagnóstico dirigido

Use [compilação ARM](COMPILAR-ARM.md), [shims](SHIMS.md) e um projeto separado para implementar contratos provados. Confira os arquivos incluídos/omitidos no manifesto antes de executar qualquer comando do README histórico; um script citado pode não fazer parte da seleção. Fixe toolchain e fontes recuperadas. Prefira AArch64 se presente e audite GLIBC ≤ 2.30 em todos os ELFs Linux públicos.

| Falha | Teste que distingue a causa |
| --- | --- |
| Congelamento depois de alocar/carregar cena | TLS por thread, GC, condição/mutex e owner da stack; exercitar workers e encerramento |
| Surface pronta, sem imagem | Ordem de callbacks, dimensões, contexto atual e pixels antes do present |
| Evento aparece no log, sem ação | Callback Java/SDL e consumidor Lime; press/release, menu e gameplay separados |
| Som falha ao retomar | Fronteira OpenSL/SDL, formato, fila e thread de callback; medir consumo e escutar |
| Reinício perde progresso | Diretório persistente, flush, save/reload e fechamento sem processo residual |

## 6. Extrair e entregar com evidência

A [receita selecionada](../../ports/tightrope-nextos/upstream/extractor.json) identifica bibliotecas e assets daquele perfil. Para outro port, inventarie todo o input Android e seus payloads críticos. O [NXExtract](NXEXTRACT.md) deve preparar a cópia completa com hooks reais, aceitar reempacotamento compatível e preservar saves. Não use dados preparados de outro jogo como fixture de sucesso.

Inclua `INSTALLATION.md` bilíngue, mantenha UI/NXSplash e pins V5 e siga [testes e entrega](TESTES-E-ENTREGA.md). Testes host de TLS/filas não aprovam o jogo ou os controles físicos. Não migre ports nem incorpore o ajuste pendente de Tightrope à V5.

## Missão para a IA

```text
Analise somente a build Android. Confirme hxcpp/Lime, ABI e bootstrap.
Leia Tightrope no commit selecionado como diagnóstico, mantendo explícita
sua aceitação pendente; não use o candidato como implementação aprovada.
Mapeie threads, TLS/GC, SDL/JNI, superfície, áudio e consumidor de input.
Implemente em projeto separado com referências públicas aprovadas e pins.
Prepare extração completa e testes dirigidos sem alterar a V5 congelada.
```
